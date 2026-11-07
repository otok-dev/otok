// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/core/tree_hash.hpp"

// Limits of a recipe directory (docs/formats/recipe-toml.md 15): text only, small, portable names.
namespace otok {

inline constexpr std::uint64_t recipe_tree_max_bytes = 1024 * 1024;
inline constexpr std::size_t recipe_tree_max_files = 256;

// Checks the files of `dir` as listed by list_tree() (which already rejects symbolic links).
// `level` is error for `otok publish` and index CI, warning for local and git recipes.
void check_recipe_tree(std::filesystem::path const& dir, std::vector<tree_file> const& files, severity level, diagnostics& diags);

// Valid UTF-8 without NUL bytes.
bool is_text(std::string_view bytes) noexcept;

}
