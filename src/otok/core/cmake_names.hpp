// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "otok/core/diagnostics.hpp"
#include "otok/core/toml_reader.hpp"

// The CMake packages of a library under the names projects find it by without otok (recipe-toml.md 11.4,
// otok-toml.md 4.12, docs/PLAN.md decision 127): `[cmake.<package>]` of a recipe, `cmake` of [deps.<library>].
namespace otok {

// A target, a component or a variable of a package, and the products it stands for.
struct cmake_name
{
    std::string name;
    std::vector<std::string> products;
    bool include = false;                   // a variable of the include directories of its one product

    bool operator==(cmake_name const&) const = default;
};

struct cmake_package_spec
{
    std::string name;                       // as projects write it: "OpenSSL"
    std::vector<cmake_name> targets;        // "OpenSSL::SSL" → { "ssl" }
    std::vector<cmake_name> components;
    std::vector<cmake_name> variables;
    source_position where{};

    bool operator==(cmake_package_spec const& other) const
    {
        return name == other.name && targets == other.targets && components == other.components && variables == other.variables;
    }
};

bool is_cmake_package_name(std::string_view name) noexcept;     // [A-Za-z0-9_.+-]{1,64}
bool is_cmake_target_name(std::string_view name) noexcept;      // <namespace>::<name>, not otok::
bool is_cmake_variable_name(std::string_view name) noexcept;    // [A-Za-z_][A-Za-z0-9_]{0,127}

// ASCII case folding, as package names are matched (otok-toml.md 4.12).
std::string cmake_lower(std::string_view text);
std::string cmake_upper(std::string_view text);

// Reads a table whose keys are packages: `[cmake]` of a recipe, `cmake` of [deps.<library>]. Problems go to the
// table's diagnostics; packages with errors are left out.
std::vector<cmake_package_spec> read_cmake_packages(toml_table& table);

// Every product the packages name, once each, in their order.
std::vector<std::string> cmake_products(std::vector<cmake_package_spec> const& packages);

}
