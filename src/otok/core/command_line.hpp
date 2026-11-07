// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"

// The command line: `otok [global options] <command> [options] [arguments]` (docs/PLAN.md decision 12).
// Options are `--name`, `--name value` or `--name=value`, with an optional one-letter alias (`-C dir`);
// a repeatable flag's alias may be repeated in one argument (`-vv`). `--` ends the options.
namespace otok {

struct option_spec
{
    std::string_view name;        // "locked" for --locked
    char alias = 0;               // 'C' for -C; 0: none
    std::string_view value_name;  // "DIR": takes a value; empty: a flag
    std::string_view help;
    bool repeatable = false;      // a flag that may be given more than once: `-vv`, `-v -v`
};

struct command_spec
{
    std::string_view name;
    std::string_view arguments;   // "[LIBRARY...]", for usage
    std::string_view summary;
    std::vector<option_spec> options;
    std::size_t min_arguments = 0;
    std::size_t max_arguments = 0;  // std::size_t(-1): any number
};

struct parsed_command
{
    std::string command;                                    // empty: no command given
    std::set<std::string, std::less<>> flags;               // global and command flags given
    std::map<std::string, std::string, std::less<>> values; // options with values
    std::map<std::string, int, std::less<>> counts;         // how often each flag was given
    std::vector<std::string> arguments;

    bool flag(std::string_view name) const { return flags.contains(name); }
    int count(std::string_view name) const;
    std::optional<std::string> value(std::string_view name) const;
};

// Parses `args` (without the program name). Global options come before the command; a command's own
// options anywhere after it. Errors name the offending argument.
result<parsed_command> parse_command_line(std::span<std::string const> args, std::span<option_spec const> global,
    std::span<command_spec const> commands);

// "usage: otok [options] <command> ...": the commands with their summaries and the global options.
std::string general_usage(std::string_view program, std::span<option_spec const> global, std::span<command_spec const> commands);

// The usage of one command with its options.
std::string command_usage(std::string_view program, command_spec const& command);

}
