// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "otok/core/diagnostics.hpp"

// Strict reading of otok's TOML files on top of toml++ (docs/formats/README.md 1.2, 1.3): every
// key a parser reads is marked, the rest are reported as unknown; wrong types and missing required
// keys are reported with their positions; reading goes on after an error, so one pass reports
// every problem of a file.
namespace otok {

enum class need
{
    optional,
    required
};

// A TOML table being read. Getters mark the key as read and return nothing when the key is absent
// or has the wrong type (the latter, and a missing required key, are reported). Move-only: the set
// of read keys belongs to one reader.
class toml_table
{
public:
    toml_table(toml_table&&) noexcept = default;
    toml_table& operator=(toml_table&&) noexcept = default;
    toml_table(toml_table const&) = delete;
    toml_table& operator=(toml_table const&) = delete;

    // "" for the top level, "deps.openssl", "package[2]" (1-based).
    std::string const& path() const noexcept { return path_; }

    // The position of the table, or of a key's value (the table's when the key is absent).
    source_position position() const;
    source_position position(std::string_view key) const;

    bool contains(std::string_view key) const;

    std::optional<std::string> string(std::string_view key, need n = need::optional);
    std::optional<std::int64_t> integer(std::string_view key, need n = need::optional);
    std::optional<bool> boolean(std::string_view key, need n = need::optional);
    std::optional<std::vector<std::string>> strings(std::string_view key, need n = need::optional);
    std::optional<toml_table> table(std::string_view key, need n = need::optional);
    // An array of tables: `[[package]]` or an array of inline tables.
    std::optional<std::vector<toml_table>> tables(std::string_view key, need n = need::optional);
    // A value of any type, for values whose type depends on context (option values, `recipe` that is
    // a string or a table). Marks the key as read.
    toml::node const* node(std::string_view key, need n = need::optional);

    // A reader for a table found inside a value of this one (an element of a mixed array); `path`
    // is appended to this table's path in messages: "defines[2]".
    toml_table nested(toml::table const& table, std::string_view path) const;

    // All keys of a table whose keys are names (`[deps]`, `[options]`), in file order; marks them read.
    std::vector<std::string> keys();
    // Marks a key as read without looking at it (`[metadata]`).
    void ignore(std::string_view key);
    // The keys not read so far, in file order.
    std::vector<std::string> unread_keys() const;
    // Marks every key as read: a file of an index that a newer otok may have given keys this one does not know
    // (formats README 1.5), checked strictly by index CI.
    void ignore_rest();

    // Reports every key that was never read. Call once, after reading everything. Unknown keys are
    // errors, except in files where a newer key must not break an older otok (config.toml).
    void finish(severity unknown_keys = severity::error);

    // For problems the parser finds in values ("unknown platform 'ipados'").
    void error(source_position where, std::string message) { diags_->error(std::move(where), std::move(message)); }
    void warning(source_position where, std::string message) { diags_->warning(std::move(where), std::move(message)); }
    // "'version' in deps.openssl", "'format' at the top level": how messages name a key.
    std::string describe(std::string_view key) const;

    // Position of any node of this document.
    source_position position_of(toml::node const& n) const;
    // The table itself, for values kept as written (`[publish.<target>]`).
    toml::table const& raw() const noexcept { return *table_; }

private:
    friend class toml_document;

    toml_table(toml::table const& table, std::string path, std::string const* file, diagnostics* diags)
        : table_{&table}, path_{std::move(path)}, file_{file}, diags_{diags} {}

    toml::node const* find(std::string_view key, need n);
    void type_error(std::string_view key, toml::node const& value, std::string_view expected);
    std::string child_path(std::string_view key) const;

    toml::table const* table_;
    std::string path_;
    std::string const* file_;
    diagnostics* diags_;
    std::set<std::string, std::less<>> read_;
};

// A parsed TOML file. Readers taken from it refer into it, so it must outlive them.
class toml_document
{
public:
    // `file` is the name used in messages. Reports a syntax error and returns nothing on failure.
    static std::optional<toml_document> parse(std::string_view content, std::string file, diagnostics& diags);

    toml_table root();
    // The parsed values, for code that works on them as data (index projections).
    toml::table const& table() const noexcept { return *table_; }

private:
    toml_document(std::unique_ptr<toml::table> table, std::unique_ptr<std::string> file, diagnostics* diags)
        : table_{std::move(table)}, file_{std::move(file)}, diags_{diags} {}

    std::unique_ptr<toml::table> table_;  // stable addresses: readers point into them
    std::unique_ptr<std::string> file_;
    diagnostics* diags_;
};

// "a string", "an integer", … for messages.
std::string_view describe_type(toml::node_type type) noexcept;

}
