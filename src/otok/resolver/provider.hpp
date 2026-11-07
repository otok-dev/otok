// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/cache/store.hpp"
#include "otok/core/error.hpp"
#include "otok/core/target_info.hpp"
#include "otok/fetch/git.hpp"
#include "otok/lock/lock.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/recipe/recipe.hpp"
#include "otok/version/version.hpp"

// Where the resolver learns about recipes (docs/PLAN.md decision 11): the candidates of a library
// for a recipe choice and the library's version order. Path and git recipes are implemented here, the
// index later (phase 9).
namespace otok {

enum class advisory_severity
{
    withdrawn,
    security,
    broken
};

// An active advisory of the index that concerns a candidate (index.md 7).
struct candidate_advisory
{
    std::string id;
    advisory_severity severity = advisory_severity::broken;
    std::string summary;
    std::vector<std::string> unknown;  // values this otok took in place of those it does not know (index.md 7.2)
};

// One recipe version the resolver may choose.
struct candidate
{
    std::shared_ptr<otok::recipe const> recipe;  // version, revision, channel, dependencies, options
    std::vector<candidate_advisory> advisories;  // the resolver avoids such a version (index.md 7.1)
    std::string deprecation;                     // why the recipe is deprecated and its replacement; empty: it is not

    // What the lock records about the recipe (otok-lock.md 2, 3).
    locked_package::recipe_kind kind = locked_package::recipe_kind::index;
    recipe_id id;                                // index recipes
    std::string index;                           // index recipes: the project's index they are from; empty for `default`
    locked_location recipe_source;
    std::string recipe_sha256;                   // empty for a local recipe source

    std::filesystem::path dir;                   // the recipe directory (build.lua, patches); empty in tests
};

class recipe_provider
{
public:
    virtual ~recipe_provider() = default;

    // The candidates of `library` built by the chosen recipe, in any order. A failure when the recipe
    // cannot be read; it stops the resolution.
    virtual result<std::vector<candidate>> candidates(std::string_view library, recipe_ref const& ref) = 0;

    // The order of the library's versions.
    virtual version_order order(std::string_view library) = 0;

    // Makes a chosen candidate ready to build: an index candidate carries only its entry until its
    // recipe tree is fetched and checked (index.md 3.1), which sets `recipe` and `dir`. Nothing to do
    // for the others.
    virtual result<void> prepare(candidate& c)
    {
        (void)c;
        return {};
    }

    // The versions of the library's recipe this otok cannot read and left aside, "1.4-r1 (apple): <why>" (index.md 3.2),
    // once its candidates were asked for: what a failed resolution or a lock names.
    virtual std::vector<std::string> unreadable(std::string_view library)
    {
        (void)library;
        return {};
    }
};

// The recipes a project names itself (otok-toml.md 6.1): `recipe = { path }`, relative to the project
// directory, and `recipe = { git, rev, path }`, a directory of a commit fetched into the cache. Without
// the index every library uses the dotted scheme (decision 11).
class project_recipe_provider : public recipe_provider
{
public:
    // `cache` and `git` are needed for git recipes only; without them such a recipe is an error.
    project_recipe_provider(std::filesystem::path project_dir, std::span<target_info const* const> targets,
        cache_store* cache = nullptr, std::optional<git_tool> git = std::nullopt);

    result<std::vector<candidate>> candidates(std::string_view library, recipe_ref const& ref) override;
    version_order order(std::string_view library) override;

private:
    result<candidate> load(std::string_view library, std::filesystem::path const& dir, std::string const& where, recipe_names names);

    std::filesystem::path project_dir_;
    std::vector<target_info const*> targets_;
    cache_store* cache_;
    std::optional<git_tool> git_;
    std::map<std::string, std::vector<candidate>, std::less<>> loaded_;  // by recipe location
};

}
