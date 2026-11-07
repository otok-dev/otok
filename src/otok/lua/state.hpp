// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "otok/core/error.hpp"

struct lua_State;

// A Lua state for recipe scripts (build-lua-api.md 7): the standard library without `io`, `os`,
// `package`, `debug`, `dofile`, `loadfile` and binary chunks; deterministic hashing and random
// numbers (docs/PLAN.md decision 18). Lua is the C++ build: its errors unwind as exceptions.
namespace otok {

class lua_state
{
public:
    // Receives what the script prints (`print`, `warn`), one call per line without the newline.
    using output = std::function<void(std::string_view line)>;

    static result<lua_state> create(output out);

    lua_state(lua_state&&) noexcept;
    lua_state& operator=(lua_state&&) noexcept;
    ~lua_state();

    lua_State* get() const noexcept { return state_.get(); }

    struct extra;  // what the state's own functions reach through an upvalue (state.cpp)

    // Loads a text chunk from `file` and pushes it as a function; `label` names it in messages
    // ("build.lua:12: …").
    result<void> load_file(std::filesystem::path const& file, std::string const& label) const;

    // Calls the function below its `nargs` arguments on the stack (lua_pcall). A failure carries the
    // error message with a stack traceback; the stack then holds nothing of the call.
    result<void> call(int nargs, int nresults) const;

private:
    struct closer
    {
        void operator()(lua_State* L) const noexcept;
    };

    lua_state();
    std::unique_ptr<extra> extra_;                 // outlives the state, whose functions use it
    std::unique_ptr<lua_State, closer> state_;
};

}
