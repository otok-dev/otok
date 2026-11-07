// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>

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

}
