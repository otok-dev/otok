// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/target/linux/linux_target.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "otok/core/fs.hpp"

namespace otok::target::linux {

namespace fs = std::filesystem;

namespace {

#if defined(_M_ARM64) || defined(__aarch64__)
constexpr char const* machine_arch = "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
constexpr char const* machine_arch = "x86_64";
#else
#   error "unknown architecture of the build machine"
#endif

// What the target found of a compiler once: the environment its builds start from and the tools in it.
struct discovered
{
    env_vars env;
    std::string cc, cxx, ar, ranlib, strip, nm;
    std::string triple;
    std::string version;        // the first line of `--version`
};

#ifdef __linux__

std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string first_line(std::string const& text)
{
    return trimmed(text.substr(0, text.find('\n')));
}

std::string slashed(fs::path const& p)
{
    return path_to_utf8(p.generic_u8string());
}

result<std::string> output_of(std::vector<std::string> argv, env_vars const& env)
{
    std::string const program = argv.front();
    std::error_code ec;
    auto exit = run_process({std::move(argv), env, fs::current_path(ec), {}, true});
    if (!exit) return std::unexpected{exit.error()};
    if (!exit->success()) return fail("'" + program + "' failed with " + describe_exit(*exit));
    return std::move(exit->output);
}

// The compilers of an environment (common 4.2) under the names they are installed by.
std::optional<std::pair<char const*, char const*>> compilers_of(std::string_view compiler)
{
    if (compiler == "gcc") return std::pair{"gcc", "g++"};
    if (compiler == "clang") return std::pair{"clang", "clang++"};
    return std::nullopt;
}

#endif

result<discovered> discover(std::string const& compiler, env_vars const& env)
{
#ifndef __linux__
    (void)env;
    return fail("the linux target builds on Linux only (" + compiler + " of this machine builds for its own system)");
#else
    auto const names = compilers_of(compiler);
    if (!names) return fail("the linux target has no compiler '" + compiler + "'");
    discovered d;
    // the tools are looked up in the user's PATH; the builds get their directories and the system's only, and no other
    // variable of the user's (build-lua-api.md 5, docs/PLAN.md decision 80)
    std::error_code ec;
    fs::path const cwd = fs::current_path(ec);
    std::vector<std::string> dirs;
    auto const add_dir = [&dirs](std::string const& dir) {
        if (std::ranges::find(dirs, dir) == dirs.end()) dirs.push_back(dir);
    };
    for (auto [name, out] : {std::pair{names->first, &d.cc}, std::pair{names->second, &d.cxx}}) {
        auto found = find_program(name, env, cwd);
        if (!found) return fail("'" + std::string{name} + "' is not in PATH: " + found.error().message);
        *out = slashed(*found);
    }
    for (auto [name, out] : {std::pair{"ar", &d.ar}, std::pair{"ranlib", &d.ranlib}, std::pair{"strip", &d.strip}, std::pair{"nm", &d.nm}}) {
        if (auto found = find_program(name, env, cwd)) *out = slashed(*found);
    }
    for (std::string const* tool : {&d.cc, &d.cxx, &d.ar, &d.ranlib, &d.strip, &d.nm}) {
        if (!tool->empty()) add_dir(slashed(path_from_utf8(*tool).parent_path()));
    }
    for (char const* dir : {"/usr/bin", "/bin", "/usr/sbin", "/sbin"}) add_dir(dir);
    std::string path;
    for (std::string const& dir : dirs) path += (path.empty() ? "" : ":") + dir;
    d.env["PATH"] = path;
    auto version = output_of({d.cc, "--version"}, d.env);
    if (!version) return std::unexpected{version.error()};
    auto machine = output_of({d.cc, "-dumpmachine"}, d.env);
    if (!machine) return std::unexpected{machine.error()};
    d.version = first_line(*version);
    d.triple = first_line(*machine);
    return d;
#endif
}

// Copies the header directory `from` into `to`; a file both have must have the same contents.
result<void> merge_headers(fs::path const& from, fs::path const& to)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator{from, ec}; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        fs::path const relative = it->path().lexically_relative(from);
        fs::path const dest = to / relative;
        if (it->is_directory(ec)) {
            fs::create_directories(dest, ec);
            continue;
        }
        if (fs::exists(dest, ec)) {
            auto const a = read_file(it->path());
            auto const b = read_file(dest);
            if (!a || !b || *a != *b) {
                return fail("the header '" + path_to_utf8(relative.generic_u8string()) + "' differs between header directories");
            }
            continue;
        }
        fs::copy_file(it->path(), dest, ec);
    }
    if (ec) return fail("cannot copy the headers of '" + path_to_utf8(from) + "': " + ec.message());
    return {};
}

class linux_build_target final : public build_target
{
public:
    explicit linux_build_target(env_vars environment) : environment_{std::move(environment)} {}

    target_info const& info() const override { return linux::info(); }

    std::string tool_arch() const override { return machine_arch; }

    result<void> find_compiler(build_environment const& environment) override
    {
        auto found = tools(environment.compiler);
        if (!found) return std::unexpected{found.error()};
        return {};
    }

    result<std::string> tools_path() override
    {
        // the PATH of the first compiler found: the toolchains of recipes are looked up there (docs/PLAN.md decision 38)
        for (char const* compiler : {"gcc", "clang"}) {
            if (auto found = tools(compiler)) return (*found)->env.at("PATH");
        }
        return std::string{};
    }

    result<toolchain_profile> profile(slice_spec const& slice) override
    {
        if (slice.platform != "linux" || slice.arch != machine_arch) return fail("the linux target has no slice " + slice_name(slice));
        auto found = tools(slice.environment.compiler);
        if (!found) return std::unexpected{found.error()};
        discovered const& d = **found;
        bool const debug = slice.configuration == "debug";

        toolchain_profile p;
        p.triple = d.triple;
        p.identity[slice.environment.compiler] = d.version;
        p.description = d.version;
        p.prefix_map_flag = "-ffile-prefix-map=";
        slice_toolchain& t = p.toolchain;
        t.cc = d.cc;
        t.cxx = d.cxx;
        t.ar = d.ar;
        t.ranlib = d.ranlib;
        t.strip = d.strip;
        t.nm = d.nm;
        // ELF: a shared object finds the other libraries of its slice beside itself (build-lua-api.md 3.1, docs/PLAN.md decision 42)
        if (slice.linkage == "dynamic") t.rpath = "$ORIGIN";
        // the configuration's flags (build-lua-api.md 3.1)
        t.cflags = debug ? std::vector<std::string>{"-O0", "-g"} : std::vector<std::string>{"-O2", "-DNDEBUG"};
        t.cxxflags = t.cflags;
        t.env = d.env;
        auto const joined = [](std::vector<std::string> const& flags) {
            std::string text;
            for (std::string const& f : flags) text += (text.empty() ? "" : " ") + f;
            return text;
        };
        t.env["CC"] = t.cc;
        t.env["CXX"] = t.cxx;
        if (!t.ar.empty()) t.env["AR"] = t.ar;
        if (!t.ranlib.empty()) t.env["RANLIB"] = t.ranlib;
        t.env["CFLAGS"] = joined(t.cflags);
        t.env["CXXFLAGS"] = joined(t.cxxflags);
        t.env["PKG_CONFIG_LIBDIR"] = "";  // only dependencies, nothing from the machine
        p.cmake_toolchain = "# Generated by otok for the linux target.\n"
                            "set(CMAKE_C_COMPILER \"" + t.cc + "\")\n"
                            "set(CMAKE_CXX_COMPILER \"" + t.cxx + "\")\n"
                            // the system's libraries and the dependencies, not what was installed beside them (build-lua-api.md 6.2,
                            // docs/PLAN.md decision 80)
                            "set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)\n"
                            "set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)\n"
                            "set(CMAKE_IGNORE_PREFIX_PATH \"/usr/local;/opt/local;/home/linuxbrew/.linuxbrew\")\n";
        // b2 finds the slice's properties on the build machine itself (build-lua-api.md 6.2); the abi, which Boost.Context
        // guesses from the build machine's system, not the slice's (the boost recipe's patch)
        p.b2_toolset = slice.environment.compiler;
        p.b2_properties.push_back(std::string_view{machine_arch} == "arm64" ? "abi=aapcs" : "abi=sysv");
        return p;
    }

    // The C and C++ runtimes (build-lua-api.md 8, docs/PLAN.md decision 80).
    bool system_library(std::string const& loaded, slice_spec const& slice) const override
    {
        (void)slice;
        static std::set<std::string, std::less<>> const runtimes{"libc.so.6", "libm.so.6", "libmvec.so.1", "libpthread.so.0", "libdl.so.2",
            "librt.so.1", "libutil.so.1", "libresolv.so.2", "libanl.so.1", "libstdc++.so.6", "libgcc_s.so.1", "libatomic.so.1"};
        return runtimes.contains(loaded) || loaded.starts_with("ld-linux");
    }

    // include/, lib/ and bin/, debug/lib/ and debug/bin/ (otok-toml.md 4.1)
    result<void> make_package(built_package const& package, recipe const& rec, std::span<output_product const> products,
        fs::path const& staging) override
    {
        (void)rec;
        (void)products;
        std::error_code ec;
        fs::create_directories(staging / "include", ec);
        if (ec) return fail("cannot create '" + path_to_utf8(staging) + "': " + ec.message());
        std::set<std::string> configurations;
        for (built_slice const& s : package.slices) {
            if (!configurations.insert(s.spec.configuration).second) return fail("a linux package has one slice per configuration");
            build_output const& out = s.output;

            // headers: the same in every configuration
            fs::path const include = configurations.size() == 1 ? staging / "include" : staging / ("include-" + s.spec.configuration);
            std::set<fs::path> headers;
            if (out.headers) headers.insert(*out.headers);
            for (auto const& [name, dir] : out.library_headers) headers.insert(dir);
            fs::create_directories(include, ec);
            for (fs::path const& dir : headers) {
                if (auto ok = merge_headers(dir, include); !ok) return ok;
            }
            if (include != staging / "include") {
                if (std::string const differ = first_difference(staging / "include", include); !differ.empty()) {
                    return fail("the header '" + differ + "' differs between the configurations");
                }
                fs::remove_all(include, ec);
            }

            // under the names the build gave them, which may be part of their contract (sonames)
            fs::path const root = s.spec.configuration == "debug" ? staging / "debug" : staging;
            std::map<std::string, std::string> files;  // file name → library output name
            auto const place = [&](std::string const& name, fs::path const& file, char const* dir) -> result<void> {
                if (auto [it, fresh] = files.emplace(path_to_utf8(file.filename()), name); !fresh) {
                    return fail("the libraries '" + it->second + "' and '" + name + "' are both files named '" + path_to_utf8(file.filename()) + "'");
                }
                fs::create_directories(root / dir, ec);
                fs::copy_file(file, root / dir / file.filename(), ec);
                if (ec) return fail("cannot copy the library '" + name + "': " + ec.message());
                return {};
            };
            for (auto const& [name, lib] : out.libraries) {
                if (auto ok = place(name, lib, "lib"); !ok) return ok;
                for (auto const* other : {&out.runtimes, &out.symbols}) {
                    if (auto it = other->find(name); it != other->end()) {
                        if (auto ok = place(name, it->second, "bin"); !ok) return ok;
                    }
                }
                // the links to a shared object: libz.so → libz.so.1 → libz.so.1.3.1
                fs::path const real = fs::canonical(lib, ec);
                for (auto it = fs::directory_iterator{lib.parent_path(), ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
                    std::error_code link_ec;
                    if (!it->is_symlink(link_ec) || fs::canonical(it->path(), link_ec) != real) continue;
                    if (auto [o, fresh] = files.emplace(path_to_utf8(it->path().filename()), name); !fresh) {
                        return fail("the libraries '" + o->second + "' and '" + name + "' are both files named '" + path_to_utf8(it->path().filename()) + "'");
                    }
                    fs::create_symlink(fs::read_symlink(it->path(), ec), root / "lib" / it->path().filename(), ec);
                    if (ec) return fail("cannot copy the link '" + path_to_utf8(it->path().filename()) + "' of '" + name + "': " + ec.message());
                }
            }
        }
        return {};
    }

    // One file name in the lib/ of two libraries matters to consumers that link through search paths.
    result<void> finish_output(fs::path const& dir, std::span<output_package const> packages, std::map<std::string, std::string> const& deployment,
        std::vector<std::string>& warnings) const override
    {
        (void)deployment;
        for (char const* sub : {"lib", "bin", "debug/lib", "debug/bin"}) {
            std::map<std::string, std::string> owners;  // file name → library
            for (output_package const& p : packages) {
                std::error_code ec;
                for (auto it = fs::directory_iterator{dir / p.dir / sub, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
                    if (auto [o, fresh] = owners.emplace(path_to_utf8(it->path().filename()), p.library); !fresh) {
                        warnings.push_back("the libraries '" + o->second + "' and '" + p.library + "' both have '" + sub + "/"
                            + path_to_utf8(it->path().filename()) + "': a consumer linking through search paths gets only one of them");
                    }
                }
            }
        }
        return {};
    }

private:
    result<discovered const*> tools(std::string const& compiler)
    {
        auto it = found_.find(compiler);
        if (it == found_.end()) it = found_.emplace(compiler, discover(compiler, environment_)).first;
        if (!it->second) return std::unexpected{it->second.error()};
        return &*it->second;
    }

    env_vars environment_;
    std::map<std::string, result<discovered>> found_;
};

}

target_info const& info()
{
    static target_info const target{
        .name = "linux",
        .platforms = {{"linux", {machine_arch}, {}}},
        .deployment_keys = {},
        .environments = {{"linux", "", "gcc"}, {"linux", "", "clang"}},
        .any_host_compilers = {},
        // both with the system's libstdc++ and glibc (docs/PLAN.md decision 91)
        .abis = {{"gcc", "gnu"}, {"clang", "gnu"}},
        .host_system = "linux",
        .tool_platform = "linux",
        .linkages = {"static", "dynamic"},
        .many_configurations = true,
        .choices = {},
        .packages_dir = "",  // the output directory itself: <output>/<library> (otok-toml.md 4.1)
        .read_recipe_table = {},
        .read_product_table = {},
        .delivers_sources = {},
        .publish_defaults = "",
        .check_publishable = {},
    };
    return target;
}

std::unique_ptr<build_target> make_build_target(env_vars environment)
{
    return std::make_unique<linux_build_target>(std::move(environment));
}

}
