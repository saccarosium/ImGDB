struct GDBState {
    // Persistent Thread Storage
    Arena arena;
    // Ephimeral Thread Storage
    Arena scratch;

    pid_t pid;
    pthread_t tid;

    int fd_stdin;
    int fd_stdout;
    int fd_stderr;
};

static GDBState g_gdb_state = {0};

namespace gdb {

static void* start_process(void*)
{
    const char* gdb_argv[] = { 
        "gdb",
        "--interpreter=mi",
        "--tty=/dev/null",
        nullptr 
    };

    g_gdb_state.pid = fork();

    if (g_gdb_state.pid == 0) {
        setsid();
        dup2(g_gdb_state.fd_stdin,  0);
        dup2(g_gdb_state.fd_stdout, 1);
        dup2(g_gdb_state.fd_stderr, 2);
        execvp("gdb", (char* const*)gdb_argv);
        fprintf(stderr, "Error: Couldn't execute gdb.\n");
        exit(EXIT_FAILURE);
    } else if (g_gdb_state.pid < 0) {
        fprintf(stderr, "Error: Couldn't fork.\n");
        exit(EXIT_FAILURE);
    }

    return nullptr;
}

void init()
{
    g_gdb_state.arena = arena_init(KB(1));
    g_gdb_state.scratch = arena_init(KB(1));

    if (pthread_create(&g_gdb_state.tid, nullptr, start_process, nullptr))
        exit(1);
}

void destroy()
{
    kill(g_gdb_state.pid, SIGKILL);
    pthread_cancel(g_gdb_state.tid);

    close(g_gdb_state.fd_stdin);
    close(g_gdb_state.fd_stdout);
    close(g_gdb_state.fd_stderr);
}

} // namespace gdb