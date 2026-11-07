// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/index/check.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "otok/cache/inputs.hpp"
#include "otok/core/environment.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/names.hpp"
#include "otok/core/sha256.hpp"
#include "otok/core/toml_reader.hpp"
#include "otok/index/index.hpp"
#include "otok/publish/publish.hpp"
#include "otok/recipe/recipe.hpp"
#include "otok/resolver/locking.hpp"

namespace otok {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

std::string messages(diagnostics const& d)
{
    std::string text;
    for (auto const& item : d.items()) text += "\n  " + format(item);
    return text;
}

std::vector<std::string> components(std::string const& path)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t const slash = std::min(path.find('/', start), path.size());
        parts.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

std::chrono::sys_seconds time_of(toml::date_time const& t)
{
    using namespace std::chrono;
    sys_days const day = year{t.date.year} / month{t.date.month} / std::chrono::day{t.date.day};
    sys_seconds s = day + hours{t.time.hour} + minutes{t.time.minute} + seconds{t.time.second};
    if (t.offset) s -= minutes{t.offset->minutes};
    return s;
}

toml::date_time date_time_of(std::chrono::system_clock::time_point tp)
{
    using namespace std::chrono;
    auto const now = floor<seconds>(tp);
    auto const day = floor<days>(now);
    year_month_day const ymd{day};
    hh_mm_ss const hms{now - day};
    return toml::date_time{toml::date{static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day())},
        toml::time{static_cast<unsigned>(hms.hours().count()), static_cast<unsigned>(hms.minutes().count()),
            static_cast<unsigned>(hms.seconds().count())},
        toml::time_offset{}};
}

// An ISO 8601 time as GitHub writes it ("2020-01-01T00:00:00Z").
std::optional<std::chrono::sys_seconds> iso_time(std::string const& text)
{
    try {
        toml::table const t = toml::parse("t = " + text);
        if (auto const* p = t["t"].as_date_time(); p && p->get().offset) return time_of(p->get());
    } catch (toml::parse_error const&) {
    }
    return std::nullopt;
}

// "<version>-r<revision>.toml" → (version, revision).
std::optional<std::pair<std::string, std::string>> entry_name(std::string const& file)
{
    if (!file.ends_with(".toml")) return std::nullopt;
    std::string const stem = file.substr(0, file.size() - 5);
    std::size_t const r = stem.rfind("-r");
    if (r == std::string::npos || r == 0 || r + 2 >= stem.size()) return std::nullopt;
    std::string const revision = stem.substr(r + 2);
    if (!std::ranges::all_of(revision, [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    return std::pair{stem.substr(0, r), revision};
}

class checker
{
public:
    explicit checker(index_check_settings const& s) : s_{s}, log_{s.cache->tmp() / "index-check.log"} {}

    result<index_check_report> run();

private:
    struct change
    {
        char status;                   // A, M, D
        std::string path;
    };

    result<std::string> git(std::vector<std::string> const& args)
    {
        return run_git(*s_.git, args, s_.root, log_, true);
    }

    std::optional<std::string> at_base(std::string const& path)
    {
        auto content = git({"show", s_.base + ":" + path});
        if (!content) return std::nullopt;
        return std::move(*content);
    }

    std::optional<std::string> at_head(std::string const& path)
    {
        auto content = read_file(s_.root / path_from_utf8(path));
        if (!content) return std::nullopt;
        return std::move(*content);
    }

    void error(std::string message) { report_.errors.push_back(std::move(message)); }
    void reason(std::string message) { report_.reasons.push_back(std::move(message)); }

    result<json> api(std::string const& path, long* status = nullptr);
    std::optional<std::string> login_of(std::int64_t id);
    std::optional<publisher_file> registered(std::string const& publisher);
    std::string refusal(std::string const& publisher);

    void publisher_file_changed(change const& c, std::string const& name);
    void entry_added(std::string const& path, std::vector<std::string> const& parts);
    void library_changed(change const& c);
    void advisory_changed(change const& c, std::string const& stem);
    void check_versions(index_entry const& e, std::string const& path);
    void check_cells(index_entry const& e, std::string const& path);
    void check_toolchain_recipes(recipe const& r, std::string const& label);
    void compare_with_git(recipe const& r, fs::path const& archive, std::string const& label);
    void rate_limits();
    void check_author();
    void approval();
    result<void> stamp();
    bool maintains(std::int64_t id);
    bool author_maintains();
    std::vector<open_pull_request> const* open_requests();
    index_settings const& settings();

    index_check_settings const& s_;
    fs::path log_;
    index_check_report report_;
    std::map<std::string, std::optional<publisher_file>, std::less<>> base_publishers_;
    std::set<std::string, std::less<>> registering_;      // publisher files added by this change
    std::map<std::string, std::size_t, std::less<>> new_entries_;  // by publisher
    std::set<std::string, std::less<>> new_paths_;
    std::map<std::int64_t, std::optional<std::string>> logins_;
    std::optional<recipe_index> index_;                    // the index at the head, read when needed
    std::set<std::string, std::less<>> published_for_;     // publishers of new entries and of advisories
    bool library_changed_ = false;
    std::map<std::int64_t, bool> maintainers_;             // by account id
    std::optional<std::vector<open_pull_request>> open_;
    std::optional<index_settings> settings_;               // of the base
};

result<json> checker::api(std::string const& path, long* status)
{
    if (!s_.github.transport) return fail("the check needs the GitHub API (set GITHUB_TOKEN)");
    auto reply = github_get(s_.github.transport, s_.github.api, s_.github.token, path);
    if (!reply) return std::unexpected{reply.error()};
    if (status) *status = reply->status;
    if (reply->status == 204) return json{};
    if (reply->status < 200 || reply->status >= 300) return fail("GitHub: GET " + path + ": " + std::to_string(reply->status));
    auto body = json::parse(reply->body, nullptr, false);
    if (body.is_discarded()) return fail("GitHub: GET " + path + ": the reply is not JSON");
    return body;
}

std::optional<std::string> checker::login_of(std::int64_t id)
{
    if (auto it = logins_.find(id); it != logins_.end()) return it->second;
    std::optional<std::string> login;
    if (auto user = api("/user/" + std::to_string(id)); user && user->contains("login") && (*user)["login"].is_string()) {
        login = (*user)["login"].get<std::string>();
    }
    logins_[id] = login;
    return login;
}

// The publisher file as it was before the change.
std::optional<publisher_file> checker::registered(std::string const& publisher)
{
    if (auto it = base_publishers_.find(publisher); it != base_publishers_.end()) return it->second;
    std::optional<publisher_file> file;
    std::string const path = "publishers/" + publisher + ".toml";
    if (auto content = at_base(path)) {
        diagnostics d;
        file = parse_publisher(*content, path, d, index_reading::ci);
    }
    base_publishers_[publisher] = file;
    return file;
}

// Why the author may not publish as `publisher`, or empty (index.md 9).
std::string checker::refusal(std::string const& publisher)
{
    auto const file = registered(publisher);
    if (!file) {
        if (registering_.contains(publisher)) return {};  // the registration is checked on its own
        return "the publisher " + in_quotes(publisher) + " is not registered (publishers/" + publisher + ".toml)";
    }
    if (!file->suspended.empty()) return "the publisher " + in_quotes(publisher) + " is suspended: " + file->suspended;
    if (!s_.author_id) return "the author of the change is not known (--author-id)";
    if (!file->organization) {
        if (file->github_id == *s_.author_id) return {};
        return "the author (GitHub account " + std::to_string(*s_.author_id) + ") is not the publisher " + in_quotes(publisher);
    }
    auto const author = login_of(*s_.author_id);
    auto const organization = login_of(file->github_id);
    if (!author || !organization) return "cannot read the GitHub accounts of the author and of the organization " + in_quotes(publisher);
    long status = 0;
    auto member = api("/orgs/" + *organization + "/public_members/" + *author, &status);
    if (member && status == 204) return {};
    return "the author " + in_quotes(*author) + " is not a public member of the organization " + in_quotes(*organization)
        + ", which the publisher " + in_quotes(publisher) + " is";
}

void checker::publisher_file_changed(change const& c, std::string const& name)
{
    std::string const label = "the publisher file of " + in_quotes(name);
    if (c.status == 'D') return error(c.path + ": publisher files are never deleted (a maintainer suspends a publisher instead)");
    diagnostics d;
    auto content = at_head(c.path);
    auto file = content ? parse_publisher(*content, c.path, d, index_reading::ci) : std::nullopt;
    if (!file) return error(c.path + " is invalid:" + messages(d));
    if (c.status == 'M') {
        auto const before = registered(name);
        if (file->established && !(before && before->established)) return reason("makes the publisher " + in_quotes(name) + " established");
        return reason("changes " + label);
    }

    reason("registers the publisher " + in_quotes(name) + (file->established ? " as established" : ""));
    if (!is_publisher_name(name)) error(in_quotes(name) + " is not a valid publisher name");
    if (!s_.author_id) error("registering a publisher needs the author of the change (--author-id)");
    else if (file->github_id != *s_.author_id && !author_maintains()) error(c.path + ": 'github-id' must be the author's GitHub account, " + std::to_string(*s_.author_id));
    std::error_code ec;
    for (auto const& other : fs::directory_iterator{s_.root / "publishers", ec}) {
        std::string const stem = path_to_utf8(other.path().stem());
        if (stem != name && other.path().extension() == ".toml" && confusable_form(stem) == confusable_form(name)) {
            error(in_quotes(name) + " is too much like the registered publisher " + in_quotes(stem));
        }
    }
    // one registration at a time
    if (!s_.author_id || author_maintains()) return;
    if (auto const* open = open_requests()) {
        for (open_pull_request const& r : *open) {
            if (r.author_id != *s_.author_id) continue;
            bool const registers = std::ranges::any_of(r.added, [](std::string const& p) {
                auto const parts = components(p);
                return parts.size() == 2 && parts[0] == "publishers" && parts[1].ends_with(".toml");
            });
            if (registers) return error("the author has another open pull request registering a publisher, #" + std::to_string(r.number));
        }
    }
}

void checker::entry_added(std::string const& path, std::vector<std::string> const& parts)
{
    auto content = at_head(path);
    diagnostics d;
    auto e = content ? parse_index_entry(*content, path, s_.targets, d, index_reading::ci) : std::nullopt;
    if (!e) return error(path + " is invalid:" + messages(d));
    ++report_.entries;
    std::string const version = e->projection.version->str() + "-r" + std::to_string(e->projection.revision);
    std::string const label = e->id.str() + " " + version + " (" + e->projection.variant + ")";
    if (e->id.publisher != parts[1] || e->id.name != parts[2]) error(path + ": the entry is of " + in_quotes(e->id.str()) + ", not of its directory");
    if (parts[3] + "/" + parts[4] != e->file()) {
        error(path + ": the entry is of the variant " + in_quotes(e->projection.variant) + " and version " + version + ", which does not match its file");
    }
    if (std::string const why = refusal(e->id.publisher); !why.empty()) error(label + ": " + why);
    ++new_entries_[e->id.publisher];
    published_for_.insert(e->id.publisher);
    new_paths_.insert(path);

    // the time of the merge, which the workflow writes (index.md 3): nothing an author could choose
    if (e->published) error(label + ": 'published' is written by index CI when it merges the entry, never by a pull request (index.md 3)");

    // the recipe tree, as publish checks it (DESIGN 7.4)
    auto tree = s_.cache->git_directory(e->recipe_source.git, e->recipe_source.commit, e->recipe_source.tag, e->recipe_source.path, *s_.git,
        e->projection.library);
    if (!tree) return error(label + ": cannot fetch its recipe tree: " + tree.error().message);
    auto made = make_index_entry(*tree, e->recipe_source, e->id.publisher, std::nullopt, s_.targets);
    if (!made) return error(label + ": " + made.error().message);
    auto same = same_index_entry(*content, made->entry_text, path, s_.targets);
    if (!same) return error(same.error().message);
    if (!*same) {
        return error(label + ": the entry does not match its recipe tree at " + e->recipe_source.git + " " + e->recipe_source.commit
            + " (recipe-sha256 or the projection, index.md 3.1)");
    }
    auto r = load_recipe(*tree, s_.targets, d);
    if (!r) return error(label + ": " + messages(d));
    if (s_.https_only) {
        for (std::string const& refused : non_https_locations(e->recipe_source, *r)) error(label + ": " + refused + " is not https");
    }
    check_toolchain_recipes(*r, label);

    // the inputs download and match their hashes
    std::vector<input_request> requests{{label + " source", location_of(r->source.location), true, r->library}};
    for (recipe_resource const& res : r->resources) requests.push_back({label + " resource " + in_quotes(res.name), location_of(res.location), res.unpack, r->library});
    auto const fetched = fetch_inputs(*s_.cache, requests, s_.http, s_.git);
    for (auto const& f : fetched) {
        if (!f) error(f.error().message);
    }
    if (fetched.front() && r->source.verify && r->source.location.type == input_location::kind::archive) compare_with_git(*r, *fetched.front(), label);
    check_versions(*e, path);
    check_cells(*e, path);
}

// index.md 9: the `default-recipe` of every toolchain has an entry of a tool recipe with its program in the index or in this change
// (docs/PLAN.md decision 85); make_index_entry has refused toolchains without one.
void checker::check_toolchain_recipes(recipe const& r, std::string const& label)
{
    if (!index_) index_.emplace(s_.root, s_.targets, index_reading::ci);
    for (toolchain_requirement const& t : r.toolchains) {
        if (!t.default_recipe) continue;
        // an entry that cannot be read is reported as its own file
        auto entries = index_->entries(*t.default_recipe);
        bool const found = entries && std::ranges::any_of(**entries, [&](index_entry const& e) {
            return std::ranges::find(e.projection.programs, t.name) != e.projection.programs.end();
        });
        if (!found) {
            error(label + ": the toolchain " + in_quotes(t.name) + " has the 'default-recipe' " + in_quotes(t.default_recipe->str())
                + ", which has no entry of a tool recipe building " + in_quotes(t.name) + " in the index (recipe-toml.md 13.1)");
        }
    }
}

// index.md 3, 9: no other variant of the recipe has an entry of the same version building a platform of this one in an
// environment one of them covers in the other's.
void checker::check_cells(index_entry const& e, std::string const& path)
{
    std::error_code ec;
    fs::path const recipe_dir = (s_.root / path_from_utf8(path)).parent_path().parent_path();
    for (auto const& variant : fs::directory_iterator{recipe_dir, ec}) {
        if (!variant.is_directory(ec) || path_to_utf8(variant.path().filename()) == e.projection.variant) continue;
        for (auto const& other : fs::directory_iterator{variant.path(), ec}) {
            std::string const name = path_to_utf8(other.path().filename());
            auto const parsed_name = entry_name(name);
            if (!parsed_name || parsed_name->first != e.projection.version->str()) continue;
            auto content = read_file(other.path());
            diagnostics d;
            auto o = content ? parse_index_entry(*content, name, s_.targets, d, index_reading::ci) : std::nullopt;
            if (!o) continue;  // checked as its own file
            for (std::string const& p : e.projection.platforms) {
                if (std::ranges::find(o->projection.platforms, p) == o->projection.platforms.end()) continue;
                for (build_environment const& a : e.projection.build_on) {
                    for (build_environment const& b : o->projection.build_on) {
                        if (!covers(a, b) && !covers(b, a)) continue;
                        return error(path + ": the variant " + in_quotes(o->projection.variant) + " builds " + in_quotes(p) + " in " + b.text()
                            + " for this version already, which this one builds in " + a.text() + " (index.md 3)");
                    }
                }
            }
        }
    }
}

// verify-against (recipe-toml.md 4, DESIGN 11 item 2): the differences go to the notes.
void checker::compare_with_git(recipe const& r, fs::path const& archive, std::string const& label)
{
    verify_against const& v = *r.source.verify;
    std::string commit = v.commit;
    if (commit.empty()) {
        auto refs = git({"ls-remote", v.git, "refs/tags/" + v.tag, "refs/tags/" + v.tag + "^{}"});
        if (!refs) return report_.notes.push_back(label + ": cannot read the tag " + in_quotes(v.tag) + " of " + v.git + " to compare the archive with");
        // the peeled line (an annotated tag) wins over the tag's own
        std::istringstream lines{*refs};
        for (std::string line; std::getline(lines, line);) {
            if (line.size() > 40 && (commit.empty() || line.ends_with("^{}"))) commit = line.substr(0, 40);
        }
        if (commit.empty()) return report_.notes.push_back(label + ": " + v.git + " has no tag " + in_quotes(v.tag));
    }
    auto tree = s_.cache->git_tree(v.git, commit, v.commit.empty() ? v.tag : std::string{}, false, *s_.git, r.library);
    if (!tree) return report_.notes.push_back(label + ": cannot fetch " + v.git + " to compare the archive with: " + tree.error().message);
    auto const files_of = [](fs::path const& dir) {
        std::map<std::string, std::string> files;
        std::error_code ec;
        for (auto const& f : fs::recursive_directory_iterator{dir, ec}) {
            if (f.is_regular_file(ec)) files[path_to_utf8(f.path().lexically_relative(dir).generic_u8string())] = sha256_file(f.path()).value_or("");
        }
        return files;
    };
    auto const in_archive = files_of(archive);
    auto const in_git = files_of(*tree);
    std::vector<std::string> differences;
    for (auto const& [file, hash] : in_archive) {
        auto const it = in_git.find(file);
        if (it == in_git.end()) differences.push_back("only in the archive: " + file);
        else if (it->second != hash) differences.push_back("differs: " + file);
    }
    for (auto const& [file, hash] : in_git) {
        if (!in_archive.contains(file)) differences.push_back("only in git: " + file);
    }
    if (differences.empty()) return;
    std::string note = label + ": the archive differs from " + v.git + " at " + (v.commit.empty() ? v.tag : v.commit) + ":";
    std::size_t const shown = std::min<std::size_t>(differences.size(), 50);
    for (std::size_t i = 0; i < shown; ++i) note += "\n  " + differences[i];
    if (shown < differences.size()) note += "\n  and " + std::to_string(differences.size() - shown) + " more";
    report_.notes.push_back(std::move(note));
}

// index.md 9: valid in the library's scheme, and not the version of another entry of the same revision.
void checker::check_versions(index_entry const& e, std::string const& path)
{
    version_order order = version_order::dotted();
    std::string const library_path = "libraries/" + e.projection.library + ".toml";
    if (auto content = at_head(library_path)) {
        diagnostics d;
        if (auto o = parse_library_file(*content, library_path, d, index_reading::ci)) order = std::move(*o);
    }
    version const& v = *e.projection.version;
    if (!order.validate(v)) return error(path + ": the version " + v.str() + " is not valid in the scheme of the library " + in_quotes(e.projection.library));
    std::error_code ec;
    fs::path const dir = (s_.root / path_from_utf8(path)).parent_path();
    for (auto const& other : fs::directory_iterator{dir, ec}) {
        std::string const file = path_to_utf8(other.path().filename());
        auto const name = entry_name(file);
        if (!name || file == path_from_utf8(path).filename().string() || name->second != std::to_string(e.projection.revision)) continue;
        auto const ov = version::parse(name->first);
        if (ov && order.validate(*ov) && ov->str() != v.str() && order.compare(v, *ov) == std::partial_ordering::equivalent) {
            error(path + ": the version " + v.str() + " is the version " + ov->str() + " of another entry in the library's scheme");
        }
    }
}

void checker::library_changed(change const& c)
{
    library_changed_ = true;
    if (c.status == 'D') return error(c.path + ": library files are never deleted");
    auto content = at_head(c.path);
    diagnostics d;
    if (!content || !parse_library_file(*content, c.path, d, index_reading::ci)) return error(c.path + " is invalid:" + messages(d));
    if (c.status == 'A') return;
    auto const old = at_base(c.path);
    auto const raw = [](std::string const& text) {
        std::pair<std::string, std::vector<std::string>> scheme{"dotted", {}};
        try {
            toml::table const t = toml::parse(text);
            scheme.first = t["scheme"].value_or(std::string{"dotted"});
            if (auto const* order = t["order"].as_array()) {
                for (auto const& v : *order) scheme.second.push_back(v.value_or(std::string{}));
            }
        } catch (toml::parse_error const&) {
        }
        return scheme;
    };
    auto const before = raw(old.value_or(""));
    auto const after = raw(*content);
    if (before.first != after.first) return reason("changes the scheme of " + c.path);
    // only versions added to an explicit order, the others kept in their order
    std::size_t at = 0;
    for (std::string const& v : after.second) {
        if (at < before.second.size() && before.second[at] == v) ++at;
    }
    if (at != before.second.size()) reason("reorders the versions of " + c.path);
}

void checker::advisory_changed(change const& c, std::string const& stem)
{
    if (c.status == 'D') return error(c.path + ": advisories are never deleted");
    auto content = at_head(c.path);
    diagnostics d;
    auto a = content ? parse_advisory(*content, c.path, d, index_reading::ci) : std::nullopt;
    if (!a) return error(c.path + " is invalid:" + messages(d));
    if (a->id != stem) return error(c.path + ": the advisory's id " + in_quotes(a->id) + " must be its file name");
    if (a->recipes.empty()) return reason("changes the advisory " + a->id + " of every recipe of the library " + in_quotes(a->library));
    for (recipe_id const& id : a->recipes) {
        if (!refusal(id.publisher).empty() || registering_.contains(id.publisher)) {
            return reason("changes the advisory " + a->id + ", which names recipes of publishers other than the author's");
        }
    }
    for (recipe_id const& id : a->recipes) published_for_.insert(id.publisher);
}

// index.md 9: an index maintainer, one with the write, maintain or admin permission on the index's repository.
bool checker::maintains(std::int64_t id)
{
    if (auto it = maintainers_.find(id); it != maintainers_.end()) return it->second;
    bool& maintainer = maintainers_[id] = false;
    if (s_.repository.empty()) return false;
    auto const login = login_of(id);
    if (!login) return false;
    auto reply = api("/repos/" + s_.repository + "/collaborators/" + *login + "/permission");
    if (!reply || !reply->contains("permission") || !(*reply)["permission"].is_string()) return false;
    std::string const permission = (*reply)["permission"].get<std::string>();
    maintainer = permission == "write" || permission == "admin";  // "maintain" is "write" there
    return maintainer;
}

bool checker::author_maintains()
{
    return s_.author_id && maintains(*s_.author_id);
}

// index.md 9.2: a maintainer's approval lets the workflow merge what waits for a maintainer; an approval by anyone else is
// noted and changes nothing.
void checker::approval()
{
    if (!s_.approved_by || report_.verdict != index_verdict::maintainer) return;
    std::string const who = login_of(*s_.approved_by).value_or("GitHub account " + std::to_string(*s_.approved_by));
    if (!maintains(*s_.approved_by)) return report_.notes.push_back("approved by " + who + ", who is not an index maintainer");
    report_.verdict = index_verdict::automatic;
    report_.notes.push_back("approved by the index maintainer " + who);
}

// index.md 3: the time of the merge in the new entries, which the workflow commits as it merges them.
result<void> checker::stamp()
{
    if (!s_.stamp || report_.verdict != index_verdict::automatic) return {};
    toml::date_time const published = date_time_of(s_.now);
    for (std::string const& path : new_paths_) {
        auto content = at_head(path);
        if (!content) return fail("cannot read " + path + " to write the time of its publication");
        auto stamped = stamp_index_entry(*content, published);
        if (!stamped) return fail(path + ": " + stamped.error().message);
        if (auto ok = write_file_atomic(s_.root / path_from_utf8(path), *stamped); !ok) return std::unexpected{ok.error()};
        report_.stamped.push_back(path);
    }
    return {};
}

// index.md 9.2: the index's other open pull requests, none without --repository; nothing when they cannot be read, which
// is an error of the check.
std::vector<open_pull_request> const* checker::open_requests()
{
    if (open_) return &*open_;
    if (s_.repository.empty()) return &open_.emplace();
    if (!s_.github.transport) {
        error("reading the open pull requests of the index needs the GitHub API (set GITHUB_TOKEN)");
        return nullptr;
    }
    auto listed = open_pull_requests(s_.github.transport, s_.github.api, s_.github.token, s_.repository);
    if (!listed) {
        error("cannot read the open pull requests of the index: " + listed.error().message);
        return nullptr;
    }
    std::erase_if(*listed, [&](open_pull_request const& r) { return s_.pull_request && r.number == *s_.pull_request; });
    return &open_.emplace(std::move(*listed));
}

index_settings const& checker::settings()
{
    if (settings_) return *settings_;
    settings_.emplace();
    if (auto content = at_base("index.toml")) {
        diagnostics d;
        if (auto s = parse_index_settings(*content, "index.toml", d, index_reading::ci)) settings_ = std::move(*s);
    }
    return *settings_;
}

// index.md 9.2: an author but a maintainer has an account at least 30 days old, unless the pull request only publishes for
// established publishers; the pull requests of a publisher that is not established wait for a maintainer.
void checker::check_author()
{
    if (author_maintains()) return;
    bool all_established = !published_for_.empty() && registering_.empty();
    for (std::string const& publisher : published_for_) {
        auto const file = registered(publisher);
        if (file && file->established) continue;
        all_established = false;
        if (file) reason("the publisher " + in_quotes(publisher) + " is not established: an index maintainer reviews its pull requests (index.md 9.1)");
    }
    if (published_for_.empty() && library_changed_) reason("changes library files without entries of an established publisher");
    if (all_established || !s_.author_id) return;
    auto account = api("/user/" + std::to_string(*s_.author_id));
    if (!account) return error("cannot read the GitHub account of the author, " + std::to_string(*s_.author_id) + ": " + account.error().message);
    auto const created = account->contains("created_at") && (*account)["created_at"].is_string()
        ? iso_time((*account)["created_at"].get<std::string>()) : std::nullopt;
    if (!created) return error("cannot read when the GitHub account of the author, " + std::to_string(*s_.author_id) + ", was created");
    if (s_.now - *created < std::chrono::days{30}) error("the author's GitHub account, " + std::to_string(*s_.author_id) + ", is less than 30 days old");
}

// index.md 9.2: at most established-entries-per-day or review-entries-per-day of the base's index.toml in 24 hours, those of
// the publisher's other open pull requests and the new ones included, but for a maintainer.
void checker::rate_limits()
{
    if (new_entries_.empty()) return;
    auto const now = std::chrono::floor<std::chrono::seconds>(s_.now);
    for (auto const& [publisher, added] : new_entries_) {
        auto const file = registered(publisher);
        publication_limit l = publication_limit_for(settings(), file && file->established);
        l.adding = added;
        if (author_maintains()) {
            l = publication_limit_of(s_.root, publisher, l, now, new_paths_);
            if (l.exceeded()) {
                report_.notes.push_back("the publisher " + in_quotes(publisher) + " has " + std::to_string(l.recent + l.adding)
                    + " entries published in the last 24 hours, more than " + std::to_string(l.limit) + "; the author is an index maintainer, whom the limit does not hold");
            }
            continue;
        }
        auto const* open = open_requests();
        if (!open) return;
        for (open_pull_request const& r : *open) {
            auto const n = std::ranges::count_if(r.added, [&](std::string const& p) { return entry_publisher(p) == publisher; });
            if (n == 0) continue;
            l.pending += static_cast<std::size_t>(n);
            l.pending_requests.push_back(r.number);
        }
        l = publication_limit_of(s_.root, publisher, l, now, new_paths_);
        if (l.exceeded()) error(publication_limit_text(publisher, l));
    }
}

result<index_check_report> checker::run()
{
    auto status = git({"status", "--porcelain"});
    if (!status) return fail(in_quotes(path_to_utf8(s_.root)) + " is not a git checkout: " + status.error().message);
    if (!status->empty()) return fail("the checkout of the index has changes that are not committed:\n" + *status);
    auto diff = git({"diff", "--name-status", "--no-renames", "-z", s_.base, "HEAD"});
    if (!diff) return fail("cannot compare with " + in_quotes(s_.base) + ": " + diff.error().message);
    std::vector<change> changes;
    for (std::size_t at = 0; at < diff->size();) {
        std::size_t const a = diff->find('\0', at);
        if (a == std::string::npos) break;
        std::size_t const b = diff->find('\0', a + 1);
        if (b == std::string::npos) break;
        char const letter = (*diff)[at];
        changes.push_back({letter == 'A' || letter == 'D' ? letter : 'M', diff->substr(a + 1, b - a - 1)});
        at = b + 1;
    }

    // publisher files first: entries of a publisher registered by the same change wait for its registration
    for (change const& c : changes) {
        auto const parts = components(c.path);
        if (parts.size() == 2 && parts[0] == "publishers" && parts[1].ends_with(".toml") && c.status == 'A') {
            registering_.insert(parts[1].substr(0, parts[1].size() - 5));
        }
    }
    for (change const& c : changes) {
        auto const parts = components(c.path);
        bool const toml_file = c.path.ends_with(".toml");
        if (parts.size() == 1 && c.path == "availability.toml") {
            error("availability.toml is written by the index's probe workflow, never by a pull request (index.md 11)");
        } else if (parts.size() == 1 && c.path == "index.toml") {
            reason("changes index.toml");
        } else if (parts.size() == 2 && parts[0] == "publishers" && toml_file) {
            publisher_file_changed(c, parts[1].substr(0, parts[1].size() - 5));
        } else if (parts.size() == 2 && parts[0] == "libraries" && toml_file) {
            library_changed(c);
        } else if (parts.size() == 2 && parts[0] == "advisories" && toml_file) {
            advisory_changed(c, parts[1].substr(0, parts[1].size() - 5));
        } else if (parts.size() == 4 && parts[0] == "recipes" && parts[3] == "recipe-status.toml") {
            if (c.status == 'D') {
                error(c.path + ": recipe status files are never deleted");
            } else {
                diagnostics d;
                auto content = at_head(c.path);
                if (!content || !parse_recipe_status(*content, c.path, d, index_reading::ci)) error(c.path + " is invalid:" + messages(d));
                reason("changes the status of " + in_quotes(parts[1] + "/" + parts[2]));
            }
        } else if (parts.size() == 5 && parts[0] == "recipes" && entry_name(parts[4])) {
            if (c.status == 'A') entry_added(c.path, parts);
            else error(c.path + ": version entries never change once published (publish a new revision instead)");
        } else if (parts.size() == 4 && parts[0] == "recipes" && entry_name(parts[3])) {
            error(c.path + ": a version entry belongs in the directory of its variant, recipes/<publisher>/<name>/<variant>/ (index.md 3)");
        } else if (parts[0] == "apple" && toml_file) {
            reason("changes the Apple data file " + c.path);
        } else {
            reason((c.status == 'D' ? "deletes " : "changes ") + in_quotes(c.path) + ", which is not one of the index's files");
        }
    }
    check_author();
    rate_limits();
    report_.verdict = !report_.errors.empty() ? index_verdict::refused : !report_.reasons.empty() ? index_verdict::maintainer : index_verdict::automatic;
    approval();
    if (auto ok = stamp(); !ok) return std::unexpected{ok.error()};
    return report_;
}

}

std::string_view verdict_name(index_verdict v) noexcept
{
    switch (v) {
    case index_verdict::automatic: return "auto";
    case index_verdict::maintainer: return "maintainer";
    case index_verdict::refused: return "refused";
    }
    return "?";
}

result<index_check_report> check_index(index_check_settings const& settings)
{
    if (!settings.cache || !settings.git) return fail("checking the index needs the cache and git");
    return checker{settings}.run();
}

std::string summary_json(index_check_report const& report)
{
    json reasons = json::array();
    for (std::string const& e : report.errors) reasons.push_back(e);
    for (std::string const& r : report.reasons) reasons.push_back(r);
    json const summary{{"format", 1}, {"verdict", verdict_name(report.verdict)}, {"reasons", reasons}, {"notes", report.notes}};
    return summary.dump(2) + "\n";
}

namespace {

constexpr std::string_view comment_marker = "<!-- otok-index-check ";

}

std::string check_comment(index_check_report const& report)
{
    // Markdown of each item on its own: a line of a list, with later lines indented under it
    auto const items = [](std::vector<std::string> const& list) {
        std::string text;
        for (std::string const& item : list) {
            text += "- ";
            for (char c : item) text += c == '\n' ? std::string{"\n  "} : std::string(1, c);
            text += "\n";
        }
        return text;
    };
    std::string text;
    switch (report.verdict) {
    case index_verdict::automatic:
        text = "**Passed the checks of the index**: the workflow merges it now.\n";
        break;
    case index_verdict::maintainer:
        text = "**Waits for an index maintainer**: it passed the checks, but index CI does not merge it itself:\n\n" + items(report.reasons);
        break;
    case index_verdict::refused:
        text = "**Refused by the checks of the index**:\n\n" + items(report.errors);
        if (!report.reasons.empty()) text += "\nIt would also wait for an index maintainer:\n\n" + items(report.reasons);
        break;
    }
    if (!report.notes.empty()) text += "\nNotes:\n\n" + items(report.notes);
    // the summary, for otok publish; no '>' in it to close the HTML comment
    std::string summary = json::parse(summary_json(report)).dump();
    for (std::size_t at = summary.find('>'); at != std::string::npos; at = summary.find('>', at)) summary.replace(at, 1, "\\u003e");
    text += "\n" + std::string{comment_marker} + summary + " -->\n";
    return text;
}

std::optional<std::string> summary_of_comment(std::string_view comment)
{
    std::size_t const start = comment.find(comment_marker);
    if (start == std::string_view::npos) return std::nullopt;
    std::string_view rest = comment.substr(start + comment_marker.size());
    std::size_t const end = rest.find(" -->");
    if (end == std::string_view::npos) return std::nullopt;
    auto const summary = json::parse(rest.substr(0, end), nullptr, false);
    if (!summary.is_object() || !summary.contains("verdict")) return std::nullopt;
    return std::string{rest.substr(0, end)};
}

namespace {

// Whether an archive answers at one of its locations (index.md 11).
bool archive_answers(http_transport const& http, input_location const& l)
{
    std::vector<std::string> urls{l.url};
    urls.insert(urls.end(), l.mirrors.begin(), l.mirrors.end());
    for (std::string const& url : urls) {
        http_exchange head;
        head.method = "HEAD";
        head.url = url;
        if (auto r = http(head); r && r->status >= 200 && r->status < 300) return true;
        http_exchange first_byte;
        first_byte.url = url;
        first_byte.headers = {"Range: bytes=0-0"};
        first_byte.max_body = 1024;
        if (auto r = http(first_byte); r && (r->status == 200 || r->status == 206)) return true;
    }
    return false;
}

}

result<index_probe_result> probe_index(index_probe_settings const& s)
{
    if (!s.cache || !s.git) return fail("probing the index needs the cache and git");
    fs::path const repository = s.cache->tmp() / "probe.git";
    fs::path const log = s.cache->tmp() / "index-probe.log";
    std::error_code ec;
    fs::remove_all(repository, ec);
    fs::create_directories(repository, ec);
    if (auto ok = run_git(*s.git, {"init", "-q", "--bare"}, repository, log); !ok) return std::unexpected{ok.error()};
    std::map<std::pair<std::string, std::string>, bool> commits;  // (url, commit) → available
    auto const commit_answers = [&](std::string const& url, std::string const& commit) {
        auto const key = std::pair{url, commit};
        if (auto it = commits.find(key); it != commits.end()) return it->second;
        bool const found = run_git(*s.git, {"fetch", "-q", "--depth", "1", "--filter=tree:0", url, commit}, repository, log).has_value();
        commits[key] = found;
        return found;
    };

    input_availability previous;
    if (auto content = read_file(s.root / "availability.toml")) {
        diagnostics d;
        if (auto a = parse_availability(*content, "availability.toml", d, index_reading::ci)) previous = std::move(*a);
        else return fail("the index has an invalid availability.toml:" + messages(d));
    }
    toml::date_time const now = date_time_of(s.now);
    index_probe_result out;
    input_availability fresh;
    fresh.checked = now;

    recipe_index idx{s.root, s.targets, index_reading::ci};
    auto ids = idx.recipe_ids();
    if (!ids) return std::unexpected{ids.error()};
    for (recipe_id const& id : *ids) {
        auto entries = idx.entries(id);
        if (!entries) return std::unexpected{entries.error()};
        std::vector<index_entry const*> sorted;
        for (index_entry const& e : **entries) sorted.push_back(&e);
        if (!sorted.empty()) {
            auto order = idx.order(sorted.front()->projection.library);
            if (!order) return std::unexpected{order.error()};
            std::ranges::sort(sorted, [&](index_entry const* a, index_entry const* b) {
                auto const c = order->compare(*a->projection.version, *b->projection.version);
                return c == std::partial_ordering::less || (c == std::partial_ordering::equivalent && a->projection.revision < b->projection.revision);
            });
        }
        for (index_entry const* e : sorted) {
            ++out.versions;
            std::vector<std::string> missing;
            auto const probe = [&](std::string const& what, input_location const& l) {
                if (l.type == input_location::kind::archive) {
                    if (!archive_answers(s.http, l)) missing.push_back(what + " " + l.url);
                } else if (!commit_answers(l.git, l.commit)) {
                    missing.push_back(what + " " + l.git + " " + l.commit);
                }
            };
            if (!commit_answers(e->recipe_source.git, e->recipe_source.commit)) missing.push_back("recipe " + e->recipe_source.git + " " + e->recipe_source.commit);
            probe("source", e->projection.source.location);
            for (recipe_resource const& res : e->projection.resources) probe("resource " + res.name, res.location);
            if (missing.empty()) continue;
            std::string const version = e->projection.version->str() + "-r" + std::to_string(e->projection.revision);
            // the variants of one version and revision share a line
            auto const known = std::ranges::find_if(fresh.unavailable, [&](unavailable_version const& u) { return u.recipe == id && u.version == version; });
            if (known != fresh.unavailable.end()) {
                for (std::string& m : missing) {
                    if (std::ranges::find(known->inputs, m) == known->inputs.end()) known->inputs.push_back(std::move(m));
                }
                continue;
            }
            unavailable_version const* before = previous.find(id, version);
            fresh.unavailable.push_back({id, version, before ? before->since : now, std::move(missing)});
        }
    }
    fs::remove_all(repository, ec);
    out.unavailable = fresh.unavailable.size();
    out.availability = write_availability(fresh);
    return out;
}

}
