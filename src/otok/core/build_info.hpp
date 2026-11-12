// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <span>
#include <string_view>

// What this otok binary is: its release version and the source revision it was built from.
namespace otok::build_info {

// The release version of otok (semantic versioning).
inline constexpr std::string_view version = "0.5.5";

// The years of the copyright notice: the first year, or the first and the last release's year (written by
// tools/release/release.sh).
inline constexpr std::string_view copyright_years = "2026";

// The source revision otok was built from (`git describe` output), or "unknown".
std::string_view revision() noexcept;

// Where the source of otok is published, with a tag `v<version>` per release (GPLv3 section 6: named beside the binary
// by `--version` and `otok licenses`).
inline constexpr std::string_view source_repository = "https://github.com/otok-dev/otok";

// A notice of third-party software built into this binary: its name and its license text (`otok licenses`).
struct third_party_notice
{
    std::string_view name;
    std::string_view text;
};

// Every notice, sorted by name (cmake/notices.cmake).
std::span<third_party_notice const> third_party_notices() noexcept;

}
