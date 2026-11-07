// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/option_values.hpp"

#include "otok/core/names.hpp"

namespace otok {

option_values read_option_values(toml_table& parent, std::string_view key)
{
    option_values result;
    auto table = parent.table(key);
    if (!table) return result;
    for (std::string const& name : table->keys()) {
        toml::node const* node = table->node(name);
        source_position where = table->position(name);
        if (!is_option_name(name)) {
            table->error(where, "invalid option name '" + name + "' in " + table->path()
                + "; option names are lowercase letters, digits and '-', starting with a letter");
            continue;
        }
        if (auto data = to_option_data(*node)) {
            result.emplace(name, option_value{std::move(*data), std::move(where)});
        } else {
            table->error(where, table->describe(name) + " must be a boolean, an integer, a string or an array of strings, not "
                + std::string{describe_type(node->type())} + (node->is_array() ? " of mixed or non-string values" : ""));
        }
    }
    table->finish();
    return result;
}

}
