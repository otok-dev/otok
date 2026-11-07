// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/paths.hpp"

namespace otok {

file_path_kind classify_file_path(std::string_view path) noexcept
{
    if (path.empty() || path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) {
        return file_path_kind::invalid;
    }
    bool const drive = path.size() >= 2 && path[1] == ':'
        && ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z'));
    if (path.front() == '/' || drive) {
        return file_path_kind::outside;
    }
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') {
            continue;
        }
        std::string_view const component = path.substr(start, i - start);
        start = i + 1;
        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            if (--depth < 0) return file_path_kind::outside;
        } else {
            ++depth;
        }
    }
    // a path that ends where it started (`.`, `a/..`) names the directory itself, not a file in it
    return depth == 0 ? file_path_kind::invalid : file_path_kind::inside;
}

}
