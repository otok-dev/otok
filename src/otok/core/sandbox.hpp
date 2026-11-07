// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "otok/core/process.hpp"

// The sandbox of build processes (DESIGN 6.5, docs/PLAN.md decisions 28 and 73): a mechanism of the host, the
// kernel sandbox through sandbox-exec on macOS, namespaces through bubblewrap on Linux; none on Windows (DESIGN 9.3,
// item 8).
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
    enum class mechanism { none, seatbelt, bubblewrap };
    mechanism kind = mechanism::none;
    std::filesystem::path program;  // sandbox-exec or bwrap
    std::string why_not;            // without one: why, for the note on unisolated builds; empty where none is known
};

// sandbox-exec on macOS; on Linux `bwrap` of `env`'s PATH, once it has made the namespaces of a build here (Ubuntu's
// AppArmor lets only programs with a profile make them).
host_sandbox find_host_sandbox(env_vars const& env);

// The Seatbelt profile (SBPL) of `spec`: everything allowed but the network (Unix sockets excepted),
// writes outside `writable` and reads of file data in `denied_reads` outside `readable`.
std::string seatbelt_profile(sandbox_spec const& spec);

// The options of bwrap for `spec`, up to `--`: the system read-only, a /dev and /proc of the sandbox, no network
// (a namespace with loopback only), `denied_reads` empty and read-only but for `readable` inside them, `writable` as
// they are.
std::vector<std::string> bubblewrap_args(sandbox_spec const& spec);

// What runs a command in the sandbox, before its argv: sandbox-exec with the profile file `profile` (which holds
// seatbelt_profile), or bwrap with its options.
std::vector<std::string> sandbox_command(host_sandbox const& host, sandbox_spec const& spec, std::filesystem::path const& profile);

}
