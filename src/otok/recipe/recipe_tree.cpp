// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/recipe/recipe_tree.hpp"

#include <algorithm>
#include <map>

#include "otok/core/fs.hpp"

namespace otok {

namespace {

bool is_portable_component(std::string_view c) noexcept
{
    if (c.empty() || c.front() == '-') return false;
    return std::ranges::all_of(c, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '+' || ch == '-';
    });
}

std::string ascii_lower(std::string_view s)
{
    std::string out{s};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}

bool is_text(std::string_view bytes) noexcept
{
    std::size_t i = 0;
    while (i < bytes.size()) {
        auto const b = static_cast<unsigned char>(bytes[i]);
        if (b == 0) return false;
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (b < 0x80) { ++i; continue; }
        if ((b & 0xE0) == 0xC0) { extra = 1; code = b & 0x1F; }
        else if ((b & 0xF0) == 0xE0) { extra = 2; code = b & 0x0F; }
        else if ((b & 0xF8) == 0xF0) { extra = 3; code = b & 0x07; }
        else return false;
        if (i + extra >= bytes.size()) return false;  // truncated sequence
        for (std::size_t k = 1; k <= extra; ++k) {
            auto const c = static_cast<unsigned char>(bytes[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            code = (code << 6) | (c & 0x3F);
        }
        // overlong forms, surrogates, beyond U+10FFFF
        constexpr std::uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
        if (code < minimum[extra] || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return false;
        i += extra + 1;
    }
    return true;
}

void check_recipe_tree(std::filesystem::path const& dir, std::vector<tree_file> const& files, severity level, diagnostics& diags)
{
    std::string const root = path_to_utf8(dir);
    auto const report = [&](std::string const& file, std::string message) {
        source_position where{file.empty() ? root : root + "/" + file};
        if (level == severity::error) diags.error(std::move(where), std::move(message));
        else diags.warning(std::move(where), std::move(message));
    };

    if (files.size() > recipe_tree_max_files) {
        report({}, "the recipe directory has " + std::to_string(files.size()) + " files; at most "
            + std::to_string(recipe_tree_max_files) + " are allowed");
    }
    std::uint64_t total = 0;
    for (tree_file const& f : files) total += f.size;
    if (total > recipe_tree_max_bytes) {
        report({}, "the recipe directory holds " + std::to_string(total) + " bytes; at most 1 MiB is allowed; "
            "large inputs belong in a [[resource]] with a URL and a hash");
    }

    std::map<std::string, std::string, std::less<>> lowered;
    for (tree_file const& f : files) {
        std::size_t start = 0;
        for (std::size_t i = 0; i <= f.path.size(); ++i) {
            if (i < f.path.size() && f.path[i] != '/') continue;
            if (!is_portable_component(std::string_view{f.path}.substr(start, i - start))) {
                report(f.path, "the path is not portable: use letters, digits, '.', '_', '+' and '-', not starting with '-'");
                break;
            }
            start = i + 1;
        }
        auto [it, inserted] = lowered.emplace(ascii_lower(f.path), f.path);
        if (!inserted) {
            report(f.path, "the path differs from '" + it->second + "' only in case");
        }
        // only read what the size limit allows; a larger tree has already been reported
        if (total <= recipe_tree_max_bytes) {
            auto content = read_file(dir / path_from_utf8(f.path));
            if (!content) {
                report(f.path, content.error().message);
            } else if (!is_text(*content)) {
                report(f.path, "a recipe holds only UTF-8 text; binary data belongs to the library "
                    "(its source or a [[resource]] with a URL and a hash)");
            }
        }
    }
}

}
