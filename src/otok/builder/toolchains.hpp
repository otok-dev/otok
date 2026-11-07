// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/builder/target.hpp"
#include "otok/core/error.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/recipe/recipe.hpp"
#include "otok/resolver/resolver.hpp"

// The toolchains recipes declare (recipe-toml.md 13, docs/PLAN.md decision 38): their programs and
// versions, found before anything is fetched or built.
namespace otok {

struct found_toolchain
{
    std::filesystem::path program;      // absolute
    std::string version;                // "3.31.6"
};

// The toolchains of one recipe on one target: name → the chosen program.
using chosen_toolchains = std::map<std::string, found_toolchain>;

// Runs `program arguments…` and returns what it printed, standard output and standard error; tests
// replace it.
using version_probe = std::function<result<std::string>(std::filesystem::path const& program, std::vector<std::string> const& arguments)>;
result<std::string> run_version_command(std::filesystem::path const& program, std::vector<std::string> const& arguments);

namespace detail {

// The first run of digits and dots with a dot in it: "3.31.6" of "cmake version 3.31.6", "1.22.3" of
// "go version go1.22.3 darwin/arm64".
std::optional<std::string> dotted_version_in(std::string_view output);

// The first run of digits: "2257" of gn's "2257 (fde0b7b)".
std::optional<std::string> integer_version_in(std::string_view output);

}

class toolchain_finder
{
public:
    // `path`: otok's own PATH, searched after the target's.
    explicit toolchain_finder(std::string path, version_probe probe = run_version_command);

    // The program chosen for `req`, looked up in `target_path` (build_target::tools_path), then in
    // otok's PATH; or why none fits: "not found in PATH", "found 3.22.1 at '/usr/bin/cmake'".
    result<found_toolchain> find(toolchain_requirement const& req, std::string const& target_path);

private:
    // What `program arguments…` printed, asked once.
    result<std::string> const& output_of(std::filesystem::path const& program, std::vector<std::string> const& arguments);

    std::string path_;
    version_probe probe_;
    std::map<std::pair<std::filesystem::path, std::vector<std::string>>, result<std::string>> outputs_;
};

// The toolchains of every recipe the configuration builds, per target: "<target>/<package key>" →
// its choice. Recipes without [toolchains] have no entry.
using toolchain_choice = std::map<std::string, chosen_toolchains>;

std::string toolchain_choice_key(std::string const& target, std::string const& package_key);

// Checks [toolchains] of every package the configuration builds (tool packages and build_tools on the
// tool target, the first target of the manifest that can build; other packages on the targets of their
// platforms). The toolchains with `default-recipe` are skipped: their recipes build them
// (recipe-toml.md 13.1). With `tool_roots` the tool packages of the project's [tool-deps] are built too
// (resolve_settings::tool_roots). On failure: one message per missing or unsuitable toolchain.
std::expected<toolchain_choice, std::vector<std::string>> check_toolchains(resolution const& r, manifest const& m, std::size_t configuration,
    std::span<build_target* const> targets, toolchain_finder& finder, bool tool_roots = false);

}
