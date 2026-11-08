// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/builder/target.hpp"
#include "otok/core/error.hpp"

// What the consumer's build is given by the targets whose package entries keep the file names the builds gave
// (`windows`, `linux`, `android`; otok-toml.md 4.11, docs/PLAN.md decision 101): the file of each package entry naming
// its files, and `otok-config.cmake` of the output. The targets add what is theirs as data and CMake code; nothing here
// names a platform.
namespace otok {

// The file of a package entry naming which file is which library output name (otok-toml.md 4.11).
inline constexpr char const* package_files_name = "otok-package.toml";

// The files of one library output name in one slice; paths relative to the package entry, with `/`.
struct package_library_file
{
    std::string library;                    // the output name
    std::string arch;
    std::string configuration = "release";
    std::string link;                       // what is linked: a static library, an import library, a shared object
    std::string runtime{};                  // the DLL; empty: none
    std::string symbols{};                  // its .pdb; empty: none
    std::string soname{};                   // the SONAME of a shared object; empty: none
    bool shared = false;                    // a dynamic library: `runtime`, or `link` itself

    bool operator==(package_library_file const&) const = default;
};

struct package_files
{
    std::map<std::string, std::string> properties;  // what the target records: "crt" → "14.44", "stl" → "shared"
    std::vector<package_library_file> files;

    bool operator==(package_files const&) const = default;
};

std::string write_package_files(package_files const& files);
result<package_files> parse_package_files(std::string_view text);

// `otok-package.toml` of a package entry, or of a library's directory in the output.
result<package_files> read_package_files(std::filesystem::path const& dir);

// A library output name of the output as its consumers link it: `otok::<package>_<name>`.
struct consumer_library
{
    std::string name;                       // "<package>_<output name>", the target without `otok::`
    std::string package;
    std::string include_dir;                // relative to the output; empty: none
    bool shared = false;
    std::vector<package_library_file> files;  // paths relative to the output
    std::vector<std::string> depends;       // the libraries it needs after it, in their order (static libraries only)
    std::vector<std::string> link;          // the system libraries of the products holding it
    bool cxx_runtime = false;               // and the C++ standard library (`link.runtime`)
};

// A product of the output: what `otok::<name>` holds.
struct consumer_product
{
    std::string name;
    std::vector<std::string> libraries;     // consumer_library::name, in link order
    std::vector<std::string> include_dirs;  // of the products of headers only it holds, relative to the output
    std::vector<std::string> link;          // the system libraries of those products of headers only
    bool cxx_runtime = false;
    std::vector<std::string> defines;       // `NAME` or `NAME=value`
};

// The output as its consumer files describe it.
struct consumer_package
{
    std::vector<std::string> architectures;     // that every library has, in the target's order
    std::vector<std::string> configurations;    // that every library has: "release", "debug"
    std::map<std::string, std::string> properties;  // of the package files; the first package's where they differ
    std::vector<consumer_library> libraries;
    std::vector<consumer_product> products;
    std::vector<std::pair<std::string, std::string>> data_dirs;  // library → its data/, relative to the output
};

// The consumer files' view of the output `dir` once its package entries are in place: reads their package files.
// `architectures`: the target's, in the order the files list them.
result<consumer_package> consumer_package_of(std::filesystem::path const& dir, std::span<output_package const> packages,
    std::span<std::string const> architectures);

// What a target adds to otok-config.cmake.
struct consumer_cmake_parts
{
    std::string target;                     // "windows": the table of otok.toml messages name
    std::string platform;                   // "windows": the key of `architectures`
    // CMake code that refuses another system and sets `_otok_arch` to the architecture the consumer builds for, as
    // `architectures` names it (empty when it is none of them).
    std::string detect;
    // What to do about an architecture the output has no libraries for; empty: add it to `architectures` of the table.
    std::string arch_cure;
    // CMake code run once the architecture is known (the CRT, the C++ runtime); it may read `_otok_dir`.
    std::string checks;
    // A configuration that was not built is refused (Windows: the CRTs of debug and release differ); otherwise the one
    // that was serves.
    bool strict_configurations = false;
    std::function<std::string(std::string const&)> link_item;  // a system library as INTERFACE_LINK_LIBRARIES takes it
    std::string cxx_runtime;                // what `link.runtime = "c++"` links; empty: nothing
    std::string not_built_suffix;           // the extension of the file a configuration not built links (".lib"), naming the cure
    bool runtime_dlls = false;              // otok_copy_runtime_dlls copies the DLLs; otherwise it does nothing
};

// The text of otok-config.cmake (otok-toml.md 4.11).
std::string consumer_cmake_config(consumer_package const& package, consumer_cmake_parts const& parts);

// CMake's quoting: `"text"` with `\`, `"`, `$` and `;` escaped.
std::string cmake_quoted(std::string_view text);

}
