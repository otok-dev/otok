// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "otok/core/error.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/fetch/locations.hpp"

// The cache directory (docs/formats/cache.md): downloads by content, unpacked sources, entries created
// under per-entry locks and published by an atomic rename.
namespace otok {

// The layout version this otok reads and writes (cache.md 2).
inline constexpr int cache_layout = 1;

struct cache_download
{
    std::string url;
    std::string sha256;  // 64 lowercase hex digits
    std::vector<std::string> mirrors{};  // other URLs of the file, tried after `url` (recipe-toml.md 4)
    std::string library{};              // whose input it is, for mirrors-only of some libraries; may be empty
};

// Messages about what the cache is doing; either may be empty.
struct cache_notices
{
    std::function<void(std::string const& what)> waiting{};   // for another process holding an entry's lock
    std::function<void(std::string const& what)> fetching{};  // before a download or a git fetch starts
};

// How a process uses the cache (cache.md 8): every otok process shares it, `otok clean` has it alone.
enum class cache_access
{
    shared,
    exclusive
};

// Parts of the cache `otok clean` removes (cache.md 8); none selected: all of them.
struct cache_parts
{
    bool downloads = false;   // downloads/
    bool sources = false;     // sources/ and git/
    bool builds = false;      // builds/ and failed/
};

struct cleaned_directory
{
    std::string name;         // "downloads", "git", …
    std::uintmax_t bytes = 0;
};

struct collected_garbage
{
    std::size_t entries = 0;
    std::uintmax_t bytes = 0;
};

class cache_store
{
public:
    // Opens the cache in `root`, creating it if needed: checks the layout, writes the marker files,
    // creates this process's tmp directory and removes those of processes that no longer run. Holds
    // `cache.lock` for as long as the store lives, shared or exclusive: an exclusive opener waits for
    // every other otok process to finish (with a waiting notice), and they wait for it.
    static result<cache_store> open(std::filesystem::path const& root, cache_notices notices = {},
        cache_access access = cache_access::shared);

    // Opens the cache for exclusive access when no other otok process uses it; nothing when one does.
    static result<std::optional<cache_store>> try_open_exclusive(std::filesystem::path const& root);

    // Whether the automatic garbage collection is due (cache.md 9): `gc-stamp` missing or more than a
    // day old.
    static bool garbage_collection_due(std::filesystem::path const& root);

    // Removes the selected parts of a cache opened with exclusive access, or with `dry_run` only
    // measures them. One result per directory present, with its size before removal.
    result<std::vector<cleaned_directory>> clean(cache_parts parts, bool dry_run);

    // Removes the entries of the selected parts unused for more than `days` days (0: every entry) from a
    // cache opened with exclusive access, or with `dry_run` only measures them; writes `gc-stamp`
    // unless `dry_run` (cache.md 9).
    result<collected_garbage> collect_garbage(cache_parts parts, std::int64_t days, bool dry_run);

    // Marks an entry as used now (cache.md 9): its modification time, when more than a day old.
    void mark_used(std::filesystem::path const& entry) const;

    std::filesystem::path const& root() const noexcept;
    std::filesystem::path const& tmp() const noexcept;  // this process's tmp directory

    // Offline (cache.md 6.1): nothing is downloaded or fetched; what is missing is an error.
    void set_offline(bool offline) noexcept;
    bool offline() const noexcept;

    // The locations of inputs a project and the machine add (otok-toml.md 10, config.md 2.1), tried as
    // cache.md 3.1 and 4 say.
    void set_mirrors(fetch_locations mirrors);

    std::filesystem::path download_path(std::string const& sha256) const;

    // Makes sure every file is in downloads/sha256/, downloading what is missing (several at once, the
    // locations of one file one after another) and checking each against its sha256. One result per
    // request, in order.
    std::vector<result<std::filesystem::path>> fetch(std::span<cache_download const> downloads, http_settings const& settings);

    // sources/archive-<sha256>/: the archive downloads/sha256/<sha256> (present already) unpacked.
    result<std::filesystem::path> unpacked_archive(std::string const& sha256);

    // sources/git-<commit>[-submodules]/ (cache.md 4, 5): the tree of `commit` of the repository at
    // `url`, fetched into git/<url key>/ when missing, with the bytes of the git objects (no line-ending
    // conversion, filters or export attributes). `tag` (may be empty) is the tag of the commit, fetched
    // first; a tag pointing elsewhere is an error. With `submodules`, every submodule is checked out at
    // its recorded commit, recursively. `library` names whose input it is (mirrors-only), may be empty.
    result<std::filesystem::path> git_tree(std::string const& url, std::string const& commit, std::string const& tag,
        bool submodules, git_tool const& git, std::string const& library = {});

    // sources/git-<commit>-<path hash>/ (cache.md 5): the directory `path` of `commit` alone, at the root of
    // the entry, for recipe trees; the whole tree of git_tree when `path` is empty.
    result<std::filesystem::path> git_directory(std::string const& url, std::string const& commit, std::string const& tag,
        std::string const& path, git_tool const& git, std::string const& library = {});

    // The entry `entry` (a path in the cache), created if missing (cache.md 6): `make` builds it in a
    // staging path under tmp(), which is then renamed to `entry`. `description` names it in wait notices.
    result<std::filesystem::path> ensure(std::filesystem::path const& entry, std::string const& description,
        std::function<result<void>(std::filesystem::path const& staging)> const& make);

private:
    // git/<url key>/ containing `commit`.
    result<std::filesystem::path> git_repository(std::string const& url, std::string const& commit, std::string const& tag,
        git_tool const& git, std::string const& library);

    // open and try_open_exclusive; without `wait`, nothing when cache.lock is held in a conflicting mode.
    static result<std::optional<cache_store>> open_with(std::filesystem::path const& root, cache_notices notices, cache_access access,
        bool wait);

    struct state;
    explicit cache_store(std::unique_ptr<state> s);
    std::shared_ptr<state> state_;
};

}
