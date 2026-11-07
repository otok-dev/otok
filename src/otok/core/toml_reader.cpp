// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/toml_reader.hpp"

#include <algorithm>
#include <utility>

namespace otok {

namespace {

source_position make_position(std::string const* file, toml::source_region const& region)
{
    return {file ? *file : std::string{}, region.begin.line, region.begin.column};
}

bool before(toml::source_region const& a, toml::source_region const& b) noexcept
{
    return std::pair{a.begin.line, a.begin.column} < std::pair{b.begin.line, b.begin.column};
}

// The keys of a table sorted by their position in the file: toml++ keeps them in name order, but
// messages and name lists should follow the file.
std::vector<toml::key const*> keys_in_file_order(toml::table const& table)
{
    std::vector<toml::key const*> keys;
    for (auto const& [key, value] : table) {
        keys.push_back(&key);
    }
    std::ranges::sort(keys, [](toml::key const* a, toml::key const* b) { return before(a->source(), b->source()); });
    return keys;
}

}

std::string_view describe_type(toml::node_type type) noexcept
{
    switch (type) {
    case toml::node_type::table:           return "a table";
    case toml::node_type::array:           return "an array";
    case toml::node_type::string:          return "a string";
    case toml::node_type::integer:         return "an integer";
    case toml::node_type::floating_point:  return "a float";
    case toml::node_type::boolean:         return "a boolean";
    case toml::node_type::date:            return "a date";
    case toml::node_type::time:            return "a time";
    case toml::node_type::date_time:       return "a date-time";
    case toml::node_type::none:            break;
    }
    return "nothing";
}

source_position toml_table::position() const
{
    return make_position(file_, table_->source());
}

source_position toml_table::position(std::string_view key) const
{
    if (toml::node const* value = table_->get(key)) {
        return position_of(*value);
    }
    return position();
}

source_position toml_table::position_of(toml::node const& n) const
{
    return make_position(file_, n.source());
}

bool toml_table::contains(std::string_view key) const
{
    return table_->contains(key);
}

std::string toml_table::describe(std::string_view key) const
{
    std::string text = "'" + std::string{key} + "'";
    text += path_.empty() ? " at the top level" : " in " + path_;
    return text;
}

std::string toml_table::child_path(std::string_view key) const
{
    return path_.empty() ? std::string{key} : path_ + "." + std::string{key};
}

toml::node const* toml_table::find(std::string_view key, need n)
{
    read_.emplace(key);
    toml::node const* value = table_->get(key);
    if (!value && n == need::required) {
        diags_->error(position(), "missing required key " + describe(key));
    }
    return value;
}

void toml_table::type_error(std::string_view key, toml::node const& value, std::string_view expected)
{
    diags_->error(position_of(value),
        describe(key) + " must be " + std::string{expected} + ", not " + std::string{describe_type(value.type())});
}

std::optional<std::string> toml_table::string(std::string_view key, need n)
{
    toml::node const* value = find(key, n);
    if (!value) return std::nullopt;
    if (auto const* s = value->as_string()) return s->get();
    type_error(key, *value, "a string");
    return std::nullopt;
}

std::optional<std::int64_t> toml_table::integer(std::string_view key, need n)
{
    toml::node const* value = find(key, n);
    if (!value) return std::nullopt;
    if (auto const* i = value->as_integer()) return i->get();
    type_error(key, *value, "an integer");
    return std::nullopt;
}

std::optional<bool> toml_table::boolean(std::string_view key, need n)
{
    toml::node const* value = find(key, n);
    if (!value) return std::nullopt;
    if (auto const* b = value->as_boolean()) return b->get();
    type_error(key, *value, "a boolean");
    return std::nullopt;
}

std::optional<std::vector<std::string>> toml_table::strings(std::string_view key, need n)
{
    toml::node const* value = find(key, n);
    if (!value) return std::nullopt;
    auto const* array = value->as_array();
    if (!array) {
        type_error(key, *value, "an array of strings");
        return std::nullopt;
    }
    std::vector<std::string> result;
    bool ok = true;
    for (std::size_t i = 0; i < array->size(); ++i) {
        toml::node const& element = (*array)[i];
        if (auto const* s = element.as_string()) {
            result.push_back(s->get());
        } else {
            diags_->error(position_of(element), describe(key) + " must be an array of strings; element "
                + std::to_string(i + 1) + " is " + std::string{describe_type(element.type())});
            ok = false;
        }
    }
    if (!ok) return std::nullopt;
    return result;
}

std::optional<toml_table> toml_table::table(std::string_view key, need n)
{
    toml::node const* value = find(key, n);
    if (!value) return std::nullopt;
    if (auto const* t = value->as_table()) return toml_table{*t, child_path(key), file_, diags_};
    type_error(key, *value, "a table");
    return std::nullopt;
}

std::optional<std::vector<toml_table>> toml_table::tables(std::string_view key, need n)
{
    toml::node const* value = find(key, n);
    if (!value) return std::nullopt;
    auto const* array = value->as_array();
    if (!array) {
        type_error(key, *value, "an array of tables");
        return std::nullopt;
    }
    std::vector<toml_table> result;
    bool ok = true;
    for (std::size_t i = 0; i < array->size(); ++i) {
        toml::node const& element = (*array)[i];
        if (auto const* t = element.as_table()) {
            result.push_back(toml_table{*t, child_path(key) + "[" + std::to_string(i + 1) + "]", file_, diags_});
        } else {
            diags_->error(position_of(element), describe(key) + " must be an array of tables; element "
                + std::to_string(i + 1) + " is " + std::string{describe_type(element.type())});
            ok = false;
        }
    }
    if (!ok) return std::nullopt;
    return result;
}

toml::node const* toml_table::node(std::string_view key, need n)
{
    return find(key, n);
}

toml_table toml_table::nested(toml::table const& table, std::string_view path) const
{
    return toml_table{table, child_path(path), file_, diags_};
}

std::vector<std::string> toml_table::keys()
{
    std::vector<std::string> names;
    for (toml::key const* key : keys_in_file_order(*table_)) {
        names.emplace_back(key->str());
        read_.emplace(key->str());
    }
    return names;
}

void toml_table::ignore(std::string_view key)
{
    read_.emplace(key);
}

std::vector<std::string> toml_table::unread_keys() const
{
    std::vector<std::string> names;
    for (toml::key const* key : keys_in_file_order(*table_)) {
        if (!read_.contains(key->str())) names.emplace_back(key->str());
    }
    return names;
}

void toml_table::ignore_rest()
{
    for (toml::key const* key : keys_in_file_order(*table_)) read_.emplace(key->str());
}

void toml_table::finish(severity unknown_keys)
{
    for (toml::key const* key : keys_in_file_order(*table_)) {
        if (!read_.contains(key->str())) {
            std::string message = "unknown key '" + std::string{key->str()} + "'" + (path_.empty() ? " at the top level" : " in " + path_);
            if (unknown_keys == severity::error) {
                diags_->error(make_position(file_, key->source()), std::move(message));
            } else {
                diags_->warning(make_position(file_, key->source()), std::move(message));
            }
        }
    }
}

std::optional<toml_document> toml_document::parse(std::string_view content, std::string file, diagnostics& diags)
{
    if (content.starts_with("\xEF\xBB\xBF")) {
        diags.error({file, 1, 1}, "the file starts with a UTF-8 byte order mark; save it as UTF-8 without BOM");
        return std::nullopt;
    }
    try {
        auto table = std::make_unique<toml::table>(toml::parse(content, std::string_view{file}));
        return toml_document{std::move(table), std::make_unique<std::string>(std::move(file)), &diags};
    } catch (toml::parse_error const& e) {
        diags.error({file, e.source().begin.line, e.source().begin.column}, std::string{e.description()});
        return std::nullopt;
    }
}

toml_table toml_document::root()
{
    return toml_table{*table_, std::string{}, file_.get(), diags_};
}

}
