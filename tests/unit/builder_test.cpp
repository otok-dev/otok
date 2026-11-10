// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The builder (docs/PLAN.md decision 19) through `otok install` on a native project, with a fake
// toolchain: build.lua writes its "libraries" itself, so no compiler is needed.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "otok/builder/builder.hpp"
#include "otok/builder/dev_tree.hpp"
#include "otok/commands/commands.hpp"
#include "otok/core/build_info.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"
#include "otok/core/process.hpp"
#include "otok/core/sandbox.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/packager/packager.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "otok/target/windows/windows_target.hpp"
#include "otok/version/constraint.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

target_info const* const native_targets[] = {&test::native_info()};

// A second target of the same compiler, which builds no tools: a project of several target tables
target_info const& other_info()
{
    static target_info const other = [] {
        target_info t = test::native_info();
        t.name = "other";
        t.platforms.front().name = "other";
        t.host_system = "";
        t.tool_platform = "";
        return t;
    }();
    return other;
}

// `other` with two compilers of the machine's system and one ABI: a library whose recipe builds with `two` only is built there for
// a graph of `one` (docs/PLAN.md decision 91)
target_info const& abi_info()
{
    static target_info const abi = [] {
        target_info t = other_info();
        std::string const system = machine_environment("").system;
        t.environments = {{system, "", "one"}, {system, "", "two"}};
        t.any_host_compilers = {};
        t.abis = {{"one", "x"}, {"two", "x"}};
        return t;
    }();
    return abi;
}

std::string machine_arch()
{
    return test::native_info().platforms.front().architectures.front();
}

struct fake_toolchain
{
    std::string version = "1";
    bool reject = false;
    int profiles = 0;
    std::string tools;                          // tools_path
    env_vars env;                               // in the environment of the profiles
    std::vector<std::string> products;          // "<library>.<product>: <libraries>", as packaged
    std::set<std::string> bundled;              // the packages whose data files are to be bundled
    std::optional<std::set<std::string>> system;  // build_target::system_library; none: every library is the system's
    std::vector<std::string> problems;          // build_target::output_problems
    std::vector<std::string> unchecked;         // and its warnings: what it could not check
};

class fake_target final : public build_target
{
public:
    explicit fake_target(fake_toolchain& t, target_info const& info = test::native_info(), std::set<std::string> lacking = {})
        : t_{t}, info_{info}, lacking_{std::move(lacking)}
    {
    }

    target_info const& info() const override { return info_; }
    std::string tool_arch() const override { return machine_arch(); }
    result<std::string> tools_path() override { return t_.tools; }

    // the compilers it is told it lacks
    result<void> find_compiler(build_environment const& e) override
    {
        if (lacking_.contains(e.compiler)) return fail("no " + e.compiler + " here");
        return {};
    }

    result<toolchain_profile> profile(slice_spec const&) override
    {
        ++t_.profiles;
        toolchain_profile p;
        p.triple = "fake-triple";
        p.identity["fake"] = t_.version;
        p.description = "the fake toolchain " + t_.version;
        p.toolchain.env = t_.env;
        p.toolchain.env["PATH"] = "";
        p.toolchain.cflags = {"-fake"};
        p.cmake_toolchain = "# the fake toolchain\n";
        return p;
    }

    result<void> check_library(std::filesystem::path const&, slice_spec const&) override
    {
        if (t_.reject) return fail("not for this architecture");
        return {};
    }

    bool system_library(std::string const& loaded, slice_spec const&) const override { return !t_.system || t_.system->contains(loaded); }

    std::vector<std::string> output_problems(std::filesystem::path const&, std::span<output_package const>,
        std::map<std::string, std::string> const&, std::vector<std::string>& warnings) const override
    {
        warnings.insert(warnings.end(), t_.unchecked.begin(), t_.unchecked.end());
        return t_.problems;
    }

    // the native output generator: it needs no toolchain
    result<void> make_package(built_package const& p, recipe const& rec, std::span<output_product const> products,
        std::filesystem::path const& staging) override
    {
        for (output_product const& o : products) {
            std::string text = p.library + "." + o.source->name + ":";
            for (library_ref const& l : o.libraries) text += " " + l.package + "/" + l.name;
            t_.products.push_back(text);
        }
        if (p.bundle_data) t_.bundled.insert(p.library);
        return native_->make_package(p, rec, products, staging);
    }

private:
    fake_toolchain& t_;
    target_info const& info_;
    std::set<std::string> lacking_;
    std::unique_ptr<build_target> native_ = target::linux::make_build_target({});  // its plain layout
};

// A build.lua that "builds" the library `name` and checks what it is given.
std::string build_lua(std::string const& name, std::string const& checks = "")
{
    return "function build(slice, options, deps)\n" + checks + "    otok.write(slice.prefix .. '/lib/lib" + name
        + ".a', '!<arch>\\n')\n    otok.write(slice.prefix .. '/include/" + name + ".h', 'int " + name + ";\\n')\n"
        + "    return { libraries = { " + name + " = slice.prefix .. '/lib/lib" + name + ".a' }, headers = slice.prefix .. '/include' }\nend\n";
}

class builder_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        for (std::string const library : {"a", "b", "gen", "ninja"}) {
            std::filesystem::path const file = upstream.path() / (library + "-1.0.tar.gz");
            test::write_tar_gz(file, {{library + "-1.0/" + library + ".c", "int " + library + ";\n"}, {library + "-1.0/LICENSE", "The license of " + library + ".\r\n"}});
            urls[library] = file_url(file);
            hashes[library] = sha256_file(file).value_or("?");
        }
        project.write("otok.toml", manifest);
        write_recipe("a", "1.0", build_lua("a", "    assert(otok.read(deps.b.include .. '/b.h') == 'int b;\\n')\n"),
            "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
        write_recipe("b", "2.0", build_lua("b"), options_of_b);
    }

    void write_recipe(std::string const& library, std::string const& version, std::string const& script, std::string const& extra = "",
        std::string const& source_extra = "")
    {
        std::string const dir = "recipes/" + library + "/";
        project.write(dir + "recipe.toml",
            "[recipe]\napi = 1\nlibrary = \"" + library + "\"\nname = \"" + library + "\"\nversion = \"" + version
            + "\"\nrevision = 1\nlicense = \"MIT\"\nlicense-files = [\"LICENSE\"]\ndescription = \"Test.\"\n"
            + "[source]\nurl = \"" + urls[library] + "\"\nsha256 = \"" + hashes[library] + "\"\n" + source_extra
            + "[platforms]\nsupported = [\"native\"]\nbuild-on = [\"*/cc\"]\n"
            + "[[product]]\nname = \"P\"\nlibraries = [\"" + library + "\"]\n" + extra);
        project.write(dir + "build.lua", script);
    }

    int run(std::vector<std::string> const& args)
    {
        out.str({});
        err.str({});
        auto parsed = parse_command_line(args, global_options(), command_specs());
        if (!parsed) {
            err << parsed.error().message;
            return 2;
        }
        std::filesystem::path const cache_dir = cache.path();
        auto const extra = env;
        environment const environment = [cache_dir, extra](std::string_view name) -> std::optional<std::string> {
            if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache_dir);
            if (auto it = extra.find(std::string{name}); it != extra.end()) return it->second;
            return std::nullopt;
        };
        target_info const* const both_targets[] = {&test::native_info(), second};
        command_context ctx{project.path(), with_other ? std::span<target_info const* const>{both_targets} : native_targets, nullptr, out, err,
            environment};
        ctx.terminal = terminal;
        ctx.tool_roots = tool_roots;
        ctx.nested = nested;
        ctx.make_build_targets = [this](machine_config const&) {
            std::vector<std::unique_ptr<build_target>> targets;
            targets.push_back(std::make_unique<fake_target>(toolchain));
            if (with_other && second_built) targets.push_back(std::make_unique<fake_target>(toolchain, *second, second_lacks));
            return targets;
        };
        // the toolchains' programs answer by name (docs/PLAN.md decision 38)
        ctx.probe_version = [this](std::filesystem::path const& program, std::vector<std::string> const&) -> result<std::string> {
            ++probes;
            auto const it = versions.find(path_to_utf8(program.stem()));
            if (it == versions.end()) return fail("not a fake program");
            return it->second;
        };
        return run_command(*parsed, ctx);
    }

    std::filesystem::path slices_of(std::string const& library, std::string const& version) const
    {
        return cache.path() / "builds" / library / (version + "-r1") / "slices";
    }

    std::size_t entries(std::filesystem::path const& dir) const
    {
        std::error_code ec;
        std::size_t n = 0;
        for (auto const& e : std::filesystem::directory_iterator{dir, ec}) n += e.is_directory() ? 1 : 0;
        return n;
    }

    std::string const options_of_b = "[options.level]\ntype = \"int\"\ndefault = 1\ndoc = \"d\"\n";
    std::string const manifest = "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\n"
                                 "[deps.a]\nrecipe = { path = \"recipes/a\" }\n[deps.b]\nrecipe = { path = \"recipes/b\" }\n";

    test::temp_dir project;
    test::temp_dir cache;
    test::temp_dir upstream;
    std::map<std::string, std::string> urls, hashes, env;
    std::map<std::string, std::string> versions;  // fake program → what its version command prints
    int probes = 0;
    bool tool_roots = false;                    // command_context::tool_roots: the project's [tool-deps] built as roots
    bool nested = false;                        // command_context::nested: an install of otok publish
    bool with_other = false;                    // the target `other` besides `native` (other_info)
    target_info const* second = &other_info();  // that target
    bool second_built = true;                   // whether this machine has a build target for it
    std::set<std::string> second_lacks;         // the compilers that target does not find
    fake_toolchain toolchain;
    std::ostringstream out, err;
    terminal_caps terminal;                     // live: the display draws the slices under way
};

}

namespace {

// A target of every host that finds only the compilers it is told to.
class environment_target final : public build_target
{
public:
    explicit environment_target(std::set<std::string> compilers) : compilers_{std::move(compilers)} {}

    target_info const& info() const override { return target::windows::info(); }
    std::string tool_arch() const override { return machine_arch(); }
    result<toolchain_profile> profile(slice_spec const&) override { return fail("not built"); }
    result<void> make_package(built_package const&, recipe const&, std::span<output_product const>, std::filesystem::path const&) override
    {
        return {};
    }

    result<void> find_compiler(build_environment const& e) override
    {
        if (compilers_.contains(e.compiler)) return {};
        return fail("no " + e.compiler);
    }

private:
    std::set<std::string> compilers_;
};

std::vector<build_environment> environments_of(std::initializer_list<char const*> texts)
{
    std::vector<build_environment> envs;
    for (char const* text : texts) envs.push_back(*parse_environment(text));
    return envs;
}

}

TEST(machine_environment, is_the_first_the_machine_has)
{
    std::string const system = machine_environment("").system;
    std::string const other = system == "linux" ? "windows" : "linux";
    environment_target both{{"msvc", "clang-cl"}};
    auto e = machine_environment_of(both, environments_of({(other + "/clang-cl").c_str(), (system + "/msvc").c_str(), (system + "/clang-cl").c_str()}));
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->text(), system + "/msvc");  // the system's, without its architecture

    // with the machine's architecture, or another's
    std::string const arch = machine_environment("").arch;
    std::string const other_arch = arch == "arm64" ? "x86_64" : "arm64";
    e = machine_environment_of(both, environments_of({(system + "-" + other_arch + "/msvc").c_str(), (system + "-" + arch + "/clang-cl").c_str()}));
    ASSERT_TRUE(e.has_value()) << e.error().message;
    EXPECT_EQ(e->text(), system + "/clang-cl");

    // a compiler the machine has not is passed over, and named when none is left
    environment_target clang_cl{{"clang-cl"}};
    e = machine_environment_of(clang_cl, environments_of({(system + "/msvc").c_str(), (system + "/clang-cl").c_str()}));
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->compiler, "clang-cl");
    environment_target none{{}};
    e = machine_environment_of(none, environments_of({(system + "/msvc").c_str(), (other + "/clang-cl").c_str()}));
    ASSERT_FALSE(e.has_value());
    EXPECT_EQ(e.error().message, "this project builds 'windows' in " + system + "/msvc, " + other + "/clang-cl; this machine is "
        + std::string{host_machine()} + "; " + system + "/msvc: no msvc");

    // a target without environments: nothing to choose
    e = machine_environment_of(none, {});
    ASSERT_TRUE(e.has_value());
    EXPECT_TRUE(e->compiler.empty());
}

// What App Store review would reject in the output (build_target::output_problems, otok-toml.md 4.4, 4.5): warnings of
// otok install, errors of the installs of otok publish
TEST_F(builder_test, output_problems_warn_install_and_fail_publish)
{
    toolchain.problems = {"the library 'a' of 'a' uses required-reason APIs that no privacy manifest of its products declares: stat "
                          "(NSPrivacyAccessedAPICategoryFileTimestamp)"};
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(err.str().find("warning: the output 'Otok': the library 'a' of 'a' uses required-reason APIs that no privacy manifest of its "
        "products declares: stat (NSPrivacyAccessedAPICategoryFileTimestamp)\n"), std::string::npos) << err.str();
    EXPECT_NE(out.str().find("output 'Otok': "), std::string::npos) << out.str();

    // the install of otok publish
    nested = true;
    EXPECT_NE(run({"install"}), 0);
    EXPECT_NE(err.str().find("error: the output 'Otok': the library 'a' of 'a' uses required-reason APIs that no privacy manifest of its "
        "products declares: stat (NSPrivacyAccessedAPICategoryFileTimestamp)"), std::string::npos) << err.str();
    EXPECT_EQ(err.str().find("warning: the output"), std::string::npos) << err.str();

    // a check it could not do stays a warning, of publish too
    toolchain.problems.clear();
    toolchain.unchecked = {"cannot read the exports of ICU (libicucore) in the SDK 'iphoneos': it is not a text stub in the YAML format (tbd version 4)"};
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(err.str().find("warning: the output 'Otok': cannot read the exports of ICU (libicucore) in the SDK 'iphoneos': it is not a text stub in "
        "the YAML format (tbd version 4)\n"), std::string::npos) << err.str();
}

// Several target tables: a package of each target, built with the dependencies of its own graph and put in the output of
// its target (docs/PLAN.md decision 82)
TEST_F(builder_test, builds_a_package_for_each_target)
{
    with_other = true;
    for (std::string const library : {"a", "b"}) {
        std::string text = read_file(project.path() / "recipes" / library / "recipe.toml").value_or("");
        auto const replace = [&text](std::string const& from, std::string const& to) { text.replace(text.find(from), from.size(), to); };
        replace("supported = [\"native\"]", "supported = [\"native\", \"other\"]");
        replace("description = \"Test.\"\n", "description = \"Test.\"\nvariant = \"all\"\n");
        project.write("recipes/" + library + "/recipe.toml", text);
    }
    project.write("otok.toml", manifest + "[other]\nplatforms = [\"other\"]\noutput = \"OtokOther\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 4 (0 from the cache)"), std::string::npos) << out.str();
    EXPECT_EQ(entries(slices_of("a", "1.0")), 2u);
    EXPECT_EQ(entries(slices_of("b", "2.0")), 2u);
    for (char const* output : {"Otok", "OtokOther"}) {
        for (char const* library : {"a", "b"}) EXPECT_TRUE(std::filesystem::exists(project.path() / output / library)) << output << "/" << library;
    }
    std::string const lock = read_file(project.path() / "otok.lock").value_or("?");
    EXPECT_NE(lock.find("library = \"a\"\ntarget = \"other\"\nbuild-on = [\"*/cc\"]\n"), std::string::npos) << lock;
    EXPECT_NE(lock.find("library = \"a\"\ntarget = \"native\"\nbuild-on = [\"*/cc\"]\n"), std::string::npos) << lock;

    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 4 (4 from the cache)"), std::string::npos) << out.str();
}

TEST(machine_environment, of_tool_slices)
{
    // the first environment of the target of the system, unless tool-build-on names another
    manifest m;
    EXPECT_EQ(m.tool_environment(target::linux::info()).text(), "linux/gcc");
    EXPECT_EQ(m.tool_environment(target::windows::info()).text(), "windows/msvc");
    EXPECT_EQ(m.tool_environment(test::native_info()).text(), machine_environment("").system + "/cc");
    m.tool_build_on = environments_of({"linux-arm64/clang"});
    EXPECT_EQ(m.tool_environment(target::linux::info()).text(), "linux/clang");  // without the architecture
    EXPECT_EQ(m.tool_environment(target::windows::info()).text(), "windows/msvc");
}

namespace {

// The fixture's recipes of a and b for `native` and `other`, with the environments `other_build_on` gives a library after
// `*/cc` in its `build-on`.
void for_both_targets(test::temp_dir& project, std::map<std::string, std::string> const& other_build_on = {})
{
    for (std::string const library : {"a", "b"}) {
        std::string text = read_file(project.path() / "recipes" / library / "recipe.toml").value_or("");
        auto const replace = [&text](std::string const& from, std::string const& to) { text.replace(text.find(from), from.size(), to); };
        replace("supported = [\"native\"]", "supported = [\"native\", \"other\"]");
        replace("description = \"Test.\"\n", "description = \"Test.\"\nvariant = \"all\"\n");
        if (auto it = other_build_on.find(library); it != other_build_on.end()) {
            replace("build-on = [\"*/cc\"]", "build-on = [\"*/cc\", \"" + it->second + "\"]");
        }
        project.write("recipes/" + library + "/recipe.toml", text);
    }
}

// A recipe for `other` only.
void only_for_other(test::temp_dir& project, std::string const& library)
{
    std::filesystem::path const file = project.path() / "recipes" / library / "recipe.toml";
    std::string text = read_file(file).value_or("");
    std::string const from = "supported = [\"native\"]";
    text.replace(text.find(from), from.size(), "supported = [\"other\"]");
    project.write("recipes/" + library + "/recipe.toml", text);
}

}

// docs/PLAN.md decision 100: the target tables this machine can build are built, the others skipped with a note each; the
// output of a skipped one is left as it is, and --locked checks the whole lock
TEST_F(builder_test, skips_a_target_this_machine_has_none_for)
{
    with_other = true;
    for_both_targets(project);
    write_recipe("gen", "1.0", build_lua("gen"));
    only_for_other(project, "gen");
    std::string const both = manifest + "[other]\nplatforms = [\"other\"]\noutput = \"OtokOther\"\n";
    std::string const gen = "[deps.gen]\nrecipe = { path = \"recipes/gen\" }\nplatforms = [\"other\"]\n";
    project.write("otok.toml", both + gen);

    // both built first: gen in OtokOther only
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(project.path() / "OtokOther" / "gen"));
    std::string const state = read_file(project.path() / "OtokOther" / ".otok-output.toml").value_or("?");

    // no target for `other` on this machine: native is built, OtokOther left with gen, which the project no longer has
    second_built = false;
    project.write("otok.toml", both);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("note: not building for 'other': this otok does not build it on this machine\n"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("slices: 2 (2 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("output 'Otok': "), std::string::npos) << out.str();
    EXPECT_EQ(out.str().find("output 'OtokOther'"), std::string::npos) << out.str();
    EXPECT_EQ(err.str().find("error"), std::string::npos) << err.str();
    for (char const* library : {"a", "b", "gen"}) EXPECT_TRUE(std::filesystem::exists(project.path() / "OtokOther" / library)) << library;
    EXPECT_EQ(read_file(project.path() / "OtokOther" / ".otok-output.toml").value_or("?"), state);
    // the lock keeps both graphs
    std::string const lock = read_file(project.path() / "otok.lock").value_or("?");
    EXPECT_NE(lock.find("library = \"a\"\ntarget = \"other\"\n"), std::string::npos) << lock;
    EXPECT_NE(lock.find("library = \"a\"\ntarget = \"native\"\n"), std::string::npos) << lock;

    // the output of the target built is written anew
    std::filesystem::remove_all(project.path() / "Otok");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(project.path() / "Otok" / "a"));

    // --locked checks the graph of `other` too
    project.write("otok.toml", both + gen);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    write_recipe("gen", "1.1", build_lua("gen"));
    only_for_other(project, "gen");
    EXPECT_EQ(run({"install", "--locked"}), 1);
    EXPECT_NE(err.str().find("error: otok.lock is out of date (--locked):\n  gen@other: 1.0 (r1) -> 1.1 (r1)\n"), std::string::npos) << err.str();
}

// docs/PLAN.md decision 100: a target whose environments this machine lacks is skipped, the note naming them
TEST_F(builder_test, skips_a_target_without_its_environments)
{
    with_other = true;
    second_lacks = {"cc"};
    for_both_targets(project);
    project.write("otok.toml", manifest + "[other]\nplatforms = [\"other\"]\nbuild-on = [\"*/cc\"]\noutput = \"OtokOther\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const system = machine_environment("").system;
    EXPECT_NE(out.str().find("note: not building for 'other': this project builds 'other' in */cc; this machine is "
        + std::string{host_machine()} + "; " + system + "/cc: no cc here\n"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_TRUE(std::filesystem::exists(project.path() / "Otok" / "a"));
    EXPECT_FALSE(std::filesystem::exists(project.path() / "OtokOther"));
}

// docs/PLAN.md decisions 91, 100: a library built in a substitute environment this machine lacks makes its target skipped
TEST_F(builder_test, skips_a_target_without_the_environment_of_a_library)
{
    with_other = true;
    second = &abi_info();
    second_lacks = {"two"};
    std::string const system = machine_environment("").system;
    for_both_targets(project, {{"a", system + "/one"}, {"b", system + "/two"}});
    project.write("otok.toml", manifest + "[other]\nplatforms = [\"other\"]\nbuild-on = [\"" + system + "/one\"]\noutput = \"OtokOther\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("note: not building for 'other': b@other is built in " + system + "/two (its recipe does not build in " + system
        + "/one), which this machine does not have: no two here\n"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_TRUE(std::filesystem::exists(project.path() / "Otok" / "a"));
    EXPECT_FALSE(std::filesystem::exists(project.path() / "OtokOther"));

    // with that compiler, both are built
    second_lacks.clear();
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(out.str().find("note:"), std::string::npos) << out.str();
    EXPECT_TRUE(std::filesystem::exists(project.path() / "OtokOther" / "b"));
}

// docs/PLAN.md decision 100: a machine that can build none of the target tables is an error naming each, after the lock is
// written and the inputs fetched (otok-toml.md 4.9)
TEST_F(builder_test, builds_nothing_without_a_target_it_can_build)
{
    with_other = true;
    second_built = false;
    for_both_targets(project);
    std::string const system = machine_environment("").system;
    std::string const other = system == "linux" ? "windows" : "linux";
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\nbuild-on = [\"" + other + "/cc\"]\n"
        "[other]\nplatforms = [\"other\"]\noutput = \"OtokOther\"\n"
        "[deps.a]\nrecipe = { path = \"recipes/a\" }\n[deps.b]\nrecipe = { path = \"recipes/b\" }\n");
    ASSERT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("error: this machine can build none of the target tables of this project:\n"
        "  'native': this project builds 'native' in " + other + "/cc; this machine is " + std::string{host_machine()} + "\n"
        "  'other': this otok does not build it on this machine\n"), std::string::npos) << err.str();
    EXPECT_NE(out.str().find("otok.lock created:\n"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("inputs in the cache: 2 sources\n"), std::string::npos) << out.str();
    EXPECT_EQ(out.str().find("note:"), std::string::npos) << out.str();
    EXPECT_TRUE(std::filesystem::exists(project.path() / "otok.lock"));
    EXPECT_FALSE(std::filesystem::exists(project.path() / "Otok"));

    // an environment of the machine among them: built
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\nbuild-on = [\"" + other + "/cc\", \"" + system + "/cc\"]\n"
        "[deps.a]\nrecipe = { path = \"recipes/a\" }\n[deps.b]\nrecipe = { path = \"recipes/b\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
}

TEST_F(builder_test, builds_and_reuses_slices)
{
    std::string const slice = "native-" + machine_arch();
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    std::string const e = err.str();
    EXPECT_NE(e.find("toolchain of 'native': the fake toolchain 1\n"), std::string::npos) << e;
    EXPECT_LT(e.find("building b 2.0 for " + slice), e.find("building a 1.0 for " + slice)) << e;  // dependencies first
    EXPECT_LT(e.find("built b 2.0 for " + slice + " ("), e.find("building a 1.0 for " + slice)) << e;  // after it is built

    ASSERT_EQ(entries(slices_of("b", "2.0")), 1u);
    std::filesystem::path entry;
    for (auto const& d : std::filesystem::directory_iterator{slices_of("b", "2.0")}) {
        if (d.is_directory()) entry = d.path();  // not its lock file
    }
    EXPECT_EQ(entry.filename().string().size(), 32u);
    EXPECT_EQ(read_file(entry / "prefix" / "lib" / "libb.a").value_or("?"), "!<arch>\n");
    std::string const key = read_file(entry / "key.txt").value_or("?");
    EXPECT_NE(key.find("library = b\n"), std::string::npos) << key;
    EXPECT_NE(key.find("option.level = 1\n"), std::string::npos) << key;
    EXPECT_NE(key.find("slice = " + slice + "\n"), std::string::npos) << key;
    EXPECT_NE(key.find("toolchain.fake = 1\n"), std::string::npos) << key;
    EXPECT_EQ(sha256_hex(key).substr(0, 32), entry.filename().string());
    EXPECT_NE(read_file(entry / "result.toml").value_or("?").find("\"b\" = \"lib/libb.a\""), std::string::npos)
        << read_file(entry / "result.toml").value_or("?");
    EXPECT_TRUE(std::filesystem::exists(entry / "build.log"));

    // the same inputs: nothing is built
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (2 from the cache)\n"), std::string::npos) << out.str();

    // another toolchain rebuilds everything; an option of b rebuilds b and a, which depends on it
    toolchain.version = "2";
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    project.write("otok.toml", manifest + "options = { level = 2 }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_EQ(entries(slices_of("b", "2.0")), 3u);
    EXPECT_EQ(entries(slices_of("a", "1.0")), 3u);

    // the toolchain was discovered for every slice it built; fetch builds nothing
    toolchain.profiles = 0;
    ASSERT_EQ(run({"fetch"}), 0) << err.str();
    EXPECT_EQ(toolchain.profiles, 0);
}

TEST_F(builder_test, reports_failures)
{
    std::string const slice = "native-" + machine_arch();
    write_recipe("b", "2.0", "function build(slice)\n    otok.log('step one')\n    error('no luck')\nend\n", options_of_b);
    EXPECT_EQ(run({"install"}), 1);
    std::string const e = err.str();
    EXPECT_NE(e.find("error: b 2.0 for " + slice + ": b/build.lua:3: no luck"), std::string::npos) << e;
    EXPECT_NE(e.find("the end of the log:\nstep one\n"), std::string::npos) << e;
    EXPECT_EQ(e.find("the work directory is kept"), std::string::npos) << e;
    EXPECT_EQ(entries(slices_of("b", "2.0")), 0u);  // nothing half-built in the cache
    EXPECT_EQ(e.find("building a 1.0"), std::string::npos) << e;  // nothing starts after a failure

    // the whole log is kept, with the key it was built for (cache.md 7.1)
    EXPECT_NE(e.find("the whole log is in '"), std::string::npos) << e;
    std::filesystem::path const failed = cache.path() / "failed";
    auto const kept = [&] {
        std::vector<std::filesystem::path> dirs;
        for (auto const& d : std::filesystem::directory_iterator{failed}) dirs.push_back(d.path());
        return dirs;
    };
    ASSERT_EQ(kept().size(), 1u);
    std::filesystem::path const log_only = kept().front();
    EXPECT_EQ(log_only.filename().string().find("b-" + slice + "-"), 0u);
    EXPECT_NE(read_file(log_only / "build.log").value_or("?").find("step one\n"), std::string::npos);
    EXPECT_NE(read_file(log_only / "key.txt").value_or("?").find("library = b\n"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(log_only / "source"));
    std::filesystem::remove_all(failed);

    // the whole work directory, by config.toml or for one run
    env["OTOK_KEEP_BUILD_DIRS"] = "1";
    EXPECT_EQ(run({"install"}), 1);
    env.erase("OTOK_KEEP_BUILD_DIRS");
    EXPECT_NE(err.str().find("the work directory is kept in '"), std::string::npos) << err.str();
    EXPECT_EQ(run({"install", "--keep-build-dirs"}), 1);
    EXPECT_NE(err.str().find("the work directory is kept in '"), std::string::npos) << err.str();
    ASSERT_EQ(kept().size(), 2u);
    for (std::filesystem::path const& d : kept()) {
        EXPECT_EQ(d.filename().string().find("b-" + slice + "-"), 0u);
        EXPECT_TRUE(std::filesystem::exists(d / "source" / "b.c"));
        EXPECT_TRUE(std::filesystem::exists(d / "key.txt"));
    }
}

// docs/PLAN.md decision 59: -v, what slices build with and the commands they run; -vv, their output.
TEST_F(builder_test, shows_what_slices_build_with)
{
    std::string const child = path_to_utf8(path_from_utf8(OTOK_TEST_CHILD).generic_u8string());
    std::string const id = "[b/native-" + machine_arch() + "] ";
    write_recipe("b", "2.0", build_lua("b", "    otok.run({'" + child + "', 'args', 'from-the-child'})\n"), options_of_b);
    ASSERT_EQ(run({"install", "-v"}), 0) << err.str();
    std::string e = err.str();
    EXPECT_NE(e.find(id + "options: level = 1\n"), std::string::npos) << e;
    EXPECT_NE(e.find(id + "cflags: -fake"), std::string::npos) << e;
    EXPECT_NE(e.find(id + "work directory: "), std::string::npos) << e;
    EXPECT_NE(e.find(id + "slice key: "), std::string::npos) << e;
    EXPECT_NE(e.find(id + "$ " + child + " args from-the-child  (in source)\n"), std::string::npos) << e;
    EXPECT_EQ(e.find("\nfrom-the-child\n"), std::string::npos) << e;  // the output stays in the log

    // -vv: the output as it comes, the commands as the log has them
    project.write("otok.toml", manifest + "options = { level = 2 }\n");
    ASSERT_EQ(run({"install", "-vv"}), 0) << err.str();
    e = err.str();
    EXPECT_NE(e.find("\n$ " + child + " args from-the-child\nfrom-the-child\n"), std::string::npos) << e;
    EXPECT_EQ(e.find(id + "$ "), std::string::npos) << e;

    EXPECT_EQ(run({"install", "--progress=fancy"}), 1);
    EXPECT_EQ(err.str(), "error: --progress must be 'auto' or 'plain', not 'fancy'\n");
    EXPECT_EQ(run({"install", "--progress=plain"}), 0) << err.str();

    // on a terminal the slices under way are drawn below the lines, which say only when each is built
    terminal.live = true;
    project.write("otok.toml", manifest + "options = { level = 3 }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    e = err.str();
    EXPECT_EQ(e.find("building b 2.0"), std::string::npos) << e;
    EXPECT_NE(e.find("built b 2.0 for native-" + machine_arch() + " ("), std::string::npos) << e;
    project.write("otok.toml", manifest + "options = { level = 4 }\n");
    ASSERT_EQ(run({"install", "--progress=plain"}), 0) << err.str();
    EXPECT_NE(err.str().find("building b 2.0"), std::string::npos) << err.str();
    EXPECT_EQ(err.str().find("\x1b["), std::string::npos) << err.str();
}

// `otok log` (docs/PLAN.md decision 59): the newest log of a library's builds in the cache, a failed one too.
TEST_F(builder_test, shows_the_logs_of_builds)
{
    std::string const slice = "native-" + machine_arch();
    write_recipe("b", "2.0", "function build(slice)\n    otok.log('step one')\n    error('no luck')\nend\n", options_of_b);
    EXPECT_EQ(run({"log", "b"}), 1);
    EXPECT_NE(err.str().find("error: no log of 'b' in the cache '"), std::string::npos) << err.str();
    EXPECT_EQ(run({"install"}), 1);
    ASSERT_EQ(run({"log", "b"}), 0) << err.str();
    EXPECT_NE(err.str().find("the log of b 2.0 " + slice + " release, failed, "), std::string::npos) << err.str();
    EXPECT_NE(out.str().find("step one\n"), std::string::npos) << out.str();

    write_recipe("b", "2.0", "function build(slice)\n    otok.log('step two')\n" + build_lua("b").substr(build_lua("b").find('\n') + 1),
        options_of_b);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    ASSERT_EQ(run({"log", "b", slice}), 0) << err.str();
    EXPECT_NE(err.str().find("the log of b 2.0 " + slice + " release, "), std::string::npos) << err.str();
    EXPECT_EQ(err.str().find("failed"), std::string::npos) << err.str();
    EXPECT_NE(out.str().find("step two\n"), std::string::npos) << out.str();

    ASSERT_EQ(run({"log", "b", "--list"}), 0) << err.str();
    std::string const listed = out.str();
    EXPECT_LT(listed.find("2.0 " + slice + " release, "), listed.find("2.0 " + slice + " release, failed, ")) << listed;
    EXPECT_EQ(run({"log", "b", "other-slice"}), 1);
    EXPECT_NE(err.str().find("error: no log of 'b' for 'other-slice'"), std::string::npos) << err.str();
}

TEST_F(builder_test, checks_libraries)
{
    write_recipe("b", "2.0",
        "function build(slice)\n    otok.write(slice.prefix .. '/lib/libb.a', 'not an archive')\n"
        "    return { libraries = { b = slice.prefix .. '/lib/libb.a' }, headers = slice.prefix }\nend\n",
        options_of_b);
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the library 'b' ("), std::string::npos) << err.str();
    EXPECT_NE(err.str().find(") is not a static archive"), std::string::npos) << err.str();

    write_recipe("b", "2.0", build_lua("b"), options_of_b);
    toolchain.reject = true;
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the library 'b': not for this architecture"), std::string::npos) << err.str();
}

// recipe-toml.md 16: every library of a product whose `when` holds is returned
TEST_F(builder_test, checks_the_libraries_of_products)
{
    std::string const extra = "[options.extra]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\n"
                              "[[product]]\nname = \"Extra\"\nlibraries = [\"extra\"]\nwhen = { option = \"extra\" }\n";
    write_recipe("b", "2.0", build_lua("b"), options_of_b + extra);
    ASSERT_EQ(run({"install"}), 0) << err.str();  // its condition does not hold
    project.write("otok.toml", manifest + "options = { extra = true }\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("build returned no library 'extra', which the product 'Extra' holds"), std::string::npos) << err.str();
}

TEST_F(builder_test, builds_every_configuration)
{
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\nconfigurations = [\"debug\", \"release\"]\n"
                               "[deps.a]\nrecipe = { path = \"recipes/a\" }\n[deps.b]\nrecipe = { path = \"recipes/b\" }\n");
    std::string const b = R"(function build(slice)
    otok.write(slice.prefix .. '/lib/libb.a', '!<arch>\n' .. slice.configuration)
    otok.write(slice.prefix .. '/include/b.h', HEADER)
    return { libraries = { b = slice.prefix .. '/lib/libb.a' }, headers = slice.prefix .. '/include' }
end
)";
    auto const with_header = [&](std::string const& header) {
        std::string script = b;
        script.replace(script.find("HEADER"), 6, header);
        write_recipe("b", "2.0", script, options_of_b);
    };
    with_header("'int b;\\n'");
    // a debug slice links the debug slices of its dependencies (otok-toml.md 5)
    write_recipe("a", "1.0", build_lua("a", "    assert(otok.read(deps.b.libraries.b) == '!<arch>\\n' .. slice.configuration)\n"),
        "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(err.str().find("building b 2.0 for native-" + machine_arch() + " (debug)\n"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("building b 2.0 for native-" + machine_arch() + "\n"), std::string::npos) << err.str();
    // the slices of a library counted as each is built (docs/PLAN.md decision 59)
    EXPECT_NE(err.str().find(" s), 1 of 2 slices\n"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find(" s), 2 of 2 slices\n"), std::string::npos) << err.str();
    EXPECT_EQ(entries(slices_of("b", "2.0")), 2u);
    std::filesystem::path const output = project.path() / "Otok" / "b";
    EXPECT_EQ(read_file(output / "lib" / "libb.a").value_or("?"), "!<arch>\nrelease");
    EXPECT_EQ(read_file(output / "debug" / "lib" / "libb.a").value_or("?"), "!<arch>\ndebug");
    EXPECT_EQ(read_file(output / "include" / "b.h").value_or("?"), "int b;\n");

    // one include/ for both: their headers must be the same
    with_header("'int b; /* ' .. slice.configuration .. ' */\\n'");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the header 'b.h' differs between the configurations"), std::string::npos) << err.str();
}

// The project's C and C++ standards (otok-toml.md 5.1): in the slice, after otok's flags, and in the slice key.
TEST_F(builder_test, passes_the_standards)
{
    // in the target table: one standard for every library of the target (otok-toml.md 5.1)
    auto const with = [&](std::string const& standard) {
        std::string text = manifest;
        text.replace(text.find("[deps.a]"), 0, "c-standard = 99\ncxx-standard = " + standard + "\n");
        project.write("otok.toml", text);
    };
    std::string const checks = "    local flag = '-std=c++' .. slice.cxx_standard\n"
                               "    assert(slice.toolchain.cxxflags[#slice.toolchain.cxxflags] == flag)\n"
                               "    assert(slice.toolchain.env.CXXFLAGS:find(flag, 1, true))\n"
                               "    assert(slice.c_standard == 99)\n"
                               "    assert(slice.toolchain.cflags[#slice.toolchain.cflags] == '-std=c99')\n"
                               "    assert(slice.toolchain.env.CFLAGS:find('-std=c99', 1, true))\n";
    write_recipe("b", "2.0", build_lua("b", checks), options_of_b);
    write_recipe("a", "1.0", build_lua("a", checks), "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
    with("23");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();

    // another standard: other slices
    with("20");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    with("23");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (2 from the cache)\n"), std::string::npos) << out.str();

}

TEST_F(builder_test, python_of_builds_has_none_of_the_users_modules)
{
    // as an environment captured from the user's brings them (docs/PLAN.md decision 71)
    toolchain.env = {{"PYTHONPATH", "/u/modules"}, {"PYTHONHOME", "/u/python"}, {"PYTHONNOUSERSITE", "0"}};
    std::string const checks = "    assert(slice.toolchain.env.PYTHONPATH == nil)\n"
                               "    assert(slice.toolchain.env.PYTHONHOME == nil)\n"
                               "    assert(slice.toolchain.env.PYTHONNOUSERSITE == '1')\n";
    write_recipe("b", "2.0", build_lua("b", checks), options_of_b);
    write_recipe("a", "1.0", build_lua("a", checks), "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
#ifdef _WIN32
    // names are case-insensitive there
    toolchain.env = {{"PythonPath", "/u/modules"}};
    write_recipe("b", "2.0", build_lua("b", checks + "    assert(slice.toolchain.env.PythonPath == nil)\n"), options_of_b);
    ASSERT_EQ(run({"install"}), 0) << err.str();
#endif
}

TEST_F(builder_test, relocates_pkg_config_files)
{
    // the prefix of the build becomes the place of the file (docs/PLAN.md decision 72); a longer name beside it stays,
    // as do line endings; b writes what its dependent must read
    write_recipe("b", "2.0", build_lua("b", R"(    local p = slice.prefix
    otok.write(p .. '/lib/pkgconfig/b.pc', 'prefix=' .. p .. '\nlibdir=' .. p .. '/lib\nincludedir=${prefix}/include\nother=' .. p
        .. '-x/y\nLibs: -L' .. p .. '/lib -lb\n')
    otok.write(p .. '/share/pkgconfig/c.pc', 'Cflags: -I' .. p .. '/include\r\nName: c')
    otok.write(p .. '/share/expected-b.pc', 'prefix=${pcfiledir}/../..\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nother=' .. p
        .. '-x/y\nLibs: -L${prefix}/lib -lb\n')
)"), options_of_b);
    write_recipe("a", "1.0", build_lua("a", R"(    local b = deps.b.prefix
    assert(otok.read(b .. '/lib/pkgconfig/b.pc') == otok.read(b .. '/share/expected-b.pc'), otok.read(b .. '/lib/pkgconfig/b.pc'))
    assert(otok.read(b .. '/share/pkgconfig/c.pc') == 'Cflags: -I${pcfiledir}/../../include\r\nName: c', otok.read(b .. '/share/pkgconfig/c.pc'))
)"), "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
}

TEST_F(builder_test, checks_libraries_by_linkage)
{
    project.write("otok.toml", manifest + "linkage = \"dynamic\"\n");
    auto const b_builds = [&](std::string const& library) {
        write_recipe("b", "2.0", R"(function build(slice)
    assert(slice.linkage == 'dynamic')
    local elf = '\127ELF\2\1' .. string.rep('\0', 10) .. '\3\0'
    local dll = 'MZ' .. string.rep('\0', 58) .. '\128\0\0\0' .. string.rep('\0', 64) .. 'PE\0\0' .. string.rep('\0', 18) .. '\34\32' .. string.rep('\0', 64)
    otok.write(slice.prefix .. '/include/b.h', 'int b;\n')
)" + library + R"(
end
)", options_of_b);
        std::string text = read_file(project.path() / "recipes" / "b" / "recipe.toml").value_or("");
        text.replace(text.find("description = "), 0, "linkage = [\"static\", \"dynamic\"]\n");
        project.write("recipes/b/recipe.toml", text);
    };
    // a shared object; its dependent is told how it was built (build-lua-api.md 4)
    b_builds("    otok.write(slice.prefix .. '/lib/libb.so', elf)\n    return { libraries = { b = slice.prefix .. '/lib/libb.so' }, headers = slice.prefix .. '/include' }");
    write_recipe("a", "1.0", build_lua("a", "    assert(deps.b.linkage == 'dynamic')\n"), "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(project.path() / "Otok" / "b" / "lib" / "libb.so"));

    // a RUNPATH into the build (docs/PLAN.md decision 31); a path in other bytes is no matter
    b_builds("    otok.write(slice.prefix .. '/lib/libb.so', elf .. slice.prefix)\n    return { libraries = { b = slice.prefix .. '/lib/libb.so' }, headers = slice.prefix .. '/include' }");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    b_builds("    otok.write(slice.prefix .. '/lib/libb.so', " + test::lua_bytes(test::elf_image(29)) + " .. slice.prefix .. '/lib\\0')\n"
             "    return { libraries = { b = slice.prefix .. '/lib/libb.so' }, headers = slice.prefix .. '/include' }");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("/lib' (an install name, RPATH or NEEDED entry), which leads into otok's directories: the consumer's executable must choose "
                             "where libraries are"),
        std::string::npos)
        << err.str();

    // a DLL linked against directly, and one with its import library
    b_builds("    otok.write(slice.prefix .. '/bin/b.dll', dll)\n    return { libraries = { b = slice.prefix .. '/bin/b.dll' }, headers = slice.prefix .. '/include' }");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find(") is a DLL: return its import library as link and the DLL as runtime"), std::string::npos) << err.str();
    b_builds("    otok.write(slice.prefix .. '/bin/b.dll', dll)\n    otok.write(slice.prefix .. '/lib/b.lib', '!<arch>\\n')\n"
             "    return { libraries = { b = { link = slice.prefix .. '/lib/b.lib', runtime = slice.prefix .. '/bin/b.dll' } }, headers = slice.prefix .. '/include' }");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(project.path() / "Otok" / "b" / "bin" / "b.dll"));
    EXPECT_TRUE(std::filesystem::exists(project.path() / "Otok" / "b" / "lib" / "b.lib"));

    // built static, it must be a static archive
    project.write("otok.toml", manifest);
    write_recipe("a", "1.0", build_lua("a"), "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
    write_recipe("b", "2.0", "function build(slice)\n    otok.write(slice.prefix .. '/lib/libb.so', '\\127ELF\\2\\1' .. string.rep('\\0', 10) .. '\\3\\0')\n"
                             "    return { libraries = { b = slice.prefix .. '/lib/libb.so' }, headers = slice.prefix }\nend\n", options_of_b);
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("/lib/libb.so) is not a static archive"), std::string::npos) << err.str();
}

// What a dynamic library loads (build-lua-api.md 8, docs/PLAN.md decision 80): its slice's, a dependency's, direct or not, a
// library of its products' `link` or the system's, as the target tells; nothing else of the machine.
TEST_F(builder_test, checks_what_libraries_load)
{
    toolchain.system = std::set<std::string>{"libc.so.6"};
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\n[deps.a]\nrecipe = { path = \"recipes/a\" }\n"
                               "linkage = \"dynamic\"\n[deps.b]\nrecipe = { path = \"recipes/b\" }\n[deps.gen]\nrecipe = { path = \"recipes/gen\" }\n");
    // a needs b, which needs gen, whose library directory has a shared object
    write_recipe("gen", "1.0", build_lua("gen", "    otok.write(slice.prefix .. '/lib/libgen.so.3', 'gen')\n"));
    write_recipe("b", "2.0", build_lua("b"), options_of_b + "[deps.gen]\nversion = \">= 1\"\ndefault-recipe = \"p/gen\"\n");
    auto const a_loads = [&](std::string const& loaded, std::string const& product = "") {
        write_recipe("a", "1.0",
            "function build(slice)\n    otok.write(slice.prefix .. '/include/a.h', 'int a;\\n')\n"
            "    otok.write(slice.prefix .. '/lib/liba.so', " + test::lua_bytes(test::elf_image(1) + loaded + '\0') + ")\n"
            "    return { libraries = { a = slice.prefix .. '/lib/liba.so' }, headers = slice.prefix .. '/include' }\nend\n",
            product + "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
        std::string text = read_file(project.path() / "recipes" / "a" / "recipe.toml").value_or("");
        text.replace(text.find("description = "), 0, "linkage = [\"static\", \"dynamic\"]\n");
        project.write("recipes/a/recipe.toml", text);
    };
    std::string const refused = "/lib/liba.so) loads '";
    std::string const why = "', which is neither its slice's, nor a dependency's, nor the system's: a library of the build machine, or of a "
                            "dependency the recipe does not declare";

    a_loads("libc.so.6");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    a_loads("libgen.so.3");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    a_loads("libfontconfig.so.1", "link = { libraries = [\"fontconfig\"] }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();

    // a library of the machine
    a_loads("libpng16.so.16");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find(refused + "libpng16.so.16" + why), std::string::npos) << err.str();
    // a dependency's name, but by a path the build machine has
    a_loads("/opt/homebrew/lib/libgen.so.3");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find(refused + "/opt/homebrew/lib/libgen.so.3" + why), std::string::npos) << err.str();
}

TEST_F(builder_test, tool_dependencies_and_build_tools)
{
    write_recipe("gen", "1.0",
        "function build(slice)\n    assert(slice.is_tool)\n    otok.write(slice.prefix .. '/bin/gen', 'generator')\n"
        "    otok.write(slice.prefix .. '/lib/libgen.a', '!<arch>\\n')\n"
        "    return { libraries = { gen = slice.prefix .. '/lib/libgen.a' }, headers = slice.prefix }\nend\n");
    write_recipe("a", "1.0",
        "function build_tools(slice, options, deps)\n    assert(slice.is_tool == false and deps.b == nil)\n"
        "    otok.write(slice.prefix .. '/bin/own-tool', 'own')\nend\n"
        + build_lua("a", "    assert(otok.read(deps.tools.gen.bin .. '/gen') == 'generator')\n"
                         "    assert(otok.read(slice.tools.prefix .. '/bin/own-tool') == 'own')\n"),
        "[tool-deps.gen]\nversion = \">= 1\"\ndefault-recipe = \"p/gen\"\n");
    project.write("otok.toml", manifest + "[tool-deps.gen]\nrecipe = { path = \"recipes/gen\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const e = err.str();
    EXPECT_LT(e.find("building tool gen 1.0"), e.find("building a 1.0 (build_tools)")) << e;
    EXPECT_LT(e.find("building a 1.0 (build_tools)"), e.find("building a 1.0 for")) << e;
    EXPECT_NE(out.str().find("slices: 3 (0 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_EQ(entries(slices_of("a", "1.0")), 2u);  // build_tools and build
}

TEST_F(builder_test, patches_and_resources)
{
    if (!find_git(current_environment())) {
#ifdef _WIN32
        // otok looks for git in PATH; the usual installation is not always in the PATH tests run with
        if (auto git = test::find_test_git()) {
            char const* old = std::getenv("PATH");  // case-insensitive; the variable is often spelled "Path"
            std::string const path = path_to_utf8(git->program.parent_path()) + ";" + (old ? old : "");
            _putenv_s("PATH", path.c_str());
        }
#endif
        if (!find_git(current_environment())) GTEST_SKIP() << "git is not in PATH";
    }
    upstream.write("data.txt", "data\n");
    std::string const data_sha = sha256_file(upstream.path() / "data.txt").value_or("?");
    project.write("recipes/b/patches/fix.patch", "--- a/b.c\n+++ b/b.c\n@@ -1 +1 @@\n-int b;\n+int b = 2;\n");
    write_recipe("b", "2.0",
        build_lua("b", "    assert(otok.read('b.c') == 'int b = 2;\\n', otok.read('b.c'))\n"
                       "    assert(otok.read('data/data.txt') == 'data\\n')\n"),
        options_of_b + "[[resource]]\nname = \"data\"\nurl = \"" + file_url(upstream.path() / "data.txt") + "\"\nsha256 = \"" + data_sha
            + "\"\nunpack = false\ndest = \"data/data.txt\"\n",
        "patches = [\"patches/fix.patch\"]\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();

    project.write("recipes/b/patches/fix.patch", "--- a/b.c\n+++ b/b.c\n@@ -1 +1 @@\n-int c;\n+int b = 2;\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the patch 'patches/fix.patch' does not apply"), std::string::npos) << err.str();
}

TEST_F(builder_test, file_options_conditional_resources_and_data)
{
    upstream.write("extra.txt", "extra\n");
    std::string const extra_sha = sha256_file(upstream.path() / "extra.txt").value_or("?");
    // b reads the project's file, needs the resource only with it, and returns a data file
    auto const script = [](std::string const& data) {
        return "function build(slice, options, deps)\n"
        "    local data = " + data + "\n"
        "    if options.filter then\n"
        "        assert(otok.read(slice.resources.extra) == 'extra\\n')\n"
        "        data = otok.read(options.filter)\n"
        "    else\n"
        "        assert(options.filter == false and slice.resources.extra == nil)\n"
        "    end\n"
        "    otok.write(slice.prefix .. '/share/b.dat', data)\n"
        "    otok.write(slice.prefix .. '/lib/libb.a', '!<arch>\\n')\n"
        "    otok.write(slice.prefix .. '/include/b.h', 'int b;\\n')\n"
        "    return { libraries = { b = slice.prefix .. '/lib/libb.a' }, headers = slice.prefix .. '/include',\n"
        "        data = { ['b.dat'] = slice.prefix .. '/share/b.dat' } }\n"
        "end\n";
    };
    std::string const options = options_of_b + "[options.filter]\ntype = \"file\"\ndoc = \"d\"\n";
    write_recipe("b", "2.0", script("'all\\n'"), options + "[[resource]]\nname = \"extra\"\nurl = \"" + file_url(upstream.path() / "extra.txt")
        + "\"\nsha256 = \"" + extra_sha + "\"\nunpack = false\nwhen = { option = \"filter\" }\n");
    std::filesystem::path const output = project.path() / "Otok";
    // the key of b's slice whose filter is `value`
    auto const key_of_b = [&](std::string const& value) {
        for (auto const& d : std::filesystem::directory_iterator{slices_of("b", "2.0")}) {
            std::string key = read_file(d.path() / "key.txt").value_or("");
            if (key.find("option.filter = " + value + "\n") != std::string::npos) return key;
        }
        return std::string{};
    };

    // without a file: no resource, the default data
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("inputs in the cache: 2 sources\n"), std::string::npos) << out.str();
    EXPECT_EQ(read_file(output / "b" / "data" / "b.dat").value_or("?"), "all\n");
    std::string key = key_of_b("none");
    ASSERT_FALSE(key.empty());
    EXPECT_EQ(key.find("resource.extra"), std::string::npos) << key;

    // with a file: its contents in the key, the resource fetched and given
    project.write("filters/f.json", "keep\n");
    project.write("otok.toml", manifest + "options = { filter = \"filters/./f.json\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("inputs in the cache: 2 sources, 1 resource\n"), std::string::npos) << out.str();
    EXPECT_EQ(read_file(output / "b" / "data" / "b.dat").value_or("?"), "keep\n");
    key = key_of_b("sha256:f660a7996deacfbc7560e4240054a8ad82eb02fe25a95064257e07084bcacb85");
    ASSERT_FALSE(key.empty());
    EXPECT_NE(key.find("resource.extra = archive:" + extra_sha + "\n"), std::string::npos) << key;
    EXPECT_EQ(key.find("filters"), std::string::npos) << key;

    // the same contents elsewhere build nothing; other contents rebuild
    project.write("other/g.json", "keep\n");
    project.write("otok.toml", manifest + "options = { filter = \"other/g.json\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (2 from the cache)\n"), std::string::npos) << out.str();
    project.write("other/g.json", "other\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_EQ(read_file(output / "b" / "data" / "b.dat").value_or("?"), "other\n");
    EXPECT_TRUE(toolchain.bundled.empty());

    // bundle-data reaches the target and the package key (otok-toml.md 6, cache-key.md 4), not the slices
    project.write("otok.toml", manifest + "options = { filter = \"other/g.json\" }\nbundle-data = true\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (2 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_EQ(toolchain.bundled, std::set<std::string>{"b"});
    bool keyed = false;
    for (auto const& d : std::filesystem::directory_iterator{cache.path() / "builds" / "b" / "2.0-r1" / "packages"}) {
        keyed = keyed || read_file(d.path() / "key.txt").value_or("").find("bundle-data = true\n") != std::string::npos;
    }
    EXPECT_TRUE(keyed);

    // a path out of the project, a missing file
    project.write("otok.toml", manifest + "options = { filter = \"../g.json\" }\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("option 'filter' expects a path of a file inside the project, relative to otok.toml, not '../g.json'"), std::string::npos) << err.str();
    project.write("otok.toml", manifest + "options = { filter = \"missing.json\" }\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the file 'missing.json' of option 'filter': "), std::string::npos) << err.str();

    // every slice returns the same data
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\nconfigurations = [\"debug\", \"release\"]\n"
                               "[deps.b]\nrecipe = { path = \"recipes/b\" }\n");
    write_recipe("b", "2.0", script("slice.configuration"), options);
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the data file 'b.dat' differs between "), std::string::npos) << err.str();
}

TEST(builder, key_text_is_sorted_lines)
{
    EXPECT_EQ(key_text({{"slice", "native-arm64"}, {"dep.zlib", "abc"}, {"library", "a"}}),
        "dep.zlib = abc\nlibrary = a\nslice = native-arm64\n");
    EXPECT_EQ(package_key("protobuf", true), "tool:protobuf");
    EXPECT_EQ(package_key("zlib", false), "zlib");
}

TEST_F(builder_test, writes_the_output)
{
    std::string const lib_a = "liba.a";  // the name the build gave it, on every host (otok-toml.md 4.1)
    std::filesystem::path const output = project.path() / "Otok";
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("output 'Otok': 2 copied, 0 unchanged, 0 removed\n"), std::string::npos) << out.str();
    EXPECT_NE(err.str().find("packaging a 1.0 for native\n"), std::string::npos) << err.str();
    EXPECT_EQ(read_file(output / "a" / "include" / "a.h").value_or("?"), "int a;\n");
    EXPECT_EQ(read_file(output / "a" / "lib" / lib_a).value_or("?"), "!<arch>\n");
    EXPECT_FALSE(std::filesystem::exists(output / "a" / "key.txt"));
    // the license files and the acknowledgements (otok-toml.md 4.2)
    EXPECT_EQ(read_file(output / "a" / "licenses" / "LICENSE").value_or("?"), "The license of a.\r\n");
    EXPECT_EQ(read_file(output / "Acknowledgements.md").value_or("?"),
        "<!-- Generated by otok; do not edit. -->\n# Acknowledgements\n\nThis product includes the following libraries.\n"
        "\n## a 1.0\n\nLicense: MIT\n\n```text\nThe license of a.\n```\n"
        "\n## b 2.0\n\nLicense: MIT\n\n```text\nThe license of b.\n```\n");

    // the package entry and its key (cache-key.md 4)
    std::filesystem::path const packages = cache.path() / "builds" / "a" / "1.0-r1" / "packages";
    ASSERT_EQ(entries(packages), 1u);
    std::filesystem::path entry;
    for (auto const& d : std::filesystem::directory_iterator{packages}) {
        if (d.is_directory()) entry = d.path();
    }
    std::string const key = read_file(entry / "key.txt").value_or("?");
    EXPECT_NE(key.find("packager = 9\n"), std::string::npos) << key;
    EXPECT_NE(key.find("slice.native-" + machine_arch() + ".release = "), std::string::npos) << key;
    EXPECT_EQ(sha256_hex(key).substr(0, 32), entry.filename().string());
    std::string const state = read_file(output / ".otok-output.toml").value_or("?");
    EXPECT_NE(state.find("format = 1\n"), std::string::npos) << state;
    EXPECT_NE(state.find("a = \"" + sha256_hex(key) + "\"\n"), std::string::npos) << state;

    // nothing changed: nothing is packaged or copied
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("output 'Otok': 0 copied, 2 unchanged, 0 removed\n"), std::string::npos) << out.str();
    EXPECT_EQ(err.str().find("packaging"), std::string::npos) << err.str();

    // an option of b changes both packages; a library no longer resolved is removed
    project.write("otok.toml", manifest + "options = { level = 2 }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("output 'Otok': 2 copied, 0 unchanged, 0 removed\n"), std::string::npos) << out.str();
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\n[deps.b]\nrecipe = { path = \"recipes/b\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("output 'Otok': 1 copied, 0 unchanged, 1 removed\n"), std::string::npos) << out.str();
    EXPECT_FALSE(std::filesystem::exists(output / "a"));
    EXPECT_TRUE(std::filesystem::exists(output / "b" / "include" / "b.h"));
}

TEST_F(builder_test, products_hold_the_dependencies_whose_condition_holds)
{
    // a needs b only with its option; the project names b itself, so b is built either way
    write_recipe("a", "1.0", build_lua("a"),
        "[options.with-b]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\n"
        "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\nwhen = { option = \"with-b\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(toolchain.products, (std::vector<std::string>{"a.P: a/a", "b.P: b/b"}));

    toolchain.products.clear();
    project.write("otok.toml", manifest + "[deps.a.options]\nwith-b = true\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(toolchain.products, (std::vector<std::string>{"a.P: a/a b/b"}));
}

TEST_F(builder_test, checks_license_files_after_fetching)
{
    std::string text = read_file(project.path() / "recipes" / "b" / "recipe.toml").value_or("?");
    std::string const files = "license-files = [\"LICENSE\"]";
    text.replace(text.find(files), files.size(), "license-files = [\"LICENSE\", \"doc/NOTICE\"]");
    project.write("recipes/b/recipe.toml", text);
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("b 2.0 (r1): the license file 'doc/NOTICE' named in license-files is not in the source"), std::string::npos) << err.str();
}

#ifndef _WIN32

// docs/PLAN.md decisions 28, 73: the processes of a build run in the sandbox
TEST_F(builder_test, builds_in_the_sandbox)
{
    // bwrap is looked up in the PATH otok has
    env["PATH"] = "/usr/bin:/bin";
    if (auto host = find_host_sandbox({{"PATH", env["PATH"]}}); host.kind == host_sandbox::mechanism::none) GTEST_SKIP() << host.why_not;
#ifdef __APPLE__
    std::string const unreachable = "Operation not permitted";
#else
    std::string const unreachable = "Network is unreachable";
#endif
    std::string const escape = path_to_utf8(std::filesystem::canonical(project.path()) / "escaped.txt");
    write_recipe("b", "2.0",
        build_lua("b",
            "    local out = otok.run({'/usr/bin/perl', '-MIO::Socket::INET', '-e', [[IO::Socket::INET->new(PeerAddr => '192.0.2.1:80', "
            "Timeout => 5) or print \"$!\"]]}, { capture = true })\n"
            "    assert(out == '" + unreachable + "', out)\n"
            "    otok.run({'/bin/sh', '-c', 'echo x > \"" + escape + "\"'}, { check = false })\n"),
        options_of_b);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_FALSE(std::filesystem::exists(escape));
    EXPECT_EQ(err.str().find("not isolated"), std::string::npos) << err.str();
}

TEST_F(builder_test, warns_when_the_sandbox_is_off)
{
    env["PATH"] = "/usr/bin:/bin";
    if (auto host = find_host_sandbox({{"PATH", env["PATH"]}}); host.kind == host_sandbox::mechanism::none) GTEST_SKIP() << host.why_not;
    env["OTOK_SANDBOX"] = "0";
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const warning = "warning: the sandbox is off ('sandbox' in config.toml): builds are not isolated\n";
    EXPECT_NE(err.str().find(warning), std::string::npos) << err.str();
    EXPECT_EQ(err.str().find(warning), err.str().rfind(warning)) << err.str();  // once
}

#endif

#ifndef __APPLE__

// Windows, and Linux without bwrap in PATH, which this test's environment has not
TEST_F(builder_test, notes_unisolated_builds)
{
    ASSERT_EQ(run({"install"}), 0) << err.str();
#ifdef _WIN32
    std::string const note = "note: builds are not isolated on this host\n";
#else
    std::string const note = "note: builds are not isolated on this host: bwrap (bubblewrap) was not found in PATH\n";
#endif
    EXPECT_NE(err.str().find(note), std::string::npos) << err.str();
    EXPECT_EQ(err.str().find(note), err.str().rfind(note)) << err.str();  // once
    // nothing built: nothing to say
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(err.str().find(note), std::string::npos) << err.str();
}

#endif

TEST(packager, defines_option_values_for_wrappers)
{
    recipe rec;
    auto const option = [&](std::string name, option_type type) { rec.options.push_back({std::move(name), type, false, "", {}, false, {}}); };
    option("no-deprecated", option_type::boolean);
    option("fast", option_type::boolean);
    option("level", option_type::integer);
    option("prefix", option_type::string);
    option("tls", option_type::enumeration);
    option("libraries", option_type::list);
    option("data-filter", option_type::file);
    option("other-filter", option_type::file);
    merged_options const values{{"no-deprecated", true}, {"fast", false}, {"level", std::int64_t{3}}, {"prefix", std::string{"a\"b\\"}},
        {"tls", std::string{"open-ssl"}}, {"libraries", std::vector<std::string>{"json", "x.y"}},
        {"data-filter", std::string{"f.json"}}, {"other-filter", std::string{}}};
    std::string text;
    for (option_define const& d : option_defines(rec, values)) text += d.name + (d.value ? "=" + *d.value : "") + "\n";
    EXPECT_EQ(text, "OTOK_OPTION_NO_DEPRECATED\nOTOK_OPTION_LEVEL=3\nOTOK_OPTION_PREFIX=\"a\\\"b\\\\\"\nOTOK_OPTION_TLS_OPEN_SSL\n"
                    "OTOK_OPTION_LIBRARIES_JSON\nOTOK_OPTION_LIBRARIES_X_Y\nOTOK_OPTION_DATA_FILTER\n");
}

TEST_F(builder_test, refuses_an_output_it_did_not_create)
{
    project.write("Other/notes.txt", "mine\n");
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\noutput = \"Other\"\n"
                               "[deps.b]\nrecipe = { path = \"recipes/b\" }\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("was not created by otok (it has no .otok-output.toml); remove it or set another 'output'"), std::string::npos)
        << err.str();
    EXPECT_EQ(read_file(project.path() / "Other" / "notes.txt").value_or("?"), "mine\n");
    EXPECT_FALSE(std::filesystem::exists(project.path() / "Other" / "b"));

    project.write("Other/.otok-output.toml", "not toml [");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the state file of the output '"), std::string::npos) << err.str();
}

TEST_F(builder_test, gives_slices_the_share_of_jobs)
{
    // b builds alone (a needs it), so its share is every job
    env["OTOK_JOBS"] = "4";
    write_recipe("b", "2.0", build_lua("b", "    assert(slice.jobs == 4, slice.jobs)\n"), options_of_b);
    ASSERT_EQ(run({"install"}), 0) << err.str();
}

TEST_F(builder_test, passes_the_projects_flags)
{
    // a reports what it was given in its headers, which the output copies
    write_recipe("a", "1.0",
        "function build(slice, options, deps)\n"
        "    otok.write(slice.prefix .. '/include/flags.txt', table.concat(slice.toolchain.cflags, ' ') .. '\\n' .. slice.toolchain.env.CFLAGS\n"
        "        .. '\\n' .. otok.read(otok.cmake_toolchain()))\n"
        + build_lua("a").substr(build_lua("a").find('\n') + 1),
        "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n");
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\ncflags = [\"-DT=1\"]\n"
                               "[deps.a]\nrecipe = { path = \"recipes/a\" }\ncflags = [\"-DA=\\\"x\\\"\"]\n"
                               "[deps.b]\nrecipe = { path = \"recipes/b\" }\n"
                               "[profile.p.native]\ncflags = [\"-DP=1\"]\n[profile.p.deps.a]\ncflags = [\"-DPA=1\"]\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string reported = read_file(project.path() / "Otok" / "a" / "include" / "flags.txt").value_or("?");
    EXPECT_EQ(reported.substr(0, reported.find("# the fake")), "-fake -DT=1 -DA=\"x\"\n-fake -DT=1 -DA=\"x\"\n") << reported;
    EXPECT_NE(reported.find("set(CMAKE_C_FLAGS_INIT \"-fake -DT=1 -DA=\\\"x\\\"\")\n"), std::string::npos) << reported;
    EXPECT_NE(reported.find("set(CMAKE_${otok_lang}_FLAGS_${otok_config} \"\" CACHE STRING"), std::string::npos) << reported;

    // a profile appends; other flags are other slices
    ASSERT_EQ(run({"install", "--profile", "p"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    reported = read_file(project.path() / "Otok" / "a" / "include" / "flags.txt").value_or("?");
    EXPECT_EQ(reported.substr(0, reported.find('\n')), "-fake -DT=1 -DP=1 -DA=\"x\" -DPA=1") << reported;
}

namespace {

// A file run_process would start; it never runs, since the tests replace the version probe.
std::string write_program(std::filesystem::path const& dir, std::string const& name)
{
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    std::filesystem::path const file = dir / (name + ".exe");
#else
    std::filesystem::path const file = dir / name;
#endif
    std::ofstream{file} << "#!/bin/sh\n";
#ifndef _WIN32
    std::filesystem::permissions(file, std::filesystem::perms::owner_all);
#endif
    return path_to_utf8(file.generic_u8string());
}

toolchain_requirement required(std::string const& name, std::string const& constraint = "", std::vector<std::string> targets = {})
{
    toolchain_requirement r;
    r.name = name;
    if (!constraint.empty()) r.version = *version_constraint::parse(constraint);
    r.targets = std::move(targets);
    return r;
}

}

TEST(toolchains, reads_versions)
{
    using detail::dotted_version_in;
    EXPECT_EQ(dotted_version_in("cmake version 3.31.6\n\nCMake suite maintained and supported by Kitware"), "3.31.6");
    EXPECT_EQ(dotted_version_in("go version go1.22.3 darwin/arm64\n"), "1.22.3");
    EXPECT_EQ(dotted_version_in("\nThis is perl 5, version 34, subversion 1 (v5.34.1) built for darwin"), "5.34.1");
    EXPECT_EQ(dotted_version_in("NASM version 2.16.03 compiled on Apr 18 2024\n"), "2.16.03");
    EXPECT_EQ(dotted_version_in("Python 3.12.1.\n"), "3.12.1");
    EXPECT_EQ(dotted_version_in("1.13.2\n"), "1.13.2");
    EXPECT_EQ(dotted_version_in("version 5, release 7"), std::nullopt);
}

TEST(toolchains, finds_programs)
{
    test::temp_dir dir;
    std::filesystem::path const target = dir.path() / "target", own = dir.path() / "otok";
    std::string const make = write_program(target, "make"), apple_libtool = write_program(target, "libtool");
    std::string const old_cmake = write_program(target, "cmake");
    std::string const gmake = write_program(own, "gmake");
    write_program(own, "cmake");
    std::map<std::string, std::string> const outputs{{"target/make", "GNU Make 3.81\n"}, {"otok/gmake", "GNU Make 4.4.1\n"},
        {"target/libtool", "Apple Inc. version cctools-1010.6\n"}, {"target/cmake", "cmake version 3.22.1\n"},
        {"otok/cmake", "cmake version 3.31.6\n"}, {"otok/glibtool", "glibtool (GNU libtool) 2.5.4\n"}};
    int probes = 0;
    toolchain_finder finder{path_to_utf8(own), [&](std::filesystem::path const& program, std::vector<std::string> const&) -> result<std::string> {
        ++probes;
        auto const it = outputs.find(path_to_utf8(program.parent_path().filename()) + "/" + path_to_utf8(program.stem()));
        if (it == outputs.end()) return fail("unknown");
        return it->second;
    }};
    std::string const target_path = path_to_utf8(target);

    // gmake before make; the target's PATH before otok's
    auto found = finder.find(required("make"), target_path);
    ASSERT_TRUE(found.has_value()) << found.error().message;
    EXPECT_EQ(path_to_utf8(found->program.generic_u8string()), gmake);
    EXPECT_EQ(found->version, "4.4.1");
    found = finder.find(required("cmake"), target_path);
    ASSERT_TRUE(found.has_value()) << found.error().message;
    EXPECT_EQ(path_to_utf8(found->program.generic_u8string()), old_cmake);
    EXPECT_EQ(found->version, "3.22.1");

    // nothing fits: what was found
    found = finder.find(required("make", ">= 5"), target_path);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().message, "found 4.4.1 at '" + gmake + "'; found 3.81 at '" + make + "'");
    found = finder.find(required("cmake", ">= 3.25"), target_path);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().message, "found 3.22.1 at '" + old_cmake + "'");
    found = finder.find(required("libtool"), target_path);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().message, "'" + apple_libtool + "' is not GNU libtool");
    std::string const glibtool = write_program(own, "glibtool");
    found = finder.find(required("libtool"), target_path);
    ASSERT_TRUE(found.has_value()) << found.error().message;
    EXPECT_EQ(path_to_utf8(found->program.generic_u8string()), glibtool);
    found = finder.find(required("ninja"), target_path);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().message, "not found in PATH");
    EXPECT_EQ(probes, 5);  // each program once: gmake, the first cmake, make, libtool, glibtool
}

TEST(toolchains, checks_rust_targets)
{
    test::temp_dir dir;
    std::string const rustc = write_program(dir.path(), "rustc");
    write_program(dir.path(), "rustup");
    toolchain_finder finder{"", [](std::filesystem::path const& program, std::vector<std::string> const& arguments) -> result<std::string> {
        if (program.stem() == "rustc") return std::string{"rustc 1.80.0 (051478957 2024-07-21)\n"};
        if (arguments != std::vector<std::string>{"target", "list", "--installed"}) return fail("unexpected");
        return std::string{"aarch64-apple-ios\nx86_64-apple-darwin\n"};
    }};
    std::string const path = path_to_utf8(dir.path());
    auto found = finder.find(required("rust", ">= 1.80", {"aarch64-apple-ios"}), path);
    ASSERT_TRUE(found.has_value()) << found.error().message;
    EXPECT_EQ(found->version, "1.80.0");
    found = finder.find(required("rust", ">= 1.80", {"aarch64-apple-ios", "aarch64-apple-ios-sim", "x86_64-apple-darwin"}), path);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().message, "found 1.80.0 at '" + rustc + "', but the targets 'aarch64-apple-ios-sim' are not installed (rustup target add)");
}

TEST_F(builder_test, checks_toolchains)
{
    std::string const dependency = "[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\n";
    test::temp_dir tools;
    std::string const cmake = write_program(tools.path(), "cmake");
    write_program(tools.path(), "ninja");
    toolchain.tools = path_to_utf8(tools.path());
    versions = {{"cmake", "cmake version 3.31.6\n"}, {"ninja", "1.12.1\n"}};

    // every problem at once, before anything is fetched
    write_recipe("a", "1.0", build_lua("a"), dependency + "[toolchains]\nmeson = \">= 1.5\"\n");
    write_recipe("b", "2.0", build_lua("b"), options_of_b + "[toolchains]\ncmake = \">= 4.0\"\nninja = \">= 1.10\"\n");
    EXPECT_EQ(run({"install"}), 1);
    std::string const e = err.str();
    EXPECT_NE(e.find("error: toolchain 'meson' (>= 1.5), needed by 'a': not found in PATH\n"), std::string::npos) << e;
    EXPECT_NE(e.find("error: toolchain 'cmake' (>= 4.0), needed by 'b': found 3.31.6 at '" + cmake + "'\n"), std::string::npos) << e;
    EXPECT_EQ(e.find("'ninja'"), std::string::npos) << e;
    EXPECT_EQ(out.str().find("inputs in the cache"), std::string::npos) << out.str();

    // the chosen programs: slice.toolchains, PATH, the key
    std::string const dir = path_to_utf8(tools.path());
    write_recipe("a", "1.0", build_lua("a"), dependency);
    write_recipe("b", "2.0",
        build_lua("b", "    assert(slice.toolchains.cmake == [[" + cmake + "]], slice.toolchains.cmake)\n"
                       "    assert(slice.toolchain.env.PATH:find([[" + dir + "]], 1, true), slice.toolchain.env.PATH)\n"),
        options_of_b + "[toolchains]\ncmake = \">= 3.20\"\nninja = \">= 1.10\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::filesystem::path entry;
    for (auto const& d : std::filesystem::directory_iterator{slices_of("b", "2.0")}) {
        if (d.is_directory()) entry = d.path();
    }
    std::string const key = read_file(entry / "key.txt").value_or("?");
    EXPECT_NE(key.find("toolchains.cmake = 3.31.6\ntoolchains.ninja = 1.12.1\n"), std::string::npos) << key;

    // a new version of a tool rebuilds what it built and what depends on that
    versions["cmake"] = "cmake version 3.31.7\n";
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("slices: 2 (0 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_EQ(entries(slices_of("b", "2.0")), 2u);

    // a toolchain of other targets is not looked for: `os` in its `when` (recipe-toml.md 13)
    write_recipe("a", "1.0", build_lua("a"), dependency + "[toolchains]\nmeson = { version = \">= 1.5\", when = { os = [\"windows\"] } }\n");
    EXPECT_EQ(run({"install"}), 0) << err.str();
    write_recipe("a", "1.0", build_lua("a"), dependency + "[toolchains]\nmeson = { version = \">= 1.5\", when = { os = [\"native\"] } }\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("error: toolchain 'meson' (>= 1.5), needed by 'a': not found in PATH\n"), std::string::npos) << err.str();
}

TEST(toolchains, reads_the_version_of_gn)
{
    EXPECT_EQ(detail::integer_version_in("2257 (fde0b7b)\n"), "2257");
    EXPECT_EQ(detail::integer_version_in("gn\n"), std::nullopt);
}

TEST_F(builder_test, toolchains_built_by_recipes)
{
    // recipe-toml.md 13.1: the recipe of a toolchain builds its program in the tool slice, before the recipe that needs it
    std::string const ninja_lua = "function build(slice)\n    assert(slice.is_tool)\n    otok.write(slice.prefix .. '/bin/ninja.exe', 'ninja')\n"
        "    otok.write(slice.prefix .. '/lib/libninja.a', '!<arch>\\n')\n"
        "    return { libraries = { ninja = slice.prefix .. '/lib/libninja.a' }, headers = slice.prefix, programs = { PROGRAMS } }\nend\n";
    auto const ninja_returning = [&](std::string const& programs) {
        std::string lua = ninja_lua;
        lua.replace(lua.find("PROGRAMS"), 8, programs);
        write_recipe("ninja", "1.13.1", lua, "[tool]\nprograms = [\"ninja\"]\n");
    };
    ninja_returning("ninja = slice.prefix .. '/bin/ninja.exe'");
    // the recipe's program
    write_recipe("b", "2.0",
        build_lua("b", "    local ninja = slice.toolchains.ninja:gsub('\\\\', '/')\n"
                       "    assert(ninja:find('/bin/ninja.exe', 1, true), ninja)\n"
                       "    assert(deps.tools.ninja == nil)\n"),
        options_of_b + "[toolchains]\nninja = { version = \">= 1.10\", default-recipe = \"p/ninja\" }\n");
    project.write("otok.toml", manifest + "[tool-deps.ninja]\nrecipe = { path = \"recipes/ninja\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const e = err.str();
    EXPECT_LT(e.find("building tool ninja 1.13.1"), e.find("building b 2.0 for")) << e;
    EXPECT_EQ(probes, 0);  // nothing looked for
    auto const keys_of_b = [&] {
        std::vector<std::string> keys;
        for (auto const& d : std::filesystem::directory_iterator{slices_of("b", "2.0")}) {
            if (d.is_directory()) keys.push_back(read_file(d.path() / "key.txt").value_or("?"));
        }
        return keys;
    };
    auto keys = keys_of_b();
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_NE(keys[0].find("tool.ninja = "), std::string::npos) << keys[0];
    EXPECT_EQ(keys[0].find("toolchains.ninja"), std::string::npos) << keys[0];
    // `*/cc` of the project (the target's default), the machine's system in the key (cache-key.md 3)
    EXPECT_NE(keys[0].find("environment = " + machine_environment("").system + "/cc\n"), std::string::npos) << keys[0];
    // the tool slice is built in the machine's tool environment (otok-toml.md 3, 7)
    for (auto const& d : std::filesystem::directory_iterator{slices_of("ninja", "1.13.1")}) {
        if (!d.is_directory()) continue;
        std::string const key = read_file(d.path() / "key.txt").value_or("?");
        std::string const tools = otok::manifest{}.tool_environment(test::native_info()).text();
        EXPECT_NE(key.find("environment = " + tools + "\n"), std::string::npos) << key;
    }
    std::string const lock = read_file(project.path() / "otok.lock").value_or("?");
    EXPECT_NE(lock.find("[[tool-package]]\nlibrary = \"ninja\""), std::string::npos) << lock;

    // what the tool recipe returns is checked
    ninja_returning("other = slice.prefix .. '/bin/ninja.exe'");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("build returned the program 'other', which [tool] programs does not name"), std::string::npos) << err.str();
}

// recipe-toml.md 9.1, 13: a toolchain for some architectures or environments only, which the builder decides per slice
TEST_F(builder_test, toolchains_of_some_slices)
{
    write_recipe("ninja", "1.13.1",
        "function build(slice)\n    otok.write(slice.prefix .. '/bin/ninja.exe', 'ninja')\n"
        "    otok.write(slice.prefix .. '/lib/libninja.a', '!<arch>\\n')\n"
        "    return { libraries = { ninja = slice.prefix .. '/lib/libninja.a' }, headers = slice.prefix,\n"
        "        programs = { ninja = slice.prefix .. '/bin/ninja.exe' } }\nend\n",
        "[tool]\nprograms = [\"ninja\"]\n");
    std::string const arch = test::native_info().platforms[0].architectures[0];
    auto const b_with = [&](std::string const& when, std::string const& check) {
        write_recipe("b", "2.0", build_lua("b", check),
            options_of_b + "[toolchains]\nninja = { version = \">= 1.10\", default-recipe = \"p/ninja\", when = " + when + " }\n");
    };
    project.write("otok.toml", manifest + "[tool-deps.ninja]\nrecipe = { path = \"recipes/ninja\" }\n");
    auto const key_of_b = [&] {
        std::string key;
        for (auto const& d : std::filesystem::directory_iterator{slices_of("b", "2.0")}) {
            if (d.is_directory()) key = read_file(d.path() / "key.txt").value_or("?");
        }
        return key;
    };

    // the slice's architecture, in an environment of the slice
    b_with("{ arch = [\"" + arch + "\"], environment = [\"*/cc\"] }", "    assert(slice.toolchains.ninja ~= nil)\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(key_of_b().find("tool.ninja = "), std::string::npos) << key_of_b();

    // another environment, which the resolver knows too: no program, nothing in the key
    std::filesystem::remove_all(slices_of("b", "2.0"));
    b_with("{ environment = [\"*/msvc\"] }", "    assert(slice.toolchains.ninja == nil)\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(key_of_b().find("tool.ninja"), std::string::npos) << key_of_b();
}

// recipe-toml.md 13.2: a program of a dependency, built by its recipe for the build machine where slice.is_tool holds
TEST_F(builder_test, programs_of_a_dependency)
{
    write_recipe("gen", "1.0",
        "function build(slice)\n    otok.write(slice.prefix .. '/lib/libgen.a', '!<arch>\\n')\n"
        "    local out = { libraries = { gen = slice.prefix .. '/lib/libgen.a' }, headers = slice.prefix }\n"
        "    if slice.is_tool then\n        otok.write(slice.prefix .. '/bin/genc.exe', 'genc')\n"
        "        out.programs = { genc = slice.prefix .. '/bin/genc.exe' }\n    end\n    return out\nend\n",
        "[tool]\nprograms = [\"genc\"]\n");
    write_recipe("b", "2.0",
        build_lua("b", "    local genc = slice.toolchains.genc:gsub('\\\\', '/')\n"
                       "    assert(genc:find('/bin/genc.exe', 1, true), genc)\n"
                       "    assert(deps.gen ~= nil and deps.tools.gen == nil)\n"),
        options_of_b + "[deps.gen]\nversion = \">= 1\"\ndefault-recipe = \"p/gen\"\ntoolchains = [\"genc\"]\n");
    // the project's recipe of the dependency is the tool's too
    project.write("otok.toml", manifest + "[deps.gen]\nrecipe = { path = \"recipes/gen\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const e = err.str();
    EXPECT_LT(e.find("building tool gen 1.0"), e.find("building b 2.0 for")) << e;
    std::string const lock = read_file(project.path() / "otok.lock").value_or("?");
    EXPECT_NE(lock.find("tool-dependencies = [\"gen@1.0\"]"), std::string::npos) << lock;
    EXPECT_NE(lock.find("[[tool-package]]\nlibrary = \"gen\""), std::string::npos) << lock;
}

TEST_F(builder_test, toolchains_that_are_scripts_run_under_their_interpreter)
{
    // docs/PLAN.md decision 70: "meson" is a script its recipe runs with the toolchain python, which a recipe builds too;
    // the child program plays the interpreter
    std::string const interpreter = read_file(path_from_utf8(OTOK_TEST_CHILD)).value_or("");
    ASSERT_FALSE(interpreter.empty());
    std::filesystem::path const archive = upstream.path() / "python-1.0.tar.gz";
    test::write_tar_gz(archive, {{"python-1.0/python.exe", interpreter}, {"python-1.0/LICENSE", "PSF\n"}});
    urls["python"] = file_url(archive);
    hashes["python"] = sha256_file(archive).value_or("?");
    std::filesystem::path const meson_archive = upstream.path() / "meson-1.0.tar.gz";
    test::write_tar_gz(meson_archive, {{"meson-1.0/meson.py", "# meson\n"}, {"meson-1.0/LICENSE", "Apache\n"}});
    urls["meson"] = file_url(meson_archive);
    hashes["meson"] = sha256_file(meson_archive).value_or("?");

    auto const tool_recipe = [&](std::string const& library, std::string const& version, std::string const& script, std::string const& extra) {
        std::string const dir = "recipes/" + library + "/";
        project.write(dir + "recipe.toml",
            "[recipe]\napi = 1\nlibrary = \"" + library + "\"\nname = \"" + library + "\"\nversion = \"" + version
            + "\"\nrevision = 1\nlicense = \"MIT\"\nlicense-files = [\"LICENSE\"]\ndescription = \"Test.\"\n"
            + "[source]\nurl = \"" + urls[library] + "\"\nsha256 = \"" + hashes[library] + "\"\n"
            + "[platforms]\nsupported = [\"native\"]\nbuild-on = [\"*/cc\"]\n[tool]\nprograms = [\"" + library + "\"]\n" + extra);
        project.write(dir + "build.lua", script);
    };
    tool_recipe("python", "3.14.10",
        "function build(slice)\n    otok.copy(slice.source .. '/python.exe', slice.prefix .. '/bin/python.exe')\n"
        "    return { programs = { python = slice.prefix .. '/bin/python.exe' } }\nend\n", "");
    auto const meson_with = [&](std::string const& interpreter_name) {
        tool_recipe("meson", "1.9.0",
            "function build(slice)\n    otok.copy(slice.source .. '/meson.py', slice.prefix .. '/meson.py')\n"
            "    return { programs = { meson = { script = slice.prefix .. '/meson.py', interpreter = '" + interpreter_name + "' } } }\nend\n",
            "[toolchains]\npython = { default-recipe = \"p/python\" }\n");
    };
    meson_with("python");
    write_recipe("b", "2.0",
        build_lua("b", "    local out = otok.run({ slice.toolchains.meson, 'args', 'setup' }, { capture = true })\n"
                       "    assert(out == 'script meson.py\\nsetup\\n', out)\n"),
        options_of_b + "[toolchains]\nmeson = { default-recipe = \"p/meson\" }\n");
    project.write("otok.toml", manifest + "[tool-deps.meson]\nrecipe = { path = \"recipes/meson\" }\n"
        "[tool-deps.python]\nrecipe = { path = \"recipes/python\" }\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const e = err.str();
    EXPECT_LT(e.find("building tool python"), e.find("building tool meson")) << e;
    EXPECT_LT(e.find("building tool meson"), e.find("building b 2.0 for")) << e;

    // the interpreter is a toolchain of the tool recipe
    meson_with("perl");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the program 'meson' runs under 'perl', which is not a toolchain of this build"), std::string::npos) << err.str();
}

TEST_F(builder_test, a_tool_recipe_without_products_is_no_dependency)
{
    project.write("recipes/ninja/recipe.toml",
        "[recipe]\napi = 1\nlibrary = \"ninja\"\nname = \"ninja\"\nversion = \"1.13.1\"\nrevision = 1\nlicense = \"MIT\"\nlicense-files = [\"LICENSE\"]\n"
        "description = \"Test.\"\n[source]\nurl = \"" + urls["ninja"] + "\"\nsha256 = \"" + hashes["ninja"] + "\"\n"
        "[platforms]\nsupported = [\"native\"]\nbuild-on = [\"*/cc\"]\n[tool]\nprograms = [\"ninja\"]\n");
    project.write("recipes/ninja/build.lua", "function build(slice)\n    return { programs = {} }\nend\n");
    project.write("otok.toml", manifest + "[deps.ninja]\nrecipe = { path = \"recipes/ninja\" }\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("ninja: a tool recipe without products can only be a tool dependency"), std::string::npos) << err.str();
}

// the project otok publish builds a tool recipe with (docs/PLAN.md decision 78): the recipe in [tool-deps], built as a
// root in the tool slice with the tool dependencies of its own; without tool_roots nothing uses it
TEST_F(builder_test, builds_tool_dependencies_as_roots)
{
    std::filesystem::path const archive = upstream.path() / "python-1.0.tar.gz";
    test::write_tar_gz(archive, {{"python-1.0/python.c", "int python;\n"}, {"python-1.0/LICENSE", "PSF\n"}});
    urls["python"] = file_url(archive);
    hashes["python"] = sha256_file(archive).value_or("?");
    auto const tool_recipe = [&](std::string const& library, std::string const& version, std::string const& extra) {
        project.write("recipes/" + library + "/recipe.toml",
            "[recipe]\napi = 1\nlibrary = \"" + library + "\"\nname = \"" + library + "\"\nversion = \"" + version
            + "\"\nrevision = 1\nlicense = \"MIT\"\nlicense-files = [\"LICENSE\"]\ndescription = \"Test.\"\n"
            + "[source]\nurl = \"" + urls[library] + "\"\nsha256 = \"" + hashes[library] + "\"\n"
            + "[platforms]\nsupported = [\"native\"]\nbuild-on = [\"*/cc\"]\n[tool]\nprograms = [\"" + library + "\"]\n" + extra);
    };
    tool_recipe("ninja", "1.13.1", "[toolchains]\npython = { default-recipe = \"p/python\" }\n");
    project.write("recipes/ninja/build.lua", "function build(slice)\n    assert(slice.is_tool)\n    assert(slice.toolchains.python)\n"
        "    otok.write(slice.prefix .. '/bin/ninja.exe', 'ninja')\n    return { programs = { ninja = slice.prefix .. '/bin/ninja.exe' } }\nend\n");
    tool_recipe("python", "1.0", "");
    project.write("recipes/python/build.lua",
        "function build(slice)\n    otok.write(slice.prefix .. '/bin/python.exe', 'python')\n"
        "    return { programs = { python = slice.prefix .. '/bin/python.exe' } }\nend\n");
    project.write("otok.toml", "[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\n"
        "[tool-deps.ninja]\nrecipe = { path = \"recipes/ninja\" }\n[tool-deps.python]\nrecipe = { path = \"recipes/python\" }\n");

    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(err.str().find("[tool-deps.ninja] is not used"), std::string::npos) << err.str();
    EXPECT_EQ(err.str().find("building tool"), std::string::npos) << err.str();

    tool_roots = true;
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const e = err.str();
    EXPECT_EQ(e.find("is not used"), std::string::npos) << e;
    EXPECT_LT(e.find("building tool python 1.0"), e.find("building tool ninja 1.13.1")) << e;
    EXPECT_EQ(entries(slices_of("ninja", "1.13.1")), 1u);
    std::string const lock = read_file(project.path() / "otok.lock").value_or("?");
    EXPECT_NE(lock.find("[[tool-package]]\nlibrary = \"ninja\""), std::string::npos) << lock;

    // a tool recipe that does not build its programs fails as in any build
    project.write("recipes/ninja/build.lua", "function build(slice)\n    return { programs = {} }\nend\n");
    EXPECT_EQ(run({"install"}), 1);
}

TEST(meson, writes_machine_files)
{
    slice_toolchain t;
    t.cc = "/x/clang";
    t.cxx = "/x/clang++";
    t.ar = "/x/ar";
    t.cflags = {"-target", "arm64-apple-ios16.0", "-DA='1'"};
    t.cxxflags = {"-std=c++20"};
    t.ldflags = {"-Wl,-x"};
    std::map<std::string, std::string> const host{{"system", "darwin"}, {"subsystem", "ios"}, {"kernel", "xnu"}, {"cpu_family", "aarch64"},
        {"cpu", "arm64"}, {"endian", "little"}};
    EXPECT_EQ(meson_machine_file(t, host, "/y/pkg-config"),
        "# Generated by otok: the meson cross file of the slice.\n"
        "[binaries]\n"
        "c = '/x/clang'\n"
        "cpp = '/x/clang++'\n"
        "objc = '/x/clang'\n"
        "objcpp = '/x/clang++'\n"
        "ar = '/x/ar'\n"
        "pkg-config = '/y/pkg-config'\n"
        "\n[built-in options]\n"
        "c_args = ['-target', 'arm64-apple-ios16.0', '-DA=\\'1\\'']\n"
        "cpp_args = ['-std=c++20']\n"
        "objc_args = ['-target', 'arm64-apple-ios16.0', '-DA=\\'1\\'']\n"
        "objcpp_args = ['-std=c++20']\n"
        "c_link_args = ['-Wl,-x']\n"
        "cpp_link_args = ['-Wl,-x']\n"
        "objc_link_args = ['-Wl,-x']\n"
        "objcpp_link_args = ['-Wl,-x']\n"
        "\n[properties]\nneeds_exe_wrapper = true\n"
        "\n[host_machine]\n"
        "cpu = 'arm64'\ncpu_family = 'aarch64'\nendian = 'little'\nkernel = 'xnu'\nsubsystem = 'ios'\nsystem = 'darwin'\n");

    // not cross-compiled: a native file; Windows paths keep their backslashes, escaped
    t.cc = "C:\\VS\\cl.exe";
    t.cxx = t.cc;
    t.ar = "C:\\VS\\lib.exe";
    t.cflags = {"/O2"};
    t.cxxflags = {"/O2"};
    t.ldflags = {};
    EXPECT_EQ(meson_machine_file(t, {}, ""),
        "# Generated by otok: the meson native file of the slice.\n"
        "[binaries]\n"
        "c = 'C:\\\\VS\\\\cl.exe'\n"
        "cpp = 'C:\\\\VS\\\\cl.exe'\n"
        "ar = 'C:\\\\VS\\\\lib.exe'\n"
        "\n[built-in options]\n"
        "c_args = ['/O2']\n"
        "cpp_args = ['/O2']\n"
        "c_link_args = []\n"
        "cpp_link_args = []\n");

    // ELF shared objects find the libraries of their slice beside themselves (docs/PLAN.md decision 42)
    t.rpath = "$ORIGIN";
    std::string const elf = meson_machine_file(t, {}, "");
    EXPECT_NE(elf.find("c_link_args = ['-Wl,-rpath,$ORIGIN']\ncpp_link_args = ['-Wl,-rpath,$ORIGIN']\n"), std::string::npos) << elf;

    // a target's own: the compiler with its arguments instead of the toolchain's, the linker and the resource compiler
    // (clang-cl with its --target, lld-link, llvm-rc)
    t.rpath.clear();
    std::string const windows = meson_machine_file(t, {}, "", {{"c", {"/l/clang-cl", "--target=aarch64-pc-windows-msvc"}}, {"c_ld", {"/l/lld-link"}},
        {"windres", {"/l/llvm-rc"}}});
    EXPECT_EQ(windows.substr(0, windows.find("\n[built-in options]")),
        "# Generated by otok: the meson native file of the slice.\n"
        "[binaries]\n"
        "cpp = 'C:\\\\VS\\\\cl.exe'\n"
        "ar = 'C:\\\\VS\\\\lib.exe'\n"
        "c = ['/l/clang-cl', '--target=aarch64-pc-windows-msvc']\n"
        "c_ld = '/l/lld-link'\n"
        "windres = '/l/llvm-rc'\n");
}

// otok publish (docs/PLAN.md decision 46): the recipes of a checkout built from its remote, each in a
// project of its own whose dependencies come from the index, then committed and pushed to the index.
TEST_F(builder_test, publishes_what_it_built)
{
    auto git = find_git(current_environment());
    if (!git) GTEST_SKIP() << "git is not in PATH";
    auto const git_in = [&](std::filesystem::path const& dir, std::vector<std::string> args) {
        std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
        full.insert(full.end(), args.begin(), args.end());
        auto done = run_git(*git, full, dir, upstream.path() / "setup.log", true);
        EXPECT_TRUE(done.has_value()) << (done ? "" : done.error().message);
        return done.value_or("");
    };
    write_recipe("b", "2.0", build_lua("b"), options_of_b + "[[publish.check]]\noptions = { level = 2 }\n");
    // the project is a checkout of the recipes, which is its own remote
    git_in(project.path(), {"init", "-q"});
    git_in(project.path(), {"add", "."});
    git_in(project.path(), {"commit", "-q", "-m", "recipes"});
    git_in(project.path(), {"remote", "add", "origin", file_url(project.path())});
    // the index: a clone of a bare repository, pushed to
    upstream.write("seed/index.toml", "format = 1\n");
    git_in(upstream.path(), {"init", "-q", "--bare", "index.git"});
    git_in(upstream.path() / "seed", {"init", "-q"});
    git_in(upstream.path() / "seed", {"add", "."});
    git_in(upstream.path() / "seed", {"commit", "-q", "-m", "index"});
    git_in(upstream.path() / "seed", {"push", "-q", file_url(upstream.path() / "index.git"), "HEAD:refs/heads/main"});
    git_in(upstream.path(), {"clone", "-q", "-b", "main", file_url(upstream.path() / "index.git"), "index"});
    std::filesystem::path const index = upstream.path() / "index";
    git_in(index, {"config", "user.name", "otok test"});
    git_in(index, {"config", "user.email", "test@otok.invalid"});
    std::string const index_arg = path_to_utf8(index);

    // a alone fails: it needs b, which the index does not have yet
    ASSERT_EQ(run({"publish", "--index", index_arg, "--publisher", "p", "recipes/a"}), 1);
    EXPECT_NE(err.str().find("the index has no recipe 'p/b'"), std::string::npos) << err.str();

    // every recipe of the checkout, published together: a's dependency b is the one published with it; b is built with
    // the defaults and with its check, and its entry records it
    ASSERT_EQ(run({"publish", "--index", index_arg, "--publisher", "p", "--all"}), 0) << err.str();
    std::string const o = out.str();
    EXPECT_NE(o.find("building p/a 1.0-r1 (native) for native in */cc\n"), std::string::npos) << o;
    EXPECT_NE(o.find("building p/b 2.0-r1 (native) for native in */cc\n"), std::string::npos) << o;
    EXPECT_NE(o.find("building p/b 2.0-r1 (native) for native in */cc with {level = 2}\n"), std::string::npos) << o;
    // each build said done; nothing of the installs of the temporary projects (docs/PLAN.md decision 60)
    EXPECT_NE(o.find("built p/b 2.0-r1 (native) for native in */cc with {level = 2} ("), std::string::npos) << o;
    EXPECT_NE(o.find("built p/a 1.0-r1 (native) for native in */cc ("), std::string::npos) << o;
    for (char const* internal : {"inputs in the cache", "otok.lock", "slices: ", "output 'Otok'"}) {
        EXPECT_EQ(o.find(internal), std::string::npos) << internal << "\n" << o;
    }
    EXPECT_EQ(err.str().find("packaging "), std::string::npos) << err.str();
    EXPECT_EQ(err.str().find("toolchain of 'native'"), err.str().rfind("toolchain of 'native'")) << err.str();  // once
    EXPECT_TRUE(o.ends_with("added recipes/p/a/native/1.0-r1.toml\nadded recipes/p/b/native/2.0-r1.toml\npushed to the index\n")) << o;
    // an entry says nothing of the build (docs/PLAN.md decision 82)
    std::string const entry = read_file(index / "recipes" / "p" / "b" / "native" / "2.0-r1.toml").value_or("");
    EXPECT_NE(entry.find("variant = \"native\""), std::string::npos) << entry;
    EXPECT_EQ(entry.find("validated-with"), std::string::npos) << entry;
    EXPECT_EQ(read_file(index / "libraries" / "b.toml").value_or("?"), "scheme = \"dotted\"\n");
    EXPECT_EQ(git_in(index, {"status", "--porcelain"}), "");
    EXPECT_EQ(git_in(upstream.path() / "index.git", {"log", "-1", "--format=%s", "main"}), "Publish p/a 1.0-r1 (native), p/b 2.0-r1 (native)\n");

    // nothing new; a dry run builds all the same
    ASSERT_EQ(run({"publish", "--index", index_arg, "--publisher", "p", "--all"}), 0) << err.str();
    EXPECT_EQ(out.str(), "p/a 1.0-r1 (native) is published already\np/b 2.0-r1 (native) is published already\n");
    // a later commit of the checkout leaves the trees of the recipes as they were: still published
    project.write("README.md", "Recipes.\n");
    git_in(project.path(), {"add", "README.md"});
    git_in(project.path(), {"commit", "-q", "-m", "readme"});
    ASSERT_EQ(run({"publish", "--index", index_arg, "--publisher", "p", "--all"}), 0) << err.str();
    EXPECT_EQ(out.str(), "p/a 1.0-r1 (native) is published already\np/b 2.0-r1 (native) is published already\n");
    ASSERT_EQ(run({"publish", "--index", index_arg, "--publisher", "p", "--dry-run", "recipes/a"}), 0) << err.str();
    EXPECT_NE(out.str().find("building p/a 1.0-r1 (native) for native in */cc\n"), std::string::npos) << out.str();
    EXPECT_TRUE(out.str().ends_with("built; --dry-run publishes nothing\n")) << out.str();
    // no publisher, none to find out: no build under a made-up name
    EXPECT_EQ(run({"publish", "--index", index_arg, "--dry-run", "recipes/a"}), 1);
    EXPECT_EQ(err.str(), "error: --dry-run with a checkout of the index needs --publisher NAME\n");

    // a failed build publishes nothing
    write_recipe("a", "1.1", "function build(slice, options, deps)\n    error('broken')\nend\n");
    git_in(project.path(), {"commit", "-q", "-am", "a 1.1"});
    EXPECT_EQ(run({"publish", "--index", index_arg, "--publisher", "p", "recipes/a"}), 1);
    EXPECT_NE(err.str().find("error: the build of p/a 1.1-r1 (native) for native in */cc failed; nothing is published"), std::string::npos) << err.str();
    EXPECT_FALSE(std::filesystem::exists(index / "recipes" / "p" / "a" / "1.1-r1.toml"));
}

// ---- the work directories of dev slices (otok-dev-toml.md 4)

namespace {

std::filesystem::file_time_type time_of(std::filesystem::path const& file)
{
    return std::filesystem::last_write_time(file);
}

void set_time(std::filesystem::path const& file, std::chrono::seconds ago)
{
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - ago);
}

}

TEST(dev_tree, synchronizes_only_what_changed)
{
    test::temp_dir dir;
    dir.write("tree/a.c", "int a;\n");
    dir.write("tree/sub/b.c", "int b;\n");
    dir.write("tree/.git/HEAD", "ref: refs/heads/main\n");
    dir.write("res/r.txt", "r\n");
    dir.write("res/unlisted.txt", "not taken\n");  // the tree names its files, as git lists a checkout's
    set_time(dir.path() / "tree/a.c", std::chrono::seconds{100});
    std::filesystem::path const source = dir.path() / "work/source";
    std::vector<synced_tree> const trees{{dir.path() / "tree", "", std::nullopt}, {dir.path() / "res", "modules/res", std::vector<std::string>{"r.txt", "gone.txt"}}};

    dev_record first;
    std::map<std::string, std::filesystem::path> origins;
    auto copied = sync_trees(trees, source, {}, first, {}, origins);
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(*copied, (std::set<std::string>{"a.c", "modules/res/r.txt", "sub/b.c"}));
    EXPECT_EQ(read_file(source / "modules/res/r.txt").value_or("?"), "r\n");
    EXPECT_FALSE(std::filesystem::exists(source / ".git"));
    EXPECT_FALSE(std::filesystem::exists(source / "modules/res/unlisted.txt"));
    // the time of the working tree, which the build tool compares
    EXPECT_EQ(time_of(source / "a.c"), time_of(dir.path() / "tree/a.c"));
    EXPECT_EQ(origins.at("modules/res/r.txt"), dir.path() / "res/r.txt");

    // what the build wrote stays; a changed file is copied, a removed one goes, the rest is left
    dir.write("work/source/generated.h", "#define G 1\n");
    dir.write("tree/sub/b.c", "int b = 2;\n");
    std::filesystem::remove(dir.path() / "tree/a.c");
    dir.write("tree/c.c", "int c;\n");
    auto const r_time = time_of(source / "modules/res/r.txt");
    dev_record second;
    copied = sync_trees(trees, source, first, second, {}, origins);
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(*copied, (std::set<std::string>{"c.c", "sub/b.c"}));
    EXPECT_EQ(read_file(source / "sub/b.c").value_or("?"), "int b = 2;\n");
    EXPECT_FALSE(std::filesystem::exists(source / "a.c"));
    EXPECT_TRUE(std::filesystem::exists(source / "generated.h"));
    EXPECT_EQ(time_of(source / "modules/res/r.txt"), r_time);
    EXPECT_EQ(second.files.size(), 3u);

    // forced, or with its copy gone
    std::filesystem::remove(source / "c.c");
    dev_record third;
    copied = sync_trees(trees, source, second, third, {"modules/res/r.txt"}, origins);
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(*copied, (std::set<std::string>{"c.c", "modules/res/r.txt"}));
}

TEST(dev_tree, keeps_its_record)
{
    test::temp_dir dir;
    dev_record r;
    r.key = "a = 1\nb = \"x\"\n";
    r.patches = "abc";
    r.files["src/a b.c"] = {12, 1761731045123456789};
    r.files["\"quoted\".h"] = {0, -5};
    ASSERT_TRUE(write_dev_record(dir.path() / "dev.toml", r));
    auto read = read_dev_record(dir.path() / "dev.toml");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->key, r.key);
    EXPECT_EQ(read->patches, r.patches);
    EXPECT_EQ(read->files, r.files);
    EXPECT_FALSE(read_dev_record(dir.path() / "missing.toml").has_value());
    dir.write("bad.toml", "[files]\n\"a\" = 1\n");
    EXPECT_FALSE(read_dev_record(dir.path() / "bad.toml").has_value());
}

TEST(dev_tree, finds_the_files_of_patches)
{
    std::string const patch = "diff --git a/src/a.c b/src/a.c\r\n--- a/src/a.c\r\n+++ b/src/a.c\r\n@@ -1 +1 @@\r\n-a\r\n+b\r\n"
                              "diff --git a/new.h b/new.h\nnew file mode 100644\n--- /dev/null\n+++ b/new.h\t2026-10-29\n@@ -0,0 +1 @@\n+n\n";
    EXPECT_EQ(patched_files(patch), (std::set<std::string>{"new.h", "src/a.c"}));
}
