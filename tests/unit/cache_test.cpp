// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The cache directory: docs/formats/cache.md.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include "otok/cache/store.hpp"
#include "otok/core/file_lock.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

std::string const abc_sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::size_t count_entries(std::filesystem::path const& dir)
{
    std::error_code ec;
    std::size_t n = 0;
    for ([[maybe_unused]] auto const& e : std::filesystem::directory_iterator{dir, ec}) ++n;
    return n;
}

void write_source_archive(std::filesystem::path const& file)
{
    test::write_tar_gz(file, {{"pkg-1.0/README", "readme\n"}, {"pkg-1.0/src/main.c", "int main;\n"}});
}

}

TEST(cache, opens_and_marks_the_directory)
{
    test::temp_dir dir;
    std::filesystem::path const root = dir.path() / "cache";
    std::filesystem::path tmp;
    {
        auto cache = cache_store::open(root);
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        EXPECT_EQ(read_file(root / "layout").value_or("?"), "1\n");
        EXPECT_EQ(read_file(root / "CACHEDIR.TAG").value_or("?").substr(0, 43), "Signature: 8a477f597d28d172789f06886806bc55");
        tmp = cache->tmp();
        EXPECT_TRUE(std::filesystem::is_directory(tmp));
        EXPECT_EQ(tmp.parent_path(), root / "tmp");
    }
    EXPECT_FALSE(std::filesystem::exists(tmp));  // removed when the process is done with the cache
    auto again = cache_store::open(root);
    EXPECT_TRUE(again.has_value()) << again.error().message;
}

TEST(cache, refuses_foreign_and_newer_directories)
{
    test::temp_dir dir;
    dir.write("photos/cat.jpg", "jpeg");
    auto foreign = cache_store::open(dir.path() / "photos");
    ASSERT_FALSE(foreign.has_value());
    EXPECT_EQ(foreign.error().message, "'" + path_to_utf8(dir.path() / "photos")
        + "' is not empty and does not look like an otok cache (it contains 'cat.jpg'); set cache-dir to another directory");
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "photos" / "layout"));

    dir.write("newer/layout", "2\n");
    auto newer = cache_store::open(dir.path() / "newer");
    ASSERT_FALSE(newer.has_value());
    EXPECT_EQ(newer.error().message.find("the cache '" + path_to_utf8(dir.path() / "newer") + "' has layout 2, which is newer"), 0u)
        << newer.error().message;
}

TEST(cache, removes_tmp_directories_of_finished_processes)
{
    test::temp_dir dir;
    std::filesystem::path const root = dir.path() / "cache";
    dir.write("cache/tmp/111-dead/partial.tar.gz", "x");      // crashed: lock file left, not held
    dir.write("cache/tmp/111-dead.lock", "");
    dir.write("cache/tmp/222-nolock/x", "x");                 // no lock file at all
    dir.write("cache/tmp/333-alive/x", "x");                  // another process still running
    auto alive = file_lock::acquire(root / "tmp" / "333-alive.lock");
    ASSERT_TRUE(alive.has_value());
    // being made by another cache_store, which has created its lock file and not locked it yet
    dir.write("cache/tmp/444-starting.lock", "");
    dir.write("cache/tmp/444-starting/x", "x");
    dir.write("cache/tmp/555-starting/x", "x");                // its lock file not listed yet
    // the dead ones, older than a minute
    auto const old = std::filesystem::file_time_type::clock::now() - std::chrono::minutes{5};
    for (char const* name : {"111-dead", "111-dead.lock", "222-nolock"}) std::filesystem::last_write_time(root / "tmp" / name, old);

    auto cache = cache_store::open(root);
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    EXPECT_FALSE(std::filesystem::exists(root / "tmp" / "111-dead"));
    EXPECT_FALSE(std::filesystem::exists(root / "tmp" / "111-dead.lock"));
    EXPECT_FALSE(std::filesystem::exists(root / "tmp" / "222-nolock"));
    EXPECT_TRUE(std::filesystem::exists(root / "tmp" / "333-alive" / "x"));
    EXPECT_TRUE(std::filesystem::exists(root / "tmp" / "444-starting.lock"));
    EXPECT_TRUE(std::filesystem::exists(root / "tmp" / "444-starting" / "x"));
    EXPECT_TRUE(std::filesystem::exists(root / "tmp" / "555-starting" / "x"));
}

TEST(cache, fetches_downloads_once)
{
    test::temp_dir dir;
    dir.write("upstream/abc.txt", "abc");
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    std::string const url = file_url(dir.path() / "upstream" / "abc.txt");

    std::vector<cache_download> const downloads{{url, abc_sha256}, {url, abc_sha256}};
    auto results = cache->fetch(downloads, {});
    ASSERT_EQ(results.size(), 2u);
    ASSERT_TRUE(results[0].has_value()) << results[0].error().message;
    ASSERT_TRUE(results[1].has_value()) << results[1].error().message;
    EXPECT_EQ(*results[0], cache->download_path(abc_sha256));
    EXPECT_EQ(read_file(*results[0]).value_or("?"), "abc");
    EXPECT_EQ(count_entries(cache->tmp()), 0u);

    // present: not downloaded again (the upstream file is gone)
    std::filesystem::remove(dir.path() / "upstream" / "abc.txt");
    auto again = cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {});
    ASSERT_TRUE(again[0].has_value()) << again[0].error().message;

    // a wrong hash leaves nothing in the cache
    dir.write("upstream/other.txt", "other");
    std::string const wrong(64, '1');
    auto bad = cache->fetch(std::vector<cache_download>{{file_url(dir.path() / "upstream" / "other.txt"), wrong}}, {});
    ASSERT_FALSE(bad[0].has_value());
    EXPECT_NE(bad[0].error().message.find("does not match its sha256"), std::string::npos) << bad[0].error().message;
    EXPECT_FALSE(std::filesystem::exists(cache->download_path(wrong)));
}

TEST(cache, downloads_from_mirrors)
{
    test::temp_dir dir;
    dir.write("wrong/abc.txt", "not abc");
    dir.write("right/abc.txt", "abc");
    std::vector<std::string> fetching;
    auto cache = cache_store::open(dir.path() / "cache", {.fetching = [&](std::string const& what) { fetching.push_back(what); }});
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    std::string const gone = file_url(dir.path() / "gone" / "abc.txt");
    std::string const wrong = file_url(dir.path() / "wrong" / "abc.txt");
    std::string const right = file_url(dir.path() / "right" / "abc.txt");

    // the url fails, the first mirror has another file, the second gives it
    auto r = cache->fetch(std::vector<cache_download>{{gone, abc_sha256, {wrong, right}}}, {});
    ASSERT_TRUE(r[0].has_value()) << r[0].error().message;
    EXPECT_EQ(read_file(*r[0]).value_or("?"), "abc");
    EXPECT_EQ(fetching, (std::vector<std::string>{"downloading " + gone, "downloading " + wrong, "downloading " + right}));
    EXPECT_EQ(count_entries(cache->tmp()), 0u);

    // none gives it: every location is named
    std::string const other(64, '3');
    auto bad = cache->fetch(std::vector<cache_download>{{gone, other, {wrong}}}, {});
    ASSERT_FALSE(bad[0].has_value());
    std::string const& message = bad[0].error().message;
    EXPECT_EQ(message.find("no location of " + gone + " gave the file:\n  cannot download " + gone + ": "), 0u) << message;
    EXPECT_NE(message.find("\n  the download of " + wrong + " does not match its sha256:\n      expected " + other), std::string::npos) << message;
    EXPECT_FALSE(std::filesystem::exists(cache->download_path(other)));
}

TEST(cache, machine_mirrors_come_first)
{
    test::temp_dir dir;
    dir.write("upstream/abc.txt", "abc");
    dir.write("assets/" + abc_sha256, "abc");
    dir.write("proxy/abc.txt", "abc");
    std::string const url = file_url(dir.path() / "upstream" / "abc.txt");
    std::vector<std::string> fetching;
    auto const fresh = [&](std::string const& name) {
        fetching.clear();
        return cache_store::open(dir.path() / name, {.fetching = [&](std::string const& what) { fetching.push_back(what); }});
    };

    {
        auto cache = fresh("cache-assets");
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        cache->set_mirrors({.machine = {{file_url(dir.path() / "assets") + "/{sha256}"}, {}, false}});
        ASSERT_TRUE(cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {})[0].has_value());
        EXPECT_EQ(fetching, std::vector<std::string>{"downloading " + file_url(dir.path() / "assets" / abc_sha256)});
    }
    {
        // a rewritten URL before the recipe's
        auto cache = fresh("cache-rewrite");
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        cache->set_mirrors({.machine = {{}, {{file_url(dir.path() / "upstream"), file_url(dir.path() / "proxy")}}, false}});
        ASSERT_TRUE(cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {})[0].has_value());
        EXPECT_EQ(fetching, std::vector<std::string>{"downloading " + file_url(dir.path() / "proxy" / "abc.txt")});
    }
    {
        // mirrors-only: never the recipe's locations
        auto cache = fresh("cache-only");
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        cache->set_mirrors({.machine = {{}, {}, true}});
        auto r = cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {});
        ASSERT_FALSE(r[0].has_value());
        EXPECT_EQ(r[0].error().message, "no asset-mirror or url-rewrite covers " + url + ", and mirrors-only is set");
        EXPECT_TRUE(fetching.empty());

        cache->set_mirrors({.machine = {{file_url(dir.path() / "empty") + "/{sha256}"}, {}, true}});
        r = cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {});
        ASSERT_FALSE(r[0].has_value());
        EXPECT_EQ(fetching, std::vector<std::string>{"downloading " + file_url(dir.path() / "empty" / abc_sha256)});
    }
}

TEST(cache, project_locations_and_libraries)
{
    test::temp_dir dir;
    dir.write("upstream/abc.txt", "abc");
    dir.write("mine/abc.txt", "abc");
    dir.write("corp/" + abc_sha256, "abc");
    std::string const url = file_url(dir.path() / "upstream" / "abc.txt");
    url_rewrite const mine{url, file_url(dir.path() / "mine" / "abc.txt")};
    input_mirrors const corp{{file_url(dir.path() / "corp") + "/{sha256}"}};
    std::vector<std::string> fetching;
    auto const fresh = [&](std::string const& name) {
        fetching.clear();
        return cache_store::open(dir.path() / name, {.fetching = [&](std::string const& what) { fetching.push_back(what); }});
    };

    {
        // the project's rewrite before the machine's mirror
        auto cache = fresh("cache-project");
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        cache->set_mirrors({{{}, {mine}}, corp});
        ASSERT_TRUE(cache->fetch(std::vector<cache_download>{{url, abc_sha256, {}, "abc"}}, {})[0].has_value());
        EXPECT_EQ(fetching, std::vector<std::string>{"downloading " + mine.to});
    }
    {
        // the machine's mirrors-only for this library: not the project's locations
        auto cache = fresh("cache-corp");
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        input_mirrors strict = corp;
        strict.mirrors_only_libraries = {"abc"};
        cache->set_mirrors({{{}, {mine}}, strict});
        ASSERT_TRUE(cache->fetch(std::vector<cache_download>{{url, abc_sha256, {}, "abc"}}, {})[0].has_value());
        EXPECT_EQ(fetching, std::vector<std::string>{"downloading " + file_url(dir.path() / "corp" / abc_sha256)});
    }
}

TEST(cache, concurrent_processes_share_entries)
{
    test::temp_dir dir;
    dir.write("upstream/abc.txt", "abc");
    std::string const url = file_url(dir.path() / "upstream" / "abc.txt");
    auto run = [&] {
        auto cache = cache_store::open(dir.path() / "cache");
        if (!cache) return std::string{"open: "} + cache.error().message;
        auto r = cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {});
        return r[0] ? std::string{"ok"} : r[0].error().message;
    };
    std::vector<std::future<std::string>> runs;
    for (int i = 0; i < 6; ++i) runs.push_back(std::async(std::launch::async, run));
    for (auto& r : runs) EXPECT_EQ(r.get(), "ok");
}

TEST(cache, unpacks_archives_once)
{
    test::temp_dir dir;
    write_source_archive(dir.path() / "pkg-1.0.tar.gz");
    std::string const sha256 = sha256_file(dir.path() / "pkg-1.0.tar.gz").value_or("?");
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    auto fetched = cache->fetch(std::vector<cache_download>{{file_url(dir.path() / "pkg-1.0.tar.gz"), sha256}}, {});
    ASSERT_TRUE(fetched[0].has_value()) << fetched[0].error().message;

    auto source = cache->unpacked_archive(sha256);
    ASSERT_TRUE(source.has_value()) << source.error().message;
    EXPECT_EQ(*source, cache->root() / "sources" / ("archive-" + sha256));
    EXPECT_EQ(read_file(*source / "src" / "main.c").value_or("?"), "int main;\n");
    auto again = cache->unpacked_archive(sha256);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, *source);

    // a slice gets its own copy
    auto copied = copy_tree(*source, dir.path() / "slice" / "source");
    ASSERT_TRUE(copied.has_value()) << copied.error().message;
    dir.write("slice/source/src/main.c", "patched\n");
    EXPECT_EQ(read_file(*source / "src" / "main.c").value_or("?"), "int main;\n");
}

TEST(cache, failed_entries_leave_nothing)
{
    test::temp_dir dir;
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    std::filesystem::path const entry = cache->root() / "sources" / "broken";
    auto r = cache->ensure(entry, "broken", [](std::filesystem::path const& staging) -> result<void> {
        std::filesystem::create_directories(staging / "half");
        return fail("no luck");
    });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message, "no luck");
    EXPECT_FALSE(std::filesystem::exists(entry));
    EXPECT_EQ(count_entries(cache->tmp()), 0u);

    auto missing = cache->unpacked_archive(std::string(64, '2'));  // not downloaded
    EXPECT_FALSE(missing.has_value());
}

namespace {

// Runs git in `dir` for setting up test repositories; fails the test on error.
std::string git_in(git_tool const& git, std::filesystem::path const& dir, std::vector<std::string> args)
{
    std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false",
        "-c", "protocol.file.allow=always", "-c", "init.defaultBranch=main"};
    full.insert(full.end(), args.begin(), args.end());
    auto out = run_git(git, full, dir, dir.parent_path() / "setup.log", true);
    EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message);
    std::string text = out.value_or("");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

struct repositories
{
    std::string lib_url, lib_commit, sub_commit;
};

// dir/sub: include/sub.h. dir/lib: files that test the export, and deps/sub as a submodule with the
// relative URL ../sub.
repositories make_repositories(git_tool const& git, test::temp_dir const& dir)
{
    dir.write("sub/include/sub.h", "int sub;\n");
    git_in(git, dir.path() / "sub", {"init", "-q"});
    git_in(git, dir.path() / "sub", {"add", "."});
    git_in(git, dir.path() / "sub", {"commit", "-q", "-m", "sub"});

    dir.write("lib/crlf.txt", "a\nb\n");        // checked out as CRLF by git; the object has LF
    dir.write("lib/ignored.txt", "kept\n");     // export-ignore must not apply
    dir.write("lib/run.sh", "#!/bin/sh\n");
    dir.write("lib/.gitattributes", "crlf.txt text eol=crlf\nignored.txt export-ignore\n");
    std::filesystem::path const lib = dir.path() / "lib";
    git_in(git, lib, {"init", "-q"});
    git_in(git, lib, {"add", "."});
    git_in(git, lib, {"update-index", "--chmod=+x", "run.sh"});
    git_in(git, lib, {"submodule", "add", "-q", file_url(dir.path() / "sub"), "deps/sub"});
    git_in(git, lib, {"config", "-f", ".gitmodules", "submodule.deps/sub.url", "../sub"});
    git_in(git, lib, {"add", ".gitmodules"});
    git_in(git, lib, {"commit", "-q", "-m", "lib"});
    std::string const first = git_in(git, lib, {"rev-parse", "HEAD"});
    // v1.0 (annotated) names the commit the tests use; v2.0 a later one
    git_in(git, lib, {"tag", "-a", "v1.0", "-m", "release 1.0"});
    dir.write("lib/second.txt", "2\n");
    git_in(git, lib, {"add", "second.txt"});
    git_in(git, lib, {"commit", "-q", "-m", "second"});
    git_in(git, lib, {"tag", "v2.0"});
    return {file_url(lib), first, git_in(git, dir.path() / "sub", {"rev-parse", "HEAD"})};
}

}

TEST(cache, git_trees)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    repositories const r = make_repositories(*git, dir);
    ASSERT_EQ(r.lib_commit.size(), 40u);
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;

    auto tree = cache->git_tree(r.lib_url, r.lib_commit, {}, false, *git);
    ASSERT_TRUE(tree.has_value()) << tree.error().message;
    EXPECT_EQ(*tree, cache->root() / "sources" / ("git-" + r.lib_commit));
    EXPECT_EQ(read_file(*tree / "crlf.txt").value_or("?"), "a\nb\n");     // the bytes of the object
    EXPECT_EQ(read_file(*tree / "ignored.txt").value_or("?"), "kept\n");
    EXPECT_FALSE(std::filesystem::exists(*tree / ".git"));
    EXPECT_FALSE(std::filesystem::exists(*tree / "deps" / "sub" / "include"));
#ifndef _WIN32
    auto const perms = std::filesystem::status(*tree / "run.sh").permissions();
    EXPECT_NE(perms & std::filesystem::perms::owner_exec, std::filesystem::perms::none);
#endif

    auto with_submodules = cache->git_tree(r.lib_url, r.lib_commit, {}, true, *git);
    ASSERT_TRUE(with_submodules.has_value()) << with_submodules.error().message;
    EXPECT_EQ(with_submodules->filename(), "git-" + r.lib_commit + "-submodules");
    EXPECT_EQ(read_file(*with_submodules / "deps" / "sub" / "include" / "sub.h").value_or("?"), "int sub;\n");
    // the submodule's tree is an entry of its own, top-level directory kept
    EXPECT_TRUE(std::filesystem::exists(cache->root() / "sources" / ("git-" + r.sub_commit + "-submodules") / "include" / "sub.h"));

    // everything is in the cache now: the upstream repositories are no longer needed
    std::filesystem::remove_all(dir.path() / "lib");
    std::filesystem::remove_all(dir.path() / "sub");
    auto again = cache->git_tree(r.lib_url, r.lib_commit, {}, true, *git);
    ASSERT_TRUE(again.has_value()) << again.error().message;
}

// cache.md 5: the directory of a recipe alone, not the repository around it (docs/PLAN.md decision 46).
TEST(cache, git_directories)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    dir.write("repo/recipes/a/recipe.toml", "a\n");
    dir.write("repo/recipes/a/patches/fix.patch", "p\n");
    dir.write("repo/library.c", "int library;\n");
    git_in(*git, dir.path() / "repo", {"init", "-q"});
    git_in(*git, dir.path() / "repo", {"add", "."});
    git_in(*git, dir.path() / "repo", {"commit", "-q", "-m", "recipes"});
    std::string const commit = git_in(*git, dir.path() / "repo", {"rev-parse", "HEAD"});
    std::string const url = file_url(dir.path() / "repo");
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;

    auto recipe = cache->git_directory(url, commit, {}, "recipes/a", *git);
    ASSERT_TRUE(recipe.has_value()) << recipe.error().message;
    EXPECT_EQ(*recipe, cache->root() / "sources" / ("git-" + commit + "-94a1a5be95274cf3"));  // sha256("recipes/a")
    EXPECT_EQ(read_file(*recipe / "recipe.toml").value_or("?"), "a\n");
    EXPECT_EQ(read_file(*recipe / "patches" / "fix.patch").value_or("?"), "p\n");
    EXPECT_FALSE(std::filesystem::exists(*recipe / "library.c"));
    EXPECT_FALSE(std::filesystem::exists(cache->root() / "sources" / ("git-" + commit)));

    auto whole = cache->git_directory(url, commit, {}, "", *git);
    ASSERT_TRUE(whole.has_value()) << whole.error().message;
    EXPECT_EQ(*whole, cache->root() / "sources" / ("git-" + commit));

    auto missing = cache->git_directory(url, commit, {}, "recipes/zz", *git);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message.find("the commit " + commit + " of " + url + " has no directory 'recipes/zz'"), 0u) << missing.error().message;
}

TEST(cache, git_errors)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    repositories const r = make_repositories(*git, dir);
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;

    std::string const unknown(40, 'a');
    auto missing = cache->git_tree(r.lib_url, unknown, {}, false, *git);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "the repository " + r.lib_url + " has no commit " + unknown);
    EXPECT_FALSE(std::filesystem::exists(cache->root() / "sources" / ("git-" + unknown)));

    auto nowhere = cache->git_tree(file_url(dir.path() / "nowhere"), r.lib_commit, {}, false, *git);
    ASSERT_FALSE(nowhere.has_value());
    EXPECT_EQ(nowhere.error().message.find("cannot fetch commit " + r.lib_commit + " from file://"), 0u) << nowhere.error().message;
}

TEST(cache, git_tags)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    repositories const r = make_repositories(*git, dir);

    {
        auto cache = cache_store::open(dir.path() / "cache-tag");
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        auto tree = cache->git_tree(r.lib_url, r.lib_commit, "v1.0", false, *git);  // an annotated tag
        ASSERT_TRUE(tree.has_value()) << tree.error().message;
        EXPECT_EQ(read_file(*tree / "crlf.txt").value_or("?"), "a\nb\n");
        EXPECT_FALSE(std::filesystem::exists(*tree / "second.txt"));
    }
    {
        // a tag pointing elsewhere is an error, never a reason to build another commit
        auto cache = cache_store::open(dir.path() / "cache-moved");
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        auto moved = cache->git_tree(r.lib_url, r.lib_commit, "v2.0", false, *git);
        ASSERT_FALSE(moved.has_value());
        EXPECT_NE(moved.error().message.find("the tag 'v2.0' of " + r.lib_url + " points to "), std::string::npos) << moved.error().message;
        EXPECT_NE(moved.error().message.find(", not to " + r.lib_commit + "; it was moved upstream"), std::string::npos) << moved.error().message;
    }
    {
        // a tag deleted upstream: the commit is fetched by its hash
        auto cache = cache_store::open(dir.path() / "cache-gone");
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        auto tree = cache->git_tree(r.lib_url, r.lib_commit, "v0.9", false, *git);
        ASSERT_TRUE(tree.has_value()) << tree.error().message;
    }
}

TEST(cache, git_url_rewrite)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    repositories const r = make_repositories(*git, dir);
    std::string const elsewhere = file_url(dir.path() / "elsewhere" / "lib");
    std::vector<std::string> fetching;
    auto cache = cache_store::open(dir.path() / "cache", {.fetching = [&](std::string const& what) { fetching.push_back(what); }});
    ASSERT_TRUE(cache.has_value()) << cache.error().message;

    // the lock names a repository that is gone; the machine knows where it is now
    cache->set_mirrors({.machine = {{}, {{elsewhere, r.lib_url}}, true}});
    auto tree = cache->git_tree(elsewhere, r.lib_commit, "v1.0", false, *git);
    ASSERT_TRUE(tree.has_value()) << tree.error().message;
    EXPECT_EQ(fetching, std::vector<std::string>{"fetching " + r.lib_url + " (v1.0)"});
    // the repository belongs to the URL of the lock
    EXPECT_TRUE(std::filesystem::exists(cache->root() / "git" / git_url_key(elsewhere)));

    // a rewrite that fails moves on to the URL itself (another repository: the tree is exported again)
    std::filesystem::remove_all(cache->root() / "sources" / ("git-" + r.lib_commit));
    fetching.clear();
    cache->set_mirrors({.machine = {{}, {{r.lib_url, elsewhere}}, false}});
    auto second = cache->git_tree(r.lib_url, r.lib_commit, {}, false, *git);
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(fetching, (std::vector<std::string>{"fetching " + elsewhere + " (" + r.lib_commit + ")", "fetching " + r.lib_url + " (" + r.lib_commit + ")"}));

    // mirrors-only without a rule
    cache->set_mirrors({.machine = {{}, {}, true}});
    auto none = cache->git_tree(file_url(dir.path() / "sub"), r.sub_commit, {}, false, *git);
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().message, "no url-rewrite covers " + file_url(dir.path() / "sub") + ", and mirrors-only is set");

    // every location fails: both are named
    cache->set_mirrors({.machine = {{}, {{file_url(dir.path() / "sub"), elsewhere}}, false}});
    std::string const unknown(40, 'b');
    auto missing = cache->git_tree(file_url(dir.path() / "sub"), unknown, {}, false, *git);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message.find("no location of " + file_url(dir.path() / "sub") + " gave commit " + unknown + ":\n  cannot fetch commit "), 0u)
        << missing.error().message;
}

TEST(cache, offline)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    repositories const r = make_repositories(*git, dir);
    dir.write("upstream/abc.txt", "abc");
    std::string const url = file_url(dir.path() / "upstream" / "abc.txt");
    std::vector<std::string> fetching;
    auto cache = cache_store::open(dir.path() / "cache", {.fetching = [&](std::string const& what) { fetching.push_back(what); }});
    ASSERT_TRUE(cache.has_value()) << cache.error().message;

    // nothing is fetched, file:// and local repositories included
    cache->set_offline(true);
    auto missing = cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {});
    ASSERT_FALSE(missing[0].has_value());
    EXPECT_EQ(missing[0].error().message, url + " is not in the cache (--offline)");
    auto no_repo = cache->git_tree(r.lib_url, r.lib_commit, {}, false, *git);
    ASSERT_FALSE(no_repo.has_value());
    EXPECT_EQ(no_repo.error().message, "commit " + r.lib_commit + " of " + r.lib_url + " is not in the cache (--offline)");
    EXPECT_TRUE(fetching.empty());

    cache->set_offline(false);
    ASSERT_TRUE(cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {})[0].has_value());
    ASSERT_TRUE(cache->git_tree(r.lib_url, r.lib_commit, "v1.0", false, *git).has_value());
    EXPECT_EQ(fetching, (std::vector<std::string>{"downloading " + url, "fetching " + r.lib_url + " (v1.0)"}));

    // what can be derived from the cache is created offline: a commit present in the repository is
    // exported (with submodules: the submodule was never fetched)
    cache->set_offline(true);
    EXPECT_TRUE(cache->fetch(std::vector<cache_download>{{url, abc_sha256}}, {})[0].has_value());
    std::filesystem::remove_all(cache->root() / "sources" / ("git-" + r.lib_commit));
    auto exported = cache->git_tree(r.lib_url, r.lib_commit, {}, false, *git);
    ASSERT_TRUE(exported.has_value()) << exported.error().message;
    auto without_sub = cache->git_tree(r.lib_url, r.lib_commit, {}, true, *git);
    ASSERT_FALSE(without_sub.has_value());
    EXPECT_EQ(without_sub.error().message,
        "submodule 'deps/sub': commit " + r.sub_commit + " of " + resolve_submodule_url(r.lib_url, "../sub") + " is not in the cache (--offline)");
    EXPECT_EQ(fetching.size(), 2u);
}

TEST(cache, clean)
{
    test::temp_dir dir;
    write_source_archive(dir.path() / "pkg-1.0.tar.gz");
    std::string const sha256 = sha256_file(dir.path() / "pkg-1.0.tar.gz").value_or("?");
    std::uintmax_t const archive_size = std::filesystem::file_size(dir.path() / "pkg-1.0.tar.gz");
    std::filesystem::path const root = dir.path() / "cache";
    auto fill = [&] {
        auto cache = cache_store::open(root);
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        ASSERT_TRUE(cache->fetch(std::vector<cache_download>{{file_url(dir.path() / "pkg-1.0.tar.gz"), sha256}}, {})[0].has_value());
        ASSERT_TRUE(cache->unpacked_archive(sha256).has_value());
        dir.write("cache/builds/pkg/1.0-r1/slices/k/lib.a", "12345");
        dir.write("cache/failed/pkg-native-x86_64-20261024-120000/build.log", "log");
        auto refused = cache->clean({}, false);  // shared access
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().message, "cleaning needs the cache opened for exclusive access");
    };
    fill();

    auto cache = cache_store::open(root, {}, cache_access::exclusive);
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    auto measured = cache->clean({}, true);
    ASSERT_TRUE(measured.has_value()) << measured.error().message;
    ASSERT_EQ(measured->size(), 4u);  // no git/
    EXPECT_EQ((*measured)[0].name, "downloads");
    EXPECT_EQ((*measured)[0].bytes, archive_size);  // lock files are empty
    EXPECT_EQ((*measured)[1].name, "sources");
    EXPECT_EQ((*measured)[1].bytes, 17u);  // "readme\n", "int main;\n"
    EXPECT_EQ((*measured)[2].name, "builds");
    EXPECT_EQ((*measured)[2].bytes, 5u);
    EXPECT_EQ((*measured)[3].name, "failed");
    EXPECT_EQ((*measured)[3].bytes, 3u);
    EXPECT_TRUE(std::filesystem::exists(root / "sources"));  // a dry run

    auto sources = cache->clean({.sources = true}, false);
    ASSERT_TRUE(sources.has_value()) << sources.error().message;
    ASSERT_EQ(sources->size(), 1u);
    EXPECT_FALSE(std::filesystem::exists(root / "sources"));
    EXPECT_TRUE(std::filesystem::exists(cache->download_path(sha256)));
    EXPECT_EQ(count_entries(cache->tmp()), 0u);

    auto rest = cache->clean({}, false);
    ASSERT_TRUE(rest.has_value()) << rest.error().message;
    EXPECT_EQ(rest->size(), 3u);
    EXPECT_FALSE(std::filesystem::exists(root / "downloads"));
    EXPECT_FALSE(std::filesystem::exists(root / "builds"));
    EXPECT_FALSE(std::filesystem::exists(root / "failed"));
    EXPECT_EQ(read_file(root / "layout").value_or("?"), "1\n");  // the markers stay
    EXPECT_TRUE(std::filesystem::exists(root / "CACHEDIR.TAG"));
    EXPECT_TRUE(cache->clean({}, false)->empty());
}

TEST(cache, clean_removes_git_repositories)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    repositories const r = make_repositories(*git, dir);
    std::filesystem::path const root = dir.path() / "cache";
    {
        auto cache = cache_store::open(root);
        ASSERT_TRUE(cache.has_value()) << cache.error().message;
        ASSERT_TRUE(cache->git_tree(r.lib_url, r.lib_commit, {}, true, *git).has_value());
    }
    auto cache = cache_store::open(root, {}, cache_access::exclusive);
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    auto cleaned = cache->clean({.sources = true}, false);  // git objects are read-only files
    ASSERT_TRUE(cleaned.has_value()) << cleaned.error().message;
    ASSERT_EQ(cleaned->size(), 2u);
    EXPECT_EQ((*cleaned)[0].name, "git");
    EXPECT_FALSE(std::filesystem::exists(root / "git"));
    EXPECT_FALSE(std::filesystem::exists(root / "sources"));
}

TEST(cache, exclusive_access_waits_for_other_processes)
{
    test::temp_dir dir;
    std::filesystem::path const root = dir.path() / "cache";
    auto user = cache_store::open(root);
    ASSERT_TRUE(user.has_value()) << user.error().message;
    auto other = cache_store::open(root);  // shared: no waiting
    ASSERT_TRUE(other.has_value()) << other.error().message;
    other = std::unexpected{error{"closed"}};

    std::atomic<bool> waiting{false};
    std::atomic<bool> opened{false};
    std::thread cleaner{[&] {
        auto cache = cache_store::open(root, {.waiting = [&](std::string const& what) {
            EXPECT_EQ(what, "waiting for other otok processes to finish with the cache");
            waiting = true;
        }}, cache_access::exclusive);
        opened = cache.has_value();
        if (!cache) return;
        // while it has the cache, a new process waits in turn
        std::atomic<bool> second_waiting{false};
        std::thread second{[&] {
            auto s = cache_store::open(root, {.waiting = [&](std::string const& what) {
                EXPECT_EQ(what, "waiting for 'otok clean' to finish");
                second_waiting = true;
            }});
            EXPECT_TRUE(s.has_value());
        }};
        for (int i = 0; i < 500 && !second_waiting; ++i) std::this_thread::sleep_for(std::chrono::milliseconds{10});
        EXPECT_TRUE(second_waiting);
        cache = std::unexpected{error{"closed"}};
        second.join();
    }};
    for (int i = 0; i < 500 && !waiting; ++i) std::this_thread::sleep_for(std::chrono::milliseconds{10});
    EXPECT_TRUE(waiting);
    EXPECT_FALSE(opened);
    user = std::unexpected{error{"closed"}};
    cleaner.join();
    EXPECT_TRUE(opened);
}

namespace {

// Sets the modification time of `path` to `days` days ago.
void age(std::filesystem::path const& path, int days)
{
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - std::chrono::hours{24 * days});
}

void put(std::filesystem::path const& path, std::string const& content)
{
    std::ofstream{path, std::ios::binary} << content;
}

int age_in_days(std::filesystem::path const& path)
{
    auto const d = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(path);
    return static_cast<int>(std::chrono::duration_cast<std::chrono::hours>(d).count() / 24);
}

}

TEST(cache, marks_entries_used)
{
    test::temp_dir dir;
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    std::filesystem::path const entry = cache->root() / "sources" / "archive-x";
    auto const make = [](std::filesystem::path const& staging) -> result<void> {
        std::filesystem::create_directories(staging);
        return {};
    };
    ASSERT_TRUE(cache->ensure(entry, "x", make));

    // a hit renews a time older than a day; a younger one is left alone
    age(entry, 3);
    ASSERT_TRUE(cache->ensure(entry, "x", make));
    EXPECT_EQ(age_in_days(entry), 0);
    std::filesystem::last_write_time(entry, std::filesystem::file_time_type::clock::now() - std::chrono::hours{20});
    auto const before = std::filesystem::last_write_time(entry);
    cache->mark_used(entry);
    EXPECT_EQ(std::filesystem::last_write_time(entry), before);

    // a new entry is used now, whatever time unpacking gave it (an archive's, a commit's)
    std::filesystem::path const unpacked = cache->root() / "sources" / "archive-y";
    ASSERT_TRUE(cache->ensure(unpacked, "y", [](std::filesystem::path const& staging) -> result<void> {
        std::filesystem::create_directories(staging);
        age(staging, 400);
        return {};
    }));
    EXPECT_EQ(age_in_days(unpacked), 0);

    // a file in downloads/ found by fetch
    std::filesystem::path const file = cache->download_path(abc_sha256);
    std::filesystem::create_directories(file.parent_path());
    put(file, "abc");
    age(file, 3);
    cache_download const d{"file:///nowhere/abc", abc_sha256};
    auto fetched = cache->fetch({&d, 1}, {});
    ASSERT_TRUE(fetched[0]) << fetched[0].error().message;
    EXPECT_EQ(age_in_days(file), 0);
}

TEST(cache, collects_unused_entries)
{
    test::temp_dir dir;
    std::filesystem::path const root = dir.path() / "cache";
    auto const mkdir = [](std::filesystem::path const& p) {
        std::filesystem::create_directories(p);
        put(p / "file", "12345");
        return p;
    };
    std::vector<std::filesystem::path> old, fresh;
    {
        auto shared = cache_store::open(root);
        ASSERT_TRUE(shared.has_value()) << shared.error().message;
        // collecting needs the cache alone
        EXPECT_FALSE(shared->collect_garbage({}, 30, false));
        EXPECT_FALSE(cache_store::try_open_exclusive(root).value_or(std::nullopt).has_value());
    }
    old.push_back(root / "downloads" / "sha256" / abc_sha256);
    std::filesystem::create_directories(old.back().parent_path());
    put(old.back(), "abc");
    put(root / "downloads" / "sha256" / (abc_sha256 + ".lock"), "");
    old.push_back(mkdir(root / "git" / "0123"));
    old.push_back(mkdir(root / "sources" / "archive-old"));
    fresh.push_back(mkdir(root / "sources" / "archive-new"));
    old.push_back(mkdir(root / "builds" / "a" / "1.0-r1" / "slices" / "k1"));
    old.push_back(mkdir(root / "builds" / "a" / "1.0-r1" / "packages" / "p1"));
    old.push_back(mkdir(root / "builds" / "b" / "2.0-r1" / "slices" / "k2"));
    fresh.push_back(mkdir(root / "builds" / "b" / "2.0-r1" / "slices" / "k3"));
    old.push_back(mkdir(root / "failed" / "a-native-x86_64-20260101-000000"));
    for (auto const& p : old) age(p, 40);
    for (auto const& p : fresh) age(p, 20);

    auto store = cache_store::try_open_exclusive(root);
    ASSERT_TRUE(store.has_value()) << store.error().message;
    ASSERT_TRUE(store->has_value());
    cache_store& cache = **store;
    EXPECT_TRUE(cache_store::garbage_collection_due(root));

    // measured only
    auto dry = cache.collect_garbage({}, 30, true);
    ASSERT_TRUE(dry) << dry.error().message;
    EXPECT_EQ(dry->entries, old.size());
    EXPECT_EQ(dry->bytes, 3u + 5u * (old.size() - 1));
    for (auto const& p : old) EXPECT_TRUE(std::filesystem::exists(p)) << p;
    EXPECT_TRUE(cache_store::garbage_collection_due(root));

    // a selected part only
    auto sources = cache.collect_garbage({.sources = true}, 30, false);
    ASSERT_TRUE(sources) << sources.error().message;
    EXPECT_EQ(sources->entries, 2u);
    EXPECT_FALSE(std::filesystem::exists(root / "git" / "0123"));
    EXPECT_TRUE(std::filesystem::exists(root / "downloads" / "sha256" / abc_sha256));

    auto all = cache.collect_garbage({}, 30, false);
    ASSERT_TRUE(all) << all.error().message;
    EXPECT_EQ(all->entries, old.size() - 2);
    for (auto const& p : old) EXPECT_FALSE(std::filesystem::exists(p)) << p;
    for (auto const& p : fresh) EXPECT_TRUE(std::filesystem::exists(p)) << p;
    EXPECT_FALSE(std::filesystem::exists(root / "downloads" / "sha256" / (abc_sha256 + ".lock")));
    EXPECT_FALSE(std::filesystem::exists(root / "builds" / "a"));  // left empty
    EXPECT_TRUE(std::filesystem::exists(root / "builds" / "b" / "2.0-r1" / "slices"));
    EXPECT_FALSE(cache_store::garbage_collection_due(root));
    age(root / "gc-stamp", 2);
    EXPECT_TRUE(cache_store::garbage_collection_due(root));

    // 0 days: every entry
    auto everything = cache.collect_garbage({}, 0, false);
    ASSERT_TRUE(everything) << everything.error().message;
    EXPECT_EQ(everything->entries, fresh.size());
}
