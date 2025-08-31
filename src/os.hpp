struct ExecResult {
    String stdout;
    i32 rc;
};

namespace os {

bool exists_process(i32 pid);
void kill_process(i32 pid);
ExecResult exec(StringView cmd);

namespace fs {

bool exists(StringView path);
bool is_executable(StringView path);

} // namespace fs

} // namespace os