// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// What a built library file is (build-lua-api.md 8, docs/PLAN.md decision 31), read from its first
// bytes: no tool is needed on any host.
namespace otok {

enum class binary_kind
{
    unknown,
    archive,        // `!<arch>`: a static library, or a Windows import library
    elf_shared,     // ELF ET_DYN: a shared object
    macho_dylib,    // Mach-O MH_DYLIB
    pe_dll,         // PE with IMAGE_FILE_DLL
    pe_other        // a PE executable
};

binary_kind binary_kind_of(std::filesystem::path const& file);

// "a static archive", "a shared object", …
std::string binary_kind_name(binary_kind kind);

// The names a dynamic library is loaded by and loads others by, which are fixed at link time
// (build-lua-api.md 8): ELF `SONAME`, `NEEDED`, `RPATH` and `RUNPATH`; the Mach-O install name
// (`LC_ID_DYLIB`), the libraries it loads (`LC_LOAD_DYLIB` and its kin) and `LC_RPATH`. Empty for other
// files, and for what cannot be read.
struct load_names
{
    std::string id;
    std::vector<std::string> needed;
    std::vector<std::string> search;
};

load_names load_names_of(std::filesystem::path const& file);

// The ELF machines (`e_machine`: 183 AArch64, 40 ARM, 62 x86-64, 3 i386) of a shared object, or of the objects of a
// static archive, sorted and without repeats; empty when it holds no ELF object.
std::vector<unsigned> elf_machines_of(std::filesystem::path const& file);

// The smallest alignment of the loadable segments (`PT_LOAD` `p_align`) of an ELF shared object: the largest page
// size it can be loaded with; 0 for other files.
std::uint64_t elf_load_alignment_of(std::filesystem::path const& file);

// The COFF machines (`IMAGE_FILE_MACHINE_*`: 0x8664 x64, 0xAA64 ARM64, 0x14C x86) of a PE image, or of the objects and
// import entries of an archive (a static or import library), sorted and without repeats; empty when it holds none.
std::vector<unsigned> coff_machines_of(std::filesystem::path const& file);

}
