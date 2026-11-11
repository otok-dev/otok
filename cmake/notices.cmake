# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Writes OTOK_OUTPUT, the license notices of the third-party software built into otok, which `otok licenses` prints
# (docs/PLAN.md decision 7: their licenses ask for the notices to go with every binary): Lua, dataforge, the JavaScript
# libraries of the catalog site and the copyright file of every vcpkg port under OTOK_VCPKG_SHARE but gtest (the tests')
# and vcpkg's own helper ports. A build without vcpkg (Homebrew's, with the system's packages) leaves those to its
# packager: OTOK_VCPKG_SHARE may be empty or missing. OTOK_EXTRA_NOTICES: a directory of more `<name>.txt` notices of
# what the build links (musl in the static Linux binary, decision 104); empty: none. The notices are staged as
# `<name>.txt` and embedded by cmake/embed.cmake. Both builds run it: CMakeLists.txt and Directory.Build.targets.

file(TO_CMAKE_PATH "${OTOK_ROOT}" OTOK_ROOT)
file(TO_CMAKE_PATH "${OTOK_OUTPUT}" OTOK_OUTPUT)
set(stage ${OTOK_OUTPUT}.d)
file(REMOVE_RECURSE ${stage})
file(MAKE_DIRECTORY ${stage})

file(COPY_FILE ${OTOK_ROOT}/third_party/lua/LICENSE ${stage}/lua.txt)
file(COPY_FILE ${OTOK_ROOT}/modules/dataforge/LICENSE_1_0.txt ${stage}/dataforge.txt)
file(COPY_FILE ${OTOK_ROOT}/src/otok/catalog/site/assets/minisearch.LICENSE.txt ${stage}/minisearch.txt)
file(COPY_FILE ${OTOK_ROOT}/src/otok/catalog/site/assets/diff.LICENSE.txt ${stage}/jsdiff.txt)

if(OTOK_VCPKG_SHARE)
    file(TO_CMAKE_PATH "${OTOK_VCPKG_SHARE}" OTOK_VCPKG_SHARE)
    if(IS_DIRECTORY ${OTOK_VCPKG_SHARE})
        file(GLOB ports RELATIVE ${OTOK_VCPKG_SHARE} ${OTOK_VCPKG_SHARE}/*)
        foreach(port IN LISTS ports)
            if(port STREQUAL "gtest" OR port MATCHES "^vcpkg-")
                continue()
            endif()
            if(EXISTS ${OTOK_VCPKG_SHARE}/${port}/copyright)
                file(COPY_FILE ${OTOK_VCPKG_SHARE}/${port}/copyright ${stage}/${port}.txt)
            endif()
        endforeach()
    endif()
endif()

if(OTOK_EXTRA_NOTICES)
    file(TO_CMAKE_PATH "${OTOK_EXTRA_NOTICES}" OTOK_EXTRA_NOTICES)
    file(GLOB extra ${OTOK_EXTRA_NOTICES}/*.txt)
    if(NOT extra)
        message(FATAL_ERROR "no notices in OTOK_EXTRA_NOTICES '${OTOK_EXTRA_NOTICES}'")
    endif()
    file(COPY ${extra} DESTINATION ${stage})
endif()

set(OTOK_EMBED_DIR ${stage})
include(${CMAKE_CURRENT_LIST_DIR}/embed.cmake)
