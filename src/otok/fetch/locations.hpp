// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/diagnostics.hpp"

// Where inputs are fetched from: the locations a recipe names and those a project and the machine add
// (docs/formats/otok-toml.md 10, config.md 2.1, cache.md 3.1, 4).
namespace otok {

class toml_table;

// A rule of `url-rewrite` (config.md 2.2).
struct url_rewrite
{
    std::string from;     // a URL pattern with {name} and {name*} placeholders
    std::string to;       // the new URL, with the placeholders of `from` and {sha256}
    bool exact = false;   // `from` matches the whole URL, not its beginning
};

// The locations of one level: a project's `[fetch]` or the machine's config.toml.
struct input_mirrors
{
    std::vector<std::string> asset_mirrors{};  // URL templates containing {sha256}
    std::vector<url_rewrite> rewrites{};
    bool mirrors_only = false;                 // for every input
    std::vector<std::string> mirrors_only_libraries{};  // for the inputs of these libraries

    // Whether only this level's locations, and those before it, may serve an input of `library`.
    bool only_for(std::string_view library) const;
};

struct fetch_locations
{
    input_mirrors project{};
    input_mirrors machine{};
};

// `url` rewritten by the first rule that matches (config.md 2.2); nothing when none does. `sha256` is the
// hash of an archive input, empty for git, where rules using {sha256} do not apply.
std::optional<std::string> rewrite_url(std::string const& url, std::span<url_rewrite const> rewrites, std::string_view sha256 = {});

// What is wrong with a rule's patterns (config.md 2.2), as a sentence; nothing when they are valid.
std::optional<std::string> url_rewrite_problem(url_rewrite const& rule);

// The locations of an archive of `library` (may be empty) in the order they are tried (cache.md 3.1):
// the project's asset mirrors and rewrites of `url` and `mirrors`, the machine's, then `url` and
// `mirrors` themselves, as mirrors-only allows. Each once.
std::vector<std::string> archive_locations(std::string const& sha256, std::string const& url, std::span<std::string const> mirrors,
    fetch_locations const& where, std::string_view library);

// The URLs a git repository of `library` is fetched from, in order (cache.md 4): rewritten by the
// project, by the machine, then the URL itself, as mirrors-only allows.
std::vector<std::string> git_locations(std::string const& url, fetch_locations const& where, std::string_view library);

// What is wrong with an asset-mirror template, as the end of a message: "must hold …".
std::optional<std::string> asset_mirror_problem(std::string_view url_template);

// Reads `asset-mirror`, `url-rewrite` and `mirrors-only` of a table into `out`, reporting problems
// through it; keys of the url-rewrite tables it does not know are reported with `unknown_keys`.
void read_input_mirrors(toml_table& t, input_mirrors& out, severity unknown_keys);

}
