// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Downloads (file:// URLs; an https one with OTOK_TEST_NETWORK=1) and archive extraction. Expected
// hashes were computed independently (sha256sum).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include "otok/core/fs.hpp"
#include "otok/fetch/archive.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/fetch/locations.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

std::string const abc_sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

struct entry
{
    std::string path;
    std::string content{};
    unsigned type = AE_IFREG;
    std::string link{};       // symlink target
    int mode = 0644;
    std::int64_t mtime = 1600000000;  // 2020-09-13
};

using add_filter = int (*)(archive*);

// Writes an archive with libarchive: a pax tar with `filter`, or a zip when `filter` is null.
void write_archive(std::filesystem::path const& file, add_filter filter, std::vector<entry> const& entries)
{
    archive* w = archive_write_new();
    if (filter) {
        archive_write_set_format_pax_restricted(w);
#ifndef _WIN32
        archive_write_set_options(w, "hdrcharset=BINARY");  // names as the bytes given: the C locale has no UTF-8
#endif
        filter(w);
    } else {
        archive_write_set_format_zip(w);
    }
    ASSERT_EQ(archive_write_open_filename(w, path_to_utf8(file).c_str()), ARCHIVE_OK) << archive_error_string(w);
    for (entry const& e : entries) {
        archive_entry* ae = archive_entry_new();
#ifdef _WIN32
        archive_entry_set_pathname_utf8(ae, e.path.c_str());
#else
        archive_entry_set_pathname(ae, e.path.c_str());
#endif
        archive_entry_set_filetype(ae, e.type);
        archive_entry_set_perm(ae, static_cast<__LA_MODE_T>(e.mode));
        archive_entry_set_mtime(ae, static_cast<time_t>(e.mtime), 0);
        if (e.type == AE_IFLNK) archive_entry_set_symlink(ae, e.link.c_str());
        if (e.type == AE_IFREG) archive_entry_set_size(ae, static_cast<la_int64_t>(e.content.size()));
        ASSERT_EQ(archive_write_header(w, ae), ARCHIVE_OK) << archive_error_string(w);
        if (!e.content.empty()) archive_write_data(w, e.content.data(), e.content.size());
        archive_entry_free(ae);
    }
    archive_write_close(w);
    archive_write_free(w);
}

std::string read(std::filesystem::path const& file)
{
    return read_file(file).value_or("<missing>");
}

}

TEST(http, downloads_file_urls_and_checks_hashes)
{
    test::temp_dir dir;
    dir.write("src dir/abc.txt", "abc");
    std::string const url = file_url(dir.path() / "src dir" / "abc.txt");
    EXPECT_NE(url.find("src%20dir/abc.txt"), std::string::npos) << url;

    auto d = download_file({url, dir.path() / "out", abc_sha256}, {});
    ASSERT_TRUE(d.has_value()) << d.error().message;
    EXPECT_EQ(d->sha256, abc_sha256);
    EXPECT_EQ(d->size, 3u);
    EXPECT_EQ(read(dir.path() / "out"), "abc");

    // a mismatch is an error and leaves nothing behind
    auto bad = download_file({url, dir.path() / "bad", std::string(64, '0')}, {});
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().message, "the download of " + url + " does not match its sha256:\n  expected " + std::string(64, '0')
        + "\n  received " + abc_sha256);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "bad"));

    auto missing = download_file({file_url(dir.path() / "missing"), dir.path() / "m", ""}, {});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message.find("cannot download file://"), 0u) << missing.error().message;
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "m"));
}

TEST(http, downloads_several_at_once)
{
    test::temp_dir dir;
    std::vector<download_request> requests;
    for (int i = 0; i < 5; ++i) {
        std::string const name = "f" + std::to_string(i);
        dir.write(name, std::string(static_cast<std::size_t>(1000 * (i + 1)), static_cast<char>('a' + i)));
        requests.push_back({file_url(dir.path() / name), dir.path() / (name + ".out"), ""});
    }
    requests[2].url = file_url(dir.path() / "missing");
    http_settings settings;
    settings.max_parallel = 2;
    auto results = download_all(requests, settings);
    ASSERT_EQ(results.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        if (i == 2) {
            EXPECT_FALSE(results[2].has_value());
            continue;
        }
        ASSERT_TRUE(results[static_cast<std::size_t>(i)].has_value()) << results[static_cast<std::size_t>(i)].error().message;
        EXPECT_EQ(results[static_cast<std::size_t>(i)]->size, 1000u * static_cast<unsigned>(i + 1));
    }
}

TEST(http, downloads_over_https)
{
    // TLS and the system's certificates; needs the network, so it runs only when asked.
    char const* enabled = std::getenv("OTOK_TEST_NETWORK");
    if (!enabled || std::string{enabled} != "1") GTEST_SKIP() << "set OTOK_TEST_NETWORK=1 to run";
    test::temp_dir dir;
    auto d = download_file({"https://raw.githubusercontent.com/madler/zlib/v1.3.1/README", dir.path() / "README",
                               "d106dcdb2a0b3087cb3a2380b9f00a0bbadc7eb835cd80408f94a2d78d69727d"}, {});
    ASSERT_TRUE(d.has_value()) << d.error().message;
    EXPECT_EQ(d->size, 5317u);

    // a request with its reply's headers, compressed or not, revalidated with its ETag (index.md 8)
    http_exchange request;
    request.url = "https://raw.githubusercontent.com/madler/zlib/v1.3.1/README";
    auto reply = http_call(request, {});
    ASSERT_TRUE(reply.has_value()) << reply.error().message;
    EXPECT_EQ(reply->status, 200);
    EXPECT_EQ(reply->body.size(), 5317u);
    std::string const* etag = reply->header("ETag");
    ASSERT_NE(etag, nullptr);
    request.headers.push_back("If-None-Match: " + *etag);
    auto again = http_call(request, {});
    ASSERT_TRUE(again.has_value()) << again.error().message;
    EXPECT_EQ(again->status, 304);
}

TEST(http, finds_the_ca_bundle_of_the_system)
{
    test::temp_dir dir;
    std::filesystem::create_directories(dir.path() / "certs");  // a directory is no bundle
    ASSERT_TRUE(write_file_atomic(dir.path() / "b.crt", "b"));
    ASSERT_TRUE(write_file_atomic(dir.path() / "c.crt", "c"));
    std::vector<std::filesystem::path> const candidates{dir.path() / "missing.crt", dir.path() / "certs", dir.path() / "b.crt",
                                                        dir.path() / "c.crt"};
    EXPECT_EQ(find_ca_bundle(nullptr, candidates), dir.path() / "b.crt");
    EXPECT_EQ(find_ca_bundle("", candidates), dir.path() / "b.crt");
    // SSL_CERT_FILE wins, whether it exists or not: what the user asked for
    EXPECT_EQ(find_ca_bundle("/elsewhere/ca.pem", candidates), std::filesystem::path{"/elsewhere/ca.pem"});
    EXPECT_EQ(find_ca_bundle(nullptr, std::span{candidates}.first(2)), std::filesystem::path{});
    ASSERT_FALSE(known_ca_bundles().empty());
    EXPECT_EQ(known_ca_bundles().front(), std::filesystem::path{"/etc/ssl/certs/ca-certificates.crt"});
}

TEST(archive, strips_a_single_top_level_directory)
{
    for (add_filter const filter : {&archive_write_add_filter_gzip, &archive_write_add_filter_bzip2, &archive_write_add_filter_xz,
             &archive_write_add_filter_zstd, add_filter{nullptr}}) {
        test::temp_dir dir;
        write_archive(dir.path() / "src.archive", filter, {
            {.path = "pkg-1.0/", .type = AE_IFDIR, .mode = 0755},
            {.path = "pkg-1.0/README", .content = "hello, archive\n"},
            {.path = "pkg-1.0/src/main.c", .content = "int main() { return 0; }\n"},
        });
        auto ok = extract_archive(dir.path() / "src.archive", dir.path() / "out");
        ASSERT_TRUE(ok.has_value()) << ok.error().message;
        EXPECT_EQ(read(dir.path() / "out" / "README"), "hello, archive\n");
        EXPECT_EQ(read(dir.path() / "out" / "src" / "main.c"), "int main() { return 0; }\n");
        EXPECT_FALSE(std::filesystem::exists(dir.path() / "out.partial"));
    }
}

TEST(archive, keeps_non_ascii_names)
{
    // Boost's sources have libs/wave/test/testwave/testfiles/utf8-test-ßµ™∃; otok runs in the C locale
    test::temp_dir dir;
    std::string const name = "utf8-test-\xc3\x9f\xc2\xb5\xe2\x84\xa2\xe2\x88\x83";
    write_archive(dir.path() / "u.tar.bz2", &archive_write_add_filter_bzip2, {{.path = "pkg/" + name + "/a.txt", .content = "a"}, {.path = "pkg/b.txt", .content = "b"}});
    auto ok = extract_archive(dir.path() / "u.tar.bz2", dir.path() / "out");
    ASSERT_TRUE(ok.has_value()) << ok.error().message;
    EXPECT_EQ(read(dir.path() / "out" / path_from_utf8(name) / "a.txt"), "a");
}

TEST(archive, keeps_several_top_level_entries)
{
    test::temp_dir dir;
    write_archive(dir.path() / "src.tgz", &archive_write_add_filter_gzip, {
        {.path = "./configure", .content = "#!/bin/sh\n", .mode = 0755, .mtime = 1600000100},
        {.path = "./lib/a.c", .content = "a"},
    });
    auto ok = extract_archive(dir.path() / "src.tgz", dir.path() / "out");
    ASSERT_TRUE(ok.has_value()) << ok.error().message;
    EXPECT_EQ(read(dir.path() / "out" / "configure"), "#!/bin/sh\n");
    EXPECT_EQ(read(dir.path() / "out" / "lib" / "a.c"), "a");

    // times are kept (make compares them; autotools tarballs rely on it)
    auto const difference = std::filesystem::last_write_time(dir.path() / "out" / "configure")
        - std::filesystem::last_write_time(dir.path() / "out" / "lib" / "a.c");
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(difference).count(), 100);
#ifndef _WIN32
    auto const perms = std::filesystem::status(dir.path() / "out" / "configure").permissions();
    EXPECT_NE(perms & std::filesystem::perms::owner_exec, std::filesystem::perms::none);
#endif
}

TEST(archive, rejects_dangerous_entries)
{
    struct bad_case
    {
        entry e;
        std::string message;
    };
    std::vector<bad_case> cases{
        {{.path = "../evil", .content = "x"}, "'../evil' leaves the archive root"},
        {{.path = "a/../../evil", .content = "x"}, "'a/../../evil' leaves the archive root"},
        {{.path = "/etc/evil", .content = "x"}, "'/etc/evil' is an absolute path"},
        {{.path = "dir/link", .type = AE_IFLNK, .link = "../../outside"}, "the link 'dir/link' points to '../../outside', outside the archive"},
        {{.path = "link", .type = AE_IFLNK, .link = "/etc/passwd"}, "the link 'link' points to '/etc/passwd'; links must stay inside the archive"},
        {{.path = "fifo", .type = AE_IFIFO}, "'fifo' is a special file (device, pipe or socket)"},
    };
#ifndef _WIN32
    // libarchive on Windows reads a backslash as a separator, which keeps the entry inside the tree
    cases.push_back({{.path = "a\\b", .content = "x"}, "'a\\b' contains a backslash"});
#endif
    for (bad_case const& c : cases) {
        test::temp_dir dir;
        write_archive(dir.path() / "bad.tar.gz", &archive_write_add_filter_gzip, {{.path = "ok.txt", .content = "fine"}, c.e});
        auto ok = extract_archive(dir.path() / "bad.tar.gz", dir.path() / "out");
        ASSERT_FALSE(ok.has_value()) << c.e.path;
        EXPECT_EQ(ok.error().message, "the archive '" + path_to_utf8(dir.path() / "bad.tar.gz") + "': " + c.message);
        EXPECT_FALSE(std::filesystem::exists(dir.path() / "out"));
        EXPECT_FALSE(std::filesystem::exists(dir.path() / "out.partial"));
    }
}

TEST(archive, other_failures)
{
    test::temp_dir dir;
    dir.write("not-an-archive", "plain text, not an archive\n");
    auto ok = extract_archive(dir.path() / "not-an-archive", dir.path() / "out");
    ASSERT_FALSE(ok.has_value());
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "out"));

    write_archive(dir.path() / "a.tgz", &archive_write_add_filter_gzip, {{.path = "a", .content = "a"}});
    std::filesystem::create_directories(dir.path() / "exists");
    ok = extract_archive(dir.path() / "a.tgz", dir.path() / "exists");
    ASSERT_FALSE(ok.has_value());
    EXPECT_EQ(ok.error().message, "'" + path_to_utf8(dir.path() / "exists") + "' already exists");
}

#ifndef _WIN32
TEST(archive, keeps_links_inside_the_tree)
{
    test::temp_dir dir;
    write_archive(dir.path() / "l.tar.xz", &archive_write_add_filter_xz, {
        {.path = "pkg/include/a.h", .content = "int a;\n"},
        {.path = "pkg/include/b.h", .type = AE_IFLNK, .link = "a.h"},
        {.path = "pkg/lib/up", .type = AE_IFLNK, .link = "../include"},
    });
    auto ok = extract_archive(dir.path() / "l.tar.xz", dir.path() / "out");
    ASSERT_TRUE(ok.has_value()) << ok.error().message;
    EXPECT_EQ(std::filesystem::read_symlink(dir.path() / "out" / "include" / "b.h"), "a.h");
    EXPECT_EQ(read(dir.path() / "out" / "lib" / "up" / "a.h"), "int a;\n");
}
#endif

// a private index on GitHub read with the token of the publication (base64 of perl's MIME::Base64)
TEST(git, gives_the_token_for_one_repository)
{
    EXPECT_TRUE(git_token_env("https://github.com/o/index", "", {}).empty());  // a public index needs none
    env_vars const first = git_token_env("https://github.com/o/index", "ghp_abc", {{"PATH", "/bin"}});
    EXPECT_EQ(first, (env_vars{{"GIT_CONFIG_COUNT", "1"}, {"GIT_CONFIG_KEY_0", "http.https://github.com/o/index.extraHeader"},
        {"GIT_CONFIG_VALUE_0", "Authorization: Basic eC1hY2Nlc3MtdG9rZW46Z2hwX2FiYw=="}}));
    // after the user's own
    env_vars const after = git_token_env("https://github.com/o/index", "t1", {{"GIT_CONFIG_COUNT", "2"}});
    EXPECT_EQ(after, (env_vars{{"GIT_CONFIG_COUNT", "3"}, {"GIT_CONFIG_KEY_2", "http.https://github.com/o/index.extraHeader"},
        {"GIT_CONFIG_VALUE_2", "Authorization: Basic eC1hY2Nlc3MtdG9rZW46dDE="}}));
}

TEST(git, url_keys_and_submodule_urls)
{
    std::string const key = git_url_key("https://GitHub.com/OpenSSL/openssl.git/");
    EXPECT_EQ(key.size(), 32u);
    EXPECT_EQ(key, git_url_key("https://github.com/OpenSSL/openssl"));      // scheme, host, suffixes
    EXPECT_NE(key, git_url_key("https://github.com/openssl/openssl"));      // the path keeps its case
    // ssh's short form has no scheme: only the suffixes go
    EXPECT_EQ(git_url_key("user@h.org:/git/a.git/"), git_url_key("user@h.org:/git/a"));
    EXPECT_NE(git_url_key("user@H.org:/git/a"), git_url_key("user@h.org:/git/a"));
    EXPECT_EQ(resolve_submodule_url("user@h.org:/git/a.git", "../b.git"), "user@h.org:/git/b.git");
    EXPECT_EQ(resolve_submodule_url("git@h.org:team/a.git", "../b.git"), "git@h.org:team/b.git");

    EXPECT_EQ(resolve_submodule_url("https://h.org/o/a.git", "../b.git"), "https://h.org/o/b.git");
    EXPECT_EQ(resolve_submodule_url("https://h.org/o/a/", "../../x/b"), "https://h.org/x/b");
    EXPECT_EQ(resolve_submodule_url("https://h.org/o/a", "./b"), "https://h.org/o/a/b");
    EXPECT_EQ(resolve_submodule_url("https://h.org/o/a", "https://other.org/c"), "https://other.org/c");
}

TEST(locations, rewrite_prefixes_in_order)
{
    // the first rule that matches: the specific one is written first
    std::vector<url_rewrite> const rules{{"https://github.com/boostorg/", "https://boost-mirror/"}, {"https://github.com/", "https://mirror/gh/"}};
    EXPECT_EQ(rewrite_url("https://github.com/boostorg/boost.git", rules), "https://boost-mirror/boost.git");
    EXPECT_EQ(rewrite_url("https://github.com/madler/zlib", rules), "https://mirror/gh/madler/zlib");
    EXPECT_FALSE(rewrite_url("https://gitlab.com/x", rules).has_value());
}

TEST(locations, rewrite_patterns)
{
    std::string const boost = "https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2";
    std::vector<url_rewrite> const by_version{{"https://archives.boost.io/release/{version}/source/{file}", "https://m/boost/{version}/{file}", true}};
    EXPECT_EQ(rewrite_url(boost, by_version), "https://m/boost/1.92.0/boost_1_92_0.tar.bz2");
    // {name} stops at /, so with exact the deeper URL does not match
    EXPECT_FALSE(rewrite_url("https://archives.boost.io/release/1.92.0/source/sub/x.tgz", by_version).has_value());
    // {name*} takes / too
    std::vector<url_rewrite> const deep{{"https://archives.boost.io/release/{path*}", "https://m/{path}", true}};
    EXPECT_EQ(rewrite_url("https://archives.boost.io/release/1.92.0/source/sub/x.tgz", deep), "https://m/1.92.0/source/sub/x.tgz");

    // a prefix: the rest of the URL is appended; text after a placeholder is found as far right as possible
    std::vector<url_rewrite> const repos{{"https://github.com/{owner}/{repo}", "https://git/{owner}-{repo}"}};
    EXPECT_EQ(rewrite_url("https://github.com/lua/lua", repos), "https://git/lua-lua");
    EXPECT_EQ(rewrite_url("https://github.com/lua/lua/archive/v5.4.tar.gz", repos), "https://git/lua-lua/archive/v5.4.tar.gz");
    std::vector<url_rewrite> const dashes{{"https://x/{name}-{version}.tgz", "https://y/{version}/{name}.tgz", true}};
    EXPECT_EQ(rewrite_url("https://x/lib-a-1.0.tgz", dashes), "https://y/1.0/lib-a.tgz");

    // exact: one URL
    std::vector<url_rewrite> const one{{boost, "https://m/b.tar.bz2", true}};
    EXPECT_EQ(rewrite_url(boost, one), "https://m/b.tar.bz2");
    EXPECT_FALSE(rewrite_url(boost + ".sig", one).has_value());

    // {sha256}: archives only; for git the next rule applies
    std::vector<url_rewrite> const by_hash{{"https://archives.boost.io/", "https://m/{sha256}"}, {"https://archives.boost.io/", "https://n/"}};
    EXPECT_EQ(rewrite_url(boost, by_hash, "abc"), "https://m/abcrelease/1.92.0/source/boost_1_92_0.tar.bz2");
    EXPECT_EQ(rewrite_url(boost, by_hash), "https://n/release/1.92.0/source/boost_1_92_0.tar.bz2");
}

TEST(locations, rewrite_rule_problems)
{
    EXPECT_FALSE(url_rewrite_problem({"https://x/{a}/{b*}", "https://y/{b}/{a}/{sha256}"}).has_value());
    EXPECT_EQ(url_rewrite_problem({"https://x/{a", "https://y/"}), "'from' has '{' without '}'");
    EXPECT_EQ(url_rewrite_problem({"https://x/a}", "https://y/"}), "'from' has '}' without '{'");
    EXPECT_EQ(url_rewrite_problem({"https://x/{A}", "https://y/"}), "'from' has the placeholder '{A}'; names are [a-z][a-z0-9_]*");
    EXPECT_EQ(url_rewrite_problem({"https://x/{a}{b}", "https://y/"}), "'from' has two placeholders without text between them");
    EXPECT_EQ(url_rewrite_problem({"https://x/{a}/{a}", "https://y/"}), "'from' has the placeholder {a} twice");
    EXPECT_EQ(url_rewrite_problem({"https://x/{sha256}", "https://y/"}), "'from' may not use {sha256}, which only 'to' can use");
    EXPECT_EQ(url_rewrite_problem({"https://x/{a}", "https://y/{b}"}), "'to' uses {b}, which 'from' does not have");
    EXPECT_EQ(url_rewrite_problem({"https://x/{a*}", "https://y/{a*}"}), "'to' has the placeholder '{a*}'; names are [a-z][a-z0-9_]*");
}

TEST(locations, archive_order)
{
    input_mirrors const machine{{"https://assets/{sha256}", "file:///m/{sha256}.bin"}, {{"https://archives.boost.io/", "https://proxy/boost/"}}};
    std::vector<std::string> const mirrors{"https://sourceforge.net/boost.tar.bz2", "https://archives.boost.io/b.tar.bz2"};
    EXPECT_EQ(archive_locations("abc", "https://archives.boost.io/a.tar.bz2", mirrors, {.machine = machine}, "boost"),
        (std::vector<std::string>{"https://assets/abc", "file:///m/abc.bin", "https://proxy/boost/a.tar.bz2", "https://proxy/boost/b.tar.bz2",
            "https://archives.boost.io/a.tar.bz2", "https://sourceforge.net/boost.tar.bz2", "https://archives.boost.io/b.tar.bz2"}));

    input_mirrors only = machine;
    only.mirrors_only = true;
    EXPECT_EQ(archive_locations("abc", "https://archives.boost.io/a.tar.bz2", {}, {.machine = only}, "boost"),
        (std::vector<std::string>{"https://assets/abc", "file:///m/abc.bin", "https://proxy/boost/a.tar.bz2"}));
    EXPECT_EQ(archive_locations("abc", "https://x/a", {}, {}, ""), std::vector<std::string>{"https://x/a"});
}

TEST(locations, the_project_comes_first)
{
    input_mirrors const project{{}, {{"https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2", "https://mine/boost.tar.bz2"}}};
    input_mirrors const machine{{"https://corp/{sha256}"}};
    std::string const url = "https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2";
    EXPECT_EQ(archive_locations("abc", url, {}, {project, machine}, "boost"),
        (std::vector<std::string>{"https://mine/boost.tar.bz2", "https://corp/abc", url}));

    // the project's mirrors-only drops the recipe's locations, for all libraries or for its own
    input_mirrors for_boost = project;
    for_boost.mirrors_only_libraries = {"boost"};
    EXPECT_EQ(archive_locations("abc", url, {}, {for_boost, machine}, "boost"),
        (std::vector<std::string>{"https://mine/boost.tar.bz2", "https://corp/abc"}));
    EXPECT_EQ(archive_locations("abc", url, {}, {for_boost, machine}, "icu4c"),
        (std::vector<std::string>{"https://mine/boost.tar.bz2", "https://corp/abc", url}));

    // the machine's mirrors-only drops the project's locations too
    input_mirrors corp = machine;
    corp.mirrors_only_libraries = {"boost"};
    EXPECT_EQ(archive_locations("abc", url, {}, {project, corp}, "boost"), std::vector<std::string>{"https://corp/abc"});
    EXPECT_EQ(archive_locations("abc", url, {}, {project, corp}, ""), (std::vector<std::string>{"https://mine/boost.tar.bz2", "https://corp/abc", url}));
}

TEST(locations, git_order)
{
    input_mirrors machine{{"https://assets/{sha256}"}, {{"https://github.com/", "https://mirror/gh/"}}};
    input_mirrors const project{{}, {{"https://github.com/lua/", "https://fork/lua/"}}};
    EXPECT_EQ(git_locations("https://github.com/lua/lua", {.machine = machine}, "lua"),
        (std::vector<std::string>{"https://mirror/gh/lua/lua", "https://github.com/lua/lua"}));
    EXPECT_EQ(git_locations("https://github.com/lua/lua", {project, machine}, "lua"),
        (std::vector<std::string>{"https://fork/lua/lua", "https://mirror/gh/lua/lua", "https://github.com/lua/lua"}));
    EXPECT_EQ(git_locations("https://gitlab.com/x", {project, machine}, "x"), std::vector<std::string>{"https://gitlab.com/x"});
    machine.mirrors_only = true;
    EXPECT_EQ(git_locations("https://github.com/lua/lua", {project, machine}, "lua"), std::vector<std::string>{"https://mirror/gh/lua/lua"});
    EXPECT_TRUE(git_locations("https://gitlab.com/x", {project, machine}, "x").empty());
}

// config.md 2: `git` names the program; it is not looked for in PATH then
TEST(git, takes_the_program_of_the_configuration)
{
    test::temp_dir dir;
    std::string const missing = path_to_utf8(dir.path() / "no-git");
    auto none = find_git({}, missing);
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().message, "'git' of config.toml is '" + missing + "', which is not a file");
    dir.write("bin/git.exe", "");
    auto found = find_git({{"PATH", ""}}, path_to_utf8(dir.path() / "bin" / "git.exe"));
    ASSERT_TRUE(found.has_value()) << found.error().message;
    EXPECT_EQ(found->program, dir.path() / "bin" / "git.exe");
    EXPECT_EQ(found->env.at("PATH"), "");
}
