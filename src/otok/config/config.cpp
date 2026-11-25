// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/config/config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <thread>

#include "otok/core/fs.hpp"
#include "otok/core/toml_reader.hpp"
#include "otok/fetch/http.hpp"

namespace otok {

namespace {

std::optional<color_mode> parse_color(std::string_view s)
{
    if (s == "auto") return color_mode::automatic;
    if (s == "always") return color_mode::always;
    if (s == "never") return color_mode::never;
    return std::nullopt;
}

// What is wrong with the URL of a store of the shared build cache (config.md 2.3); nothing when it is right.
std::optional<std::string> build_cache_url_problem(std::string const& url)
{
    if (url.starts_with("file:")) {
        if (!file_url_path(url)) return "must be a file: URL of a directory of this machine (file:///srv/otok-cache), not '" + url + "'";
        return std::nullopt;
    }
    if ((url.starts_with("https://") && url.size() > 8) || (url.starts_with("http://") && url.size() > 7)) return std::nullopt;
    return std::string{"must be a file:, https or http URL, not '"} + url + "'";
}

std::optional<bool> parse_flag(std::string_view s)
{
    if (s == "1" || s == "true" || s == "yes") return true;
    if (s == "0" || s == "false" || s == "no") return false;
    return std::nullopt;
}

std::optional<std::int64_t> parse_integer(std::string_view s)
{
    std::int64_t value = 0;
    auto const [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return value;
}

std::optional<std::string> non_empty(environment const& env, std::string_view name)
{
    auto value = env(name);
    if (value && value->empty()) return std::nullopt;
    return value;
}

// What a setting holding a path may hold besides one (config.md 2).
enum class path_setting
{
    path,          // a path alone
    path_or_name,  // a path, or a name without a directory (git, a Visual Studio version) kept as it is
    path_or_url,   // a path, or a URL kept as it is (index-mirror)
};

// A relative path made absolute against `base` (config.md 2): otok runs git and the builds in directories of its own,
// where a relative path would name something else.
// Only a path of neither a root name nor a root directory: `/srv/cache` and `D:cache` on Windows name their place.
bool relative_alone(std::filesystem::path const& p)
{
    return !p.empty() && !p.has_root_name() && !p.has_root_directory();
}

void anchor(std::string& value, std::filesystem::path const& base, path_setting kind = path_setting::path)
{
    if (value.empty() || value.starts_with("~/")) return;  // `~/` of token-file is the home directory
    if (kind == path_setting::path_or_name && value.find_first_of("/\\") == std::string::npos) return;
    if (kind == path_setting::path_or_url && value.find("://") != std::string::npos) return;
    std::filesystem::path const p = path_from_utf8(value);
    if (!relative_alone(p)) return;
    value = path_to_utf8((base / p).lexically_normal());
}

void anchor(std::optional<std::string>& value, std::filesystem::path const& base, path_setting kind = path_setting::path)
{
    if (value) anchor(*value, base, kind);
}

}

void anchor_config_paths(config_file& file, std::filesystem::path const& base)
{
    anchor(file.cache_dir, base);
    anchor(file.index_mirror, base, path_setting::path_or_url);
    anchor(file.visual_studio, base, path_setting::path_or_name);
    anchor(file.xcode, base);
    anchor(file.android_ndk, base);
    anchor(file.windows_sdk, base);
    anchor(file.llvm, base);
    anchor(file.git, base, path_setting::path_or_name);
    if (file.build_caches) {
        for (build_cache_store& s : *file.build_caches) anchor(s.token_file, base);
    }
}

environment process_environment()
{
    return [](std::string_view name) -> std::optional<std::string> {
#ifdef _WIN32
        std::wstring const wide(name.begin(), name.end());  // variable names are ASCII
        wchar_t const* value = _wgetenv(wide.c_str());
        if (!value) return std::nullopt;
        return path_to_utf8(std::filesystem::path{value});
#else
        char const* value = std::getenv(std::string{name}.c_str());
        if (!value) return std::nullopt;
        return std::string{value};
#endif
    };
}

std::optional<config_file> parse_config(std::string_view content, std::string const& file, diagnostics& diags)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();

    config_file c;
    if (auto jobs = root.integer("jobs")) {
        if (*jobs < 1) root.error(root.position("jobs"), root.describe("jobs") + " must be 1 or more");
        c.jobs = *jobs;
    }
    if (auto days = root.integer("cache-max-age")) {
        if (*days < 0) root.error(root.position("cache-max-age"), root.describe("cache-max-age") + " must be 0 or more");
        c.cache_max_age = *days;
    }
    for (auto [key, out] : {std::pair{"cache-dir", &c.cache_dir}, std::pair{"index", &c.index}, std::pair{"index-mirror", &c.index_mirror},
             std::pair{"proxy", &c.proxy}, std::pair{"visual-studio", &c.visual_studio}, std::pair{"xcode", &c.xcode},
             std::pair{"android-ndk", &c.android_ndk}, std::pair{"windows-sdk", &c.windows_sdk}, std::pair{"llvm", &c.llvm}, std::pair{"git", &c.git}}) {
        if (auto value = root.string(key)) {
            // index = "" is a machine without an index
            if (value->empty() && std::string_view{key} != "index") root.error(root.position(key), root.describe(key) + " must not be empty");
            *out = std::move(*value);
        }
    }
    // gone (docs/PLAN.md decision 85): refused, so that a machine relying on them learns what replaces them
    if (root.contains("tool-paths")) {
        root.ignore("tool-paths");
        root.error(root.position("tool-paths"), root.describe("tool-paths") + " is no longer read: the toolchains of published recipes are built "
            "by their recipes; set 'git' for git and 'llvm' for clang-cl, and give other tools in PATH");
    }
    if (root.contains("toolchains")) {
        root.ignore("toolchains");
        root.error(root.position("toolchains"), root.describe("toolchains") + " is no longer read: a toolchain with 'default-recipe' is always "
            "built by its recipe");
    }
    read_input_mirrors(root, c.mirrors, severity::warning);
    if (auto stores = root.tables("build-cache")) {
        std::vector<build_cache_store> list;
        for (toml_table& s : *stores) {
            build_cache_store store;
            store.url = s.string("url", need::required).value_or("");
            if (s.contains("url")) {
                if (auto problem = build_cache_url_problem(store.url)) s.error(s.position("url"), s.describe("url") + " " + *problem);
            }
            store.push = s.boolean("push").value_or(false);
            store.push_local = s.boolean("push-local").value_or(false);
            if (auto token = s.string("token-file")) {
                if (token->empty()) s.error(s.position("token-file"), s.describe("token-file") + " must not be empty");
                store.token_file = std::move(*token);
            }
            s.finish(severity::warning);
            list.push_back(std::move(store));
        }
        c.build_caches = std::move(list);
    }
    c.keep_build_dirs = root.boolean("keep-build-dirs");
    c.sandbox = root.boolean("sandbox");
    c.virtual_paths = root.boolean("virtual-paths");
    if (auto color = root.string("color")) {
        c.color = parse_color(*color);
        if (!c.color) root.error(root.position("color"), root.describe("color") + " must be \"auto\", \"always\" or \"never\"");
    }
    // A newer otok may know more keys; they never change the output, so this one only warns.
    root.finish(severity::warning);

    if (diags.error_count() != errors_before) return std::nullopt;
    return c;
}

std::optional<std::filesystem::path> config_file_location(environment const& env)
{
    if (auto explicit_path = non_empty(env, "OTOK_CONFIG")) return path_from_utf8(*explicit_path);
#ifdef _WIN32
    if (auto appdata = non_empty(env, "APPDATA")) return path_from_utf8(*appdata) / "otok" / "config.toml";
#else
    if (auto xdg = non_empty(env, "XDG_CONFIG_HOME")) return path_from_utf8(*xdg) / "otok" / "config.toml";
    if (auto home = non_empty(env, "HOME")) return path_from_utf8(*home) / ".config" / "otok" / "config.toml";
#endif
    return std::nullopt;
}

namespace {

std::optional<std::filesystem::path> default_cache_dir(environment const& env)
{
#if defined(_WIN32)
    if (auto local = non_empty(env, "LOCALAPPDATA")) return path_from_utf8(*local) / "otok" / "cache";
#elif defined(__APPLE__)
    if (auto home = non_empty(env, "HOME")) return path_from_utf8(*home) / "Library" / "Caches" / "otok";
#else
    if (auto xdg = non_empty(env, "XDG_CACHE_HOME")) return path_from_utf8(*xdg) / "otok";
    if (auto home = non_empty(env, "HOME")) return path_from_utf8(*home) / ".cache" / "otok";
#endif
    return std::nullopt;
}

}

std::optional<machine_config> resolve_config(config_file const& file, environment const& env, diagnostics& diags)
{
    std::size_t const errors_before = diags.error_count();
    machine_config c;

    c.jobs = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::thread::hardware_concurrency()));
    if (file.jobs) c.jobs = *file.jobs;
    if (auto jobs = non_empty(env, "OTOK_JOBS")) {
        auto value = parse_integer(*jobs);
        if (!value || *value < 1) diags.error({}, "OTOK_JOBS must be a number of 1 or more, not '" + *jobs + "'");
        else c.jobs = *value;
    }

    if (auto dir = non_empty(env, "OTOK_CACHE_DIR")) c.cache_dir = path_from_utf8(*dir);
    else if (file.cache_dir) c.cache_dir = path_from_utf8(*file.cache_dir);
    else if (auto fallback = default_cache_dir(env)) c.cache_dir = std::move(*fallback);
    else diags.error({}, "cannot determine the cache directory: set OTOK_CACHE_DIR or 'cache-dir' in config.toml");

    // OTOK_INDEX set but empty, like index = "", means no index
    if (auto index = env("OTOK_INDEX")) c.index = std::move(*index);
    else c.index = file.index.value_or(std::string{default_index});
    c.index_mirror = non_empty(env, "OTOK_INDEX_MIRROR").value_or(file.index_mirror.value_or(""));
    c.mirrors = file.mirrors;
    if (auto mirrors = non_empty(env, "OTOK_ASSET_MIRROR")) {
        // templates separated by spaces
        c.mirrors.asset_mirrors.clear();
        std::size_t start = 0;
        while ((start = mirrors->find_first_not_of(" \t", start)) != std::string::npos) {
            std::size_t const end = std::min(mirrors->find_first_of(" \t", start), mirrors->size());
            std::string const t = mirrors->substr(start, end - start);
            if (auto problem = asset_mirror_problem(t)) diags.error({}, "OTOK_ASSET_MIRROR " + *problem);
            c.mirrors.asset_mirrors.push_back(t);
            start = end;
        }
    }
    if (auto only = non_empty(env, "OTOK_MIRRORS_ONLY")) {
        auto value = parse_flag(*only);
        if (!value) {
            diags.error({}, "OTOK_MIRRORS_ONLY must be 1, 0, true, false, yes or no, not '" + *only + "'");
        } else {
            c.mirrors.mirrors_only = *value;
            c.mirrors.mirrors_only_libraries.clear();
        }
    }
    // the stores of the shared build cache (config.md 2.3): OTOK_BUILD_CACHE replaces the file's, read only
    if (file.build_caches) c.build_caches = *file.build_caches;
    for (build_cache_store& s : c.build_caches) {
        if (s.token_file.starts_with("~/")) {
#ifdef _WIN32
            auto const home = non_empty(env, "USERPROFILE");
#else
            auto const home = non_empty(env, "HOME");
#endif
            if (home) s.token_file = path_to_utf8(path_from_utf8(*home) / path_from_utf8(s.token_file.substr(2)));
        }
    }
    auto const variable_token = non_empty(env, "OTOK_BUILD_CACHE_TOKEN").value_or("");
    auto const urls_of = [](std::string const& text) {
        std::vector<std::string> urls;
        std::size_t start = 0;
        while ((start = text.find_first_not_of(" \t", start)) != std::string::npos) {
            std::size_t const end = std::min(text.find_first_of(" \t", start), text.size());
            urls.push_back(text.substr(start, end - start));
            start = end;
        }
        return urls;
    };
    if (auto stores = env("OTOK_BUILD_CACHE")) {
        c.build_caches.clear();
        for (std::string const& url : urls_of(*stores)) {
            if (auto problem = build_cache_url_problem(url)) diags.error({}, "OTOK_BUILD_CACHE " + *problem);
            c.build_caches.push_back({url, false, false, {}, variable_token});
        }
    }
    if (auto pushed = non_empty(env, "OTOK_BUILD_CACHE_PUSH")) {
        if (auto problem = build_cache_url_problem(*pushed)) diags.error({}, "OTOK_BUILD_CACHE_PUSH " + *problem);
        auto const it = std::ranges::find(c.build_caches, *pushed, &build_cache_store::url);
        if (it != c.build_caches.end()) it->push = true;
        else c.build_caches.push_back({*pushed, true, false, {}, variable_token});
    }

    c.visual_studio = non_empty(env, "OTOK_VISUAL_STUDIO").value_or(file.visual_studio.value_or(""));
    c.xcode = non_empty(env, "OTOK_XCODE").value_or(file.xcode.value_or(""));
    c.android_ndk = non_empty(env, "OTOK_ANDROID_NDK").value_or(file.android_ndk.value_or(""));
    c.windows_sdk = non_empty(env, "OTOK_WINDOWS_SDK").value_or(file.windows_sdk.value_or(""));
    c.llvm = non_empty(env, "OTOK_LLVM").value_or(file.llvm.value_or(""));
    c.git = non_empty(env, "OTOK_GIT").value_or(file.git.value_or(""));
    for (std::string_view gone : {"OTOK_TOOL_PATH", "OTOK_TOOLCHAINS"}) {
        if (non_empty(env, gone)) diags.error({}, std::string{gone} + " is no longer read (config.md 2): unset it");
    }

    if (auto proxy = non_empty(env, "OTOK_PROXY")) c.proxy = *proxy;
    else if (file.proxy) c.proxy = *file.proxy;
    else {
        for (std::string_view name : {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"}) {
            if (auto generic = non_empty(env, name)) {
                c.proxy = *generic;
                break;
            }
        }
    }

    c.cache_max_age = file.cache_max_age.value_or(30);
    if (auto days = non_empty(env, "OTOK_CACHE_MAX_AGE")) {
        auto value = parse_integer(*days);
        if (!value || *value < 0) diags.error({}, "OTOK_CACHE_MAX_AGE must be a number of 0 or more, not '" + *days + "'");
        else c.cache_max_age = *value;
    }

    c.keep_build_dirs = file.keep_build_dirs.value_or(false);
    if (auto keep = non_empty(env, "OTOK_KEEP_BUILD_DIRS")) {
        auto value = parse_flag(*keep);
        if (!value) diags.error({}, "OTOK_KEEP_BUILD_DIRS must be 1, 0, true, false, yes or no, not '" + *keep + "'");
        else c.keep_build_dirs = *value;
    }

    c.sandbox = file.sandbox.value_or(true);
    if (auto sandbox = non_empty(env, "OTOK_SANDBOX")) {
        auto value = parse_flag(*sandbox);
        if (!value) diags.error({}, "OTOK_SANDBOX must be 1, 0, true, false, yes or no, not '" + *sandbox + "'");
        else c.sandbox = *value;
    }

    c.virtual_paths = file.virtual_paths.value_or(true);
    if (auto virtual_paths = non_empty(env, "OTOK_VIRTUAL_PATHS")) {
        auto value = parse_flag(*virtual_paths);
        if (!value) diags.error({}, "OTOK_VIRTUAL_PATHS must be 1, 0, true, false, yes or no, not '" + *virtual_paths + "'");
        else c.virtual_paths = *value;
    }

    c.color = file.color.value_or(color_mode::automatic);
    if (auto color = non_empty(env, "OTOK_COLOR")) {
        auto value = parse_color(*color);
        if (!value) diags.error({}, "OTOK_COLOR must be auto, always or never, not '" + *color + "'");
        else c.color = *value;
    }

    // the paths of variables, relative to the current directory (those of the file are made absolute as it is read)
    std::error_code ec;
    std::filesystem::path const current = std::filesystem::current_path(ec);
    if (!ec) {
        if (relative_alone(c.cache_dir)) c.cache_dir = (current / c.cache_dir).lexically_normal();
        anchor(c.index_mirror, current, path_setting::path_or_url);
        anchor(c.visual_studio, current, path_setting::path_or_name);
        anchor(c.xcode, current);
        anchor(c.android_ndk, current);
        anchor(c.windows_sdk, current);
        anchor(c.llvm, current);
        anchor(c.git, current, path_setting::path_or_name);
        for (build_cache_store& s : c.build_caches) anchor(s.token_file, current);
    }

    if (diags.error_count() != errors_before) return std::nullopt;
    return c;
}

std::optional<machine_config> load_config(environment const& env, diagnostics& diags)
{
    config_file file;
    if (auto location = config_file_location(env)) {
        std::error_code ec;
        if (relative_alone(*location)) {
            auto const current = std::filesystem::current_path(ec);
            if (!ec) *location = (current / *location).lexically_normal();
        }
        if (std::filesystem::exists(*location, ec)) {
            std::string const display = path_to_utf8(*location);
            auto content = read_file(*location);
            if (!content) {
                diags.error({display}, content.error().message);
                return std::nullopt;
            }
            auto parsed = parse_config(*content, display, diags);
            if (!parsed) return std::nullopt;
            file = std::move(*parsed);
            anchor_config_paths(file, location->parent_path());
        } else if (non_empty(env, "OTOK_CONFIG")) {
            diags.error({}, "OTOK_CONFIG names '" + path_to_utf8(*location) + "', which does not exist");
            return std::nullopt;
        }
    }
    return resolve_config(file, env, diags);
}

}
