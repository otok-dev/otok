// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"

// SPDX license expressions (docs/formats/README.md 5): `MIT`, `MIT OR Apache-2.0`,
// `(GPL-2.0-only WITH Classpath-exception-2.0) AND BSD-3-Clause`.
namespace otok {

struct license_expression
{
    enum class kind
    {
        license,  // `id`, possibly "<license> WITH <exception>"
        all_of,   // AND
        any_of    // OR
    };

    kind type = kind::license;
    std::string id;
    std::vector<license_expression> operands;
};

// WITH binds tighter than AND, AND tighter than OR; operators are upper case.
result<license_expression> parse_license_expression(std::string_view text);

// Whether the expression can be satisfied with licenses for which `allowed` is true:
// `A OR B` if either side can, `A AND B` if both can.
bool license_satisfiable(license_expression const& e, std::function<bool(std::string_view id)> const& allowed);

}
