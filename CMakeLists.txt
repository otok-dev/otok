# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Build of otok for macOS and Linux (and CI). Development on Windows uses otok.slnx; both builds compile
# the same sources, found here by glob.
#
# Dependencies: find_package() of tomlplusplus, nlohmann_json, inja, CURL, LibArchive and GTest, from vcpkg
# (CMAKE_TOOLCHAIN_FILE) or from the system (Homebrew; libcurl of the macOS SDK).

cmake_minimum_required(VERSION 3.25)

# The version is `version` of src/otok/core/build_info.hpp (docs/PLAN.md decision 50).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/src/otok/core/build_info.hpp OTOK_VERSION_LINE REGEX "std::string_view version = ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" OTOK_VERSION "${OTOK_VERSION_LINE}")
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/src/otok/core/build_info.hpp OTOK_YEARS_LINE REGEX "std::string_view copyright_years = ")
string(REGEX MATCH "[0-9]+(-[0-9]+)?" OTOK_COPYRIGHT_YEARS "${OTOK_YEARS_LINE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/otok/core/build_info.hpp)

project(otok VERSION ${OTOK_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OTOK_BUILD_TESTS "Build otok-tests" ON)
set(OTOK_REVISION "" CACHE STRING "Source revision to embed instead of `git describe` (builds without .git)")

find_package(tomlplusplus CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(inja CONFIG REQUIRED)  # the templates of the catalog site (docs/PLAN.md decision 49)
# macOS: the system libcurl of the SDK (docs/PLAN.md decision 14); elsewhere vcpkg's static one.
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)
find_package(Threads REQUIRED)  # the builder runs slices on threads (docs/PLAN.md decision 20)
if(OTOK_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

set(OTOK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/otok)
set(OTOK_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# otok_revision.hpp: `git describe` of this repository, rewritten only when it changes
# (the MSBuild build generates the same file in Directory.Build.targets).
add_custom_target(otok-revision
    COMMAND ${CMAKE_COMMAND}
        -DOTOK_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DOTOK_OUTPUT=${OTOK_GENERATED_DIR}/otok_revision.hpp
        -DOTOK_REVISION_OVERRIDE=${OTOK_REVISION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/revision.cmake
    BYPRODUCTS ${OTOK_GENERATED_DIR}/otok_revision.hpp
    COMMENT "Updating otok_revision.hpp")

# otok_catalog_site.inc: the files of the catalog site, src/otok/catalog/site/, built into otok
# (the MSBuild build generates the same file in Directory.Build.targets).
set(OTOK_SITE_DIR ${OTOK_SRC}/catalog/site)
file(GLOB_RECURSE OTOK_SITE_FILES CONFIGURE_DEPENDS ${OTOK_SITE_DIR}/*)
add_custom_command(
    OUTPUT ${OTOK_GENERATED_DIR}/otok_catalog_site.inc
    COMMAND ${CMAKE_COMMAND}
        -DOTOK_EMBED_DIR=${OTOK_SITE_DIR}
        -DOTOK_OUTPUT=${OTOK_GENERATED_DIR}/otok_catalog_site.inc
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${OTOK_SITE_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding the catalog site")

# otok_notices.inc: the license notices of the third-party software built into otok, `otok licenses`
# (cmake/notices.cmake; the MSBuild build generates the same file in Directory.Build.targets). Run at every build: what
# vcpkg installed decides what is linked.
set(OTOK_EXTRA_NOTICES "" CACHE PATH "A directory of more <name>.txt notices for `otok licenses` (cmake/notices.cmake)")
set(OTOK_VCPKG_SHARE "")
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(OTOK_VCPKG_SHARE ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share)
endif()
add_custom_target(otok-notices
    COMMAND ${CMAKE_COMMAND}
        -DOTOK_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DOTOK_VCPKG_SHARE=${OTOK_VCPKG_SHARE}
        -DOTOK_EXTRA_NOTICES=${OTOK_EXTRA_NOTICES}
        -DOTOK_OUTPUT=${OTOK_GENERATED_DIR}/otok_notices.inc
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/notices.cmake
    BYPRODUCTS ${OTOK_GENERATED_DIR}/otok_notices.inc
    COMMENT "Embedding the third-party license notices")

# Compiler settings shared by every otok target (the counterpart of projects/vs/otok.props).
add_library(otok-options INTERFACE)
target_include_directories(otok-options INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OTOK_GENERATED_DIR})
# third-party headers: no warnings from them
target_include_directories(otok-options SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/dataforge/include)
if(MSVC)
    target_compile_options(otok-options INTERFACE /utf-8 /W4 /Zc:__cplusplus /Zc:preprocessor /bigobj
        /external:anglebrackets /external:W0)
    target_compile_definitions(otok-options INTERFACE
        _CRT_SECURE_NO_WARNINGS _SCL_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(otok-options INTERFACE -Wall -Wextra)
endif()
if(APPLE)
    # otok-core reaches the executables both directly and through the target libraries; Apple's ld warns.
    target_link_options(otok-options INTERFACE LINKER:-no_warn_duplicate_libraries)
endif()

# Lua 5.5.1, part of otok's sources, compiled as C++ so that its errors are C++ exceptions (third_party/lua/README.md;
# docs/PLAN.md decisions 18 and 50). Third-party code: compiled without warnings.
file(GLOB OTOK_LUA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src/*.c)
set_source_files_properties(${OTOK_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(otok-lua STATIC ${OTOK_LUA_SOURCES})
target_include_directories(otok-lua SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src)
if(MSVC)
    target_compile_options(otok-lua PRIVATE /w)
    target_compile_definitions(otok-lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(otok-lua PRIVATE -w)
endif()
if(UNIX)
    target_compile_definitions(otok-lua PRIVATE LUA_USE_POSIX)
    target_link_libraries(otok-lua PUBLIC m)
endif()

# otok-core: everything except the targets and the command line.
file(GLOB_RECURSE OTOK_CORE_SOURCES CONFIGURE_DEPENDS ${OTOK_SRC}/*.cpp)
list(FILTER OTOK_CORE_SOURCES EXCLUDE REGEX "/src/otok/(target|cli)/")
add_library(otok-core STATIC ${OTOK_CORE_SOURCES} ${OTOK_GENERATED_DIR}/otok_catalog_site.inc)
add_dependencies(otok-core otok-revision otok-notices)
target_link_libraries(otok-core PUBLIC otok-options tomlplusplus::tomlplusplus nlohmann_json::nlohmann_json pantor::inja otok-lua CURL::libcurl LibArchive::LibArchive Threads::Threads)
# Microsoft Detours on Windows: the sandbox injects otok-sandbox.dll with it (docs/PLAN.md decision 103).
if(WIN32)
    find_path(OTOK_DETOURS_INCLUDE_DIR detours/detours.h REQUIRED)
    # vcpkg's release and debug builds, each for its configuration: the Visual Studio generator builds both from one
    # configure, and the debug library needs the debug CRT
    find_library(OTOK_DETOURS_LIBRARY_RELEASE detours PATHS ${OTOK_DETOURS_INCLUDE_DIR}/../lib NO_DEFAULT_PATH REQUIRED)
    find_library(OTOK_DETOURS_LIBRARY_DEBUG detours PATHS ${OTOK_DETOURS_INCLUDE_DIR}/../debug/lib NO_DEFAULT_PATH)
    include(SelectLibraryConfigurations)
    select_library_configurations(OTOK_DETOURS)
    target_include_directories(otok-core SYSTEM PUBLIC ${OTOK_DETOURS_INCLUDE_DIR})
    target_link_libraries(otok-core PUBLIC ${OTOK_DETOURS_LIBRARIES} userenv)
endif()

file(GLOB_RECURSE OTOK_TARGET_LINUX_SOURCES CONFIGURE_DEPENDS ${OTOK_SRC}/target/linux/*.cpp)
add_library(otok-target-linux STATIC ${OTOK_TARGET_LINUX_SOURCES})
target_link_libraries(otok-target-linux PUBLIC otok-core)

file(GLOB_RECURSE OTOK_TARGET_ANDROID_SOURCES CONFIGURE_DEPENDS ${OTOK_SRC}/target/android/*.cpp)
add_library(otok-target-android STATIC ${OTOK_TARGET_ANDROID_SOURCES})
target_link_libraries(otok-target-android PUBLIC otok-core)

file(GLOB_RECURSE OTOK_TARGET_WINDOWS_SOURCES CONFIGURE_DEPENDS ${OTOK_SRC}/target/windows/*.cpp)
add_library(otok-target-windows STATIC ${OTOK_TARGET_WINDOWS_SOURCES})
target_link_libraries(otok-target-windows PUBLIC otok-core)

file(GLOB_RECURSE OTOK_TARGET_APPLE_SOURCES CONFIGURE_DEPENDS ${OTOK_SRC}/target/apple/*.cpp)
add_library(otok-target-apple STATIC ${OTOK_TARGET_APPLE_SOURCES})
target_link_libraries(otok-target-apple PUBLIC otok-core)

# otok-sandbox.dll: injected with Microsoft Detours into the processes of sandboxed builds on Windows (docs/PLAN.md
# decision 103); beside otok.exe.
if(WIN32)
    add_library(otok-sandbox SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/otok-sandbox/sandbox_dll.cpp)
    target_include_directories(otok-sandbox SYSTEM PRIVATE ${OTOK_DETOURS_INCLUDE_DIR})
    target_link_libraries(otok-sandbox PRIVATE otok-options ${OTOK_DETOURS_LIBRARIES} advapi32)
endif()

file(GLOB_RECURSE OTOK_CLI_SOURCES CONFIGURE_DEPENDS ${OTOK_SRC}/cli/*.cpp)
add_executable(otok ${OTOK_CLI_SOURCES})
target_link_libraries(otok PRIVATE otok-core otok-target-linux otok-target-android otok-target-windows otok-target-apple)
if(WIN32)
    add_dependencies(otok otok-sandbox)
endif()

# The version resources of otok.exe and otok-sandbox.dll (src/otok/core/version_info.rc, docs/PLAN.md decision 107), with
# otok_version.h from build_info.hpp, written only when it changes. Directory.Build.targets generates the same file.
if(WIN32)
    string(REPLACE "." ";" OTOK_VERSION_PARTS "${OTOK_VERSION}")
    list(GET OTOK_VERSION_PARTS 0 OTOK_VERSION_MAJOR)
    list(GET OTOK_VERSION_PARTS 1 OTOK_VERSION_MINOR)
    list(GET OTOK_VERSION_PARTS 2 OTOK_VERSION_PATCH)
    file(CONFIGURE OUTPUT ${OTOK_GENERATED_DIR}/otok_version.h CONTENT [[
// generated by CMakeLists.txt from src/otok/core/build_info.hpp
#pragma once
#define OTOK_VERSION_MAJOR @OTOK_VERSION_MAJOR@
#define OTOK_VERSION_MINOR @OTOK_VERSION_MINOR@
#define OTOK_VERSION_PATCH @OTOK_VERSION_PATCH@
#define OTOK_VERSION_STRING "@OTOK_VERSION@"
#define OTOK_LEGAL_COPYRIGHT "Copyright (C) @OTOK_COPYRIGHT_YEARS@ Alexander A Pototskiy"
]] @ONLY)
    set(OTOK_RC_INCLUDES ${OTOK_GENERATED_DIR} ${OTOK_SRC}/core)
    target_sources(otok PRIVATE ${OTOK_SRC}/cli/otok.rc)
    target_sources(otok-sandbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/otok-sandbox/otok-sandbox.rc)
    set_source_files_properties(${OTOK_SRC}/cli/otok.rc ${CMAKE_CURRENT_SOURCE_DIR}/src/otok-sandbox/otok-sandbox.rc
        PROPERTIES INCLUDE_DIRECTORIES "${OTOK_RC_INCLUDES}"
        OBJECT_DEPENDS "${OTOK_GENERATED_DIR}/otok_version.h;${OTOK_SRC}/core/version_info.rc")
endif()

if(OTOK_BUILD_TESTS)
    enable_testing()
    # The program tests/unit/process_test.cpp starts.
    add_executable(otok-test-child ${CMAKE_CURRENT_SOURCE_DIR}/tests/child/child.cpp)
    target_link_libraries(otok-test-child PRIVATE otok-options)

    file(GLOB_RECURSE OTOK_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp)
    add_executable(otok-tests ${OTOK_TEST_SOURCES})
    target_link_libraries(otok-tests PRIVATE otok-core otok-target-linux otok-target-android otok-target-windows otok-target-apple GTest::gtest GTest::gtest_main)
    add_dependencies(otok-tests otok-test-child)
    if(WIN32)
        add_dependencies(otok-tests otok-sandbox)
    endif()
    # Fixtures are found through this path, independent of the working directory.
    target_compile_definitions(otok-tests PRIVATE OTOK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        OTOK_TEST_CHILD="$<TARGET_FILE:otok-test-child>")
    include(GoogleTest)
    gtest_discover_tests(otok-tests)
endif()
