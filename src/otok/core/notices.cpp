// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/build_info.hpp"

#include <string_view>
#include <vector>

namespace otok::build_info {

namespace {

struct embedded_file
{
    std::string_view path;
    std::string_view contents;
};

// embedded_files: written by cmake/notices.cmake, `<name>.txt` sorted by name
#include "otok_notices.inc"

std::vector<third_party_notice> make_notices()
{
    std::vector<third_party_notice> list;
    for (embedded_file const& f : embedded_files) {
        std::string_view name = f.path;
        if (name.ends_with(".txt")) name.remove_suffix(4);
        list.push_back({name, f.contents});
    }
    return list;
}

}

std::span<third_party_notice const> third_party_notices() noexcept
{
    static std::vector<third_party_notice> const list = make_notices();
    return list;
}

}
