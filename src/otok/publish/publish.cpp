// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/publish/publish.hpp"

#include <algorithm>
#include <sstream>
#include <variant>

#include <toml++/toml.hpp>

namespace otok {

namespace {

// Tables merged key by key, other values replaced (recipe-toml.md 14).
void merge_into(toml::table& base, toml::table const& over)
{
    for (auto const& [key, value] : over) {
        toml::table* const inner = base[key].as_table();
        if (inner && value.is_table()) {
            merge_into(*inner, *value.as_table());
            continue;
        }
        value.visit([&base, &key](auto const& v) { base.insert_or_assign(key, v); });
    }
}

toml::array string_array(std::span<std::string const> strings)
{
    toml::array a;
    for (std::string const& s : strings) a.push_back(s);
    return a;
}

void insert_option(toml::table& to, std::string const& name, option_data const& value)
{
    std::visit([&](auto const& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::vector<std::string>>) to.insert_or_assign(name, string_array(v));
        else to.insert_or_assign(name, v);
    }, value);
}

}

result<target_info const*> publish_target(recipe const& r, std::span<target_info const* const> targets)
{
    target_info const* found = nullptr;
    for (std::string const& p : r.platforms) {
        for (target_info const* t : targets) {
            if (!t->find_platform(p)) continue;
            if (found && found != t) {
                return fail("its platforms are of the targets '" + found->name + "' and '" + t->name + "', and a published recipe has one (index.md 3)");
            }
            found = t;
        }
    }
    if (!found) return fail("this otok knows none of its platforms");
    return found;
}

result<publish_project> make_publish_project(recipe const& r, locked_location const& source, target_info const& target,
    build_environment const& environment, option_values const* options)
{
    publish_project project;
    toml::table manifest;
    manifest.insert("otok", toml::table{{"format", 1}});
    project.tool = !r.programs.empty() && r.products.empty();
    toml::array const build_on = string_array(std::vector<std::string>{environment.text()});
    // the target table: the target's defaults (Apple's deployment versions) and the recipe's [publish.<target>], with the
    // platforms, environment and output of the publication
    toml::table table;
    if (!target.publish_defaults.empty()) {
        try {
            table = toml::parse(target.publish_defaults);
        } catch (toml::parse_error const& e) {
            return fail("the publication defaults of the target '" + target.name + "' are invalid: " + std::string{e.description()});
        }
    }
    if (auto it = r.publish_targets.find(target.name); it != r.publish_targets.end()) merge_into(table, it->second);
    if (project.tool) {
        // its target's tool slice, in this environment
        if (std::ranges::find(r.platforms, target.tool_platform) == r.platforms.end() || !target.find_platform(target.tool_platform)) {
            return fail("a tool recipe is built in the tool slice of its target, and it does not support '" + target.tool_platform + "'");
        }
        manifest.insert_or_assign("otok", toml::table{{"format", 1}, {"tool-build-on", build_on}});
        project.platforms.push_back(target.tool_platform);
    } else {
        toml::table architectures;
        for (std::string const& p : r.platforms) {
            if (!target.find_platform(p)) continue;
            project.platforms.push_back(p);
            if (auto it = r.architectures.find(p); it != r.architectures.end()) architectures.insert(p, string_array(it->second));
        }
        if (project.platforms.empty()) return fail("it supports no platform of the target '" + target.name + "'");
        if (!architectures.empty()) table.insert_or_assign("architectures", std::move(architectures));
    }
    table.insert_or_assign("platforms", string_array(project.platforms));
    table.insert_or_assign("build-on", build_on);
    table.insert_or_assign("output", "out-" + target.name);
    manifest.insert(target.name, std::move(table));

    toml::table recipe_location{{"git", source.git}, {"rev", source.commit}};
    if (!source.tag.empty()) recipe_location.insert("tag", source.tag);
    if (!source.path.empty()) recipe_location.insert("path", source.path);
    recipe_location.is_inline(true);
    toml::table dep{{"recipe", std::move(recipe_location)}};
    if (options && !options->empty()) {
        toml::table values;
        for (auto const& [name, value] : *options) insert_option(values, name, value.value);
        values.is_inline(true);
        dep.insert("options", std::move(values));
    }
    manifest.insert(project.tool ? "tool-deps" : "deps", toml::table{{r.library, std::move(dep)}});

    std::ostringstream out;
    out << toml::toml_formatter{manifest, toml::format_flags::allow_unicode_strings} << "\n";
    project.manifest = out.str();
    return project;
}

std::vector<std::string> non_https_locations(locked_location const& recipe_source, recipe const& r)
{
    std::vector<std::string> found;
    auto const check = [&](std::string const& url, std::string const& what) {
        if (!url.empty() && !url.starts_with("https://")) found.push_back(what + " '" + url + "'");
    };
    check(recipe_source.git, "the recipe's repository");
    auto const location = [&](input_location const& l, std::string const& what) {
        check(l.url, what);
        check(l.git, what);
        for (std::string const& m : l.mirrors) check(m, what + " mirror");
    };
    location(r.source.location, "the source");
    for (recipe_resource const& res : r.resources) location(res.location, "the resource '" + res.name + "'");
    return found;
}

}
