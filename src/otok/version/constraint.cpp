// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/version/constraint.hpp"

#include "otok/version/version_detail.hpp"

namespace otok {

namespace {

std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

struct op_spelling
{
    std::string_view text;
    constraint_op op;
};

// Longer spellings first, so ">=" is not read as ">".
constexpr op_spelling op_spellings[] = {
    {"~>", constraint_op::compatible},
    {">=", constraint_op::greater_equal},
    {"<=", constraint_op::less_equal},
    {"=", constraint_op::equal},
    {">", constraint_op::greater},
    {"<", constraint_op::less},
};

std::string_view spelling(constraint_op op) noexcept
{
    for (op_spelling const& s : op_spellings) {
        if (s.op == op) return s.text;
    }
    return "?";
}

result<version_comparison> parse_comparison(std::string_view text, std::string_view whole)
{
    text = trim(text);
    if (text.empty()) {
        return fail("constraint '" + std::string{whole} + "' has an empty comparison");
    }
    constraint_op op = constraint_op::equal;
    for (op_spelling const& s : op_spellings) {
        if (text.starts_with(s.text)) {
            op = s.op;
            text = trim(text.substr(s.text.size()));
            break;
        }
    }
    if (text.empty()) {
        return fail("constraint '" + std::string{whole} + "' has an operator without a version");
    }
    auto operand = version::parse(text);
    if (!operand) {
        return fail("constraint '" + std::string{whole} + "': " + operand.error().message);
    }
    return version_comparison{op, std::move(*operand)};
}

}

result<version_constraint> version_constraint::parse(std::string_view text)
{
    version_constraint constraint;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != ',') {
            continue;
        }
        auto comparison = parse_comparison(text.substr(start, i - start), text);
        if (!comparison) {
            return std::unexpected{comparison.error()};
        }
        constraint.comparisons_.push_back(std::move(*comparison));
        start = i + 1;
    }
    return constraint;
}

result<void> version_constraint::validate(version_order const& order) const
{
    for (version_comparison const& c : comparisons_) {
        if (order.scheme() == version_scheme::explicit_order && c.op != constraint_op::equal) {
            return fail("'" + std::string{spelling(c.op)} + "' is not available for a library with an explicit version order; use '='");
        }
        if (auto valid = order.validate_operand(c.operand); !valid) {
            return fail("constraint '" + str() + "': " + valid.error().message);
        }
        if (c.op == constraint_op::compatible) {
            if (auto bound = detail::compatible_upper_bound(order.scheme(), c.operand); !bound) {
                return std::unexpected{bound.error()};
            }
        }
    }
    return {};
}

bool version_constraint::matches(version const& v, version_order const& order) const
{
    for (version_comparison const& c : comparisons_) {
        std::partial_ordering const cmp = order.compare(v, c.operand, true);
        if (cmp == std::partial_ordering::unordered) {
            return false;
        }
        bool holds = false;
        switch (c.op) {
        case constraint_op::equal:         holds = cmp == 0; break;
        case constraint_op::greater_equal: holds = cmp >= 0; break;
        case constraint_op::greater:       holds = cmp > 0; break;
        case constraint_op::less_equal:    holds = cmp <= 0; break;
        case constraint_op::less:          holds = cmp < 0; break;
        case constraint_op::compatible: {
            auto const bound = detail::compatible_upper_bound(order.scheme(), c.operand);
            holds = bound && cmp >= 0 && order.compare(v, *bound, true) < 0;
            break;
        }
        }
        if (!holds) {
            return false;
        }
    }
    return true;
}

bool version_constraint::names_exactly(version const& v, version_order const& order) const
{
    for (version_comparison const& c : comparisons_) {
        if (c.op == constraint_op::equal && order.compare(v, c.operand, true) == 0) {
            return true;
        }
    }
    return false;
}

std::string version_constraint::str() const
{
    std::string text;
    for (version_comparison const& c : comparisons_) {
        if (!text.empty()) text += ", ";
        text.append(spelling(c.op)).append(" ").append(c.operand.str());
    }
    return text;
}

}
