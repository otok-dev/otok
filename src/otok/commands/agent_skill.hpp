// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <string_view>

namespace otok {

// The skill for coding agents (SKILL.md of the Agent Skills format) that `otok agent-skill` writes, from
// src/otok/commands/agents/SKILL.md.
std::string_view agent_skill_text();

}
