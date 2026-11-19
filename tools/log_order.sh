#!/usr/bin/env bash
# The Hombre Operating System.
# Copyright © 2026 Adam Waldenberg and Adeptum AB.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Succeeds when each marker occurs, as a fixed string, in the log after the
# previous marker. Prints the first marker out of order and exits 1.

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <log> <marker>..." >&2
    exit 2
fi

log=$1
shift
line=1
for marker in "$@"; do
    if [[ ! -f $log ]]; then
        echo "$marker"
        exit 1
    fi
    found=$(tail -n +"$line" "$log" | grep -nF -m1 -- "$marker" | cut -d: -f1 || true)
    if [[ -z $found ]]; then
        echo "$marker"
        exit 1
    fi
    line=$((line + found))
done
