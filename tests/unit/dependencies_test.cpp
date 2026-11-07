// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Smoke tests of the third-party libraries: they are found, linked and usable on every build platform.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <toml++/toml.hpp>

// lua-c++: C++ linkage, no extern "C" (docs/PLAN.md decision 18)
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

namespace {

bool destroyed = false;

struct sentinel
{
    ~sentinel() { destroyed = true; }
};

int fails(lua_State* L)
{
    sentinel s;
    return luaL_error(L, "failed");
}

}

TEST(dependencies, lua)
{
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);
    ASSERT_EQ(luaL_dostring(L, "return 6 * 7"), LUA_OK);
    EXPECT_EQ(lua_tointeger(L, -1), 42);

    // the C++ build raises errors as exceptions: destructors on the way to pcall run
    lua_pushcfunction(L, fails);
    EXPECT_EQ(lua_pcall(L, 0, 0, 0), LUA_ERRRUN);
    EXPECT_STREQ(lua_tostring(L, -1), "failed");
    EXPECT_TRUE(destroyed);
    lua_close(L);
}

TEST(dependencies, tomlplusplus)
{
    auto table = toml::parse("[otok]\nformat = 1\n");
    EXPECT_EQ(table["otok"]["format"].value<int64_t>(), 1);
}

TEST(dependencies, curl)
{
    curl_version_info_data const* info = curl_version_info(CURLVERSION_NOW);
    ASSERT_NE(info, nullptr);
    EXPECT_NE(info->features & CURL_VERSION_SSL, 0) << "libcurl without TLS";
    bool https = false;
    for (char const* const* p = info->protocols; *p; ++p) https = https || std::string_view{*p} == "https";
    EXPECT_TRUE(https);
}

// Writes a one-file tar with each codec of source archives and reads it back.
TEST(dependencies, libarchive)
{
    using add_filter = int (*)(archive*);
    for (add_filter const filter : {&archive_write_add_filter_gzip, &archive_write_add_filter_bzip2, &archive_write_add_filter_xz,
             &archive_write_add_filter_zstd}) {
        std::string const content = "int main() { return 0; }\n";
        std::vector<char> buffer(64 * 1024);
        std::size_t used = 0;

        archive* w = archive_write_new();
        ASSERT_EQ(archive_write_set_format_pax_restricted(w), ARCHIVE_OK);
        ASSERT_EQ(filter(w), ARCHIVE_OK) << archive_error_string(w);
        ASSERT_EQ(archive_write_open_memory(w, buffer.data(), buffer.size(), &used), ARCHIVE_OK) << archive_error_string(w);
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, "src/main.c");
        archive_entry_set_size(e, static_cast<la_int64_t>(content.size()));
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        ASSERT_EQ(archive_write_header(w, e), ARCHIVE_OK) << archive_error_string(w);
        ASSERT_EQ(archive_write_data(w, content.data(), content.size()), static_cast<la_ssize_t>(content.size()));
        archive_entry_free(e);
        ASSERT_EQ(archive_write_close(w), ARCHIVE_OK);
        archive_write_free(w);

        archive* r = archive_read_new();
        archive_read_support_filter_all(r);
        archive_read_support_format_all(r);
        ASSERT_EQ(archive_read_open_memory(r, buffer.data(), used), ARCHIVE_OK) << archive_error_string(r);
        archive_entry* entry = nullptr;
        ASSERT_EQ(archive_read_next_header(r, &entry), ARCHIVE_OK) << archive_error_string(r);
        EXPECT_STREQ(archive_entry_pathname(entry), "src/main.c");
        std::string read(content.size(), '\0');
        EXPECT_EQ(archive_read_data(r, read.data(), read.size()), static_cast<la_ssize_t>(content.size()));
        EXPECT_EQ(read, content);
        archive_read_free(r);
    }
}
