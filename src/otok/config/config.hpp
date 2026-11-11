// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/fetch/locations.hpp"

// Machine settings `config.toml` (docs/formats/config.md). They never change what is built, so
// none of them enters a cache key.
namespace otok {

// The public index, the `index` of a machine whose config.toml and environment name none (config.md 2).
inline constexpr std::string_view default_index = "view+https://otok-dev.github.io/index";

enum class color_mode
{
    automatic,
    always,
    never
};

// What config.toml says; keys it does not set are empty.
struct config_file
{
    std::optional<std::int64_t> jobs;
    std::optional<std::string> cache_dir;
    std::optional<std::string> index;
    std::optional<std::string> index_mirror;
    input_mirrors mirrors;      // asset-mirror, url-rewrite, mirrors-only
    std::optional<std::string> proxy;
    std::optional<std::int64_t> cache_max_age;
    std::optional<bool> keep_build_dirs;
    std::optional<bool> sandbox;
    std::optional<bool> virtual_paths;
    std::optional<std::string> visual_studio;
    std::optional<std::string> xcode;
    std::optional<std::string> android_ndk;
    std::optional<std::string> windows_sdk;
    std::optional<std::string> llvm;
    std::optional<std::string> git;
    std::optional<color_mode> color;
};

// The settings in effect: file, environment and defaults combined (command-line flags are applied
// by the caller on top).
struct machine_config
{
    std::int64_t jobs = 1;
    std::filesystem::path cache_dir;
    std::string index;          // the index: its view (view+https://) or repository URL; empty: none
    std::string index_mirror;   // a checkout of it, or a mirror of its HTTPS view; empty: none
    input_mirrors mirrors;      // asset-mirror, url-rewrite, mirrors-only (config.md 2.1)
    std::string proxy;          // empty: none
    std::int64_t cache_max_age = 30;  // days; 0: no automatic garbage collection
    bool keep_build_dirs = false;
    bool sandbox = true;         // build processes in the host's sandbox (DESIGN 6.5)
    bool virtual_paths = true;   // Windows: build processes see the cache and work directories through short drive letters
    std::string visual_studio;  // Windows, native target: a major version or a path; empty: the newest
    std::string xcode;          // Apple target: Xcode.app or its Developer directory; empty: xcode-select -p
    std::string android_ndk;    // Android target: the NDK's root; empty: found from the environment and the SDK
    std::string windows_sdk;    // Windows target: the CRT and the Windows SDK as `xwin splat` lays them out; empty: none
    std::string llvm;           // Windows target, clang-cl: the LLVM directory; empty: clang-cl of PATH, else Visual Studio's
    std::string git;            // the git program; empty: git of PATH
    color_mode color = color_mode::automatic;
};

// Reads an environment variable (UTF-8); nothing when unset. Injected so tests control it.
using environment = std::function<std::optional<std::string>(std::string_view name)>;

// The environment of this process.
environment process_environment();

std::optional<config_file> parse_config(std::string_view content, std::string const& file, diagnostics& diags);

// OTOK_CONFIG, or the host's default location (config.md 1).
std::optional<std::filesystem::path> config_file_location(environment const& env);

// Combines the file with OTOK_* variables and the defaults (config.md 2).
std::optional<machine_config> resolve_config(config_file const& file, environment const& env, diagnostics& diags);

// Reads the config file (a missing file means all defaults) and resolves it.
std::optional<machine_config> load_config(environment const& env, diagnostics& diags);

}
