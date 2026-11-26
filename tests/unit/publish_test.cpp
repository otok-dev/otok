// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// otok publish (docs/PLAN.md decision 46): the project a recipe is built with, and pull requests through the
// GitHub API, answered here by a fake.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "otok/manifest/manifest.hpp"
#include "otok/publish/bump.hpp"
#include "otok/publish/github.hpp"
#include "otok/publish/publish.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

target_info const* const targets[] = {&target::apple::info(), &test::native_info()};
std::string replace_in(std::string text, std::string const& from, std::string const& to)
{
    std::size_t const at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    return at == std::string::npos ? text : text.replace(at, from.size(), to);
}

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += format(item) + "\n";
    return text;
}

std::string recipe_text(std::string_view extra = "", std::string_view platforms = "\"iossim\", \"ios\"")
{
    return "[recipe]\napi = 1\nlibrary = \"a\"\nname = \"a\"\nversion = \"1.0\"\nrevision = 1\nlicense = \"MIT\"\nlicense-files = [\"LICENSE\"]\n"
        "description = \"Test.\"\n[source]\nurl = \"https://example.com/a.tar.gz\"\nsha256 = \"" + std::string(64, 'a') + "\"\n"
        "[platforms]\nsupported = [" + std::string{platforms} + "]\n" + test::build_on_line(platforms) + "architectures = { iossim = [\"arm64\"] }\n"
        "[options.fast]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\n"
        "[options.parts]\ntype = \"list\"\nvalues = [\"x\", \"y\"]\ndefault = []\ndoc = \"d\"\n"
        "[[product]]\nname = \"A\"\nlibraries = [\"a\"]\n" + std::string{extra};
}

recipe parsed_recipe(std::string const& text)
{
    diagnostics d;
    auto r = parse_recipe(text, "recipe.toml", targets, {}, d);
    EXPECT_TRUE(r.has_value()) << messages(d);
    return r ? std::move(*r) : recipe{};
}

locked_location const source{.type = locked_location::kind::git, .git = "https://example.com/recipes",
    .commit = "0123456789abcdef0123456789abcdef01234567", .path = "a"};

// A fake GitHub: answers by method and path, records the calls.
struct fake_github
{
    std::map<std::string, std::vector<http_reply>> replies;   // "POST /repos/o/r/pulls" → replies in turn
    std::vector<http_exchange> calls;

    void on(std::string const& call, long status, nlohmann::json const& body)
    {
        replies[call].push_back(test::http_answer(status, body.dump()));
    }

    http_transport transport()
    {
        return [this](http_exchange const& e) -> result<http_reply> {
            calls.push_back(e);
            std::string const path = e.url.substr(std::string{"https://api.github.com"}.size());
            auto it = replies.find(e.method + " " + path);
            if (it == replies.end() || it->second.empty()) return test::http_answer(404, R"({"message":"Not Found"})");
            http_reply r = it->second.front();
            if (it->second.size() > 1) it->second.erase(it->second.begin());
            return r;
        };
    }

    // The JSON body of the first call to `call`.
    nlohmann::json body_of(std::string const& call) const
    {
        for (http_exchange const& e : calls) {
            if (e.method + " " + e.url.substr(std::string{"https://api.github.com"}.size()) == call) return nlohmann::json::parse(e.body);
        }
        return nullptr;
    }

    void repository(bool push)
    {
        on("GET /repos/idx/index", 200, {{"default_branch", "main"}, {"permissions", {{"push", push}}}});
        on("GET /repos/idx/index/git/ref/heads/main", 200, {{"object", {{"sha", "head1"}}}});
    }
};

std::vector<github_file> const entry_files{{"recipes/p/a/apple/1.0-r1.toml", "format = 1\n"}, {"libraries/a.toml", "scheme = \"dotted\"\n"}};

}

TEST(publish_project, builds_what_this_machine_can)
{
    recipe const r = parsed_recipe(recipe_text("[publish.apple]\ndeployment = { ios = \"16.0\" }\ncflags = [\"-DX\"]\n"));
    option_values options;
    options["fast"].value = true;
    options["parts"].value = std::vector<std::string>{"x"};
    auto project = make_publish_project(r, source, target::apple::info(), *parse_environment("macos/xcode"), &options);
    ASSERT_TRUE(project.has_value()) << project.error().message;
    EXPECT_EQ(project->platforms, (std::vector<std::string>{"iossim", "ios"}));  // the recipe's order

    diagnostics d;
    auto m = parse_manifest(project->manifest, "otok.toml", targets, {}, d);
    ASSERT_TRUE(m.has_value()) << messages(d) << project->manifest;
    ASSERT_EQ(m->targets.size(), 1u);
    target_settings const& apple = m->targets[0];
    EXPECT_EQ(apple.target, "apple");
    EXPECT_EQ(apple.platforms, (std::vector<std::string>{"iossim", "ios"}));
    EXPECT_EQ(apple.architectures.at("iossim"), (std::vector<std::string>{"arm64"}));
    EXPECT_EQ(apple.deployment.at("ios"), "16.0");     // the recipe's, over the default
    EXPECT_EQ(apple.deployment.at("watchos"), "26.0");  // the default (recipe-toml.md 14)
    EXPECT_EQ(apple.output, "out-apple");
    ASSERT_EQ(apple.build_on.size(), 1u);
    EXPECT_EQ(apple.build_on[0].text(), "macos/xcode");
    ASSERT_EQ(m->deps.size(), 1u);
    dependency const& dep = m->deps[0];
    EXPECT_EQ(dep.library, "a");
    EXPECT_EQ(dep.recipe.source, recipe_ref::kind::git);
    EXPECT_EQ(dep.recipe.git, source.git);
    EXPECT_EQ(dep.recipe.rev, source.commit);
    EXPECT_EQ(dep.recipe.path, "a");
    EXPECT_EQ(dep.options.at("fast").value, option_data{true});
    EXPECT_EQ(dep.options.at("parts").value, option_data{std::vector<std::string>{"x"}});

    // no options
    auto plain = make_publish_project(r, source, target::apple::info(), *parse_environment("macos/xcode"), nullptr);
    ASSERT_TRUE(plain.has_value()) << plain.error().message;
    m = parse_manifest(plain->manifest, "otok.toml", targets, {}, d);
    ASSERT_TRUE(m.has_value()) << messages(d) << plain->manifest;
    EXPECT_TRUE(m->deps[0].options.empty());
}

// a published recipe has one target (index.md 3, docs/PLAN.md decision 82)
TEST(publish_project, is_of_the_target_of_the_recipe)
{
    EXPECT_EQ(publish_target(parsed_recipe(recipe_text()), targets).value_or(nullptr), &target::apple::info());
    auto both = publish_target(parsed_recipe(replace_in(recipe_text("", "\"native\", \"iossim\""), "name = \"a\"\n", "name = \"a\"\nvariant = \"all\"\n")), targets);
    ASSERT_FALSE(both.has_value());
    EXPECT_EQ(both.error().message, "its platforms are of the targets 'native' and 'apple', and a published recipe has one (index.md 3)");
    auto project = make_publish_project(parsed_recipe(recipe_text()), source, test::native_info(), *parse_environment("*/cc"), nullptr);
    ASSERT_FALSE(project.has_value());
    EXPECT_EQ(project.error().message, "it supports no platform of the target 'native'");
}

// a tool recipe without products is a tool dependency only (recipe-toml.md 3.1): [tool-deps], built in the tool slice
// (docs/PLAN.md decision 78)
TEST(publish_project, builds_a_tool_recipe_in_the_tool_slice)
{
    std::string const tool = "[recipe]\napi = 1\nlibrary = \"ninja\"\nname = \"ninja\"\nversion = \"1.13.2\"\nrevision = 1\nlicense = \"Apache-2.0\"\n"
        "license-files = [\"COPYING\"]\ndescription = \"Test.\"\n[tool]\nprograms = [\"ninja\"]\n[source]\nurl = \"https://example.com/n.tar.gz\"\n"
        "sha256 = \"" + std::string(64, 'a') + "\"\n[platforms]\nsupported = [\"native\"]\nbuild-on = [\"*/cc\"]\n";
    recipe const r = parsed_recipe(tool);
    build_environment const machine = *parse_environment(machine_environment("").system + "/cc");
    auto project = make_publish_project(r, source, test::native_info(), machine, nullptr);
    ASSERT_TRUE(project.has_value()) << project.error().message;
    EXPECT_TRUE(project->tool);
    EXPECT_EQ(project->platforms, std::vector<std::string>{"native"});

    diagnostics d;
    target_info const* const native_only[] = {&test::native_info()};  // on macOS apple would build the tools
    auto m = parse_manifest(project->manifest, "otok.toml", native_only, {}, d);
    ASSERT_TRUE(m.has_value()) << messages(d) << project->manifest;
    ASSERT_EQ(m->targets.size(), 1u);
    EXPECT_EQ(m->targets[0].target, "native");
    EXPECT_EQ(m->targets[0].platforms, std::vector<std::string>{"native"});
    // the tool slice in the environment it is published from
    EXPECT_EQ(m->tool_environment(test::native_info()).text(), machine.text());
    EXPECT_TRUE(m->deps.empty());
    ASSERT_EQ(m->tool_deps.size(), 1u);
    EXPECT_EQ(m->tool_deps[0].library, "ninja");
    EXPECT_EQ(m->tool_deps[0].recipe.rev, source.commit);

    // the apple variant: its target's defaults too, the deployment version of the tool platform macosx
    auto apple = make_publish_project(parsed_recipe(replace_in(tool, "supported = [\"native\"]\nbuild-on = [\"*/cc\"]",
        "supported = [\"macosx\"]\nbuild-on = [\"macos/xcode\"]")), source, target::apple::info(), *parse_environment("macos/xcode"), nullptr);
    ASSERT_TRUE(apple.has_value()) << apple.error().message;
    m = parse_manifest(apple->manifest, "otok.toml", targets, {}, d);
    ASSERT_TRUE(m.has_value()) << messages(d) << apple->manifest;
    ASSERT_EQ(m->targets.size(), 1u);
    EXPECT_EQ(m->targets[0].platforms, std::vector<std::string>{"macosx"});
    EXPECT_EQ(m->targets[0].deployment.at("macos"), "12.0");

    // a tool recipe that is also a library is published as a library
    auto library = make_publish_project(parsed_recipe(tool + "[[product]]\nname = \"N\"\nlibraries = [\"n\"]\n"), source, test::native_info(), machine, nullptr);
    ASSERT_TRUE(library.has_value()) << library.error().message;
    EXPECT_FALSE(library->tool);
}

TEST(publish_project, refuses_platforms_in_publish_tables)
{
    diagnostics d;
    EXPECT_FALSE(parse_recipe(recipe_text("[publish.apple]\nplatforms = [\"ios\"]\n"), "recipe.toml", targets, {}, d));
    EXPECT_NE(messages(d).find("'platforms' in publish.apple is not allowed: publication builds every supported platform and architecture"),
        std::string::npos) << messages(d);
}

TEST(github_index, opens_a_pull_request_from_a_branch_of_the_index)
{
    fake_github gh;
    gh.repository(true);
    gh.on("GET /repos/idx/index/git/commits/head1", 200, {{"tree", {{"sha", "tree1"}}}});
    gh.on("POST /repos/idx/index/git/trees", 201, {{"sha", "tree2"}});
    gh.on("POST /repos/idx/index/git/commits", 201, {{"sha", "commit2"}});
    gh.on("POST /repos/idx/index/git/refs", 201, {{"ref", "refs/heads/publish/p/a-1.0-r1"}});
    gh.on("POST /repos/idx/index/pulls", 201, {{"html_url", "https://github.com/idx/index/pull/7"}});
    auto index = github_index::open("https://github.com/idx/index.git", "secret", gh.transport());
    ASSERT_TRUE(index.has_value()) << index.error().message;
    EXPECT_EQ(index->repository(), "idx/index");
    EXPECT_EQ(gh.calls.front().headers[1], "Authorization: Bearer secret");

    auto pr = index->propose("publish/p/a-1.0-r1", "Publish p/a 1.0-r1", "Body.\n", entry_files);
    ASSERT_TRUE(pr.has_value()) << pr.error().message;
    EXPECT_EQ(pr->url, "https://github.com/idx/index/pull/7");
    EXPECT_FALSE(pr->reused);
    nlohmann::json const tree = gh.body_of("POST /repos/idx/index/git/trees");
    EXPECT_EQ(tree["base_tree"], "tree1");
    ASSERT_EQ(tree["tree"].size(), 2u);
    EXPECT_EQ(tree["tree"][0]["path"], "recipes/p/a/apple/1.0-r1.toml");
    EXPECT_EQ(tree["tree"][0]["content"], "format = 1\n");
    EXPECT_EQ(tree["tree"][0]["mode"], "100644");
    nlohmann::json const commit = gh.body_of("POST /repos/idx/index/git/commits");
    EXPECT_EQ(commit["parents"], nlohmann::json::array({"head1"}));
    EXPECT_EQ(commit["tree"], "tree2");
    EXPECT_EQ(gh.body_of("POST /repos/idx/index/git/refs")["sha"], "commit2");
    nlohmann::json const pull = gh.body_of("POST /repos/idx/index/pulls");
    EXPECT_EQ(pull["head"], "publish/p/a-1.0-r1");
    EXPECT_EQ(pull["base"], "main");
}

TEST(github_index, reuses_the_branch_and_the_pull_request)
{
    fake_github gh;
    gh.repository(true);
    gh.on("GET /repos/idx/index/git/commits/head1", 200, {{"tree", {{"sha", "tree1"}}}});
    gh.on("POST /repos/idx/index/git/trees", 201, {{"sha", "tree2"}});
    gh.on("POST /repos/idx/index/git/commits", 201, {{"sha", "commit3"}});
    gh.on("POST /repos/idx/index/git/refs", 422, {{"message", "Reference already exists"}});
    gh.on("PATCH /repos/idx/index/git/refs/heads/publish/p/a-1.0-r1", 200, {{"ref", "refs/heads/publish/p/a-1.0-r1"}});
    gh.on("POST /repos/idx/index/pulls", 422, {{"message", "Validation Failed"}});
    gh.on("GET /repos/idx/index/pulls?state=open&head=idx:publish/p/a-1.0-r1", 200,
        nlohmann::json::array({{{"html_url", "https://github.com/idx/index/pull/7"}, {"number", 7}}}));
    gh.on("GET /repos/idx/index/issues/7/comments?per_page=100", 200,
        nlohmann::json::array({{{"user", {{"type", "User"}}}, {"body", "mine"}}, {{"user", {{"type", "Bot"}}}, {"body", "first check"}},
            {{"user", {{"type", "Bot"}}}, {"body", "second check"}}}));
    auto index = github_index::open("https://github.com/idx/index", "secret", gh.transport());
    ASSERT_TRUE(index.has_value()) << index.error().message;
    auto pr = index->propose("publish/p/a-1.0-r1", "Publish p/a 1.0-r1", "Body.\n", entry_files);
    ASSERT_TRUE(pr.has_value()) << pr.error().message;
    EXPECT_TRUE(pr->reused);
    EXPECT_EQ(pr->url, "https://github.com/idx/index/pull/7");
    EXPECT_EQ(pr->number, 7);
    EXPECT_TRUE(index->can_push());
    auto comments = index->bot_comments(pr->number);
    ASSERT_TRUE(comments.has_value()) << comments.error().message;
    EXPECT_EQ(*comments, (std::vector<std::string>{"first check", "second check"}));
    nlohmann::json const moved = gh.body_of("PATCH /repos/idx/index/git/refs/heads/publish/p/a-1.0-r1");
    EXPECT_EQ(moved["sha"], "commit3");
    EXPECT_EQ(moved["force"], true);
}

// a commit just made may not be known yet where the ref is created: tried again; another refusal is not taken for a
// branch that exists
TEST(github_index, creates_the_branch_once_the_commit_is_known)
{
    fake_github gh;
    gh.repository(true);
    gh.on("GET /repos/idx/index/git/commits/head1", 200, {{"tree", {{"sha", "tree1"}}}});
    gh.on("POST /repos/idx/index/git/trees", 201, {{"sha", "tree2"}});
    gh.on("POST /repos/idx/index/git/commits", 201, {{"sha", "commit2"}});
    gh.on("POST /repos/idx/index/git/refs", 422, {{"message", "Object does not exist"}});
    gh.on("POST /repos/idx/index/git/refs", 201, {{"ref", "refs/heads/publish/p/a-1.0-r1"}});
    gh.on("POST /repos/idx/index/pulls", 201, {{"html_url", "https://github.com/idx/index/pull/7"}});
    auto index = github_index::open("https://github.com/idx/index", "secret", gh.transport());
    ASSERT_TRUE(index.has_value()) << index.error().message;
    auto pr = index->propose("publish/p/a-1.0-r1", "Publish p/a 1.0-r1", "Body.\n", entry_files);
    ASSERT_TRUE(pr.has_value()) << pr.error().message;
    EXPECT_EQ(pr->url, "https://github.com/idx/index/pull/7");
    EXPECT_EQ(std::ranges::count_if(gh.calls, [](http_exchange const& e) { return e.method == "POST" && e.url.ends_with("/git/refs"); }), 2);

    fake_github refused;
    refused.repository(true);
    refused.on("GET /repos/idx/index/git/commits/head1", 200, {{"tree", {{"sha", "tree1"}}}});
    refused.on("POST /repos/idx/index/git/trees", 201, {{"sha", "tree2"}});
    refused.on("POST /repos/idx/index/git/commits", 201, {{"sha", "commit2"}});
    refused.on("POST /repos/idx/index/git/refs", 422, {{"message", "Reference update failed"}});
    auto other = github_index::open("https://github.com/idx/index", "secret", refused.transport());
    ASSERT_TRUE(other.has_value()) << other.error().message;
    auto failed = other->propose("publish/p/a-1.0-r1", "Publish p/a 1.0-r1", "Body.\n", entry_files);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "GitHub: POST /repos/idx/index/git/refs: 422 Reference update failed");
    EXPECT_FALSE(std::ranges::any_of(refused.calls, [](http_exchange const& e) { return e.method == "PATCH"; }));
}

TEST(github_index, proposes_from_a_fork_without_push_access)
{
    fake_github gh;
    gh.repository(false);
    gh.on("POST /repos/idx/index/forks", 202, {{"full_name", "pub/index"}});
    gh.on("POST /repos/pub/index/merge-upstream", 200, {{"message", "ok"}});
    gh.on("GET /repos/pub/index/git/commits/head1", 200, {{"tree", {{"sha", "tree1"}}}});
    gh.on("POST /repos/pub/index/git/trees", 201, {{"sha", "tree2"}});
    gh.on("POST /repos/pub/index/git/commits", 201, {{"sha", "commit2"}});
    gh.on("POST /repos/pub/index/git/refs", 201, {{"ref", "refs/heads/publish/p/a-1.0-r1"}});
    gh.on("POST /repos/idx/index/pulls", 201, {{"html_url", "https://github.com/idx/index/pull/8"}});
    auto index = github_index::open("https://github.com/idx/index", "secret", gh.transport());
    ASSERT_TRUE(index.has_value()) << index.error().message;
    auto pr = index->propose("publish/p/a-1.0-r1", "Publish p/a 1.0-r1", "Body.\n", entry_files);
    ASSERT_TRUE(pr.has_value()) << pr.error().message;
    EXPECT_EQ(pr->url, "https://github.com/idx/index/pull/8");
    EXPECT_EQ(gh.body_of("POST /repos/idx/index/pulls")["head"], "pub:publish/p/a-1.0-r1");
    EXPECT_EQ(gh.body_of("POST /repos/pub/index/merge-upstream")["branch"], "main");
}

TEST(github_index, reads_files_and_reports_errors)
{
    fake_github gh;
    gh.repository(true);
    gh.replies["GET /repos/idx/index/contents/libraries/a.toml?ref=head1"].push_back(test::http_answer(200, "scheme = \"dotted\"\n"));
    gh.on("GET /user", 200, {{"login", "Pub"}, {"id", 77}});
    auto index = github_index::open("https://github.com/idx/index", "secret", gh.transport());
    ASSERT_TRUE(index.has_value()) << index.error().message;
    auto file = index->file("libraries/a.toml");
    ASSERT_TRUE(file.has_value()) << file.error().message;
    EXPECT_EQ(file->value_or("?"), "scheme = \"dotted\"\n");
    EXPECT_EQ(gh.calls.back().headers[0], "Accept: application/vnd.github.raw+json");
    auto missing = index->file("libraries/zz.toml");
    ASSERT_TRUE(missing.has_value()) << missing.error().message;
    EXPECT_FALSE(missing->has_value());
    EXPECT_EQ(index->login().value_or("?"), "Pub");
    EXPECT_EQ(index->account().value_or(github_account{}).id, 77);

    // the tree is refused: the call and GitHub's message
    gh.on("GET /repos/idx/index/git/commits/head1", 200, {{"tree", {{"sha", "tree1"}}}});
    gh.on("POST /repos/idx/index/git/trees", 422, {{"message", "tree.path contains a malformed path component"}});
    auto pr = index->propose("publish/p/a-1.0-r1", "t", "b", entry_files);
    ASSERT_FALSE(pr.has_value());
    EXPECT_EQ(pr.error().message, "GitHub: POST /repos/idx/index/git/trees: 422 tree.path contains a malformed path component");

    auto unknown = github_index::open("https://github.com/idx/nothing", "secret", gh.transport());
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().message, "GitHub has no repository idx/nothing that this token can read");
    EXPECT_FALSE(github_index::open("https://github.com/idx/index", "", gh.transport()).has_value());
    EXPECT_FALSE(github_index::is_github_url("https://github.com/idx"));
    EXPECT_FALSE(github_index::is_github_url("file:///tmp/index"));
    EXPECT_TRUE(github_index::is_github_url("https://git.example.com/idx/index"));
}

// a 422 without an open pull request of the branch: GitHub's message and the details of its 'errors'
TEST(github_index, reports_the_details_of_a_refused_pull_request)
{
    fake_github gh;
    gh.repository(true);
    gh.on("GET /repos/idx/index/git/commits/head1", 200, {{"tree", {{"sha", "tree1"}}}});
    gh.on("POST /repos/idx/index/git/trees", 201, {{"sha", "tree2"}});
    gh.on("POST /repos/idx/index/git/commits", 201, {{"sha", "commit2"}});
    gh.on("POST /repos/idx/index/git/refs", 201, {{"ref", "refs/heads/publish/p/a-1.0-r1"}});
    gh.on("POST /repos/idx/index/pulls", 422, {{"message", "Validation Failed"},
        {"errors", nlohmann::json::array({{{"resource", "PullRequest"}, {"code", "custom"}, {"message", "title is too long (maximum is 256 characters)"}},
            {{"resource", "Issue"}, {"field", "body"}, {"code", "too_long"}}})}});
    gh.on("GET /repos/idx/index/pulls?state=open&head=idx:publish/p/a-1.0-r1", 200, nlohmann::json::array());
    auto index = github_index::open("https://github.com/idx/index", "secret", gh.transport());
    ASSERT_TRUE(index.has_value()) << index.error().message;
    auto pr = index->propose("publish/p/a-1.0-r1", "t", "b", entry_files);
    ASSERT_FALSE(pr.has_value());
    EXPECT_EQ(pr.error().message, "GitHub: POST /repos/idx/index/pulls: 422 Validation Failed: title is too long (maximum is 256 characters); "
        "Issue body too_long");
}

TEST(github_index, keeps_titles_and_bodies_within_the_limits)
{
    // the publisher once, the entries by variant in the order they come
    std::vector<publication_entry> const mixed{{"android", "a 1.0-r1"}, {"windows", "a 1.0-r1"}, {"android", "b 2.0-r1"}};
    EXPECT_EQ(publication_title("p", mixed, 2), "Publish p — android: a 1.0-r1, b 2.0-r1; windows: a 1.0-r1");
    EXPECT_EQ(publication_title("p", std::vector<publication_entry>{{"apple", "a 1.0-r1"}}, 1), "Publish p — apple: a 1.0-r1");
    // what does not fit GitHub's limit is counted
    std::vector<publication_entry> many;
    for (int i = 0; i < 40; ++i) many.push_back({i % 2 ? "windows" : "android", "lib" + std::to_string(i / 2) + " 1.0-r1"});
    EXPECT_EQ(publication_title("p", many, 20), "Publish p — 40 entries of 20 libraries");
    EXPECT_EQ(publication_title("p", std::vector<publication_entry>{{"apple", std::string(300, 'x')}}, 1), "Publish p — 1 entry of 1 library");
    // the longest that fits: 256 bytes
    std::string const head = "Publish p — apple: ";
    std::string const fits = std::string(github_title_limit - head.size(), 'x');
    EXPECT_EQ(publication_title("p", std::vector<publication_entry>{{"apple", fits}}, 1), head + fits);
    EXPECT_EQ(publication_title("p", std::vector<publication_entry>{{"apple", fits + "x"}}, 1), "Publish p — 1 entry of 1 library");

    std::vector<std::string> const two{"p/a 1.0-r1 (native)", "p/b 2.0-r1 (native)"};
    EXPECT_EQ(publication_body("Head:\n", two, "\n"), "Head:\n\np/a 1.0-r1 (native)\np/b 2.0-r1 (native)\n");
    std::vector<std::string> const long_lines(1000, "- " + std::string(98, 'y'));  // 100 bytes a line
    std::string const body = publication_body("Head:\n", long_lines, "\n\nTail.\n");
    EXPECT_LE(body.size(), github_body_limit);
    EXPECT_TRUE(body.ends_with("\n\nTail.\n")) << body.substr(body.size() - 100);
    std::size_t const kept = static_cast<std::size_t>(std::ranges::count(body, 'y')) / 98;
    EXPECT_LT(kept, 1000u);
    EXPECT_NE(body.find("\n- … and " + std::to_string(1000 - kept) + " more\n\nTail.\n"), std::string::npos);
    EXPECT_GT(body.size() + 101, github_body_limit);  // no room for one more line
}

// index.md 9.2: the open pull requests of the index and the files they add, every page of both listings
TEST(github_index, lists_open_pull_requests_and_what_they_add)
{
    fake_github gh;
    gh.repository(false);
    nlohmann::json page = nlohmann::json::array();
    for (int i = 1; i <= 100; ++i) page.push_back({{"number", i}, {"user", {{"id", 1000 + i}}}, {"head", {{"label", "u:b" + std::to_string(i)}}}});
    gh.on("GET /repos/idx/index/pulls?state=open&per_page=100&page=1", 200, page);
    gh.on("GET /repos/idx/index/pulls?state=open&per_page=100&page=2", 200,
        nlohmann::json::array({{{"number", 101}, {"user", {{"id", 7}}}, {"head", {{"label", "pub:publish/p/a-apple-1.0-r1"}}}}}));
    for (int i = 1; i <= 100; ++i) gh.on("GET /repos/idx/index/pulls/" + std::to_string(i) + "/files?per_page=100&page=1", 200, nlohmann::json::array());
    gh.on("GET /repos/idx/index/pulls/101/files?per_page=100&page=1", 200,
        nlohmann::json::array({{{"filename", "recipes/p/a/apple/1.0-r1.toml"}, {"status", "added"}}, {{"filename", "libraries/a.toml"}, {"status", "modified"}}}));
    auto index = github_index::open("https://github.com/idx/index", "secret", gh.transport());
    ASSERT_TRUE(index.has_value()) << index.error().message;
    auto open = index->open_pull_requests();
    ASSERT_TRUE(open.has_value()) << open.error().message;
    ASSERT_EQ(open->size(), 101u);
    open_pull_request const& last = open->back();
    EXPECT_EQ(last.number, 101);
    EXPECT_EQ(last.author_id, 7);
    EXPECT_EQ(last.head, "pub:publish/p/a-apple-1.0-r1");
    EXPECT_EQ(last.added, std::vector<std::string>{"recipes/p/a/apple/1.0-r1.toml"});

    fake_github broken;
    broken.repository(false);
    auto other = github_index::open("https://github.com/idx/index", "secret", broken.transport());
    ASSERT_TRUE(other.has_value());
    auto failed = other->open_pull_requests();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message, "GitHub: GET /repos/idx/index/pulls?state=open&per_page=100&page=1: 404 Not Found");
}

// a working tree (docs/PLAN.md decision 146): the recipe by its absolute path, outside the publication's project
TEST(publish_project, builds_a_working_tree_by_path)
{
    recipe const r = parsed_recipe(recipe_text());
    locked_location const tree{.type = locked_location::kind::path, .path = "C:/recipes/a"};
    auto project = make_publish_project(r, tree, target::apple::info(), *parse_environment("macos/xcode"), nullptr);
    ASSERT_TRUE(project.has_value()) << project.error().message;
    EXPECT_NE(project->manifest.find("recipe = { path = \"C:/recipes/a\" }"), std::string::npos) << project->manifest;
    diagnostics d;
    auto m = parse_manifest(project->manifest, "otok.toml", targets, {}, d);
    ASSERT_TRUE(m.has_value()) << messages(d) << project->manifest;
    EXPECT_EQ(m->deps[0].recipe.source, recipe_ref::kind::path);
    EXPECT_EQ(m->deps[0].recipe.path, "C:/recipes/a");
}

// The spellings of a version otok bump replaces (docs/PLAN.md decision 146).
TEST(bump, spells_the_version_as_urls_do)
{
    auto const spellings = version_spellings("1.92.0", "1.93.1");
    std::vector<std::string> from;
    for (version_spelling const& s : spellings) from.push_back(s.from + ">" + s.to);
    EXPECT_EQ(from, (std::vector<std::string>{"1.92.0>1.93.1", "1_92_0>1_93_1", "1-92-0>1-93-1", "1.92>1.93", "1_92>1_93"}));
    // within the line, the line stays
    EXPECT_EQ(version_spellings("4.4.5", "4.4.7").size(), 3u);
    EXPECT_EQ(version_spellings("2175", "2190").size(), 1u);

    auto const replaced = [](std::string_view text, std::string_view from, std::string_view to) {
        return replace_version(text, version_spellings(from, to)).value_or("<none>");
    };
    EXPECT_EQ(replaced("https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2", "1.92.0", "1.93.0"),
        "https://archives.boost.io/release/1.93.0/source/boost_1_93_0.tar.bz2");
    EXPECT_EQ(replaced("https://download.gnome.org/sources/glib/2.84/glib-2.84.1.tar.xz", "2.84.1", "2.86.0"),
        "https://download.gnome.org/sources/glib/2.86/glib-2.86.0.tar.xz");
    EXPECT_EQ(replaced("https://github.com/unicode-org/icu/releases/download/release-79-1/icu4c-79_1-src.tgz", "79.1", "80.1"),
        "https://github.com/unicode-org/icu/releases/download/release-80-1/icu4c-80_1-src.tgz");
    EXPECT_EQ(replaced("https://download.gnome.org/sources/libxml2/2.13/libxml2-2.13.5.tar.xz", "2.13.5", "2.14.0"),
        "https://download.gnome.org/sources/libxml2/2.14/libxml2-2.14.0.tar.xz");
    EXPECT_EQ(replaced("v1.2.1", "1.2.1", "1.2.2"), "v1.2.2");
    // alone only: not within another version or number
    EXPECT_EQ(replaced("x-1.2.10.tar.gz", "1.2.1", "1.2.2"), "<none>");
    EXPECT_EQ(replaced("x-11.2.1.tar.gz", "1.2.1", "1.2.2"), "<none>");
    EXPECT_EQ(replaced("x-0.1.2.1.tar.gz", "1.2.1", "1.2.2"), "<none>");
    EXPECT_EQ(replaced("https://example.com/data.zip", "1.2.1", "1.2.2"), "<none>");

    EXPECT_TRUE(mentions_version_digits("https://example.com/x-1920.zip", "1.92.0"));
    EXPECT_FALSE(mentions_version_digits("https://example.com/x-19201.zip", "1.92.0"));
    EXPECT_FALSE(mentions_version_digits("https://example.com/x-2175.zip", "2175"));
}

namespace {

// fetchers answering from maps: sha256 by URL, commits by tag
bump_fetchers fake_fetchers(std::map<std::string, std::string> files, std::map<std::string, std::string> tags)
{
    bump_fetchers f;
    f.archive = [files](std::string const& url) -> result<std::string> {
        auto it = files.find(url);
        if (it == files.end()) return fail("HTTP 404");
        return it->second;
    };
    f.mirror = f.archive;
    f.tag_commit = [tags](std::string const&, std::string const& tag) -> result<std::string> {
        auto it = tags.find(tag);
        if (it == tags.end()) return fail("the repository has no such tag");
        return it->second;
    };
    return f;
}

}

TEST(bump, rewrites_only_the_values_it_changes)
{
    std::string const sha(64, 'a'), new_sha(64, 'b'), commit(40, '1'), new_commit(40, '2');
    // CRLF, comments, a non-ASCII comment before a value on its line
    std::string const old_text =
        "[recipe]  # head\r\nversion = \"1.0.0\"\r\nrevision = 7 # r\r\n"
        "[source]\r\ngit = \"https://example.com/a.git\"\r\ncommit = \"" + commit + "\"\r\ntag = \"v1.0.0\"\r\n"
        "verify-against = { git = \"https://example.com/a.git\", tag = \"v1.0.0\" }\r\n"
        "[[resource]]\r\nname = \"doc\" # \xc3\xa9\r\nurl =\"https://example.com/doc-1.0.0.zip\"  # \xc3\xa9\r\n"
        "sha256 = \"" + sha + "\"\r\n";
    std::string const fixed = old_text;
    diagnostics d;
    auto r = bump_recipe_text(fixed, "recipe.toml", "1.1.0", fake_fetchers({{"https://example.com/doc-1.1.0.zip", new_sha}}, {{"v1.1.0", new_commit}}), d);
    ASSERT_TRUE(r.has_value()) << messages(d);
    EXPECT_FALSE(d.has_errors()) << messages(d);
    std::string expected = fixed;
    expected = replace_in(expected, "\"1.0.0\"", "\"1.1.0\"");
    expected = replace_in(expected, "revision = 7", "revision = 1");
    expected = replace_in(expected, commit, new_commit);
    expected = replace_in(expected, "tag = \"v1.0.0\"", "tag = \"v1.1.0\"");
    expected = replace_in(expected, "tag = \"v1.0.0\"", "tag = \"v1.1.0\"");
    expected = replace_in(expected, "doc-1.0.0", "doc-1.1.0");
    expected = replace_in(expected, sha, new_sha);
    EXPECT_EQ(r->text, expected);
    ASSERT_EQ(r->inputs.size(), 3u);
    EXPECT_EQ(r->inputs[0].name, "source");
    EXPECT_EQ(r->inputs[0].after.commit, new_commit);
    EXPECT_EQ(r->inputs[1].name, "source.verify-against");
    EXPECT_EQ(r->inputs[1].status, bump_status::changed);
    EXPECT_EQ(r->inputs[2].name, "doc");
    EXPECT_EQ(r->old_revision, 7);

    // literal strings stay literal
    std::string const literal = "[recipe]\nversion = '1.0'\nrevision = 1\n[source]\nurl = 'https://example.com/a-1.0.zip'\nsha256 = '" + sha + "'\n";
    d = {};
    r = bump_recipe_text(literal, "recipe.toml", "1.1", fake_fetchers({{"https://example.com/a-1.1.zip", new_sha}}, {}), d);
    ASSERT_TRUE(r.has_value()) << messages(d);
    EXPECT_EQ(r->text, "[recipe]\nversion = '1.1'\nrevision = 1\n[source]\nurl = 'https://example.com/a-1.1.zip'\nsha256 = '" + new_sha + "'\n");

    // a value after non-ASCII text on its line: toml++ counts code points
    std::string const inline_source = "source = { note = \"\xc3\xa9\xc3\xa9\", url = \"https://example.com/a-1.0.zip\", sha256 = \""
        + sha + "\" }\n[recipe]\nversion = \"1.0\"\nrevision = 1\n";
    d = {};
    r = bump_recipe_text(inline_source, "recipe.toml", "1.1", fake_fetchers({{"https://example.com/a-1.1.zip", new_sha}}, {}), d);
    ASSERT_TRUE(r.has_value()) << messages(d);
    EXPECT_EQ(r->text, replace_in(replace_in(replace_in(inline_source, "\"1.0\"", "\"1.1\""), "a-1.0", "a-1.1"), sha, new_sha)) << messages(d);
}

TEST(bump, says_what_it_cannot_bump)
{
    std::string const sha(64, 'a');
    auto const bumped = [&](std::string const& source_table, std::map<std::string, std::string> files, std::map<std::string, std::string> tags,
                            diagnostics& d) {
        return bump_recipe_text("[recipe]\nversion = \"1.92.0\"\nrevision = 1\n[source]\n" + source_table, "recipe.toml", "1.93.0",
            fake_fetchers(std::move(files), std::move(tags)), d);
    };
    // a commit without a tag
    diagnostics d;
    auto r = bumped("git = \"https://example.com/a.git\"\ncommit = \"" + std::string(40, '1') + "\"\n", {}, {}, d);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->inputs[0].status, bump_status::not_bumped);
    EXPECT_TRUE(r->text.empty());
    EXPECT_EQ(messages(d), "recipe.toml:4:1: error: the source does not follow the version: a git commit without a tag: otok does not know "
                           "the commit of version 1.93.0; bump the recipe by hand\n");
    // the version in a spelling otok does not replace
    d = {};
    r = bumped("url = \"https://example.com/a-1920.zip\"\nsha256 = \"" + sha + "\"\n", {}, {}, d);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->inputs[0].status, bump_status::not_bumped);
    EXPECT_NE(messages(d).find("its URL holds the version 1.92.0 in a way otok does not replace: https://example.com/a-1920.zip"), std::string::npos)
        << messages(d);
    // a URL without the version
    d = {};
    r = bumped("url = \"https://example.com/a.zip\"\nsha256 = \"" + sha + "\"\n", {}, {}, d);
    EXPECT_NE(messages(d).find("the source does not follow the version: its URL does not hold the version 1.92.0"), std::string::npos) << messages(d);
    // no such tag
    d = {};
    r = bumped("git = \"https://example.com/a.git\"\ncommit = \"" + std::string(40, '1') + "\"\ntag = \"boost-1.92.0\"\n", {}, {}, d);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->inputs[0].status, bump_status::failed);
    EXPECT_NE(messages(d).find("recipe.toml:7:7: error: the source of version 1.93.0: cannot find the tag boost-1.93.0 in https://example.com/a.git: "
                               "the repository has no such tag"), std::string::npos) << messages(d);
    // a mirror giving another file is an error, one that gives none a warning
    std::string const other(64, 'c');
    d = {};
    r = bumped("url = \"https://a.example/a-1.92.0.zip\"\nsha256 = \"" + sha + "\"\nmirrors = [\"https://b.example/a-1.92.0.zip\"]\n",
        {{"https://a.example/a-1.93.0.zip", sha}, {"https://b.example/a-1.93.0.zip", other}}, {}, d);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->inputs[0].status, bump_status::failed);
    EXPECT_NE(messages(d).find("error: the source of version 1.93.0: the mirror https://b.example/a-1.93.0.zip gives another file"), std::string::npos)
        << messages(d);
    d = {};
    r = bumped("url = \"https://a.example/a-1.92.0.zip\"\nsha256 = \"" + sha + "\"\nmirrors = [\"https://b.example/a-1.92.0.zip\"]\n",
        {{"https://a.example/a-1.93.0.zip", sha}}, {}, d);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(d.has_errors()) << messages(d);
    EXPECT_FALSE(r->text.empty());
    EXPECT_NE(messages(d).find("warning: the mirror https://b.example/a-1.93.0.zip of the source does not give the file of version 1.93.0 (HTTP 404)"),
        std::string::npos) << messages(d);
}
