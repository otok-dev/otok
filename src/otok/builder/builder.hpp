// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <ostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "otok/builder/target.hpp"
#include "otok/builder/toolchains.hpp"
#include "otok/cache/shared.hpp"
#include "otok/cache/store.hpp"
#include "otok/core/console.hpp"
#include "otok/core/error.hpp"
#include "otok/core/executor.hpp"
#include "otok/core/jobserver.hpp"
#include "otok/core/sandbox.hpp"
#include "otok/fetch/git.hpp"
#include "otok/lock/lock.hpp"
#include "otok/lua/build_script.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/resolver/resolver.hpp"
#include "otok/version/standard.hpp"

// Building the slices of resolved packages into the build cache (DESIGN 6.4, docs/formats/cache-key.md,
// docs/PLAN.md decision 19): a fresh, patched copy of the source per slice, build.lua with the target's
// toolchain, checks of the result, and an entry under the slice key.
namespace otok {

// What of a library otok.dev.toml gives (otok-dev-toml.md 4): its slices are built incrementally in work directories
// kept in the cache's dev/.
struct dev_inputs
{
    std::string label;                  // what the build says it is built from: a working tree as the file gives it
    bool source = false;
    bool recipe = false;
    std::set<std::string> resources;
    bool quiet = false;                 // the build's output not shown as it comes
};

// Where a package's inputs are, as fetching left them.
struct package_inputs
{
    std::filesystem::path recipe_dir;
    std::filesystem::path source;                               // copied for every slice
    std::map<std::string, std::filesystem::path> resources;     // name → the store's tree or file
    std::optional<dev_inputs> dev;                              // otok.dev.toml names the library; nothing: the lock's inputs
};

// How build processes are isolated (docs/PLAN.md decisions 28, 73).
enum class build_isolation
{
    unavailable,    // the host has no sandbox: builds fail (docs/PLAN.md decision 103)
    off,            // asked: --no-sandbox, `sandbox = false` in config.toml (OTOK_SANDBOX)
    sandbox
};

// Windows: the check of the sandbox's exits (builder::check_sandbox_exits), made by the first sandboxed slice while the
// others wait.
struct sandbox_exits_check
{
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    bool done = false;
};

// What builders running together say or do once: the toolchains of their targets, the check of the sandbox's exits (the
// builds of otok publish, docs/PLAN.md decision 103).
struct announcements
{
    std::mutex m;
    std::set<std::string> said;
    sandbox_exits_check exits;

    bool first(std::string const& what)
    {
        std::lock_guard lock{m};
        return said.insert(what).second;
    }
};

// A slice as it starts building, is built, is found in the cache, or fails: for `--format json` (cli-json.md 9).
struct slice_event
{
    enum class kind
    {
        building,
        built,
        cached,
        failed
    };
    kind state = kind::building;
    std::string library, id, version;
    std::int64_t revision = 0;
    bool tool = false;
    bool build_tools = false;
    slice_spec spec;
    double seconds = 0;  // built or failed: how long it took
    std::string dev;     // the working trees of otok.dev.toml it was built from; empty: none
    std::string from;    // cached: the URL of the store of the shared build cache it came from; empty: the local cache
    // failed: the error, the log kept in failed/ (cache.md 7.1; empty: none), what the user can do (empty: nothing known)
    std::string message;
    std::filesystem::path log;
    std::string hint;
};

// A store of the shared build cache as a build uses it (cache.md 12, config.md 2.3).
struct build_cache_use
{
    shared_store store;
    bool push = false;
    bool push_local = false;
};

// Where a slice of the build would come from (cache.md 12.2): for `otok cache status` and `otok cache push`.
struct located_slice
{
    enum class where
    {
        local,      // the local cache has its entry
        store,      // a store of the shared build cache has it
        build       // it is built: in no cache, or a dev slice or one depending on a dev slice
    };
    std::string library, id, version;
    std::int64_t revision = 0;
    bool tool = false;
    bool build_tools = false;
    slice_spec spec;
    std::string key;                    // 64 hex digits; empty: known only once its dependencies are built (dev)
    std::filesystem::path entry;        // in the local cache
    where from = where::build;
    std::string store;                  // the URL of the store, with `store`
    bool dev = false;                   // of otok.dev.toml, or depending on it: never pushed
    bool local_input = false;           // its source or recipe, or a dependency's, is a path of this machine
    bool licenses = false;              // the local entry has licenses/ (cache.md 7.1): it can be pushed
};

struct build_settings
{
    std::size_t configuration = 0;          // index into resolution::configurations
    std::int64_t jobs = 1;
    bool keep_build_dirs = false;           // the whole work directory of a failed build, not only its log
    // docs/PLAN.md decision 59: 1, what each slice builds with and the commands it runs; 2, their output too, one slice at a time
    int verbosity = 0;
    progress_display* display = nullptr;    // shows the slices under way; nothing: none
    // the jobs, shared with other builders (the builds of otok publish, docs/PLAN.md decisions 60, 61); nothing: a broker
    // of its own with `jobs`
    job_broker* broker = nullptr;
    announcements* announced = nullptr;     // shared with other builders; nothing: each says its own
    // Told of each slice, from the threads of the builds; nothing: none
    std::function<void(slice_event const&)> on_slice;
    std::optional<git_tool> git;            // applies patches
    build_isolation isolation = build_isolation::unavailable;
    host_sandbox sandbox;                   // the mechanism, with `isolation == sandbox`; why there is none, with `unavailable`
    // Windows: the cache and the work directories through short drive letters (core/virtual_drives), which needs
    // otok-sandbox.dll (`sandbox.program`) also when the builds are unisolated
    bool virtual_paths = false;
    std::vector<std::filesystem::path> denied_reads;    // in the sandbox: HOME
    std::filesystem::path launcher;         // otok itself, for otok.b2 (build-lua-api.md 6.2); empty: none
    toolchain_choice toolchains;            // check_toolchains: what each recipe declares, as found
    // the tool packages of the project's [tool-deps] are built though no recipe uses them: the build of a tool recipe by
    // otok publish (resolve_settings::tool_roots)
    bool tool_roots = false;
    // The stores of the shared build cache, asked in this order after the local cache (cache.md 12.2); empty: none
    std::vector<build_cache_use> stores;
    std::string pushed_by;                  // the user named in the descriptors of pushes
    // Fetches the inputs of the packages (package keys) into the builder's inputs, of the slices it builds only (cache.md
    // 12.2); nothing: they were all fetched before
    std::function<result<void>(std::set<std::string> const& packages, std::map<std::string, package_inputs>& inputs)> fetch;
    // Only where each slice would come from, into this, nothing fetched or built (otok cache status, push); nothing: a build
    std::vector<located_slice>* locate_only = nullptr;
};

// A log of a build kept in the cache (cache.md 7.1): a slice's, or a failed build's in failed/.
struct kept_log
{
    std::filesystem::path file;
    std::string version, slice, configuration;  // from the key; slice empty for a patched source
    std::string entry;                          // "build", "build_tools", "sources"
    bool failed = false;
    std::filesystem::file_time_type time;
};

// The logs of a library's builds in the cache, the newest first.
std::vector<kept_log> find_build_logs(cache_store const& cache, std::string const& library);

// Package keys: "<library>" for target packages, "tool:<library>" for tool packages.
std::string package_key(std::string const& library, bool tool);

// The dynamic libraries of the built slices that export symbols of the static libraries inside them, which their links
// should have hidden (otok-toml.md 5.2, docs/PLAN.md decision 136): a message for each. A warning of `otok install`, an
// error of `otok publish`.
std::vector<std::string> hidden_export_problems(resolution const& r, std::size_t configuration, std::span<built_package const> built);

// The canonical description of a key (cache-key.md 3) and its SHA-256.
std::string key_text(std::map<std::string, std::string> const& inputs);

// The meson machine file of a slice (build-lua-api.md 6.2): a cross file when `host` describes the host
// machine (`[host_machine]`), else a native file. `pkg_config`: its program; empty: none. `binaries`: more of `[binaries]`
// (toolchain_profile::meson_binaries). Values are written as they are, meson knowing no escapes; one it cannot hold is an error.
result<std::string> meson_machine_file(slice_toolchain const& t, std::map<std::string, std::string> const& host, std::string const& pkg_config,
    std::map<std::string, std::vector<std::string>> const& binaries = {});

// Builders of this process skip the check of the sandbox's exits (builder::check_sandbox_exits): for the tests, whose
// every build would otherwise spend ~2 s on it, one process at a time; the check itself is tested apart.
void skip_sandbox_exit_checks() noexcept;

namespace detail {

// The text of a slice entry's result.toml: what build returned (`out`), relative to `slice_prefix` as build_script checks
// those paths (resolved, through the drives when the slice's paths are on them: `virtual_paths`); an error naming the first
// path it cannot write so (cache.md 6.2: the entry would be damaged, so it is never made).
result<std::string> write_result(build_output const& out, std::filesystem::path const& slice_prefix, bool virtual_paths = false);

}  // namespace detail

class builder
{
public:
    builder(resolution const& r, manifest const& m, lock_file const& lock, std::filesystem::path project_dir,
        std::map<std::string, package_inputs> inputs, std::span<build_target* const> targets, cache_store& cache,
        build_settings settings, std::ostream& progress);
    ~builder();
    builder(builder const&) = delete;
    builder& operator=(builder const&) = delete;

    // Builds every package the selected configuration builds, slices concurrently as the jobserver
    // allows (docs/PLAN.md decision 20), each after the slices it needs; slices already in the cache are
    // reused. On failure: the errors of every slice that failed.
    std::expected<std::vector<built_package>, std::vector<error>> build_all();

private:
    // The entry of the lock of a package: of its graph (otok-lock.md 2).
    locked_package const* locked_of(resolved_package const& p, bool tool) const;
    // The package a tool package depends on for a library: its dependencies are ids (otok-lock.md 5.7).
    resolved_package const* tool_dependency_of(resolved_package const& p, std::string_view library) const;

    // The recipe's sha256: the locked one, or the tree hash of a local recipe at build time.
    result<std::string> recipe_hash(resolved_package const& p, bool tool);

    // The program the toolchain `name` of a tool package runs: built by its recipe in the tool slice, or found installed
    // (docs/PLAN.md decision 70: the interpreter of a script).
    result<std::filesystem::path> toolchain_program(resolved_package const& tool_package, std::string const& name);

    // The key of a slice and what follows from it (cache-key.md 2, 3), before anything is built (cache.md 12.2) or as it
    // builds.
    struct slice_plan
    {
        build_target* target = nullptr;
        std::string os;
        toolchain_profile profile;
        std::string text, hex;              // the description of the key and its SHA-256
        bool known = true;                  // false: a dependency's key comes from its build (a dev slice, otok-dev-toml.md 4)
        bool local = false;                 // its source or recipe, or a dependency's, is a path of this machine (cache.md 12.3)
        std::map<std::string, std::string> option_keys;
        std::map<std::string, std::filesystem::path> option_files;
        language_standards standards;
        compile_flags project;
        chosen_toolchains const* toolchains = nullptr;
        std::vector<std::string> inherited; // the defines of the dependencies' products (build-lua-api.md 4)
        std::vector<std::string> defines;   // its own products', then the dependencies'
        std::filesystem::path entry;
        std::string work_name;
        bool dev = false;
    };
    // What the key of a slice takes from one it needs.
    struct dependency_key
    {
        std::string key;
        std::vector<std::string> defines;
        bool known = true;
        bool local = false;
    };
    // The slice of an id (slice_id) a slice needs; nothing: not built, or not planned.
    using key_lookup = std::function<std::optional<dependency_key>(std::string const& id)>;
    result<slice_plan> plan_slice(resolved_package const& p, bool tool, slice_spec const& spec, build_entry entry,
        std::optional<dependency_key> const& tools, key_lookup const& find);

    // The slice `key` from the stores of the shared build cache, unpacked into its local entry and checked (cache.md 12.2);
    // the URL of the store, or nothing when no store has it intact (a warning for each that failed).
    std::optional<std::string> fetch_from_store(std::string const& key, std::filesystem::path const& entry, slice_spec const& spec,
        build_target& target, std::string const& label);

    // Uploads a slice built here to the stores with `push`, as a task of its own (cache.md 12.3).
    void push_to_stores(std::string const& key, std::filesystem::path const& entry, bool local, std::string const& label);

    // Fetches the inputs of a package for building it, once (settings_.fetch).
    result<void> ensure_inputs(std::string const& package);

    // The slice `id` built or found, by its task, when the plan did not count it needed (cache.md 12.2).
    result<void> ensure_built(std::string const& id);

    // Waits for the uploads under way.
    void join_pushes();

    // One entry: `build` of a slice, or `build_tools` on the tool slice; `tools` is the latter's entry
    // for the target slices of a recipe that has one.
    result<built_slice> build_slice(resolved_package const& p, bool tool, slice_spec const& spec, build_entry entry, built_slice const* tools);

    // The patched source of a recipe that delivers it (recipe-toml.md 12.1): its cache entry
    // `builds/<library>/<version>-r<revision>/sources/<key>/`, whose `source/` the target packages.
    result<void> prepare_sources(resolved_package const& p, built_package& out);

    // Applies the recipe's patches to the copy of the source in `source` (DESIGN 6.4).
    result<void> apply_patches(recipe const& rec, std::filesystem::path const& recipe_dir, std::filesystem::path const& source,
        std::filesystem::path const& work, std::filesystem::path const& log) const;

    // The project's flags for a library on a target (otok-toml.md 4.3), in their order.
    compile_flags project_flags(std::string const& library, bool tool, std::string const& target) const;

    // Counts a slice of the library done: ", 3 of 8 slices" when it has more than one, for the progress lines.
    std::string slice_done(std::string const& library, bool tool, build_entry entry);

    // A line of progress, whole.
    void say(std::string const& line);

    // What runs the processes of a slice in the sandbox, before their argv (docs/PLAN.md decisions 28, 73); on macOS with
    // `<work>/sandbox.sb` written.
    result<std::vector<std::string>> sandbox_command_of(std::filesystem::path const& work, std::filesystem::path const& recipe_dir,
        std::vector<std::filesystem::path> const& read_paths, std::filesystem::path const& jobserver_fifo) const;

    // Windows, in the AppContainer: fails, naming each path the container cannot read and the command that lets it, when
    // some of `needed` (a path and what needs it, "the toolchain of 'android' (…)") outside the cache is not readable to
    // it; otok never changes their ACLs (docs/PLAN.md decision 109). Each path is checked once per builder. Elsewhere
    // nothing.
    result<void> check_sandbox_reads(std::vector<std::pair<std::filesystem::path, std::string>> const& needed);

    // Windows: what the processes of the AppContainer cost the kernel at their exit (sandbox_exit_cost), checked with a
    // task of its own on the display, told in a line and warned of when it is much (docs/PLAN.md decision 103).
    void check_sandbox_exits();

    // Whether `tool` in the PATH of `env` understands the jobserver, asked once per program.
    jobserver_protocol jobserver_client(std::string const& tool, env_vars const& env);

    resolution const& r_;
    manifest const& m_;
    lock_file const& lock_;
    std::filesystem::path project_dir_;
    std::map<std::string, package_inputs> inputs_;
    std::span<build_target* const> targets_;
    cache_store& cache_;
    build_settings settings_;
    std::ostream& progress_;
    std::optional<slice_spec> tool_slice_;
    std::unique_ptr<job_broker> own_broker_;
    job_broker* broker_ = nullptr;                      // own_broker_, or that of the settings
    std::filesystem::path work_root_;                   // its work directories: <cache builds>/<n> (cache.md 7.1)
    std::filesystem::path builds_drive_;                // the drive of the work directories (core/virtual_drives); empty: none
    std::vector<std::filesystem::path> drives_;         // the letters this builder uses, released when it ends

    // Shared by the slices building at once.
    std::mutex mutex_;
    std::map<std::string, std::string> described_;      // targets whose toolchain was announced
    std::map<std::string, built_slice> built_;          // slice id (slice_id) → its entry
    std::map<std::string, jobserver_protocol> jobserver_clients_;     // program → jobserver_client
    bool isolation_told_ = false;                       // the warning on unisolated builds was printed
    std::map<std::filesystem::path, bool> sandbox_reads_;  // check_sandbox_reads: a path → whether the container reads it
    std::map<std::string, std::string> recipe_hashes_;  // package key → recipe_hash; filled before building
    std::set<std::string> headers_packages_;            // package keys built from a headers slice; filled before building
    std::map<std::string, std::pair<std::size_t, std::size_t>> slices_done_;  // package key → slices done, of all
    std::map<std::string, dependency_key> planned_;     // slice id → its key, planned before the build (cache.md 12.2)
    std::map<std::string, std::string> source_hashes_;  // package key → the tree hash of its local source, once
    std::set<std::string> fetched_;                     // package keys whose inputs settings_.fetch gave
    std::mutex fetch_mutex_;
    std::mutex push_mutex_;
    std::vector<std::thread> pushes_;                   // uploads under way, joined when the build ends
    // the tasks by slice id, and what runs each once (ensure_built)
    std::map<std::string, std::size_t> task_of_;
    std::vector<std::function<result<void>()>> task_runs_;
    std::vector<std::unique_ptr<std::mutex>> task_mutexes_;
    // Windows: check_sandbox_exits of this builder alone, without settings_.announced
    sandbox_exits_check exits_;
};

}
