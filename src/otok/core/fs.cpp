// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/fs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "otok/core/process.hpp"

#ifdef __APPLE__
#   include <cerrno>
#   include <sys/clonefile.h>
#endif

namespace otok {

std::filesystem::path path_from_utf8(std::string_view utf8)
{
    return std::filesystem::path{std::u8string{reinterpret_cast<char8_t const*>(utf8.data()), utf8.size()}};
}

std::string path_to_utf8(std::filesystem::path const& path)
{
    std::u8string const s = path.u8string();
    return std::string{reinterpret_cast<char const*>(s.data()), s.size()};
}

std::filesystem::path long_path(std::filesystem::path const& path)
{
#ifdef _WIN32
    std::wstring const given = path.native();
    if (given.empty() || given.starts_with(LR"(\\?\)")) return path;
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec) return path;
    absolute.make_preferred();  // the extended form takes no '/'
    std::wstring const text = absolute.native();
    if (text.starts_with(LR"(\\)")) return std::filesystem::path{LR"(\\?\UNC\)" + text.substr(2)};
    return std::filesystem::path{LR"(\\?\)" + text};
#else
    return path;
#endif
}

result<std::string> read_file(std::filesystem::path const& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return fail("cannot open '" + path_to_utf8(path) + "'");
    }
    std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (in.bad()) {
        return fail("cannot read '" + path_to_utf8(path) + "'");
    }
    return content;
}

result<void> write_file_atomic(std::filesystem::path const& path, std::string_view content)
{
    // a name of its own per write: threads and processes may replace the same file at once (the state of an index read by
    // the projects otok publish builds together)
    static std::atomic<unsigned> writes{0};
    std::filesystem::path temporary = path;
    temporary += "." + std::to_string(current_process_id()) + "-" + std::to_string(++writes) + ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out) return fail("cannot write '" + path_to_utf8(temporary) + "'");
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) return fail("cannot write '" + path_to_utf8(temporary) + "'");
    }
    std::error_code ec;
    // Windows refuses to replace a file another thread is replacing at that moment: tried again a while
    for (int attempt = 0;; ++attempt) {
        std::filesystem::rename(temporary, path, ec);
        if (ec != std::errc::permission_denied || attempt == 100) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (ec) {
        std::string const reason = ec.message();
        std::filesystem::remove(temporary, ec);
        return fail("cannot replace '" + path_to_utf8(path) + "': " + reason);
    }
    return {};
}

result<bool> write_file_if_changed(std::filesystem::path const& path, std::string_view content)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        if (auto old = read_file(path); old && *old == content) return false;
    }
    if (auto ok = write_file_atomic(path, content); !ok) return std::unexpected{ok.error()};
    return true;
}

result<void> copy_tree(std::filesystem::path const& from, std::filesystem::path const& to)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(to, ec))) return fail("'" + path_to_utf8(to) + "' already exists");
    if (to.has_parent_path()) {
        fs::create_directories(to.parent_path(), ec);
        if (ec) return fail("cannot create '" + path_to_utf8(to.parent_path()) + "': " + ec.message());
    }
#ifdef __APPLE__
    if (::clonefile(from.c_str(), to.c_str(), CLONE_NOFOLLOW) == 0) return {};
    if (errno != ENOTSUP && errno != EXDEV) {
        return fail("cannot copy '" + path_to_utf8(from) + "' to '" + path_to_utf8(to) + "': " + std::generic_category().message(errno));
    }
    // another file system, or one without copy-on-write: copy
#endif
    fs::path const source_root = long_path(from), target_root = long_path(to);
    auto const failed = [&](fs::path const& path, std::error_code const& error) {
        std::error_code ignored;
        fs::remove_all(target_root, ignored);
        return fail("cannot copy '" + path_to_utf8(path) + "': " + error.message());
    };
    if (!fs::create_directory(target_root, ec)) return failed(to, ec);
    fs::recursive_directory_iterator it{source_root, fs::directory_options::none, ec};
    if (ec) return failed(from, ec);
    for (; it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) return failed(from, ec);
        fs::path const source = it->path();
        fs::path const target = target_root / source.lexically_relative(source_root);
        fs::file_status const status = it->symlink_status(ec);
        if (ec) return failed(source, ec);
        if (fs::is_symlink(status)) {
            fs::copy_symlink(source, target, ec);
        } else if (fs::is_directory(status)) {
            fs::create_directory(target, source, ec);  // with the source's permissions
        } else {
            fs::copy_file(source, target, ec);
            // copy_file does not keep times everywhere; builds compare them (make, autotools)
            if (!ec) {
                fs::file_time_type const time = fs::last_write_time(source, ec);
                if (!ec) fs::last_write_time(target, time, ec);
            }
        }
        if (ec) return failed(source, ec);
    }
    return {};
}

std::string first_difference(std::filesystem::path const& a, std::filesystem::path const& b)
{
    auto const listing = [](std::filesystem::path const& root) {
        std::set<std::string> files;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator{root, ec}; !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
            if (it->is_regular_file(ec)) files.insert(path_to_utf8(it->path().lexically_relative(root).generic_u8string()));
        }
        return files;
    };
    std::set<std::string> const x = listing(a), y = listing(b);
    std::string ba(65536, char{}), bb(65536, char{});
    for (auto ix = x.begin(), iy = y.begin(); ix != x.end() || iy != y.end();) {
        if (iy == y.end() || (ix != x.end() && *ix < *iy)) return *ix;
        if (ix == x.end() || *iy < *ix) return *iy;
        std::filesystem::path const fa = a / path_from_utf8(*ix), fb = b / path_from_utf8(*iy);
        std::error_code ec;
        if (std::filesystem::file_size(fa, ec) != std::filesystem::file_size(fb, ec)) return *ix;
        std::ifstream sa{fa, std::ios::binary}, sb{fb, std::ios::binary};
        while (sa && sb) {
            sa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
            sb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
            if (sa.gcount() != sb.gcount() || !std::equal(ba.begin(), ba.begin() + sa.gcount(), bb.begin())) return *ix;
        }
        ++ix;
        ++iy;
    }
    return {};
}

result<generated_files_written> write_generated_files(std::filesystem::path const& root, std::map<std::string, std::string> const& files,
    std::span<std::string const> owned)
{
    generated_files_written done;
    std::error_code ec;
    for (auto const& [path, contents] : files) {
        std::filesystem::path const file = root / path_from_utf8(path);
        if (auto old = read_file(file); old && *old == contents) {
            ++done.unchanged;
            continue;
        }
        std::filesystem::create_directories(file.parent_path(), ec);
        if (auto ok = write_file_atomic(file, contents); !ok) return std::unexpected{ok.error()};
        ++done.written;
    }
    std::vector<std::filesystem::path> stale;
    std::vector<std::filesystem::path> directories;
    for (std::string const& part : owned) {
        std::filesystem::path const top = root / path_from_utf8(part);
        if (std::filesystem::is_regular_file(top, ec)) {
            if (!files.contains(part)) stale.push_back(top);
            continue;
        }
        if (!std::filesystem::is_directory(top, ec)) continue;
        for (auto const& item : std::filesystem::recursive_directory_iterator{top, ec}) {
            if (item.is_directory(ec)) {
                directories.push_back(item.path());
                continue;
            }
            if (!files.contains(path_to_utf8(item.path().lexically_relative(root).generic_u8string()))) stale.push_back(item.path());
        }
        if (ec) return fail("cannot read '" + path_to_utf8(top) + "': " + ec.message());
    }
    for (std::filesystem::path const& file : stale) {
        if (!std::filesystem::remove(file, ec) || ec) return fail("cannot remove '" + path_to_utf8(file) + "': " + ec.message());
        ++done.removed;
    }
    std::ranges::sort(directories, std::greater{});  // the deepest first
    for (std::filesystem::path const& d : directories) {
        if (std::filesystem::is_empty(d, ec)) std::filesystem::remove(d, ec);
    }
    return done;
}

}
