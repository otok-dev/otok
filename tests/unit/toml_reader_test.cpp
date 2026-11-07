// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Strict TOML reading: docs/formats/README.md 1.2, 1.3.

#include <gtest/gtest.h>

#include <string>

#include "otok/core/toml_reader.hpp"

using otok::diagnostics;
using otok::need;
using otok::toml_document;

namespace {

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += otok::format(item) + "\n";
    return text;
}

}

TEST(toml_reader, reads_typed_values)
{
    diagnostics d;
    auto doc = toml_document::parse("[otok]\nformat = 1\nname = \"x\"\nstrict = true\nlist = [\"a\", \"b\"]\n", "otok.toml", d);
    ASSERT_TRUE(doc.has_value()) << messages(d);
    auto root = doc->root();
    auto otok = root.table("otok", need::required);
    ASSERT_TRUE(otok.has_value());
    EXPECT_EQ(otok->integer("format"), 1);
    EXPECT_EQ(otok->string("name"), "x");
    EXPECT_EQ(otok->boolean("strict"), true);
    EXPECT_EQ(otok->strings("list"), (std::vector<std::string>{"a", "b"}));
    EXPECT_FALSE(otok->string("absent").has_value());
    otok->finish();
    root.finish();
    EXPECT_FALSE(d.has_errors()) << messages(d);
}

TEST(toml_reader, reports_unknown_keys_with_position)
{
    diagnostics d;
    auto doc = toml_document::parse("[deps.openssl]\nversion = \"~> 3.5\"\noptins = {}\n", "otok.toml", d);
    ASSERT_TRUE(doc.has_value());
    auto root = doc->root();
    auto deps = root.table("deps");
    ASSERT_TRUE(deps.has_value());
    auto openssl = deps->table("openssl");
    ASSERT_TRUE(openssl.has_value());
    EXPECT_EQ(openssl->path(), "deps.openssl");
    openssl->string("version");
    openssl->finish();
    deps->finish();
    root.finish();
    ASSERT_EQ(d.error_count(), 1u) << messages(d);
    EXPECT_EQ(otok::format(d.items()[0]), "otok.toml:3:1: error: unknown key 'optins' in deps.openssl");
}

TEST(toml_reader, reports_wrong_types_and_missing_keys)
{
    diagnostics d;
    auto doc = toml_document::parse("format = \"1\"\nplatforms = [\"ios\", 2]\n", "otok.toml", d);
    ASSERT_TRUE(doc.has_value());
    auto root = doc->root();
    EXPECT_FALSE(root.integer("format").has_value());
    EXPECT_FALSE(root.strings("platforms").has_value());
    EXPECT_FALSE(root.table("apple", need::required).has_value());
    root.finish();
    ASSERT_EQ(d.error_count(), 3u) << messages(d);
    EXPECT_EQ(otok::format(d.items()[0]), "otok.toml:1:10: error: 'format' at the top level must be an integer, not a string");
    EXPECT_EQ(otok::format(d.items()[1]), "otok.toml:2:21: error: 'platforms' at the top level must be an array of strings; element 2 is an integer");
    EXPECT_NE(otok::format(d.items()[2]).find("missing required key 'apple' at the top level"), std::string::npos);
}

TEST(toml_reader, collects_every_error_in_file_order)
{
    diagnostics d;
    auto doc = toml_document::parse("zeta = 1\nalpha = 2\n[metadata]\nanything = true\n", "f.toml", d);
    ASSERT_TRUE(doc.has_value());
    auto root = doc->root();
    root.ignore("metadata");
    root.finish();
    ASSERT_EQ(d.error_count(), 2u) << messages(d);
    EXPECT_NE(d.items()[0].message.find("'zeta'"), std::string::npos);
    EXPECT_NE(d.items()[1].message.find("'alpha'"), std::string::npos);
}

TEST(toml_reader, lists_names_in_file_order)
{
    diagnostics d;
    auto doc = toml_document::parse("[deps.zlib]\n[deps.openssl]\n[deps.boost]\n", "otok.toml", d);
    ASSERT_TRUE(doc.has_value());
    auto root = doc->root();
    auto deps = root.table("deps");
    ASSERT_TRUE(deps.has_value());
    EXPECT_EQ(deps->keys(), (std::vector<std::string>{"zlib", "openssl", "boost"}));
    deps->finish();
    root.finish();
    EXPECT_FALSE(d.has_errors()) << messages(d);
}

TEST(toml_reader, reads_arrays_of_tables)
{
    diagnostics d;
    auto doc = toml_document::parse("[[package]]\nlibrary = \"a\"\n[[package]]\nlibrary = \"b\"\nextra = 1\n", "otok.lock", d);
    ASSERT_TRUE(doc.has_value());
    auto root = doc->root();
    auto packages = root.tables("package");
    ASSERT_TRUE(packages.has_value());
    ASSERT_EQ(packages->size(), 2u);
    EXPECT_EQ((*packages)[1].path(), "package[2]");
    for (auto& p : *packages) {
        p.string("library", need::required);
        p.finish();
    }
    root.finish();
    ASSERT_EQ(d.error_count(), 1u) << messages(d);
    EXPECT_EQ(otok::format(d.items()[0]), "otok.lock:5:1: error: unknown key 'extra' in package[2]");
}

TEST(toml_reader, reports_syntax_errors)
{
    diagnostics d;
    EXPECT_FALSE(toml_document::parse("[otok]\nformat = = 1\n", "otok.toml", d).has_value());
    ASSERT_EQ(d.error_count(), 1u);
    EXPECT_EQ(d.items()[0].where.line, 2u);
}

TEST(toml_reader, rejects_byte_order_mark)
{
    diagnostics d;
    EXPECT_FALSE(toml_document::parse("\xEF\xBB\xBF[otok]\n", "otok.toml", d).has_value());
    ASSERT_EQ(d.error_count(), 1u);
    EXPECT_NE(d.items()[0].message.find("byte order mark"), std::string::npos);
}
