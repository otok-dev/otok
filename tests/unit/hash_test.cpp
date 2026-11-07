// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// SHA-256 and the tree hash of docs/formats/recipe-hash.md. Expected values were computed
// independently (Python hashlib) from the same inputs.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "otok/core/sha256.hpp"
#include "otok/core/tree_hash.hpp"
#include "otok/recipe/recipe_tree.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += format(item) + "\n";
    return text;
}

std::string binary_block()
{
    std::string block;
    for (int i = 0; i < 256; ++i) block += static_cast<char>(i);
    std::string data;
    for (int i = 0; i < 1000; ++i) data += block;
    return data;
}

}

TEST(sha256, known_vectors)
{
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    EXPECT_EQ(sha256_hex(binary_block()), "b57b64b198d5d59ce5a22a9b9f25e72a7d081476d432051aa923f3dbebb90934");
}

TEST(sha256, incremental_equals_whole)
{
    std::string const data = binary_block();
    sha256_hasher hasher;
    for (std::size_t i = 0; i < data.size(); i += 777) {
        hasher.update(std::string_view{data}.substr(i, 777));
    }
    EXPECT_EQ(hasher.hex_digest(), sha256_hex(data));
    // the hasher starts over after a digest
    hasher.update("abc");
    EXPECT_EQ(hasher.hex_digest(), sha256_hex("abc"));
}

TEST(sha256, files)
{
    test::temp_dir dir;
    dir.write("data.bin", binary_block());
    auto sha = sha256_file(dir.path() / "data.bin");
    ASSERT_TRUE(sha.has_value());
    EXPECT_EQ(*sha, "b57b64b198d5d59ce5a22a9b9f25e72a7d081476d432051aa923f3dbebb90934");
    EXPECT_FALSE(sha256_file(dir.path() / "missing").has_value());
}

TEST(tree_hash, matches_the_specified_algorithm)
{
    test::temp_dir dir;
    dir.write("b.txt", "hello\n");
    dir.write("a/x.lua", "print(1)\n");
    dir.write("a/b/c", "");
    dir.write("Z.md", "z");
    dir.write("\xC3\xA9.txt", "e");
    dir.write(".git/config", "ignored");
    std::filesystem::create_directories(dir.path() / "empty");  // directories contribute nothing

    auto files = list_tree(dir.path());
    ASSERT_TRUE(files.has_value()) << files.error().message;
    ASSERT_EQ(files->size(), 5u);
    // sorted as bytes: uppercase before lowercase, UTF-8 after ASCII
    EXPECT_EQ((*files)[0].path, "Z.md");
    EXPECT_EQ((*files)[1].path, "a/b/c");
    EXPECT_EQ((*files)[4].path, "\xC3\xA9.txt");
    EXPECT_EQ((*files)[3].size, 6u);
    EXPECT_EQ(tree_hash(*files), "78e982d79b6f89f49ed049feb9aacae8e5506341862ffe0e573ef0e28c40c68e");
    EXPECT_NE(tree_listing(*files).find("  a/x.lua\n"), std::string::npos);
}

TEST(tree_hash, content_changes_change_the_hash)
{
    test::temp_dir dir;
    dir.write("build.lua", "print(1)\n");
    auto before = tree_hash(dir.path());
    dir.write("build.lua", "print(1)\r\n");  // no line-ending normalization
    auto after = tree_hash(dir.path());
    ASSERT_TRUE(before && after);
    EXPECT_NE(*before, *after);
}

TEST(tree_hash, rejects_symbolic_links)
{
    test::temp_dir dir;
    dir.write("target.txt", "x");
    std::error_code ec;
    std::filesystem::create_symlink(dir.path() / "target.txt", dir.path() / "link.txt", ec);
    if (ec) GTEST_SKIP() << "cannot create symbolic links here: " << ec.message();
    auto files = list_tree(dir.path());
    ASSERT_FALSE(files.has_value());
    EXPECT_NE(files.error().message.find("symbolic link"), std::string::npos);
}

TEST(tree_hash, records_links_in_source_trees)
{
    test::temp_dir dir;
    dir.write("a.txt", "x\n");
    std::filesystem::create_directories(dir.path() / "sub");
    std::error_code ec;
    std::filesystem::create_symlink("a.txt", dir.path() / "l", ec);
    if (!ec) std::filesystem::create_symlink("../a.txt", dir.path() / "sub" / "up", ec);
    if (ec) GTEST_SKIP() << "cannot create symbolic links here: " << ec.message();

    auto files = list_tree(dir.path(), tree_links::record);
    ASSERT_TRUE(files.has_value()) << files.error().message;
    ASSERT_EQ(files->size(), 3u);
    EXPECT_EQ((*files)[1].link_target, "a.txt");
    EXPECT_EQ((*files)[2].link_target, "../a.txt");
    // also what the shell script of recipe-hash.md 1 prints
    EXPECT_EQ(tree_hash(*files), "40afeced44d8e589d21d61495df57da4556491b8152ae3016e20002b87678a72");
    EXPECT_FALSE(list_tree(dir.path()).has_value());  // a recipe tree rejects them
}

TEST(tree_hash, links_must_stay_inside_the_tree)
{
    test::temp_dir dir;
    std::filesystem::create_directories(dir.path() / "sub");
    std::error_code ec;
    std::filesystem::create_symlink("../../outside", dir.path() / "sub" / "escape", ec);
    if (ec) GTEST_SKIP() << "cannot create symbolic links here: " << ec.message();
    auto files = list_tree(dir.path(), tree_links::record);
    ASSERT_FALSE(files.has_value());
    EXPECT_EQ(files.error().message, "the link 'sub/escape' points to '../../outside', outside the tree");

    test::temp_dir absolute;
    std::filesystem::create_symlink(absolute.path(), absolute.path() / "abs", ec);
    ASSERT_FALSE(ec) << ec.message();
    files = list_tree(absolute.path(), tree_links::record);
    ASSERT_FALSE(files.has_value());
    EXPECT_NE(files.error().message.find("points to the absolute path"), std::string::npos) << files.error().message;
}

TEST(recipe_tree, accepts_a_text_recipe)
{
    test::temp_dir dir;
    dir.write("recipe.toml", "[recipe]\n");
    dir.write("build.lua", "-- \xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\n");  // UTF-8 text
    dir.write("patches/0001-fix.patch", "diff\n");
    auto files = list_tree(dir.path());
    ASSERT_TRUE(files.has_value());
    diagnostics d;
    check_recipe_tree(dir.path(), *files, severity::error, d);
    EXPECT_TRUE(d.items().empty()) << messages(d);
}

TEST(recipe_tree, reports_limits)
{
    test::temp_dir dir;
    dir.write("recipe.toml", "[recipe]\n");
    dir.write("libssl.a", std::string("!<arch>\n\0\0", 10));
    dir.write("-flag", "x");
    dir.write("a b.txt", "x");
    dir.write("README.md", "b");
    auto files = list_tree(dir.path());
    ASSERT_TRUE(files.has_value());
    // Two paths that differ only in case cannot both exist on a case-insensitive file system
    // (the very reason for the rule), so the second one is added to the listing directly.
    auto readme = std::ranges::find(*files, std::string{"README.md"}, &tree_file::path);
    ASSERT_NE(readme, files->end());
    tree_file other = *readme;
    other.path = "Readme.md";
    files->push_back(other);

    diagnostics errors;
    check_recipe_tree(dir.path(), *files, severity::error, errors);
    std::string const all = messages(errors);
    EXPECT_NE(all.find("libssl.a: error: a recipe holds only UTF-8 text"), std::string::npos) << all;
    EXPECT_NE(all.find("-flag: error: the path is not portable"), std::string::npos) << all;
    EXPECT_NE(all.find("a b.txt: error: the path is not portable"), std::string::npos) << all;
    EXPECT_NE(all.find("only in case"), std::string::npos) << all;

    diagnostics warnings;
    check_recipe_tree(dir.path(), *files, severity::warning, warnings);
    EXPECT_FALSE(warnings.has_errors());
    EXPECT_FALSE(warnings.items().empty());
}

TEST(recipe_tree, reports_size_and_count)
{
    test::temp_dir dir;
    dir.write("big.txt", std::string(recipe_tree_max_bytes + 1, 'x'));
    for (std::size_t i = 0; i < recipe_tree_max_files; ++i) dir.write("f/" + std::to_string(i), "");
    auto files = list_tree(dir.path());
    ASSERT_TRUE(files.has_value());
    diagnostics d;
    check_recipe_tree(dir.path(), *files, severity::error, d);
    std::string const all = messages(d);
    EXPECT_NE(all.find("at most 256 are allowed"), std::string::npos) << all;
    EXPECT_NE(all.find("at most 1 MiB is allowed"), std::string::npos) << all;
}

TEST(recipe_tree, utf8_validation)
{
    EXPECT_TRUE(is_text("plain"));
    EXPECT_TRUE(is_text("\xE2\x82\xAC"));            // euro sign
    EXPECT_TRUE(is_text("\xF0\x9F\x98\x80"));        // emoji
    EXPECT_FALSE(is_text(std::string_view{"a\0b", 3}));
    EXPECT_FALSE(is_text("\xC3"));                   // truncated
    EXPECT_FALSE(is_text("\xC0\xAF"));               // overlong
    EXPECT_FALSE(is_text("\xED\xA0\x80"));           // surrogate
    EXPECT_FALSE(is_text("\xFF"));
}
