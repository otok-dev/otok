// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <any>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "otok/core/option_data.hpp"
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

// A system framework of a product (recipe-toml.md 11): `when` tests options and the platform, never telling the device
// and the simulator of a platform apart (docs/PLAN.md decision 135).
struct system_framework
{
    std::string name;
    std::shared_ptr<condition const> when{};
};

// `apple = { … }` of a product.
struct product_settings
{
    std::string module;                       // empty: the product name
    std::vector<system_framework> frameworks;
    std::vector<std::string> wrappers;        // directories in wrappers/
    std::optional<std::string> privacy_manifest;   // a path in the recipe directory, or no_privacy_data
};

// `privacy-manifest = "none"`: the recipe states that the product neither tracks nor collects data, and
// otok writes the manifest that says so, declaring no required-reason API (recipe-toml.md 11).
inline constexpr std::string_view no_privacy_data = "none";

std::any read_recipe_table(toml_table& table, recipe_file_check const& files);
std::any read_product_table(toml_table& table, product_table_context const& context);
bool delivers_sources(std::any const& recipe_table);

// The Apple platforms a framework is linked on: those where its `when` holds with the options of its package, in the
// order of the target's platforms.
std::vector<std::string> framework_platforms(system_framework const& framework, std::map<std::string, option_data, std::less<>> const& options);

// The `swift-package` of a recipe that delivers a SwiftPM package; null for one that builds.
recipe_settings const* swift_package_settings(recipe const& r);

// DESIGN 7.4: a privacy manifest for every product of a recipe for Apple platforms.
std::vector<std::string> check_publishable(recipe const& r);

}
