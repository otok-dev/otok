// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/index/index.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <sstream>

#include "otok/core/fs.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/toml_reader.hpp"
#include "otok/core/tree_hash.hpp"
#include "otok/fetch/git.hpp"
#include "otok/recipe/recipe_tree.hpp"

namespace otok {

namespace {

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

std::string messages(diagnostics const& diags)
{
    std::string text;
    for (diagnostic const& d : diags.items()) text += "\n  " + format(d);
    return text;
}

// https for everything published; file:// for local and test indexes and recipes.
bool is_repository_url(std::string_view s) noexcept
{
    return (s.starts_with("https://") && s.size() > 8) || (s.starts_with("file://") && s.size() > 7);
}

void copy_node(toml::table& to, std::string_view key, toml::node const& value)
{
    value.visit([&](auto const& v) { to.insert(key, v); });
}

// The keys of a location kept in an entry (index.md 3).
constexpr std::array<std::string_view, 7> location_keys = {"url", "sha256", "mirrors", "git", "commit", "tag", "submodules"};

toml::table location_pins(toml::table const& from, std::string_view first = {})
{
    toml::table to;
    if (!first.empty()) {
        if (toml::node const* n = from.get(first)) copy_node(to, first, *n);
    }
    for (std::string_view key : location_keys) {
        if (toml::node const* n = from.get(key)) copy_node(to, key, *n);
    }
    to.is_inline(true);
    return to;
}

// The publication keys, which the projection leaves out (index.md 3.1).
constexpr std::array<std::string_view, 3> publication_keys = {"recipe-source", "recipe-sha256", "published"};

std::string projection_key(recipe_id const& id, recipe const& r)
{
    return id.str() + " " + r.variant + " " + r.version->str() + " " + std::to_string(r.revision);
}

}

toml::table recipe_projection(toml::table const& recipe_toml, std::string_view publisher, std::span<target_info const* const> targets)
{
    toml::table out;
    out.insert("format", index_entry_format);
    toml::table const* r = recipe_toml["recipe"].as_table();
    if (r) {
        out.insert("recipe", std::string{publisher} + "/" + (*r)["name"].value_or(std::string{}));
        // the variant, or the one target of the platforms (recipe-toml.md 3)
        if (toml::node const* n = r->get("variant")) {
            copy_node(out, "variant", *n);
        } else if (toml::array const* supported = recipe_toml["platforms"]["supported"].as_array()) {
            std::set<std::string> names;
            for (toml::node const& p : *supported) {
                for (target_info const* t : targets) {
                    if (t->find_platform(p.value_or(std::string{}))) names.insert(t->name);
                }
            }
            if (names.size() == 1) out.insert("variant", *names.begin());
        }
        for (std::string_view key : {"api", "library", "version", "revision", "channel", "license", "description", "keywords", "linkage", "c-standard", "cxx-standard"}) {
            if (toml::node const* n = r->get(key)) copy_node(out, key, *n);
        }
    }
    if (toml::table const* source = recipe_toml["source"].as_table()) out.insert("source", location_pins(*source));
    if (toml::array const* resources = recipe_toml["resource"].as_array()) {
        toml::array pins;
        for (toml::node const& res : *resources) {
            if (toml::table const* t = res.as_table()) {
                toml::table pin = location_pins(*t, "name");
                pin.is_inline(false);
                pins.push_back(std::move(pin));
            }
        }
        out.insert("resource", std::move(pins));
    }
    // `[tool]`, `[platforms]`, `[options]`, `[deps]`, `[tool-deps]`, `[toolchains]` and the target tables: every other table,
    // those of targets this otok does not know too, so that it projects a newer otok's recipe as that otok did
    // (docs/PLAN.md decision 98); a valid recipe has no other
    for (auto const& [key, n] : recipe_toml) {
        static constexpr std::string_view left_out[] = {"recipe", "source", "resource", "vars", "product", "publish", "metadata"};
        if (n.is_table() && std::ranges::none_of(left_out, [&](std::string_view k) { return k == key.str(); })) copy_node(out, key.str(), n);
    }
    if (toml::table* options = out["options"].as_table()) {
        for (auto& [name, option] : *options) {
            if (toml::table* o = option.as_table()) o->erase("doc");
        }
    }
    return out;
}

std::optional<index_entry> parse_index_entry(std::string_view content, std::string const& file,
    std::span<target_info const* const> targets, diagnostics& diags, index_reading reading)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    index_entry e;

    if (auto format = root.integer("format", need::required); format && *format != index_entry_format) {
        root.error(root.position("format"), "index entry format " + std::to_string(*format)
            + " needs a newer otok: this one reads format " + std::to_string(index_entry_format));
        return std::nullopt;
    }
    if (auto id = root.string("recipe", need::required)) {
        if (auto parsed = parse_recipe_id(*id)) e.id = std::move(*parsed);
        else root.error(root.position("recipe"), root.describe("recipe") + " must be a recipe id \"publisher/name\"");
    }
    if (auto source = root.table("recipe-source", need::required)) {
        e.recipe_source.type = locked_location::kind::git;
        if (auto git = source->string("git", need::required)) {
            if (!is_repository_url(*git)) source->error(source->position("git"), source->describe("git") + " must be an https URL");
            e.recipe_source.git = std::move(*git);
        }
        if (auto commit = source->string("commit", need::required)) {
            if (!is_git_commit(*commit)) source->error(source->position("commit"), source->describe("commit") + " must be a full 40-character commit SHA");
            e.recipe_source.commit = std::move(*commit);
        }
        if (auto tag = source->string("tag")) {
            if (!is_git_tag_name(*tag)) source->error(source->position("tag"), source->describe("tag") + " is not a valid tag name: " + in_quotes(*tag));
            e.recipe_source.tag = std::move(*tag);
        }
        if (auto path = source->string("path")) {
            if (!path->empty() && classify_file_path(*path) != file_path_kind::inside) {
                source->error(source->position("path"), source->describe("path") + " must be a directory inside the repository");
            }
            e.recipe_source.path = std::move(*path);
        }
        source->finish();
    }
    if (auto sha = root.string("recipe-sha256", need::required)) {
        if (!is_sha256_hex(*sha)) root.error(root.position("recipe-sha256"), root.describe("recipe-sha256") + " must be 64 lowercase hexadecimal digits");
        e.recipe_sha256 = std::move(*sha);
    }
    if (toml::node const* published = root.node("published")) {
        auto const* time = published->as_date_time();
        if (!time || !time->get().offset) root.error(root.position("published"), root.describe("published") + " must be a date and time with its offset (UTC)");
        else e.published = time->get();
    }
    auto projection = read_recipe_projection(root, targets, diags, reading == index_reading::client ? recipe_names::open : recipe_names::strict);
    if (!projection || diags.error_count() != errors_before) return std::nullopt;
    e.projection = std::move(*projection);
    e.projection.name = e.id.name;

    auto table = std::make_shared<toml::table>(doc->table());
    for (std::string_view key : publication_keys) table->erase(key);
    e.table = std::move(table);
    return e;
}

std::string index_entry::file() const
{
    return projection.variant + "/" + projection.version->str() + "-r" + std::to_string(projection.revision) + ".toml";
}

std::string unreadable_entry::label() const
{
    // "<variant>/<version>-r<revision>.toml" (index.md 3)
    std::string_view name = file;
    std::size_t const slash = name.find('/');
    std::string_view const variant = slash == std::string_view::npos ? std::string_view{} : name.substr(0, slash);
    std::string_view stem = slash == std::string_view::npos ? name : name.substr(slash + 1);
    if (stem.ends_with(".toml")) stem.remove_suffix(5);
    return std::string{stem} + (variant.empty() ? "" : " (" + std::string{variant} + ")");
}

std::string target_of_entry(recipe const& projection, std::span<target_info const* const> targets)
{
    for (std::string const& p : projection.platforms) {
        for (target_info const* t : targets) {
            if (t->find_platform(p)) return t->name;
        }
    }
    return {};
}

std::string write_index_entry(toml::table const& projection, locked_location const& recipe_source, std::string const& recipe_sha256,
    std::optional<toml::date_time> const& published)
{
    toml::table entry = projection;
    toml::table source;
    source.insert("git", recipe_source.git);
    source.insert("commit", recipe_source.commit);
    if (!recipe_source.tag.empty()) source.insert("tag", recipe_source.tag);
    if (!recipe_source.path.empty()) source.insert("path", recipe_source.path);
    source.is_inline(true);
    entry.insert("recipe-source", std::move(source));
    entry.insert("recipe-sha256", recipe_sha256);
    if (published) entry.insert("published", *published);
    std::ostringstream out;
    out << toml::toml_formatter{entry, toml::format_flags::allow_unicode_strings} << "\n";
    return out.str();
}

result<std::string> stamp_index_entry(std::string_view text, toml::date_time const& published)
{
    toml::table entry;
    try {
        entry = toml::parse(text);
    } catch (toml::parse_error const& e) {
        return fail("the entry cannot be read: " + std::string{e.description()});
    }
    entry.insert_or_assign("published", published);
    std::ostringstream out;
    out << toml::toml_formatter{entry, toml::format_flags::allow_unicode_strings} << "\n";
    return out.str();
}

std::string_view severity_name(advisory_severity s) noexcept
{
    switch (s) {
    case advisory_severity::withdrawn: return "withdrawn";
    case advisory_severity::security: return "security";
    case advisory_severity::broken: return "broken";
    }
    return "?";
}

std::optional<advisory> parse_advisory(std::string_view content, std::string const& file, diagnostics& diags, index_reading reading)
{
    bool const client = reading == index_reading::client;
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    advisory a;
    a.id = root.string("id", need::required).value_or("");
    bool const by_recipes = root.contains("recipes");
    if (by_recipes == root.contains("library")) {
        root.error(root.position(), "an advisory needs exactly one of 'recipes' (recipe ids) or 'library' (a library name)");
        root.ignore("recipes");
        root.ignore("library");
    } else if (by_recipes) {
        for (std::string const& text : root.strings("recipes").value_or(std::vector<std::string>{})) {
            if (auto id = parse_recipe_id(text)) a.recipes.push_back(std::move(*id));
            else root.error(root.position("recipes"), in_quotes(text) + " in " + root.describe("recipes") + " is not a recipe id \"publisher/name\"");
        }
    } else {
        a.library = root.string("library").value_or("");
        if (!is_library_name(a.library)) root.error(root.position("library"), in_quotes(a.library) + " in " + root.describe("library") + " is not a library name");
    }
    if (auto variants = root.strings("variants")) {
        if (!by_recipes) root.error(root.position("variants"), root.describe("variants") + " goes with 'recipes', not 'library'");
        if (variants->empty()) root.error(root.position("variants"), root.describe("variants") + " must not be empty");
        for (std::string const& v : *variants) {
            if (!is_recipe_name(v)) root.error(root.position("variants"), in_quotes(v) + " in " + root.describe("variants") + " is not a variant name");
        }
        a.variants = std::move(*variants);
    }
    if (auto versions = root.strings("versions", need::required)) {
        if (versions->empty()) root.error(root.position("versions"), root.describe("versions") + " must not be empty");
        a.versions = std::move(*versions);
    }
    if (auto severity = root.string("severity", need::required)) {
        if (*severity == "withdrawn") a.severity = advisory_severity::withdrawn;
        else if (*severity == "security") a.severity = advisory_severity::security;
        else if (*severity == "broken") a.severity = advisory_severity::broken;
        else if (client) {
            // a severity of a newer otok: the strictest this one knows (index.md 7.2)
            a.severity = advisory_severity::security;
            a.unknown.push_back("severity " + in_quotes(*severity) + " is unknown to this otok and taken as 'security'");
        } else {
            root.error(root.position("severity"), root.describe("severity") + " must be \"withdrawn\", \"security\" or \"broken\", not " + in_quotes(*severity));
        }
    }
    if (auto status = root.string("status", need::required)) {
        if (*status == "under-review") a.status = advisory_status::under_review;
        else if (*status == "confirmed") a.status = advisory_status::confirmed;
        else if (*status == "rejected") a.status = advisory_status::rejected;
        else if (client) {
            a.status = advisory_status::confirmed;
            a.unknown.push_back("status " + in_quotes(*status) + " is unknown to this otok and taken as 'confirmed'");
        } else {
            root.error(root.position("status"), root.describe("status") + " must be \"under-review\", \"confirmed\" or \"rejected\", not " + in_quotes(*status));
        }
    }
    a.summary = root.string("summary", need::required).value_or("");
    a.aliases = root.strings("aliases").value_or(std::vector<std::string>{});
    a.details = root.string("details").value_or("");
    a.references = root.strings("references").value_or(std::vector<std::string>{});
    for (std::string_view key : {"published", "updated"}) {
        toml::node const* time = root.node(key, key == "published" ? need::required : need::optional);
        if (time && !time->is_date_time()) root.error(root.position(key), root.describe(key) + " must be a date and time");
        else if (time) (key == "published" ? a.published : a.updated) = time->as_date_time()->get();
    }
    // a key of a newer otok only narrows what an advisory concerns: left aside, it applies more widely (index.md 7.2)
    if (client) root.ignore_rest();
    root.finish();
    if (diags.error_count() != errors_before) return std::nullopt;
    return a;
}

bool advisory_concerns(advisory const& a, recipe_id const& id, std::string_view variant, std::string_view library, version const& v,
    std::int64_t revision, version_order const& order)
{
    bool const recipe = a.recipes.empty() ? a.library == library : std::ranges::find(a.recipes, id) != a.recipes.end();
    if (!recipe || !order.validate(v)) return false;
    if (!a.variants.empty() && std::ranges::find(a.variants, variant) == a.variants.end()) return false;
    auto const same = [&](version const& other) { return order.validate(other) && order.compare(v, other) == std::partial_ordering::equivalent; };
    for (std::string const& entry : a.versions) {
        // "<version>-r<revision>", then "<version>", then a constraint
        std::size_t const r = entry.rfind("-r");
        if (r != std::string::npos && r + 2 < entry.size() && std::ranges::all_of(entry.substr(r + 2), [](char c) { return c >= '0' && c <= '9'; })) {
            if (auto exact = version::parse(entry.substr(0, r)); exact && order.validate(*exact)) {
                if (same(*exact) && entry.substr(r + 2) == std::to_string(revision)) return true;
                continue;
            }
        }
        if (auto plain = version::parse(entry); plain && order.validate(*plain)) {
            if (same(*plain)) return true;
            continue;
        }
        if (auto constraint = version_constraint::parse(entry); constraint && constraint->validate(order) && constraint->matches(v, order)) return true;
    }
    return false;
}

std::string recipe_status::deprecation() const
{
    if (!deprecated) return {};
    std::string text = "deprecated";
    if (!deprecated_reason.empty()) text += ": " + deprecated_reason;
    if (in_favor_of) text += "; use '" + in_favor_of->str() + "' instead";
    return text;
}

std::optional<recipe_status> parse_recipe_status(std::string_view content, std::string const& file, diagnostics& diags, index_reading reading)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    recipe_status s;
    auto const read_id = [&](toml_table& t, std::string_view key) -> std::optional<recipe_id> {
        auto text = t.string(key);
        if (!text) return std::nullopt;
        auto id = parse_recipe_id(*text);
        if (!id) t.error(t.position(key), t.describe(key) + " must be a recipe id \"publisher/name\"");
        return id;
    };
    if (auto d = root.table("deprecated")) {
        s.deprecated = true;
        s.deprecated_reason = d->string("reason").value_or("");
        s.in_favor_of = read_id(*d, "in-favor-of");
        if (reading == index_reading::client) d->ignore_rest();
        d->finish();
    }
    s.transferred_from = read_id(root, "transferred-from");
    if (reading == index_reading::client) root.ignore_rest();
    root.finish();
    if (diags.error_count() != errors_before) return std::nullopt;
    return s;
}

std::optional<version_order> parse_library_file(std::string_view content, std::string const& file, diagnostics& diags, index_reading reading)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    std::optional<version_order> order;
    std::string const scheme = root.string("scheme").value_or("dotted");
    auto listed = root.strings("order");
    if (scheme == "dotted") {
        order = version_order::dotted();
    } else if (scheme == "semver") {
        order = version_order::semver();
    } else if (scheme == "explicit") {
        if (!listed) {
            root.error(root.position(), "the explicit scheme needs 'order', all known versions, oldest first");
        } else {
            std::vector<version> versions;
            for (std::string const& text : *listed) {
                auto v = version::parse(text);
                if (v) versions.push_back(std::move(*v));
                else root.error(root.position("order"), v.error().message);
            }
            auto explicit_order = version_order::explicit_order(std::move(versions));
            if (explicit_order) order = std::move(*explicit_order);
            else root.error(root.position("order"), explicit_order.error().message);
        }
    } else if (reading == index_reading::client) {
        root.error(root.position("scheme"), "the version scheme " + in_quotes(scheme) + " needs a newer otok");
    } else {
        root.error(root.position("scheme"), root.describe("scheme") + " must be \"dotted\", \"semver\" or \"explicit\", not " + in_quotes(scheme));
    }
    if (listed && scheme != "explicit") root.error(root.position("order"), root.describe("order") + " is for the explicit scheme only");
    if (reading == index_reading::client) root.ignore_rest();
    root.finish();
    if (diags.error_count() != errors_before) return std::nullopt;
    return order;
}

std::optional<index_settings> parse_index_settings(std::string_view content, std::string const& file, diagnostics& diags, index_reading reading)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    index_settings s;
    if (auto format = root.integer("format", need::required); format && *format != 1) {
        root.error(root.position("format"), "index format " + std::to_string(*format) + " needs a newer otok: this one reads format 1");
        return std::nullopt;
    }
    s.name = root.string("name").value_or("");
    for (auto [key, out] : {std::pair{"catalog", &s.catalog}, std::pair{"view", &s.view}, std::pair{"repository", &s.repository}}) {
        auto url = root.string(key);
        if (!url) continue;
        if (!url->starts_with("https://")) root.error(root.position(key), root.describe(key) + " must be an https URL, not " + in_quotes(*url));
        *out = std::move(*url);
    }
    for (auto [key, out] : {std::pair{"established-entries-per-day", &s.established_entries_per_day}, std::pair{"review-entries-per-day", &s.review_entries_per_day}}) {
        auto limit = root.integer(key);
        if (!limit) continue;
        if (*limit < 1) root.error(root.position(key), root.describe(key) + " must be at least 1");
        else *out = static_cast<std::size_t>(*limit);
    }
    if (reading == index_reading::client) root.ignore_rest();
    root.finish();
    if (diags.error_count() != errors_before) return std::nullopt;
    return s;
}

publication_limit publication_limit_for(index_settings const& settings, bool established)
{
    publication_limit l;
    l.key = established ? "established-entries-per-day" : "review-entries-per-day";
    l.limit = established ? settings.established_entries_per_day : settings.review_entries_per_day;
    return l;
}

std::optional<std::string> entry_publisher(std::string_view path)
{
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
        std::size_t const slash = path.find('/', start);
        parts.push_back(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    if (parts.size() != 5 || parts[0] != "recipes" || !parts[4].ends_with(".toml")) return std::nullopt;
    std::string_view const stem = parts[4].substr(0, parts[4].size() - 5);
    std::size_t const r = stem.rfind("-r");
    if (r == std::string_view::npos || r == 0 || r + 2 >= stem.size()) return std::nullopt;
    if (!std::ranges::all_of(stem.substr(r + 2), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    return std::string{parts[1]};
}

publication_limit publication_limit_of(std::filesystem::path const& root, std::string_view publisher, publication_limit l,
    std::chrono::sys_seconds now, std::set<std::string, std::less<>> const& skip)
{
    std::vector<std::chrono::sys_seconds> times;
    std::error_code ec;
    for (auto const& f : std::filesystem::recursive_directory_iterator{root / "recipes" / path_from_utf8(publisher), ec}) {
        if (!f.is_regular_file(ec) || f.path().extension() != ".toml" || f.path().filename() == "recipe-status.toml") continue;
        if (skip.contains(path_to_utf8(f.path().lexically_relative(root).generic_u8string()))) continue;
        auto const content = read_file(f.path());
        if (!content) continue;
        try {
            toml::table const t = toml::parse(*content);
            auto const* published = t["published"].as_date_time();
            if (!published || !published->get().offset) continue;
            toml::date_time const& p = published->get();
            using namespace std::chrono;
            sys_seconds at = sys_days{year{p.date.year} / month{p.date.month} / day{p.date.day}} + hours{p.time.hour} + minutes{p.time.minute}
                + seconds{p.time.second} - minutes{p.offset->minutes};
            if (at > now - hours{24}) times.push_back(at);
        } catch (toml::parse_error const&) {
        }
    }
    l.recent = times.size();
    l.passes_at.reset();
    if (l.pending + l.adding > l.limit) return l;
    if (!l.exceeded()) {
        l.passes_at = now;
        return l;
    }
    // once enough of the others are older than 24 hours
    std::ranges::sort(times);
    l.passes_at = times[l.recent + l.pending + l.adding - l.limit - 1] + std::chrono::hours{24};
    return l;
}

std::string publication_limit_text(std::string_view publisher, publication_limit const& l)
{
    std::string const limit = "more than " + std::to_string(l.limit) + " of the index (" + l.key + ")";
    if (l.adding > l.limit) {
        return "these " + std::to_string(l.adding) + " entries are " + limit + ": publish at most " + std::to_string(l.limit) + " at a time";
    }
    std::string requests;
    for (std::int64_t n : l.pending_requests) requests += (requests.empty() ? "#" : ", #") + std::to_string(n);
    std::string const making = (l.adding == 1 ? std::string{"this one makes "} : "these " + std::to_string(l.adding) + " make ")
        + std::to_string(l.recent + l.pending + l.adding) + ", " + limit;
    std::string const pending = std::to_string(l.pending) + (l.pending == 1 ? " entry" : " entries") + " in its other open pull requests ("
        + requests + ")";
    if (!l.passes_at) {
        return "the publisher " + in_quotes(publisher) + " has " + pending + ", and " + making + "; publish them again once those are merged or closed";
    }
    using namespace std::chrono;
    auto const minute = ceil<minutes>(*l.passes_at);
    auto const date = floor<days>(minute);
    year_month_day const ymd{date};
    hh_mm_ss const hms{minute - date};
    auto const two = [](long long n) { return (n < 10 ? "0" : "") + std::to_string(n); };
    std::string const when = std::to_string(static_cast<int>(ymd.year())) + "-" + two(static_cast<unsigned>(ymd.month())) + "-"
        + two(static_cast<unsigned>(ymd.day())) + " " + two(hms.hours().count()) + ":" + two(hms.minutes().count()) + " UTC";
    return "the publisher " + in_quotes(publisher) + " has " + std::to_string(l.recent) + (l.recent == 1 ? " entry" : " entries")
        + " published in the last 24 hours" + (l.pending > 0 ? " and " + pending : std::string{}) + ", and " + making
        + "; they pass when published again after " + when;
}

std::optional<publisher_file> parse_publisher(std::string_view content, std::string const& file, diagnostics& diags, index_reading reading)
{
    bool const client = reading == index_reading::client;
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    publisher_file p;
    if (auto id = root.integer("github-id", need::required)) {
        if (*id <= 0) root.error(root.position("github-id"), root.describe("github-id") + " must be a positive account id");
        p.github_id = *id;
    }
    if (auto kind = root.string("kind", need::required)) {
        if (*kind == "organization") p.organization = true;
        else if (*kind != "user") root.error(root.position("kind"), root.describe("kind") + " must be \"user\" or \"organization\", not " + in_quotes(*kind));
    }
    p.established = root.boolean("established").value_or(false);
    if (auto trusted = root.tables("trusted-publishing")) {
        for (toml_table& t : *trusted) {
            (void)t.string("repository", need::required);
            (void)t.string("workflow", need::required);
            (void)t.string("environment");
            if (client) t.ignore_rest();
            t.finish();
        }
    }
    if (auto suspended = root.string("suspended")) {
        if (suspended->empty()) root.error(root.position("suspended"), root.describe("suspended") + " must give the reason");
        p.suspended = std::move(*suspended);
    }
    if (client) root.ignore_rest();
    root.finish();
    if (diags.error_count() != errors_before) return std::nullopt;
    return p;
}

std::string write_publisher(publisher_file const& p)
{
    std::string text = "github-id = " + std::to_string(p.github_id) + "\nkind = \"" + (p.organization ? "organization" : "user") + "\"\n";
    if (p.established) text += "established = true\n";
    if (!p.suspended.empty()) {
        std::ostringstream reason;
        reason << toml::value<std::string>{p.suspended};
        text += "suspended = " + reason.str() + "\n";
    }
    return text;
}

std::string confusable_form(std::string_view name)
{
    std::string form;
    for (char c : name) {
        if (c == '-') continue;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '0') c = 'o';
        else if (c == '1') c = 'l';
        else if (c == '5') c = 's';
        form += c;
    }
    return form;
}

unavailable_version const* input_availability::find(recipe_id const& id, std::string_view version) const noexcept
{
    for (unavailable_version const& u : unavailable) {
        if (u.recipe == id && u.version == version) return &u;
    }
    return nullptr;
}

std::optional<input_availability> parse_availability(std::string_view content, std::string const& file, diagnostics& diags,
    index_reading reading)
{
    bool const client = reading == index_reading::client;
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();
    input_availability a;
    auto const time_of = [](toml_table& t, std::string_view key, need n) -> std::optional<toml::date_time> {
        toml::node const* node = t.node(key, n);
        if (!node) return std::nullopt;
        auto const* time = node->as_date_time();
        if (!time || !time->get().offset) {
            t.error(t.position(key), t.describe(key) + " must be a date and time with its offset (UTC)");
            return std::nullopt;
        }
        return time->get();
    };
    a.checked = time_of(root, "checked", need::required);
    if (auto list = root.tables("unavailable")) {
        for (toml_table& t : *list) {
            unavailable_version u;
            if (auto id = t.string("recipe", need::required)) {
                if (auto parsed = parse_recipe_id(*id)) u.recipe = std::move(*parsed);
                else t.error(t.position("recipe"), t.describe("recipe") + " must be a recipe id \"publisher/name\"");
            }
            u.version = t.string("version", need::required).value_or("");
            u.since = time_of(t, "since", need::required).value_or(toml::date_time{});
            u.inputs = t.strings("inputs", need::required).value_or(std::vector<std::string>{});
            if (client) t.ignore_rest();
            t.finish();
            a.unavailable.push_back(std::move(u));
        }
    }
    if (client) root.ignore_rest();
    root.finish();
    if (diags.error_count() != errors_before) return std::nullopt;
    return a;
}

std::string write_availability(input_availability const& a)
{
    std::ostringstream out;
    out << "# Generated by otok index-probe; do not edit.\n";
    if (a.checked) out << "checked = " << *a.checked << "\n";
    for (unavailable_version const& u : a.unavailable) {
        toml::array inputs;
        for (std::string const& i : u.inputs) inputs.push_back(i);
        toml::table t{{"recipe", u.recipe.str()}, {"version", u.version}, {"since", u.since}, {"inputs", std::move(inputs)}};
        out << "\n[[unavailable]]\n" << toml::toml_formatter{t, toml::format_flags::allow_unicode_strings} << "\n";
    }
    return out.str();
}

result<index_addition> make_index_entry(std::filesystem::path const& recipe_dir, locked_location const& recipe_source,
    std::string_view publisher, std::optional<toml::date_time> const& published, std::span<target_info const* const> targets)
{
    if (!is_publisher_name(publisher)) return fail(in_quotes(publisher) + " is not a valid publisher name");
    diagnostics diags;
    auto r = load_recipe(recipe_dir, targets, diags);
    auto files = list_tree(recipe_dir);
    if (!files) return std::unexpected{files.error()};
    check_recipe_tree(recipe_dir, *files, severity::error, diags);
    if (r) {
        for (target_info const* t : targets) {
            if (!t->check_publishable) continue;
            for (std::string const& problem : t->check_publishable(*r)) diags.error({}, problem);
        }
        // what builds for the publisher builds for everyone (recipe-toml.md 13, docs/PLAN.md decision 85)
        for (toolchain_requirement const& t : r->toolchains) {
            if (t.default_recipe || !t.from.empty()) continue;  // built by a recipe: its own, or the dependency's
            diags.error(t.where, "the toolchain " + in_quotes(t.name) + " has no 'default-recipe': a published recipe names the recipe "
                "of each of its toolchains, so that no build needs it installed (recipe-toml.md 13)");
        }
    }
    if (!r || diags.has_errors()) return fail("the recipe in '" + path_to_utf8(recipe_dir) + "' cannot be published:" + messages(diags));
    auto content = read_file(recipe_dir / "recipe.toml");
    if (!content) return std::unexpected{content.error()};
    auto doc = toml_document::parse(*content, "recipe.toml", diags);
    if (!doc) return fail("the recipe in '" + path_to_utf8(recipe_dir) + "' cannot be read:" + messages(diags));

    // a published recipe has one target (index.md 3)
    std::set<std::string> of;
    for (std::string const& p : r->platforms) {
        for (target_info const* t : targets) {
            if (t->find_platform(p)) of.insert(t->name);
        }
    }
    if (of.size() > 1) {
        std::string names;
        for (std::string const& t : of) names += (names.empty() ? "'" : ", '") + t + "'";
        return fail("the recipe in '" + path_to_utf8(recipe_dir) + "' cannot be published: its platforms are of the targets " + names
            + ", and a published recipe has one (index.md 3); make a variant of each");
    }

    index_addition a;
    std::string const version = r->version->str() + "-r" + std::to_string(r->revision);
    a.label = std::string{publisher} + "/" + r->name + " " + version + " (" + r->variant + ")";
    a.entry_path = "recipes/" + std::string{publisher} + "/" + r->name + "/" + r->variant + "/" + version + ".toml";
    a.entry_text = write_index_entry(recipe_projection(doc->table(), publisher, targets), recipe_source, tree_hash(*files), published);
    a.library_path = "libraries/" + r->library + ".toml";
    a.library_text = "scheme = \"dotted\"\n";
    return a;
}

result<bool> same_index_entry(std::string_view existing, std::string_view fresh, std::string const& file,
    std::span<target_info const* const> targets)
{
    diagnostics diags;
    auto old = parse_index_entry(existing, file, targets, diags, index_reading::ci);
    if (!old) return fail(file + " in the index is invalid:" + messages(diags));
    auto made = parse_index_entry(fresh, file, targets, diags, index_reading::ci);
    if (!made) return fail("the new entry " + file + " is invalid:" + messages(diags));
    // the recipe's tree, wherever it is now: a later commit of a repository of many recipes, or another
    // repository (DESIGN 7.4); the entry keeps the location it was published from
    return *old->table == *made->table && old->recipe_sha256 == made->recipe_sha256;
}

result<index_added> add_index_entry(std::filesystem::path const& index_dir, index_addition const& addition,
    std::span<target_info const* const> targets)
{
    std::filesystem::path const entry = index_dir / path_from_utf8(addition.entry_path);
    std::error_code ec;
    if (std::filesystem::exists(entry, ec)) {
        auto existing = read_file(entry);
        if (!existing) return std::unexpected{existing.error()};
        auto same = same_index_entry(*existing, addition.entry_text, addition.entry_path, targets);
        if (!same) return std::unexpected{same.error()};
        if (*same) return index_added::already;
        return fail("the index has " + addition.entry_path + " already, published differently; entries never change, so publish a new revision instead");
    }
    std::filesystem::create_directories(entry.parent_path(), ec);
    if (auto ok = write_file_atomic(entry, addition.entry_text); !ok) return std::unexpected{ok.error()};

    std::filesystem::path const library = index_dir / path_from_utf8(addition.library_path);
    if (!std::filesystem::exists(library, ec)) {
        std::filesystem::create_directories(library.parent_path(), ec);
        if (auto ok = write_file_atomic(library, addition.library_text); !ok) return std::unexpected{ok.error()};
    }
    return index_added::written;
}

namespace {

// A file of the directory, or nothing when it does not exist.
result<std::optional<std::string>> optional_file(std::filesystem::path const& file)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return std::nullopt;
    auto content = read_file(file);
    if (!content) return std::unexpected{content.error()};
    return std::move(*content);
}

class directory_source : public index_source
{
public:
    explicit directory_source(std::filesystem::path root) : root_{std::move(root)} {}

    result<std::optional<std::string>> settings() override { return optional_file(root_ / "index.toml"); }

    result<std::optional<index_recipe_files>> recipe(recipe_id const& id) override
    {
        std::filesystem::path const dir = root_ / "recipes" / path_from_utf8(id.publisher) / path_from_utf8(id.name);
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return std::nullopt;
        // the entries in the directories of the variants (index.md 3); one beside them is read to be refused
        std::vector<std::string> files;  // relative to `dir`
        for (auto const& item : std::filesystem::directory_iterator{dir, ec}) {
            std::filesystem::path const& p = item.path();
            if (item.is_directory(ec)) {
                for (auto const& entry : std::filesystem::directory_iterator{p, ec}) {
                    if (entry.path().extension() == ".toml") files.push_back(path_to_utf8(p.filename()) + "/" + path_to_utf8(entry.path().filename()));
                }
            } else if (p.extension() == ".toml" && p.filename() != "recipe-status.toml") {
                files.push_back(path_to_utf8(p.filename()));
            }
        }
        if (ec) return fail("cannot read '" + path_to_utf8(dir) + "': " + ec.message());
        std::ranges::sort(files);
        index_recipe_files found;
        for (std::string const& file : files) {
            auto content = read_file(dir / path_from_utf8(file));
            if (!content) return std::unexpected{content.error()};
            found.entries.push_back({file, std::move(*content)});
        }
        auto status = optional_file(dir / "recipe-status.toml");
        if (!status) return std::unexpected{status.error()};
        found.status = std::move(*status);
        return found;
    }

    result<std::optional<std::string>> library(std::string_view library) override
    {
        return optional_file(root_ / "libraries" / path_from_utf8(std::string{library} + ".toml"));
    }

    result<std::vector<index_file>> advisories() override
    {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        std::filesystem::path const dir = root_ / "advisories";
        if (std::filesystem::is_directory(dir, ec)) {
            for (auto const& item : std::filesystem::directory_iterator{dir, ec}) {
                if (item.path().extension() == ".toml") files.push_back(item.path());
            }
        }
        std::ranges::sort(files);
        std::vector<index_file> list;
        for (std::filesystem::path const& file : files) {
            auto content = read_file(file);
            if (!content) return std::unexpected{content.error()};
            list.push_back({path_to_utf8(file.filename()), std::move(*content)});
        }
        return list;
    }

    result<std::optional<std::string>> availability() override { return optional_file(root_ / "availability.toml"); }

    result<std::vector<recipe_id>> recipe_ids() override
    {
        std::vector<recipe_id> ids;
        std::error_code ec;
        std::filesystem::path const dir = root_ / "recipes";
        if (!std::filesystem::is_directory(dir, ec)) return ids;
        for (auto const& publisher : std::filesystem::directory_iterator{dir, ec}) {
            if (!publisher.is_directory(ec)) continue;
            for (auto const& name : std::filesystem::directory_iterator{publisher.path(), ec}) {
                if (name.is_directory(ec)) ids.push_back({path_to_utf8(publisher.path().filename()), path_to_utf8(name.path().filename())});
            }
        }
        if (ec) return fail("cannot read '" + path_to_utf8(dir) + "': " + ec.message());
        std::ranges::sort(ids, {}, &recipe_id::str);
        return ids;
    }

private:
    std::filesystem::path root_;
};

}

std::unique_ptr<index_source> directory_index_source(std::filesystem::path root)
{
    return std::make_unique<directory_source>(std::move(root));
}

namespace {

class overlay_source : public index_source
{
public:
    overlay_source(std::unique_ptr<index_source> base, std::vector<index_addition> additions)
        : base_{std::move(base)}
    {
        for (index_addition& a : additions) {
            // "recipes/<publisher>/<name>/<variant>/<file>"
            std::string_view rest = a.entry_path;
            if (!rest.starts_with("recipes/")) continue;
            rest.remove_prefix(8);
            std::size_t const publisher = rest.find('/');
            std::size_t const name = publisher == std::string_view::npos ? publisher : rest.find('/', publisher + 1);
            if (name == std::string_view::npos) continue;
            auto id = parse_recipe_id(rest.substr(0, name));
            if (!id) continue;
            entries_[id->str()].push_back({std::string{rest.substr(name + 1)}, std::move(a.entry_text)});
            if (!a.library_path.empty()) {
                std::string_view library = a.library_path;
                if (library.starts_with("libraries/") && library.ends_with(".toml")) {
                    libraries_.emplace(std::string{library.substr(10, library.size() - 15)}, std::move(a.library_text));
                }
            }
        }
    }

    result<std::optional<std::string>> settings() override { return base_->settings(); }

    result<std::optional<index_recipe_files>> recipe(recipe_id const& id) override
    {
        auto found = base_->recipe(id);
        if (!found) return found;
        auto const it = entries_.find(id.str());
        if (it == entries_.end()) return found;
        index_recipe_files files = found->value_or(index_recipe_files{});
        for (index_file const& f : it->second) {
            if (std::ranges::find(files.entries, f.file, &index_file::file) == files.entries.end()) files.entries.push_back(f);
        }
        return files;
    }

    result<std::optional<std::string>> library(std::string_view library) override
    {
        auto found = base_->library(library);
        if (!found || *found) return found;
        auto const it = libraries_.find(library);
        if (it == libraries_.end()) return found;
        return it->second;
    }

    result<std::vector<index_file>> advisories() override { return base_->advisories(); }
    result<std::optional<std::string>> availability() override { return base_->availability(); }

    result<std::vector<recipe_id>> recipe_ids() override
    {
        auto ids = base_->recipe_ids();
        if (!ids) return ids;
        for (auto const& [id, files] : entries_) {
            if (std::ranges::find(*ids, id, &recipe_id::str) == ids->end()) ids->push_back(*parse_recipe_id(id));
        }
        std::ranges::sort(*ids, {}, &recipe_id::str);
        return ids;
    }

private:
    std::unique_ptr<index_source> base_;
    std::map<std::string, std::vector<index_file>, std::less<>> entries_;  // by recipe id
    std::map<std::string, std::string, std::less<>> libraries_;           // by library
};

}

std::unique_ptr<index_source> overlay_index_source(std::unique_ptr<index_source> base, std::vector<index_addition> additions)
{
    return std::make_unique<overlay_source>(std::move(base), std::move(additions));
}

index_entry const* newest_stable(std::span<index_entry const* const> newest_first)
{
    for (index_entry const* e : newest_first) {
        if (e->projection.channel == release_channel::stable) return e;
    }
    return newest_first.empty() ? nullptr : newest_first.front();
}

std::vector<std::string> version_targets(std::span<index_entry const* const> entries, version const& v, std::span<target_info const* const> targets)
{
    std::set<std::string> found;
    for (index_entry const* e : entries) {
        if (*e->projection.version != v) continue;
        if (std::string t = target_of_entry(e->projection, targets); !t.empty()) found.insert(std::move(t));
    }
    return {found.begin(), found.end()};
}

recipe_index::recipe_index(std::unique_ptr<index_source> source, std::span<target_info const* const> targets, index_reading reading)
    : source_{std::move(source)}, targets_{targets.begin(), targets.end()}, reading_{reading}
{
}

recipe_index::recipe_index(std::filesystem::path root, std::span<target_info const* const> targets, index_reading reading)
    : recipe_index{directory_index_source(std::move(root)), targets, reading}
{
}

result<index_settings> recipe_index::settings()
{
    auto content = source_->settings();
    if (!content) return std::unexpected{content.error()};
    if (!*content) return fail("the index has no index.toml");
    diagnostics diags;
    auto s = parse_index_settings(**content, "index.toml", diags, reading_);
    if (!s) return fail("the index has an invalid index.toml:" + messages(diags));
    return std::move(*s);
}

result<bool> recipe_index::has(recipe_id const& id)
{
    if (entries_.contains(id.str())) return true;
    auto files = source_->recipe(id);
    if (!files) return std::unexpected{files.error()};
    return files->has_value();
}

result<std::vector<index_entry> const*> recipe_index::entries(recipe_id const& id)
{
    if (auto it = entries_.find(id.str()); it != entries_.end()) return &it->second;
    auto files = source_->recipe(id);
    if (!files) return std::unexpected{files.error()};
    if (!*files) return fail("the index has no recipe " + in_quotes(id.str()));

    std::vector<index_entry> list;
    std::vector<unreadable_entry> unreadable;
    diagnostics diags;
    for (index_file const& file : (*files)->entries) {
        std::string const display = "recipes/" + id.str() + "/" + file.file;
        // a client leaves aside what it cannot read (index.md 3.2); index CI refuses it
        diagnostics one;
        diagnostics& d = reading_ == index_reading::client ? one : diags;
        auto e = parse_index_entry(file.text, display, targets_, d, reading_);
        if (e) {
            std::string const stem = e->projection.version->str() + "-r" + std::to_string(e->projection.revision);
            if (e->id != id) {
                d.error({display}, "the entry is of recipe " + in_quotes(e->id.str()) + ", but its file is under " + in_quotes(id.str()));
            } else if (file.file != e->file()) {
                d.error({display}, "the entry is of the variant " + in_quotes(e->projection.variant) + " and version " + stem
                    + ", which does not match its file: it belongs in " + e->file());
            } else if (e->projection.platforms.empty()) {
                // every platform of a newer otok: of no use to this one (index.md 3.2)
                d.error({display}, "the entry has no platform this otok knows; it needs a newer otok");
            } else {
                list.push_back(std::move(*e));
                continue;
            }
        }
        if (reading_ == index_reading::client) {
            std::string reason = "it cannot be read";
            for (diagnostic const& item : one.items()) {
                if (item.level == severity::error) {
                    reason = item.message;
                    break;
                }
            }
            unreadable.push_back({file.file, std::move(reason)});
        }
    }
    if (diags.has_errors()) return fail("the index has invalid entries of " + in_quotes(id.str()) + ":" + messages(diags));
    std::ranges::sort(unreadable, {}, &unreadable_entry::file);
    if (list.empty() && !unreadable.empty()) {
        std::string text;
        for (unreadable_entry const& u : unreadable) text += "\n  " + u.label() + ": " + u.reason;
        return fail("the index has no version of recipe " + in_quotes(id.str()) + " that this otok can read; a newer otok may read them:" + text);
    }
    if (list.empty()) return fail("the index has no versions of recipe " + in_quotes(id.str()));
    unreadable_.insert_or_assign(id.str(), std::move(unreadable));
    recipe_status status;
    if ((*files)->status) {
        auto s = parse_recipe_status(*(*files)->status, "recipes/" + id.str() + "/recipe-status.toml", diags, reading_);
        // a status a client cannot read only informs: left aside (index.md 5)
        if (!s && reading_ == index_reading::client) s.emplace();
        if (!s) return fail("the index has an invalid status of " + in_quotes(id.str()) + ":" + messages(diags));
        status = std::move(*s);
    }
    statuses_.emplace(id.str(), std::move(status));
    return &entries_.emplace(id.str(), std::move(list)).first->second;
}

result<std::vector<index_entry const*>> recipe_index::newest_first(recipe_id const& id)
{
    auto all = entries(id);
    if (!all) return std::unexpected{all.error()};
    std::vector<index_entry const*> list;
    for (index_entry const& e : **all) list.push_back(&e);
    auto o = order(list.front()->projection.library);
    if (!o) return std::unexpected{o.error()};
    std::ranges::stable_sort(list, [&](index_entry const* a, index_entry const* b) {
        auto const cmp = o->compare(*a->projection.version, *b->projection.version);
        if (cmp != std::partial_ordering::equivalent) return cmp == std::partial_ordering::greater;
        if (a->projection.variant != b->projection.variant) return a->projection.variant < b->projection.variant;
        return a->projection.revision > b->projection.revision;
    });
    return list;
}

result<version_order> recipe_index::order(std::string_view library)
{
    if (auto it = orders_.find(library); it != orders_.end()) return it->second;
    auto content = source_->library(library);
    if (!content) return std::unexpected{content.error()};
    if (!*content) return orders_.emplace(std::string{library}, version_order::dotted()).first->second;
    diagnostics diags;
    auto order = parse_library_file(**content, "libraries/" + std::string{library} + ".toml", diags, reading_);
    if (!order) return fail("the index's library file of " + in_quotes(library) + " is invalid:" + messages(diags));
    return orders_.emplace(std::string{library}, std::move(*order)).first->second;
}

result<std::vector<advisory> const*> recipe_index::advisories()
{
    if (advisories_) return &*advisories_;
    auto files = source_->advisories();
    if (!files) return std::unexpected{files.error()};
    std::vector<advisory> list;
    diagnostics diags;
    for (index_file const& file : *files) {
        std::string const display = "advisories/" + file.file;
        diagnostics one;
        diagnostics& d = reading_ == index_reading::client ? one : diags;
        auto a = parse_advisory(file.text, display, d, reading_);
        if (a && a->id + ".toml" != file.file) d.error({display}, "the advisory's id " + in_quotes(a->id) + " does not match its file name");
        else if (a) list.push_back(std::move(*a));
        if (reading_ == index_reading::client && one.has_errors()) {
            // what it concerns is unknown: warned of, and an error of audit (index.md 7.2)
            std::string reason = "it cannot be read";
            for (diagnostic const& item : one.items()) {
                if (item.level == severity::error) {
                    reason = item.message;
                    break;
                }
            }
            unreadable_advisories_.push_back(display + ": " + reason);
        }
    }
    if (diags.has_errors()) return fail("the index has invalid advisories:" + messages(diags));
    advisories_ = std::move(list);
    return &*advisories_;
}

result<std::vector<candidate_advisory>> recipe_index::advisories_of(recipe_id const& id, std::string_view variant, std::string_view library,
    version const& v, std::int64_t revision)
{
    auto all = advisories();
    if (!all) return std::unexpected{all.error()};
    auto o = order(library);
    if (!o) return std::unexpected{o.error()};
    std::vector<candidate_advisory> found;
    for (advisory const& a : **all) {
        if (a.active() && advisory_concerns(a, id, variant, library, v, revision, *o)) found.push_back({a.id, a.severity, a.summary, a.unknown});
    }
    return found;
}

std::vector<unreadable_entry> const& recipe_index::unreadable(recipe_id const& id) const
{
    static std::vector<unreadable_entry> const none;
    auto const it = unreadable_.find(id.str());
    return it == unreadable_.end() ? none : it->second;
}

result<recipe_status> recipe_index::status(recipe_id const& id)
{
    if (auto e = entries(id); !e) return std::unexpected{e.error()};
    return statuses_.find(id.str())->second;
}

result<input_availability const*> recipe_index::availability()
{
    if (!availability_) {
        auto content = source_->availability();
        if (!content) return std::unexpected{content.error()};
        if (!*content) {
            availability_.emplace();
        } else {
            diagnostics diags;
            auto a = parse_availability(**content, "availability.toml", diags, reading_);
            if (!a) return fail("the index has an invalid availability.toml:" + messages(diags));
            availability_ = std::move(*a);
        }
    }
    return &*availability_;
}

result<std::vector<recipe_id>> recipe_index::recipe_ids()
{
    return source_->recipe_ids();
}

result<std::vector<recipe_summary>> recipe_index::summaries()
{
    auto given = source_->summaries();
    if (!given) return std::unexpected{given.error()};
    if (*given) return std::move(**given);
    auto ids = recipe_ids();
    if (!ids) return std::unexpected{ids.error()};
    std::vector<recipe_summary> list;
    for (recipe_id const& id : *ids) {
        auto sorted = newest_first(id);
        if (!sorted) return std::unexpected{sorted.error()};
        recipe const& r = newest_stable(*sorted)->projection;
        list.push_back({id, r.library, r.description, r.keywords, r.version->str(), version_targets(*sorted, *r.version, targets_)});
    }
    return list;
}

result<std::filesystem::path> index_checkout(std::string const& url, std::string const& mirror, cache_store& cache, git_tool const* git)
{
    std::error_code ec;
    if (!mirror.empty()) {
        std::filesystem::path const dir = path_from_utf8(mirror);
        if (std::filesystem::is_directory(dir, ec)) return dir;
        return fail("index-mirror " + in_quotes(mirror) + " is not a directory");
    }
    if (url.empty()) return fail("no recipe index: 'index' in config.toml (or OTOK_INDEX) is empty; set it to the view of an index, view+https://<base>, or the URL of its git repository");
    if (!is_repository_url(url)) return fail("index " + in_quotes(url) + " must be the https URL of the index's git repository");
    if (!git) return fail("otok needs git to read the index " + in_quotes(url) + ", and git is not in PATH");

    std::filesystem::path const state = cache.root() / "index" / git_url_key(url);
    std::string commit;
    if (cache.offline()) {
        auto text = read_file(state);
        if (!text) return fail("offline, and the index " + in_quotes(url) + " has not been read on this machine yet");
        commit = text->substr(0, text->find('\n'));
        if (!is_git_commit(commit)) return fail("'" + path_to_utf8(state) + "' does not hold a commit; read the index online once");
    } else {
        auto head = run_git(*git, {"ls-remote", url, "HEAD"}, cache.tmp(), cache.tmp() / "index-head.log", true);
        if (!head) return fail("cannot read the index " + in_quotes(url) + ": " + head.error().message);
        commit = head->substr(0, head->find_first_of("\t \n"));
        if (!is_git_commit(commit)) return fail("the index " + in_quotes(url) + " has no HEAD commit");
        auto old = read_file(state);
        if (!old || old->substr(0, old->find('\n')) != commit) {
            std::filesystem::create_directories(state.parent_path(), ec);
            if (auto ok = write_file_atomic(state, commit + "\n"); !ok) return std::unexpected{ok.error()};
        }
    }
    auto tree = cache.git_tree(url, commit, "", false, *git);
    if (!tree) return fail("cannot read the index " + in_quotes(url) + ": " + tree.error().message);
    return std::move(*tree);
}

std::string index_label(std::string_view name)
{
    return name == "default" ? std::string{"the index"} : "the index " + in_quotes(name);
}

index_set::index_set(std::vector<project_index> indexes, std::map<std::string, std::string, std::less<>> index_of, std::string none,
    std::span<target_info const* const> targets)
    : indexes_{std::move(indexes)}, index_of_{std::move(index_of)}, none_{std::move(none)}, targets_{targets.begin(), targets.end()}
{
}

std::vector<std::string> index_set::names() const
{
    std::vector<std::string> out;
    for (project_index const& i : indexes_) out.push_back(i.name);
    return out;
}

std::vector<std::pair<std::string, recipe_index*>> index_set::opened()
{
    std::vector<std::pair<std::string, recipe_index*>> out;
    for (auto& [name, index] : opened_) {
        if (index && *index) out.emplace_back(name, &**index);
    }
    return out;
}

result<recipe_index*> index_set::get(std::string_view name)
{
    auto const declared = std::ranges::find(indexes_, name, &project_index::name);
    if (declared == indexes_.end()) {
        if (name == "default" && !none_.empty()) return fail(none_);
        return fail("the project has no index " + in_quotes(name));
    }
    auto it = opened_.find(name);
    if (it == opened_.end()) it = opened_.emplace(std::string{name}, std::nullopt).first;
    if (!it->second) {
        auto source = declared->open();
        if (source) it->second.emplace(recipe_index{std::move(*source), targets_});
        else if (name == "default") it->second.emplace(std::unexpected{source.error()});
        else it->second.emplace(fail(index_label(name) + ": " + source.error().message));
    }
    if (!*it->second) return std::unexpected{it->second->error()};
    return &**it->second;
}

result<index_set::located> index_set::locate(recipe_id const& id)
{
    // [index-of], then the owner of the publisher: that index alone (otok-toml.md 6.4)
    std::string chosen;
    if (auto it = index_of_.find(id.str()); it != index_of_.end()) chosen = it->second;
    for (project_index const& i : indexes_) {
        if (chosen.empty() && std::ranges::find(i.publishers, id.publisher) != i.publishers.end()) chosen = i.name;
    }
    if (!chosen.empty()) {
        auto idx = get(chosen);
        if (!idx) return std::unexpected{idx.error()};
        auto found = (*idx)->has(id);
        if (!found) return std::unexpected{found.error()};
        if (!*found) return fail(index_label(chosen) + " has no recipe " + in_quotes(id.str()));
        return located{chosen, *idx};
    }
    if (indexes_.empty()) return fail(none_.empty() ? std::string{"the project has no index"} : none_);
    // every index; one that has it, never two
    std::vector<located> holding;
    for (project_index const& i : indexes_) {
        auto idx = get(i.name);
        if (!idx) return std::unexpected{idx.error()};
        auto found = (*idx)->has(id);
        if (!found) return std::unexpected{found.error()};
        if (*found) holding.push_back({i.name, *idx});
    }
    if (holding.empty()) {
        if (indexes_.size() == 1) return fail(index_label(indexes_.front().name) + " has no recipe " + in_quotes(id.str()));
        return fail("no index of the project has the recipe " + in_quotes(id.str()));
    }
    if (holding.size() > 1) {
        std::string names;
        for (located const& l : holding) names += (names.empty() ? "" : ", ") + in_quotes(l.name);
        return fail("the recipe " + in_quotes(id.str()) + " is in the indexes " + names + "; choose one in [index-of] of otok.toml, as \""
            + id.str() + "\" = " + in_quotes(holding.front().name) + " (otok-toml.md 6.4)");
    }
    return holding.front();
}

index_recipe_provider::index_recipe_provider(recipe_provider& local, std::shared_ptr<index_set> indexes, std::span<target_info const* const> targets,
    cache_store* cache, std::optional<git_tool> git)
    : local_{local}, indexes_{std::move(indexes)}, targets_{targets.begin(), targets.end()}, cache_{cache}, git_{std::move(git)}
{
}

result<std::vector<candidate>> index_recipe_provider::candidates(std::string_view library, recipe_ref const& ref)
{
    if (ref.source != recipe_ref::kind::index) return local_.candidates(library, ref);
    auto where = indexes_->locate(ref.id);
    if (!where) return fail("the recipe " + in_quotes(ref.id.str()) + " of " + in_quotes(library) + " needs the index: " + where.error().message);
    recipe_index* idx = where->index;
    auto entries = idx->entries(ref.id);
    if (!entries) return std::unexpected{entries.error()};
    from_index_.insert_or_assign(std::string{library}, where->name);
    ids_.insert_or_assign(std::string{library}, ref.id);
    auto status = idx->status(ref.id);
    if (!status) return std::unexpected{status.error()};

    std::vector<candidate> list;
    for (index_entry const& e : **entries) {
        candidate c;
        auto found = idx->advisories_of(e.id, e.projection.variant, e.projection.library, *e.projection.version, e.projection.revision);
        if (!found) return std::unexpected{found.error()};
        c.advisories = std::move(*found);
        c.deprecation = status->deprecation();
        c.recipe = std::make_shared<recipe const>(e.projection);
        c.kind = locked_package::recipe_kind::index;
        c.id = e.id;
        c.index = where->name == "default" ? std::string{} : where->name;
        c.recipe_source = e.recipe_source;
        c.recipe_sha256 = e.recipe_sha256;
        tables_.emplace(projection_key(e.id, e.projection), e.table);
        list.push_back(std::move(c));
    }
    return list;
}

std::vector<std::string> index_recipe_provider::unreadable(std::string_view library)
{
    auto const index = from_index_.find(library);
    auto const id = ids_.find(library);
    if (index == from_index_.end() || id == ids_.end()) return local_.unreadable(library);
    auto idx = indexes_->get(index->second);
    if (!idx) return {};
    std::vector<std::string> out;
    for (unreadable_entry const& u : (*idx)->unreadable(id->second)) out.push_back(u.label() + ": " + u.reason);
    return out;
}

version_order index_recipe_provider::order(std::string_view library)
{
    if (auto it = from_index_.find(library); it != from_index_.end()) {
        if (auto idx = indexes_->get(it->second)) {
            if (auto o = (*idx)->order(library)) return std::move(*o);
        }
    }
    return local_.order(library);
}

result<std::filesystem::path> fetch_recipe_tree(std::string const& label, std::string const& library, locked_location const& recipe_source,
    std::string const& recipe_sha256, cache_store& cache, git_tool const& git)
{
    auto tree = cache.git_directory(recipe_source.git, recipe_source.commit, recipe_source.tag, recipe_source.path, git, library);
    if (!tree) return fail("cannot fetch the recipe " + label + ": " + tree.error().message);
    auto hash = tree_hash(*tree);
    if (!hash) return fail("the recipe " + label + ": " + hash.error().message);
    if (*hash != recipe_sha256) {
        return fail("the recipe tree of " + label + " at " + recipe_source.git + " " + recipe_source.commit
            + " does not match the recipe-sha256 of its index entry");
    }
    return std::move(*tree);
}

result<void> index_recipe_provider::prepare(candidate& c)
{
    if (c.kind != locked_package::recipe_kind::index || !c.dir.empty()) return local_.prepare(c);
    std::string const label = c.id.str() + " " + c.recipe->version->str() + " (r" + std::to_string(c.recipe->revision) + ")";
    auto const table = tables_.find(projection_key(c.id, *c.recipe));
    if (table == tables_.end()) return fail("the recipe " + label + " is not an entry of the index");
    if (!cache_ || !git_) return fail("otok needs git and the cache for the recipe " + label);

    auto tree = fetch_recipe_tree(label, c.recipe->library, c.recipe_source, c.recipe_sha256, *cache_, *git_);
    if (!tree) return std::unexpected{tree.error()};
    std::filesystem::path const dir = *tree;

    diagnostics diags;
    auto full = load_recipe(dir, targets_, diags, recipe_names::open);
    if (!full) return fail("the recipe " + label + " is invalid:" + messages(diags));
    auto content = read_file(dir / "recipe.toml");
    if (!content) return std::unexpected{content.error()};
    auto doc = toml_document::parse(*content, "recipe.toml", diags);
    if (!doc || recipe_projection(doc->table(), c.id.publisher, targets_) != *table->second) {
        return fail("the index entry of " + label + " does not match its recipe.toml (index.md 3.1)");
    }
    c.recipe = std::make_shared<recipe const>(std::move(*full));
    c.dir = dir;
    return {};
}

}
