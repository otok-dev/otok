// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/core/sandbox.hpp"
#include "otok/core/virtual_drives.hpp"
#include "test_util.hpp"

using namespace otok;

#ifdef _WIN32

namespace {

// A directory whose files are beyond MAX_PATH, with `file.txt` in it.
std::filesystem::path long_directory(test::temp_dir const& dir, std::string const& name)
{
    std::filesystem::path const d = std::filesystem::canonical(dir.path()) / name / std::string(100, 'd') / std::string(100, 'e');
    std::filesystem::create_directories(long_path(d));
    std::ofstream{long_path(d / "file.txt"), std::ios::binary} << "long\n";
    return d;
}

// The temporary directory with what is beyond MAX_PATH in it, which its own removal does not reach.
void remove_long(test::temp_dir const& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(long_path(dir.path()), ec);
}

}

// docs/PLAN.md open item 23: the map is the process's own, so otok reaches the long directories through the letters too.
// Enabled once for the whole test process: every process it starts afterwards gets the map.
TEST(virtual_drives, give_long_directories_short_letters)
{
    test::temp_dir dir;
    std::filesystem::path const real = long_directory(dir, "a");
    ASSERT_GT(path_to_utf8(real / "file.txt").size(), 260u);
    ASSERT_TRUE(virtual_drives::enable());
    EXPECT_TRUE(virtual_drives::enabled());
    auto drive = virtual_drives::drive_for(real);
    ASSERT_TRUE(drive) << drive.error().message;
    std::wstring const root = drive->wstring();
    ASSERT_EQ(root.size(), 3u);
    EXPECT_EQ(root.substr(1), L":\\");
    // the same directory, the same letter; another, another letter
    EXPECT_EQ(virtual_drives::drive_for(real).value(), *drive);
    std::filesystem::path const other = long_directory(dir, "b");
    auto second = virtual_drives::drive_for(other);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(*second, *drive);
    // both ways, the deeper directory first
    EXPECT_EQ(virtual_drives::to_virtual(real / "x" / "y.c"), *drive / "x" / "y.c");
    EXPECT_EQ(virtual_drives::to_virtual(real), *drive);
    // with '/', as config.toml names the cache ("X:/Devel/otok-cache"), and a path appended to it with '\'
    std::filesystem::path const forward{std::wstring{real.generic_wstring()}};
    EXPECT_EQ(virtual_drives::to_virtual(forward / "x" / "y.c"), *drive / "x" / "y.c");
    EXPECT_EQ(virtual_drives::to_virtual(forward), *drive);
    EXPECT_EQ(virtual_drives::to_real(*drive / "x" / "y.c"), real / "x" / "y.c");
    EXPECT_EQ(virtual_drives::to_virtual(dir.path()), dir.path());
    EXPECT_EQ(virtual_drives::to_real(std::filesystem::path{"C:\\Windows"}), std::filesystem::path{"C:\\Windows"});
    // otok's own file operations
    auto content = read_file(*drive / "file.txt");
    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(*content, "long\n");
    EXPECT_FALSE(virtual_drives::drive_for(std::filesystem::path{"relative"}));
    // a letter lives while it is used: twice here, then gone
    virtual_drives::release(*drive);
    EXPECT_EQ(virtual_drives::to_virtual(real), *drive);
    virtual_drives::release(*drive);
    EXPECT_EQ(virtual_drives::to_virtual(real), real);
    EXPECT_FALSE(std::filesystem::exists(*drive / "file.txt"));
    virtual_drives::release(*second);
    remove_long(dir);
}

// The programs otok starts: the map given to each, and by otok-sandbox.dll to their children, outside the AppContainer
// (sandbox = false) and in it through otok's broker; GetFinalPathNameByHandleW gives the paths through the letter.
TEST(virtual_drives, reach_the_children)
{
    host_sandbox const host = find_host_sandbox({});
    ASSERT_EQ(host.kind, host_sandbox::mechanism::appcontainer) << host.why_not;
    test::temp_dir dir;
    std::filesystem::path const real = long_directory(dir, "c");
    ASSERT_TRUE(virtual_drives::enable());
    auto drive = virtual_drives::drive_for(real);
    ASSERT_TRUE(drive) << drive.error().message;
    std::filesystem::path const child = path_from_utf8(OTOK_TEST_CHILD);
    sandbox_spec spec;
    spec.writable = {*drive};
    spec.readable = {child.parent_path()};
    ASSERT_TRUE(grant_sandbox_access(host, spec));
    std::string const file = path_to_utf8(*drive / "file.txt");
    std::string const through = "\\\\?\\" + file + "\n";
    env_vars const env{{"PATH", "C:\\Windows\\System32"}, {"SystemRoot", "C:\\Windows"}, {"TEMP", path_to_utf8(*drive)}, {"TMP", path_to_utf8(*drive)}};
    auto const run = [&](std::vector<std::string> argv, std::shared_ptr<sandbox_launch const> launch) {
        process_spec s{argv, env, *drive, *drive / "log.txt", true};
        s.sandbox = std::move(launch);
        auto exit = run_process(s);
        EXPECT_TRUE(exit) << exit.error().message;
        return exit ? exit->output : std::string{"?"};
    };
    // a program started by otok, in a working directory on the letter
    EXPECT_EQ(run({path_to_utf8(child), "cwd"}, nullptr), path_to_utf8(*drive) + "\n");
    for (std::string const& container : {std::string{}, host.container}) {
        auto const launch = std::make_shared<sandbox_launch const>(sandbox_launch{container, host.program, *drive / "sandbox.log", *drive});
        SCOPED_TRACE(container.empty() ? "without the container" : "in the container");
        EXPECT_EQ(run({path_to_utf8(child), "final", "file.txt"}, launch), through);
        // a child's child
        EXPECT_EQ(run({"cmd.exe", "/d", "/c", path_to_utf8(child) + " final file.txt"}, launch), through);
        EXPECT_EQ(run({"cmd.exe", "/d", "/c", "cmd.exe /d /c type " + file}, launch), "long\n");
        // the volume of the letter's root, which the container may not open (perlglob asks it)
        EXPECT_EQ(run({path_to_utf8(child), "volume", path_to_utf8(*drive)}, launch), "preserves case\n");
    }
    virtual_drives::release(*drive);
    remove_long(dir);
}

#else

TEST(virtual_drives, exist_on_windows_only)
{
    EXPECT_FALSE(virtual_drives::enable());
    EXPECT_FALSE(virtual_drives::enabled());
    EXPECT_EQ(virtual_drives::to_virtual("/a/b"), std::filesystem::path{"/a/b"});
    EXPECT_EQ(virtual_drives::to_real("/a/b"), std::filesystem::path{"/a/b"});
}

#endif
