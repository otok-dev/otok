#!/usr/bin/env bash
# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Builds the self-contained otok of a release for macOS (docs/PLAN.md decision 50): every dependency static from
# vcpkg (triplets/arm64-osx-otok, triplets/x64-osx-otok: deployment target macOS 14.0), one build per architecture,
# joined with lipo. Checks the architectures and that the binary uses only the system's libraries.
#
#   macos-universal.sh [OUTPUT DIR]      (default: build/release-macos in the checkout)
#
# VCPKG_ROOT: the vcpkg checkout (default ~/projects/vcpkg). OTOK_REVISION: the revision to embed when the sources
# have no .git (a source archive). Needs cmake and ninja.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)    # also in a source archive, without .git
out=${1:-$root/build/release-macos}
vcpkg_root=${VCPKG_ROOT:-$HOME/projects/vcpkg}
export MACOSX_DEPLOYMENT_TARGET=14.0
mkdir -p "$out"

slices=()
for arch in arm64:arm64 x64:x86_64; do
    triplet=${arch%%:*}-osx-otok
    machine=${arch##*:}
    build=$out/build-$machine
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$vcpkg_root/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_OVERLAY_TRIPLETS="$root/triplets" \
        -DVCPKG_TARGET_TRIPLET="$triplet" \
        -DCMAKE_OSX_ARCHITECTURES="$machine" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
        -DOTOK_BUILD_TESTS=OFF \
        -DOTOK_REVISION="${OTOK_REVISION:-}"
    cmake --build "$build" --target otok
    slices+=("$build/otok")
done

lipo -create -output "$out/otok" "${slices[@]}"
strip -x "$out/otok"

archs=$(lipo -archs "$out/otok")
[[ $archs == *x86_64* && $archs == *arm64* ]] || { echo "error: architectures '$archs'" >&2; exit 1; }
# the system's libraries only: /usr/lib (libc++, libSystem, libcurl, libz) and /System
foreign=$(otool -L "$out/otok" | grep -E $'^\t' | awk '{print $1}' | grep -v -E '^(/usr/lib/|/System/)' || true)  # the library lines, not the headers of the slices
[[ -z $foreign ]] || { echo "error: the binary links libraries outside the system:"$'\n'"$foreign" >&2; exit 1; }
minos=$(otool -l "$out/otok" | awk '/LC_BUILD_VERSION/ {found=1} found && /minos/ {print $2; exit}')
echo "$out/otok: $archs, macOS $minos and later, $("$out/otok" --version)"
