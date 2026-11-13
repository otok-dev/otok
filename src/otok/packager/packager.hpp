// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "otok/builder/target.hpp"
#include "otok/cache/store.hpp"
#include "otok/core/error.hpp"
#include "otok/resolver/options.hpp"
#include "otok/resolver/resolver.hpp"

// Packaging built slices (DESIGN 9.2): package entries under package keys (docs/formats/cache-key.md 4),
// laid out by the target's output generator, and the output directories of the project
// (docs/formats/otok-toml.md 4.2).
namespace otok {

// A package entry in the cache.
struct packaged
{
    std::string library;
    std::string target;                     // the target whose slices it holds
    std::string key;                        // 64 hex digits
    std::filesystem::path entry;            // builds/<library>/<version>-r<revision>/packages/<key>/
    bool cached = false;                    // found, not made
    recipe const* rec = nullptr;
    std::vector<std::string> libraries;     // library output names
    std::vector<output_product> products;   // those whose `when` holds, with the dependencies' libraries
    std::vector<option_define> option_defines;
    std::vector<std::string> data;          // the names of its data files
    bool bundle_data = false;               // as resources (otok-toml.md 6)
    std::string linkage = "static";         // of its slices (otok-toml.md 5)
};

// The package entry of every built target package, one per target its slices belong to; tool
// packages have none. `configuration` indexes resolution::configurations. `packaging` is told of each package
// before it is made (not of those in the cache): the library and the target.
result<std::vector<packaged>> package_all(resolution const& r, std::size_t configuration, std::span<built_package const> built,
    std::span<build_target* const> targets, cache_store& cache, std::ostream& progress,
    std::function<void(std::string const& library, std::string const& target)> const& packaging = {});

struct output_changes
{
    std::size_t copied = 0;
    std::size_t unchanged = 0;
    std::size_t removed = 0;
    std::vector<std::string> warnings;      // the target's, about the output
    std::vector<std::string> problems;      // build_target::output_problems: warnings of install, errors of publish
};

// The state file otok keeps in an output directory.
inline constexpr char const* output_state_file = ".otok-output.toml";

// What the state file of the output directory `dir` records (otok-toml.md 4.2): library → package key.
result<std::map<std::string, std::string>> read_output_state(std::filesystem::path const& dir);

// Makes the output directory `dir` of `target` hold exactly its packages among `packages`: copies the
// changed ones, keeps the unchanged ones, removes those no longer there, writes Acknowledgements.md,
// then lets the target finish it (`deployment` as for build_target::finish_output). Refuses a directory
// otok did not create.
result<output_changes> write_output(std::filesystem::path const& dir, build_target const& target, std::span<packaged const> packages,
    std::map<std::string, std::string> const& deployment);

// A tool the project delivers (otok-toml.md 7.1): the prefix of its tool slice.
struct delivered_tool
{
    std::string library;
    std::string key;                                // of its slice: 64 hex digits, or "dev:<hash>" (otok-dev-toml.md 4)
    std::filesystem::path prefix;                   // <entry>/prefix
    std::map<std::string, std::string> programs;    // name → its path relative to the prefix, with '/'
};

// The file of the tools output naming their programs (otok-toml.md 7.1).
inline constexpr char const* tools_file = "otok-tools.toml";

// Its text: the programs of `tools`, relative to the file.
std::string tools_file_text(std::span<delivered_tool const> tools);

// Makes the tools output `dir` hold exactly `tools`, `<dir>/<library>/` each, as write_output does with libraries (the
// state file records the keys of their slices), and writes otok-tools.toml. Refuses a directory otok did not create.
result<output_changes> write_tools_output(std::filesystem::path const& dir, std::span<delivered_tool const> tools);

// The option values of a package as wrappers see them (recipe-toml.md 8.2).
std::vector<option_define> option_defines(recipe const& rec, merged_options const& options);

// Acknowledgements.md of an output (otok-toml.md 4.2): `packages` in name order.
std::string acknowledgements_markdown(std::span<output_package const> packages);

}
