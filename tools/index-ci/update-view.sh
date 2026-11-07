#!/bin/sh
# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# Keeps the HTTPS view of an otok index (docs/formats/index.md 8) and its catalog site (DESIGN 8) up to date on a
# web server of one's own: run it from a timer (cron, systemd) or a webhook. It fetches the index repository and,
# when its commit moved, runs `otok index-view` and `otok index-catalog` into the web root, which rewrite only the
# files that changed. The catalog fetches the recipe trees into otok's cache (OTOK_CACHE_DIR).
#
#   update-view.sh <repository URL> <checkout directory> <web root>
#
# OTOK: the otok to run (default: otok in PATH). OTOK_VIEW_TOKEN_FILE: a file holding a token for a
# private repository on GitHub; otherwise git uses the machine's credentials.
set -eu

repository=$1
checkout=$2
root=$3
otok=${OTOK:-otok}

git_() {
    if [ -n "${OTOK_VIEW_TOKEN_FILE:-}" ]; then
        # the token goes to git through a credential helper, never through the command line
        git -c credential.helper= \
            -c credential.helper='!f() { echo username=x-access-token; printf "password="; tr -d "\r\n" < "$OTOK_VIEW_TOKEN_FILE"; echo; }; f' \
            "$@"
    else
        git "$@"
    fi
}

# one update at a time: a push arriving meanwhile waits, then finds its commit
exec 9> "$checkout.lock"
flock 9

if [ ! -d "$checkout/.git" ]; then
    git_ clone -q "$repository" "$checkout"
fi
git_ -C "$checkout" fetch -q origin HEAD
commit=$(git -C "$checkout" rev-parse FETCH_HEAD)
stamp="$checkout/.git/otok-view"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$commit" ] && [ -f "$root/index/config.json" ] && [ -f "$root/index.html" ]; then
    exit 0
fi
git -C "$checkout" checkout -q --detach "$commit"
"$otok" index-view "$checkout" --output "$root"
"$otok" index-catalog "$checkout" --output "$root"
echo "$commit" > "$stamp"
