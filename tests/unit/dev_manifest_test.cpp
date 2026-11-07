// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// otok.dev.toml: docs/formats/otok-dev-toml.md.

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "otok/core/fs.hpp"
#include "otok/manifest/dev_manifest.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += format(item) + "\n";
    return text;
}

std::string refusal(std::string_view content)
{
    diagnostics d;
    EXPECT_FALSE(parse_dev_manifest(content, "otok.dev.toml", d).has_value());
    return messages(d);
}

}

// The example of the format document (otok-dev-toml.md 1).
TEST(dev_manifest, reads_the_example_of_the_format_document)
{
    std::string const doc = read_file(path_from_utf8(OTOK_TEST_DATA_DIR "/../../docs/formats/otok-dev-toml.md")).value_or("");
    std::size_t const begin = doc.find("```toml\n");
    ASSERT_NE(begin, std::string::npos);
    std::size_t const end = doc.find("\n```", begin + 8);
    diagnostics d;
    auto m = parse_dev_manifest(doc.substr(begin + 8, end - begin - 8), "otok.dev.toml", d);
    ASSERT_TRUE(m.has_value()) << messages(d);
    dev_library const* lib = m->find("engine");
    ASSERT_NE(lib, nullptr);
    EXPECT_EQ(lib->source, "../../modules/engine");
    EXPECT_EQ(lib->recipe, "../../../recipes/engine");
    EXPECT_EQ(lib->resources, (std::map<std::string, std::string>{{"core", "../../modules/core"}, {"math", "../../modules/math"},
                                  {"parser", "../../modules/parser"}, {"utf", "../../modules/utf"}}));
    EXPECT_EQ(lib->where.line, 2u);
    EXPECT_EQ(m->find("boost"), nullptr);
    EXPECT_FALSE(lib->quiet);
}

TEST(dev_manifest, takes_format_1_and_metadata)
{
    diagnostics d;
    auto m = parse_dev_manifest("format = 1\n[deps.a]\nsource = \"../a\"\nquiet = true\n[metadata]\nmine = 1\n", "otok.dev.toml", d);
    ASSERT_TRUE(m.has_value()) << messages(d);
    EXPECT_EQ(m->deps.size(), 1u);
    EXPECT_TRUE(m->find("a")->quiet);
    diagnostics none;
    auto empty = parse_dev_manifest("", "otok.dev.toml", none);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->deps.empty());
}

TEST(dev_manifest, refuses_another_format)
{
    EXPECT_EQ(refusal("format = 2\n"), "otok.dev.toml:1:10: error: 'format' at the top level is 2; this otok reads format 1\n");
}

TEST(dev_manifest, refuses_unknown_keys)
{
    EXPECT_EQ(refusal("[deps.a]\nsource = \"../a\"\nsorce = \"../b\"\n"), "otok.dev.toml:3:1: error: unknown key 'sorce' in deps.a\n");
    EXPECT_EQ(refusal("[dep.a]\n"), "otok.dev.toml:1:2: error: unknown key 'dep' at the top level\n");
}

TEST(dev_manifest, refuses_values_of_the_wrong_type)
{
    EXPECT_NE(refusal("[deps.a]\nsource = 1\n").find("otok.dev.toml:2:10: error: 'source' in deps.a must be a string"), std::string::npos);
    EXPECT_NE(refusal("[deps.a]\nresources = { r = true }\n").find("error: 'r' in deps.a.resources must be a string"), std::string::npos);
}

TEST(dev_manifest, refuses_an_invalid_library_name)
{
    EXPECT_EQ(refusal("[deps.Big]\nsource = \"../a\"\n"), "otok.dev.toml:1:1: error: 'Big' is not a valid library name\n");
}

TEST(dev_manifest, refuses_an_invalid_path)
{
    EXPECT_EQ(refusal("[deps.a]\nsource = \"..\\\\a\"\n"), "otok.dev.toml:2:10: error: 'source' in deps.a is not a valid directory path: '..\\a'\n");
    EXPECT_EQ(refusal("[deps.a]\nresources = { r = \"\" }\n"),
        "otok.dev.toml:2:19: error: 'r' in deps.a.resources is not a valid directory path: ''\n");
}

TEST(dev_manifest, refuses_a_library_without_working_trees)
{
    EXPECT_EQ(refusal("[deps.a]\n"), "otok.dev.toml:1:1: error: 'deps.a' names no working tree: give 'source', 'resources' or 'recipe'\n");
}

TEST(dev_manifest, reads_a_missing_file_as_empty)
{
    test::temp_dir dir;
    diagnostics d;
    auto m = load_dev_manifest(dir.path() / "otok.dev.toml", d);
    ASSERT_TRUE(m.has_value());
    EXPECT_TRUE(m->deps.empty());
}
