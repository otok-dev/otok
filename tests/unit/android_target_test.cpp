// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The Android target (docs/PLAN.md decision 56): the names of ABIs, the NDK and the CMake toolchain file on
// every host; real builds where an NDK is found (ANDROID_NDK_HOME and the like).

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "otok/commands/commands.hpp"
#include "otok/core/binary.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "machine_targets.hpp"
#include "test_util.hpp"

using namespace otok;
using namespace otok::target::android::detail;

namespace {

target_info const* const all_targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info(), &target::linux::info()};

#ifdef _WIN32
constexpr char const* exe = ".exe";
#else
constexpr char const* exe = "";
#endif

// An NDK of empty files: enough for the target to find its tools and API levels.
void fake_ndk(test::temp_dir& dir, std::string const& root, std::string const& revision)
{
    dir.write(root + "/source.properties", "Pkg.Desc = Android NDK\nPkg.Revision = " + revision + "\n");
    dir.write(root + "/meta/platforms.json", "{\n  \"min\": 21,\n  \"max\": 35,\n  \"aliases\": {\n    \"20\": 19\n  }\n}\n");
    dir.write(root + "/toolchains/llvm/prebuilt/" + host_tag() + "/bin/clang" + exe, "");
}

bool contains(std::vector<std::string> const& list, std::string const& item)
{
    return std::ranges::find(list, item) != list.end();
}

}

TEST(android_target, describes_one_platform_of_four_abis)
{
    target_info const& t = target::android::info();
    ASSERT_EQ(t.platforms.size(), 1u);
    EXPECT_EQ(t.platforms[0].name, "android");
    EXPECT_EQ(t.platforms[0].architectures, (std::vector<std::string>{"arm64-v8a", "armeabi-v7a", "x86_64", "x86"}));
    EXPECT_EQ(t.tool_platform, "");  // tools by the target of the build machine (otok-toml.md 7)
    EXPECT_EQ(t.host_system, "");
    EXPECT_EQ(t.linkages, (std::vector<std::string>{"static", "dynamic"}));
    EXPECT_TRUE(t.has_environment(*parse_environment("*/ndk")));
    EXPECT_TRUE(t.has_environment(*parse_environment("windows-arm64/ndk")));
    EXPECT_FALSE(t.has_environment(*parse_environment("*/clang")));

    // [android] needs no platforms (otok-toml.md 4.7), and a deployment version
    diagnostics d;
    auto m = parse_manifest("[otok]\nformat = 1\n[android]\narchitectures = { android = [\"x86_64\"] }\ndeployment = { android = \"24\" }\n",
        "otok.toml", all_targets, {}, d);
    ASSERT_TRUE(m) << (d.items().empty() ? "" : d.items().front().message);
    EXPECT_EQ(m->targets[0].platforms, std::vector<std::string>{"android"});
    diagnostics none;
    EXPECT_FALSE(parse_manifest("[otok]\nformat = 1\n[android]\n", "otok.toml", all_targets, {}, none));
    EXPECT_NE(format(none.items().front()).find("no deployment version for platform 'android'"), std::string::npos) << format(none.items().front());
}

TEST(android_target, names_abis)
{
    auto arm64 = names_of("arm64-v8a");
    ASSERT_TRUE(arm64);
    EXPECT_EQ(arm64->clang_triple, "aarch64-linux-android");
    EXPECT_EQ(arm64->autotools_host, "aarch64-linux-android");
    EXPECT_EQ(arm64->elf_machine, 183u);
    EXPECT_EQ(arm64->b2_properties, (std::vector<std::string>{"target-os=android", "architecture=arm", "address-model=64", "abi=aapcs", "binary-format=elf"}));
    EXPECT_EQ(arm64->rust_target, "aarch64-linux-android");

    auto arm = names_of("armeabi-v7a");
    ASSERT_TRUE(arm);
    EXPECT_EQ(arm->clang_triple, "armv7a-linux-androideabi");
    EXPECT_EQ(arm->autotools_host, "arm-linux-androideabi");
    EXPECT_EQ(arm->elf_machine, 40u);
    EXPECT_FALSE(arm->wide);
    EXPECT_EQ(arm->cflags, (std::vector<std::string>{"-march=armv7-a", "-mthumb"}));

    auto x86 = names_of("x86");
    ASSERT_TRUE(x86);
    EXPECT_EQ(x86->clang_triple, "i686-linux-android");
    EXPECT_EQ(x86->b2_properties, (std::vector<std::string>{"target-os=android", "architecture=x86", "address-model=32", "abi=sysv", "binary-format=elf"}));
    EXPECT_EQ(x86->goarch, "386");
    EXPECT_EQ(names_of("x86_64")->elf_machine, 62u);
    EXPECT_FALSE(names_of("arm64"));
    EXPECT_FALSE(names_of("riscv64"));
}

TEST(android_target, reads_the_ndk)
{
    EXPECT_EQ(ndk_revision("Pkg.Desc = Android NDK\r\nPkg.Revision = 29.0.14206865\r\nPkg.BaseRevision = 29.0.14206865\r\n"), "29.0.14206865");
    EXPECT_EQ(ndk_revision("Pkg.Desc = Android NDK\n"), "");
    EXPECT_EQ(api_levels("{\n  \"min\": 21,\n  \"max\": 35,\n  \"aliases\": {\"20\": 19}\n}"), (std::pair{21, 35}));
    EXPECT_FALSE(api_levels("{\"min\": 21}"));

    test::temp_dir dir;
    fake_ndk(dir, "sdk/ndk/27.3.13750724", "27.3.13750724");
    fake_ndk(dir, "sdk/ndk/29.0.14206865", "29.0.14206865");
    fake_ndk(dir, "sdk/ndk/9.1.0", "9.1.0");  // compared as numbers, not as text
    dir.write("sdk/ndk/30.0.1/README", "no source.properties: not an NDK");
    auto newest = newest_ndk(dir.path() / "sdk" / "ndk");
    ASSERT_TRUE(newest);
    EXPECT_EQ(newest->filename(), "29.0.14206865");
    EXPECT_FALSE(newest_ndk(dir.path() / "missing"));
}

TEST(android_target, finds_the_ndk)
{
    test::temp_dir dir;
    fake_ndk(dir, "named", "28.2.1");
    fake_ndk(dir, "sdk/ndk/29.0.14206865", "29.0.14206865");
    std::string const base = path_to_utf8(dir.path().generic_u8string());

    // config.toml first, then ANDROID_NDK_HOME, then the SDK
    auto from_config = target::android::make_build_target(base + "/named", {{"ANDROID_HOME", base + "/sdk"}});
    auto p = from_config->profile({"android", "arm64-v8a", "24"});
    ASSERT_TRUE(p) << p.error().message;
    EXPECT_EQ(p->identity.at("ndk"), "28.2.1");
    auto from_sdk = target::android::make_build_target("", {{"ANDROID_HOME", base + "/sdk"}});
    auto q = from_sdk->profile({"android", "arm64-v8a", "24"});
    ASSERT_TRUE(q) << q.error().message;
    EXPECT_EQ(q->identity.at("ndk"), "29.0.14206865");
    EXPECT_EQ(q->description, "Android NDK 29.0.14206865 at '" + base + "/sdk/ndk/29.0.14206865'");

    auto wrong = target::android::make_build_target("", {{"ANDROID_NDK_HOME", base + "/sdk"}, {"ANDROID_HOME", base + "/sdk"}});
    auto r = wrong->profile({"android", "arm64-v8a", "24"});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "ANDROID_NDK_HOME names '" + base + "/sdk', which is not an NDK (it has no source.properties)");
    auto nothing = target::android::make_build_target("", {});
    auto s = nothing->profile({"android", "arm64-v8a", "24"});
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().message, "no Android NDK was found: set 'android-ndk' in config.toml or ANDROID_NDK_HOME, or install one with the SDK manager");
}

TEST(android_target, describes_slices)
{
    test::temp_dir dir;
    fake_ndk(dir, "ndk", "29.0.14206865");
    std::string const ndk = path_to_utf8(dir.path().generic_u8string()) + "/ndk";
    std::string const bin = ndk + "/toolchains/llvm/prebuilt/" + host_tag() + "/bin";
    auto target = target::android::make_build_target(ndk, {});

    auto p = target->profile({"android", "armeabi-v7a", "24", "release", "static"});
    ASSERT_TRUE(p) << p.error().message;
    EXPECT_EQ(p->triple, "armv7a-linux-androideabi24");
    EXPECT_EQ(p->autotools_host, "arm-linux-androideabi");
    EXPECT_EQ(p->toolchain.cc, bin + "/clang" + exe);
    EXPECT_EQ(p->toolchain.ar, bin + "/llvm-ar" + exe);
    EXPECT_TRUE(p->toolchain.rpath.empty());
    std::vector<std::string> const& c = p->toolchain.cflags;
    ASSERT_FALSE(c.empty());
    EXPECT_EQ(c.front(), "--target=armv7a-linux-androideabi24");
    for (char const* flag : {"-DANDROID", "-fPIC", "-march=armv7-a", "-mthumb", "-O2", "-DNDEBUG"}) EXPECT_TRUE(contains(c, flag)) << flag;
    // the target, part of the compiler's command for libtool's links (build-lua-api.md 3.1), and still the first flag
    EXPECT_EQ(p->toolchain.driver_flags, (std::vector<std::string>{"--target=armv7a-linux-androideabi24"}));
    std::vector<std::string> const& l = p->toolchain.ldflags;
    ASSERT_FALSE(l.empty());
    EXPECT_EQ(l.front(), "--target=armv7a-linux-androideabi24");
    EXPECT_EQ(p->toolchain.cxxflags.front(), "--target=armv7a-linux-androideabi24");
    EXPECT_EQ(p->toolchain.env.at("CC"), p->toolchain.cc);  // the bare compiler: otok.autotools adds the flags
    EXPECT_TRUE(contains(l, "-Wl,--no-rosegment"));
    EXPECT_TRUE(contains(l, "-static-libstdc++"));
    EXPECT_FALSE(contains(l, "-Wl,-z,max-page-size=16384"));  // 32-bit
    EXPECT_EQ(p->target_tables.at("android").at("api"), "24");
    EXPECT_EQ(p->target_tables.at("android").at("abi"), "armeabi-v7a");
    EXPECT_EQ(p->target_fields.at("goos"), "android");
    EXPECT_EQ(p->meson_host.at("system"), "android");
    EXPECT_EQ(p->meson_host.at("cpu_family"), "arm");
    EXPECT_EQ(p->b2_toolset, "clang");

    // 64-bit: 16 KiB pages; dynamic: c++_shared; API 30 and later: no --no-rosegment
    auto q = target->profile({"android", "arm64-v8a", "30", "debug", "dynamic"});
    ASSERT_TRUE(q) << q.error().message;
    EXPECT_TRUE(contains(q->toolchain.ldflags, "-Wl,-z,max-page-size=16384"));
    EXPECT_FALSE(contains(q->toolchain.ldflags, "-static-libstdc++"));
    EXPECT_FALSE(contains(q->toolchain.ldflags, "-Wl,--no-rosegment"));
    EXPECT_TRUE(contains(q->toolchain.cflags, "-O0"));
    EXPECT_NE(q->cmake_toolchain.find("set(CMAKE_ANDROID_STL_TYPE c++_shared)\n"), std::string::npos) << q->cmake_toolchain;
    EXPECT_EQ(q->identity.at("stl"), "shared");
    EXPECT_FALSE(p->identity.contains("stl"));  // a static slice links no runtime

    // stl = "static": the shared objects carry the runtime
    auto s = target->profile({"android", "arm64-v8a", "30", "release", "dynamic", {{"stl", "static"}}});
    ASSERT_TRUE(s) << s.error().message;
    EXPECT_TRUE(contains(s->toolchain.ldflags, "-static-libstdc++"));
    EXPECT_NE(s->cmake_toolchain.find("set(CMAKE_ANDROID_STL_TYPE c++_static)\n"), std::string::npos) << s->cmake_toolchain;
    EXPECT_EQ(s->identity.at("stl"), "static");

    auto low = target->profile({"android", "arm64-v8a", "19"});
    ASSERT_FALSE(low);
    EXPECT_EQ(low.error().message, "the Android NDK 29.0.14206865 at '" + ndk + "' builds for API levels 21 to 35, not 19");
    auto text = target->profile({"android", "arm64-v8a", "24.1"});
    ASSERT_FALSE(text);
    EXPECT_EQ(text.error().message, "the API level of android-arm64-v8a must be a number such as \"24\", not '24.1'");
    EXPECT_FALSE(target->profile({"android", "arm64"}));
    EXPECT_FALSE(target->profile({"native", "x86_64"}));
}

TEST(android_target, writes_the_cmake_toolchain_file)
{
    std::string const text = cmake_toolchain({"x86_64", "24", "/ndk", "/ndk/bin/clang", "/ndk/bin/clang++", "/ndk/bin/llvm-ar", "/ndk/bin/llvm-ranlib", false});
    EXPECT_EQ(text,
        "# Generated by otok for an Android slice (x86_64, API 24).\n"
        "set(CMAKE_SYSTEM_NAME Android)\n"
        "set(CMAKE_SYSTEM_VERSION 24)\n"
        "set(CMAKE_ANDROID_ARCH_ABI x86_64)\n"
        "set(CMAKE_ANDROID_NDK \"/ndk\")\n"
        "set(CMAKE_ANDROID_STL_TYPE c++_static)\n"
        "set(CMAKE_C_COMPILER \"/ndk/bin/clang\")\n"
        "set(CMAKE_CXX_COMPILER \"/ndk/bin/clang++\")\n"
        "set(CMAKE_AR \"/ndk/bin/llvm-ar\" CACHE FILEPATH \"\")\n"
        "set(CMAKE_RANLIB \"/ndk/bin/llvm-ranlib\" CACHE FILEPATH \"\")\n"
        "# nothing from outside the NDK and the dependencies (docs/PLAN.md decision 25)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n");
}

TEST(android_target, checks_libraries)
{
    test::temp_dir dir;
    fake_ndk(dir, "ndk", "29.0.14206865");
    auto target = target::android::make_build_target(path_to_utf8(dir.path() / "ndk"), {});
    // ELF64 relocatable objects of AArch64 (183) and x86-64 (62) in archives
    auto const object = [](char machine) {
        std::string o(64, '\0');
        o.replace(0, 6, std::string{"\x7f" "ELF\x02\x01", 6});
        o[16] = 1;
        o[18] = machine;
        return o;
    };
    auto const archive = [](std::string const& data) {
        std::string const size = std::to_string(data.size());
        return "!<arch>\na.o/" + std::string(12, ' ') + std::string(32, ' ') + size + std::string(10 - size.size(), ' ') + "`\n" + data;
    };
    dir.write("libarm.a", archive(object('\xb7')));
    dir.write("libx64.a", archive(object('\x3e')));
    dir.write("libnone.a", "!<arch>\n");
    EXPECT_TRUE(target->check_library(dir.path() / "libarm.a", {"android", "arm64-v8a", "24"}));
    auto wrong = target->check_library(dir.path() / "libx64.a", {"android", "arm64-v8a", "24"});
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().message, "its objects are for the ELF machines 62, not for arm64-v8a (183)");
    auto none = target->check_library(dir.path() / "libnone.a", {"android", "arm64-v8a", "24"});
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().message, "it holds no ELF object");

    // a shared object with a versioned SONAME (DT_SONAME 14)
    std::string so = test::elf_image(14) + std::string{"libfoo.so.1\0", 12};
    so[18] = '\xb7';
    dir.write("libfoo.so", so);
    auto versioned = target->check_library(dir.path() / "libfoo.so", {"android", "arm64-v8a", "24", "release", "dynamic"});
    ASSERT_FALSE(versioned);
    EXPECT_EQ(versioned.error().message, "its SONAME is 'libfoo.so.1', not its file name 'libfoo.so' without a version (libfoo.so), which Android needs");
}

// The ABI of the NDK's toolchain file, and the C++ runtime the app must use with dynamic libraries (otok-toml.md 4.7, 4.11).
TEST(android_target, writes_its_parts_of_the_cmake_config)
{
    consumer_package p;
    p.libraries = {{"z_z", "z", "", false, {{"z", "arm64-v8a", "release", "z/lib/arm64-v8a/libz.a"}}, {}, {}, false}};
    consumer_cmake_parts parts = cmake_parts(p);
    EXPECT_NE(parts.detect.find("if(CMAKE_ANDROID_ARCH_ABI)\n  set(_otok_arch \"${CMAKE_ANDROID_ARCH_ABI}\")\nelseif(ANDROID_ABI)\n"), std::string::npos)
        << parts.detect;
    EXPECT_EQ(parts.link_item("log"), "-llog");
    EXPECT_EQ(parts.cxx_runtime, "");  // the app's ANDROID_STL
    EXPECT_EQ(parts.checks, "");       // static libraries take the app's runtime

    p.libraries.front().shared = true;
    p.properties = {{"stl", "shared"}};
    parts = cmake_parts(p);
    EXPECT_NE(parts.checks.find("if(NOT _otok_stl STREQUAL \"c++_shared\")\n"
                                "  message(FATAL_ERROR \"otok: the dynamic libraries of ${_otok_dir} use the C++ runtime c++_shared (stl = \\\"shared\\\" of "
                                "[android]), \"\n"
                                "    \"which the app must use and ship too, not ${_otok_stl}: set ANDROID_STL to c++_shared (Gradle: arguments "
                                "\\\"-DANDROID_STL=c++_shared\\\")\")\n"),
        std::string::npos)
        << parts.checks;
    p.properties = {{"stl", "static"}};
    parts = cmake_parts(p);
    EXPECT_NE(parts.checks.find("if(_otok_stl STREQUAL \"c++_shared\")\n"
                                "  message(FATAL_ERROR \"otok: the dynamic libraries of ${_otok_dir} each have the C++ runtime c++_static in them (stl = "
                                "\\\"static\\\" of [android]), \"\n"
                                "    \"and an app using c++_shared would have one more: set ANDROID_STL to c++_static, or stl = \\\"shared\\\" of [android] in "
                                "otok.toml and run otok install\")\n"),
        std::string::npos)
        << parts.checks;
}

TEST(android_target, packages_include_and_lib_per_abi)
{
    test::temp_dir dir;
    auto target = target::android::make_build_target("", {});
    dir.write("a/include/z.h", "z\n");
    dir.write("a/lib/libz.a", "!<arch>\n");
    dir.write("b/include/z.h", "z\n");
    dir.write("b/lib/libz.a", "!<arch>\n");
    dir.write("c/include/z.h", "z\n");
    dir.write("c/lib/libz.a", "!<arch>\n");
    built_package package;
    package.library = "z";
    auto const slice = [&](std::string const& prefix, std::string const& abi, std::string const& configuration) {
        built_slice s;
        s.spec = {"android", abi, "24", configuration, "static"};
        s.output.headers = dir.path() / prefix / "include";
        s.output.libraries["z"] = dir.path() / prefix / "lib" / "libz.a";
        return s;
    };
    package.slices = {slice("a", "arm64-v8a", "release"), slice("b", "x86_64", "release"), slice("c", "arm64-v8a", "debug")};
    recipe const rec{};
    ASSERT_TRUE(target->make_package(package, rec, {}, dir.path() / "out"));
    EXPECT_EQ(read_file(dir.path() / "out" / "include" / "z.h").value_or("?"), "z\n");
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "out" / "lib" / "arm64-v8a" / "libz.a"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "out" / "lib" / "x86_64" / "libz.a"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "out" / "debug" / "lib" / "arm64-v8a" / "libz.a"));

    // one include/ for every slice
    dir.write("b/include/z.h", "other\n");
    auto differ = target->make_package(package, rec, {}, dir.path() / "out2");
    ASSERT_FALSE(differ);
    EXPECT_EQ(differ.error().message, "the header 'z.h' of android-x86_64 differs from that of another slice: the Android target keeps one include/ "
                                      "for all ABIs and configurations");
}

namespace {

// A project of tests/data/projects for Android, installed by otok commands with the real targets of this machine.
class android_project
{
public:
    android_project(std::string const& fixture, std::string const& android_table)
    {
        base_ = std::filesystem::canonical(dir_.path());
        project_ = base_ / "project";
        auto copied = copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/" + fixture), project_);
        EXPECT_TRUE(copied) << copied.error().message;
        // the fixture's [linux] table becomes [android]
        std::string manifest = read_file(project_ / "otok.toml").value_or("");
        std::size_t const at = manifest.find("[linux]\n");
        std::size_t const end = manifest.find("\n[deps.", at);
        manifest.replace(at, end - at, "[android]\n" + android_table);
        dir_.write("project/otok.toml", manifest);
    }

    // Why this machine cannot build Android slices; empty when it can.
    std::string missing_tools()
    {
        auto target = target::android::make_build_target("", current_environment());
        auto p = target->profile({"android", "arm64-v8a", "24"});
        if (!p) return p.error().message;
        env_vars env = current_environment();
        for (char const* tool : {"cmake", "ninja"}) {
            if (!find_program(tool, env, base_)) return std::string{tool} + " is not in PATH";
        }
        profile_ = std::move(*p);
        return {};
    }

    int install()
    {
        out.str({});
        err.str({});
        auto parsed = parse_command_line(std::vector<std::string>{"install"}, global_options(), command_specs());
        if (!parsed) return 2;
        std::filesystem::path const cache = base_ / "cache";
        command_context ctx{project_, all_targets, nullptr, out, err, [cache](std::string_view name) -> std::optional<std::string> {
            if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache);
            if (name == "PATH") return process_environment()(name);  // the toolchains (docs/PLAN.md decision 38)
            return std::nullopt;
        }};
        ctx.make_build_targets = [](machine_config const& config) { return test::machine_build_targets(config, current_environment()); };
        return run_command(*parsed, ctx);
    }

    std::filesystem::path output(std::string const& relative) const { return project_ / "Otok" / path_from_utf8(relative); }

    void write(std::string const& relative, std::string const& text) { dir_.write("project/" + relative, text); }

    // Links a shared object of `source` against libraries of the output for `abi`, as an app's CMake build would.
    result<void> link_consumer(std::string const& source, std::string const& abi, std::vector<std::string> const& libraries)
    {
        dir_.write("consumer.c", source);
        auto names = names_of(abi);
        std::vector<std::string> argv{profile_.toolchain.cc, "--target=" + names->clang_triple + "24", "-shared", "-fPIC", "-o",
            path_to_utf8(base_ / ("libconsumer-" + abi + ".so")), path_to_utf8(base_ / "consumer.c")};
        for (std::string const& library : libraries) {
            argv.push_back("-I" + path_to_utf8(output(library + "/include")));
            argv.push_back("-L" + path_to_utf8(output(library + "/lib/" + abi)));
        }
        for (std::string library : libraries) {
            std::ranges::replace(library, '-', '_');  // tiny-b builds libtiny_b
            argv.push_back("-l" + library);
        }
        auto ran = run_process({argv, profile_.toolchain.env, base_, base_ / "consumer.log", true});
        if (!ran) return std::unexpected{ran.error()};
        if (!ran->success()) return fail(read_file(base_ / "consumer.log").value_or("the consumer did not link"));
        return {};
    }

    std::filesystem::path consumer(std::string const& abi) const { return base_ / ("libconsumer-" + abi + ".so"); }

    // A shared object of `source` linking `product` through the output's otok-config.cmake (otok-toml.md 4.11), configured with
    // the NDK's toolchain file for `abi` and built, as an app's externalNativeBuild does: the shared object, or the log.
    result<std::filesystem::path> build_cmake_consumer(std::string const& source, std::string const& product, std::string const& abi,
        std::vector<std::string> const& defines = {})
    {
        std::filesystem::path const dir = base_ / ("cmake-" + abi + "-" + std::to_string(consumers_++));
        std::filesystem::create_directories(dir);
        std::ofstream{dir / "consumer.c", std::ios::binary} << source;
        std::ofstream{dir / "CMakeLists.txt", std::ios::binary}
            << "cmake_minimum_required(VERSION 3.28)\nproject(consumer C)\n"
               "find_package(Otok CONFIG REQUIRED PATHS \"" + path_to_utf8(output("").generic_u8string()) + "\" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)\n"
               "add_library(consumer SHARED consumer.c)\ntarget_link_libraries(consumer PRIVATE otok::" + product + ")\n";
        std::string const ndk = profile_.target_tables.at("android").at("ndk");
        std::vector<std::string> configure{"cmake", "-S", path_to_utf8(dir), "-B", path_to_utf8(dir / "build"), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=" + ndk + "/build/cmake/android.toolchain.cmake", "-DANDROID_ABI=" + abi,
            "-DANDROID_PLATFORM=24"};
        for (std::string const& d : defines) configure.push_back("-D" + d);
        env_vars const env = current_environment();
        std::filesystem::path const log = dir / "build.log";
        for (auto const& argv : {configure, std::vector<std::string>{"cmake", "--build", path_to_utf8(dir / "build")}}) {
            auto ran = run_process({argv, env, dir, log});
            if (!ran) return std::unexpected{ran.error()};
            if (!ran->success()) return fail(read_file(log).value_or("the consumer does not build"));
        }
        return dir / "build" / "libconsumer.so";
    }

    std::ostringstream out, err;

private:
    int consumers_ = 0;
    test::temp_dir dir_;
    std::filesystem::path base_, project_;
    toolchain_profile profile_;
};

}

TEST(android_target, installs_static_libraries_for_two_abis)
{
    android_project p{"tiny", "architectures = { android = [\"arm64-v8a\", \"x86_64\"] }\ndeployment = { android = \"24\" }\ncflags = [\"-DTINY_BONUS=2\"]\n"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.install(), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.out.str().find("slices: 4 (0 from the cache)\n"), std::string::npos) << p.out.str();
    for (std::string const abi : {"arm64-v8a", "x86_64"}) {
        std::filesystem::path const lib = p.output("tiny-a/lib/" + abi + "/libtiny_a.a");
        ASSERT_TRUE(std::filesystem::exists(lib)) << abi;
        EXPECT_EQ(elf_machines_of(lib), std::vector<unsigned>{abi == "x86_64" ? 62u : 183u});
        EXPECT_TRUE(std::filesystem::exists(p.output("tiny-b/lib/" + abi + "/libtiny_b.a"))) << abi;
        auto linked = p.link_consumer("#include <tiny_b.h>\n\nint consumer(void) { return tiny_b(); }\n", abi, {"tiny-b", "tiny-a"});
        EXPECT_TRUE(linked) << linked.error().message;
    }
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-a/include/tiny_a.h")));
    EXPECT_TRUE(std::filesystem::exists(p.output("Acknowledgements.md")));

    ASSERT_EQ(p.install(), 0) << p.err.str();
    EXPECT_NE(p.out.str().find("slices: 4 (4 from the cache)\n"), std::string::npos) << p.out.str();
}

TEST(android_target, installs_shared_objects_in_both_configurations)
{
    android_project p{"dynamic", "architectures = { android = [\"arm64-v8a\"] }\ndeployment = { android = \"24\" }\n"
                                 "configurations = [\"debug\", \"release\"]\nlinkage = \"dynamic\"\n"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.install(), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.out.str().find("slices: 4 (0 from the cache)\n"), std::string::npos) << p.out.str();
    for (std::string const sub : {"lib", "debug/lib"}) {
        // unversioned, as Gradle packs them; the 16 KiB pages of 64-bit libraries
        std::filesystem::path const so = p.output("tiny-c/" + sub + "/arm64-v8a/libtiny_c.so");
        ASSERT_TRUE(std::filesystem::exists(so)) << sub;
        EXPECT_FALSE(std::filesystem::is_symlink(so));
        EXPECT_EQ(load_names_of(so).id, "libtiny_c.so");
        EXPECT_TRUE(contains(load_names_of(so).needed, "libtiny_d.so"));
        EXPECT_TRUE(load_names_of(so).search.empty());
        EXPECT_GE(elf_load_alignment_of(so), 16384u);
    }
    auto linked = p.link_consumer("#include <tiny_c.h>\n\nint consumer(void) { return tiny_c(); }\n", "arm64-v8a", {"tiny-c", "tiny-d"});
    EXPECT_TRUE(linked) << linked.error().message;
}

namespace {

// CMake's message, its lines folded: one space for every run of white space.
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

// otok-config.cmake for an app's CMake (otok-toml.md 4.11): the ABI of the NDK's toolchain file picks the libraries.
TEST(android_target, gives_cmake_consumers_static_libraries)
{
    android_project p{"tiny", "architectures = { android = [\"arm64-v8a\", \"x86_64\"] }\ndeployment = { android = \"24\" }\ncflags = [\"-DTINY_BONUS=2\"]\n"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.install(), 0) << p.err.str() << p.out.str();
    std::string const source = "#include <tiny_b.h>\n#if TINY_A_EXTRA != 5\n#error the defines of TinyA\n#endif\n\nint consumer(void) { return tiny_b(); }\n";
    for (std::string const abi : {"arm64-v8a", "x86_64"}) {
        auto built = p.build_cmake_consumer(source, "TinyB", abi);
        ASSERT_TRUE(built) << abi << ": " << built.error().message;
        EXPECT_EQ(elf_machines_of(*built), std::vector<unsigned>{abi == "x86_64" ? 62u : 183u});
        EXPECT_TRUE(contains(load_names_of(*built).needed, "libz.so")) << abi;  // TinyA's system library
    }
    auto other = p.build_cmake_consumer(source, "TinyB", "armeabi-v7a");
    ASSERT_FALSE(other);
    EXPECT_NE(flat(other.error().message).find("has no libraries for the architecture 'armeabi-v7a': add it to 'architectures' of [android] in otok.toml "
                                               "and run otok install"),
        std::string::npos)
        << other.error().message;
}

// Dynamic libraries linked with c++_shared (stl = "shared") need an app of the same runtime (otok-toml.md 4.7, 4.11).
TEST(android_target, gives_cmake_consumers_shared_objects)
{
    android_project p{"dynamic", "architectures = { android = [\"arm64-v8a\"] }\ndeployment = { android = \"24\" }\n"
                                 "configurations = [\"debug\", \"release\"]\nlinkage = \"dynamic\"\n"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.install(), 0) << p.err.str() << p.out.str();
    std::string const source = "#include <tiny_c.h>\n\nint consumer(void) { return tiny_c(); }\n";
    auto refused = p.build_cmake_consumer(source, "TinyC", "arm64-v8a");
    ASSERT_FALSE(refused);
    EXPECT_NE(flat(refused.error().message).find("use the C++ runtime c++_shared (stl = \"shared\" of [android]), which the app must use and ship too, "
                                                 "not c++_static: set ANDROID_STL to c++_shared"),
        std::string::npos)
        << refused.error().message;
    auto built = p.build_cmake_consumer(source, "TinyC", "arm64-v8a", {"ANDROID_STL=c++_shared"});
    ASSERT_TRUE(built) << built.error().message;
    EXPECT_TRUE(contains(load_names_of(*built).needed, "libtiny_c.so"));
}

// build_tools of a recipe runs on the target of the build machine, in its tool environment, with the recipe's toolchains
// checked for it (otok-toml.md 7, docs/PLAN.md decisions 56, 82)
TEST(android_target, builds_tools_with_the_target_of_the_machine)
{
    android_project p{"tiny", "architectures = { android = [\"x86_64\"] }\ndeployment = { android = \"24\" }\ncflags = [\"-DTINY_BONUS=2\"]\n"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;
    std::string const system = machine_environment("").system;
    std::string const os = system == "macos" ? "apple" : system;
    std::string const platform = system == "macos" ? "macosx" : system;
    std::string const environment = system == "macos" ? "macos/xcode" : system == "linux" ? "linux/gcc" : "windows/msvc";
    p.write("recipes/tiny-a/build.lua",
        "function build_tools(slice, options, deps)\n"
        "    assert(slice.os == '" + os + "' and slice.platform == '" + platform + "' and slice.environment == '" + environment + "', slice.os)\n"
        "    assert(slice.toolchains.cmake)\n"
        "    otok.write(slice.prefix .. '/bin/tool.txt', 'from the build machine')\n"
        "end\n\n"
        "function build(slice, options, deps)\n"
        "    assert(slice.os == 'android' and slice.android.abi == 'x86_64', slice.os)\n"
        "    assert(otok.read(slice.tools.prefix .. '/bin/tool.txt') == 'from the build machine')\n"
        "    otok.cmake{ defines = { TINY_A_VALUE = options.value } }\n"
        "    return { libraries = { tiny_a = otok.glob('lib/*tiny_a*', slice.prefix)[1] }, headers = slice.prefix .. '/include' }\n"
        "end\n");

    ASSERT_EQ(p.install(), 0) << p.err.str() << p.out.str();
    // build read what build_tools wrote; the count has the target's slices only
    EXPECT_NE(p.out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << p.out.str();
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-a/lib/x86_64/libtiny_a.a")));
}
