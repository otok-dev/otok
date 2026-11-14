// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/core/environment.hpp"
#include "otok/core/target_info.hpp"
#include "otok/lock/lock.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/resolver/options.hpp"
#include "otok/resolver/provider.hpp"

// Dependency resolution (docs/formats/otok-lock.md 5, DESIGN 5.2, 6.8; algorithm: docs/PLAN.md
// decisions 11, 82). A graph per target and environment of the project and a tool graph per system they run on, each
// for the base configuration and every profile: versions are shared by all graphs, variants chosen per graph, options
// and platforms computed per graph and configuration.
namespace otok {

struct resolve_settings
{
    lock_file const* preferred = nullptr;  // the current lock: its versions are kept where they qualify
    std::vector<std::string> unlocked;     // libraries resolved without that preference (`otok update <library>`)
    std::size_t step_limit = 100000;       // choices before the search gives up
    // the project's [tool-deps] are roots of the tool resolution, not only choices: the build of a tool recipe
    // by otok publish (docs/PLAN.md decision 78)
    bool tool_roots = false;
    // the systems whose graphs are resolved (docs/PLAN.md decision 88): those of the lock and of this machine; nothing:
    // every system the project's environments run on
    std::optional<std::vector<std::string>> systems;
};

// A constraint on a package and who imposed it.
struct package_requirement
{
    std::string requester;   // "otok.toml", "apotocki/libssh2 1.11.1"
    std::string constraint;  // canonical text; empty: any version
};

// A package in one configuration (the base or a profile).
struct package_configuration
{
    std::vector<std::string> platforms;  // sorted; empty: not built in this configuration
    merged_options options;
    std::map<std::string, std::vector<option_request>, std::less<>> requests;  // who asked for which value
    // platform → "static" or "dynamic" (otok-toml.md 5): the project's choice, its dependents' request or the target
    // tables'; empty in the tool resolution, whose packages are static
    std::map<std::string, std::string, std::less<>> linkage;
};

// A graph (otok-lock.md 2): a target and an environment of a target table of the project, or the target that builds a
// system's tools and the environment of `[otok] tool-build-on` for it.
struct resolution_graph
{
    std::string target;
    build_environment environment;  // as the project writes it: "*/ndk"
    std::string tool_platform;      // tool graphs: the platform of the tool slice
};

struct resolved_package
{
    std::string library;
    std::size_t graph = 0;                       // in resolution::graphs, or tool_graphs for a tool package
    std::string target;                          // the graph's
    candidate chosen;                            // the variant of the graph
    // the environment its slices are built in: the graph's, or for a target package whose recipe does not build there another
    // of the same system and ABI (otok-toml.md 4.9, docs/PLAN.md decision 91)
    build_environment environment;
    std::vector<package_requirement> required_by;
    // in the same graph, active in some configuration, sorted: libraries, the ids of tool packages in a tool package
    std::vector<std::string> dependencies;
    std::vector<std::string> tool_dependencies;  // ids of resolution::tool_packages, sorted
    // the tool package of each of the recipe's tool dependencies (recipe::tool_dependencies), "" where it is inactive
    std::vector<std::string> tool_edges;
    std::vector<package_configuration> configurations;  // parallel to resolution::configurations
    // what a package is called after the resolver: "zlib", "zlib@android" when the project has several target tables;
    // a tool package its tool reference, "ninja@1.13.2" (otok-lock.md 2)
    std::string id;
};

struct resolution
{
    std::vector<std::string> configurations;  // "" (the base), then the profile names
    std::vector<resolution_graph> graphs;     // by the target tables of the project and their `build-on`
    std::vector<resolution_graph> tool_graphs;
    std::vector<resolved_package> packages;   // sorted by library and graph
    std::vector<resolved_package> tool_packages;
    // The C and C++ standards of each target (otok-toml.md 5.1), parallel to `configurations`: target name → for each language
    // the project's, or else the lowest every recipe of the target allows in all its graphs; none: neither, each recipe chooses.
    std::vector<std::map<std::string, language_standards, std::less<>>> standards;
    // The standards of the tool packages without one of their own in [tool-deps] (otok-toml.md 5.1, docs/PLAN.md decision
    // 116), by id: one for the tool packages linked together, the highest one of them is given, else the lowest every
    // recipe of them allows; none: neither, each recipe chooses.
    std::map<std::string, language_standards, std::less<>> tool_standards;
    // The tools the project delivers (otok-toml.md 7.1): library → the id of its tool package, the same in each tool graph.
    std::map<std::string, std::string, std::less<>> delivered_tools;
    bool several_targets = false;             // packages are named `<library>@<target>`
    // the systems resolved, sorted (otok-lock.md 2): the graphs of an environment of another system are not here
    std::vector<std::string> systems;

    // The package of a library for a target (of its first graph, before `select_graphs`); a tool package by its id, or the
    // first of a tool library.
    resolved_package const* find(std::string_view library, std::string_view target) const noexcept;
    resolved_package const* find_tool(std::string_view id_or_library) const noexcept;
    // A package by resolved_package::id.
    resolved_package const* find_id(std::string_view id) const noexcept;
};

// The resolution of the graphs one machine builds: `graphs`, at most one of each target, and the tool graphs `tool_graphs` of its
// system.
resolution select_graphs(resolution const& r, std::span<std::size_t const> graphs, std::span<std::size_t const> tool_graphs);

// Resolves the manifest's dependencies. A failure is reported to `diags` with an explanation that
// names the conflicting requirements and who imposes them; warnings (an unused [tool-deps] entry, a
// dependency not built in a profile) are reported there too.
std::optional<resolution> resolve(manifest const& m, std::span<target_info const* const> targets,
    recipe_provider& provider, resolve_settings const& settings, diagnostics& diags);

// Why `[policy]` (otok-toml.md 9) refuses a license expression; nothing when it allows it.
std::optional<std::string> policy_refusal(license_expression const& license, license_policy const& policy);

// `[policy]` (otok-toml.md 9) for every package of both arrays. Reports each violation; returns
// whether there was none.
bool check_license_policy(resolution const& r, license_policy const& policy, diagnostics& diags);

}
