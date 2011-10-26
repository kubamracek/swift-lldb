//===-- CommandObjectMemory.cpp ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "CommandObjectMemory.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/DataExtractor.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/ValueObjectMemory.h"
#include "lldb/Interpreter/Args.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Interpreter/OptionGroupFormat.h"
#include "lldb/Interpreter/OptionGroupOutputFile.h"
#include "lldb/Interpreter/OptionGroupValueObjectDisplay.h"
#include "lldb/Symbol/ClangNamespaceDecl.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StackFrame.h"

using namespace lldb;
using namespace lldb_private;

static OptionDefinition
g_option_table[] =
{
    { LLDB_OPT_SET_1, false, "num-per-line" ,'l', required_argument, NULL, 0, eArgTypeNumberPerLine ,"The number of items per line to display."},
    { LLDB_OPT_SET_2, false, "binary"       ,'b', no_argument      , NULL, 0, eArgTypeNone          ,"If true, memory will be saved as binary. If false, the memory is saved save as an ASCII dump that uses the format, size, count and number per line settings."},
    { LLDB_OPT_SET_3, true , "view-as"      ,'t', required_argument, NULL, 0, eArgTypeNone          ,"The name of a type to view memory as."},
};



class OptionGroupReadMemory : public OptionGroup
{
public:

    OptionGroupReadMemory () :
        m_num_per_line (1,1),
        m_output_as_binary (false),
        m_view_as_type()
    {
    }

    virtual
    ~OptionGroupReadMemory ()
    {
    }
    
    
    virtual uint32_t
    GetNumDefinitions ()
    {
        return sizeof (g_option_table) / sizeof (OptionDefinition);
    }
    
    virtual const OptionDefinition*
    GetDefinitions ()
    {
        return g_option_table;
    }
    
    virtual Error
    SetOptionValue (CommandInterpreter &interpreter,
                    uint32_t option_idx,
                    const char *option_arg)
    {
        Error error;
        char short_option = (char) g_option_table[option_idx].short_option;
        
        switch (short_option)
        {
            case 'l':
                error = m_num_per_line.SetValueFromCString (option_arg);
                if (m_num_per_line.GetCurrentValue() == 0)
                    error.SetErrorStringWithFormat("invalid value for --num-per-line option '%s'", option_arg);
                break;

            case 'b':
                m_output_as_binary = true;
                break;
                
            case 't':
                error = m_view_as_type.SetValueFromCString (option_arg);
                break;
                
            default:
                error.SetErrorStringWithFormat("unrecognized short option '%c'", short_option);
                break;
        }
        return error;
    }
    
    virtual void
    OptionParsingStarting (CommandInterpreter &interpreter)
    {
        m_num_per_line.Clear();
        m_output_as_binary = false;
        m_view_as_type.Clear();
    }
    
    Error
    FinalizeSettings (Target *target, OptionGroupFormat& format_options)
    {
        Error error;
        OptionValueUInt64 &byte_size_value = format_options.GetByteSizeValue();
        OptionValueUInt64 &count_value = format_options.GetCountValue();
        const bool byte_size_option_set = byte_size_value.OptionWasSet();
        const bool num_per_line_option_set = m_num_per_line.OptionWasSet();
        const bool count_option_set = format_options.GetCountValue().OptionWasSet();
        
        switch (format_options.GetFormat())
        {
            default:
                break;
                
            case eFormatBoolean:
                if (!byte_size_option_set)
                    byte_size_value = 1;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;
                
            case eFormatCString:
                break;
                
            case eFormatPointer:
                byte_size_value = target->GetArchitecture().GetAddressByteSize();
                if (!num_per_line_option_set)
                    m_num_per_line = 4;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;
                
            case eFormatBinary:
            case eFormatFloat:
            case eFormatOctal:
            case eFormatDecimal:
            case eFormatEnum:
            case eFormatUnicode16:
            case eFormatUnicode32:
            case eFormatUnsigned:
                if (!byte_size_option_set)
                    byte_size_value = 4;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;
                
            case eFormatBytes:
            case eFormatBytesWithASCII:
                if (byte_size_option_set)
                {
                    if (byte_size_value > 1)
                        error.SetErrorString ("use --count option to specify an end address to display a number of bytes");
                }
                else
                    byte_size_value = 1;
                if (!num_per_line_option_set)
                    m_num_per_line = 16;
                if (!count_option_set)
                    format_options.GetCountValue() = 32;
                break;
            case eFormatCharArray:
            case eFormatChar:
            case eFormatCharPrintable:
                if (!byte_size_option_set)
                    byte_size_value = 1;
                if (!num_per_line_option_set)
                    m_num_per_line = 32;
                if (!count_option_set)
                    format_options.GetCountValue() = 64;
                break;
            case eFormatComplex:
                if (!byte_size_option_set)
                    byte_size_value = 8;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    format_options.GetCountValue() = 8;
                break;
            case eFormatHex:
                if (!byte_size_option_set)
                    byte_size_value = 4;
                if (!num_per_line_option_set)
                {
                    switch (byte_size_value)
                    {
                        case 1:
                        case 2:
                            m_num_per_line = 8;
                            break;
                        case 4:
                            m_num_per_line = 4;
                            break;
                        case 8:
                            m_num_per_line = 2;
                            break;
                        default:
                            m_num_per_line = 1;
                            break;
                    }
                }
                if (!count_option_set)
                    count_value = 8;
                break;
                
            case eFormatVectorOfChar:
            case eFormatVectorOfSInt8:
            case eFormatVectorOfUInt8:
            case eFormatVectorOfSInt16:
            case eFormatVectorOfUInt16:
            case eFormatVectorOfSInt32:
            case eFormatVectorOfUInt32:
            case eFormatVectorOfSInt64:
            case eFormatVectorOfUInt64:
            case eFormatVectorOfFloat32:
            case eFormatVectorOfFloat64:
            case eFormatVectorOfUInt128:
                if (!byte_size_option_set)
                    byte_size_value = 128;
                if (!num_per_line_option_set)
                    m_num_per_line = 1;
                if (!count_option_set)
                    count_value = 4;
                break;
        }
        return error;
    }

    OptionValueUInt64 m_num_per_line;
    bool m_output_as_binary;
    OptionValueString m_view_as_type;
};



//----------------------------------------------------------------------
// Read memory from the inferior process
//----------------------------------------------------------------------
class CommandObjectMemoryRead : public CommandObject
{
public:

    CommandObjectMemoryRead (CommandInterpreter &interpreter) :
        CommandObject (interpreter,
                       "memory read",
                       "Read from the memory of the process being debugged.",
                       NULL,
                       eFlagProcessMustBePaused),
        m_option_group (interpreter),
        m_format_options (eFormatBytesWithASCII, 1, 8),
        m_memory_options (),
        m_outfile_options (),
        m_varobj_options()
    {
        CommandArgumentEntry arg1;
        CommandArgumentEntry arg2;
        CommandArgumentData start_addr_arg;
        CommandArgumentData end_addr_arg;
        
        // Define the first (and only) variant of this arg.
        start_addr_arg.arg_type = eArgTypeStartAddress;
        start_addr_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (start_addr_arg);
        
        // Define the first (and only) variant of this arg.
        end_addr_arg.arg_type = eArgTypeEndAddress;
        end_addr_arg.arg_repetition = eArgRepeatOptional;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg2.push_back (end_addr_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
        m_arguments.push_back (arg2);
        
        // Add the "--format" and "--count" options to group 1 and 3
        m_option_group.Append (&m_format_options, 
                               OptionGroupFormat::OPTION_GROUP_FORMAT | OptionGroupFormat::OPTION_GROUP_COUNT, 
                               LLDB_OPT_SET_1 | LLDB_OPT_SET_3);
        // Add the "--size" option to group 1 and 2
        m_option_group.Append (&m_format_options, 
                               OptionGroupFormat::OPTION_GROUP_SIZE, 
                               LLDB_OPT_SET_1 | LLDB_OPT_SET_2);
        m_option_group.Append (&m_memory_options);
        m_option_group.Append (&m_outfile_options, LLDB_OPT_SET_ALL, LLDB_OPT_SET_1 | LLDB_OPT_SET_2 | LLDB_OPT_SET_3);
        m_option_group.Append (&m_varobj_options, LLDB_OPT_SET_ALL, LLDB_OPT_SET_3);
        m_option_group.Finalize();
    }

    virtual
    ~CommandObjectMemoryRead ()
    {
    }

    Options *
    GetOptions ()
    {
        return &m_option_group;
    }

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result)
    {
        ExecutionContext exe_ctx (m_interpreter.GetExecutionContext());
        Target *target = exe_ctx.GetTargetPtr();
        if (target == NULL)
        {
            result.AppendError("need at least a target to read memory");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }
        const size_t argc = command.GetArgumentCount();

        
        if (argc == 0 || argc > 2)
        {
            result.AppendErrorWithFormat ("%s takes 1 or two args.\n", m_cmd_name.c_str());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        ClangASTType clang_ast_type;        
        Error error;

        Format format = m_format_options.GetFormat();
        const char *view_as_type_cstr = m_memory_options.m_view_as_type.GetCurrentValue();
        if (view_as_type_cstr && view_as_type_cstr[0])
        {
            // We are viewing memory as a type
            SymbolContext sc;
            const bool append = true;
            TypeList type_list;
            uint32_t reference_count = 0;
            uint32_t pointer_count = 0;
            size_t idx;
            static const char *g_keywords[] = { "const", "volatile", "restrict", "struct", "class", "union"};
            static size_t g_num_keywords = sizeof(g_keywords)/sizeof(const char *);
            std::string type_str(view_as_type_cstr);
            
            // Remove all instances of g_keywords that are followed by spaces
            for (size_t i = 0; i < g_num_keywords; ++i)
            {
                const char *keyword = g_keywords[i];
                int keyword_len = ::strlen (keyword);
                while ((idx = type_str.find (keyword)) != std::string::npos)
                {
                    if (type_str[idx + keyword_len] == ' ' || type_str[idx + keyword_len] == '\t')
                        type_str.erase(idx, keyword_len+1);
                }
            }
            bool done = type_str.empty();
            // 
            idx = type_str.find_first_not_of (" \t");
            if (idx > 0 && idx != std::string::npos)
                type_str.erase (0, idx);
            while (!done)
            {
                // Strip trailing spaces
                if (type_str.empty())
                    done = true;
                else
                {
                    switch (type_str[type_str.size()-1])
                    {
                    case '*':
                        ++pointer_count;
                        // fall through...
                    case ' ':
                    case '\t':
                        type_str.erase(type_str.size()-1);
                        break;

                    case '&':
                        if (reference_count == 0)
                        {
                            reference_count = 1;
                            type_str.erase(type_str.size()-1);
                        }
                        else
                        {
                            result.AppendErrorWithFormat ("invalid type string: '%s'\n", view_as_type_cstr);
                            result.SetStatus(eReturnStatusFailed);
                            return false;
                        }
                        break;

                    default:
                        done = true;
                        break;
                    }
                }
            }
                    
            ConstString lookup_type_name(type_str.c_str());
            StackFrame *frame = exe_ctx.GetFramePtr();
            if (frame)
            {
                sc = frame->GetSymbolContext (eSymbolContextModule);
                if (sc.module_sp)
                {
                    sc.module_sp->FindTypes (sc,
                                             lookup_type_name,
                                             NULL,
                                             append, 
                                             1, 
                                             type_list);
                }
            }
            if (type_list.GetSize() == 0)
            {
                target->GetImages().FindTypes (sc, 
                                               lookup_type_name, 
                                               append, 
                                               1, 
                                               type_list);
            }
            
            if (type_list.GetSize() == 0)
            {
                result.AppendErrorWithFormat ("unable to find any types that match the raw type '%s' for full type '%s'\n", 
                                              lookup_type_name.GetCString(), 
                                              view_as_type_cstr);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            
            TypeSP type_sp (type_list.GetTypeAtIndex(0));
            clang_ast_type.SetClangType (type_sp->GetClangAST(), type_sp->GetClangFullType());
            
            while (pointer_count > 0)
            {
                clang_type_t pointer_type = ClangASTContext::CreatePointerType (clang_ast_type.GetASTContext(), clang_ast_type.GetOpaqueQualType());
                if (pointer_type)
                    clang_ast_type.SetClangType (clang_ast_type.GetASTContext(), pointer_type);
                else
                {
                    result.AppendError ("unable make a pointer type\n");
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                --pointer_count;
            }

            m_format_options.GetByteSizeValue() = (clang_ast_type.GetClangTypeBitWidth () + 7) / 8;
            
            if (m_format_options.GetByteSizeValue() == 0)
            {
                result.AppendErrorWithFormat ("unable to get the byte size of the type '%s'\n", 
                                              view_as_type_cstr);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            
            if (!m_format_options.GetCountValue().OptionWasSet())
                m_format_options.GetCountValue() = 1;
        }
        else
        {
            error = m_memory_options.FinalizeSettings (target, m_format_options);
        }

        // Look for invalid combinations of settings
        if (error.Fail())
        {
            result.AppendErrorWithFormat("%s", error.AsCString());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        size_t item_count = m_format_options.GetCountValue().GetCurrentValue();
        const size_t item_byte_size = m_format_options.GetByteSizeValue().GetCurrentValue();
        const size_t num_per_line = m_memory_options.m_num_per_line.GetCurrentValue();

        size_t total_byte_size = item_count * item_byte_size;
        if (total_byte_size == 0)
            total_byte_size = 32;

        lldb::addr_t addr = Args::StringToUInt64(command.GetArgumentAtIndex(0), LLDB_INVALID_ADDRESS, 0);

        if (addr == LLDB_INVALID_ADDRESS)
        {
            result.AppendErrorWithFormat("invalid start address string '%s'.\n", command.GetArgumentAtIndex(0));
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        if (argc == 2)
        {
            lldb::addr_t end_addr = Args::StringToUInt64(command.GetArgumentAtIndex(1), LLDB_INVALID_ADDRESS, 0);
            if (end_addr == LLDB_INVALID_ADDRESS)
            {
                result.AppendErrorWithFormat("invalid end address string '%s'.\n", command.GetArgumentAtIndex(1));
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            else if (end_addr <= addr)
            {
                result.AppendErrorWithFormat("end address (0x%llx) must be greater that the start address (0x%llx).\n", end_addr, addr);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            else if (m_format_options.GetCountValue().OptionWasSet())
            {
                result.AppendErrorWithFormat("specify either the end address (0x%llx) or the count (--count %lu), not both.\n", end_addr, item_count);
                result.SetStatus(eReturnStatusFailed);
                return false;
            }

            total_byte_size = end_addr - addr;
            item_count = total_byte_size / item_byte_size;
        }
        
        DataBufferSP data_sp;
        size_t bytes_read = 0;
        if (!clang_ast_type.GetOpaqueQualType())
        {
            data_sp.reset (new DataBufferHeap (total_byte_size, '\0'));
            Address address(NULL, addr);
            bytes_read = target->ReadMemory(address, false, data_sp->GetBytes (), data_sp->GetByteSize(), error);
            if (bytes_read == 0)
            {
                result.AppendWarningWithFormat("Read from 0x%llx failed.\n", addr);
                result.AppendError(error.AsCString());
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
            
            if (bytes_read < total_byte_size)
                result.AppendWarningWithFormat("Not all bytes (%lu/%lu) were able to be read from 0x%llx.\n", bytes_read, total_byte_size, addr);
        }

        StreamFile outfile_stream;
        Stream *output_stream = NULL;
        const FileSpec &outfile_spec = m_outfile_options.GetFile().GetCurrentValue();
        if (outfile_spec)
        {
            char path[PATH_MAX];
            outfile_spec.GetPath (path, sizeof(path));
            
            uint32_t open_options = File::eOpenOptionWrite | File::eOpenOptionCanCreate;
            const bool append = m_outfile_options.GetAppend().GetCurrentValue();
            if (append)
                open_options |= File::eOpenOptionAppend;
            
            if (outfile_stream.GetFile ().Open (path, open_options).Success())
            {
                if (m_memory_options.m_output_as_binary)
                {
                    int bytes_written = outfile_stream.Write (data_sp->GetBytes(), bytes_read);
                    if (bytes_written > 0)
                    {
                        result.GetOutputStream().Printf ("%i bytes %s to '%s'\n", 
                                                         bytes_written, 
                                                         append ? "appended" : "written", 
                                                         path);
                        return true;
                    }
                    else 
                    {
                        result.AppendErrorWithFormat("Failed to write %zu bytes to '%s'.\n", bytes_read, path);
                        result.SetStatus(eReturnStatusFailed);
                        return false;
                    }
                }
                else
                {
                    // We are going to write ASCII to the file just point the
                    // output_stream to our outfile_stream...
                    output_stream = &outfile_stream;
                }
            }
            else 
            {
                result.AppendErrorWithFormat("Failed to open file '%s' for %s.\n", path, append ? "append" : "write");
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
        }
        else 
        {
            output_stream = &result.GetOutputStream();
        }


        if (clang_ast_type.GetOpaqueQualType())
        {
            for (uint32_t i = 0; i<item_count; ++i)
            {
                addr_t item_addr = addr + (i * item_byte_size);
                Address address (NULL, item_addr);
                StreamString name_strm;
                name_strm.Printf ("0x%llx", item_addr);
                ValueObjectSP valobj_sp (ValueObjectMemory::Create (exe_ctx.GetBestExecutionContextScope(), 
                                                                    name_strm.GetString().c_str(), 
                                                                    address, 
                                                                    clang_ast_type));
                if (valobj_sp)
                {
                    if (format != eFormatDefault)
                        valobj_sp->SetFormat (format);

                    bool scope_already_checked = true;
                    
                    ValueObject::DumpValueObject (*output_stream,
                                                  valobj_sp.get(),
                                                  NULL,
                                                  m_varobj_options.ptr_depth,
                                                  0,
                                                  m_varobj_options.max_depth,
                                                  m_varobj_options.show_types,
                                                  m_varobj_options.show_location,
                                                  m_varobj_options.use_objc,
                                                  m_varobj_options.use_dynamic,
                                                  m_varobj_options.be_raw ? false : m_varobj_options.use_synth,
                                                  scope_already_checked,
                                                  m_varobj_options.flat_output,
                                                  m_varobj_options.be_raw ? UINT32_MAX : m_varobj_options.no_summary_depth,
                                                  m_varobj_options.be_raw ? true : m_varobj_options.ignore_cap);
                }
                else
                {
                    result.AppendErrorWithFormat ("failed to create a value object for: (%s) %s\n", 
                                                  view_as_type_cstr, 
                                                  name_strm.GetString().c_str());
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
            }
            return true;
        }

        result.SetStatus(eReturnStatusSuccessFinishResult);
        DataExtractor data (data_sp, 
                            target->GetArchitecture().GetByteOrder(), 
                            target->GetArchitecture().GetAddressByteSize());


        assert (output_stream);
        data.Dump (output_stream,
                   0,
                   m_format_options.GetFormat(),
                   item_byte_size,
                   item_count,
                   num_per_line,
                   addr,
                   0,
                   0);
        output_stream->EOL();
        return true;
    }

protected:
    OptionGroupOptions m_option_group;
    OptionGroupFormat m_format_options;
    OptionGroupReadMemory m_memory_options;
    OptionGroupOutputFile m_outfile_options;
    OptionGroupValueObjectDisplay m_varobj_options;

};


OptionDefinition
g_memory_write_option_table[] =
{
{ LLDB_OPT_SET_1, true,  "infile", 'i', required_argument, NULL, 0, eArgTypeFilename, "Write memory using the contents of a file."},
{ LLDB_OPT_SET_1, false, "offset", 'o', required_argument, NULL, 0, eArgTypeOffset,   "Start writng bytes from an offset within the input file."},
};


//----------------------------------------------------------------------
// Write memory to the inferior process
//----------------------------------------------------------------------
class CommandObjectMemoryWrite : public CommandObject
{
public:

    class OptionGroupWriteMemory : public OptionGroup
    {
    public:
        OptionGroupWriteMemory () :
            OptionGroup()
        {
        }

        virtual
        ~OptionGroupWriteMemory ()
        {
        }

        virtual uint32_t
        GetNumDefinitions ()
        {
            return sizeof (g_memory_write_option_table) / sizeof (OptionDefinition);
        }
        
        virtual const OptionDefinition*
        GetDefinitions ()
        {
            return g_memory_write_option_table;
        }
        
        virtual Error
        SetOptionValue (CommandInterpreter &interpreter,
                        uint32_t option_idx,
                        const char *option_arg)
        {
            Error error;
            char short_option = (char) g_memory_write_option_table[option_idx].short_option;
            
            switch (short_option)
            {
                case 'i':
                    m_infile.SetFile (option_arg, true);
                    if (!m_infile.Exists())
                    {
                        m_infile.Clear();
                        error.SetErrorStringWithFormat("input file does not exist: '%s'", option_arg);
                    }
                    break;
                    
                case 'o':
                    {
                        bool success;
                        m_infile_offset = Args::StringToUInt64(option_arg, 0, 0, &success);
                        if (!success)
                        {
                            error.SetErrorStringWithFormat("invalid offset string '%s'", option_arg);
                        }
                    }
                    break;
                    
                default:
                    error.SetErrorStringWithFormat("unrecognized short option '%c'", short_option);
                    break;
            }
            return error;
        }
        
        virtual void
        OptionParsingStarting (CommandInterpreter &interpreter)
        {
            m_infile.Clear();
            m_infile_offset = 0;
        }

        FileSpec m_infile;
        off_t m_infile_offset;
    };

    CommandObjectMemoryWrite (CommandInterpreter &interpreter) :
        CommandObject (interpreter,
                       "memory write",
                       "Write to the memory of the process being debugged.",
                       //"memory write [<cmd-options>] <addr> [value1 value2 ...]",
                       NULL,
                       eFlagProcessMustBeLaunched),
        m_option_group (interpreter),
        m_format_options (eFormatBytes, 1, UINT64_MAX),
        m_memory_options ()
    {
        CommandArgumentEntry arg1;
        CommandArgumentEntry arg2;
        CommandArgumentData addr_arg;
        CommandArgumentData value_arg;
        
        // Define the first (and only) variant of this arg.
        addr_arg.arg_type = eArgTypeAddress;
        addr_arg.arg_repetition = eArgRepeatPlain;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg1.push_back (addr_arg);
        
        // Define the first (and only) variant of this arg.
        value_arg.arg_type = eArgTypeValue;
        value_arg.arg_repetition = eArgRepeatPlus;
        
        // There is only one variant this argument could be; put it into the argument entry.
        arg2.push_back (value_arg);
        
        // Push the data for the first argument into the m_arguments vector.
        m_arguments.push_back (arg1);
        m_arguments.push_back (arg2);
        
        m_option_group.Append (&m_format_options, OptionGroupFormat::OPTION_GROUP_FORMAT, LLDB_OPT_SET_1);
        m_option_group.Append (&m_format_options, OptionGroupFormat::OPTION_GROUP_SIZE  , LLDB_OPT_SET_1|LLDB_OPT_SET_2);
        m_option_group.Append (&m_memory_options, LLDB_OPT_SET_ALL, LLDB_OPT_SET_2);
        m_option_group.Finalize();

    }

    virtual
    ~CommandObjectMemoryWrite ()
    {
    }

    Options *
    GetOptions ()
    {
        return &m_option_group;
    }

    bool
    UIntValueIsValidForSize (uint64_t uval64, size_t total_byte_size)
    {
        if (total_byte_size > 8)
            return false;

        if (total_byte_size == 8)
            return true;

        const uint64_t max = ((uint64_t)1 << (uint64_t)(total_byte_size * 8)) - 1;
        return uval64 <= max;
    }

    bool
    SIntValueIsValidForSize (int64_t sval64, size_t total_byte_size)
    {
        if (total_byte_size > 8)
            return false;

        if (total_byte_size == 8)
            return true;

        const int64_t max = ((int64_t)1 << (uint64_t)(total_byte_size * 8 - 1)) - 1;
        const int64_t min = ~(max);
        return min <= sval64 && sval64 <= max;
    }

    virtual bool
    Execute (Args& command,
             CommandReturnObject &result)
    {
        Process *process = m_interpreter.GetExecutionContext().GetProcessPtr();
        if (process == NULL)
        {
            result.AppendError("need a process to read memory");
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        const size_t argc = command.GetArgumentCount();

        if (m_memory_options.m_infile)
        {
            if (argc < 1)
            {
                result.AppendErrorWithFormat ("%s takes a destination address when writing file contents.\n", m_cmd_name.c_str());
                result.SetStatus(eReturnStatusFailed);
                return false;
            }       
        }
        else if (argc < 2)
        {
            result.AppendErrorWithFormat ("%s takes a destination address and at least one value.\n", m_cmd_name.c_str());
            result.SetStatus(eReturnStatusFailed);
            return false;
        }

        StreamString buffer (Stream::eBinary,
                             process->GetTarget().GetArchitecture().GetAddressByteSize(),
                             process->GetTarget().GetArchitecture().GetByteOrder());

        OptionValueUInt64 &byte_size_value = m_format_options.GetByteSizeValue();
        size_t item_byte_size = byte_size_value.GetCurrentValue();

        lldb::addr_t addr = Args::StringToUInt64(command.GetArgumentAtIndex(0), LLDB_INVALID_ADDRESS, 0);

        if (addr == LLDB_INVALID_ADDRESS)
        {
            result.AppendErrorWithFormat("Invalid address string '%s'.\n", command.GetArgumentAtIndex(0));
            result.SetStatus(eReturnStatusFailed);
            return false;
        }
        
        if (m_memory_options.m_infile)
        {
            size_t length = SIZE_MAX;
            if (item_byte_size > 0)
                length = item_byte_size;
            lldb::DataBufferSP data_sp (m_memory_options.m_infile.ReadFileContents (m_memory_options.m_infile_offset, length));
            if (data_sp)
            {
                length = data_sp->GetByteSize();
                if (length > 0)
                {
                    Error error;
                    size_t bytes_written = process->WriteMemory (addr, data_sp->GetBytes(), length, error);
                    
                    if (bytes_written == length)
                    {
                        // All bytes written
                        result.GetOutputStream().Printf("%zu bytes were written to 0x%llx\n", bytes_written, addr);
                        result.SetStatus(eReturnStatusSuccessFinishResult);
                    }
                    else if (bytes_written > 0)
                    {
                        // Some byte written
                        result.GetOutputStream().Printf("%zu bytes of %zu requested were written to 0x%llx\n", bytes_written, length, addr);
                        result.SetStatus(eReturnStatusSuccessFinishResult);
                    }
                    else 
                    {
                        result.AppendErrorWithFormat ("Memory write to 0x%llx failed: %s.\n", addr, error.AsCString());
                        result.SetStatus(eReturnStatusFailed);
                    }
                }
            }
            else
            {
                result.AppendErrorWithFormat ("Unable to read contents of file.\n");
                result.SetStatus(eReturnStatusFailed);
            }
            return result.Succeeded();
        }
        else if (item_byte_size == 0)
        {
            if (m_format_options.GetFormat() == eFormatPointer)
                item_byte_size = buffer.GetAddressByteSize();
            else
                item_byte_size = 1;
        }

        command.Shift(); // shift off the address argument
        uint64_t uval64;
        int64_t sval64;
        bool success = false;
        const uint32_t num_value_args = command.GetArgumentCount();
        uint32_t i;
        for (i=0; i<num_value_args; ++i)
        {
            const char *value_str = command.GetArgumentAtIndex(i);

            switch (m_format_options.GetFormat())
            {
            case kNumFormats:
            case eFormatFloat:  // TODO: add support for floats soon
            case eFormatCharPrintable:
            case eFormatBytesWithASCII:
            case eFormatComplex:
            case eFormatEnum:
            case eFormatUnicode16:
            case eFormatUnicode32:
            case eFormatVectorOfChar:
            case eFormatVectorOfSInt8:
            case eFormatVectorOfUInt8:
            case eFormatVectorOfSInt16:
            case eFormatVectorOfUInt16:
            case eFormatVectorOfSInt32:
            case eFormatVectorOfUInt32:
            case eFormatVectorOfSInt64:
            case eFormatVectorOfUInt64:
            case eFormatVectorOfFloat32:
            case eFormatVectorOfFloat64:
            case eFormatVectorOfUInt128:
            case eFormatOSType:
            case eFormatComplexInteger:
                result.AppendError("unsupported format for writing memory");
                result.SetStatus(eReturnStatusFailed);
                return false;

            case eFormatDefault:
            case eFormatBytes:
            case eFormatHex:
            case eFormatPointer:
                
                // Decode hex bytes
                uval64 = Args::StringToUInt64(value_str, UINT64_MAX, 16, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid hex string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!UIntValueIsValidForSize (uval64, item_byte_size))
                {
                    result.AppendErrorWithFormat ("Value 0x%llx is too large to fit in a %lu byte unsigned integer value.\n", uval64, item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;

            case eFormatBoolean:
                uval64 = Args::StringToBoolean(value_str, false, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid boolean string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;

            case eFormatBinary:
                uval64 = Args::StringToUInt64(value_str, UINT64_MAX, 2, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid binary string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!UIntValueIsValidForSize (uval64, item_byte_size))
                {
                    result.AppendErrorWithFormat ("Value 0x%llx is too large to fit in a %lu byte unsigned integer value.\n", uval64, item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;

            case eFormatCharArray:
            case eFormatChar:
            case eFormatCString:
                if (value_str[0])
                {
                    size_t len = strlen (value_str);
                    // Include the NULL for C strings...
                    if (m_format_options.GetFormat() == eFormatCString)
                        ++len;
                    Error error;
                    if (process->WriteMemory (addr, value_str, len, error) == len)
                    {
                        addr += len;
                    }
                    else
                    {
                        result.AppendErrorWithFormat ("Memory write to 0x%llx failed: %s.\n", addr, error.AsCString());
                        result.SetStatus(eReturnStatusFailed);
                        return false;
                    }
                }
                break;

            case eFormatDecimal:
                sval64 = Args::StringToSInt64(value_str, INT64_MAX, 0, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid signed decimal value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!SIntValueIsValidForSize (sval64, item_byte_size))
                {
                    result.AppendErrorWithFormat ("Value %lli is too large or small to fit in a %lu byte signed integer value.\n", sval64, item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (sval64, item_byte_size);
                break;

            case eFormatUnsigned:
                uval64 = Args::StringToUInt64(value_str, UINT64_MAX, 0, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid unsigned decimal string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!UIntValueIsValidForSize (uval64, item_byte_size))
                {
                    result.AppendErrorWithFormat ("Value %llu is too large to fit in a %lu byte unsigned integer value.\n", uval64, item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;

            case eFormatOctal:
                uval64 = Args::StringToUInt64(value_str, UINT64_MAX, 8, &success);
                if (!success)
                {
                    result.AppendErrorWithFormat ("'%s' is not a valid octal string value.\n", value_str);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                else if (!UIntValueIsValidForSize (uval64, item_byte_size))
                {
                    result.AppendErrorWithFormat ("Value %llo is too large to fit in a %lu byte unsigned integer value.\n", uval64, item_byte_size);
                    result.SetStatus(eReturnStatusFailed);
                    return false;
                }
                buffer.PutMaxHex64 (uval64, item_byte_size);
                break;
            }
        }

        if (!buffer.GetString().empty())
        {
            Error error;
            if (process->WriteMemory (addr, buffer.GetString().c_str(), buffer.GetString().size(), error) == buffer.GetString().size())
                return true;
            else
            {
                result.AppendErrorWithFormat ("Memory write to 0x%llx failed: %s.\n", addr, error.AsCString());
                result.SetStatus(eReturnStatusFailed);
                return false;
            }
        }
        return true;
    }

protected:
    
    OptionGroupOptions m_option_group;
    OptionGroupFormat m_format_options;
    OptionGroupWriteMemory m_memory_options;
};


//-------------------------------------------------------------------------
// CommandObjectMemory
//-------------------------------------------------------------------------

CommandObjectMemory::CommandObjectMemory (CommandInterpreter &interpreter) :
    CommandObjectMultiword (interpreter,
                            "memory",
                            "A set of commands for operating on memory.",
                            "memory <subcommand> [<subcommand-options>]")
{
    LoadSubCommand ("read",  CommandObjectSP (new CommandObjectMemoryRead (interpreter)));
    LoadSubCommand ("write", CommandObjectSP (new CommandObjectMemoryWrite (interpreter)));
}

CommandObjectMemory::~CommandObjectMemory ()
{
}
