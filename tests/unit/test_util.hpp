// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include "otok/core/environment.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/target_info.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/lock/lock.hpp"

namespace otok::test {

// The first entry of a library in a lock, whatever its graph.
inline locked_package const* locked(lock_file const& lock, std::string_view library)
{
    for (locked_package const& p : lock.packages) {
        if (p.library == library) return &p;
    }
    return nullptr;
}

// A target for tests that build nothing for real: `native`, one platform of the machine's architecture, built with the
// compiler `cc` on any host (`*/cc`), the target of the machine's system for tool slices. The core knows no platform, so
// this one serves as well as a real one (the development target `native` was this until docs/PLAN.md decision 82).
inline target_info const& native_info()
{
#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr char const* arch = "arm64";
#else
    constexpr char const* arch = "x86_64";
#endif
    static target_info const native{
        .name = "native",
        .platforms = {{"native", {arch}, {}}},
        .deployment_keys = {},
        .environments = {{"macos", "", "cc"}, {"linux", "", "cc"}, {"windows", "", "cc"}},
        .any_host_compilers = {"cc"},
        .abis = {},
        .host_system = machine_environment("").system,
        .tool_platform = "native",
        .linkages = {"static", "dynamic"},
        .many_configurations = true,
        .choices = {},
        .packages_dir = "",
        .read_recipe_table = {},
        .read_product_table = {},
        .delivers_sources = {},
        .publish_defaults = "",
        .check_publishable = {},
    };
    return native;
}

// `build-on` of a test recipe whose `supported` is `platforms`, as written in TOML ("\"native\", \"ios\""): an environment
// of each of their targets (recipe-toml.md 6).
inline std::string build_on_line(std::string_view platforms)
{
    auto const has = [&](std::string_view name) { return platforms.find("\"" + std::string{name} + "\"") != std::string_view::npos; };
    std::string list;
    auto const add = [&](std::string_view e) { list += (list.empty() ? "\"" : ", \"") + std::string{e} + "\""; };
    if (has("native")) add("*/cc");
    for (char const* p : {"ios", "iossim", "macosx", "catalyst", "tvos", "tvossim", "watchos", "watchossim", "xros", "xrossim"}) {
        if (has(p)) {
            add("macos/xcode");
            break;
        }
    }
    if (has("android")) add("*/ndk");
    if (has("windows")) add("windows/msvc");
    if (has("linux")) add("linux/gcc");
    return "build-on = [" + list + "]\n";
}

// The system git, or nothing (tests that need it skip). `git` is not on the PATH of every Windows
// shell, so the usual install location is tried too.
inline std::optional<git_tool> find_test_git()
{
    env_vars env = current_environment();
    if (auto git = find_git(env)) return *git;
#ifdef _WIN32
    std::filesystem::path const fallback = "C:/Program Files/Git/cmd/git.exe";
    std::error_code ec;
    if (std::filesystem::exists(fallback, ec)) return git_tool{fallback, std::move(env)};
#endif
    return std::nullopt;
}

// A relative path with `/` of more than 260 characters, so that a path ending in it is longer than MAX_PATH below any
// directory, however short the temporary directory of the machine: the paths otok must reach on Windows through long_path.
inline std::string deep_path()
{
    std::string path;
    for (char c = 'a'; c < 'a' + 6; ++c) path += (path.empty() ? "" : "/") + std::string(48, c);
    return path;
}

// A fresh directory under the system temporary directory, removed with everything in it.
class temp_dir
{
public:
    temp_dir()
    {
        static std::atomic<unsigned> counter{0};
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path()
            / ("otok-test-" + std::to_string(rd()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~temp_dir()
    {
        std::error_code ec;
        std::filesystem::remove_all(long_path(path_), ec);
    }
    temp_dir(temp_dir const&) = delete;
    temp_dir& operator=(temp_dir const&) = delete;

    std::filesystem::path const& path() const noexcept { return path_; }

    // Writes `content` byte for byte to `relative` (with `/`), creating directories; beyond 260 characters too (deep_path).
    void write(std::string_view relative, std::string_view content) const
    {
        std::filesystem::path const file = long_path(path_ / path_from_utf8(relative));
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out{file, std::ios::binary};
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

private:
    std::filesystem::path path_;
};

// Writes a .tar.gz of regular files (path with `/`, content); those ending in `.exe` are executable.
inline void write_tar_gz(std::filesystem::path const& file, std::vector<std::pair<std::string, std::string>> const& files)
{
    std::filesystem::create_directories(file.parent_path());
    archive* w = archive_write_new();
    archive_write_set_format_pax_restricted(w);
    archive_write_add_filter_gzip(w);
    archive_write_open_filename(w, path_to_utf8(file).c_str());
    for (auto const& [path, content] : files) {
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, path.c_str());
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, path.ends_with(".exe") ? 0755 : 0644);
        archive_entry_set_size(e, static_cast<la_int64_t>(content.size()));
        archive_write_header(w, e);
        archive_write_data(w, content.data(), content.size());
        archive_entry_free(e);
    }
    archive_write_close(w);
    archive_write_free(w);
}

// The start of a minimal little-endian ELF64 shared object whose dynamic section has one entry,
// `tag` (29: DT_RUNPATH, 1: DT_NEEDED, 14: DT_SONAME), naming the string that follows the image:
// append it and a NUL. Section headers at 0x40 (null, .dynamic, .dynstr), .dynamic at 0x100, .dynstr
// at 0x120 (an empty string, then the value).
inline std::string elf_image(unsigned char tag)
{
    std::string b(0x121, '\0');
    auto const put = [&b](std::size_t at, std::uint64_t value, int size) {
        for (int i = 0; i < size; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<char>(value >> (8 * i) & 0xff);
    };
    b.replace(0, 6, std::string{"\x7f" "ELF\x02\x01", 6});
    put(16, 3, 2);         // ET_DYN
    put(0x28, 0x40, 8);    // e_shoff
    put(0x3a, 0x40, 2);    // e_shentsize
    put(0x3c, 3, 2);       // e_shnum
    put(0x80 + 4, 6, 4);   // .dynamic: SHT_DYNAMIC, offset, size, link to .dynstr
    put(0x80 + 0x18, 0x100, 8);
    put(0x80 + 0x20, 32, 8);
    put(0x80 + 0x28, 2, 4);
    put(0xc0 + 4, 3, 4);   // .dynstr: SHT_STRTAB, offset, size
    put(0xc0 + 0x18, 0x120, 8);
    put(0xc0 + 0x20, 4096, 8);
    put(0x100, tag, 8);    // the entry, then DT_NULL
    put(0x108, 1, 8);
    return b;
}

// `bytes` as a Lua string literal.
inline std::string lua_bytes(std::string const& bytes)
{
    std::string out = "'";
    for (char c : bytes) out += "\\" + std::to_string(static_cast<unsigned char>(c));
    return out + "'";
}


// An HTTP reply with a status and a body, without headers.
inline http_reply http_answer(long status, std::string body)
{
    http_reply reply;
    reply.status = status;
    reply.body = std::move(body);
    return reply;
}


// An HTTP store of the shared build cache in memory (cache.md 12.4): GET, HEAD and PUT with If-None-Match: * of the paths
// under `base`, answered only with the bearer `token` when it has one. Shared by the copies of its transport.
class fake_http_store
{
public:
    explicit fake_http_store(std::string base, std::string token = {}) : state_{std::make_shared<state>()}
    {
        state_->base = std::move(base);
        state_->token = std::move(token);
    }

    http_transport transport() const
    {
        return [s = state_](http_exchange const& e) -> result<http_reply> {
            std::lock_guard lock{s->m};
            s->requests.push_back(e.method + " " + e.url);
            if (s->failing) return fail("connection refused");
            if (!e.url.starts_with(s->base + "/")) return http_answer(404, "");
            if (!s->token.empty() && std::ranges::find(e.headers, "Authorization: Bearer " + s->token) == e.headers.end()) return http_answer(401, "");
            std::string const path = e.url.substr(s->base.size() + 1);
            auto const it = s->files.find(path);
            if (e.method == "GET" || e.method == "HEAD") {
                if (it == s->files.end()) return http_answer(404, "");
                if (e.method == "HEAD") return http_answer(200, "");
                if (!e.download.empty()) {
                    std::ofstream{e.download, std::ios::binary} << it->second;
                    return http_answer(200, "");
                }
                return http_answer(200, it->second);
            }
            if (e.method == "PUT") {
                bool const only_new = std::ranges::find(e.headers, "If-None-Match: *") != e.headers.end();
                if (only_new && it != s->files.end()) return http_answer(412, "");
                std::string body = e.body;
                if (!e.upload.empty()) body = read_file(e.upload).value_or("");
                s->files[path] = std::move(body);
                return http_answer(201, "");
            }
            return http_answer(405, "");
        };
    }

    std::map<std::string, std::string> files() const
    {
        std::lock_guard lock{state_->m};
        return state_->files;
    }
    void set(std::string const& path, std::string content)
    {
        std::lock_guard lock{state_->m};
        state_->files[path] = std::move(content);
    }
    std::vector<std::string> requests() const
    {
        std::lock_guard lock{state_->m};
        return state_->requests;
    }
    void set_failing(bool failing)
    {
        std::lock_guard lock{state_->m};
        state_->failing = failing;
    }

private:
    struct state
    {
        std::mutex m;
        std::string base, token;
        std::map<std::string, std::string> files;
        std::vector<std::string> requests;
        bool failing = false;
    };
    std::shared_ptr<state> state_;
};

}
