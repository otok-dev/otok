// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/environment.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <system_error>

#include "otok/core/fs.hpp"
#include "otok/core/names.hpp"
#include "otok/core/process.hpp"

namespace otok {

namespace {

constexpr std::array<std::string_view, 3> systems = {"windows", "macos", "linux"};
constexpr std::array<std::string_view, 2> architectures = {"x86_64", "arm64"};

bool known(std::span<std::string_view const> names, std::string_view s) noexcept
{
    return std::ranges::find(names, s) != names.end();
}

// A compiler's name: as a library's ([a-z0-9][a-z0-9_-]*), which "clang-cl" is.
bool is_compiler_name(std::string_view s) noexcept
{
    return is_library_name(s);
}

}

std::span<std::string_view const> host_systems() noexcept
{
    return systems;
}

std::span<std::string_view const> host_architectures() noexcept
{
    return architectures;
}

bool is_host_machine(std::string_view s) noexcept
{
    std::size_t const dash = s.find('-');
    if (dash == std::string_view::npos) return false;
    std::string_view const system = s.substr(0, dash);
    std::string_view arch = s.substr(dash + 1);
    constexpr std::string_view musl = "-musl";
    if (system == "linux" && arch.ends_with(musl)) arch.remove_suffix(musl.size());
    return known(systems, system) && known(architectures, arch);
}

std::string_view libc_of_root(std::filesystem::path const& root)
{
    std::error_code ec;
    bool musl = false;
    for (std::filesystem::directory_iterator it{root / "lib", ec}, end; !ec && it != end; it.increment(ec)) {
        std::string const name = path_to_utf8(it->path().filename());
        if (name.starts_with("ld-musl-") && name.ends_with(".so.1")) musl = true;
    }
    if (!musl) return "glibc";
    // glibc's loaders of x86_64 and arm64
    for (char const* loader : {"lib64/ld-linux-x86-64.so.2", "lib/ld-linux-x86-64.so.2", "lib/ld-linux-aarch64.so.1", "lib64/ld-linux-aarch64.so.1"}) {
        if (std::filesystem::exists(root / loader, ec)) return "glibc";
    }
    return "musl";
}

std::string_view host_libc()
{
#if defined(__linux__)
    static std::string_view const libc = libc_of_root("/");
    return libc;
#else
    return {};
#endif
}

std::string const& host_condition()
{
    static std::string const name = std::string{host_machine()} + (host_libc() == "musl" ? "-musl" : "");
    return name;
}

std::string build_environment::text() const
{
    return (any_host() ? std::string{"*"} : system + (arch.empty() ? "" : "-" + arch)) + "/" + compiler;
}

std::optional<build_environment> parse_environment(std::string_view text)
{
    std::size_t const slash = text.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    std::string_view const host = text.substr(0, slash);
    std::string_view const compiler = text.substr(slash + 1);
    if (!is_compiler_name(compiler)) return std::nullopt;
    build_environment e;
    e.compiler = compiler;
    if (host == "*") return e;
    std::size_t const dash = host.find('-');
    std::string_view const system = host.substr(0, dash);
    if (!known(systems, system)) return std::nullopt;
    e.system = system;
    if (dash != std::string_view::npos) {
        std::string_view const arch = host.substr(dash + 1);
        if (!known(architectures, arch)) return std::nullopt;
        e.arch = arch;
    }
    return e;
}

bool covers(build_environment const& a, build_environment const& b) noexcept
{
    if (a.compiler != b.compiler) return false;
    if (a.any_host()) return true;
    if (a.system != b.system) return false;
    return a.arch.empty() || a.arch == b.arch;
}

build_environment machine_environment(std::string_view compiler)
{
    std::string_view const machine = host_machine();
    std::size_t const dash = machine.find('-');
    return {std::string{machine.substr(0, dash)}, std::string{machine.substr(dash + 1)}, std::string{compiler}};
}

}
