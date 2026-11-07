// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "otok/cache/store.hpp"
#include "otok/core/error.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/lock/lock.hpp"

// The inputs of locked packages (sources and resources) made available in the cache.
namespace otok {

struct input_request
{
    std::string what;           // names the input in messages: "openssl 3.5.4 source"
    locked_location location;   // archive or git
    bool unpack = true;         // an archive: unpacked into sources/, or kept as the downloaded file
    std::string library{};      // whose input it is (mirrors-only of some libraries)
};

// Makes every input available in the cache: archives are downloaded (several at once) and unpacked,
// git inputs are fetched and their trees exported. One result per request, in order: the unpacked
// tree, or the downloaded file of an archive kept packed. `git` may be null when no input needs it.
std::vector<result<std::filesystem::path>> fetch_inputs(cache_store& cache, std::span<input_request const> inputs,
    http_settings const& settings, git_tool const* git);

}
