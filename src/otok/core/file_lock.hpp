// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include "otok/core/error.hpp"

// Advisory locks on files (docs/formats/cache.md 6): `flock` on macOS and Linux, `LockFileEx` on
// Windows. The operating system releases them when the process ends, so a crash leaves no stale lock.
// Two locks on the same file conflict also within one process (each lock opens the file anew).
namespace otok {

enum class lock_mode
{
    exclusive,  // one holder
    shared      // any number of shared holders, but no exclusive one
};

class file_lock
{
public:
    // Takes the lock, creating the file if needed; waits while a conflicting holder has it, calling
    // `on_wait` once before waiting.
    static result<file_lock> acquire(std::filesystem::path const& path, std::function<void()> const& on_wait = {},
        lock_mode mode = lock_mode::exclusive);

    // Takes the lock if no conflicting holder has it; nothing otherwise.
    static result<std::optional<file_lock>> try_acquire(std::filesystem::path const& path, lock_mode mode = lock_mode::exclusive);

    file_lock(file_lock&& other) noexcept;
    file_lock& operator=(file_lock&& other) noexcept;
    file_lock(file_lock const&) = delete;
    file_lock& operator=(file_lock const&) = delete;
    ~file_lock();

    // Releases early; the destructor does the same.
    void release() noexcept;

private:
    file_lock() = default;

#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}
