// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/file_lock.hpp"

#include <system_error>
#include <utility>

#ifdef _WIN32
#   include <windows.h>
#else
#   include <cerrno>
#   include <fcntl.h>
#   include <sys/file.h>
#   include <unistd.h>
#endif

#include "otok/core/fs.hpp"

namespace otok {

namespace {

enum class wait
{
    no,
    yes
};

#ifdef _WIN32

std::string last_error()
{
    return std::system_category().message(static_cast<int>(::GetLastError()));
}

// The handle on success, nullptr when busy (and not waiting), an error otherwise.
result<HANDLE> lock(std::filesystem::path const& path, lock_mode mode, wait w, std::function<void()> const& on_wait)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot open the lock '" + path_to_utf8(path) + "': " + last_error());
    DWORD const flags = mode == lock_mode::exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    OVERLAPPED o{};
    if (::LockFileEx(h, flags | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &o)) return h;
    if (::GetLastError() != ERROR_LOCK_VIOLATION) {
        std::string const message = last_error();
        ::CloseHandle(h);
        return fail("cannot lock '" + path_to_utf8(path) + "': " + message);
    }
    if (w == wait::no) {
        ::CloseHandle(h);
        return static_cast<HANDLE>(nullptr);
    }
    if (on_wait) on_wait();
    o = {};
    if (!::LockFileEx(h, flags, 0, 1, 0, &o)) {
        std::string const message = last_error();
        ::CloseHandle(h);
        return fail("cannot lock '" + path_to_utf8(path) + "': " + message);
    }
    return h;
}

#else

// The descriptor on success, -1 when busy (and not waiting), an error otherwise.
result<int> lock(std::filesystem::path const& path, lock_mode mode, wait w, std::function<void()> const& on_wait)
{
    int const fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    int const operation = mode == lock_mode::exclusive ? LOCK_EX : LOCK_SH;
    if (fd < 0) return fail("cannot open the lock '" + path_to_utf8(path) + "': " + std::generic_category().message(errno));
    if (::flock(fd, operation | LOCK_NB) == 0) return fd;
    if (errno != EWOULDBLOCK) {
        int const err = errno;
        ::close(fd);
        return fail("cannot lock '" + path_to_utf8(path) + "': " + std::generic_category().message(err));
    }
    if (w == wait::no) {
        ::close(fd);
        return -1;
    }
    if (on_wait) on_wait();
    while (::flock(fd, operation) != 0) {
        if (errno == EINTR) continue;
        int const err = errno;
        ::close(fd);
        return fail("cannot lock '" + path_to_utf8(path) + "': " + std::generic_category().message(err));
    }
    return fd;
}

#endif

}

result<file_lock> file_lock::acquire(std::filesystem::path const& path, std::function<void()> const& on_wait, lock_mode mode)
{
    auto h = lock(path, mode, wait::yes, on_wait);
    if (!h) return std::unexpected{h.error()};
    file_lock l;
#ifdef _WIN32
    l.handle_ = *h;
#else
    l.fd_ = *h;
#endif
    return l;
}

result<std::optional<file_lock>> file_lock::try_acquire(std::filesystem::path const& path, lock_mode mode)
{
    auto h = lock(path, mode, wait::no, {});
    if (!h) return std::unexpected{h.error()};
    file_lock l;
#ifdef _WIN32
    if (!*h) return std::optional<file_lock>{};
    l.handle_ = *h;
#else
    if (*h < 0) return std::optional<file_lock>{};
    l.fd_ = *h;
#endif
    return std::optional<file_lock>{std::move(l)};
}

file_lock::file_lock(file_lock&& other) noexcept
{
    *this = std::move(other);
}

file_lock& file_lock::operator=(file_lock&& other) noexcept
{
    if (this != &other) {
        release();
#ifdef _WIN32
        handle_ = std::exchange(other.handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
    }
    return *this;
}

file_lock::~file_lock()
{
    release();
}

void file_lock::release() noexcept
{
#ifdef _WIN32
    if (handle_) ::CloseHandle(static_cast<HANDLE>(handle_));  // closing releases the lock
    handle_ = nullptr;
#else
    if (fd_ >= 0) ::close(fd_);  // closing releases the flock
    fd_ = -1;
#endif
}

}
