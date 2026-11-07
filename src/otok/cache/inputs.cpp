// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/cache/inputs.hpp"

#include <optional>

namespace otok {

std::vector<result<std::filesystem::path>> fetch_inputs(cache_store& cache, std::span<input_request const> inputs,
    http_settings const& settings, git_tool const* git)
{
    std::vector<std::optional<result<std::filesystem::path>>> results(inputs.size());
    auto const failed = [&](std::size_t i, std::string const& message) { results[i] = fail(inputs[i].what + ": " + message); };

    // the downloads first, all at once
    std::vector<cache_download> downloads;
    std::vector<std::size_t> downloaded;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        locked_location const& l = inputs[i].location;
        if (l.type == locked_location::kind::archive) {
            downloads.push_back({l.url, l.sha256, l.mirrors, inputs[i].library});
            downloaded.push_back(i);
        } else if (l.type != locked_location::kind::git) {
            failed(i, "not an input to fetch");
        }
    }
    auto files = cache.fetch(downloads, settings);
    for (std::size_t k = 0; k < downloaded.size(); ++k) {
        std::size_t const i = downloaded[k];
        if (!files[k]) {
            failed(i, files[k].error().message);
        } else if (!inputs[i].unpack) {
            results[i] = std::move(*files[k]);
        } else if (auto tree = cache.unpacked_archive(inputs[i].location.sha256); !tree) {
            failed(i, tree.error().message);
        } else {
            results[i] = std::move(*tree);
        }
    }

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        locked_location const& l = inputs[i].location;
        if (l.type != locked_location::kind::git) continue;
        if (!git) {
            failed(i, "otok needs git to fetch " + l.git + ", and git is not in PATH");
        } else if (auto tree = cache.git_tree(l.git, l.commit, l.tag, l.submodules, *git, inputs[i].library); !tree) {
            failed(i, tree.error().message);
        } else {
            results[i] = std::move(*tree);
        }
    }

    std::vector<result<std::filesystem::path>> out;
    out.reserve(results.size());
    for (auto& r : results) out.push_back(std::move(*r));
    return out;
}

}
