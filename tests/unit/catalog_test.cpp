// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "otok/cache/store.hpp"
#include "otok/catalog/catalog.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"
#include "otok/fetch/git.hpp"
#include "otok/fetch/http.hpp"
#include "otok/index/index.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

target_info const* const targets[] = {&target::apple::info(), &test::native_info()};

std::string const hex64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
std::string const other64 = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
std::string const privacy_manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict/></plist>\n";
toml::date_time const published{toml::date{2026, 10, 26}, toml::time{12, 30, 0}, toml::time_offset{}};

// Recipe files, and their SHA-256 (Python hashlib).
std::string const old_patch = "--- a\n+++ b\n";
std::string const old_patch_sha256 = "6e53bf2ad8f234c60294a05a013874a211fe3c03653f48df43ac4ec085ab9a24";
std::string const new_patch = "--- a\n+++ b\n@@ -1 +1 @@\n-x\n+<y>\n";
std::string const new_patch_sha256 = "25654b67fdcf3ee5738e6aa454ee5806818b70209feef76e977bdd9506a94afe";

std::string a_recipe(std::string const& version, std::string const& description, std::string const& source_sha256)
{
    return "[recipe]\napi = 1\nlibrary = \"a\"\nname = \"a-recipe\"\nversion = \"" + version + "\"\nrevision = 1\nlicense = \"MIT\"\n"
        "license-files = [\"LICENSE\"]\n"
        "description = \"" + description + "\"\nhomepage = \"https://example.com/a\"\ndocumentation = \"https://example.com/a/docs\"\n"
        "keywords = [\"compression\"]\n"
        "[source]\nurl = \"https://example.com/a.tar.gz\"\nsha256 = \"" + source_sha256 + "\"\npatches = [\"fix.patch\"]\n"
        "[platforms]\nsupported = [\"macosx\"]\nbuild-on = [\"macos/xcode\"]\n"
        "[options.zlib]\ntype = \"bool\"\ndefault = false\ndoc = \"Use <zlib>.\"\n"
        "[deps.zlib]\nversion = \"~> 1.3\"\ndefault-recipe = \"p/zlib\"\nwhen = { option = \"zlib\" }\n"
        "[[product]]\nname = \"A\"\nlibraries = [\"a\"]\napple = { privacy-manifest = \"PrivacyInfo.xcprivacy\" }\n";
}

std::string const b_recipe = "[recipe]\napi = 1\nlibrary = \"b\"\nname = \"b-recipe\"\nversion = \"2.0\"\nrevision = 1\nlicense = \"MIT\"\n"
    "license-files = [\"LICENSE\"]\ndescription = \"B.\"\n[source]\nurl = \"https://example.com/b.tar.gz\"\nsha256 = \"" + hex64 + "\"\n"
    "[platforms]\nsupported = [\"native\"]\nbuild-on = [\"*/cc\"]\n[[product]]\nname = \"B\"\nlibraries = [\"b\"]\n";

std::string git_in(git_tool const& git, std::filesystem::path const& dir, std::vector<std::string> args)
{
    std::vector<std::string> full{"-c", "user.name=otok test", "-c", "user.email=test@otok.invalid", "-c", "core.autocrlf=false"};
    full.insert(full.end(), args.begin(), args.end());
    auto out = run_git(git, full, dir, dir.parent_path() / "git.log", true);
    EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message);
    std::string text = out.value_or("");
    return text.substr(0, text.find_first_of("\r\n"));
}

void add_entry(std::filesystem::path const& index, std::filesystem::path const& recipe_dir, locked_location const& source)
{
    auto addition = make_index_entry(recipe_dir, source, "p", published, targets);
    ASSERT_TRUE(addition.has_value()) << addition.error().message;
    auto added = add_index_entry(index, *addition, targets);
    ASSERT_TRUE(added.has_value()) << added.error().message;
}

bool has(std::string const& text, std::string const& part)
{
    return text.find(part) != std::string::npos;
}

}

TEST(catalog, embeds_the_files_of_the_site)
{
    auto const files = catalog_site_files();
    auto const named = [&](std::string_view path) { return std::ranges::find(files, path, &embedded_file::path) != files.end(); };
    EXPECT_TRUE(named("templates/base.html"));
    EXPECT_TRUE(named("templates/entry.html"));
    EXPECT_TRUE(named("assets/style.css"));
    EXPECT_TRUE(std::ranges::is_sorted(files, {}, &embedded_file::path));
    for (embedded_file const& f : files) EXPECT_EQ(f.contents.find('\r'), std::string_view::npos) << f.path;  // LF on every checkout
}

TEST(catalog, generates_the_site_of_a_checkout)
{
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    test::temp_dir dir;
    std::filesystem::path const repo = dir.path() / "recipes";
    std::filesystem::path const index = dir.path() / "index";

    // two versions of p/a-recipe at two commits of one repository
    dir.write("recipes/otok/recipe.toml", a_recipe("1.0", "Old.", hex64));
    dir.write("recipes/otok/fix.patch", old_patch);
    dir.write("recipes/otok/PrivacyInfo.xcprivacy", privacy_manifest);
    git_in(*git, repo, {"init", "-q"});
    git_in(*git, repo, {"add", "."});
    git_in(*git, repo, {"commit", "-q", "-m", "1.0"});
    locked_location source{.type = locked_location::kind::git, .git = file_url(repo), .commit = git_in(*git, repo, {"rev-parse", "HEAD"}), .path = "otok"};
    add_entry(index, repo / "otok", source);
    dir.write("recipes/otok/recipe.toml", a_recipe("1.1", "A <test> recipe.", other64));
    dir.write("recipes/otok/fix.patch", new_patch);
    git_in(*git, repo, {"commit", "-q", "-a", "-m", "1.1"});
    source.commit = git_in(*git, repo, {"rev-parse", "HEAD"});
    add_entry(index, repo / "otok", source);
    // p/b-recipe from a repository that does not exist
    dir.write("b/recipe.toml", b_recipe);
    add_entry(index, dir.path() / "b", {.type = locked_location::kind::git, .git = file_url(dir.path() / "missing"), .commit = source.commit});
    dir.write("index/index.toml", "format = 1\nname = \"Test index\"\n");
    dir.write("index/libraries/c.toml", "scheme = \"dotted\"\n");
    dir.write("index/advisories/2026-0001.toml", "id = \"2026-0001\"\nrecipes = [\"p/a-recipe\"]\nversions = [\"1.0-r1\"]\n"
        "severity = \"withdrawn\"\nstatus = \"confirmed\"\nsummary = \"Broken <build>.\"\npublished = 2026-11-02T10:00:00Z\n");

    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    std::filesystem::path const out = dir.path() / "site";
    dir.write("site/robots.txt", "not the catalog's\n");
    dir.write("site/index/config.json", "{}\n");
    dir.write("site/recipes/p/gone/index.html", "stale\n");
    catalog_settings settings;
    settings.root = index;
    settings.commit = source.commit;
    settings.output = out;
    settings.targets = targets;
    settings.cache = &*cache;
    settings.git = &*git;
    auto written = write_index_catalog(settings);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    ASSERT_EQ(written->unavailable.size(), 1u);
    EXPECT_TRUE(has(written->unavailable[0], "cannot fetch the recipe p/b-recipe 2.0 (r1, native)")) << written->unavailable[0];
    // front, search.json, the feed, /libraries/ and 3 libraries, advisories, 2 recipes with entries.json, 3 entries;
    // 5 contents of recipe files; 6 assets
    EXPECT_EQ(written->files.written, 26u);
    EXPECT_EQ(written->files.removed, 1u);
    EXPECT_FALSE(std::filesystem::exists(out / "recipes" / "p" / "gone"));
    EXPECT_TRUE(std::filesystem::exists(out / "robots.txt"));
    EXPECT_TRUE(std::filesystem::exists(out / "index" / "config.json"));

    EXPECT_EQ(read_file(out / "search.json").value_or("?"),
        "[{\"id\":\"p/a-recipe\",\"library\":\"a\",\"description\":\"A <test> recipe.\",\"keywords\":[\"compression\"],"
        "\"platforms\":[\"macosx\"],\"targets\":[\"apple\"],\"license\":\"MIT\","
        "\"variants\":[{\"target\":\"apple\",\"version\":\"1.1 (r1)\",\"href\":\"recipes/p/a-recipe/apple/1.1-r1/\"}]},"
        "{\"id\":\"p/b-recipe\",\"library\":\"b\",\"description\":\"B.\",\"keywords\":[],\"platforms\":[\"native\"],\"targets\":[\"native\"],"
        "\"license\":\"MIT\",\"variants\":[{\"target\":\"native\",\"version\":\"2.0 (r1)\",\"href\":\"recipes/p/b-recipe/native/2.0-r1/\"}]}]\n");
    // the recipe files by content
    EXPECT_EQ(read_file(out / "files" / (old_patch_sha256 + ".txt")).value_or("?"), old_patch);
    EXPECT_EQ(read_file(out / "files" / (new_patch_sha256 + ".txt")).value_or("?"), new_patch);

    std::string const front = read_file(out / "index.html").value_or("");
    EXPECT_TRUE(has(front, "<title>Test index · Test index</title>")) << front;
    EXPECT_TRUE(has(front, "3 libraries, 2 recipes. Each recipe builds a library from source on your machine, or delivers its Swift package.")) << front;
    // the versions published last: at one time here, so by id and version
    // with their targets, for the filter by target
    EXPECT_TRUE(has(front, "<tbody id=\"recent\">\n<tr data-target=\"apple\">\n<td><a href=\"libraries/a/\">a</a></td>\n"
        "<td><a href=\"recipes/p/a-recipe/apple/1.0-r1/\">p/a-recipe 1.0 (r1, apple)</a></td>\n<td>apple</td>\n"
        "<td>2026-10-26</td>\n</tr>\n<tr data-target=\"apple\">\n<td><a href=\"libraries/a/\">a</a></td>\n<td><a href=\"recipes/p/a-recipe/apple/1.1-r1/\">")) << front;
    // assets by their hash (sha256_hex is tested on its own): a cache never pairs a page with an older script
    auto const versioned = [&](std::string const& name) {
        return "assets/" + name + "?v=" + sha256_hex(read_file(out / "assets" / name).value_or("")).substr(0, 12);
    };
    EXPECT_TRUE(has(front, "<link rel=\"stylesheet\" href=\"" + versioned("style.css") + "\">")) << front;
    // the filter by target (docs/PLAN.md decision 82)
    EXPECT_TRUE(has(front, "<option value=\"apple\">apple</option>\n<option value=\"native\">native</option>\n</select>")) << front;
    EXPECT_TRUE(has(front, "<script src=\"" + versioned("minisearch.js") + "\"></script>")) << front;
    std::string const all = read_file(out / "libraries" / "index.html").value_or("");
    EXPECT_TRUE(has(all, "<p class=\"letters\"><a href=\"#A\">A</a><a href=\"#B\">B</a><a href=\"#C\">C</a></p>")) << all;
    EXPECT_TRUE(has(all, "<li><a href=\"a/\">a</a> <span class=\"meta\">1 recipe</span></li>")) << all;
    EXPECT_TRUE(has(all, "<li><a href=\"c/\">c</a> <span class=\"meta\">0 recipes</span></li>")) << all;
    // what the browser compares versions with
    std::string const entries = read_file(out / "recipes" / "p" / "a-recipe" / "entries.json").value_or("");
    EXPECT_TRUE(entries.starts_with("{\"entries\":[{\"dir\":\"apple/1.1-r1\",\"label\":\"1.1 (r1, apple)\",\"files\":{\"PrivacyInfo.xcprivacy\":\"")) << entries;
    EXPECT_TRUE(has(entries, "\"fix.patch\":\"" + new_patch_sha256 + "\"")) << entries;
    EXPECT_TRUE(has(entries, "\"fix.patch\":\"" + old_patch_sha256 + "\"")) << entries;
    EXPECT_EQ(read_file(out / "recipes" / "p" / "b-recipe" / "entries.json").value_or(""),
        "{\"entries\":[{\"dir\":\"native/2.0-r1\",\"label\":\"2.0 (r1, native)\",\"files\":null}]}\n");

    std::string const recipe_page = read_file(out / "recipes" / "p" / "a-recipe" / "index.html").value_or("");
    EXPECT_TRUE(has(recipe_page, "<link rel=\"stylesheet\" href=\"../../../" + versioned("style.css") + "\">")) << recipe_page;
    EXPECT_TRUE(has(recipe_page, "[deps.a]\nrecipe = &quot;p/a-recipe&quot;\nversion = &quot;~&gt; 1.1&quot;\n")) << recipe_page;
    EXPECT_TRUE(has(recipe_page, "<a href=\"https://example.com/a\">")) << recipe_page;
    EXPECT_TRUE(has(recipe_page, "<a href=\"https://example.com/a/docs\">")) << recipe_page;
    EXPECT_TRUE(has(recipe_page, "<td><a href=\"apple/1.0-r1/\">1.0 (r1, apple)</a></td>\n<td>apple</td>")) << recipe_page;
    EXPECT_TRUE(has(recipe_page, "<span class=\"badge bad\">withdrawn</span>")) << recipe_page;
    EXPECT_TRUE(has(recipe_page, "<td>Use &lt;zlib&gt;.</td>")) << recipe_page;
    EXPECT_TRUE(has(recipe_page, "<code>when = { option = &quot;zlib&quot; }</code>")) << recipe_page;

    std::string const newer = read_file(out / "recipes" / "p" / "a-recipe" / "apple" / "1.1-r1" / "index.html").value_or("");
    EXPECT_TRUE(has(newer, "<pre><code>--- a\n+++ b\n@@ -1 +1 @@\n-x\n+&lt;y&gt;\n</code></pre>")) << newer;
    EXPECT_TRUE(has(newer, "<a href=\"../../../../../files/" + new_patch_sha256 + ".txt\">raw</a>")) << newer;
    EXPECT_TRUE(has(newer, "<dt>Previous</dt><dd><a href=\"../../apple/1.0-r1/\">1.0 (r1, apple)</a></dd>")) << newer;
    EXPECT_TRUE(has(newer, "<tr><th>source.sha256</th><td><code>" + other64 + "</code></td></tr>")) << newer;
    // the changes since 1.0: the inputs as data, the files by their hashes
    EXPECT_TRUE(has(newer, "<h2>Changes since 1.0 (r1, apple)</h2>")) << newer;
    EXPECT_TRUE(has(newer, "<tr class=\"changed\"><th>source.sha256</th><td><code>&quot;" + hex64 + "&quot;</code></td><td><code>&quot;"
        + other64 + "&quot;</code></td></tr>")) << newer;
    EXPECT_FALSE(has(newer, "<tr class=\"changed\"><th>description</th>")) << newer;
    EXPECT_TRUE(has(newer, "<div id=\"diff\" data-root=\"../../../../../\" data-entries=\"../../entries.json\" data-current=\"apple/1.1-r1\" data-base=\"apple/1.0-r1\">")) << newer;
    EXPECT_TRUE(has(newer, "<li><code>fix.patch</code> <span class=\"meta\">changed</span></li>\n"
        "<li><code>recipe.toml</code> <span class=\"meta\">changed</span></li>\n</ul>\n<p class=\"meta\">1 file is the same as in 1.0 (r1, apple).</p>"))
        << newer;
    EXPECT_TRUE(has(newer, "<script src=\"../../../../../" + versioned("diff.min.js") + "\"></script>")) << newer;
    EXPECT_TRUE(has(newer, "<dt>Published</dt><dd>2026-10-26</dd>")) << newer;
    EXPECT_TRUE(has(newer, "<dt>Variant</dt><dd>apple, target apple</dd>")) << newer;
    EXPECT_TRUE(has(newer, "<p class=\"crumbs\"><a href=\"../../\">p/a-recipe</a></p>")) << newer;
    std::string const older = read_file(out / "recipes" / "p" / "a-recipe" / "apple" / "1.0-r1" / "index.html").value_or("");
    EXPECT_TRUE(has(older, "This version is withdrawn")) << older;
    EXPECT_TRUE(has(older, "2026-0001</a> (withdrawn): Broken &lt;build&gt;.")) << older;
    std::string const missing = read_file(out / "recipes" / "p" / "b-recipe" / "native" / "2.0-r1" / "index.html").value_or("");
    EXPECT_TRUE(has(missing, "The recipe source is unavailable: it cannot be fetched from " + file_url(dir.path() / "missing") + " at "
        + source.commit + ", or it does not match the recipe-sha256 of its entry.</p>")) << missing;

    std::string const library = read_file(out / "libraries" / "a" / "index.html").value_or("");
    EXPECT_TRUE(has(library, "<td><a href=\"../../recipes/p/a-recipe/\">p/a-recipe</a></td>")) << library;
    EXPECT_TRUE(has(read_file(out / "libraries" / "c" / "index.html").value_or(""), "No recipe builds this library yet."));
    std::string const advisories = read_file(out / "advisories" / "index.html").value_or("");
    EXPECT_TRUE(has(advisories, "<article class=\"advisory\" id=\"2026-0001\">")) << advisories;
    EXPECT_TRUE(has(advisories, "<dt>Published</dt><dd>2026-11-02</dd>")) << advisories;
    std::string const feed = read_file(out / "advisories.atom").value_or("");
    EXPECT_TRUE(feed.starts_with("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<feed xmlns=\"http://www.w3.org/2005/Atom\">\n"
        "<title>Advisories of Test index</title>\n<id>urn:otok:advisories</id>\n")) << feed;
    EXPECT_TRUE(has(feed, "<updated>2026-11-02T10:00:00Z</updated>\n<author>")) << feed;
    EXPECT_TRUE(has(feed, "<entry>\n<id>urn:otok:advisory:2026-0001</id>\n<title>2026-0001 (withdrawn, confirmed): Broken &lt;build&gt;.</title>\n"
        "<link rel=\"alternate\" type=\"text/html\" href=\"advisories/#2026-0001\"/>\n<published>2026-11-02T10:00:00Z</published>\n"
        "<updated>2026-11-02T10:00:00Z</updated>\n<summary>p/a-recipe 1.0-r1: Broken &lt;build&gt;.</summary>\n</entry>")) << feed;

    // the same again: nothing written
    auto again = write_index_catalog(settings);
    ASSERT_TRUE(again.has_value()) << again.error().message;
    EXPECT_EQ(again->files.written, 0u);
    EXPECT_EQ(again->files.removed, 0u);
    EXPECT_EQ(again->files.unchanged, 26u);
}

TEST(catalog, lists_the_versions_published_last_of_each_target)
{
    test::temp_dir dir;
    std::filesystem::path const index = dir.path() / "index";
    locked_location const missing{.type = locked_location::kind::git, .git = file_url(dir.path() / "missing"), .commit = hex64.substr(0, 40)};
    auto const publish = [&](std::string const& text, int day, int hour) {
        dir.write("recipe/recipe.toml", text);
        toml::date_time const at{toml::date{2026, 10, static_cast<std::uint8_t>(day)}, toml::time{static_cast<std::uint8_t>(hour), 0, 0}, toml::time_offset{}};
        auto addition = make_index_entry(dir.path() / "recipe", missing, "p", at, targets);
        ASSERT_TRUE(addition.has_value()) << addition.error().message;
        auto added = add_index_entry(index, *addition, targets);
        ASSERT_TRUE(added.has_value()) << added.error().message;
    };
    // one apple version, older than 31 native ones
    dir.write("recipe/fix.patch", old_patch);
    dir.write("recipe/PrivacyInfo.xcprivacy", privacy_manifest);
    publish(a_recipe("1.0", "A.", hex64), 1, 0);
    for (int i = 0; i < 31; ++i) {
        std::string text = b_recipe;
        text.replace(text.find("version = \"2.0\""), 15, "version = \"2." + std::to_string(i) + "\"");
        publish(text, 2 + i / 24, i % 24);
    }
    dir.write("index/index.toml", "format = 1\nname = \"Test index\"\n");

    auto cache = cache_store::open(dir.path() / "cache");
    ASSERT_TRUE(cache.has_value()) << cache.error().message;
    auto git = test::find_test_git();
    if (!git) GTEST_SKIP() << "git is not installed";
    catalog_settings settings;
    settings.root = index;
    settings.commit = hex64.substr(0, 40);
    settings.output = dir.path() / "site";
    settings.targets = targets;
    settings.cache = &*cache;
    settings.git = &*git;
    auto written = write_index_catalog(settings);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    std::string const front = read_file(dir.path() / "site" / "index.html").value_or("");
    auto const count = [&](std::string const& part) {
        std::size_t n = 0;
        for (std::size_t at = front.find(part); at != std::string::npos; at = front.find(part, at + 1)) ++n;
        return n;
    };
    // the 30 newest shown, all native; the apple one hidden, for the filter of its target
    EXPECT_EQ(count("<tr data-target=\"native\">"), 30u) << front;
    EXPECT_EQ(count("<tr hidden data-target=\"native\">"), 0u) << front;
    EXPECT_EQ(count("<tr hidden data-target=\"apple\">"), 1u) << front;
}
