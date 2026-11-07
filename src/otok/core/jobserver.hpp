// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "otok/core/error.hpp"
#include "otok/core/process.hpp"

// The jobserver of a build (DESIGN 6.4, docs/PLAN.md decision 20): the GNU make protocol with a named
// pool, so nothing has to be inherited: a fifo on macOS and Linux, a named semaphore on Windows.
namespace otok {

class jobserver
{
public:
    // A pool of `jobs - 1` tokens: the holder of the jobserver has the implicit one. The fifo is
    // created in `dir` (unused on Windows).
    static result<jobserver> create(std::int64_t jobs, std::filesystem::path const& dir);

    // A pool of `tokens` tokens named `name` (the fifo's file in `dir`), for clients that may run up to `jobs` (the -j of
    // MAKEFLAGS): a slice's, which the job broker fills (docs/PLAN.md decision 61).
    static result<jobserver> create(std::int64_t jobs, std::int64_t tokens, std::filesystem::path const& dir, std::string const& name);

    // A client of the jobserver of `auth`, the value of --jobserver-auth= in its MAKEFLAGS ("fifo:<path>";
    // on Windows the semaphore's name). The client removes nothing when it goes.
    static result<jobserver> connect(std::string_view auth);

    jobserver(jobserver&&) noexcept;
    jobserver& operator=(jobserver&&) noexcept;
    ~jobserver();

    // The value of MAKEFLAGS for clients: "-j<jobs> --jobserver-auth=…".
    std::string const& makeflags() const noexcept;

    // The fifo clients open (the sandbox lets them write it); empty on Windows.
    std::filesystem::path fifo() const;

    // Takes a token, waiting at most `timeout`; false when none came.
    bool try_acquire(std::chrono::milliseconds timeout);
    void release();

private:
    struct state;
    explicit jobserver(std::unique_ptr<state> s);
    std::unique_ptr<state> state_;
};

// The launcher of tools without the jobserver (`otok jobserver-exec`, build-lua-api.md 6.2): runs `argv`
// with otok's own standard streams once it holds a job, the slot of its slice (the lock file `slot`, the
// implicit token the slice holds, so each slice always runs one) or a token of the jobserver of `auth`
// (empty: none, run at once), and gives the token back when the program ends.
result<process_exit> run_with_job(std::vector<std::string> argv, std::filesystem::path const& slot, std::string_view auth, env_vars env,
    std::filesystem::path const& cwd);

// How a tool takes the tokens of this jobserver (build-lua-api.md 6.2).
enum class jobserver_protocol
{
    none,
    named,              // MAKEFLAGS as it is, `--jobserver-auth=fifo:<path>` (the semaphore's name on Windows): GNU make 4.4
                        // and later, ninja 1.13 and later
    descriptors,        // `--jobserver-auth=3,4`, the fifo inherited as descriptors 3 and 4: GNU make 4.2 and 4.3, on POSIX
    old_descriptors,    // `--jobserver-fds=3,4` and a bare `-j`, the same: GNU make before 4.2, on POSIX
};

// How the program `tool` ("make", "ninja", or its path), looked up in the PATH of `env`, takes them. Runs
// `<tool> --version`.
jobserver_protocol jobserver_protocol_of(std::string_view tool, env_vars const& env);

// For a client taking the tokens through inherited descriptors (process_spec::jobserver_fifo).
struct inherited_jobserver
{
    std::string makeflags;
    std::filesystem::path fifo;
};

// The MAKEFLAGS of a client of `protocol` (descriptors, old_descriptors) and the fifo it inherits, for the jobserver
// whose MAKEFLAGS is `makeflags` ("-j<n> --jobserver-auth=fifo:<path>"); nothing when it has no fifo.
std::optional<inherited_jobserver> inherit_jobserver(std::string_view makeflags, jobserver_protocol protocol);

namespace detail {

// The same from the output of `--version`; `descriptors`: whether the host passes descriptors (POSIX).
jobserver_protocol make_jobserver_protocol(std::string_view version_output, bool descriptors);
bool ninja_understands_jobserver(std::string_view version_output);

}

}
