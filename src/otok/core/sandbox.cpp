// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/sandbox.hpp"

#include <algorithm>
#include <iterator>
#include <string_view>

#include "otok/core/fs.hpp"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <detours/detours.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

#include "otok/core/build_info.hpp"
#include "otok/core/sandbox_windows.hpp"
#include "otok/core/virtual_drives.hpp"
#endif

namespace otok {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32

std::string windows_error(DWORD code)
{
    return std::system_category().message(static_cast<int>(code));
}

std::wstring sid_text(PSID sid)
{
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(sid, &text)) return {};
    std::wstring out = text;
    ::LocalFree(text);
    return out;
}

struct local_sid
{
    PSID sid = nullptr;
    ~local_sid() { if (sid) ::LocalFree(sid); }
};

// The SID of a container's text.
bool parse_sid(std::string const& text, local_sid& out)
{
    return ::ConvertStringSidToSidW(path_from_utf8(text).c_str(), &out.sid) != FALSE;
}

// Whether the DACL of `path` has an entry allowing `sid` at least `access`, inherited by what is made in it.
bool grants(PACL dacl, PSID sid, DWORD access)
{
    if (!dacl) return false;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!::GetAce(dacl, i, &ace)) continue;
        auto const header = static_cast<ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto const allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (::EqualSid(reinterpret_cast<PSID>(&allowed->SidStart), sid) && (allowed->Mask & access) == access) return true;
    }
    return false;
}

// The access entries written: generic rights become specific ones in a file system ACL.
constexpr DWORD file_all_access = FILE_ALL_ACCESS;
constexpr DWORD file_read_execute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;

result<void> grant(PSID sid, fs::path const& path, DWORD access, bool required)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD r = ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    if (r != ERROR_SUCCESS) {
        if (!required) return {};
        return fail("cannot read the ACL of '" + path_to_utf8(path) + "': " + windows_error(r));
    }
    struct sd_guard
    {
        PSECURITY_DESCRIPTOR sd;
        ~sd_guard() { ::LocalFree(sd); }
    } guard{sd};
    if (grants(dacl, sid, access)) return {};
    std::error_code ec;
    bool const directory = fs::is_directory(path, ec);
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = access;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL updated = nullptr;
    r = ::SetEntriesInAclW(1, &entry, dacl, &updated);
    if (r == ERROR_SUCCESS) {
        // carried to what is in it already, which SetNamedSecurityInfoW does for an inheritable entry
        r = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
            updated, nullptr);
        ::LocalFree(updated);
    }
    if (r != ERROR_SUCCESS && required) {
        return fail("cannot let the sandbox reach '" + path_to_utf8(path) + "': " + windows_error(r));
    }
    return {};
}

// The ANSI path Detours writes into a child's import table: the long path when the code page holds it, else the
// short (8.3) one; empty when neither does.
std::string ansi_path(fs::path const& path)
{
    auto const convert = [](std::wstring const& wide) -> std::string {
        BOOL lossy = FALSE;
        int const n = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, &lossy);
        if (n <= 0 || lossy) return {};
        std::string out(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, &lossy);
        return lossy ? std::string{} : out;
    };
    if (auto long_path = convert(path.wstring()); !long_path.empty()) return long_path;
    wchar_t short_path[MAX_PATH];
    DWORD const n = ::GetShortPathNameW(path.c_str(), short_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return convert(std::wstring(short_path, n));
}

// The DLL as the children's import tables can name it (ansi_path): the one beside otok, or, when neither its long nor its
// short path can be written in the system's code page (a user name in another script), a copy of it in
// %ProgramData%\otok\<version>\, where every user may make directories.
result<fs::path> loadable_dll(fs::path const& beside)
{
    if (!ansi_path(beside).empty()) return beside;
    wchar_t data[MAX_PATH];
    DWORD const n = ::GetEnvironmentVariableW(L"ProgramData", data, MAX_PATH);
    fs::path const dir = (n && n < MAX_PATH ? fs::path{std::wstring(data, n)} : fs::path{L"C:\\ProgramData"}) / "otok"
        / path_from_utf8(std::string{build_info::version});
    fs::path const copy = dir / sandbox_windows::dll_name;
    std::string const why = "the path of '" + path_to_utf8(beside) + "' cannot be written in the system's code page, and ";
    if (ansi_path(dir).empty()) return fail(why + "neither can '" + path_to_utf8(dir) + "'");
    auto const wanted = read_file(beside);
    if (!wanted) return fail(why + "it cannot be read: " + wanted.error().message);
    if (auto const there = read_file(copy); !there || *there != *wanted) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (auto ok = write_file_atomic(copy, *wanted); !ok) return fail(why + "it cannot be copied to '" + path_to_utf8(dir) + "': " + ok.error().message);
    }
    return copy;
}

std::wstring current_user_sid()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::wstring out;
    alignas(TOKEN_USER) char buffer[256];
    DWORD size = sizeof buffer;
    if (::GetTokenInformation(token, TokenUser, buffer, size, &size)) out = sid_text(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid);
    ::CloseHandle(token);
    return out;
}

void copy_text(wchar_t* to, std::size_t size, std::wstring const& text)
{
    ::wcsncpy_s(to, size, text.c_str(), _TRUNCATE);
}

// Duplicates a handle of this process into `child`: its value there, 0 for none.
unsigned long long duplicate_into(HANDLE child, HANDLE handle)
{
    HANDLE copy = nullptr;
    if (!handle || handle == INVALID_HANDLE_VALUE
        || !::DuplicateHandle(::GetCurrentProcess(), handle, child, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return 0;
    return reinterpret_cast<ULONG_PTR>(copy);
}

// The broker of the device map (core/virtual_drives) for the processes of an AppContainer, which may not set a map on
// their children: a named pipe of this process that the container may write to. A client sends the value of a handle
// to its suspended child in its own table; the broker duplicates it out of the client, whose id the pipe tells, sets the
// map and answers the NTSTATUS. Only a handle the client holds can be named, and the child does not run meanwhile, so
// no process id is taken for another. Started once, for the first container; the pipe's name, empty when it fails.
std::wstring map_broker(PSID container)
{
    static std::once_flag once;
    static std::wstring name;
    std::call_once(once, [&] {
        std::wstring const pipe = L"\\\\.\\pipe\\otok-map-" + std::to_wstring(::GetCurrentProcessId());
        std::wstring const sddl = L"D:(A;;GA;;;" + current_user_sid() + L")(A;;GRGW;;;" + sid_text(container) + L")";
        auto const serve = [pipe, sddl] {
            SECURITY_ATTRIBUTES attributes{sizeof attributes, nullptr, FALSE};
            if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &attributes.lpSecurityDescriptor, nullptr))
                return;
            HANDLE const p = ::CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 64, 64, 0, &attributes);
            ::LocalFree(attributes.lpSecurityDescriptor);
            if (p == INVALID_HANDLE_VALUE) return;
            for (;;) {
                if (!::ConnectNamedPipe(p, nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED) continue;
                std::uint64_t request = 0;
                DWORD got = 0;
                LONG reply = static_cast<LONG>(0xC000000DL);  // STATUS_INVALID_PARAMETER
                ULONG client = 0;
                if (::ReadFile(p, &request, sizeof request, &got, nullptr) && got == sizeof request && ::GetNamedPipeClientProcessId(p, &client)) {
                    reply = static_cast<LONG>(0xC0000022L);  // STATUS_ACCESS_DENIED
                    if (HANDLE const parent = ::OpenProcess(PROCESS_DUP_HANDLE, FALSE, client)) {
                        HANDLE child = nullptr;
                        if (::DuplicateHandle(parent, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(request)), ::GetCurrentProcess(), &child,
                                PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
                            reply = virtual_drives::give(child) ? 0 : static_cast<LONG>(0xC0000001L);
                            ::CloseHandle(child);
                        }
                        ::CloseHandle(parent);
                    }
                }
                DWORD written = 0;
                ::WriteFile(p, &reply, sizeof reply, &written, nullptr);
                ::FlushFileBuffers(p);
                ::DisconnectNamedPipe(p);
            }
        };
        // a few instances: processes start at once in many slices
        for (int i = 0; i < 4; ++i) std::thread{serve}.detach();
        name = pipe;
    });
    return name;
}

// The payload of otok-sandbox.dll for a new process: what the container may not open, opened here and duplicated
// into it (sandbox_windows.hpp); no container (`container` null): the device map alone.
result<void> give_payload(sandbox_launch const& launch, PSID container, HANDLE child, wchar_t const* environment)
{
    static sandbox_windows::payload p;  // large
    static std::mutex mutex;
    std::lock_guard lock{mutex};
    p = {};
    p.version = sandbox_windows::payload_version;
    auto const opened = [](wchar_t const* name, DWORD access, DWORD flags) {
        return ::CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
    };
    auto const keep = [&](HANDLE h) {
        unsigned long long const in_child = duplicate_into(child, h);
        if (h != INVALID_HANDLE_VALUE && h) ::CloseHandle(h);
        return in_child;
    };
    p.null_device = keep(opened(L"NUL", GENERIC_READ | GENERIC_WRITE, 0));
    p.mountmgr = keep(opened(L"\\\\.\\MountPointManager", sandbox_windows::mountmgr_access, 0));
    DWORD const drives = ::GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        wchar_t const letter[3] = {static_cast<wchar_t>(L'A' + d), L':', 0};
        if (!::QueryDosDeviceW(letter, p.dos_devices[d], static_cast<DWORD>(std::size(p.dos_devices[d])))) {
            p.dos_devices[d][0] = 0;
            continue;
        }
        wchar_t const root[4] = {static_cast<wchar_t>(L'A' + d), L':', L'\\', 0};
        p.roots[d] = keep(opened(root, sandbox_windows::root_access, FILE_FLAG_BACKUP_SEMANTICS));
    }
    copy_text(p.user_sid, std::size(p.user_sid), current_user_sid());
    if (container) {
        std::wstring const container_text = sid_text(container);
        copy_text(p.container_sid, std::size(p.container_sid), container_text);
        DWORD session = 0;
        ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
        copy_text(p.container_objects, std::size(p.container_objects),
            L"\\Sessions\\" + std::to_wstring(session) + L"\\AppContainerNamedObjects\\" + container_text);
    }
    if (!launch.log.empty()) copy_text(p.log, std::size(p.log), launch.log.wstring());
    sandbox_windows::take_variables(p, environment);
    // the ancestors of the work directory, from the drive root down to its parent, to pass through and nothing more
    if (container && !launch.work.empty()) {
        std::error_code ec;
        // on a virtual drive as it is: resolved, it would be the long real path, whose ancestors the build never walks
        bool const virtual_work = virtual_drives::to_real(launch.work) != launch.work;
        fs::path const work = virtual_work ? launch.work.lexically_normal() : fs::weakly_canonical(launch.work, ec);
        std::vector<fs::path> ancestors;
        for (fs::path a = work.parent_path(); !a.empty(); a = a.parent_path()) {
            ancestors.push_back(a);
            if (a == a.root_path()) break;
        }
        for (fs::path const& a : ancestors) {
            if (p.ancestor_count == sandbox_windows::max_ancestors) break;
            std::wstring name = L"\\??\\" + a.wstring();
            if (name.size() >= std::size(p.ancestors[0].name)) continue;
            auto& entry = p.ancestors[p.ancestor_count];
            entry.handle = keep(opened(a.c_str(), sandbox_windows::ancestor_access, FILE_FLAG_BACKUP_SEMANTICS));
            if (!entry.handle) continue;
            copy_text(entry.name, std::size(entry.name), name);
            ++p.ancestor_count;
        }
    }
    // the device map for the children (core/virtual_drives): the directory outside a container, the broker in one
    if (virtual_drives::enabled()) {
        if (container) copy_text(p.map_pipe, std::size(p.map_pipe), map_broker(container));
        else p.map_directory = duplicate_into(child, virtual_drives::directory());
        for (virtual_drives::drive const& d : virtual_drives::drives()) {
            if (p.drive_count == sandbox_windows::max_drives) break;
            auto& entry = p.drives[p.drive_count++];
            entry.letter = d.letter;
            copy_text(entry.real, std::size(entry.real), d.real.wstring());
            // QueryDosDeviceW of the letter as of a subst drive: the AppContainer may not read the link
            copy_text(p.dos_devices[d.letter - L'A'], std::size(p.dos_devices[0]), d.device);
            // its root as the roots of the other drives (hook 7): the container may not open the directory, and a program
            // asking the volume's attributes there (GetVolumeInformationW: perlglob lowercases every name it finds on a
            // volume that does not preserve case) would get nothing
            if (container) {
                wchar_t const root[4] = {d.letter, L':', L'\\', 0};
                p.roots[d.letter - L'A'] = keep(opened(root, sandbox_windows::root_access, FILE_FLAG_BACKUP_SEMANTICS));
            }
        }
    }
    if (!::DetourCopyPayloadToProcess(child, sandbox_windows::payload_guid, &p, sizeof p))
        return fail("cannot give the sandbox's payload to the process: " + windows_error(::GetLastError()));
    return {};
}

#endif

// An SBPL string literal.
std::string sbpl_string(fs::path const& p)
{
    std::string out = "\"";
    for (char c : path_to_utf8(p.generic_u8string())) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string filters(std::string_view kind, std::vector<fs::path> const& paths)
{
    std::string out;
    for (fs::path const& p : paths) out += "\n    (" + std::string{kind} + " " + sbpl_string(p) + ")";
    return out;
}

bool inside(fs::path const& p, fs::path const& dir)
{
    auto const [d, q] = std::mismatch(dir.begin(), dir.end(), p.begin(), p.end());
    return d == dir.end();
}

}

host_sandbox find_host_sandbox([[maybe_unused]] env_vars const& env)
{
#if defined(__APPLE__)
    return {host_sandbox::mechanism::seatbelt, "/usr/bin/sandbox-exec", {}, {}};
#elif defined(__linux__)
    auto bwrap = find_program("bwrap", env, "/");
    if (!bwrap) return {host_sandbox::mechanism::none, {}, "bwrap (bubblewrap) was not found in PATH", {}};
    // the namespaces of a build, with nothing else
    process_spec probe;
    probe.argv = {path_to_utf8(*bwrap)};
    std::ranges::copy(bubblewrap_args({}), std::back_inserter(probe.argv));
    probe.argv.push_back("/bin/true");
    probe.env = env;
    probe.cwd = "/";
    auto made = run_process(probe);
    if (!made || !made->success()) {
        std::string const why = made ? describe_exit(*made) : made.error().message;
        return {host_sandbox::mechanism::none, {}, "'" + path_to_utf8(*bwrap) + "' cannot make a sandbox here (" + why + ")", {}};
    }
    return {host_sandbox::mechanism::bubblewrap, *bwrap, {}, {}};
#elif defined(_WIN32)
    (void)env;
    fs::path const beside = current_executable().parent_path() / sandbox_windows::dll_name;
    std::error_code ec;
    if (!fs::is_regular_file(beside, ec)) return {host_sandbox::mechanism::none, {}, "'" + path_to_utf8(beside) + "' was not found", {}};
    auto const dll = loadable_dll(beside);
    if (!dll) return {host_sandbox::mechanism::none, {}, dll.error().message, {}};
    PSID sid = nullptr;
    HRESULT hr = ::CreateAppContainerProfile(sandbox_windows::container_name, L"otok builds", L"The sandbox of otok's builds", nullptr, 0, &sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) hr = ::DeriveAppContainerSidFromAppContainerName(sandbox_windows::container_name, &sid);
    if (FAILED(hr)) return {host_sandbox::mechanism::none, {}, "no AppContainer can be made: " + windows_error(static_cast<DWORD>(hr)), {}};
    std::string const container = path_to_utf8(fs::path{sid_text(sid)});
    // the DLL loads in every process of the container
    auto const granted = grant(sid, *dll, file_read_execute, true);
    ::FreeSid(sid);
    if (!granted) return {host_sandbox::mechanism::none, {}, granted.error().message, {}};
    return {host_sandbox::mechanism::appcontainer, *dll, {}, container};
#else
    (void)env;
    return {};
#endif
}

result<void> grant_sandbox_access([[maybe_unused]] host_sandbox const& host, [[maybe_unused]] sandbox_spec const& spec)
{
#ifdef _WIN32
    if (host.kind != host_sandbox::mechanism::appcontainer) return {};
    local_sid sid;
    if (!parse_sid(host.container, sid)) return fail("the AppContainer's SID '" + host.container + "' cannot be read");
    // a directory is walked once per otok process at most (the cache, by every slice); one grant at a time: an inheritable
    // entry on the cache root rewrites the ACL of everything below it from what it read, and an entry another slice added
    // to its work directory meanwhile was lost (the work directory then only readable, docs/PLAN.md decision 103)
    static std::mutex mutex;
    static std::set<std::pair<fs::path, DWORD>> done;
    std::lock_guard lock{mutex};
    auto const once = [&](fs::path const& path, DWORD access, bool required) -> result<void> {
        // remembered by the real directory: a virtual drive's letter is another directory once it is given again
        fs::path const real = virtual_drives::to_real(path);
        if (done.contains({real, access})) return {};
        if (auto ok = grant(sid.sid, path, access, required); !ok) return ok;
        done.insert({real, access});
        return {};
    };
    for (fs::path const& w : spec.writable) {
        if (auto ok = once(w, file_all_access, true); !ok) return ok;
    }
    for (fs::path const& f : spec.writable_files) {
        if (auto ok = once(f, file_all_access, true); !ok) return ok;
    }
    for (fs::path const& r : spec.readable) {
        if (auto ok = once(r, file_read_execute, false); !ok) return ok;
    }
#endif
    return {};
}

std::string seatbelt_profile(sandbox_spec const& spec)
{
    // later rules win: each exception follows its denial
    std::string text = "; Generated by otok: the sandbox of a build (docs/PLAN.md decision 28).\n(version 1)\n(allow default)\n"
                       "(deny network*)\n(allow network* (remote unix-socket))\n(deny file-write*)\n"
                       "(allow file-write*\n    (subpath \"/dev\")"
                       + filters("subpath", spec.writable) + filters("literal", spec.writable_files) + ")\n";
    if (!spec.denied_reads.empty()) {
        text += "(deny file-read-data" + filters("subpath", spec.denied_reads) + ")\n";
        if (!spec.readable.empty() || !spec.writable.empty() || !spec.writable_files.empty()) {
            text += "(allow file-read-data" + filters("subpath", spec.readable) + filters("subpath", spec.writable)
                + filters("literal", spec.writable_files) + ")\n";
        }
    }
    return text;
}

std::vector<std::string> bubblewrap_args(sandbox_spec const& spec)
{
    std::vector<std::string> args{"--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc"};
    auto const add = [&](std::string_view option, fs::path const& p) {
        std::string const path = path_to_utf8(p.generic_u8string());
        args.insert(args.end(), {std::string{option}, path, path});
    };
    // later mounts cover earlier ones: the denied directories emptied, what is readable inside them, then what is
    // writable, which may lie inside what is readable (the work directory in the cache); a path that is not there
    // (a recipe directory removed meanwhile) is no error
    for (fs::path const& d : spec.denied_reads) args.insert(args.end(), {"--tmpfs", path_to_utf8(d.generic_u8string())});
    for (fs::path const& r : spec.readable) {
        if (std::ranges::any_of(spec.denied_reads, [&](fs::path const& d) { return inside(r, d); })) add("--ro-bind-try", r);
    }
    for (fs::path const& w : spec.writable) add("--bind", w);
    for (fs::path const& f : spec.writable_files) add("--bind", f);
    // read-only once what lies inside is mounted, which needed directories made in them
    for (fs::path const& d : spec.denied_reads) args.insert(args.end(), {"--remount-ro", path_to_utf8(d.generic_u8string())});
    args.insert(args.end(), {"--unshare-net", "--unshare-pid", "--die-with-parent", "--"});
    return args;
}

std::vector<std::string> sandbox_command(host_sandbox const& host, sandbox_spec const& spec, fs::path const& profile)
{
    if (host.kind == host_sandbox::mechanism::appcontainer || host.kind == host_sandbox::mechanism::none) return {};
    std::vector<std::string> command{path_to_utf8(host.program)};
    if (host.kind == host_sandbox::mechanism::seatbelt) {
        command.insert(command.end(), {"-f", path_to_utf8(profile)});
    } else if (host.kind == host_sandbox::mechanism::bubblewrap) {
        std::ranges::copy(bubblewrap_args(spec), std::back_inserter(command));
    }
    return command;
}

std::optional<double> kernel_worker_seconds()
{
#ifdef _WIN32
    // NtQuerySystemInformation(SystemProcessInformation): the entries of every process, readable without
    // administrator rights; the times of an entry at their documented offsets (winternl.h hides them)
    using query_t = LONG(NTAPI*)(int, void*, ULONG, ULONG*);
    static auto const query = reinterpret_cast<query_t>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return std::nullopt;
    constexpr int system_process_information = 5;
    constexpr LONG info_length_mismatch = static_cast<LONG>(0xC0000004L);
    std::vector<std::byte> buffer(1 << 20);
    ULONG needed = 0;
    LONG status;
    while ((status = query(system_process_information, buffer.data(), static_cast<ULONG>(buffer.size()), &needed)) == info_length_mismatch)
        buffer.resize(needed + (64 << 10));
    if (status < 0) return std::nullopt;
    for (std::size_t offset = 0;;) {
        std::byte const* entry = buffer.data() + offset;
        ULONG next = 0;
        std::uint64_t user = 0, kernel = 0;
        ULONG_PTR pid = 0;
        std::memcpy(&next, entry, sizeof next);
        std::memcpy(&user, entry + 0x28, sizeof user);
        std::memcpy(&kernel, entry + 0x30, sizeof kernel);
        std::memcpy(&pid, entry + 0x50, sizeof pid);
        if (pid == 4) return static_cast<double>(user + kernel) / 1e7;
        if (!next) return std::nullopt;
        offset += next;
    }
#else
    return std::nullopt;
#endif
}

result<double> sandbox_exit_cost([[maybe_unused]] host_sandbox const& host, [[maybe_unused]] int count)
{
#ifdef _WIN32
    if (host.kind != host_sandbox::mechanism::appcontainer || count <= 0) return 0.0;
    local_sid sid;
    if (!parse_sid(host.container, sid)) return fail("the AppContainer's SID '" + host.container + "' cannot be read");
    wchar_t system[MAX_PATH];
    UINT const length = ::GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return fail("cannot find the system directory: " + windows_error(::GetLastError()));
    fs::path const cmd = fs::path{system} / L"cmd.exe";
    // CreateProcess wants LOCALAPPDATA for a container (it puts the container's own there)
    wchar_t windows[MAX_PATH];
    UINT const windows_length = ::GetWindowsDirectoryW(windows, MAX_PATH);
    std::wstring environment = L"SystemRoot=" + std::wstring{windows, windows_length < MAX_PATH ? windows_length : 0} + L'\0'
        + L"LOCALAPPDATA=" + system + L'\0' + L'\0';
    auto const run = [&](bool contained, int processes) -> result<void> {
        SECURITY_CAPABILITIES capabilities{sid.sid, nullptr, 0, 0};
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<std::byte> attributes(size);
        auto const list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!::InitializeProcThreadAttributeList(list, 1, 0, &size)) return fail("cannot start the check: " + windows_error(::GetLastError()));
        struct list_guard
        {
            LPPROC_THREAD_ATTRIBUTE_LIST list;
            ~list_guard() { ::DeleteProcThreadAttributeList(list); }
        } guard{list};
        if (contained
            && !::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof capabilities, nullptr, nullptr))
            return fail("cannot start the check: " + windows_error(::GetLastError()));
        for (int i = 0; i < processes; ++i) {
            std::wstring line = L"\"" + cmd.wstring() + L"\" /d /c exit 0";
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof startup;
            startup.lpAttributeList = list;
            PROCESS_INFORMATION information{};
            if (!::CreateProcessW(cmd.c_str(), line.data(), nullptr, nullptr, FALSE,
                    EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT, environment.data(), system,
                    &startup.StartupInfo, &information))
                return fail("cannot start '" + path_to_utf8(cmd) + "'" + (contained ? " in the AppContainer" : "") + ": "
                    + windows_error(::GetLastError()));
            ::WaitForSingleObject(information.hProcess, INFINITE);
            ::CloseHandle(information.hThread);
            ::CloseHandle(information.hProcess);
        }
        // what the kernel does after the exits
        ::Sleep(100);
        return {};
    };
    // in rounds, the quietest of each kind counted: what else the kernel does meanwhile (the cache manager writing,
    // another build) only adds to a round
    constexpr int rounds = 5;
    int const per_round = std::max(1, count / rounds);
    double ordinary = std::numeric_limits<double>::max();
    double contained = std::numeric_limits<double>::max();
    for (int round = 0; round < rounds; ++round) {
        for (bool const in_container : {false, true}) {
            auto const before = kernel_worker_seconds();
            if (!before) return 0.0;
            if (auto ok = run(in_container, per_round); !ok) return std::unexpected{ok.error()};
            auto const after = kernel_worker_seconds();
            if (!after) return 0.0;
            double& least = in_container ? contained : ordinary;
            least = std::min(least, *after - *before);
        }
    }
    return std::max(0.0, contained - ordinary) / per_round;
#else
    return 0.0;
#endif
}

result<void> detail::create_sandboxed_process([[maybe_unused]] sandbox_launch const& launch, [[maybe_unused]] fs::path const& program,
    [[maybe_unused]] std::wstring& command_line, [[maybe_unused]] std::wstring& environment, [[maybe_unused]] fs::path const& cwd,
    [[maybe_unused]] std::vector<void*> const& inherited, [[maybe_unused]] void* startup, [[maybe_unused]] void* information)
{
#ifdef _WIN32
    // no container: otok-sandbox.dll alone, for the device map of the children (core/virtual_drives)
    bool const contained = !launch.container.empty();
    local_sid sid;
    if (contained && !parse_sid(launch.container, sid)) return fail("the AppContainer's SID '" + launch.container + "' cannot be read");
    std::string const dll = ansi_path(launch.dll);
    if (dll.empty()) return fail("the path of '" + path_to_utf8(launch.dll) + "' cannot be written in the system's code page");

    // the handles it inherits, the container without capabilities (no network), and no high-entropy ASLR: the
    // DYNAMIC_BASE images of MSYS2 land at one address in every process, which Cygwin's fork needs (the other ASLR
    // policies cannot be turned off in an AppContainer)
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = sid.sid;
    DWORD64 mitigation = PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_OFF;
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 3, 0, &size);
    std::vector<std::byte> storage(size);
    auto const list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!::InitializeProcThreadAttributeList(list, 3, 0, &size)) return fail("cannot prepare the process attributes: " + windows_error(::GetLastError()));
    struct list_guard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~list_guard() { ::DeleteProcThreadAttributeList(list); }
    } guard{list};
    std::vector<void*> handles = inherited;
    if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), handles.size() * sizeof(HANDLE), nullptr, nullptr)
        || (contained && !::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof capabilities, nullptr, nullptr))
        || (contained && !::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigation, sizeof mitigation, nullptr, nullptr))) {
        return fail("cannot prepare the process attributes: " + windows_error(::GetLastError()));
    }
    // CreateProcess refuses a container process without LOCALAPPDATA and rewrites it, TEMP and TMP; the DLL sets back
    // what the environment gives (the payload), removing what it does not
    std::wstring const intended = environment;
    std::wstring given = contained ? sandbox_windows::with_variable(environment.c_str(), L"LOCALAPPDATA", cwd.wstring()) : environment;
    STARTUPINFOEXW extended{};
    extended.StartupInfo = *static_cast<STARTUPINFOW*>(startup);
    extended.StartupInfo.cb = sizeof extended;
    extended.lpAttributeList = list;
    auto const pi = static_cast<PROCESS_INFORMATION*>(information);
    if (!::DetourCreateProcessWithDllExW(program.c_str(), command_line.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, given.data(), cwd.c_str(),
            &extended.StartupInfo, pi, dll.c_str(), nullptr)) {
        return fail("cannot start '" + path_to_utf8(program) + "' in the sandbox: " + windows_error(::GetLastError()));
    }
    if (auto ok = give_payload(launch, sid.sid, pi->hProcess, intended.c_str()); !ok) {
        ::TerminateProcess(pi->hProcess, 1);
        ::CloseHandle(pi->hThread);
        ::CloseHandle(pi->hProcess);
        return ok;
    }
    return {};
#else
    return fail("an AppContainer exists on Windows only");
#endif
}

}
