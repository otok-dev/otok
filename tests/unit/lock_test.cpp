// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// otok.lock, format 1: docs/formats/otok-lock.md.

#include <gtest/gtest.h>

#include <string>

#include "otok/core/fs.hpp"
#include "otok/lock/lock.hpp"

using namespace otok;

namespace {

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += format(item) + "\n";
    return text;
}

std::string fixture()
{
    auto content = read_file(path_from_utf8(OTOK_TEST_DATA_DIR "/lock/example.lock"));
    EXPECT_TRUE(content.has_value());
    // the file may have been checked out with CRLF; the lock format writes \n
    std::string text = content.value_or("");
    std::erase(text, '\r');
    return text;
}

std::string const sha_a(64, 'a');
std::string const commit_b(40, 'b');

// An index package of the apple target, in the graph of macos/xcode.
std::string package(std::string_view library, std::string_view extra = "", std::string_view graph = "target = \"apple\"\nbuild-on = [\"macos/xcode\"]\n")
{
    return "[[package]]\nlibrary = \"" + std::string{library} + "\"\n" + std::string{graph} + "recipe = \"p/" + std::string{library}
        + "\"\nvariant = \"apple\"\nversion = \"1.0\"\nrevision = 1\n"
        "recipe-source = { git = \"https://x\", commit = \"" + commit_b + "\" }\nrecipe-sha256 = \"" + sha_a + "\"\n"
        "source = { url = \"https://x/a.tgz\", sha256 = \"" + sha_a + "\" }\nlicense = \"MIT\"\n" + std::string{extra};
}

// The keys of a path package of the apple target before `recipe`.
std::string const apple = "target = \"apple\"\nbuild-on = [\"macos/xcode\"]\n";

struct parsed
{
    std::optional<lock_file> lock;
    diagnostics diags;

    bool reports(std::string_view fragment) const
    {
        for (auto const& item : diags.items()) {
            if (format(item).find(fragment) != std::string::npos) return true;
        }
        return false;
    }
};

parsed parse(std::string const& text)
{
    parsed p;
    p.lock = parse_lock(text, "otok.lock", p.diags);
    return p;
}

}

TEST(lock, reads_the_example)
{
    auto p = parse(fixture());
    ASSERT_TRUE(p.lock.has_value()) << messages(p.diags);
    ASSERT_EQ(p.lock->packages.size(), 3u);
    ASSERT_EQ(p.lock->tool_packages.size(), 1u);

    locked_package const* openssl = p.lock->find("openssl", "apple", "macos/xcode");
    ASSERT_NE(openssl, nullptr);
    EXPECT_EQ(openssl->kind, locked_package::recipe_kind::index);
    EXPECT_EQ(openssl->id.str(), "apotocki/openssl");
    EXPECT_EQ(openssl->variant, "apple");
    EXPECT_EQ(openssl->version->str(), "3.5.4");
    EXPECT_EQ(openssl->recipe_source.type, locked_location::kind::git);
    EXPECT_EQ(openssl->recipe_source.path, "otok");
    EXPECT_EQ(openssl->source.type, locked_location::kind::archive);
    ASSERT_EQ(openssl->resources.size(), 1u);
    EXPECT_EQ(openssl->resources[0].name, "data");
    EXPECT_TRUE(openssl->reproducible());

    // one entry for both graphs of the windows target, another variant and revision of the same version
    locked_package const* windows = p.lock->find("openssl", "windows", "windows/msvc");
    ASSERT_NE(windows, nullptr);
    EXPECT_EQ(windows, p.lock->find("openssl", "windows", "linux/clang-cl"));
    EXPECT_EQ(windows->build_on, (std::vector<std::string>{"linux/clang-cl", "windows/msvc"}));
    EXPECT_EQ(windows->variant, "windows");
    EXPECT_EQ(windows->revision, 3);
    EXPECT_EQ(p.lock->find("openssl", "windows", "windows/clang-cl"), nullptr);
    EXPECT_EQ(p.lock->find("openssl", "android", "macos/xcode"), nullptr);

    EXPECT_EQ(p.lock->find("libssh2", "apple", "macos/xcode")->dependencies, (std::vector<std::string>{"openssl"}));
    EXPECT_TRUE(p.lock->find_tool("protobuf", "macos/xcode")->source.submodules);
    EXPECT_EQ(p.lock->find_tool("protobuf", "linux/gcc"), nullptr);
    EXPECT_EQ(p.lock->systems, (std::vector<std::string>{"linux", "macos", "windows"}));
    // tool references (otok-lock.md 2)
    EXPECT_EQ(p.lock->find("libssh2", "apple", "macos/xcode")->tool_dependencies, (std::vector<std::string>{"protobuf@31.1"}));
    EXPECT_NE(p.lock->find_tool("protobuf@31.1", "macos/xcode"), nullptr);
    EXPECT_EQ(p.lock->find_tool("protobuf@31.2", "macos/xcode"), nullptr);
}

TEST(lock, writes_the_same_bytes_it_reads)
{
    std::string const text = fixture();
    auto p = parse(text);
    ASSERT_TRUE(p.lock.has_value()) << messages(p.diags);
    EXPECT_EQ(write_lock(*p.lock), text);
}

TEST(lock, writing_is_canonical)
{
    // packages, environments, dependencies and resources come out sorted whatever order they were built in
    std::string const windows = "target = \"windows\"\nbuild-on = [\"windows/msvc\", \"linux/clang-cl\"]\n";
    auto p = parse("format = 1\n" + package("zlib") + "\n" + package("abc", "dependencies = [\"zlib\", \"mno\", \"zlib\"]\n") + "\n" + package("mno")
        + "\n" + package("abc", "", windows));
    ASSERT_TRUE(p.lock.has_value()) << messages(p.diags);
    std::string const text = write_lock(*p.lock);
    EXPECT_LT(text.find("library = \"abc\"\ntarget = \"apple\""), text.find("library = \"abc\"\ntarget = \"windows\""));
    EXPECT_LT(text.find("library = \"abc\"\ntarget = \"windows\""), text.find("library = \"mno\""));
    EXPECT_LT(text.find("library = \"mno\""), text.find("library = \"zlib\""));
    EXPECT_NE(text.find("dependencies = [\"mno\", \"zlib\"]"), std::string::npos) << text;
    EXPECT_NE(text.find("build-on = [\"linux/clang-cl\", \"windows/msvc\"]"), std::string::npos) << text;
    auto again = parse(text);
    ASSERT_TRUE(again.lock.has_value()) << messages(again.diags);
    EXPECT_EQ(write_lock(*again.lock), text);
}

TEST(lock, local_inputs)
{
    std::string const text = "format = 1\n[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"path\"\nversion = \"1\"\nrevision = 1\n"
        "recipe-source = \"local\"\nsource = \"local\"\nlicense = \"MIT\"\n"
        "[[package]]\nlibrary = \"b\"\n" + apple + "recipe = \"path\"\nversion = \"1\"\nrevision = 1\n"
        "recipe-source = { path = \"recipes/b\" }\nrecipe-sha256 = \"" + sha_a + "\"\nsource = { path = \"vendor/b\", sha256 = \"" + sha_a + "\" }\nlicense = \"MIT\"\n";
    auto p = parse(text);
    ASSERT_TRUE(p.lock.has_value()) << messages(p.diags);
    EXPECT_FALSE(p.lock->find("a", "apple", "macos/xcode")->reproducible());
    EXPECT_TRUE(p.lock->find("b", "apple", "macos/xcode")->reproducible());
    std::string const written = write_lock(*p.lock);
    EXPECT_NE(written.find("recipe-source = \"local\""), std::string::npos);
    EXPECT_NE(written.find("source = { path = \"vendor/b\", sha256 = "), std::string::npos);
    EXPECT_EQ(written.find("variant"), std::string::npos) << written;  // path recipes are their own variant
    EXPECT_TRUE(parse(written).lock.has_value());
}

TEST(lock, validates_entries)
{
    std::string const head = "format = 1\n";
    EXPECT_TRUE(parse(head + package("a") + "\n" + package("a")).reports("library 'a' is locked twice in [[package]] for 'apple' in macos/xcode"));
    // the same library in two graphs of a target, or of two targets, is not twice
    EXPECT_TRUE(parse(head + package("a") + "\n" + package("a", "", "target = \"apple\"\nbuild-on = [\"linux/clang-cl\"]\n")).lock.has_value());
    EXPECT_TRUE(parse(head + package("a") + "\n" + package("a", "", "target = \"android\"\nbuild-on = [\"*/ndk\"]\n")).lock.has_value());
    EXPECT_TRUE(parse(head + package("a", "dependencies = [\"missing\"]\n")).reports("lists 'missing' in 'dependencies', but the lock has no package of that name in macos/xcode"));
    // a dependency in another graph is not there
    EXPECT_TRUE(parse(head + package("a", "dependencies = [\"b\"]\n") + "\n" + package("b", "", "target = \"android\"\nbuild-on = [\"*/ndk\"]\n"))
                    .reports("lists 'b' in 'dependencies', but the lock has no package"));
    EXPECT_TRUE(parse(head + package("a", "tool-dependencies = [\"protoc@1\"]\n")).reports("the lock has no tool package"));
    EXPECT_TRUE(parse(head + package("a", "tool-dependencies = [\"protoc\"]\n")).reports("lists 'protoc' in 'tool-dependencies', which is no tool reference"));
    // a tool library once per version (otok-lock.md 2, 5.7)
    auto const tool = [&](std::string_view version) {
        return "[[tool-package]]\nlibrary = \"t\"\nbuild-on = [\"macos/xcode\"]\nrecipe = \"p/t\"\nvariant = \"apple\"\nversion = \"" + std::string{version}
            + "\"\nrevision = 1\nrecipe-source = { git = \"https://x\", commit = \"" + commit_b + "\" }\nrecipe-sha256 = \"" + sha_a + "\"\n"
            "source = { url = \"https://x/a.tgz\", sha256 = \"" + sha_a + "\" }\nlicense = \"MIT\"\n";
    };
    auto two = parse(head + package("a", "tool-dependencies = [\"t@1.0\"]\n") + "\n" + package("b", "tool-dependencies = [\"t@2.0\"]\n") + "\n"
        + tool("1.0") + "\n" + tool("2.0"));
    ASSERT_TRUE(two.lock.has_value()) << messages(two.diags);
    EXPECT_EQ(two.lock->find_tool("t@2.0", "macos/xcode")->version->str(), "2.0");
    EXPECT_TRUE(parse(head + tool("1.0") + "\n" + tool("1.0")).reports("library 't@1.0' is locked twice in [[tool-package]] for macos/xcode"));
    EXPECT_TRUE(parse(head + package("a", "extra = 1\n")).reports("unknown key 'extra' in package[1]"));
    EXPECT_TRUE(parse("format = 2\n").reports("lock format 2 needs a newer otok: this one reads format 1"));
    // the systems the project is built on (docs/PLAN.md decision 88)
    EXPECT_TRUE(parse("format = 1\nsystems = [\"bsd\"]\n").reports("'bsd' in 'systems' is not a system of build machines"));
    EXPECT_TRUE(parse("format = 1\nsystems = [\"linux\", \"linux\"]\n").reports("'linux' is twice in 'systems'"));
    EXPECT_EQ(lock_format_of("# Generated by otok. Do not edit.\nformat = 1\n"), 1);
    EXPECT_EQ(lock_format_of("[[package]]\n"), std::nullopt);

    EXPECT_TRUE(parse(head + package("a", "", "build-on = [\"macos/xcode\"]\n")).reports("missing required key 'target'"));
    EXPECT_TRUE(parse(head + package("a", "", "target = \"apple\"\n")).reports("missing required key 'build-on'"));
    EXPECT_TRUE(parse(head + package("a", "", "target = \"apple\"\nbuild-on = []\n")).reports("'build-on' in package[1] is empty"));
    EXPECT_TRUE(parse(head + package("a", "", "target = \"apple\"\nbuild-on = [\"macos\"]\n")).reports("'macos' in 'build-on' in package[1] is not an environment"));
    EXPECT_TRUE(parse(head + package("a", "", "target = \"apple\"\nbuild-on = [\"macos/xcode\", \"macos/xcode\"]\n")).reports("'macos/xcode' is twice in 'build-on'"));
    std::string no_variant = package("a");
    no_variant.erase(no_variant.find("variant = \"apple\"\n"), 18);
    EXPECT_TRUE(parse(head + no_variant).reports("missing required key 'variant'"));
    EXPECT_TRUE(parse(head + "[[tool-package]]\nlibrary = \"t\"\ntarget = \"apple\"\nbuild-on = [\"macos/xcode\"]\nrecipe = \"path\"\nversion = \"1\"\n"
        "revision = 1\nrecipe-source = \"local\"\nsource = \"local\"\nlicense = \"MIT\"\n").reports("unknown key 'target' in tool-package[1]"));

    std::string bad_sha = package("a");
    bad_sha.replace(bad_sha.find("recipe-sha256 = \"") + 17, 3, "XYZ");
    EXPECT_TRUE(parse(head + bad_sha).reports("'recipe-sha256' in package[1] must be 64 lowercase hexadecimal digits"));

    EXPECT_TRUE(parse(head + "[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"p/a\"\nvariant = \"apple\"\nversion = \"1\"\nrevision = 1\nrecipe-source = { path = \"r\" }\n"
        "recipe-sha256 = \"" + sha_a + "\"\nsource = \"local\"\nlicense = \"MIT\"\n").reports("does not match 'recipe'"));
    EXPECT_TRUE(parse(head + "[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"path\"\nvariant = \"apple\"\nversion = \"1\"\nrevision = 1\nrecipe-source = \"local\"\n"
        "source = \"local\"\nlicense = \"MIT\"\n").reports("a git or path recipe has no 'variant'"));
    EXPECT_TRUE(parse(head + "[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"path\"\nversion = \"1\"\nrevision = 1\nrecipe-source = \"local\"\n"
        "recipe-sha256 = \"" + sha_a + "\"\nsource = \"local\"\nlicense = \"MIT\"\n").reports("a local recipe has no 'recipe-sha256'"));
    EXPECT_TRUE(parse(head + "[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"git\"\nversion = \"1\"\nrevision = 1\n"
        "recipe-source = { git = \"https://x\", commit = \"" + commit_b + "\" }\nsource = \"local\"\nlicense = \"MIT\"\n").reports("missing required key 'recipe-sha256'"));
    EXPECT_TRUE(parse(head + "[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"path\"\nversion = \"1\"\nrevision = 1\nrecipe-source = { path = \"../r\" }\n"
        "recipe-sha256 = \"" + sha_a + "\"\nsource = \"local\"\nlicense = \"MIT\"\n").reports("must be a directory inside the project"));
}

// the index of the project a recipe came from (otok-lock.md 2, otok-toml.md 6.4)
TEST(lock, the_index_of_a_package)
{
    std::string const head = "format = 1\n";
    std::string with_index = package("a");
    with_index.insert(with_index.find("variant = \"apple\"\n") + 18, "index = \"corp\"\n");
    auto p = parse(head + with_index);
    ASSERT_TRUE(p.lock.has_value()) << messages(p.diags);
    EXPECT_EQ(p.lock->find("a", "apple", "macos/xcode")->index, "corp");
    std::string const written = write_lock(*p.lock);
    EXPECT_NE(written.find("variant = \"apple\"\nindex = \"corp\"\n"), std::string::npos) << written;
    EXPECT_EQ(write_lock(*parse(head + package("a")).lock).find("index ="), std::string::npos);  // `default` is not written

    std::string named_default = with_index;
    named_default.replace(named_default.find("\"corp\""), 6, "\"default\"");
    EXPECT_TRUE(parse(head + named_default).reports("'default' in 'index' in package[1] is not the name of an index of the project"));
    EXPECT_TRUE(parse(head + "[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"path\"\nindex = \"corp\"\nversion = \"1\"\nrevision = 1\nrecipe-source = \"local\"\n"
        "source = \"local\"\nlicense = \"MIT\"\n").reports("a git or path recipe has no 'index'"));
}

TEST(lock, git_tags)
{
    std::string const text = "format = 1\n\n[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"git\"\nversion = \"1.0\"\nrevision = 1\n"
        "recipe-source = { git = \"https://x/r\", commit = \"" + commit_b + "\", tag = \"recipes-1\", path = \"otok\" }\n"
        "recipe-sha256 = \"" + sha_a + "\"\n"
        "source = { git = \"https://x/a\", commit = \"" + commit_b + "\", tag = \"v1.0\", submodules = true }\n"
        "license = \"MIT\"\n";
    auto p = parse(text);
    ASSERT_TRUE(p.lock.has_value()) << messages(p.diags);
    EXPECT_EQ(p.lock->find("a", "apple", "macos/xcode")->recipe_source.tag, "recipes-1");
    EXPECT_EQ(p.lock->find("a", "apple", "macos/xcode")->source.tag, "v1.0");
    // written back in the same key order
    std::string const written = write_lock(*p.lock);
    EXPECT_NE(written.find("recipe-source = { git = \"https://x/r\", commit = \"" + commit_b + "\", tag = \"recipes-1\", path = \"otok\" }"),
        std::string::npos) << written;
    EXPECT_NE(written.find("source = { git = \"https://x/a\", commit = \"" + commit_b + "\", tag = \"v1.0\", submodules = true }"),
        std::string::npos) << written;

    std::string bad = text;
    bad.replace(bad.find("v1.0"), 4, "v..1");
    EXPECT_TRUE(parse(bad).reports("is not a valid tag name"));
}

TEST(lock, archive_mirrors)
{
    std::string const text = "# Generated by otok. Do not edit.\nformat = 1\n\n[[package]]\nlibrary = \"a\"\n" + apple + "recipe = \"git\"\nversion = \"1.0\"\nrevision = 1\n"
        "recipe-source = { git = \"https://x/r\", commit = \"" + commit_b + "\" }\n"
        "recipe-sha256 = \"" + sha_a + "\"\n"
        "source = { url = \"https://x/a.tgz\", sha256 = \"" + sha_a + "\", mirrors = [\"https://y/a.tgz\", \"https://z/a.tgz\"] }\n"
        "resources = [{ name = \"data\", url = \"https://x/d.zip\", sha256 = \"" + sha_a + "\", mirrors = [\"https://y/d.zip\"] }]\n"
        "license = \"MIT\"\n";
    auto p = parse(text);
    ASSERT_TRUE(p.lock.has_value()) << messages(p.diags);
    EXPECT_EQ(p.lock->find("a", "apple", "macos/xcode")->source.mirrors, (std::vector<std::string>{"https://y/a.tgz", "https://z/a.tgz"}));
    EXPECT_EQ(p.lock->find("a", "apple", "macos/xcode")->resources[0].location.mirrors, std::vector<std::string>{"https://y/d.zip"});
    EXPECT_EQ(write_lock(*p.lock), text);
}
