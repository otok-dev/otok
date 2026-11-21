// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "otok/core/binary.hpp"
#include "test_util.hpp"

using namespace otok;

namespace {

// `size` zero bytes with `bytes` written at `at`.
std::string image(std::size_t size, std::vector<std::pair<std::size_t, std::string>> const& parts)
{
    std::string out(size, '\0');
    for (auto const& [at, bytes] : parts) out.replace(at, bytes.size(), bytes);
    return out;
}

}

TEST(binary, tells_library_kinds_from_their_bytes)
{
    test::temp_dir dir;
    // ELF: e_type at 16, little-endian (EI_DATA 1) or big-endian (2)
    dir.write("libz.so", image(64, {{0, std::string{"\x7f" "ELF\x02\x01", 6}}, {16, std::string{"\x03\x00", 2}}}));
    dir.write("big.so", image(64, {{0, std::string{"\x7f" "ELF\x02\x02", 6}}, {16, std::string{"\x00\x03", 2}}}));
    dir.write("exe", image(64, {{0, std::string{"\x7f" "ELF\x02\x01", 6}}, {16, std::string{"\x02\x00", 2}}}));
    // Mach-O 64: filetype at 12, MH_DYLIB = 6, MH_OBJECT = 1
    dir.write("libz.dylib", image(64, {{0, std::string{"\xcf\xfa\xed\xfe", 4}}, {12, std::string{"\x06\x00\x00\x00", 4}}}));
    dir.write("z.o", image(64, {{0, std::string{"\xcf\xfa\xed\xfe", 4}}, {12, std::string{"\x01\x00\x00\x00", 4}}}));
    // PE: e_lfanew at 0x3c, "PE\0\0", Characteristics 22 bytes after it; IMAGE_FILE_DLL = 0x2000
    dir.write("z.dll", image(256, {{0, "MZ"}, {0x3c, std::string{"\x80\x00\x00\x00", 4}}, {0x80, std::string{"PE\0\0", 4}},
                                      {0x80 + 22, std::string{"\x22\x20", 2}}}));
    dir.write("z.exe", image(256, {{0, "MZ"}, {0x3c, std::string{"\x80\x00\x00\x00", 4}}, {0x80, std::string{"PE\0\0", 4}},
                                      {0x80 + 22, std::string{"\x22\x00", 2}}}));
    dir.write("libz.a", "!<arch>\n/               0           0     0     0       4         `\n");
    dir.write("text.txt", "hello");

    EXPECT_EQ(binary_kind_of(dir.path() / "libz.so"), binary_kind::elf_shared);
    EXPECT_EQ(binary_kind_of(dir.path() / "big.so"), binary_kind::elf_shared);
    EXPECT_EQ(binary_kind_of(dir.path() / "exe"), binary_kind::unknown);
    EXPECT_EQ(binary_kind_of(dir.path() / "libz.dylib"), binary_kind::macho_dylib);
    EXPECT_EQ(binary_kind_of(dir.path() / "z.o"), binary_kind::unknown);
    EXPECT_EQ(binary_kind_of(dir.path() / "z.dll"), binary_kind::pe_dll);
    EXPECT_EQ(binary_kind_of(dir.path() / "z.exe"), binary_kind::pe_other);
    EXPECT_EQ(binary_kind_of(dir.path() / "libz.a"), binary_kind::archive);
    EXPECT_EQ(binary_kind_of(dir.path() / "text.txt"), binary_kind::unknown);
    EXPECT_EQ(binary_kind_of(dir.path() / "missing"), binary_kind::unknown);
    EXPECT_EQ(binary_kind_name(binary_kind::pe_dll), "a DLL");
}

TEST(binary, reads_the_names_fixed_at_link_time)
{
    test::temp_dir dir;
    dir.write("runpath.so", test::elf_image(29) + std::string{"/work/x/lib\0", 12});
    dir.write("needed.so", test::elf_image(1) + std::string{"libz.so.1\0", 10});
    dir.write("soname.so", test::elf_image(14) + std::string{"libtiny.so.1\0", 13});
    EXPECT_EQ(load_names_of(dir.path() / "runpath.so").search, std::vector<std::string>{"/work/x/lib"});
    EXPECT_EQ(load_names_of(dir.path() / "needed.so").needed, std::vector<std::string>{"libz.so.1"});
    EXPECT_EQ(load_names_of(dir.path() / "soname.so").id, "libtiny.so.1");

    // Mach-O 64: the header (32 bytes, ncmds at 16), then LC_ID_DYLIB, LC_LOAD_DYLIB and LC_RPATH, each
    // with its string at offset 24 (dylib_command) or 12 (rpath_command)
    std::string m(32, '\0');
    m.replace(0, 4, std::string{"\xcf\xfa\xed\xfe", 4});
    m[12] = 6;
    m[16] = 3;
    auto const command = [&m](std::uint32_t cmd, std::uint32_t offset, std::string const& name) {
        std::string c(offset, '\0');
        c += name + std::string(8 - name.size() % 8, '\0');
        for (int i = 0; i < 4; ++i) {
            c[static_cast<std::size_t>(i)] = static_cast<char>(cmd >> (8 * i) & 0xff);
            c[static_cast<std::size_t>(4 + i)] = static_cast<char>(c.size() >> (8 * i) & 0xff);
            c[static_cast<std::size_t>(8 + i)] = static_cast<char>(offset >> (8 * i) & 0xff);
        }
        m += c;
    };
    command(0xd, 24, "@rpath/libtiny.1.dylib");
    command(0xc, 24, "/usr/lib/libSystem.B.dylib");
    command(0x8000001c, 12, "@loader_path/../lib");
    dir.write("libtiny.dylib", m);
    load_names const names = load_names_of(dir.path() / "libtiny.dylib");
    EXPECT_EQ(names.id, "@rpath/libtiny.1.dylib");
    EXPECT_EQ(names.needed, std::vector<std::string>{"/usr/lib/libSystem.B.dylib"});
    EXPECT_EQ(names.search, std::vector<std::string>{"@loader_path/../lib"});

    dir.write("libz.a", "!<arch>\n");
    EXPECT_TRUE(load_names_of(dir.path() / "libz.a").id.empty());
}

TEST(binary, reads_elf_machines_of_shared_objects_and_archives)
{
    test::temp_dir dir;
    // e_machine at 18: AArch64 183 (0xb7), x86-64 62 (0x3e)
    std::string const arm = image(64, {{0, std::string{"\x7f" "ELF\x02\x01", 6}}, {16, std::string{"\x01\x00\xb7\x00", 4}}});
    std::string const x64 = image(64, {{0, std::string{"\x7f" "ELF\x02\x01", 6}}, {16, std::string{"\x01\x00\x3e\x00", 4}}});
    // members: name (16), date (12), uid (6), gid (6), mode (8), size (10), "`\n"; data padded to an even size
    auto const member = [](std::string const& name, std::string const& data) {
        std::string size = std::to_string(data.size());
        std::string header = name + std::string(16 - name.size(), ' ') + std::string(32, ' ') + size + std::string(10 - size.size(), ' ') + "`\n";
        return header + data + (data.size() % 2 ? "\n" : "");
    };
    dir.write("libone.a", "!<arch>\n" + member("/", "xyz") + member("a.o/", arm) + member("b.o/", arm));
    dir.write("libmixed.a", "!<arch>\n" + member("a.o/", arm) + member("b.o/", x64));
    dir.write("libz.so", arm);
    dir.write("text.txt", "hello");
    EXPECT_EQ(elf_machines_of(dir.path() / "libone.a"), std::vector<unsigned>{183});
    EXPECT_EQ(elf_machines_of(dir.path() / "libmixed.a"), (std::vector<unsigned>{62, 183}));
    EXPECT_EQ(elf_machines_of(dir.path() / "libz.so"), std::vector<unsigned>{183});
    EXPECT_TRUE(elf_machines_of(dir.path() / "text.txt").empty());
}

// docs/PLAN.md decision 75: what the Windows target checks of its libraries
TEST(binary, reads_coff_machines_of_images_objects_and_archives)
{
    test::temp_dir dir;
    // a COFF object: Machine at 0 (x64 0x8664, ARM64 0xAA64); an import entry or a /bigobj object: 0x0000 0xFFFF, Machine at 6
    std::string const x64 = image(20, {{0, std::string{"\x64\x86", 2}}});
    std::string const arm = image(20, {{0, std::string{"\x64\xaa", 2}}});
    std::string const import_arm = image(20, {{2, std::string{"\xff\xff", 2}}, {6, std::string{"\x64\xaa", 2}}});
    auto const member = [](std::string const& name, std::string const& data) {
        std::string size = std::to_string(data.size());
        std::string header = name + std::string(16 - name.size(), ' ') + std::string(32, ' ') + size + std::string(10 - size.size(), ' ') + "`\n";
        return header + data + (data.size() % 2 ? "\n" : "");
    };
    // the linker members ("/") and the long names ("//") are no objects, whatever their bytes
    dir.write("z.lib", "!<arch>\n" + member("/", x64) + member("//", x64) + member("/0", arm) + member("b.obj/", arm));
    dir.write("z-import.lib", "!<arch>\n" + member("/", "xyz") + member("z.dll/", import_arm));
    dir.write("mixed.lib", "!<arch>\n" + member("a.obj/", arm) + member("b.obj/", x64));
    // a PE image: e_lfanew at 0x3c, "PE\0\0", then Machine
    dir.write("z.dll", image(0x60, {{0, "MZ"}, {0x3c, std::string{"\x40", 1}}, {0x40, std::string{"PE\0\0\x64\x86", 6}}}));
    dir.write("z.obj", x64);
    dir.write("text.txt", "hello, this is no object at all");
    EXPECT_EQ(coff_machines_of(dir.path() / "z.lib"), std::vector<unsigned>{0xAA64});
    EXPECT_EQ(coff_machines_of(dir.path() / "z-import.lib"), std::vector<unsigned>{0xAA64});
    EXPECT_EQ(coff_machines_of(dir.path() / "mixed.lib"), (std::vector<unsigned>{0x8664, 0xAA64}));
    EXPECT_EQ(coff_machines_of(dir.path() / "z.dll"), std::vector<unsigned>{0x8664});
    EXPECT_EQ(coff_machines_of(dir.path() / "z.obj"), std::vector<unsigned>{0x8664});
    EXPECT_TRUE(coff_machines_of(dir.path() / "text.txt").empty());
}

TEST(binary, reads_the_load_alignment_of_shared_objects)
{
    test::temp_dir dir;
    // ELF64: e_phoff at 0x20, e_phentsize at 0x36, e_phnum at 0x38; program headers of 56 bytes, p_type at 0, p_align at 0x30
    std::string so = image(64 + 3 * 56, {{0, std::string{"\x7f" "ELF\x02\x01", 6}}, {16, std::string{"\x03\x00\xb7\x00", 4}},
                                             {0x20, std::string{"\x40", 1}}, {0x36, std::string{"\x38", 1}}, {0x38, std::string{"\x03", 1}}});
    auto const segment = [&so](std::size_t i, char type, std::string const& align) {
        so[64 + i * 56] = type;
        so.replace(64 + i * 56 + 0x30, align.size(), align);
    };
    segment(0, 1, std::string{"\x00\x40", 2});   // PT_LOAD, 16 KiB
    segment(1, 2, std::string{"\x08", 1});       // PT_DYNAMIC: not a load segment
    segment(2, 1, std::string{"\x00\x40", 2});
    dir.write("aligned.so", so);
    segment(2, 1, std::string{"\x00\x10", 2});   // 4 KiB
    dir.write("small.so", so);
    dir.write("text.txt", "hello");
    EXPECT_EQ(elf_load_alignment_of(dir.path() / "aligned.so"), 16384u);
    EXPECT_EQ(elf_load_alignment_of(dir.path() / "small.so"), 4096u);
    EXPECT_EQ(elf_load_alignment_of(dir.path() / "text.txt"), 0u);
}

namespace {

// `value` as `size` little-endian (or big-endian) bytes
std::string bytes_of(std::uint64_t value, int size, bool little = true)
{
    std::string out(static_cast<std::size_t>(size), '\0');
    for (int i = 0; i < size; ++i) out[static_cast<std::size_t>(little ? i : size - 1 - i)] = static_cast<char>(value >> (8 * i) & 0xff);
    return out;
}

std::string archive_member(std::string const& name, std::string const& data)
{
    std::string size = std::to_string(data.size());
    std::string header = name + std::string(16 - name.size(), ' ') + std::string(32, ' ') + size + std::string(10 - size.size(), ' ') + "`\n";
    return header + data + (data.size() % 2 ? "\n" : "");
}

}

// docs/PLAN.md decision 136: what a dynamic library exports of the static libraries inside it
TEST(binary, reads_the_symbol_index_of_archives)
{
    test::temp_dir dir;
    // GNU and COFF: "/" with a big-endian count, the members' offsets, the names
    dir.write("libgnu.a", "!<arch>\n" + archive_member("/", bytes_of(2, 4, false) + bytes_of(0, 4, false) + bytes_of(0, 4, false)
                                                                + std::string{"foo\0bar\0", 8}) + archive_member("a.o/", "xx"));
    // 64-bit GNU: "/SYM64/" with 8-byte numbers
    dir.write("libgnu64.a", "!<arch>\n" + archive_member("/SYM64/", bytes_of(1, 8, false) + bytes_of(0, 8, false) + std::string{"wide\0", 5}));
    // BSD (Apple): "#1/20", the name after the header, then the ranlib entries' size, entries (string, member), the strings' size, the strings
    std::string const name{"__.SYMDEF SORTED\0\0\0\0", 20};
    std::string const ranlib = bytes_of(16, 4) + bytes_of(0, 4) + bytes_of(0, 4) + bytes_of(5, 4) + bytes_of(0, 4) + bytes_of(12, 4)
        + std::string{"_foo\0_bar\0\0\0", 12};
    dir.write("libbsd.a", "!<arch>\n" + archive_member("#1/20", name + ranlib) + archive_member("#1/4", std::string{"a.o\0", 4} + "xx"));
    dir.write("libnone.a", "!<arch>\n" + archive_member("a.o/", "xx"));
    dir.write("text.txt", "hello");
    EXPECT_EQ(archive_symbols_of(dir.path() / "libgnu.a"), (std::vector<std::string>{"bar", "foo"}));
    EXPECT_EQ(archive_symbols_of(dir.path() / "libgnu64.a"), std::vector<std::string>{"wide"});
    EXPECT_EQ(archive_symbols_of(dir.path() / "libbsd.a"), (std::vector<std::string>{"_bar", "_foo"}));
    EXPECT_TRUE(archive_symbols_of(dir.path() / "libnone.a").empty());
    EXPECT_TRUE(archive_symbols_of(dir.path() / "text.txt").empty());
}

TEST(binary, reads_the_strong_exports_of_dynamic_libraries)
{
    test::temp_dir dir;

    // Mach-O 64: one LC_DYLD_EXPORTS_TRIE (dataoff, datasize) and the trie at 64: the root with the edges "_a" and "_w",
    // the node of _a terminal (flags 0), that of _w a weak definition (flags 0x04), left out
    std::string const trie = std::string{"\x00\x02_a\x00\x0a_w\x00\x0e", 10} + std::string{"\x02\x00\x00\x00", 4} + std::string{"\x02\x04\x00\x00", 4};
    std::string const macho = image(64, {{0, std::string{"\xcf\xfa\xed\xfe", 4}}, {12, bytes_of(6, 4)}, {16, bytes_of(1, 4)},
                                            {32, bytes_of(0x80000033, 4) + bytes_of(16, 4) + bytes_of(64, 4) + bytes_of(trie.size(), 4)}}) + trie;
    dir.write("libm.dylib", macho);
    EXPECT_EQ(exported_symbols_of(dir.path() / "libm.dylib"), std::vector<std::string>{"_a"});
    // universal: the first architecture
    std::string const fat = image(4096, {{0, std::string{"\xca\xfe\xba\xbe", 4}}, {4, bytes_of(1, 4, false)}, {16, bytes_of(4096, 4, false)}}) + macho;
    dir.write("libfat.dylib", fat);
    EXPECT_EQ(exported_symbols_of(dir.path() / "libfat.dylib"), std::vector<std::string>{"_a"});

    // ELF64: the strings at 64, .dynsym at 96 (entries of 24 bytes: name, info, other, shndx), the section headers at 216
    // (null, .dynsym with sh_link 2, .dynstr); only "exp" is defined, global and visible
    std::string const strings{"\0exp\0weak\0undef\0hidden\0", 23};
    auto const symbol = [](std::uint32_t name, unsigned char info, unsigned char other, std::uint16_t shndx) {
        return bytes_of(name, 4) + std::string{static_cast<char>(info), static_cast<char>(other)} + bytes_of(shndx, 2) + std::string(16, '\0');
    };
    std::string const dynsym = std::string(24, '\0') + symbol(1, 0x12, 0, 5) + symbol(5, 0x22, 0, 5) + symbol(10, 0x12, 0, 0) + symbol(16, 0x12, 2, 5);
    auto const section = [](std::uint32_t type, std::uint64_t offset, std::uint64_t size, std::uint32_t link) {
        return bytes_of(0, 4) + bytes_of(type, 4) + std::string(16, '\0') + bytes_of(offset, 8) + bytes_of(size, 8) + bytes_of(link, 4) + std::string(20, '\0');
    };
    std::string elf = image(64, {{0, std::string{"\x7f" "ELF\x02\x01", 6}}, {16, std::string{"\x03\x00\x3e\x00", 4}}, {0x28, bytes_of(216, 8)},
                                    {0x3a, bytes_of(64, 2)}, {0x3c, bytes_of(3, 2)}});
    elf += strings + std::string(96 - 64 - strings.size(), '\0') + dynsym;
    elf += std::string(64, '\0') + section(11, 96, dynsym.size(), 2) + section(3, 64, strings.size(), 0);
    dir.write("libe.so", elf);
    EXPECT_EQ(exported_symbols_of(dir.path() / "libe.so"), std::vector<std::string>{"exp"});

    // PE32+: e_lfanew 0x40, one section (RVA 0x1000 at file 0x200), the export directory at its start with two names
    std::string pe = image(0x400, {{0, "MZ"}, {0x3c, bytes_of(0x40, 4)}, {0x40, std::string{"PE\0\0", 4} + bytes_of(0x8664, 2) + bytes_of(1, 2)},
                                      {0x40 + 20, bytes_of(240, 2)}, {0x58, bytes_of(0x20b, 2)}, {0x58 + 112, bytes_of(0x1000, 4) + bytes_of(0x40, 4)},
                                      {0x58 + 240 + 8, bytes_of(0x200, 4) + bytes_of(0x1000, 4) + bytes_of(0x200, 4) + bytes_of(0x200, 4)},
                                      {0x200 + 24, bytes_of(2, 4)}, {0x200 + 32, bytes_of(0x1028, 4)},
                                      {0x228, bytes_of(0x1030, 4) + bytes_of(0x1036, 4)}, {0x230, std::string{"alpha\0beta\0", 11}}});
    dir.write("z.dll", pe);
    EXPECT_EQ(exported_symbols_of(dir.path() / "z.dll"), (std::vector<std::string>{"alpha", "beta"}));

    dir.write("text.txt", "hello");
    EXPECT_TRUE(exported_symbols_of(dir.path() / "text.txt").empty());
}

TEST(binary, reads_libraries_beyond_max_path)
{
    // the libraries of a package entry, deep below a cache on Windows (the Android target reads the soname of each)
    test::temp_dir dir;
    std::string const deep = test::deep_path();
    dir.write(deep + "/z.lib", "!<arch>\n");
    dir.write(deep + "/z.obj", image(20, {{0, std::string{"\x64\x86", 2}}}));
    std::filesystem::path const at = dir.path() / path_from_utf8(deep);
    EXPECT_EQ(binary_kind_of(at / "z.lib"), binary_kind::archive);
    EXPECT_EQ(coff_machines_of(at / "z.obj"), std::vector<unsigned>{0x8664});
}
