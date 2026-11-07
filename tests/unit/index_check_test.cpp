// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Index CI (index.md 9) and the probe of inputs (index.md 11), docs/PLAN.md decision 47: changes of a
// git checkout of an index, the recipes in a git repository of their own, GitHub answered by a fake.

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "otok/cache/store.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"
#include "otok/fetch/http.hpp"
#include "otok/index/check.hpp"
#include "otok/index/index.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

target_info const* const targets[] = {&test::native_info()};

std::chrono::system_clock::time_point const now = std::chrono::sys_days{std::chrono::year{2026} / 11 / 2} + std::chrono::hours{12};

toml::date_time published_at(std::chrono::system_clock::time_point tp)
{
    using namespace std::chrono;
    auto const s = floor<seconds>(tp);
    auto const day = floor<days>(s);
    year_month_day const ymd{day};
    hh_mm_ss const hms{s - day};
    return {toml::date{static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day())},
        toml::time{static_cast<unsigned>(hms.hours().count()), static_cast<unsigned>(hms.minutes().count()), static_cast<unsigned>(hms.seconds().count())},
        toml::time_offset{}};
}

std::string recipe_toml(std::string const& name, std::string const& version, std::string const& url, std::string const& sha256)
{
    return "[recipe]\napi = 1\nlibrary = \"" + name + "\"\nname = \"" + name + "\"\nversion = \"" + version + "\"\nrevision = 1\n"
        "license = \"MIT\"\nlicense-files = [\"LICENSE\"]\ndescription = \"Test.\"\n"
        "[source]\nurl = \"" + url + "\"\nsha256 = \"" + sha256 + "\"\n"
        "[platforms]\nsupported = [\"native\"]\nbuild-on = [\"*/cc\"]\n[[product]]\nname = \"P\"\nlibraries = [\"" + name + "\"]\n";
}

std::string replace_in(std::string text, std::string const& from, std::string const& to)
{
    std::size_t const at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    return at == std::string::npos ? text : text.replace(at, from.size(), to);
}

class index_check : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto found = test::find_test_git();
        if (!found) GTEST_SKIP() << "git is not installed";
        git = std::move(*found);
        std::filesystem::path const archive = dir.path() / "upstream" / "a-1.0.tar.gz";
        test::write_tar_gz(archive, {{"a-1.0/a.c", "int a;\n"}, {"a-1.0/LICENSE", "MIT\n"}});
        archive_url = file_url(archive);
        archive_sha = sha256_file(archive).value_or("?");
        // the recipes, in a repository of their own
        dir.write("recipes/a/recipe.toml", recipe_toml("a", "1.0", archive_url, archive_sha));
        dir.write("recipes/b/recipe.toml", recipe_toml("b", "1.0", archive_url, archive_sha));
        run(dir.path() / "recipes", {"init", "-q"});
        run(dir.path() / "recipes", {"add", "."});
        run(dir.path() / "recipes", {"commit", "-q", "-m", "recipes"});
        recipes_commit = run(dir.path() / "recipes", {"rev-parse", "HEAD"});
        // the index, publisher p registered and established
        dir.write("index/index.toml", "format = 1\n");
        dir.write("index/publishers/p.toml", "github-id = 100\nkind = \"user\"\nestablished = true\n");
        run(index(), {"init", "-q", "-b", "main"});
        run(index(), {"add", "."});
        run(index(), {"commit", "-q", "-m", "index"});
        base = run(index(), {"rev-parse", "HEAD"});
        auto opened = cache_store::open(dir.path() / "cache");
        ASSERT_TRUE(opened.has_value()) << opened.error().message;
        cache.emplace(std::move(*opened));
        github["/user/100"] = test::http_answer(200, R"({"login":"pub","created_at":"2020-01-01T00:00:00Z"})");
        github["/user/200"] = test::http_answer(200, R"({"login":"other","created_at":"2020-06-01T00:00:00Z"})");
    }

    std::filesystem::path index() const { return dir.path() / "index"; }

    std::string run(std::filesystem::path const& where, std::vector<std::string> args)
    {
        std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
        full.insert(full.end(), args.begin(), args.end());
        auto out = run_git(*git, full, where, dir.path() / "git.log", true);
        EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message);
        std::string text = out.value_or("");
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        return text;
    }

    // The entry of a recipe of the repository, published by `publisher`: as a pull request proposes it, or merged `at`.
    std::string entry_of(std::string const& name, std::string const& publisher = "p", std::optional<std::chrono::system_clock::time_point> at = std::nullopt)
    {
        locked_location const source{.type = locked_location::kind::git, .git = file_url(dir.path() / "recipes"), .commit = recipes_commit, .path = name};
        auto a = make_index_entry(dir.path() / "recipes" / name, source, publisher, at ? std::optional{published_at(*at)} : std::nullopt, targets);
        EXPECT_TRUE(a.has_value()) << (a ? "" : a.error().message);
        return a ? a->entry_text : std::string{};
    }

    // A new base: the files committed on the current one.
    void extend_base(std::map<std::string, std::string> const& files)
    {
        propose(files);
        base = run(index(), {"rev-parse", "HEAD"});
    }

    // A pull request on the base: the files, committed.
    void propose(std::map<std::string, std::string> const& files, std::vector<std::string> const& removed = {})
    {
        run(index(), {"checkout", "-q", "-B", "pr", base});
        for (auto const& [path, content] : files) dir.write("index/" + path, content);
        for (std::string const& path : removed) std::filesystem::remove(index() / path_from_utf8(path));
        run(index(), {"add", "-A"});
        run(index(), {"commit", "-q", "--allow-empty", "-m", "pull request"});
    }

    index_check_report check(std::int64_t author = 100)
    {
        index_check_settings s;
        s.root = index();
        s.base = base;
        s.author_id = author;
        s.repository = repository;
        s.pull_request = 9;
        s.approved_by = approved_by;
        s.stamp = stamp;
        s.now = now;
        s.targets = targets;
        s.cache = &*cache;
        s.git = &*git;
        s.https_only = false;
        s.github.transport = [this](http_exchange const& e) -> result<http_reply> {
            std::string const path = e.url.substr(std::string{"https://api.github.com"}.size());
            if (auto it = github.find(path); it != github.end()) return it->second;
            return test::http_answer(404, R"({"message":"Not Found"})");
        };
        auto r = check_index(s);
        EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message);
        return r.value_or(index_check_report{});
    }

    // The open pull requests of the index "idx/index" (`repository`, set), each with the files it adds; #9 is the one checked.
    void open_requests(std::vector<std::tuple<int, std::int64_t, std::vector<std::string>>> const& requests)
    {
        repository = "idx/index";
        nlohmann::json list = nlohmann::json::array();
        for (auto const& [number, author, added] : requests) {
            list.push_back({{"number", number}, {"user", {{"id", author}}}, {"head", {{"label", "x:publish/" + std::to_string(number)}}}});
            nlohmann::json files = nlohmann::json::array();
            for (std::string const& f : added) files.push_back({{"filename", f}, {"status", "added"}});
            files.push_back({{"filename", "libraries/x.toml"}, {"status", "modified"}});
            github["/repos/idx/index/pulls/" + std::to_string(number) + "/files?per_page=100&page=1"] = test::http_answer(200, files.dump());
        }
        github["/repos/idx/index/pulls?state=open&per_page=100&page=1"] = test::http_answer(200, list.dump());
    }

    static std::string all(index_check_report const& r)
    {
        std::string text;
        for (auto const* list : {&r.errors, &r.reasons, &r.notes}) {
            for (std::string const& s : *list) text += s + "\n";
        }
        return text;
    }

    test::temp_dir dir;
    std::optional<git_tool> git;
    std::optional<cache_store> cache;
    std::string archive_url, archive_sha, recipes_commit, base, repository;
    std::optional<std::int64_t> approved_by;
    bool stamp = false;
    std::map<std::string, http_reply> github;  // API path → reply
};

}

TEST_F(index_check, merges_new_entries_of_their_publisher)
{
    propose({{"recipes/p/a/native/1.0-r1.toml", entry_of("a")}, {"libraries/a.toml", "scheme = \"dotted\"\n"}});
    index_check_report const r = check();
    EXPECT_EQ(r.verdict, index_verdict::automatic) << all(r);
    EXPECT_EQ(r.entries, 1u);
    nlohmann::json const summary = nlohmann::json::parse(summary_json(r));
    EXPECT_EQ(summary["verdict"], "auto");
    EXPECT_EQ(summary["format"], 1);
    EXPECT_TRUE(summary["reasons"].empty());

    // someone else may not
    index_check_report const other = check(200);
    EXPECT_EQ(other.verdict, index_verdict::refused);
    EXPECT_NE(all(other).find("p/a 1.0-r1 (native): the author (GitHub account 200) is not the publisher 'p'"), std::string::npos) << all(other);
    EXPECT_EQ(nlohmann::json::parse(summary_json(other))["verdict"], "refused");
}

// index.md 3, 9.2: the workflow writes the time of the merge into the entries it merges, and merges what a maintainer approved
TEST_F(index_check, writes_the_time_of_the_merge)
{
    std::string const path = "recipes/p/a/native/1.0-r1.toml";
    propose({{path, entry_of("a")}, {"libraries/a.toml", "scheme = \"dotted\"\n"}});
    stamp = true;
    index_check_report r = check();
    EXPECT_EQ(r.verdict, index_verdict::automatic) << all(r);
    EXPECT_EQ(r.stamped, std::vector<std::string>{path});
    auto const text = read_file(index() / path);
    ASSERT_TRUE(text.has_value());
    diagnostics d;
    auto const e = parse_index_entry(*text, path, targets, d);
    ASSERT_TRUE(e.has_value()) << *text;
    ASSERT_TRUE(e->published.has_value());
    EXPECT_EQ(*e->published, published_at(now));
    auto const same = same_index_entry(*text, entry_of("a"), path, targets);
    ASSERT_TRUE(same.has_value()) << same.error().message;
    EXPECT_TRUE(*same);
    EXPECT_EQ(*text, stamp_index_entry(entry_of("a"), published_at(now)).value_or(""));
    run(index(), {"checkout", "-q", "--", "."});

    // waiting for a maintainer: nothing is written
    extend_base({{"publishers/q.toml", "github-id = 200\nkind = \"user\"\n"}});
    std::string const waiting = "recipes/q/b/native/1.0-r1.toml";
    propose({{waiting, entry_of("b", "q")}});
    r = check(200);
    EXPECT_EQ(r.verdict, index_verdict::maintainer) << all(r);
    EXPECT_TRUE(r.stamped.empty());
    EXPECT_EQ(read_file(index() / waiting).value_or(""), entry_of("b", "q"));

    // approved: by someone who is not a maintainer, nothing changes; by a maintainer, the workflow merges it
    open_requests({});
    github["/repos/idx/index/collaborators/pub/permission"] = test::http_answer(200, R"({"permission":"read","role_name":"read"})");
    approved_by = 100;
    r = check(200);
    EXPECT_EQ(r.verdict, index_verdict::maintainer) << all(r);
    EXPECT_EQ(r.notes, std::vector<std::string>{"approved by pub, who is not an index maintainer"});
    github["/repos/idx/index/collaborators/pub/permission"] = test::http_answer(200, R"({"permission":"write","role_name":"maintain"})");
    r = check(200);
    EXPECT_EQ(r.verdict, index_verdict::automatic) << all(r);
    EXPECT_EQ(r.notes, std::vector<std::string>{"approved by the index maintainer pub"});
    EXPECT_EQ(r.stamped, std::vector<std::string>{waiting});
    run(index(), {"checkout", "-q", "--", "."});

    // an approval does not let a refused one pass
    propose({{waiting, entry_of("b", "q", now)}});
    r = check(200);
    EXPECT_EQ(r.verdict, index_verdict::refused) << all(r);
    EXPECT_TRUE(r.stamped.empty());
}

TEST_F(index_check, refuses_entries_that_do_not_match_their_tree)
{
    std::string entry = entry_of("a");
    std::size_t const at = entry.find("recipe-sha256 = \"") + 17;
    entry.replace(at, 64, std::string(64, '0'));
    propose({{"recipes/p/a/native/1.0-r1.toml", entry}});
    index_check_report r = check();
    EXPECT_EQ(r.verdict, index_verdict::refused);
    EXPECT_NE(all(r).find("p/a 1.0-r1 (native): the entry does not match its recipe tree at"), std::string::npos) << all(r);

    // under the wrong file name; with the time of its publication, which the workflow writes
    propose({{"recipes/p/a/native/2.0-r1.toml", entry_of("a")}, {"recipes/p/b/native/1.0-r1.toml", entry_of("b", "p", now)}});
    r = check();
    EXPECT_NE(all(r).find("recipes/p/a/native/2.0-r1.toml: the entry is of the variant 'native' and version 1.0-r1, which does not match its file"), std::string::npos) << all(r);
    EXPECT_NE(all(r).find("p/b 1.0-r1 (native): 'published' is written by index CI when it merges the entry, never by a pull request (index.md 3)"), std::string::npos) << all(r);
}

// index.md 3, 9: an entry in the directory of its variant, sharing no cell with another variant of its version
TEST_F(index_check, refuses_variants_that_share_a_cell)
{
    propose({{"recipes/p/a/1.0-r1.toml", entry_of("a")}});
    index_check_report r = check();
    EXPECT_NE(all(r).find("recipes/p/a/1.0-r1.toml: a version entry belongs in the directory of its variant"), std::string::npos) << all(r);

    extend_base({{"recipes/p/a/native/1.0-r1.toml", entry_of("a")}});
    // a variant of its own environment: */cc covers linux/cc
    dir.write("recipes/a/recipe.toml", replace_in(replace_in(recipe_toml("a", "1.0", archive_url, archive_sha), "build-on = [\"*/cc\"]", "build-on = [\"linux/cc\"]"),
        "name = \"a\"\n", "name = \"a\"\nvariant = \"linux\"\n"));
    run(dir.path() / "recipes", {"commit", "-q", "-a", "-m", "linux"});
    recipes_commit = run(dir.path() / "recipes", {"rev-parse", "HEAD"});
    propose({{"recipes/p/a/linux/1.0-r1.toml", entry_of("a")}});
    r = check();
    EXPECT_EQ(r.verdict, index_verdict::refused);
    EXPECT_NE(all(r).find("recipes/p/a/linux/1.0-r1.toml: the variant 'native' builds 'native' in */cc for this version already, which this one "
        "builds in linux/cc (index.md 3)"), std::string::npos) << all(r);
}

// index.md 9, docs/PLAN.md decision 85: the recipe of a toolchain is in the index, or comes with the pull request
TEST_F(index_check, needs_the_recipes_of_toolchains_in_the_index)
{
    dir.write("recipes/c/recipe.toml", recipe_toml("c", "1.0", archive_url, archive_sha)
        + "[toolchains]\nninja = { version = \">= 1.10\", default-recipe = \"p/ninja\" }\n");
    dir.write("recipes/ninja/recipe.toml", replace_in(recipe_toml("ninja", "1.13", archive_url, archive_sha),
        "[[product]]\nname = \"P\"\nlibraries = [\"ninja\"]\n", "[tool]\nprograms = [\"ninja\"]\n"));
    run(dir.path() / "recipes", {"add", "."});
    run(dir.path() / "recipes", {"commit", "-q", "-m", "tools"});
    recipes_commit = run(dir.path() / "recipes", {"rev-parse", "HEAD"});

    propose({{"recipes/p/c/native/1.0-r1.toml", entry_of("c")}, {"libraries/c.toml", "scheme = \"dotted\"\n"}});
    index_check_report r = check();
    EXPECT_EQ(r.verdict, index_verdict::refused);
    EXPECT_NE(all(r).find("p/c 1.0-r1 (native): the toolchain 'ninja' has the 'default-recipe' 'p/ninja', which has no entry of a tool recipe "
        "building 'ninja' in the index (recipe-toml.md 13.1)"), std::string::npos) << all(r);

    propose({{"recipes/p/c/native/1.0-r1.toml", entry_of("c")}, {"libraries/c.toml", "scheme = \"dotted\"\n"},
        {"recipes/p/ninja/native/1.13-r1.toml", entry_of("ninja")}, {"libraries/ninja.toml", "scheme = \"dotted\"\n"}});
    r = check();
    EXPECT_EQ(r.verdict, index_verdict::automatic) << all(r);
}

TEST_F(index_check, keeps_entries_and_generated_files)
{
    extend_base({{"recipes/p/a/native/1.0-r1.toml", entry_of("a")}, {"advisories/2026-0001.toml", "id = \"2026-0001\"\nrecipes = [\"p/a\"]\nversions = [\"1.0\"]\nseverity = \"broken\"\n"
                                                 "status = \"confirmed\"\nsummary = \"Slow.\"\npublished = 2026-11-01T10:00:00Z\n"}});

    propose({{"recipes/p/a/native/1.0-r1.toml", entry_of("a") + "# changed\n"}});
    index_check_report r = check();
    EXPECT_EQ(r.verdict, index_verdict::refused);
    EXPECT_NE(all(r).find("recipes/p/a/native/1.0-r1.toml: version entries never change once published"), std::string::npos) << all(r);

    propose({}, {"advisories/2026-0001.toml"});
    EXPECT_NE(all(check()).find("advisories/2026-0001.toml: advisories are never deleted"), std::string::npos);
    propose({{"availability.toml", "checked = 2026-11-01T00:00:00Z\n"}});
    EXPECT_NE(all(check()).find("availability.toml is written by the index's probe workflow"), std::string::npos);
}

TEST_F(index_check, leaves_registrations_to_a_maintainer)
{
    propose({{"publishers/q.toml", "github-id = 200\nkind = \"user\"\n"}, {"recipes/q/b/native/1.0-r1.toml", entry_of("b", "q")}});
    index_check_report r = check(200);
    EXPECT_EQ(r.verdict, index_verdict::maintainer) << all(r);
    EXPECT_EQ(r.reasons, std::vector<std::string>{"registers the publisher 'q'"});
    propose({{"publishers/q.toml", "github-id = 200\nkind = \"user\"\nestablished = true\n"}});
    EXPECT_EQ(check(200).reasons, std::vector<std::string>{"registers the publisher 'q' as established"});

    // one open registration of an author at a time
    open_requests({{7, 200, {"publishers/r.toml"}}, {9, 200, {"publishers/q.toml"}}});
    propose({{"publishers/q.toml", "github-id = 200\nkind = \"user\"\n"}});
    EXPECT_EQ(check(200).errors, std::vector<std::string>{"the author has another open pull request registering a publisher, #7"});
    open_requests({{7, 100, {"publishers/r.toml"}}, {9, 200, {"publishers/q.toml"}}});
    EXPECT_EQ(check(200).verdict, index_verdict::maintainer) << all(check(200));
    repository.clear();

    // a young account; a name like another; a file naming someone else
    github["/user/200"] = test::http_answer(200, R"({"login":"newcomer","created_at":"2026-10-20T00:00:00Z"})");
    propose({{"publishers/q.toml", "github-id = 200\nkind = \"user\"\n"}});
    EXPECT_EQ(check(200).errors, std::vector<std::string>{"the author's GitHub account, 200, is less than 30 days old"});
    extend_base({{"publishers/apotocki.toml", "github-id = 400\nkind = \"user\"\n"}});
    propose({{"publishers/ap0-tocki.toml", "github-id = 200\nkind = \"user\"\n"}});
    EXPECT_NE(all(check(200)).find("'ap0-tocki' is too much like the registered publisher 'apotocki'"), std::string::npos) << all(check(200));
    propose({{"publishers/q.toml", "github-id = 300\nkind = \"user\"\n"}});
    EXPECT_NE(all(check(200)).find("'github-id' must be the author's GitHub account, 200"), std::string::npos) << all(check(200));

    // a maintainer registers an organization
    repository = "idx/index";
    github["/repos/idx/index/collaborators/pub/permission"] = test::http_answer(200, R"({"permission":"admin","role_name":"admin"})");
    propose({{"publishers/the-org.toml", "github-id = 300\nkind = \"organization\"\n"}});
    r = check();
    EXPECT_EQ(r.verdict, index_verdict::maintainer) << all(r);
    EXPECT_EQ(r.reasons, std::vector<std::string>{"registers the publisher 'the-org'"});
    repository.clear();
}

TEST_F(index_check, sorts_out_advisories_libraries_and_the_rest)
{
    std::string const advisory = "id = \"2026-0002\"\nrecipes = [\"p/a\"]\nversions = [\"1.0\"]\nseverity = \"broken\"\n"
                                 "status = \"confirmed\"\nsummary = \"Slow.\"\npublished = 2026-11-01T10:00:00Z\n";
    propose({{"advisories/2026-0002.toml", advisory}});
    EXPECT_EQ(check().verdict, index_verdict::automatic);
    EXPECT_EQ(check(200).verdict, index_verdict::maintainer);  // naming another's recipes
    propose({{"advisories/2026-0003.toml", advisory}});
    EXPECT_NE(all(check()).find("the advisory's id '2026-0002' must be its file name"), std::string::npos);

    extend_base({{"libraries/a.toml", "scheme = \"explicit\"\norder = [\"beta\", \"1.0\"]\n"}});
    // a library file of an established publisher's entries; alone, it waits for a maintainer
    std::string const alone = "changes library files without entries of an established publisher";
    propose({{"libraries/a.toml", "scheme = \"explicit\"\norder = [\"beta\", \"1.0\", \"2.0\"]\n"}, {"recipes/p/a/native/1.0-r1.toml", entry_of("a")}});
    EXPECT_EQ(check().verdict, index_verdict::automatic) << all(check());
    propose({{"libraries/a.toml", "scheme = \"explicit\"\norder = [\"beta\", \"1.0\", \"2.0\"]\n"}});
    EXPECT_EQ(check().reasons, std::vector<std::string>{alone});
    propose({{"libraries/a.toml", "scheme = \"explicit\"\norder = [\"1.0\", \"beta\"]\n"}});
    EXPECT_EQ(check().reasons, (std::vector<std::string>{"reorders the versions of libraries/a.toml", alone}));
    propose({{"libraries/a.toml", "scheme = \"semver\"\n"}});
    EXPECT_EQ(check().reasons, (std::vector<std::string>{"changes the scheme of libraries/a.toml", alone}));

    propose({{"README.md", "hello\n"}, {"recipes/p/a/recipe-status.toml", "[deprecated]\nreason = \"old\"\n"}});
    index_check_report const r = check();
    EXPECT_EQ(r.verdict, index_verdict::maintainer);
    EXPECT_NE(all(r).find("changes 'README.md', which is not one of the index's files"), std::string::npos) << all(r);
    EXPECT_NE(all(r).find("changes the status of 'p/a'"), std::string::npos) << all(r);
}

TEST_F(index_check, limits_entries_a_day_and_asks_organizations)
{
    // 20 entries of p published in the last day already
    std::map<std::string, std::string> many;
    for (int i = 0; i < 20; ++i) {
        std::string entry = entry_of("a", "p", now);
        std::size_t const v = entry.find("version = \"1.0\"");
        entry.replace(v, 15, "version = \"0." + std::to_string(i) + "\"");
        many["recipes/p/a/0." + std::to_string(i) + "-r1.toml"] = entry;
    }
    extend_base(many);
    propose({{"recipes/p/b/native/1.0-r1.toml", entry_of("b")}});
    index_check_report r = check();
    EXPECT_EQ(r.verdict, index_verdict::refused) << all(r);
    EXPECT_EQ(all(r), "the publisher 'p' has 20 entries published in the last 24 hours, and this one makes 21, more than 20 of the index "
                      "(established-entries-per-day); they pass when published again after 2026-11-03 12:00 UTC\n");

    // an index maintainer: no limit
    open_requests({});
    r = check();
    EXPECT_EQ(r.verdict, index_verdict::refused) << all(r);  // GitHub does not say so
    github["/repos/idx/index/collaborators/pub/permission"] = test::http_answer(200, R"({"permission":"write","role_name":"maintain"})");
    r = check();
    EXPECT_EQ(r.verdict, index_verdict::automatic) << all(r);
    EXPECT_EQ(all(r), "the publisher 'p' has 21 entries published in the last 24 hours, more than 20; the author is an index maintainer, whom the limit does not hold\n");
    github["/repos/idx/index/collaborators/pub/permission"] = test::http_answer(200, R"({"permission":"read","role_name":"triage"})");
    EXPECT_EQ(check().verdict, index_verdict::refused);
    repository.clear();

    // the limit of the base's index.toml
    extend_base({{"index.toml", "format = 1\nestablished-entries-per-day = 21\n"}});
    propose({{"recipes/p/b/native/1.0-r1.toml", entry_of("b")}});
    EXPECT_EQ(check().verdict, index_verdict::automatic) << all(check());
    propose({{"index.toml", "format = 1\nestablished-entries-per-day = 40\n"}, {"recipes/p/b/native/1.0-r1.toml", entry_of("b")},
        {"recipes/p/a/native/1.0-r1.toml", entry_of("a")}});
    r = check();
    EXPECT_EQ(all(r), "the publisher 'p' has 20 entries published in the last 24 hours, and these 2 make 22, more than 21 of the "
                      "index (established-entries-per-day); they pass when published again after 2026-11-03 12:00 UTC\nchanges index.toml\n");
    // more than the limit at once
    extend_base({{"index.toml", "format = 1\nestablished-entries-per-day = 1\n"}});
    propose({{"recipes/p/b/native/1.0-r1.toml", entry_of("b")},
        {"recipes/p/a/native/1.0-r1.toml", entry_of("a")}});
    EXPECT_EQ(all(check()), "these 2 entries are more than 1 of the index (established-entries-per-day): publish at most 1 at a time\n");

    // an organization: its public members publish
    extend_base({{"publishers/org.toml", "github-id = 300\nkind = \"organization\"\n"}});
    github["/user/300"] = test::http_answer(200, R"({"login":"the-org"})");
    propose({{"recipes/org/b/native/1.0-r1.toml", entry_of("b", "org")}});
    r = check();
    EXPECT_NE(all(r).find("the author 'pub' is not a public member of the organization 'the-org'"), std::string::npos) << all(r);
    github["/orgs/the-org/public_members/pub"] = test::http_answer(204, "");
    r = check();
    EXPECT_EQ(r.verdict, index_verdict::maintainer) << all(r);  // not established
    EXPECT_EQ(r.reasons, std::vector<std::string>{"the publisher 'org' is not established: an index maintainer reviews its pull requests (index.md 9.1)"});
}

// index.md 9.1, 9.2: a publisher that is not established waits for a maintainer, within review-entries-per-day counting its
// other open pull requests, and its author's account is at least 30 days old
TEST_F(index_check, leaves_publishers_not_established_to_a_maintainer)
{
    extend_base({{"publishers/q.toml", "github-id = 200\nkind = \"user\"\n"}, {"index.toml", "format = 1\nreview-entries-per-day = 3\n"}});
    propose({{"recipes/q/a/native/1.0-r1.toml", entry_of("a", "q")}, {"libraries/a.toml", "scheme = \"dotted\"\n"}});
    index_check_report r = check(200);
    EXPECT_EQ(r.verdict, index_verdict::maintainer) << all(r);
    EXPECT_EQ(r.reasons, std::vector<std::string>{"the publisher 'q' is not established: an index maintainer reviews its pull requests (index.md 9.1)"});

    // its other open pull requests count
    open_requests({{4, 200, {"recipes/q/b/native/1.0-r1.toml", "recipes/q/b/native/1.1-r1.toml"}}, {5, 300, {"recipes/z/b/native/1.0-r1.toml"}},
        {9, 200, {"recipes/q/a/native/1.0-r1.toml"}}});
    EXPECT_EQ(check(200).verdict, index_verdict::maintainer) << all(check(200));
    open_requests({{4, 200, {"recipes/q/b/native/1.0-r1.toml", "recipes/q/b/native/1.1-r1.toml"}}, {6, 200, {"recipes/q/c/native/1.0-r1.toml"}},
        {9, 200, {"recipes/q/a/native/1.0-r1.toml"}}});
    r = check(200);
    EXPECT_EQ(r.verdict, index_verdict::refused) << all(r);
    EXPECT_EQ(r.errors, std::vector<std::string>{"the publisher 'q' has 3 entries in its other open pull requests (#4, #6), and this one makes 4, more "
                                                 "than 3 of the index (review-entries-per-day); publish them again once those are merged or closed"});
    // with entries published in the last day: when they pass
    open_requests({{4, 200, {"recipes/q/b/native/1.0-r1.toml", "recipes/q/b/native/1.1-r1.toml"}}});
    extend_base({{"recipes/q/b/native/1.0-r1.toml", entry_of("b", "q", now - std::chrono::hours{20})}});
    propose({{"recipes/q/a/native/1.0-r1.toml", entry_of("a", "q")}});
    EXPECT_EQ(check(200).errors, std::vector<std::string>{"the publisher 'q' has 1 entry published in the last 24 hours and 2 entries in its other open "
                                                          "pull requests (#4), and this one makes 4, more than 3 of the index (review-entries-per-day); they pass "
                                                          "when published again after 2026-11-02 16:00 UTC"});
    repository.clear();

    // a young account: refused, unless publishing for an established publisher
    github["/user/200"] = test::http_answer(200, R"({"login":"other","created_at":"2026-10-20T00:00:00Z"})");
    propose({{"recipes/q/a/native/1.0-r1.toml", entry_of("a", "q")}});
    EXPECT_EQ(check(200).errors, std::vector<std::string>{"the author's GitHub account, 200, is less than 30 days old"});
    extend_base({{"publishers/q.toml", "github-id = 200\nkind = \"user\"\nestablished = true\n"}});
    propose({{"recipes/q/a/native/1.0-r1.toml", entry_of("a", "q")}});
    EXPECT_EQ(check(200).verdict, index_verdict::automatic) << all(check(200));

    // making a publisher established is a maintainer's
    propose({{"publishers/p.toml", "github-id = 100\nkind = \"user\"\n"}});
    EXPECT_EQ(check().reasons, std::vector<std::string>{"changes the publisher file of 'p'"});
    extend_base({{"publishers/p.toml", "github-id = 100\nkind = \"user\"\n"}});
    propose({{"publishers/p.toml", "github-id = 100\nkind = \"user\"\nestablished = true\n"}});
    EXPECT_EQ(check().reasons, std::vector<std::string>{"makes the publisher 'p' established"});
}

TEST(index_check_comment, says_why_and_carries_the_summary)
{
    index_check_report r;
    r.verdict = index_verdict::refused;
    r.errors = {"p/a 1.0-r1 (linux): cannot fetch it:\n404 -->"};
    r.reasons = {"changes index.toml"};
    r.notes = {"a note"};
    std::string const comment = check_comment(r);
    EXPECT_EQ(comment,
        "**Refused by the checks of the index**:\n\n"
        "- p/a 1.0-r1 (linux): cannot fetch it:\n  404 -->\n"
        "\nIt would also wait for an index maintainer:\n\n- changes index.toml\n"
        "\nNotes:\n\n- a note\n"
        "\n<!-- otok-index-check {\"format\":1,\"notes\":[\"a note\"],\"reasons\":[\"p/a 1.0-r1 (linux): cannot fetch it:\\n404 --\\u003e\","
        "\"changes index.toml\"],\"verdict\":\"refused\"} -->\n");
    auto const summary = summary_of_comment("Above.\n" + comment);
    ASSERT_TRUE(summary.has_value());
    auto const j = nlohmann::json::parse(*summary);
    EXPECT_EQ(j["verdict"], "refused");
    EXPECT_EQ(j["reasons"][0], "p/a 1.0-r1 (linux): cannot fetch it:\n404 -->");
    EXPECT_FALSE(summary_of_comment("**Passed**").has_value());

    r = {};
    EXPECT_TRUE(check_comment(r).starts_with("**Passed the checks of the index**: the workflow merges it now.\n\n<!-- otok-index-check "));
}

TEST_F(index_check, probes_the_inputs_of_every_version)
{
    dir.write("index/recipes/p/a/native/1.0-r1.toml", entry_of("a"));
    std::string missing = entry_of("b");
    std::size_t const c = missing.find(recipes_commit);
    missing.replace(c, 40, std::string(40, 'a'));  // a commit the repository does not have
    dir.write("index/recipes/p/b/native/1.0-r1.toml", missing);
    std::vector<std::string> asked;
    bool answers = true;
    index_probe_settings s;
    s.root = index();
    s.now = now;
    s.targets = targets;
    s.cache = &*cache;
    s.git = &*git;
    s.http = [&](http_exchange const& e) -> result<http_reply> {
        asked.push_back(e.method + " " + e.url);
        return test::http_answer(answers ? 200 : 404, "");
    };
    auto probed = probe_index(s);
    ASSERT_TRUE(probed.has_value()) << probed.error().message;
    EXPECT_EQ(probed->versions, 2u);
    EXPECT_EQ(probed->unavailable, 1u);
    EXPECT_EQ(asked.front(), "HEAD " + archive_url);
    diagnostics d;
    auto a = parse_availability(probed->availability, "availability.toml", d);
    ASSERT_TRUE(a.has_value()) << probed->availability;
    ASSERT_EQ(a->unavailable.size(), 1u);
    EXPECT_EQ(a->unavailable[0].recipe.str(), "p/b");
    EXPECT_EQ(a->unavailable[0].version, "1.0-r1");
    EXPECT_EQ(a->unavailable[0].inputs, std::vector<std::string>{"recipe " + file_url(dir.path() / "recipes") + " " + std::string(40, 'a')});
    EXPECT_TRUE(probed->availability.starts_with("# Generated by otok index-probe; do not edit.\nchecked = 2026-11-02T12:00:00Z\n"))
        << probed->availability;

    // a week later the archive is gone too; p/b keeps when it went missing
    dir.write("index/availability.toml", probed->availability);
    answers = false;
    s.now = now + std::chrono::days{7};
    probed = probe_index(s);
    ASSERT_TRUE(probed.has_value()) << probed.error().message;
    a = parse_availability(probed->availability, "availability.toml", d);
    ASSERT_TRUE(a.has_value()) << probed->availability;
    ASSERT_EQ(a->unavailable.size(), 2u);
    EXPECT_EQ(a->unavailable[0].recipe.str(), "p/a");
    EXPECT_EQ(a->unavailable[0].inputs, std::vector<std::string>{"source " + archive_url});
    EXPECT_EQ(a->unavailable[0].since.date.day, 9u);
    EXPECT_EQ(a->unavailable[1].since.date.day, 2u);
    // a GET of the first byte is tried after a HEAD
    EXPECT_NE(std::ranges::find(asked, "GET " + archive_url), asked.end());
}
