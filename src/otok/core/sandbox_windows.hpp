// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

// What otok and otok-sandbox.dll share (docs/PLAN.md decision 103): the payload otok copies into the first process of
// a sandboxed command with Microsoft Detours and the DLL passes on to every child, each copy with handles duplicated
// into that child. Windows only; plain data, no otok types, since the DLL links nothing of otok.
#ifdef _WIN32

#include <windows.h>

#include <cstddef>
#include <cwchar>
#include <iterator>
#include <string>

namespace otok::sandbox_windows {

// {9b3f61d2-4c8e-4f0a-b5d7-2e81c6a4f903}
inline constexpr GUID payload_guid = {0x9b3f61d2, 0x4c8e, 0x4f0a, {0xb5, 0xd7, 0x2e, 0x81, 0xc6, 0xa4, 0xf9, 0x03}};

// Raised whenever the layout changes: a DLL and an otok of different versions never meet (the DLL lies beside
// otok.exe), but the check costs nothing.
inline constexpr unsigned payload_version = 4;

// The DLL's file name, beside otok.exe.
inline constexpr wchar_t dll_name[] = L"otok-sandbox.dll";

// The name of the AppContainer profile of the builds (one per user).
inline constexpr wchar_t container_name[] = L"otok.build";

// The variables CreateProcess rewrites for a process in an AppContainer (to `%LOCALAPPDATA%\Packages\<name>\AC\…`,
// refusing to start it without LOCALAPPDATA): the DLL sets them back to what the process's creator gave, before the
// program's runtime reads its environment.
inline constexpr wchar_t const* restored_variables[] = {L"TEMP", L"TMP", L"LOCALAPPDATA"};
inline constexpr std::size_t restored_count = std::size(restored_variables);

// The access drive roots and the MountPointManager are opened with outside the container: no more than a volume's
// attributes (Cygwin reads them from the root) and the volume-to-letter queries of GetFinalPathNameByHandleW.
inline constexpr DWORD root_access = READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_READ_EA | SYNCHRONIZE;
inline constexpr DWORD mountmgr_access = SYNCHRONIZE | FILE_READ_ATTRIBUTES;

// The access the ancestors of the work directory (the drive root, C:\Users, ..., the cache) are opened with outside the
// container: to pass through them (GNU mkdir -p walks down into each, Cygwin reads their attributes and security),
// never to list them nor to open what is in them.
inline constexpr DWORD ancestor_access = SYNCHRONIZE | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL;
inline constexpr std::size_t max_ancestors = 24;

// The virtual drives a payload carries (core/virtual_drives): the cache and the work directories.
inline constexpr std::size_t max_drives = 4;

struct payload
{
    unsigned version;
    // A handle to the null device, valid in the process holding this copy (0 for none).
    unsigned long long null_device;
    // \\.\MountPointManager and each drive root (A: to Z:), opened outside with the access above (0 for none).
    unsigned long long mountmgr;
    unsigned long long roots[26];
    // QueryDosDeviceW of each drive letter, taken outside ("" for none): the container may not open the links of \??.
    wchar_t dos_devices[26][128];
    // The container's SID and the user's as text, and the container's object namespace,
    // \Sessions\<n>\AppContainerNamedObjects\<SID>: a forked Cygwin child cannot always read its own token. No container:
    // the DLL runs outside an AppContainer, for the device map alone (sandbox = false).
    wchar_t container_sid[192];
    wchar_t user_sid[192];
    wchar_t container_objects[320];
    // Told apart from the objects of the container's other builds: the first name of an object directory the DLL moves into the
    // container's namespace begins with it (Cygwin's shared tables, with the mount of /tmp, are one per name; "" for none).
    wchar_t object_scope[32];
    // Where the DLL appends what the container was refused ("" for nowhere): a file in the work directory.
    wchar_t log[1024];
    // restored_variables as the creator gave them; not present: removed.
    struct variable
    {
        unsigned present;
        wchar_t value[1024];
    } variables[restored_count];
    // The ways to the work directory and to what the build reads outside the cache, each directory on them down to
    // these themselves (docs/PLAN.md decision 111: a listing of one of them shows the next names alone), opened outside
    // with ancestor_access: each NT name (\??\C:\, \??\C:\Users,
    // ..., without a backslash at the end but the root's) and its handle (0 for none). The DLL hands a duplicate for
    // exactly these names.
    unsigned ancestor_count;
    struct ancestor
    {
        unsigned long long handle;
        wchar_t name[520];
    } ancestors[max_ancestors];
    // The device map of otok (core/virtual_drives), which a map not being inherited the DLL sets on each child: the map's
    // object directory, valid in this process, outside an AppContainer; in one, which may not set it, the pipe of otok's
    // broker (core/sandbox), to which the DLL sends a handle to the suspended child. Both empty: no map.
    unsigned long long map_directory;
    wchar_t map_pipe[128];
    // The virtual drives, the deepest directories first: GetFinalPathNameByHandleW gives the real directory's paths
    // through the letter.
    unsigned drive_count;
    struct drive
    {
        wchar_t letter;
        wchar_t real[1024];  // C:\…, no separator at the end
    } drives[max_drives];
};

// The value of `name` in an environment block ("NAME=value\0…\0"), compared without case; null when not there.
inline wchar_t const* block_value(wchar_t const* block, wchar_t const* name)
{
    std::size_t const length = ::wcslen(name);
    for (wchar_t const* p = block; p && *p; p += ::wcslen(p) + 1)
        if (::_wcsnicmp(p, name, length) == 0 && p[length] == L'=') return p + length + 1;
    return nullptr;
}

// restored_variables of the environment block a process is given, into its payload.
inline void take_variables(payload& p, wchar_t const* block)
{
    for (std::size_t i = 0; i < restored_count; ++i) {
        wchar_t const* value = block_value(block, restored_variables[i]);
        p.variables[i].present = value != nullptr;
        ::wcsncpy_s(p.variables[i].value, value ? value : L"", _TRUNCATE);
    }
}

// A copy of `block` with `name=value` at its end when `name` is given and the block lacks it; it ends with two nulls.
inline std::wstring with_variable(wchar_t const* block, wchar_t const* name = nullptr, std::wstring const& value = {})
{
    std::wstring out;
    for (wchar_t const* p = block; p && *p; p += ::wcslen(p) + 1) out.append(p).push_back(L'\0');
    if (name && !block_value(block, name)) out.append(name).append(L"=").append(value).push_back(L'\0');
    if (out.empty()) out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}

}

#endif
