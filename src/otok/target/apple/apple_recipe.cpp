// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/target/apple/apple_recipe.hpp"

#include <algorithm>
#include <set>

#include "otok/core/names.hpp"
#include "otok/core/paths.hpp"
#include "otok/recipe/recipe.hpp"
#include "otok/target/apple/apple_target.hpp"

namespace otok::target::apple {

namespace {

// A directory name in wrappers/: one path component of portable characters.
bool is_wrapper_name(std::string_view s) noexcept
{
    return is_product_name(s);
}

}

std::any read_recipe_table(toml_table& table, recipe_file_check const&)
{
    recipe_settings settings;
    if (auto xcode = table.string("xcode")) {
        auto c = version_constraint::parse(*xcode);
        if (c) settings.xcode = std::move(*c);
        else table.error(table.position("xcode"), c.error().message);
    }
    if (auto tools = table.string("swift-tools-version")) {
        auto v = version::parse(*tools);
        if (v) settings.swift_tools_version = std::move(*v);
        else table.error(table.position("swift-tools-version"), v.error().message);
    }
    if (auto dir = table.string("swift-package")) {
        if (*dir != "." && classify_file_path(*dir) != file_path_kind::inside) {
            table.error(table.position("swift-package"), table.describe("swift-package") + " must be \".\" or a directory inside the source");
        }
        if (settings.swift_tools_version) {
            table.error(table.position("swift-tools-version"), table.describe("swift-tools-version") + " is for wrappers, not for a recipe with 'swift-package'");
        }
        settings.swift_package = std::move(*dir);
    }
    table.finish();
    return settings;
}

bool delivers_sources(std::any const& recipe_table)
{
    auto const* s = std::any_cast<recipe_settings>(&recipe_table);
    return s && s->swift_package;
}

recipe_settings const* swift_package_settings(recipe const& r)
{
    auto const it = r.target_settings.find("apple");
    auto const* s = it == r.target_settings.end() ? nullptr : std::any_cast<recipe_settings>(&it->second);
    return s && s->swift_package ? s : nullptr;
}

std::any read_product_table(toml_table& table, recipe_file_check const& files)
{
    product_settings settings;
    if (auto module = table.string("module")) {
        if (!is_product_name(*module)) {
            table.error(table.position("module"), table.describe("module") + " is not a valid Clang module name ([A-Za-z_][A-Za-z0-9_]*)");
        }
        settings.module = std::move(*module);
    }
    if (auto frameworks = table.strings("frameworks")) {
        settings.frameworks = std::move(*frameworks);
    }
    if (auto wrappers = table.strings("wrappers")) {
        for (std::string const& w : *wrappers) {
            if (!is_wrapper_name(w)) {
                table.error(table.position("wrappers"), "wrapper '" + w + "' must be a directory name in wrappers/ ([A-Za-z_][A-Za-z0-9_]*)");
            } else if (files && !files("wrappers/" + w, true)) {
                table.error(table.position("wrappers"), "directory 'wrappers/" + w + "' named in " + table.describe("wrappers")
                    + " does not exist in the recipe directory");
            }
        }
        settings.wrappers = std::move(*wrappers);
    }
    if (auto manifest = table.string("privacy-manifest")) {
        if (*manifest == no_privacy_data) {
            // generated when packaging
        } else if (classify_file_path(*manifest) != file_path_kind::inside) {
            table.error(table.position("privacy-manifest"), table.describe("privacy-manifest") + " must be a path inside the recipe directory");
        } else if (files && !files(*manifest, false)) {
            table.error(table.position("privacy-manifest"), "file '" + *manifest + "' named in " + table.describe("privacy-manifest")
                + " does not exist in the recipe directory");
        }
        settings.privacy_manifest = std::move(*manifest);
    }
    table.finish();
    return settings;
}

std::vector<std::string> check_publishable(recipe const& r)
{
    std::vector<std::string> problems;
    if (std::ranges::none_of(r.platforms, [](std::string const& p) { return info().find_platform(p) != nullptr; })) return problems;
    if (r.delivers_sources) return problems;  // a SwiftPM package brings its own privacy manifest
    auto const has_manifest = [](product const& prod) {
        auto const it = prod.target_settings.find("apple");
        auto const* s = it == prod.target_settings.end() ? nullptr : std::any_cast<product_settings>(&it->second);
        return s && s->privacy_manifest;
    };
    // its own, or one of a product it depends on, whose shim it holds (otok-toml.md 4)
    auto const carries = [&](auto& self, product const& prod, std::set<std::string>& seen) -> bool {
        if (!seen.insert(prod.name).second) return false;
        if (has_manifest(prod)) return true;
        for (std::string const& d : prod.depends) {
            auto const it = std::ranges::find(r.products, d, &product::name);
            if (it != r.products.end() && self(self, *it, seen)) return true;
        }
        return false;
    };
    for (product const& prod : r.products) {
        std::set<std::string> seen;
        if (!carries(carries, prod, seen)) {
            problems.push_back("the product '" + prod.name + "' has no privacy manifest ('privacy-manifest' in apple), neither its own nor one of a product it depends on, which publishing requires (recipe-toml.md 11)");
        }
    }
    return problems;
}

}
