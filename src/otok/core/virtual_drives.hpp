// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "otok/core/error.hpp"

// Short drive letters for long directories on Windows (docs/PLAN.md open item 23, docs/tasks/windows-long-paths.md):
// the build tools of Windows fail beyond MAX_PATH whatever LongPathsEnabled says, so build processes see the cache and
// the work directories through letters of a private device map. The map is an object directory of otok's with a
// symbolic link per letter: the letters this process saw when the map was made (a private map hides the logon
// session's: subst and network drives) and the virtual ones, each to the NT path of a long directory. otok sets it on
// itself, so that the virtual paths serve its own file operations and the programs it starts, and on every process it
// starts (core/process); a map is not inherited, so otok-sandbox.dll sets it on each child in turn, and in an
// AppContainer, which may not set it, asks otok's broker (core/sandbox). Elsewhere nothing of this exists and paths stay
// as they are.
namespace otok::virtual_drives {

// Makes the map and sets it on this process, once; later calls return the first one's result.
result<void> enable();

// Whether enable() succeeded.
bool enabled() noexcept;

// The root of a letter linked to `real` (an absolute directory), as "Q:\": the same letter for the same directory, a new
// one, the first free of a fixed order, otherwise. Fails when the map is not enabled or no letter is free. Each call is
// a use of the letter, which release() ends.
result<std::filesystem::path> drive_for(std::filesystem::path const& real);

// Ends a use of the letter of `root` (as drive_for returned it); after the last, the letter is gone and free again.
void release(std::filesystem::path const& root);

// `p` under a directory with a letter, through that letter (the deepest such directory); otherwise `p` as it is.
std::filesystem::path to_virtual(std::filesystem::path const& p);

// `p` on a virtual letter, as the real path; otherwise `p` as it is.
std::filesystem::path to_real(std::filesystem::path const& p);

struct drive
{
    wchar_t letter = 0;
    std::filesystem::path real;  // the directory
    std::wstring device;         // the link's target, \Device\HarddiskVolumeN\…
};

// The virtual letters, the deepest directories first.
std::vector<drive> drives();

// The object directory of the map (a HANDLE), null when not enabled.
void* directory() noexcept;

// Sets the map on a process (a HANDLE with PROCESS_SET_INFORMATION): nothing when not enabled.
result<void> give(void* process);

}
