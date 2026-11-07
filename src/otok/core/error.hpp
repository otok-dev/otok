// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <expected>
#include <string>
#include <utility>

namespace otok {

// A failure that is reported to the user. The message is complete on its own; callers that know
// more context (a file, a line, a dependency) add it when they report the error.
struct error
{
    std::string message;
};

template <typename T>
using result = std::expected<T, error>;

inline std::unexpected<error> fail(std::string message)
{
    return std::unexpected<error>{error{std::move(message)}};
}

}
