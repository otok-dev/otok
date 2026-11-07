// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/publish/github.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

namespace otok {

namespace {

using json = nlohmann::json;

bool success(http_reply const& r) noexcept
{
    return r.status >= 200 && r.status < 300;
}

// "GitHub: POST /repos/o/r/pulls: 422 Validation Failed"
error api_error(std::string const& method, std::string const& path, http_reply const& r)
{
    std::string message;
    auto const body = json::parse(r.body, nullptr, false);
    if (body.is_object() && body.contains("message") && body["message"].is_string()) message = " " + body["message"].get<std::string>();
    return error{"GitHub: " + method + " " + path + ": " + std::to_string(r.status) + message};
}

result<json> parsed(std::string const& method, std::string const& path, http_reply const& r)
{
    if (!success(r)) return std::unexpected{api_error(method, path, r)};
    auto body = json::parse(r.body, nullptr, false);
    if (body.is_discarded()) return fail("GitHub: " + method + " " + path + ": the reply is not JSON");
    return body;
}

// A string member of a JSON object, or an error naming it.
result<std::string> member(json const& object, std::string_view path, std::string const& what)
{
    json const* at = &object;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t const dot = std::min(path.find('.', start), path.size());
        std::string const key{path.substr(start, dot - start)};
        if (!at->is_object() || !at->contains(key)) return fail("GitHub: the reply to " + what + " has no '" + std::string{path} + "'");
        at = &(*at)[key];
        start = dot + 1;
    }
    if (!at->is_string()) return fail("GitHub: '" + std::string{path} + "' in the reply to " + what + " is not a string");
    return at->get<std::string>();
}

}

result<http_reply> github_get(http_transport const& transport, std::string const& api, std::string const& token, std::string const& path)
{
    http_exchange e;
    e.url = api + path;
    e.headers = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"};
    if (!token.empty()) e.headers.push_back("Authorization: Bearer " + token);
    return transport(e);
}

result<std::vector<open_pull_request>> open_pull_requests(http_transport const& transport, std::string const& api, std::string const& token,
    std::string const& repository)
{
    // every page of a listing, 100 a page
    auto const pages = [&](std::string const& path) -> result<std::vector<json>> {
        std::vector<json> items;
        for (int page = 1;; ++page) {
            std::string const what = path + (path.find('?') == std::string::npos ? "?" : "&") + "per_page=100&page=" + std::to_string(page);
            auto reply = github_get(transport, api, token, what);
            if (!reply) return std::unexpected{reply.error()};
            auto list = parsed("GET", what, *reply);
            if (!list) return std::unexpected{list.error()};
            if (!list->is_array()) return fail("GitHub: GET " + what + ": the reply is not a list");
            for (json& item : *list) items.push_back(std::move(item));
            if (list->size() < 100) return items;
        }
    };
    std::string const pulls = "/repos/" + repository + "/pulls";
    auto listed = pages(pulls + "?state=open");
    if (!listed) return std::unexpected{listed.error()};
    std::vector<open_pull_request> out;
    for (json const& p : *listed) {
        if (!p.is_object()) continue;
        open_pull_request r;
        r.number = p.value("number", std::int64_t{0});
        if (p.contains("user") && p["user"].is_object()) r.author_id = p["user"].value("id", std::int64_t{0});
        if (p.contains("head") && p["head"].is_object()) r.head = p["head"].value("label", "");
        auto files = pages(pulls + "/" + std::to_string(r.number) + "/files");
        if (!files) return std::unexpected{files.error()};
        for (json const& f : *files) {
            if (f.is_object() && f.value("status", "") == "added" && f.contains("filename") && f["filename"].is_string()) {
                r.added.push_back(f["filename"].get<std::string>());
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

bool github_index::is_github_url(std::string const& url)
{
    if (!url.starts_with("https://")) return false;
    std::string_view rest{url};
    rest.remove_prefix(8);
    std::size_t const slash = rest.find('/');
    return slash != std::string_view::npos && slash > 0 && rest.substr(slash + 1).find('/') != std::string_view::npos;
}

result<github_index> github_index::open(std::string const& url, std::string token, http_transport transport)
{
    if (!is_github_url(url)) return fail("'" + url + "' is not the https URL of a repository on GitHub");
    std::string rest = url.substr(8);
    while (rest.ends_with('/')) rest.pop_back();
    if (rest.ends_with(".git")) rest.resize(rest.size() - 4);
    std::size_t const slash = rest.find('/');
    std::string const host = rest.substr(0, slash);
    std::string const repository = rest.substr(slash + 1);
    if (repository.find('/') == std::string::npos || repository.find('/') != repository.rfind('/')) {
        return fail("'" + url + "' is not the URL of a repository (https://<host>/<owner>/<repository>)");
    }
    std::string const api = host == "github.com" ? "https://api.github.com" : "https://" + host + "/api/v3";
    if (token.empty()) return fail("publishing to GitHub needs a token: set GITHUB_TOKEN, or sign in with 'gh auth login'");
    github_index index{api, repository, std::move(token), std::move(transport)};

    std::string const what = "/repos/" + repository;
    auto reply = index.call("GET", what);
    if (!reply) return std::unexpected{reply.error()};
    if (reply->status == 404) return fail("GitHub has no repository " + repository + " that this token can read");
    auto repo = parsed("GET", what, *reply);
    if (!repo) return std::unexpected{repo.error()};
    auto branch = member(*repo, "default_branch", what);
    if (!branch) return std::unexpected{branch.error()};
    index.default_branch_ = std::move(*branch);
    if (repo->contains("permissions") && (*repo)["permissions"].is_object()) index.can_push_ = (*repo)["permissions"].value("push", false);

    std::string const ref = what + "/git/ref/heads/" + index.default_branch_;
    auto head_reply = index.call("GET", ref);
    if (!head_reply) return std::unexpected{head_reply.error()};
    auto head = parsed("GET", ref, *head_reply);
    if (!head) return fail("the index " + repository + " has no commit on its default branch: " + head.error().message);
    auto sha = member(*head, "object.sha", ref);
    if (!sha) return std::unexpected{sha.error()};
    index.head_ = std::move(*sha);
    return index;
}

result<http_reply> github_index::call(std::string const& method, std::string const& path, std::string const& body, bool raw)
{
    http_exchange e;
    e.method = method;
    e.url = api_ + path;
    e.headers = {std::string{"Accept: "} + (raw ? "application/vnd.github.raw+json" : "application/vnd.github+json"),
        "Authorization: Bearer " + token_, "X-GitHub-Api-Version: 2022-11-28"};
    if (!body.empty()) e.headers.push_back("Content-Type: application/json");
    e.body = body;
    return transport_(e);
}

result<github_account> github_index::account()
{
    auto reply = call("GET", "/user");
    if (!reply) return std::unexpected{reply.error()};
    auto user = parsed("GET", "/user", *reply);
    if (!user) return std::unexpected{user.error()};
    auto login = member(*user, "login", "/user");
    if (!login) return std::unexpected{login.error()};
    if (!user->contains("id") || !(*user)["id"].is_number_integer()) return fail("GitHub: the reply to /user has no numeric 'id'");
    return github_account{std::move(*login), (*user)["id"].get<std::int64_t>()};
}

result<std::string> github_index::login()
{
    auto a = account();
    if (!a) return std::unexpected{a.error()};
    return std::move(a->login);
}

result<std::optional<std::string>> github_index::file(std::string const& path)
{
    std::string const what = "/repos/" + repository_ + "/contents/" + path + "?ref=" + head_;
    auto reply = call("GET", what, {}, true);
    if (!reply) return std::unexpected{reply.error()};
    if (reply->status == 404) return std::optional<std::string>{};
    if (!success(*reply)) return std::unexpected{api_error("GET", what, *reply)};
    return std::optional<std::string>{std::move(reply->body)};
}

result<pull_request> github_index::propose(std::string const& branch, std::string const& title, std::string const& body,
    std::span<github_file const> files)
{
    // a step: the call, its JSON, an error naming it
    auto const step = [this](std::string const& method, std::string const& path, json const& payload = nullptr) -> result<json> {
        auto reply = call(method, path, payload.is_null() ? std::string{} : payload.dump());
        if (!reply) return std::unexpected{reply.error()};
        return parsed(method, path, *reply);
    };

    std::string head_repository = repository_;
    std::string head_owner = repository_.substr(0, repository_.find('/'));
    if (!can_push_) {
        // the account's fork: created when missing (asynchronously), else brought up to the index's head
        auto fork = step("POST", "/repos/" + repository_ + "/forks", json{{"default_branch_only", true}});
        if (!fork) return std::unexpected{fork.error()};
        auto name = member(*fork, "full_name", "the fork");
        if (!name) return std::unexpected{name.error()};
        head_repository = std::move(*name);
        head_owner = head_repository.substr(0, head_repository.find('/'));
        (void)call("POST", "/repos/" + head_repository + "/merge-upstream", json{{"branch", default_branch_}}.dump());
        std::string const probe = "/repos/" + head_repository + "/git/commits/" + head_;
        for (int attempt = 0;; ++attempt) {
            auto reply = call("GET", probe);
            if (!reply) return std::unexpected{reply.error()};
            if (success(*reply)) break;
            if (attempt == 30) return fail("GitHub: the fork " + head_repository + " does not have the index's head " + head_ + " yet; try again later");
            std::this_thread::sleep_for(std::chrono::seconds{2});
        }
    }
    std::string const base = "/repos/" + head_repository + "/git";
    auto commit = step("GET", base + "/commits/" + head_);
    if (!commit) return std::unexpected{commit.error()};
    auto base_tree = member(*commit, "tree.sha", "the head commit");
    if (!base_tree) return std::unexpected{base_tree.error()};

    json entries = json::array();
    for (github_file const& f : files) entries.push_back({{"path", f.path}, {"mode", "100644"}, {"type", "blob"}, {"content", f.content}});
    auto tree = step("POST", base + "/trees", json{{"base_tree", *base_tree}, {"tree", entries}});
    if (!tree) return std::unexpected{tree.error()};
    auto tree_sha = member(*tree, "sha", "the new tree");
    if (!tree_sha) return std::unexpected{tree_sha.error()};
    auto made = step("POST", base + "/commits", json{{"message", title + "\n\n" + body}, {"tree", *tree_sha}, {"parents", json::array({head_})}});
    if (!made) return std::unexpected{made.error()};
    auto made_sha = member(*made, "sha", "the new commit");
    if (!made_sha) return std::unexpected{made_sha.error()};

    // the branch: created, or moved to the new commit when an earlier attempt made it. A commit just made may not be
    // known yet to the part of GitHub that creates the ref ("Object does not exist"): tried again a few times.
    for (int attempt = 0;; ++attempt) {
        auto created = call("POST", base + "/refs", json{{"ref", "refs/heads/" + branch}, {"sha", *made_sha}}.dump());
        if (!created) return std::unexpected{created.error()};
        if (success(*created)) break;
        error const refused = api_error("POST", base + "/refs", *created);
        if (created->status == 422 && refused.message.find("already exists") != std::string::npos) {
            auto moved = step("PATCH", base + "/refs/heads/" + branch, json{{"sha", *made_sha}, {"force", true}});
            if (!moved) return std::unexpected{moved.error()};
            break;
        }
        if (created->status != 422 || refused.message.find("Object does not exist") == std::string::npos || attempt == 5) {
            return std::unexpected{refused};
        }
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }

    std::string const pulls = "/repos/" + repository_ + "/pulls";
    std::string const head = head_repository == repository_ ? branch : head_owner + ":" + branch;
    auto opened = call("POST", pulls, json{{"title", title}, {"head", head}, {"base", default_branch_}, {"body", body}}.dump());
    if (!opened) return std::unexpected{opened.error()};
    if (opened->status == 422) {
        // one is open for the branch already: it shows the new commit
        auto list = step("GET", pulls + "?state=open&head=" + head_owner + ":" + branch);
        if (!list) return std::unexpected{list.error()};
        if (!list->is_array() || list->empty()) return std::unexpected{api_error("POST", pulls, *opened)};
        auto url = member(list->front(), "html_url", "the pull requests of the branch");
        if (!url) return std::unexpected{url.error()};
        return pull_request{std::move(*url), list->front().value("number", std::int64_t{0}), true};
    }
    auto pr = parsed("POST", pulls, *opened);
    if (!pr) return std::unexpected{pr.error()};
    auto url = member(*pr, "html_url", "the new pull request");
    if (!url) return std::unexpected{url.error()};
    return pull_request{std::move(*url), pr->value("number", std::int64_t{0}), false};
}

result<std::vector<std::string>> github_index::bot_comments(std::int64_t number)
{
    std::string const what = "/repos/" + repository_ + "/issues/" + std::to_string(number) + "/comments?per_page=100";
    auto reply = call("GET", what);
    if (!reply) return std::unexpected{reply.error()};
    auto list = parsed("GET", what, *reply);
    if (!list) return std::unexpected{list.error()};
    std::vector<std::string> bodies;
    if (!list->is_array()) return bodies;
    for (json const& c : *list) {
        if (!c.is_object() || !c.contains("user") || !c["user"].is_object() || c["user"].value("type", "") != "Bot") continue;
        if (c.contains("body") && c["body"].is_string()) bodies.push_back(c["body"].get<std::string>());
    }
    return bodies;
}

}
