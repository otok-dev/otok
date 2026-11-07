// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "otok/core/error.hpp"
#include "otok/version/constraint.hpp"

// The language standards a project builds with (otok-toml.md 5.1) and the ones a recipe allows (recipe-toml.md 3).
namespace otok {

enum class language
{
    c,
    cxx
};

inline constexpr std::array languages{language::c, language::cxx};

// "c-standard", "cxx-standard": the key in otok.toml and recipe.toml.
std::string_view standard_key(language l) noexcept;

// "C", "C++".
std::string_view language_name(language l) noexcept;

// The standards otok knows, oldest first, by their two-digit numbers: C 89, 99, 11, 17, 23; C++ 11, 14, 17, 20, 23, 26.
std::span<int const> known_standards(language l) noexcept;

// "89, 99, 11, 17 or 23", for messages.
std::string known_standards_text(language l);

// The year of a standard's two-digit number: 89 is 1989, 11 is 2011.
int standard_year(int standard) noexcept;

// A value per language.
template <typename T>
struct per_language
{
    T c{};
    T cxx{};

    T& operator[](language l) noexcept { return l == language::c ? c : cxx; }
    T const& operator[](language l) const noexcept { return l == language::c ? c : cxx; }

    friend bool operator==(per_language const&, per_language const&) = default;
};

// The standard of each language, where one is set.
using language_standards = per_language<std::optional<int>>;

// The standards of a language a recipe builds with: a constraint of two-digit numbers, ">= 99" or ">= 14, <= 20", compared
// by year, so that C99 is older than C11.
class standard_constraint
{
public:
    static result<standard_constraint> parse(std::string_view text);

    bool allows(int standard) const;

    // Canonical text: `>= 14, <= 20`.
    std::string str() const { return written_.str(); }

private:
    standard_constraint(version_constraint written, version_constraint years) : written_{std::move(written)}, years_{std::move(years)} {}

    version_constraint written_;
    version_constraint years_;
};

using standard_constraints = per_language<std::optional<standard_constraint>>;

}
