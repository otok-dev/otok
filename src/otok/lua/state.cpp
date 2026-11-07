// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/lua/state.hpp"

// lua-c++: C++ linkage, no extern "C" (docs/PLAN.md decision 18)
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "otok/core/fs.hpp"

namespace otok {

namespace {

// Fixed seeds (docs/PLAN.md decision 18): table order and math.random repeat from run to run.
constexpr unsigned hash_seed = 0x6f746f6b;  // "otok"
constexpr lua_Integer random_seed = 1;

void emit_lines(lua_state::output const& out, std::string_view text)
{
    if (!out) return;
    std::size_t start = 0;
    for (std::size_t end; (end = text.find('\n', start)) != std::string_view::npos; start = end + 1) out(text.substr(start, end - start));
    out(text.substr(start));
}

}

struct lua_state::extra
{
    output out;
    std::string warning;  // a warning being assembled from its pieces
};

namespace {

lua_state::extra& extra_of(lua_State* L)
{
    return *static_cast<lua_state::extra*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// print(…): the arguments as tostring shows them, separated by tabs.
int print(lua_State* L)
{
    int const n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    std::size_t size = 0;
    char const* text = lua_tolstring(L, -1, &size);
    emit_lines(extra_of(L).out, {text, size});
    return 0;
}

// warn(…) reaches this in pieces; control messages ("@on", "@off") are ignored.
void warning(void* ud, char const* message, int to_continue)
{
    auto& e = *static_cast<lua_state::extra*>(ud);
    if (e.warning.empty() && !to_continue && message[0] == '@') return;
    e.warning += message;
    if (to_continue) return;
    emit_lines(e.out, "warning: " + e.warning);
    e.warning.clear();
}

// load(chunk, name, mode, env) with the mode forced to text: binary chunks bypass the compiler's
// checks and can crash the interpreter.
int load_text(lua_State* L)
{
    bool const has_env = lua_gettop(L) >= 4;
    lua_settop(L, 4);
    lua_pushvalue(L, lua_upvalueindex(1));  // the original load
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushliteral(L, "t");
    if (has_env) lua_pushvalue(L, 4);
    lua_call(L, has_env ? 4 : 3, LUA_MULTRET);
    return lua_gettop(L) - 4;
}

int message_handler(lua_State* L)
{
    char const* message = lua_tostring(L, 1);
    if (!message) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) {
            message = lua_tostring(L, -1);
        } else {
            message = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
        }
    }
    luaL_traceback(L, L, message, 1);
    return 1;
}

}

void lua_state::closer::operator()(lua_State* L) const noexcept
{
    lua_close(L);
}

lua_state::lua_state() = default;
lua_state::lua_state(lua_state&&) noexcept = default;
lua_state& lua_state::operator=(lua_state&&) noexcept = default;
lua_state::~lua_state() = default;

result<lua_state> lua_state::create(output out)
{
    lua_state s;
    s.extra_ = std::make_unique<extra>();
    s.extra_->out = std::move(out);
    s.state_.reset(lua_newstate(luaL_alloc, nullptr, hash_seed));
    lua_State* L = s.state_.get();
    if (!L) return fail("cannot create a Lua state: out of memory");
    lua_setwarnf(L, warning, s.extra_.get());
    luaL_openselectedlibs(L, LUA_GLIBK | LUA_COLIBK | LUA_MATHLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK, 0);

    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_getglobal(L, "load");
    lua_pushcclosure(L, load_text, 1);
    lua_setglobal(L, "load");
    lua_pushlightuserdata(L, s.extra_.get());
    lua_pushcclosure(L, print, 1);
    lua_setglobal(L, "print");

    lua_getglobal(L, "math");
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, random_seed);
    lua_call(L, 1, 0);
    lua_pop(L, 1);
    return s;
}

result<void> lua_state::load_file(std::filesystem::path const& file, std::string const& label) const
{
    auto text = read_file(file);
    if (!text) return std::unexpected{text.error()};
    lua_State* L = state_.get();
    std::string const name = "@" + label;
    if (luaL_loadbufferx(L, text->data(), text->size(), name.c_str(), "t") != LUA_OK) {
        std::string message = lua_tostring(L, -1);
        lua_pop(L, 1);
        return fail(std::move(message));
    }
    return {};
}

result<void> lua_state::call(int nargs, int nresults) const
{
    lua_State* L = state_.get();
    int const base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, message_handler);
    lua_insert(L, base);
    int const status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (status == LUA_OK) return {};
    std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(no error message)";
    lua_pop(L, 1);
    return fail(std::move(message));
}

}
