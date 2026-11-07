// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/lock/lock.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <tuple>

#include "otok/core/environment.hpp"
#include "otok/core/license.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/toml_reader.hpp"

namespace otok {

namespace {

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

// recipe-source: "local", { git, commit, tag?, path }, { path }.
std::optional<locked_location> read_recipe_source(toml_table& t)
{
    toml::node const* node = t.node("recipe-source", need::required);
    if (!node) return std::nullopt;
    locked_location loc;
    if (auto const* s = node->as_string()) {
        if (s->get() != "local") {
            t.error(t.position("recipe-source"), t.describe("recipe-source") + " must be \"local\" or a table");
            return std::nullopt;
        }
        loc.type = locked_location::kind::local;
        return loc;
    }
    auto table = t.table("recipe-source");
    if (!table) return std::nullopt;
    bool ok = true;
    if (table->contains("git")) {
        loc.type = locked_location::kind::git;
        loc.git = table->string("git", need::required).value_or("");
        loc.commit = table->string("commit", need::required).value_or("");
        loc.tag = table->string("tag").value_or("");
        loc.path = table->string("path").value_or("");
        if (!is_git_commit(loc.commit)) {
            table->error(table->position("commit"), table->describe("commit") + " must be a full 40-character commit SHA");
            ok = false;
        }
        if (table->contains("tag") && !is_git_tag_name(loc.tag)) {
            table->error(table->position("tag"), table->describe("tag") + " is not a valid tag name");
            ok = false;
        }
        if (!loc.path.empty() && classify_file_path(loc.path) != file_path_kind::inside) {
            table->error(table->position("path"), table->describe("path") + " must be a directory inside the repository");
            ok = false;
        }
    } else {
        loc.type = locked_location::kind::path;
        loc.path = table->string("path", need::required).value_or("");
        if (classify_file_path(loc.path) != file_path_kind::inside) {
            table->error(table->position("path"), table->describe("path") + " must be a directory inside the project");
            ok = false;
        }
    }
    table->finish();
    return ok ? std::optional{std::move(loc)} : std::nullopt;
}

// An archive or git location inside an already open table; `allow_path` adds { path, sha256 }.
std::optional<locked_location> read_location(toml_table& t, bool allow_path)
{
    locked_location loc;
    bool ok = true;
    auto const check_sha = [&](std::string_view key) {
        if (!is_sha256_hex(loc.sha256)) {
            t.error(t.position(key), t.describe(key) + " must be 64 lowercase hexadecimal digits");
            ok = false;
        }
    };
    if (t.contains("url")) {
        loc.type = locked_location::kind::archive;
        loc.url = t.string("url", need::required).value_or("");
        loc.sha256 = t.string("sha256", need::required).value_or("");
        check_sha("sha256");
        loc.mirrors = t.strings("mirrors").value_or(std::vector<std::string>{});
    } else if (t.contains("git")) {
        loc.type = locked_location::kind::git;
        loc.git = t.string("git", need::required).value_or("");
        loc.commit = t.string("commit", need::required).value_or("");
        loc.tag = t.string("tag").value_or("");
        loc.submodules = t.boolean("submodules").value_or(false);
        if (!is_git_commit(loc.commit)) {
            t.error(t.position("commit"), t.describe("commit") + " must be a full 40-character commit SHA");
            ok = false;
        }
        if (t.contains("tag") && !is_git_tag_name(loc.tag)) {
            t.error(t.position("tag"), t.describe("tag") + " is not a valid tag name");
            ok = false;
        }
    } else if (allow_path && t.contains("path")) {
        loc.type = locked_location::kind::path;
        loc.path = t.string("path", need::required).value_or("");
        loc.sha256 = t.string("sha256", need::required).value_or("");
        check_sha("sha256");
        if (classify_file_path(loc.path) != file_path_kind::inside) {
            t.error(t.position("path"), t.describe("path") + " must be a directory inside the project");
            ok = false;
        }
    } else {
        t.error(t.position(), t.path() + (allow_path ? " needs 'url', 'git' or 'path'" : " needs 'url' or 'git'"));
        ok = false;
    }
    return ok ? std::optional{std::move(loc)} : std::nullopt;
}

std::optional<locked_location> read_source(toml_table& t)
{
    toml::node const* node = t.node("source", need::required);
    if (!node) return std::nullopt;
    if (auto const* s = node->as_string()) {
        if (s->get() != "local") {
            t.error(t.position("source"), t.describe("source") + " must be \"local\" or a table");
            return std::nullopt;
        }
        locked_location loc;
        loc.type = locked_location::kind::local;
        return loc;
    }
    auto table = t.table("source");
    if (!table) return std::nullopt;
    auto loc = read_location(*table, true);
    table->finish();
    return loc;
}

std::optional<locked_package> read_package(toml_table& t, bool tool)
{
    locked_package p;
    p.where = t.position();
    bool ok = true;

    if (auto library = t.string("library", need::required)) {
        if (!is_library_name(*library)) {
            t.error(t.position("library"), in_quotes(*library) + " in " + t.describe("library") + " is not a valid library name");
            ok = false;
        }
        p.library = std::move(*library);
    } else {
        ok = false;
    }

    if (!tool) {
        if (auto target = t.string("target", need::required)) {
            if (!is_library_name(*target)) {
                t.error(t.position("target"), in_quotes(*target) + " in " + t.describe("target") + " is not a valid target name");
                ok = false;
            }
            p.target = std::move(*target);
        } else {
            ok = false;
        }
    }
    if (auto build_on = t.strings("build-on", need::required)) {
        std::set<std::string, std::less<>> seen;
        for (std::string const& e : *build_on) {
            if (!parse_environment(e)) {
                t.error(t.position("build-on"), in_quotes(e) + " in " + t.describe("build-on") + " is not an environment");
                ok = false;
            } else if (!seen.insert(e).second) {
                t.error(t.position("build-on"), in_quotes(e) + " is twice in " + t.describe("build-on"));
                ok = false;
            }
        }
        if (build_on->empty()) {
            t.error(t.position("build-on"), t.describe("build-on") + " is empty");
            ok = false;
        }
        p.build_on.assign(seen.begin(), seen.end());
    } else {
        ok = false;
    }

    if (auto recipe = t.string("recipe", need::required)) {
        if (*recipe == "git") {
            p.kind = locked_package::recipe_kind::git;
        } else if (*recipe == "path") {
            p.kind = locked_package::recipe_kind::path;
        } else if (auto id = parse_recipe_id(*recipe)) {
            p.kind = locked_package::recipe_kind::index;
            p.id = std::move(*id);
        } else {
            t.error(t.position("recipe"), t.describe("recipe") + " must be a recipe id, \"git\" or \"path\"");
            ok = false;
        }
    } else {
        ok = false;
    }
    // the variant of an index recipe; git and path recipes are their own (otok-lock.md 2)
    if (p.kind == locked_package::recipe_kind::index) {
        if (auto variant = t.string("variant", need::required)) {
            if (!is_recipe_name(*variant)) {
                t.error(t.position("variant"), in_quotes(*variant) + " in " + t.describe("variant") + " is not a valid variant name");
                ok = false;
            }
            p.variant = std::move(*variant);
        } else {
            ok = false;
        }
        // the index of the project it came from; none for `default` (otok-toml.md 6.4)
        if (auto index = t.string("index")) {
            if (!is_library_name(*index) || *index == "default") {
                t.error(t.position("index"), in_quotes(*index) + " in " + t.describe("index") + " is not the name of an index of the project");
                ok = false;
            }
            p.index = std::move(*index);
        }
    } else if (t.contains("variant") || t.contains("index")) {
        std::string const key = t.contains("variant") ? "variant" : "index";
        t.error(t.position(key), "a git or path recipe has no " + t.describe(key));
        (void)t.string("variant");
        (void)t.string("index");
        ok = false;
    }

    if (auto text = t.string("version", need::required)) {
        auto v = version::parse(*text);
        if (v) p.version = std::move(*v);
        else { t.error(t.position("version"), v.error().message); ok = false; }
    } else {
        ok = false;
    }
    if (auto revision = t.integer("revision", need::required)) {
        if (*revision < 1) { t.error(t.position("revision"), t.describe("revision") + " must be 1 or more"); ok = false; }
        p.revision = *revision;
    } else {
        ok = false;
    }

    if (auto source = read_recipe_source(t)) {
        p.recipe_source = std::move(*source);
        bool const consistent = p.kind == locked_package::recipe_kind::path
            ? p.recipe_source.type != locked_location::kind::git
            : p.recipe_source.type == locked_location::kind::git;
        if (!consistent) {
            t.error(t.position("recipe-source"), t.describe("recipe-source") + " does not match " + t.describe("recipe"));
            ok = false;
        }
    } else {
        ok = false;
    }

    auto sha = t.string("recipe-sha256");
    if (p.recipe_source.type == locked_location::kind::local) {
        if (sha) { t.error(t.position("recipe-sha256"), "a local recipe has no 'recipe-sha256'"); ok = false; }
    } else if (!sha) {
        if (!t.contains("recipe-sha256")) t.error(t.position(), "missing required key " + t.describe("recipe-sha256"));
        ok = false;
    } else if (!is_sha256_hex(*sha)) {
        t.error(t.position("recipe-sha256"), t.describe("recipe-sha256") + " must be 64 lowercase hexadecimal digits");
        ok = false;
    } else {
        p.recipe_sha256 = std::move(*sha);
    }

    if (auto source = read_source(t)) p.source = std::move(*source);
    else ok = false;

    if (auto resources = t.tables("resources")) {
        std::set<std::string, std::less<>> names;
        for (toml_table& r : *resources) {
            locked_resource res;
            res.name = r.string("name", need::required).value_or("");
            if (!res.name.empty() && !names.insert(res.name).second) {
                r.error(r.position("name"), "resource " + in_quotes(res.name) + " is listed twice");
                ok = false;
            }
            if (auto loc = read_location(r, false)) res.location = std::move(*loc);
            else ok = false;
            r.finish();
            p.resources.push_back(std::move(res));
        }
        std::ranges::sort(p.resources, {}, &locked_resource::name);  // as written, whatever order an edited file has
    }

    if (auto license = t.string("license", need::required)) {
        if (auto e = parse_license_expression(*license); !e) {
            t.error(t.position("license"), e.error().message);
            ok = false;
        }
        p.license = std::move(*license);
    } else {
        ok = false;
    }

    p.dependencies = t.strings("dependencies").value_or(std::vector<std::string>{});
    p.tool_dependencies = t.strings("tool-dependencies").value_or(std::vector<std::string>{});
    t.finish();
    return ok ? std::optional{std::move(p)} : std::nullopt;
}

void read_packages(toml_table& root, std::string_view key, std::vector<locked_package>& out, diagnostics& diags)
{
    auto list = root.tables(key);
    if (!list) return;
    bool const tool = key == "tool-package";
    std::set<std::tuple<std::string, std::string, std::string>, std::less<>> graphs;  // library, target, environment
    for (toml_table& t : *list) {
        auto p = read_package(t, tool);
        if (!p) continue;
        bool twice = false;
        for (std::string const& e : p->build_on) {
            // a tool library once per version (otok-lock.md 2)
            std::string const name = tool && p->version ? tool_reference(p->library, p->version->str()) : p->library;
            if (!graphs.emplace(name, p->target, e).second && !twice) {
                diags.error(p->where, "library " + in_quotes(name) + " is locked twice in [[" + std::string{key} + "]] for "
                    + (tool ? "" : in_quotes(p->target) + " in ") + e);
                twice = true;
            }
        }
        if (!twice) out.push_back(std::move(*p));
    }
}

// Dependencies name packages of the lock in the same graphs: target packages depend on target packages and use tool
// packages; tool packages depend only on tool packages.
void check_references(lock_file const& lock, diagnostics& diags)
{
    auto const check = [&](locked_package const& p, std::vector<std::string> const& names, bool tool_array, std::string_view what) {
        for (std::string const& name : names) {
            // the tool graph of a target package's environment is the project's to say (otok-toml.md 7): any will do here
            bool const any_graph = tool_array && !p.target.empty();
            std::string missing;
            // a tool package is named by a tool reference (otok-lock.md 2)
            if (tool_array && split_tool_reference(name).second.empty()) {
                diags.error(p.where, "package " + in_quotes(p.library) + " lists " + in_quotes(name) + " in '" + std::string{what}
                    + "', which is no tool reference '<library>@<version>'");
                continue;
            }
            for (std::string const& e : p.build_on) {
                bool const found = any_graph ? lock.find_tool(name) != nullptr
                    : tool_array ? lock.find_tool(name, e) != nullptr : lock.find(name, p.target, e) != nullptr;
                if (!found && missing.empty()) missing = e;
            }
            if (!missing.empty()) {
                diags.error(p.where, "package " + in_quotes(p.library) + " lists " + in_quotes(name) + " in '" + std::string{what}
                    + "', but the lock has no " + (tool_array ? "tool package" : "package") + " of that name"
                    + (any_graph ? std::string{} : " in " + missing));
            }
        }
    };
    for (locked_package const& p : lock.packages) {
        check(p, p.dependencies, false, "dependencies");
        check(p, p.tool_dependencies, true, "tool-dependencies");
    }
    for (locked_package const& p : lock.tool_packages) {
        check(p, p.dependencies, true, "dependencies");
        check(p, p.tool_dependencies, true, "tool-dependencies");
    }
}

// TOML basic string.
std::string toml_string(std::string_view s)
{
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string toml_strings(std::vector<std::string> const& list)
{
    std::string out = "[";
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i) out += ", ";
        out += toml_string(list[i]);
    }
    return out + "]";
}

std::string write_location(locked_location const& loc, bool recipe_source)
{
    switch (loc.type) {
    case locked_location::kind::archive:
        return "{ url = " + toml_string(loc.url) + ", sha256 = " + toml_string(loc.sha256)
            + (loc.mirrors.empty() ? "" : ", mirrors = " + toml_strings(loc.mirrors)) + " }";
    case locked_location::kind::git: {
        std::string out = "{ git = " + toml_string(loc.git) + ", commit = " + toml_string(loc.commit);
        if (!loc.tag.empty()) out += ", tag = " + toml_string(loc.tag);
        if (recipe_source && !loc.path.empty()) out += ", path = " + toml_string(loc.path);
        if (!recipe_source && loc.submodules) out += ", submodules = true";
        return out + " }";
    }
    case locked_location::kind::path:
        return recipe_source
            ? "{ path = " + toml_string(loc.path) + " }"
            : "{ path = " + toml_string(loc.path) + ", sha256 = " + toml_string(loc.sha256) + " }";
    case locked_location::kind::local:
        return "\"local\"";
    }
    return "\"local\"";
}

void write_package(std::string& out, std::string_view array, locked_package const& p)
{
    out += "\n[[" + std::string{array} + "]]\n";
    out += "library = " + toml_string(p.library) + "\n";
    if (!p.target.empty()) out += "target = " + toml_string(p.target) + "\n";
    std::vector<std::string> build_on = p.build_on;
    std::ranges::sort(build_on);
    out += "build-on = " + toml_strings(build_on) + "\n";
    switch (p.kind) {
    case locked_package::recipe_kind::index:
        out += "recipe = " + toml_string(p.id.str()) + "\n";
        out += "variant = " + toml_string(p.variant) + "\n";
        if (!p.index.empty()) out += "index = " + toml_string(p.index) + "\n";
        break;
    case locked_package::recipe_kind::git: out += "recipe = \"git\"\n"; break;
    case locked_package::recipe_kind::path: out += "recipe = \"path\"\n"; break;
    }
    out += "version = " + toml_string(p.version ? p.version->str() : std::string{}) + "\n";
    out += "revision = " + std::to_string(p.revision) + "\n";
    out += "recipe-source = " + write_location(p.recipe_source, true) + "\n";
    if (!p.recipe_sha256.empty()) out += "recipe-sha256 = " + toml_string(p.recipe_sha256) + "\n";
    out += "source = " + write_location(p.source, false) + "\n";
    if (!p.resources.empty()) {
        std::vector<locked_resource> resources = p.resources;
        std::ranges::sort(resources, {}, &locked_resource::name);
        out += "resources = [";
        for (std::size_t i = 0; i < resources.size(); ++i) {
            if (i) out += ", ";
            std::string loc = write_location(resources[i].location, false);
            out += "{ name = " + toml_string(resources[i].name) + ", " + loc.substr(2);
        }
        out += "]\n";
    }
    out += "license = " + toml_string(p.license) + "\n";
    auto const sorted = [](std::vector<std::string> list) {
        std::ranges::sort(list);
        list.erase(std::ranges::unique(list).begin(), list.end());
        return list;
    };
    if (!p.dependencies.empty()) out += "dependencies = " + toml_strings(sorted(p.dependencies)) + "\n";
    if (!p.tool_dependencies.empty()) out += "tool-dependencies = " + toml_strings(sorted(p.tool_dependencies)) + "\n";
}

}

bool locked_package::in_graph(std::string_view environment) const noexcept
{
    return std::ranges::find(build_on, environment) != build_on.end();
}

locked_package const* lock_file::find(std::string_view library, std::string_view target, std::string_view environment) const noexcept
{
    auto it = std::ranges::find_if(packages, [&](locked_package const& p) {
        return p.library == library && p.target == target && p.in_graph(environment);
    });
    return it == packages.end() ? nullptr : &*it;
}

namespace {

bool is_tool_package(locked_package const& p, std::string_view reference)
{
    auto const [library, version] = split_tool_reference(reference);
    return p.library == library && (version.empty() || (p.version && p.version->str() == version));
}

}

locked_package const* lock_file::find_tool(std::string_view reference, std::string_view environment) const noexcept
{
    auto it = std::ranges::find_if(tool_packages, [&](locked_package const& p) { return is_tool_package(p, reference) && p.in_graph(environment); });
    return it == tool_packages.end() ? nullptr : &*it;
}

locked_package const* lock_file::find_tool(std::string_view reference) const noexcept
{
    auto it = std::ranges::find_if(tool_packages, [&](locked_package const& p) { return is_tool_package(p, reference); });
    return it == tool_packages.end() ? nullptr : &*it;
}

std::string tool_reference(std::string_view library, std::string_view version)
{
    return std::string{library} + "@" + std::string{version};
}

std::pair<std::string_view, std::string_view> split_tool_reference(std::string_view reference) noexcept
{
    std::size_t const at = reference.find('@');
    if (at == std::string_view::npos) return {reference, {}};
    return {reference.substr(0, at), reference.substr(at + 1)};
}

std::optional<std::int64_t> lock_format_of(std::string_view content)
{
    diagnostics ignored;
    auto doc = toml_document::parse(content, "otok.lock", ignored);
    if (!doc) return std::nullopt;
    if (auto const* format = doc->table()["format"].as_integer()) return format->get();
    return std::nullopt;
}

std::optional<lock_file> parse_lock(std::string_view content, std::string const& file, diagnostics& diags)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();

    lock_file lock;
    if (auto format = root.integer("format", need::required)) {
        lock.format = *format;
        if (*format != lock_format) {
            root.error(root.position("format"), "lock format " + std::to_string(*format)
                + " needs a newer otok: this one reads format " + std::to_string(lock_format));
            return std::nullopt;
        }
    }
    // the systems the project is built on (otok-lock.md 2, docs/PLAN.md decision 88)
    if (auto systems = root.strings("systems")) {
        for (std::string const& s : *systems) {
            if (std::ranges::find(host_systems(), s) == host_systems().end()) {
                root.error(root.position("systems"), in_quotes(s) + " in 'systems' is not a system of build machines");
            } else if (std::ranges::count(*systems, s) > 1) {
                root.error(root.position("systems"), in_quotes(s) + " is twice in 'systems'");
            }
        }
        lock.systems = std::move(*systems);
        std::ranges::sort(lock.systems);
    }
    read_packages(root, "package", lock.packages, diags);
    read_packages(root, "tool-package", lock.tool_packages, diags);
    root.finish();
    check_references(lock, diags);

    if (diags.error_count() != errors_before) return std::nullopt;
    return lock;
}

std::string write_lock(lock_file const& lock)
{
    std::string out = "# Generated by otok. Do not edit.\nformat = " + std::to_string(lock.format) + "\n";
    if (!lock.systems.empty()) {
        std::vector<std::string> systems = lock.systems;
        std::ranges::sort(systems);
        out += "systems = " + toml_strings(systems) + "\n";
    }
    for (auto [array, list] : {std::pair{"package", &lock.packages}, std::pair{"tool-package", &lock.tool_packages}}) {
        std::vector<locked_package const*> sorted;
        for (locked_package const& p : *list) sorted.push_back(&p);
        // by library, target (tool packages: version) and the first environment (otok-lock.md 4)
        std::ranges::sort(sorted, {}, [](locked_package const* p) {
            std::string const second = p->target.empty() && p->version ? p->version->str() : p->target;
            return std::tuple{p->library, second, p->build_on.empty() ? std::string{} : std::ranges::min(p->build_on)};
        });
        for (locked_package const* p : sorted) write_package(out, array, *p);
    }
    return out;
}

}
