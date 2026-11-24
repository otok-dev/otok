// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "otok/core/error.hpp"

// Unpacking source archives with libarchive (docs/PLAN.md decision 14; recipe-toml.md 4).
namespace otok {

enum class top_level
{
    strip_single,  // a single top-level directory is stripped (source archives, recipe-toml.md 4)
    keep           // the archive root is the tree root (a git tree exported with `git archive`)
};

// Unpacks `archive_file` (tar with gzip, bzip2, xz or zstd, or zip; recognized by content) into
// `destination`, which must not exist yet. Archives come from strangers, so entries with absolute
// paths, `..`, backslashes, links leaving the tree and special files (devices, pipes) are errors.
// File times and the executable bit are kept. On failure nothing is left at `destination`.
result<void> extract_archive(std::filesystem::path const& archive_file, std::filesystem::path const& destination,
    top_level root = top_level::strip_single);

// The time of every entry of the archives otok writes: 2000-01-01T00:00:00Z (cache.md 12.1).
inline constexpr std::int64_t archive_time = 946684800;

struct archive_written
{
    std::string sha256;
    std::uintmax_t size = 0;
};

// Writes `names` of `root` (files, links or directories with all they hold) into `archive_file` as cache.md 12.1 lays out the
// archive of a slice entry: a POSIX pax tar compressed with zstd at level 10, paths sorted bytewise, one time, owner and group
// 0 without names, modes 0755 and 0644 (0755 for files with the executable bit, never on Windows), links as links. The same
// tree gives the same bytes. Names that do not exist are left out; a special file is an error.
result<archive_written> write_tree_archive(std::filesystem::path const& root, std::span<std::string const> names, std::filesystem::path const& archive_file);

}
