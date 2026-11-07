// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#include "otok/core/environment.hpp"

#include <algorithm>
#include <array>

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
    return dash != std::string_view::npos && known(systems, s.substr(0, dash)) && known(architectures, s.substr(dash + 1));
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
