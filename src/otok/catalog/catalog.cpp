// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/catalog/catalog.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// C4702 (unreachable code) comes from inja's parser when MSVC generates its code, past /external:W0
#ifdef _MSC_VER
#   pragma warning(push)
#   pragma warning(disable: 4702)
#endif
#include <inja/inja.hpp>
#ifdef _MSC_VER
#   pragma warning(pop)
#endif
#include <nlohmann/json.hpp>

#include "otok/core/diagnostics.hpp"
#include "otok/core/sha256.hpp"
#include "otok/core/tree_hash.hpp"
#include "otok/index/index.hpp"
#include "otok/recipe/recipe.hpp"

namespace otok {

namespace {

using json = nlohmann::json;

std::string date_text(toml::date const& d)
{
    char text[16];
    std::snprintf(text, sizeof text, "%04u-%02u-%02u", unsigned{d.year}, unsigned{d.month}, unsigned{d.day});
    return text;
}

// The date of an entry's publication, or null for an entry merged without one (index.md 3).
json published_date(std::optional<toml::date_time> const& published)
{
    return published ? json(date_text(published->date)) : json{};
}

std::string toml_string(std::string_view s)
{
    std::string out = "\"";
    for (char const c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
                char escaped[8];
                std::snprintf(escaped, sizeof escaped, "\\u%04x", unsigned{static_cast<unsigned char>(c)});
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string toml_key(std::string_view key)
{
    bool const bare = !key.empty() && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    return bare ? std::string{key} : toml_string(key);
}

// A value as inline TOML, as a recipe writes it: `false`, `"x"`, `["a", "b"]`, `{ option = "zlib" }`.
std::string inline_toml(toml::node const& n)
{
    if (auto const* s = n.as_string()) return toml_string(s->get());
    if (auto const* a = n.as_array()) {
        std::string out = "[";
        for (std::size_t i = 0; i < a->size(); ++i) out += (i ? ", " : "") + inline_toml(*a->get(i));
        return out + "]";
    }
    if (auto const* t = n.as_table()) {
        if (t->empty()) return "{}";
        std::string out = "{ ";
        bool first = true;
        for (auto const& [key, value] : *t) {
            out += (first ? "" : ", ") + toml_key(key.str()) + " = " + inline_toml(value);
            first = false;
        }
        return out + " }";
    }
    std::ostringstream os;
    n.visit([&os](auto const& v) { os << v; });
    return os.str();
}

// A text a template tests with `if`: inja takes every string for true, so an empty one is null.
json text_or_null(std::string text)
{
    return text.empty() ? json{} : json(std::move(text));
}

// A time as Atom writes it (RFC 3339); one without an offset is taken for UTC.
std::string rfc3339(toml::date_time const& t)
{
    char text[40];
    std::snprintf(text, sizeof text, "%04u-%02u-%02uT%02u:%02u:%02u", unsigned{t.date.year}, unsigned{t.date.month}, unsigned{t.date.day},
        unsigned{t.time.hour}, unsigned{t.time.minute}, unsigned{t.time.second});
    std::string out = text;
    int const minutes = t.offset ? t.offset->minutes : 0;
    if (minutes == 0) return out + "Z";
    std::snprintf(text, sizeof text, "%c%02d:%02d", minutes < 0 ? '-' : '+', std::abs(minutes) / 60, std::abs(minutes) % 60);
    return out + text;
}

// What a reviewer of an update compares besides the files (DESIGN 13 point 4): the entry's keys, tables split one
// level (`source.sha256`, `options.zlib`, `deps.zlib`), resources by name; values as inline TOML. The
// version, its revision and the texts of the catalog are left out.
std::map<std::string, std::string> input_rows(toml::table const& entry)
{
    std::map<std::string, std::string> rows;
    for (auto const& [key, value] : entry) {
        std::string const k{key.str()};
        if (k == "format" || k == "recipe" || k == "version" || k == "revision" || k == "description" || k == "keywords") continue;
        if (toml::array const* list = value.as_array(); list && k == "resource") {
            for (toml::node const& r : *list) {
                toml::table const* resource = r.as_table();
                if (!resource) continue;
                std::string const name = (*resource)["name"].value_or(std::string{});
                for (auto const& [rk, rv] : *resource) {
                    if (rk.str() != "name") rows["resource." + name + "." + std::string{rk.str()}] = inline_toml(rv);
                }
            }
        } else if (toml::table const* t = value.as_table()) {
            for (auto const& [sk, sv] : *t) rows[k + "." + toml_key(sk.str())] = inline_toml(sv);
        } else {
            rows[k] = inline_toml(value);
        }
    }
    return rows;
}

// The ref of the upstream source of an entry to compare on its host: the tag, else the commit; empty for an archive.
std::string upstream_ref(toml::table const& entry)
{
    toml::table const* source = entry["source"].as_table();
    if (!source || !(*source)["git"].is_string()) return {};
    if (auto tag = (*source)["tag"].value<std::string>()) return *tag;
    return (*source)["commit"].value_or(std::string{});
}

// A string as itself, anything else as inline TOML.
std::string value_text(toml::node const& n)
{
    if (auto const* s = n.as_string()) return s->get();
    return inline_toml(n);
}

// Valid UTF-8 without NUL: shown as text; other files as their hash.
bool is_text(std::string_view s)
{
    for (std::size_t i = 0; i < s.size();) {
        unsigned char const c = static_cast<unsigned char>(s[i]);
        if (c == 0) return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t const more = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
        if (more == 0 || s.size() - i <= more) return false;
        for (std::size_t k = 1; k <= more; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) return false;
        }
        i += more + 1;
    }
    return true;
}

// `https://github.com/<owner>/<repository>` of a GitHub URL, without `.git`; empty for other hosts.
std::string github_repository(std::string url)
{
    constexpr std::string_view prefix = "https://github.com/";
    if (!url.starts_with(prefix)) return {};
    while (url.ends_with('/')) url.pop_back();
    if (url.ends_with(".git")) url.resize(url.size() - 4);
    if (std::ranges::count(url.substr(prefix.size()), '/') != 1) return {};
    return url;
}

// A URL of the recipe's author to link to: http and https only, so a page never carries a `javascript:` link.
std::string web_link(std::string const& url)
{
    return url.starts_with("https://") || url.starts_with("http://") ? url : std::string{};
}

std::string label_of(index_entry const& e)
{
    return e.projection.version->str() + " (r" + std::to_string(e.projection.revision) + ", " + e.projection.variant + ")";
}

// "1.0-r1": the version and revision, as availability.toml names them (index.md 11)
std::string version_of(index_entry const& e)
{
    return e.projection.version->str() + "-r" + std::to_string(e.projection.revision);
}

// "apple/1.0-r1": the page of an entry in its recipe's directory, as its file in the index (index.md 3)
std::string directory_of(index_entry const& e)
{
    return e.projection.variant + "/" + version_of(e);
}

std::string channel_of(index_entry const& e)
{
    return (*e.table)["channel"].value_or(std::string{"stable"});
}

std::string_view status_name(advisory_status s) noexcept
{
    switch (s) {
    case advisory_status::under_review: return "under-review";
    case advisory_status::confirmed: return "confirmed";
    case advisory_status::rejected: return "rejected";
    }
    return "";
}

// The `[deps.<library>]` a project writes for the recipe (otok-toml.md 4).
std::string snippet_of(index_entry const& e)
{
    std::string const& v = e.projection.version->str();
    bool const numbered = !v.empty() && v.back() >= '0' && v.back() <= '9';  // `~>` increments the last part
    return "[deps." + toml_key(e.projection.library) + "]\nrecipe = " + toml_string(e.id.str()) + "\nversion = "
        + toml_string((numbered ? "~> " : ">= ") + v) + "\n";
}

// A recipe tree of an entry: its files (stored by content in `files/`) and the recipe read from it.
struct entry_tree
{
    json files = json::array();        // [{ path, size, sha256, file, text, binary }]
    std::map<std::string, std::string> shas;  // path → sha256
    std::string error;                 // why it is not shown
    std::optional<recipe> full;
};

class catalog_writer
{
public:
    explicit catalog_writer(catalog_settings const& s) : s_{s}, index_{s.root, s.targets, index_reading::ci} {}

    result<catalog_written> run()
    {
        if (auto ok = load_templates(); !ok) return std::unexpected{ok.error()};
        // the pages name each asset with its hash: caches (a CDN's for hours) never pair a page with an older script
        for (embedded_file const& f : catalog_site_files()) {
            if (!f.path.starts_with("assets/")) continue;
            std::string name{f.path.substr(7)};
            std::ranges::replace(name, '.', '_');      // catalog.js → catalog_js
            assets_[name] = std::string{f.path} + "?v=" + sha256_hex(f.contents).substr(0, 12);
        }
        auto settings = index_.settings();
        if (!settings) return std::unexpected{settings.error()};
        site_["name"] = settings->name.empty() ? "otok index" : settings->name;
        auto advisories = index_.advisories();
        if (!advisories) return std::unexpected{advisories.error()};
        advisories_ = *advisories;
        auto availability = index_.availability();
        if (!availability) return std::unexpected{availability.error()};
        availability_ = *availability;

        catalog_ = settings->catalog;
        while (catalog_.ends_with('/')) catalog_.pop_back();
        repository_ = settings->repository;

        auto ids = index_.recipe_ids();
        if (!ids) return std::unexpected{ids.error()};
        nlohmann::ordered_json search = nlohmann::ordered_json::array();
        for (recipe_id const& id : *ids) {
            if (auto ok = recipe_pages(id, search); !ok) return std::unexpected{ok.error()};
        }
        recipes_ = ids->size();
        files_["search.json"] = search.dump() + "\n";

        std::error_code ec;
        if (std::filesystem::is_directory(s_.root / "libraries", ec)) {
            for (auto const& item : std::filesystem::directory_iterator{s_.root / "libraries", ec}) {
                if (item.path().extension() == ".toml") libraries_[path_to_utf8(item.path().stem())];
            }
        }
        for (auto& [library, rows] : libraries_) {
            if (rows.is_null()) rows = json::array();
            json page = page_of(2, library);
            page["library"] = library;
            page["recipes"] = rows;  // /libraries/ counts them later
            if (auto ok = render("library.html", "libraries/" + library + "/index.html", page); !ok) return std::unexpected{ok.error()};
        }

        if (auto ok = front_pages(); !ok) return std::unexpected{ok.error()};
        if (auto ok = advisories_page(); !ok) return std::unexpected{ok.error()};

        for (embedded_file const& f : catalog_site_files()) {
            if (f.path.starts_with("assets/")) files_[std::string{f.path}] = std::string{f.contents};
        }
        std::vector<std::string> const owned = {"index.html", "search.json", "advisories.atom", "libraries", "recipes", "advisories", "files", "assets"};
        auto written = write_generated_files(s_.output, files_, owned);
        if (!written) return std::unexpected{written.error()};
        done_.files = *written;
        return std::move(done_);
    }

private:
    result<void> load_templates()
    {
        env_.set_html_autoescape(true);
        env_.set_search_included_templates_in_files(false);
        env_.set_throw_at_missing_includes(true);
        env_.set_trim_blocks(true);
        constexpr std::string_view prefix = "templates/";
        try {
            // the base template and the parts first: the pages extend and include them
            for (bool const base : {true, false}) {
                for (embedded_file const& f : catalog_site_files()) {
                    if (!f.path.starts_with(prefix)) continue;
                    std::string const name{f.path.substr(prefix.size())};
                    if ((name == "base.html" || name.starts_with('_')) != base) continue;
                    inja::Template t = env_.parse(f.contents);
                    env_.include_template(name, t);
                    templates_.emplace(name, std::move(t));
                }
            }
        } catch (std::exception const& e) {
            return fail(std::string{"a template of the catalog is invalid: "} + e.what());
        }
        return {};
    }

    // The members every page has; `depth` is the number of directories it is under.
    json page_of(std::size_t depth, std::string const& title) const
    {
        json page = json::object();
        page["site"] = site_;
        page["assets"] = assets_;
        page["title"] = title;
        std::string root;
        for (std::size_t i = 0; i < depth; ++i) root += "../";
        page["root"] = root;
        return page;
    }

    result<void> render(std::string const& name, std::string const& path, json const& page)
    {
        try {
            files_[path] = env_.render(templates_.at(name), page);
        } catch (std::exception const& e) {
            return fail("cannot render " + path + " with the template " + name + ": " + e.what());
        }
        return {};
    }

    entry_tree tree_of(index_entry const& e)
    {
        entry_tree tree;
        std::string const label = e.id.str() + " " + label_of(e);
        // the page says the same on every run; the reason, which may name the paths of this run, is a warning
        auto const unavailable = [&](std::string why) {
            done_.unavailable.push_back(std::move(why));
            tree.files = json::array();
            tree.shas.clear();
            tree.error = "it cannot be fetched from " + e.recipe_source.git + " at " + e.recipe_source.commit
                + ", or it does not match the recipe-sha256 of its entry";
            return std::move(tree);
        };
        if (!s_.cache || !s_.git) return unavailable("otok needs git and the cache for the recipe " + label);
        auto dir = fetch_recipe_tree(label, e.projection.library, e.recipe_source, e.recipe_sha256, *s_.cache, *s_.git);
        if (!dir) return unavailable(dir.error().message);
        auto listed = list_tree(*dir);
        if (!listed) return unavailable("the recipe " + label + ": " + listed.error().message);
        for (tree_file const& f : *listed) {
            auto contents = read_file(*dir / path_from_utf8(f.path));
            if (!contents) return unavailable("the recipe " + label + ": " + contents.error().message);
            json item = json::object();
            item["path"] = f.path;
            item["size"] = f.size;
            item["sha256"] = f.sha256;
            item["file"] = "files/" + f.sha256 + ".txt";
            bool const text = is_text(*contents);
            item["text"] = text ? *contents : std::string{};
            item["binary"] = !text;
            tree.shas[f.path] = f.sha256;
            files_["files/" + f.sha256 + ".txt"] = std::move(*contents);
            tree.files.push_back(std::move(item));
        }
        diagnostics diags;
        tree.full = load_recipe(*dir, s_.targets, diags);
        return tree;
    }

    // The active advisories of an entry, and whether one withdraws it.
    result<std::pair<json, bool>> advisories_of(index_entry const& e)
    {
        auto order = index_.order(e.projection.library);
        if (!order) return std::unexpected{order.error()};
        json list = json::array();
        bool withdrawn = false;
        for (advisory const& a : *advisories_) {
            if (!a.active() || !advisory_concerns(a, e.id, e.projection.variant, e.projection.library, *e.projection.version, e.projection.revision, *order)) continue;
            json item = json::object();
            item["id"] = a.id;
            item["severity"] = severity_name(a.severity);
            item["summary"] = a.summary;
            list.push_back(std::move(item));
            withdrawn = withdrawn || a.severity == advisory_severity::withdrawn;
        }
        return std::pair{std::move(list), withdrawn};
    }

    // `{ key, value }` rows of a table of the entry (`source`, a resource), in the order of the keys.
    static json rows_of(toml::table const& t, std::initializer_list<std::string_view> skip = {})
    {
        json rows = json::array();
        for (auto const& [key, value] : t) {
            if (std::ranges::find(skip, key.str()) != skip.end()) continue;
            json row = json::object();
            row["key"] = key.str();
            row["value"] = value_text(value);
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // The members of the entry page that the recipe page shows for its newest stable version too.
    static void describe_contents(json& page, index_entry const& e, entry_tree const& tree)
    {
        toml::table const& t = *e.table;
        std::map<std::string, std::string, std::less<>> docs;
        if (tree.full) {
            for (option_schema const& o : tree.full->options) docs[o.name] = o.doc;
        }
        json options = json::array();
        if (toml::table const* list = t["options"].as_table()) {
            for (auto const& [name, value] : *list) {
                toml::table const* o = value.as_table();
                if (!o) continue;
                json item = json::object();
                item["name"] = name.str();
                item["type"] = (*o)["type"].value_or(std::string{});
                item["default"] = (*o)["default"].node() ? inline_toml(*(*o)["default"].node()) : "no file";  // only a file option has none
                item["values"] = text_or_null((*o)["values"].node() ? inline_toml(*(*o)["values"].node()) : "");
                item["additive"] = (*o)["additive"].value_or(false);
                auto const doc = docs.find(name.str());
                item["doc"] = doc == docs.end() ? "" : doc->second;
                options.push_back(std::move(item));
            }
        }
        page["options"] = std::move(options);
        page["platforms"] = e.projection.platforms;
        json groups = json::array();
        for (auto const& [key, title] : {std::pair{"deps", "Dependencies"}, std::pair{"tool-deps", "Tool dependencies"}}) {
            json list = json::array();
            if (toml::table const* deps = t[key].as_table()) {
                for (auto const& [library, value] : *deps) {
                    toml::table const* d = value.as_table();
                    if (!d) continue;
                    json item = json::object();
                    item["library"] = library.str();
                    item["version"] = (*d)["version"].value_or(std::string{});
                    item["recipe"] = text_or_null((*d)["default-recipe"].value_or(std::string{}));
                    item["details"] = rows_of(*d, {"version", "default-recipe"});
                    list.push_back(std::move(item));
                }
            }
            if (list.empty()) continue;
            json group = json::object();
            group["title"] = title;
            group["items"] = std::move(list);
            groups.push_back(std::move(group));
        }
        page["dependency_groups"] = std::move(groups);
        json toolchains = json::array();
        if (toml::table const* list = t["toolchains"].as_table()) toolchains = rows_of(*list);
        page["toolchains"] = std::move(toolchains);
        page["files"] = tree.files;
        page["tree_error"] = text_or_null(tree.error);
    }

    // The pages of a recipe and its entries, and its entries.json. Adds its library's row, its search item and its versions to the recent ones.
    result<void> recipe_pages(recipe_id const& id, nlohmann::ordered_json& search)
    {
        auto sorted = index_.newest_first(id);
        if (!sorted) return std::unexpected{sorted.error()};
        auto status = index_.status(id);
        if (!status) return std::unexpected{status.error()};
        index_entry const& newest = *newest_stable(*sorted);
        std::string const dir = "recipes/" + id.str() + "/";

        // the trees first: each page compares its version with the previous one
        std::vector<entry_tree> trees;
        nlohmann::ordered_json listed = nlohmann::ordered_json::array();  // entries.json, for the diffs in the browser
        for (index_entry const* e : *sorted) {
            trees.push_back(tree_of(*e));
            nlohmann::ordered_json item = nlohmann::ordered_json::object();
            item["dir"] = directory_of(*e);
            item["label"] = label_of(*e);
            if (trees.back().error.empty()) item["files"] = trees.back().shas;
            else item["files"] = nullptr;
            listed.push_back(std::move(item));
        }
        nlohmann::ordered_json entries_file = nlohmann::ordered_json::object();
        entries_file["entries"] = std::move(listed);
        files_[dir + "entries.json"] = entries_file.dump() + "\n";

        json versions = json::array();
        json recipe_page = page_of(3, id.str());
        std::size_t newest_advisories = 0;
        for (std::size_t i = 0; i < sorted->size(); ++i) {
            index_entry const& e = *(*sorted)[i];
            auto advisories = advisories_of(e);
            if (!advisories) return std::unexpected{advisories.error()};
            unavailable_version const* gone = availability_->find(id, version_of(e));
            entry_tree const& tree = trees[i];
            if (e.published) {
                recent_.push_back({rfc3339(*e.published), id.str(), label_of(e), directory_of(e), e.projection.library, date_text(e.published->date),
                    target_of_entry(e.projection, s_.targets)});
            }

            // the entry before it: the newest older one of its variant, whose revisions are its own (index.md 3)
            std::size_t before = i + 1;
            while (before < sorted->size() && (*sorted)[before]->projection.variant != e.projection.variant) ++before;
            json page = page_of(5, id.str() + " " + label_of(e));
            page["id"] = id.str();
            page["dir"] = directory_of(e);
            page["entries"] = "../../entries.json";
            page["others"] = sorted->size() > 1;
            page["compare"] = before < sorted->size() ? compare(e, tree, *(*sorted)[before], trees[before]) : json{};
            page["variant"] = e.projection.variant;
            page["target"] = text_or_null(target_of_entry(e.projection, s_.targets));
            page["library"] = e.projection.library;
            page["label"] = label_of(e);
            page["version"] = e.projection.version->str();
            page["revision"] = e.projection.revision;
            page["channel"] = channel_of(e);
            page["published"] = published_date(e.published);
            json source = json::object();
            source["git"] = e.recipe_source.git;
            source["commit"] = e.recipe_source.commit;
            source["tag"] = text_or_null(e.recipe_source.tag);
            source["path"] = text_or_null(e.recipe_source.path);
            std::string const github = github_repository(e.recipe_source.git);
            source["browse"] = text_or_null(github.empty() ? "" : github + "/tree/" + e.recipe_source.commit + (e.recipe_source.path.empty() ? "" : "/" + e.recipe_source.path));
            page["recipe_source"] = std::move(source);
            page["recipe_sha256"] = e.recipe_sha256;
            toml::table const& t = *e.table;
            page["source"] = t["source"].as_table() ? rows_of(*t["source"].as_table()) : json::array();
            json resources = json::array();
            if (toml::array const* list = t["resource"].as_array()) {
                for (toml::node const& r : *list) {
                    toml::table const* res = r.as_table();
                    if (!res) continue;
                    json item = json::object();
                    item["name"] = (*res)["name"].value_or(std::string{});
                    item["rows"] = rows_of(*res, {"name"});
                    resources.push_back(std::move(item));
                }
            }
            page["resources"] = std::move(resources);
            page["advisories"] = advisories->first;
            page["withdrawn"] = advisories->second;
            json unavailable = nullptr;
            if (gone) {
                unavailable = json::object();
                unavailable["since"] = date_text(gone->since.date);
                unavailable["inputs"] = gone->inputs;
            }
            page["unavailable"] = unavailable;
            json previous = nullptr;
            if (before < sorted->size()) {
                index_entry const& p = *(*sorted)[before];
                previous = json::object();
                previous["label"] = label_of(p);
                previous["href"] = "../../" + directory_of(p) + "/";
            }
            page["previous"] = std::move(previous);
            describe_contents(page, e, tree);
            if (auto ok = render("entry.html", dir + directory_of(e) + "/index.html", page); !ok) return std::unexpected{ok.error()};

            json row = json::object();
            row["label"] = label_of(e);
            row["href"] = directory_of(e) + "/";
            row["target"] = text_or_null(target_of_entry(e.projection, s_.targets));
            row["channel"] = channel_of(e);
            row["published"] = published_date(e.published);
            row["withdrawn"] = advisories->second;
            row["advisories"] = advisories->first.size();
            row["unavailable"] = gone != nullptr;
            versions.push_back(std::move(row));

            if (&e == &newest) {
                newest_advisories = advisories->first.size();
                describe_contents(recipe_page, e, tree);
                recipe_page["homepage"] = text_or_null(tree.full ? web_link(tree.full->homepage) : "");
                recipe_page["documentation"] = text_or_null(tree.full ? web_link(tree.full->documentation) : "");
            }
        }

        std::string const deprecation = status->deprecation();
        recipe_page["id"] = id.str();
        recipe_page["library"] = newest.projection.library;
        recipe_page["description"] = newest.projection.description;
        recipe_page["license"] = text_or_null(newest.projection.license_text);
        recipe_page["keywords"] = newest.projection.keywords;
        recipe_page["deprecated"] = text_or_null(deprecation);
        recipe_page["in_favor_of"] = text_or_null(status->in_favor_of ? status->in_favor_of->str() : "");
        recipe_page["transferred_from"] = text_or_null(status->transferred_from ? status->transferred_from->str() : "");
        recipe_page["snippet"] = snippet_of(newest);
        recipe_page["newest"] = label_of(newest);
        recipe_page["newest_href"] = directory_of(newest) + "/";
        recipe_page["versions"] = std::move(versions);
        std::vector<std::string> const targets = version_targets(*sorted, *newest.projection.version, s_.targets);
        recipe_page["targets"] = targets;
        if (auto ok = render("recipe.html", dir + "index.html", recipe_page); !ok) return std::unexpected{ok.error()};

        json library_row = json::object();
        library_row["id"] = id.str();
        library_row["version"] = label_of(newest);
        library_row["published"] = published_date(newest.published);
        library_row["platforms"] = newest.projection.platforms;
        library_row["targets"] = targets;
        json options = json::array();
        for (option_schema const& o : newest.projection.options) options.push_back(o.name);
        library_row["options"] = std::move(options);
        library_row["advisories"] = newest_advisories;
        library_row["deprecated"] = text_or_null(deprecation);
        libraries_[newest.projection.library].push_back(std::move(library_row));

        nlohmann::ordered_json item = nlohmann::ordered_json::object();
        item["id"] = id.str();
        item["library"] = newest.projection.library;
        item["description"] = newest.projection.description;
        item["keywords"] = newest.projection.keywords;
        item["platforms"] = newest.projection.platforms;
        // each target's newest version, so that the front page shows what a filter by target keeps
        std::vector<std::string> item_targets;
        nlohmann::ordered_json variants = nlohmann::ordered_json::array();
        for (target_info const* t : s_.targets) {
            std::vector<index_entry const*> of_target;
            for (index_entry const* e : *sorted) {
                if (target_of_entry(e->projection, s_.targets) == t->name) of_target.push_back(e);
            }
            if (of_target.empty()) continue;
            index_entry const& v = *newest_stable(of_target);
            nlohmann::ordered_json variant = nlohmann::ordered_json::object();
            variant["target"] = t->name;
            variant["version"] = v.projection.version->str() + " (r" + std::to_string(v.projection.revision) + ")";
            variant["href"] = dir + directory_of(v) + "/";
            variants.push_back(std::move(variant));
            item_targets.push_back(t->name);
            targets_seen_.insert(t->name);
        }
        item["targets"] = item_targets;
        item["license"] = newest.projection.license_text;
        item["variants"] = std::move(variants);
        search.push_back(std::move(item));
        return {};
    }

    // The changes of `e` since `base`, the version before it: the inputs as data, the link to the upstream changes,
    // the recipe files added, removed and changed (null when a tree is unavailable).
    static json compare(index_entry const& e, entry_tree const& tree, index_entry const& base, entry_tree const& base_tree)
    {
        json c = json::object();
        c["label"] = label_of(base);
        c["dir"] = directory_of(base);
        std::map<std::string, std::string> const before = input_rows(*base.table);
        std::map<std::string, std::string> const after = input_rows(*e.table);
        std::set<std::string> keys;
        for (auto const& [key, value] : before) keys.insert(key);
        for (auto const& [key, value] : after) keys.insert(key);
        json inputs = json::array();
        for (std::string const& key : keys) {
            auto const old = before.find(key);
            auto const now = after.find(key);
            if (old != before.end() && now != after.end() && old->second == now->second) continue;
            json row = json::object();
            row["key"] = key;
            row["old"] = old == before.end() ? json{} : json(old->second);
            row["new"] = now == after.end() ? json{} : json(now->second);
            row["change"] = old == before.end() ? "added" : now == after.end() ? "removed" : "changed";
            inputs.push_back(std::move(row));
        }
        c["inputs"] = std::move(inputs);

        std::string upstream;
        toml::table const* from = (*base.table)["source"].as_table();
        toml::table const* to = (*e.table)["source"].as_table();
        std::string const old_ref = upstream_ref(*base.table);
        std::string const new_ref = upstream_ref(*e.table);
        if (from && to && !old_ref.empty() && !new_ref.empty() && old_ref != new_ref) {
            std::string const repository = github_repository((*to)["git"].value_or(std::string{}));
            if (!repository.empty() && repository == github_repository((*from)["git"].value_or(std::string{}))) {
                upstream = repository + "/compare/" + old_ref + "..." + new_ref;
            }
        }
        c["upstream"] = text_or_null(std::move(upstream));

        if (!tree.error.empty() || !base_tree.error.empty()) {
            c["files"] = nullptr;
            return c;
        }
        std::set<std::string> paths;
        for (auto const& [path, sha] : base_tree.shas) paths.insert(path);
        for (auto const& [path, sha] : tree.shas) paths.insert(path);
        json files = json::array();
        std::size_t same = 0;
        for (std::string const& path : paths) {
            auto const old = base_tree.shas.find(path);
            auto const now = tree.shas.find(path);
            if (old != base_tree.shas.end() && now != tree.shas.end() && old->second == now->second) {
                ++same;
                continue;
            }
            json f = json::object();
            f["path"] = path;
            f["change"] = old == base_tree.shas.end() ? "added" : now == tree.shas.end() ? "removed" : "changed";
            files.push_back(std::move(f));
        }
        c["files"] = std::move(files);
        c["same"] = same;
        return c;
    }

    // The front page: the search, the versions published last and the numbers; and the list of every library.
    result<void> front_pages()
    {
        std::ranges::sort(recent_, [](recent_version const& a, recent_version const& b) {
            return std::tie(b.time, a.id, a.dir) < std::tie(a.time, b.id, b.dir);  // the newest first
        });
        // the newest of all, shown at first, and the newest of each target, which the filter by target shows instead
        json recent = json::array();
        std::map<std::string, std::size_t> of_target;
        for (std::size_t i = 0; i < recent_.size(); ++i) {
            recent_version const& v = recent_[i];
            bool const newest = i < recent_count;
            bool const of_its_target = !v.target.empty() && of_target[v.target] < recent_count;
            if (!newest && !of_its_target) continue;
            if (!v.target.empty()) ++of_target[v.target];
            json r = json::object();
            r["id"] = v.id;
            r["label"] = v.label;
            r["dir"] = v.dir;
            r["library"] = v.library;
            r["published"] = v.date;
            r["target"] = text_or_null(v.target);
            r["newest"] = newest;
            recent.push_back(std::move(r));
        }
        json front = page_of(0, site_["name"].get<std::string>());
        front["recent"] = std::move(recent);
        front["libraries"] = libraries_.size();
        front["recipes"] = recipes_;
        front["targets"] = targets_seen_;
        front["commit"] = s_.commit;
        if (auto ok = render("search.html", "index.html", front); !ok) return ok;

        // A–Z: a group per first letter, the others under `#`
        json groups = json::array();
        for (auto const& [library, rows] : libraries_) {
            char const first = library.empty() ? '#' : library.front();
            std::string const letter = first >= 'a' && first <= 'z' ? std::string(1, static_cast<char>(first - 'a' + 'A'))
                : first >= 'A' && first <= 'Z' ? std::string(1, first) : "#";
            if (groups.empty() || groups.back()["letter"] != letter) {
                json g = json::object();
                g["letter"] = letter;
                g["anchor"] = letter == "#" ? "other" : letter;
                g["libraries"] = json::array();
                groups.push_back(std::move(g));
            }
            json l = json::object();
            l["name"] = library;
            l["recipes"] = rows.is_array() ? rows.size() : 0;
            groups.back()["libraries"].push_back(std::move(l));
        }
        json page = page_of(1, "Libraries");
        page["groups"] = std::move(groups);
        return render("libraries.html", "libraries/index.html", page);
    }

    // The advisories page and its Atom feed.
    result<void> advisories_page()
    {
        json list = json::array();
        std::string feed_updated = "1970-01-01T00:00:00Z";  // a feed without advisories
        for (auto it = advisories_->rbegin(); it != advisories_->rend(); ++it) {  // the newest first
            advisory const& a = *it;
            json item = json::object();
            item["id"] = a.id;
            item["severity"] = severity_name(a.severity);
            item["status"] = status_name(a.status);
            item["summary"] = a.summary;
            json recipes = json::array();
            for (recipe_id const& r : a.recipes) recipes.push_back(r.str());
            item["recipes"] = std::move(recipes);
            item["library"] = text_or_null(a.library);
            item["versions"] = a.versions;
            item["aliases"] = a.aliases;
            item["details"] = text_or_null(a.details);
            json references = json::array();
            for (std::string const& r : a.references) {
                if (!web_link(r).empty()) references.push_back(r);
            }
            item["references"] = std::move(references);
            toml::date_time const published = a.published.value_or(toml::date_time{});
            toml::date_time const updated = a.updated.value_or(published);
            item["published"] = date_text(published.date);
            item["updated"] = text_or_null(a.updated ? date_text(a.updated->date) : "");
            item["published_time"] = rfc3339(published);
            item["updated_time"] = rfc3339(updated);
            std::string concerns = a.library;  // "p/a 1.0-r1, 1.1"
            for (recipe_id const& r : a.recipes) concerns += (concerns.empty() ? "" : ", ") + r.str();
            for (std::size_t i = 0; i < a.versions.size(); ++i) concerns += (i ? ", " : " ") + a.versions[i];
            item["concerns"] = concerns;
            // Atom: ids that stay the same, from the catalog's URL when index.toml names it, else from the repository's
            item["entry_id"] = !catalog_.empty() ? catalog_ + "/advisories/#" + a.id
                : !repository_.empty() ? repository_ + "#advisory-" + a.id : "urn:otok:advisory:" + a.id;
            if (rfc3339(updated) > feed_updated) feed_updated = rfc3339(updated);
            list.push_back(std::move(item));
        }
        json page = page_of(1, "Advisories");
        page["advisories"] = list;
        if (auto ok = render("advisories.html", "advisories/index.html", page); !ok) return ok;

        json feed = page_of(0, "Advisories");
        feed["advisories"] = std::move(list);
        feed["base"] = catalog_.empty() ? "" : catalog_ + "/";
        feed["feed_id"] = !catalog_.empty() ? catalog_ + "/advisories.atom"
            : !repository_.empty() ? repository_ + "#advisories" : "urn:otok:advisories";
        feed["updated"] = feed_updated;
        return render("advisories.atom", "advisories.atom", feed);
    }

    catalog_settings const& s_;
    recipe_index index_;
    inja::Environment env_;
    std::map<std::string, inja::Template> templates_;
    json site_ = json::object();
    json assets_ = json::object();             // assets/style.css as style_css: its path with ?v=<hash>
    std::vector<advisory> const* advisories_ = nullptr;
    input_availability const* availability_ = nullptr;
    std::map<std::string, json> libraries_;     // library → its recipes' rows
    std::map<std::string, std::string> files_;  // path → contents
    catalog_written done_;
    std::string catalog_;                       // index.toml: the catalog's URL, the repository's
    std::string repository_;
    std::size_t recipes_ = 0;

    struct recent_version
    {
        std::string time;                       // RFC 3339, to sort
        std::string id;
        std::string label;
        std::string dir;
        std::string library;
        std::string date;
        std::string target;                     // empty when no target of this otok builds it
    };
    static constexpr std::size_t recent_count = 30;
    std::vector<recent_version> recent_;
    std::set<std::string> targets_seen_;  // of the newest version of each target of each recipe, for the filter of the front page
};

}

result<catalog_written> write_index_catalog(catalog_settings const& settings)
{
    catalog_writer writer{settings};
    return writer.run();
}

}
