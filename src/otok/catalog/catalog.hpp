// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/cache/store.hpp"
#include "otok/core/error.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/target_info.hpp"
#include "otok/fetch/git.hpp"

// The catalog site (DESIGN 8, docs/PLAN.md decision 49): static pages of a checkout of the index, generated
// next to its view.
namespace otok {

// A file built into otok.
struct embedded_file
{
    std::string_view path;
    std::string_view contents;
};

// The templates (`templates/`) and assets (`assets/`) of the site, from src/otok/catalog/site/, sorted by path.
std::span<embedded_file const> catalog_site_files();

struct catalog_settings
{
    std::filesystem::path root;        // a checkout of the index
    std::string commit;                // its commit
    std::filesystem::path output;
    std::span<target_info const* const> targets;
    cache_store* cache = nullptr;      // the recipe trees are fetched into it
    git_tool const* git = nullptr;
};

struct catalog_written
{
    generated_files_written files;
    std::vector<std::string> unavailable;  // why a recipe tree is not shown
};

// Generates the site of `settings.root` into `settings.output`, beside the view's `index/`: every file written only
// when its contents change, the site's files no longer generated removed.
result<catalog_written> write_index_catalog(catalog_settings const& settings);

}
