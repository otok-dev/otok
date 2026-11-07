// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"

// The work directories of dev slices, kept between builds (docs/formats/otok-dev-toml.md 4, docs/PLAN.md
// decision 67): `source/` synchronized from working trees rather than copied afresh, and `dev.toml`, the
// record of what was synchronized.
namespace otok {

// A file's size and modification time, as the host's file system gives them.
struct file_stamp
{
    std::uintmax_t size = 0;
    std::int64_t time = 0;

    friend bool operator==(file_stamp const&, file_stamp const&) = default;
};

// `dev.toml` (otok-dev-toml.md 4.1).
struct dev_record
{
    std::string key;                                // the dev key's description; another one empties build/
    std::string patches;                            // the SHA-256 of the patches as last applied; empty: none
    std::map<std::string, file_stamp> files;        // path in source/ → the stamp of the file it was copied from
};

// Nothing when the file is missing or not a record: the work directory starts afresh.
std::optional<dev_record> read_dev_record(std::filesystem::path const& file);
result<void> write_dev_record(std::filesystem::path const& file, dev_record const& record);

// A tree (or a file) synchronized into source/ at `dest` (`/`-separated, "" for the root).
struct synced_tree
{
    std::filesystem::path from;
    std::string dest;
    std::optional<std::vector<std::string>> files;  // the files of `from` to take, `/`-separated; nothing: every file
};

// Synchronizes `trees` into `source` (otok-dev-toml.md 4): a file is copied, with its modification time, when its
// stamp differs from that of `old`, when its copy is missing, or when `force` names it; a file `old` lists that no tree
// has now is removed; files no tree has are left. `.git` entries are skipped. Fills `now.files` and returns the files
// copied; `origins` receives where each file came from.
result<std::set<std::string>> sync_trees(std::vector<synced_tree> const& trees, std::filesystem::path const& source, dev_record const& old,
    dev_record& now, std::set<std::string> const& force, std::map<std::string, std::filesystem::path>& origins);

// The files a patch in git's format touches (`--- a/…`, `+++ b/…`), `/`-separated.
std::set<std::string> patched_files(std::string_view patch);

}
