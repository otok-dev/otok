// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/commands/agent_skill.hpp"

namespace otok {

namespace {

struct embedded_file
{
    std::string_view path;
    std::string_view contents;
};

// embedded_files: written by cmake/embed.cmake from src/otok/commands/agents/
#include "otok_agent_skill.inc"

}

std::string_view agent_skill_text()
{
    for (embedded_file const& f : embedded_files) {
        if (f.path == "SKILL.md") return f.contents;
    }
    return {};
}

}
