// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/publish/bump.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <toml++/toml.hpp>

#include "otok/core/fs.hpp"
#include "otok/core/tree_hash.hpp"
#include "otok/version/version.hpp"

namespace otok {

namespace {

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

std::vector<std::string> split_parts(std::string_view v)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        std::size_t const dot = v.find('.', start);
        parts.emplace_back(v.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts;
}

std::string join_parts(std::span<std::string const> parts, char separator)
{
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) text += separator;
        text += parts[i];
    }
    return text;
}

// Whether the `n` characters of `text` at `at` stand alone as a version (version_spellings).
bool stands_alone(std::string_view text, std::size_t at, std::size_t n, char separator) noexcept
{
    if (at > 0) {
        char const c = text[at - 1];
        if (is_digit(c)) return false;
        if (separator != 0 && c == separator && at > 1 && is_digit(text[at - 2])) return false;
    }
    std::size_t const end = at + n;
    if (end < text.size()) {
        char const c = text[end];
        if (is_digit(c)) return false;
        if (separator != 0 && c == separator && end + 1 < text.size() && is_digit(text[end + 1])) return false;
    }
    return true;
}

// The spelling standing alone at `at`, the first of `spellings` that does; none.
version_spelling const* spelling_at(std::string_view text, std::size_t at, std::span<version_spelling const> spellings) noexcept
{
    for (version_spelling const& s : spellings) {
        if (text.substr(at, s.from.size()) == s.from && stands_alone(text, at, s.from.size(), s.separator)) return &s;
    }
    return nullptr;
}

version_spelling const* first_mention(std::string_view text, std::span<version_spelling const> spellings) noexcept
{
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (auto const* s = spelling_at(text, i, spellings)) return s;
    }
    return nullptr;
}

// The byte offset of a toml++ position, whose columns count code points.
std::optional<std::size_t> offset_of(std::string_view content, toml::source_position const& p)
{
    if (p.line == 0 || p.column == 0) return std::nullopt;
    std::size_t at = 0;
    for (toml::source_index line = 1; line < p.line; ++line) {
        std::size_t const nl = content.find('\n', at);
        if (nl == std::string_view::npos) return std::nullopt;
        at = nl + 1;
    }
    for (toml::source_index column = 1; column < p.column; ++column) {
        if (at >= content.size() || content[at] == '\n') return std::nullopt;
        ++at;
        while (at < content.size() && (static_cast<unsigned char>(content[at]) & 0xC0) == 0x80) ++at;
    }
    return at;
}

// A replacement of the text of one value.
struct text_edit
{
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string text;
};

// The text of the value `n` in `content`: a one-line string with its quotes, or an integer.
std::optional<std::pair<std::size_t, std::size_t>> value_span(std::string_view content, toml::node const& n)
{
    auto const at = offset_of(content, n.source().begin);
    if (!at || *at >= content.size()) return std::nullopt;
    char const q = content[*at];
    if (q == '"' || q == '\'') {
        if (content.substr(*at, 3) == std::string(3, q)) return std::nullopt;  // multi-line: not edited
        for (std::size_t i = *at + 1; i < content.size(); ++i) {
            char const c = content[i];
            if (c == '\n' || c == '\r') return std::nullopt;
            if (q == '"' && c == '\\') {
                ++i;
                continue;
            }
            if (c == q) return std::pair{*at, i + 1 - *at};
        }
        return std::nullopt;
    }
    std::size_t end = *at;
    while (end < content.size() && (is_digit(content[end]) || content[end] == '_' || content[end] == '+')) ++end;
    if (end == *at) return std::nullopt;
    return std::pair{*at, end - *at};
}

// `value` as a TOML string, in the quotes of the one it replaces when it can be.
std::string toml_string(std::string_view value, char quote)
{
    bool const plain = std::ranges::none_of(value, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
    if (quote == '\'' && plain && value.find('\'') == std::string_view::npos) return "'" + std::string{value} + "'";
    std::string text = "\"";
    for (char const c : value) {
        if (c == '"' || c == '\\') {
            text += '\\';
            text += c;
        } else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
            text += buffer;
        } else {
            text += c;
        }
    }
    return text + "\"";
}

source_position position_of(std::string const& file, toml::node const& n)
{
    return {file, n.source().begin.line, n.source().begin.column};
}

std::string string_of(toml::node const* n)
{
    if (!n) return {};
    return n->value_or(std::string{});
}

class bumper
{
public:
    bumper(std::string_view content, std::string const& file, std::vector<version_spelling> spellings, std::string old_version,
        std::string new_version, bump_fetchers const& fetchers, diagnostics& diags)
        : content_{content}, file_{file}, spellings_{std::move(spellings)}, old_{std::move(old_version)}, new_{std::move(new_version)},
          fetchers_{fetchers}, diags_{diags}
    {
    }

    // Replaces the string `n` by `value`; false (an error said) when its text cannot be edited.
    bool edit_string(toml::node const& n, std::string const& value, std::string const& what)
    {
        auto const span = value_span(content_, n);
        if (!span) {
            diags_.error(position_of(file_, n), what + " is not written on one line as a quoted string or an integer; bump it by hand");
            return false;
        }
        char const quote = content_[span->first];
        edits_.push_back({span->first, span->second, quote == '"' || quote == '\'' ? toml_string(value, quote) : value});
        return true;
    }

    bump_input archive(std::string const& name, std::string const& label, toml::table const& t)
    {
        bump_input in{.name = name};
        in.before.url = string_of(t.get("url"));
        in.before.sha256 = string_of(t.get("sha256"));
        toml::array const* mirrors = t.get_as<toml::array>("mirrors");
        if (mirrors) {
            for (toml::node const& m : *mirrors) in.before.mirrors.push_back(string_of(&m));
        }
        in.after = in.before;
        toml::node const* url_node = t.get("url");
        auto const url = replace_version(in.before.url, spellings_);
        if (!url) {
            if (mentions_version_digits(in.before.url, old_)) {
                in.status = bump_status::not_bumped;
                in.message = "its URL holds the version " + old_ + " in a way otok does not replace: " + in.before.url;
            }
            return in;
        }
        in.after.url = *url;
        auto const sha = fetchers_.archive(*url);
        if (!sha) {
            in.status = bump_status::failed;
            in.message = "cannot fetch " + *url + ": " + sha.error().message;
            diags_.error(position_of(file_, *url_node), label + " of version " + new_ + ": " + in.message);
            return in;
        }
        in.after.sha256 = *sha;
        in.status = bump_status::changed;
        bool editable = edit_string(*url_node, *url, label + "'s 'url'");
        if (toml::node const* s = t.get("sha256")) editable = edit_string(*s, *sha, label + "'s 'sha256'") && editable;
        if (mirrors) {
            in.after.mirrors.clear();
            for (toml::node const& m : *mirrors) {
                std::string const before = string_of(&m);
                std::string const mirror = replace_version(before, spellings_).value_or(before);
                in.after.mirrors.push_back(mirror);
                auto const got = fetchers_.mirror(mirror);
                if (!got) {
                    diags_.warning(position_of(file_, m), "the mirror " + mirror + " of " + label + " does not give the file of version " + new_
                        + " (" + got.error().message + "); it is kept, tried after the URL");
                } else if (*got != *sha) {
                    in.status = bump_status::failed;
                    in.message = "the mirror " + mirror + " gives another file than " + *url + " (sha256 " + *got + ", not " + *sha + ")";
                    diags_.error(position_of(file_, m), label + " of version " + new_ + ": " + in.message);
                }
                if (mirror != before) editable = edit_string(m, mirror, label + "'s mirror") && editable;
            }
        }
        if (!editable && in.status != bump_status::failed) {
            in.status = bump_status::failed;
            in.message = "its values in recipe.toml cannot be edited";
        }
        return in;
    }

    bump_input git(std::string const& name, std::string const& label, toml::table const& t)
    {
        bump_input in{.name = name, .git = true};
        in.before.git = string_of(t.get("git"));
        in.before.commit = string_of(t.get("commit"));
        in.before.tag = string_of(t.get("tag"));
        in.after = in.before;
        if (in.before.tag.empty()) {
            if (!in.before.commit.empty()) {
                in.status = bump_status::not_bumped;
                in.message = "a git commit without a tag: otok does not know the commit of version " + new_;
            }
            return in;
        }
        auto const tag = replace_version(in.before.tag, spellings_);
        if (!tag) {
            if (mentions_version_digits(in.before.tag, old_)) {
                in.status = bump_status::not_bumped;
                in.message = "its tag holds the version " + old_ + " in a way otok does not replace: " + in.before.tag;
            }
            return in;
        }
        in.after.tag = *tag;
        toml::node const& tag_node = *t.get("tag");
        auto const commit = fetchers_.tag_commit(in.before.git, *tag);
        if (!commit) {
            in.status = bump_status::failed;
            in.message = "cannot find the tag " + *tag + " in " + in.before.git + ": " + commit.error().message;
            diags_.error(position_of(file_, tag_node), label + " of version " + new_ + ": " + in.message);
            return in;
        }
        in.status = bump_status::changed;
        bool editable = edit_string(tag_node, *tag, label + "'s 'tag'");
        // verify-against with a tag has no commit: the tag found is all it needs
        if (toml::node const* c = t.get("commit")) {
            in.after.commit = *commit;
            editable = edit_string(*c, *commit, label + "'s 'commit'") && editable;
        }
        if (!editable) {
            in.status = bump_status::failed;
            in.message = "its values in recipe.toml cannot be edited";
        }
        return in;
    }

    bump_input input(std::string const& name, std::string const& label, toml::table const& t)
    {
        return t.contains("git") ? git(name, label, t) : archive(name, label, t);
    }

    std::string apply() const
    {
        std::vector<text_edit> edits = edits_;
        std::ranges::sort(edits, [](text_edit const& a, text_edit const& b) { return a.offset > b.offset; });
        std::string text{content_};
        for (text_edit const& e : edits) text.replace(e.offset, e.length, e.text);
        return text;
    }

    void add_edit(text_edit e) { edits_.push_back(std::move(e)); }

private:
    std::string_view content_;
    std::string const& file_;
    std::vector<version_spelling> spellings_;
    std::string old_;
    std::string new_;
    bump_fetchers const& fetchers_;
    diagnostics& diags_;
    std::vector<text_edit> edits_;
};

}

std::vector<version_spelling> version_spellings(std::string_view from, std::string_view to)
{
    std::vector<version_spelling> spellings;
    auto const add = [&](std::string f, std::string t, char separator) {
        if (f.empty() || f == t) return;
        if (std::ranges::any_of(spellings, [&](version_spelling const& s) { return s.from == f; })) return;
        spellings.push_back({std::move(f), std::move(t), separator});
    };
    std::vector<std::string> const old_parts = split_parts(from);
    std::vector<std::string> const new_parts = split_parts(to);
    if (old_parts.size() <= 1) {
        add(std::string{from}, std::string{to}, 0);
        return spellings;
    }
    for (char const separator : {'.', '_', '-'}) add(join_parts(old_parts, separator), join_parts(new_parts, separator), separator);
    if (old_parts.size() >= 3 && new_parts.size() >= 2) {
        std::span<std::string const> const old_line{old_parts.data(), 2};
        std::span<std::string const> const new_line{new_parts.data(), 2};
        for (char const separator : {'.', '_'}) add(join_parts(old_line, separator), join_parts(new_line, separator), separator);
    }
    return spellings;
}

std::optional<std::string> replace_version(std::string_view text, std::span<version_spelling const> spellings)
{
    std::string out;
    bool found = false;
    for (std::size_t i = 0; i < text.size();) {
        if (auto const* s = spelling_at(text, i, spellings)) {
            out += s->to;
            i += s->from.size();
            found = true;
        } else {
            out += text[i++];
        }
    }
    if (!found) return std::nullopt;
    return out;
}

bool mentions_version(std::string_view text, std::span<version_spelling const> spellings)
{
    return first_mention(text, spellings) != nullptr;
}

bool mentions_version_digits(std::string_view text, std::string_view version)
{
    std::vector<std::string> const parts = split_parts(version);
    if (parts.size() < 2) return false;
    std::string digits;
    for (std::string const& p : parts) {
        if (p.empty() || !std::ranges::all_of(p, is_digit)) return false;
        digits += p;
    }
    for (std::size_t at = text.find(digits); at != std::string_view::npos; at = text.find(digits, at + 1)) {
        if (stands_alone(text, at, digits.size(), 0)) return true;
    }
    return false;
}

std::string_view bump_status_name(bump_status s) noexcept
{
    switch (s) {
    case bump_status::changed: return "changed";
    case bump_status::unchanged: return "unchanged";
    case bump_status::not_bumped: return "not-bumped";
    case bump_status::failed: return "failed";
    }
    return "failed";
}

std::optional<bump_result> bump_recipe_text(std::string_view content, std::string const& file, std::string_view new_version,
    bump_fetchers const& fetchers, diagnostics& diags)
{
    toml::table doc;
    try {
        doc = toml::parse(content, file);
    } catch (toml::parse_error const& e) {
        diags.error({file, e.source().begin.line, e.source().begin.column}, std::string{e.description()});
        return std::nullopt;
    }
    toml::table const* recipe_table = doc.get_as<toml::table>("recipe");
    toml::node const* version_node = recipe_table ? recipe_table->get("version") : nullptr;
    toml::node const* revision_node = recipe_table ? recipe_table->get("revision") : nullptr;
    if (!version_node || !version_node->is_string() || !revision_node || !revision_node->is_integer()) {
        diags.error({file, 0, 0}, "the recipe has no [recipe] version and revision to bump");
        return std::nullopt;
    }
    bump_result r;
    r.old_version = string_of(version_node);
    r.old_revision = revision_node->value_or(std::int64_t{0});
    r.new_version = std::string{new_version};
    if (auto parsed = version::parse(new_version); !parsed) {
        diags.error({}, "'" + std::string{new_version} + "' is not a version: " + parsed.error().message);
        return std::nullopt;
    }
    if (r.old_version == new_version) {
        diags.error(position_of(file, *version_node), "the recipe is at version " + r.old_version + " already");
        return std::nullopt;
    }

    bumper b{content, file, version_spellings(r.old_version, new_version), r.old_version, r.new_version, fetchers, diags};
    bool editable = b.edit_string(*version_node, r.new_version, "[recipe] version");
    if (r.old_revision != 1) editable = b.edit_string(*revision_node, "1", "[recipe] revision") && editable;

    toml::table const* source = doc.get_as<toml::table>("source");
    if (source) {
        r.inputs.push_back(b.input("source", "the source", *source));
        if (toml::array const* patches = source->get_as<toml::array>("patches")) {
            for (toml::node const& p : *patches) r.patches.push_back(string_of(&p));
            if (!r.patches.empty()) {
                std::string list;
                for (std::string const& p : r.patches) list += (list.empty() ? "" : ", ") + p;
                diags.warning(position_of(file, *patches), "the source has " + std::to_string(r.patches.size())
                    + (r.patches.size() == 1 ? " patch (" : " patches (") + list
                    + "), written for version " + r.old_version + ": otok publish --dry-run --working-tree tells whether they still apply");
            }
        }
        if (toml::table const* verify = source->get_as<toml::table>("verify-against")) {
            r.inputs.push_back(b.input("source.verify-against", "the source's verify-against", *verify));
        }
    }
    if (toml::array const* resources = doc.get_as<toml::array>("resource")) {
        for (toml::node const& n : *resources) {
            toml::table const* t = n.as_table();
            if (!t) continue;
            std::string const name = string_of(t->get("name"));
            r.inputs.push_back(b.input(name, "the resource '" + name + "'", *t));
        }
    }

    // the source follows the version: a recipe whose source stays is not one otok can bump
    if (source && !r.inputs.empty() && r.inputs.front().name == "source") {
        bump_input const& s = r.inputs.front();
        if (s.status == bump_status::unchanged || s.status == bump_status::not_bumped) {
            std::string const why = s.status == bump_status::not_bumped ? s.message
                : s.git ? "its tag does not hold the version " + r.old_version : "its URL does not hold the version " + r.old_version;
            diags.error(position_of(file, *source), "the source does not follow the version: " + why + "; bump the recipe by hand");
        }
    }
    if (!editable || diags.has_errors()) return r;
    r.text = b.apply();
    return r;
}

void warn_version_mentions(std::filesystem::path const& dir, std::string_view recipe_text, std::span<version_spelling const> spellings,
    std::string const& old_version, diagnostics& diags)
{
    auto files = list_tree(dir);
    if (!files) return;
    for (tree_file const& f : *files) {
        std::string text;
        if (f.path == "recipe.toml") {
            text = recipe_text;
        } else {
            auto read = read_file(dir / path_from_utf8(f.path));
            if (!read) continue;
            text = std::move(*read);
        }
        if (text.find('\0') != std::string::npos) continue;  // not text
        std::string const shown = path_to_utf8(dir / path_from_utf8(f.path));
        std::uint32_t line = 1;
        for (std::size_t start = 0; start <= text.size();) {
            std::size_t const nl = text.find('\n', start);
            std::string_view const l = std::string_view{text}.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (auto const* s = first_mention(l, spellings)) {
                std::string const what = s->from == old_version ? "the version " + old_version : s->from + " (of the version " + old_version + ")";
                diags.warning({shown, line, 0}, "still mentions " + what + "; change it by hand if it should follow the version");
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
            ++line;
        }
    }
}

}
