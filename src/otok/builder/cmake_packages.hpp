// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "otok/builder/target.hpp"
#include "otok/core/error.hpp"

// The CMake packages of an output under the names projects find them by without otok, over its products, and the
// dependency provider answering find_package with them (otok-toml.md 4.12, docs/PLAN.md decision 127). The same on
// every target: the packages hold `otok::<product>` of otok-config.cmake, which the target writes.
namespace otok {

// The provider, read through CMAKE_PROJECT_TOP_LEVEL_INCLUDES, and the directory of the packages in the output.
inline constexpr char const* cmake_provider_file = "otok-provider.cmake";
inline constexpr char const* cmake_packages_dir = "cmake.d";

// The version a package gives: the library's up to its fourth numeric component; empty when it starts with no number.
std::string cmake_package_version(std::string_view version);

// The files of the output's packages and the provider: path relative to the output, with `/` → text. Refuses one
// package in two libraries (without regard to case) and one target in two packages.
result<std::map<std::string, std::string>> cmake_package_files(std::span<output_package const> packages);

// Writes them into the output `dir` (each only when it changes) and removes the other files of cmake.d.
result<void> write_cmake_packages(std::filesystem::path const& dir, std::span<output_package const> packages);

}
