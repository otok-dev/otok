// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/fetch/http.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include <curl/curl.h>

#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"

namespace otok {

namespace {

void global_init()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// The CA bundle and directory of the system for every handle, found once (decision 104); nothing elsewhere than on
// Linux, where libcurl uses the system's store (Schannel, Secure Transport).
void set_ca(CURL* h)
{
#if defined(__linux__)
    static std::string const bundle = path_to_utf8(find_ca_bundle(std::getenv("SSL_CERT_FILE"), known_ca_bundles()));
    static char const* const dir = std::getenv("SSL_CERT_DIR");
    if (!bundle.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, bundle.c_str());
    if (dir && *dir) curl_easy_setopt(h, CURLOPT_CAPATH, dir);
#else
    (void)h;
#endif
}

struct easy_deleter
{
    void operator()(CURL* h) const noexcept { curl_easy_cleanup(h); }
};

struct multi_deleter
{
    void operator()(CURLM* h) const noexcept { curl_multi_cleanup(h); }
};

// One download in flight.
struct transfer
{
    download_request const* request = nullptr;
    std::unique_ptr<CURL, easy_deleter> easy;
    std::ofstream out;
    sha256_hasher hasher;
    std::uint64_t size = 0;
    bool write_failed = false;
    std::array<char, CURL_ERROR_SIZE> error{};
};

std::size_t on_data(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& t = *static_cast<transfer*>(user);
    std::size_t const n = size * count;
    t.out.write(data, static_cast<std::streamsize>(n));
    if (!t.out) {
        t.write_failed = true;
        return 0;  // makes libcurl stop with CURLE_WRITE_ERROR
    }
    t.hasher.update({data, n});
    t.size += n;
    return n;
}

result<void> start(transfer& t, http_settings const& settings)
{
    t.out.open(t.request->file, std::ios::binary | std::ios::trunc);
    if (!t.out) return fail("cannot write '" + path_to_utf8(t.request->file) + "'");
    t.easy.reset(curl_easy_init());
    if (!t.easy) return fail("cannot start a download");
    CURL* h = t.easy.get();
    curl_easy_setopt(h, CURLOPT_URL, t.request->url.c_str());
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https,http,file");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, settings.connect_timeout);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, settings.stall_timeout);
    curl_easy_setopt(h, CURLOPT_USERAGENT, settings.user_agent.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (!settings.proxy.empty()) curl_easy_setopt(h, CURLOPT_PROXY, settings.proxy.c_str());
    set_ca(h);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, t.error.data());
    curl_easy_setopt(h, CURLOPT_PRIVATE, &t);
    return {};
}

result<downloaded> finish(transfer& t, CURLcode code)
{
    t.out.close();
    auto const failed = [&](std::string message) -> result<downloaded> {
        std::error_code ec;
        std::filesystem::remove(t.request->file, ec);
        return fail(std::move(message));
    };
    std::string const& url = t.request->url;
    if (t.write_failed || (code == CURLE_OK && !t.out)) return failed("cannot write '" + path_to_utf8(t.request->file) + "'");
    if (code == CURLE_HTTP_RETURNED_ERROR) {
        long status = 0;
        curl_easy_getinfo(t.easy.get(), CURLINFO_RESPONSE_CODE, &status);
        return failed("cannot download " + url + ": " + http_status_text(status));
    }
    if (code != CURLE_OK) {
        std::string detail = t.error[0] ? std::string{t.error.data()} : std::string{curl_easy_strerror(code)};
        return failed("cannot download " + url + ": " + detail);
    }
    downloaded d{t.hasher.hex_digest(), t.size};
    if (!t.request->sha256.empty() && d.sha256 != t.request->sha256) {
        return failed("the download of " + url + " does not match its sha256:\n  expected " + t.request->sha256 + "\n  received " + d.sha256);
    }
    return d;
}

}

std::vector<result<downloaded>> download_all(std::span<download_request const> requests, http_settings const& settings)
{
    global_init();
    std::vector<std::optional<result<downloaded>>> results(requests.size());
    std::vector<std::unique_ptr<transfer>> transfers(requests.size());

    std::unique_ptr<CURLM, multi_deleter> multi{curl_multi_init()};
    std::size_t next = 0;
    std::size_t running = 0;
    auto const launch = [&] {
        while (next < requests.size() && running < std::max<std::size_t>(settings.max_parallel, 1)) {
            std::size_t const i = next++;
            transfers[i] = std::make_unique<transfer>();
            transfers[i]->request = &requests[i];
            if (auto ok = start(*transfers[i], settings); !ok) {
                results[i] = std::unexpected{ok.error()};
                transfers[i].reset();
                continue;
            }
            curl_multi_add_handle(multi.get(), transfers[i]->easy.get());
            ++running;
        }
    };

    launch();
    while (running > 0) {
        int active = 0;
        if (CURLMcode const mc = curl_multi_perform(multi.get(), &active); mc != CURLM_OK) {
            for (std::size_t i = 0; i < requests.size(); ++i) {
                if (!results[i]) results[i] = fail(std::string{"download failed: "} + curl_multi_strerror(mc));
            }
            break;
        }
        int queued = 0;
        while (CURLMsg* msg = curl_multi_info_read(multi.get(), &queued)) {
            if (msg->msg != CURLMSG_DONE) continue;
            transfer* t = nullptr;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &t);
            std::size_t const i = static_cast<std::size_t>(t->request - requests.data());
            CURLcode const code = msg->data.result;
            curl_multi_remove_handle(multi.get(), msg->easy_handle);
            results[i] = finish(*t, code);
            transfers[i].reset();
            --running;
        }
        launch();
        if (running > 0) curl_multi_poll(multi.get(), nullptr, 0, 1000, nullptr);
    }

    std::vector<result<downloaded>> out;
    out.reserve(results.size());
    for (auto& r : results) out.push_back(std::move(*r));
    return out;
}

result<downloaded> download_file(download_request const& request, http_settings const& settings)
{
    return std::move(download_all({&request, 1}, settings).front());
}

namespace {

// The body of a reply, up to a limit.
struct reply_sink
{
    std::string* body;
    std::size_t limit;
    bool truncated = false;
};

std::size_t on_reply(char* data, std::size_t size, std::size_t count, void* user)
{
    auto* sink = static_cast<reply_sink*>(user);
    if (sink->body->size() + size * count > sink->limit) {
        sink->truncated = true;
        return 0;  // stops the transfer
    }
    sink->body->append(data, size * count);
    return size * count;
}

// The header lines of a reply: those of the last response when redirects were followed.
std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user)
{
    auto* headers = static_cast<std::vector<std::pair<std::string, std::string>>*>(user);
    std::string_view line{data, size * count};
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (line.starts_with("HTTP/")) {
        headers->clear();
    } else if (auto const colon = line.find(':'); colon != std::string_view::npos) {
        std::string name{line.substr(0, colon)};
        std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        headers->emplace_back(std::move(name), std::string{value});
    }
    return size * count;
}

struct header_list_deleter
{
    void operator()(curl_slist* l) const noexcept { curl_slist_free_all(l); }
};

}

std::string const* http_reply::header(std::string_view name) const noexcept
{
    for (auto const& [n, value] : headers) {
        if (n.size() == name.size() && std::ranges::equal(n, name, [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); })) {
            return &value;
        }
    }
    return nullptr;
}

std::string http_status_text(long status)
{
    std::string text = "HTTP status " + std::to_string(status);
    if (status >= 500 && status < 600) text += ", an error of the server, not of this request: try again later";
    return text;
}

result<http_reply> http_call(http_exchange const& request, http_settings const& settings)
{
    global_init();
    std::unique_ptr<CURL, easy_deleter> easy{curl_easy_init()};
    if (!easy) return fail("cannot start a transfer (curl_easy_init)");
    CURL* const h = easy.get();
    std::unique_ptr<curl_slist, header_list_deleter> headers;
    for (std::string const& header : request.headers) {
        curl_slist* const list = curl_slist_append(headers.get(), header.c_str());  // the same list once it has one
        if (!list) return fail("cannot build the request headers");
        if (!headers) headers.reset(list);
    }
    http_reply reply;
    reply_sink sink{&reply.body, request.max_body};
    std::array<char, CURL_ERROR_SIZE> error{};
    curl_easy_setopt(h, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    if (request.method == "HEAD") {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    }
    if (!request.body.empty() || request.method == "POST") {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, settings.connect_timeout);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, settings.stall_timeout);
    curl_easy_setopt(h, CURLOPT_USERAGENT, settings.user_agent.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (!settings.proxy.empty()) curl_easy_setopt(h, CURLOPT_PROXY, settings.proxy.c_str());
    set_ca(h);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_reply);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &reply.headers);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // whatever libcurl can decode
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, error.data());
    if (CURLcode const code = curl_easy_perform(h); code != CURLE_OK && !(code == CURLE_WRITE_ERROR && sink.truncated)) {
        return fail(request.method + " " + request.url + ": " + (error[0] ? std::string{error.data()} : std::string{curl_easy_strerror(code)}));
    }
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &reply.status);
    reply.truncated = sink.truncated;
    return reply;
}

std::filesystem::path find_ca_bundle(char const* ssl_cert_file, std::span<std::filesystem::path const> candidates)
{
    if (ssl_cert_file && *ssl_cert_file) return path_from_utf8(ssl_cert_file);
    for (std::filesystem::path const& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;  // a link to a file too
    }
    return {};
}

std::span<std::filesystem::path const> known_ca_bundles()
{
    static std::array<std::filesystem::path, 6> const bundles{
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo, Alpine, NixOS, Void
        "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // RHEL, CentOS
        "/etc/ssl/ca-bundle.pem",                             // openSUSE
        "/etc/ssl/cert.pem",                                  // Alpine, the BSDs' layout
        "/etc/pki/tls/cacert.pem",                            // OpenELEC
    };
    return bundles;
}

std::string file_url(std::filesystem::path const& path)
{
    std::string const generic = path_to_utf8(std::filesystem::absolute(path).generic_u8string());
    std::string url = "file://";
    if (!generic.starts_with('/')) url += '/';  // C:/dir → file:///C:/dir
    for (unsigned char const c : generic) {
        bool const plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':';
        if (plain) {
            url += static_cast<char>(c);
        } else {
            std::array<char, 4> hex;
            std::snprintf(hex.data(), hex.size(), "%%%02X", c);
            url += hex.data();
        }
    }
    return url;
}

}
