// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The Windows target (docs/PLAN.md decision 75): the names of architectures, the versions of the CRT, the SDK and
// clang-cl and the CMake toolchain file on every host; a fake xwin tree and clang-cl on POSIX hosts; real builds where
// xwin's tree (OTOK_WINDOWS_SDK, else ~/devs/xwin) and LLVM's clang-cl are found.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "otok/commands/commands.hpp"
#include "otok/core/binary.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "otok/target/windows/windows_target.hpp"
#include "machine_targets.hpp"
#include "test_util.hpp"

using namespace otok;
using namespace otok::target::windows::detail;

namespace {

target_info const* const all_targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info(), &target::linux::info()};

#ifndef _WIN32

// xwin's tree with the files the target reads, for x86_64 only, and LLVM's tools as scripts in `llvm/`
std::string fake_tools(test::temp_dir& dir, std::string const& clang_version, bool debug_libraries = true)
{
    dir.write("xwin/crt/include/crtversion.h", "#pragma once\n#define _VC_CRT_MAJOR_VERSION 14\n\n#define _VC_CRT_MINOR_VERSION 44\n"
                                               "#define _VC_CRT_BUILD_VERSION 35220\n#define _VC_CRT_RBUILD_VERSION 0\n");
    dir.write("xwin/crt/include/yvals_core.h", "#elif defined(__clang__)\n#if __clang_major__ < 19\n_EMIT_STL_ERROR(STL1000, \"…\");\n");
    dir.write("xwin/crt/lib/x86_64/msvcrt.lib", "!<arch>\n");
    if (debug_libraries) dir.write("xwin/crt/lib/x86_64/msvcrtd.lib", "!<arch>\n");
    dir.write("xwin/sdk/include/ucrt/stdio.h", "");
    dir.write("xwin/sdk/include/10.0.22621/README", "");
    dir.write("xwin/sdk/include/10.0.26100/README", "");
    dir.write("xwin/sdk/lib/um/x86_64/kernel32.lib", "!<arch>\n");
    dir.write("llvm/clang-cl", "#!/bin/sh\necho 'Ubuntu clang version " + clang_version + " (6ubuntu1)'\necho 'Target: x86_64-pc-linux-gnu'\n");
    for (char const* tool : {"lld-link", "llvm-lib", "llvm-rc", "llvm-mt"}) dir.write(std::string{"llvm/"} + tool, "#!/bin/sh\n");
    for (char const* tool : {"clang-cl", "lld-link", "llvm-lib", "llvm-rc", "llvm-mt"}) {
        std::filesystem::permissions(dir.path() / "llvm" / tool, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    }
    // as Debian and Ubuntu install it: clang-cl-<major> in a directory of the PATH, a link into LLVM's own
    std::filesystem::create_directories(dir.path() / "bin");
    std::filesystem::create_symlink(dir.path() / "llvm" / "clang-cl", dir.path() / "bin" / ("clang-cl-" + clang_version.substr(0, 2)));
    return path_to_utf8(std::filesystem::canonical(dir.path() / "xwin"));
}

#endif

// A slice built across from Linux with xwin's CRT and SDK (`linux/clang-cl`).
slice_spec xwin_slice(std::string const& arch, std::string const& configuration = "release")
{
    return {"windows", arch, "", configuration, "static", {}, {"linux", "", "clang-cl"}};
}

}

TEST(windows_target, describes_one_platform_of_two_architectures)
{
    target_info const& t = target::windows::info();
    ASSERT_EQ(t.platforms.size(), 1u);
    EXPECT_EQ(t.platforms[0].name, "windows");
    EXPECT_EQ(t.platforms[0].architectures, (std::vector<std::string>{"x86_64", "arm64"}));
    EXPECT_EQ(t.tool_platform, "windows");  // the tool slices of a Windows machine (otok-toml.md 7)
    EXPECT_EQ(t.host_system, "windows");
    EXPECT_EQ(t.linkages, (std::vector<std::string>{"static", "dynamic"}));
    EXPECT_TRUE(t.many_configurations);
    for (char const* e : {"windows/msvc", "windows-arm64/clang-cl", "linux/clang-cl"}) EXPECT_TRUE(t.has_environment(*parse_environment(e))) << e;
    for (char const* e : {"linux/msvc", "macos/clang-cl", "*/clang-cl"}) EXPECT_FALSE(t.has_environment(*parse_environment(e))) << e;
}

TEST(windows_target, names_the_architectures_of_vcvarsall)
{
    EXPECT_EQ(target::windows::detail::vcvars_arch("x86_64", "x86_64"), "x64");
    EXPECT_EQ(target::windows::detail::vcvars_arch("x86_64", "arm64"), "x64_arm64");
    EXPECT_EQ(target::windows::detail::vcvars_arch("arm64", "arm64"), "arm64");
    EXPECT_EQ(target::windows::detail::vcvars_arch("arm64", "x86_64"), "arm64_x64");
}

TEST(windows_target, parses_the_output_of_set)
{
    env_vars const vars = target::windows::detail::parse_set_output("ALLUSERSPROFILE=C:\\ProgramData\r\nINCLUDE=a;b=c\r\n=C:=C:\\\r\nempty=\r\n");
    EXPECT_EQ(vars, (env_vars{{"ALLUSERSPROFILE", "C:\\ProgramData"}, {"INCLUDE", "a;b=c"}, {"empty", ""}}));
}

TEST(windows_target, names_architectures)
{
    auto x64 = names_of("x86_64");
    ASSERT_TRUE(x64);
    EXPECT_EQ(x64->xwin, "x86_64");
    EXPECT_EQ(x64->triple, "x86_64-pc-windows-msvc");
    EXPECT_EQ(x64->coff_machine, 0x8664u);
    EXPECT_EQ(x64->cmake_processor, "AMD64");
    auto arm = names_of("arm64");
    ASSERT_TRUE(arm);
    EXPECT_EQ(arm->xwin, "aarch64");
    EXPECT_EQ(arm->triple, "aarch64-pc-windows-msvc");
    EXPECT_EQ(arm->coff_machine, 0xAA64u);
    EXPECT_EQ(arm->goarch, "arm64");
    EXPECT_FALSE(names_of("x86"));
}

TEST(windows_target, finds_the_helpers_of_compiler_rt)
{
    EXPECT_EQ(compiler_rt_helpers("\na.obj:\n__udivti3\nmemcpy\n__udivti3\n\nb.obj:\n__divti3\r\n__udivti3x\n"),
        (std::vector<std::string>{"__divti3", "__udivti3"}));
    EXPECT_TRUE(compiler_rt_helpers("memcpy\n__chkstk\n").empty());
}

TEST(windows_target, reads_versions)
{
    EXPECT_EQ(crt_version("#define _VC_CRT_MAJOR_VERSION 14\n#define _VC_CRT_MINOR_VERSION 44\n#define _VC_CRT_BUILD_VERSION 35220\n"), "14.44.35220");
    EXPECT_EQ(crt_version("#define _VC_CRT_MAJOR_VERSION 14\n"), "");
    EXPECT_EQ(clang_minimum("#if __clang_major__ < 19\n_EMIT_STL_ERROR(STL1000, \"expected Clang 19.0.0 or newer\");\n"), 19);
    EXPECT_EQ(clang_minimum("no requirement"), 0);
    EXPECT_EQ(clang_version("Ubuntu clang version 21.1.8 (6ubuntu1)\nTarget: x86_64-pc-linux-gnu\n"), "21.1.8");
    EXPECT_EQ(clang_version("clang version 19.1.0\n"), "19.1.0");
    EXPECT_EQ(clang_version("cl.exe\n"), "");

    test::temp_dir dir;
    dir.write("include/ucrt/stdio.h", "");
    EXPECT_EQ(sdk_version(dir.path() / "include"), "");
    dir.write("include/10.0.22621/x", "");
    dir.write("include/10.0.26100/x", "");
    EXPECT_EQ(sdk_version(dir.path() / "include"), "10.0.26100");
}

TEST(windows_target, writes_the_cmake_toolchain_file)
{
    EXPECT_EQ(cmake_toolchain({"arm64", "aarch64-pc-windows-msvc", "ARM64", "/l/clang-cl", "/l/lld-link", "/l/llvm-lib", "/l/llvm-rc", "/l/llvm-mt", true}),
        "# Generated by otok for a Windows slice (arm64), built with clang-cl.\n"
        "set(CMAKE_SYSTEM_NAME Windows)\n"
        "set(CMAKE_SYSTEM_VERSION 10.0)\n"
        "set(CMAKE_SYSTEM_PROCESSOR ARM64)\n"
        "set(CMAKE_C_COMPILER \"/l/clang-cl\")\n"
        "set(CMAKE_CXX_COMPILER \"/l/clang-cl\")\n"
        "set(CMAKE_ASM_COMPILER_TARGET aarch64-pc-windows-msvc)\n"
        "set(CMAKE_LINKER \"/l/lld-link\" CACHE FILEPATH \"\")\n"
        "set(CMAKE_AR \"/l/llvm-lib\" CACHE FILEPATH \"\")\n"
        "set(CMAKE_RC_COMPILER \"/l/llvm-rc\" CACHE FILEPATH \"\")\n"
        "set(CMAKE_MT \"/l/llvm-mt\" CACHE FILEPATH \"\")\n"
        "set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreadedDebugDLL\")\n"
        "set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT \"\")\n"
        "set(CMAKE_MSVC_RUNTIME_CHECKS \"\")\n"
        "set(CMAKE_TRY_COMPILE_CONFIGURATION Debug)\n"
        "# nothing from the build machine, only the dependencies (docs/PLAN.md decision 25)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n");
}

#ifndef _WIN32

TEST(windows_target, needs_the_sdk_and_clang_cl)
{
    test::temp_dir dir;
    std::string const sdk = fake_tools(dir, "21.1.8");
    env_vars const env{{"PATH", path_to_utf8(dir.path() / "bin")}};
    auto const error = [&](std::string const& configured, env_vars const& e, slice_spec const& slice = xwin_slice("x86_64")) {
        auto p = target::windows::make_build_target("", configured, "", e)->profile(slice);
        return p ? std::string{"no error"} : p.error().message;
    };
    EXPECT_EQ(error("", env), "the Windows target needs the CRT and the Windows SDK: set 'windows-sdk' in config.toml (or OTOK_WINDOWS_SDK) to a "
                              "directory made by `xwin --accept-license splat --include-debug-libs --output <dir>`");
    std::string const llvm = path_to_utf8(dir.path() / "llvm");
    EXPECT_EQ(error(llvm, env), "'windows-sdk' names '" + llvm + "', which is not a directory of `xwin splat` (it has no crt/include/crtversion.h, "
                                "crt/include/yvals_core.h or sdk/include/ucrt)");
    EXPECT_EQ(error(sdk, {{"PATH", path_to_utf8(dir.path() / "xwin")}}),
        "the Windows target needs clang-cl (LLVM) in PATH, as clang-cl or clang-cl-<version>, or set 'llvm' in config.toml; install LLVM with its lld");

    // 'llvm' of config.toml: its clang-cl, not the PATH's
    std::string const nowhere = path_to_utf8(dir.path() / "nowhere");
    auto missing = target::windows::make_build_target("", sdk, nowhere, env)->profile(xwin_slice("x86_64"));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().message, "'llvm' names '" + nowhere + "', which has no bin/clang-cl (an LLVM installation)");
    dir.write("llvm-22/bin/clang-cl", "#!/bin/sh\necho 'clang version 22.1.8'\necho 'Target: x86_64-unknown-linux-gnu'\n");
    for (char const* tool : {"lld-link", "llvm-lib", "llvm-rc", "llvm-mt"}) dir.write(std::string{"llvm-22/bin/"} + tool, "#!/bin/sh\n");
    for (char const* tool : {"clang-cl", "lld-link", "llvm-lib", "llvm-rc", "llvm-mt"}) {
        std::filesystem::permissions(dir.path() / "llvm-22" / "bin" / tool, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    }
    std::string const llvm22 = path_to_utf8(std::filesystem::canonical(dir.path() / "llvm-22"));
    auto configured = target::windows::make_build_target("", sdk, llvm22, env)->profile(xwin_slice("x86_64"));
    ASSERT_TRUE(configured) << configured.error().message;
    EXPECT_EQ(configured->toolchain.cc, llvm22 + "/bin/clang-cl");
    EXPECT_EQ(configured->identity.at("clang-cl"), "22.1.8");
    EXPECT_EQ(error(sdk, env, xwin_slice("arm64")), "'" + sdk + "' has no libraries for arm64: make it with `xwin --arch aarch64 … splat`");
    EXPECT_EQ(error(sdk, env, xwin_slice("x86")), "the Windows target has no slice windows-x86");

    // the STL's oldest Clang
    test::temp_dir old;
    std::string const old_sdk = fake_tools(old, "18.1.3", false);
    std::string const old_clang = path_to_utf8(std::filesystem::canonical(old.path() / "llvm" / "clang-cl"));
    EXPECT_EQ(error(old_sdk, {{"PATH", path_to_utf8(old.path() / "bin")}}),
        "the STL of the CRT 14.44.35220 at '" + old_sdk + "' needs Clang 19 or newer, and '" + old_clang + "' is 18.1.3");
}

TEST(windows_target, describes_slices)
{
    test::temp_dir dir;
    std::string const sdk = fake_tools(dir, "21.1.8");
    std::string const llvm = path_to_utf8(std::filesystem::canonical(dir.path() / "llvm"));
    auto target = target::windows::make_build_target("", sdk, "", {{"PATH", path_to_utf8(dir.path() / "bin")}});
    auto p = target->profile(xwin_slice("x86_64", "debug"));
    ASSERT_TRUE(p) << p.error().message;
    EXPECT_EQ(p->triple, "x86_64-pc-windows-msvc");
    EXPECT_EQ(p->identity, (std::map<std::string, std::string>{{"clang-cl", "21.1.8"}, {"crt", "14.44.35220"}, {"windows-sdk", "10.0.26100"}}));
    EXPECT_EQ(p->description, "clang-cl 21.1.8 at '" + llvm + "', the CRT 14.44.35220 and the Windows SDK 10.0.26100 at '" + sdk + "'");
    EXPECT_EQ(p->toolchain.cc, llvm + "/clang-cl");
    EXPECT_EQ(p->toolchain.ar, llvm + "/llvm-lib");
    std::vector<std::string> const flags{"--target=x86_64-pc-windows-msvc", "-fuse-ld=lld-link", "-fms-compatibility-version=19.44", "/Brepro", "/Od", "/Z7",
        "/MDd"};
    EXPECT_EQ(p->toolchain.cflags, flags);
    EXPECT_EQ(p->toolchain.driver_flags, (std::vector<std::string>{"--target=x86_64-pc-windows-msvc"}));  // build-lua-api.md 3.1
    std::vector<std::string> cxxflags = flags;
    cxxflags.push_back("-Wno-register");  // as MSVC
    EXPECT_EQ(p->toolchain.cxxflags, cxxflags);
    EXPECT_EQ(p->toolchain.env.at("PATH"), llvm + ":/usr/bin:/bin:/usr/sbin:/sbin");
    EXPECT_EQ(p->toolchain.env.at("INCLUDE"), sdk + "/crt/include;" + sdk + "/sdk/include/ucrt;" + sdk + "/sdk/include/um;" + sdk + "/sdk/include/shared;"
        + sdk + "/sdk/include/winrt;" + sdk + "/sdk/include/cppwinrt");
    EXPECT_EQ(p->toolchain.env.at("LIB"), sdk + "/crt/lib/x86_64;" + sdk + "/sdk/lib/um/x86_64;" + sdk + "/sdk/lib/ucrt/x86_64");
    EXPECT_EQ(p->meson_host.at("system"), "windows");
    EXPECT_EQ(p->meson_binaries, (std::map<std::string, std::vector<std::string>>{{"c", {llvm + "/clang-cl", "--target=x86_64-pc-windows-msvc"}},
                                     {"cpp", {llvm + "/clang-cl", "--target=x86_64-pc-windows-msvc"}}, {"c_ld", {llvm + "/lld-link"}},
                                     {"cpp_ld", {llvm + "/lld-link"}}, {"windres", {llvm + "/llvm-rc"}}}));
    EXPECT_EQ(p->prefix_map_flag, "/clang:-ffile-prefix-map=");
    EXPECT_EQ(p->standard_flag(language::cxx, 20), "/std:c++20");
    EXPECT_EQ(p->target_fields.at("rust_target"), "x86_64-pc-windows-msvc");
    EXPECT_NE(p->cmake_toolchain.find("set(CMAKE_LINKER \"" + llvm + "/lld-link\" CACHE FILEPATH \"\")\n"), std::string::npos) << p->cmake_toolchain;

    // a tree without the debug libraries builds release only
    test::temp_dir release_only;
    std::string const sdk2 = fake_tools(release_only, "21.1.8", false);
    auto target2 = target::windows::make_build_target("", sdk2, "", {{"PATH", path_to_utf8(release_only.path() / "bin")}});
    EXPECT_TRUE(target2->profile(xwin_slice("x86_64")));
    auto debug = target2->profile(xwin_slice("x86_64", "debug"));
    ASSERT_FALSE(debug);
    EXPECT_EQ(debug.error().message, "'" + sdk2 + "' has no debug libraries of the CRT for x86_64 (msvcrtd.lib): make it with `xwin … splat "
                                     "--include-debug-libs`");
}

#endif

TEST(windows_target, checks_libraries)
{
    test::temp_dir dir;
    auto const member = [](std::string const& name, std::string const& data) {
        std::string size = std::to_string(data.size());
        return name + std::string(16 - name.size(), ' ') + std::string(32, ' ') + size + std::string(10 - size.size(), ' ') + "`\n" + data;
    };
    dir.write("x64.lib", "!<arch>\n" + member("a.obj/", std::string{"\x64\x86", 2} + std::string(18, '\0')));
    dir.write("empty.lib", "!<arch>\n");
    auto target = target::windows::make_build_target("", "", "", {});
    EXPECT_TRUE(target->check_library(dir.path() / "x64.lib", {"windows", "x86_64"}));
    auto wrong = target->check_library(dir.path() / "x64.lib", {"windows", "arm64"});
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().message, "its objects are for the COFF machines 0x8664, not for arm64 (0xAA64)");
    auto empty = target->check_library(dir.path() / "empty.lib", {"windows", "x86_64"});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().message, "it holds no COFF object");
}

TEST(windows_target, packages_include_lib_and_bin_per_architecture)
{
    test::temp_dir dir;
    auto target = target::windows::make_build_target("", "", "", {});
    built_package package;
    package.library = "z";
    auto const slice = [&](std::string const& prefix, std::string const& arch, std::string const& configuration) {
        dir.write(prefix + "/include/z.h", "z\n");
        dir.write(prefix + "/lib/z.lib", "!<arch>\n");
        dir.write(prefix + "/bin/z.dll", "MZ");
        built_slice s;
        s.spec = {"windows", arch, "", configuration, "dynamic"};
        s.output.headers = dir.path() / prefix / "include";
        s.output.libraries["z"] = dir.path() / prefix / "lib" / "z.lib";
        s.output.runtimes["z"] = dir.path() / prefix / "bin" / "z.dll";
        return s;
    };
    package.slices = {slice("a", "x86_64", "release"), slice("b", "arm64", "release"), slice("c", "x86_64", "debug")};
    recipe const rec{};
    ASSERT_TRUE(target->make_package(package, rec, {}, dir.path() / "out"));
    EXPECT_EQ(read_file(dir.path() / "out" / "include" / "z.h").value_or("?"), "z\n");
    for (std::string const sub : {"lib/x86_64/z.lib", "bin/x86_64/z.dll", "lib/arm64/z.lib", "bin/arm64/z.dll", "debug/lib/x86_64/z.lib",
             "debug/bin/x86_64/z.dll"}) {
        EXPECT_TRUE(std::filesystem::exists(dir.path() / "out" / path_from_utf8(sub))) << sub;
    }

    // which file is which library, and the toolset of the newest CRT (otok-toml.md 4.11)
    auto files = read_package_files(dir.path() / "out");
    ASSERT_TRUE(files) << files.error().message;
    EXPECT_EQ(files->files.size(), 3u);
    EXPECT_EQ(files->files[0], (package_library_file{"z", "x86_64", "release", "lib/x86_64/z.lib", "bin/x86_64/z.dll", "", "", true}));
    EXPECT_EQ(files->files[2], (package_library_file{"z", "x86_64", "debug", "debug/lib/x86_64/z.lib", "debug/bin/x86_64/z.dll", "", "", true}));
    EXPECT_FALSE(files->properties.contains("crt"));  // slices without a key.txt
    dir.write("a/key.txt", "toolchain.crt = 14.44.35207\ntoolchain.msvc = 14.44.35207\n");
    dir.write("b/key.txt", "toolchain.crt = 14.51.36231\n");
    package.slices[0].entry = dir.path() / "a";
    package.slices[1].entry = dir.path() / "b";
    ASSERT_TRUE(target->make_package(package, rec, {}, dir.path() / "out3"));
    EXPECT_EQ(read_package_files(dir.path() / "out3").value_or(package_files{}).properties, (std::map<std::string, std::string>{{"crt", "14.51"}}));

    // one include/ for every slice
    dir.write("b/include/z.h", "other\n");
    auto differ = target->make_package(package, rec, {}, dir.path() / "out2");
    ASSERT_FALSE(differ);
    EXPECT_EQ(differ.error().message, "the header 'z.h' of windows-arm64 differs from that of another slice: the Windows target keeps one include/ "
                                      "for all architectures and configurations");
}

TEST(windows_target, reads_the_toolset_of_the_crt)
{
    EXPECT_EQ(crt_of_key("library = z\ntoolchain.crt = 14.44.35220\r\ntoolchain.windows-sdk = 10.0.26100\n"), "14.44");
    EXPECT_EQ(crt_of_key("toolchain.crt = 14\n"), "");
    EXPECT_EQ(crt_of_key("library = z\n"), "");
}

namespace {

// An output of a static library of two products for x86_64 in release, with the toolset of its CRT.
consumer_package windows_output()
{
    consumer_package p;
    p.architectures = {"x86_64"};
    p.configurations = {"release"};
    p.properties = {{"crt", "14.44"}};
    p.libraries = {{"tiny-a_tiny_a", "tiny-a", "tiny-a/include", false, {{"tiny_a", "x86_64", "release", "tiny-a/lib/x86_64/tiny_a.lib"}}, {}, {"ws2_32"}},
        {"tiny-c_tiny_c", "tiny-c", "tiny-c/include", true,
            {{"tiny_c", "x86_64", "release", "tiny-c/lib/x86_64/tiny_c.lib", "tiny-c/bin/x86_64/tiny_c.dll", "", "", true}}, {}, {}, false}};
    p.products = {{"TinyA", {"tiny-a_tiny_a"}, {}, {}, false, {"TINY_A_EXTRA=5", "Q=\"a;b\""}}, {"TinyC", {"tiny-c_tiny_c"}, {}, {}, false, {}}};
    p.data_dirs = {{"tiny-a", "tiny-a/data"}};
    return p;
}

}

// The toolset of MSVC the libraries need, the architecture of the generator platform or of the compiler (otok-toml.md 4.11).
TEST(windows_target, writes_its_parts_of_the_cmake_config)
{
    consumer_cmake_parts const parts = cmake_parts(windows_output());
    EXPECT_TRUE(parts.strict_configurations);
    EXPECT_TRUE(parts.runtime_dlls);
    EXPECT_EQ(parts.link_item("ws2_32"), "ws2_32.lib");
    EXPECT_EQ(parts.link_item("Crypt32.Lib"), "Crypt32.Lib");
    EXPECT_NE(parts.detect.find("if(CMAKE_VS_PLATFORM_NAME)\n  set(_otok_arch \"${CMAKE_VS_PLATFORM_NAME}\")\n"), std::string::npos) << parts.detect;
    EXPECT_NE(parts.detect.find("if(_otok_arch MATCHES \"^(x64|amd64|x86_64)$\")\n  set(_otok_arch \"x86_64\")\n"), std::string::npos) << parts.detect;
    EXPECT_NE(parts.checks.find("if(_otok_cl MATCHES \"^19\\\\.([0-9]+)\" AND CMAKE_MATCH_1 LESS 44)\n"
                                "  message(FATAL_ERROR \"otok: the libraries of ${_otok_dir} were built with the toolset 14.44 of MSVC and link with it or a newer "
                                "one, not with 14.${CMAKE_MATCH_1} (cl ${_otok_cl})\")\n"),
        std::string::npos)
        << parts.checks;
    consumer_package p = windows_output();
    p.properties.clear();
    EXPECT_EQ(cmake_parts(p).checks, "");
}

TEST(windows_target, writes_the_property_sheet)
{
    EXPECT_EQ(msbuild_props(windows_output()),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<!-- Generated by otok; do not edit. The libraries of this output for projects of Visual Studio (otok-toml.md 4.11):\n"
        "     import otok.props before the project's settings (or from Directory.Build.props) and otok.targets after them (or from\n"
        "     Directory.Build.targets), and name the products a project links in OtokProducts: <OtokProducts>LZ4;Boost.JSON</OtokProducts>. -->\n"
        "<Project>\n"
        "  <PropertyGroup>\n"
        "    <OtokDir>$(MSBuildThisFileDirectory)</OtokDir>\n"
        "    <OtokAcknowledgements>$(OtokDir)Acknowledgements.md</OtokAcknowledgements>\n"
        "    <OtokDataDir_tiny_a>$(OtokDir)tiny-a\\data\\</OtokDataDir_tiny_a>\n"
        "  </PropertyGroup>\n"
        "</Project>\n");
}

TEST(windows_target, writes_the_targets_of_products)
{
    std::string const text = msbuild_targets(windows_output());
    EXPECT_NE(text.find("    <OtokIncludePath>$(OtokDir)tiny-a\\include;$(OtokDir)tiny-c\\include</OtokIncludePath>\n"
                        "    <OtokLibraryPath>$(OtokDir)tiny-a\\$(_OtokRoot)lib\\$(OtokArch);$(OtokDir)tiny-c\\$(_OtokRoot)lib\\$(OtokArch)</OtokLibraryPath>\n"),
        std::string::npos)
        << text;
    EXPECT_NE(text.find("  <PropertyGroup Condition=\"'$(OtokArch)|$(OtokConfiguration)' == 'x86_64|release'\">\n"
                        "    <_OtokLib0>$(OtokDir)tiny-a\\lib\\x86_64\\tiny_a.lib</_OtokLib0><!-- tiny-a_tiny_a -->\n"
                        "    <_OtokLib1>$(OtokDir)tiny-c\\lib\\x86_64\\tiny_c.lib</_OtokLib1><!-- tiny-c_tiny_c -->\n"
                        "    <_OtokDll1>$(OtokDir)tiny-c\\bin\\x86_64\\tiny_c.dll</_OtokDll1>\n"
                        "  </PropertyGroup>\n"),
        std::string::npos)
        << text;
    // a product's libraries, system libraries and defines, MSBuild's characters escaped
    EXPECT_NE(text.find("  <PropertyGroup Condition=\"$(_OtokProducts.Contains(';tinya;'))\">\n"
                        "    <OtokLibraries>$(OtokLibraries);$(_OtokLib0);ws2_32.lib</OtokLibraries>\n"
                        "    <OtokDefinitions>$(OtokDefinitions);TINY_A_EXTRA=5;Q=&quot;a%3Bb&quot;</OtokDefinitions>\n"
                        "  </PropertyGroup>\n"
                        "  <PropertyGroup Condition=\"$(_OtokProducts.Contains(';tinyc;'))\">\n"
                        "    <OtokLibraries>$(OtokLibraries);$(_OtokLib1)</OtokLibraries>\n"
                        "    <OtokDlls>$(OtokDlls);$(_OtokDll1)</OtokDlls>\n"
                        "  </PropertyGroup>\n"),
        std::string::npos)
        << text;
}

// What a project of Visual Studio cannot have, each refused with its cure (otok-toml.md 4.11).
TEST(windows_target, refuses_what_the_output_lacks_in_msbuild)
{
    std::string const text = msbuild_targets(windows_output());
    EXPECT_NE(text.find("<Error Condition=\"'$(OtokArch)' == ''\" Text=\"otok: $(OtokDir) has no libraries for the platform '$(Platform)': it has x64\" />\n"),
        std::string::npos)
        << text;
    EXPECT_NE(text.find("<Error Condition=\"'$(OtokArch)' == 'arm64'\" Text=\"otok: $(OtokDir) has no libraries for the platform '$(Platform)': add 'arm64' "
                        "to 'architectures' of [windows] in otok.toml and run otok install\" />\n"),
        std::string::npos)
        << text;
    EXPECT_NE(text.find("<Error Condition=\"'$(OtokConfiguration)' == 'debug'\" Text=\"otok: $(OtokDir) has no libraries of the configuration 'debug', which "
                        "'$(Configuration)' links with (the debug CRT): add &quot;debug&quot; to 'configurations' of [windows] in otok.toml and run otok "
                        "install\" />\n"),
        std::string::npos)
        << text;
    EXPECT_NE(text.find("<Error Condition=\"'$(VCToolsVersion)' != '' and $([MSBuild]::VersionLessThan('$(VCToolsVersion)', '14.44'))\" Text=\"otok: the "
                        "libraries of $(OtokDir) were built with the toolset 14.44 of MSVC and link with it or a newer one, not with $(VCToolsVersion)\" />\n"),
        std::string::npos)
        << text;
    EXPECT_NE(text.find("      <_OtokUnknown Include=\"@(_OtokRequested)\" Exclude=\"TinyA;TinyC\" />\n    </ItemGroup>\n"
                        "    <Error Condition=\"'@(_OtokUnknown)' != ''\" Text=\"otok: $(OtokDir) has no product '@(_OtokUnknown)' of OtokProducts; it has "
                        "TinyA, TinyC\" />\n"),
        std::string::npos)
        << text;
}

namespace {

// A project of tests/data/projects for Windows across from Linux (`linux/clang-cl`), installed by otok commands with the
// real targets of this machine.
class windows_project
{
public:
    windows_project(std::string const& fixture, std::string const& windows_table)
    {
        base_ = std::filesystem::canonical(dir_.path());
        project_ = base_ / "project";
        auto copied = copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/" + fixture), project_);
        EXPECT_TRUE(copied) << copied.error().message;
        // the fixture's [linux] table becomes [windows]
        std::string manifest = read_file(project_ / "otok.toml").value_or("");
        std::size_t const at = manifest.find("[linux]\n");
        std::size_t const end = manifest.find("\n[deps.", at);
        manifest.replace(at, end - at, "[windows]\nbuild-on = [\"linux/clang-cl\"]\n" + windows_table);
        dir_.write("project/otok.toml", manifest);
        env_vars const env = current_environment();
        if (auto it = env.find("OTOK_WINDOWS_SDK"); it != env.end()) {
            sdk_ = it->second;
        } else if (auto home = env.find("HOME"); home != env.end()) {
            sdk_ = home->second + "/devs/xwin";
        }
    }

    // Why this machine cannot build Windows slices; empty when it can.
    std::string missing_tools(std::vector<std::string> const& tools = {"cmake", "ninja"})
    {
        auto target = target::windows::make_build_target("", sdk_, "", current_environment());
        auto p = target->profile(xwin_slice("x86_64"));
        if (!p) return p.error().message;
        env_vars env = current_environment();
        for (std::string const& tool : tools) {
            if (!find_program(tool, env, base_)) return tool + " is not in PATH";
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
        std::string const sdk = sdk_;
        command_context ctx{project_, all_targets, nullptr, out, err, [cache, sdk](std::string_view name) -> std::optional<std::string> {
            if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache);
            if (name == "OTOK_WINDOWS_SDK") return sdk;
            if (name == "PATH") return process_environment()(name);  // the toolchains (docs/PLAN.md decision 38) and bwrap
            return std::nullopt;
        }};
        ctx.make_build_targets = [](machine_config const& config) { return test::machine_build_targets(config, current_environment()); };
        return run_command(*parsed, ctx);
    }

    std::filesystem::path output(std::string const& relative) const { return project_ / "Otok" / path_from_utf8(relative); }

    void write(std::string const& relative, std::string const& text) { dir_.write("project/" + relative, text); }

    // Links a program of `source` against libraries of the output for `arch`, as a consumer's MSVC build would.
    result<void> link_consumer(std::string const& source, std::string const& arch, std::vector<std::string> const& libraries)
    {
        dir_.write("consumer.c", source);
        auto names = names_of(arch);
        std::filesystem::path const object = base_ / ("consumer-" + arch + ".obj");
        std::vector<std::string> compile{profile_.toolchain.cc, "--target=" + names->triple, "/MD", "/c", "/Fo" + path_to_utf8(object)};
        for (std::string const& library : libraries) compile.insert(compile.end(), {"-imsvc", path_to_utf8(output(library + "/include"))});
        compile.push_back(path_to_utf8(base_ / "consumer.c"));
        // the libraries of this architecture, as LIB names them for the linker
        env_vars env = profile_.toolchain.env;
        std::string lib = env["LIB"];
        for (std::string const& from : {std::string{"/x86_64"}}) {
            for (std::size_t at = lib.find(from); at != std::string::npos; at = lib.find(from, at + 1)) lib.replace(at, from.size(), "/" + names->xwin);
        }
        env["LIB"] = lib;
        std::vector<std::string> link{profile_.meson_binaries.at("c_ld").front(), "/out:" + path_to_utf8(consumer(arch)), path_to_utf8(object)};
        for (std::string const& library : libraries) link.push_back("/libpath:" + path_to_utf8(output(library + "/lib/" + arch)));
        for (std::string library : libraries) {
            std::ranges::replace(library, '-', '_');  // tiny-b builds tiny_b.lib
            link.push_back(library + ".lib");
        }
        for (auto const& argv : {compile, link}) {
            auto ran = run_process({argv, env, base_, base_ / "consumer.log", false});
            if (!ran) return std::unexpected{ran.error()};
            if (!ran->success()) return fail(read_file(base_ / "consumer.log").value_or("the consumer did not build"));
        }
        return {};
    }

    std::filesystem::path consumer(std::string const& arch) const { return base_ / ("consumer-" + arch + ".exe"); }

    std::ostringstream out, err;

private:
    test::temp_dir dir_;
    std::filesystem::path base_, project_;
    std::string sdk_;
    toolchain_profile profile_;
};

}

TEST(windows_target, installs_static_libraries_for_two_architectures)
{
    windows_project p{"tiny", "architectures = { windows = [\"x86_64\", \"arm64\"] }\ncflags = [\"-DTINY_BONUS=2\"]\n"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.install(), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.out.str().find("slices: 4 (0 from the cache)\n"), std::string::npos) << p.out.str();
    for (std::string const arch : {"x86_64", "arm64"}) {
        std::filesystem::path const lib = p.output("tiny-a/lib/" + arch + "/tiny_a.lib");
        ASSERT_TRUE(std::filesystem::exists(lib)) << arch;
        EXPECT_EQ(coff_machines_of(lib), std::vector<unsigned>{names_of(arch)->coff_machine});
        EXPECT_TRUE(std::filesystem::exists(p.output("tiny-b/lib/" + arch + "/tiny_b.lib"))) << arch;
        auto linked = p.link_consumer("#include <tiny_b.h>\n\nint main(void) { return tiny_b(); }\n", arch, {"tiny-b", "tiny-a"});
        ASSERT_TRUE(linked) << linked.error().message;
        EXPECT_EQ(binary_kind_of(p.consumer(arch)), binary_kind::pe_other);
        EXPECT_EQ(coff_machines_of(p.consumer(arch)), std::vector<unsigned>{names_of(arch)->coff_machine});
    }
    EXPECT_TRUE(std::filesystem::exists(p.output("tiny-a/include/tiny_a.h")));

    ASSERT_EQ(p.install(), 0) << p.err.str();
    EXPECT_NE(p.out.str().find("slices: 4 (4 from the cache)\n"), std::string::npos) << p.out.str();
}

TEST(windows_target, installs_dlls_in_both_configurations)
{
    windows_project p{"dynamic", "architectures = { windows = [\"x86_64\"] }\nconfigurations = [\"debug\", \"release\"]\nlinkage = \"dynamic\"\n"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.install(), 0) << p.err.str() << p.out.str();
    EXPECT_NE(p.out.str().find("slices: 4 (0 from the cache)\n"), std::string::npos) << p.out.str();
    for (std::string const root : {"", "debug/"}) {
        std::vector<std::filesystem::path> dlls;
        std::error_code ec;
        for (auto const& e : std::filesystem::directory_iterator{p.output("tiny-c/" + root + "bin/x86_64"), ec}) {
            if (e.path().extension() == ".dll") dlls.push_back(e.path());
        }
        ASSERT_EQ(dlls.size(), 1u) << root;
        EXPECT_EQ(binary_kind_of(dlls[0]), binary_kind::pe_dll);
        EXPECT_EQ(coff_machines_of(dlls[0]), std::vector<unsigned>{0x8664});
    }
    auto linked = p.link_consumer("#include <tiny_c.h>\n\nint main(void) { return tiny_c(); }\n", "x86_64", {"tiny-c", "tiny-d"});
    EXPECT_TRUE(linked) << linked.error().message;
}

TEST(windows_target, builds_with_meson)
{
    windows_project p{"meson", "architectures = { windows = [\"arm64\"] }\ncflags = [\"-DTINY_M_BONUS=2\"]\n"};
    if (std::string const missing = p.missing_tools({"meson", "ninja"}); !missing.empty()) GTEST_SKIP() << missing;

    ASSERT_EQ(p.install(), 0) << p.err.str() << p.out.str();
    std::filesystem::path const lib = p.output("tiny-m/lib/arm64/libtiny_m.a");
    ASSERT_TRUE(std::filesystem::exists(lib)) << p.out.str();
    EXPECT_EQ(coff_machines_of(lib), std::vector<unsigned>{0xAA64});
}

// docs/PLAN.md decision 76: 128-bit division calls compiler-rt, which MSVC's runtime lacks
TEST(windows_target, refuses_libraries_that_need_compiler_rt)
{
    windows_project p{"tiny", "architectures = { windows = [\"x86_64\"] }\ncflags = [\"-DTINY_BONUS=2\"]\n"};
    if (std::string const missing = p.missing_tools(); !missing.empty()) GTEST_SKIP() << missing;
    p.write("vendor/tiny-a/tiny_a.c", "#include \"tiny_a.h\"\n\nint tiny_a(void) { return TINY_A_VALUE + TINY_BONUS; }\n\n"
                                      "unsigned long long tiny_a_divide(unsigned __int128 a, unsigned __int128 b) { return (unsigned long long)(a / b); }\n");

    ASSERT_EQ(p.install(), 1) << p.out.str();
    EXPECT_NE(p.err.str().find("it calls __udivti3 of compiler-rt (128-bit integers, which clang has and MSVC has not)"), std::string::npos) << p.err.str();
}
