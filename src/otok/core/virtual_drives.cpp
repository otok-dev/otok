// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/virtual_drives.hpp"

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <cstdio>
#include <optional>

#ifdef _WIN32
#   include <windows.h>
#   include <sddl.h>
#   include <winternl.h>
#endif

#include "otok/core/fs.hpp"

namespace otok::virtual_drives {

namespace fs = std::filesystem;

#ifdef _WIN32

namespace {

using nt_create_directory_object_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using nt_create_symbolic_link_object_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING);
using nt_set_information_process_t = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);

constexpr ACCESS_MASK directory_all_access = 0xF000F;
constexpr ACCESS_MASK symbolic_link_all_access = 0xF0001;
constexpr ULONG process_device_map = 23;

// The order letters are given in: away from A and B (floppies to some programs) and from the end of the alphabet, where
// network drives are usually mapped.
constexpr wchar_t letter_order[] = L"OPQRSTUVNMLKJIHGFEDWXYZ";

struct state
{
    std::mutex mutex;
    // the map's objects readable to AppContainers (ALL APPLICATION PACKAGES), whose processes query a drive's link
    // (GetVolumeInformationW of its root) and may not otherwise: only names of directories are in them
    PSECURITY_DESCRIPTOR readable = nullptr;
    std::optional<result<void>> enabled;
    HANDLE directory = nullptr;
    std::vector<HANDLE> links;   // of the system's letters, kept open: a link lives as long as a handle to it
    HANDLE drive_links[26]{};    // of the virtual letters, closed when their last user releases them
    unsigned users[26]{};
    DWORD taken = 0;             // letters of the system when the map was made, and the virtual ones since
    std::vector<drive> drives;   // the deepest first
    nt_create_symbolic_link_object_t create_link = nullptr;
    nt_set_information_process_t set_information = nullptr;
};

state& the()
{
    static state s;
    return s;
}

std::string status_text(NTSTATUS status)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lx", static_cast<unsigned long>(status));
    return text;
}

// "C:\a\b" without "\\?\" and without a separator at the end; empty when not an absolute path with a drive letter.
std::wstring plain_dos(fs::path const& p)
{
    std::wstring s = p.wstring();
    if (s.starts_with(L"\\\\?\\")) s.erase(0, 4);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (s.size() < 3 || s[1] != L':' || s[2] != L'\\') return {};
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

result<HANDLE> make_link(state& st, wchar_t letter, std::wstring const& target)
{
    std::wstring name{letter, L':'};
    UNICODE_STRING n{static_cast<USHORT>(name.size() * sizeof(wchar_t)), static_cast<USHORT>(name.size() * sizeof(wchar_t)), name.data()};
    std::wstring to = target;
    UNICODE_STRING t{static_cast<USHORT>(to.size() * sizeof(wchar_t)), static_cast<USHORT>(to.size() * sizeof(wchar_t)), to.data()};
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE, st.directory, st.readable);
    HANDLE link = nullptr;
    NTSTATUS const status = st.create_link(&link, symbolic_link_all_access, &oa, &t);
    if (status < 0) return fail("cannot make the drive " + path_to_utf8(fs::path{name}) + ": NtCreateSymbolicLinkObject " + status_text(status));
    return link;
}

result<void> make_map(state& st)
{
    HMODULE const ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto const create_directory = reinterpret_cast<nt_create_directory_object_t>(::GetProcAddress(ntdll, "NtCreateDirectoryObject"));
    st.create_link = reinterpret_cast<nt_create_symbolic_link_object_t>(::GetProcAddress(ntdll, "NtCreateSymbolicLinkObject"));
    st.set_information = reinterpret_cast<nt_set_information_process_t>(::GetProcAddress(ntdll, "NtSetInformationProcess"));
    if (!create_directory || !st.create_link || !st.set_information) return fail("ntdll.dll lacks the functions of a device map");
    // the user and SYSTEM all, AppContainers to query and traverse (GENERIC_READ maps to that for both kinds of object)
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;OW)(A;;GA;;;SY)(A;;GR;;;AC)", SDDL_REVISION_1, &st.readable, nullptr))
        return fail("cannot make the security descriptor of the device map");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, nullptr, 0, nullptr, st.readable);
    if (NTSTATUS const status = create_directory(&st.directory, directory_all_access, &oa); status < 0)
        return fail("cannot make the device map: NtCreateDirectoryObject " + status_text(status));
    // the letters this process sees: those of the logon session (subst, network drives) are not looked up through a
    // private map, the global ones are found anyway
    st.taken = ::GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(st.taken & (1u << d))) continue;
        wchar_t const letter[3] = {static_cast<wchar_t>(L'A' + d), L':', 0};
        wchar_t target[1024];
        if (!::QueryDosDeviceW(letter, target, static_cast<DWORD>(std::size(target)))) continue;
        auto made = make_link(st, letter[0], target);
        if (!made) return std::unexpected{made.error()};
        st.links.push_back(*made);
    }
    if (NTSTATUS const status = st.set_information(::GetCurrentProcess(), process_device_map, &st.directory, sizeof st.directory); status < 0)
        return fail("cannot set the device map of otok: NtSetInformationProcess " + status_text(status));
    return {};
}

} // namespace

result<void> enable()
{
    state& st = the();
    std::lock_guard lock{st.mutex};
    if (!st.enabled) st.enabled = make_map(st);
    return *st.enabled;
}

bool enabled() noexcept
{
    state& st = the();
    std::lock_guard lock{st.mutex};
    return st.enabled && st.enabled->has_value();
}

result<fs::path> drive_for(fs::path const& real)
{
    state& st = the();
    std::lock_guard lock{st.mutex};
    if (!st.enabled || !*st.enabled) return fail("the virtual drives are not enabled");
    std::wstring const dos = plain_dos(real);
    if (dos.empty()) return fail("'" + path_to_utf8(real) + "' is not an absolute path on a drive");
    for (drive const& d : st.drives) {
        if (::_wcsicmp(d.real.c_str(), dos.c_str()) != 0) continue;
        ++st.users[d.letter - L'A'];
        return fs::path{std::wstring{d.letter, L':', L'\\'}};
    }
    wchar_t letter = 0;
    for (wchar_t const* c = letter_order; *c && !letter; ++c) {
        if (!(st.taken & (1u << (*c - L'A')))) letter = *c;
    }
    if (!letter) return fail("no drive letter is free for '" + path_to_utf8(real) + "'");
    // through \??\, as subst links: resolved in the map itself, so a letter of the logon session serves too
    std::wstring const target = L"\\??\\" + dos;
    auto made = make_link(st, letter, target);
    if (!made) return std::unexpected{made.error()};
    st.drive_links[letter - L'A'] = *made;
    st.users[letter - L'A'] = 1;
    st.taken |= 1u << (letter - L'A');
    st.drives.push_back(drive{letter, fs::path{dos}, target});
    std::ranges::stable_sort(st.drives, [](drive const& a, drive const& b) { return a.real.native().size() > b.real.native().size(); });
    return fs::path{std::wstring{letter, L':', L'\\'}};
}

void release(fs::path const& root)
{
    state& st = the();
    std::lock_guard lock{st.mutex};
    std::wstring const s = root.wstring();
    if (s.size() < 2 || s[1] != L':') return;
    wchar_t const letter = static_cast<wchar_t>(std::towupper(s[0]));
    auto const it = std::ranges::find(st.drives, letter, &drive::letter);
    if (it == st.drives.end() || --st.users[letter - L'A'] > 0) return;
    ::CloseHandle(st.drive_links[letter - L'A']);
    st.drive_links[letter - L'A'] = nullptr;
    st.taken &= ~(1u << (letter - L'A'));
    st.drives.erase(it);
}

namespace {

// `s` begins with `prefix` (without case), followed by a separator or nothing.
bool under(std::wstring_view s, std::wstring_view prefix)
{
    if (s.size() < prefix.size() || ::_wcsnicmp(s.data(), prefix.data(), prefix.size()) != 0) return false;
    if (s.size() == prefix.size()) return true;
    return s[prefix.size()] == L'\\' || s[prefix.size()] == L'/' || prefix.back() == L'\\';
}

}

fs::path to_virtual(fs::path const& p)
{
    state& st = the();
    std::lock_guard lock{st.mutex};
    if (st.drives.empty()) return p;
    std::wstring s = p.wstring();
    if (s.starts_with(L"\\\\?\\")) s.erase(0, 4);
    for (drive const& d : st.drives) {
        std::wstring const& real = d.real.native();
        if (!under(s, real)) continue;
        std::wstring rest = s.substr(real.size());
        while (!rest.empty() && (rest.front() == L'\\' || rest.front() == L'/')) rest.erase(0, 1);
        return fs::path{std::wstring{d.letter, L':', L'\\'} + rest};
    }
    return p;
}

fs::path to_real(fs::path const& p)
{
    state& st = the();
    std::lock_guard lock{st.mutex};
    std::wstring const s = p.wstring();
    if (s.size() < 2 || s[1] != L':') return p;
    wchar_t const letter = static_cast<wchar_t>(std::towupper(s[0]));
    for (drive const& d : st.drives) {
        if (d.letter != letter) continue;
        std::wstring rest = s.substr(2);
        while (!rest.empty() && (rest.front() == L'\\' || rest.front() == L'/')) rest.erase(0, 1);
        return rest.empty() ? d.real : d.real / rest;
    }
    return p;
}

std::vector<drive> drives()
{
    state& st = the();
    std::lock_guard lock{st.mutex};
    return st.drives;
}

void* directory() noexcept
{
    state& st = the();
    std::lock_guard lock{st.mutex};
    return st.enabled && st.enabled->has_value() ? st.directory : nullptr;
}

result<void> give(void* process)
{
    state& st = the();
    HANDLE dir = nullptr;
    {
        std::lock_guard lock{st.mutex};
        if (!st.enabled || !*st.enabled) return {};
        dir = st.directory;
    }
    if (NTSTATUS const status = st.set_information(static_cast<HANDLE>(process), process_device_map, &dir, sizeof dir); status < 0)
        return fail("cannot set the device map of a process: NtSetInformationProcess " + status_text(status));
    return {};
}

#else

result<void> enable()
{
    return fail("virtual drives exist on Windows only");
}

bool enabled() noexcept
{
    return false;
}

result<fs::path> drive_for(fs::path const&)
{
    return fail("virtual drives exist on Windows only");
}

void release(fs::path const&)
{
}

fs::path to_virtual(fs::path const& p)
{
    return p;
}

fs::path to_real(fs::path const& p)
{
    return p;
}

std::vector<drive> drives()
{
    return {};
}

void* directory() noexcept
{
    return nullptr;
}

result<void> give(void*)
{
    return {};
}

#endif

}
