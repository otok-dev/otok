// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/command_line.hpp"

#include <algorithm>

namespace otok {

namespace {

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

option_spec const* find_option(std::span<option_spec const> options, std::string_view name, char alias)
{
    for (option_spec const& o : options) {
        if (alias ? o.alias == alias : o.name == name) return &o;
    }
    return nullptr;
}

std::string option_label(option_spec const& o)
{
    std::string label = o.alias ? std::string{"-"} + o.alias + ", " : std::string{"    "};
    label += "--" + std::string{o.name};
    if (!o.value_name.empty()) label += " " + std::string{o.value_name};
    return label;
}

std::string option_table(std::span<option_spec const> options)
{
    std::size_t width = 0;
    for (option_spec const& o : options) width = std::max(width, option_label(o).size());
    std::string text;
    for (option_spec const& o : options) {
        std::string const label = option_label(o);
        text += "  " + label + std::string(width - label.size() + 2, ' ') + std::string{o.help} + "\n";
    }
    return text;
}

}

std::optional<std::string> parsed_command::value(std::string_view name) const
{
    auto const it = values.find(name);
    if (it == values.end()) return std::nullopt;
    return it->second;
}

int parsed_command::count(std::string_view name) const
{
    auto const it = counts.find(name);
    return it == counts.end() ? 0 : it->second;
}

result<parsed_command> parse_command_line(std::span<std::string const> args, std::span<option_spec const> global,
    std::span<command_spec const> commands)
{
    parsed_command out;
    command_spec const* command = nullptr;
    bool options_ended = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string const& arg = args[i];
        bool const is_option = !options_ended && arg.size() > 1 && arg[0] == '-';
        if (!is_option) {
            if (!command) {
                auto const it = std::ranges::find(commands, std::string_view{arg}, &command_spec::name);
                if (it == commands.end()) return fail("unknown command " + in_quotes(arg) + "; see 'otok help'");
                command = &*it;
                out.command = arg;
            } else {
                out.arguments.push_back(arg);
            }
            continue;
        }
        if (arg == "--") {
            options_ended = true;
            continue;
        }

        // --name, --name=value, -C
        std::string name;
        std::optional<std::string> inline_value;
        char alias = 0;
        int times = 1;
        if (arg.starts_with("--")) {
            std::size_t const eq = arg.find('=');
            name = arg.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
            if (eq != std::string::npos) inline_value = arg.substr(eq + 1);
        } else if (arg.size() == 2) {
            alias = arg[1];
        } else if (std::ranges::all_of(arg.substr(1), [&](char c) { return c == arg[1]; })) {
            // `-vv`: a repeatable flag, as often as its alias
            alias = arg[1];
            times = static_cast<int>(arg.size() - 1);
        } else {
            return fail("unknown option " + in_quotes(arg));
        }
        option_spec const* spec = find_option(global, name, alias);
        if (!spec && command) spec = find_option(command->options, name, alias);
        if (!spec) {
            return fail("unknown option " + in_quotes(arg) + (command ? " for 'otok " + out.command + "'" : std::string{}));
        }
        std::string const key{spec->name};
        if (times > 1 && !spec->repeatable) return fail("unknown option " + in_quotes(arg));
        if (!spec->repeatable && (out.flags.contains(key) || out.values.contains(key))) return fail("option '--" + key + "' is given twice");
        if (spec->value_name.empty()) {
            if (inline_value) return fail("option '--" + key + "' takes no value");
            out.flags.insert(key);
            out.counts[key] += times;
            continue;
        }
        if (!inline_value) {
            if (i + 1 >= args.size()) return fail("option '--" + key + "' needs a value (" + std::string{spec->value_name} + ")");
            inline_value = args[++i];
        }
        out.values.emplace(key, std::move(*inline_value));
    }

    if (command) {
        std::size_t const n = out.arguments.size();
        if (n < command->min_arguments || n > command->max_arguments) {
            std::string const expected = command->max_arguments == 0 ? "takes no arguments" : "expects " + std::string{command->arguments};
            return fail("'otok " + out.command + "' " + expected + "; see 'otok help " + out.command + "'");
        }
    }
    return out;
}

std::string general_usage(std::string_view program, std::span<option_spec const> global, std::span<command_spec const> commands)
{
    std::string text = "usage: " + std::string{program} + " [options] <command> [command options] [arguments]\n\ncommands:\n";
    std::size_t width = 0;
    for (command_spec const& c : commands) width = std::max(width, c.name.size());
    for (command_spec const& c : commands) {
        text += "  " + std::string{c.name} + std::string(width - c.name.size() + 2, ' ') + std::string{c.summary} + "\n";
    }
    text += "\noptions:\n" + option_table(global);
    return text;
}

std::string command_usage(std::string_view program, command_spec const& command)
{
    std::string text = "usage: " + std::string{program} + " " + std::string{command.name};
    if (!command.options.empty()) text += " [options]";
    if (!command.arguments.empty()) text += " " + std::string{command.arguments};
    text += "\n\n" + std::string{command.summary} + "\n";
    if (!command.options.empty()) text += "\noptions:\n" + option_table(command.options);
    return text;
}

}
