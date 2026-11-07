// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/resolver/locking.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include "otok/core/fs.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/tree_hash.hpp"

namespace otok {

locked_location location_of(input_location const& in)
{
    locked_location out;
    if (in.type == input_location::kind::archive) {
        out.type = locked_location::kind::archive;
        out.url = in.url;
        out.sha256 = in.sha256;
        out.mirrors = in.mirrors;
    } else {
        out.type = locked_location::kind::git;
        out.git = in.git;
        out.commit = in.commit;
        out.tag = in.tag;
        out.submodules = in.submodules;
    }
    return out;
}

namespace {

// `previous`: the current lock's entry when its local source hash may be kept.
result<locked_package> make_package(resolved_package const& p, resolution_graph const& graph, bool tool, dependency const* project_entry,
    std::filesystem::path const& project_dir, locked_package const* previous)
{
    candidate const& c = p.chosen;
    recipe const& r = *c.recipe;
    locked_package out;
    out.library = p.library;
    if (!tool) out.target = graph.target;
    out.build_on = {graph.environment.text()};
    out.kind = c.kind;
    out.id = c.id;
    if (c.kind == locked_package::recipe_kind::index) {
        out.variant = r.variant;
        out.index = c.index;
    }
    out.version = r.version;
    out.revision = r.revision;
    out.recipe_source = c.recipe_source;
    out.recipe_sha256 = c.recipe_sha256;

    if (project_entry && project_entry->source_path) {
        std::string const& path = *project_entry->source_path;
        if (classify_file_path(path) == file_path_kind::inside) {
            std::filesystem::path const relative = path_from_utf8(path).lexically_normal();
            out.source.type = locked_location::kind::path;
            out.source.path = path_to_utf8(relative.generic_u8string());
            if (previous && previous->source.type == locked_location::kind::path && previous->source.path == out.source.path) {
                out.source.sha256 = previous->source.sha256;  // checked before the build instead (decision 13)
            } else {
                auto hash = tree_hash(project_dir / relative, tree_links::record);
                if (!hash) return fail("cannot hash the source of '" + p.library + "' in '" + path + "': " + hash.error().message);
                out.source.sha256 = std::move(*hash);
            }
        } else {
            out.source.type = locked_location::kind::local;
        }
    } else {
        out.source = location_of(r.source.location);
    }
    for (recipe_resource const& res : r.resources) out.resources.push_back({res.name, location_of(res.location)});
    std::ranges::sort(out.resources, {}, &locked_resource::name);  // as otok.lock has them, whatever the recipe's order
    out.license = r.license_text;
    out.dependencies = p.dependencies;
    out.tool_dependencies = p.tool_dependencies;
    return out;
}

// Whether two entries are the same in every key but `build-on` (otok-lock.md 2).
bool same_but_graphs(locked_package const& a, locked_package const& b)
{
    return a.library == b.library && a.target == b.target && a.kind == b.kind && a.id == b.id && a.variant == b.variant && a.index == b.index
        && a.version == b.version && a.revision == b.revision && a.recipe_source == b.recipe_source && a.recipe_sha256 == b.recipe_sha256
        && a.source == b.source && a.resources == b.resources && a.license == b.license && a.dependencies == b.dependencies
        && a.tool_dependencies == b.tool_dependencies;
}

// Entries equal but in `build-on` are written as one (otok-lock.md 2).
void merge_entries(std::vector<locked_package>& list)
{
    std::vector<locked_package> merged;
    for (locked_package& p : list) {
        auto it = std::ranges::find_if(merged, [&](locked_package const& m) { return same_but_graphs(m, p); });
        if (it == merged.end()) {
            merged.push_back(std::move(p));
            continue;
        }
        it->build_on.insert(it->build_on.end(), p.build_on.begin(), p.build_on.end());
        std::ranges::sort(it->build_on);
    }
    list = std::move(merged);
}

std::string label(locked_package const& p)
{
    return (p.version ? p.version->str() : std::string{"?"}) + " (r" + std::to_string(p.revision) + ")";
}

std::string recipe_label(locked_package const& p)
{
    switch (p.kind) {
    case locked_package::recipe_kind::index: return p.id.str() + (p.index.empty() ? std::string{} : " (index " + p.index + ")");
    case locked_package::recipe_kind::git: return "a git recipe";
    case locked_package::recipe_kind::path: return "a local recipe";
    }
    return "?";
}

// The entries of a lock by graph: (library, target, environment) → entry.
using graph_key = std::tuple<std::string, std::string, std::string>;

std::map<graph_key, locked_package const*> by_graph(std::vector<locked_package> const& list)
{
    std::map<graph_key, locked_package const*> out;
    for (locked_package const& p : list) {
        for (std::string const& e : p.build_on) out.emplace(graph_key{p.library, p.target, e}, &p);
    }
    return out;
}

// How messages name a package of a graph: "zlib", "zlib@android" with several targets, "zlib (linux/clang-cl)" when its
// target has several environments.
struct namer
{
    std::set<std::string> targets;
    std::map<std::string, std::set<std::string>, std::less<>> environments;  // target → its environments

    void add(std::vector<locked_package> const& list)
    {
        for (locked_package const& p : list) {
            targets.insert(p.target);
            environments[p.target].insert(p.build_on.begin(), p.build_on.end());
        }
    }

    std::string operator()(graph_key const& k) const
    {
        auto const& [library, target, environment] = k;
        std::string name = targets.size() > 1 ? library + "@" + target : library;
        if (environments.at(target).size() > 1) name += " (" + environment + ")";
        return name;
    }
};

// Adds `line` unless it is there: merged entries are in several graphs.
void add_once(std::vector<std::string>& lines, std::string line)
{
    if (std::ranges::find(lines, line) == lines.end()) lines.push_back(std::move(line));
}

void compare(std::vector<locked_package> const& current, std::vector<locked_package> const& fresh, std::string const& prefix,
    std::vector<std::string>& changes)
{
    auto const before = by_graph(current);
    auto const after = by_graph(fresh);
    namer name_of;
    name_of.add(current);
    name_of.add(fresh);

    for (auto const& [key, p] : after) {
        auto const it = before.find(key);
        if (it == before.end()) {
            add_once(changes, prefix + "added " + name_of(key) + " " + label(*p));
            continue;
        }
        locked_package const& old = *it->second;
        std::string const name = prefix + name_of(key) + ": ";
        if (old.kind != p->kind || old.id != p->id) {
            add_once(changes, name + "recipe " + recipe_label(old) + " -> " + recipe_label(*p));
        } else if (old.index != p->index) {
            add_once(changes, name + "index " + (old.index.empty() ? std::string{"default"} : old.index) + " -> " + (p->index.empty() ? std::string{"default"} : p->index));
        } else if (old.variant != p->variant) {
            add_once(changes, name + "variant " + old.variant + " -> " + p->variant);
        }
        if (old.version != p->version || old.revision != p->revision) {
            add_once(changes, name + label(old) + " -> " + label(*p));
        } else {
            // the same tree elsewhere (a later commit, another directory or repository) builds the same
            if (old.recipe_sha256 != p->recipe_sha256) add_once(changes, name + "recipe contents changed");
            else if (old.recipe_source != p->recipe_source) add_once(changes, name + "recipe moved, contents unchanged");
            if (old.source != p->source || old.resources != p->resources) add_once(changes, name + "inputs changed");
        }
        if (old.license != p->license) add_once(changes, name + "license '" + old.license + "' -> '" + p->license + "'");
        if (old.dependencies != p->dependencies || old.tool_dependencies != p->tool_dependencies) {
            add_once(changes, name + "dependencies changed");
        }
    }
    for (auto const& [key, p] : before) {
        if (!after.contains(key)) add_once(changes, prefix + "removed " + name_of(key) + " " + label(*p));
    }
}

}

result<lock_file> make_lock(resolution const& r, manifest const& m, std::filesystem::path const& project_dir,
    lock_file const* current, std::span<std::string const> rehash)
{
    auto const previous = [&](resolved_package const& p, resolution_graph const& graph, bool tool) -> locked_package const* {
        if (!current || std::ranges::find(rehash, p.library) != rehash.end()) return nullptr;
        std::string const environment = graph.environment.text();
        return tool ? current->find_tool(p.id, environment) : current->find(p.library, graph.target, environment);
    };
    lock_file lock;
    for (resolved_package const& p : r.packages) {
        resolution_graph const& graph = r.graphs[p.graph];
        auto package = make_package(p, graph, false, m.find_dep(p.library), project_dir, previous(p, graph, false));
        if (!package) return std::unexpected{package.error()};
        lock.packages.push_back(std::move(*package));
    }
    for (resolved_package const& p : r.tool_packages) {
        resolution_graph const& graph = r.tool_graphs[p.graph];
        auto package = make_package(p, graph, true, m.find_tool_dep(p.library), project_dir, previous(p, graph, true));
        if (!package) return std::unexpected{package.error()};
        lock.tool_packages.push_back(std::move(*package));
    }
    merge_entries(lock.packages);
    merge_entries(lock.tool_packages);
    lock.systems = r.systems;
    return lock;
}

result<void> verify_local_source(locked_package const& p, std::filesystem::path const& project_dir, std::string const& label)
{
    auto hash = tree_hash(project_dir / path_from_utf8(p.source.path), tree_links::record);
    if (!hash) return fail(label + ": cannot hash the source in '" + p.source.path + "': " + hash.error().message);
    if (*hash == p.source.sha256) return {};
    return fail(label + ": the source in '" + p.source.path + "' has changed since otok.lock was written (tree hash " + *hash
        + ", locked " + p.source.sha256 + "); run 'otok update " + p.library + "' to accept it");
}

std::vector<std::string> lock_changes(lock_file const& current, lock_file const& fresh)
{
    std::vector<std::string> changes;
    // the systems the project is built on (docs/PLAN.md decision 88), but those of a lock created now
    bool const created = current.systems.empty() && current.packages.empty() && current.tool_packages.empty();
    for (std::string const& s : created ? std::vector<std::string>{} : fresh.systems) {
        if (std::ranges::find(current.systems, s) == current.systems.end()) changes.push_back("added the system " + s);
    }
    for (std::string const& s : current.systems) {
        if (std::ranges::find(fresh.systems, s) == fresh.systems.end()) changes.push_back("removed the system " + s);
    }
    compare(current.packages, fresh.packages, "", changes);
    compare(current.tool_packages, fresh.tool_packages, "tool ", changes);
    return changes;
}

namespace {

bool is_override(locked_location const& l)
{
    return l.type == locked_location::kind::path || l.type == locked_location::kind::local;
}

void find_conflicts(std::vector<locked_package> const& current, std::vector<locked_package> const& fresh, std::string const& prefix,
    std::vector<std::string>& conflicts)
{
    std::string const remedy = "; if the new contents are trusted, remove the package from otok.lock and run 'otok install'";
    auto const before = by_graph(current);
    namer name_of;
    name_of.add(current);
    name_of.add(fresh);
    for (auto const& [key, entry] : by_graph(fresh)) {
        locked_package const& p = *entry;
        auto const it = before.find(key);
        if (it == before.end() || it->second->recipe_source != p.recipe_source) continue;
        locked_package const& old = *it->second;
        std::string const name = prefix + "'" + name_of(key) + "'";
        if (old.recipe_sha256 != p.recipe_sha256) {
            // a tree by path inside the project is edited with the project; one from git must not change
            if (p.recipe_source.type != locked_location::kind::git) continue;
            add_once(conflicts, name + ": the recipe at " + p.recipe_source.git + " commit " + p.recipe_source.commit
                + (p.recipe_source.path.empty() ? std::string{} : " ('" + p.recipe_source.path + "')") + " has recipe-sha256 "
                + p.recipe_sha256 + ", but otok.lock records " + old.recipe_sha256 + remedy);
            continue;
        }
        if (p.recipe_source.type == locked_location::kind::local) continue;
        bool const sources_differ = !is_override(old.source) && !is_override(p.source) && old.source != p.source;
        if (old.version != p.version || old.revision != p.revision || sources_differ || old.resources != p.resources) {
            add_once(conflicts, name + ": otok.lock records another version or other inputs than its recipe (" + label(p)
                + ") declares; was otok.lock edited?" + remedy);
        }
    }
}

}

std::vector<std::string> lock_conflicts(lock_file const& current, lock_file const& fresh)
{
    std::vector<std::string> conflicts;
    find_conflicts(current.packages, fresh.packages, "", conflicts);
    find_conflicts(current.tool_packages, fresh.tool_packages, "tool ", conflicts);
    return conflicts;
}

std::vector<std::string> local_packages(lock_file const& lock)
{
    std::set<std::string> targets;
    for (locked_package const& p : lock.packages) targets.insert(p.target);
    std::vector<std::string> names;
    for (locked_package const& p : lock.packages) {
        if (!p.reproducible()) add_once(names, targets.size() > 1 ? p.library + "@" + p.target : p.library);
    }
    for (locked_package const& p : lock.tool_packages) {
        if (!p.reproducible()) add_once(names, "tool " + p.library);
    }
    return names;
}

}
