// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "otok/core/diagnostics.hpp"

// `otok.dev.toml`, format 1 (docs/formats/otok-dev-toml.md): the libraries the developer works on, built
// incrementally from working trees (docs/PLAN.md decision 67). This module reads what can be checked
// without the lock and the recipes; the commands check the rest (otok-dev-toml.md 5).
namespace otok {

// `[deps.<library>]` (otok-dev-toml.md 2). Paths as written, relative to the file's directory.
struct dev_library
{
    std::string library;
    std::optional<std::string> source;
    std::map<std::string, std::string> resources;   // resource name → path
    std::optional<std::string> recipe;
    bool quiet = false;                             // `quiet`: the build's output not shown
    source_position where;
};

struct dev_manifest
{
    std::map<std::string, dev_library> deps;

    dev_library const* find(std::string_view library) const
    {
        auto const it = deps.find(std::string{library});
        return it == deps.end() ? nullptr : &it->second;
    }
};

inline constexpr std::string_view dev_manifest_name = "otok.dev.toml";

std::optional<dev_manifest> parse_dev_manifest(std::string_view content, std::string const& file, diagnostics& diags);

// Reads `path`; a missing file is an empty manifest. Nothing after an error.
std::optional<dev_manifest> load_dev_manifest(std::filesystem::path const& path, diagnostics& diags);

}
