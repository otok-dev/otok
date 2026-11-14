// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/resolver/resolver.hpp"

#include <algorithm>
#include <compare>
#include <expected>
#include <memory>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

namespace otok {

namespace {

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

std::string joined(auto const& names)
{
    std::string text;
    for (auto const& n : names) text += (text.empty() ? "" : ", ") + in_quotes(n);
    return text;
}

// Continuation lines of a multi-line message, indented under its first line.
std::string indented(std::string_view text, std::string_view indent)
{
    std::string out;
    for (char const c : text) {
        out += c;
        if (c == '\n') out += indent;
    }
    return out;
}

// Stops the whole resolution: a recipe that cannot be read, the step limit.
struct fatal_error
{
    std::string message;
};

// Why a search branch failed. Causes are the failures of deeper choices.
struct failure
{
    std::string headline;
    std::vector<std::string> details;
    std::vector<std::shared_ptr<failure const>> causes;
};

using failure_ptr = std::shared_ptr<failure const>;

std::string render(failure const& f, std::size_t depth)
{
    std::string const indent(2 * (depth + 1), ' ');
    std::string text = indented(f.headline, indent);
    for (std::string const& d : f.details) text += "\n" + indent + indented(d, indent + "  ");
    if (depth < 3) {
        std::set<std::string> shown;
        for (failure_ptr const& cause : f.causes) {
            if (shown.insert(cause->headline).second) text += "\n" + indent + render(*cause, depth + 1);
        }
    }
    return text;
}

// An edge into a library: a dependency of a chosen package, or a root.
struct incoming
{
    std::string requester;                       // empty for a project root (the choice adds the requirement)
    version_constraint const* constraint = nullptr;
    bool prerelease = false;
    recipe_id const* default_recipe = nullptr;
    option_values const* requires_options = nullptr;
    std::string const* linkage = nullptr;        // the edge's [deps.<library>] linkage
    recipe_ref const* ref = nullptr;             // a recipe the edge requires: a tool taken from a dependency (recipe-toml.md 13.2)
};

bool same_ref(recipe_ref const& a, recipe_ref const& b)
{
    return a.source == b.source && a.id == b.id && a.git == b.git && a.rev == b.rev && a.path == b.path;
}

std::string ref_text(recipe_ref const& r)
{
    switch (r.source) {
    case recipe_ref::kind::index: return r.id.str();
    case recipe_ref::kind::git: return "git " + r.git + " " + r.rev + " " + r.path;
    case recipe_ref::kind::path: return "path " + r.path;
    }
    return {};
}

struct requirement
{
    std::string requester;
    std::optional<version_constraint> constraint;
    bool prerelease = false;
};

struct node_config
{
    std::set<std::string> platforms;
    std::map<std::string, std::vector<option_request>, std::less<>> requests;
    merged_options merged;
    std::set<std::pair<std::size_t, std::string>> active;  // (edge, platform) that hold
    // the linkages the dependents' recipes ask for (recipe-toml.md 9): platform → (requester, linkage)
    std::map<std::string, std::vector<std::pair<std::string, std::string>>, std::less<>> linkage_requests;
};

// A library in one graph: the variant of the chosen version, and what each configuration builds of it.
struct node_graph
{
    int chosen = -1;  // in node::candidates
    // the environment the chosen variant builds in: the graph's, or a substitute of it (docs/PLAN.md decision 91)
    build_environment environment{};
    std::vector<node_config> configs;

    bool present() const noexcept
    {
        return std::ranges::any_of(configs, [](node_config const& c) { return !c.platforms.empty(); });
    }
};

// A library in every graph: one version for all (otok-lock.md 5.4).
struct node
{
    std::string library;
    recipe_ref ref;
    std::string ref_requester;  // who chose the recipe
    std::vector<requirement> requirements;
    std::shared_ptr<std::vector<candidate> const> candidates;
    int chosen = -1;            // a candidate of the chosen version
    std::vector<node_graph> graphs;
};

struct state
{
    std::map<std::string, node, std::less<>> nodes;
};

// The project's entry for a library: recipe, constraint, option values per configuration.
struct project_choice
{
    dependency const* dep = nullptr;
    std::vector<std::vector<std::pair<std::string, option_request>>> options;  // per configuration: option → request
};

struct root
{
    std::string library;
    incoming in;
    std::vector<std::vector<std::set<std::string>>> platforms;  // per graph and configuration
};

// What one resolution (target or tool) works on.
struct graph_spec
{
    bool tool = false;
    std::vector<resolution_graph> graphs;
    // per graph of the target resolution: the environments its libraries may be built in when their recipes do not build in
    // the graph's (target_info::substitutes, docs/PLAN.md decision 91); none for tool graphs
    std::vector<std::vector<build_environment>> substitutes;
    std::vector<std::string> labels;  // per configuration: "" or "in profile 'ci': "
    std::map<std::string, std::string, std::less<>> os_of_platform;
    std::map<std::string, std::string, std::less<>> target_of_platform;
    std::vector<std::map<std::string, std::vector<std::string>, std::less<>>> architectures;  // per configuration
    std::map<std::string, project_choice, std::less<>> choices;
    std::vector<root> roots;
    lock_file const* preferred = nullptr;
    std::set<std::string, std::less<>> unlocked;
    // the graph of a tool dependency (otok-lock.md 5.7): library → the versions the lock references for it there
    std::map<std::string, std::vector<std::string>, std::less<>> tool_preference;
    // The linkage the project asks for (otok-toml.md 5): configuration, library, platform → linkage;
    // none in the tool resolution, whose packages are static. `chosen_linkage`: the one it chooses for the
    // library itself, which wins over the dependents' requests; `linkage` comes after them.
    std::function<std::string(std::size_t, std::string const&, std::string const&)> linkage;
    std::function<std::optional<std::string>(std::size_t, std::string const&)> chosen_linkage;
    // The standard of a language the project asks for (otok-toml.md 5.1), in the same way; none: the recipe chooses.
    std::function<std::optional<int>(std::size_t, std::string const&, std::string const&, language)> standard;

    // "for 'windows' in linux/clang-cl: " when there are several graphs
    std::string graph_label(std::size_t g) const
    {
        if (graphs.size() < 2) return {};
        return tool ? "for the tools of " + graphs[g].environment.text() + ": "
                    : "for " + in_quotes(graphs[g].target) + " in " + graphs[g].environment.text() + ": ";
    }

    // The lock's entry of a library in a graph.
    locked_package const* locked(std::string const& library, std::size_t g) const
    {
        if (!preferred || unlocked.contains(library)) return nullptr;
        std::string const environment = graphs[g].environment.text();
        if (!tool) return preferred->find(library, graphs[g].target, environment);
        auto const versions = tool_preference.find(library);
        if (versions == tool_preference.end()) return nullptr;
        for (std::string const& v : versions->second) {
            if (auto const* p = preferred->find_tool(tool_reference(library, v), environment)) return p;
        }
        return nullptr;
    }

    // The lock's tool package of a library and version in a graph, whoever it was resolved for: one tool package of a
    // library and version in a graph (otok-lock.md 5.7), so a request the lock does not know yet (a new tool dependency)
    // takes its revision, which the packages locked before keep (docs/PLAN.md decision 120).
    locked_package const* locked_tool(std::string const& library, std::string const& version, std::size_t g) const
    {
        if (!tool || !preferred || unlocked.contains(library)) return nullptr;
        return preferred->find_tool(tool_reference(library, version), graphs[g].environment.text());
    }
};

// The linkage of `n` in graph `g` and configuration `c` for platform `p` (otok-toml.md 5) and who asked for it ("" for
// the project): the project's choice for the library, else its dependents' one request, else the project's target tables.
// Dependents asking for different ones without a choice of the project are an error.
std::expected<std::pair<std::string, std::string>, std::string> node_linkage(graph_spec const& spec, node const& n, std::size_t g,
    std::size_t c, std::string const& p)
{
    if (spec.chosen_linkage) {
        if (auto chosen = spec.chosen_linkage(c, n.library)) return std::pair{std::move(*chosen), std::string{}};
    }
    node_config const& nc = n.graphs[g].configs[c];
    if (auto it = nc.linkage_requests.find(p); it != nc.linkage_requests.end() && !it->second.empty()) {
        auto const& requests = it->second;
        if (std::ranges::any_of(requests, [&](auto const& r) { return r.second != requests.front().second; })) {
            std::string message = "the dependents of " + in_quotes(n.library) + " ask for different linkages for " + in_quotes(p) + ":";
            for (auto const& [requester, linkage] : requests) message += "\n  " + requester + ": " + linkage;
            return std::unexpected(message + "\nchoose one in otok.toml: [deps." + n.library + "] linkage = \"...\"");
        }
        return std::pair{requests.front().second, requests.front().first};
    }
    return std::pair{spec.linkage(c, n.library, p), std::string{}};
}

class solver
{
public:
    solver(graph_spec const& spec, recipe_provider& provider, std::size_t step_limit)
        : spec_{spec}, provider_{provider}, step_limit_{step_limit}
    {
    }

    // Throws fatal_error.
    std::variant<state, failure_ptr> run()
    {
        state s;
        if (auto c = propagate(s)) return std::make_shared<failure const>(failure{*c, {}, {}});
        return solve(std::move(s));
    }

    // "apotocki/openssl 3.5.4", "openssl 3.5.4" for local recipes
    static std::string display(node const& n)
    {
        candidate const& c = (*n.candidates)[static_cast<std::size_t>(n.chosen)];
        return name_of(n, c) + " " + c.recipe->version->str();
    }

private:
    static std::string name_of(node const& n, candidate const& c)
    {
        return c.kind == locked_package::recipe_kind::index ? c.id.str() : n.library;
    }

    std::variant<state, failure_ptr> solve(state s)
    {
        if (++steps_ > step_limit_) {
            throw fatal_error{"dependency resolution gave up after " + std::to_string(step_limit_)
                + " choices; narrow the version constraints in otok.toml"};
        }

        // the undecided library with the fewest viable versions (ties: by name)
        node const* next = nullptr;
        std::vector<std::size_t> next_viable;
        std::vector<std::string> next_rejected;
        for (auto const& [name, n] : s.nodes) {
            if (n.chosen >= 0) continue;
            std::vector<std::string> rejected;
            auto viable = viable_versions(n, rejected);
            if (!next || viable.size() < next_viable.size()) {
                next = &n;
                next_viable = std::move(viable);
                next_rejected = std::move(rejected);
            }
        }
        if (!next) return s;

        std::string const library = next->library;
        failure f;
        f.details = next_rejected;
        for (std::size_t const k : next_viable) {
            state attempt = s;
            node& n = attempt.nodes.at(library);
            n.chosen = static_cast<int>(k);
            std::string const label = version_label(n, k);
            if (auto c = propagate(attempt)) {
                f.details.push_back(label + ": " + *c);
                continue;
            }
            auto result = solve(std::move(attempt));
            if (std::holds_alternative<state>(result)) return result;
            failure_ptr const& cause = std::get<failure_ptr>(result);
            f.details.push_back(label + ": then " + cause->headline);
            f.causes.push_back(cause);
        }

        // A forced choice, its one version failing only through a deeper one: that is the cause. Without this, a chain of
        // libraries of one version each, chosen before the one at fault, nests the explanation deeper than it is shown.
        if (next_viable.size() == 1 && next_rejected.empty() && f.causes.size() == 1 && f.details.size() == 1) return f.causes.front();

        node const& n = s.nodes.at(library);
        f.headline = versions_of(n).size() == 1
            ? in_quotes(library) + " " + n.candidates->front().recipe->version->str() + " cannot be used"
            : "no version of " + in_quotes(library) + " satisfies all requirements";
        std::vector<std::string> required;
        for (requirement const& r : n.requirements) {
            required.push_back(r.requester + ": " + (r.constraint ? r.constraint->str() : std::string{"any version"}));
        }
        if (!required.empty()) {
            std::string text = "required by:";
            for (std::string const& r : required) text += "\n" + r;
            f.details.insert(f.details.begin(), text);
        }
        // versions left aside, which a newer otok may read (index.md 3.2)
        if (auto const skipped = provider_.unreadable(library); !skipped.empty()) {
            std::string text = "versions this otok cannot read (a newer otok may):";
            for (std::string const& version : skipped) text += "\n" + version;
            f.details.push_back(std::move(text));
        }
        return std::make_shared<failure const>(std::move(f));
    }

    // The candidates of the version of candidate `k`: its variants and revisions.
    static std::vector<std::size_t> same_version(node const& n, std::size_t k)
    {
        version const& v = *(*n.candidates)[k].recipe->version;
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < n.candidates->size(); ++i) {
            if (*(*n.candidates)[i].recipe->version == v) out.push_back(i);
        }
        return out;
    }

    // The first candidate of every version.
    static std::vector<std::size_t> versions_of(node const& n)
    {
        std::vector<std::size_t> out;
        for (std::size_t k = 0; k < n.candidates->size(); ++k) {
            if (same_version(n, k).front() == k) out.push_back(k);
        }
        return out;
    }

    // "1.2.0 (r1)", without the revision when the version has several recipes
    static std::string version_label(node const& n, std::size_t k)
    {
        candidate const& c = (*n.candidates)[k];
        if (same_version(n, k).size() > 1) return c.recipe->version->str();
        return c.recipe->version->str() + " (r" + std::to_string(c.recipe->revision) + ")";
    }

    version_order const& order(std::string const& library)
    {
        auto it = orders_.find(library);
        if (it == orders_.end()) it = orders_.emplace(library, provider_.order(library)).first;
        return it->second;
    }

    // Why `c` does not meet `r`; nothing when it does.
    std::optional<std::string> rejects(std::string const& library, candidate const& c, requirement const& r)
    {
        version const& v = *c.recipe->version;
        if (r.constraint) {
            version_order const& o = order(library);
            if (auto ok = r.constraint->validate(o); !ok) {
                return "cannot be checked against '" + r.constraint->str() + "' of " + r.requester + ": " + ok.error().message;
            }
            if (!r.constraint->matches(v, o)) return "does not satisfy '" + r.constraint->str() + "' of " + r.requester;
        }
        // An explicitly chosen git or path recipe is what the project asked for, pre-release or not.
        if (c.kind == locked_package::recipe_kind::index && c.recipe->channel == release_channel::prerelease
            && !r.prerelease && !(r.constraint && r.constraint->names_exactly(v, order(library)))) {
            return "is a pre-release, which " + r.requester + " does not allow (prerelease = true)";
        }
        return std::nullopt;
    }

    static bool is_locked(candidate const& c, locked_package const* locked)
    {
        return locked && locked->version && *locked->version == *c.recipe->version && locked->kind == c.kind
            && (c.kind != locked_package::recipe_kind::index || (locked->id == c.id && locked->index == c.index));
    }

    // Viable versions (their first candidates) in the order to try them; reasons for the others.
    std::vector<std::size_t> viable_versions(node const& n, std::vector<std::string>& rejected)
    {
        std::vector<std::size_t> viable;
        for (std::size_t const k : versions_of(n)) {
            candidate const& c = (*n.candidates)[k];
            std::optional<std::string> why;
            for (std::size_t i = 0; !why && i < n.requirements.size(); ++i) why = rejects(n.library, c, n.requirements[i]);
            if (why) {
                rejected.push_back(version_label(n, k) + ": " + *why);
            } else {
                viable.push_back(k);
            }
        }

        std::vector<locked_package const*> locked;
        for (std::size_t g = 0; g < spec_.graphs.size(); ++g) {
            if (auto const* p = spec_.locked(n.library, g)) locked.push_back(p);
        }
        auto const locked_version = [&](std::size_t k) {
            return std::ranges::any_of(locked, [&](locked_package const* p) { return is_locked((*n.candidates)[k], p); });
        };
        // index.md 7.1: versions with advisories are avoided, not refused
        auto const clean = [&](std::size_t k) {
            return std::ranges::any_of(same_version(n, k), [&](std::size_t i) { return (*n.candidates)[i].advisories.empty(); });
        };
        version_order const& o = order(n.library);
        std::ranges::stable_sort(viable, [&](std::size_t a, std::size_t b) {
            if (locked_version(a) != locked_version(b)) return locked_version(a);
            if (clean(a) != clean(b)) return clean(a);
            return o.compare(*(*n.candidates)[a].recipe->version, *(*n.candidates)[b].recipe->version) == std::partial_ordering::greater;
        });
        return viable;
    }

    // The variant of the chosen version for graph `g` and the platforms it builds there (otok-lock.md 5.3), with the
    // environment it builds in: the one whose `build-on` covers the graph's environment, else one of its substitutes, the
    // first some variant covers (otok-toml.md 4.9, docs/PLAN.md decision 91); and whose `supported` holds the platforms. Of its
    // revisions the locked one, else the highest without advisories.
    std::expected<std::pair<std::size_t, build_environment>, std::string> variant_of(node const& n, std::size_t g,
        std::set<std::string> const& platforms)
    {
        resolution_graph const& graph = spec_.graphs[g];
        candidate const& any = (*n.candidates)[static_cast<std::size_t>(n.chosen)];
        auto const covering_in = [&](build_environment const& environment) {
            std::vector<std::size_t> out;
            for (std::size_t const k : same_version(n, static_cast<std::size_t>(n.chosen))) {
                auto const& build_on = (*n.candidates)[k].recipe->build_on;
                if (std::ranges::any_of(build_on, [&](build_environment const& e) { return covers(e, environment); })) out.push_back(k);
            }
            return out;
        };
        build_environment environment = graph.environment;
        std::vector<std::size_t> covering = covering_in(environment);
        if (covering.empty() && g < spec_.substitutes.size()) {
            for (build_environment const& other : spec_.substitutes[g]) {
                covering = covering_in(other);
                if (covering.empty()) continue;
                environment = other;
                break;
            }
        }
        if (covering.empty()) {
            if (any.kind != locked_package::recipe_kind::index) {
                std::vector<std::string> build_on;
                for (build_environment const& e : any.recipe->build_on) build_on.push_back(e.text());
                return std::unexpected(display(n) + " is not built in " + graph.environment.text() + " (its recipe builds in " + joined(build_on) + ")");
            }
            // with several graphs, the conflict names the graph (graph_label)
            return std::unexpected("no variant of " + name_of(n, any)
                + (spec_.graphs.size() < 2 ? " for " + in_quotes(graph.target) + " in " + graph.environment.text() : std::string{}));
        }

        locked_package const* locked = spec_.locked(n.library, g);
        // the revision of each variant
        std::map<std::string, std::size_t, std::less<>> best;
        for (std::size_t const k : covering) {
            candidate const& c = (*n.candidates)[k];
            auto const rank = [&](candidate const& x) {
                locked_package const* const l = spec_.tool ? spec_.locked_tool(n.library, x.recipe->version->str(), g) : locked;
                bool const lock = is_locked(x, l) && l->revision == x.recipe->revision
                    && (x.kind != locked_package::recipe_kind::index || l->variant == x.recipe->variant);
                return std::tuple{lock, x.advisories.empty(), x.recipe->revision};
            };
            auto [it, inserted] = best.try_emplace(c.recipe->variant, k);
            if (!inserted && rank(c) > rank((*n.candidates)[it->second])) it->second = k;
        }
        for (auto const& [variant, k] : best) {
            auto const& supported = (*n.candidates)[k].recipe->platforms;
            if (std::ranges::all_of(platforms, [&](std::string const& p) { return std::ranges::find(supported, p) != supported.end(); })) {
                return std::pair{k, environment};
            }
        }
        if (best.size() == 1) {
            auto const& supported = (*n.candidates)[best.begin()->second].recipe->platforms;
            for (std::string const& p : platforms) {
                if (std::ranges::find(supported, p) == supported.end()) {
                    return std::unexpected(display(n) + " does not support platform " + in_quotes(p) + " (it supports " + joined(supported) + ")");
                }
            }
        }
        std::string variants;
        for (auto const& [variant, k] : best) variants += "\n  " + variant + ": " + joined((*n.candidates)[k].recipe->platforms);
        return std::unexpected("no variant of " + display(n) + " in " + environment.text() + " builds " + joined(platforms) + ":" + variants);
    }

    std::shared_ptr
<std::vector<candidate> const> load(std::string const& library, recipe_ref const& ref)
    {
        std::string key = library + "\n" + std::to_string(static_cast<int>(ref.source)) + "\n" + ref.id.str() + "\n" + ref.git
            + "\n" + ref.rev + "\n" + ref.path;
        auto it = loaded_.find(key);
        if (it != loaded_.end()) return it->second;
        auto list = provider_.candidates(library, ref);
        if (!list) throw fatal_error{list.error().message};
        if (list->empty()) throw fatal_error{"no recipe versions found for " + in_quotes(library)};
        for (candidate const& c : *list) {
            if (c.recipe->library != library) {
                throw fatal_error{"the recipe chosen for " + in_quotes(library) + " builds " + in_quotes(c.recipe->library)};
            }
        }
        auto shared = std::make_shared<std::vector<candidate> const>(std::move(*list));
        loaded_.emplace(std::move(key), shared);
        return shared;
    }

    // The graph of a library of the tool resolution on the system of graph `g` (otok-toml.md 7, docs/PLAN.md decision 86): the
    // first graph of that system whose environment some candidate of its recipe builds in, else the system's first, the
    // environment of `tool-build-on`, where the variant's absence is reported.
    std::size_t tool_graph_of(node const& n, std::size_t g) const
    {
        std::string const& system = spec_.graphs[g].environment.system;
        std::optional<std::size_t> first;
        for (std::size_t h = 0; h < spec_.graphs.size(); ++h) {
            build_environment const& e = spec_.graphs[h].environment;
            if (e.system != system) continue;
            if (!first) first = h;
            bool const builds = std::ranges::any_of(*n.candidates, [&](candidate const& c) {
                return std::ranges::any_of(c.recipe->build_on, [&](build_environment const& b) { return covers(b, e); });
            });
            if (builds) return h;
        }
        return first.value_or(g);
    }

    // Adds an edge into `library` for graph `g` and configuration `c`. Returns a conflict.
    std::optional<std::string> apply(state& s, std::string const& library, incoming const& in, std::size_t g, std::size_t c,
        std::set<std::string> const& platforms, bool& changed)
    {
        auto const choice_it = spec_.choices.find(library);
        project_choice const* choice = choice_it == spec_.choices.end() ? nullptr : &choice_it->second;

        auto it = s.nodes.find(library);
        if (it == s.nodes.end()) {
            node n;
            n.library = library;
            if (in.ref) {
                // the dependency's recipe, which the project chose for the target, not another for the tool
                if (choice && !same_ref(choice->dep->recipe, *in.ref)) {
                    return in.requester + " takes programs of " + in_quotes(library) + " from its dependency, whose recipe is "
                        + ref_text(*in.ref) + ", but otok.toml chooses " + ref_text(choice->dep->recipe) + " in [tool-deps." + library + "]";
                }
                n.ref = *in.ref;
                n.ref_requester = in.requester;
            } else if (choice) {
                n.ref = choice->dep->recipe;
                n.ref_requester = "otok.toml";
            } else {
                n.ref.source = recipe_ref::kind::index;
                n.ref.id = *in.default_recipe;
                n.ref_requester = in.requester;
            }
            n.candidates = load(library, n.ref);
            n.graphs.resize(spec_.graphs.size());
            for (node_graph& ng : n.graphs) ng.configs.resize(spec_.labels.size());
            if (choice) {
                n.requirements.push_back({"otok.toml", choice->dep->version, choice->dep->prerelease});
                for (node_graph& ng : n.graphs) {
                    for (std::size_t k = 0; k < spec_.labels.size(); ++k) {
                        for (auto const& [option, r] : choice->options[k]) ng.configs[k].requests[option].push_back(r);
                    }
                }
            }
            it = s.nodes.emplace(library, std::move(n)).first;
            changed = true;
        } else if (!choice && in.default_recipe && it->second.ref.source == recipe_ref::kind::index
            && !(it->second.ref.id == *in.default_recipe)) {
            node const& n = it->second;
            return in_quotes(library) + " has no recipe chosen in otok.toml, and its dependents name different default recipes:\n  "
                + n.ref_requester + ": " + n.ref.id.str() + "\n  " + in.requester + ": " + in.default_recipe->str()
                + "\nchoose one in otok.toml: [deps." + library + "] recipe = \"...\"";
        }
        node& n = it->second;
        if (spec_.tool) g = tool_graph_of(n, g);

        if (!in.requester.empty()) {
            std::string const text = in.constraint ? in.constraint->str() : std::string{};
            bool const known = std::ranges::any_of(n.requirements, [&](requirement const& r) {
                return r.requester == in.requester && (r.constraint ? r.constraint->str() : std::string{}) == text;
            });
            if (!known) {
                n.requirements.push_back({in.requester, in.constraint ? std::optional{*in.constraint} : std::nullopt, in.prerelease});
                changed = true;
            }
        }
        node_config& nc = n.graphs[g].configs[c];
        for (std::string const& p : platforms) {
            if (nc.platforms.insert(p).second) changed = true;
        }
        if (in.requires_options) {
            for (auto const& [name, value] : *in.requires_options) {
                auto& list = nc.requests[name];
                if (std::ranges::none_of(list, [&](option_request const& r) { return r.requester == in.requester; })) {
                    list.push_back({value.value, in.requester, value.where});
                    changed = true;
                }
            }
        }
        if (in.linkage) {
            for (std::string const& p : platforms) {
                auto& list = nc.linkage_requests[p];
                if (std::ranges::none_of(list, [&](auto const& r) { return r.first == in.requester; })) {
                    list.emplace_back(in.requester, *in.linkage);
                    changed = true;
                }
            }
        }
        return std::nullopt;
    }

    // Brings the state to a fixed point: active edges of chosen packages under the current options
    // and platforms, their requests, merged options. Returns the first contradiction.
    std::optional<std::string> propagate(state& s)
    {
        for (;;) {
            bool changed = false;
            for (root const& r : spec_.roots) {
                for (std::size_t g = 0; g < r.platforms.size(); ++g) {
                    for (std::size_t c = 0; c < r.platforms[g].size(); ++c) {
                        if (r.platforms[g][c].empty()) continue;
                        if (auto conflict = apply(s, r.library, r.in, g, c, r.platforms[g][c], changed)) return conflict;
                    }
                }
            }

            std::vector<std::string> chosen;
            for (auto const& [name, n] : s.nodes) {
                if (n.chosen >= 0) chosen.push_back(name);
            }
            for (std::string const& name : chosen) {
                node& n = s.nodes.at(name);
                for (std::size_t g = 0; g < n.graphs.size(); ++g) {
                    if (auto conflict = propagate_node(s, name, g, changed)) return spec_.graph_label(g) + *conflict;
                }
            }
            for (std::string const& name : chosen) {
                node const& n = s.nodes.at(name);
                candidate const& cand = (*n.candidates)[static_cast<std::size_t>(n.chosen)];
                for (requirement const& r : n.requirements) {
                    if (auto why = rejects(n.library, cand, r)) return display(n) + " " + *why;
                }
            }
            if (!changed) {
                // the linkages once every request of the dependents has arrived: before, a library would get the
                // project's default where a dependent asks for another
                for (std::string const& name : chosen) {
                    if (auto conflict = check_linkage(s.nodes.at(name))) return conflict;
                }
                return std::nullopt;
            }
        }
    }

    std::optional<std::string> check_linkage(node const& n) const
    {
        if (spec_.tool || !spec_.linkage) return std::nullopt;
        for (std::size_t g = 0; g < n.graphs.size(); ++g) {
            node_graph const& ng = n.graphs[g];
            if (ng.chosen < 0) continue;
            recipe const& r = *(*n.candidates)[static_cast<std::size_t>(ng.chosen)].recipe;
            for (std::size_t c = 0; c < ng.configs.size(); ++c) {
                std::string const label = spec_.labels[c] + spec_.graph_label(g);
                for (std::string const& p : ng.configs[c].platforms) {
                    auto const linkage = node_linkage(spec_, n, g, c, p);
                    if (!linkage) return label + linkage.error();
                    if (std::ranges::find(r.linkage, linkage->first) == r.linkage.end()) {
                        return label + display(n) + " cannot be built as " + linkage->first + " libraries for " + in_quotes(p)
                            + (linkage->second.empty() ? std::string{} : ", which " + linkage->second + " asks for,") + " (its recipe builds "
                            + joined(r.linkage) + "); set 'linkage' in [deps." + n.library + "]";
                    }
                }
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> propagate_node(state& s, std::string const& name, std::size_t g, bool& changed)
    {
        node& n = s.nodes.at(name);
        node_graph& ng = n.graphs[g];
        if (!ng.present()) return std::nullopt;
        resolution_graph const& graph = spec_.graphs[g];

        std::set<std::string> const tool_platforms{graph.tool_platform};
        std::set<std::string> all_platforms;
        for (node_config const& nc : ng.configs) all_platforms.insert(nc.platforms.begin(), nc.platforms.end());
        auto const variant = variant_of(n, g, spec_.tool ? tool_platforms : all_platforms);
        if (!variant) return variant.error();
        ng.chosen = static_cast<int>(variant->first);
        ng.environment = variant->second;
        candidate const& cand = (*n.candidates)[variant->first];
        recipe const& r = *cand.recipe;
        std::string const who = display(n);

        for (std::size_t c = 0; c < ng.configs.size(); ++c) {
            std::string const& label = spec_.labels[c];
            node_config& nc = ng.configs[c];
            if (nc.platforms.empty()) continue;

            for (auto const& [option, requests] : nc.requests) {
                if (!requests.empty() && !r.find_option(option)) {
                    return label + requests.front().requester + " sets option " + in_quotes(option) + " of " + who + ", which has no such option";
                }
            }
            merged_options merged;
            merged_options initial;  // the project's values and the defaults, before any request of a dependent
            for (option_schema const& o : r.options) {
                std::vector<option_request> own;
                std::set<std::string, std::less<>> project;  // the project's requesters: otok.toml, its profiles
                if (auto choice = spec_.choices.find(n.library); choice != spec_.choices.end()) {
                    for (auto const& [option, req] : choice->second.options[c]) {
                        if (option != o.name) continue;
                        project.insert(req.requester);
                        if (auto value = check_option_value(o, req.value)) own.push_back({std::move(*value), req.requester, req.where});
                    }
                }
                if (auto value = merge_option(n.library, o, own)) initial.emplace(o.name, std::move(*value));

                std::vector<option_request> canonical;
                if (auto it = nc.requests.find(o.name); it != nc.requests.end()) {
                    for (option_request const& req : it->second) {
                        if (o.type == option_type::file && !project.contains(req.requester)) {
                            return label + req.requester + " sets option " + in_quotes(o.name) + " of " + who
                                + ", a file option, which only the project sets (recipe-toml.md 8.3)";
                        }
                        auto value = check_option_value(o, req.value);
                        if (!value) return label + req.requester + " sets " + who + ": " + value.error().message;
                        canonical.push_back({std::move(*value), req.requester, req.where});
                    }
                }
                auto value = merge_option(n.library, o, canonical);
                if (!value) return label + value.error().message;
                merged.emplace(o.name, std::move(*value));
            }
            nc.merged = merged;

            std::set<std::string> const& platforms = spec_.tool ? tool_platforms : nc.platforms;
            for (std::string const& p : platforms) {
                for (language const l : languages) {
                    if (!spec_.standard || !r.standards[l]) continue;
                    std::optional<int> const standard = spec_.standard(c, n.library, p, l);
                    if (standard && !r.standards[l]->allows(*standard)) {
                        return label + who + " cannot be built with " + std::string{language_name(l)} + std::to_string(*standard) + " for " + in_quotes(p)
                            + " (its recipe builds with '" + r.standards[l]->str() + "'); the project sets it in ["
                            + (spec_.tool ? "tool-deps." + n.library : spec_.target_of_platform.at(p)) + "]";
                    }
                }
                if (spec_.tool) continue;
                auto const limit = r.architectures.find(p);
                auto const used = spec_.architectures[c].find(p);
                if (limit == r.architectures.end() || used == spec_.architectures[c].end()) continue;
                for (std::string const& a : used->second) {
                    if (std::ranges::find(limit->second, a) == limit->second.end()) {
                        return label + who + " builds only " + joined(limit->second) + " for " + in_quotes(p) + ", but the project builds "
                            + joined(used->second) + "; restrict them with [" + spec_.target_of_platform.at(p) + "] architectures";
                    }
                }
            }

            // Edges: the recipe's dependencies. Its tool dependencies are graphs of their own (otok-lock.md 5.7).
            for (std::size_t i = 0; i < r.dependencies.size(); ++i) {
                recipe_dependency const& e = r.dependencies[i];
                for (std::string const& p : platforms) {
                    std::string const& os = spec_.tool ? graph.target : spec_.os_of_platform.at(p);
                    bool const holds = !e.when || condition_holds(*e.when, merged, os, p);
                    // Requests may switch dependencies on, never off; otherwise the result would depend
                    // on the order in which requests arrive (otok-lock.md 5.5).
                    if (!holds && condition_holds(*e.when, initial, os, p)) {
                        std::string const& option = *tested_option(*e.when);  // only options differ between the two
                        std::string requesters;
                        for (option_request const& req : nc.requests.at(option)) {
                            if (req.requester != "otok.toml" && !req.requester.starts_with("profile ")) requesters += "\n  " + req.requester + " asks for " + format_option_value(req.value);
                        }
                        return label + "option " + in_quotes(option) + " of " + who + " becomes "
                            + format_option_value(merged.at(option)) + ", which switches off its dependency on " + in_quotes(e.library)
                            + "; requests may switch dependencies on, never off:" + requesters;
                    }
                    std::pair<std::size_t, std::string> const key{i, p};
                    if (!holds) {
                        if (nc.active.contains(key)) {
                            std::string message = label + who + ": its dependency on " + in_quotes(e.library) + " for " + in_quotes(p)
                                + " became active and was then switched off by option values requested through the graph";
                            if (std::string const* option = tested_option(*e.when)) {
                                message += " (option " + in_quotes(*option) + " is now " + format_option_value(merged.at(*option)) + ")";
                            }
                            return message;
                        }
                        continue;
                    }
                    if (nc.active.insert(key).second) changed = true;
                    incoming const in{who, &e.version, e.prerelease, &e.default_recipe, &e.requires_options, e.linkage ? &*e.linkage : nullptr};
                    if (auto conflict = apply(s, e.library, in, g, c, {p}, changed)) return label.empty() ? conflict : std::optional{label + *conflict};
                }
            }
        }
        return std::nullopt;
    }

    graph_spec const& spec_;
    recipe_provider& provider_;
    std::size_t step_limit_;
    std::size_t steps_ = 0;
    std::map<std::string, version_order, std::less<>> orders_;
    std::map<std::string, std::shared_ptr<std::vector<candidate> const>, std::less<>> loaded_;
};

// Project option values for one configuration: the base values, overridden option by option by the
// profile (otok-toml.md 8).
std::vector<std::pair<std::string, option_request>> project_requests(option_values const& base,
    option_values const* overrides, std::string const& profile)
{
    std::map<std::string, option_request, std::less<>> values;
    for (auto const& [name, v] : base) values[name] = {v.value, "otok.toml", v.where};
    if (overrides) {
        for (auto const& [name, v] : *overrides) values[name] = {v.value, "profile " + in_quotes(profile), v.where};
    }
    return {values.begin(), values.end()};
}

resolved_package make_package(node const& n, std::size_t g, graph_spec const& spec, std::size_t config_count)
{
    node_graph const& ng = n.graphs[g];
    candidate const& c = (*n.candidates)[static_cast<std::size_t>(ng.chosen)];
    recipe const& r = *c.recipe;
    resolved_package p;
    p.library = n.library;
    p.graph = g;
    p.target = spec.graphs[g].target;
    p.chosen = c;
    p.environment = ng.environment;
    for (requirement const& req : n.requirements) p.required_by.push_back({req.requester, req.constraint ? req.constraint->str() : std::string{}});
    std::set<std::string> deps;
    for (std::size_t k = 0; k < config_count; ++k) {
        node_config const& nc = ng.configs[k];
        package_configuration pc;
        if (!nc.platforms.empty()) {
            pc.platforms.assign(nc.platforms.begin(), nc.platforms.end());
            pc.options = nc.merged;
            pc.requests = nc.requests;
            if (!spec.tool && spec.linkage) {
                for (std::string const& platform : nc.platforms) {
                    if (auto linkage = node_linkage(spec, n, g, k, platform)) pc.linkage.emplace(platform, std::move(linkage->first));
                }
            }
        }
        for (auto const& [edge, platform] : nc.active) deps.insert(r.dependencies[edge].library);
        p.configurations.push_back(std::move(pc));
    }
    p.dependencies.assign(deps.begin(), deps.end());
    return p;
}

// A dependency cycle as "a -> b -> a", or nothing, in the packages of some graphs: one target graph, or the tool graphs of a
// system (docs/PLAN.md decision 86), whose packages name their dependencies by id (otok-lock.md 5.7). Packages are built in
// dependency order, so a cycle cannot be built. Cycles through tool dependencies are found as they are resolved.
std::optional<std::string> find_cycle(std::vector<resolved_package> const& packages, std::set<std::size_t> const& graphs, bool tool)
{
    std::map<std::string, resolved_package const*, std::less<>> by_name;
    for (resolved_package const& p : packages) {
        if (graphs.contains(p.graph)) by_name[tool ? p.id : p.library] = &p;
    }
    std::map<std::string, int, std::less<>> mark;  // 1: on the current path, 2: done
    std::vector<std::string> path;
    std::optional<std::string> cycle;
    auto visit = [&](auto& self, std::string const& library) -> void {
        if (cycle || mark[library] == 2) return;
        if (mark[library] == 1) {
            std::string text;
            for (auto it = std::ranges::find(path, library); it != path.end(); ++it) text += *it + " -> ";
            cycle = text + library;
            return;
        }
        mark[library] = 1;
        path.push_back(library);
        auto const it = by_name.find(library);
        if (it != by_name.end()) {
            for (std::string const& d : it->second->dependencies) self(self, d);
        }
        path.pop_back();
        mark[library] = 2;
    };
    for (auto const& [library, p] : by_name) visit(visit, library);
    return cycle;
}

// Runs one resolution; reports its failure. Empty on failure.
std::optional<state> run_solver(graph_spec const& spec, recipe_provider& provider, std::size_t step_limit,
    std::string_view what, diagnostics& diags)
{
    try {
        solver s{spec, provider, step_limit};
        auto result = s.run();
        if (auto* st = std::get_if<state>(&result)) return std::move(*st);
        diags.error({}, "cannot resolve " + std::string{what} + ": " + render(*std::get<failure_ptr>(result), 0));
    } catch (fatal_error const& e) {
        diags.error({}, e.message);
    }
    return std::nullopt;
}

// A tool dependency to resolve (otok-lock.md 5.7): an edge of a package, or a root of the project's [tool-deps].
struct tool_request
{
    std::string requester;                    // the package, "apotocki/gn 2175"; empty for a root of the project
    std::string library;                      // empty for a toolchain: the library of the recipe it names (recipe-toml.md 13.1)
    std::vector<std::string> programs;        // the toolchains it gives, which its recipe must build
    version_constraint const* constraint = nullptr;
    bool prerelease = false;
    recipe_id const* default_recipe = nullptr;
    option_values const* requires_options = nullptr;
    std::optional<recipe_ref> ref;            // programs of a dependency (recipe-toml.md 13.2): its recipe
    std::optional<version_constraint> exact;  // and its version
    std::vector<std::vector<std::set<std::string>>> platforms;  // per tool graph and configuration
    std::vector<std::string> preferred;       // the tool references the lock has for the requester
};

// Whether the dependency of `r` on `library` is active in a configuration of a package.
bool active_dependency(recipe const& r, node_config const& nc, std::string const& library)
{
    return std::ranges::any_of(nc.active, [&](auto const& a) { return r.dependencies[a.first].library == library; });
}

// "= <version>": the version a node chose.
version_constraint exact_version(node const& n)
{
    return *version_constraint::parse("= " + (*n.candidates)[static_cast<std::size_t>(n.chosen)].recipe->version->str());
}

// Two tool dependencies of one package on one library (recipe-toml.md 13.1), or nothing.
std::optional<std::string> twice_used(std::vector<std::string> const& edges)
{
    std::set<std::string_view> libraries;
    for (std::string const& id : edges) {
        if (!id.empty() && !libraries.insert(split_tool_reference(id).first).second) return std::string{split_tool_reference(id).first};
    }
    return std::nullopt;
}

// Each tool dependency as a graph of its own, rooted at the edge (otok-lock.md 5.7), in the tool graphs of `base`; the tool
// packages of all of them, one of each library and version in a graph.
class tool_resolver
{
public:
    tool_resolver(graph_spec const& base, recipe_provider& provider, std::size_t step_limit, diagnostics& diags)
        : base_{base}, provider_{provider}, step_limit_{step_limit}, diags_{diags}
    {
    }

    // A request for an edge of a recipe, active nowhere yet.
    tool_request request_of(recipe_dependency const& e, std::string requester) const
    {
        tool_request req;
        req.requester = std::move(requester);
        req.library = e.library;
        req.programs = e.programs;
        req.constraint = &e.version;
        req.prerelease = e.prerelease;
        req.default_recipe = &e.default_recipe;
        req.requires_options = &e.requires_options;
        req.platforms.assign(base_.graphs.size(), std::vector<std::set<std::string>>(base_.labels.size()));
        return req;
    }

    // The id of the tool package at the root of the request's graph; nothing once a failure is reported.
    std::optional<std::string> resolve(tool_request const& req)
    {
        std::string library = req.library;
        if (library.empty()) {
            auto found = library_of(req);
            if (!found) return std::nullopt;
            library = std::move(*found);
        }
        if (auto const it = std::ranges::find(stack_, library); it != stack_.end()) {
            std::string text;
            for (auto i = it; i != stack_.end(); ++i) text += *i + " -> ";
            error("tool dependency cycle: " + text + library);
            return std::nullopt;
        }

        graph_spec spec = base_;
        spec.tool_preference = preference(req.preferred);
        std::string const key = key_of(library, req, spec.tool_preference);
        if (auto const it = memo_.find(key); it != memo_.end()) return it->second;

        incoming const in{req.requester, req.exact ? &*req.exact : req.constraint, req.prerelease, req.default_recipe, req.requires_options,
            nullptr, req.ref ? &*req.ref : nullptr};
        spec.roots.push_back({library, in, req.platforms});
        std::string const what = req.requester.empty() ? "the tool " + in_quotes(library)
                                                       : "the tool dependency of " + req.requester + " on " + in_quotes(library);
        auto const st = run_solver(spec, provider_, step_limit_, what, diags_);
        std::optional<std::string> id;
        if (st) {
            stack_.push_back(library);
            id = collect(spec, *st, library, req);
            stack_.pop_back();
        }
        memo_.emplace(key, id);
        return id;
    }

    // The tool dependencies of a package of a graph (target or tool) whose configurations are `ng`, each resolved: the id of
    // each edge of its recipe ("" where it is inactive). `active(e, c, h)`: whether edge `e` is active in configuration `c` for
    // tool graph `h`, one of `tool_graphs`.
    std::optional<std::vector<std::string>> edges(recipe const& r, std::string const& requester, node_graph const& ng,
        std::span<std::size_t const> tool_graphs, std::vector<std::string> const& preferred, state const& from,
        auto const& active)
    {
        std::vector<std::string> ids(r.tool_dependencies.size());
        bool ok = true;
        for (std::size_t i = 0; i < r.tool_dependencies.size(); ++i) {
            recipe_dependency const& e = r.tool_dependencies[i];
            tool_request req = request_of(e, requester);
            bool used = false;
            for (std::size_t const h : tool_graphs) {
                for (std::size_t c = 0; c < ng.configs.size(); ++c) {
                    if (ng.configs[c].platforms.empty() || !active(e, c, h)) continue;
                    req.platforms[h][c] = {base_.graphs[h].tool_platform};
                    used = true;
                }
            }
            if (!used) continue;
            if (e.from_dependency) {
                node const& dep = from.nodes.at(e.library);
                req.ref = dep.ref;
                req.exact = exact_version(dep);
            }
            req.preferred = preferred;
            auto id = resolve(req);
            if (!id) {
                ok = false;
                continue;
            }
            ids[i] = std::move(*id);
        }
        if (!ok) return std::nullopt;
        if (auto library = twice_used(ids)) {
            error(requester + " has two tool dependencies on " + in_quotes(*library) + ": a recipe has one on a library (recipe-toml.md 13.1)");
            return std::nullopt;
        }
        return ids;
    }

    // Every tool package, by library, version and graph.
    std::vector<resolved_package> packages() const
    {
        std::vector<resolved_package> out;
        for (auto const& [key, p] : packages_) out.push_back(p);
        std::ranges::sort(out, {}, [](resolved_package const& p) { return std::tuple{p.library, p.id, p.graph}; });
        return out;
    }

    bool uses(std::string_view library) const
    {
        return std::ranges::any_of(packages_, [&](auto const& entry) { return entry.second.library == library; });
    }

private:
    void error(std::string message) { diags_.error({}, std::move(message)); }

    // The library of the recipe a toolchain names (recipe-toml.md 13.1).
    std::optional<std::string> library_of(tool_request const& req)
    {
        recipe_ref ref;
        ref.source = recipe_ref::kind::index;
        ref.id = *req.default_recipe;
        auto list = provider_.candidates(req.programs.front(), ref);
        if (!list) {
            // a recipe the project chooses for a library of the program's name stands in for one it cannot reach
            for (std::string const& program : req.programs) {
                if (base_.choices.contains(program)) return program;
            }
            error(list.error().message);
            return std::nullopt;
        }
        if (list->empty()) {
            error("no recipe versions found for " + in_quotes(ref.id.str()) + ", the 'default-recipe' of the toolchain "
                + in_quotes(req.programs.front()) + " of " + req.requester);
            return std::nullopt;
        }
        std::string const& library = list->front().recipe->library;
        for (candidate const& c : *list) {
            if (c.recipe->library != library) {
                error("the recipe " + in_quotes(ref.id.str()) + " builds " + in_quotes(library) + " and " + in_quotes(c.recipe->library));
                return std::nullopt;
            }
        }
        return library;
    }

    // The versions the lock references in the graph of an edge: its own reference, and what those packages link.
    std::map<std::string, std::vector<std::string>, std::less<>> preference(std::vector<std::string> const& references) const
    {
        std::map<std::string, std::vector<std::string>, std::less<>> out;
        if (!base_.preferred) return out;
        std::vector<std::string> pending = references;
        std::set<std::string> seen;
        while (!pending.empty()) {
            std::string const reference = std::move(pending.back());
            pending.pop_back();
            if (!seen.insert(reference).second) continue;
            auto const [library, version] = split_tool_reference(reference);
            if (version.empty()) continue;
            auto& versions = out[std::string{library}];
            if (std::ranges::find(versions, version) == versions.end()) versions.emplace_back(version);
            if (auto const* p = base_.preferred->find_tool(reference)) pending.insert(pending.end(), p->dependencies.begin(), p->dependencies.end());
        }
        return out;
    }

    static std::string key_of(std::string const& library, tool_request const& req,
        std::map<std::string, std::vector<std::string>, std::less<>> const& preference)
    {
        std::string key = library + "\n" + (req.ref ? ref_text(*req.ref) : req.default_recipe ? req.default_recipe->str() : std::string{});
        key += "\n" + (req.exact ? req.exact->str() : req.constraint ? req.constraint->str() : std::string{}) + (req.prerelease ? " pre" : "");
        for (std::string const& p : req.programs) key += "\nprogram " + p;
        if (req.requires_options) {
            for (auto const& [name, value] : *req.requires_options) key += "\noption " + name + "=" + format_option_value(value.value);
        }
        for (std::size_t h = 0; h < req.platforms.size(); ++h) {
            for (std::size_t c = 0; c < req.platforms[h].size(); ++c) {
                if (!req.platforms[h][c].empty()) key += "\ngraph " + std::to_string(h) + " " + std::to_string(c);
            }
        }
        for (auto const& [l, versions] : preference) {
            for (std::string const& v : versions) key += "\nlocked " + l + "@" + v;
        }
        return key;
    }

    // The packages of a solved graph, their tool dependencies resolved in turn; the id of its root.
    std::optional<std::string> collect(graph_spec const& spec, state const& st, std::string const& root, tool_request const& req)
    {
        std::map<std::string, std::string, std::less<>> id_of;
        for (auto const& [name, n] : st.nodes) {
            if (n.chosen >= 0) id_of.emplace(name, tool_reference(name, (*n.candidates)[static_cast<std::size_t>(n.chosen)].recipe->version->str()));
        }
        bool ok = true;
        for (auto const& [name, n] : st.nodes) {
            for (std::size_t g = 0; g < n.graphs.size(); ++g) {
                node_graph const& ng = n.graphs[g];
                if (ng.chosen < 0) continue;
                resolved_package p = make_package(n, g, spec, spec.labels.size());
                p.id = id_of.at(name);
                for (std::string& d : p.dependencies) d = id_of.at(d);
                recipe const& r = *p.chosen.recipe;
                if (name == root) {
                    for (std::string const& program : req.programs) {
                        if (std::ranges::find(r.programs, program) != r.programs.end()) continue;
                        error(spec.graph_label(g) + req.requester + " takes the program " + in_quotes(program) + " from " + solver::display(n)
                            + ", which builds " + (r.programs.empty() ? std::string{"none"} : joined(r.programs)) + " ([tool] programs, recipe-toml.md 13)");
                        ok = false;
                    }
                }
                // its own tool dependencies, in the tool graphs of its system: rooted at the first, each library goes into its own
                resolution_graph const& graph = spec.graphs[g];
                std::size_t first = g;
                while (first > 0 && spec.graphs[first - 1].environment.system == graph.environment.system) --first;
                std::vector<std::string> preferred;
                if (spec.preferred && !spec.unlocked.contains(name)) {
                    if (auto const* locked = spec.preferred->find_tool(p.id, graph.environment.text())) preferred = locked->tool_dependencies;
                }
                std::size_t const graphs[] = {first};
                auto ids = edges(r, solver::display(n), ng, graphs, preferred, st, [&](recipe_dependency const& e, std::size_t c, std::size_t) {
                    if (e.from_dependency) return active_dependency(r, ng.configs[c], e.library);
                    return !e.when || condition_holds(*e.when, ng.configs[c].merged, graph.target, graph.tool_platform, {},
                        graph.environment.compiler.empty() ? nullptr : &graph.environment);
                });
                if (!ids) {
                    ok = false;
                    continue;
                }
                p.tool_edges = std::move(*ids);
                std::set<std::string> used;
                for (std::string const& id : p.tool_edges) {
                    if (!id.empty()) used.insert(id);
                }
                p.tool_dependencies.assign(used.begin(), used.end());
                if (!merge(std::move(p))) ok = false;
            }
        }
        if (!ok) return std::nullopt;
        return id_of.at(root);
    }

    // A tool package of a library and version is one in its graph, whichever edges reach it (otok-lock.md 5.7).
    bool merge(resolved_package p)
    {
        auto [it, fresh] = packages_.try_emplace({p.id, p.graph}, p);
        if (fresh) return true;
        resolved_package& q = it->second;
        std::string const what = in_quotes(p.library) + " " + p.chosen.recipe->version->str();
        auto const requesters = [](resolved_package const& x) {
            std::string text;
            for (package_requirement const& r : x.required_by) text += (text.empty() ? "" : ", ") + r.requester;
            return text;
        };
        recipe const& a = *q.chosen.recipe;
        recipe const& b = *p.chosen.recipe;
        if (a.revision != b.revision || a.variant != b.variant || q.chosen.kind != p.chosen.kind || !(q.chosen.id == p.chosen.id)) {
            error("the tool " + what + " is resolved to two recipes: r" + std::to_string(a.revision) + " for " + requesters(q) + ", r"
                + std::to_string(b.revision) + " for " + requesters(p));
            return false;
        }
        for (std::size_t c = 0; c < p.configurations.size(); ++c) {
            package_configuration& qc = q.configurations[c];
            package_configuration const& pc = p.configurations[c];
            if (pc.platforms.empty()) continue;
            if (qc.platforms.empty()) {
                qc = pc;
            } else if (qc.options != pc.options) {
                error(base_.labels[c] + "the tool " + what + " is needed with different options by " + requesters(q) + " and by " + requesters(p)
                    + "; one build serves both: choose the options in otok.toml, [tool-deps." + p.library + "] options");
                return false;
            }
        }
        for (package_requirement const& r : p.required_by) {
            bool const known = std::ranges::any_of(q.required_by, [&](package_requirement const& x) { return x.requester == r.requester && x.constraint == r.constraint; });
            if (!known) q.required_by.push_back(r);
        }
        auto const unite = [](std::vector<std::string>& into, std::vector<std::string> const& from) {
            std::set<std::string> all{into.begin(), into.end()};
            all.insert(from.begin(), from.end());
            into.assign(all.begin(), all.end());
        };
        unite(q.dependencies, p.dependencies);
        unite(q.tool_dependencies, p.tool_dependencies);
        for (std::size_t i = 0; i < p.tool_edges.size(); ++i) {
            if (q.tool_edges[i].empty()) q.tool_edges[i] = p.tool_edges[i];
            else if (!p.tool_edges[i].empty() && q.tool_edges[i] != p.tool_edges[i]) {
                error("the tool " + what + " uses " + in_quotes(q.tool_edges[i]) + " for " + requesters(q) + " and " + in_quotes(p.tool_edges[i])
                    + " for " + requesters(p) + "; one build serves both");
                return false;
            }
        }
        return true;
    }

    graph_spec const& base_;
    recipe_provider& provider_;
    std::size_t step_limit_;
    diagnostics& diags_;
    std::vector<std::string> stack_;  // the libraries whose graphs are being resolved
    std::map<std::string, std::optional<std::string>> memo_;
    std::map<std::pair<std::string, std::size_t>, resolved_package> packages_;
};

}

resolved_package const* resolution::find(std::string_view library, std::string_view target) const noexcept
{
    for (resolved_package const& p : packages) {
        if (p.library == library && p.target == target) return &p;
    }
    return nullptr;
}

resolved_package const* resolution::find_tool(std::string_view id_or_library) const noexcept
{
    for (resolved_package const& p : tool_packages) {
        if (p.id == id_or_library) return &p;
    }
    for (resolved_package const& p : tool_packages) {
        if (p.library == id_or_library) return &p;
    }
    return nullptr;
}

resolved_package const* resolution::find_id(std::string_view id) const noexcept
{
    for (resolved_package const& p : packages) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

resolution select_graphs(resolution const& r, std::span<std::size_t const> graphs, std::span<std::size_t const> tool_graphs)
{
    resolution out = r;
    std::erase_if(out.packages, [&](resolved_package const& p) { return std::ranges::find(graphs, p.graph) == graphs.end(); });
    std::erase_if(out.tool_packages, [&](resolved_package const& p) { return std::ranges::find(tool_graphs, p.graph) == tool_graphs.end(); });
    return out;
}

std::optional<resolution> resolve(manifest const& m, std::span<target_info const* const> targets,
    recipe_provider& provider, resolve_settings const& settings, diagnostics& diags)
{
    resolution out;
    out.configurations.push_back("");
    for (profile const& p : m.profiles) out.configurations.push_back(p.name);
    std::size_t const config_count = out.configurations.size();
    auto const profile_of = [&](std::size_t c) -> profile const* { return c == 0 ? nullptr : &m.profiles[c - 1]; };
    out.several_targets = m.targets.size() > 1;

    graph_spec target;
    for (std::size_t c = 0; c < config_count; ++c) {
        target.labels.push_back(c == 0 ? std::string{} : "in profile " + in_quotes(out.configurations[c]) + ": ");
    }
    for (target_info const* t : targets) {
        for (platform_info const& p : t->platforms) {
            target.os_of_platform[p.name] = t->name;
            target.target_of_platform[p.name] = t->name;
        }
    }
    // the systems resolved (docs/PLAN.md decision 88): of those asked for, the ones some environment runs on
    auto const asked = [&](std::string_view system) {
        return !settings.systems || std::ranges::find(*settings.systems, system) != settings.systems->end();
    };
    for (std::string_view const system : host_systems()) {
        bool const runs = std::ranges::any_of(m.targets, [&](target_settings const& ts) {
            return std::ranges::any_of(ts.build_on, [&](build_environment const& e) { return e.any_host() || e.system == system; });
        });
        if (runs && asked(system)) out.systems.emplace_back(system);
    }
    std::ranges::sort(out.systems);
    // a graph per target table and environment (otok-lock.md 2), of an environment of a system resolved
    for (target_settings const& ts : m.targets) {
        for (build_environment const& e : ts.build_on) {
            if (!e.any_host() && !asked(e.system)) continue;
            target.graphs.push_back({ts.target, e, ""});
            auto const info = std::ranges::find(targets, ts.target, &target_info::name);
            target.substitutes.push_back(info != targets.end() ? (*info)->substitutes(e) : std::vector<build_environment>{});
        }
    }

    // effective platforms and architectures per configuration and target (otok-toml.md 8, 11)
    std::vector<std::map<std::string, std::set<std::string>, std::less<>>> effective(config_count);
    target.architectures.resize(config_count);
    for (std::size_t c = 0; c < config_count; ++c) {
        profile const* prof = profile_of(c);
        for (target_settings const& ts : m.targets) {
            profile::target_overrides const* over = nullptr;
            if (prof) {
                if (auto it = prof->targets.find(ts.target); it != prof->targets.end()) over = &it->second;
            }
            std::vector<std::string> const& platforms = over && over->platforms ? *over->platforms : ts.platforms;
            target_info const* info = nullptr;
            for (target_info const* t : targets) {
                if (t->name == ts.target) info = t;
            }
            for (std::string const& p : platforms) {
                effective[c][ts.target].insert(p);
                std::vector<std::string> archs;
                if (over && over->architectures.contains(p)) {
                    archs = over->architectures.at(p);
                } else if (ts.architectures.contains(p)) {
                    archs = ts.architectures.at(p);
                } else if (info && info->find_platform(p)) {
                    archs = info->find_platform(p)->architectures;
                }
                target.architectures[c][p] = std::move(archs);
            }
        }
    }

    for (dependency const& d : m.deps) {
        project_choice choice{&d, {}};
        root r{d.library, {}, std::vector<std::vector<std::set<std::string>>>(target.graphs.size())};
        for (std::size_t c = 0; c < config_count; ++c) {
            profile const* prof = profile_of(c);
            profile::dependency_overrides const* over = nullptr;
            if (prof) {
                if (auto it = prof->deps.find(d.library); it != prof->deps.end()) over = &it->second;
            }
            choice.options.push_back(project_requests(d.options, over ? &over->options : nullptr, prof ? prof->name : ""));
            std::optional<std::vector<std::string>> const& narrowed = over && over->platforms ? over->platforms : d.platforms;
            bool built = false;
            for (std::size_t g = 0; g < target.graphs.size(); ++g) {
                std::set<std::string> platforms;
                if (auto it = effective[c].find(target.graphs[g].target); it != effective[c].end()) {
                    for (std::string const& p : it->second) {
                        if (!narrowed || std::ranges::find(*narrowed, p) != narrowed->end()) platforms.insert(p);
                    }
                }
                built = built || !platforms.empty();
                r.platforms[g].push_back(std::move(platforms));
            }
            if (!built && c > 0) {
                diags.warning(d.where, in_quotes(d.library) + " is not built in profile " + in_quotes(prof->name)
                    + ": none of its platforms is in the profile's platform set");
            }
        }
        target.choices.emplace(d.library, std::move(choice));
        target.roots.push_back(std::move(r));
    }
    target.preferred = settings.preferred;
    target.unlocked.insert(settings.unlocked.begin(), settings.unlocked.end());
    target.linkage = [&](std::size_t c, std::string const& library, std::string const& platform) {
        for (target_info const* t : targets) {
            if (t->find_platform(platform)) return linkage_of(m, profile_of(c), library, *t);
        }
        return std::string{"static"};
    };
    target.chosen_linkage = [&](std::size_t c, std::string const& library) { return chosen_linkage_of(m, profile_of(c), library); };
    target.standard = [&](std::size_t c, std::string const& library, std::string const& platform, language l) -> std::optional<int> {
        for (target_info const* t : targets) {
            if (t->find_platform(platform)) return standard_of(m, profile_of(c), library, false, *t, l);
        }
        return std::nullopt;
    };

    auto target_state = run_solver(target, provider, settings.step_limit, "the dependencies", diags);
    if (!target_state) return std::nullopt;

    // Tool graphs: for every system the environments of the project run on, built by the target of that system, one in the
    // environment of [otok] tool-build-on and one in each other environment of that system, for the libraries whose recipes
    // do not build in it (otok-toml.md 7, docs/PLAN.md decision 86); rooted at the active tool dependencies of target
    // packages (DESIGN 6.8), which go into the first.
    graph_spec tool;
    tool.tool = true;
    std::vector<std::vector<std::size_t>> tool_graphs_of(target.graphs.size());  // target graph → its tool graphs
    for (std::string_view const system : host_systems()) {
        if (!asked(system)) continue;
        auto const host = std::ranges::find(targets, system, &target_info::host_system);
        if (host == targets.end()) continue;
        bool used = false;
        for (std::size_t g = 0; g < target.graphs.size(); ++g) {
            build_environment const& e = target.graphs[g].environment;
            if (e.any_host() || e.system == system) {
                tool_graphs_of[g].push_back(tool.graphs.size());
                used = true;
            }
        }
        if (!used) continue;
        build_environment const preferred = m.tool_environment(**host);
        tool.graphs.push_back({(*host)->name, preferred, (*host)->tool_platform});
        for (build_environment const& e : (*host)->environments) {
            build_environment const other{e.system, "", e.compiler};
            if (e.system == system && other != preferred) tool.graphs.push_back({(*host)->name, other, (*host)->tool_platform});
        }
    }
    // a tool dependency's own standard; the target does not matter for it
    tool.standard = [&](std::size_t, std::string const& library, std::string const&, language l) -> std::optional<int> {
        return targets.empty() ? std::nullopt : standard_of(m, nullptr, library, true, *targets.front(), l);
    };
    tool.labels = target.labels;
    tool.os_of_platform = target.os_of_platform;
    tool.target_of_platform = target.target_of_platform;
    tool.architectures.resize(config_count);
    for (dependency const& d : m.tool_deps) {
        project_choice choice{&d, {}};
        for (std::size_t c = 0; c < config_count; ++c) {
            profile const* prof = profile_of(c);
            option_values const* over = nullptr;
            if (prof) {
                if (auto it = prof->tool_deps.find(d.library); it != prof->tool_deps.end()) over = &it->second.options;
            }
            choice.options.push_back(project_requests(d.options, over, prof ? prof->name : ""));
        }
        tool.choices.emplace(d.library, std::move(choice));
    }
    tool.preferred = settings.preferred;
    tool.unlocked = target.unlocked;

    // every tool dependency of a target package as a graph of its own (otok-lock.md 5.7), rooted in the first tool graph of
    // each system the package's graph runs on
    tool_resolver tools{tool, provider, settings.step_limit, diags};
    bool tools_ok = true;
    std::map<std::pair<std::string, std::size_t>, std::vector<std::string>> target_tool_edges;  // (library, graph) → ids
    for (auto const& [name, n] : target_state->nodes) {
        for (std::size_t g = 0; g < n.graphs.size(); ++g) {
            node_graph const& ng = n.graphs[g];
            if (ng.chosen < 0) continue;
            recipe const& r = *(*n.candidates)[static_cast<std::size_t>(ng.chosen)].recipe;
            if (r.tool_dependencies.empty()) continue;
            std::vector<std::string> preferred;
            if (target.preferred && !target.unlocked.contains(name)) {
                if (auto const* locked = target.preferred->find(name, target.graphs[g].target, target.graphs[g].environment.text())) {
                    preferred = locked->tool_dependencies;
                }
            }
            auto ids = tools.edges(r, solver::display(n), ng, tool_graphs_of[g], preferred, *target_state,
                [&](recipe_dependency const& e, std::size_t c, std::size_t h) {
                    node_config const& nc = ng.configs[c];
                    if (e.from_dependency) return active_dependency(r, nc, e.library);
                    if (!e.when) return true;
                    // the environment the slices are built in: the graph's, on the system of tool graph `h` for `*`; the
                    // architecture is not known here, so its tests hold (recipe-toml.md 9.1)
                    build_environment environment = ng.environment;
                    if (environment.any_host()) environment.system = tool.graphs[h].environment.system;
                    return condition_holds(*e.when, nc.merged, tool.graphs[h].target, tool.graphs[h].tool_platform, {},
                        environment.compiler.empty() ? nullptr : &environment);
                });
            if (!ids) {
                tools_ok = false;
                continue;
            }
            target_tool_edges[{name, g}] = std::move(*ids);
        }
    }
    // the [tool-deps] of a tool recipe otok publish builds, or those the project wants delivered (otok-toml.md 7.1)
    for (dependency const& d : m.tool_deps) {
        if (settings.tool_roots || d.deliver) {
            tool_request req;
            req.library = d.library;
            // in the first graph of each system: the library goes into its own from there
            for (std::size_t h = 0; h < tool.graphs.size(); ++h) {
                bool const first = h == 0 || tool.graphs[h - 1].environment.system != tool.graphs[h].environment.system;
                req.platforms.emplace_back(config_count, first ? std::set<std::string>{tool.graphs[h].tool_platform} : std::set<std::string>{});
            }
            if (!tools.resolve(req)) tools_ok = false;
        }
    }
    if (!tools_ok) return std::nullopt;
    for (dependency const& d : m.tool_deps) {
        if (!d.deliver && !tools.uses(d.library)) {
            diags.warning(d.where, "[tool-deps." + d.library + "] is not used: no recipe has " + in_quotes(d.library) + " as a tool dependency");
        }
    }

    out.graphs = target.graphs;
    out.tool_graphs = tool.graphs;
    for (auto const& [name, n] : target_state->nodes) {
        for (std::size_t g = 0; g < n.graphs.size(); ++g) {
            if (n.graphs[g].chosen < 0) continue;
            resolved_package p = make_package(n, g, target, config_count);
            p.id = out.several_targets ? p.library + "@" + p.target : p.library;
            if (auto it = target_tool_edges.find({name, g}); it != target_tool_edges.end()) {
                p.tool_edges = it->second;
                std::set<std::string> used;
                for (std::string const& id : p.tool_edges) {
                    if (!id.empty()) used.insert(id);
                }
                p.tool_dependencies.assign(used.begin(), used.end());
            }
            out.packages.push_back(std::move(p));
        }
    }
    out.tool_packages = tools.packages();

    // One standard of each language for every library of a target (otok-toml.md 5.1): the project's, which the solver checked
    // against the recipes, or else the lowest every recipe of the target allows, in all its graphs
    bool ok = true;
    out.standards.resize(config_count);
    for (std::size_t c = 0; c < config_count; ++c) {
        for (target_settings const& ts : m.targets) {
            auto const info = std::ranges::find(targets, ts.target, &target_info::name);
            if (info == targets.end()) continue;
            language_standards standards;
            for (language const l : languages) {
                if (auto given = standard_of(m, profile_of(c), "", false, **info, l)) {
                    standards[l] = given;
                    continue;
                }
                std::vector<resolved_package const*> constrained;
                for (resolved_package const& p : out.packages) {
                    bool const here = p.target == ts.target && !p.configurations[c].platforms.empty();
                    if (here && p.chosen.recipe->standards[l]) constrained.push_back(&p);
                }
                if (constrained.empty()) continue;
                for (int const s : known_standards(l)) {
                    if (std::ranges::all_of(constrained, [&](resolved_package const* p) { return p->chosen.recipe->standards[l]->allows(s); })) {
                        standards[l] = s;
                        break;
                    }
                }
                if (!standards[l]) {
                    std::string list;
                    std::set<std::string> listed;
                    for (resolved_package const* p : constrained) {
                        std::string const item = p->library + " '" + p->chosen.recipe->standards[l]->str() + "'";
                        if (listed.insert(item).second) list += (list.empty() ? "" : ", ") + item;
                    }
                    diags.error({}, target.labels[c] + "no " + std::string{language_name(l)} + " standard suits every library of [" + ts.target + "]: " + list);
                    ok = false;
                }
            }
            if (standards != language_standards{}) out.standards[c].emplace(ts.target, standards);
        }
    }
    // The tool packages (docs/PLAN.md decision 116): those linked together, a tool and what it links, take one standard, as the
    // libraries of a target do; a tool package's own in [tool-deps] stays its own
    {
        std::map<std::string, std::string> parent;  // id → a representative of its group
        auto const root = [&](auto& self, std::string const& id) -> std::string {
            auto const it = parent.find(id);
            if (it == parent.end() || it->second == id) return id;
            return it->second = self(self, it->second);
        };
        for (resolved_package const& p : out.tool_packages) parent.emplace(p.id, p.id);
        for (resolved_package const& p : out.tool_packages) {
            for (std::string const& d : p.dependencies) {
                resolved_package const* dp = out.find_tool(d);
                if (dp) parent[root(root, dp->id)] = root(root, p.id);
            }
        }
        std::map<std::string, std::vector<resolved_package const*>> groups;
        for (resolved_package const& p : out.tool_packages) groups[root(root, p.id)].push_back(&p);
        auto const given = [&](resolved_package const& p, language l) {
            return targets.empty() ? std::nullopt : standard_of(m, nullptr, p.library, true, *targets.front(), l);
        };
        for (auto const& [representative, members] : groups) {
            for (language const l : languages) {
                std::optional<int> standard;
                for (resolved_package const* p : members) {
                    if (auto const s = given(*p, l); s && (!standard || *s > *standard)) standard = s;
                }
                std::vector<resolved_package const*> constrained;
                for (resolved_package const* p : members) {
                    if (p->chosen.recipe->standards[l]) constrained.push_back(p);
                }
                auto const allowed = [&](int s) {
                    return std::ranges::all_of(constrained, [&](resolved_package const* p) {
                        return given(*p, l) || p->chosen.recipe->standards[l]->allows(s);
                    });
                };
                if (!standard) {
                    if (constrained.empty()) continue;
                    for (int const s : known_standards(l)) {
                        if (allowed(s)) {
                            standard = s;
                            break;
                        }
                    }
                }
                if (!standard || !allowed(*standard)) {
                    std::string list;
                    std::set<std::string> listed;
                    for (resolved_package const* p : members) {
                        std::string item = p->library;
                        if (auto const s = given(*p, l)) item += " " + std::string{language_name(l)} + std::to_string(*s) + " ([tool-deps." + p->library + "])";
                        else if (p->chosen.recipe->standards[l]) item += " '" + p->chosen.recipe->standards[l]->str() + "'";
                        else continue;
                        if (listed.insert(item).second) list += (list.empty() ? "" : ", ") + item;
                    }
                    diags.error({}, "no " + std::string{language_name(l)} + " standard suits every tool linked together: " + list
                        + "; set it in [tool-deps] of each");
                    ok = false;
                    continue;
                }
                for (resolved_package const* p : members) {
                    if (!given(*p, l)) out.tool_standards[p->id][l] = *standard;
                }
            }
        }
    }
    std::set<std::string> cycles;
    for (std::size_t g = 0; g < out.graphs.size(); ++g) {
        if (auto cycle = find_cycle(out.packages, {g}, false); cycle && cycles.insert(*cycle).second) {
            diags.error({}, "dependency cycle: " + *cycle);
            ok = false;
        }
    }
    std::map<std::string, std::set<std::size_t>> tool_graphs_of_system;
    for (std::size_t g = 0; g < out.tool_graphs.size(); ++g) tool_graphs_of_system[out.tool_graphs[g].environment.system].insert(g);
    for (auto const& [system, graphs] : tool_graphs_of_system) {
        if (auto cycle = find_cycle(out.tool_packages, graphs, true); cycle && cycles.insert(*cycle).second) {
            diags.error({}, "tool dependency cycle: " + *cycle);
            ok = false;
        }
    }
    if (!ok) return std::nullopt;
    return out;
}

std::optional<std::string> policy_refusal(license_expression const& license, license_policy const& policy)
{
    if (policy.deny.empty() && policy.allow.empty()) return std::nullopt;
    // An entry without an exception covers that license with any exception too.
    auto const covers = [](std::string const& entry, std::string_view id) {
        if (entry == id) return true;
        std::size_t const with = id.find(" WITH ");
        return with != std::string_view::npos && entry == id.substr(0, with);
    };
    auto const allowed = [&](std::string_view id) {
        if (!policy.allow.empty()) return std::ranges::any_of(policy.allow, [&](std::string const& e) { return covers(e, id); });
        return std::ranges::none_of(policy.deny, [&](std::string const& e) { return covers(e, id); });
    };
    if (license_satisfiable(license, allowed)) return std::nullopt;
    return policy.allow.empty() ? "[policy] deny-licenses forbids it" : "it is not covered by [policy] allow-licenses";
}

bool check_license_policy(resolution const& r, license_policy const& policy, diagnostics& diags)
{
    bool ok = true;
    std::set<std::string> reported;
    auto const check = [&](resolved_package const& p, char const* kind) {
        recipe const& rec = *p.chosen.recipe;
        auto const rule = policy_refusal(rec.license, policy);
        if (!rule) return;
        std::string message = std::string{kind} + in_quotes(p.library) + " " + rec.version->str() + " is licensed under '" + rec.license_text
            + "', and " + *rule;
        if (reported.insert(message).second) diags.error({}, message);
        ok = false;
    };
    for (resolved_package const& p : r.packages) check(p, "");
    for (resolved_package const& p : r.tool_packages) check(p, "tool dependency ");
    return ok;
}

}
