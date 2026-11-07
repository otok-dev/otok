# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Static libraries with the dynamic CRT: otok.exe carries its dependencies but no private CRT copy.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CXX_FLAGS "/std:c++latest /utf-8 -D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS")
set(VCPKG_C_FLAGS "/utf-8")

set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    "-DCMAKE_CXX_EXTENSIONS=OFF"
)
