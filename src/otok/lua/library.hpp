// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"
#include "otok/lua/build_script.hpp"

struct lua_State;

// The `otok` library of build.lua (build-lua-api.md 6), internal to the lua module.
namespace otok::lua_detail {

// What the library's functions share during one run of a script.
struct script_env
{
    slice_context const& slice;
    std::vector<std::filesystem::path> read_roots;   // canonical; the write roots are among them
    std::vector<std::filesystem::path> write_roots;
    std::vector<std::string> warnings;

    explicit script_env(slice_context const& s);

    // Appends a line to the slice's log.
    void log(std::string_view line) const;
};

enum class access
{
    read,
    write
};

// A path given by the script (absolute, or relative to slice.source) resolved through symbolic links
// and checked against the directories it may read or write (build-lua-api.md 6.3).
result<std::filesystem::path> confine(script_env const& env, std::string_view path, access a);

// A path as the script sees it: absolute, with `/`.
std::string script_path(std::filesystem::path const& p);

// Sets the global table `otok`.
void open_otok_library(lua_State* L, script_env& env);

// Raises a Lua error with the position of the calling Lua code ("build.lua:12: message").
[[noreturn]] void raise(lua_State* L, std::string const& message);

}
