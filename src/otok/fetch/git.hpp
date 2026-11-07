// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"
#include "otok/core/process.hpp"

// The system `git` (docs/PLAN.md decision 14), run for the user: with their environment, so their
// credentials, SSH agent and proxies apply.
namespace otok {

struct git_tool
{
    std::filesystem::path program;
    env_vars env;
};

// The program `configured` (`git` of config.toml, config.md 2), or `git` of the PATH of `env`.
result<git_tool> find_git(env_vars env, std::string const& configured = {});

// Runs `git <args>` in `cwd`; its output goes to `log`. With `capture`, standard output is returned.
// A failure carries the command and the end of the log.
result<std::string> run_git(git_tool const& git, std::vector<std::string> const& args, std::filesystem::path const& cwd,
    std::filesystem::path const& log, bool capture = false);

// The variables that give git `token` for the repository `url` alone: http.<url>.extraHeader with HTTP Basic
// authentication as x-access-token, in GIT_CONFIG_* after those `env` has, so that no command line or message shows it.
// For a private index on GitHub read where git has no credentials of its own (a Mac reached over SSH, whose keychain git
// cannot open). Nothing without a token.
env_vars git_token_env(std::string const& url, std::string const& token, env_vars const& env);

// The key of a repository URL in the cache (docs/formats/cache.md 4): the first 32 hex digits of the
// SHA-256 of the URL with scheme and host lower-cased and a trailing `/` and `.git` removed.
std::string git_url_key(std::string_view url);

// A submodule URL from `.gitmodules`: absolute as it is, `./x` and `../x` relative to the
// superproject's URL, as git resolves them.
std::string resolve_submodule_url(std::string_view superproject, std::string_view url);

}
