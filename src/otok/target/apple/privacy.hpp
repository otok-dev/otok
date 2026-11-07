// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "otok/core/error.hpp"

// The required-reason API check (DESIGN 10.1, otok-toml.md 4.4, index.md 10.1).
namespace otok::target::apple {

// A category of index.md 10.1 with the symbols that use it.
struct required_reason_api
{
    std::string category;               // "NSPrivacyAccessedAPICategoryFileTimestamp"
    std::vector<std::string> symbols;   // C names: "stat"
    std::vector<std::string> classes;   // Objective-C classes: "NSUserDefaults"
};

// Reads `apple/required-reason-apis.toml`; `file` names it in messages.
result<std::vector<required_reason_api>> parse_required_reason_apis(std::string_view content, std::string file);

// otok's built-in copy of the list.
std::vector<required_reason_api> const& builtin_required_reason_apis();

// The categories a privacy manifest declares (`NSPrivacyAccessedAPITypes`), from its XML property list.
result<std::set<std::string>> declared_api_categories(std::string_view manifest);

// The undefined symbols in the output of `nm -u -j`: sorted, each once, without the headers of archive
// members and architectures.
std::vector<std::string> parse_undefined_symbols(std::string_view nm_output);

// The external symbols in the output of `nm -g -U -m` (otok-toml.md 4.5): all of them, and those that may be
// defined more than once (weak definitions and common symbols), each sorted and once.
struct defined_symbols
{
    std::vector<std::string> all;
    std::vector<std::string> weak;
};

defined_symbols parse_defined_symbols(std::string_view nm_output);

// Uses of required-reason APIs that `declared` does not cover: the symbols (by their C or class names)
// grouped by the categories any of which would declare them.
struct api_use
{
    std::vector<std::string> categories;
    std::vector<std::string> symbols;
};

std::vector<api_use> undeclared_uses(std::span<std::string const> undefined, std::span<required_reason_api const> apis,
    std::set<std::string> const& declared);

// A private system library of index.md 10.2.
struct private_library
{
    std::string path;   // its text stub, relative to the SDK root
    std::string name;   // for messages
};

// Reads `apple/private-libraries.toml`; `file` names it in messages.
result<std::vector<private_library>> parse_private_libraries(std::string_view content, std::string file);

// otok's built-in copy of the list.
std::vector<private_library> const& builtin_private_libraries();

// The symbols a text stub (`.tbd`, version 4) exports: those of its `exports` and `reexports`, by their
// Mach-O names (`_ubrk_next`), without Objective-C classes.
result<std::set<std::string>> parse_tbd_exports(std::string_view tbd);

}
