#!/usr/bin/env bash
# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Builds the otok of a release for Linux x64 (docs/PLAN.md decision 99): every dependency static from vcpkg
# (triplets/x64-linux-otok), with GCC. Checks that the binary loads only the C and C++ runtime libraries of the system,
# then packs otok-<version>-linux-x64.tar.gz (otok, its license, the licenses of what it carries) and its .sha256.
# The release workflow (.github/workflows/release.yml) runs it in a container of ubuntu:26.04, the system of the
# index's CI.
#
#   linux-x64.sh [OUTPUT DIR]      (default: build/release-linux in the checkout)
#
# VCPKG_ROOT: the vcpkg checkout (default ~/projects/vcpkg). OTOK_REVISION: the revision to embed when the sources
# have no .git (a source archive). Needs cmake, ninja and g++.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)    # also in a source archive, without .git
out=${1:-$root/build/release-linux}
vcpkg_root=${VCPKG_ROOT:-$HOME/projects/vcpkg}
triplet=x64-linux-otok
mkdir -p "$out"
out=$(cd "$out" && pwd)

build=$out/build
cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg_root/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_OVERLAY_TRIPLETS="$root/triplets" \
    -DVCPKG_TARGET_TRIPLET="$triplet" \
    -DOTOK_BUILD_TESTS=OFF \
    -DOTOK_REVISION="${OTOK_REVISION:-}"
cmake --build "$build" --target otok
exe=$build/otok
strip "$exe"

# the C and C++ runtime of the system only: every other library is built into otok
needed=$(readelf -d "$exe" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
foreign=$(echo "$needed" | grep -v -E '^(libc|libm|libstdc\+\+|libgcc_s|libpthread|libdl|librt|ld-linux-x86-64)\.so\.[0-9]+$' || true)
[[ -z $foreign ]] || { echo "error: otok loads libraries outside the system's runtime:"$'\n'"$foreign" >&2; exit 1; }

version=$("$exe" --version | sed -n '1s/^otok \([^ ]*\).*/\1/p')
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]] || { echo "error: no version from $exe --version" >&2; exit 1; }

# the archive: otok, otok's license and its exception, the licenses of the libraries built into it (not gtest: tests only)
name=otok-$version-linux-x64
stage=$out/$name
rm -rf "$stage"
mkdir -p "$stage/licenses"
cp "$exe" "$root/LICENSE" "$root/LICENSE-EXCEPTION" "$stage/"
cp "$root/third_party/lua/LICENSE" "$stage/licenses/lua.txt"
cp "$root/modules/dataforge/LICENSE_1_0.txt" "$stage/licenses/dataforge.txt"
cp "$root/src/otok/catalog/site/assets/minisearch.LICENSE.txt" "$stage/licenses/minisearch.txt"
cp "$root/src/otok/catalog/site/assets/diff.LICENSE.txt" "$stage/licenses/jsdiff.txt"
for dir in "$build/vcpkg_installed/$triplet/share"/*/; do
    port=$(basename "$dir")
    [[ $port != gtest && -f $dir/copyright ]] && cp "$dir/copyright" "$stage/licenses/$port.txt"
done
tar -cf - -C "$out" "$name" | gzip -n -9 > "$out/$name.tar.gz"
(cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")

echo "$out/$name.tar.gz: $("$exe" --version | head -1)"
echo "otok loads: $(echo $needed)"
