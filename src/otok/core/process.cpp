// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <system_error>
#include <utility>

#ifdef _WIN32
#   include <cwchar>
#   include <windows.h>
#else
#   ifdef __APPLE__
#       include <Availability.h>
#       include <mach-o/dyld.h>
#   endif
#   include <cstring>
#   include <cerrno>
#   include <fcntl.h>
#   include <signal.h>
#   include <spawn.h>
#   include <sys/wait.h>
#   include <unistd.h>
#endif

#include "otok/core/fs.hpp"
#include "otok/core/sandbox.hpp"
#include "otok/core/virtual_drives.hpp"

#ifndef _WIN32
extern char** environ;
#endif

namespace otok {

namespace {

#ifdef _WIN32
constexpr char path_list_separator = ';';
#else
constexpr char path_list_separator = ':';
#endif

bool has_path_separator(std::string_view program)
{
#ifdef _WIN32
    return program.find_first_of("/\\") != std::string_view::npos;
#else
    return program.find('/') != std::string_view::npos;
#endif
}

#ifdef _WIN32
std::string lowercase(std::string_view s)
{
    std::string out{s};
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
#endif

// Variable names are case-insensitive on Windows.
env_vars::const_iterator find_variable(env_vars const& env, std::string_view name)
{
#ifdef _WIN32
    return std::ranges::find_if(env, [&](auto const& v) { return lowercase(v.first) == lowercase(name); });
#else
    return env.find(std::string{name});
#endif
}

bool is_program_file(std::filesystem::path const& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
#ifndef _WIN32
    if (::access(path.c_str(), X_OK) != 0) return false;
#endif
    return true;
}

std::filesystem::path with_default_extension(std::filesystem::path path)
{
#ifdef _WIN32
    if (!path.has_extension()) path += L".exe";
#endif
    return path;
}

result<void> check_spec(process_spec const& spec)
{
    if (spec.argv.empty()) return fail("no program to run");
    for (std::string const& arg : spec.argv) {
        if (arg.find('\0') != std::string::npos) return fail("an argument of '" + spec.argv[0] + "' contains a NUL character");
    }
    for (auto const& [name, value] : spec.env) {
        if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos) {
            return fail("'" + name + "' is not a valid environment variable name");
        }
        if (value.find('\0') != std::string::npos) return fail("the environment variable '" + name + "' contains a NUL character");
    }
#ifdef _WIN32
    for (auto it = spec.env.begin(); it != spec.env.end(); ++it) {
        for (auto next = std::next(it); next != spec.env.end(); ++next) {
            if (lowercase(it->first) == lowercase(next->first)) {
                return fail("the environment variables '" + it->first + "' and '" + next->first + "' differ only in case");
            }
        }
    }
#endif
    std::error_code ec;
    if (!std::filesystem::is_directory(spec.cwd, ec)) {
        return fail("the working directory '" + path_to_utf8(spec.cwd) + "' does not exist");
    }
    return {};
}

// Appends what the child wrote to the captured output, keeping at most `limit` bytes.
void append_captured(process_exit& e, char const* data, std::size_t size, std::size_t limit)
{
    std::size_t const room = limit - std::min(limit, e.output.size());
    if (size > room) e.output_truncated = true;
    e.output.append(data, std::min(size, room));
}

#ifdef _WIN32

std::string last_error_message()
{
    return std::system_category().message(static_cast<int>(::GetLastError()));
}

std::wstring widen(std::string_view s)
{
    if (s.empty()) return {};
    int const n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

class handle
{
public:
    handle() = default;
    explicit handle(HANDLE h) noexcept : h_{h == INVALID_HANDLE_VALUE ? nullptr : h} {}
    ~handle() { reset(); }
    handle(handle const&) = delete;
    handle& operator=(handle const&) = delete;
    handle& operator=(handle&& other) noexcept
    {
        reset();
        h_ = std::exchange(other.h_, nullptr);
        return *this;
    }

    HANDLE get() const noexcept { return h_; }
    HANDLE* out() noexcept { reset(); return &h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }
    void reset() noexcept
    {
        if (h_) ::CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

// The job of every process otok starts (docs/PLAN.md decision 74): the kernel closes it when otok ends, however it ends
// (Ctrl+C, a crash, the task manager), and ends what is left of its builds with it, children of children too, which
// have no console to hear Ctrl+C. A program that crashes ends at once, without the dialog of Windows Error Reporting
// holding a build. Null where the job cannot be made: processes then run outside one.
HANDLE children_job()
{
    static HANDLE const job = [] {
        HANDLE const j = ::CreateJobObjectW(nullptr, nullptr);
        if (!j) return HANDLE{nullptr};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (!::SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
            ::CloseHandle(j);
            return HANDLE{nullptr};
        }
        return j;
    }();
    return job;
}

// "NAME=value\0..." sorted case-insensitively, as CreateProcessW requires, ending with "\0\0".
std::wstring environment_block(env_vars const& env)
{
    std::vector<std::wstring> entries;
    for (auto const& [name, value] : env) entries.push_back(widen(name) + L"=" + widen(value));
    std::ranges::sort(entries, [](std::wstring const& a, std::wstring const& b) {
        std::wstring const an = a.substr(0, a.find(L'=')), bn = b.substr(0, b.find(L'='));
        return ::CompareStringOrdinal(an.data(), static_cast<int>(an.size()), bn.data(), static_cast<int>(bn.size()), TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (std::wstring const& e : entries) {
        block += e;
        block += L'\0';
    }
    block += L'\0';
    if (entries.empty()) block += L'\0';
    return block;
}

result<process_exit> spawn(std::filesystem::path const& program, process_spec const& spec)
{
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

    handle input{::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr)};
    if (!input) return fail("cannot open the null device: " + last_error_message());

    handle log;
    if (spec.log.empty()) {
        log = handle{::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr)};
    } else {
        // written at its end: the programs of MSYS2 and Cygwin write nothing to a handle opened for appending only
        log = handle{::CreateFileW(spec.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (log) ::SetFilePointerEx(log.get(), LARGE_INTEGER{}, nullptr, FILE_END);
    }
    if (!log) return fail("cannot open the log '" + path_to_utf8(spec.log) + "': " + last_error_message());

    handle capture_read, capture_write;
    if (spec.capture) {
        if (!::CreatePipe(capture_read.out(), capture_write.out(), nullptr, 0)
            || !::SetHandleInformation(capture_write.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            return fail("cannot create a pipe: " + last_error_message());
        }
    }

    // otok's own streams, as inheritable copies; the null device where otok has none
    handle own_in, own_out, own_err;
    if (spec.inherit_io) {
        auto const copy = [&](DWORD which, handle& to, HANDLE fallback) {
            HANDLE const h = ::GetStdHandle(which);
            if (h == nullptr || h == INVALID_HANDLE_VALUE
                || !::DuplicateHandle(::GetCurrentProcess(), h, ::GetCurrentProcess(), to.out(), 0, TRUE, DUPLICATE_SAME_ACCESS)) {
                ::DuplicateHandle(::GetCurrentProcess(), fallback, ::GetCurrentProcess(), to.out(), 0, TRUE, DUPLICATE_SAME_ACCESS);
            }
        };
        copy(STD_INPUT_HANDLE, own_in, input.get());
        copy(STD_OUTPUT_HANDLE, own_out, log.get());
        copy(STD_ERROR_HANDLE, own_err, log.get());
    }

    // Only these handles reach the child, not every inheritable handle of otok: another thread's
    // pipe held open by an unrelated child would never report its end.
    std::vector<HANDLE> inherited{input.get(), log.get()};
    if (spec.capture) inherited.push_back(capture_write.get());
    if (spec.inherit_io) inherited.insert(inherited.end(), {own_in.get(), own_out.get(), own_err.get()});

    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> list_storage(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
    if (!::InitializeProcThreadAttributeList(list, 1, 0, &size)) {
        return fail("cannot prepare the process attributes: " + last_error_message());
    }
    struct list_guard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~list_guard() { ::DeleteProcThreadAttributeList(list); }
    } guard{list};
    if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
            inherited.size() * sizeof(HANDLE), nullptr, nullptr)) {
        return fail("cannot prepare the process attributes: " + last_error_message());
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = spec.inherit_io ? own_in.get() : input.get();
    si.StartupInfo.hStdOutput = spec.inherit_io ? own_out.get() : spec.capture ? capture_write.get() : log.get();
    si.StartupInfo.hStdError = spec.inherit_io ? own_err.get() : log.get();
    si.lpAttributeList = list;

    std::string command_line;
    for (std::string const& arg : spec.argv) {
        if (!command_line.empty()) command_line += ' ';
        command_line += detail::quote_windows_argument(arg);
    }
    std::wstring wide_command_line = widen(command_line);
    std::wstring env = environment_block(spec.env);

    PROCESS_INFORMATION pi{};
    if (spec.sandbox) {
        // in the AppContainer, with otok-sandbox.dll (docs/PLAN.md decision 103)
        STARTUPINFOW plain = si.StartupInfo;
        plain.cb = sizeof plain;
        std::vector<void*> const handles{inherited.begin(), inherited.end()};
        if (auto ok = detail::create_sandboxed_process(*spec.sandbox, program, wide_command_line, env, spec.cwd, handles, &plain, &pi); !ok)
            return std::unexpected{ok.error()};
    } else if (!::CreateProcessW(program.c_str(), wide_command_line.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, env.data(),
            spec.cwd.c_str(), &si.StartupInfo, &pi)) {
        return fail("cannot start '" + path_to_utf8(program) + "': " + last_error_message());
    }
    handle process{pi.hProcess};
    handle thread{pi.hThread};
    // otok's device map, before it runs: a map is not inherited (core/virtual_drives)
    if (auto ok = virtual_drives::give(process.get()); !ok) {
        ::TerminateProcess(process.get(), 1);
        return fail("cannot start '" + path_to_utf8(program) + "': " + ok.error().message);
    }
    // in the job before it runs, so that whatever it starts is in it too
    if (HANDLE const job = children_job()) ::AssignProcessToJobObject(job, process.get());
    if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        std::string const why = last_error_message();
        ::TerminateProcess(process.get(), 1);
        return fail("cannot start '" + path_to_utf8(program) + "': " + why);
    }
    capture_write.reset();  // the pipe ends when the child (and its children) close their copies

    process_exit e;
    if (spec.capture) {
        std::array<char, 64 * 1024> buffer;
        DWORD read = 0;
        while (::ReadFile(capture_read.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
            append_captured(e, buffer.data(), read, spec.capture_limit);
        }
    }
    ::WaitForSingleObject(process.get(), INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(process.get(), &code);
    e.code = static_cast<int>(code);
    return e;
}

#else

std::string errno_message(int err)
{
    return std::generic_category().message(err);
}

class fd
{
public:
    fd() = default;
    explicit fd(int f) noexcept : f_{f} {}
    ~fd() { reset(); }
    fd(fd const&) = delete;
    fd& operator=(fd const&) = delete;
    fd& operator=(fd&& other) noexcept
    {
        reset();
        f_ = std::exchange(other.f_, -1);
        return *this;
    }

    int get() const noexcept { return f_; }
    explicit operator bool() const noexcept { return f_ >= 0; }
    void reset() noexcept
    {
        if (f_ >= 0) ::close(f_);
        f_ = -1;
    }

private:
    int f_ = -1;
};

result<process_exit> spawn(std::filesystem::path const& program, process_spec const& spec)
{
    fd input{::open("/dev/null", O_RDONLY | O_CLOEXEC)};
    if (!input) return fail("cannot open /dev/null: " + errno_message(errno));

    fd log;
    if (spec.log.empty()) {
        log = fd{::open("/dev/null", O_WRONLY | O_CLOEXEC)};
    } else {
        log = fd{::open(spec.log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)};
    }
    if (!log) return fail("cannot open the log '" + path_to_utf8(spec.log) + "': " + errno_message(errno));

    fd capture_read, capture_write;
    if (spec.capture) {
        int fds[2];
#ifdef __APPLE__
        // No pipe2 on macOS; the race with a concurrent fork is harmless for our own children
        // (POSIX_SPAWN_CLOEXEC_DEFAULT below).
        if (::pipe(fds) != 0) return fail("cannot create a pipe: " + errno_message(errno));
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#else
        if (::pipe2(fds, O_CLOEXEC) != 0) return fail("cannot create a pipe: " + errno_message(errno));
#endif
        capture_read = fd{fds[0]};
        capture_write = fd{fds[1]};
    }

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    struct actions_guard
    {
        posix_spawn_file_actions_t* a;
        ~actions_guard() { ::posix_spawn_file_actions_destroy(a); }
    } actions_cleanup{&actions};
    if (spec.inherit_io) {
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        for (int std_fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) ::posix_spawn_file_actions_addinherit_np(&actions, std_fd);
#endif
    } else {
        ::posix_spawn_file_actions_adddup2(&actions, input.get(), STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, spec.capture ? capture_write.get() : log.get(), STDOUT_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, log.get(), STDERR_FILENO);
    }
    // the jobserver: blocking descriptors of their own, as make reads them, above those the standard ones come from
    fd jobserver_read, jobserver_write;
    if (!spec.jobserver_fifo.empty()) {
        for (fd* f : {&jobserver_read, &jobserver_write}) {
            fd const opened{::open(spec.jobserver_fifo.c_str(), O_RDWR | O_CLOEXEC)};
            if (!opened) return fail("cannot open the jobserver fifo '" + path_to_utf8(spec.jobserver_fifo) + "': " + errno_message(errno));
            *f = fd{::fcntl(opened.get(), F_DUPFD_CLOEXEC, 5)};
            if (!*f) return fail("cannot open the jobserver fifo '" + path_to_utf8(spec.jobserver_fifo) + "': " + errno_message(errno));
        }
        ::posix_spawn_file_actions_adddup2(&actions, jobserver_read.get(), 3);
        ::posix_spawn_file_actions_adddup2(&actions, jobserver_write.get(), 4);
    }
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
    ::posix_spawn_file_actions_addchdir(&actions, spec.cwd.c_str());  // POSIX.1-2024
#else
    ::posix_spawn_file_actions_addchdir_np(&actions, spec.cwd.c_str());
#endif

    posix_spawnattr_t attributes;
    ::posix_spawnattr_init(&attributes);
    struct attributes_guard
    {
        posix_spawnattr_t* a;
        ~attributes_guard() { ::posix_spawnattr_destroy(a); }
    } attributes_cleanup{&attributes};
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    // Only the descriptors set up above reach the child.
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    ::posix_spawnattr_setflags(&attributes, flags);
    sigset_t signals;
    sigemptyset(&signals);
    ::posix_spawnattr_setsigmask(&attributes, &signals);
    sigfillset(&signals);
    ::posix_spawnattr_setsigdefault(&attributes, &signals);

    std::vector<char*> argv;
    for (std::string const& arg : spec.argv) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> env_entries;
    for (auto const& [name, value] : spec.env) env_entries.push_back(name + "=" + value);
    std::vector<char*> envp;
    for (std::string& entry : env_entries) envp.push_back(entry.data());
    envp.push_back(nullptr);

    pid_t pid = 0;
    if (int const err = ::posix_spawn(&pid, program.c_str(), &actions, &attributes, argv.data(), envp.data()); err != 0) {
        return fail("cannot start '" + path_to_utf8(program) + "': " + errno_message(err));
    }
    capture_write.reset();  // the pipe ends when the child (and its children) close their copies

    process_exit e;
    if (spec.capture) {
        std::array<char, 64 * 1024> buffer;
        for (;;) {
            ssize_t const n = ::read(capture_read.get(), buffer.data(), buffer.size());
            if (n > 0) {
                append_captured(e, buffer.data(), static_cast<std::size_t>(n), spec.capture_limit);
            } else if (n == 0 || errno != EINTR) {
                break;
            }
        }
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return fail("cannot wait for '" + path_to_utf8(program) + "': " + errno_message(errno));
    }
    if (WIFEXITED(status)) {
        e.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        e.signal = WTERMSIG(status);
    }
    return e;
}

#endif

}

result<std::filesystem::path> find_program(std::string_view program, env_vars const& env, std::filesystem::path const& cwd)
{
    if (program.empty()) return fail("no program to run");
    if (has_path_separator(program)) {
        std::filesystem::path path = with_default_extension(path_from_utf8(program));
        if (path.is_relative()) path = cwd / path;
        path = path.lexically_normal();
        if (!is_program_file(path)) return fail("'" + path_to_utf8(path) + "' is not an executable file");
        return path;
    }
    auto const path_variable = find_variable(env, "PATH");
    if (path_variable != env.end()) {
        std::string_view rest = path_variable->second;
        while (!rest.empty()) {
            std::size_t const end = std::min(rest.find(path_list_separator), rest.size());
            std::string_view const dir = rest.substr(0, end);
            rest.remove_prefix(std::min(end + 1, rest.size()));
            // an empty entry means the working directory to a shell; here it means nothing
            if (dir.empty()) continue;
            std::filesystem::path const candidate = with_default_extension(path_from_utf8(dir) / path_from_utf8(program));
            if (candidate.is_absolute() && is_program_file(candidate)) return candidate.lexically_normal();
        }
    }
    return fail("cannot find '" + std::string{program} + "' in PATH");
}

result<process_exit> run_process(process_spec const& spec)
{
    if (auto ok = check_spec(spec); !ok) return std::unexpected{ok.error()};
    auto program = find_program(spec.argv[0], spec.env, spec.cwd);
    if (!program) return std::unexpected{program.error()};
    return spawn(*program, spec);
}

env_vars current_environment()
{
    env_vars env;
#ifdef _WIN32
    wchar_t* block = ::GetEnvironmentStringsW();
    for (wchar_t const* p = block; p && *p; p += std::wcslen(p) + 1) {
        std::string const entry = path_to_utf8(std::filesystem::path{p});
        std::size_t const eq = entry.find('=', 1);  // hidden "=C:=C:\…" entries start with '='
        if (entry.starts_with('=') || eq == std::string::npos) continue;
        env.emplace(entry.substr(0, eq), entry.substr(eq + 1));
    }
    if (block) ::FreeEnvironmentStringsW(block);
#else
    for (char** p = environ; *p; ++p) {
        std::string_view const entry{*p};
        std::size_t const eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        env.emplace(std::string{entry.substr(0, eq)}, std::string{entry.substr(eq + 1)});
    }
#endif
    return env;
}

unsigned long current_process_id() noexcept
{
#ifdef _WIN32
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

std::string_view host_machine() noexcept
{
#if defined(_WIN32)
#define OTOK_HOST_OS "windows"
#elif defined(__APPLE__)
#define OTOK_HOST_OS "macos"
#else
#define OTOK_HOST_OS "linux"
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
    return OTOK_HOST_OS "-arm64";
#else
    return OTOK_HOST_OS "-x86_64";
#endif
#undef OTOK_HOST_OS
}

std::filesystem::path current_executable()
{
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    DWORD const n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) return {};
    buffer.resize(n);
    return std::filesystem::path{buffer};
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::strlen(buffer.c_str()));
    auto const resolved = std::filesystem::canonical(buffer, ec);
    return ec ? std::filesystem::path{} : resolved;
#else
    auto const resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : resolved;
#endif
}

std::string describe_exit(process_exit const& e)
{
    if (e.signal != 0) return "terminated by signal " + std::to_string(e.signal);
    if (e.code < 0) {
        // Windows: an NTSTATUS such as 0xC0000005 (access violation)
        std::array<char, 16> hex;
        std::snprintf(hex.data(), hex.size(), "0x%08X", static_cast<unsigned>(e.code));
        return std::string{"exit code "} + hex.data();
    }
    return "exit code " + std::to_string(e.code);
}

namespace detail {

std::string quote_windows_argument(std::string_view arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) return std::string{arg};
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char const c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        // backslashes are literal unless they precede a quote: then each is doubled and the quote escaped
        out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        out += c;
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');  // before the closing quote
    out += '"';
    return out;
}

}

}
