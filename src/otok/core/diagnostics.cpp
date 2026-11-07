// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/diagnostics.hpp"

namespace otok {

std::string format(diagnostic const& d)
{
    std::string text;
    if (!d.where.file.empty()) {
        text += d.where.file;
        if (d.where.line != 0) {
            text += ':' + std::to_string(d.where.line);
            if (d.where.column != 0) {
                text += ':' + std::to_string(d.where.column);
            }
        }
        text += ": ";
    }
    text += d.level == severity::error ? "error: " : "warning: ";
    text += d.message;
    return text;
}

void diagnostics::error(source_position where, std::string message)
{
    items_.push_back({severity::error, std::move(where), std::move(message)});
    ++error_count_;
}

void diagnostics::warning(source_position where, std::string message)
{
    items_.push_back({severity::warning, std::move(where), std::move(message)});
}

}
