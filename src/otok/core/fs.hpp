// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstddef>
#include <filesystem>
#include <system_error>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "otok/core/error.hpp"

// File system access. Paths are UTF-8 inside otok and converted at this boundary, so non-ASCII
// paths work on Windows, where the native encoding is UTF-16.
namespace otok {

std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(std::filesystem::path const& path);

// The path as the file functions of the OS take it beyond 260 characters: on Windows the extended-length
// form of the absolute path (`\\?\C:\…`, `\\?\UNC\server\…`), so trees with deep paths (Boost's
// documentation) can be copied and removed; elsewhere the path itself.
std::filesystem::path long_path(std::filesystem::path const& path);

// std::filesystem::rename that waits out another process holding a file in `from` open: Windows refuses meanwhile
// with access denied (an indexer or an antivirus reading what was just unpacked, another thread replacing the same
// file), so there it is tried again for about five seconds, waiting longer each time; elsewhere once.
void rename_path(std::filesystem::path const& from, std::filesystem::path const& to, std::error_code& ec);

// The whole file as bytes.
result<std::string> read_file(std::filesystem::path const& path);

// Replaces the file with `content`: written next to it under a temporary name, then renamed over it,
// so readers see the old or the new file, never a partial one.
result<void> write_file_atomic(std::filesystem::path const& path, std::string_view content);

// Writes `content` to `path` unless the file has it already, so that the builds reading it see no new file (the consumer files
// of an output, otok-toml.md 4.2); true when it was written.
result<bool> write_file_if_changed(std::filesystem::path const& path, std::string_view content);

// Copies the directory tree `from` to `to`, which must not exist (its parents are created): files with their times and
// permissions, symbolic links as links. On macOS a `clonefile` (APFS copy-on-write) when the file
// system supports it (docs/formats/cache.md 5).
result<void> copy_tree(std::filesystem::path const& from, std::filesystem::path const& to);

// The first regular file, as a path relative to the roots with `/`, that only one of two directory trees
// has or that differs between them; empty when they hold the same files. Reads the files a piece at a
// time, so large trees (Boost's headers) are not held in memory.
std::string first_difference(std::filesystem::path const& a, std::filesystem::path const& b);

struct generated_files_written
{
    std::size_t written = 0;           // new or changed files
    std::size_t removed = 0;           // files no longer generated
    std::size_t unchanged = 0;
};

// Writes generated files under `root` (`files`: paths relative to it with `/`, and their contents), each only when
// its contents differ, so web servers keep the `ETag`s they derive from file times; then removes the files under
// `owned` (directories or files relative to `root`) that `files` does not hold, and the directories left empty.
result<generated_files_written> write_generated_files(std::filesystem::path const& root, std::map<std::string, std::string> const& files,
    std::span<std::string const> owned);

}
