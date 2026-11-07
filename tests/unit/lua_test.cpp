// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The Lua runtime: the restricted state (build-lua-api.md 7) and build.lua runs (build-lua-api.md 2-6, 8).

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/lua/build_script.hpp"
#include "otok/lua/state.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

// Runs `script` in a fresh state; the printed lines, or the error.
result<std::vector<std::string>> run_chunk(std::string const& script)
{
    test::temp_dir dir;
    dir.write("test.lua", script);
    std::vector<std::string> lines;
    auto state = lua_state::create([&](std::string_view line) { lines.emplace_back(line); });
    if (!state) return std::unexpected{state.error()};
    if (auto ok = state->load_file(dir.path() / "test.lua", "test.lua"); !ok) return std::unexpected{ok.error()};
    if (auto ok = state->call(0, 0); !ok) return std::unexpected{ok.error()};
    return lines;
}

}

TEST(lua_state, restricted_standard_library)
{
    auto lines = run_chunk(R"(
assert(io == nil and os == nil and package == nil and require == nil and debug == nil)
assert(dofile == nil and loadfile == nil)
assert(string and table and math and utf8 and coroutine)
local f, err = load(string.dump(function() end))
assert(f == nil and err:find("binary"), err)
assert(load("return 1 + 1")() == 2)
assert(load("return x", "chunk", "b", {x = 5})() == 5)  -- text, whatever mode is asked
print("a", 1, nil)
print("two\nlines")
warn("careful ", "now")
warn("@on")
)");
    ASSERT_TRUE(lines.has_value()) << lines.error().message;
    EXPECT_EQ(*lines, (std::vector<std::string>{"a\t1\tnil", "two", "lines", "warning: careful now"}));
}

TEST(lua_state, is_deterministic)
{
    std::string const script = R"(
local t = {}
for i = 1, 64 do t["key" .. i] = i end
local order = {}
for k in pairs(t) do order[#order + 1] = k end
print(table.concat(order, ","))
print(math.random(1, 1000000), math.random(1, 1000000))
)";
    auto first = run_chunk(script);
    auto second = run_chunk(script);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(*first, *second);
}

TEST(lua_state, reports_errors_with_positions)
{
    auto runtime = run_chunk("local x = 1\nerror('boom')\n");
    ASSERT_FALSE(runtime.has_value());
    EXPECT_EQ(runtime.error().message.find("test.lua:2: boom\nstack traceback:"), 0u) << runtime.error().message;

    auto syntax = run_chunk("x = = 1\n");
    ASSERT_FALSE(syntax.has_value());
    EXPECT_EQ(syntax.error().message.find("test.lua:1: "), 0u) << syntax.error().message;

    auto object = run_chunk("error({})\n");
    ASSERT_FALSE(object.has_value());
    EXPECT_EQ(object.error().message.find("(error object is a table value)"), 0u) << object.error().message;
}

namespace {

// Writes a library and a header directory into the prefix and returns them.
std::string const finish = R"(
local function finish(slice)
    otok.write(slice.prefix .. "/lib/libp.a", "!<arch>\n")
    otok.mkdir(slice.prefix .. "/include")
    return { libraries = { p = slice.prefix .. "/lib/libp.a" }, headers = slice.prefix .. "/include" }
end
)";

class build_script : public ::testing::Test
{
protected:
    void SetUp() override
    {
        for (char const* d : {"source", "build", "prefix", "recipe", "dep/include", "dep/lib", "res"}) {
            std::filesystem::create_directories(dir.path() / d);
        }
        // as the builder gives them: resolved (macOS: /var is a link to /private/var)
        base = std::filesystem::canonical(dir.path());
        slice.name = "iossim-arm64";
        slice.os = "apple";
        slice.platform = "iossim";
        slice.arch = "arm64";
        slice.environment = "macos/xcode";
        slice.host = "macos-arm64";
        slice.compiler = "xcode";
        slice.triple = "arm64-apple-ios15.0-simulator";
        slice.source = base / "source";
        slice.build = base / "build";
        slice.prefix = base / "prefix";
        slice.recipe = base / "recipe";
        slice.resources["data"] = base / "res";
        slice.vars["openssl_target"] = std::string{"iossimulator-xcrun"};
        slice.jobs = 4;
        slice.toolchains = {{"cmake", "cmake"}, {"ninja", "ninja"}, {"make", "make"}, {"autoconf", "autoconf"}, {"automake", "automake"}, {"libtool", "libtool"}};
        slice.toolchain.cc = "/usr/bin/clang";
        slice.toolchain.cflags = {"-O2", "-target", "arm64-apple-ios15.0-simulator"};
        slice.toolchain.env = {{"PATH", "/usr/bin"}, {"CC", "/usr/bin/clang"}, {"LDFLAGS", "-Wl,-x"}};
        slice.target_fields["rust_target"] = "aarch64-apple-ios-sim";
        slice.target_tables["apple"] = {{"sdk", "iphonesimulator"}, {"min_version", "15.0"}};
        slice.options = {{"shared", false}, {"level", std::int64_t{3}}, {"features", std::vector<std::string>{"a", "b"}}};
        slice_dependency dep;
        dep.prefix = base / "dep";
        dep.include = base / "dep" / "include";
        dep.libraries["z"] = base / "dep" / "lib" / "libz.a";
        dep.lib_dirs = {base / "dep" / "lib"};
        dep.version = "1.3.1";
        slice.deps["zlib"] = dep;
        slice.log = base / "slice.log";
        slice.run = [this](process_spec const& spec) -> result<process_exit> {
            specs.push_back(spec);
            return next_exit;
        };
        slice.jobserver_client = [this](std::string const& tool, env_vars const&) {
            auto const it = jobserver_clients.find(tool);
            return it != jobserver_clients.end() ? it->second : jobserver_protocol::none;
        };
    }

    result<build_output> run(std::string const& script, build_entry entry = build_entry::build)
    {
        dir.write("recipe/build.lua", finish + script);
        return run_build_script(base / "recipe" / "build.lua", "build.lua", entry, slice);
    }

    std::string path(std::string const& relative) const { return path_to_utf8((base / relative).generic_u8string()); }

    std::string log() const { return read_file(slice.log).value_or(""); }

    test::temp_dir dir;
    std::filesystem::path base;
    slice_context slice;
    std::vector<process_spec> specs;
    // how the tools take the jobserver
    std::map<std::string, jobserver_protocol> jobserver_clients{{"make", jobserver_protocol::named}, {"ninja", jobserver_protocol::named}};
    process_exit next_exit;
};

}

TEST_F(build_script, sees_slice_options_and_deps)
{
    auto out = run(R"(
function build(slice, options, deps)
    assert(slice.name == "iossim-arm64" and slice.os == "apple" and slice.platform == "iossim" and slice.arch == "arm64")
    assert(slice.environment == "macos/xcode" and slice.host == "macos-arm64" and slice.compiler == "xcode")
    assert(slice.triple == "arm64-apple-ios15.0-simulator" and slice.configuration == "release" and slice.is_tool == false)
    assert(slice.source:find("/source$") and slice.prefix:find("/prefix$") and not slice.source:find("\\", 1, true))
    assert(slice.resources.data:find("/res$") and slice.vars.openssl_target == "iossimulator-xcrun" and slice.jobs == 4)
    assert(slice.toolchain.cc == "/usr/bin/clang" and #slice.toolchain.cflags == 3 and slice.toolchain.env.CC == "/usr/bin/clang")
    assert(table.concat(slice.toolchain.cflags, " ") == "-O2 -target arm64-apple-ios15.0-simulator")
    assert(slice.rust_target == "aarch64-apple-ios-sim" and slice.apple.sdk == "iphonesimulator" and slice.tools == nil)
    assert(options.shared == false and options.level == 3 and options.features[2] == "b")
    assert(deps.zlib.version == "1.3.1" and deps.zlib.libraries.z:find("/libz.a$") and deps.zlib.lib_dirs[1]:find("/lib$"))
    assert(next(deps.tools) == nil)
    local keys = {}
    for k in pairs(slice.apple) do keys[#keys + 1] = k end
    table.sort(keys)
    assert(table.concat(keys, ",") == "min_version,sdk")
    for _, f in ipairs(options.features) do keys[#keys + 1] = f end
    assert(otok.api == 1)
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(out->libraries.at("p").filename(), "libp.a");
    ASSERT_TRUE(out->headers.has_value());
    EXPECT_EQ(out->headers->filename(), "include");
}

TEST_F(build_script, tables_are_read_only)
{
    for (std::string const statement : {"slice.name = 'x'", "slice.toolchain.cflags[1] = 'x'", "options.level = 1", "deps.zlib = nil",
             "deps.zlib.libraries.z = 'x'", "setmetatable(slice, nil)"}) {
        auto out = run("function build(slice, options, deps)\n    " + statement + "\nend\n");
        ASSERT_FALSE(out.has_value()) << statement;
        EXPECT_NE(out.error().message.find("build.lua:"), std::string::npos) << out.error().message;
        bool const reported = out.error().message.find("attempt to modify a read-only table") != std::string::npos
            || out.error().message.find("cannot change a protected metatable") != std::string::npos;
        EXPECT_TRUE(reported) << out.error().message;
    }
}

TEST_F(build_script, runs_programs)
{
    next_exit.output = "captured\n";
    auto out = run(R"(
function build(slice, options, deps)
    assert(otok.run{"cc", "-c", "a file.c"} == 0)
    local text, code = otok.run({"git", "describe"}, { cwd = slice.build, env = { CC = false, EXTRA = "1" }, capture = true })
    assert(text == "captured\n" and code == 0)
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 2u);
    EXPECT_EQ(specs[0].argv, (std::vector<std::string>{"cc", "-c", "a file.c"}));
    EXPECT_EQ(specs[0].cwd, slice.source);
    EXPECT_EQ(specs[0].env, slice.toolchain.env);
    EXPECT_EQ(specs[0].log, slice.log);
    EXPECT_FALSE(specs[0].capture);
    EXPECT_EQ(path_to_utf8(specs[1].cwd.generic_u8string()), path("build"));
    EXPECT_FALSE(specs[1].env.contains("CC"));
    EXPECT_EQ(specs[1].env.at("EXTRA"), "1");
    EXPECT_TRUE(specs[1].capture);
    EXPECT_NE(log().find("$ cc -c 'a file.c'\n$ git describe\n"), std::string::npos) << log();
}

TEST_F(build_script, failed_programs)
{
    next_exit.code = 2;
    auto failed = run("function build(slice)\n    otok.run{'make', 'all'}\nend\n");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message.find("build.lua:8: 'make' failed with exit code 2: make all\n"), 0u) << failed.error().message;

    auto unchecked = run("function build(slice)\n    assert(otok.run({'make'}, {check = false}) == 2)\n    return finish(slice)\nend\n");
    EXPECT_TRUE(unchecked.has_value()) << unchecked.error().message;

    auto typo = run("function build(slice)\n    otok.run({'make'}, {chek = false})\nend\n");
    ASSERT_FALSE(typo.has_value());
    EXPECT_NE(typo.error().message.find("otok.run: unknown option 'chek'"), std::string::npos) << typo.error().message;

    auto empty = run("function build(slice)\n    otok.run{}\nend\n");
    ASSERT_FALSE(empty.has_value());
    EXPECT_NE(empty.error().message.find("otok.run: the command is empty"), std::string::npos) << empty.error().message;
}

TEST_F(build_script, runs_real_processes)
{
    slice.run = run_process;
    slice.toolchain.env = {{"FOO", "1"}};
    std::string const child = path_to_utf8(path_from_utf8(OTOK_TEST_CHILD).generic_u8string());
    auto out = run(R"(
function build(slice)
    local child = ")" + child + R"("
    assert(otok.run({child, "args", "a b"}, {capture = true}) == "a b\n")
    assert(otok.run({child, "env", "FOO", "BAR"}, {capture = true, env = {FOO = false, BAR = "2"}}) == "<unset>\n2\n")
    assert(otok.run({child, "exit", "3"}, {check = false}) == 3)
    otok.run{child, "stderr", "to the log"}
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_NE(log().find("to the log"), std::string::npos) << log();
}

TEST_F(build_script, runs_b2_with_a_user_config)
{
    slice.toolchain.cxx = "/usr/bin/clang++";
    slice.toolchain.cxxflags = {"-O2", "-std=c++20"};
    slice.toolchain.ldflags = {"-target", "arm64-apple-ios15.0-simulator"};
    slice.toolchain.env["MAKEFLAGS"] = "--jobserver-auth=fifo:/tmp/x";
    slice.b2_toolset = "darwin";
    slice.b2_properties = {"target-os=iphone", "architecture=arm"};
    auto out = run(R"(
function build(slice, options, deps)
    otok.b2{ b2 = "/tools/bin/b2", args = { "--with-json" }, targets = { "stage" } }
    otok.b2{ b2 = "/tools/bin/b2" }
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 2u);
    EXPECT_EQ(specs[0].argv, (std::vector<std::string>{"/tools/bin/b2", "--user-config=" + path("build/otok-user-config.jam"), "--ignore-site-config", "-j4",
        "--build-dir=" + path("build/b2"), "--prefix=" + path("prefix"), "--stagedir=" + path("prefix"), "toolset=darwin-otok", "variant=release",
        "link=static", "runtime-link=shared", "threading=multi", "target-os=iphone", "architecture=arm", "--with-json", "stage"}));
    EXPECT_EQ(specs[0].cwd, base / "source");
    EXPECT_FALSE(specs[0].env.contains("MAKEFLAGS"));  // b2 has no jobserver
    EXPECT_EQ(specs[1].argv.back(), "install");
    EXPECT_EQ(read_file(base / "build" / "otok-user-config.jam").value_or("?"),
        "# Generated by otok for the slice iossim-arm64 (arm64-apple-ios15.0-simulator).\n"
        "using darwin : otok : \"/usr/bin/clang++\"\n"
        "    : <cflags>\"-O2\" <cflags>\"-target\" <cflags>\"arm64-apple-ios15.0-simulator\""
        " <asmflags>\"-O2\" <asmflags>\"-target\" <asmflags>\"arm64-apple-ios15.0-simulator\""
        " <cxxflags>\"-O2\" <cxxflags>\"-std=c++20\" <linkflags>\"-target\" <linkflags>\"arm64-apple-ios15.0-simulator\" <striper>\n"
        "    ;\n");

    // with otok's launcher, the compilers wait for the jobserver's tokens
    specs.clear();
    slice.launcher = "/opt/otok/bin/otok";
    out = run("function build(slice)\n    otok.b2{ b2 = 'b2' }\n    return finish(slice)\nend\n");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_NE(read_file(base / "build" / "otok-user-config.jam").value_or("?").find(
                  "using darwin : otok : \"/opt/otok/bin/otok\" \"jobserver-exec\" \"--slot\" \"" + path("build/otok-job.lock")
                  + "\" \"--auth\" \"fifo:/tmp/x\" \"--\" \"/usr/bin/clang++\"\n"),
        std::string::npos)
        << read_file(base / "build" / "otok-user-config.jam").value_or("?");
    EXPECT_EQ(specs[0].argv[3], "-j4");
    slice.launcher.clear();

    // gcc gets the archiver; debug and dynamic; a target without b2
    specs.clear();
    slice.b2_toolset = "gcc";
    slice.b2_properties.clear();
    slice.toolchain.ar = "/usr/bin/ar";
    slice.toolchain.ldflags = {};
    slice.configuration = "debug";
    slice.linkage = "dynamic";
    out = run(R"(
function build(slice, options, deps)
    otok.b2{ b2 = "b2" }
    assert(not pcall(otok.b2, {}))
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs[0].argv[8], "variant=debug");
    EXPECT_EQ(specs[0].argv[9], "link=shared");
    EXPECT_NE(read_file(base / "build" / "otok-user-config.jam").value_or("?").find("<archiver>\"/usr/bin/ar\"\n"), std::string::npos);
    slice.b2_toolset.clear();
    auto none = run("function build(slice)\n    otok.b2{ b2 = 'b2' }\nend\n");
    ASSERT_FALSE(none.has_value());
    EXPECT_NE(none.error().message.find("otok.b2: the target of this slice cannot build with b2"), std::string::npos) << none.error().message;
}

TEST_F(build_script, build_system_helpers)
{
    slice.cmake_toolchain = base / "toolchain.cmake";
    auto out = run(R"(
function build(slice, options, deps)
    otok.cmake{ defines = { B = true, A = "x", C = 3, D = false }, args = { "-Wno-dev" }, targets = { "lib" } }
    otok.cmake{ source = slice.source .. "/sub", install = false, generator = "Unix Makefiles" }
    otok.make{ "install", args = { "V=1" } }
    otok.make{ jobs = 2 }
    otok.autotools{ args = { "--with-zlib" } }
    assert(otok.cmake_toolchain():find("/toolchain.cmake$"))
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 9u);
    std::string const dep = path("dep");
    EXPECT_EQ(specs[0].argv, (std::vector<std::string>{"cmake", "-S", path("source"), "-B", path("build"), "-G", "Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=" + path("toolchain.cmake"), "-DCMAKE_MAKE_PROGRAM=ninja",
        "-DCMAKE_INSTALL_PREFIX=" + path("prefix"), "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF",
        "-DCMAKE_PREFIX_PATH=" + dep, "-DCMAKE_FIND_ROOT_PATH=" + dep, "-DA=x", "-DB=ON", "-DC=3", "-DD=OFF", "-Wno-dev",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"}));
    EXPECT_EQ(specs[1].argv, (std::vector<std::string>{"cmake", "--build", path("build"), "--config", "Release", "--target", "lib", "--parallel", "4"}));
    EXPECT_EQ(specs[2].argv, (std::vector<std::string>{"cmake", "--install", path("build"), "--config", "Release"}));
    EXPECT_EQ(specs[3].argv[2], path("source/sub"));
    EXPECT_EQ(specs[3].argv[6], "Unix Makefiles");
    EXPECT_EQ(specs[4].argv[1], "--build");  // and no install
    EXPECT_EQ(specs[5].argv, (std::vector<std::string>{"make", "-j", "4", "V=1", "install"}));
    EXPECT_EQ(specs[6].argv, (std::vector<std::string>{"make", "-j", "2"}));
    EXPECT_EQ(specs[7].argv, (std::vector<std::string>{"./configure", "--prefix=" + path("prefix"), "--disable-shared", "--enable-static", "--with-zlib"}));
    EXPECT_EQ(specs[7].env.at("CPPFLAGS"), "-I" + path("dep/include"));
    EXPECT_EQ(specs[7].env.at("LDFLAGS"), "-Wl,-x -L" + path("dep/lib"));
    EXPECT_EQ(specs[7].env.at("PKG_CONFIG_PATH"), path("dep/lib/pkgconfig"));
    EXPECT_EQ(specs[8].argv, (std::vector<std::string>{"make", "-j", "4", "install"}));

    // the jobserver replaces the fixed share; a cross build gets --host; debug builds, dynamic libraries
    specs.clear();
    slice.toolchain.env["MAKEFLAGS"] = "--jobserver-auth=fifo:/tmp/x";
    slice.autotools_host = "aarch64-apple-darwin";
    slice.autotools_build = "aarch64-apple-darwin27.0.0";
    slice.configuration = "debug";
    slice.linkage = "dynamic";
    slice.cmake_toolchain.clear();
    out = run(R"(
function build(slice, options, deps)
    assert(slice.linkage == "dynamic")
    otok.make{ "all" }
    otok.autotools{ autoreconf = true, targets = { "install-lib" } }
    otok.cmake{}
    assert(not pcall(otok.cmake_toolchain))
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 7u);
    EXPECT_EQ(specs[0].argv, (std::vector<std::string>{"make", "all"}));
    EXPECT_EQ(specs[1].argv, (std::vector<std::string>{"autoreconf", "-fi"}));
    EXPECT_EQ(specs[2].argv.back(), "--build=aarch64-apple-darwin27.0.0");
    EXPECT_EQ(specs[2].argv[specs[2].argv.size() - 2], "--host=aarch64-apple-darwin");
    EXPECT_EQ(specs[2].argv[2], "--enable-shared");
    EXPECT_EQ(specs[2].argv[3], "--disable-static");
    EXPECT_EQ(specs[3].argv, (std::vector<std::string>{"make", "install-lib"}));
    EXPECT_EQ(specs[4].argv[7], "-DCMAKE_MAKE_PROGRAM=ninja");  // no toolchain file
    EXPECT_EQ(specs[4].argv[8], "-DCMAKE_INSTALL_PREFIX=" + path("prefix"));
    EXPECT_EQ(specs[4].argv[9], "-DCMAKE_BUILD_TYPE=Debug");
    EXPECT_EQ(specs[4].argv[10], "-DBUILD_SHARED_LIBS=ON");
    EXPECT_EQ(specs[5].argv, (std::vector<std::string>{"cmake", "--build", path("build"), "--config", "Debug"}));
    EXPECT_EQ(specs[5].env.at("MAKEFLAGS"), "--jobserver-auth=fifo:/tmp/x");

    // a tool that does not understand it, a generator without a known tool and a fixed number run
    // without MAKEFLAGS; otok.run keeps it
    specs.clear();
    jobserver_clients = {{"ninja", jobserver_protocol::named}};
    out = run(R"(
function build(slice, options, deps)
    otok.make{ "all" }
    otok.cmake{ generator = "Xcode", install = false }
    otok.cmake{ install = false }
    otok.make{ jobs = 3 }
    otok.run{ "tool" }
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 7u);
    EXPECT_EQ(specs[0].argv, (std::vector<std::string>{"make", "-j", "4", "all"}));
    EXPECT_FALSE(specs[0].env.contains("MAKEFLAGS"));
    EXPECT_EQ(specs[2].argv.back(), "4");  // cmake --build … --parallel 4
    EXPECT_FALSE(specs[2].env.contains("MAKEFLAGS"));
    EXPECT_TRUE(specs[1].env.contains("MAKEFLAGS"));  // configuring is not a build
    EXPECT_EQ(specs[4].argv, (std::vector<std::string>{"cmake", "--build", path("build"), "--config", "Debug"}));
    EXPECT_EQ(specs[5].argv, (std::vector<std::string>{"make", "-j", "3"}));
    EXPECT_FALSE(specs[5].env.contains("MAKEFLAGS"));
    EXPECT_TRUE(specs[6].env.contains("MAKEFLAGS"));
}

TEST_F(build_script, older_make_inherits_the_jobserver)
{
    slice.toolchain.env["MAKEFLAGS"] = "-j4 --jobserver-auth=fifo:/tmp/jobserver-1";
    jobserver_clients = {{"make", jobserver_protocol::old_descriptors}};
    auto out = run(R"(
function build(slice, options, deps)
    otok.make{ "all" }
    otok.autotools{}
    otok.cmake{ generator = "Unix Makefiles", install = false }
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 5u);
    // make 3.81 run by otok: the fifo as its descriptors, a bare -j
    EXPECT_EQ(specs[0].argv, (std::vector<std::string>{"make", "all"}));
    EXPECT_EQ(specs[0].env.at("MAKEFLAGS"), "-j --jobserver-fds=3,4");
    EXPECT_EQ(specs[0].jobserver_fifo, std::filesystem::path{"/tmp/jobserver-1"});
    EXPECT_TRUE(specs[1].jobserver_fifo.empty());   // configure
    EXPECT_EQ(specs[2].argv, (std::vector<std::string>{"make", "install"}));
    EXPECT_EQ(specs[2].env.at("MAKEFLAGS"), "-j --jobserver-fds=3,4");
    // run by cmake, which need not pass descriptors on: a fixed number
    EXPECT_EQ(specs[4].argv.back(), "4");
    EXPECT_FALSE(specs[4].env.contains("MAKEFLAGS"));
    EXPECT_TRUE(specs[4].jobserver_fifo.empty());
}

// docs/PLAN.md decision 61: a tool without the jobserver runs the jobs the slice may hold when it starts, given back
// when it ends; a number the recipe gives stays.
TEST_F(build_script, tools_without_the_jobserver_hold_jobs_while_they_run)
{
    jobserver_clients = {};
    slice.toolchain.env["MAKEFLAGS"] = "--jobserver-auth=fifo:/tmp/x";
    std::vector<std::string> events;
    slice.hold_jobs = [&](std::int64_t most) -> std::pair<std::int64_t, std::function<void()>> {
        events.push_back("hold " + std::to_string(most));
        return {2, [&] { events.push_back("put back"); }};
    };
    slice.run = [&](process_spec const& spec) -> result<process_exit> {
        specs.push_back(spec);
        events.push_back("run");
        return next_exit;
    };
    auto out = run(R"(
function build(slice, options, deps)
    otok.make{ "all" }
    otok.make{ jobs = 3 }
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 2u);
    EXPECT_EQ(specs[0].argv, (std::vector<std::string>{"make", "-j", "2", "all"}));
    EXPECT_EQ(specs[1].argv, (std::vector<std::string>{"make", "-j", "3"}));
    EXPECT_EQ(events, (std::vector<std::string>{"hold 4", "run", "put back", "run"}));
    EXPECT_NE(log().find("$ make -j 2 all\n"), std::string::npos) << log();
}

TEST_F(build_script, tells_the_stage_of_each_command)
{
    std::vector<std::string> stages;
    slice.stage = [&](std::string const& s) { stages.push_back(s); };
    auto out = run(R"(
function build(slice, options, deps)
    otok.run{ "perl", "Configure", "darwin64-arm64" }
    otok.run{ "/usr/bin/sh", "-c", "true" }
    otok.run{ "C:/tools/Python.EXE", "setup.py" }
    otok.make{ "-C", "sub", "VAR=1", "install_sw" }
    otok.run{ "ninja", "-C", "build/dir" }
    otok.run{ "/opt/bin/b2", "toolset=clang", "--prefix=/x", "install" }
    otok.run{ "install_name_tool", "-id", "@rpath/libz.dylib", "libz.dylib" }
    otok.cmake{ generator = "Unix Makefiles" }
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(stages, (std::vector<std::string>{"perl Configure", "sh", "Python setup.py", "make install_sw", "ninja", "b2 install",
        "install_name_tool", "configure", "build", "install"}));
}

TEST_F(build_script, helpers_run_the_declared_toolchains)
{
    slice.toolchains = {{"make", base / "tools" / "gmake"}, {"cmake", base / "tools" / "cmake"}, {"autoconf", base / "ac" / "autoconf"},
        {"automake", base / "am" / "automake"}, {"libtool", base / "lt" / "glibtool"}};
    auto out = run(R"(
function build(slice)
    assert(slice.toolchains.make:find("/tools/gmake$") and slice.toolchains.ninja == nil)
    otok.make{ "all" }
    otok.cmake{ generator = "Unix Makefiles", install = false }
    otok.autotools{ autoreconf = true }
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 6u);
    EXPECT_EQ(specs[0].argv.front(), path("tools/gmake"));
    EXPECT_EQ(specs[1].argv.front(), path("tools/cmake"));
    EXPECT_NE(std::ranges::find(specs[1].argv, "-DCMAKE_MAKE_PROGRAM=" + path("tools/gmake")), specs[1].argv.end());
    EXPECT_EQ(specs[2].argv.front(), path("tools/cmake"));
    // autoreconf and the companions of the chosen tools
    EXPECT_EQ(specs[3].argv, (std::vector<std::string>{path("ac/autoreconf"), "-fi"}));
    EXPECT_EQ(specs[3].env.at("AUTOCONF"), path("ac/autoconf"));
    EXPECT_EQ(specs[3].env.at("AUTOHEADER"), path("ac/autoheader"));
    EXPECT_EQ(specs[3].env.at("AUTOMAKE"), path("am/automake"));
    EXPECT_EQ(specs[3].env.at("ACLOCAL"), path("am/aclocal"));
    EXPECT_EQ(specs[3].env.at("LIBTOOLIZE"), path("lt/glibtoolize"));
    EXPECT_EQ(specs[5].argv.front(), path("tools/gmake"));

#ifdef _WIN32
    // the make of MSYS2: configure through the sh beside it, with paths as MSYS2 names them, and its directory before
    // Windows' own in PATH (build-lua-api.md 6.2)
    specs.clear();
    std::filesystem::create_directories(base / "tools");
    std::ofstream{base / "tools" / "sh.exe"} << "";
    slice.toolchain.env["PATH"] = "C:\\VS\\bin;C:\\Windows\\System32;C:\\Other";
    slice.toolchain.env["SystemRoot"] = "C:\\Windows";
    out = run("function build(slice)\n    otok.autotools{}\n    return finish(slice)\nend\n");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 2u);
    std::string const prefix = path("prefix");
    std::string const msys_prefix = "/" + std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[0])))) + prefix.substr(2);
    EXPECT_EQ(specs[0].argv[0], path("tools/sh.exe"));
    EXPECT_EQ(specs[0].argv[1], "./configure");
    EXPECT_EQ(specs[0].argv[2], "--prefix=" + msys_prefix);
    EXPECT_EQ(specs[0].env.at("PATH"), "C:\\VS\\bin;" + path_to_utf8(path_from_utf8(path("tools")).native()) + ";C:\\Windows\\System32;C:\\Other");
    EXPECT_EQ(specs[1].env.at("PATH"), specs[0].env.at("PATH"));
    std::filesystem::remove(base / "tools" / "sh.exe");
#endif

    // an undeclared toolchain fails before anything runs
    specs.clear();
    for (auto const& [code, message] : {std::pair{"otok.cmake{}", "otok.cmake: 'ninja' is not in [toolchains] of the recipe"},
             std::pair{"otok.autotools{ autoreconf = true, env = { X = '1' } }", "otok.autotools: 'autoconf' is not in [toolchains] of the recipe"}}) {
        if (std::string_view{code}.starts_with("otok.autotools")) slice.toolchains.erase("autoconf");
        auto failed = run(std::string{"function build(slice)\n    "} + code + "\nend\n");
        ASSERT_FALSE(failed.has_value()) << code;
        EXPECT_NE(failed.error().message.find(message), std::string::npos) << failed.error().message;
    }
    slice.toolchains.erase("make");
    auto make = run("function build(slice)\n    otok.make{}\nend\n");
    ASSERT_FALSE(make.has_value());
    EXPECT_NE(make.error().message.find("otok.make: 'make' is not in [toolchains] of the recipe"), std::string::npos) << make.error().message;
    EXPECT_TRUE(specs.empty());
}

TEST_F(build_script, helpers_set_the_rpath)
{
    slice.linkage = "dynamic";
    slice.toolchain.rpath = "$ORIGIN";
    slice.b2_toolset = "gcc";
    slice.deps.clear();
    auto out = run(R"(
function build(slice, options, deps)
    assert(slice.toolchain.rpath == "$ORIGIN")
    otok.cmake{ install = false }
    otok.autotools{}
    otok.b2{ b2 = "b2" }
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 5u);
    EXPECT_NE(std::ranges::find(specs[0].argv, "-DCMAKE_INSTALL_RPATH=$ORIGIN"), specs[0].argv.end());
    // configure's shell and make each take one level of the escape
    std::string const ldflags = specs[2].env.at("LDFLAGS");
    EXPECT_TRUE(ldflags.ends_with(" -Wl,-rpath,\\$$ORIGIN")) << ldflags;
    EXPECT_NE(std::ranges::find(specs[4].argv, "linkflags=-Wl,-rpath,'$ORIGIN'"), specs[4].argv.end());  // b2 links through the shell

    // none where the target sets none
    specs.clear();
    slice.toolchain.rpath.clear();
    out = run("function build(slice)\n    otok.cmake{ install = false }\n    otok.b2{ b2 = 'b2' }\n    return finish(slice)\nend\n");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    for (process_spec const& s : specs) {
        for (std::string const& a : s.argv) EXPECT_EQ(a.find("ORIGIN"), std::string::npos) << a;
    }
}

// The project's standards (otok-toml.md 5.1) as each build system takes them; a recipe's other choice fails.
TEST_F(build_script, helpers_pass_the_standards)
{
    slice.c_standard = 89;
    slice.cxx_standard = 23;
    slice.meson_c_std = "c89";  // the target's names for meson (toolchain_profile::meson_standard)
    slice.meson_cpp_std = "c++23";
    slice.b2_toolset = "gcc";
    slice.toolchains["meson"] = "meson";
    slice.meson_file = base / "meson.ini";
    auto out = run(R"(
function build(slice, options, deps)
    assert(slice.c_standard == 89)
    assert(slice.cxx_standard == 23)
    otok.cmake{ install = false }
    otok.b2{ b2 = "b2" }
    otok.b2{ b2 = "b2", args = { "cxxstd=23" } }
    otok.meson{ options = { cpp_std = "c++23" } }
    local ok, message = pcall(otok.b2, { b2 = "b2", args = { "cxxstd=20" } })
    assert(not ok and message:find("otok.b2: the project builds C++23, and args has 'cxxstd=20': take slice.cxx_standard", 1, true), message)
    ok, message = pcall(otok.meson, { options = { cpp_std = "c++17" } })
    assert(not ok and message:find("otok.meson: the project builds c++23, and options.cpp_std is 'c++17': take slice.cxx_standard", 1, true), message)
    ok, message = pcall(otok.meson, { options = { c_std = "c11" } })
    assert(not ok and message:find("otok.meson: the project builds c89, and options.c_std is 'c11': take slice.c_standard", 1, true), message)
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 7u);  // cmake configure and build, b2 twice, meson setup, build and install
    for (char const* define : {"-DCMAKE_CXX_STANDARD=23", "-DCMAKE_CXX_STANDARD_REQUIRED=ON", "-DCMAKE_CXX_EXTENSIONS=OFF",
             "-DCMAKE_C_STANDARD=90", "-DCMAKE_C_STANDARD_REQUIRED=ON", "-DCMAKE_C_EXTENSIONS=OFF"}) {
        EXPECT_NE(std::ranges::find(specs[0].argv, define), specs[0].argv.end()) << define;
    }
    EXPECT_EQ(std::ranges::count(specs[2].argv, "cxxstd=23"), 1);
    EXPECT_EQ(std::ranges::count(specs[3].argv, "cxxstd=23"), 1);
    EXPECT_EQ(std::ranges::count(specs[4].argv, "-Dcpp_std=c++23"), 1);
    EXPECT_EQ(std::ranges::count(specs[4].argv, "-Dc_std=c89"), 1);

    // without one, the helpers add nothing
    specs.clear();
    slice.c_standard.reset();
    slice.cxx_standard.reset();
    out = run("function build(slice)\n    assert(slice.cxx_standard == nil)\n    otok.cmake{ install = false }\n    otok.b2{ b2 = 'b2' }\n    return finish(slice)\nend\n");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(std::ranges::count_if(specs[0].argv, [](std::string const& a) { return a.starts_with("-DCMAKE_CXX_STANDARD") || a.starts_with("-DCMAKE_C_STANDARD"); }), 0);
    EXPECT_EQ(std::ranges::count_if(specs[2].argv, [](std::string const& a) { return a.starts_with("cxxstd="); }), 0);
}

TEST_F(build_script, meson)
{
    slice.toolchains["meson"] = "meson";
    slice.meson_file = base / "meson.ini";
    slice.meson_cross = true;
    slice.toolchain.env["CC"] = "/usr/bin/clang";
    slice.toolchain.env["CFLAGS"] = "-O2";
    auto out = run(R"(
function build(slice, options, deps)
    otok.meson{ options = { tests = false, enable_asm = true, bitdepths = { "8", "16" }, level = 3, name = "x" }, args = { "--werror" } }
    otok.meson{ source = slice.source .. "/sub", build = slice.build .. "/b", env = { CFLAGS = "-DMINE" } }
    assert(otok.meson_cross_file():find("/meson.ini$"))
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 6u);
    std::string const dep = path("dep");
    EXPECT_EQ(specs[0].argv, (std::vector<std::string>{"meson", "setup", path("build"), path("source"), "--cross-file=" + path("meson.ini"),
        "--prefix=" + path("prefix"), "--libdir=lib", "--buildtype=plain", "--default-library=static", "--wrap-mode=nodownload",
        "-Dpkg_config_path=" + path("dep/lib/pkgconfig"), "-Dcmake_prefix_path=" + dep, "-Dbitdepths=8,16", "-Denable_asm=true", "-Dlevel=3",
        "-Dname=x", "-Dtests=false", "--werror"}));
    EXPECT_FALSE(specs[0].env.contains("CC"));  // the machine file has them
    EXPECT_FALSE(specs[0].env.contains("CFLAGS"));
    EXPECT_FALSE(specs[0].env.contains("LDFLAGS"));
    EXPECT_EQ(specs[0].env.at("PATH"), "/usr/bin");
    EXPECT_EQ(specs[1].argv, (std::vector<std::string>{"ninja", "-C", path("build"), "-j", "4"}));
    EXPECT_EQ(specs[2].argv, (std::vector<std::string>{"meson", "install", "-C", path("build"), "--no-rebuild"}));
    EXPECT_EQ(specs[3].argv[2], path("build/b"));
    EXPECT_EQ(specs[3].argv[3], path("source/sub"));
    EXPECT_EQ(specs[3].env.at("CFLAGS"), "-DMINE");  // given by the call: kept

    // a native file, dynamic libraries, the target's options, the jobserver
    specs.clear();
    slice.meson_cross = false;
    slice.linkage = "dynamic";
    slice.meson_options = {"-Db_vscrt=none"};
    slice.toolchain.env["MAKEFLAGS"] = "--jobserver-auth=fifo:/tmp/x";
    slice.deps.clear();
    out = run("function build(slice)\n    otok.meson{}\n    return finish(slice)\nend\n");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 3u);
    EXPECT_EQ(specs[0].argv[4], "--native-file=" + path("meson.ini"));
    EXPECT_EQ(specs[0].argv[8], "--default-library=shared");
    EXPECT_EQ(specs[0].argv.back(), "-Db_vscrt=none");
    EXPECT_EQ(specs[1].argv, (std::vector<std::string>{"ninja", "-C", path("build")}));

    // meson and ninja must be declared
    specs.clear();
    slice.toolchains.erase("meson");
    auto undeclared = run("function build(slice)\n    otok.meson{}\nend\n");
    ASSERT_FALSE(undeclared.has_value());
    EXPECT_NE(undeclared.error().message.find("otok.meson: 'meson' is not in [toolchains] of the recipe"), std::string::npos)
        << undeclared.error().message;
    EXPECT_TRUE(specs.empty());
}

TEST_F(build_script, file_functions)
{
    dir.write("source/src/a.c", "int a;\n");
    dir.write("source/src/sub/b.c", "int b;\n");
    dir.write("source/README", "readme\n");
    dir.write("recipe/CMakeLists.txt", "project(x)\n");
    auto out = run(R"(
function build(slice, options, deps)
    assert(otok.exists("README") and not otok.exists("missing"))
    assert(otok.read("src/a.c") == "int a;\n")
    otok.write("gen/config.h", "#define X 1\n")
    assert(otok.read(slice.source .. "/gen/config.h") == "#define X 1\n")
    otok.copy(slice.recipe .. "/CMakeLists.txt", "CMakeLists.txt")
    otok.copy("src", slice.build .. "/src-copy")
    assert(otok.exists(slice.build .. "/src-copy/sub/b.c"))
    otok.move("README", "docs/README")
    assert(not otok.exists("README") and otok.exists("docs/README"))
    otok.mkdir(slice.build .. "/a/b/c")
    otok.remove(slice.build .. "/a")
    assert(not otok.exists(slice.build .. "/a"))
    local all = otok.glob("**/*.c")
    assert(#all == 2 and all[1]:find("/source/src/a.c$") and all[2]:find("/source/src/sub/b.c$"), #all)
    local top = otok.glob("src/*.c")
    assert(#top == 1 and top[1]:find("/src/a.c$"))
    assert(#otok.glob("*.c", slice.build .. "/src-copy/sub") == 1)
    assert(#otok.glob("s?c/**") == 3)  -- src/a.c, src/sub, src/sub/b.c
    assert(otok.join("a", "b/", "/c", "", "d") == "a/b/c/d")
    otok.log("logged", 42)
    otok.warn("check this")
    print("printed")
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(read_file(dir.path() / "source" / "CMakeLists.txt").value_or("?"), "project(x)\n");
    EXPECT_EQ(out->warnings, std::vector<std::string>{"check this"});
    EXPECT_NE(log().find("logged\t42\nwarning: check this\nprinted\n"), std::string::npos) << log();
}

TEST_F(build_script, files_are_confined)
{
    test::temp_dir outside;
    outside.write("secret.txt", "secret\n");
    std::string const secret = path_to_utf8((outside.path() / "secret.txt").generic_u8string());
    struct refused
    {
        std::string call, message;
    };
    for (refused const& r : std::vector<refused>{
             {"otok.read('" + secret + "')", "is outside the directories build.lua may read"},
             {"otok.exists('../../" + path_to_utf8(outside.path().filename()) + "/secret.txt')", "is outside the directories build.lua may read"},
             {"otok.write(slice.recipe .. '/x', 'x')", "is outside the directories build.lua may write"},
             {"otok.write(deps.zlib.include .. '/x.h', 'x')", "is outside the directories build.lua may write"},
             {"otok.copy('" + secret + "', 'x')", "is outside the directories build.lua may read"},
             {"otok.remove(slice.source)", "is a directory of the slice itself"},
             {"otok.run({'cc'}, {cwd = '" + secret + "'})", "is outside the directories build.lua may read"},
             {"otok.glob('*', '" + path_to_utf8(outside.path().generic_u8string()) + "')", "is outside the directories build.lua may read"},
         }) {
        auto out = run("function build(slice, options, deps)\n    " + r.call + "\nend\n");
        ASSERT_FALSE(out.has_value()) << r.call;
        EXPECT_NE(out.error().message.find("build.lua:8: "), std::string::npos) << out.error().message;
        EXPECT_NE(out.error().message.find(r.message), std::string::npos) << out.error().message;
    }
    // reading is allowed in the recipe, the resources and the dependencies
    dir.write("recipe/patch.txt", "p\n");
    dir.write("dep/include/z.h", "z\n");
    auto allowed = run(R"(
function build(slice, options, deps)
    assert(otok.read(slice.recipe .. "/patch.txt") == "p\n")
    assert(otok.read(deps.zlib.include .. "/z.h") == "z\n")
    assert(otok.exists(slice.resources.data))
    return finish(slice)
end
)");
    EXPECT_TRUE(allowed.has_value()) << allowed.error().message;

    // a symbolic link is resolved before the check
    std::error_code ec;
    std::filesystem::create_directory_symlink(outside.path(), dir.path() / "source" / "link", ec);
    if (ec) GTEST_SKIP() << "cannot create symbolic links here: " << ec.message();
    auto linked = run("function build(slice)\n    otok.read('link/secret.txt')\nend\n");
    ASSERT_FALSE(linked.has_value());
    EXPECT_NE(linked.error().message.find("'link/secret.txt' is outside the directories build.lua may read"), std::string::npos)
        << linked.error().message;
}

TEST_F(build_script, checks_the_returned_table)
{
    struct returned
    {
        std::string value, message;
    };
    for (returned const& r : std::vector<returned>{
             {"42", "build.lua: build returned number, not a table"},
             {"{ headers = slice.prefix }", "build.lua: build returned no 'libraries'"},
             {"{ libraries = {} }", "build.lua: build returned no 'headers'"},
             {"{ libraries = {}, headers = slice.prefix, extra = 1 }", "build.lua: build returned the unknown field 'extra'"},
             {"{ libraries = { p = slice.prefix .. '/lib/missing.a' }, headers = slice.prefix }", "does not exist"},
             {"{ libraries = { p = slice.build }, headers = slice.prefix }", "is not inside slice.prefix"},
             {"{ libraries = { 'x' }, headers = slice.prefix }", "build.lua: libraries must be a table of name = path"},
             {"{ libraries = {}, headers = 5 }", "build.lua: headers must be a path, not number"},
             {"{ libraries = { p = { runtime = slice.prefix } }, headers = slice.prefix }", "build.lua: libraries.p has no 'link'"},
             {"{ libraries = { p = { link = slice.prefix, dll = 1 } }, headers = slice.prefix }",
                 "build.lua: libraries.p has the unknown field 'dll'; it takes link, runtime and symbols"},
             {"{ libraries = { p = { link = slice.prefix, symbols = slice.prefix } }, headers = slice.prefix }",
                 "build.lua: libraries.p has 'symbols' without 'runtime'"},
             {"{ libraries = { p = { link = slice.prefix, runtime = slice.build } }, headers = slice.prefix }",
                 "libraries.p.runtime"},
         }) {
        auto out = run("function build(slice, options, deps)\n    finish(slice)\n    return " + r.value + "\nend\n");
        ASSERT_FALSE(out.has_value()) << r.value;
        EXPECT_NE(out.error().message.find(r.message), std::string::npos) << r.value << ": " << out.error().message;
    }

    // a tool recipe's programs (recipe-toml.md 3.1), alone or with libraries
    auto tool = run(R"(
function build(slice)
    otok.write(slice.prefix .. "/bin/ninja.exe", "ninja")
    return { programs = { ninja = slice.prefix .. "/bin/ninja.exe" } }
end
)");
    ASSERT_TRUE(tool.has_value()) << tool.error().message;
    EXPECT_EQ(tool->programs.at("ninja").filename(), "ninja.exe");
    EXPECT_TRUE(tool->libraries.empty());
    // a script and the toolchain that runs it (docs/PLAN.md decision 70)
    auto script = run(R"(
function build(slice)
    otok.write(slice.prefix .. "/meson.py", "# meson")
    return { programs = { meson = { script = slice.prefix .. "/meson.py", interpreter = "python" } } }
end
)");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    EXPECT_EQ(script->programs.at("meson").filename(), "meson.py");
    EXPECT_EQ(script->interpreters.at("meson"), "python");
    for (returned const& r : std::vector<returned>{
             {"{ meson = { script = slice.prefix .. '/meson.py' } }", "build.lua: programs.meson.interpreter must name a toolchain of the recipe"},
             {"{ meson = { interpreter = 'python' } }", "build.lua: programs.meson has no 'script'"},
             {"{ meson = { script = slice.prefix .. '/meson.py', interpreter = 'python', args = 1 } }",
                 "build.lua: programs.meson has the unknown field 'args'; it takes script and interpreter"},
         }) {
        auto out = run("function build(slice)\n    otok.write(slice.prefix .. '/meson.py', '#')\n    return { programs = " + r.value + " }\nend\n");
        ASSERT_FALSE(out.has_value()) << r.value;
        EXPECT_NE(out.error().message.find(r.message), std::string::npos) << r.value << ": " << out.error().message;
    }

    auto not_a_file = run("function build(slice)\n    otok.mkdir(slice.prefix .. '/bin')\n    return { programs = { ninja = slice.prefix .. '/bin' } }\nend\n");
    ASSERT_FALSE(not_a_file.has_value());
    EXPECT_NE(not_a_file.error().message.find("build.lua: programs.ninja is not a file"), std::string::npos) << not_a_file.error().message;

    auto per_library = run(R"(
function build(slice)
    local out = finish(slice)
    otok.write(slice.prefix .. "/module.modulemap", "module p {}\n")
    return { libraries = out.libraries, headers = { p = out.headers }, module_map = slice.prefix .. "/module.modulemap" }
end
)");
    ASSERT_TRUE(per_library.has_value()) << per_library.error().message;
    EXPECT_FALSE(per_library->headers.has_value());
    EXPECT_EQ(per_library->library_headers.at("p").filename(), "include");
    EXPECT_EQ(per_library->module_map->filename(), "module.modulemap");

    // a DLL with its import library and .pdb (build-lua-api.md 8)
    auto dll = run(R"(
function build(slice)
    for _, f in ipairs{ "lib/p.lib", "bin/p.dll", "bin/p.pdb" } do otok.write(slice.prefix .. "/" .. f, "x") end
    return { libraries = { p = { link = slice.prefix .. "/lib/p.lib", runtime = slice.prefix .. "/bin/p.dll", symbols = slice.prefix .. "/bin/p.pdb" } },
        headers = slice.prefix }
end
)");
    ASSERT_TRUE(dll.has_value()) << dll.error().message;
    EXPECT_EQ(dll->libraries.at("p").filename(), "p.lib");
    EXPECT_EQ(dll->runtimes.at("p").filename(), "p.dll");
    EXPECT_EQ(dll->symbols.at("p").filename(), "p.pdb");

    auto missing = run("x = 1\n");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "build.lua: defines no function 'build'");
}

TEST_F(build_script, build_tools)
{
    auto none = run("function build(slice) return finish(slice) end\n", build_entry::build_tools);
    EXPECT_TRUE(none.has_value()) << none.error().message;

    slice.is_tool = true;
    auto tool = run("function build_tools(slice)\n    assert(slice.is_tool)\n    otok.run{'make', 'tools'}\nend\n", build_entry::build_tools);
    ASSERT_TRUE(tool.has_value()) << tool.error().message;
    ASSERT_EQ(specs.size(), 1u);

    slice.is_tool = false;
    slice.tools_prefix = base / "tools-prefix";
    dir.write("tools-prefix/bin/tool", "tool\n");
    auto target = run(R"(
function build(slice)
    assert(slice.tools.prefix:find("/tools%-prefix$"))
    assert(otok.read(slice.tools.prefix .. "/bin/tool") == "tool\n")
    return finish(slice)
end
)");
    EXPECT_TRUE(target.has_value()) << target.error().message;
}

#ifndef _WIN32
// A real build of a tiny C library with CMake and with make (docs/PLAN.md decision 18); a real
// Windows build needs the MSVC environment of the native target (phase 6).
TEST_F(build_script, builds_a_c_library)
{
    env_vars const environment = current_environment();
    for (char const* tool : {"cc", "make", "cmake"}) {
        auto program = find_program(tool, environment, dir.path());
        if (!program) GTEST_SKIP() << tool << " is not installed";
        slice.toolchains[tool] = *program;  // as otok chose them
    }
    slice.run = run_process;
    slice.toolchain.env = environment;
    slice.toolchain.env.erase("MAKEFLAGS");
    slice.deps.clear();
    dir.write("source/hello.h", "int hello(void);\n");
    dir.write("source/hello.c", "#include \"hello.h\"\nint hello(void) { return HELLO_VALUE; }\n");
    dir.write("source/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.16)\nproject(hello C)\nadd_library(hello STATIC hello.c)\n"
        "target_compile_definitions(hello PRIVATE HELLO_VALUE=${HELLO_VALUE})\n"
        "install(TARGETS hello ARCHIVE DESTINATION lib)\ninstall(FILES hello.h DESTINATION include)\n");
    dir.write("source/Makefile",
        "libhello.a: hello.o\n\tar rcs $@ hello.o\n"
        "hello.o: hello.c hello.h\n\tcc -DHELLO_VALUE=2 -c hello.c -o hello.o\n"
        "install: libhello.a\n\tmkdir -p $(PREFIX)/lib $(PREFIX)/include\n\tcp libhello.a $(PREFIX)/lib/\n\tcp hello.h $(PREFIX)/include/\n");

    auto cmake = run(R"(
function build(slice)
    otok.cmake{ generator = "Unix Makefiles", defines = { HELLO_VALUE = 1 } }
    return { libraries = { hello = slice.prefix .. "/lib/libhello.a" }, headers = slice.prefix .. "/include" }
end
)");
    ASSERT_TRUE(cmake.has_value()) << cmake.error().message << "\n" << log();
    EXPECT_TRUE(std::filesystem::is_regular_file(cmake->libraries.at("hello")));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "prefix" / "include" / "hello.h"));

    std::filesystem::remove_all(dir.path() / "prefix");
    std::filesystem::create_directories(dir.path() / "prefix");
    auto make = run(R"(
function build(slice)
    otok.make{ "install", args = { "PREFIX=" .. slice.prefix } }
    return { libraries = { hello = slice.prefix .. "/lib/libhello.a" }, headers = slice.prefix .. "/include" }
end
)");
    ASSERT_TRUE(make.has_value()) << make.error().message << "\n" << log();
    EXPECT_TRUE(std::filesystem::is_regular_file(make->libraries.at("hello")));
}
#endif


TEST_F(build_script, cmake_takes_flags_from_the_toolchain_file)
{
    slice.cmake_toolchain = base / "toolchain.cmake";
    slice.toolchain.env["CFLAGS"] = "-O2";
    slice.toolchain.env["LDFLAGS"] = "-Wl,-x";
    auto out = run(R"(
function build(slice, options, deps)
    otok.cmake{ install = false }
    otok.cmake{ install = false, env = { CFLAGS = "-DMINE" } }
    otok.make{}
    return finish(slice)
end
)");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    ASSERT_EQ(specs.size(), 5u);
    EXPECT_FALSE(specs[0].env.contains("CFLAGS"));  // inherited: the toolchain file has them
    EXPECT_FALSE(specs[1].env.contains("LDFLAGS"));
    EXPECT_EQ(specs[2].env.at("CFLAGS"), "-DMINE");  // given by the call: kept
    EXPECT_EQ(specs[4].env.at("CFLAGS"), "-O2");     // make takes them from the environment

    // without a toolchain file CMake has nowhere else to take them from
    specs.clear();
    slice.cmake_toolchain.clear();
    out = run("function build(slice)\n    otok.cmake{ install = false }\n    return finish(slice)\nend\n");
    ASSERT_TRUE(out.has_value()) << out.error().message;
    EXPECT_EQ(specs[0].env.at("CFLAGS"), "-O2");
}
