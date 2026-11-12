// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "otok/core/process.hpp"

// The sandbox of build processes (DESIGN 6.5, docs/PLAN.md decisions 28, 73 and 103): a mechanism of the host, the
// kernel sandbox through sandbox-exec on macOS, namespaces through bubblewrap on Linux, an AppContainer with
// otok-sandbox.dll injected into every process on Windows.
namespace otok {

// What a sandboxed process may touch besides reading the rest of the system. Paths must be canonical:
// the kernel compares resolved paths.
struct sandbox_spec
{
    std::vector<std::filesystem::path> writable;        // directories
    std::vector<std::filesystem::path> writable_files;  // single files (the jobserver fifo)
    std::vector<std::filesystem::path> denied_reads;    // directories whose file data is not readable (HOME)
    std::vector<std::filesystem::path> readable;        // exceptions inside them
};

// The sandbox this host has.
struct host_sandbox
{
    enum class mechanism { none, seatbelt, bubblewrap, appcontainer };
    mechanism kind = mechanism::none;
    std::filesystem::path program;  // sandbox-exec or bwrap; otok-sandbox.dll with an AppContainer
    std::string why_not;            // without one: why, for the note on unisolated builds; empty where none is known
    std::string container;          // the AppContainer's SID, as text
};

// sandbox-exec on macOS; on Linux `bwrap` of `env`'s PATH, once it has made the namespaces of a build here (Ubuntu's
// AppArmor lets only programs with a profile make them); on Windows the AppContainer profile `otok.build`, made once
// per user, with otok-sandbox.dll beside the executable (both readable to the container).
host_sandbox find_host_sandbox(env_vars const& env);

// How run_process starts a program in an AppContainer (process_spec::sandbox).
struct sandbox_launch
{
    std::string container;      // the AppContainer's SID, as text
    std::filesystem::path dll;  // otok-sandbox.dll
    std::filesystem::path log;  // where the DLL notes what the container is refused; empty: nowhere
    // The work directory: its ancestors (the drive root up to the cache) are opened outside with
    // sandbox_windows::ancestor_access for the DLL to hand, so that the build's programs can pass through them
    // (`mkdir -p`); empty: none.
    std::filesystem::path work;
};

// Windows: lets the AppContainer of `host` reach what `spec` names, through inheritable entries for its SID in the
// directories' ACLs: full access to `writable`, read and execute to `readable` (`denied_reads` needs nothing: the
// container reads only what is granted to it). A directory whose ACL already grants it is left alone, so the cache
// is walked once; a readable directory whose ACL otok may not change (Program Files) is left as it is, readable to
// every AppContainer anyway. Elsewhere nothing.
result<void> grant_sandbox_access(host_sandbox const& host, sandbox_spec const& spec);

// Windows: of `paths`, those the AppContainer of `host` cannot read and run (a directory listed and passed through, a
// file read and executed), as the kernel decides for a process of the container started by this one: Authz with the
// container's SID over this process's token, on each path's own ACL. otok never changes the ACLs of what lies outside
// its cache (docs/PLAN.md decision 109): the caller tells the user how to open them. A path that is not there is not
// one of them (whoever needs it says so). Elsewhere, and for another mechanism, none.
result<std::vector<std::filesystem::path>> sandbox_unreadable(host_sandbox const& host, std::vector<std::filesystem::path> const& paths);

// What lets the AppContainer of `host` read the directory or file `path`: an icacls command granting its SID read and
// execute, inherited by what is in it.
std::string sandbox_read_command(host_sandbox const& host, std::filesystem::path const& path);

// The Seatbelt profile (SBPL) of `spec`: everything allowed but the network (Unix sockets excepted),
// writes outside `writable` and reads of file data in `denied_reads` outside `readable`.
std::string seatbelt_profile(sandbox_spec const& spec);

// The options of bwrap for `spec`, up to `--`: the system read-only, a /dev and /proc of the sandbox, no network
// (a namespace with loopback only), `denied_reads` empty and read-only but for `readable` inside them, `writable` as
// they are.
std::vector<std::string> bubblewrap_args(sandbox_spec const& spec);

// What runs a command in the sandbox, before its argv: sandbox-exec with the profile file `profile` (which holds
// seatbelt_profile), or bwrap with its options; nothing for an AppContainer (process_spec::sandbox).
std::vector<std::string> sandbox_command(host_sandbox const& host, sandbox_spec const& spec, std::filesystem::path const& profile);

// Windows: the CPU time the System process (the kernel's worker threads) has used so far, in seconds; elsewhere
// nothing.
std::optional<double> kernel_worker_seconds();

// What each process of the AppContainer costs the kernel at its exit, in seconds (docs/PLAN.md decision 103): the CPU
// time of the System process while empty processes (cmd /c exit) run one after another in the container and a tenth of
// a second after, less that while as many run outside it, per process: `count` of each kind in five rounds, the
// quietest round of each kind counted (the System process's time grows by ticks of 15.6 ms, and other work only adds to
// a round). Windows sometimes gets into a state
// where each costs ~20 ms (healthy: a small fraction of one), which makes sandboxed builds slow and stalls the desktop.
// 0 for another mechanism, and where the System process cannot be read.
result<double> sandbox_exit_cost(host_sandbox const& host, int count);

namespace detail {

// Windows, for run_process: starts `program` suspended in the AppContainer of `launch` with its DLL injected and the
// DLL's payload copied in (null device, drive table and roots, MountPointManager, SIDs, the log), inheriting only
// `inherited`. `startup` is the STARTUPINFOW with the standard handles, `information` the PROCESS_INFORMATION filled;
// the caller puts the process in its job and resumes it. Fails elsewhere.
result<void> create_sandboxed_process(sandbox_launch const& launch, std::filesystem::path const& program, std::wstring& command_line,
    std::wstring& environment, std::filesystem::path const& cwd, std::vector<void*> const& inherited, void* startup, void* information);

}

}
