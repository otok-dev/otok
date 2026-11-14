// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <compare>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "otok/core/environment.hpp"
#include "otok/core/error.hpp"
#include "otok/core/process.hpp"
#include "otok/core/target_info.hpp"
#include "otok/lua/build_script.hpp"
#include "otok/recipe/recipe.hpp"

// What the builder asks a target for (DESIGN 9.2, docs/PLAN.md decision 19). The core never names a
// platform: a target describes each slice's toolchain as data, and the builder runs build.lua with it.
namespace otok {

// One slice the builder is about to build.
struct slice_spec
{
    std::string platform{};         // "iossim", "native"
    std::string arch{};             // "arm64"
    std::string deployment{};       // the platform's deployment version; empty when the target has none
    std::string configuration = "release";
    std::string linkage = "static";  // otok-toml.md 5
    std::map<std::string, std::string, std::less<>> choices{};  // the target's own keys (target_info::choices); none in tool slices
    // The environment it is built in (common 4.2): the machine's system and the compiler, "linux/clang-cl"; empty: none
    // (a target without environments)
    build_environment environment{};
    // The one slice of a package of products of headers only (recipe-toml.md 11.3): no platform, architecture or compiler;
    // the builder gives its result as every slice of the package
    bool headers = false;
};

// The slice name of common 4: `<platform>-<arch>`; `headers` for the headers slice.
inline std::string slice_name(slice_spec const& s)
{
    return s.headers ? std::string{"headers"} : s.platform + "-" + s.arch;
}

// The slice for messages: `native-x86_64`, `native-x86_64 (debug)` (docs/PLAN.md decision 31), `headers`.
inline std::string slice_label(slice_spec const& s)
{
    return slice_name(s) + (s.configuration == "debug" && !s.headers ? " (debug)" : "");
}

// A slice's toolchain as the target prepares it (build-lua-api.md 3, 3.1, 5).
struct toolchain_profile
{
    std::string triple;
    slice_toolchain toolchain;      // compilers, flags and the base environment of processes
    std::map<std::string, std::string> target_fields;                         // slice.<field>
    std::map<std::string, std::map<std::string, std::string>> target_tables;  // slice.<table>
    std::string cmake_toolchain;    // text of the CMake toolchain file; empty: none
    std::string autotools_host;     // `--host` of configure; empty: not cross-compiling
    std::string autotools_build;    // `--build` of configure; empty: configure guesses it
    // otok.b2 (build-lua-api.md 6.2): the toolset of user-config.jam ("darwin", "gcc", "clang", "msvc"),
    // empty when b2 cannot build the slice, and the target properties ("target-os=iphone", …)
    std::string b2_toolset;
    std::string b2_version = "otok";  // msvc.jam derives its settings from the version: "14.5"
    std::vector<std::string> b2_properties;
    // otok.meson (build-lua-api.md 6.2): `[host_machine]` of the cross file, key → value ("system" → "darwin",
    // "cpu" → "arm64", …), empty when the slice is not cross-compiled (a native file); options the target
    // adds ("-Db_vscrt=none")
    std::map<std::string, std::string> meson_host;
    std::vector<std::string> meson_options;
    // `[binaries]` of the machine file, a program and its arguments, beside or instead of those of `toolchain`: "c_ld" →
    // lld-link, "windres" → llvm-rc, "c" → clang-cl with its --target (meson asks the compiler alone for its machine)
    std::map<std::string, std::vector<std::string>> meson_binaries;
    // "-ffile-prefix-map=": the builder adds `<flag><work directory>=/otok` to the compiler flags, so no
    // absolute path of the build reaches the library; empty: the compiler has no such flag.
    std::string prefix_map_flag;
    // The compiler's flag for a standard of a language (otok-toml.md 5.1): C 11 → "-std=c11", C++ 23 → "-std=c++23"; absent: that
    // form. Empty: no flag, the compiler's default (MSVC has no C89 or C99 mode).
    std::function<std::string(language, int standard)> standard_flag;
    // meson's value of c_std or cpp_std for a standard (otok.meson): absent "c11", "c++23"; MSVC's meson takes
    // "c++latest" for C++23 and no C89 or C99 ("": the option is not set, as there is no flag).
    std::function<std::string(language, int standard)> meson_standard;
    // Directories the toolchain reads, readable in the sandbox even under a denied directory (HOME):
    // Xcode (docs/PLAN.md decision 28); the builder adds those of the recipe's toolchains.
    std::vector<std::filesystem::path> read_paths;
    // The toolchain identity of the slice key (cache-key.md 2): name → version, e.g. "msvc" → "14.44.35207".
    std::map<std::string, std::string> identity;
    std::string description;        // for messages: "Visual Studio 18 (MSVC 14.51.36231)"
};

// Where a library's package entry goes in an output directory of the target (target_info::packages_dir).
inline std::filesystem::path package_output_of(target_info const& t, std::string const& library)
{
    return t.packages_dir.empty() ? std::filesystem::path{library} : std::filesystem::path{t.packages_dir} / library;
}

// One slice in the cache.
struct built_slice
{
    slice_spec spec;
    std::string key;                        // 64 hex digits
    std::map<std::string, std::string> option_keys;   // option → its value in the key (cache-key.md 3)
    std::filesystem::path entry;            // builds/<library>/<version>-r<revision>/slices/<key>/
    build_output output;                    // paths inside entry/prefix
    bool cached = false;                    // found, not built
};

struct built_package
{
    std::string library;
    bool tool = false;
    std::string recipe_sha256;              // as in the slice keys
    std::vector<built_slice> slices;
    std::filesystem::path recipe_dir;       // wrappers, the privacy manifest
    std::filesystem::path source;           // the fetched source: its license files
    // A recipe that delivers its source (recipe-toml.md 12.1), which has no slices: the patched source in
    // the cache, and its key (cache-key.md 3.1).
    std::filesystem::path sources{};
    std::string sources_key{};
    bool bundle_data = false;               // `bundle-data` of its [deps] entry (otok-toml.md 6)
    std::string target{};                   // of its graph; a tool package: the target that builds the tools
    std::string id{};                       // resolved_package::id
};

// A library output name of a package: output names are unique in a recipe only (docs/PLAN.md decision 30).
struct library_ref
{
    std::string package;                    // the library (recipe) building it
    std::string name;                       // its output name

    auto operator<=>(library_ref const&) const = default;
};

// A product of a package as it reaches the output (otok-toml.md 4): `libraries` are the libraries it
// holds, the dependencies' included.
struct output_product
{
    product const* source = nullptr;
    std::vector<library_ref> libraries;
    std::vector<std::string> defines;       // those whose `when` holds: `NAME` or `NAME=value`
    std::vector<library_ref> header_products{};  // products of headers only it holds: { package, product name }
    std::vector<std::string> link{};        // its own `link.libraries` whose `when` holds on the target (recipe-toml.md 11)
};

// An option value as wrappers see it (recipe-toml.md 8.2): `OTOK_OPTION_TLS_OPENSSL`, a condition of
// Swift and C, or with a value (`3`, `"ab"` as a C string literal) a C definition only.
struct option_define
{
    std::string name;
    std::optional<std::string> value;
};

// A license file of a package (otok-toml.md 4.2), its text with LF line ends.
struct license_file
{
    std::string path;                       // as in `license-files`
    std::string text;
};

// A package in the output directory of a target.
struct output_package
{
    std::string library;
    recipe const* rec = nullptr;
    std::vector<std::string> libraries;     // its library output names
    std::vector<output_product> products;   // those whose `when` holds
    std::vector<option_define> option_defines;
    std::vector<license_file> licenses;
    std::filesystem::path dir;             // relative to the output directory (package_output)
    std::vector<std::string> data{};       // the names of its data files, in data/ (build-lua-api.md 8)
    bool bundle_data = false;              // and they are resources (otok-toml.md 6)
    std::string linkage = "static";        // of its slices (otok-toml.md 5)
};

class build_target
{
public:
    virtual ~build_target() = default;

    virtual target_info const& info() const = 0;

    // The architecture of the tool slice (platform target_info::tool_platform): the build machine's.
    virtual std::string tool_arch() const = 0;

    // The toolchain of a slice. A target may discover its tools once and reuse them.
    virtual result<toolchain_profile> profile(slice_spec const& slice) = 0;

    // Whether the compiler of an environment is on this machine (common 4.2: the machine has the environment), and what
    // is missing otherwise.
    virtual result<void> find_compiler(build_environment const& environment)
    {
        (void)environment;
        return {};
    }

    // The PATH its builds start with, where the toolchains of recipes (recipe-toml.md 13) are looked up
    // before otok's own PATH (docs/PLAN.md decision 38); empty: none of its own.
    virtual result<std::string> tools_path() { return std::string{}; }

    // What the target makes of the libraries of a slice of `package` once it is built, before the checks and before
    // anything links against them: the Apple target names its dynamic libraries after their frameworks (otok-toml.md
    // 4.10). `libraries`: output name → file, as build returned them.
    virtual result<void> prepare_libraries(std::string const& package, std::map<std::string, std::filesystem::path> const& libraries,
        slice_spec const& slice)
    {
        (void)package;
        (void)libraries;
        (void)slice;
        return {};
    }

    // Target-specific checks of a built library (the architecture of its objects); the core has
    // checked that it is a static archive.
    virtual result<void> check_library(std::filesystem::path const& library, slice_spec const& slice)
    {
        (void)library;
        (void)slice;
        return {};
    }

    // Whether a library that a dynamic library or program of the slice loads (an ELF `NEEDED` name, a Mach-O load
    // command) is the system's, which no dependency provides (build-lua-api.md 8, docs/PLAN.md decision 80). The core
    // refuses the others that are neither the slice's nor a dependency's; a target whose toolchain sees only its SDK
    // checks nothing: all are the system's.
    virtual bool system_library(std::string const& loaded, slice_spec const& slice) const
    {
        (void)loaded;
        (void)slice;
        return true;
    }

    // The output generator (DESIGN 9.2, cache-key.md 4). Lays out the package entry of `package` (its
    // slices of this target) in the empty directory `staging`, as it goes into the project; `products`
    // are those whose `when` holds. The core adds `licenses/` afterwards.
    virtual result<void> make_package(built_package const& package, recipe const& rec, std::span<output_product const> products,
        std::filesystem::path const& staging) = 0;

    // Where a library's package entry is copied, relative to the target's output directory.
    std::filesystem::path package_output(std::string const& library) const { return package_output_of(info(), library); }

    // Writes what the output directory `dir` needs besides the package entries and Acknowledgements.md,
    // once they are in place (Apple: Package.swift, Acknowledgements.plist). `deployment`: deployment
    // key → version, as the project sets them. Problems that do not stop the output go to `warnings`.
    virtual result<void> finish_output(std::filesystem::path const& dir, std::span<output_package const> packages,
        std::map<std::string, std::string> const& deployment, std::vector<std::string>& warnings) const
    {
        (void)dir;
        (void)packages;
        (void)deployment;
        (void)warnings;
        return {};
    }

    // What App Store review would reject in the output directory `dir`, once finish_output has written it (Apple:
    // otok-toml.md 4.4, 4.5): warnings of `otok install`, errors of `otok publish`. `deployment` as for finish_output.
    // What keeps a check from being done (Apple: an SDK's text stub otok cannot read) goes to `warnings`, never failing
    // publish for otok's own shortcoming.
    virtual std::vector<std::string> output_problems(std::filesystem::path const& dir, std::span<output_package const> packages,
        std::map<std::string, std::string> const& deployment, std::vector<std::string>& warnings) const
    {
        (void)dir;
        (void)packages;
        (void)deployment;
        (void)warnings;
        return {};
    }
};

// The environment this machine builds a target in (otok-toml.md 4.9): the first of `build_on` that covers the machine
// and whose compiler the target finds, as the machine's system and that compiler. Nothing to choose when `build_on` is
// empty (a target without environments).
inline result<build_environment> machine_environment_of(build_target& target, std::vector<build_environment> const& build_on,
    std::size_t* index = nullptr)
{
    if (build_on.empty()) return build_environment{};
    std::string names, missing;
    for (std::size_t i = 0; i < build_on.size(); ++i) {
        build_environment const& e = build_on[i];
        names += (names.empty() ? "" : ", ") + e.text();
        build_environment machine = machine_environment(e.compiler);
        if (!covers(e, machine)) continue;
        machine.arch.clear();
        if (auto found = target.find_compiler(machine); !found) {
            missing += "; " + machine.text() + ": " + found.error().message;
            continue;
        }
        if (index) *index = i;  // the graph of the project (otok-lock.md 2)
        return machine;
    }
    return fail("this project builds '" + target.info().name + "' in " + names + "; this machine is " + std::string{host_machine()}
        + (missing.empty() ? "" : missing));
}

// The target that builds the tool slices on this machine: the one of its system (target_info::host_system, otok-toml.md
// 7), on its tool platform; none when this otok has none for it.
inline build_target* host_target(std::span<build_target* const> targets)
{
    std::string const system = machine_environment("").system;
    for (build_target* t : targets) {
        if (t->info().host_system == system) return t;
    }
    return nullptr;
}

}
