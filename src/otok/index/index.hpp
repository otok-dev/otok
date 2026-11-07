// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "otok/cache/store.hpp"
#include "otok/core/diagnostics.hpp"
#include "otok/core/error.hpp"
#include "otok/core/names.hpp"
#include "otok/core/target_info.hpp"
#include "otok/lock/lock.hpp"
#include "otok/recipe/recipe.hpp"
#include "otok/resolver/provider.hpp"
#include "otok/version/version.hpp"

// The recipe index (docs/formats/index.md): version entries read as recipes, the projection of a
// recipe.toml, the repository read from a checkout, and the provider of `recipe = "publisher/name"`.
namespace otok {

// How the files of an index are read (formats README 1.5, docs/PLAN.md decision 98): `ci` strictly, by index CI and the
// commands that write or check an index; `client` by otok reading an index a newer otok may have written: entries it
// cannot read left aside (index.md 3.2), unknown platforms never matching, unknown keys of the mutable files ignored,
// advisories acting at least as much as they mean (7.2).
enum class index_reading
{
    client,
    ci
};

// The entry format this otok reads and writes (index.md 3).
inline constexpr std::int64_t index_entry_format = 1;

// A version entry (index.md 3).
struct index_entry
{
    recipe projection;                 // a recipe without products; `name` from `recipe`, `variant` always set
    recipe_id id;
    locked_location recipe_source;     // git: git, commit, tag, path
    std::string recipe_sha256;
    std::shared_ptr<toml::table const> table;  // the projection part of the entry (index.md 3.1)
    std::optional<toml::date_time> published;  // written by index CI when it merges the entry (index.md 3); none in a pull request

    // "<variant>/<version>-r<revision>.toml": its file in the recipe's directory
    std::string file() const;
};

// An entry a client cannot read (index.md 3.2), known by its file.
struct unreadable_entry
{
    std::string file;                  // "<variant>/<version>-r<revision>.toml"
    std::string reason;                // why: the first problem its reader found

    // "1.4-r1 (apple)"
    std::string label() const;
};

// The target of an entry's platforms (index.md 3: one); empty when this otok knows none of them.
std::string target_of_entry(recipe const& projection, std::span<target_info const* const> targets);

// The projection of a recipe.toml for `publisher` (index.md 3.1); `recipe_toml` is a valid recipe.
toml::table recipe_projection(toml::table const& recipe_toml, std::string_view publisher, std::span<target_info const* const> targets);

// Reads a version entry. `file` names it in messages. Empty result when there was at least one error.
std::optional<index_entry> parse_index_entry(std::string_view content, std::string const& file,
    std::span<target_info const* const> targets, diagnostics& diags, index_reading reading = index_reading::ci);

// The text of an entry: the projection with the publication keys (`published` in UTC, when given).
std::string write_index_entry(toml::table const& projection, locked_location const& recipe_source, std::string const& recipe_sha256,
    std::optional<toml::date_time> const& published);

// The entry `text` with `published` set (index.md 3: by index CI as it merges the entry, or by `otok publish` writing to a
// checkout), written as `write_index_entry` writes entries.
result<std::string> stamp_index_entry(std::string_view text, toml::date_time const& published);

enum class advisory_status
{
    under_review,
    confirmed,
    rejected
};

// An advisory (index.md 7).
struct advisory
{
    std::string id;
    std::vector<recipe_id> recipes;    // or
    std::string library;
    std::vector<std::string> variants; // with `recipes`: only these; empty: every variant
    std::vector<std::string> versions;
    advisory_severity severity = advisory_severity::broken;
    advisory_status status = advisory_status::under_review;
    std::string summary;
    std::vector<std::string> aliases;
    std::string details;               // Markdown
    std::vector<std::string> references;
    std::optional<toml::date_time> published;  // set in a successfully parsed advisory
    std::optional<toml::date_time> updated;
    // What a client took in place of values it does not know (index.md 7.2): "severity 'x' unknown to this otok, taken as 'security'".
    std::vector<std::string> unknown;

    bool active() const noexcept { return status != advisory_status::rejected; }
};

std::string_view severity_name(advisory_severity s) noexcept;

// Reads an advisory; `file` names it in messages. Empty result when there was at least one error.
std::optional<advisory> parse_advisory(std::string_view content, std::string const& file, diagnostics& diags,
    index_reading reading = index_reading::ci);

// Whether `a` concerns `version`-r`revision` of the variant `variant` of the recipe `id`, which builds `library`, its
// versions compared in `order` (index.md 7.1). Activity is not considered.
bool advisory_concerns(advisory const& a, recipe_id const& id, std::string_view variant, std::string_view library, version const& v,
    std::int64_t revision, version_order const& order);

// `recipe-status.toml` (index.md 5).
struct recipe_status
{
    std::string deprecated_reason;     // empty: not deprecated
    bool deprecated = false;
    std::optional<recipe_id> in_favor_of;
    std::optional<recipe_id> transferred_from;

    // "deprecated: <reason>; use '<id>' instead", or empty.
    std::string deprecation() const;
};

std::optional<recipe_status> parse_recipe_status(std::string_view content, std::string const& file, diagnostics& diags,
    index_reading reading = index_reading::ci);

// Reads a library file (index.md 4): the library's version order.
std::optional<version_order> parse_library_file(std::string_view content, std::string const& file, diagnostics& diags,
    index_reading reading = index_reading::ci);

// A publisher file (index.md 6).
struct publisher_file
{
    std::int64_t github_id = 0;
    bool organization = false;
    bool established = false;          // index CI merges its pull requests itself (index.md 9.1)
    std::string suspended;             // the reason; empty: may publish
};

std::optional<publisher_file> parse_publisher(std::string_view content, std::string const& file, diagnostics& diags,
    index_reading reading = index_reading::ci);

// The text of a publisher file (index.md 6): what `otok publish` adds to register a publisher.
std::string write_publisher(publisher_file const& p);

// The form of a publisher name two names that look alike share (index.md 9): lowercase, without `-`,
// `0`, `1`, `5` read as `o`, `l`, `s`.
std::string confusable_form(std::string_view name);

// `availability.toml` (index.md 11).
struct unavailable_version
{
    recipe_id recipe;
    std::string version;               // "<version>-r<revision>"
    toml::date_time since;
    std::vector<std::string> inputs;
};

struct input_availability
{
    std::optional<toml::date_time> checked;
    std::vector<unavailable_version> unavailable;

    unavailable_version const* find(recipe_id const& id, std::string_view version) const noexcept;
};

std::optional<input_availability> parse_availability(std::string_view content, std::string const& file, diagnostics& diags,
    index_reading reading = index_reading::ci);
std::string write_availability(input_availability const& a);

// The files a publication adds to an index (index.md 3, 4).
struct index_addition
{
    std::string label;                 // "p/a 1.0-r1 (apple)"
    std::string entry_path;            // "recipes/<publisher>/<name>/<variant>/<version>-r<revision>.toml"
    std::string entry_text;
    std::string library_path;          // "libraries/<library>.toml", added when the index has none
    std::string library_text;
};

// Checks the recipe tree `recipe_dir` for publication (the recipe, the limits of its tree as errors and
// what the targets require, target_info::check_publishable) and makes its entry, published by `publisher`
// from `recipe_source`; without `published`, as a pull request proposes it (index.md 3).
result<index_addition> make_index_entry(std::filesystem::path const& recipe_dir, locked_location const& recipe_source,
    std::string_view publisher, std::optional<toml::date_time> const& published, std::span<target_info const* const> targets);

// Whether the entry `existing` publishes the same as the entry `fresh` (DESIGN 7.4): the same projection and
// `recipe-sha256`; `recipe-source` (where the tree is now) and the time do not count.
// `file` names it in messages.
result<bool> same_index_entry(std::string_view existing, std::string_view fresh, std::string const& file,
    std::span<target_info const* const> targets);

enum class index_added
{
    written,
    already                            // the same entry is there (DESIGN 7.4)
};

// Adds an entry to the index checkout `index_dir`, with the library file when the library is new; a
// different entry of the same version and revision is an error.
result<index_added> add_index_entry(std::filesystem::path const& index_dir, index_addition const& addition,
    std::span<target_info const* const> targets);

// `index.toml` (index.md 2).
struct index_settings
{
    std::string name;
    std::string catalog;
    std::string view;
    std::string repository;            // the git repository; empty: not stated
    std::size_t established_entries_per_day = 20;
    std::size_t review_entries_per_day = 5;
};

std::optional<index_settings> parse_index_settings(std::string_view content, std::string const& file, diagnostics& diags,
    index_reading reading = index_reading::ci);

// The limit of new entries a publisher may have in 24 hours (index.md 9.2).
struct publication_limit
{
    std::string key = "review-entries-per-day";  // of index.toml
    std::size_t limit = 5;
    std::size_t recent = 0;            // the publisher's other entries published in the last 24 hours
    std::size_t pending = 0;           // its entries in other open pull requests
    std::vector<std::int64_t> pending_requests;  // their numbers
    std::size_t adding = 0;            // the new ones
    std::optional<std::chrono::sys_seconds> passes_at;  // now or later; nothing: not by waiting (too many at once, or pending)

    bool exceeded() const noexcept { return recent + pending + adding > limit; }
};

// The limit of an established publisher or of another, by the index's settings: `key` and `limit`.
publication_limit publication_limit_for(index_settings const& settings, bool established);

// `l` (its key, limit, pending entries and new ones set) with `recent` and `passes_at` for the publisher `publisher` in the
// checkout `root` at `now`; the entries of `skip` (their paths in the checkout, "recipes/p/a/linux/1.0-r1.toml") are the new
// ones, not counted as published before.
publication_limit publication_limit_of(std::filesystem::path const& root, std::string_view publisher, publication_limit l,
    std::chrono::sys_seconds now, std::set<std::string, std::less<>> const& skip = {});

// The publisher of a version entry's path in the index ("recipes/<publisher>/<name>/<variant>/<version>-r<revision>.toml"),
// or nothing when it is not one.
std::optional<std::string> entry_publisher(std::string_view path);

// What exceeding the limit means: "the publisher 'p' has 18 entries published in the last 24 hours, and these 3 make 21, more
// than 20 of the index (review-entries-per-day); they pass when published again after 2026-11-01 14:20 UTC".
std::string publication_limit_text(std::string_view publisher, publication_limit const& l);

// A file of the repository: its path in its directory (an entry's in the recipe's: "apple/1.0-r1.toml") and its text.
struct index_file
{
    std::string file;
    std::string text;
};

// The files of a recipe (index.md 3, 5).
struct index_recipe_files
{
    std::vector<index_file> entries;
    std::optional<std::string> status;  // recipe-status.toml
};

// A recipe and its newest stable version (the newest when all are pre-releases): `index/recipes.json` of
// the view (index.md 8).
struct recipe_summary
{
    recipe_id id;
    std::string library;
    std::string description;
    std::vector<std::string> keywords;
    std::string version;
    std::vector<std::string> targets;  // of its variants of that version, sorted
};

// Where an index is read from: a directory with the repository's files (index.md 8.1), or a view (8).
// Missing files are empty results.
class index_source
{
public:
    virtual ~index_source() = default;

    virtual result<std::optional<std::string>> settings() = 0;                                // index.toml
    virtual result<std::optional<index_recipe_files>> recipe(recipe_id const& id) = 0;
    virtual result<std::optional<std::string>> library(std::string_view library) = 0;         // libraries/<library>.toml
    virtual result<std::vector<index_file>> advisories() = 0;
    virtual result<std::optional<std::string>> availability() = 0;                            // availability.toml
    // The ids of every recipe, sorted.
    virtual result<std::vector<recipe_id>> recipe_ids() = 0;
    // The summaries of every recipe, sorted by id, when the source has them; else recipe_index makes them.
    virtual result<std::optional<std::vector<recipe_summary>>> summaries() { return std::nullopt; }
};

// The files of a checkout or a commit tree.
std::unique_ptr<index_source> directory_index_source(std::filesystem::path root);

// `base` with the entries (and the library files of new libraries) of `additions` as if they were published:
// what `otok publish` builds together resolves its dependencies on each other (DESIGN 7.4). Summaries are
// left to recipe_index.
std::unique_ptr<index_source> overlay_index_source(std::unique_ptr<index_source> base, std::vector<index_addition> additions);

// The newest stable entry of a list sorted newest first, or the newest when every version is a pre-release.
index_entry const* newest_stable(std::span<index_entry const* const> newest_first);

// The targets of the variants of a version among a recipe's entries, sorted (index.md 8: `targets` of `recipes.json`).
std::vector<std::string> version_targets(std::span<index_entry const* const> entries, version const& v, std::span<target_info const* const> targets);

// The index (index.md 1), read from a source.
class recipe_index
{
public:
    recipe_index(std::unique_ptr<index_source> source, std::span<target_info const* const> targets,
        index_reading reading = index_reading::client);
    // A checkout.
    recipe_index(std::filesystem::path root, std::span<target_info const* const> targets, index_reading reading = index_reading::client);

    // index.toml; a failure when the index has none.
    result<index_settings> settings();

    // The entries of a recipe, in no particular order; a failure when the index has no such recipe, when it has no entry
    // this otok reads, or (`ci`) when one of its entries is invalid.
    result<std::vector<index_entry> const*> entries(recipe_id const& id);

    // The entries of a recipe a client left aside (index.md 3.2), once `entries` has read them; sorted by file.
    std::vector<unreadable_entry> const& unreadable(recipe_id const& id) const;

    // Whether the index has the recipe, without reading its entries.
    result<bool> has(recipe_id const& id);

    // The entries of a recipe, newest first: the library's order, then the variant, then the revision.
    result<std::vector<index_entry const*>> newest_first(recipe_id const& id);

    // The order of a library's versions: from its library file, dotted when there is none.
    result<version_order> order(std::string_view library);

    // Every advisory of the index, sorted by id.
    result<std::vector<advisory> const*> advisories();

    // The advisories a client could not read (index.md 7.2), once `advisories` has read them: "<file>: <reason>".
    std::vector<std::string> const& unreadable_advisories() const noexcept { return unreadable_advisories_; }

    // The active advisories that concern a version of a variant of a recipe (index.md 7.1).
    result<std::vector<candidate_advisory>> advisories_of(recipe_id const& id, std::string_view variant, std::string_view library,
        version const& v, std::int64_t revision);

    // The status of a recipe; the default when it has no recipe-status.toml.
    result<recipe_status> status(recipe_id const& id);

    // The ids of every recipe, sorted.
    result<std::vector<recipe_id>> recipe_ids();

    // Every recipe with its newest stable version, sorted by id.
    result<std::vector<recipe_summary>> summaries();

    // availability.toml (index.md 11); empty when the index has none.
    result<input_availability const*> availability();

private:
    std::unique_ptr<index_source> source_;
    std::vector<target_info const*> targets_;
    index_reading reading_;
    std::map<std::string, std::vector<index_entry>, std::less<>> entries_;  // by recipe id
    std::map<std::string, std::vector<unreadable_entry>, std::less<>> unreadable_;  // by recipe id
    std::vector<std::string> unreadable_advisories_;
    std::map<std::string, recipe_status, std::less<>> statuses_;            // by recipe id
    std::optional<std::vector<advisory>> advisories_;
    std::optional<input_availability> availability_;
    std::map<std::string, version_order, std::less<>> orders_;              // by library
};

// An index of a project (otok-toml.md 6.4): `default`, the one of config.toml, or one of `[indexes]`.
struct project_index
{
    std::string name;
    std::function<result<std::unique_ptr<index_source>>()> open;  // called when the index is first needed
    std::vector<std::string> publishers;                          // owned: their ids are looked up here only
};

// The indexes of a project and where each recipe id is taken from (otok-toml.md 6.4, docs/PLAN.md decision 95).
class index_set
{
public:
    // `index_of`: `[index-of]`, recipe id → index name. `none` says why there is no index at all when `indexes` is
    // empty or an id names `default` and it is not among them ("'index' in config.toml is empty").
    index_set(std::vector<project_index> indexes, std::map<std::string, std::string, std::less<>> index_of, std::string none,
        std::span<target_info const* const> targets);

    struct located
    {
        std::string name;                  // "default" or of [indexes]
        recipe_index* index = nullptr;
    };

    // The index the recipe `id` is taken from: the one `[index-of]` names, else the one owning its publisher, else the
    // only one that has it; a failure when none has it or several do.
    result<located> locate(recipe_id const& id);

    // An index by its name, opened when first asked for.
    result<recipe_index*> get(std::string_view name);

    // The names of the indexes, `default` first when it is one of them.
    std::vector<std::string> names() const;

    // The indexes opened so far, by name.
    std::vector<std::pair<std::string, recipe_index*>> opened();

private:
    std::vector<project_index> indexes_;
    std::map<std::string, std::string, std::less<>> index_of_;
    std::string none_;
    std::vector<target_info const*> targets_;
    std::map<std::string, std::optional<result<recipe_index>>, std::less<>> opened_;
};

// "the index 'corp'", or "the index" for `default`, in messages.
std::string index_label(std::string_view name);

// The directory to read the index from (index.md 8.1): `mirror` when it is a directory, else the tree
// of the `HEAD` of the repository at `url`, fetched into the cache (offline: the commit last read).
result<std::filesystem::path> index_checkout(std::string const& url, std::string const& mirror, cache_store& cache, git_tool const* git);

// The recipe tree an entry pins, fetched into the cache and checked against its `recipe-sha256`. `label` names
// the version in messages ("p/a 1.0 (r1)"), `library` is the library it builds (mirrors-only).
result<std::filesystem::path> fetch_recipe_tree(std::string const& label, std::string const& library, locked_location const& recipe_source,
    std::string const& recipe_sha256, cache_store& cache, git_tool const& git);

// Recipes from the indexes of the project, other recipes from `local`.
class index_recipe_provider : public recipe_provider
{
public:
    index_recipe_provider(recipe_provider& local, std::shared_ptr<index_set> indexes, std::span<target_info const* const> targets,
        cache_store* cache, std::optional<git_tool> git);

    result<std::vector<candidate>> candidates(std::string_view library, recipe_ref const& ref) override;
    // From the index of the library's recipe when it is from one; the others' as `local` says.
    version_order order(std::string_view library) override;
    result<void> prepare(candidate& c) override;
    std::vector<std::string> unreadable(std::string_view library) override;

private:
    recipe_provider& local_;
    std::shared_ptr<index_set> indexes_;
    std::vector<target_info const*> targets_;
    cache_store* cache_;
    std::optional<git_tool> git_;
    std::map<std::string, std::string, std::less<>> from_index_;          // library → the name of its index
    std::map<std::string, recipe_id, std::less<>> ids_;                   // library → its recipe of the index
    std::map<std::string, std::shared_ptr<toml::table const>, std::less<>> tables_;  // "<id> <variant> <version> <revision>"
};

}
