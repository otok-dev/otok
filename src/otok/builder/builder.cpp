// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/builder/builder.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include <toml++/toml.hpp>

#include "otok/builder/dev_tree.hpp"
#include "otok/core/binary.hpp"
#include "otok/core/environment.hpp"
#include "otok/core/file_lock.hpp"
#include "otok/core/executor.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"
#include "otok/core/process.hpp"
#include "otok/core/sandbox.hpp"
#include "otok/core/tree_hash.hpp"
#include "otok/core/virtual_drives.hpp"
#include "otok/resolver/options.hpp"

namespace otok {

namespace fs = std::filesystem;

namespace {

// Bumped when the meaning of a key changes, or when otok changes what it gives builds (cache-key.md 2).
constexpr char const* key_format = "1";
constexpr char const* toolchain_profile_version = "9";  // 2: -DNDEBUG, /MD, -ffile-prefix-map; 3: /Z7, @rpath install names; 4: CMake MSVC settings; 5: $ORIGIN in RUNPATH; 6: --build of configure; 7: relocated .pc files; 8: the windows target's -fuse-ld, -Wno-register; 9: the native target's clean environment and CMake searches

// 1980-01-01, the earliest time zip archives can store (build-lua-api.md 5).
constexpr char const* source_date_epoch = "315532800";

std::string slashed(fs::path const& p)
{
    return path_to_utf8(p.generic_u8string());
}

std::string relative_to(fs::path const& p, fs::path const& base)
{
    return slashed(p.lexically_relative(base));
}

std::string toml_string(std::string const& s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// Sets a variable of a process environment; on Windows names are case-insensitive, so a differently
// spelled one is replaced.
void set_variable(env_vars& env, std::string const& name, std::string const& value)
{
#ifdef _WIN32
    std::erase_if(env, [&](auto const& v) {
        return v.first.size() == name.size() && std::ranges::equal(v.first, name, [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    });
#endif
    env[name] = value;
}

// Removes a variable of a process environment, however it is spelled on Windows.
void remove_variable(env_vars& env, std::string const& name)
{
    std::erase_if(env, [&](auto const& v) {
#ifdef _WIN32
        return v.first.size() == name.size() && std::ranges::equal(v.first, name, [](char a, char b) { return std::tolower(a) == std::tolower(b); });
#else
        return v.first == name;
#endif
    });
}

#ifdef _WIN32
constexpr char path_list_separator = ';';
#else
constexpr char path_list_separator = ':';
#endif

// Adds a directory to the end of PATH, unless it is in it already.
void append_to_path(env_vars& env, fs::path const& dir)
{
    std::string const added = path_to_utf8(dir);
    auto it = env.find("PATH");
#ifdef _WIN32
    for (auto v = env.begin(); v != env.end() && it == env.end(); ++v) {
        if (v->first.size() == 4 && std::ranges::equal(v->first, std::string_view{"PATH"}, [](char a, char b) { return std::toupper(a) == b; })) it = v;
    }
#endif
    if (it == env.end()) it = env.emplace("PATH", "").first;
    std::string_view rest = it->second;
    while (!rest.empty()) {
        std::size_t const end = std::min(rest.find(path_list_separator), rest.size());
        if (rest.substr(0, end) == added) return;
        rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    it->second += (it->second.empty() ? "" : std::string(1, path_list_separator)) + added;
}

// The last lines of a log, for messages.
std::string log_tail(fs::path const& log, std::size_t lines)
{
    std::string const text = read_file(log).value_or("");
    std::size_t pos = text.size();
    if (pos > 0 && text.back() == '\n') --pos;
    for (std::size_t n = 0; n < lines && pos > 0; ++n) {
        pos = text.rfind('\n', pos - 1);
        if (pos == std::string::npos) return text;
    }
    return text.substr(pos == 0 ? 0 : pos + 1);
}

std::string timestamp()
{
    std::time_t const now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &tm);
    return text;
}

// The files of a working tree that a commit of it could hold, as a dev slice takes them (otok-dev-toml.md 4): those git
// does not ignore, tracked or not, and so in its submodules; nothing for a directory that is no checkout, or without git.
result<std::optional<std::vector<std::string>>> files_of_checkout(std::optional<git_tool> const& git, fs::path const& tree, fs::path const& log)
{
    std::error_code ec;
    if (!git || !fs::exists(tree / ".git", ec)) return std::nullopt;
    auto listed = run_git(*git, {"ls-files", "-z", "--cached", "--others", "--exclude-standard"}, tree, log, true);
    if (!listed) return std::unexpected{listed.error()};
    std::vector<std::string> files;
    for (std::size_t at = 0; at < listed->size();) {
        std::size_t const end = std::min(listed->find('\0', at), listed->size());
        std::string const name = listed->substr(at, end - at);
        at = end + 1;
        if (name.empty()) continue;
        fs::path const path = tree / path_from_utf8(name);
        if (fs::is_directory(path, ec) && fs::exists(path / ".git", ec)) {
            auto inner = files_of_checkout(git, path, log);
            if (!inner) return std::unexpected{inner.error()};
            if (*inner) {
                for (std::string const& f : **inner) files.push_back(name + "/" + f);
            }
            continue;
        }
        files.push_back(name);
    }
    return files;
}

// Writes a file unless it has the text already: a kept work directory's build tool sees it unchanged (otok-dev-toml.md 4).
void write_if_changed(fs::path const& file, std::string const& text)
{
    if (read_file(file).value_or(std::string{}) == text && fs::exists(file)) return;
    std::ofstream{file, std::ios::binary} << text;
}

// The pkg-config files of a slice made to find it where it is, not where it was built (build-lua-api.md 8, docs/PLAN.md
// decision 72): the `prefix` variable that names the build's prefix becomes `${pcfiledir}/../..`, and the prefix
// elsewhere `${prefix}` (or `${pcfiledir}/../..` in a file without that variable).
void relocate_pkg_config(fs::path const& prefix)
{
    std::vector<std::string> forms{path_to_utf8(prefix), slashed(prefix)};
    if (forms[0] == forms[1]) forms.pop_back();
    // the prefix itself, not a longer name beside it
    auto const find_form = [&](std::string const& text, std::size_t from) -> std::pair<std::size_t, std::size_t> {
        std::pair<std::size_t, std::size_t> found{std::string::npos, 0};
        for (std::string const& form : forms) {
            for (std::size_t at = text.find(form, from); at != std::string::npos && at < found.first; at = text.find(form, at + 1)) {
                std::size_t const end = at + form.size();
                if (end == text.size() || !(std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '-' || text[end] == '_' || text[end] == '.')) {
                    found = {at, form.size()};
                    break;
                }
            }
        }
        return found;
    };
    std::error_code ec;
    for (char const* dir : {"lib/pkgconfig", "share/pkgconfig"}) {
        for (auto const& entry : fs::directory_iterator{prefix / dir, ec}) {
            if (entry.path().extension() != ".pc" || !entry.is_regular_file(ec) || entry.is_symlink(ec)) continue;
            auto const text = read_file(entry.path());
            if (!text) continue;
            std::string const here = "${pcfiledir}/" + relative_to(prefix, entry.path().parent_path());
            std::vector<std::string> lines;
            for (std::size_t at = 0; at < text->size();) {
                std::size_t const end = std::min(text->find('\n', at), text->size());
                lines.push_back(text->substr(at, end - at));
                at = end + 1;
            }
            auto const is_prefix_line = [&](std::string const& line) {
                if (!line.starts_with("prefix=")) return false;
                std::string value = line.substr(7);
                while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '/')) value.pop_back();
                return std::ranges::find(forms, value) != forms.end();
            };
            bool const has_prefix = std::ranges::any_of(lines, is_prefix_line);
            std::string out;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                std::string line = lines[i];
                if (is_prefix_line(line)) {
                    line = "prefix=" + here + (line.ends_with("\r") ? "\r" : "");
                } else {
                    std::string const by = has_prefix ? "${prefix}" : here;
                    for (std::size_t from = 0;;) {
                        auto const [at, size] = find_form(line, from);
                        if (at == std::string::npos) break;
                        line.replace(at, size, by);
                        from = at + by.size();
                    }
                }
                out += line;
                if (i + 1 < lines.size() || text->ends_with("\n")) out += '\n';
            }
            if (out != *text) std::ofstream{entry.path(), std::ios::binary} << out;
        }
    }
}

// Keeps what a failed build leaves for inspection in `failed/<name>-<time>/` (cache.md 7.1): the whole work
// directory, or its log only, with the key it was built for. What to add to the message.
std::string keep_failure(cache_store const& cache, fs::path const& work, fs::path const& log, std::string const& key, std::string const& name,
    bool whole, bool remove_work = true)
{
    std::error_code ec;
    // a second failure within the second: "-2"
    fs::path kept = cache.root() / "failed" / (name + "-" + timestamp());
    for (int n = 2; fs::exists(long_path(kept), ec); ++n) kept.replace_filename(name + "-" + timestamp() + "-" + std::to_string(n));
    fs::create_directories(long_path(kept.parent_path()), ec);
    if (whole) {
        rename_path(work, kept, ec);
        if (!ec) {
            std::ofstream{long_path(kept / "key.txt"), std::ios::binary} << key;
            return "\nthe work directory is kept in '" + slashed(kept) + "'";
        }
    }
    fs::create_directories(long_path(kept), ec);
    fs::copy_file(long_path(log), long_path(kept / log.filename()), fs::copy_options::overwrite_existing, ec);
    bool const copied = !ec;
    if (remove_work) fs::remove_all(long_path(work), ec);
    if (!copied) return "";
    std::ofstream{long_path(kept / "key.txt"), std::ios::binary} << key;
    return "\nthe whole log is in '" + slashed(kept / log.filename()) + "'";
}

// Gives the lines appended to a log while it is written to `line` (`-vv`, docs/PLAN.md decision 59); the
// last ones when destroyed.
class log_follower
{
public:
    log_follower(fs::path log, std::function<void(std::string const&)> line) : log_{std::move(log)}, line_{std::move(line)}
    {
        thread_ = std::thread{[this] {
            std::unique_lock lock{m_};
            while (!stop_changed_.wait_for(lock, std::chrono::milliseconds{100}, [this] { return stop_; })) read();
        }};
    }

    ~log_follower()
    {
        {
            std::lock_guard lock{m_};
            stop_ = true;
        }
        stop_changed_.notify_all();
        thread_.join();
        read();
        if (!partial_.empty()) line_(partial_);
    }

    log_follower(log_follower const&) = delete;
    log_follower& operator=(log_follower const&) = delete;

private:
    // opened for each read, so that the file is never held open meanwhile (Windows could not move it)
    void read()
    {
        std::ifstream in{log_, std::ios::binary};
        if (!in) return;
        in.seekg(static_cast<std::streamoff>(read_));
        std::string chunk{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        read_ += chunk.size();
        for (char c : chunk) {
            if (c != '\n') {
                partial_ += c;
                continue;
            }
            if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
            line_(partial_);
            partial_.clear();
        }
    }

    fs::path log_;
    std::function<void(std::string const&)> line_;
    std::uintmax_t read_ = 0;
    std::string partial_;
    std::mutex m_;
    std::condition_variable stop_changed_;
    bool stop_ = false;
    std::thread thread_;
};

// The values of a key.txt (cache-key.md 3): `name = value` lines.
std::map<std::string, std::string> read_key(fs::path const& file)
{
    std::map<std::string, std::string> values;
    std::istringstream in{read_file(file).value_or("")};
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::size_t const eq = line.find(" = "); eq != std::string::npos) values.emplace(line.substr(0, eq), line.substr(eq + 3));
    }
    return values;
}

// ---- result.toml: what build returned, relative to the entry's prefix

}  // namespace

result<std::string> detail::write_result(build_output const& out, fs::path const& slice_prefix)
{
    // as build_script checks the paths build.lua returns, which are in this form: resolved, through the drive on Windows
    std::error_code ec;
    fs::path const prefix = virtual_drives::to_virtual(fs::weakly_canonical(slice_prefix, ec));
    std::optional<std::string> wrong;
    auto const relative = [&](std::string const& what, fs::path const& p) {
        fs::path const r = p.lexically_relative(prefix);
        if (!wrong && (r.empty() || r.has_root_name() || r.has_root_directory() || *r.begin() == "..")) {
            wrong = "cannot record " + what + " ('" + slashed(p) + "') relative to the slice's prefix ('" + slashed(prefix) + "')";
        }
        return toml_string(slashed(r));
    };
    std::string text = "# What build.lua returned, relative to prefix/.\n";
    if (out.headers) text += "headers = " + relative("headers", *out.headers) + "\n";
    if (out.module_map) text += "module-map = " + relative("module-map", *out.module_map) + "\n";
    auto const table = [&](char const* name, std::map<std::string, fs::path> const& paths) {
        if (paths.empty() && std::string_view{name} != "libraries") return;
        text += std::string{"\n["} + name + "]\n";
        for (auto const& [key, p] : paths) text += toml_string(key) + " = " + relative(std::string{name} + "." + key, p) + "\n";
    };
    table("libraries", out.libraries);
    table("runtimes", out.runtimes);
    table("symbols", out.symbols);
    table("library-headers", out.library_headers);
    table("library-module-maps", out.library_module_maps);
    table("data", out.data);
    table("programs", out.programs);
    if (!out.interpreters.empty()) {
        text += "\n[program-interpreters]\n";
        for (auto const& [program, toolchain] : out.interpreters) text += toml_string(program) + " = " + toml_string(toolchain) + "\n";
    }
    if (wrong) return fail(*wrong);
    return text;
}

namespace {

// What an entry's build returned, every path checked (cache.md 6.2): a damaged entry is an error saying why.
result<build_output> read_result(fs::path const& entry)
{
    fs::path const file = entry / "result.toml";
    fs::path const prefix = entry / "prefix";
    std::string const damaged = "the cache entry '" + slashed(entry) + "' is damaged: ";
    auto content = read_file(file);
    if (!content) return fail(damaged + content.error().message);
    toml::table t;
    try {
        t = toml::parse(*content);
    } catch (toml::parse_error const& e) {
        return fail(damaged + std::string{e.description()});
    }
    build_output out;
    std::optional<std::string> wrong;
    // a file, or (headers, data) anything that is there
    auto const path = [&](std::string const& what, std::string const& relative, bool file) {
        fs::path const r = path_from_utf8(relative);
        fs::path const full = prefix / r;
        if (wrong) return full;
        std::error_code ec;
        if (relative.empty()) {
            wrong = what + " is empty";
        } else if (r.has_root_name() || r.has_root_directory()) {
            wrong = what + " '" + relative + "' is not relative to prefix/";
        } else if (std::ranges::find(r, fs::path{".."}) != r.end()) {
            wrong = what + " '" + relative + "' leaves prefix/";
        } else if (file ? !fs::is_regular_file(long_path(full), ec) : !fs::exists(long_path(full), ec)) {
            wrong = what + " '" + relative + "' is not " + (file ? "a file" : "there");
        }
        return full;
    };
    if (auto h = t["headers"].value<std::string>()) out.headers = path("headers", *h, false);
    if (auto m = t["module-map"].value<std::string>()) out.module_map = path("module-map", *m, true);
    auto const table = [&](char const* name, std::map<std::string, fs::path>& paths, bool files) {
        if (toml::table const* tt = t[name].as_table()) {
            for (auto const& [key, value] : *tt) {
                std::string const what = std::string{name} + "." + std::string{key.str()};
                if (auto s = value.value<std::string>()) paths.emplace(std::string{key.str()}, path(what, *s, files));
                else if (!wrong) wrong = what + " is not a string";
            }
        }
    };
    table("libraries", out.libraries, true);
    table("runtimes", out.runtimes, true);
    table("symbols", out.symbols, true);
    table("library-headers", out.library_headers, false);
    table("library-module-maps", out.library_module_maps, true);
    table("data", out.data, false);
    table("programs", out.programs, true);
    if (toml::table const* tt = t["program-interpreters"].as_table()) {
        for (auto const& [key, value] : *tt) {
            if (auto s = value.value<std::string>()) out.interpreters.emplace(std::string{key.str()}, *s);
        }
    }
    if (wrong) return fail(damaged + *wrong);
    return out;
}

// What a dependency gives build.lua (build-lua-api.md 4).
slice_dependency dependency_of(built_slice const& s, resolved_package const& p, std::size_t configuration)
{
    slice_dependency d;
    d.prefix = s.entry / "prefix";
    d.include = s.output.headers ? *s.output.headers : d.prefix / "include";
    d.libraries = s.output.libraries;
    std::set<fs::path> dirs;
    for (auto const& [name, lib] : s.output.libraries) dirs.insert(lib.parent_path());
    d.lib_dirs.assign(dirs.begin(), dirs.end());
    d.bin = d.prefix / "bin";
    d.version = p.chosen.recipe->version->str();
    d.linkage = s.spec.linkage;
    for (auto const& [name, value] : p.configurations[configuration].options) {
        // a file option: whether the project gave one (build-lua-api.md 4); the file was the dependency's
        option_schema const* o = p.chosen.recipe->find_option(name);
        if (o && o->type == option_type::file) d.options.emplace(name, option_data{!std::get<std::string>(value).empty()});
        else d.options.emplace(name, value);
    }
    return d;
}

// Flags as a TOML array, for the slice key (cache-key.md 3).
std::string flags_text(std::vector<std::string> const& flags)
{
    std::string text = "[";
    for (std::string const& f : flags) text += (text.size() > 1 ? ", " : "") + toml_string(f);
    return text + "]";
}

// Flags as one string in a CMake file: quotes, backslashes and `$` escaped.
std::string cmake_flags_string(std::vector<std::string> const& flags)
{
    std::string text;
    for (std::string const& f : flags) {
        if (!text.empty()) text += ' ';
        for (char c : f) {
            if (c == '"' || c == '\\' || c == '$') text += '\\';
            text += c;
        }
    }
    return text;
}

// The flags part of the CMake toolchain file (build-lua-api.md 6.2): every flag of the slice in the
// *_INIT variables, and no configuration flags of CMake's own after them.
std::string cmake_flags(slice_toolchain const& t)
{
    std::string const ld = cmake_flags_string(t.ldflags);
    return "\n# Flags: otok's, then the project's (otok-toml.md 4.3); CMake adds no configuration flags of its own.\n"
           "set(CMAKE_C_FLAGS_INIT \"" + cmake_flags_string(t.cflags) + "\")\n"
           "set(CMAKE_CXX_FLAGS_INIT \"" + cmake_flags_string(t.cxxflags) + "\")\n"
           "set(CMAKE_EXE_LINKER_FLAGS_INIT \"" + ld + "\")\n"
           "set(CMAKE_SHARED_LINKER_FLAGS_INIT \"" + ld + "\")\n"
           "set(CMAKE_MODULE_LINKER_FLAGS_INIT \"" + ld + "\")\n"
           "foreach(otok_lang C CXX OBJC OBJCXX)\n"
           "    foreach(otok_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)\n"
           "        set(CMAKE_${otok_lang}_FLAGS_${otok_config} \"\" CACHE STRING \"Empty: otok gives every flag\")\n"
           "    endforeach()\n"
           "endforeach()\n";
}

std::string source_pin(locked_location const& l)
{
    switch (l.type) {
    case locked_location::kind::archive: return "archive:" + l.sha256;
    case locked_location::kind::git: return "git:" + l.commit + (l.submodules ? "+submodules" : "");
    case locked_location::kind::path: return "path:" + l.sha256;
    case locked_location::kind::local: return "local";
    }
    return "?";
}

}

std::string package_key(std::string const& library, bool tool)
{
    return (tool ? "tool:" : "") + library;
}

std::string key_text(std::map<std::string, std::string> const& inputs)
{
    std::string text;
    for (auto const& [key, value] : inputs) text += key + " = " + value + "\n";
    return text;
}

std::vector<kept_log> find_build_logs(cache_store const& cache, std::string const& library)
{
    std::vector<kept_log> logs;
    std::error_code ec;
    auto const add = [&](fs::path const& dir, bool failed) {
        auto key = read_key(dir / "key.txt");
        if (key["library"] != library) return;
        for (char const* name : {"build.log", "patch.log"}) {
            fs::path const file = dir / name;
            if (!fs::is_regular_file(file, ec)) continue;
            logs.push_back({file, key["version"], key["slice"], key["configuration"], key["entry"], failed, fs::last_write_time(file, ec)});
        }
    };
    // a library's directory of builds/ holds its versions; failed/ every library's
    fs::path const builds = cache.root() / "builds" / path_from_utf8(library);
    for (auto const& version : fs::directory_iterator{builds, ec}) {
        for (auto const& slice : fs::directory_iterator{version.path() / "slices", ec}) add(slice.path(), false);
    }
    for (auto const& kept : fs::directory_iterator{cache.root() / "failed", ec}) {
        if (kept.is_directory(ec)) add(kept.path(), true);
    }
    std::ranges::sort(logs, [](kept_log const& a, kept_log const& b) { return a.time > b.time; });
    return logs;
}

std::string meson_machine_file(slice_toolchain const& t, std::map<std::string, std::string> const& host, std::string const& pkg_config,
    std::map<std::string, std::vector<std::string>> const& binaries)
{
    auto const quoted = [](std::string const& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'' || c == '\\') out += '\\';
            out += c;
        }
        return out + "'";
    };
    auto const array = [&](std::vector<std::string> const& items) {
        std::string out;
        for (std::string const& item : items) out += (out.empty() ? "" : ", ") + quoted(item);
        return "[" + out + "]";
    };
    bool const cross = !host.empty();
    auto const system = host.find("system");
    bool const objc = system != host.end() && system->second == "darwin";  // Apple slices: Objective-C too

    std::string text = std::string{"# Generated by otok: the meson "} + (cross ? "cross" : "native") + " file of the slice.\n[binaries]\n";
    auto const binary = [&](std::string const& name, std::string const& program) {
        if (binaries.contains(name)) return;  // the target's
        if (!program.empty()) text += name + " = " + quoted(program) + "\n";
    };
    binary("c", t.cc);
    binary("cpp", t.cxx);
    if (objc) {
        binary("objc", t.cc);
        binary("objcpp", t.cxx);
    }
    binary("ar", t.ar);
    binary("strip", t.strip);
    binary("pkg-config", pkg_config);
    for (auto const& [name, program] : binaries) text += name + " = " + (program.size() == 1 ? quoted(program.front()) : array(program)) + "\n";

    // the flags here, not from the environment (build-lua-api.md 6.2)
    text += "\n[built-in options]\n";
    std::vector<std::pair<std::string, std::vector<std::string> const*>> meson_languages{{"c", &t.cflags}, {"cpp", &t.cxxflags}};
    if (objc) {
        meson_languages.emplace_back("objc", &t.cflags);
        meson_languages.emplace_back("objcpp", &t.cxxflags);
    }
    for (auto const& [name, flags] : meson_languages) text += name + "_args = " + array(*flags) + "\n";
    // meson escapes the $ of $ORIGIN for ninja and keeps it at install (build-lua-api.md 6.2)
    std::vector<std::string> link = t.ldflags;
    if (!t.rpath.empty()) link.push_back("-Wl,-rpath," + t.rpath);
    for (auto const& [name, flags] : meson_languages) text += name + "_link_args = " + array(link) + "\n";

    if (cross) {
        text += "\n[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\n";
        for (auto const& [key, value] : host) text += key + " = " + quoted(value) + "\n";
    }
    return text;
}

builder::builder(resolution const& r, manifest const& m, lock_file const& lock, fs::path project_dir,
    std::map<std::string, package_inputs> inputs, std::span<build_target* const> targets, cache_store& cache, build_settings settings,
    std::ostream& progress)
    : r_{r}, m_{m}, lock_{lock}, project_dir_{std::move(project_dir)}, inputs_{std::move(inputs)}, targets_{targets}, cache_{cache},
      settings_{std::move(settings)}, progress_{progress}
{
}

namespace {

std::atomic<bool> exit_checks_skipped{false};

}

void skip_sandbox_exit_checks() noexcept
{
    exit_checks_skipped = true;
}

builder::~builder()
{
    for (fs::path const& d : drives_) virtual_drives::release(d);
}

namespace {

build_target* target_of_platform(std::span<build_target* const> targets, std::string const& platform)
{
    for (build_target* t : targets) {
        if (t->info().find_platform(platform)) return t;
    }
    return nullptr;
}

// The target that builds a slice: of its platform, or the one whose tool slices it is (`native` for Android, docs/PLAN.md
// decision 56).
build_target* target_of_slice(std::span<build_target* const> targets, slice_spec const& spec)
{
    if (build_target* t = target_of_platform(targets, spec.platform)) return t;
    build_target* found = nullptr;
    for (build_target* t : targets) {
        if (t->info().tool_platform == spec.platform) found = t;
    }
    return found;
}

// The path as the kernel compares it; nothing when it is not there.
std::optional<fs::path> resolved_path(fs::path const& p)
{
    std::error_code ec;
    fs::path r = fs::canonical(p, ec);
    if (ec) return std::nullopt;
    return r;
}

bool is_within(fs::path const& p, fs::path const& dir)
{
    auto const [d, q] = std::mismatch(dir.begin(), dir.end(), p.begin(), p.end());
    return d == dir.end();
}

// A task of the build: one entry of one slice.
struct slice_task
{
    resolved_package const* p = nullptr;
    bool tool = false;
    slice_spec spec;
    build_entry entry = build_entry::build;
    std::string id;
    std::string tools;                      // the id of the recipe's build_tools entry; empty: none
};

// The environment of a slice for `when = { environment }` (recipe-toml.md 9.1); null for a target without environments.
build_environment const* environment_of(slice_spec const& spec) noexcept
{
    return spec.environment.compiler.empty() ? nullptr : &spec.environment;
}

// "a/native-x86_64", "a/native-x86_64/debug", "tool:gen/native-x86_64", "a/native-x86_64/build_tools".
std::string slice_id(std::string const& library, bool tool, slice_spec const& spec, build_entry entry = build_entry::build)
{
    return package_key(library, tool) + "/" + slice_name(spec) + (spec.configuration == "debug" ? "/debug" : "")
        + (entry == build_entry::build_tools ? "/build_tools" : "");
}

}

std::expected<std::vector<built_package>, std::vector<error>> builder::build_all()
{
    using failure = std::unexpected<std::vector<error>>;
    auto const failed = [](std::string message) { return failure{std::vector<error>{error{std::move(message)}}}; };
    std::size_t const c = settings_.configuration;
    profile const* const pr = c > 0 ? &m_.profiles[c - 1] : nullptr;

    // The tool slice is the machine's, built by the target of its system (otok-toml.md 7) in tool-build-on's environment, a
    // tool package in its graph's; tools are release and static whatever the project builds (docs/PLAN.md decision 31).
    build_target* const tool_target = host_target(targets_);
    if (tool_target) {
        tool_slice_ = slice_spec{tool_target->info().tool_platform, tool_target->tool_arch(), "", "release", "static"};
        tool_slice_->environment = m_.tool_environment(tool_target->info());  // otok-toml.md 3, 7
    }
    // short drive letters for the cache and the work directories (core/virtual_drives, docs/PLAN.md open item 23), with
    // otok-sandbox.dll, which gives the device map to the children of the build's programs
    if (settings_.virtual_paths && settings_.sandbox.kind == host_sandbox::mechanism::appcontainer && !settings_.sandbox.program.empty()) {
        std::error_code ec;
        fs::create_directories(cache_.tmp() / "build", ec);
        auto const cache_drive = virtual_drives::enable().and_then([&] { return virtual_drives::drive_for(cache_.root()); });
        if (cache_drive) drives_.push_back(*cache_drive);
        auto const drive = cache_drive.and_then([&](fs::path const&) { return virtual_drives::drive_for(cache_.tmp() / "build"); });
        if (drive) drives_.push_back(builds_drive_ = *drive);
        else say("warning: the paths of the builds are not shortened: " + drive.error().message);
    }
    // the environment this machine builds each target of the project in (otok-toml.md 4.9)
    std::map<std::string, build_environment, std::less<>> environments;
    for (target_settings const& ts : m_.targets) {
        for (build_target* t : targets_) {
            if (t->info().name != ts.target) continue;
            auto e = machine_environment_of(*t, ts.build_on);
            if (!e) return failed(e.error().message);
            environments.emplace(ts.target, std::move(*e));
        }
    }

    // dependencies before dependents, tool packages first; only the tool packages some build uses: not those of
    // toolchains found installed (recipe-toml.md 13.1)
    std::vector<std::pair<resolved_package const*, bool>> order;
    std::set<std::string> visited;
    auto visit = [&](auto& self, resolved_package const& p, bool tool) -> void {
        if (!visited.insert(package_key(p.id, tool)).second) return;
        for (std::string const& d : p.dependencies) {
            if (resolved_package const* dp = tool ? r_.find_tool(d) : r_.find(d, p.target)) self(self, *dp, tool);
        }
        for (std::string const& d : p.tool_dependencies) {
            if (resolved_package const* dp = r_.find_tool(d)) self(self, *dp, true);
        }
        order.emplace_back(&p, tool);
    };
    for (resolved_package const& p : r_.packages) {
        if (p.configurations[c].platforms.empty()) continue;
        for (std::string const& d : p.tool_dependencies) {
            if (resolved_package const* dp = r_.find_tool(d)) visit(visit, *dp, true);
        }
    }
    for (resolved_package const& p : r_.packages) visit(visit, p, false);
    if (settings_.tool_roots) {
        for (dependency const& d : m_.tool_deps) {
            if (resolved_package const* dp = r_.find_tool(d.library)) visit(visit, *dp, true);
        }
    }

    // the tasks, each after the ones it needs (docs/PLAN.md decision 20)
    std::vector<slice_task> tasks;
    std::vector<executor_task> graph;
    std::map<std::string, std::size_t> index;                   // slice id → task
    std::map<std::string, std::vector<std::string>> slices_of;  // package key → ids of its build entries
    std::vector<built_package> packages;
    auto const add = [&](slice_task t, std::vector<std::size_t> after) {
        index.emplace(t.id, tasks.size());
        tasks.push_back(std::move(t));
        graph.push_back({std::move(after), {}});
    };
    for (auto const& [p, tool] : order) {
        package_configuration const& pc = p->configurations[c];
        if (pc.platforms.empty()) continue;
        auto recipe_sha256 = recipe_hash(*p, tool);
        if (!recipe_sha256) return failed(p->library + ": " + recipe_sha256.error().message);
        packages.push_back({p->library, tool, *recipe_sha256, {}, {}, {}});
        packages.back().target = p->target;
        packages.back().id = p->id;
        if (auto const in = inputs_.find(package_key(p->id, tool)); in != inputs_.end()) {
            packages.back().recipe_dir = in->second.recipe_dir;
            packages.back().source = in->second.source;
        }
        if (dependency const* entry = tool ? nullptr : m_.find_dep(p->library)) packages.back().bundle_data = entry->bundle_data;
        if (!tool && p->chosen.recipe->products.empty() && !p->chosen.recipe->programs.empty()) {
            return failed(p->library + ": a tool recipe without products can only be a tool dependency (recipe-toml.md 3.1)");
        }
        // nothing to build: the target packages the patched source (recipe-toml.md 12.1)
        if (p->chosen.recipe->delivers_sources) {
            if (tool) return failed(p->library + ": a recipe that delivers its source cannot be a tool dependency");
            if (auto ok = prepare_sources(*p, packages.back()); !ok) return failed(ok.error().message);
            continue;
        }

        std::vector<slice_spec> specs;
        if (tool) {
            if (!tool_target) return failed("no target of this project can build the tool package '" + p->library + "'");
            // in the environment of its graph: tool-build-on's, or another its recipe builds in (docs/PLAN.md decision 86)
            specs.push_back(*tool_slice_);
            specs.back().environment = r_.tool_graphs[p->graph].environment;
        } else {
            for (std::string const& platform : pc.platforms) {
                build_target* target = target_of_platform(targets_, platform);
                if (!target) return failed("building for platform '" + platform + "' is not implemented in this otok");
                target_info const& info = target->info();
                platform_info const& pi = *info.find_platform(platform);
                auto const ts = std::ranges::find(m_.targets, info.name, &target_settings::target);
                std::vector<std::string> archs = pi.architectures;
                auto deployment = ts != m_.targets.end() ? ts->deployment : decltype(ts->deployment){};
                if (ts != m_.targets.end()) {
                    if (auto it = ts->architectures.find(platform); it != ts->architectures.end()) archs = it->second;
                }
                if (pr) {
                    if (auto it = pr->targets.find(info.name); it != pr->targets.end()) {
                        if (auto a = it->second.architectures.find(platform); a != it->second.architectures.end()) archs = a->second;
                        for (auto const& [key, value] : it->second.deployment) deployment.insert_or_assign(key, value);
                    }
                }
                std::string version;
                for (std::string const& key : pi.deployment_keys) {
                    if (auto it = deployment.find(key); it != deployment.end() && version.empty()) version = it->second;
                }
                std::vector<std::string> const configurations = ts != m_.targets.end() ? build_configurations(*ts, pr) : std::vector<std::string>{"release"};
                // the resolution's: the project's choice, the dependents' request or the target tables' (otok-toml.md 5)
                auto const resolved_linkage = pc.linkage.find(platform);
                std::string const linkage = resolved_linkage != pc.linkage.end() ? resolved_linkage->second : linkage_of(m_, pr, p->library, info);
                std::map<std::string, std::string, std::less<>> choices;
                for (target_choice const& tc : info.choices) choices.emplace(tc.key, tc.values.front());
                if (ts != m_.targets.end()) {
                    for (auto const& [key, value] : ts->choices) choices.insert_or_assign(key, value);
                }
                auto const machine = environments.find(info.name);
                build_environment environment = machine != environments.end() ? machine->second : build_environment{};
                // a package of a recipe that does not build in the graph's environment: in its own, of the same system and ABI
                // (otok-toml.md 4.9, docs/PLAN.md decision 91)
                if (!environment.compiler.empty() && !p->environment.compiler.empty() && p->environment.compiler != environment.compiler) {
                    environment.compiler = p->environment.compiler;
                    if (auto found = target->find_compiler(environment); !found) return failed(p->library + ": " + found.error().message);
                }
                for (std::string const& configuration : configurations) {
                    for (std::string const& arch : archs) {
                        specs.push_back({platform, arch, version, configuration, linkage, choices, environment});
                    }
                }
            }
        }

        // every slice needs the tool packages its recipe uses
        std::vector<std::size_t> tool_after;
        for (std::string const& d : p->tool_dependencies) {
            for (std::string const& id : slices_of[package_key(d, true)]) tool_after.push_back(index.at(id));
        }
        // build_tools first, once, on the tool slice (build-lua-api.md 2)
        std::string tools;
        if (!tool) {
            auto const in = inputs_.find(package_key(p->id, false));
            if (in == inputs_.end()) return failed(p->library + ": its inputs were not fetched");
            auto defined = script_defines(in->second.recipe_dir / "build.lua", p->library + "/build.lua", "build_tools");
            if (!defined) return failure{std::vector<error>{defined.error()}};
            if (*defined) {
                if (!tool_target) return failed(p->library + ": no target of this project can run build_tools");
                tools = slice_id(p->id, false, *tool_slice_, build_entry::build_tools);
                add({p, false, *tool_slice_, build_entry::build_tools, tools, {}}, tool_after);
            }
        }
        for (slice_spec const& spec : specs) {
            std::vector<std::size_t> after = tool_after;
            if (!tools.empty()) after.push_back(index.at(tools));
            for (std::string const& d : p->dependencies) {
                resolved_package const* dp = tool ? r_.find_tool(d) : r_.find(d, p->target);
                if (!dp) continue;
                if (auto it = index.find(slice_id(dp->id, tool, spec)); it != index.end()) after.push_back(it->second);
            }
            std::string const id = slice_id(p->id, tool, spec);
            slices_of[package_key(p->id, tool)].push_back(id);
            add({p, tool, spec, build_entry::build, id, tools}, std::move(after));
        }
    }

    // what the toolchains of the slices need outside the cache, readable to the AppContainer before any build starts, not
    // when a slice in the middle comes to it (docs/PLAN.md decision 109); a slice whose toolchain fails says so itself
    if (settings_.isolation == build_isolation::sandbox && settings_.sandbox.kind == host_sandbox::mechanism::appcontainer) {
        // the targets keep what they discover: a profile per task costs little
        std::vector<std::pair<fs::path, std::string>> needed;
        for (slice_task const& t : tasks) {
            build_target* target = target_of_slice(targets_, t.spec);
            if (!target) continue;
            result<toolchain_profile> profile = fail("");
            {
                std::lock_guard lock{mutex_};
                profile = target->profile(t.spec);
            }
            if (!profile) continue;
            for (fs::path const& p : profile->read_paths) {
                needed.emplace_back(p, "the toolchain of '" + target->info().name + "' (" + profile->description + ")");
            }
        }
        if (auto ok = check_sandbox_reads(needed); !ok) return failed(ok.error().message);
    }

    broker_ = settings_.broker;
    if (!broker_) {
        own_broker_ = std::make_unique<job_broker>(settings_.jobs, cache_.tmp());
        broker_ = own_broker_.get();
    }
    for (auto const& [package, ids] : slices_of) slices_done_.emplace(package, std::pair<std::size_t, std::size_t>{0, ids.size()});
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        graph[i].run = [this, &t = tasks[i]](std::size_t) -> result<void> {
            built_slice const* tools = nullptr;
            if (!t.tools.empty()) {
                std::lock_guard lock{mutex_};
                tools = &built_.at(t.tools);
            }
            auto s = build_slice(*t.p, t.tool, t.spec, t.entry, tools);
            if (!s) return std::unexpected{s.error()};
            std::lock_guard lock{mutex_};
            built_.insert_or_assign(t.id, std::move(*s));
            return {};
        };
    }
    // the output of builds one after another (docs/PLAN.md decision 59); each still has every job of the jobserver
    bool const one_at_a_time = settings_.verbosity >= 2;
    if (one_at_a_time && settings_.jobs > 1) {
        say("note: -vv builds one slice at a time, so that their output does not interleave; each still runs " + std::to_string(settings_.jobs) + " jobs");
    }
    if (auto errors = run_tasks(graph, *broker_, one_at_a_time ? 1 : std::numeric_limits<std::size_t>::max()); !errors.empty()) {
        // once each: the slices under way when one fails can fail alike (a host without a sandbox fails them all)
        std::set<std::string> seen;
        std::erase_if(errors, [&seen](error const& e) { return !seen.insert(e.message).second; });
        return failure{std::move(errors)};
    }

    for (built_package& bp : packages) {
        for (std::string const& id : slices_of[package_key(bp.id, bp.tool)]) bp.slices.push_back(built_.at(id));
        // every slice returns the same libraries (build-lua-api.md 8)
        auto const names = [](built_slice const& x) {
            std::vector<std::string> n;
            for (auto const& [name, path] : x.output.libraries) n.push_back(name);
            return n;
        };
        for (built_slice const& s : bp.slices) {
            if (names(s) != names(bp.slices.front())) {
                std::string a, b;
                for (std::string const& n : names(bp.slices.front())) a += (a.empty() ? "" : ", ") + n;
                for (std::string const& n : names(s)) b += (b.empty() ? "" : ", ") + n;
                return failed(bp.library + ": the slices return different libraries: " + slice_name(bp.slices.front().spec) + " {" + a + "}, "
                    + slice_name(s.spec) + " {" + b + "}");
            }
        }
    }
    return packages;
}

result<fs::path> builder::toolchain_program(resolved_package const& tool_package, std::string const& name)
{
    recipe const& rec = *tool_package.chosen.recipe;
    if (std::ranges::find(rec.toolchains, name, &toolchain_requirement::name) == rec.toolchains.end()) {
        return fail("'" + name + "' is not in [toolchains] of its recipe");
    }
    auto const edge = std::ranges::find_if(rec.tool_dependencies, [&](recipe_dependency const& d) { return std::ranges::find(d.programs, name) != d.programs.end(); });
    if (edge != rec.tool_dependencies.end()) {
        auto const i = static_cast<std::size_t>(edge - rec.tool_dependencies.begin());
        if (i >= tool_package.tool_edges.size() || tool_package.tool_edges[i].empty()) return fail("its recipe was not resolved");
        std::lock_guard lock{mutex_};
        auto const built = built_.find(slice_id(tool_package.tool_edges[i], true, *tool_slice_));
        if (built == built_.end()) return fail("its recipe was not built");
        auto const program = built->second.output.programs.find(name);
        if (program == built->second.output.programs.end()) return fail("its recipe builds no program '" + name + "'");
        if (built->second.output.interpreters.contains(name)) return fail("it is a script itself");
        return program->second;
    }
    // found installed when the tool package was checked, on the target of the tool slice
    for (build_target* t : targets_) {
        if (t->info().tool_platform != tool_slice_->platform) continue;
        auto const chosen = settings_.toolchains.find(toolchain_choice_key(t->info().name, package_key(tool_package.library, true)));
        if (chosen == settings_.toolchains.end()) continue;
        if (auto const found = chosen->second.find(name); found != chosen->second.end()) return found->second.program;
    }
    return fail("it was not found");
}

compile_flags builder::project_flags(std::string const& library, bool tool, std::string const& target) const
{
    compile_flags out;
    auto const add = [&out](compile_flags const& f) {
        out.cflags.insert(out.cflags.end(), f.cflags.begin(), f.cflags.end());
        out.cxxflags.insert(out.cxxflags.end(), f.cxxflags.begin(), f.cxxflags.end());
        out.ldflags.insert(out.ldflags.end(), f.ldflags.begin(), f.ldflags.end());
    };
    std::size_t const c = settings_.configuration;
    profile const* const pr = c > 0 ? &m_.profiles[c - 1] : nullptr;
    // the target table's: not for tool dependencies, which run on the build machine
    if (!tool) {
        auto const ts = std::ranges::find(m_.targets, target, &target_settings::target);
        if (ts != m_.targets.end()) add(ts->flags);
        if (pr) {
            if (auto it = pr->targets.find(target); it != pr->targets.end()) add(it->second.flags);
        }
    }
    auto const& deps = tool ? m_.tool_deps : m_.deps;
    auto const dep = std::ranges::find(deps, library, &dependency::library);
    if (dep != deps.end()) add(dep->flags);
    if (pr) {
        auto const& overrides = tool ? pr->tool_deps : pr->deps;
        if (auto it = overrides.find(library); it != overrides.end()) add(it->second.flags);
    }
    return out;
}

std::string builder::slice_done(std::string const& library, bool tool, build_entry entry)
{
    if (entry != build_entry::build) return {};
    std::lock_guard lock{mutex_};
    auto const it = slices_done_.find(package_key(library, tool));
    if (it == slices_done_.end()) return {};
    auto& [done, total] = it->second;
    ++done;
    return total > 1 ? ", " + std::to_string(done) + " of " + std::to_string(total) + " slices" : std::string{};
}

void builder::say(std::string const& line)
{
    std::lock_guard lock{mutex_};
    progress_ << line << "\n" << std::flush;
}

result<std::vector<std::string>> builder::sandbox_command_of(fs::path const& work, fs::path const& recipe_dir, std::vector<fs::path> const& read_paths,
    fs::path const& jobserver_fifo) const
{
    // the kernel compares resolved paths
    auto const resolved = resolved_path;
    sandbox_spec spec;
    spec.writable.push_back(work);
    if (!jobserver_fifo.empty()) {
        if (auto fifo = resolved(jobserver_fifo)) spec.writable_files.push_back(*fifo);
    }
    for (fs::path const& d : settings_.denied_reads) {
        if (auto r = resolved(d)) spec.denied_reads.push_back(*r);
    }
    for (fs::path const& d : {cache_.root(), recipe_dir}) {
        if (auto r = resolved(d)) spec.readable.push_back(*r);
    }
    for (fs::path const& d : read_paths) {
        if (auto r = resolved(d)) spec.readable.push_back(*r);
    }
    // otok itself runs the compilers of tools without the jobserver (build-lua-api.md 6.2)
    if (!settings_.launcher.empty()) {
        if (auto r = resolved(settings_.launcher)) spec.readable.push_back(*r);
    }
    fs::path const file = work / "sandbox.sb";
    if (settings_.sandbox.kind == host_sandbox::mechanism::seatbelt) {
        if (auto ok = write_file_atomic(file, seatbelt_profile(spec)); !ok) return std::unexpected{ok.error()};
    }
    // an AppContainer reaches what the ACLs grant it (docs/PLAN.md decision 103): entries that stay and are inherited, so
    // only in otok's cache and on otok itself; the rest (Visual Studio, the Windows Kits, LLVM in Program Files) the
    // container reads already, or a build's refusals in sandbox.log name it
    if (settings_.sandbox.kind == host_sandbox::mechanism::appcontainer) {
        sandbox_spec granted = spec;
        auto const root = resolved(cache_.root());
        auto const launcher = settings_.launcher.empty() ? std::nullopt : resolved(settings_.launcher);
        std::erase_if(granted.readable, [&](fs::path const& p) {
            if (launcher && p == *launcher) return false;
            return !root || !is_within(p, *root);
        });
        if (auto ok = grant_sandbox_access(settings_.sandbox, granted); !ok) return std::unexpected{ok.error()};
    }
    return sandbox_command(settings_.sandbox, spec, file);
}

result<void> builder::check_sandbox_reads(std::vector<std::pair<fs::path, std::string>> const& needed)
{
    if (settings_.isolation != build_isolation::sandbox || settings_.sandbox.kind != host_sandbox::mechanism::appcontainer) return {};
    // what lies in the cache and otok itself are granted by otok (sandbox_command_of); the rest is the user's
    auto const root = resolved_path(cache_.root());
    std::vector<fs::path> paths;
    std::map<fs::path, std::string> who;
    std::vector<std::pair<fs::path, std::string>> refused;
    {
        std::lock_guard lock{mutex_};
        for (auto const& [p, what] : needed) {
            auto const r = resolved_path(p);
            if (!r || (root && is_within(*r, *root)) || who.contains(*r)) continue;
            if (auto const known = sandbox_reads_.find(*r); known != sandbox_reads_.end()) {
                if (!known->second) refused.emplace_back(*r, what);
                continue;
            }
            paths.push_back(*r);
            who.emplace(*r, what);
        }
    }
    auto unreadable = sandbox_unreadable(settings_.sandbox, paths);
    if (!unreadable) return std::unexpected{unreadable.error()};
    {
        std::lock_guard lock{mutex_};
        for (fs::path const& p : paths) {
            bool const readable = std::ranges::find(*unreadable, p) == unreadable->end();
            sandbox_reads_.insert_or_assign(p, readable);
            if (!readable) refused.emplace_back(p, who.at(p));
        }
    }
    if (refused.empty()) return {};
    std::ranges::sort(refused);
    std::string text = "the sandbox cannot read what the builds need; otok does not change the access of files outside its cache "
                       "(docs/PLAN.md decision 109):";
    for (auto const& [p, what] : refused) text += "\n  '" + path_to_utf8(fs::path{p}.make_preferred()) + "', for " + what;
    text += "\nlet it read them, once (as their owner or an administrator):";
    for (auto const& [p, what] : refused) text += "\n  " + sandbox_read_command(settings_.sandbox, p);
    text += "\nor build without the sandbox: --no-sandbox, OTOK_SANDBOX=0 or 'sandbox = false' in config.toml";
    return fail(text);
}

void builder::check_sandbox_exits()
{
    // the load of a sandboxed build cannot tell it from the kernel's own work (ACLs written, many processes ending at
    // once took most of a core on a healthy machine): empty processes before it can
    constexpr int processes = 50;
    constexpr double much = 0.005;  // seconds per process; healthy well under 0.001, the bad state ~0.02
    auto const started = std::chrono::steady_clock::now();
    std::uint64_t const task = settings_.display ? settings_.display->begin("checking the sandbox") : 0;
    auto const cost = sandbox_exit_cost(settings_.sandbox, processes);
    if (settings_.display) settings_.display->end(task);
    if (!cost) {
        say("warning: cannot check the sandbox: " + cost.error().message);
        return;
    }
    std::chrono::duration<double> const took = std::chrono::steady_clock::now() - started;
    char text[160];
    std::snprintf(text, sizeof text, "checked the sandbox: %.2f ms of the kernel for each of its processes that ends (%.1f s)", *cost * 1000,
        took.count());
    say(text);
    if (*cost > much) {
        say("warning: Windows spends much time in the kernel after each process of the sandbox ends: builds will be slow and the "
            "desktop may stall; a reboot usually clears it, and --no-sandbox builds without the sandbox");
    }
}

jobserver_protocol builder::jobserver_client(std::string const& tool, env_vars const& env)
{
    auto program = find_program(tool, env, cache_.tmp());
    if (!program) return jobserver_protocol::none;
    std::string const key = path_to_utf8(*program);
    {
        std::lock_guard lock{mutex_};
        if (auto it = jobserver_clients_.find(key); it != jobserver_clients_.end()) return it->second;
    }
    jobserver_protocol const protocol = jobserver_protocol_of(tool, env);
    std::lock_guard lock{mutex_};
    jobserver_clients_.insert_or_assign(key, protocol);
    return protocol;
}

locked_package const* builder::locked_of(resolved_package const& p, bool tool) const
{
    if (tool) return lock_.find_tool(p.id, r_.tool_graphs[p.graph].environment.text());
    return lock_.find(p.library, p.target, r_.graphs[p.graph].environment.text());
}

resolved_package const* builder::tool_dependency_of(resolved_package const& p, std::string_view library) const
{
    for (std::string const& id : p.dependencies) {
        if (split_tool_reference(id).first == library) return r_.find_tool(id);
    }
    return nullptr;
}

result<std::string> builder::recipe_hash(resolved_package const& p, bool tool)
{
    std::string const key = package_key(p.id, tool);
    // the recipe of otok.dev.toml: its working tree, which the dev slice keeps apart (otok-dev-toml.md 4)
    if (auto const in = inputs_.find(key); in != inputs_.end() && in->second.dev && in->second.dev->recipe) return std::string{"dev"};
    if (!p.chosen.recipe_sha256.empty()) return p.chosen.recipe_sha256;
    if (auto it = recipe_hashes_.find(key); it != recipe_hashes_.end()) return it->second;
    auto const in = inputs_.find(key);
    if (in == inputs_.end()) return fail("its inputs were not fetched");
    auto hash = tree_hash(in->second.recipe_dir);  // a local recipe: its contents at build time
    if (!hash) return fail("cannot hash the recipe: " + hash.error().message);
    recipe_hashes_.emplace(key, *hash);
    return *hash;
}

result<void> builder::apply_patches(recipe const& rec, fs::path const& recipe_dir, fs::path const& source, fs::path const& work,
    fs::path const& log) const
{
    for (std::string const& patch : rec.source.patches) {
        if (!settings_.git) return fail("applying patches needs git, and git is not in PATH");
        git_tool git = *settings_.git;
        // git must not take a repository around the copy for the one to patch
        git.env["GIT_CEILING_DIRECTORIES"] = path_to_utf8(virtual_drives::to_real(work));
        // in the real directory: git is otok's, not the build's, and has no otok-sandbox.dll to give it the paths through a
        // virtual drive, so the working directory it is given and the one it resolves would differ (core/virtual_drives)
        auto applied = run_git(git, {"apply", "-p1", "--whitespace=nowarn", path_to_utf8(recipe_dir / path_from_utf8(patch))},
            virtual_drives::to_real(source), log);
        if (!applied) return fail("the patch '" + patch + "' does not apply: " + applied.error().message);
    }
    return {};
}

result<void> builder::prepare_sources(resolved_package const& p, built_package& out)
{
    recipe const& rec = *p.chosen.recipe;
    std::string const label = p.library + " " + rec.version->str();
    locked_package const* locked = locked_of(p, false);
    if (!locked) return fail(label + " is not in otok.lock");
    auto const in = inputs_.find(package_key(p.id, false));
    if (in == inputs_.end()) return fail(label + ": its inputs were not fetched");
    package_inputs const& inputs = in->second;

    // The key (cache-key.md 3.1): what the patched tree depends on.
    std::map<std::string, std::string> key;
    key["key-format"] = key_format;
    key["entry"] = "sources";
    key["library"] = p.library;
    key["version"] = rec.version->str();
    key["revision"] = std::to_string(rec.revision);
    key["recipe-sha256"] = out.recipe_sha256;
    key["source"] = source_pin(locked->source);
    if (locked->source.type == locked_location::kind::local) {
        auto hash = tree_hash(inputs.source, tree_links::record);
        if (!hash) return fail(label + ": cannot hash the source in '" + slashed(inputs.source) + "': " + hash.error().message);
        key["source"] = "local:" + *hash;
    }
    std::string const text = key_text(key);
    out.sources_key = sha256_hex(text);
    fs::path const entry = cache_.root() / "builds" / p.library / (rec.version->str() + "-r" + std::to_string(rec.revision)) / "sources"
        / out.sources_key.substr(0, 32);
    out.sources = entry / "source";

    std::error_code ec;
    if (fs::exists(entry, ec)) {
        cache_.mark_used(entry);
        say(label + " source: cached");
        return {};
    }
    say("patching the source of " + label);
    fs::path const work = cache_.tmp() / "build" / (p.library + "-sources");
    auto made = cache_.ensure(entry, "patching the source of " + label, [&](fs::path const& staging) -> result<void> {
        fs::remove_all(long_path(work), ec);
        fs::create_directories(work, ec);
        fs::path const log = work / "patch.log";
        std::ofstream{log, std::ios::binary};
        auto failed = [&](std::string const& message) -> result<void> {
            std::string const tail = log_tail(log, 30);
            std::string const kept = keep_failure(cache_, work, log, text, p.library + "-sources", settings_.keep_build_dirs);
            return fail(label + ": " + message + (tail.empty() ? "" : "\nthe end of the log:\n" + tail) + kept);
        };
        if (auto ok = copy_tree(inputs.source, work / "source"); !ok) return failed("cannot copy the source: " + ok.error().message);
        if (auto ok = apply_patches(rec, inputs.recipe_dir, work / "source", work, log); !ok) return failed(ok.error().message);
        fs::create_directories(long_path(staging), ec);
        rename_path(work / "source", staging / "source", ec);
        if (ec) return failed("cannot move the patched source into the cache: " + ec.message());
        std::ofstream{long_path(staging / "key.txt"), std::ios::binary} << text;
        fs::remove_all(long_path(work), ec);
        return {};
    });
    if (!made) return std::unexpected{made.error()};
    return {};
}

result<built_slice> builder::build_slice(resolved_package const& p, bool tool, slice_spec const& spec, build_entry entry,
    built_slice const* tools)
{
    std::size_t const c = settings_.configuration;
    recipe const& rec = *p.chosen.recipe;
    bool const building_tools = entry == build_entry::build_tools;
    std::string const label = (tool ? "tool " : "") + p.library + " " + rec.version->str() + (building_tools ? " (build_tools)" : "");
    locked_package const* locked = locked_of(p, tool);
    if (!locked) return fail(label + " is not in otok.lock");
    auto const in = inputs_.find(package_key(p.id, tool));
    if (in == inputs_.end()) return fail(label + ": its inputs were not fetched");
    package_inputs const& inputs = in->second;

    build_target* const target = target_of_slice(targets_, spec);
    if (!target) return fail(label + ": no target builds platform '" + spec.platform + "'");
    std::string const os = target->info().name;
    result<toolchain_profile> profile = fail("");
    {
        // a target may discover its tools on first use
        std::lock_guard lock{mutex_};
        profile = target->profile(spec);
        if (profile && described_.emplace(os, profile->description).second
            && (!settings_.announced || settings_.announced->first(os + ": " + profile->description))) {
            progress_ << "toolchain of '" << os << "': " << profile->description << "\n" << std::flush;
        }
    }
    if (!profile) return fail(label + " for " + slice_label(spec) + ": " + profile.error().message);

    merged_options const& options = p.configurations[c].options;
    // a slice this one needs: built before it started (the executor's order)
    auto const find_built = [&](resolved_package const* dp, bool tool_dep) -> built_slice const* {
        if (!dp) return nullptr;
        std::lock_guard lock{mutex_};
        auto const it = built_.find(slice_id(dp->id, tool_dep, tool_dep ? *tool_slice_ : spec));
        return it == built_.end() ? nullptr : &it->second;
    };

    // The key (cache-key.md 2, 3).
    std::map<std::string, std::string> key;
    key["key-format"] = key_format;
    key["toolchain-profile"] = toolchain_profile_version;
    key["os"] = os;
    key["slice"] = slice_name(spec);
    if (!spec.environment.compiler.empty()) key["environment"] = spec.environment.text();
    if (host_libc() == "musl") key["host-libc"] = "musl";  // absent elsewhere: the keys of other machines unchanged (decision 105)
    key["linkage"] = spec.linkage;
    key["library"] = p.library;
    key["version"] = rec.version->str();
    key["revision"] = std::to_string(rec.revision);
    key["configuration"] = spec.configuration;
    if (!spec.deployment.empty()) key["deployment"] = spec.deployment;
    auto recipe_sha256 = recipe_hash(p, tool);
    if (!recipe_sha256) return fail(label + ": " + recipe_sha256.error().message);
    key["recipe-sha256"] = *recipe_sha256;
    key["source"] = source_pin(locked->source);
    // the working trees of otok.dev.toml: `dev`, their contents are the dev slice's business (otok-dev-toml.md 4)
    bool const dev = inputs.dev.has_value();
    if (dev && inputs.dev->source) {
        key["source"] = "dev";
    } else if (locked->source.type == locked_location::kind::local) {
        auto hash = tree_hash(inputs.source, tree_links::record);
        if (!hash) return fail(label + ": cannot hash the source in '" + slashed(inputs.source) + "': " + hash.error().message);
        key["source"] = "local:" + *hash;
    }
    for (locked_resource const& res : locked->resources) {
        auto const declared = std::ranges::find(rec.resources, res.name, &recipe_resource::name);
        if (declared != rec.resources.end() && declared->when && !condition_holds(*declared->when, options, "", "", spec.arch, environment_of(spec))) continue;
        key["resource." + res.name] = dev && inputs.dev->resources.contains(res.name) ? "dev" : source_pin(res.location);
    }
    // file options (recipe-toml.md 8.3): their contents in the key, not their paths; a copy for the build
    std::map<std::string, std::string> option_keys;
    std::map<std::string, fs::path> option_files;
    for (auto const& [name, value] : options) {
        option_schema const* o = rec.find_option(name);
        if (!o || o->type != option_type::file) {
            option_keys[name] = format_option_value(value);
            continue;
        }
        std::string const& path = std::get<std::string>(value);
        if (path.empty()) {
            option_keys[name] = "none";
            continue;
        }
        fs::path const file = project_dir_ / path_from_utf8(path);
        auto hash = sha256_file(file);
        if (!hash) return fail(label + ": the file '" + path + "' of option '" + name + "': " + hash.error().message);
        option_keys[name] = "sha256:" + *hash;
        option_files.emplace(name, file);
    }
    for (auto const& [name, value] : option_keys) key["option." + name] = value;
    for (auto const& [name, value] : profile->identity) key["toolchain." + name] = value;
    // whether this slice needs a toolchain: one the check found for the package may be for some architectures or
    // environments only (recipe-toml.md 9.1)
    auto const slice_needs = [&](std::string const& name) {
        auto const req = std::ranges::find(rec.toolchains, name, &toolchain_requirement::name);
        return req == rec.toolchains.end() || !req->when
            || condition_holds(*req->when, options, os, "", spec.arch, environment_of(spec));
    };
    // the recipe's toolchains as check_toolchains found them (recipe-toml.md 13)
    chosen_toolchains const* toolchains = nullptr;
    if (!rec.toolchains.empty()) {
        auto const it = settings_.toolchains.find(toolchain_choice_key(os, package_key(p.id, tool)));
        if (it == settings_.toolchains.end()) return fail(label + ": its toolchains were not checked for the target '" + os + "'");
        toolchains = &it->second;
        for (auto const& [name, found] : it->second) {
            if (slice_needs(name)) key["toolchains." + name] = found.version;
        }
    }
    compile_flags const project = building_tools ? compile_flags{} : project_flags(p.library, tool, os);
    for (auto const& [name, flags] : {std::pair{"cflags", &project.cflags}, std::pair{"cxxflags", &project.cxxflags},
             std::pair{"ldflags", &project.ldflags}}) {
        if (!flags->empty()) key[std::string{"flags."} + name] = flags_text(*flags);
    }
    // the standards of the target, as the resolution chose them (otok-toml.md 5.1); a tool dependency's own; build_tools get
    // none, as they get no project flags
    language_standards standards;
    if (tool && !building_tools) {
        for (language const l : languages) standards[l] = standard_of(m_, nullptr, p.library, true, target->info(), l);
    } else if (!building_tools && settings_.configuration < r_.standards.size()) {
        auto const& chosen = r_.standards[settings_.configuration];
        if (auto it = chosen.find(os); it != chosen.end()) standards = it->second;
    }
    for (language const l : languages) {
        if (standards[l]) key[std::string{standard_key(l)}] = std::to_string(*standards[l]);
    }

    slice_context slice;
    for (recipe_dependency const& d : rec.dependencies) {
        if (building_tools) break;  // the tool slice has only the tool dependencies
        if (d.when && !condition_holds(*d.when, options, os, spec.platform)) continue;
        // a tool package names its dependencies by id (otok-lock.md 5.7)
        resolved_package const* dp = tool ? tool_dependency_of(p, d.library) : r_.find(d.library, p.target);
        built_slice const* dep = find_built(dp, tool);
        if (!dep || !dp) return fail(label + ": its dependency '" + d.library + "' was not built for " + slice_label(spec));
        key["dep." + d.library] = dep->key;
        slice.deps.emplace(d.library, dependency_of(*dep, *dp, c));
    }
    // the programs of toolchains their recipes built (recipe-toml.md 13.1, 13.2): name → program; scripts among them → their interpreter
    std::map<std::string, fs::path> built_toolchains;
    std::map<fs::path, fs::path> interpreted;
    for (std::size_t i = 0; i < rec.tool_dependencies.size(); ++i) {
        recipe_dependency const& d = rec.tool_dependencies[i];
        if (d.when && !condition_holds(*d.when, options, os, spec.platform, spec.arch, environment_of(spec))) continue;
        if (i >= p.tool_edges.size() || p.tool_edges[i].empty()) continue;  // inactive in every configuration resolved
        resolved_package const* dp = r_.find_tool(p.tool_edges[i]);
        built_slice const* dep = find_built(dp, true);
        if (!dep || !dp) return fail(label + ": its tool dependency '" + p.tool_edges[i] + "' was not built");
        key["tool." + dp->library] = dep->key;
        if (d.programs.empty()) {
            slice.tool_deps.emplace(dp->library, dependency_of(*dep, *dp, c));
            continue;
        }
        for (std::string const& name : d.programs) {
            auto const program = dep->output.programs.find(name);
            if (program == dep->output.programs.end()) {
                return fail(label + ": the toolchain '" + name + "' comes from the tool recipe " + dp->library + " " + dp->chosen.recipe->version->str()
                    + ", which builds no program '" + name + "' ([tool] programs, recipe-toml.md 13.1)");
            }
            built_toolchains.emplace(name, program->second);
            // a script: run by the interpreter the tool recipe built it for, a toolchain of its own (docs/PLAN.md decision 70)
            if (auto const interpreter = dep->output.interpreters.find(name); interpreter != dep->output.interpreters.end()) {
                auto run_by = toolchain_program(*dp, interpreter->second);
                if (!run_by) return fail(label + ": the toolchain '" + name + "' is a script of " + interpreter->second + ": " + run_by.error().message);
                interpreted.emplace(program->second.lexically_normal(), *run_by);
            }
        }
    }

    // what the build is given of the cache, through its drive (core/virtual_drives)
    if (!builds_drive_.empty()) {
        auto const shorten = [](slice_dependency& d) {
            d.prefix = virtual_drives::to_virtual(d.prefix);
            d.include = virtual_drives::to_virtual(d.include);
            for (auto& [name, lib] : d.libraries) lib = virtual_drives::to_virtual(lib);
            for (fs::path& dir : d.lib_dirs) dir = virtual_drives::to_virtual(dir);
            d.bin = virtual_drives::to_virtual(d.bin);
        };
        for (auto& [name, d] : slice.deps) shorten(d);
        for (auto& [name, d] : slice.tool_deps) shorten(d);
        for (auto& [name, program] : built_toolchains) program = virtual_drives::to_virtual(program);
        std::map<fs::path, fs::path> through;
        for (auto const& [program, run_by] : interpreted)
            through.emplace(virtual_drives::to_virtual(program).lexically_normal(), virtual_drives::to_virtual(run_by));
        interpreted = std::move(through);
    }
    fs::path const script = inputs.recipe_dir / "build.lua";
    std::string const script_label = p.library + "/build.lua";
    if (tools) {
        key["build-tools"] = tools->key;
        slice.tools_prefix = builds_drive_.empty() ? tools->entry / "prefix" : virtual_drives::to_virtual(tools->entry / "prefix");
    }
    key["entry"] = building_tools ? "build_tools" : "build";
    // build.lua sees it (slice.is_tool): a library that is a tool too builds its programs there only (recipe-toml.md 13.2)
    if (tool) key["tool"] = "true";
    std::string const text = key_text(key);
    std::string const hex = sha256_hex(text);
    built_slice out;
    out.spec = spec;
    out.key = hex;
    out.option_keys = option_keys;
    out.entry = cache_.root() / "builds" / p.library / (rec.version->str() + "-r" + std::to_string(rec.revision)) / "slices" / hex.substr(0, 32);
    std::string const work_name = p.library + "-" + slice_name(spec) + (spec.configuration == "debug" ? "-debug" : "") + (tool ? "-tool" : building_tools ? "-tools" : "");
    // a dev slice: built every time in its work directory, kept in dev/, which is its entry (otok-dev-toml.md 4)
    if (dev) {
        std::string const project_key = sha256_hex(path_to_utf8(fs::absolute(project_dir_))).substr(0, 32);
        out.entry = cache_.root() / "dev" / project_key / work_name;
    }

    std::error_code ec;
    if (!dev && fs::exists(out.entry, ec)) {
        auto result = read_result(out.entry);
        if (result) {
            out.output = std::move(*result);
            out.cached = true;
            cache_.mark_used(out.entry);
            say(label + " " + slice_label(spec) + ": cached" + slice_done(p.id, tool, entry));
            return out;
        }
        // damaged (by an older otok, or by hand): removed and built again (cache.md 6.2)
        say("warning: " + result.error().message + "; building it again");
        auto gone = cache_.discard(out.entry, "removing the damaged " + label + " for " + slice_label(spec),
            [](fs::path const& entry) { return !read_result(entry); });
        if (!gone) return std::unexpected{gone.error()};
    }

    // on a terminal the slice is shown below the lines while it builds (docs/PLAN.md decision 59); with -vv this heads its output
    if (!settings_.display || settings_.verbosity >= 2) say("building " + label + " for " + slice_label(spec));
    // below the lines while it builds (docs/PLAN.md decision 59)
    struct shown_task
    {
        progress_display* display;
        std::uint64_t id;
        ~shown_task()
        {
            on_job_wait({});
            if (display) display->end(id);
        }
    } const shown{settings_.display, settings_.display ? settings_.display->begin(label + "  " + slice_label(spec), current_priority()) : 0};
    // and what it does now
    auto const show_stage = [&shown](std::string text) {
        if (shown.display) shown.display->stage(shown.id, std::move(text));
    };
    // and whether it waits for a job (docs/PLAN.md decision 61)
    if (shown.display) on_job_wait([display = shown.display, id = shown.id](bool waits, std::string const& what) { display->waiting(id, waits, what); });
    std::string const id = "[" + slice_id(p.id, tool, spec, entry) + "] ";
    bool tell = false;
    {
        std::lock_guard const lock{mutex_};
        tell = !isolation_told_ && settings_.isolation == build_isolation::off;
        isolation_told_ = isolation_told_ || tell;
    }
    if (tell && settings_.isolation == build_isolation::off) {
        say("warning: the sandbox is off (--no-sandbox, OTOK_SANDBOX or 'sandbox' in config.toml): builds are not isolated");
    }
    // never unisolated unless asked (docs/PLAN.md decision 103)
    if (settings_.isolation == build_isolation::unavailable) {
        std::string const& why = settings_.sandbox.why_not;
        return fail("builds cannot be isolated on this host" + (why.empty() ? std::string{} : " (" + why + ")")
            + "; to build without the sandbox, give --no-sandbox, set OTOK_SANDBOX=0 or 'sandbox = false' in config.toml");
    }
    // what the container must read outside the cache (docs/PLAN.md decision 109): the programs found installed are known
    // only now; the profile's paths were checked before the builds, so a refusal here is theirs
    {
        std::vector<std::pair<fs::path, std::string>> needed;
        for (fs::path const& path : profile->read_paths) needed.emplace_back(path, "the toolchain of '" + os + "' (" + profile->description + ")");
        if (toolchains) {
            for (auto const& [name, found] : *toolchains) {
                if (slice_needs(name)) needed.emplace_back(found.program.parent_path(), "the toolchain '" + name + "' ('" + path_to_utf8(found.program) + "')");
            }
        }
        if (auto ok = check_sandbox_reads(needed); !ok) return fail(label + " for " + slice_label(spec) + ": " + ok.error().message);
    }
    // before the first sandboxed process, the slices that come meanwhile waiting, so the check counts its own processes
    if (settings_.isolation == build_isolation::sandbox && settings_.sandbox.kind == host_sandbox::mechanism::appcontainer
        && !exit_checks_skipped.load()) {
        std::unique_lock lock{mutex_};
        if (!exits_check_started_) {
            exits_check_started_ = true;
            lock.unlock();
            check_sandbox_exits();
            lock.lock();
            exits_checked_ = true;
            exits_checked_cv_.notify_all();
        } else if (!exits_checked_) {
            if (shown.display) shown.display->waiting(shown.id, true, "the check of the sandbox");
            exits_checked_cv_.wait(lock, [this] { return exits_checked_; });
            if (shown.display) shown.display->waiting(shown.id, false);
        }
    }
    auto const started = std::chrono::steady_clock::now();
    fs::path const work = dev ? out.entry : cache_.tmp() / "build" / work_name;
    auto const build = [&](fs::path const& staging) -> result<void> {
        // a dev slice keeps source/ and build/ while the rest of its key stays (otok-dev-toml.md 4)
        std::optional<dev_record> record;
        if (dev) {
            record = read_dev_record(work / "dev.toml");
            if (!record || record->key != text) {
                fs::remove_all(long_path(work / "source"), ec);
                fs::remove_all(long_path(work / "build"), ec);
                record = dev_record{};
            }
            for (char const* d : {"prefix", "tmp"}) fs::remove_all(long_path(work / d), ec);
        } else {
            fs::remove_all(long_path(work), ec);
        }
        for (char const* d : {"build", "prefix", "home", "tmp"}) fs::create_directories(work / d);
        // resolved (macOS: /var is a link), as the paths build.lua returns are; through its drive on Windows, where the
        // build sees short paths (core/virtual_drives)
        fs::path const base = builds_drive_.empty() ? fs::canonical(work, ec) : virtual_drives::to_virtual(work);
        fs::path const log = base / "build.log";
        {
            std::ofstream made{log, std::ios::binary};  // exists even when nothing is logged
            // the letters the build's paths are on, for whoever reads the log
            for (fs::path const& d : drives_) made << "otok: " << path_to_utf8(d.root_name()) << " is " << slashed(virtual_drives::to_real(d)) << "\n";
        }
        // the output of the build as it comes (`-vv`); a dev slice's always, unless `quiet`, each line with its slice, since
        // the slices build at once (otok-dev-toml.md 4)
        std::optional<log_follower> follow;
        if (settings_.verbosity >= 2) follow.emplace(log, [this](std::string const& line) { say(line); });
        else if (dev && !inputs.dev->quiet) follow.emplace(log, [this, &id](std::string const& line) { say(id + line); });
        auto failed = [&](std::string message) -> result<void> {
            bool const shown_log = follow.has_value();
            follow.reset();
            std::string const tail = log_tail(log, 30);
            // the tail was just shown as it came
            if (!tail.empty() && !shown_log) message += "\nthe end of the log:\n" + tail;
            // a dev slice's work directory stays as the failure left it
            message += keep_failure(cache_, work, log, text, work_name, settings_.keep_build_dirs && !dev, !dev);
            return fail(label + " for " + slice_label(spec) + ": " + message);
        };
        // the AppContainer's entry given while the work directory is empty, so what is made in it inherits the entry: given
        // later, it was carried to every file of the source copied there, the other slices waiting for their grants
        // meanwhile (docs/PLAN.md decision 103)
        if (settings_.isolation == build_isolation::sandbox && settings_.sandbox.kind == host_sandbox::mechanism::appcontainer) {
            sandbox_spec own;
            own.writable = {base};
            if (auto ok = grant_sandbox_access(settings_.sandbox, own); !ok) return failed(ok.error().message);
        }

        // a fresh copy of the source, resources with `dest`, patches (DESIGN 6.4); in a dev slice, the trees synchronized
        show_stage("sources");
        std::vector<synced_tree> trees;
        // a working tree of otok.dev.toml: what git does not ignore; a tree of the cache: all of it
        auto const tree_of = [&](fs::path const& from, std::string const& dest, bool working) -> result<synced_tree> {
            if (!working) return synced_tree{from, dest, std::nullopt};
            auto files = files_of_checkout(settings_.git, from, log);
            if (!files) return std::unexpected{files.error()};
            return synced_tree{from, dest, std::move(*files)};
        };
        if (dev) {
            auto tree = tree_of(inputs.source, "", inputs.dev->source);
            if (!tree) return failed("cannot list the files of the source: " + tree.error().message);
            trees.push_back(std::move(*tree));
        } else if (auto ok = copy_tree(inputs.source, base / "source"); !ok) return failed("cannot copy the source: " + ok.error().message);
        std::map<std::string, fs::path> resources;
        for (recipe_resource const& res : rec.resources) {
            if (res.when && !condition_holds(*res.when, options, "", "", spec.arch, environment_of(spec))) continue;
            auto const r = inputs.resources.find(res.name);
            if (r == inputs.resources.end()) return failed("the resource '" + res.name + "' was not fetched");
            if (!res.dest) {
                resources.emplace(res.name, builds_drive_.empty() ? r->second : virtual_drives::to_virtual(r->second));
                continue;
            }
            fs::path const dest = base / "source" / path_from_utf8(*res.dest);
            if (dev) {
                auto tree = tree_of(r->second, *res.dest, inputs.dev->resources.contains(res.name));
                if (!tree) return failed("cannot list the files of the resource '" + res.name + "': " + tree.error().message);
                trees.push_back(std::move(*tree));
            } else if (fs::is_directory(r->second, ec)) {
                if (auto ok = copy_tree(r->second, dest); !ok) return failed("cannot place the resource '" + res.name + "': " + ok.error().message);
            } else {
                fs::create_directories(dest.parent_path(), ec);
                fs::copy_file(r->second, dest, fs::copy_options::overwrite_existing, ec);
                if (ec) return failed("cannot place the resource '" + res.name + "': " + ec.message());
            }
            resources.emplace(res.name, dest);
        }
        if (dev) {
            // the patches again when they or a file they touch changed, onto those files as their trees have them
            std::string patches;
            std::set<std::string> touched;
            for (std::string const& patch : rec.source.patches) {
                auto text_of = read_file(inputs.recipe_dir / path_from_utf8(patch));
                if (!text_of) return failed("cannot read the patch '" + patch + "': " + text_of.error().message);
                patches += *text_of;
                touched.merge(patched_files(*text_of));
            }
            dev_record now;
            now.key = text;
            now.patches = patches.empty() ? std::string{} : sha256_hex(patches);
            std::map<std::string, fs::path> origins;
            bool const new_patches = now.patches != record->patches;
            auto copied = sync_trees(trees, base / "source", *record, now, new_patches ? touched : std::set<std::string>{}, origins);
            if (!copied) return failed("cannot synchronize the sources: " + copied.error().message);
            if (!patches.empty() && (new_patches || std::ranges::any_of(touched, [&](std::string const& f) { return copied->contains(f); }))) {
                for (std::string const& f : touched) {
                    fs::path const to = long_path(base / "source" / path_from_utf8(f));
                    auto const origin = origins.find(f);
                    if (origin == origins.end()) {
                        fs::remove(to, ec);  // made by a patch
                    } else if (!copied->contains(f)) {
                        fs::copy_file(origin->second, to, fs::copy_options::overwrite_existing, ec);
                        if (ec) return failed("cannot copy '" + slashed(origin->second) + "': " + ec.message());
                    }
                }
                // patched again next time, should they fail halfway now
                dev_record unpatched = now;
                unpatched.patches.clear();
                if (auto ok = write_dev_record(work / "dev.toml", unpatched); !ok) return failed(ok.error().message);
                show_stage("patches");
                if (auto ok = apply_patches(rec, inputs.recipe_dir, base / "source", work, log); !ok) return failed(ok.error().message);
            }
            // what source/ holds now, whether the build succeeds or not
            if (auto ok = write_dev_record(work / "dev.toml", now); !ok) return failed(ok.error().message);
        } else {
            if (!rec.source.patches.empty()) show_stage("patches");
            if (auto ok = apply_patches(rec, inputs.recipe_dir, base / "source", work, log); !ok) return failed(ok.error().message);
        }
        // the files of file options, where the sandbox lets the build read them
        std::map<std::string, fs::path> option_copies;
        for (auto const& [name, file] : option_files) {
            fs::path const copy = base / "options" / path_from_utf8(name) / file.filename();
            fs::create_directories(copy.parent_path(), ec);
            fs::copy_file(file, copy, fs::copy_options::overwrite_existing, ec);
            if (ec) return failed("cannot copy the file of option '" + name + "': " + ec.message());
            option_copies.emplace(name, copy);
            slice.option_files.push_back(copy);
        }

        slice.name = slice_name(spec);
        slice.os = os;
        slice.platform = spec.platform;
        slice.arch = spec.arch;
        if (!spec.environment.compiler.empty()) {
            slice.environment = spec.environment.text();
            slice.host = std::string{host_machine()};
            slice.host_libc = std::string{host_libc()};
            slice.compiler = spec.environment.compiler;
        }
        slice.triple = profile->triple;
        slice.configuration = spec.configuration;
        slice.linkage = spec.linkage;
        slice.is_tool = tool;
        slice.source = base / "source";
        slice.build = base / "build";
        slice.prefix = base / "prefix";
        slice.recipe = inputs.recipe_dir;
        // the AppContainer reads only what otok grants it, in its cache: a recipe of a path elsewhere (the user's working
        // tree, never touched) is a copy in the work directory, as a recipe of git is one in the cache (docs/PLAN.md
        // decision 103)
        if (settings_.isolation == build_isolation::sandbox && settings_.sandbox.kind == host_sandbox::mechanism::appcontainer) {
            std::error_code failure;
            fs::path const root = fs::weakly_canonical(cache_.root(), failure);
            fs::path const dir = fs::weakly_canonical(inputs.recipe_dir, failure);
            if (auto const [r, d] = std::mismatch(root.begin(), root.end(), dir.begin(), dir.end()); r != root.end()) {
                fs::remove_all(long_path(base / "recipe"), failure);
                if (auto ok = copy_tree(inputs.recipe_dir, base / "recipe"); !ok) return failed("cannot copy the recipe: " + ok.error().message);
                slice.recipe = base / "recipe";
            }
        }
        if (!builds_drive_.empty() && slice.recipe == inputs.recipe_dir) slice.recipe = virtual_drives::to_virtual(inputs.recipe_dir);
        slice.resources = resources;
        for (recipe_variable const& v : rec.vars) {
            for (std::string const& selector : {slice_name(spec), spec.platform, os, std::string{"default"}}) {
                if (auto it = v.values.find(selector); it != v.values.end()) {
                    slice.vars.emplace(v.name, it->second);
                    break;
                }
            }
        }
        // the most a tool without the jobserver may run; the helpers give it what the slice may hold when it starts
        // (docs/PLAN.md decision 61)
        slice.jobs = std::max<std::int64_t>(1, settings_.jobs);
        slice.toolchain = profile->toolchain;
        if (!profile->prefix_map_flag.empty()) {
            std::string const map = profile->prefix_map_flag + path_to_utf8(base) + "=/otok";
            slice.toolchain.cflags.push_back(map);
            slice.toolchain.cxxflags.push_back(map);
        }
        // the standards after otok's flags and before the project's (otok-toml.md 5.1)
        slice.c_standard = standards.c;
        slice.cxx_standard = standards.cxx;
        for (language const l : languages) {
            if (!standards[l]) continue;
            std::string const flag = profile->standard_flag ? profile->standard_flag(l, *standards[l])
                                                            : (l == language::c ? "-std=c" : "-std=c++") + std::to_string(*standards[l]);
            if (!flag.empty()) (l == language::c ? slice.toolchain.cflags : slice.toolchain.cxxflags).push_back(flag);
            (l == language::c ? slice.meson_c_std : slice.meson_cpp_std) = profile->meson_standard
                ? profile->meson_standard(l, *standards[l]) : (l == language::c ? "c" : "c++") + std::to_string(*standards[l]);
        }
        // the project's flags after otok's (otok-toml.md 4.3), in the arrays and in the environment
        auto const append = [](std::vector<std::string>& to, std::vector<std::string> const& from) { to.insert(to.end(), from.begin(), from.end()); };
        append(slice.toolchain.cflags, project.cflags);
        append(slice.toolchain.cxxflags, project.cxxflags);
        append(slice.toolchain.ldflags, project.ldflags);
        for (auto const& [name, flags] : {std::pair{"CFLAGS", &slice.toolchain.cflags}, std::pair{"CXXFLAGS", &slice.toolchain.cxxflags},
                 std::pair{"LDFLAGS", &slice.toolchain.ldflags}}) {
            std::string joined;
            for (std::string const& f : *flags) joined += (joined.empty() ? "" : " ") + f;
            if (!joined.empty()) set_variable(slice.toolchain.env, name, joined);
        }
        // what the slice builds with (`-v`, docs/PLAN.md decision 59)
        if (settings_.verbosity >= 1) {
            auto const joined = [](std::vector<std::string> const& items) {
                std::string text;
                for (std::string const& item : items) text += (text.empty() ? "" : " ") + item;
                return text;
            };
            std::string values;
            for (auto const& [name, value] : option_keys) values += (values.empty() ? "" : ", ") + name + " = " + value;
            if (!values.empty()) say(id + "options: " + values);
            std::string chosen;
            for (language const l : languages) {
                if (standards[l]) chosen += (chosen.empty() ? "" : ", ") + std::string{language_name(l)} + std::to_string(*standards[l]);
            }
            if (!chosen.empty()) say(id + "standards: " + chosen);
            say(id + "compilers: " + slice.toolchain.cc + ", " + slice.toolchain.cxx);
            say(id + "cflags: " + joined(slice.toolchain.cflags));
            say(id + "cxxflags: " + joined(slice.toolchain.cxxflags));
            say(id + "ldflags: " + joined(slice.toolchain.ldflags));
            say(id + "work directory: " + slashed(base));
            say(id + "slice key: " + hex);
            if (settings_.verbosity == 1) {
                slice.echo = [this, &id, &base](std::string const& command, fs::path const& cwd) {
                    std::string const where = relative_to(cwd, base);
                    say(id + "$ " + command + "  (in " + (where.empty() || where.starts_with("..") ? slashed(cwd) : where) + ")");
                };
            }
        }
        // the home of a build, by the names of every system (Python on Windows asks USERPROFILE)
        for (char const* name : {"HOME", "USERPROFILE"}) set_variable(slice.toolchain.env, name, path_to_utf8(base / "home"));
        for (char const* name : {"TMPDIR", "TEMP", "TMP"}) set_variable(slice.toolchain.env, name, path_to_utf8(base / "tmp"));
        set_variable(slice.toolchain.env, "LANG", "C");
        set_variable(slice.toolchain.env, "SOURCE_DATE_EPOCH", source_date_epoch);
        // Python of the build without the user's modules: those of PYTHONPATH and PYTHONHOME, which an environment
        // captured from the user's (Visual Studio's) brings, and the user's site-packages, which Windows finds in
        // APPDATA, not under HOME (docs/PLAN.md decision 71)
        for (char const* name : {"PYTHONPATH", "PYTHONHOME"}) remove_variable(slice.toolchain.env, name);
        set_variable(slice.toolchain.env, "PYTHONNOUSERSITE", "1");
        // the slice's own jobserver, which the broker fills while its tools use every token (docs/PLAN.md decision 61)
        auto jobs = broker_->attach(current_priority());
        if (!jobs) return failed(jobs.error().message);
        std::shared_ptr<job_broker::lease> const lease = std::move(*jobs);
        set_variable(slice.toolchain.env, "MAKEFLAGS", lease->server().makeflags());
        // shown: the job of the slice and the tokens of its tools
        if (shown.display) {
            shown.display->count_jobs(shown.id, [weak = std::weak_ptr<job_broker::lease>{lease}]() -> std::int64_t {
                auto const l = weak.lock();
                return l ? 1 + l->in_use() : 0;
            });
        }
        // a tool without the jobserver runs as many as nothing before this slice wants
        slice.hold_jobs = [lease](std::int64_t most) -> std::pair<std::int64_t, std::function<void()>> {
            std::int64_t const held = lease->hold(most - 1);
            return {held + 1, [lease, held] { lease->put_back(held); }};
        };
        slice.jobserver_client = [this](std::string const& tool, env_vars const& env) { return jobserver_client(tool, env); };
        // the programs of the toolchains, their directories in PATH and readable with the prefix they are
        // installed in (docs/PLAN.md decision 38)
        std::vector<fs::path> read_paths = profile->read_paths;
        if (toolchains) {
            for (auto const& [name, found] : *toolchains) {
                if (!slice_needs(name)) continue;
                slice.toolchains.emplace(name, found.program);
                append_to_path(slice.toolchain.env, found.program.parent_path());
                read_paths.push_back(found.program.parent_path());
                fs::path const installed = fs::canonical(found.program, ec).parent_path().parent_path();
                // never a directory holding HOME: a program in ~/bin must not open the whole of it
                bool const opens_denied = std::ranges::any_of(settings_.denied_reads, [&](fs::path const& denied) {
                    auto const [a, b] = std::mismatch(installed.begin(), installed.end(), denied.begin(), denied.end());
                    return a == installed.end();
                });
                if (!ec && !opens_denied) read_paths.push_back(installed);
            }
        }
        // and those their recipes built, in the cache, which the sandbox reads
        for (auto const& [name, program] : built_toolchains) {
            slice.toolchains.insert_or_assign(name, program);
            append_to_path(slice.toolchain.env, program.parent_path());
        }
        slice.target_fields = profile->target_fields;
        slice.target_tables = profile->target_tables;
        if (!profile->cmake_toolchain.empty()) {
            slice.cmake_toolchain = base / "toolchain.cmake";
            write_if_changed(slice.cmake_toolchain, profile->cmake_toolchain + cmake_flags(slice.toolchain));
        }
        slice.meson_file = base / "meson.ini";
        auto const pkg_config = slice.toolchains.find("pkg-config");
        write_if_changed(slice.meson_file,
            meson_machine_file(slice.toolchain, profile->meson_host, pkg_config == slice.toolchains.end() ? "" : path_to_utf8(pkg_config->second),
                profile->meson_binaries));
        slice.meson_cross = !profile->meson_host.empty();
        if (auto const system = profile->meson_host.find("system"); system != profile->meson_host.end()) slice.meson_system = system->second;
        slice.meson_options = profile->meson_options;
        slice.autotools_host = profile->autotools_host;
        slice.autotools_build = profile->autotools_build;
        slice.b2_toolset = profile->b2_toolset;
        slice.b2_version = profile->b2_version;
        slice.b2_properties = profile->b2_properties;
        slice.launcher = settings_.launcher;
        for (auto const& [name, value] : options) {
            option_schema const* o = rec.find_option(name);
            if (!o || o->type != option_type::file) {
                slice.options.emplace(name, value);
            } else if (auto const copy = option_copies.find(name); copy != option_copies.end()) {
                slice.options.emplace(name, option_data{slashed(copy->second)});
            } else {
                slice.options.emplace(name, option_data{false});
            }
        }
        slice.log = log;
        if (settings_.isolation == build_isolation::sandbox) {
            auto const command = sandbox_command_of(base, slice.recipe, read_paths, lease->server().fifo());
            if (!command) return failed(command.error().message);
            std::shared_ptr<sandbox_launch const> launch;
            if (settings_.sandbox.kind == host_sandbox::mechanism::appcontainer) {
                // the toolchains outside the cache, whose ancestors the DLL hands (decision 111); the cache's are the work
                // directory's
                std::vector<fs::path> reached;
                auto const cache_root = resolved_path(cache_.root());
                for (fs::path const& path : read_paths) {
                    auto const r = resolved_path(path);
                    if (r && !(cache_root && is_within(*r, *cache_root)) && std::ranges::find(reached, *r) == reached.end()) reached.push_back(*r);
                }
                launch = std::make_shared<sandbox_launch const>(
                    sandbox_launch{settings_.sandbox.container, settings_.sandbox.program, base / "sandbox.log", base, std::move(reached)});
            }
            slice.run = [command = *command, launch](process_spec const& s) -> result<process_exit> {
                // looked up here, so a missing program fails as it does outside the sandbox
                auto program = find_program(s.argv.front(), s.env, s.cwd);
                if (!program) return std::unexpected{program.error()};
                process_spec sandboxed = s;
                sandboxed.argv.front() = path_to_utf8(*program);
                sandboxed.argv.insert(sandboxed.argv.begin(), command.begin(), command.end());
                sandboxed.sandbox = launch;
                return run_process(sandboxed);
            };
        } else if (!builds_drive_.empty()) {
            // unisolated, otok-sandbox.dll alone gives the device map to the children of the build's programs
            auto const launch = std::make_shared<sandbox_launch const>(sandbox_launch{"", settings_.sandbox.program, base / "sandbox.log", base});
            slice.run = [launch](process_spec const& s) -> result<process_exit> {
                auto program = find_program(s.argv.front(), s.env, s.cwd);
                if (!program) return std::unexpected{program.error()};
                process_spec with = s;
                with.argv.front() = path_to_utf8(*program);
                with.sandbox = launch;
                return run_process(with);
            };
        }
        // a toolchain that is a script runs under its interpreter, before the sandbox wraps the command (docs/PLAN.md decision 70)
        if (!interpreted.empty()) {
            slice.run = [run = std::move(slice.run), interpreted](process_spec const& s) -> result<process_exit> {
                auto const it = s.argv.empty() ? interpreted.end() : interpreted.find(path_from_utf8(s.argv.front()).lexically_normal());
                if (it == interpreted.end()) return run(s);
                process_spec with = s;
                with.argv.insert(with.argv.begin(), path_to_utf8(it->second));
                return run(with);
            };
        }
        // between two commands the job goes to a slice before this one that wants it; during one, the slice's tools
        // may want more (docs/PLAN.md decision 61)
        slice.run = [run = std::move(slice.run), lease](process_spec const& s) -> result<process_exit> {
            job_boundary();
            lease->set_running(true, s.env.contains("MAKEFLAGS"));
            auto ran = run(s);
            lease->set_running(false);
            return ran;
        };

        slice.stage = show_stage;
        auto built = run_build_script(script, script_label, entry, slice);
        if (!built) return failed(built.error().message);
        show_stage("checks");
        for (std::string const& w : built->warnings) say("warning: " + label + " " + slice_label(spec) + ": " + w);
        // what the target makes of them before anything links against them: Apple's frameworks (otok-toml.md 4.10)
        if (!building_tools) {
            if (auto ok = target->prepare_libraries(p.library, built->libraries, spec); !ok) return failed(ok.error().message);
        }

        // the results (build-lua-api.md 8): their kinds by the linkage, checked by the target too
        std::vector<std::string> leaks;  // as the host writes them and with `/`, as build.lua sees them
        for (fs::path const& dir : {base, work, cache_.root(), virtual_drives::to_virtual(cache_.root())}) {
            leaks.push_back(path_to_utf8(dir));
            leaks.push_back(slashed(dir));
        }
        for (auto const& [name, lib] : built->libraries) {
            std::string const what = "the library '" + name + "' (" + slashed(lib) + ")";
            binary_kind const kind = binary_kind_of(lib);
            auto const runtime = built->runtimes.find(name);
            if (spec.linkage == "static") {
                if (kind != binary_kind::archive) return failed(what + " is not a static archive");
                if (runtime != built->runtimes.end()) return failed("the library '" + name + "' has a runtime, but " + slice_label(spec) + " is built static");
            } else if (runtime != built->runtimes.end()) {
                if (kind != binary_kind::archive) return failed(what + " is " + binary_kind_name(kind) + ", not the import library of a DLL");
                if (binary_kind_of(runtime->second) != binary_kind::pe_dll) return failed("the runtime of '" + name + "' (" + slashed(runtime->second) + ") is not a DLL");
            } else if (kind == binary_kind::pe_dll) {
                return failed(what + " is a DLL: return its import library as link and the DLL as runtime");
            } else if (kind != binary_kind::archive && kind != binary_kind::elf_shared && kind != binary_kind::macho_dylib) {
                return failed(what + " is neither a static archive nor a dynamic library");
            }
            if (kind == binary_kind::elf_shared || kind == binary_kind::macho_dylib) {
                // names fixed at link time must not lead into this build (docs/PLAN.md decision 31)
                load_names const names = load_names_of(lib);
                std::vector<std::string> fixed = names.needed;
                fixed.insert(fixed.end(), names.search.begin(), names.search.end());
                fixed.push_back(names.id);
                for (std::string const& fixed_name : fixed) {
                    if (std::ranges::any_of(leaks, [&](std::string const& dir) { return fixed_name.find(dir) != std::string::npos; })) {
                        return failed(what + " names '" + fixed_name + "' (an install name, RPATH or NEEDED entry), which leads into otok's directories: "
                            + "the consumer's executable must choose where libraries are");
                    }
                }
                if (kind == binary_kind::macho_dylib && !names.id.starts_with("@rpath/")) {
                    return failed(what + " has the install name '" + names.id + "', not '@rpath/…': the consumer's executable must choose where it is");
                }
                // the libraries of the slice it needs: found through its own RUNPATH, since the consumer's does
                // not reach them (docs/PLAN.md decision 42)
                std::string const& rpath = slice.toolchain.rpath;
                if (kind == binary_kind::elf_shared && !rpath.empty()) {
                    bool carried = false;
                    for (std::string const& paths : names.search) {
                        for (std::size_t at = 0; at <= paths.size() && !carried;) {
                            std::size_t const end = std::min(paths.find(':', at), paths.size());
                            carried = paths.substr(at, end - at) == rpath;
                            at = end + 1;
                        }
                    }
                    for (std::string const& needed : names.needed) {
                        if (carried || needed.find('/') != std::string::npos) break;
                        bool const sibling = std::ranges::any_of(built->libraries, [&](auto const& other) {
                            return fs::exists(other.second.parent_path() / path_from_utf8(needed), ec);
                        });
                        if (sibling) {
                            return failed(what + " needs '" + needed + "' of its slice, but its RPATH and RUNPATH lack '" + rpath
                                + "', by which it would find it beside itself; the build must set it (slice.toolchain.rpath)");
                        }
                    }
                }
            }
            if (auto ok = target->check_library(lib, spec); !ok) return failed("the library '" + name + "': " + ok.error().message);
        }
        // every library of the products that hold (recipe-toml.md 16): one missing would be missing in the consumer's link
        if (!building_tools) {
            for (product const& prod : rec.products) {
                if (prod.when && !condition_holds(*prod.when, options, os, spec.platform)) continue;
                for (std::string const& name : prod.libraries) {
                    if (!built->libraries.contains(name)) {
                        return failed("build returned no library '" + name + "', which the product '" + prod.name + "' holds");
                    }
                }
            }
        }

        for (auto const& [name, file] : built->data) {
            if (!fs::is_regular_file(file, ec)) return failed("the data file '" + name + "' (" + slashed(file) + ") is not a regular file");
        }
        // the programs of [tool], and only those (recipe-toml.md 3.1)
        if (!building_tools) {
            for (auto const& [name, program] : built->programs) {
                if (std::ranges::find(rec.programs, name) == rec.programs.end()) {
                    return failed("build returned the program '" + name + "', which [tool] programs does not name");
                }
            }
            
            // all of them in the slices of a tool package; a library that is a tool too builds them there only (recipe-toml.md 13.2)
            if (tool) {
                for (std::string const &name : rec.programs) {
                    if (!built->programs.contains(name))
                        return failed("build returned no program '" + name + "' of [tool] programs");
                }
            }

            // a script's interpreter: a toolchain of this recipe, which the slices of its users find as its build did
            for (auto const& [name, interpreter] : built->interpreters) {
                if (!slice.toolchains.contains(interpreter)) {
                    return failed("the program '" + name + "' runs under '" + interpreter + "', which is not a toolchain of this build ([toolchains])");
                }
            }
        }
        // what its dynamic libraries and programs load: the slice's, a dependency's or the system's, never a library of the
        // machine (build-lua-api.md 8, docs/PLAN.md decision 80)
        if (!building_tools) {
            std::optional<std::set<std::string>> known;  // the file names of the slice's prefix and of the dependencies' library directories
            auto const known_names = [&]() -> std::set<std::string> const& {
                if (known) return *known;
                known.emplace();
                auto const add_dir = [&](fs::path const& dir, bool deep) {
                    std::error_code walk_ec;
                    if (deep) {
                        for (auto it = fs::recursive_directory_iterator{dir, walk_ec}; !walk_ec && it != fs::recursive_directory_iterator{}; it.increment(walk_ec)) {
                            known->insert(path_to_utf8(it->path().filename()));
                        }
                    } else {
                        for (auto it = fs::directory_iterator{dir, walk_ec}; !walk_ec && it != fs::directory_iterator{}; it.increment(walk_ec)) {
                            known->insert(path_to_utf8(it->path().filename()));
                        }
                    }
                };
                // a dynamic library is known by its install name too, which the target may have given it (otok-toml.md 4.10)
                auto const add_install_name = [&](fs::path const& lib) {
                    if (binary_kind_of(lib) != binary_kind::macho_dylib) return;
                    std::string const id = load_names_of(lib).id;
                    if (!id.empty()) known->insert(id.substr(id.rfind('/') + 1));
                };
                add_dir(slice.prefix, true);
                for (auto const& [name, lib] : built->libraries) add_install_name(lib);
                // the dependencies, direct or not, built before this slice
                std::vector<std::string> pending = p.dependencies;
                std::set<std::string> visited;
                while (!pending.empty()) {
                    std::string const library = std::move(pending.back());
                    pending.pop_back();
                    if (!visited.insert(library).second) continue;
                    resolved_package const* dp = tool ? r_.find_tool(library) : r_.find(library, p.target);
                    built_slice const* dep = find_built(dp, tool);
                    if (!dp || !dep) continue;
                    for (auto const& [name, lib] : dep->output.libraries) {
                        add_dir(lib.parent_path(), false);
                        add_install_name(lib);
                    }
                    for (auto const& [name, runtime] : dep->output.runtimes) add_dir(runtime.parent_path(), false);
                    pending.insert(pending.end(), dp->dependencies.begin(), dp->dependencies.end());
                }
                return *known;
            };
            // a library of the products' `link` (recipe-toml.md 11): `z` is libz.so.1 or libz.1.dylib
            auto const linked = [&](std::string const& file) {
                return std::ranges::any_of(rec.products, [&](product const& prod) {
                    return std::ranges::any_of(prod.link_libraries, [&](product_link const& l) { return file.starts_with("lib" + l.library + "."); });
                });
            };
            std::vector<std::pair<std::string, fs::path>> loading;
            for (auto const& [name, lib] : built->libraries) loading.emplace_back("the library '" + name + "'", lib);
            for (auto const& [name, program] : built->programs) loading.emplace_back("the program '" + name + "'", program);
            for (auto const& [what, file] : loading) {
                for (std::string const& loaded : load_names_of(file).needed) {
                    if (target->system_library(loaded, spec)) continue;
                    // found by its name: without a directory, or in a directory the loader chooses (Mach-O)
                    std::string name;
                    if (loaded.find('/') == std::string::npos) name = loaded;
                    else if (loaded.starts_with("@rpath/") || loaded.starts_with("@loader_path/") || loaded.starts_with("@executable_path/")) name = loaded.substr(loaded.rfind('/') + 1);
                    if (!name.empty() && (known_names().contains(name) || linked(name))) continue;
                    return failed(what + " (" + slashed(file) + ") loads '" + loaded + "', which is neither its slice's, nor a dependency's, nor the system's: "
                        + "a library of the build machine, or of a dependency the recipe does not declare");
                }
            }
        }

        relocate_pkg_config(slice.prefix);

        // before the prefix moves: an entry whose result.toml would be damaged is never made (cache.md 6.2)
        auto const result_text = detail::write_result(*built, slice.prefix);
        if (!result_text) return failed(result_text.error().message);
        if (dev) {
            // the entry is the work directory, last used now (cache.md 9)
            std::ofstream{base / "result.toml", std::ios::binary} << *result_text;
            std::ofstream{base / "key.txt", std::ios::binary} << text;
            follow.reset();
            fs::last_write_time(work, fs::file_time_type::clock::now(), ec);
            return {};
        }
        fs::create_directories(long_path(staging), ec);
        rename_path(virtual_drives::to_real(slice.prefix), staging / "prefix", ec);
        if (ec) return failed("cannot move the prefix into the cache: " + ec.message());
        std::ofstream{long_path(staging / "result.toml"), std::ios::binary} << *result_text;
        std::ofstream{long_path(staging / "key.txt"), std::ios::binary} << text;
        follow.reset();
        fs::copy_file(long_path(log), long_path(staging / "build.log"), ec);
        fs::remove_all(long_path(work), ec);
        return {};
    };
    result<void> made;
    if (dev) {
        // one otok at a time in the work directory
        fs::create_directories(work.parent_path(), ec);
        auto held = file_lock::acquire(path_from_utf8(path_to_utf8(work) + ".lock"),
            [&] { say("note: waiting for another otok building " + label + " for " + slice_label(spec)); });
        made = held ? build({}) : result<void>{std::unexpected{held.error()}};
    } else {
        if (auto ensured = cache_.ensure(out.entry, "building " + label + " for " + slice_label(spec), build); !ensured) made = std::unexpected{ensured.error()};
    }
    if (!made) return std::unexpected{made.error()};
    if (dev) {
        // dependents see it by its output (cache-key.md 3)
        auto hash = tree_hash(out.entry / "prefix", tree_links::record);
        if (!hash) return fail(label + " for " + slice_label(spec) + ": cannot hash its output: " + hash.error().message);
        out.key = "dev:" + *hash;
    }
    auto result = read_result(out.entry);
    if (!result) return std::unexpected{result.error()};
    out.output = std::move(*result);
    std::chrono::duration<double> const took = std::chrono::steady_clock::now() - started;
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.1f", took.count());
    if (shown.display) shown.display->end(shown.id);  // off the display before its line
    say("built " + label + " for " + slice_label(spec) + (dev ? " (dev: " + inputs.dev->label + ")" : std::string{}) + " (" + seconds + " s)"
        + slice_done(p.id, tool, entry));
    return out;
}

}
