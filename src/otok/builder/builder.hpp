// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <condition_variable>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "otok/builder/target.hpp"
#include "otok/builder/toolchains.hpp"
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

// What builders running together say once: the toolchains of their targets (the builds of otok publish).
struct announcements
{
    std::mutex m;
    std::set<std::string> said;

    bool first(std::string const& what)
    {
        std::lock_guard lock{m};
        return said.insert(what).second;
    }
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

// The canonical description of a key (cache-key.md 3) and its SHA-256.
std::string key_text(std::map<std::string, std::string> const& inputs);

// The meson machine file of a slice (build-lua-api.md 6.2): a cross file when `host` describes the host
// machine (`[host_machine]`), else a native file. `pkg_config`: its program; empty: none. `binaries`: more of `[binaries]`
// (toolchain_profile::meson_binaries).
std::string meson_machine_file(slice_toolchain const& t, std::map<std::string, std::string> const& host, std::string const& pkg_config,
    std::map<std::string, std::vector<std::string>> const& binaries = {});

// Builders of this process skip the check of the sandbox's exits (builder::check_sandbox_exits): for the tests, whose
// every build would otherwise spend ~2 s on it, one process at a time; the check itself is tested apart.
void skip_sandbox_exit_checks() noexcept;

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
    std::map<std::string, std::pair<std::size_t, std::size_t>> slices_done_;  // package key → slices done, of all
    // Windows: check_sandbox_exits, by the first sandboxed slice while the others wait
    bool exits_check_started_ = false;
    bool exits_checked_ = false;
    std::condition_variable exits_checked_cv_;
};

}
