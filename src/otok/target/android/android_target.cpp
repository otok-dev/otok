// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/target/android/android_target.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "otok/core/binary.hpp"
#include "otok/core/fs.hpp"

namespace otok::target::android {

namespace fs = std::filesystem;

namespace {

#if defined(_M_ARM64) || defined(__aarch64__)
constexpr char const* machine_arch = "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
constexpr char const* machine_arch = "x86_64";
#else
#   error "unknown architecture of the build machine"
#endif

#ifdef _WIN32
constexpr char const* exe = ".exe";
constexpr char path_separator = ';';
#else
constexpr char const* exe = "";
constexpr char path_separator = ':';
#endif

std::string slashed(fs::path const& p)
{
    return path_to_utf8(p.generic_u8string());
}

std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s;
}

// A variable of otok's environment, whatever its case on Windows.
std::optional<std::string> variable(env_vars const& env, std::string_view name)
{
    for (auto const& [key, value] : env) {
        bool const same = key.size() == name.size() && std::ranges::equal(key, name, [](char a, char b) {
#ifdef _WIN32
            return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
#else
            return a == b;
#endif
        });
        if (same && !value.empty()) return value;
    }
    return std::nullopt;
}

// What the target found once: the NDK and the environment its builds start from.
struct ndk_found
{
    std::string root;               // with '/'
    std::string bin;                // toolchains/llvm/prebuilt/<host>/bin
    std::string revision;           // "29.0.14206865"
    int min_api = 0, max_api = 0;
    env_vars env;
    std::string description;
};

#ifdef __APPLE__
// The output of a program run for otok (xcrun).
result<std::string> output_of(std::vector<std::string> argv, env_vars const& env)
{
    std::string const program = argv.front();
    auto exit = run_process({std::move(argv), env, fs::current_path(), {}, true});
    if (!exit) return std::unexpected{exit.error()};
    if (!exit->success()) return fail("'" + program + "' failed with " + describe_exit(*exit));
    return std::move(exit->output);
}
#endif

// The NDK (config.md `android-ndk`): the one named, else the newest of the SDK.
result<fs::path> ndk_root(std::string const& configured, env_vars const& env)
{
    for (auto const& [name, value] : {std::pair<std::string, std::optional<std::string>>{"'android-ndk' in config.toml", configured.empty()
                                                                                              ? std::nullopt : std::optional{configured}},
             std::pair<std::string, std::optional<std::string>>{"ANDROID_NDK_HOME", variable(env, "ANDROID_NDK_HOME")},
             std::pair<std::string, std::optional<std::string>>{"ANDROID_NDK_ROOT", variable(env, "ANDROID_NDK_ROOT")}}) {
        if (!value) continue;
        fs::path const root = path_from_utf8(*value);
        std::error_code ec;
        if (!fs::is_regular_file(root / "source.properties", ec)) return fail(name + " names '" + *value + "', which is not an NDK (it has no source.properties)");
        return root;
    }
    std::vector<fs::path> sdks;
    for (char const* name : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        if (auto value = variable(env, name)) sdks.push_back(path_from_utf8(*value));
    }
#if defined(_WIN32)
    if (auto local = variable(env, "LOCALAPPDATA")) sdks.push_back(path_from_utf8(*local) / "Android" / "Sdk");
#elif defined(__APPLE__)
    if (auto home = variable(env, "HOME")) sdks.push_back(path_from_utf8(*home) / "Library" / "Android" / "sdk");
#else
    if (auto home = variable(env, "HOME")) sdks.push_back(path_from_utf8(*home) / "Android" / "Sdk");
#endif
    for (fs::path const& sdk : sdks) {
        if (auto ndk = detail::newest_ndk(sdk / "ndk")) return *ndk;
    }
    return fail("no Android NDK was found: set 'android-ndk' in config.toml or ANDROID_NDK_HOME, or install one with the SDK manager");
}

result<ndk_found> discover(std::string const& configured, env_vars const& env)
{
    auto root = ndk_root(configured, env);
    if (!root) return std::unexpected{root.error()};
    ndk_found n;
    n.root = slashed(*root);
    auto properties = read_file(*root / "source.properties");
    if (!properties) return std::unexpected{properties.error()};
    n.revision = detail::ndk_revision(*properties);
    if (n.revision.empty()) return fail("'" + n.root + "/source.properties' has no Pkg.Revision");
    auto platforms = read_file(*root / "meta" / "platforms.json");
    auto levels = platforms ? detail::api_levels(*platforms) : std::nullopt;
    if (!levels) return fail("cannot read the API levels of the NDK from '" + n.root + "/meta/platforms.json'");
    n.min_api = levels->first;
    n.max_api = levels->second;
    n.bin = n.root + "/toolchains/llvm/prebuilt/" + detail::host_tag() + "/bin";
    std::error_code ec;
    if (!fs::is_regular_file(path_from_utf8(n.bin + "/clang" + exe), ec)) {
        return fail("the NDK at '" + n.root + "' has no compilers for this machine ('" + n.bin + "/clang" + exe + "' is missing)");
    }

    // PATH: the NDK's and the system's; the builder adds the directories of the recipe's toolchains (docs/PLAN.md decision 38)
    std::vector<std::string> dirs{n.bin};
#if defined(_WIN32)
    std::string const system_root = variable(env, "SystemRoot").value_or("C:\\Windows");
    dirs.insert(dirs.end(), {system_root + "\\System32", system_root, system_root + "\\System32\\Wbem"});
    // what Windows programs expect to find
    for (char const* name : {"SystemRoot", "SystemDrive", "windir", "ComSpec", "PATHEXT", "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS"}) {
        if (auto value = variable(env, name)) n.env[name] = *value;
    }
#else
#   if defined(__APPLE__)
    // /usr/bin/make is an xcrun shim, which updates xcrun's cache in the user's temporary directory, and the
    // sandbox forbids that: the active developer directory's make comes first (as for the native target)
    if (auto make = output_of({"/usr/bin/xcrun", "--find", "make"}, {{"PATH", "/usr/bin:/bin"}})) {
        dirs.push_back(slashed(path_from_utf8(trimmed(*make)).parent_path()));
    }
#   endif
    dirs.insert(dirs.end(), {"/usr/bin", "/bin", "/usr/sbin", "/sbin"});
#endif
    std::string path;
    for (std::string const& d : dirs) path += (path.empty() ? "" : std::string{path_separator}) + d;
    n.env["PATH"] = path;
    n.description = "Android NDK " + n.revision + " at '" + n.root + "'";
    return n;
}

// A file name as linkers compare it: Windows ignores case.
std::string link_name(fs::path const& file)
{
    std::string name = path_to_utf8(file.filename());
#ifdef _WIN32
    for (char& c : name) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
#endif
    return name;
}

// Copies the header directory `from` into `to`; a file both have must have the same contents. Beyond 260 characters on Windows
// (merge_tree): Boost's deepest header is 90 characters below include/.
result<void> merge_headers(fs::path const& from, fs::path const& to, std::string const& slice)
{
    auto const differing = merge_tree(from, to);
    if (!differing) return fail("cannot copy the headers of '" + path_to_utf8(from) + "': " + differing.error().message);
    if (!differing->empty()) {
        return fail("the header '" + *differing + "' of " + slice + " differs from that of another slice: "
            + "the Android target keeps one include/ for all ABIs and configurations");
    }
    return {};
}

class android_build_target final : public build_target
{
public:
    android_build_target(std::string ndk, env_vars environment) : ndk_{std::move(ndk)}, environment_{std::move(environment)} {}

    target_info const& info() const override { return android::info(); }

    // never used: tool slices are built by the target of the build machine (otok-toml.md 7)
    std::string tool_arch() const override { return machine_arch; }

    // `*/ndk` (common 4.2): the NDK of this machine
    result<void> find_compiler(build_environment const& environment) override
    {
        if (environment.compiler != "ndk") return fail("the Android target has no compiler '" + environment.compiler + "'");
        auto found = ndk();
        if (!found) return std::unexpected{found.error()};
        return {};
    }

    result<std::string> tools_path() override
    {
        auto found = ndk();
        if (!found) return std::unexpected{found.error()};
        return (*found)->env.at("PATH");
    }

    result<toolchain_profile> profile(slice_spec const& slice) override
    {
        auto names = slice.platform == "android" ? detail::names_of(slice.arch) : std::nullopt;
        if (!names) return fail("the Android target has no slice " + slice_name(slice));
        auto found = ndk();
        if (!found) return std::unexpected{found.error()};
        ndk_found const& n = **found;
        std::string const& api = slice.deployment;
        int level = 0;
        for (char c : api) level = std::isdigit(static_cast<unsigned char>(c)) && level < 1000 ? level * 10 + (c - '0') : -1;
        if (api.empty() || level <= 0) return fail("the API level of " + slice_name(slice) + " must be a number such as \"24\", not '" + api + "'");
        if (level < n.min_api || level > n.max_api) {
            return fail("the " + n.description + " builds for API levels " + std::to_string(n.min_api) + " to " + std::to_string(n.max_api)
                + ", not " + api);
        }
        bool const debug = slice.configuration == "debug";
        bool const dynamic = slice.linkage == "dynamic";
        auto const stl = slice.choices.find("stl");
        bool const static_stl = stl != slice.choices.end() && stl->second == "static";
        bool const shared_runtime = dynamic && !static_stl;

        toolchain_profile p;
        p.read_paths.push_back(path_from_utf8(n.root));
        p.triple = names->clang_triple + api;
        p.identity = {{"ndk", n.revision}};
        if (dynamic) p.identity["stl"] = static_stl ? "static" : "shared";  // what the shared objects carry (cache-key.md 3)
        p.description = n.description;
        p.autotools_host = names->autotools_host;
        p.b2_toolset = "clang";
        p.b2_properties = names->b2_properties;
        p.meson_host = {{"system", "android"}, {"kernel", "linux"}, {"cpu_family", names->meson_cpu_family}, {"cpu", names->meson_cpu},
            {"endian", "little"}};
        p.prefix_map_flag = "-ffile-prefix-map=";
        slice_toolchain& t = p.toolchain;
        t.cc = n.bin + "/clang" + exe;
        t.cxx = n.bin + "/clang++" + exe;
        t.ar = n.bin + "/llvm-ar" + exe;
        t.ranlib = n.bin + "/llvm-ranlib" + exe;
        t.strip = n.bin + "/llvm-strip" + exe;
        t.nm = n.bin + "/llvm-nm" + exe;
        // the target, part of the compiler's command too: libtool links shared objects without it (build-lua-api.md 3.1)
        t.driver_flags = {"--target=" + p.triple};
        // the NDK's own defaults (build/cmake/flags.cmake), and PIC: static libraries end up in shared objects
        t.cflags = {"--target=" + p.triple, "-DANDROID", "-fPIC", "-fdata-sections", "-ffunction-sections", "-funwind-tables",
            "-fstack-protector-strong", "-no-canonical-prefixes", "-D_FORTIFY_SOURCE=2"};
        t.cflags.insert(t.cflags.end(), names->cflags.begin(), names->cflags.end());
        if (slice.arch == "x86" && level < 24) t.cflags.push_back("-mstackrealign");  // stack alignment of old x86 devices
        t.ldflags = {"--target=" + p.triple, "-Wl,--build-id=sha1", "-Wl,--no-undefined", "-Qunused-arguments"};
        if (level < 30) t.ldflags.push_back("-Wl,--no-rosegment");  // unwinding on old devices (NDK issues 1196, 1589)
        // 16 KiB pages (Google Play requires them of 64-bit libraries); the default of NDK r28 and later
        if (names->wide) t.ldflags.push_back("-Wl,-z,max-page-size=16384");
        // the C++ runtime (otok-toml.md 4.7): c++_shared for shared objects unless `stl = "static"`; programs of a static
        // slice (configure's) link it statically
        if (!shared_runtime) t.ldflags.push_back("-static-libstdc++");
        // the configuration's (build-lua-api.md 3.1)
        std::vector<std::string> const configuration = debug ? std::vector<std::string>{"-O0", "-g"} : std::vector<std::string>{"-O2", "-DNDEBUG"};
        t.cflags.insert(t.cflags.end(), configuration.begin(), configuration.end());
        t.cxxflags = t.cflags;
        t.env = n.env;
        t.env["PKG_CONFIG_LIBDIR"] = "";  // only dependencies, nothing from the machine
        t.env["ANDROID_NDK"] = n.root;
        t.env["CC"] = t.cc;
        t.env["CXX"] = t.cxx;
        t.env["AR"] = t.ar;
        t.env["RANLIB"] = t.ranlib;
        t.env["STRIP"] = t.strip;
        t.env["NM"] = t.nm;

        for (auto [field, value] : {std::pair{"rust_target", &names->rust_target}, std::pair{"zig_target", &names->zig_target},
                 std::pair{"goarch", &names->goarch}}) {
            if (!value->empty()) p.target_fields[field] = *value;
        }
        p.target_fields["goos"] = "android";
        p.target_tables["android"] = {{"abi", slice.arch}, {"api", api}, {"ndk", n.root}};
        p.cmake_toolchain = detail::cmake_toolchain({slice.arch, api, n.root, t.cc, t.cxx, t.ar, t.ranlib, shared_runtime});
        return p;
    }

    result<void> check_library(fs::path const& library, slice_spec const& slice) override
    {
        auto names = detail::names_of(slice.arch);
        if (!names) return fail("the Android target has no slice " + slice_name(slice));
        std::vector<unsigned> const machines = elf_machines_of(library);
        if (machines.empty()) return fail("it holds no ELF object");
        if (machines != std::vector<unsigned>{names->elf_machine}) {
            std::string list;
            for (unsigned m : machines) list += (list.empty() ? "" : ", ") + std::to_string(m);
            return fail("its objects are for the ELF machines " + list + ", not for " + slice.arch + " (" + std::to_string(names->elf_machine) + ")");
        }
        if (binary_kind_of(library) != binary_kind::elf_shared) return {};
        // Gradle packs lib<name>.so only, and the loader finds a library by the name others were linked with
        std::string const file = path_to_utf8(library.filename());
        std::string const soname = load_names_of(library).id;
        if (soname != file || !file.ends_with(".so")) {
            return fail("its SONAME is '" + soname + "', not its file name '" + file + "' without a version (libfoo.so), which Android needs");
        }
        if (names->wide) {
            if (std::uint64_t const align = elf_load_alignment_of(library); align < 16384) {
                return fail("its segments are aligned to " + std::to_string(align) + " bytes: 64-bit Android libraries need 16 KiB pages "
                    + "(-Wl,-z,max-page-size=16384)");
            }
        }
        return {};
    }

    // include/, lib/<abi>/ and debug/lib/<abi>/ (otok-toml.md 4.7)
    result<void> make_package(built_package const& package, recipe const& rec, std::span<output_product const> products,
        fs::path const& staging) override
    {
        (void)rec;
        (void)products;
        std::error_code ec;
        fs::create_directories(long_path(staging / "include"), ec);
        if (ec) return fail("cannot create '" + path_to_utf8(staging) + "': " + ec.message());
        package_files recorded;  // which file is which library (otok-toml.md 4.11)
        for (built_slice const& s : package.slices) {
            build_output const& out = s.output;
            // the C++ runtime of dynamic libraries, which the app uses too (otok-toml.md 4.7)
            if (s.spec.linkage == "dynamic") {
                auto const stl = s.spec.choices.find("stl");
                recorded.properties["stl"] = stl != s.spec.choices.end() ? stl->second : "shared";
            }
            std::set<fs::path> headers;
            if (out.headers) headers.insert(*out.headers);
            for (auto const& [name, dir] : out.library_headers) headers.insert(dir);
            for (fs::path const& dir : headers) {
                if (auto ok = merge_headers(dir, staging / "include", slice_label(s.spec)); !ok) return ok;
            }
            fs::path const dir = (s.spec.configuration == "debug" ? staging / "debug" / "lib" : staging / "lib") / s.spec.arch;
            fs::create_directories(long_path(dir), ec);
            std::map<std::string, std::string> files;  // link name → library output name
            for (auto const& [name, lib] : out.libraries) {
                if (auto [it, fresh] = files.emplace(link_name(lib), name); !fresh) {
                    return fail("the libraries '" + it->second + "' and '" + name + "' are both files named '" + path_to_utf8(lib.filename()) + "'");
                }
                // the file itself under the name the build gave it, a link or not
                fs::copy_file(long_path(fs::canonical(long_path(lib), ec)), long_path(dir / lib.filename()), ec);  // canonical drops the extended prefix
                if (ec) return fail("cannot copy the library '" + name + "': " + ec.message());
                package_library_file f{name, s.spec.arch, s.spec.configuration,
                    (s.spec.configuration == "debug" ? "debug/lib/" : "lib/") + s.spec.arch + "/" + path_to_utf8(lib.filename())};
                if (binary_kind_of(lib) == binary_kind::elf_shared) {
                    f.shared = true;
                    f.soname = load_names_of(lib).id;
                }
                recorded.files.push_back(std::move(f));
            }
        }
        return write_file_atomic(staging / package_files_name, write_package_files(recorded));
    }

    // otok-config.cmake (otok-toml.md 4.11); and one file name in the lib/<abi>/ of two libraries, which matters to consumers that link
    // through search paths.
    result<void> finish_output(fs::path const& dir, std::span<output_package const> packages, std::map<std::string, std::string> const& deployment,
        std::vector<std::string>& warnings) const override
    {
        (void)deployment;
        std::map<std::string, std::string> owners;  // <sub>/<abi>/<link name> → library
        for (output_package const& p : packages) {
            for (char const* sub : {"lib", "debug/lib"}) {
                std::error_code ec;
                for (auto abi = fs::directory_iterator{long_path(dir / p.dir / sub), ec}; !ec && abi != fs::directory_iterator{}; abi.increment(ec)) {
                    std::error_code file_ec;
                    for (auto it = fs::directory_iterator{abi->path(), file_ec}; !file_ec && it != fs::directory_iterator{}; it.increment(file_ec)) {
                        std::string const where = std::string{sub} + "/" + path_to_utf8(abi->path().filename()) + "/" + path_to_utf8(it->path().filename());
                        if (auto [o, fresh] = owners.emplace(std::string{sub} + "/" + path_to_utf8(abi->path().filename()) + "/" + link_name(it->path()),
                                p.library);
                            !fresh) {
                            warnings.push_back("the libraries '" + o->second + "' and '" + p.library + "' both have '" + where
                                + "': a consumer linking through search paths gets only one of them");
                        }
                    }
                }
            }
        }
        auto c = consumer_package_of(dir, packages, info().platforms.front().architectures);
        if (!c) return std::unexpected{c.error()};
        if (auto ok = write_file_if_changed(dir / "otok-config.cmake", consumer_cmake_config(*c, detail::cmake_parts(*c))); !ok) return std::unexpected{ok.error()};
        return {};
    }

private:
    result<ndk_found const*> ndk()
    {
        if (!found_) found_ = discover(ndk_, environment_);
        if (!*found_) return std::unexpected{found_->error()};
        return &**found_;
    }

    std::string ndk_;
    env_vars environment_;
    std::optional<result<ndk_found>> found_;
};

}

target_info const& info()
{
    static target_info const android{
        .name = "android",
        .platforms = {{"android", {"arm64-v8a", "armeabi-v7a", "x86_64", "x86"}, {"android"}}},
        .deployment_keys = {"android"},
        .environments = {{"macos", "", "ndk"}, {"linux", "", "ndk"}, {"windows", "", "ndk"}},
        .any_host_compilers = {"ndk"},  // the same cross build on every host (common 4.2)
        .abis = {},
        .host_system = "",
        .tool_platform = "",  // tools are built by the target of the build machine (otok-toml.md 7)
        .linkages = {"static", "dynamic"},
        .many_configurations = true,
        // the C++ runtime of shared objects (otok-toml.md 4.7): NDK's c++_shared, or c++_static in each
        .choices = {{"stl", {"shared", "static"}, {{"dynamic", "shared"}}}},
        .packages_dir = "",  // the output directory itself: <output>/<library> (otok-toml.md 4.7)
        .read_recipe_table = {},
        .read_product_table = {},
        .delivers_sources = {},
        .publish_defaults = "deployment = { android = \"24\" }\n",
        .check_publishable = {},
    };
    return android;
}

std::unique_ptr<build_target> make_build_target(std::string ndk, env_vars environment)
{
    return std::make_unique<android_build_target>(std::move(ndk), std::move(environment));
}

consumer_cmake_parts detail::cmake_parts(consumer_package const& package)
{
    consumer_cmake_parts parts;
    parts.target = "android";
    parts.platform = "android";
    parts.detect =
        "if(NOT CMAKE_SYSTEM_NAME STREQUAL \"Android\")\n"
        "  message(FATAL_ERROR \"otok: ${_otok_dir} holds libraries for Android, not for '${CMAKE_SYSTEM_NAME}'\")\n"
        "endif()\n"
        "if(CMAKE_ANDROID_ARCH_ABI)\n"
        "  set(_otok_arch \"${CMAKE_ANDROID_ARCH_ABI}\")\n"
        "elseif(ANDROID_ABI)\n"
        "  set(_otok_arch \"${ANDROID_ABI}\")\n"
        "else()\n"
        "  set(_otok_arch \"${CMAKE_SYSTEM_PROCESSOR}\")\n"
        "endif()\n";
    // the C++ runtime is the app's: static libraries take the one it links; dynamic ones were linked with that of `stl`
    bool const shared = std::ranges::any_of(package.libraries, &consumer_library::shared);
    auto const stl = package.properties.find("stl");
    if (shared && stl != package.properties.end()) {
        parts.checks =
            "\n# the C++ runtime of the app (ANDROID_STL of the NDK's toolchain file, c++_static by default), and that of the dynamic libraries\n"
            "if(ANDROID_STL)\n"
            "  set(_otok_stl \"${ANDROID_STL}\")\n"
            "elseif(CMAKE_ANDROID_STL_TYPE)\n"
            "  set(_otok_stl \"${CMAKE_ANDROID_STL_TYPE}\")\n"
            "else()\n"
            "  set(_otok_stl \"c++_static\")\n"
            "endif()\n";
        if (stl->second == "shared") {
            parts.checks +=
                "if(NOT _otok_stl STREQUAL \"c++_shared\")\n"
                "  message(FATAL_ERROR \"otok: the dynamic libraries of ${_otok_dir} use the C++ runtime c++_shared (stl = \\\"shared\\\" of [android]), \"\n"
                "    \"which the app must use and ship too, not ${_otok_stl}: set ANDROID_STL to c++_shared (Gradle: arguments \\\"-DANDROID_STL=c++_shared\\\")\")\n"
                "endif()\n";
        } else {
            parts.checks +=
                "if(_otok_stl STREQUAL \"c++_shared\")\n"
                "  message(FATAL_ERROR \"otok: the dynamic libraries of ${_otok_dir} each have the C++ runtime c++_static in them (stl = \\\"static\\\" of [android]), \"\n"
                "    \"and an app using c++_shared would have one more: set ANDROID_STL to c++_static, or stl = \\\"shared\\\" of [android] in otok.toml and run otok install\")\n"
                "endif()\n";
        }
        parts.checks += "unset(_otok_stl)\n";
    }
    parts.link_item = [](std::string const& name) { return "-l" + name; };
    return parts;
}

std::optional<detail::abi_names> detail::names_of(std::string_view abi)
{
    abi_names n;
    if (abi == "arm64-v8a") {
        n = {"aarch64-linux-android", "aarch64-linux-android", 183, true, {}, "aarch64", "aarch64", "aarch64-linux-android", "aarch64-linux-android", "arm64", {}};
    } else if (abi == "armeabi-v7a") {
        n = {"armv7a-linux-androideabi", "arm-linux-androideabi", 40, false, {}, "arm", "armv7", "armv7-linux-androideabi", "arm-linux-androideabi", "arm",
            {"-march=armv7-a", "-mthumb"}};
    } else if (abi == "x86_64") {
        n = {"x86_64-linux-android", "x86_64-linux-android", 62, true, {}, "x86_64", "x86_64", "x86_64-linux-android", "x86_64-linux-android", "amd64", {}};
    } else if (abi == "x86") {
        n = {"i686-linux-android", "i686-linux-android", 3, false, {}, "x86", "i686", "i686-linux-android", "x86-linux-android", "386", {}};
    } else {
        return std::nullopt;
    }
    bool const arm = abi.starts_with("arm");
    n.b2_properties = {"target-os=android", std::string{"architecture="} + (arm ? "arm" : "x86"), std::string{"address-model="} + (n.wide ? "64" : "32"),
        std::string{"abi="} + (arm ? "aapcs" : "sysv"), "binary-format=elf"};
    return n;
}

std::string detail::ndk_revision(std::string_view text)
{
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string const line = trimmed(std::string{text.substr(start, end - start)});
        start = end + 1;
        std::size_t const eq = line.find('=');
        if (eq != std::string::npos && trimmed(line.substr(0, eq)) == "Pkg.Revision") return trimmed(line.substr(eq + 1));
    }
    return {};
}

std::optional<fs::path> detail::newest_ndk(fs::path const& dir)
{
    // "29.0.14206865" as numbers, part by part
    auto const parts = [](std::string const& name) {
        std::vector<long long> out;
        std::size_t at = 0;
        while (at <= name.size()) {
            std::size_t const end = std::min(name.find('.', at), name.size());
            long long v = 0;
            for (std::size_t i = at; i < end; ++i) v = std::isdigit(static_cast<unsigned char>(name[i])) ? v * 10 + (name[i] - '0') : v;
            out.push_back(v);
            at = end + 1;
        }
        return out;
    };
    std::optional<fs::path> best;
    std::vector<long long> best_version;
    std::error_code ec;
    for (auto it = fs::directory_iterator{dir, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
        std::error_code file_ec;
        if (!fs::is_regular_file(it->path() / "source.properties", file_ec)) continue;
        std::vector<long long> const version = parts(path_to_utf8(it->path().filename()));
        if (!best || version > best_version) {
            best = it->path();
            best_version = version;
        }
    }
    return best;
}

std::optional<std::pair<int, int>> detail::api_levels(std::string_view json)
{
    // {"min": 21, "max": 35, "aliases": {…}}: the two numbers at the top level
    auto const number = [&](std::string_view key) -> std::optional<int> {
        std::size_t const at = json.find("\"" + std::string{key} + "\"");
        if (at == std::string_view::npos) return std::nullopt;
        std::size_t i = json.find(':', at);
        if (i == std::string_view::npos) return std::nullopt;
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        int v = 0;
        std::size_t const from = i;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) v = v * 10 + (json[i++] - '0');
        if (i == from) return std::nullopt;
        return v;
    };
    auto const min = number("min");
    auto const max = number("max");
    if (!min || !max || *min > *max) return std::nullopt;
    return std::pair{*min, *max};
}

std::string detail::host_tag()
{
#if defined(_WIN32)
    return "windows-x86_64";
#elif defined(__APPLE__)
    return "darwin-x86_64";
#else
    return "linux-x86_64";
#endif
}

std::string detail::cmake_toolchain(cmake_inputs const& in)
{
    auto const quoted = [](std::string const& s) { return "\"" + s + "\""; };
    std::string text = "# Generated by otok for an Android slice (" + in.abi + ", API " + in.api + ").\n";
    // CMake's Android support, as Gradle's builds use it: ANDROID, the NDK's system libraries, unversioned sonames
    text += "set(CMAKE_SYSTEM_NAME Android)\n";
    text += "set(CMAKE_SYSTEM_VERSION " + in.api + ")\n";
    text += "set(CMAKE_ANDROID_ARCH_ABI " + in.abi + ")\n";
    text += "set(CMAKE_ANDROID_NDK " + quoted(in.ndk) + ")\n";
    text += std::string{"set(CMAKE_ANDROID_STL_TYPE "} + (in.shared_runtime ? "c++_shared" : "c++_static") + ")\n";
    text += "set(CMAKE_C_COMPILER " + quoted(in.cc) + ")\n";
    text += "set(CMAKE_CXX_COMPILER " + quoted(in.cxx) + ")\n";
    text += "set(CMAKE_AR " + quoted(in.ar) + " CACHE FILEPATH \"\")\n";
    text += "set(CMAKE_RANLIB " + quoted(in.ranlib) + " CACHE FILEPATH \"\")\n";
    text += "# nothing from outside the NDK and the dependencies (docs/PLAN.md decision 25)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n";
    return text;
}

}
