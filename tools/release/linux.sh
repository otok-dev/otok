#!/usr/bin/env bash
# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Builds the otok of a release for Linux (docs/PLAN.md decisions 99 and 104): one static binary for every distribution,
# made on Alpine with its GCC, musl linked in, every dependency static from vcpkg (triplets/<arch>-linux-musl-otok).
# Checks that the binary loads nothing and runs, then packs otok-<version>-linux-<arch>.tar.gz (otok, its license, the
# licenses of what it carries) and its .sha256. The release workflow (.github/workflows/release.yml) runs it in a
# container of Alpine on x64 and arm64.
#
#   linux.sh [OUTPUT DIR]      (default: build/release-linux in the checkout)
#
# The architecture is the machine's: x64 (x86_64) or arm64 (aarch64). VCPKG_ROOT: the vcpkg checkout (default
# ~/projects/vcpkg). OTOK_REVISION: the revision to embed when the sources have no .git (a source archive). Needs cmake,
# ninja (on Alpine ninja-is-really-ninja: its `ninja` is samurai, which vcpkg's meson ports fail with) and g++ of a
# musl system.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)    # also in a source archive, without .git
out=${1:-$root/build/release-linux}
vcpkg_root=${VCPKG_ROOT:-$HOME/projects/vcpkg}
case $(uname -m) in
    x86_64) arch=x64 ;;
    aarch64 | arm64) arch=arm64 ;;
    *) echo "error: no release binary for $(uname -m)" >&2; exit 1 ;;
esac
triplet=$arch-linux-musl-otok
# the C library of a static binary is the one linked in: glibc's needs itself at run time (NSS, iconv), musl's does not
ls /lib/ld-musl-*.so.1 >/dev/null 2>&1 || { echo "error: not a musl system (Alpine): the release binary links musl" >&2; exit 1; }
mkdir -p "$out"
out=$(cd "$out" && pwd)

# -static: nothing loaded at run time, so one binary runs on every distribution, old or new, glibc or musl.
# stack-size: musl gives threads 128 KiB unless the binary asks for more (PT_GNU_STACK); otok's threads resolve, parse
# and run Lua as deep as the main thread, which has the 8 MiB of glibc's threads.
build=$out/build
cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,-z,stack-size=8388608" \
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg_root/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_OVERLAY_TRIPLETS="$root/triplets" \
    -DVCPKG_TARGET_TRIPLET="$triplet" \
    -DOTOK_BUILD_TESTS=OFF \
    -DOTOK_EXTRA_NOTICES="$root/tools/release/notices" \
    -DOTOK_REVISION="${OTOK_REVISION:-}"
cmake --build "$build" --target otok
exe=$build/otok
strip "$exe"

# static: no interpreter, no library to load
if readelf -l "$exe" | grep -q 'INTERP' || readelf -d "$exe" | grep -q '(NEEDED)'; then
    echo "error: $exe is not static:" >&2
    readelf -d "$exe" | grep '(NEEDED)' >&2 || true
    exit 1
fi
stack=$(readelf -lW "$exe" | awk '$1 == "GNU_STACK" { print $6 }')
[[ $((stack)) -ge 8388608 ]] || { echo "error: $exe asks threads for a stack of $stack bytes, not 8 MiB" >&2; exit 1; }

version=$("$exe" --version | sed -n '1s/^otok \([^ ]*\).*/\1/p')
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]] || { echo "error: no version from $exe --version" >&2; exit 1; }

# the archive: otok, otok's license and its exception, the licenses of the libraries built into it (not gtest: tests
# only), musl's among them (tools/release/notices: Alpine installs no license of musl)
name=otok-$version-linux-$arch
stage=$out/$name
rm -rf "$stage"
mkdir -p "$stage/licenses"
cp "$exe" "$root/LICENSE" "$root/LICENSE-EXCEPTION" "$stage/"
cp "$root/third_party/lua/LICENSE" "$stage/licenses/lua.txt"
cp "$root/modules/dataforge/LICENSE_1_0.txt" "$stage/licenses/dataforge.txt"
cp "$root/src/otok/catalog/site/assets/minisearch.LICENSE.txt" "$stage/licenses/minisearch.txt"
cp "$root/src/otok/catalog/site/assets/diff.LICENSE.txt" "$stage/licenses/jsdiff.txt"
cp "$root"/tools/release/notices/*.txt "$stage/licenses/"
for dir in "$build/vcpkg_installed/$triplet/share"/*/; do
    port=$(basename "$dir")
    [[ $port != gtest && -f $dir/copyright ]] && cp "$dir/copyright" "$stage/licenses/$port.txt"
done
tar -cf - -C "$out" "$name" | gzip -n -9 > "$out/$name.tar.gz"
(cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")

echo "$out/$name.tar.gz: $("$exe" --version | head -1)"
