// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Build environments (common 4.2, docs/PLAN.md decision 82): where and with what a slice is built, written
// `<host>/<compiler>`.
namespace otok {

// The systems and architectures of build machines: `when = { host = … }` (recipe-toml.md 9.1) and environments.
std::span<std::string_view const> host_systems() noexcept;
std::span<std::string_view const> host_architectures() noexcept;

// `<system>-<arch>`: "macos-arm64".
bool is_host_machine(std::string_view s) noexcept;

struct build_environment
{
    std::string system{};     // "macos", "linux", "windows"; empty: every host ("*")
    std::string arch{};       // "x86_64", "arm64"; empty: every architecture of the system
    std::string compiler{};   // "xcode", "ndk", "msvc", "clang-cl", "gcc", "clang"

    bool any_host() const noexcept { return system.empty(); }

    // "windows-arm64/msvc", "*/ndk"
    std::string text() const;

    auto operator<=>(build_environment const&) const = default;
};

// Reads the text of an environment: the syntax only, not whether a target has it. Nothing when it is not one.
std::optional<build_environment> parse_environment(std::string_view text);

// Whether `a` covers `b` (common 4.2): `*` covers every host, a system without architecture its every
// architecture; the compilers are the same.
bool covers(build_environment const& a, build_environment const& b) noexcept;

// The environment of the machine otok runs on for a compiler: "linux-x86_64/gcc" as `<system>-<arch>/<compiler>`.
build_environment machine_environment(std::string_view compiler);

}
