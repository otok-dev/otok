// Otok (c) Alexander A Pototskiy
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception

// What the Windows target gives the consumer's build (otok-toml.md 4.11, docs/PLAN.md decision 101): its parts of
// otok-config.cmake, and otok.props and otok.targets for projects of Visual Studio without CMake.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "otok/target/windows/windows_target.hpp"

namespace otok::target::windows {

namespace {

// The toolset's minor number, 44 of "14.44"; -1 when it is not one of 14.
int toolset_minor(std::string const& toolset)
{
    if (!toolset.starts_with("14.")) return -1;
    int n = 0;
    std::size_t i = 3;
    for (; i < toolset.size() && toolset[i] >= '0' && toolset[i] <= '9'; ++i) n = n * 10 + (toolset[i] - '0');
    return i == 3 ? -1 : n;
}

// A system library as the linker takes it: "ws2_32.lib".
std::string library_file(std::string const& name)
{
    std::string lower = name;
    std::ranges::transform(lower, lower.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return lower.ends_with(".lib") ? name : name + ".lib";
}

// A text in MSBuild's XML: its special characters (`%`, `$`, `@`, `'`, `;`, `?`, `*`) escaped as MSBuild does, then XML's.
std::string msbuild_text(std::string_view s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '%': out += "%25"; break;
        case '$': out += "%24"; break;
        case '@': out += "%40"; break;
        case '\'': out += "%27"; break;
        case ';': out += "%3B"; break;
        case '?': out += "%3F"; break;
        case '*': out += "%2A"; break;
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// A path relative to the output: `$(OtokDir)tiny-a\lib\x86_64\tiny_a.lib`.
std::string msbuild_path(std::string_view relative)
{
    std::string p{relative};
    std::ranges::replace(p, '/', '\\');
    return "$(OtokDir)" + msbuild_text(p);
}

std::string joined(std::vector<std::string> const& items, std::string_view separator)
{
    std::string out;
    for (std::string const& i : items) out += (out.empty() ? "" : std::string{separator}) + i;
    return out;
}

// A name MSBuild takes in a property's name: letters, digits and '_'.
std::string property_name(std::string_view s)
{
    std::string out;
    for (char c : s) out += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    return out;
}

std::string lowered(std::string s)
{
    std::ranges::transform(s, s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return s;
}

// The platform of Visual Studio an architecture is.
std::string vs_platform(std::string_view arch)
{
    return arch == "arm64" ? "ARM64" : arch == "x86_64" ? "x64" : std::string{arch};
}

}

std::string detail::crt_of_key(std::string_view key_text)
{
    constexpr std::string_view prefix = "toolchain.crt = ";
    for (std::size_t at = 0; at < key_text.size();) {
        std::size_t end = key_text.find('\n', at);
        if (end == std::string_view::npos) end = key_text.size();
        std::string_view line = key_text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with(prefix)) {
            std::string_view const version = line.substr(prefix.size());
            std::size_t const first = version.find('.');
            if (first == std::string_view::npos) return {};
            std::size_t const second = version.find('.', first + 1);
            return std::string{version.substr(0, second)};
        }
        at = end + 1;
    }
    return {};
}

consumer_cmake_parts detail::cmake_parts(consumer_package const& package)
{
    consumer_cmake_parts parts;
    parts.target = "windows";
    parts.platform = "windows";
    parts.detect =
        "if(NOT CMAKE_SYSTEM_NAME STREQUAL \"Windows\")\n"
        "  message(FATAL_ERROR \"otok: ${_otok_dir} holds libraries for Windows, not for '${CMAKE_SYSTEM_NAME}'\")\n"
        "endif()\n"
        "if(NOT MSVC)\n"
        "  message(FATAL_ERROR \"otok: ${_otok_dir} holds libraries for MSVC (cl or clang-cl), which this build does not use\")\n"
        "endif()\n"
        "if(CMAKE_VS_PLATFORM_NAME)\n"
        "  set(_otok_arch \"${CMAKE_VS_PLATFORM_NAME}\")\n"
        "elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID)\n"
        "  set(_otok_arch \"${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}\")\n"
        "elseif(CMAKE_C_COMPILER_ARCHITECTURE_ID)\n"
        "  set(_otok_arch \"${CMAKE_C_COMPILER_ARCHITECTURE_ID}\")\n"
        "else()\n"
        "  set(_otok_arch \"${CMAKE_SYSTEM_PROCESSOR}\")\n"
        "endif()\n"
        "string(TOLOWER \"${_otok_arch}\" _otok_arch)\n"
        "if(_otok_arch MATCHES \"^(x64|amd64|x86_64)$\")\n"
        "  set(_otok_arch \"x86_64\")\n"
        "elseif(_otok_arch MATCHES \"^(arm64|aarch64)$\")\n"
        "  set(_otok_arch \"arm64\")\n"
        "endif()\n";
    if (auto it = package.properties.find("crt"); it != package.properties.end() && toolset_minor(it->second) >= 0) {
        std::string const minor = std::to_string(toolset_minor(it->second));
        parts.checks =
            "\n# cl links libraries of its own toolset or an older one (its version 19.<n> is the toolset 14.<n>)\n"
            "if(CMAKE_CXX_COMPILER_ID STREQUAL \"MSVC\")\n"
            "  set(_otok_cl \"${CMAKE_CXX_COMPILER_VERSION}\")\n"
            "elseif(CMAKE_C_COMPILER_ID STREQUAL \"MSVC\")\n"
            "  set(_otok_cl \"${CMAKE_C_COMPILER_VERSION}\")\n"
            "else()\n"
            "  set(_otok_cl \"\")\n"
            "endif()\n"
            "if(_otok_cl MATCHES \"^19\\\\.([0-9]+)\" AND CMAKE_MATCH_1 LESS " + minor + ")\n"
            "  message(FATAL_ERROR \"otok: the libraries of ${_otok_dir} were built with the toolset " + it->second
            + " of MSVC and link with it or a newer one, not with 14.${CMAKE_MATCH_1} (cl ${_otok_cl})\")\n"
              "endif()\n"
              "unset(_otok_cl)\n";
    }
    parts.strict_configurations = true;
    parts.link_item = library_file;
    parts.not_built_suffix = ".lib";
    parts.runtime_dlls = true;
    return parts;
}

std::string detail::msbuild_props(consumer_package const& package)
{
    std::string text =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<!-- Generated by otok; do not edit. The libraries of this output for projects of Visual Studio (otok-toml.md 4.11):\n"
        "     import otok.props before the project's settings (or from Directory.Build.props) and otok.targets after them (or from\n"
        "     Directory.Build.targets), and name the products a project links in OtokProducts: <OtokProducts>LZ4;Boost.JSON</OtokProducts>. -->\n"
        "<Project>\n"
        "  <PropertyGroup>\n"
        "    <OtokDir>$(MSBuildThisFileDirectory)</OtokDir>\n"
        "    <OtokAcknowledgements>$(OtokDir)Acknowledgements.md</OtokAcknowledgements>\n";
    for (auto const& [library, data] : package.data_dirs) {
        text += "    <OtokDataDir_" + property_name(library) + ">" + msbuild_path(data) + "\\</OtokDataDir_" + property_name(library) + ">\n";
    }
    return text + "  </PropertyGroup>\n</Project>\n";
}

std::string detail::msbuild_targets(consumer_package const& package)
{
    std::string text =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<!-- Generated by otok; do not edit. Imported after the project's settings: the headers and libraries of this output\n"
        "     for the platform and the configuration (debug libraries with the debug CRT, UseDebugLibraries), the libraries,\n"
        "     system libraries and defines of the products named in OtokProducts, and their DLLs beside an application\n"
        "     (otok-toml.md 4.11). -->\n"
        "<Project>\n"
        "  <PropertyGroup>\n"
        "    <OtokArch Condition=\"'$(Platform)' == 'x64'\">x86_64</OtokArch>\n"
        "    <OtokArch Condition=\"'$(Platform)' == 'ARM64'\">arm64</OtokArch>\n"
        "    <OtokConfiguration Condition=\"'$(UseDebugLibraries)' == 'true'\">debug</OtokConfiguration>\n"
        "    <OtokConfiguration Condition=\"'$(UseDebugLibraries)' != 'true'\">release</OtokConfiguration>\n"
        "    <_OtokRoot Condition=\"'$(OtokConfiguration)' == 'debug'\">debug\\</_OtokRoot>\n";
    std::set<std::string> packages;
    std::vector<std::string> includes, library_dirs;
    for (consumer_library const& l : package.libraries) {
        if (packages.insert(l.package).second) library_dirs.push_back(msbuild_path(l.package) + "\\$(_OtokRoot)lib\\$(OtokArch)");
    }
    std::set<std::string> seen;
    for (consumer_library const& l : package.libraries) {
        if (!l.include_dir.empty() && seen.insert(l.include_dir).second) includes.push_back(msbuild_path(l.include_dir));
    }
    for (consumer_product const& p : package.products) {
        for (std::string const& d : p.include_dirs) {
            if (seen.insert(d).second) includes.push_back(msbuild_path(d));
        }
    }
    text += "    <OtokIncludePath>" + joined(includes, ";") + "</OtokIncludePath>\n"
            "    <OtokLibraryPath>" + joined(library_dirs, ";") + "</OtokLibraryPath>\n"
            "    <_OtokProducts>;$([MSBuild]::Unescape($(OtokProducts.Replace(' ', '').ToLowerInvariant())));</_OtokProducts>\n"
            "  </PropertyGroup>\n";

    // the files of each library for each platform and configuration
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < package.libraries.size(); ++i) index[package.libraries[i].name] = i;
    for (std::string const& arch : package.architectures) {
        for (std::string const& configuration : package.configurations) {
            text += "  <PropertyGroup Condition=\"'$(OtokArch)|$(OtokConfiguration)' == '" + arch + "|" + configuration + "'\">\n";
            for (std::size_t i = 0; i < package.libraries.size(); ++i) {
                consumer_library const& l = package.libraries[i];
                auto const f = std::ranges::find_if(l.files, [&](package_library_file const& f) { return f.arch == arch && f.configuration == configuration; });
                if (f == l.files.end()) continue;
                std::string const n = std::to_string(i);
                text += "    <_OtokLib" + n + ">" + msbuild_path(f->link) + "</_OtokLib" + n + "><!-- " + l.name + " -->\n";
                if (!f->runtime.empty()) text += "    <_OtokDll" + n + ">" + msbuild_path(f->runtime) + "</_OtokDll" + n + ">\n";
            }
            text += "  </PropertyGroup>\n";
        }
    }

    // the products a project names
    std::vector<std::string> names;
    for (consumer_product const& p : package.products) {
        names.push_back(msbuild_text(p.name));
        std::vector<std::string> libraries, dlls, defines;
        std::vector<std::string> system;
        for (std::string const& l : p.libraries) {
            auto const at = index.find(l);
            if (at == index.end()) continue;
            std::string const n = std::to_string(at->second);
            consumer_library const& lib = package.libraries[at->second];
            libraries.push_back("$(_OtokLib" + n + ")");
            if (lib.shared) dlls.push_back("$(_OtokDll" + n + ")");
            for (std::string const& s : lib.link) {
                if (std::ranges::find(system, library_file(s)) == system.end()) system.push_back(library_file(s));
            }
        }
        for (std::string const& s : p.link) {
            if (std::ranges::find(system, library_file(s)) == system.end()) system.push_back(library_file(s));
        }
        for (std::string const& s : system) libraries.push_back(msbuild_text(s));
        for (std::string const& d : p.defines) defines.push_back(msbuild_text(d));
        text += "  <PropertyGroup Condition=\"$(_OtokProducts.Contains(';" + msbuild_text(lowered(p.name)) + ";'))\">\n";
        if (!libraries.empty()) text += "    <OtokLibraries>$(OtokLibraries);" + joined(libraries, ";") + "</OtokLibraries>\n";
        if (!dlls.empty()) text += "    <OtokDlls>$(OtokDlls);" + joined(dlls, ";") + "</OtokDlls>\n";
        if (!defines.empty()) text += "    <OtokDefinitions>$(OtokDefinitions);" + joined(defines, ";") + "</OtokDefinitions>\n";
        text += "  </PropertyGroup>\n";
    }

    text += "  <ItemDefinitionGroup>\n"
            "    <ClCompile>\n"
            "      <AdditionalIncludeDirectories>$(OtokIncludePath);%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>\n"
            "      <PreprocessorDefinitions>$(OtokDefinitions);%(PreprocessorDefinitions)</PreprocessorDefinitions>\n"
            "    </ClCompile>\n"
            "    <Link>\n"
            "      <AdditionalLibraryDirectories>$(OtokLibraryPath);%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>\n"
            "      <AdditionalDependencies>$(OtokLibraries);%(AdditionalDependencies)</AdditionalDependencies>\n"
            "    </Link>\n"
            "    <Lib>\n"
            "      <AdditionalLibraryDirectories>$(OtokLibraryPath);%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>\n"
            "    </Lib>\n"
            "  </ItemDefinitionGroup>\n";

    // what the project cannot have: a platform or a configuration not built, an older toolset, an unknown product
    std::string const known = joined(names, ";");
    std::string const cure = " in otok.toml and run otok install";
    text += "  <Target Name=\"OtokCheck\" BeforeTargets=\"PrepareForBuild\" Condition=\"'$(MSBuildProjectExtension)' == '.vcxproj'\">\n"
            "    <Error Condition=\"'$(OtokArch)' == ''\" Text=\"otok: $(OtokDir) has no libraries for the platform '$(Platform)': it has "
        + [&] {
              std::vector<std::string> platforms;
              for (std::string const& a : package.architectures) platforms.push_back(vs_platform(a));
              return platforms.empty() ? std::string{"none"} : joined(platforms, ", ");
          }()
        + "\" />\n";
    for (std::string const& arch : {std::string{"x86_64"}, std::string{"arm64"}}) {
        if (std::ranges::find(package.architectures, arch) != package.architectures.end()) continue;
        text += "    <Error Condition=\"'$(OtokArch)' == '" + arch + "'\" Text=\"otok: $(OtokDir) has no libraries for the platform '$(Platform)': add '"
            + arch + "' to 'architectures' of [windows]" + cure + "\" />\n";
    }
    for (std::string const& configuration : {std::string{"release"}, std::string{"debug"}}) {
        if (std::ranges::find(package.configurations, configuration) != package.configurations.end()) continue;
        text += "    <Error Condition=\"'$(OtokConfiguration)' == '" + configuration + "'\" Text=\"otok: $(OtokDir) has no libraries of the configuration '"
            + configuration + "', which '$(Configuration)' links with (" + (configuration == "debug" ? "the debug CRT" : "the release CRT")
            + "): add &quot;" + configuration + "&quot; to 'configurations' of [windows]" + cure + "\" />\n";
    }
    if (auto it = package.properties.find("crt"); it != package.properties.end() && toolset_minor(it->second) >= 0) {
        text += "    <Error Condition=\"'$(VCToolsVersion)' != '' and $([MSBuild]::VersionLessThan('$(VCToolsVersion)', '" + it->second
            + "'))\" Text=\"otok: the libraries of $(OtokDir) were built with the toolset " + it->second
            + " of MSVC and link with it or a newer one, not with $(VCToolsVersion)\" />\n";
    }
    text += "    <ItemGroup>\n"
            "      <_OtokRequested Include=\"$(OtokProducts)\" />\n"
            "      <_OtokUnknown Include=\"@(_OtokRequested)\" Exclude=\"" + known + "\" />\n"
            "    </ItemGroup>\n"
            "    <Error Condition=\"'@(_OtokUnknown)' != ''\" Text=\"otok: $(OtokDir) has no product '@(_OtokUnknown)' of OtokProducts; it has "
        + (names.empty() ? std::string{"none"} : joined(names, ", ")) + "\" />\n"
          "  </Target>\n";

    // the DLLs beside an application: those of its products, or all without OtokProducts
    text += "  <Target Name=\"OtokCopyDlls\" AfterTargets=\"Build\" Condition=\"'$(ConfigurationType)' == 'Application'\">\n"
            "    <ItemGroup>\n"
            "      <_OtokDll Include=\"$(OtokDlls)\" Condition=\"'$(OtokProducts)' != ''\" />\n"
            "      <_OtokDll Include=\"$(OtokDir)*\\$(_OtokRoot)bin\\$(OtokArch)\\*.dll\" Condition=\"'$(OtokProducts)' == ''\" />\n"
            "    </ItemGroup>\n"
            "    <Copy SourceFiles=\"@(_OtokDll)\" DestinationFolder=\"$(OutDir)\" SkipUnchangedFiles=\"true\" />\n"
            "  </Target>\n"
            "</Project>\n";
    return text;
}

}
