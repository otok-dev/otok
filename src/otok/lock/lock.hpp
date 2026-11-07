// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/core/names.hpp"
#include "otok/version/version.hpp"

// The lock file `otok.lock`, format 1 (docs/formats/otok-lock.md): what to fetch, with the hash of
// every input, per graph of a target and an environment (docs/PLAN.md decision 82). Read and written here;
// resolution produces its content.
namespace otok {

// Where a locked input comes from (otok-lock.md 2, 3).
struct locked_location
{
    enum class kind
    {
        archive,  // { url, sha256 }
        git,      // { git, commit, submodules? }; recipe-source: { git, commit, path }
        path,     // { path } (+ sha256 for a source); inside the project
        local     // "local": outside the project, not reproducible
    };

    kind type = kind::archive;
    std::string url{};
    std::string sha256{};
    std::vector<std::string> mirrors{};  // archive: other URLs of the file (recipe-toml.md 4)
    std::string git{};
    std::string commit{};
    std::string tag{};    // git: optional, the tag of `commit` (recipe-toml.md 4)
    bool submodules = false;
    std::string path{};   // git: directory in the repository; path: directory in the project

    friend bool operator==(locked_location const&, locked_location const&) = default;
};

struct locked_resource
{
    std::string name;
    locked_location location;   // archive or git

    friend bool operator==(locked_resource const&, locked_resource const&) = default;
};

struct locked_package
{
    enum class recipe_kind
    {
        index,  // recipe = "<publisher>/<name>"
        git,    // recipe = "git"
        path    // recipe = "path"
    };

    std::string library;
    std::string target;                 // package only: the target of its graphs
    std::vector<std::string> build_on;  // the environments of its graphs as the project writes them, sorted
    recipe_kind kind = recipe_kind::index;
    recipe_id id;                       // index only
    std::string variant;                // index only
    std::string index;                  // index only: the project's index it came from; empty for `default` (otok-toml.md 6.4)
    std::optional<otok::version> version;
    std::int64_t revision = 0;
    locked_location recipe_source;      // git, path or local
    std::string recipe_sha256;          // empty when the recipe source is local
    locked_location source;             // archive, git, path or local
    std::vector<locked_resource> resources;
    std::string license;
    std::vector<std::string> dependencies;
    std::vector<std::string> tool_dependencies;
    source_position where;

    // Whether everything about the package can be rebuilt by someone else (no "local" input).
    bool reproducible() const noexcept
    {
        return recipe_source.type != locked_location::kind::local && source.type != locked_location::kind::local;
    }

    // Whether it is in the graph of an environment, as the project writes it.
    bool in_graph(std::string_view environment) const noexcept;
};

struct lock_file
{
    std::int64_t format = 1;
    // the systems of the machines the project is built on, sorted: their graphs are resolved (docs/PLAN.md decision 88)
    std::vector<std::string> systems{};
    std::vector<locked_package> packages{};
    std::vector<locked_package> tool_packages{};

    // The entry of a library in the graph of a target and an environment; of a tool reference `<library>@<version>` (otok-lock.md 2),
    // or the first of a tool library, in a tool graph.
    locked_package const* find(std::string_view library, std::string_view target, std::string_view environment) const noexcept;
    locked_package const* find_tool(std::string_view reference, std::string_view environment) const noexcept;
    // The entry of a tool reference in any tool graph.
    locked_package const* find_tool(std::string_view reference) const noexcept;
};

inline constexpr std::int64_t lock_format = 1;

// Parses and validates otok.lock. Empty result when there was at least one error, a lock of another format too
// (`lock_format_of` tells it).
std::optional<lock_file> parse_lock(std::string_view content, std::string const& file, diagnostics& diags);

// The format a lock's text declares; nothing when it declares none.
std::optional<std::int64_t> lock_format_of(std::string_view content);

// The canonical text of a lock (otok-lock.md 4): packages sorted by library, target and environments, keys in a fixed order,
// `\n` line endings, so the same resolution always gives the same bytes.
std::string write_lock(lock_file const& lock);

// A tool reference `<library>@<version>` (otok-lock.md 2) of a tool package, and its parts.
std::string tool_reference(std::string_view library, std::string_view version);
std::pair<std::string_view, std::string_view> split_tool_reference(std::string_view reference) noexcept;

}
