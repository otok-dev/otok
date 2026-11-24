// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "otok/core/error.hpp"
#include "otok/fetch/archive.hpp"
#include "otok/fetch/http.hpp"

// The stores of a build cache shared by the machines of an organization (cache.md 12, docs/PLAN.md decision 142):
// `cas/<sha256>`, the archives of slice entries, and `ac/<slice key>`, their descriptors.
namespace otok {

// The descriptor format this otok reads and writes (cache.md 12.1).
inline constexpr std::int64_t descriptor_format = 1;

// ac/<slice key> (cache.md 12.1).
struct entry_descriptor
{
    std::string archive;        // the SHA-256 of the archive in cas/
    std::uint64_t size = 0;     // its size in bytes
    std::string pushed_by;      // the user name of the process that pushed it
    std::string machine;        // its host name
    std::string otok;           // its version
    std::string time;           // UTC, 2026-11-24T10:15:00Z
};

std::string descriptor_text(entry_descriptor const& d);

// A descriptor's text; an error for another `format` or a missing or wrong key. Unknown keys are ignored.
result<entry_descriptor> read_descriptor(std::string_view text, std::string const& where);

// A descriptor of an archive pushed now by this process: its user, its machine and this otok.
entry_descriptor describe_push(archive_written const& archive, std::string pushed_by);

// The name of this machine, for descriptors.
std::string host_name();

// What a push did (cache.md 12.3).
struct push_outcome
{
    bool added = false;                         // the descriptor is ours
    std::optional<entry_descriptor> other;      // a descriptor was there, naming another archive: not reproducible
};

class shared_store
{
public:
    // The store at `url` (config.md 2.3): `file:` for a directory, which must exist; `https` or `http` for a server (cache.md
    // 12.4), asked with `token` as a bearer token through `transport` (tests), else http_call with `settings`.
    static result<shared_store> open(std::string const& url, std::string token = {}, http_transport transport = {}, http_settings settings = {});

    std::string const& url() const noexcept;

    // ac/<key> (64 hex digits): nothing when the store has none. A directory store's descriptor is marked as used.
    result<std::optional<entry_descriptor>> lookup(std::string const& key) const;

    // cas/<archive> of `d` into `file` (written from scratch), checked against its SHA-256 and size.
    result<void> download(entry_descriptor const& d, std::filesystem::path const& file) const;

    // Uploads `archive` as cas/<d.archive>, then `d` as ac/<key>, each only when absent (cache.md 12.3).
    result<push_outcome> push(std::string const& key, std::filesystem::path const& archive, entry_descriptor const& d) const;

private:
    struct state;
    explicit shared_store(std::shared_ptr<state const> s);
    std::shared_ptr<state const> state_;
};

// The names of a slice entry the archive holds (cache.md 12.1): not build.log.
inline constexpr std::string_view entry_archive_names[] = {"key.txt", "licenses", "prefix", "result.toml"};

// The archive of the slice entry `entry` (cache.md 12.1) in `file`.
result<archive_written> write_entry_archive(std::filesystem::path const& entry, std::filesystem::path const& file);

// Unpacks the archive of a slice entry into `staging`, which must not exist, and checks it: the names of
// entry_archive_names only, its key.txt hashing to `key` (cache.md 12.2). Nothing is left at `staging` on failure.
result<void> unpack_entry_archive(std::filesystem::path const& file, std::filesystem::path const& staging, std::string const& key);

}
