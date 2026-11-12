// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// File system helpers of core: file locks, tree copies, atomic writes.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "otok/core/file_lock.hpp"
#include "otok/core/fs.hpp"
#include "test_util.hpp"

using namespace otok;

TEST(file_lock, excludes_other_holders)
{
    test::temp_dir dir;
    auto const path = dir.path() / "entry.lock";
    auto first = file_lock::acquire(path);
    ASSERT_TRUE(first.has_value()) << first.error().message;

    auto second = file_lock::try_acquire(path);  // also within one process
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_FALSE(second->has_value());

    first->release();
    auto third = file_lock::try_acquire(path);
    ASSERT_TRUE(third.has_value());
    EXPECT_TRUE(third->has_value());
}

TEST(file_lock, waits_for_the_holder)
{
    test::temp_dir dir;
    auto const path = dir.path() / "entry.lock";
    auto holder = file_lock::acquire(path);
    ASSERT_TRUE(holder.has_value());

    std::atomic<bool> waiting{false};
    std::atomic<bool> acquired{false};
    std::thread waiter{[&] {
        auto lock = file_lock::acquire(path, [&] { waiting = true; });
        acquired = lock.has_value();
    }};
    for (int i = 0; i < 500 && !waiting; ++i) std::this_thread::sleep_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(waiting);
    EXPECT_FALSE(acquired);
    holder->release();
    waiter.join();
    EXPECT_TRUE(acquired);
}

TEST(file_lock, shared_holders)
{
    test::temp_dir dir;
    auto const path = dir.path() / "cache.lock";
    auto first = file_lock::acquire(path, {}, lock_mode::shared);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    auto second = file_lock::try_acquire(path, lock_mode::shared);
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_TRUE(second->has_value());

    EXPECT_FALSE(file_lock::try_acquire(path).value().has_value());  // exclusive: busy while any shared holder remains
    first->release();
    EXPECT_FALSE(file_lock::try_acquire(path).value().has_value());
    second->reset();
    auto exclusive = file_lock::try_acquire(path);
    ASSERT_TRUE(exclusive.has_value());
    ASSERT_TRUE(exclusive->has_value());
    EXPECT_FALSE(file_lock::try_acquire(path, lock_mode::shared).value().has_value());
}

TEST(fs, copy_tree_keeps_contents_and_times)
{
    test::temp_dir dir;
    dir.write("src/configure", "#!/bin/sh\n");
    dir.write("src/lib/a.c", "int a;\n");
    std::filesystem::create_directories(dir.path() / "src" / "empty");
    auto const old_time = std::filesystem::last_write_time(dir.path() / "src" / "lib" / "a.c") - std::chrono::hours{24 * 365};
    std::filesystem::last_write_time(dir.path() / "src" / "lib" / "a.c", old_time);

    auto ok = copy_tree(dir.path() / "src", dir.path() / "copy");
    ASSERT_TRUE(ok.has_value()) << ok.error().message;
    EXPECT_EQ(read_file(dir.path() / "copy" / "lib" / "a.c").value_or("?"), "int a;\n");
    EXPECT_TRUE(std::filesystem::is_directory(dir.path() / "copy" / "empty"));
    auto const copied = std::filesystem::last_write_time(dir.path() / "copy" / "lib" / "a.c");
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(copied - old_time).count(), 0);

    // the copy is independent of the original
    dir.write("copy/lib/a.c", "changed\n");
    EXPECT_EQ(read_file(dir.path() / "src" / "lib" / "a.c").value_or("?"), "int a;\n");

    auto again = copy_tree(dir.path() / "src", dir.path() / "copy");
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().message, "'" + path_to_utf8(dir.path() / "copy") + "' already exists");
}

TEST(fs, copy_tree_copies_links_as_links)
{
    test::temp_dir dir;
    dir.write("src/a.h", "int a;\n");
    std::error_code ec;
    std::filesystem::create_symlink("a.h", dir.path() / "src" / "b.h", ec);
    if (ec) GTEST_SKIP() << "cannot create symbolic links here: " << ec.message();
    auto ok = copy_tree(dir.path() / "src", dir.path() / "copy");
    ASSERT_TRUE(ok.has_value()) << ok.error().message;
    EXPECT_TRUE(std::filesystem::is_symlink(dir.path() / "copy" / "b.h"));
    EXPECT_EQ(std::filesystem::read_symlink(dir.path() / "copy" / "b.h"), "a.h");
}

TEST(fs, copies_and_removes_trees_with_long_paths)
{
    // Boost's documentation has paths of more than 260 characters below its root
    test::temp_dir dir;
    std::filesystem::path deep = "src";
    for (int i = 0; i < 6; ++i) deep /= std::string(50, static_cast<char>('a' + i));
    std::filesystem::create_directories(long_path(dir.path() / deep));
    {
        std::ofstream{long_path(dir.path() / deep / "page.html"), std::ios::binary} << "deep";
    }
    ASSERT_GT(path_to_utf8(dir.path() / deep / "page.html").size(), 300u);

    auto ok = copy_tree(dir.path() / "src", dir.path() / "copy");
    ASSERT_TRUE(ok.has_value()) << ok.error().message;
    std::filesystem::path const copied = dir.path() / "copy" / deep.lexically_relative("src") / "page.html";
    EXPECT_EQ(read_file(long_path(copied)).value_or("?"), "deep");
    std::error_code ec;
    std::filesystem::remove_all(long_path(dir.path() / "copy"), ec);
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "copy"));
    std::filesystem::remove_all(long_path(dir.path() / "src"), ec);
#ifdef _WIN32
    EXPECT_EQ(path_to_utf8(long_path(path_from_utf8("C:/a/b/../c"))), "\\\\?\\C:\\a\\c");
    EXPECT_EQ(path_to_utf8(long_path(path_from_utf8("//server/share/x"))), "\\\\?\\UNC\\server\\share\\x");
#else
    EXPECT_EQ(long_path("/a/b"), std::filesystem::path{"/a/b"});
#endif
}

TEST(fs, first_difference)
{
    test::temp_dir dir;
    dir.write("a/x/1.h", "one");
    dir.write("a/2.h", "two");
    dir.write("b/x/1.h", "one");
    dir.write("b/2.h", "two");
    EXPECT_EQ(first_difference(dir.path() / "a", dir.path() / "b"), "");
    dir.write("b/2.h", "tow");
    EXPECT_EQ(first_difference(dir.path() / "a", dir.path() / "b"), "2.h");
    dir.write("b/2.h", "two");
    dir.write("b/x/3.h", "three");
    EXPECT_EQ(first_difference(dir.path() / "a", dir.path() / "b"), "x/3.h");
    EXPECT_EQ(first_difference(dir.path() / "b", dir.path() / "a"), "x/3.h");
}

TEST(fs, write_file_atomic)
{
    test::temp_dir dir;
    ASSERT_TRUE(write_file_atomic(dir.path() / "f", "one").has_value());
    ASSERT_TRUE(write_file_atomic(dir.path() / "f", "two").has_value());
    EXPECT_EQ(read_file(dir.path() / "f").value_or("?"), "two");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{dir.path()}, std::filesystem::directory_iterator{}), 1);  // no temporary left

    // threads replacing one file at once: each write is whole (the state of an index read by the projects otok publish
    // builds together)
    std::vector<std::thread> threads;
    std::atomic<int> failed{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 20; ++i) {
                if (!write_file_atomic(dir.path() / "g", std::string(100, static_cast<char>('a' + t)))) ++failed;
            }
        });
    }
    for (std::thread& t : threads) t.join();
    EXPECT_EQ(failed, 0);
    std::string const g = read_file(dir.path() / "g").value_or("");
    EXPECT_EQ(g, std::string(100, g.empty() ? '?' : g[0]));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{dir.path()}, std::filesystem::directory_iterator{}), 2);
}

TEST(fs, rename_path)
{
    test::temp_dir dir;
    dir.write("unpacked/file.txt", "x");
    std::error_code ec;
#ifdef _WIN32
    // a file open in the directory (an indexer reading what was just unpacked): Windows refuses to move the directory
    // until it is closed, which rename_path waits for
    auto held = std::make_unique<std::ifstream>(dir.path() / "unpacked" / "file.txt");
    ASSERT_TRUE(*held);
    std::filesystem::rename(dir.path() / "unpacked", dir.path() / "moved", ec);
    EXPECT_EQ(ec, std::errc::permission_denied) << ec.message();
    std::thread closer{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        held.reset();
    }};
    rename_path(dir.path() / "unpacked", dir.path() / "moved", ec);
    closer.join();
#else
    rename_path(dir.path() / "unpacked", dir.path() / "moved", ec);
#endif
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(read_file(dir.path() / "moved" / "file.txt").value_or("?"), "x");
    // what cannot be moved fails as std::filesystem::rename does
    rename_path(dir.path() / "missing", dir.path() / "elsewhere", ec);
    EXPECT_TRUE(ec);
}

TEST(fs, files_beyond_max_path)
{
    // the files of package entries and outputs on Windows: deep below a cache or a project (docs/tasks/windows-long-paths-otok-files.md)
    test::temp_dir dir;
    std::filesystem::path const deep = dir.path() / path_from_utf8(test::deep_path());
    std::filesystem::create_directories(long_path(deep));
    ASSERT_TRUE(write_file_atomic(deep / "f.txt", "one").has_value());
    EXPECT_EQ(read_file(deep / "f.txt").value_or("?"), "one");
    EXPECT_EQ(write_file_if_changed(deep / "f.txt", "one").value_or(true), false);
    EXPECT_EQ(write_file_if_changed(deep / "f.txt", "two").value_or(false), true);
    EXPECT_EQ(read_file(deep / "f.txt").value_or("?"), "two");
    std::error_code ec;
    rename_path(deep / "f.txt", deep / "g.txt", ec);
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(read_file(deep / "g.txt").value_or("?"), "two");

    // messages show the path as the user gave it
    auto missing = read_file(deep / "missing.txt");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().message, "cannot open '" + path_to_utf8(deep / "missing.txt") + "'");
#ifdef _WIN32
    EXPECT_EQ(path_to_utf8(plain_path(path_from_utf8(R"(\\?\C:\a\c)"))), R"(C:\a\c)");
    EXPECT_EQ(path_to_utf8(plain_path(path_from_utf8(R"(\\?\UNC\server\share\x)"))), R"(\\server\share\x)");
#endif
    EXPECT_EQ(plain_path(deep), deep);
}

TEST(fs, merges_trees_beyond_max_path)
{
    // the headers of the slices into one include/ (the Windows and Android targets): Boost's deepest is 90 characters below it
    test::temp_dir dir;
    std::string const deep = test::deep_path();
    dir.write("a/" + deep + "/x.h", "x");
    dir.write("a/top.h", "top");
    dir.write("b/" + deep + "/x.h", "x");
    dir.write("b/" + deep + "/y.h", "y");
    std::filesystem::path const to = dir.path() / path_from_utf8(deep) / "include";
    auto merged = merge_tree(dir.path() / "a", to);
    ASSERT_TRUE(merged) << merged.error().message;
    EXPECT_EQ(*merged, "");
    merged = merge_tree(dir.path() / "b", to);
    ASSERT_TRUE(merged) << merged.error().message;
    EXPECT_EQ(*merged, "");
    EXPECT_EQ(read_file(to / path_from_utf8(deep) / "x.h").value_or("?"), "x");
    EXPECT_EQ(read_file(to / path_from_utf8(deep) / "y.h").value_or("?"), "y");
    EXPECT_EQ(read_file(to / "top.h").value_or("?"), "top");

    // a file both have with other contents: returned, nothing more copied
    dir.write("c/" + deep + "/x.h", "other");
    merged = merge_tree(dir.path() / "c", to);
    ASSERT_TRUE(merged) << merged.error().message;
    EXPECT_EQ(*merged, deep + "/x.h");
    EXPECT_EQ(read_file(to / path_from_utf8(deep) / "x.h").value_or("?"), "x");
}

TEST(fs, compares_and_generates_trees_beyond_max_path)
{
    test::temp_dir dir;
    std::string const deep = test::deep_path();
    dir.write("a/" + deep + "/1.h", "one");
    dir.write("b/" + deep + "/1.h", "one");
    EXPECT_EQ(first_difference(dir.path() / "a", dir.path() / "b"), "");
    dir.write("b/" + deep + "/1.h", "eno");
    EXPECT_EQ(first_difference(dir.path() / "a", dir.path() / "b"), deep + "/1.h");

    // written, then removed when no longer generated
    std::vector<std::string> const owned{"site"};
    auto written = write_generated_files(dir.path(), {{"site/" + deep + "/page.html", "page"}, {"site/index.html", "index"}}, owned);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written->written, 2u);
    EXPECT_EQ(read_file(dir.path() / "site" / path_from_utf8(deep) / "page.html").value_or("?"), "page");
    written = write_generated_files(dir.path(), {{"site/index.html", "index"}}, owned);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written->unchanged, 1u);
    EXPECT_EQ(written->removed, 1u);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "site" / std::string(48, 'a')));  // the directories left empty too
}
