// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// SPDX license expressions: docs/formats/README.md 5.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "otok/core/license.hpp"

using namespace otok;

namespace {

bool passes(std::string_view expression, std::set<std::string, std::less<>> const& denied)
{
    auto e = parse_license_expression(expression);
    EXPECT_TRUE(e.has_value()) << expression;
    return e && license_satisfiable(*e, [&](std::string_view id) { return !denied.contains(id); });
}

}

TEST(license, parses_expressions)
{
    for (auto text : {"MIT", "MIT OR Apache-2.0", "GPL-2.0-or-later WITH Classpath-exception-2.0", "(MIT AND BSD-3-Clause) OR Apache-2.0",
             "LicenseRef-custom", "A AND B AND C"}) {
        EXPECT_TRUE(parse_license_expression(text).has_value()) << text;
    }
    for (auto text : {"", "MIT OR", "AND MIT", "(MIT", "MIT)", "MIT WITH", "MIT / Apache", "MIT Apache-2.0"}) {
        EXPECT_FALSE(parse_license_expression(text).has_value()) << text;
    }
}

TEST(license, keeps_with_as_part_of_the_id)
{
    auto e = parse_license_expression("GPL-2.0-only WITH Classpath-exception-2.0");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->type, license_expression::kind::license);
    EXPECT_EQ(e->id, "GPL-2.0-only WITH Classpath-exception-2.0");
}

TEST(license, and_binds_tighter_than_or)
{
    auto e = parse_license_expression("A OR B AND C");
    ASSERT_TRUE(e.has_value());
    ASSERT_EQ(e->type, license_expression::kind::any_of);
    ASSERT_EQ(e->operands.size(), 2u);
    EXPECT_EQ(e->operands[1].type, license_expression::kind::all_of);
}

TEST(license, evaluates_against_a_policy)
{
    std::set<std::string, std::less<>> const gpl3{"GPL-3.0-only", "GPL-3.0-or-later"};
    EXPECT_TRUE(passes("MIT", gpl3));
    EXPECT_FALSE(passes("GPL-3.0-only", gpl3));
    EXPECT_TRUE(passes("MIT OR GPL-3.0-only", gpl3));       // MIT can be chosen
    EXPECT_FALSE(passes("MIT AND GPL-3.0-only", gpl3));     // both apply
    EXPECT_TRUE(passes("(GPL-3.0-only AND MIT) OR BSD-2-Clause", gpl3));
}
