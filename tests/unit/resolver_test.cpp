// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Dependency resolution: docs/formats/otok-lock.md 5, recipe-toml.md 8-10, otok-toml.md 6-11,
// docs/PLAN.md decision 11. Recipes come from an in-memory provider, so the search is tested with
// any number of versions.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "otok/cache/store.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/tree_hash.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/resolver/locking.hpp"
#include "otok/resolver/options.hpp"
#include "otok/resolver/provider.hpp"
#include "otok/resolver/resolver.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "otok/target/windows/windows_target.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

target_info const* const apple_targets[] = {&target::apple::info()};
target_info const* const recipe_targets[] = {&target::apple::info(), &test::native_info(), &target::android::info(), &target::windows::info(),
    &target::linux::info()};  // what test recipes may name

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += format(item) + "\n";
    return text;
}

// The package of a library in the first graph that has it; of a lock, its first entry.
resolved_package const* first(resolution const& r, std::string_view library)
{
    auto it = std::ranges::find(r.packages, library, &resolved_package::library);
    return it == r.packages.end() ? nullptr : &*it;
}

locked_package const* first(lock_file const& lock, std::string_view library)
{
    auto it = std::ranges::find(lock.packages, library, &locked_package::library);
    return it == lock.packages.end() ? nullptr : &*it;
}

struct recipe_spec
{
    std::string library;
    std::string version;
    std::string extra{};                                 // tables after [[product]]
    int revision = 1;
    std::string platforms = "\"ios\", \"iossim\", \"macosx\"";
    std::string platform_keys{};                         // more keys of [platforms]
    std::string recipe_keys{};                           // more keys of [recipe]
    std::string license = "MIT";
    std::string build_on{};                              // `[platforms] build-on`; empty: of the platforms' targets
};

std::string recipe_text(recipe_spec const& s)
{
    return "[recipe]\napi = 1\nlibrary = \"" + s.library + "\"\nname = \"" + s.library + "\"\nversion = \"" + s.version
        + "\"\nrevision = " + std::to_string(s.revision) + "\nlicense = \"" + s.license
        + "\"\nlicense-files = [\"LICENSE\"]\ndescription = \"Test.\"\n" + s.recipe_keys
        + "[source]\ngit = \"https://example.com/src\"\ncommit = \"" + std::string(40, 'a') + "\"\n"
        + "[platforms]\nsupported = [" + s.platforms + "]\n" + (s.build_on.empty() ? test::build_on_line(s.platforms) : "build-on = [" + s.build_on + "]\n") + s.platform_keys
        + "[[product]]\nname = \"P\"\nlibraries = [\"" + s.library + "\"]\n" + s.extra;
}

std::string recipe_dep(std::string_view library, std::string_view constraint, std::string_view more = "",
    std::string_view table = "deps")
{
    return "[" + std::string{table} + "." + std::string{library} + "]\nversion = \"" + std::string{constraint}
        + "\"\ndefault-recipe = \"p/" + std::string{library} + "\"\n" + std::string{more};
}

// Index-like recipes held in memory: recipe id `<publisher>/<library>`.
class memory_provider : public recipe_provider
{
public:
    void add(recipe_spec const& s, std::string const& publisher = "p")
    {
        diagnostics d;
        auto r = parse_recipe(recipe_text(s), s.library + "/recipe.toml", recipe_targets, {}, d);
        if (!r) {
            ADD_FAILURE() << messages(d);
            return;
        }
        candidate c;
        c.kind = locked_package::recipe_kind::index;
        c.id = {publisher, r->name};
        c.recipe_source.type = locked_location::kind::git;
        c.recipe_source.git = "https://github.com/" + publisher + "/" + r->name;
        c.recipe_source.commit = std::string(40, 'b');
        c.recipe_sha256 = std::string(64, 'c');
        c.recipe = std::make_shared<recipe const>(std::move(*r));
        recipes_[s.library].push_back(std::move(c));
    }

    void withdraw(std::string const& library, std::string const& version)
    {
        for (candidate& c : recipes_[library]) {
            if (c.recipe->version->str() == version) {
                c.advisories.push_back({"2026-0001", advisory_severity::withdrawn, "Withdrawn.", {}});
            }
        }
    }

    result<std::vector<candidate>> candidates(std::string_view library, recipe_ref const& ref) override
    {
        if (ref.source != recipe_ref::kind::index) return fail("not an index recipe");
        // by the id, as an index finds them: a toolchain names the recipe, not the library (docs/PLAN.md decision 87)
        std::vector<candidate> list;
        for (auto const& [name, recipes] : recipes_) {
            for (candidate const& c : recipes) {
                if (c.id == ref.id) list.push_back(c);
            }
        }
        if (list.empty()) return fail("no recipe " + ref.id.str() + " for '" + std::string{library} + "'");
        return list;
    }

    version_order order(std::string_view) override { return version_order::dotted(); }

private:
    std::map<std::string, std::vector<candidate>, std::less<>> recipes_;
};

constexpr std::string_view manifest_header =
    "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\", \"iossim\"]\ndeployment = { ios = \"15.0\" }\n";

struct outcome
{
    std::optional<manifest> m;
    std::optional<resolution> r;
    diagnostics diags;

    std::string version(std::string_view library) const
    {
        auto const* p = r ? first(*r, library) : nullptr;
        return p ? p->chosen.recipe->version->str() : "<none>";
    }
    std::string tool_version(std::string_view library) const
    {
        auto const* p = r ? r->find_tool(library) : nullptr;
        return p ? p->chosen.recipe->version->str() : "<none>";
    }
    std::string text() const { return messages(diags); }
};

outcome run(recipe_provider& provider, std::string_view body, resolve_settings settings = {}, std::string_view header = manifest_header)
{
    outcome o;
    o.m = parse_manifest(std::string{header} + std::string{body}, "otok.toml", apple_targets,
        [](std::string const&) { return std::nullopt; }, o.diags);
    if (!o.m) {
        ADD_FAILURE() << o.text();
        return o;
    }
    o.r = resolve(*o.m, apple_targets, provider, settings, o.diags);
    return o;
}

std::string dep(std::string_view library, std::string_view constraint, std::string_view more = "")
{
    return "[deps." + std::string{library} + "]\nrecipe = \"p/" + std::string{library} + "\"\nversion = \"" + std::string{constraint}
        + "\"\n" + std::string{more};
}

option_schema schema(std::string_view toml_options, std::string_view name)
{
    diagnostics d;
    auto r = parse_recipe(recipe_text({.library = "x", .version = "1.0", .extra = std::string{toml_options}}), "recipe.toml", apple_targets, {}, d);
    EXPECT_TRUE(r.has_value()) << messages(d);
    return r ? *r->find_option(name) : option_schema{};
}

}

TEST(options, canonical_values)
{
    option_schema const libs = schema("[options.libraries]\ntype = \"list\"\ndefault = []\ndoc = \"d\"\nvalues = [\"json\", \"filesystem\", \"regex\"]\n", "libraries");
    auto v = check_option_value(libs, std::vector<std::string>{"regex", "json", "regex"});
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(std::get<std::vector<std::string>>(*v), (std::vector<std::string>{"json", "regex"}));  // order of `values`
    EXPECT_EQ(check_option_value(libs, std::vector<std::string>{"thread"}).error().message,
        "'thread' is not a value of option 'libraries' ('json', 'filesystem', 'regex')");
    EXPECT_EQ(check_option_value(libs, true).error().message, "option 'libraries' expects an array of strings, not a boolean");

    option_schema const free = schema("[options.flags]\ntype = \"list\"\ndefault = []\ndoc = \"d\"\n", "flags");
    EXPECT_EQ(std::get<std::vector<std::string>>(*check_option_value(free, std::vector<std::string>{"b", "a", "b"})),
        (std::vector<std::string>{"a", "b"}));  // sorted without `values`

    option_schema const mode = schema("[options.mode]\ntype = \"enum\"\ndefault = \"x\"\ndoc = \"d\"\nvalues = [\"x\", \"y\"]\n", "mode");
    EXPECT_EQ(check_option_value(mode, std::string{"z"}).error().message, "\"z\" is not a value of option 'mode' ('x', 'y')");
    EXPECT_EQ(format_option_value(std::vector<std::string>{"a", "b"}), "[\"a\", \"b\"]");
}

TEST(options, merging)
{
    option_schema const libs = schema("[options.libraries]\ntype = \"list\"\ndefault = [\"json\"]\ndoc = \"d\"\nadditive = true\n", "libraries");
    std::vector<option_request> requests{{std::vector<std::string>{"regex"}, "otok.toml", {}},
        {std::vector<std::string>{"filesystem", "regex"}, "p/b 1.0", {}}};
    EXPECT_EQ(std::get<std::vector<std::string>>(*merge_option("boost", libs, requests)),
        (std::vector<std::string>{"filesystem", "regex"}));  // the default is not part of the union
    EXPECT_EQ(std::get<std::vector<std::string>>(*merge_option("boost", libs, {})), (std::vector<std::string>{"json"}));

    option_schema const flag = schema("[options.tools]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\nadditive = true\n", "tools");
    std::vector<option_request> flags{{false, "otok.toml", {}}, {true, "p/b 1.0", {}}};
    EXPECT_EQ(std::get<bool>(*merge_option("protobuf", flag, flags)), true);

    option_schema const exclusive = schema("[options.no-deprecated]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\n", "no-deprecated");
    std::vector<option_request> same{{true, "otok.toml", {}}, {true, "p/b 1.0", {}}};
    EXPECT_EQ(std::get<bool>(*merge_option("openssl", exclusive, same)), true);
    EXPECT_EQ(merge_option("openssl", exclusive, flags).error().message,
        "conflicting values for option 'no-deprecated' of 'openssl' (not additive, so all requests must be equal):\n"
        "  otok.toml asks for false\n  p/b 1.0 asks for true");
}

TEST(options, conditions)
{
    merged_options const values{{"ssl", true}, {"mode", std::string{"x"}}, {"libs", std::vector<std::string>{"json"}}};
    condition c;
    c.option = "ssl";
    EXPECT_TRUE(condition_holds(c, values, "apple", "ios"));
    c.platform = std::vector<std::string>{"iossim"};
    EXPECT_FALSE(condition_holds(c, values, "apple", "ios"));
    EXPECT_TRUE(condition_holds(c, values, "apple", "iossim"));

    condition is;
    is.option = "mode";
    is.is = std::string{"y"};
    EXPECT_FALSE(condition_holds(is, values, "apple", "ios"));
    condition has;
    has.option = "libs";
    has.has = "json";
    EXPECT_TRUE(condition_holds(has, values, "apple", "ios"));
    condition os;
    os.os = std::vector<std::string>{"android"};
    EXPECT_FALSE(condition_holds(os, values, "apple", "ios"));
    EXPECT_EQ(tested_option(os), nullptr);

    // not = { … }: with the other keys, holds when its table does not
    condition negated;
    negated.negated = std::make_shared<condition const>(c);  // ssl on iossim
    EXPECT_TRUE(condition_holds(negated, values, "apple", "ios"));
    EXPECT_FALSE(condition_holds(negated, values, "apple", "iossim"));
    negated.os = std::vector<std::string>{"android"};
    EXPECT_FALSE(condition_holds(negated, values, "apple", "ios"));
    EXPECT_EQ(*tested_option(negated), "ssl");

    // the slice's architecture and environment; where they are not known (the resolver), their tests hold
    condition slice;
    slice.arch = std::vector<std::string>{"arm64"};
    slice.environment = std::vector<build_environment>{*parse_environment("windows/msvc")};
    build_environment const msvc = *parse_environment("windows/msvc");
    build_environment const clang_cl = *parse_environment("linux/clang-cl");
    EXPECT_TRUE(condition_holds(slice, values, "windows", "windows", "arm64", &msvc));
    EXPECT_FALSE(condition_holds(slice, values, "windows", "windows", "x86_64", &msvc));
    EXPECT_FALSE(condition_holds(slice, values, "windows", "windows", "arm64", &clang_cl));
    EXPECT_TRUE(condition_holds(slice, values, "windows", "windows", "", &msvc));
    EXPECT_TRUE(condition_holds(slice, values, "windows", "windows"));
    EXPECT_FALSE(condition_holds(slice, values, "windows", "windows", "", &clang_cl));
    condition any_host;
    any_host.environment = std::vector<build_environment>{*parse_environment("*/ndk")};
    build_environment const ndk = *parse_environment("macos/ndk");
    EXPECT_TRUE(condition_holds(any_host, values, "android", "android", "arm64-v8a", &ndk));
}

TEST(options, file_values)
{
    // recipe-toml.md 8.3, otok-toml.md 6.2: a path inside the project, normalized; a condition holds with a file
    option_schema const filter = schema("[options.filter]\ntype = \"file\"\ndoc = \"d\"\n", "filter");
    EXPECT_EQ(std::get<std::string>(*check_option_value(filter, std::string{"icu/./data/../filter.json"})), "icu/filter.json");
    EXPECT_EQ(check_option_value(filter, std::string{"../filter.json"}).error().message,
        "option 'filter' expects a path of a file inside the project, relative to otok.toml, not '../filter.json'");
    EXPECT_FALSE(check_option_value(filter, std::string{}).has_value());
    EXPECT_EQ(check_option_value(filter, true).error().message, "option 'filter' expects a path of a file in the project, not a boolean");
    EXPECT_EQ(std::get<std::string>(*merge_option("icu4c", filter, {})), "");

    condition c;
    c.option = "filter";
    EXPECT_TRUE(condition_holds(c, {{"filter", std::string{"f.json"}}}, "apple", "ios"));
    EXPECT_FALSE(condition_holds(c, {{"filter", std::string{}}}, "apple", "ios"));
}

TEST(resolver, only_the_project_sets_file_options)
{
    memory_provider p;
    p.add({.library = "icu", .version = "79.1", .extra = "[options.filter]\ntype = \"file\"\ndoc = \"d\"\n"});
    p.add({.library = "app", .version = "1.0", .extra = recipe_dep("icu", ">= 79", "requires-options = { filter = \"f.json\" }\n")});
    auto o = run(p, dep("app", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("p/app 1.0 sets option 'filter' of p/icu 79.1, a file option, which only the project sets"), std::string::npos) << o.text();
    auto own = run(p, dep("icu", ">= 79", "options = { filter = \"f.json\" }\n"));
    ASSERT_TRUE(own.r) << own.text();
    EXPECT_EQ(std::get<std::string>(first(*own.r, "icu")->configurations[0].options.at("filter")), "f.json");
}

TEST(resolver, chooses_the_highest_matching_version)
{
    memory_provider p;
    for (char const* v : {"1.0", "1.1", "1.2.3", "2.0"}) p.add({.library = "a", .version = v});
    auto o = run(p, dep("a", "~> 1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("a"), "1.2.3");
    EXPECT_EQ(o.r->configurations, std::vector<std::string>{""});
    EXPECT_EQ(o.r->packages[0].configurations[0].platforms, (std::vector<std::string>{"ios", "iossim"}));
    EXPECT_EQ(o.r->packages[0].required_by.size(), 1u);
    EXPECT_EQ(o.r->packages[0].required_by[0].requester, "otok.toml");
    EXPECT_EQ(o.r->packages[0].required_by[0].constraint, "~> 1.0");
}

TEST(resolver, unifies_transitive_dependencies_by_library)
{
    memory_provider p;
    for (char const* v : {"1.0", "1.2", "2.0"}) p.add({.library = "a", .version = v});
    p.add({.library = "b", .version = "1.0", .extra = recipe_dep("a", ">= 1.1")});
    auto o = run(p, dep("b", "1.0") + dep("a", "< 2"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("a"), "1.2");
    EXPECT_EQ(first(*o.r, "b")->dependencies, std::vector<std::string>{"a"});
    EXPECT_EQ(first(*o.r, "a")->required_by.size(), 2u);

    // a transitive dependency alone uses its default recipe
    o = run(p, dep("b", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("a"), "2.0");
    EXPECT_EQ(first(*o.r, "a")->chosen.id.str(), "p/a");
}

TEST(resolver, backtracks_on_a_conflict)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0"});
    p.add({.library = "a", .version = "2.0"});
    p.add({.library = "b", .version = "1.0"});
    p.add({.library = "b", .version = "2.0", .extra = recipe_dep("a", "< 2")});
    auto o = run(p, dep("a", ">= 1") + dep("b", ">= 1"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("a"), "2.0");  // chosen first (fewest candidates, then name)
    EXPECT_EQ(o.version("b"), "1.0");
}

TEST(resolver, backtracks_through_a_deeper_failure)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0"});
    p.add({.library = "a", .version = "2.0", .extra = recipe_dep("c", "~> 2.0")});
    p.add({.library = "c", .version = "1.0"});
    auto o = run(p, dep("a", ">= 1"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("a"), "1.0");
    EXPECT_EQ(first(*o.r, "c"), nullptr);
}

TEST(resolver, explains_a_failure)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0"});
    p.add({.library = "a", .version = "2.0"});
    p.add({.library = "b", .version = "1.0", .extra = recipe_dep("a", "< 2")});
    auto o = run(p, dep("a", "~> 2.0") + dep("b", ">= 1"));
    ASSERT_FALSE(o.r);
    std::string const text = o.text();
    EXPECT_NE(text.find("error: cannot resolve the dependencies: no version of 'a' satisfies all requirements"), std::string::npos) << text;
    EXPECT_NE(text.find("required by:\n    otok.toml: ~> 2.0"), std::string::npos) << text;
    EXPECT_NE(text.find("1.0 (r1): does not satisfy '~> 2.0' of otok.toml"), std::string::npos) << text;
    EXPECT_NE(text.find("2.0 (r1): then 'b' 1.0 cannot be used"), std::string::npos) << text;
    EXPECT_NE(text.find("1.0 (r1): p/a 2.0 does not satisfy '< 2' of p/b 1.0"), std::string::npos) << text;
}

// Libraries of one version each, chosen before the one at fault, do not hide it: a forced choice failing only through a
// deeper one is not a level of the explanation
TEST(resolver, explains_a_failure_behind_forced_choices)
{
    memory_provider p;
    std::string body;
    for (char const* name : {"a", "b", "c", "d", "e"}) {
        p.add({.library = name, .version = "1.0"});
        body += dep(name, ">= 1");
    }
    p.add({.library = "z", .version = "1.0", .extra = recipe_dep("w", ">= 2")});
    p.add({.library = "w", .version = "1.0"});
    auto o = run(p, body + dep("z", ">= 1"));
    ASSERT_FALSE(o.r);
    std::string const text = o.text();
    EXPECT_NE(text.find("error: cannot resolve the dependencies: 'w' 1.0 cannot be used"), std::string::npos) << text;
    EXPECT_NE(text.find("1.0 (r1): does not satisfy '>= 2' of p/z 1.0"), std::string::npos) << text;
    EXPECT_EQ(text.find("'a' 1.0 cannot be used"), std::string::npos) << text;
}

TEST(resolver, prerelease_rules)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0"});
    p.add({.library = "a", .version = "2.0.beta1", .recipe_keys = "channel = \"prerelease\"\n"});
    EXPECT_EQ(run(p, dep("a", ">= 1")).version("a"), "1.0");
    EXPECT_EQ(run(p, dep("a", "= 2.0.beta1")).version("a"), "2.0.beta1");
    EXPECT_EQ(run(p, dep("a", ">= 1", "prerelease = true\n")).version("a"), "2.0.beta1");

    auto o = run(p, dep("a", ">= 2"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("2.0.beta1 (r1): is a pre-release, which otok.toml does not allow (prerelease = true)"), std::string::npos) << o.text();
}

TEST(resolver, revisions_and_advisories)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0", .revision = 1});
    p.add({.library = "a", .version = "1.0", .revision = 2});
    p.add({.library = "a", .version = "1.1"});
    p.withdraw("a", "1.1");
    auto o = run(p, dep("a", ">= 1"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("a"), "1.0");
    EXPECT_EQ(first(*o.r, "a")->chosen.recipe->revision, 2);

    // avoided, not refused (index.md 7.1): the only version allowed is still chosen
    o = run(p, dep("a", ">= 1.1"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("a"), "1.1");
    EXPECT_EQ(first(*o.r, "a")->chosen.advisories.size(), 1u);
}

TEST(resolver, keeps_locked_versions_with_advisories)
{
    memory_provider p;
    for (char const* v : {"1.0", "1.1"}) p.add({.library = "a", .version = v});
    p.withdraw("a", "1.1");
    lock_file lock;
    locked_package a;
    a.library = "a";
    a.target = "apple";
    a.build_on = {"macos/xcode"};
    a.id = {"p", "a"};
    a.version = *version::parse("1.1");
    a.revision = 1;
    lock.packages.push_back(a);
    resolve_settings settings;
    settings.preferred = &lock;
    auto o = run(p, dep("a", ">= 1"), settings);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("a"), "1.1");
}

TEST(resolver, keeps_locked_versions)
{
    memory_provider p;
    for (char const* v : {"1.0", "1.1"}) p.add({.library = "a", .version = v});
    lock_file lock;
    locked_package a;
    a.library = "a";
    a.target = "apple";
    a.build_on = {"macos/xcode"};
    a.id = {"p", "a"};
    a.version = *version::parse("1.0");
    a.revision = 1;
    lock.packages.push_back(a);

    resolve_settings settings;
    settings.preferred = &lock;
    EXPECT_EQ(run(p, dep("a", "~> 1.0"), settings).version("a"), "1.0");
    // a locked version that no longer qualifies is replaced
    EXPECT_EQ(run(p, dep("a", ">= 1.1"), settings).version("a"), "1.1");
    // `otok update a`
    settings.unlocked = {"a"};
    EXPECT_EQ(run(p, dep("a", "~> 1.0"), settings).version("a"), "1.1");
}

TEST(resolver, recipe_choice)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0"});
    p.add({.library = "a", .version = "1.5"}, "q");
    p.add({.library = "b", .version = "1.0", .extra = recipe_dep("a", ">= 1")});
    p.add({.library = "c", .version = "1.0", .extra = "[deps.a]\nversion = \">= 1\"\ndefault-recipe = \"q/a\"\n"});

    // the project's choice wins over default recipes
    auto o = run(p, dep("b", "1.0") + dep("c", "1.0") + "[deps.a]\nrecipe = \"q/a\"\nversion = \">= 1\"\n");
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(first(*o.r, "a")->chosen.id.str(), "q/a");
    EXPECT_EQ(o.version("a"), "1.5");

    // without it, different default recipes are an error
    o = run(p, dep("b", "1.0") + dep("c", "1.0"));
    ASSERT_FALSE(o.r);
    std::string const text = o.text();
    EXPECT_NE(text.find("'a' has no recipe chosen in otok.toml, and its dependents name different default recipes:"), std::string::npos) << text;
    EXPECT_NE(text.find("p/b 1.0: p/a\n"), std::string::npos) << text;
    EXPECT_NE(text.find("p/c 1.0: q/a\n"), std::string::npos) << text;
    EXPECT_NE(text.find("choose one in otok.toml: [deps.a] recipe = \"...\""), std::string::npos) << text;
}

TEST(resolver, merges_options)
{
    std::string const options =
        "[options.libraries]\ntype = \"list\"\ndefault = []\ndoc = \"d\"\nadditive = true\nvalues = [\"json\", \"filesystem\", \"regex\"]\n"
        "[options.no-deprecated]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\n";
    memory_provider p;
    p.add({.library = "boost", .version = "1.92", .extra = options});
    p.add({.library = "b", .version = "1.0",
        .extra = recipe_dep("boost", ">= 1", "requires-options = { libraries = [\"filesystem\"], no-deprecated = true }\n")});

    auto o = run(p, dep("boost", ">= 1", "options = { libraries = [\"regex\", \"json\"], no-deprecated = true }\n") + dep("b", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    package_configuration const& boost = first(*o.r, "boost")->configurations[0];
    EXPECT_EQ(std::get<std::vector<std::string>>(boost.options.at("libraries")), (std::vector<std::string>{"json", "filesystem", "regex"}));
    EXPECT_EQ(std::get<bool>(boost.options.at("no-deprecated")), true);
    ASSERT_EQ(boost.requests.at("libraries").size(), 2u);
    EXPECT_EQ(boost.requests.at("libraries")[1].requester, "p/b 1.0");

    o = run(p, dep("boost", ">= 1", "options = { no-deprecated = false }\n") + dep("b", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("conflicting values for option 'no-deprecated' of 'boost'"), std::string::npos) << o.text();
    EXPECT_NE(o.text().find("otok.toml asks for false"), std::string::npos) << o.text();
    EXPECT_NE(o.text().find("p/b 1.0 asks for true"), std::string::npos) << o.text();

    o = run(p, dep("boost", ">= 1", "options = { threads = true }\n"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("otok.toml sets option 'threads' of p/boost 1.92, which has no such option"), std::string::npos) << o.text();

    o = run(p, dep("boost", ">= 1", "options = { no-deprecated = \"yes\" }\n"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("otok.toml sets p/boost 1.92: option 'no-deprecated' expects a boolean, not a string"), std::string::npos) << o.text();
}

TEST(resolver, conditional_dependencies_follow_options)
{
    std::string const ssl = "[options.ssl]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\nadditive = true\n"
        + recipe_dep("openssl", ">= 3", "when = { option = \"ssl\" }\n");
    memory_provider p;
    p.add({.library = "curl", .version = "8.0", .extra = ssl});
    p.add({.library = "openssl", .version = "3.5"});
    p.add({.library = "git2", .version = "1.0", .extra = recipe_dep("curl", ">= 8", "requires-options = { ssl = true }\n")});

    EXPECT_EQ(run(p, dep("curl", ">= 8")).version("openssl"), "<none>");
    EXPECT_EQ(run(p, dep("curl", ">= 8", "options = { ssl = true }\n")).version("openssl"), "3.5");
    // switched on by a dependent's request
    auto o = run(p, dep("git2", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("openssl"), "3.5");
    EXPECT_EQ(first(*o.r, "curl")->dependencies, std::vector<std::string>{"openssl"});
}

TEST(resolver, requests_never_switch_dependencies_off)
{
    std::string const tls = "[options.tls]\ntype = \"enum\"\ndefault = \"openssl\"\ndoc = \"d\"\nvalues = [\"openssl\", \"mbedtls\"]\n"
        + recipe_dep("openssl", ">= 3", "when = { option = \"tls\", is = \"openssl\" }\n");
    memory_provider p;
    p.add({.library = "curl", .version = "8.0", .extra = tls});
    p.add({.library = "openssl", .version = "3.5"});
    p.add({.library = "app", .version = "1.0", .extra = recipe_dep("curl", ">= 8", "requires-options = { tls = \"mbedtls\" }\n")});

    // The default turns the edge on; app's request would turn it off. The outcome must not depend on
    // which package the search happens to choose first, so it fails either way.
    for (std::string const& body : {dep("curl", ">= 8") + dep("app", "1.0"), dep("app", "1.0")}) {
        auto o = run(p, body);
        ASSERT_FALSE(o.r) << body;
        EXPECT_NE(o.text().find("option 'tls' of p/curl 8.0 becomes \"mbedtls\", which switches off its dependency on 'openssl'"), std::string::npos) << o.text();
        EXPECT_NE(o.text().find("p/app 1.0 asks for \"mbedtls\""), std::string::npos) << o.text();
    }
    // the project's own value is the starting point, not a request through the graph
    auto o = run(p, dep("curl", ">= 8", "options = { tls = \"mbedtls\" }\n") + dep("app", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("openssl"), "<none>");
}

TEST(resolver, platforms)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0", .extra = recipe_dep("x", ">= 1", "when = { platform = [\"ios\"] }\n")});
    p.add({.library = "x", .version = "1.0"});
    p.add({.library = "b", .version = "1.0"});
    p.add({.library = "c", .version = "1.0", .platforms = "\"ios\""});

    auto o = run(p, dep("a", "1.0") + dep("b", "1.0", "platforms = [\"iossim\"]\n"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(first(*o.r, "x")->configurations[0].platforms, std::vector<std::string>{"ios"});
    EXPECT_EQ(first(*o.r, "b")->configurations[0].platforms, std::vector<std::string>{"iossim"});

    o = run(p, dep("c", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("p/c 1.0 does not support platform 'iossim' (it supports 'ios')"), std::string::npos) << o.text();
}

TEST(resolver, linkage_needs_the_recipe)
{
    target_info const* const native[] = {&test::native_info()};
    memory_provider p;
    p.add({.library = "a", .version = "1.0", .platforms = "\"native\"", .recipe_keys = "linkage = [\"static\", \"dynamic\"]\n"});
    p.add({.library = "b", .version = "1.0", .platforms = "\"native\""});
    auto const resolve_with = [&](std::string const& body) {
        diagnostics d;
        auto m = parse_manifest("[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\nlinkage = \"dynamic\"\n" + body, "otok.toml", native, {}, d);
        EXPECT_TRUE(m) << messages(d);
        auto r = resolve(*m, native, p, {}, d);
        return std::pair{r.has_value(), messages(d)};
    };
    EXPECT_TRUE(resolve_with("[deps.a]\nrecipe = \"p/a\"\nversion = \"1.0\"\n").first);
    // static only: the target table's dynamic is refused, the dependency's static is fine
    auto const [ok, text] = resolve_with("[deps.b]\nrecipe = \"p/b\"\nversion = \"1.0\"\n");
    EXPECT_FALSE(ok);
    EXPECT_NE(text.find("p/b 1.0 cannot be built as dynamic libraries for 'native' (its recipe builds 'static'); set 'linkage' in [deps.b]"),
        std::string::npos) << text;
    EXPECT_TRUE(resolve_with("[deps.b]\nrecipe = \"p/b\"\nversion = \"1.0\"\nlinkage = \"static\"\n").first);
}

// [deps.<library>] linkage: the dependency's when the project chooses none (docs/PLAN.md decision 68)
TEST(resolver, linkage_a_dependent_asks_for)
{
    target_info const* const native[] = {&test::native_info()};
    memory_provider p;
    std::string const both = "linkage = [\"static\", \"dynamic\"]\n";
    p.add({.library = "cairo", .version = "1.0", .extra = recipe_dep("pixman", "1.0", "linkage = \"static\"\n"),
        .platforms = "\"native\"", .recipe_keys = both});
    p.add({.library = "other", .version = "1.0", .extra = recipe_dep("pixman", "1.0", "linkage = \"dynamic\"\n"),
        .platforms = "\"native\"", .recipe_keys = both});
    p.add({.library = "pixman", .version = "1.0", .platforms = "\"native\"", .recipe_keys = both});
    p.add({.library = "only-dynamic", .version = "1.0", .platforms = "\"native\"", .recipe_keys = "linkage = [\"dynamic\"]\n"});
    p.add({.library = "wants-static", .version = "1.0", .extra = recipe_dep("only-dynamic", "1.0", "linkage = \"static\"\n"),
        .platforms = "\"native\"", .recipe_keys = both});
    struct result
    {
        std::optional<resolution> r;
        std::string text;
        std::string linkage(std::string_view library) const { return first(*r, library)->configurations[0].linkage.at("native"); }
    };
    auto const resolve_with = [&](std::string const& body) {
        diagnostics d;
        auto m = parse_manifest("[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\nlinkage = \"dynamic\"\n" + body, "otok.toml", native, {}, d);
        EXPECT_TRUE(m) << messages(d);
        auto r = resolve(*m, native, p, {}, d);
        return result{std::move(r), messages(d)};
    };

    // the dependent's request before the target table's dynamic
    auto o = resolve_with(dep("cairo", "1.0"));
    ASSERT_TRUE(o.r) << o.text;
    EXPECT_EQ(o.linkage("cairo"), "dynamic");
    EXPECT_EQ(o.linkage("pixman"), "static");

    // the project's choice before the request
    o = resolve_with(dep("cairo", "1.0") + dep("pixman", "1.0", "linkage = \"dynamic\"\n"));
    ASSERT_TRUE(o.r) << o.text;
    EXPECT_EQ(o.linkage("pixman"), "dynamic");

    // two dependents asking for different ones: an error naming them, which the project's choice settles
    o = resolve_with(dep("cairo", "1.0") + dep("other", "1.0"));
    EXPECT_FALSE(o.r);
    // (the explanation indents the lines of the message)
    for (char const* part : {"the dependents of 'pixman' ask for different linkages for 'native':", "p/cairo 1.0: static", "p/other 1.0: dynamic",
             "choose one in otok.toml: [deps.pixman] linkage = \"...\""}) {
        EXPECT_NE(o.text.find(part), std::string::npos) << part << "\n" << o.text;
    }
    o = resolve_with(dep("cairo", "1.0") + dep("other", "1.0") + dep("pixman", "1.0", "linkage = \"static\"\n"));
    ASSERT_TRUE(o.r) << o.text;
    EXPECT_EQ(o.linkage("pixman"), "static");

    // a request the recipe cannot build names who asked for it
    o = resolve_with(dep("wants-static", "1.0"));
    EXPECT_FALSE(o.r);
    EXPECT_NE(o.text.find("p/only-dynamic 1.0 cannot be built as static libraries for 'native', which p/wants-static 1.0 asks for, "
                          "(its recipe builds 'dynamic'); set 'linkage' in [deps.only-dynamic]"), std::string::npos) << o.text;
}

TEST(resolver, cxx_standard_within_the_recipe)
{
    target_info const* const native[] = {&test::native_info()};
    memory_provider p;
    p.add({.library = "a", .version = "1.0", .platforms = "\"native\"", .recipe_keys = "cxx-standard = \">= 17, <= 20\"\n"});
    auto const resolve_with = [&](std::string const& body) {
        diagnostics d;
        auto m = parse_manifest("[otok]\nformat = 1\n[native]\n" + body + "[deps.a]\nrecipe = \"p/a\"\nversion = \"1.0\"\n", "otok.toml", native, {}, d);
        EXPECT_TRUE(m) << messages(d);
        auto r = resolve(*m, native, p, {}, d);
        return std::pair{r.has_value(), messages(d)};
    };
    EXPECT_TRUE(resolve_with("").first);  // no standard: the recipe chooses
    EXPECT_TRUE(resolve_with("cxx-standard = 20\n").first);
    auto const [ok, text] = resolve_with("cxx-standard = 23\n");
    EXPECT_FALSE(ok);
    EXPECT_NE(text.find("p/a 1.0 cannot be built with C++23 for 'native' (its recipe builds with '>= 17, <= 20'); the project sets it in [native]"),
        std::string::npos) << text;
    EXPECT_FALSE(resolve_with("cxx-standard = 14\n").first);
}

// Without a standard of the project's, the lowest every recipe of the target allows (otok-toml.md 5.1).
TEST(resolver, chooses_the_cxx_standard_of_a_target)
{
    target_info const* const native[] = {&test::native_info()};
    auto const resolve_with = [&](std::string const& a, std::string const& b, std::string const& table, std::string const& profile = "") {
        memory_provider p;
        p.add({.library = "a", .version = "1.0", .platforms = "\"native\"", .recipe_keys = a.empty() ? "" : "cxx-standard = \"" + a + "\"\n"});
        p.add({.library = "b", .version = "1.0", .platforms = "\"native\"", .recipe_keys = b.empty() ? "" : "cxx-standard = \"" + b + "\"\n"});
        p.add({.library = "c", .version = "1.0", .platforms = "\"native\""});
        diagnostics d;
        auto m = parse_manifest("[otok]\nformat = 1\n[native]\n" + table + "[deps.a]\nrecipe = \"p/a\"\nversion = \"1.0\"\n"
            "[deps.b]\nrecipe = \"p/b\"\nversion = \"1.0\"\n[deps.c]\nrecipe = \"p/c\"\nversion = \"1.0\"\n" + profile,
            "otok.toml", native, {}, d);
        EXPECT_TRUE(m) << messages(d);
        auto r = resolve(*m, native, p, {}, d);
        return std::pair{std::move(r), messages(d)};
    };
    auto const [chosen, text] = resolve_with(">= 17", ">= 14, <= 20", "", "[profile.x]\nnative = { cxx-standard = 20 }\n");
    ASSERT_TRUE(chosen) << text;
    EXPECT_EQ(chosen->standards[0].at("native").cxx, 17);
    EXPECT_EQ(chosen->standards[1].at("native").cxx, 20);  // the profile's
    auto const [given, given_text] = resolve_with(">= 17", "", "cxx-standard = 23\n");
    ASSERT_TRUE(given) << given_text;
    EXPECT_EQ(given->standards[0].at("native").cxx, 23);
    // no recipe says: none, each recipe chooses
    auto const [free, free_text] = resolve_with("", "", "");
    ASSERT_TRUE(free) << free_text;
    EXPECT_FALSE(free->standards[0].contains("native"));
    auto const [none, none_text] = resolve_with(">= 17", "<= 14", "");
    EXPECT_FALSE(none);
    EXPECT_NE(none_text.find("no C++ standard suits every library of [native]: a '>= 17', b '<= 14'"), std::string::npos) << none_text;
}

// The C standard is chosen in the same way, by year (otok-toml.md 5.1).
TEST(resolver, chooses_the_c_standard_of_a_target)
{
    target_info const* const native[] = {&test::native_info()};
    auto const resolve_with = [&](std::string const& a, std::string const& b, std::string const& table) {
        memory_provider p;
        p.add({.library = "a", .version = "1.0", .platforms = "\"native\"", .recipe_keys = "c-standard = \"" + a + "\"\ncxx-standard = \">= 14\"\n"});
        p.add({.library = "b", .version = "1.0", .platforms = "\"native\"", .recipe_keys = "c-standard = \"" + b + "\"\n"});
        diagnostics d;
        auto m = parse_manifest("[otok]\nformat = 1\n[native]\n" + table + "[deps.a]\nrecipe = \"p/a\"\nversion = \"1.0\"\n"
            "[deps.b]\nrecipe = \"p/b\"\nversion = \"1.0\"\n", "otok.toml", native, {}, d);
        EXPECT_TRUE(m) << messages(d);
        auto r = resolve(*m, native, p, {}, d);
        return std::pair{std::move(r), messages(d)};
    };
    auto const [chosen, text] = resolve_with(">= 99", ">= 11", "");
    ASSERT_TRUE(chosen) << text;
    EXPECT_EQ(chosen->standards[0].at("native").c, 11);
    EXPECT_EQ(chosen->standards[0].at("native").cxx, 14);
    auto const [old, old_text] = resolve_with(">= 89", "<= 99", "");
    ASSERT_TRUE(old) << old_text;
    EXPECT_EQ(old->standards[0].at("native").c, 89);
    auto const [refused, refused_text] = resolve_with(">= 11", ">= 89", "c-standard = 99\n");
    EXPECT_FALSE(refused);
    EXPECT_NE(refused_text.find("p/a 1.0 cannot be built with C99 for 'native' (its recipe builds with '>= 11'); the project sets it in [native]"),
        std::string::npos) << refused_text;
    auto const [none, none_text] = resolve_with(">= 11", "<= 99", "");
    EXPECT_FALSE(none);
    EXPECT_NE(none_text.find("no C standard suits every library of [native]: a '>= 11', b '<= 99'"), std::string::npos) << none_text;
}

// A tool and what it links take one standard, the lowest their recipes allow, without one of theirs in [tool-deps]
// (docs/PLAN.md decision 116); the target's does not reach them.
TEST(resolver, chooses_the_cxx_standard_of_tools_linked_together)
{
    target_info const* const native[] = {&test::native_info()};
    auto const resolve_with = [&](std::string const& gen, std::string const& lib, std::string const& tools) {
        memory_provider p;
        p.add({.library = "lib", .version = "1.0", .platforms = "\"native\"", .recipe_keys = lib.empty() ? "" : "cxx-standard = \"" + lib + "\"\n"});
        p.add({.library = "gen", .version = "1.0", .extra = recipe_dep("lib", ">= 1"), .platforms = "\"native\"",
            .recipe_keys = gen.empty() ? "" : "cxx-standard = \"" + gen + "\"\n"});
        p.add({.library = "other", .version = "1.0", .platforms = "\"native\"", .recipe_keys = "cxx-standard = \">= 20\"\n"});
        p.add({.library = "a", .version = "1.0", .extra = recipe_dep("gen", ">= 1", "", "tool-deps") + recipe_dep("other", ">= 1", "", "tool-deps"),
            .platforms = "\"native\""});
        diagnostics d;
        auto m = parse_manifest("[otok]\nformat = 1\n[native]\ncxx-standard = 23\n[deps.a]\nrecipe = \"p/a\"\nversion = \"1.0\"\n" + tools,
            "otok.toml", native, {}, d);
        EXPECT_TRUE(m) << messages(d);
        auto r = resolve(*m, native, p, {}, d);
        return std::pair{std::move(r), messages(d)};
    };
    auto const cxx = [](resolution const& r, std::string const& library) -> std::optional<int> {
        auto const it = r.tool_standards.find(r.find_tool(library)->id);
        return it == r.tool_standards.end() ? std::nullopt : it->second.cxx;
    };
    auto const [chosen, text] = resolve_with(">= 17", ">= 14", "");
    ASSERT_TRUE(chosen) << text;
    EXPECT_EQ(cxx(*chosen, "gen"), 17);
    EXPECT_EQ(cxx(*chosen, "lib"), 17);    // what gen links: gen's, not its own lowest
    EXPECT_EQ(cxx(*chosen, "other"), 20);  // not linked with them
    // no recipe says: none
    auto const [free, free_text] = resolve_with("", "", "");
    ASSERT_TRUE(free) << free_text;
    EXPECT_EQ(cxx(*free, "gen"), std::nullopt);
    // a tool's own stays its own, and the tools linked with it take it
    auto const [given, given_text] = resolve_with(">= 17", ">= 14", "[tool-deps.gen]\nrecipe = \"p/gen\"\nversion = \"1.0\"\ncxx-standard = 20\n");
    ASSERT_TRUE(given) << given_text;
    EXPECT_EQ(cxx(*given, "gen"), std::nullopt);
    EXPECT_EQ(cxx(*given, "lib"), 20);
    auto const [none, none_text] = resolve_with(">= 17", "<= 14", "");
    EXPECT_FALSE(none);
    EXPECT_NE(none_text.find("no C++ standard suits every tool linked together: gen '>= 17', lib '<= 14'; set it in [tool-deps] of each"),
        std::string::npos) << none_text;
}

TEST(resolver, architectures_stay_within_the_recipe)
{
    memory_provider p;
    p.add({.library = "c", .version = "1.0", .platforms = "\"ios\", \"iossim\"", .platform_keys = "architectures = { iossim = [\"arm64\"] }\n"});
    auto o = run(p, dep("c", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("p/c 1.0 builds only 'arm64' for 'iossim', but the project builds 'arm64', 'x86_64'; restrict them with [apple] architectures"),
        std::string::npos) << o.text();

    std::string const header = "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\", \"iossim\"]\narchitectures = { iossim = [\"arm64\"] }\n"
                               "deployment = { ios = \"15.0\" }\n";
    o = run(p, dep("c", "1.0"), {}, header);
    EXPECT_TRUE(o.r) << o.text();
}

TEST(resolver, profiles)
{
    std::string const mode = "[options.mode]\ntype = \"enum\"\ndefault = \"x\"\ndoc = \"d\"\nvalues = [\"x\", \"y\"]\n";
    memory_provider p;
    p.add({.library = "a", .version = "1.0", .extra = mode + recipe_dep("x", ">= 1", "when = { platform = [\"ios\"] }\n")});
    p.add({.library = "x", .version = "1.0"});
    p.add({.library = "b", .version = "1.0", .extra = recipe_dep("a", ">= 1", "requires-options = { mode = \"x\" }\n")});

    std::string const ci = "[profile.ci]\napple = { platforms = [\"iossim\"] }\n";
    auto o = run(p, dep("a", "1.0") + ci);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.r->configurations, (std::vector<std::string>{"", "ci"}));
    // locked for every profile, built only where its condition holds
    EXPECT_EQ(first(*o.r, "x")->configurations[0].platforms, std::vector<std::string>{"ios"});
    EXPECT_TRUE(first(*o.r, "x")->configurations[1].platforms.empty());

    // a conflict in one profile fails the resolution: one lock serves every profile
    o = run(p, dep("a", "1.0") + dep("b", "1.0") + "[profile.ci.deps.a.options]\nmode = \"y\"\n");
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("in profile 'ci': conflicting values for option 'mode' of 'a'"), std::string::npos) << o.text();
    EXPECT_NE(o.text().find("profile 'ci' asks for \"y\""), std::string::npos) << o.text();

    // a library outside a profile's platforms is not built there
    o = run(p, dep("a", "1.0", "platforms = [\"ios\"]\n") + ci);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_NE(o.text().find("warning: 'a' is not built in profile 'ci'"), std::string::npos) << o.text();
}

TEST(resolver, tool_dependencies)
{
    std::string const tools = "[options.tools]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\nadditive = true\n";
    memory_provider p;
    for (char const* v : {"1.0", "2.0"}) p.add({.library = "protobuf", .version = v, .extra = tools});
    p.add({.library = "grpc", .version = "1.0",
        .extra = recipe_dep("protobuf", "~> 2.0") + recipe_dep("protobuf", "~> 1.0", "requires-options = { tools = true }\n", "tool-deps")});
    p.add({.library = "zlib", .version = "1.0", .platforms = "\"ios\", \"iossim\""});

    auto o = run(p, dep("grpc", "1.0") + "[tool-deps.zlib]\nrecipe = \"p/zlib\"\nversion = \"1.0\"\n");
    ASSERT_TRUE(o.r) << o.text();
    // the target and the tool copy are resolved separately
    EXPECT_EQ(o.version("protobuf"), "2.0");
    EXPECT_EQ(o.tool_version("protobuf"), "1.0");
    EXPECT_EQ(first(*o.r, "grpc")->tool_dependencies, std::vector<std::string>{"protobuf@1.0"});
    EXPECT_EQ(o.r->find_tool("protobuf")->configurations[0].platforms, std::vector<std::string>{"macosx"});
    EXPECT_EQ(std::get<bool>(o.r->find_tool("protobuf")->configurations[0].options.at("tools")), true);
    EXPECT_EQ(std::get<bool>(first(*o.r, "protobuf")->configurations[0].options.at("tools")), false);
    EXPECT_NE(o.text().find("warning: [tool-deps.zlib] is not used"), std::string::npos) << o.text();

    // [tool-deps] chooses for tool uses only
    o = run(p, dep("grpc", "1.0") + "[tool-deps.protobuf]\nrecipe = \"p/protobuf\"\nversion = \"< 1.5\"\n");
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.tool_version("protobuf"), "1.0");
    o = run(p, dep("grpc", "1.0") + "[tool-deps.protobuf]\nrecipe = \"p/protobuf\"\nversion = \">= 2\"\n");
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("cannot resolve the tool dependency of p/grpc 1.0 on 'protobuf': no version of 'protobuf'"), std::string::npos) << o.text();
}

// Graphs (otok-lock.md 2, 5.3, 5.4; docs/PLAN.md decision 82): one per target table and environment, one version of a
// library in all of them, the variant of each by its cells; tool graphs by the systems the environments run on.
namespace {

target_info const* const delivery_targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info(), &target::linux::info()};

outcome run_on(recipe_provider& provider, std::string const& body, resolve_settings settings = {}, std::string const& otok_keys = "")
{
    outcome o;
    o.m = parse_manifest("[otok]\nformat = 1\n" + otok_keys + body, "otok.toml", delivery_targets, [](std::string const&) { return std::nullopt; },
        o.diags);
    if (!o.m) {
        ADD_FAILURE() << o.text();
        return o;
    }
    o.r = resolve(*o.m, delivery_targets, provider, settings, o.diags);
    return o;
}

std::string const windows_table = "[windows]\nplatforms = [\"windows\"]\nbuild-on = [\"windows/msvc\", \"linux/clang-cl\"]\n";

// "<variant> r<revision>" of the library in each graph that has it
std::vector<std::string> variants_of(resolution const& r, std::string_view library, bool tool = false)
{
    std::vector<std::string> out;
    for (resolved_package const& p : tool ? r.tool_packages : r.packages) {
        if (p.library == library) out.push_back(p.chosen.recipe->variant + " r" + std::to_string(p.chosen.recipe->revision));
    }
    return out;
}

}

TEST(resolver, a_variant_for_each_graph)
{
    memory_provider p;
    // b2 on Windows, CMake in the cross build: two variants of one recipe, with revisions of their own
    p.add({.library = "boost", .version = "1.92", .extra = recipe_dep("zlib", "1.0"), .revision = 1, .platforms = "\"windows\"",
        .recipe_keys = "variant = \"windows-msvc\"\n", .build_on = "\"windows/msvc\""});
    p.add({.library = "boost", .version = "1.92", .extra = recipe_dep("zlib", "1.0"), .revision = 3, .platforms = "\"windows\"",
        .recipe_keys = "variant = \"windows-clang-cl\"\n", .build_on = "\"linux/clang-cl\""});
    p.add({.library = "zlib", .version = "1.0", .platforms = "\"windows\"", .build_on = "\"windows/msvc\", \"linux/clang-cl\""});

    auto o = run_on(p, windows_table + dep("boost", "1.92"));
    ASSERT_TRUE(o.r) << o.text();
    ASSERT_EQ(o.r->graphs.size(), 2u);
    EXPECT_EQ(o.r->graphs[1].environment.text(), "linux/clang-cl");
    EXPECT_FALSE(o.r->several_targets);
    EXPECT_EQ(variants_of(*o.r, "boost"), (std::vector<std::string>{"windows-msvc r1", "windows-clang-cl r3"}));
    EXPECT_EQ(o.r->find("boost", "windows")->id, "boost");

    // entries equal in all but the graph are one
    test::temp_dir project;
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;
    ASSERT_EQ(lock->packages.size(), 3u);
    EXPECT_EQ(lock->find("zlib", "windows", "linux/clang-cl"), lock->find("zlib", "windows", "windows/msvc"));
    EXPECT_EQ(lock->find("zlib", "windows", "windows/msvc")->build_on, (std::vector<std::string>{"linux/clang-cl", "windows/msvc"}));
    EXPECT_EQ(lock->find("boost", "windows", "linux/clang-cl")->variant, "windows-clang-cl");
    EXPECT_EQ(lock->find("boost", "windows", "windows/msvc")->revision, 1);
    std::string const text = write_lock(*lock);
    diagnostics d;
    auto again = parse_lock(text, "otok.lock", d);
    ASSERT_TRUE(again.has_value()) << messages(d) << text;
    EXPECT_EQ(write_lock(*again), text);
    EXPECT_EQ(lock_changes({}, *lock), (std::vector<std::string>{"added boost (linux/clang-cl) 1.92 (r3)", "added boost (windows/msvc) 1.92 (r1)",
        "added zlib (linux/clang-cl) 1.0 (r1)", "added zlib (windows/msvc) 1.0 (r1)"}));

    // the lock as the preference: kept, a new revision of one variant too
    p.add({.library = "boost", .version = "1.92", .extra = recipe_dep("zlib", "1.0"), .revision = 4, .platforms = "\"windows\"",
        .recipe_keys = "variant = \"windows-clang-cl\"\n", .build_on = "\"linux/clang-cl\""});
    resolve_settings settings;
    settings.preferred = &*again;
    auto kept = run_on(p, windows_table + dep("boost", "1.92"), settings);
    ASSERT_TRUE(kept.r) << kept.text();
    EXPECT_TRUE(lock_changes(*again, *make_lock(*kept.r, *kept.m, project.path())).empty());
    // `otok update` moves to it
    auto updated = run_on(p, windows_table + dep("boost", "1.92"));
    ASSERT_TRUE(updated.r) << updated.text();
    EXPECT_EQ(lock_changes(*again, *make_lock(*updated.r, *updated.m, project.path())),
        std::vector<std::string>{"boost (linux/clang-cl): 1.92 (r3) -> 1.92 (r4)"});
}

TEST(resolver, one_version_in_every_graph)
{
    memory_provider p;
    for (char const* v : {"1.0", "1.1"}) {
        p.add({.library = "boost", .version = v, .platforms = "\"windows\"", .recipe_keys = "variant = \"windows-msvc\"\n", .build_on = "\"windows/msvc\""});
    }
    p.add({.library = "boost", .version = "1.0", .platforms = "\"windows\"", .recipe_keys = "variant = \"windows-clang-cl\"\n",
        .build_on = "\"linux/clang-cl\""});

    // 1.1 has no variant for the cross build
    auto o = run_on(p, windows_table + dep("boost", ">= 1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(variants_of(*o.r, "boost"), (std::vector<std::string>{"windows-msvc r1", "windows-clang-cl r1"}));
    EXPECT_EQ(o.r->find("boost", "windows")->chosen.recipe->version->str(), "1.0");
    for (resolved_package const& pkg : o.r->packages) EXPECT_EQ(pkg.chosen.recipe->version->str(), "1.0");

    o = run_on(p, windows_table + dep("boost", ">= 1.1"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("1.1 (r1): for 'windows' in linux/clang-cl: no variant of p/boost"), std::string::npos) << o.text();
    // one graph: the message names it
    o = run_on(p, "[windows]\nplatforms = [\"windows\"]\nbuild-on = [\"linux/clang-cl\"]\n" + dep("boost", ">= 1.1"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("1.1 (r1): no variant of p/boost for 'windows' in linux/clang-cl"), std::string::npos) << o.text();
}

// docs/PLAN.md decision 91: a library whose recipe does not build in the graph's environment is built in another of the same
// system and ABI, the first some variant builds in
TEST(resolver, a_library_in_another_environment_of_the_same_abi)
{
    memory_provider p;
    p.add({.library = "skia", .version = "152", .extra = recipe_dep("zlib", "1.0"), .platforms = "\"windows\"",
        .build_on = "\"windows/clang-cl\""});
    p.add({.library = "zlib", .version = "1.0", .platforms = "\"windows\"", .build_on = "\"windows/msvc\", \"linux/clang-cl\""});
    std::string const msvc = "[windows]\nplatforms = [\"windows\"]\nbuild-on = [\"windows/msvc\"]\n";

    auto o = run_on(p, msvc + dep("skia", "152"));
    ASSERT_TRUE(o.r) << o.text();
    ASSERT_EQ(o.r->graphs.size(), 1u);
    EXPECT_EQ(o.r->find("skia", "windows")->environment.text(), "windows/clang-cl");
    EXPECT_EQ(o.r->find("zlib", "windows")->environment.text(), "windows/msvc");
    // the lock names the graph, not the environment, which the variant gives
    test::temp_dir project;
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;
    EXPECT_EQ(lock->find("skia", "windows", "windows/msvc")->build_on, std::vector<std::string>{"windows/msvc"});

    // the graph's own environment first
    p.add({.library = "skia", .version = "152", .extra = recipe_dep("zlib", "1.0"), .revision = 2, .platforms = "\"windows\"",
        .build_on = "\"windows/clang-cl\", \"windows/msvc\""});
    o = run_on(p, msvc + dep("skia", "152"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.r->find("skia", "windows")->environment.text(), "windows/msvc");

    // not another system: linux/clang-cl has the same ABI, but the graph is built on Windows
    memory_provider q;
    q.add({.library = "zlib", .version = "1.0", .platforms = "\"windows\"", .build_on = "\"linux/clang-cl\""});
    o = run_on(q, msvc + dep("zlib", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("no variant of p/zlib for 'windows' in windows/msvc"), std::string::npos) << o.text();

    // Linux: gcc and clang share the system's libstdc++
    memory_provider l;
    l.add({.library = "zlib", .version = "1.0", .platforms = "\"linux\"", .build_on = "\"linux/clang\""});
    o = run_on(l, "[linux]\nplatforms = [\"linux\"]\nbuild-on = [\"linux/gcc\"]\n" + dep("zlib", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.r->find("zlib", "linux")->environment.text(), "linux/clang");
}

TEST(target_info, substitutes_of_an_environment)
{
    target_info const& w = target::windows::info();
    auto const texts = [](std::vector<build_environment> const& es) {
        std::vector<std::string> out;
        for (build_environment const& e : es) out.push_back(e.text());
        return out;
    };
    EXPECT_EQ(texts(w.substitutes({"windows", "", "msvc"})), std::vector<std::string>{"windows/clang-cl"});
    EXPECT_EQ(texts(w.substitutes({"windows", "arm64", "clang-cl"})), std::vector<std::string>{"windows-arm64/msvc"});
    EXPECT_TRUE(w.substitutes({"linux", "", "clang-cl"}).empty());
    // a compiler of an ABI of its own, and `*` environments, have none
    target_info other = w;
    other.abis.erase("clang-cl");
    EXPECT_TRUE(other.substitutes({"windows", "", "msvc"}).empty());
    EXPECT_TRUE(target::android::info().substitutes({"", "", "ndk"}).empty());
}

TEST(resolver, several_targets)
{
    memory_provider p;
    // a recipe of each target, the Apple one with a dependency the other has not; 1.1 only for Linux
    p.add({.library = "a", .version = "1.0", .extra = recipe_dep("b", "1.0"), .platforms = "\"ios\", \"iossim\""});
    p.add({.library = "a", .version = "1.0", .platforms = "\"linux\""});
    p.add({.library = "a", .version = "1.1", .platforms = "\"linux\""});
    p.add({.library = "b", .version = "1.0", .platforms = "\"ios\", \"iossim\""});
    auto o = run_on(p, "[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n[linux]\nplatforms = [\"linux\"]\nbuild-on = [\"linux/gcc\"]\n"
        "output = \"linux\"\n" + dep("a", ">= 1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_TRUE(o.r->several_targets);
    std::vector<std::string> ids;
    for (resolved_package const& pkg : o.r->packages) ids.push_back(pkg.id + " " + pkg.chosen.recipe->version->str());
    EXPECT_EQ(ids, (std::vector<std::string>{"a@apple 1.0", "a@linux 1.0", "b@apple 1.0"}));
    EXPECT_EQ(o.r->find("a", "apple")->dependencies, std::vector<std::string>{"b"});
    EXPECT_TRUE(o.r->find("a", "linux")->dependencies.empty());
    EXPECT_EQ(o.r->find("a", "linux")->configurations[0].platforms, std::vector<std::string>{"linux"});
    EXPECT_EQ(o.r->find("b", "linux"), nullptr);
    EXPECT_EQ(o.r->find_id("b@apple"), o.r->find("b", "apple"));

    test::temp_dir project;
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;
    EXPECT_EQ(lock_changes({}, *lock), (std::vector<std::string>{"added a@apple 1.0 (r1)", "added a@linux 1.0 (r1)", "added b@apple 1.0 (r1)"}));
    EXPECT_EQ(lock->find("a", "linux", "linux/gcc")->variant, "linux");

    // the graphs of one machine
    resolution const linux_only = select_graphs(*o.r, std::vector<std::size_t>{1}, std::vector<std::size_t>{});
    ASSERT_EQ(linux_only.packages.size(), 1u);
    EXPECT_EQ(linux_only.packages[0].id, "a@linux");
}

TEST(resolver, a_tool_graph_for_each_system)
{
    memory_provider p;
    // a tool recipe of each host target; 1.14 only for Linux
    p.add({.library = "ninja", .version = "1.13", .platforms = "\"macosx\""});
    p.add({.library = "ninja", .version = "1.13", .platforms = "\"linux\""});
    p.add({.library = "ninja", .version = "1.13", .platforms = "\"linux\"", .recipe_keys = "variant = \"linux-clang\"\n", .build_on = "\"linux/clang\""});
    p.add({.library = "ninja", .version = "1.13", .platforms = "\"windows\""});
    p.add({.library = "ninja", .version = "1.14", .platforms = "\"linux\""});
    std::string const uses_ninja = recipe_dep("ninja", ">= 1.0", "", "tool-deps");
    p.add({.library = "boost", .version = "1.92", .extra = uses_ninja, .platforms = "\"windows\"", .build_on = "\"windows/msvc\", \"linux/clang-cl\""});
    p.add({.library = "lz4", .version = "1.10", .extra = uses_ninja, .platforms = "\"android\""});

    // the systems of windows/msvc and linux/clang-cl, the tool environments of [otok] tool-build-on
    auto o = run_on(p, windows_table + dep("boost", "1.92"));
    ASSERT_TRUE(o.r) << o.text();
    std::vector<std::string> graphs;
    for (resolution_graph const& g : o.r->tool_graphs) graphs.push_back(g.target + " " + g.environment.text() + " " + g.tool_platform);
    // tool-build-on's environment first, then the system's others (docs/PLAN.md decision 86)
    EXPECT_EQ(graphs, (std::vector<std::string>{"windows windows/msvc windows", "windows windows/clang-cl windows", "linux linux/gcc linux",
        "linux linux/clang linux"}));
    EXPECT_EQ(variants_of(*o.r, "ninja", true), (std::vector<std::string>{"windows r1", "linux r1"}));  // one version: 1.13
    EXPECT_EQ(o.r->find("boost", "windows")->tool_dependencies, std::vector<std::string>{"ninja@1.13"});
    test::temp_dir project;
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;
    ASSERT_EQ(lock->tool_packages.size(), 2u);
    EXPECT_EQ(lock->find_tool("ninja", "linux/gcc")->variant, "linux");
    EXPECT_EQ(lock->find_tool("ninja", "linux/gcc")->target, "");

    o = run_on(p, windows_table + dep("boost", "1.92"), {}, "tool-build-on = [\"linux/clang\"]\n");
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(variants_of(*o.r, "ninja", true), (std::vector<std::string>{"windows r1", "linux-clang r1"}));

    // */ndk runs on every system
    o = run_on(p, "[android]\narchitectures = { android = [\"x86_64\"] }\ndeployment = { android = \"24\" }\n" + dep("lz4", "1.10"));
    ASSERT_TRUE(o.r) << o.text();
    ASSERT_EQ(o.r->tool_graphs.size(), 5u);
    EXPECT_EQ(variants_of(*o.r, "ninja", true), (std::vector<std::string>{"windows r1", "apple r1", "linux r1"}));
    EXPECT_EQ(o.r->graphs[0].environment.text(), "*/ndk");
}

// otok-toml.md 7, docs/PLAN.md decision 86: a tool in the environment of tool-build-on runs, and links, libraries whose recipes
// build in another environment of the system
TEST(resolver, tools_in_the_environments_their_recipes_build_in)
{
    memory_provider p;
    p.add({.library = "ninja", .version = "1.13", .platforms = "\"windows\"", .build_on = "\"windows/msvc\""});
    p.add({.library = "zlib", .version = "1.3", .platforms = "\"windows\"", .build_on = "\"windows/msvc\""});
    p.add({.library = "gn", .version = "2175", .extra = recipe_dep("ninja", ">= 1.0", "", "tool-deps") + recipe_dep("zlib", ">= 1.0"),
        .platforms = "\"windows\"", .build_on = "\"windows/msvc\", \"windows/clang-cl\""});
    p.add({.library = "skia", .version = "152", .extra = recipe_dep("gn", ">= 1", "", "tool-deps"), .platforms = "\"windows\"",
        .build_on = "\"windows/clang-cl\""});
    std::string const table = "[windows]\nbuild-on = [\"windows/clang-cl\"]\n";
    auto o = run_on(p, table + dep("skia", "152"), {}, "tool-build-on = [\"windows/clang-cl\"]\n");
    ASSERT_TRUE(o.r) << o.text();
    auto const environment_of = [&](std::string_view library) {
        resolved_package const* t = o.r->find_tool(library);
        return t ? o.r->tool_graphs[t->graph].environment.text() : std::string{"none"};
    };
    EXPECT_EQ(environment_of("gn"), "windows/clang-cl");
    EXPECT_EQ(environment_of("ninja"), "windows/msvc");
    EXPECT_EQ(environment_of("zlib"), "windows/msvc");
    EXPECT_EQ(o.r->find_tool("gn")->tool_dependencies, std::vector<std::string>{"ninja@1.13"});
    EXPECT_EQ(o.r->find_tool("gn")->dependencies, std::vector<std::string>{"zlib@1.3"});
    test::temp_dir project;
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;
    EXPECT_NE(lock->find_tool("gn", "windows/clang-cl"), nullptr);
    EXPECT_NE(lock->find_tool("ninja", "windows/msvc"), nullptr);
    EXPECT_EQ(lock->find_tool("ninja", "windows/clang-cl"), nullptr);

    // a recipe that builds in tool-build-on's environment stays there
    o = run_on(p, table + dep("skia", "152"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(environment_of("gn"), "windows/msvc");
}

// docs/PLAN.md decision 87, otok-lock.md 5.7: each tool dependency resolved for its package alone
TEST(resolver, tools_of_each_package_in_their_own_versions)
{
    memory_provider p;
    std::string const tool = "[tool]\nprograms = [\"ninja\"]\n";
    for (char const* v : {"1.0", "2.0"}) p.add({.library = "ninja", .version = v, .extra = tool});
    p.add({.library = "a", .version = "1.0", .extra = "[toolchains]\nninja = { version = \">= 2\", default-recipe = \"p/ninja\" }\n"});
    p.add({.library = "b", .version = "1.0", .extra = "[toolchains]\nninja = { version = \"< 2\", default-recipe = \"p/ninja\" }\n"});
    p.add({.library = "c", .version = "1.0", .extra = "[toolchains]\nninja = { default-recipe = \"p/ninja\" }\n"});

    auto o = run(p, dep("a", "1.0") + dep("b", "1.0") + dep("c", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(first(*o.r, "a")->tool_dependencies, std::vector<std::string>{"ninja@2.0"});
    EXPECT_EQ(first(*o.r, "b")->tool_dependencies, std::vector<std::string>{"ninja@1.0"});
    EXPECT_EQ(first(*o.r, "c")->tool_dependencies, std::vector<std::string>{"ninja@2.0"});  // the highest, whatever the others need
    EXPECT_EQ(first(*o.r, "a")->tool_edges, std::vector<std::string>{"ninja@2.0"});
    ASSERT_EQ(o.r->tool_packages.size(), 2u);  // a and c share one
    EXPECT_EQ(o.r->find_tool("ninja@1.0")->chosen.recipe->version->str(), "1.0");

    test::temp_dir project;
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;
    ASSERT_EQ(lock->tool_packages.size(), 2u);
    EXPECT_EQ(first(*lock, "b")->tool_dependencies, std::vector<std::string>{"ninja@1.0"});
    auto again = parse_lock(write_lock(*lock), "otok.lock", o.diags);
    ASSERT_TRUE(again.has_value()) << o.text();

    // the lock's versions are kept for each package: c stays on 1.0 when the lock says so
    lock_file kept = *lock;
    for (locked_package& l : kept.packages) {
        if (l.library == "c") l.tool_dependencies = {"ninja@1.0"};
    }
    resolve_settings settings;
    settings.preferred = &kept;
    o = run(p, dep("a", "1.0") + dep("b", "1.0") + dep("c", "1.0"), settings);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(first(*o.r, "c")->tool_dependencies, std::vector<std::string>{"ninja@1.0"});
    EXPECT_EQ(first(*o.r, "a")->tool_dependencies, std::vector<std::string>{"ninja@2.0"});
}

// docs/PLAN.md decision 120: a new tool dependency takes the lock's revision of a tool package of the same version, which
// the packages locked before keep (one tool package of a library and version in a graph)
TEST(resolver, new_tools_take_the_locked_revision)
{
    memory_provider p;
    std::string const tool = "[tool]\nprograms = [\"ninja\"]\n";
    p.add({.library = "ninja", .version = "1.0", .extra = tool, .revision = 1});
    std::string const uses = "[toolchains]\nninja = { default-recipe = \"p/ninja\" }\n";
    p.add({.library = "a", .version = "1.0", .extra = uses});
    p.add({.library = "b", .version = "1.0", .extra = uses});
    auto o = run(p, dep("a", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    test::temp_dir project;
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;

    p.add({.library = "ninja", .version = "1.0", .extra = tool, .revision = 2});
    resolve_settings settings;
    settings.preferred = &*lock;
    o = run(p, dep("a", "1.0") + dep("b", "1.0"), settings);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(first(*o.r, "b")->tool_dependencies, std::vector<std::string>{"ninja@1.0"});
    ASSERT_EQ(o.r->tool_packages.size(), 1u);
    EXPECT_EQ(o.r->find_tool("ninja@1.0")->chosen.recipe->revision, 1);

    // unlocked, the newest for both
    settings.unlocked = {"ninja"};
    o = run(p, dep("a", "1.0") + dep("b", "1.0"), settings);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.r->find_tool("ninja@1.0")->chosen.recipe->revision, 2);
}

// recipe-toml.md 13.1: a toolchain names a program; the library is that of the recipe
TEST(resolver, toolchains_are_programs_of_the_recipes_they_name)
{
    memory_provider p;
    p.add({.library = "flatbuffers", .version = "25.1", .extra = "[tool]\nprograms = [\"flatc\"]\n"});
    p.add({.library = "a", .version = "1.0", .extra = "[toolchains]\nflatc = { version = \">= 25\", default-recipe = \"p/flatbuffers\" }\n"});
    p.add({.library = "b", .version = "1.0", .extra = "[toolchains]\nflathash = { default-recipe = \"p/flatbuffers\" }\n"});
    auto o = run(p, dep("a", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(first(*o.r, "a")->tool_dependencies, std::vector<std::string>{"flatbuffers@25.1"});
    EXPECT_EQ(o.r->find_tool("flatbuffers")->required_by.front().requester, "p/a 1.0");

    o = run(p, dep("b", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("p/b 1.0 takes the program 'flathash' from p/flatbuffers 25.1, which builds 'flatc' ([tool] programs"),
        std::string::npos) << o.text();
}

// recipe-toml.md 13.2: programs of a dependency, of its recipe and version
TEST(resolver, programs_of_a_dependency)
{
    memory_provider p;
    for (char const* v : {"23.5", "25.1"}) p.add({.library = "flatbuffers", .version = v, .extra = "[tool]\nprograms = [\"flatc\"]\n"});
    p.add({.library = "a", .version = "1.0",
        .extra = recipe_dep("flatbuffers", ">= 23", "toolchains = [\"flatc\"]\nwhen = { platform = [\"ios\"] }\n")});
    p.add({.library = "b", .version = "1.0", .extra = recipe_dep("flatbuffers", "< 24")});

    auto o = run(p, dep("a", "1.0") + dep("b", "1.0"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("flatbuffers"), "23.5");
    // the tool has the version the dependency got, not the highest
    EXPECT_EQ(first(*o.r, "a")->tool_dependencies, std::vector<std::string>{"flatbuffers@23.5"});
    ASSERT_EQ(o.r->tool_packages.size(), 1u);

    // only where the dependency is
    o = run(p, dep("a", "1.0") + dep("b", "1.0", "platforms = [\"iossim\"]\n"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(o.version("flatbuffers"), "23.5");
    o = run(p, "[otok]\nformat = 1\n[apple]\nplatforms = [\"iossim\"]\ndeployment = { ios = \"15.0\" }\n" + dep("a", "1.0"), {}, "");
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_TRUE(first(*o.r, "a")->tool_dependencies.empty());
    EXPECT_TRUE(o.r->tool_packages.empty());

    // the project cannot give the tool another recipe than the dependency's
    p.add({.library = "flatbuffers", .version = "25.1", .extra = "[tool]\nprograms = [\"flatc\"]\n"}, "q");
    o = run(p, dep("a", "1.0") + "[tool-deps.flatbuffers]\nrecipe = \"q/flatbuffers\"\nversion = \">= 1\"\n");
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("p/a 1.0 takes programs of 'flatbuffers' from its dependency, whose recipe is p/flatbuffers, but otok.toml "
        "chooses q/flatbuffers in [tool-deps.flatbuffers]"), std::string::npos) << o.text();
}

// otok-lock.md 5.7: a tool needing itself is a cycle
TEST(resolver, rejects_tool_cycles)
{
    memory_provider p;
    p.add({.library = "gen", .version = "1.0", .extra = "[tool]\nprograms = [\"gen\"]\n[toolchains]\nmaker = { default-recipe = \"p/maker\" }\n"});
    p.add({.library = "maker", .version = "1.0", .extra = "[tool]\nprograms = [\"maker\"]\n[toolchains]\ngen = { default-recipe = \"p/gen\" }\n"});
    p.add({.library = "a", .version = "1.0", .extra = "[toolchains]\ngen = { default-recipe = \"p/gen\" }\n"});
    auto o = run(p, dep("a", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("tool dependency cycle: gen -> maker -> gen"), std::string::npos) << o.text();
}

// recipe-toml.md 13.1: one tool dependency on a library
TEST(resolver, one_tool_dependency_on_a_library)
{
    memory_provider p;
    p.add({.library = "ninja", .version = "1.0", .extra = "[tool]\nprograms = [\"ninja\"]\n"});
    p.add({.library = "a", .version = "1.0", .extra = recipe_dep("ninja", ">= 1", "", "tool-deps")
        + "[toolchains]\nninja = { default-recipe = \"p/ninja\" }\n"});
    auto o = run(p, dep("a", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("p/a 1.0 has two tool dependencies on 'ninja'"), std::string::npos) << o.text();
}

TEST(resolver, rejects_cycles)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0", .extra = recipe_dep("b", ">= 1")});
    p.add({.library = "b", .version = "1.0", .extra = recipe_dep("a", ">= 1")});
    auto o = run(p, dep("a", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("error: dependency cycle: a -> b -> a"), std::string::npos) << o.text();
}

TEST(resolver, license_policy)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0", .license = "GPL-3.0-only"});
    p.add({.library = "b", .version = "1.0", .license = "MIT OR GPL-3.0-only"});
    p.add({.library = "c", .version = "1.0", .license = "GPL-2.0-only WITH Classpath-exception-2.0"});
    auto o = run(p, dep("a", "1.0") + dep("b", "1.0") + dep("c", "1.0"));
    ASSERT_TRUE(o.r) << o.text();

    diagnostics d;
    EXPECT_FALSE(check_license_policy(*o.r, {.deny = {"GPL-3.0-only", "GPL-2.0-only"}, .allow = {}}, d));
    std::string const text = messages(d);
    EXPECT_NE(text.find("error: 'a' 1.0 is licensed under 'GPL-3.0-only', and [policy] deny-licenses forbids it"), std::string::npos) << text;
    EXPECT_EQ(text.find("'b'"), std::string::npos) << text;  // MIT can be chosen
    EXPECT_NE(text.find("'c' 1.0"), std::string::npos) << text;  // an entry without an exception covers it too

    diagnostics allow;
    EXPECT_FALSE(check_license_policy(*o.r, {.deny = {}, .allow = {"MIT"}}, allow));
    EXPECT_EQ(allow.error_count(), 2u) << messages(allow);
    EXPECT_NE(messages(allow).find("it is not covered by [policy] allow-licenses"), std::string::npos);
}

TEST(resolver, failures_of_the_provider_stop_the_resolution)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0", .extra = recipe_dep("missing", ">= 1")});
    auto o = run(p, dep("a", "1.0"));
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("error: no recipe p/missing for 'missing'"), std::string::npos) << o.text();

    resolve_settings settings;
    settings.step_limit = 1;
    memory_provider q;
    q.add({.library = "a", .version = "1.0"});
    q.add({.library = "b", .version = "1.0"});
    o = run(q, dep("a", "1.0") + dep("b", "1.0"), settings);
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("dependency resolution gave up after 1 choices"), std::string::npos) << o.text();
}

TEST(local_recipes, path_recipes)
{
    test::temp_dir project;
    test::temp_dir elsewhere;
    project.write("recipes/a/recipe.toml", recipe_text({.library = "a", .version = "1.0", .extra = recipe_dep("b", ">= 1")}));
    elsewhere.write("b/recipe.toml", recipe_text({.library = "b", .version = "2.0"}));
    project.write("recipes/wrong/recipe.toml", recipe_text({.library = "b", .version = "1.0"}));

    project_recipe_provider provider{project.path(), apple_targets};
    std::string const b_path = path_to_utf8((elsewhere.path() / "b").generic_u8string());
    auto o = run(provider, "[deps.a]\nrecipe = { path = \"recipes/a\" }\n[deps.b]\nrecipe = { path = \"" + b_path + "\" }\n");
    ASSERT_TRUE(o.r) << o.text();
    candidate const& a = first(*o.r, "a")->chosen;
    EXPECT_EQ(a.kind, locked_package::recipe_kind::path);
    EXPECT_EQ(a.recipe_source.type, locked_location::kind::path);
    EXPECT_EQ(a.recipe_source.path, "recipes/a");
    EXPECT_EQ(a.recipe_sha256, tree_hash(project.path() / "recipes" / "a").value_or("?"));
    candidate const& b = first(*o.r, "b")->chosen;
    EXPECT_EQ(b.recipe_source.type, locked_location::kind::local);  // outside the project
    EXPECT_TRUE(b.recipe_sha256.empty());
    EXPECT_EQ(o.version("b"), "2.0");

    o = run(provider, "[deps.a]\nrecipe = { path = \"recipes/wrong\" }\n");
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("error: the recipe chosen for 'a' builds 'b'"), std::string::npos) << o.text();

    o = run(provider, "[deps.a]\nrecipe = { path = \"recipes/a\" }\n");  // b's default recipe is in the index
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("error: the recipe index is not available in this version of otok, so 'p/b' cannot be found for 'b'"),
        std::string::npos) << o.text();

    o = run(provider, "[deps.a]\nrecipe = { path = \"recipes/a\" }\nversion = \">= 2\"\n");
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("'a' 1.0 cannot be used"), std::string::npos) << o.text();
}

TEST(locking, lock_of_a_resolution)
{
    memory_provider p;
    // resources out of the order otok.lock keeps them in
    std::string const resources = "[[resource]]\nname = \"zeta\"\ngit = \"https://example.com/z\"\ncommit = \"" + std::string(40, 'e') + "\"\n"
        "[[resource]]\nname = \"alpha\"\ngit = \"https://example.com/a\"\ncommit = \"" + std::string(40, 'f') + "\"\n";
    p.add({.library = "a", .version = "1.0", .extra = recipe_dep("b", ">= 1") + recipe_dep("tool", ">= 1", "", "tool-deps") + resources,
        .license = "Apache-2.0"});
    p.add({.library = "b", .version = "2.0", .revision = 3});
    p.add({.library = "tool", .version = "1.0"});
    auto o = run(p, dep("a", "1.0"));
    ASSERT_TRUE(o.r) << o.text();

    test::temp_dir project;
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;
    ASSERT_EQ(lock->packages.size(), 2u);
    locked_package const& a = *first(*lock, "a");
    EXPECT_EQ(a.id.str(), "p/a");
    EXPECT_EQ(a.version->str(), "1.0");
    EXPECT_EQ(a.recipe_source.git, "https://github.com/p/a");
    EXPECT_EQ(a.source.type, locked_location::kind::git);
    EXPECT_EQ(a.source.commit, std::string(40, 'a'));
    EXPECT_EQ(a.license, "Apache-2.0");
    EXPECT_EQ(a.dependencies, std::vector<std::string>{"b"});
    EXPECT_EQ(a.tool_dependencies, std::vector<std::string>{"tool@1.0"});
    EXPECT_EQ(first(*lock, "b")->revision, 3);
    ASSERT_EQ(lock->tool_packages.size(), 1u);
    EXPECT_EQ(lock->tool_packages[0].library, "tool");

    // the written lock reads back to the same text
    std::string const text = write_lock(*lock);
    diagnostics d;
    auto parsed = parse_lock(text, "otok.lock", d);
    ASSERT_TRUE(parsed.has_value()) << messages(d) << text;
    EXPECT_EQ(write_lock(*parsed), text);
    EXPECT_TRUE(lock_changes(*parsed, *lock).empty());
    // what was read is what a new resolution locks
    ASSERT_EQ(a.resources.size(), 2u);
    EXPECT_EQ(a.resources[0].name, "alpha");
    EXPECT_TRUE(lock_conflicts(*parsed, *lock).empty());
}

TEST(locking, local_sources)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0"});
    p.add({.library = "b", .version = "1.0"});
    test::temp_dir project;
    test::temp_dir elsewhere;
    project.write("vendor/a/a.c", "int a;\n");
    std::string const outside = path_to_utf8(elsewhere.path().generic_u8string());
    auto o = run(p, dep("a", "1.0", "source = { path = \"vendor/a\" }\n") + dep("b", "1.0", "source = { path = \"" + outside + "\" }\n"));
    ASSERT_TRUE(o.r) << o.text();
    auto lock = make_lock(*o.r, *o.m, project.path());
    ASSERT_TRUE(lock.has_value()) << lock.error().message;
    locked_package const& a = *first(*lock, "a");
    EXPECT_EQ(a.source.type, locked_location::kind::path);
    EXPECT_EQ(a.source.path, "vendor/a");
    EXPECT_EQ(a.source.sha256, tree_hash(project.path() / "vendor" / "a").value_or("?"));
    EXPECT_EQ(first(*lock, "b")->source.type, locked_location::kind::local);
    EXPECT_EQ(local_packages(*lock), std::vector<std::string>{"b"});

    o = run(p, dep("a", "1.0", "source = { path = \"vendor/missing\" }\n"));
    ASSERT_TRUE(o.r) << o.text();
    auto missing = make_lock(*o.r, *o.m, project.path());
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message.find("cannot hash the source of 'a' in 'vendor/missing': "), 0u) << missing.error().message;
}

TEST(locking, keeps_the_locked_hash_of_local_sources)
{
    memory_provider p;
    p.add({.library = "a", .version = "1.0"});
    test::temp_dir project;
    project.write("vendor/a/a.c", "int a;\n");
    project.write("vendor/a2/a.c", "int a2;\n");
    auto o = run(p, dep("a", "1.0", "source = { path = \"vendor/a\" }\n"));
    ASSERT_TRUE(o.r) << o.text();
    lock_file const current = *make_lock(*o.r, *o.m, project.path());
    std::string const locked = first(current, "a")->source.sha256;
    EXPECT_TRUE(verify_local_source(*first(current, "a"), project.path(), "a 1.0").has_value());

    // a valid lock keeps its hash without reading the directory, which is checked before the build
    project.write("vendor/a/a.c", "int a = 1;\n");
    std::string const changed = tree_hash(project.path() / "vendor" / "a", tree_links::record).value_or("?");
    auto kept = make_lock(*o.r, *o.m, project.path(), &current);
    ASSERT_TRUE(kept.has_value()) << kept.error().message;
    EXPECT_EQ(first(*kept, "a")->source.sha256, locked);
    auto verified = verify_local_source(*first(*kept, "a"), project.path(), "a 1.0");
    ASSERT_FALSE(verified.has_value());
    EXPECT_EQ(verified.error().message, "a 1.0: the source in 'vendor/a' has changed since otok.lock was written (tree hash " + changed
        + ", locked " + locked + "); run 'otok update a' to accept it");

    // `otok update a` hashes again
    std::vector<std::string> const rehash{"a"};
    EXPECT_EQ(first(*make_lock(*o.r, *o.m, project.path(), &current, rehash), "a")->source.sha256, changed);

    // another path is hashed
    o = run(p, dep("a", "1.0", "source = { path = \"vendor/a2\" }\n"));
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(first(*make_lock(*o.r, *o.m, project.path(), &current), "a")->source.sha256,
        tree_hash(project.path() / "vendor" / "a2", tree_links::record).value_or("?"));
}

TEST(locking, staleness)
{
    memory_provider p;
    for (char const* v : {"1.0", "1.1"}) p.add({.library = "a", .version = v});
    p.add({.library = "b", .version = "1.0"});
    test::temp_dir project;

    auto o = run(p, dep("a", "~> 1.0"));
    ASSERT_TRUE(o.r) << o.text();
    lock_file current = *make_lock(*o.r, *o.m, project.path());
    current.packages[0].version = *version::parse("1.0");  // locked before 1.1 existed

    resolve_settings settings;
    settings.preferred = &current;
    o = run(p, dep("a", "~> 1.0"), settings);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_TRUE(lock_changes(current, *make_lock(*o.r, *o.m, project.path())).empty());  // the locked version still qualifies

    o = run(p, dep("a", "~> 1.0") + dep("b", "1.0"), settings);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(lock_changes(current, *make_lock(*o.r, *o.m, project.path())), std::vector<std::string>{"added b 1.0 (r1)"});

    o = run(p, dep("a", ">= 1.1"), settings);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(lock_changes(current, *make_lock(*o.r, *o.m, project.path())), std::vector<std::string>{"a: 1.0 (r1) -> 1.1 (r1)"});

    o = run(p, dep("b", "1.0"), settings);
    ASSERT_TRUE(o.r) << o.text();
    EXPECT_EQ(lock_changes(current, *make_lock(*o.r, *o.m, project.path())),
        (std::vector<std::string>{"added b 1.0 (r1)", "removed a 1.0 (r1)"}));
}

// A recipe tree that moved (another commit or directory) is told apart from one that changed.
TEST(locking, recipe_moved_or_changed)
{
    locked_package a;
    a.library = "a";
    a.target = "apple";
    a.build_on = {"macos/xcode"};
    a.kind = locked_package::recipe_kind::git;
    a.version = *version::parse("1.0");
    a.revision = 1;
    a.recipe_source = {.type = locked_location::kind::git, .git = "https://example.com/r", .commit = std::string(40, 'c'), .path = "a"};
    a.recipe_sha256 = std::string(64, '1');
    lock_file current;
    current.packages.push_back(a);

    lock_file fresh = current;
    fresh.packages[0].recipe_source.path = "a/1.0";
    fresh.packages[0].recipe_source.commit = std::string(40, 'd');
    EXPECT_EQ(lock_changes(current, fresh), std::vector<std::string>{"a: recipe moved, contents unchanged"});
    fresh.packages[0].recipe_sha256 = std::string(64, '2');
    EXPECT_EQ(lock_changes(current, fresh), std::vector<std::string>{"a: recipe contents changed"});
}

TEST(locking, conflicts)
{
    locked_package base;
    base.library = "a";
    base.target = "apple";
    base.build_on = {"macos/xcode"};
    base.kind = locked_package::recipe_kind::git;
    base.version = *version::parse("1.0");
    base.revision = 1;
    base.recipe_source = {.type = locked_location::kind::git, .git = "https://example.com/r", .commit = std::string(40, 'c'), .path = "otok"};
    base.recipe_sha256 = std::string(64, '1');
    base.source = {.type = locked_location::kind::archive, .url = "https://example.com/a.tgz", .sha256 = std::string(64, 'a')};
    lock_file const current{.packages = {base}};
    auto conflicts_with = [&](locked_package const& p) { return lock_conflicts(current, lock_file{.packages = {p}}); };
    std::string const remedy = "; if the new contents are trusted, remove the package from otok.lock and run 'otok install'";

    EXPECT_TRUE(conflicts_with(base).empty());

    // the same commit with another tree
    locked_package p = base;
    p.recipe_sha256 = std::string(64, '2');
    EXPECT_EQ(conflicts_with(p), std::vector<std::string>{"'a': the recipe at https://example.com/r commit " + std::string(40, 'c')
        + " ('otok') has recipe-sha256 " + std::string(64, '2') + ", but otok.lock records " + std::string(64, '1') + remedy});

    // another commit: an update, not a conflict
    p.recipe_source.commit = std::string(40, 'd');
    p.version = *version::parse("1.1");
    EXPECT_TRUE(conflicts_with(p).empty());

    // the same tree with other inputs: an edited lock
    std::string const edited = "'a': otok.lock records another version or other inputs than its recipe (1.0 (r1)) declares; was otok.lock edited?" + remedy;
    p = base;
    p.source.sha256 = std::string(64, 'b');
    EXPECT_EQ(conflicts_with(p), std::vector<std::string>{edited});
    p = base;
    p.resources.push_back({"data", {.type = locked_location::kind::archive, .url = "https://example.com/d", .sha256 = std::string(64, 'd')}});
    EXPECT_EQ(conflicts_with(p), std::vector<std::string>{edited});
    p = base;
    p.revision = 2;
    EXPECT_NE(conflicts_with(p).at(0).find("(1.0 (r2))"), std::string::npos);

    // source overrides come from the manifest
    p = base;
    p.source = {.type = locked_location::kind::local};
    EXPECT_TRUE(conflicts_with(p).empty());

    // a recipe by path inside the project changes with it
    locked_package by_path = base;
    by_path.kind = locked_package::recipe_kind::path;
    by_path.recipe_source = {.type = locked_location::kind::path, .path = "recipes/a"};
    p = by_path;
    p.recipe_sha256 = std::string(64, '2');
    p.version = *version::parse("1.1");
    EXPECT_TRUE(lock_conflicts(lock_file{.packages = {by_path}}, lock_file{.packages = {p}}).empty());

    // tool packages are checked too, with their own prefix
    p = base;
    p.recipe_sha256 = std::string(64, '2');
    auto const tool = lock_conflicts(lock_file{.tool_packages = {base}}, lock_file{.tool_packages = {p}});
    ASSERT_EQ(tool.size(), 1u);
    EXPECT_EQ(tool[0].find("tool 'a': the recipe at "), 0u) << tool[0];
}

TEST(local_recipes, git_recipes)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    dir.write("repo/otok/recipe.toml", recipe_text({.library = "a", .version = "1.2"}));
    dir.write("repo/README", "upstream\n");
    std::filesystem::path const repo = dir.path() / "repo";
    std::vector<std::string> const identity{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
    auto git_run = [&](std::vector<std::string> args) {
        std::vector<std::string> full = identity;
        full.insert(full.end(), args.begin(), args.end());
        auto out = run_git(*git, full, repo, dir.path() / "setup.log", true);
        EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message);
        std::string text = out.value_or("");
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        return text;
    };
    git_run({"init", "-q"});
    git_run({"add", "."});
    git_run({"commit", "-q", "-m", "recipe"});
    std::string const commit = git_run({"rev-parse", "HEAD"});

    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    test::temp_dir project;
    project_recipe_provider provider{project.path(), apple_targets, &*cache, *git};
    std::string const url = file_url(repo);
    auto o = run(provider, "[deps.a]\nrecipe = { git = \"" + url + "\", rev = \"" + commit + "\", path = \"otok\" }\n");
    ASSERT_TRUE(o.r) << o.text();
    candidate const& a = first(*o.r, "a")->chosen;
    EXPECT_EQ(o.version("a"), "1.2");
    EXPECT_EQ(a.kind, locked_package::recipe_kind::git);
    EXPECT_EQ(a.recipe_source.git, url);
    EXPECT_EQ(a.recipe_source.commit, commit);
    EXPECT_EQ(a.recipe_source.path, "otok");
    EXPECT_EQ(a.recipe_sha256, tree_hash(cache->root() / "sources" / ("git-" + commit + "-dfd2aa79320a5ecb")).value_or("?"));  // the directory alone (cache.md 5)

    o = run(provider, "[deps.a]\nrecipe = { git = \"" + url + "\", rev = \"" + std::string(40, 'b') + "\" }\n");
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("cannot fetch the recipe of 'a': the repository " + url + " has no commit"), std::string::npos) << o.text();

    project_recipe_provider without_cache{project.path(), apple_targets};
    o = run(without_cache, "[deps.a]\nrecipe = { git = \"" + url + "\", rev = \"" + commit + "\" }\n");
    ASSERT_FALSE(o.r);
    EXPECT_NE(o.text().find("git recipes need the cache"), std::string::npos) << o.text();
}
