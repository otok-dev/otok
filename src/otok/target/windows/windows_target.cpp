// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/target/windows/windows_target.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#   include <windows.h>
#endif

#include "otok/core/binary.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"

namespace otok::target::windows {

namespace fs = std::filesystem;

namespace {

#if defined(_M_ARM64) || defined(__aarch64__)
constexpr char const* machine_arch = "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
constexpr char const* machine_arch = "x86_64";
#else
#   error "unknown architecture of the build machine"
#endif

#ifdef _WIN32
constexpr char const* exe = ".exe";
constexpr char path_separator = ';';
#else
constexpr char const* exe = "";
constexpr char path_separator = ':';
#endif

std::string slashed(fs::path const& p)
{
    return path_to_utf8(p.generic_u8string());
}

// Compares two toolsets of MSVC, "14.44" and "14.51", by their numbers.
int compare_toolsets(std::string_view a, std::string_view b)
{
    auto const numbers = [](std::string_view s) {
        std::vector<int> out;
        int n = 0;
        for (char c : s) {
            if (c == '.') {
                out.push_back(n);
                n = 0;
            } else if (c >= '0' && c <= '9') {
                n = n * 10 + (c - '0');
            }
        }
        out.push_back(n);
        return out;
    };
    auto const x = numbers(a), y = numbers(b);
    return x < y ? -1 : y < x ? 1 : 0;
}

#ifdef _WIN32
// A value of Visual Studio's environment or of vswhere without its line end and trailing backslash.
std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\\')) s.pop_back();
    return s;
}
#endif

// A variable of an environment, whatever its case on Windows.
std::optional<std::string> variable(env_vars const& env, std::string_view name)
{
    for (auto const& [key, value] : env) {
        bool const same = key.size() == name.size() && std::ranges::equal(key, name, [](char a, char b) {
#ifdef _WIN32
            return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
#else
            return a == b;
#endif
        });
        if (same && !value.empty()) return value;
    }
    return std::nullopt;
}

bool is_path_name(std::string_view name)
{
    return name.size() == 4 && std::ranges::equal(name, std::string_view{"PATH"}, [](char a, char b) {
        return std::toupper(static_cast<unsigned char>(a)) == b;
    });
}

result<std::string> output_of(std::vector<std::string> argv, env_vars const& env)
{
    std::string const program = argv.front();
    std::error_code ec;
    auto exit = run_process({std::move(argv), env, fs::current_path(ec), {}, true});
    if (!exit) return std::unexpected{exit.error()};
    if (!exit->success()) return fail("'" + program + "' failed with " + describe_exit(*exit));
    return std::move(exit->output);
}

std::string joined_flags(std::vector<std::string> const& flags)
{
    std::string text;
    for (std::string const& flag : flags) text += (text.empty() ? "" : " ") + flag;
    return text;
}

// The clang-cl a link `program` leads to in LLVM's own directory, which has the other tools under their plain names:
// links are followed while they name a clang-cl, not to clang itself, which takes its mode from the name it is run by.
fs::path in_llvm_directory(fs::path program)
{
    std::error_code ec;
    for (int i = 0; i < 16 && fs::is_symlink(program, ec); ++i) {
        fs::path next = fs::read_symlink(program, ec);
        if (ec) break;
        if (next.is_relative()) next = program.parent_path() / next;
        if (!path_to_utf8(next.filename()).starts_with("clang-cl")) break;
        program = next;
    }
    fs::path const dir = fs::canonical(program.parent_path(), ec);
    return ec ? program : dir / program.filename();
}

// clang-cl of the LLVM directory `llvm` (config.md), else of the PATH, or the newest clang-cl-<major> there (Debian and Ubuntu
// install them so), in LLVM's directory.
result<fs::path> find_clang_cl(std::string const& llvm, env_vars const& env)
{
    std::error_code ec;
    if (!llvm.empty()) {
        fs::path const program = path_from_utf8(llvm) / "bin" / ("clang-cl" + std::string{exe});
        if (!fs::is_regular_file(program, ec)) return fail("'llvm' names '" + llvm + "', which has no bin/clang-cl" + std::string{exe} + " (an LLVM installation)");
        return in_llvm_directory(program);
    }
    if (auto plain = find_program("clang-cl", env, fs::current_path(ec))) return in_llvm_directory(*plain);
    std::optional<fs::path> best;
    int best_major = -1;
    std::string const path = variable(env, "PATH").value_or("");
    for (std::size_t at = 0; at <= path.size();) {
        std::size_t const end = std::min(path.find(path_separator, at), path.size());
        fs::path const dir = path_from_utf8(path.substr(at, end - at));
        at = end + 1;
        for (auto it = fs::directory_iterator{dir, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
            std::string name = path_to_utf8(it->path().filename());
            if (!std::string_view{exe}.empty() && name.ends_with(exe)) name.resize(name.size() - std::string_view{exe}.size());
            if (!name.starts_with("clang-cl-") || name.size() == 9) continue;
            int major = 0;
            for (char c : name.substr(9)) major = std::isdigit(static_cast<unsigned char>(c)) && major < 10000 ? major * 10 + (c - '0') : -1;
            if (major > best_major) {
                best_major = major;
                best = it->path();
            }
        }
        ec.clear();
    }
    if (!best) return fail("the Windows target needs clang-cl (LLVM) in PATH, as clang-cl or clang-cl-<version>, or set 'llvm' in config.toml; install LLVM with its lld");
    return in_llvm_directory(*best);
}

// clang-cl and the LLVM tools beside it.
struct llvm_tools
{
    std::string bin;                    // LLVM's directory, with '/'
    std::string clang_cl, lld_link, llvm_lib, llvm_rc, llvm_mt, llvm_nm;
    std::string version;                // "21.1.8"
};

result<llvm_tools> llvm_tools_of(fs::path const& clang_cl, env_vars const& env)
{
    llvm_tools t;
    fs::path const bin = clang_cl.parent_path();
    t.bin = slashed(bin);
    t.clang_cl = slashed(clang_cl);
    std::error_code ec;
    for (auto [name, out, required] : {std::tuple{"lld-link", &t.lld_link, true}, std::tuple{"llvm-lib", &t.llvm_lib, true},
             std::tuple{"llvm-rc", &t.llvm_rc, true}, std::tuple{"llvm-mt", &t.llvm_mt, false}, std::tuple{"llvm-nm", &t.llvm_nm, false}}) {
        fs::path const tool = bin / (std::string{name} + exe);
        if (fs::is_regular_file(tool, ec)) {
            *out = slashed(tool);
        } else if (required) {
            return fail("'" + t.bin + "' has clang-cl but no " + name + ": install LLVM's " + (std::string_view{name} == "lld-link" ? "lld" : "tools"));
        }
    }
    auto version = output_of({t.clang_cl, "--version"}, env);
    if (!version) return std::unexpected{version.error()};
    t.version = detail::clang_version(*version);
    return t;
}

// What the target found for `linux/clang-cl` once: clang-cl and its tools, the CRT and the SDK of `xwin splat`, and the
// environment its builds start from.
struct xwin_tools
{
    std::string root;                   // xwin's directory, with '/'
    std::string crt, sdk;               // "14.44.35220", "10.0.26100"
    llvm_tools llvm;
    env_vars env;
    std::string description;
};

result<xwin_tools> discover_xwin(std::string const& configured, std::string const& llvm_dir, env_vars const& env)
{
    if (configured.empty()) {
        return fail("the Windows target needs the CRT and the Windows SDK: set 'windows-sdk' in config.toml (or OTOK_WINDOWS_SDK) to a "
                    "directory made by `xwin --accept-license splat --include-debug-libs --output <dir>`");
    }
    xwin_tools f;
    fs::path const root = path_from_utf8(configured);
    f.root = slashed(root);
    auto crtversion = read_file(root / "crt" / "include" / "crtversion.h");
    auto yvals = read_file(root / "crt" / "include" / "yvals_core.h");
    std::error_code ec;
    if (!crtversion || !yvals || !fs::is_directory(root / "sdk" / "include" / "ucrt", ec)) {
        return fail("'windows-sdk' names '" + configured + "', which is not a directory of `xwin splat` (it has no crt/include/crtversion.h, "
            + "crt/include/yvals_core.h or sdk/include/ucrt)");
    }
    f.crt = detail::crt_version(*crtversion);
    if (f.crt.empty()) return fail("'" + f.root + "/crt/include/crtversion.h' has no version of the CRT");
    f.sdk = detail::sdk_version(root / "sdk" / "include");
    if (f.sdk.empty()) return fail("'" + f.root + "/sdk/include' has no version of the Windows SDK (an entry 10.0.<build>)");

    auto clang_cl = find_clang_cl(llvm_dir, env);
    if (!clang_cl) return std::unexpected{clang_cl.error()};
    auto llvm = llvm_tools_of(*clang_cl, env);
    if (!llvm) return std::unexpected{llvm.error()};
    f.llvm = std::move(*llvm);
    int const major = std::atoi(f.llvm.version.c_str());
    if (int const minimum = detail::clang_minimum(*yvals); minimum > 0 && major < minimum) {
        return fail("the STL of the CRT " + f.crt + " at '" + f.root + "' needs Clang " + std::to_string(minimum) + " or newer, and '" + f.llvm.clang_cl
            + "' is " + (f.llvm.version.empty() ? std::string{"of no known version"} : f.llvm.version));
    }

    // PATH: LLVM's and the system's; the builder adds the directories of the recipe's toolchains (docs/PLAN.md decision 38)
    std::vector<std::string> dirs{f.llvm.bin};
#if defined(_WIN32)
    std::string const system_root = variable(env, "SystemRoot").value_or("C:\\Windows");
    dirs.insert(dirs.end(), {system_root + "\\System32", system_root, system_root + "\\System32\\Wbem"});
    for (char const* name : {"SystemRoot", "SystemDrive", "windir", "ComSpec", "PATHEXT", "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS"}) {
        if (auto value = variable(env, name)) f.env[name] = *value;
    }
#else
    dirs.insert(dirs.end(), {"/usr/bin", "/bin", "/usr/sbin", "/sbin"});
#endif
    std::string path;
    for (std::string const& d : dirs) path += (path.empty() ? "" : std::string{path_separator}) + d;
    f.env["PATH"] = path;
    f.description = "clang-cl " + f.llvm.version + " at '" + f.llvm.bin + "', the CRT " + f.crt + " and the Windows SDK " + f.sdk + " at '" + f.root + "'";
    return f;
}

// The environment of Visual Studio for one architecture of slices: vcvarsall.bat's, or a developer prompt's.
struct visual_studio_environment
{
    env_vars env;
    std::string crt, sdk;               // VCToolsVersion "14.51.36231", WindowsSDKVersion "10.0.26100.0"
    std::string vc_dir;                 // VCINSTALLDIR, where Visual Studio's LLVM is
    std::string description;
};

#ifdef _WIN32

std::string utf8_of_utf16(std::string const& bytes)
{
    std::wstring const wide(reinterpret_cast<wchar_t const*>(bytes.data()), bytes.size() / sizeof(wchar_t));
    if (wide.empty()) return {};
    int const size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// The Visual Studio installation (docs/PLAN.md decision 19): a path, or found by vswhere.
result<fs::path> visual_studio_installation(std::string const& wanted, env_vars const& env)
{
    if (wanted.find_first_of("/\\:") != std::string::npos) return path_from_utf8(wanted);
    fs::path const vswhere = path_from_utf8(variable(env, "ProgramFiles(x86)").value_or("C:\\Program Files (x86)"))
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    std::error_code ec;
    if (!fs::exists(vswhere, ec)) return fail("Visual Studio was not found ('" + path_to_utf8(vswhere) + "' is missing); install it with the C++ tools");
    std::vector<std::string> argv{path_to_utf8(vswhere), "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property", "installationPath", "-utf8", "-latest"};
    if (!wanted.empty()) {
        char* end = nullptr;
        long major = std::strtol(wanted.c_str(), &end, 10);
        // the product year as well as the major version: "2022" is 17
        static std::map<long, long> const years{{2017, 15}, {2019, 16}, {2022, 17}, {2026, 18}};
        if (auto it = years.find(major); it != years.end()) major = it->second;
        if (*end != '\0' || major <= 0 || major >= 2000) {
            return fail("visual-studio must be a major version such as \"17\", a product year such as \"2022\" or an installation path, not '"
                + wanted + "'");
        }
        argv.push_back("-version");
        argv.push_back("[" + std::to_string(major) + "," + std::to_string(major + 1) + ")");
    }
    auto out = output_of(std::move(argv), env);
    if (!out) return std::unexpected{out.error()};
    std::string const path = trimmed(out->substr(0, out->find('\n')));
    if (path.empty()) {
        return fail("no Visual Studio " + (wanted.empty() ? std::string{} : wanted + " ") + "with the C++ tools (MSVC x64) was found; "
            + "install the workload 'Desktop development with C++'" + (wanted.empty() ? "" : ", or change 'visual-studio' in config.toml"));
    }
    return path_from_utf8(path);
}

// Windows' own variables, from which vcvarsall.bat makes the environment of builds: nothing of the user's (build-lua-api.md 5,
// docs/PLAN.md decision 80).
env_vars windows_environment(env_vars const& env)
{
    env_vars clean;
    for (char const* name : {"SystemRoot", "SystemDrive", "windir", "ComSpec", "PATHEXT", "OS", "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER",
             "NUMBER_OF_PROCESSORS", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "CommonProgramFiles", "CommonProgramFiles(x86)",
             "CommonProgramW6432", "ProgramData", "ALLUSERSPROFILE", "TEMP", "TMP"}) {
        if (auto value = variable(env, name)) clean[name] = *value;
    }
    std::string const root = variable(env, "SystemRoot").value_or("C:\\Windows");
    clean["PATH"] = root + "\\System32;" + root + ";" + root + "\\System32\\Wbem;" + root + "\\System32\\WindowsPowerShell\\v1.0";
    return clean;
}

// The last lines of `text`, for a message.
std::string last_lines(std::string text, std::size_t count)
{
    std::erase(text, '\r');
    while (!text.empty() && text.back() == '\n') text.pop_back();
    std::size_t begin = text.size();
    for (std::size_t n = 0; n < count && begin > 0; ++n) {
        std::size_t const nl = text.rfind('\n', begin - 1);
        begin = nl == std::string::npos ? 0 : nl;
    }
    return text.substr(begin == 0 ? 0 : begin + 1);
}

// The environment of `vcvarsall.bat <arch>`, through a script run by cmd /u (UTF-16 output). Its own output goes to a
// file, shown when it fails.
result<env_vars> vcvars_environment(fs::path const& installation, std::string const& arch, env_vars const& env)
{
    fs::path const vcvars = installation / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
    std::error_code ec;
    if (!fs::exists(vcvars, ec)) return fail("'" + path_to_utf8(vcvars) + "' is missing: this Visual Studio has no C++ tools");
    // one name per call: the targets of the projects otok publish builds at once call this at the same time
    static std::atomic<unsigned> calls{0};
    std::string const name = "otok-vcvars-" + std::to_string(current_process_id()) + "-" + std::to_string(++calls) + "-" + arch;
    fs::path const script = fs::temp_directory_path() / (name + ".cmd");
    fs::path const log = fs::temp_directory_path() / (name + ".log");
    {
        std::ofstream out{script, std::ios::binary};
        out << "@call \"" << path_to_utf8(vcvars) << "\" " << arch << " >\"" << path_to_utf8(log) << "\" 2>&1\r\n"
            << "@if errorlevel 1 exit /b 1\r\n@set\r\n";
    }
    std::string const cmd = variable(env, "ComSpec").value_or(variable(env, "SystemRoot").value_or("C:\\Windows") + "\\System32\\cmd.exe");
    auto exit = run_process({{cmd, "/d", "/u", "/c", path_to_utf8(script)}, env, fs::current_path(), {}, true});
    auto const output = read_file(log);
    fs::remove(script, ec);
    fs::remove(log, ec);
    if (!exit) return std::unexpected{exit.error()};
    if (!exit->success()) {
        std::string const tail = output ? last_lines(*output, 10) : std::string{};
        return fail("'" + path_to_utf8(vcvars) + "' " + arch + " failed with " + describe_exit(*exit) + (tail.empty() ? "" : ":\n" + tail));
    }
    env_vars vars = detail::parse_set_output(utf8_of_utf16(exit->output));
    if (!variable(vars, "VCToolsInstallDir")) return fail("'" + path_to_utf8(vcvars) + "' " + arch + " did not set up MSVC");
    return vars;
}

result<visual_studio_environment> discover_visual_studio(std::string const& visual_studio, std::string const& arch, env_vars const& env)
{
    visual_studio_environment vs;
    std::string const vcvars = detail::vcvars_arch(machine_arch, arch);
    auto const target = variable(env, "VSCMD_ARG_TGT_ARCH");
    if (variable(env, "VSCMD_VER") && variable(env, "VCToolsInstallDir") && target == (arch == "arm64" ? "arm64" : "x64")) {
        vs.env = env;  // a developer prompt of the slice's architecture: the user chose this Visual Studio
        vs.description = "the Visual Studio " + *variable(env, "VSCMD_VER") + " developer prompt";
    } else {
        auto installation = visual_studio_installation(visual_studio, env);
        if (!installation) return std::unexpected{installation.error()};
        auto vars = vcvars_environment(*installation, vcvars, windows_environment(env));
        if (!vars) return std::unexpected{vars.error()};
        vs.env = std::move(*vars);
        vs.description = "Visual Studio " + variable(vs.env, "VisualStudioVersion").value_or("?") + " at '" + path_to_utf8(*installation) + "'";
        // vcvarsall.bat succeeds without the compiler of the slice's architecture, and the build would take the host's
        std::string const host = std::string_view{machine_arch} == "arm64" ? "ARM64" : "X64";
        std::string const slice_dir = arch == "arm64" ? "arm64" : "x64";
        fs::path const cl = path_from_utf8(trimmed(*variable(vs.env, "VCToolsInstallDir"))) / "bin" / ("Host" + host) / slice_dir / "cl.exe";
        std::error_code ec;
        if (!fs::exists(cl, ec)) {
            return fail(vs.description + " has no MSVC compiler for " + arch + " ('" + path_to_utf8(cl) + "' is missing): install its "
                + (arch == "arm64" ? "ARM64" : "x64") + " build tools, or leave " + arch + " out of 'architectures'");
        }
    }
    vs.crt = trimmed(variable(vs.env, "VCToolsVersion").value_or(""));
    vs.sdk = trimmed(variable(vs.env, "WindowsSDKVersion").value_or(""));
    vs.vc_dir = trimmed(variable(vs.env, "VCINSTALLDIR").value_or(""));
    return vs;
}

#else

result<visual_studio_environment> discover_visual_studio(std::string const&, std::string const&, env_vars const&)
{
    return fail("Visual Studio is on Windows only: this machine builds 'windows' with clang-cl (linux/clang-cl)");
}

#endif

// A file name as linkers compare it: Windows ignores case.
std::string link_name(fs::path const& file)
{
    std::string name = path_to_utf8(file.filename());
    for (char& c : name) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return name;
}

// Copies the header directory `from` into `to`; a file both have must have the same contents. Beyond 260 characters on Windows
// (merge_tree): Boost's deepest header is 90 characters below include/.
result<void> merge_headers(fs::path const& from, fs::path const& to, std::string const& slice)
{
    auto const differing = merge_tree(from, to);
    if (!differing) return fail("cannot copy the headers of '" + path_to_utf8(from) + "': " + differing.error().message);
    if (!differing->empty()) {
        return fail("the header '" + *differing + "' of " + slice + " differs from that of another slice: "
            + "the Windows target keeps one include/ for all architectures and configurations");
    }
    return {};
}

// MSVC's /std: values, which clang-cl knows too (otok-toml.md 5.1), and meson's names of them.
std::string standard_flag(language l, int standard)
{
    if (l == language::c) {
        if (standard_year(standard) < 2011) return std::string{};  // C89 with most of C99 without a flag
        return standard >= 23 ? std::string{"/std:clatest"} : "/std:c" + std::to_string(standard);
    }
    return standard >= 23 ? std::string{"/std:c++latest"} : "/std:c++" + std::to_string(std::max(standard, 14));
}

std::string meson_standard(language l, int standard)
{
    if (l == language::c) {
        if (standard_year(standard) < 2011) return std::string{};
        return standard >= 23 ? std::string{"clatest"} : "c" + std::to_string(standard);
    }
    return standard >= 23 ? std::string{"c++latest"} : "c++" + std::to_string(std::max(standard, 14));
}

// The configuration's flags as MSVC takes them (build-lua-api.md 3.1): /MD explicitly, or old CMake projects get /MT;
// /Z7 keeps the debug information in the objects, so a static library needs no .pdb (docs/PLAN.md decision 31).
std::vector<std::string> configuration_flags(bool debug)
{
    return debug ? std::vector<std::string>{"/Od", "/Z7", "/MDd"} : std::vector<std::string>{"/O2", "/DNDEBUG", "/MD"};
}

// The CMake toolchain of a slice built on Windows (`windows/msvc`, `windows/clang-cl`): the compilers, CMake's own MSVC
// settings off, nothing of Program Files (build-lua-api.md 6.2, docs/PLAN.md decision 80), the processor when the slice
// is of another architecture than the machine.
std::string windows_cmake_toolchain(std::string const& cc, std::string const& extra, std::string const& processor, bool cross, bool debug)
{
    std::string text = "# Generated by otok for a Windows slice built on Windows.\n";
    if (cross) {
        text += "set(CMAKE_SYSTEM_NAME Windows)\n";
        text += "set(CMAKE_SYSTEM_PROCESSOR " + processor + ")\n";
    }
    text += "set(CMAKE_C_COMPILER \"" + cc + "\")\n";
    text += "set(CMAKE_CXX_COMPILER \"" + cc + "\")\n";
    text += extra;
    text += "set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)\n";
    text += "set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)\n";
    text += "set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)\n";
    // CMake's own MSVC settings (policies CMP0091, CMP0141, CMP0184) would add a runtime library, /Zi and /RTC1 beside otok's
    // flags, also in the Debug try-compiles of a Release build
    text += std::string{"set(CMAKE_MSVC_RUNTIME_LIBRARY \""} + (debug ? "MultiThreadedDebugDLL" : "MultiThreadedDLL") + "\")\n";
    text += "set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT \"\")\n";
    text += "set(CMAKE_MSVC_RUNTIME_CHECKS \"\")\n";
    text += std::string{"set(CMAKE_TRY_COMPILE_CONFIGURATION "} + (debug ? "Debug" : "Release") + ")\n";
    return text;
}

class windows_build_target final : public build_target
{
public:
    windows_build_target(std::string visual_studio, std::string sdk, std::string llvm, env_vars environment)
        : visual_studio_name_{std::move(visual_studio)}, sdk_{std::move(sdk)}, llvm_{std::move(llvm)}, environment_{std::move(environment)}
    {
    }

    target_info const& info() const override { return windows::info(); }

    // tool slices on Windows (target_info::host_system)
    std::string tool_arch() const override { return machine_arch; }

    result<void> find_compiler(build_environment const& environment) override
    {
        if (environment.compiler == "msvc") {
            auto vs = visual_studio(machine_arch);
            if (!vs) return std::unexpected{vs.error()};
            if (!find_program("cl", (*vs)->env, fs::current_path())) return fail("cl.exe is not in the environment of " + (*vs)->description);
            return {};
        }
        if (environment.compiler == "clang-cl") {
#ifdef _WIN32
            auto llvm = windows_llvm();
            if (!llvm) return std::unexpected{llvm.error()};
#else
            auto found = xwin();
            if (!found) return std::unexpected{found.error()};
#endif
            return {};
        }
        return fail("the Windows target has no compiler '" + environment.compiler + "'");
    }

    // the PATH of the target's builds on this machine: Visual Studio's on Windows, LLVM's and the system's elsewhere
    result<std::string> tools_path() override
    {
#ifdef _WIN32
        auto vs = visual_studio(machine_arch);
        if (!vs) return std::unexpected{vs.error()};
        return variable((*vs)->env, "PATH").value_or("");
#else
        auto found = xwin();
        if (!found) return std::unexpected{found.error()};
        return (*found)->env.at("PATH");
#endif
    }

    result<toolchain_profile> profile(slice_spec const& slice) override
    {
        auto names = slice.platform == "windows" ? detail::names_of(slice.arch) : std::nullopt;
        if (!names) return fail("the Windows target has no slice " + slice_name(slice));
        result<toolchain_profile> p = slice.environment.compiler == "msvc" ? msvc_profile(slice, *names)
            : slice.environment.compiler != "clang-cl"                    ? fail("the Windows target has no compiler '" + slice.environment.compiler + "'")
            : slice.environment.system == "windows"                       ? clang_cl_on_windows(slice, *names)
                                                                          : clang_cl_with_xwin(slice, *names);
        if (!p) return p;
        for (auto [field, value] : {std::pair{"rust_target", &names->rust_target}, std::pair{"zig_target", &names->zig_target},
                 std::pair{"goarch", &names->goarch}}) {
            p->target_fields[field] = *value;
        }
        p->target_fields["goos"] = "windows";
        return p;
    }

    result<void> check_library(fs::path const& library, slice_spec const& slice) override
    {
        auto names = detail::names_of(slice.arch);
        if (!names) return fail("the Windows target has no slice " + slice_name(slice));
        std::vector<unsigned> const machines = coff_machines_of(library);
        if (machines.empty()) return fail("it holds no COFF object");
        if (machines != std::vector<unsigned>{names->coff_machine}) {
            auto const hex = [](unsigned m) {
                char text[16];
                std::snprintf(text, sizeof text, "0x%X", m);
                return std::string{text};
            };
            std::string list;
            for (unsigned m : machines) list += (list.empty() ? "" : ", ") + hex(m);
            return fail("its objects are for the COFF machines " + list + ", not for " + slice.arch + " (" + hex(names->coff_machine) + ")");
        }
        // what clang calls for 128-bit integers and MSVC's runtime lacks: a program linked by MSVC would miss it
        if (slice.environment.compiler != "clang-cl" || binary_kind_of(library) != binary_kind::archive) return {};
        std::string nm;
        env_vars env;
        if (slice.environment.system == "windows") {
#ifdef _WIN32
            auto llvm = windows_llvm();
            if (llvm) nm = (*llvm)->llvm_nm;
            if (auto vs = visual_studio(machine_arch)) env = (*vs)->env;
#endif
        } else if (auto found = xwin()) {
            nm = (*found)->llvm.llvm_nm;
            env = (*found)->env;
        }
        if (nm.empty()) return {};
        auto undefined = output_of({nm, "--undefined-only", "--format=just-symbols", path_to_utf8(library)}, env);
        if (!undefined) return std::unexpected{undefined.error()};
        if (std::vector<std::string> const helpers = detail::compiler_rt_helpers(*undefined); !helpers.empty()) {
            std::string list;
            for (std::string const& h : helpers) list += (list.empty() ? "" : ", ") + h;
            return fail("it calls " + list + " of compiler-rt (128-bit integers, which clang has and MSVC has not), which MSVC's runtime lacks: "
                + "a program linked by MSVC would not link; the build must not use __int128 (__uint128_t, __SIZEOF_INT128__)");
        }
        return {};
    }

    // include/, lib/<arch>/ and bin/<arch>/, debug/lib/<arch>/ and debug/bin/<arch>/ (otok-toml.md 4.8)
    result<void> make_package(built_package const& package, recipe const& rec, std::span<output_product const> products,
        fs::path const& staging) override
    {
        (void)rec;
        (void)products;
        std::error_code ec;
        fs::create_directories(long_path(staging / "include"), ec);
        if (ec) return fail("cannot create '" + path_to_utf8(staging) + "': " + ec.message());
        package_files recorded;  // which file is which library (otok-toml.md 4.11)
        for (built_slice const& s : package.slices) {
            build_output const& out = s.output;
            std::set<fs::path> headers;
            if (out.headers) headers.insert(*out.headers);
            for (auto const& [name, dir] : out.library_headers) headers.insert(dir);
            for (fs::path const& dir : headers) {
                if (auto ok = merge_headers(dir, staging / "include", slice_label(s.spec)); !ok) return ok;
            }
            // the newest toolset of the slices' CRTs, which a consumer needs at least
            if (std::string const crt = detail::crt_of_key(read_file(s.entry / "key.txt").value_or("")); !crt.empty()) {
                std::string& known = recorded.properties["crt"];
                if (known.empty() || compare_toolsets(crt, known) > 0) known = crt;
            }
            // under the names the build gave them, which may be part of their contract (MSVC auto-linking)
            fs::path const root = s.spec.configuration == "debug" ? staging / "debug" : staging;
            std::map<std::string, std::string> files;  // link name → library output name
            auto const place = [&](std::string const& name, fs::path const& file, char const* dir) -> result<void> {
                if (auto [it, fresh] = files.emplace(link_name(file), name); !fresh) {
                    return fail("the libraries '" + it->second + "' and '" + name + "' are both files named '" + path_to_utf8(file.filename()) + "'");
                }
                fs::path const to = root / dir / s.spec.arch;
                fs::create_directories(long_path(to), ec);
                // canonical gives the path back without the extended prefix
                fs::copy_file(long_path(fs::canonical(long_path(file), ec)), long_path(to / file.filename()), ec);
                if (ec) return fail("cannot copy the library '" + name + "': " + ec.message());
                return {};
            };
            std::string const at = std::string{s.spec.configuration == "debug" ? "debug/" : ""};
            for (auto const& [name, lib] : out.libraries) {
                package_library_file f{name, s.spec.arch, s.spec.configuration, at + "lib/" + s.spec.arch + "/" + path_to_utf8(lib.filename())};
                if (auto ok = place(name, lib, "lib"); !ok) return ok;
                if (auto it = out.runtimes.find(name); it != out.runtimes.end()) {
                    if (auto ok = place(name, it->second, "bin"); !ok) return ok;
                    f.runtime = at + "bin/" + s.spec.arch + "/" + path_to_utf8(it->second.filename());
                    f.shared = true;
                }
                if (auto it = out.symbols.find(name); it != out.symbols.end()) {
                    if (auto ok = place(name, it->second, "bin"); !ok) return ok;
                    f.symbols = at + "bin/" + s.spec.arch + "/" + path_to_utf8(it->second.filename());
                }
                recorded.files.push_back(std::move(f));
            }
        }
        return write_file_atomic(staging / package_files_name, write_package_files(recorded));
    }

    // otok-config.cmake, otok.props and otok.targets (otok-toml.md 4.11); and one file name in the lib/<arch>/ of two libraries, which
    // matters to consumers that link through search paths.
    result<void> finish_output(fs::path const& dir, std::span<output_package const> packages, std::map<std::string, std::string> const& deployment,
        std::vector<std::string>& warnings) const override
    {
        (void)deployment;
        std::map<std::string, std::string> owners;  // <sub>/<arch>/<link name> → library
        for (output_package const& p : packages) {
            for (char const* sub : {"lib", "bin", "debug/lib", "debug/bin"}) {
                std::error_code ec;
                for (auto arch = fs::directory_iterator{long_path(dir / p.dir / sub), ec}; !ec && arch != fs::directory_iterator{}; arch.increment(ec)) {
                    std::string const at = std::string{sub} + "/" + path_to_utf8(arch->path().filename()) + "/";
                    std::error_code file_ec;
                    for (auto it = fs::directory_iterator{arch->path(), file_ec}; !file_ec && it != fs::directory_iterator{}; it.increment(file_ec)) {
                        if (auto [o, fresh] = owners.emplace(at + link_name(it->path()), p.library); !fresh) {
                            warnings.push_back("the libraries '" + o->second + "' and '" + p.library + "' both have '" + at
                                + path_to_utf8(it->path().filename()) + "': a consumer linking through search paths gets only one of them");
                        }
                    }
                }
            }
        }
        auto c = consumer_package_of(dir, packages, info().platforms.front().architectures);
        if (!c) return std::unexpected{c.error()};
        for (auto const& [name, text] : {std::pair{"otok-config.cmake", consumer_cmake_config(*c, detail::cmake_parts(*c))},
                 std::pair{"otok.props", detail::msbuild_props(*c)}, std::pair{"otok.targets", detail::msbuild_targets(*c)}}) {
            if (auto ok = write_file_if_changed(dir / name, text); !ok) return std::unexpected{ok.error()};
        }
        return {};
    }

private:
    // The flags, the environment and the target table every slice has, whatever its compiler.
    static void common_parts(toolchain_profile& p, slice_spec const& slice, detail::arch_names const& names, std::string const& crt,
        std::string const& sdk, std::string const& sdk_dir)
    {
        slice_toolchain& t = p.toolchain;
        t.env["PKG_CONFIG_LIBDIR"] = "";  // only dependencies, nothing from the machine
        t.env["CC"] = t.cc;
        t.env["CXX"] = t.cxx;
        t.env["CFLAGS"] = joined_flags(t.cflags);
        t.env["CXXFLAGS"] = joined_flags(t.cxxflags);
        p.standard_flag = standard_flag;
        p.meson_standard = meson_standard;
        p.meson_options = {"-Db_vscrt=none"};  // otok's flags choose the runtime (build-lua-api.md 6.2)
        // a slice of another architecture than the machine's is cross-compiled for meson
        if (slice.environment.system != "windows" || slice.arch != machine_arch) {
            p.meson_host = {{"system", "windows"}, {"cpu_family", names.meson_cpu_family}, {"cpu", names.meson_cpu}, {"endian", "little"}};
        }
        p.target_tables["windows"] = {{"arch", slice.arch}, {"sdk", sdk_dir}, {"crt_version", crt}, {"sdk_version", sdk}};
    }

    // `windows/msvc` (otok-toml.md 4.8): cl.exe, link.exe and lib.exe of Visual Studio's environment for the slice.
    // What a build reads of Visual Studio's environment: the VC directory (the compilers, vcvarsall.bat) and the Windows
    // SDK. Readable to every AppContainer; named so that otok-sandbox.dll hands their ancestors and shows them in a listing
    // of C:\ (docs/PLAN.md decision 111: b2's msvc.jam lists every ancestor of cl.exe, and found no compiler).
    std::vector<fs::path> visual_studio_reads(visual_studio_environment const& v)
    {
        std::vector<fs::path> out;
        if (!v.vc_dir.empty()) out.push_back(path_from_utf8(v.vc_dir));
        std::string sdk = variable(v.env, "WindowsSdkDir").value_or("");
        while (!sdk.empty() && (sdk.back() == '\\' || sdk.back() == '/' || sdk.back() == ' ')) sdk.pop_back();
        if (!sdk.empty()) out.push_back(path_from_utf8(sdk));
        return out;
    }

    result<toolchain_profile> msvc_profile(slice_spec const& slice, detail::arch_names const& names)
    {
        auto vs = visual_studio(slice.arch);
        if (!vs) return std::unexpected{vs.error()};
        visual_studio_environment const& v = **vs;
        auto cl = find_program("cl", v.env, fs::current_path());
        if (!cl) return fail("cl.exe is not in the environment of " + v.description + ": " + cl.error().message);
        bool const debug = slice.configuration == "debug";
        toolchain_profile p;
        p.read_paths = visual_studio_reads(v);
        p.triple = names.triple;
        p.identity = {{"msvc", v.crt}, {"crt", v.crt}, {"windows-sdk", v.sdk}};
        p.description = v.description + " (MSVC " + v.crt + ", Windows SDK " + v.sdk + ")";
        slice_toolchain& t = p.toolchain;
        t.cc = t.cxx = slashed(*cl);
        if (auto lib = find_program("lib", v.env, fs::current_path())) t.ar = slashed(*lib);
        t.cflags = configuration_flags(debug);
        t.cxxflags = t.cflags;
        t.env = v.env;
        // cl /Zi and /FS write PDBs through mspdbsrv, one per user unless named: another otok's server sees that otok's
        // drive letters, P:\ its work directories, and cl failed with C1041 (docs/PLAN.md decision 114). One per otok
        // process, started in its job; not in the slice's key (the environment never is).
        t.env["_MSPDBSRV_ENDPOINT_"] = "otok-" + std::to_string(current_process_id());
        common_parts(p, slice, names, v.crt, v.sdk, "");
        p.cmake_toolchain = windows_cmake_toolchain(t.cc, "", names.cmake_processor, slice.arch != machine_arch, debug);
        // b2 (build-lua-api.md 6.2): msvc.jam knows its versions by toolset, 14.51.36231 is 14.5 (vc145), and takes 32-bit x86
        // without the architecture, whatever Visual Studio's environment is
        p.b2_toolset = "msvc";
        if (v.crt.size() >= 4) p.b2_version = v.crt.substr(0, 4);
        p.b2_properties = {std::string{"architecture="} + (slice.arch == "arm64" ? "arm" : "x86"), "address-model=64", "abi=ms"};
        return p;
    }

    // clang-cl's flags: the target, MSVC's version of the CRT (19.44 for 14.44), which the STL checks, objects without
    // timestamps, lld-link for the links clang-cl runs itself (configure's, meson's checks: its default on Linux is GNU ld),
    // and the configuration's flags as MSVC takes them; C++17 removed `register`, which MSVC still takes with a warning
    // (C5033) and clang makes an error: as MSVC.
    static void clang_cl_parts(toolchain_profile& p, llvm_tools const& llvm, detail::arch_names const& names, std::string const& crt, bool debug)
    {
        p.meson_binaries = {{"c", {llvm.clang_cl, "--target=" + names.triple}}, {"cpp", {llvm.clang_cl, "--target=" + names.triple}},
            {"c_ld", {llvm.lld_link}}, {"cpp_ld", {llvm.lld_link}}, {"windres", {llvm.llvm_rc}}};
        p.prefix_map_flag = "/clang:-ffile-prefix-map=";  // clang-cl takes GCC's options behind /clang:
        slice_toolchain& t = p.toolchain;
        t.cc = llvm.clang_cl;
        t.cxx = llvm.clang_cl;
        t.ar = llvm.llvm_lib;
        t.nm = llvm.llvm_nm;
        std::size_t const dot = crt.find('.');
        std::string const minor = crt.substr(dot + 1, crt.find('.', dot + 1) - dot - 1);
        t.driver_flags = {"--target=" + names.triple};  // as in meson's binaries (build-lua-api.md 3.1)
        t.cflags = {"--target=" + names.triple, "-fuse-ld=lld-link", "-fms-compatibility-version=19." + minor, "/Brepro"};
        std::vector<std::string> const configuration = configuration_flags(debug);
        t.cflags.insert(t.cflags.end(), configuration.begin(), configuration.end());
        t.cxxflags = t.cflags;
        t.cxxflags.push_back("-Wno-register");
    }

    // `windows/clang-cl`: clang-cl of LLVM with the CRT and the SDK of Visual Studio's environment for the slice.
    result<toolchain_profile> clang_cl_on_windows(slice_spec const& slice, detail::arch_names const& names)
    {
        auto vs = visual_studio(slice.arch);
        if (!vs) return std::unexpected{vs.error()};
        auto llvm = windows_llvm();
        if (!llvm) return std::unexpected{llvm.error()};
        visual_studio_environment const& v = **vs;
        llvm_tools const& l = **llvm;
        bool const debug = slice.configuration == "debug";
        toolchain_profile p;
        p.read_paths = visual_studio_reads(v);
        p.read_paths.push_back(path_from_utf8(l.bin));
        p.triple = names.triple;
        p.identity = {{"clang-cl", l.version}, {"crt", v.crt}, {"windows-sdk", v.sdk}};
        p.description = "clang-cl " + l.version + " at '" + l.bin + "' with " + v.description + " (CRT " + v.crt + ", Windows SDK " + v.sdk + ")";
        clang_cl_parts(p, l, names, v.crt, debug);
        slice_toolchain& t = p.toolchain;
        t.env = v.env;
        // LLVM's directory first: its lld-link, llvm-lib and llvm-rc, not Visual Studio's own tools of those names
        std::string const path = variable(t.env, "PATH").value_or("");
        std::erase_if(t.env, [](auto const& e) { return is_path_name(e.first); });  // Windows spells it "Path" as often as not
        t.env["PATH"] = l.bin + ";" + path;
        common_parts(p, slice, names, v.crt, v.sdk, "");
        std::string extra = "set(CMAKE_LINKER \"" + l.lld_link + "\" CACHE FILEPATH \"\")\n"
                            "set(CMAKE_AR \"" + l.llvm_lib + "\" CACHE FILEPATH \"\")\n"
                            "set(CMAKE_RC_COMPILER \"" + l.llvm_rc + "\" CACHE FILEPATH \"\")\n"
                            "set(CMAKE_ASM_COMPILER_TARGET " + names.triple + ")\n";
        if (!l.llvm_mt.empty()) extra += "set(CMAKE_MT \"" + l.llvm_mt + "\" CACHE FILEPATH \"\")\n";
        p.cmake_toolchain = windows_cmake_toolchain(t.cc, extra, names.cmake_processor, slice.arch != machine_arch, debug);
        return p;
    }

    // `linux/clang-cl`: clang-cl across from Linux, against the CRT and the SDK of `xwin splat`.
    result<toolchain_profile> clang_cl_with_xwin(slice_spec const& slice, detail::arch_names const& names)
    {
        auto found = xwin();
        if (!found) return std::unexpected{found.error()};
        xwin_tools const& f = **found;
        bool const debug = slice.configuration == "debug";
        std::error_code ec;
        std::string const crt_lib = f.root + "/crt/lib/" + names.xwin;
        if (!fs::is_directory(path_from_utf8(crt_lib), ec) || !fs::is_directory(path_from_utf8(f.root + "/sdk/lib/um/" + names.xwin), ec)) {
            return fail("'" + f.root + "' has no libraries for " + slice.arch + ": make it with `xwin --arch " + names.xwin + " … splat`");
        }
        if (debug && !fs::is_regular_file(path_from_utf8(crt_lib + "/msvcrtd.lib"), ec)) {
            return fail("'" + f.root + "' has no debug libraries of the CRT for " + slice.arch + " (msvcrtd.lib): make it with `xwin … splat "
                + "--include-debug-libs`");
        }
        toolchain_profile p;
        p.read_paths = {path_from_utf8(f.root), path_from_utf8(f.llvm.bin)};
        p.triple = names.triple;
        p.identity = {{"clang-cl", f.llvm.version}, {"crt", f.crt}, {"windows-sdk", f.sdk}};
        p.description = f.description;
        // configure scripts are cross-compiling; few know this triple yet (autotools builds come later)
        p.autotools_host = names.triple;
        clang_cl_parts(p, f.llvm, names, f.crt, debug);
        slice_toolchain& t = p.toolchain;
        // the headers and libraries as vcvars gives them, for every tool that runs clang-cl, lld-link or llvm-rc itself
        t.env = f.env;
        t.env["INCLUDE"] = f.root + "/crt/include;" + f.root + "/sdk/include/ucrt;" + f.root + "/sdk/include/um;" + f.root + "/sdk/include/shared;"
            + f.root + "/sdk/include/winrt;" + f.root + "/sdk/include/cppwinrt";
        t.env["LIB"] = crt_lib + ";" + f.root + "/sdk/lib/um/" + names.xwin + ";" + f.root + "/sdk/lib/ucrt/" + names.xwin;
        common_parts(p, slice, names, f.crt, f.sdk, f.root);
        p.cmake_toolchain = detail::cmake_toolchain({slice.arch, names.triple, names.cmake_processor, t.cc, f.llvm.lld_link, f.llvm.llvm_lib,
            f.llvm.llvm_rc, f.llvm.llvm_mt, debug});
        return p;
    }

    result<xwin_tools const*> xwin()
    {
        if (!xwin_) xwin_ = discover_xwin(sdk_, llvm_, environment_);
        if (!*xwin_) return std::unexpected{xwin_->error()};
        return &**xwin_;
    }

    result<visual_studio_environment const*> visual_studio(std::string const& arch)
    {
        auto it = visual_studio_.find(arch);
        if (it == visual_studio_.end()) it = visual_studio_.emplace(arch, discover_visual_studio(visual_studio_name_, arch, environment_)).first;
        if (!it->second) return std::unexpected{it->second.error()};
        return &*it->second;
    }

    // clang-cl on Windows: of the PATH (LLVM's installer), else Visual Studio's LLVM (its component "C++ Clang tools").
    result<llvm_tools const*> windows_llvm()
    {
        if (!windows_llvm_) {
            windows_llvm_ = [&]() -> result<llvm_tools> {
                auto clang_cl = find_clang_cl(llvm_, environment_);
                if (!clang_cl && !llvm_.empty()) return std::unexpected{clang_cl.error()};
                if (!clang_cl) {
                    auto vs = visual_studio(machine_arch);
                    if (!vs) return std::unexpected{vs.error()};
                    fs::path const bundled = path_from_utf8((*vs)->vc_dir) / "Tools" / "Llvm" / (std::string_view{machine_arch} == "arm64" ? "ARM64" : "x64")
                        / "bin" / "clang-cl.exe";
                    std::error_code ec;
                    if (!fs::is_regular_file(bundled, ec)) {
                        return fail("clang-cl is neither in PATH nor in Visual Studio ('" + path_to_utf8(bundled) + "'): set 'llvm' in config.toml to an LLVM "
                            + "installation, or install the component 'C++ Clang tools for Windows' of Visual Studio");
                    }
                    clang_cl = bundled;
                }
                return llvm_tools_of(*clang_cl, environment_);
            }();
        }
        if (!*windows_llvm_) return std::unexpected{windows_llvm_->error()};
        return &**windows_llvm_;
    }

    std::string visual_studio_name_;
    std::string sdk_;
    std::string llvm_;
    env_vars environment_;
    std::optional<result<xwin_tools>> xwin_;
    std::map<std::string, result<visual_studio_environment>> visual_studio_;
    std::optional<result<llvm_tools>> windows_llvm_;
};

}

target_info const& info()
{
    static target_info const windows{
        .name = "windows",
        .platforms = {{"windows", {"x86_64", "arm64"}, {}}},
        .deployment_keys = {},
        .environments = {{"windows", "", "msvc"}, {"windows", "", "clang-cl"}, {"linux", "", "clang-cl"}},
        .any_host_compilers = {},
        // MSVC's, with its CRT (docs/PLAN.md decision 91)
        .abis = {{"msvc", "msvc"}, {"clang-cl", "msvc"}},
        .host_system = "windows",
        .tool_platform = "windows",
        .linkages = {"static", "dynamic"},
        .many_configurations = true,
        .choices = {},
        .packages_dir = "",  // the output directory itself: <output>/<library> (otok-toml.md 4.8)
        .read_recipe_table = {},
        .read_product_table = {},
        .delivers_sources = {},
        .publish_defaults = "",
        .check_publishable = {},
    };
    return windows;
}

std::unique_ptr<build_target> make_build_target(std::string visual_studio, std::string sdk, std::string llvm, env_vars environment)
{
    return std::make_unique<windows_build_target>(std::move(visual_studio), std::move(sdk), std::move(llvm), std::move(environment));
}

env_vars detail::parse_set_output(std::string_view text)
{
    env_vars vars;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::size_t const eq = line.find('=');
        // "=C:=C:\…": cmd's hidden per-drive directories, not variables
        if (eq != std::string_view::npos && eq > 0) vars[std::string{line.substr(0, eq)}] = std::string{line.substr(eq + 1)};
        start = end + 1;
    }
    return vars;
}

std::string detail::vcvars_arch(std::string_view machine, std::string_view slice)
{
    std::string const host = machine == "arm64" ? "arm64" : "x64";
    std::string const target = slice == "arm64" ? "arm64" : "x64";
    return host == target ? host : host + "_" + target;
}

std::optional<detail::arch_names> detail::names_of(std::string_view arch)
{
    if (arch == "x86_64") {
        return arch_names{"x86_64", "x86_64-pc-windows-msvc", 0x8664, "AMD64", "x86_64", "x86_64", "x86_64-pc-windows-msvc", "x86_64-windows-msvc", "amd64"};
    }
    if (arch == "arm64") {
        return arch_names{"aarch64", "aarch64-pc-windows-msvc", 0xAA64, "ARM64", "aarch64", "aarch64", "aarch64-pc-windows-msvc", "aarch64-windows-msvc",
            "arm64"};
    }
    return std::nullopt;
}

std::string detail::crt_version(std::string_view text)
{
    // #define _VC_CRT_MAJOR_VERSION 14
    auto const number = [&](std::string_view name) -> std::optional<std::string> {
        std::string const define = "#define " + std::string{name};
        std::size_t at = text.find(define);
        if (at == std::string_view::npos) return std::nullopt;
        at += define.size();
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
        std::size_t const from = at;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
        if (at == from) return std::nullopt;
        return std::string{text.substr(from, at - from)};
    };
    auto const major = number("_VC_CRT_MAJOR_VERSION");
    auto const minor = number("_VC_CRT_MINOR_VERSION");
    auto const build = number("_VC_CRT_BUILD_VERSION");
    if (!major || !minor || !build) return {};
    return *major + "." + *minor + "." + *build;
}

int detail::clang_minimum(std::string_view text)
{
    std::string_view const marker = "__clang_major__ <";
    std::size_t at = text.find(marker);
    if (at == std::string_view::npos) return 0;
    at += marker.size();
    while (at < text.size() && text[at] == ' ') ++at;
    int v = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) v = v * 10 + (text[at++] - '0');
    return v;
}

std::string detail::clang_version(std::string_view text)
{
    std::string_view const marker = "clang version ";
    std::size_t at = text.find(marker);
    if (at == std::string_view::npos) return {};
    at += marker.size();
    std::size_t end = at;
    while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.')) ++end;
    return std::string{text.substr(at, end - at)};
}

std::string detail::sdk_version(fs::path const& sdk_include)
{
    std::string best;
    std::error_code ec;
    for (auto it = fs::directory_iterator{sdk_include, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
        std::string const name = path_to_utf8(it->path().filename());
        if (name.starts_with("10.0.") && name > best) best = name;
    }
    return best;
}

std::vector<std::string> detail::compiler_rt_helpers(std::string_view undefined)
{
    // compiler-rt's builtins for TImode integers: division, remainder, overflow checks, shifts, conversions
    static std::set<std::string_view> const helpers{"__udivti3", "__umodti3", "__divti3", "__modti3", "__udivmodti4", "__divmodti4", "__muloti4",
        "__multi3", "__ashlti3", "__ashrti3", "__lshrti3", "__fixdfti", "__fixsfti", "__fixunsdfti", "__fixunssfti", "__floattidf", "__floattisf",
        "__floatuntidf", "__floatuntisf", "__popcountti2", "__clzti2", "__ctzti2", "__ffsti2", "__parityti2"};
    std::set<std::string> found;
    for (std::size_t at = 0; at < undefined.size();) {
        std::size_t const end = std::min(undefined.find('\n', at), undefined.size());
        std::string_view line = undefined.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (helpers.contains(line)) found.emplace(line);
    }
    return {found.begin(), found.end()};
}

std::string detail::cmake_toolchain(cmake_inputs const& in)
{
    auto const quoted = [](std::string const& s) { return "\"" + s + "\""; };
    std::string text = "# Generated by otok for a Windows slice (" + in.arch + "), built with clang-cl.\n";
    text += "set(CMAKE_SYSTEM_NAME Windows)\n";
    text += "set(CMAKE_SYSTEM_VERSION 10.0)\n";
    text += "set(CMAKE_SYSTEM_PROCESSOR " + in.processor + ")\n";
    text += "set(CMAKE_C_COMPILER " + quoted(in.cc) + ")\n";
    text += "set(CMAKE_CXX_COMPILER " + quoted(in.cc) + ")\n";
    // assembly (`.S`) is compiled by clang-cl too, which the C and C++ flags with their --target do not reach
    text += "set(CMAKE_ASM_COMPILER_TARGET " + in.triple + ")\n";
    text += "set(CMAKE_LINKER " + quoted(in.linker) + " CACHE FILEPATH \"\")\n";
    text += "set(CMAKE_AR " + quoted(in.ar) + " CACHE FILEPATH \"\")\n";
    text += "set(CMAKE_RC_COMPILER " + quoted(in.rc) + " CACHE FILEPATH \"\")\n";
    if (!in.mt.empty()) text += "set(CMAKE_MT " + quoted(in.mt) + " CACHE FILEPATH \"\")\n";
    // CMake's own MSVC settings would add a runtime library, /Zi and /RTC1 beside otok's flags (as for the native target)
    text += std::string{"set(CMAKE_MSVC_RUNTIME_LIBRARY \""} + (in.debug ? "MultiThreadedDebugDLL" : "MultiThreadedDLL") + "\")\n";
    text += "set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT \"\")\n";
    text += "set(CMAKE_MSVC_RUNTIME_CHECKS \"\")\n";
    text += std::string{"set(CMAKE_TRY_COMPILE_CONFIGURATION "} + (in.debug ? "Debug" : "Release") + ")\n";
    text += "# nothing from the build machine, only the dependencies (docs/PLAN.md decision 25)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n";
    text += "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n";
    return text;
}

}
