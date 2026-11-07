// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include <gtest/gtest.h>

#include "otok/core/environment.hpp"
#include "otok/core/process.hpp"

using namespace otok;

TEST(environment, reads_and_writes_the_three_forms)
{
    auto e = parse_environment("windows/msvc");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->system, "windows");
    EXPECT_EQ(e->arch, "");
    EXPECT_EQ(e->compiler, "msvc");
    EXPECT_EQ(e->text(), "windows/msvc");

    e = parse_environment("linux-arm64/clang-cl");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->system, "linux");
    EXPECT_EQ(e->arch, "arm64");
    EXPECT_EQ(e->text(), "linux-arm64/clang-cl");

    e = parse_environment("*/ndk");
    ASSERT_TRUE(e.has_value());
    EXPECT_TRUE(e->any_host());
    EXPECT_EQ(e->text(), "*/ndk");
}

TEST(environment, refuses_what_is_not_one)
{
    for (char const* text : {"", "msvc", "windows", "windows/", "/msvc", "solaris/gcc", "windows-mips/msvc", "windows-/msvc", "*-arm64/ndk",
             "windows/MSVC", "windows/ms vc", "windows/msvc/x"}) {
        EXPECT_FALSE(parse_environment(text).has_value()) << text;
    }
}

TEST(environment, covers_hosts_and_architectures)
{
    auto const env = [](char const* text) { return *parse_environment(text); };
    EXPECT_TRUE(covers(env("windows/msvc"), env("windows-arm64/msvc")));
    EXPECT_TRUE(covers(env("windows/msvc"), env("windows/msvc")));
    EXPECT_FALSE(covers(env("windows-arm64/msvc"), env("windows/msvc")));
    EXPECT_FALSE(covers(env("windows-arm64/msvc"), env("windows-x86_64/msvc")));
    EXPECT_FALSE(covers(env("windows/msvc"), env("windows/clang-cl")));
    EXPECT_FALSE(covers(env("linux/clang-cl"), env("windows/clang-cl")));
    EXPECT_TRUE(covers(env("*/ndk"), env("linux/ndk")));
    EXPECT_TRUE(covers(env("*/ndk"), env("*/ndk")));
    EXPECT_FALSE(covers(env("linux/ndk"), env("*/ndk")));
    EXPECT_FALSE(covers(env("*/ndk"), env("linux/gcc")));
}

TEST(environment, of_the_machine)
{
    build_environment const e = machine_environment("gcc");
    EXPECT_EQ(e.system + "-" + e.arch, host_machine());
    EXPECT_EQ(e.compiler, "gcc");
    EXPECT_TRUE(is_host_machine(host_machine()));
    EXPECT_FALSE(is_host_machine("macos"));
    EXPECT_FALSE(is_host_machine("macos-ppc"));
}
