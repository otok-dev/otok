// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"
#include "otok/version/version.hpp"

// Version constraints (docs/formats/README.md 3.2).
namespace otok {

enum class constraint_op
{
    equal,          // "=" or a bare version
    greater_equal,  // ">="
    greater,        // ">"
    less_equal,     // "<="
    less,           // "<"
    compatible      // "~>": at least the version, below the next value of the last but one part
};

struct version_comparison
{
    constraint_op op;
    version operand;
};

// A comma-separated list of comparisons, all of which must hold: `~> 3.5`, `>= 3.0, < 3.6`,
// `1.1.1w`. Parsing checks the syntax only; what the operands mean depends on the library's
// version scheme, so validate() is called once the scheme is known.
class version_constraint
{
public:
    static result<version_constraint> parse(std::string_view text);

    std::span<version_comparison const> comparisons() const noexcept { return comparisons_; }

    // Checks the constraint against a scheme: operands valid in it (semver operands may be partial,
    // `3.5`), `~>` incrementable, only `=` for the explicit scheme.
    result<void> validate(version_order const& order) const;

    // Whether `v` satisfies every comparison. Expects a constraint that passed validate(); a version
    // that is not valid in the scheme never matches. Pre-release rules (common 3.2) are the
    // resolver's: they depend on the index entry's channel, not on the version text.
    bool matches(version const& v, version_order const& order) const;

    // Whether the constraint names `v` with `=` — the way to ask for one pre-release explicitly.
    bool names_exactly(version const& v, version_order const& order) const;

    // Canonical text: `>= 3.0, < 3.6`; a bare version is written `= 1.1.1w`.
    std::string str() const;

private:
    std::vector<version_comparison> comparisons_;
};

}
