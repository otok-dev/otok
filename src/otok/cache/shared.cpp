// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/cache/shared.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <system_error>
#include <vector>

#include <toml++/toml.hpp>

#include "otok/core/build_info.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/core/sha256.hpp"
#include "otok/fetch/http.hpp"

#ifdef _WIN32
#   include <windows.h>
#else
#   include <unistd.h>
#endif

namespace otok {

namespace fs = std::filesystem;

namespace {

std::string toml_quoted(std::string const& s)
{
    std::string out = "\"";
    for (char const c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;  // nothing of the kind in what otok writes
        out += c;
    }
    return out + "\"";
}

bool is_hex64(std::string_view s)
{
    return s.size() == 64 && std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string utc_now()
{
    std::time_t const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return text;
}

// A name of its own in the store's tmp/ for each file this process writes there.
std::string temporary_name()
{
    static std::atomic<unsigned> written{0};
    auto const ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(current_process_id()) + "-" + std::to_string(++written) + "-" + std::to_string(ticks) + ".tmp";
}

}

std::string descriptor_text(entry_descriptor const& d)
{
    std::string text = "format = " + std::to_string(descriptor_format) + "\n";
    text += "archive = " + toml_quoted(d.archive) + "\n";
    text += "size = " + std::to_string(d.size) + "\n";
    text += "pushed-by = " + toml_quoted(d.pushed_by) + "\n";
    text += "machine = " + toml_quoted(d.machine) + "\n";
    text += "otok = " + toml_quoted(d.otok) + "\n";
    if (!d.time.empty()) text += "time = " + d.time + "\n";
    return text;
}

result<entry_descriptor> read_descriptor(std::string_view text, std::string const& where)
{
    toml::table t;
    try {
        t = toml::parse(text);
    } catch (toml::parse_error const& e) {
        return fail("the descriptor " + where + " is not TOML: " + std::string{e.description()});
    }
    auto const format = t["format"].value<std::int64_t>();
    if (!format) return fail("the descriptor " + where + " has no 'format'");
    if (*format != descriptor_format) {
        return fail("the descriptor " + where + " has the format " + std::to_string(*format) + ", which this otok does not read");
    }
    entry_descriptor d;
    d.archive = t["archive"].value_or(std::string{});
    if (!is_hex64(d.archive)) return fail("the descriptor " + where + " has no 'archive' of 64 hex digits");
    auto const size = t["size"].value<std::int64_t>();
    if (!size || *size < 0) return fail("the descriptor " + where + " has no 'size'");
    d.size = static_cast<std::uint64_t>(*size);
    d.pushed_by = t["pushed-by"].value_or(std::string{});
    d.machine = t["machine"].value_or(std::string{});
    d.otok = t["otok"].value_or(std::string{});
    if (auto const time = t["time"].value<toml::date_time>()) {
        char text_of[32];
        std::snprintf(text_of, sizeof text_of, "%04d-%02d-%02dT%02d:%02d:%02dZ", static_cast<int>(time->date.year), static_cast<int>(time->date.month),
            static_cast<int>(time->date.day), static_cast<int>(time->time.hour), static_cast<int>(time->time.minute), static_cast<int>(time->time.second));
        d.time = text_of;
    }
    return d;
}

std::string host_name()
{
#ifdef _WIN32
    wchar_t name[256];
    DWORD size = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size)) return {};
    return path_to_utf8(fs::path{std::wstring{name, size}});
#else
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) != 0) return {};
    return name;
#endif
}

entry_descriptor describe_push(archive_written const& archive, std::string pushed_by)
{
    entry_descriptor d;
    d.archive = archive.sha256;
    d.size = archive.size;
    d.pushed_by = std::move(pushed_by);
    d.machine = host_name();
    d.otok = std::string{build_info::version};
    d.time = utc_now();
    return d;
}

struct shared_store::state
{
    std::string url;
    std::string token;
    fs::path directory;         // a file: store; empty: HTTP
    std::string base;           // an HTTP store: its URL without a final `/`
    http_transport transport;   // an HTTP store's requests
    // an HTTP store that failed (cache.md 12.2): not asked again in this command
    mutable std::atomic<bool> unreachable{false};

    std::string where() const { return "the build cache '" + url + "'"; }

    result<http_reply> call(std::string method, std::string const& path, std::vector<std::string> headers = {}, fs::path upload = {},
        fs::path download = {}) const
    {
        http_exchange request;
        request.method = std::move(method);
        request.url = base + "/" + path;
        request.headers = std::move(headers);
        // never in a message: errors name the method and the URL alone
        if (!token.empty()) request.headers.push_back("Authorization: Bearer " + token);
        request.upload = std::move(upload);
        request.download = std::move(download);
        // a GET may be sent to an object store's presigned URL, a PUT never elsewhere
        request.follow_redirects = request.method == "GET" || request.method == "HEAD";
        // a connection refused once is not yet a store out of reach (cache.md 12.2)
        request.retry_connect = true;
        return transport(request);
    }

    // A request that went wrong: the store not asked again in this command.
    std::unexpected<error> broken(std::string const& what) const
    {
        unreachable = true;
        return std::unexpected{error{where() + ": " + what + "; it is not asked again in this command"}};
    }
};

shared_store::shared_store(std::shared_ptr<state const> s) : state_{std::move(s)} {}

result<shared_store> shared_store::open(std::string const& url, std::string token, http_transport transport, http_settings settings)
{
    auto s = std::make_shared<state>();
    s->url = url;
    s->token = std::move(token);
    if (url.starts_with("https://") || url.starts_with("http://")) {
        s->base = url;
        while (s->base.ends_with('/')) s->base.pop_back();
        s->transport = transport ? std::move(transport)
                                 : http_transport{[settings = std::move(settings)](http_exchange const& r) { return http_call(r, settings); }};
        return shared_store{std::move(s)};
    }
    auto const dir = file_url_path(url);
    if (!dir) return fail("the build cache '" + url + "' is not a file: URL of this machine");
    std::error_code ec;
    if (!fs::is_directory(long_path(*dir), ec)) return fail("the build cache '" + url + "': '" + path_to_utf8(*dir) + "' is not a directory");
    s->directory = *dir;
    return shared_store{std::move(s)};
}

std::string const& shared_store::url() const noexcept
{
    return state_->url;
}

result<std::optional<entry_descriptor>> shared_store::lookup(std::string const& key) const
{
    state const& s = *state_;
    std::string text;
    if (s.directory.empty()) {
        if (s.unreachable) return std::optional<entry_descriptor>{};
        auto reply = s.call("GET", "ac/" + key);
        if (!reply) return s.broken(reply.error().message);
        if (reply->status == 404) return std::optional<entry_descriptor>{};
        if (reply->status != 200) return s.broken("GET ac/" + key + ": " + http_status_text(reply->status));
        text = std::move(reply->body);
    } else {
        fs::path const file = s.directory / "ac" / key;
        std::error_code ec;
        if (!fs::exists(long_path(file), ec)) return std::optional<entry_descriptor>{};
        auto read = read_file(file);
        if (!read) return fail(s.where() + ": " + read.error().message);
        text = std::move(*read);
        // its last use, by which the store may be pruned (cache.md 12.1); a read-only store keeps its time
        auto const time = fs::last_write_time(long_path(file), ec);
        if (!ec && fs::file_time_type::clock::now() - time > std::chrono::hours{24}) fs::last_write_time(long_path(file), fs::file_time_type::clock::now(), ec);
    }
    auto d = read_descriptor(text, "ac/" + key + " of " + s.where());
    if (!d) return std::unexpected{d.error()};
    return std::optional<entry_descriptor>{std::move(*d)};
}

result<void> shared_store::download(entry_descriptor const& d, fs::path const& file) const
{
    state const& s = *state_;
    std::error_code ec;
    fs::remove(long_path(file), ec);
    if (s.directory.empty()) {
        if (s.unreachable) return fail(s.where() + " is not asked again in this command");
        auto reply = s.call("GET", "cas/" + d.archive, {}, {}, file);
        if (!reply || reply->status != 200) {
            fs::remove(long_path(file), ec);
            return s.broken(!reply ? reply.error().message : "GET cas/" + d.archive + ": " + http_status_text(reply->status));
        }
    } else {
        fs::copy_file(long_path(s.directory / "cas" / d.archive), long_path(file), ec);
        if (ec) return fail(s.where() + ": cannot copy cas/" + d.archive + ": " + ec.message());
    }
    std::uintmax_t const size = fs::file_size(long_path(file), ec);
    auto hash = sha256_file(file);
    if (!hash || ec || size != d.size || *hash != d.archive) {
        fs::remove(long_path(file), ec);
        return fail(s.where() + ": cas/" + d.archive + " does not have the SHA-256 and size its descriptor names");
    }
    return {};
}

namespace {

// A PUT answered: written (2xx), or there already (412, If-None-Match: *).
bool written(long status)
{
    return status >= 200 && status < 300;
}

}

result<push_outcome> shared_store::push(std::string const& key, fs::path const& archive, entry_descriptor const& d) const
{
    state const& s = *state_;
    push_outcome out;
    // the descriptor there already: nothing to upload, the contents compared
    auto const compare = [&](std::string const& text) {
        auto other = read_descriptor(text, "ac/" + key + " of " + s.where());
        if (other && other->archive != d.archive) out.other = std::move(*other);
    };
    if (s.directory.empty()) {
        if (s.unreachable) return fail(s.where() + " is not asked again in this command");
        // a server that ignores If-None-Match would replace it: asked first
        auto there = s.call("GET", "ac/" + key);
        if (!there) return s.broken(there.error().message);
        if (there->status == 200) {
            compare(there->body);
            return out;
        }
        if (there->status != 404) return s.broken("GET ac/" + key + ": " + http_status_text(there->status));
        // the archive first, so that a reader never finds a descriptor naming nothing; not sent again when it is there
        auto stored = s.call("HEAD", "cas/" + d.archive);
        if (!stored) return s.broken(stored.error().message);
        if (stored->status != 200) {
            auto put = s.call("PUT", "cas/" + d.archive, {"If-None-Match: *", "Content-Type: application/zstd"}, archive);
            if (!put) return s.broken(put.error().message);
            if (!written(put->status) && put->status != 412) return s.broken("PUT cas/" + d.archive + ": " + http_status_text(put->status));
        }
        fs::path const text_file = archive.parent_path() / (archive.filename().string() + ".ac");
        if (auto ok = write_file_atomic(text_file, descriptor_text(d)); !ok) return std::unexpected{ok.error()};
        auto put = s.call("PUT", "ac/" + key, {"If-None-Match: *", "Content-Type: application/toml"}, text_file);
        std::error_code ec;
        fs::remove(long_path(text_file), ec);
        if (!put) return s.broken(put.error().message);
        if (put->status == 412) {
            // pushed by another machine meanwhile
            auto again = s.call("GET", "ac/" + key);
            if (again && again->status == 200) compare(again->body);
            return out;
        }
        if (!written(put->status)) return s.broken("PUT ac/" + key + ": " + http_status_text(put->status));
        out.added = true;
        return out;
    }

    fs::path const& base = s.directory;
    std::error_code ec;
    for (char const* dir : {"ac", "cas", "tmp"}) {
        fs::create_directories(long_path(base / dir), ec);
        if (ec) return fail(s.where() + ": cannot create " + dir + "/: " + ec.message());
    }
    fs::path const stored = base / "cas" / d.archive;
    if (!fs::exists(long_path(stored), ec)) {
        fs::path const temporary = base / "tmp" / temporary_name();
        fs::copy_file(long_path(archive), long_path(temporary), ec);
        if (ec) {
            fs::remove(long_path(temporary), ec);
            return fail(s.where() + ": cannot write cas/" + d.archive + ": " + ec.message());
        }
        auto moved = move_file_if_absent(temporary, stored);
        if (!moved || !*moved) fs::remove(long_path(temporary), ec);
        if (!moved) return fail(s.where() + ": " + moved.error().message);
    }
    fs::path const descriptor = base / "ac" / key;
    fs::path const temporary = base / "tmp" / temporary_name();
    if (auto ok = write_file_atomic(temporary, descriptor_text(d)); !ok) return fail(s.where() + ": " + ok.error().message);
    auto moved = move_file_if_absent(temporary, descriptor);
    if (!moved || !*moved) fs::remove(long_path(temporary), ec);
    if (!moved) return fail(s.where() + ": " + moved.error().message);
    out.added = *moved;
    if (!out.added) {
        if (auto text = read_file(descriptor)) compare(*text);
    }
    return out;
}

result<archive_written> write_entry_archive(fs::path const& entry, fs::path const& file)
{
    std::vector<std::string> names{std::begin(entry_archive_names), std::end(entry_archive_names)};
    return write_tree_archive(entry, names, file);
}

result<void> unpack_entry_archive(fs::path const& file, fs::path const& staging, std::string const& key)
{
    if (auto ok = extract_archive(file, staging, top_level::keep); !ok) return ok;
    std::error_code ec;
    auto const refuse = [&](std::string const& why) -> result<void> {
        fs::remove_all(long_path(staging), ec);
        return fail(why);
    };
    for (fs::directory_entry const& e : fs::directory_iterator{long_path(staging), ec}) {
        std::string const name = path_to_utf8(e.path().filename());
        if (std::ranges::find(entry_archive_names, name) == std::end(entry_archive_names)) return refuse("the archive holds '" + name + "', which no slice entry has");
    }
    auto text = read_file(staging / "key.txt");
    if (!text) return refuse("the archive has no key.txt");
    if (sha256_hex(*text) != key) return refuse("the key.txt of the archive is not that of the slice " + key);
    return {};
}

}
