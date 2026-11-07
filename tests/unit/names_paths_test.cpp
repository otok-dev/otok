// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Names (docs/formats/README.md 2) and file paths (README.md 6).

#include <gtest/gtest.h>

#include "otok/core/names.hpp"
#include "otok/core/paths.hpp"

using namespace otok;

TEST(names, library)
{
    for (auto s : {"openssl", "libssh2", "zxing-cpp", "boost_json", "7zip"}) EXPECT_TRUE(is_library_name(s)) << s;
    for (auto s : {"", "OpenSSL", "-x", "_x", "a.b", "a b", "ü"}) EXPECT_FALSE(is_library_name(s)) << s;
    EXPECT_TRUE(is_library_name(std::string(64, 'a')));
    EXPECT_FALSE(is_library_name(std::string(65, 'a')));
}

TEST(names, publisher)
{
    for (auto s : {"apotocki", "otok-dev", "a1"}) EXPECT_TRUE(is_publisher_name(s)) << s;
    for (auto s : {"", "Apotocki", "-a", "a-", "a--b", "a_b"}) EXPECT_FALSE(is_publisher_name(s)) << s;
    EXPECT_FALSE(is_publisher_name(std::string(40, 'a')));
}

TEST(names, option_product_hashes)
{
    EXPECT_TRUE(is_option_name("no-deprecated"));
    EXPECT_FALSE(is_option_name("1x"));
    EXPECT_FALSE(is_option_name("no_deprecated"));
    EXPECT_TRUE(is_product_name("OpenSSL"));
    EXPECT_TRUE(is_product_name("_C"));
    EXPECT_FALSE(is_product_name("Open-SSL"));
    EXPECT_TRUE(is_sha256_hex(std::string(64, 'a')));
    EXPECT_FALSE(is_sha256_hex(std::string(64, 'A')));
    EXPECT_TRUE(is_git_commit("0123456789abcdef0123456789abcdef01234567"));
    EXPECT_FALSE(is_git_commit("0123456"));
}

TEST(names, recipe_id)
{
    auto id = parse_recipe_id("apotocki/openssl");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->publisher, "apotocki");
    EXPECT_EQ(id->name, "openssl");
    EXPECT_EQ(id->str(), "apotocki/openssl");
    for (auto s : {"openssl", "a/b/c", "/b", "a/", "A/b"}) EXPECT_FALSE(parse_recipe_id(s).has_value()) << s;
}

TEST(paths, classifies_file_paths)
{
    for (auto s : {"Otok", "recipes/foo", "a/../b", "./a", "a//b"}) EXPECT_EQ(classify_file_path(s), file_path_kind::inside) << s;
    for (auto s : {"../x", "a/../../b", "/Users/me/x", "C:/src/x"}) EXPECT_EQ(classify_file_path(s), file_path_kind::outside) << s;
    for (auto s : {"", ".", "a/..", "a\\b"}) EXPECT_EQ(classify_file_path(s), file_path_kind::invalid) << s;
}

TEST(names, git_tags)
{
    for (char const* good : {"v1.0", "openssl-3.5.4", "release/2026.10", "1.91.0-1", "OpenSSL_1_1_1w"}) EXPECT_TRUE(is_git_tag_name(good)) << good;
    for (char const* bad : {"", "-v1", "/v1", "v1/", "v1.", "v1.lock", "a..b", "a b", "a~1", "a^", "a:b", "a?", "a*", "a[1]", "a\b", "a@{1}", "a//b", "@"}) {
        EXPECT_FALSE(is_git_tag_name(bad)) << bad;
    }
}
