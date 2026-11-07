// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/core/names.hpp"
#include "otok/core/option_values.hpp"
#include "otok/core/target_info.hpp"
#include "otok/fetch/locations.hpp"
#include "otok/version/constraint.hpp"
#include "otok/version/standard.hpp"

// The project manifest `otok.toml`, format 1 (docs/formats/otok-toml.md). This module reads and
// validates what can be checked without recipes (otok-toml.md 13); option values against recipe
// schemas, versions and platforms against recipes are checked by the resolver.
namespace otok {

// `recipe = …` (otok-toml.md 6.1).
struct recipe_ref
{
    enum class kind
    {
        index,  // "publisher/name"
        git,    // { git, rev, path }
        path    // { path }
    };

    kind source = kind::index;
    recipe_id id;        // index
    std::string git;     // git: repository URL
    std::string rev;     // git: full commit SHA
    std::string tag;     // git: optional, the tag of `rev` (otok-toml.md 6.1)
    std::string path;    // git: directory in the repository ("" = root); path: the recipe directory
    source_position where;
};

// `cflags`, `cxxflags` and `ldflags` of a manifest table (otok-toml.md 4.3), one argument each.
struct compile_flags
{
    std::vector<std::string> cflags;
    std::vector<std::string> cxxflags;
    std::vector<std::string> ldflags;
};

// `[deps.<library>]` or `[tool-deps.<library>]` (otok-toml.md 6, 7).
struct dependency
{
    std::string library;
    recipe_ref recipe;
    std::optional<version_constraint> version;
    bool prerelease = false;
    option_values options;
    std::optional<std::vector<std::string>> platforms;  // never for tool dependencies
    std::optional<std::string> source_path;             // `source = { path }`
    compile_flags flags;
    std::optional<std::string> linkage;                 // "static", "dynamic"; never for tool dependencies
    language_standards standards;                       // tool dependencies only (otok-toml.md 5.1)
    bool bundle_data = false;                           // its data files as resources (otok-toml.md 6); never for tool dependencies
    source_position where;                              // the table; `where.file` names the defining file
};

// A target table such as `[apple]` (otok-toml.md 4).
struct target_settings
{
    std::string target;
    std::vector<std::string> platforms;
    std::map<std::string, std::vector<std::string>, std::less<>> architectures;
    std::map<std::string, std::string, std::less<>> deployment;
    std::string output = "Otok";
    compile_flags flags;                // for every library of the target, not tool dependencies
    std::vector<std::string> configurations{"release"};  // otok-toml.md 5
    std::optional<std::string> linkage; // none: the target's default
    language_standards standards;       // none: the lowest every recipe allows (otok-toml.md 5.1)
    std::map<std::string, std::string, std::less<>> choices;  // every target_choice of the target, its default when not given
    // The environments the project is built in (otok-toml.md 4, 4.9); without `build-on`, the target's one environment or
    // `*/<compiler>` of a compiler of every host, else none.
    std::vector<build_environment> build_on;
    source_position where;
};

// `[profile.<name>]` (otok-toml.md 8): only what a profile may change.
struct profile
{
    struct target_overrides
    {
        std::optional<std::vector<std::string>> platforms;
        std::map<std::string, std::vector<std::string>, std::less<>> architectures;
        std::map<std::string, std::string, std::less<>> deployment;
        compile_flags flags;            // appended to the base flags
        std::optional<std::vector<std::string>> configurations;
        std::optional<std::string> linkage;
        language_standards standards;
    };

    struct dependency_overrides
    {
        option_values options;
        std::optional<std::vector<std::string>> platforms;
        compile_flags flags;            // appended to the base flags
        std::optional<std::string> linkage;
        source_position where;
    };

    std::string name;
    std::map<std::string, target_overrides, std::less<>> targets;
    std::map<std::string, dependency_overrides, std::less<>> deps;
    std::map<std::string, dependency_overrides, std::less<>> tool_deps;  // options only
    source_position where;
};

// `[policy]` (otok-toml.md 9).
struct license_policy
{
    std::vector<std::string> deny;
    std::vector<std::string> allow;
};

// The name projects give the index of config.toml (otok-toml.md 6.4).
inline constexpr std::string_view default_index_name = "default";

// `[indexes.<name>]` (otok-toml.md 6.4).
struct index_declaration
{
    std::string name;
    std::string url;                         // view+https://<base>, or the URL of a git repository
    std::vector<std::string> publishers;     // whose ids are in this index only
    source_position where;
};

struct manifest
{
    std::int64_t format = 0;
    std::vector<std::string> includes;
    std::vector<build_environment> tool_build_on;  // `[otok] tool-build-on` as written, one per system (otok-toml.md 3)
    std::vector<target_settings> targets;
    std::vector<dependency> deps;       // the manifest's, then each fragment's, in file order
    std::vector<dependency> tool_deps;
    std::vector<profile> profiles;
    license_policy policy;
    input_mirrors fetch;                // `[fetch]` (otok-toml.md 10)
    std::vector<index_declaration> indexes;  // `[indexes.<name>]`, sorted by name (otok-toml.md 6.4)
    std::map<std::string, std::string, std::less<>> index_of;  // `[index-of]`: recipe id → index name

    dependency const* find_dep(std::string_view library) const noexcept;
    // The environment tool slices are built in on the system of `host`, the target that builds its tools (otok-toml.md 3):
    // `tool-build-on`'s, else the first of the target's environments on that system (`macos/xcode`, `linux/gcc`,
    // `windows/msvc`); without architecture.
    build_environment tool_environment(target_info const& host) const;
    dependency const* find_tool_dep(std::string_view library) const noexcept;
    profile const* find_profile(std::string_view name) const noexcept;
};

// The manifest format versions this otok reads.
inline constexpr std::int64_t manifest_format = 1;

// Returns the content of an included fragment (`path` as written in `include`), or nothing if it
// cannot be read.
using fragment_reader = std::function<std::optional<std::string>(std::string const& path)>;

// Parses and validates a manifest. Every problem is reported to `diags`; the result is empty when
// there was at least one error. `targets` lists the target modules this otok has.
std::optional<manifest> parse_manifest(std::string_view content, std::string const& file,
    std::span<target_info const* const> targets, fragment_reader const& read_fragment, diagnostics& diags);

// Reads `otok.toml` (and its fragments) from disk.
std::optional<manifest> load_manifest(std::filesystem::path const& path,
    std::span<target_info const* const> targets, diagnostics& diags);

// The build configurations of the target table `ts` with profile `p` (or none) over it (otok-toml.md 5).
std::vector<std::string> build_configurations(target_settings const& ts, profile const* p);

// The linkage the project chooses for `library` itself: the profile's and then the manifest's `[deps.<library>]`; nothing
// when neither does, and the recipes of its dependents may ask for one (otok-toml.md 5).
std::optional<std::string> chosen_linkage_of(manifest const& m, profile const* p, std::string_view library);

// The linkage of `library` on `target` as the project alone gives it (otok-toml.md 5): chosen_linkage_of, the profile's and
// then the manifest's target table, the target's default. Resolution puts the dependents' requests between the two
// (package_configuration::linkage).
std::string linkage_of(manifest const& m, profile const* p, std::string_view library, target_info const& target);

// The standard of `lang` for `library` on `target` (otok-toml.md 5.1): the profile's target table's, else the manifest's, the
// same for every library of the target; for a tool dependency (`tool`) its own. None: the project sets none.
std::optional<int> standard_of(manifest const& m, profile const* p, std::string_view library, bool tool, target_info const& target, language lang);

}
