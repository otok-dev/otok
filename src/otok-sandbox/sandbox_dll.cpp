// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// otok-sandbox.dll (docs/PLAN.md decision 103): injected with Microsoft Detours into every process of a build that
// runs in otok's AppContainer. It answers what the container is refused but builds need, and only ever grants: every
// access still goes through the kernel's check, so a program bypassing these hooks gains nothing. Each hook is a
// finding of the probe (docs/tasks/windows-sandbox-appcontainer.md, section 3):
//   1. the null device, opened outside and duplicated;
//   2. named pipes retried in the container's LOCAL\ namespace;
//   3. the container added to explicit DACLs, NULL DACLs replaced, named objects made without security given the
//      user, SYSTEM and the container;
//   4. object directories (\BaseNamedObjects, the session's) redirected to the container's namespace;
//   5. QueryDosDeviceW answered from a drive table taken outside;
//   6. Cygwin's token default DACL and process DACL widened to the container;
//   7. drive roots and 8. the MountPointManager, opened outside with the least access and duplicated; a root's
//      attributes asked by name (NtQueryAttributesFile, xcopy's check of a drive) answered from that handle; the
//      ancestors of the work directory, exactly these, opened outside to pass through only (mkdir -p walks down them);
//   9. symbolic links (Cygwin's winpid.<n>) given the container;
//  10. MSVC's telemetry (VCTIP.EXE) not started: without the network it stalls the compiler for seconds;
//  11. otok's jobserver semaphore, made in the session's namespace and granted to the container, found there;
//  12. TEMP, TMP and LOCALAPPDATA, which CreateProcess rewrites for a container process, set back;
//  13. an ALPC client asking its server to be the user (cl /Zi /FS and mspdbsrv) asked again for the user in this
//      container, for the ports of the container's own RPC Control;
//  14. otok's device map (otok/core/virtual_drives: short letters for the cache and the work directories), which is not
//      inherited, set on every child before it runs: by the DLL itself outside a container, by otok's broker in one,
//      where a process may not set it;
//  15. GetFinalPathNameByHandleW giving a path under a virtual drive's directory through the letter, as the build named it
//      (realpath of Python, CMake, LLVM);
//  16. GetLongPathNameW refused for a path with no short name in it (it lists the ancestors, which the container may not
//      outside the cache: an NDK on another drive) giving the path back as it is (LLVM's programs expand their own);
//  17. FindFirstFile of one name refused (it lists the directory holding it) answered as the one entry found, from the
//      name's attributes (b2 finds the canonical name of each component of a path so); the ancestors of the toolchains
//      outside the cache handed as those of the work directory are (hook 8), for their attributes;
//  18. a listing of a directory on those ways, refused, showing the next names on them alone (b2 tells whether a file
//      exists by listing each of its ancestors).
// The DLL passes itself and the payload (otok/core/sandbox_windows.hpp) to every child through CreateProcessW/A. Without a
// container in the payload (sandbox = false) it does 14 and 15 alone.

#include <windows.h>
#include <winternl.h>
#include <sddl.h>

#include <detours/detours.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/sandbox_windows.hpp"

// Detours' helper for 32-bit children of a 64-bit process (rundll32 calls it by ordinal 1).
#pragma comment(linker, "/export:DetourFinishHelperProcess,@1,NONAME")

namespace {

using namespace otok::sandbox_windows;

constexpr NTSTATUS status_access_denied = static_cast<NTSTATUS>(0xC0000022L);
constexpr NTSTATUS status_object_name_not_found = static_cast<NTSTATUS>(0xC0000034L);
constexpr NTSTATUS status_object_name_collision = static_cast<NTSTATUS>(0xC0000035L);
constexpr ULONG file_opened = 1;

payload the_payload{};
char dll_path[MAX_PATH * 3];  // the ANSI path Detours writes into each child's import table
PSID container_sid;
std::vector<char> container_sid_buffer;
std::wstring container_objects;
std::wstring object_scope;  // the payload's: begins the first name of what is moved into the container's namespace

// --- the log of refusals ---------------------------------------------------------------------------------------------

thread_local bool in_log;

std::wstring_view view(UNICODE_STRING const* s)
{
    return s && s->Buffer ? std::wstring_view(s->Buffer, s->Length / sizeof(wchar_t)) : std::wstring_view();
}

std::wstring_view name_of(POBJECT_ATTRIBUTES oa)
{
    return oa ? view(oa->ObjectName) : std::wstring_view();
}

void write_log(std::wstring_view what, std::wstring_view name, NTSTATUS status)
{
    static LONG lines = 0;
    if (!the_payload.log[0] || in_log || InterlockedIncrement(&lines) > 1000) return;
    in_log = true;
    HANDLE const file = ::CreateFileW(the_payload.log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        wchar_t exe[MAX_PATH];
        DWORD const n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring_view program(exe, n);
        if (auto const slash = program.rfind(L'\\'); slash != std::wstring_view::npos) program.remove_prefix(slash + 1);
        wchar_t status_text[16];
        ::swprintf_s(status_text, L"0x%08lx", static_cast<unsigned long>(status));
        std::wstring line = std::to_wstring(::GetCurrentProcessId());
        line.append(L" ").append(program).append(L" ").append(what).append(L" ").append(name).append(L" ")
            .append(status_text).append(L"\r\n");
        int const size = ::WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(size), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), size, nullptr, nullptr);
        DWORD written;
        ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        ::CloseHandle(file);
    }
    in_log = false;
}

void log_denied(wchar_t const* what, POBJECT_ATTRIBUTES oa, ACCESS_MASK access, NTSTATUS status)
{
    wchar_t text[64];
    ::swprintf_s(text, L"%ls access=0x%lx", what, static_cast<unsigned long>(access));
    write_log(text, name_of(oa), status);
}

// --- names -----------------------------------------------------------------------------------------------------------

bool equals_ci(std::wstring_view s, std::wstring_view literal)
{
    return s.size() == literal.size() && ::_wcsnicmp(s.data(), literal.data(), s.size()) == 0;
}

bool starts_with_ci(std::wstring_view s, std::wstring_view prefix)
{
    return s.size() >= prefix.size() && ::_wcsnicmp(s.data(), prefix.data(), prefix.size()) == 0;
}

template <class Call>
NTSTATUS with_name(POBJECT_ATTRIBUTES oa, std::wstring const& name, Call call)
{
    auto const bytes = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    UNICODE_STRING string{bytes, bytes, const_cast<wchar_t*>(name.c_str())};
    OBJECT_ATTRIBUTES copy = *oa;
    copy.ObjectName = &string;
    return call(&copy);
}

bool is_null_device(POBJECT_ATTRIBUTES oa)
{
    if (!oa || oa->RootDirectory) return false;
    auto const name = name_of(oa);
    return equals_ci(name, L"\\??\\NUL") || equals_ci(name, L"\\Device\\Null") || equals_ci(name, L"\\??\\GLOBALROOT\\Device\\Null");
}

// \??\pipe\name → \??\pipe\LOCAL\name, the only pipe namespace an AppContainer may create in; a name relative to a
// directory handle (Cygwin opens its pipes relative to \Device\NamedPipe\) → LOCAL\name.
bool local_pipe_name(POBJECT_ATTRIBUTES oa, std::wstring& out)
{
    if (!oa) return false;
    auto const name = name_of(oa);
    if (oa->RootDirectory) {
        if (name.empty() || name.find(L'\\') != std::wstring_view::npos) return false;
        out = L"LOCAL\\";
        out.append(name);
        return true;
    }
    constexpr std::wstring_view prefix = L"\\??\\pipe\\";
    if (name.size() <= prefix.size() || !starts_with_ci(name, prefix) || starts_with_ci(name.substr(prefix.size()), L"LOCAL\\"))
        return false;
    out.assign(prefix).append(L"LOCAL\\").append(name.substr(prefix.size()));
    return true;
}

// \BaseNamedObjects\x, \Sessions\BNOLINKS\<n>\x, \Sessions\<n>\BaseNamedObjects\x → the container's namespace\x.
bool container_object_name(POBJECT_ATTRIBUTES oa, std::wstring& out)
{
    if (container_objects.empty() || !oa || oa->RootDirectory) return false;
    auto const name = name_of(oa);
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    std::wstring const prefixes[] = {
        L"\\BaseNamedObjects\\",
        L"\\Sessions\\BNOLINKS\\" + std::to_wstring(session) + L"\\",
        L"\\Sessions\\" + std::to_wstring(session) + L"\\BaseNamedObjects\\",
    };
    for (auto const& prefix : prefixes)
        if (name.size() > prefix.size() && starts_with_ci(name, prefix)) {
            out = container_objects + L"\\";
            if (!object_scope.empty()) out.append(object_scope).append(L"-");
            out.append(name.substr(prefix.size()));
            return true;
        }
    return false;
}

// --- handles opened outside ------------------------------------------------------------------------------------------

bool give_duplicate(unsigned long long handle, PHANDLE out, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb)
{
    if (!handle) return false;
    BOOL const inherit = oa && (oa->Attributes & OBJ_INHERIT) != 0;
    if (!::DuplicateHandle(::GetCurrentProcess(), reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(handle)), ::GetCurrentProcess(),
            out, 0, inherit, DUPLICATE_SAME_ACCESS))
        return false;
    iosb->Status = 0;
    iosb->Information = file_opened;
    return true;
}

// The handle of the drive root \??\X:\ opened outside (root_access), 0 for none or another name.
unsigned long long root_handle(POBJECT_ATTRIBUTES oa)
{
    if (!oa || oa->RootDirectory) return 0;
    auto const name = name_of(oa);
    if (name.size() != 7 || !starts_with_ci(name, L"\\??\\") || name[5] != L':' || name[6] != L'\\') return 0;
    wchar_t const letter = static_cast<wchar_t>(std::towupper(name[4]));
    if (letter < L'A' || letter > L'Z') return 0;
    return the_payload.roots[letter - L'A'];
}

// \??\X:\ asked with no more than root_access.
bool give_root(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb)
{
    if (access & ~root_access) return false;
    return give_duplicate(root_handle(oa), out, oa, iosb);
}

// The handle of an ancestor of the work directory opened outside (ancestor_access), 0 for any other name: the name must
// be one of the payload's whole (a backslash at its end aside), never something below or beside it.
unsigned long long ancestor_handle(POBJECT_ATTRIBUTES oa)
{
    if (!oa || oa->RootDirectory) return 0;
    auto name = name_of(oa);
    if (name.size() > 7 && name.back() == L'\\') name.remove_suffix(1);
    for (unsigned i = 0; i < the_payload.ancestor_count && i < max_ancestors; ++i)
        if (equals_ci(name, the_payload.ancestors[i].name)) return the_payload.ancestors[i].handle;
    return 0;
}

// An ancestor asked with no more than ancestor_access: to pass through it (mkdir -p walking down from the root).
bool give_ancestor(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb)
{
    if (access & ~ancestor_access) return false;
    return give_duplicate(ancestor_handle(oa), out, oa, iosb);
}

bool give_mountmgr(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb)
{
    if (!oa || oa->RootDirectory || (access & ~mountmgr_access) || !equals_ci(name_of(oa), L"\\??\\MountPointManager")) return false;
    return give_duplicate(the_payload.mountmgr, out, oa, iosb);
}

// --- security descriptors --------------------------------------------------------------------------------------------

// A security descriptor like `in` whose DACL grants the container too. Python's mkdir(mode=0o700) makes protected
// owner-only DACLs that would shut the container out of its own temporary directories; a NULL DACL opens an object to
// everyone but AppContainers, so it becomes Everyone + the container.
struct widened_sd
{
    SECURITY_DESCRIPTOR sd{};
    std::vector<char> acl;

    bool widen(PSECURITY_DESCRIPTOR in)
    {
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        if (!container_sid || !in || !::GetSecurityDescriptorDacl(in, &present, &dacl, &defaulted) || !present) return false;
        ::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        if (!dacl) {
            SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
            PSID everyone = nullptr;
            if (!::AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone)) return false;
            DWORD const size = sizeof(ACL) + 2 * sizeof(ACCESS_ALLOWED_ACE) + ::GetLengthSid(everyone) + ::GetLengthSid(container_sid);
            acl.assign(size, 0);
            auto const list = reinterpret_cast<PACL>(acl.data());
            ::InitializeAcl(list, size, ACL_REVISION);
            ::AddAccessAllowedAce(list, ACL_REVISION, GENERIC_ALL, everyone);
            ::AddAccessAllowedAce(list, ACL_REVISION, GENERIC_ALL, container_sid);
            ::FreeSid(everyone);
        }
        else {
            ACL_SIZE_INFORMATION info{};
            ::GetAclInformation(dacl, &info, sizeof info, AclSizeInformation);
            DWORD const size = info.AclBytesInUse + sizeof(ACCESS_ALLOWED_ACE) + ::GetLengthSid(container_sid);
            acl.assign(size, 0);
            auto const list = reinterpret_cast<PACL>(acl.data());
            ::InitializeAcl(list, size, ACL_REVISION);
            for (DWORD i = 0; i < info.AceCount; ++i) {
                void* ace = nullptr;
                if (::GetAce(dacl, i, &ace)) ::AddAce(list, ACL_REVISION, MAXDWORD, ace, static_cast<ACE_HEADER*>(ace)->AceSize);
            }
            ::AddAccessAllowedAceEx(list, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, GENERIC_ALL, container_sid);
            SECURITY_DESCRIPTOR_CONTROL control = 0;
            DWORD revision = 0;
            ::GetSecurityDescriptorControl(in, &control, &revision);
            if (control & SE_DACL_PROTECTED) ::SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
            PSID owner = nullptr;
            if (::GetSecurityDescriptorOwner(in, &owner, &defaulted) && owner) ::SetSecurityDescriptorOwner(&sd, owner, defaulted);
        }
        ::SetSecurityDescriptorDacl(&sd, TRUE, reinterpret_cast<PACL>(acl.data()), FALSE);
        return true;
    }
};

// The security of named objects and pipes made without one: the defaults (NPFS's, the token's default DACL made in
// the container) leave the container out, which then could not open its own objects. From the payload's SIDs: a
// forked Cygwin child cannot always read its own token.
PSECURITY_DESCRIPTOR container_default_sd()
{
    static PSECURITY_DESCRIPTOR const sd = [] {
        PSECURITY_DESCRIPTOR result = nullptr;
        if (!the_payload.user_sid[0] || !the_payload.container_sid[0]) return result;
        std::wstring const sddl = std::wstring(L"D:(A;;GA;;;") + the_payload.user_sid + L")(A;;GA;;;SY)(A;;GA;;;" +
                                  the_payload.container_sid + L")";
        ::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &result, nullptr);
        return result;
    }();
    return sd;
}

// Object attributes with the container granted: a named object without security gets container_default_sd, one with
// a DACL gets the container added.
struct secured_attributes
{
    widened_sd wide;
    OBJECT_ATTRIBUTES copy{};

    POBJECT_ATTRIBUTES apply(POBJECT_ATTRIBUTES in, bool named_only = true)
    {
        if (!in || (named_only && name_of(in).empty())) return in;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!in->SecurityDescriptor) sd = named_only ? container_default_sd() : nullptr;
        else if (wide.widen(in->SecurityDescriptor)) sd = &wide.sd;
        if (!sd) return in;
        copy = *in;
        copy.SecurityDescriptor = sd;
        return &copy;
    }
};

// --- the hooks -------------------------------------------------------------------------------------------------------

using nt_create_file_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
    ULONG, ULONG, ULONG, PVOID, ULONG);
using nt_open_file_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
using nt_create_named_pipe_file_t = NTSTATUS(NTAPI*)(PHANDLE, ULONG, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG, ULONG,
    ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, PLARGE_INTEGER);
using nt_directory_object_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using nt_create_directory_object_ex_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, ULONG);
using nt_create_event_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, BOOLEAN);
using nt_create_mutant_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN);
using nt_create_semaphore_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG, LONG);
using nt_open_semaphore_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
using nt_create_section_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
using nt_create_symbolic_link_object_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PUNICODE_STRING);
using nt_set_information_token_t = NTSTATUS(NTAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, PVOID, ULONG);
using nt_set_security_object_t = NTSTATUS(NTAPI*)(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);
using query_dos_device_w_t = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
using create_process_w_t = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
    LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
using create_process_a_t = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
    LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);

nt_create_file_t real_nt_create_file;
nt_open_file_t real_nt_open_file;
nt_create_named_pipe_file_t real_nt_create_named_pipe_file;
nt_directory_object_t real_nt_create_directory_object;
nt_create_directory_object_ex_t real_nt_create_directory_object_ex;
nt_directory_object_t real_nt_open_directory_object;
nt_create_event_t real_nt_create_event;
nt_create_mutant_t real_nt_create_mutant;
nt_create_semaphore_t real_nt_create_semaphore;
nt_open_semaphore_t real_nt_open_semaphore;
nt_create_section_t real_nt_create_section;
nt_create_symbolic_link_object_t real_nt_create_symbolic_link_object;
nt_set_information_token_t real_nt_set_information_token;
nt_set_security_object_t real_nt_set_security_object;
query_dos_device_w_t real_query_dos_device_w;
create_process_w_t real_create_process_w;
create_process_a_t real_create_process_a;

NTSTATUS NTAPI hook_nt_create_file(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb,
    PLARGE_INTEGER allocation, ULONG attributes, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG ea_length)
{
    secured_attributes secured;
    oa = secured.apply(oa, false);
    NTSTATUS const status = real_nt_create_file(handle, access, oa, iosb, allocation, attributes, share, disposition, options, ea, ea_length);
    if (status == status_access_denied) {
        if (is_null_device(oa) && give_duplicate(the_payload.null_device, handle, oa, iosb)) return 0;
        if (give_mountmgr(handle, access, oa, iosb)) return 0;
        if ((disposition == FILE_OPEN || disposition == FILE_OPEN_IF) && give_root(handle, access, oa, iosb)) return 0;
        if ((disposition == FILE_OPEN || disposition == FILE_OPEN_IF) && give_ancestor(handle, access, oa, iosb)) return 0;
    }
    std::wstring local;
    if ((status == status_access_denied || status == status_object_name_not_found) && local_pipe_name(oa, local)) {
        NTSTATUS const retried = with_name(oa, local, [&](POBJECT_ATTRIBUTES named) {
            return real_nt_create_file(handle, access, named, iosb, allocation, attributes, share, disposition, options, ea, ea_length);
        });
        if (retried >= 0) return retried;
    }
    if (status == status_access_denied) log_denied(L"denied-create", oa, access, status);
    return status;
}

NTSTATUS NTAPI hook_nt_open_file(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb, ULONG share, ULONG options)
{
    NTSTATUS const status = real_nt_open_file(handle, access, oa, iosb, share, options);
    if (status == status_access_denied) {
        if (is_null_device(oa) && give_duplicate(the_payload.null_device, handle, oa, iosb)) return 0;
        if (give_mountmgr(handle, access, oa, iosb)) return 0;
        if (give_root(handle, access, oa, iosb)) return 0;
        if (give_ancestor(handle, access, oa, iosb)) return 0;
    }
    std::wstring local;
    if ((status == status_access_denied || status == status_object_name_not_found) && local_pipe_name(oa, local)) {
        NTSTATUS const retried = with_name(oa, local, [&](POBJECT_ATTRIBUTES named) {
            return real_nt_open_file(handle, access, named, iosb, share, options);
        });
        if (retried >= 0) return retried;
    }
    if (status == status_access_denied) log_denied(L"denied-open", oa, access, status);
    return status;
}

// The attributes of a drive root asked by name (GetFileAttributesW("C:\"), with which xcopy checks a drive): answered from
// the root opened outside, which holds FILE_READ_ATTRIBUTES; nothing more than hook 7 gives.
using nt_query_information_file_t = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
using nt_query_attributes_file_t = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
nt_query_information_file_t nt_query_information_file;
nt_query_attributes_file_t real_nt_query_attributes_file;
nt_query_attributes_file_t real_nt_query_full_attributes_file;

constexpr ULONG file_basic_information = 4;          // FILE_BASIC_INFORMATION, 40 bytes
constexpr ULONG file_network_open_information = 34;  // FILE_NETWORK_OPEN_INFORMATION, 56 bytes

NTSTATUS root_attributes(NTSTATUS status, POBJECT_ATTRIBUTES oa, PVOID information, ULONG type, ULONG size)
{
    if (status != status_access_denied || !nt_query_information_file || !information) return status;
    auto root = root_handle(oa);
    if (!root) root = ancestor_handle(oa);
    if (!root) return status;
    IO_STATUS_BLOCK iosb{};
    NTSTATUS const answered =
        nt_query_information_file(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(root)), &iosb, information, size, type);
    return answered >= 0 ? answered : status;
}

NTSTATUS NTAPI hook_nt_query_attributes_file(POBJECT_ATTRIBUTES oa, PVOID information)
{
    return root_attributes(real_nt_query_attributes_file(oa, information), oa, information, file_basic_information, 40);
}

NTSTATUS NTAPI hook_nt_query_full_attributes_file(POBJECT_ATTRIBUTES oa, PVOID information)
{
    return root_attributes(real_nt_query_full_attributes_file(oa, information), oa, information, file_network_open_information, 56);
}

NTSTATUS NTAPI hook_nt_create_named_pipe_file(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK iosb,
    ULONG share, ULONG disposition, ULONG options, ULONG type, ULONG read_mode, ULONG completion, ULONG instances,
    ULONG in_quota, ULONG out_quota, PLARGE_INTEGER timeout)
{
    secured_attributes secured;
    oa = secured.apply(oa, false);
    if (oa && !oa->SecurityDescriptor && container_default_sd()) {
        secured.copy = *oa;
        secured.copy.SecurityDescriptor = container_default_sd();
        oa = &secured.copy;
    }
    NTSTATUS const status = real_nt_create_named_pipe_file(handle, access, oa, iosb, share, disposition, options, type, read_mode,
        completion, instances, in_quota, out_quota, timeout);
    std::wstring local;
    if (status == status_access_denied && local_pipe_name(oa, local))
        return with_name(oa, local, [&](POBJECT_ATTRIBUTES named) {
            return real_nt_create_named_pipe_file(handle, access, named, iosb, share, disposition, options, type, read_mode,
                completion, instances, in_quota, out_quota, timeout);
        });
    if (status == status_access_denied) log_denied(L"denied-pipe", oa, access, status);
    return status;
}

NTSTATUS NTAPI hook_nt_create_directory_object(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa)
{
    secured_attributes secured;
    oa = secured.apply(oa);
    NTSTATUS const status = real_nt_create_directory_object(handle, access, oa);
    std::wstring name;
    if (status == status_access_denied && container_object_name(oa, name)) {
        NTSTATUS retried = with_name(oa, name, [&](POBJECT_ATTRIBUTES named) { return real_nt_create_directory_object(handle, access, named); });
        // the global and the session's directory are one in the container: the second one opens the first
        if (retried == status_object_name_collision)
            retried = with_name(oa, name, [&](POBJECT_ATTRIBUTES named) { return real_nt_open_directory_object(handle, access, named); });
        return retried;
    }
    if (status == status_access_denied) log_denied(L"denied-directory", oa, access, status);
    return status;
}

NTSTATUS NTAPI hook_nt_create_directory_object_ex(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, HANDLE shadow, ULONG flags)
{
    NTSTATUS const status = real_nt_create_directory_object_ex(handle, access, oa, shadow, flags);
    std::wstring name;
    if (status == status_access_denied && container_object_name(oa, name))
        return with_name(oa, name, [&](POBJECT_ATTRIBUTES named) {
            return real_nt_create_directory_object_ex(handle, access, named, shadow, flags);
        });
    return status;
}

NTSTATUS NTAPI hook_nt_open_directory_object(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa)
{
    NTSTATUS const status = real_nt_open_directory_object(handle, access, oa);
    std::wstring name;
    if ((status == status_access_denied || status == status_object_name_not_found) && container_object_name(oa, name)) {
        NTSTATUS const retried = with_name(oa, name, [&](POBJECT_ATTRIBUTES named) { return real_nt_open_directory_object(handle, access, named); });
        if (retried >= 0) return retried;
    }
    if (status == status_access_denied) log_denied(L"denied-directory-open", oa, access, status);
    return status;
}

NTSTATUS NTAPI hook_nt_create_event(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, ULONG type, BOOLEAN state)
{
    secured_attributes secured;
    return real_nt_create_event(handle, access, secured.apply(oa), type, state);
}

NTSTATUS NTAPI hook_nt_create_mutant(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, BOOLEAN owner)
{
    secured_attributes secured;
    return real_nt_create_mutant(handle, access, secured.apply(oa), owner);
}

NTSTATUS NTAPI hook_nt_create_semaphore(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, LONG initial, LONG maximum)
{
    secured_attributes secured;
    return real_nt_create_semaphore(handle, access, secured.apply(oa), initial, maximum);
}

// A semaphore the container's namespace lacks, looked for in the session's: otok's jobserver, made outside the
// container (whose namespace exists only while a process of it does) and granted to it. The kernel opens it only if
// its DACL grants the container.
NTSTATUS NTAPI hook_nt_open_semaphore(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa)
{
    NTSTATUS const status = real_nt_open_semaphore(handle, access, oa);
    if (status != status_object_name_not_found || !oa) return status;
    auto name = name_of(oa);
    if (!oa->RootDirectory) {
        // \Sessions\<n>\AppContainerNamedObjects\<SID>\name
        if (container_objects.empty() || !starts_with_ci(name, container_objects + L"\\")) return status;
        name.remove_prefix(container_objects.size() + 1);
    }
    if (name.empty() || name.find(L'\\') != std::wstring_view::npos) return status;
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    std::wstring const session_name = L"\\Sessions\\" + std::to_wstring(session) + L"\\BaseNamedObjects\\" + std::wstring(name);
    OBJECT_ATTRIBUTES absolute = *oa;
    absolute.RootDirectory = nullptr;
    NTSTATUS const retried = with_name(&absolute, session_name, [&](POBJECT_ATTRIBUTES named) { return real_nt_open_semaphore(handle, access, named); });
    return retried >= 0 ? retried : status;
}

NTSTATUS NTAPI hook_nt_create_section(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PLARGE_INTEGER size,
    ULONG protection, ULONG attributes, HANDLE file)
{
    secured_attributes secured;
    oa = secured.apply(oa);
    NTSTATUS const status = real_nt_create_section(handle, access, oa, size, protection, attributes, file);
    if (status == status_access_denied) log_denied(L"denied-section", oa, access, status);
    return status;
}

NTSTATUS NTAPI hook_nt_create_symbolic_link_object(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PUNICODE_STRING target)
{
    secured_attributes secured;
    oa = secured.apply(oa);
    NTSTATUS const status = real_nt_create_symbolic_link_object(handle, access, oa, target);
    if (status == status_access_denied) log_denied(L"denied-link", oa, access, status);
    return status;
}

// Cygwin sets its token's default DACL and its process's DACL to the user, Administrators and SYSTEM (uinfo.cc):
// objects it then makes without security, and its children, would shut the container out.
NTSTATUS NTAPI hook_nt_set_information_token(HANDLE token, TOKEN_INFORMATION_CLASS type, PVOID information, ULONG size)
{
    if (type == TokenDefaultDacl && information && static_cast<TOKEN_DEFAULT_DACL*>(information)->DefaultDacl) {
        SECURITY_DESCRIPTOR in;
        ::InitializeSecurityDescriptor(&in, SECURITY_DESCRIPTOR_REVISION);
        ::SetSecurityDescriptorDacl(&in, TRUE, static_cast<TOKEN_DEFAULT_DACL*>(information)->DefaultDacl, FALSE);
        widened_sd wide;
        if (wide.widen(&in)) {
            TOKEN_DEFAULT_DACL dacl{reinterpret_cast<PACL>(wide.acl.data())};
            return real_nt_set_information_token(token, type, &dacl, sizeof dacl);
        }
    }
    return real_nt_set_information_token(token, type, information, size);
}

NTSTATUS NTAPI hook_nt_set_security_object(HANDLE handle, SECURITY_INFORMATION what, PSECURITY_DESCRIPTOR sd)
{
    widened_sd wide;
    if ((what & DACL_SECURITY_INFORMATION) && sd && wide.widen(sd)) return real_nt_set_security_object(handle, what, &wide.sd);
    return real_nt_set_security_object(handle, what, sd);
}

// QueryDosDeviceW(NULL) lists \??, which an AppContainer may not enumerate, nor may it open the drive links: the drive
// letters and the table taken outside stand in (Cygwin's handle_to_fn needs them to name inherited handles).
DWORD WINAPI hook_query_dos_device_w(LPCWSTR name, LPWSTR buffer, DWORD size)
{
    DWORD const result = real_query_dos_device_w(name, buffer, size);
    DWORD const error = ::GetLastError();
    if (result || error != ERROR_ACCESS_DENIED) return result;
    if (!name) {
        DWORD const drives = ::GetLogicalDrives();
        DWORD n = 0;
        for (int i = 0; i < 26; ++i) {
            if (!(drives & (1u << i))) continue;
            if (n + 4 > size) {
                ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            buffer[n++] = static_cast<wchar_t>(L'A' + i);
            buffer[n++] = L':';
            buffer[n++] = 0;
        }
        buffer[n++] = 0;
        ::SetLastError(0);
        return n;
    }
    if (std::wcslen(name) == 2 && name[1] == L':') {
        wchar_t const letter = static_cast<wchar_t>(std::towupper(name[0]));
        if (letter >= L'A' && letter <= L'Z') {
            wchar_t const* device = the_payload.dos_devices[letter - L'A'];
            std::size_t const n = std::wcslen(device);
            if (n && n + 2 <= size) {
                std::wmemcpy(buffer, device, n);
                buffer[n] = 0;
                buffer[n + 1] = 0;
                ::SetLastError(0);
                return static_cast<DWORD>(n + 2);
            }
        }
    }
    ::SetLastError(error);
    return result;
}

using nt_set_information_process_t = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
nt_set_information_process_t real_nt_set_information_process;  // not hooked: what hook 14 calls

// --- children --------------------------------------------------------------------------------------------------------

// Duplicates `handle` into the child: its value there, 0 for none.
unsigned long long duplicate_into(HANDLE child, unsigned long long handle)
{
    HANDLE copy = nullptr;
    if (!handle || !::DuplicateHandle(::GetCurrentProcess(), reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(handle)), child, &copy,
                       0, FALSE, DUPLICATE_SAME_ACCESS))
        return 0;
    return reinterpret_cast<ULONG_PTR>(copy);
}

// The device map of a new suspended child (hook 14): set here outside a container; in one, by otok's broker, to which
// the handle's value is sent.
void give_map(HANDLE child)
{
    if (the_payload.map_directory) {
        HANDLE directory = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(the_payload.map_directory));
        if (NTSTATUS const status = real_nt_set_information_process(child, 23 /* ProcessDeviceMap */, &directory, sizeof directory); status < 0)
            write_log(L"device-map", L"", status);
        return;
    }
    if (!the_payload.map_pipe[0]) return;
    unsigned long long request = reinterpret_cast<ULONG_PTR>(child);
    LONG reply = 0;
    DWORD got = 0;
    if (!::CallNamedPipeW(the_payload.map_pipe, &request, sizeof request, &reply, sizeof reply, &got, 10000) || got != sizeof reply)
        write_log(L"device-map-broker", L"", static_cast<NTSTATUS>(HRESULT_FROM_WIN32(::GetLastError())));
    else if (reply < 0)
        write_log(L"device-map", L"", reply);
}

// Gives a new suspended child its copy of the payload, then lets it run unless its creator wanted it suspended.
void after_create(DWORD flags, LPPROCESS_INFORMATION pi, wchar_t const* intended_environment)
{
    static payload copy;  // large: not on the stack of whatever thread starts processes
    static SRWLOCK lock = SRWLOCK_INIT;
    ::AcquireSRWLockExclusive(&lock);
    copy = the_payload;
    take_variables(copy, intended_environment);
    copy.null_device = duplicate_into(pi->hProcess, the_payload.null_device);
    copy.mountmgr = duplicate_into(pi->hProcess, the_payload.mountmgr);
    for (int i = 0; i < 26; ++i) copy.roots[i] = duplicate_into(pi->hProcess, the_payload.roots[i]);
    for (unsigned i = 0; i < the_payload.ancestor_count && i < max_ancestors; ++i)
        copy.ancestors[i].handle = duplicate_into(pi->hProcess, the_payload.ancestors[i].handle);
    copy.map_directory = duplicate_into(pi->hProcess, the_payload.map_directory);
    ::DetourCopyPayloadToProcess(pi->hProcess, payload_guid, &copy, sizeof copy);
    ::ReleaseSRWLockExclusive(&lock);
    give_map(pi->hProcess);
    if (!(flags & CREATE_SUSPENDED)) ::ResumeThread(pi->hThread);
}

// The children's ASLR as the first process's: no high entropy, so that DYNAMIC_BASE images land where they did in the
// parent (an AppContainer forces relocation; Cygwin's fork copies memory by the parent's addresses).
struct child_attributes
{
    std::vector<char> list;
    DWORD64 mitigation = PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_OFF;

    LPPROC_THREAD_ATTRIBUTE_LIST make()
    {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        list.resize(size);
        auto const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
        if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
            list.clear();
            return nullptr;
        }
        if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigation, sizeof mitigation, nullptr, nullptr))
            return nullptr;
        return attributes;
    }

    ~child_attributes()
    {
        if (!list.empty()) ::DeleteProcThreadAttributeList(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data()));
    }
};

template <class Char>
bool is_telemetry(Char const* application, Char const* command_line)
{
    Char const* const both[] = {application, command_line};
    for (Char const* s : both) {
        if (!s) continue;
        std::basic_string<Char> lower(s);
        for (auto& c : lower) c = static_cast<Char>(std::towlower(static_cast<wint_t>(c)));
        constexpr Char name[] = {'v', 'c', 't', 'i', 'p', '.', 'e', 'x', 'e', 0};
        if (lower.find(name) != std::basic_string<Char>::npos) return true;
    }
    return false;
}

// An environment block as CreateProcess takes it (UTF-16 with CREATE_UNICODE_ENVIRONMENT, else the ANSI code page;
// null: this process's), as UTF-16, ending with two nulls.
std::wstring intended_environment(LPVOID environment, DWORD flags)
{
    if (!environment) {
        wchar_t* const own = ::GetEnvironmentStringsW();
        std::wstring out = with_variable(own);
        ::FreeEnvironmentStringsW(own);
        return out;
    }
    if (flags & CREATE_UNICODE_ENVIRONMENT) return with_variable(static_cast<wchar_t const*>(environment));
    auto const ansi = static_cast<char const*>(environment);
    std::size_t length = 0;
    while (ansi[length] || ansi[length + 1]) ++length;
    length += 2;
    int const n = ::MultiByteToWideChar(CP_ACP, 0, ansi, static_cast<int>(length), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, ansi, static_cast<int>(length), out.data(), n);
    return out;
}

template <class StartupInfoEx, class StartupInfo, class Char, class Create, class Real>
BOOL create_process(Char const* application, Char* command_line, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment, Char const* directory,
    StartupInfo* startup, LPPROCESS_INFORMATION pi, Create create_with_dll, Real real)
{
    if (is_telemetry(application, command_line)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    StartupInfoEx extended{};
    child_attributes attributes;
    DWORD extra = 0;
    if (!(flags & EXTENDED_STARTUPINFO_PRESENT) && startup)
        if (auto const list = attributes.make()) {
            std::memcpy(&extended.StartupInfo, startup, std::min<std::size_t>(startup->cb, sizeof extended.StartupInfo));
            extended.StartupInfo.cb = sizeof extended;
            extended.lpAttributeList = list;
            startup = &extended.StartupInfo;
            extra = EXTENDED_STARTUPINFO_PRESENT;
        }
    // the environment the creator means (its own when it gives none), as UTF-16; what the child is given has
    // LOCALAPPDATA, without which CreateProcess refuses a container process (the child's DLL removes it again)
    std::wstring const intended = intended_environment(environment, flags);
    std::wstring given = with_variable(intended.c_str(), L"LOCALAPPDATA", L"C:\\");
    if (!create_with_dll(application, command_line, process_attributes, thread_attributes, inherit,
            flags | extra | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, given.data(), directory, startup, pi, dll_path, real))
        return FALSE;
    after_create(flags, pi, intended.c_str());
    return TRUE;
}

BOOL WINAPI hook_create_process_w(LPCWSTR application, LPWSTR command_line, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION pi)
{
    return create_process<STARTUPINFOEXW>(application, command_line, process_attributes, thread_attributes, inherit, flags,
        environment, directory, startup, pi, &::DetourCreateProcessWithDllExW, real_create_process_w);
}

BOOL WINAPI hook_create_process_a(LPCSTR application, LPSTR command_line, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment, LPCSTR directory,
    LPSTARTUPINFOA startup, LPPROCESS_INFORMATION pi)
{
    return create_process<STARTUPINFOEXA>(application, command_line, process_attributes, thread_attributes, inherit, flags,
        environment, directory, startup, pi, &::DetourCreateProcessWithDllExA, real_create_process_a);
}

// --- ALPC ------------------------------------------------------------------------------------------------------------

using nt_alpc_connect_port_t = NTSTATUS(NTAPI*)(PHANDLE, PUNICODE_STRING, POBJECT_ATTRIBUTES, PVOID, ULONG, PSID, PVOID, PULONG,
    PVOID, PVOID, PLARGE_INTEGER);
using nt_alpc_connect_port_ex_t = NTSTATUS(NTAPI*)(PHANDLE, POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES, PVOID, ULONG,
    PSECURITY_DESCRIPTOR, PVOID, PSIZE_T, PVOID, PVOID, PLARGE_INTEGER);

nt_alpc_connect_port_t real_nt_alpc_connect_port;
nt_alpc_connect_port_ex_t nt_alpc_connect_port_ex;

constexpr NTSTATUS status_server_sid_mismatch = static_cast<NTSTATUS>(0xC00002A0L);

// A client that asks its server to be the user (mspdb140.dll of `cl /Zi /FS` connecting to mspdbsrv): the kernel checks
// the server's token against a descriptor granting that SID, and a container's token passes only where its package SID
// is granted too, so a server in the container is never "the user". For a port of the container's own `RPC Control`,
// the requirement is asked again as the user in this container: a server of another user or container is refused as
// before, and ports outside the container's directory (LSA, the endpoint mapper) are never tried again
// (docs/tasks/windows-sandbox-mspdbsrv.md).
NTSTATUS NTAPI hook_nt_alpc_connect_port(PHANDLE port, PUNICODE_STRING name, POBJECT_ATTRIBUTES attributes, PVOID port_attributes,
    ULONG flags, PSID server, PVOID message, PULONG length, PVOID out_attributes, PVOID in_attributes, PLARGE_INTEGER timeout)
{
    ULONG const given = length ? *length : 0;
    NTSTATUS const status = real_nt_alpc_connect_port(port, name, attributes, port_attributes, flags, server, message, length,
        out_attributes, in_attributes, timeout);
    if (status != status_server_sid_mismatch || !nt_alpc_connect_port_ex || !server || container_objects.empty()
        || !the_payload.user_sid[0] || !the_payload.container_sid[0])
        return status;
    if (!starts_with_ci(view(name), container_objects + L"\\RPC Control\\") || view(name).size() <= container_objects.size() + 13)
        return status;
    PSID user = nullptr;
    if (!::ConvertStringSidToSidW(the_payload.user_sid, &user)) return status;
    bool const asks_user = ::EqualSid(server, user) != FALSE;
    ::LocalFree(user);
    if (!asks_user) return status;
    std::wstring const sddl = std::wstring(L"O:") + the_payload.user_sid + L"G:" + the_payload.user_sid + L"D:(A;;GA;;;"
        + the_payload.user_sid + L")(A;;GA;;;" + the_payload.container_sid + L")";
    PSECURITY_DESCRIPTOR required = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &required, nullptr)) return status;
    OBJECT_ATTRIBUTES connection;
    InitializeObjectAttributes(&connection, name, 0, nullptr, nullptr);
    SIZE_T size = given;
    NTSTATUS const retried = nt_alpc_connect_port_ex(port, &connection, attributes, port_attributes, flags, required, message,
        length ? &size : nullptr, out_attributes, in_attributes, timeout);
    if (length) *length = static_cast<ULONG>(size);
    ::LocalFree(required);
    return retried;
}

// --- virtual drives --------------------------------------------------------------------------------------------------

using get_final_path_name_by_handle_w_t = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
get_final_path_name_by_handle_w_t real_get_final_path_name_by_handle_w;

// "\\?\C:\long\x" or "C:\long\x" under a virtual drive's directory as "\\?\Q:\x" / "Q:\x"; empty otherwise.
std::wstring through_drive(std::wstring_view path)
{
    std::wstring_view prefix;
    std::wstring_view rest = path;
    if (rest.starts_with(L"\\\\?\\")) {
        prefix = rest.substr(0, 4);
        rest.remove_prefix(4);
    }
    for (unsigned i = 0; i < the_payload.drive_count && i < max_drives; ++i) {
        std::wstring_view const real = the_payload.drives[i].real;
        if (real.empty() || !starts_with_ci(rest, real) || (rest.size() > real.size() && rest[real.size()] != L'\\')) continue;
        std::wstring out{prefix};
        out += the_payload.drives[i].letter;
        out += L":\\";
        std::wstring_view tail = rest.substr(real.size());
        if (!tail.empty()) tail.remove_prefix(1);
        out += tail;
        return out;
    }
    return {};
}

// Hook 15: a path as a DOS path through the virtual drive (the deepest directory first, as the payload orders them).
// The volume as a GUID or an NT device, and the path alone, are left as they are.
DWORD WINAPI hook_get_final_path_name_by_handle_w(HANDLE file, LPWSTR buffer, DWORD size, DWORD flags)
{
    if ((flags & (VOLUME_NAME_GUID | VOLUME_NAME_NT | VOLUME_NAME_NONE)) != 0 || !the_payload.drive_count)
        return real_get_final_path_name_by_handle_w(file, buffer, size, flags);
    std::wstring real(1024, L'\0');
    DWORD n = real_get_final_path_name_by_handle_w(file, real.data(), static_cast<DWORD>(real.size()), flags);
    if (n >= real.size()) {
        real.resize(n + 1);
        n = real_get_final_path_name_by_handle_w(file, real.data(), static_cast<DWORD>(real.size()), flags);
    }
    if (n == 0 || n >= real.size()) return real_get_final_path_name_by_handle_w(file, buffer, size, flags);
    real.resize(n);
    std::wstring const short_path = through_drive(real);
    std::wstring const& out = short_path.empty() ? real : short_path;
    if (out.size() + 1 > size) return static_cast<DWORD>(out.size() + 1);
    std::wmemcpy(buffer, out.c_str(), out.size() + 1);
    return static_cast<DWORD>(out.size());
}

// --- long names ------------------------------------------------------------------------------------------------------

using get_long_path_name_w_t = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD);
get_long_path_name_w_t real_get_long_path_name_w;  // kernelbase's (the api-ms-win sets)
get_long_path_name_w_t real_get_long_path_name_w_kernel32;  // kernel32 has its own, which LLVM imports

// Hook 16: GetLongPathNameW lists each ancestor in which a name could be a short one (S:\Developments for "Android"); the
// container may not list the ancestors of a toolchain outside the cache, and LLVM's programs, which expand their own
// path at the start, end with "Permission denied". Refused, a path with no '~' (a short name NTFS makes always has one)
// that the container can reach is its own long form and is given back as it is. Nothing is listed; a path with a '~'
// keeps the refusal.
DWORD kept_long_path(get_long_path_name_w_t real, LPCWSTR short_path, LPWSTR long_path, DWORD size)
{
    // the call may be in place (LLVM's is): the path is kept before the real one writes into the buffer
    std::wstring const path = short_path ? std::wstring{short_path} : std::wstring{};
    DWORD const n = real(short_path, long_path, size);
    if (n != 0 || ::GetLastError() != ERROR_ACCESS_DENIED || path.empty() || path.find(L'~') != std::wstring::npos) return n;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    if (path.size() + 1 > size) return static_cast<DWORD>(path.size() + 1);
    std::wmemcpy(long_path, path.c_str(), path.size() + 1);
    return static_cast<DWORD>(path.size());
}

DWORD WINAPI hook_get_long_path_name_w(LPCWSTR short_path, LPWSTR long_path, DWORD size)
{
    return kept_long_path(real_get_long_path_name_w, short_path, long_path, size);
}

DWORD WINAPI hook_get_long_path_name_w_kernel32(LPCWSTR short_path, LPWSTR long_path, DWORD size)
{
    return kept_long_path(real_get_long_path_name_w_kernel32, short_path, long_path, size);
}

using find_first_file_ex_w_t = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using find_next_file_w_t = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAW);
using find_close_t = BOOL(WINAPI*)(HANDLE);
find_first_file_ex_w_t real_find_first_file_ex_w;
find_next_file_w_t real_find_next_file_w;
find_close_t real_find_close;

// The searches hooks 17 and 18 answered: each an event of its own, a handle no other search has, with the entries
// FindNextFile still gives; closed by FindClose.
struct answered_search
{
    HANDLE handle;
    std::vector<WIN32_FIND_DATAW> rest;
};
SRWLOCK answered_lock = SRWLOCK_INIT;
std::vector<answered_search> answered_searches;

// A search answered with `entries` (the first written into `data`), or the refusal kept when there is none.
HANDLE answer_search(std::vector<WIN32_FIND_DATAW> entries, LPVOID data)
{
    HANDLE const search = entries.empty() ? nullptr : ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!search) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    *static_cast<WIN32_FIND_DATAW*>(data) = entries.front();
    entries.erase(entries.begin());
    ::AcquireSRWLockExclusive(&answered_lock);
    answered_searches.push_back({search, std::move(entries)});
    ::ReleaseSRWLockExclusive(&answered_lock);
    ::SetLastError(ERROR_SUCCESS);
    return search;
}

WIN32_FIND_DATAW find_data(std::wstring_view name, DWORD attributes, FILETIME created, FILETIME accessed, FILETIME written, DWORD size_high,
    DWORD size_low)
{
    WIN32_FIND_DATAW found{};
    found.dwFileAttributes = attributes;
    found.ftCreationTime = created;
    found.ftLastAccessTime = accessed;
    found.ftLastWriteTime = written;
    found.nFileSizeHigh = size_high;
    found.nFileSizeLow = size_low;
    std::wmemcpy(found.cFileName, name.data(), std::min<std::size_t>(name.size(), MAX_PATH - 1));
    return found;
}

// "X:\a\b", "X:/a/b/" as the payload names directories: "\??\X:\a\b"; the root "\??\X:\". Empty for another form.
std::wstring nt_directory(std::wstring_view dos)
{
    if (dos.size() < 3 || dos[1] != L':' || (dos[2] != L'\\' && dos[2] != L'/')) return {};
    std::wstring out = L"\\??\\";
    for (wchar_t const c : dos) out += c == L'/' ? L'\\' : c;
    while (out.size() > 7 && out.back() == L'\\') out.pop_back();
    return out;
}

// Hook 18: a listing ("dir\*", "dir\*.*") of a directory on the ways the payload names, refused, shows the next names on
// those ways alone, from their handles: b2 tells whether a file exists by listing each of its ancestors (filent.cpp), so
// the NDK's compiler was missing to it. What the container learns is what otok named to it.
HANDLE listed_way(std::wstring_view pattern, LPVOID data)
{
    std::size_t const slash = pattern.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return INVALID_HANDLE_VALUE;
    std::wstring_view const mask = pattern.substr(slash + 1);
    if (mask != L"*" && mask != L"*.*") return INVALID_HANDLE_VALUE;
    std::wstring const directory = nt_directory(pattern.substr(0, slash + 1));
    if (directory.empty()) return INVALID_HANDLE_VALUE;
    std::vector<WIN32_FIND_DATAW> entries;
    for (unsigned i = 0; i < the_payload.ancestor_count && i < max_ancestors; ++i) {
        std::wstring_view const name = the_payload.ancestors[i].name;
        std::size_t const last = name.find_last_of(L'\\');
        if (last == std::wstring_view::npos || last + 1 >= name.size()) continue;  // a root, or no name
        // the parent of \??\X:\a is the root \??\X:\, with its backslash
        std::wstring_view const parent = name.substr(0, name[last - 1] == L':' ? last + 1 : last);
        if (!equals_ci(parent, directory)) continue;
        BY_HANDLE_FILE_INFORMATION information{};
        HANDLE const handle = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(the_payload.ancestors[i].handle));
        if (!::GetFileInformationByHandle(handle, &information)) continue;
        entries.push_back(find_data(name.substr(last + 1), information.dwFileAttributes, information.ftCreationTime,
            information.ftLastAccessTime, information.ftLastWriteTime, information.nFileSizeHigh, information.nFileSizeLow));
    }
    return answer_search(std::move(entries), data);
}

// Hook 17: FindFirstFile of one name, no wildcard in it, lists the directory holding it, which the container may not
// for the ancestors of a toolchain outside the cache: b2 finds the canonical name of each component of a path so (its
// pathnt.cpp). Refused, a name with no '~' whose attributes the container can read (a directory otok handed, or what is
// granted) is answered as the one entry found, its name as given; nothing beside it is told, and a name with another
// wildcard or a '~' keeps the refusal. FindFirstFileA/W/ExA come here through kernelbase.
HANDLE WINAPI hook_find_first_file_ex_w(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
{
    HANDLE const h = real_find_first_file_ex_w(name, level, data, op, filter, flags);
    if (h != INVALID_HANDLE_VALUE || ::GetLastError() != ERROR_ACCESS_DENIED || !name || !data || op != FindExSearchNameMatch
        || (level != FindExInfoStandard && level != FindExInfoBasic))
        return h;
    std::wstring_view const path = name;
    std::size_t const slash = path.find_last_of(L"\\/");
    std::wstring_view const leaf = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    if (leaf == L"*" || leaf == L"*.*") {
        HANDLE const listed = listed_way(path, data);
        if (listed == INVALID_HANDLE_VALUE) ::SetLastError(ERROR_ACCESS_DENIED);
        return listed;
    }
    if (leaf.empty() || leaf.size() >= MAX_PATH || leaf == L"." || leaf == L".." || path.find_first_of(L"*?<>\"~") != std::wstring_view::npos)
        return h;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!::GetFileAttributesExW(name, GetFileExInfoStandard, &a)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return answer_search({find_data(leaf, a.dwFileAttributes, a.ftCreationTime, a.ftLastAccessTime, a.ftLastWriteTime, a.nFileSizeHigh, a.nFileSizeLow)},
        data);
}

BOOL WINAPI hook_find_next_file_w(HANDLE search, LPWIN32_FIND_DATAW data)
{
    ::AcquireSRWLockExclusive(&answered_lock);
    auto const at = std::ranges::find(answered_searches, search, &answered_search::handle);
    bool const ours = at != answered_searches.end();
    bool const more = ours && !at->rest.empty();
    if (more) {
        *data = at->rest.front();
        at->rest.erase(at->rest.begin());
    }
    ::ReleaseSRWLockExclusive(&answered_lock);
    if (!ours) return real_find_next_file_w(search, data);
    ::SetLastError(more ? ERROR_SUCCESS : ERROR_NO_MORE_FILES);
    return more ? TRUE : FALSE;
}

BOOL WINAPI hook_find_close(HANDLE search)
{
    ::AcquireSRWLockExclusive(&answered_lock);
    auto const at = std::ranges::find(answered_searches, search, &answered_search::handle);
    bool const ours = at != answered_searches.end();
    if (ours) answered_searches.erase(at);
    ::ReleaseSRWLockExclusive(&answered_lock);
    return ours ? ::CloseHandle(search) : real_find_close(search);
}

// --- start -----------------------------------------------------------------------------------------------------------

// The container's SID from the token, or from the payload where the token cannot be read (a forked Cygwin child).
void find_container()
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        ::GetTokenInformation(token, TokenAppContainerSid, nullptr, 0, &size);
        container_sid_buffer.resize(size);
        if (size && ::GetTokenInformation(token, TokenAppContainerSid, container_sid_buffer.data(), size, &size))
            container_sid = reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(container_sid_buffer.data())->TokenAppContainer;
        ::CloseHandle(token);
    }
    if (!container_sid && the_payload.container_sid[0]) {
        PSID sid = nullptr;
        if (::ConvertStringSidToSidW(the_payload.container_sid, &sid)) container_sid = sid;
    }
    container_objects = the_payload.container_objects;
    object_scope = the_payload.object_scope;
}

template <class T>
void attach(T& real, HMODULE module, char const* name, T hook)
{
    real = reinterpret_cast<T>(::GetProcAddress(module, name));
    if (real) ::DetourAttach(&reinterpret_cast<PVOID&>(real), reinterpret_cast<PVOID>(hook));
}

void install()
{
    HMODULE const ntdll = ::GetModuleHandleW(L"ntdll.dll");
    HMODULE const kernelbase = ::GetModuleHandleW(L"kernelbase.dll");
    real_nt_set_information_process = reinterpret_cast<nt_set_information_process_t>(::GetProcAddress(ntdll, "NtSetInformationProcess"));
    ::DetourTransactionBegin();
    ::DetourUpdateThread(::GetCurrentThread());
    attach(real_nt_create_file, ntdll, "NtCreateFile", &hook_nt_create_file);
    attach(real_nt_open_file, ntdll, "NtOpenFile", &hook_nt_open_file);
    nt_query_information_file = reinterpret_cast<nt_query_information_file_t>(::GetProcAddress(ntdll, "NtQueryInformationFile"));
    attach(real_nt_query_attributes_file, ntdll, "NtQueryAttributesFile", &hook_nt_query_attributes_file);
    attach(real_nt_query_full_attributes_file, ntdll, "NtQueryFullAttributesFile", &hook_nt_query_full_attributes_file);
    attach(real_nt_create_named_pipe_file, ntdll, "NtCreateNamedPipeFile", &hook_nt_create_named_pipe_file);
    attach(real_nt_create_directory_object, ntdll, "NtCreateDirectoryObject", &hook_nt_create_directory_object);
    attach(real_nt_create_directory_object_ex, ntdll, "NtCreateDirectoryObjectEx", &hook_nt_create_directory_object_ex);
    attach(real_nt_open_directory_object, ntdll, "NtOpenDirectoryObject", &hook_nt_open_directory_object);
    attach(real_nt_create_event, ntdll, "NtCreateEvent", &hook_nt_create_event);
    attach(real_nt_create_mutant, ntdll, "NtCreateMutant", &hook_nt_create_mutant);
    attach(real_nt_create_semaphore, ntdll, "NtCreateSemaphore", &hook_nt_create_semaphore);
    attach(real_nt_open_semaphore, ntdll, "NtOpenSemaphore", &hook_nt_open_semaphore);
    attach(real_nt_create_section, ntdll, "NtCreateSection", &hook_nt_create_section);
    attach(real_nt_create_symbolic_link_object, ntdll, "NtCreateSymbolicLinkObject", &hook_nt_create_symbolic_link_object);
    attach(real_nt_set_information_token, ntdll, "NtSetInformationToken", &hook_nt_set_information_token);
    attach(real_nt_set_security_object, ntdll, "NtSetSecurityObject", &hook_nt_set_security_object);
    nt_alpc_connect_port_ex = reinterpret_cast<nt_alpc_connect_port_ex_t>(::GetProcAddress(ntdll, "NtAlpcConnectPortEx"));
    attach(real_nt_alpc_connect_port, ntdll, "NtAlpcConnectPort", &hook_nt_alpc_connect_port);
    attach(real_query_dos_device_w, kernelbase, "QueryDosDeviceW", &hook_query_dos_device_w);
    attach(real_create_process_w, kernelbase, "CreateProcessW", &hook_create_process_w);
    attach(real_create_process_a, kernelbase, "CreateProcessA", &hook_create_process_a);
    attach(real_get_final_path_name_by_handle_w, kernelbase, "GetFinalPathNameByHandleW", &hook_get_final_path_name_by_handle_w);
    attach(real_get_long_path_name_w, kernelbase, "GetLongPathNameW", &hook_get_long_path_name_w);
    attach(real_get_long_path_name_w_kernel32, ::GetModuleHandleW(L"kernel32.dll"), "GetLongPathNameW", &hook_get_long_path_name_w_kernel32);
    attach(real_find_first_file_ex_w, kernelbase, "FindFirstFileExW", &hook_find_first_file_ex_w);
    attach(real_find_next_file_w, kernelbase, "FindNextFileW", &hook_find_next_file_w);
    attach(real_find_close, kernelbase, "FindClose", &hook_find_close);
    ::DetourTransactionCommit();
}

// Outside a container: the device map of the children and the paths through it (hooks 14 and 15), nothing else.
void install_drives_only()
{
    HMODULE const ntdll = ::GetModuleHandleW(L"ntdll.dll");
    HMODULE const kernelbase = ::GetModuleHandleW(L"kernelbase.dll");
    real_nt_set_information_process = reinterpret_cast<nt_set_information_process_t>(::GetProcAddress(ntdll, "NtSetInformationProcess"));
    ::DetourTransactionBegin();
    ::DetourUpdateThread(::GetCurrentThread());
    attach(real_create_process_w, kernelbase, "CreateProcessW", &hook_create_process_w);
    attach(real_create_process_a, kernelbase, "CreateProcessA", &hook_create_process_a);
    attach(real_get_final_path_name_by_handle_w, kernelbase, "GetFinalPathNameByHandleW", &hook_get_final_path_name_by_handle_w);
    ::DetourTransactionCommit();
}

}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (::DetourIsHelperProcess()) return TRUE;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    ::DetourRestoreAfterWith();
    ::GetModuleFileNameA(self, dll_path, sizeof dll_path);
    DWORD size = 0;
    auto const found = static_cast<payload const*>(::DetourFindPayloadEx(payload_guid, &size));
    // Without its payload (a process started otherwise) the DLL does nothing.
    if (!found || size != sizeof(payload) || found->version != payload_version) return TRUE;
    the_payload = *found;
    // what CreateProcess rewrote for the container, back as the creator gave it, before the program reads it
    for (std::size_t i = 0; i < restored_count; ++i) {
        auto const& v = the_payload.variables[i];
        ::SetEnvironmentVariableW(restored_variables[i], v.present ? v.value : nullptr);
    }
    if (!the_payload.container_sid[0]) {
        install_drives_only();
        return TRUE;
    }
    find_container();
    install();
    return TRUE;
}
