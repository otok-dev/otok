// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <map>
#include <string>
#include <string_view>

#include "otok/core/diagnostics.hpp"
#include "otok/core/option_data.hpp"
#include "otok/core/toml_reader.hpp"

namespace otok {

// A value given for a recipe option (by a project, a profile or a dependent recipe); its type is
// checked against the recipe's schema when the recipe is known.
struct option_value
{
    option_data value;
    source_position where;
};

using option_values = std::map<std::string, option_value, std::less<>>;

// Reads a table of option values (`options = { … }`, `requires-options = { … }`): checks option
// names and that each value is a boolean, an integer, a string or an array of strings.
option_values read_option_values(toml_table& parent, std::string_view key);

}
