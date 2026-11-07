// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <any>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/core/license.hpp"
#include "otok/core/names.hpp"
#include "otok/core/option_data.hpp"
#include "otok/core/option_values.hpp"
#include "otok/core/target_info.hpp"
#include "otok/version/constraint.hpp"
#include "otok/version/standard.hpp"
#include "otok/version/version.hpp"

// Recipe metadata `recipe.toml`, API 1 (docs/formats/recipe-toml.md). This module reads and
// validates the file (recipe-toml.md 16, "before resolution"); checks that need the fetched source
// or a build happen later.
namespace otok {

// Where an input comes from: an archive with its hash, or a git commit (recipe-toml.md 4, 5).
struct input_location
{
    enum class kind
    {
        archive,
        git
    };

    kind type = kind::archive;
    std::string url;        // archive
    std::string sha256;     // archive
    std::vector<std::string> mirrors;  // archive: other URLs of the same file, tried in order after `url`
    std::string git;        // git
    std::string commit;     // git: full SHA
    std::string tag;        // git: optional, the tag of `commit` (recipe-toml.md 4)
    bool submodules = false;
};

struct verify_against
{
    std::string git;
    std::string tag;        // or
    std::string commit;
};

struct recipe_source
{
    input_location location;
    std::vector<std::string> patches;            // recipe-relative paths, applied in order
    std::optional<verify_against> verify;        // archives only
};

// `when = { … }` (recipe-toml.md 9.1). All present keys must hold.
struct condition
{
    std::optional<std::string> option;
    std::optional<std::string> has;              // list option contains
    std::optional<option_data> is;               // enum, string or int option equals
    std::optional<std::vector<std::string>> os;
    std::optional<std::vector<std::string>> platform;
    std::optional<std::vector<std::string>> host;  // the build machine, "windows-x86_64": resources only
    // The slice's architecture and build environment: resources and toolchains only, which no graph depends on, so the
    // resolver, not knowing the architecture, takes them as holding; not under `not` (recipe-toml.md 9.1)
    std::optional<std::vector<std::string>> arch;
    std::optional<std::vector<build_environment>> environment;
    std::shared_ptr<condition const> negated;    // `not = { … }`: holds when it does not
    source_position where;
};

struct recipe_resource
{
    std::string name;
    input_location location;
    bool unpack = true;
    std::optional<std::string> dest;             // inside the source tree
    std::optional<condition> when;               // options, the host, the architecture and the environment only
    source_position where;
};

// `[vars.<name>]`: selector → value (recipe-toml.md 7).
struct recipe_variable
{
    std::string name;
    std::map<std::string, option_data, std::less<>> values;
    source_position where;
};

enum class option_type
{
    boolean,
    integer,
    string,
    enumeration,
    list,
    file                                         // a file of the project (recipe-toml.md 8.3): its path, "" for none
};

struct option_schema
{
    std::string name;
    option_type type = option_type::boolean;
    option_data default_value;
    std::string doc;
    std::vector<std::string> values;             // enum: required; list: optional
    bool additive = false;
    source_position where;
};

struct recipe_dependency
{
    std::string library;
    version_constraint version;
    recipe_id default_recipe;
    bool prerelease = false;
    option_values requires_options;
    std::optional<std::string> linkage;  // "static" or "dynamic": the dependency's when the project chooses none
    std::optional<condition> when;
    // A tool dependency giving programs to build.lua as toolchains, not in deps.tools: those with `default-recipe`
    // (recipe-toml.md 13.1; `library` empty, learned from that recipe by the resolver), or of a dependency
    // (`[deps.<library>] toolchains`, 13.2: `from_dependency`, the dependency's recipe and version).
    std::vector<std::string> programs;
    bool from_dependency = false;
    // [deps.<library>] toolchains: the programs of this dependency the build runs (recipe-toml.md 9)
    std::vector<std::string> toolchains;
    source_position where;
};

struct product_define
{
    std::string define;                          // `NAME` or `NAME=value`
    std::optional<condition> when;
};

// An entry of `link.libraries` (recipe-toml.md 11): a system library, for the targets and options of `when`.
struct product_link
{
    std::string library;
    std::optional<condition> when;               // options and `os` only
};

struct product
{
    std::string name;
    std::vector<std::string> libraries;          // empty: a product of headers only (recipe-toml.md 11.1)
    std::string headers;                         // the library carrying the headers; defaults to the first, empty for none

    bool headers_only() const noexcept { return libraries.empty(); }
    std::vector<std::string> depends;
    std::vector<product_define> defines;
    std::vector<product_link> link_libraries;
    std::string runtime;                         // `link.runtime`: "c++", or empty for none (recipe-toml.md 11)
    std::optional<condition> when;
    std::string alternatives;                   // the group of products excluding each other, or empty (recipe-toml.md 11.2)
    std::map<std::string, std::any, std::less<>> target_settings;   // `<target> = { … }`, read by the target module
    source_position where;
};

// An entry of `license-files` (recipe-toml.md 3): a path in the source or in the recipe directory.
struct license_entry
{
    std::string path;
    bool in_recipe = false;
};

struct toolchain_requirement
{
    std::string name;
    std::optional<version_constraint> version;
    std::vector<std::string> targets;            // rust only
    std::optional<condition> when;               // options only
    std::optional<recipe_id> default_recipe;     // a tool recipe that builds it (recipe-toml.md 13.1)
    std::string from;                            // the dependency whose program it is (recipe-toml.md 13.2)
    source_position where;
};

enum class release_channel
{
    stable,
    prerelease
};

struct recipe
{
    std::int64_t api = 0;
    std::string library;
    std::string name;
    // One of the recipes of the id (recipe-toml.md 3, 6.1): `[recipe] variant`, else the target of the platforms.
    std::string variant;
    std::optional<otok::version> version;        // always set in a successfully parsed recipe
    std::int64_t revision = 0;
    release_channel channel = release_channel::stable;
    std::string license_text;
    license_expression license;
    std::vector<license_entry> license_files;
    std::string description;
    std::string homepage;
    std::string documentation;
    std::vector<std::string> keywords;
    std::vector<std::string> linkage{"static"};  // what the build can produce (recipe-toml.md 3)
    standard_constraints standards;  // the C and C++ standards it builds with (recipe-toml.md 3)

    recipe_source source;
    std::vector<recipe_resource> resources;

    std::vector<std::string> platforms;          // supported
    std::map<std::string, std::vector<std::string>, std::less<>> architectures;
    std::vector<build_environment> build_on;     // where and with what it builds (recipe-toml.md 6)

    std::vector<recipe_variable> vars;
    std::vector<option_schema> options;
    std::vector<recipe_dependency> dependencies;
    std::vector<recipe_dependency> tool_dependencies;  // by library, then by default-recipe: one order in recipe.toml and an index entry
    std::vector<product> products;
    std::map<std::string, std::any, std::less<>> target_settings;   // `[<target>]`, read by the target module
    // A target table says the recipe delivers its patched source, built by the consumer's tools: no build.lua,
    // no slices, products without libraries (recipe-toml.md 12.1).
    bool delivers_sources = false;
    std::vector<toolchain_requirement> toolchains;
    std::vector<std::string> programs;           // `[tool] programs` (recipe-toml.md 3.1); empty: not a tool recipe
    std::vector<option_values> publish_checks;
    std::map<std::string, toml::table, std::less<>> publish_targets;   // `[publish.<target>]`, as written

    option_schema const* find_option(std::string_view option) const noexcept;
    // The product and those it holds through `depends`, transitively.
    std::vector<product const*> held_products(product const& p) const;
    // Two products of which a consumer links one (recipe-toml.md 11.2): together they hold two products of one group.
    bool exclude_each_other(product const& a, product const& b) const;
};

// How the names of platforms, architectures, environments and targets are read (formats README 1.5, 4.1): `strict`
// where a person writes the recipe (a recipe by `path`, publishing, index CI), `open` in what a newer otok may have
// written (an index, a recipe tree of the index or of a git repository), where an unknown name is one that never matches.
enum class recipe_names
{
    strict,
    open
};

// The recipe API versions this otok reads.
inline constexpr std::int64_t recipe_api = 1;

// Toolchain names known in API 1 (recipe-toml.md 13).
std::span<std::string_view const> known_toolchains() noexcept;

// Parses and validates recipe.toml. `files` checks referenced files of the recipe directory (patches,
// wrappers, …); pass nothing to skip those checks. Empty result when there was at least one error.
std::optional<recipe> parse_recipe(std::string_view content, std::string const& file,
    std::span<target_info const* const> targets, recipe_file_check const& files, diagnostics& diags,
    recipe_names names = recipe_names::strict);

// Reads the recipe part of an index entry (index.md 3.1) from its top level, whose other keys the
// caller has read already, and finishes it: a recipe without name and products. Empty result when
// there was at least one error.
std::optional<recipe> read_recipe_projection(toml_table& root, std::span<target_info const* const> targets, diagnostics& diags,
    recipe_names names);

// Reads `<dir>/recipe.toml` and checks the files it references.
std::optional<recipe> load_recipe(std::filesystem::path const& dir,
    std::span<target_info const* const> targets, diagnostics& diags,
    recipe_names names = recipe_names::strict);

}
