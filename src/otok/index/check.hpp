// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "otok/cache/store.hpp"
#include "otok/core/target_info.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/publish/github.hpp"

// The checks of index CI (index.md 9) and the probe of the inputs of published versions (index.md 11),
// docs/PLAN.md decision 47.
namespace otok {

// What a GitHub account the checks ask about: through the REST API.
struct github_access
{
    http_transport transport;          // empty: no API; the checks that need it fail
    std::string api = "https://api.github.com";
    std::string token;
};

struct index_check_settings
{
    std::filesystem::path root;        // a clean checkout of the index, at the commit checked
    std::string base;                  // the commit it is compared with
    std::optional<std::int64_t> author_id;  // the pull request's author
    std::string repository;            // the index's on GitHub, "owner/name": its maintainers have no limit; empty: nobody is one
    std::optional<std::int64_t> pull_request;  // the number of the one checked, left out of the open ones
    std::optional<std::int64_t> approved_by;   // the account that approved it: a maintainer's approval lets the workflow merge it
    bool stamp = false;                // with the verdict auto, `published` written into the new entries of the checkout (index.md 3)
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::span<target_info const* const> targets;
    cache_store* cache = nullptr;
    git_tool const* git = nullptr;
    http_settings http;                // downloads of the inputs
    github_access github;
    bool https_only = true;            // the locations of new entries (index.md 9); tests use file://
};

enum class index_verdict
{
    automatic,                         // "auto": the workflow merges it
    maintainer,                        // valid, but waits for an index maintainer
    refused
};

std::string_view verdict_name(index_verdict v) noexcept;

struct index_check_report
{
    index_verdict verdict = index_verdict::automatic;
    std::vector<std::string> errors;   // why it is refused
    std::vector<std::string> reasons;  // why it waits for a maintainer
    std::vector<std::string> notes;    // listed, deciding nothing (verify-against)
    std::size_t entries = 0;           // new version entries checked
    std::vector<std::string> stamped;  // the entries given `published` (index_check_settings::stamp), paths in the checkout
};

// Checks the changes of the checkout since `base` (index.md 9). A failure only when the check itself
// cannot run (not a checkout, no such commit, an entry that cannot be stamped); problems of the changes are in the report.
result<index_check_report> check_index(index_check_settings const& settings);

// The summary `--summary` writes (index.md 9): JSON.
std::string summary_json(index_check_report const& report);

// The comment for the pull request `--comment` writes (index.md 9): Markdown, ending with the summary.
std::string check_comment(index_check_report const& report);

// The summary in a comment `check_comment` wrote, as JSON text; nothing when it holds none.
std::optional<std::string> summary_of_comment(std::string_view comment);

struct index_probe_settings
{
    std::filesystem::path root;        // a checkout of the index
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::span<target_info const* const> targets;
    cache_store* cache = nullptr;      // its tmp/ holds the repository shallow fetches go to
    git_tool const* git = nullptr;
    http_transport http;               // HEAD and ranged GET requests
};

struct index_probe_result
{
    std::size_t versions = 0;          // versions probed
    std::string availability;          // the new availability.toml
    std::size_t unavailable = 0;
};

// Probes the inputs of every version of the index (index.md 11); `since` is kept from the checkout's
// availability.toml for versions still missing something.
result<index_probe_result> probe_index(index_probe_settings const& settings);

}
