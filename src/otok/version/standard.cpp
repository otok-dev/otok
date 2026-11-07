// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/version/standard.hpp"

#include <cctype>

namespace otok {

std::string_view standard_key(language l) noexcept
{
    return l == language::c ? "c-standard" : "cxx-standard";
}

std::string_view language_name(language l) noexcept
{
    return l == language::c ? "C" : "C++";
}

std::span<int const> known_standards(language l) noexcept
{
    static constexpr std::array c{89, 99, 11, 17, 23};
    static constexpr std::array cxx{11, 14, 17, 20, 23, 26};
    if (l == language::c) return c;
    return cxx;
}

std::string known_standards_text(language l)
{
    std::span<int const> const all = known_standards(l);
    std::string text;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i > 0) text += i + 1 == all.size() ? " or " : ", ";
        text += std::to_string(all[i]);
    }
    return text;
}

int standard_year(int standard) noexcept
{
    return standard >= 70 ? 1900 + standard : 2000 + standard;
}

result<standard_constraint> standard_constraint::parse(std::string_view text)
{
    std::string const example = "such as \">= 17\" or \">= 14, <= 20\"";
    if (text.find_first_not_of("0123456789<>=~!, ") != std::string_view::npos) {
        return fail("must compare standards by number, " + example + ", not '" + std::string{text} + "'");
    }
    // the same constraint over years, for the order
    std::string years;
    for (std::size_t i = 0; i < text.size();) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            years += text[i++];
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
        if (end - i > 2) return fail("must name standards by their two-digit numbers, " + example + ", not '" + std::string{text} + "'");
        years += std::to_string(standard_year(std::stoi(std::string{text.substr(i, end - i)})));
        i = end;
    }
    auto written = version_constraint::parse(text);
    if (!written) return fail("is not a constraint: " + written.error().message);
    if (auto valid = written->validate(version_order::dotted()); !valid) return fail("is not a constraint: " + valid.error().message);
    auto by_year = version_constraint::parse(years);
    if (!by_year) return fail("is not a constraint: " + by_year.error().message);
    if (auto valid = by_year->validate(version_order::dotted()); !valid) return fail("is not a constraint: " + valid.error().message);
    return standard_constraint{std::move(*written), std::move(*by_year)};
}

bool standard_constraint::allows(int standard) const
{
    auto const year = version::parse(std::to_string(standard_year(standard)));
    return year && years_.matches(*year, version_order::dotted());
}

}
