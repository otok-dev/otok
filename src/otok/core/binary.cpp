// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/binary.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>

#include "otok/core/fs.hpp"

namespace otok {

namespace fs = std::filesystem;

namespace {

std::uint32_t little32(std::string const& b, std::size_t at)
{
    if (at + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8
        | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16 | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24;
}

std::uint16_t read16(std::string const& b, std::size_t at, bool little)
{
    if (at + 2 > b.size()) return 0;
    auto const lo = static_cast<unsigned char>(b[little ? at : at + 1]);
    auto const hi = static_cast<unsigned char>(b[little ? at + 1 : at]);
    return static_cast<std::uint16_t>(lo | hi << 8);
}

}

binary_kind binary_kind_of(fs::path const& file)
{
    std::ifstream in{long_path(file), std::ios::binary};
    std::string head(4096, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    if (head.starts_with("!<arch>\n")) return binary_kind::archive;
    if (head.starts_with("\x7f" "ELF") && head.size() >= 18) {
        return read16(head, 16, head[5] != 2) == 3 ? binary_kind::elf_shared : binary_kind::unknown;  // ET_DYN
    }
    std::uint32_t const magic = little32(head, 0);
    if (magic == 0xfeedfacf || magic == 0xfeedface) return little32(head, 12) == 6 ? binary_kind::macho_dylib : binary_kind::unknown;  // MH_DYLIB
    if (head.starts_with("MZ")) {
        std::uint32_t const pe = little32(head, 0x3c);
        if (pe + 24 > head.size() || head.compare(pe, 4, std::string_view{"PE\0\0", 4}) != 0) return binary_kind::unknown;
        return read16(head, pe + 22, true) & 0x2000 ? binary_kind::pe_dll : binary_kind::pe_other;  // IMAGE_FILE_DLL
    }
    return binary_kind::unknown;
}

std::string binary_kind_name(binary_kind kind)
{
    switch (kind) {
    case binary_kind::archive: return "a static archive";
    case binary_kind::elf_shared: return "a shared object";
    case binary_kind::macho_dylib: return "a dynamic library";
    case binary_kind::pe_dll: return "a DLL";
    case binary_kind::pe_other: return "an executable";
    case binary_kind::unknown: break;
    }
    return "no library";
}

namespace {

// Reads little- or big-endian integers of `size` bytes from a file image; 0 past its end.
struct image_reader
{
    std::string const& b;
    bool little;

    std::uint64_t get(std::uint64_t at, int size) const
    {
        if (at + static_cast<std::uint64_t>(size) > b.size()) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < size; ++i) {
            auto const byte = static_cast<unsigned char>(b[static_cast<std::size_t>(at) + static_cast<std::size_t>(little ? size - 1 - i : i)]);
            v = v << 8 | byte;
        }
        return v;
    }

    // a NUL-terminated string at `at`, at most up to `end`
    std::string string(std::uint64_t at, std::uint64_t end) const
    {
        end = std::min<std::uint64_t>(end, b.size());
        if (at >= end) return {};
        std::size_t const from = static_cast<std::size_t>(at);
        std::size_t const nul = b.find('\0', from);
        std::size_t const to = nul == std::string::npos ? static_cast<std::size_t>(end) : std::min(nul, static_cast<std::size_t>(end));
        return b.substr(from, to - from);
    }
};

// The dynamic section: DT_NEEDED 1, DT_SONAME 14, DT_RPATH 15, DT_RUNPATH 29, its strings in the
// section sh_link names.
load_names elf_names(std::string const& b)
{
    load_names out;
    bool const wide = b[4] == 2;
    image_reader const r{b, b[5] != 2};
    int const word = wide ? 8 : 4;
    std::uint64_t const shoff = r.get(wide ? 0x28 : 0x20, word);
    std::uint64_t const shentsize = r.get(wide ? 0x3a : 0x2e, 2);
    std::uint64_t const shnum = r.get(wide ? 0x3c : 0x30, 2);
    auto const section = [&](std::uint64_t i, std::uint64_t field64, std::uint64_t field32, int size64, int size32) {
        return r.get(shoff + i * shentsize + (wide ? field64 : field32), wide ? size64 : size32);
    };
    for (std::uint64_t i = 0; i < shnum && shentsize > 0; ++i) {
        if (section(i, 4, 4, 4, 4) != 6) continue;  // SHT_DYNAMIC
        std::uint64_t const offset = section(i, 0x18, 0x10, 8, 4);
        std::uint64_t const size = section(i, 0x20, 0x14, 8, 4);
        std::uint64_t const link = section(i, 0x28, 0x18, 4, 4);
        std::uint64_t const strtab = section(link, 0x18, 0x10, 8, 4);
        std::uint64_t const strend = strtab + section(link, 0x20, 0x14, 8, 4);
        for (std::uint64_t at = offset; at + 2 * static_cast<std::uint64_t>(word) <= offset + size; at += 2 * static_cast<std::uint64_t>(word)) {
            std::uint64_t const tag = r.get(at, word);
            if (tag == 0) break;
            std::string const value = r.string(strtab + r.get(at + static_cast<std::uint64_t>(word), word), strend);
            if (tag == 1) out.needed.push_back(value);
            else if (tag == 14) out.id = value;
            else if (tag == 15 || tag == 29) out.search.push_back(value);
        }
    }
    return out;
}

// Load commands: LC_ID_DYLIB, LC_LOAD_DYLIB and its kin, LC_RPATH, a name offset at +8 of each.
load_names macho_names(std::string const& b, bool wide)
{
    load_names out;
    image_reader const r{b, true};
    std::uint64_t const ncmds = r.get(16, 4);
    std::uint64_t at = wide ? 32 : 28;
    for (std::uint64_t i = 0; i < ncmds && at + 8 <= b.size(); ++i) {
        std::uint64_t const cmd = r.get(at, 4);
        std::uint64_t const size = r.get(at + 4, 4);
        if (size < 8) break;
        std::string const name = r.string(at + r.get(at + 8, 4), at + size);
        switch (cmd) {
        case 0xd: out.id = name; break;                    // LC_ID_DYLIB
        case 0xc: case 0x20: case 0x80000018: case 0x8000001f: case 0x80000023: out.needed.push_back(name); break;
        case 0x8000001c: out.search.push_back(name); break; // LC_RPATH
        default: break;
        }
        at += size;
    }
    return out;
}

}

load_names load_names_of(fs::path const& file)
{
    std::ifstream in{long_path(file), std::ios::binary};
    std::string const bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (bytes.size() >= 0x40 && bytes.starts_with("\x7f" "ELF")) return elf_names(bytes);
    image_reader const r{bytes, true};
    std::uint64_t const magic = r.get(0, 4);
    if (magic == 0xfeedfacf || magic == 0xfeedface) return macho_names(bytes, magic == 0xfeedfacf);
    return {};
}

std::vector<unsigned> elf_machines_of(fs::path const& file)
{
    std::ifstream in{long_path(file), std::ios::binary};
    std::string const bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<unsigned> machines;
    auto const add = [&](std::size_t at) {
        if (bytes.compare(at, 4, "\x7f" "ELF") != 0 || at + 20 > bytes.size()) return;
        machines.push_back(read16(bytes, at + 18, bytes[at + 5] != 2));
    };
    if (bytes.starts_with("!<arch>\n")) {
        // members: a 60-byte header (the size in decimal at 48, 10 characters), then the data, padded to an even size
        for (std::size_t at = 8; at + 60 <= bytes.size();) {
            std::uint64_t size = 0;
            for (std::size_t i = at + 48; i < at + 58 && bytes[i] >= '0' && bytes[i] <= '9'; ++i) size = size * 10 + static_cast<std::uint64_t>(bytes[i] - '0');
            add(at + 60);
            at += 60 + static_cast<std::size_t>(size + (size & 1));
        }
    } else {
        add(0);
    }
    std::ranges::sort(machines);
    machines.erase(std::unique(machines.begin(), machines.end()), machines.end());
    return machines;
}

std::vector<unsigned> coff_machines_of(fs::path const& file)
{
    std::ifstream in{long_path(file), std::ios::binary};
    std::string const bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<unsigned> machines;
    // the machines COFF knows; anything else is no object (LLVM bitcode, the linker members' data)
    auto const known = [](unsigned m) { return m == 0x8664 || m == 0xAA64 || m == 0x14C || m == 0x1C4 || m == 0xA641; };
    if (bytes.starts_with("!<arch>\n")) {
        for (std::size_t at = 8; at + 60 <= bytes.size();) {
            std::uint64_t size = 0;
            for (std::size_t i = at + 48; i < at + 58 && bytes[i] >= '0' && bytes[i] <= '9'; ++i) size = size * 10 + static_cast<std::uint64_t>(bytes[i] - '0');
            std::size_t const data = at + 60;
            // "/" and "//": the linker members and the long names, which are no objects
            bool const special = bytes.compare(at, 2, "/ ") == 0 || bytes.compare(at, 3, "// ") == 0;
            if (!special && data + 8 <= bytes.size()) {
                // an import entry or a /bigobj object starts with 0x0000 0xFFFF and has the machine at 6; an object, at 0
                bool const header = read16(bytes, data, true) == 0 && read16(bytes, data + 2, true) == 0xFFFF;
                unsigned const machine = read16(bytes, header ? data + 6 : data, true);
                if (known(machine)) machines.push_back(machine);
            }
            at = data + static_cast<std::size_t>(size + (size & 1));
        }
    } else if (bytes.starts_with("MZ")) {
        std::uint32_t const pe = little32(bytes, 0x3c);
        if (pe + 6 <= bytes.size() && bytes.compare(pe, 4, std::string_view{"PE\0\0", 4}) == 0 && known(read16(bytes, pe + 4, true))) {
            machines.push_back(read16(bytes, pe + 4, true));
        }
    } else if (bytes.size() >= 20 && known(read16(bytes, 0, true))) {
        machines.push_back(read16(bytes, 0, true));  // an object file
    }
    std::ranges::sort(machines);
    machines.erase(std::unique(machines.begin(), machines.end()), machines.end());
    return machines;
}

std::uint64_t elf_load_alignment_of(fs::path const& file)
{
    std::ifstream in{long_path(file), std::ios::binary};
    std::string const bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (bytes.size() < 0x40 || !bytes.starts_with("\x7f" "ELF")) return 0;
    bool const wide = bytes[4] == 2;
    image_reader const r{bytes, bytes[5] != 2};
    std::uint64_t const phoff = r.get(wide ? 0x20 : 0x1c, wide ? 8 : 4);
    std::uint64_t const phentsize = r.get(wide ? 0x36 : 0x2a, 2);
    std::uint64_t const phnum = r.get(wide ? 0x38 : 0x2c, 2);
    std::uint64_t alignment = 0;
    for (std::uint64_t i = 0; i < phnum && phentsize > 0; ++i) {
        std::uint64_t const at = phoff + i * phentsize;
        if (r.get(at, 4) != 1) continue;  // PT_LOAD
        std::uint64_t const align = wide ? r.get(at + 0x30, 8) : r.get(at + 0x1c, 4);
        if (alignment == 0 || align < alignment) alignment = align;
    }
    return alignment;
}

}
