// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/index/view.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "otok/core/fs.hpp"

namespace otok {

namespace {

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

// The members of a file of the view, checked as they are read.
class view_document
{
public:
    static result<view_document> parse(std::string const& text, std::string where)
    {
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return fail(where + " is not a JSON object");
        auto const format = j.find("view-format");
        if (format == j.end() || !format->is_number_integer()) return fail(where + " has no 'view-format'");
        if (format->get<std::int64_t>() != view_format) {
            return fail(where + " has view format " + std::to_string(format->get<std::int64_t>()) + ", which needs a newer otok: this one reads format "
                + std::to_string(view_format));
        }
        return view_document{std::move(j), std::move(where)};
    }

    nlohmann::json const& json() const noexcept { return j_; }
    std::string const& where() const noexcept { return where_; }

    result<std::optional<std::string>> optional_string(nlohmann::json const& in, std::string_view key) const
    {
        auto const it = in.find(std::string{key});
        if (it == in.end()) return std::nullopt;
        if (!it->is_string()) return fail(where_ + ": '" + std::string{key} + "' must be a string");
        return it->get<std::string>();
    }

    result<std::string> string(nlohmann::json const& in, std::string_view key) const
    {
        auto s = optional_string(in, key);
        if (!s) return std::unexpected{s.error()};
        if (!*s) return fail(where_ + " has no '" + std::string{key} + "'");
        return std::move(**s);
    }

    result<nlohmann::json const*> array(nlohmann::json const& in, std::string_view key) const
    {
        auto const it = in.find(std::string{key});
        if (it == in.end() || !it->is_array()) return fail(where_ + ": '" + std::string{key} + "' must be an array");
        return &*it;
    }

    // `{ file, toml }` items (index.md 8); `in_variant`: a file in the directory of a variant ("apple/1.0-r1.toml").
    result<std::vector<index_file>> files(std::string_view key, bool in_variant = false) const
    {
        auto list = array(j_, key);
        if (!list) return std::unexpected{list.error()};
        std::vector<index_file> out;
        for (nlohmann::json const& item : **list) {
            if (!item.is_object()) return fail(where_ + ": the items of '" + std::string{key} + "' must be objects");
            auto file = string(item, "file");
            if (!file) return std::unexpected{file.error()};
            auto text = string(item, "toml");
            if (!text) return std::unexpected{text.error()};
            std::size_t const slash = file->find('/');
            std::string_view const name = in_variant && slash != std::string::npos ? std::string_view{*file}.substr(slash + 1) : std::string_view{*file};
            bool const variant_ok = !in_variant || (slash != std::string::npos && is_recipe_name(file->substr(0, slash)));
            if (!variant_ok || name.find('/') != std::string::npos || file->find('\\') != std::string::npos || !name.ends_with(".toml")) {
                return fail(where_ + ": " + in_quotes(*file) + " is not the name of a TOML file" + (in_variant ? " in the directory of a variant" : ""));
            }
            out.push_back({std::move(*file), std::move(*text)});
        }
        return out;
    }

private:
    view_document(nlohmann::json j, std::string where) : j_(std::move(j)), where_{std::move(where)} {}  // braces would make an array of j

    nlohmann::json j_;
    std::string where_;
};

class view_source : public index_source
{
public:
    view_source(view_files files, std::string where) : files_{std::move(files)}, where_{std::move(where)} {}

    result<std::optional<std::string>> settings() override
    {
        auto doc = required("config.json");
        if (!doc) return std::unexpected{doc.error()};
        return doc->optional_string(doc->json(), "toml");
    }

    result<std::optional<index_recipe_files>> recipe(recipe_id const& id) override
    {
        auto doc = optional("recipes/" + id.str() + ".json");
        if (!doc) return std::unexpected{doc.error()};
        if (!*doc) return std::nullopt;
        view_document const& d = **doc;
        auto named = d.string(d.json(), "id");
        if (!named) return std::unexpected{named.error()};
        if (*named != id.str()) return fail(d.where() + " is of recipe " + in_quotes(*named) + ", not " + in_quotes(id.str()));
        index_recipe_files found;
        auto entries = d.files("entries", true);
        if (!entries) return std::unexpected{entries.error()};
        found.entries = std::move(*entries);
        auto status = d.optional_string(d.json(), "status");
        if (!status) return std::unexpected{status.error()};
        found.status = std::move(*status);
        return found;
    }

    result<std::optional<std::string>> library(std::string_view library) override
    {
        auto doc = optional("libraries/" + std::string{library} + ".json");
        if (!doc) return std::unexpected{doc.error()};
        if (!*doc) return std::nullopt;
        return (*doc)->optional_string((*doc)->json(), "toml");
    }

    result<std::vector<index_file>> advisories() override
    {
        auto doc = required("advisories.json");
        if (!doc) return std::unexpected{doc.error()};
        return doc->files("advisories");
    }

    result<std::optional<std::string>> availability() override
    {
        auto doc = required("availability.json");
        if (!doc) return std::unexpected{doc.error()};
        return doc->optional_string(doc->json(), "toml");
    }

    result<std::vector<recipe_id>> recipe_ids() override
    {
        auto list = summaries();
        if (!list) return std::unexpected{list.error()};
        std::vector<recipe_id> ids;
        for (recipe_summary const& s : **list) ids.push_back(s.id);
        return ids;
    }

    result<std::optional<std::vector<recipe_summary>>> summaries() override
    {
        auto doc = required("recipes.json");
        if (!doc) return std::unexpected{doc.error()};
        auto items = doc->array(doc->json(), "recipes");
        if (!items) return std::unexpected{items.error()};
        std::vector<recipe_summary> list;
        for (nlohmann::json const& item : **items) {
            if (!item.is_object()) return fail(doc->where() + ": the items of 'recipes' must be objects");
            recipe_summary s;
            auto id = doc->string(item, "id");
            if (!id) return std::unexpected{id.error()};
            auto parsed = parse_recipe_id(*id);
            if (!parsed) return fail(doc->where() + ": " + in_quotes(*id) + " is not a recipe id");
            s.id = std::move(*parsed);
            for (auto [key, out] : {std::pair{"library", &s.library}, std::pair{"description", &s.description}, std::pair{"version", &s.version}}) {
                auto value = doc->string(item, key);
                if (!value) return std::unexpected{value.error()};
                *out = std::move(*value);
            }
            auto keywords = doc->array(item, "keywords");
            if (!keywords) return std::unexpected{keywords.error()};
            for (nlohmann::json const& k : **keywords) {
                if (!k.is_string()) return fail(doc->where() + ": keywords must be strings");
                s.keywords.push_back(k.get<std::string>());
            }
            auto targets = doc->array(item, "targets");
            if (!targets) return std::unexpected{targets.error()};
            for (nlohmann::json const& t : **targets) {
                if (!t.is_string()) return fail(doc->where() + ": targets must be strings");
                s.targets.push_back(t.get<std::string>());
            }
            list.push_back(std::move(s));
        }
        return list;
    }

private:
    result<std::optional<view_document>> optional(std::string const& path)
    {
        auto text = files_(path);
        if (!text) return std::unexpected{text.error()};
        if (!*text) return std::nullopt;
        auto doc = view_document::parse(**text, where_ + "/index/" + path);
        if (!doc) return std::unexpected{doc.error()};
        return std::move(*doc);
    }

    result<view_document> required(std::string const& path)
    {
        auto doc = optional(path);
        if (!doc) return std::unexpected{doc.error()};
        if (!*doc) return fail("the index view " + where_ + " has no index/" + path);
        return std::move(**doc);
    }

    view_files files_;
    std::string where_;
};

// The state of a view read over HTTP, shared by the copies of its view_files.
struct http_view
{
    std::string base;
    std::filesystem::path dir;         // view/<url key>/
    std::optional<cache_store> cache;  // a copy shares the store: the caller's may move
    http_transport http;
    std::map<std::string, std::optional<std::string>, std::less<>> read;  // this command's files
    std::size_t staged = 0;

    // Writes `content` to `file` through this process's tmp directory: two otok processes may update it.
    result<void> store(std::filesystem::path const& file, std::string_view content)
    {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::filesystem::path const staging = cache->tmp() / ("view-" + std::to_string(++staged));
        if (auto ok = write_file_atomic(staging, content); !ok) return ok;
        rename_path(staging, file, ec);
        if (ec) return fail("cannot write '" + path_to_utf8(file) + "': " + ec.message());
        return {};
    }

    result<std::optional<std::string>> get(std::string const& path)
    {
        if (auto it = read.find(path); it != read.end()) return it->second;
        std::filesystem::path const file = dir / path_from_utf8(path);
        std::filesystem::path etag_file = file;
        etag_file += ".etag";
        std::string const url = base + "/index/" + path;
        std::error_code ec;

        if (cache->offline()) {
            auto cached = read_file(file);
            if (!cached) return fail("offline, and " + url + " is not in the cache; read the index online once");
            return read.emplace(path, std::move(*cached)).first->second;
        }

        std::optional<std::string> etag;
        if (std::filesystem::exists(file, ec)) {
            if (auto text = read_file(etag_file)) etag = std::move(*text);
        }
        http_exchange request;
        request.url = url;
        request.headers.push_back("Accept: application/json");
        if (etag) request.headers.push_back("If-None-Match: " + *etag);
        auto reply = http(request);
        if (!reply) return fail("cannot read the index view: " + reply.error().message);

        if (reply->status == 304 && etag) {
            auto cached = read_file(file);
            if (cached) return read.emplace(path, std::move(*cached)).first->second;
            // removed meanwhile: asked again without a condition
            request.headers.pop_back();
            reply = http(request);
            if (!reply) return fail("cannot read the index view: " + reply.error().message);
        }
        if (reply->status == 404) {
            std::filesystem::remove(etag_file, ec);
            std::filesystem::remove(file, ec);
            return read.emplace(path, std::nullopt).first->second;
        }
        if (reply->status != 200) return fail("GET " + url + ": HTTP status " + std::to_string(reply->status));

        std::filesystem::remove(etag_file, ec);
        if (auto ok = store(file, reply->body); !ok) return std::unexpected{ok.error()};
        if (std::string const* tag = reply->header("etag"); tag && !tag->empty()) {
            if (auto ok = store(etag_file, *tag); !ok) return std::unexpected{ok.error()};
        }
        return read.emplace(path, std::move(reply->body)).first->second;
    }
};

bool is_view_directory(std::filesystem::path const& dir)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "index" / "config.json", ec);
}

}

std::string view_base(std::string_view index)
{
    constexpr std::string_view prefix = "view+";
    if (!index.starts_with(prefix)) return {};
    std::string base{index.substr(prefix.size())};
    while (base.ends_with('/')) base.pop_back();
    return base;
}

view_files view_directory(std::filesystem::path dir)
{
    return [dir = std::move(dir)](std::string const& path) -> result<std::optional<std::string>> {
        std::filesystem::path const file = dir / "index" / path_from_utf8(path);
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) return std::nullopt;
        auto text = read_file(file);
        if (!text) return std::unexpected{text.error()};
        return std::move(*text);
    };
}

view_files view_over_http(std::string base, cache_store& cache, http_transport http)
{
    auto state = std::make_shared<http_view>();
    state->dir = cache.root() / "view" / git_url_key(base);
    state->base = std::move(base);
    state->cache = cache;
    state->http = std::move(http);
    return [state](std::string const& path) { return state->get(path); };
}

std::unique_ptr<index_source> view_index_source(view_files files, std::string where)
{
    return std::make_unique<view_source>(std::move(files), std::move(where));
}

result<std::unique_ptr<index_source>> open_index_source(std::string const& index, std::string const& mirror, cache_store& cache,
    git_tool const* git, http_transport const& http)
{
    auto const over_http = [&](std::string base) -> result<std::unique_ptr<index_source>> {
        if (!base.starts_with("https://")) return fail("the index view " + in_quotes(base) + " must be an https URL");
        std::string where = base;
        return view_index_source(view_over_http(std::move(base), cache, http), std::move(where));
    };
    if (!mirror.empty()) {
        if (mirror.find("://") != std::string::npos) {
            std::string base = mirror;
            while (base.ends_with('/')) base.pop_back();
            return over_http(std::move(base));
        }
        std::filesystem::path const dir = path_from_utf8(mirror);
        if (is_view_directory(dir)) return view_index_source(view_directory(dir), mirror);
    }
    if (mirror.empty()) {
        if (std::string base = view_base(index); !base.empty()) return over_http(std::move(base));
    }
    auto dir = index_checkout(index, mirror, cache, git);
    if (!dir) return std::unexpected{dir.error()};
    return directory_index_source(std::move(*dir));
}

namespace {

std::string json_text(nlohmann::ordered_json const& j)
{
    return j.dump(2) + "\n";
}

nlohmann::ordered_json files_json(std::vector<index_file> const& files)
{
    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (index_file const& f : files) list.push_back({{"file", f.file}, {"toml", f.text}});
    return list;
}

nlohmann::ordered_json view_object()
{
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    j["view-format"] = view_format;
    return j;
}

void set_optional(nlohmann::ordered_json& j, char const* key, std::optional<std::string> const& text)
{
    if (text) j[key] = *text;
}

}

result<view_written> write_index_view(std::filesystem::path const& root, std::string const& commit, std::filesystem::path const& output,
    std::span<target_info const* const> targets)
{
    recipe_index idx{root, targets, index_reading::ci};
    auto source = directory_index_source(root);
    std::map<std::string, std::string> files;  // path under index/ → contents

    // config.json
    if (auto s = idx.settings(); !s) return std::unexpected{s.error()};
    auto settings = source->settings();
    if (!settings) return std::unexpected{settings.error()};
    nlohmann::ordered_json config = view_object();
    config["commit"] = commit;
    config["toml"] = **settings;
    files["config.json"] = json_text(config);

    // the recipes, their summaries and the libraries they build
    auto ids = idx.recipe_ids();
    if (!ids) return std::unexpected{ids.error()};
    std::map<std::string, std::set<std::string>> libraries;  // library → recipe ids
    nlohmann::ordered_json summaries = nlohmann::ordered_json::array();
    for (recipe_id const& id : *ids) {
        auto sorted = idx.newest_first(id);
        if (!sorted) return std::unexpected{sorted.error()};
        auto status = idx.status(id);
        if (!status) return std::unexpected{status.error()};
        auto raw = source->recipe(id);
        if (!raw) return std::unexpected{raw.error()};
        std::map<std::string, std::string, std::less<>> texts;
        for (index_file& f : (*raw)->entries) texts.emplace(f.file, std::move(f.text));
        std::vector<index_file> entries;
        for (index_entry const* e : *sorted) entries.push_back({e->file(), texts.at(e->file())});
        nlohmann::ordered_json j = view_object();
        j["id"] = id.str();
        j["entries"] = files_json(entries);
        set_optional(j, "status", (*raw)->status);
        files["recipes/" + id.str() + ".json"] = json_text(j);

        recipe const& newest = newest_stable(*sorted)->projection;
        summaries.push_back({{"id", id.str()}, {"library", newest.library}, {"description", newest.description},
            {"keywords", newest.keywords}, {"version", newest.version->str()}, {"targets", version_targets(*sorted, *newest.version, targets)}});
        libraries[newest.library].insert(id.str());
    }
    nlohmann::ordered_json all = view_object();
    all["recipes"] = std::move(summaries);
    files["recipes.json"] = json_text(all);

    std::error_code ec;
    if (std::filesystem::is_directory(root / "libraries", ec)) {
        for (auto const& item : std::filesystem::directory_iterator{root / "libraries", ec}) {
            if (item.path().extension() == ".toml") libraries[path_to_utf8(item.path().stem())];
        }
    }
    for (auto const& [library, recipes] : libraries) {
        if (auto o = idx.order(library); !o) return std::unexpected{o.error()};
        auto text = source->library(library);
        if (!text) return std::unexpected{text.error()};
        nlohmann::ordered_json j = view_object();
        j["library"] = library;
        set_optional(j, "toml", *text);
        j["recipes"] = recipes;
        files["libraries/" + library + ".json"] = json_text(j);
    }

    // advisories, availability, the Apple data files
    if (auto a = idx.advisories(); !a) return std::unexpected{a.error()};
    auto advisories = source->advisories();
    if (!advisories) return std::unexpected{advisories.error()};
    nlohmann::ordered_json adv = view_object();
    adv["advisories"] = files_json(*advisories);
    files["advisories.json"] = json_text(adv);

    if (auto a = idx.availability(); !a) return std::unexpected{a.error()};
    auto availability = source->availability();
    if (!availability) return std::unexpected{availability.error()};
    nlohmann::ordered_json av = view_object();
    set_optional(av, "toml", *availability);
    files["availability.json"] = json_text(av);

    for (char const* name : {"required-reason-apis", "privacy-sdks", "private-libraries"}) {
        std::filesystem::path const file = root / "apple" / (std::string{name} + ".toml");
        nlohmann::ordered_json j = view_object();
        if (std::filesystem::exists(file, ec)) {
            auto text = read_file(file);
            if (!text) return std::unexpected{text.error()};
            j["toml"] = std::move(*text);
        }
        files["apple/" + std::string{name} + ".json"] = json_text(j);
    }

    // written where they changed; the files no longer generated removed
    std::map<std::string, std::string> under_root;
    for (auto& [path, contents] : files) under_root.emplace("index/" + path, std::move(contents));
    std::string const owned[] = {"index"};
    return write_generated_files(output, under_root, owned);
}

}
