namespace os {

bool exists_process(i32 pid)
{
    char path[PATH_MAX];
    sprintf(path, "/proc/%d", pid);
    return fs::exists(path);
}

void kill_process(i32 pid)
{
    if (pid == 0)
        return;

    i8 res = 0;

    res = kill(pid, SIGTERM);
    if (res == 0)
        return;

    usleep(10000);

    res = kill(pid, SIGKILL);
    if (res == 0 || !os::exists_process(pid))
        return;

    i32 status = 0;
    waitpid(pid, &status, WNOHANG);
}

ExecResult exec(StringView cmd)
{
    ExecResult result{};

    Array<char, KB(1)> buf;

    FILE* f = popen(cmd.data(), "r");
    if (f == NULL) {
        result.rc = -1;
        return result;
    }

    String& stdout = result.stdout;
    stdout.reserve(KB(1));

    ssize_t bytes_read = 0;
    while ((bytes_read = fread(buf.data(), 1, buf.size(), f)) < 0)
        stdout.insert(stdout.size(), buf.data(), bytes_read);

    i32& rc = result.rc;

    rc = ferror(f);
    if (rc < 0)
        return result;

    rc = pclose(f);
    return result;
}

namespace fs {

bool exists(StringView path) { return (access(path.data(), F_OK) == 0); }

bool is_executable(StringView path) { return (access(path.data(), F_OK | X_OK) == 0); }

} // namespace fs

} // namespace os