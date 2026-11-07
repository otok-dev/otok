// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/version/version.hpp"

#include <algorithm>
#include <array>

#include "otok/version/version_detail.hpp"

namespace otok {

namespace {

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

bool is_alnum(char c) noexcept
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_all_digits(std::string_view s) noexcept
{
    return !s.empty() && std::ranges::all_of(s, is_digit);
}

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

// Numbers are compared as decimal strings, so dates and long build numbers never overflow.
std::string_view strip_leading_zeros(std::string_view digits) noexcept
{
    while (digits.size() > 1 && digits.front() == '0') {
        digits.remove_prefix(1);
    }
    return digits;
}

std::strong_ordering compare_numbers(std::string_view a, std::string_view b) noexcept
{
    a = strip_leading_zeros(a);
    b = strip_leading_zeros(b);
    if (a.size() != b.size()) {
        return a.size() <=> b.size();
    }
    return a.compare(b) <=> 0;
}

std::string increment_number(std::string_view digits)
{
    std::string s{strip_leading_zeros(digits)};
    std::size_t i = s.size();
    while (i > 0 && s[i - 1] == '9') {
        s[--i] = '0';
    }
    if (i == 0) {
        s.insert(s.begin(), '1');
    } else {
        ++s[i - 1];
    }
    return s;
}

// dotted: a part is a number (possibly absent) followed by a suffix (possibly empty).
struct dotted_part
{
    std::string_view number;
    std::string_view suffix;
};

result<std::vector<dotted_part>> parse_dotted(std::string_view text)
{
    std::vector<dotted_part> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != '.' && text[i] != '-' && text[i] != '_') {
            continue;
        }
        std::string_view const part = text.substr(start, i - start);
        if (part.empty()) {
            return fail("version " + in_quotes(text) + " has an empty part");
        }
        std::size_t digits = 0;
        while (digits < part.size() && is_digit(part[digits])) {
            ++digits;
        }
        parts.push_back({part.substr(0, digits), part.substr(digits)});
        start = i + 1;
    }
    return parts;
}

std::strong_ordering compare_dotted(std::vector<dotted_part> const& a, std::vector<dotted_part> const& b) noexcept
{
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        // a missing part sorts first
        if (i == a.size()) return std::strong_ordering::less;
        if (i == b.size()) return std::strong_ordering::greater;

        dotted_part const& x = a[i];
        dotted_part const& y = b[i];
        // an absent number sorts before any number
        if (x.number.empty() != y.number.empty()) {
            return x.number.empty() ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        if (!x.number.empty()) {
            if (auto c = compare_numbers(x.number, y.number); c != 0) return c;
        }
        // no suffix before a suffix, suffixes in ASCII order
        if (auto c = x.suffix.compare(y.suffix) <=> 0; c != 0) return c;
    }
    return std::strong_ordering::equal;
}

// semver: MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]; with `partial`, also MAJOR and MAJOR.MINOR.
struct semver_parts
{
    std::array<std::string_view, 3> core{"0", "0", "0"};
    std::size_t core_count = 0;
    std::vector<std::string_view> prerelease;
};

result<std::vector<std::string_view>> split_identifiers(std::string_view text, std::string_view what, std::string_view version_text)
{
    std::vector<std::string_view> ids;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != '.') {
            if (!is_alnum(text[i]) && text[i] != '-') {
                return fail("semver version " + in_quotes(version_text) + " has an invalid character in its " + std::string{what});
            }
            continue;
        }
        std::string_view const id = text.substr(start, i - start);
        if (id.empty()) {
            return fail("semver version " + in_quotes(version_text) + " has an empty " + std::string{what} + " identifier");
        }
        ids.push_back(id);
        start = i + 1;
    }
    return ids;
}

result<semver_parts> parse_semver(std::string_view text, bool partial)
{
    semver_parts parts;

    std::string_view main = text;
    if (std::size_t const plus = text.find('+'); plus != std::string_view::npos) {
        if (auto build = split_identifiers(text.substr(plus + 1), "build metadata", text); !build) {
            return std::unexpected{build.error()};
        }
        main = text.substr(0, plus);
    }

    std::string_view core = main;
    if (std::size_t const dash = main.find('-'); dash != std::string_view::npos) {
        auto pre = split_identifiers(main.substr(dash + 1), "pre-release", text);
        if (!pre) {
            return std::unexpected{pre.error()};
        }
        for (std::string_view id : *pre) {
            if (is_all_digits(id) && id.size() > 1 && id.front() == '0') {
                return fail("semver version " + in_quotes(text) + " has a numeric pre-release identifier with a leading zero");
            }
        }
        parts.prerelease = std::move(*pre);
        core = main.substr(0, dash);
    }

    std::size_t start = 0;
    for (std::size_t i = 0; i <= core.size(); ++i) {
        if (i < core.size() && core[i] != '.') {
            continue;
        }
        std::string_view const number = core.substr(start, i - start);
        if (parts.core_count == 3 || !is_all_digits(number)) {
            return fail("version " + in_quotes(text) + " is not MAJOR.MINOR.PATCH as semver requires");
        }
        if (number.size() > 1 && number.front() == '0') {
            return fail("semver version " + in_quotes(text) + " has a number with a leading zero");
        }
        parts.core[parts.core_count++] = number;
        start = i + 1;
    }
    if (parts.core_count < 3 && (!partial || !parts.prerelease.empty())) {
        return fail("version " + in_quotes(text) + " is not MAJOR.MINOR.PATCH as semver requires");
    }
    return parts;
}

std::strong_ordering compare_semver(semver_parts const& a, semver_parts const& b) noexcept
{
    for (std::size_t i = 0; i < 3; ++i) {
        if (auto c = compare_numbers(a.core[i], b.core[i]); c != 0) return c;
    }
    // a pre-release sorts before the release
    if (a.prerelease.empty() != b.prerelease.empty()) {
        return a.prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
    }
    for (std::size_t i = 0; i < std::min(a.prerelease.size(), b.prerelease.size()); ++i) {
        std::string_view const x = a.prerelease[i];
        std::string_view const y = b.prerelease[i];
        bool const xn = is_all_digits(x);
        bool const yn = is_all_digits(y);
        if (xn != yn) {
            // numeric identifiers sort before alphanumeric ones
            return xn ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        auto const c = xn ? compare_numbers(x, y) : x.compare(y) <=> 0;
        if (c != 0) return c;
    }
    return a.prerelease.size() <=> b.prerelease.size();
}

}

result<version> version::parse(std::string_view text)
{
    if (text.empty()) {
        return fail("version is empty");
    }
    if (text.size() > max_length) {
        return fail("version " + in_quotes(text) + " is longer than " + std::to_string(max_length) + " characters");
    }
    for (char c : text) {
        if (!is_alnum(c) && c != '.' && c != '-' && c != '_' && c != '+') {
            return fail("version " + in_quotes(text) + " contains " + in_quotes(std::string_view{&c, 1})
                + "; allowed are letters, digits, '.', '-', '_' and '+'");
        }
    }
    if (!is_alnum(text.front()) || !is_alnum(text.back())) {
        return fail("version " + in_quotes(text) + " must start and end with a letter or a digit");
    }
    return version{std::string{text}};
}

version_order version_order::dotted()
{
    return version_order{version_scheme::dotted};
}

version_order version_order::semver()
{
    return version_order{version_scheme::semver};
}

result<version_order> version_order::explicit_order(std::vector<version> order)
{
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (order[i] == order[j]) {
                return fail("version " + in_quotes(order[i].str()) + " is listed twice in the explicit order");
            }
        }
    }
    version_order listed{version_scheme::explicit_order};
    listed.order_ = std::move(order);
    return listed;
}

result<void> version_order::validate(version const& v) const
{
    switch (scheme_) {
    case version_scheme::dotted:
        if (auto parts = parse_dotted(v.str()); !parts) return std::unexpected{parts.error()};
        return {};
    case version_scheme::semver:
        if (auto parts = parse_semver(v.str(), false); !parts) return std::unexpected{parts.error()};
        return {};
    case version_scheme::explicit_order:
        if (std::ranges::find(order_, v) == order_.end()) {
            return fail("version " + in_quotes(v.str()) + " is not in the explicit order of the library");
        }
        return {};
    }
    return {};
}

result<void> version_order::validate_operand(version const& v) const
{
    if (scheme_ == version_scheme::semver) {
        if (auto parts = parse_semver(v.str(), true); !parts) return std::unexpected{parts.error()};
        return {};
    }
    return validate(v);
}

std::partial_ordering version_order::compare(version const& a, version const& b) const
{
    return compare(a, b, false);
}

std::partial_ordering version_order::compare(version const& a, version const& b, bool partial_b) const
{
    switch (scheme_) {
    case version_scheme::dotted: {
        auto const x = parse_dotted(a.str());
        auto const y = parse_dotted(b.str());
        if (!x || !y) return std::partial_ordering::unordered;
        return compare_dotted(*x, *y);
    }
    case version_scheme::semver: {
        auto const x = parse_semver(a.str(), false);
        auto const y = parse_semver(b.str(), partial_b);
        if (!x || !y) return std::partial_ordering::unordered;
        return compare_semver(*x, *y);
    }
    case version_scheme::explicit_order: {
        auto const x = std::ranges::find(order_, a);
        auto const y = std::ranges::find(order_, b);
        if (x == order_.end() || y == order_.end()) return std::partial_ordering::unordered;
        return x <=> y;
    }
    }
    return std::partial_ordering::unordered;
}

namespace detail {

result<version> compatible_upper_bound(version_scheme scheme, version const& operand)
{
    switch (scheme) {
    case version_scheme::dotted: {
        auto parts = parse_dotted(operand.str());
        if (!parts) return std::unexpected{parts.error()};
        // the last given part may grow: increment the one before it (or the only one)
        std::size_t const grow = parts->size() == 1 ? 0 : parts->size() - 2;
        if ((*parts)[grow].number.empty()) {
            return fail("'~> " + operand.str() + "' needs a number in the part to increment");
        }
        std::string bound;
        for (std::size_t i = 0; i < grow; ++i) {
            bound.append((*parts)[i].number).append((*parts)[i].suffix).push_back('.');
        }
        bound += increment_number((*parts)[grow].number);
        return version::parse(bound);
    }
    case version_scheme::semver: {
        auto parts = parse_semver(operand.str(), true);
        if (!parts) return std::unexpected{parts.error()};
        std::size_t const grow = parts->core_count == 1 ? 0 : parts->core_count - 2;
        std::string const major{strip_leading_zeros(parts->core[0])};
        return version::parse(grow == 0
            ? increment_number(parts->core[0]) + ".0.0"
            : major + "." + increment_number(parts->core[1]) + ".0");
    }
    case version_scheme::explicit_order:
        return fail("'~>' is not available for a library with an explicit version order; use '='");
    }
    return fail("unknown version scheme");
}

}

}
