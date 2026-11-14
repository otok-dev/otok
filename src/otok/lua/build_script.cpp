// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/lua/build_script.hpp"

#include <algorithm>
#include <set>

// lua-c++: C++ linkage, no extern "C" (docs/PLAN.md decision 18)
#include <lauxlib.h>
#include <lua.h>

#include "otok/core/virtual_drives.hpp"
#include "otok/lua/library.hpp"
#include "otok/lua/state.hpp"

namespace otok {

namespace fs = std::filesystem;
using lua_detail::script_path;

namespace {

// ---- read-only tables (build-lua-api.md 3, 4)

int refuse_write(lua_State* L)
{
    return luaL_error(L, "attempt to modify a read-only table");
}

int proxy_len(lua_State* L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(lua_rawlen(L, lua_upvalueindex(1))));
    return 1;
}

int proxy_pairs(lua_State* L)
{
    lua_pushcfunction(L, [](lua_State* S) -> int {
        lua_settop(S, 2);
        return lua_next(S, 1) ? 2 : (lua_pushnil(S), 1);
    });
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushnil(L);
    return 3;
}

// Replaces the table on top of the stack with a read-only view of it. Tables inside it must already
// be views (the builders below freeze from the inside out).
void freeze(lua_State* L)
{
    int const table = lua_gettop(L);
    lua_newtable(L);                   // the view
    lua_createtable(L, 0, 5);          // its metatable
    lua_pushvalue(L, table);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, refuse_write);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, table);
    lua_pushcclosure(L, proxy_len, 1);
    lua_setfield(L, -2, "__len");
    lua_pushvalue(L, table);
    lua_pushcclosure(L, proxy_pairs, 1);
    lua_setfield(L, -2, "__pairs");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");  // getmetatable() cannot reach the real table
    lua_setmetatable(L, -2);
    lua_replace(L, table);
}

void set_string(lua_State* L, char const* key, std::string const& value)
{
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
}

void set_path(lua_State* L, char const* key, fs::path const& value)
{
    set_string(L, key, script_path(value));
}

void push_strings(lua_State* L, std::vector<std::string> const& values)
{
    lua_createtable(L, static_cast<int>(values.size()), 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        lua_pushlstring(L, values[i].data(), values[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    freeze(L);
}

void push_value(lua_State* L, option_data const& value)
{
    switch (value.index()) {
    case 0: lua_pushboolean(L, std::get<bool>(value)); break;
    case 1: lua_pushinteger(L, std::get<std::int64_t>(value)); break;
    case 2: lua_pushlstring(L, std::get<std::string>(value).data(), std::get<std::string>(value).size()); break;
    default: push_strings(L, std::get<std::vector<std::string>>(value)); break;
    }
}

template <typename Map, typename Push>
void push_map(lua_State* L, Map const& map, Push const& push)
{
    lua_createtable(L, 0, static_cast<int>(map.size()));
    for (auto const& [key, value] : map) {
        push(value);
        lua_setfield(L, -2, key.c_str());
    }
    freeze(L);
}

void push_values(lua_State* L, std::map<std::string, option_data> const& values)
{
    push_map(L, values, [L](option_data const& v) { push_value(L, v); });
}

void push_paths(lua_State* L, std::map<std::string, fs::path> const& paths)
{
    push_map(L, paths, [L](fs::path const& p) { lua_pushstring(L, script_path(p).c_str()); });
}

void push_dependency(lua_State* L, slice_dependency const& d, bool tool)
{
    lua_newtable(L);
    set_path(L, "prefix", d.prefix);
    set_path(L, "include", d.include);
    push_paths(L, d.libraries);
    lua_setfield(L, -2, "libraries");
    std::vector<std::string> dirs;
    for (fs::path const& dir : d.lib_dirs) dirs.push_back(script_path(dir));
    push_strings(L, dirs);
    lua_setfield(L, -2, "lib_dirs");
    if (tool) set_path(L, "bin", d.bin);
    set_string(L, "version", d.version);
    set_string(L, "linkage", d.linkage);
    push_values(L, d.options);
    lua_setfield(L, -2, "options");
    freeze(L);
}

void push_slice(lua_State* L, slice_context const& s)
{
    lua_newtable(L);
    for (auto const& [key, value] : s.target_fields) {
        if (!s.headers) set_string(L, key.c_str(), value);
    }
    for (auto const& [key, table] : s.target_tables) {
        if (s.headers) break;
        push_map(L, table, [L](std::string const& v) { lua_pushlstring(L, v.data(), v.size()); });
        lua_setfield(L, -2, key.c_str());
    }
    set_string(L, "name", s.name);
    set_string(L, "os", s.os);
    lua_pushboolean(L, s.headers);
    lua_setfield(L, -2, "headers");
    if (s.headers) {
        // build-lua-api.md 3.5: the paths, the programs of [toolchains] and the environment of processes only
        lua_pushboolean(L, s.is_tool);
        lua_setfield(L, -2, "is_tool");
        set_path(L, "source", s.source);
        set_path(L, "build", s.build);
        set_path(L, "prefix", s.prefix);
        set_path(L, "recipe", s.recipe);
        push_paths(L, s.resources);
        lua_setfield(L, -2, "resources");
        push_values(L, s.vars);
        lua_setfield(L, -2, "vars");
        lua_pushinteger(L, s.jobs);
        lua_setfield(L, -2, "jobs");
        lua_newtable(L);
        push_map(L, s.toolchain.env, [L](std::string const& v) { lua_pushlstring(L, v.data(), v.size()); });
        lua_setfield(L, -2, "env");
        freeze(L);
        lua_setfield(L, -2, "toolchain");
        push_paths(L, s.toolchains);
        lua_setfield(L, -2, "toolchains");
        freeze(L);
        return;
    }
    set_string(L, "platform", s.platform);
    set_string(L, "arch", s.arch);
    if (!s.environment.empty()) {
        set_string(L, "environment", s.environment);
        set_string(L, "host", s.host);
        if (!s.host_libc.empty()) set_string(L, "host_libc", s.host_libc);
        set_string(L, "compiler", s.compiler);
    }
    set_string(L, "triple", s.triple);
    set_string(L, "configuration", s.configuration);
    set_string(L, "linkage", s.linkage);
    if (s.c_standard) {
        lua_pushinteger(L, *s.c_standard);
        lua_setfield(L, -2, "c_standard");
    }
    if (s.cxx_standard) {
        lua_pushinteger(L, *s.cxx_standard);
        lua_setfield(L, -2, "cxx_standard");
    }
    lua_pushboolean(L, s.is_tool);
    lua_setfield(L, -2, "is_tool");
    set_path(L, "source", s.source);
    set_path(L, "build", s.build);
    set_path(L, "prefix", s.prefix);
    set_path(L, "recipe", s.recipe);
    push_paths(L, s.resources);
    lua_setfield(L, -2, "resources");
    push_values(L, s.vars);
    lua_setfield(L, -2, "vars");
    lua_pushinteger(L, s.jobs);
    lua_setfield(L, -2, "jobs");

    lua_newtable(L);
    set_string(L, "cc", s.toolchain.cc);
    set_string(L, "cxx", s.toolchain.cxx);
    set_string(L, "ar", s.toolchain.ar);
    set_string(L, "ranlib", s.toolchain.ranlib);
    set_string(L, "strip", s.toolchain.strip);
    set_string(L, "nm", s.toolchain.nm);
    set_string(L, "install_name_tool", s.toolchain.install_name_tool);
    set_string(L, "rpath", s.toolchain.rpath);
    push_strings(L, s.toolchain.cflags);
    lua_setfield(L, -2, "cflags");
    push_strings(L, s.toolchain.cxxflags);
    lua_setfield(L, -2, "cxxflags");
    push_strings(L, s.toolchain.ldflags);
    lua_setfield(L, -2, "ldflags");
    push_strings(L, s.toolchain.driver_flags);
    lua_setfield(L, -2, "driver_flags");
    push_map(L, s.toolchain.env, [L](std::string const& v) { lua_pushlstring(L, v.data(), v.size()); });
    lua_setfield(L, -2, "env");
    freeze(L);
    lua_setfield(L, -2, "toolchain");
    push_paths(L, s.toolchains);
    lua_setfield(L, -2, "toolchains");

    if (s.tools_prefix) {
        lua_newtable(L);
        set_path(L, "prefix", *s.tools_prefix);
        freeze(L);
        lua_setfield(L, -2, "tools");
    }
    freeze(L);
}

void push_deps(lua_State* L, slice_context const& s)
{
    lua_newtable(L);
    for (auto const& [name, d] : s.deps) {
        push_dependency(L, d, false);
        lua_setfield(L, -2, name.c_str());
    }
    lua_newtable(L);
    for (auto const& [name, d] : s.tool_deps) {
        push_dependency(L, d, true);
        lua_setfield(L, -2, name.c_str());
    }
    freeze(L);
    lua_setfield(L, -2, "tools");
    freeze(L);
}

// ---- the returned table (build-lua-api.md 8)

class output_reader
{
public:
    output_reader(lua_State* L, lua_detail::script_env const& env, std::string const& label) : L_{L}, env_{env}, label_{label} {}

    result<build_output> read()
    {
        if (!lua_istable(L_, -1)) return fail(label_ + ": build returned " + luaL_typename(L_, -1) + ", not a table");
        build_output out;
        int const table = lua_gettop(L_);
        lua_pushnil(L_);
        while (lua_next(L_, table) != 0) {
            std::string const key = lua_type(L_, -2) == LUA_TSTRING ? lua_tostring(L_, -2) : std::string{};
            result<void> ok;
            if (key == "libraries") {
                ok = libraries(out);
            } else if (key == "headers") {
                ok = path_or_paths(out.headers, out.library_headers, "headers");
            } else if (key == "module_map") {
                ok = path_or_paths(out.module_map, out.library_module_maps, "module_map");
            } else if (key == "data") {
                ok = paths(out.data, "data");
                for (auto const& [name, file] : out.data) {
                    bool const valid = !name.empty() && name.front() != '.' && std::ranges::all_of(name, [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                    });
                    if (ok && !valid) ok = fail(label_ + ": data." + name + ": a data file name is letters, digits, '.', '_' and '-', not starting with '.'");
                }
            } else if (key == "programs") {
                ok = programs(out);
                std::error_code ec;
                for (auto const& [name, file] : out.programs) {
                    if (ok && !fs::is_regular_file(file, ec)) ok = fail(label_ + ": programs." + name + " is not a file");
                }
            } else {
                ok = fail(label_ + ": build returned the unknown field " + (key.empty() ? std::string{"of a non-string key"} : "'" + key + "'"));
            }
            lua_pop(L_, 1);
            if (!ok) return std::unexpected{ok.error()};
        }
        // a tool recipe without products returns its programs only; the builder checks what its products need
        bool const tool = present_.contains("programs");
        if (!tool && !present_.contains("libraries")) return fail(label_ + ": build returned no 'libraries'");
        if (!tool && !present_.contains("headers")) return fail(label_ + ": build returned no 'headers'");
        return out;
    }

private:
    // A path inside slice.prefix that exists.
    result<fs::path> path(std::string const& what)
    {
        if (lua_type(L_, -1) != LUA_TSTRING) return fail(label_ + ": " + what + " must be a path, not " + luaL_typename(L_, -1));
        std::string const given = lua_tostring(L_, -1);
        auto p = lua_detail::confine(env_, given, lua_detail::access::read);
        if (!p) return fail(label_ + ": " + what + ": " + p.error().message);
        std::error_code ec;
        fs::path const prefix = virtual_drives::to_virtual(fs::weakly_canonical(env_.slice.prefix, ec));
        auto const [r, q] = std::mismatch(prefix.begin(), prefix.end(), p->begin(), p->end());
        if (r != prefix.end()) return fail(label_ + ": " + what + " '" + given + "' is not inside slice.prefix");
        if (!fs::exists(*p, ec)) return fail(label_ + ": " + what + " '" + given + "' does not exist");
        return *p;
    }

    // name → path
    result<void> paths(std::map<std::string, fs::path>& out, std::string const& what)
    {
        present_.insert(what);
        if (!lua_istable(L_, -1)) return fail(label_ + ": " + what + " must be a table of name = path, not " + luaL_typename(L_, -1));
        int const table = lua_gettop(L_);
        lua_pushnil(L_);
        while (lua_next(L_, table) != 0) {
            if (lua_type(L_, -2) != LUA_TSTRING) {
                lua_pop(L_, 2);
                return fail(label_ + ": " + what + " must be a table of name = path");
            }
            std::string const name = lua_tostring(L_, -2);
            auto p = path(what + "." + name);
            lua_pop(L_, 1);
            if (!p) {
                lua_pop(L_, 1);
                return std::unexpected{p.error()};
            }
            out.emplace(name, std::move(*p));
        }
        return {};
    }

    // name → path or { link, runtime, symbols }
    result<void> libraries(build_output& out)
    {
        present_.insert("libraries");
        if (!lua_istable(L_, -1)) return fail(label_ + ": libraries must be a table of name = path, not " + luaL_typename(L_, -1));
        int const table = lua_gettop(L_);
        lua_pushnil(L_);
        while (lua_next(L_, table) != 0) {
            if (lua_type(L_, -2) != LUA_TSTRING) {
                lua_pop(L_, 2);
                return fail(label_ + ": libraries must be a table of name = path");
            }
            std::string const name = lua_tostring(L_, -2);
            auto ok = library(out, name);
            lua_pop(L_, 1);
            if (!ok) {
                lua_pop(L_, 1);
                return ok;
            }
        }
        return {};
    }

    result<void> library(build_output& out, std::string const& name)
    {
        std::string const what = "libraries." + name;
        if (!lua_istable(L_, -1)) {
            auto p = path(what);
            if (!p) return std::unexpected{p.error()};
            out.libraries.emplace(name, std::move(*p));
            return {};
        }
        int const entry = lua_gettop(L_);
        lua_pushnil(L_);
        while (lua_next(L_, entry) != 0) {
            std::string const field = lua_type(L_, -2) == LUA_TSTRING ? lua_tostring(L_, -2) : std::string{};
            lua_pop(L_, 1);
            if (field != "link" && field != "runtime" && field != "symbols") {
                lua_pop(L_, 1);
                return fail(label_ + ": " + what + " has the unknown field " + (field.empty() ? std::string{"of a non-string key"} : "'" + field + "'")
                    + "; it takes link, runtime and symbols");
            }
        }
        for (auto [field, map] : {std::pair{"link", &out.libraries}, std::pair{"runtime", &out.runtimes}, std::pair{"symbols", &out.symbols}}) {
            lua_getfield(L_, entry, field);
            if (lua_isnil(L_, -1)) {
                lua_pop(L_, 1);
                if (std::string_view{field} == "link") return fail(label_ + ": " + what + " has no 'link'");
                continue;
            }
            auto p = path(what + "." + field);
            lua_pop(L_, 1);
            if (!p) return std::unexpected{p.error()};
            map->emplace(name, std::move(*p));
        }
        if (out.symbols.contains(name) && !out.runtimes.contains(name)) return fail(label_ + ": " + what + " has 'symbols' without 'runtime'");
        return {};
    }

    // name → path, or { script, interpreter }: a script the toolchain `interpreter` of the recipe runs (docs/PLAN.md decision 70)
    result<void> programs(build_output& out)
    {
        present_.insert("programs");
        if (!lua_istable(L_, -1)) return fail(label_ + ": programs must be a table of name = path, not " + luaL_typename(L_, -1));
        int const table = lua_gettop(L_);
        lua_pushnil(L_);
        while (lua_next(L_, table) != 0) {
            if (lua_type(L_, -2) != LUA_TSTRING) {
                lua_pop(L_, 2);
                return fail(label_ + ": programs must be a table of name = path");
            }
            std::string const name = lua_tostring(L_, -2);
            std::string const what = "programs." + name;
            result<void> ok;
            if (!lua_istable(L_, -1)) {
                auto p = path(what);
                if (p) out.programs.emplace(name, std::move(*p));
                else ok = std::unexpected{p.error()};
            } else {
                int const entry = lua_gettop(L_);
                lua_pushnil(L_);
                while (ok && lua_next(L_, entry) != 0) {
                    std::string const field = lua_type(L_, -2) == LUA_TSTRING ? lua_tostring(L_, -2) : std::string{};
                    if (field != "script" && field != "interpreter") {
                        ok = fail(label_ + ": " + what + " has the unknown field " + (field.empty() ? std::string{"of a non-string key"} : "'" + field + "'")
                            + "; it takes script and interpreter");
                        lua_pop(L_, 1);
                    }
                    lua_pop(L_, 1);
                }
                if (ok) {
                    lua_getfield(L_, entry, "interpreter");
                    if (lua_type(L_, -1) != LUA_TSTRING) ok = fail(label_ + ": " + what + ".interpreter must name a toolchain of the recipe");
                    else out.interpreters.emplace(name, lua_tostring(L_, -1));
                    lua_pop(L_, 1);
                }
                if (ok) {
                    lua_getfield(L_, entry, "script");
                    auto p = lua_isnil(L_, -1) ? result<fs::path>{fail(label_ + ": " + what + " has no 'script'")} : path(what + ".script");
                    lua_pop(L_, 1);
                    if (p) out.programs.emplace(name, std::move(*p));
                    else ok = std::unexpected{p.error()};
                }
            }
            lua_pop(L_, 1);
            if (!ok) {
                lua_pop(L_, 1);
                return ok;
            }
        }
        return {};
    }

    result<void> path_or_paths(std::optional<fs::path>& one, std::map<std::string, fs::path>& each, std::string const& what)
    {
        if (lua_istable(L_, -1)) return paths(each, what);
        present_.insert(what);
        auto p = path(what);
        if (!p) return std::unexpected{p.error()};
        one = std::move(*p);
        return {};
    }

    lua_State* L_;
    lua_detail::script_env const& env_;
    std::string const& label_;
    std::set<std::string> present_;
};

}

result<bool> script_defines(fs::path const& script, std::string const& label, std::string const& name)
{
    auto state = lua_state::create({});
    if (!state) return std::unexpected{state.error()};
    lua_State* L = state->get();
    lua_newtable(L);
    lua_setglobal(L, "otok");  // present, so that a top level reading otok.api does not fail
    if (auto ok = state->load_file(script, label); !ok) return std::unexpected{ok.error()};
    if (auto ok = state->call(0, 0); !ok) return std::unexpected{ok.error()};
    bool const defined = lua_getglobal(L, name.c_str()) == LUA_TFUNCTION;
    lua_pop(L, 1);
    return defined;
}

result<build_output> run_build_script(fs::path const& script, std::string const& label, build_entry entry, slice_context const& slice)
{
    lua_detail::script_env env{slice};
    auto state = lua_state::create([&env](std::string_view line) { env.log(line); });
    if (!state) return std::unexpected{state.error()};
    lua_State* L = state->get();
    lua_detail::open_otok_library(L, env);

    if (auto ok = state->load_file(script, label); !ok) return std::unexpected{ok.error()};
    if (auto ok = state->call(0, 0); !ok) return std::unexpected{ok.error()};

    char const* const function = entry == build_entry::build ? "build" : "build_tools";
    int const type = lua_getglobal(L, function);
    if (type != LUA_TFUNCTION) {
        lua_pop(L, 1);
        if (entry == build_entry::build_tools && type == LUA_TNIL) return build_output{};
        return fail(label + ": " + (type == LUA_TNIL ? std::string{"defines no function '"} + function + "'"
                                                       : std::string{"'"} + function + "' is not a function"));
    }
    push_slice(L, slice);
    push_values(L, slice.options);
    push_deps(L, slice);
    if (auto ok = state->call(3, 1); !ok) return std::unexpected{ok.error()};

    if (entry == build_entry::build_tools) {
        lua_pop(L, 1);
        build_output out;
        out.warnings = std::move(env.warnings);
        return out;
    }
    auto out = output_reader{L, env, label}.read();
    lua_pop(L, 1);
    if (!out) return out;
    out->warnings = std::move(env.warnings);
    return out;
}

}
