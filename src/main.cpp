#include <fstream>
#include <functional>
#include <filesystem>
#include <unistd.h>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <libgen.h>
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <fcntl.h>
#include <semaphore.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/stat.h>

// third party
#include <GLFW/glfw3.h>
#include <imgui/all.hpp>
#include <liberation_mono.h>

// Mine
#include "zero.hpp"
#include "os.hpp"
#include "common.h"
#include "gdb.h"

#include "os.cpp"
#include "gdb.cpp"

// imgui/misc/imgui_stdlib.cpp
namespace ImGui {
struct InputTextCallback_UserData {
    String* Str;
    ImGuiInputTextCallback ChainCallback;
    void* ChainCallbackUserData;
};

static int InputTextCallback(ImGuiInputTextCallbackData* data)
{
    InputTextCallback_UserData* user_data = (InputTextCallback_UserData*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        // Resize string callback
        // If for some reason we refuse the new length (BufTextLen) and/or capacity (BufSize) we
        // need to set them back to what we want.
        String* str = user_data->Str;
        IM_ASSERT(data->Buf == str->c_str());
        str->resize(data->BufTextLen);
        data->Buf = (char*)str->c_str();
    } else if (user_data->ChainCallback) {
        // Forward to user callback, if any
        data->UserData = user_data->ChainCallbackUserData;
        return user_data->ChainCallback(data);
    }
    return 0;
}

bool InputText(const char* label, String* str, ImGuiInputTextFlags flags = 0,
    ImGuiInputTextCallback callback = NULL, void* user_data = NULL)
{
    IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
    flags |= ImGuiInputTextFlags_CallbackResize;

    InputTextCallback_UserData cb_user_data = {};
    cb_user_data.Str = str;
    cb_user_data.ChainCallback = callback;
    cb_user_data.ChainCallbackUserData = user_data;
    return InputText(
        label, (char*)str->c_str(), str->capacity() + 1, flags, InputTextCallback, &cb_user_data);
}
}

#include <errnoname.c>
// get error macro name and description
// ex: ENOENT No such file or directory
const char* GetErrorString(int _errno)
{
    thread_local char buf[4096] = {};
    char errname[1024] = {};
    const char* n = errnoname(_errno); // equivalent to strerrorname_np on newer machines
    if (n) {
        tsnprintf(errname, "%s", n);
    } else {
        tsnprintf(errname, "ERRNO %d", _errno);
    }
    tsnprintf(buf, "%s %s", errname, strerror(_errno));
    return buf;
}

// dynamic colors that change upon the brightness of the background
ImVec4 IM_COL32_WIN_RED;

static bool IsKeyPressed(ImGuiKey key, ImGuiKeyModFlags mod = ImGuiKeyModFlags_None)
{
    bool result = false;
    ImGuiIO& io = ImGui::GetIO();
    bool key_in_range = (size_t)key < ArrayCount(ImGuiIO::KeysData);
    Assert(key_in_range);

    if (key_in_range) {
        result = ImGui::IsKeyPressed(key);
        if (mod & ImGuiKeyModFlags_Alt)
            result &= io.KeyAlt;
        if (mod & ImGuiKeyModFlags_Ctrl)
            result &= io.KeyCtrl;
        if (mod & ImGuiKeyModFlags_Shift)
            result &= io.KeyShift;
        if (mod & ImGuiKeyModFlags_Super)
            result &= io.KeySuper;
    }

    return result;
}

String _StringPrintf(int /* vargs_check */, const char* fmt, ...)
{
    String result;
    va_list args;
    va_start(args, fmt);
    errno = 0;
    int len_minus_nt = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (len_minus_nt < 0) {
        PrintErrorf("vsnprintf %s\n", GetErrorString(errno));
        result = "";
    } else {
        result.resize(len_minus_nt + 1, '\0');
        va_start(args, fmt);
        int copied_minus_nt = vsnprintf((char*)result.data(), result.size(), fmt, args);
        va_end(args);

        if (copied_minus_nt < 0) {
            PrintErrorf("vsnprintf %s\n", GetErrorString(errno));
            result = "";
        } else {
            // remove trailing NT, std::string provides one with .c_str()
            result.pop_back();
        }
    }

    return result;
}

void ResetProgramState()
{
    prog.local_vars.clear();
    for (VarObj& iter : prog.watch_vars) {
        String name = iter.name;
        iter = {};
        iter.name = name;
        iter.value = "???";
    }

    prog.running = false;
    prog.started = false;
    prog.source_out_of_date = false;

    prog.read_recs.clear();
    prog.num_recs = 0;

    prog.frames.clear();
    prog.frame_idx = BAD_INDEX;
    prog.inferior_process = 0;

    prog.threads.clear();
    prog.thread_idx = BAD_INDEX;
}

enum class LineDisplay {
    Source,
    Disassembly,
    Source_And_Disassembly,
};

enum WindowTheme {
    WindowTheme_Light,
    WindowTheme_DarkPurple,
    WindowTheme_DarkBlue,
};

enum Jump {
    Jump_None,
    Jump_Goto,
    Jump_Search,
    Jump_Stopped,
};

#define DEFAULT_FONT_SIZE 16.0f
#define MIN_FONT_SIZE 8.0f
#define MAX_FONT_SIZE 72.0f

struct Session {
    String debug_exe;
    String debug_args;
};

struct GUI {
    GLFWwindow* window;
    LineDisplay line_display = LineDisplay::Source;
    std::vector<DisassemblyLine> line_disasm;
    std::vector<DisassemblySourceLine> line_disasm_source;
    bool show_machine_interpreter_commands;

    Jump jump_type;
    bool source_search_bar_open;
    char source_search_keyword[256];
    bool source_found_line;
    size_t source_found_line_idx;
    size_t goto_line_idx;
    bool refresh_docking_space = true;

    // use two font sizes: global and source window
    // change source window size with CTRL+Scroll or settings option
    bool use_default_font = true;
    ImFont* default_font;
    float font_size = DEFAULT_FONT_SIZE;
    String font_filename;
    ImFont* source_font;
    float source_font_size = DEFAULT_FONT_SIZE;

    bool show_source = true;
    bool show_control = true;
    bool show_callstack = true;
    bool show_registers = false;
    bool show_locals = true;
    bool show_watch = true;
    bool show_breakpoints = false;
    bool show_threads = false;
    bool show_directory_viewer = false;
    bool show_about_tug = false;
    WindowTheme window_theme = WindowTheme_DarkBlue;
    std::vector<Session> session_history;
    int hover_delay_ms = 100;
    String drag_drop_exe_path;

    // shutdown variables
    bool started_imgui_opengl2;
    bool started_imgui_glfw;
    bool created_imgui_context;
    bool initialized_glfw;
};

Program prog;
GDB g_gdb;
GUI gui;

static uint64_t ParseHex(const String& str)
{
    uint64_t result = 0;
    uint64_t pow = 1;
    for (size_t i = str.size() - 1; i < str.size(); i--) {
        uint64_t num = 0;
        char c = str[i];
        if (c == 'x' || c == 'X')
            break;

        if (c >= 'a' && c <= 'f') {
            num = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            num = 10 + (c - 'A');
        } else if (c >= '0' && c <= '9') {
            num = c - '0';
        }
        result += (num * pow);
        pow *= 16;
    }
    return result;
}

static void SetWindowTheme(WindowTheme theme)
{
    static const auto GetLuminance01 = [](ImColor col) -> float {
        return (0.2126 * col.Value.x) + (0.7152 * col.Value.y) + (0.0722 * col.Value.z);
    };
    float lum = GetLuminance01(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    IM_COL32_WIN_RED = ImColor(1.0f, 0.5f - 0.5f * lum, 0.5f - 0.5f * lum, 1.0f);
    ImGuiStyle& style = ImGui::GetStyle();

    switch (theme) {
    case WindowTheme_Light: {
        ImGui::StyleColorsLight();
        style.FrameBorderSize = 1.0f;

        // make popups grey background
        style.Colors[ImGuiCol_PopupBg] = style.Colors[ImGuiCol_WindowBg];
    } break;

    case WindowTheme_DarkPurple: {
        ImGui::StyleColorsClassic();
        style.FrameBorderSize = 0.0f;
    } break;

    case WindowTheme_DarkBlue: {
        ImGui::StyleColorsDark();
        style.FrameBorderSize = 0.0f;
    } break;

        DefaultInvalid
    }

    gui.window_theme = theme;
}

bool LoadFile(File& file)
{
    struct stat sb = {};
    if (file.lines.size() != 0 || stat(file.filename.c_str(), &sb) != 0)
        return false;

    FILE* f = fopen(file.filename.c_str(), "rb");
    if (f == NULL) {
        PrintErrorf("fopen %s\n", GetErrorString(errno));
        return false;
    }

    size_t filesize = sb.st_size;
    file.data.resize(filesize);
    if (0 < fread((void*)file.data.data(), 1, filesize, f)) {
        // move file up so that the data will be packed
        // lines will be accessed by offsetting into one big buf
        char* fd = (char*)file.data.data();
        size_t i = 0;
        char* lst = fd;
        size_t num_trunc = 0;

        while (i < filesize) {
            char c0 = fd[i];
            char c1 = (i + 1 < filesize) ? fd[i + 1] : '\0';
            size_t end = (c0 == '\n') ? 1 : (c0 == '\r' && c1 == '\n') ? 2 : (c0 == '\r') ? 1 : 0;

            if (end != 0) {
                char* dest = lst - num_trunc;
                memmove(dest, lst, (fd + i) - lst);

                file.lines.push_back(dest - fd);

                num_trunc += end;
                i += end;
                lst = fd + i;
            } else {
                i++;
            }
        }

        // truncate file to size minus sum of line endings
        file.data.resize(file.data.size() - num_trunc);
    }

    // TODO: possibly not the longest line when line number is large enough
    // format is [breakpoint] [ %-4d line number] [line]
    file.longest_line_idx = 0;
    size_t max_chars = 0;
    size_t len = file.data.size();
    for (size_t i = file.lines.size() - 1; i < file.lines.size(); i--) {
        size_t this_line_len = len - file.lines[i];
        if (max_chars < this_line_len) {
            max_chars = this_line_len;
            file.longest_line_idx = i;
        }

        len = file.lines[i];
    }

    fclose(f);
    return true;
}

static size_t FindOrCreateFile(const String& filename)
{
    size_t result = BAD_INDEX;

    // search for the stored file context
    for (size_t i = 0; i < prog.files.size(); i++) {
        if (prog.files[i].filename == filename) {
            result = i;
            break;
        }
    }

    // file not found add new entry
    // load file lines on calling LoadFile
    if (result == BAD_INDEX) {
        result = prog.files.size();
        prog.files.resize(prog.files.size() + 1);
        prog.files[result].filename = filename;
    }

    return result;
}

#define ImGuiDisabled(is_disabled, code) \
    ImGui::BeginDisabled(is_disabled);   \
    code;                                \
    ImGui::EndDisabled();

void WriteToConsoleBuffer(const char* buf, size_t bufsize)
{
    bool is_mi_record = (bufsize > 0)
        && (buf[0] == PREFIX_ASYNC0 || buf[0] == PREFIX_ASYNC1 || buf[0] == PREFIX_RESULT);

    if (is_mi_record && !gui.show_machine_interpreter_commands)
        return;

    // truncate messages too large
    bufsize = GetMin(bufsize, sizeof(prog.log));

    const auto PushChar = [&](char c) {
        Assert(prog.log_idx <= sizeof(prog.log));
        if (prog.log_idx == sizeof(prog.log)) {
            // exhaused log data, pop memory from the front
            size_t i = sizeof(prog.log) / 16;
            while (i < sizeof(prog.log) && (prog.log[i] != '\n' && prog.log[i] != '\0')) {
                i++; // walk until the end of a line or unused data
            }

            // move line data up, clear moved area
            Assert(prog.log_idx >= i);
            memmove(prog.log, prog.log + i, prog.log_idx - i);
            prog.log_idx -= i;
            memset(prog.log + prog.log_idx, 0, sizeof(prog.log) - prog.log_idx);
        }

        if (c == '\n' || (c >= 32 && c <= 126))
            prog.log[prog.log_idx++] = c;
    };

    if (bufsize > 2
        && (buf[0] == PREFIX_DEBUG_LOG || buf[0] == PREFIX_TARGET_LOG
            || buf[0] == PREFIX_CONSOLE_LOG)
        && buf[1] == '\"') {
        // console record, format ~"text text text"\n
        // skip over the beginning/ending characters
        size_t i = 2;
        bufsize -= 2;

        for (; i < bufsize; i++) {
            char c = buf[i];
            char n = (i + 1 < bufsize) ? buf[i + 1] : '\0';

            if (c == '\\') {
                switch (n) {
                case 'n':
                    PushChar('\n');
                    break;
                case 't':
                    for (size_t t = 0; t < 2; t++)
                        PushChar(' ');
                    break;
                case '\\':
                case '\"':
                    PushChar(n);
                    break;
                default:
                    break;
                }

                i++; // skip over the evaluated literal char
            } else {
                PushChar(buf[i]);
            }
        }
    } else {
        // text that isn't a log record ex: shell ls
        for (size_t i = 0; i < bufsize; i++)
            PushChar(buf[i]);

        // newline is chopped in user input, parsed as MI record
        // Printf newline isn't chopped, check last char
        if (bufsize > 0 && buf[bufsize - 1] != '\n')
            PushChar('\n');
    }

    prog.log_scroll_to_bottom = true;
}

VarObj CreateVarObj(String name, String value = "")
{
    VarObj result = {};
    result.name = name;
    result.value = value;
    result.changed = true;

    if (result.value == "") {
        result.value = "???";
    }

    if (result.value[0] == '{') {
        static bool resize_once = true;
        value = name + " = " + value;
        static struct ParseRecordContext ctx = {};

        if (resize_once) {
            resize_once = false;
            ctx.atoms.resize(16 * 1024); // @Hack
        }

        ctx.i = 0;
        ctx.atom_idx = 0;
        ctx.num_end_atoms = 0;
        ctx.error = false;
        ctx.buf = value.c_str();
        ctx.bufsize = value.size();

        RecordAtom root = gdb::recurse_evaluation(ctx).atom;

        if (!ctx.error) {
            // put the root in place since it doesn't get popped to the
            // ordered section of the array
            ctx.num_end_atoms++;

            // atoms are in order at the end of the array,
            // subtract the difference from base ordered
            Assert(ctx.num_end_atoms <= ctx.atoms.size());
            size_t ordered_offset = ctx.atoms.size() - ctx.num_end_atoms;
            RecordAtom* ordered_base = &ctx.atoms[ordered_offset];
            ordered_base[0] = root;

            for (size_t i = 0; i < ctx.num_end_atoms; i++) {
                RecordAtom* atom = &ordered_base[i];
                if ((atom->type == Atom_Array || atom->type == Atom_Struct)
                    && (atom->value.length != 0)) {
                    // adjust aggregate offset
                    Assert(atom->value.index > ordered_offset);
                    atom->value.index -= ordered_offset;
                }
            }

            result.expr.atoms.resize(ctx.num_end_atoms);
            memcpy(&result.expr.atoms[0], &ctx.atoms[ctx.atoms.size() - ctx.num_end_atoms],
                sizeof(RecordAtom) * ctx.num_end_atoms);
            result.expr.buf = value;
            result.expr_changed.resize(ctx.num_end_atoms);

            // GDB_PrintRecordAtom(result.expr, result.expr.atoms[0], 0, out);
            //  remove format backslashes in atom strings
            //  ignore those of name "value" because these get handled in GDB_RecurseEvaluation
            const auto RemoveStringBackslashes
                = [](Record& record, RecordAtom& iter, void* /* user context */) {
                      if (iter.type == Atom_String) {
                          size_t new_length = iter.value.length;
                          for (size_t i = 0; i < iter.value.length; i++) {
                              size_t buf_idx = iter.value.index + i;
                              char c = record.buf[buf_idx];
                              char n = (i + 1 < iter.value.length) ? record.buf[buf_idx + 1] : '\0';
                              if (c == '\\' && (n == '\\' || n == '\"')) {
                                  // record.buf[iter.value.index + new_length - 1] = ' ';
                                  memmove(&record.buf[buf_idx], &record.buf[buf_idx + 1],
                                      iter.value.length - (i + 1));
                                  new_length--;
                              }
                          }
                          iter.value.length = new_length;
                      }
                  };

            if (result.expr.atoms.size() > 1)
                gdb::iterate_atoms(result.expr, result.expr.atoms[0], RemoveStringBackslashes, NULL);
        }
    }

    return result;
}

bool RecurseCheckChanged(
    VarObj& this_var, size_t this_parent_idx, const VarObj& last_var, size_t last_parent_idx)
{
    bool changed = false;
    Assert((this_parent_idx < this_var.expr.atoms.size())
        && (last_parent_idx < last_var.expr.atoms.size()));

    RecordAtom& this_parent = this_var.expr.atoms[this_parent_idx];
    const RecordAtom& last_parent = last_var.expr.atoms[last_parent_idx];
    Assert((this_parent.type == Atom_Struct || this_parent.type == Atom_Array)
        && (this_parent.type == last_parent.type));

    if (this_parent.value.length == last_parent.value.length) {
        size_t t_idx = this_parent.value.index;
        size_t t_end = t_idx + this_parent.value.length;
        size_t o_idx = last_parent.value.index;
        size_t o_end = o_idx + last_parent.value.length;

        for (; t_idx < t_end && o_idx < o_end; t_idx++, o_idx++) {
            const RecordAtom& this_child = this_var.expr.atoms[t_idx];
            const RecordAtom& last_child = last_var.expr.atoms[o_idx];
            if (this_child.type == Atom_Struct || this_child.type == Atom_Array) {
                changed |= RecurseCheckChanged(this_var, t_idx, last_var, o_idx);
            } else if (this_child.type == Atom_String) {
                const char* this_buf = this_var.expr.buf.c_str();
                const char* last_buf = last_var.expr.buf.c_str();

                Assert(
                    this_child.value.index + this_child.value.length <= this_var.expr.buf.size());
                Assert(
                    last_child.value.index + last_child.value.length <= last_var.expr.buf.size());

                const char* this_text = this_buf + this_child.value.index;
                const char* last_text = last_buf + last_child.value.index;

                this_var.expr_changed[t_idx] = (this_child.value.length != last_child.value.length)
                    || (0 != memcmp(this_text, last_text, this_child.value.length));

                changed |= this_var.expr_changed[t_idx];
            } else {
                Assert(false);
            }
        }
    } else {
        // atom array/struct changed its length, set all to changed
        changed = true;
        size_t t_idx = this_parent.value.index;
        size_t t_end = t_idx + this_parent.value.length;
        for (size_t i = t_idx; i < t_end; i++) {
            this_var.expr_changed[i] = true;
        }
    }

    this_var.expr_changed[this_parent_idx] = changed;
    return changed;
}

void CheckIfChanged(VarObj& this_var, const VarObj& last_var)
{
    bool this_agg = this_var.value[0] == '{';
    bool last_agg = last_var.value[0] == '{';
    if (this_agg && last_agg) {
        // aggregate, go through each child and check if it changed
        this_var.changed = RecurseCheckChanged(this_var, 0, last_var, 0);
    } else if (!this_agg && !last_agg) {
        this_var.changed = (this_var.value != last_var.value);
    } else {
        this_var.changed = true;
        for (size_t i = 0; i < this_var.expr_changed.size(); i++)
            this_var.expr_changed[i] = true;
    }
}

int GetActiveThreadID()
{
    int result = 0;
    if (prog.thread_idx < prog.threads.size()) {
        result = prog.threads[prog.thread_idx].id;
    }
    return result;
}

bool ExecuteCommand(const char* cmd, bool remove_after = true)
{
    // @GDB: interpreter bugs out with -exec-continue --all with no threads to contiue
    if (prog.threads.size() == 0)
        return false;

    bool result = false;
    String mi;
    bool focused_all = true;
    for (Thread& t : prog.threads)
        if (!t.focused)
            focused_all = false;

    if (focused_all) {
        mi = StringPrintf("%s --all", cmd);
        result = gdb::send_blocking(mi.c_str(), remove_after);
    } else {
        result = true;
        for (size_t i = 0; i < prog.threads.size(); i++) {
            if (prog.threads[i].focused) {
                mi += StringPrintf("%s --thread %d", cmd, prog.threads[i].id);
                result &= gdb::send_blocking(mi.c_str(), remove_after);
            }
        }
    }

    return result;
}

void QueryWatchlist()
{
    // evaluate user defined watch variables
    Record rec;
    for (VarObj& iter : prog.watch_vars) {
        String expr;
        const char* src = iter.name.c_str();
        const char* comma = strchr(src, ',');

        if (comma != NULL) {
            // translate visual studio syntax to GDB syntax
            // arrayname, 10 -> *arrayname@10
            expr = StringPrintf("*(%.*s)@%s", (int)(comma - src), src, comma + 1);
        } else {
            expr = iter.name;
        }

        String cmd = StringPrintf("-data-evaluate-expression --frame %zu --thread %d \"%s\"",
            prog.frame_idx, GetActiveThreadID(), expr.c_str());

        VarObj incoming = {};
        incoming.name = iter.name;
        incoming.value = "???";
        if (gdb::send_blocking(cmd.c_str(), rec)) {
            static uint32_t counter = 0;
            String exprname = StringPrintf("expression##%u", counter);
            counter++;

            incoming = CreateVarObj(exprname, gdb::extract_value("value", rec));
        }

        CheckIfChanged(incoming, iter);
        iter.value = incoming.value;
        iter.expr = incoming.expr;
        iter.changed = incoming.changed;
        iter.expr_changed = incoming.expr_changed;
    }
}

void GetFunctionDisassembly(const Frame& frame)
{
    char tmpbuf[4096];
    Record rec = {};
    gui.line_disasm.clear();
    gui.line_disasm_source.clear();

    // functions with this name don't support function disassembly from address or file/line combos
    // found this type of function in file: /lib64/ld-linux-x86-64.so.2
    if (frame.func == "??")
        return;

    const File& file = prog.files[frame.file_idx];
    if (file.lines.size() == 0) {
        if (!(g_gdb.capabilities & GDB_DATADIS)) {
            return; // operation not supported, bail early
        } else {
            // some frames don't have an associated file ex: _start function after returning from
            // main
            tsnprintf(tmpbuf, "-data-disassemble -a %s 0", // 0 = disasm only
                frame.func.c_str());
        }
    } else {
        // -n -1 = disassemble all lines in the function its contained in
        // 5 = source and disasm with opcodes
        tsnprintf(tmpbuf, "-data-disassemble -f \"%s\" -l %zu -n -1 5", file.filename.c_str(),
            frame.line_idx + 1);
    }

    gdb::send_blocking(tmpbuf, rec);

    const RecordAtom* instrs = gdb::extract_atom("asm_insns", rec);
    if (file.lines.size() != 0) {
        for (const RecordAtom& src_and_asm_line : gdb::iter_child(rec, instrs)) {
            // array of src_and_asm_line
            //     line="32"
            //     file="debug.c"
            //     fullname="/mnt/c/Users/Kyle/Documents/Visual Studio 2017/Projects/Tug/debug.c"
            //     line_asm_insn
            bool is_first_inst = true;
            DisassemblySourceLine line_src = {};
            const RecordAtom* atom = gdb::extract_atom("line_asm_insn", src_and_asm_line, rec);
            line_src.line_idx = (size_t)gdb::extract_int("line", src_and_asm_line, rec) - 1;
            line_src.num_instructions = 0;

            for (const RecordAtom& line_asm_inst : gdb::iter_child(rec, atom)) {
                // array of unnamed struct
                //     address="0x0000555555555248"
                //     func-name="main"
                //     offset="176"
                //     opcodes="74 05"
                //     line_asm_inst="je     0x55555555524f <main+183>"

                DisassemblyLine add = {};
                String string_addr = gdb::extract_value("address", line_asm_inst, rec);
                String func = gdb::extract_value("func-name", line_asm_inst, rec);
                String offset_from_func = gdb::extract_value("offset", line_asm_inst, rec);
                String inst = gdb::extract_value("inst", line_asm_inst, rec);
                String opcodes = gdb::extract_value("opcodes", line_asm_inst, rec);

                tsnprintf(tmpbuf, "%s <%s+%s> %s", string_addr.c_str(), func.c_str(),
                    offset_from_func.c_str(), inst.c_str());

                add.addr = ParseHex(string_addr);
                add.text = tmpbuf;
                gui.line_disasm.emplace_back(add);
                line_src.num_instructions++;

                if (is_first_inst) {
                    line_src.addr = add.addr;
                    is_first_inst = false;
                }
            }

            gui.line_disasm_source.emplace_back(line_src);
        }
    } else {
        // getting function disassembly for a fileless frame
        for (const RecordAtom& line_asm_inst : gdb::iter_child(rec, instrs)) {
            // array of unnamed struct
            //     address="0x0000555555555248"
            //     func-name="main"
            //     offset="176"
            //     opcodes="74 05"
            //     inst="je     0x55555555524f <main+183>"

            DisassemblyLine add = {};
            String string_addr = gdb::extract_value("address", line_asm_inst, rec);
            String func = gdb::extract_value("func-name", line_asm_inst, rec);
            String offset_from_func = gdb::extract_value("offset", line_asm_inst, rec);
            String inst = gdb::extract_value("inst", line_asm_inst, rec);
            String opcodes = gdb::extract_value("opcodes", line_asm_inst, rec);

            tsnprintf(tmpbuf, "%s <%s+%s> %s", string_addr.c_str(), func.c_str(),
                offset_from_func.c_str(), inst.c_str());

            add.addr = ParseHex(string_addr);
            add.text = tmpbuf;
            gui.line_disasm.emplace_back(add);
        }
    }
}

void RecurseSetNodeState(const Record& rec, size_t atom_idx, int state, String name)
{
    const RecordAtom& parent = rec.atoms[atom_idx];
    ImGuiID parent_id = ImGui::GetID(name.c_str());
    ImGui::GetStateStorage()->SetInt(parent_id, state);

    for (size_t i = 0; i < parent.value.length; i++) {
        size_t childoffset = parent.value.index + i;
        const RecordAtom& child = rec.atoms[childoffset];
        String childname = name + String(rec.buf.c_str() + child.name.index, child.name.length);
        if (child.type == Atom_Array || child.type == Atom_Struct) {
            RecurseSetNodeState(rec, childoffset, state, childname);
        }
    }
}

// draw an aggregate data type in the form of a two column table
void RecurseExpressionTreeNodes(const VarObj& var, size_t atom_idx, size_t parent_array_index = 0)
{
    char tmpbuf[4096];
    const Record& src = var.expr;
    const RecordAtom& parent = src.atoms[atom_idx];
    Assert(parent.type == Atom_Struct || parent.type == Atom_Array);
    Assert(parent.value.length > 0);

    if (parent.name.length != 0) {
        tsnprintf(
            tmpbuf, "%.*s##%zu", (int)parent.name.length, &src.buf[parent.name.index], atom_idx);
    } else {
        tsnprintf(tmpbuf, "[%zu]", parent_array_index);
    }

    bool close_after = false;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    bool tree_node_clicked = ImGui::TreeNode(tmpbuf);

    // recurse aggregates until first/last string positions
    size_t string_start_idx = 0;
    size_t string_end_idx = 0;
    size_t iter_idx;

    // get preview string start
    iter_idx = atom_idx;
    while (true) {
        const RecordAtom& iter = src.atoms[iter_idx];
        if (iter.type == Atom_Struct || iter.type == Atom_Array) {
            iter_idx = iter.value.index;
        } else if (iter.type == Atom_String) {
            string_start_idx = (iter.name.index != 0) ? iter.name.index : iter.value.index;
            break;
        }
    }

    // get preview string end
    iter_idx = atom_idx;
    while (iter_idx < src.atoms.size()) {
        const RecordAtom& iter = src.atoms[iter_idx];
        if (iter.type == Atom_Struct || iter.type == Atom_Array) {
            iter_idx = iter.value.index + iter.value.length - 1;
        } else if (iter.type == Atom_String) {
            string_end_idx = iter.value.index + iter.value.length;
            break;
        }
    }

    Assert(string_end_idx > string_start_idx && string_end_idx < src.buf.size());

    // set the right column value text of the aggregate tree node
    size_t preview_count = GetMin(string_end_idx - string_start_idx, 40);
    ImGui::TableNextColumn();
    ImColor preview_color
        = (var.expr_changed[atom_idx]) ? IM_COL32_WIN_RED : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::TextColored(preview_color, "%.*s", (int)preview_count, &src.buf[string_start_idx]);

    if (tree_node_clicked) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            close_after = true;

        size_t i = parent.value.index;
        size_t end = i + parent.value.length;
        size_t array_index = 0;
        for (; i < end; i++) {
            const RecordAtom& child = src.atoms[i];
            if (child.type == Atom_Struct || child.type == Atom_Array) {
                RecurseExpressionTreeNodes(var, i, array_index);
            } else {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (child.name.length > 0) {
                    ImGui::Text("%.*s", (int)child.name.length, &src.buf[child.name.index]);
                } else {
                    ImGui::Text("[%zu]", array_index);
                }

                ImColor color = (var.expr_changed[i]) ? IM_COL32_WIN_RED
                                                      : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::TableNextColumn();
                ImGui::TextColored(
                    color, "%.*s", (int)child.value.length, &src.buf[child.value.index]);
            }

            array_index++;
        }

        ImGui::TreePop();
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        String name = String(src.buf.c_str() + parent.name.index, parent.name.length);
        // TODO fix empty names RecurseSetNodeState(src, atom_idx, 1, name);
    }

    if (close_after) {
        String name = String(src.buf.c_str() + parent.name.index, parent.name.length);
        // TODO fix empty names RecurseSetNodeState(src, atom_idx, 0, name);
    }
}

Breakpoint ExtractBreakpoint(const Record& rec)
{
    Breakpoint result = {};
    String filename = gdb::extract_value("bkpt.fullname", rec);
    result.file_idx = FindOrCreateFile(filename);
    result.number = gdb::extract_int("bkpt.number", rec);
    result.addr = ParseHex(gdb::extract_value("bkpt.addr", rec));
    result.enabled = ("y" == gdb::extract_value("bkpt.enabled", rec));

    int line = gdb::extract_int("bkpt.line", rec);
    result.line_idx = (line > 0) ? (size_t)(line - 1) : BAD_INDEX;

    String what = gdb::extract_value("bkpt.what", rec);
    if (what != "") {
        result.cond = "watch " + what;
    } else {
        result.cond = gdb::extract_value("bkpt.cond", rec);
    }

    return result;
}

void QueryFrame(bool force_clear_locals)
{
    // query the prog.frame_idx for locals, callstack, globals
    Record rec;
    char tmpbuf[4096];
    gui.jump_type = Jump_Stopped;
    QueryWatchlist();

    tsnprintf(tmpbuf, "-stack-list-frames --thread %d", GetActiveThreadID());
    gdb::send_blocking(tmpbuf, rec);
    const RecordAtom* callstack = gdb::extract_atom("stack", rec);
    if (callstack) {
        String arch = "";
        static bool set_default_registers = true;
        prog.frames.clear();
        static String last_stack_sig;
        String stack_sig;

        for (const RecordAtom& level : gdb::iter_child(rec, callstack)) {
            Frame add = {};
            add.line_idx = (size_t)gdb::extract_int("line", level, rec) - 1;
            add.addr = ParseHex(gdb::extract_value("addr", level, rec));
            add.func = gdb::extract_value("func", level, rec);
            arch = gdb::extract_value("arch", level, rec);
            stack_sig += add.func;

            String fullpath = gdb::extract_value("fullname", level, rec);
            add.file_idx = FindOrCreateFile(fullpath);

            prog.frames.emplace_back(add);
        }

        prog.source_out_of_date = false;
        if (prog.frame_idx < prog.frames.size()) {
            const Frame& frame = prog.frames[prog.frame_idx];
            if (frame.file_idx < prog.files.size()) {
                prog.file_idx = frame.file_idx;
                File& file = prog.files[prog.file_idx];
                if (file.lines.size() == 0)
                    LoadFile(file);

                // check to see if the source file is newer than executable
                // same as what GDB does in source-cache.c
                struct stat source_st = {};
                struct stat exe_st = {};
                if (std::filesystem::exists(file.filename)
                    && (0 > stat(file.filename.c_str(), &source_st)
                        || 0 > stat(g_gdb.debug_filename.c_str(), &exe_st))) {
                    PrintErrorf("stat %s\n", GetErrorString(errno));
                } else {
                    time_t src = source_st.st_mtime;
                    time_t exe = exe_st.st_mtime;
                    if (src != 0 && exe != 0 && difftime(src, exe) > 0) {
                        prog.source_out_of_date = true;
                    }
                }
            }
        }

        if (prog.stack_sig != stack_sig || force_clear_locals) {
            prog.stack_sig = stack_sig;
            prog.local_vars.clear();
            if (gui.line_display != LineDisplay::Source && prog.frame_idx < prog.frames.size())
                GetFunctionDisassembly(prog.frames[prog.frame_idx]);
        }

        if (set_default_registers && arch != "") {
            set_default_registers = false;

            // set the default registers for a given architecture
            // TODO: @GDB: is there a command to query this without having to
            // get stack frames
            const char* const* registers = NULL;
            size_t num_registers = 0;
            if (arch == "i386:x86-64") {
                registers = DEFAULT_REG_AMD64;
                num_registers = ArrayCount(DEFAULT_REG_AMD64);
            } else if (arch == "i386") {
                registers = DEFAULT_REG_X86;
                num_registers = ArrayCount(DEFAULT_REG_X86);
            } else if (NULL != strstr(arch.c_str(), "arm")) {
                registers = DEFAULT_REG_ARM;
                num_registers = ArrayCount(DEFAULT_REG_ARM);
            }

            for (size_t i = 0; i < num_registers; i++) {
                // add register varobj, copied from var creation in async_stopped if statement
                // the only difference is GLOBAL_NAME_PREFIX and '@' used to signify a varobj
                // that lasts the duration of the program

                String str = StringPrintf(
                    "-var-create " GLOBAL_NAME_PREFIX "%s @ $%s", registers[i], registers[i]);
                if (gdb::send_blocking(str.c_str(), rec)) {
                    VarObj add = CreateVarObj(registers[i], gdb::extract_value("value", rec));
                    prog.global_vars.emplace_back(add);
                }
            }
        }
    }

    // get local variables for this stack frame
    // prog.local_vars not actually GDB variable objects,
    // problems with aggregates displaying updates

    tsnprintf(tmpbuf, "-stack-list-variables --frame %zu --thread %d --all-values", prog.frame_idx,
        GetActiveThreadID());
    gdb::send_blocking(tmpbuf, rec);
    for (VarObj& local : prog.local_vars)
        local.changed = false;

    const RecordAtom* vars = gdb::extract_atom("variables", rec);
    size_t start_locals_length = prog.local_vars.size();
    std::vector<bool> var_found(start_locals_length);

    for (const RecordAtom& child : gdb::iter_child(rec, vars)) {
        VarObj incoming = CreateVarObj(
            gdb::extract_value("name", child, rec), gdb::extract_value("value", child, rec));

        bool found = false;
        for (size_t i = start_locals_length - 1; i < start_locals_length; i--) {
            VarObj& local = prog.local_vars[i];
            if (local.name == incoming.name) {
                // @Hack: clean this up
                CheckIfChanged(incoming, local);
                local.value = incoming.value;
                local.expr = incoming.expr;
                local.expr_changed = incoming.expr_changed;
                local.changed = incoming.changed;
                found = true;
                var_found[i] = true;
                break;
            }
        }

        if (!found)
            prog.local_vars.emplace_back(incoming);
    }

    // remove any locals that went out of scope
    for (size_t i = var_found.size() - 1; i < var_found.size(); i--) {
        if (!var_found[i])
            prog.local_vars.erase(prog.local_vars.begin() + i, prog.local_vars.begin() + i + 1);
    }

    // update global values, just registers right now
    gdb::send_blocking("-var-update --all-values *", rec);
    const RecordAtom* changelist = gdb::extract_atom("changelist", rec);
    for (VarObj& global : prog.global_vars)
        global.changed = false;

    for (const RecordAtom& iter : gdb::iter_child(rec, changelist)) {
        VarObj incoming = CreateVarObj(
            gdb::extract_value("name", iter, rec), gdb::extract_value("value", iter, rec));

        const char* srcname = incoming.name.c_str();
        const char* namestart = strstr(srcname, GLOBAL_NAME_PREFIX);
        if (srcname == namestart) {
            // check for global variable change
            namestart += strlen(GLOBAL_NAME_PREFIX);
            for (VarObj& global : prog.global_vars) {
                if (global.name == namestart) {
                    CheckIfChanged(incoming, global);
                    global.value = incoming.value;
                    global.changed = incoming.changed;
                    global.expr_changed = incoming.expr_changed;
                    break;
                }
            }
        }
    }
}

bool IsValidLine(size_t line_idx, size_t file_idx)
{
    bool result = false;
    if (file_idx < prog.files.size()) {
        if (line_idx < prog.files[file_idx].lines.size()) {
            result = true;
        }
    }

    return result;
}

String GetLine(const File& f, size_t line_idx)
{
    String result;
    if (line_idx < f.lines.size()) {
        size_t num_chars = 0;
        if (line_idx == (f.lines.size() - 1)) {
            num_chars = f.data.size() - f.lines[line_idx];
        } else {
            num_chars = f.lines[line_idx + 1] - f.lines[line_idx];
        }

        result.assign(f.data.data() + f.lines[line_idx], num_chars);
    }

    return result;
}

void Draw()
{
    Record rec;
    char tmpbuf[4096];
    const ImVec2 MIN_WINSIZE = ImVec2(350.0f, 250.0f);
    const ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInner;

    // check for new blocks
    int recv_block_semvalue = 0;
    sem_getvalue(g_gdb.recv_block, &recv_block_semvalue);
    if (recv_block_semvalue > 0) {
        gdb::grab_block_data();
        sem_wait(g_gdb.recv_block);
    }

    // process and clear all records found
    size_t last_num_recs = prog.num_recs;
    for (size_t i = 0; i < last_num_recs; i++) {
        RecordHolder& iter = prog.read_recs[i];
        if (!iter.parsed) {
            iter.parsed = true;
            Record& parse_rec = iter.rec;
            const char* src = parse_rec.buf.c_str();

            const char* comma = (char*)memchr(src, ',', parse_rec.buf.size());
            if (comma == NULL)
                comma = &src[parse_rec.buf.size()];

            // skip MI prefix char
            String record_action;
            char prefix = '\0';
            if (parse_rec.buf.size() > 0) {
                prefix = parse_rec.buf[0];
                record_action = gdb::get_record_action(parse_rec);
            }

            if (prefix == PREFIX_ASYNC0) {
                if (record_action == "breakpoint-created") {
                    // breakpoints created from console ex: "b main.cpp:14"
                    prog.breakpoints.push_back(ExtractBreakpoint(parse_rec));
                } else if (record_action == "breakpoint-modified") {
                    Breakpoint b = ExtractBreakpoint(parse_rec);
                    for (Breakpoint& bkpt : prog.breakpoints) {
                        if (bkpt.number == b.number) {
                            bkpt = b;
                            break;
                        }
                    }
                } else if (record_action == "breakpoint-deleted") {
                    // breakpoints deleted from console ex: "d 1"
                    size_t id = (size_t)gdb::extract_int("id", parse_rec);
                    auto& bpts = prog.breakpoints;
                    for (size_t b = 0; b < bpts.size(); b++) {
                        if (bpts[b].number == id) {
                            bpts.erase(bpts.begin() + b, bpts.begin() + b + 1);
                            break;
                        }
                    }
                } else if (record_action == "thread-group-started") {
                    prog.inferior_process = (pid_t)gdb::extract_int("pid", parse_rec);
                } else if (record_action == "thread-group-exited") {
                    String group_id = gdb::extract_value("id", parse_rec);
                    for (size_t end = prog.threads.size(); end > 0; end--) {
                        size_t t = end - 1;
                        if (prog.threads[t].group_id == group_id) {
                            prog.threads.erase(
                                prog.threads.begin() + t, prog.threads.begin() + t + 1);
                            break;
                        }
                    }
                } else if (record_action == "thread-selected") {
                    int tid = gdb::extract_int("id", parse_rec);
                    for (size_t t = 0; t < prog.threads.size(); t++)
                        if (prog.threads[t].id == tid)
                            prog.thread_idx = t;

                    // user jumped to a new thread/frame from the console window
                    if (!prog.running) {
                        size_t index = (size_t)gdb::extract_int("frame.level", parse_rec);
                        if (index < prog.frames.size()) {
                            prog.frame_idx = index;
                            QueryFrame(true);
                        }
                    }
                } else if (record_action == "thread-created") {
                    Thread t = {};
                    t.id = gdb::extract_int("id", parse_rec);
                    t.group_id = gdb::extract_value("group-id", parse_rec);
                    t.focused = true;

                    if (t.id != 0 && t.group_id != "")
                        prog.threads.push_back(t);
                } else if (record_action == "thread-exited") {
                    int id = gdb::extract_int("id", parse_rec);
                    String group_id = gdb::extract_value("group-id", parse_rec);
                    for (size_t t = 0; t < prog.threads.size(); t++) {
                        if (prog.threads[t].id == id && prog.threads[t].group_id == group_id) {
                            prog.threads.erase(
                                prog.threads.begin() + t, prog.threads.begin() + t + 1);
                            break;
                        }
                    }
                }
            } else if (record_action == "running") {
                prog.running = true;
                String thread = gdb::extract_value("thread-id", parse_rec);
                if (thread == "all") {
                    for (Thread& t : prog.threads)
                        t.running = true;
                } else {
                    int tid = atoi(thread.c_str());
                    for (Thread& t : prog.threads)
                        if (t.id == tid)
                            t.running = true;
                }
            } else if (record_action == "stopped") {
                // jump to the stopped thread if the current index is running
                bool jump_to_thread = true;
                if (prog.thread_idx < prog.threads.size()) {
                    bool no_lines_shown = false;
                    if (prog.frame_idx < prog.frames.size()) {
                        size_t idx = prog.frames[prog.frame_idx].file_idx;
                        if (idx < prog.files.size() && prog.files[idx].lines.size() == 0)
                            no_lines_shown = true;
                    }

                    if (!prog.threads[prog.thread_idx].running && !no_lines_shown)
                        jump_to_thread = false;
                }

                prog.running = false;
                String reason = gdb::extract_value("reason", parse_rec);
                int tid = gdb::extract_int("thread-id", parse_rec);

                // wonky: sometimes it's stopped-threads="all", and sometimes it's
                // stopped-threads=["all"]
                bool stopped_all = false;
                const RecordAtom* stopped_threads = gdb::extract_atom("stopped-threads", parse_rec);
                if (stopped_threads != NULL) {
                    if (stopped_threads->type == Atom_String) {
                        stopped_all = ("all" == gdb::get_atom_string(stopped_threads->value, parse_rec));
                    } else if (stopped_threads->type == Atom_Array) {
                        for (const RecordAtom& stopped : gdb::iter_child(rec, stopped_threads))
                            stopped_all |= ("all" == gdb::get_atom_string(stopped.value, rec));
                    }
                }

                for (size_t t = 0; t < prog.threads.size(); t++) {
                    if (prog.threads[t].id == tid || stopped_all)
                        prog.threads[t].running = false;

                    if (prog.threads[t].id == tid && jump_to_thread) {
                        prog.thread_idx = t;
                        prog.frame_idx = 0;
                    }
                }

                if ((NULL != strstr(reason.c_str(), "exited"))) {
                    ResetProgramState();
                } else {
                    prog.started = true;
                    if (jump_to_thread)
                        QueryFrame(false);
                }
            }
        }
    }

    // reset recs only when no more have been added
    if (last_num_recs == prog.num_recs)
        prog.num_recs = 0;

    if (ImGui::BeginMainMenuBar()) {
        struct RegisterName {
            String text;
            bool registered;
        };

        static bool show_open_file = false;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...")) {
                show_open_file = true;
            }
            ImGui::EndMenu();
        }

        if (show_open_file) {
            static FileWindowContext ctx;
            if (ImGuiFileWindow(ctx, ImGuiFileWindowMode_SelectFile)) {
                if (ctx.selected) {
                    // always reload the file on clicking open
                    size_t idx = FindOrCreateFile(ctx.path.c_str());
                    prog.files[idx].lines.clear();
                    if (LoadFile(prog.files[idx])) {
                        prog.file_idx = idx;
                        gui.jump_type = Jump_Goto;
                        gui.goto_line_idx = 0;
                    }
                }

                show_open_file = false;
            }
        }

        static std::vector<RegisterName> all_registers;
        static bool show_register_window = false;
        static bool is_debug_program_open = false;

        if (gui.drag_drop_exe_path != "") {
            is_debug_program_open = false; // assign input boxes
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            ImGui::OpenPopup(window->GetID("Debug"));
            ImGui::SetActiveID(window->GetID(""), window);
            GImGui->ActiveIdMouseButton = ImGuiMouseButton_Left; // avoid hitting assertion
        }

        if (ImGui::BeginMenu("Debug")) {
            static FileWindowContext ctx;
            static char gdb_filename[PATH_MAX];
            static char gdb_args[1024];
            static bool pick_gdb_file = false;
            static char debug_filename[PATH_MAX];
            static char debug_args[1024];
            static bool pick_debug_file = false;

            if (!is_debug_program_open) {
                is_debug_program_open = true;
                if (gui.drag_drop_exe_path != "") {
                    tsnprintf(debug_filename, "%s", gui.drag_drop_exe_path.c_str());
                    gui.drag_drop_exe_path = "";
                } else {
                    tsnprintf(debug_filename, "%s", g_gdb.debug_filename.c_str());
                }
                tsnprintf(debug_args, "%s", g_gdb.debug_args.c_str());
                tsnprintf(gdb_filename, "%s", g_gdb.filename.c_str());
                tsnprintf(gdb_args, "%s", g_gdb.args.c_str());
            }

            ImGui::InputText("GDB filename", gdb_filename, sizeof(gdb_filename));
            ImGui::SameLine();
            if (ImGui::Button("...##gdb_filename"))
                pick_gdb_file = true;
            ImGui::InputText("GDB arguments", gdb_args, sizeof(gdb_args));

            ImGui::InputText("debug filename", debug_filename, sizeof(debug_filename));
            ImGui::SameLine();
            if (ImGui::Button("...##debug_filename"))
                pick_debug_file = true;
            ImGui::InputText("debug arguments", debug_args, sizeof(debug_args));

            if ((pick_gdb_file || pick_debug_file)
                && ImGuiFileWindow(ctx, ImGuiFileWindowMode_SelectFile)) {
                if (ctx.selected) {
                    if (pick_gdb_file)
                        tsnprintf(gdb_filename, "%s", ctx.path.c_str());
                    if (pick_debug_file)
                        tsnprintf(debug_filename, "%s", ctx.path.c_str());
                }

                pick_debug_file = false;
                pick_gdb_file = false;
            }

            if (ImGui::BeginCombo("debug history", "")) {
                for (Session& iter : gui.session_history) {
                    String str
                        = StringPrintf("%s %s", iter.debug_exe.c_str(), iter.debug_args.c_str());
                    if (ImGui::Selectable(str.c_str())) {
                        tsnprintf(debug_filename, "%s", iter.debug_exe.c_str());
                        tsnprintf(debug_args, "%s", iter.debug_args.c_str());
                    }
                }

                ImGui::EndCombo();
            }

            bool started = false;
            ImGuiDisabled(prog.started, started = ImGui::Button("Start##Debug Program Menu"));
            if (started) {
                if (g_gdb.filename != gdb_filename) {
                    if (g_gdb.spawned_pid != 0) {
                        Printf("ending %s...", g_gdb.filename.c_str());
                        g_gdb.filename = "";
                        os::kill_process(g_gdb.spawned_pid);
                        ResetProgramState();
                        g_gdb.spawned_pid = 0;
                    }

                    gdb::start_process(gdb_filename, gdb_args);
                }

                if (g_gdb.spawned_pid != 0 && gdb::set_inferior_exe(debug_filename)
                    && gdb::set_inferior_args(debug_args)) {
                    if (g_gdb.capabilities & GDB_RUNSTART)
                        gdb::send_blocking("-exec-run --start");

                    char* exe_abspath = realpath(debug_filename, NULL);
                    if (exe_abspath) {
                        Session s = {};
                        s.debug_exe = exe_abspath;
                        s.debug_args = debug_args;
                        free(exe_abspath);
                        exe_abspath = NULL;

                        // add session entry if it's different from newest
                        bool has_session_changed = false;
                        if (gui.session_history.size() == 0) {
                            has_session_changed = true;
                        } else {
                            Session& newest = gui.session_history[0];
                            if (newest.debug_exe != s.debug_exe
                                || newest.debug_args != s.debug_args) {
                                has_session_changed = true;
                            }
                        }

                        if (has_session_changed) {
                            gui.session_history.insert(gui.session_history.begin(), s);
                        }
                    }

                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::EndMenu();
        } else {
            is_debug_program_open = false;
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Source##Checkbox", "", &gui.show_source);
            ImGui::MenuItem("Control##Checkbox", "", &gui.show_control);
            ImGui::MenuItem("Callstack##Checkbox", "", &gui.show_callstack);
            ImGui::MenuItem("Registers##Checkbox", "", &gui.show_registers);
            ImGui::MenuItem("Locals##Checkbox", "", &gui.show_locals);
            ImGui::MenuItem("Watch##Checkbox", "", &gui.show_watch);
            ImGui::MenuItem("Breakpoints##Checkbox", "", &gui.show_breakpoints);
            ImGui::MenuItem("Threads##Checkbox", "", &gui.show_threads);
            ImGui::MenuItem("Directory Viewer##Checkbox", "", &gui.show_directory_viewer);

            ImGui::EndMenu();
        }

        // modify register window: query GDB for the list of register
        // names then let the user pick which ones to use
        if (show_register_window) {
            ImGui::SetNextWindowSize({ 400, 400 });
            ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
            ImGui::Begin("Configure Registers##Window", &show_register_window);

            for (RegisterName& reg : all_registers) {
                if (ImGui::Checkbox(reg.text.c_str(), &reg.registered)) {
                    if (reg.registered) {
                        // add register varobj, copied from var creation in async_stopped if
                        // statement the only difference is GLOBAL_NAME_PREFIX and '@' used to
                        // signify a varobj that lasts the duration of the program

                        tsnprintf(tmpbuf, "-var-create " GLOBAL_NAME_PREFIX "%s @ $%s",
                            reg.text.c_str(), reg.text.c_str());
                        gdb::send_blocking(tmpbuf, rec);

                        VarObj add = CreateVarObj(reg.text, gdb::extract_value("value", rec));
                        prog.global_vars.emplace_back(add);
                    } else {
                        // delete register varobj
                        for (size_t i = 0; i < prog.global_vars.size(); i++) {
                            if (prog.global_vars[i].name == reg.text) {
                                tsnprintf(tmpbuf, "-var-delete " GLOBAL_NAME_PREFIX "%s",
                                    reg.text.c_str());
                                if (gdb::send_blocking(tmpbuf)) {
                                    prog.global_vars.erase(prog.global_vars.begin() + i,
                                        prog.global_vars.begin() + i + 1);
                                }
                            }
                        }
                    }
                }
            }

            ImGui::End();
        }

        ImGui::EndMainMenuBar();
    }

    if (gui.show_source) {
        float saved_frame_border_size = ImGui::GetStyle().FrameBorderSize;
        ImGui::GetStyle().FrameBorderSize = 0.0f; // disable line border around breakpoints
        ImGui::PushFont(gui.source_font);
        ImGui::SetNextWindowBgAlpha(
            1.0); // @Imgui: bug where GetStyleColor doesn't respect window opacity
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Source", &gui.show_source, ImGuiWindowFlags_HorizontalScrollbar);

        if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl
            && ImGui::GetIO().MouseWheel != 0.0f) {
            // increase/decrease the font
            float tmp = GetPinned(gui.source_font_size + ImGui::GetIO().MouseWheel, 8.0f, 72.0f);
            if (gui.source_font_size != tmp) {
                gui.source_font_size = tmp;
            }
        }

        if (IsKeyPressed(ImGuiKey_F, ImGuiKeyModFlags_Ctrl)) {
            gui.source_search_bar_open = true;
            ImGui::SetKeyboardFocusHere(0); // auto click the input box
            gui.source_search_keyword[0] = '\0';
        } else if (gui.source_search_bar_open && IsKeyPressed(ImGuiKey_Escape)) {
            // jump to the found line permanently, if found
            gui.source_search_bar_open = false;

            if (gui.source_found_line) {
                gui.jump_type = Jump_Goto;
                gui.goto_line_idx = gui.source_found_line_idx;
            }
        }

        //
        // goto window: jump to a line in the source document
        //
        {
            static bool goto_line_open = false;
            bool goto_line_activate = false;
            int goto_line_idx = 0;

            if (IsKeyPressed(ImGuiKey_G, ImGuiKeyModFlags_Ctrl)) {
                goto_line_open = true;
                goto_line_activate = true;
            }

            if (goto_line_open && prog.file_idx < prog.files.size()) {
                ImGui::SetNextWindowSize(ImVec2(150.0f, 100.0f), ImGuiCond_Once);
                ImGui::Begin("Goto Line", &goto_line_open);
                if (goto_line_activate) {
                    ImGui::SetKeyboardFocusHere(0); // auto click the goto line field
                    goto_line_activate = false;
                }

                if (IsKeyPressed(ImGuiKey_Escape))
                    goto_line_open = false;

                if (ImGui::InputInt("##goto_line", &goto_line_idx, 1, 1,
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                    size_t linecount = prog.files[prog.file_idx].lines.size();
                    if (goto_line_idx < 0)
                        goto_line_idx = 0;
                    if ((size_t)goto_line_idx >= linecount)
                        goto_line_idx = (linecount > 0) ? linecount - 1 : 0;
                    goto_line_open = false;
                    gui.jump_type = Jump_Goto;
                    gui.goto_line_idx = (size_t)goto_line_idx;
                }
                ImGui::End();
            }
        }

        //
        // search bar: look for text in source window
        //

        if (gui.source_search_bar_open) {
            ImGui::InputText(
                "##source_search", gui.source_search_keyword, sizeof(gui.source_search_keyword));
            if (prog.file_idx < prog.files.size()) {
                size_t dir = 1;
                const File& this_file = prog.files[prog.file_idx];

                if (IsKeyPressed(ImGuiKey_N) && !ImGui::GetIO().WantCaptureKeyboard) {
                    // N = search forward
                    // Shift N = search backward
                    dir = (ImGui::GetIO().KeyShift) ? -1 : 1;

                    // advance search by skipping the previous match
                    gui.source_found_line_idx += dir;
                    size_t linesize = this_file.lines.size();
                    if (gui.source_found_line_idx > linesize) // wrap around
                        gui.source_found_line_idx = linesize - 1;
                }

                // search for a keyword starting at the last found index + 1
                bool wraparound = false;
                gui.source_found_line = false;
                for (size_t i = gui.source_found_line_idx; i < this_file.lines.size(); i += dir) {
                    const String& line = GetLine(this_file, i);
                    if (NULL != strstr(line.c_str(), gui.source_search_keyword)) {
                        gui.source_found_line = true;
                        gui.jump_type = Jump_Search;
                        gui.source_found_line_idx = i;
                        break;
                    }

                    if (!wraparound && i + dir >= this_file.lines.size()) {
                        // continue searching at the other end of the array
                        i = (dir == 1) ? 0 : this_file.lines.size() - 1;
                        wraparound = true;
                    }
                }
                if (!gui.source_found_line)
                    gui.source_found_line_idx = 0;
            }

            ImGui::Separator();
            ImGui::BeginChild("SourceScroll");
        }

        if (prog.file_idx < prog.files.size() && prog.files[prog.file_idx].lines.size() > 0) {
            const File& file = prog.files[prog.file_idx];

            // draw radio button and line offscreen to get the line size
            // TODO: how to do this without drawing offscreen
            float start_curpos_y = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(-100);
            float ystartoffscreen = ImGui::GetCursorPosY();
            ImGui::RadioButton("##MeasureRadioHeight", false);
            ImGui::SameLine();
            ImGui::Text("MeasureText");
            float lineheight = ImGui::GetCursorPosY() - ystartoffscreen;
            size_t perscreen = ceilf(ImGui::GetWindowHeight() / lineheight) + 1;

            // set the max horizontal scrollbar size
            String longest = GetLine(file, file.longest_line_idx);
            float start_curpos_x = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(start_curpos_x
                + ImGui::CalcTextSize(longest.data(), longest.data() + longest.size()).x);
            ImGui::Text("foobarbaz");

            // reset the draw position
            ImGui::SetCursorPosY(start_curpos_y);
            ImGui::SetCursorPosX(start_curpos_x);

            // @Optimization: only draw the visible lines then SetCursorPosY to
            // be lineheight * height per line to set the total scroll
            if (gui.line_display == LineDisplay::Source) {
                bool in_active_frame_file = prog.frame_idx < prog.frames.size()
                    && prog.frames[prog.frame_idx].file_idx == prog.file_idx;

                // automatically scroll to the next executed line if it is far enough away
                // and we've just stopped execution
                size_t start_idx = (ImGui::GetScrollY() / lineheight);
                if (gui.jump_type != Jump_None) {
                    size_t middle_idx = BAD_INDEX;
                    switch (gui.jump_type) {
                    case Jump_Stopped: {
                        if (in_active_frame_file) {
                            const Frame& frame = prog.frames[prog.frame_idx];
                            bool is_next_exec_visible = frame.line_idx >= start_idx + 5
                                && frame.line_idx <= start_idx + perscreen - 5;
                            if (!is_next_exec_visible)
                                middle_idx = frame.line_idx;
                        }
                    } break;

                    case Jump_Goto: {
                        middle_idx = gui.goto_line_idx;
                    } break;

                    case Jump_Search: {
                        bool is_found_visible = gui.source_found_line_idx >= start_idx
                            && gui.source_found_line_idx < start_idx + perscreen;

                        if (!is_found_visible)
                            middle_idx = gui.source_found_line_idx;
                    } break;

                        DefaultInvalid
                    }

                    if (middle_idx < file.lines.size()) {
                        size_t s = middle_idx - (perscreen / 2);
                        if (s >= file.lines.size())
                            s = 0;
                        start_idx = s;
                        ImGui::SetScrollY(start_curpos_y + start_idx * lineheight);
                    }

                    gui.jump_type = Jump_None;
                }

                size_t end_idx = GetMin(start_idx + perscreen, file.lines.size());
                if (file.lines.size() > perscreen) {
                    // set scrollbar height
                    ImGui::SetCursorPosY(start_curpos_y + file.lines.size() * lineheight);
                }
                ImGui::SetCursorPosY(start_idx * lineheight + start_curpos_y);

                for (size_t line_idx = start_idx; line_idx < end_idx; line_idx++) {
                    const String& line = GetLine(file, line_idx);

                    bool is_breakpoint_on_line = false;
                    bool is_breakpoint_disabled = false;
                    for (Breakpoint& iter : prog.breakpoints) {
                        if (iter.line_idx == line_idx && iter.file_idx == prog.file_idx) {
                            is_breakpoint_on_line = true;
                            is_breakpoint_disabled |= !iter.enabled;
                        }
                    }

                    // start radio button style
                    ImColor window_bg_color = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                    ImColor bkpt_active_color = IM_COL32(255, 64, 64, 255);
                    ImColor bkpt_hovered_color = {};

                    const float inc = 32 / 255.0f;
                    bkpt_hovered_color.Value.x = window_bg_color.Value.x + inc;
                    bkpt_hovered_color.Value.y = window_bg_color.Value.y + inc;
                    bkpt_hovered_color.Value.z = window_bg_color.Value.z + inc;
                    bkpt_hovered_color.Value.w = window_bg_color.Value.w;

                    if (is_breakpoint_on_line && is_breakpoint_disabled) {
                        // disabled breakpoint on this line, make breakpoint color lighter
                        window_bg_color.Value.w = 0.3f;
                        bkpt_active_color.Value.w = 0.3f;
                        bkpt_hovered_color.Value.w = 0.3f;
                    }

                    ImGui::PushStyleColor(ImGuiCol_FrameBg, window_bg_color.Value);
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, bkpt_hovered_color.Value);

                    // color while pressing mouse left on button
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, bkpt_active_color.Value);

                    // color while active
                    ImGui::PushStyleColor(ImGuiCol_CheckMark, bkpt_active_color.Value);

                    tsnprintf(tmpbuf, "##bkpt%zu", line_idx);
                    if (ImGui::RadioButton(tmpbuf, is_breakpoint_on_line)) {
                        if (is_breakpoint_on_line) {
                            for (size_t b = 0; b < prog.breakpoints.size(); b++) {
                                Breakpoint& iter = prog.breakpoints[b];
                                if (iter.line_idx == line_idx && iter.file_idx == prog.file_idx) {
                                    if (!iter.enabled) {
                                        // change breakpoint state from disabled to enabled
                                        tsnprintf(tmpbuf, "-break-enable %zu", iter.number);
                                        if (gdb::send_blocking(tmpbuf))
                                            iter.enabled = true;
                                    } else {
                                        // remove breakpoint
                                        tsnprintf(tmpbuf, "-break-delete %zu", iter.number);
                                        if (gdb::send_blocking(tmpbuf)) {
                                            prog.breakpoints.erase(prog.breakpoints.begin() + b,
                                                prog.breakpoints.begin() + b + 1);
                                        }
                                    }

                                    break;
                                }
                            }
                        } else {
                            // create breakpoint
                            tsnprintf(tmpbuf, "-break-insert \"%s:%d\"", file.filename.c_str(),
                                (int)(line_idx + 1));
                            if (gdb::send_blocking(tmpbuf, rec)) {
                                Breakpoint bkpt = ExtractBreakpoint(rec);
                                prog.breakpoints.push_back(bkpt);
                                const char* filename = prog.files[bkpt.file_idx].filename.c_str();
                                const char* last_fwd = strrchr(filename, '/');
                                if (last_fwd != NULL)
                                    filename = last_fwd + 1;

                                // add console message matching what GDB would send
                                // Breakpoint 2 at 0x555555555185: file debug.c, line 13.
                                Printf("Breakpoint %d at 0x%" PRIx64 ": file %s, line %d",
                                    (int)bkpt.number, bkpt.addr, filename,
                                    (int)(bkpt.line_idx + 1));
                            }
                        }
                    }

                    // stop radio button style
                    ImGui::PopStyleColor(4);

                    ImGui::SameLine();
                    int line_written = tsnprintf(tmpbuf, "%-4zu %s", line_idx + 1, line.c_str());
                    if (line_written >= (int)sizeof(tmpbuf))
                        line_written = sizeof(tmpbuf) - 1;

                    ImVec2 textstart = ImGui::GetCursorPos();
                    textstart.x += ImGui::CalcTextSize(tmpbuf, tmpbuf + line_written - line.size())
                                       .x; // skip line number for hover eval

                    if (in_active_frame_file && line_idx == prog.frames[prog.frame_idx].line_idx) {
                        // prevent any "##" text from being hidden
                        if (line_written < (int)sizeof(tmpbuf))
                            snprintf(tmpbuf + line_written, sizeof(tmpbuf) - line_written, "##%zu",
                                line_idx);
                        ImGui::Selectable(tmpbuf, !prog.running);
                    } else {
                        if (gui.source_search_bar_open && line_idx == gui.source_found_line_idx) {
                            ImGui::TextColored(ImColor(1.0f, 1.0f, 0.0f, 1.0f).Value, "%s", tmpbuf);
                        } else {
                            ImGui::Text("%s", tmpbuf);
                        }
                    }

                    if (ImGui::IsItemHovered()) {
                        // convert absolute mouse to window relative position
                        ImVec2 relpos = {};
                        relpos.x = ImGui::GetMousePos().x - ImGui::GetWindowPos().x
                            + ImGui::GetScrollX();
                        relpos.y = ImGui::GetMousePos().y - ImGui::GetWindowPos().y;

                        // enumerate words of the line
                        size_t word_idx = BAD_INDEX;
                        size_t delim_idx = BAD_INDEX;
                        for (size_t char_idx = 0; char_idx < line.size(); char_idx++) {
                            char c = line[char_idx];
                            bool is_ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                                || (word_idx != BAD_INDEX && (c >= '0' && c <= '9')) || (c == '_');

                            if (char_idx == line.size() - 1 && is_ident) {
                                // force a word on the last index
                                if (word_idx == BAD_INDEX)
                                    word_idx = char_idx;

                                char_idx++;
                                is_ident = false;
                            }

                            if (!is_ident) {
                                bool not_delim_struct = true;
                                if (word_idx != BAD_INDEX) {
                                    // we got a word delimited by spaces
                                    // calculate size and see if mouse is over it
                                    ImVec2 worddim = ImGui::CalcTextSize(
                                        line.data() + word_idx, line.data() + char_idx);
                                    if (relpos.x >= textstart.x
                                        && relpos.x <= textstart.x + worddim.x && prog.started) {
                                        // query a word if we moved words / callstack frames
                                        // TODO: register hover needs '$' in front of name for asm
                                        // debugging
                                        static size_t hover_line_idx;
                                        static size_t hover_word_idx;
                                        static size_t hover_char_idx;
                                        static size_t hover_num_frames;
                                        static size_t hover_frame_idx;
                                        static double hover_time;
                                        static String hover_value;
                                        static bool hover_value_evaluated;

                                        // check to see if we should add the variable
                                        // to the watch variables
                                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                                            String hover_string(
                                                line.data() + word_idx, char_idx - word_idx);
                                            VarObj add = CreateVarObj(hover_string);
                                            prog.watch_vars.push_back(add);
                                            QueryWatchlist();
                                        }

                                        if (hover_word_idx != word_idx || hover_char_idx != char_idx
                                            || hover_line_idx != line_idx
                                            || hover_num_frames != prog.frames.size()
                                            || hover_frame_idx != prog.frame_idx) {
                                            hover_word_idx = word_idx;
                                            hover_char_idx = char_idx;
                                            hover_line_idx = line_idx;
                                            hover_num_frames = prog.frames.size();
                                            hover_frame_idx = prog.frame_idx;
                                            hover_time = ImGui::GetTime();
                                            hover_value_evaluated = false;
                                            hover_value = "";
                                        }

                                        if (!hover_value_evaluated) {
                                            if (ImGui::GetTime() - hover_time
                                                > (gui.hover_delay_ms / 1000.0)) {
                                                hover_value_evaluated = true;
                                                String word(
                                                    line.data() + word_idx, char_idx - word_idx);
                                                tsnprintf(tmpbuf,
                                                    "-data-evaluate-expression --frame %zu "
                                                    "--thread %d \"%s\"",
                                                    prog.frame_idx, GetActiveThreadID(),
                                                    word.c_str());
                                                if (gdb::send_blocking(tmpbuf, rec)) {
                                                    hover_value = gdb::extract_value("value", rec);
                                                }
                                            }
                                        } else {
                                            ImGui::PushFont(gui.default_font);
                                            ImGui::BeginTooltip();
                                            ImGui::Text("%s", hover_value.c_str());
                                            ImGui::EndTooltip();
                                            ImGui::PopFont();
                                        }

                                        break;
                                    }

                                    char n = '\0';
                                    if (char_idx + 1 < line.size())
                                        n = line[char_idx + 1];

                                    // C/C++: skip over struct syntax chars to evaluate their
                                    // members
                                    if (c == '.') {
                                        char_idx += 1;
                                        not_delim_struct = false;
                                    } else if (c == '-' && n == '>') {
                                        char_idx += 2;
                                        not_delim_struct = false;
                                    } else {
                                        textstart.x += worddim.x;
                                    }
                                }

                                if (not_delim_struct) {
                                    word_idx = BAD_INDEX;
                                    if (delim_idx == BAD_INDEX) {
                                        delim_idx = char_idx;
                                    }
                                }
                            } else if (word_idx == BAD_INDEX) {
                                if (delim_idx != BAD_INDEX) {
                                    // advance non-ident text width
                                    ImVec2 dim = ImGui::CalcTextSize(
                                        line.data() + delim_idx, line.data() + char_idx);
                                    textstart.x += dim.x;
                                }
                                word_idx = char_idx;
                                delim_idx = BAD_INDEX;
                            }
                        }
                    }
                }
            } else if ((gui.line_display == LineDisplay::Disassembly
                           || gui.line_display == LineDisplay::Source_And_Disassembly)
                && prog.frame_idx < prog.frames.size()
                && prog.frames[prog.frame_idx].file_idx == prog.file_idx) {
                const Frame& frame = prog.frames[prog.frame_idx];
                // automatically scroll to the next executed line if it is far enough away
                // and we've just stopped execution
                if (gui.jump_type == Jump_Stopped)
                    gui.jump_type = Jump_None;

                size_t start_idx = (ImGui::GetScrollY() / lineheight);
                size_t end_idx = GetMin(start_idx + perscreen, gui.line_disasm.size());
                if (gui.line_disasm.size() > perscreen) {
                    // set the proper scroll size by setting the cursor position to the last line
                    ImGui::SetCursorPosY(start_curpos_y + gui.line_disasm.size() * lineheight);
                }

                ImGui::SetCursorPosY(start_idx * lineheight + start_curpos_y);

                // display source window using retrieved disassembly
                size_t src_idx = 0;
                size_t inst_left = 0;
                for (size_t i = start_idx; i < end_idx; i++) {
                    const DisassemblyLine& line = gui.line_disasm[i];

                    if (gui.line_display == LineDisplay::Source_And_Disassembly) {
                        // display source line then all of its instructions below
                        if (inst_left == 0) {
                            while (src_idx < gui.line_disasm_source.size()) {
                                size_t lidx = gui.line_disasm_source[src_idx].line_idx;
                                inst_left = gui.line_disasm_source[src_idx].num_instructions;
                                if (lidx < file.lines.size()) {
                                    String s = GetLine(file, lidx);
                                    ImGui::Text("%s", s.c_str());
                                }

                                src_idx++;
                                if (inst_left != 0)
                                    break;
                            }
                        }

                        inst_left--;
                    }

                    bool is_breakpoint_set = false;
                    for (Breakpoint& bkpt : prog.breakpoints) {
                        if (bkpt.addr == line.addr && bkpt.file_idx == frame.file_idx)
                            is_breakpoint_set = true;
                    }

                    // start radio button style
                    ImColor window_bg_color = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                    ImColor bkpt_active_color = IM_COL32(255, 64, 64, 255);
                    ImColor bkpt_hovered_color = {};

                    const float inc = 32 / 255.0f;
                    bkpt_hovered_color.Value.x = window_bg_color.Value.x + inc;
                    bkpt_hovered_color.Value.y = window_bg_color.Value.y + inc;
                    bkpt_hovered_color.Value.z = window_bg_color.Value.z + inc;
                    bkpt_hovered_color.Value.w = window_bg_color.Value.w;

                    ImGui::PushStyleColor(ImGuiCol_FrameBg, window_bg_color.Value);
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, bkpt_hovered_color.Value);

                    // color while pressing mouse left on button
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, bkpt_active_color.Value);

                    // color while active
                    ImGui::PushStyleColor(ImGuiCol_CheckMark, bkpt_active_color.Value);

                    tsnprintf(tmpbuf, "##bkpt%d", (int)i);
                    if (ImGui::RadioButton(tmpbuf, is_breakpoint_set)) {
                        // dispatch command to set breakpoint
                        if (is_breakpoint_set) {
                            for (size_t b = 0; b < prog.breakpoints.size(); b++) {
                                Breakpoint& bkpt = prog.breakpoints[b];
                                if (bkpt.addr == line.addr && bkpt.file_idx == frame.file_idx) {
                                    // remove breakpoint
                                    tsnprintf(tmpbuf, "-break-delete %zu", bkpt.number);
                                    if (gdb::send_blocking(tmpbuf)) {
                                        prog.breakpoints.erase(prog.breakpoints.begin() + b,
                                            prog.breakpoints.begin() + b + 1);
                                    }
                                    break;
                                }
                            }
                        } else {
                            // insert breakpoint
                            tsnprintf(tmpbuf, "-break-insert *0x%" PRIx64, line.addr);
                            if (gdb::send_blocking(tmpbuf, rec))
                                prog.breakpoints.push_back(ExtractBreakpoint(rec));
                        }
                    }

                    // stop radio button style
                    ImGui::PopStyleColor(4);

                    ImGui::SameLine();
                    if (line.addr == frame.addr) {
                        // prevent any "##" text from being hidden
                        tsnprintf(tmpbuf, "%s##%zu", line.text.c_str(), i);
                        ImGui::Selectable(tmpbuf, true);
                    } else {
                        // @Imgui: ImGui::Text isn't selectable with a caret cursor, lame
                        ImGui::Text("%s", line.text.c_str());
                    }

                    if (line.addr == frame.addr) {
                        //@@@
                        // size_t s = gui.source_highlighted_line;
                        // size_t linediff = (s > i) ? s - i : i - s;
                        // if (linediff > 10)
                        //{
                        //    // @Hack: line number is now line_disasm index
                        //    gui.source_highlighted_line = i;
                        //    ImGui::SetScrollHereY();
                        //}
                    }
                }
            }

            // scroll once then repeat after delay
            if (ImGui::IsWindowFocused()) {
                int scroll_dir = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                    scroll_dir = 1;
                else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                    scroll_dir = -1;

                static double first_down_milliseconds;
                if (scroll_dir != 0) {
                    timeval tv = {};
                    gettimeofday(&tv, NULL); // get current time
                    double ms = (tv.tv_sec) * 1000
                        + (tv.tv_usec) / 1000; // convert tv_sec & tv_usec to millisecond

                    if (first_down_milliseconds == 0) {
                        // first time pressing up/down
                        // move one line then delay until repeating
                        first_down_milliseconds = ms;
                    } else {
                        // don't repeat the scroll until delay is met
                        if (ms - first_down_milliseconds < 250.0)
                            scroll_dir = 0;
                    }
                } else {
                    first_down_milliseconds = 0;
                }

                if (scroll_dir) {
                    size_t line_idx = ImGui::GetScrollY() / lineheight;
                    if ((line_idx > 0 && scroll_dir == -1)
                        || (line_idx + 1 < file.lines.size() && scroll_dir == 1)) {
                        // TODO: vsync frequency dependent, get consistent scroll
                        ImGui::SetScrollY((line_idx + scroll_dir) * lineheight);
                    }
                }
            }
        }

        if (gui.source_search_bar_open)
            ImGui::EndChild();

        ImGui::End();
        ImGui::PopFont();
        ImGui::GetStyle().FrameBorderSize = saved_frame_border_size; // restore saved size
    }

    if (gui.show_control) {
        ImGui::SetNextWindowBgAlpha(
            1.0); // @Imgui: bug where GetStyleColor doesn't respect window opacity
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Control", &gui.show_control, ImGuiWindowFlags_NoScrollbar);

        // jump to next executed line
        if (ImGui::Button("---") && prog.frame_idx < prog.frames.size()) {
            gui.jump_type = Jump_Goto;
            gui.goto_line_idx = prog.frames[prog.frame_idx].line_idx;
        }

        // resume execution
        ImGui::SameLine();
        if (ImGui::Button("|>") || (!prog.running && IsKeyPressed(ImGuiKey_F5))) {
            if (prog.started)
                ExecuteCommand("-exec-continue");
            else
                gdb::send_blocking("-exec-run");
        }

        // send SIGINT
        ImGui::SameLine();
        if (ImGui::Button("||##Pause")) {
            if (prog.inferior_process != 0)
                kill(prog.inferior_process, SIGINT);

            gdb::send_blocking("-exec-interrupt --all");
        }

        // step line
        ImGui::SameLine();
        if (ImGui::Button("-->")) {
            ExecuteCommand("-exec-step", false);
        }

        // step over
        ImGui::SameLine();
        if (ImGui::Button("/\\>")) {
            ExecuteCommand("-exec-next", false);
        }

        // step out
        ImGui::SameLine();
        if (ImGui::Button("</\\")) {
            if (prog.frame_idx == prog.frames.size() - 1) {
                // GDB error in top frame: "finish" not meaningful in the outermost frame.
                // emulate visual studios by just running the program
                ExecuteCommand("-exec-continue");
            } else {
                ExecuteCommand("-exec-finish", false);
            }
        }

        if (prog.source_out_of_date) {
            ImGui::SameLine();
            ImGui::TextColored(ImColor(1.0f, 1.0f, 0.0f, 1.0f).Value, "%s",
                "Warning: Source file is more recent than executable.");
        }

        static String input_command;

        static const auto GetInputCommand = [&](size_t i) -> String {
            String result;
            if (i < prog.input_cmd_offsets.size()) {
                size_t off = prog.input_cmd_offsets[i];
                if (off < prog.input_cmd_data.size()) {
                    result = prog.input_cmd_data.c_str() + off;
                }
            }
            return result;
        };

        static const auto HistoryCallback = [](ImGuiInputTextCallbackData* data) -> int {
            String new_input_cmd;
            if (data->EventKey == ImGuiKey_UpArrow) {
                if ((size_t)(prog.input_cmd_idx) + 1 < prog.input_cmd_offsets.size()) {
                    prog.input_cmd_idx += 1;
                    new_input_cmd = GetInputCommand(prog.input_cmd_idx);
                }
            } else if (data->EventKey == ImGuiKey_DownArrow) {
                if (prog.input_cmd_idx - 1 < 0) {
                    prog.input_cmd_idx = -1;
                    data->DeleteChars(0, data->BufTextLen);
                } else {
                    prog.input_cmd_idx -= 1;
                    new_input_cmd = GetInputCommand(prog.input_cmd_idx);
                }
            }

            if (new_input_cmd.size()) {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, new_input_cmd.c_str());
            }

            data->BufDirty = true;
            data->CursorPos = data->BufTextLen;
            data->SelectionStart = data->SelectionEnd; // no select
            return 0;
        };

        // show autocomplete modal after pressing tab on input
        static std::vector<String> phrases;
        static size_t phrase_idx;
        static String query_phrase;

        // TODO: syncing up gui disabled buttons when user inputs step next continue
        const float CONSOLE_BAR_HEIGHT = 30.0f;
        ImVec2 logstart = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(logstart.x, ImGui::GetWindowHeight() - CONSOLE_BAR_HEIGHT));
        const ImVec2 AUTOCOMPLETE_START = ImVec2(ImGui::GetCursorScreenPos().x,
            ImGui::GetCursorScreenPos().y
                - (phrases.size() + 1) * ImGui::GetTextLineHeightWithSpacing());

        bool is_autocomplete_selected = false;
        if (phrases.size() > 0) {
            ImGui::SetNextWindowPos(AUTOCOMPLETE_START);
            ImGuiWindowFlags flags = ImGuiWindowFlags_Tooltip |
                // ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
                | ImGuiWindowFlags_NoDocking;
            ImGui::Begin("##Autocomplete", NULL, flags);
            for (size_t i = 0; i < phrases.size(); i++) {
                if (i == phrase_idx) {
                    ImVec2 pos = ImGui::GetWindowPos();
                    ImVec2 sz = ImGui::GetWindowSize();
                    bool hovered = (ImGui::IsMouseHoveringRect(pos, pos + sz));
                    if (ImGui::Selectable(phrases[i].c_str(), !hovered)
                        || IsKeyPressed(ImGuiKey_Enter))
                        is_autocomplete_selected = true;
                } else {
                    if (ImGui::Selectable(phrases[i].c_str(), false)) {
                        phrase_idx = i;
                        is_autocomplete_selected = true;
                    }
                }
            }

            ImGui::End();
        }

        if (ImGui::InputText("##input_command", &input_command,
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                HistoryCallback, NULL)
            || is_autocomplete_selected) {
            // retain focus on the input line
            ImGui::SetKeyboardFocusHere(-1);

            if (is_autocomplete_selected)
                input_command = phrases[phrase_idx];

            // emulate GDB, repeat last executed command upon hitting enter on an empty line
            bool use_last_command
                = (input_command.size() == 0 && prog.input_cmd_offsets.size() > 0);
            String send_command = (use_last_command) ? GetInputCommand(0) : input_command;
            std::erase_if(send_command, [](char c) { return c == '\r' || c == '\n' || c == ' '; });
            String tagged_send_command = StringPrintf("(gdb) %s", send_command.c_str());
            WriteToConsoleBuffer(tagged_send_command.c_str(), tagged_send_command.size());

            query_phrase = "";
            if (phrase_idx < phrases.size()) {
                input_command = phrases[phrase_idx];
                phrase_idx = 0;
                phrases.clear();
            }

            static const auto PopFrontWord = [](String& str) -> String {
                // remove the beginning word from full and return it
                String result;
                std::erase_if(str, [](char c) { return c == '\r' || c == '\n' || c == ' '; });
                size_t space_idx = str.find(' ');

                if (space_idx < str.size()) {
                    result = str.substr(0, space_idx);
                    str = str.substr(space_idx + 1);
                } else {
                    // return whole str as word
                    result = str;
                    str = "";
                }

                return result;
            };

            String rest = send_command; // remaining send command after popping words
            String keyword = PopFrontWord(rest);

            // inject MI commands for program control commands to match their
            // respective button console output

            String exec_mi;
            if (keyword == "file") {
                gdb::set_inferior_exe(rest);
            } else if (keyword == "set") {
                String set_target = PopFrontWord(rest);
                if (set_target == "args")
                    gdb::set_inferior_args(rest);
            } else if (keyword == "step" || keyword == "s") {
                exec_mi = "-exec-step";
            } else if (keyword == "stepi") {
                exec_mi = "-exec-step-instruction";
            } else if (keyword == "continue" || keyword == "c") {
                exec_mi = "-exec-continue";
            } else if (keyword == "next" || keyword == "n") {
                exec_mi = "-exec-next";
            } else if (keyword == "nexti") {
                exec_mi = "-exec-next-instruction";
            }

            if (exec_mi != "") {
                // run the command with the current thread layout
                if (rest != "")
                    exec_mi += " " + rest;

                ExecuteCommand(exec_mi.c_str());
            } else if (send_command.size() > 0 && send_command[0] == '-') {
                // send the machine interpreter command as is
                gdb::send_blocking(send_command.c_str());
            } else {
                // wrap command in machine interpreter statement
                String s = StringPrintf("-interpreter-exec console \"%s\"", send_command.c_str());
                gdb::send_blocking(s.c_str());
            }

            if (!use_last_command) {
                prog.input_cmd_idx = -1;
                if (input_command != GetInputCommand(0)) {
                    prog.input_cmd_offsets.insert(
                        prog.input_cmd_offsets.begin(), prog.input_cmd_data.size());
                    prog.input_cmd_data += input_command;
                    prog.input_cmd_data += '\0';
                }
                input_command = "";
            }
        }

        if (input_command.size() < query_phrase.size()) {
            // went outside of completion scope, clear old data
            phrase_idx = 0;
            phrases.clear();
        }

        size_t len_before_culling = phrases.size();
        for (size_t end = phrases.size(); end > 0; end--) {
            size_t i = end - 1;
            if (NULL == strstr(phrases[i].c_str(), input_command.c_str())) {
                phrases.erase(phrases.begin() + i, phrases.begin() + i + 1);
            }
        }

        if (len_before_culling != phrases.size())
            phrase_idx = 0;

        if (ImGui::IsItemActive()
            && IsKeyPressed(ImGuiKey_Tab)) // && ImGui::GetIO().WantCaptureKeyboard)
        {
            if (phrases.size() == 0) {
                String cmd = StringPrintf("-complete \"%s\"", input_command.c_str());
                if (gdb::send_blocking(cmd.c_str(), rec)) {
                    phrase_idx = 0;
                    phrases.clear();
                    query_phrase = input_command;
                    const RecordAtom* matches = gdb::extract_atom("matches", rec);
                    for (const RecordAtom& match : gdb::iter_child(rec, matches)) {
                        phrases.push_back(gdb::get_atom_string(match.value, rec));
                    }
                }
            } else {
                bool shift_down = ImGui::GetIO().KeyShift;
                if (phrase_idx == phrases.size() - 1 && !shift_down)
                    phrase_idx = 0;
                else if (phrase_idx == 0 && shift_down)
                    phrase_idx = phrases.size() - 1;
                else
                    phrase_idx += (shift_down) ? -1 : 1;
            }
        }

        if (IsKeyPressed(ImGuiKey_Escape)) {
            phrase_idx = 0;
            phrases.clear();
        }

        {
            if (prog.started) {
                // read in inferior stdout
                while (true) {
                    pollfd p = {};
                    p.fd = g_gdb.fd_ptty_master;
                    p.events = POLLIN;

                    int rc = poll(&p, 1, 0);
                    if (rc < 0) {
                        PrintErrorf("poll %s\n", GetErrorString(errno));
                        break;
                    } else if (rc == 0) // timeout
                    {
                        break;
                    } else if (rc > 0) {
                        // ensure we got data ready to poll
                        if ((p.revents & POLLIN) == 0)
                            break;

                        char buf[1024] = {};
                        int bytes_read = read(g_gdb.fd_ptty_master, buf, sizeof(buf));
                        if (bytes_read < 0) {
                            PrintErrorf("read %s\n", GetErrorString(errno));
                            break;
                        } else {
                            WriteToConsoleBuffer(buf, bytes_read);
                        }
                    }
                }
            }

            // draw the console log
            ImGui::SetCursorPos(logstart);
            ImVec2 logsize = ImGui::GetWindowSize();
            logsize.y = logsize.y - logstart.y - CONSOLE_BAR_HEIGHT;
            logsize.x = 0.0f; // take up the full window width
            ImGui::BeginChild("##GDB_Console", logsize, true, ImGuiWindowFlags_HorizontalScrollbar);

            // draw the log lines upwards from the bottom of the child window
            // @@@ ImGui::SetCursorPosY( GetMax(ImGui::GetCursorPosY(), logsize.y -
            // prog.num_log_rows * ImGui::GetTextLineHeightWithSpacing()) );

            ImGui::TextUnformatted(prog.log, prog.log + prog.log_idx);

            if (prog.log_scroll_to_bottom) {
                ImGui::SetScrollHereY(1.0f);
                prog.log_scroll_to_bottom = false;
            }

            ImGui::EndChild();
        }

        ImGui::End();
    }

    if (gui.show_locals) {
        ImGui::SetNextWindowBgAlpha(
            1.0); // @Imgui: bug where GetStyleColor doesn't respect window opacity
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Locals", &gui.show_locals);
        if (ImGui::BeginTable("##LocalsTable", 2, TABLE_FLAGS)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 125.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_NoResize);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < prog.local_vars.size(); i++) {
                const VarObj& iter = prog.local_vars[i];
                if (iter.value[0] == '{') {
                    RecurseExpressionTreeNodes(iter, 0);
                } else {
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::Text("%s", iter.name.c_str());

                    ImGui::TableNextColumn();
                    ImColor color = (iter.changed) ? IM_COL32_WIN_RED
                                                   : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    ImGui::TextColored(color, "%s", iter.value.c_str());
                }
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

    if (gui.show_callstack) {
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Callstack", &gui.show_callstack);

        // draw the thread stack as a dropdown box
        String thread_preview;
        if (prog.thread_idx < prog.threads.size()) {
            const Thread& t = prog.threads[prog.thread_idx];
            thread_preview = StringPrintf("Thread ID %d Group ID %s", t.id, t.group_id.c_str());
        }

        if (ImGui::BeginCombo("Threads##Callstack", thread_preview.c_str())) {
            for (size_t i = 0; i < prog.threads.size(); i++) {
                const Thread& t = prog.threads[i];
                String str = StringPrintf("Thread ID %d Group ID %s", t.id, t.group_id.c_str());
                if (ImGui::Selectable(str.c_str(), prog.thread_idx == i) && prog.thread_idx != i) {
                    prog.thread_idx = i;
                    prog.frame_idx = 0;
                    QueryFrame(true);
                }
            }

            ImGui::EndCombo();
        }

        for (size_t i = 0; i < prog.frames.size(); i++) {
            const Frame& iter = prog.frames[i];

            String file
                = (iter.file_idx < prog.files.size()) ? prog.files[iter.file_idx].filename : "???";

            // @Windows
            // get the filename from the full path
            const char* last_fwd_slash = strrchr(file.c_str(), '/');
            const char* filename = (last_fwd_slash != NULL) ? last_fwd_slash + 1 : file.c_str();

            tsnprintf(tmpbuf, "%4zu %s##%zu", iter.line_idx + 1, filename, i);

            if (ImGui::Selectable(tmpbuf, i == prog.frame_idx)) {
                prog.frame_idx = i;
                const Frame& frame = prog.frames[prog.frame_idx];
                if (frame.file_idx < prog.files.size()) {
                    prog.file_idx = frame.file_idx;
                    QueryFrame(true);
                }
            }
        }

        ImGui::End();
    }

    if (gui.show_registers) {
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Registers", &gui.show_registers);
        if (ImGui::BeginTable("##RegistersTable", 2, TABLE_FLAGS)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 125.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_NoResize);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < prog.global_vars.size(); i++) {
                const VarObj& iter = prog.global_vars[i];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::Text("%s", iter.name.c_str());

                ImGui::TableNextColumn();
                ImColor color
                    = (iter.changed) ? IM_COL32_WIN_RED : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::TextColored(color, "%s", iter.value.c_str());
            }

            ImGui::EndTable();
        }
        ImGui::End();
    }

    //
    // watch: query variables every time the program stops
    //
    if (gui.show_watch) {
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Watch", &gui.show_watch);
        if (ImGui::BeginTable("##WatchTable", 2, TABLE_FLAGS)) {
            static size_t edit_var_name_idx = -1;
            static bool focus_name_input = false;
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 125.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_NoResize);
            ImGui::TableHeadersRow();

            // InputText color
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 16));

            for (size_t i = 0; i < prog.watch_vars.size(); i++) {
                VarObj& iter = prog.watch_vars[i];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);

                // is column clicked?
                static char editwatch[4096];
                bool column_clicked = false;
                if (i == edit_var_name_idx) {
                    if (ImGui::InputText("##edit_watch", editwatch, sizeof(editwatch),
                            ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL)) {
                        iter = {};
                        iter.name = editwatch;
                        QueryWatchlist();
                        Zeroize(editwatch);
                        edit_var_name_idx = -1;
                    }

                    static int delay = 0; // @Imgui: need a delay or else it will auto de-focus
                    if (focus_name_input) {
                        ImGui::SetKeyboardFocusHere(-1);
                        focus_name_input = false;
                        delay = 0;
                    } else {
                        bool deleted = false;
                        delay++;
                        bool active = ImGui::IsItemFocused()
                            && (delay < 2 || ImGui::GetIO().WantCaptureKeyboard);
                        if (IsKeyPressed(ImGuiKey_Delete)) {
                            prog.watch_vars.erase(
                                prog.watch_vars.begin() + i, prog.watch_vars.begin() + i + 1);
                            size_t sz = prog.watch_vars.size();
                            if (sz > 0) {
                                // activate another watch variable input box
                                ImGui::SetKeyboardFocusHere(0);
                                edit_var_name_idx = (i > sz) ? sz - 1 : i;
                                column_clicked = true;
                            } else {
                                deleted = true;
                            }
                        }

                        if (!active || deleted || IsKeyPressed(ImGuiKey_Escape)) {
                            edit_var_name_idx = -1;
                            continue;
                        }
                    }

                } else {
                    // check if table cell is clicked
                    ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::Text("%s", iter.name.c_str());
                    ImVec2 sz
                        = ImVec2(ImGui::GetColumnWidth(), ImGui::GetCursorScreenPos().y - p0.y);
                    if (ImGui::IsMouseHoveringRect(p0, p0 + sz)
                        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        column_clicked = true;
                    }
                }

                if (column_clicked) {
                    tsnprintf(editwatch, "%s", iter.name.c_str());
                    focus_name_input = true;
                    edit_var_name_idx = i;
                }

                ImGui::TableNextColumn();
                ImColor color
                    = (iter.changed) ? IM_COL32_WIN_RED : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::TextColored(color, "%s", iter.value.c_str());

                if (iter.expr.atoms.size() > 0) {
                    RecurseExpressionTreeNodes(iter, 0);
                }
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            static char watch[256];

            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##create_new_watch", watch, sizeof(watch),
                    ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL)) {
                VarObj add = CreateVarObj(watch);
                prog.watch_vars.push_back(add);
                QueryWatchlist();
                Zeroize(watch);
            }
            ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            ImGui::Text("%s", "");
            ImGui::EndTable();
        }

        ImGui::End();
    }

    //
    // breakpoints
    //
    if (gui.show_breakpoints) {
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Breakpoints", &gui.show_breakpoints);
        if (ImGui::BeginTable("##BreakpointsTable", 5, TABLE_FLAGS)) {
            // TODO: checkbox column to enable/disable breakpoints
            static size_t edit_bkpt_idx = BAD_INDEX;
            static bool focus_cond_input = false;
            bool tmp = true;

            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn("Number");
            ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthFixed, 125.0f);
            ImGui::TableSetupColumn("Line");
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_NoResize);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

            // enable all, disable all, delete all
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Button("X##BreakpointDeleteAll") && gdb::send_blocking("-break-delete --all"))
                prog.breakpoints.clear();

            ImGui::SameLine();
            if (ImGui::Checkbox("##BreakpointEnableAll", &tmp)
                && gdb::send_blocking("-break-enable --all"))
                for (Breakpoint& b : prog.breakpoints)
                    b.enabled = true;

            tmp = false;
            ImGui::SameLine();
            if (ImGui::Checkbox("##BreakpointDisableAll", &tmp)
                && gdb::send_blocking("-break-disable --all"))
                for (Breakpoint& b : prog.breakpoints)
                    b.enabled = false;

            ImGui::TableSetColumnIndex(1);
            ImGui::TableHeader("Number");

            ImGui::TableSetColumnIndex(2);
            ImGui::TableHeader("Condition");

            ImGui::TableSetColumnIndex(3);
            ImGui::TableHeader("Line");

            ImGui::TableSetColumnIndex(4);
            ImGui::TableHeader("File");

            // InputText color
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 16));

            for (size_t i = 0; i < prog.breakpoints.size(); i++) {
                Breakpoint& iter = prog.breakpoints[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                tsnprintf(tmpbuf, "X##BreakpointDelete%d", (int)i);
                if (ImGui::Button(tmpbuf)) {
                    tsnprintf(tmpbuf, "-break-delete %zu", iter.number);
                    if (gdb::send_blocking(tmpbuf)) {
                        prog.breakpoints.erase(
                            prog.breakpoints.begin() + i, prog.breakpoints.begin() + i + 1);
                        continue;
                    }
                }

                ImGui::SameLine();
                tsnprintf(tmpbuf, "##BreakpointToggle%d", (int)i);
                tmp = iter.enabled;
                if (ImGui::Checkbox(tmpbuf, &tmp)) {
                    if (iter.enabled)
                        tsnprintf(tmpbuf, "-break-disable %zu", iter.number);
                    else
                        tsnprintf(tmpbuf, "-break-enable %zu", iter.number);

                    if (gdb::send_blocking(tmpbuf))
                        iter.enabled = !iter.enabled;
                }

                // check if breakpoint doesn't have line (watchpoints)
                bool has_line = IsValidLine(iter.line_idx, iter.file_idx);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", (int)iter.number);

                ImGui::TableSetColumnIndex(2);
                static char editcond[1024];
                if (i == edit_bkpt_idx) {
                    if (ImGui::InputText("##EditBreakpointCond", editcond, sizeof(editcond),
                            ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL)) {
                        g_gdb.echo_next_no_symbol_in_context = true;
                        tsnprintf(tmpbuf, "-break-condition %d %s", (int)iter.number, editcond);
                        if (gdb::send_blocking(tmpbuf, rec)) {
                            iter.cond = editcond;
                        } else {
                            tsnprintf(tmpbuf, "-break-condition %d", (int)iter.number);
                            gdb::send_blocking(tmpbuf, rec);
                        }

                        Zeroize(editcond);
                        edit_bkpt_idx = BAD_INDEX;
                    }

                    if (focus_cond_input) {
                        ImGui::SetKeyboardFocusHere(-1);
                        focus_cond_input = false;
                    }

                    // user clicked outside of textbox
                    if (ImGui::IsItemDeactivated())
                        edit_bkpt_idx = BAD_INDEX;
                } else {
                    // check if table cell is clicked
                    ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGuiDisabled(!has_line, ImGui::Text("%s", iter.cond.c_str()));
                    ImVec2 sz
                        = ImVec2(ImGui::GetColumnWidth(), ImGui::GetCursorScreenPos().y - p0.y);

                    if (has_line && ImGui::IsMouseHoveringRect(p0, p0 + sz)
                        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        tsnprintf(editcond, "%s", iter.cond.c_str());
                        edit_bkpt_idx = i;
                        focus_cond_input = true;
                    }
                }

                ImGui::TableSetColumnIndex(3);
                if (has_line)
                    ImGui::Text("%u", (int)(iter.line_idx + 1));

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s",
                    (iter.file_idx < prog.files.size()) ? prog.files[iter.file_idx].filename.c_str()
                                                        : "???");
            }

            ImGui::PopStyleColor();
            ImGui::EndTable();
        }

        ImGui::End();
    }

    //
    // threads
    //
    if (gui.show_threads) {
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Threads", &gui.show_threads);
        if (ImGui::BeginTable("##ThreadsTable", 4, TABLE_FLAGS)) {
            ImGui::TableSetupColumn(""); // lock/unlock all
            ImGui::TableSetupColumn(""); // resume all
            ImGui::TableSetupColumn(""); // pause all
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoResize);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

            // custom headers row, |> and || resume and pause all respectively
            // @GDB: becomes unresponsive if sending -exec-continue --all with no threads available
            ImGui::TableSetColumnIndex(0);
            bool tmp = true;
            if (ImGui::Checkbox("##ThreadFocusAll", &tmp))
                for (Thread& t : prog.threads)
                    t.focused = true;

            tmp = false;
            ImGui::SameLine();
            if (ImGui::Checkbox("##ThreadUnfocusAll", &tmp))
                for (Thread& t : prog.threads)
                    t.focused = false;

            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button("|>##ResumeAll") && prog.threads.size() > 0)
                gdb::send_blocking("-exec-continue --all");

            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button("||##PauseAll"))
                gdb::send_blocking("-exec-interrupt --all");

            ImGui::TableSetColumnIndex(3);
            ImGui::TableHeader("Name");

            for (size_t i = 0; i < prog.threads.size(); i++) {
                Thread& thread = prog.threads[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                tsnprintf(tmpbuf, "##ThreadToggleFocus%zu", i);
                if (ImGui::Checkbox(tmpbuf, &thread.focused)) {
                    // lower all other checkboxes except this one
                    if (ImGui::GetIO().KeyShift) {
                        thread.focused = true;
                        for (size_t j = 0; j < prog.threads.size(); j++)
                            if (i != j)
                                prog.threads[j].focused = false;
                    }
                }

                bool clicked_run = false;
                ImGui::TableSetColumnIndex(1);
                tsnprintf(tmpbuf, "|>##Thread%i", (int)i);
                ImGuiDisabled(thread.running, clicked_run = ImGui::Button(tmpbuf));
                if (clicked_run) {
                    tsnprintf(tmpbuf, "-exec-continue --thread %d", thread.id);
                    gdb::send_blocking(tmpbuf);
                }

                bool clicked_pause = false;
                ImGui::TableSetColumnIndex(2);
                tsnprintf(tmpbuf, "||##Thread%i", (int)i);
                ImGuiDisabled(!thread.running, clicked_pause = ImGui::Button(tmpbuf));
                if (clicked_pause) {
                    tsnprintf(tmpbuf, "-exec-interrupt --thread %d", thread.id);
                    gdb::send_blocking(tmpbuf);
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("Thread ID %d Group ID %s", thread.id, thread.group_id.c_str());
            }

            // @ImGui:: when double clicking a column separator to resize to fit,
            // the resize messes up with a button column header. Draw two invisible buttons
            ImGui::EndTable();
        }

        ImGui::End();
    }

    if (gui.show_directory_viewer) {
        // treenode directory viewer
        ImGui::SetNextWindowSize(MIN_WINSIZE, ImGuiCond_Once);
        ImGui::Begin("Directory Viewer", &gui.show_directory_viewer);

        struct FileEntry {
            unsigned char dirent_d_type; // dirent.d_type
            String filename;
            std::vector<FileEntry> entries;
            bool queried;
            FileEntry(String fn, unsigned char d_type)
            {
                dirent_d_type = d_type;
                filename = fn;
                queried = false;
            }
        };

        char abspath[PATH_MAX];
        static FileEntry root = FileEntry(realpath(".", abspath), 0);
        static FileEntry* query_output = &root;
        static String query_dir = root.filename;

        // TODO: is there a cleaner way to recurse lambdas
        typedef std::function<void(FileEntry & root, String & path)> RecurseFilesFn;
        static RecurseFilesFn RecurseFiles;
        int id = 0;
        const RecurseFilesFn _RecurseFiles
            = [&id, &tmpbuf, &abspath](FileEntry& parent, String& path) {
                  for (FileEntry& ent : parent.entries) {
                      if (ent.dirent_d_type & DT_DIR) {
                          if (ImGui::TreeNode(ent.filename.c_str())) {
                              if (!ent.queried) {
                                  query_output = &ent;
                                  query_dir = path + "/" + ent.filename;
                              } else if (ent.entries.size() > 0) {
                                  String next = path + "/" + ent.filename;
                                  RecurseFiles(ent, next);
                              }

                              ImGui::TreePop();
                          }
                      } else {
                          tsnprintf(tmpbuf, "%s##%d", ent.filename.c_str(), id++);
                          if (ImGui::Selectable(tmpbuf)) {
                              String relpath = path + "/" + ent.filename;
                              char* rc = realpath(relpath.c_str(), abspath);
                              if (rc == NULL) {
                                  PrintErrorf("realpath %s\n", GetErrorString(errno));
                              } else {
                                  size_t idx = FindOrCreateFile(abspath);
                                  File& f = prog.files[idx];
                                  if (f.lines.size() > 0 || LoadFile(f)) {
                                      prog.file_idx = idx;
                                      gui.jump_type = Jump_Goto;
                                      gui.goto_line_idx = 0;
                                  }
                              }
                          }
                      }
                  }
              };

        static FileWindowContext ctx;
        static bool show_change_dir = false;
        show_change_dir |= ImGui::Button("...##ChangeDirectory");

        if (show_change_dir
            && ImGuiFileWindow(ctx, ImGuiFileWindowMode_SelectDirectory, root.filename.c_str())) {
            if (ctx.selected) {
                query_output = &root;
                root.filename = ctx.path.c_str();
                query_dir = root.filename;
            }

            show_change_dir = false;
        }

        ImGui::SameLine();
        ImGui::Text("%s", root.filename.c_str());

        RecurseFiles = _RecurseFiles;
        RecurseFiles(root, root.filename);

        if (query_output != NULL) {
            query_output->entries.clear();
            query_output->queried = true;
            struct dirent* entry = NULL;
            DIR* dir = NULL;

            dir = opendir(query_dir.c_str());
            if (dir == NULL) {
                PrintErrorf("opendir on %s %s\n", query_dir.c_str(), GetErrorString(errno));
            } else {
                // TODO: lstat then S_ISREG and S_ISDIR macros for when d_type isn't supported
                while (NULL != (entry = readdir(dir))) {
                    if (0 == strcmp(".", entry->d_name) || 0 == strcmp("..", entry->d_name))
                        continue; // skip current and parent dir

                    // insert directories before files, all sorted a-z
                    bool entry_dir = (entry->d_type & DT_DIR);
                    size_t insert_idx = query_output->entries.size();
                    for (size_t i = 0; i < query_output->entries.size(); i++) {
                        const FileEntry& iter = query_output->entries[i];
                        bool iter_dir = (iter.dirent_d_type & DT_DIR);
                        if (entry_dir == iter_dir) {
                            if (-1 == strcmp(entry->d_name, iter.filename.c_str())) {
                                insert_idx = i;
                                break;
                            }
                        } else if (entry_dir && !iter_dir) {
                            insert_idx = i;
                            break;
                        }
                    }

                    query_output->entries.insert(query_output->entries.begin() + insert_idx,
                        FileEntry(entry->d_name, entry->d_type));
                }

                closedir(dir);
                dir = NULL;
            }

            query_output = NULL;
        }

        ImGui::End();
    }
}

static void glfw_error_callback(int error, const char* description)
{
    PrintErrorf("Glfw Error %d %s\n", error, description);
}

void DrawDebugOverlay()
{
    const ImGuiIO& io = ImGui::GetIO();
    static bool debug_window_toggled;
    if (IsKeyPressed(ImGuiKey_F1))
        debug_window_toggled = !debug_window_toggled;

    if (debug_window_toggled) {
        char tmp[4096];
        ImDrawList* drawlist = ImGui::GetForegroundDrawList();
        snprintf(tmp, sizeof(tmp), "Mouse Position: (%.1f,%.1f)", io.MousePos.x, io.MousePos.y);
        ImVec2 BR = ImGui::CalcTextSize(tmp);
        ImVec2 TL = { 0, 0 };

        drawlist->AddRectFilled(TL, BR, 0xFFFFFFFF);
        drawlist->AddText(TL, 0xFF000000, tmp);

        snprintf(tmp, sizeof(tmp), "Application average %.3f ms/frame (%.1f FPS)",
            1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

        TL.y = BR.y;
        BR = ImGui::CalcTextSize(tmp);
        BR.x += TL.x;
        BR.y += TL.y;
        drawlist->AddRectFilled(TL, BR, 0xFFFFFFFF);
        drawlist->AddText(TL, 0xFF000000, tmp);

        static bool pinned_point_toggled;
        static ImVec2 pinned_point;
        static ImVec2 pinned_window;

        ImVec2 mousepos = ImGui::GetMousePos();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            pinned_point_toggled = !pinned_point_toggled;
            if (pinned_point_toggled) {
                // get relative window position
                ImGuiContext* ctx = ImGui::GetCurrentContext();

                // windows are stored back to front
                for (ImGuiWindow* iter : ctx->Windows) {
                    const ImVec2 WINDOW_BR
                        = ImVec2(iter->Pos.x + iter->Size.x, iter->Pos.y + iter->Size.y);
                    if ((!iter->Hidden && !iter->Collapsed)
                        && (iter->Pos.x <= mousepos.x && iter->Pos.y <= mousepos.y)
                        && (WINDOW_BR.x >= mousepos.x && WINDOW_BR.y >= mousepos.y)) {
                        pinned_window = iter->Pos;
                    }
                }
                pinned_point = mousepos;
            }
        }

        if (pinned_point_toggled) {
            // draw a rect in the selected window
            const ImVec2 PIN_BR
                = ImVec2(mousepos.x - pinned_window.x, mousepos.y - pinned_window.y);
            const ImVec2 PIN_TL
                = ImVec2(pinned_point.x - pinned_window.x, pinned_point.y - pinned_window.y);

            uint32_t col = IM_COL32(0, 255, 0, 32);
            drawlist->AddRectFilled(pinned_point, mousepos, col);

            snprintf(tmp, sizeof(tmp), "window rect:\n  pos: (%d, %d)\n  size: (%d, %d)",
                (int)PIN_TL.x, (int)PIN_TL.y, (int)(PIN_BR.x - PIN_TL.x),
                (int)(PIN_BR.y - PIN_TL.y));
            BR = ImGui::CalcTextSize(tmp);
            TL = ImVec2(pinned_point.x, pinned_point.y - BR.y);
            BR.x += TL.x;
            BR.y += TL.y;

            drawlist->AddRectFilled(TL, BR, IM_COL32_WHITE);
            drawlist->AddText(TL, IM_COL32_BLACK, tmp);
        }
    }
}

int main(int argc, char** argv)
{
#define ExitMessagef(fmt, ...)           \
    do {                                 \
        PrintErrorf(fmt, ##__VA_ARGS__); \
        return EXIT_FAILURE;             \
    } while (0)
#define ExitMessage(msg) ExitMessagef("%s", msg)

    atexit([]() {
        // shutdown imgui
        if (gui.started_imgui_opengl2)
            ImGui_ImplOpenGL2_Shutdown();

        if (gui.started_imgui_glfw)
            ImGui_ImplGlfw_Shutdown();

        if (gui.created_imgui_context)
            ImGui::DestroyContext();

        // shutdown glfw
        if (gui.window)
            glfwDestroyWindow(gui.window);

        if (gui.initialized_glfw)
            glfwTerminate();

        // shutdown GDB
        if (g_gdb.thread_read_interp) {
            pthread_cancel(g_gdb.thread_read_interp);
            pthread_join(g_gdb.thread_read_interp, NULL);
        }

        if (g_gdb.recv_block)
            sem_close(g_gdb.recv_block);

        if (g_gdb.fd_ptty_master)
            close(g_gdb.fd_ptty_master);

        if (g_gdb.fd_in_read)
            close(g_gdb.fd_in_read);

        if (g_gdb.fd_out_read)
            close(g_gdb.fd_out_read);

        if (g_gdb.fd_in_write)
            close(g_gdb.fd_in_write);

        if (g_gdb.fd_out_write)
            close(g_gdb.fd_out_write);

        if (g_gdb.spawned_pid)
            os::kill_process(g_gdb.spawned_pid);

        pthread_mutex_t zmutex = {};
        if (memcmp(&zmutex, &g_gdb.modify_block, sizeof(pthread_mutex_t)) != 0)
            pthread_mutex_destroy(&g_gdb.modify_block);
    });

    {
        // GDB Init
        int rc = 0;

        int pipes[2] = {};
        rc = pipe(pipes);
        if (rc < 0)
            ExitMessagef("from gdb pipe %s\n", GetErrorString(errno));

        g_gdb.fd_in_read = pipes[0];
        g_gdb.fd_in_write = pipes[1];

        rc = pipe(pipes);
        if (rc < 0)
            ExitMessagef("to gdb pipe %s\n", GetErrorString(errno));

        g_gdb.fd_out_read = pipes[0];
        g_gdb.fd_out_write = pipes[1];

        rc = pthread_mutex_init(&g_gdb.modify_block, NULL);
        if (rc < 0)
            ExitMessagef("pthread_mutex_init %s\n", GetErrorString(errno));

        g_gdb.recv_block = sem_open("/sem_recv_gdb_block", O_CREAT, S_IRWXU, 0);
        if (g_gdb.recv_block == NULL) {
            if (errno == ENOSYS) // function not implemented
            {
                static sem_t unnamed_sem;
                if (0 == sem_init(&unnamed_sem, 0, 0)) {
                    g_gdb.recv_block = &unnamed_sem;
                } else {
                    ExitMessagef("sem_init %s\n", GetErrorString(errno));
                }
            } else {
                ExitMessagef("sem_open %s\n", GetErrorString(errno));
            }
        }

        extern void* GDB_ReadInterpreterBlocks(void*);
        rc = pthread_create(&g_gdb.thread_read_interp, NULL, gdb::read_interpreter_blocks, (void*)NULL);
        if (rc < 0)
            ExitMessagef("pthread_create %s\n", GetErrorString(errno));

        // attempt to open a pseudoterminal for debugged program input/output
        int ptty_fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (ptty_fd != -1) {
            if (0 == grantpt(ptty_fd)) {
                if (0 == unlockpt(ptty_fd)) {
                    g_gdb.fd_ptty_master = ptty_fd;
                    Printf("pty slave: %s\n", ptsname(g_gdb.fd_ptty_master));
                } else {
                    PrintErrorf("unlockpt %s\n", GetErrorString(errno));
                }
            } else {
                PrintErrorf("grantpt %s\n", GetErrorString(errno));
            }

            // cleanup fd if grantpt/unlockpt failed
            if (g_gdb.fd_ptty_master == 0) {
                close(ptty_fd);
                ptty_fd = 0;
            }
        } else {
            PrintErrorf("posix_openpt %s\n", GetErrorString(errno));
        }

        auto sig_handler = [](int) {
            if (gui.window)
                glfwSetWindowShouldClose(gui.window, 1);
        };

        struct sigaction act = {};
        act.sa_handler = sig_handler;
        if (0 > sigaction(SIGINT, &act, NULL) || 0 > sigaction(SIGTERM, &act, NULL)) {
            ExitMessagef("sigaction %s\n", GetErrorString(errno));
        }
    }

    if (argc <= 1) {
        ExitMessagef("missing %s param\n", "");
    }

    g_gdb.debug_filename = argv[1];

    if (!g_gdb.filename.empty() && !gdb::start_process(g_gdb.filename, "")) {
        g_gdb.filename = "";
    }

    if (g_gdb.spawned_pid != 0 && !g_gdb.debug_filename.empty()) {
        if (gdb::set_inferior_exe(g_gdb.debug_filename)) {
            if (g_gdb.capabilities & GDB_RUNSTART)
                gdb::send_blocking("-exec-run --start");
        } else {
            g_gdb.debug_filename = "";
        }
    }

    // initialize GLFW
    glfwSetErrorCallback(glfw_error_callback);
    gui.initialized_glfw = glfwInit();
    if (!gui.initialized_glfw)
        ExitMessage("glfwInit\n");

    gui.window = glfwCreateWindow(1080, 720, "ImGDB", NULL, NULL);
    if (gui.window == NULL)
        ExitMessage("glfwCreateWindow\n");

    glfwMakeContextCurrent(gui.window);
    glfwSwapInterval(1); // Enable vsync

    const auto OnDragDrop = [](GLFWwindow* /*window*/, int count, const char** paths) {
        if (count == 1) {
            const char* file = paths[0];
            if (os::fs::is_executable(file)) {
                gui.drag_drop_exe_path = file;
            }
        }
    };
    glfwSetDropCallback(gui.window, OnDragDrop);

    // Startup Dear ImGui
    if (!IMGUI_CHECKVERSION())
        ExitMessage("IMGUI_CHECKVERSION\n");

    gui.created_imgui_context = ImGui::CreateContext();
    if (!gui.created_imgui_context)
        ExitMessage("ImGui::CreateContext\n");

    gui.started_imgui_glfw = ImGui_ImplGlfw_InitForOpenGL(gui.window, true);
    if (!gui.started_imgui_glfw)
        ExitMessage("ImGui_ImplGlfw_InitForOpenGL\n");

    gui.started_imgui_opengl2 = ImGui_ImplOpenGL2_Init();
    if (!gui.started_imgui_opengl2)
        ExitMessage("ImGui_ImplOpenGL2_Init\n");

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking

    // Setup Dear ImGui style
    SetWindowTheme(gui.window_theme);
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScrollbarRounding = 2.0f;
    style.TabRounding = 2.0f;

    // change font data before it gets locked with NewFrame
    // TODO: reloading both fonts when source font changes, might change to font scaling
    // instead
    io.Fonts->Clear();
    ImGui_ImplOpenGL2_DestroyFontsTexture();

    const auto LoadFont = [&io](float font_size) -> ImFont* {
        ImFontConfig cfg = {};
        cfg.FontDataOwnedByAtlas = false; // static memory
        ImFont* result = io.Fonts->AddFontFromMemoryTTF(
            liberation_mono_ttf, sizeof(liberation_mono_ttf), font_size, &cfg);

        if (result == NULL)
            PrintError("error loading default font?!?!?");

        return result;
    };

    gui.default_font = LoadFont(gui.font_size);
    gui.source_font = LoadFont(gui.source_font_size);

    if (gui.default_font == NULL || gui.source_font == NULL)
        return EXIT_FAILURE;

    ImGui_ImplOpenGL2_CreateFontsTexture();

    // Main loop
    while (!glfwWindowShouldClose(gui.window)) {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui
        // wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main
        // application.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main
        // application. Generally you may always pass all inputs to dear imgui, and hide them from
        // your application based on those two flags.

        if (!glfwGetWindowAttrib(gui.window, GLFW_VISIBLE)
            || glfwGetWindowAttrib(gui.window, GLFW_ICONIFIED)) {
            glfwWaitEvents();
        }

        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(
            ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
        DrawDebugOverlay();
        Draw();

        // Rendering
        int display_w = 0, display_h = 0;
        ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

        ImGui::Render();
        glfwGetFramebufferSize(gui.window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
            clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        // If you are using this code with non-legacy OpenGL header/contexts (which you should not,
        // prefer using imgui_impl_opengl3.cpp!!), you may need to backup/reset/restore other state,
        // e.g. for current shader using the commented lines below. GLint last_program;
        // glGetIntegerv(GL_CURRENT_PROGRAM, &last_program); glUseProgram(0);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        // glUseProgram(last_program);

        glfwMakeContextCurrent(gui.window);
        glfwSwapBuffers(gui.window);
    }

    // Shutdown lambda called here from atexit
    return EXIT_SUCCESS;
}
