// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// otok.toml, format 1: docs/formats/otok-toml.md.

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "otok/core/fs.hpp"
#include "otok/manifest/manifest.hpp"
#include "otok/target/android/android_target.hpp"
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
    std::optional<manifest> m;
    diagnostics diags;

    bool reports(std::string_view fragment) const
    {
        for (auto const& item : diags.items()) {
            if (format(item).find(fragment) != std::string::npos) return true;
        }
        return false;
    }
};

constexpr std::string_view header = "[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n";

parsed parse(std::string_view text, std::map<std::string, std::string> const& fragments = {})
{
    parsed p;
    p.m = parse_manifest(text, "otok.toml", apple_targets,
        [&](std::string const& path) -> std::optional<std::string> {
            auto it = fragments.find(path);
            if (it == fragments.end()) return std::nullopt;
            return it->second;
        },
        p.diags);
    return p;
}

parsed parse_with_header(std::string_view body)
{
    return parse(std::string{header} + std::string{body});
}

}

TEST(manifest, reads_the_specification_example)
{
    diagnostics d;
    auto m = load_manifest(path_from_utf8(OTOK_TEST_DATA_DIR "/manifest/example/otok.toml"), apple_targets, d);
    ASSERT_TRUE(m.has_value()) << messages(d);
    EXPECT_FALSE(d.has_errors());

    EXPECT_EQ(m->format, 1);
    ASSERT_EQ(m->targets.size(), 1u);
    target_settings const& apple = m->targets[0];
    EXPECT_EQ(apple.target, "apple");
    EXPECT_EQ(apple.platforms, (std::vector<std::string>{"ios", "iossim", "macosx", "catalyst"}));
    EXPECT_EQ(apple.architectures.at("iossim"), (std::vector<std::string>{"arm64"}));
    EXPECT_EQ(apple.deployment.at("ios"), "15.0");
    EXPECT_EQ(apple.output, "Otok");
    EXPECT_EQ(apple.configurations, (std::vector<std::string>{"release"}));
    EXPECT_FALSE(apple.linkage.has_value());

    ASSERT_EQ(m->deps.size(), 4u);  // three in the manifest, one in the fragment
    dependency const* openssl = m->find_dep("openssl");
    ASSERT_NE(openssl, nullptr);
    EXPECT_EQ(openssl->recipe.source, recipe_ref::kind::index);
    EXPECT_EQ(openssl->recipe.id.str(), "apotocki/openssl");
    ASSERT_TRUE(openssl->version.has_value());
    EXPECT_EQ(openssl->version->str(), "~> 3.5");
    EXPECT_EQ(std::get<bool>(openssl->options.at("no-deprecated").value), true);

    dependency const* boost = m->find_dep("boost");
    ASSERT_NE(boost, nullptr);
    EXPECT_EQ(std::get<std::vector<std::string>>(boost->options.at("libraries").value),
        (std::vector<std::string>{"json", "program_options"}));
    EXPECT_EQ(boost->platforms, (std::vector<std::string>{"ios", "iossim"}));

    dependency const* libssh2 = m->find_dep("libssh2");
    ASSERT_NE(libssh2, nullptr);
    EXPECT_EQ(libssh2->recipe.source, recipe_ref::kind::path);
    EXPECT_EQ(libssh2->recipe.path, "../otok-recipes/libssh2");
    EXPECT_FALSE(libssh2->version.has_value());

    dependency const* lz4 = m->find_dep("lz4");
    ASSERT_NE(lz4, nullptr);
    EXPECT_EQ(lz4->recipe.source, recipe_ref::kind::git);
    EXPECT_EQ(lz4->recipe.path, "otok");
    EXPECT_EQ(lz4->where.file, "otok.generated.toml");

    ASSERT_NE(m->find_tool_dep("protobuf"), nullptr);
    ASSERT_EQ(m->profiles.size(), 2u);
    EXPECT_EQ(m->find_profile("ci")->targets.at("apple").platforms, (std::vector<std::string>{"iossim"}));
    EXPECT_EQ(m->find_profile("debug")->targets.at("apple").configurations, (std::vector<std::string>{"debug"}));
    EXPECT_EQ(m->policy.deny.size(), 4u);
}

TEST(manifest, reports_unknown_keys)
{
    auto p = parse_with_header("[deps.openssl]\nrecipe = \"apotocki/openssl\"\nversion = \"~> 3.5\"\noptins = {}\n");
    EXPECT_FALSE(p.m.has_value());
    EXPECT_TRUE(p.reports("otok.toml:9:1: error: unknown key 'optins' in deps.openssl")) << messages(p.diags);
}

TEST(manifest, rejects_unknown_format_without_further_noise)
{
    auto p = parse("[otok]\nformat = 2\n[something-new]\nx = 1\n");
    EXPECT_FALSE(p.m.has_value());
    EXPECT_TRUE(p.reports("manifest format 2 needs a newer otok: this one reads format 1")) << messages(p.diags);
    EXPECT_EQ(p.diags.error_count(), 1u) << messages(p.diags);
}

TEST(manifest, requires_otok_and_target_tables)
{
    EXPECT_TRUE(parse("").reports("missing required key 'otok'"));
    EXPECT_TRUE(parse("[otok]\nformat = 1\n").reports("missing target settings: the manifest needs [apple]"));
}

TEST(manifest, checks_platforms_and_architectures)
{
    auto p = parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ipados\", \"ios\", \"ios\"]\n"
        "architectures = { ios = [\"x86_64\"], androidx = [\"arm64\"] }\ndeployment = { ios = \"15.0\" }\n");
    EXPECT_TRUE(p.reports("unknown platform 'ipados'")) << messages(p.diags);
    EXPECT_TRUE(p.reports("platform 'ios' is listed twice")) << messages(p.diags);
    EXPECT_TRUE(p.reports("architecture 'x86_64' is not available for platform 'ios'")) << messages(p.diags);
    EXPECT_TRUE(p.reports("unknown platform 'androidx'")) << messages(p.diags);
    EXPECT_TRUE(parse("[otok]\nformat = 1\n[apple]\nplatforms = []\n").reports("must not be empty"));
}

TEST(manifest, platforms_of_a_target_of_one_platform_are_optional)
{
    target_info const* const both[] = {&target::apple::info(), &test::native_info()};
    diagnostics d;
    auto m = parse_manifest("[otok]\nformat = 1\n[native]\n", "otok.toml", both, {}, d);
    ASSERT_TRUE(m) << messages(d);
    EXPECT_EQ(m->targets[0].platforms, (std::vector<std::string>{"native"}));
    // a target of many platforms has no default
    EXPECT_TRUE(parse("[otok]\nformat = 1\n[apple]\ndeployment = { ios = \"15.0\" }\n").reports("'platforms'")) << messages(d);
}

TEST(manifest, reads_the_environments)
{
    target_info const* const targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info()};
    diagnostics d;
    auto m = parse_manifest("[otok]\nformat = 1\ntool-build-on = [\"windows/clang-cl\"]\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
        "[android]\ndeployment = { android = \"24\" }\noutput = \"a\"\n[windows]\nbuild-on = [\"windows/msvc\", \"linux/clang-cl\"]\noutput = \"w\"\n",
        "otok.toml", targets, {}, d);
    ASSERT_TRUE(m) << messages(d);
    auto const texts = [](std::vector<build_environment> const& envs) {
        std::vector<std::string> out;
        for (build_environment const& e : envs) out.push_back(e.text());
        return out;
    };
    EXPECT_EQ(texts(m->tool_build_on), (std::vector<std::string>{"windows/clang-cl"}));
    ASSERT_EQ(m->targets.size(), 3u);
    EXPECT_EQ(texts(m->targets[0].build_on), (std::vector<std::string>{"macos/xcode"}));  // the target's one environment
    EXPECT_EQ(texts(m->targets[1].build_on), (std::vector<std::string>{"*/ndk"}));        // a compiler of every host
    EXPECT_EQ(texts(m->targets[2].build_on), (std::vector<std::string>{"windows/msvc", "linux/clang-cl"}));
}

TEST(manifest, checks_the_environments)
{
    target_info const* const targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info()};
    auto const with = [&](std::string const& text) {
        parsed p;
        p.m = parse_manifest("[otok]\nformat = 1\n" + text, "otok.toml", targets, {}, p.diags);
        return p;
    };
    std::string const windows = "[windows]\nbuild-on = [\"windows/msvc\"]\n";
    EXPECT_TRUE(with("[windows]\nbuild-on = []\n").reports("'build-on' in windows must not be empty"));
    EXPECT_TRUE(with("[windows]\nbuild-on = [\"msvc\"]\n").reports("'msvc' in 'build-on' in windows is not an environment"));
    EXPECT_TRUE(with("[windows]\nbuild-on = [\"windows/msvc\", \"windows/msvc\"]\n").reports("environment 'windows/msvc' is listed twice in 'build-on' in windows"));
    EXPECT_TRUE(with("[windows]\nbuild-on = [\"macos/msvc\"]\n").reports("environment 'macos/msvc' in 'build-on' in windows is not one of the windows target"));
    EXPECT_TRUE(with("[windows]\nbuild-on = [\"*/clang-cl\"]\n").reports("environment '*/clang-cl' in 'build-on' in windows is not for this target: '*' is for a compiler that builds alike on every host"));
    EXPECT_TRUE(with("tool-build-on = [\"*/msvc\"]\n" + windows).reports("environment '*/msvc' in 'tool-build-on' in otok names no system"));
    EXPECT_TRUE(with("tool-build-on = [\"windows/msvc\", \"windows-arm64/clang-cl\"]\n" + windows).reports("is a second one of the system 'windows'"));
    EXPECT_TRUE(with("tool-build-on = [\"windows/gcc\"]\n" + windows).reports("is not one of the windows target, which builds the tools of 'windows'"));
    EXPECT_TRUE(with("tool-build-on = [\"linux/gcc\"]\n" + windows).reports("names a system no target of this otok builds tools for"));
}

TEST(manifest, requires_a_deployment_version_for_every_platform)
{
    auto p = parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\", \"tvos\"]\ndeployment = { ios = \"15.0\" }\n");
    EXPECT_TRUE(p.reports("no deployment version for platform 'tvos': set 'tvos' in [apple] deployment")) << messages(p.diags);

    // catalyst falls back to the iOS version
    EXPECT_TRUE(parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"catalyst\"]\ndeployment = { ios = \"15.0\" }\n").m.has_value());

    // a profile that adds a platform needs its version too
    auto q = parse_with_header("[profile.mac]\napple = { platforms = [\"macosx\"] }\n");
    EXPECT_TRUE(q.reports("no deployment version for platform 'macosx' in profile 'mac'")) << messages(q.diags);
    EXPECT_TRUE(parse_with_header("[profile.mac]\napple = { platforms = [\"macosx\"], deployment = { macos = \"12.0\" } }\n").m.has_value());
}

TEST(manifest, checks_deployment_values)
{
    auto p = parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = 15, tvos = \"17\", vision = \"1.0\" }\n");
    EXPECT_TRUE(p.reports("'ios' in apple.deployment must be a string, not an integer")) << messages(p.diags);
    EXPECT_TRUE(p.reports("unknown deployment key 'vision'")) << messages(p.diags);
    EXPECT_TRUE(parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.\" }\n").reports("digits and dots"));
}

TEST(manifest, checks_recipe_forms)
{
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = \"Apotocki/a\"\nversion = \"1\"\n").reports("must be a recipe id"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = \"a\"\nversion = \"1\"\n").reports("must be a recipe id"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = 1\n").reports("must be a recipe id \"publisher/name\", a table"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { git = \"https://x\", rev = \"main\" }\n").reports("full 40-character commit SHA"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { git = \"https://x\", rev = \"0123456\" }\n").reports("full 40-character commit SHA"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { git = \"https://x\" }\n").reports("missing required key 'rev'"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"x\", rev = \"y\" }\n").reports("unknown key 'rev' in deps.a.recipe"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { git = \"https://x\", rev = \"0123456789abcdef0123456789abcdef01234567\", path = \"../up\" }\n")
        .reports("must be a directory inside the repository"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"..\\\\x\" }\n").reports("not a valid directory path"));
}

TEST(manifest, requires_a_version_for_index_recipes)
{
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = \"p/a\"\n").reports("missing required key 'version' in deps.a (required for a recipe from the index)"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"r/a\" }\n").m.has_value());
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = \"p/a\"\nversion = \"=> 1\"\n").reports("constraint '=> 1'"));
}

TEST(manifest, checks_library_names)
{
    EXPECT_TRUE(parse_with_header("[deps.OpenSSL]\nrecipe = { path = \"r\" }\n").reports("invalid library name 'OpenSSL'"));
    EXPECT_TRUE(parse_with_header("[deps.tools]\nrecipe = { path = \"r\" }\n").reports("the library name 'tools' is reserved"));
}

TEST(manifest, checks_option_values)
{
    auto p = parse_with_header("[deps.a]\nrecipe = { path = \"r\" }\noptions = { ok = 1, list = [], mixed = [\"a\", 1], Bad = true, f = 1.5 }\n");
    EXPECT_TRUE(p.reports("'mixed' in deps.a.options must be a boolean, an integer, a string or an array of strings")) << messages(p.diags);
    EXPECT_TRUE(p.reports("invalid option name 'Bad'")) << messages(p.diags);
    EXPECT_TRUE(p.reports("'f' in deps.a.options must be")) << messages(p.diags);
    EXPECT_EQ(p.diags.error_count(), 3u) << messages(p.diags);
}

TEST(manifest, narrows_platforms_per_dependency)
{
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"r\" }\nplatforms = [\"ios\"]\n").m.has_value());
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"r\" }\nplatforms = [\"tvos\"]\n")
        .reports("deps.a lists platform 'tvos', which is not among the project's platforms"));
}

TEST(manifest, tool_dependencies_have_no_platforms)
{
    EXPECT_TRUE(parse_with_header("[tool-deps.protobuf]\nrecipe = { path = \"r\" }\nplatforms = [\"ios\"]\n")
        .reports("a tool dependency is built for the tool slice only"));
}

TEST(manifest, local_source_overrides)
{
    auto p = parse_with_header("[deps.a]\nrecipe = { path = \"r\" }\nsource = { path = \"../a-src\" }\n");
    ASSERT_TRUE(p.m.has_value()) << messages(p.diags);
    EXPECT_EQ(p.m->deps[0].source_path, "../a-src");
}

TEST(manifest, profiles_may_only_override)
{
    EXPECT_TRUE(parse_with_header("[profile.ci]\ndeps.nope = { options = { x = true } }\n")
        .reports("profile 'ci' refers to library 'nope', which has no [deps.nope] entry"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"r\" }\n[profile.ci]\ndeps.a = { version = \"1\" }\n")
        .reports("unknown key 'version' in profile.ci.deps.a"));
    EXPECT_TRUE(parse_with_header("[profile.ci]\npolicy = {}\n").reports("unknown key 'policy' in profile.ci"));
    EXPECT_TRUE(parse_with_header("[profile.Ci]\n").reports("invalid profile name 'Ci'"));
    EXPECT_TRUE(parse_with_header("[profile.ci]\nbuild = { configuration = \"debug\" }\n").reports("unknown key 'build' in profile.ci"));
}

TEST(manifest, reads_configurations_and_linkage)
{
    target_info const* const both[] = {&target::apple::info(), &test::native_info()};
    diagnostics d;
    auto m = parse_manifest("[otok]\nformat = 1\n[native]\nplatforms = [\"native\"]\nconfigurations = [\"debug\", \"release\"]\nlinkage = \"dynamic\"\n"
                            "[deps.a]\nrecipe = { path = \"a\" }\nlinkage = \"static\"\n[deps.b]\nrecipe = { path = \"b\" }\n"
                            "[profile.p]\nnative = { configurations = [\"release\"], linkage = \"static\" }\ndeps.b = { linkage = \"dynamic\" }\n",
        "otok.toml", both, {}, d);
    ASSERT_TRUE(m) << d.items().front().message;
    target_settings const& native = m->targets[0];
    EXPECT_EQ(native.configurations, (std::vector<std::string>{"debug", "release"}));
    profile const* p = m->find_profile("p");
    EXPECT_EQ(build_configurations(native, nullptr), (std::vector<std::string>{"debug", "release"}));
    EXPECT_EQ(build_configurations(native, p), (std::vector<std::string>{"release"}));
    // the dependency's, then the target table's, then the target's default (otok-toml.md 5)
    target_info const& info = test::native_info();
    EXPECT_EQ(linkage_of(*m, nullptr, "a", info), "static");
    EXPECT_EQ(linkage_of(*m, nullptr, "b", info), "dynamic");
    EXPECT_EQ(linkage_of(*m, nullptr, "transitive", info), "dynamic");
    EXPECT_EQ(linkage_of(*m, p, "b", info), "dynamic");
    EXPECT_EQ(linkage_of(*m, p, "transitive", info), "static");
    EXPECT_EQ(linkage_of(*m, nullptr, "a", target::apple::info()), "static");  // no [apple] table: the target's default
}

// keys a target reads itself (target_info::choices), such as `stl` of [android]
TEST(manifest, reads_the_choices_of_a_target)
{
    target_info device;
    device.name = "device";
    device.platforms = {{"device", {"arm64"}, {}}};
    device.choices = {{"runtime", {"shared", "static", "none"}, {{"dynamic", "shared"}}}};
    target_info const* const targets[] = {&device};
    auto const read = [&](std::string const& keys) {
        parsed p;
        p.m = parse_manifest("[otok]\nformat = 1\n[device]\n" + keys, "otok.toml", targets, {}, p.diags);
        return p;
    };
    auto given = read("runtime = \"static\"\n");
    ASSERT_TRUE(given.m.has_value()) << messages(given.diags);
    EXPECT_EQ(given.m->targets.at(0).choices, (std::map<std::string, std::string, std::less<>>{{"runtime", "static"}}));
    auto absent = read("");
    ASSERT_TRUE(absent.m.has_value()) << messages(absent.diags);
    EXPECT_EQ(absent.m->targets.at(0).choices.at("runtime"), "shared");  // the default
    EXPECT_TRUE(read("runtime = \"dynamic\"\n")
            .reports("'runtime' in device must be \"shared\", \"static\" or \"none\", not 'dynamic': did you mean \"shared\"?"));
    auto other = read("runtime = \"fast\"\n");
    EXPECT_TRUE(other.reports("'runtime' in device must be \"shared\", \"static\" or \"none\", not 'fast'")) << messages(other.diags);
    EXPECT_FALSE(other.reports("did you mean"));
    // the target's own: no profile changes them
    EXPECT_TRUE(read("[profile.p.device]\nruntime = \"static\"\n").reports("unknown key 'runtime'"));
}

// a choice of a free value: `bundle-id-prefix` of [apple] (otok-toml.md 4)
TEST(manifest, reads_the_bundle_id_prefix)
{
    auto const apple = [](std::string const& keys) {
        return parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n" + keys);
    };
    auto absent = apple("");
    ASSERT_TRUE(absent.m.has_value()) << messages(absent.diags);
    EXPECT_EQ(absent.m->targets.at(0).choices.at("bundle-id-prefix"), "org.otok");
    auto given = apple("bundle-id-prefix = \"com.example-1.app\"\n");
    ASSERT_TRUE(given.m.has_value()) << messages(given.diags);
    EXPECT_EQ(given.m->targets.at(0).choices.at("bundle-id-prefix"), "com.example-1.app");
    for (char const* wrong : {"", ".com", "com.", "com..example", "com_example", "com example"}) {
        EXPECT_TRUE(apple("bundle-id-prefix = \"" + std::string{wrong} + "\"\n")
                .reports("'bundle-id-prefix' in apple must be letters, digits, '-' and '.', neither starting nor ending with '.' and without '..', not '"
                    + std::string{wrong} + "'"))
            << wrong;
    }
}

TEST(manifest, checks_configurations_and_linkage)
{
    auto const apple = [](std::string const& keys) {
        return parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n" + keys);
    };
    EXPECT_TRUE(apple("configurations = []\n").reports("'configurations' in apple must not be empty"));
    EXPECT_TRUE(apple("configurations = [\"fast\"]\n").reports("'configurations' in apple may hold \"release\" and \"debug\", not 'fast'"));
    EXPECT_TRUE(apple("configurations = [\"debug\", \"debug\"]\n").reports("'configurations' in apple lists 'debug' twice"));
    EXPECT_TRUE(apple("configurations = [\"debug\", \"release\"]\n")
            .reports("'configurations' in apple takes one configuration: the apple target cannot give a project's build a library per configuration"));
    EXPECT_TRUE(apple("configurations = [\"debug\"]\n").m.has_value());
    EXPECT_TRUE(apple("linkage = \"shared\"\n").reports("'linkage' in apple must be \"static\" or \"dynamic\", not 'shared': did you mean \"dynamic\"?"));
    // frameworks (otok-toml.md 4.10)
    auto dynamic = apple("linkage = \"dynamic\"\n[deps.a]\nrecipe = { path = \"a\" }\nlinkage = \"dynamic\"\n");
    EXPECT_TRUE(dynamic.m.has_value()) << messages(dynamic.diags);
    // a target building static libraries only
    target_info device;
    device.name = "device";
    device.platforms = {{"device", {"arm64"}, {}}};
    target_info const* const targets[] = {&device};
    auto const static_only = [&](std::string const& keys) {
        parsed p;
        p.m = parse_manifest("[otok]\nformat = 1\n[device]\n" + keys, "otok.toml", targets, {}, p.diags);
        return p;
    };
    EXPECT_TRUE(static_only("linkage = \"dynamic\"\n").reports("'linkage' in device must be 'static': the device target does not build dynamic libraries"));
    EXPECT_TRUE(static_only("[deps.a]\nrecipe = { path = \"a\" }\nlinkage = \"dynamic\"\n")
            .reports("deps.a asks for dynamic libraries, which the device target does not build; narrow 'platforms' of deps.a"));
    EXPECT_TRUE(static_only("[deps.a]\nrecipe = { path = \"a\" }\n[profile.p]\ndeps.a = { linkage = \"dynamic\" }\n")
            .reports("profile.p.deps.a asks for dynamic libraries, which the device target does not build"));
    EXPECT_TRUE(apple("[tool-deps.a]\nrecipe = { path = \"a\" }\nlinkage = \"dynamic\"\n")
            .reports("'linkage' in tool-deps.a is not allowed: a tool dependency is always built static"));
    EXPECT_TRUE(apple("[tool-deps.a]\nrecipe = { path = \"a\" }\nbundle-data = true\n")
            .reports("'bundle-data' in tool-deps.a is not allowed: a tool dependency does not reach the product"));
    auto bundled = apple("[deps.a]\nrecipe = { path = \"a\" }\nbundle-data = true\n");
    ASSERT_TRUE(bundled.m.has_value());
    EXPECT_TRUE(bundled.m->find_dep("a")->bundle_data);
    EXPECT_TRUE(apple("[build]\nconfiguration = \"debug\"\n").reports("unknown key 'build'"));
}

TEST(manifest, output_stays_in_the_project)
{
    EXPECT_TRUE(parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\noutput = \"../Otok\"\n")
        .reports("must be a directory inside the project"));
}

TEST(manifest, target_outputs_do_not_overlap)
{
    target_info const* const both[] = {&target::apple::info(), &test::native_info()};
    auto parse_both = [&](std::string const& native_output) {
        parsed p;
        p.m = parse_manifest("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n"
            "[native]\nplatforms = [\"native\"]\n" + native_output, "otok.toml", both, {}, p.diags);
        return p;
    };
    EXPECT_TRUE(parse_both("").reports("'output' of [native] ('Otok') overlaps 'output' of [apple] ('Otok')"));
    EXPECT_TRUE(parse_both("output = \"otok/native\"\n").reports("overlaps 'output' of [apple]"));
    auto separate = parse_both("output = \"OtokNative\"\n");
    EXPECT_TRUE(separate.m) << messages(separate.diags);
}

TEST(manifest, reads_includes)
{
    std::string const manifest_text = "[otok]\nformat = 1\ninclude = [\"gen.toml\"]\n[apple]\nplatforms = [\"ios\"]\n"
        "deployment = { ios = \"15.0\" }\n[deps.a]\nrecipe = { path = \"r\" }\n";

    auto ok = parse(manifest_text, {{"gen.toml", "[deps.b]\nrecipe = { path = \"r\" }\n"}});
    ASSERT_TRUE(ok.m.has_value()) << messages(ok.diags);
    EXPECT_EQ(ok.m->deps.size(), 2u);

    EXPECT_TRUE(parse(manifest_text).reports("cannot read the included fragment 'gen.toml'"));
    EXPECT_TRUE(parse(manifest_text, {{"gen.toml", "[deps.a]\nrecipe = { path = \"r\" }\n"}})
        .reports("library 'a' in deps is defined both in otok.toml and in gen.toml"));
    EXPECT_TRUE(parse(manifest_text, {{"gen.toml", "[otok]\nformat = 1\n"}}).reports("a fragment has no [otok] table"));
    EXPECT_TRUE(parse(manifest_text, {{"gen.toml", "[profile.x]\n"}}).reports("unknown key 'profile' at the top level"));
    EXPECT_TRUE(parse("[otok]\nformat = 1\ninclude = [\"../gen.toml\"]\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\n")
        .reports("must be a file inside the project"));
}

TEST(manifest, checks_the_license_policy)
{
    EXPECT_TRUE(parse_with_header("[policy]\ndeny-licenses = [\"GPL-3.0-only\"]\nallow-licenses = [\"MIT\"]\n").reports("not both"));
    EXPECT_TRUE(parse_with_header("[policy]\ndeny-licenses = [\"GPL 3\"]\n").reports("is not an SPDX license id"));
    EXPECT_TRUE(parse_with_header("[policy]\ndeny-licenses = [\"GPL-2.0-only WITH Classpath-exception-2.0\"]\n").m.has_value());
}

TEST(manifest, reads_fetch_locations)
{
    auto p = parse_with_header("[fetch]\nasset-mirror = \"https://files/{sha256}\"\nmirrors-only = [\"boost\"]\n"
        "[[fetch.url-rewrite]]\nfrom = \"https://archives.boost.io/release/\"\nto = \"https://files/boost/\"\n");
    ASSERT_TRUE(p.m.has_value()) << messages(p.diags);
    EXPECT_EQ(p.m->fetch.asset_mirrors, std::vector<std::string>{"https://files/{sha256}"});
    ASSERT_EQ(p.m->fetch.rewrites.size(), 1u);
    EXPECT_EQ(p.m->fetch.rewrites[0].to, "https://files/boost/");
    EXPECT_FALSE(p.m->fetch.mirrors_only);
    EXPECT_EQ(p.m->fetch.mirrors_only_libraries, std::vector<std::string>{"boost"});
}

TEST(manifest, checks_fetch_locations)
{
    EXPECT_TRUE(parse_with_header("[fetch]\nasset-mirror = [\"https://files/\"]\n").reports("'asset-mirror' in fetch must hold URL templates containing {sha256}"));
    EXPECT_TRUE(parse_with_header("[fetch]\nmirrors-only = 1\n").reports("'mirrors-only' in fetch"));
    EXPECT_TRUE(parse_with_header("[[fetch.url-rewrite]]\nfrom = \"https://a/\"\nextra = 1\n").reports("unknown key 'extra'"));
    EXPECT_TRUE(parse_with_header("[fetch]\nproxy = \"http://p\"\n").reports("unknown key 'proxy' in fetch"));
    auto bad = parse_with_header("[[fetch.url-rewrite]]\nfrom = \"https://a/{x}\"\nto = \"https://b/{y}\"\nexact = true\n");
    EXPECT_TRUE(bad.reports("fetch.url-rewrite[1]: 'to' uses {y}, which 'from' does not have")) << messages(bad.diags);
}

// otok-toml.md 6.4, docs/PLAN.md decision 95
TEST(manifest, reads_indexes)
{
    auto p = parse_with_header("[indexes.corp]\nurl = \"view+https://otok.corp.example/index\"\npublishers = [\"corp\", \"corp-tools\"]\n"
                               "[indexes.lab]\nurl = \"file:///srv/lab-index\"\n"
                               "[index-of]\n\"apotocki/zlib\" = \"corp\"\n\"q/b\" = \"default\"\n");
    ASSERT_TRUE(p.m.has_value()) << messages(p.diags);
    ASSERT_EQ(p.m->indexes.size(), 2u);
    EXPECT_EQ(p.m->indexes[0].name, "corp");
    EXPECT_EQ(p.m->indexes[0].url, "view+https://otok.corp.example/index");
    EXPECT_EQ(p.m->indexes[0].publishers, (std::vector<std::string>{"corp", "corp-tools"}));
    EXPECT_EQ(p.m->indexes[1].name, "lab");
    EXPECT_TRUE(p.m->indexes[1].publishers.empty());
    EXPECT_EQ(p.m->index_of, (std::map<std::string, std::string, std::less<>>{{"apotocki/zlib", "corp"}, {"q/b", "default"}}));
}

TEST(manifest, checks_indexes)
{
    EXPECT_TRUE(parse_with_header("[indexes.default]\nurl = \"https://x/index\"\n").reports("'default' is the index of config.toml"));
    EXPECT_TRUE(parse_with_header("[indexes.Corp]\nurl = \"https://x/index\"\n").reports("invalid index name 'Corp'"));
    EXPECT_TRUE(parse_with_header("[indexes.corp]\npublishers = [\"corp\"]\n").reports("missing required key 'url'"));
    EXPECT_TRUE(parse_with_header("[indexes.corp]\nurl = \"http://x/index\"\n").reports("'url' in indexes.corp must be the view of an index"));
    EXPECT_TRUE(parse_with_header("[indexes.corp]\nurl = \"view+http://x\"\n").reports("must be the view of an index"));
    EXPECT_TRUE(parse_with_header("[indexes.corp]\nurl = \"https://x\"\npublishers = [\"Bad_Name\"]\n").reports("invalid publisher name 'Bad_Name'"));
    EXPECT_TRUE(parse_with_header("[indexes.corp]\nurl = \"https://x\"\nextra = 1\n").reports("unknown key 'extra' in indexes.corp"));
    EXPECT_TRUE(parse_with_header("[indexes.a]\nurl = \"https://x\"\npublishers = [\"p\"]\n[indexes.b]\nurl = \"https://y\"\npublishers = [\"p\"]\n")
                    .reports("the publisher 'p' is owned by both [indexes.a] and [indexes.b]"));
    EXPECT_TRUE(parse_with_header("[index-of]\n\"zlib\" = \"default\"\n").reports("'zlib' in [index-of] is not a recipe id"));
    EXPECT_TRUE(parse_with_header("[index-of]\n\"p/zlib\" = \"corp\"\n").reports("names the index 'corp', which is neither 'default' nor one of [indexes]"));
    EXPECT_TRUE(parse_with_header("[indexes.corp]\nurl = \"https://x\"\npublishers = [\"p\"]\n[index-of]\n\"p/zlib\" = \"default\"\n")
                    .reports("takes 'p/zlib' from 'default', but [indexes.corp] owns the publisher 'p'"));
    EXPECT_TRUE(parse_with_header("[index-of]\n\"p/zlib\" = 1\n").reports("in index-of"));
}

TEST(manifest, collects_all_errors_in_one_pass)
{
    auto p = parse("[otok]\nformat = 1\nextra = 1\n[apple]\nplatforms = [\"ios\", \"tvos\"]\ndeployment = { ios = \"15.0\" }\n"
        "[deps.a]\nrecipe = \"p/a\"\n[deps.B]\nrecipe = { path = \"r\" }\n");
    EXPECT_GE(p.diags.error_count(), 4u) << messages(p.diags);
}

TEST(manifest, git_recipe_tags)
{
    std::string const rev = "0123456789abcdef0123456789abcdef01234567";
    auto p = parse_with_header("[deps.a]\nrecipe = { git = \"https://x\", rev = \"" + rev + "\", tag = \"recipes-2026.1\" }\n");
    ASSERT_TRUE(p.m.has_value()) << messages(p.diags);
    EXPECT_EQ(p.m->find_dep("a")->recipe.tag, "recipes-2026.1");
    EXPECT_EQ(p.m->find_dep("a")->recipe.rev, rev);
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { git = \"https://x\", rev = \"" + rev + "\", tag = \"a:b\" }\n")
        .reports("is not a valid tag name: 'a:b'"));
}

TEST(manifest, reads_compiler_flags)
{
    auto p = parse_with_header("cflags = [\"-fno-omit-frame-pointer\"]\n"
                               "[deps.a]\nrecipe = { path = \"r\" }\ncflags = [\"-DA=1\"]\ncxxflags = [\"-DB\"]\nldflags = [\"-Wl,-x\"]\n"
                               "[tool-deps.g]\nrecipe = { path = \"g\" }\ncflags = [\"-DG\"]\n"
                               "[profile.asan.apple]\ncflags = [\"-fsanitize=address\"]\n"
                               "[profile.asan.deps.a]\ncflags = [\"-DA=2\"]\n"
                               "[profile.asan.tool-deps.g]\nldflags = [\"-v\"]\n");
    ASSERT_TRUE(p.m) << messages(p.diags);
    EXPECT_EQ(p.m->targets[0].flags.cflags, std::vector<std::string>{"-fno-omit-frame-pointer"});
    ASSERT_EQ(p.m->deps.size(), 1u);
    EXPECT_EQ(p.m->deps[0].flags.cflags, std::vector<std::string>{"-DA=1"});
    EXPECT_EQ(p.m->deps[0].flags.cxxflags, std::vector<std::string>{"-DB"});
    EXPECT_EQ(p.m->deps[0].flags.ldflags, std::vector<std::string>{"-Wl,-x"});
    EXPECT_EQ(p.m->tool_deps[0].flags.cflags, std::vector<std::string>{"-DG"});
    ASSERT_EQ(p.m->profiles.size(), 1u);
    profile const& asan = p.m->profiles[0];
    EXPECT_EQ(asan.targets.at("apple").flags.cflags, std::vector<std::string>{"-fsanitize=address"});
    EXPECT_EQ(asan.deps.at("a").flags.cflags, std::vector<std::string>{"-DA=2"});
    EXPECT_EQ(asan.tool_deps.at("g").flags.ldflags, std::vector<std::string>{"-v"});
}

TEST(manifest, flags_that_choose_the_slice_are_otoks)
{
    for (char const* flag : {"-target", "--target=arm64-apple-ios", "-arch", "-isysroot", "--sysroot", "--sysroot=/sdk",
             "-mios-version-min=12.0", "-mmacosx-version-min=10.15", "-mtargetos=ios15", "-ffile-prefix-map=/a=/b",
             "-fdebug-prefix-map=/a=/b", "-fmacro-prefix-map=/a=/b"}) {
        auto p = parse_with_header(std::string{"[deps.a]\nrecipe = { path = \"r\" }\ncflags = [\"-O3\", \""} + flag + "\"]\n");
        EXPECT_TRUE(p.reports(std::string{"'cflags' in deps.a contains '"} + flag + "', which chooses the slice: otok gives it"))
            << flag << "\n" << messages(p.diags);
    }
    EXPECT_TRUE(parse_with_header("ldflags = [\"-arch\"]\n").reports("'ldflags' in apple contains '-arch'"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"r\" }\ncflags = [\"-march=native\", \"-mno-avx\"]\n").m);
}

TEST(manifest, reads_the_cxx_standard)
{
    target_info const* const both[] = {&target::apple::info(), &test::native_info()};
    diagnostics d;
    auto m = parse_manifest("[otok]\nformat = 1\n[native]\ncxx-standard = 23\n"
                            "[deps.a]\nrecipe = { path = \"a\" }\n[tool-deps.t]\nrecipe = { path = \"t\" }\ncxx-standard = 20\n"
                            "[profile.p]\nnative = { cxx-standard = 20 }\n",
        "otok.toml", both, {}, d);
    ASSERT_TRUE(m) << messages(d);
    profile const* p = m->find_profile("p");
    target_info const& native = test::native_info();
    // one for every library of the target: the profile's target table's, else the manifest's (otok-toml.md 5.1)
    EXPECT_EQ(standard_of(*m, nullptr, "a", false, native, language::cxx), 23);
    EXPECT_EQ(standard_of(*m, nullptr, "transitive", false, native, language::cxx), 23);
    EXPECT_EQ(standard_of(*m, p, "a", false, native, language::cxx), 20);
    // a tool dependency has its own only
    EXPECT_EQ(standard_of(*m, nullptr, "t", true, native, language::cxx), 20);
    EXPECT_EQ(standard_of(*m, nullptr, "other", true, native, language::cxx), std::nullopt);
    // none: the recipe chooses
    EXPECT_EQ(standard_of(*m, nullptr, "a", false, target::apple::info(), language::cxx), std::nullopt);
}

TEST(manifest, checks_the_cxx_standard)
{
    EXPECT_TRUE(parse_with_header("").m.has_value());
    EXPECT_TRUE(parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\ncxx-standard = 24\n")
            .reports("'cxx-standard' in apple must be 11, 14, 17, 20, 23 or 26, not 24"));
    // the libraries of a target link together: one standard for all of them
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"a\" }\ncxx-standard = 23\n")
            .reports("'cxx-standard' in deps.a is not allowed: the libraries of a target are built with one standard, so that they link together; "
                     "set it in the target table"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"a\" }\n[profile.p]\ndeps.a = { cxx-standard = 23 }\n").reports("is not allowed"));
    EXPECT_TRUE(parse_with_header("[tool-deps.t]\nrecipe = { path = \"t\" }\ncxx-standard = \"23\"\n").reports("'cxx-standard' in tool-deps.t"));
    // the standard is not a flag of the project's
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"a\" }\ncxxflags = [\"-std=c++23\"]\n")
            .reports("'cxxflags' in deps.a contains '-std=c++23': the C++ standard is set with 'cxx-standard'"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"a\" }\ncxxflags = [\"/std:c++latest\"]\n").reports("the C++ standard is set with 'cxx-standard'"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"a\" }\ncflags = [\"-std=c11\"]\n")
            .reports("'cflags' in deps.a contains '-std=c11': the C standard is set with 'c-standard'"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"a\" }\nldflags = [\"-std=c++23\"]\n").m.has_value());  // not a compiler's
}

TEST(manifest, reads_the_c_standard)
{
    target_info const* const both[] = {&target::apple::info(), &test::native_info()};
    diagnostics d;
    auto m = parse_manifest("[otok]\nformat = 1\n[native]\nc-standard = 99\ncxx-standard = 23\n"
                            "[deps.a]\nrecipe = { path = \"a\" }\n[tool-deps.t]\nrecipe = { path = \"t\" }\nc-standard = 11\n"
                            "[profile.p]\nnative = { c-standard = 17 }\n",
        "otok.toml", both, {}, d);
    ASSERT_TRUE(m) << messages(d);
    profile const* p = m->find_profile("p");
    target_info const& native = test::native_info();
    EXPECT_EQ(standard_of(*m, nullptr, "a", false, native, language::c), 99);
    EXPECT_EQ(standard_of(*m, p, "a", false, native, language::c), 17);
    EXPECT_EQ(standard_of(*m, p, "a", false, native, language::cxx), 23);  // the profile changes C's only
    EXPECT_EQ(standard_of(*m, nullptr, "t", true, native, language::c), 11);
    EXPECT_EQ(standard_of(*m, nullptr, "t", true, native, language::cxx), std::nullopt);
}

TEST(manifest, checks_the_c_standard)
{
    EXPECT_TRUE(parse("[otok]\nformat = 1\n[apple]\nplatforms = [\"ios\"]\ndeployment = { ios = \"15.0\" }\nc-standard = 18\n")
            .reports("'c-standard' in apple must be 89, 99, 11, 17 or 23, not 18"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"a\" }\nc-standard = 11\n")
            .reports("'c-standard' in deps.a is not allowed: the libraries of a target are built with one standard, so that they link together; "
                     "set it in the target table"));
    EXPECT_TRUE(parse_with_header("[deps.a]\nrecipe = { path = \"a\" }\n[profile.p]\ndeps.a = { c-standard = 11 }\n")
            .reports("set it in the profile's target table"));
}
