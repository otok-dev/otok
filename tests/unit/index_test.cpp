// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "otok/cache/store.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/toml_reader.hpp"
#include "otok/core/tree_hash.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/index/index.hpp"
#include "otok/index/view.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

// The indexes of a project that has only `default` (otok-toml.md 6.4).
std::shared_ptr<index_set> default_only(std::function<result<std::unique_ptr<index_source>>()> open, std::span<target_info const* const> targets)
{
    return std::make_shared<index_set>(std::vector<project_index>{{"default", std::move(open), {}}}, std::map<std::string, std::string, std::less<>>{},
        std::string{}, targets);
}

target_info const* const targets[] = {&target::apple::info(), &test::native_info()};

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += format(item) + "\n";
    return text;
}

std::string const hex40 = "0123456789abcdef0123456789abcdef01234567";
std::string const hex64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// A recipe with every part the projection keeps or leaves out.
std::string recipe_text(std::string_view description = "Test.")
{
    return "[recipe]\napi = 1\nlibrary = \"a\"\nname = \"a-recipe\"\nversion = \"1.0\"\nrevision = 2\nlicense = \"MIT\"\n"
        "license-files = [\"LICENSE\"]\ndescription = \"" + std::string{description} + "\"\nhomepage = \"https://example.com\"\n"
        "keywords = [\"test\"]\nlinkage = [\"static\", \"dynamic\"]\n"
        "[source]\nurl = \"https://example.com/a-1.0.tar.gz\"\nsha256 = \"" + hex64 + "\"\npatches = [\"fix.patch\"]\n"
        "[[resource]]\nname = \"data\"\ngit = \"https://example.com/data\"\ncommit = \"" + hex40 + "\"\ndest = \"data\"\n"
        "[platforms]\nsupported = [\"ios\", \"macosx\"]\nbuild-on = [\"macos/xcode\"]\n"
        "[vars.flag]\ndefault = \"x\"\n"
        "[options.zlib]\ntype = \"bool\"\ndefault = false\ndoc = \"Use zlib.\"\nadditive = true\n"
        "[deps.zlib]\nversion = \"~> 1.3\"\ndefault-recipe = \"p/zlib\"\nwhen = { option = \"zlib\" }\n"
        "[toolchains]\ncmake = { version = \">= 3.20\", default-recipe = \"p/cmake\" }\n"
        "[apple]\nxcode = \">= 16\"\n"
        "[[product]]\nname = \"A\"\nlibraries = [\"a\"]\napple = { privacy-manifest = \"PrivacyInfo.xcprivacy\" }\n"
        "[metadata]\nnote = \"kept out\"\n";
}

locked_location test_recipe_source()
{
    return {.type = locked_location::kind::git, .git = "https://example.com/recipes", .commit = hex40, .path = "otok"};
}

toml::date_time const published{toml::date{2026, 10, 26}, toml::time{12, 30, 0}, toml::time_offset{}};

std::string const privacy_manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict/></plist>\n";

// The entry of the recipe tree `recipe_dir` added to the index checkout `index`: its path, or "already <path>".
result<std::string> publish_entry(std::filesystem::path const& index, std::filesystem::path const& recipe_dir, locked_location const& source,
    std::string_view publisher)
{
    auto addition = make_index_entry(recipe_dir, source, publisher, published, targets);
    if (!addition) return std::unexpected{addition.error()};
    auto added = add_index_entry(index, *addition, targets);
    if (!added) return std::unexpected{added.error()};
    return (*added == index_added::already ? "already " : "") + addition->entry_path;
}

toml::table projection_of(std::string const& text)
{
    diagnostics d;
    auto doc = toml_document::parse(text, "recipe.toml", d);
    EXPECT_TRUE(doc.has_value()) << messages(d);
    return doc ? recipe_projection(doc->table(), "p", targets) : toml::table{};
}

// The first ```toml block after `heading` in a format document.
std::string toml_block(std::string const& document, std::string const& heading)
{
    std::string const text = read_file(path_from_utf8(OTOK_TEST_DATA_DIR "/../../docs/formats/") / document).value_or("");
    std::size_t const at = text.find(heading);
    std::size_t const begin = text.find("```toml\n", at);
    std::size_t const end = text.find("```", begin + 8);
    if (at == std::string::npos || begin == std::string::npos || end == std::string::npos) return {};
    return text.substr(begin + 8, end - begin - 8);
}

// git with a fixed identity, for repositories of tests.
struct test_repo
{
    git_tool git;
    std::filesystem::path dir;
    std::filesystem::path log;

    std::string operator()(std::vector<std::string> args) const
    {
        std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
        full.insert(full.end(), args.begin(), args.end());
        auto out = run_git(git, full, dir, log, true);
        EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message);
        std::string text = out.value_or("");
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        return text;
    }

    std::string commit_all()
    {
        (*this)({"init", "-q"});
        (*this)({"add", "."});
        (*this)({"commit", "-q", "-m", "test"});
        return (*this)({"rev-parse", "HEAD"});
    }
};

std::string replaced(std::string text, std::string const& from, std::string const& to)
{
    std::size_t const at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    return at == std::string::npos ? text : text.replace(at, from.size(), to);
}

}

TEST(index_entry, reads_the_example_of_the_format_document)
{
    std::string const example = toml_block("index.md", "## 3. Version entries");
    ASSERT_FALSE(example.empty());
    diagnostics d;
    auto e = parse_index_entry(example, "index.md", targets, d);
    ASSERT_TRUE(e.has_value()) << messages(d);
    EXPECT_EQ(e->id.str(), "apotocki/openssl");
    EXPECT_EQ(e->projection.name, "openssl");
    EXPECT_EQ(e->projection.library, "openssl");
    EXPECT_EQ(e->projection.version->str(), "3.5.4");
    EXPECT_EQ(e->projection.revision, 1);
    EXPECT_EQ(e->recipe_source.git, "https://github.com/apotocki/openssl-iosx");
    EXPECT_EQ(e->recipe_source.path, "otok");
    EXPECT_EQ(e->projection.options.size(), 3u);
    ASSERT_EQ(e->projection.dependencies.size(), 1u);
    EXPECT_EQ(e->projection.dependencies[0].default_recipe.str(), "apotocki/zlib");
    EXPECT_TRUE(e->projection.products.empty());
    EXPECT_FALSE(e->table->contains("recipe-source"));
    EXPECT_FALSE(e->table->contains("validated-with"));
    EXPECT_TRUE(e->table->contains("apple"));
}

TEST(index_entry, projects_a_recipe)
{
    toml::table const p = projection_of(recipe_text());
    EXPECT_EQ(p["format"].value_or(0), 1);
    EXPECT_EQ(p["recipe"].value_or(std::string{}), "p/a-recipe");
    EXPECT_EQ(p["revision"].value_or(0), 2);
    EXPECT_TRUE(p.contains("linkage"));
    EXPECT_TRUE(p.contains("keywords"));
    for (std::string_view key : {"name", "homepage", "license-files", "vars", "product", "metadata", "channel"}) {
        EXPECT_FALSE(p.contains(key)) << key;
    }
    EXPECT_FALSE(p["source"].as_table()->contains("patches"));
    EXPECT_EQ(p["source"]["sha256"].value_or(std::string{}), hex64);
    toml::table const* resource = p["resource"][0].as_table();
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ((*resource)["name"].value_or(std::string{}), "data");
    EXPECT_FALSE(resource->contains("dest"));
    EXPECT_FALSE(p["options"]["zlib"].as_table()->contains("doc"));
    EXPECT_TRUE(p["options"]["zlib"]["additive"].value_or(false));
    EXPECT_EQ(p["deps"]["zlib"]["when"]["option"].value_or(std::string{}), "zlib");
    EXPECT_EQ(p["toolchains"]["cmake"]["default-recipe"].value_or(std::string{}), "p/cmake");
    EXPECT_EQ(p["apple"]["xcode"].value_or(std::string{}), ">= 16");
}

TEST(index_entry, writes_and_reads_an_entry)
{
    toml::table const p = projection_of(recipe_text());
    std::string const text = write_index_entry(p, test_recipe_source(), hex64, published);
    EXPECT_NE(text.find("recipe-source = { commit = \"" + hex40 + "\", git = \"https://example.com/recipes\", path = \"otok\" }"),
        std::string::npos) << text;
    EXPECT_NE(text.find("published = 2026-10-26T12:30:00Z"), std::string::npos) << text;
    EXPECT_NE(text.find("[[resource]]"), std::string::npos) << text;

    diagnostics d;
    auto e = parse_index_entry(text, "entry.toml", targets, d);
    ASSERT_TRUE(e.has_value()) << messages(d) << text;
    EXPECT_EQ(*e->table, p);
    EXPECT_EQ(e->recipe_source, test_recipe_source());
    EXPECT_EQ(e->recipe_sha256, hex64);
    EXPECT_EQ(e->projection.name, "a-recipe");
    EXPECT_EQ(e->projection.linkage, (std::vector<std::string>{"static", "dynamic"}));
    ASSERT_EQ(e->projection.options.size(), 1u);
    EXPECT_TRUE(e->projection.options[0].doc.empty());
    ASSERT_EQ(e->projection.resources.size(), 1u);
    EXPECT_EQ(e->projection.resources[0].name, "data");
    ASSERT_EQ(e->projection.toolchains.size(), 1u);
    EXPECT_TRUE(e->projection.source.patches.empty());
}

TEST(index_entry, needs_the_publication_keys)
{
    std::string const text = write_index_entry(projection_of(recipe_text()), test_recipe_source(), hex64, published);
    diagnostics d;
    EXPECT_FALSE(parse_index_entry(replaced(text, "recipe-sha256 = \"" + hex64 + "\"", ""), "entry.toml", targets, d));
    EXPECT_NE(messages(d).find("'recipe-sha256'"), std::string::npos) << messages(d);

    d = {};
    EXPECT_FALSE(parse_index_entry(replaced(text, "2026-10-26T12:30:00Z", "2026-10-26T12:30:00"), "entry.toml", targets, d));
    EXPECT_NE(messages(d).find("'published' at the top level must be a date and time with its offset (UTC)"), std::string::npos) << messages(d);

    d = {};
    EXPECT_FALSE(parse_index_entry(replaced(text, "format = 1", "format = 2"), "entry.toml", targets, d));
    EXPECT_NE(messages(d).find("index entry format 2 needs a newer otok: this one reads format 1"), std::string::npos) << messages(d);
}

// index.md 3: an entry of a pull request has no `published`, which index CI writes as it merges it
TEST(index_entry, gets_the_time_of_its_publication_when_merged)
{
    std::string const proposed = write_index_entry(projection_of(recipe_text()), test_recipe_source(), hex64, std::nullopt);
    EXPECT_EQ(proposed.find("published"), std::string::npos) << proposed;
    diagnostics d;
    auto e = parse_index_entry(proposed, "entry.toml", targets, d);
    ASSERT_TRUE(e.has_value()) << messages(d);
    EXPECT_FALSE(e->published.has_value());

    auto const merged = stamp_index_entry(proposed, published);
    ASSERT_TRUE(merged.has_value()) << merged.error().message;
    EXPECT_EQ(*merged, write_index_entry(projection_of(recipe_text()), test_recipe_source(), hex64, published));
    e = parse_index_entry(*merged, "entry.toml", targets, d);
    ASSERT_TRUE(e.has_value()) << messages(d);
    EXPECT_EQ(e->published, published);
    EXPECT_FALSE(stamp_index_entry("format = ", published).has_value());
}

TEST(index_entry, refuses_keys_the_projection_leaves_out)
{
    std::string const text = write_index_entry(projection_of(recipe_text()), test_recipe_source(), hex64, published);
    diagnostics d;
    EXPECT_FALSE(parse_index_entry(text + "homepage = \"https://example.com\"\n", "entry.toml", targets, d));
    EXPECT_FALSE(d.items().empty());

    d = {};
    EXPECT_FALSE(parse_index_entry(replaced(text, "additive = true", "additive = true\ndoc = \"Use zlib.\""), "entry.toml", targets, d));
    EXPECT_NE(messages(d).find("'doc'"), std::string::npos) << messages(d);
}

TEST(index_library, reads_the_scheme)
{
    diagnostics d;
    auto o = parse_library_file("", "a.toml", d);
    ASSERT_TRUE(o.has_value()) << messages(d);
    EXPECT_EQ(o->scheme(), version_scheme::dotted);
    o = parse_library_file("scheme = \"semver\"\n", "a.toml", d);
    ASSERT_TRUE(o.has_value()) << messages(d);
    EXPECT_EQ(o->scheme(), version_scheme::semver);
    o = parse_library_file("scheme = \"explicit\"\norder = [\"beta\", \"1.0\"]\n", "a.toml", d);
    ASSERT_TRUE(o.has_value()) << messages(d);
    EXPECT_EQ(o->compare(*version::parse("beta"), *version::parse("1.0")), std::partial_ordering::less);

    EXPECT_FALSE(parse_library_file("scheme = \"explicit\"\n", "a.toml", d));
    EXPECT_NE(messages(d).find("the explicit scheme needs 'order'"), std::string::npos) << messages(d);
    d = {};
    EXPECT_FALSE(parse_library_file("order = [\"1.0\"]\n", "a.toml", d));
    EXPECT_NE(messages(d).find("'order' at the top level is for the explicit scheme only"), std::string::npos) << messages(d);
    d = {};
    EXPECT_FALSE(parse_library_file("scheme = \"calendar\"\n", "a.toml", d));
    EXPECT_NE(messages(d).find("must be \"dotted\", \"semver\" or \"explicit\", not 'calendar'"), std::string::npos) << messages(d);
}

TEST(recipe_index, adds_and_reads_entries)
{
    test::temp_dir dir;
    dir.write("recipe/recipe.toml", recipe_text());
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    dir.write("recipe/PrivacyInfo.xcprivacy", privacy_manifest);
    std::filesystem::path const index = dir.path() / "index";
    auto added = publish_entry(index, dir.path() / "recipe", test_recipe_source(), "p");
    ASSERT_TRUE(added.has_value()) << added.error().message;
    std::string const first = *added;
    EXPECT_EQ(first, "recipes/p/a-recipe/apple/1.0-r2.toml");
    EXPECT_EQ(read_file(index / "libraries" / "a.toml").value_or("?"), "scheme = \"dotted\"\n");

    // the same again: published already (DESIGN 7.4); a changed tree is refused
    added = publish_entry(index, dir.path() / "recipe", test_recipe_source(), "p");
    ASSERT_TRUE(added.has_value()) << added.error().message;
    EXPECT_EQ(*added, "already recipes/p/a-recipe/apple/1.0-r2.toml");
    // the same tree elsewhere, a later commit of another repository: published already, the entry unchanged
    locked_location moved = test_recipe_source();
    moved.git = "https://example.com/moved";
    moved.commit = std::string(40, 'b');
    added = publish_entry(index, dir.path() / "recipe", moved, "p");
    ASSERT_TRUE(added.has_value()) << added.error().message;
    EXPECT_EQ(*added, "already recipes/p/a-recipe/apple/1.0-r2.toml");
    EXPECT_NE(read_file(index / "recipes" / "p" / "a-recipe" / "apple" / "1.0-r2.toml").value_or("").find("https://example.com/recipes"), std::string::npos);
    dir.write("recipe/fix.patch", "--- a\n+++ c\n");
    added = publish_entry(index, dir.path() / "recipe", test_recipe_source(), "p");
    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().message,
        "the index has recipes/p/a-recipe/apple/1.0-r2.toml already, published differently; entries never change, so publish a new revision instead");
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    added = publish_entry(index, dir.path() / "recipe", test_recipe_source(), "P!");
    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().message, "'P!' is not a valid publisher name");

    recipe_index idx{index, targets};
    auto entries = idx.entries({"p", "a-recipe"});
    ASSERT_TRUE(entries.has_value()) << entries.error().message;
    ASSERT_EQ((*entries)->size(), 1u);
    EXPECT_EQ((**entries)[0].recipe_sha256, tree_hash(dir.path() / "recipe").value_or("?"));
    auto order = idx.order("a");
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->scheme(), version_scheme::dotted);
    EXPECT_EQ(idx.order("unknown").value_or(version_order::semver()).scheme(), version_scheme::dotted);

    auto missing = idx.entries({"p", "zz"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "the index has no recipe 'p/zz'");

    // an entry under another name, or beside the directories of the variants (index.md 3)
    dir.write("index/recipes/p/a-recipe/windows/2.0-r1.toml", read_file(index / path_from_utf8(first)).value_or(""));
    recipe_index renamed{index, targets, index_reading::ci};
    auto wrong = renamed.entries({"p", "a-recipe"});
    ASSERT_FALSE(wrong.has_value());
    EXPECT_NE(wrong.error().message.find("recipes/p/a-recipe/windows/2.0-r1.toml: error: the entry is of the variant 'apple' and version 1.0-r2, "
        "which does not match its file: it belongs in apple/1.0-r2.toml"), std::string::npos) << wrong.error().message;
    std::filesystem::remove(index / "recipes" / "p" / "a-recipe" / "windows" / "2.0-r1.toml");
    dir.write("index/recipes/p/a-recipe/1.0-r2.toml", read_file(index / path_from_utf8(first)).value_or(""));
    recipe_index beside{index, targets, index_reading::ci};
    wrong = beside.entries({"p", "a-recipe"});
    ASSERT_FALSE(wrong.has_value());
    EXPECT_NE(wrong.error().message.find("recipes/p/a-recipe/1.0-r2.toml: error: the entry is of the variant 'apple'"), std::string::npos)
        << wrong.error().message;
}

TEST(recipe_index, refuses_recipes_beyond_the_limits)
{
    test::temp_dir dir;
    dir.write("recipe/recipe.toml", recipe_text());
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    dir.write("recipe/PrivacyInfo.xcprivacy", privacy_manifest);
    dir.write("recipe/blob.bin", std::string{"\0\1\2", 3});
    auto added = publish_entry(dir.path() / "index", dir.path() / "recipe", test_recipe_source(), "p");
    ASSERT_FALSE(added.has_value());
    EXPECT_NE(added.error().message.find("cannot be published"), std::string::npos) << added.error().message;
    EXPECT_NE(added.error().message.find("blob.bin"), std::string::npos) << added.error().message;
}

TEST(recipe_index, refuses_what_publication_requires)
{
    test::temp_dir dir;
    dir.write("recipe/recipe.toml", replaced(recipe_text(), "apple = { privacy-manifest = \"PrivacyInfo.xcprivacy\" }\n", ""));
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    auto added = publish_entry(dir.path() / "index", dir.path() / "recipe", test_recipe_source(), "p");
    ASSERT_FALSE(added.has_value());
    EXPECT_NE(added.error().message.find("the product 'A' has no privacy manifest ('privacy-manifest' in apple), neither its own nor one of a product it depends on"),
        std::string::npos) << added.error().message;
}

// An entry of one target, in the directory of its variant; it says nothing of a build (docs/PLAN.md decision 82)
TEST(index_entry, is_of_one_target)
{
    test::temp_dir dir;
    dir.write("recipe/recipe.toml", recipe_text());
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    dir.write("recipe/PrivacyInfo.xcprivacy", privacy_manifest);
    auto addition = make_index_entry(dir.path() / "recipe", test_recipe_source(), "p", published, targets);
    ASSERT_TRUE(addition.has_value()) << addition.error().message;
    EXPECT_EQ(addition->label, "p/a-recipe 1.0-r2 (apple)");
    EXPECT_EQ(addition->entry_path, "recipes/p/a-recipe/apple/1.0-r2.toml");
    EXPECT_EQ(addition->library_path, "libraries/a.toml");
    EXPECT_NE(addition->entry_text.find("variant = \"apple\""), std::string::npos) << addition->entry_text;
    diagnostics d;
    EXPECT_FALSE(parse_index_entry(addition->entry_text + "validated-with = { otok = \"0.1.0\" }\n", "e.toml", targets, d).has_value());
    EXPECT_NE(messages(d).find("validated-with"), std::string::npos) << messages(d);

    // a variant of its own name
    dir.write("recipe/recipe.toml", replaced(recipe_text(), "name = \"a-recipe\"\n", "name = \"a-recipe\"\nvariant = \"ios-only\"\n"));
    addition = make_index_entry(dir.path() / "recipe", test_recipe_source(), "p", published, targets);
    ASSERT_TRUE(addition.has_value()) << addition.error().message;
    EXPECT_EQ(addition->entry_path, "recipes/p/a-recipe/ios-only/1.0-r2.toml");

    // two targets: a variant of each
    dir.write("recipe/recipe.toml", replaced(replaced(recipe_text(), "name = \"a-recipe\"\n", "name = \"a-recipe\"\nvariant = \"all\"\n"),
        "supported = [\"ios\", \"macosx\"]\nbuild-on = [\"macos/xcode\"]", "supported = [\"ios\", \"native\"]\nbuild-on = [\"macos/xcode\", \"*/cc\"]"));
    addition = make_index_entry(dir.path() / "recipe", test_recipe_source(), "p", published, targets);
    ASSERT_FALSE(addition.has_value());
    EXPECT_NE(addition.error().message.find("its platforms are of the targets 'apple', 'native', and a published recipe has one (index.md 3)"),
        std::string::npos) << addition.error().message;
}

// recipe-toml.md 13, docs/PLAN.md decision 85: what builds for the publisher builds for everyone
TEST(index_entry, needs_the_recipes_of_toolchains)
{
    test::temp_dir dir;
    dir.write("recipe/recipe.toml", replaced(recipe_text(), "cmake = { version = \">= 3.20\", default-recipe = \"p/cmake\" }\n",
        "cmake = { version = \">= 3.20\", default-recipe = \"p/cmake\" }\nperl = \">= 5.30\"\n"));
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    dir.write("recipe/PrivacyInfo.xcprivacy", privacy_manifest);
    auto addition = make_index_entry(dir.path() / "recipe", test_recipe_source(), "p", published, targets);
    ASSERT_FALSE(addition.has_value());
    EXPECT_NE(addition.error().message.find("the toolchain 'perl' has no 'default-recipe': a published recipe names the recipe of each of "
        "its toolchains, so that no build needs it installed (recipe-toml.md 13)"), std::string::npos) << addition.error().message;
    EXPECT_EQ(addition.error().message.find("'cmake'"), std::string::npos) << addition.error().message;
}

TEST(index_checkout, reads_a_git_repository)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    dir.write("index/index.toml", "format = 1\n");
    test_repo repo{*git, dir.path() / "index", dir.path() / "setup.log"};
    std::string const commit = repo.commit_all();
    std::string const url = file_url(dir.path() / "index");

    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    auto checkout = index_checkout(url, "", *cache, &*git);
    ASSERT_TRUE(checkout.has_value()) << checkout.error().message;
    EXPECT_EQ(read_file(*checkout / "index.toml").value_or("?"), "format = 1\n");
    EXPECT_EQ(read_file(dir.path() / "cache" / "index" / git_url_key(url)).value_or("?"), commit + "\n");

    cache->set_offline(true);
    auto offline = index_checkout(url, "", *cache, &*git);
    ASSERT_TRUE(offline.has_value()) << offline.error().message;
    EXPECT_EQ(*offline, *checkout);

    auto other = cache_store::open(dir.path() / "other-cache");
    ASSERT_TRUE(other.has_value());
    other->set_offline(true);
    auto never_read = index_checkout(url, "", *other, &*git);
    ASSERT_FALSE(never_read.has_value());
    EXPECT_EQ(never_read.error().message, "offline, and the index '" + url + "' has not been read on this machine yet");

    auto mirror = index_checkout(url, path_to_utf8(dir.path() / "index"), *other, nullptr);
    ASSERT_TRUE(mirror.has_value()) << mirror.error().message;
    EXPECT_EQ(*mirror, dir.path() / "index");

    auto none = index_checkout("", "", *cache, &*git);
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().message, "no recipe index: 'index' in config.toml (or OTOK_INDEX) is empty; set it to the view of an index, view+https://<base>, or the URL of its git repository");
}

TEST(index_provider, checks_the_recipe_tree_of_a_chosen_version)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    dir.write("recipes/otok/recipe.toml", recipe_text());
    dir.write("recipes/otok/fix.patch", "--- a\n+++ b\n");
    dir.write("recipes/otok/PrivacyInfo.xcprivacy", privacy_manifest);
    test_repo repo{*git, dir.path() / "recipes", dir.path() / "setup.log"};
    locked_location const source{.type = locked_location::kind::git, .git = file_url(repo.dir), .commit = repo.commit_all(), .path = "otok"};
    std::filesystem::path const index = dir.path() / "index";
    auto added = publish_entry(index, repo.dir / "otok", source, "p");
    ASSERT_TRUE(added.has_value()) << added.error().message;
    std::string const entry_text = read_file(index / path_from_utf8(*added)).value_or("");

    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    project_recipe_provider local{dir.path(), targets};
    recipe_ref ref;
    ref.source = recipe_ref::kind::index;
    ref.id = {"p", "a-recipe"};

    auto const chosen = [&](std::string const& text) -> result<candidate> {
        if (auto ok = write_file_atomic(index / path_from_utf8(*added), text); !ok) return std::unexpected{ok.error()};
        index_recipe_provider provider{local, default_only([&] { return result<std::unique_ptr<index_source>>{directory_index_source(index)}; }, targets), targets, &*cache, *git};
        auto list = provider.candidates("a", ref);
        if (!list) return std::unexpected{list.error()};
        EXPECT_EQ(list->size(), 1u);
        candidate c = list->front();
        EXPECT_TRUE(c.dir.empty());
        EXPECT_TRUE(c.recipe->products.empty());
        if (auto ok = provider.prepare(c); !ok) return std::unexpected{ok.error()};
        return c;
    };
    auto c = chosen(entry_text);
    ASSERT_TRUE(c.has_value()) << c.error().message;
    EXPECT_EQ(c->kind, locked_package::recipe_kind::index);
    EXPECT_EQ(c->id.str(), "p/a-recipe");
    EXPECT_EQ(c->recipe_source, source);
    EXPECT_EQ(c->dir, cache->root() / "sources" / ("git-" + source.commit + "-dfd2aa79320a5ecb"));  // sha256("otok")
    ASSERT_EQ(c->recipe->products.size(), 1u);
    EXPECT_EQ(c->recipe->options[0].doc, "Use zlib.");

    auto const sha = c->recipe_sha256;
    c = chosen(replaced(entry_text, sha, hex64));
    ASSERT_FALSE(c.has_value());
    EXPECT_NE(c.error().message.find("the recipe tree of p/a-recipe 1.0 (r2) at " + source.git + " " + source.commit
        + " does not match the recipe-sha256 of its index entry"), std::string::npos) << c.error().message;

    c = chosen(replaced(entry_text, "description = \"Test.\"", "description = \"Other.\""));
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().message, "the index entry of p/a-recipe 1.0 (r2) does not match its recipe.toml (index.md 3.1)");

    index_recipe_provider unconfigured{local, default_only([] { return result<std::unique_ptr<index_source>>{fail("no recipe index is configured")}; }, targets), targets,
        &*cache, *git};
    auto list = unconfigured.candidates("a", ref);
    ASSERT_FALSE(list.has_value());
    EXPECT_EQ(list.error().message, "the recipe 'p/a-recipe' of 'a' needs the index: no recipe index is configured");
}

namespace {

advisory advisory_of(std::string const& text)
{
    diagnostics d;
    auto a = parse_advisory(text, "advisory.toml", d);
    EXPECT_TRUE(a.has_value()) << messages(d);
    return a.value_or(advisory{});
}

std::string advisory_text(std::string const& what, std::string const& versions, std::string const& status = "confirmed")
{
    return "id = \"2026-0001\"\n" + what + "\nversions = [" + versions + "]\nseverity = \"withdrawn\"\nstatus = \"" + status
        + "\"\nsummary = \"Bad.\"\npublished = 2026-11-02T10:00:00Z\n";
}

}

TEST(index_advisory, reads_the_example_of_the_format_document)
{
    std::string const example = toml_block("index.md", "## 7. Advisories");
    ASSERT_FALSE(example.empty());
    advisory const a = advisory_of(example);
    EXPECT_EQ(a.id, "2026-0001");
    ASSERT_EQ(a.recipes.size(), 1u);
    EXPECT_EQ(a.recipes[0].str(), "apotocki/openssl");
    EXPECT_EQ(a.severity, advisory_severity::withdrawn);
    EXPECT_EQ(a.status, advisory_status::confirmed);
    EXPECT_TRUE(a.active());
}

TEST(index_advisory, matches_versions)
{
    recipe_id const id{"p", "a"};
    version_order const dotted = version_order::dotted();
    auto const concerns = [&](advisory const& a, std::string const& v, std::int64_t revision, std::string_view library = "a",
                              recipe_id const& rid = {"p", "a"}) {
        return advisory_concerns(a, rid, "apple", library, *version::parse(v), revision, dotted);
    };
    advisory const exact = advisory_of(advisory_text("recipes = [\"p/a\"]", "\"1.2-r1\""));
    EXPECT_TRUE(concerns(exact, "1.2", 1));
    EXPECT_FALSE(concerns(exact, "1.2", 2));
    EXPECT_FALSE(concerns(exact, "1.2", 1, "a", {"q", "a"}));

    advisory const every = advisory_of(advisory_text("recipes = [\"p/a\"]", "\"1.2\""));
    EXPECT_TRUE(concerns(every, "1.2", 7));
    EXPECT_FALSE(concerns(every, "1.3", 1));

    advisory const range = advisory_of(advisory_text("library = \"a\"", "\"< 1.3\", \"2.0-r2\""));
    EXPECT_TRUE(concerns(range, "1.2", 1, "a", {"q", "a"}));
    EXPECT_FALSE(concerns(range, "1.3", 1));
    EXPECT_TRUE(concerns(range, "2.0", 2));
    EXPECT_FALSE(concerns(range, "1.0", 1, "b", {"p", "b"}));

    // a version named like a revision ("1.0-rc1" is not one: "rc1" is no number)
    advisory const rc = advisory_of(advisory_text("recipes = [\"p/a\"]", "\"1.0-rc1\""));
    EXPECT_TRUE(concerns(rc, "1.0-rc1", 3));

    // of some variants only (index.md 7)
    advisory const windows = advisory_of(advisory_text("recipes = [\"p/a\"]\nvariants = [\"windows-msvc\"]", "\"1.2-r1\""));
    EXPECT_FALSE(concerns(windows, "1.2", 1));
    EXPECT_TRUE(advisory_concerns(windows, id, "windows-msvc", "a", *version::parse("1.2"), 1, dotted));
}

TEST(index_advisory, reports_errors)
{
    diagnostics d;
    EXPECT_FALSE(parse_advisory(advisory_text("recipes = [\"p/a\"]\nlibrary = \"a\"", "\"1.0\""), "a.toml", d));
    EXPECT_NE(messages(d).find("an advisory needs exactly one of 'recipes' (recipe ids) or 'library' (a library name)"), std::string::npos)
        << messages(d);
    d = {};
    EXPECT_FALSE(parse_advisory(replaced(advisory_text("library = \"a\"", "\"1.0\""), "\"withdrawn\"", "\"fatal\""), "a.toml", d));
    EXPECT_NE(messages(d).find("must be \"withdrawn\", \"security\" or \"broken\", not 'fatal'"), std::string::npos) << messages(d);
    d = {};
    EXPECT_FALSE(parse_advisory(advisory_text("library = \"a\"", "\"1.0\"", "open"), "a.toml", d));
    EXPECT_NE(messages(d).find("must be \"under-review\", \"confirmed\" or \"rejected\", not 'open'"), std::string::npos) << messages(d);
    d = {};
    EXPECT_FALSE(parse_advisory(advisory_text("library = \"a\"\nvariants = [\"apple\"]", "\"1.0\""), "a.toml", d));
    EXPECT_NE(messages(d).find("'variants' at the top level goes with 'recipes', not 'library'"), std::string::npos) << messages(d);
}

TEST(index_status, reads_deprecations)
{
    diagnostics d;
    auto s = parse_recipe_status("[deprecated]\nreason = \"unmaintained\"\nin-favor-of = \"q/a\"\n", "s.toml", d);
    ASSERT_TRUE(s.has_value()) << messages(d);
    EXPECT_EQ(s->deprecation(), "deprecated: unmaintained; use 'q/a' instead");
    s = parse_recipe_status("transferred-from = \"old/a\"\n", "s.toml", d);
    ASSERT_TRUE(s.has_value()) << messages(d);
    EXPECT_EQ(s->deprecation(), "");
    EXPECT_EQ(s->transferred_from->str(), "old/a");
    EXPECT_FALSE(parse_recipe_status("[deprecated]\nin-favor-of = \"nobody\"\n", "s.toml", d));
    EXPECT_NE(messages(d).find("'in-favor-of' in deprecated must be a recipe id"), std::string::npos) << messages(d);
}

TEST(recipe_index, reads_advisories_and_status)
{
    test::temp_dir dir;
    dir.write("recipe/recipe.toml", recipe_text());
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    dir.write("recipe/PrivacyInfo.xcprivacy", privacy_manifest);
    std::filesystem::path const index = dir.path() / "index";
    ASSERT_TRUE(publish_entry(index, dir.path() / "recipe", test_recipe_source(), "p").has_value());
    dir.write("index/advisories/2026-0001.toml", advisory_text("recipes = [\"p/a-recipe\"]", "\"1.0\""));
    dir.write("index/advisories/2026-0002.toml", replaced(advisory_text("library = \"a\"", "\"1.0-r2\"", "rejected"), "2026-0001", "2026-0002"));
    dir.write("index/recipes/p/a-recipe/recipe-status.toml", "[deprecated]\nreason = \"old\"\n");

    recipe_index idx{index, targets};
    auto found = idx.advisories_of({"p", "a-recipe"}, "apple", "a", *version::parse("1.0"), 2);
    ASSERT_TRUE(found.has_value()) << found.error().message;
    ASSERT_EQ(found->size(), 1u);  // the rejected one does not count
    EXPECT_EQ((*found)[0].id, "2026-0001");
    EXPECT_EQ((*found)[0].severity, advisory_severity::withdrawn);
    auto status = idx.status({"p", "a-recipe"});
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->deprecation(), "deprecated: old");

    project_recipe_provider local{dir.path(), targets};
    index_recipe_provider provider{local, default_only([&] { return result<std::unique_ptr<index_source>>{directory_index_source(index)}; }, targets), targets, nullptr, std::nullopt};
    recipe_ref ref;
    ref.source = recipe_ref::kind::index;
    ref.id = {"p", "a-recipe"};
    auto list = provider.candidates("a", ref);
    ASSERT_TRUE(list.has_value()) << list.error().message;
    ASSERT_EQ(list->size(), 1u);
    EXPECT_EQ(list->front().advisories.size(), 1u);
    EXPECT_EQ(list->front().deprecation, "deprecated: old");

    dir.write("index/advisories/2026-0003.toml", advisory_text("library = \"a\"", "\"1.0\""));
    recipe_index misnamed{index, targets, index_reading::ci};
    auto wrong = misnamed.advisories();
    ASSERT_FALSE(wrong.has_value());
    EXPECT_NE(wrong.error().message.find("advisories/2026-0003.toml: error: the advisory's id '2026-0001' does not match its file name"),
        std::string::npos) << wrong.error().message;
}

namespace {

std::string json_block(std::string const& document, std::string const& heading)
{
    std::string const text = read_file(path_from_utf8(OTOK_TEST_DATA_DIR "/../../docs/formats/") / document).value_or("");
    std::size_t const at = text.find(heading);
    std::size_t const begin = text.find("```json\n", at);
    std::size_t const end = text.find("```", begin + 8);
    if (at == std::string::npos || begin == std::string::npos || end == std::string::npos) return {};
    return text.substr(begin + 8, end - begin - 8);
}

// A checkout of an index with two versions of p/a-recipe, its status, an advisory and index.toml.
std::filesystem::path make_test_index(test::temp_dir const& dir)
{
    dir.write("recipe/recipe.toml", recipe_text());
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    dir.write("recipe/PrivacyInfo.xcprivacy", privacy_manifest);
    dir.write("newer/recipe.toml", replaced(recipe_text("Newer."), "version = \"1.0\"", "version = \"1.1\""));
    dir.write("newer/fix.patch", "--- a\n+++ b\n");
    dir.write("newer/PrivacyInfo.xcprivacy", privacy_manifest);
    std::filesystem::path const index = dir.path() / "index";
    EXPECT_TRUE(publish_entry(index, dir.path() / "recipe", test_recipe_source(), "p").has_value());
    EXPECT_TRUE(publish_entry(index, dir.path() / "newer", test_recipe_source(), "p").has_value());
    dir.write("index/index.toml", "format = 1\nrepository = \"https://github.com/o/index\"\n");
    dir.write("index/recipes/p/a-recipe/recipe-status.toml", "[deprecated]\nreason = \"old\"\n");
    dir.write("index/advisories/2026-0001.toml", advisory_text("recipes = [\"p/a-recipe\"]", "\"1.0\""));
    return index;
}

// A server of the files of a view on disk, with ETags of their contents.
struct fake_view_server
{
    std::filesystem::path root;        // holds index/
    std::vector<http_exchange> requests;
    int status = 0;                    // when set: the reply to every request

    http_transport transport()
    {
        return [this](http_exchange const& e) -> result<http_reply> {
            requests.push_back(e);
            http_reply reply;
            if (status) {
                reply.status = status;
                return reply;
            }
            std::string const prefix = "https://view.example.com/";
            if (!e.url.starts_with(prefix)) return fail("unexpected URL " + e.url);
            auto body = read_file(root / path_from_utf8(e.url.substr(prefix.size())));
            if (!body) {
                reply.status = 404;
                return reply;
            }
            std::string const etag = "\"" + std::to_string(std::hash<std::string>{}(*body)) + "\"";
            if (std::ranges::find(e.headers, "If-None-Match: " + etag) != e.headers.end()) {
                reply.status = 304;
                return reply;
            }
            reply.status = 200;
            reply.body = std::move(*body);
            reply.headers.emplace_back("etag", etag);
            return reply;
        };
    }

    bool conditional(std::size_t i) const
    {
        return std::ranges::any_of(requests[i].headers, [](std::string const& h) { return h.starts_with("If-None-Match: "); });
    }
};

}

TEST(index_settings, reads_the_example_of_the_format_document)
{
    std::string const example = toml_block("index.md", "## 2. `index.toml`");
    ASSERT_FALSE(example.empty());
    diagnostics d;
    auto s = parse_index_settings(example, "index.toml", d);
    ASSERT_TRUE(s.has_value()) << messages(d);
    EXPECT_EQ(s->name, "Otok public index");
    EXPECT_EQ(s->view, "https://otok-dev.github.io/index");
    EXPECT_EQ(s->repository, "https://github.com/otok-dev/index");
    EXPECT_EQ(s->established_entries_per_day, 50u);
    EXPECT_EQ(s->review_entries_per_day, 10u);
    auto const defaults = parse_index_settings("format = 1\n", "index.toml", d).value();
    EXPECT_EQ(defaults.established_entries_per_day, 20u);
    EXPECT_EQ(defaults.review_entries_per_day, 5u);
    EXPECT_EQ(publication_limit_for(defaults, true).key, "established-entries-per-day");
    EXPECT_EQ(publication_limit_for(*s, false).limit, 10u);

    // nothing else is read in an index of another format
    diagnostics newer;
    EXPECT_FALSE(parse_index_settings("format = 2\nrepository = \"git@github.com:o/i\"\n", "index.toml", newer, index_reading::client).has_value());
    EXPECT_EQ(messages(newer), "index.toml:1:10: error: index format 2 needs a newer otok: this one reads format 1\n");

    diagnostics wrong;
    EXPECT_FALSE(parse_index_settings("format = 1\nrepository = \"git@github.com:o/i\"\nestablished-entries-per-day = 0\nreview-entries-per-day = 0\n",
        "index.toml", wrong).has_value());
    EXPECT_EQ(messages(wrong),
        "index.toml:2:14: error: 'repository' at the top level must be an https URL, not 'git@github.com:o/i'\n"
        "index.toml:3:31: error: 'established-entries-per-day' at the top level must be at least 1\n"
        "index.toml:4:26: error: 'review-entries-per-day' at the top level must be at least 1\n");
}

// index.md 6: who holds a publisher name, and whether it is established
TEST(index_publisher, reads_a_publisher_file)
{
    diagnostics d;
    auto p = parse_publisher("github-id = 7\nkind = \"organization\"\nestablished = true\n", "publishers/o.toml", d);
    ASSERT_TRUE(p.has_value()) << messages(d);
    EXPECT_EQ(p->github_id, 7);
    EXPECT_TRUE(p->organization);
    EXPECT_TRUE(p->established);
    EXPECT_FALSE(parse_publisher("github-id = 7\nkind = \"user\"\n", "publishers/u.toml", d).value().established);
    diagnostics wrong;
    EXPECT_FALSE(parse_publisher("github-id = 7\nkind = \"user\"\nestablished = \"yes\"\n", "publishers/u.toml", wrong).has_value());

    // what otok publish registers, read back
    publisher_file user;
    user.github_id = 42;
    EXPECT_EQ(write_publisher(user), "github-id = 42\nkind = \"user\"\n");
    std::string const full = write_publisher({.github_id = 7, .organization = true, .established = true, .suspended = "spam \"x\""});
    EXPECT_EQ(full, "github-id = 7\nkind = \"organization\"\nestablished = true\nsuspended = 'spam \"x\"'\n");
    auto back = parse_publisher(full, "publishers/o.toml", d);
    ASSERT_TRUE(back.has_value()) << messages(d);
    EXPECT_EQ(back->suspended, "spam \"x\"");
}

TEST(index_publisher, knows_entries_by_their_path)
{
    EXPECT_EQ(entry_publisher("recipes/p/a/apple/1.0-r1.toml"), "p");
    EXPECT_EQ(entry_publisher("recipes/p/a/apple/1.0-r12.toml"), "p");
    EXPECT_EQ(entry_publisher("recipes/p/a/1.0-r1.toml"), std::nullopt);
    EXPECT_EQ(entry_publisher("recipes/p/a/apple/recipe-status.toml"), std::nullopt);
    EXPECT_EQ(entry_publisher("recipes/p/a/apple/1.0.toml"), std::nullopt);
    EXPECT_EQ(entry_publisher("publishers/p.toml"), std::nullopt);
}

TEST(index_view, generates_the_view_of_a_checkout)
{
    test::temp_dir dir;
    std::filesystem::path const index = make_test_index(dir);
    std::filesystem::path const out = dir.path() / "site";
    auto written = write_index_view(index, hex40, out, targets);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    EXPECT_EQ(written->written, 9u);  // config, recipes, the recipe, the library, advisories, availability, 3 Apple files

    // the example of the format document, for this index
    std::string example = json_block("index.md", "## 8. HTTPS view");
    ASSERT_FALSE(example.empty());
    example = replaced(replaced(example, "apotocki/openssl", "p/a-recipe"), "\"openssl\"", "\"a\"");
    EXPECT_EQ(read_file(out / "index" / "libraries" / "a.json").value_or("?"), example);
    EXPECT_EQ(read_file(out / "index" / "config.json").value_or("?"),
        "{\n  \"view-format\": 1,\n  \"commit\": \"" + hex40 + "\",\n  \"toml\": \"format = 1\\nrepository = \\\"https://github.com/o/index\\\"\\n\"\n}\n");
    EXPECT_EQ(read_file(out / "index" / "availability.json").value_or("?"), "{\n  \"view-format\": 1\n}\n");

    // read back as a client reads it: the same as the checkout
    recipe_index checkout{index, targets};
    recipe_index view{view_index_source(view_directory(out), "site"), targets};
    auto sorted = view.newest_first({"p", "a-recipe"});
    ASSERT_TRUE(sorted.has_value()) << sorted.error().message;
    ASSERT_EQ(sorted->size(), 2u);
    EXPECT_EQ((*sorted)[0]->projection.version->str(), "1.1");
    auto expected = checkout.entries({"p", "a-recipe"});
    ASSERT_TRUE(expected.has_value());
    for (index_entry const& e : **expected) {
        auto const same = std::ranges::find_if(*sorted, [&](index_entry const* v) { return *v->table == *e.table; });
        EXPECT_NE(same, sorted->end()) << e.projection.version->str();
    }
    EXPECT_EQ(view.status({"p", "a-recipe"}).value_or(recipe_status{}).deprecation(), "deprecated: old");
    EXPECT_EQ(view.advisories_of({"p", "a-recipe"}, "apple", "a", *version::parse("1.0"), 2).value_or(std::vector<candidate_advisory>{}).size(), 1u);
    auto summaries = view.summaries();
    ASSERT_TRUE(summaries.has_value()) << summaries.error().message;
    ASSERT_EQ(summaries->size(), 1u);
    EXPECT_EQ((*summaries)[0].id.str(), "p/a-recipe");
    EXPECT_EQ((*summaries)[0].version, "1.1");
    EXPECT_EQ((*summaries)[0].targets, std::vector<std::string>{"apple"});
    EXPECT_EQ((*summaries)[0].description, "Newer.");
    EXPECT_EQ((*summaries)[0].keywords, std::vector<std::string>{"test"});
    EXPECT_EQ(checkout.summaries().value_or(std::vector<recipe_summary>{}).size(), 1u);
    EXPECT_EQ(view.settings().value_or(index_settings{}).repository, "https://github.com/o/index");
    EXPECT_EQ(view.order("a").value_or(version_order::semver()).scheme(), version_scheme::dotted);
    auto missing = view.entries({"p", "zz"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message, "the index has no recipe 'p/zz'");

    // again: nothing rewritten, so the files keep their times (and their ETags); stale files go
    auto const time = std::filesystem::last_write_time(out / "index" / "config.json");
    dir.write("site/index/recipes/q/old.json", "{}");
    written = write_index_view(index, hex40, out, targets);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    EXPECT_EQ(written->written, 0u);
    EXPECT_EQ(written->unchanged, 9u);
    EXPECT_EQ(written->removed, 1u);
    EXPECT_EQ(std::filesystem::last_write_time(out / "index" / "config.json"), time);
    EXPECT_FALSE(std::filesystem::exists(out / "index" / "recipes" / "q"));

    // an index with an invalid file has no view
    dir.write("index/advisories/2026-0002.toml", advisory_text("library = \"a\"", "\"1.0\""));
    written = write_index_view(index, hex40, out, targets);
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().message.find("the advisory's id '2026-0001' does not match its file name"), std::string::npos)
        << written.error().message;
}

TEST(index_view, refuses_what_it_cannot_read)
{
    test::temp_dir dir;
    dir.write("site/index/config.json", "{\"view-format\": 2}");
    dir.write("site/index/recipes/p/a.json", "{\"view-format\": 1, \"id\": \"p/b\", \"entries\": []}");
    dir.write("site/index/recipes/p/c.json", "{\"view-format\": 1, \"id\": \"p/c\", \"entries\": [{\"file\": \"../x.toml\", \"toml\": \"\"}]}");
    dir.write("site/index/advisories.json", "[]");
    recipe_index view{view_index_source(view_directory(dir.path() / "site"), "site"), targets};
    auto settings = view.settings();
    ASSERT_FALSE(settings.has_value());
    EXPECT_EQ(settings.error().message, "site/index/config.json has view format 2, which needs a newer otok: this one reads format 1");
    auto other = view.entries({"p", "a"});
    ASSERT_FALSE(other.has_value());
    EXPECT_EQ(other.error().message, "site/index/recipes/p/a.json is of recipe 'p/b', not 'p/a'");
    auto outside = view.entries({"p", "c"});
    ASSERT_FALSE(outside.has_value());
    EXPECT_EQ(outside.error().message, "site/index/recipes/p/c.json: '../x.toml' is not the name of a TOML file in the directory of a variant");
    auto advisories = view.advisories();
    ASSERT_FALSE(advisories.has_value());
    EXPECT_EQ(advisories.error().message, "site/index/advisories.json is not a JSON object");
    auto availability = view.availability();
    ASSERT_FALSE(availability.has_value());
    EXPECT_EQ(availability.error().message, "the index view site has no index/availability.json");
}

TEST(index_view, revalidates_files_over_http)
{
    test::temp_dir dir;
    std::filesystem::path const index = make_test_index(dir);
    ASSERT_TRUE(write_index_view(index, hex40, dir.path() / "site", targets).has_value());
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    fake_view_server server;
    server.root = dir.path() / "site";

    // one request a file and command, kept with its ETag
    auto source = open_index_source("view+https://view.example.com/", "", *cache, nullptr, server.transport());
    ASSERT_TRUE(source.has_value()) << source.error().message;
    recipe_index first{std::move(*source), targets};
    ASSERT_TRUE(first.entries({"p", "a-recipe"}).has_value());
    ASSERT_TRUE(first.status({"p", "a-recipe"}).has_value());
    ASSERT_EQ(server.requests.size(), 1u);
    EXPECT_EQ(server.requests[0].url, "https://view.example.com/index/recipes/p/a-recipe.json");
    EXPECT_FALSE(server.conditional(0));
    std::filesystem::path const cached = dir.path() / "cache" / "view" / git_url_key("https://view.example.com") / "recipes" / "p" / "a-recipe.json";
    EXPECT_EQ(read_file(cached).value_or("?"), read_file(dir.path() / "site" / "index" / "recipes" / "p" / "a-recipe.json").value_or("!"));
    std::filesystem::path etag = cached;
    etag += ".etag";
    EXPECT_TRUE(std::filesystem::exists(etag));

    // the next command revalidates: 304, the cached file
    auto again = view_over_http("https://view.example.com", *cache, server.transport());
    auto text = again("recipes/p/a-recipe.json");
    ASSERT_TRUE(text.has_value()) << text.error().message;
    EXPECT_EQ(text->value_or("?"), read_file(cached).value_or("!"));
    ASSERT_EQ(server.requests.size(), 2u);
    EXPECT_TRUE(server.conditional(1));

    // changed on the server: fetched whole; gone: removed from the cache
    dir.write("site/index/recipes/p/a-recipe.json", "{\"view-format\": 1}");
    auto changed = view_over_http("https://view.example.com", *cache, server.transport());
    EXPECT_EQ(changed("recipes/p/a-recipe.json").value_or(std::nullopt).value_or("?"), "{\"view-format\": 1}");
    EXPECT_EQ(read_file(cached).value_or("?"), "{\"view-format\": 1}");
    EXPECT_EQ(changed("recipes/p/zz.json").value_or("?"), std::nullopt);
    std::filesystem::remove(dir.path() / "site" / "index" / "recipes" / "p" / "a-recipe.json");
    auto gone = view_over_http("https://view.example.com", *cache, server.transport());
    EXPECT_EQ(gone("recipes/p/a-recipe.json").value_or("?"), std::nullopt);
    EXPECT_FALSE(std::filesystem::exists(cached));
    EXPECT_FALSE(std::filesystem::exists(etag));

    server.status = 500;
    auto failing = view_over_http("https://view.example.com", *cache, server.transport());
    auto error = failing("recipes.json");
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().message, "GET https://view.example.com/index/recipes.json: HTTP status 500");

    // offline: the cached files only
    server.status = 0;
    ASSERT_TRUE(view_over_http("https://view.example.com", *cache, server.transport())("config.json").has_value());
    std::size_t const asked = server.requests.size();
    cache->set_offline(true);
    auto offline = view_over_http("https://view.example.com", *cache, server.transport());
    EXPECT_TRUE(offline("config.json").value_or(std::nullopt).has_value());
    auto uncached = offline("advisories.json");
    ASSERT_FALSE(uncached.has_value());
    EXPECT_EQ(uncached.error().message, "offline, and https://view.example.com/index/advisories.json is not in the cache; read the index online once");
    EXPECT_EQ(server.requests.size(), asked);
}

TEST(index_view, opens_the_index_config_names)
{
    test::temp_dir dir;
    std::filesystem::path const index = make_test_index(dir);
    ASSERT_TRUE(write_index_view(index, hex40, dir.path() / "site", targets).has_value());
    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    fake_view_server server;
    server.root = dir.path() / "site";
    auto const summary_of = [&](std::string const& url, std::string const& mirror) -> std::string {
        auto source = open_index_source(url, mirror, *cache, nullptr, server.transport());
        if (!source) return source.error().message;
        recipe_index idx{std::move(*source), targets};
        auto list = idx.summaries();
        return list ? list->front().description : list.error().message;
    };
    EXPECT_EQ(summary_of("view+https://view.example.com", ""), "Newer.");
    EXPECT_EQ(summary_of("", "https://view.example.com/"), "Newer.");  // a mirror of the view
    EXPECT_EQ(summary_of("", path_to_utf8(dir.path() / "site")), "Newer.");  // a view on disk
    std::size_t const asked = server.requests.size();
    EXPECT_EQ(summary_of("view+https://elsewhere.example.com", path_to_utf8(index)), "Newer.");  // a checkout
    EXPECT_EQ(server.requests.size(), asked);
    EXPECT_EQ(summary_of("view+http://view.example.com", ""), "the index view 'http://view.example.com' must be an https URL");
    EXPECT_EQ(view_base("view+https://a.example.com/x/"), "https://a.example.com/x");
    EXPECT_EQ(view_base("https://github.com/o/index"), "");
}

// Where a recipe id is taken from (otok-toml.md 6.4, docs/PLAN.md decision 95): [index-of], the owner of the publisher,
// else the one index that has it; two are an error, equal or not.
TEST(index_set, locates_recipes_in_the_indexes_of_a_project)
{
    test::temp_dir dir;
    for (std::string const recipe : {"default/recipes/p/a", "default/recipes/p/both", "corp/recipes/corp/c", "corp/recipes/p/both",
             "corp/recipes/corp/own"}) {
        std::filesystem::create_directories(dir.path() / path_from_utf8(recipe));
    }
    auto const open = [&](std::string const& name) {
        return [root = dir.path() / name]() -> result<std::unique_ptr<index_source>> { return directory_index_source(root); };
    };
    auto const set = [&](std::map<std::string, std::string, std::less<>> index_of, std::vector<std::string> publishers = {}) {
        return index_set{{{"default", open("default"), {}}, {"corp", open("corp"), std::move(publishers)}}, std::move(index_of), "", targets};
    };
    auto const where = [](index_set& s, std::string_view id) -> std::string {
        auto l = s.locate(*parse_recipe_id(id));
        return l ? l->name : "error: " + l.error().message;
    };

    index_set plain = set({});
    EXPECT_EQ(where(plain, "p/a"), "default");
    EXPECT_EQ(where(plain, "corp/c"), "corp");
    EXPECT_EQ(where(plain, "p/both"), "error: the recipe 'p/both' is in the indexes 'default', 'corp'; choose one in [index-of] of otok.toml, "
                                      "as \"p/both\" = 'default' (otok-toml.md 6.4)");
    EXPECT_EQ(where(plain, "p/none"), "error: no index of the project has the recipe 'p/none'");

    index_set chosen = set({{"p/both", "corp"}, {"p/a", "corp"}});
    EXPECT_EQ(where(chosen, "p/both"), "corp");
    EXPECT_EQ(where(chosen, "p/a"), "error: the index 'corp' has no recipe 'p/a'");  // [index-of] is the only place looked at

    // an owner index is the only one looked at for its publishers, and others still find the rest there
    index_set owned = set({}, {"p"});
    EXPECT_EQ(where(owned, "p/both"), "corp");
    EXPECT_EQ(where(owned, "p/a"), "error: the index 'corp' has no recipe 'p/a'");
    EXPECT_EQ(where(owned, "corp/c"), "corp");

    // without `default` (index = "" in config.toml)
    index_set alone{{{"corp", open("corp"), {}}}, {}, "no recipe index: 'index' in config.toml (or OTOK_INDEX) is empty", targets};
    EXPECT_EQ(where(alone, "corp/c"), "corp");
    EXPECT_EQ(where(alone, "p/a"), "error: the index 'corp' has no recipe 'p/a'");
    EXPECT_EQ(alone.get("default").error().message, "no recipe index: 'index' in config.toml (or OTOK_INDEX) is empty");
    index_set none{{}, {}, "no recipe index: 'index' in config.toml (or OTOK_INDEX) is empty", targets};
    EXPECT_EQ(where(none, "p/a"), "error: no recipe index: 'index' in config.toml (or OTOK_INDEX) is empty");

    // an index that cannot be opened stops the search: it might have the recipe
    index_set broken{{{"default", open("default"), {}}, {"down", [] { return result<std::unique_ptr<index_source>>{fail("unreachable")}; }, {}}}, {}, "", targets};
    EXPECT_EQ(where(broken, "p/a"), "error: the index 'down': unreachable");
    EXPECT_EQ(broken.names(), (std::vector<std::string>{"default", "down"}));
}

// docs/PLAN.md decision 98, index.md 3.2: an entry of a newer otok's platforms, environments and targets is read with the
// names this otok knows; index CI refuses the names it does not know
TEST(index_entry, takes_names_of_a_newer_otok_as_never_matching)
{
    std::string recipe = replaced(recipe_text(), "supported = [\"ios\", \"macosx\"]\nbuild-on = [\"macos/xcode\"]\n",
        "supported = [\"ios\", \"macosx\", \"newos\"]\nbuild-on = [\"macos/xcode\", \"macos/newcc\"]\n"
        "architectures = { ios = [\"arm64\", \"arm128\"], newos = [\"arm64\"] }\n");
    recipe = replaced(recipe, "when = { option = \"zlib\" }", "when = { option = \"zlib\", platform = [\"ios\", \"newos\"] }");
    recipe = replaced(recipe, "[apple]\n", "[newtarget]\nkey = 1\n[apple]\n");
    std::string const text = write_index_entry(projection_of(recipe), test_recipe_source(), hex64, published);
    EXPECT_NE(text.find("[newtarget]"), std::string::npos) << text;

    diagnostics d;
    auto e = parse_index_entry(text, "entry.toml", targets, d, index_reading::client);
    ASSERT_TRUE(e.has_value()) << messages(d);
    EXPECT_EQ(d.items().size(), 0u) << messages(d);
    EXPECT_EQ(e->projection.platforms, (std::vector<std::string>{"ios", "macosx"}));
    EXPECT_EQ(e->projection.architectures.at("ios"), std::vector<std::string>{"arm64"});
    EXPECT_FALSE(e->projection.architectures.contains("newos"));
    ASSERT_EQ(e->projection.build_on.size(), 1u);
    EXPECT_EQ(e->projection.build_on[0].text(), "macos/xcode");
    ASSERT_EQ(e->projection.dependencies.size(), 1u);
    EXPECT_EQ(e->projection.dependencies[0].when->platform, (std::vector<std::string>{"ios", "newos"}));

    diagnostics strict;
    EXPECT_FALSE(parse_index_entry(text, "entry.toml", targets, strict).has_value());
    for (std::string const expected : {"unknown platform 'newos' in", "architecture 'arm128' is not available for platform 'ios'",
             "environment 'macos/newcc' in", "unknown key 'newtarget' at the top level"}) {
        EXPECT_NE(messages(strict).find(expected), std::string::npos) << expected << "\n" << messages(strict);
    }
}

// index.md 3.2: entries a client cannot read are not there for it
TEST(recipe_index, leaves_aside_entries_it_cannot_read)
{
    test::temp_dir dir;
    dir.write("recipe/recipe.toml", recipe_text());
    dir.write("recipe/fix.patch", "--- a\n+++ b\n");
    dir.write("recipe/PrivacyInfo.xcprivacy", privacy_manifest);
    std::filesystem::path const index = dir.path() / "index";
    auto added = publish_entry(index, dir.path() / "recipe", test_recipe_source(), "p");
    ASSERT_TRUE(added.has_value()) << added.error().message;
    std::string const readable = read_file(index / path_from_utf8(*added)).value_or("");
    auto const newer = [&](std::string const& file, std::string const& text) { dir.write("index/recipes/p/a-recipe/" + file, text); };
    newer("apple/1.1-r1.toml", replaced(replaced(replaced(readable, "version = \"1.0\"", "version = \"1.1\""), "revision = 2", "revision = 1"),
        "api = 1", "api = 2"));
    newer("apple/1.2-r1.toml", replaced(replaced(replaced(readable, "version = \"1.0\"", "version = \"1.2\""), "revision = 2", "revision = 1"),
        "format = 1", "format = 2"));
    newer("newos/1.0-r2.toml", replaced(replaced(readable, "variant = \"apple\"", "variant = \"newos\""), "\"ios\", \"macosx\"", "\"newos\""));

    recipe_index idx{index, targets};
    auto entries = idx.entries({"p", "a-recipe"});
    ASSERT_TRUE(entries.has_value()) << entries.error().message;
    ASSERT_EQ((*entries)->size(), 1u);
    EXPECT_EQ((**entries)[0].file(), "apple/1.0-r2.toml");
    auto const& skipped = idx.unreadable({"p", "a-recipe"});
    ASSERT_EQ(skipped.size(), 3u);
    EXPECT_EQ(skipped[0].label(), "1.1-r1 (apple)");
    EXPECT_NE(skipped[0].reason.find("recipe API 2 needs a newer otok: this one reads API 1"), std::string::npos) << skipped[0].reason;
    EXPECT_EQ(skipped[1].label(), "1.2-r1 (apple)");
    EXPECT_NE(skipped[1].reason.find("index entry format 2 needs a newer otok: this one reads format 1"), std::string::npos) << skipped[1].reason;
    EXPECT_EQ(skipped[2].label(), "1.0-r2 (newos)");
    EXPECT_EQ(skipped[2].reason, "the entry has no platform this otok knows; it needs a newer otok");

    // what the resolver is told of them
    project_recipe_provider local{dir.path(), targets};
    index_recipe_provider provider{local, default_only([&] { return result<std::unique_ptr<index_source>>{directory_index_source(index)}; }, targets),
        targets, nullptr, std::nullopt};
    recipe_ref ref;
    ref.source = recipe_ref::kind::index;
    ref.id = {"p", "a-recipe"};
    auto list = provider.candidates("a", ref);
    ASSERT_TRUE(list.has_value()) << list.error().message;
    EXPECT_EQ(list->size(), 1u);
    auto const told = provider.unreadable("a");
    ASSERT_EQ(told.size(), 3u);
    EXPECT_TRUE(told[0].starts_with("1.1-r1 (apple): ")) << told[0];

    // index CI refuses them
    recipe_index strict{index, targets, index_reading::ci};
    EXPECT_FALSE(strict.entries({"p", "a-recipe"}).has_value());

    // none left
    std::filesystem::remove(index / path_from_utf8(*added));
    recipe_index none{index, targets};
    auto nothing = none.entries({"p", "a-recipe"});
    ASSERT_FALSE(nothing.has_value());
    EXPECT_NE(nothing.error().message.find("the index has no version of recipe 'p/a-recipe' that this otok can read; a newer otok may read them:\n"
        "  1.1-r1 (apple): "), std::string::npos) << nothing.error().message;
}

TEST(index_library, needs_a_newer_otok_for_an_unknown_scheme)
{
    diagnostics d;
    EXPECT_FALSE(parse_library_file("scheme = \"calendar\"\nnext = 1\n", "a.toml", d, index_reading::client));
    EXPECT_EQ(messages(d), "a.toml:1:10: error: the version scheme 'calendar' needs a newer otok\n");
}

// index.md 7.2
TEST(index_advisory, acts_at_least_as_much_as_it_means)
{
    std::string const text = advisory_text("library = \"a\"", "\"1.0\"", "disputed") + "platforms = [\"ios\"]\n";
    std::string const newer = replaced(text, "\"withdrawn\"", "\"malicious\"");
    diagnostics d;
    auto a = parse_advisory(newer, "a.toml", d, index_reading::client);
    ASSERT_TRUE(a.has_value()) << messages(d);
    EXPECT_EQ(a->severity, advisory_severity::security);
    EXPECT_EQ(a->status, advisory_status::confirmed);
    EXPECT_TRUE(a->active());
    EXPECT_EQ(a->unknown, (std::vector<std::string>{"severity 'malicious' is unknown to this otok and taken as 'security'",
                              "status 'disputed' is unknown to this otok and taken as 'confirmed'"}));

    diagnostics strict;
    EXPECT_FALSE(parse_advisory(newer, "a.toml", strict).has_value());
    EXPECT_NE(messages(strict).find("unknown key 'platforms'"), std::string::npos) << messages(strict);

    // one it cannot read: what it concerns is unknown
    test::temp_dir dir;
    dir.write("index/advisories/2026-0001.toml", newer);
    dir.write("index/advisories/2026-0002.toml", replaced(replaced(text, "2026-0001", "2026-0002"), "versions = [\"1.0\"]", "versions = 1"));
    recipe_index idx{dir.path() / "index", targets};
    auto all = idx.advisories();
    ASSERT_TRUE(all.has_value()) << all.error().message;
    ASSERT_EQ((*all)->size(), 1u);
    ASSERT_EQ(idx.unreadable_advisories().size(), 1u);
    EXPECT_TRUE(idx.unreadable_advisories()[0].starts_with("advisories/2026-0002.toml: ")) << idx.unreadable_advisories()[0];
    recipe_index ci{dir.path() / "index", targets, index_reading::ci};
    EXPECT_FALSE(ci.advisories().has_value());
}

// index.md 1: the mutable files of an index, read without the check of unknown keys by a client
TEST(index_files, take_keys_of_a_newer_otok)
{
    diagnostics d;
    EXPECT_TRUE(parse_index_settings("format = 1\nmirrors = [\"https://example.com\"]\n", "index.toml", d, index_reading::client).has_value());
    EXPECT_TRUE(parse_recipe_status("[deprecated]\nreason = \"old\"\nsince = 1\n[moved]\nto = 1\n", "s.toml", d, index_reading::client).has_value());
    EXPECT_TRUE(parse_publisher("github-id = 7\nkind = \"user\"\nverified = true\n", "p.toml", d, index_reading::client).has_value());
    EXPECT_TRUE(parse_availability("checked = 2026-11-02T03:00:00Z\nprobe = 2\n", "availability.toml", d, index_reading::client).has_value());
    EXPECT_FALSE(d.has_errors()) << messages(d);

    diagnostics strict;
    EXPECT_FALSE(parse_index_settings("format = 1\nmirrors = [\"https://example.com\"]\n", "index.toml", strict).has_value());
    EXPECT_FALSE(parse_recipe_status("[deprecated]\nreason = \"old\"\nsince = 1\n", "s.toml", strict).has_value());
    EXPECT_FALSE(parse_publisher("github-id = 7\nkind = \"user\"\nverified = true\n", "p.toml", strict).has_value());
    EXPECT_FALSE(parse_availability("checked = 2026-11-02T03:00:00Z\nprobe = 2\n", "availability.toml", strict).has_value());
}
