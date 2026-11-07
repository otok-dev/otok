// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <memory>
#include <vector>

#include "otok/builder/target.hpp"
#include "otok/config/config.hpp"
#include "otok/core/process.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "otok/target/windows/windows_target.hpp"

namespace otok::test {

// The real targets of this machine, as otok's own main makes them: tool slices are built by the one of its system
// (otok-toml.md 7), `env` without the user's MAKEFLAGS.
inline std::vector<std::unique_ptr<build_target>> machine_build_targets(machine_config const& config, env_vars env)
{
    std::erase_if(env, [](auto const& v) { return v.first == "MAKEFLAGS"; });
    std::vector<std::unique_ptr<build_target>> targets;
    targets.push_back(target::android::make_build_target(config.android_ndk, env));
    targets.push_back(target::windows::make_build_target(config.visual_studio, config.windows_sdk, config.llvm, env));
    targets.push_back(target::linux::make_build_target(env));
#ifdef __APPLE__
    targets.push_back(target::apple::make_build_target(config.xcode, env));
#endif
    return targets;
}

}
