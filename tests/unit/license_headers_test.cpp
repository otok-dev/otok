// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/build_info.hpp"
#include "otok/core/fs.hpp"

using namespace otok;

// docs/PLAN.md decision 7, open item 33: every source file of otok names its license with the Otok Output Exception
// (LICENSE-EXCEPTION), those whose text reaches otok's output above all. The tree is walked, not listed by git: the
// remote builds get it without .git.
TEST(license_headers, every_source_file_has_one)
{
    std::filesystem::path const root = std::filesystem::path{path_from_utf8(OTOK_TEST_DATA_DIR)}.parent_path().parent_path();
    static constexpr std::array<std::string_view, 13> extensions{".cpp", ".hpp", ".cmake", ".sh", ".ps1", ".rb", ".cmd", ".yml", ".service",
        ".html", ".atom", ".js", ".css"};
    // third-party files of the catalog site, under their own licenses (their .LICENSE.txt beside them)
    static constexpr std::array<std::string_view, 2> vendored{"diff.min.js", "minisearch.js"};
    auto const checked = [&](std::filesystem::path const& file) {
        std::string const name = path_to_utf8(file.filename());
        if (name == "CMakeLists.txt") return true;
        if (std::ranges::find(vendored, name) != vendored.end()) return false;
        return std::ranges::find(extensions, path_to_utf8(file.extension())) != extensions.end();
    };

    std::vector<std::filesystem::path> files;
    for (std::string_view const dir : {"src", "tests/unit", "tests/child", "tools", "cmake", "triplets"}) {
        std::filesystem::path const top = root / path_from_utf8(dir);
        ASSERT_TRUE(std::filesystem::is_directory(top)) << path_to_utf8(top);
        for (auto const& entry : std::filesystem::recursive_directory_iterator(top)) {
            if (entry.is_regular_file() && checked(entry.path())) files.push_back(entry.path());
        }
    }
    for (auto const& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_regular_file() && checked(entry.path())) files.push_back(entry.path());
    }
    EXPECT_GT(files.size(), 150u);

    std::vector<std::string> missing;
    for (std::filesystem::path const& file : files) {
        std::string const text = read_file(file).value_or("");
        // a copyright line of a holder: the author's today (docs/PLAN.md decision 7), any name should that change
        constexpr std::string_view notice = "Otok (c) ";
        std::size_t const copyright = text.find(notice);
        bool const named = copyright != std::string::npos && copyright + notice.size() < text.size()
            && std::string_view{" \r\n"}.find(text[copyright + notice.size()]) == std::string_view::npos;
        if (!named || text.find("SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception") == std::string::npos) {
            missing.push_back(path_to_utf8(file.lexically_relative(root).generic_u8string()));
        }
    }
    std::string list;
    for (std::string const& m : missing) list += "\n  " + m;
    EXPECT_TRUE(missing.empty()) << "files without the license header:" << list;
}

// docs/PLAN.md decision 7: the notices of the third-party software built into otok go with every binary (`otok licenses`)
TEST(license_notices, are_built_into_otok)
{
    std::vector<std::string> names;
    for (build_info::third_party_notice const& n : build_info::third_party_notices()) {
        names.emplace_back(n.name);
        EXPECT_FALSE(n.text.empty()) << n.name;
    }
    // otok's own sources, and the vcpkg ports of every build of the tests; gtest and vcpkg's helpers are not built into otok
    for (std::string const expected : {"dataforge", "jsdiff", "libarchive", "lua", "minisearch", "nlohmann-json", "tomlplusplus"}) {
        EXPECT_NE(std::ranges::find(names, expected), names.end()) << expected;
    }
    EXPECT_EQ(std::ranges::find(names, "gtest"), names.end());
    EXPECT_TRUE(std::ranges::none_of(names, [](std::string const& n) { return n.starts_with("vcpkg-"); }));
    EXPECT_TRUE(std::ranges::is_sorted(names));
}
