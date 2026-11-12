// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/sha256.hpp"

#include <array>
#include <fstream>
#include <iterator>

// <...>: third-party, so its warnings are not ours (projects/vs/otok.props, CMakeLists.txt)
#include <dataforge/base_xx/base16.hpp>
#include <dataforge/hashes/sha2.hpp>
#include <dataforge/quark_push_iterator.hpp>

#include "otok/core/fs.hpp"

namespace otok {

namespace {

// bytes → SHA-256 → lowercase hex, appended to `out`
auto make_iterator(std::string& out)
{
    return dataforge::quark_push_iterator(dataforge::int8 | dataforge::sha256 | dataforge::base16l, std::back_inserter(out));
}

using hex_iterator = decltype(make_iterator(std::declval<std::string&>()));

}

// Lives behind a unique_ptr: the iterator writes into `hex` through a back_inserter, so the object
// must never move.
struct sha256_hasher::impl
{
    std::string hex;
    hex_iterator it = make_iterator(hex);
};

sha256_hasher::sha256_hasher() : impl_{std::make_unique<impl>()} {}
sha256_hasher::~sha256_hasher() = default;
sha256_hasher::sha256_hasher(sha256_hasher&&) noexcept = default;
sha256_hasher& sha256_hasher::operator=(sha256_hasher&&) noexcept = default;

void sha256_hasher::update(std::string_view bytes)
{
    if (!bytes.empty()) {
        *impl_->it = bytes;
    }
}

std::string sha256_hasher::hex_digest()
{
    impl_->it.finish();
    std::string hex = std::move(impl_->hex);
    impl_ = std::make_unique<impl>();
    return hex;
}

std::string sha256_hex(std::string_view bytes)
{
    std::string hex;
    auto it = make_iterator(hex);
    if (!bytes.empty()) {
        *it = bytes;
    }
    it.finish();
    return hex;
}

result<std::string> sha256_file(std::filesystem::path const& path)
{
    std::ifstream in{long_path(path), std::ios::binary};
    if (!in) {
        return fail("cannot open '" + path_to_utf8(plain_path(path)) + "'");
    }
    std::string hex;
    auto it = make_iterator(hex);
    std::array<char, 64 * 1024> buffer;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) {
            *it = std::string_view{buffer.data(), static_cast<std::size_t>(in.gcount())};
        }
    }
    if (in.bad()) {
        return fail("cannot read '" + path_to_utf8(plain_path(path)) + "'");
    }
    it.finish();
    return hex;
}

}
