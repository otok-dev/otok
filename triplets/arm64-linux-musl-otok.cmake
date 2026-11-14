# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# The release binary for Linux (tools/release/linux.sh, docs/PLAN.md decision 104): built on Alpine with its GCC and
# musl, static libraries, release only; otok links them and musl statically.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# OpenSSL's Configure disables threads when the linker flags have -static, which vcpkg's Linux toolchain adds for a static CRT:
# without them its error state is one for the process, and two threads opening HTTPS connections at once (the builds of
# otok publish) corrupt the heap. The library stays static; otok itself is linked -static (docs/PLAN.md decision 118).
if(PORT STREQUAL "openssl")
    set(VCPKG_CRT_LINKAGE dynamic)
endif()
