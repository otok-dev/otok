// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "otok/core/error.hpp"

// Downloads over libcurl (docs/PLAN.md decisions 10, 14): several at a time through the multi
// interface in one thread, the SHA-256 computed while the bytes arrive.
namespace otok {

struct http_settings
{
    std::string proxy;               // empty: libcurl's own (https_proxy, HTTPS_PROXY, …)
    std::string user_agent = "otok";
    long connect_timeout = 30;       // seconds
    long stall_timeout = 60;         // seconds below 1 byte/s before giving up
    std::size_t max_parallel = 8;
};

struct download_request
{
    std::string url;                 // https, http or file
    std::filesystem::path file;      // written from scratch; removed again on failure
    std::string sha256;              // expected (lowercase hex); empty: not checked
};

struct downloaded
{
    std::string sha256;
    std::uint64_t size = 0;
};

// Downloads every request; one result per request, in the same order. A hash mismatch is an error.
std::vector<result<downloaded>> download_all(std::span<download_request const> requests, http_settings const& settings);

result<downloaded> download_file(download_request const& request, http_settings const& settings);

// One request whose reply is returned whatever its status (web APIs: docs/PLAN.md decision 46); only a failed
// transfer is an error. `headers` are "Name: value" lines.
struct http_exchange
{
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    std::size_t max_body = static_cast<std::size_t>(-1);  // a longer reply stops there: truncated
};

struct http_reply
{
    long status = 0;
    std::string body;
    bool truncated = false;
    std::vector<std::pair<std::string, std::string>> headers;  // of the last response; names lowercase

    // The value of a header, or null; names compare without case.
    std::string const* header(std::string_view name) const noexcept;
};

result<http_reply> http_call(http_exchange const& request, http_settings const& settings);

// What makes requests: http_call, or a fake in tests (command_context::http).
using http_transport = std::function<result<http_reply>(http_exchange const&)>;

// `file:///C:/dir/a%20b.tar.gz` for a local path (tests, `file://` sources of local recipes).
std::string file_url(std::filesystem::path const& path);

}
