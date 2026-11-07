// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/resolver/provider.hpp"

#include "otok/core/diagnostics.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/tree_hash.hpp"

namespace otok {

project_recipe_provider::project_recipe_provider(std::filesystem::path project_dir, std::span<target_info const* const> targets,
    cache_store* cache, std::optional<git_tool> git)
    : project_dir_{std::move(project_dir)}, targets_{targets.begin(), targets.end()}, cache_{cache}, git_{std::move(git)}
{
}

result<candidate> project_recipe_provider::load(std::string_view library, std::filesystem::path const& dir, std::string const& where,
    recipe_names names)
{
    diagnostics diags;
    auto r = load_recipe(dir, targets_, diags, names);
    if (!r) {
        std::string message = "the recipe of '" + std::string{library} + "' in " + where + " is invalid:";
        for (auto const& d : diags.items()) message += "\n  " + format(d);
        return fail(std::move(message));
    }
    candidate c;
    c.recipe = std::make_shared<recipe const>(std::move(*r));
    c.dir = dir;
    return c;
}

result<std::vector<candidate>> project_recipe_provider::candidates(std::string_view library, recipe_ref const& ref)
{
    if (ref.source == recipe_ref::kind::index) {
        return fail("the recipe index is not available in this version of otok, so '" + ref.id.str()
            + "' cannot be found for '" + std::string{library} + "'; use recipe = { path = \"…\" } or recipe = { git = \"…\", rev = \"…\" }");
    }
    std::string const key = ref.source == recipe_ref::kind::git ? "git " + ref.git + " " + ref.rev + " " + ref.tag + " " + ref.path : "path " + ref.path;
    if (auto it = loaded_.find(key); it != loaded_.end()) return it->second;

    candidate c;
    if (ref.source == recipe_ref::kind::git) {
        if (!cache_) return fail("git recipes need the cache, which is not available here ('" + std::string{library} + "')");
        if (!git_) return fail("otok needs git for the recipe of '" + std::string{library} + "', and git is not in PATH");
        auto tree = cache_->git_directory(ref.git, ref.rev, ref.tag, ref.path, *git_, std::string{library});
        if (!tree) return fail("cannot fetch the recipe of '" + std::string{library} + "': " + tree.error().message);
        std::filesystem::path const dir = *tree;
        std::string const where = ref.git + " at " + ref.rev + (ref.path.empty() ? std::string{} : " ('" + ref.path + "')");
        // a recipe a newer otok may have written (formats README 1.5)
        auto loaded = load(library, dir, where, recipe_names::open);
        if (!loaded) return std::unexpected{loaded.error()};
        auto hash = tree_hash(dir);
        if (!hash) return fail("the recipe of '" + std::string{library} + "' in " + where + ": " + hash.error().message);
        c = std::move(*loaded);
        c.kind = locked_package::recipe_kind::git;
        c.recipe_source.type = locked_location::kind::git;
        c.recipe_source.git = ref.git;
        c.recipe_source.commit = ref.rev;
        c.recipe_source.tag = ref.tag;
        c.recipe_source.path = ref.path;
        c.recipe_sha256 = std::move(*hash);
    } else {
        std::filesystem::path const dir = (project_dir_ / path_from_utf8(ref.path)).lexically_normal();
        auto loaded = load(library, dir, "'" + path_to_utf8(dir) + "'", recipe_names::strict);
        if (!loaded) return std::unexpected{loaded.error()};
        c = std::move(*loaded);
        c.kind = locked_package::recipe_kind::path;
        if (classify_file_path(ref.path) == file_path_kind::inside) {
            auto hash = tree_hash(dir);
            if (!hash) return std::unexpected{hash.error()};
            c.recipe_source.type = locked_location::kind::path;
            c.recipe_source.path = path_to_utf8(path_from_utf8(ref.path).lexically_normal().generic_u8string());
            c.recipe_sha256 = std::move(*hash);
        } else {
            // outside the project, which is what gets committed: not reproducible (otok-lock.md 3)
            c.recipe_source.type = locked_location::kind::local;
        }
    }
    return loaded_.emplace(key, std::vector{std::move(c)}).first->second;
}

version_order project_recipe_provider::order(std::string_view)
{
    return version_order::dotted();
}

}
