// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/recipe/recipe.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <tuple>

#include "otok/core/environment.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/toml_reader.hpp"

namespace otok {

namespace {

constexpr std::array<std::string_view, 15> toolchain_names = {
    "autoconf", "automake", "cmake", "gn", "go", "libtool", "make", "meson",
    "nasm", "ninja", "perl", "pkg-config", "python", "rust", "zig",
};

// Two different products of one group among `held` (recipe-toml.md 11.2), or nothing.
std::optional<std::pair<product const*, product const*>> two_of_a_group(std::span<product const* const> held)
{
    std::map<std::string_view, product const*> groups;
    for (product const* p : held) {
        if (p->alternatives.empty()) continue;
        if (auto [it, fresh] = groups.emplace(p->alternatives, p); !fresh && it->second != p) return std::pair{it->second, p};
    }
    return std::nullopt;
}

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

std::string joined(std::vector<std::string> const& names)
{
    std::string text;
    for (std::string const& n : names) {
        if (!text.empty()) text += ", ";
        text += n;
    }
    return text;
}

bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Program names (recipe-toml.md 3.1): [a-z0-9][a-z0-9_+-]{0,63}.
bool is_program_name(std::string_view s) noexcept
{
    return !s.empty() && s.size() <= 64 && (is_lower(s.front()) || is_digit(s.front()))
        && std::ranges::all_of(s, [](char c) { return is_lower(c) || is_digit(c) || c == '_' || c == '+' || c == '-'; });
}

// Lua identifiers for [vars] and resource names: [a-z][a-z0-9_]{0,63}.
bool is_variable_name(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 64 || !is_lower(s.front())) return false;
    return std::ranges::all_of(s, [](char c) { return is_lower(c) || is_digit(c) || c == '_'; });
}

// Library output names: [A-Za-z0-9_+-]{1,64}.
bool is_library_output_name(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 64) return false;
    return std::ranges::all_of(s, [](char c) {
        return is_lower(c) || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' || c == '+' || c == '-';
    });
}

bool is_keyword(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 32) return false;
    return std::ranges::all_of(s, [](char c) { return is_lower(c) || is_digit(c) || c == '-'; });
}

// `NAME` or `NAME=value`, NAME a C identifier.
bool is_define(std::string_view s) noexcept
{
    std::string_view const name = s.substr(0, s.find('='));
    if (name.empty() || is_digit(name.front())) return false;
    return std::ranges::all_of(name, [](char c) { return is_lower(c) || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_'; });
}

// https for everything published; file:// for local and test recipes (rejected by `otok publish`).
bool is_input_url(std::string_view s) noexcept
{
    return (s.starts_with("https://") && s.size() > 8) || (s.starts_with("file://") && s.size() > 7);
}

// A git repository also by ssh (recipe-toml.md 4): `ssh://[user@]host[:port]/path`, or git's short form
// `[user@]host:path`, whose `:` comes before any `/` and after more than a drive letter.
bool is_git_url(std::string_view s) noexcept
{
    if (is_input_url(s)) return true;
    if (s.starts_with("ssh://")) return s.size() > 6;
    if (s.find("://") != std::string_view::npos) return false;
    std::size_t const colon = s.find(':');
    if (colon == std::string_view::npos || colon + 1 == s.size()) return false;
    std::string_view const before = s.substr(0, colon);
    std::size_t const at = before.find('@');
    std::string_view const host = at == std::string_view::npos ? before : before.substr(at + 1);
    return host.size() > 1 && before.find('/') == std::string_view::npos
        && std::ranges::none_of(s, [](char c) { return c == ' ' || static_cast<unsigned char>(c) < 0x20; });
}

bool is_web_url(std::string_view s) noexcept
{
    return (s.starts_with("https://") && s.size() > 8) || (s.starts_with("http://") && s.size() > 7);
}

std::size_t utf8_length(std::string_view s) noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

class recipe_reader
{
public:
    // `entry`: the projection of an index entry (index.md 3.1) instead of a recipe.toml
    recipe_reader(std::span<target_info const* const> targets, recipe_file_check const& files, diagnostics& diags, recipe_names names,
        bool entry = false)
        : targets_{targets}, files_{files}, diags_{diags}, open_names_{names == recipe_names::open}, entry_{entry} {}

    recipe read(toml_table& root);

private:
    platform_info const* find_platform(std::string_view name) const;
    target_info const* target_of(std::string_view platform) const;
    bool is_target_name(std::string_view name) const;
    void check_file(toml_table& t, std::string_view key, std::string const& path, bool directory);

    void recipe_table(toml_table& root, recipe& r);
    void recipe_fields(toml_table& t, recipe& r);
    std::optional<input_location> location(toml_table& t);
    void source_table(toml_table& root, recipe& r);
    void resources(toml_table& root, recipe& r);
    void platforms_table(toml_table& root, recipe& r);
    void default_variant(toml_table& root, recipe& r);
    void options(toml_table& root, recipe& r);
    std::optional<option_schema> option_entry(toml_table& options, std::string const& name);
    void vars(toml_table& root, recipe& r);
    // What a `when` may test (recipe-toml.md 9.1): anything but the host, options only, options and the host (resources),
    // options and the target's OS (toolchains, which are checked per target), options and the target's OS without the
    // architecture and the environment (the system libraries of a product, which a target's output holds for all its slices).
    enum class tests
    {
        anything,
        options,
        options_and_host,
        options_and_os,
        options_and_target
    };
    // `when = { … }`, or with `key` its `not = { … }` (recipe-toml.md 9.1)
    std::optional<condition> when(toml_table& parent, recipe const& r, tests allowed, std::string_view key = "when");
    void tool_table(toml_table& root, recipe& r);
    void dependencies(toml_table& root, std::string_view key, bool tool, recipe& r);
    void target_tables(toml_table& root, recipe& r);
    void products(toml_table& root, recipe& r);
    void source_delivery(toml_table& root, recipe const& r);
    void toolchains(toml_table& root, recipe& r);
    void publish(toml_table& root, recipe& r);

    std::vector<std::string> slices(recipe const& r) const;
    // Open names (formats README 1.5): the tables of targets this otok does not know, keys of `t` left aside.
    void ignore_unknown_targets(toml_table& t) const;

    std::span<target_info const* const> targets_;
    recipe_file_check const& files_;
    diagnostics& diags_;
    bool open_names_;
    bool entry_;
};

platform_info const* recipe_reader::find_platform(std::string_view name) const
{
    for (target_info const* target : targets_) {
        if (platform_info const* p = target->find_platform(name)) return p;
    }
    return nullptr;
}

target_info const* recipe_reader::target_of(std::string_view platform) const
{
    for (target_info const* target : targets_) {
        if (target->find_platform(platform)) return target;
    }
    return nullptr;
}

bool recipe_reader::is_target_name(std::string_view name) const
{
    return std::ranges::any_of(targets_, [&](target_info const* t) { return t->name == name; });
}

void recipe_reader::check_file(toml_table& t, std::string_view key, std::string const& path, bool directory)
{
    if (classify_file_path(path) != file_path_kind::inside) {
        t.error(t.position(key), in_quotes(path) + " in " + t.describe(key) + " must be a path inside the recipe directory");
    } else if (files_ && !files_(path, directory)) {
        t.error(t.position(key), (directory ? "directory " : "file ") + in_quotes(path) + " named in " + t.describe(key)
            + " does not exist in the recipe directory");
    }
}

void recipe_reader::recipe_table(toml_table& root, recipe& r)
{
    auto t = root.table("recipe", need::required);
    if (!t) return;
    recipe_fields(*t, r);
    t->ignore("api");
    t->finish();
}

// The keys of [recipe], or of the top level of an index entry, which has no `name` (it is in the
// entry's `recipe`) and no keys the projection leaves out.
void recipe_reader::recipe_fields(toml_table& fields, recipe& r)
{
    toml_table* const t = &fields;
    if (auto library = t->string("library", need::required)) {
        if (!is_library_name(*library) || *library == reserved_library_name) {
            t->error(t->position("library"), in_quotes(*library) + " in " + t->describe("library")
                + " is not a valid library name (lowercase letters, digits, '_' and '-'; 'tools' is reserved)");
        }
        r.library = std::move(*library);
    }
    if (auto name = entry_ ? std::nullopt : t->string("name", need::required)) {
        if (!is_recipe_name(*name)) {
            t->error(t->position("name"), in_quotes(*name) + " in " + t->describe("name") + " is not a valid recipe name");
        }
        r.name = std::move(*name);
    }
    if (auto variant = t->string("variant")) {
        if (!is_recipe_name(*variant)) {
            t->error(t->position("variant"), in_quotes(*variant) + " in " + t->describe("variant") + " is not a valid variant name (as a recipe name)");
        }
        r.variant = std::move(*variant);
    }
    if (auto text = t->string("version", need::required)) {
        auto v = version::parse(*text);
        if (v) r.version = std::move(*v);
        else t->error(t->position("version"), v.error().message);
    }
    if (auto revision = t->integer("revision", need::required)) {
        if (*revision < 1) t->error(t->position("revision"), t->describe("revision") + " must be 1 or more");
        r.revision = *revision;
    }
    if (auto channel = t->string("channel")) {
        if (*channel == "prerelease") r.channel = release_channel::prerelease;
        else if (*channel != "stable") t->error(t->position("channel"), t->describe("channel") + " must be \"stable\" or \"prerelease\"");
    }
    if (auto license = t->string("license", need::required)) {
        auto e = parse_license_expression(*license);
        if (e) r.license = std::move(*e);
        else t->error(t->position("license"), e.error().message);
        r.license_text = std::move(*license);
    }
    if (toml::node const* files = entry_ ? nullptr : t->node("license-files", need::required)) {
        std::string const expected = t->describe("license-files") + " must be an array of paths or { recipe } tables";
        auto const* array = files->as_array();
        if (!array) t->error(t->position("license-files"), expected);
        else if (array->empty()) t->error(t->position("license-files"), t->describe("license-files") + " must not be empty");
        std::set<std::string> paths;
        for (std::size_t i = 0; array && i < array->size(); ++i) {
            toml::node const& element = (*array)[i];
            license_entry entry;
            if (auto const* s = element.as_string()) {
                entry.path = s->get();
                if (classify_file_path(entry.path) != file_path_kind::inside) {
                    t->error(t->position_of(element), in_quotes(entry.path) + " in " + t->describe("license-files") + " must be a path inside the source");
                }
            } else if (auto const* table = element.as_table()) {
                toml_table lt = t->nested(*table, "license-files[" + std::to_string(i + 1) + "]");
                entry.path = lt.string("recipe", need::required).value_or("");
                entry.in_recipe = true;
                if (!entry.path.empty()) check_file(lt, "recipe", entry.path, false);
                lt.finish();
            } else {
                t->error(t->position_of(element), expected);
                continue;
            }
            if (!entry.path.empty() && !paths.insert(entry.path).second) {
                t->error(t->position_of(element), in_quotes(entry.path) + " appears twice in " + t->describe("license-files"));
            }
            r.license_files.push_back(std::move(entry));
        }
    }
    if (auto description = t->string("description", need::required)) {
        if (description->empty() || description->find('\n') != std::string::npos || utf8_length(*description) > 200) {
            t->error(t->position("description"), t->describe("description") + " must be one non-empty line of at most 200 characters");
        }
        r.description = std::move(*description);
    }
    for (auto [key, out] : {std::pair{"homepage", &r.homepage}, std::pair{"documentation", &r.documentation}}) {
        if (auto url = entry_ ? std::nullopt : t->string(key)) {
            if (!is_web_url(*url)) t->error(t->position(key), t->describe(key) + " must be an http(s) URL");
            *out = std::move(*url);
        }
    }
    if (auto keywords = t->strings("keywords")) {
        if (keywords->size() > 10) t->error(t->position("keywords"), t->describe("keywords") + " may list at most 10 keywords");
        for (std::string const& k : *keywords) {
            if (!is_keyword(k)) t->error(t->position("keywords"), "keyword " + in_quotes(k) + " must be 1 to 32 lowercase letters, digits or '-'");
        }
        r.keywords = std::move(*keywords);
    }
    if (auto linkage = t->strings("linkage")) {
        if (linkage->empty()) t->error(t->position("linkage"), t->describe("linkage") + " must not be empty");
        for (std::size_t i = 0; i < linkage->size(); ++i) {
            std::string const& l = (*linkage)[i];
            if (l != "static" && l != "dynamic") {
                t->error(t->position("linkage"), t->describe("linkage") + " may hold \"static\" and \"dynamic\", not " + in_quotes(l));
            } else if (std::ranges::count(*linkage, l) > 1 && std::ranges::find(*linkage, l) - linkage->begin() == static_cast<std::ptrdiff_t>(i)) {
                t->error(t->position("linkage"), t->describe("linkage") + " lists " + in_quotes(l) + " twice");
            }
        }
        r.linkage = std::move(*linkage);
    }
    // the standards the library builds with, as constraints of numbers (recipe-toml.md 3): ">= 99", ">= 14, <= 20"
    for (language const l : languages) {
        std::string const key{standard_key(l)};
        auto text = t->string(key);
        if (!text) continue;
        if (auto constraint = standard_constraint::parse(*text)) {
            r.standards[l] = std::move(*constraint);
        } else {
            t->error(t->position(key), t->describe(key) + " " + constraint.error().message);
        }
    }
}

std::optional<input_location> recipe_reader::location(toml_table& t)
{
    bool const archive = t.contains("url");
    bool const git = t.contains("git");
    if (archive == git) {
        t.error(t.position(), t.path() + " needs exactly one of 'url' (with 'sha256') or 'git' (with 'commit')");
        t.ignore("url");
        t.ignore("git");
        return std::nullopt;
    }
    input_location loc;
    bool ok = true;
    if (archive) {
        loc.type = input_location::kind::archive;
        auto url = t.string("url", need::required);
        auto sha = t.string("sha256", need::required);
        if (url && !is_input_url(*url)) {
            t.error(t.position("url"), t.describe("url") + " must be an https URL");
            ok = false;
        }
        if (sha && !is_sha256_hex(*sha)) {
            t.error(t.position("sha256"), t.describe("sha256") + " must be 64 lowercase hexadecimal digits");
            ok = false;
        }
        if (auto mirrors = t.strings("mirrors")) {
            for (std::size_t i = 0; i < mirrors->size(); ++i) {
                std::string const& m = (*mirrors)[i];
                if (!is_input_url(m)) {
                    t.error(t.position("mirrors"), t.describe("mirrors") + " must hold https URLs, not " + in_quotes(m));
                    ok = false;
                } else if ((url && m == *url) || std::find(mirrors->begin(), mirrors->begin() + static_cast<std::ptrdiff_t>(i), m) != mirrors->begin() + static_cast<std::ptrdiff_t>(i)) {
                    t.error(t.position("mirrors"), t.describe("mirrors") + " lists " + in_quotes(m) + " twice");
                    ok = false;
                }
            }
            loc.mirrors = std::move(*mirrors);
        }
        if (!url || !sha) return std::nullopt;
        loc.url = std::move(*url);
        loc.sha256 = std::move(*sha);
    } else {
        loc.type = input_location::kind::git;
        auto url = t.string("git", need::required);
        auto commit = t.string("commit", need::required);
        if (url && !is_git_url(*url)) {
            t.error(t.position("git"), t.describe("git") + " must be an https or ssh URL");
            ok = false;
        }
        if (commit && !is_git_commit(*commit)) {
            t.error(t.position("commit"), t.describe("commit") + " must be a full 40-character commit SHA");
            ok = false;
        }
        if (auto tag = t.string("tag")) {
            if (!is_git_tag_name(*tag)) {
                t.error(t.position("tag"), t.describe("tag") + " is not a valid tag name: " + in_quotes(*tag));
                ok = false;
            }
            loc.tag = std::move(*tag);
        }
        loc.submodules = t.boolean("submodules").value_or(false);
        if (!url || !commit) return std::nullopt;
        loc.git = std::move(*url);
        loc.commit = std::move(*commit);
    }
    return ok ? std::optional{std::move(loc)} : std::nullopt;
}

void recipe_reader::source_table(toml_table& root, recipe& r)
{
    auto t = root.table("source", need::required);
    if (!t) return;
    if (auto loc = location(*t)) r.source.location = std::move(*loc);
    if (auto patches = entry_ ? std::nullopt : t->strings("patches")) {
        for (std::string const& p : *patches) check_file(*t, "patches", p, false);
        r.source.patches = std::move(*patches);
    }
    if (auto v = entry_ ? std::nullopt : t->table("verify-against")) {
        if (t->contains("git")) {
            t->error(t->position("verify-against"), t->describe("verify-against") + " is for archive sources; a git source needs no comparison");
        }
        verify_against va;
        if (auto git = v->string("git", need::required)) {
            if (!is_input_url(*git)) v->error(v->position("git"), v->describe("git") + " must be an https URL");
            va.git = std::move(*git);
        }
        auto tag = v->string("tag");
        auto commit = v->string("commit");
        if (tag.has_value() == commit.has_value()) {
            v->error(v->position(), v->path() + " needs exactly one of 'tag' or 'commit'");
        }
        if (commit && !is_git_commit(*commit)) v->error(v->position("commit"), v->describe("commit") + " must be a full 40-character commit SHA");
        va.tag = tag.value_or("");
        va.commit = commit.value_or("");
        v->finish();
        r.source.verify = std::move(va);
    }
    t->finish();
}

void recipe_reader::resources(toml_table& root, recipe& r)
{
    auto list = root.tables("resource");
    if (!list) return;
    std::set<std::string, std::less<>> names;
    for (toml_table& t : *list) {
        recipe_resource res;
        res.where = t.position();
        if (auto name = t.string("name", need::required)) {
            if (!is_variable_name(*name)) {
                t.error(t.position("name"), in_quotes(*name) + " in " + t.describe("name") + " must match [a-z][a-z0-9_]*");
            } else if (!names.insert(*name).second) {
                t.error(t.position("name"), "resource " + in_quotes(*name) + " is defined twice");
            }
            res.name = std::move(*name);
        }
        auto loc = location(t);
        if (auto unpack = entry_ ? std::nullopt : t.boolean("unpack")) {
            if (loc && loc->type == input_location::kind::git) t.error(t.position("unpack"), t.describe("unpack") + " applies to archive resources only");
            res.unpack = *unpack;
        }
        if (auto dest = entry_ ? std::nullopt : t.string("dest")) {
            if (classify_file_path(*dest) != file_path_kind::inside) {
                t.error(t.position("dest"), t.describe("dest") + " must be a path inside the source tree");
            }
            res.dest = std::move(*dest);
        }
        // not in the index projection (index.md 3.1): fetching reads it from the recipe tree
        if (!entry_) res.when = when(t, r, tests::options_and_host);
        t.finish();
        if (loc) {
            res.location = std::move(*loc);
            r.resources.push_back(std::move(res));
        }
    }
}

void recipe_reader::platforms_table(toml_table& root, recipe& r)
{
    auto t = root.table("platforms", need::required);
    if (!t) return;
    if (auto supported = t->strings("supported", need::required)) {
        if (supported->empty()) t->error(t->position("supported"), t->describe("supported") + " must not be empty");
        std::set<std::string, std::less<>> seen;
        for (std::string const& p : *supported) {
            if (!find_platform(p)) {
                if (open_names_) continue;  // a platform of a newer otok, never one of this one's slices
                t->error(t->position("supported"), "unknown platform " + in_quotes(p) + " in " + t->describe("supported"));
            } else if (!seen.insert(p).second) {
                t->error(t->position("supported"), "platform " + in_quotes(p) + " is listed twice in " + t->describe("supported"));
            } else {
                r.platforms.push_back(p);
            }
        }
    }
    if (auto archs = t->table("architectures")) {
        for (std::string const& platform : archs->keys()) {
            auto list = archs->strings(platform);
            platform_info const* info = find_platform(platform);
            if (!info && open_names_) continue;
            if (!info || std::ranges::find(r.platforms, platform) == r.platforms.end()) {
                archs->error(archs->position(platform), "platform " + in_quotes(platform) + " in " + archs->path() + " is not a supported platform");
                continue;
            }
            if (!list) continue;
            if (list->empty()) {
                archs->error(archs->position(platform), archs->describe(platform) + " must not be empty");
                continue;
            }
            bool ok = true;
            if (open_names_) {
                std::erase_if(*list, [&](std::string const& a) { return std::ranges::find(info->architectures, a) == info->architectures.end(); });
                if (list->empty()) {
                    // architectures of a newer otok only: no slice of this one
                    std::erase(r.platforms, platform);
                    continue;
                }
            }
            for (std::string const& a : *list) {
                if (std::ranges::find(info->architectures, a) == info->architectures.end()) {
                    archs->error(archs->position(platform), "architecture " + in_quotes(a) + " is not available for platform " + in_quotes(platform));
                    ok = false;
                }
            }
            if (ok) r.architectures.emplace(platform, std::move(*list));
        }
        archs->finish();
    }
    // the environments of the targets of `supported`, each of those with one (recipe-toml.md 6, 6.1); entries of the index
    // published before docs/PLAN.md decision 82 have none
    if (auto build_on = t->strings("build-on", entry_ ? need::optional : need::required)) {
        if (build_on->empty()) t->error(t->position("build-on"), t->describe("build-on") + " must not be empty");
        std::vector<target_info const*> targets;
        for (std::string const& p : r.platforms) {
            if (target_info const* target = target_of(p); target && std::ranges::find(targets, target) == targets.end()) targets.push_back(target);
        }
        std::set<target_info const*> served;
        for (std::string const& text : *build_on) {
            auto e = parse_environment(text);
            if (!e) {
                t->error(t->position("build-on"), in_quotes(text) + " in " + t->describe("build-on")
                    + " is not an environment (<system>/<compiler>, <system>-<arch>/<compiler> or */<compiler>)");
                continue;
            }
            if (std::ranges::find(r.build_on, *e) != r.build_on.end()) {
                t->error(t->position("build-on"), "environment " + in_quotes(text) + " is listed twice in " + t->describe("build-on"));
                continue;
            }
            bool known = false;
            for (target_info const* target : targets) {
                if (target->has_environment(*e)) {
                    known = true;
                    served.insert(target);
                }
            }
            if (!known && open_names_) continue;  // an environment of a newer otok: not built in here
            if (!known) {
                std::string names;
                for (target_info const* target : targets) names += (names.empty() ? "'" : ", '") + target->name + "'";
                t->error(t->position("build-on"), e->any_host()
                    ? in_quotes(text) + " in " + t->describe("build-on") + ": '*' is for a compiler that builds alike on every host, which "
                        + in_quotes(e->compiler) + " is not for " + names
                    : "environment " + in_quotes(text) + " in " + t->describe("build-on") + " is not one of the target " + names);
                continue;
            }
            r.build_on.push_back(std::move(*e));
        }
        for (target_info const* target : targets) {
            if (!build_on->empty() && !served.contains(target) && !open_names_) {
                t->error(t->position("build-on"), t->describe("build-on") + " names no environment of the target '" + target->name + "'");
            }
        }
    }
    t->finish();
}

// `[recipe] variant`, by default the target of the platforms; required when they are of several (recipe-toml.md 3, 6.1).
void recipe_reader::default_variant(toml_table& root, recipe& r)
{
    std::vector<std::string> targets;
    for (std::string const& p : r.platforms) {
        if (target_info const* target = target_of(p); target && std::ranges::find(targets, target->name) == targets.end()) targets.push_back(target->name);
    }
    if (!r.variant.empty() || targets.empty()) return;
    if (targets.size() == 1) {
        r.variant = targets.front();
        return;
    }
    root.error(root.position("platforms"), "a recipe whose platforms are of several targets (" + joined(targets) + ") names its 'variant' in [recipe]");
}

std::optional<option_schema> recipe_reader::option_entry(toml_table& options, std::string const& name)
{
    auto t = options.table(name);
    if (!t) return std::nullopt;
    option_schema o;
    o.name = name;
    o.where = t->position();
    bool ok = true;
    if (!is_option_name(name)) {
        options.error(options.position(name), "invalid option name " + in_quotes(name)
            + "; option names are lowercase letters, digits and '-', starting with a letter");
        ok = false;
    }

    auto type = t->string("type", need::required);
    if (!type) {
        t->ignore("default");
        t->ignore("values");
        t->ignore("additive");
        t->ignore("doc");
        t->finish();
        return std::nullopt;
    }
    if (*type == "bool") o.type = option_type::boolean;
    else if (*type == "int") o.type = option_type::integer;
    else if (*type == "string") o.type = option_type::string;
    else if (*type == "enum") o.type = option_type::enumeration;
    else if (*type == "list") o.type = option_type::list;
    else if (*type == "file") o.type = option_type::file;
    else {
        t->error(t->position("type"), t->describe("type") + " must be \"bool\", \"int\", \"string\", \"enum\", \"list\" or \"file\", not " + in_quotes(*type));
        ok = false;
    }

    if (auto values = t->strings("values")) {
        if (o.type != option_type::enumeration && o.type != option_type::list) {
            t->error(t->position("values"), t->describe("values") + " applies to enum and list options only");
            ok = false;
        }
        std::set<std::string, std::less<>> seen;
        for (std::string const& v : *values) {
            if (!seen.insert(v).second) {
                t->error(t->position("values"), "value " + in_quotes(v) + " is listed twice in " + t->describe("values"));
                ok = false;
            }
        }
        o.values = std::move(*values);
    }
    if (o.type == option_type::enumeration && o.values.empty()) {
        t->error(t->position(), "enum option " + in_quotes(name) + " needs a non-empty 'values' list");
        ok = false;
    }

    if (o.type == option_type::file) {
        // recipe-toml.md 8.3: only the project gives the file
        if (t->contains("default")) {
            t->error(t->position("default"), "a file option takes no " + t->describe("default") + ": without a file from the project it has none");
            t->ignore("default");
            ok = false;
        }
        o.default_value = std::string{};
    } else if (toml::node const* def = t->node("default", need::required)) {
        auto data = to_option_data(*def);
        bool const type_ok = data && [&] {
            switch (o.type) {
            case option_type::boolean: return std::holds_alternative<bool>(*data);
            case option_type::integer: return std::holds_alternative<std::int64_t>(*data);
            case option_type::string:
            case option_type::enumeration: return std::holds_alternative<std::string>(*data);
            case option_type::list: return std::holds_alternative<std::vector<std::string>>(*data);
            case option_type::file: return false;
            }
            return false;
        }();
        if (!type_ok) {
            t->error(t->position("default"), t->describe("default") + " does not match the option type " + in_quotes(*type));
            ok = false;
        } else {
            auto const allowed = [&](std::string const& v) { return o.values.empty() || std::ranges::find(o.values, v) != o.values.end(); };
            if (o.type == option_type::enumeration && !allowed(std::get<std::string>(*data))) {
                t->error(t->position("default"), t->describe("default") + " is not one of 'values'");
                ok = false;
            }
            if (o.type == option_type::list) {
                for (std::string const& v : std::get<std::vector<std::string>>(*data)) {
                    if (!allowed(v)) {
                        t->error(t->position("default"), "default element " + in_quotes(v) + " of option " + in_quotes(name) + " is not one of 'values'");
                        ok = false;
                    }
                }
            }
            o.default_value = std::move(*data);
        }
    } else {
        ok = false;
    }

    if (auto doc = entry_ ? std::nullopt : t->string("doc", need::required)) {
        if (doc->empty()) t->error(t->position("doc"), t->describe("doc") + " must not be empty");
        o.doc = std::move(*doc);
    } else if (!entry_) {
        ok = false;
    }

    if (auto additive = t->boolean("additive")) {
        if (*additive && o.type != option_type::boolean && o.type != option_type::list) {
            t->error(t->position("additive"), "only bool and list options can be additive");
            ok = false;
        }
        o.additive = *additive;
    }
    t->finish();
    return ok ? std::optional{std::move(o)} : std::nullopt;
}

void recipe_reader::options(toml_table& root, recipe& r)
{
    auto t = root.table("options");
    if (!t) return;
    for (std::string const& name : t->keys()) {
        if (auto o = option_entry(*t, name)) r.options.push_back(std::move(*o));
    }
    t->finish();
}

void recipe_reader::ignore_unknown_targets(toml_table& t) const
{
    if (!open_names_) return;
    for (std::string const& key : t.unread_keys()) {
        // a new key is a new API (formats README 1.4): an unknown table named as a target is a target of a newer otok
        toml::node const* node = t.raw().get(key);
        if (node && node->is_table() && is_library_name(key) && !is_target_name(key)) t.ignore(key);
    }
}

std::vector<std::string> recipe_reader::slices(recipe const& r) const
{
    std::vector<std::string> result;
    for (std::string const& p : r.platforms) {
        platform_info const* info = find_platform(p);
        if (!info) continue;
        auto it = r.architectures.find(p);
        for (std::string const& a : it != r.architectures.end() ? it->second : info->architectures) {
            result.push_back(p + "-" + a);
        }
    }
    return result;
}

void recipe_reader::vars(toml_table& root, recipe& r)
{
    auto t = root.table("vars");
    if (!t) return;
    for (std::string const& name : t->keys()) {
        auto values = t->table(name);
        if (!values) continue;
        recipe_variable var;
        var.name = name;
        var.where = values->position();
        if (!is_variable_name(name)) {
            t->error(t->position(name), "invalid variable name " + in_quotes(name) + "; variable names match [a-z][a-z0-9_]*");
        }
        std::optional<std::size_t> type;
        for (std::string const& selector : values->keys()) {
            toml::node const* node = values->node(selector);
            source_position where = values->position(selector);

            bool valid = selector == "default" || is_target_name(selector) || find_platform(selector);
            if (!valid) {
                std::size_t const dash = selector.find('-');
                if (dash != std::string::npos) {
                    platform_info const* info = find_platform(selector.substr(0, dash));
                    valid = info && std::ranges::find(info->architectures, selector.substr(dash + 1)) != info->architectures.end();
                }
            }
            if (!valid && open_names_) continue;  // a platform or slice of a newer otok, never one of this one's
            if (!valid) {
                values->error(where, "unknown selector " + in_quotes(selector) + " in " + values->path()
                    + "; selectors are a slice (\"iossim-arm64\"), a platform, a target OS or \"default\"");
                continue;
            }
            auto data = to_option_data(*node);
            if (!data) {
                values->error(where, values->describe(selector) + " must be a string, an integer, a boolean or an array of strings");
                continue;
            }
            if (type && *type != data->index()) {
                values->error(where, values->describe(selector) + " is " + describe(*data) + ", but other values of variable "
                    + in_quotes(name) + " have another type");
                continue;
            }
            type = data->index();
            var.values.emplace(selector, std::move(*data));
        }
        values->finish();

        std::vector<std::string> missing;
        for (std::string const& slice : slices(r)) {
            std::string const platform = slice.substr(0, slice.find('-'));
            target_info const* target = target_of(platform);
            bool const covered = var.values.contains(slice) || var.values.contains(platform)
                || (target && var.values.contains(target->name)) || var.values.contains("default");
            if (!covered) missing.push_back(slice);
        }
        if (!missing.empty()) {
            t->error(var.where, "variable " + in_quotes(name) + " has no value for " + joined(missing) + "; add those selectors or \"default\"");
        }
        r.vars.push_back(std::move(var));
    }
    t->finish();
}

std::optional<condition> recipe_reader::when(toml_table& parent, recipe const& r, tests allowed, std::string_view key)
{
    auto t = parent.table(key);
    if (!t) return std::nullopt;
    condition c;
    c.where = t->position();
    bool ok = true;

    c.option = t->string("option");
    c.has = t->string("has");
    if (toml::node const* is = t->node("is")) {
        auto data = to_option_data(*is);
        if (!data || (!std::holds_alternative<std::string>(*data) && !std::holds_alternative<std::int64_t>(*data))) {
            t->error(t->position("is"), t->describe("is") + " must be a string or an integer");
            ok = false;
        } else {
            c.is = std::move(*data);
        }
    }
    if (t->contains("host")) {
        if (allowed != tests::options_and_host) {
            t->error(t->position("host"), t->describe("host") + ": only the 'when' of a [[resource]] may test the build machine");
            t->ignore("host");
            ok = false;
        } else {
            c.host = t->strings("host");
            for (std::string const& h : c.host.value_or(std::vector<std::string>{})) {
                if (!is_host_machine(h)) {
                    t->error(t->position("host"), in_quotes(h) + " in " + t->describe("host")
                        + " is not a build machine: <os>-<arch> of windows, macos, linux and x86_64, arm64, a Linux one on musl linux-<arch>-musl");
                    ok = false;
                }
            }
        }
    }
    if (t->contains("arch") || t->contains("environment")) {
        // what the slice is built for and in: the edges of no graph depend on them (recipe-toml.md 9.1)
        if (allowed != tests::options_and_host && allowed != tests::options_and_os) {
            t->error(t->position(), t->path() + (allowed == tests::options_and_target ? " may only test options and 'os' here"
                                                     : ": only the 'when' of a [[resource]] or of [toolchains] may test 'arch' and 'environment'"));
            t->ignore("arch");
            t->ignore("environment");
            ok = false;
        } else if (key == "not") {
            // the resolver takes them as holding, which a negation would turn into never
            t->error(t->position(), t->path() + ": 'arch' and 'environment' cannot be tested under 'not'; list those that hold");
            t->ignore("arch");
            t->ignore("environment");
            ok = false;
        } else {
            c.arch = t->strings("arch");
            for (std::string const& a : c.arch.value_or(std::vector<std::string>{})) {
                bool const known = std::ranges::any_of(targets_, [&](target_info const* target) {
                    return std::ranges::any_of(target->platforms, [&](platform_info const& p) { return std::ranges::find(p.architectures, a) != p.architectures.end(); });
                });
                if (!known && !open_names_) {
                    t->error(t->position("arch"), "unknown architecture " + in_quotes(a) + " in " + t->describe("arch"));
                    ok = false;
                }
            }
            if (auto texts = t->strings("environment")) {
                c.environment.emplace();
                for (std::string const& text : *texts) {
                    auto env = parse_environment(text);
                    if (!env) {
                        t->error(t->position("environment"), in_quotes(text) + " in " + t->describe("environment")
                            + " is not a build environment <host>/<compiler> (\"windows/msvc\", \"*/ndk\")");
                        ok = false;
                    } else {
                        c.environment->push_back(std::move(*env));
                    }
                }
            }
        }
    }
    if (t->contains("os") || t->contains("platform")) {
        bool const os_allowed = allowed == tests::anything || allowed == tests::options_and_os || allowed == tests::options_and_target;
        if (!os_allowed || (t->contains("platform") && allowed != tests::anything)) {
            t->error(t->position(), t->path() + (allowed == tests::options_and_host ? " may only test options, 'host', 'arch' and 'environment' here"
                                                 : allowed == tests::options_and_os ? " may only test options, 'os', 'arch' and 'environment' here"
                                                 : allowed == tests::options_and_target ? " may only test options and 'os' here"
                                                                                    : " may only test options here"));
            t->ignore("os");
            t->ignore("platform");
            ok = false;
        } else {
            c.os = t->strings("os");
            c.platform = t->strings("platform");
            for (std::string const& os : c.os.value_or(std::vector<std::string>{})) {
                // later targets (android, windows) may be named before this otok knows them
                if (!is_library_name(os)) {
                    t->error(t->position("os"), "invalid target OS name " + in_quotes(os));
                    ok = false;
                }
            }
            for (std::string const& p : c.platform.value_or(std::vector<std::string>{})) {
                if (!find_platform(p) && !open_names_) {
                    t->error(t->position("platform"), "unknown platform " + in_quotes(p) + " in " + t->describe("platform"));
                    ok = false;
                }
            }
        }
    }
    if (t->contains("not")) {
        if (auto negated = when(*t, r, allowed, "not")) c.negated = std::make_shared<condition const>(std::move(*negated));
        else ok = false;
    }
    t->finish();

    if ((c.has || c.is) && !c.option) {
        t->error(c.where, t->path() + ": 'has' and 'is' need 'option'");
        return std::nullopt;
    }
    if (c.has && c.is) {
        t->error(c.where, t->path() + " may have 'has' or 'is', not both");
        return std::nullopt;
    }
    if (!c.option && !c.os && !c.platform && !c.host && !c.arch && !c.environment && !c.negated) {
        if (ok) {
            t->error(c.where, t->path() + (allowed == tests::options_and_host ? " must test at least one of 'option', 'host', 'arch', 'environment' or 'not'"
                                           : allowed == tests::options_and_os ? " must test at least one of 'option', 'os', 'arch', 'environment' or 'not'"
                                           : allowed == tests::options_and_target ? " must test at least one of 'option', 'os' or 'not'"
                                                                              : " must test at least one of 'option', 'os', 'platform' or 'not'"));
        }
        return std::nullopt;
    }
    if (c.option) {
        option_schema const* o = r.find_option(*c.option);
        if (!o) {
            t->error(t->position("option"), t->path() + " tests " + in_quotes(*c.option) + ", which is not an option of this recipe");
            return std::nullopt;
        }
        if (c.has) {
            if (o->type != option_type::list) {
                t->error(t->position("has"), "'has' needs a list option, but " + in_quotes(o->name) + " is not one");
                ok = false;
            } else if (!o->values.empty() && std::ranges::find(o->values, *c.has) == o->values.end()) {
                t->error(t->position("has"), in_quotes(*c.has) + " is not one of the values of option " + in_quotes(o->name));
                ok = false;
            }
        } else if (c.is) {
            bool const want_int = o->type == option_type::integer;
            bool const type_ok = (want_int && std::holds_alternative<std::int64_t>(*c.is))
                || ((o->type == option_type::string || o->type == option_type::enumeration) && std::holds_alternative<std::string>(*c.is));
            if (!type_ok) {
                t->error(t->position("is"), "'is' does not match the type of option " + in_quotes(o->name));
                ok = false;
            } else if (o->type == option_type::enumeration && std::ranges::find(o->values, std::get<std::string>(*c.is)) == o->values.end()) {
                t->error(t->position("is"), in_quotes(std::get<std::string>(*c.is)) + " is not one of the values of option " + in_quotes(o->name));
                ok = false;
            }
        } else if (o->type != option_type::boolean && o->type != option_type::file) {
            t->error(t->position("option"), t->path() + " tests option " + in_quotes(o->name)
                + " alone, which needs a bool or file option; use 'has' for a list or 'is' for other types");
            ok = false;
        }
    }
    return ok ? std::optional{std::move(c)} : std::nullopt;
}

void recipe_reader::dependencies(toml_table& root, std::string_view key, bool tool, recipe& r)
{
    // the names otok.toml has (docs/PLAN.md decision 87)
    std::string_view const old = tool ? "tool-dependencies" : "dependencies";
    if (root.contains(old)) {
        root.error(root.position(old), "[" + std::string{old} + "] is now [" + std::string{key} + "], as in otok.toml");
        root.ignore(old);
    }
    auto t = root.table(key);
    if (!t) return;
    for (std::string const& library : t->keys()) {
        auto d = t->table(library);
        if (!d) continue;
        recipe_dependency dep;
        dep.library = library;
        dep.where = d->position();
        bool ok = true;
        if (!is_library_name(library) || library == reserved_library_name) {
            t->error(t->position(library), in_quotes(library) + " in " + t->path() + " is not a valid library name");
            ok = false;
        } else if (library == r.library) {
            t->error(t->position(library), "a recipe cannot depend on its own library " + in_quotes(library));
            ok = false;
        }
        if (auto text = d->string("version", need::required)) {
            auto constraint = version_constraint::parse(*text);
            if (constraint) dep.version = std::move(*constraint);
            else { d->error(d->position("version"), constraint.error().message); ok = false; }
        } else {
            ok = false;
        }
        if (auto id_text = d->string("default-recipe", need::required)) {
            auto id = parse_recipe_id(*id_text);
            if (id) dep.default_recipe = std::move(*id);
            else { d->error(d->position("default-recipe"), d->describe("default-recipe") + " must be a recipe id \"publisher/name\""); ok = false; }
        } else {
            ok = false;
        }
        dep.prerelease = d->boolean("prerelease").value_or(false);
        dep.requires_options = read_option_values(*d, "requires-options");
        if (auto linkage = d->string("linkage")) {
            if (tool) {
                d->error(d->position("linkage"), d->describe("linkage") + " is not for a tool dependency: tools are static");
                ok = false;
            } else if (*linkage != "static" && *linkage != "dynamic") {
                d->error(d->position("linkage"), d->describe("linkage") + " must be \"static\" or \"dynamic\", not " + in_quotes(*linkage));
                ok = false;
            } else {
                dep.linkage = std::move(*linkage);
            }
        }
        if (d->contains("when")) {
            dep.when = when(*d, r, tool ? tests::options : tests::anything);
            if (!dep.when) ok = false;
        }
        // programs of the dependency the build runs (recipe-toml.md 13.2)
        if (auto programs = d->strings("toolchains")) {
            if (tool) {
                d->error(d->position("toolchains"), d->describe("toolchains") + " is not for a tool dependency: name its programs in [deps."
                    + library + "] or [toolchains]");
                ok = false;
            }
            for (std::string const& program : *programs) {
                if (!is_program_name(program)) {
                    d->error(d->position("toolchains"), in_quotes(program) + " in " + d->describe("toolchains") + " is not a valid program name ([a-z0-9][a-z0-9_+-]*)");
                    ok = false;
                } else if (std::ranges::find(r.toolchains, program, &toolchain_requirement::name) != r.toolchains.end()) {
                    d->error(d->position("toolchains"), "the program " + in_quotes(program) + " of " + d->describe("toolchains") + " is declared twice");
                    ok = false;
                } else {
                    toolchain_requirement req;
                    req.name = program;
                    req.when = dep.when;
                    req.from = library;
                    req.where = d->position("toolchains");
                    r.toolchains.push_back(std::move(req));
                }
            }
            dep.toolchains = std::move(*programs);
        }
        d->finish();
        if (ok && !dep.toolchains.empty()) {
            recipe_dependency edge = dep;
            edge.programs = dep.toolchains;
            edge.toolchains.clear();
            edge.from_dependency = true;
            edge.requires_options.clear();
            edge.linkage.reset();
            r.tool_dependencies.push_back(std::move(edge));
        }
        if (ok) (tool ? r.tool_dependencies : r.dependencies).push_back(std::move(dep));
    }
    t->finish();
}

void recipe_reader::tool_table(toml_table& root, recipe& r)
{
    auto t = root.table("tool");
    if (!t) return;
    if (auto programs = t->strings("programs", need::required)) {
        if (programs->empty()) t->error(t->position("programs"), t->describe("programs") + " must name at least one program");
        std::set<std::string, std::less<>> seen;
        for (std::string const& program : *programs) {
            if (!is_program_name(program)) {
                t->error(t->position("programs"), in_quotes(program) + " in " + t->describe("programs") + " is not a valid program name ([a-z0-9][a-z0-9_+-]*)");
            } else if (!seen.insert(program).second) {
                t->error(t->position("programs"), "program " + in_quotes(program) + " is named twice in " + t->describe("programs"));
            }
        }
        r.programs = std::move(*programs);
    }
    t->finish();
}

void recipe_reader::products(toml_table& root, recipe& r)
{
    // a tool recipe may have none (recipe-toml.md 3.1)
    bool const tool = root.contains("tool");
    auto list = root.tables("product", tool ? need::optional : need::required);
    if (!list) return;
    if (list->empty() && !tool) root.error(root.position("product"), "a recipe needs at least one [[product]]");

    std::set<std::string, std::less<>> names;
    for (toml_table& t : *list) {
        product p;
        p.where = t.position();
        if (auto name = t.string("name", need::required)) {
            if (!is_product_name(*name)) {
                t.error(t.position("name"), in_quotes(*name) + " in " + t.describe("name") + " is not a valid product name ([A-Za-z_][A-Za-z0-9_]*)");
            } else if (!names.insert(*name).second) {
                t.error(t.position("name"), "product " + in_quotes(*name) + " is defined twice");
            }
            p.name = std::move(*name);
        }
        if (r.delivers_sources) {
            // the consumer's build makes the libraries (recipe-toml.md 12.1)
            std::vector<std::string> keys{"libraries", "headers", "depends", "defines", "link", "when", "alternatives"};
            for (target_info const* target : targets_) keys.push_back(target->name);
            for (std::string const& key : keys) {
                if (t.contains(key)) t.error(t.position(key), t.describe(key) + " is not for a recipe that delivers its source");
                t.ignore(key);
            }
            t.finish();
            r.products.push_back(std::move(p));
            continue;
        }
        if (auto libraries = t.strings("libraries", need::required)) {
            std::set<std::string, std::less<>> seen;
            for (std::string const& l : *libraries) {
                if (!is_library_output_name(l)) t.error(t.position("libraries"), "library output name " + in_quotes(l) + " must match [A-Za-z0-9_+-]+");
                else if (!seen.insert(l).second) t.error(t.position("libraries"), "library " + in_quotes(l) + " is listed twice in " + t.describe("libraries"));
            }
            p.libraries = std::move(*libraries);
        }
        // a library's name, or false (recipe-toml.md 11.1)
        if (toml::node const* headers = t.contains("headers") ? t.node("headers") : nullptr) {
            if (p.libraries.empty() && t.contains("libraries")) {
                t.error(t.position("headers"), t.describe("headers") + " is not for a product of headers only (libraries = [])");
            } else if (auto const* flag = headers->as_boolean()) {
                if (flag->get()) t.error(t.position("headers"), t.describe("headers") + " must name one of the product's libraries, or be false");
            } else if (auto const* name = headers->as_string()) {
                if (std::ranges::find(p.libraries, name->get()) == p.libraries.end()) {
                    t.error(t.position("headers"), t.describe("headers") + " must name one of the product's libraries, or be false");
                }
                p.headers = name->get();
            } else {
                t.error(t.position("headers"), t.describe("headers") + " must name one of the product's libraries, or be false");
            }
        } else if (!p.libraries.empty()) {
            p.headers = p.libraries.front();
        }
        if (auto depends = t.strings("depends")) p.depends = std::move(*depends);

        if (toml::node const* defines = t.node("defines")) {
            auto const* array = defines->as_array();
            if (!array) {
                t.error(t.position("defines"), t.describe("defines") + " must be an array of strings or { define, when } tables");
            } else {
                for (std::size_t i = 0; i < array->size(); ++i) {
                    toml::node const& element = (*array)[i];
                    product_define def;
                    if (auto const* s = element.as_string()) {
                        def.define = s->get();
                    } else if (auto const* table = element.as_table()) {
                        toml_table dt = t.nested(*table, "defines[" + std::to_string(i + 1) + "]");
                        def.define = dt.string("define", need::required).value_or("");
                        if (dt.contains("when")) def.when = when(dt, r, tests::options);
                        dt.finish();
                    } else {
                        t.error(t.position_of(element), t.describe("defines") + " must be an array of strings or { define, when } tables");
                        continue;
                    }
                    if (!def.define.empty() && !is_define(def.define)) {
                        t.error(t.position_of(element), in_quotes(def.define) + " is not a preprocessor definition (NAME or NAME=value)");
                    }
                    p.defines.push_back(std::move(def));
                }
            }
            // its headers are reached through a search path: no module would carry the definitions
            if (p.libraries.empty() && t.contains("libraries") && !p.defines.empty()) {
                t.error(t.position("defines"), t.describe("defines") + " is not for a product of headers only (libraries = [])");
            }
        }
        if (auto link = t.table("link")) {
            if (toml::node const* libraries = link->node("libraries")) {
                std::string const shape = link->describe("libraries") + " must be an array of strings or { library, when } tables";
                auto const* array = libraries->as_array();
                if (!array) link->error(link->position("libraries"), shape);
                for (std::size_t i = 0; array && i < array->size(); ++i) {
                    toml::node const& element = (*array)[i];
                    product_link entry;
                    if (auto const* s = element.as_string()) {
                        entry.library = s->get();
                    } else if (auto const* table = element.as_table()) {
                        toml_table lt = link->nested(*table, "libraries[" + std::to_string(i + 1) + "]");
                        entry.library = lt.string("library", need::required).value_or("");
                        if (lt.contains("when")) entry.when = when(lt, r, tests::options_and_target);
                        lt.finish();
                    } else {
                        link->error(link->position_of(element), shape);
                        continue;
                    }
                    // the toolchain names the C++ standard library (recipe-toml.md 11)
                    static constexpr std::string_view cxx_runtimes[]{"c++", "stdc++", "c++_shared", "c++_static", "c++abi"};
                    if (std::ranges::find(cxx_runtimes, entry.library) != std::end(cxx_runtimes)) {
                        link->error(link->position_of(element), in_quotes(entry.library) + " in " + link->describe("libraries")
                            + " is the C++ standard library, which the target names: write runtime = \"c++\" in " + t.describe("link"));
                    } else if (entry.library.empty() && element.is_string()) {
                        link->error(link->position_of(element), link->describe("libraries") + " has an empty library name");
                    }
                    if (std::ranges::find(p.link_libraries, entry.library, &product_link::library) != p.link_libraries.end()) {
                        link->error(link->position_of(element), "library " + in_quotes(entry.library) + " is listed twice in " + link->describe("libraries"));
                    }
                    p.link_libraries.push_back(std::move(entry));
                }
            }
            if (auto runtime = link->string("runtime")) {
                if (*runtime != "c++") link->error(link->position("runtime"), link->describe("runtime") + " must be \"c++\", not " + in_quotes(*runtime));
                p.runtime = std::move(*runtime);
            }
            link->finish();
        }
        if (t.contains("when")) p.when = when(t, r, tests::options);
        if (auto group = t.string("alternatives")) {
            if (!is_product_name(*group)) {
                t.error(t.position("alternatives"), in_quotes(*group) + " in " + t.describe("alternatives") + " is not a valid group name ([A-Za-z_][A-Za-z0-9_]*)");
            }
            p.alternatives = std::move(*group);
        }
        for (target_info const* target : targets_) {
            if (!target->read_product_table || !t.contains(target->name)) continue;
            if (auto tt = t.table(target->name)) {
                p.target_settings.emplace(target->name, target->read_product_table(*tt, files_));
            }
        }
        ignore_unknown_targets(t);
        t.finish();
        r.products.push_back(std::move(p));
    }

    // depends: existing products, no cycles
    for (product const& p : r.products) {
        for (std::string const& d : p.depends) {
            if (!names.contains(d)) diags_.error(p.where, "product " + in_quotes(p.name) + " depends on unknown product " + in_quotes(d));
        }
    }
    std::map<std::string, int, std::less<>> state;  // 1 visiting, 2 done
    std::function<bool(std::string const&)> visit = [&](std::string const& name) {
        int& s = state[name];
        if (s == 1) return false;
        if (s == 2) return true;
        s = 1;
        auto it = std::ranges::find(r.products, name, &product::name);
        if (it != r.products.end()) {
            for (std::string const& d : it->depends) {
                if (names.contains(d) && !visit(d)) return false;
            }
        }
        state[name] = 2;
        return true;
    };
    for (product const& p : r.products) {
        if (!visit(p.name)) {
            diags_.error(p.where, "products depend on each other in a cycle through " + in_quotes(p.name));
            break;
        }
    }

    // alternatives: a group of two products or more, never held twice (recipe-toml.md 11.2)
    std::map<std::string, std::vector<product const*>, std::less<>> groups;
    for (product const& p : r.products) {
        if (!p.alternatives.empty()) groups[p.alternatives].push_back(&p);
    }
    for (auto const& [group, members] : groups) {
        if (members.size() == 1) {
            diags_.error(members.front()->where, "'alternatives' in product " + in_quotes(members.front()->name) + " names the group " + in_quotes(group)
                + ", which no other product names");
        }
    }
    for (product const& p : r.products) {
        if (auto two = two_of_a_group(r.held_products(p))) {
            diags_.error(p.where, "product " + in_quotes(p.name) + " holds " + in_quotes(two->first->name) + " and " + in_quotes(two->second->name)
                + " through 'depends', which exclude each other (alternatives " + in_quotes(two->first->alternatives) + ")");
        }
    }
}

void recipe_reader::target_tables(toml_table& root, recipe& r)
{
    for (target_info const* target : targets_) {
        if (!target->read_recipe_table || !root.contains(target->name)) continue;
        if (auto t = root.table(target->name)) {
            std::any settings = target->read_recipe_table(*t, files_);
            if (target->delivers_sources && target->delivers_sources(settings)) r.delivers_sources = true;
            r.target_settings.emplace(target->name, std::move(settings));
        }
    }
}

// recipe-toml.md 12.1: nothing is built, so nothing a build needs.
void recipe_reader::source_delivery(toml_table& root, recipe const& r)
{
    std::string const why = " is not for a recipe that delivers its source";
    std::vector<target_info const*> delivering;
    for (target_info const* target : targets_) {
        auto const it = r.target_settings.find(target->name);
        if (it != r.target_settings.end() && target->delivers_sources && target->delivers_sources(it->second)) delivering.push_back(target);
    }
    std::string const table = delivering.empty() ? std::string{"recipe"} : delivering.front()->name;
    if (files_ && files_("build.lua", false)) {
        root.error(root.position(table), "a recipe that delivers its source has no build.lua: the consumer's build compiles it");
    }
    for (std::string_view key : {"resource", "vars", "options", "deps", "tool-deps", "toolchains", "tool"}) {
        if (root.contains(key)) root.error(root.position(key), "[" + std::string{key} + "]" + why);
    }
    if (r.linkage != std::vector<std::string>{"static"}) root.error(root.position("recipe"), "'linkage' in recipe" + why);
    for (language const l : languages) {
        if (r.standards[l]) root.error(root.position("recipe"), "'" + std::string{standard_key(l)} + "' in recipe" + why);
    }
    for (std::string const& platform : r.platforms) {
        target_info const* target = target_of(platform);
        if (target && std::ranges::find(delivering, target) == delivering.end()) {
            root.error(root.position("platforms"), "the platform " + in_quotes(platform) + " is of the target '" + target->name
                + "', which takes no source from this recipe");
        }
    }
}

void recipe_reader::toolchains(toml_table& root, recipe& r)
{
    auto t = root.table("toolchains");
    if (!t) return;
    for (std::string const& name : t->keys()) {
        toolchain_requirement req;
        req.name = name;
        req.where = t->position(name);
        toml::node const* node = t->node(name);
        bool const known = std::ranges::find(toolchain_names, name) != toolchain_names.end();
        if (!known && !is_program_name(name)) {
            t->error(req.where, in_quotes(name) + " in [toolchains] is not a valid program name ([a-z0-9][a-z0-9_+-]*)");
            continue;
        }
        if (std::ranges::find(r.toolchains, name, &toolchain_requirement::name) != r.toolchains.end()) {
            t->error(req.where, "the program " + in_quotes(name) + " of [toolchains] is in the 'toolchains' of a dependency too");
            continue;
        }
        auto const read_constraint = [&](toml_table& table, std::string_view key, std::string const& text) {
            auto c = version_constraint::parse(text);
            if (c) req.version = std::move(*c);
            else table.error(table.position(key), c.error().message);
        };
        if (auto const* s = node->as_string()) {
            read_constraint(*t, name, s->get());
        } else if (node->is_table()) {
            auto tt = t->table(name);
            if (auto version = tt->string("version")) read_constraint(*tt, "version", *version);
            if (auto targets = tt->strings("targets")) {
                if (name != "rust") tt->error(tt->position("targets"), tt->describe("targets") + " applies to the rust toolchain only");
                req.targets = std::move(*targets);
            }
            req.when = when(*tt, r, tests::options_and_os);
            // the OS of a condition, here or under `not`
            auto const tests_os = [](auto& self, condition const& c) -> bool { return c.os || (c.negated && self(self, *c.negated)); };
            if (auto id_text = tt->string("default-recipe")) {
                auto id = parse_recipe_id(*id_text);
                if (!id) {
                    tt->error(tt->position("default-recipe"), tt->describe("default-recipe") + " must be a recipe id \"publisher/name\"");
                } else if (req.when && tests_os(tests_os, *req.when)) {
                    // its tool dependency is resolved for the build machine, whose OS is always the same
                    tt->error(tt->position("default-recipe"), tt->describe("default-recipe") + ": a toolchain built by its recipe is needed "
                        + "whatever the target, so its 'when' may test options only");
                } else if (!req.targets.empty()) {
                    tt->error(tt->position("default-recipe"), tt->describe("default-recipe") + " cannot build a toolchain with 'targets'");
                } else {
                    req.default_recipe = std::move(*id);
                }
            }
            tt->finish();
        } else {
            t->error(req.where, t->describe(name) + " must be a version constraint or a table { version, targets, when, default-recipe }");
            continue;
        }
        if (!known && !req.default_recipe) {
            std::vector<std::string> names{toolchain_names.begin(), toolchain_names.end()};
            t->error(req.where, "the program " + in_quotes(name) + " of [toolchains] has no 'default-recipe' building it, and otok does not "
                "find it installed; it finds " + joined(names) + " (recipe-toml.md 13)");
            continue;
        }
        // a tool dependency on the library of the recipe it names, one for all programs of that recipe (recipe-toml.md 13.1)
        if (req.default_recipe) {
            auto edge = std::ranges::find_if(r.tool_dependencies, [&](recipe_dependency const& d) {
                return d.library.empty() && d.default_recipe == *req.default_recipe;
            });
            if (edge == r.tool_dependencies.end()) {
                recipe_dependency dep;
                if (req.version) dep.version = *req.version;
                dep.default_recipe = *req.default_recipe;
                dep.when = req.when;
                dep.where = req.where;
                r.tool_dependencies.push_back(std::move(dep));
                edge = std::prev(r.tool_dependencies.end());
            } else {
                if (req.version) {
                    std::string const both = edge->version.str().empty() ? req.version->str() : edge->version.str() + ", " + req.version->str();
                    auto c = version_constraint::parse(both);
                    if (c) edge->version = std::move(*c);
                    else t->error(req.where, c.error().message);
                }
                if (req.when.has_value() != edge->when.has_value()) {
                    t->error(req.where, "the programs of " + in_quotes(req.default_recipe->str()) + " in [toolchains] are one tool dependency, "
                        "so they need the same 'when'");
                }
            }
            edge->programs.push_back(name);
        }
        r.toolchains.push_back(std::move(req));
    }
    t->finish();
}

// The tool dependencies in an order of their own, by library, then by the recipe a toolchain names: the order of the keys in
// the file is lost in the projection of an index entry, which toml++ writes sorted, and the resolver's tool edges of a package
// follow the dependencies of the recipe it read, which the builder reads from recipe.toml (resolved_package::tool_edges).
void canonical_tool_dependencies(recipe& r)
{
    std::ranges::stable_sort(r.tool_dependencies, {}, [](recipe_dependency const& d) { return std::tuple{d.library, d.default_recipe.str()}; });
}

void recipe_reader::publish(toml_table& root, recipe& r)
{
    auto t = root.table("publish");
    if (!t) return;
    if (auto checks = t->tables("check")) {
        for (toml_table& c : *checks) {
            r.publish_checks.push_back(read_option_values(c, "options"));
            for (auto const& [option, value] : r.publish_checks.back()) {
                option_schema const* o = r.find_option(option);
                if (o && o->type == option_type::file) {
                    c.error(value.where, "option " + in_quotes(option) + " is a file option, which only a project sets (recipe-toml.md 8.3)");
                }
            }
            c.finish();
        }
    }
    // recipe-toml.md 14: the target tables of the project publish builds with, checked as it reads them
    for (target_info const* target : targets_) {
        auto table = t->table(target->name);
        if (!table) continue;
        for (std::string_view key : {"platforms", "architectures", "output"}) {
            if (table->contains(key)) {
                table->error(table->position(key), table->describe(key) + " is not allowed: publication builds every supported platform and architecture");
            }
        }
        (void)table->keys();  // read when publish builds with them
        r.publish_targets.emplace(target->name, table->raw());
        table->finish();
    }
    ignore_unknown_targets(*t);
    t->finish();
}

recipe recipe_reader::read(toml_table& root)
{
    recipe r;
    if (entry_) {
        if (auto api = root.integer("api", need::required)) {
            r.api = *api;
            if (*api != recipe_api) {
                root.error(root.position("api"), "recipe API " + std::to_string(*api)
                    + " needs a newer otok: this one reads API " + std::to_string(recipe_api));
                (void)root.keys();
                return r;
            }
        }
        recipe_fields(root, r);
        source_table(root, r);
        resources(root, r);
        platforms_table(root, r);
        default_variant(root, r);
        options(root, r);
        dependencies(root, "deps", false, r);
        dependencies(root, "tool-deps", true, r);
        target_tables(root, r);
        tool_table(root, r);
        toolchains(root, r);
        canonical_tool_dependencies(r);
        ignore_unknown_targets(root);
        root.finish();
        return r;
    }
    if (auto t = root.table("recipe", need::required)) {
        if (auto api = t->integer("api", need::required)) {
            r.api = *api;
            if (*api != recipe_api) {
                t->error(t->position("api"), "recipe API " + std::to_string(*api)
                    + " needs a newer otok: this one reads API " + std::to_string(recipe_api));
            }
        }
        (void)t->keys();  // read in full by recipe_table below
    }
    if (r.api != recipe_api) {
        (void)root.keys();  // a recipe of another API cannot be read by the rules of this one
        return r;
    }

    recipe_table(root, r);
    source_table(root, r);
    options(root, r);  // before the `when` of resources, which tests them
    resources(root, r);
    platforms_table(root, r);
    default_variant(root, r);
    vars(root, r);
    dependencies(root, "deps", false, r);
    dependencies(root, "tool-deps", true, r);
    // before the products, whose rules depend on whether the recipe delivers its source
    target_tables(root, r);
    tool_table(root, r);
    products(root, r);
    toolchains(root, r);
    canonical_tool_dependencies(r);
    publish(root, r);
    if (r.delivers_sources) source_delivery(root, r);
    root.ignore("metadata");
    ignore_unknown_targets(root);
    root.finish();
    return r;
}

}

option_schema const* recipe::find_option(std::string_view option) const noexcept
{
    auto it = std::ranges::find(options, option, &option_schema::name);
    return it == options.end() ? nullptr : &*it;
}

std::vector<product const*> recipe::held_products(product const& p) const
{
    std::vector<product const*> held{&p};
    for (std::size_t i = 0; i < held.size(); ++i) {
        for (std::string const& d : held[i]->depends) {
            auto it = std::ranges::find(products, d, &product::name);
            if (it != products.end() && std::ranges::find(held, &*it) == held.end()) held.push_back(&*it);
        }
    }
    return held;
}

bool recipe::exclude_each_other(product const& a, product const& b) const
{
    std::vector<product const*> held = held_products(a);
    for (product const* p : held_products(b)) {
        if (std::ranges::find(held, p) == held.end()) held.push_back(p);
    }
    return two_of_a_group(held).has_value();
}

std::span<std::string_view const> known_toolchains() noexcept
{
    return toolchain_names;
}

std::optional<recipe> parse_recipe(std::string_view content, std::string const& file,
    std::span<target_info const* const> targets, recipe_file_check const& files, diagnostics& diags, recipe_names names)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    recipe r = recipe_reader{targets, files, diags, names}.read(root);
    if (diags.error_count() != errors_before) return std::nullopt;
    return r;
}

std::optional<recipe> read_recipe_projection(toml_table& root, std::span<target_info const* const> targets, diagnostics& diags,
    recipe_names names)
{
    std::size_t const errors_before = diags.error_count();
    recipe r = recipe_reader{targets, recipe_file_check{}, diags, names, true}.read(root);
    if (diags.error_count() != errors_before) return std::nullopt;
    return r;
}

std::optional<recipe> load_recipe(std::filesystem::path const& dir,
    std::span<target_info const* const> targets, diagnostics& diags, recipe_names names)
{
    std::filesystem::path const file = dir / "recipe.toml";
    std::string const display = path_to_utf8(file);
    auto content = read_file(file);
    if (!content) {
        diags.error({display}, content.error().message);
        return std::nullopt;
    }
    recipe_file_check const files = [&dir](std::string_view relative, bool directory) {
        std::error_code ec;
        std::filesystem::path const p = dir / path_from_utf8(relative);
        return directory ? std::filesystem::is_directory(p, ec) : std::filesystem::is_regular_file(p, ec);
    };
    return parse_recipe(*content, display, targets, files, diags, names);
}

}
