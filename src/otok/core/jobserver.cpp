// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/jobserver.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#   include <windows.h>
#   include <sddl.h>
#   include <mutex>
#else
#   include <cerrno>
#   include <fcntl.h>
#   include <poll.h>
#   include <sys/stat.h>
#   include <unistd.h>
#endif

#include "otok/core/file_lock.hpp"
#include "otok/core/fs.hpp"

namespace otok {

namespace fs = std::filesystem;

struct jobserver::state
{
    std::string makeflags;
#ifdef _WIN32
    HANDLE semaphore = nullptr;

    ~state()
    {
        if (semaphore) ::CloseHandle(semaphore);
    }
#else
    fs::path fifo;
    int fd = -1;

    ~state()
    {
        if (fd >= 0) ::close(fd);
        if (!fifo.empty()) ::unlink(fifo.c_str());
    }
#endif
};

jobserver::jobserver(std::unique_ptr<state> s) : state_{std::move(s)} {}
jobserver::jobserver(jobserver&&) noexcept = default;
jobserver& jobserver::operator=(jobserver&&) noexcept = default;
jobserver::~jobserver() = default;

result<jobserver> jobserver::create(std::int64_t jobs, fs::path const& dir)
{
    if (jobs < 1) jobs = 1;
    return create(jobs, jobs - 1, dir, "jobserver");
}

#ifdef _WIN32
namespace {

// The SID of the AppContainer granted the semaphores (use_container_jobservers), as text; empty: none.
struct container_grant
{
    std::mutex mutex;
    std::wstring sid;
};

container_grant& jobserver_container()
{
    static container_grant c;
    return c;
}

}
#endif

void use_container_jobservers([[maybe_unused]] std::string const& container)
{
#ifdef _WIN32
    auto& c = jobserver_container();
    std::lock_guard lock{c.mutex};
    c.sid = path_from_utf8(container).wstring();
#endif
}

result<jobserver> jobserver::create(std::int64_t jobs, std::int64_t tokens, fs::path const& dir, std::string const& name)
{
    if (jobs < 1) jobs = 1;
    auto s = std::make_unique<state>();
#ifdef _WIN32
    (void)dir;
    std::random_device random;
    std::wstring const semaphore = L"otok-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + path_from_utf8(name).wstring() + L"-"
        + std::to_wstring(random());
    // granted to the AppContainer when builds run in it: its programs find it through otok-sandbox.dll, which looks in
    // the session's namespace for a semaphore its own lacks
    PSECURITY_DESCRIPTOR sd = nullptr;
    {
        auto& c = jobserver_container();
        std::lock_guard lock{c.mutex};
        if (!c.sid.empty()) {
            std::wstring const sddl = L"D:(A;;GA;;;SY)(A;;GA;;;OW)(A;;GA;;;" + c.sid + L")";
            ::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
        }
    }
    SECURITY_ATTRIBUTES attributes{sizeof attributes, sd, FALSE};
    // as many as clients may run, at least one: the maximum must be positive even when the pool is empty
    s->semaphore = ::CreateSemaphoreW(sd ? &attributes : nullptr, static_cast<LONG>(tokens),
        static_cast<LONG>(std::max<std::int64_t>({tokens, jobs, 1})), semaphore.c_str());
    if (sd) ::LocalFree(sd);
    if (!s->semaphore) return fail("cannot create the jobserver semaphore: error " + std::to_string(::GetLastError()));
    s->makeflags = "-j" + std::to_string(jobs) + " --jobserver-auth=" + path_to_utf8(fs::path{semaphore});
#else
    s->fifo = dir / path_from_utf8(name);
    ::unlink(s->fifo.c_str());
    if (::mkfifo(s->fifo.c_str(), 0600) != 0) {
        int const err = errno;
        s->fifo.clear();
        return fail("cannot create the jobserver fifo in '" + path_to_utf8(dir) + "': " + std::generic_category().message(err));
    }
    // read and write: it never reports the end, and reads never block (try_acquire polls)
    s->fd = ::open(s->fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0) return fail("cannot open the jobserver fifo: " + std::generic_category().message(errno));
    for (std::int64_t i = 0; i < tokens; ++i) {
        if (::write(s->fd, "+", 1) != 1) return fail("cannot fill the jobserver fifo: " + std::generic_category().message(errno));
    }
    s->makeflags = "-j" + std::to_string(jobs) + " --jobserver-auth=fifo:" + path_to_utf8(s->fifo);
#endif
    return jobserver{std::move(s)};
}

result<jobserver> jobserver::connect(std::string_view auth)
{
    auto s = std::make_unique<state>();
    s->makeflags = "--jobserver-auth=" + std::string{auth};
#ifdef _WIN32
    s->semaphore = ::OpenSemaphoreW(SEMAPHORE_ALL_ACCESS, FALSE, path_from_utf8(auth).c_str());
    if (!s->semaphore) return fail("cannot open the jobserver semaphore '" + std::string{auth} + "': error " + std::to_string(::GetLastError()));
#else
    if (!auth.starts_with("fifo:")) return fail("the jobserver '" + std::string{auth} + "' is not a fifo");
    fs::path const fifo = path_from_utf8(auth.substr(5));
    s->fd = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0) return fail("cannot open the jobserver fifo '" + path_to_utf8(fifo) + "': " + std::generic_category().message(errno));
#endif
    return jobserver{std::move(s)};
}

result<process_exit> run_with_job(std::vector<std::string> argv, fs::path const& slot, std::string_view auth, env_vars env, fs::path const& cwd)
{
    process_spec spec;
    spec.argv = std::move(argv);
    spec.env = std::move(env);
    spec.cwd = cwd;
    spec.inherit_io = true;
    std::optional<jobserver> pool;
    if (!auth.empty()) {
        auto connected = jobserver::connect(auth);
        if (!connected) return std::unexpected{connected.error()};
        pool = std::move(*connected);
    }
    for (;;) {
        // the slice's own token first: no pool, or none of its tokens free, still runs one per slice
        if (slot.empty()) return run_process(spec);
        auto mine = file_lock::try_acquire(slot);
        if (!mine) return std::unexpected{mine.error()};
        if (*mine) return run_process(spec);
        if (!pool) {
            auto waited = file_lock::acquire(slot);
            if (!waited) return std::unexpected{waited.error()};
            return run_process(spec);
        }
        // a hundred launchers may wait at once: not too often, compilations take seconds
        if (pool->try_acquire(std::chrono::milliseconds{200})) {
            auto ran = run_process(spec);
            pool->release();
            return ran;
        }
    }
}

std::string const& jobserver::makeflags() const noexcept
{
    return state_->makeflags;
}

std::filesystem::path jobserver::fifo() const
{
#ifdef _WIN32
    return {};
#else
    return state_->fifo;
#endif
}

bool jobserver::try_acquire(std::chrono::milliseconds timeout)
{
#ifdef _WIN32
    return ::WaitForSingleObject(state_->semaphore, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
#else
    pollfd p{state_->fd, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(timeout.count())) <= 0) return false;
    char token = 0;
    return ::read(state_->fd, &token, 1) == 1;  // another client may have been faster
#endif
}

void jobserver::release()
{
#ifdef _WIN32
    ::ReleaseSemaphore(state_->semaphore, 1, nullptr);
#else
    [[maybe_unused]] auto const written = ::write(state_->fd, "+", 1);
#endif
}

namespace {

// "4.4.1" → {4, 4}; nothing parsed → {0, 0}.
std::pair<int, int> major_minor(std::string_view text)
{
    int major = 0, minor = 0;
    char const* end = text.data() + text.size();
    auto [p, ec] = std::from_chars(text.data(), end, major);
    if (ec != std::errc{}) return {0, 0};
    if (p != end && *p == '.') std::from_chars(p + 1, end, minor);
    return {major, minor};
}

}

jobserver_protocol detail::make_jobserver_protocol(std::string_view version_output, bool descriptors)
{
    std::string_view const marker = "GNU Make ";
    std::size_t const at = version_output.find(marker);
    if (at == std::string_view::npos) return jobserver_protocol::none;
    // the make of MSYS2 or Cygwin on Windows is a POSIX program: it takes a fifo, not otok's named semaphore
    if (version_output.find("-pc-cygwin") != std::string_view::npos || version_output.find("-pc-msys") != std::string_view::npos) {
        return jobserver_protocol::none;
    }
    auto const version = major_minor(version_output.substr(at + marker.size()));
    if (version >= std::pair{4, 4}) return jobserver_protocol::named;
    // older ones inherit the descriptors of a pipe, which a fifo opened twice stands for
    if (!descriptors || version == std::pair{0, 0}) return jobserver_protocol::none;
    return version >= std::pair{4, 2} ? jobserver_protocol::descriptors : jobserver_protocol::old_descriptors;
}

bool detail::ninja_understands_jobserver(std::string_view version_output)
{
    return major_minor(version_output) >= std::pair{1, 13};
}

jobserver_protocol jobserver_protocol_of(std::string_view tool, env_vars const& env)
{
    std::error_code ec;
    process_spec spec{{std::string{tool}, "--version"}, env, fs::temp_directory_path(ec), {}, true};
    auto exit = run_process(spec);
    if (!exit || !exit->success()) return jobserver_protocol::none;
    // `tool` may be the path of the program
    if (path_from_utf8(tool).stem() == "ninja") {
        return detail::ninja_understands_jobserver(exit->output) ? jobserver_protocol::named : jobserver_protocol::none;
    }
#ifdef _WIN32
    return detail::make_jobserver_protocol(exit->output, false);
#else
    return detail::make_jobserver_protocol(exit->output, true);
#endif
}

std::optional<inherited_jobserver> inherit_jobserver(std::string_view makeflags, jobserver_protocol protocol)
{
    // the fifo comes last: its path may have spaces
    std::string_view const marker = "--jobserver-auth=fifo:";
    std::size_t const at = makeflags.find(marker);
    if (at == std::string_view::npos) return std::nullopt;
    inherited_jobserver out{{}, path_from_utf8(makeflags.substr(at + marker.size()))};
    if (protocol == jobserver_protocol::old_descriptors) {
        // make before 4.2 takes a -j with a number as forced on it, and leaves the jobserver
        out.makeflags = "-j --jobserver-fds=3,4";
    } else {
        std::string_view jobs = makeflags.substr(0, at);
        jobs = jobs.substr(0, jobs.find(' '));
        out.makeflags = std::string{jobs.starts_with("-j") ? jobs : "-j"} + " --jobserver-auth=3,4";
    }
    return out;
}

}
