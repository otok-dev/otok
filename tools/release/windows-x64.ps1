# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Builds the self-contained otok of a release for Windows x64: every dependency static from vcpkg with the static CRT
# (triplets/x64-windows-static-otok), with CMake and the compiler of Visual Studio. Checks that otok.exe uses only the
# system's DLLs, then packs otok-<version>-windows-x64.zip (otok.exe, otok-sandbox.dll beside it (docs/PLAN.md decision
# 103), its license, the licenses of what it carries) and its .sha256, the files of a release that the Scoop manifest names (tools/scoop/otok.json).
#
#   windows-x64.ps1 [-Output DIR]      (default: build\release-windows in the checkout)
#
# VCPKG_ROOT: the vcpkg checkout (default ..\vcpkg beside this checkout). OTOK_REVISION: the revision to embed when the
# sources have no .git (a source archive).
param([string]$Output)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Output) { $Output = Join-Path $root 'build\release-windows' }
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $root '..\vcpkg' }
$triplet = 'x64-windows-static-otok'

function Fail([string]$message) { [Console]::Error.WriteLine("windows-x64.ps1: $message"); exit 1 }
function Run([string]$program) {
    & $program @args
    if ($LASTEXITCODE -ne 0) { Fail "$program failed with exit code $LASTEXITCODE" }
}

# Visual Studio: its CMake, and dumpbin of its newest MSVC toolset
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail "no vswhere at $vswhere" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'no Visual Studio with the C++ tools' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { Fail "no CMake of Visual Studio at $cmake" }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'

$build = Join-Path $Output 'build'
Run $cmake -S $root -B $build -G 'Visual Studio 18 2026' -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
    "-DVCPKG_OVERLAY_TRIPLETS=$root\triplets" `
    "-DVCPKG_TARGET_TRIPLET=$triplet" `
    "-DVCPKG_INSTALLED_DIR=$build\vcpkg_installed" `
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' `
    '-DOTOK_BUILD_TESTS=OFF' `
    "-DOTOK_REVISION=$env:OTOK_REVISION"
Run $cmake --build $build --config Release --target otok --parallel
$exe = Join-Path $build 'Release\otok.exe'
$sandbox = Join-Path $build 'Release\otok-sandbox.dll'
if (-not (Test-Path $sandbox)) { Fail "no $sandbox" }

# the system's DLLs only: no Visual C++ runtime, no DLL of a dependency (otok-sandbox.dll loads in every process of a
# sandboxed build)
foreach ($binary in $exe, $sandbox) {
    $needed = & $dumpbin /nologo /dependents $binary | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
    $foreign = $needed | Where-Object { $_ -match '^(vcruntime|msvcp|concrt|ucrtbase|api-ms-win-crt-)' -or
        -not (Test-Path (Join-Path $env:SystemRoot "System32\$_")) }
    if ($foreign) { Fail "$(Split-Path -Leaf $binary) needs DLLs outside the system: $($foreign -join ', ')" }
    if ($binary -eq $exe) { $dlls = $needed }
}

$version = (& $exe --version | Select-Object -First 1) -replace '^otok (\S+).*$', '$1'
if ($version -notmatch '^\d+\.\d+\.\d+') { Fail "no version from $exe --version" }

# the archive: otok.exe, otok's license and its exception, the licenses of the libraries built into it (not gtest: tests only)
$name = "otok-$version-windows-x64"
$stage = Join-Path $Output $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$licenses = New-Item -ItemType Directory -Force (Join-Path $stage 'licenses')
Copy-Item $exe $stage
Copy-Item $sandbox $stage
Copy-Item (Join-Path $root 'LICENSE') $stage
Copy-Item (Join-Path $root 'LICENSE-EXCEPTION') $stage
Copy-Item (Join-Path $root 'third_party\lua\LICENSE') (Join-Path $licenses 'lua.txt')
Copy-Item (Join-Path $root 'modules\dataforge\LICENSE_1_0.txt') (Join-Path $licenses 'dataforge.txt')
Copy-Item (Join-Path $root 'src\otok\catalog\site\assets\minisearch.LICENSE.txt') (Join-Path $licenses 'minisearch.txt')
Copy-Item (Join-Path $root 'src\otok\catalog\site\assets\diff.LICENSE.txt') (Join-Path $licenses 'jsdiff.txt')
Get-ChildItem (Join-Path $build "vcpkg_installed\$triplet\share") -Directory |
    Where-Object { $_.Name -ne 'gtest' -and (Test-Path (Join-Path $_.FullName 'copyright')) } |
    ForEach-Object { Copy-Item (Join-Path $_.FullName 'copyright') (Join-Path $licenses "$($_.Name).txt") }

$zip = Join-Path $Output "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# entries named with '/' (Compress-Archive of PowerShell 5.1 writes '\', which unzip takes as part of a file name)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    Get-ChildItem $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry, 'Optimal')
    }
} finally { $archive.Dispose() }
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$zip.sha256", "$hash  $name.zip`n")

Write-Output "${zip}: otok $version, sha256 $hash"
Write-Output "otok.exe needs: $($dlls -join ', ')"
