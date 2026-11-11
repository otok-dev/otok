// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The targets that build for the machine's own system (docs/PLAN.md decision 82): `linux` with GCC on Linux, `windows`
// with MSVC on Windows; their data, the toolchain they find, and a real build of a tiny C library with it; milestone A:
// projects installed end to end (docs/PLAN.md decision 19). macOS builds for itself through the Apple target
// (apple_target_test). Until decision 82 the development target `native` did this.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "otok/builder/toolchains.hpp"
#include "otok/commands/commands.hpp"
#include "otok/core/environment.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/fetch/git.hpp"
#include "otok/lua/build_script.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "otok/target/windows/windows_target.hpp"
#include "machine_targets.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

// what the otok executable knows: recipes may name the platforms of all
target_info const* const all_targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info(), &target::linux::info()};

#if defined(_M_ARM64) || defined(__aarch64__)
constexpr char const* machine_arch = "arm64";
#else
constexpr char const* machine_arch = "x86_64";
#endif

// The target of this machine's system, its platform and the environment the tests build in; none on macOS.
#if defined(_WIN32)
constexpr char const* host = "windows";
build_environment const host_environment{"windows", "", "msvc"};
std::unique_ptr<build_target> make_host_target(env_vars env) { return target::windows::make_build_target("", "", "", std::move(env)); }
#elif defined(__APPLE__)
constexpr char const* host = "";
build_environment const host_environment{};
std::unique_ptr<build_target> make_host_target(env_vars) { return nullptr; }
#else
constexpr char const* host = "linux";
build_environment const host_environment{"linux", "", "gcc"};
std::unique_ptr<build_target> make_host_target(env_vars env) { return target::linux::make_build_target(std::move(env)); }
#endif

slice_spec host_slice(std::string const& configuration = "release")
{
    return {host, machine_arch, "", configuration, "static", {}, host_environment};
}

// Where a library's files of a configuration are in the output: `lib/`, or `lib/<arch>/` on Windows (otok-toml.md 4.1, 4.8).
std::string output_dir(std::string const& sub, std::string const& configuration)
{
    std::string const dir = (configuration == "debug" ? "debug/" : "") + sub;
    return std::string_view{host} == "windows" ? dir + "/" + machine_arch : dir;
}

}

TEST(linux_target, describes_one_platform)
{
    target_info const& info = target::linux::info();
    EXPECT_EQ(info.name, "linux");
    ASSERT_EQ(info.platforms.size(), 1u);
    EXPECT_EQ(info.platforms[0].name, "linux");
    EXPECT_EQ(info.platforms[0].architectures, std::vector<std::string>{machine_arch});
    EXPECT_TRUE(info.deployment_keys.empty());
    EXPECT_EQ(info.tool_platform, "linux");
    EXPECT_EQ(info.host_system, "linux");
    EXPECT_TRUE(info.has_environment(*parse_environment("linux/gcc")));
    EXPECT_TRUE(info.has_environment(*parse_environment("linux-arm64/clang")));
    EXPECT_FALSE(info.has_environment(*parse_environment("*/gcc")));
    EXPECT_FALSE(info.has_environment(*parse_environment("windows/clang")));

    // a manifest for it names its compiler (otok-toml.md 4.1, 4.9), and needs no deployment versions
    target_info const* const targets[] = {&target::linux::info()};
    diagnostics d;
    auto m = parse_manifest("[otok]\nformat = 1\n[linux]\nbuild-on = [\"linux/clang\"]\n[deps.a]\nrecipe = { path = \"a\" }\n", "otok.toml", targets, {}, d);
    ASSERT_TRUE(m.has_value());
    ASSERT_EQ(m->targets.size(), 1u);
    EXPECT_EQ(m->targets[0].platforms, std::vector<std::string>{"linux"});
    diagnostics none;
    EXPECT_FALSE(parse_manifest("[otok]\nformat = 1\n[linux]\n", "otok.toml", targets, {}, none));
    ASSERT_EQ(none.items().size(), 1u);
    EXPECT_NE(format(none.items()[0]).find("[linux] needs 'build-on': the linux target builds in linux/gcc, linux/clang, in different ways"),
        std::string::npos) << format(none.items()[0]);
}

TEST(linux_target, refuses_other_slices_and_compilers)
{
    auto target = target::linux::make_build_target(current_environment());
    auto other = target->profile({"iossim", "arm64", "15.0", "release", "static", {}, {"linux", "", "gcc"}});
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().message, "the linux target has no slice iossim-arm64");
#ifdef __linux__
    auto compiler = target->find_compiler({"linux", "", "msvc"});
    ASSERT_FALSE(compiler);
    EXPECT_EQ(compiler.error().message, "the linux target has no compiler 'msvc'");
#else
    auto elsewhere = target->find_compiler({"linux", "", "gcc"});
    ASSERT_FALSE(elsewhere);
    EXPECT_EQ(elsewhere.error().message, "the linux target builds on Linux only (gcc of this machine builds for its own system)");
#endif
}

#ifdef _WIN32
TEST(windows_target, chooses_visual_studio)
{
    env_vars env = current_environment();
    std::erase_if(env, [](auto const& v) { return v.first == "VSCMD_VER"; });  // not a developer prompt
    slice_spec const spec = host_slice();
    auto missing = target::windows::make_build_target("99", "", "", env)->profile(spec);
    ASSERT_FALSE(missing.has_value());
    bool const expected = missing.error().message.find("no Visual Studio 99 with the C++ tools") != std::string::npos
        || missing.error().message.find("Visual Studio was not found") != std::string::npos;
    EXPECT_TRUE(expected) << missing.error().message;

    auto wrong = target::windows::make_build_target("vs2022", "", "", env)->profile(spec);
    ASSERT_FALSE(wrong.has_value());
    EXPECT_TRUE(wrong.error().message.find("visual-studio must be a major version such as \"17\"") != std::string::npos
        || wrong.error().message.find("Visual Studio was not found") != std::string::npos) << wrong.error().message;
    auto year = target::windows::make_build_target("2015", "", "", env)->profile(spec);
    ASSERT_FALSE(year.has_value());
    EXPECT_TRUE(year.error().message.find("a product year such as \"2022\"") != std::string::npos
        || year.error().message.find("Visual Studio was not found") != std::string::npos) << year.error().message;

    // the major version and the product year find the same installation
    auto by_major = target::windows::make_build_target("17", "", "", env)->profile(spec);
    auto by_year = target::windows::make_build_target("2022", "", "", env)->profile(spec);
    ASSERT_EQ(by_major.has_value(), by_year.has_value());
    if (by_major) EXPECT_EQ(by_major->description, by_year->description);

    auto path = target::windows::make_build_target("C:/nowhere/vs", "", "", env)->profile(spec);
    ASSERT_FALSE(path.has_value());
    EXPECT_NE(path.error().message.find("nowhere"), std::string::npos) << path.error().message;
}
#endif

// The toolchain the target of this machine finds builds a static library with CMake (MSVC and the Ninja of Visual Studio
// on Windows).
TEST(host_target, builds_with_its_toolchain)
{
    env_vars env = current_environment();
    std::erase_if(env, [](auto const& v) { return v.first == "MAKEFLAGS"; });
    auto target = make_host_target(env);
    if (!target) GTEST_SKIP() << "macOS builds for itself with the Apple target";
    slice_spec const spec = host_slice();
    auto profile = target->profile(spec);
    if (!profile) GTEST_SKIP() << "no toolchain of " << host_environment.text() << " here: " << profile.error().message;
    EXPECT_FALSE(profile->identity.empty());
    EXPECT_TRUE(profile->identity.contains(host_environment.compiler));  // toolchain.<compiler> (cache-key.md 3)
    EXPECT_FALSE(profile->description.empty());
    EXPECT_NE(profile->cmake_toolchain.find("set(CMAKE_C_COMPILER \"" + profile->toolchain.cc + "\")"), std::string::npos);
    EXPECT_EQ(profile->toolchain.env.at("CC"), profile->toolchain.cc);
    auto again = target->profile(host_slice("debug"));  // discovered once, flags per configuration
    ASSERT_TRUE(again.has_value());
    EXPECT_NE(again->toolchain.cflags, profile->toolchain.cflags);
#ifdef _WIN32
    // msvc.jam would take 32-bit x86
    EXPECT_EQ(profile->b2_toolset, "msvc");
    EXPECT_NE(std::ranges::find(profile->b2_properties, "address-model=64"), profile->b2_properties.end());
    EXPECT_NE(std::ranges::find(profile->b2_properties, "abi=ms"), profile->b2_properties.end());
    // meson's names of MSVC's standards: no c++23, no C89
    ASSERT_TRUE(profile->meson_standard);
    EXPECT_EQ(profile->meson_standard(language::cxx, 23), "c++latest");
    EXPECT_EQ(profile->meson_standard(language::cxx, 17), "c++17");
    EXPECT_EQ(profile->meson_standard(language::c, 11), "c11");
    EXPECT_EQ(profile->meson_standard(language::c, 99), "");
    EXPECT_TRUE(profile->meson_host.empty());  // the machine's own architecture: not cross-compiled
#else
    EXPECT_EQ(profile->b2_toolset, "gcc");
#endif

    // the programs as otok chooses them: the target's PATH, then otok's (docs/PLAN.md decision 38)
    auto const tools = target->tools_path();
    ASSERT_TRUE(tools.has_value()) << tools.error().message;
    toolchain_finder finder{process_environment()("PATH").value_or("")};
    auto const find = [&](char const* name) {
        toolchain_requirement req;
        req.name = name;
        return finder.find(req, *tools);
    };
    bool const ninja = find("ninja").has_value();
    if (!find("cmake")) GTEST_SKIP() << "cmake is in neither the target's PATH nor otok's";
#ifdef _WIN32
    if (!ninja) GTEST_SKIP() << "ninja is in neither the target's PATH nor otok's";
#endif
    std::map<std::string, std::filesystem::path> toolchains;
    for (char const* name : {"cmake", ninja ? "ninja" : "make"}) {
        auto found = find(name);
        ASSERT_TRUE(found.has_value()) << name << ": " << found.error().message;
        EXPECT_TRUE(found->program.is_absolute());
        EXPECT_NE(found->version.find('.'), std::string::npos);
        toolchains[name] = found->program;
    }

    test::temp_dir dir;
    std::filesystem::path const base = std::filesystem::canonical(dir.path());
    for (char const* d : {"source", "build", "prefix", "recipe"}) std::filesystem::create_directories(base / d);
    dir.write("source/hello.h", "int hello(void);\n");
    dir.write("source/hello.c", "#include \"hello.h\"\nint hello(void) { return 1; }\n");
    dir.write("source/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.16)\nproject(hello C)\nadd_library(hello STATIC hello.c)\n"
        "install(TARGETS hello ARCHIVE DESTINATION lib)\ninstall(FILES hello.h DESTINATION include)\n");
    dir.write("toolchain.cmake", profile->cmake_toolchain);
    dir.write("recipe/build.lua", std::string{R"(
function build(slice)
    otok.cmake{ generator = ")"} + (ninja ? "Ninja" : "Unix Makefiles") + R"(" }
    local libs = otok.glob("lib/*", slice.prefix)
    assert(#libs == 1, "libraries: " .. #libs)
    return { libraries = { hello = libs[1] }, headers = slice.prefix .. "/include" }
end
)");

    slice_context slice;
    slice.name = slice_name(spec);
    slice.os = host;
    slice.platform = spec.platform;
    slice.arch = spec.arch;
    slice.triple = profile->triple;
    slice.source = base / "source";
    slice.build = base / "build";
    slice.prefix = base / "prefix";
    slice.recipe = base / "recipe";
    slice.toolchain = profile->toolchain;
    slice.cmake_toolchain = base / "toolchain.cmake";
    slice.toolchains = toolchains;
    slice.log = base / "build.log";
    auto out = run_build_script(base / "recipe" / "build.lua", "build.lua", build_entry::build, slice);
    ASSERT_TRUE(out.has_value()) << out.error().message << "\n" << read_file(slice.log).value_or("");
    std::string const archive = read_file(out->libraries.at("hello")).value_or("");
    EXPECT_EQ(archive.substr(0, 8), "!<arch>\n");  // .a and MSVC .lib alike
}

// Nothing of the user's environment reaches the builds, nor what was installed beside the system (build-lua-api.md 5, 6.2, 8;
// docs/PLAN.md decision 80).
TEST(host_target, gives_builds_nothing_of_the_user)
{
    env_vars env = current_environment();
    // not a developer prompt, whose environment would be taken as it is
    std::erase_if(env, [](auto const& v) { return v.first == "MAKEFLAGS" || v.first == "VSCMD_VER"; });
    env["OTOK_TEST_USER_VARIABLE"] = "1";
    env["CPATH"] = "/somewhere";
    auto target = make_host_target(env);
    if (!target) GTEST_SKIP() << "macOS builds for itself with the Apple target";
    slice_spec const spec = host_slice();
    auto profile = target->profile(spec);
    if (!profile) GTEST_SKIP() << "no toolchain of " << host_environment.text() << " here: " << profile.error().message;
    env_vars const& built = profile->toolchain.env;
    EXPECT_FALSE(built.contains("OTOK_TEST_USER_VARIABLE"));
    EXPECT_FALSE(built.contains("CPATH"));
    EXPECT_EQ(built.at("PKG_CONFIG_LIBDIR"), "");
    std::string const& cmake = profile->cmake_toolchain;
    EXPECT_NE(cmake.find("set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)\nset(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)\n"), std::string::npos) << cmake;
#ifdef _WIN32
    EXPECT_NE(cmake.find("set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)\n"), std::string::npos) << cmake;
    EXPECT_TRUE(target->system_library("libpng16.so.16", spec));  // PE imports are not read
#else
    EXPECT_NE(cmake.find("set(CMAKE_IGNORE_PREFIX_PATH \"/usr/local;/opt/local;/home/linuxbrew/.linuxbrew\")\n"), std::string::npos) << cmake;
    // the compilers' directory first, then the system's
    std::string const& path = built.at("PATH");
    EXPECT_TRUE(path.starts_with(path_to_utf8(path_from_utf8(profile->toolchain.cc).parent_path()) + ":")) << path;
    EXPECT_TRUE(path.ends_with(":/usr/bin:/bin:/usr/sbin:/sbin") || path == "/usr/bin:/bin:/usr/sbin:/sbin") << path;
    for (char const* runtime : {"libc.so.6", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1", "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1"}) {
        EXPECT_TRUE(target->system_library(runtime, spec)) << runtime;
    }
    EXPECT_FALSE(target->system_library("libpng16.so.16", spec));
    EXPECT_FALSE(target->system_library("libz.so.1", spec));
    // musl's library and loader on a machine of musl only (decision 105)
    EXPECT_EQ(target->system_library("libc.so", spec), host_libc() == "musl");
    EXPECT_EQ(target->system_library("ld-musl-aarch64.so.1", spec), host_libc() == "musl");
    EXPECT_EQ(target->system_library("libc.musl-aarch64.so.1", spec), host_libc() == "musl");
#endif
}

TEST(linux_target, packages_include_and_lib)
{
    test::temp_dir dir;
    dir.write("prefix/lib/libssl_static.a", "!<arch>\nssl");
    dir.write("prefix/lib/libcrypto.a", "!<arch>\ncrypto");
    dir.write("prefix/ssl/include/openssl/ssl.h", "ssl\n");
    dir.write("prefix/ssl/include/openssl/common.h", "common\n");
    dir.write("prefix/crypto/include/openssl/crypto.h", "crypto\n");
    dir.write("prefix/crypto/include/openssl/common.h", "common\n");
    built_package package{"openssl", false, "", {}, {}, {}};
    built_slice& s = package.slices.emplace_back();
    s.spec = {"linux", machine_arch};
    s.output.libraries = {{"ssl", dir.path() / "prefix/lib/libssl_static.a"}, {"crypto", dir.path() / "prefix/lib/libcrypto.a"}};
    s.output.library_headers = {{"ssl", dir.path() / "prefix/ssl/include"}, {"crypto", dir.path() / "prefix/crypto/include"}};

    auto target = target::linux::make_build_target({});
    EXPECT_EQ(target->package_output("openssl"), std::filesystem::path{"openssl"});
    recipe const rec{};
    std::filesystem::path const staging = dir.path() / "package";
    ASSERT_TRUE(target->make_package(package, rec, {}, staging));
    // the names the build gave them, which may be part of their contract (sonames)
    EXPECT_EQ(read_file(staging / "lib" / "libssl_static.a").value_or("?"), "!<arch>\nssl");
    EXPECT_EQ(read_file(staging / "lib" / "libcrypto.a").value_or("?"), "!<arch>\ncrypto");
    EXPECT_EQ(read_file(staging / "include" / "openssl" / "ssl.h").value_or("?"), "ssl\n");
    EXPECT_EQ(read_file(staging / "include" / "openssl" / "crypto.h").value_or("?"), "crypto\n");
    EXPECT_EQ(read_file(staging / "include" / "openssl" / "common.h").value_or("?"), "common\n");

    // one header path with different contents
    dir.write("prefix/crypto/include/openssl/common.h", "other\n");
    auto clash = target->make_package(package, rec, {}, dir.path() / "clash");
    ASSERT_FALSE(clash);
    EXPECT_EQ(clash.error().message, "the header 'openssl/common.h' differs between header directories");
}

TEST(linux_target, refuses_two_libraries_in_one_file_name)
{
    test::temp_dir dir;
    dir.write("a/libx.a", "!<arch>\na");
    dir.write("b/libx.a", "!<arch>\nb");
    built_package package{"x", false, "", {}, {}, {}};
    built_slice& s = package.slices.emplace_back();
    s.spec = {"linux", machine_arch};
    s.output.libraries = {{"one", dir.path() / "a/libx.a"}, {"two", dir.path() / "b/libx.a"}};
    auto target = target::linux::make_build_target({});
    auto made = target->make_package(package, recipe{}, {}, dir.path() / "package");
    ASSERT_FALSE(made);
    EXPECT_EQ(made.error().message, "the libraries 'one' and 'two' are both files named 'libx.a'");
}

TEST(linux_target, warns_of_one_file_name_in_two_libraries)
{
    test::temp_dir dir;
    dir.write("out/a/lib/libutil.a", "a");
    dir.write("out/a/lib/liba.a", "a");
    dir.write("out/b/lib/libutil.a", "b");
    std::vector<output_package> packages(2);
    packages[0].library = "a";
    packages[0].dir = "a";
    packages[1].library = "b";
    packages[1].dir = "b";
    auto target = target::linux::make_build_target({});
    std::vector<std::string> warnings;
    ASSERT_TRUE(target->finish_output(dir.path() / "out", packages, {}, warnings));
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0], "the libraries 'a' and 'b' both have 'lib/libutil.a': a consumer linking through search paths gets only one of them");
}

namespace {

// A project of tests/data/projects copied to a temporary directory, installed by otok commands with the real targets of
// this machine and a cache of its own (milestone A, docs/PLAN.md decision 19). The fixtures build for Linux with GCC; on
// Windows their [linux] table becomes [windows] with MSVC.
class host_project
{
public:
    explicit host_project(std::string const& fixture)
    {
        base_ = std::filesystem::canonical(dir_.path());
        project_ = base_ / "project";
        auto copied = copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/" + fixture), project_);
        EXPECT_TRUE(copied) << copied.error().message;
        manifest_ = host_manifest(read_file(project_ / "otok.toml").value_or(""));
        write("otok.toml", manifest_);
        env_ = current_environment();
        std::erase_if(env_, [](auto const& v) { return v.first == "MAKEFLAGS"; });
    }

    // A fixture's otok.toml for the target of this machine.
    static std::string host_manifest(std::string text)
    {
        if (std::string_view{host} == "windows") {
            // the machine's architecture: MSVC has the libraries of others only when they are installed
            std::string const from = "[linux]\nbuild-on = [\"linux/gcc\"]\n";
            if (std::size_t const at = text.find(from); at != std::string::npos) {
                text.replace(at, from.size(), "[windows]\nbuild-on = [\"windows/msvc\"]\narchitectures = { windows = [\"" + std::string{machine_arch} + "\"] }\n");
            }
        }
        return text;
    }

    // Why the toolchain here cannot build the project (its recipes use CMake with Ninja, or `tools`); empty when it can. A
    // tool is in the toolchain's PATH (`cc`), or where otok looks up toolchains: the target's PATH, then otok's (docs/PLAN.md
    // decisions 38, 80).
    std::string missing_tools(std::initializer_list<char const*> tools = {"cmake", "ninja"})
    {
        auto target = make_host_target(env_);
        if (!target) return "macOS builds for itself with the Apple target";
        auto p = target->profile(host_slice());
        if (!p) return "no toolchain of " + host_environment.text() + ": " + p.error().message;
        auto const tools_path = target->tools_path();
        toolchain_finder finder{process_environment()("PATH").value_or("")};
        for (char const* tool : tools) {
            toolchain_requirement req;
            req.name = tool;
            if (!find_program(tool, p->toolchain.env, base_) && !finder.find(req, tools_path.value_or(""))) {
                return std::string{tool} + " is in neither the toolchain's PATH nor otok's";
            }
        }
        profile_ = std::move(*p);
        return {};
    }

    int run(std::vector<std::string> const& args)
    {
        out.str({});
        err.str({});
        auto parsed = parse_command_line(args, global_options(), command_specs());
        if (!parsed) {
            err << parsed.error().message;
            return 2;
        }
        std::filesystem::path const cache = base_ / "cache";
        environment const environment = [cache](std::string_view name) -> std::optional<std::string> {
            if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache);
            if (name == "PATH") return process_environment()(name);  // the toolchains after the target's PATH (docs/PLAN.md decision 38)
            return std::nullopt;
        };
        command_context ctx{project_, all_targets, nullptr, out, err, environment};
        env_vars const env = env_;
        ctx.make_build_targets = [env](machine_config const& config) { return test::machine_build_targets(config, env); };
        return run_command(*parsed, ctx);
    }

    std::filesystem::path output(std::string const& relative) const { return project_ / "Otok" / path_from_utf8(relative); }
    std::filesystem::path project_dir() const { return project_; }
    std::filesystem::path cache_dir() const { return base_ / "cache"; }
    std::string const& manifest() const { return manifest_; }

    void write(std::string const& relative, std::string const& text) const { std::ofstream{project_ / path_from_utf8(relative), std::ios::binary} << text; }

    // Compiles `main.c` with the headers and libraries of the output of `libraries` (in this order) of
    // `configuration`, as a consumer of the output would, and runs it: its exit code. Dynamic libraries
    // are found as an app would ship them: DLLs next to the program; elsewhere the shared objects of every
    // library in one directory, the program's rpath, and for the libraries they load in turn their own
    // $ORIGIN, with no LD_LIBRARY_PATH (docs/PLAN.md decision 42).
    result<int> run_consumer(std::string const& main_c, std::vector<std::string> const& libraries, std::string const& configuration = "release")
    {
        std::filesystem::path const dir = base_ / ("consumer-" + configuration);
        std::filesystem::create_directories(dir);
        std::ofstream{dir / "main.c", std::ios::binary} << main_c;
        std::vector<std::string> argv{profile_.toolchain.cc};
        env_vars env = profile_.toolchain.env;
#ifdef _WIN32
        argv.insert(argv.end(), {"/nologo", configuration == "debug" ? "/MDd" : "/MD", "main.c"});
        for (std::string const& l : libraries) argv.push_back("/I" + path_to_utf8(output(l + "/include")));
        for (std::string const& l : libraries) append_libraries(argv, l + "/" + output_dir("lib", configuration));
        argv.push_back("/Femain.exe");
        std::filesystem::path const program = dir / "main.exe";
        for (std::string const& l : libraries) {
            std::error_code ec;
            for (auto const& e : std::filesystem::directory_iterator{output(l + "/" + output_dir("bin", configuration)), ec}) {
                std::filesystem::copy_file(e.path(), dir / e.path().filename(), std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
#else
        argv.push_back("main.c");
        for (std::string const& l : libraries) argv.push_back("-I" + path_to_utf8(output(l + "/include")));
        // shipped as an application ships them: the shared objects of every library in one directory, found
        // through the program's rpath and, for their own dependencies, their $ORIGIN (otok-toml.md 4.1)
        std::filesystem::path const runtime = dir / "lib";
        std::filesystem::create_directories(runtime);
        for (std::string const& l : libraries) {
            std::string const lib = l + "/" + output_dir("lib", configuration);
            append_libraries(argv, lib);
            for (auto const& e : std::filesystem::directory_iterator{output(lib)}) {
                if (path_to_utf8(e.path().filename()).find(".so") == std::string::npos) continue;
                std::filesystem::path const to = runtime / e.path().filename();
                std::error_code ec;
                std::filesystem::remove(to, ec);
                if (e.is_symlink()) std::filesystem::copy_symlink(e.path(), to);
                else std::filesystem::copy_file(e.path(), to);
            }
        }
        argv.push_back("-Wl,-rpath," + path_to_utf8(runtime));
        argv.insert(argv.end(), {"-o", "main"});
        std::filesystem::path const program = dir / "main";
#endif
        auto compiled = run_process({argv, profile_.toolchain.env, dir, dir / "compile.log"});
        if (!compiled) return std::unexpected{compiled.error()};
        if (!compiled->success()) return fail("the consumer does not compile:\n" + read_file(dir / "compile.log").value_or(""));
        auto ran = run_process({{path_to_utf8(program)}, env, dir, {}});
        if (!ran) return std::unexpected{ran.error()};
        return ran->code;
    }

    static env_vars with(env_vars env, env_vars const& more)
    {
        for (auto const& [name, value] : more) env[name] = value;
        return env;
    }

    // The environment of a consumer's build: the toolchain's, with CMake and Ninja where otok finds them.
    env_vars consumer_env() const
    {
        env_vars env = profile_.toolchain.env;
        std::string path;
        for (char const* tool : {"cmake", "ninja"}) {
            auto found = find_program(tool, env, base_);
            if (!found) found = find_program(tool, current_environment(), base_);
            if (found) path += path_to_utf8(found->parent_path()) + (std::string_view{host} == "windows" ? ";" : ":");
        }
        std::string& p = env[std::string_view{host} == "windows" && env.contains("Path") ? "Path" : "PATH"];
        p = path + p;
        return env;
    }

    // A program of `main_c` linking `product` with CMake through the output's otok-config.cmake (otok-toml.md 4.11), built in
    // `configuration` ("Debug", "Release") with `generator` and run: its exit code. A failure carries the log.
    result<int> run_cmake_consumer(std::string const& main_c, std::string const& product, std::string const& configuration,
        std::string const& generator = "Ninja", env_vars const& run_env = {})
    {
        std::string const name = "cmake-" + configuration + "-" + std::to_string(cmake_consumers_++);
        std::filesystem::path const dir = base_ / name;
        std::filesystem::create_directories(dir);
        std::ofstream{dir / "main.c", std::ios::binary} << main_c;
        std::ofstream{dir / "CMakeLists.txt", std::ios::binary}
            << "cmake_minimum_required(VERSION 3.28)\nproject(consumer C)\n"
               "find_package(Otok CONFIG REQUIRED PATHS \"" + path_to_utf8(output("").generic_u8string()) + "\" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)\n"
               "add_executable(app main.c)\ntarget_link_libraries(app PRIVATE otok::" + product + ")\notok_copy_runtime_dlls(app)\n";
        env_vars const env = consumer_env();
        std::filesystem::path const log = dir / "build.log";
        bool const multi = generator.starts_with("Visual Studio");
        std::vector<std::string> configure{"cmake", "-S", path_to_utf8(dir), "-B", path_to_utf8(dir / "build"), "-G", generator};
        if (multi) configure.insert(configure.end(), {"-A", machine_arch == std::string_view{"arm64"} ? "ARM64" : "x64"});
        else configure.push_back("-DCMAKE_BUILD_TYPE=" + configuration);
        for (auto const& argv : {configure, std::vector<std::string>{"cmake", "--build", path_to_utf8(dir / "build"), "--config", configuration}}) {
            auto ran = run_process({argv, env, dir, log});
            if (!ran) return std::unexpected{ran.error()};
            if (!ran->success()) return fail(read_file(log).value_or("the consumer does not build"));
        }
        std::filesystem::path const program = multi ? dir / "build" / configuration / "app.exe"
            : dir / "build" / (std::string_view{host} == "windows" ? "app.exe" : "app");
        auto ran = run_process({{path_to_utf8(program)}, with(env, run_env), dir, {}});
        if (!ran) return std::unexpected{ran.error()};
        return ran->code;
    }

#ifdef _WIN32
    // A program of `main_c` in a project of Visual Studio importing the output's otok.props and otok.targets (otok-toml.md
    // 4.11) with `products` in OtokProducts, built with MSBuild in `configuration` and run: its exit code.
    result<int> run_msbuild_consumer(std::string const& main_c, std::string const& products, std::string const& configuration)
    {
        std::filesystem::path const dir = base_ / ("msbuild-" + configuration + "-" + std::to_string(cmake_consumers_++));
        std::filesystem::create_directories(dir);
        std::ofstream{dir / "main.c", std::ios::binary} << main_c;
        std::string const platform = machine_arch == std::string_view{"arm64"} ? "ARM64" : "x64";
        std::string const otok = path_to_utf8(output("").make_preferred());
        std::string project =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<Project DefaultTargets=\"Build\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
            "  <ItemGroup Label=\"ProjectConfigurations\">\n";
        for (char const* c : {"Debug", "Release"}) {
            project += std::string{"    <ProjectConfiguration Include=\""} + c + "|" + platform + "\"><Configuration>" + c + "</Configuration><Platform>" + platform
                + "</Platform></ProjectConfiguration>\n";
        }
        project += "  </ItemGroup>\n"
                   "  <PropertyGroup Label=\"Globals\"><ProjectGuid>{6F0B5A0E-0C36-4E39-9A61-3C2B3C5D9E11}</ProjectGuid><RootNamespace>app</RootNamespace></PropertyGroup>\n"
                   "  <Import Project=\"" + otok + "otok.props\" />\n"
                   "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n"
                   "  <PropertyGroup Label=\"Configuration\"><ConfigurationType>Application</ConfigurationType><PlatformToolset>$(DefaultPlatformToolset)</PlatformToolset>"
                   "<UseDebugLibraries Condition=\"'$(Configuration)' == 'Debug'\">true</UseDebugLibraries></PropertyGroup>\n"
                   "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n"
                   "  <PropertyGroup><OtokProducts>" + products + "</OtokProducts></PropertyGroup>\n"
                   "  <ItemGroup><ClCompile Include=\"main.c\" /></ItemGroup>\n"
                   "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n"
                   "  <Import Project=\"" + otok + "otok.targets\" />\n"
                   "</Project>\n";
        std::ofstream{dir / "app.vcxproj", std::ios::binary} << project;
        env_vars const env = consumer_env();
        auto const vs = env.find("VSINSTALLDIR");
        if (vs == env.end()) return fail("no VSINSTALLDIR in the toolchain's environment");
        std::filesystem::path const msbuild = path_from_utf8(vs->second) / "MSBuild" / "Current" / "Bin" / "MSBuild.exe";
        std::filesystem::path const log = dir / "build.log";
        auto ran = run_process({{path_to_utf8(msbuild), "app.vcxproj", "/nologo", "/v:minimal", "/p:Configuration=" + configuration, "/p:Platform=" + platform}, env,
            dir, log});
        if (!ran) return std::unexpected{ran.error()};
        if (!ran->success()) return fail(read_file(log).value_or("the consumer does not build"));
        ran = run_process({{path_to_utf8(dir / platform / configuration / "app.exe")}, env, dir, {}});
        if (!ran) return std::unexpected{ran.error()};
        return ran->code;
    }
#endif

    std::ostringstream out, err;

private:
    int cmake_consumers_ = 0;

    // The files of a lib/ directory of the output, under the names their builds gave them
    // (otok-toml.md 4.1); a shared object's links are left to the loader.
    void append_libraries(std::vector<std::string>& argv, std::string const& lib_dir) const
    {
        std::vector<std::string> files;
        for (auto const& e : std::filesystem::directory_iterator{output(lib_dir)}) {
            if (!e.is_symlink()) files.push_back(path_to_utf8(e.path()));
        }
        std::ranges::sort(files);
        argv.insert(argv.end(), files.begin(), files.end());
    }

    test::temp_dir dir_;
    std::filesystem::path base_, project_;
    std::string manifest_;
    env_vars env_;
    toolchain_profile profile_;
};

// The object file of `source` in the build/ of the dev slices of the cache: none, or more than one, is empty.
std::filesystem::path object_of(std::filesystem::path const& cache, std::string const& source)
{
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator{cache / "dev", ec}; !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
        std::string const name = path_to_utf8(it->path().filename());
        if (name == source + ".o" || name == source + ".obj") found.push_back(it->path());
    }
    return found.size() == 1 ? found.front() : std::filesystem::path{};
}

}

TEST(host_target, installs_a_project_end_to_end)
{
    host_project p{"tiny"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << p.out.str();
    EXPECT_NE(p.out.str().find("output 'Otok': 2 copied, 0 unchanged, 0 removed\n"), std::string::npos) << p.out.str();
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-a/include/tiny_a.h")));
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-b/include/tiny_b.h")));
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-a/licenses/LICENSE")));
    std::string const notes = read_file(p.output("Acknowledgements.md")).value_or("?");
    EXPECT_NE(notes.find("\n## tiny-a 1.0\n\nLicense: MIT\n\n```text\n"), std::string::npos) << notes;
    EXPECT_NE(notes.find("\n## tiny-b 1.0\n"), std::string::npos) << notes;
    // a license file of the recipe directory (recipe-toml.md 3)
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-b/licenses/NOTICE")));
    EXPECT_NE(notes.find("tiny-b notice: shipped with the recipe, not with the source.\n"), std::string::npos) << notes;

    // the libraries link; tiny-a was built with the option the project gives it (value = 7) and the
    // flags in their order: the target's -DTINY_BONUS=2, then tiny-a's -DTINY_BONUS=3
    auto code = p.run_consumer("#include <tiny_b.h>\n\nint main(void) { return tiny_b(); }\n", {"tiny-b", "tiny-a"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 13);  // tiny_a = 7 + 3, tiny_b = tiny_a + 1 + 2

    // nothing changed: everything comes from the cache
    ASSERT_EQ(p.run({"install"}), 0) << p.err.str();
    EXPECT_NE(p.out.str().find("slices: 2 (2 from the cache)\n"), std::string::npos) << p.out.str();
    EXPECT_NE(p.out.str().find("output 'Otok': 0 copied, 2 unchanged, 0 removed\n"), std::string::npos) << p.out.str();
}

// otok.dev.toml (otok-dev-toml.md 4): tiny-a from a working tree, built in a work directory kept between installs, so
// that only what changed is compiled again; tiny-b, which depends on it, from the cache while tiny-a's output stays.
TEST(host_target, builds_working_trees_incrementally)
{
    host_project p{"tiny"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;
    std::filesystem::path const tree = p.project_dir() / "work" / "tiny-a";
    ASSERT_TRUE(copy_tree(p.project_dir() / "vendor" / "tiny-a", tree));
    std::string cmake = read_file(tree / "CMakeLists.txt").value_or("");
    cmake.replace(cmake.find("add_library(tiny_a STATIC tiny_a.c)"), 35, "add_library(tiny_a STATIC tiny_a.c tiny_extra.c)");
    // a header written at every configure would compile its includers again: written when it changes, as a project
    // built incrementally does
    if (std::size_t const at = cmake.find("file(WRITE ${CMAKE_BINARY_DIR}/tiny_a_arch.h"); at != std::string::npos) {
        cmake.replace(at, 10, "file(CONFIGURE OUTPUT");
        cmake.replace(cmake.find("tiny_a_arch.h \"#define", at) + 14, 0, "CONTENT ");
    }
    p.write("work/tiny-a/CMakeLists.txt", cmake);
    p.write("work/tiny-a/tiny_extra.c", "int tiny_extra(void) { return 1; }\n");
    p.write("otok.dev.toml", "[deps.tiny-a]\nsource = \"work/tiny-a\"\n");
    // a checkout: what git ignores is not taken
    auto const git = find_git(current_environment());
    if (git) {
        ASSERT_TRUE(run_git(*git, {"init", "-q"}, tree, p.project_dir() / "git.log"));
        std::filesystem::create_directories(tree / "ignored");
        p.write("work/tiny-a/.gitignore", "/ignored/\n");
        p.write("work/tiny-a/ignored/junk.c", "this is no C\n");
    }
    std::string const slice = std::string{host} + "-" + machine_arch;

    ASSERT_EQ(p.run({"install", "--locked"}), 1);  // no lock yet
    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    if (git) {
        std::filesystem::path source;
        for (auto const& e : std::filesystem::recursive_directory_iterator{p.cache_dir() / "dev"}) {
            if (e.path().filename() == "tiny_extra.c") source = e.path().parent_path();
        }
        ASSERT_FALSE(source.empty());
        EXPECT_TRUE(std::filesystem::exists(source / ".gitignore"));
        EXPECT_FALSE(std::filesystem::exists(source / "ignored"));
        EXPECT_FALSE(std::filesystem::exists(source / ".git"));
    }
    EXPECT_NE(p.err.str().find("built tiny-a 1.0 for " + slice + " (dev: work/tiny-a) ("), std::string::npos) << p.err.str();
    EXPECT_NE(p.out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << p.out.str();
    auto code = p.run_consumer("#include <tiny_b.h>\n\nint main(void) { return tiny_b(); }\n", {"tiny-b", "tiny-a"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 13);
    std::filesystem::path const a_object = object_of(p.cache_dir(), "tiny_a.c"), extra_object = object_of(p.cache_dir(), "tiny_extra.c");
    ASSERT_FALSE(a_object.empty());
    ASSERT_FALSE(extra_object.empty());
    auto const a_time = std::filesystem::last_write_time(a_object), extra_time = std::filesystem::last_write_time(extra_object);

    // nothing changed: built again, compiling nothing, and its dependent from the cache
    ASSERT_EQ(p.run({"install", "--locked"}), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.err.str().find("built tiny-a 1.0 for " + slice + " (dev: work/tiny-a)"), std::string::npos) << p.err.str();
    EXPECT_NE(p.out.str().find("slices: 2 (1 from the cache)\n"), std::string::npos) << p.out.str();
    EXPECT_EQ(std::filesystem::last_write_time(a_object), a_time);
    EXPECT_EQ(std::filesystem::last_write_time(extra_object), extra_time);

    // one file changed: compiled alone, and the dependent built again with the new output
    p.write("work/tiny-a/tiny_a.c", "#include \"tiny_a.h\"\n\nint tiny_a(void) { return TINY_A_VALUE + TINY_BONUS + 20; }\n");
    ASSERT_EQ(p.run({"install", "--locked"}), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << p.out.str();
    // the dev slice's output as it came, after its slice: the file compiled, not the other; not that of tiny-b
    EXPECT_NE(p.err.str().find("[tiny-a/" + slice + "] "), std::string::npos) << p.err.str();
    EXPECT_NE(p.err.str().find("tiny_a.c"), std::string::npos) << p.err.str();
    EXPECT_EQ(p.err.str().find("tiny_extra.c"), std::string::npos) << p.err.str();
    EXPECT_EQ(p.err.str().find("[tiny-b/"), std::string::npos) << p.err.str();
    EXPECT_NE(std::filesystem::last_write_time(a_object), a_time);
    EXPECT_EQ(std::filesystem::last_write_time(extra_object), extra_time);
    code = p.run_consumer("#include <tiny_b.h>\n\nint main(void) { return tiny_b(); }\n", {"tiny-b", "tiny-a"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 33);

    // another option empties the build: everything compiled again; without the file, the locked source
    std::string manifest = read_file(p.project_dir() / "otok.toml").value_or("");
    manifest.replace(manifest.find("options = { value = 7 }"), 23, "options = { value = 8 }");
    p.write("otok.toml", manifest);
    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    EXPECT_NE(std::filesystem::last_write_time(extra_object), extra_time);
    code = p.run_consumer("#include <tiny_b.h>\n\nint main(void) { return tiny_b(); }\n", {"tiny-b", "tiny-a"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 34);
    ASSERT_EQ(p.run({"install", "--no-dev"}), 0) << p.err.str() << p.out.str();
    EXPECT_EQ(p.err.str().find("(dev: "), std::string::npos) << p.err.str();
    code = p.run_consumer("#include <tiny_b.h>\n\nint main(void) { return tiny_b(); }\n", {"tiny-b", "tiny-a"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 14);
}

// A meson build (build-lua-api.md 6.2): the native file carries the project's flags.
TEST(host_target, installs_a_meson_project)
{
    host_project p{"meson"};
    if (std::string const missing = p.missing_tools({"meson", "ninja"}); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-m/include/tiny_m.h")));
    auto code = p.run_consumer("#include <tiny_m.h>\n\nint main(void) { return tiny_m(); }\n", {"tiny-m"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 7);  // the option value = 5, the target's -DTINY_M_BONUS=2

    // a shared library
    p.write("otok.toml", p.manifest() + "\n[profile.shared." + std::string{host} + "]\nlinkage = \"dynamic\"\n");
    ASSERT_EQ(p.run({"install", "--profile", "shared"}), 0) << p.err.str() << p.out.str();
    code = p.run_consumer("#include <tiny_m.h>\n\nint main(void) { return tiny_m(); }\n", {"tiny-m"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 7);
}

#if defined(__linux__)
// A shared object that needs another of its slice must find it beside itself: slice.toolchain.rpath
// (docs/PLAN.md decision 42).
TEST(host_target, requires_origin_in_shared_objects)
{
    host_project p{"rpath"};
    if (std::string const missing = p.missing_tools({"cc"}); !missing.empty()) GTEST_SKIP() << missing;
    std::string const manifest = p.manifest();

    p.write("otok.toml", manifest + "options = { origin = false }\n");
    EXPECT_EQ(p.run({"install"}), 1);
    EXPECT_NE(p.err.str().find("needs 'libone.so' of its slice, but its RPATH and RUNPATH lack '$ORIGIN'"), std::string::npos) << p.err.str();

    p.write("otok.toml", manifest);
    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    auto code = p.run_consumer("#include <pair.h>\n\nint main(void) { return two(); }\n", {"pair"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 4);
}
#endif

// A product of headers only beside a compiled one without headers of its own (recipe-toml.md 11.1).
TEST(host_target, installs_products_of_headers_only)
{
    host_project p{"shared-headers"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-h/include/tiny_h/inline.h")));
    auto code = p.run_consumer("#include <tiny_h/compiled.h>\n#include <tiny_h/inline.h>\n\nint main(void) { return tiny_h_compiled() + tiny_h_twice(1); }\n",
        {"tiny-h"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 43);
}

// Dynamic libraries, the one linked against the other, in both configurations (docs/PLAN.md decision 31).
TEST(host_target, installs_dynamic_libraries_in_both_configurations)
{
    host_project p{"dynamic"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.out.str().find("slices: 4 (0 from the cache)\n"), std::string::npos) << p.out.str();
    EXPECT_TRUE(p.err.str().find("warning:") == std::string::npos) << p.err.str();
    for (std::string const configuration : {"release", "debug"}) {
        std::string const lib = "tiny-c/" + output_dir("lib", configuration);
#ifdef _WIN32
        EXPECT_TRUE(std::filesystem::exists(p.output(lib + "/tiny_c.lib"))) << configuration;
        EXPECT_TRUE(std::filesystem::exists(p.output("tiny-c/" + output_dir("bin", configuration) + "/tiny_c.dll"))) << configuration;
#else
        EXPECT_TRUE(std::filesystem::is_symlink(p.output(lib + "/libtiny_c.so"))) << configuration;
        EXPECT_TRUE(std::filesystem::is_symlink(p.output(lib + "/libtiny_c.so.1"))) << configuration;
        EXPECT_TRUE(std::filesystem::exists(p.output(lib + "/libtiny_c.so.1.0"))) << configuration;
#endif
    }
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-c/include/tiny_c.h")));

    // each configuration of the consumer with the libraries of its own: tiny_d is 1 in release, 2 in debug
    std::string const main_c = "#include <tiny_c.h>\n\nint main(void) { return tiny_c(); }\n";
    auto release = p.run_consumer(main_c, {"tiny-c", "tiny-d"});
    ASSERT_TRUE(release) << release.error().message;
    EXPECT_EQ(*release, 11);
    auto debug = p.run_consumer(main_c, {"tiny-c", "tiny-d"}, "debug");
    ASSERT_TRUE(debug) << debug.error().message;
    EXPECT_EQ(*debug, 12);

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str();
    EXPECT_NE(p.out.str().find("slices: 4 (4 from the cache)\n"), std::string::npos) << p.out.str();
}

namespace {

// tiny with a system library every machine has: libm on Linux; on Windows ws2_32 only (recipe-toml.md 11)
void use_system_libraries_of_the_machine(host_project const& p)
{
    std::filesystem::path const recipe = p.project_dir() / "recipes" / "tiny-a" / "recipe.toml";
    std::string text = read_file(recipe).value_or("");
    std::size_t const at = text.find("[\"z\", ");
    text.replace(at, 6, std::string_view{host} == "windows" ? "[" : "[\"m\", ");
    p.write("recipes/tiny-a/recipe.toml", text);
}

// The consumer of tiny: TinyB holds tiny_a, TinyA's defines and its system libraries.
constexpr char const* tiny_main = "#include <tiny_b.h>\n#if TINY_A_EXTRA != 5\n#error the defines of TinyA\n#endif\n\nint main(void) { return tiny_b(); }\n";

// A message as CMake prints it, its lines folded: one space for every run of white space.
std::string flat(std::string const& text)
{
    std::string out;
    for (char c : text) {
        bool const space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
        if (!space) out += c;
        else if (!out.empty() && out.back() != ' ') out += ' ';
    }
    return out;
}

}

// otok-config.cmake, otok.props and otok.targets (otok-toml.md 4.11): consumers with CMake (Ninja; on Windows the Visual Studio
// generator too) and with MSBuild link the products of static libraries in both configurations.
TEST(host_target, gives_consumers_static_libraries)
{
    host_project p{"tiny"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;
    use_system_libraries_of_the_machine(p);
    std::string manifest = p.manifest();
    manifest.insert(manifest.find("cflags = [\"-DTINY_BONUS=2\"]\n"), "configurations = [\"debug\", \"release\"]\n");
    p.write("otok.toml", manifest);

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-a/otok-package.toml")));
    std::filesystem::path const config = p.output("otok-config.cmake");
    std::string const text = read_file(config).value_or("");
    EXPECT_NE(text.find("add_library(otok::tiny-a_tiny_a STATIC IMPORTED)\n"), std::string::npos) << text;
    EXPECT_NE(text.find("set_property(TARGET otok::tiny-b_tiny_b PROPERTY INTERFACE_LINK_LIBRARIES \"otok::tiny-a_tiny_a\")\n"), std::string::npos) << text;
    std::vector<std::string> generators{"Ninja"};
    if (std::string_view{host} == "windows") {
        EXPECT_TRUE(std::filesystem::exists(p.output("otok.props")));
        EXPECT_TRUE(std::filesystem::exists(p.output("otok.targets")));
        generators.emplace_back("Visual Studio 18 2026");
    } else {
        EXPECT_NE(text.find("\"-lm\" \"-lstdc++\""), std::string::npos) << text;  // a C consumer of a C++ library (link.runtime)
    }
    for (std::string const& generator : generators) {
        for (char const* configuration : {"Release", "Debug"}) {
            auto code = p.run_cmake_consumer(tiny_main, "TinyB", configuration, generator);
            ASSERT_TRUE(code) << generator << " " << configuration << ": " << code.error().message;
            EXPECT_EQ(*code, 13) << generator << " " << configuration;
        }
    }
#ifdef _WIN32
    for (char const* configuration : {"Release", "Debug"}) {
        auto code = p.run_msbuild_consumer(tiny_main, "TinyA;TinyB", configuration);
        ASSERT_TRUE(code) << configuration << ": " << code.error().message;
        EXPECT_EQ(*code, 13) << configuration;
    }
    // a product the output does not have
    auto unknown = p.run_msbuild_consumer(tiny_main, "TinyB;Nope", "Release");
    ASSERT_FALSE(unknown);
    EXPECT_NE(unknown.error().message.find("has no product 'Nope' of OtokProducts; it has TinyA, TinyB"), std::string::npos) << unknown.error().message;
#endif

    // nothing changed: the consumer's build sees the same files
    auto const written = std::filesystem::last_write_time(config);
    ASSERT_EQ(p.run({"install"}), 0) << p.err.str();
    EXPECT_EQ(std::filesystem::last_write_time(config), written);
}

// A configuration of the consumer's build that the project did not build: on Linux the release libraries serve a Debug build;
// on Windows, whose debug and release CRTs differ, it is refused, when CMake configures a build of one configuration, when a
// Visual Studio solution links Debug, and by otok.targets (otok-toml.md 4.11).
TEST(host_target, serves_or_refuses_a_configuration_not_built)
{
    host_project p{"tiny"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;
    use_system_libraries_of_the_machine(p);

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    auto code = p.run_cmake_consumer(tiny_main, "TinyB", "Release");
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 13);
    auto debug = p.run_cmake_consumer(tiny_main, "TinyB", "Debug");
    if (std::string_view{host} != "windows") {
        ASSERT_TRUE(debug) << debug.error().message;
        EXPECT_EQ(*debug, 13);
        return;
    }
    ASSERT_FALSE(debug);
    EXPECT_NE(flat(debug.error().message).find("has no libraries of the configuration 'debug', which the build type 'Debug' links with: add \"debug\" to "
                                         "'configurations' of [windows] in otok.toml and run otok install"),
        std::string::npos)
        << debug.error().message;
    debug = p.run_cmake_consumer(tiny_main, "TinyB", "Debug", "Visual Studio 18 2026");
    ASSERT_FALSE(debug);
    EXPECT_NE(debug.error().message.find("otok-has-no-debug-libraries--add-debug-to-configurations-of-windows-in-otok.toml.lib"), std::string::npos)
        << debug.error().message;
#ifdef _WIN32
    debug = p.run_msbuild_consumer(tiny_main, "TinyB", "Debug");
    ASSERT_FALSE(debug);
    EXPECT_NE(debug.error().message.find("has no libraries of the configuration 'debug', which 'Debug' links with (the debug CRT): add \"debug\" to "
                                         "'configurations' of [windows] in otok.toml and run otok install"),
        std::string::npos)
        << debug.error().message;
#endif
}

// Dynamic libraries for consumers (otok-toml.md 4.11): imported shared libraries, the DLLs beside the program on Windows
// (otok_copy_runtime_dlls, OtokCopyDlls), each configuration with its own.
TEST(host_target, gives_consumers_dynamic_libraries)
{
    host_project p{"dynamic"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    std::string const text = read_file(p.output("otok-config.cmake")).value_or("");
    EXPECT_NE(text.find("add_library(otok::tiny-c_tiny_c SHARED IMPORTED)\n"), std::string::npos) << text;
    std::string const main_c = "#include <tiny_c.h>\n\nint main(void) { return tiny_c(); }\n";
    std::vector<std::string> generators{"Ninja"};
    if (std::string_view{host} == "windows") generators.emplace_back("Visual Studio 18 2026");
    for (std::string const& generator : generators) {
        // tiny_d is 1 in release, 2 in debug
        for (auto [configuration, expected, lib] : {std::tuple{"Release", 11, ""}, std::tuple{"Debug", 12, "debug/"}}) {
            env_vars run_env;
            if (std::string_view{host} != "windows") {
                // run from the output, not shipped: the shared objects of both recipes on the loader's path (otok-toml.md 4.1)
                run_env["LD_LIBRARY_PATH"] = path_to_utf8(p.output("tiny-c/" + std::string{lib} + "lib")) + ":"
                    + path_to_utf8(p.output("tiny-d/" + std::string{lib} + "lib"));
            }
            auto code = p.run_cmake_consumer(main_c, "TinyC", configuration, generator, run_env);
            ASSERT_TRUE(code) << generator << " " << configuration << ": " << code.error().message;
            EXPECT_EQ(*code, expected) << generator << " " << configuration;
        }
    }
#ifdef _WIN32
    for (auto [configuration, expected] : {std::pair{"Release", 11}, std::pair{"Debug", 12}}) {
        auto code = p.run_msbuild_consumer(main_c, "TinyC", configuration);
        ASSERT_TRUE(code) << configuration << ": " << code.error().message;
        EXPECT_EQ(*code, expected) << configuration;
    }
#endif
}

// Milestone A: zlib and lz4 from upstream (an archive and a git repository), built at the same time.
TEST(host_target, builds_zlib_and_lz4)
{
    char const* enabled = std::getenv("OTOK_TEST_NETWORK");
    if (!enabled || std::string{enabled} != "1") GTEST_SKIP() << "set OTOK_TEST_NETWORK=1 to run";
    if (!find_git(current_environment())) {
#ifdef _WIN32
        // otok looks for git in PATH; the usual installation is not always in the PATH tests run with
        if (auto git = test::find_test_git()) {
            char const* old = std::getenv("PATH");  // case-insensitive; the variable is often spelled "Path"
            std::string const path = path_to_utf8(git->program.parent_path()) + ";" + (old ? old : "");
            _putenv_s("PATH", path.c_str());
        }
#endif
        if (!find_git(current_environment())) GTEST_SKIP() << "git is not in PATH";
    }
    host_project p{"zlib-lz4"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.run({"install"}), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << p.out.str();
    for (char const* header : {"zlib/include/zlib.h", "zlib/include/zconf.h", "lz4/include/lz4.h"}) {
        EXPECT_TRUE(std::filesystem::exists(p.output(header))) << header;
    }
    auto code = p.run_consumer(R"(#include <string.h>
#include <lz4.h>
#include <zlib.h>

int main(void)
{
    static char const text[] = "otok otok otok otok otok otok otok otok otok otok";
    unsigned char packed[256];
    char back[256];
    uLongf packed_size = sizeof packed, back_size = sizeof back;
    if (compress(packed, &packed_size, (Bytef const*)text, sizeof text) != Z_OK) return 1;
    if (uncompress((Bytef*)back, &back_size, packed, packed_size) != Z_OK) return 2;
    if (back_size != sizeof text || memcmp(back, text, sizeof text) != 0) return 3;
    int const n = LZ4_compress_default(text, (char*)packed, (int)sizeof text, (int)sizeof packed);
    if (n <= 0 || LZ4_decompress_safe((char const*)packed, back, n, (int)sizeof back) != (int)sizeof text) return 4;
    return memcmp(back, text, sizeof text) == 0 ? 0 : 5;
}
)",
        {"zlib", "lz4"});
    ASSERT_TRUE(code) << code.error().message;
    EXPECT_EQ(*code, 0);
}
