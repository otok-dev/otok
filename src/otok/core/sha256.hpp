// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "otok/core/error.hpp"

// SHA-256 (from dataforge), as 64 lowercase hexadecimal digits everywhere in otok's files.
namespace otok {

// Incremental hashing of a byte stream.
class sha256_hasher
{
public:
    sha256_hasher();
    ~sha256_hasher();
    sha256_hasher(sha256_hasher&&) noexcept;
    sha256_hasher& operator=(sha256_hasher&&) noexcept;

    void update(std::string_view bytes);

    // The digest of everything passed to update(); the hasher starts over afterwards.
    std::string hex_digest();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

std::string sha256_hex(std::string_view bytes);

// Streams the file; never holds it in memory as a whole.
result<std::string> sha256_file(std::filesystem::path const& path);

}
