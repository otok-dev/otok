// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <memory>
#include <string>

#include "otok/builder/target.hpp"
#include "otok/core/process.hpp"
#include "otok/core/target_info.hpp"

// The Linux target (otok-toml.md 4.1, docs/PLAN.md decision 82): libraries for programs of the build machine's Linux,
// with its glibc and GNU C++ runtime, built with GCC (`linux/gcc`) or Clang (`linux/clang`); output as a plain prefix
// directory. Until decision 82 the development target `native` did this, and MSVC on Windows. (GCC defines the macro
// `linux` in its GNU modes only; otok is built with -std=c++23, CMAKE_CXX_EXTENSIONS OFF.)
namespace otok::target::linux {

// One platform, `linux`, of the architecture otok runs on.
target_info const& info();

// The target for builds. The compilers are looked up in the PATH of `environment`; its builds get nothing else of it.
std::unique_ptr<build_target> make_build_target(env_vars environment);

}
