// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// install, update, fetch, tree, why, metadata on a project in a temporary directory with local recipes whose
// sources are file:// archives.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "otok/commands/commands.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"
#include "otok/core/tree_hash.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/lock/lock.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "otok/target/windows/windows_target.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

target_info const* const apple_targets[] = {&target::apple::info()};
target_info const* const all_targets[] = {&target::apple::info(), &target::android::info(), &test::native_info(), &target::windows::info(),
    &target::linux::info()};

// `source` is the body of [source].
std::string recipe_toml(std::string_view library, std::string_view version, std::string_view source, std::string_view extra = "")
{
    return "[recipe]\napi = 1\nlibrary = \"" + std::string{library} + "\"\nname = \"" + std::string{library} + "\"\nversion = \""
        + std::string{version} + "\"\nrevision = 1\nlicense = \"MIT\"\nlicense-files = [\"LICENSE\"]\ndescription = \"Test.\"\n"
        + "[source]\n" + std::string{source}
        + "[platforms]\nsupported = [\"ios\", \"iossim\", \"macosx\"]\nbuild-on = [\"macos/xcode\"]\n"
        + "[[product]]\nname = \"P\"\nlibraries = [\"" + std::string{library} + "\"]\n" + std::string{extra};
}

std::string const options_of_b = "[options.shared]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\n"
                                 "[options.level]\ntype = \"int\"\ndefault = 1\ndoc = \"d\"\n";

// What publishing requires of a product for Apple platforms (DESIGN 7.4), the first key after [[product]].
std::string const privacy = "apple = { privacy-manifest = \"PrivacyInfo.xcprivacy\" }\n";
std::string const privacy_manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict/></plist>\n";

std::string const dependency_on_b ="[deps.b]\nversion = \">= 1\"\ndefault-recipe = \"p/b\"\nrequires-options = { shared = true }\n";

std::string const manifest_toml =
    "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\", \"iossim\"]\ndeployment = { ios = \"15.0\" }\n"
    "[deps.a]\nrecipe = { path = \"recipes/a\" }\n"
    "[deps.b]\nrecipe = { path = \"recipes/b\" }\n"
    "[profile.ci]\napple = { platforms = [\"iossim\"] }\n";

class commands : public ::testing::Test
{
protected:
    void SetUp() override
    {
        for (std::string const library : {"a", "b", "c"}) {
            std::filesystem::path const file = upstream.path() / (library + "-1.0.tar.gz");
            test::write_tar_gz(file, {{library + "-1.0/" + library + ".c", "int " + library + ";\n"}, {library + "-1.0/LICENSE", "MIT\n"}});
            urls[library] = file_url(file);
            hashes[library] = sha256_file(file).value_or("?");
        }
        project.write("otok.toml", manifest_toml);
        project.write("recipes/a/recipe.toml", recipe_of("a", "1.0", dependency_on_b));
        project.write("recipes/b/recipe.toml", recipe_of("b", "2.0", options_of_b));
    }

    std::string source_of(std::string const& library) { return "url = \"" + urls[library] + "\"\nsha256 = \"" + hashes[library] + "\"\n"; }

    std::string recipe_of(std::string const& library, std::string_view version, std::string_view extra = "")
    {
        return recipe_toml(library, version, source_of(library), extra);
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
        // a cache of its own, never the user's
        std::filesystem::path const cache_dir = cache.path();
        auto const extra = env_vars;
        environment const env = [cache_dir, extra](std::string_view name) -> std::optional<std::string> {
            if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache_dir);
            if (auto it = extra.find(std::string{name}); it != extra.end()) return it->second;
            if (name == "OTOK_INDEX") return "";  // not the public index: no network
            return std::nullopt;
        };
        command_context ctx{project.path(), targets, nullptr, out, err, env};
        ctx.http = http;
        return run_command(*parsed, ctx);
    }

    std::string lock_text() const { return read_file(project.path() / "otok.lock").value_or("<missing>"); }
    std::string dev_file() const { return path_to_utf8(project.path() / "otok.dev.toml"); }

    std::filesystem::path unpacked(std::string const& library) const { return cache.path() / "sources" / ("archive-" + hashes.at(library)); }

    test::temp_dir project;
    test::temp_dir cache;
    test::temp_dir upstream;
    std::map<std::string, std::string> urls;
    std::map<std::string, std::string> hashes;
    std::map<std::string, std::string> env_vars;  // more of the environment
    std::span<target_info const* const> targets = apple_targets;
    http_transport http;                          // empty: libcurl
    std::ostringstream out;
    std::ostringstream err;
};

std::string const not_built = "note: this machine cannot build for 'apple'\n";

}

TEST_F(commands, install_writes_the_lock_and_fetches)
{
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock created:\n  added a 1.0 (r1)\n  added b 2.0 (r1)\ninputs in the cache: 2 sources\n" + not_built);
    // in the order of the hashes
    EXPECT_NE(err.str().find("downloading " + urls["a"] + "\n"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("downloading " + urls["b"] + "\n"), std::string::npos) << err.str();
    EXPECT_EQ(read_file(unpacked("a") / "a.c").value_or("?"), "int a;\n");
    EXPECT_EQ(read_file(unpacked("b") / "b.c").value_or("?"), "int b;\n");
    diagnostics d;
    auto lock = parse_lock(lock_text(), "otok.lock", d);
    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(test::locked(*lock, "a")->recipe_source.path, "recipes/a");
    EXPECT_EQ(test::locked(*lock, "a")->dependencies, std::vector<std::string>{"b"});
    EXPECT_EQ(test::locked(*lock, "b")->source.url, urls["b"]);

    // everything is in the cache now: nothing is downloaded (the upstream files are gone)
    std::string const first = lock_text();
    std::filesystem::remove_all(upstream.path() / "a-1.0.tar.gz");
    std::filesystem::remove_all(upstream.path() / "b-1.0.tar.gz");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock is up to date\ninputs in the cache: 2 sources\n" + not_built);
    EXPECT_EQ(err.str(), "");
    EXPECT_EQ(lock_text(), first);
    EXPECT_FALSE(std::filesystem::exists(project.path() / "otok.lock.tmp"));
}

TEST_F(commands, fetch_locations_of_the_project)
{
    // a's archive moved; the project knows where
    std::filesystem::path const moved = upstream.path() / "moved" / "a.tar.gz";
    std::filesystem::create_directories(moved.parent_path());
    std::filesystem::rename(upstream.path() / "a-1.0.tar.gz", moved);
    project.write("otok.toml", manifest_toml + "[fetch]\nmirrors-only = [\"a\", \"zzz\"]\n[[fetch.url-rewrite]]\nfrom = \"" + urls["a"]
        + "\"\nto = \"" + file_url(moved) + "\"\n");

    // the machine allows only its own locations for a: the project's rewrite is not used
    env_vars["OTOK_MIRRORS_ONLY"] = "1";
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("no asset-mirror or url-rewrite covers " + urls["a"] + ", and mirrors-only is set"), std::string::npos) << err.str();
    env_vars.clear();

    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(err.str().find("downloading " + file_url(moved) + "\n"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("warning: 'mirrors-only' in fetch names 'zzz', which is not a dependency of the project"), std::string::npos) << err.str();
    EXPECT_EQ(read_file(unpacked("a") / "a.c").value_or("?"), "int a;\n");
    // the lock keeps the recipe's location
    diagnostics d;
    auto lock = parse_lock(lock_text(), "otok.lock", d);
    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(test::locked(*lock, "a")->source.url, urls["a"]);
}

TEST_F(commands, locked)
{
    EXPECT_EQ(run({"install", "--locked"}), 1);
    EXPECT_NE(err.str().find("error: --locked needs otok.lock, and there is none"), std::string::npos) << err.str();

    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(run({"install", "--locked"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock is up to date\ninputs in the cache: 2 sources\n" + not_built);

    project.write("recipes/b/recipe.toml", recipe_of("b", "2.1", options_of_b));
    std::string const before = lock_text();
    EXPECT_EQ(run({"install", "--locked"}), 1);
    EXPECT_NE(err.str().find("error: otok.lock is out of date (--locked):\n  b: 2.0 (r1) -> 2.1 (r1)\nrun 'otok install' to update it"),
        std::string::npos) << err.str();
    EXPECT_EQ(lock_text(), before);  // --locked never writes

    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("otok.lock updated:\n  b: 2.0 (r1) -> 2.1 (r1)\n"), std::string::npos) << out.str();
}

TEST_F(commands, locked_refuses_local_inputs)
{
    test::temp_dir elsewhere;
    elsewhere.write("b/recipe.toml", recipe_of("b", "2.0", options_of_b));
    std::string const outside = path_to_utf8((elsewhere.path() / "b").generic_u8string());
    std::string text = manifest_toml;
    text.replace(text.find("recipes/b"), 9, outside);
    project.write("otok.toml", text);

    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(err.str().find("warning: 'b' has local inputs outside the project"), std::string::npos) << err.str();
    EXPECT_EQ(run({"install", "--locked"}), 1);
    EXPECT_NE(err.str().find("error: --locked refuses local inputs outside the project, which cannot be reproduced: 'b'"),
        std::string::npos) << err.str();
}

TEST_F(commands, profiles_fetch_what_they_build)
{
    project.write("otok.toml", manifest_toml + "[deps.c]\nrecipe = { path = \"recipes/c\" }\nplatforms = [\"ios\"]\n");
    project.write("recipes/c/recipe.toml", recipe_of("c", "1.0"));

    // the profile builds only for iossim, where c is not built
    ASSERT_EQ(run({"install", "--profile", "ci"}), 0) << err.str();
    EXPECT_NE(out.str().find("inputs in the cache: 2 sources\n"), std::string::npos) << out.str();
    EXPECT_FALSE(std::filesystem::exists(unpacked("c")));

    // fetch: every package of the lock
    ASSERT_EQ(run({"fetch"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock is up to date\ninputs in the cache: 3 sources\n");  // fetch builds nothing
    EXPECT_EQ(err.str().find("downloading " + urls["c"] + "\n"), 0u) << err.str();  // then the warning about the profile
    EXPECT_TRUE(std::filesystem::exists(unpacked("c") / "c.c"));

    EXPECT_EQ(run({"install", "--profile", "nightly"}), 1);
    EXPECT_NE(err.str().find("error: unknown profile 'nightly'; otok.toml defines 'ci'"), std::string::npos) << err.str();
}

TEST_F(commands, resources)
{
    upstream.write("data.bin", "abc");
    std::string const data_url = file_url(upstream.path() / "data.bin");
    std::string const abc_sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    project.write("recipes/b/recipe.toml", recipe_of("b", "2.0", options_of_b
        + "[[resource]]\nname = \"data\"\nurl = \"" + data_url + "\"\nsha256 = \"" + abc_sha256 + "\"\nunpack = false\n"
        + "[[resource]]\nname = \"extra\"\nurl = \"" + urls["c"] + "\"\nsha256 = \"" + hashes["c"] + "\"\n"));

    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("inputs in the cache: 2 sources, 2 resources\n"), std::string::npos) << out.str();
    EXPECT_EQ(read_file(cache.path() / "downloads" / "sha256" / abc_sha256).value_or("?"), "abc");
    EXPECT_FALSE(std::filesystem::exists(cache.path() / "sources" / ("archive-" + abc_sha256)));  // kept packed
    EXPECT_EQ(read_file(unpacked("c") / "c.c").value_or("?"), "int c;\n");
}

TEST_F(commands, offline)
{
    EXPECT_EQ(run({"install", "--offline"}), 1);
    EXPECT_NE(err.str().find("error: a 1.0 (r1) source: " + urls["a"] + " is not in the cache (--offline)"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("error: b 2.0 (r1) source: " + urls["b"] + " is not in the cache (--offline)"), std::string::npos) << err.str();

    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(run({"install", "--offline"}), 0) << err.str();
    EXPECT_EQ(err.str(), "");
    EXPECT_EQ(run({"install", "--frozen"}), 0) << err.str();

    // an archive downloaded but not unpacked is unpacked offline
    std::filesystem::remove_all(unpacked("a"));
    EXPECT_EQ(run({"fetch", "--frozen"}), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(unpacked("a") / "a.c"));

    // --frozen is --locked as well
    project.write("recipes/b/recipe.toml", recipe_of("b", "2.1", options_of_b));
    EXPECT_EQ(run({"install", "--frozen"}), 1);
    EXPECT_NE(err.str().find("error: otok.lock is out of date (--locked):"), std::string::npos) << err.str();
}

TEST_F(commands, refuses_a_contradicting_lock)
{
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string edited = lock_text();
    edited.replace(edited.find(hashes["b"]), 64, std::string(64, '0'));
    project.write("otok.lock", edited);

    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("error: 'b': otok.lock records another version or other inputs than its recipe (2.0 (r1)) declares; "
                             "was otok.lock edited?; if the new contents are trusted, remove the package from otok.lock and run 'otok install'"),
        std::string::npos) << err.str();
    EXPECT_EQ(lock_text(), edited);
    EXPECT_EQ(run({"update"}), 1);
    EXPECT_EQ(lock_text(), edited);
}

TEST_F(commands, local_sources_keep_the_locked_hash)
{
    project.write("vendor/b/b.c", "int b;\n");
    project.write("vendor/b/LICENSE", "MIT\n");
    std::string text = manifest_toml;
    text.insert(text.find("[profile.ci]"), "source = { path = \"vendor/b\" }\n");
    project.write("otok.toml", text);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock created:\n  added a 1.0 (r1)\n  added b 2.0 (r1)\ninputs in the cache: 1 source\n" + not_built);
    EXPECT_EQ(run({"install", "--locked"}), 0) << err.str();

    // the changed directory is found before the build, and the lock is not rewritten
    project.write("vendor/b/b.c", "int b = 2;\n");
    std::string const before = lock_text();
    for (std::vector<std::string> const& args : {std::vector<std::string>{"install"}, {"install", "--locked"}, {"fetch"}}) {
        EXPECT_EQ(run(args), 1) << args.back();
        EXPECT_NE(err.str().find("error: b 2.0 (r1): the source in 'vendor/b' has changed since otok.lock was written (tree hash "),
            std::string::npos) << err.str();
        EXPECT_NE(err.str().find("); run 'otok update b' to accept it\n"), std::string::npos) << err.str();
        EXPECT_EQ(lock_text(), before);
    }
    EXPECT_EQ(run({"tree"}), 0) << err.str();  // reads no source

    ASSERT_EQ(run({"update", "b"}), 0) << err.str();
    EXPECT_NE(out.str().find("otok.lock updated:\n  b: inputs changed\n"), std::string::npos) << out.str();
    EXPECT_EQ(run({"install", "--locked"}), 0) << err.str();
}

// otok.dev.toml (otok-dev-toml.md 3): its working trees are fetched nothing, and the lock stays as it was
TEST_F(commands, dev_working_trees_leave_the_lock)
{
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const before = lock_text();
    project.write("work/b/b.c", "int b = 2;\n");
    project.write("work/b/LICENSE", "MIT\n");
    project.write("otok.dev.toml", "[deps.b]\nsource = \"work/b\"\n");
    ASSERT_EQ(run({"install", "--locked"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock is up to date\ninputs in the cache: 1 source\n" + not_built);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(lock_text(), before);
    ASSERT_EQ(run({"tree"}), 0) << err.str();
    EXPECT_EQ(out.str(),
        "a 1.0 (r1) path recipes/a [ios, iossim]\n"
        "└── b 2.0 (r1) path recipes/b [ios, iossim] {shared = true} (dev)\n"
        "b 2.0 (r1) path recipes/b [ios, iossim] {shared = true} (dev) (*)\n");
    ASSERT_EQ(run({"install", "--no-dev"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock is up to date\ninputs in the cache: 2 sources\n" + not_built);
    ASSERT_EQ(run({"tree", "--no-dev"}), 0) << err.str();
    EXPECT_EQ(out.str().find("(dev)"), std::string::npos) << out.str();
}

TEST_F(commands, dev_libraries_are_dependencies)
{
    project.write("otok.dev.toml", "[deps.zlib]\nsource = \"work/zlib\"\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_EQ(err.str(), dev_file() + ":1:1: error: 'zlib' in otok.dev.toml is not a dependency of this project\n");
}

TEST_F(commands, dev_working_trees_are_directories)
{
    project.write("work/b.c", "int b;\n");
    project.write("otok.dev.toml", "[deps.b]\nsource = \"work/b.c\"\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_EQ(err.str(), dev_file() + ":1:1: error: 'b' in otok.dev.toml: the source 'work/b.c' is not a directory\n");
}

TEST_F(commands, dev_resources_are_the_recipes)
{
    project.write("work/r/x", "x\n");
    project.write("otok.dev.toml", "[deps.b]\nresources = { r = \"work/r\" }\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_EQ(err.str(), dev_file() + ":1:1: error: 'b' in otok.dev.toml: its recipe has no resource 'r'\n");
}

// A working tree in place of the source of otok.toml (6.3), which is then neither built nor checked against the lock.
TEST_F(commands, dev_source_replaces_the_manifests)
{
    project.write("vendor/b/b.c", "int b;\n");
    project.write("vendor/b/LICENSE", "MIT\n");
    std::string text = manifest_toml;
    text.insert(text.find("[profile.ci]"), "source = { path = \"vendor/b\" }\n");
    project.write("otok.toml", text);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    project.write("vendor/b/b.c", "int b = 2;\n");
    EXPECT_EQ(run({"install", "--locked"}), 1);
    project.write("work/b/b.c", "int b = 3;\n");
    project.write("work/b/LICENSE", "MIT\n");
    project.write("otok.dev.toml", "[deps.b]\nsource = \"work/b\"\n");
    EXPECT_EQ(run({"install", "--locked"}), 0) << err.str();
}

// A recipe of otok.dev.toml builds the same graph: another revision does, another default of an option does not.
TEST_F(commands, dev_recipes_keep_the_graph)
{
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const before = lock_text();
    std::string recipe = recipe_of("b", "2.0", options_of_b);
    recipe.replace(recipe.find("revision = 1"), 12, "revision = 2");
    project.write("work/recipe-b/recipe.toml", recipe);
    project.write("otok.dev.toml", "[deps.b]\nrecipe = \"work/recipe-b\"\n");
    ASSERT_EQ(run({"install", "--locked"}), 0) << err.str();
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(lock_text(), before);

    recipe.replace(recipe.find("default = 1"), 11, "default = 2");
    project.write("work/recipe-b/recipe.toml", recipe);
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_EQ(err.str(), "error: the recipes of otok.dev.toml change the project; such a change is made in otok.toml (otok-dev-toml.md 3):\n"
                         "  'b': options\n"
                         "  'b': options in profile 'ci'\n");
    EXPECT_EQ(lock_text(), before);
    EXPECT_EQ(run({"install", "--no-dev"}), 0) << err.str();
}

TEST_F(commands, reports_download_errors)
{
    std::string wrong = recipe_of("b", "2.0", options_of_b);
    wrong.replace(wrong.find(hashes["b"]), 64, std::string(64, '1'));  // not a hash of anything in the cache
    project.write("recipes/b/recipe.toml", wrong);
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("error: b 2.0 (r1) source: "), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("does not match its sha256"), std::string::npos) << err.str();
}

TEST_F(commands, tree)
{
    ASSERT_EQ(run({"tree"}), 0) << err.str();
    EXPECT_EQ(out.str(),
        "a 1.0 (r1) path recipes/a [ios, iossim]\n"
        "└── b 2.0 (r1) path recipes/b [ios, iossim] {shared = true}\n"
        "b 2.0 (r1) path recipes/b [ios, iossim] {shared = true} (*)\n");

    ASSERT_EQ(run({"tree", "--profile", "ci"}), 0) << err.str();
    EXPECT_EQ(out.str().substr(0, out.str().find('\n')), "profile 'ci'");
    EXPECT_NE(out.str().find("a 1.0 (r1) path recipes/a [iossim]\n"), std::string::npos) << out.str();

    EXPECT_EQ(run({"tree", "--profile", "nightly"}), 1);
    EXPECT_NE(err.str().find("error: unknown profile 'nightly'; otok.toml defines 'ci'"), std::string::npos) << err.str();
}

// A lock of another format is not read: install resolves anew and writes format 1 (otok-lock.md 2)
// A lock of another format is a newer otok's: never resolved anew over it (otok-lock.md 2, docs/PLAN.md decision 98)
TEST_F(commands, refuses_a_lock_of_another_format)
{
    std::string const newer = "# Generated by otok. Do not edit.\nformat = 2\n\n[[package]]\nlibrary = \"a\"\n";
    project.write("otok.lock", newer);
    for (std::vector<std::string> const& args : {std::vector<std::string>{"install", "--locked"}, {"install"}, {"update"}}) {
        err.str({});
        EXPECT_EQ(run(args), 1);
        EXPECT_NE(err.str().find("lock format 2 needs a newer otok: this one reads format 1"), std::string::npos) << err.str();
        EXPECT_EQ(lock_text(), newer);
    }
}

// Several target tables: a package of each (`<library>@<target>`), the graph this machine builds of each (otok-lock.md 2)
TEST_F(commands, several_targets)
{
    targets = all_targets;
    auto const of_both = [](std::string text) {
        auto const replace = [&text](std::string const& from, std::string const& to) { text.replace(text.find(from), from.size(), to); };
        replace("description = \"Test.\"\n", "description = \"Test.\"\nvariant = \"all\"\n");
        replace("supported = [\"ios\", \"iossim\", \"macosx\"]\nbuild-on = [\"macos/xcode\"]\n",
            "supported = [\"ios\", \"iossim\", \"macosx\", \"windows\"]\nbuild-on = [\"macos/xcode\", \"windows/msvc\", \"linux/clang-cl\"]\n");
        return text;
    };
    project.write("recipes/a/recipe.toml", of_both(recipe_of("a", "1.0", dependency_on_b)));
    project.write("recipes/b/recipe.toml", of_both(recipe_of("b", "2.0", options_of_b)));
    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
        "[windows]\nplatforms = [\"windows\"]\nbuild-on = [\"windows/msvc\", \"linux/clang-cl\"]\noutput = \"OtokWindows\"\n"
        "[deps.a]\nrecipe = { path = \"recipes/a\" }\n[deps.b]\nrecipe = { path = \"recipes/b\" }\n");
    // built on every system: their graphs are resolved (docs/PLAN.md decision 88)
    project.write("otok.lock", "# Generated by otok. Do not edit.\nformat = 1\nsystems = [\"linux\", \"macos\", \"windows\"]\n");

    // the windows graph whose environment covers this machine, else the first
    std::string const windows = machine_environment("").system == "linux" ? "linux/clang-cl" : "windows/msvc";
    ASSERT_EQ(run({"tree"}), 0) << err.str();
    EXPECT_EQ(out.str(),
        "[apple] macos/xcode\n"
        "a@apple 1.0 (r1) path recipes/a [ios]\n"
        "└── b@apple 2.0 (r1) path recipes/b [ios] {shared = true}\n"
        "b@apple 2.0 (r1) path recipes/b [ios] {shared = true} (*)\n"
        "[windows] " + windows + "\n"
        "a@windows 1.0 (r1) path recipes/a [windows]\n"
        "└── b@windows 2.0 (r1) path recipes/b [windows] {shared = true}\n"
        "b@windows 2.0 (r1) path recipes/b [windows] {shared = true} (*)\n");

    ASSERT_EQ(run({"why", "b"}), 0) << err.str();
    EXPECT_NE(out.str().find("b@apple 2.0 (r1) path recipes/b\n"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("b@windows 2.0 (r1) path recipes/b\n"), std::string::npos) << out.str();

    // every graph in the lock, the two of windows in one entry
    ASSERT_EQ(run({"fetch"}), 0) << err.str();
    EXPECT_NE(out.str().find("  added a@apple 1.0 (r1)\n  added a@windows (linux/clang-cl) 1.0 (r1)\n"), std::string::npos) << out.str();
    EXPECT_NE(lock_text().find("library = \"a\"\ntarget = \"windows\"\nbuild-on = [\"linux/clang-cl\", \"windows/msvc\"]\nrecipe = \"path\"\n"),
        std::string::npos) << lock_text();
    EXPECT_NE(lock_text().find("library = \"b\"\ntarget = \"apple\"\nbuild-on = [\"macos/xcode\"]\n"), std::string::npos) << lock_text();
    ASSERT_EQ(run({"install", "--locked"}), 0) << err.str();

    ASSERT_EQ(run({"metadata"}), 0) << err.str();
    auto const doc = nlohmann::json::parse(out.str());
    ASSERT_EQ(doc["packages"].size(), 4u);
    EXPECT_EQ(doc["packages"][1]["library"], "a");
    EXPECT_EQ(doc["packages"][1]["target"], "windows");
    EXPECT_EQ(doc["packages"][1]["environment"], windows);
    EXPECT_EQ(doc["packages"][1]["outputs"].size(), 1u);
    EXPECT_EQ(doc["packages"][1]["outputs"][0]["target"], "windows");
    EXPECT_EQ(doc["targets"][1]["build-on"], (nlohmann::json{"windows/msvc", "linux/clang-cl"}));
}

// docs/PLAN.md decision 88: the graphs of the systems the project is built on, this machine's added, --drop removes one
TEST_F(commands, systems_the_project_is_built_on)
{
    targets = all_targets;
    std::string const recipe = [&] {
        std::string text = recipe_of("a", "1.0");
        auto const replace = [&text](std::string const& from, std::string const& to) { text.replace(text.find(from), from.size(), to); };
        replace("description = \"Test.\"\n", "description = \"Test.\"\nvariant = \"all\"\n");
        replace("supported = [\"ios\", \"iossim\", \"macosx\"]\nbuild-on = [\"macos/xcode\"]\n",
            "supported = [\"ios\", \"iossim\", \"macosx\", \"windows\"]\nbuild-on = [\"macos/xcode\", \"windows/msvc\", \"linux/clang-cl\"]\n");
        return text;
    }();
    project.write("recipes/a/recipe.toml", recipe);
    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
        "[windows]\nplatforms = [\"windows\"]\nbuild-on = [\"windows/msvc\", \"linux/clang-cl\"]\noutput = \"OtokWindows\"\n"
        "[deps.a]\nrecipe = { path = \"recipes/a\" }\n");
    std::string const machine = machine_environment("").system;
    std::string const other = machine == "linux" ? "windows" : "linux";

    // a new lock: this machine's system only
    ASSERT_EQ(run({"fetch"}), 0) << err.str();
    EXPECT_NE(lock_text().find("format = 1\nsystems = [\"" + machine + "\"]\n"), std::string::npos) << lock_text();
    EXPECT_EQ(lock_text().find("build-on = [\"macos/xcode\"]") != std::string::npos, machine == "macos") << lock_text();

    // another system of the lock stays; --drop removes it, never this machine's
    project.write("otok.lock", "# Generated by otok. Do not edit.\nformat = 1\nsystems = [\"" + std::min(machine, other) + "\", \"" + std::max(machine, other) + "\"]\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(lock_text().find("systems = [\"" + std::min(machine, other) + "\", \"" + std::max(machine, other) + "\"]\n"), std::string::npos) << lock_text();
    ASSERT_EQ(run({"install", "--locked"}), 0) << err.str();
    ASSERT_EQ(run({"update", "--drop", other}), 0) << err.str();
    EXPECT_NE(out.str().find("removed the system " + other), std::string::npos) << out.str();
    EXPECT_NE(lock_text().find("systems = [\"" + machine + "\"]\n"), std::string::npos) << lock_text();
    EXPECT_EQ(run({"update", "--drop", machine}), 1);
    EXPECT_NE(err.str().find("--drop: '" + machine + "' is the system of this machine"), std::string::npos) << err.str();
    EXPECT_EQ(run({"update", "--drop", "bsd"}), 1);
    EXPECT_NE(err.str().find("--drop: 'bsd' is not a system of build machines"), std::string::npos) << err.str();

    // a lock without this machine's system is out of date for --locked
    project.write("otok.lock", "# Generated by otok. Do not edit.\nformat = 1\nsystems = [\"" + other + "\"]\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::string const both = "systems = [\"" + std::min(machine, other) + "\", \"" + std::max(machine, other) + "\"]\n";
    EXPECT_NE(lock_text().find(both), std::string::npos) << lock_text();
    project.write("otok.lock", "# Generated by otok. Do not edit.\nformat = 1\nsystems = [\"" + other + "\"]\n");
    EXPECT_EQ(run({"install", "--locked"}), 1);
    EXPECT_NE(err.str().find("added the system " + machine), std::string::npos) << err.str();
}

TEST_F(commands, why)
{
    ASSERT_EQ(run({"why", "b"}), 0) << err.str();
    EXPECT_EQ(out.str(),
        "b 2.0 (r1) path recipes/b\n"
        "  required by:\n"
        "    otok.toml: any version\n"
        "    a 1.0: >= 1\n"
        "  platforms: ios, iossim\n"
        "  options:\n"
        "    level = 1 (default)\n"
        "    shared = true\n"
        "      a 1.0 asks for true\n");

    EXPECT_EQ(run({"why", "zlib"}), 1);
    EXPECT_EQ(err.str(), "error: 'zlib' is not a dependency of this project\n");
}

TEST_F(commands, update)
{
    ASSERT_EQ(run({"install"}), 0) << err.str();
    project.write("recipes/c/recipe.toml", recipe_of("c", "1.0"));
    project.write("otok.toml", manifest_toml + "[deps.c]\nrecipe = { path = \"recipes/c\" }\n");
    ASSERT_EQ(run({"update", "b"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock updated:\n  added c 1.0 (r1)\ninputs in the cache: 3 sources\n" + not_built);
    EXPECT_EQ(err.str(), "downloading " + urls["c"] + "\n");
    EXPECT_EQ(run({"update", "zlib"}), 1);
    EXPECT_NE(err.str().find("error: 'zlib' is not a dependency of this project"), std::string::npos) << err.str();
}

TEST_F(commands, clean)
{
    ASSERT_EQ(run({"install"}), 0) << err.str();
    std::uintmax_t const downloads = std::filesystem::file_size(upstream.path() / "a-1.0.tar.gz")
        + std::filesystem::file_size(upstream.path() / "b-1.0.tar.gz");
    std::string const where = " in '" + path_to_utf8(cache.path()) + "'\n";

    ASSERT_EQ(run({"clean", "--dry-run"}), 0) << err.str();
    EXPECT_EQ(out.str(), "would remove downloads/ (" + std::to_string(downloads) + " B)\nwould remove sources/ (22 B)\nwould free "
        + std::to_string(downloads + 22) + " B" + where);
    EXPECT_TRUE(std::filesystem::exists(unpacked("a")));

    ASSERT_EQ(run({"clean", "--sources"}), 0) << err.str();
    EXPECT_EQ(out.str(), "removed sources/ (22 B)\nfreed 22 B" + where);
    EXPECT_FALSE(std::filesystem::exists(unpacked("a")));
    EXPECT_TRUE(std::filesystem::exists(cache.path() / "downloads"));

    // a clean cache works as before; the project is not needed
    ASSERT_EQ(run({"install", "--offline"}), 0) << err.str();
    ASSERT_EQ(run({"clean"}), 0) << err.str();
    EXPECT_FALSE(std::filesystem::exists(cache.path() / "downloads"));
    std::filesystem::remove(project.path() / "otok.toml");
    ASSERT_EQ(run({"clean"}), 0) << err.str();
    EXPECT_EQ(out.str(), "nothing to remove" + where);
}

TEST_F(commands, reports_project_errors)
{
    std::filesystem::remove(project.path() / "otok.toml");
    EXPECT_EQ(run({"tree"}), 1);
    EXPECT_NE(err.str().find("error: no otok.toml in"), std::string::npos) << err.str();

    project.write("otok.toml", manifest_toml + "[deps.c]\nrecipe = { path = \"recipes/missing\" }\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("error: the recipe of 'c' in"), std::string::npos) << err.str();
    EXPECT_FALSE(std::filesystem::exists(project.path() / "otok.lock"));

    ASSERT_EQ(run({"help", "fetch"}), 0);
    EXPECT_NE(out.str().find("usage: otok fetch [options]"), std::string::npos) << out.str();
    EXPECT_EQ(run({"tree", "--locked"}), 2);  // tree takes --offline only
}

TEST_F(commands, collects_unused_cache_entries)
{
    auto const age = [](std::filesystem::path const& p, int days) {
        std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now() - std::chrono::hours{24 * days});
    };
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(cache.path() / "gc-stamp"));  // collected, nothing to remove
    std::filesystem::path const old = cache.path() / "sources" / "archive-old";
    std::filesystem::create_directories(old);
    std::ofstream{old / "f", std::ios::binary} << "12345";
    age(old, 40);
    age(unpacked("a"), 40);  // used by the install below

    // at most once a day; turned off with 0
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(old));
    age(cache.path() / "gc-stamp", 2);
    env_vars["OTOK_CACHE_MAX_AGE"] = "0";
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(old));

    env_vars.clear();
    age(unpacked("a"), 40);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("removed 1 cache entry unused for 30 days (5 B)\n"), std::string::npos) << out.str();
    EXPECT_FALSE(std::filesystem::exists(old));
    EXPECT_TRUE(std::filesystem::exists(unpacked("a")));

    // by hand
    ASSERT_EQ(run({"clean", "--unused", "--days", "0", "--dry-run", "--sources"}), 0) << err.str();
    EXPECT_EQ(out.str(), "would remove 2 cache entries (22 B)\n");
    ASSERT_EQ(run({"clean", "--unused"}), 0) << err.str();
    EXPECT_EQ(out.str(), "removed 0 cache entries unused for 30 days (0 B)\n");
    EXPECT_EQ(run({"clean", "--days", "3"}), 1);
    EXPECT_EQ(err.str(), "error: --days needs --unused\n");
    EXPECT_EQ(run({"clean", "--unused", "--days", "x"}), 1);
    EXPECT_EQ(err.str(), "error: --days must be a number of 0 or more, not 'x'\n");
}

namespace {

// The JSON of `otok metadata` in the project directory `dir` (absolute) with `args`, its exit code in
// `code`; null when it printed nothing parseable.
nlohmann::json metadata_of(std::filesystem::path const& dir, std::filesystem::path const& cache_dir, std::vector<std::string> args, int& code)
{
    std::ostringstream out, err;
    args.insert(args.begin(), "metadata");
    auto parsed = parse_command_line(args, global_options(), command_specs());
    EXPECT_TRUE(parsed);
    environment const env = [cache_dir](std::string_view name) -> std::optional<std::string> {
        if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache_dir);
        return std::nullopt;
    };
    command_context ctx{dir, all_targets, nullptr, out, err, env};
    code = run_command(*parsed, ctx);
    EXPECT_EQ(code, 0) << err.str();
    EXPECT_TRUE(out.str().ends_with("}\n")) << out.str();
    return nlohmann::json::parse(out.str(), nullptr, false);
}

// The values that differ between machines and checkouts, as metadata.md writes them: the project
// directory as /work/app, 64-digit hashes (line ends change tree hashes) and the otok version as "…".
void normalize(nlohmann::json& j, std::string const& dir)
{
    if (j.is_object()) {
        for (auto& [key, value] : j.items()) {
            if (key == "otok" && value.is_string()) value = "…";
            else normalize(value, dir);
        }
    } else if (j.is_array()) {
        for (auto& value : j) normalize(value, dir);
    } else if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos) s = "…";
        if (s.starts_with(dir)) s = "/work/app" + s.substr(dir.size());
        j = s;
    }
}

}

// The example of metadata.md, read from the document itself, is what otok prints for it.
// docs/PLAN.md decision 44: recipes published into a git index with `publish --no-build`, then installed
// from it.
TEST_F(commands, installs_recipes_from_the_index)
{
    auto git = find_git(current_environment());
    if (!git) GTEST_SKIP() << "git is not in PATH";
    auto const git_in = [&](std::filesystem::path const& dir, std::vector<std::string> args) {
        std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
        full.insert(full.end(), args.begin(), args.end());
        auto done = run_git(*git, full, dir, upstream.path() / "setup.log");
        EXPECT_TRUE(done.has_value()) << (done ? "" : done.error().message);
    };
    // a repository of recipes, which is its own remote
    upstream.write("recipes/a/recipe.toml", recipe_of("a", "1.0", privacy + dependency_on_b));
    upstream.write("recipes/b/recipe.toml", recipe_of("b", "2.0", privacy + options_of_b));
    upstream.write("recipes/a/PrivacyInfo.xcprivacy", privacy_manifest);
    upstream.write("recipes/b/PrivacyInfo.xcprivacy", privacy_manifest);
    std::filesystem::path const recipes = upstream.path() / "recipes";
    git_in(recipes, {"init", "-q"});
    git_in(recipes, {"add", "."});
    git_in(recipes, {"commit", "-q", "-m", "recipes"});
    git_in(recipes, {"remote", "add", "origin", file_url(recipes)});
    // the index, a checkout committed to (docs/PLAN.md decision 46)
    upstream.write("index/index.toml", "format = 1\n");
    std::filesystem::path const index = upstream.path() / "index";
    git_in(index, {"init", "-q"});
    git_in(index, {"add", "."});
    git_in(index, {"commit", "-q", "-m", "index"});
    git_in(index, {"config", "user.name", "otok test"});
    git_in(index, {"config", "user.email", "test@otok.invalid"});

    std::string const index_arg = path_to_utf8(index);
    ASSERT_EQ(run({"publish", "--no-build", "--no-push", "--index", index_arg, "--publisher", "p", path_to_utf8(recipes / "a")}), 0) << err.str();
    EXPECT_EQ(out.str(), "added recipes/p/a/apple/1.0-r1.toml\ncommitted to the index; push it to publish\n");
    ASSERT_EQ(run({"publish", "--no-build", "--no-push", "--index", index_arg, "--publisher", "p", path_to_utf8(recipes / "b")}), 0) << err.str();
    ASSERT_EQ(run({"publish", "--no-build", "--no-push", "--index", index_arg, "--publisher", "p", path_to_utf8(recipes / "a")}), 0) << err.str();
    EXPECT_EQ(out.str(), "p/a 1.0-r1 (apple) is published already\n");
    // nothing here builds Apple platforms
    EXPECT_EQ(run({"publish", "--dry-run", "--index", index_arg, "--publisher", "p", path_to_utf8(recipes / "a")}), 1);
    EXPECT_NE(err.str().find("p/a 1.0-r1 (apple): this otok cannot build for 'apple' on this machine"),
        std::string::npos) << err.str();
    EXPECT_EQ(run({"publish", "--dry-run", "--no-build", "--index", index_arg}), 1);
    EXPECT_NE(err.str().find("--dry-run builds the recipe and publishes nothing, so it cannot go with --no-build"), std::string::npos) << err.str();
    upstream.write("recipes/a/notes.txt", "not committed\n");
    EXPECT_EQ(run({"publish", "--no-build", "--index", index_arg, "--publisher", "p", path_to_utf8(recipes / "a")}), 1);
    EXPECT_NE(err.str().find("has changes that are not committed"), std::string::npos) << err.str();
    std::filesystem::remove(recipes / "a" / "notes.txt");

    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
                               "[deps.a]\nrecipe = \"p/a\"\nversion = \">= 1\"\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the recipe 'p/a' of 'a' needs the index: no recipe index: 'index' in config.toml (or OTOK_INDEX) is empty"), std::string::npos) << err.str();

    env_vars["OTOK_INDEX"] = file_url(index);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock created:\n  added a 1.0 (r1)\n  added b 2.0 (r1)\ninputs in the cache: 2 sources\n" + not_built);
    diagnostics d;
    auto lock = parse_lock(lock_text(), "otok.lock", d);
    ASSERT_TRUE(lock.has_value());
    locked_package const* a = test::locked(*lock, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->kind, locked_package::recipe_kind::index);
    EXPECT_EQ(a->id.str(), "p/a");
    EXPECT_EQ(a->recipe_source.git, file_url(recipes));
    EXPECT_EQ(a->recipe_source.path, "a");
    EXPECT_EQ(a->recipe_sha256, tree_hash(cache.path() / "sources" / ("git-" + a->recipe_source.commit + "-ca978112ca1bbdca")).value_or("?"));
    ASSERT_NE(test::locked(*lock, "b"), nullptr);
    EXPECT_EQ(test::locked(*lock, "b")->id.str(), "p/b");

    ASSERT_EQ(run({"install", "--frozen"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock is up to date\ninputs in the cache: 2 sources\n" + not_built);
}

// docs/PLAN.md decision 95: the index of config.toml (`default`) and one of the project (otok-toml.md 6.4).
TEST_F(commands, takes_recipes_from_several_indexes)
{
    auto git = find_git(current_environment());
    if (!git) GTEST_SKIP() << "git is not in PATH";
    auto const git_in = [&](std::filesystem::path const& dir, std::vector<std::string> args) {
        std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
        full.insert(full.end(), args.begin(), args.end());
        auto done = run_git(*git, full, dir, upstream.path() / "setup.log");
        EXPECT_TRUE(done.has_value()) << (done ? "" : done.error().message);
    };
    // the recipes, a repository that is its own remote
    upstream.write("recipes/a/recipe.toml", recipe_of("a", "1.0", privacy + dependency_on_b));
    upstream.write("recipes/b/recipe.toml", recipe_of("b", "2.0", privacy + options_of_b));
    upstream.write("recipes/c/recipe.toml", recipe_of("c", "1.0", privacy));
    for (std::string const r : {"a", "b", "c"}) upstream.write("recipes/" + r + "/PrivacyInfo.xcprivacy", privacy_manifest);
    std::filesystem::path const recipes = upstream.path() / "recipes";
    git_in(recipes, {"init", "-q"});
    git_in(recipes, {"add", "."});
    git_in(recipes, {"commit", "-q", "-m", "recipes"});
    git_in(recipes, {"remote", "add", "origin", file_url(recipes)});
    // two indexes: p/a and p/b in the first, corp/c and p/b again (the same entry) in the second
    std::map<std::string, std::filesystem::path> index;
    for (std::string const name : {"public", "corp"}) {
        index[name] = upstream.path() / name;
        upstream.write(name + "/index.toml", "format = 1\n");
        git_in(index[name], {"init", "-q"});
        git_in(index[name], {"add", "."});
        git_in(index[name], {"commit", "-q", "-m", "index"});
        git_in(index[name], {"config", "user.name", "otok test"});
        git_in(index[name], {"config", "user.email", "test@otok.invalid"});
    }
    auto const publish = [&](std::string const& to, std::string const& publisher, std::string const& recipe) {
        ASSERT_EQ(run({"publish", "--no-build", "--no-push", "--index", path_to_utf8(index[to]), "--publisher", publisher, path_to_utf8(recipes / recipe)}), 0)
            << err.str();
    };
    publish("public", "p", "b");
    publish("public", "p", "a");
    publish("corp", "p", "b");
    publish("corp", "corp", "c");
    env_vars["OTOK_INDEX"] = file_url(index["public"]);

    std::string const head = "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
                             "[deps.a]\nrecipe = \"p/a\"\nversion = \">= 1\"\n[deps.c]\nrecipe = \"corp/c\"\nversion = \">= 1\"\n"
                             "[indexes.corp]\nurl = \"" + file_url(index["corp"]) + "\"\n";
    // p/b, which p/a depends on, is in both: no priority, an error even for the same entry
    project.write("otok.toml", head);
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the recipe 'p/b' of 'b' needs the index: the recipe 'p/b' is in the indexes 'default', 'corp'; choose one in [index-of] of "
                             "otok.toml, as \"p/b\" = 'default' (otok-toml.md 6.4)"), std::string::npos) << err.str();
    EXPECT_FALSE(std::filesystem::exists(project.path() / "otok.lock"));

    // the project chooses, for the dependency of a recipe too; the lock records it
    project.write("otok.toml", head + "[index-of]\n\"p/b\" = \"corp\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock created:\n  added a 1.0 (r1)\n  added b 2.0 (r1)\n  added c 1.0 (r1)\ninputs in the cache: 3 sources\n" + not_built);
    diagnostics d;
    auto lock = parse_lock(lock_text(), "otok.lock", d);
    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(test::locked(*lock, "a")->index, "");
    EXPECT_EQ(test::locked(*lock, "b")->index, "corp");
    EXPECT_EQ(test::locked(*lock, "c")->index, "corp");
    EXPECT_NE(lock_text().find("recipe = \"p/b\"\nvariant = \"apple\"\nindex = \"corp\"\n"), std::string::npos) << lock_text();
    ASSERT_EQ(run({"install", "--locked"}), 0) << err.str();
    ASSERT_EQ(run({"tree"}), 0) << err.str();
    EXPECT_NE(out.str().find("p/b (index corp)"), std::string::npos) << out.str();

    // choosing the other index changes the lock
    project.write("otok.toml", head + "[index-of]\n\"p/b\" = \"default\"\n");
    EXPECT_EQ(run({"install", "--locked"}), 1);
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("b: index corp -> default"), std::string::npos) << out.str();
    EXPECT_EQ(test::locked(*parse_lock(lock_text(), "otok.lock", d), "b")->index, "");

    // an index owning a publisher is the only one its ids are looked up in
    project.write("otok.toml", head + "publishers = [\"p\"]\n");
    ASSERT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("the recipe 'p/a' of 'a' needs the index: the index 'corp' has no recipe 'p/a'"), std::string::npos) << err.str();

    // audit reads the advisories of the package's index
    project.write("otok.toml", head + "[index-of]\n\"p/b\" = \"corp\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    upstream.write("corp/advisories/2026-0009.toml", "id = \"2026-0009\"\nrecipes = [\"p/b\"]\nversions = [\"2.0\"]\nseverity = \"security\"\n"
                                                    "status = \"confirmed\"\nsummary = \"Leaks.\"\npublished = 2026-11-05T10:00:00Z\n");
    git_in(index["corp"], {"add", "."});
    git_in(index["corp"], {"commit", "-q", "-m", "advisory"});
    EXPECT_EQ(run({"audit"}), 1);
    EXPECT_NE(err.str().find("error: b 2.0 (r1) from 'p/b' has advisory 2026-0009 (security): Leaks."), std::string::npos) << err.str();

    // the queries name the index of each recipe when there are several
    ASSERT_EQ(run({"search", "test"}), 0) << err.str();
    EXPECT_EQ(out.str(), "p/a     default  1.0 (apple)  Test.\np/b     default  2.0 (apple)  Test.\ncorp/c  corp     1.0 (apple)  Test.\n"
                         "p/b     corp     2.0 (apple)  Test.\n");
    ASSERT_EQ(run({"info", "corp/c"}), 0) << err.str();
    EXPECT_NE(out.str().find("corp/c: Test.\nindex: corp\nlibrary: c\n"), std::string::npos) << out.str();
    ASSERT_EQ(run({"info", "p/a"}), 0) << err.str();
    EXPECT_EQ(out.str().find("index:"), std::string::npos) << out.str();
    ASSERT_EQ(run({"info", "p/b"}), 0) << err.str();
    EXPECT_NE(out.str().find("p/b: Test.\nindex: corp\n"), std::string::npos) << out.str();  // as [index-of] says
}

// docs/PLAN.md decision 45: advisories, deprecations and the queries of the index. The index is read from
// a directory (index-mirror), the recipe trees from a git repository.
TEST_F(commands, checks_advisories_and_queries_the_index)
{
    auto git = find_git(current_environment());
    if (!git) GTEST_SKIP() << "git is not in PATH";
    auto const git_in = [&](std::filesystem::path const& dir, std::vector<std::string> args) {
        std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
        full.insert(full.end(), args.begin(), args.end());
        auto done = run_git(*git, full, dir, upstream.path() / "setup.log");
        EXPECT_TRUE(done.has_value()) << (done ? "" : done.error().message);
    };
    std::filesystem::path const recipes = upstream.path() / "recipes";
    std::filesystem::path const index = upstream.path() / "index";
    std::string const index_arg = path_to_utf8(index);
    auto const publish = [&](std::string const& dir) {
        git_in(recipes, {"add", "."});
        git_in(recipes, {"commit", "-q", "-m", dir});
        ASSERT_EQ(run({"publish", "--no-build", "--no-push", "--index", index_arg, "--publisher", "p", path_to_utf8(recipes / dir)}), 0) << err.str();
    };
    upstream.write("index/index.toml", "format = 1\n");
    git_in(upstream.path(), {"init", "-q", "index"});
    git_in(index, {"add", "."});
    git_in(index, {"commit", "-q", "-m", "index"});
    git_in(index, {"config", "user.name", "otok test"});
    git_in(index, {"config", "user.email", "test@otok.invalid"});
    upstream.write("recipes/b/recipe.toml", recipe_of("b", "2.0", privacy + options_of_b));
    upstream.write("recipes/b/PrivacyInfo.xcprivacy", privacy_manifest);
    git_in(upstream.path(), {"init", "-q", "recipes"});
    git_in(recipes, {"remote", "add", "origin", file_url(recipes)});
    publish("b");
    upstream.write("recipes/a/recipe.toml", recipe_of("a", "1.0", privacy + dependency_on_b));
    upstream.write("recipes/a/PrivacyInfo.xcprivacy", privacy_manifest);
    publish("a");
    upstream.write("recipes/a/recipe.toml", recipe_of("a", "1.1", privacy + dependency_on_b));
    publish("a");
    env_vars["OTOK_INDEX_MIRROR"] = index_arg;
    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
                               "[deps.a]\nrecipe = \"p/a\"\nversion = \">= 1\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_EQ(out.str(), "otok.lock created:\n  added a 1.1 (r1)\n  added b 2.0 (r1)\ninputs in the cache: 2 sources\n" + not_built);

    // a withdrawn version stops install, which keeps the lock (index.md 7.1)
    upstream.write("index/advisories/2026-0001.toml", "id = \"2026-0001\"\nrecipes = [\"p/a\"]\nversions = [\"1.1\"]\nseverity = \"withdrawn\"\n"
                                                      "status = \"under-review\"\nsummary = \"Builds the wrong thing.\"\npublished = 2026-11-02T10:00:00Z\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("error: a 1.1 (r1) from 'p/a' has advisory 2026-0001 (withdrawn): Builds the wrong thing.; "
                             "--allow-advisory=2026-0001 installs it all the same"), std::string::npos) << err.str();
    EXPECT_EQ(run({"audit"}), 1);
    EXPECT_NE(err.str().find("error: a 1.1 (r1) from 'p/a' has advisory 2026-0001 (withdrawn): Builds the wrong thing."), std::string::npos) << err.str();
    ASSERT_EQ(run({"install", "--allow-advisory=2026-0001"}), 0) << err.str();
    EXPECT_NE(err.str().find("warning: a 1.1 (r1) from 'p/a' has advisory 2026-0001 (withdrawn)"), std::string::npos) << err.str();

    // outdated: update would move away from the avoided version
    ASSERT_EQ(run({"outdated"}), 0) << err.str();
    EXPECT_EQ(out.str(), "library  current   compatible  latest    recipe\n"
                         "a        1.1 (r1)  1.0 (r1)    1.1 (r1)  p/a\n"
                         "b        2.0 (r1)  2.0 (r1)    2.0 (r1)  p/b\n");
    ASSERT_EQ(run({"update"}), 0) << err.str();
    EXPECT_NE(out.str().find("a: 1.1 (r1) -> 1.0 (r1)"), std::string::npos) << out.str();

    // broken warns; deprecated warns
    upstream.write("index/advisories/2026-0002.toml", "id = \"2026-0002\"\nlibrary = \"b\"\nversions = [\"< 3\"]\nseverity = \"broken\"\n"
                                                      "status = \"confirmed\"\nsummary = \"Slow.\"\npublished = 2026-11-03T10:00:00Z\n");
    upstream.write("index/recipes/p/b/recipe-status.toml", "[deprecated]\nreason = \"unmaintained\"\nin-favor-of = \"q/b\"\n");
    EXPECT_EQ(run({"audit"}), 0) << err.str();
    EXPECT_NE(err.str().find("warning: b 2.0 (r1) from 'p/b' has advisory 2026-0002 (broken): Slow."), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("warning: b 2.0 (r1) from 'p/b' is deprecated: unmaintained; use 'q/b' instead"), std::string::npos) << err.str();
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(err.str().find("warning: b 2.0 (r1) from 'p/b' is deprecated: unmaintained; use 'q/b' instead"), std::string::npos) << err.str();

    // a refused license
    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
                               "[deps.a]\nrecipe = \"p/a\"\nversion = \">= 1\"\n[policy]\ndeny-licenses = [\"MIT\"]\n");
    EXPECT_EQ(run({"audit"}), 1);
    EXPECT_NE(err.str().find("error: a 1.0 (r1) is licensed under 'MIT', and [policy] deny-licenses forbids it"), std::string::npos) << err.str();

    // the queries
    ASSERT_EQ(run({"search", "TEST"}), 0) << err.str();
    EXPECT_EQ(out.str(), "p/a  1.1 (apple)  Test.\np/b  2.0 (apple)  Test.\n");
    ASSERT_EQ(run({"search", "zzz"}), 0) << err.str();
    EXPECT_EQ(out.str(), "no recipes match 'zzz'\n");
    ASSERT_EQ(run({"info", "b"}), 0) << err.str();
    EXPECT_EQ(out.str(), "recipes of 'b':\np/b  2.0 (apple)  Test.\n");
    upstream.write("index/availability.toml", "checked = 2026-11-02T03:00:00Z\n[[unavailable]]\nrecipe = \"p/a\"\nversion = \"1.0-r1\"\n"
                                              "since = 2026-10-26T03:00:00Z\ninputs = [\"source https://example.com/a.tar.gz\"]\n");
    ASSERT_EQ(run({"info", "p/a"}), 0) << err.str();
    EXPECT_NE(out.str().find("p/a: Test.\nlibrary: a\nlicense: MIT\nversions:\n"
                             "  1.1 (r1)  apple  advisory 2026-0001 (withdrawn): Builds the wrong thing.\n"
                             "  1.0 (r1)  apple  inputs unavailable since 2026-10-26\n"),
        std::string::npos) << out.str();
    EXPECT_NE(out.str().find("dependencies of 1.1 (r1):\n  b  >= 1  p/b\n"), std::string::npos) << out.str();
    ASSERT_EQ(run({"info", "p/b"}), 0) << err.str();
    EXPECT_NE(out.str().find("status: deprecated: unmaintained; use 'q/b' instead\n"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("options of 2.0 (r1):\n  level   int   default 1\n  shared  bool  default false\n"), std::string::npos) << out.str();
    EXPECT_EQ(run({"info", "p/zz"}), 1);
    EXPECT_NE(err.str().find("the index has no recipe 'p/zz'"), std::string::npos) << err.str();

    // the same through the HTTPS view of the index (index.md 8)
    std::filesystem::path const site = upstream.path() / "site";
    EXPECT_EQ(run({"index-view", index_arg, "--output", path_to_utf8(site)}), 1);
    EXPECT_NE(err.str().find("error: the checkout of the index has changes that are not committed"), std::string::npos) << err.str();
    git_in(index, {"add", "."});
    git_in(index, {"commit", "-q", "-m", "advisories"});
    ASSERT_EQ(run({"index-view", index_arg, "--output", path_to_utf8(site)}), 0) << err.str();
    EXPECT_NE(out.str().find(": 11 files written, 0 removed, 0 unchanged\n"), std::string::npos) << out.str();
    // the catalog beside it (docs/PLAN.md decision 49), which leaves the view alone
    ASSERT_EQ(run({"index-catalog", index_arg, "--output", path_to_utf8(site)}), 0) << err.str();
    EXPECT_NE(out.str().find("catalog of "), std::string::npos) << out.str();
    EXPECT_EQ(err.str(), "");  // every recipe tree fetched
    EXPECT_TRUE(std::filesystem::exists(site / "recipes" / "p" / "a" / "index.html"));
    ASSERT_EQ(run({"index-view", index_arg, "--output", path_to_utf8(site)}), 0) << err.str();
    EXPECT_NE(out.str().find(": 0 files written, 0 removed, 11 unchanged\n"), std::string::npos) << out.str();
    std::vector<std::string> requested;
    http = [&](http_exchange const& e) -> result<http_reply> {
        requested.push_back(e.url);
        http_reply reply;
        std::string const prefix = "https://view.example.com/";
        if (!e.url.starts_with(prefix)) return fail("unexpected URL " + e.url);
        auto body = read_file(site / path_from_utf8(e.url.substr(prefix.size())));
        reply.status = body ? 200 : 404;
        if (body) reply.body = std::move(*body);
        return reply;
    };
    env_vars.erase("OTOK_INDEX_MIRROR");
    env_vars["OTOK_INDEX"] = "view+https://view.example.com";
    ASSERT_EQ(run({"search", "TEST"}), 0) << err.str();
    EXPECT_EQ(out.str(), "p/a  1.1 (apple)  Test.\np/b  2.0 (apple)  Test.\n");
    EXPECT_EQ(requested, std::vector<std::string>{"https://view.example.com/index/recipes.json"});
    ASSERT_EQ(run({"info", "p/a"}), 0) << err.str();
    EXPECT_NE(out.str().find("  1.0 (r1)  apple  inputs unavailable since 2026-10-26\n"), std::string::npos) << out.str();
    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
                               "[deps.a]\nrecipe = \"p/a\"\nversion = \">= 1\"\n");
    ASSERT_EQ(run({"install", "--allow-advisory=2026-0001"}), 0) << err.str();
    EXPECT_EQ(run({"publish", "--no-build", "--index", "view+https://view.example.com", path_to_utf8(recipes / "a")}), 1);
    EXPECT_EQ(err.str(), "error: the index.toml of the view https://view.example.com names no 'repository' to publish to\n");
    env_vars["OTOK_INDEX_MIRROR"] = path_to_utf8(site);  // a view on disk
    requested.clear();
    ASSERT_EQ(run({"info", "b"}), 0) << err.str();
    EXPECT_EQ(out.str(), "recipes of 'b':\np/b  2.0 (apple)  Test.\n");
    EXPECT_TRUE(requested.empty());
}

// docs/PLAN.md decision 98: an index holding entries and advisories of a newer otok (index.md 3.2, 7.2)
TEST_F(commands, reads_an_index_written_for_a_newer_otok)
{
    auto git = find_git(current_environment());
    if (!git) GTEST_SKIP() << "git is not in PATH";
    auto const git_in = [&](std::filesystem::path const& dir, std::vector<std::string> args) {
        std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
        full.insert(full.end(), args.begin(), args.end());
        auto done = run_git(*git, full, dir, upstream.path() / "setup.log");
        EXPECT_TRUE(done.has_value()) << (done ? "" : done.error().message);
    };
    std::filesystem::path const recipes = upstream.path() / "recipes";
    std::filesystem::path const index = upstream.path() / "index";
    std::string const index_arg = path_to_utf8(index);
    auto const publish = [&](std::string const& dir) {
        git_in(recipes, {"add", "."});
        git_in(recipes, {"commit", "-q", "-m", dir});
        ASSERT_EQ(run({"publish", "--no-build", "--no-push", "--index", index_arg, "--publisher", "p", path_to_utf8(recipes / dir)}), 0) << err.str();
    };
    upstream.write("index/index.toml", "format = 1\n");
    git_in(upstream.path(), {"init", "-q", "index"});
    git_in(index, {"add", "."});
    git_in(index, {"commit", "-q", "-m", "index"});
    git_in(index, {"config", "user.name", "otok test"});
    git_in(index, {"config", "user.email", "test@otok.invalid"});
    upstream.write("recipes/b/recipe.toml", recipe_of("b", "2.0", privacy + options_of_b));
    upstream.write("recipes/b/PrivacyInfo.xcprivacy", privacy_manifest);
    git_in(upstream.path(), {"init", "-q", "recipes"});
    git_in(recipes, {"remote", "add", "origin", file_url(recipes)});
    publish("b");
    upstream.write("recipes/a/recipe.toml", recipe_of("a", "1.0", privacy + dependency_on_b));
    upstream.write("recipes/a/PrivacyInfo.xcprivacy", privacy_manifest);
    publish("a");
    upstream.write("recipes/a/recipe.toml", recipe_of("a", "1.1", privacy + dependency_on_b));
    publish("a");
    env_vars["OTOK_INDEX_MIRROR"] = index_arg;
    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
                               "[deps.a]\nrecipe = \"p/a\"\nversion = \">= 1\"\n");
    ASSERT_EQ(run({"install"}), 0) << err.str();
    EXPECT_NE(out.str().find("added a 1.1 (r1)"), std::string::npos) << out.str();
    std::string const locked = lock_text();

    // 1.1 as a newer otok would have written it: the lock keeps it, so install stops and leaves the lock alone
    std::filesystem::path const entry = index / "recipes" / "p" / "a" / "apple" / "1.1-r1.toml";
    std::string const text = read_file(entry).value_or("");
    ASSERT_NE(text.find("api = 1\n"), std::string::npos) << text;
    upstream.write("index/recipes/p/a/apple/1.1-r1.toml", text.substr(0, text.find("api = 1\n")) + "api = 2\n" + text.substr(text.find("api = 1\n") + 8));
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("error: otok.lock has a 1.1 (r1, apple) from 'p/a', which this otok cannot read: recipe API 2 needs a newer otok: "
                             "this one reads API 1\nupdate otok, or run 'otok update a' to choose a version this otok reads"),
        std::string::npos) << err.str();
    EXPECT_EQ(lock_text(), locked);
    ASSERT_EQ(run({"outdated"}), 0) << err.str();
    EXPECT_NE(out.str().find("'p/a' has versions that need a newer otok: 1.1-r1 (apple)\n"), std::string::npos) << out.str();
    ASSERT_EQ(run({"info", "p/a"}), 0) << err.str();
    EXPECT_NE(out.str().find("  1.0 (r1)  apple\n  1.1-r1    apple  needs a newer otok: recipe API 2 needs a newer otok: this one reads API 1\n"),
        std::string::npos) << out.str();
    ASSERT_EQ(run({"update", "a"}), 0) << err.str();
    EXPECT_NE(err.str().find("warning: otok.lock has a 1.1 (r1, apple) from 'p/a', which this otok cannot read: "), std::string::npos) << err.str();
    EXPECT_NE(out.str().find("a: 1.1 (r1) -> 1.0 (r1)"), std::string::npos) << out.str();

    // a constraint that only it meets: the resolver says why
    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
                               "[deps.a]\nrecipe = \"p/a\"\nversion = \">= 1.1\"\n");
    EXPECT_EQ(run({"update"}), 1);
    EXPECT_NE(err.str().find("versions this otok cannot read (a newer otok may):\n"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("1.1-r1 (apple): recipe API 2 needs a newer otok: this one reads API 1"), std::string::npos) << err.str();
    project.write("otok.toml", "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
                               "[deps.a]\nrecipe = \"p/a\"\nversion = \">= 1\"\n");

    // an advisory of a newer severity acts as a security one; one that cannot be read warns, and fails audit
    upstream.write("index/advisories/2026-0001.toml", "id = \"2026-0001\"\nrecipes = [\"p/a\"]\nversions = [\"1.0\"]\nseverity = \"malicious\"\n"
                                                      "status = \"confirmed\"\nsummary = \"Bad.\"\npublished = 2026-11-02T10:00:00Z\n");
    upstream.write("index/advisories/2026-0002.toml", "id = \"2026-0002\"\nrecipes = [\"p/b\"]\nversions = 2\nseverity = \"broken\"\n"
                                                      "status = \"confirmed\"\nsummary = \"Slow.\"\npublished = 2026-11-02T10:00:00Z\n");
    EXPECT_EQ(run({"install"}), 1);
    EXPECT_NE(err.str().find("error: a 1.0 (r1) from 'p/a' has advisory 2026-0001 (security): Bad.; severity 'malicious' is unknown to this otok "
                             "and taken as 'security' (a newer otok knows it); --allow-advisory=2026-0001 installs it all the same"),
        std::string::npos) << err.str();
    EXPECT_NE(err.str().find("warning: the index has an advisory this otok cannot read, which a newer otok may: advisories/2026-0002.toml: "),
        std::string::npos) << err.str();
    EXPECT_EQ(run({"audit"}), 1);
    EXPECT_NE(err.str().find("error: the index has an advisory this otok cannot read, which a newer otok may: advisories/2026-0002.toml: "),
        std::string::npos) << err.str();
}

TEST(metadata, matches_the_format_document)
{
    std::string const doc = read_file(path_from_utf8(OTOK_TEST_DATA_DIR "/../../docs/formats/metadata.md")).value_or("");
    std::size_t const begin = doc.find("```json\n");
    ASSERT_NE(begin, std::string::npos);
    std::size_t const end = doc.find("\n```", begin + 8);
    nlohmann::json const expected = nlohmann::json::parse(doc.substr(begin + 8, end - begin - 8), nullptr, false);
    ASSERT_FALSE(expected.is_discarded());

    test::temp_dir dir;
    std::filesystem::path const project = std::filesystem::absolute(dir.path() / "app");
    ASSERT_TRUE(copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/tiny"), project));
    dir.write("app/otok.toml",
        "[otok]\nformat = 1\n\n[apple]\nplatforms = [\"ios\", \"iossim\"]\ndeployment = { ios = \"16.0\" }\n\n"
        "[deps.tiny-a]\nrecipe = { path = \"recipes/tiny-a\" }\nsource = { path = \"vendor/tiny-a\" }\noptions = { value = 7 }\n\n"
        "[deps.tiny-b]\nrecipe = { path = \"recipes/tiny-b\" }\nsource = { path = \"vendor/tiny-b\" }\n\n"
        "[profile.simulator.apple]\nplatforms = [\"iossim\"]\n");
    int code = 0;
    nlohmann::json actual = metadata_of(project, dir.path() / "cache", {}, code);
    ASSERT_FALSE(actual.is_discarded());
    normalize(actual, path_to_utf8(project.lexically_normal().generic_u8string()));
    EXPECT_EQ(actual, expected) << actual.dump(2);
}

TEST_F(commands, metadata)
{
    int code = 0;
    std::filesystem::path const dir = std::filesystem::absolute(project.path());
    nlohmann::json j = metadata_of(dir, cache.path(), {}, code);
    ASSERT_TRUE(j.is_object());
    EXPECT_EQ(j["project"]["lock"]["state"], "missing");
    EXPECT_FALSE(std::filesystem::exists(dir / "otok.lock"));  // never written
    ASSERT_EQ(j["packages"].size(), 2u);
    nlohmann::json const& b = j["packages"][1];
    EXPECT_EQ(b["library"], "b");
    EXPECT_EQ(b["options"], (nlohmann::json{{"level", 1}, {"shared", true}}));
    EXPECT_EQ(b["source"], (nlohmann::json{{"url", urls["b"]}, {"sha256", hashes["b"]}}));
    EXPECT_EQ(b["outputs"][0]["package-key"], nullptr);

    // after install the lock is current; a changed manifest makes it stale
    ASSERT_EQ(run({"install"}), 0) << err.str();
    j = metadata_of(dir, cache.path(), {}, code);
    EXPECT_EQ(j["project"]["lock"]["state"], "current");
    project.write("otok.toml", manifest_toml + "[deps.c]\nrecipe = { path = \"recipes/c\" }\n");
    project.write("recipes/c/recipe.toml", recipe_of("c", "1.0"));
    j = metadata_of(dir, cache.path(), {}, code);
    EXPECT_EQ(j["project"]["lock"]["state"], "stale");
    EXPECT_EQ(j["packages"].size(), 3u);

    // a profile: the targets and platforms it builds
    j = metadata_of(dir, cache.path(), {"--profile", "ci"}, code);
    EXPECT_EQ(j["project"]["profile"], "ci");
    EXPECT_EQ(j["project"]["profiles"], nlohmann::json::array({"ci"}));
    EXPECT_EQ(j["targets"][0]["platforms"], nlohmann::json::array({"iossim"}));
    EXPECT_EQ(j["targets"][0]["architectures"], (nlohmann::json{{"iossim", {"arm64", "x86_64"}}}));
    EXPECT_EQ(j["packages"][0]["platforms"], nlohmann::json::array({"iossim"}));

    // what the output's state file records
    project.write("Otok/.otok-output.toml", "# Generated by otok; do not edit.\nformat = 1\n\n[packages]\na = \"" + std::string(64, 'e') + "\"\n");
    j = metadata_of(dir, cache.path(), {}, code);
    EXPECT_EQ(j["packages"][0]["outputs"][0]["package-key"], std::string(64, 'e'));
    EXPECT_EQ(j["packages"][0]["outputs"][0]["dir"], path_to_utf8((dir / "Otok" / "Packages" / "a").lexically_normal().generic_u8string()));
}
