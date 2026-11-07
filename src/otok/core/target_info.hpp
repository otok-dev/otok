// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <algorithm>
#include <any>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/environment.hpp"
#include "otok/core/toml_reader.hpp"

// What the platform-neutral core needs to know about a target OS to read the files that mention it
// (DESIGN principle 9): its platforms, how deployment versions apply to them, and readers for its
// own parts of recipe.toml. Each target module provides one; the core never names a platform itself.
namespace otok {

struct recipe;

struct platform_info
{
    std::string name;                         // "iossim"
    std::vector<std::string> architectures;   // supported, default first
    std::vector<std::string> deployment_keys; // keys of the deployment table that apply, most specific first
};

// A key of the project's target table that only the target reads (otok-toml.md 4): `stl` of `[android]`.
struct target_choice
{
    std::string key;                          // "stl"
    std::vector<std::string> values;          // the allowed values, the default first
    std::map<std::string, std::string> hints; // a value meant as another, for the message: "dynamic" → "shared"
    // A free value: `values` holds only the default, and `check` says what is wrong with a value ("must be …"),
    // nothing when it is good.
    std::function<std::optional<std::string>(std::string_view value)> check{};
    // It changes the packages, not the builds: in the package key as `choice.<key>` (cache-key.md 4), not the slice key.
    bool packaging = false;
};

// Lets a reader check files of the recipe directory. Without it (parsing text alone) file checks are skipped.
using recipe_file_check = std::function<bool(std::string_view relative_path, bool directory)>;

struct target_info
{
    std::string name;                         // "apple": the target's table in otok.toml and recipe.toml
    std::vector<platform_info> platforms;
    std::vector<std::string> deployment_keys; // every valid key of the deployment table
    // The environments it builds in (common 4.2), systems without architecture: {"linux", "", "clang-cl"}; and the
    // compilers that build the same cross build on every host, which may be written `*/<compiler>` ("ndk").
    std::vector<build_environment> environments;
    std::vector<std::string> any_host_compilers;
    // The ABI of what the compilers of its environments build (docs/PLAN.md decision 91): compiler → ABI ("msvc", "gnu"). The
    // libraries of environments of one system and one ABI link together, so a library whose recipe does not build in a graph's
    // environment is built in another of them (substitutes). A compiler without an entry has an ABI of its own.
    std::map<std::string, std::string, std::less<>> abis;
    // The build machine's system whose own programs it builds, on its tool platform: "macos" for `apple`, "windows",
    // "linux"; it builds the tool slices there (otok-toml.md 7). Empty: none (`android`).
    std::string host_system;
    // The platform tool dependencies are built for (DESIGN 6.8). A property of the target, not of the
    // machine otok runs on, so a lock resolved anywhere is the same.
    std::string tool_platform;
    // What the target builds (otok-toml.md 5): the linkages it can package, the first its default,
    // and whether a project may build more than one configuration.
    std::vector<std::string> linkages{"static"};
    bool many_configurations = false;
    // Keys of its own in the project's target table, which reach its slices (slice_spec::choices).
    std::vector<target_choice> choices;
    // Where the package entry of each library goes in the target's output directory: `<packages_dir>/<library>`;
    // empty: `<library>` (otok-toml.md 4, 4.1).
    std::string packages_dir;

    // Readers of the target's parts of recipe.toml: the `[<name>]` table and the `<name> = { … }`
    // table of a product. They report problems through the table and call finish() themselves;
    // the core keeps what they return without looking inside.
    std::function<std::any(toml_table& table, recipe_file_check const& files)> read_recipe_table;
    std::function<std::any(toml_table& table, recipe_file_check const& files)> read_product_table;
    // Whether a recipe's `[<name>]` table, as read_recipe_table returned it, makes the recipe deliver its
    // patched source instead of a build (recipe-toml.md 12.1): it has no build.lua, nothing is built, and the
    // target's make_package lays out the source. Absent: the target delivers no sources.
    std::function<bool(std::any const& recipe_table)> delivers_sources;

    // For `otok publish` (recipe-toml.md 14): TOML keys of the target table of the project it builds with, under
    // the recipe's `[publish.<name>]`; and what publication requires of a recipe beyond what every install does
    // (DESIGN 7.4), one message per problem.
    std::string publish_defaults;
    std::function<std::vector<std::string>(recipe const& r)> check_publishable;

    platform_info const* find_platform(std::string_view platform) const noexcept
    {
        for (platform_info const& p : platforms) {
            if (p.name == platform) return &p;
        }
        return nullptr;
    }

    // Whether `e` is one of its environments (common 4.2): `*/<compiler>` for a compiler of every host, else a system
    // and compiler it has, with any architecture.
    bool has_environment(build_environment const& e) const noexcept
    {
        if (e.any_host()) return std::ranges::find(any_host_compilers, e.compiler) != any_host_compilers.end();
        for (build_environment const& own : environments) {
            if (own.system == e.system && own.compiler == e.compiler) return true;
        }
        return false;
    }

    // The environments a library of a graph in `e` may be built in when its recipe does not build in `e` (otok-toml.md 4.9,
    // docs/PLAN.md decision 91), in the order of `environments`: those of the same system with another compiler of the same
    // ABI, on `e`'s architecture. None for `*` environments.
    std::vector<build_environment> substitutes(build_environment const& e) const
    {
        std::vector<build_environment> out;
        if (e.any_host()) return out;
        auto const abi = abis.find(e.compiler);
        if (abi == abis.end()) return out;
        for (build_environment const& own : environments) {
            if (own.system != e.system || own.compiler == e.compiler) continue;
            auto const other = abis.find(own.compiler);
            if (other != abis.end() && other->second == abi->second) out.push_back({e.system, e.arch, own.compiler});
        }
        return out;
    }
};

}
