// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <span>
#include <string>
#include <vector>

#include "otok/core/environment.hpp"
#include "otok/core/error.hpp"
#include "otok/core/option_values.hpp"
#include "otok/core/target_info.hpp"
#include "otok/lock/lock.hpp"
#include "otok/recipe/recipe.hpp"

// The build of `otok publish` (DESIGN 7.4, docs/PLAN.md decision 46).
namespace otok {

// The project a recipe is built with before it is published.
struct publish_project
{
    std::string manifest;              // otok.toml
    std::vector<std::string> platforms;  // the platforms it builds, in the recipe's order
    bool tool = false;                 // the recipe is in [tool-deps], built as a root (command_context::tool_roots)
};

// The target of a recipe to publish (index.md 3: one): that of its platforms; a failure when they are of several
// targets or of none of `targets`.
result<target_info const*> publish_target(recipe const& r, std::span<target_info const* const> targets);

// The project that builds the recipe `r`, published from `source` (git, commit, tag, path), as its only dependency with
// `options` (nothing: the defaults), in `environment`, one of its `build-on` (docs/PLAN.md decision 82): the table of its
// target with the supported platforms, `[platforms] architectures` and `[publish.<target>]` over
// target_info::publish_defaults (recipe-toml.md 14), and `build-on = [<environment>]`. A tool recipe without products,
// which can only be a tool dependency (recipe-toml.md 3.1), is the project's [tool-deps] entry instead, built in the tool
// slice of its target, in `environment` as `[otok] tool-build-on` (docs/PLAN.md decision 78), the target table with the
// tool platform alone over the same defaults; a failure when it does not support the target's tool platform.
result<publish_project> make_publish_project(recipe const& r, locked_location const& source, target_info const& target,
    build_environment const& environment, option_values const* options);

// The locations an index on GitHub refuses (recipe-toml.md 4, index.md 9): the recipe's repository, the
// source, the resources and the mirrors that are not https, each as "<what> '<location>'".
std::vector<std::string> non_https_locations(locked_location const& recipe_source, recipe const& r);

}
