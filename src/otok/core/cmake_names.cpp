// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/cmake_names.hpp"

#include <algorithm>
#include <map>
#include <optional>

#include "otok/core/names.hpp"

namespace otok {

namespace {

bool is_letter(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_word(char c) noexcept { return is_letter(c) || is_digit(c) || c == '_' || c == '.' || c == '+' || c == '-'; }

std::string in_quotes(std::string_view s) { return "'" + std::string{s} + "'"; }

// A list of products: one name or an array of them, none empty, none twice.
std::optional<std::vector<std::string>> products_of(toml_table& t, std::string const& key)
{
    toml::node const* node = t.node(key);
    if (!node) return std::nullopt;
    std::vector<std::string> products;
    if (node->is_string()) {
        products.push_back(*t.string(key));
    } else if (node->is_array()) {
        auto list = t.strings(key);
        if (!list) return std::nullopt;
        products = std::move(*list);
        if (products.empty()) {
            t.error(t.position(key), t.describe(key) + " names no product");
            return std::nullopt;
        }
    } else {
        t.error(t.position(key), t.describe(key) + " must be a product name or a list of them, not " + std::string{describe_type(node->type())});
        return std::nullopt;
    }
    bool ok = true;
    for (auto it = products.begin(); it != products.end(); ++it) {
        if (!is_product_name(*it)) {
            t.error(t.position(key), in_quotes(*it) + " in " + t.describe(key) + " is not a valid product name ([A-Za-z_][A-Za-z0-9_]*)");
            ok = false;
        } else if (std::find(products.begin(), it, *it) != it) {
            t.error(t.position(key), t.describe(key) + " names the product " + in_quotes(*it) + " twice");
            ok = false;
        }
    }
    if (!ok) return std::nullopt;
    return products;
}

// The alternatives of the one group of braces of a target key (`ZLIB::{ZLIB,ZLIBSTATIC}`, recipe-toml.md 11.4) as names;
// nothing, with `why`, when the braces break the rules.
std::optional<std::vector<std::string>> brace_alternatives(std::string_view key, std::string& why)
{
    std::size_t const open = key.find('{');
    std::size_t const close = key.find('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open || key.find('{', open + 1) != std::string_view::npos
        || key.find('}', close + 1) != std::string_view::npos) {
        why = "has one group of braces { … } at most";
        return std::nullopt;
    }
    std::size_t const last = key.rfind("::");
    if (last == std::string_view::npos || open < last + 2) {
        why = "has braces in its namespace: they go in its last part, after the last '::'";
        return std::nullopt;
    }
    std::vector<std::string> alternatives;
    std::string_view const inner = key.substr(open + 1, close - open - 1);
    for (std::size_t at = 0;;) {
        std::size_t const comma = inner.find(',', at);
        alternatives.emplace_back(inner.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    if (alternatives.size() < 2) {
        why = "needs two alternatives or more in its braces";
        return std::nullopt;
    }
    std::vector<std::string> names;
    for (auto it = alternatives.begin(); it != alternatives.end(); ++it) {
        if (std::find(alternatives.begin(), it, *it) != it) {
            why = "has the alternative '" + *it + "' twice in its braces";
            return std::nullopt;
        }
        names.push_back(std::string{key.substr(0, open)} + *it + std::string{key.substr(close + 1)});
    }
    return names;
}

// The targets a key of `targets` names: itself, the alternatives of its braces, or itself as a pattern.
std::vector<cmake_name> target_keys(toml_table& t, std::string const& key, bool& ok)
{
    std::string const form = " is not a target name for a package: <namespace>::<name> of letters, digits, '_', '.', '+' and '-', not in otok::";
    if (key.find('*') != std::string::npos) {
        if (key.find_first_of("{}") != std::string::npos) {
            t.error(t.position(key), t.describe(key) + " is a pattern with braces: give the one or the other");
        } else if (!is_cmake_target_pattern(key)) {
            t.error(t.position(key), t.describe(key) + form + "; '*' goes in its last part, after the last '::'");
        } else {
            return {cmake_name{key, {}, false, true}};
        }
        ok = false;
        return {};
    }
    if (key.find_first_of("{}") != std::string::npos) {
        std::string why;
        auto names = brace_alternatives(key, why);
        if (!names) {
            t.error(t.position(key), t.describe(key) + " " + why);
            ok = false;
            return {};
        }
        std::vector<cmake_name> out;
        for (std::string const& name : *names) {
            if (!is_cmake_target_name(name)) {
                t.error(t.position(key), t.describe(key) + " names '" + name + "', which" + form);
                ok = false;
                return {};
            }
            out.push_back(cmake_name{name, {}});
        }
        return out;
    }
    if (!is_cmake_target_name(key)) {
        t.error(t.position(key), t.describe(key) + form);
        ok = false;
        return {};
    }
    return {cmake_name{key, {}}};
}

// `targets`, `components`, `variables` of a package: names → products.
std::vector<cmake_name> names_of(toml_table& package, std::string_view key, bool& ok)
{
    std::vector<cmake_name> names;
    auto t = package.table(key);
    if (!t) {
        if (package.contains(key)) ok = false;
        return names;
    }
    for (std::string const& name : t->keys()) {
        std::vector<cmake_name> named{cmake_name{name, {}}};
        if (key == "targets") {
            named = target_keys(*t, name, ok);
        } else if (key == "components" && !is_cmake_package_name(name)) {
            t->error(t->position(name), t->describe(name) + " is not a valid component name ([A-Za-z0-9_.+-]{1,64})");
            ok = false;
        } else if (key == "variables" && !is_cmake_variable_name(name)) {
            t->error(t->position(name), t->describe(name) + " is not a valid variable name ([A-Za-z_][A-Za-z0-9_]*)");
            ok = false;
        }
        std::vector<std::string> products;
        bool include = false;
        toml::node const* node = t->node(name);
        if (key == "variables" && node && node->is_table()) {
            auto v = t->table(name);
            if (auto product = v->string("include", need::required)) {
                if (is_product_name(*product)) {
                    products.push_back(std::move(*product));
                    include = true;
                } else {
                    v->error(v->position("include"), in_quotes(*product) + " in " + v->describe("include") + " is not a valid product name ([A-Za-z_][A-Za-z0-9_]*)");
                    ok = false;
                }
            } else {
                ok = false;
            }
            v->finish();
        } else if (auto listed = products_of(*t, name)) {
            products = std::move(*listed);
        } else {
            ok = false;
        }
        for (cmake_name& n : named) {
            if (std::ranges::find(names, n.name, &cmake_name::name) != names.end()) {
                t->error(t->position(name), "the target '" + n.name + "' is named twice in " + t->path());
                ok = false;
                continue;
            }
            n.products = products;
            n.include = include;
            names.push_back(std::move(n));
        }
    }
    t->finish();
    return names;
}

}

bool is_cmake_package_name(std::string_view name) noexcept
{
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, is_word);
}

bool is_cmake_target_name(std::string_view name) noexcept
{
    if (name.size() > 128 || cmake_lower(name).starts_with("otok::")) return false;
    std::size_t separators = 0;
    for (std::size_t at = 0;;) {
        std::size_t const end = name.find("::", at);
        std::string_view const part = name.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        if (part.empty() || !std::ranges::all_of(part, is_word)) return false;
        if (end == std::string_view::npos) break;
        ++separators;
        at = end + 2;
    }
    return separators >= 1;
}

bool is_cmake_target_pattern(std::string_view pattern) noexcept
{
    std::size_t const last = pattern.rfind("::");
    std::size_t const star = pattern.find('*');
    if (star == std::string_view::npos || last == std::string_view::npos || star < last + 2) return false;
    std::string name{pattern};
    std::ranges::replace(name, '*', 'x');
    return is_cmake_target_name(name);
}

namespace {

// `*` any run of characters of `text`, the rest as it is.
bool glob_matches(std::string_view pattern, std::string_view text) noexcept
{
    if (pattern.empty()) return text.empty();
    if (pattern.front() == '*') {
        for (std::size_t skip = 0; skip <= text.size(); ++skip) {
            if (glob_matches(pattern.substr(1), text.substr(skip))) return true;
        }
        return false;
    }
    return !text.empty() && text.front() == pattern.front() && glob_matches(pattern.substr(1), text.substr(1));
}

}

bool cmake_target_matches(std::string_view pattern, std::string_view name) noexcept
{
    std::size_t const p = pattern.rfind("::");
    std::size_t const n = name.rfind("::");
    if (p == std::string_view::npos || n == std::string_view::npos || pattern.substr(0, p) != name.substr(0, n)) return false;
    return glob_matches(pattern.substr(p + 2), name.substr(n + 2));
}

bool is_cmake_variable_name(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 128 || (!is_letter(name.front()) && name.front() != '_')) return false;
    return std::ranges::all_of(name, [](char c) { return is_letter(c) || is_digit(c) || c == '_'; });
}

std::string cmake_lower(std::string_view text)
{
    std::string out{text};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string cmake_upper(std::string_view text)
{
    std::string out{text};
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

std::vector<cmake_package_spec> read_cmake_packages(toml_table& table)
{
    std::vector<cmake_package_spec> packages;
    std::map<std::string, std::string> folded;  // lower case → as written
    for (std::string const& name : table.keys()) {
        auto t = table.table(name, need::required);
        if (!t) continue;
        bool ok = true;
        if (!is_cmake_package_name(name)) {
            table.error(table.position(name), in_quotes(name) + " in " + table.describe(name) + " is not a valid CMake package name ([A-Za-z0-9_.+-]{1,64})");
            ok = false;
        } else if (auto [it, added] = folded.emplace(cmake_lower(name), name); !added) {
            table.error(table.position(name), "the CMake packages " + in_quotes(it->second) + " and " + in_quotes(name)
                + " differ only in case, which find_package does not tell apart");
            ok = false;
        }
        cmake_package_spec p{name, {}, {}, {}, t->position()};
        if (!t->contains("targets")) {
            t->error(t->position(), "missing required key " + t->describe("targets"));
            ok = false;
        }
        p.targets = names_of(*t, "targets", ok);
        if (t->contains("targets") && p.targets.empty() && ok) {
            t->error(t->position("targets"), t->describe("targets") + " names no target");
            ok = false;
        }
        p.components = names_of(*t, "components", ok);
        p.variables = names_of(*t, "variables", ok);
        t->finish();
        if (ok) packages.push_back(std::move(p));
    }
    return packages;
}

std::vector<std::string> cmake_products(std::vector<cmake_package_spec> const& packages)
{
    std::vector<std::string> products;
    for (cmake_package_spec const& p : packages) {
        for (auto const* names : {&p.targets, &p.components, &p.variables}) {
            for (cmake_name const& n : *names) {
                for (std::string const& product : n.products) {
                    if (std::ranges::find(products, product) == products.end()) products.push_back(product);
                }
            }
        }
    }
    return products;
}

}
