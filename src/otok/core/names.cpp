// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/names.hpp"

#include <algorithm>

namespace otok {

namespace {

bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool is_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_lower_hex(char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f'); }

}

bool is_library_name(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 64) return false;
    if (!is_lower(s.front()) && !is_digit(s.front())) return false;
    return std::ranges::all_of(s, [](char c) { return is_lower(c) || is_digit(c) || c == '_' || c == '-'; });
}

bool is_publisher_name(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 39) return false;
    if (s.front() == '-' || s.back() == '-' || s.find("--") != std::string_view::npos) return false;
    return std::ranges::all_of(s, [](char c) { return is_lower(c) || is_digit(c) || c == '-'; });
}

bool is_option_name(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 64 || !is_lower(s.front())) return false;
    return std::ranges::all_of(s, [](char c) { return is_lower(c) || is_digit(c) || c == '-'; });
}

bool is_product_name(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 64) return false;
    if (!is_lower(s.front()) && !is_upper(s.front()) && s.front() != '_') return false;
    return std::ranges::all_of(s, [](char c) { return is_lower(c) || is_upper(c) || is_digit(c) || c == '_'; });
}

bool is_sha256_hex(std::string_view s) noexcept
{
    return s.size() == 64 && std::ranges::all_of(s, is_lower_hex);
}

bool is_git_commit(std::string_view s) noexcept
{
    return s.size() == 40 && std::ranges::all_of(s, is_lower_hex);
}

bool is_git_tag_name(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 255 || s.starts_with('-') || s.starts_with('/') || s.ends_with('/') || s.ends_with('.')
        || s.ends_with(".lock") || s == "@" || s.find("..") != std::string_view::npos || s.find("@{") != std::string_view::npos
        || s.find("//") != std::string_view::npos) {
        return false;
    }
    return std::ranges::none_of(s, [](char c) {
        auto const u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7f || c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' || c == '\\';
    });
}

std::optional<recipe_id> parse_recipe_id(std::string_view s)
{
    std::size_t const slash = s.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    std::string_view const publisher = s.substr(0, slash);
    std::string_view const name = s.substr(slash + 1);
    if (!is_publisher_name(publisher) || !is_recipe_name(name)) return std::nullopt;
    return recipe_id{std::string{publisher}, std::string{name}};
}

}
