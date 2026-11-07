// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "otok/builder/target.hpp"
#include "otok/core/process.hpp"
#include "otok/core/target_info.hpp"

// The Android target: libraries for an app's shared objects, built with the NDK (docs/PLAN.md decision 56,
// otok-toml.md 4.7).
namespace otok::target::android {

// One platform, `android`, whose architectures are the ABIs (docs/formats/README.md 4).
target_info const& info();

// The target for builds. `ndk` is config.md's `android-ndk` (the NDK's root; empty: found from `environment`,
// otok's own, and the SDK).
std::unique_ptr<build_target> make_build_target(std::string ndk, env_vars environment);

namespace detail {

// What an ABI is called by the tools (build-lua-api.md 3.3, 6.2).
struct abi_names
{
    std::string clang_triple;           // "aarch64-linux-android", the API level appended for the compilers
    std::string autotools_host;         // "aarch64-linux-android", "arm-linux-androideabi"
    unsigned elf_machine = 0;           // e_machine of its objects: 183 AArch64
    bool wide = false;                  // 64-bit
    std::vector<std::string> b2_properties;
    std::string meson_cpu_family, meson_cpu;
    std::string rust_target, zig_target, goarch;
    std::vector<std::string> cflags;    // what the ABI needs beyond the triple
};

std::optional<abi_names> names_of(std::string_view abi);

// `Pkg.Revision` of an NDK's source.properties: "29.0.14206865"; empty when it has none.
std::string ndk_revision(std::string_view source_properties);

// The NDK with the highest version among the subdirectories of `dir` (an SDK's `ndk/`) that have a
// source.properties; none when there is none.
std::optional<std::filesystem::path> newest_ndk(std::filesystem::path const& dir);

// The API levels the NDK's sysroot has (meta/platforms.json `min`, `max`); none when it cannot be read.
std::optional<std::pair<int, int>> api_levels(std::string_view platforms_json);

// The NDK's directory of prebuilt tools for the build machine: "darwin-x86_64" (universal binaries on
// Apple silicon too), "linux-x86_64", "windows-x86_64".
std::string host_tag();

// The CMake toolchain file of a slice, without the flags part the builder adds.
struct cmake_inputs
{
    std::string abi, api, ndk, cc, cxx, ar, ranlib;
    bool shared_runtime = false;        // c++_shared (dynamic slices), else c++_static
};

std::string cmake_toolchain(cmake_inputs const& in);

}

}
