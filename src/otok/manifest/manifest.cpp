// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/manifest/manifest.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/environment.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/option_values.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/toml_reader.hpp"

namespace otok {

namespace {

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

// The components of an output directory, ASCII lower-cased: file systems may ignore case.
std::vector<std::string> output_components(std::string const& output)
{
    std::vector<std::string> parts;
    for (auto const& part : std::filesystem::path{output}.lexically_normal()) {
        std::string s = part.generic_string();
        if (s.empty() || s == ".") continue;
        std::ranges::transform(s, s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        parts.push_back(std::move(s));
    }
    return parts;
}

// Whether two output directories are the same or one contains the other (otok-toml.md 4).
bool outputs_overlap(std::string const& a, std::string const& b)
{
    auto const pa = output_components(a);
    auto const pb = output_components(b);
    std::size_t const n = std::min(pa.size(), pb.size());
    return std::equal(pa.begin(), pa.begin() + static_cast<std::ptrdiff_t>(n), pb.begin());
}

// "15.0", "17.4": digits and dots, never a number (otok-toml.md 4).
bool is_deployment_version(std::string_view s) noexcept
{
    bool digit_expected = true;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            digit_expected = false;
        } else if (c == '.' && !digit_expected) {
            digit_expected = true;
        } else {
            return false;
        }
    }
    return !s.empty() && !digit_expected;
}

// A license id with an optional exception: "GPL-2.0-only WITH Classpath-exception-2.0".
bool is_license_id(std::string_view s) noexcept
{
    auto const is_id = [](std::string_view id) {
        return !id.empty() && std::ranges::all_of(id, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+';
        });
    };
    std::size_t const with = s.find(" WITH ");
    if (with == std::string_view::npos) return is_id(s);
    return is_id(s.substr(0, with)) && is_id(s.substr(with + 6));
}

// `configurations` of a target table (otok-toml.md 5).
std::optional<std::vector<std::string>> read_configurations(toml_table& t, target_info const& target)
{
    auto list = t.strings("configurations");
    if (!list) return std::nullopt;
    bool ok = !list->empty();
    if (list->empty()) t.error(t.position("configurations"), t.describe("configurations") + " must not be empty");
    for (std::size_t i = 0; i < list->size(); ++i) {
        std::string const& c = (*list)[i];
        if (c != "release" && c != "debug") {
            t.error(t.position("configurations"), t.describe("configurations") + " may hold \"release\" and \"debug\", not " + in_quotes(c));
            ok = false;
        } else if (std::find(list->begin(), list->begin() + static_cast<std::ptrdiff_t>(i), c) != list->begin() + static_cast<std::ptrdiff_t>(i)) {
            t.error(t.position("configurations"), t.describe("configurations") + " lists " + in_quotes(c) + " twice");
            ok = false;
        }
    }
    if (ok && list->size() > 1 && !target.many_configurations) {
        t.error(t.position("configurations"), t.describe("configurations") + " takes one configuration: the " + target.name
            + " target cannot give a project's build a library per configuration");
        ok = false;
    }
    if (!ok) return std::nullopt;
    return list;
}

// `linkage` of a target table (`target`) or of a dependency (none) (otok-toml.md 5).
std::optional<std::string> read_linkage(toml_table& t, target_info const* target)
{
    auto text = t.string("linkage");
    if (!text) return std::nullopt;
    if (*text != "static" && *text != "dynamic") {
        t.error(t.position("linkage"), t.describe("linkage") + " must be \"static\" or \"dynamic\", not " + in_quotes(*text)
            + (*text == "shared" ? ": did you mean \"dynamic\"?" : ""));
        return std::nullopt;
    }
    if (target && std::ranges::find(target->linkages, *text) == target->linkages.end()) {
        t.error(t.position("linkage"), t.describe("linkage") + " must be " + in_quotes(target->linkages.front()) + ": the " + target->name
            + " target does not build " + *text + " libraries");
        return std::nullopt;
    }
    return text;
}

// A list of environments (common 4.2): `build-on` of a target table, `tool-build-on` of [otok]. `allowed` says whether
// an environment is one there and returns what is wrong otherwise.
std::optional<std::vector<build_environment>> read_environments(toml_table& t, std::string_view key,
    std::function<std::optional<std::string>(build_environment const&)> const& wrong)
{
    auto list = t.strings(key);
    if (!list) return std::nullopt;
    if (list->empty()) t.error(t.position(key), t.describe(key) + " must not be empty");
    std::vector<build_environment> envs;
    bool ok = !list->empty();
    for (std::string const& text : *list) {
        auto e = parse_environment(text);
        if (!e) {
            t.error(t.position(key), in_quotes(text) + " in " + t.describe(key)
                + " is not an environment (<system>/<compiler>, <system>-<arch>/<compiler> or */<compiler>)");
            ok = false;
        } else if (std::ranges::find(envs, *e) != envs.end()) {
            t.error(t.position(key), "environment " + in_quotes(text) + " is listed twice in " + t.describe(key));
            ok = false;
        } else if (auto problem = wrong(*e)) {
            t.error(t.position(key), "environment " + in_quotes(text) + " in " + t.describe(key) + " " + *problem);
            ok = false;
        } else {
            envs.push_back(std::move(*e));
        }
    }
    if (!ok) return std::nullopt;
    return envs;
}

// `c-standard` and `cxx-standard` of a target table or a tool dependency (otok-toml.md 5.1).
language_standards read_standards(toml_table& t)
{
    language_standards standards;
    for (language const l : languages) {
        std::string const key{standard_key(l)};
        auto value = t.integer(key);
        if (!value) continue;
        std::span<int const> const known = known_standards(l);
        if (std::ranges::find(known, *value) == known.end()) {
            t.error(t.position(key), t.describe(key) + " must be " + known_standards_text(l) + ", not " + std::to_string(*value));
            continue;
        }
        standards[l] = static_cast<int>(*value);
    }
    return standards;
}

// Refuses `c-standard` and `cxx-standard` in a table of a library: one standard for every library of a target, so that
// they link together (otok-toml.md 5.1). Whether it refused one.
bool refuse_standards(toml_table& t, std::string_view where)
{
    bool refused = false;
    for (language const l : languages) {
        std::string const key{standard_key(l)};
        if (!t.contains(key)) continue;
        t.error(t.position(key), t.describe(key) + " is not allowed: the libraries of a target are built with one standard, so that they link "
            + "together; set it in " + std::string{where});
        t.ignore(key);
        refused = true;
    }
    return refused;
}

// A flag that chooses the slice, which only otok gives (otok-toml.md 4.3).
bool chooses_slice(std::string_view flag)
{
    for (std::string_view exact : {"-target", "-arch", "-isysroot", "--sysroot"}) {
        if (flag == exact) return true;
    }
    for (std::string_view prefix : {"--target=", "--sysroot=", "-mtargetos=", "-ffile-prefix-map=", "-fdebug-prefix-map=", "-fmacro-prefix-map="}) {
        if (flag.starts_with(prefix)) return true;
    }
    return flag.starts_with("-m") && flag.find("-version-min=") != std::string_view::npos;
}

// `cflags`, `cxxflags` and `ldflags` of a table (otok-toml.md 4.3).
compile_flags read_flags(toml_table& t)
{
    compile_flags flags;
    for (auto [key, out] : {std::pair{"cflags", &flags.cflags}, std::pair{"cxxflags", &flags.cxxflags}, std::pair{"ldflags", &flags.ldflags}}) {
        auto list = t.strings(key);
        if (!list) continue;
        for (std::string const& flag : *list) {
            if (chooses_slice(flag)) {
                t.error(t.position(key), t.describe(key) + " contains " + in_quotes(flag) + ", which chooses the slice: otok gives it");
            }
            // the standards are settings of their own, which the helpers pass on (otok-toml.md 5.1)
            bool const standard = flag.starts_with("-std=") || flag.starts_with("/std:") || flag.starts_with("-std:");
            if (standard && std::string_view{key} != "ldflags") {
                language const l = std::string_view{key} == "cflags" ? language::c : language::cxx;
                t.error(t.position(key), t.describe(key) + " contains " + in_quotes(flag) + ": the " + std::string{language_name(l)} + " standard is set with "
                    + in_quotes(standard_key(l)));
            }
        }
        *out = std::move(*list);
    }
    return flags;
}

class manifest_reader
{
public:
    manifest_reader(std::span<target_info const* const> targets, diagnostics& diags)
        : targets_{targets}, diags_{diags} {}

    manifest read(toml_table& root, std::string const& file, fragment_reader const& read_fragment);

private:
    platform_info const* find_platform(std::string_view name) const;
    std::string known_platforms(target_info const* target) const;

    std::optional<std::vector<std::string>> platforms(toml_table& t, std::string_view key, target_info const* target, need n);
    std::map<std::string, std::vector<std::string>, std::less<>> architectures(toml_table& t, target_info const& target);
    std::map<std::string, std::string, std::less<>> deployment(toml_table& t, target_info const& target);
    std::optional<recipe_ref> recipe(toml_table& t);
    std::optional<dependency> dependency_entry(toml_table& deps, std::string const& library, bool tool);
    void dependencies(toml_table& root, std::string_view key, bool tool, manifest& m);
    target_settings target_table(toml_table& t, target_info const& target);
    std::optional<profile> profile_entry(toml_table& profiles, std::string const& name);
    void includes(manifest& m, std::string const& file, std::vector<std::string> const& paths, fragment_reader const& read_fragment);
    void cross_check(manifest& m);
    void check_deployment(manifest const& m, profile const* p);
    void indexes(toml_table& root, manifest& m);

    std::span<target_info const* const> targets_;
    diagnostics& diags_;
    std::map<std::string, std::string, std::less<>> dep_files_;       // library → defining file
    std::map<std::string, std::string, std::less<>> tool_dep_files_;
};

platform_info const* manifest_reader::find_platform(std::string_view name) const
{
    for (target_info const* target : targets_) {
        if (platform_info const* p = target->find_platform(name)) return p;
    }
    return nullptr;
}

std::string manifest_reader::known_platforms(target_info const* target) const
{
    std::vector<std::string> names;
    for (target_info const* t : targets_) {
        if (target && t != target) continue;
        for (platform_info const& p : t->platforms) names.push_back(p.name);
    }
    return joined(names);
}

std::optional<std::vector<std::string>> manifest_reader::platforms(toml_table& t, std::string_view key, target_info const* target, need n)
{
    auto list = t.strings(key, n);
    if (!list) return std::nullopt;
    bool ok = true;
    if (list->empty()) {
        t.error(t.position(key), t.describe(key) + " must not be empty");
        ok = false;
    }
    std::set<std::string, std::less<>> seen;
    for (std::string const& name : *list) {
        bool const known = target ? target->find_platform(name) != nullptr : find_platform(name) != nullptr;
        if (!known) {
            t.error(t.position(key), "unknown platform " + in_quotes(name) + " in " + t.describe(key) + "; known platforms: " + known_platforms(target));
            ok = false;
        } else if (!seen.insert(name).second) {
            t.error(t.position(key), "platform " + in_quotes(name) + " is listed twice in " + t.describe(key));
            ok = false;
        }
    }
    return ok ? list : std::nullopt;
}

std::map<std::string, std::vector<std::string>, std::less<>> manifest_reader::architectures(toml_table& t, target_info const& target)
{
    std::map<std::string, std::vector<std::string>, std::less<>> result;
    auto table = t.table("architectures");
    if (!table) return result;
    for (std::string const& platform : table->keys()) {
        platform_info const* info = target.find_platform(platform);
        auto list = table->strings(platform);
        if (!info) {
            table->error(table->position(platform), "unknown platform " + in_quotes(platform) + " in " + table->path()
                + "; known platforms: " + known_platforms(&target));
            continue;
        }
        if (!list) continue;
        if (list->empty()) {
            table->error(table->position(platform), table->describe(platform) + " must not be empty");
            continue;
        }
        bool ok = true;
        std::set<std::string, std::less<>> seen;
        for (std::string const& arch : *list) {
            if (std::ranges::find(info->architectures, arch) == info->architectures.end()) {
                table->error(table->position(platform), "architecture " + in_quotes(arch) + " is not available for platform "
                    + in_quotes(platform) + "; available: " + joined(info->architectures));
                ok = false;
            } else if (!seen.insert(arch).second) {
                table->error(table->position(platform), "architecture " + in_quotes(arch) + " is listed twice in " + table->describe(platform));
                ok = false;
            }
        }
        if (ok) result.emplace(platform, std::move(*list));
    }
    table->finish();
    return result;
}

std::map<std::string, std::string, std::less<>> manifest_reader::deployment(toml_table& t, target_info const& target)
{
    std::map<std::string, std::string, std::less<>> result;
    auto table = t.table("deployment");
    if (!table) return result;
    for (std::string const& key : table->keys()) {
        auto value = table->string(key);
        if (std::ranges::find(target.deployment_keys, key) == target.deployment_keys.end()) {
            table->error(table->position(key), "unknown deployment key " + in_quotes(key) + " in " + table->path()
                + "; known keys: " + joined(target.deployment_keys));
            continue;
        }
        if (!value) continue;
        if (!is_deployment_version(*value)) {
            table->error(table->position(key), table->describe(key) + " must be a version of digits and dots such as \"15.0\", not "
                + in_quotes(*value));
            continue;
        }
        result.emplace(key, std::move(*value));
    }
    table->finish();
    return result;
}

std::optional<recipe_ref> manifest_reader::recipe(toml_table& t)
{
    toml::node const* node = t.node("recipe", need::required);
    if (!node) return std::nullopt;
    recipe_ref ref;
    ref.where = t.position("recipe");

    if (auto const* s = node->as_string()) {
        auto id = parse_recipe_id(s->get());
        if (!id) {
            t.error(ref.where, t.describe("recipe") + " must be a recipe id \"publisher/name\" (lowercase), not " + in_quotes(s->get()));
            return std::nullopt;
        }
        ref.source = recipe_ref::kind::index;
        ref.id = std::move(*id);
        return ref;
    }
    if (!node->is_table()) {
        t.error(ref.where, t.describe("recipe") + " must be a recipe id \"publisher/name\", a table { git, rev, path } or a table { path }, not "
            + std::string{describe_type(node->type())});
        return std::nullopt;
    }

    auto table = t.table("recipe");
    bool ok = true;
    if (table->contains("git")) {
        ref.source = recipe_ref::kind::git;
        auto git = table->string("git", need::required);
        auto rev = table->string("rev", need::required);
        auto tag = table->string("tag");
        auto path = table->string("path");
        if (tag) {
            if (!is_git_tag_name(*tag)) {
                table->error(table->position("tag"), table->describe("tag") + " is not a valid tag name: " + in_quotes(*tag));
                ok = false;
            }
            ref.tag = std::move(*tag);
        }
        if (git) {
            if (git->empty()) {
                table->error(table->position("git"), table->describe("git") + " must not be empty");
                ok = false;
            }
            ref.git = std::move(*git);
        } else {
            ok = false;
        }
        if (rev && !is_git_commit(*rev)) {
            table->error(table->position("rev"), table->describe("rev") + " must be a full 40-character commit SHA, not " + in_quotes(*rev)
                + "; branches, tags and short SHAs move or are ambiguous");
            ok = false;
        } else if (rev) {
            ref.rev = std::move(*rev);
        } else {
            ok = false;
        }
        if (path) {
            if (classify_file_path(*path) != file_path_kind::inside) {
                table->error(table->position("path"), table->describe("path") + " must be a directory inside the repository, not " + in_quotes(*path));
                ok = false;
            }
            ref.path = std::move(*path);
        }
    } else {
        ref.source = recipe_ref::kind::path;
        auto path = table->string("path", need::required);
        if (path && classify_file_path(*path) == file_path_kind::invalid) {
            table->error(table->position("path"), table->describe("path") + " is not a valid directory path: " + in_quotes(*path));
            ok = false;
        } else if (path) {
            ref.path = std::move(*path);
        } else {
            ok = false;
        }
    }
    table->finish();
    return ok ? std::optional{std::move(ref)} : std::nullopt;
}

std::optional<dependency> manifest_reader::dependency_entry(toml_table& deps, std::string const& library, bool tool)
{
    auto t = deps.table(library);
    if (!t) return std::nullopt;

    bool ok = true;
    if (!is_library_name(library)) {
        deps.error(deps.position(library), "invalid library name " + in_quotes(library) + " in " + deps.path()
            + "; library names are lowercase letters, digits, '_' and '-'");
        ok = false;
    } else if (library == reserved_library_name) {
        deps.error(deps.position(library), "the library name " + in_quotes(library) + " is reserved");
        ok = false;
    }

    dependency dep;
    dep.library = library;
    dep.where = t->position();

    auto ref = recipe(*t);
    if (!ref) ok = false;

    if (auto text = t->string("version")) {
        auto constraint = version_constraint::parse(*text);
        if (constraint) {
            dep.version = std::move(*constraint);
        } else {
            t->error(t->position("version"), constraint.error().message);
            ok = false;
        }
    } else if (ref && ref->source == recipe_ref::kind::index && !t->contains("version")) {
        t->error(t->position(), "missing required key " + t->describe("version") + " (required for a recipe from the index)");
        ok = false;
    }

    dep.prerelease = t->boolean("prerelease").value_or(false);
    dep.options = read_option_values(*t, "options");
    dep.flags = read_flags(*t);
    // one standard for every library of a target, so that they link together (otok-toml.md 5.1); a tool is a program of its own
    if (tool) {
        dep.standards = read_standards(*t);
    } else if (refuse_standards(*t, "the target table")) {
        ok = false;
    }

    if (tool) {
        if (t->contains("platforms")) {
            t->error(t->position("platforms"), t->describe("platforms") + " is not allowed: a tool dependency is built for the tool slice only");
            t->ignore("platforms");
            ok = false;
        }
        if (t->contains("linkage")) {
            t->error(t->position("linkage"), t->describe("linkage") + " is not allowed: a tool dependency is always built static");
            t->ignore("linkage");
            ok = false;
        }
        if (t->contains("bundle-data")) {
            t->error(t->position("bundle-data"), t->describe("bundle-data") + " is not allowed: a tool dependency does not reach the product");
            t->ignore("bundle-data");
            ok = false;
        }
    } else {
        if (t->contains("platforms")) {
            dep.platforms = platforms(*t, "platforms", nullptr, need::optional);
            if (!dep.platforms) ok = false;
        }
        if (t->contains("linkage")) {
            dep.linkage = read_linkage(*t, nullptr);
            if (!dep.linkage) ok = false;
        }
        dep.bundle_data = t->boolean("bundle-data").value_or(false);
    }

    if (auto source = t->table("source")) {
        auto path = source->string("path", need::required);
        if (path && classify_file_path(*path) == file_path_kind::invalid) {
            source->error(source->position("path"), source->describe("path") + " is not a valid directory path: " + in_quotes(*path));
            ok = false;
        } else if (path) {
            dep.source_path = std::move(*path);
        } else {
            ok = false;
        }
        source->finish();
    }
    t->finish();

    if (!ok) return std::nullopt;
    dep.recipe = std::move(*ref);
    return dep;
}

void manifest_reader::dependencies(toml_table& root, std::string_view key, bool tool, manifest& m)
{
    auto table = root.table(key);
    if (!table) return;
    auto& files = tool ? tool_dep_files_ : dep_files_;
    auto& list = tool ? m.tool_deps : m.deps;
    for (std::string const& library : table->keys()) {
        source_position const where = table->position(library);
        if (auto it = files.find(library); it != files.end()) {
            table->error(where, "library " + in_quotes(library) + " in " + std::string{key} + " is defined both in " + it->second
                + " and in " + where.file);
            table->ignore(library);
            continue;
        }
        files.emplace(library, where.file);
        if (auto dep = dependency_entry(*table, library, tool)) {
            list.push_back(std::move(*dep));
        }
    }
    table->finish();
}

target_settings manifest_reader::target_table(toml_table& t, target_info const& target)
{
    target_settings settings;
    settings.target = target.name;
    settings.where = t.position();
    // a target of one platform has nothing to choose (otok-toml.md 4)
    if (target.platforms.size() == 1 && !t.contains("platforms")) {
        settings.platforms = {target.platforms.front().name};
    } else if (auto list = platforms(t, "platforms", &target, need::required)) {
        settings.platforms = std::move(*list);
    }
    settings.architectures = architectures(t, target);
    settings.deployment = deployment(t, target);
    if (auto output = t.string("output")) {
        if (classify_file_path(*output) != file_path_kind::inside) {
            t.error(t.position("output"), t.describe("output") + " must be a directory inside the project, not " + in_quotes(*output));
        } else {
            settings.output = std::move(*output);
        }
    }
    settings.flags = read_flags(t);
    if (auto list = read_configurations(t, target)) settings.configurations = std::move(*list);
    settings.linkage = read_linkage(t, &target);
    settings.standards = read_standards(t);
    // the target's own environments; without the key its one environment, or `*/<compiler>` (otok-toml.md 4)
    auto build_on = read_environments(t, "build-on", [&](build_environment const& e) -> std::optional<std::string> {
        if (target.has_environment(e)) return std::nullopt;
        if (e.any_host()) return "is not for this target: '*' is for a compiler that builds alike on every host";
        return "is not one of the " + target.name + " target";
    });
    if (build_on) {
        settings.build_on = std::move(*build_on);
    } else if (!t.contains("build-on")) {
        if (target.environments.size() == 1) {
            settings.build_on = target.environments;
        } else if (target.any_host_compilers.size() == 1) {
            settings.build_on = {build_environment{"", "", target.any_host_compilers.front()}};
        } else if (!target.environments.empty()) {
            std::string names;
            for (build_environment const& e : target.environments) names += (names.empty() ? "" : ", ") + e.text();
            t.error(t.position(), "[" + target.name + "] needs 'build-on': the " + target.name + " target builds in " + names
                + ", in different ways (otok-toml.md 4.9)");
        }
    }
    for (target_choice const& c : target.choices) {
        std::string value = c.values.front();
        if (auto text = t.string(c.key)) {
            if (c.check) {
                if (auto wrong = c.check(*text)) {
                    t.error(t.position(c.key), t.describe(c.key) + " " + *wrong + ", not " + in_quotes(*text));
                } else {
                    value = std::move(*text);
                }
            } else if (std::ranges::find(c.values, *text) != c.values.end()) {
                value = std::move(*text);
            } else {
                std::string allowed;
                for (std::size_t i = 0; i < c.values.size(); ++i) {
                    allowed += (i == 0 ? "" : i + 1 == c.values.size() ? " or " : ", ") + ("\"" + c.values[i] + "\"");
                }
                auto const hint = c.hints.find(*text);
                t.error(t.position(c.key), t.describe(c.key) + " must be " + allowed + ", not " + in_quotes(*text)
                    + (hint != c.hints.end() ? ": did you mean \"" + hint->second + "\"?" : ""));
            }
        }
        settings.choices.emplace(c.key, std::move(value));
    }
    t.finish();
    return settings;
}

std::optional<profile> manifest_reader::profile_entry(toml_table& profiles, std::string const& name)
{
    auto t = profiles.table(name);
    if (!t) return std::nullopt;
    if (!is_profile_name(name)) {
        profiles.error(profiles.position(name), "invalid profile name " + in_quotes(name)
            + "; profile names are lowercase letters, digits and '-', starting with a letter");
    }

    profile p;
    p.name = name;
    p.where = t->position();
    for (target_info const* target : targets_) {
        auto tt = t->table(target->name);
        if (!tt) continue;
        profile::target_overrides overrides;
        if (tt->contains("platforms")) {
            overrides.platforms = platforms(*tt, "platforms", target, need::optional);
        }
        overrides.architectures = architectures(*tt, *target);
        overrides.deployment = deployment(*tt, *target);
        overrides.flags = read_flags(*tt);
        overrides.configurations = read_configurations(*tt, *target);
        overrides.linkage = read_linkage(*tt, target);
        overrides.standards = read_standards(*tt);
        tt->finish();
        p.targets.emplace(target->name, std::move(overrides));
    }
    for (bool const tool : {false, true}) {
        auto deps = t->table(tool ? "tool-deps" : "deps");
        if (!deps) continue;
        for (std::string const& library : deps->keys()) {
            auto lt = deps->table(library);
            if (!lt) continue;
            profile::dependency_overrides overrides;
            overrides.where = lt->position();
            overrides.options = read_option_values(*lt, "options");
            overrides.flags = read_flags(*lt);
            if (!tool && lt->contains("platforms")) {
                overrides.platforms = platforms(*lt, "platforms", nullptr, need::optional);
            }
            if (!tool) overrides.linkage = read_linkage(*lt, nullptr);
            if (!tool) refuse_standards(*lt, "the profile's target table");
            lt->finish();
            (tool ? p.tool_deps : p.deps).emplace(library, std::move(overrides));
        }
        deps->finish();
    }
    t->finish();
    return p;
}

void manifest_reader::includes(manifest& m, std::string const& file, std::vector<std::string> const& paths, fragment_reader const& read_fragment)
{
    for (std::string const& path : paths) {
        auto content = read_fragment ? read_fragment(path) : std::nullopt;
        if (!content) {
            diags_.error({file}, "cannot read the included fragment " + in_quotes(path));
            continue;
        }
        auto doc = toml_document::parse(*content, path, diags_);
        if (!doc) continue;
        auto root = doc->root();
        if (root.contains("otok")) {
            root.error(root.position("otok"), "a fragment has no [otok] table: it takes the manifest's format");
            root.ignore("otok");
        }
        dependencies(root, "deps", false, m);
        dependencies(root, "tool-deps", true, m);
        root.ignore("metadata");
        root.finish();
    }
}

void manifest_reader::check_deployment(manifest const& m, profile const* p)
{
    std::string const context = p ? " in profile " + in_quotes(p->name) : std::string{};
    for (target_settings const& settings : m.targets) {
        target_info const* target = nullptr;
        for (target_info const* t : targets_) {
            if (t->name == settings.target) target = t;
        }
        if (!target) continue;

        std::vector<std::string> const* platforms = &settings.platforms;
        auto deployment = settings.deployment;
        if (p) {
            if (auto it = p->targets.find(settings.target); it != p->targets.end()) {
                if (it->second.platforms) platforms = &*it->second.platforms;
                for (auto const& [key, value] : it->second.deployment) deployment.insert_or_assign(key, value);
            }
        }
        for (std::string const& platform : *platforms) {
            platform_info const* info = target->find_platform(platform);
            if (!info || info->deployment_keys.empty()) continue;  // a platform without deployment versions
            bool const found = std::ranges::any_of(info->deployment_keys, [&](std::string const& key) { return deployment.contains(key); });
            if (!found) {
                diags_.error(p ? p->where : settings.where, "no deployment version for platform " + in_quotes(platform) + context
                    + ": set " + in_quotes(info->deployment_keys.back()) + " in [" + settings.target + "] deployment");
            }
        }
    }
}

void manifest_reader::cross_check(manifest& m)
{
    std::set<std::string, std::less<>> base_platforms;
    for (target_settings const& settings : m.targets) {
        base_platforms.insert(settings.platforms.begin(), settings.platforms.end());
    }
    auto const check_subset = [&](std::vector<std::string> const& list, source_position const& where, std::string const& what) {
        for (std::string const& platform : list) {
            if (find_platform(platform) && !base_platforms.contains(platform)) {
                diags_.error(where, what + " lists platform " + in_quotes(platform) + ", which is not among the project's platforms");
            }
        }
    };
    // a dependency's linkage on every target it is built for (otok-toml.md 5)
    auto const check_linkage = [&](dependency const& dep, std::string const& linkage, std::optional<std::vector<std::string>> const& platforms,
                                   source_position const& where, std::string const& what) {
        for (target_settings const& ts : m.targets) {
            auto const info = std::ranges::find(targets_, ts.target, &target_info::name);
            if (info == targets_.end() || std::ranges::find((*info)->linkages, linkage) != (*info)->linkages.end()) continue;
            bool const used = std::ranges::any_of(ts.platforms, [&](std::string const& pl) {
                return !platforms || std::ranges::find(*platforms, pl) != platforms->end();
            });
            if (used) {
                diags_.error(where, what + " asks for " + linkage + " libraries, which the " + ts.target + " target does not build; narrow 'platforms' of "
                    + "deps." + dep.library);
            }
        }
    };
    for (dependency const& dep : m.deps) {
        if (dep.platforms) check_subset(*dep.platforms, dep.where, "deps." + dep.library);
        if (dep.linkage) check_linkage(dep, *dep.linkage, dep.platforms, dep.where, "deps." + dep.library);
    }

    for (profile const& p : m.profiles) {
        for (auto const& [library, overrides] : p.deps) {
            if (!m.find_dep(library)) {
                diags_.error(overrides.where, "profile " + in_quotes(p.name) + " refers to library " + in_quotes(library)
                    + ", which has no [deps." + library + "] entry");
            } else {
                if (overrides.platforms) check_subset(*overrides.platforms, overrides.where, "profile." + p.name + ".deps." + library);
                if (overrides.linkage) {
                    dependency const& dep = *m.find_dep(library);
                    check_linkage(dep, *overrides.linkage, overrides.platforms ? overrides.platforms : dep.platforms, overrides.where,
                        "profile." + p.name + ".deps." + library);
                }
            }
        }
        for (auto const& [library, overrides] : p.tool_deps) {
            if (!m.find_tool_dep(library)) {
                diags_.error(overrides.where, "profile " + in_quotes(p.name) + " refers to tool library " + in_quotes(library)
                    + ", which has no [tool-deps." + library + "] entry");
            }
        }
    }

    check_deployment(m, nullptr);
    for (profile const& p : m.profiles) {
        check_deployment(m, &p);
    }
}

// `[indexes.<name>]` and `[index-of]` (otok-toml.md 6.4).
void manifest_reader::indexes(toml_table& root, manifest& m)
{
    std::map<std::string, std::string, std::less<>> owners;  // publisher → index
    if (auto list = root.table("indexes")) {
        for (std::string const& name : list->keys()) {
            if (name == default_index_name) {
                list->error(list->position(name), "[indexes." + name + "]: '" + name + "' is the index of config.toml; name the project's indexes otherwise");
                list->ignore(name);
                continue;
            }
            if (!is_library_name(name)) {
                list->error(list->position(name), "invalid index name " + in_quotes(name) + " (lowercase letters, digits, '-' and '_')");
                list->ignore(name);
                continue;
            }
            auto t = list->table(name);
            if (!t) continue;
            index_declaration d;
            d.name = name;
            d.where = t->position();
            if (auto url = t->string("url", need::required)) {
                bool const view = url->starts_with("view+https://") && url->size() > 13;
                bool const repository = (url->starts_with("https://") && url->size() > 8) || (url->starts_with("file://") && url->size() > 7);
                if (!view && !repository) {
                    t->error(t->position("url"), t->describe("url") + " must be the view of an index, view+https://<base>, or the https or file URL of its git repository, not "
                        + in_quotes(*url));
                }
                d.url = std::move(*url);
            }
            if (auto publishers = t->strings("publishers")) {
                for (std::string const& p : *publishers) {
                    if (!is_publisher_name(p)) {
                        t->error(t->position("publishers"), "invalid publisher name " + in_quotes(p) + " in " + t->describe("publishers"));
                    } else if (auto [it, fresh] = owners.emplace(p, name); !fresh) {
                        t->error(t->position("publishers"), "the publisher " + in_quotes(p) + " is owned by both [indexes." + it->second + "] and [indexes." + name + "]");
                    }
                }
                d.publishers = std::move(*publishers);
            }
            t->finish();
            m.indexes.push_back(std::move(d));
        }
        list->finish();
    }
    if (auto of = root.table("index-of")) {
        for (std::string const& key : of->keys()) {
            auto index = of->string(key);
            if (!index) continue;
            auto const id = parse_recipe_id(key);
            if (!id) {
                of->error(of->position(key), in_quotes(key) + " in [index-of] is not a recipe id \"publisher/name\"");
                continue;
            }
            if (*index != default_index_name && std::ranges::find(m.indexes, *index, &index_declaration::name) == m.indexes.end()) {
                of->error(of->position(key), of->describe(key) + " names the index " + in_quotes(*index) + ", which is neither 'default' nor one of [indexes]");
                continue;
            }
            if (auto owner = owners.find(id->publisher); owner != owners.end() && owner->second != *index) {
                of->error(of->position(key), of->describe(key) + " takes " + in_quotes(key) + " from " + in_quotes(*index) + ", but [indexes." + owner->second
                    + "] owns the publisher " + in_quotes(id->publisher));
                continue;
            }
            m.index_of.emplace(key, std::move(*index));
        }
        of->finish();
    }
}

manifest manifest_reader::read(toml_table& root, std::string const& file, fragment_reader const& read_fragment)
{
    manifest m;

    if (auto otok = root.table("otok", need::required)) {
        if (auto format = otok->integer("format", need::required)) {
            m.format = *format;
            if (*format != manifest_format) {
                otok->error(otok->position("format"), "manifest format " + std::to_string(*format)
                    + " needs a newer otok: this one reads format " + std::to_string(manifest_format));
            }
        }
        if (auto list = otok->strings("include")) {
            for (std::string const& path : *list) {
                if (classify_file_path(path) != file_path_kind::inside) {
                    otok->error(otok->position("include"), "included fragment " + in_quotes(path) + " must be a file inside the project");
                } else {
                    m.includes.push_back(path);
                }
            }
        }
        // an environment of the target that builds a system's own programs, one per system (otok-toml.md 3)
        std::set<std::string, std::less<>> systems;
        auto tools = read_environments(*otok, "tool-build-on", [&](build_environment const& e) -> std::optional<std::string> {
            if (e.any_host()) return std::optional<std::string>{"names no system: tool slices are built on the machine's own"};
            if (!systems.insert(e.system).second) return "is a second one of the system '" + e.system + "'";
            for (target_info const* target : targets_) {
                if (target->host_system != e.system) continue;
                if (target->has_environment(e)) return std::nullopt;
                return "is not one of the " + target->name + " target, which builds the tools of '" + e.system + "'";
            }
            return "names a system no target of this otok builds tools for";
        });
        if (tools) m.tool_build_on = std::move(*tools);
        otok->finish();
    }
    if (m.format != manifest_format) {
        // A manifest of another format cannot be read by the rules of this one: report nothing else.
        (void)root.keys();
        return m;
    }

    std::vector<std::string> target_names;
    for (target_info const* target : targets_) {
        target_names.push_back("[" + target->name + "]");
        if (auto t = root.table(target->name)) {
            m.targets.push_back(target_table(*t, *target));
        }
    }
    if (m.targets.empty()) {
        root.error(root.position(), "missing target settings: the manifest needs " + joined(target_names));
    }
    for (std::size_t i = 1; i < m.targets.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (outputs_overlap(m.targets[i].output, m.targets[j].output)) {
                diags_.error(m.targets[i].where, "'output' of [" + m.targets[i].target + "] (" + in_quotes(m.targets[i].output)
                    + ") overlaps 'output' of [" + m.targets[j].target + "] (" + in_quotes(m.targets[j].output)
                    + "); set a separate 'output' for one of them");
            }
        }
    }

    dependencies(root, "deps", false, m);
    dependencies(root, "tool-deps", true, m);

    if (auto profiles = root.table("profile")) {
        for (std::string const& name : profiles->keys()) {
            if (auto p = profile_entry(*profiles, name)) m.profiles.push_back(std::move(*p));
        }
        profiles->finish();
    }

    if (auto policy = root.table("policy")) {
        auto const read_ids = [&](std::string_view key, std::vector<std::string>& out) {
            auto list = policy->strings(key);
            if (!list) return;
            for (std::string const& id : *list) {
                if (!is_license_id(id)) {
                    policy->error(policy->position(key), in_quotes(id) + " in " + policy->describe(key)
                        + " is not an SPDX license id (optionally with 'WITH <exception>')");
                }
            }
            out = std::move(*list);
        };
        read_ids("deny-licenses", m.policy.deny);
        read_ids("allow-licenses", m.policy.allow);
        if (policy->contains("deny-licenses") && policy->contains("allow-licenses")) {
            policy->error(policy->position(), "[policy] may have 'deny-licenses' or 'allow-licenses', not both");
        }
        policy->finish();
    }

    if (auto fetch = root.table("fetch")) {
        read_input_mirrors(*fetch, m.fetch, severity::error);
        fetch->finish();
    }
    indexes(root, m);

    root.ignore("metadata");
    root.finish();

    includes(m, file, m.includes, read_fragment);
    cross_check(m);
    return m;
}

}

dependency const* manifest::find_dep(std::string_view library) const noexcept
{
    auto it = std::ranges::find(deps, library, &dependency::library);
    return it == deps.end() ? nullptr : &*it;
}

build_environment manifest::tool_environment(target_info const& host) const
{
    for (build_environment const& e : tool_build_on) {
        if (e.system == host.host_system) return {e.system, "", e.compiler};
    }
    for (build_environment const& e : host.environments) {
        if (e.system == host.host_system) return {e.system, "", e.compiler};
    }
    return {};
}

dependency const* manifest::find_tool_dep(std::string_view library) const noexcept
{
    auto it = std::ranges::find(tool_deps, library, &dependency::library);
    return it == tool_deps.end() ? nullptr : &*it;
}

profile const* manifest::find_profile(std::string_view name) const noexcept
{
    auto it = std::ranges::find(profiles, name, &profile::name);
    return it == profiles.end() ? nullptr : &*it;
}

std::vector<std::string> build_configurations(target_settings const& ts, profile const* p)
{
    if (p) {
        if (auto it = p->targets.find(ts.target); it != p->targets.end() && it->second.configurations) return *it->second.configurations;
    }
    return ts.configurations;
}

std::optional<int> standard_of(manifest const& m, profile const* p, std::string_view library, bool tool, target_info const& target, language lang)
{
    if (tool) {
        dependency const* d = m.find_tool_dep(library);
        return d ? d->standards[lang] : std::nullopt;
    }
    if (p) {
        if (auto it = p->targets.find(target.name); it != p->targets.end() && it->second.standards[lang]) return it->second.standards[lang];
    }
    auto const ts = std::ranges::find(m.targets, target.name, &target_settings::target);
    if (ts != m.targets.end()) return ts->standards[lang];
    return std::nullopt;
}

std::optional<std::string> chosen_linkage_of(manifest const& m, profile const* p, std::string_view library)
{
    if (p) {
        if (auto it = p->deps.find(library); it != p->deps.end() && it->second.linkage) return *it->second.linkage;
    }
    if (dependency const* d = m.find_dep(library); d && d->linkage) return *d->linkage;
    return std::nullopt;
}

std::string linkage_of(manifest const& m, profile const* p, std::string_view library, target_info const& target)
{
    if (auto chosen = chosen_linkage_of(m, p, library)) return *chosen;
    if (p) {
        if (auto it = p->targets.find(target.name); it != p->targets.end() && it->second.linkage) return *it->second.linkage;
    }
    auto const ts = std::ranges::find(m.targets, target.name, &target_settings::target);
    if (ts != m.targets.end() && ts->linkage) return *ts->linkage;
    return target.linkages.front();
}

std::optional<manifest> parse_manifest(std::string_view content, std::string const& file,
    std::span<target_info const* const> targets, fragment_reader const& read_fragment, diagnostics& diags)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    manifest m = manifest_reader{targets, diags}.read(root, file, read_fragment);
    if (diags.error_count() != errors_before) return std::nullopt;
    return m;
}

std::optional<manifest> load_manifest(std::filesystem::path const& path,
    std::span<target_info const* const> targets, diagnostics& diags)
{
    std::string const display = path_to_utf8(path);
    auto content = read_file(path);
    if (!content) {
        diags.error({display}, content.error().message);
        return std::nullopt;
    }
    std::filesystem::path const dir = path.parent_path();
    fragment_reader const read_fragment = [&dir](std::string const& relative) -> std::optional<std::string> {
        auto fragment = read_file(dir / path_from_utf8(relative));
        if (!fragment) return std::nullopt;
        return std::move(*fragment);
    };
    return parse_manifest(*content, display, targets, read_fragment, diags);
}

}
