// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Machine settings: docs/formats/config.md.

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "otok/config/config.hpp"
#include "otok/core/fs.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

environment fake(std::map<std::string, std::string, std::less<>> vars)
{
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        auto it = vars.find(name);
        if (it == vars.end()) return std::nullopt;
        return it->second;
    };
}

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += format(item) + "\n";
    return text;
}

}

TEST(config, reads_every_key)
{
    diagnostics d;
    auto c = parse_config("jobs = 12\ncache-dir = \"/Volumes/Fast/otok-cache\"\nindex-mirror = \"https://mirror\"\n"
        "asset-mirror = \"https://assets/{sha256}\"\nproxy = \"http://proxy:3128\"\nkeep-build-dirs = true\nsandbox = false\ncolor = \"never\"\n"
        "visual-studio = \"17\"\nandroid-ndk = \"/opt/ndk/29\"\nwindows-sdk = \"/opt/xwin\"\nllvm = \"/opt/llvm\"\ngit = \"/opt/git/bin/git\"\ncache-max-age = 7\nmirrors-only = true\n"
        "[[url-rewrite]]\nfrom = \"https://github.com/\"\nto = \"https://git.example.com/github/\"\n", "config.toml", d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_EQ(c->mirrors.asset_mirrors, std::vector<std::string>{"https://assets/{sha256}"});
    ASSERT_EQ(c->mirrors.rewrites.size(), 1u);
    EXPECT_EQ(c->mirrors.rewrites.front().from, "https://github.com/");
    EXPECT_EQ(c->mirrors.rewrites.front().to, "https://git.example.com/github/");
    EXPECT_TRUE(c->mirrors.mirrors_only);
    EXPECT_EQ(c->jobs, 12);
    EXPECT_EQ(c->cache_dir, "/Volumes/Fast/otok-cache");
    EXPECT_EQ(c->keep_build_dirs, true);
    EXPECT_EQ(c->sandbox, false);
    EXPECT_EQ(c->color, color_mode::never);
    EXPECT_EQ(c->visual_studio, "17");
    EXPECT_EQ(c->android_ndk, "/opt/ndk/29");
    EXPECT_EQ(c->windows_sdk, "/opt/xwin");
    EXPECT_EQ(c->llvm, "/opt/llvm");
    EXPECT_EQ(c->git, "/opt/git/bin/git");
    EXPECT_EQ(c->cache_max_age, 7);
}

TEST(config, unknown_keys_only_warn)
{
    diagnostics d;
    auto c = parse_config("jobs = 4\nfuture-setting = true\n", "config.toml", d);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(d.items().size(), 1u);
    EXPECT_EQ(d.items()[0].level, severity::warning);
    EXPECT_FALSE(d.has_errors());
}

TEST(config, wrong_values_are_errors)
{
    diagnostics d;
    EXPECT_FALSE(parse_config("jobs = 0\ncolor = \"rainbow\"\nproxy = 1\ncache-max-age = -1\n", "config.toml", d).has_value());
    EXPECT_EQ(d.error_count(), 4u) << messages(d);
}

TEST(config, environment_overrides_the_file)
{
    config_file file;
    file.jobs = 4;
    file.cache_dir = "/from-file";
    file.color = color_mode::always;
    diagnostics d;
    auto c = resolve_config(file, fake({{"OTOK_JOBS", "16"}, {"OTOK_CACHE_DIR", "/from-env"}, {"OTOK_COLOR", "never"},
        {"OTOK_KEEP_BUILD_DIRS", "yes"}, {"HTTPS_PROXY", "http://corp:8080"}, {"OTOK_VISUAL_STUDIO", "18"}, {"OTOK_ANDROID_NDK", "/ndk"},
        {"OTOK_WINDOWS_SDK", "/xwin"}, {"OTOK_LLVM", "/llvm"}, {"OTOK_CACHE_MAX_AGE", "0"}, {"OTOK_SANDBOX", "0"}, {"OTOK_GIT", "/git"}}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_EQ(c->jobs, 16);
    EXPECT_EQ(path_to_utf8(c->cache_dir), "/from-env");
    EXPECT_EQ(c->color, color_mode::never);
    EXPECT_EQ(c->visual_studio, "18");
    EXPECT_EQ(c->android_ndk, "/ndk");
    EXPECT_EQ(c->windows_sdk, "/xwin");
    EXPECT_EQ(c->llvm, "/llvm");
    EXPECT_EQ(c->cache_max_age, 0);
    EXPECT_TRUE(c->keep_build_dirs);
    EXPECT_FALSE(c->sandbox);
    EXPECT_EQ(c->git, "/git");
    EXPECT_EQ(c->proxy, "http://corp:8080");
}

TEST(config, refuses_the_keys_of_installed_toolchains)
{
    // docs/PLAN.md decision 85: what replaces them is named
    diagnostics d;
    EXPECT_FALSE(parse_config("tool-paths = [\"/opt/msys/usr/bin\"]\ntoolchains = \"installed\"\n", "config.toml", d).has_value());
    EXPECT_EQ(d.error_count(), 2u) << messages(d);
    EXPECT_NE(messages(d).find("'tool-paths' at the top level is no longer read: the toolchains of published recipes are built by their "
        "recipes; set 'git' for git and 'llvm' for clang-cl, and give other tools in PATH"), std::string::npos) << messages(d);
    EXPECT_NE(messages(d).find("'toolchains' at the top level is no longer read: a toolchain with 'default-recipe' is always built by its recipe"),
        std::string::npos) << messages(d);

    config_file file;
    file.cache_dir = "/c";
    diagnostics e;
    EXPECT_FALSE(resolve_config(file, fake({{"OTOK_TOOL_PATH", "/opt/python"}, {"OTOK_TOOLCHAINS", "installed"}}), e).has_value());
    EXPECT_NE(messages(e).find("OTOK_TOOL_PATH is no longer read (config.md 2): unset it"), std::string::npos) << messages(e);
    EXPECT_NE(messages(e).find("OTOK_TOOLCHAINS is no longer read (config.md 2): unset it"), std::string::npos) << messages(e);
}

TEST(config, file_values_and_defaults)
{
    config_file file;
    file.cache_dir = "/from-file";
    file.proxy = "http://file-proxy";
    diagnostics d;
    auto c = resolve_config(file, fake({{"HTTPS_PROXY", "http://env-proxy"}}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_GE(c->jobs, 1);
    EXPECT_EQ(path_to_utf8(c->cache_dir), "/from-file");
    EXPECT_EQ(c->proxy, "http://file-proxy");  // config.toml before the generic proxy variables
    EXPECT_FALSE(c->keep_build_dirs);
    EXPECT_TRUE(c->sandbox);
    EXPECT_EQ(c->git, "");  // git of PATH
    EXPECT_EQ(c->color, color_mode::automatic);
    EXPECT_EQ(c->cache_max_age, 30);
}

TEST(config, the_public_index_unless_one_is_named)
{
    config_file file;
    file.cache_dir = "/c";
    diagnostics d;
    auto c = resolve_config(file, fake({}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_EQ(c->index, "view+https://otok-dev.github.io/index");

    file.index = "view+https://corp.example.com";
    EXPECT_EQ(resolve_config(file, fake({}), d)->index, "view+https://corp.example.com");
    EXPECT_EQ(resolve_config(file, fake({{"OTOK_INDEX", "https://git.example.com/index"}}), d)->index, "https://git.example.com/index");
}

TEST(config, an_empty_index_is_none)
{
    diagnostics d;
    auto file = parse_config("cache-dir = \"/c\"\nindex = \"\"\n", "config.toml", d);
    ASSERT_TRUE(file.has_value()) << messages(d);
    EXPECT_EQ(resolve_config(*file, fake({}), d)->index, "");

    config_file named;
    named.cache_dir = "/c";
    named.index = "view+https://corp.example.com";
    EXPECT_EQ(resolve_config(named, fake({{"OTOK_INDEX", ""}}), d)->index, "");
    EXPECT_FALSE(d.has_errors()) << messages(d);
}

TEST(config, bad_environment_values_are_errors)
{
    diagnostics d;
    config_file file;
    file.cache_dir = "/c";
    EXPECT_FALSE(resolve_config(file, fake({{"OTOK_JOBS", "many"}, {"OTOK_KEEP_BUILD_DIRS", "maybe"}, {"OTOK_COLOR", "red"},
        {"OTOK_CACHE_MAX_AGE", "-3"}, {"OTOK_SANDBOX", "off"}}), d).has_value());
    EXPECT_EQ(d.error_count(), 5u) << messages(d);
}

TEST(config, asset_mirror_takes_several_templates)
{
    diagnostics d;
    auto c = parse_config("asset-mirror = [\"https://a/{sha256}\", \"file:///m/{sha256}.bin\"]\n", "config.toml", d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_EQ(c->mirrors.asset_mirrors, (std::vector<std::string>{"https://a/{sha256}", "file:///m/{sha256}.bin"}));

    auto r = resolve_config(*c, fake({{"OTOK_CACHE_DIR", "/c"}}), d);
    ASSERT_TRUE(r.has_value()) << messages(d);
    EXPECT_EQ(r->mirrors.asset_mirrors, c->mirrors.asset_mirrors);
    EXPECT_FALSE(r->mirrors.mirrors_only);
}

TEST(config, asset_mirror_needs_a_sha256_template)
{
    diagnostics d;
    EXPECT_FALSE(parse_config("asset-mirror = \"https://assets/\"\n", "config.toml", d).has_value());
    EXPECT_NE(messages(d).find("'asset-mirror' at the top level must hold URL templates containing {sha256}, not 'https://assets/'"), std::string::npos)
        << messages(d);
}

TEST(config, asset_mirror_needs_a_url)
{
    diagnostics d;
    EXPECT_FALSE(parse_config("asset-mirror = [\"/srv/mirror/{sha256}\"]\n", "config.toml", d).has_value());
    EXPECT_NE(messages(d).find("must hold https, http or file URLs, not '/srv/mirror/{sha256}'"), std::string::npos) << messages(d);
}

TEST(config, url_rewrite_needs_from_and_to)
{
    diagnostics d;
    EXPECT_FALSE(parse_config("[[url-rewrite]]\nfrom = \"https://a/\"\n[[url-rewrite]]\nfrom = \"\"\nto = \"https://b/\"\n", "config.toml", d).has_value());
    std::string const text = messages(d);
    EXPECT_EQ(d.error_count(), 2u) << text;
    EXPECT_NE(text.find("'to'"), std::string::npos) << text;
    EXPECT_NE(text.find("'from' in url-rewrite[2] must not be empty"), std::string::npos) << text;
}

TEST(config, mirrors_only_for_some_libraries)
{
    diagnostics d;
    auto c = parse_config("mirrors-only = [\"boost\", \"icu4c\"]\n", "config.toml", d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_FALSE(c->mirrors.mirrors_only);
    EXPECT_EQ(c->mirrors.mirrors_only_libraries, (std::vector<std::string>{"boost", "icu4c"}));

    EXPECT_FALSE(parse_config("mirrors-only = [\"boost\", \"boost\", \"Bad Name\"]\n", "config.toml", d).has_value());
    EXPECT_NE(messages(d).find("'mirrors-only' at the top level lists 'boost' twice"), std::string::npos) << messages(d);
    EXPECT_NE(messages(d).find("holds 'Bad Name', which is not a library name"), std::string::npos) << messages(d);
}

TEST(config, mirror_environment_variables)
{
    config_file file;
    file.cache_dir = "/c";
    file.mirrors.asset_mirrors = {"https://file/{sha256}"};
    diagnostics d;
    auto c = resolve_config(file, fake({{"OTOK_ASSET_MIRROR", " https://a/{sha256}  file:///b/{sha256} "}, {"OTOK_MIRRORS_ONLY", "yes"}}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_EQ(c->mirrors.asset_mirrors, (std::vector<std::string>{"https://a/{sha256}", "file:///b/{sha256}"}));
    EXPECT_TRUE(c->mirrors.mirrors_only);

    EXPECT_FALSE(resolve_config(file, fake({{"OTOK_ASSET_MIRROR", "https://a/"}, {"OTOK_MIRRORS_ONLY", "sometimes"}}), d).has_value());
    EXPECT_EQ(d.error_count(), 2u) << messages(d);
}

TEST(config, host_default_locations)
{
#if defined(_WIN32)
    auto const env = fake({{"APPDATA", "C:/Users/u/AppData/Roaming"}, {"LOCALAPPDATA", "C:/Users/u/AppData/Local"}});
    std::string const config = "C:/Users/u/AppData/Roaming/otok/config.toml";
    std::string const cache = "C:/Users/u/AppData/Local/otok/cache";
#elif defined(__APPLE__)
    auto const env = fake({{"HOME", "/Users/u"}});
    std::string const config = "/Users/u/.config/otok/config.toml";
    std::string const cache = "/Users/u/Library/Caches/otok";
#else
    auto const env = fake({{"HOME", "/home/u"}});
    std::string const config = "/home/u/.config/otok/config.toml";
    std::string const cache = "/home/u/.cache/otok";
#endif
    auto location = config_file_location(env);
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(std::filesystem::path{*location}, path_from_utf8(config));

    diagnostics d;
    auto c = resolve_config({}, env, d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_EQ(c->cache_dir, path_from_utf8(cache));

    EXPECT_EQ(*config_file_location(fake({{"OTOK_CONFIG", "/etc/otok.toml"}})), path_from_utf8("/etc/otok.toml"));
}

// [[build-cache]] (config.md 2.3)
TEST(config, build_cache_stores)
{
    diagnostics d;
    auto c = parse_config("[[build-cache]]\nurl = \"https://cache.example.com/team\"\ntoken-file = \"~/.config/example/token\"\n"
        "[[build-cache]]\nurl = \"file:///mnt/builds/otok\"\npush = true\npush-local = true\n", "config.toml", d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    ASSERT_TRUE(c->build_caches.has_value());
    ASSERT_EQ(c->build_caches->size(), 2u);
    EXPECT_EQ((*c->build_caches)[0].url, "https://cache.example.com/team");
    EXPECT_FALSE((*c->build_caches)[0].push);
    EXPECT_EQ((*c->build_caches)[0].token_file, "~/.config/example/token");
    EXPECT_TRUE((*c->build_caches)[1].push);
    EXPECT_TRUE((*c->build_caches)[1].push_local);

    c->cache_dir = "/c";
#ifdef _WIN32
    char const* const home = "USERPROFILE";
#else
    char const* const home = "HOME";
#endif
    diagnostics e;
    auto resolved = resolve_config(*c, fake({{home, "/home/u"}}), e);
    ASSERT_TRUE(resolved.has_value()) << messages(e);
    ASSERT_EQ(resolved->build_caches.size(), 2u);
    EXPECT_EQ(path_from_utf8(resolved->build_caches[0].token_file), path_from_utf8("/home/u/.config/example/token"));
    EXPECT_EQ(resolved->build_caches[0].token, "");
}

// Relative paths of config.toml are relative to its directory (config.md 2): git and the builds run elsewhere.
TEST(config, relative_paths_of_the_file_are_relative_to_its_directory)
{
    diagnostics d;
    auto c = parse_config("cache-dir = \"../cache\"\nindex-mirror = \"mirror\"\nvisual-studio = \"2026\"\nandroid-ndk = \"ndk\"\n"
        "windows-sdk = \"/opt/xwin\"\nllvm = \"tools/llvm\"\ngit = \"git\"\n"
        "[[build-cache]]\nurl = \"https://cache.example.com\"\ntoken-file = \"tokens/read\"\n"
        "[[build-cache]]\nurl = \"https://cache.example.com\"\ntoken-file = \"~/token\"\n", "config.toml", d);
    ASSERT_TRUE(c.has_value()) << messages(d);
#ifdef _WIN32
    std::string const root = "C:/";
#else
    std::string const root = "/";
#endif
    anchor_config_paths(*c, path_from_utf8(root + "etc/otok"));
    EXPECT_EQ(path_from_utf8(*c->cache_dir), path_from_utf8(root + "etc/cache"));
    EXPECT_EQ(path_from_utf8(*c->index_mirror), path_from_utf8(root + "etc/otok/mirror"));
    EXPECT_EQ(*c->visual_studio, "2026");  // a version, not a path
    EXPECT_EQ(path_from_utf8(*c->android_ndk), path_from_utf8(root + "etc/otok/ndk"));
    EXPECT_EQ(*c->windows_sdk, "/opt/xwin");
    EXPECT_EQ(path_from_utf8(*c->llvm), path_from_utf8(root + "etc/otok/tools/llvm"));
    EXPECT_EQ(*c->git, "git");  // a program of PATH
    EXPECT_EQ(path_from_utf8((*c->build_caches)[0].token_file), path_from_utf8(root + "etc/otok/tokens/read"));
    EXPECT_EQ((*c->build_caches)[1].token_file, "~/token");

    diagnostics e;
    auto urls = parse_config("index-mirror = \"https://mirror.example.com\"\ngit = \"bin/git\"\n", "config.toml", e);
    ASSERT_TRUE(urls.has_value()) << messages(e);
    anchor_config_paths(*urls, path_from_utf8(root + "etc/otok"));
    EXPECT_EQ(*urls->index_mirror, "https://mirror.example.com");
    EXPECT_EQ(path_from_utf8(*urls->git), path_from_utf8(root + "etc/otok/bin/git"));
}

TEST(config, relative_paths_of_variables_are_relative_to_the_current_directory)
{
    diagnostics d;
    auto c = resolve_config({}, fake({{"OTOK_CACHE_DIR", "../cache"}, {"OTOK_ANDROID_NDK", "ndk"}, {"OTOK_GIT", "git"},
        {"OTOK_INDEX_MIRROR", "https://mirror.example.com"}}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    std::filesystem::path const current = std::filesystem::current_path();
    EXPECT_EQ(c->cache_dir, (current / "../cache").lexically_normal());
    EXPECT_TRUE(c->cache_dir.is_absolute());
    EXPECT_EQ(path_from_utf8(c->android_ndk), (current / "ndk").lexically_normal());
    EXPECT_EQ(c->git, "git");
    EXPECT_EQ(c->index_mirror, "https://mirror.example.com");
}

TEST(config, load_config_reads_paths_against_the_file)
{
    test::temp_dir dir;
    dir.write("machine/config.toml", "cache-dir = \"cache\"\n");
    diagnostics d;
    auto c = load_config(fake({{"OTOK_CONFIG", path_to_utf8(dir.path() / "machine" / "config.toml")}}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_EQ(c->cache_dir, (dir.path() / "machine" / "cache").lexically_normal());
}

TEST(config, build_cache_urls_are_checked)
{
    diagnostics d;
    EXPECT_FALSE(parse_config("[[build-cache]]\nurl = \"ftp://x\"\n[[build-cache]]\nurl = \"file://host/share\"\n[[build-cache]]\npush = true\n",
        "config.toml", d).has_value());
    std::string const m = messages(d);
    EXPECT_NE(m.find("'url' in build-cache[1] must be a file:, https or http URL, not 'ftp://x'"), std::string::npos) << m;
    EXPECT_NE(m.find("'url' in build-cache[2] must be a file: URL of a directory of this machine (file:///srv/otok-cache), not 'file://host/share'"),
        std::string::npos) << m;
    EXPECT_NE(m.find("build-cache[3]"), std::string::npos) << m;
}

// OTOK_BUILD_CACHE replaces the file's stores, read only; OTOK_BUILD_CACHE_PUSH pushes to one; the token of the variables
TEST(config, build_cache_environment_variables)
{
    config_file file;
    file.cache_dir = "/c";
    file.build_caches = std::vector<build_cache_store>{{"https://file-store", true, false, "/token", ""}};
    diagnostics d;
    auto c = resolve_config(file, fake({{"OTOK_BUILD_CACHE", "https://a  file:///b"}, {"OTOK_BUILD_CACHE_PUSH", "file:///b"},
        {"OTOK_BUILD_CACHE_TOKEN", "secret"}}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    ASSERT_EQ(c->build_caches.size(), 2u);
    EXPECT_EQ(c->build_caches[0].url, "https://a");
    EXPECT_FALSE(c->build_caches[0].push);
    EXPECT_EQ(c->build_caches[0].token, "secret");
    EXPECT_EQ(c->build_caches[1].url, "file:///b");
    EXPECT_TRUE(c->build_caches[1].push);

    // a store to push to besides the file's; an empty OTOK_BUILD_CACHE is none
    c = resolve_config(file, fake({{"OTOK_BUILD_CACHE_PUSH", "https://ci"}}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    ASSERT_EQ(c->build_caches.size(), 2u);
    EXPECT_EQ(c->build_caches[0].url, "https://file-store");
    EXPECT_EQ(c->build_caches[1].url, "https://ci");
    EXPECT_TRUE(c->build_caches[1].push);
    c = resolve_config(file, fake({{"OTOK_BUILD_CACHE", ""}}), d);
    ASSERT_TRUE(c.has_value()) << messages(d);
    EXPECT_TRUE(c->build_caches.empty());

    diagnostics e;
    EXPECT_FALSE(resolve_config(file, fake({{"OTOK_BUILD_CACHE", "s3://bucket"}}), e).has_value());
    EXPECT_NE(messages(e).find("OTOK_BUILD_CACHE must be a file:, https or http URL, not 's3://bucket'"), std::string::npos) << messages(e);
}
