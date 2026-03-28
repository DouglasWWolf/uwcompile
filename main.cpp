//=================================================================================================
// uwcompile - A userwave compiler.  Translates a userwave CSV file to binary
//
// Author: D. Wolf
//
// Ver    Date      Who  What
//---------------------------------------------------------------------------------------------
// 1.0   28-Mar-26  DWW  Initial creation
//=================================================================================================

#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <iostream>
#include <exception>
#include <stdexcept>
#include <string>
#include <cstring>
#include <vector>

using std::string;
using std::vector;


// These are command-line options
struct t_opt
{
    string ifilename;
    string ofilename;
    double vref = 1.6;
} opt;

// Line number from the input file
int    line_number;

// If this is true, the DAC values in the input file are expressed
// as voltages and need to be converted to DAC values 
bool   convert_voltage_to_dac_value = false;

// This is a key/value pair
struct kv_pair_t
{
    string key;
    string value;
};


//=============================================================================
// This is a binary userwave-command, with all fields in little-endian.
//
// By the time the FPGA sees it, the order of all 64 bytes will have been
// reversed, placing these fields (in the FPGA) in reverse order and in big-
// endian.
//=============================================================================
#pragma pack(push, 1)
struct uw_cmd_t
{
    uint32_t    cmd_index;
    uint16_t    cmd_duration;
    uint16_t    read_flags;
    uint16_t    read_start_time;
    uint16_t    read_characterization_id;
    uint16_t    read_data_type;
    uint16_t    reserved0;
    uint32_t    reserved1;
    
    uint16_t    vpretop_a;
    uint16_t    vpretop_b;
    uint16_t    vpretop_sw_delay;
    
    uint16_t    vprebot_a;
    uint16_t    vprebot_b;
    uint16_t    vprebot_sw_delay;

    uint16_t    refp_a;
    uint16_t    refp_b;
    uint16_t    refp_sw_delay;

    uint16_t    refn_a;
    uint16_t    refn_b;
    uint16_t    refn_sw_delay;

    uint16_t    liq_a;
    uint16_t    liq_b;
    uint16_t    liq_sw_delay;

    uint16_t    switch_flags;

    uint16_t    roll_pre_top_duration;
    uint16_t    roll_pre_top_start;

    uint16_t    roll_pre_bot_duration;
    uint16_t    roll_pre_bot_start;

    uint16_t    glb_pre_duration;
    uint16_t    glb_pre_start;
};
#pragma pack(pop)
//=============================================================================


//=============================================================================
// We're going to build a long list of records.  A record can be:
//
// (1) A userwave command
// (2) The top of a loop
// (3) The bottom of a loop structure
//=============================================================================
enum rec_type_t
{
    COMMAND,
    LOOP_START,
    LOOP_END
};
//=============================================================================


//=============================================================================
// We're going to build a vector of these records, and copy that vector into 
// our output file while "unrolling" the loops.   Loops can be nested!
//=============================================================================
struct rec_t
{
    rec_type_t  rec_type;
    string      loop_command;
    uint32_t    loop_count;
    uw_cmd_t    command;    
};
//=============================================================================

// This is vector of records we're going to create from our input file
vector<rec_t> master_list;

//=============================================================================
// Forward declarations
//=============================================================================
void execute(int argc, const char** argv);
void append_master_record(vector<kv_pair_t>&);
//=============================================================================


//=============================================================================
// main() - We just call execute and catch the exceptions
//=============================================================================
int main(int argc, const char** argv)
{
    try
    {
        execute(argc, argv);
    }
    
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }
}
//=============================================================================


//=================================================================================================
// throw_runtime() - Throws a runtime exception
//=================================================================================================
static void throw_runtime(const char* fmt, ...)
{
    char buffer[1024];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buffer, fmt, ap);
    va_end(ap);

    throw std::runtime_error(buffer);
}
//=================================================================================================


//=================================================================================================
// parse_key_value_token() - Extracts either the "key" or the "value" of a "key : value" pair
//                           in a line where key:value pairs are comma delimited
//
// On Exit: in points to ':' or ',' or an end-of-line character
//
// Returns: true if the token terminates with an end-of-line character
//=================================================================================================
bool parse_key_value_token(const char*& in, char* out, bool is_key)
{
    // Keep track of where the start of the buffer is
    const char* buffer = out;

    // We haven't yet encountered the end of the line
    bool is_eol = false;

    // Skip over any leading whitespace
    while (*in == ' ' || *in == '\t') ++in;

    // Loop through each character of the input...
    while (true)
    {
        // Fetch the next character
        int c = *in;

        // A comma delimeter is the end of the token
        if (c == ',') break;
        
        // A colon delimiter is the end of the "key"
        if (is_key && c == ':') break;

        // Is this the end of the input line?
        is_eol = (c == 10 || c == 13 || c == 0);
        
        // If it's the end of the input line...
        if (is_eol)
        {
            if (out != buffer && is_key)    
                throw_runtime("parsing error on line %i", line_number);
            else
                break;
        }

        // Output the character and fetch the next one
        *out++ = c;
        ++in;
    }

    // Trim off trailing spaces
    if (out != buffer && *(out-1) == ' ') --out;

    // Terminate the output string
    *out = 0;

    // Tell the caller whether we've encountered the end of the input line
    return is_eol;
}
//=================================================================================================



//=================================================================================================
// build_master_record_list() - This builds the "master_list" from the vectors of key/value pairs
//                              that are parsed from each line of the input file
//=================================================================================================
void build_master_record_list()
{
    char buffer[5000];
    char key[1000], value[1000];
    bool is_eol;

    // A key/value pair
    kv_pair_t kv;

    // A vector of key/value pairs
    vector<kv_pair_t> kv_vect;

    // Pointer to the characters of our input line
    const char* in;

    // Get a handy pointer to the filename
    const char* filename = opt.ifilename.c_str();

    // Open the input file and complain if we can't
    FILE* ifile = fopen(filename, "r");
    if (ifile == nullptr) throw_runtime("Can't open %s", filename);

    // We're going to count input lines
    line_number = 0;

    // Loop through each line of the input file
    while (fgets(buffer, sizeof buffer, ifile))
    {
        // Clear the vector of key/value strings the represent this line
        kv_vect.clear();

        // Keep track of the line number we're on
        ++line_number;

        // Point to the line of text we just read in
        in = buffer;
        
        // Skip over leading spaces and tabs
        while (*in == ' ' || *in == '\t') ++in;

        // If the first line of the file starts with "# voltages:" it's a sign that the
        // DAC value fields in the input file are expressed as voltages and will need
        // to be converted to genuine DAC values before packing into our output structure
        if (line_number == 1 && strncmp(in, "# voltages", 10) == 0)
        {
            convert_voltage_to_dac_value = true;
            continue;
        }

        // If the line is a comment, skip it
        if (*in == '#') continue;

        // If the line is blank, skip it
        if (*in == 10 || *in == 13 || *in == 0) continue;

        while (true)
        {
            // Parse the "key" portion of a "key : value" pair
            is_eol = parse_key_value_token(in, key, true);

            // If we've reached the end of the line, we're done with the line
            if (is_eol) break;

            // If we've reached a comma, this key/value pair is empty
            if (*in == ',')
            {
                ++in;
                continue;
            }

            // Skip over the colon
            ++in;

            // Fetch the "value" part of the "key:value" pair
            is_eol = parse_key_value_token(in, value, false);

            // Append this key/value pair to "kv_vect"
            kv.key   = key;
            kv.value = value;
            kv_vect.push_back(kv);

            // If this is the end of the line, we're done in this loop,
            // otherwise, skip over the comma that "in" is pointing at
            if (is_eol)
                break;
            else
                ++in;
        }

        // Create a master record from this vector of key/value pairs
        append_master_record(kv_vect);
    }

    // Make sure we close the input file when we're done!
    fclose(ifile);
}
//=================================================================================================


//=================================================================================================
// parse_u32() - Convert a string to a uint32_t
//=================================================================================================
uint32_t parse_u32(string& value)
{
    return (uint32_t) strtoul(value.c_str(), nullptr, 0);    
}
//=================================================================================================


//=================================================================================================
// parse_u16() - Convert a string to a uint16_t
//=================================================================================================
uint32_t parse_u16(string& value)
{
    return (uint16_t) strtoul(value.c_str(), nullptr, 0);    
}
//=================================================================================================

//=================================================================================================
// parse_bit() - Converts a numeric string to a 1 or a 0
//=================================================================================================
uint16_t parse_bit(string& value)
{
    return (parse_u16(value) != 0);
}
//=================================================================================================


//=================================================================================================
// parse_command_type() - Returns a 1 if the value string contains 's' or 'S'
//=================================================================================================
uint16_t parse_command_type(string& value)
{
    const char* str = value.c_str();
    if (strchr(str, 's')) return 1;
    if (strchr(str, 'S')) return 1;
    return 0;
}
//=================================================================================================


//=================================================================================================
// parse_dac() - Parses a DAC value into a uint16_t.   The DAC value we are parsing may be 
//               expressed as a voltage, in which case we have to compute the DAC value
//=================================================================================================
uint16_t parse_dac(string& value)
{
    if (convert_voltage_to_dac_value)
    {
        double voltage = strtod(value.c_str(), nullptr);        
        if (voltage > opt.vref)
        {
            throw_runtime("voltage %1.3lf exceeds max of %1.3lf", voltage, opt.vref);
        }
        return (uint16_t) ((0xFFFF * voltage) / opt.vref);
    }

    // If we get here, the DAC value is expressed in DAC counts
    return parse_u16(value);
}
//=================================================================================================


//=================================================================================================
// parse_simple_token() - Parses a space-delimited token from an input string
//=================================================================================================
const char* parse_simple_token(const char* in, char* out)
{
    // Skip over any leading tabs or spaces
    while (*in == 32 || *in == 9) ++in;

    // Copy the token to the output until we hit a delimiter
    while (*in != 0 && *in != 32 && *in != 9) *out++ = *in++;

    // Nul-terminate the output token
    *out = 0;

    // And hand a caller a pointer to the remainder of the input string
    return in;
}
//=================================================================================================

//=================================================================================================
// parse_loop_control() - Parses a "loop_control" key/value pair into a rec_t
//=================================================================================================
void parse_loop_control(string& s)
{
    char op[1000], name[1000], count[1000];
    rec_t rec;

    // Save the loop command
    rec.loop_command = s;
    
    // Get a pointer to our input string
    const char* in = s.c_str();

    // Parse the fields of the loop control
    in = parse_simple_token(in, op);
    in = parse_simple_token(in, name);
    in = parse_simple_token(in, count);

    // Is this a "loop_control : begin" statement?
    if (strcmp(op, "begin") == 0)
    {
        rec.rec_type = LOOP_START;
        rec.loop_count = (uint32_t) strtoul(count, nullptr, 0);
        master_list.push_back(rec);       
        return;
    }

    // Is this a "loop_control : end" statement?
    if (strcmp(op, "end") == 0)
    {
        rec.rec_type = LOOP_END;
        master_list.push_back(rec);       
        return;
    }

    // If we get here, the loop_control statement was malformed
    throw_runtime("bad loop_control: \'%s'", s.c_str());
}
//=================================================================================================


//=================================================================================================
// append_master_record() - Takes as its input a vector of key/value pairs, creates a rec_t
//                          from it, and appends that rec_t to "master_list"
//=================================================================================================
void append_master_record(vector<kv_pair_t>& v)
{
    rec_t       rec;
    uint16_t    bit;

    // Don't attempt to translate an empty vector of key/value pairs
    if (v.size() == 0) return;

    // If this was a "loop_control" command, perform special handling
    if (v[0].key == "loop_control")
    {
        parse_loop_control(v[0].value);
        return;
    }

    // We're about to create a userwave command record (i.e., not loop control)
    rec.rec_type = COMMAND;

    // Start out with a completely empty userwave-command
    memset(&rec.command, 0, sizeof rec.command);

    // Loop through each key-value pair in the input vector
    for (auto& kv : v)
    {
        if (kv.key == "cmd_index")
        {
            rec.command.cmd_index = parse_u32(kv.value);
            continue;         
        }   
        
        if (kv.key == "cmd_duration")
        {
            rec.command.cmd_duration = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "read_en")
        {
            bit = parse_bit(kv.value);
            rec.command.read_flags |= (bit << 0);
            continue;            
        }

        if (kv.key == "read_phase")
        {
            bit = parse_bit(kv.value);
            rec.command.read_flags |= (bit << 1);
            continue;            
        }

        if (kv.key == "read_bright_flag")
        {
            bit = parse_bit(kv.value);
            rec.command.read_flags |= (bit << 2);
            continue;            
        }

        if (kv.key == "read_safe_halting_point")
        {
            bit = parse_bit(kv.value);
            rec.command.read_flags |= (bit << 3);
            continue;            
        }

        // This is a hack-ish synonym for "read_safe_halting_point"
        if (kv.key == "command_type")
        {
            bit = parse_command_type(kv.value);
            rec.command.read_flags |= (bit << 3);
            continue;            
        }


        if (kv.key == "read_start_time")
        {
            rec.command.read_start_time = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "read_characterization_id")
        {
            rec.command.read_characterization_id = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "read_data_type")
        {
            rec.command.read_data_type = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "vpretop_a")
        {
            rec.command.vpretop_a = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "vpretop_b")
        {
            rec.command.vpretop_b = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "vpretop_sw_delay")
        {
            rec.command.vpretop_sw_delay = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "vprebot_a")
        {
            rec.command.vprebot_a = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "vprebot_b")
        {
            rec.command.vprebot_b = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "vprebot_sw_delay")
        {
            rec.command.vprebot_sw_delay = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "refp_a")
        {
            rec.command.refp_a = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "refp_b")
        {
            rec.command.refp_b = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "refp_sw_delay")
        {
            rec.command.refp_sw_delay = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "refn_a")
        {
            rec.command.refn_a = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "refn_b")
        {
            rec.command.refn_b = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "refn_sw_delay")
        {
            rec.command.refn_sw_delay = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "liq_a")
        {
            rec.command.liq_a = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "liq_b")
        {
            rec.command.liq_b = parse_dac(kv.value);
            continue;            
        }

        if (kv.key == "liq_sw_delay")
        {
            rec.command.liq_sw_delay = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "vpretop_sw")
        {
            bit = parse_bit(kv.value);
            rec.command.switch_flags |= (bit << 0);
            continue;            
        }

        if (kv.key == "vprebot_sw")
        {
            bit = parse_bit(kv.value);
            rec.command.switch_flags |= (bit << 1);
            continue;            
        }

        if (kv.key == "refp_sw")
        {
            bit = parse_bit(kv.value);
            rec.command.switch_flags |= (bit << 2);
            continue;            
        }

        if (kv.key == "refn_sw")
        {
            bit = parse_bit(kv.value);
            rec.command.switch_flags |= (bit << 3);
            continue;            
        }

        if (kv.key == "liq_sw")
        {
            bit = parse_bit(kv.value);
            rec.command.switch_flags |= (bit << 4);
            continue;            
        }

        if (kv.key == "roll_pre_top_duration")
        {
            rec.command.roll_pre_top_duration = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "roll_pre_top_start")
        {
            rec.command.roll_pre_top_start = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "roll_pre_bot_duration")
        {
            rec.command.roll_pre_bot_duration = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "roll_pre_bot_start")
        {
            rec.command.roll_pre_bot_start = parse_u16(kv.value);
            continue;            
        }


        if (kv.key == "roll_pre_bot_duration")
        {
            rec.command.roll_pre_bot_duration = parse_u16(kv.value);
            continue;            
        }

        if (kv.key == "glb_pre_start")
        {
            rec.command.glb_pre_start = parse_u16(kv.value);
            continue;            
        }


        if (kv.key == "glb_pre_duration")
        {
            rec.command.glb_pre_duration = parse_u16(kv.value);
            continue;            
        }


        // If we have an unknown key, complain about it
        throw_runtime("Unknown key '%s' at line %i", kv.key.c_str(), line_number);
    }

    // Append this record to the master-list
    master_list.push_back(rec);
}
//=================================================================================================


//=================================================================================================
// unmatched_loop() - A convenience method for throwing loop_control errors
//=================================================================================================
void unmatched_loop(string s)
{
    throw_runtime("Unmatched loop_control '%s'", s.c_str());
}
//=================================================================================================

// This tells us about the currently executing loop
struct loop_control_t
{
    uint32_t    count;
    uint32_t    top_index;
    string      name;
};

// This stack is used for managing nested loops
vector<loop_control_t> loop_stack;

//=================================================================================================
// write_output - Writes the binary output file
//=================================================================================================
void write_output()
{
    loop_control_t loop = {1, 1, "#main#"};

    // This is how many entries are in the master list
    uint32_t master_list_size = master_list.size();

    // Fetch the name of the output file
    const char* filename = opt.ofilename.c_str();

    // Open the output file
    FILE* ofile = fopen(filename, "w");
    if (ofile == nullptr) throw_runtime("Can't create file %s", filename);

    // Loop through every record in the master list
    for (uint32_t i = 0; i < master_list_size; ++i)
    {
        // Get a handy reference to this record
        auto& rec = master_list[i];

        // If we're starting a new loop, push the old loop-control onto 
        // the stack
        if (rec.rec_type == LOOP_START)
        {
            loop_stack.push_back(loop);
            loop.count     = rec.loop_count;
            loop.top_index = i;
            loop.name      = rec.loop_command;
            if (loop.top_index >= master_list_size) unmatched_loop(rec.loop_command);
            continue;
        }


        // If we're at the end of a loop...
        if (rec.rec_type == LOOP_END)
        {
            // If there is no corresponding LOOP_START, complain
            if (loop_stack.empty()) unmatched_loop(rec.loop_command);

            // If we need to make another iteration of the current loop, do so
            if (loop.count > 1)
            {
                loop.count--;
                i = loop.top_index;
                continue;
            }

            // Fetch the loop control at the top of the stack
            loop = loop_stack[loop_stack.size() - 1];

            // And remove that structure from the stack
            loop_stack.pop_back();

            // Fall off the bottom of the loop
            continue;
        }

        // If the loop_count isn't 0, output this userwave command
        if (loop.count) fwrite(&rec.command, 1, sizeof rec.command, ofile);
    }

    // We're done!
    fclose(ofile);

    // When we're finished outputting the master-list, there should be
    // nothing left on the loop stack
    if (loop_stack.size()) unmatched_loop(loop.name);
}
//=================================================================================================


//=================================================================================================
// make_output_filename() - If there's no filename in opt.ofilename, we create one
//=================================================================================================
void make_output_filename()
{
    char filename[1000];

    // If there's already an output filename, do nothing
    if (!opt.ofilename.empty()) return;

    // Fetch a copy of the input filename
    strcpy(filename, opt.ifilename.c_str());

    // How long is the input filename?
    int len = strlen(filename);

    // Either replace ".csv" with ".bin", or append ".bin" to the end
    if (len > 4 && strcmp(filename + len - 4, ".csv") == 0)
        strcpy(filename + len - 4, ".bin");
    else
        strcat(filename, ".bin");

    // Stuff the output filename into "opt.ofilename"
    opt.ofilename = filename;
}
//=================================================================================================



//=================================================================================================
// show_help() - Display help text for the user
//=================================================================================================
void show_help()
{
    printf("uwcompile [-vref <voltage>] [-output <filename>] <input_file>\n");
    exit(1);    
}
//=================================================================================================


//=================================================================================================
// parse_command_line() - Fills in the "opt" structure from the command line options
//=================================================================================================
void parse_command_line(const char** argv)
{
    ++argv;

    while (*argv)
    {
        // Fetch the option
        string option = *argv++;

        // Ignore empty tokens
        if (option.empty()) continue;

        // Does the user want to change the value of the reference voltage?
        if (option == "-vref" && *argv)
        {
            opt.vref = strtod(*argv++, nullptr);
            continue;
        }

        // Does the user want to specify an output filename?
        if (option == "-output" && *argv)
        {
            opt.ofilename = *argv++;
            continue;
        }

        // If we don't recognize the command line option, complain
        if (option[0] == '-') 
        {
            throw_runtime("Unknown command line option '%s'", option.c_str());
        }

        // If we don't have an input filename yet, store it
        if (opt.ifilename.empty())
        {
            opt.ifilename = option;
            continue;
        }

        // If we get here, the user gave us too many filenames
        show_help();
    }

    // If the user didn't give us a filename, show help
    if (opt.ifilename.empty()) show_help();
}
//=================================================================================================


//=================================================================================================
// execute() - Very simple main-line program execution
//=================================================================================================
void execute(int argc, const char** argv)
{
    parse_command_line(argv);
    make_output_filename();
    build_master_record_list();
    write_output();
}
//=================================================================================================
