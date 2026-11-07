// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <string_view>

// Paths as written in otok's files (docs/formats/README.md 6): `/` as separator on every host,
// relative to the directory of the file that contains them.
namespace otok {

enum class file_path_kind
{
    inside,   // relative and stays below the file's directory: `recipes/foo`, `a/../b`
    outside,  // leaves it (`../otok-recipes/foo`) or is absolute (`/Users/me/foo`, `C:/src/foo`)
    invalid   // empty, contains a backslash or a NUL, or names the directory itself (`.`, `a/..`)
};

file_path_kind classify_file_path(std::string_view path) noexcept;

}
