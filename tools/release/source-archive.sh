#!/bin/sh
# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Makes otok-<version>.tar.gz, the source archive of a release (docs/PLAN.md decision 50): the commit HEAD with the
# dataforge submodule at the commit HEAD records, which the archives of GitHub and Forgejo leave out. The Homebrew
# formula builds from it. Line endings are those of the repository (LF), whatever core.autocrlf says.
#
#   source-archive.sh [OUTPUT DIR]        (default: the current directory)
set -eu

root=$(git rev-parse --show-toplevel)
out=$(cd "${1:-.}" && pwd)
version=$(sed -n 's/.*std::string_view version = "\([0-9.]*\)".*/\1/p' "$root/src/otok/core/build_info.hpp")
[ -n "$version" ] || { echo "no version in src/otok/core/build_info.hpp" >&2; exit 1; }
if [ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]; then
    echo "warning: the working tree has changes; the archive holds HEAD" >&2
fi

name=otok-$version
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -c core.autocrlf=false -C "$root" archive --format=tar --prefix="$name/" HEAD | tar -xf - -C "$work"
git -C "$root" ls-tree HEAD modules/ | while read -r mode type commit path; do
    [ "$type" = commit ] || continue
    git -c core.autocrlf=false -C "$root/$path" archive --format=tar --prefix="$name/$path/" "$commit" | tar -xf - -C "$work"
done
tar -cf - -C "$work" "$name" | gzip -n -9 > "$out/$name.tar.gz"
echo "$out/$name.tar.gz"
shasum -a 256 "$out/$name.tar.gz" 2>/dev/null || sha256sum "$out/$name.tar.gz"
