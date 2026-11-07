// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

namespace otok {

// A value of an option or of a recipe variable: a boolean, an integer, a string or an array of
// strings (docs/formats/recipe-toml.md 7, 8).
using option_data = std::variant<bool, std::int64_t, std::string, std::vector<std::string>>;

// Converts a TOML value; nothing for other types (floats, tables, dates, arrays of non-strings).
inline std::optional<option_data> to_option_data(toml::node const& node)
{
    if (auto const* b = node.as_boolean()) return option_data{b->get()};
    if (auto const* i = node.as_integer()) return option_data{i->get()};
    if (auto const* s = node.as_string()) return option_data{s->get()};
    if (auto const* a = node.as_array()) {
        std::vector<std::string> list;
        for (toml::node const& element : *a) {
            auto const* s = element.as_string();
            if (!s) return std::nullopt;
            list.push_back(s->get());
        }
        return option_data{std::move(list)};
    }
    return std::nullopt;
}

// "a boolean", "an integer", "a string", "an array of strings".
inline char const* describe(option_data const& value) noexcept
{
    switch (value.index()) {
    case 0: return "a boolean";
    case 1: return "an integer";
    case 2: return "a string";
    default: return "an array of strings";
    }
}

}
