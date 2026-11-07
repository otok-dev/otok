// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/manifest/dev_manifest.hpp"

#include <system_error>
#include <utility>

#include "otok/core/fs.hpp"
#include "otok/core/names.hpp"
#include "otok/core/paths.hpp"
#include "otok/core/toml_reader.hpp"

namespace otok {

namespace {

std::string in_quotes(std::string_view s)
{
    return "'" + std::string{s} + "'";
}

// A directory path of the file (common 6): it may leave the project.
std::optional<std::string> directory(toml_table& t, std::string_view key, std::optional<std::string> value)
{
    if (value && classify_file_path(*value) == file_path_kind::invalid) {
        t.error(t.position(key), t.describe(key) + " is not a valid directory path: " + in_quotes(*value));
        return std::nullopt;
    }
    return value;
}

}

std::optional<dev_manifest> parse_dev_manifest(std::string_view content, std::string const& file, diagnostics& diags)
{
    std::size_t const errors_before = diags.error_count();
    auto doc = toml_document::parse(content, file, diags);
    if (!doc) return std::nullopt;
    auto root = doc->root();

    if (auto format = root.integer("format"); format && *format != 1) {
        root.error(root.position("format"), root.describe("format") + " is " + std::to_string(*format) + "; this otok reads format 1");
    }
    dev_manifest m;
    if (auto deps = root.table("deps")) {
        for (std::string const& library : deps->keys()) {
            auto t = deps->table(library);
            if (!t) continue;
            if (!is_library_name(library)) {
                t->error(t->position(), in_quotes(library) + " is not a valid library name");
                continue;
            }
            dev_library lib;
            lib.library = library;
            lib.where = t->position();
            bool ok = true;
            for (auto [key, out] : {std::pair{"source", &lib.source}, std::pair{"recipe", &lib.recipe}}) {
                if (!t->contains(key)) continue;
                *out = directory(*t, key, t->string(key));
                if (!*out) ok = false;
            }
            lib.quiet = t->boolean("quiet").value_or(false);
            if (auto resources = t->table("resources")) {
                for (std::string const& name : resources->keys()) {
                    if (auto path = directory(*resources, name, resources->string(name))) lib.resources.emplace(name, std::move(*path));
                    else ok = false;
                }
                resources->finish();
            }
            if (ok && !lib.source && !lib.recipe && lib.resources.empty()) {
                t->error(t->position(), in_quotes(t->path()) + " names no working tree: give 'source', 'resources' or 'recipe'");
            }
            t->finish();
            if (ok) m.deps.emplace(library, std::move(lib));
        }
        deps->finish();
    }
    root.ignore("metadata");
    root.finish();

    if (diags.error_count() != errors_before) return std::nullopt;
    return m;
}

std::optional<dev_manifest> load_dev_manifest(std::filesystem::path const& path, diagnostics& diags)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return dev_manifest{};
    std::string const display = path_to_utf8(path);
    auto content = read_file(path);
    if (!content) {
        diags.error({display}, content.error().message);
        return std::nullopt;
    }
    return parse_dev_manifest(*content, display, diags);
}

}
