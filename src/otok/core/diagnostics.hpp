// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Messages about the user's files, with the place they refer to (docs/formats/README.md 1.3).
namespace otok {

// A place in a file. Lines and columns start at 1; 0 means unknown.
struct source_position
{
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

enum class severity
{
    error,
    warning
};

struct diagnostic
{
    severity level;
    source_position where;
    std::string message;
};

// `otok.toml:12:5: error: unknown key 'optins' in deps.openssl`
std::string format(diagnostic const& d);

// Collects every problem found in a pass, so the user sees them all at once instead of one per run.
class diagnostics
{
public:
    void error(source_position where, std::string message);
    void warning(source_position where, std::string message);

    bool has_errors() const noexcept { return error_count_ != 0; }
    std::size_t error_count() const noexcept { return error_count_; }
    std::span<diagnostic const> items() const noexcept { return items_; }

private:
    std::vector<diagnostic> items_;
    std::size_t error_count_ = 0;
};

}
