// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/target/apple/apple_target.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "otok/target/apple/apple_recipe.hpp"

namespace otok::target::apple {

namespace {

// `[apple] bundle-id-prefix` (otok-toml.md 4): the start of a bundle identifier.
std::optional<std::string> check_bundle_id_prefix(std::string_view value)
{
    bool const valid = !value.empty() && value.front() != '.' && value.back() != '.' && value.find("..") == std::string_view::npos
        && std::ranges::all_of(value, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'; });
    if (valid) return std::nullopt;
    return "must be letters, digits, '-' and '.', neither starting nor ending with '.' and without '..'";
}

}

target_info const& info()
{
    static target_info const apple{
        .name = "apple",
        .platforms = {
            {"ios",        {"arm64"},             {"ios"}},
            {"iossim",     {"arm64", "x86_64"},   {"ios"}},
            {"macosx",     {"arm64", "x86_64"},   {"macos"}},
            {"catalyst",   {"arm64", "x86_64"},   {"catalyst", "ios"}},  // Mac Catalyst uses iOS versions
            {"tvos",       {"arm64"},             {"tvos"}},
            {"tvossim",    {"arm64", "x86_64"},   {"tvos"}},
            {"watchos",    {"arm64_32", "arm64"}, {"watchos"}},
            {"watchossim", {"arm64", "x86_64"},   {"watchos"}},
            {"xros",       {"arm64"},             {"visionos"}},
            {"xrossim",    {"arm64"},             {"visionos"}},
        },
        .deployment_keys = {"ios", "catalyst", "macos", "tvos", "watchos", "visionos"},
        .environments = {{"macos", "", "xcode"}},
        .any_host_compilers = {},
        .abis = {},
        .host_system = "macos",
        .tool_platform = "macosx",  // Apple builds run on a Mac
        // dynamic libraries are frameworks (otok-toml.md 4.10)
        .linkages = {"static", "dynamic"},
        .choices = {{"bundle-id-prefix", {"org.otok"}, {}, check_bundle_id_prefix, true}},
        .packages_dir = "Packages",
        .read_recipe_table = read_recipe_table,
        .read_product_table = read_product_table,
        .delivers_sources = delivers_sources,
        .publish_defaults = "deployment = { ios = \"15.0\", macos = \"12.0\", tvos = \"15.0\", watchos = \"26.0\", visionos = \"1.0\" }\n",
        .check_publishable = check_publishable,
    };
    return apple;
}

}
