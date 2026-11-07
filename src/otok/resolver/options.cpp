// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/resolver/options.hpp"

#include <algorithm>
#include <type_traits>
#include <variant>

#include "otok/core/fs.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/process.hpp"

namespace otok {

namespace {

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

char const* type_name(option_type type) noexcept
{
    switch (type) {
    case option_type::boolean: return "a boolean";
    case option_type::integer: return "an integer";
    case option_type::string: return "a string";
    case option_type::enumeration: return "one of its values";
    case option_type::list: return "an array of strings";
    case option_type::file: return "a path of a file in the project";
    }
    return "?";
}

bool allowed(option_schema const& schema, std::string const& v)
{
    return schema.values.empty() || std::ranges::find(schema.values, v) != schema.values.end();
}

std::vector<std::string> canonical_list(option_schema const& schema, std::vector<std::string> list)
{
    std::ranges::sort(list);
    list.erase(std::ranges::unique(list).begin(), list.end());
    if (!schema.values.empty()) {
        auto const rank = [&](std::string const& v) { return std::ranges::find(schema.values, v) - schema.values.begin(); };
        std::ranges::sort(list, {}, rank);
    }
    return list;
}

std::string quoted_value_list(std::vector<std::string> const& values)
{
    std::string text;
    for (std::string const& v : values) text += (text.empty() ? "" : ", ") + in_quotes(v);
    return text;
}

}

result<option_data> check_option_value(option_schema const& schema, option_data const& value)
{
    std::string const subject = "option " + in_quotes(schema.name);
    bool const type_ok = [&] {
        switch (schema.type) {
        case option_type::boolean: return std::holds_alternative<bool>(value);
        case option_type::integer: return std::holds_alternative<std::int64_t>(value);
        case option_type::string:
        case option_type::enumeration: return std::holds_alternative<std::string>(value);
        case option_type::list: return std::holds_alternative<std::vector<std::string>>(value);
        case option_type::file: return std::holds_alternative<std::string>(value);
        }
        return false;
    }();
    if (!type_ok) {
        return fail(subject + " expects " + type_name(schema.type) + ", not " + describe(value));
    }
    if (schema.type == option_type::enumeration && !allowed(schema, std::get<std::string>(value))) {
        return fail(format_option_value(value) + " is not a value of " + subject + " (" + quoted_value_list(schema.values) + ")");
    }
    if (schema.type == option_type::list) {
        auto const& list = std::get<std::vector<std::string>>(value);
        for (std::string const& v : list) {
            if (!allowed(schema, v)) {
                return fail(in_quotes(v) + " is not a value of " + subject + " (" + quoted_value_list(schema.values) + ")");
            }
        }
        return option_data{canonical_list(schema, list)};
    }
    if (schema.type == option_type::file) {
        // otok-toml.md 6.2: relative to the manifest, inside the project; "" (no file) only as the default
        auto const& path = std::get<std::string>(value);
        if (classify_file_path(path) != file_path_kind::inside) {
            return fail(subject + " expects a path of a file inside the project, relative to otok.toml, not " + in_quotes(path));
        }
        return option_data{path_to_utf8(path_from_utf8(path).lexically_normal().generic_u8string())};
    }
    return value;
}

result<option_data> merge_option(std::string_view library, option_schema const& schema,
    std::span<option_request const> requests)
{
    if (requests.empty()) {
        if (schema.type == option_type::list) {
            return option_data{canonical_list(schema, std::get<std::vector<std::string>>(schema.default_value))};
        }
        return schema.default_value;
    }
    if (schema.additive) {
        if (schema.type == option_type::boolean) {
            bool const any = std::ranges::any_of(requests, [](option_request const& r) { return std::get<bool>(r.value); });
            return option_data{any};
        }
        std::vector<std::string> all;
        for (option_request const& r : requests) {
            auto const& list = std::get<std::vector<std::string>>(r.value);
            all.insert(all.end(), list.begin(), list.end());
        }
        return option_data{canonical_list(schema, std::move(all))};
    }
    bool const agree = std::ranges::all_of(requests, [&](option_request const& r) { return r.value == requests.front().value; });
    if (agree) return requests.front().value;

    std::string message = "conflicting values for option " + in_quotes(schema.name) + " of " + in_quotes(library)
        + " (not additive, so all requests must be equal):";
    for (option_request const& r : requests) {
        message += "\n  " + r.requester + " asks for " + format_option_value(r.value);
    }
    return fail(std::move(message));
}

std::string format_option_value(option_data const& value)
{
    return std::visit([](auto const& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return std::to_string(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return "\"" + v + "\"";
        } else {
            std::string text = "[";
            for (std::size_t i = 0; i < v.size(); ++i) text += (i ? ", \"" : "\"") + v[i] + "\"";
            return text + "]";
        }
    }, value);
}

bool condition_holds(condition const& c, merged_options const& options, std::string_view os, std::string_view platform,
    std::string_view arch, build_environment const* environment)
{
    if (c.negated && condition_holds(*c.negated, options, os, platform, arch, environment)) return false;
    if (c.arch && !arch.empty() && std::ranges::find(*c.arch, arch) == c.arch->end()) return false;
    if (c.environment && environment && !std::ranges::any_of(*c.environment, [&](build_environment const& e) { return covers(e, *environment); })) {
        return false;
    }
    if (c.os && std::ranges::find(*c.os, os) == c.os->end()) return false;
    if (c.platform && std::ranges::find(*c.platform, platform) == c.platform->end()) return false;
    if (c.host && std::ranges::find(*c.host, host_machine()) == c.host->end()) return false;
    if (c.option) {
        auto const it = options.find(*c.option);
        if (it == options.end()) return false;
        option_data const& value = it->second;
        if (c.has) {
            auto const* list = std::get_if<std::vector<std::string>>(&value);
            return list && std::ranges::find(*list, *c.has) != list->end();
        }
        if (c.is) return value == *c.is;
        if (auto const* file = std::get_if<std::string>(&value)) return !file->empty();  // a file option: a file is given
        auto const* flag = std::get_if<bool>(&value);
        return flag && *flag;
    }
    return true;
}

std::vector<std::string> link_on(product const& prod, merged_options const& options, std::string_view os)
{
    std::vector<std::string> out;
    for (product_link const& l : prod.link_libraries) {
        if (!l.when || condition_holds(*l.when, options, os, "")) out.push_back(l.library);
    }
    return out;
}

std::string const* tested_option(condition const& c) noexcept
{
    if (c.option) return &*c.option;
    return c.negated ? tested_option(*c.negated) : nullptr;
}

}
