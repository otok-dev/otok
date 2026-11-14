// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/commands/commands.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "otok/builder/builder.hpp"
#include "otok/cache/inputs.hpp"
#include "otok/cache/store.hpp"
#include "otok/catalog/catalog.hpp"
#include "otok/core/build_info.hpp"
#include "otok/core/diagnostics.hpp"
#include "otok/core/environment.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/core/jobserver.hpp"
#include "otok/core/sandbox.hpp"
#include "otok/core/sha256.hpp"
#include "otok/fetch/git.hpp"
#include "otok/index/check.hpp"
#include "otok/index/index.hpp"
#include "otok/index/view.hpp"
#include "otok/lock/lock.hpp"
#include "otok/manifest/dev_manifest.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/packager/packager.hpp"
#include "otok/publish/github.hpp"
#include "otok/publish/publish.hpp"
#include "otok/resolver/locking.hpp"
#include "otok/resolver/options.hpp"
#include "otok/resolver/resolver.hpp"

namespace otok {

namespace {

constexpr std::size_t any_number = static_cast<std::size_t>(-1);

option_spec const profile_option{"profile", 0, "NAME", "Show the configuration of this profile"};
option_spec const build_profile_option{"profile", 0, "NAME", "Fetch (and build) what this profile builds"};
option_spec const locked_option{"locked", 0, "", "Fail if otok.lock is missing or would change (for CI)"};
option_spec const offline_option{"offline", 0, "", "Use only the cache: download and fetch nothing"};
option_spec const frozen_option{"frozen", 0, "", "--locked and --offline together"};
option_spec const allow_advisory_option{"allow-advisory", 0, "ID,...", "Install versions with these withdrawn or security advisories all the same"};
// What a build shows (docs/PLAN.md decision 59).
option_spec const verbose_option{"verbose", 'v', "", "Show what each slice builds with and the commands it runs; -vv: their output too, one slice at a time", true};
option_spec const keep_build_dirs_option{"keep-build-dirs", 0, "", "Keep the whole work directory of a failed build in the cache (failed/), not only its log"};
option_spec const no_sandbox_option{"no-sandbox", 0, "", "Build without the sandbox, unisolated: on a host that has none, or for a tool the sandbox stops"};
option_spec const no_virtual_paths_option{"no-virtual-paths", 0, "",
    "Windows: build processes see the real, long paths of the cache and the work directories, not short drive letters"};
option_spec const progress_option{"progress", 0, "auto|plain", "auto: the slices under way below the lines on a terminal; plain: lines only"};
// otok.dev.toml (docs/formats/otok-dev-toml.md 3).
option_spec const no_dev_option{"no-dev", 0, "", "Build what otok.lock says, ignoring the working trees of otok.dev.toml"};
option_spec const drop_option{"drop", 0, "SYSTEM,...", "Resolve the project for machines of these systems no longer (windows, linux, macos): remove their graphs from otok.lock"};

// The flags of reproducible runs (DESIGN 5.4).
bool offline(parsed_command const& cmd)
{
    return cmd.flag("offline") || cmd.flag("frozen");
}

bool locked(parsed_command const& cmd)
{
    return cmd.flag("locked") || cmd.flag("frozen");
}

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

void report(diagnostics const& diags, std::ostream& err)
{
    for (diagnostic const& d : diags.items()) err << format(d) << "\n";
}

// --progress (docs/PLAN.md decision 59): false, reported, when it is neither `auto` nor `plain`.
bool progress_valid(parsed_command const& cmd, command_context& ctx)
{
    auto const progress = cmd.value("progress");
    if (!progress || *progress == "auto" || *progress == "plain") return true;
    ctx.err << "error: --progress must be 'auto' or 'plain', not " << in_quotes(*progress) << "\n";
    return false;
}

// The manifest, the current lock and a resolution that prefers it.
struct project
{
    manifest m;
    std::optional<lock_file> lock;
    std::string lock_text;                // as read, to rewrite only on change
    resolution full;                      // every graph of the project: what the lock holds
    resolution r;                         // the graphs this machine builds (machine_graphs)
    std::size_t configuration = 0;        // --profile
    machine_config config;
    std::optional<cache_store> cache;
    std::optional<file_lock> held;        // the project's lock (cache.md 6.3), for as long as the command works in it
    std::optional<git_tool> git;
    dev_manifest dev;                    // otok.dev.toml, unless --no-dev (otok-dev-toml.md)
    std::optional<resolution> dev_full;   // the resolution with its recipes, when it names one (3)
    std::optional<resolution> dev_r;      // of the graphs this machine builds
    // where the recipes come from, kept for the recipe trees of the graphs chosen later (select_graphs)
    std::shared_ptr<recipe_provider> local_provider, index_provider, dev_provider;
    std::shared_ptr<index_set> indexes;   // otok-toml.md 6.4; null when the context gives the provider
    recipe_provider* provider = nullptr;

    // What is built: the resolution with the recipes of otok.dev.toml, else that of the lock.
    resolution const& built() const noexcept { return dev_r ? *dev_r : r; }
};

// Which environment of a target table this machine builds it in, as an index into its `build-on`; nothing: none.
using environment_choice = std::function<std::optional<std::size_t>(target_settings const&)>;

// The graphs this machine builds (otok-toml.md 4.9, otok-lock.md 2): of each target table the environment `chosen` gives,
// else the first of `build-on` that covers the machine, else its first (what tree and metadata show; a build refuses
// such a table before fetching); the tool graphs of the machine's system. None of the target tables in `skipped`, which this
// machine does not build (docs/PLAN.md decision 100).
resolution machine_graphs(resolution const& r, manifest const& m, environment_choice const& chosen, std::set<std::string> const& skipped)
{
    std::vector<std::size_t> graphs;
    for (target_settings const& ts : m.targets) {
        if (skipped.contains(ts.target)) continue;
        std::optional<std::size_t> k = chosen ? chosen(ts) : std::nullopt;
        for (std::size_t i = 0; !k && i < ts.build_on.size(); ++i) {
            if (covers(ts.build_on[i], machine_environment(ts.build_on[i].compiler))) k = i;
        }
        if (ts.build_on.empty()) continue;
        build_environment const& e = ts.build_on[k.value_or(0)];
        for (std::size_t g = 0; g < r.graphs.size(); ++g) {
            if (r.graphs[g].target == ts.target && r.graphs[g].environment == e) graphs.push_back(g);
        }
    }
    std::vector<std::size_t> tools;
    std::string const system = machine_environment("").system;
    for (std::size_t g = 0; g < r.tool_graphs.size(); ++g) {
        if (r.tool_graphs[g].environment.system == system) tools.push_back(g);
    }
    return select_graphs(r, graphs, tools);
}

// Chooses the graphs of this machine (machine_graphs) and fetches the recipe trees of their index versions (index.md 3.1).
// False after an error.
bool choose_graphs(project& p, environment_choice const& chosen, diagnostics& diags, std::set<std::string> const& skipped = {})
{
    p.r = machine_graphs(p.full, p.m, chosen, skipped);
    if (p.dev_full) p.dev_r = machine_graphs(*p.dev_full, p.m, chosen, skipped);
    for (auto [r, provider] : {std::pair{&p.r, p.provider}, std::pair{p.dev_r ? &*p.dev_r : nullptr, p.dev_provider.get()}}) {
        if (!r) continue;
        for (auto* packages : {&r->packages, &r->tool_packages}) {
            for (resolved_package& pkg : *packages) {
                if (auto ok = provider->prepare(pkg.chosen); !ok) diags.error({}, ok.error().message);
            }
        }
    }
    return !diags.has_errors();
}

// The entry of the lock for a package of `r`.
locked_package const* locked_entry(lock_file const& lock, resolution const& r, resolved_package const& pkg, bool tool)
{
    if (tool) return lock.find_tool(pkg.id, r.tool_graphs[pkg.graph].environment.text());
    return lock.find(pkg.library, pkg.target, r.graphs[pkg.graph].environment.text());
}

// Whether the resolution has the library in some graph.
bool has_library(resolution const& r, std::string_view library)
{
    return std::ranges::any_of(r.packages, [&](resolved_package const& p) { return p.library == library; })
        || std::ranges::any_of(r.tool_packages, [&](resolved_package const& p) { return p.library == library; });
}

// HTTP requests: the context's, or libcurl with the machine's proxy.
http_transport transport_of(command_context const& ctx, machine_config const& config)
{
    if (ctx.http) return ctx.http;
    http_settings settings;
    settings.proxy = config.proxy;
    return [settings](http_exchange const& e) { return http_call(e, settings); };
}

// The indexes of a project (otok-toml.md 6.4): `default` from config.toml, with the entries `otok publish` adds when it
// builds them, and those of `[indexes]` (none without a manifest).
std::shared_ptr<index_set> indexes_of(command_context const& ctx, machine_config const& config, manifest const* m, cache_store& cache,
    std::optional<git_tool> const& git, std::vector<index_addition> const& unpublished = {})
{
    std::vector<project_index> list;
    std::string none;
    http_transport const transport = transport_of(ctx, config);
    if (!config.index.empty() || !config.index_mirror.empty()) {
        list.push_back({std::string{default_index_name},
            [cache = &cache, git, config, transport, unpublished]() -> result<std::unique_ptr<index_source>> {
                auto source = open_index_source(config.index, config.index_mirror, *cache, git ? &*git : nullptr, transport);
                if (!source || unpublished.empty()) return source;
                return overlay_index_source(std::move(*source), unpublished);
            },
            {}});
    } else {
        none = "no recipe index: 'index' in config.toml (or OTOK_INDEX) is empty; set it to the view of an index, view+https://<base>, "
               "or the URL of its git repository";
    }
    for (index_declaration const& d : m ? m->indexes : std::vector<index_declaration>{}) {
        list.push_back({d.name,
            [cache = &cache, git, url = d.url, transport]() -> result<std::unique_ptr<index_source>> {
                return open_index_source(url, "", *cache, git ? &*git : nullptr, transport);
            },
            d.publishers});
    }
    return std::make_shared<index_set>(std::move(list), m ? m->index_of : std::map<std::string, std::string, std::less<>>{}, std::move(none), ctx.targets);
}

std::filesystem::path lock_path(command_context const& ctx)
{
    return ctx.project_dir / "otok.lock";
}

// The recipes of otok.dev.toml in place of those chosen for their libraries (otok-dev-toml.md 3); everything else
// from `inner`.
class dev_recipe_provider : public recipe_provider
{
public:
    dev_recipe_provider(recipe_provider& inner, std::filesystem::path const& project_dir, std::span<target_info const* const> targets,
        std::map<std::string, std::filesystem::path> recipes)
        : inner_{inner}, local_{project_dir, targets}, recipes_{std::move(recipes)}
    {
    }

    result<std::vector<candidate>> candidates(std::string_view library, recipe_ref const& ref) override
    {
        auto const it = recipes_.find(std::string{library});
        if (it == recipes_.end()) return inner_.candidates(library, ref);
        recipe_ref dev = ref;
        dev.source = recipe_ref::kind::path;
        dev.path = path_to_utf8(it->second);
        return local_.candidates(library, dev);
    }

    version_order order(std::string_view library) override { return inner_.order(library); }
    result<void> prepare(candidate& c) override { return inner_.prepare(c); }

private:
    recipe_provider& inner_;
    project_recipe_provider local_;
    std::map<std::string, std::filesystem::path> recipes_;
};

// Reads otok.dev.toml into `p` and checks it against the resolution (otok-dev-toml.md 5); with a recipe of its own,
// resolves again with it into `p.dev_r`, which check_dev_graph compares with the lock. False after an error.
bool load_dev(project& p, command_context& ctx, recipe_provider& provider, resolve_settings const& settings, diagnostics& diags)
{
    std::filesystem::path const file = ctx.project_dir / dev_manifest_name;
    auto dev = load_dev_manifest(file, diags);
    if (!dev) return false;
    std::string const where = std::string{dev_manifest_name};
    std::map<std::string, std::filesystem::path> recipes;
    for (auto const& [library, lib] : dev->deps) {
        std::vector<resolved_package const*> found;
        for (auto const* packages : {&p.full.packages, &p.full.tool_packages}) {
            for (resolved_package const& pkg : *packages) {
                if (pkg.library == library) found.push_back(&pkg);
            }
        }
        std::string const name = in_quotes(library) + " in " + where;
        if (found.empty()) {
            diags.error(lib.where, name + " is not a dependency of this project");
            continue;
        }
        auto const directory = [&](std::string const& path, std::string const& what) {
            std::filesystem::path const dir = (ctx.project_dir / path_from_utf8(path)).lexically_normal();
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) diags.error(lib.where, name + ": the " + what + " " + in_quotes(path) + " is not a directory");
            return dir;
        };
        if (lib.source) directory(*lib.source, "source");
        for (auto const& [resource, path] : lib.resources) directory(path, "resource " + in_quotes(resource));
        if (lib.recipe) recipes.emplace(library, directory(*lib.recipe, "recipe"));
        for (resolved_package const* pkg : found) {
            if (pkg->chosen.recipe->delivers_sources) diags.error(lib.where, name + ": its recipe delivers its source and builds no slices");
            // the resources of the recipe of the file are checked against its own recipe below
            if (lib.recipe) continue;
            for (auto const& [resource, path] : lib.resources) {
                if (std::ranges::find(pkg->chosen.recipe->resources, resource, &recipe_resource::name) == pkg->chosen.recipe->resources.end()) {
                    diags.error(lib.where, name + ": its recipe has no resource " + in_quotes(resource));
                }
            }
        }
    }
    if (diags.has_errors()) return false;
    p.dev = std::move(*dev);
    if (recipes.empty()) return true;

    auto dev_provider = std::make_shared<dev_recipe_provider>(provider, ctx.project_dir, ctx.targets, std::move(recipes));
    diagnostics resolving;
    auto r = resolve(p.m, ctx.targets, *dev_provider, settings, resolving);
    for (diagnostic const& d : resolving.items()) {
        if (d.level == severity::error) diags.error(d.where, "with the recipes of " + where + ": " + d.message);
    }
    if (!r) return false;
    for (auto const& [library, lib] : p.dev.deps) {
        if (!lib.recipe) continue;
        std::set<std::string> reported;
        for (auto const* packages : {&r->packages, &r->tool_packages}) {
            for (resolved_package const& pkg : *packages) {
                if (pkg.library != library) continue;
                for (auto const& [resource, path] : lib.resources) {
                    if (std::ranges::find(pkg.chosen.recipe->resources, resource, &recipe_resource::name) == pkg.chosen.recipe->resources.end()
                        && reported.insert(resource).second) {
                        diags.error(lib.where, in_quotes(library) + " in " + where + ": its recipe has no resource " + in_quotes(resource));
                    }
                }
            }
        }
    }
    p.dev_full = std::move(*r);
    p.dev_provider = std::move(dev_provider);
    return !diags.has_errors();
}

// Loads otok.toml and otok.lock and resolves. `unlocked`: libraries resolved without the lock's
// preference; `use_lock = false` ignores the lock entirely (`otok update`); `dev`: with otok.dev.toml.
std::unique_ptr<project> load_project(command_context& ctx, parsed_command const& cmd, bool use_lock,
    std::vector<std::string> const& unlocked, diagnostics& diags, bool dev = false)
{
    std::filesystem::path const manifest_path = ctx.project_dir / "otok.toml";
    std::error_code ec;
    if (!std::filesystem::exists(manifest_path, ec)) {
        diags.error({}, "no otok.toml in '" + path_to_utf8(ctx.project_dir) + "'");
        return nullptr;
    }
    auto config = load_config(ctx.env, diags);
    if (!config) return nullptr;
    cache_notices notices{
        .waiting = [&ctx](std::string const& what) { ctx.err << "note: " << what << "\n"; },
        .fetching = [&ctx](std::string const& what) { ctx.err << what << "\n"; },
    };
    auto cache = cache_store::open(config->cache_dir, std::move(notices));
    if (!cache) {
        diags.error({}, cache.error().message);
        return nullptr;
    }
    // one otok at a time writes a project, readers beside each other (cache.md 6.3): the manifest and the lock are read
    // once it is held, as the otok waited for may have changed them
    bool const writes = cmd.command == "install" || cmd.command == "update" || cmd.command == "fetch";
    auto held = cache->hold_project(ctx.project_dir, writes);
    if (!held) {
        diags.error({}, held.error().message);
        return nullptr;
    }
    auto m = load_manifest(manifest_path, ctx.targets, diags);
    if (!m) return nullptr;
    cache->set_offline(offline(cmd));
    cache->set_mirrors({m->fetch, config->mirrors});

    // on the heap: the recipe providers it keeps point into it
    auto owned = std::make_unique<project>();
    project& p = *owned;
    p.held = std::move(*held);
    p.m = std::move(*m);
    p.config = std::move(*config);
    p.cache = std::move(*cache);
    if (auto found = find_git(current_environment(), p.config.git)) {
        p.git = std::move(*found);
        for (auto const& [name, value] : ctx.git_env) p.git->env[name] = value;
    }
    if (std::filesystem::exists(lock_path(ctx), ec)) {
        auto text = read_file(lock_path(ctx));
        if (!text) {
            diags.error({}, text.error().message);
            return nullptr;
        }
        p.lock_text = std::move(*text);
        // a lock of another format is a newer otok's: never resolved anew over it (otok-lock.md 2, formats README 1.4)
        p.lock = parse_lock(p.lock_text, path_to_utf8(lock_path(ctx)), diags);
        if (!p.lock) return nullptr;
    }

    if (auto profile = cmd.value("profile")) {
        auto const it = std::ranges::find(p.m.profiles, *profile, &otok::profile::name);
        if (it == p.m.profiles.end()) {
            std::string known;
            for (otok::profile const& pr : p.m.profiles) known += (known.empty() ? "" : ", ") + in_quotes(pr.name);
            diags.error({}, "unknown profile " + in_quotes(*profile) + (known.empty() ? "; otok.toml defines none" : "; otok.toml defines " + known));
            return nullptr;
        }
        p.configuration = static_cast<std::size_t>(it - p.m.profiles.begin()) + 1;
    }

    // the graphs of the systems the project is built on: those of the lock and this machine's (docs/PLAN.md decisions 82, 88);
    // `otok update --drop` removes some
    resolve_settings settings;
    std::vector<std::string> systems = p.lock ? p.lock->systems : std::vector<std::string>{};
    std::string const machine = machine_environment("").system;
    // this machine's, when the project is built on it at all
    bool const builds_here = std::ranges::any_of(p.m.targets, [&](target_settings const& ts) {
        return std::ranges::any_of(ts.build_on, [&](build_environment const& e) { return e.any_host() || e.system == machine; });
    });
    if (builds_here && std::ranges::find(systems, machine) == systems.end()) systems.push_back(machine);
    if (auto const drop = cmd.value("drop")) {
        std::string_view rest = *drop;
        while (!rest.empty()) {
            std::size_t const comma = rest.find(',');
            std::string const system{rest.substr(0, comma)};
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            if (std::ranges::find(host_systems(), system) == host_systems().end()) {
                diags.error({}, "--drop: " + in_quotes(system) + " is not a system of build machines (windows, linux, macos)");
                return nullptr;
            }
            if (system == machine) {
                diags.error({}, "--drop: " + in_quotes(system) + " is the system of this machine, which builds the project; drop it on another");
                return nullptr;
            }
            std::erase(systems, system);
        }
    }
    // none, on a machine that does not build the project and without a lock: every system, to show or check it here
    if (!systems.empty()) settings.systems = std::move(systems);
    if (use_lock && p.lock) settings.preferred = &*p.lock;
    settings.unlocked = unlocked;
    settings.tool_roots = ctx.tool_roots;

    p.provider = ctx.provider;
    if (!p.provider) {
        p.local_provider = std::make_shared<project_recipe_provider>(ctx.project_dir, ctx.targets, &*p.cache, p.git);
        p.indexes = indexes_of(ctx, p.config, &p.m, *p.cache, p.git, ctx.unpublished);
        p.index_provider = std::make_shared<index_recipe_provider>(*p.local_provider, p.indexes, ctx.targets, &*p.cache, p.git);
        p.provider = p.index_provider.get();
    }
    auto r = resolve(p.m, ctx.targets, *p.provider, settings, diags);
    if (!r) return nullptr;
    p.full = std::move(*r);
    if (!check_license_policy(p.full, p.m.policy, diags)) return nullptr;
    if (dev && !load_dev(p, ctx, *p.provider, settings, diags)) return nullptr;
    // the graphs whose environment covers this machine; a build chooses again by the compilers it finds
    if (!choose_graphs(p, {}, diags)) return nullptr;
    return owned;
}

// The resolution with the recipes of otok.dev.toml against the lock written without them (otok-dev-toml.md 3): the
// same libraries, versions, dependencies, options and platforms, and the same inputs but those the file gives.
void check_dev_graph(project const& p, lock_file const& fresh, command_context const& ctx, diagnostics& diags)
{
    if (!p.dev_full) return;
    std::string const intro = "the recipes of " + std::string{dev_manifest_name} + " change the project; such a change is made in otok.toml (otok-dev-toml.md 3):";
    auto dev_lock = make_lock(*p.dev_full, p.m, ctx.project_dir, &fresh, {});
    if (!dev_lock) {
        diags.error({}, intro + " " + dev_lock.error().message);
        return;
    }
    std::vector<std::string> changes;
    auto const names = [](std::vector<std::string> const& list) {
        std::string text;
        for (std::string const& s : list) text += (text.empty() ? "" : ", ") + s;
        return "[" + text + "]";
    };
    auto const add = [&](std::string change) {
        if (std::ranges::find(changes, change) == changes.end()) changes.push_back(std::move(change));
    };
    auto const compare = [&](std::vector<resolved_package> const& locked, std::vector<resolved_package> const& dev, bool tool) {
        std::string const kind = tool ? "tool " : "";
        auto const same = [](resolved_package const& a) {
            return [&a](resolved_package const& b) { return a.library == b.library && a.graph == b.graph; };
        };
        for (resolved_package const& a : locked) {
            auto const b = std::ranges::find_if(dev, same(a));
            if (b == dev.end()) {
                add(kind + in_quotes(a.id) + " is no longer needed");
                continue;
            }
            std::string const name = kind + in_quotes(a.id);
            if (a.chosen.recipe->version->str() != b->chosen.recipe->version->str()) {
                add(name + ": version " + a.chosen.recipe->version->str() + " -> " + b->chosen.recipe->version->str());
            }
            if (a.dependencies != b->dependencies) add(name + ": dependencies " + names(a.dependencies) + " -> " + names(b->dependencies));
            if (a.tool_dependencies != b->tool_dependencies) {
                add(name + ": tool dependencies " + names(a.tool_dependencies) + " -> " + names(b->tool_dependencies));
            }
            for (std::size_t c = 0; c < a.configurations.size() && c < b->configurations.size(); ++c) {
                std::string const in = c == 0 ? std::string{} : " in profile " + in_quotes(p.full.configurations[c]);
                if (a.configurations[c].platforms != b->configurations[c].platforms) add(name + ": platforms" + in);
                if (a.configurations[c].options != b->configurations[c].options) add(name + ": options" + in);
            }
            locked_package const* la = locked_entry(fresh, p.full, a, tool);
            locked_package const* lb = locked_entry(*dev_lock, *p.dev_full, *b, tool);
            dev_library const* lib = p.dev.find(a.library);
            if (!la || !lb) continue;
            if ((!lib || !lib->source) && la->source != lb->source) add(name + ": the source");
            for (locked_resource const& res : lb->resources) {
                if (lib && lib->resources.contains(res.name)) continue;
                auto const old = std::ranges::find(la->resources, res.name, &locked_resource::name);
                if (old == la->resources.end()) add(name + ": the resource " + in_quotes(res.name) + ", which the lock does not have");
                else if (*old != res) add(name + ": the resource " + in_quotes(res.name));
            }
        }
        for (resolved_package const& b : dev) {
            if (std::ranges::none_of(locked, same(b))) add(kind + in_quotes(b.id) + " is needed besides");
        }
    };
    compare(p.full.packages, p.dev_full->packages, false);
    compare(p.full.tool_packages, p.dev_full->tool_packages, true);
    if (changes.empty()) return;
    std::string message = intro;
    for (std::string const& c : changes) message += "\n  " + c;
    diags.error({}, message);
}

std::string recipe_label(candidate const& c)
{
    switch (c.kind) {
    case locked_package::recipe_kind::index: return c.id.str() + (c.index.empty() ? std::string{} : " (index " + c.index + ")");
    case locked_package::recipe_kind::git: return "git " + c.recipe_source.git;
    case locked_package::recipe_kind::path:
        return c.recipe_source.type == locked_location::kind::local ? std::string{"local recipe"} : "path " + c.recipe_source.path;
    }
    return "?";
}

// What messages call a package: its id (`zlib`, `zlib@android`). A tool package's id holds its version (`cmake@4.4.5`,
// otok-lock.md 2), so it is called by its library, or by its id when `tools`, the tool packages of its resolution, has
// another of that library.
std::string package_name(resolved_package const& p, bool tool, std::span<resolved_package const> tools)
{
    if (!tool || std::ranges::count(tools, p.library, &resolved_package::library) > 1) return p.id;
    return p.library;
}

// The name with the version, unless the name holds it, and the revision: `zlib 1.3.1 (r1)`, `cmake@4.4.5 (r1)`.
std::string package_label(resolved_package const& p, bool tool, std::span<resolved_package const> tools)
{
    recipe const& r = *p.chosen.recipe;
    std::string const name = package_name(p, tool, tools);
    bool const holds_version = tool && name != p.library;
    return name + (holds_version ? "" : " " + r.version->str()) + " (r" + std::to_string(r.revision) + ")";
}

// Merged options that differ from the defaults, `{a = 1, b = true}`; empty when there are none.
std::string changed_options(resolved_package const& p, package_configuration const& c)
{
    std::string text;
    for (auto const& [name, value] : c.options) {
        option_schema const* schema = p.chosen.recipe->find_option(name);
        if (schema && merge_option(p.library, *schema, {}).value_or(option_data{}) == value) continue;
        text += (text.empty() ? "" : ", ") + name + " = " + format_option_value(value);
    }
    return text.empty() ? text : " {" + text + "}";
}

std::string platform_list(package_configuration const& c)
{
    if (c.platforms.empty()) return "not built";
    std::string text;
    for (std::string const& p : c.platforms) text += (text.empty() ? "" : ", ") + p;
    return text;
}

std::string configuration_note(project const& p)
{
    return p.configuration == 0 ? std::string{} : " (profile " + in_quotes(p.r.configurations[p.configuration]) + ")";
}

// Writes the lock when it changed and prints what changed. False after an error.
bool write_changes(project const& p, lock_file const& fresh, command_context& ctx, diagnostics& diags)
{
    std::string const text = write_lock(fresh);
    std::vector<std::string> const changes = p.lock ? lock_changes(*p.lock, fresh) : lock_changes({}, fresh);
    if (text != p.lock_text) {
        if (auto ok = write_file_atomic(lock_path(ctx), text); !ok) {
            diags.error({}, ok.error().message);
            return false;
        }
    }
    for (std::string const& local : local_packages(fresh)) {
        diags.warning({}, in_quotes(local) + " has local inputs outside the project; `otok install --locked` will refuse them");
    }
    // a misspelled name would leave that library's inputs to the recipe's locations
    for (std::string const& library : p.m.fetch.mirrors_only_libraries) {
        auto const named = [&](locked_package const& l) { return l.library == library; };
        if (std::ranges::none_of(fresh.packages, named) && std::ranges::none_of(fresh.tool_packages, named)) {
            diags.warning({}, "'mirrors-only' in fetch names " + in_quotes(library) + ", which is not a dependency of the project");
        }
    }
    if (changes.empty()) {
        ctx.out << (text == p.lock_text ? "otok.lock is up to date\n" : "otok.lock rewritten\n");
    } else {
        ctx.out << (p.lock ? "otok.lock updated:\n" : "otok.lock created:\n");
        for (std::string const& c : changes) ctx.out << "  " << c << "\n";
    }
    return true;
}

// --locked: the lock exists, would not change and has no local inputs.
void check_locked(project const& p, lock_file const& fresh, diagnostics& diags)
{
    if (!p.lock) {
        diags.error({}, "--locked needs otok.lock, and there is none; run 'otok install' without --locked to create it");
    } else if (auto changes = lock_changes(*p.lock, fresh); !changes.empty()) {
        std::string message = "otok.lock is out of date (--locked):";
        for (std::string const& c : changes) message += "\n  " + c;
        diags.error({}, message + "\nrun 'otok install' to update it");
    } else if (auto locals = local_packages(fresh); !locals.empty()) {
        std::string names;
        for (std::string const& l : locals) names += (names.empty() ? "" : ", ") + in_quotes(l);
        diags.error({}, "--locked refuses local inputs outside the project, which cannot be reproduced: " + names);
    }
}

// "; severity 'x' is unknown to this otok and taken as 'security' (a newer otok knows it)", or nothing (index.md 7.2).
std::string unknown_note(std::span<std::string const> unknown)
{
    if (unknown.empty()) return {};
    std::string text;
    for (std::string const& u : unknown) text += "; " + u;
    return text + " (a newer otok knows it)";
}

// The advisories and deprecations of the chosen index versions (index.md 5, 7.1): `withdrawn` and
// `security` stop unless --allow-advisory names them, the rest warns.
void check_advisories(project const& p, parsed_command const& cmd, diagnostics& diags)
{
    std::set<std::string, std::less<>> allowed;
    std::string const list = cmd.value("allow-advisory").value_or("");
    for (std::size_t at = 0; at < list.size();) {
        std::size_t const comma = std::min(list.find(',', at), list.size());
        if (comma > at) allowed.emplace(list.substr(at, comma - at));
        at = comma + 1;
    }
    // of every graph, which the lock holds; a merged entry is in several
    std::set<std::string> said;
    auto const check = [&](resolved_package const& pkg, bool tool) {
        std::string const label = (tool ? "tool " : "") + package_label(pkg, tool, p.full.tool_packages) + " from " + in_quotes(recipe_label(pkg.chosen));
        for (candidate_advisory const& a : pkg.chosen.advisories) {
            std::string const text = label + " has advisory " + a.id + " (" + std::string{severity_name(a.severity)} + "): " + a.summary
                + unknown_note(a.unknown);
            if (!said.insert(text).second) continue;
            bool const stops = a.severity == advisory_severity::withdrawn || a.severity == advisory_severity::security;
            if (stops && !allowed.contains(a.id)) diags.error({}, text + "; --allow-advisory=" + a.id + " installs it all the same");
            else diags.warning({}, text);
        }
        if (!pkg.chosen.deprecation.empty() && said.insert(label).second) diags.warning({}, label + " is " + pkg.chosen.deprecation);
    };
    for (resolved_package const& pkg : p.full.packages) check(pkg, false);
    for (resolved_package const& pkg : p.full.tool_packages) check(pkg, true);
    // what an advisory concerns is unknown when it cannot be read (index.md 7.2)
    if (p.indexes) {
        for (auto const& [name, index] : p.indexes->opened()) {
            for (std::string const& u : index->unreadable_advisories()) {
                diags.warning({}, index_label(name) + " has an advisory this otok cannot read, which a newer otok may: " + u);
            }
        }
    }
}

bool is_override(locked_location const& l)
{
    return l.type == locked_location::kind::path || l.type == locked_location::kind::local;
}

// Fetches the sources and resources of the packages the selected configuration builds, or of every
// package with `everything`, and checks their local sources whose hash `lock` kept from `previous`
// (make_lock). Where each package's inputs are (package_key → inputs), or nothing after an error.
std::optional<std::map<std::string, package_inputs>> fetch_project_inputs(project& p, lock_file const& lock, lock_file const* previous,
    std::span<std::string const> rehash, bool everything, command_context& ctx, diagnostics& diags)
{
    std::vector<input_request> requests;
    std::map<std::string, package_inputs> inputs;
    // where the fetched paths go: the request's index → (package, resource name; empty: the source)
    std::vector<std::pair<std::string, std::string>> targets;
    std::map<std::string, std::pair<std::string, recipe const*>> licensed;  // package key → label, recipe
    auto const add = [&](resolved_package const& pkg, locked_package const* locked, bool tool) {
        if (!locked || (!everything && pkg.configurations[p.configuration].platforms.empty())) return;
        std::string const name = (tool ? "tool " : "") + package_label(pkg, tool, p.built().tool_packages);
        std::string const key = package_key(pkg.id, tool);
        package_inputs& in = inputs[key];
        in.recipe_dir = pkg.chosen.dir;
        licensed[key] = {name, pkg.chosen.recipe.get()};
        // otok.dev.toml: its working trees instead of the locked inputs, fetched nothing (otok-dev-toml.md 2); a source
        // of otok.toml it replaces is not built, nor checked
        dev_library const* const dev = p.dev.find(pkg.library);
        if (locked->source.type == locked_location::kind::path && previous && std::ranges::find(rehash, pkg.library) == rehash.end()
            && !(dev && dev->source)) {
            locked_package const* old = locked_entry(*previous, p.built(), pkg, tool);
            if (old && old->source == locked->source) {
                if (auto ok = verify_local_source(*locked, ctx.project_dir, name); !ok) diags.error({}, ok.error().message);
            }
        }
        auto const working_tree = [&](std::string const& path) { return (ctx.project_dir / path_from_utf8(path)).lexically_normal(); };
        if (dev) {
            in.dev.emplace();
            in.dev->label = dev->source ? *dev->source : dev->recipe ? *dev->recipe : dev->resources.begin()->second;
            in.dev->source = dev->source.has_value();
            in.dev->recipe = dev->recipe.has_value();
            in.dev->quiet = dev->quiet;
            for (auto const& [resource, path] : dev->resources) {
                in.resources[resource] = working_tree(path);
                in.dev->resources.insert(resource);
            }
        }
        if (dev && dev->source) {
            in.source = working_tree(*dev->source);
        } else if (is_override(locked->source)) {
            dependency const* entry = tool ? p.m.find_tool_dep(pkg.library) : p.m.find_dep(pkg.library);
            if (entry && entry->source_path) in.source = (ctx.project_dir / path_from_utf8(*entry->source_path)).lexically_normal();
        } else {
            requests.push_back({name + " source", locked->source, true, pkg.library});
            targets.emplace_back(key, std::string{});
        }
        for (locked_resource const& res : locked->resources) {
            if (dev && dev->resources.contains(res.name)) continue;
            auto const& declared = pkg.chosen.recipe->resources;
            auto const it = std::ranges::find(declared, res.name, &recipe_resource::name);
            // recipe-toml.md 5: a resource whose `when` holds in no configuration fetched is not needed
            if (it != declared.end() && it->when) {
                bool needed = false;
                for (std::size_t c = 0; c < pkg.configurations.size(); ++c) {
                    if (!everything && c != p.configuration) continue;
                    if (!pkg.configurations[c].platforms.empty() && condition_holds(*it->when, pkg.configurations[c].options, "", "")) needed = true;
                }
                if (!needed) continue;
            }
            requests.push_back({name + " resource " + in_quotes(res.name), res.location, it == declared.end() || it->unpack, pkg.library});
            targets.emplace_back(key, res.name);
        }
    };
    // the recipes of otok.dev.toml, when it names any: their directories and resources (otok-dev-toml.md 3)
    for (resolved_package const& pkg : p.built().packages) add(pkg, locked_entry(lock, p.built(), pkg, false), false);
    for (resolved_package const& pkg : p.built().tool_packages) add(pkg, locked_entry(lock, p.built(), pkg, true), true);

    http_settings settings;
    settings.proxy = p.config.proxy;
    auto const results = fetch_inputs(*p.cache, requests, settings, p.git ? &*p.git : nullptr);
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (!results[i]) {
            diags.error({}, results[i].error().message);
            continue;
        }
        auto const& [key, resource] = targets[i];
        (resource.empty() ? inputs[key].source : inputs[key].resources[resource]) = *results[i];
    }
    // recipe-toml.md 16: the license files exist in the source
    for (auto const& [key, named] : licensed) {
        std::filesystem::path const& source = inputs[key].source;
        if (source.empty()) continue;
        for (license_entry const& file : named.second->license_files) {
            if (file.in_recipe) continue;  // checked with the recipe
            std::error_code ec;
            if (!std::filesystem::is_regular_file(long_path(source / path_from_utf8(file.path)), ec)) {
                diags.error({}, named.first + ": the license file " + in_quotes(file.path) + " named in license-files is not in the source");
            }
        }
    }
    if (diags.has_errors()) return std::nullopt;
    std::size_t const sources = std::ranges::count_if(targets, [](auto const& t) { return t.second.empty(); });
    auto const counted = [](std::size_t n, std::string const& what) { return std::to_string(n) + " " + what + (n == 1 ? "" : "s"); };
    std::string summary = sources ? counted(sources, "source") : std::string{};
    if (std::size_t const resources = targets.size() - sources) summary += (summary.empty() ? "" : ", ") + counted(resources, "resource");
    ctx.out << (requests.empty() ? std::string{"no inputs to fetch"} : "inputs in the cache: " + summary) << "\n";
    return inputs;
}

// The target tables of the project this machine does not build (docs/PLAN.md decision 100): target → why.
using skipped_targets = std::map<std::string, std::string>;

std::set<std::string> names_of(skipped_targets const& skipped)
{
    std::set<std::string> names;
    for (auto const& [target, why] : skipped) names.insert(target);
    return names;
}

// The targets of this machine that build projects (docs/PLAN.md decision 19); the target tables of the project it has no
// target for are added to `skipped`.
std::vector<std::unique_ptr<build_target>> project_targets(project& p, command_context& ctx, skipped_targets& skipped)
{
    std::vector<std::unique_ptr<build_target>> owned;
    if (ctx.make_build_targets) owned = ctx.make_build_targets(p.config);
    for (target_settings const& ts : p.m.targets) {
        if (std::ranges::none_of(owned, [&](auto const& t) { return t->info().name == ts.target; })) {
            skipped.emplace(ts.target, "this otok does not build it on this machine");
        }
    }
    return owned;
}

// The toolchains of the recipes the selected configuration builds (docs/PLAN.md decision 38), looked
// up before anything is fetched. Nothing after an error.
std::optional<toolchain_choice> project_toolchains(project& p, std::span<build_target* const> targets, command_context& ctx, diagnostics& diags)
{
    toolchain_finder finder{ctx.env("PATH").value_or(""), ctx.probe_version};
    auto checked = check_toolchains(p.built(), p.m, p.configuration, targets, finder, ctx.tool_roots);
    if (checked) return std::move(*checked);
    for (std::string const& message : checked.error()) diags.error({}, "toolchain " + message);
    return std::nullopt;
}

// The tools of [tool-deps] with `deliver` into `[otok] tools-output` (otok-toml.md 7.1), after the outputs of the targets
// (an output that otok creates must be empty); a tools output left from before is emptied and removed when none is.
bool deliver_tools(project const& p, manifest const& built_m, std::span<built_package const> built, std::span<build_target* const> targets,
    command_context& ctx, diagnostics& diags)
{
    namespace fs = std::filesystem;
    fs::path const dir = ctx.project_dir / path_from_utf8(p.m.tools_output);
    std::error_code ec;
    bool const wanted = std::ranges::any_of(p.m.tool_deps, &dependency::deliver);
    if (!wanted && !fs::exists(long_path(dir / output_state_file), ec)) return true;

    // a library of an output may not be where the tools are (`<output>/tools/` of a library named so)
    auto const lowered = [](fs::path const& path) {
        std::string s = path_to_utf8(path.lexically_normal().generic_u8string());
        while (!s.empty() && s.back() == '/') s.pop_back();
        std::ranges::transform(s, s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        return s;
    };
    std::string const tools = lowered(path_from_utf8(p.m.tools_output));
    for (target_settings const& ts : built_m.targets) {
        auto const target = std::ranges::find_if(targets, [&](build_target* t) { return t->info().name == ts.target; });
        if (target == targets.end()) continue;
        for (resolved_package const& rp : p.built().packages) {
            if (rp.target != ts.target) continue;
            std::string const library = lowered(path_from_utf8(ts.output) / (*target)->package_output(rp.library));
            if (library == tools || library.starts_with(tools + "/") || tools.starts_with(library + "/")) {
                diags.error({}, "'tools-output' of [otok] (" + in_quotes(p.m.tools_output) + ") is where the output '" + ts.output + "' has '"
                    + rp.library + "'; set another 'tools-output'");
                return false;
            }
        }
    }

    std::vector<delivered_tool> delivered;
    for (dependency const& d : p.m.tool_deps) {
        if (!d.deliver) continue;
        resolved_package const* rp = p.built().find_tool(d.library);
        auto const bp = rp ? std::ranges::find_if(built, [&](built_package const& b) { return b.tool && b.id == rp->id; }) : built.end();
        if (bp == built.end() || bp->slices.empty()) {
            diags.error({}, "[tool-deps." + d.library + "] has 'deliver', but it was not built for this machine");
            return false;
        }
        built_slice const& slice = bp->slices.front();
        delivered_tool t{d.library, slice.key, slice.entry / "prefix", {}};
        for (auto const& [name, program] : slice.output.programs) {
            if (slice.output.interpreters.contains(name)) {
                diags.error({}, "[tool-deps." + d.library + "] has 'deliver', but its program '" + name
                    + "' is a script run by another tool, which is not delivered");
                return false;
            }
            t.programs.emplace(name, path_to_utf8(program.lexically_relative(t.prefix).generic_u8string()));
        }
        if (t.programs.empty()) {
            diags.error({}, "[tool-deps." + d.library + "] has 'deliver', but its recipe builds no programs ([tool] programs)");
            return false;
        }
        delivered.push_back(std::move(t));
    }
    // one name of the consumer files for each program (docs/PLAN.md decision 118)
    std::map<std::string, std::pair<std::string, std::string>> variables;  // variable → library, program
    for (delivered_tool const& t : delivered) {
        for (auto const& [program, path] : t.programs) {
            auto const [it, added] = variables.try_emplace(tool_variable_name(program), t.library, program);
            if (!added) {
                diags.error({}, "the program '" + it->second.second + "' of [tool-deps." + it->second.first + "] and '" + program
                    + "' of [tool-deps." + t.library + "] are both OtokTool_" + it->first + " and Otok_TOOL_" + it->first
                    + " of the tools output; deliver one of them");
                return false;
            }
        }
    }
    auto changes = write_tools_output(dir, delivered);
    if (!changes) {
        diags.error({}, changes.error().message);
        return false;
    }
    if (delivered.empty()) {
        fs::remove(long_path(dir / output_state_file), ec);
        for (char const* file : {tools_file, tools_msbuild_file, tools_cmake_file}) fs::remove(long_path(dir / file), ec);
        fs::remove(long_path(dir), ec);  // when empty
    } else {
        ctx.out << "tools '" << p.m.tools_output << "': " << changes->copied << " copied, " << changes->unchanged << " unchanged, "
                << changes->removed << " removed\n";
    }
    return true;
}

// Builds the selected configuration (docs/PLAN.md decision 19) with the targets of project_targets, for the target tables
// not in `skipped` (decision 100): the outputs of those are left as they are. False after an error.
bool build_project(parsed_command const& cmd, project& p, lock_file const& lock, std::map<std::string, package_inputs> inputs,
    std::span<build_target* const> targets, toolchain_choice toolchains, skipped_targets const& skipped, command_context& ctx,
    diagnostics& diags)
{
    manifest built_m = p.m;
    std::erase_if(built_m.targets, [&](target_settings const& ts) { return skipped.contains(ts.target); });
    // standards the project does not set, chosen by the recipes (otok-toml.md 5.1)
    if (p.configuration < p.built().standards.size()) {
        for (auto const& [target, standards] : p.built().standards[p.configuration]) {
            if (skipped.contains(target)) continue;
            target_info const* info = nullptr;
            for (target_info const* t : ctx.targets) {
                if (t->name == target) info = t;
            }
            otok::profile const* const prof = p.configuration > 0 ? &p.m.profiles[p.configuration - 1] : nullptr;
            for (language const l : languages) {
                if (info && standards[l] && !standard_of(p.m, prof, "", false, *info, l)) {
                    ctx.out << language_name(l) << " standard of '" << target << "': " << *standards[l] << ", the lowest every recipe allows\n";
                }
            }
        }
    }
    build_settings settings;
    settings.toolchains = std::move(toolchains);
    settings.tool_roots = ctx.tool_roots;
    settings.configuration = p.configuration;
    settings.jobs = p.config.jobs;
    settings.keep_build_dirs = p.config.keep_build_dirs || cmd.flag("keep-build-dirs");
    settings.verbosity = cmd.count("verbose");
    settings.git = p.git;
    // docs/PLAN.md decisions 28, 73: nothing of the user's home but what the build needs
    // decision 103: unisolated only when asked; a host without a sandbox fails the builds
    settings.sandbox = find_host_sandbox({{"PATH", ctx.env("PATH").value_or("")}});
    settings.isolation = !p.config.sandbox || cmd.flag("no-sandbox") ? build_isolation::off
        : settings.sandbox.kind == host_sandbox::mechanism::none ? build_isolation::unavailable : build_isolation::sandbox;
    // the programs of an AppContainer find the jobserver only in its own namespace (docs/PLAN.md decision 103)
    if (settings.isolation == build_isolation::sandbox && settings.sandbox.kind == host_sandbox::mechanism::appcontainer)
        use_container_jobservers(settings.sandbox.container);
    // short drive letters for the cache and the work directories on Windows (docs/PLAN.md open item 23)
    settings.virtual_paths = p.config.virtual_paths && !cmd.flag("no-virtual-paths");
    if (auto home = ctx.env("HOME"); home && !home->empty()) settings.denied_reads.push_back(path_from_utf8(*home));
    settings.launcher = ctx.launcher;
    // the slices under way below the lines on a terminal (docs/PLAN.md decision 59): whatever is written to ctx.err
    // meanwhile goes above them, line by line
    std::expected<std::vector<built_package>, std::vector<error>> built;
    settings.broker = ctx.jobs;
    settings.announced = ctx.announced;
    if (ctx.display) {
        // the command running this build shows it, and ctx.err reaches its display already (docs/PLAN.md decision 60)
        if (ctx.display->live()) settings.display = ctx.display;
        builder b{p.built(), built_m, lock, ctx.project_dir, std::move(inputs), targets, *p.cache, std::move(settings), ctx.err};
        built = b.build_all();
    } else {
        std::optional<progress_display::terminal> live;
        if (ctx.terminal.live && cmd.value("progress").value_or("auto") == "auto") live = progress_display::terminal{.utf8 = ctx.terminal.utf8};
        std::ostream terminal{ctx.err.rdbuf()};
        progress_display display{terminal, std::move(live)};
        if (display.live()) settings.display = &display;
        struct restore
        {
            std::ostream& stream;
            std::streambuf* buffer;
            ~restore() { stream.rdbuf(buffer); }
        } const restored{ctx.err, display.live() ? ctx.err.rdbuf(&display.lines()) : ctx.err.rdbuf()};
        builder b{p.built(), built_m, lock, ctx.project_dir, std::move(inputs), targets, *p.cache, std::move(settings), ctx.err};
        built = b.build_all();
    }
    if (!built) {
        for (error const& e : built.error()) diags.error({}, e.message);
        return false;
    }
    std::size_t slices = 0, cached = 0;
    for (built_package const& bp : *built) {
        for (built_slice const& s : bp.slices) {
            ++slices;
            if (s.cached) ++cached;
        }
    }
    ctx.out << "slices: " << slices << " (" << cached << " from the cache)\n";

    std::ostream nothing{nullptr};  // an install for another command: the packages of its temporary project are its own business
    // ... but the display it shares shows that it is under way: packaging the libraries of a large recipe for every
    // Apple platform takes a minute, with no slice left on the display
    struct shown_task
    {
        progress_display* display = nullptr;
        std::uint64_t id = 0;
        void stage(std::string text) const
        {
            if (display) display->stage(id, std::move(text));
        }
        ~shown_task()
        {
            if (display) display->end(id);
        }
    };
    progress_display* const display = ctx.nested && !ctx.label.empty() ? ctx.display : nullptr;
    shown_task const shown{display, display ? display->begin(ctx.label) : 0};
    // the recipe the install is for: its packages by target only
    std::string const own = p.m.deps.size() == 1 ? p.m.deps.front().library : std::string{};
    auto packages = package_all(p.built(), p.configuration, *built, targets, *p.cache, ctx.nested ? nothing : ctx.err,
        [&](std::string const& library, std::string const& target) {
            shown.stage("packaging " + (library == own ? target : library + " for " + target));
        });
    if (!packages) {
        diags.error({}, packages.error().message);
        return false;
    }
    shown.stage("output");
    for (target_settings const& ts : built_m.targets) {
        auto const target = std::ranges::find_if(targets, [&](build_target* t) { return t->info().name == ts.target; });
        // the deployment versions in effect: the target table's, those of the profile over them
        std::map<std::string, std::string> deployment{ts.deployment.begin(), ts.deployment.end()};
        if (p.configuration > 0) {
            profile const& pr = p.m.profiles[p.configuration - 1];
            if (auto it = pr.targets.find(ts.target); it != pr.targets.end()) {
                for (auto const& [key, value] : it->second.deployment) deployment.insert_or_assign(key, value);
            }
        }
        auto changes = write_output(ctx.project_dir / path_from_utf8(ts.output), **target, *packages, deployment);
        if (!changes) {
            diags.error({}, changes.error().message);
            return false;
        }
        for (std::string const& w : changes->warnings) ctx.err << "warning: the output '" << ts.output << "': " << w << "\n";
        // what App Store review would reject: an error of otok publish (otok-toml.md 4.4, 4.5), whose installs are nested
        for (std::string const& w : changes->problems) {
            if (ctx.nested) diags.error({}, "the output '" + ts.output + "': " + w);
            else ctx.err << "warning: the output '" << ts.output << "': " << w << "\n";
        }
        if (ctx.nested && !changes->problems.empty()) return false;
        ctx.out << "output '" << ts.output << "': " << changes->copied << " copied, " << changes->unchanged << " unchanged, "
                << changes->removed << " removed\n";
    }
    return deliver_tools(p, built_m, *built, targets, ctx, diags);
}

// The locked versions of index recipes that this otok cannot read (index.md 3.2): an error while the lock keeps them
// (`use_lock`, the library not in `unlocked`), the resolution having chosen another; a warning when `update` moves them.
void check_unreadable_locked(project const& p, bool use_lock, std::span<std::string const> unlocked, diagnostics& diags)
{
    if (!p.lock || !p.indexes) return;
    std::set<std::string> said;
    for (auto const* packages : {&p.lock->packages, &p.lock->tool_packages}) {
        for (locked_package const& l : *packages) {
            if (l.kind != locked_package::recipe_kind::index || !l.version || !said.insert(l.id.str() + " " + l.variant).second) continue;
            auto idx = p.indexes->get(l.index.empty() ? std::string{default_index_name} : l.index);
            if (!idx) continue;
            std::string const file = l.variant + "/" + l.version->str() + "-r" + std::to_string(l.revision) + ".toml";
            for (unreadable_entry const& u : (*idx)->unreadable(l.id)) {
                if (u.file != file) continue;
                std::string const text = "otok.lock has " + l.library + " " + l.version->str() + " (r" + std::to_string(l.revision) + ", "
                    + l.variant + ") from " + in_quotes(l.id.str()) + ", which this otok cannot read: " + u.reason;
                if (use_lock && std::ranges::find(unlocked, l.library) == unlocked.end()) {
                    diags.error({}, text + "\nupdate otok, or run 'otok update " + l.library + "' to choose a version this otok reads");
                } else {
                    diags.warning({}, text + "; another version was chosen");
                }
            }
        }
    }
}

// At the end of install, update and fetch (cache.md 9).
void collect_garbage_if_due(project& p, command_context& ctx);

// install, update and fetch (docs/PLAN.md decision 16): resolve, check against the lock, write it,
// fetch the inputs. `use_lock` and `unlocked` as for load_project; `everything`: every package's inputs.
int install_pipeline(parsed_command const& cmd, command_context& ctx, bool use_lock, std::vector<std::string> const& unlocked,
    bool everything)
{
    if (!progress_valid(cmd, ctx)) return 1;
    diagnostics diags;
    auto p = load_project(ctx, cmd, use_lock, unlocked, diags, !cmd.flag("no-dev"));
    if (!p) {
        report(diags, ctx.err);
        return 1;
    }
    for (std::string const& library : unlocked) {
        if (!has_library(p->full, library)) diags.error({}, in_quotes(library) + " is not a dependency of this project");
    }
    check_advisories(*p, cmd, diags);
    lock_file const* const previous = use_lock && p->lock ? &*p->lock : nullptr;
    auto fresh = make_lock(p->full, p->m, ctx.project_dir, previous, unlocked);
    if (!fresh) diags.error({}, fresh.error().message);
    if (fresh && p->lock) {
        for (std::string const& c : lock_conflicts(*p->lock, *fresh)) diags.error({}, c);
        check_unreadable_locked(*p, use_lock, unlocked, diags);
    }
    if (fresh) check_dev_graph(*p, *fresh, ctx, diags);
    if (!diags.has_errors()) {
        if (locked(cmd)) {
            check_locked(*p, *fresh, diags);
            if (!diags.has_errors()) ctx.out << "otok.lock is up to date\n";
        } else {
            write_changes(*p, *fresh, ctx, diags);
        }
    }
    // the targets and the toolchains before anything is fetched (docs/PLAN.md decision 38); the target tables this machine
    // cannot build are skipped (decision 100)
    std::vector<std::unique_ptr<build_target>> owned;
    skipped_targets skipped;
    std::vector<build_target*> targets;
    std::optional<toolchain_choice> toolchains;
    bool const building = !diags.has_errors() && !everything;
    auto const none_built = [&] { return !p->m.targets.empty() && skipped.size() == p->m.targets.size(); };
    if (building) {
        owned = project_targets(*p, ctx, skipped);
        for (auto const& t : owned) targets.push_back(t.get());
        // an environment of each target table this machine has (otok-toml.md 4.9): the graph it builds
        std::map<std::string, std::size_t> environments;  // target → index in its build-on
        for (target_settings const& ts : p->m.targets) {
            for (build_target* t : targets) {
                if (t->info().name != ts.target) continue;
                std::size_t index = 0;
                if (auto e = machine_environment_of(*t, ts.build_on, &index); !e) skipped.emplace(ts.target, e.error().message);
                else environments[ts.target] = index;
            }
        }
        environment_choice const chosen = [&](target_settings const& ts) -> std::optional<std::size_t> {
            auto it = environments.find(ts.target);
            return it == environments.end() ? std::nullopt : std::optional{it->second};
        };
        bool chose = false;  // graphs other than load_project's
        if (!none_built()) {
            choose_graphs(*p, chosen, diags, names_of(skipped));
            chose = true;
        }
        // and the environments of packages built in another than their graph's (docs/PLAN.md decision 91)
        if (!diags.has_errors() && !none_built()) {
            std::set<std::string> checked;
            std::size_t const before = skipped.size();
            for (resolved_package const& rp : p->built().packages) {
                build_environment const& graph = p->built().graphs[rp.graph].environment;
                if (rp.environment.compiler.empty() || rp.environment.compiler == graph.compiler) continue;
                for (build_target* t : targets) {
                    if (t->info().name != rp.target) continue;
                    build_environment machine = machine_environment(rp.environment.compiler);
                    machine.arch.clear();
                    if (!checked.insert(rp.target + " " + machine.text()).second) continue;
                    if (auto found = t->find_compiler(machine); !found) {
                        skipped.emplace(rp.target, rp.id + " is built in " + machine.text() + " (its recipe does not build in " + graph.text()
                            + "), which this machine does not have: " + found.error().message);
                    }
                }
            }
            if (skipped.size() != before && !none_built()) choose_graphs(*p, chosen, diags, names_of(skipped));
        }
        // nothing to build: the graphs of load_project, whose inputs are fetched as before (otok-toml.md 4.9)
        if (none_built() && chose) choose_graphs(*p, {}, diags);
        if (!diags.has_errors() && !none_built()) toolchains = project_toolchains(*p, targets, ctx, diags);
    }
    std::optional<std::map<std::string, package_inputs>> inputs;
    if (!diags.has_errors()) inputs = fetch_project_inputs(*p, *fresh, previous, unlocked, everything, ctx, diags);
    if (inputs && building) {
        std::string none = "this machine can build none of the target tables of this project:";
        for (target_settings const& ts : p->m.targets) {
            auto const why = skipped.find(ts.target);
            if (why == skipped.end()) continue;
            if (none_built()) none += "\n  '" + ts.target + "': " + why->second;
            else ctx.out << "note: not building for '" << ts.target << "': " << why->second << "\n";
        }
        if (none_built()) diags.error({}, none);
    }
    bool const ok = inputs && (everything
        || (!none_built() && build_project(cmd, *p, *fresh, std::move(*inputs), targets, std::move(*toolchains), skipped, ctx, diags)));
    report(diags, ctx.err);
    collect_garbage_if_due(*p, ctx);
    return ok ? 0 : 1;
}

int install(parsed_command const& cmd, command_context& ctx)
{
    return install_pipeline(cmd, ctx, true, {}, false);
}

int update(parsed_command const& cmd, command_context& ctx)
{
    // `otok update <library>...` keeps every other locked version; without libraries nothing is kept.
    return install_pipeline(cmd, ctx, !cmd.arguments.empty(), cmd.arguments, false);
}

int fetch(parsed_command const& cmd, command_context& ctx)
{
    return install_pipeline(cmd, ctx, true, {}, true);
}

// otok tree: the graph from the direct dependencies down; a package already shown is marked (*).
int tree(parsed_command const& cmd, command_context& ctx)
{
    diagnostics diags;
    auto p = load_project(ctx, cmd, true, {}, diags, !cmd.flag("no-dev"));
    report(diags, ctx.err);
    if (!p) return 1;

    resolution const& r = p->r;
    std::set<std::string> shown;
    auto print = [&](auto& self, resolved_package const& pkg, bool tool, std::string const& prefix, std::string const& branch) -> void {
        std::string const key = (tool ? "tool:" : "") + pkg.id;
        package_configuration const& c = pkg.configurations[p->configuration];
        // a target package built in another environment than its graph's (docs/PLAN.md decision 91)
        build_environment const& graph = r.graphs[pkg.graph].environment;
        bool const elsewhere = !tool && !pkg.environment.compiler.empty() && pkg.environment.compiler != graph.compiler;
        ctx.out << prefix << branch << package_label(pkg, tool, r.tool_packages) << " " << recipe_label(pkg.chosen) << (tool ? " (tool)" : "")
                << (elsewhere ? " (" + pkg.environment.text() + ")" : std::string{})
                << " [" << platform_list(c) << "]" << changed_options(pkg, c)
                << (p->dev.find(pkg.library) ? " (dev)" : "");
        if (!shown.insert(key).second) {
            ctx.out << " (*)\n";
            return;
        }
        ctx.out << "\n";
        std::vector<std::pair<resolved_package const*, bool>> children;
        for (std::string const& d : pkg.dependencies) children.emplace_back(tool ? r.find_tool(d) : r.find(d, pkg.target), tool);
        for (std::string const& d : pkg.tool_dependencies) children.emplace_back(r.find_tool(d), true);
        std::string const next = prefix + (branch.empty() ? "" : branch == "└── " ? "    " : "│   ");
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (children[i].first) self(self, *children[i].first, children[i].second, next, i + 1 == children.size() ? "└── " : "├── ");
        }
    };
    std::vector<std::string> roots;
    for (dependency const& d : p->m.deps) roots.push_back(d.library);
    std::ranges::sort(roots);
    if (p->configuration != 0) ctx.out << "profile " << in_quotes(r.configurations[p->configuration]) << "\n";
    // the graph of each target this machine builds (otok-lock.md 2), headed by it when there are several
    std::set<std::size_t> graphs;
    for (resolved_package const& pkg : r.packages) graphs.insert(pkg.graph);
    for (std::size_t const g : graphs) {
        if (r.several_targets || r.graphs.size() > 1) ctx.out << "[" << r.graphs[g].target << "] " << r.graphs[g].environment.text() << "\n";
        for (std::string const& library : roots) {
            if (resolved_package const* pkg = r.find(library, r.graphs[g].target)) print(print, *pkg, false, "", "");
        }
    }
    return 0;
}

// ---- otok metadata (docs/formats/metadata.md)

// A location as otok.lock writes it (otok-lock.md 2).
nlohmann::json location_json(locked_location const& l)
{
    using kind = locked_location::kind;
    if (l.type == kind::local) return "local";
    nlohmann::json j = nlohmann::json::object();
    switch (l.type) {
    case kind::archive:
        j["url"] = l.url;
        j["sha256"] = l.sha256;
        if (!l.mirrors.empty()) j["mirrors"] = l.mirrors;
        break;
    case kind::git:
        j["git"] = l.git;
        j["commit"] = l.commit;
        if (!l.tag.empty()) j["tag"] = l.tag;
        if (l.submodules) j["submodules"] = true;
        if (!l.path.empty()) j["path"] = l.path;
        break;
    case kind::path:
        j["path"] = l.path;
        if (!l.sha256.empty()) j["sha256"] = l.sha256;
        break;
    case kind::local: break;
    }
    return j;
}

nlohmann::json option_json(option_data const& value)
{
    return std::visit([](auto const& v) { return nlohmann::json(v); }, value);
}

nlohmann::json string_or_null(std::string const& s)
{
    return s.empty() ? nlohmann::json(nullptr) : nlohmann::json(s);
}

std::string absolute_path(std::filesystem::path const& p)
{
    return path_to_utf8(std::filesystem::absolute(p).lexically_normal().generic_u8string());
}

int metadata(parsed_command const& cmd, command_context& ctx)
{
    diagnostics diags;
    auto p = load_project(ctx, cmd, true, {}, diags);
    if (!p) {
        report(diags, ctx.err);
        return 1;
    }
    auto fresh = make_lock(p->full, p->m, ctx.project_dir, p->lock ? &*p->lock : nullptr);
    if (!fresh) diags.error({}, fresh.error().message);
    report(diags, ctx.err);
    if (!fresh) return 1;

    std::size_t const c = p->configuration;
    profile const* const pr = c > 0 ? &p->m.profiles[c - 1] : nullptr;
    auto const target_of = [&](std::string const& name) -> target_info const* {
        for (target_info const* t : ctx.targets) {
            if (t->name == name) return t;
        }
        return nullptr;
    };

    nlohmann::json doc;
    doc["format"] = 1;
    doc["otok"] = std::string{build_info::version};

    nlohmann::json project;
    project["dir"] = absolute_path(ctx.project_dir);
    project["manifest"] = absolute_path(ctx.project_dir / "otok.toml");
    project["lock"] = {{"path", absolute_path(lock_path(ctx))},
        {"state", !p->lock ? "missing" : write_lock(*fresh) == p->lock_text ? "current" : "stale"}};
    project["profile"] = pr ? nlohmann::json(pr->name) : nlohmann::json(nullptr);
    project["profiles"] = nlohmann::json::array();
    for (profile const& each : p->m.profiles) project["profiles"].push_back(each.name);
    doc["project"] = std::move(project);

    // the targets as the selected profile has them (otok-toml.md 8)
    doc["targets"] = nlohmann::json::array();
    std::map<std::string, std::map<std::string, std::string>> output_states;  // target → its output's state
    for (target_settings const& ts : p->m.targets) {
        target_info const* info = target_of(ts.target);
        if (!info) continue;
        profile::target_overrides const* over = nullptr;
        if (pr) {
            if (auto it = pr->targets.find(ts.target); it != pr->targets.end()) over = &it->second;
        }
        nlohmann::json t;
        t["name"] = ts.target;
        t["output"] = absolute_path(ctx.project_dir / path_from_utf8(ts.output));
        std::vector<std::string> const platforms = over && over->platforms ? *over->platforms : ts.platforms;
        t["platforms"] = platforms;
        nlohmann::json architectures = nlohmann::json::object();
        for (std::string const& platform : platforms) {
            std::vector<std::string> archs;
            if (platform_info const* pi = info->find_platform(platform)) archs = pi->architectures;
            if (auto it = ts.architectures.find(platform); it != ts.architectures.end()) archs = it->second;
            if (over) {
                if (auto it = over->architectures.find(platform); it != over->architectures.end()) archs = it->second;
            }
            architectures[platform] = archs;
        }
        t["architectures"] = std::move(architectures);
        std::map<std::string, std::string> deployment{ts.deployment.begin(), ts.deployment.end()};
        if (over) {
            for (auto const& [key, value] : over->deployment) deployment.insert_or_assign(key, value);
        }
        t["deployment"] = deployment;
        t["configurations"] = build_configurations(ts, pr);
        t["linkage"] = linkage_of(p->m, pr, "", *info);
        std::vector<std::string> build_on;
        for (build_environment const& e : ts.build_on) build_on.push_back(e.text());
        t["build-on"] = build_on;
        doc["targets"].push_back(std::move(t));
        auto state = read_output_state(ctx.project_dir / path_from_utf8(ts.output));
        if (!state) {
            ctx.err << "warning: " << state.error().message << "\n";
            state = std::map<std::string, std::string>{};
        }
        output_states[ts.target] = std::move(*state);
    }

    doc["packages"] = nlohmann::json::array();
    auto const add = [&](resolved_package const& pkg, bool tool) {
        locked_package const* locked = locked_entry(*fresh, p->r, pkg, tool);
        if (!locked) return;
        recipe const& rec = *pkg.chosen.recipe;
        package_configuration const& pc = pkg.configurations[c];
        nlohmann::json j;
        j["library"] = pkg.library;
        j["tool"] = tool;
        // the graph this machine builds it in (otok-lock.md 2)
        j["target"] = pkg.target;
        j["environment"] = (tool ? p->r.tool_graphs : p->r.graphs)[pkg.graph].environment.text();
        j["variant"] = string_or_null(locked->variant);
        // the index of the project it came from (otok-toml.md 6.4); null for a git or path recipe
        j["index"] = locked->kind == locked_package::recipe_kind::index ? nlohmann::json(locked->index.empty() ? std::string{default_index_name} : locked->index) : nlohmann::json(nullptr);
        j["version"] = rec.version->str();
        j["revision"] = rec.revision;
        j["recipe"] = locked->kind == locked_package::recipe_kind::index ? locked->id.str()
                    : locked->kind == locked_package::recipe_kind::git ? "git" : "path";
        j["recipe-source"] = location_json(locked->recipe_source);
        j["recipe-sha256"] = string_or_null(locked->recipe_sha256);
        j["source"] = location_json(locked->source);
        j["resources"] = nlohmann::json::array();
        for (locked_resource const& res : locked->resources) {
            nlohmann::json r = location_json(res.location);
            r["name"] = res.name;
            j["resources"].push_back(std::move(r));
        }
        j["license"] = locked->license;
        j["description"] = string_or_null(rec.description);
        j["homepage"] = string_or_null(rec.homepage);
        j["dependencies"] = locked->dependencies;
        j["tool-dependencies"] = locked->tool_dependencies;
        j["required-by"] = nlohmann::json::array();
        for (package_requirement const& r : pkg.required_by) {
            j["required-by"].push_back({{"requester", r.requester}, {"constraint", string_or_null(r.constraint)}});
        }
        j["platforms"] = pc.platforms;
        nlohmann::json options = nlohmann::json::object();
        for (auto const& [name, value] : pc.options) options[name] = option_json(value);
        j["options"] = std::move(options);

        // where the package lands in the output of its target (tool packages land nowhere)
        j["outputs"] = nlohmann::json::array();
        for (target_settings const& ts : p->m.targets) {
            target_info const* info = target_of(ts.target);
            if (tool || !info || ts.target != pkg.target || pc.platforms.empty()) continue;
            nlohmann::json o;
            o["target"] = ts.target;
            o["dir"] = absolute_path(ctx.project_dir / path_from_utf8(ts.output) / package_output_of(*info, pkg.library));
            // the resolution's, which counts the dependents' requests (otok-toml.md 5), on a platform of this target
            std::string linkage = linkage_of(p->m, pr, pkg.library, *info);
            for (std::string const& pl : pc.platforms) {
                if (auto it = pc.linkage.find(pl); info->find_platform(pl) && it != pc.linkage.end()) { linkage = it->second; break; }
            }
            o["linkage"] = std::move(linkage);
            auto const& state = output_states[ts.target];
            auto const key = state.find(pkg.library);
            o["package-key"] = key == state.end() ? nlohmann::json(nullptr) : nlohmann::json(key->second);
            o["products"] = nlohmann::json::array();
            for (product const& prod : rec.products) {
                if (prod.when && !condition_holds(*prod.when, pc.options, ts.target, "")) continue;
                o["products"].push_back({{"name", prod.name}, {"libraries", prod.libraries}, {"depends", prod.depends},
                    {"link", link_on(prod, pc.options, ts.target)}, {"runtime", string_or_null(prod.runtime)}});
            }
            j["outputs"].push_back(std::move(o));
        }
        doc["packages"].push_back(std::move(j));
    };
    for (resolved_package const& pkg : p->r.packages) add(pkg, false);
    for (resolved_package const& pkg : p->r.tool_packages) add(pkg, true);

    ctx.out << doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
    return 0;
}

void explain(resolved_package const& pkg, bool tool, project const& p, std::ostream& out)
{
    package_configuration const& c = pkg.configurations[p.configuration];
    out << (tool ? "tool dependency " : "") << package_label(pkg, tool, p.r.tool_packages) << " " << recipe_label(pkg.chosen) << configuration_note(p) << "\n";
    out << "  required by:\n";
    for (package_requirement const& r : pkg.required_by) {
        out << "    " << r.requester << ": " << (r.constraint.empty() ? std::string{"any version"} : r.constraint) << "\n";
    }
    out << "  platforms: " << platform_list(c) << "\n";
    if (c.options.empty()) return;
    out << "  options:\n";
    for (auto const& [name, value] : c.options) {
        out << "    " << name << " = " << format_option_value(value);
        auto const requests = c.requests.find(name);
        if (requests == c.requests.end() || requests->second.empty()) {
            out << " (default)\n";
            continue;
        }
        out << "\n";
        for (option_request const& r : requests->second) out << "      " << r.requester << " asks for " << format_option_value(r.value) << "\n";
    }
}

int why(parsed_command const& cmd, command_context& ctx)
{
    diagnostics diags;
    auto p = load_project(ctx, cmd, true, {}, diags);
    report(diags, ctx.err);
    if (!p) return 1;
    std::string const& library = cmd.arguments.front();
    bool found = false;
    for (auto [packages, tool] : {std::pair{&p->r.packages, false}, std::pair{&p->r.tool_packages, true}}) {
        for (resolved_package const& pkg : *packages) {
            if (pkg.library != library) continue;
            explain(pkg, tool, *p, ctx.out);
            found = true;
        }
    }
    if (!found) {
        ctx.err << "error: " << in_quotes(library) << " is not a dependency of this project\n";
        return 1;
    }
    return 0;
}

// "512 B", "1.5 KiB", "12.3 MiB"
std::string format_size(std::uintmax_t bytes)
{
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double value = static_cast<double>(bytes);
    char const* unit = "B";
    for (char const* u : {"KiB", "MiB", "GiB", "TiB"}) {
        if (value < 1024) break;
        value /= 1024;
        unit = u;
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.1f %s", value, unit);
    return text;
}

// "12 cache entries unused for 30 days (1.4 GiB)"
std::string collected_text(collected_garbage const& c, std::int64_t days)
{
    std::string text = std::to_string(c.entries) + (c.entries == 1 ? " cache entry" : " cache entries");
    if (days > 0) text += " unused for " + std::to_string(days) + (days == 1 ? " day" : " days");
    return text + " (" + format_size(c.bytes) + ")";
}

void collect_garbage_if_due(project& p, command_context& ctx)
{
    std::int64_t const days = p.config.cache_max_age;
    if (days == 0 || !p.cache || !cache_store::garbage_collection_due(p.cache->root())) return;
    std::filesystem::path const root = p.cache->root();
    p.cache.reset();  // our shared use of the cache ends here
    auto store = cache_store::try_open_exclusive(root);
    if (!store || !*store) return;  // another otok process uses the cache: a later run collects
    auto collected = (*store)->collect_garbage({}, days, false);
    if (!collected) {
        ctx.err << "warning: garbage collection of the cache: " << collected.error().message << "\n";
    } else if (collected->entries > 0) {
        ctx.out << "removed " << collected_text(*collected, days) << "\n";
    }
}

// "5 min ago", "3 h ago", "12 days ago".
std::string age_text(std::filesystem::file_time_type time)
{
    auto const minutes = std::chrono::duration_cast<std::chrono::minutes>(std::filesystem::file_time_type::clock::now() - time).count();
    if (minutes < 1) return "just now";
    if (minutes < 60) return std::to_string(minutes) + " min ago";
    if (minutes < 48 * 60) return std::to_string(minutes / 60) + " h ago";
    return std::to_string(minutes / (24 * 60)) + " days ago";
}

// "1.2.3 native-x86_64 release", "79.1 source", with "build_tools" and "failed".
std::string log_label(kept_log const& l)
{
    std::string text = l.version + " " + (l.entry == "sources" ? std::string{"source"} : l.slice + " " + l.configuration);
    if (l.entry == "build_tools") text += " build_tools";
    if (l.failed) text += ", failed";
    return text;
}

// otok licenses: the notices of the third-party software built into this binary, which their licenses ask to go with it,
// and where the source of otok is (GPLv3 section 6; docs/PLAN.md decision 7).
int licenses(parsed_command const&, command_context& ctx)
{
    std::string_view const version = build_info::version;
    std::string_view const source = build_info::source_repository;
    ctx.out << "otok " << version << " is licensed under the GNU General Public License, version 3 only, with the Otok Output\n"
            << "Exception. Its source is " << source << ", tag v" << version << ":\n"
            << "  git clone --recurse-submodules --branch v" << version << " " << source << "\n\n"
            << "It includes the following third-party software, under their own licenses.\n";
    for (build_info::third_party_notice const& n : build_info::third_party_notices()) {
        ctx.out << "\n==== " << n.name << " ====\n\n" << n.text;
        if (!n.text.ends_with('\n')) ctx.out << "\n";
    }
    return 0;
}

// otok log: the logs of a library's builds kept in the cache (docs/PLAN.md decision 59); no project needed.
int show_log(parsed_command const& cmd, command_context& ctx)
{
    diagnostics diags;
    auto config = load_config(ctx.env, diags);
    report(diags, ctx.err);
    if (!config) return 1;
    cache_notices notices{.waiting = [&ctx](std::string const& what) { ctx.err << "note: " << what << "\n"; }};
    auto cache = cache_store::open(config->cache_dir, std::move(notices));
    if (!cache) {
        ctx.err << "error: " << cache.error().message << "\n";
        return 1;
    }
    std::string const& library = cmd.arguments.front();
    std::vector<kept_log> logs = find_build_logs(*cache, library);
    if (cmd.arguments.size() > 1) std::erase_if(logs, [&](kept_log const& l) { return l.slice != cmd.arguments[1]; });
    if (logs.empty()) {
        ctx.err << "error: no log of " << in_quotes(library) << (cmd.arguments.size() > 1 ? " for " + in_quotes(cmd.arguments[1]) : "")
                << " in the cache '" << path_to_utf8(cache->root()) << "'\n";
        return 1;
    }
    if (cmd.flag("list")) {
        for (kept_log const& l : logs) ctx.out << log_label(l) << ", " << age_text(l.time) << ": " << path_to_utf8(l.file) << "\n";
        return 0;
    }
    kept_log const& l = logs.front();
    auto text = read_file(l.file);
    if (!text) {
        ctx.err << "error: " << text.error().message << "\n";
        return 1;
    }
    ctx.err << "the log of " << library << " " << log_label(l) << ", " << age_text(l.time) << ": " << path_to_utf8(l.file) << "\n";
    ctx.out << *text << std::flush;
    return 0;
}

// otok clean: the machine's cache, not a project (cache.md 8).
int clean(parsed_command const& cmd, command_context& ctx)
{
    diagnostics diags;
    auto config = load_config(ctx.env, diags);
    report(diags, ctx.err);
    if (!config) return 1;
    cache_notices notices{.waiting = [&ctx](std::string const& what) { ctx.err << "note: " << what << "\n"; }};
    auto cache = cache_store::open(config->cache_dir, std::move(notices), cache_access::exclusive);
    if (!cache) {
        ctx.err << "error: " << cache.error().message << "\n";
        return 1;
    }
    bool const dry_run = cmd.flag("dry-run");
    cache_parts const parts{cmd.flag("downloads"), cmd.flag("sources"), cmd.flag("builds")};
    if (cmd.flag("unused")) {
        std::int64_t days = config->cache_max_age;
        if (auto value = cmd.value("days")) {
            std::int64_t parsed = -1;
            auto const [end, ec] = std::from_chars(value->data(), value->data() + value->size(), parsed);
            if (ec != std::errc{} || end != value->data() + value->size() || parsed < 0) {
                ctx.err << "error: --days must be a number of 0 or more, not " << in_quotes(*value) << "\n";
                return 1;
            }
            days = parsed;
        }
        auto collected = cache->collect_garbage(parts, days, dry_run);
        if (!collected) {
            ctx.err << "error: " << collected.error().message << "\n";
            return 1;
        }
        ctx.out << (dry_run ? "would remove " : "removed ") << collected_text(*collected, days) << "\n";
        return 0;
    }
    if (cmd.value("days")) {
        ctx.err << "error: --days needs --unused\n";
        return 1;
    }
    auto cleaned = cache->clean(parts, dry_run);
    if (!cleaned) {
        ctx.err << "error: " << cleaned.error().message << "\n";
        return 1;
    }
    if (cleaned->empty()) {
        ctx.out << "nothing to remove in '" << path_to_utf8(cache->root()) << "'\n";
        return 0;
    }
    std::uintmax_t total = 0;
    for (cleaned_directory const& d : *cleaned) {
        ctx.out << (dry_run ? "would remove " : "removed ") << d.name << "/ (" << format_size(d.bytes) << ")\n";
        total += d.bytes;
    }
    ctx.out << (dry_run ? "would free " : "freed ") << format_size(total) << " in '" << path_to_utf8(cache->root()) << "'\n";
    return 0;
}

// The indexes for commands that resolve nothing: search, info, audit (index.md 8.1): `default` and, in a project, those of
// its manifest (otok-toml.md 6.4).
struct index_session
{
    machine_config config;
    std::optional<cache_store> cache;
    std::optional<git_tool> git;
    std::shared_ptr<index_set> indexes;
};

// `m`: the project's manifest; null outside a project.
std::unique_ptr<index_session> open_index(parsed_command const& cmd, command_context& ctx, manifest const* m)
{
    diagnostics diags;
    auto config = load_config(ctx.env, diags);
    report(diags, ctx.err);
    if (!config) return nullptr;
    cache_notices notices{
        .waiting = [&ctx](std::string const& what) { ctx.err << "note: " << what << "\n"; },
        .fetching = [&ctx](std::string const& what) { ctx.err << what << "\n"; },
    };
    auto cache = cache_store::open(config->cache_dir, std::move(notices));
    if (!cache) {
        ctx.err << "error: " << cache.error().message << "\n";
        return nullptr;
    }
    cache->set_offline(offline(cmd));
    auto s = std::make_unique<index_session>(std::move(*config), std::move(*cache), std::nullopt, nullptr);
    if (auto found = find_git(current_environment(), s->config.git)) s->git = std::move(*found);
    s->indexes = indexes_of(ctx, s->config, m, *s->cache, s->git);
    return s;
}

// The manifest of the project in the working directory, for the indexes it adds; null without one. False when it is invalid.
bool manifest_here(command_context& ctx, std::optional<manifest>& m)
{
    std::filesystem::path const path = ctx.project_dir / "otok.toml";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    diagnostics diags;
    m = load_manifest(path, ctx.targets, diags);
    report(diags, ctx.err);
    return m.has_value();
}

// Every index of a session, with its name; the first failure ends it.
result<std::vector<std::pair<std::string, recipe_index*>>> all_indexes(index_session& s)
{
    std::vector<std::pair<std::string, recipe_index*>> out;
    std::vector<std::string> const names = s.indexes->names();
    if (names.empty()) {
        auto none = s.indexes->get("default");
        return std::unexpected{none ? error{"no index"} : none.error()};
    }
    for (std::string const& name : names) {
        auto idx = s.indexes->get(name);
        if (!idx) return std::unexpected{idx.error()};
        out.emplace_back(name, *idx);
    }
    return out;
}

std::string lowercase(std::string_view s)
{
    std::string out{s};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string version_text(recipe const& r)
{
    return r.version->str() + " (r" + std::to_string(r.revision) + ")";
}

std::string version_text(recipe_summary const& s)
{
    std::string targets;
    for (std::string const& t : s.targets) targets += (targets.empty() ? "" : ", ") + t;
    return s.version + (targets.empty() ? std::string{} : " (" + targets + ")");
}

void print_columns(std::vector<std::vector<std::string>> const& rows, std::ostream& out)
{
    std::vector<std::size_t> widths;
    for (auto const& row : rows) {
        widths.resize(std::max(widths.size(), row.size()));
        for (std::size_t i = 0; i < row.size(); ++i) widths[i] = std::max(widths[i], row[i].size());
    }
    for (auto const& row : rows) {
        std::string line;
        for (std::size_t i = 0; i < row.size(); ++i) {
            line += row[i];
            if (i + 1 < row.size()) line += std::string(widths[i] - row[i].size() + 2, ' ');
        }
        out << line.substr(0, line.find_last_not_of(' ') + 1) << "\n";
    }
}

// otok search: recipes whose id, library, description or keywords contain the text (docs/PLAN.md decision 45).
int search(parsed_command const& cmd, command_context& ctx)
{
    std::string text;
    for (std::string const& a : cmd.arguments) text += (text.empty() ? "" : " ") + a;
    std::string const needle = lowercase(text);
    std::optional<manifest> m;
    if (!manifest_here(ctx, m)) return 1;
    auto s = open_index(cmd, ctx, m ? &*m : nullptr);
    if (!s) return 1;
    auto indexes = all_indexes(*s);
    if (!indexes) {
        ctx.err << "error: " << indexes.error().message << "\n";
        return 1;
    }
    // the index of each recipe when the project has several (otok-toml.md 6.4)
    bool const named = indexes->size() > 1;
    std::vector<std::vector<std::string>> rows;
    for (auto const& [name, idx] : *indexes) {
        auto summaries = idx->summaries();
        if (!summaries) {
            ctx.err << "error: " << summaries.error().message << "\n";
            return 1;
        }
        for (recipe_summary const& r : *summaries) {
            std::string haystack = r.id.str() + "\n" + r.library + "\n" + r.description;
            for (std::string const& k : r.keywords) haystack += "\n" + k;
            if (lowercase(haystack).find(needle) == std::string::npos) continue;
            if (named) rows.push_back({r.id.str(), name, version_text(r), r.description});
            else rows.push_back({r.id.str(), version_text(r), r.description});
        }
    }
    if (rows.empty()) ctx.out << "no recipes match " << in_quotes(text) << "\n";
    print_columns(rows, ctx.out);
    return 0;
}

std::string condition_text(condition const& c)
{
    std::string text;
    auto const add = [&](std::string const& part) { text += (text.empty() ? "" : ", ") + part; };
    if (c.option) {
        if (c.has) add("option " + *c.option + " has " + *c.has);
        else if (c.is) add("option " + *c.option + " is " + format_option_value(*c.is));
        else add("option " + *c.option);
    }
    auto const names = [](std::vector<std::string> const& list) {
        std::string joined;
        for (std::string const& n : list) joined += (joined.empty() ? "" : "|") + n;
        return joined;
    };
    if (c.os) add("os " + names(*c.os));
    if (c.platform) add("platform " + names(*c.platform));
    if (c.host) add("host " + names(*c.host));
    if (c.arch) add("arch " + names(*c.arch));
    if (c.environment) {
        std::vector<std::string> environments;
        for (build_environment const& e : *c.environment) environments.push_back(e.text());
        add("environment " + names(environments));
    }
    return text;
}

// otok info: a recipe of the index, or the recipes of a library (docs/PLAN.md decision 45).
int info(parsed_command const& cmd, command_context& ctx)
{
    std::string const& name = cmd.arguments.front();
    std::optional<manifest> m;
    if (!manifest_here(ctx, m)) return 1;
    auto s = open_index(cmd, ctx, m ? &*m : nullptr);
    if (!s) return 1;
    auto const failed = [&](error const& e) {
        ctx.err << "error: " << e.message << "\n";
        return 1;
    };

    auto const id = parse_recipe_id(name);
    if (!id) {
        if (!is_library_name(name)) return failed(error{in_quotes(name) + " is neither a recipe id \"publisher/name\" nor a library name"});
        auto indexes = all_indexes(*s);
        if (!indexes) return failed(indexes.error());
        bool const named = indexes->size() > 1;
        std::vector<std::vector<std::string>> rows;
        for (auto const& [index_name, idx] : *indexes) {
            auto summaries = idx->summaries();
            if (!summaries) return failed(summaries.error());
            for (recipe_summary const& r : *summaries) {
                if (r.library != name) continue;
                if (named) rows.push_back({r.id.str(), index_name, version_text(r), r.description});
                else rows.push_back({r.id.str(), version_text(r), r.description});
            }
        }
        if (rows.empty()) return failed(error{"no recipe of " + std::string{named ? "the indexes" : "the index"} + " builds " + in_quotes(name)});
        ctx.out << "recipes of " << in_quotes(name) << ":\n";
        print_columns(rows, ctx.out);
        return 0;
    }

    auto where = s->indexes->locate(*id);
    if (!where) return failed(where.error());
    recipe_index& idx = *where->index;
    auto list = idx.newest_first(*id);
    if (!list) return failed(list.error());
    auto status = idx.status(*id);
    if (!status) return failed(status.error());
    index_entry const& newest = *newest_stable(*list);
    recipe const& r = newest.projection;
    std::ostream& out = ctx.out;
    out << id->str() << ": " << r.description << "\n";
    if (where->name != default_index_name) out << "index: " << where->name << "\n";
    out << "library: " << r.library << "\n";
    out << "license: " << r.license_text << "\n";
    if (status->deprecated) out << "status: " << status->deprecation() << "\n";
    if (status->transferred_from) out << "transferred from: " << status->transferred_from->str() << "\n";
    out << "versions:\n";
    auto availability = idx.availability();
    if (!availability) return failed(availability.error());
    std::vector<std::vector<std::string>> rows;
    for (index_entry const* e : *list) {
        auto found = idx.advisories_of(e->id, e->projection.variant, e->projection.library, *e->projection.version, e->projection.revision);
        if (!found) return failed(found.error());
        std::string notes = e->projection.channel == release_channel::prerelease ? "pre-release" : "";
        for (candidate_advisory const& a : *found) {
            notes += (notes.empty() ? "" : "; ") + ("advisory " + a.id + " (" + std::string{severity_name(a.severity)} + "): " + a.summary);
        }
        // index.md 11
        std::string const version = e->projection.version->str() + "-r" + std::to_string(e->projection.revision);
        if (unavailable_version const* u = (*availability)->find(e->id, version)) {
            std::ostringstream since;
            since << u->since.date;
            notes += (notes.empty() ? "" : "; ") + ("inputs unavailable since " + since.str());
        }
        rows.push_back({"  " + version_text(e->projection), e->projection.variant, notes});
    }
    // index.md 3.2
    for (unreadable_entry const& u : idx.unreadable(*id)) {
        std::string const file = u.file.substr(u.file.find('/') + 1);
        rows.push_back({"  " + file.substr(0, file.size() - std::string_view{".toml"}.size()), u.file.substr(0, u.file.find('/')),
            "needs a newer otok: " + u.reason});
    }
    print_columns(rows, out);

    // the variants of the newest version (recipe-toml.md 6.1), each its newest revision, in the order of their names
    std::vector<index_entry const*> variants;
    for (index_entry const* e : *list) {
        if (e->projection.version->str() != r.version->str()) continue;
        if (std::ranges::none_of(variants, [&](index_entry const* v) { return v->projection.variant == e->projection.variant; })) variants.push_back(e);
    }
    std::ranges::sort(variants, {}, [](index_entry const* e) { return e->projection.variant; });
    auto const joined = [](auto const& list, auto const& text) {
        std::string out;
        for (auto const& item : list) out += (out.empty() ? "" : ", ") + text(item);
        return out;
    };
    auto const same_source = [&](index_entry const* e) {
        // one repository however its URL is written (with '.git' or without), as the cache tells repositories apart
        return git_url_key(e->recipe_source.git) == git_url_key(newest.recipe_source.git) && e->recipe_source.commit == newest.recipe_source.commit;
    };
    bool const one_source = std::ranges::all_of(variants, same_source);
    out << "variants of " << r.version->str() << ":\n";
    rows.clear();
    for (index_entry const* e : variants) {
        recipe const& v = e->projection;
        std::string location = e->recipe_source.path;
        if (!one_source) location = e->recipe_source.git + " " + e->recipe_source.commit + (location.empty() ? "" : " (" + location + ")");
        rows.push_back({"  " + v.variant, "(r" + std::to_string(v.revision) + ")", joined(v.platforms, [](std::string const& p) { return p; }),
            joined(v.build_on, [](build_environment const& b) { return b.text(); }), location});
    }
    print_columns(rows, out);

    // a section of each group of variants whose text of it is the same, headed by the variants unless it is all of them
    auto const section = [&](std::string const& title, auto const& text_of) {
        std::vector<std::pair<std::string, std::vector<std::string>>> groups;  // text → variants
        for (index_entry const* e : variants) {
            std::string const text = text_of(e->projection);
            auto const group = std::ranges::find(groups, text, &std::pair<std::string, std::vector<std::string>>::first);
            if (group == groups.end()) groups.push_back({text, {e->projection.variant}});
            else group->second.push_back(e->projection.variant);
        }
        for (auto const& [text, names] : groups) {
            if (text.empty()) continue;
            std::string const which = variants.size() == 1 ? version_text(r)
                : names.size() == variants.size() ? r.version->str() + " (every variant)"
                : r.version->str() + " (" + joined(names, [](std::string const& n) { return n; }) + ")";
            out << title << " of " << which << ":\n" << text;
        }
    };
    section("options", [](recipe const& v) {
        std::vector<std::vector<std::string>> lines;
        for (option_schema const& o : v.options) {
            static constexpr char const* types[] = {"bool", "int", "string", "enum", "list", "file"};
            std::string type = types[static_cast<int>(o.type)];
            if (!o.values.empty()) {
                std::string values;
                for (std::string const& value : o.values) values += (values.empty() ? "" : "|") + value;
                type += " (" + values + ")";
            }
            lines.push_back({"  " + o.name, type, o.type == option_type::file ? "no file by default" : "default " + format_option_value(o.default_value)});
        }
        std::ostringstream text;
        if (!lines.empty()) print_columns(lines, text);
        return text.str();
    });
    for (auto [deps, title] : {std::pair{&recipe::dependencies, "dependencies"}, std::pair{&recipe::tool_dependencies, "tool dependencies"}}) {
        section(title, [deps](recipe const& v) {
            std::vector<std::vector<std::string>> lines;
            for (recipe_dependency const& d : v.*deps) {
                // a toolchain names its programs; its library is that of its recipe (recipe-toml.md 13.1)
                std::string label = d.library;
                for (std::string const& program : d.library.empty() ? d.programs : std::vector<std::string>{}) label += (label.empty() ? "" : ", ") + program;
                lines.push_back({"  " + label, d.from_dependency ? std::string{"its dependency's"} : d.version.str(), d.default_recipe.str(),
                    d.when ? "when " + condition_text(*d.when) : ""});
            }
            std::ostringstream text;
            if (!lines.empty()) print_columns(lines, text);
            return text.str();
        });
    }
    if (one_source) out << "recipe source: " << newest.recipe_source.git << " " << newest.recipe_source.commit << "\n";
    return 0;
}

// otok audit: otok.lock against the advisories, [policy] and the deprecations, without resolving
// (docs/PLAN.md decision 45). Exit 1 on a withdrawn or security advisory or a refused license.
int audit(parsed_command const& cmd, command_context& ctx)
{
    diagnostics diags;
    std::filesystem::path const manifest_path = ctx.project_dir / "otok.toml";
    auto m = load_manifest(manifest_path, ctx.targets, diags);
    std::optional<lock_file> lock;
    if (m) {
        auto text = read_file(lock_path(ctx));
        if (!text) diags.error({}, "audit needs otok.lock, and there is none; run 'otok install' to create it");
        else lock = parse_lock(*text, path_to_utf8(lock_path(ctx)), diags);
    }
    if (!m || !lock) {
        report(diags, ctx.err);
        return 1;
    }

    std::size_t findings = 0;
    auto const label = [](locked_package const& p, bool tool) {
        return (tool ? "tool " : "") + p.library + " " + (p.version ? p.version->str() : "?") + " (r" + std::to_string(p.revision) + ")";
    };
    for (auto [packages, tool] : {std::pair{&lock->packages, false}, std::pair{&lock->tool_packages, true}}) {
        for (locked_package const& p : *packages) {
            auto license = parse_license_expression(p.license);
            if (!license) continue;  // the lock parser has checked it
            if (auto rule = policy_refusal(*license, m->policy)) {
                diags.error({}, label(p, tool) + " is licensed under '" + p.license + "', and " + *rule);
                ++findings;
            }
        }
    }

    bool const any_index = std::ranges::any_of(lock->packages, [](locked_package const& p) { return p.kind == locked_package::recipe_kind::index; })
        || std::ranges::any_of(lock->tool_packages, [](locked_package const& p) { return p.kind == locked_package::recipe_kind::index; });
    std::unique_ptr<index_session> s;
    if (any_index) {
        s = open_index(cmd, ctx, &*m);
        if (!s) {
            report(diags, ctx.err);
            return 1;
        }
    }
    for (auto [packages, tool] : {std::pair{&lock->packages, false}, std::pair{&lock->tool_packages, true}}) {
        for (locked_package const& p : *packages) {
            if (p.kind != locked_package::recipe_kind::index || !p.version) continue;
            std::string const name = label(p, tool) + " from " + in_quotes(p.id.str());
            // the index the lock took it from (otok-lock.md 2)
            auto idx = s->indexes->get(p.index.empty() ? std::string{default_index_name} : p.index);
            if (!idx) {
                diags.error({}, name + ": " + idx.error().message);
                continue;
            }
            recipe_index* const index = *idx;
            auto found = index->advisories_of(p.id, p.variant, p.library, *p.version, p.revision);
            if (!found) {
                diags.error({}, found.error().message);
                continue;
            }
            for (candidate_advisory const& a : *found) {
                std::string const text = name + " has advisory " + a.id + " (" + std::string{severity_name(a.severity)} + "): " + a.summary
                    + unknown_note(a.unknown);
                if (a.severity == advisory_severity::broken) diags.warning({}, text);
                else diags.error({}, text);
                ++findings;
            }
            auto status = index->status(p.id);
            if (!status) diags.error({}, status.error().message);
            else if (status->deprecated) {
                diags.warning({}, name + " is " + status->deprecation());
                ++findings;
            }
        }
    }
    if (s) {
        for (auto const& [index_name, index] : s->indexes->opened()) {
            for (std::string const& u : index->unreadable_advisories()) {
                diags.error({}, index_label(index_name) + " has an advisory this otok cannot read, which a newer otok may: " + u);
                ++findings;
            }
        }
    }
    report(diags, ctx.err);
    std::size_t const count = lock->packages.size() + lock->tool_packages.size();
    if (findings == 0) ctx.out << "otok.lock: " << count << (count == 1 ? " package" : " packages") << ", nothing to report\n";
    return diags.has_errors() ? 1 : 0;
}

// otok outdated: for each package of otok.lock, the version update would choose and the newest of its
// recipe (docs/PLAN.md decision 45).
int outdated(parsed_command const& cmd, command_context& ctx)
{
    diagnostics diags;
    auto current = load_project(ctx, cmd, true, {}, diags);
    if (current && !current->lock) diags.error({}, "outdated needs otok.lock, and there is none; run 'otok install' to create it");
    std::unique_ptr<project> compatible;
    if (current && !diags.has_errors()) compatible = load_project(ctx, cmd, false, {}, diags);
    if (!compatible) {
        report(diags, ctx.err);
        return 1;
    }
    std::map<std::string, std::vector<std::string>> unreadable;  // by recipe id: versions needing a newer otok (index.md 3.2)
    auto const latest_of = [&](resolved_package const& pkg) -> std::string {
        if (pkg.chosen.kind != locked_package::recipe_kind::index) return "-";
        if (!current->indexes) return "?";
        auto idx = current->indexes->get(pkg.chosen.index.empty() ? std::string{default_index_name} : pkg.chosen.index);
        if (!idx) return "?";
        auto list = (*idx)->newest_first(pkg.chosen.id);
        for (unreadable_entry const& u : (*idx)->unreadable(pkg.chosen.id)) {
            auto& labels = unreadable[pkg.chosen.id.str()];
            if (std::ranges::find(labels, u.label()) == labels.end()) labels.push_back(u.label());
        }
        return list ? version_text(newest_stable(*list)->projection) : "?";
    };
    std::vector<std::vector<std::string>> rows{{"library", "current", "compatible", "latest", "recipe"}};
    std::size_t behind = 0;
    for (auto [packages, tool] : {std::pair{&current->r.packages, false}, std::pair{&current->r.tool_packages, true}}) {
        for (resolved_package const& pkg : *packages) {
            locked_package const* locked = locked_entry(*current->lock, current->r, pkg, tool);
            // a tool package by its id, which several of one library have (otok-lock.md 5.7), else by its library
            resolved_package const* fresh = !tool ? compatible->r.find(pkg.library, pkg.target)
                : compatible->r.find_tool(pkg.id) ? compatible->r.find_tool(pkg.id) : compatible->r.find_tool(pkg.library);
            std::string const now = locked && locked->version ? locked->version->str() + " (r" + std::to_string(locked->revision) + ")" : "-";
            std::string const update = fresh ? version_text(*fresh->chosen.recipe) : "-";
            std::string const latest = latest_of(pkg);
            if (update != now || (latest != "-" && latest != now)) ++behind;
            std::string recipe = recipe_label(pkg.chosen);
            if (!pkg.chosen.deprecation.empty()) recipe += " (" + pkg.chosen.deprecation + ")";
            rows.push_back({(tool ? "tool " : "") + package_name(pkg, tool, current->r.tool_packages), now, update, latest, recipe});
        }
    }
    report(diags, ctx.err);
    print_columns(rows, ctx.out);
    for (auto const& [id, labels] : unreadable) {
        std::string list;
        for (std::string const& l : labels) list += (list.empty() ? "" : ", ") + l;
        ctx.out << in_quotes(id) << " has versions that need a newer otok: " << list << "\n";
    }
    if (behind == 0) ctx.out << "everything is up to date\n";
    return 0;
}

toml::date_time utc_now()
{
    auto const now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto const day = std::chrono::floor<std::chrono::days>(now);
    std::chrono::year_month_day const ymd{day};
    std::chrono::hh_mm_ss const hms{now - day};
    return toml::date_time{
        toml::date{static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day())},
        toml::time{static_cast<unsigned>(hms.hours().count()), static_cast<unsigned>(hms.minutes().count()),
            static_cast<unsigned>(hms.seconds().count())},
        toml::time_offset{}};
}

// A recipe being published (DESIGN 7.4): its directory in a checkout, where users fetch it, the tree fetched
// back from there, the recipe read from that tree, and its entry.
struct publish_recipe
{
    std::filesystem::path dir;
    locked_location source;            // the remote's URL, HEAD, the directory in the repository
    std::filesystem::path tree;
    std::shared_ptr<recipe const> r;
    std::optional<index_addition> addition;
    bool published = false;            // the index has the same entry (DESIGN 7.4)
};

// The branch of the pull request publishing `fresh` for `publisher`, one per publication: publishing it again updates it.
std::string publish_branch_of(std::string const& publisher, std::vector<publish_recipe const*> const& fresh)
{
    std::string branch = "publish/" + publisher + "/";
    if (fresh.size() == 1) {
        recipe const& r = *fresh.front()->r;
        return branch + r.name + "-" + r.variant + "-" + r.version->str() + "-r" + std::to_string(r.revision);
    }
    std::string paths;
    for (publish_recipe const* p : fresh) paths += p->addition->entry_path + "\n";
    return branch + sha256_hex(paths).substr(0, 12);
}

// The token for the GitHub API: GITHUB_TOKEN, else that of the GitHub CLI; empty when there is none.
std::string github_token(command_context& ctx, cache_store& cache)
{
    auto const trimmed_line = [](std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        return s;
    };
    if (auto token = ctx.env("GITHUB_TOKEN"); token && !token->empty()) return trimmed_line(*token);
    process_spec spec;
    spec.argv = {"gh", "auth", "token"};
    spec.env = current_environment();
    spec.cwd = cache.tmp();
    spec.capture = true;
    auto done = run_process(spec);
    if (!done || !done->success()) return {};
    return trimmed_line(done->output);
}

// The recipes of the git checkout holding `dir`: every directory with a tracked recipe.toml.
result<std::vector<std::filesystem::path>> checkout_recipes(git_tool const& git, std::filesystem::path const& dir, std::filesystem::path const& log)
{
    auto top = run_git(git, {"rev-parse", "--show-toplevel"}, dir, log, true);
    if (!top) return fail("'" + path_to_utf8(dir) + "' is not in a git checkout: " + top.error().message);
    std::filesystem::path const root = path_from_utf8(top->substr(0, top->find_last_not_of("\r\n") + 1));
    auto listed = run_git(git, {"ls-files", "--", "recipe.toml", "*/recipe.toml"}, root, log, true);
    if (!listed) return std::unexpected{listed.error()};
    std::vector<std::filesystem::path> dirs;
    std::size_t start = 0;
    for (std::size_t end; (end = listed->find('\n', start)) != std::string::npos; start = end + 1) {
        std::string const file = listed->substr(start, end - start);
        if (!file.empty()) dirs.push_back((root / path_from_utf8(file)).parent_path());
    }
    if (dirs.empty()) return fail("the checkout '" + path_to_utf8(root) + "' has no recipes");
    return dirs;
}

// otok publish (DESIGN 7.4, docs/PLAN.md decisions 44, 46): recipes committed and pushed in a git checkout,
// built from a fresh fetch, then added to the index through a pull request on GitHub or a checkout of it.
int publish(parsed_command const& cmd, command_context& ctx)
{
    auto const error = [&ctx](std::string const& message) {
        ctx.err << "error: " << message << "\n";
        return 1;
    };
    bool const build = !cmd.flag("no-build");
    bool const dry_run = cmd.flag("dry-run");
    if (dry_run && !build) return error("--dry-run builds the recipe and publishes nothing, so it cannot go with --no-build");
    if (!progress_valid(cmd, ctx)) return 1;

    diagnostics diags;
    auto config = load_config(ctx.env, diags);
    report(diags, ctx.err);
    if (!config) return 1;
    cache_notices notices{
        .waiting = [&ctx](std::string const& what) { ctx.err << "note: " << what << "\n"; },
        .fetching = [&ctx](std::string const& what) { ctx.err << what << "\n"; },
    };
    auto cache = cache_store::open(config->cache_dir, std::move(notices));
    if (!cache) return error(cache.error().message);
    auto git = find_git(current_environment(), config->git);
    if (!git) return error("otok publish needs git, and git is not in PATH");
    std::filesystem::path const log = cache->tmp() / "publish.log";
    auto const git_line = [&](std::vector<std::string> const& args, std::filesystem::path const& dir) -> result<std::string> {
        auto out = run_git(*git, args, dir, log, true);
        if (!out) return std::unexpected{out.error()};
        return out->substr(0, out->find_last_not_of("\r\n") + 1);
    };

    // the index: a checkout, or a repository on GitHub
    std::string const index = cmd.value("index").value_or(config->index);
    if (index.empty()) return error("publish needs the index: --index DIR (a checkout of it) or URL, or 'index' in config.toml");
    std::error_code ec;
    bool const checkout = std::filesystem::is_directory(path_from_utf8(index), ec);
    std::filesystem::path const index_dir = checkout ? std::filesystem::absolute(path_from_utf8(index)) : std::filesystem::path{};
    std::string repository = index;  // the index's git repository
    if (std::string const base = view_base(index); !checkout && !base.empty()) {
        recipe_index view{view_index_source(view_over_http(base, *cache, transport_of(ctx, *config)), base), ctx.targets};
        auto settings = view.settings();
        if (!settings) return error(settings.error().message);
        if (settings->repository.empty()) return error("the index.toml of the view " + base + " names no 'repository' to publish to");
        repository = settings->repository;
    }
    if (!checkout && !github_index::is_github_url(repository)) {
        return error("the index " + in_quotes(repository) + " is neither a directory (a checkout of it) nor a repository on GitHub; "
            "publish to an index elsewhere through a checkout of it");
    }
    std::optional<github_index> github;
    std::string publisher = cmd.value("publisher").value_or("");
    if (checkout && !dry_run) {
        if (publisher.empty()) return error("publishing through a checkout of the index needs --publisher NAME");
        auto status = git_line({"status", "--porcelain"}, index_dir);
        if (!status) return error("the index '" + index + "' is not a git checkout: " + status.error().message);
        if (!status->empty()) return error("the checkout of the index has changes that are not committed:\n" + *status);
    } else if (!checkout && !dry_run) {
        auto opened = github_index::open(repository, github_token(ctx, *cache), transport_of(ctx, *config));
        if (!opened) return error(opened.error().message);
        github = std::move(*opened);
        if (publisher.empty()) {
            auto login = github->login();
            if (!login) return error(login.error().message);
            publisher = lowercase(*login);
        }
    } else if (!checkout) {
        // --dry-run: the index on GitHub when it can be read, for the publisher and the limit of entries
        auto opened = github_index::open(repository, github_token(ctx, *cache), transport_of(ctx, *config));
        if (opened) github = std::move(*opened);
        else if (publisher.empty()) return error("the publisher is not known (" + opened.error().message + "): give --publisher NAME");
    }
    if (publisher.empty()) {
        // --dry-run: the publisher all the same, the name under which recipes published together find each other
        if (checkout) return error("--dry-run with a checkout of the index needs --publisher NAME");
        auto login = github->login();
        if (!login) return error("the publisher is not known (" + login.error().message + "): give --publisher NAME");
        publisher = lowercase(*login);
    }

    // the recipes, fetched back from their remotes: what users will fetch, which also shows they were pushed
    std::vector<std::filesystem::path> dirs;
    if (cmd.flag("all")) {
        if (!cmd.arguments.empty()) return error("--all publishes every recipe of the checkout; name no directories with it");
        auto found = checkout_recipes(*git, ctx.project_dir, log);
        if (!found) return error(found.error().message);
        dirs = std::move(*found);
    } else if (cmd.arguments.empty()) {
        dirs.push_back(ctx.project_dir);
    } else {
        for (std::string const& a : cmd.arguments) dirs.push_back(ctx.project_dir / path_from_utf8(a));
    }
    std::string const remote = cmd.value("remote").value_or("origin");
    std::vector<publish_recipe> recipes;
    for (std::filesystem::path const& dir : dirs) {
        publish_recipe p;
        p.dir = dir;
        std::string const where = "'" + path_to_utf8(dir) + "'";
        auto prefix = git_line({"rev-parse", "--show-prefix"}, dir);
        if (!prefix) return error(where + " is not in a git checkout: " + prefix.error().message);
        auto status = git_line({"status", "--porcelain", "--", "."}, dir);
        if (!status) return error(status.error().message);
        if (!status->empty()) return error("the recipe directory " + where + " has changes that are not committed:\n" + *status);
        auto commit = git_line({"rev-parse", "HEAD"}, dir);
        if (!commit) return error(commit.error().message);
        auto url = git_line({"remote", "get-url", remote}, dir);
        if (!url) return error("the checkout of " + where + " has no remote " + in_quotes(remote) + ": " + url.error().message);
        p.source.type = locked_location::kind::git;
        p.source.git = *url;
        p.source.commit = *commit;
        if (!prefix->empty()) p.source.path = prefix->substr(0, prefix->find_last_not_of('/') + 1);
        auto tree = cache->git_directory(p.source.git, p.source.commit, "", p.source.path, *git);
        if (!tree) return error("cannot fetch " + p.source.commit + " from " + in_quotes(remote) + " (is it pushed?): " + tree.error().message);
        p.tree = *tree;
        // checked as it will be published, before anything is built
        auto addition = make_index_entry(p.tree, p.source, publisher, std::nullopt, ctx.targets);
        if (!addition) return error(addition.error().message);
        auto loaded = load_recipe(p.tree, ctx.targets, diags);
        if (!loaded) {
            report(diags, ctx.err);
            return 1;
        }
        p.r = std::make_shared<recipe const>(std::move(*loaded));
        if (!checkout) {
            if (auto refused = non_https_locations(p.source, *p.r); !refused.empty()) {
                std::string list;
                for (std::string const& r : refused) list += "\n  " + r;
                return error(addition->label + ": an index on GitHub takes https locations only:" + list);
            }
        }
        // the same entry in the index: published already (DESIGN 7.4)
        std::optional<std::string> existing;
        if (checkout) {
            std::filesystem::path const file = index_dir / path_from_utf8(addition->entry_path);
            if (std::filesystem::exists(file, ec)) {
                auto text = read_file(file);
                if (!text) return error(text.error().message);
                existing = std::move(*text);
            }
        } else if (github) {
            auto text = github->file(addition->entry_path);
            if (!text) return error(text.error().message);
            existing = std::move(*text);
        }
        if (existing) {
            auto same = same_index_entry(*existing, addition->entry_text, addition->entry_path, ctx.targets);
            if (!same) return error(same.error().message);
            if (!*same) {
                return error("the index has " + addition->entry_path + " already, published differently; entries never change, so publish a new revision instead");
            }
            p.published = true;
        }
        p.addition = std::move(*addition);
        recipes.push_back(std::move(p));
    }

    // the index's limit of entries per publisher, before anything is built (DESIGN 7.4, index.md 9.2): a pull request's, which
    // index CI would refuse; none for a maintainer
    std::size_t const adding = static_cast<std::size_t>(std::ranges::count_if(recipes, [](publish_recipe const& p) { return !p.published; }));
    if (github && !github->can_push() && adding > 0) {
        diagnostics ignored;  // index CI reads them; invalid ones are not the publisher's
        index_settings settings;
        auto settings_text = github->file("index.toml");
        if (!settings_text) return error(settings_text.error().message);
        if (*settings_text) {
            if (auto s = parse_index_settings(**settings_text, "index.toml", ignored, index_reading::client)) settings = std::move(*s);
        }
        auto publisher_text = github->file("publishers/" + publisher + ".toml");
        if (!publisher_text) return error(publisher_text.error().message);
        std::optional<publisher_file> file;
        if (*publisher_text) file = parse_publisher(**publisher_text, "publishers/" + publisher + ".toml", ignored, index_reading::client);
        publication_limit l = publication_limit_for(settings, file && file->established);
        l.adding = adding;
        std::vector<publish_recipe const*> unpublished;
        for (publish_recipe const& p : recipes) {
            if (!p.published) unpublished.push_back(&p);
        }
        std::string const publish_branch = publish_branch_of(publisher, unpublished);
        auto open = github->open_pull_requests();
        if (!open) return error(open.error().message);
        for (open_pull_request const& r : *open) {
            if (r.head.ends_with(":" + publish_branch)) continue;  // updated by this publication
            auto const n = std::ranges::count_if(r.added, [&](std::string const& p) { return entry_publisher(p) == publisher; });
            if (n == 0) continue;
            l.pending += static_cast<std::size_t>(n);
            l.pending_requests.push_back(r.number);
        }
        git_tool reader = *git;
        for (auto const& [name, value] : git_token_env(repository, github_token(ctx, *cache), current_environment())) reader.env[name] = value;
        auto dir = index_checkout(repository, "", *cache, &reader);
        if (!dir) return error(dir.error().message);
        l = publication_limit_of(*dir, publisher, l, std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        if (l.exceeded()) {
            std::string const text = publication_limit_text(publisher, l);
            if (!dry_run) return error(text + "; nothing is built");
            ctx.err << "warning: " << text << "\n";
        }
    }

    // the builds: a project of its own per recipe and option set, its dependencies from this index
    if (build) {
        std::vector<std::unique_ptr<build_target>> machine;
        if (ctx.make_build_targets) machine = ctx.make_build_targets(*config);
        environment const base = ctx.env;
        environment const env = [base, checkout, index, index_dir](std::string_view name) -> std::optional<std::string> {
            if (checkout && name == "OTOK_INDEX_MIRROR") return path_to_utf8(index_dir);
            if (!checkout && name == "OTOK_INDEX") return index;
            return base(name);
        };
        // the index on GitHub read by git with the token of the publication
        env_vars const git_env = checkout ? env_vars{} : git_token_env(index, github_token(ctx, *cache), current_environment());
        // the recipes published together depend on each other as they will once the entries are in
        std::vector<index_addition> unpublished;
        for (publish_recipe const& p : recipes) {
            if (!p.published) unpublished.push_back(*p.addition);
        }
        // a project per recipe, environment of this machine and option set (docs/PLAN.md decision 82)
        struct publish_build
        {
            std::string text;           // "p/b 2.0-r1 (linux) for linux in linux/gcc with {level = 2}"
            std::string label;          // "p/b 2.0-r1 (linux) in linux/gcc with {level = 2}": the display's while it packages
            std::string manifest;
            std::filesystem::path dir;
            bool tool = false;          // a tool recipe, in [tool-deps] (publish_project::tool)
            int result = 0;
        };
        std::vector<publish_build> builds;
        for (publish_recipe& p : recipes) {
            if (p.published && !dry_run) continue;
            auto target = publish_target(*p.r, ctx.targets);
            if (!target) return error(p.addition->label + ": " + target.error().message);
            auto const builder_of = std::ranges::find_if(machine, [&](auto const& t) { return t->info().name == (*target)->name; });
            if (builder_of == machine.end()) return error(p.addition->label + ": this otok cannot build for '" + (*target)->name + "' on this machine");
            // the recipe's environments this machine has: the publisher's check (docs/PLAN.md decision 82)
            std::vector<build_environment> environments;
            std::string missing;
            for (build_environment const& e : p.r->build_on) {
                if (auto found = machine_environment_of(**builder_of, {e}); found) environments.push_back(e);
                else missing += "\n  " + found.error().message;
            }
            if (environments.empty()) return error(p.addition->label + ": this machine has none of the environments it builds in:" + missing);
            std::vector<option_values const*> sets{nullptr};
            for (option_values const& check : p.r->publish_checks) sets.push_back(&check);
            for (build_environment const& e : environments) {
                for (option_values const* options : sets) {
                    auto project = make_publish_project(*p.r, p.source, **target, e, options);
                    if (!project) return error(p.addition->label + ": " + project.error().message);
                    std::string text = p.addition->label + " for";
                    for (std::size_t i = 0; i < project->platforms.size(); ++i) text += (i ? ", " : " ") + project->platforms[i];
                    text += " in " + e.text();
                    std::string label = p.addition->label + " in " + e.text();
                    if (options) {
                        std::string values;
                        for (auto const& [name, value] : *options) values += (values.empty() ? "" : ", ") + name + " = " + format_option_value(value.value);
                        text += " with {" + values + "}";
                        label += " with {" + values + "}";
                    }
                    ctx.out << "building " << text << "\n";
                    builds.push_back({text, label, project->manifest, cache->tmp() / ("publish-" + std::to_string(builds.size() + 1)), project->tool, 0});
                }
            }
        }

        // all at once (docs/PLAN.md decision 60), with the jobs of one jobserver and one display; one after another
        // with -vv, whose output would interleave
        job_broker broker{config->jobs, cache->tmp()};
        bool const one_at_a_time = cmd.count("verbose") >= 2;
        {
            std::optional<progress_display::terminal> live;
            if (ctx.terminal.live && cmd.value("progress").value_or("auto") == "auto") live = progress_display::terminal{.utf8 = ctx.terminal.utf8};
            // what the builds write, line by line, each thread's apart; on a live terminal all of it above the slices
            progress_display err_lines{ctx.err, std::move(live)};
            progress_display out_lines{ctx.out, std::nullopt};
            std::ostream sub_err{&err_lines.lines()};
            std::ostream summary{err_lines.live() ? &err_lines.lines() : &out_lines.lines()};
            // what the installs of the temporary projects sum up (their lock, inputs, slices, output) is not this command's
            std::ostream nothing{nullptr};
            announcements announced;
            auto const run_build = [&](publish_build& b) {
                std::error_code removed;
                std::filesystem::remove_all(b.dir, removed);
                std::filesystem::create_directories(b.dir, removed);
                if (auto ok = write_file_atomic(b.dir / "otok.toml", b.manifest); !ok) {
                    sub_err << "error: " << ok.error().message << "\n";
                    b.result = 1;
                    return;
                }
                command_context sub{b.dir, ctx.targets, nullptr, nothing, sub_err, env};
                sub.nested = true;
                sub.announced = &announced;
                sub.label = b.label;
                sub.make_build_targets = ctx.make_build_targets;
                sub.launcher = ctx.launcher;
                sub.probe_version = ctx.probe_version;
                sub.unpublished = unpublished;
                sub.tool_roots = b.tool;
                sub.git_env = git_env;
                sub.display = &err_lines;
                sub.jobs = one_at_a_time ? nullptr : &broker;
                parsed_command install;
                install.command = "install";
                // what the build shows, as asked
                for (char const* name : {"verbose", "keep-build-dirs", "no-sandbox", "no-virtual-paths"}) {
                    if (cmd.flag(name)) {
                        install.flags.insert(name);
                        install.counts[name] = cmd.count(name);
                    }
                }
                auto const started = std::chrono::steady_clock::now();
                b.result = install_pipeline(install, sub, true, {}, false);
                if (b.result == 0) summary << "built " << b.text << " (" << detail::elapsed_text(std::chrono::steady_clock::now() - started) << ")\n";
                std::filesystem::remove_all(b.dir, removed);
            };
            if (one_at_a_time) {
                for (publish_build& b : builds) run_build(b);
            } else {
                std::vector<std::thread> threads;
                for (publish_build& b : builds) threads.emplace_back([&run_build, &b] { run_build(b); });
                for (std::thread& t : threads) t.join();
            }
        }
        // every build that failed, not only the first
        std::string failed;
        std::size_t failures = 0;
        for (publish_build const& b : builds) {
            if (b.result == 0) continue;
            failed += (failed.empty() ? "" : ", ") + b.text;
            ++failures;
        }
        if (failures > 0) return error(std::string{failures == 1 ? "the build of " : "the builds of "} + failed + " failed; nothing is published");

    }
    if (dry_run) {
        ctx.out << "built; --dry-run publishes nothing\n";
        return 0;
    }

    std::vector<publish_recipe const*> fresh;
    for (publish_recipe const& p : recipes) {
        if (p.published) ctx.out << p.addition->label << " is published already\n";
        else fresh.push_back(&p);
    }
    if (fresh.empty()) return 0;
    std::string title = "Publish";
    for (std::size_t i = 0; i < fresh.size(); ++i) title += (i ? ", " : " ") + fresh[i]->addition->label;

    if (checkout) {
        // pushed by one who may push to the index: the time is theirs to write, as the workflow writes it on GitHub (index.md 3)
        toml::date_time const published = utc_now();
        std::vector<std::string> add{"add", "--"};
        for (publish_recipe const* p : fresh) {
            index_addition addition = *p->addition;
            auto stamped = stamp_index_entry(addition.entry_text, published);
            if (!stamped) return error(addition.entry_path + ": " + stamped.error().message);
            addition.entry_text = std::move(*stamped);
            auto added = add_index_entry(index_dir, addition, ctx.targets);
            if (!added) return error(added.error().message);
            add.push_back(p->addition->entry_path);
            add.push_back(p->addition->library_path);
            ctx.out << "added " << p->addition->entry_path << "\n";
        }
        if (auto ok = git_line(add, index_dir); !ok) return error(ok.error().message);
        if (auto ok = git_line({"commit", "-q", "-m", title}, index_dir); !ok) return error(ok.error().message);
        if (cmd.flag("no-push")) {
            ctx.out << "committed to the index; push it to publish\n";
            return 0;
        }
        if (auto ok = git_line({"push", "-q"}, index_dir); !ok) return error("cannot push the index (pull, then publish again): " + ok.error().message);
        ctx.out << "pushed to the index\n";
        return 0;
    }

    std::vector<github_file> files;
    std::set<std::string, std::less<>> libraries;
    std::string body = "Published with otok " + std::string{build_info::version} + ":\n";
    for (publish_recipe const* p : fresh) {
        files.push_back({p->addition->entry_path, p->addition->entry_text});
        body += "\n- " + p->addition->label + " from " + p->source.git + " at " + p->source.commit;
        if (!libraries.insert(p->addition->library_path).second) continue;
        auto library = github->file(p->addition->library_path);
        if (!library) return error(library.error().message);
        if (!*library) files.push_back({p->addition->library_path, p->addition->library_text});
    }
    // the publisher's file when the index has none: this pull request registers the name for the token's account, which an
    // index maintainer accepts by merging it (index.md 6, 9.1); an organization is registered by a maintainer
    std::string const publisher_path = "publishers/" + publisher + ".toml";
    auto registered = github->file(publisher_path);
    if (!registered) return error(registered.error().message);
    if (!*registered) {
        auto account = github->account();
        if (!account) return error("cannot read the GitHub account of the token, to register the publisher " + in_quotes(publisher) + ": " + account.error().message);
        publisher_file registration;
        registration.github_id = account->id;
        files.push_back({publisher_path, write_publisher(registration)});
        body += "\n\nRegisters the publisher " + in_quotes(publisher) + " for the GitHub account " + account->login + ".";
        ctx.out << "registering the publisher " << in_quotes(publisher) << " for the GitHub account " << account->login
                << ": an index maintainer accepts the name by merging the pull request\n";
    }
    auto pr = github->propose(publish_branch_of(publisher, fresh), title, body + "\n", files);
    if (!pr) return error(pr.error().message);
    ctx.out << (pr->reused ? "updated the pull request " : "opened the pull request ") << pr->url << "\n";
    // what its last check said (index.md 9): the check of this update takes minutes, so the comment is still the earlier one's
    if (pr->reused && pr->number > 0) {
        if (auto comments = github->bot_comments(pr->number)) {
            for (auto it = comments->rbegin(); it != comments->rend(); ++it) {
                auto const summary = summary_of_comment(*it);
                if (!summary) continue;
                auto const j = nlohmann::json::parse(*summary, nullptr, false);
                std::string const verdict = j.value("verdict", "");
                std::string const said = verdict == "auto" ? "passed" : verdict == "maintainer" ? "waited for an index maintainer" : "was refused";
                ctx.out << "its last check, before this update: it " << said << (verdict == "auto" ? "\n" : ":\n");
                if (j.contains("reasons") && j["reasons"].is_array()) {
                    for (auto const& r : j["reasons"]) {
                        if (r.is_string()) ctx.out << "  " << r.get<std::string>() << "\n";
                    }
                }
                break;
            }
        }
    }
    ctx.out << "index CI checks it, then merges it or says in it why not: follow the publication at " << pr->url << "\n";
    return 0;
}

// The machine settings, the cache and git of the index commands.
struct index_tools
{
    machine_config config;
    std::optional<cache_store> cache;
    git_tool git;
};

std::optional<index_tools> open_index_tools(command_context& ctx)
{
    diagnostics diags;
    auto config = load_config(ctx.env, diags);
    report(diags, ctx.err);
    if (!config) return std::nullopt;
    cache_notices notices{
        .waiting = [&ctx](std::string const& what) { ctx.err << "note: " << what << "\n"; },
        .fetching = [&ctx](std::string const& what) { ctx.err << what << "\n"; },
    };
    auto cache = cache_store::open(config->cache_dir, std::move(notices));
    if (!cache) {
        ctx.err << "error: " << cache.error().message << "\n";
        return std::nullopt;
    }
    auto git = find_git(current_environment(), config->git);
    if (!git) {
        ctx.err << "error: the index commands need git, and git is not in PATH\n";
        return std::nullopt;
    }
    return index_tools{std::move(*config), std::move(*cache), std::move(*git)};
}

// otok index-check (index.md 9, docs/PLAN.md decision 47): the changes of a checkout of the index since
// --base, as index CI checks a pull request.
int index_check(parsed_command const& cmd, command_context& ctx)
{
    auto tools = open_index_tools(ctx);
    if (!tools) return 1;
    auto const base = cmd.value("base");
    if (!base) {
        ctx.err << "error: index-check needs --base REV, the commit the changes are made on\n";
        return 1;
    }
    index_check_settings settings;
    settings.root = cmd.arguments.empty() ? ctx.project_dir : ctx.project_dir / path_from_utf8(cmd.arguments.front());
    settings.base = *base;
    for (auto [option, out, what] : {std::tuple{"author-id", &settings.author_id, "a GitHub account id"},
             std::tuple{"pull-request", &settings.pull_request, "the number of a pull request"},
             std::tuple{"approved-by", &settings.approved_by, "a GitHub account id"}}) {
        auto value = cmd.value(option);
        if (!value) continue;
        std::int64_t id = 0;
        auto const [end, ec] = std::from_chars(value->data(), value->data() + value->size(), id);
        if (ec != std::errc{} || end != value->data() + value->size() || id <= 0) {
            ctx.err << "error: --" << option << " must be " << what << ", not " << in_quotes(*value) << "\n";
            return 1;
        }
        *out = id;
    }
    settings.repository = cmd.value("repository").value_or("");
    settings.stamp = cmd.flag("stamp");
    settings.targets = ctx.targets;
    settings.cache = &*tools->cache;
    settings.git = &tools->git;
    settings.http.proxy = tools->config.proxy;
    settings.github.transport = transport_of(ctx, tools->config);
    settings.github.token = ctx.env("GITHUB_TOKEN").value_or("");
    auto report = check_index(settings);
    if (!report) {
        ctx.err << "error: " << report.error().message << "\n";
        return 1;
    }
    for (std::string const& e : report->errors) ctx.err << "error: " << e << "\n";
    for (std::string const& r : report->reasons) ctx.out << "for a maintainer: " << r << "\n";
    for (std::string const& n : report->notes) ctx.out << "note: " << n << "\n";
    ctx.out << report->entries << (report->entries == 1 ? " new entry" : " new entries") << " checked; verdict: "
            << verdict_name(report->verdict) << "\n";
    for (std::string const& s : report->stamped) ctx.out << "published " << s << "\n";
    for (auto const& [option, text] : {std::pair{"summary", summary_json(*report)}, std::pair{"comment", check_comment(*report)}}) {
        auto const file = cmd.value(option);
        if (!file) continue;
        if (auto ok = write_file_atomic(ctx.project_dir / path_from_utf8(*file), text); !ok) {
            ctx.err << "error: " << ok.error().message << "\n";
            return 1;
        }
    }
    return report->verdict == index_verdict::refused ? 1 : 0;
}

// otok index-probe (index.md 11): the inputs of every version, into availability.toml.
int index_probe(parsed_command const& cmd, command_context& ctx)
{
    auto tools = open_index_tools(ctx);
    if (!tools) return 1;
    index_probe_settings settings;
    settings.root = cmd.arguments.empty() ? ctx.project_dir : ctx.project_dir / path_from_utf8(cmd.arguments.front());
    settings.targets = ctx.targets;
    settings.cache = &*tools->cache;
    settings.git = &tools->git;
    settings.http = transport_of(ctx, tools->config);
    auto probed = probe_index(settings);
    if (!probed) {
        ctx.err << "error: " << probed.error().message << "\n";
        return 1;
    }
    std::filesystem::path const output = cmd.value("output") ? ctx.project_dir / path_from_utf8(*cmd.value("output")) : settings.root / "availability.toml";
    if (auto ok = write_file_atomic(output, probed->availability); !ok) {
        ctx.err << "error: " << ok.error().message << "\n";
        return 1;
    }
    ctx.out << probed->versions << " versions probed, " << probed->unavailable << " with inputs unavailable\n";
    return 0;
}

// The commit of a clean checkout of the index, which index-view and index-catalog generate from.
result<std::string> clean_checkout_commit(git_tool const& git, std::filesystem::path const& root, std::filesystem::path const& log)
{
    auto status = run_git(git, {"status", "--porcelain"}, root, log, true);
    if (!status) return fail("'" + path_to_utf8(root) + "' is not a checkout of the index: " + status.error().message);
    if (!status->empty()) return fail("the checkout of the index has changes that are not committed:\n" + *status);
    auto head = run_git(git, {"rev-parse", "HEAD"}, root, log, true);
    if (!head) return std::unexpected{head.error()};
    return head->substr(0, head->find_first_of("\r\n"));
}

// otok index-view (index.md 8, docs/PLAN.md decision 48): the HTTPS view of a checkout of the index.
int index_view(parsed_command const& cmd, command_context& ctx)
{
    auto const error = [&ctx](std::string const& message) {
        ctx.err << "error: " << message << "\n";
        return 1;
    };
    auto const output = cmd.value("output");
    if (!output) return error("index-view needs --output DIR, where the view is written");
    auto tools = open_index_tools(ctx);
    if (!tools) return 1;
    std::filesystem::path const root = cmd.arguments.empty() ? ctx.project_dir : ctx.project_dir / path_from_utf8(cmd.arguments.front());
    std::filesystem::path const out = ctx.project_dir / path_from_utf8(*output);
    auto commit = clean_checkout_commit(tools->git, root, tools->cache->tmp() / "index-view.log");
    if (!commit) return error(commit.error().message);
    auto written = write_index_view(root, *commit, out, ctx.targets);
    if (!written) return error(written.error().message);
    ctx.out << "view of " << *commit << ": " << written->written << " files written, " << written->removed << " removed, "
            << written->unchanged << " unchanged\n";
    return 0;
}

// otok index-catalog (DESIGN 8, docs/PLAN.md decision 49): the catalog site of a checkout of the index.
int index_catalog(parsed_command const& cmd, command_context& ctx)
{
    auto const error = [&ctx](std::string const& message) {
        ctx.err << "error: " << message << "\n";
        return 1;
    };
    auto const output = cmd.value("output");
    if (!output) return error("index-catalog needs --output DIR, where the site is written");
    auto tools = open_index_tools(ctx);
    if (!tools) return 1;
    catalog_settings settings;
    settings.root = cmd.arguments.empty() ? ctx.project_dir : ctx.project_dir / path_from_utf8(cmd.arguments.front());
    settings.output = ctx.project_dir / path_from_utf8(*output);
    settings.targets = ctx.targets;
    settings.cache = &*tools->cache;
    settings.git = &tools->git;
    auto commit = clean_checkout_commit(tools->git, settings.root, tools->cache->tmp() / "index-catalog.log");
    if (!commit) return error(commit.error().message);
    settings.commit = *commit;
    auto written = write_index_catalog(settings);
    if (!written) return error(written.error().message);
    for (std::string const& why : written->unavailable) ctx.err << "warning: " << why << "\n";
    ctx.out << "catalog of " << *commit << ": " << written->files.written << " files written, " << written->files.removed << " removed, "
            << written->files.unchanged << " unchanged\n";
    return 0;
}

int help(parsed_command const& cmd, command_context& ctx)
{
    if (cmd.arguments.empty()) {
        ctx.out << general_usage("otok", global_options(), command_specs());
        return 0;
    }
    auto const commands = command_specs();
    auto const it = std::ranges::find(commands, std::string_view{cmd.arguments.front()}, &command_spec::name);
    if (it == commands.end()) {
        ctx.err << "error: unknown command " << in_quotes(cmd.arguments.front()) << "; see 'otok help'\n";
        return 1;
    }
    ctx.out << command_usage("otok", *it);
    return 0;
}

}

std::span<option_spec const> global_options()
{
    static option_spec const options[] = {
        {"directory", 'C', "DIR", "Run as if otok was started in DIR"},
        {"version", 0, "", "Print the version and exit"},
        {"help", 'h', "", "Print this help and exit"},
    };
    return options;
}

std::span<command_spec const> command_specs()
{
    static command_spec const commands[] = {
        {"install", "", "Resolve the dependencies (keeping locked versions), write otok.lock, fetch the inputs and build the libraries into the output",
            {build_profile_option, locked_option, offline_option, frozen_option, allow_advisory_option, verbose_option, keep_build_dirs_option, no_sandbox_option, no_virtual_paths_option, progress_option, no_dev_option}, 0, 0},
        {"update", "[LIBRARY...]", "Move every library (or the named ones) to the newest allowed version, then as install",
            {build_profile_option, offline_option, allow_advisory_option, verbose_option, keep_build_dirs_option, no_sandbox_option, no_virtual_paths_option, progress_option, no_dev_option, drop_option}, 0, any_number},
        {"fetch", "", "Resolve and write otok.lock as install does, then fetch the inputs of every package in it (all profiles); build nothing",
            {locked_option, offline_option, frozen_option, allow_advisory_option, no_dev_option}, 0, 0},
        {"tree", "", "Print the resolved graph with versions, recipes, platforms and changed options", {profile_option, offline_option, no_dev_option}, 0, 0},
        {"why", "LIBRARY", "Show who requires a library and who asked for each of its option values", {profile_option, offline_option}, 1, 1},
        {"audit", "", "Check otok.lock against the advisories of the index, [policy] and deprecated recipes (exit 1 on a withdrawn or security advisory or a refused license)", {offline_option}, 0, 0},
        {"outdated", "", "List, for each locked library, the version update would choose and the newest of its recipe", {profile_option, offline_option}, 0, 0},
        {"search", "TEXT...", "Find recipes of the index by id, library, description or keyword", {offline_option}, 1, any_number},
        {"info", "RECIPE|LIBRARY", "Show the versions, options and dependencies of a recipe of the index, or the recipes of a library", {offline_option}, 1, 1},
        {"metadata", "", "Print the resolved graph, the targets and where each library lands, as JSON for IDEs and scripts", {profile_option, offline_option}, 0, 0},
        {"clean", "", "Remove the cache of this machine, or the selected parts of it",
            {{"downloads", 0, "", "Remove downloaded files"}, {"sources", 0, "", "Remove unpacked sources and git repositories"},
                {"builds", 0, "", "Remove built libraries, the work directories of dev builds and kept work directories of failed builds"},
                {"unused", 0, "", "Remove only the entries unused for cache-max-age days"},
                {"days", 0, "N", "With --unused: entries unused for N days"}, {"dry-run", 0, "", "Show what would be removed"}}, 0, 0},
        {"publish", "[DIR...]", "Build the recipe in DIR (committed and pushed) from its remote and add its version entry to the index",
            {{"index", 0, "DIR|URL", "A checkout of the index (committed and pushed to), or its repository on GitHub (a pull request)"},
                {"publisher", 0, "NAME", "The publisher the recipe is published under (default on GitHub: the token's account)"},
                {"remote", 0, "NAME", "The git remote users fetch the recipe from (default: origin)"},
                {"all", 0, "", "Every recipe of the checkout; those published already are skipped"},
                {"dry-run", 0, "", "Build, and publish nothing"}, {"no-build", 0, "", "Add the entry without building the recipe"},
                {"no-push", 0, "", "With a checkout of the index: commit the entry, but do not push"},
                verbose_option,
                keep_build_dirs_option, no_sandbox_option, no_virtual_paths_option, progress_option}, 0, any_number},
        {"licenses", "", "Show the licenses of the third-party software built into otok, and where otok's source is", {}, 0, 0},
        {"log", "LIBRARY [SLICE]", "Show the log of the latest build of a library (or of one slice of it) kept in the cache, a failed one too",
            {{"list", 0, "", "List the logs of the library kept in the cache, the newest first"}}, 1, 2},
        {"index-check", "[DIR]", "Check the changes of a checkout of the index as index CI does with a pull request (for index maintainers)",
            {{"base", 0, "REV", "The commit the changes are made on"}, {"author-id", 0, "N", "The GitHub account id of their author"},
                {"repository", 0, "OWNER/NAME", "The index's repository on GitHub: its maintainers and open pull requests"},
                {"pull-request", 0, "N", "The number of the pull request checked, left out of the open ones"},
                {"approved-by", 0, "N", "The GitHub account id of who approved it: an index maintainer's approval lets the workflow merge it"},
                {"stamp", 0, "", "With the verdict auto, write the time of the merge into the new entries (published)"},
                {"summary", 0, "FILE", "Write the verdict as JSON"}, {"comment", 0, "FILE", "Write the verdict as the Markdown of a comment for the pull request"}},
            0, 1},
        {"index-probe", "[DIR]", "Probe the inputs of every version of a checkout of the index into availability.toml (for index maintainers)",
            {{"output", 0, "FILE", "Write it elsewhere"}}, 0, 1},
        {"index-view", "[DIR]", "Generate the HTTPS view of a checkout of the index (for index maintainers)",
            {{"output", 0, "DIR", "Where the view is written: its files go to DIR/index/"}}, 0, 1},
        {"index-catalog", "[DIR]", "Generate the catalog site of a checkout of the index, beside its view (for index maintainers)",
            {{"output", 0, "DIR", "Where the site is written"}}, 0, 1},
        {"help", "[COMMAND]", "Print help for otok or for a command", {}, 0, 1},
    };
    return commands;
}

int run_command(parsed_command const& command, command_context& ctx)
{
    if (command.command == "install") return install(command, ctx);
    if (command.command == "update") return update(command, ctx);
    if (command.command == "fetch") return fetch(command, ctx);
    if (command.command == "tree") return tree(command, ctx);
    if (command.command == "why") return why(command, ctx);
    if (command.command == "metadata") return metadata(command, ctx);
    if (command.command == "audit") return audit(command, ctx);
    if (command.command == "outdated") return outdated(command, ctx);
    if (command.command == "search") return search(command, ctx);
    if (command.command == "info") return info(command, ctx);
    if (command.command == "clean") return clean(command, ctx);
    if (command.command == "log") return show_log(command, ctx);
    if (command.command == "licenses") return licenses(command, ctx);
    if (command.command == "publish") return publish(command, ctx);
    if (command.command == "index-check") return index_check(command, ctx);
    if (command.command == "index-probe") return index_probe(command, ctx);
    if (command.command == "index-view") return index_view(command, ctx);
    if (command.command == "index-catalog") return index_catalog(command, ctx);
    if (command.command == "help") return help(command, ctx);
    ctx.err << "error: unknown command " << in_quotes(command.command) << "\n";
    return 1;
}

}
