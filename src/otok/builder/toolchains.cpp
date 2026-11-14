// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/builder/toolchains.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <set>

#include "otok/builder/builder.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/resolver/options.hpp"
#include "otok/version/constraint.hpp"

namespace otok {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr char path_list_separator = ';';
#else
constexpr char path_list_separator = ':';
#endif

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

// How a known toolchain is found (recipe-toml.md 13): its programs in order, the argument that prints
// the version, and what the output must contain when another program has the same name.
struct toolchain_kind
{
    std::string_view name;
    std::array<std::string_view, 2> programs;
    std::string_view argument;
    std::string_view marker;
    bool integer = false;  // the version is the first run of digits, without a dot (gn's commit count)
};

constexpr std::array<toolchain_kind, 15> kinds{{
    {"autoconf", {"autoconf"}, "--version", ""},
    {"automake", {"automake"}, "--version", ""},
    {"cmake", {"cmake"}, "--version", ""},
    {"gn", {"gn"}, "--version", "", true},
    {"go", {"go"}, "version", ""},
    {"libtool", {"glibtool", "libtool"}, "--version", "GNU libtool"},
    {"make", {"gmake", "make"}, "--version", "GNU Make"},
    {"meson", {"meson"}, "--version", ""},
    {"nasm", {"nasm"}, "-v", ""},
    {"ninja", {"ninja"}, "--version", ""},
    {"perl", {"perl"}, "--version", ""},
    {"pkg-config", {"pkg-config", "pkgconf"}, "--version", ""},
#ifdef _WIN32
    // Python's installers make python.exe only; python3.exe is the Store's alias or MSYS2's
    {"python", {"python", "python3"}, "--version", ""},
#else
    {"python", {"python3", "python"}, "--version", ""},
#endif
    {"rust", {"rustc"}, "--version", ""},
    {"zig", {"zig"}, "version", ""},
}};

std::string slashed(fs::path const& p)
{
    return path_to_utf8(p.generic_u8string());
}

std::string package_label(resolved_package const& p, bool tool)
{
    return (tool ? "tool " : "") + in_quotes(p.id);
}

}

std::optional<std::string> detail::dotted_version_in(std::string_view output)
{
    auto const digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (!digit(output[i])) continue;
        std::size_t end = i;
        while (end < output.size() && (digit(output[end]) || output[end] == '.')) ++end;
        std::string_view run = output.substr(i, end - i);
        while (run.ends_with('.')) run.remove_suffix(1);
        if (run.find('.') != std::string_view::npos && run.find("..") == std::string_view::npos) return std::string{run};
        i = end;
    }
    return std::nullopt;
}

std::optional<std::string> detail::integer_version_in(std::string_view output)
{
    auto const digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    auto const first = std::ranges::find_if(output, digit);
    if (first == output.end()) return std::nullopt;
    return std::string{first, std::find_if_not(first, output.end(), digit)};
}

result<std::string> run_version_command(fs::path const& program, std::vector<std::string> const& arguments)
{
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    fs::path const tmp = fs::temp_directory_path(ec);
    // standard error goes to the log: some programs print their version there
    fs::path const log = tmp / ("otok-version-" + std::to_string(current_process_id()) + "-" + std::to_string(counter++) + ".txt");
    std::vector<std::string> argv{path_to_utf8(program)};
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    auto exit = run_process({std::move(argv), current_environment(), tmp, log, true});
    std::string const errors = read_file(log).value_or("");
    fs::remove(log, ec);
    if (!exit) return std::unexpected{exit.error()};
    if (!exit->success()) return fail(describe_exit(*exit));
    return exit->output + errors;
}

toolchain_finder::toolchain_finder(std::string path, version_probe probe) : path_{std::move(path)}, probe_{std::move(probe)} {}

result<std::string> const& toolchain_finder::output_of(fs::path const& program, std::vector<std::string> const& arguments)
{
    auto const key = std::pair{program, arguments};
    auto it = outputs_.find(key);
    if (it == outputs_.end()) it = outputs_.emplace(key, probe_(program, arguments)).first;
    return it->second;
}

result<found_toolchain> toolchain_finder::find(toolchain_requirement const& req, std::string const& target_path)
{
    auto const kind = std::ranges::find(kinds, req.name, &toolchain_kind::name);
    if (kind == kinds.end()) return fail("otok does not know this toolchain");
    if (req.version) {
        if (auto valid = req.version->validate(version_order::dotted()); !valid) return fail(valid.error().message);
    }
    std::string path = target_path;
    if (!path_.empty()) path += (path.empty() ? "" : std::string(1, path_list_separator)) + path_;
    env_vars const env{{"PATH", path}};
    std::error_code ec;
    fs::path const cwd = fs::temp_directory_path(ec);

    std::vector<std::string> problems;
    std::set<fs::path> tried;
    for (std::string_view name : kind->programs) {
        if (name.empty()) continue;
        auto program = find_program(name, env, cwd);
        if (!program || !tried.insert(*program).second) continue;
        std::string const where = in_quotes(slashed(*program));
        auto const& output = output_of(*program, {std::string{kind->argument}});
        if (!output) {
            problems.push_back("cannot run " + where + ": " + output.error().message);
            continue;
        }
        if (!kind->marker.empty() && output->find(kind->marker) == std::string::npos) {
            problems.push_back(where + " is not " + std::string{kind->marker});
            continue;
        }
        auto const text = kind->integer ? detail::integer_version_in(*output) : detail::dotted_version_in(*output);
        result<version> const v = text ? version::parse(*text) : fail("no version");
        if (!v) {
            problems.push_back("cannot read the version of " + where);
            continue;
        }
        if (req.version && !req.version->matches(*v, version_order::dotted())) {
            problems.push_back("found " + *text + " at " + where);
            continue;
        }
        if (!req.targets.empty()) {
            auto rustup = find_program("rustup", env, cwd);
            if (!rustup) return fail("found " + *text + " at " + where + ", but rustup, which checks its targets, is not in PATH");
            auto const& installed = output_of(*rustup, {"target", "list", "--installed"});
            if (!installed) return fail("cannot run " + in_quotes(slashed(*rustup)) + ": " + installed.error().message);
            std::vector<std::string> missing;
            for (std::string const& t : req.targets) {
                bool found = false;
                for (std::size_t at = installed->find(t); at != std::string::npos && !found; at = installed->find(t, at + 1)) {
                    auto const separate = [&](std::size_t i) { return i >= installed->size() || std::isspace(static_cast<unsigned char>((*installed)[i])); };
                    found = (at == 0 || separate(at - 1)) && separate(at + t.size());
                }
                if (!found) missing.push_back(t);
            }
            if (!missing.empty()) {
                std::string list;
                for (std::string const& t : missing) list += (list.empty() ? "" : ", ") + in_quotes(t);
                return fail("found " + *text + " at " + where + ", but the targets " + list + " are not installed (rustup target add)");
            }
        }
        return found_toolchain{*program, *text};
    }
    if (problems.empty()) return fail("not found in PATH");
    std::string message;
    for (std::string const& p : problems) message += (message.empty() ? "" : "; ") + p;
    return fail(message);
}

std::string toolchain_choice_key(std::string const& target, std::string const& package_key)
{
    return target + "/" + package_key;
}

std::expected<toolchain_choice, std::vector<std::string>> check_toolchains(resolution const& r, manifest const& m, std::size_t configuration,
    std::span<build_target* const> targets, toolchain_finder& finder, bool tool_roots)
{
    toolchain_choice choice;
    std::vector<std::string> errors;
    auto const error = [&](std::string message) {
        if (std::ranges::find(errors, message) == errors.end()) errors.push_back(std::move(message));
    };

    // as the builder chooses: the tool slice is built by the target of the machine's system (otok-toml.md 7)
    build_target* const tool_target = host_target(targets);
    std::map<build_target*, result<std::string>> paths;
    auto const check = [&](resolved_package const& p, bool tool, build_target* target) {
        auto path = paths.find(target);
        if (path == paths.end()) path = paths.emplace(target, target->tools_path()).first;
        if (!path->second) {
            error("the target '" + target->info().name + "': " + path->second.error().message);
            return;
        }
        chosen_toolchains& chosen = choice[toolchain_choice_key(target->info().name, package_key(p.id, tool))];
        for (toolchain_requirement const& req : p.chosen.recipe->toolchains) {
            // options and the target's OS (recipe-toml.md 13)
            if (req.when && !condition_holds(*req.when, p.configurations[configuration].options, target->info().name, "")) continue;
            if (req.default_recipe || !req.from.empty()) continue;  // built by its recipe
            auto found = finder.find(req, *path->second);
            if (found) {
                chosen.emplace(req.name, std::move(*found));
                continue;
            }
            error(in_quotes(req.name) + (req.version ? " (" + req.version->str() + ")" : "") + ", needed by " + package_label(p, tool) + ": "
                + found.error().message);
        }
    };

    // the tool packages some build uses
    std::set<std::string> used;
    // `p` uses its tool dependencies; a tool package (`tool`) also its dependencies, built in the tool slice
    auto const use = [&](auto& self, resolved_package const& p, bool tool) -> void {
        std::vector<std::string> next = p.tool_dependencies;
        if (tool) next.insert(next.end(), p.dependencies.begin(), p.dependencies.end());
        for (std::string const& d : next) {
            resolved_package const* dp = r.find_tool(d);
            if (dp && used.insert(d).second) self(self, *dp, true);
        }
    };
    for (resolved_package const& p : r.packages) {
        if (!p.configurations[configuration].platforms.empty()) use(use, p, false);
    }
    std::vector<std::string> roots;  // publish's [tool-deps], and the tools the project delivers
    for (dependency const& d : m.tool_deps) {
        if (tool_roots) roots.push_back(d.library);
    }
    for (auto const& [library, id] : r.delivered_tools) roots.push_back(id);
    for (std::string const& root : roots) {
        resolved_package const* dp = r.find_tool(root);
        if (dp && used.insert(dp->id).second) use(use, *dp, true);
    }
    for (resolved_package const& p : r.tool_packages) {
        if (p.configurations[configuration].platforms.empty() || p.chosen.recipe->toolchains.empty() || !tool_target) continue;
        if (!used.contains(p.id)) continue;
        check(p, true, tool_target);
    }
    for (resolved_package const& p : r.packages) {
        if (p.configurations[configuration].platforms.empty() || p.chosen.recipe->toolchains.empty()) continue;
        std::vector<build_target*> on;  // in the order of the targets
        for (std::string const& platform : p.configurations[configuration].platforms) {
            for (build_target* t : targets) {
                if (t->info().find_platform(platform)) {
                    if (std::ranges::find(on, t) == on.end()) on.push_back(t);
                    break;
                }
            }
        }
        if (tool_target && std::ranges::find(on, tool_target) == on.end()) {
            auto defined = script_defines(p.chosen.dir / "build.lua", p.library + "/build.lua", "build_tools");
            if (!defined) error(defined.error().message);
            else if (*defined) on.push_back(tool_target);
        }
        for (build_target* t : on) check(p, false, t);
    }
    if (!errors.empty()) return std::unexpected{std::move(errors)};
    return choice;
}

}
