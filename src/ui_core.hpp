#pragma once

enum class LineDisplay {
    Source,
    Disassembly,
    Source_And_Disassembly,
};

struct Session {
    String debug_exe;
    String debug_args;
};

struct UI {
    GLFWwindow* window;
    LineDisplay line_display = LineDisplay::Source;
    std::vector<DisassemblyLine> line_disasm;
    std::vector<DisassemblySourceLine> line_disasm_source;
    bool show_machine_interpreter_commands;

    bool source_search_bar_open;
    char source_search_keyword[256];
    bool source_found_line;
    size_t source_found_line_idx;
    size_t goto_line_idx;
    bool refresh_docking_space = true;

    bool show_source = true;
    bool show_control = true;
    bool show_callstack = true;
    bool show_registers = false;
    bool show_locals = true;
    bool show_watch = true;
    bool show_breakpoints = false;
    bool show_threads = false;
    std::vector<Session> session_history;
    int hover_delay_ms = 100;
};

namespace ui_core {
};