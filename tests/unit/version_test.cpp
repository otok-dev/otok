// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Versions and version schemes: docs/formats/README.md 3.1.

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>

#include "otok/version/version.hpp"

using otok::version;
using otok::version_order;

namespace {

version v(std::string_view text)
{
    auto parsed = version::parse(text);
    EXPECT_TRUE(parsed.has_value()) << text << ": " << (parsed ? "" : parsed.error().message);
    return parsed ? *parsed : *version::parse("0");
}

// Every version in the list is strictly less than the next one.
void expect_ascending(version_order const& order, std::initializer_list<std::string_view> texts)
{
    std::vector<version> list;
    for (auto t : texts) list.push_back(v(t));
    for (std::size_t i = 0; i + 1 < list.size(); ++i) {
        EXPECT_TRUE(order.compare(list[i], list[i + 1]) < 0) << list[i].str() << " < " << list[i + 1].str();
        EXPECT_TRUE(order.compare(list[i + 1], list[i]) > 0) << list[i + 1].str() << " > " << list[i].str();
    }
}

void expect_equal(version_order const& order, std::string_view a, std::string_view b)
{
    EXPECT_TRUE(order.compare(v(a), v(b)) == 0) << a << " == " << b;
}

}

TEST(version, accepts_upstream_strings)
{
    for (auto text : {"3.5.4", "1.1.1w", "78.3", "1.91.0-1", "2026.10.01", "1.0.0-rc.1+build.5", "v8_11", "1"}) {
        EXPECT_TRUE(version::parse(text).has_value()) << text;
    }
}

TEST(version, rejects_invalid_strings)
{
    for (auto text : {"", "-1", "1-", ".1", "1.", "1 2", "1/2", "1,2", "ü1"}) {
        EXPECT_FALSE(version::parse(text).has_value()) << text;
    }
    EXPECT_TRUE(version::parse(std::string(64, '1')).has_value());
    EXPECT_FALSE(version::parse(std::string(65, '1')).has_value());
}

TEST(version, equality_is_textual)
{
    EXPECT_EQ(v("1.91.0-1"), v("1.91.0-1"));
    EXPECT_NE(v("1.91.0-1"), v("1.91.0.1"));
}

TEST(dotted, orders_as_specified)
{
    auto const order = version_order::dotted();
    expect_ascending(order, {"1.1.1", "1.1.1a", "1.1.1w", "1.1.2"});
    expect_ascending(order, {"1.91.0", "1.91.0-1"});
    expect_ascending(order, {"1.2", "1.2.0", "1.2.1"});
    expect_ascending(order, {"78.3", "78.10"});
    expect_ascending(order, {"9", "10", "20261001", "123456789012345678901234567890"});
}

TEST(dotted, suffix_is_not_a_prerelease)
{
    expect_ascending(version_order::dotted(), {"1.92.0", "1.92.0.beta1", "1.92.0.beta2"});
}

TEST(dotted, absent_number_sorts_before_any_number)
{
    expect_ascending(version_order::dotted(), {"1.beta", "1.0", "1.0rc1", "1.1"});
}

TEST(dotted, separators_carry_no_meaning)
{
    auto const order = version_order::dotted();
    expect_equal(order, "1.91.0-1", "1.91.0.1");
    expect_equal(order, "1_2", "1.2");
    expect_equal(order, "1.01", "1.1");
}

TEST(dotted, rejects_empty_parts)
{
    auto const order = version_order::dotted();
    EXPECT_FALSE(order.validate(v("1..2")).has_value());
    EXPECT_FALSE(order.validate(v("1.-2")).has_value());
    EXPECT_TRUE(order.compare(v("1..2"), v("1.2")) == std::partial_ordering::unordered);
}

TEST(semver, orders_as_the_specification_example)
{
    // semver.org, item 11
    expect_ascending(version_order::semver(), {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0"});
}

TEST(semver, ignores_build_metadata)
{
    expect_equal(version_order::semver(), "1.0.0+a", "1.0.0+b.2");
}

TEST(semver, validates_syntax)
{
    auto const order = version_order::semver();
    for (auto text : {"1.0.0", "0.0.1-rc.1", "10.20.30+build-7", "1.0.0-x-y.z"}) {
        EXPECT_TRUE(order.validate(v(text)).has_value()) << text;
    }
    for (auto text : {"1.0", "1", "01.0.0", "1.00.0", "1.0.0-01", "1.0.0.0", "1.0.0-a..b", "1_0_0", "1.0.0-a_b"}) {
        EXPECT_FALSE(order.validate(v(text)).has_value()) << text;
    }
}

TEST(explicit_order, orders_as_listed)
{
    auto const order = version_order::explicit_order({v("2b"), v("2a"), v("10")});
    ASSERT_TRUE(order.has_value());
    expect_ascending(*order, {"2b", "2a", "10"});
    EXPECT_TRUE(order->validate(v("2a")).has_value());
    EXPECT_FALSE(order->validate(v("3")).has_value());
    EXPECT_TRUE(order->compare(v("3"), v("2a")) == std::partial_ordering::unordered);
}

TEST(explicit_order, rejects_duplicates)
{
    EXPECT_FALSE(version_order::explicit_order({v("1"), v("2"), v("1")}).has_value());
}
