#!/bin/sh
# Otok (c) Alexander A Pototskiy
# SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
#
# What the webhook and the fallback timer run: update-view.sh with this server's settings, logged.
dir=$HOME/.local/share/otok-view
export OTOK="$dir/otok"
export OTOK_VIEW_TOKEN_FILE="$HOME/.config/otok-dev/github-token"
export OTOK_CACHE_DIR="$HOME/.cache/otok-view"
exec >> "$dir/update.log" 2>&1
echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) update"
exec "$dir/update-view.sh" https://github.com/otok-dev/index "$dir/index" /var/www/otok-view
