// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "otok/core/error.hpp"

// The tree hash of docs/formats/recipe-hash.md: `recipe-sha256` and the hash of a local source.
namespace otok {

// Symbolic links are errors in a recipe tree and recorded by target in a source tree (recipe-hash.md 1).
enum class tree_links
{
    reject,
    record
};

struct tree_file
{
    std::string path;          // relative to the root, `/` separators
    std::uint64_t size = 0;
    std::string sha256;        // of the contents; of the target text for a link
    std::optional<std::string> link_target;  // a symbolic link: its target, `/` separators
};

// Every regular file (and, with tree_links::record, every symbolic link) under `root`, sorted by path
// as bytes; `.git` at the root is skipped. Other non-regular files, paths with a newline or a
// backslash, and link targets that are absolute or leave the tree are errors.
result<std::vector<tree_file>> list_tree(std::filesystem::path const& root, tree_links links = tree_links::reject);

// `<sha256>  <path>\n` per file (the text `shasum -a 256` prints), `symlink:<sha256>  <path>\n` per link.
std::string tree_listing(std::vector<tree_file> const& files);

// SHA-256 of the listing.
std::string tree_hash(std::vector<tree_file> const& files);
result<std::string> tree_hash(std::filesystem::path const& root, tree_links links = tree_links::reject);

}
