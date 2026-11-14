// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// Building for Apple platforms: Xcode and SDK discovery, toolchain profiles of the slices, checks of
// built libraries (docs/PLAN.md decision 25).

#include <algorithm>
#include <any>
#include <charconv>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if __has_include(<sys/utsname.h>)
#include <sys/utsname.h>
#endif

#include "otok/core/binary.hpp"
#include "otok/core/fs.hpp"
#include "otok/target/apple/apple_recipe.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/apple/privacy.hpp"

namespace otok::target::apple {

namespace fs = std::filesystem;

namespace {

#if defined(__aarch64__) || defined(_M_ARM64)
constexpr char const* machine_arch = "arm64";
#else
constexpr char const* machine_arch = "x86_64";
#endif

// The kernel version of this machine ("27.0.0"), as config.guess puts it into the name of a Mac.
std::string darwin_release()
{
#if __has_include(<sys/utsname.h>)
    struct utsname u{};
    if (::uname(&u) == 0) return u.release;
#endif
    return {};
}

struct platform_row
{
    char const* platform;
    char const* sdk;
    char const* os;             // in the triple
    char const* suffix;         // of the triple: "-simulator", "-macabi"
    char const* min_flag;       // empty: the triple is the only way to say it
    char const* variable;       // the deployment variable of the linker and other tools
    char const* cmake;          // CMAKE_SYSTEM_NAME; empty: none (macOS builds run here)
    char const* rust;           // "apple-ios-sim"
    char const* zig;            // "ios-simulator"
    char const* goos;
    int id;                     // LC_BUILD_VERSION platform
};

constexpr platform_row platforms[] = {
    {"ios", "iphoneos", "ios", "", "-mios-version-min=", "IPHONEOS_DEPLOYMENT_TARGET", "iOS", "apple-ios", "ios", "ios", 2},
    {"iossim", "iphonesimulator", "ios", "-simulator", "-mios-simulator-version-min=", "IPHONEOS_DEPLOYMENT_TARGET", "iOS", "apple-ios-sim",
        "ios-simulator", "ios", 7},
    {"macosx", "macosx", "macos", "", "-mmacosx-version-min=", "MACOSX_DEPLOYMENT_TARGET", "", "apple-darwin", "macos", "darwin", 1},
    {"catalyst", "macosx", "ios", "-macabi", "", "", "Darwin", "apple-ios-macabi", "ios-macabi", "", 6},
    {"tvos", "appletvos", "tvos", "", "-mtvos-version-min=", "TVOS_DEPLOYMENT_TARGET", "tvOS", "apple-tvos", "tvos", "", 3},
    {"tvossim", "appletvsimulator", "tvos", "-simulator", "-mtvos-simulator-version-min=", "TVOS_DEPLOYMENT_TARGET", "tvOS", "apple-tvos-sim",
        "tvos-simulator", "", 8},
    {"watchos", "watchos", "watchos", "", "-mwatchos-version-min=", "WATCHOS_DEPLOYMENT_TARGET", "watchOS", "apple-watchos", "watchos", "", 4},
    {"watchossim", "watchsimulator", "watchos", "-simulator", "-mwatchos-simulator-version-min=", "WATCHOS_DEPLOYMENT_TARGET", "watchOS",
        "apple-watchos-sim", "watchos-simulator", "", 9},
    {"xros", "xros", "xros", "", "", "XROS_DEPLOYMENT_TARGET", "visionOS", "apple-visionos", "visionos", "", 11},
    {"xrossim", "xrsimulator", "xros", "-simulator", "", "XROS_DEPLOYMENT_TARGET", "visionOS", "apple-visionos-sim", "visionos-simulator", "", 12},
};

std::string platform_name(int id)
{
    static std::map<int, char const*> const names{{1, "macOS"}, {2, "iOS"}, {3, "tvOS"}, {4, "watchOS"}, {5, "bridgeOS"}, {6, "Mac Catalyst"},
        {7, "iOS Simulator"}, {8, "tvOS Simulator"}, {9, "watchOS Simulator"}, {10, "DriverKit"}, {11, "visionOS"}, {12, "visionOS Simulator"}};
    auto const it = names.find(id);
    return it != names.end() ? it->second : "platform " + std::to_string(id);
}

// "15.0" < "15.10" < "16"; missing parts are 0.
int compare_versions(std::string_view a, std::string_view b)
{
    while (!a.empty() || !b.empty()) {
        auto const part = [](std::string_view& s) {
            int n = 0;
            auto const [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
            s.remove_prefix(static_cast<std::size_t>(end - s.data()));
            if (!s.empty() && s.front() == '.') s.remove_prefix(1);
            (void)ec;
            return n;
        };
        int const x = part(a), y = part(b);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string slashed(fs::path const& p)
{
    return path_to_utf8(p.generic_u8string());
}

result<std::string> output_of(std::vector<std::string> argv, env_vars const& env)
{
    std::string const program = argv.front();
    std::error_code ec;
    auto exit = run_process({std::move(argv), env, fs::temp_directory_path(ec), {}, true});
    if (!exit) return std::unexpected{exit.error()};
    if (!exit->success()) return fail("'" + program + "' failed with " + describe_exit(*exit));
    return trimmed(std::move(exit->output));
}

// Runs a tool whose output goes to `log`; a failure carries the end of the log.
result<void> run_logged(std::vector<std::string> argv, env_vars const& env, fs::path const& log)
{
    std::string const program = argv.front();
    std::error_code ec;
    fs::remove(log, ec);
    auto exit = run_process({std::move(argv), env, log.parent_path(), log});
    if (!exit) return std::unexpected{exit.error()};
    if (exit->success()) return {};
    std::string text = read_file(log).value_or("");
    if (text.size() > 4000) text = "…" + text.substr(text.size() - 4000);
    return fail("'" + program + "' failed with " + describe_exit(*exit) + (text.empty() ? "" : ":\n" + trimmed(text)));
}

// The files of `a` and `b` (directory trees) that only one has or that differ, relative paths.
std::vector<std::string> differing_files(fs::path const& a, fs::path const& b)
{
    auto const listing = [](fs::path const& root) {
        std::map<std::string, std::string> files;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator{root, ec}; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
            if (it->is_regular_file(ec)) files.emplace(slashed(it->path().lexically_relative(root)), read_file(it->path()).value_or(""));
        }
        return files;
    };
    auto const x = listing(a), y = listing(b);
    std::vector<std::string> out;
    for (auto const& [name, content] : x) {
        auto const it = y.find(name);
        if (it == y.end() || it->second != content) out.push_back(name);
    }
    for (auto const& [name, content] : y) {
        if (!x.contains(name)) out.push_back(name);
    }
    return out;
}

// The headers under `dir`, relative and sorted, for a generated module map.
std::vector<std::string> header_files(fs::path const& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator{dir, ec}; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        std::string const ext = path_to_utf8(it->path().extension());
        if (it->is_regular_file(ec) && (ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx")) {
            out.push_back(slashed(it->path().lexically_relative(dir)));
        }
    }
    std::ranges::sort(out);
    return out;
}

// `apple = { … }` of a product; nothing when it has none.
product_settings const* settings_of(product const& prod)
{
    auto const it = prod.target_settings.find("apple");
    return it != prod.target_settings.end() ? std::any_cast<product_settings>(&it->second) : nullptr;
}

std::string module_of(product const& prod)
{
    product_settings const* s = settings_of(prod);
    return s && !s->module.empty() ? s->module : prod.name;
}

// Whether a product's privacy manifest is a resource of its shim: not when its libraries are frameworks, which hold
// it (otok-toml.md 4.10).
bool manifest_in_shim(product const& prod, bool dynamic)
{
    product_settings const* s = settings_of(prod);
    return s && s->privacy_manifest && (!dynamic || prod.headers_only());
}

// The system libraries a product's consumers link (recipe-toml.md 11): those of `link.libraries` whose `when` holds,
// and libc++ for `runtime = "c++"`.
std::vector<std::string> system_libraries(output_product const& o)
{
    std::vector<std::string> out = o.link;
    if (o.source->runtime == "c++" && std::ranges::find(out, "c++") == out.end()) out.emplace_back("c++");
    return out;
}

// Whether a product needs a shim target (otok-toml.md 4).
bool needs_shim(output_product const& o, bool dynamic)
{
    product const& prod = *o.source;
    product_settings const* s = settings_of(prod);
    return prod.headers_only() || !system_libraries(o).empty() || (s && !s->frameworks.empty()) || manifest_in_shim(prod, dynamic);
}

// The privacy manifest of a product: the recipe's file, or the one of `privacy-manifest = "none"` (recipe-toml.md 11).
result<std::string> manifest_text(built_package const& package, product const& prod, product_settings const& s)
{
    if (*s.privacy_manifest == no_privacy_data) return std::string{detail::no_privacy_data_manifest};
    auto text = read_file(package.recipe_dir / path_from_utf8(*s.privacy_manifest));
    if (!text) return fail("cannot read the privacy manifest '" + *s.privacy_manifest + "': " + text.error().message);
    if (auto categories = declared_api_categories(*text); !categories) {
        return fail("the privacy manifest '" + *s.privacy_manifest + "' of the product '" + prod.name + "': " + categories.error().message);
    }
    return std::move(*text);
}

// The package entry of a recipe that delivers a SwiftPM package (otok-toml.md 4.6): the patched package in
// `<library>/`, its Package.swift as written, and a Swift target per product, through which the product
// of the package becomes a product of Otok.
result<void> make_source_package(built_package const& package, recipe_settings const& settings, std::span<output_product const> products,
    fs::path const& staging)
{
    fs::path const from = *settings.swift_package == "." ? package.sources : package.sources / path_from_utf8(*settings.swift_package);
    auto manifest = read_file(from / "Package.swift");
    if (!manifest) return fail("'" + *settings.swift_package + "' of the source, named in 'swift-package', has no Package.swift");
    if (detail::has_package_dependencies(*manifest)) {
        return fail("its Package.swift depends on other packages, which otok cannot pin; a recipe delivers only a package without dependencies");
    }
    if (auto ok = copy_tree(from, staging / path_from_utf8(package.library)); !ok) return fail("cannot copy the package: " + ok.error().message);
    for (output_product const& o : products) {
        std::string const name = o.source->name;
        std::string const comment = "// Generated by otok: the product '" + name + "' of the package '" + package.library + "' as a product of Otok.\n";
        fs::path const shim = staging / "shims" / path_from_utf8(name);
        std::error_code ec;
        fs::create_directories(shim, ec);
        if (auto ok = write_file_atomic(shim / path_from_utf8(name + "_otok.swift"), comment); !ok) return ok;
    }
    return {};
}

// A Swift string literal.
std::string swift_string(std::string_view s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string identifier(std::string_view s)
{
    std::string out;
    for (char c : s) out += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    return out;
}

// The name of a library's XCFramework, its binary target and the archive inside (otok-toml.md 4):
// the authors of different recipes choose output names that may meet in one build directory.
std::string framework_name(std::string const& package, std::string const& library)
{
    return package + "_" + library;
}

// Warnings about the products of a dynamic library holding a static library of another recipe, which the dynamic one
// holds a copy of (otok-toml.md 5, 4.10).
void mixed_linkage(std::span<output_package const> packages, std::vector<std::string>& warnings)
{
    std::map<std::string, std::string> linkage;
    for (output_package const& p : packages) linkage[p.library] = p.linkage;
    for (output_package const& p : packages) {
        if (p.linkage != "dynamic") continue;
        std::set<library_ref> seen;
        for (output_product const& o : p.products) {
            for (library_ref const& l : o.libraries) {
                if (l.package == p.library || !seen.insert(l).second) continue;
                auto const it = linkage.find(l.package);
                if (it == linkage.end() || it->second == "dynamic") continue;
                warnings.push_back("the dynamic library '" + p.library + "' holds a copy of the static library '" + framework_name(l.package, l.name) + "' of '"
                    + l.package + "', which its products hold too: an app linking them has its code twice; 'linkage = \"dynamic\"' in [deps."
                    + l.package + "] avoids it");
            }
        }
    }
}

void add_unique(std::vector<std::string>& to, std::vector<std::string> const& items)
{
    for (std::string const& i : items) {
        if (std::ranges::find(to, i) == to.end()) to.push_back(i);
    }
}

// otok-config.cmake: what the products of Package.swift hold, for CMake (otok-toml.md 4, docs/PLAN.md decision 92).
result<detail::cmake_package> cmake_package_of(fs::path const& dir, std::span<output_package const> packages)
{
    detail::cmake_package c;
    std::map<library_ref, std::vector<std::string>> link_of;        // library → the link settings of the products holding it
    std::map<library_ref, std::vector<std::string>> defines_of;     // library carrying a module → the defines of its product
    std::map<library_ref, std::vector<std::string>> header_link;    // { package, product of headers only } → its link settings
    std::map<library_ref, std::vector<std::string>> header_defines; // and its defines
    std::set<library_ref> cxx_of;                                   // the libraries of products needing the C++ runtime
    std::set<library_ref> header_cxx;                               // the products of headers only needing it
    std::map<std::string, fs::path> package_dirs;
    std::set<std::string> library_targets;
    std::set<std::string> with_headers;            // the dynamic libraries with an XCFramework of their headers
    std::optional<std::set<std::string>> slices;  // those every XCFramework has
    for (output_package const& p : packages) {
        if (swift_package_settings(*p.rec)) {
            c.warnings.push_back("'" + p.library + "' delivers a SwiftPM package, which has no CMake targets");
            continue;
        }
        package_dirs[p.library] = p.dir;
        if (!p.data.empty()) c.data_dirs.emplace_back(p.library, slashed(p.dir / "data"));
        bool const dynamic = p.linkage == "dynamic";
        for (std::string const& library : p.libraries) {
            std::string const name = framework_name(p.library, library);
            fs::path const framework = p.dir / (name + ".xcframework");
            c.libraries.emplace_back(name, slashed(framework));
            library_targets.insert(name);
            std::error_code ec;
            if (dynamic) {
                // frameworks, and their headers in an XCFramework of their own (otok-toml.md 4.10)
                c.shared.insert(name);
                c.frameworks.push_back(slashed(framework));
                fs::path const headers = p.dir / (name + "_headers.xcframework");
                if (fs::is_directory(dir / headers, ec)) {
                    c.libraries.emplace_back(name + "_headers", slashed(headers));
                    library_targets.insert(name + "_headers");
                    with_headers.insert(name);
                }
            }
            std::set<std::string> mine;
            for (auto it = fs::directory_iterator{dir / framework, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
                if (!it->is_directory(ec)) continue;
                std::string const identifier = path_to_utf8(it->path().filename());
                auto s = detail::cmake_slice_of(identifier);
                if (!s) continue;
                if (dynamic && *s == "macos") c.macos_frameworks.push_back(slashed(framework / path_from_utf8(identifier)));
                mine.insert(*s);
            }
            if (mine.empty()) continue;
            if (!slices) {
                slices = std::move(mine);
            } else {
                std::erase_if(*slices, [&](std::string const& s) { return !mine.contains(s); });
            }
        }
        for (output_product const& o : p.products) {
            product const& prod = *o.source;
            product_settings const* s = settings_of(prod);
            std::vector<std::string> link;
            // libc++ of `link.runtime` apart: only a consumer not linking with a C++ driver needs it (docs/PLAN.md decision 125)
            for (std::string const& l : o.link) link.push_back("-l" + l);
            for (std::string const& f : s ? s->frameworks : std::vector<std::string>{}) link.push_back("-framework " + f);
            for (std::string const& l : prod.libraries) add_unique(link_of[{p.library, l}], link);
            bool const cxx = prod.runtime == "c++";
            if (cxx) for (std::string const& l : prod.libraries) cxx_of.insert({p.library, l});
            if (cxx && prod.headers_only()) header_cxx.insert({p.library, prod.name});
            if (prod.headers_only()) {
                header_link[{p.library, prod.name}] = link;
                header_defines[{p.library, prod.name}] = o.defines;
            }
            if (!prod.headers.empty()) defines_of.emplace(library_ref{p.library, prod.headers}, o.defines);  // the module's, as packaging chose it
            if (manifest_in_shim(prod, dynamic)) c.privacy_manifests.push_back(slashed(p.dir / "shims" / path_from_utf8(prod.name) / "PrivacyInfo.xcprivacy"));
            if (s && !s->wrappers.empty()) {
                std::string list;
                for (std::string const& w : s->wrappers) list += (list.empty() ? "" : ", ") + w;
                c.warnings.push_back("the wrappers of the product '" + prod.name + "' (" + list + ") are SwiftPM targets, which CMake's consumers do not get");
            }
        }
        if (p.bundle_data && !p.data.empty()) {
            c.warnings.push_back("the data files of '" + p.library + "' are a resource bundle of SwiftPM, and CMake's consumers find them in "
                + slashed(p.dir / "data") + "/, without otok_" + identifier(p.library) + "_data_directory()");
        }
    }
    if (slices) c.slices.assign(slices->begin(), slices->end());
    for (output_package const& p : packages) {
        if (swift_package_settings(*p.rec)) continue;
        for (output_product const& o : p.products) {
            product const& prod = *o.source;
            if (library_targets.contains(prod.name)) {
                return fail("the product '" + prod.name + "' of '" + p.library + "' has the name of an XCFramework, which is a CMake target too");
            }
            detail::cmake_product cp;
            cp.name = prod.name;
            add_unique(cp.defines, o.defines);
            for (library_ref const& l : o.libraries) {
                std::string const name = framework_name(l.package, l.name);
                cp.libraries.push_back(name);
                if (with_headers.contains(name)) cp.libraries.push_back(name + "_headers");
                if (auto it = link_of.find(l); it != link_of.end()) add_unique(cp.link, it->second);
                if (cxx_of.contains(l)) cp.cxx_runtime = true;
                if (auto it = defines_of.find(l); it != defines_of.end()) add_unique(cp.defines, it->second);
            }
            for (library_ref const& h : o.header_products) {
                auto const d = package_dirs.find(h.package);
                if (d == package_dirs.end()) continue;
                cp.include_dirs.push_back(slashed(d->second / "shims" / path_from_utf8(h.name) / "include"));
                if (auto it = header_link.find(h); it != header_link.end()) add_unique(cp.link, it->second);
                if (header_cxx.contains(h)) cp.cxx_runtime = true;
                if (auto it = header_defines.find(h); it != header_defines.end()) add_unique(cp.defines, it->second);
            }
            c.products.push_back(std::move(cp));
        }
    }
    return c;
}

// Signs frameworks and the binaries changed by install_name_tool ad hoc (otok-toml.md 4.10).
constexpr char const* codesign = "/usr/bin/codesign";

// The directory of a package entry listing the undefined symbols of each library (otok-toml.md 4.4).
constexpr char const* undefined_symbols_dir = "undefined-symbols";

// The directory of a package entry listing the external symbols each library defines (otok-toml.md 4.5).
constexpr char const* defined_symbols_dir = "defined-symbols";

// The categories the privacy manifests in the frameworks of a dynamic library's XCFramework declare (otok-toml.md 4.10):
// beside Info.plist, in `<name>.framework/` or, versioned, in `Versions/A/Resources/`.
std::set<std::string> framework_api_categories(fs::path const& xcframework, std::string const& name)
{
    std::set<std::string> declared;
    std::error_code ec;
    for (auto it = fs::directory_iterator{xcframework, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
        fs::path const framework = it->path() / path_from_utf8(name + ".framework");
        for (fs::path const& plist : {framework / "PrivacyInfo.xcprivacy", framework / "Versions" / "A" / "Resources" / "PrivacyInfo.xcprivacy"}) {
            auto text = read_file(plist);
            if (!text) continue;
            if (auto categories = declared_api_categories(*text)) declared.insert(categories->begin(), categories->end());
        }
    }
    return declared;
}

// Warnings about the required-reason APIs a package's libraries use and none of the privacy manifests
// of their products declares (otok-toml.md 4.4): those of the shims, or of a dynamic library's frameworks.
void check_required_reasons(fs::path const& dir, output_package const& p, std::vector<std::string>& warnings)
{
    bool const dynamic = p.linkage == "dynamic";
    for (std::string const& library : p.libraries) {
        auto text = read_file(dir / p.dir / undefined_symbols_dir / path_from_utf8(library + ".txt"));
        if (!text) continue;  // nothing to match
        std::set<std::string> declared;
        bool manifest = false;
        for (output_product const& o : p.products) {
            product const& prod = *o.source;
            product_settings const* s = settings_of(prod);
            if (!s || !s->privacy_manifest || std::ranges::find(prod.libraries, library) == prod.libraries.end()) continue;
            manifest = true;
            if (!manifest_in_shim(prod, dynamic)) continue;  // in the frameworks, read below
            auto plist = read_file(dir / p.dir / "shims" / path_from_utf8(prod.name) / "PrivacyInfo.xcprivacy");
            if (!plist) continue;
            if (auto categories = declared_api_categories(*plist)) declared.insert(categories->begin(), categories->end());
        }
        if (dynamic && manifest) {
            std::string const name = framework_name(p.library, library);
            std::set<std::string> const in_frameworks = framework_api_categories(dir / p.dir / path_from_utf8(name + ".xcframework"), name);
            declared.insert(in_frameworks.begin(), in_frameworks.end());
        }
        std::vector<std::string> const undefined = parse_undefined_symbols(*text);
        std::vector<api_use> const uses = undeclared_uses(undefined, builtin_required_reason_apis(), declared);
        if (uses.empty()) continue;
        std::string list;
        for (api_use const& u : uses) {
            std::string symbols, categories;
            for (std::string const& s : u.symbols) symbols += (symbols.empty() ? "" : ", ") + s;
            for (std::string const& c : u.categories) categories += (categories.empty() ? "" : " or ") + c;
            list += (list.empty() ? "" : "; ") + symbols + " (" + categories + ")";
        }
        warnings.push_back("the library '" + library + "' of '" + p.library + "' uses required-reason APIs that "
            + (manifest ? "no privacy manifest of its products declares: " : "need a privacy manifest, which none of its products has: ") + list);
    }
}

// Whether two libraries of a package can meet in one image: some two products holding them do not exclude each
// other (recipe-toml.md 11.2). A library in no product meets every other.
bool can_meet(output_package const& p, std::string const& a, std::string const& b)
{
    if (!p.rec) return true;
    std::vector<product const*> with_a, with_b;
    for (output_product const& o : p.products) {
        for (product const* held : p.rec->held_products(*o.source)) {
            if (std::ranges::find(held->libraries, a) != held->libraries.end()) with_a.push_back(o.source);
            if (std::ranges::find(held->libraries, b) != held->libraries.end()) with_b.push_back(o.source);
        }
    }
    if (with_a.empty() || with_b.empty()) return true;
    for (product const* x : with_a) {
        for (product const* y : with_b) {
            if (!p.rec->exclude_each_other(*x, *y)) return true;
        }
    }
    return false;
}

// Refuses symbols that two libraries of the output both define, other than weak ones (otok-toml.md 4.5):
// the app links all their archives into one image, unless they are held only by products excluding each other.
result<void> check_symbol_overlaps(fs::path const& dir, std::span<output_package const> packages)
{
    using owner = std::pair<std::size_t, std::string>;  // { package, library }
    std::map<std::string, std::vector<owner>> owners;   // symbol → the libraries defining it
    for (std::size_t i = 0; i < packages.size(); ++i) {
        output_package const& p = packages[i];
        for (std::string const& library : p.libraries) {
            fs::path const symbols = dir / p.dir / defined_symbols_dir;
            auto text = read_file(symbols / path_from_utf8(library + ".txt"));
            if (!text) continue;  // nothing to match
            std::set<std::string> weak;
            if (auto w = read_file(symbols / path_from_utf8(library + ".weak.txt"))) {
                for (std::string& s : parse_undefined_symbols(*w)) weak.insert(std::move(s));
            }
            for (std::string const& s : parse_undefined_symbols(*text)) {
                if (!weak.contains(s)) owners[s].emplace_back(i, library);
            }
        }
    }
    auto const name = [&](owner const& o) { return "'" + o.second + "' of '" + packages[o.first].library + "'"; };
    std::map<std::pair<owner, owner>, bool> meet;       // two libraries of one package
    std::map<std::pair<owner, owner>, std::vector<std::string>> overlaps;
    for (auto const& [s, defining] : owners) {
        for (std::size_t i = 0; i < defining.size(); ++i) {
            for (std::size_t j = i + 1; j < defining.size(); ++j) {
                owner const& a = defining[i];
                owner const& b = defining[j];
                if (a.first == b.first) {
                    auto [it, fresh] = meet.try_emplace({a, b});
                    if (fresh) it->second = can_meet(packages[a.first], a.second, b.second);
                    if (!it->second) continue;
                }
                overlaps[{a, b}].push_back(s);
            }
        }
    }
    if (overlaps.empty()) return {};
    std::string message;
    for (auto const& [libraries, symbols] : overlaps) {
        std::string list;
        for (std::size_t i = 0; i < symbols.size() && i < 10; ++i) list += (list.empty() ? "" : ", ") + symbols[i];
        if (symbols.size() > 10) list += " and " + std::to_string(symbols.size() - 10) + " more";
        message += (message.empty() ? "" : "; ") + std::string{"the libraries "} + name(libraries.first) + " and " + name(libraries.second) + " both define "
            + std::to_string(symbols.size()) + (symbols.size() == 1 ? " symbol" : " symbols") + ": " + list;
    }
    return fail(message + "; an app linking both fails to link or gets one definition for every caller");
}

struct xcode_found
{
    env_vars env;               // what builds start from
    std::string developer_dir, cc, cxx, ar, ranlib, strip, nm, lipo, otool, install_name_tool;
    std::string version;        // "27.0"
    std::string build_version, description;
    std::vector<std::string> read_dirs;  // what builds read besides the system: Xcode
};

struct sdk_found
{
    std::string path, version;
};

result<xcode_found> discover(std::string const& xcode, env_vars const& environment)
{
    xcode_found x;
    if (auto it = environment.find("DEVELOPER_DIR"); it != environment.end() && !it->second.empty()) {
        x.developer_dir = it->second;
    } else if (!xcode.empty()) {
        x.developer_dir = xcode.ends_with(".app") || xcode.ends_with(".app/") ? slashed(path_from_utf8(xcode) / "Contents" / "Developer") : xcode;
    } else {
        env_vars plain{{"PATH", "/usr/bin:/bin:/usr/sbin:/sbin"}};
        auto selected = output_of({"/usr/bin/xcode-select", "-p"}, plain);
        if (!selected) return fail("no Xcode is selected (xcode-select -p): " + selected.error().message);
        x.developer_dir = *selected;
    }
    std::error_code ec;
    if (!fs::is_directory(path_from_utf8(x.developer_dir), ec)) {
        return fail("the Xcode developer directory '" + x.developer_dir + "' does not exist; set 'xcode' in config.toml or DEVELOPER_DIR");
    }

    // PATH: Xcode's and the system's; the builder adds the directories of the recipe's toolchains (docs/PLAN.md decision 38)
    std::vector<std::string> dirs{x.developer_dir + "/Toolchains/XcodeDefault.xctoolchain/usr/bin", x.developer_dir + "/usr/bin", "/usr/bin",
        "/bin", "/usr/sbin", "/sbin"};
    x.read_dirs.push_back(x.developer_dir);
    std::string path;
    for (std::string const& d : dirs) path += (path.empty() ? "" : ":") + d;
    x.env = {{"PATH", path}, {"DEVELOPER_DIR", x.developer_dir}};

    for (auto [name, out] : {std::pair{"clang", &x.cc}, std::pair{"clang++", &x.cxx}, std::pair{"ar", &x.ar}, std::pair{"ranlib", &x.ranlib},
             std::pair{"strip", &x.strip}, std::pair{"nm", &x.nm}, std::pair{"lipo", &x.lipo}, std::pair{"otool", &x.otool},
             std::pair{"install_name_tool", &x.install_name_tool}}) {
        auto found = output_of({"/usr/bin/xcrun", "--find", name}, x.env);
        if (!found) return fail("xcrun cannot find '" + std::string{name} + "' in '" + x.developer_dir + "': " + found.error().message);
        *out = *found;
    }
    auto version = output_of({"/usr/bin/xcrun", "xcodebuild", "-version"}, x.env);
    if (!version) return fail("cannot read the Xcode version: " + version.error().message);
    std::string const line = version->substr(0, version->find('\n'));  // "Xcode 27.0"
    if (auto at = version->find("Build version "); at != std::string::npos) x.build_version = trimmed(version->substr(at + 14));
    if (x.build_version.empty()) return fail("cannot read the Xcode build version from '" + *version + "'");
    if (line.starts_with("Xcode ")) x.version = trimmed(line.substr(6));
    x.description = line + " (" + x.build_version + ") at '" + x.developer_dir + "'";
    return x;
}

class apple_build_target final : public build_target
{
public:
    apple_build_target(std::string xcode, env_vars environment) : xcode_{std::move(xcode)}, environment_{std::move(environment)} {}

    target_info const& info() const override { return apple::info(); }

    std::string tool_arch() const override { return machine_arch; }

    // `macos/xcode` (common 4.2): the Xcode of this Mac
    result<void> find_compiler(build_environment const& environment) override
    {
        if (environment.compiler != "xcode") return fail("the Apple target has no compiler '" + environment.compiler + "'");
        if (!found_) found_ = discover(xcode_, environment_);
        if (!*found_) return std::unexpected{found_->error()};
        return {};
    }

    result<std::string> tools_path() override
    {
        if (!found_) found_ = discover(xcode_, environment_);
        if (!*found_) return std::unexpected{found_->error()};
        return (*found_)->env.at("PATH");
    }

    result<toolchain_profile> profile(slice_spec const& slice) override
    {
        auto names = detail::names_of(slice);
        if (!names) return fail("the Apple target has no slice " + slice_name(slice));
        if (!found_) found_ = discover(xcode_, environment_);
        if (!*found_) return std::unexpected{found_->error()};
        xcode_found const& x = **found_;
        auto sdk = sdk_of(names->sdk, x);
        if (!sdk) return std::unexpected{sdk.error()};
        bool const debug = slice.configuration == "debug";
        bool const catalyst = slice.platform == "catalyst";

        toolchain_profile p;
        for (std::string const& d : x.read_dirs) p.read_paths.push_back(path_from_utf8(d));
        p.triple = names->triple;
        p.identity = {{"xcode", x.build_version}, {"sdk", names->sdk + " " + sdk->version}};
        p.description = x.description;
        p.autotools_host = names->autotools_host;
        // the Mac runs the programs of macOS and Mac Catalyst, not those of the devices and simulators
        if (slice.platform != "macosx" && !catalyst) p.autotools_build = detail::autotools_build(machine_arch, darwin_release());
        p.b2_toolset = "darwin";
        p.b2_properties = names->b2_properties;
        p.meson_host = {{"system", "darwin"}, {"subsystem", names->meson_subsystem}, {"kernel", "xnu"}, {"cpu_family", names->meson_cpu_family},
            {"cpu", slice.arch}, {"endian", "little"}};
        p.prefix_map_flag = "-ffile-prefix-map=";
        slice_toolchain& t = p.toolchain;
        t.cc = x.cc;
        t.cxx = x.cxx;
        t.ar = x.ar;
        t.ranlib = x.ranlib;
        t.strip = x.strip;
        t.nm = x.nm;
        t.install_name_tool = x.install_name_tool;  // @rpath/ install names of dynamic libraries (build-lua-api.md 6.2)
        // the target and the SDK, part of the compiler's command too: libtool links dynamic libraries without -target
        // (build-lua-api.md 3.1)
        t.driver_flags = {"-target", names->triple, "-isysroot", sdk->path};
        t.cflags = t.driver_flags;
        t.ldflags = t.cflags;
        // room for the install names of frameworks, which the target gives dynamic libraries (otok-toml.md 4.10)
        if (slice.linkage == "dynamic") t.ldflags.push_back("-Wl,-headerpad_max_install_names");
        if (catalyst) {
            std::string const support = sdk->path + "/System/iOSSupport";
            t.cflags.insert(t.cflags.end(), {"-isystem", support + "/usr/include", "-iframework", support + "/System/Library/Frameworks"});
            t.ldflags.insert(t.ldflags.end(), {"-L" + support + "/usr/lib", "-F" + support + "/System/Library/Frameworks"});
        }
        // the configuration's (build-lua-api.md 3.1)
        std::vector<std::string> const configuration = debug ? std::vector<std::string>{"-O0", "-g"} : std::vector<std::string>{"-O2", "-DNDEBUG"};
        t.cflags.insert(t.cflags.end(), configuration.begin(), configuration.end());
        t.cxxflags = t.cflags;
        t.env = x.env;
        t.env["SDKROOT"] = sdk->path;
        t.env["ZERO_AR_DATE"] = "1";  // reproducible archives
        t.env["PKG_CONFIG_LIBDIR"] = "";  // only dependencies, nothing from the machine
        if (!names->deployment_variable.empty() && !slice.deployment.empty()) t.env[names->deployment_variable] = slice.deployment;
        t.env["CC"] = t.cc;
        t.env["CXX"] = t.cxx;
        t.env["AR"] = t.ar;
        t.env["RANLIB"] = t.ranlib;

        for (auto [field, value] : {std::pair{"rust_target", &names->rust_target}, std::pair{"zig_target", &names->zig_target},
                 std::pair{"goos", &names->goos}, std::pair{"goarch", &names->goarch}}) {
            if (!value->empty()) p.target_fields[field] = *value;
        }
        p.target_tables["apple"] = {{"sdk", names->sdk}, {"sdk_path", sdk->path}, {"min_version", slice.deployment},
            {"version_min_flag", names->version_min_flag}};
        p.cmake_toolchain = detail::cmake_toolchain({*names, slice.arch, slice.deployment, sdk->path, t.cc, t.cxx, t.ar, t.ranlib});
        return p;
    }

    // A dynamic library is named after its framework as soon as it is built, so its dependents load it under that name
    // (otok-toml.md 4.10); the references between the libraries of the slice change with it.
    result<void> prepare_libraries(std::string const& package, std::map<std::string, fs::path> const& libraries, slice_spec const& slice) override
    {
        if (slice.linkage != "dynamic") return {};
        if (!found_ || !*found_) return fail("the Apple toolchain was not discovered");
        xcode_found const& x = **found_;
        std::map<std::string, std::string> renamed;          // install name → the framework's
        std::vector<std::pair<fs::path, std::string>> dylibs;  // the file, its new install name
        std::error_code ec;
        for (auto const& [name, lib] : libraries) {
            if (binary_kind_of(lib) != binary_kind::macho_dylib) continue;
            fs::path const file = fs::canonical(lib, ec);
            if (ec) return fail("cannot resolve '" + slashed(lib) + "': " + ec.message());
            std::string const id = detail::framework_install_name(framework_name(package, name), detail::versioned_framework(slice.platform));
            renamed[load_names_of(file).id] = id;
            dylibs.emplace_back(file, id);
        }
        for (auto const& [file, id] : dylibs) {
            std::vector<std::string> argv{x.install_name_tool, "-id", id};
            for (std::string const& needed : load_names_of(file).needed) {
                if (auto it = renamed.find(needed); it != renamed.end() && it->second != needed) argv.insert(argv.end(), {"-change", needed, it->second});
            }
            argv.push_back(path_to_utf8(file));
            if (auto ok = output_of(argv, x.env); !ok) return fail("install_name_tool of '" + slashed(file) + "': " + ok.error().message);
            // the change voids the linker's signature, without which arm64 loads nothing
            if (auto ok = output_of({codesign, "--force", "--sign", "-", path_to_utf8(file)}, x.env); !ok) {
                return fail("codesign of '" + slashed(file) + "': " + ok.error().message);
            }
        }
        return {};
    }

    result<void> check_library(fs::path const& library, slice_spec const& slice) override
    {
        if (!found_ || !*found_) return fail("the Apple toolchain was not discovered");
        xcode_found const& x = **found_;
        auto archs = output_of({x.lipo, "-archs", path_to_utf8(library)}, x.env);
        if (!archs) return std::unexpected{archs.error()};
        if (*archs != slice.arch) return fail("it is for the architectures '" + *archs + "', not for '" + slice.arch + "'");
        auto load_commands = output_of({x.otool, "-l", path_to_utf8(library)}, x.env);
        if (!load_commands) return std::unexpected{load_commands.error()};
        return detail::check_build_versions(detail::parse_build_versions(*load_commands), slice);
    }

    // One XCFramework per library output name (otok-toml.md 4)
    result<void> make_package(built_package const& package, recipe const& rec, std::span<output_product const> products,
        fs::path const& staging) override
    {
        if (recipe_settings const* s = swift_package_settings(rec)) return make_source_package(package, *s, products, staging);
        auto found = xcode();
        if (!found) return std::unexpected{found.error()};
        xcode_found const& x = **found;

        // the libraries carrying headers, and the product whose module they carry
        std::map<std::string, output_product const*> modules;
        for (output_product const& o : products) {
            product const& prod = *o.source;
            if (!prod.headers.empty()) modules.emplace(prod.headers, &o);
        }
        // the slices of each platform, in the order of target_info
        std::vector<std::pair<std::string, std::vector<built_slice const*>>> groups;
        for (platform_info const& pi : info().platforms) {
            std::vector<built_slice const*> slices;
            for (built_slice const& s : package.slices) {
                if (s.spec.platform == pi.name) slices.push_back(&s);
            }
            if (!slices.empty()) groups.emplace_back(pi.name, std::move(slices));
        }

        // dynamic libraries are frameworks (otok-toml.md 4.10), holding the privacy manifests of their products
        bool const dynamic = package.slices.front().spec.linkage == "dynamic";
        std::map<std::string, std::pair<std::string, std::string>> manifests;  // library → the manifest's text, the product giving it
        for (output_product const& o : products) {
            product const& prod = *o.source;
            product_settings const* s = settings_of(prod);
            if (!dynamic || !s || !s->privacy_manifest || prod.headers_only()) continue;
            auto text = manifest_text(package, prod, *s);
            if (!text) return std::unexpected{text.error()};
            for (std::string const& l : prod.libraries) {
                auto const [it, fresh] = manifests.emplace(l, std::pair{*text, prod.name});
                if (!fresh && it->second.first != *text) {
                    return fail("the products '" + it->second.second + "' and '" + prod.name + "' give the dynamic library '" + l + "' different privacy manifests");
                }
            }
        }
        std::string bundle_prefix = "org.otok";
        if (auto it = package.slices.front().spec.choices.find("bundle-id-prefix"); it != package.slices.front().spec.choices.end()) bundle_prefix = it->second;

        std::error_code ec;
        fs::path const work = staging.parent_path() / (path_to_utf8(staging.filename()) + "-work");
        fs::remove_all(work, ec);
        auto const done = [&](result<void> r) {
            fs::remove_all(work, ec);
            return r;
        };
        for (auto const& [library, unused] : package.slices.front().output.libraries) {
            std::string const name = framework_name(package.library, library);
            std::vector<std::string> argv{"/usr/bin/xcrun", "xcodebuild", "-create-xcframework"};
            std::vector<std::string> headers_argv = argv;  // of a dynamic library (otok-toml.md 4.10)
            auto const module = modules.find(library);
            std::set<std::string> undefined;  // of every platform, for the required-reason API check (otok-toml.md 4.4)
            std::set<std::string> defined, weak;  // of every platform, for the checks of otok-toml.md 4.5
            for (auto const& [platform, slices] : groups) {
                fs::path const dir = work / library / platform;
                fs::create_directories(dir, ec);
                fs::path const framework = dir / (name + ".framework");
                bool const versioned = detail::versioned_framework(platform);
                fs::path const contents = versioned ? framework / "Versions" / "A" : framework;
                fs::path const lib = dynamic ? contents / path_from_utf8(name) : dir / ("lib" + name + ".a");
                fs::create_directories(lib.parent_path(), ec);
                if (slices.size() == 1) {
                    fs::copy_file(slices.front()->output.libraries.at(library), lib, ec);
                    if (ec) return done(fail("cannot copy the library '" + library + "': " + ec.message()));
                } else {
                    std::vector<std::string> lipo{x.lipo, "-create"};
                    for (built_slice const* s : slices) lipo.push_back(path_to_utf8(s->output.libraries.at(library)));
                    lipo.insert(lipo.end(), {"-output", path_to_utf8(lib)});
                    if (auto ok = run_logged(lipo, x.env, work / "lipo.log"); !ok) return done(fail("lipo of '" + library + "' for " + platform + ": " + ok.error().message));
                }
                if (dynamic) {
                    auto const manifest = manifests.find(library);
                    detail::framework_plist plist{name, detail::bundle_identifier(bundle_prefix, package.library, library),
                        detail::bundle_short_version(rec.version->str()), std::to_string(rec.revision), detail::supported_platform_of(platform), {}, {}};
                    if (platform == "macosx") plist.minimum_key = "LSMinimumSystemVersion";
                    else if (platform != "catalyst") plist.minimum_key = "MinimumOSVersion";
                    plist.minimum = slices.front()->spec.deployment;
                    if (auto ok = make_framework(framework, contents, name, plist, manifest != manifests.end() ? &manifest->second.first : nullptr, x); !ok) {
                        return done(fail("the framework of '" + library + "' for " + platform + ": " + ok.error().message));
                    }
                    argv.insert(argv.end(), {"-framework", path_to_utf8(framework)});
                } else {
                    argv.insert(argv.end(), {"-library", path_to_utf8(lib)});
                }
                auto symbols = output_of({x.nm, "-u", "-j", path_to_utf8(lib)}, x.env);
                if (!symbols) return done(fail("nm of '" + library + "' for " + platform + ": " + symbols.error().message));
                for (std::string& s : parse_undefined_symbols(*symbols)) undefined.insert(std::move(s));
                auto exported = output_of({x.nm, "-g", "-U", "-m", path_to_utf8(lib)}, x.env);
                if (!exported) return done(fail("nm of '" + library + "' for " + platform + ": " + exported.error().message));
                defined_symbols found_symbols = parse_defined_symbols(*exported);
                for (std::string& s : found_symbols.all) defined.insert(std::move(s));
                for (std::string& s : found_symbols.weak) weak.insert(std::move(s));
                if (module == modules.end()) continue;

                // headers: the same in every slice of the platform, or kept per architecture
                auto const headers_of = [&](built_slice const* s) -> std::optional<fs::path> {
                    if (auto it = s->output.library_headers.find(library); it != s->output.library_headers.end()) return it->second;
                    return s->output.headers;
                };
                product const& prod = *module->second->source;
                std::vector<std::string> const& defines = module->second->defines;
                std::string const module_name = module_of(prod);
                std::vector<fs::path> roots;
                for (built_slice const* s : slices) {
                    auto const root = headers_of(s);
                    if (!root) return done(fail("the library '" + library + "' carries the headers of '" + module_name + "', but build returned none for "
                        + slice_name(s->spec)));
                    roots.push_back(*root);
                }
                std::set<std::string> per_arch, partial;
                for (std::size_t i = 1; i < roots.size(); ++i) {
                    for (std::string const& f : differing_files(roots.front(), roots[i])) {
                        bool const both = fs::is_regular_file(roots.front() / path_from_utf8(f), ec) && fs::is_regular_file(roots[i] / path_from_utf8(f), ec);
                        (both ? per_arch : partial).insert(f);
                    }
                }
                if (!partial.empty()) {
                    std::string list;
                    for (std::string const& f : partial) list += (list.empty() ? "" : ", ") + f;
                    return done(fail("only some slices of " + platform + " have these headers of '" + library + "': " + list));
                }
                fs::path const headers = dir / "Headers";
                if (auto ok = copy_tree(roots.front(), headers); !ok) return done(fail("cannot copy the headers of '" + library + "': " + ok.error().message));
                for (std::string const& f : per_arch) {
                    std::vector<std::string> archs;
                    for (std::size_t i = 0; i < slices.size(); ++i) {
                        std::string const& arch = slices[i]->spec.arch;
                        fs::path const copy = headers / detail::arch_headers_dir / path_from_utf8(arch) / path_from_utf8(f);
                        fs::create_directories(copy.parent_path(), ec);
                        fs::copy_file(roots[i] / path_from_utf8(f), copy, fs::copy_options::overwrite_existing, ec);
                        if (ec) return done(fail("cannot copy the header '" + f + "' of '" + library + "': " + ec.message()));
                        archs.push_back(arch);
                    }
                    if (auto ok = write_file_atomic(headers / path_from_utf8(f), detail::arch_header(f, archs)); !ok) return done(ok);
                }
                fs::path const map_dir = headers / path_from_utf8(module_name);
                std::vector<std::string> header_list, textual;
                for (std::string& h : header_files(headers)) (h.starts_with(std::string{detail::arch_headers_dir} + "/") ? textual : header_list).push_back(std::move(h));
                fs::create_directories(map_dir, ec);
                if (!defines.empty()) {
                    if (auto ok = write_file_atomic(map_dir / "otok_defines.h", detail::defines_header(module_name, defines)); !ok) return done(ok);
                }
                std::optional<fs::path> given;
                if (auto it = slices.front()->output.library_module_maps.find(library); it != slices.front()->output.library_module_maps.end()) {
                    given = it->second;
                } else {
                    given = slices.front()->output.module_map;
                }
                if (given) {
                    auto text = read_file(*given);
                    if (!text) return done(fail("cannot read the module map of '" + library + "': " + text.error().message));
                    if (!defines.empty() && text->find("otok_defines.h") == std::string::npos) {
                        return done(fail("the module map of '" + library + "' that build returned does not list otok_defines.h, which holds the defines of the product '"
                            + prod.name + "'"));
                    }
                    if (auto ok = write_file_atomic(map_dir / "module.modulemap", *text); !ok) return done(ok);
                } else if (auto ok = write_file_atomic(map_dir / "module.modulemap", detail::module_map(module_name, header_list, textual, !defines.empty())); !ok) {
                    return done(ok);
                }
                if (dynamic) {
                    auto stub = headers_archive(dir, name, slices, x);
                    if (!stub) return done(fail("the archive carrying the headers of '" + library + "' for " + platform + ": " + stub.error().message));
                    headers_argv.insert(headers_argv.end(), {"-library", path_to_utf8(*stub), "-headers", path_to_utf8(headers)});
                } else {
                    argv.insert(argv.end(), {"-headers", path_to_utf8(headers)});
                }
            }
            argv.insert(argv.end(), {"-output", path_to_utf8(staging / (name + ".xcframework"))});
            if (auto ok = run_logged(argv, x.env, work / "xcodebuild.log"); !ok) return done(fail("the XCFramework of '" + library + "': " + ok.error().message));
            if (headers_argv.size() > 3) {
                headers_argv.insert(headers_argv.end(), {"-output", path_to_utf8(staging / (name + "_headers.xcframework"))});
                if (auto ok = run_logged(headers_argv, x.env, work / "xcodebuild.log"); !ok) {
                    return done(fail("the XCFramework of the headers of '" + library + "': " + ok.error().message));
                }
            }
            std::string list;
            for (std::string const& s : undefined) list += s + "\n";
            fs::create_directories(staging / undefined_symbols_dir, ec);
            if (auto ok = write_file_atomic(staging / undefined_symbols_dir / path_from_utf8(library + ".txt"), list); !ok) return done(ok);
            list.clear();
            for (std::string const& s : defined) list += s + "\n";
            fs::create_directories(staging / defined_symbols_dir, ec);
            if (auto ok = write_file_atomic(staging / defined_symbols_dir / path_from_utf8(library + ".txt"), list); !ok) return done(ok);
            list.clear();
            for (std::string const& s : weak) list += s + "\n";
            if (auto ok = write_file_atomic(staging / defined_symbols_dir / path_from_utf8(library + ".weak.txt"), list); !ok) return done(ok);
        }

        // shim targets and wrappers (otok-toml.md 4)
        for (output_product const& o : products) {
            product const& prod = *o.source;
            product_settings const* s = settings_of(prod);
            std::string const header = prod.name + "_otok.h";
            if (prod.headers_only()) {
                // the headers, once for every platform, in the shim's include/ (recipe-toml.md 11.1)
                if (s && (!s->module.empty() || !s->wrappers.empty())) {
                    return done(fail("the product '" + prod.name + "' has headers only and no module, so it takes no apple.module or apple.wrappers"));
                }
                built_slice const& first = package.slices.front();
                for (built_slice const& sl : package.slices) {
                    if (!sl.output.headers) {
                        return done(fail("the product '" + prod.name + "' has headers only, but build returned no single headers directory for "
                            + slice_name(sl.spec)));
                    }
                    // the headers slice is every slice (recipe-toml.md 11.3)
                    if (*sl.output.headers == *first.output.headers) continue;
                    if (std::string const differ = first_difference(*first.output.headers, *sl.output.headers); !differ.empty()) {
                        return done(fail("the header '" + differ + "' of the product '" + prod.name + "' differs between " + slice_name(first.spec) + " and "
                            + slice_name(sl.spec) + "; a product of headers only has the same headers on every platform"));
                    }
                }
                fs::path const include = staging / "shims" / path_from_utf8(prod.name) / "include";
                if (auto ok = copy_tree(*first.output.headers, include); !ok) {
                    return done(fail("cannot copy the headers of '" + prod.name + "': " + ok.error().message));
                }
                // only the shim's header: the others are reached through the search path, never as a module
                std::string const map = "module " + prod.name + "_otok {\n    header \"" + header + "\"\n    export *\n}\n";
                if (auto ok = write_file_atomic(include / "module.modulemap", map); !ok) return done(ok);
            }
            if (needs_shim(o, dynamic)) {
                // SwiftPM wants the public headers directory of a C target
                fs::path const shim = staging / "shims" / path_from_utf8(prod.name);
                fs::create_directories(shim / "include", ec);
                std::string const symbol = "otok_shim_" + identifier(prod.name);
                std::string const comment = "// Generated by otok: the target carrying the link settings and resources of the product '" + prod.name + "'.\n";
                if (auto ok = write_file_atomic(shim / "include" / path_from_utf8(header), comment + "int " + symbol + "(void);\n"); !ok) return done(ok);
                if (auto ok = write_file_atomic(shim / "shim.c", comment + "#include \"" + header + "\"\n\nint " + symbol + "(void) { return 0; }\n"); !ok) {
                    return done(ok);
                }
                if (manifest_in_shim(prod, dynamic)) {
                    auto text = manifest_text(package, prod, *s);
                    if (!text) return done(std::unexpected{text.error()});
                    if (auto ok = write_file_atomic(shim / "PrivacyInfo.xcprivacy", *text); !ok) return done(ok);
                }
            }
            for (std::string const& w : s ? s->wrappers : std::vector<std::string>{}) {
                fs::path const to = staging / "wrappers" / path_from_utf8(w);
                if (fs::exists(to, ec)) continue;
                if (auto ok = copy_tree(package.recipe_dir / "wrappers" / path_from_utf8(w), to); !ok) {
                    return done(fail("cannot copy the wrapper '" + w + "': " + ok.error().message));
                }
            }
        }

        // `bundle-data` (otok-toml.md 4, 6): data/, where the packager puts the data files, is also a target whose
        // bundle holds them, with a function giving the bundle's directory of them
        if (package.bundle_data && !package.slices.empty() && !package.slices.front().output.data.empty()) {
            std::string const id = identifier(package.library);
            std::string const header = "otok_" + id + "_data.h";
            std::string const function = "otok_" + id + "_data_directory";
            std::string const comment = "// Generated by otok: where the data files of '" + package.library + "' are, in the bundle SwiftPM makes for them.\n";
            fs::path const data = staging / "data";
            fs::create_directories(data / "include", ec);
            std::string const declaration = comment + "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
                "// The directory of the bundle's resources, which holds the data files (UTF-8, valid for the whole run).\n"
                "char const* " + function + "(void);\n\n#ifdef __cplusplus\n}\n#endif\n";
            if (auto ok = write_file_atomic(data / "include" / path_from_utf8(header), declaration); !ok) return done(ok);
            std::string const definition = comment + "#import <Foundation/Foundation.h>\n#include <string.h>\n#include \"" + header + "\"\n\n"
                "char const* " + function + "(void)\n{\n    static char const* path;\n    static dispatch_once_t once;\n"
                "    dispatch_once(&once, ^{ path = strdup(SWIFTPM_MODULE_BUNDLE.resourcePath.fileSystemRepresentation); });\n"
                "    return path;\n}\n";
            if (auto ok = write_file_atomic(data / "otok_data.m", definition); !ok) return done(ok);
        }
        return done({});
    }

    // Package.swift (otok-toml.md 4)
    result<void> finish_output(fs::path const& dir, std::span<output_package const> packages, std::map<std::string, std::string> const& deployment,
        std::vector<std::string>& warnings) const override
    {
        detail::swift_package swift;
        swift.tools_version = "5.9";  // the first to know .visionOS
        std::map<std::string, std::string> owners;  // "library <binary target>", "product <name>", "module <name>" → library
        std::map<std::string, std::string> header_owners;  // header path in Headers/ → its XCFramework
        std::set<std::string> header_targets;              // the dynamic libraries with an XCFramework of their headers
        for (output_package const& p : packages) {
            if (auto it = p.rec->target_settings.find("apple"); it != p.rec->target_settings.end()) {
                if (auto const* s = std::any_cast<recipe_settings>(&it->second); s && s->swift_tools_version) {
                    if (compare_versions(s->swift_tools_version->str(), swift.tools_version) > 0) swift.tools_version = s->swift_tools_version->str();
                }
            }
            for (std::string const& library : p.libraries) {
                std::string const name = framework_name(p.library, library);
                if (auto [it, fresh] = owners.emplace("library " + name, p.library); !fresh) {
                    return fail("the libraries '" + it->second + "' and '" + p.library + "' both have an XCFramework named '" + name + "'");
                }
                fs::path const framework = p.dir / (name + ".xcframework");
                swift.binary_targets.emplace_back(name, slashed(framework));
                // the headers of a dynamic library: an XCFramework of their own (otok-toml.md 4.10)
                std::vector<fs::path> carrying{framework};
                std::error_code ec;
                if (fs::path const headers = p.dir / (name + "_headers.xcframework"); fs::is_directory(dir / headers, ec)) {
                    if (auto [it, fresh] = owners.emplace("library " + name + "_headers", p.library); !fresh) {
                        return fail("the libraries '" + it->second + "' and '" + p.library + "' both have an XCFramework named '" + name + "_headers'");
                    }
                    swift.binary_targets.emplace_back(name + "_headers", slashed(headers));
                    header_targets.insert(name);
                    carrying.push_back(headers);
                }
                // Xcode merges the headers of all XCFrameworks into one directory
                std::set<std::string> mine;
                for (fs::path const& xcf : carrying) {
                    for (auto it = fs::directory_iterator{dir / xcf, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
                        fs::path const headers = it->path() / "Headers";
                        if (!fs::is_directory(headers, ec)) continue;
                        for (auto h = fs::recursive_directory_iterator{headers, ec}; !ec && h != fs::recursive_directory_iterator{}; h.increment(ec)) {
                            if (h->is_regular_file(ec)) mine.insert(slashed(h->path().lexically_relative(headers)));
                        }
                    }
                    ec.clear();
                }
                for (std::string const& h : mine) {
                    if (auto [it, fresh] = header_owners.emplace(h, name); !fresh) {
                        if (it->second.starts_with("the product")) {
                            return fail(it->second + " and the XCFramework '" + name + "' both have the header '" + h + "', and a consumer would see one of them");
                        }
                        return fail("the XCFrameworks '" + it->second + "' and '" + name + "' both have the header '" + h
                            + "', and Xcode merges their headers into one directory");
                    }
                }
            }
            // the headers of products of headers only: an include path of every target using them, beside
            // the merged headers of the XCFrameworks
            for (output_product const& o : p.products) {
                product const& prod = *o.source;
                if (!prod.headers_only()) continue;
                fs::path const include = dir / p.dir / "shims" / path_from_utf8(prod.name) / "include";
                std::error_code ec;
                for (auto h = fs::recursive_directory_iterator{include, ec}; !ec && h != fs::recursive_directory_iterator{}; h.increment(ec)) {
                    std::string const path = slashed(h->path().lexically_relative(include));
                    if (!h->is_regular_file(ec) || path == "module.modulemap" || path == prod.name + "_otok.h") continue;
                    if (auto [it, fresh] = header_owners.emplace(path, "the product '" + prod.name + "'"); !fresh) {
                        std::string const other = it->second.starts_with("the product") ? it->second : "the XCFramework '" + it->second + "'";
                        return fail(other + " and the product '" + prod.name + "' both have the header '" + path + "', and a consumer would see one of them");
                    }
                }
            }
            // a module per library carrying headers; products naming one such library share its module
            std::map<std::string, std::string> modules;  // module → the library carrying it
            for (output_product const& o : p.products) {
                product const& prod = *o.source;
                if (prod.headers.empty()) continue;  // no headers of its own, or a shim's module (headers only)
                std::string const carrier = prod.headers;
                std::string const module = module_of(prod);
                if (auto [it, fresh] = modules.emplace(module, carrier); !fresh && it->second != carrier) {
                    return fail("the products of '" + p.library + "' have two modules named '" + module + "'");
                }
            }
            for (auto const& [module, carrier] : modules) {
                if (auto [it, fresh] = owners.emplace("module " + module, p.library); !fresh) {
                    return fail("the libraries '" + it->second + "' and '" + p.library + "' both have a module named '" + module + "'");
                }
            }
        }
        // symbols meet in one image between static libraries only (otok-toml.md 4.5, 4.10)
        std::vector<output_package> statics;
        for (output_package const& p : packages) {
            if (p.linkage != "dynamic") statics.push_back(p);
        }
        if (auto ok = check_symbol_overlaps(dir, statics); !ok) return ok;
        mixed_linkage(packages, warnings);
        // the source targets: a name is a binary target's, a shim's or a wrapper's (shared by the products
        // of one recipe)
        std::map<std::string, std::string> source_owners;   // target name → what it is
        std::map<std::string, detail::swift_target> wrappers;
        auto const claim = [&](std::string const& name, std::string const& what) -> result<void> {
            std::string const other = owners.contains("library " + name) ? "an XCFramework of '" + owners.at("library " + name) + "'"
                : owners.contains("module " + name)               ? "a module of '" + owners.at("module " + name) + "'"
                : source_owners.contains(name)                    ? source_owners.at(name)
                                                                  : std::string{};
            if (other.empty()) {
                source_owners.emplace(name, what);
                return {};
            }
            if (other == what && what.starts_with("a wrapper")) return {};
            return fail("the target name '" + name + "' is taken by both " + other + " and " + what);
        };
        // shims first: a product holds the shims of every product whose libraries it holds, its own, those
        // of the products it depends on and of its dependencies' products, so their link settings and
        // privacy manifests come along
        std::vector<detail::swift_target> shims;
        std::map<library_ref, std::vector<std::string>> shims_of;   // library → shims
        std::map<library_ref, std::string> header_shims;            // { package, product of headers only } → its shim
        std::map<std::string, std::string> data_targets;             // library → the target of its data files (bundle-data)
        for (output_package const& p : packages) {
            // a SwiftPM package (otok-toml.md 4.6): its identity is the library's name, the last component of its path
            bool const source = swift_package_settings(*p.rec) != nullptr;
            if (source) swift.package_paths.push_back(slashed(p.dir / path_from_utf8(p.library)));
            for (output_product const& o : p.products) {
                product const& prod = *o.source;
                if (auto [it, fresh] = owners.emplace("product " + prod.name, p.library); !fresh) {
                    return fail("the libraries '" + it->second + "' and '" + p.library + "' both have a product named '" + prod.name + "'");
                }
                if (source) {
                    detail::swift_target shim;
                    shim.name = prod.name + "_otok";
                    if (auto ok = claim(shim.name, "the target of the product '" + prod.name + "'"); !ok) return ok;
                    shim.path = slashed(p.dir / "shims" / path_from_utf8(prod.name));
                    shim.product_dependencies.emplace_back(prod.name, p.library);
                    shims.push_back(std::move(shim));
                    continue;
                }
                if (!needs_shim(o, p.linkage == "dynamic")) continue;
                product_settings const* s = settings_of(prod);
                detail::swift_target shim;
                shim.name = prod.name + "_otok";
                if (auto ok = claim(shim.name, "the shim of the product '" + prod.name + "'"); !ok) return ok;
                shim.path = slashed(p.dir / "shims" / path_from_utf8(prod.name));
                for (std::string const& l : system_libraries(o)) shim.linker_settings.push_back(".linkedLibrary(" + swift_string(l) + ")");
                if (s) {
                    for (std::string const& f : s->frameworks) shim.linker_settings.push_back(".linkedFramework(" + swift_string(f) + ")");
                    if (manifest_in_shim(prod, p.linkage == "dynamic")) shim.resources.push_back(".copy(\"PrivacyInfo.xcprivacy\")");
                }
                for (std::string const& l : prod.libraries) shims_of[{p.library, l}].push_back(shim.name);
                if (prod.headers_only()) header_shims[{p.library, prod.name}] = shim.name;
                shims.push_back(std::move(shim));
            }
            if (p.bundle_data && p.data.empty()) {
                warnings.push_back("'bundle-data' of '" + p.library + "': its recipe returns no data files");
            } else if (p.bundle_data) {
                detail::swift_target data;
                data.name = identifier(p.library) + "_otok_data";
                if (auto ok = claim(data.name, "the target of the data files of '" + p.library + "'"); !ok) return ok;
                data.path = slashed(p.dir / "data");
                for (std::string const& file : p.data) data.resources.push_back(".copy(" + swift_string(file) + ")");
                data.linker_settings.push_back(".linkedFramework(\"Foundation\")");
                data_targets[p.library] = data.name;
                shims.push_back(std::move(data));
            }
        }
        for (output_package const& p : packages) {
            for (output_product const& o : p.products) {
                product const& prod = *o.source;
                if (swift_package_settings(*p.rec)) {
                    swift.products.emplace_back(prod.name, std::vector<std::string>{prod.name + "_otok"});
                    continue;
                }
                std::vector<std::string> targets;
                for (library_ref const& l : o.libraries) {
                    std::string const name = framework_name(l.package, l.name);
                    targets.push_back(name);
                    if (header_targets.contains(name)) targets.push_back(name + "_headers");
                }
                for (library_ref const& l : o.libraries) {
                    for (std::string const& shim : shims_of[l]) {
                        if (std::ranges::find(targets, shim) == targets.end()) targets.push_back(shim);
                    }
                }
                for (library_ref const& l : o.libraries) {
                    auto const data = data_targets.find(l.package);
                    if (data != data_targets.end() && std::ranges::find(targets, data->second) == targets.end()) targets.push_back(data->second);
                }
                for (library_ref const& h : o.header_products) {
                    if (auto it = header_shims.find(h); it != header_shims.end() && std::ranges::find(targets, it->second) == targets.end()) {
                        targets.push_back(it->second);
                    }
                }
                std::vector<std::string> const needed = targets;
                product_settings const* s = settings_of(prod);
                for (std::string const& w : s ? s->wrappers : std::vector<std::string>{}) {
                    if (auto ok = claim(w, "a wrapper of '" + p.library + "'"); !ok) return ok;
                    detail::swift_target& t = wrappers[w];
                    if (t.name.empty()) {
                        t.name = w;
                        fs::path const rel = p.dir / "wrappers" / path_from_utf8(w);
                        t.path = slashed(rel);
                        std::error_code ec;
                        if (fs::is_directory(dir / rel / "Resources", ec)) t.resources.push_back(".process(\"Resources\")");
                        for (option_define const& d : p.option_defines) {
                            if (d.value) {
                                t.c_settings.push_back(".define(" + swift_string(d.name) + ", to: " + swift_string(*d.value) + ")");
                            } else {
                                t.c_settings.push_back(".define(" + swift_string(d.name) + ")");
                                t.swift_settings.push_back(".define(" + swift_string(d.name) + ")");
                            }
                        }
                    }
                    for (std::string const& n : needed) {
                        if (std::ranges::find(t.dependencies, n) == t.dependencies.end()) t.dependencies.push_back(n);
                    }
                    targets.push_back(w);
                }
                swift.products.emplace_back(prod.name, std::move(targets));
            }
        }
        swift.targets = std::move(shims);
        for (auto& [name, t] : wrappers) swift.targets.push_back(std::move(t));
        for (auto [key, platform] : {std::pair{"ios", ".iOS"}, std::pair{"macos", ".macOS"}, std::pair{"catalyst", ".macCatalyst"},
                 std::pair{"tvos", ".tvOS"}, std::pair{"watchos", ".watchOS"}, std::pair{"visionos", ".visionOS"}}) {
            if (auto it = deployment.find(key); it != deployment.end()) swift.platforms.emplace_back(platform, it->second);
        }
        // each only when it changes, so Xcode and CMake see no new file (otok-toml.md 4.2)
        auto cmake = cmake_package_of(dir, packages);
        if (!cmake) return std::unexpected{cmake.error()};
        for (auto const& [name, text] : {std::pair{"Package.swift", detail::package_swift(swift)}, std::pair{"otok-config.cmake", detail::cmake_config(*cmake)},
                 std::pair{"Acknowledgements.plist", detail::acknowledgements_plist(packages)}}) {
            if (auto ok = write_file_if_changed(dir / name, text); !ok) return std::unexpected{ok.error()};
        }
        return {};
    }

    // The required-reason APIs no manifest declares (otok-toml.md 4.4) and the symbols of private system libraries (4.5)
    std::vector<std::string> output_problems(fs::path const& dir, std::span<output_package const> packages,
        std::map<std::string, std::string> const& deployment, std::vector<std::string>& warnings) const override
    {
        std::vector<std::string> problems;
        for (output_package const& p : packages) check_required_reasons(dir, p, problems);
        check_private_libraries(dir, packages, deployment, problems, warnings);
        return problems;
    }

private:
    // The framework of a dynamic library for one platform (otok-toml.md 4.10), whose binary is in `contents` already:
    // Info.plist, the privacy manifest, the links of a versioned one, the signature.
    static result<void> make_framework(fs::path const& framework, fs::path const& contents, std::string const& name, detail::framework_plist const& plist,
        std::string const* manifest, xcode_found const& x)
    {
        bool const versioned = contents != framework;
        fs::path const resources = versioned ? contents / "Resources" : framework;
        std::error_code ec;
        fs::create_directories(resources, ec);
        if (auto ok = write_file_atomic(resources / "Info.plist", detail::framework_info_plist(plist)); !ok) return ok;
        if (manifest) {
            if (auto ok = write_file_atomic(resources / "PrivacyInfo.xcprivacy", *manifest); !ok) return ok;
        }
        if (versioned) {
            fs::create_directory_symlink("A", framework / "Versions" / "Current", ec);
            if (!ec) fs::create_symlink(path_from_utf8("Versions/Current/" + name), framework / path_from_utf8(name), ec);
            if (!ec) fs::create_directory_symlink(path_from_utf8("Versions/Current/Resources"), framework / "Resources", ec);
            if (ec) return fail("cannot link the versions: " + ec.message());
        }
        if (auto ok = output_of({codesign, "--force", "--sign", "-", path_to_utf8(framework)}, x.env); !ok) return fail("codesign: " + ok.error().message);
        return {};
    }

    // The archive of the XCFramework carrying the headers of a dynamic library for one platform (otok-toml.md 4.10): an
    // object of each architecture defining nothing, so that `-all_load` meets no symbol twice.
    result<fs::path> headers_archive(fs::path const& dir, std::string const& name, std::vector<built_slice const*> const& slices, xcode_found const& x) const
    {
        fs::path const source = dir / "otok_headers.c";
        if (auto ok = write_file_atomic(source, "/* Generated by otok: defines nothing; its archive carries the headers of a dynamic library. */\n"); !ok) {
            return std::unexpected{ok.error()};
        }
        env_vars env = x.env;
        env["ZERO_AR_DATE"] = "1";  // reproducible archives
        std::vector<std::string> archives;
        for (built_slice const* s : slices) {
            auto const names = detail::names_of(s->spec);
            if (!names) return fail("no slice " + slice_name(s->spec));
            auto sdk = sdk_of(names->sdk, x);
            if (!sdk) return std::unexpected{sdk.error()};
            fs::path const object = dir / ("otok_headers-" + s->spec.arch + ".o");
            fs::path const archive = dir / ("otok_headers-" + s->spec.arch + ".a");
            if (auto ok = output_of({x.cc, "-target", names->triple, "-isysroot", sdk->path, "-c", path_to_utf8(source), "-o", path_to_utf8(object)}, env); !ok) {
                return std::unexpected{ok.error()};
            }
            if (auto ok = output_of({x.ar, "rcs", path_to_utf8(archive), path_to_utf8(object)}, env); !ok) return std::unexpected{ok.error()};
            archives.push_back(path_to_utf8(archive));
        }
        fs::path const out = dir / ("lib" + name + "_headers.a");
        std::vector<std::string> lipo{x.lipo, "-create"};
        lipo.insert(lipo.end(), archives.begin(), archives.end());
        lipo.insert(lipo.end(), {"-output", path_to_utf8(out)});
        if (auto ok = output_of(lipo, env); !ok) return std::unexpected{ok.error()};
        return out;
    }

    // The libraries that define symbols exported by private system libraries (otok-toml.md 4.5), read from the text
    // stubs of the SDKs of the output's platforms. A stub otok cannot read is a warning: the check is not done.
    void check_private_libraries(fs::path const& dir, std::span<output_package const> packages, std::map<std::string, std::string> const& deployment,
        std::vector<std::string>& problems, std::vector<std::string>& warnings) const
    {
        std::map<std::string, std::string> owners;  // exported symbol → the private library's name
        for (auto [family, sdk_name] : {std::pair{"ios", "iphoneos"}, std::pair{"macos", "macosx"}, std::pair{"catalyst", "macosx"},
                 std::pair{"tvos", "appletvos"}, std::pair{"watchos", "watchos"}, std::pair{"visionos", "xros"}}) {
            if (!deployment.contains(family)) continue;
            auto x = xcode();
            if (!x) return;
            auto sdk = sdk_of(sdk_name, **x);
            if (!sdk) continue;
            for (private_library const& library : builtin_private_libraries()) {
                auto text = read_file(fs::path{sdk->path} / path_from_utf8(library.path));
                if (!text) continue;  // not in this SDK
                auto exports = parse_tbd_exports(*text);
                if (!exports) {
                    warnings.push_back("cannot read the exports of " + library.name + " in the SDK '" + sdk_name + "': " + exports.error().message);
                    continue;
                }
                for (std::string const& s : *exports) owners.emplace(s, library.name);
            }
        }
        if (owners.empty()) return;
        for (output_package const& p : packages) {
            for (std::string const& library : p.libraries) {
                auto text = read_file(dir / p.dir / defined_symbols_dir / path_from_utf8(library + ".txt"));
                if (!text) continue;
                std::map<std::string, std::vector<std::string>> collisions;  // private library → symbols
                for (std::string const& s : parse_undefined_symbols(*text)) {
                    if (auto it = owners.find(s); it != owners.end()) collisions[it->second].push_back(s);
                }
                for (auto const& [name, symbols] : collisions) {
                    std::string list;
                    for (std::size_t i = 0; i < symbols.size() && i < 10; ++i) list += (list.empty() ? "" : ", ") + symbols[i];
                    if (symbols.size() > 10) list += " and " + std::to_string(symbols.size() - 10) + " more";
                    problems.push_back("the library '" + library + "' of '" + p.library + "' defines " + std::to_string(symbols.size())
                        + (symbols.size() == 1 ? " symbol" : " symbols") + " that " + name
                        + " exports, a private system library, and App Store review rejects such an app (ITMS-90338): " + list);
                }
            }
        }
    }

    result<xcode_found const*> xcode() const
    {
        if (!found_) found_ = discover(xcode_, environment_);
        if (!*found_) return std::unexpected{found_->error()};
        return &**found_;
    }

    result<sdk_found> sdk_of(std::string const& name, xcode_found const& x) const
    {
        if (auto it = sdks_.find(name); it != sdks_.end()) return it->second;
        auto path = output_of({"/usr/bin/xcrun", "--sdk", name, "--show-sdk-path"}, x.env);
        if (!path) return fail("the SDK '" + name + "' is not in '" + x.developer_dir + "': " + path.error().message);
        auto version = output_of({"/usr/bin/xcrun", "--sdk", name, "--show-sdk-version"}, x.env);
        if (!version) return fail("cannot read the version of the SDK '" + name + "': " + version.error().message);
        return sdks_.emplace(name, sdk_found{*path, *version}).first->second;
    }

    std::string xcode_;
    env_vars environment_;
    mutable std::optional<result<xcode_found>> found_;  // discovered once, also by finish_output
    mutable std::map<std::string, sdk_found> sdks_;
};

}

std::unique_ptr<build_target> make_build_target(std::string xcode, env_vars environment)
{
    return std::make_unique<apple_build_target>(std::move(xcode), std::move(environment));
}

std::string detail::autotools_build(std::string_view machine, std::string_view darwin_release)
{
    return std::string{machine == "arm64" ? "aarch64" : machine} + "-apple-darwin" + std::string{darwin_release};
}

std::optional<detail::slice_names> detail::names_of(slice_spec const& slice)
{
    platform_row const* row = nullptr;
    for (platform_row const& r : platforms) {
        if (slice.platform == r.platform) row = &r;
    }
    platform_info const* pi = info().find_platform(slice.platform);
    if (!row || !pi || std::ranges::find(pi->architectures, slice.arch) == pi->architectures.end()) return std::nullopt;
    std::string const& arch = slice.arch;
    bool const x86 = arch == "x86_64";
    std::string const& v = slice.deployment;

    slice_names n;
    n.sdk = row->sdk;
    n.triple = arch + "-apple-" + row->os + v + row->suffix;
    n.version_min_flag = *row->min_flag ? std::string{row->min_flag} + v : "--target=" + n.triple;
    n.deployment_variable = row->variable;
    n.cmake_system = row->cmake;
    n.autotools_host = (arch == "arm64" ? "aarch64" : arch == "arm64_32" ? "arm" : arch) + "-apple-darwin";
    // macOS and Catalyst are darwin to b2, the devices and simulators iphone
    bool const mac = slice.platform == "macosx" || slice.platform == "catalyst";
    n.b2_properties = {std::string{"target-os="} + (mac ? "darwin" : "iphone"), std::string{"architecture="} + (x86 ? "x86" : "arm"),
        std::string{"address-model="} + (arch == "arm64_32" ? "32" : "64"), std::string{"abi="} + (x86 ? "sysv" : "aapcs"), "binary-format=mach-o"};
    // meson names the platforms as zig does, but has no Catalyst: its apps are iOS apps
    n.meson_subsystem = slice.platform == "catalyst" ? "ios" : row->zig;
    n.meson_cpu_family = x86 ? "x86_64" : "aarch64";  // arm64_32 too: an AArch64 processor with 32-bit pointers
    std::string const rust_arch = arch == "arm64" ? "aarch64" : arch;
    // the Intel simulators predate the "-sim" names
    std::string rust_os = row->rust;
    if (x86 && (slice.platform == "iossim" || slice.platform == "tvossim")) rust_os = rust_os.substr(0, rust_os.size() - 4);
    n.rust_target = rust_arch + "-" + rust_os;
    if (arch != "arm64_32") n.zig_target = rust_arch + "-" + row->zig;
    n.goos = row->goos;
    if (!n.goos.empty()) n.goarch = x86 ? "amd64" : "arm64";
    n.platform = row->id;
    return n;
}

std::string detail::cmake_toolchain(cmake_inputs const& in)
{
    auto const quoted = [](std::string const& s) { return "\"" + s + "\""; };
    std::string text = "# Generated by otok for an Apple slice (" + in.names.triple + ").\n";
    // macOS builds run on this Mac: not cross compiling
    if (!in.names.cmake_system.empty()) {
        text += "set(CMAKE_SYSTEM_NAME " + in.names.cmake_system + ")\n";
        text += "set(CMAKE_SYSTEM_PROCESSOR " + in.arch + ")\n";
    }
    // cache entries: CMake's Darwin module creates them, and creating one drops a plain variable of the
    // same name (CMP0126), which left the host's architecture in macOS builds
    text += "set(CMAKE_OSX_SYSROOT " + quoted(in.sdk_path) + " CACHE PATH \"\")\n";
    text += "set(CMAKE_OSX_ARCHITECTURES " + in.arch + " CACHE STRING \"\")\n";
    // Mac Catalyst has no deployment flag of its own: the triple carries the version
    if (!in.deployment.empty() && in.names.platform != 6) {
        text += "set(CMAKE_OSX_DEPLOYMENT_TARGET " + quoted(in.deployment) + " CACHE STRING \"\")\n";
    }
    text += "set(CMAKE_C_COMPILER " + quoted(in.cc) + ")\n";
    text += "set(CMAKE_CXX_COMPILER " + quoted(in.cxx) + ")\n";
    text += "set(CMAKE_AR " + quoted(in.ar) + ")\n";
    text += "set(CMAKE_RANLIB " + quoted(in.ranlib) + ")\n";
    text += "set(CMAKE_MACOSX_BUNDLE OFF)\n";
    text += "# nothing from outside the SDK and the dependencies (docs/PLAN.md decision 25)\n";
    text += "list(APPEND CMAKE_FIND_ROOT_PATH " + quoted(in.sdk_path) + ")\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n";
    return text;
}

std::vector<detail::build_version> detail::parse_build_versions(std::string_view text)
{
    std::vector<build_version> out;
    std::optional<build_version> current;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.remove_suffix(1);
        if (line.starts_with("cmd ")) {
            current.reset();
            if (line == "cmd LC_BUILD_VERSION") current.emplace();
        } else if (current && line.starts_with("platform ")) {
            std::string_view const n = line.substr(9);
            std::from_chars(n.data(), n.data() + n.size(), current->platform);
        } else if (current && line.starts_with("minos ")) {
            current->minos = std::string{line.substr(6)};
            out.push_back(*current);
            current.reset();
        }
    }
    return out;
}

result<void> detail::check_build_versions(std::vector<build_version> const& versions, slice_spec const& slice)
{
    auto const names = names_of(slice);
    if (!names) return fail("no slice " + slice_name(slice));
    for (build_version const& v : versions) {
        if (v.platform != names->platform) {
            return fail("it contains code for " + platform_name(v.platform) + ", not for " + platform_name(names->platform));
        }
        if (!slice.deployment.empty() && compare_versions(v.minos, slice.deployment) > 0) {
            return fail("it contains code for " + platform_name(v.platform) + " " + v.minos + ", newer than the deployment version " + slice.deployment);
        }
    }
    return {};
}

std::string detail::module_map(std::string const& module, std::vector<std::string> const& headers, std::vector<std::string> const& textual,
    bool defines)
{
    std::string text = "// Generated by otok.\nmodule " + module + " {\n";
    if (defines) text += "    header \"otok_defines.h\"\n";
    for (std::string const& h : headers) text += "    header \"../" + h + "\"\n";
    for (std::string const& h : textual) text += "    textual header \"../" + h + "\"\n";
    return text + "    export *\n}\n";
}

std::string detail::arch_header(std::string const& path, std::vector<std::string> const& archs)
{
    std::string up;
    for (char c : path) {
        if (c == '/') up += "../";
    }
    std::string text = "// Generated by otok: this header differs between the architectures of the platform (otok-toml.md 4).\n";
    for (std::size_t i = 0; i < archs.size(); ++i) {
        std::string const& arch = archs[i];
        std::string const condition = arch == "x86_64" ? "defined(__x86_64__)"
            : arch == "arm64"                         ? "defined(__arm64__) && defined(__LP64__)"
            : arch == "arm64_32"                      ? "defined(__arm64__) && !defined(__LP64__)"
                                                      : "defined(__" + arch + "__)";
        text += (i == 0 ? "#if " : "#elif ") + condition + "\n#include \"" + up + arch_headers_dir + "/" + arch + "/" + path + "\"\n";
    }
    return text + "#else\n#error \"otok: no copy of " + path + " for this architecture\"\n#endif\n";
}

std::string detail::defines_header(std::string const& module, std::vector<std::string> const& defines)
{
    std::string text = "// Generated by otok: the defines of the module " + module + " (otok-toml.md 4).\n#pragma once\n";
    for (std::string const& d : defines) {
        auto const eq = d.find('=');
        text += "#define " + (eq == std::string::npos ? d : d.substr(0, eq) + " " + d.substr(eq + 1)) + "\n";
    }
    return text;
}

std::string detail::acknowledgements_plist(std::span<output_package const> packages)
{
    auto const xml = [](std::string_view s) {
        std::string out;
        for (char c : s) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
            }
        }
        return out;
    };
    auto const group = [&](std::string const& title, std::string const& footer, std::string const* license) {
        std::string text = "\t\t<dict>\n\t\t\t<key>FooterText</key>\n\t\t\t<string>" + xml(footer) + "</string>\n";
        if (license) text += "\t\t\t<key>License</key>\n\t\t\t<string>" + xml(*license) + "</string>\n";
        return text + "\t\t\t<key>Title</key>\n\t\t\t<string>" + xml(title) + "</string>\n\t\t\t<key>Type</key>\n\t\t\t<string>PSGroupSpecifier</string>\n\t\t</dict>\n";
    };
    std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                       "<plist version=\"1.0\">\n<dict>\n\t<key>PreferenceSpecifiers</key>\n\t<array>\n";
    text += group("Acknowledgements", "This product includes the following libraries.", nullptr);
    for (output_package const& p : packages) {
        std::string footer;
        for (license_file const& f : p.licenses) {
            std::string body = f.text;
            while (!body.empty() && (body.back() == '\n' || body.back() == ' ')) body.pop_back();
            footer += (footer.empty() ? "" : "\n\n") + body;
        }
        text += group(p.library, footer, &p.rec->license_text);
    }
    return text + "\t</array>\n\t<key>StringsTable</key>\n\t<string>Acknowledgements</string>\n\t<key>Title</key>\n\t<string>Acknowledgements</string>\n</dict>\n</plist>\n";
}

bool detail::versioned_framework(std::string_view platform)
{
    return platform == "macosx" || platform == "catalyst";
}

std::string detail::framework_install_name(std::string const& name, bool versioned)
{
    return "@rpath/" + name + ".framework/" + (versioned ? "Versions/A/" : "") + name;
}

std::string detail::bundle_identifier(std::string_view prefix, std::string_view library, std::string_view output)
{
    std::string id = std::string{prefix} + ".";
    for (std::string_view part : {library, std::string_view{"."}, output}) {
        for (char c : part) {
            bool const kept = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
            id += kept || part == "." ? c : '-';
        }
    }
    return id;
}

std::string detail::bundle_short_version(std::string_view version)
{
    std::string out;
    int components = 0;
    std::size_t at = 0;
    while (components < 3 && at < version.size() && version[at] >= '0' && version[at] <= '9') {
        std::size_t end = at;
        while (end < version.size() && version[end] >= '0' && version[end] <= '9') ++end;
        out += (components ? "." : "") + std::string{version.substr(at, end - at)};
        ++components;
        if (end + 1 >= version.size() || version[end] != '.') break;
        at = end + 1;
    }
    return out.empty() ? "0" : out;
}

std::string detail::supported_platform_of(std::string_view platform)
{
    static std::map<std::string, std::string, std::less<>> const names{{"ios", "iPhoneOS"}, {"iossim", "iPhoneSimulator"}, {"macosx", "MacOSX"},
        {"catalyst", "MacOSX"}, {"tvos", "AppleTVOS"}, {"tvossim", "AppleTVSimulator"}, {"watchos", "WatchOS"}, {"watchossim", "WatchSimulator"},
        {"xros", "XROS"}, {"xrossim", "XRSimulator"}};
    auto const it = names.find(platform);
    return it != names.end() ? it->second : std::string{};
}

std::string detail::framework_info_plist(framework_plist const& p)
{
    auto const entry = [](std::string const& key, std::string const& value) { return "\t<key>" + key + "</key>\n\t<string>" + value + "</string>\n"; };
    std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                       "<plist version=\"1.0\">\n<dict>\n";
    text += entry("CFBundleDevelopmentRegion", "en");
    text += entry("CFBundleExecutable", p.name);
    text += entry("CFBundleIdentifier", p.identifier);
    text += entry("CFBundleInfoDictionaryVersion", "6.0");
    text += entry("CFBundleName", p.name);
    text += entry("CFBundlePackageType", "FMWK");
    text += entry("CFBundleShortVersionString", p.short_version);
    text += "\t<key>CFBundleSupportedPlatforms</key>\n\t<array>\n\t\t<string>" + p.supported_platform + "</string>\n\t</array>\n";
    text += entry("CFBundleVersion", p.version);
    if (!p.minimum_key.empty()) text += entry(p.minimum_key, p.minimum);
    return text + "</dict>\n</plist>\n";
}

bool detail::has_package_dependencies(std::string_view manifest)
{
    return manifest.find(".package(") != std::string_view::npos;
}

std::string detail::package_swift(swift_package const& p)
{
    auto const quoted = [](std::string const& s) { return swift_string(s); };
    auto const list = [&](std::vector<std::string> const& items, bool quote) {
        std::string out;
        for (std::string const& i : items) out += (out.empty() ? "" : ", ") + (quote ? quoted(i) : i);
        return "[" + out + "]";
    };
    std::string text = "// swift-tools-version:" + p.tools_version + "\n// Generated by otok; do not edit.\n\nimport PackageDescription\n\n"
                       "let package = Package(\n    name: \"Otok\",\n";
    if (!p.platforms.empty()) {
        text += "    platforms: [";
        for (std::size_t i = 0; i < p.platforms.size(); ++i) text += (i ? ", " : "") + p.platforms[i].first + "(" + quoted(p.platforms[i].second) + ")";
        text += "],\n";
    }
    text += "    products: [\n";
    for (auto const& [name, targets] : p.products) {
        text += "        .library(name: " + quoted(name) + ", targets: " + list(targets, true) + "),\n";
    }
    text += "    ],\n";
    if (!p.package_paths.empty()) {
        text += "    dependencies: [\n";
        for (std::string const& path : p.package_paths) text += "        .package(path: " + quoted(path) + "),\n";
        text += "    ],\n";
    }
    text += "    targets: [\n";
    for (auto const& [name, path] : p.binary_targets) text += "        .binaryTarget(name: " + quoted(name) + ", path: " + quoted(path) + "),\n";
    for (swift_target const& t : p.targets) {
        text += "        .target(\n            name: " + quoted(t.name) + ",\n";
        std::vector<std::string> dependencies;
        for (std::string const& d : t.dependencies) dependencies.push_back(quoted(d));
        for (auto const& [product, package] : t.product_dependencies) {
            dependencies.push_back(".product(name: " + quoted(product) + ", package: " + quoted(package) + ")");
        }
        if (!dependencies.empty()) text += "            dependencies: " + list(dependencies, false) + ",\n";
        text += "            path: " + quoted(t.path);
        for (auto [key, items] : {std::pair{"resources", &t.resources}, std::pair{"cSettings", &t.c_settings},
                 std::pair{"swiftSettings", &t.swift_settings}, std::pair{"linkerSettings", &t.linker_settings}}) {
            if (!items->empty()) text += std::string{",\n            "} + key + ": " + list(*items, false);
        }
        text += "\n        ),\n";
    }
    return text + "    ]\n)\n";
}

namespace {

// The inside of a quoted argument of CMake.
std::string cmake_escaped(std::string_view s)
{
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"' || c == '$' || c == ';') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out;
}

std::string cmake_string(std::string_view s)
{
    return "\"" + cmake_escaped(s) + "\"";
}

// A path relative to the output, as the file finds it wherever the output is.
std::string cmake_path(std::string_view relative)
{
    return "\"${_otok_dir}/" + cmake_escaped(relative) + "\"";
}

std::string cmake_list(std::vector<std::string> const& items, std::string (*quote)(std::string_view))
{
    std::string out;
    for (std::string const& i : items) out += " " + quote(i);
    return out;
}

}

std::optional<std::string> detail::cmake_slice_of(std::string_view identifier)
{
    auto const first = identifier.find('-');
    if (first == std::string_view::npos || first == 0) return std::nullopt;
    std::string slice{identifier.substr(0, first)};
    auto const last = identifier.rfind('-');
    if (last != first) {
        std::string_view const variant = identifier.substr(last + 1);
        if (variant != "simulator" && variant != "maccatalyst") return std::nullopt;
        slice += "-" + std::string{variant};
    }
    return slice;
}

std::string detail::cmake_config(cmake_package const& p)
{
    std::string text =
        "# Generated by otok; do not edit.\n"
        "# The products of this output as imported targets for CMake 3.28 or later (otok-toml.md 4):\n"
        "#   find_package(Otok CONFIG REQUIRED PATHS <this directory> NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)\n"
        "#   target_link_libraries(<target> PRIVATE otok::<product>)\n\n"
        "if(CMAKE_VERSION VERSION_LESS 3.28)\n"
        "  message(FATAL_ERROR \"otok: the XCFrameworks of ${CMAKE_CURRENT_LIST_DIR} need CMake 3.28 or later\")\n"
        "endif()\n";
    std::string const guard = !p.products.empty() ? p.products.front().name : !p.libraries.empty() ? p.libraries.front().first : std::string{};
    if (!guard.empty()) text += "if(TARGET otok::" + guard + ")\n  return()\nendif()\n";
    text += "set(_otok_dir \"${CMAKE_CURRENT_LIST_DIR}\")\n\n"
            "# the platform built for, as the XCFrameworks name it\n"
            "if(CMAKE_SYSTEM_NAME STREQUAL \"Darwin\")\n  set(_otok_slice \"macos\")\n"
            "elseif(CMAKE_SYSTEM_NAME STREQUAL \"iOS\")\n  set(_otok_slice \"ios\")\n"
            "elseif(CMAKE_SYSTEM_NAME STREQUAL \"tvOS\")\n  set(_otok_slice \"tvos\")\n"
            "elseif(CMAKE_SYSTEM_NAME STREQUAL \"watchOS\")\n  set(_otok_slice \"watchos\")\n"
            "elseif(CMAKE_SYSTEM_NAME STREQUAL \"visionOS\")\n  set(_otok_slice \"xros\")\n"
            "else()\n  message(FATAL_ERROR \"otok: ${_otok_dir} holds libraries for Apple's platforms, not for '${CMAKE_SYSTEM_NAME}'\")\nendif()\n"
            "string(TOLOWER \"${CMAKE_OSX_SYSROOT}\" _otok_sysroot)\n"
            "if(NOT _otok_slice STREQUAL \"macos\" AND _otok_sysroot MATCHES \"simulator\")\n  string(APPEND _otok_slice \"-simulator\")\nendif()\n";
    if (!p.slices.empty()) {
        text += "set(_otok_slices" + cmake_list(p.slices, cmake_string) + ")\n"
                "if(NOT _otok_slice IN_LIST _otok_slices)\n"
                "  string(REPLACE \"-simulator\" \"sim\" _otok_platform \"${_otok_slice}\")\n"
                "  if(_otok_platform STREQUAL \"macos\")\n    set(_otok_platform \"macosx\")\n  endif()\n"
                "  message(FATAL_ERROR \"otok: ${_otok_dir} has no libraries for the platform '${_otok_platform}': add it to 'platforms' of [apple] \"\n"
                "    \"in otok.toml and run otok install\")\n"
                "endif()\n";
    }
    text += "\n# Xcode picks the slice of the SDK it builds with when the XCFrameworks are in the link phase\n"
            "if(CMAKE_GENERATOR STREQUAL \"Xcode\" AND NOT DEFINED CMAKE_XCODE_LINK_BUILD_PHASE_MODE)\n"
            "  set(CMAKE_XCODE_LINK_BUILD_PHASE_MODE KNOWN_LOCATION)\nendif()\n";
    if (!p.libraries.empty()) text += "\n# the XCFrameworks\n";
    for (auto const& [name, path] : p.libraries) {
        text += "add_library(otok::" + name + (p.shared.contains(name) ? " SHARED" : " STATIC") + " IMPORTED)\n"
                "set_target_properties(otok::" + name + " PROPERTIES IMPORTED_LOCATION " + cmake_path(path) + ")\n";
    }
    if (!p.products.empty()) text += "\n# the products\n";
    for (cmake_product const& prod : p.products) {
        std::string const target = "otok::" + prod.name;
        text += "add_library(" + target + " INTERFACE IMPORTED)\n";
        std::vector<std::string> link;
        for (std::string const& l : prod.libraries) link.push_back("otok::" + l);
        link.insert(link.end(), prod.link.begin(), prod.link.end());
        std::string items = cmake_list(link, cmake_string);
        // libc++ for a consumer linking with another driver than the C++ one, which links it itself (docs/PLAN.md decision 125)
        if (prod.cxx_runtime) items += " \"$<$<NOT:$<LINK_LANGUAGE:CXX,OBJCXX>>:-lc++>\"";
        if (!items.empty()) text += "set_property(TARGET " + target + " PROPERTY INTERFACE_LINK_LIBRARIES" + items + ")\n";
        if (!prod.include_dirs.empty()) text += "set_property(TARGET " + target + " PROPERTY INTERFACE_INCLUDE_DIRECTORIES" + cmake_list(prod.include_dirs, cmake_path) + ")\n";
        if (!prod.defines.empty()) text += "set_property(TARGET " + target + " PROPERTY INTERFACE_COMPILE_DEFINITIONS" + cmake_list(prod.defines, cmake_string) + ")\n";
    }
    text += "\n# the privacy manifests of the products, which an app puts into its bundle\nset(Otok_PRIVACY_MANIFESTS" + cmake_list(p.privacy_manifests, cmake_path) + ")\n";
    text += "\n# the data files of the libraries (build-lua-api.md 8), and the notices of their licenses (otok-toml.md 4.2)\n";
    for (auto const& [library, data] : p.data_dirs) text += "set(Otok_" + library + "_DATA_DIR " + cmake_path(data) + ")\n";
    text += "set(Otok_ACKNOWLEDGEMENTS \"${_otok_dir}/Acknowledgements.md\")\n";
    text += "\n# the frameworks of the dynamic libraries in the bundle of a target of the Xcode generator, or on the run path of its build\n"
            "function(otok_embed_frameworks target)\n";
    if (!p.frameworks.empty()) {
        auto const at = [](std::string_view relative) { return "\"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/" + cmake_escaped(relative) + "\""; };
        std::string frameworks, macos;
        for (std::string const& f : p.frameworks) frameworks += " " + at(f);
        for (std::string const& f : p.macos_frameworks) macos += " " + at(f);
        text += "  if(CMAKE_GENERATOR STREQUAL \"Xcode\")\n"
                "    set_property(TARGET ${target} APPEND PROPERTY XCODE_EMBED_FRAMEWORKS" + frameworks + ")\n"
                "    set_property(TARGET ${target} PROPERTY XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON)\n"
                "    set_property(TARGET ${target} PROPERTY XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS \"$(inherited) @executable_path/Frameworks @executable_path/../Frameworks\")\n";
        if (!macos.empty()) text += "  elseif(CMAKE_SYSTEM_NAME STREQUAL \"Darwin\")\n    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH" + macos + ")\n";
        text += "  endif()\n";
    }
    text += "endfunction()\n"
            "\n# the DLLs a target loads on Windows (otok-toml.md 4.11): nothing on Apple's platforms, so that a project of several targets calls it alike\n"
            "function(otok_copy_runtime_dlls target)\nendfunction()\n";
    if (!p.warnings.empty()) text += "\n";
    for (std::string const& w : p.warnings) text += "message(WARNING " + cmake_string("otok: " + w) + ")\n";
    return text + "\nunset(_otok_dir)\nunset(_otok_slice)\nunset(_otok_sysroot)\nunset(_otok_slices)\nunset(_otok_platform)\n";
}

}
