// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// recipe.toml, API 1: docs/formats/recipe-toml.md.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/recipe/recipe.hpp"
#include "otok/resolver/options.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_recipe.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/windows/windows_target.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

target_info const* const apple_targets[] = {&target::apple::info()};

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += format(item) + "\n";
    return text;
}

struct parsed
{
    std::optional<recipe> r;
    diagnostics diags;

    bool reports(std::string_view fragment) const
    {
        for (auto const& item : diags.items()) {
            if (format(item).find(fragment) != std::string::npos) return true;
        }
        return false;
    }
};

// A minimal valid recipe; tests append or replace parts.
std::string const head_text =
    "[recipe]\napi = 1\nlibrary = \"lz4\"\nname = \"lz4\"\nversion = \"1.10.0\"\nrevision = 1\n"
    "license = \"BSD-2-Clause\"\nlicense-files = [\"LICENSE\"]\ndescription = \"LZ4.\"\n";
std::string const source_text = "[source]\ngit = \"https://github.com/lz4/lz4\"\ncommit = \"ebb370ca83af193212df4dcbadcc5d87bc0de2f0\"\n";
std::string const platforms_text = "[platforms]\nsupported = [\"ios\", \"iossim\"]\nbuild-on = [\"macos/xcode\"]\n";
std::string const product_text = "[[product]]\nname = \"LZ4\"\nlibraries = [\"lz4\"]\n";

parsed parse(std::string_view text)
{
    parsed p;
    p.r = parse_recipe(text, "recipe.toml", apple_targets, {}, p.diags);
    return p;
}

parsed parse_with(std::string_view extra)
{
    return parse(head_text + source_text + platforms_text + product_text + std::string{extra});
}

}

TEST(recipe, reads_the_specification_example)
{
    diagnostics d;
    auto r = load_recipe(path_from_utf8(OTOK_TEST_DATA_DIR "/recipe/openssl"), apple_targets, d);
    ASSERT_TRUE(r.has_value()) << messages(d);

    EXPECT_EQ(r->api, 1);
    EXPECT_EQ(r->library, "openssl");
    EXPECT_EQ(r->version->str(), "3.5.4");
    EXPECT_EQ(r->revision, 1);
    EXPECT_EQ(r->channel, release_channel::stable);
    EXPECT_EQ(r->license.id, "Apache-2.0");
    EXPECT_EQ(r->source.location.type, input_location::kind::archive);
    ASSERT_TRUE(r->source.verify.has_value());
    EXPECT_EQ(r->source.verify->tag, "openssl-3.5.4");
    EXPECT_EQ(r->source.patches, (std::vector<std::string>{"patches/0001-ios-no-fork.patch"}));
    EXPECT_EQ(r->platforms.size(), 10u);

    ASSERT_EQ(r->vars.size(), 1u);
    EXPECT_EQ(std::get<std::string>(r->vars[0].values.at("iossim-x86_64")), "iossimulator-x86_64-xcrun");

    ASSERT_EQ(r->options.size(), 3u);
    EXPECT_EQ(r->find_option("features")->type, option_type::list);
    EXPECT_TRUE(r->find_option("zlib")->additive);

    ASSERT_EQ(r->dependencies.size(), 1u);
    recipe_dependency const& zlib = r->dependencies[0];
    EXPECT_EQ(zlib.default_recipe.str(), "apotocki/zlib");
    ASSERT_TRUE(zlib.when.has_value());
    EXPECT_EQ(zlib.when->option, "zlib");
    EXPECT_EQ(zlib.when->os, (std::vector<std::string>{"android", "windows"}));

    ASSERT_EQ(r->products.size(), 1u);
    product const& p = r->products[0];
    EXPECT_EQ(p.headers, "crypto");
    ASSERT_EQ(p.defines.size(), 1u);
    EXPECT_EQ(p.defines[0].define, "OPENSSL_NO_DEPRECATED");
    ASSERT_EQ(p.link_libraries.size(), 1u);
    EXPECT_EQ(p.link_libraries[0].library, "z");
    EXPECT_FALSE(p.link_libraries[0].when.has_value());
    EXPECT_EQ(p.runtime, "");
    auto const& apple_product = std::any_cast<target::apple::product_settings const&>(p.target_settings.at("apple"));
    EXPECT_EQ(apple_product.module, "COpenSSL");
    EXPECT_EQ(apple_product.wrappers, (std::vector<std::string>{"OpenSSLSwift"}));

    auto const& apple_recipe = std::any_cast<target::apple::recipe_settings const&>(r->target_settings.at("apple"));
    ASSERT_TRUE(apple_recipe.xcode.has_value());
    EXPECT_EQ(apple_recipe.xcode->str(), ">= 16");

    ASSERT_EQ(r->toolchains.size(), 2u);
    EXPECT_EQ(r->toolchains[0].name, "make");
    EXPECT_EQ(r->toolchains[1].name, "perl");
    ASSERT_EQ(r->publish_checks.size(), 1u);
}

TEST(recipe, minimal_recipe_is_valid)
{
    auto p = parse_with("");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    EXPECT_EQ(p.r->products[0].headers, "lz4");
}

TEST(recipe, checks_referenced_files)
{
    diagnostics d;
    std::string const text = head_text + source_text + "patches = [\"patches/missing.patch\"]\n" + platforms_text
        + product_text + "apple = { wrappers = [\"Missing\"], privacy-manifest = \"PrivacyInfo.xcprivacy\" }\n";
    auto r = parse_recipe(text, "recipe.toml", apple_targets, [](std::string_view, bool) { return false; }, d);
    EXPECT_FALSE(r.has_value());
    std::string const all = messages(d);
    EXPECT_NE(all.find("file 'patches/missing.patch'"), std::string::npos) << all;
    EXPECT_NE(all.find("directory 'wrappers/Missing'"), std::string::npos) << all;
    EXPECT_NE(all.find("file 'PrivacyInfo.xcprivacy'"), std::string::npos) << all;
}

TEST(recipe, reads_license_files_of_the_recipe_directory)
{
    std::string text = head_text + source_text + platforms_text + product_text;
    text.replace(text.find("license-files = [\"LICENSE\"]"), 27, "license-files = [\"LICENSE\", { recipe = \"NOTICE\" }]");
    diagnostics d;
    auto r = parse_recipe(text, "recipe.toml", apple_targets, [](std::string_view path, bool) { return path == "NOTICE"; }, d);
    ASSERT_TRUE(r.has_value()) << messages(d);
    ASSERT_EQ(r->license_files.size(), 2u);
    EXPECT_EQ(r->license_files[0].path, "LICENSE");
    EXPECT_FALSE(r->license_files[0].in_recipe);
    EXPECT_EQ(r->license_files[1].path, "NOTICE");
    EXPECT_TRUE(r->license_files[1].in_recipe);

    auto missing = parse_recipe(text, "recipe.toml", apple_targets, [](std::string_view, bool) { return false; }, d);
    EXPECT_FALSE(missing.has_value());
    EXPECT_NE(messages(d).find("file 'NOTICE' named in 'recipe' in recipe.license-files[2] does not exist in the recipe directory"), std::string::npos)
        << messages(d);
}

TEST(recipe, reads_linkage)
{
    auto const with = [](std::string const& value) {
        std::string text = head_text + source_text + platforms_text + product_text;
        text.replace(text.find("description = "), 0, value.empty() ? "" : "linkage = " + value + "\n");
        return parse(text);
    };
    auto plain = with("");
    ASSERT_TRUE(plain.r) << messages(plain.diags);
    EXPECT_EQ(plain.r->linkage, (std::vector<std::string>{"static"}));
    auto both = with("[\"static\", \"dynamic\"]");
    ASSERT_TRUE(both.r) << messages(both.diags);
    EXPECT_EQ(both.r->linkage, (std::vector<std::string>{"static", "dynamic"}));
    EXPECT_TRUE(with("[]").reports("'linkage' in recipe must not be empty"));
    EXPECT_TRUE(with("[\"shared\"]").reports("'linkage' in recipe may hold \"static\" and \"dynamic\", not 'shared'"));
    EXPECT_TRUE(with("[\"dynamic\", \"dynamic\"]").reports("'linkage' in recipe lists 'dynamic' twice"));
}

TEST(recipe, reads_the_cxx_standards)
{
    auto const with = [](std::string const& value) {
        std::string text = head_text + source_text + platforms_text + product_text;
        text.replace(text.find("description = "), 0, "cxx-standard = " + value + "\n");
        return parse(text);
    };
    auto range = with("\">= 14, <= 20\"");
    ASSERT_TRUE(range.r) << messages(range.diags);
    ASSERT_TRUE(range.r->standards.cxx);
    EXPECT_EQ(range.r->standards.cxx->str(), ">= 14, <= 20");
    EXPECT_TRUE(with("\"newest\"").reports("'cxx-standard' in recipe"));
    EXPECT_TRUE(with("17").reports("'cxx-standard' in recipe"));
}

TEST(recipe, reads_the_c_standards)
{
    auto const with = [](std::string const& value) {
        std::string text = head_text + source_text + platforms_text + product_text;
        text.replace(text.find("description = "), 0, "c-standard = " + value + "\n");
        return parse(text);
    };
    auto range = with("\">= 99, < 23\"");
    ASSERT_TRUE(range.r) << messages(range.diags);
    ASSERT_TRUE(range.r->standards.c);
    EXPECT_FALSE(range.r->standards.cxx);
    EXPECT_EQ(range.r->standards.c->str(), ">= 99, < 23");
    // by year: C99 is older than C11
    EXPECT_FALSE(range.r->standards.c->allows(89));
    EXPECT_TRUE(range.r->standards.c->allows(99));
    EXPECT_TRUE(range.r->standards.c->allows(11));
    EXPECT_TRUE(range.r->standards.c->allows(17));
    EXPECT_FALSE(range.r->standards.c->allows(23));
    EXPECT_TRUE(with("\"c99\"").reports("'c-standard' in recipe must compare standards by number, such as \">= 17\" or \">= 14, <= 20\", not 'c99'"));
    EXPECT_TRUE(with("\">= 2011\"").reports("'c-standard' in recipe must name standards by their two-digit numbers"));
    EXPECT_TRUE(with("\">= \"").reports("'c-standard' in recipe is not a constraint"));
}

TEST(recipe, checks_license_files)
{
    auto const with = [](std::string const& files) {
        std::string text = head_text + source_text + platforms_text + product_text;
        text.replace(text.find("license-files = [\"LICENSE\"]"), 27, "license-files = " + files);
        return parse(text);
    };
    EXPECT_TRUE(with("[]").reports("'license-files' in recipe must not be empty"));
    EXPECT_TRUE(with("\"LICENSE\"").reports("'license-files' in recipe must be an array of paths or { recipe } tables"));
    EXPECT_TRUE(with("[1]").reports("'license-files' in recipe must be an array of paths or { recipe } tables"));
    EXPECT_TRUE(with("[\"LICENSE\", { recipe = \"LICENSE\" }]").reports("'LICENSE' appears twice in 'license-files' in recipe"));
    EXPECT_TRUE(with("[{ recipe = \"../LICENSE\" }]").reports("must be a path inside the recipe directory"));
    EXPECT_TRUE(with("[{ path = \"LICENSE\" }]").reports("missing"));
}

TEST(recipe, rejects_unknown_api_without_further_noise)
{
    auto p = parse("[recipe]\napi = 2\nsomething = 1\n[new-table]\n");
    EXPECT_TRUE(p.reports("recipe API 2 needs a newer otok: this one reads API 1")) << messages(p.diags);
    EXPECT_EQ(p.diags.error_count(), 1u) << messages(p.diags);
}

TEST(recipe, checks_the_recipe_table)
{
    auto p = parse("[recipe]\napi = 1\nlibrary = \"LZ4\"\nname = \"lz4\"\nversion = \"1 0\"\nrevision = 0\nlicense = \"MIT OR\"\n"
        "license-files = [\"../LICENSE\"]\ndescription = \"two\\nlines\"\nkeywords = [\"Bad\"]\nhomepage = \"ftp://x\"\nchannel = \"beta\"\n"
        + source_text + platforms_text + product_text);
    for (auto fragment : {"is not a valid library name", "contains ' '", "'revision' in recipe must be 1 or more", "missing a license id",
             "must be a path inside the source", "one non-empty line", "keyword 'Bad'", "http(s) URL", "\"stable\" or \"prerelease\""}) {
        EXPECT_TRUE(p.reports(fragment)) << fragment << "\n" << messages(p.diags);
    }
}

TEST(recipe, checks_source_forms)
{
    auto const with_source = [](std::string const& source) {
        return parse(head_text + source + platforms_text + product_text);
    };
    EXPECT_TRUE(with_source("[source]\npatches = []\n").reports("needs exactly one of 'url'"));
    EXPECT_TRUE(with_source("[source]\nurl = \"https://x/a.tgz\"\ngit = \"https://x\"\n").reports("needs exactly one of 'url'"));
    EXPECT_TRUE(with_source("[source]\nurl = \"http://x/a.tgz\"\nsha256 = \"" + std::string(64, 'a') + "\"\n").reports("must be an https URL"));
    EXPECT_TRUE(with_source("[source]\nurl = \"https://x/a.tgz\"\nsha256 = \"abc\"\n").reports("64 lowercase hexadecimal digits"));
    EXPECT_TRUE(with_source("[source]\ngit = \"https://x\"\ncommit = \"main\"\n").reports("full 40-character commit SHA"));
    EXPECT_TRUE(with_source(source_text + "verify-against = { git = \"https://x\", tag = \"v1\" }\n").reports("is for archive sources"));
    EXPECT_TRUE(with_source("[source]\nurl = \"https://x/a.tgz\"\nsha256 = \"" + std::string(64, 'a') + "\"\nverify-against = { git = \"https://x\" }\n")
        .reports("needs exactly one of 'tag' or 'commit'"));
}

TEST(recipe, takes_git_repositories_by_ssh)
{
    std::string const commit = std::string(40, 'a');
    auto const with_git = [&](std::string const& url) {
        return parse(head_text + "[source]\ngit = \"" + url + "\"\ncommit = \"" + commit + "\"\n" + platforms_text
            + "[[resource]]\nname = \"core\"\ngit = \"" + url + "\"\ncommit = \"" + commit + "\"\n" + product_text);
    };
    for (std::string const url : {"ssh://git@git.example.com:2222/team/core.git", "user@203.0.113.7:/srv/git/core.git",
             "git.example.com:team/core.git"}) {
        auto p = with_git(url);
        ASSERT_TRUE(p.r.has_value()) << url << "\n" << messages(p.diags);
        EXPECT_EQ(p.r->source.location.git, url);
        EXPECT_EQ(p.r->resources.at(0).location.git, url);
    }
    // not a host before the ':', or no repository after it; archives stay https
    for (std::string const url : {"C:/repos/core", "ssh://", "user@host:", "dir/user@host:core", "http://x/core", "git://x/core"}) {
        auto p = with_git(url);
        EXPECT_TRUE(p.reports("'git' in source must be an https or ssh URL")) << url << "\n" << messages(p.diags);
        EXPECT_TRUE(p.reports("must be an https or ssh URL")) << url;
    }
    EXPECT_TRUE(parse(head_text + "[source]\nurl = \"git@x:a.tgz\"\nsha256 = \"" + std::string(64, 'a') + "\"\n" + platforms_text + product_text)
        .reports("'url' in source must be an https URL"));
}

TEST(recipe, reads_source_mirrors)
{
    std::string const archive = "[source]\nurl = \"https://archives.boost.io/b.tar.bz2\"\nsha256 = \"" + std::string(64, 'a') + "\"\n";
    auto p = parse(head_text + archive + "mirrors = [\"https://sourceforge.net/b.tar.bz2\", \"https://mirror/b.tar.bz2\"]\n" + platforms_text
        + "[[resource]]\nname = \"data\"\nurl = \"https://x/d.zip\"\nmirrors = [\"https://y/d.zip\"]\nsha256 = \"" + std::string(64, 'b') + "\"\n"
        + product_text);
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    EXPECT_EQ(p.r->source.location.mirrors, (std::vector<std::string>{"https://sourceforge.net/b.tar.bz2", "https://mirror/b.tar.bz2"}));
    ASSERT_EQ(p.r->resources.size(), 1u);
    EXPECT_EQ(p.r->resources[0].location.mirrors, std::vector<std::string>{"https://y/d.zip"});
}

TEST(recipe, checks_source_mirrors)
{
    auto const with_mirrors = [](std::string const& mirrors) {
        return parse(head_text + "[source]\nurl = \"https://x/a.tgz\"\nsha256 = \"" + std::string(64, 'a') + "\"\nmirrors = " + mirrors + "\n"
            + platforms_text + product_text);
    };
    EXPECT_TRUE(with_mirrors("[\"http://y/a.tgz\"]").reports("'mirrors' in source must hold https URLs, not 'http://y/a.tgz'"));
    EXPECT_TRUE(with_mirrors("[\"https://x/a.tgz\"]").reports("'mirrors' in source lists 'https://x/a.tgz' twice"));
    EXPECT_TRUE(with_mirrors("[\"https://y/a.tgz\", \"https://y/a.tgz\"]").reports("'mirrors' in source lists 'https://y/a.tgz' twice"));
    // git sources have one repository; other locations of it are a machine setting (url-rewrite)
    auto git = parse(head_text + "[source]\ngit = \"https://x\"\ncommit = \"" + std::string(40, 'c') + "\"\nmirrors = [\"https://y\"]\n"
        + platforms_text + product_text);
    EXPECT_TRUE(git.reports("mirrors")) << messages(git.diags);
}

TEST(recipe, checks_resources)
{
    auto p = parse_with("[[resource]]\nname = \"data\"\nurl = \"https://x/d.tgz\"\nsha256 = \"" + std::string(64, 'b') + "\"\ndest = \"data\"\n"
        "[[resource]]\nname = \"data\"\ngit = \"https://x\"\ncommit = \"" + std::string(40, 'c') + "\"\nunpack = false\n");
    EXPECT_TRUE(p.reports("resource 'data' is defined twice")) << messages(p.diags);
    EXPECT_TRUE(p.reports("applies to archive resources only")) << messages(p.diags);
}

TEST(recipe, checks_platforms)
{
    auto p = parse(head_text + source_text + "[platforms]\nsupported = [\"ios\", \"ipados\"]\narchitectures = { ios = [\"x86_64\"], tvos = [\"arm64\"] }\n" + product_text);
    EXPECT_TRUE(p.reports("unknown platform 'ipados'")) << messages(p.diags);
    EXPECT_TRUE(p.reports("architecture 'x86_64' is not available for platform 'ios'")) << messages(p.diags);
    EXPECT_TRUE(p.reports("platform 'tvos' in platforms.architectures is not a supported platform")) << messages(p.diags);
}

TEST(recipe, checks_option_schemas)
{
    auto p = parse_with(
        "[options.a]\ntype = \"bool\"\ndefault = \"yes\"\ndoc = \"x\"\n"
        "[options.b]\ntype = \"enum\"\ndefault = \"x\"\ndoc = \"x\"\n"
        "[options.c]\ntype = \"enum\"\nvalues = [\"p\", \"q\"]\ndefault = \"r\"\ndoc = \"x\"\n"
        "[options.d]\ntype = \"string\"\ndefault = \"\"\ndoc = \"x\"\nadditive = true\n"
        "[options.e]\ntype = \"list\"\nvalues = [\"p\", \"p\"]\ndefault = [\"z\"]\ndoc = \"x\"\n"
        "[options.f]\ntype = \"float\"\ndefault = 1\ndoc = \"x\"\n"
        "[options.g]\ntype = \"int\"\ndefault = 1\n");
    for (auto fragment : {"'default' in options.a does not match the option type 'bool'", "enum option 'b' needs a non-empty 'values' list",
             "'default' in options.c is not one of 'values'", "only bool and list options can be additive", "value 'p' is listed twice",
             "default element 'z' of option 'e'", "must be \"bool\", \"int\"", "missing required key 'doc' in options.g"}) {
        EXPECT_TRUE(p.reports(fragment)) << fragment << "\n" << messages(p.diags);
    }
}

TEST(recipe, reads_file_options_and_conditional_inputs)
{
    // recipe-toml.md 5, 8.3, 13: a file option without a default; resources and toolchains needed with it only
    std::string const file = "[options.filter]\ntype = \"file\"\ndoc = \"x\"\n";
    std::string const resource = "[[resource]]\nname = \"data\"\nurl = \"https://x/d.zip\"\nsha256 = \"" + std::string(64, 'b') + "\"\n";
    auto p = parse_with(file + resource + "when = { option = \"filter\" }\n"
        "[toolchains]\npython = { version = \">= 3.9\", when = { option = \"filter\" } }\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    ASSERT_EQ(p.r->options.size(), 1u);
    EXPECT_EQ(p.r->options[0].type, option_type::file);
    EXPECT_EQ(p.r->options[0].default_value, option_data{std::string{}});
    ASSERT_TRUE(p.r->resources[0].when.has_value());
    EXPECT_EQ(p.r->resources[0].when->option, "filter");
    ASSERT_TRUE(p.r->toolchains[0].when.has_value());
    EXPECT_EQ(p.r->toolchains[0].when->option, "filter");

    EXPECT_TRUE(parse_with("[options.filter]\ntype = \"file\"\ndefault = \"f.json\"\ndoc = \"x\"\n")
        .reports("a file option takes no 'default' in options.filter"));
    EXPECT_TRUE(parse_with(file + resource + "when = { platform = [\"ios\"] }\n").reports("may only test options, 'host', 'arch' and 'environment' here"));
    EXPECT_TRUE(parse_with(file + resource + "when = { option = \"nope\" }\n").reports("not an option of this recipe"));
    // toolchains are checked per target: its OS, not a platform; not that of one built by its recipe, a tool of the build machine
    auto with_os = parse_with(file + "[toolchains]\npython = { when = { os = [\"windows\"] } }\n");
    ASSERT_TRUE(with_os.r.has_value()) << messages(with_os.diags);
    EXPECT_EQ(with_os.r->toolchains[0].when->os, std::vector<std::string>{"windows"});
    EXPECT_TRUE(parse_with(file + "[toolchains]\npython = { when = { platform = [\"ios\"] } }\n").reports("may only test options, 'os', 'arch' and 'environment' here"));
    EXPECT_TRUE(parse_with(file + "[toolchains]\npython = { when = { not = { os = [\"apple\"] } }, default-recipe = \"p/python\" }\n")
                    .reports("a toolchain built by its recipe is needed whatever the target, so its 'when' may test options only"));
    EXPECT_TRUE(parse_with(file + "[toolchains]\npython = { when = { option = \"filter\", is = \"x\" } }\n").reports("'is' does not match the type"));
    EXPECT_TRUE(parse_with(file + "[[publish.check]]\noptions = { filter = \"f.json\" }\n")
        .reports("option 'filter' is a file option, which only a project sets"));
}

TEST(recipe, vars_cover_every_slice)
{
    EXPECT_TRUE(parse_with("[vars]\ntarget = { ios = \"a\", iossim-arm64 = \"b\" }\n").reports("variable 'target' has no value for iossim-x86_64"));
    EXPECT_TRUE(parse_with("[vars]\ntarget = { ios = \"a\", iossim = \"b\" }\n").r.has_value());
    EXPECT_TRUE(parse_with("[vars]\ntarget = { apple = \"a\" }\n").r.has_value());
    EXPECT_TRUE(parse_with("[vars]\ntarget = { default = \"a\" }\n").r.has_value());

    auto p = parse_with("[vars]\nt = { ios = \"a\", iossim = 1, default = \"c\", ios-x86_64 = \"d\", mars = \"e\" }\nBad = { default = 1 }\n");
    EXPECT_TRUE(p.reports("other values of variable 't' have another type")) << messages(p.diags);
    EXPECT_TRUE(p.reports("unknown selector 'ios-x86_64'")) << messages(p.diags);
    EXPECT_TRUE(p.reports("unknown selector 'mars'")) << messages(p.diags);
    EXPECT_TRUE(p.reports("invalid variable name 'Bad'")) << messages(p.diags);
}

TEST(recipe, checks_conditions)
{
    std::string const options =
        "[options.flag]\ntype = \"bool\"\ndefault = false\ndoc = \"x\"\n"
        "[options.parts]\ntype = \"list\"\nvalues = [\"json\"]\ndefault = []\ndoc = \"x\"\n"
        "[options.tls]\ntype = \"enum\"\nvalues = [\"openssl\", \"none\"]\ndefault = \"none\"\ndoc = \"x\"\n";
    auto const dep = [&](std::string const& when) {
        return parse_with(options + "[deps.zlib]\nversion = \"~> 1.3\"\ndefault-recipe = \"p/zlib\"\nwhen = " + when + "\n");
    };
    EXPECT_TRUE(dep("{ option = \"flag\" }").r.has_value());
    EXPECT_TRUE(dep("{ option = \"parts\", has = \"json\" }").r.has_value());
    EXPECT_TRUE(dep("{ option = \"tls\", is = \"openssl\" }").r.has_value());
    EXPECT_TRUE(dep("{ platform = [\"ios\"], os = [\"apple\"] }").r.has_value());
    auto negated = dep("{ not = { option = \"flag\" } }");
    ASSERT_TRUE(negated.r.has_value()) << messages(negated.diags);
    ASSERT_TRUE(negated.r->dependencies[0].when->negated);
    EXPECT_EQ(negated.r->dependencies[0].when->negated->option, "flag");
    EXPECT_TRUE(dep("{ os = [\"apple\"], not = { platform = [\"ios\"], not = { option = \"flag\" } } }").r.has_value());
    EXPECT_TRUE(dep("{ not = { option = \"nope\" } }").reports("deps.zlib.when.not tests 'nope', which is not an option of this recipe"));
    EXPECT_TRUE(dep("{ not = {} }").reports("must test at least one of"));

    EXPECT_TRUE(dep("{ option = \"nope\" }").reports("not an option of this recipe"));
    EXPECT_TRUE(dep("{ option = \"parts\" }").reports("needs a bool or file option"));
    EXPECT_TRUE(dep("{ option = \"flag\", has = \"x\" }").reports("'has' needs a list option"));
    EXPECT_TRUE(dep("{ option = \"parts\", has = \"xml\" }").reports("'xml' is not one of the values"));
    EXPECT_TRUE(dep("{ option = \"tls\", is = \"gnutls\" }").reports("'gnutls' is not one of the values"));
    EXPECT_TRUE(dep("{ option = \"tls\", is = 1 }").reports("'is' does not match the type"));
    EXPECT_TRUE(dep("{ has = \"json\" }").reports("'has' and 'is' need 'option'"));
    EXPECT_TRUE(dep("{}").reports("must test at least one of"));
    EXPECT_TRUE(dep("{ platform = [\"ipados\"] }").reports("unknown platform 'ipados'"));
    EXPECT_TRUE(dep("{ option = \"flag\", else = 1 }").reports("unknown key 'else' in deps.zlib.when"));

    // tool dependencies and products test options only
    EXPECT_TRUE(parse_with(options + "[tool-deps.protobuf]\nversion = \"~> 31.0\"\ndefault-recipe = \"p/protobuf\"\nwhen = { platform = [\"ios\"] }\n")
        .reports("may only test options here"));
    EXPECT_TRUE(parse_with(options + "[tool-deps.protobuf]\nversion = \"~> 31.0\"\ndefault-recipe = \"p/protobuf\"\nwhen = { not = { os = [\"apple\"] } }\n")
        .reports("may only test options here"));
}

TEST(recipe, checks_dependencies)
{
    EXPECT_TRUE(parse_with("[deps.lz4]\nversion = \"1\"\ndefault-recipe = \"p/lz4\"\n").reports("cannot depend on its own library"));
    EXPECT_TRUE(parse_with("[deps.zlib]\nversion = \"1\"\n").reports("missing required key 'default-recipe'"));
    EXPECT_TRUE(parse_with("[deps.zlib]\nversion = \"1\"\ndefault-recipe = \"zlib\"\n").reports("must be a recipe id"));
    EXPECT_TRUE(parse_with("[deps.zlib]\ndefault-recipe = \"p/zlib\"\n").reports("missing required key 'version'"));
    auto ok = parse_with("[deps.zlib]\nversion = \"~> 1.3\"\ndefault-recipe = \"p/zlib\"\nrequires-options = { shared = false }\n");
    ASSERT_TRUE(ok.r.has_value()) << messages(ok.diags);
    EXPECT_EQ(std::get<bool>(ok.r->dependencies[0].requires_options.at("shared").value), false);
    EXPECT_FALSE(ok.r->dependencies[0].linkage.has_value());
}

// the names of otok.toml (docs/PLAN.md decision 87)
TEST(recipe, refuses_the_old_names_of_dependencies)
{
    EXPECT_TRUE(parse_with("[dependencies.zlib]\nversion = \"1\"\ndefault-recipe = \"p/zlib\"\n")
                    .reports("[dependencies] is now [deps], as in otok.toml"));
    EXPECT_TRUE(parse_with("[tool-dependencies.zlib]\nversion = \"1\"\ndefault-recipe = \"p/zlib\"\n")
                    .reports("[tool-dependencies] is now [tool-deps], as in otok.toml"));
}

// the dependency's linkage when the project chooses none (docs/PLAN.md decision 68)
TEST(recipe, reads_the_linkage_of_a_dependency)
{
    auto ok = parse_with("[deps.zlib]\nversion = \"1\"\ndefault-recipe = \"p/zlib\"\nlinkage = \"static\"\n");
    ASSERT_TRUE(ok.r.has_value()) << messages(ok.diags);
    EXPECT_EQ(ok.r->dependencies[0].linkage, "static");
    EXPECT_TRUE(parse_with("[deps.zlib]\nversion = \"1\"\ndefault-recipe = \"p/zlib\"\nlinkage = \"shared\"\n")
                    .reports("'linkage' in deps.zlib must be \"static\" or \"dynamic\", not 'shared'"));
    EXPECT_TRUE(parse_with("[tool-deps.zlib]\nversion = \"1\"\ndefault-recipe = \"p/zlib\"\nlinkage = \"dynamic\"\n")
                    .reports("'linkage' in tool-deps.zlib is not for a tool dependency: tools are static"));
}

TEST(recipe, checks_products)
{
    auto p = parse(head_text + source_text + platforms_text
        + "[[product]]\nname = \"A\"\nlibraries = [\"a\", \"a\"]\nheaders = \"z\"\ndepends = [\"B\", \"Nope\"]\ndefines = [\"1BAD\", { define = \"OK=1\", when = { platform = [\"ios\"] } }, 5]\n"
          "[[product]]\nname = \"B\"\nlibraries = [\"b\"]\ndepends = [\"A\"]\n"
          "[[product]]\nname = \"A\"\nlibraries = [\"c\"]\napple = { module = \"Bad-Module\", unknown = 1 }\n");
    for (auto fragment : {"library 'a' is listed twice", "'headers' in product[1] must name one of", "depends on unknown product 'Nope'",
             "'1BAD' is not a preprocessor definition", "product[1].defines[2].when may only test options", "must be an array of strings or { define, when }",
             "product 'A' is defined twice", "in a cycle", "not a valid Clang module name", "unknown key 'unknown' in product[3].apple"}) {
        EXPECT_TRUE(p.reports(fragment)) << fragment << "\n" << messages(p.diags);
    }
    EXPECT_TRUE(parse(head_text + source_text + platforms_text).reports("missing required key 'product'"));
}

TEST(recipe, reads_products_of_headers_only)
{
    auto p = parse(head_text + source_text + platforms_text
        + "[[product]]\nname = \"Boost\"\nlibraries = []\nlink = { runtime = \"c++\" }\n"
          "[[product]]\nname = \"BoostFilesystem\"\nlibraries = [\"filesystem\", \"atomic\"]\nheaders = false\ndepends = [\"Boost\"]\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    ASSERT_EQ(p.r->products.size(), 2u);
    EXPECT_TRUE(p.r->products[0].headers_only());
    EXPECT_EQ(p.r->products[0].headers, "");
    EXPECT_FALSE(p.r->products[1].headers_only());
    EXPECT_EQ(p.r->products[1].headers, "");  // no headers of its own
    EXPECT_EQ(p.r->products[0].runtime, "c++");
    EXPECT_TRUE(p.r->products[0].link_libraries.empty());
}

// recipe-toml.md 11: system libraries with conditions of options and the target, the C++ standard library as `runtime`
TEST(recipe, reads_the_link_of_a_product)
{
    std::string const option = "[options.net]\ntype = \"bool\"\ndefault = false\ndoc = \"x\"\n";
    auto const with_link = [&](std::string const& link) {
        return parse(head_text + source_text + platforms_text + option + "[[product]]\nname = \"P\"\nlibraries = [\"p\"]\nlink = " + link + "\n");
    };
    auto ok = with_link("{ libraries = [\"z\", { library = \"ws2_32\", when = { os = [\"windows\"], option = \"net\" } }, { library = \"m\" }], runtime = \"c++\" }");
    ASSERT_TRUE(ok.r.has_value()) << messages(ok.diags);
    std::vector<product_link> const& link = ok.r->products[0].link_libraries;
    ASSERT_EQ(link.size(), 3u);
    EXPECT_EQ(link[0].library, "z");
    EXPECT_FALSE(link[0].when.has_value());
    EXPECT_EQ(link[1].library, "ws2_32");
    ASSERT_TRUE(link[1].when.has_value());
    EXPECT_EQ(link[1].when->os, (std::vector<std::string>{"windows"}));
    EXPECT_EQ(link[1].when->option, "net");
    EXPECT_EQ(link[2].library, "m");
    EXPECT_EQ(ok.r->products[0].runtime, "c++");

    merged_options const off{{"net", option_data{false}}}, on{{"net", option_data{true}}};
    EXPECT_EQ(link_on(ok.r->products[0], on, "windows"), (std::vector<std::string>{"z", "ws2_32", "m"}));
    EXPECT_EQ(link_on(ok.r->products[0], off, "windows"), (std::vector<std::string>{"z", "m"}));
    EXPECT_EQ(link_on(ok.r->products[0], on, "apple"), (std::vector<std::string>{"z", "m"}));
}

TEST(recipe, refuses_the_cxx_standard_library_in_link)
{
    auto const with_link = [](std::string const& link) {
        return parse(head_text + source_text + platforms_text + "[[product]]\nname = \"P\"\nlibraries = [\"p\"]\nlink = " + link + "\n");
    };
    for (std::string const name : {"c++", "stdc++", "c++_shared", "c++_static", "c++abi"}) {
        EXPECT_TRUE(with_link("{ libraries = [\"" + name + "\"] }")
                        .reports("'" + name + "' in 'libraries' in product[1].link is the C++ standard library, which the target names: "
                                 "write runtime = \"c++\" in 'link' in product[1]"))
            << name;
    }
    EXPECT_TRUE(with_link("{ libraries = [{ library = \"c++\", when = { os = [\"apple\"] } }] }").reports("is the C++ standard library"));
}

TEST(recipe, checks_the_link_of_a_product)
{
    auto const with_link = [](std::string const& link) {
        return parse(head_text + source_text + platforms_text + "[[product]]\nname = \"P\"\nlibraries = [\"p\"]\nlink = " + link + "\n");
    };
    EXPECT_TRUE(with_link("{ runtime = \"stdc++\" }").reports("'runtime' in product[1].link must be \"c++\", not 'stdc++'"));
    EXPECT_TRUE(with_link("{ libraries = \"z\" }").reports("'libraries' in product[1].link must be an array of strings or { library, when } tables"));
    EXPECT_TRUE(with_link("{ libraries = [5] }").reports("must be an array of strings or { library, when } tables"));
    EXPECT_TRUE(with_link("{ libraries = [\"z\", \"z\"] }").reports("library 'z' is listed twice in 'libraries' in product[1].link"));
    EXPECT_TRUE(with_link("{ libraries = [{ when = { os = [\"apple\"] } }] }").reports("missing required key 'library'"));
    EXPECT_TRUE(with_link("{ libraries = [{ library = \"z\", when = { platform = [\"ios\"] } }] }").reports("may only test options and 'os' here"));
    EXPECT_TRUE(with_link("{ libraries = [{ library = \"z\", when = { arch = [\"arm64\"] } }] }").reports("may only test options and 'os' here"));
    EXPECT_TRUE(with_link("{ libraries = [{ library = \"z\", when = { environment = [\"macos/xcode\"] } }] }").reports("may only test options and 'os' here"));
    EXPECT_TRUE(with_link("{ libraries = [{ library = \"z\", when = { option = \"nope\" } }] }").reports("which is not an option of this recipe"));
    EXPECT_TRUE(with_link("{ libraries = [{ library = \"z\", extra = 1 }] }").reports("unknown key 'extra'"));
    EXPECT_TRUE(with_link("{ frameworks = [\"Security\"] }").reports("unknown key 'frameworks' in product[1].link"));
}

TEST(recipe, checks_products_of_headers_only)
{
    auto const with_product = [](std::string const& body) { return parse(head_text + source_text + platforms_text + "[[product]]\nname = \"P\"\n" + body); };
    EXPECT_TRUE(with_product("libraries = []\nheaders = \"a\"\n").reports("'headers' in product[1] is not for a product of headers only (libraries = [])"));
    EXPECT_TRUE(with_product("libraries = []\nheaders = false\n").reports("is not for a product of headers only"));
    EXPECT_TRUE(with_product("libraries = []\ndefines = [\"X\"]\n").reports("'defines' in product[1] is not for a product of headers only (libraries = [])"));
    EXPECT_TRUE(with_product("libraries = [\"a\"]\nheaders = true\n").reports("must name one of the product's libraries, or be false"));
    EXPECT_TRUE(with_product("libraries = [\"a\"]\nheaders = 1\n").reports("must name one of the product's libraries, or be false"));
}

TEST(recipe, reads_products_that_exclude_each_other)
{
    auto p = parse(head_text + source_text + platforms_text
        + "[[product]]\nname = \"Boost\"\nlibraries = []\n"
          "[[product]]\nname = \"Basic\"\nlibraries = [\"basic\"]\nheaders = false\ndepends = [\"Boost\"]\nalternatives = \"stacktrace\"\n"
          "[[product]]\nname = \"Noop\"\nlibraries = [\"noop\"]\nheaders = false\ndepends = [\"Boost\"]\nalternatives = \"stacktrace\"\n"
          "[[product]]\nname = \"UsesBasic\"\nlibraries = [\"uses\"]\nheaders = false\ndepends = [\"Basic\"]\n"
          "[[product]]\nname = \"Other\"\nlibraries = [\"other\"]\nheaders = false\ndepends = [\"Boost\"]\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    auto const& products = p.r->products;
    EXPECT_EQ(products[0].alternatives, "");
    EXPECT_EQ(products[1].alternatives, "stacktrace");
    EXPECT_EQ(p.r->held_products(products[3]), (std::vector<product const*>{&products[3], &products[1], &products[0]}));
    EXPECT_TRUE(p.r->exclude_each_other(products[1], products[2]));
    EXPECT_TRUE(p.r->exclude_each_other(products[3], products[2]));  // through depends
    EXPECT_FALSE(p.r->exclude_each_other(products[1], products[1]));
    EXPECT_FALSE(p.r->exclude_each_other(products[1], products[3]));
    EXPECT_FALSE(p.r->exclude_each_other(products[2], products[4]));
}

TEST(recipe, checks_products_that_exclude_each_other)
{
    std::string const two = "[[product]]\nname = \"A\"\nlibraries = [\"a\"]\nalternatives = \"g\"\n"
                            "[[product]]\nname = \"B\"\nlibraries = [\"b\"]\nalternatives = \"g\"\n";
    EXPECT_TRUE(parse(head_text + source_text + platforms_text + "[[product]]\nname = \"A\"\nlibraries = [\"a\"]\nalternatives = \"1g\"\n")
                    .reports("'1g' in 'alternatives' in product[1] is not a valid group name ([A-Za-z_][A-Za-z0-9_]*)"));
    EXPECT_TRUE(parse(head_text + source_text + platforms_text + "[[product]]\nname = \"A\"\nlibraries = [\"a\"]\nalternatives = \"g\"\n")
                    .reports("'alternatives' in product 'A' names the group 'g', which no other product names"));
    EXPECT_TRUE(parse(head_text + source_text + platforms_text + two + "[[product]]\nname = \"C\"\nlibraries = [\"c\"]\ndepends = [\"A\", \"B\"]\n")
                    .reports("product 'C' holds 'A' and 'B' through 'depends', which exclude each other (alternatives 'g')"));
    EXPECT_TRUE(parse(head_text + source_text + platforms_text + "[apple]\nswift-package = \".\"\n[[product]]\nname = \"LZ4\"\nalternatives = \"g\"\n")
                    .reports("'alternatives' in product[1] is not for a recipe that delivers its source"));
    EXPECT_TRUE(parse(head_text + source_text + platforms_text + two).r.has_value());
}

TEST(recipe, reads_the_environments_and_the_variant)
{
    auto p = parse(head_text + source_text + "[platforms]\nsupported = [\"ios\", \"iossim\"]\nbuild-on = [\"macos/xcode\"]\n" + product_text);
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    ASSERT_EQ(p.r->build_on.size(), 1u);
    EXPECT_EQ(p.r->build_on[0].text(), "macos/xcode");
    EXPECT_EQ(p.r->variant, "apple");  // the target of the platforms

    target_info const* const targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info()};
    parsed w;
    w.r = parse_recipe("[recipe]\napi = 1\nlibrary = \"lz4\"\nname = \"lz4\"\nvariant = \"windows-msvc\"\nversion = \"1.10.0\"\nrevision = 1\n"
        "license = \"BSD-2-Clause\"\nlicense-files = [\"LICENSE\"]\ndescription = \"LZ4.\"\n" + source_text
        + "[platforms]\nsupported = [\"windows\"]\nbuild-on = [\"windows/msvc\", \"windows-arm64/clang-cl\"]\n" + product_text,
        "recipe.toml", targets, {}, w.diags);
    ASSERT_TRUE(w.r.has_value()) << messages(w.diags);
    EXPECT_EQ(w.r->variant, "windows-msvc");
    ASSERT_EQ(w.r->build_on.size(), 2u);
    EXPECT_EQ(w.r->build_on[1].text(), "windows-arm64/clang-cl");

    // several targets, each with its environments
    parsed a;
    a.r = parse_recipe(head_text.substr(0, head_text.find("version")) + "variant = \"all\"\n" + head_text.substr(head_text.find("version"))
        + source_text + "[platforms]\nsupported = [\"ios\", \"android\"]\nbuild-on = [\"macos/xcode\", \"*/ndk\"]\n" + product_text,
        "recipe.toml", targets, {}, a.diags);
    ASSERT_TRUE(a.r.has_value()) << messages(a.diags);
    EXPECT_EQ(a.r->variant, "all");
}

TEST(recipe, checks_the_environments_and_the_variant)
{
    target_info const* const targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info()};
    auto const with = [&](std::string const& platforms, std::string const& head = head_text) {
        parsed p;
        p.r = parse_recipe(head + source_text + "[platforms]\n" + platforms + product_text, "recipe.toml", targets, {}, p.diags);
        return p;
    };
    auto const ios = [&](std::string const& build_on) { return with("supported = [\"ios\"]\nbuild-on = [" + build_on + "]\n"); };
    EXPECT_TRUE(ios("").reports("'build-on' in platforms must not be empty"));
    EXPECT_TRUE(ios("\"macos\"").reports("'macos' in 'build-on' in platforms is not an environment (<system>/<compiler>, <system>-<arch>/<compiler> or */<compiler>)"));
    EXPECT_TRUE(ios("\"macos/xcode\", \"macos/xcode\"").reports("environment 'macos/xcode' is listed twice in 'build-on' in platforms"));
    EXPECT_TRUE(ios("\"linux/xcode\"").reports("environment 'linux/xcode' in 'build-on' in platforms is not one of the target 'apple'"));
    EXPECT_TRUE(ios("\"*/xcode\"").reports("'*/xcode' in 'build-on' in platforms: '*' is for a compiler that builds alike on every host, which 'xcode' is not for 'apple'"));
    EXPECT_TRUE(with("supported = [\"windows\"]\nbuild-on = [\"*/clang-cl\"]\n").reports("which 'clang-cl' is not for 'windows'"));
    EXPECT_TRUE(with("supported = [\"ios\", \"android\"]\nbuild-on = [\"*/ndk\"]\n", head_text.substr(0, head_text.find("version")) + "variant = \"all\"\n"
        + head_text.substr(head_text.find("version"))).reports("'build-on' in platforms names no environment of the target 'apple'"));
    EXPECT_TRUE(with("supported = [\"ios\", \"android\"]\n").reports("a recipe whose platforms are of several targets (apple, android) names its 'variant' in [recipe]"));
    EXPECT_TRUE(with("supported = [\"ios\"]\n", head_text.substr(0, head_text.find("version")) + "variant = \"Apple\"\n" + head_text.substr(head_text.find("version")))
                    .reports("'Apple' in 'variant' in recipe is not a valid variant name (as a recipe name)"));
}

TEST(recipe, checks_toolchains)
{
    auto p = parse_with("[toolchains]\nperl = \">= 5.30\"\nrust = { version = \">= 1.80\", targets = [\"aarch64-apple-ios\"] }\n"
        "cmake = { targets = [\"x\"] }\nmsbuild = \"17\"\nninja = 1\nclang-cl = \">= 20\"\n\"Flat C\" = { default-recipe = \"a/flatc\" }\n");
    EXPECT_TRUE(p.reports("applies to the rust toolchain only")) << messages(p.diags);
    // a program otok does not find installed is built by a recipe (docs/PLAN.md decision 87)
    EXPECT_TRUE(p.reports("the program 'msbuild' of [toolchains] has no 'default-recipe' building it")) << messages(p.diags);
    // the compiler of an environment, which its target finds, is no toolchain (docs/PLAN.md decision 85)
    EXPECT_TRUE(p.reports("the program 'clang-cl' of [toolchains] has no 'default-recipe' building it")) << messages(p.diags);
    EXPECT_TRUE(p.reports("must be a version constraint or a table")) << messages(p.diags);
    EXPECT_TRUE(p.reports("'Flat C' in [toolchains] is not a valid program name")) << messages(p.diags);
}

TEST(recipe, a_toolchain_names_the_recipe_that_builds_it)
{
    // recipe-toml.md 13.1: a tool dependency on the library of the recipe it names, with its constraint and when
    auto p = parse_with("[options.gen]\ntype = \"bool\"\ndefault = false\ndoc = \"x\"\n"
        "[toolchains]\nninja = { version = \">= 1.10\", default-recipe = \"apotocki/ninja\" }\n"
        "cmake = { default-recipe = \"apotocki/cmake\", when = { option = \"gen\" } }\nperl = \">= 5\"\n"
        "flatc = { version = \">= 25\", default-recipe = \"apotocki/flatbuffers\" }\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    ASSERT_EQ(p.r->toolchains.size(), 4u);
    ASSERT_EQ(p.r->tool_dependencies.size(), 3u);
    auto const edge = [&](std::string_view id) {
        return std::ranges::find_if(p.r->tool_dependencies, [&](recipe_dependency const& d) { return d.default_recipe.str() == id; });
    };
    auto const ninja = edge("apotocki/ninja");
    auto const cmake = edge("apotocki/cmake");
    auto const flatc = edge("apotocki/flatbuffers");
    ASSERT_TRUE(ninja != p.r->tool_dependencies.end() && cmake != p.r->tool_dependencies.end() && flatc != p.r->tool_dependencies.end());
    EXPECT_EQ(ninja->library, "");  // the resolver learns it from the recipe
    EXPECT_EQ(ninja->programs, std::vector<std::string>{"ninja"});
    EXPECT_FALSE(ninja->from_dependency);
    EXPECT_EQ(ninja->version.str(), ">= 1.10");
    EXPECT_FALSE(ninja->when.has_value());
    EXPECT_EQ(cmake->version.str(), "");  // any
    ASSERT_TRUE(cmake->when.has_value());
    EXPECT_EQ(cmake->when->option, "gen");
    EXPECT_EQ(flatc->programs, std::vector<std::string>{"flatc"});
    auto const perl = std::ranges::find(p.r->toolchains, "perl", &toolchain_requirement::name);
    EXPECT_FALSE(perl->default_recipe.has_value());
}

// several programs of one recipe are one tool dependency, with all their constraints
TEST(recipe, programs_of_one_recipe_are_one_tool_dependency)
{
    auto p = parse_with("[toolchains]\ncmake = { version = \">= 3.20\", default-recipe = \"apotocki/cmake\" }\n"
        "ctest = { version = \"< 5\", default-recipe = \"apotocki/cmake\" }\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    ASSERT_EQ(p.r->tool_dependencies.size(), 1u);
    EXPECT_EQ(p.r->tool_dependencies[0].programs, (std::vector<std::string>{"cmake", "ctest"}));
    EXPECT_EQ(p.r->tool_dependencies[0].version.str(), ">= 3.20, < 5");
    EXPECT_TRUE(parse_with("[options.gen]\ntype = \"bool\"\ndefault = false\ndoc = \"x\"\n"
        "[toolchains]\ncmake = { default-recipe = \"apotocki/cmake\", when = { option = \"gen\" } }\n"
        "ctest = { default-recipe = \"apotocki/cmake\" }\n").reports("are one tool dependency, so they need the same 'when'"));
}

// The order of the file is lost in the projection of an index entry, written sorted: tool dependencies have one order of their own,
// which the tool edges of a resolved package follow (resolved_package::tool_edges)
TEST(recipe, orders_tool_dependencies_whatever_the_file)
{
    auto p = parse_with("[tool-deps.zz]\nversion = \">= 1\"\ndefault-recipe = \"a/zz\"\n[tool-deps.aa]\nversion = \">= 1\"\ndefault-recipe = \"a/aa\"\n"
        "[toolchains]\npython = { version = \">= 3.6\", default-recipe = \"apotocki/python\" }\n"
        "ninja = { version = \">= 1.10\", default-recipe = \"apotocki/ninja\" }\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    std::vector<std::string> order;
    for (recipe_dependency const& d : p.r->tool_dependencies) order.push_back(d.library.empty() ? d.default_recipe.str() : d.library);
    EXPECT_EQ(order, (std::vector<std::string>{"apotocki/ninja", "apotocki/python", "aa", "zz"}));
}

// recipe-toml.md 13.2: programs of a dependency
TEST(recipe, takes_programs_from_a_dependency)
{
    auto p = parse_with("[deps.flatbuffers]\nversion = \">= 23\"\ndefault-recipe = \"apotocki/flatbuffers\"\ntoolchains = [\"flatc\"]\n"
        "when = { platform = [\"ios\"] }\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    ASSERT_EQ(p.r->dependencies.size(), 1u);
    EXPECT_EQ(p.r->dependencies[0].toolchains, std::vector<std::string>{"flatc"});
    ASSERT_EQ(p.r->tool_dependencies.size(), 1u);
    recipe_dependency const& tool = p.r->tool_dependencies[0];
    EXPECT_EQ(tool.library, "flatbuffers");
    EXPECT_TRUE(tool.from_dependency);
    EXPECT_EQ(tool.programs, std::vector<std::string>{"flatc"});
    ASSERT_TRUE(tool.when.has_value());
    auto const flatc = std::ranges::find(p.r->toolchains, "flatc", &toolchain_requirement::name);
    ASSERT_NE(flatc, p.r->toolchains.end());
    EXPECT_EQ(flatc->from, "flatbuffers");

    EXPECT_TRUE(parse_with("[tool-deps.flatbuffers]\nversion = \">= 23\"\ndefault-recipe = \"a/flatbuffers\"\ntoolchains = [\"flatc\"]\n")
                    .reports("'toolchains' in tool-deps.flatbuffers is not for a tool dependency"));
    EXPECT_TRUE(parse_with("[deps.flatbuffers]\nversion = \">= 23\"\ndefault-recipe = \"a/flatbuffers\"\ntoolchains = [\"flatc\"]\n"
        "[toolchains]\nflatc = { default-recipe = \"a/flatc\" }\n").reports("the program 'flatc' of [toolchains] is in the 'toolchains' of a dependency too"));
    EXPECT_TRUE(parse_with("[deps.flatbuffers]\nversion = \">= 23\"\ndefault-recipe = \"a/flatbuffers\"\ntoolchains = [\"Flat C\"]\n")
                    .reports("'Flat C' in 'toolchains' in deps.flatbuffers is not a valid program name"));
}

TEST(recipe, checks_the_recipes_of_toolchains)
{
    EXPECT_TRUE(parse_with("[toolchains]\nninja = { default-recipe = \"ninja\" }\n").reports("must be a recipe id \"publisher/name\""));
    EXPECT_TRUE(parse_with("[toolchains]\nrust = { targets = [\"aarch64-apple-ios\"], default-recipe = \"a/rust\" }\n")
        .reports("cannot build a toolchain with 'targets'"));
    EXPECT_TRUE(parse_with("[toolchains]\ngn = \"> 2000\"\n").r.has_value());
}

TEST(recipe, reads_a_tool_recipe)
{
    // recipe-toml.md 3.1: programs, and no products needed
    auto p = parse(head_text + source_text + platforms_text + "[tool]\nprograms = [\"ninja\", \"ninja-gen\"]\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    EXPECT_EQ(p.r->programs, (std::vector<std::string>{"ninja", "ninja-gen"}));
    EXPECT_TRUE(p.r->products.empty());
    EXPECT_TRUE(parse_with("[tool]\nprograms = [\"cmake\"]\n").r.has_value());  // a library that is also a tool
}

TEST(recipe, checks_a_tool_recipe)
{
    auto const tool = [](std::string const& body) { return parse(head_text + source_text + platforms_text + "[tool]\n" + body); };
    EXPECT_TRUE(tool("programs = []\n").reports("must name at least one program"));
    EXPECT_TRUE(tool("programs = [\"Ninja\"]\n").reports("'Ninja' in 'programs' in tool is not a valid program name"));
    EXPECT_TRUE(tool("programs = [\"a\", \"a\"]\n").reports("program 'a' is named twice"));
    EXPECT_TRUE(tool("").reports("missing required key 'programs' in tool"));
}

TEST(recipe, a_resource_may_need_a_build_machine)
{
    // recipe-toml.md 5, 9.1: release binaries of the machine only
    std::string const resource = "[[resource]]\nname = \"bin\"\nurl = \"https://x/b.zip\"\nsha256 = \"" + std::string(64, 'b') + "\"\n";
    auto p = parse_with(resource + "when = { host = [\"windows-x86_64\", \"macos-arm64\"] }\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    ASSERT_TRUE(p.r->resources[0].when.has_value());
    EXPECT_EQ(p.r->resources[0].when->host, (std::vector<std::string>{"windows-x86_64", "macos-arm64"}));

    EXPECT_TRUE(parse_with(resource + "when = { host = [\"win64\"] }\n").reports("'win64' in 'host' in resource[1].when is not a build machine"));
    EXPECT_TRUE(parse_with("[deps.zlib]\nversion = \">= 1\"\ndefault-recipe = \"a/zlib\"\nwhen = { host = [\"linux-x86_64\"] }\n")
        .reports("only the 'when' of a [[resource]] may test the build machine"));
    EXPECT_TRUE(parse_with(resource + "when = {}\n").reports("must test at least one of 'option', 'host', 'arch', 'environment' or 'not'"));
}

// recipe-toml.md 9.1: the architecture and the environment of the slice, for resources and toolchains only
TEST(recipe, conditions_on_the_architecture_and_the_environment)
{
    std::string const resource = "[[resource]]\nname = \"bin\"\nurl = \"https://x/b.zip\"\nsha256 = \"" + std::string(64, 'b') + "\"\n";
    auto p = parse_with(resource + "when = { arch = [\"arm64\"], environment = [\"windows/msvc\", \"*/ndk\"] }\n"
        "[toolchains]\nperl = { version = \">= 5.30\", default-recipe = \"apotocki/perl\", when = { arch = [\"arm64\"] } }\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    condition const& c = *p.r->resources[0].when;
    EXPECT_EQ(c.arch, std::vector<std::string>{"arm64"});
    ASSERT_TRUE(c.environment.has_value());
    ASSERT_EQ(c.environment->size(), 2u);
    EXPECT_EQ((*c.environment)[0].text(), "windows/msvc");
    EXPECT_EQ((*c.environment)[1].text(), "*/ndk");
    ASSERT_EQ(p.r->tool_dependencies.size(), 1u);
    EXPECT_EQ(p.r->tool_dependencies[0].when->arch, std::vector<std::string>{"arm64"});

    EXPECT_TRUE(parse_with(resource + "when = { arch = [\"z80\"] }\n").reports("unknown architecture 'z80' in 'arch' in resource[1].when"));
    EXPECT_TRUE(parse_with(resource + "when = { environment = [\"msvc\"] }\n")
                    .reports("'msvc' in 'environment' in resource[1].when is not a build environment <host>/<compiler>"));
    EXPECT_TRUE(parse_with(resource + "when = { not = { arch = [\"arm64\"] } }\n")
                    .reports("'arch' and 'environment' cannot be tested under 'not'; list those that hold"));
    EXPECT_TRUE(parse_with("[deps.zlib]\nversion = \">= 1\"\ndefault-recipe = \"a/zlib\"\nwhen = { arch = [\"arm64\"] }\n")
                    .reports("only the 'when' of a [[resource]] or of [toolchains] may test 'arch' and 'environment'"));
}

TEST(recipe, a_condition_on_the_build_machine_holds_on_it)
{
    condition c;
    c.host = std::vector<std::string>{std::string{host_machine()}};
    EXPECT_TRUE(condition_holds(c, {}, "", ""));
    c.host = std::vector<std::string>{"linux-arm64", "windows-arm64"};
    if (host_machine() != "linux-arm64" && host_machine() != "windows-arm64") {
        EXPECT_FALSE(condition_holds(c, {}, "", ""));
    }
}

TEST(recipe, git_source_tags)
{
    std::string const commit = "ebb370ca83af193212df4dcbadcc5d87bc0de2f0";
    auto p = parse(head_text + "[source]\ngit = \"https://github.com/lz4/lz4\"\ncommit = \"" + commit + "\"\ntag = \"v1.10.0\"\n"
        + platforms_text + product_text);
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    EXPECT_EQ(p.r->source.location.tag, "v1.10.0");
    EXPECT_EQ(p.r->source.location.commit, commit);

    auto bad = parse(head_text + "[source]\ngit = \"https://x\"\ncommit = \"" + commit + "\"\ntag = \"v 1\"\n" + platforms_text + product_text);
    EXPECT_TRUE(bad.reports("'tag' in source is not a valid tag name: 'v 1'")) << messages(bad.diags);
}

namespace {

// The first ```toml block after `heading` in a format document.
std::string doc_toml_block(std::string const& document, std::string const& heading)
{
    std::string const text = read_file(path_from_utf8(OTOK_TEST_DATA_DIR "/../../docs/formats/") / document).value_or("");
    std::size_t const at = text.find(heading);
    std::size_t const begin = text.find("```toml\n", at);
    std::size_t const end = text.find("```", begin + 8);
    if (at == std::string::npos || begin == std::string::npos || end == std::string::npos) return {};
    return text.substr(begin + 8, end - begin - 8);
}

std::string const swift_package_text = "[apple]\nswift-package = \".\"\n[[product]]\nname = \"LZ4\"\n";

parsed parse_swift_package(std::string_view extra, std::string const& platforms = platforms_text)
{
    return parse(head_text + source_text + platforms + swift_package_text + std::string{extra});
}

}

TEST(recipe, reads_the_swift_package_example)
{
    std::string const example = doc_toml_block("recipe-toml.md", "### 12.1 SwiftPM packages");
    ASSERT_FALSE(example.empty());
    auto p = parse(example);
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    EXPECT_TRUE(p.r->delivers_sources);
    EXPECT_EQ(p.r->source.patches, (std::vector<std::string>{"patches/0001-itunes-https.patch"}));
    ASSERT_EQ(p.r->products.size(), 1u);
    EXPECT_EQ(p.r->products[0].name, "Updates");
    EXPECT_TRUE(p.r->products[0].libraries.empty());
    auto const* settings = target::apple::swift_package_settings(*p.r);
    ASSERT_NE(settings, nullptr);
    EXPECT_EQ(*settings->swift_package, ".");

    auto built = parse_with("");
    ASSERT_TRUE(built.r.has_value()) << messages(built.diags);
    EXPECT_FALSE(built.r->delivers_sources);
    EXPECT_EQ(target::apple::swift_package_settings(*built.r), nullptr);
}

TEST(recipe, a_swift_package_recipe_has_no_build_lua)
{
    parsed p;
    recipe_file_check const files = [](std::string_view path, bool) { return path == "build.lua"; };
    p.r = parse_recipe(head_text + source_text + platforms_text + swift_package_text, "recipe.toml", apple_targets, files, p.diags);
    EXPECT_TRUE(p.reports("a recipe that delivers its source has no build.lua: the consumer's build compiles it")) << messages(p.diags);
}

TEST(recipe, a_swift_package_recipe_needs_nothing_a_build_needs)
{
    EXPECT_TRUE(parse_swift_package("[options.fast]\ntype = \"bool\"\ndefault = false\ndoc = \"d\"\n")
                    .reports("[options] is not for a recipe that delivers its source"));
    EXPECT_TRUE(parse_swift_package("[deps.zlib]\nversion = \">= 1\"\ndefault-recipe = \"p/zlib\"\n")
                    .reports("[deps] is not for a recipe that delivers its source"));
    EXPECT_TRUE(parse_swift_package("[toolchains]\ncmake = \">= 3\"\n").reports("[toolchains] is not for a recipe that delivers its source"));
    EXPECT_TRUE(parse(head_text + "linkage = [\"dynamic\"]\n" + source_text + platforms_text + swift_package_text)
                    .reports("'linkage' in recipe is not for a recipe that delivers its source"));
}

TEST(recipe, swift_package_products_have_a_name_only)
{
    auto p = parse(head_text + source_text + platforms_text + "[apple]\nswift-package = \".\"\n[[product]]\nname = \"LZ4\"\nlibraries = [\"lz4\"]\n"
        "apple = { module = \"CLZ4\" }\n");
    EXPECT_TRUE(p.reports("'libraries' in product[1] is not for a recipe that delivers its source")) << messages(p.diags);
    EXPECT_TRUE(p.reports("'apple' in product[1] is not for a recipe that delivers its source")) << messages(p.diags);
    EXPECT_FALSE(p.reports("unknown")) << messages(p.diags);
}

TEST(recipe, swift_packages_are_for_the_apple_target_only)
{
    target_info const* const both[] = {&target::apple::info(), &test::native_info()};
    parsed p;
    p.r = parse_recipe(head_text + source_text + "[platforms]\nsupported = [\"ios\", \"native\"]\n" + swift_package_text, "recipe.toml", both, {}, p.diags);
    EXPECT_TRUE(p.reports("the platform 'native' is of the target 'native', which takes no source from this recipe")) << messages(p.diags);
}

TEST(recipe, checks_swift_package)
{
    auto const with_apple = [](std::string const& apple) {
        return parse(head_text + source_text + platforms_text + "[apple]\n" + apple + "[[product]]\nname = \"LZ4\"\n");
    };
    EXPECT_TRUE(with_apple("swift-package = \"../x\"\n").reports("'swift-package' in apple must be \".\" or a directory inside the source"));
    EXPECT_TRUE(with_apple("swift-package = \".\"\nswift-tools-version = \"5.10\"\n")
                    .reports("'swift-tools-version' in apple is for wrappers, not for a recipe with 'swift-package'"));
    EXPECT_TRUE(with_apple("swift-package = \"Sources/Pkg\"\n").r.has_value());
}

TEST(recipe, publication_takes_privacy_manifests_through_depends)
{
    // as Boost: the manifest of the product of headers only reaches the products depending on it
    auto p = parse(head_text + source_text + platforms_text
        + "[[product]]\nname = \"H\"\nlibraries = []\napple = { privacy-manifest = \"PrivacyInfo.xcprivacy\" }\n"
          "[[product]]\nname = \"A\"\nlibraries = [\"a\"]\nheaders = false\ndepends = [\"H\"]\n"
          "[[product]]\nname = \"B\"\nlibraries = [\"b\"]\ndepends = [\"A\"]\n"
          "[[product]]\nname = \"C\"\nlibraries = [\"c\"]\n");
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    EXPECT_EQ(target::apple::check_publishable(*p.r), (std::vector<std::string>{"the product 'C' has no privacy manifest ('privacy-manifest' in apple), "
        "neither its own nor one of a product it depends on, which publishing requires (recipe-toml.md 11)"}));
}

TEST(recipe, a_privacy_manifest_of_none_needs_no_file)
{
    // a recipe directory holding no file at all: "none" is no path
    parsed p;
    recipe_file_check const files = [](std::string_view, bool) { return false; };
    p.r = parse_recipe(head_text + source_text + platforms_text + "[[product]]\nname = \"LZ4\"\nlibraries = [\"lz4\"]\n"
        "apple = { privacy-manifest = \"none\" }\n", "recipe.toml", apple_targets, files, p.diags);
    ASSERT_TRUE(p.r.has_value()) << messages(p.diags);
    EXPECT_TRUE(target::apple::check_publishable(*p.r).empty());

    parsed missing;
    missing.r = parse_recipe(head_text + source_text + platforms_text + "[[product]]\nname = \"LZ4\"\nlibraries = [\"lz4\"]\n"
        "apple = { privacy-manifest = \"Privacy.xcprivacy\" }\n", "recipe.toml", apple_targets, files, missing.diags);
    EXPECT_TRUE(missing.reports("file 'Privacy.xcprivacy' named in")) << messages(missing.diags);
}

// What a newer otok may have written (README 1.5, 4.1; docs/PLAN.md decision 98): open names in a recipe of an index or a
// git repository, never matching on this otok; strict in a recipe by `path`.
TEST(recipe, reads_names_of_a_newer_otok_as_never_matching)
{
    std::string const text = head_text + source_text
        + "[platforms]\nsupported = [\"ios\", \"iossim\", \"newos\"]\nbuild-on = [\"macos/xcode\", \"macos/newcc\"]\n"
          "architectures = { ios = [\"arm64\", \"arm128\"], iossim = [\"arm128\"], newos = [\"arm64\"] }\n"
          "[vars.flag]\ndefault = \"x\"\nnewos = \"y\"\n\"ios-arm128\" = \"z\"\n"
          "[[resource]]\nname = \"data\"\nurl = \"https://example.com/data.zip\"\nsha256 = \"" + std::string(64, 'a') + "\"\n"
          "when = { arch = [\"arm64\", \"arm128\"] }\n"
          "[deps.zlib]\nversion = \"1.3\"\ndefault-recipe = \"p/zlib\"\nwhen = { platform = [\"newos\"] }\n"
          "[newtarget]\nkey = 1\n"
        + product_text + "newtarget = { key = 1 }\n"
        + "[publish.newtarget]\nkey = 1\n";

    parsed open;
    open.r = parse_recipe(text, "recipe.toml", apple_targets, {}, open.diags, recipe_names::open);
    ASSERT_TRUE(open.r.has_value()) << messages(open.diags);
    EXPECT_EQ(open.diags.items().size(), 0u) << messages(open.diags);
    // iossim of architectures this otok does not know only: no slice of it
    EXPECT_EQ(open.r->platforms, std::vector<std::string>{"ios"});
    EXPECT_EQ(open.r->architectures.at("ios"), std::vector<std::string>{"arm64"});
    EXPECT_FALSE(open.r->architectures.contains("newos"));
    ASSERT_EQ(open.r->build_on.size(), 1u);
    EXPECT_EQ(open.r->build_on[0].text(), "macos/xcode");
    ASSERT_EQ(open.r->vars.size(), 1u);
    EXPECT_EQ(open.r->vars[0].values.size(), 1u);
    ASSERT_EQ(open.r->dependencies.size(), 1u);
    EXPECT_EQ(open.r->dependencies[0].when->platform, std::vector<std::string>{"newos"});
    EXPECT_EQ(open.r->resources[0].when->arch, (std::vector<std::string>{"arm64", "arm128"}));
    ASSERT_EQ(open.r->products.size(), 1u);

    parsed strict = parse(text);
    EXPECT_FALSE(strict.r.has_value());
    for (std::string_view expected : {"unknown platform 'newos' in", "architecture 'arm128' is not available for platform 'ios'",
             "environment 'macos/newcc' in", "unknown selector 'newos'", "unknown architecture 'arm128'", "unknown key 'newtarget' at the top level",
             "unknown key 'newtarget' in product", "unknown key 'newtarget' in publish"}) {
        EXPECT_TRUE(strict.reports(expected)) << expected << "\n" << messages(strict.diags);
    }
}

// A new key is a new API (README 1.4): left aside only when it is a table, as a target's
TEST(recipe, reads_only_tables_of_a_newer_otok_as_targets)
{
    parsed p;
    p.r = parse_recipe("something = 1\n" + head_text + source_text + platforms_text + product_text + "[sources]\nx = 1\n", "recipe.toml",
        apple_targets, {}, p.diags, recipe_names::open);
    EXPECT_FALSE(p.r.has_value());
    EXPECT_TRUE(p.reports("unknown key 'something' at the top level")) << messages(p.diags);
    EXPECT_FALSE(p.reports("unknown key 'sources'")) << messages(p.diags);
}
