// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/target/apple/privacy.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include "otok/core/diagnostics.hpp"
#include "otok/core/toml_reader.hpp"

namespace otok::target::apple {

namespace {

// index.md 10.1, from Apple's "Describing use of required reason API"
constexpr std::string_view builtin_list = R"(format = 1

[[api]]
category = "NSPrivacyAccessedAPICategoryFileTimestamp"
symbols = ["stat", "fstat", "lstat", "fstatat", "getattrlist", "fgetattrlist", "getattrlistat", "getattrlistbulk",
           "NSFileCreationDate", "NSFileModificationDate", "NSURLCreationDateKey", "NSURLContentModificationDateKey"]

[[api]]
category = "NSPrivacyAccessedAPICategorySystemBootTime"
symbols = ["mach_absolute_time"]

[[api]]
category = "NSPrivacyAccessedAPICategoryDiskSpace"
symbols = ["statfs", "statvfs", "fstatfs", "fstatvfs", "getattrlist", "fgetattrlist", "getattrlistat",
           "NSFileSystemFreeSize", "NSFileSystemSize", "NSURLVolumeAvailableCapacityKey",
           "NSURLVolumeAvailableCapacityForImportantUsageKey", "NSURLVolumeAvailableCapacityForOpportunisticUsageKey",
           "NSURLVolumeTotalCapacityKey"]

[[api]]
category = "NSPrivacyAccessedAPICategoryActiveKeyboards"
classes = ["UITextInputMode"]

[[api]]
category = "NSPrivacyAccessedAPICategoryUserDefaults"
classes = ["NSUserDefaults"]
)";

// A value of an XML property list: what the check reads of it.
struct plist_value
{
    enum class kind { dict, array, string, other } type = kind::other;
    std::string text;
    std::vector<std::pair<std::string, plist_value>> entries;
    std::vector<plist_value> items;
};

// A reader of XML property lists: elements, text with the five entities and character references,
// comments, the XML declaration and the doctype.
class plist_reader
{
public:
    explicit plist_reader(std::string_view text) : text_{text} {}

    result<plist_value> read()
    {
        if (text_.starts_with("bplist")) return fail("it is a binary property list; otok reads XML ones");
        skip_misc();
        auto tag = open_tag();
        if (!tag) return std::unexpected{tag.error()};
        if (tag->name != "plist" || tag->empty) return fail("it is not a property list: no <plist> element");
        auto value = read_value();
        if (!value) return value;
        if (auto ok = close_tag("plist"); !ok) return std::unexpected{ok.error()};
        skip_misc();
        if (pos_ != text_.size()) return fail("text after </plist>");
        return value;
    }

private:
    struct tag
    {
        std::string name;
        bool empty = false;     // <name/>
    };

    std::string_view text_;
    std::size_t pos_ = 0;

    bool at(std::string_view s) const { return text_.substr(pos_).starts_with(s); }

    void skip_space()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_;
    }

    // whitespace, comments, <?…?> and <!DOCTYPE …>
    void skip_misc()
    {
        for (;;) {
            skip_space();
            std::string_view end;
            if (at("<!--")) end = "-->";
            else if (at("<?")) end = "?>";
            else if (at("<!")) end = ">";
            else return;
            std::size_t const found = text_.find(end, pos_);
            pos_ = found == std::string_view::npos ? text_.size() : found + end.size();
        }
    }

    result<tag> open_tag()
    {
        if (!at("<") || at("</")) return fail("an element expected at offset " + std::to_string(pos_));
        std::size_t const end = text_.find('>', pos_);
        if (end == std::string_view::npos) return fail("an unterminated element at offset " + std::to_string(pos_));
        std::string_view inside = text_.substr(pos_ + 1, end - pos_ - 1);
        pos_ = end + 1;
        tag t;
        if (inside.ends_with('/')) {
            t.empty = true;
            inside.remove_suffix(1);
        }
        t.name = std::string{inside.substr(0, inside.find_first_of(" \t\r\n"))};
        return t;
    }

    result<void> close_tag(std::string_view name)
    {
        skip_misc();
        std::string const expected = "</" + std::string{name} + ">";
        if (!at(expected)) return fail(expected + " expected at offset " + std::to_string(pos_));
        pos_ += expected.size();
        return {};
    }

    result<std::string> read_text(std::string_view name)
    {
        std::string const end = "</" + std::string{name} + ">";
        std::size_t const found = text_.find(end, pos_);
        if (found == std::string_view::npos) return fail(end + " is missing");
        std::string_view raw = text_.substr(pos_, found - pos_);
        pos_ = found + end.size();
        std::string out;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            std::size_t const semi = raw.find(';', i);
            if (semi == std::string_view::npos) return fail("an unterminated entity in <" + std::string{name} + ">");
            std::string_view const entity = raw.substr(i + 1, semi - i - 1);
            static constexpr std::pair<std::string_view, char> named[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}};
            auto const it = std::ranges::find(named, entity, &std::pair<std::string_view, char>::first);
            if (it != std::end(named)) {
                out += it->second;
            } else if (entity.starts_with('#') && entity.size() > 1) {
                bool const hex = entity[1] == 'x';
                unsigned long code = 0;
                try {
                    code = std::stoul(std::string{entity.substr(hex ? 2 : 1)}, nullptr, hex ? 16 : 10);
                } catch (std::exception const&) {
                    return fail("a bad character reference '&" + std::string{entity} + ";'");
                }
                // UTF-8
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else if (code < 0x10000) {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (code >> 18));
                    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
            } else {
                return fail("an unknown entity '&" + std::string{entity} + ";'");
            }
            i = semi;
        }
        return out;
    }

    result<plist_value> read_value()
    {
        skip_misc();
        auto t = open_tag();
        if (!t) return std::unexpected{t.error()};
        plist_value v;
        if (t->name == "dict") {
            v.type = plist_value::kind::dict;
            if (t->empty) return v;
            for (;;) {
                skip_misc();
                if (at("</")) break;
                auto key = open_tag();
                if (!key) return std::unexpected{key.error()};
                if (key->name != "key") return fail("<key> expected in <dict>, not <" + key->name + ">");
                std::string name;
                if (!key->empty) {
                    auto text = read_text("key");
                    if (!text) return std::unexpected{text.error()};
                    name = std::move(*text);
                }
                auto value = read_value();
                if (!value) return value;
                v.entries.emplace_back(std::move(name), std::move(*value));
            }
            if (auto ok = close_tag("dict"); !ok) return std::unexpected{ok.error()};
        } else if (t->name == "array") {
            v.type = plist_value::kind::array;
            if (t->empty) return v;
            for (;;) {
                skip_misc();
                if (at("</")) break;
                auto item = read_value();
                if (!item) return item;
                v.items.push_back(std::move(*item));
            }
            if (auto ok = close_tag("array"); !ok) return std::unexpected{ok.error()};
        } else if (t->name == "string" || t->name == "integer" || t->name == "real" || t->name == "date" || t->name == "data") {
            v.type = t->name == "string" ? plist_value::kind::string : plist_value::kind::other;
            if (t->empty) return v;
            auto text = read_text(t->name);
            if (!text) return std::unexpected{text.error()};
            v.text = std::move(*text);
        } else if (t->name == "true" || t->name == "false") {
            if (!t->empty) {
                if (auto ok = close_tag(t->name); !ok) return std::unexpected{ok.error()};
            }
        } else {
            return fail("an unknown element <" + t->name + ">");
        }
        return v;
    }
};

plist_value const* entry(plist_value const& dict, std::string_view key)
{
    for (auto const& [k, v] : dict.entries) {
        if (k == key) return &v;
    }
    return nullptr;
}

}

result<std::vector<required_reason_api>> parse_required_reason_apis(std::string_view content, std::string file)
{
    diagnostics diags;
    std::vector<required_reason_api> apis;
    if (auto doc = toml_document::parse(content, file, diags)) {
        auto root = doc->root();
        if (auto format = root.integer("format", need::required); format && *format != 1) {
            root.error(root.position("format"), root.describe("format") + " is " + std::to_string(*format) + "; this otok reads format 1");
        }
        for (toml_table& t : root.tables("api", need::required).value_or(std::vector<toml_table>{})) {
            required_reason_api api;
            api.category = t.string("category", need::required).value_or("");
            api.symbols = t.strings("symbols").value_or(std::vector<std::string>{});
            api.classes = t.strings("classes").value_or(std::vector<std::string>{});
            if (std::ranges::any_of(apis, [&](required_reason_api const& a) { return a.category == api.category; })) {
                t.error(t.position("category"), "the category '" + api.category + "' appears twice");
            }
            t.finish(severity::warning);
            apis.push_back(std::move(api));
        }
        root.finish(severity::warning);
    }
    if (diags.has_errors()) {
        std::string message;
        for (diagnostic const& d : diags.items()) {
            if (d.level == severity::error) message += (message.empty() ? "" : "\n") + format(d);
        }
        return fail(message);
    }
    return apis;
}

std::vector<required_reason_api> const& builtin_required_reason_apis()
{
    static std::vector<required_reason_api> const apis = [] {
        auto parsed = parse_required_reason_apis(builtin_list, "required-reason-apis.toml");
        return parsed ? std::move(*parsed) : std::vector<required_reason_api>{};
    }();
    return apis;
}

result<std::set<std::string>> declared_api_categories(std::string_view manifest)
{
    auto root = plist_reader{manifest}.read();
    if (!root) return std::unexpected{root.error()};
    if (root->type != plist_value::kind::dict) return fail("its top-level value is not a dictionary");
    std::set<std::string> categories;
    plist_value const* types = entry(*root, "NSPrivacyAccessedAPITypes");
    if (!types) return categories;
    if (types->type != plist_value::kind::array) return fail("NSPrivacyAccessedAPITypes is not an array");
    for (plist_value const& item : types->items) {
        plist_value const* type = item.type == plist_value::kind::dict ? entry(item, "NSPrivacyAccessedAPIType") : nullptr;
        if (!type || type->type != plist_value::kind::string) return fail("an item of NSPrivacyAccessedAPITypes has no NSPrivacyAccessedAPIType string");
        categories.insert(type->text);
    }
    return categories;
}

std::vector<std::string> parse_undefined_symbols(std::string_view nm_output)
{
    std::set<std::string> symbols;
    while (!nm_output.empty()) {
        std::size_t const end = nm_output.find('\n');
        std::string_view line = nm_output.substr(0, end);
        nm_output.remove_prefix(end == std::string_view::npos ? nm_output.size() : end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        // "libz.a(inflate.o):", "libz.a (for architecture arm64):"
        if (line.empty() || line.ends_with(':')) continue;
        symbols.emplace(line);
    }
    return {symbols.begin(), symbols.end()};
}

defined_symbols parse_defined_symbols(std::string_view nm_output)
{
    std::set<std::string> all, weak;
    // "0000000000000010 (__TEXT,__text) weak private external __Z3inlv",
    // "0000000000000004 (common) (alignment 2^2) external _x", "... external [no dead strip] _y"
    for (std::string const& line : parse_undefined_symbols(nm_output)) {
        std::size_t const at = line.find(" external ");
        if (at == std::string::npos) continue;
        std::string_view name = std::string_view{line}.substr(at + 10);
        if (name.starts_with("automatically hidden ")) name.remove_prefix(21);  // "weak external automatically hidden"
        while (name.starts_with('[')) {
            std::size_t const end = name.find("] ");
            if (end == std::string_view::npos) break;
            name.remove_prefix(end + 2);
        }
        if (name.empty()) continue;
        std::string_view const before = std::string_view{line}.substr(0, at + 1);
        if (before.find(" weak ") != std::string_view::npos || before.find("(common)") != std::string_view::npos) weak.emplace(name);
        all.emplace(name);
    }
    return {{all.begin(), all.end()}, {weak.begin(), weak.end()}};
}

std::vector<api_use> undeclared_uses(std::span<std::string const> undefined, std::span<required_reason_api const> apis,
    std::set<std::string> const& declared)
{
    std::map<std::string, std::vector<std::string>> by_symbol, by_class;  // name → categories
    for (required_reason_api const& a : apis) {
        for (std::string const& s : a.symbols) by_symbol[s].push_back(a.category);
        for (std::string const& c : a.classes) by_class[c].push_back(a.category);
    }
    constexpr std::string_view class_prefix = "_OBJC_CLASS_$_";
    std::map<std::vector<std::string>, std::set<std::string>> uses;
    for (std::string const& u : undefined) {
        std::string_view name = u;
        std::map<std::string, std::vector<std::string>> const* table = &by_symbol;
        if (name.starts_with(class_prefix)) {
            name.remove_prefix(class_prefix.size());
            table = &by_class;
        } else {
            // Mach-O names: "_stat", "_stat$INODE64"
            if (!name.starts_with('_')) continue;
            name.remove_prefix(1);
            name = name.substr(0, name.find('$'));
        }
        auto const it = table->find(std::string{name});
        if (it == table->end()) continue;
        if (std::ranges::any_of(it->second, [&](std::string const& c) { return declared.contains(c); })) continue;
        std::vector<std::string> categories = it->second;
        std::ranges::sort(categories);
        uses[std::move(categories)].emplace(name);
    }
    std::vector<api_use> out;
    for (auto& [categories, symbols] : uses) out.push_back({categories, {symbols.begin(), symbols.end()}});
    return out;
}


namespace {

// index.md 10.2
constexpr std::string_view builtin_private_list = R"toml(format = 1

[[library]]
path = "usr/lib/libicucore.tbd"
name = "ICU (libicucore)"
)toml";

}

result<std::vector<private_library>> parse_private_libraries(std::string_view content, std::string file)
{
    diagnostics diags;
    std::vector<private_library> libraries;
    if (auto doc = toml_document::parse(content, file, diags)) {
        auto root = doc->root();
        if (auto format = root.integer("format", need::required); format && *format != 1) {
            root.error(root.position("format"), root.describe("format") + " is " + std::to_string(*format) + "; this otok reads format 1");
        }
        for (toml_table& t : root.tables("library", need::required).value_or(std::vector<toml_table>{})) {
            private_library library;
            library.path = t.string("path", need::required).value_or("");
            library.name = t.string("name", need::required).value_or("");
            t.finish(severity::warning);
            libraries.push_back(std::move(library));
        }
        root.finish(severity::warning);
    }
    if (diags.has_errors()) {
        std::string message;
        for (diagnostic const& d : diags.items()) {
            if (d.level == severity::error) message += (message.empty() ? "" : "\n") + format(d);
        }
        return fail(message);
    }
    return libraries;
}

std::vector<private_library> const& builtin_private_libraries()
{
    static std::vector<private_library> const libraries = [] {
        auto parsed = parse_private_libraries(builtin_private_list, "private-libraries.toml");
        return parsed ? std::move(*parsed) : std::vector<private_library>{};
    }();
    return libraries;
}

result<std::set<std::string>> parse_tbd_exports(std::string_view tbd)
{
    if (!tbd.starts_with("--- !tapi-tbd")) return fail("it is not a text stub in the YAML format (tbd version 4)");
    std::set<std::string> symbols;
    std::string_view section;   // the top-level key the lines belong to
    bool in_list = false;       // inside the [ … ] of a symbol key
    while (!tbd.empty()) {
        std::size_t const end = tbd.find('\n');
        std::string_view line = tbd.substr(0, end);
        tbd.remove_prefix(end == std::string_view::npos ? tbd.size() : end + 1);
        if (!in_list) {
            if (!line.empty() && line.front() != ' ' && line.front() != '-') {
                section = line.substr(0, line.find(':'));
                continue;
            }
            if (section != "exports" && section != "reexports") continue;
            std::string_view key = line.substr(line.find_first_not_of(" -"));
            std::size_t const colon = key.find(':');
            if (colon == std::string_view::npos) continue;
            std::string_view const name = key.substr(0, colon);
            if (name != "symbols" && name != "weak-symbols" && name != "thread-local-symbols") continue;
            std::size_t const open = key.find('[', colon);
            if (open == std::string_view::npos) continue;
            line = key.substr(open + 1);
            in_list = true;
        }
        std::size_t const close = line.find(']');
        if (close != std::string_view::npos) {
            line = line.substr(0, close);
            in_list = false;
        }
        while (!line.empty()) {
            std::size_t const comma = line.find(',');
            std::string_view item = line.substr(0, comma);
            line.remove_prefix(comma == std::string_view::npos ? line.size() : comma + 1);
            std::size_t const first = item.find_first_not_of(" \t'\"");
            if (first == std::string_view::npos) continue;
            item = item.substr(first, item.find_last_not_of(" \t'\"\r") - first + 1);
            symbols.emplace(item);
        }
    }
    return symbols;
}

}
