#!/bin/bash
# The Hombre Operating System.
# Copyright © 2026 Adam Waldenberg and Adeptum AB.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Website: https://www.adeptum.se
# Contact: info@adeptum.se

# Unpacks every DMS archive below a directory with both xdms and adftool and
# compares the two images byte for byte. Prints OK or DIFF (with the reason)
# per archive and exits non-zero if any archive differs.
#
#   tools/dms_compare.sh <dir>

set -u

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
    echo "usage: $0 <directory with .dms files>" >&2
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
adftool="$root/build/tools/adftool"
tmp="$root/build/dms-compare"
reference="$tmp/xdms.adf"
ours="$tmp/adftool.adf"
log="$tmp/log"

if [ ! -x "$adftool" ]; then
    echo "$0: $adftool is missing; run make build/tools/adftool" >&2
    exit 2
fi
mkdir -p "$tmp"

total=0
differ=0
while IFS= read -r -d '' file; do
    total=$((total + 1))
    rm -f "$reference" "$ours" "$log"
    reason=
    if ! xdms -q u "$file" "+$reference" >"$log" 2>&1; then
        reason="xdms failed: $(tr '\n' ' ' <"$log")"
    elif ! "$adftool" unpack "$file" "$ours" 2>"$log"; then
        reason="adftool: $(tr '\n' ' ' <"$log")"
    elif ! cmp "$reference" "$ours" >"$log" 2>&1; then
        reason="$(tr '\n' ' ' <"$log")"
    fi
    if [ -z "$reason" ]; then
        printf 'OK   %s\n' "$file"
    else
        printf 'DIFF %s: %s\n' "$file" "$reason"
        differ=$((differ + 1))
    fi
done < <(find "$1" -type f -iname '*.dms' -print0 | sort -z)

rm -f "$reference" "$ours" "$log"
echo "$total archives, $differ differ"
[ "$total" -gt 0 ] && [ "$differ" -eq 0 ]
