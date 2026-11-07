// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"

// Running programs directly, never through a shell (build-lua-api.md 5): an argument vector, a
// complete environment, a working directory, output to a log. CreateProcessW on Windows,
// posix_spawn elsewhere. The sandbox and the jobserver are added on top of this layer.
namespace otok {

// The complete environment of a child process (UTF-8); nothing is inherited from otok's own.
using env_vars = std::map<std::string, std::string>;

struct process_spec
{
    // argv[0] is the program: a name without a path separator is looked up in the PATH of `env`
    // (not otok's own), otherwise it is a path, relative ones taken from `cwd`. On Windows a name
    // without an extension gets ".exe"; batch files are not run (they would need cmd.exe).
    std::vector<std::string> argv;
    env_vars env;
    std::filesystem::path cwd;
    // Standard output and standard error are appended to this file; empty: discarded. Standard
    // input is always the null device.
    std::filesystem::path log;
    // Standard output is returned in process_exit::output instead of going to the log.
    bool capture = false;
    // Standard input, output and error are otok's own (`log` and `capture` are ignored): otok runs
    // the program for another one, as the jobserver launcher does.
    bool inherit_io = false;
    std::size_t capture_limit = 1024 * 1024;
    // POSIX: a jobserver fifo the child inherits as descriptors 3 (to read) and 4 (to write), as GNU make before
    // 4.4 takes the jobserver's pipe (inherit_jobserver); empty: none. Ignored on Windows.
    std::filesystem::path jobserver_fifo{};
};

struct process_exit
{
    int code = 0;    // the exit code; meaningful when signal == 0
    int signal = 0;  // POSIX: the signal that terminated the process
    std::string output;            // captured standard output, at most capture_limit bytes
    bool output_truncated = false;  // the process wrote more than capture_limit

    bool success() const noexcept { return signal == 0 && code == 0; }
};

// Starts the process and waits for it. Fails when the process cannot be started (program not
// found, bad working directory, unwritable log); a process that runs and fails is a process_exit.
result<process_exit> run_process(process_spec const& spec);

// The program run_process would start for argv[0]: the absolute path, or a failure.
result<std::filesystem::path> find_program(std::string_view program, env_vars const& env, std::filesystem::path const& cwd);

// "exit code 2", "terminated by signal 9" — for messages.
std::string describe_exit(process_exit const& e);

// The id of this process.
unsigned long current_process_id() noexcept;

// The machine otok runs on as `when = { host = … }` names it (recipe-toml.md 9.1): "windows-x86_64", "macos-arm64".
std::string_view host_machine() noexcept;

// The executable of this process, as an absolute path; empty when the OS does not tell.
std::filesystem::path current_executable();

// otok's own environment, for tools that act for the user (git: credentials, SSH agent, proxies).
// Builds never get it (build-lua-api.md 5).
env_vars current_environment();

namespace detail {

// One argument quoted for a Windows command line so that CommandLineToArgvW and the MSVC runtime
// read it back unchanged. Compiled on every host so that it is tested everywhere.
std::string quote_windows_argument(std::string_view arg);

}

}
