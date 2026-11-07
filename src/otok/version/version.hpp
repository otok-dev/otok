// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <compare>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"

// Versions and their order (docs/formats/README.md 3.1).
namespace otok {

// An upstream version string, kept verbatim: `3.5.4`, `1.1.1w`, `1.91.0-1`. Only the character set
// is checked here; whether it is meaningful, and how it orders, depends on the library's version
// scheme, so versions are compared through a version_order. operator== compares the text.
class version
{
public:
    static constexpr std::size_t max_length = 64;

    static result<version> parse(std::string_view text);

    std::string const& str() const noexcept { return text_; }

    friend bool operator==(version const&, version const&) = default;

private:
    explicit version(std::string text) : text_{std::move(text)} {}

    std::string text_;
};

enum class version_scheme
{
    dotted,         // part by part, numbers with letter suffixes (the default)
    semver,         // Semantic Versioning 2.0.0 precedence
    explicit_order  // the order listed in the library file of the index
};

class version_constraint;

// The order of a library's versions.
class version_order
{
public:
    static version_order dotted();
    static version_order semver();
    // `order` lists all known versions, oldest first; they must be distinct.
    static result<version_order> explicit_order(std::vector<version> order);

    version_scheme scheme() const noexcept { return scheme_; }

    // A version is valid in a scheme when it parses as a version of that scheme
    // (semver syntax; dotted parts; listed in the explicit order).
    result<void> validate(version const& v) const;

    // Unordered only when a version is not valid in the scheme. Versions with different text may
    // compare equal (`1.91.0-1` and `1.91.0.1` in the dotted scheme).
    std::partial_ordering compare(version const& a, version const& b) const;

private:
    friend class version_constraint;

    explicit version_order(version_scheme scheme) noexcept : scheme_{scheme} {}

    // Constraint operands may be partial in the semver scheme (`3.5` means `3.5.0`).
    result<void> validate_operand(version const& v) const;
    std::partial_ordering compare(version const& a, version const& b, bool partial_b) const;

    version_scheme scheme_;
    std::vector<version> order_;
};

}
