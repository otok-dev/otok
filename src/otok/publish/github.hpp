// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "otok/core/error.hpp"
#include "otok/fetch/http.hpp"

// An index on GitHub, published to through pull requests made with its REST API, without a clone
// (DESIGN 7.4, docs/PLAN.md decision 46).
namespace otok {

struct github_file
{
    std::string path;
    std::string content;
};

struct github_account
{
    std::string login;
    std::int64_t id = 0;               // numeric, never reused (index.md 6)
};

struct pull_request
{
    std::string url;                   // its page
    std::int64_t number = 0;
    bool reused = false;               // an open pull request of the branch, updated
};

// A GET of the GitHub REST API at `api` ("https://api.github.com") with `token` (empty: none), whatever its
// status.
result<http_reply> github_get(http_transport const& transport, std::string const& api, std::string const& token, std::string const& path);

// An open pull request of a repository and the files it adds (index.md 9.2).
struct open_pull_request
{
    std::int64_t number = 0;
    std::int64_t author_id = 0;        // the numeric id of the account that opened it
    std::string head;                  // "<owner>:<branch>"
    std::vector<std::string> added;    // paths of the files it adds
};

// The open pull requests of `repository` ("owner/name") and the files they add, through the REST API at `api`.
result<std::vector<open_pull_request>> open_pull_requests(http_transport const& transport, std::string const& api, std::string const& token,
    std::string const& repository);

class github_index
{
public:
    // The index repository at `url` (`https://github.com/<owner>/<repo>`, or another GitHub host, whose API
    // is `<host>/api/v3`), with the token `token`. Reads the repository, its default branch and its head.
    static result<github_index> open(std::string const& url, std::string token, http_transport transport);

    // Whether `url` names a repository on a GitHub host this otok can publish to.
    static bool is_github_url(std::string const& url);

    // The account of the token.
    result<github_account> account();
    result<std::string> login();

    // Whether the token may push to the index: its account maintains it (index.md 9).
    bool can_push() const noexcept { return can_push_; }

    // A file at the head of the default branch; nothing when it is missing.
    result<std::optional<std::string>> file(std::string const& path);

    // Commits `files` on top of the head as the branch `branch` (forced) of the index when the token may
    // push to it, else of the account's fork (created when missing), and opens a pull request of it.
    result<pull_request> propose(std::string const& branch, std::string const& title, std::string const& body,
        std::span<github_file const> files);

    // The open pull requests of the index and the files they add.
    result<std::vector<open_pull_request>> open_pull_requests() const { return otok::open_pull_requests(transport_, api_, token_, repository_); }

    // The comments of bots (index CI) on the pull request `number`, the oldest first.
    result<std::vector<std::string>> bot_comments(std::int64_t number);

    std::string const& repository() const noexcept { return repository_; }

private:
    github_index(std::string api, std::string repository, std::string token, http_transport transport)
        : api_{std::move(api)}, repository_{std::move(repository)}, token_{std::move(token)}, transport_{std::move(transport)} {}

    // A call of the API, whatever its status; `raw` asks for a file's bytes rather than JSON.
    result<http_reply> call(std::string const& method, std::string const& path, std::string const& body = {}, bool raw = false);

    std::string api_;                  // "https://api.github.com"
    std::string repository_;           // "owner/repo"
    std::string token_;
    http_transport transport_;
    std::string default_branch_;
    std::string head_;                 // its commit
    bool can_push_ = false;
};

}
