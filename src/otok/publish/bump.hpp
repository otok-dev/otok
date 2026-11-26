// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/core/error.hpp"

// `otok bump`: a recipe moved to another upstream version (DESIGN 7.4.2, docs/PLAN.md decision 146).
namespace otok {

// One way a URL or a tag spells a version, and how it spells the new one: `1.92.0` → `1.93.0`, `1_92_0` → `1_93_0`,
// the line `1.92` → `1.93`.
struct version_spelling
{
    std::string from;
    std::string to;
    char separator = 0;       // between the numbers of `from`: '.', '_', '-'; 0 for a version of one part
};

// The spellings of the version `from` otok replaces by those of `to`, the longest first: the whole version with dots,
// underscores and hyphens between its parts, then its line (the first two parts) with dots and underscores when it has
// three parts or more and the line changes. A spelling that does not change is left out.
std::vector<version_spelling> version_spellings(std::string_view from, std::string_view to);

// `text` with every spelling of `spellings` replaced where it stands alone: not after or before a digit, nor after or
// before its separator next to a digit (`1.2.1` is not in `1.2.10` or `11.2.1`); nothing when none was found.
std::optional<std::string> replace_version(std::string_view text, std::span<version_spelling const> spellings);

// Whether `text` holds one of the spellings' `from` standing alone, as replace_version finds them.
bool mentions_version(std::string_view text, std::span<version_spelling const> spellings);

// Whether `text` holds the numbers of the version `version` written together (`1920` of `1.92.0`), standing alone among
// digits: a spelling otok does not know, which it does not guess. False for a version of one part.
bool mentions_version_digits(std::string_view text, std::string_view version);

enum class bump_status
{
    changed,
    unchanged,     // its location does not hold the version: pinned apart from it
    not_bumped,    // it holds the version in a way otok does not replace, or it is a git commit without a tag
    failed
};

std::string_view bump_status_name(bump_status s) noexcept;  // "changed", "unchanged", "not-bumped", "failed"

// Where an input is: an archive's url, mirrors and sha256, or a git repository's commit and tag.
struct bump_location
{
    std::string url;
    std::vector<std::string> mirrors;
    std::string sha256;
    std::string git;
    std::string commit;
    std::string tag;
};

struct bump_input
{
    std::string name;          // "source", a resource's name, "source.verify-against"
    bool git = false;
    bump_status status = bump_status::unchanged;
    std::string message;       // why it is not bumped or failed; empty otherwise
    bump_location before;
    bump_location after;       // as before unless changed
};

// What the bump asks of the network. Each returns the sha256 of the file at `url` or the commit of the tag.
struct bump_fetchers
{
    std::function<result<std::string>(std::string const& url)> archive;  // kept in the cache for the build that follows
    std::function<result<std::string>(std::string const& url)> mirror;   // only compared
    std::function<result<std::string>(std::string const& git, std::string const& tag)> tag_commit;
};

struct bump_result
{
    std::string old_version;
    std::string new_version;
    std::int64_t old_revision = 0;
    std::vector<bump_input> inputs;
    std::vector<std::string> patches;  // [source] patches
    std::string text;                  // the new recipe.toml; empty when an error was reported
};

// Bumps the recipe.toml `content` (named `file` in messages) to `new_version`: [recipe] version, revision 1, and every
// input whose location holds the version, fetched anew through `fetchers`. Only the values it changes are rewritten; the
// rest of the text, comments and layout, stays. Errors and warnings go to `diags`; the result is there whenever the
// recipe.toml could be read (its inputs say what became of each), with `text` only when nothing was in error.
std::optional<bump_result> bump_recipe_text(std::string_view content, std::string const& file, std::string_view new_version,
    bump_fetchers const& fetchers, diagnostics& diags);

// The places of the files of `dir` (recipe.toml as `recipe_text`, the others as they are; files with a NUL byte left out)
// that still mention a spelling of `spellings`, as warnings with their file and line: comments, build.lua, keys that are
// no input. `old_version` is how messages name the version.
void warn_version_mentions(std::filesystem::path const& dir, std::string_view recipe_text, std::span<version_spelling const> spellings,
    std::string const& old_version, diagnostics& diags);

}
