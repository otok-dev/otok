// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/fetch/git.hpp"

#include <cctype>
#include <cstdlib>

#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"

namespace otok {

namespace {

// The last `count` lines of a log, for error messages.
std::string log_tail(std::filesystem::path const& log, std::size_t count)
{
    std::string const text = read_file(log).value_or("");
    std::size_t start = text.size();
    for (std::size_t lines = 0; start > 0 && lines <= count; --start) {
        if (text[start - 1] == '\n') ++lines;
    }
    std::string tail = text.substr(start);
    while (!tail.empty() && (tail.front() == '\n')) tail.erase(0, 1);
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
    return tail;
}

std::string strip_url_end(std::string_view url)
{
    std::string s{url};
    while (s.ends_with('/')) s.pop_back();
    if (s.ends_with(".git")) s.resize(s.size() - 4);
    while (s.ends_with('/')) s.pop_back();
    return s;
}

}

result<git_tool> find_git(env_vars env, std::string const& configured)
{
    std::error_code ec;
    std::filesystem::path const cwd = std::filesystem::current_path(ec);
    // `git` of config.toml (config.md 2), else PATH; it runs with otok's environment either way
    if (!configured.empty()) {
        std::filesystem::path program = path_from_utf8(configured);
        if (!std::filesystem::is_regular_file(program, ec)) return fail("'git' of config.toml is '" + configured + "', which is not a file");
        return git_tool{std::move(program), std::move(env)};
    }
    auto program = find_program("git", env, cwd);
    if (!program) return fail("otok needs git for git sources and recipes, and it is not in PATH; set 'git' in config.toml (or OTOK_GIT) to the program");
    return git_tool{std::move(*program), std::move(env)};
}

env_vars git_token_env(std::string const& url, std::string const& token, env_vars const& env)
{
    if (token.empty()) return {};
    static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string const plain = "x-access-token:" + token;
    std::string encoded;
    for (std::size_t i = 0; i < plain.size(); i += 3) {
        std::size_t const left = plain.size() - i;
        unsigned const n = static_cast<unsigned>(static_cast<unsigned char>(plain[i])) << 16
            | (left > 1 ? static_cast<unsigned>(static_cast<unsigned char>(plain[i + 1])) << 8 : 0u)
            | (left > 2 ? static_cast<unsigned>(static_cast<unsigned char>(plain[i + 2])) : 0u);
        encoded += digits[n >> 18 & 63];
        encoded += digits[n >> 12 & 63];
        encoded += left > 1 ? digits[n >> 6 & 63] : '=';
        encoded += left > 2 ? digits[n & 63] : '=';
    }
    std::size_t index = 0;
    if (auto it = env.find("GIT_CONFIG_COUNT"); it != env.end()) index = std::strtoul(it->second.c_str(), nullptr, 10);
    std::string const n = std::to_string(index);
    return {{"GIT_CONFIG_COUNT", std::to_string(index + 1)}, {"GIT_CONFIG_KEY_" + n, "http." + url + ".extraHeader"},
        {"GIT_CONFIG_VALUE_" + n, "Authorization: Basic " + encoded}};
}

result<std::string> run_git(git_tool const& git, std::vector<std::string> const& args, std::filesystem::path const& cwd,
    std::filesystem::path const& log, bool capture)
{
    process_spec spec;
    spec.argv.push_back(path_to_utf8(git.program));
    spec.argv.insert(spec.argv.end(), args.begin(), args.end());
    spec.env = git.env;
    spec.env["GIT_TERMINAL_PROMPT"] = "0";  // fail instead of waiting for a password nobody types
    spec.cwd = cwd;
    spec.log = log;
    spec.capture = capture;
    spec.capture_limit = 16 * 1024 * 1024;

    std::string command = "git";
    for (std::string const& a : args) command += " " + a;
    auto e = run_process(spec);
    if (!e) return fail("cannot run '" + command + "': " + e.error().message);
    if (!e->success()) {
        std::string message = "'" + command + "' failed (" + describe_exit(*e) + ")";
        if (std::string const tail = log_tail(log, 10); !tail.empty()) message += ":\n" + tail;
        return fail(std::move(message));
    }
    return std::move(e->output);
}

std::string git_url_key(std::string_view url)
{
    std::string normalized = strip_url_end(url);
    std::size_t const scheme_end = normalized.find("://");
    std::size_t const host_end = scheme_end == std::string::npos ? 0 : normalized.find('/', scheme_end + 3);
    std::size_t const lower_end = scheme_end == std::string::npos ? 0 : (host_end == std::string::npos ? normalized.size() : host_end);
    for (std::size_t i = 0; i < lower_end; ++i) normalized[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(normalized[i])));
    return sha256_hex(normalized).substr(0, 32);
}

std::string resolve_submodule_url(std::string_view superproject, std::string_view url)
{
    if (!url.starts_with("./") && !url.starts_with("../")) return std::string{url};
    std::string base{superproject};
    while (base.ends_with('/')) base.pop_back();
    while (true) {
        if (url.starts_with("./")) {
            url.remove_prefix(2);
        } else if (url.starts_with("../")) {
            url.remove_prefix(3);
            std::size_t const slash = base.rfind('/');
            if (slash != std::string::npos && base.find("://") + 2 != slash) base.resize(slash);
        } else {
            break;
        }
    }
    return base + "/" + std::string{url};
}

}
