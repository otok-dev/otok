// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// The Apple target (docs/PLAN.md decision 25): the names of slices, the CMake toolchain file and the
// checks of built libraries on every host; real builds for every platform on a Mac.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "otok/commands/commands.hpp"
#include "otok/core/binary.hpp"
#include "otok/core/fs.hpp"
#include "otok/core/process.hpp"
#include "otok/target/android/android_target.hpp"
#include "otok/target/apple/apple_recipe.hpp"
#include "otok/target/apple/apple_target.hpp"
#include "otok/target/apple/privacy.hpp"
#include "otok/target/linux/linux_target.hpp"
#include "otok/target/windows/windows_target.hpp"
#include "test_util.hpp"

using namespace otok;
using namespace otok::target::apple::detail;

namespace {

target_info const* const all_targets[] = {&target::apple::info(), &target::android::info(), &target::windows::info(), &target::linux::info()};

slice_names names(std::string platform, std::string arch, std::string deployment = "")
{
    auto n = names_of({std::move(platform), std::move(arch), std::move(deployment)});
    EXPECT_TRUE(n.has_value());
    return n.value_or(slice_names{});
}

}

TEST(apple_target, names_slices)
{
    slice_names const ios = names("ios", "arm64", "16.0");
    EXPECT_EQ(ios.sdk, "iphoneos");
    EXPECT_EQ(ios.triple, "arm64-apple-ios16.0");
    EXPECT_EQ(ios.version_min_flag, "-mios-version-min=16.0");
    EXPECT_EQ(ios.deployment_variable, "IPHONEOS_DEPLOYMENT_TARGET");
    EXPECT_EQ(ios.cmake_system, "iOS");
    EXPECT_EQ(ios.autotools_host, "aarch64-apple-darwin");
    EXPECT_EQ(ios.rust_target, "aarch64-apple-ios");
    EXPECT_EQ(ios.zig_target, "aarch64-ios");
    EXPECT_EQ(ios.goos, "ios");
    EXPECT_EQ(ios.goarch, "arm64");
    EXPECT_EQ(ios.platform, 2);

    slice_names const sim = names("iossim", "x86_64", "16.0");
    EXPECT_EQ(sim.sdk, "iphonesimulator");
    EXPECT_EQ(sim.triple, "x86_64-apple-ios16.0-simulator");
    EXPECT_EQ(sim.version_min_flag, "-mios-simulator-version-min=16.0");
    EXPECT_EQ(sim.rust_target, "x86_64-apple-ios");  // the Intel simulator predates "-sim"
    EXPECT_EQ(sim.goarch, "amd64");
    EXPECT_EQ(sim.platform, 7);
    EXPECT_EQ(names("iossim", "arm64", "16.0").rust_target, "aarch64-apple-ios-sim");
    EXPECT_EQ(sim.b2_properties, (std::vector<std::string>{"target-os=iphone", "architecture=x86", "address-model=64", "abi=sysv", "binary-format=mach-o"}));
    EXPECT_EQ(names("macosx", "arm64").b2_properties,
        (std::vector<std::string>{"target-os=darwin", "architecture=arm", "address-model=64", "abi=aapcs", "binary-format=mach-o"}));
    EXPECT_EQ(names("watchos", "arm64_32", "9.0").b2_properties[2], "address-model=32");

    slice_names const catalyst = names("catalyst", "arm64", "16.0");
    EXPECT_EQ(catalyst.sdk, "macosx");
    EXPECT_EQ(catalyst.triple, "arm64-apple-ios16.0-macabi");
    EXPECT_EQ(catalyst.version_min_flag, "--target=arm64-apple-ios16.0-macabi");
    EXPECT_EQ(catalyst.cmake_system, "Darwin");
    EXPECT_EQ(catalyst.platform, 6);

    slice_names const mac = names("macosx", "arm64");  // the tool slice: no deployment version
    EXPECT_EQ(mac.triple, "arm64-apple-macos");
    EXPECT_EQ(mac.cmake_system, "");
    EXPECT_EQ(mac.rust_target, "aarch64-apple-darwin");
    EXPECT_EQ(mac.goos, "darwin");

    slice_names const watch = names("watchos", "arm64_32", "9.0");
    EXPECT_EQ(watch.triple, "arm64_32-apple-watchos9.0");
    EXPECT_EQ(watch.autotools_host, "arm-apple-darwin");
    // the Mac as config.guess names it: never equal to a --host, so configure runs no test program
    EXPECT_EQ(autotools_build("arm64", "27.0.0"), "aarch64-apple-darwin27.0.0");
    EXPECT_EQ(autotools_build("x86_64", "24.6.0"), "x86_64-apple-darwin24.6.0");
    EXPECT_EQ(watch.rust_target, "arm64_32-apple-watchos");
    EXPECT_EQ(watch.zig_target, "");
    EXPECT_EQ(watch.goos, "");
    EXPECT_EQ(names("xrossim", "arm64", "1.0").triple, "arm64-apple-xros1.0-simulator");
    EXPECT_EQ(names("xrossim", "arm64", "1.0").sdk, "xrsimulator");
    EXPECT_EQ(names("tvossim", "arm64", "16.0").version_min_flag, "-mtvos-simulator-version-min=16.0");
    // meson's names (build-lua-api.md 6.2)
    EXPECT_EQ(ios.meson_subsystem, "ios");
    EXPECT_EQ(ios.meson_cpu_family, "aarch64");
    EXPECT_EQ(sim.meson_subsystem, "ios-simulator");
    EXPECT_EQ(sim.meson_cpu_family, "x86_64");
    EXPECT_EQ(catalyst.meson_subsystem, "ios");
    EXPECT_EQ(mac.meson_subsystem, "macos");
    EXPECT_EQ(watch.meson_cpu_family, "aarch64");
    EXPECT_EQ(names("watchossim", "arm64", "9.0").meson_subsystem, "watchos-simulator");
    EXPECT_EQ(names("xros", "arm64", "1.0").meson_subsystem, "visionos");
    EXPECT_EQ(names("tvossim", "arm64", "16.0").meson_subsystem, "tvos-simulator");

    EXPECT_FALSE(names_of({"ios", "x86_64", "16.0"}));  // no Intel iOS devices
    EXPECT_FALSE(names_of({"native", "arm64"}));
}

TEST(apple_target, writes_the_cmake_toolchain_file)
{
    cmake_inputs in{names("iossim", "arm64", "16.0"), "arm64", "16.0", "/sdk/iPhoneSimulator.sdk", "/x/clang", "/x/clang++", "/x/ar", "/x/ranlib"};
    EXPECT_EQ(cmake_toolchain(in),
        "# Generated by otok for an Apple slice (arm64-apple-ios16.0-simulator).\n"
        "set(CMAKE_SYSTEM_NAME iOS)\n"
        "set(CMAKE_SYSTEM_PROCESSOR arm64)\n"
        "set(CMAKE_OSX_SYSROOT \"/sdk/iPhoneSimulator.sdk\" CACHE PATH \"\")\n"
        "set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING \"\")\n"
        "set(CMAKE_OSX_DEPLOYMENT_TARGET \"16.0\" CACHE STRING \"\")\n"
        "set(CMAKE_C_COMPILER \"/x/clang\")\n"
        "set(CMAKE_CXX_COMPILER \"/x/clang++\")\n"
        "set(CMAKE_AR \"/x/ar\")\n"
        "set(CMAKE_RANLIB \"/x/ranlib\")\n"
        "set(CMAKE_MACOSX_BUNDLE OFF)\n"
        "# nothing from outside the SDK and the dependencies (docs/PLAN.md decision 25)\n"
        "list(APPEND CMAKE_FIND_ROOT_PATH \"/sdk/iPhoneSimulator.sdk\")\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n"
        "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n");

    // macOS builds run on the Mac; Mac Catalyst takes its version from the triple only
    in.names = names("macosx", "arm64", "13.0");
    in.deployment = "13.0";
    std::string const mac = cmake_toolchain(in);
    EXPECT_EQ(mac.find("CMAKE_SYSTEM_NAME"), std::string::npos) << mac;
    EXPECT_NE(mac.find("set(CMAKE_OSX_DEPLOYMENT_TARGET \"13.0\" CACHE STRING \"\")\n"), std::string::npos) << mac;
    in.names = names("catalyst", "arm64", "16.0");
    std::string const catalyst = cmake_toolchain(in);
    EXPECT_NE(catalyst.find("set(CMAKE_SYSTEM_NAME Darwin)\n"), std::string::npos) << catalyst;
    EXPECT_EQ(catalyst.find("CMAKE_OSX_DEPLOYMENT_TARGET"), std::string::npos) << catalyst;
}

TEST(apple_target, checks_build_versions)
{
    // `otool -l` of an archive with two members
    std::string const otool = "Archive : /tmp/libt.a\n/tmp/libt.a(a.o):\nLoad command 0\n      cmd LC_SEGMENT_64\n  cmdsize 232\n"
                              "Load command 1\n      cmd LC_BUILD_VERSION\n  cmdsize 24\n platform 7\n    minos 15.0\n      sdk 27.0\n   ntools 0\n"
                              "/tmp/libt.a(b.o):\nLoad command 0\n      cmd LC_BUILD_VERSION\n  cmdsize 24\n platform 7\n    minos 16.2\n      sdk 27.0\n";
    auto const versions = parse_build_versions(otool);
    ASSERT_EQ(versions.size(), 2u);
    EXPECT_EQ(versions[0].platform, 7);
    EXPECT_EQ(versions[0].minos, "15.0");
    EXPECT_EQ(versions[1].minos, "16.2");

    EXPECT_TRUE(check_build_versions(versions, {"iossim", "arm64", "16.2"}));
    EXPECT_TRUE(check_build_versions(versions, {"iossim", "arm64", "17"}));
    auto newer = check_build_versions(versions, {"iossim", "arm64", "16.1"});
    ASSERT_FALSE(newer);
    EXPECT_EQ(newer.error().message, "it contains code for iOS Simulator 16.2, newer than the deployment version 16.1");
    auto other = check_build_versions(versions, {"ios", "arm64", "17.0"});
    ASSERT_FALSE(other);
    EXPECT_EQ(other.error().message, "it contains code for iOS Simulator, not for iOS");
    EXPECT_TRUE(check_build_versions(versions, {"iossim", "arm64", ""}));  // the tool slice has no deployment version
}

TEST(apple_target, writes_module_maps_and_package_swift)
{
    EXPECT_EQ(module_map("COpenSSL", {"openssl/crypto.h", "openssl/ssl.h"}, {}, false),
        "// Generated by otok.\nmodule COpenSSL {\n    header \"../openssl/crypto.h\"\n    header \"../openssl/ssl.h\"\n    export *\n}\n");
    // product defines: otok_defines.h first, so they take effect in the library's headers
    EXPECT_EQ(module_map("COpenSSL", {"openssl/ssl.h"}, {}, true),
        "// Generated by otok.\nmodule COpenSSL {\n    header \"otok_defines.h\"\n    header \"../openssl/ssl.h\"\n    export *\n}\n");
    // per-architecture copies are textual: only the copy of the architecture compiled is included
    EXPECT_EQ(module_map("COpenSSL", {"openssl/configuration.h"}, {"otok-arch/arm64/openssl/configuration.h"}, false),
        "// Generated by otok.\nmodule COpenSSL {\n    header \"../openssl/configuration.h\"\n"
        "    textual header \"../otok-arch/arm64/openssl/configuration.h\"\n    export *\n}\n");
    EXPECT_EQ(arch_header("openssl/configuration.h", {"arm64_32", "arm64"}),
        "// Generated by otok: this header differs between the architectures of the platform (otok-toml.md 4).\n"
        "#if defined(__arm64__) && !defined(__LP64__)\n#include \"../otok-arch/arm64_32/openssl/configuration.h\"\n"
        "#elif defined(__arm64__) && defined(__LP64__)\n#include \"../otok-arch/arm64/openssl/configuration.h\"\n"
        "#else\n#error \"otok: no copy of openssl/configuration.h for this architecture\"\n#endif\n");
    EXPECT_EQ(arch_header("top.h", {"x86_64"}),
        "// Generated by otok: this header differs between the architectures of the platform (otok-toml.md 4).\n"
        "#if defined(__x86_64__)\n#include \"otok-arch/x86_64/top.h\"\n"
        "#else\n#error \"otok: no copy of top.h for this architecture\"\n#endif\n");
    EXPECT_EQ(defines_header("COpenSSL", {"OPENSSL_NO_DEPRECATED", "LEVEL=3"}),
        "// Generated by otok: the defines of the module COpenSSL (otok-toml.md 4).\n#pragma once\n#define OPENSSL_NO_DEPRECATED\n#define LEVEL 3\n");

    swift_package p;
    p.tools_version = "5.9";
    p.platforms = {{".iOS", "16.0"}, {".macOS", "13.0"}};
    p.products = {{"OpenSSL", {"ssl", "crypto"}}, {"LibSSH2", {"ssh2", "ssl", "crypto"}}};
    p.binary_targets = {{"ssl", "Packages/openssl/ssl.xcframework"}, {"crypto", "Packages/openssl/crypto.xcframework"},
        {"ssh2", "Packages/libssh2/ssh2.xcframework"}};
    EXPECT_EQ(package_swift(p),
        "// swift-tools-version:5.9\n"
        "// Generated by otok; do not edit.\n"
        "\n"
        "import PackageDescription\n"
        "\n"
        "let package = Package(\n"
        "    name: \"Otok\",\n"
        "    platforms: [.iOS(\"16.0\"), .macOS(\"13.0\")],\n"
        "    products: [\n"
        "        .library(name: \"OpenSSL\", targets: [\"ssl\", \"crypto\"]),\n"
        "        .library(name: \"LibSSH2\", targets: [\"ssh2\", \"ssl\", \"crypto\"]),\n"
        "    ],\n"
        "    targets: [\n"
        "        .binaryTarget(name: \"ssl\", path: \"Packages/openssl/ssl.xcframework\"),\n"
        "        .binaryTarget(name: \"crypto\", path: \"Packages/openssl/crypto.xcframework\"),\n"
        "        .binaryTarget(name: \"ssh2\", path: \"Packages/libssh2/ssh2.xcframework\"),\n"
        "    ]\n"
        ")\n");

    // a shim and a wrapper (otok-toml.md 4)
    p.platforms.clear();
    p.products = {{"OpenSSL", {"ssl", "crypto", "OpenSSL_otok", "OpenSSLSwift"}}};
    p.binary_targets = {{"ssl", "Packages/openssl/ssl.xcframework"}};
    p.targets = {{"OpenSSL_otok", "Packages/openssl/shims/OpenSSL", {}, {".copy(\"PrivacyInfo.xcprivacy\")"}, {}, {},
                     {".linkedLibrary(\"z\")", ".linkedFramework(\"Security\")"}},
        {"OpenSSLSwift", "Packages/openssl/wrappers/OpenSSLSwift", {"ssl", "OpenSSL_otok"}, {".process(\"Resources\")"},
            {".define(\"OTOK_OPTION_FAST\")", ".define(\"OTOK_OPTION_LEVEL\", to: \"3\")"}, {".define(\"OTOK_OPTION_FAST\")"}, {}}};
    EXPECT_EQ(package_swift(p),
        "// swift-tools-version:5.9\n"
        "// Generated by otok; do not edit.\n"
        "\n"
        "import PackageDescription\n"
        "\n"
        "let package = Package(\n"
        "    name: \"Otok\",\n"
        "    products: [\n"
        "        .library(name: \"OpenSSL\", targets: [\"ssl\", \"crypto\", \"OpenSSL_otok\", \"OpenSSLSwift\"]),\n"
        "    ],\n"
        "    targets: [\n"
        "        .binaryTarget(name: \"ssl\", path: \"Packages/openssl/ssl.xcframework\"),\n"
        "        .target(\n"
        "            name: \"OpenSSL_otok\",\n"
        "            path: \"Packages/openssl/shims/OpenSSL\",\n"
        "            resources: [.copy(\"PrivacyInfo.xcprivacy\")],\n"
        "            linkerSettings: [.linkedLibrary(\"z\"), .linkedFramework(\"Security\")]\n"
        "        ),\n"
        "        .target(\n"
        "            name: \"OpenSSLSwift\",\n"
        "            dependencies: [\"ssl\", \"OpenSSL_otok\"],\n"
        "            path: \"Packages/openssl/wrappers/OpenSSLSwift\",\n"
        "            resources: [.process(\"Resources\")],\n"
        "            cSettings: [.define(\"OTOK_OPTION_FAST\"), .define(\"OTOK_OPTION_LEVEL\", to: \"3\")],\n"
        "            swiftSettings: [.define(\"OTOK_OPTION_FAST\")]\n"
        "        ),\n"
        "    ]\n"
        ")\n");
}

TEST(apple_target, writes_swift_packages_of_recipes)
{
    // otok-toml.md 4.6: the package as a dependency, a target per product depending on its product
    swift_package p;
    p.tools_version = "5.9";
    p.products = {{"Updates", {"Updates_otok"}}};
    p.package_paths = {"Packages/updates/updates"};
    swift_target t;
    t.name = "Updates_otok";
    t.path = "Packages/updates/shims/Updates";
    t.product_dependencies = {{"Updates", "updates"}};
    p.targets = {t};
    EXPECT_EQ(package_swift(p),
        "// swift-tools-version:5.9\n"
        "// Generated by otok; do not edit.\n"
        "\n"
        "import PackageDescription\n"
        "\n"
        "let package = Package(\n"
        "    name: \"Otok\",\n"
        "    products: [\n"
        "        .library(name: \"Updates\", targets: [\"Updates_otok\"]),\n"
        "    ],\n"
        "    dependencies: [\n"
        "        .package(path: \"Packages/updates/updates\"),\n"
        "    ],\n"
        "    targets: [\n"
        "        .target(\n"
        "            name: \"Updates_otok\",\n"
        "            dependencies: [.product(name: \"Updates\", package: \"updates\")],\n"
        "            path: \"Packages/updates/shims/Updates\"\n"
        "        ),\n"
        "    ]\n"
        ")\n");

    EXPECT_FALSE(has_package_dependencies("let package = Package(name: \"Updates\", dependencies: [], targets: [])\n"));
    EXPECT_TRUE(has_package_dependencies("dependencies: [.package(url: \"https://github.com/a/b\", from: \"1.0.0\")]\n"));
}

TEST(apple_target, writes_the_cmake_config)
{
    using namespace target::apple::detail;
    EXPECT_EQ(cmake_slice_of("ios-arm64"), "ios");
    EXPECT_EQ(cmake_slice_of("ios-arm64_x86_64-simulator"), "ios-simulator");
    EXPECT_EQ(cmake_slice_of("ios-arm64_x86_64-maccatalyst"), "ios-maccatalyst");
    EXPECT_EQ(cmake_slice_of("watchos-arm64_arm64_32"), "watchos");
    EXPECT_EQ(cmake_slice_of("xros-arm64-simulator"), "xros-simulator");
    EXPECT_EQ(cmake_slice_of("Info.plist"), std::nullopt);
    EXPECT_EQ(cmake_slice_of("ios-arm64-device"), std::nullopt);

    cmake_package p;
    p.slices = {"ios", "ios-simulator", "macos"};
    p.libraries = {{"openssl_crypto", "Packages/openssl/openssl_crypto.xcframework"}, {"openssl_ssl", "Packages/openssl/openssl_ssl.xcframework"}};
    p.products = {{"OpenSSL", {"openssl_ssl", "openssl_crypto"}, {}, {"-lz", "-framework Security"}, {"OPENSSL_X=\"a;b\"", "OPENSSL_Y"}},
        {"H", {}, {"Packages/h/shims/H/include"}, {}, {}}};
    p.privacy_manifests = {"Packages/openssl/shims/OpenSSL/PrivacyInfo.xcprivacy"};
    p.data_dirs = {{"icu4c", "Packages/icu4c/data"}};
    p.warnings = {"the wrappers of the product 'OpenSSL' (W) are SwiftPM targets, which CMake's consumers do not get"};
    EXPECT_EQ(cmake_config(p),
        "# Generated by otok; do not edit.\n"
        "# The products of this output as imported targets for CMake 3.28 or later (otok-toml.md 4):\n"
        "#   find_package(Otok CONFIG REQUIRED PATHS <this directory> NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)\n"
        "#   target_link_libraries(<target> PRIVATE otok::<product>)\n\n"
        "if(CMAKE_VERSION VERSION_LESS 3.28)\n"
        "  message(FATAL_ERROR \"otok: the XCFrameworks of ${CMAKE_CURRENT_LIST_DIR} need CMake 3.28 or later\")\n"
        "endif()\n"
        "if(TARGET otok::OpenSSL)\n  return()\nendif()\n"
        "set(_otok_dir \"${CMAKE_CURRENT_LIST_DIR}\")\n\n"
        "# the platform built for, as the XCFrameworks name it\n"
        "if(CMAKE_SYSTEM_NAME STREQUAL \"Darwin\")\n  set(_otok_slice \"macos\")\n"
        "elseif(CMAKE_SYSTEM_NAME STREQUAL \"iOS\")\n  set(_otok_slice \"ios\")\n"
        "elseif(CMAKE_SYSTEM_NAME STREQUAL \"tvOS\")\n  set(_otok_slice \"tvos\")\n"
        "elseif(CMAKE_SYSTEM_NAME STREQUAL \"watchOS\")\n  set(_otok_slice \"watchos\")\n"
        "elseif(CMAKE_SYSTEM_NAME STREQUAL \"visionOS\")\n  set(_otok_slice \"xros\")\n"
        "else()\n  message(FATAL_ERROR \"otok: ${_otok_dir} holds libraries for Apple's platforms, not for '${CMAKE_SYSTEM_NAME}'\")\nendif()\n"
        "string(TOLOWER \"${CMAKE_OSX_SYSROOT}\" _otok_sysroot)\n"
        "if(NOT _otok_slice STREQUAL \"macos\" AND _otok_sysroot MATCHES \"simulator\")\n  string(APPEND _otok_slice \"-simulator\")\nendif()\n"
        "set(_otok_slices \"ios\" \"ios-simulator\" \"macos\")\n"
        "if(NOT _otok_slice IN_LIST _otok_slices)\n"
        "  string(REPLACE \"-simulator\" \"sim\" _otok_platform \"${_otok_slice}\")\n"
        "  if(_otok_platform STREQUAL \"macos\")\n    set(_otok_platform \"macosx\")\n  endif()\n"
        "  message(FATAL_ERROR \"otok: ${_otok_dir} has no libraries for the platform '${_otok_platform}': add it to 'platforms' of [apple] \"\n"
        "    \"in otok.toml and run otok install\")\n"
        "endif()\n\n"
        "# Xcode picks the slice of the SDK it builds with when the XCFrameworks are in the link phase\n"
        "if(CMAKE_GENERATOR STREQUAL \"Xcode\" AND NOT DEFINED CMAKE_XCODE_LINK_BUILD_PHASE_MODE)\n"
        "  set(CMAKE_XCODE_LINK_BUILD_PHASE_MODE KNOWN_LOCATION)\nendif()\n\n"
        "# the XCFrameworks\n"
        "add_library(otok::openssl_crypto STATIC IMPORTED)\n"
        "set_target_properties(otok::openssl_crypto PROPERTIES IMPORTED_LOCATION \"${_otok_dir}/Packages/openssl/openssl_crypto.xcframework\")\n"
        "add_library(otok::openssl_ssl STATIC IMPORTED)\n"
        "set_target_properties(otok::openssl_ssl PROPERTIES IMPORTED_LOCATION \"${_otok_dir}/Packages/openssl/openssl_ssl.xcframework\")\n\n"
        "# the products\n"
        "add_library(otok::OpenSSL INTERFACE IMPORTED)\n"
        "set_property(TARGET otok::OpenSSL PROPERTY INTERFACE_LINK_LIBRARIES \"otok::openssl_ssl\" \"otok::openssl_crypto\" \"-lz\" \"-framework Security\")\n"
        "set_property(TARGET otok::OpenSSL PROPERTY INTERFACE_COMPILE_DEFINITIONS \"OPENSSL_X=\\\"a\\;b\\\"\" \"OPENSSL_Y\")\n"
        "add_library(otok::H INTERFACE IMPORTED)\n"
        "set_property(TARGET otok::H PROPERTY INTERFACE_INCLUDE_DIRECTORIES \"${_otok_dir}/Packages/h/shims/H/include\")\n\n"
        "# the privacy manifests of the products, which an app puts into its bundle\n"
        "set(Otok_PRIVACY_MANIFESTS \"${_otok_dir}/Packages/openssl/shims/OpenSSL/PrivacyInfo.xcprivacy\")\n\n"
        "# the data files of the libraries (build-lua-api.md 8), and the notices of their licenses (otok-toml.md 4.2)\n"
        "set(Otok_icu4c_DATA_DIR \"${_otok_dir}/Packages/icu4c/data\")\n"
        "set(Otok_ACKNOWLEDGEMENTS \"${_otok_dir}/Acknowledgements.md\")\n\n"
        "# the frameworks of the dynamic libraries in the bundle of a target of the Xcode generator, or on the run path of its build\n"
        "function(otok_embed_frameworks target)\nendfunction()\n\n"
        "# the DLLs a target loads on Windows (otok-toml.md 4.11): nothing on Apple's platforms, so that a project of several targets calls it alike\n"
        "function(otok_copy_runtime_dlls target)\nendfunction()\n\n"
        "message(WARNING \"otok: the wrappers of the product 'OpenSSL' (W) are SwiftPM targets, which CMake's consumers do not get\")\n\n"
        "unset(_otok_dir)\nunset(_otok_slice)\nunset(_otok_sysroot)\nunset(_otok_slices)\nunset(_otok_platform)\n");
}

// The frameworks of dynamic libraries (otok-toml.md 4.10).
TEST(apple_target, describes_frameworks)
{
    using namespace target::apple::detail;
    EXPECT_TRUE(versioned_framework("macosx"));
    EXPECT_TRUE(versioned_framework("catalyst"));
    EXPECT_FALSE(versioned_framework("ios"));
    EXPECT_FALSE(versioned_framework("watchossim"));
    EXPECT_EQ(framework_install_name("tiny-a_tiny_a", false), "@rpath/tiny-a_tiny_a.framework/tiny-a_tiny_a");
    EXPECT_EQ(framework_install_name("tiny-a_tiny_a", true), "@rpath/tiny-a_tiny_a.framework/Versions/A/tiny-a_tiny_a");
    EXPECT_EQ(bundle_identifier("org.otok", "tiny-a", "tiny_a"), "org.otok.tiny-a.tiny-a");
    EXPECT_EQ(bundle_identifier("dev.example", "boost", "unit_test_framework"), "dev.example.boost.unit-test-framework");
    EXPECT_EQ(bundle_identifier("org.otok", "lib+x", "a.b"), "org.otok.lib-x.a.b");
    // App Store takes at most three integers
    EXPECT_EQ(bundle_short_version("1.92.0"), "1.92.0");
    EXPECT_EQ(bundle_short_version("79.1"), "79.1");
    EXPECT_EQ(bundle_short_version("2175"), "2175");
    EXPECT_EQ(bundle_short_version("1.3.1.2"), "1.3.1");
    EXPECT_EQ(bundle_short_version("1.0-beta.2"), "1.0");
    EXPECT_EQ(bundle_short_version("2024-01-05"), "2024");
    EXPECT_EQ(bundle_short_version("1."), "1");
    EXPECT_EQ(bundle_short_version("v2"), "0");
    EXPECT_EQ(supported_platform_of("iossim"), "iPhoneSimulator");
    EXPECT_EQ(supported_platform_of("catalyst"), "MacOSX");
    EXPECT_EQ(supported_platform_of("xros"), "XROS");
    EXPECT_EQ(framework_info_plist({"tiny-a_tiny_a", "org.otok.tiny-a.tiny-a", "1.0", "3", "iPhoneOS", "MinimumOSVersion", "16.0"}),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>CFBundleDevelopmentRegion</key>\n\t<string>en</string>\n"
        "\t<key>CFBundleExecutable</key>\n\t<string>tiny-a_tiny_a</string>\n"
        "\t<key>CFBundleIdentifier</key>\n\t<string>org.otok.tiny-a.tiny-a</string>\n"
        "\t<key>CFBundleInfoDictionaryVersion</key>\n\t<string>6.0</string>\n"
        "\t<key>CFBundleName</key>\n\t<string>tiny-a_tiny_a</string>\n"
        "\t<key>CFBundlePackageType</key>\n\t<string>FMWK</string>\n"
        "\t<key>CFBundleShortVersionString</key>\n\t<string>1.0</string>\n"
        "\t<key>CFBundleSupportedPlatforms</key>\n\t<array>\n\t\t<string>iPhoneOS</string>\n\t</array>\n"
        "\t<key>CFBundleVersion</key>\n\t<string>3</string>\n"
        "\t<key>MinimumOSVersion</key>\n\t<string>16.0</string>\n"
        "</dict>\n</plist>\n");
    // Mac Catalyst: no minimum, its deployment being an iOS version
    EXPECT_EQ(framework_info_plist({"x", "org.otok.x.x", "1", "1", "MacOSX", {}, "16.0"}).find("Minimum"), std::string::npos);
}

// A dynamic library beside a static one (otok-toml.md 4.10): the XCFramework of its headers, no overlap check, the
// warning of a static library held by its products, the privacy manifest in its frameworks rather than a shim.
TEST(apple_target, declares_frameworks_and_their_headers)
{
    test::temp_dir dir;
    recipe rec_a, rec_d;
    product pa, pd;
    pa.name = "A";
    pa.libraries = {"util"};
    pa.headers = "util";
    pd.name = "D";
    pd.libraries = {"core"};
    pd.headers = "core";
    target::apple::product_settings manifest;
    manifest.privacy_manifest = std::string{target::apple::no_privacy_data};
    pd.target_settings["apple"] = manifest;
    std::vector<output_package> packages(2);
    packages[0] = {"a", &rec_a, {"util"}, {{&pa, {{"a", "util"}}, {}}}, {}, {}, "Packages/a"};
    packages[1] = {"d", &rec_d, {"core"}, {{&pd, {{"d", "core"}, {"a", "util"}}, {}}}, {}, {}, "Packages/d", {}, false, "dynamic"};
    dir.write("Packages/d/d_core.xcframework/ios-arm64/d_core.framework/Info.plist", "");
    dir.write("Packages/d/d_core.xcframework/macos-arm64_x86_64/d_core.framework/Versions/A/Resources/Info.plist", "");
    dir.write("Packages/d/d_core_headers.xcframework/ios-arm64/Headers/d/core.h", "");
    dir.write("Packages/d/d_core_headers.xcframework/ios-arm64/libd_core_headers.a", "");
    // the same symbol in both: one image each, no overlap
    dir.write("Packages/a/defined-symbols/util.txt", "_shared\n");
    dir.write("Packages/a/defined-symbols/util.weak.txt", "");
    dir.write("Packages/d/defined-symbols/core.txt", "_shared\n");
    dir.write("Packages/d/defined-symbols/core.weak.txt", "");
    auto target = target::apple::make_build_target("", {});
    std::vector<std::string> warnings;

    auto ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_TRUE(ok) << ok.error().message;
    std::string const swift = read_file(dir.path() / "Package.swift").value_or("?");
    EXPECT_NE(swift.find(".library(name: \"D\", targets: [\"d_core\", \"d_core_headers\", \"a_util\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".binaryTarget(name: \"d_core_headers\", path: \"Packages/d/d_core_headers.xcframework\")"), std::string::npos) << swift;
    EXPECT_EQ(swift.find("D_otok"), std::string::npos) << swift;  // the manifest is in the frameworks
    EXPECT_EQ(warnings, std::vector<std::string>{"the dynamic library 'd' holds a copy of the static library 'a_util' of 'a', which its products hold too: "
                                                 "an app linking them has its code twice; 'linkage = \"dynamic\"' in [deps.a] avoids it"});

    std::string const cmake = read_file(dir.path() / "otok-config.cmake").value_or("?");
    EXPECT_NE(cmake.find("add_library(otok::d_core SHARED IMPORTED)\n"), std::string::npos) << cmake;
    EXPECT_NE(cmake.find("add_library(otok::d_core_headers STATIC IMPORTED)\n"
                         "set_target_properties(otok::d_core_headers PROPERTIES IMPORTED_LOCATION \"${_otok_dir}/Packages/d/d_core_headers.xcframework\")\n"),
        std::string::npos) << cmake;
    EXPECT_NE(cmake.find("set_property(TARGET otok::D PROPERTY INTERFACE_LINK_LIBRARIES \"otok::d_core\" \"otok::d_core_headers\" \"otok::a_util\")\n"),
        std::string::npos) << cmake;
    EXPECT_NE(cmake.find("set(Otok_PRIVACY_MANIFESTS)\n"), std::string::npos) << cmake;
    EXPECT_NE(cmake.find("function(otok_embed_frameworks target)\n"
                         "  if(CMAKE_GENERATOR STREQUAL \"Xcode\")\n"
                         "    set_property(TARGET ${target} APPEND PROPERTY XCODE_EMBED_FRAMEWORKS \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Packages/d/d_core.xcframework\")\n"
                         "    set_property(TARGET ${target} PROPERTY XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON)\n"
                         "    set_property(TARGET ${target} PROPERTY XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS \"$(inherited) @executable_path/Frameworks @executable_path/../Frameworks\")\n"
                         "  elseif(CMAKE_SYSTEM_NAME STREQUAL \"Darwin\")\n"
                         "    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Packages/d/d_core.xcframework/macos-arm64_x86_64\")\n"
                         "  endif()\nendfunction()\n"),
        std::string::npos) << cmake;

    // both static: the overlap is refused
    packages[1].linkage = "static";
    auto overlap = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(overlap);
    EXPECT_NE(overlap.error().message.find("_shared"), std::string::npos) << overlap.error().message;
}

TEST(apple_target, names_xcframeworks_after_their_recipes)
{
    test::temp_dir dir;
    recipe rec_a, rec_b;
    product pa, pb;
    pa.name = "A";
    pa.libraries = {"util"};
    pa.headers = "util";  // the recipe's default
    pb.name = "B";
    pb.libraries = {"util"};
    pb.headers = "util";
    std::vector<output_package> packages(2);
    packages[0] = {"a", &rec_a, {"util"}, {{&pa, {{"a", "util"}}, {}}}, {}, {}, "Packages/a"};
    packages[1] = {"b", &rec_b, {"util"}, {{&pb, {{"b", "util"}, {"a", "util"}}, {}}}, {}, {}, "Packages/b"};
    auto target = target::apple::make_build_target("", {});
    std::vector<std::string> warnings;

    // one output name in two recipes is no clash (docs/PLAN.md decision 30)
    auto ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_TRUE(ok) << ok.error().message;
    std::string const swift = read_file(dir.path() / "Package.swift").value_or("?");
    EXPECT_NE(swift.find(".library(name: \"B\", targets: [\"b_util\", \"a_util\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".binaryTarget(name: \"a_util\", path: \"Packages/a/a_util.xcframework\")"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".binaryTarget(name: \"b_util\", path: \"Packages/b/b_util.xcframework\")"), std::string::npos) << swift;
    // the same for CMake (otok-toml.md 4)
    std::string const cmake = read_file(dir.path() / "otok-config.cmake").value_or("?");
    EXPECT_NE(cmake.find("add_library(otok::a_util STATIC IMPORTED)\n"
                         "set_target_properties(otok::a_util PROPERTIES IMPORTED_LOCATION \"${_otok_dir}/Packages/a/a_util.xcframework\")\n"),
        std::string::npos) << cmake;
    EXPECT_NE(cmake.find("set_property(TARGET otok::B PROPERTY INTERFACE_LINK_LIBRARIES \"otok::b_util\" \"otok::a_util\")\n"), std::string::npos) << cmake;
    EXPECT_EQ(cmake.find("\nset(_otok_slices"), std::string::npos) << cmake;  // no slices to check

    // the slices every XCFramework has
    dir.write("Packages/a/a_util.xcframework/ios-arm64/liba_util.a", "");
    dir.write("Packages/a/a_util.xcframework/ios-arm64_x86_64-simulator/liba_util.a", "");
    dir.write("Packages/b/b_util.xcframework/ios-arm64/libb_util.a", "");
    dir.write("Packages/b/b_util.xcframework/macos-arm64/libb_util.a", "");
    ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_NE(read_file(dir.path() / "otok-config.cmake").value_or("?").find("set(_otok_slices \"ios\")\n"), std::string::npos);

    // a product named as an XCFramework's target
    pb.name = "a_util";
    auto named = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(named);
    EXPECT_EQ(named.error().message, "the product 'a_util' of 'b' has the name of an XCFramework, which is a CMake target too");
    pb.name = "B";

    // one header path in two XCFrameworks: Xcode merges them
    dir.write("Packages/a/a_util.xcframework/ios-arm64/Headers/util/util.h", "a");
    dir.write("Packages/b/b_util.xcframework/ios-arm64/Headers/util/util.h", "b");
    auto headers = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(headers);
    EXPECT_EQ(headers.error().message, "the XCFrameworks 'a_util' and 'b_util' both have the header 'util/util.h', and Xcode merges their headers into one directory");
    std::filesystem::remove_all(dir.path() / "Packages/b");

    // what consumers refer to: modules, wrappers
    pb.name = "A";
    auto module = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(module);
    EXPECT_EQ(module.error().message, "the libraries 'a' and 'b' both have a module named 'A'");
    pb.name = "B";
    target::apple::product_settings wrapped;
    wrapped.wrappers = {"A"};
    pb.target_settings["apple"] = wrapped;
    auto wrapper = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(wrapper);
    EXPECT_EQ(wrapper.error().message, "the target name 'A' is taken by both a module of 'a' and a wrapper of 'b'");
    EXPECT_TRUE(warnings.empty());
}

TEST(apple_target, declares_the_target_of_bundled_data)
{
    // bundle-data (otok-toml.md 4, 6): data/ is a target with the data files as resources, held by the products
    // holding the library's libraries, those of other recipes too
    test::temp_dir dir;
    recipe rec_icu, rec_app;
    product icu, app;
    icu.name = "ICU";
    icu.libraries = {"uc"};
    icu.headers = "uc";
    app.name = "App";
    app.libraries = {"app"};
    app.headers = "app";
    std::vector<output_package> packages(2);
    packages[0] = {"icu-4c", &rec_icu, {"uc"}, {{&icu, {{"icu-4c", "uc"}}, {}}}, {}, {}, "Packages/icu-4c", {"icudt79l.dat"}, true};
    packages[1] = {"app", &rec_app, {"app"}, {{&app, {{"app", "app"}, {"icu-4c", "uc"}}, {}}}, {}, {}, "Packages/app"};
    auto target = target::apple::make_build_target("", {});
    std::vector<std::string> warnings;

    auto ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_TRUE(ok) << ok.error().message;
    std::string const swift = read_file(dir.path() / "Package.swift").value_or("?");
    EXPECT_NE(swift.find(".library(name: \"ICU\", targets: [\"icu-4c_uc\", \"icu_4c_otok_data\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".library(name: \"App\", targets: [\"app_app\", \"icu-4c_uc\", \"icu_4c_otok_data\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find("        .target(\n            name: \"icu_4c_otok_data\",\n            path: \"Packages/icu-4c/data\",\n"
                         "            resources: [.copy(\"icudt79l.dat\")],\n            linkerSettings: [.linkedFramework(\"Foundation\")]\n        ),\n"),
        std::string::npos) << swift;
    EXPECT_TRUE(warnings.empty());
    // CMake's consumers have no bundle: the file warns
    EXPECT_NE(read_file(dir.path() / "otok-config.cmake").value_or("?").find(
                  "message(WARNING \"otok: the data files of 'icu-4c' are a resource bundle of SwiftPM, and CMake's consumers find them in "
                  "Packages/icu-4c/data/, without otok_icu_4c_data_directory()\")\n"),
        std::string::npos)
        << read_file(dir.path() / "otok-config.cmake").value_or("?");

    // nothing to bundle
    packages[0].data.clear();
    ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(read_file(dir.path() / "Package.swift").value_or("?").find("otok_data"), std::string::npos);
    EXPECT_EQ(warnings, std::vector<std::string>{"'bundle-data' of 'icu-4c': its recipe returns no data files"});
}

TEST(apple_target, holds_the_shims_of_products_of_headers_only)
{
    test::temp_dir dir;
    recipe rec_h, rec_u;
    product headers, compiled, user;
    headers.name = "H";               // libraries = []: headers only
    compiled.name = "HCompiled";
    compiled.libraries = {"compiled"};
    compiled.depends = {"H"};
    user.name = "U";
    user.libraries = {"u"};
    user.headers = "u";
    std::vector<output_package> packages(2);
    packages[0] = {"h", &rec_h, {"compiled"}, {{&headers, {}, {}, {{"h", "H"}}}, {&compiled, {{"h", "compiled"}}, {}, {{"h", "H"}}}}, {}, {}, "Packages/h"};
    // a recipe depending on h holds its headers too
    packages[1] = {"u", &rec_u, {"u"}, {{&user, {{"u", "u"}, {"h", "compiled"}}, {}, {{"h", "H"}}}}, {}, {}, "Packages/u"};
    dir.write("Packages/h/shims/H/include/h/h.h", "");
    dir.write("Packages/h/shims/H/include/H_otok.h", "");
    dir.write("Packages/h/shims/H/include/module.modulemap", "");
    auto target = target::apple::make_build_target("", {});
    std::vector<std::string> warnings;

    auto ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_TRUE(ok) << ok.error().message;
    std::string const swift = read_file(dir.path() / "Package.swift").value_or("?");
    EXPECT_NE(swift.find(".library(name: \"H\", targets: [\"H_otok\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".library(name: \"HCompiled\", targets: [\"h_compiled\", \"H_otok\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".library(name: \"U\", targets: [\"u_u\", \"h_compiled\", \"H_otok\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find("        .target(\n            name: \"H_otok\",\n            path: \"Packages/h/shims/H\"\n        ),\n"), std::string::npos) << swift;
    // CMake: the shim's include/ is an include directory of the products holding it
    std::string const cmake = read_file(dir.path() / "otok-config.cmake").value_or("?");
    EXPECT_NE(cmake.find("add_library(otok::H INTERFACE IMPORTED)\n"
                         "set_property(TARGET otok::H PROPERTY INTERFACE_INCLUDE_DIRECTORIES \"${_otok_dir}/Packages/h/shims/H/include\")\n"),
        std::string::npos) << cmake;
    EXPECT_NE(cmake.find("set_property(TARGET otok::U PROPERTY INTERFACE_LINK_LIBRARIES \"otok::u_u\" \"otok::h_compiled\")\n"
                         "set_property(TARGET otok::U PROPERTY INTERFACE_INCLUDE_DIRECTORIES \"${_otok_dir}/Packages/h/shims/H/include\")\n"),
        std::string::npos) << cmake;

    // the same header in an XCFramework
    dir.write("Packages/u/u_u.xcframework/ios-arm64/Headers/h/h.h", "");
    auto twice = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().message, "the product 'H' and the XCFramework 'u_u' both have the header 'h/h.h', and a consumer would see one of them");
}

TEST(apple_target, reads_the_required_reason_api_list)
{
    auto const& builtin = target::apple::builtin_required_reason_apis();
    ASSERT_EQ(builtin.size(), 5u);
    EXPECT_EQ(builtin[0].category, "NSPrivacyAccessedAPICategoryFileTimestamp");
    EXPECT_EQ(builtin[4].classes, std::vector<std::string>{"NSUserDefaults"});

    auto format = target::apple::parse_required_reason_apis("format = 2\napi = []\n", "r.toml");
    ASSERT_FALSE(format);
    EXPECT_EQ(format.error().message, "r.toml:1:10: error: 'format' at the top level is 2; this otok reads format 1");
    auto twice = target::apple::parse_required_reason_apis(
        "format = 1\n[[api]]\ncategory = \"A\"\n[[api]]\ncategory = \"A\"\nsymbols = [\"stat\"]\n", "r.toml");
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().message, "r.toml:5:12: error: the category 'A' appears twice");
}

TEST(apple_target, reads_privacy_manifests)
{
    std::string const manifest = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>NSPrivacyTracking</key>
	<false/>
	<key>NSPrivacyTrackingDomains</key>
	<array/>
	<!-- <key>NSPrivacyAccessedAPITypes</key> -->
	<key>NSPrivacyAccessedAPITypes</key>
	<array>
		<dict>
			<key>NSPrivacyAccessedAPIType</key>
			<string>NSPrivacyAccessedAPICategoryFileTimestamp</string>
			<key>NSPrivacyAccessedAPITypeReasons</key>
			<array><string>C617.1</string></array>
		</dict>
		<dict>
			<key>NSPrivacyAccessedAPITypeReasons</key>
			<array><string>&#x33;5F9.1</string></array>
			<key>NSPrivacyAccessedAPIType</key>
			<string>NSPrivacyAccessedAPICategory&#83;ystemBootTime</string>
		</dict>
	</array>
</dict>
</plist>
)";
    auto categories = target::apple::declared_api_categories(manifest);
    ASSERT_TRUE(categories) << categories.error().message;
    EXPECT_EQ(*categories, (std::set<std::string>{"NSPrivacyAccessedAPICategoryFileTimestamp", "NSPrivacyAccessedAPICategorySystemBootTime"}));

    auto none = target::apple::declared_api_categories("<plist><dict><key>NSPrivacyTracking</key><true/></dict></plist>");
    ASSERT_TRUE(none) << none.error().message;
    EXPECT_TRUE(none->empty());

    auto binary = target::apple::declared_api_categories(std::string{"bplist00\xd1\x01\x02", 11});
    ASSERT_FALSE(binary);
    EXPECT_EQ(binary.error().message, "it is a binary property list; otok reads XML ones");
    auto untyped = target::apple::declared_api_categories("<plist><dict><key>NSPrivacyAccessedAPITypes</key><array><dict/></array></dict></plist>");
    ASSERT_FALSE(untyped);
    EXPECT_EQ(untyped.error().message, "an item of NSPrivacyAccessedAPITypes has no NSPrivacyAccessedAPIType string");
    auto broken = target::apple::declared_api_categories("<plist><dict><key>A</key><string>x</dict></plist>");
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().message, "</string> is missing");

    // privacy-manifest = "none" (recipe-toml.md 11): a valid manifest declaring nothing
    auto generated = target::apple::declared_api_categories(no_privacy_data_manifest);
    ASSERT_TRUE(generated) << generated.error().message;
    EXPECT_TRUE(generated->empty());
}

TEST(apple_target, matches_undefined_symbols_with_required_reason_apis)
{
    std::string const nm = "\nlibx.a(a.o):\n_stat$INODE64\n_memcpy\n\nlibx.a(b.o):\n_getattrlist\n_OBJC_CLASS_$_NSUserDefaults\n_stat\n"
                           "_mach_absolute_time\n";
    std::vector<std::string> const undefined = target::apple::parse_undefined_symbols(nm);
    EXPECT_EQ(undefined, (std::vector<std::string>{"_OBJC_CLASS_$_NSUserDefaults", "_getattrlist", "_mach_absolute_time", "_memcpy", "_stat",
        "_stat$INODE64"}));
    auto const& apis = target::apple::builtin_required_reason_apis();

    auto uses = target::apple::undeclared_uses(undefined, apis, {"NSPrivacyAccessedAPICategorySystemBootTime"});
    ASSERT_EQ(uses.size(), 3u);
    EXPECT_EQ(uses[0].categories, (std::vector<std::string>{"NSPrivacyAccessedAPICategoryDiskSpace", "NSPrivacyAccessedAPICategoryFileTimestamp"}));
    EXPECT_EQ(uses[0].symbols, std::vector<std::string>{"getattrlist"});
    EXPECT_EQ(uses[1].categories, std::vector<std::string>{"NSPrivacyAccessedAPICategoryFileTimestamp"});
    EXPECT_EQ(uses[1].symbols, std::vector<std::string>{"stat"});
    EXPECT_EQ(uses[2].categories, std::vector<std::string>{"NSPrivacyAccessedAPICategoryUserDefaults"});
    EXPECT_EQ(uses[2].symbols, std::vector<std::string>{"NSUserDefaults"});

    // one of getattrlist's categories declares it
    uses = target::apple::undeclared_uses(undefined, apis,
        {"NSPrivacyAccessedAPICategorySystemBootTime", "NSPrivacyAccessedAPICategoryFileTimestamp", "NSPrivacyAccessedAPICategoryUserDefaults"});
    EXPECT_TRUE(uses.empty());
}

TEST(apple_target, reads_defined_symbols)
{
    // as nm -g -U -m prints an archive (Xcode 27)
    std::string const nm = "\nlibx.a(a.o):\n"
                           "0000000000000010 (__TEXT,__text) weak private external __Z11weak_hiddenv\n"
                           "000000000000006c (__TEXT,__text) weak external __Z3inlv\n"
                           "0000000000000018 (__TEXT,__text) external __Z3usev\n"
                           "0000000000000008 (__TEXT,__text) private external __Z9hidden_fnv\n"
                           "0000000000000020 (__TEXT,__text) weak external automatically hidden __Z4autov\n"
                           "0000000000000030 (__TEXT,__text) external [no dead strip] _kept\n"
                           "0000000000000098 (__DATA,__data) weak external _weak_c\n"
                           "\nlibx.a(b.o):\n"
                           "0000000000000004 (common) (alignment 2^2) external _common_var\n"
                           "0000000000000018 (__TEXT,__text) external __Z3usev\n";
    auto const symbols = target::apple::parse_defined_symbols(nm);
    EXPECT_EQ(symbols.all, (std::vector<std::string>{"__Z11weak_hiddenv", "__Z3inlv", "__Z3usev", "__Z4autov", "__Z9hidden_fnv", "_common_var",
        "_kept", "_weak_c"}));
    EXPECT_EQ(symbols.weak, (std::vector<std::string>{"__Z11weak_hiddenv", "__Z3inlv", "__Z4autov", "_common_var", "_weak_c"}));
}

TEST(apple_target, refuses_symbols_two_libraries_define)
{
    test::temp_dir dir;
    recipe rec_a, rec_b;
    product a, b;
    a.name = "A";
    a.libraries = {"a", "a2"};
    b.name = "B";
    b.libraries = {"b"};
    std::vector<output_package> packages(2);
    packages[0] = {"pa", &rec_a, {"a", "a2"}, {{&a, {{"pa", "a"}, {"pa", "a2"}}, {}}}, {}, {}, "Packages/pa"};
    packages[1] = {"pb", &rec_b, {"b"}, {{&b, {{"pb", "b"}}, {}}}, {}, {}, "Packages/pb"};
    dir.write("Packages/pa/defined-symbols/a.txt", "__Z3inlv\n_deflate\n_inflate\n_own_a\n");
    dir.write("Packages/pa/defined-symbols/a.weak.txt", "__Z3inlv\n");
    dir.write("Packages/pa/defined-symbols/a2.txt", "_own_a2\n");
    dir.write("Packages/pa/defined-symbols/a2.weak.txt", "");
    dir.write("Packages/pb/defined-symbols/b.txt", "__Z3inlv\n_deflate\n_inflate\n_own_a2\n_weak_here\n");
    dir.write("Packages/pb/defined-symbols/b.weak.txt", "__Z3inlv\n_own_a2\n");
    auto target = target::apple::make_build_target("", {});
    std::vector<std::string> warnings;

    // weak in both or in one of them is no overlap
    auto ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(ok);
    EXPECT_EQ(ok.error().message, "the libraries 'a' of 'pa' and 'b' of 'pb' both define 2 symbols: _deflate, _inflate; an app linking both fails to "
        "link or gets one definition for every caller");

    // two output names of one recipe, and more than ten symbols
    std::string many;
    for (int i = 10; i < 22; ++i) many += "_s" + std::to_string(i) + "\n";
    dir.write("Packages/pa/defined-symbols/a.txt", many);
    dir.write("Packages/pa/defined-symbols/a2.txt", many);
    dir.write("Packages/pb/defined-symbols/b.txt", "_b\n");
    ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(ok);
    EXPECT_EQ(ok.error().message, "the libraries 'a' of 'pa' and 'a2' of 'pa' both define 12 symbols: _s10, _s11, _s12, _s13, _s14, _s15, _s16, "
        "_s17, _s18, _s19 and 2 more; an app linking both fails to link or gets one definition for every caller");

    dir.write("Packages/pa/defined-symbols/a2.txt", "_a2\n");
    ok = target->finish_output(dir.path(), packages, {}, warnings);
    EXPECT_TRUE(ok) << ok.error().message;
}

TEST(apple_target, allows_symbols_of_products_that_exclude_each_other)
{
    test::temp_dir dir;
    // Boost.Test: two variants sharing a library, and a product of its own beside them
    recipe rec;
    rec.products.resize(4);
    rec.products[0].name = "Utf";
    rec.products[0].libraries = {"utf", "exception"};
    rec.products[0].alternatives = "test";
    rec.products[1].name = "Tem";
    rec.products[1].libraries = {"tem", "exception"};
    rec.products[1].alternatives = "test";
    rec.products[2].name = "UsesTem";
    rec.products[2].libraries = {"uses"};
    rec.products[2].depends = {"Tem"};
    rec.products[3].name = "Other";
    rec.products[3].libraries = {"other"};
    std::vector<output_package> packages(1);
    packages[0] = {"boost", &rec, {"utf", "tem", "exception", "uses", "other"}, {}, {}, {}, "Packages/boost"};
    for (product const& p : rec.products) {
        output_product o{&p, {}, {}};
        for (std::string const& l : p.libraries) o.libraries.push_back({"boost", l});
        packages[0].products.push_back(std::move(o));
    }
    dir.write("Packages/boost/defined-symbols/utf.txt", "_framework\n_monitor\n_utf_only\n");
    dir.write("Packages/boost/defined-symbols/tem.txt", "_framework\n_monitor\n_test_main\n");
    dir.write("Packages/boost/defined-symbols/exception.txt", "_exception\n");
    dir.write("Packages/boost/defined-symbols/other.txt", "_other\n");
    auto target = target::apple::make_build_target("", {});
    std::vector<std::string> warnings;

    // the variants, and a product holding one of them through depends, never meet the other
    dir.write("Packages/boost/defined-symbols/uses.txt", "_uses\n_utf_only\n");
    auto ok = target->finish_output(dir.path(), packages, {}, warnings);
    EXPECT_TRUE(ok) << ok.error().message;

    // but meet what they hold
    dir.write("Packages/boost/defined-symbols/uses.txt", "_uses\n_test_main\n");
    ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(ok);
    EXPECT_EQ(ok.error().message, "the libraries 'tem' of 'boost' and 'uses' of 'boost' both define 1 symbol: _test_main; an app linking both fails "
        "to link or gets one definition for every caller");

    // a product beside them meets both
    dir.write("Packages/boost/defined-symbols/uses.txt", "_uses\n");
    dir.write("Packages/boost/defined-symbols/other.txt", "_other\n_monitor\n");
    ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_FALSE(ok);
    EXPECT_EQ(ok.error().message, "the libraries 'tem' of 'boost' and 'other' of 'boost' both define 1 symbol: _monitor; the libraries 'utf' of 'boost' "
        "and 'other' of 'boost' both define 1 symbol: _monitor; an app linking both fails to link or gets one definition for every caller");
}

TEST(apple_target, warns_of_undeclared_required_reason_apis)
{
    test::temp_dir dir;
    recipe rec;
    product declared, silent;
    declared.name = "A";
    declared.libraries = {"a"};
    target::apple::product_settings with_manifest;
    with_manifest.privacy_manifest = "PrivacyInfo.xcprivacy";
    declared.target_settings["apple"] = with_manifest;
    silent.name = "B";
    silent.libraries = {"b"};
    std::vector<output_package> packages(1);
    packages[0] = {"p", &rec, {"a", "b"}, {{&declared, {{"p", "a"}}, {}}, {&silent, {{"p", "b"}}, {}}}, {}, {}, "Packages/p"};
    dir.write("Packages/p/undefined-symbols/a.txt", "_mach_absolute_time\n_stat\n");
    dir.write("Packages/p/undefined-symbols/b.txt", "_memcpy\n_statfs$INODE64\n");
    dir.write("Packages/p/shims/A/PrivacyInfo.xcprivacy", "<plist><dict><key>NSPrivacyAccessedAPITypes</key><array><dict>"
        "<key>NSPrivacyAccessedAPIType</key><string>NSPrivacyAccessedAPICategoryFileTimestamp</string></dict></array></dict></plist>");
    auto target = target::apple::make_build_target("", {});
    std::vector<std::string> warnings;

    auto ok = target->finish_output(dir.path(), packages, {}, warnings);
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(warnings, (std::vector<std::string>{
        "the library 'a' of 'p' uses required-reason APIs that no privacy manifest of its products declares: mach_absolute_time "
        "(NSPrivacyAccessedAPICategorySystemBootTime)",
        "the library 'b' of 'p' uses required-reason APIs that need a privacy manifest, which none of its products has: statfs "
        "(NSPrivacyAccessedAPICategoryDiskSpace)"}));
}

TEST(apple_target, reads_the_private_library_list)
{
    // the built-in copy is the example of the format document
    std::string const doc = read_file(path_from_utf8(OTOK_TEST_DATA_DIR "/../../docs/formats/index.md")).value_or("");
    std::size_t const begin = doc.find("```toml\n", doc.find("### 10.2"));
    ASSERT_NE(begin, std::string::npos);
    std::string const example = doc.substr(begin + 8, doc.find("```", begin + 8) - begin - 8);
    auto parsed = target::apple::parse_private_libraries(example, "index.md");
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto const& builtin = target::apple::builtin_private_libraries();
    ASSERT_EQ(builtin.size(), parsed->size());
    ASSERT_EQ(builtin.size(), 1u);
    EXPECT_EQ(builtin[0].path, (*parsed)[0].path);
    EXPECT_EQ(builtin[0].path, "usr/lib/libicucore.tbd");
    EXPECT_EQ(builtin[0].name, "ICU (libicucore)");

    auto missing = target::apple::parse_private_libraries("format = 1\n[[library]]\nname = \"X\"\n", "p.toml");
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().message.find("'path'"), std::string::npos) << missing.error().message;
}

TEST(apple_target, reads_text_stubs)
{
    std::string const tbd = "--- !tapi-tbd\ntbd-version:     4\ntargets:         [ arm64-ios ]\ninstall-name:    '/usr/lib/libx.dylib'\n"
                            "exports:\n  - targets:         [ arm64-ios ]\n    symbols:         [ _a, _b, \n                       '_c$INODE64', _d ]\n"
                            "    objc-classes:    [ NSThing ]\n    weak-symbols:    [ _w ]\n"
                            "reexports:\n  - targets:         [ arm64-ios ]\n    symbols:         [ _r ]\n"
                            "undefineds:\n  - targets:         [ arm64-ios ]\n    symbols:         [ _u ]\n...\n";
    auto exports = target::apple::parse_tbd_exports(tbd);
    ASSERT_TRUE(exports) << exports.error().message;
    EXPECT_EQ(*exports, (std::set<std::string>{"_a", "_b", "_c$INODE64", "_d", "_w", "_r"}));
    auto json = target::apple::parse_tbd_exports("{\"tapi_tbd_version\": 5}");
    ASSERT_FALSE(json);
    EXPECT_EQ(json.error().message, "it is not a text stub in the YAML format (tbd version 4)");
}

#ifdef __APPLE__
TEST(apple_target, warns_of_symbols_of_private_libraries)
{
    test::temp_dir dir;
    recipe rec;
    product icu;
    icu.name = "ICU";
    icu.libraries = {"icuuc"};
    std::vector<output_package> packages(1);
    packages[0] = {"icu4c", &rec, {"icuuc"}, {{&icu, {{"icu4c", "icuuc"}}, {}}}, {}, {}, "Packages/icu4c"};
    dir.write("Packages/icu4c/defined-symbols/icuuc.txt", "_my_own\n_ubrk_next\n_unorm_normalize\n");
    auto target = target::apple::make_build_target("", current_environment());
    std::vector<std::string> warnings;
    auto ok = target->finish_output(dir.path(), packages, {{"ios", "15.0"}}, warnings);
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(warnings, (std::vector<std::string>{
        "the library 'icuuc' of 'icu4c' defines 2 symbols that ICU (libicucore) exports, a private system library, and App Store review "
        "rejects such an app (ITMS-90338): _ubrk_next, _unorm_normalize"}));
}
#endif

TEST(apple_target, writes_acknowledgements_plist)
{
    recipe a, b;
    a.license_text = "MIT";
    b.license_text = "Zlib AND BSD-3-Clause";
    std::vector<output_package> packages(2);
    packages[0].library = "tiny-a";
    packages[0].rec = &a;
    packages[0].licenses = {{"LICENSE", "Copyright <a> & b\n"}};
    packages[1].library = "zlib";
    packages[1].rec = &b;
    packages[1].licenses = {{"LICENSE", "one\n\n"}, {"doc/COPYING", "two"}};
    EXPECT_EQ(acknowledgements_plist(packages),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n\t<key>PreferenceSpecifiers</key>\n\t<array>\n"
        "\t\t<dict>\n\t\t\t<key>FooterText</key>\n\t\t\t<string>This product includes the following libraries.</string>\n"
        "\t\t\t<key>Title</key>\n\t\t\t<string>Acknowledgements</string>\n\t\t\t<key>Type</key>\n\t\t\t<string>PSGroupSpecifier</string>\n\t\t</dict>\n"
        "\t\t<dict>\n\t\t\t<key>FooterText</key>\n\t\t\t<string>Copyright &lt;a&gt; &amp; b</string>\n"
        "\t\t\t<key>License</key>\n\t\t\t<string>MIT</string>\n"
        "\t\t\t<key>Title</key>\n\t\t\t<string>tiny-a</string>\n\t\t\t<key>Type</key>\n\t\t\t<string>PSGroupSpecifier</string>\n\t\t</dict>\n"
        "\t\t<dict>\n\t\t\t<key>FooterText</key>\n\t\t\t<string>one\n\ntwo</string>\n"
        "\t\t\t<key>License</key>\n\t\t\t<string>Zlib AND BSD-3-Clause</string>\n"
        "\t\t\t<key>Title</key>\n\t\t\t<string>zlib</string>\n\t\t\t<key>Type</key>\n\t\t\t<string>PSGroupSpecifier</string>\n\t\t</dict>\n"
        "\t</array>\n\t<key>StringsTable</key>\n\t<string>Acknowledgements</string>\n\t<key>Title</key>\n\t<string>Acknowledgements</string>\n"
        "</dict>\n</plist>\n");
}

#ifdef __APPLE__

namespace {

// The tiny project of tests/data/projects for every Apple platform, installed by otok commands with
// the real Apple target.
std::string const apple_manifest =
    "[otok]\nformat = 1\n\n[apple]\n"
    "platforms = [\"ios\", \"iossim\", \"macosx\", \"catalyst\", \"tvos\", \"tvossim\", \"watchos\", \"watchossim\", \"xros\", \"xrossim\"]\n"
    // Apple Watch's arm64 needs watchOS 26: clang raises the version, and otok's check refuses the library
    "architectures = { watchos = [\"arm64_32\"] }\n"
    "deployment = { ios = \"16.0\", macos = \"13.0\", tvos = \"16.0\", watchos = \"9.0\", visionos = \"1.0\" }\n"
    "cflags = [\"-DTINY_BONUS=2\"]\n\n"
    "[deps.tiny-a]\nrecipe = { path = \"recipes/tiny-a\" }\nsource = { path = \"vendor/tiny-a\" }\noptions = { value = 7, fast = true }\n\n"
    "[deps.tiny-b]\nrecipe = { path = \"recipes/tiny-b\" }\nsource = { path = \"vendor/tiny-b\" }\n";

// A log with every run of white space as one space: CMake wraps the lines of warnings and errors.
std::string squeezed(std::string const& text)
{
    std::string out;
    for (char c : text) {
        bool const space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
        if (!space) out += c;
        else if (!out.empty() && out.back() != ' ') out += ' ';
    }
    return out;
}

}

TEST(apple_target, installs_every_platform)
{
    auto target = target::apple::make_build_target("", current_environment());
    if (auto p = target->profile({"iossim", "arm64", "16.0"}); !p) GTEST_SKIP() << "no Xcode here: " << p.error().message;

    test::temp_dir dir;
    std::filesystem::path const base = std::filesystem::canonical(dir.path());
    std::filesystem::path const project = base / "project";
    ASSERT_TRUE(copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/tiny"), project));
    dir.write("project/otok.toml", apple_manifest);

    std::ostringstream out, err;
    auto parsed = parse_command_line(std::vector<std::string>{"install"}, global_options(), command_specs());
    ASSERT_TRUE(parsed);
    std::filesystem::path const cache = base / "cache";
    command_context ctx{project, all_targets, nullptr, out, err, [cache](std::string_view name) -> std::optional<std::string> {
        if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache);
        if (name == "PATH") return process_environment()(name);  // the toolchains (docs/PLAN.md decision 38)
        return std::nullopt;
    }};
    ctx.make_build_targets = [](machine_config const& config) {
        std::vector<std::unique_ptr<build_target>> targets;
        targets.push_back(target::apple::make_build_target(config.xcode, current_environment()));
        return targets;
    };
    ASSERT_EQ(run_command(*parsed, ctx), 0) << out.str() << err.str();
    EXPECT_NE(out.str().find("slices: 30 (0 from the cache)\n"), std::string::npos) << out.str() << err.str();
    EXPECT_NE(out.str().find("output 'Otok': 2 copied, 0 unchanged, 0 removed\n"), std::string::npos) << out.str();
    // tiny-b calls mach_absolute_time without a privacy manifest (otok-toml.md 4.4)
    EXPECT_NE(err.str().find("warning: the output 'Otok': the library 'tiny_b' of 'tiny-b' uses required-reason APIs that need a privacy manifest, "
                             "which none of its products has: mach_absolute_time (NSPrivacyAccessedAPICategorySystemBootTime)\n"),
        std::string::npos) << err.str();

    // one package: the XCFrameworks and Package.swift (otok-toml.md 4)
    std::filesystem::path const otok_dir = project / "Otok";
    EXPECT_TRUE(std::filesystem::exists(otok_dir / "Packages" / "tiny-a" / "tiny-a_tiny_a.xcframework" / "Info.plist"));
    std::string const swift = read_file(otok_dir / "Package.swift").value_or("?");
    // TinyB holds tiny_a, so it holds the shim of TinyA with its link settings
    EXPECT_NE(swift.find(".library(name: \"TinyB\", targets: [\"tiny-b_tiny_b\", \"tiny-a_tiny_a\", \"TinyA_otok\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".library(name: \"TinyA\", targets: [\"tiny-a_tiny_a\", \"TinyA_otok\", \"TinyASwift\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".binaryTarget(name: \"tiny-a_tiny_a\", path: \"Packages/tiny-a/tiny-a_tiny_a.xcframework\")"), std::string::npos) << swift;
    EXPECT_NE(swift.find("        .target(\n            name: \"TinyA_otok\",\n            path: \"Packages/tiny-a/shims/TinyA\",\n"
                         "            resources: [.copy(\"PrivacyInfo.xcprivacy\")],\n"
                         "            linkerSettings: [.linkedLibrary(\"z\"), .linkedLibrary(\"c++\"), .linkedFramework(\"CoreFoundation\")]\n        ),\n"),
        std::string::npos)
        << swift;
    EXPECT_NE(swift.find("        .target(\n            name: \"TinyASwift\",\n            dependencies: [\"tiny-a_tiny_a\", \"TinyA_otok\"],\n"
                         "            path: \"Packages/tiny-a/wrappers/TinyASwift\",\n            resources: [.process(\"Resources\")],\n"
                         "            cSettings: [.define(\"OTOK_OPTION_VALUE\", to: \"7\"), .define(\"OTOK_OPTION_FAST\")],\n"
                         "            swiftSettings: [.define(\"OTOK_OPTION_FAST\")]\n        ),\n"),
        std::string::npos)
        << swift;
    EXPECT_TRUE(std::filesystem::exists(otok_dir / "Packages" / "tiny-a" / "shims" / "TinyA" / "PrivacyInfo.xcprivacy"));
    EXPECT_TRUE(std::filesystem::exists(otok_dir / "Packages" / "tiny-a" / "licenses" / "LICENSE"));
    EXPECT_NE(read_file(otok_dir / "Acknowledgements.md").value_or("?").find("\n## tiny-b 1.0\n\nLicense: MIT\n"), std::string::npos);
    EXPECT_NE(read_file(otok_dir / "Acknowledgements.plist").value_or("?").find("<string>tiny-a</string>"), std::string::npos);
    EXPECT_NE(swift.find("platforms: [.iOS(\"16.0\"), .macOS(\"13.0\"), .tvOS(\"16.0\"), .watchOS(\"9.0\"), .visionOS(\"1.0\")]"), std::string::npos)
        << swift;
    std::size_t maps = 0;
    for (auto const& e : std::filesystem::recursive_directory_iterator{otok_dir / "Packages" / "tiny-b" / "tiny-b_tiny_b.xcframework"}) {
        if (e.path().filename() == "module.modulemap") {
            ++maps;
            EXPECT_EQ(e.path().parent_path().filename(), "TinyB");
            EXPECT_EQ(read_file(e.path()).value_or("?"), "// Generated by otok.\nmodule TinyB {\n    header \"../tiny_b.h\"\n    export *\n}\n");
        }
    }
    EXPECT_EQ(maps, 10u);  // one per platform
    // the product's defines come first in TinyA; tiny_a_arch.h differs between architectures, so the
    // platforms with two keep a copy per architecture (otok-toml.md 4)
    maps = 0;
    std::size_t dispatched = 0;
    for (auto const& e : std::filesystem::recursive_directory_iterator{otok_dir / "Packages" / "tiny-a" / "tiny-a_tiny_a.xcframework"}) {
        if (e.path().filename() != "module.modulemap") continue;
        ++maps;
        std::filesystem::path const headers = e.path().parent_path().parent_path();
        bool const two = path_to_utf8(headers.parent_path().filename()).find("arm64_x86_64") != std::string::npos;
        std::string const textual = two ? "    textual header \"../otok-arch/arm64/tiny_a_arch.h\"\n    textual header \"../otok-arch/x86_64/tiny_a_arch.h\"\n" : "";
        EXPECT_EQ(read_file(e.path()).value_or("?"), "// Generated by otok.\nmodule TinyA {\n    header \"otok_defines.h\"\n    header \"../tiny_a.h\"\n"
                                                     "    header \"../tiny_a_arch.h\"\n" + textual + "    export *\n}\n");
        EXPECT_NE(read_file(e.path().parent_path() / "otok_defines.h").value_or("?").find("#define TINY_A_EXTRA 5\n"), std::string::npos);
        if (two) {
            ++dispatched;
            EXPECT_NE(read_file(headers / "tiny_a_arch.h").value_or("?").find("#include \"otok-arch/x86_64/tiny_a_arch.h\"\n"), std::string::npos);
            EXPECT_EQ(read_file(headers / "otok-arch" / "x86_64" / "tiny_a_arch.h").value_or("?"), "#define TINY_A_ARCH \"x86_64\"\n");
        }
    }
    EXPECT_EQ(maps, 10u);
    EXPECT_EQ(dispatched, 5u);  // iossim, macosx, catalyst, tvossim, watchossim

    // a Swift package that uses it, as an app would: imports the modules and calls the libraries
    std::filesystem::path const consumer = base / "consumer";
    dir.write("consumer/Package.swift",
        "// swift-tools-version:5.9\nimport PackageDescription\n\nlet package = Package(\n    name: \"Consumer\",\n"
        "    platforms: [.macOS(\"13.0\")],\n    dependencies: [.package(path: \"../project/Otok\")],\n"
        "    targets: [.executableTarget(name: \"Consumer\", dependencies: [.product(name: \"TinyB\", package: \"Otok\"), "
        ".product(name: \"TinyA\", package: \"Otok\")])]\n)\n");
    dir.write("consumer/Sources/Consumer/main.swift", "import TinyA\nimport TinyB\nimport TinyASwift\n\nprint(tiny_b(), tiny_a_extra(), tinyAReport(), TINY_A_ARCH)\n");
    env_vars env = current_environment();
    auto ran = run_process({{"/usr/bin/xcrun", "swift", "run", "--package-path", path_to_utf8(consumer), "-q", "Consumer"}, env, consumer,
        base / "swift.log", true});
    ASSERT_TRUE(ran) << ran.error().message;
    EXPECT_TRUE(ran->success()) << read_file(base / "swift.log").value_or("");
    // tiny_b = tiny_a + 1 + 2; the wrapper: tiny_a = 7 + 2 (the value option, the target's -DTINY_BONUS=2), the
    // product's define, tiny_a_zlib through the shim's -lz, OTOK_OPTION_FAST, its resource; this Mac's
    // architecture through the generated tiny_a_arch.h
#if defined(__aarch64__)
    std::string const arch = "arm64";
#else
    std::string const arch = "x86_64";
#endif
    EXPECT_EQ(ran->output, "12 5 9 5 1 fast hello " + arch + "\n") << read_file(base / "swift.log").value_or("");
    // the privacy manifest reaches the build as a resource of the shim
    bool manifest = false;
    for (auto const& e : std::filesystem::recursive_directory_iterator{consumer / ".build"}) {
        if (e.path().filename() == "PrivacyInfo.xcprivacy") manifest = true;
    }
    EXPECT_TRUE(manifest);

    // a CMake project that uses it (otok-toml.md 4): TinyB holds tiny_a, so the define of TinyA's module and its
    // link settings come along; CMake picks the slice of the platform built for
    dir.write("cmake-consumer/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.28)\nproject(consumer C)\n"
        "find_package(Otok CONFIG REQUIRED PATHS \"${CMAKE_CURRENT_SOURCE_DIR}/../project/Otok\" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)\n"
        "if(NOT CMAKE_SYSTEM_NAME STREQUAL \"Darwin\")\n  set(CMAKE_MACOSX_BUNDLE ON)\nendif()\n"
        "add_executable(consumer main.c)\nset_target_properties(consumer PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER org.otok.consumer)\n"
        "target_link_libraries(consumer PRIVATE otok::TinyB)\n"
        "list(LENGTH Otok_PRIVACY_MANIFESTS manifests)\nmessage(STATUS \"privacy manifests: ${manifests}\")\n");
    dir.write("cmake-consumer/main.c", "#include <stdio.h>\n#include <tiny_a.h>\n#include <tiny_b.h>\n\n"
                                       "int main(void)\n{\n    printf(\"%d %d %d %d %s\\n\", tiny_b(), tiny_a(), tiny_a_extra(), tiny_a_zlib(), TINY_A_ARCH);\n    return 0;\n}\n");
    auto const cmake = [&](std::vector<std::string> args, std::string const& log) {
        args.insert(args.begin(), "cmake");
        auto done = run_process({args, env, base, base / log});
        return done && done->success();
    };
    std::string const source = path_to_utf8(base / "cmake-consumer");
    ASSERT_TRUE(cmake({"-S", source, "-B", path_to_utf8(base / "cmake-macos"), "-G", "Ninja"}, "cmake-macos.log"))
        << read_file(base / "cmake-macos.log").value_or("");
    std::string const configured = squeezed(read_file(base / "cmake-macos.log").value_or("?"));
    EXPECT_NE(configured.find("otok: the wrappers of the product 'TinyA' (TinyASwift) are SwiftPM targets"), std::string::npos) << configured;
    EXPECT_NE(configured.find("-- privacy manifests: 1 "), std::string::npos) << configured;
    ASSERT_TRUE(cmake({"--build", path_to_utf8(base / "cmake-macos")}, "cmake-macos.log")) << read_file(base / "cmake-macos.log").value_or("");
    auto consumed = run_process({{path_to_utf8(base / "cmake-macos" / "consumer")}, env, base, {}, true});
    ASSERT_TRUE(consumed) << consumed.error().message;
    EXPECT_EQ(consumed->output, "12 9 5 1 " + arch + "\n");
    ASSERT_TRUE(cmake({"-S", source, "-B", path_to_utf8(base / "cmake-iossim"), "-G", "Ninja", "-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_SYSROOT=iphonesimulator",
                          "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0"},
        "cmake-iossim.log"))
        << read_file(base / "cmake-iossim.log").value_or("");
    ASSERT_TRUE(cmake({"--build", path_to_utf8(base / "cmake-iossim"), "--verbose"}, "cmake-iossim.log")) << read_file(base / "cmake-iossim.log").value_or("");
    EXPECT_NE(read_file(base / "cmake-iossim.log").value_or("?").find("tiny-a_tiny_a.xcframework/ios-arm64_x86_64-simulator/Headers"), std::string::npos);
}

// A product of headers only beside a compiled one without headers of its own (recipe-toml.md 11.1): the
// headers in the shim's include/, once, reached by a C target through the header search path.
TEST(apple_target, installs_products_of_headers_only)
{
    auto target = target::apple::make_build_target("", current_environment());
    if (auto p = target->profile({"macosx", "arm64", "13.0"}); !p) GTEST_SKIP() << "no Xcode here: " << p.error().message;

    test::temp_dir dir;
    std::filesystem::path const base = std::filesystem::canonical(dir.path());
    std::filesystem::path const project = base / "project";
    ASSERT_TRUE(copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/shared-headers"), project));
    dir.write("project/otok.toml", "[otok]\nformat = 1\n\n[apple]\nplatforms = [\"macosx\", \"iossim\"]\n"
                                   "deployment = { ios = \"16.0\", macos = \"13.0\" }\n\n"
                                   "[deps.tiny-h]\nrecipe = { path = \"recipes/tiny-h\" }\nsource = { path = \"vendor/tiny-h\" }\n");

    std::ostringstream out, err;
    auto parsed = parse_command_line(std::vector<std::string>{"install"}, global_options(), command_specs());
    ASSERT_TRUE(parsed);
    std::filesystem::path const cache = base / "cache";
    command_context ctx{project, all_targets, nullptr, out, err, [cache](std::string_view name) -> std::optional<std::string> {
        if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache);
        if (name == "PATH") return process_environment()(name);  // the toolchains (docs/PLAN.md decision 38)
        return std::nullopt;
    }};
    ctx.make_build_targets = [](machine_config const& config) {
        std::vector<std::unique_ptr<build_target>> targets;
        targets.push_back(target::apple::make_build_target(config.xcode, current_environment()));
        return targets;
    };
    ASSERT_EQ(run_command(*parsed, ctx), 0) << out.str() << err.str();

    std::filesystem::path const otok_dir = project / "Otok";
    std::filesystem::path const shim = otok_dir / "Packages" / "tiny-h" / "shims" / "TinyH";
    EXPECT_TRUE(std::filesystem::exists(shim / "include" / "tiny_h" / "inline.h"));
    EXPECT_EQ(read_file(shim / "include" / "module.modulemap").value_or("?"), "module TinyH_otok {\n    header \"TinyH_otok.h\"\n    export *\n}\n");
    // privacy-manifest = "none": the manifest otok writes (recipe-toml.md 11)
    EXPECT_EQ(read_file(shim / "PrivacyInfo.xcprivacy").value_or("?"), no_privacy_data_manifest);
    // the XCFramework of the compiled part carries no headers
    for (auto const& e : std::filesystem::recursive_directory_iterator{otok_dir / "Packages" / "tiny-h" / "tiny-h_compiled.xcframework"}) {
        EXPECT_NE(e.path().filename(), "Headers") << path_to_utf8(e.path());
    }
    std::string const swift = read_file(otok_dir / "Package.swift").value_or("?");
    EXPECT_NE(swift.find(".library(name: \"TinyH\", targets: [\"TinyH_otok\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".library(name: \"TinyHCompiled\", targets: [\"tiny-h_compiled\", \"TinyH_otok\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find("resources: [.copy(\"PrivacyInfo.xcprivacy\")]"), std::string::npos) << swift;

    // a C target of a consumer includes both parts; Swift calls the C target
    std::filesystem::path const consumer = base / "consumer";
    dir.write("consumer/Package.swift",
        "// swift-tools-version:5.9\nimport PackageDescription\n\nlet package = Package(\n    name: \"Consumer\",\n"
        "    platforms: [.macOS(\"13.0\")],\n    dependencies: [.package(path: \"../project/Otok\")],\n"
        "    targets: [\n        .target(name: \"Check\", dependencies: [.product(name: \"TinyHCompiled\", package: \"Otok\")]),\n"
        "        .executableTarget(name: \"Consumer\", dependencies: [\"Check\"]),\n    ]\n)\n");
    dir.write("consumer/Sources/Check/include/check.h", "int check(void);\n");
    dir.write("consumer/Sources/Check/check.c",
        "#include \"check.h\"\n#include <tiny_h/compiled.h>\n#include <tiny_h/inline.h>\n\nint check(void) { return tiny_h_compiled() + tiny_h_twice(1); }\n");
    dir.write("consumer/Sources/Consumer/main.swift", "import Check\n\nprint(check())\n");
    env_vars env = current_environment();
    auto ran = run_process({{"/usr/bin/xcrun", "swift", "run", "--package-path", path_to_utf8(consumer), "-q", "Consumer"}, env, consumer,
        base / "swift.log", true});
    ASSERT_TRUE(ran) << ran.error().message;
    EXPECT_TRUE(ran->success()) << read_file(base / "swift.log").value_or("");
    EXPECT_EQ(ran->output, "43\n") << read_file(base / "swift.log").value_or("");

    // CMake: the headers of the product of headers only through the include directory of the shim; a platform
    // without a slice refused by the file, named as otok.toml names it (otok-toml.md 4)
    dir.write("cmake-consumer/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.28)\nproject(consumer C)\n"
        "find_package(Otok CONFIG REQUIRED PATHS \"${CMAKE_CURRENT_SOURCE_DIR}/../project/Otok\" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)\n"
        "add_executable(consumer main.c)\ntarget_link_libraries(consumer PRIVATE otok::TinyHCompiled)\n");
    dir.write("cmake-consumer/main.c", "#include <stdio.h>\n#include <tiny_h/compiled.h>\n#include <tiny_h/inline.h>\n\n"
                                       "int main(void)\n{\n    printf(\"%d\\n\", tiny_h_compiled() + tiny_h_twice(1));\n    return 0;\n}\n");
    auto const cmake = [&](std::vector<std::string> args, std::string const& log) {
        args.insert(args.begin(), "cmake");
        auto done = run_process({args, env, base, base / log});
        return done && done->success();
    };
    std::string const source = path_to_utf8(base / "cmake-consumer");
    ASSERT_TRUE(cmake({"-S", source, "-B", path_to_utf8(base / "cmake-macos"), "-G", "Ninja"}, "cmake-macos.log"))
        << read_file(base / "cmake-macos.log").value_or("");
    ASSERT_TRUE(cmake({"--build", path_to_utf8(base / "cmake-macos")}, "cmake-macos.log")) << read_file(base / "cmake-macos.log").value_or("");
    auto consumed = run_process({{path_to_utf8(base / "cmake-macos" / "consumer")}, env, base, {}, true});
    ASSERT_TRUE(consumed) << consumed.error().message;
    EXPECT_EQ(consumed->output, "43\n");
    EXPECT_FALSE(cmake({"-S", source, "-B", path_to_utf8(base / "cmake-tvos"), "-G", "Ninja", "-DCMAKE_SYSTEM_NAME=tvOS"}, "cmake-tvos.log"));
    EXPECT_NE(squeezed(read_file(base / "cmake-tvos.log").value_or("?")).find("has no libraries for the platform 'tvos': add it to 'platforms' of [apple]"),
        std::string::npos)
        << read_file(base / "cmake-tvos.log").value_or("");
}

// Dynamic libraries as frameworks on every platform (otok-toml.md 4.10): tiny-c loads tiny-d under its framework's
// name, both signed, their headers beside them; a SwiftPM and a CMake consumer run on this Mac.
TEST(apple_target, installs_dynamic_frameworks)
{
    auto target = target::apple::make_build_target("", current_environment());
    if (auto p = target->profile({"macosx", "arm64", "13.0"}); !p) GTEST_SKIP() << "no Xcode here: " << p.error().message;

    test::temp_dir dir;
    std::filesystem::path const base = std::filesystem::canonical(dir.path());
    std::filesystem::path const project = base / "project";
    ASSERT_TRUE(copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/dynamic"), project));
    dir.write("project/otok.toml",
        "[otok]\nformat = 1\n\n[apple]\n"
        "platforms = [\"ios\", \"iossim\", \"macosx\", \"catalyst\", \"tvos\", \"tvossim\", \"watchos\", \"watchossim\", \"xros\", \"xrossim\"]\n"
        "architectures = { watchos = [\"arm64_32\"] }\n"
        "deployment = { ios = \"16.0\", macos = \"13.0\", tvos = \"16.0\", watchos = \"9.0\", visionos = \"1.0\" }\n"
        "linkage = \"dynamic\"\nbundle-id-prefix = \"dev.otok.test\"\n\n"
        "[deps.tiny-c]\nrecipe = { path = \"recipes/tiny-c\" }\nsource = { path = \"vendor/tiny-c\" }\n\n"
        "[deps.tiny-d]\nrecipe = { path = \"recipes/tiny-d\" }\nsource = { path = \"vendor/tiny-d\" }\n");

    std::ostringstream out, err;
    auto parsed = parse_command_line(std::vector<std::string>{"install"}, global_options(), command_specs());
    ASSERT_TRUE(parsed);
    std::filesystem::path const cache = base / "cache";
    command_context ctx{project, all_targets, nullptr, out, err, [cache](std::string_view name) -> std::optional<std::string> {
        if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache);
        if (name == "PATH") return process_environment()(name);  // the toolchains (docs/PLAN.md decision 38)
        return std::nullopt;
    }};
    ctx.make_build_targets = [](machine_config const& config) {
        std::vector<std::unique_ptr<build_target>> targets;
        targets.push_back(target::apple::make_build_target(config.xcode, current_environment()));
        return targets;
    };
    ASSERT_EQ(run_command(*parsed, ctx), 0) << out.str() << err.str();
    EXPECT_NE(out.str().find("slices: 30 (0 from the cache)\n"), std::string::npos) << out.str() << err.str();

    std::filesystem::path const packages = project / "Otok" / "Packages";
    std::filesystem::path const c = packages / "tiny-c" / "tiny-c_tiny_c.xcframework";
    // a flat framework on the devices, its binary named after its framework and loading tiny-d's
    std::filesystem::path const ios = c / "ios-arm64" / "tiny-c_tiny_c.framework";
    load_names const names = load_names_of(ios / "tiny-c_tiny_c");
    EXPECT_EQ(names.id, "@rpath/tiny-c_tiny_c.framework/tiny-c_tiny_c");
    EXPECT_NE(std::ranges::find(names.needed, "@rpath/tiny-d_tiny_d.framework/tiny-d_tiny_d"), names.needed.end());
    EXPECT_EQ(read_file(ios / "PrivacyInfo.xcprivacy").value_or("?"), no_privacy_data_manifest);
    std::string const plist = read_file(ios / "Info.plist").value_or("?");
    EXPECT_NE(plist.find("<key>CFBundleIdentifier</key>\n\t<string>dev.otok.test.tiny-c.tiny-c</string>"), std::string::npos) << plist;
    EXPECT_NE(plist.find("<key>MinimumOSVersion</key>\n\t<string>16.0</string>"), std::string::npos) << plist;
    // a versioned one on macOS
    std::filesystem::path const mac = c / "macos-arm64_x86_64" / "tiny-c_tiny_c.framework";
    EXPECT_TRUE(std::filesystem::is_symlink(mac / "tiny-c_tiny_c"));
    EXPECT_TRUE(std::filesystem::is_symlink(mac / "Versions" / "Current"));
    env_vars env = current_environment();
    // a universal binary, which otool reads (load_names_of reads the thin ones of slices)
    auto id = run_process({{"/usr/bin/xcrun", "otool", "-arch", "arm64", "-D", path_to_utf8(mac / "Versions" / "A" / "tiny-c_tiny_c")}, env, base, {}, true});
    ASSERT_TRUE(id) << id.error().message;
    EXPECT_NE(id->output.find("\n@rpath/tiny-c_tiny_c.framework/Versions/A/tiny-c_tiny_c"), std::string::npos) << id->output;
    EXPECT_NE(read_file(mac / "Versions" / "A" / "Resources" / "Info.plist").value_or("?").find("<key>LSMinimumSystemVersion</key>\n\t<string>13.0</string>"),
        std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(mac / "Versions" / "A" / "Resources" / "PrivacyInfo.xcprivacy"));
    for (auto const& framework : {ios, mac, c / "watchos-arm64_32" / "tiny-c_tiny_c.framework", c / "ios-arm64_x86_64-maccatalyst" / "tiny-c_tiny_c.framework"}) {
        auto verified = run_process({{"/usr/bin/codesign", "--verify", path_to_utf8(framework)}, env, base, base / "codesign.log"});
        ASSERT_TRUE(verified) << verified.error().message;
        EXPECT_TRUE(verified->success()) << path_to_utf8(framework) << ": " << read_file(base / "codesign.log").value_or("");
    }
    // the headers beside it, on every platform
    EXPECT_TRUE(std::filesystem::exists(packages / "tiny-c" / "tiny-c_tiny_c_headers.xcframework" / "ios-arm64" / "Headers" / "tiny_c.h"));
    std::string const swift = read_file(project / "Otok" / "Package.swift").value_or("?");
    EXPECT_NE(swift.find(".library(name: \"TinyC\", targets: [\"tiny-c_tiny_c\", \"tiny-c_tiny_c_headers\", \"tiny-d_tiny_d\", \"tiny-d_tiny_d_headers\"])"),
        std::string::npos) << swift;

    // SwiftPM: a C target calls tiny-c, a Swift executable calls the C target
    dir.write("consumer/Package.swift",
        "// swift-tools-version:5.9\nimport PackageDescription\n\nlet package = Package(\n    name: \"Consumer\",\n"
        "    platforms: [.macOS(\"13.0\")],\n    dependencies: [.package(path: \"../project/Otok\")],\n"
        "    targets: [\n        .target(name: \"Check\", dependencies: [.product(name: \"TinyC\", package: \"Otok\")]),\n"
        "        .executableTarget(name: \"Consumer\", dependencies: [\"Check\"]),\n    ]\n)\n");
    dir.write("consumer/Sources/Check/include/check.h", "int check(void);\n");
    dir.write("consumer/Sources/Check/check.c", "#include \"check.h\"\n#include <tiny_c.h>\n#include <tiny_d.h>\n\nint check(void) { return tiny_c() * 10 + tiny_d(); }\n");
    dir.write("consumer/Sources/Consumer/main.swift", "import Check\n\nprint(check())\n");
    std::filesystem::path const consumer = base / "consumer";
    auto ran = run_process({{"/usr/bin/xcrun", "swift", "run", "--package-path", path_to_utf8(consumer), "-q", "Consumer"}, env, consumer,
        base / "swift.log", true});
    ASSERT_TRUE(ran) << ran.error().message;
    EXPECT_TRUE(ran->success()) << read_file(base / "swift.log").value_or("");
    EXPECT_EQ(ran->output, "111\n") << read_file(base / "swift.log").value_or("");  // tiny_c = tiny_d + 10, tiny_d = 1

    // CMake: the shared imported libraries, run from the build tree through otok_embed_frameworks
    dir.write("cmake-consumer/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.28)\nproject(consumer C)\n"
        "find_package(Otok CONFIG REQUIRED PATHS \"${CMAKE_CURRENT_SOURCE_DIR}/../project/Otok\" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)\n"
        "add_executable(consumer main.c)\ntarget_link_libraries(consumer PRIVATE otok::TinyC)\notok_embed_frameworks(consumer)\n");
    dir.write("cmake-consumer/main.c", "#include <stdio.h>\n#include <tiny_c.h>\n\nint main(void)\n{\n    printf(\"%d\\n\", tiny_c());\n    return 0;\n}\n");
    for (std::vector<std::string> const& args : {std::vector<std::string>{"cmake", "-S", path_to_utf8(base / "cmake-consumer"), "-B", path_to_utf8(base / "cmake-build"), "-G", "Ninja"},
             std::vector<std::string>{"cmake", "--build", path_to_utf8(base / "cmake-build")}}) {
        auto done = run_process({args, env, base, base / "cmake.log"});
        ASSERT_TRUE(done && done->success()) << read_file(base / "cmake.log").value_or("");
    }
    auto consumed = run_process({{path_to_utf8(base / "cmake-build" / "consumer")}, env, base, {}, true});
    ASSERT_TRUE(consumed) << consumed.error().message;
    EXPECT_EQ(consumed->output, "11\n");
}

// A SwiftPM package delivered patched by its recipe (recipe-toml.md 12.1, otok-toml.md 4.6), which a
// consumer imports through Otok.
TEST(apple_target, installs_a_swift_package)
{
    auto target = target::apple::make_build_target("", current_environment());
    if (auto p = target->profile({"macosx", "arm64", "13.0"}); !p) GTEST_SKIP() << "no Xcode here: " << p.error().message;

    test::temp_dir dir;
    std::filesystem::path const base = std::filesystem::canonical(dir.path());
    std::filesystem::path const project = base / "project";
    ASSERT_TRUE(copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/swift-package"), project));
    dir.write("project/otok.toml", "[otok]\nformat = 1\n\n[apple]\nplatforms = [\"macosx\", \"iossim\"]\n"
                                   "deployment = { ios = \"16.0\", macos = \"13.0\" }\n\n"
                                   "[deps.greeter]\nrecipe = { path = \"recipes/greeter\" }\nsource = { path = \"vendor/greeter\" }\n");

    std::ostringstream out, err;
    auto parsed = parse_command_line(std::vector<std::string>{"install"}, global_options(), command_specs());
    ASSERT_TRUE(parsed);
    std::filesystem::path const cache = base / "cache";
    command_context ctx{project, all_targets, nullptr, out, err, [cache](std::string_view name) -> std::optional<std::string> {
        if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache);
        if (name == "PATH") return process_environment()(name);  // git for the patch
        return std::nullopt;
    }};
    ctx.make_build_targets = [](machine_config const& config) {
        std::vector<std::unique_ptr<build_target>> targets;
        targets.push_back(target::apple::make_build_target(config.xcode, current_environment()));
        return targets;
    };
    ASSERT_EQ(run_command(*parsed, ctx), 0) << out.str() << err.str();
    EXPECT_NE(out.str().find("slices: 0 (0 from the cache)\n"), std::string::npos) << out.str();
    EXPECT_NE(err.str().find("patching the source of greeter 1.0\n"), std::string::npos) << err.str();

    std::filesystem::path const otok_dir = project / "Otok";
    std::filesystem::path const package = otok_dir / "Packages" / "greeter";
    EXPECT_NE(read_file(package / "greeter" / "Sources" / "Greeter" / "Greeter.swift").value_or("?").find("https://example.com"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(package / "greeter" / "Package.swift"));
    EXPECT_TRUE(std::filesystem::exists(package / "shims" / "Greeter" / "Greeter_otok.swift"));
    EXPECT_TRUE(std::filesystem::exists(package / "licenses" / "LICENSE"));
    std::string const swift = read_file(otok_dir / "Package.swift").value_or("?");
    EXPECT_NE(swift.find(".library(name: \"Greeter\", targets: [\"Greeter_otok\"])"), std::string::npos) << swift;
    EXPECT_NE(swift.find(".package(path: \"Packages/greeter/greeter\")"), std::string::npos) << swift;
    EXPECT_NE(swift.find("dependencies: [.product(name: \"Greeter\", package: \"greeter\")]"), std::string::npos) << swift;

    // the consumer imports the package's module as it is
    std::filesystem::path const consumer = base / "consumer";
    dir.write("consumer/Package.swift",
        "// swift-tools-version:5.9\nimport PackageDescription\n\nlet package = Package(\n    name: \"Consumer\",\n"
        "    platforms: [.macOS(\"13.0\")],\n    dependencies: [.package(path: \"../project/Otok\")],\n"
        "    targets: [.executableTarget(name: \"Consumer\", dependencies: [.product(name: \"Greeter\", package: \"Otok\")])]\n)\n");
    dir.write("consumer/Sources/Consumer/main.swift", "import Greeter\n\nprint(greeting())\n");
    env_vars env = current_environment();
    auto ran = run_process({{"/usr/bin/xcrun", "swift", "run", "--package-path", path_to_utf8(consumer), "-q", "Consumer"}, env, consumer,
        base / "swift.log", true});
    ASSERT_TRUE(ran) << ran.error().message;
    EXPECT_TRUE(ran->success()) << read_file(base / "swift.log").value_or("");
    EXPECT_EQ(ran->output, "https://example.com\n") << read_file(base / "swift.log").value_or("");

    // nothing changed: the patched source and the package come from the cache
    ASSERT_EQ(run_command(*parsed, ctx), 0) << out.str() << err.str();
    EXPECT_NE(err.str().find("greeter 1.0 source: cached\n"), std::string::npos) << err.str();

    // a package that depends on other packages is refused (decision 52)
    dir.write("project/vendor/greeter/Package.swift",
        "// swift-tools-version:5.9\nimport PackageDescription\n\nlet package = Package(\n    name: \"Greeter\",\n"
        "    dependencies: [.package(url: \"https://github.com/apple/swift-log\", from: \"1.0.0\")],\n"
        "    targets: [.target(name: \"Greeter\")]\n)\n");
    auto update = parse_command_line(std::vector<std::string>{"update"}, global_options(), command_specs());  // a new hash of the source
    ASSERT_TRUE(update);
    EXPECT_EQ(run_command(*update, ctx), 1);
    EXPECT_NE(err.str().find("its Package.swift depends on other packages, which otok cannot pin"), std::string::npos) << err.str();
}

// A meson build for every Apple platform: a cross file per slice (build-lua-api.md 6.2), whose libraries
// pass the checks of their platform and architecture.
TEST(apple_target, installs_a_meson_project)
{
    auto target = target::apple::make_build_target("", current_environment());
    if (auto p = target->profile({"macosx", "arm64", "13.0"}); !p) GTEST_SKIP() << "no Xcode here: " << p.error().message;
    if (!find_program("meson", current_environment(), std::filesystem::current_path())) GTEST_SKIP() << "meson is not in PATH";

    test::temp_dir dir;
    std::filesystem::path const base = std::filesystem::canonical(dir.path());
    std::filesystem::path const project = base / "project";
    ASSERT_TRUE(copy_tree(path_from_utf8(OTOK_TEST_DATA_DIR "/projects/meson"), project));
    dir.write("project/otok.toml",
        "[otok]\nformat = 1\n\n[apple]\n"
        "platforms = [\"ios\", \"iossim\", \"macosx\", \"catalyst\", \"tvos\", \"tvossim\", \"watchos\", \"watchossim\", \"xros\", \"xrossim\"]\n"
        "architectures = { watchos = [\"arm64_32\"] }\n"
        "deployment = { ios = \"16.0\", macos = \"13.0\", tvos = \"16.0\", watchos = \"9.0\", visionos = \"1.0\" }\n"
        "cflags = [\"-DTINY_M_BONUS=2\"]\n\n"
        "[deps.tiny-m]\nrecipe = { path = \"recipes/tiny-m\" }\nsource = { path = \"vendor/tiny-m\" }\noptions = { value = 5 }\n");

    std::ostringstream out, err;
    auto parsed = parse_command_line(std::vector<std::string>{"install"}, global_options(), command_specs());
    ASSERT_TRUE(parsed);
    std::filesystem::path const cache = base / "cache";
    command_context ctx{project, all_targets, nullptr, out, err, [cache](std::string_view name) -> std::optional<std::string> {
        if (name == "OTOK_CACHE_DIR") return path_to_utf8(cache);
        if (name == "PATH") return process_environment()(name);  // the toolchains (docs/PLAN.md decision 38)
        return std::nullopt;
    }};
    ctx.make_build_targets = [](machine_config const& config) {
        std::vector<std::unique_ptr<build_target>> targets;
        targets.push_back(target::apple::make_build_target(config.xcode, current_environment()));
        return targets;
    };
    ASSERT_EQ(run_command(*parsed, ctx), 0) << out.str() << err.str();
    EXPECT_NE(out.str().find("slices: 15 (0 from the cache)\n"), std::string::npos) << out.str();

    // Swift calls it on this Mac: the option and the project's flags reached the build
    std::filesystem::path const consumer = base / "consumer";
    dir.write("consumer/Package.swift",
        "// swift-tools-version:5.9\nimport PackageDescription\n\nlet package = Package(\n    name: \"Consumer\",\n"
        "    platforms: [.macOS(\"13.0\")],\n    dependencies: [.package(path: \"../project/Otok\")],\n"
        "    targets: [.executableTarget(name: \"Consumer\", dependencies: [.product(name: \"TinyM\", package: \"Otok\")])]\n)\n");
    dir.write("consumer/Sources/Consumer/main.swift", "import TinyM\n\nprint(tiny_m())\n");
    env_vars env = current_environment();
    auto ran = run_process({{"/usr/bin/xcrun", "swift", "run", "--package-path", path_to_utf8(consumer), "-q", "Consumer"}, env, consumer,
        base / "swift.log", true});
    ASSERT_TRUE(ran) << ran.error().message;
    EXPECT_TRUE(ran->success()) << read_file(base / "swift.log").value_or("");
    EXPECT_EQ(ran->output, "7\n") << read_file(base / "swift.log").value_or("");
}

TEST(apple_target, refuses_libraries_of_other_platforms)
{
    auto target = target::apple::make_build_target("", current_environment());
    auto profile = target->profile({"macosx", "arm64", "13.0"});
    if (!profile) GTEST_SKIP() << "no Xcode here: " << profile.error().message;

    test::temp_dir dir;
    std::filesystem::path const base = std::filesystem::canonical(dir.path());
    dir.write("t.c", "int t(void) { return 1; }\n");
    std::vector<std::string> compile{profile->toolchain.cc, "-c", "t.c", "-o", "t.o"};
    compile.insert(compile.end(), profile->toolchain.cflags.begin(), profile->toolchain.cflags.end());
    auto compiled = run_process({compile, profile->toolchain.env, base, base / "log"});
    ASSERT_TRUE(compiled && compiled->success()) << read_file(base / "log").value_or("");
    auto archived = run_process({{profile->toolchain.ar, "rcs", "libt.a", "t.o"}, profile->toolchain.env, base, base / "log"});
    ASSERT_TRUE(archived && archived->success()) << read_file(base / "log").value_or("");

    EXPECT_TRUE(target->check_library(base / "libt.a", {"macosx", "arm64", "13.0"}));
    auto platform = target->check_library(base / "libt.a", {"iossim", "arm64", "16.0"});
    ASSERT_FALSE(platform);
    EXPECT_EQ(platform.error().message, "it contains code for macOS, not for iOS Simulator");
    auto arch = target->check_library(base / "libt.a", {"macosx", "x86_64", "13.0"});
    ASSERT_FALSE(arch);
    EXPECT_EQ(arch.error().message, "it is for the architectures 'arm64', not for 'x86_64'");
    auto newer = target->check_library(base / "libt.a", {"macosx", "arm64", "12.0"});
    ASSERT_FALSE(newer);
    EXPECT_EQ(newer.error().message, "it contains code for macOS 13.0, newer than the deployment version 12.0");
}

#endif
