// Copyright (C) 2022 Kyle Sylvestre
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

using String = std::basic_string<char, std::char_traits<char>>;

#define VARGS_CHECK(fmt, ...) (0 && snprintf(NULL, 0, fmt, __VA_ARGS__))
#define StringPrintf(fmt, ...) _StringPrintf(VARGS_CHECK(fmt, __VA_ARGS__), fmt, __VA_ARGS__)
String _StringPrintf(int vargs_check, const char* fmt, ...);

#define ArrayCount(arr) (sizeof(arr) / sizeof(arr[0]))
#define tsnprintf(buf, fmt, ...) snprintf(buf, sizeof(buf), fmt, __VA_ARGS__)
#define DefaultInvalid                            \
    default:                                      \
        PrintError("hit invalid default switch"); \
        break;
#define GetMax(a, b) (a > b) ? a : b
#define GetMin(a, b) (a < b) ? a : b
#define GetPinned(v, min, max) GetMin(GetMax(v, min), max)
#define GetAbs(a, b) (a > b) ? a - b : b - a

template <typename T>
inline void Zeroize(T& value)
{
    memset(&value, 0, sizeof(value));
}

// c standard wrappers
#if !defined(NDEBUG)
#define Assert(cond)                                        \
    if (!(cond)) {                                          \
        char _gdb_buf[128];                                 \
        tsnprintf(_gdb_buf, "gdb --pid %d", (int)getpid()); \
        int _rc = system(_gdb_buf);                         \
        (void)_rc;                                          \
        exit(1);                                            \
    }
#else
#define Assert(cond) (void)0;
#endif

#define Printf(fmt, ...)                                \
    do {                                                \
        String _msg = StringPrintf(fmt, __VA_ARGS__);   \
        WriteToConsoleBuffer(_msg.data(), _msg.size()); \
    } while (0)
#define Print(msg) Printf("%s", msg)

// log user error message
#define PrintError(str) PrintErrorf("%s", str)
#define PrintErrorf(fmt, ...)                                                 \
    do {                                                                      \
        fprintf(stderr, "(%s : %s : %d) ", __FILE__, __FUNCTION__, __LINE__); \
        String _msg = StringPrintf("Error " fmt, __VA_ARGS__);                \
        fprintf(stderr, "%s", _msg.c_str());                                  \
        WriteToConsoleBuffer(_msg.data(), _msg.size());                       \
        /*Assert(false);*/                                                    \
    } while (0)

#define INVALID_LINE 0
#define RECORD_ROOT_IDX 0

// prefix for preventing name clashes
#define GLOBAL_NAME_PREFIX "GB__"
#define LOCAL_NAME_PREFIX "LC__"

// values with child elements from -data-evaluate-expression
// struct: value={ a = "foo", b = "bar", c = "baz" }
// union: value={ a = "foo", b = "bar", c = "baz" }
// array: value={1, 2, 3}
#define AGGREGATE_CHAR_START '{'
#define AGGREGATE_CHAR_END '}'

// maximum amount of variables displayed in an expression if there
// are no run length values
#define AGGREGATE_MAX 200

const char* const DEFAULT_REG_ARM[] = {
    "r0",
    "r1",
    "r2",
    "r3",
    "r4",
    "r5",
    "r6",
    "r7",
    "r8",
    "r9",
    "r10",
    "r11",
    /*"fp",*/ "r12",
    "sp",
    "lr",
    "pc",
    "cpsr",
};

const char* const DEFAULT_REG_AMD64[] = {
    "rax", "rbx", "rcx", "rdx",
    "rbp", "rsp", "rip", "rsi",
    "rdi", "r8", "r9", "r10", "r11",
    "r12", "r13", "r14", "r15"
};

const char* const DEFAULT_REG_X86[] = {
    "eax",
    "ebx",
    "ecx",
    "edx",
    "ebp",
    "esp",
    "eip",
    "esi",
    "edi",
};

// TODO: threads
struct Frame {
    String func;
    uint64_t addr; // current PC/IP
    size_t file_idx; // in prog.files
    size_t line_idx; // next line to be executed - 1
};

struct Breakpoint {
    uint64_t addr;
    size_t number; // ordinal assigned by GDB
    size_t line_idx; // file line number - 1
    size_t file_idx; // index in prog.files
    bool enabled;
    String cond;
};

struct DisassemblyLine {
    uint64_t addr;
    String text;
};

struct DisassemblySourceLine {
    uint64_t addr;
    size_t num_instructions;
    size_t line_idx;
};

struct File {
    std::vector<size_t> lines; // offset to line within data
    String filename;
    String data; // file chars excluding line endings
    size_t longest_line_idx; // line with most chars, used for horizontal scrollbar
};

#define INVALID_BLOCK_STRING_IDX 0

enum AtomType {
    Atom_None, // parse state
    Atom_Name, // parse state
    Atom_Array,
    Atom_Struct,
    Atom_String,
};

// range of data that lives inside another buffer
struct Span {
    size_t index;
    size_t length;
};

struct RecordAtom {
    AtomType type;

    // text span inside Record.buf
    Span name;

    // variant variable based upon type
    // array/struct= array span inside Record.atoms
    // string= text span inside Record.buf
    Span value;
};

struct Record {
    // ordinal that gets send preceding MI-Commands that gets sent
    // back in the response record
    uint32_t id;

    // data describing the line elements
    std::vector<RecordAtom> atoms;

    // line buffer, RecordAtom name/value strings point to data inside this
    String buf;
};

struct RecordHolder {
    bool parsed;
    Record rec;
};

struct VarObj {
    String name;
    String value;
    bool changed;

    // structs, unions, arrays
    Record expr;
    std::vector<bool> expr_changed;
};

// run length RecordAtom in expression value
struct RecordAtomSequence {
    RecordAtom atom;
    size_t length;
};

struct Thread {
    int id;
    String group_id;
    bool running;
    bool focused; // thread is included in ExecuteCommand
};

// GDB MI sends output as human readable lines, starting with a symbol
// * = exec-async-output contains asynchronous state change on the target
//     (stopped, started, disappeared)
//
// & = log-stream-output is debugging messages being produced by GDB’s internals.
//
// ^ = record
//
// @ = The target output stream contains any textual output from the running target.
//     This is only present when GDB’s event loop is truly asynchronous,
//     which is currently only the case for remote targets.
//
// ~ = console-stream-output is output that should be displayed
//     as is in the console. It is the textual response to a CLI command
//
// after the commands, it ends with signature "(gdb)"

#define PREFIX_ASYNC0 '='
#define PREFIX_ASYNC1 '*'
#define PREFIX_RESULT '^'
#define PREFIX_DEBUG_LOG '&'
#define PREFIX_TARGET_LOG '@'
#define PREFIX_CONSOLE_LOG '~'

const size_t BAD_INDEX
    = ~0;

#define FILE_IDX_INVALID 0

struct Program {
    // console messages ordered from newest to oldest
    char log[64 * 1024];
    bool log_scroll_to_bottom = true;
    size_t log_idx;

    // GDB console history buffer
    String input_cmd_data;
    std::vector<size_t> input_cmd_offsets;
    int input_cmd_idx = -1;

    std::vector<VarObj> local_vars; // locals for the current frame
    std::vector<VarObj> global_vars; // watch for entire program, -var-create name @ expr
    std::vector<VarObj> watch_vars; // user defined watch for entire program
    bool running;
    bool started;
    bool source_out_of_date;
    std::vector<Breakpoint> breakpoints;
    // TODO: threads, active_thread

    std::vector<RecordHolder> read_recs;
    size_t num_recs;

    std::vector<File> files;
    std::vector<Thread> threads;
    std::vector<Frame> frames;
    size_t frame_idx = BAD_INDEX;
    size_t file_idx = BAD_INDEX;
    size_t thread_idx = BAD_INDEX;
    pid_t inferior_process;
    String stack_sig; // string of all function names combined
};

extern Program prog;

const char* GetErrorString(int _errno);
void WriteToConsoleBuffer(const char* raw, size_t rawsize);
bool is_executable(const char* path);
bool DoesProcessExist(pid_t p);
bool InvokeShellCommand(std::string_view command, String& output);
