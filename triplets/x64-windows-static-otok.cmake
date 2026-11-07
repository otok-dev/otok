# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# The release binary for Windows (tools/release/windows-x64.ps1): static libraries with the static CRT, so that
# otok.exe needs no Visual C++ Redistributable.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CXX_FLAGS "/std:c++latest /utf-8 -D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS")
set(VCPKG_C_FLAGS "/utf-8")

set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    "-DCMAKE_CXX_EXTENSIONS=OFF"
)
