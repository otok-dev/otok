// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/core/error.hpp"
#include "otok/core/option_data.hpp"
#include "otok/recipe/recipe.hpp"

// Option values against a recipe's schema and the merging of requests (docs/formats/recipe-toml.md
// 8, 8.1).
namespace otok {

// A value asked for an option: by the project, a profile or a dependent recipe.
struct option_request
{
    option_data value;      // canonical (check_option_value)
    std::string requester;  // "otok.toml", "profile 'ci'", "apotocki/libssh2 1.11.1"
    source_position where;
};

// The value checked against the option's type and allowed values, in canonical form: a list
// deduplicated and ordered as in `values` (sorted without `values`). The error message is complete
// apart from who gave the value.
result<option_data> check_option_value(option_schema const& schema, option_data const& value);

// The merged value of one option (8.1): additive options take the union (bool: OR), all others need
// equal requests. No requests: the default. The error lists the requesters and their values.
result<option_data> merge_option(std::string_view library, option_schema const& schema,
    std::span<option_request const> requests);

// `true`, `3`, `"shared"`, `["filesystem", "json"]`: the TOML spelling, for messages.
std::string format_option_value(option_data const& value);

// Merged values of every option of a recipe.
using merged_options = std::map<std::string, option_data, std::less<>>;

// Whether a `when` condition holds for a slice (recipe-toml.md 9.1). `options` must contain the
// options the condition names (the parser checked that they exist). `arch` and `environment` are those of the slice;
// empty, or null, where they are not known (the resolver, which resolves no graph per architecture): their tests then hold.
bool condition_holds(condition const& c, merged_options const& options, std::string_view os, std::string_view platform,
    std::string_view arch = {}, build_environment const* environment = nullptr);

// The system libraries of a product's `link.libraries` whose `when` holds on the target `os` (recipe-toml.md 11).
std::vector<std::string> link_on(product const& prod, merged_options const& options, std::string_view os);

// The option a condition tests, its own or that of its `not`; null when it tests none.
std::string const* tested_option(condition const& c) noexcept;

}
