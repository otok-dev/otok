// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <optional>
#include <string>
#include <string_view>

// Names chosen by users and publishers (docs/formats/README.md 2) and hash spellings.
namespace otok {

// `openssl`, `libssh2`, `zxing-cpp`: [a-z0-9][a-z0-9_-]{0,63}.
bool is_library_name(std::string_view s) noexcept;

// The library name reserved for `deps.tools` in build.lua.
inline constexpr std::string_view reserved_library_name = "tools";

// A GitHub login, lowercase: [a-z0-9]([a-z0-9-]{0,38}), no "--", no trailing "-".
bool is_publisher_name(std::string_view s) noexcept;

// Recipe names follow library names.
inline bool is_recipe_name(std::string_view s) noexcept { return is_library_name(s); }

// Options and profiles: [a-z][a-z0-9-]{0,63}.
bool is_option_name(std::string_view s) noexcept;
inline bool is_profile_name(std::string_view s) noexcept { return is_option_name(s); }

// Products and Clang modules: [A-Za-z_][A-Za-z0-9_]{0,63}.
bool is_product_name(std::string_view s) noexcept;

// 64 lowercase hexadecimal digits.
bool is_sha256_hex(std::string_view s) noexcept;

// A full git commit SHA: 40 lowercase hexadecimal digits.
bool is_git_commit(std::string_view s) noexcept;

// A tag name valid as a git ref name (git check-ref-format): no spaces, control characters, `..`,
// `~^:?*[\`, `@{`, `//`; not starting with `-` or `/`, not ending with `/`, `.` or `.lock`.
bool is_git_tag_name(std::string_view s) noexcept;

// `<publisher>/<recipe name>`.
struct recipe_id
{
    std::string publisher;
    std::string name;

    std::string str() const { return publisher + "/" + name; }
    friend bool operator==(recipe_id const&, recipe_id const&) = default;
};

std::optional<recipe_id> parse_recipe_id(std::string_view s);

}
