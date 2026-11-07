// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/builder/dev_tree.hpp"

#include <sstream>
#include <system_error>

#include <toml++/toml.hpp>

#include "otok/core/fs.hpp"

namespace otok {

namespace fs = std::filesystem;

std::optional<dev_record> read_dev_record(fs::path const& file)
{
    auto content = read_file(file);
    if (!content) return std::nullopt;
    toml::table t;
    try {
        t = toml::parse(*content);
    } catch (toml::parse_error const&) {
        return std::nullopt;
    }
    dev_record r;
    r.key = t["key"].value_or(std::string{});
    r.patches = t["patches"].value_or(std::string{});
    if (toml::table const* files = t["files"].as_table()) {
        for (auto const& [path, value] : *files) {
            toml::array const* stamp = value.as_array();
            if (!stamp || stamp->size() != 2) return std::nullopt;
            auto const size = stamp->get(0)->value<std::int64_t>();
            auto const time = stamp->get(1)->value<std::int64_t>();
            if (!size || !time) return std::nullopt;
            r.files.emplace(std::string{path.str()}, file_stamp{static_cast<std::uintmax_t>(*size), *time});
        }
    }
    return r;
}

result<void> write_dev_record(fs::path const& file, dev_record const& record)
{
    toml::table t;
    t.insert("key", record.key);
    if (!record.patches.empty()) t.insert("patches", record.patches);
    toml::table files;
    for (auto const& [path, stamp] : record.files) {
        files.insert(path, toml::array{static_cast<std::int64_t>(stamp.size), stamp.time});
    }
    t.insert("files", std::move(files));
    std::ostringstream text;
    text << "# The record of this work directory of a dev slice (otok-dev-toml.md 4.1).\n" << t << "\n";
    return write_file_atomic(file, text.str());
}

namespace {

result<file_stamp> stamp_of(fs::path const& file)
{
    std::error_code ec;
    std::uintmax_t const size = fs::file_size(file, ec);
    if (ec) return fail("cannot read '" + path_to_utf8(file) + "': " + ec.message());
    fs::file_time_type const time = fs::last_write_time(file, ec);
    if (ec) return fail("cannot read '" + path_to_utf8(file) + "': " + ec.message());
    return file_stamp{size, static_cast<std::int64_t>(time.time_since_epoch().count())};
}

// Copies a file with its modification time, which the build tools compare.
result<void> copy_with_time(fs::path const& from, fs::path const& to)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (fs::is_symlink(fs::symlink_status(to, ec))) fs::remove(to, ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::last_write_time(to, fs::last_write_time(from, ec), ec);
    if (ec) return fail("cannot copy '" + path_to_utf8(from) + "': " + ec.message());
    return {};
}

}

result<std::set<std::string>> sync_trees(std::vector<synced_tree> const& trees, fs::path const& source, dev_record const& old,
    dev_record& now, std::set<std::string> const& force, std::map<std::string, fs::path>& origins)
{
    std::set<std::string> copied;
    std::error_code ec;
    auto const file = [&](fs::path const& from, std::string const& relative) -> result<void> {
        auto stamp = stamp_of(from);
        if (!stamp) return std::unexpected{stamp.error()};
        now.files.insert_or_assign(relative, *stamp);
        origins.insert_or_assign(relative, from);
        fs::path const to = source / path_from_utf8(relative);
        auto const was = old.files.find(relative);
        if (was != old.files.end() && was->second == *stamp && !force.contains(relative) && fs::exists(to, ec)) return {};
        if (auto ok = copy_with_time(long_path(from), long_path(to)); !ok) return ok;
        copied.insert(relative);
        return {};
    };
    // an entry of a tree: `in_tree` relative to its root, `/`-separated
    auto const entry = [&](synced_tree const& tree, std::string const& in_tree, fs::file_status const& status) -> result<void> {
        std::string const relative = (tree.dest.empty() ? std::string{} : tree.dest + "/") + in_tree;
        fs::path const from = tree.from / path_from_utf8(in_tree);
        if (fs::is_directory(status)) return {};
        if (!fs::is_symlink(status)) return file(from, relative);
        // a link is made again when its target changed
        fs::path const target = fs::read_symlink(long_path(from), ec);
        fs::path const to = long_path(source / path_from_utf8(relative));
        std::error_code missing;
        if (fs::read_symlink(to, missing) != target || missing) {
            fs::remove(to, missing);
            fs::create_directories(to.parent_path(), missing);
            fs::copy_symlink(long_path(from), to, ec);
            if (ec) return fail("cannot copy '" + path_to_utf8(from) + "': " + ec.message());
            copied.insert(relative);
        }
        now.files.insert_or_assign(relative, file_stamp{});
        return {};
    };
    for (synced_tree const& tree : trees) {
        if (!fs::is_directory(tree.from, ec)) {
            if (auto ok = file(tree.from, tree.dest); !ok) return std::unexpected{ok.error()};
            continue;
        }
        if (tree.files) {
            for (std::string const& in_tree : *tree.files) {
                // listed and gone (a tracked file deleted): not in the tree
                fs::file_status const status = fs::symlink_status(long_path(tree.from / path_from_utf8(in_tree)), ec);
                if (!fs::exists(status)) continue;
                if (auto ok = entry(tree, in_tree, status); !ok) return std::unexpected{ok.error()};
            }
            continue;
        }
        fs::path const root = long_path(tree.from);
        fs::recursive_directory_iterator it{root, fs::directory_options::none, ec};
        if (ec) return fail("cannot read '" + path_to_utf8(tree.from) + "': " + ec.message());
        for (; it != fs::recursive_directory_iterator{}; it.increment(ec)) {
            if (ec) return fail("cannot read '" + path_to_utf8(tree.from) + "': " + ec.message());
            // the repository of a checkout and those of its submodules are not sources
            if (it->path().filename() == ".git") {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            fs::file_status const status = it->symlink_status(ec);
            if (auto ok = entry(tree, path_to_utf8(it->path().lexically_relative(root).generic_u8string()), status); !ok) {
                return std::unexpected{ok.error()};
            }
        }
    }
    for (auto const& [relative, stamp] : old.files) {
        if (!now.files.contains(relative)) fs::remove(long_path(source / path_from_utf8(relative)), ec);
    }
    return copied;
}

std::set<std::string> patched_files(std::string_view patch)
{
    std::set<std::string> files;
    std::size_t at = 0;
    while (at < patch.size()) {
        std::size_t end = patch.find('\n', at);
        if (end == std::string_view::npos) end = patch.size();
        std::string_view line = patch.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        for (std::string_view prefix : {"--- a/", "+++ b/"}) {
            if (!line.starts_with(prefix)) continue;
            std::string_view path = line.substr(prefix.size());
            if (std::size_t const tab = path.find('\t'); tab != std::string_view::npos) path = path.substr(0, tab);
            files.emplace(path);
        }
        at = end + 1;
    }
    return files;
}

}
