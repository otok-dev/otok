// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "otok/builder/consumer.hpp"
#include "otok/builder/target.hpp"
#include "otok/core/process.hpp"
#include "otok/core/target_info.hpp"

// The Windows target: libraries for programs built with MSVC (docs/PLAN.md decisions 75, 82, otok-toml.md 4.8), in
// one of three environments: `windows/msvc`, cl.exe of Visual Studio; `windows/clang-cl`, clang-cl with Visual Studio's
// CRT and SDK; `linux/clang-cl`, clang-cl across from Linux against the CRT and the Windows SDK as `xwin splat` lays
// them out, its builds in the sandbox.
namespace otok::target::windows {

// One platform, `windows`, with the architectures x86_64 and arm64.
target_info const& info();

// The target for builds. `visual_studio` is config.md's `visual-studio` (a major version, a product year or an
// installation path; empty: the newest with the C++ tools), `sdk` its `windows-sdk`, the output directory of `xwin
// splat` (empty: none, and every slice of `linux/clang-cl` fails saying how to make one), `llvm` its `llvm`, the LLVM
// directory of clang-cl and its tools (empty: found in the PATH of `environment`, otok's own, and on Windows also in Visual
// Studio's LLVM).
std::unique_ptr<build_target> make_build_target(std::string visual_studio, std::string sdk, std::string llvm, env_vars environment);

namespace detail {

// The variables printed by cmd's `set` ("NAME=value" lines).
env_vars parse_set_output(std::string_view text);

// The argument of vcvarsall.bat for the build machine's architecture and the slice's: "x64", "x64_arm64", "arm64",
// "arm64_x64".
std::string vcvars_arch(std::string_view machine, std::string_view slice);

// What an architecture is called by the tools.
struct arch_names
{
    std::string xwin;                   // the directories of xwin's CRT and SDK libraries: "x86_64", "aarch64"
    std::string triple;                 // "x86_64-pc-windows-msvc"
    unsigned coff_machine = 0;          // IMAGE_FILE_MACHINE_*: 0x8664, 0xAA64
    std::string cmake_processor;        // CMAKE_SYSTEM_PROCESSOR as Windows says it: "AMD64", "ARM64"
    std::string meson_cpu_family, meson_cpu;
    std::string rust_target, zig_target, goarch;
};

std::optional<arch_names> names_of(std::string_view arch);

// "14.44.35220" of crtversion.h (`_VC_CRT_MAJOR_VERSION`, `_MINOR_`, `_BUILD_`); empty when it has none.
std::string crt_version(std::string_view crtversion_h);

// The oldest Clang the STL of yvals_core.h accepts (`#if __clang_major__ < 19`); 0 when it names none.
int clang_minimum(std::string_view yvals_core_h);

// "21.1.8" of `clang-cl --version` ("Ubuntu clang version 21.1.8 (6ubuntu1)"); empty when it says none.
std::string clang_version(std::string_view version_output);

// The version of the Windows SDK in an `sdk/include` of xwin: the name of its entry "10.0.<build>" (a link to the
// directory itself); empty when it has none.
std::string sdk_version(std::filesystem::path const& sdk_include);

// The helpers of compiler-rt for 128-bit integers (__udivti3, __divti3, __muloti4, …) among the symbols of `llvm-nm
// --undefined-only --format=just-symbols`, sorted and without repeats: clang calls them, MSVC's runtime has none.
std::vector<std::string> compiler_rt_helpers(std::string_view undefined_symbols);

// The CMake toolchain file of a slice, without the flags part the builder adds.
struct cmake_inputs
{
    std::string arch, triple, processor, cc, linker, ar, rc, mt;
    bool debug = false;
};

std::string cmake_toolchain(cmake_inputs const& in);

// The toolset of MSVC whose CRT a slice was built with, "14.44", from the `toolchain.crt` of its key.txt (cache-key.md 3); empty
// when it has none.
std::string crt_of_key(std::string_view key_text);

// What the Windows target adds to otok-config.cmake (otok-toml.md 4.11): the system, the architecture of the generator
// platform or of the compiler, the toolset of MSVC.
consumer_cmake_parts cmake_parts(consumer_package const& package);

// otok.props and otok.targets of the output, for projects of Visual Studio (otok-toml.md 4.11).
std::string msbuild_props(consumer_package const& package);
std::string msbuild_targets(consumer_package const& package);

}

}
