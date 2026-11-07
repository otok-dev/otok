// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/fetch/archive.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include <archive.h>
#include <archive_entry.h>

#include "otok/core/fs.hpp"

namespace otok {

namespace {

struct read_deleter
{
    void operator()(archive* a) const noexcept { archive_read_free(a); }
};

struct write_deleter
{
    void operator()(archive* a) const noexcept { archive_write_free(a); }
};

// A path inside the tree, `/`-separated: no absolute path, no `..`, no backslash. "./a/b/" → "a/b";
// the root itself → "".
result<std::string> entry_path(std::string_view name)
{
    if (name.find('\\') != std::string_view::npos) return fail("'" + std::string{name} + "' contains a backslash");
    if (name.starts_with('/') || (name.size() > 1 && name[1] == ':')) return fail("'" + std::string{name} + "' is an absolute path");
    std::string out;
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t const end = std::min(name.find('/', start), name.size());
        std::string_view const part = name.substr(start, end - start);
        if (part == "..") return fail("'" + std::string{name} + "' leaves the archive root");
        if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + std::string{part};
        start = end + 1;
    }
    return out;
}

// A symbolic link must point inside the tree.
result<void> check_link(std::string const& path, std::string_view target)
{
    if (target.empty() || target.starts_with('/') || target.find('\\') != std::string_view::npos
        || (target.size() > 1 && target[1] == ':')) {
        return fail("the link '" + path + "' points to '" + std::string{target} + "'; links must stay inside the archive");
    }
    std::filesystem::path const resolved = (path_from_utf8(path).parent_path() / path_from_utf8(target)).lexically_normal();
    if (!resolved.empty() && *resolved.begin() == "..") {
        return fail("the link '" + path + "' points to '" + std::string{target} + "', outside the archive");
    }
    return {};
}

std::string utf8_name(char const* utf8, char const* fallback)
{
    return utf8 ? utf8 : (fallback ? fallback : "");
}

// The names libarchive writes to disk. POSIX file names are bytes and otok's are UTF-8: given as they
// are, since the _utf8 setters would convert them to the locale's charset, which turns a non-ASCII
// name into an empty one in the C locale (Boost's sources have one). Windows takes UTF-8 to UTF-16.
void set_disk_name(archive_entry* entry, void (*utf8_setter)(archive_entry*, char const*), void (*raw_setter)(archive_entry*, char const*),
    std::string const& name)
{
#ifdef _WIN32
    (void)raw_setter;
    utf8_setter(entry, name.c_str());
#else
    (void)utf8_setter;
    raw_setter(entry, name.c_str());
#endif
}

result<void> unpack(std::filesystem::path const& archive_file, std::filesystem::path const& root)
{
    std::unique_ptr<archive, read_deleter> in{archive_read_new()};
    for (auto const filter : {&archive_read_support_filter_gzip, &archive_read_support_filter_bzip2, &archive_read_support_filter_xz,
             &archive_read_support_filter_zstd}) {
        filter(in.get());
    }
    archive_read_support_filter_none(in.get());
    archive_read_support_format_tar(in.get());
    archive_read_support_format_zip(in.get());
    std::string const display = path_to_utf8(archive_file);
#ifdef _WIN32
    int const opened = archive_read_open_filename_w(in.get(), archive_file.c_str(), 64 * 1024);
#else
    int const opened = archive_read_open_filename(in.get(), archive_file.c_str(), 64 * 1024);
#endif
    if (opened != ARCHIVE_OK) return fail("cannot read the archive '" + display + "': " + archive_error_string(in.get()));

    // Entries are written under `root`, an absolute path, so ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS
    // cannot be used; entry_path() refuses absolute names instead. SECURE_SYMLINKS refuses to write
    // through a link created by an earlier entry.
    std::unique_ptr<archive, write_deleter> out{archive_write_disk_new()};
    archive_write_disk_set_options(out.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS);

    auto const bad = [&](std::string const& message) { return fail("the archive '" + display + "': " + message); };
    archive_entry* entry = nullptr;
    for (;;) {
        int const status = archive_read_next_header(in.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        if (status < ARCHIVE_WARN) return bad(archive_error_string(in.get()));

        std::string const name = utf8_name(archive_entry_pathname_utf8(entry), archive_entry_pathname(entry));
        auto path = entry_path(name);
        if (!path) return bad(path.error().message);
        if (path->empty()) continue;  // the root directory itself

        auto const type = archive_entry_filetype(entry);
        char const* hardlink = archive_entry_hardlink(entry);
        if (hardlink) {
            auto target = entry_path(utf8_name(archive_entry_hardlink_utf8(entry), hardlink));
            if (!target) return bad(target.error().message);
            set_disk_name(entry, archive_entry_set_hardlink_utf8, archive_entry_set_hardlink, path_to_utf8(root / path_from_utf8(*target)));
        } else if (type == AE_IFLNK) {
            std::string const target = utf8_name(archive_entry_symlink_utf8(entry), archive_entry_symlink(entry));
            if (auto ok = check_link(*path, target); !ok) return bad(ok.error().message);
            set_disk_name(entry, archive_entry_set_symlink_utf8, archive_entry_set_symlink, target);
        } else if (type != AE_IFREG && type != AE_IFDIR) {
            return bad("'" + *path + "' is a special file (device, pipe or socket)");
        }
        set_disk_name(entry, archive_entry_set_pathname_utf8, archive_entry_set_pathname, path_to_utf8(root / path_from_utf8(*path)));

        if (archive_write_header(out.get(), entry) < ARCHIVE_WARN) return bad(*path + ": " + archive_error_string(out.get()));
        if (archive_entry_size(entry) > 0 && !hardlink) {
            void const* block = nullptr;
            std::size_t size = 0;
            la_int64_t offset = 0;
            for (;;) {
                int const r = archive_read_data_block(in.get(), &block, &size, &offset);
                if (r == ARCHIVE_EOF) break;
                if (r < ARCHIVE_WARN) return bad(*path + ": " + archive_error_string(in.get()));
                if (archive_write_data_block(out.get(), block, size, offset) < ARCHIVE_WARN) {
                    return bad(*path + ": " + archive_error_string(out.get()));
                }
            }
        }
        if (archive_write_finish_entry(out.get()) < ARCHIVE_WARN) return bad(*path + ": " + archive_error_string(out.get()));
    }
    if (archive_write_close(out.get()) < ARCHIVE_WARN) return bad(archive_error_string(out.get()));
    return {};
}

}

result<void> extract_archive(std::filesystem::path const& archive_file, std::filesystem::path const& destination, top_level root_mode)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(destination, ec)) return fail("'" + path_to_utf8(destination) + "' already exists");

    fs::path partial = destination;
    partial += ".partial";
    fs::remove_all(long_path(partial), ec);  // left over by an interrupted run
    if (!fs::create_directories(partial, ec)) return fail("cannot create '" + path_to_utf8(partial) + "': " + ec.message());

    // Without links in the root itself (macOS: /var -> /private/var), SECURE_SYMLINKS checks only the
    // entries' own components.
    fs::path const root = fs::canonical(partial, ec);
    if (ec) return fail("cannot resolve '" + path_to_utf8(partial) + "': " + ec.message());
    auto result = unpack(archive_file, root);
    if (result) {
        // A single top-level directory is stripped (recipe-toml.md 4).
        fs::path single;
        std::size_t count = 0;
        for (fs::directory_entry const& e : fs::directory_iterator{partial, ec}) {
            ++count;
            single = e.path();
        }
        bool const strip = root_mode == top_level::strip_single && count == 1 && fs::is_directory(fs::symlink_status(single, ec));
        fs::rename(strip ? single : partial, destination, ec);
        if (ec) result = fail("cannot move the unpacked archive to '" + path_to_utf8(destination) + "': " + ec.message());
    }
    fs::remove_all(long_path(partial), ec);
    if (!result) fs::remove_all(long_path(destination), ec);
    return result;
}

}
