// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <any>
#include <optional>
#include <string>
#include <vector>

#include "otok/core/target_info.hpp"
#include "otok/core/toml_reader.hpp"
#include "otok/version/constraint.hpp"
#include "otok/version/version.hpp"

// The Apple parts of recipe.toml (docs/formats/recipe-toml.md 11, 12).
namespace otok::target::apple {

// `[apple]` of a recipe.
struct recipe_settings
{
    std::optional<version_constraint> xcode;
    std::optional<version> swift_tools_version;
    // The directory of the source that is a SwiftPM package, "." for its root (recipe-toml.md 12.1).
    std::optional<std::string> swift_package;
};

// `apple = { … }` of a product.
struct product_settings
{
    std::string module;                       // empty: the product name
    std::vector<std::string> frameworks;
    std::vector<std::string> wrappers;        // directories in wrappers/
    std::optional<std::string> privacy_manifest;   // a path in the recipe directory, or no_privacy_data
};

// `privacy-manifest = "none"`: the recipe states that the product neither tracks nor collects data, and
// otok writes the manifest that says so, declaring no required-reason API (recipe-toml.md 11).
inline constexpr std::string_view no_privacy_data = "none";

std::any read_recipe_table(toml_table& table, recipe_file_check const& files);
std::any read_product_table(toml_table& table, recipe_file_check const& files);
bool delivers_sources(std::any const& recipe_table);

// The `swift-package` of a recipe that delivers a SwiftPM package; null for one that builds.
recipe_settings const* swift_package_settings(recipe const& r);

// DESIGN 7.4: a privacy manifest for every product of a recipe for Apple platforms.
std::vector<std::string> check_publishable(recipe const& r);

}
