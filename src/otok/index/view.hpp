// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "otok/cache/store.hpp"
#include "otok/core/error.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/target_info.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/index/index.hpp"

// The HTTPS view of the index (docs/formats/index.md 8, docs/PLAN.md decision 48): its client, with the
// cache of cache.md 11, and its generator.
namespace otok {

// The view format this otok reads and writes (index.md 8).
inline constexpr std::int64_t view_format = 1;

// The base URL of the view `index` names as `view+https://<base>`, without a final `/`; empty when it
// names no view.
std::string view_base(std::string_view index);

// The files of a view by their path under `index/` (`recipes/a/b.json`); empty when the view has none.
using view_files = std::function<result<std::optional<std::string>>(std::string const& path)>;

// A view on disk: the files of `<dir>/index/`.
view_files view_directory(std::filesystem::path dir);

// A view over HTTP at `base`, each file requested at most once and kept in the cache with its `ETag`
// (cache.md 11); offline, the cached files. `cache` is shared, not borrowed.
view_files view_over_http(std::string base, cache_store& cache, http_transport http);

// The index read from a view; `where` names the view in messages.
std::unique_ptr<index_source> view_index_source(view_files files, std::string where);

// The index `config.md` names: `mirror` when given (a view at a URL, a view on disk or a checkout), else
// the view or the git repository of `index`.
result<std::unique_ptr<index_source>> open_index_source(std::string const& index, std::string const& mirror, cache_store& cache,
    git_tool const* git, http_transport const& http);

using view_written = generated_files_written;

// Generates the view of the checkout `root` at `commit` into `<output>/index/` (index.md 8): every file
// read and checked as a client reads it, a file written only when its contents change.
result<view_written> write_index_view(std::filesystem::path const& root, std::string const& commit, std::filesystem::path const& output,
    std::span<target_info const* const> targets);

}
