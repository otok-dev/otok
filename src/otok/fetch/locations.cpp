// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/fetch/locations.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <variant>

#include "otok/core/names.hpp"
#include "otok/core/toml_reader.hpp"

namespace otok {

namespace {

void add(std::vector<std::string>& out, std::string location)
{
    if (std::ranges::find(out, location) == out.end()) out.push_back(std::move(location));
}

// A pattern of config.md 2.2: literal text and placeholders.
struct piece
{
    std::string text;           // the literal, or the placeholder's name
    bool placeholder = false;
    bool slashes = false;       // {name*}
};

bool is_placeholder_name(std::string_view s)
{
    if (s.empty() || s.front() < 'a' || s.front() > 'z') return false;
    return std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// The pieces of a pattern, or what is wrong with it.
std::variant<std::vector<piece>, std::string> parse_pattern(std::string_view p, bool stars)
{
    std::vector<piece> out;
    std::size_t at = 0;
    while (at < p.size()) {
        std::size_t const open = p.find_first_of("{}", at);
        if (open == std::string_view::npos) {
            out.push_back({std::string{p.substr(at)}});
            break;
        }
        if (p[open] == '}') return "has '}' without '{'";
        if (open > at) out.push_back({std::string{p.substr(at, open - at)}});
        std::size_t const close = p.find_first_of("{}", open + 1);
        if (close == std::string_view::npos || p[close] == '{') return "has '{' without '}'";
        std::string name{p.substr(open + 1, close - open - 1)};
        bool const slashes = stars && name.ends_with('*');
        if (slashes) name.pop_back();
        if (!is_placeholder_name(name)) return "has the placeholder '" + std::string{p.substr(open, close - open + 1)} + "'; names are [a-z][a-z0-9_]*";
        if (!out.empty() && out.back().placeholder) return "has two placeholders without text between them";
        out.push_back({std::move(name), true, slashes});
        at = close + 1;
    }
    return out;
}

// Matches `pieces` at `pos` of `url`, each placeholder as long as the rest allows; sets `end`.
bool match(std::span<piece const> pieces, std::string_view url, std::size_t pos, bool exact, std::map<std::string, std::string>& found,
    std::size_t& end)
{
    if (pieces.empty()) {
        if (exact && pos != url.size()) return false;
        end = pos;
        return true;
    }
    piece const& p = pieces.front();
    if (!p.placeholder) return url.substr(pos).starts_with(p.text) && match(pieces.subspan(1), url, pos + p.text.size(), exact, found, end);
    std::size_t const limit = p.slashes ? url.size() : std::min(url.find('/', pos), url.size());
    for (std::size_t last = limit; last > pos; --last) {
        found[p.text] = std::string{url.substr(pos, last - pos)};
        if (match(pieces.subspan(1), url, last, exact, found, end)) return true;
    }
    found.erase(p.text);
    return false;
}

// The locations one level adds for an archive.
void add_archive(std::vector<std::string>& out, input_mirrors const& level, std::string const& sha256, std::span<std::string const> own)
{
    for (std::string const& t : level.asset_mirrors) {
        std::string location = t;
        for (std::size_t at; (at = location.find("{sha256}")) != std::string::npos;) location.replace(at, 8, sha256);
        add(out, std::move(location));
    }
    for (std::string const& u : own) {
        if (auto rewritten = rewrite_url(u, level.rewrites, sha256)) add(out, std::move(*rewritten));
    }
}

}

bool input_mirrors::only_for(std::string_view library) const
{
    return mirrors_only || (!library.empty() && std::ranges::find(mirrors_only_libraries, library) != mirrors_only_libraries.end());
}

std::optional<std::string> rewrite_url(std::string const& url, std::span<url_rewrite const> rewrites, std::string_view sha256)
{
    for (url_rewrite const& r : rewrites) {
        auto from = parse_pattern(r.from, true);
        auto to = parse_pattern(r.to, false);
        if (!std::holds_alternative<std::vector<piece>>(from) || !std::holds_alternative<std::vector<piece>>(to)) continue;  // checked when read
        std::map<std::string, std::string> found;
        std::size_t end = 0;
        if (!match(std::get<std::vector<piece>>(from), url, 0, r.exact, found, end)) continue;
        std::string out;
        bool applies = true;
        for (piece const& p : std::get<std::vector<piece>>(to)) {
            if (!p.placeholder) {
                out += p.text;
            } else if (auto it = found.find(p.text); it != found.end()) {
                out += it->second;
            } else if (p.text == "sha256" && !sha256.empty()) {
                out += sha256;
            } else {
                applies = false;  // {sha256} for a git URL
            }
        }
        if (applies) return out + url.substr(end);
    }
    return std::nullopt;
}

std::optional<std::string> url_rewrite_problem(url_rewrite const& rule)
{
    auto from = parse_pattern(rule.from, true);
    if (auto const* problem = std::get_if<std::string>(&from)) return "'from' " + *problem;
    auto to = parse_pattern(rule.to, false);
    if (auto const* problem = std::get_if<std::string>(&to)) return "'to' " + *problem;
    std::set<std::string> names;
    for (piece const& p : std::get<std::vector<piece>>(from)) {
        if (!p.placeholder) continue;
        if (p.text == "sha256") return std::string{"'from' may not use {sha256}, which only 'to' can use"};
        if (!names.insert(p.text).second) return "'from' has the placeholder {" + p.text + "} twice";
    }
    for (piece const& p : std::get<std::vector<piece>>(to)) {
        if (p.placeholder && p.text != "sha256" && !names.contains(p.text)) return "'to' uses {" + p.text + "}, which 'from' does not have";
    }
    return std::nullopt;
}

std::vector<std::string> archive_locations(std::string const& sha256, std::string const& url, std::span<std::string const> mirrors,
    fetch_locations const& where, std::string_view library)
{
    std::vector<std::string> own{url};
    own.insert(own.end(), mirrors.begin(), mirrors.end());
    std::vector<std::string> out;
    // the machine's mirrors-only leaves the machine alone: a project cannot lead it elsewhere
    bool const machine_only = where.machine.only_for(library);
    if (!machine_only) add_archive(out, where.project, sha256, own);
    add_archive(out, where.machine, sha256, own);
    if (!machine_only && !where.project.only_for(library)) {
        for (std::string const& u : own) add(out, u);
    }
    return out;
}

std::vector<std::string> git_locations(std::string const& url, fetch_locations const& where, std::string_view library)
{
    std::vector<std::string> out;
    bool const machine_only = where.machine.only_for(library);
    if (!machine_only) {
        if (auto rewritten = rewrite_url(url, where.project.rewrites)) add(out, std::move(*rewritten));
    }
    if (auto rewritten = rewrite_url(url, where.machine.rewrites)) add(out, std::move(*rewritten));
    if (!machine_only && !where.project.only_for(library)) add(out, url);
    return out;
}

std::optional<std::string> asset_mirror_problem(std::string_view t)
{
    if (!t.starts_with("https://") && !t.starts_with("http://") && !t.starts_with("file://")) {
        return "must hold https, http or file URLs, not '" + std::string{t} + "'";
    }
    if (t.find("{sha256}") == std::string_view::npos) return "must hold URL templates containing {sha256}, not '" + std::string{t} + "'";
    return std::nullopt;
}

void read_input_mirrors(toml_table& t, input_mirrors& out, severity unknown_keys)
{
    // one template or several
    if (toml::node const* node = t.contains("asset-mirror") ? t.node("asset-mirror") : nullptr) {
        if (node->is_string()) {
            out.asset_mirrors = {node->as_string()->get()};
        } else if (auto list = t.strings("asset-mirror")) {
            out.asset_mirrors = std::move(*list);
        }
        for (std::string const& m : out.asset_mirrors) {
            if (auto problem = asset_mirror_problem(m)) t.error(t.position("asset-mirror"), t.describe("asset-mirror") + " " + *problem);
        }
    }
    if (auto rules = t.tables("url-rewrite")) {
        for (toml_table& r : *rules) {
            url_rewrite rule;
            rule.from = r.string("from", need::required).value_or("");
            rule.to = r.string("to", need::required).value_or("");
            rule.exact = r.boolean("exact").value_or(false);
            if (r.contains("from") && rule.from.empty()) {
                r.error(r.position("from"), r.describe("from") + " must not be empty");
            } else if (auto problem = url_rewrite_problem(rule)) {
                r.error(r.position(), r.path() + ": " + *problem);
            }
            r.finish(unknown_keys);
            out.rewrites.push_back(std::move(rule));
        }
    }
    // true, or the libraries it holds for
    if (toml::node const* node = t.contains("mirrors-only") ? t.node("mirrors-only") : nullptr) {
        if (node->is_boolean()) {
            out.mirrors_only = node->as_boolean()->get();
        } else if (auto list = t.strings("mirrors-only")) {
            for (std::size_t i = 0; i < list->size(); ++i) {
                std::string const& l = (*list)[i];
                if (!is_library_name(l)) {
                    t.error(t.position("mirrors-only"), t.describe("mirrors-only") + " holds " + std::string{"'"} + l + "', which is not a library name");
                } else if (std::find(list->begin(), list->begin() + static_cast<std::ptrdiff_t>(i), l) != list->begin() + static_cast<std::ptrdiff_t>(i)) {
                    t.error(t.position("mirrors-only"), t.describe("mirrors-only") + " lists '" + l + "' twice");
                }
            }
            out.mirrors_only_libraries = std::move(*list);
        }
    }
}

}
