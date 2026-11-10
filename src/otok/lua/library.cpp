// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/lua/library.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <set>
#include <string_view>
#include <tuple>

// lua-c++: C++ linkage, no extern "C" (docs/PLAN.md decision 18)
#include <lauxlib.h>
#include <lua.h>

#include "otok/core/binary.hpp"
#include "otok/core/fs.hpp"

namespace otok::lua_detail {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr char path_list_separator = ';';
#else
constexpr char path_list_separator = ':';
#endif

bool within(fs::path const& p, fs::path const& root)
{
    auto const [r, q] = std::mismatch(root.begin(), root.end(), p.begin(), p.end());
    return r == root.end();
}

script_env& env_of(lua_State* L)
{
    return *static_cast<script_env*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// A library function: C++ exceptions (std::filesystem, std::bad_alloc) become Lua errors, which Lua
// itself would turn into an anonymous failure.
template <int (*F)(lua_State*)>
int guarded(lua_State* L)
{
    std::string message;
    try {
        return F(L);
    } catch (std::exception const& e) {
        message = e.what();
    }
    raise(L, message);
}

std::string string_value(lua_State* L, int idx, std::string const& what)
{
    int const type = lua_type(L, idx);
    if (type != LUA_TSTRING && type != LUA_TNUMBER) raise(L, what + " must be a string, not " + luaL_typename(L, idx));
    std::size_t size = 0;
    char const* s = lua_tolstring(L, idx, &size);
    return {s, size};
}

std::vector<std::string> string_array(lua_State* L, int idx, std::string const& what)
{
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) raise(L, what + " must be an array of strings, not " + luaL_typename(L, idx));
    std::vector<std::string> out;
    lua_Integer const n = luaL_len(L, idx);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, idx, i);
        out.push_back(string_value(L, -1, what + "[" + std::to_string(i) + "]"));
        lua_pop(L, 1);
    }
    return out;
}

// Checks that the table at `idx` has only the named keys (and, with `array`, an array part), so a
// misspelled option fails instead of being ignored.
void check_keys(lua_State* L, int idx, std::string const& function, std::initializer_list<std::string_view> names, bool array)
{
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1);
        if (lua_type(L, -1) == LUA_TSTRING) {
            std::string_view const key = lua_tostring(L, -1);
            if (std::ranges::find(names, key) == names.end()) {
                lua_pop(L, 1);
                raise(L, function + ": unknown option '" + std::string{key} + "'");
            }
        } else if (!array || !lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            raise(L, function + ": unexpected key of type " + luaL_typename(L, -1));
        }
    }
}

// The field `name` of the options table at `idx` pushed, or nothing pushed and false when absent.
bool push_field(lua_State* L, int idx, char const* name)
{
    if (lua_getfield(L, idx, name) != LUA_TNIL) return true;
    lua_pop(L, 1);
    return false;
}

std::optional<std::string> string_field(lua_State* L, int idx, char const* name, std::string const& function)
{
    if (!push_field(L, idx, name)) return std::nullopt;
    std::string value = string_value(L, -1, function + ": " + name);
    lua_pop(L, 1);
    return value;
}

std::vector<std::string> array_field(lua_State* L, int idx, char const* name, std::string const& function)
{
    if (!push_field(L, idx, name)) return {};
    auto value = string_array(L, -1, function + ": " + name);
    lua_pop(L, 1);
    return value;
}

std::optional<bool> bool_field(lua_State* L, int idx, char const* name, std::string const& function)
{
    if (!push_field(L, idx, name)) return std::nullopt;
    if (!lua_isboolean(L, -1)) raise(L, function + ": " + name + " must be a boolean, not " + luaL_typename(L, -1));
    bool const value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

std::optional<lua_Integer> integer_field(lua_State* L, int idx, char const* name, std::string const& function)
{
    if (!push_field(L, idx, name)) return std::nullopt;
    if (!lua_isinteger(L, -1)) raise(L, function + ": " + name + " must be an integer, not " + luaL_typename(L, -1));
    lua_Integer const value = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return value;
}

fs::path confined(lua_State* L, std::string_view path, access a)
{
    auto p = confine(env_of(L), path, a);
    if (!p) raise(L, p.error().message);
    return *p;
}

// The options every process-starting function takes: `cwd` and `env` (build-lua-api.md 5, 6.1).
struct process_options
{
    fs::path cwd;
    env_vars env;
    fs::path jobserver_fifo{};  // process_spec::jobserver_fifo
};

process_options read_process_options(lua_State* L, int idx, std::string const& function)
{
    script_env const& e = env_of(L);
    process_options o{e.slice.source, e.slice.toolchain.env};
    if (idx == 0) return o;
    if (auto cwd = string_field(L, idx, "cwd", function)) o.cwd = confined(L, *cwd, access::read);
    if (push_field(L, idx, "env")) {
        if (!lua_istable(L, -1)) raise(L, function + ": env must be a table, not " + luaL_typename(L, -1));
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) raise(L, function + ": env keys must be strings");
            std::string const name = lua_tostring(L, -2);
            if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
                o.env.erase(name);
            } else {
                o.env[name] = string_value(L, -1, function + ": env." + name);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return o;
}

// In the arguments of a command, the number of jobs of a tool without the jobserver: what the slice may run when
// the command starts (docs/PLAN.md decision 61).
constexpr std::string_view jobs_placeholder = "\x01jobs\x01";

std::string command_line(std::vector<std::string> const& argv)
{
    std::string text;
    for (std::string const& a : argv) {
        bool const plain = !a.empty() && a.find_first_of(" \t\"'") == std::string::npos;
        text += (text.empty() ? "" : " ") + (plain ? a : "'" + a + "'");
    }
    return text;
}

// The stage a command shows (docs/PLAN.md decision 61): the program's name, with the script an interpreter runs, or the
// first target of make and its like: "perl Configure", "make install_sw".
std::string command_stage(std::vector<std::string> const& argv)
{
    auto const name_of = [](std::string const& path) {
        std::string name = path_to_utf8(path_from_utf8(path).filename());
        for (std::string_view const ext : {".exe", ".bat", ".cmd"}) {
            if (name.size() > ext.size() && std::ranges::equal(std::string_view{name}.substr(name.size() - ext.size()), ext,
                    [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; })) {
                name.resize(name.size() - ext.size());
                break;
            }
        }
        return name;
    };
    std::string const program = name_of(argv.front());
    std::string lower = program;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static std::set<std::string, std::less<>> const interpreters{"bash", "lua", "node", "perl", "python", "python3", "ruby", "sh"};
    static std::set<std::string, std::less<>> const makes{"b2", "gmake", "jom", "make", "mingw32-make", "ninja", "nmake"};
    // their options that take the next argument
    static std::set<std::string, std::less<>> const with_value{"-C", "-I", "-W", "-d", "-f", "-j", "-k", "-l", "-o", "-t"};
    if (interpreters.contains(lower)) {
        if (argv.size() > 1 && !argv[1].starts_with('-')) return program + " " + name_of(argv[1]);
    } else if (makes.contains(lower)) {
        for (std::size_t i = 1; i < argv.size(); ++i) {
            std::string const& a = argv[i];
            if (with_value.contains(a)) {
                ++i;
            } else if (!a.empty() && a.front() != '-' && a.front() != '/' && a.find_first_of("=/\\") == std::string::npos
                && !std::ranges::all_of(a, [](unsigned char c) { return std::isdigit(c) != 0; })) {
                return program + " " + a;
            }
        }
    }
    return program;
}

// Runs one process for the script; failures (with `check`) are Lua errors. `stage`: what the display shows while it runs;
// empty: command_stage.
process_exit run_command(lua_State* L, std::vector<std::string> argv, process_options const& o, bool check, bool capture,
    std::string_view stage = {})
{
    script_env const& e = env_of(L);
    if (argv.empty()) raise(L, "otok.run: the command is empty");
    // the jobs of a tool without the jobserver, held while it runs
    std::function<void()> put_back;
    if (std::ranges::any_of(argv, [](std::string const& a) { return a.find(jobs_placeholder) != std::string::npos; })) {
        std::int64_t jobs = e.slice.jobs;
        if (e.slice.hold_jobs) std::tie(jobs, put_back) = e.slice.hold_jobs(e.slice.jobs);
        for (std::string& a : argv) {
            for (std::size_t at; (at = a.find(jobs_placeholder)) != std::string::npos;) a.replace(at, jobs_placeholder.size(), std::to_string(jobs));
        }
    }
    e.log("$ " + command_line(argv));
    if (e.slice.stage) e.slice.stage(stage.empty() ? command_stage(argv) : std::string{stage});
    if (e.slice.echo) e.slice.echo(command_line(argv), o.cwd);
    process_spec spec{std::move(argv), o.env, o.cwd, e.slice.log, capture};
    spec.jobserver_fifo = o.jobserver_fifo;
    auto exit = e.slice.run(spec);
    if (put_back) put_back();
    if (!exit) raise(L, exit.error().message);
    if (check && !exit->success()) {
        raise(L, "'" + spec.argv.front() + "' failed with " + describe_exit(*exit) + ": " + command_line(spec.argv));
    }
    if (exit->output_truncated) e.log("(the captured output was cut at " + std::to_string(spec.capture_limit) + " bytes)");
    return std::move(*exit);
}

// Parallelism of a build tool (build-lua-api.md 6.2): the jobserver of MAKEFLAGS when `tool` ("make",
// "ninja"; empty: none) understands it, through inherited descriptors for an older make that otok runs itself
// (`direct`); otherwise no MAKEFLAGS and a fixed number, `jobs` when asked, else the slice's share.
std::vector<std::string> parallel_flags(script_env const& e, process_options& o, std::optional<lua_Integer> jobs, std::string const& tool,
    std::string const& flag, bool direct = false)
{
    auto const makeflags = o.env.find("MAKEFLAGS");
    if (!jobs && !tool.empty() && makeflags != o.env.end() && e.slice.jobserver_client) {
        jobserver_protocol const protocol = e.slice.jobserver_client(tool, o.env);
        if (protocol == jobserver_protocol::named) return {};
        if (direct && protocol != jobserver_protocol::none) {
            if (auto inherited = inherit_jobserver(makeflags->second, protocol)) {
                makeflags->second = std::move(inherited->makeflags);
                o.jobserver_fifo = std::move(inherited->fifo);
                return {};
            }
        }
    }
    o.env.erase("MAKEFLAGS");
    // a number the recipe gives stays; otherwise as many as the slice may run when the command starts
    return {flag, jobs ? std::to_string(*jobs) : std::string{jobs_placeholder}};
}

// Removes the variables the slice gives (build-lua-api.md 5) from a process environment, unless the
// call changed them: for build systems that take them from a file instead.
void drop_given(script_env const& e, process_options& o, std::initializer_list<char const*> names)
{
    for (char const* name : names) {
        auto const given = e.slice.toolchain.env.find(name);
        auto const now = o.env.find(name);
        if (now != o.env.end() && given != e.slice.toolchain.env.end() && now->second == given->second) o.env.erase(now);
    }
}

// The program of a toolchain a helper runs (build-lua-api.md 6.2): one the recipe declares, as otok chose it.
std::string toolchain_program(lua_State* L, std::string const& function, std::string const& name)
{
    script_env const& e = env_of(L);
    auto const it = e.slice.toolchains.find(name);
    if (it == e.slice.toolchains.end()) raise(L, function + ": '" + name + "' is not in [toolchains] of the recipe");
    return script_path(it->second);
}

std::string build_type(script_env const& e)
{
    return e.slice.configuration == "debug" ? "Debug" : "Release";
}

// ---- running programs (6.1)

int otok_run(lua_State* L)
{
    std::vector<std::string> argv = string_array(L, 1, "otok.run: the command");
    bool const has_options = !lua_isnoneornil(L, 2);
    if (has_options) {
        luaL_checktype(L, 2, LUA_TTABLE);
        check_keys(L, 2, "otok.run", {"cwd", "env", "check", "capture"}, false);
    }
    process_options const o = read_process_options(L, has_options ? 2 : 0, "otok.run");
    bool const check = has_options ? bool_field(L, 2, "check", "otok.run").value_or(true) : true;
    bool const capture = has_options && bool_field(L, 2, "capture", "otok.run").value_or(false);
    process_exit const exit = run_command(L, std::move(argv), o, check, capture);
    if (capture) {
        lua_pushlstring(L, exit.output.data(), exit.output.size());
        lua_pushinteger(L, exit.code);
        return 2;
    }
    lua_pushinteger(L, exit.signal == 0 ? exit.code : 128 + exit.signal);
    return 1;
}

// ---- build-system helpers (6.2)

#ifdef _WIN32
// A path as the programs of MSYS2 name it: C:/x is /c/x; others unchanged.
std::string msys_path(std::string path)
{
    if (path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' && path[2] == '/') {
        path = "/" + std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])))) + path.substr(2);
    }
    return path;
}

// Puts the directory of MSYS2's programs into PATH before the directories of Windows: after the toolchain's (MSVC's
// link.exe, not the link of coreutils), before System32 (whose bash.exe, WSL's, programs such as ICU's pkgdata would
// otherwise run).
void msys_before_windows(env_vars& env, fs::path const& dir)
{
    auto const lower = [](std::string s) {
        for (char& c : s) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (s.ends_with('/')) s.pop_back();
        return s;
    };
    auto path = env.end();
    std::string system_root = "c:/windows";
    for (auto v = env.begin(); v != env.end(); ++v) {
        if (lower(v->first) == "path") path = v;
        if (lower(v->first) == "systemroot" && !v->second.empty()) system_root = lower(v->second);
    }
    if (path == env.end()) path = env.emplace("PATH", "").first;
    std::string const added = path_to_utf8(dir.native());
    std::vector<std::string> dirs;
    for (std::size_t start = 0; start <= path->second.size();) {
        std::size_t const end = std::min(path->second.find(';', start), path->second.size());
        std::string const d = path->second.substr(start, end - start);
        if (!d.empty() && lower(d) != lower(added)) dirs.push_back(d);
        start = end + 1;
    }
    std::size_t windows = dirs.size();
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        if (lower(dirs[i]) == system_root || lower(dirs[i]).starts_with(system_root + "/")) {
            windows = i;
            break;
        }
    }
    dirs.insert(dirs.begin() + static_cast<std::ptrdiff_t>(windows), added);
    std::string joined;
    for (std::string const& d : dirs) joined += (joined.empty() ? "" : ";") + d;
    path->second = joined;
}
#endif

// The compilers of otok.make and otok.autotools (build-lua-api.md 6.2): CC and CXX followed by the driver flags, which
// libtool does not pass on to the links of shared libraries, nor a makefile linking with $(CC) alone; nothing when the
// call changes or removes CC or CXX. `once`: the flags without them, so every command has them once (otok.autotools,
// whose configure puts both into the makefiles; a makefile of otok.make may have its own CC and take CFLAGS alone).
void put_driver_flags(script_env const& e, process_options& o, bool once)
{
    slice_toolchain const& t = e.slice.toolchain;
    if (t.driver_flags.empty()) return;
    auto const given = [&](std::string const& name) {
        auto const g = t.env.find(name);
        auto const now = o.env.find(name);
        return g != t.env.end() && now != o.env.end() && g->second == now->second;
    };
    if (!given("CC") || !given("CXX")) return;
    std::string driver;
    for (std::string const& f : t.driver_flags) driver += (driver.empty() ? "" : " ") + f;
    o.env["CC"] += " " + driver;
    o.env["CXX"] += " " + driver;
    if (!once) return;
    for (std::string const name : {"CFLAGS", "CXXFLAGS", "LDFLAGS"}) {
        if (!given(name)) continue;
        std::string& value = o.env[name];
        if (!value.starts_with(driver) || (value.size() > driver.size() && value[driver.size()] != ' ')) continue;
        value.erase(0, std::min(value.size(), driver.size() + 1));
        if (value.empty()) o.env.erase(name);
    }
}

int otok_make(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    check_keys(L, 1, "otok.make", {"args", "jobs", "cwd", "env"}, true);
    script_env const& e = env_of(L);
    std::string const make = toolchain_program(L, "otok.make", "make");
    process_options o = read_process_options(L, 1, "otok.make");
    put_driver_flags(e, o, false);
#ifdef _WIN32
    // the make of MSYS2 runs its rules with the programs beside it (otok.autotools)
    if (fs::path const dir = path_from_utf8(make).parent_path(); fs::is_regular_file(dir / "sh.exe")) msys_before_windows(o.env, dir);
#endif
    std::vector<std::string> argv{make};
    std::ranges::copy(parallel_flags(e, o, integer_field(L, 1, "jobs", "otok.make"), make, "-j", true), std::back_inserter(argv));
    std::ranges::copy(array_field(L, 1, "args", "otok.make"), std::back_inserter(argv));
    std::ranges::copy(string_array(L, 1, "otok.make: targets"), std::back_inserter(argv));
    run_command(L, std::move(argv), o, true, false);
    return 0;
}

int otok_cmake(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string const fn = "otok.cmake";
    check_keys(L, 1, fn, {"source", "build", "defines", "args", "generator", "targets", "install", "cwd", "env"}, false);
    script_env const& e = env_of(L);
    std::string const generator = string_field(L, 1, "generator", fn).value_or("Ninja");
    std::string const cmake = toolchain_program(L, fn, "cmake");
    // the tool of the generator, given to CMake as the one to run
    std::string const tool = generator == "Ninja" ? toolchain_program(L, fn, "ninja") : generator == "Unix Makefiles" ? toolchain_program(L, fn, "make") : "";
    process_options o = read_process_options(L, 1, fn);
    // the toolchain file carries the flags; CMake would add these a second time (build-lua-api.md 6.2)
    if (!e.slice.cmake_toolchain.empty()) drop_given(e, o, {"CFLAGS", "CXXFLAGS", "LDFLAGS"});
    auto const source = string_field(L, 1, "source", fn);
    auto const build = string_field(L, 1, "build", fn);
    std::string const build_dir = script_path(build ? confined(L, *build, access::write) : e.slice.build);
    std::string const type = build_type(e);

    std::vector<std::string> configure{cmake, "-S", script_path(source ? confined(L, *source, access::read) : e.slice.source), "-B",
        build_dir, "-G", generator};
    if (!e.slice.cmake_toolchain.empty()) configure.push_back("-DCMAKE_TOOLCHAIN_FILE=" + script_path(e.slice.cmake_toolchain));
    if (!tool.empty()) configure.push_back("-DCMAKE_MAKE_PROGRAM=" + tool);
    configure.push_back("-DCMAKE_INSTALL_PREFIX=" + script_path(e.slice.prefix));
    configure.push_back("-DCMAKE_BUILD_TYPE=" + type);
    configure.push_back(std::string{"-DBUILD_SHARED_LIBS="} + (e.slice.linkage == "dynamic" ? "ON" : "OFF"));
    // the project's standards (otok-toml.md 5.1): CMake then adds none of its own below them; C89 is CMake's 90
    if (e.slice.c_standard) {
        configure.push_back("-DCMAKE_C_STANDARD=" + std::to_string(*e.slice.c_standard == 89 ? 90 : *e.slice.c_standard));
        configure.push_back("-DCMAKE_C_STANDARD_REQUIRED=ON");
        configure.push_back("-DCMAKE_C_EXTENSIONS=OFF");
    }
    if (e.slice.cxx_standard) {
        configure.push_back("-DCMAKE_CXX_STANDARD=" + std::to_string(*e.slice.cxx_standard));
        configure.push_back("-DCMAKE_CXX_STANDARD_REQUIRED=ON");
        configure.push_back("-DCMAKE_CXX_EXTENSIONS=OFF");
    }
    // the libraries find each other beside themselves (build-lua-api.md 6.2)
    if (!e.slice.toolchain.rpath.empty()) configure.push_back("-DCMAKE_INSTALL_RPATH=" + e.slice.toolchain.rpath);
    if (!e.slice.deps.empty()) {
        std::string prefixes;
        for (auto const& [name, d] : e.slice.deps) prefixes += (prefixes.empty() ? "" : ";") + script_path(d.prefix);
        configure.push_back("-DCMAKE_PREFIX_PATH=" + prefixes);
        configure.push_back("-DCMAKE_FIND_ROOT_PATH=" + prefixes);
    }
    if (push_field(L, 1, "defines")) {
        if (!lua_istable(L, -1)) raise(L, fn + ": defines must be a table, not " + luaL_typename(L, -1));
        std::map<std::string, std::string> defines;  // sorted: the same command line on every run
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) raise(L, fn + ": defines keys must be strings");
            std::string const name = lua_tostring(L, -2);
            defines[name] = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? "ON" : "OFF") : string_value(L, -1, fn + ": defines." + name);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        for (auto const& [name, value] : defines) configure.push_back("-D" + name + "=" + value);
    }
    std::ranges::copy(array_field(L, 1, "args", fn), std::back_inserter(configure));
    // last, so neither defines nor args turn it off: FetchContent downloads nothing (build-lua-api.md 2, 6.2)
    configure.push_back("-DFETCHCONTENT_FULLY_DISCONNECTED=ON");
    run_command(L, std::move(configure), o, true, false, "configure");

    std::vector<std::string> build_command{cmake, "--build", build_dir, "--config", type};
    for (std::string& t : array_field(L, 1, "targets", fn)) {
        build_command.push_back("--target");
        build_command.push_back(std::move(t));
    }
    std::ranges::copy(parallel_flags(e, o, std::nullopt, tool, "--parallel"), std::back_inserter(build_command));
    run_command(L, std::move(build_command), o, true, false, "build");

    if (bool_field(L, 1, "install", fn).value_or(true)) run_command(L, {cmake, "--install", build_dir, "--config", type}, o, true, false, "install");
    return 0;
}

int otok_autotools(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string const fn = "otok.autotools";
    check_keys(L, 1, fn, {"configure", "args", "autoreconf", "targets", "cwd", "env"}, false);
    script_env const& e = env_of(L);
    process_options o = read_process_options(L, 1, fn);
    put_driver_flags(e, o, true);
    std::string const make = toolchain_program(L, fn, "make");
    bool const autoreconf = bool_field(L, 1, "autoreconf", fn).value_or(false);
    std::string autoreconf_program;
    if (autoreconf) {
        // autoreconf runs the chosen tools and their companions (build-lua-api.md 6.2)
        fs::path const autoconf = path_from_utf8(toolchain_program(L, fn, "autoconf"));
        fs::path const automake = path_from_utf8(toolchain_program(L, fn, "automake"));
        fs::path const libtool = path_from_utf8(toolchain_program(L, fn, "libtool"));
        std::string const libtoolize = path_to_utf8(libtool.stem()).starts_with("g") ? "glibtoolize" : "libtoolize";
        autoreconf_program = script_path(autoconf.parent_path() / "autoreconf");
        for (auto const& [name, program] : {std::pair{"AUTOCONF", autoconf}, std::pair{"AUTOHEADER", autoconf.parent_path() / "autoheader"},
                 std::pair{"AUTOMAKE", automake}, std::pair{"ACLOCAL", automake.parent_path() / "aclocal"},
                 std::pair{"LIBTOOLIZE", libtool.parent_path() / libtoolize}}) {
            if (!o.env.contains(name)) o.env[name] = script_path(program);
        }
    }

    // dependencies made findable (build-lua-api.md 6.2)
    std::string cppflags, ldflags, pkg_config;
    for (auto const& [name, d] : e.slice.deps) {
        cppflags += " -I" + script_path(d.include);
        for (fs::path const& dir : d.lib_dirs) ldflags += " -L" + script_path(dir);
        pkg_config += (pkg_config.empty() ? "" : std::string(1, path_list_separator)) + script_path(d.prefix / "lib" / "pkgconfig");
    }
    auto const append = [&](char const* name, std::string const& extra, char separator) {
        if (extra.empty()) return;
        std::string& value = o.env[name];
        value = value.empty() ? extra : value + separator + extra;
    };
    append("CPPFLAGS", cppflags.empty() ? cppflags : cppflags.substr(1), ' ');
    // the $ escaped once for the shell of configure and once for make (build-lua-api.md 6.2)
    if (!e.slice.toolchain.rpath.empty()) {
        std::string rpath;
        for (char const c : e.slice.toolchain.rpath) rpath += c == '$' ? std::string{"\\$$"} : std::string(1, c);
        ldflags += " -Wl,-rpath," + rpath;
    }
    append("LDFLAGS", ldflags.empty() ? ldflags : ldflags.substr(1), ' ');
    append("PKG_CONFIG_PATH", pkg_config, path_list_separator);

    if (autoreconf) run_command(L, {autoreconf_program, "-fi"}, o, true, false);
    std::vector<std::string> configure;
    std::string script = string_field(L, 1, "configure", fn).value_or("./configure");
    std::string prefix = script_path(e.slice.prefix);
#ifdef _WIN32
    // Windows runs no shell script: configure goes through the sh beside the make of MSYS2, whose directory is in
    // PATH with the tools configure runs (build-lua-api.md 6.2); the script and the prefix as MSYS2 names them,
    // /c/…: autoconf takes the ':' of C:/… for a separator of its search lists, and mkinstalldirs for a directory
    if (fs::path const sh = path_from_utf8(make).parent_path() / "sh.exe"; fs::is_regular_file(sh)) {
        configure.push_back(script_path(sh));
        msys_before_windows(o.env, sh.parent_path());
        script = msys_path(script);
        prefix = msys_path(prefix);
    }
#endif
    configure.insert(configure.end(), {script, "--prefix=" + prefix,
        e.slice.linkage == "dynamic" ? "--enable-shared" : "--disable-shared", e.slice.linkage == "dynamic" ? "--disable-static" : "--enable-static"});
    if (!e.slice.autotools_host.empty()) configure.push_back("--host=" + e.slice.autotools_host);
    if (!e.slice.autotools_build.empty()) configure.push_back("--build=" + e.slice.autotools_build);
    std::ranges::copy(array_field(L, 1, "args", fn), std::back_inserter(configure));
    run_command(L, std::move(configure), o, true, false);

    std::vector<std::string> build{make};
    std::ranges::copy(parallel_flags(e, o, std::nullopt, make, "-j", true), std::back_inserter(build));
    lua_getfield(L, 1, "targets");
    bool const has_targets = !lua_isnil(L, -1);
    lua_pop(L, 1);
    std::ranges::copy(has_targets ? array_field(L, 1, "targets", fn) : std::vector<std::string>{"install"}, std::back_inserter(build));
    run_command(L, std::move(build), o, true, false);
    return 0;
}

// A string of a Jam file, quoted.
std::string jam_string(std::string_view s)
{
    std::string out = "\"";
    for (char const c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

int otok_b2(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string const fn = "otok.b2";
    check_keys(L, 1, fn, {"b2", "args", "targets", "cwd", "env"}, false);
    script_env const& e = env_of(L);
    if (e.slice.b2_toolset.empty()) raise(L, fn + ": the target of this slice cannot build with b2");
    auto const program = string_field(L, 1, "b2", fn);
    if (!program) raise(L, fn + ": b2, the path of the program, is required");
    process_options o = read_process_options(L, 1, fn);

    // the slice's toolchain (build-lua-api.md 6.2)
    std::string const& toolset = e.slice.b2_toolset;
    slice_toolchain const& t = e.slice.toolchain;
    // b2 has no jobserver: its compilers run through otok's launcher, which waits for a token, so it
    // can have all of `jobs` without the slices oversubscribing the machine together. msvc.jam finds
    // link.exe beside the compiler's command, so it runs MSVC itself.
    std::string auth;
    if (auto const flags = o.env.find("MAKEFLAGS"); flags != o.env.end()) {
        if (auto const at = flags->second.find("--jobserver-auth="); at != std::string::npos) {
            std::size_t const start = at + std::string_view{"--jobserver-auth="}.size();
            auth = flags->second.substr(start, flags->second.find(' ', start) - start);
        }
    }
    std::string command = jam_string(t.cxx);
    if (!auth.empty() && !e.slice.launcher.empty() && toolset != "msvc") {
        command = jam_string(script_path(e.slice.launcher)) + " \"jobserver-exec\" \"--slot\" " + jam_string(script_path(e.slice.build / "otok-job.lock"))
            + " \"--auth\" " + jam_string(auth) + " \"--\" " + command;
    }
    std::string config = "# Generated by otok for the slice " + e.slice.name + " (" + e.slice.triple + ").\nusing " + toolset + " : " + e.slice.b2_version + " : "
        + command + "\n    :";
    auto const add = [&](char const* feature, std::vector<std::string> const& flags) {
        for (std::string const& f : flags) config += std::string{" <"} + feature + ">" + jam_string(f);
    };
    add("cflags", t.cflags);
    add("asmflags", t.cflags);  // Boost.Context's assembly is compiled for the slice too
    add("cxxflags", t.cxxflags);
    add("linkflags", t.ldflags);
    if (toolset == "gcc" || toolset == "clang") {
        if (!t.ar.empty()) config += " <archiver>" + jam_string(t.ar);
        if (!t.ranlib.empty()) config += " <ranlib>" + jam_string(t.ranlib);
    }
    if (toolset == "darwin") config += " <striper>";  // the symbols stay until the app is linked
    config += "\n    ;\n";
    fs::path const file = e.slice.build / "otok-user-config.jam";
    if (auto ok = write_file_atomic(file, config); !ok) raise(L, fn + ": " + ok.error().message);

    // b2 has no jobserver: "-j8"; with the launcher, which takes the tokens of the slice's jobserver, all of slice.jobs,
    // else the jobs the slice may hold (docs/PLAN.md decision 61)
    std::string jobs;
    for (std::string const& f : parallel_flags(e, o, std::nullopt, "", "-j")) jobs += f;
    if (command.find("jobserver-exec") != std::string::npos) jobs = "-j" + std::to_string(e.slice.jobs);
    std::vector<std::string> argv{*program, "--user-config=" + script_path(file), "--ignore-site-config", jobs,
        "--build-dir=" + script_path(e.slice.build / "b2"), "--prefix=" + script_path(e.slice.prefix), "--stagedir=" + script_path(e.slice.prefix),
        "toolset=" + toolset + "-" + e.slice.b2_version, std::string{"variant="} + (e.slice.configuration == "debug" ? "debug" : "release"),
        std::string{"link="} + (e.slice.linkage == "dynamic" ? "shared" : "static"), "runtime-link=shared", "threading=multi"};
    std::ranges::copy(e.slice.b2_properties, std::back_inserter(argv));
    // not dll-path, which b2 normalizes as a path: linker flags, which it gives the shell as they are but
    // with backslashes turned into slashes, so the shell's single quotes keep the $ (build-lua-api.md 6.2)
    if (!e.slice.toolchain.rpath.empty()) argv.push_back("linkflags=-Wl,-rpath,'" + e.slice.toolchain.rpath + "'");
    std::vector<std::string> const args = array_field(L, 1, "args", fn);
    // the project's C++ standard (otok-toml.md 5.1)
    if (e.slice.cxx_standard) {
        std::string const wanted = "cxxstd=" + std::to_string(*e.slice.cxx_standard);
        for (std::string const& a : args) {
            if (a.starts_with("cxxstd=") && a != wanted) raise(L, fn + ": the project builds C++" + wanted.substr(7) + ", and args has '" + a + "': take slice.cxx_standard");
        }
        if (std::ranges::find(args, wanted) == args.end()) argv.push_back(wanted);
    }
    std::ranges::copy(args, std::back_inserter(argv));
    lua_getfield(L, 1, "targets");
    bool const has_targets = !lua_isnil(L, -1);
    lua_pop(L, 1);
    std::ranges::copy(has_targets ? array_field(L, 1, "targets", fn) : std::vector<std::string>{"install"}, std::back_inserter(argv));
    run_command(L, std::move(argv), o, true, false);
    return 0;
}

int otok_meson(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string const fn = "otok.meson";
    check_keys(L, 1, fn, {"source", "build", "options", "args", "cwd", "env"}, false);
    script_env const& e = env_of(L);
    std::string const meson = toolchain_program(L, fn, "meson");
    std::string const ninja = toolchain_program(L, fn, "ninja");
    if (e.slice.meson_file.empty()) raise(L, fn + ": the target of this slice generates no meson machine file");
    process_options o = read_process_options(L, 1, fn);
    // the machine file carries the tools and the flags (build-lua-api.md 6.2)
    drop_given(e, o, {"CC", "CXX", "AR", "CFLAGS", "CXXFLAGS", "LDFLAGS"});
    auto const source = string_field(L, 1, "source", fn);
    auto const build = string_field(L, 1, "build", fn);
    std::string const build_dir = script_path(build ? confined(L, *build, access::write) : e.slice.build);

    // meson checks the prefix by the paths of the host machine (meson 1.12): a prefix with a drive is not absolute for a
    // host of POSIX paths, so it is "/" and the prefix is the destdir of the install
    bool const posix_host = e.slice.meson_cross && !e.slice.meson_system.empty() && e.slice.meson_system != "windows" && e.slice.meson_system != "cygwin";
    bool const destdir = posix_host && e.slice.prefix.has_root_name();
    std::vector<std::string> setup{meson, "setup", build_dir, script_path(source ? confined(L, *source, access::read) : e.slice.source),
        (e.slice.meson_cross ? "--cross-file=" : "--native-file=") + script_path(e.slice.meson_file),
        "--prefix=" + (destdir ? std::string{"/"} : script_path(e.slice.prefix)),
        "--libdir=lib", "--buildtype=plain", std::string{"--default-library="} + (e.slice.linkage == "dynamic" ? "shared" : "static"),
        "--wrap-mode=nodownload"};
    if (!e.slice.deps.empty()) {
        std::string pkg_config, prefixes;
        for (auto const& [name, d] : e.slice.deps) {
            pkg_config += (pkg_config.empty() ? "" : ",") + script_path(d.prefix / "lib" / "pkgconfig");
            prefixes += (prefixes.empty() ? "" : ",") + script_path(d.prefix);
        }
        setup.push_back("-Dpkg_config_path=" + pkg_config);
        setup.push_back("-Dcmake_prefix_path=" + prefixes);
    }
    std::ranges::copy(e.slice.meson_options, std::back_inserter(setup));
    std::map<std::string, std::string> options;  // sorted: the same command line on every run
    if (push_field(L, 1, "options")) {
        if (!lua_istable(L, -1)) raise(L, fn + ": options must be a table, not " + luaL_typename(L, -1));
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) raise(L, fn + ": options keys must be strings");
            std::string const name = lua_tostring(L, -2);
            std::string const what = fn + ": options." + name;
            if (lua_isboolean(L, -1)) {
                options[name] = lua_toboolean(L, -1) ? "true" : "false";
            } else if (lua_istable(L, -1)) {
                std::string joined;
                for (std::string const& item : string_array(L, -1, what)) joined += (joined.empty() ? "" : ",") + item;
                options[name] = joined;
            } else {
                options[name] = string_value(L, -1, what);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    // the project's standards (otok-toml.md 5.1)
    for (auto const& [standard, option, meson_value, field] : {std::tuple{e.slice.c_standard, "c_std", &e.slice.meson_c_std, "c_standard"},
             std::tuple{e.slice.cxx_standard, "cpp_std", &e.slice.meson_cpp_std, "cxx_standard"}}) {
        if (!standard) continue;
        // the target's value for meson (c++23; MSVC: c++latest), none for MSVC's C89 and C99, which have no flag
        std::string const& wanted = *meson_value;
        if (wanted.empty()) continue;
        if (auto it = options.find(option); it != options.end() && it->second != wanted) {
            raise(L, fn + ": the project builds " + wanted + ", and options." + option + " is '" + it->second + "': take slice." + field);
        }
        options[option] = wanted;
    }
    for (auto const& [name, value] : options) setup.push_back("-D" + name + "=" + value);
    std::ranges::copy(array_field(L, 1, "args", fn), std::back_inserter(setup));
    run_command(L, std::move(setup), o, true, false, "setup");

    std::vector<std::string> compile{ninja, "-C", build_dir};
    std::ranges::copy(parallel_flags(e, o, std::nullopt, ninja, "-j"), std::back_inserter(compile));
    run_command(L, std::move(compile), o, true, false, "compile");
    std::vector<std::string> install{meson, "install", "-C", build_dir, "--no-rebuild"};
    if (destdir) install.push_back("--destdir=" + script_path(e.slice.prefix));
    run_command(L, std::move(install), o, true, false, "install");

    // meson install names a Mach-O dynamic library after the place it installs it in, but where the
    // libraries are is the consumer's choice (docs/PLAN.md decision 31): `@rpath/<file>` again, and so
    // are the references between the slice's libraries
    std::string const& install_name_tool = e.slice.toolchain.install_name_tool;
    std::error_code ec;
    if (e.slice.linkage == "dynamic" && !install_name_tool.empty() && fs::is_directory(e.slice.prefix / "lib", ec)) {
        std::string const prefix = script_path(e.slice.prefix) + "/";
        auto const rpath_name = [](std::string const& name) { return "@rpath/" + path_to_utf8(path_from_utf8(name).filename()); };
        for (auto const& entry : fs::directory_iterator{e.slice.prefix / "lib"}) {
            if (entry.is_symlink() || binary_kind_of(entry.path()) != binary_kind::macho_dylib) continue;
            load_names const names = load_names_of(entry.path());
            std::vector<std::string> argv{install_name_tool};
            if (names.id.starts_with(prefix)) argv.insert(argv.end(), {"-id", rpath_name(names.id)});
            for (std::string const& needed : names.needed) {
                if (needed.starts_with(prefix)) argv.insert(argv.end(), {"-change", needed, rpath_name(needed)});
            }
            if (argv.size() == 1) continue;
            argv.push_back(script_path(entry.path()));
            run_command(L, std::move(argv), o, true, false);
        }
    }
    return 0;
}

int otok_meson_cross_file(lua_State* L)
{
    script_env const& e = env_of(L);
    if (e.slice.meson_file.empty()) raise(L, "otok.meson_cross_file: the target of this slice generates no meson machine file");
    lua_pushstring(L, script_path(e.slice.meson_file).c_str());
    return 1;
}

int otok_cmake_toolchain(lua_State* L)
{
    script_env const& e = env_of(L);
    if (e.slice.cmake_toolchain.empty()) raise(L, "otok.cmake_toolchain: the target of this slice generates no CMake toolchain file");
    lua_pushstring(L, script_path(e.slice.cmake_toolchain).c_str());
    return 1;
}

// ---- files (6.3)

fs::path path_arg(lua_State* L, int idx, std::string const& function, access a)
{
    return confined(L, string_value(L, idx, function + ": the path"), a);
}

int otok_exists(lua_State* L)
{
    std::error_code ec;
    lua_pushboolean(L, fs::exists(path_arg(L, 1, "otok.exists", access::read), ec));
    return 1;
}

int otok_read(lua_State* L)
{
    auto text = read_file(path_arg(L, 1, "otok.read", access::read));
    if (!text) raise(L, text.error().message);
    lua_pushlstring(L, text->data(), text->size());
    return 1;
}

// Whether `file` is a regular file holding `text`: otok.write and otok.copy leave it, its modification time too, so
// that a build directory kept between builds sees no change (build-lua-api.md 6, otok-dev-toml.md 4).
bool holds(fs::path const& file, std::string_view text)
{
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(file, ec)) || fs::file_size(file, ec) != text.size() || ec) return false;
    auto const content = read_file(file);
    return content && *content == text;
}

int otok_write(lua_State* L)
{
    fs::path const file = path_arg(L, 1, "otok.write", access::write);
    std::size_t size = 0;
    char const* text = luaL_checklstring(L, 2, &size);
    if (holds(file, {text, size})) return 0;
    fs::create_directories(file.parent_path());
    std::ofstream out{file, std::ios::binary | std::ios::trunc};
    out.write(text, static_cast<std::streamsize>(size));
    out.close();
    if (!out) raise(L, "otok.write: cannot write '" + script_path(file) + "'");
    return 0;
}

// A file copied unless the target holds its contents already.
void copy_file_unless_same(fs::path const& from, fs::path const& to)
{
    std::error_code ec;
    if (fs::file_size(from, ec) == fs::file_size(to, ec) && !ec) {
        auto const content = read_file(from);
        if (content && holds(to, *content)) return;
    }
    if (fs::is_symlink(fs::symlink_status(to, ec))) fs::remove(to);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

int otok_copy(lua_State* L)
{
    fs::path const from = path_arg(L, 1, "otok.copy", access::read);
    fs::path const to = path_arg(L, 2, "otok.copy", access::write);
    fs::create_directories(to.parent_path());
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(from, ec)) || !fs::is_directory(from, ec)) {
        if (fs::is_symlink(fs::symlink_status(from, ec))) {
            fs::copy(from, to, fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks);
        } else {
            copy_file_unless_same(from, fs::is_directory(to, ec) ? to / from.filename() : to);
        }
        return 0;
    }
    fs::create_directories(to);
    for (auto it = fs::recursive_directory_iterator{from}; it != fs::recursive_directory_iterator{}; ++it) {
        fs::path const target = to / it->path().lexically_relative(from);
        fs::file_status const status = it->symlink_status();
        if (fs::is_symlink(status)) {
            if (fs::exists(fs::symlink_status(target, ec))) fs::remove(target);
            fs::copy_symlink(it->path(), target);
        } else if (fs::is_directory(status)) {
            fs::create_directories(target);
        } else {
            copy_file_unless_same(it->path(), target);
        }
    }
    return 0;
}

int otok_move(lua_State* L)
{
    fs::path const from = path_arg(L, 1, "otok.move", access::write);
    fs::path const to = path_arg(L, 2, "otok.move", access::write);
    fs::create_directories(to.parent_path());
    fs::rename(from, to);
    return 0;
}

int otok_remove(lua_State* L)
{
    fs::path const p = path_arg(L, 1, "otok.remove", access::write);
    if (std::ranges::find(env_of(L).write_roots, p) != env_of(L).write_roots.end()) {
        raise(L, "otok.remove: '" + script_path(p) + "' is a directory of the slice itself");
    }
    fs::remove_all(long_path(p));
    return 0;
}

int otok_mkdir(lua_State* L)
{
    fs::create_directories(path_arg(L, 1, "otok.mkdir", access::write));
    return 0;
}

// `*` and `?` within one path component, `**` across components ("**/" also matches nothing).
bool glob_match(std::string_view pattern, std::string_view path)
{
    if (pattern.empty()) return path.empty();
    if (pattern.starts_with("**")) {
        std::string_view rest = pattern.substr(2);
        if (rest.starts_with('/') && glob_match(rest.substr(1), path)) return true;
        for (std::size_t i = 0; i <= path.size(); ++i) {
            if (glob_match(rest, path.substr(i))) return true;
        }
        return false;
    }
    if (pattern.front() == '*') {
        for (std::size_t i = 0; i <= path.size(); ++i) {
            if (glob_match(pattern.substr(1), path.substr(i))) return true;
            if (i < path.size() && path[i] == '/') break;
        }
        return false;
    }
    if (path.empty()) return false;
    if (pattern.front() == '?' ? path.front() == '/' : pattern.front() != path.front()) return false;
    return glob_match(pattern.substr(1), path.substr(1));
}

int otok_glob(lua_State* L)
{
    std::string const pattern = string_value(L, 1, "otok.glob: the pattern");
    script_env const& e = env_of(L);
    fs::path const base = lua_isnoneornil(L, 2) ? confined(L, script_path(e.slice.source), access::read) : path_arg(L, 2, "otok.glob", access::read);
    std::vector<std::string> found;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator{base, ec}; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        std::string const relative = path_to_utf8(it->path().lexically_relative(base).generic_u8string());
        if (glob_match(pattern, relative)) found.push_back(script_path(it->path()));
    }
    if (ec) raise(L, "otok.glob: cannot read '" + script_path(base) + "': " + ec.message());
    std::ranges::sort(found);
    lua_createtable(L, static_cast<int>(found.size()), 0);
    for (std::size_t i = 0; i < found.size(); ++i) {
        lua_pushlstring(L, found[i].data(), found[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int otok_join(lua_State* L)
{
    int const n = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= n; ++i) {
        std::string const part = string_value(L, i, "otok.join: argument " + std::to_string(i));
        if (part.empty()) continue;
        if (!out.empty() && out.back() != '/') out += '/';
        out += out.empty() ? part : part.substr(part.find_first_not_of('/') == std::string::npos ? part.size() : part.find_first_not_of('/'));
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// ---- messages (6.4)

int otok_log(lua_State* L)
{
    int const n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        std::size_t size = 0;
        char const* s = luaL_tolstring(L, i, &size);
        line += (i > 1 ? "\t" : "") + std::string{s, size};
        lua_pop(L, 1);
    }
    env_of(L).log(line);
    return 0;
}

int otok_warn(lua_State* L)
{
    std::string const text = luaL_checkstring(L, 1);
    script_env& e = env_of(L);
    e.warnings.push_back(text);
    e.log("warning: " + text);
    return 0;
}

}

script_env::script_env(slice_context const& s) : slice{s}
{
    auto const add = [](std::vector<fs::path>& roots, fs::path const& p) {
        if (p.empty()) return;
        std::error_code ec;
        fs::path real = fs::weakly_canonical(p, ec);
        roots.push_back(ec ? p.lexically_normal() : std::move(real));
    };
    for (fs::path const& p : {s.source, s.build, s.prefix}) {
        add(write_roots, p);
        add(read_roots, p);
    }
    add(read_roots, s.recipe);
    for (auto const& [name, p] : s.resources) add(read_roots, p);
    for (fs::path const& p : s.option_files) add(read_roots, p);
    for (auto const* deps : {&s.deps, &s.tool_deps}) {
        for (auto const& [name, d] : *deps) {
            add(read_roots, d.prefix);
            add(read_roots, d.include);
            add(read_roots, d.bin);
            for (fs::path const& dir : d.lib_dirs) add(read_roots, dir);
        }
    }
    if (s.tools_prefix) add(read_roots, *s.tools_prefix);
    add(read_roots, s.cmake_toolchain);  // the generated file only, not its directory
    add(read_roots, s.meson_file);
}

void script_env::log(std::string_view line) const
{
    if (slice.log.empty()) return;
    std::ofstream out{slice.log, std::ios::binary | std::ios::app};
    out << line << '\n';
}

result<fs::path> confine(script_env const& env, std::string_view path, access a)
{
    fs::path p = path_from_utf8(path);
    if (p.is_relative()) p = env.slice.source / p;
    std::error_code ec;
    fs::path const real = fs::weakly_canonical(p, ec);
    if (ec) return fail("cannot resolve '" + std::string{path} + "': " + ec.message());
    for (fs::path const& root : a == access::write ? env.write_roots : env.read_roots) {
        if (within(real, root)) return real;
    }
    return fail("'" + std::string{path} + "' is outside the directories build.lua may " + (a == access::write ? "write" : "read"));
}

std::string script_path(fs::path const& p)
{
    return path_to_utf8(p.generic_u8string());
}

void raise(lua_State* L, std::string const& message)
{
    luaL_where(L, 1);
    lua_pushlstring(L, message.data(), message.size());
    lua_concat(L, 2);
    lua_error(L);
    std::abort();  // lua_error throws
}

void open_otok_library(lua_State* L, script_env& env)
{
    static luaL_Reg const functions[] = {
        {"run", guarded<otok_run>},
        {"make", guarded<otok_make>},
        {"cmake", guarded<otok_cmake>},
        {"autotools", guarded<otok_autotools>},
        {"b2", guarded<otok_b2>},
        {"meson", guarded<otok_meson>},
        {"cmake_toolchain", guarded<otok_cmake_toolchain>},
        {"meson_cross_file", guarded<otok_meson_cross_file>},
        {"exists", guarded<otok_exists>},
        {"read", guarded<otok_read>},
        {"write", guarded<otok_write>},
        {"copy", guarded<otok_copy>},
        {"move", guarded<otok_move>},
        {"remove", guarded<otok_remove>},
        {"mkdir", guarded<otok_mkdir>},
        {"glob", guarded<otok_glob>},
        {"join", guarded<otok_join>},
        {"log", guarded<otok_log>},
        {"warn", guarded<otok_warn>},
        {nullptr, nullptr},
    };
    lua_newtable(L);
    lua_pushlightuserdata(L, &env);
    luaL_setfuncs(L, functions, 1);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "api");
    lua_setglobal(L, "otok");
}

}
