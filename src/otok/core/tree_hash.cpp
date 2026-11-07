// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/tree_hash.hpp"

#include <algorithm>

#include "otok/core/fs.hpp"
#include "otok/core/sha256.hpp"

namespace otok {

namespace {

// The link's target with `/` separators, checked to stay inside the tree.
result<std::string> link_target(std::filesystem::path const& link, std::string const& relative)
{
    std::error_code ec;
    std::filesystem::path const target = std::filesystem::read_symlink(link, ec);
    if (ec) return fail("cannot read the link '" + relative + "': " + ec.message());
    std::string const text = path_to_utf8(target.generic_u8string());
    if (text.empty() || text.find('\n') != std::string::npos) {
        return fail("the link '" + relative + "' has an empty target or one with a newline");
    }
    if (target.is_absolute() || target.has_root_name() || target.has_root_directory()) {
        return fail("the link '" + relative + "' points to the absolute path '" + text + "'; links must stay inside the tree");
    }
    std::filesystem::path const resolved = (path_from_utf8(relative).parent_path() / target).lexically_normal();
    if (!resolved.empty() && *resolved.begin() == "..") {
        return fail("the link '" + relative + "' points to '" + text + "', outside the tree");
    }
    return text;
}

}

result<std::vector<tree_file>> list_tree(std::filesystem::path const& root, tree_links links)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return fail("'" + path_to_utf8(root) + "' is not a directory");
    }

    std::vector<tree_file> files;
    fs::recursive_directory_iterator it{root, fs::directory_options::none, ec};
    if (ec) return fail("cannot read '" + path_to_utf8(root) + "': " + ec.message());

    for (; it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) return fail("cannot read '" + path_to_utf8(root) + "': " + ec.message());
        fs::directory_entry const& entry = *it;
        std::string const relative = path_to_utf8(entry.path().lexically_relative(root).generic_u8string());

        if (it.depth() == 0 && entry.path().filename() == ".git") {
            it.disable_recursion_pending();
            continue;
        }
        fs::file_status const status = entry.symlink_status(ec);
        if (ec) return fail("cannot read '" + relative + "': " + ec.message());
        if (relative.find('\n') != std::string::npos || relative.find('\\') != std::string::npos) {
            return fail("the path '" + relative + "' contains a newline or a backslash");
        }
        if (fs::is_symlink(status)) {
            if (links == tree_links::reject) {
                return fail("'" + relative + "' is a symbolic link; recipe trees cannot contain symbolic links");
            }
            auto target = link_target(entry.path(), relative);
            if (!target) return std::unexpected{target.error()};
            files.push_back({relative, 0, sha256_hex(*target), std::move(*target)});
            continue;  // a link to a directory is not followed: the iterator does not follow links
        }
        if (fs::is_directory(status)) {
            continue;
        }
        if (!fs::is_regular_file(status)) {
            return fail("'" + relative + "' is not a regular file");
        }
        auto sha = sha256_file(entry.path());
        if (!sha) return std::unexpected{sha.error()};
        files.push_back({relative, static_cast<std::uint64_t>(entry.file_size(ec)), std::move(*sha), std::nullopt});
    }

    // std::string compares as unsigned bytes, which is the order recipe-hash.md specifies
    std::ranges::sort(files, {}, &tree_file::path);
    return files;
}

std::string tree_listing(std::vector<tree_file> const& files)
{
    std::size_t size = 0;
    for (tree_file const& f : files) size += (f.link_target ? 8 : 0) + f.sha256.size() + 2 + f.path.size() + 1;
    std::string listing;
    listing.reserve(size);
    for (tree_file const& f : files) {
        if (f.link_target) listing += "symlink:";
        listing += f.sha256;
        listing += "  ";
        listing += f.path;
        listing += '\n';
    }
    return listing;
}

std::string tree_hash(std::vector<tree_file> const& files)
{
    return sha256_hex(tree_listing(files));
}

result<std::string> tree_hash(std::filesystem::path const& root, tree_links links)
{
    auto files = list_tree(root, links);
    if (!files) return std::unexpected{files.error()};
    return tree_hash(*files);
}

}
