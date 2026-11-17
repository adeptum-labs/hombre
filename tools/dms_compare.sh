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
# compares the two images byte for byte. Then, for an image unadf can mount,
# compares what adftool's file system reader says with unadf: the volume name
# and file system type, the names in the root directory, and
# S/Startup-Sequence when the disk has one. Prints OK or DIFF (with the
# reason) per archive and exits non-zero if any archive differs.
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

# The names in an unadf listing: the part after the date and the time, which
# is preceded by a size for a file and by blanks only for a directory.
unadf_names() {
    LC_ALL=C sed -n 's|^ *\([0-9][0-9]* \+\)\?[0-9]\{4\}/[0-9][0-9]/[0-9][0-9] \+[0-9]\{1,2\}:[0-9][0-9]:[0-9][0-9]  ||p' "$1" |
        LC_ALL=C sort
}

# Compares the file system reader with unadf on the image xdms wrote. Sets
# fs_reason on a difference and fs_note when there was nothing to compare
# or something was left out.
compare_fs() {
    local file=$1
    fs_reason=
    fs_note=
    local theirs="$tmp/unadf.list" tree="$tmp/unadf.tree" names="$tmp/names"
    if ! unadf -l "$reference" >"$theirs" 2>/dev/null || ! grep -q '^Volume : ' "$theirs"; then
        fs_note="file system not compared, unadf cannot mount it"
        return
    fi

    local volume type our_info
    volume=$(sed -n 's/^Volume : .*"\(.*\)" between sectors.*/\1/p' "$theirs")
    type=OFS
    if grep -q '^Volume : .*sectors \[[0-9-]*\]\. FFS' "$theirs"; then
        type=FFS
    fi
    if ! our_info=$("$adftool" info "$file" 2>"$log"); then
        fs_reason="adftool info: $(tr '\n' ' ' <"$log")"
        return
    fi
    if [ "$(printf '%s\n' "$our_info" | sed -n 's/^volume: //p')" != "$volume" ] ||
       [ "$(printf '%s\n' "$our_info" | sed -n 's/^type: //p')" != "$type" ]; then
        fs_reason="volume differs: adftool says $(printf '%s' "$our_info" | tr '\n' ' '), unadf says $volume $type"
        return
    fi

    if ! "$adftool" ls "$file" 2>"$log" | cut -c13- | LC_ALL=C sort >"$names" ||
       [ -s "$log" ]; then
        fs_reason="adftool ls: $(tr '\n' ' ' <"$log")"
        return
    fi
    if ! unadf_names "$theirs" | cmp -s - "$names"; then
        fs_reason="root directory names differ from unadf"
        return
    fi

    # Only a disk that has the file, according to unadf, is asked for it.
    if unadf -r -l "$reference" 2>/dev/null >"$tree" && grep -q '  S/Startup-Sequence$' "$tree"; then
        if ! unadf -p "$reference" S/Startup-Sequence 2>/dev/null >"$tmp/startup.unadf"; then
            fs_note="startup sequence not compared, unadf failed to extract it"
        elif ! "$adftool" cat "$file" S/Startup-Sequence >"$tmp/startup.ours" 2>"$log"; then
            fs_reason="adftool cat: $(tr '\n' ' ' <"$log")"
        elif ! cmp -s "$tmp/startup.unadf" "$tmp/startup.ours"; then
            fs_reason="S/Startup-Sequence differs from unadf"
        fi
    fi
}

total=0
differ=0
while IFS= read -r -d '' file; do
    total=$((total + 1))
    rm -f "$reference" "$ours" "$log"
    reason=
    note=
    if ! xdms -q u "$file" "+$reference" >"$log" 2>&1; then
        reason="xdms failed: $(tr '\n' ' ' <"$log")"
    elif ! "$adftool" unpack "$file" "$ours" 2>"$log"; then
        reason="adftool: $(tr '\n' ' ' <"$log")"
    elif ! cmp "$reference" "$ours" >"$log" 2>&1; then
        reason="$(tr '\n' ' ' <"$log")"
    else
        compare_fs "$file"
        reason=$fs_reason
        note=$fs_note
    fi
    if [ -z "$reason" ] && [ -z "$note" ]; then
        printf 'OK   %s\n' "$file"
    elif [ -z "$reason" ]; then
        printf 'OK   %s (%s)\n' "$file" "$note"
    else
        printf 'DIFF %s: %s\n' "$file" "$reason"
        differ=$((differ + 1))
    fi
done < <(find "$1" -type f -iname '*.dms' -print0 | sort -z)

rm -f "$reference" "$ours" "$log" "$tmp"/unadf.list "$tmp"/unadf.tree "$tmp"/names \
    "$tmp"/startup.unadf "$tmp"/startup.ours
echo "$total archives, $differ differ"
[ "$total" -gt 0 ] && [ "$differ" -eq 0 ]
