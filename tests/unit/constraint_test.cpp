// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Version constraints: docs/formats/README.md 3.2.

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>

#include "otok/version/constraint.hpp"

using otok::version;
using otok::version_constraint;
using otok::version_order;

namespace {

version v(std::string_view text)
{
    auto parsed = version::parse(text);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed ? *parsed : *version::parse("0");
}

version_constraint c(std::string_view text, version_order const& order)
{
    auto parsed = version_constraint::parse(text);
    EXPECT_TRUE(parsed.has_value()) << text << ": " << (parsed ? "" : parsed.error().message);
    if (!parsed) return *version_constraint::parse("0");
    auto valid = parsed->validate(order);
    EXPECT_TRUE(valid.has_value()) << text << ": " << (valid ? "" : valid.error().message);
    return *parsed;
}

void expect_matches(std::string_view constraint, version_order const& order,
    std::initializer_list<std::string_view> yes, std::initializer_list<std::string_view> no)
{
    auto const parsed = c(constraint, order);
    for (auto t : yes) EXPECT_TRUE(parsed.matches(v(t), order)) << "'" << constraint << "' should match " << t;
    for (auto t : no) EXPECT_FALSE(parsed.matches(v(t), order)) << "'" << constraint << "' should not match " << t;
}

bool validates(std::string_view constraint, version_order const& order)
{
    auto parsed = version_constraint::parse(constraint);
    return parsed && parsed->validate(order);
}

}

TEST(constraint, bare_version_is_an_exact_match)
{
    auto const order = version_order::dotted();
    expect_matches("1.1.1w", order, {"1.1.1w"}, {"1.1.1", "1.1.1x", "1.1.2"});
    expect_matches("= 1.1.1w", order, {"1.1.1w"}, {"1.1.1v"});
}

TEST(constraint, compatible_grows_the_last_given_part)
{
    auto const order = version_order::dotted();
    expect_matches("~> 1.1.1", order, {"1.1.1", "1.1.1a", "1.1.1w", "1.1.2", "1.1.99"}, {"1.1.0", "1.2", "1.2.0", "2.0"});
    expect_matches("~> 3.5", order, {"3.5", "3.5.4", "3.6", "3.99.1"}, {"3.4.9", "4", "4.0"});
    expect_matches("~> 3.5.4", order, {"3.5.4", "3.5.10"}, {"3.5.3", "3.6"});
    expect_matches("~> 1.91.0-1", order, {"1.91.0-1", "1.91.0-7"}, {"1.91.0", "1.91.1"});
}

TEST(constraint, compatible_with_one_part_is_the_same_as_with_two)
{
    auto const order = version_order::dotted();
    expect_matches("~> 3", order, {"3", "3.0", "3.0.1", "3.1", "3.9.9"}, {"2.9", "4", "4.0"});
    // "3" < "3.0": a missing part sorts first
    expect_matches("~> 3.0", order, {"3.0", "3.0.1", "3.1", "3.9.9"}, {"3", "4.0"});
    expect_matches("~> 3.0.0", order, {"3.0.0", "3.0.1"}, {"3.1"});
    expect_matches("~> 9", order, {"9.5"}, {"10"});
}

TEST(constraint, compatible_needs_a_number_to_increment)
{
    auto const order = version_order::dotted();
    EXPECT_FALSE(validates("~> beta.1", order));
    EXPECT_FALSE(validates("~> beta", order));
    EXPECT_TRUE(validates("~> 1.2beta", order));
    // the part before the last one is incremented: here "1", which has a number
    expect_matches("~> 1.beta", order, {"1.beta", "1.0", "1.9"}, {"1.alpha", "2"});
}

TEST(constraint, ranges_combine_comparisons)
{
    auto const order = version_order::dotted();
    expect_matches(">= 3.0, < 3.6", order, {"3.0", "3.5.4"}, {"2.9", "3.6", "3.6.0"});
    expect_matches(">= 1.1.1", order, {"1.1.1", "1.1.1w", "99"}, {"1.1.0"});
    expect_matches("> 1.0, <= 2.0", order, {"1.0.1", "2.0"}, {"1.0", "2.0.1"});
}

TEST(constraint, whitespace_is_ignored)
{
    auto const order = version_order::dotted();
    expect_matches(">=3.0,<3.6", order, {"3.1"}, {"3.6"});
    expect_matches("  ~>  3.5  ", order, {"3.5.1"}, {"4"});
}

TEST(constraint, rejects_bad_syntax)
{
    for (auto text : {"", ">= ", "~>", "1,", ",1", "=> 1", ">= 1 2", "== 1", ">= 1;", "1, , 2"}) {
        EXPECT_FALSE(version_constraint::parse(text).has_value()) << "'" << text << "'";
    }
}

TEST(constraint, canonical_text)
{
    auto const parsed = version_constraint::parse(">=3.0,<3.6");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->str(), ">= 3.0, < 3.6");
    EXPECT_EQ(version_constraint::parse("1.1.1w")->str(), "= 1.1.1w");
}

TEST(constraint, semver_operands_may_be_partial)
{
    auto const order = version_order::semver();
    expect_matches(">= 3.5", order, {"3.5.0", "3.6.1"}, {"3.4.9"});
    expect_matches("~> 3.5", order, {"3.5.0", "3.9.9"}, {"4.0.0", "3.4.0"});
    expect_matches("~> 3", order, {"3.0.0", "3.9.9"}, {"4.0.0"});
    expect_matches("~> 3.5.4", order, {"3.5.4", "3.5.9"}, {"3.6.0"});
    expect_matches("= 1.0.0-rc.1", order, {"1.0.0-rc.1"}, {"1.0.0"});
    EXPECT_FALSE(validates(">= 3.5-rc.1", order));
    EXPECT_FALSE(validates(">= 03.5", order));
}

TEST(constraint, explicit_order_allows_only_equality)
{
    auto const order = version_order::explicit_order({v("b"), v("a")});
    ASSERT_TRUE(order.has_value());
    expect_matches("= a", *order, {"a"}, {"b"});
    expect_matches("b", *order, {"b"}, {"a"});
    EXPECT_FALSE(validates(">= a", *order));
    EXPECT_FALSE(validates("~> a", *order));
    EXPECT_FALSE(validates("= c", *order));
}

TEST(constraint, names_exactly_finds_explicit_requests)
{
    auto const order = version_order::dotted();
    auto const exact = c("= 1.92.0.beta1", order);
    EXPECT_TRUE(exact.names_exactly(v("1.92.0.beta1"), order));
    EXPECT_FALSE(exact.names_exactly(v("1.92.0.beta2"), order));
    EXPECT_FALSE(c(">= 1.92.0.beta1", order).names_exactly(v("1.92.0.beta1"), order));
}
