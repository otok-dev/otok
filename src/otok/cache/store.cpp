// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/cache/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <set>

#include "otok/core/executor.hpp"
#include "otok/core/file_lock.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/process.hpp"
#include "otok/core/sha256.hpp"
#include "otok/fetch/archive.hpp"
#include "otok/fetch/git.hpp"

namespace otok {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view cachedir_tag =
    "Signature: 8a477f597d28d172789f06886806bc55\n"
    "# This file is a cache directory tag created by otok.\n"
    "# For information about cache directory tags see https://bford.info/cachedir/\n";

// Names a layout-1 cache may contain at its top (cache.md 1).
bool is_layout_name(std::string const& name)
{
    static std::set<std::string, std::less<>> const names{
        "CACHEDIR.TAG", "layout", "downloads", "git", "index", "view", "sources", "builds", "dev", "projects", "failed", "tmp", "gc-stamp"};
    return names.contains(name) || name.ends_with(".lock") || name.ends_with(".tmp");
}

result<void> check_layout(fs::path const& root)
{
    std::error_code ec;
    fs::path const file = root / "layout";
    if (fs::exists(file, ec)) {
        auto text = read_file(file);
        if (!text) return std::unexpected{text.error()};
        int version = 0;
        if (std::sscanf(text->c_str(), "%d", &version) != 1 || version < 1) {
            return fail("'" + path_to_utf8(file) + "' is not a valid layout file");
        }
        if (version > cache_layout) {
            return fail("the cache '" + path_to_utf8(root) + "' has layout " + std::to_string(version) + ", which is newer than this otok knows ("
                + std::to_string(cache_layout) + "); use a newer otok, or another cache-dir");
        }
        return {};
    }
    for (fs::directory_entry const& e : fs::directory_iterator{root, ec}) {
        std::string const name = path_to_utf8(e.path().filename());
        if (!is_layout_name(name)) {
            return fail("'" + path_to_utf8(root) + "' is not empty and does not look like an otok cache (it contains '" + name
                + "'); set cache-dir to another directory");
        }
    }
    if (ec) return fail("cannot read '" + path_to_utf8(root) + "': " + ec.message());
    return write_file_atomic(file, std::to_string(cache_layout) + "\n");
}

std::string random_suffix()
{
    std::random_device rd;
    std::array<char, 9> hex;
    std::snprintf(hex.data(), hex.size(), "%08x", static_cast<unsigned>(rd()));
    return hex.data();
}

// The size of the files of a directory tree, or of a file.
result<std::uintmax_t> tree_size(fs::path const& path)
{
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) return fs::file_size(path, ec);
    std::uintmax_t bytes = 0;
    for (auto it = fs::recursive_directory_iterator{long_path(path), ec}; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (it->is_regular_file(ec)) bytes += it->file_size(ec);
    }
    if (ec) return fail("cannot read '" + path_to_utf8(path) + "': " + ec.message());
    return bytes;
}

// Entries are marked used, and garbage collection is due, when their time is older than this (cache.md 9).
constexpr std::chrono::hours a_day{24};

// Whether a file was written within the last minute: a tmp directory or lock being made, whose process may not hold the
// lock yet (it creates the lock file, then locks it, then creates the directory), so not one to remove.
bool just_made(fs::path const& path)
{
    std::error_code ec;
    auto const written = fs::last_write_time(path, ec);
    return !ec && fs::file_time_type::clock::now() - written < std::chrono::minutes{1};
}

// Removes tmp directories of processes that no longer run (cache.md 7): their lock can be taken. Those made within the
// last minute are left: another cache_store, of this process (the builds of `otok publish` open one each, at once) or of
// another, may be between creating its lock file and locking it, and removing the file would orphan its directory.
void remove_stale_tmp(fs::path const& tmp_root, std::string const& own)
{
    std::error_code ec;
    std::set<std::string> locked, dirs;
    for (fs::directory_entry const& e : fs::directory_iterator{tmp_root, ec}) {
        std::string const name = path_to_utf8(e.path().filename());
        if (name == own || name == own + ".lock") continue;
        if (name.ends_with(".lock")) {
            locked.insert(name.substr(0, name.size() - 5));
        } else {
            dirs.insert(name);
        }
    }
    for (std::string const& name : locked) {
        fs::path const lock_file = tmp_root / (name + ".lock");
        if (just_made(lock_file)) {
            dirs.erase(name);
            continue;
        }
        auto lock = file_lock::try_acquire(lock_file);
        if (!lock || !*lock) {
            dirs.erase(name);
            continue;  // still running
        }
        fs::remove_all(long_path(tmp_root / name), ec);
        (*lock)->release();
        fs::remove(lock_file, ec);
        dirs.erase(name);
    }
    // a directory without a lock: its process takes the lock before creating it, so it is gone
    for (std::string const& name : dirs) {
        if (!just_made(tmp_root / name)) fs::remove_all(long_path(tmp_root / name), ec);
    }
}

}

struct cache_store::state
{
    fs::path root;
    fs::path tmp;
    std::optional<file_lock> use_lock;  // cache.lock, released last (declared before the others)
    std::optional<file_lock> tmp_lock;
    cache_notices notices;
    cache_access access = cache_access::shared;
    bool offline = false;
    fetch_locations mirrors;

    void waiting(std::string const& what) const
    {
        if (notices.waiting) notices.waiting("waiting for another otok process: " + what);
    }

    void fetching(std::string const& what) const
    {
        if (notices.fetching) notices.fetching(what);
    }

    ~state()
    {
        std::error_code ec;
        if (!tmp.empty()) fs::remove_all(long_path(tmp), ec);
        if (tmp_lock) {
            tmp_lock->release();
            fs::path lock_file = tmp;
            lock_file += ".lock";
            fs::remove(lock_file, ec);
        }
    }
};

cache_store::cache_store(std::unique_ptr<state> s) : state_{std::move(s)} {}

result<cache_store> cache_store::open(fs::path const& root, cache_notices notices, cache_access access)
{
    auto store = open_with(root, std::move(notices), access, true);
    if (!store) return std::unexpected{store.error()};
    return std::move(**store);
}

result<std::optional<cache_store>> cache_store::try_open_exclusive(fs::path const& root)
{
    return open_with(root, {}, cache_access::exclusive, false);
}

result<std::optional<cache_store>> cache_store::open_with(fs::path const& root, cache_notices notices, cache_access access, bool wait)
{
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) return fail("cannot create the cache '" + path_to_utf8(root) + "': " + ec.message());

    // otok processes share the cache; `otok clean` has it alone (cache.md 8)
    lock_mode const mode = access == cache_access::shared ? lock_mode::shared : lock_mode::exclusive;
    std::optional<file_lock> use;
    if (wait) {
        auto taken = file_lock::acquire(root / "cache.lock", [&] {
            if (!notices.waiting) return;
            notices.waiting(access == cache_access::shared ? "waiting for 'otok clean' to finish"
                                                           : "waiting for other otok processes to finish with the cache");
        }, mode);
        if (!taken) return std::unexpected{taken.error()};
        use = std::move(*taken);
    } else {
        auto taken = file_lock::try_acquire(root / "cache.lock", mode);
        if (!taken) return std::unexpected{taken.error()};
        if (!*taken) return std::optional<cache_store>{};
        use = std::move(**taken);
    }

    // Setting up the markers is serialized between processes.
    auto setup = file_lock::acquire(root / "layout.lock");
    if (!setup) return std::unexpected{setup.error()};
    if (auto ok = check_layout(root); !ok) return std::unexpected{ok.error()};
    if (!fs::exists(root / "CACHEDIR.TAG", ec)) {
        if (auto ok = write_file_atomic(root / "CACHEDIR.TAG", cachedir_tag); !ok) return std::unexpected{ok.error()};
    }
    setup->release();

    auto s = std::make_unique<state>();
    s->root = root;
    s->notices = std::move(notices);
    s->access = access;
    s->use_lock = std::move(*use);
    fs::path const tmp_root = root / "tmp";
    fs::create_directories(tmp_root, ec);
    if (ec) return fail("cannot create '" + path_to_utf8(tmp_root) + "': " + ec.message());
    std::string const own = std::to_string(current_process_id()) + "-" + random_suffix();
    auto lock = file_lock::acquire(tmp_root / (own + ".lock"));
    if (!lock) return std::unexpected{lock.error()};
    s->tmp_lock = std::move(*lock);
    s->tmp = tmp_root / own;
    if (!fs::create_directory(s->tmp, ec)) return fail("cannot create '" + path_to_utf8(s->tmp) + "': " + ec.message());
    remove_stale_tmp(tmp_root, own);
    return std::optional<cache_store>{cache_store{std::move(s)}};
}

fs::path const& cache_store::root() const noexcept
{
    return state_->root;
}

fs::path const& cache_store::tmp() const noexcept
{
    return state_->tmp;
}

void cache_store::set_offline(bool offline) noexcept
{
    state_->offline = offline;
}

bool cache_store::offline() const noexcept
{
    return state_->offline;
}

void cache_store::set_mirrors(fetch_locations mirrors)
{
    state_->mirrors = std::move(mirrors);
}

fs::path cache_store::download_path(std::string const& sha256) const
{
    return state_->root / "downloads" / "sha256" / sha256;
}

result<fs::path> cache_store::ensure(fs::path const& entry, std::string const& description,
    std::function<result<void>(fs::path const& staging)> const& make)
{
    std::error_code ec;
    if (fs::exists(entry, ec)) {  // entries appear atomically and never change
        mark_used(entry);
        return entry;
    }
    fs::create_directories(entry.parent_path(), ec);
    if (ec) return fail("cannot create '" + path_to_utf8(entry.parent_path()) + "': " + ec.message());

    fs::path lock_file = entry;
    lock_file += ".lock";
    // a build waiting for another one lends its job meanwhile (docs/PLAN.md decision 60)
    std::optional<lent_job> lent;
    auto lock = file_lock::acquire(lock_file, [&] {
        state_->waiting(description);
        lent.emplace("another build");
    });
    lent.reset();
    if (!lock) return std::unexpected{lock.error()};
    if (fs::exists(entry, ec)) {  // created by the holder we waited for
        mark_used(entry);
        return entry;
    }

    fs::path const staging = state_->tmp / entry.filename();
    fs::remove_all(long_path(staging), ec);
    if (auto ok = make(staging); !ok) {
        fs::remove_all(long_path(staging), ec);
        return std::unexpected{ok.error()};
    }
    // its last use is now (cache.md 9), whatever time unpacking gave it (an archive's, a commit's)
    fs::last_write_time(staging, fs::file_time_type::clock::now(), ec);
    rename_path(staging, entry, ec);
    if (ec) {
        std::string const reason = ec.message();
        fs::remove_all(long_path(staging), ec);
        return fail("cannot move '" + path_to_utf8(staging) + "' to '" + path_to_utf8(entry) + "': " + reason);
    }
    return entry;
}

result<void> cache_store::discard(fs::path const& entry, std::string const& description, std::function<bool(fs::path const& entry)> const& damaged)
{
    fs::path lock_file = entry;
    lock_file += ".lock";
    std::optional<lent_job> lent;
    auto lock = file_lock::acquire(lock_file, [&] {
        state_->waiting(description);
        lent.emplace("another build");
    });
    lent.reset();
    if (!lock) return std::unexpected{lock.error()};
    std::error_code ec;
    if (!fs::exists(entry, ec) || !damaged(entry)) return {};
    fs::path aside = state_->tmp / entry.filename();
    aside += "-damaged";
    fs::remove_all(long_path(aside), ec);
    rename_path(entry, aside, ec);
    if (ec) {
        return fail("the damaged cache entry '" + path_to_utf8(entry) + "' cannot be removed (" + ec.message()
            + "): remove it by hand, or run 'otok clean --builds'");
    }
    fs::remove_all(long_path(aside), ec);
    return {};
}

std::string cache_store::project_key(fs::path const& project_dir)
{
    return sha256_hex(path_to_utf8(identity_path(project_dir))).substr(0, 32);
}

result<file_lock> cache_store::hold_project(fs::path const& project_dir, bool exclusive)
{
    std::error_code ec;
    std::string const key = project_key(project_dir);
    fs::path const dir = state_->root / "projects";
    fs::create_directories(dir, ec);
    if (ec) return fail("cannot create '" + path_to_utf8(dir) + "': " + ec.message());
    fs::path const owner = dir / (key + ".owner");
    auto lock = file_lock::acquire(dir / (key + ".lock"), [&] {
        std::string holder = read_file(owner).value_or("");
        while (!holder.empty() && (holder.back() == '\n' || holder.back() == '\r')) holder.pop_back();
        state_->waiting("working in the project '" + path_to_utf8(project_dir) + "'" + (holder.empty() ? std::string{} : " (PID " + holder + ")"));
    }, exclusive ? lock_mode::exclusive : lock_mode::shared);
    if (!lock) return std::unexpected{lock.error()};
    // for whoever waits next; not needed to work
    (void)write_file_atomic(owner, std::to_string(current_process_id()) + "\n");
    return lock;
}

std::vector<result<fs::path>> cache_store::fetch(std::span<cache_download const> downloads, http_settings const& settings)
{
    std::vector<std::optional<result<fs::path>>> results(downloads.size());
    std::error_code ec;

    // Missing files, each once; their locks are taken in sorted order, so two processes fetching
    // overlapping sets cannot deadlock.
    std::map<std::string, std::vector<std::size_t>> missing;
    for (std::size_t i = 0; i < downloads.size(); ++i) {
        fs::path const path = download_path(downloads[i].sha256);
        if (fs::exists(path, ec)) {
            mark_used(path);
            results[i] = path;
        } else {
            missing[downloads[i].sha256].push_back(i);
        }
    }
    if (!missing.empty()) fs::create_directories(state_->root / "downloads" / "sha256", ec);

    // a missing file being downloaded: its locations (cache.md 3.1), the next to try, what went wrong
    struct pending
    {
        std::string sha256;
        std::vector<std::string> locations;
        std::size_t next = 0;
        std::vector<std::string> errors{};
    };
    std::vector<file_lock> locks;
    std::vector<pending> files;
    for (auto const& [sha256, indices] : missing) {
        fs::path const path = download_path(sha256);
        fs::path lock_file = path;
        lock_file += ".lock";
        cache_download const& d = downloads[indices.front()];
        if (state_->offline) {
            for (std::size_t i : indices) results[i] = fail(d.url + " is not in the cache (--offline)");
            continue;
        }
        std::vector<std::string> locations = archive_locations(sha256, d.url, d.mirrors, state_->mirrors, d.library);
        if (locations.empty()) {
            for (std::size_t i : indices) results[i] = fail("no asset-mirror or url-rewrite covers " + d.url + ", and mirrors-only is set");
            continue;
        }
        auto lock = file_lock::acquire(lock_file, [&] { state_->waiting("downloading " + d.url); });
        if (!lock) {
            for (std::size_t i : indices) results[i] = std::unexpected{lock.error()};
            continue;
        }
        locks.push_back(std::move(*lock));
        if (fs::exists(path, ec)) {
            for (std::size_t i : indices) results[i] = path;
            continue;
        }
        files.push_back({sha256, std::move(locations)});
    }

    // rounds: every file not yet downloaded tries its next location, all files at once
    for (;;) {
        std::vector<download_request> requests;
        std::vector<pending*> requested;
        for (pending& f : files) {
            if (f.next >= f.locations.size()) continue;
            std::string const& location = f.locations[f.next++];
            state_->fetching("downloading " + location);
            requests.push_back({location, state_->tmp / f.sha256, f.sha256});
            requested.push_back(&f);
        }
        if (requests.empty()) break;
        auto const fetched = download_all(requests, settings);
        for (std::size_t k = 0; k < requests.size(); ++k) {
            pending& f = *requested[k];
            std::vector<std::size_t> const& indices = missing.at(f.sha256);
            if (!fetched[k]) {
                f.errors.push_back(fetched[k].error().message);
                if (f.next < f.locations.size()) continue;
                std::string message = f.errors.front();
                if (f.errors.size() > 1) {
                    message = "no location of " + downloads[indices.front()].url + " gave the file:";
                    for (std::string const& e : f.errors) {
                        message += "\n  ";
                        for (char const c : e) message += c == '\n' ? std::string{"\n    "} : std::string{c};
                    }
                }
                for (std::size_t i : indices) results[i] = fail(message);
                continue;
            }
            f.next = f.locations.size();  // done
            fs::path const path = download_path(f.sha256);
            rename_path(requests[k].file, path, ec);
            if (ec) {
                result<fs::path> const failure = fail("cannot move the download of " + requests[k].url + " into the cache: " + ec.message());
                for (std::size_t i : indices) results[i] = failure;
                fs::remove(requests[k].file, ec);
                continue;
            }
            for (std::size_t i : indices) results[i] = path;
        }
    }
    locks.clear();

    std::vector<result<fs::path>> out;
    out.reserve(results.size());
    for (auto& r : results) out.push_back(std::move(*r));
    return out;
}

result<std::vector<cleaned_directory>> cache_store::clean(cache_parts parts, bool dry_run)
{
    if (state_->access != cache_access::exclusive) return fail("cleaning needs the cache opened for exclusive access");
    bool const all = !parts.downloads && !parts.sources && !parts.builds;
    std::vector<std::string> names;
    if (all || parts.downloads) names.emplace_back("downloads");
    if (all || parts.sources) {
        names.emplace_back("git");
        names.emplace_back("index");  // the commits of git indexes last read (cache.md 10)
        names.emplace_back("view");   // the files of index views (cache.md 11)
        names.emplace_back("sources");
    }
    if (all || parts.builds) {
        names.emplace_back("builds");
        names.emplace_back("failed");  // kept work directories of failed builds (keep-build-dirs)
        names.emplace_back("dev");     // the work directories of dev slices (otok-dev-toml.md 4)
    }

    std::vector<cleaned_directory> cleaned;
    for (std::string const& name : names) {
        fs::path const dir = state_->root / name;
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        auto bytes = tree_size(dir);
        if (!bytes) return std::unexpected{bytes.error()};
        cleaned.push_back({name, *bytes});
        if (dry_run) continue;
        // out of the way first: an interrupted clean leaves the rest in tmp/, removed by the next otok
        fs::path const trash = state_->tmp / ("clean-" + name);
        rename_path(dir, trash, ec);
        if (ec) return fail("cannot move '" + path_to_utf8(dir) + "' out of the cache: " + ec.message());
        fs::remove_all(long_path(trash), ec);
        if (ec) return fail("cannot remove '" + path_to_utf8(trash) + "': " + ec.message());
    }
    return cleaned;
}

result<collected_garbage> cache_store::collect_garbage(cache_parts parts, std::int64_t days, bool dry_run)
{
    if (state_->access != cache_access::exclusive) return fail("garbage collection needs the cache opened for exclusive access");
    bool const all = !parts.downloads && !parts.sources && !parts.builds;
    fs::path const& root = state_->root;
    std::error_code ec;

    // the entries of cache.md 1 and the kept work directories, without their lock files
    std::vector<fs::path> entries;
    auto const children = [&](fs::path const& dir, bool directories_only) {
        std::vector<fs::path> found;
        for (fs::directory_entry const& e : fs::directory_iterator{dir, ec}) {
            if (e.path().extension() == ".lock" || (directories_only && !e.is_directory(ec))) continue;
            found.push_back(e.path());
        }
        ec.clear();  // a part that does not exist has nothing to collect
        return found;
    };
    auto const add = [&](std::vector<fs::path> const& found) { entries.insert(entries.end(), found.begin(), found.end()); };
    if (all || parts.downloads) add(children(root / "downloads" / "sha256", false));
    if (all || parts.sources) {
        add(children(root / "git", true));
        add(children(root / "sources", true));
    }
    std::vector<fs::path> build_dirs;  // removed when left empty, deepest first
    if (all || parts.builds) {
        for (fs::path const& library : children(root / "builds", true)) {
            for (fs::path const& version : children(library, true)) {
                for (char const* kind : {"slices", "sources", "packages"}) {
                    add(children(version / kind, true));
                    build_dirs.push_back(version / kind);
                }
                build_dirs.push_back(version);
            }
            build_dirs.push_back(library);
        }
        add(children(root / "failed", true));
        // the work directories of dev slices, aged by their last build (otok-dev-toml.md 4)
        for (fs::path const& project : children(root / "dev", true)) {
            add(children(project, true));
            build_dirs.push_back(project);
        }
    }

    auto const now = fs::file_time_type::clock::now();
    collected_garbage out;
    for (fs::path const& entry : entries) {
        auto const used = fs::last_write_time(entry, ec);
        if (ec) return fail("cannot read the time of '" + path_to_utf8(entry) + "': " + ec.message());
        if (days > 0 && now - used <= a_day * std::min<std::int64_t>(days, 100 * 365)) continue;
        auto bytes = tree_size(entry);
        if (!bytes) return std::unexpected{bytes.error()};
        ++out.entries;
        out.bytes += *bytes;
        if (dry_run) continue;
        // out of the way first, as clean does
        fs::path const trash = state_->tmp / ("gc-" + std::to_string(out.entries));
        rename_path(entry, trash, ec);
        if (ec) return fail("cannot move '" + path_to_utf8(entry) + "' out of the cache: " + ec.message());
        fs::remove_all(long_path(trash), ec);
        if (ec) return fail("cannot remove '" + path_to_utf8(trash) + "': " + ec.message());
        fs::path lock_file = entry;
        lock_file += ".lock";
        fs::remove(lock_file, ec);  // no other process can hold it: the cache is ours alone
    }
    if (dry_run) return out;
    for (fs::path const& dir : build_dirs) {
        if (fs::is_empty(dir, ec)) fs::remove(dir, ec);
    }
    if (auto ok = write_file_atomic(root / "gc-stamp", ""); !ok) return std::unexpected{ok.error()};
    return out;
}

void cache_store::mark_used(fs::path const& entry) const
{
    std::error_code ec;
    auto const now = fs::file_time_type::clock::now();
    auto const used = fs::last_write_time(entry, ec);
    if (ec || now - used < a_day) return;
    fs::last_write_time(entry, now, ec);  // a read-only cache is still usable
}

bool cache_store::garbage_collection_due(fs::path const& root)
{
    std::error_code ec;
    auto const last = fs::last_write_time(root / "gc-stamp", ec);
    return ec || fs::file_time_type::clock::now() - last >= a_day;
}

result<fs::path> cache_store::unpacked_archive(std::string const& sha256)
{
    fs::path const archive_file = download_path(sha256);
    return ensure(state_->root / "sources" / ("archive-" + sha256), "unpacking " + sha256,
        [&](fs::path const& staging) { return extract_archive(archive_file, staging); });
}

result<fs::path> cache_store::git_repository(std::string const& url, std::string const& commit, std::string const& tag,
    git_tool const& git, std::string const& library)
{
    fs::path const repo = state_->root / "git" / git_url_key(url);
    fs::path const log = state_->tmp / "git.log";
    std::string const git_dir = "--git-dir=" + path_to_utf8(repo);
    std::string const offline_miss = "commit " + commit + " of " + url + " is not in the cache (--offline)";
    std::error_code ec;
    if (state_->offline && !fs::exists(repo, ec)) return fail(offline_miss);

    auto created = ensure(repo, "creating the repository of " + url, [&](fs::path const& staging) -> result<void> {
        if (auto ok = run_git(git, {"init", "--bare", "-q", path_to_utf8(staging)}, state_->tmp, log); !ok) return std::unexpected{ok.error()};
        // `git archive` takes these attributes over the tree's .gitattributes: the exported files are
        // the bytes of the git objects (recipe-hash.md 2), whatever the repository asks for.
        return write_file_atomic(staging / "info" / "attributes",
            "* -text -eol -filter -ident -working-tree-encoding -export-ignore -export-subst\n");
    });
    if (!created) return std::unexpected{created.error()};

    fs::path lock_file = repo;
    lock_file += ".lock";
    auto lock = file_lock::acquire(lock_file, [&] { state_->waiting("fetching " + url); });
    if (!lock) return std::unexpected{lock.error()};

    auto const present = [&] { return run_git(git, {git_dir, "cat-file", "-e", commit + "^{commit}"}, state_->tmp, log).has_value(); };
    if (present()) return repo;
    if (state_->offline) return fail(offline_miss);

    // the commit from one location of the repository
    auto const fetch_from = [&](std::string const& from) -> result<void> {
        state_->fetching("fetching " + from + " (" + (tag.empty() ? commit : tag) + ")");
        // The tag first (cache.md 4): every server serves its tags. A moved tag is an error; a deleted
        // one leaves the fetch by hash.
        if (!tag.empty()) {
            std::string const ref = "refs/otok/tags/" + tag;
            if (run_git(git, {git_dir, "fetch", "-q", "--depth", "1", "--no-tags", from, "+refs/tags/" + tag + ":" + ref}, state_->tmp, log)) {
                auto points = run_git(git, {git_dir, "rev-parse", ref + "^{commit}"}, state_->tmp, log, true);
                std::string target = points.value_or("");
                while (!target.empty() && (target.back() == '\n' || target.back() == '\r')) target.pop_back();
                if (target != commit) {
                    return fail("the tag '" + tag + "' of " + from + " points to " + target + ", not to " + commit
                        + "; it was moved upstream, or the recipe names the wrong commit");
                }
                if (present()) return {};
            }
        }
        // One commit by its hash; a server that refuses unadvertised commits gets a fetch of its
        // branches and tags instead. The ref keeps the commit from being pruned.
        auto fetched = run_git(git, {git_dir, "fetch", "-q", "--depth", "1", "--no-tags", from, "+" + commit + ":refs/otok/" + commit}, state_->tmp, log);
        if (!fetched || !present()) {
            auto all = run_git(git, {git_dir, "fetch", "-q", "--no-tags", from, "+refs/heads/*:refs/otok/heads/*", "+refs/tags/*:refs/otok/tags/*"},
                state_->tmp, log);
            if (!all) return fail("cannot fetch commit " + commit + " from " + from + ": " + (fetched ? all.error().message : fetched.error().message));
            if (!present()) return fail("the repository " + from + " has no commit " + commit);
        }
        return {};
    };
    std::vector<std::string> const locations = git_locations(url, state_->mirrors, library);
    if (locations.empty()) return fail("no url-rewrite covers " + url + ", and mirrors-only is set");
    std::vector<std::string> errors;
    for (std::string const& from : locations) {
        auto ok = fetch_from(from);
        if (ok) return repo;
        errors.push_back(ok.error().message);
    }
    if (errors.size() == 1) return fail(errors.front());
    std::string message = "no location of " + url + " gave commit " + commit + ":";
    for (std::string const& e : errors) message += "\n  " + e;
    return fail(message);
}

result<fs::path> cache_store::git_tree(std::string const& url, std::string const& commit, std::string const& tag, bool submodules,
    git_tool const& git, std::string const& library)
{
    std::string const name = "git-" + commit + (submodules ? "-submodules" : "");
    return ensure(state_->root / "sources" / name, "exporting " + commit + " of " + url, [&](fs::path const& staging) -> result<void> {
        auto repo = git_repository(url, commit, tag, git, library);
        if (!repo) return std::unexpected{repo.error()};
        fs::path const log = state_->tmp / "git.log";
        std::string const git_dir = "--git-dir=" + path_to_utf8(*repo);
        fs::path const tar = state_->tmp / (name + ".tar");
        if (auto ok = run_git(git, {git_dir, "archive", "--format=tar", "-o", path_to_utf8(tar), commit}, state_->tmp, log); !ok) {
            return std::unexpected{ok.error()};
        }
        auto extracted = extract_archive(tar, staging, top_level::keep);
        std::error_code ec;
        fs::remove(tar, ec);
        if (!extracted) return extracted;
        fs::create_directories(staging, ec);  // an empty tree gives an empty archive
        if (!submodules || !fs::exists(staging / ".gitmodules", ec)) return {};

        // submodule.<name>.path / .url from .gitmodules
        auto config = run_git(git, {"config", "-f", path_to_utf8(staging / ".gitmodules"), "--get-regexp", "^submodule\\..*\\.(path|url)$"},
            state_->tmp, log, true);
        if (!config) return std::unexpected{config.error()};
        std::map<std::string, std::pair<std::string, std::string>> modules;  // name → (path, url)
        std::size_t start = 0;
        for (std::size_t end; (end = config->find('\n', start)) != std::string::npos; start = end + 1) {
            std::string const line = config->substr(start, end - start);
            std::size_t const space = line.find(' ');
            std::size_t const dot = line.rfind('.', space);
            if (space == std::string::npos || dot == std::string::npos || dot < 10) continue;
            std::string const module = line.substr(10, dot - 10);  // after "submodule."
            std::string const key = line.substr(dot + 1, space - dot - 1);
            (key == "path" ? modules[module].first : modules[module].second) = line.substr(space + 1);
        }
        for (auto const& [module, location] : modules) {
            auto const& [path, sub_url] = location;
            if (path.empty() || sub_url.empty()) continue;
            if (classify_file_path(path) != file_path_kind::inside) return fail("submodule '" + module + "' has the invalid path '" + path + "'");
            // "160000 commit <sha>\t<path>": the commit the superproject records
            auto entry = run_git(git, {git_dir, "ls-tree", commit, "--", path}, state_->tmp, log, true);
            if (!entry) return std::unexpected{entry.error()};
            if (!entry->starts_with("160000 commit ")) continue;  // listed in .gitmodules but not a submodule here
            std::string const sub_commit = entry->substr(14, 40);
            auto subtree = git_tree(resolve_submodule_url(url, sub_url), sub_commit, {}, true, git, library);
            if (!subtree) return fail("submodule '" + module + "': " + subtree.error().message);
            fs::path const target = staging / path_from_utf8(path);
            fs::remove_all(long_path(target), ec);
            if (auto ok = copy_tree(*subtree, target); !ok) return ok;
        }
        return {};
    });
}

result<fs::path> cache_store::git_directory(std::string const& url, std::string const& commit, std::string const& tag,
    std::string const& path, git_tool const& git, std::string const& library)
{
    if (path.empty()) return git_tree(url, commit, tag, false, git, library);
    std::string const name = "git-" + commit + "-" + sha256_hex(path).substr(0, 16);
    return ensure(state_->root / "sources" / name, "exporting '" + path + "' of " + commit + " of " + url, [&](fs::path const& staging) -> result<void> {
        auto repo = git_repository(url, commit, tag, git, library);
        if (!repo) return std::unexpected{repo.error()};
        fs::path const log = state_->tmp / "git.log";
        fs::path const tar = state_->tmp / (name + ".tar");
        auto archived = run_git(git, {"--git-dir=" + path_to_utf8(*repo), "archive", "--format=tar", "-o", path_to_utf8(tar), commit + ":" + path},
            state_->tmp, log);
        if (!archived) return fail("the commit " + commit + " of " + url + " has no directory '" + path + "': " + archived.error().message);
        auto extracted = extract_archive(tar, staging, top_level::keep);
        std::error_code ec;
        fs::remove(tar, ec);
        if (!extracted) return extracted;
        fs::create_directories(staging, ec);
        return {};
    });
}

}
