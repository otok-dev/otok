// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <span>
#include <vector>

#include "otok/builder/builder.hpp"
#include "otok/builder/target.hpp"
#include "otok/builder/toolchains.hpp"
#include "otok/config/config.hpp"
#include "otok/core/command_line.hpp"
#include "otok/core/console.hpp"
#include "otok/core/executor.hpp"
#include "otok/core/process.hpp"
#include "otok/core/target_info.hpp"
#include "otok/index/index.hpp"
#include "otok/publish/github.hpp"
#include "otok/resolver/provider.hpp"

// The commands of DESIGN 5.4 implemented so far: install and update (resolution and the lock; fetching
// and building come with phases 4-6), tree, why, help.
namespace otok {

struct command_context
{
    std::filesystem::path project_dir;              // where otok.toml and otok.lock are
    std::span<target_info const* const> targets;
    recipe_provider* provider = nullptr;            // nothing: the project's own path and git recipes
    std::ostream& out;
    std::ostream& err;
    environment env = process_environment();        // for config.toml, OTOK_* and PATH (tests set their own)
    // What standard error can show (docs/PLAN.md decision 59); main asks the terminal. Tests: plain lines.
    terminal_caps terminal{};
    // Given by a command that runs builds of its own (otok publish, docs/PLAN.md decision 60): the display they share,
    // and their jobs. Nothing: each build has its own.
    progress_display* display = nullptr;
    job_broker* jobs = nullptr;
    // An install another command runs for itself (otok publish): it says what it builds, but not what it packages,
    // and the builders say once what is the same for all.
    bool nested = false;
    announcements* announced = nullptr;
    // What `display` calls such an install while it packages and writes its output, after its slices (otok publish:
    // the recipe, "apotocki/boost 1.92.0-r3")
    std::string label{};
    // The targets that can build (docs/PLAN.md decision 19), made with the machine settings; a project
    // whose target has none is resolved and fetched only.
    std::function<std::vector<std::unique_ptr<build_target>>(machine_config const&)> make_build_targets{};
    // The otok executable, whose `jobserver-exec` runs the compilers of tools without the jobserver
    // (build-lua-api.md 6.2); empty: none (tests, whose executable is not otok).
    std::filesystem::path launcher{};
    // Runs the version commands of toolchains (recipe-toml.md 13); tests replace it.
    version_probe probe_version = run_version_command;
    // The calls of web APIs (`otok publish` to GitHub); empty: libcurl. Tests answer them.
    http_transport http{};
    // Entries read as if the index had them: those `otok publish` builds together (DESIGN 7.4).
    std::vector<index_addition> unpublished{};
    // The project's [tool-deps] are built as roots: the project `otok publish` builds a tool recipe with
    // (docs/PLAN.md decision 78).
    bool tool_roots = false;
    // Variables added to the environment of git: `otok publish` gives git its GitHub token for the index, as GIT_CONFIG_*
    // of an http.extraHeader, which no command line or message shows.
    env_vars git_env{};
};

std::span<option_spec const> global_options();
std::span<command_spec const> command_specs();

// Runs a parsed command. Returns the process exit code: 0 success, 1 failure (reported to ctx.err).
int run_command(parsed_command const& command, command_context& ctx);

}
