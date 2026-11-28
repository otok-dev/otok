// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The command-line parser (docs/PLAN.md decision 12).

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "otok/core/command_line.hpp"

using namespace otok;

namespace {

option_spec const global[] = {
    {"directory", 'C', "DIR", "Run in DIR"},
    {"version", 0, "", "Print the version"},
};

command_spec const commands[] = {
    {"install", "", "Install", {{"locked", 0, "", "Use the lock"}, {"profile", 0, "NAME", "A profile"}, {"jobs", 'j', "N", "Jobs"}, {"verbose", 'v', "", "Say more", true}, {"skip", 0, "NAME", "Skip", true}}, 0, 0},
    {"why", "LIBRARY", "Explain", {}, 1, 1},
    {"update", "[LIBRARY...]", "Update", {}, 0, static_cast<std::size_t>(-1)},
};

result<parsed_command> parse(std::vector<std::string> const& args)
{
    return parse_command_line(args, global, commands);
}

}

TEST(command_line, parses_commands_options_and_arguments)
{
    auto p = parse({"-C", "proj", "install", "--locked", "--profile=ci"});
    ASSERT_TRUE(p.has_value()) << p.error().message;
    EXPECT_EQ(p->command, "install");
    EXPECT_EQ(p->value("directory"), "proj");
    EXPECT_TRUE(p->flag("locked"));
    EXPECT_EQ(p->value("profile"), "ci");
    EXPECT_FALSE(p->value("missing").has_value());

    p = parse({"install", "--profile", "ci", "--directory=d"});  // global options after the command too
    ASSERT_TRUE(p.has_value()) << p.error().message;
    EXPECT_EQ(p->value("profile"), "ci");
    EXPECT_EQ(p->value("directory"), "d");

    p = parse({"update", "openssl", "--", "-odd-name"});
    ASSERT_TRUE(p.has_value()) << p.error().message;
    EXPECT_EQ(p->arguments, (std::vector<std::string>{"openssl", "-odd-name"}));

    p = parse({"--version"});
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->command.empty());
    EXPECT_TRUE(p->flag("version"));
}

// `-v`, `-vv`, `-v --verbose`: counted (docs/PLAN.md decision 59).
TEST(command_line, counts_repeatable_flags)
{
    auto p = parse({"install"});
    ASSERT_TRUE(p.has_value()) << p.error().message;
    EXPECT_EQ(p->count("verbose"), 0);
    EXPECT_EQ(p->count("locked"), 0);
    for (auto const& [args, times] : {std::pair{std::vector<std::string>{"install", "-v"}, 1}, std::pair{std::vector<std::string>{"install", "-vv"}, 2},
             std::pair{std::vector<std::string>{"install", "-v", "--verbose", "-vv"}, 4}}) {
        p = parse(args);
        ASSERT_TRUE(p.has_value()) << p.error().message;
        EXPECT_TRUE(p->flag("verbose"));
        EXPECT_EQ(p->count("verbose"), times);
    }
    p = parse({"install", "--locked"});
    ASSERT_TRUE(p.has_value()) << p.error().message;
    EXPECT_EQ(p->count("locked"), 1);
    EXPECT_EQ(parse({"-CC"}).value().value("directory"), "C");  // only a flag repeats: an option with a value takes it
    EXPECT_EQ(parse({"install", "-vx"}).error().message, "unknown option '-vx'");
}

// `--skip a --skip b`: every value, in order
TEST(command_line, keeps_every_value_of_a_repeatable_option)
{
    auto p = parse({"install", "--skip", "a", "--skip=b", "--profile", "ci"});
    ASSERT_TRUE(p.has_value()) << p.error().message;
    EXPECT_EQ(p->values_of("skip"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(p->value("skip"), "a");
    EXPECT_EQ(p->values_of("profile"), std::vector<std::string>{"ci"});
    EXPECT_TRUE(p->values_of("locked").empty());
    EXPECT_EQ(parse({"install", "--profile", "a", "--profile", "b"}).error().message, "option '--profile' is given twice");
}

TEST(command_line, reports_errors)
{
    EXPECT_EQ(parse({"instal"}).error().message, "unknown command 'instal'; see 'otok help'");
    EXPECT_EQ(parse({"--locked"}).error().message, "unknown option '--locked'");  // a command option before the command
    EXPECT_EQ(parse({"why", "a", "--locked"}).error().message, "unknown option '--locked' for 'otok why'");
    EXPECT_EQ(parse({"install", "--locked", "--locked"}).error().message, "option '--locked' is given twice");
    EXPECT_EQ(parse({"install", "--locked=yes"}).error().message, "option '--locked' takes no value");
    EXPECT_EQ(parse({"install", "--profile"}).error().message, "option '--profile' needs a value (NAME)");
    EXPECT_EQ(parse({"-x"}).error().message, "unknown option '-x'");
    EXPECT_EQ(parse({"install", "extra"}).error().message, "'otok install' takes no arguments; see 'otok help install'");
    EXPECT_EQ(parse({"why"}).error().message, "'otok why' expects LIBRARY; see 'otok help why'");
    EXPECT_EQ(parse({"why", "a", "b"}).error().message, "'otok why' expects LIBRARY; see 'otok help why'");
}

TEST(command_line, usage)
{
    std::string const general = general_usage("otok", global, commands);
    EXPECT_NE(general.find("usage: otok [options] <command>"), std::string::npos) << general;
    EXPECT_NE(general.find("  install  Install\n"), std::string::npos) << general;
    EXPECT_NE(general.find("  -C, --directory DIR  Run in DIR\n"), std::string::npos) << general;
    EXPECT_NE(general.find("      --version        Print the version\n"), std::string::npos) << general;

    std::string const one = command_usage("otok", commands[0]);
    EXPECT_NE(one.find("usage: otok install [options]\n"), std::string::npos) << one;
    EXPECT_NE(one.find("    --profile NAME  A profile\n"), std::string::npos) << one;
}

// `-j15`, `-Cdir`: the value right after the alias of an option with a value.
TEST(command_line, takes_values_after_aliases)
{
    auto p = parse({"-Cdir", "install", "-j15"});
    ASSERT_TRUE(p.has_value()) << p.error().message;
    EXPECT_EQ(p->value("directory"), "dir");
    EXPECT_EQ(p->value("jobs"), "15");

    p = parse({"install", "-j", "3"});
    ASSERT_TRUE(p.has_value()) << p.error().message;
    EXPECT_EQ(p->value("jobs"), "3");

    EXPECT_EQ(parse({"install", "-j4", "--jobs=5"}).error().message, "option '--jobs' is given twice");
    EXPECT_EQ(parse({"why", "a", "-j4"}).error().message, "unknown option '-j4'");
    EXPECT_EQ(parse({"install", "-vj4"}).error().message, "unknown option '-vj4'");  // flags and values do not combine
}
