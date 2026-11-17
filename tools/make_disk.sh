#!/usr/bin/env bash
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

# Copies <src> to <dst> and pads it with zero bytes to a whole number of
# 512-byte sectors, so it can be attached as a raw drive. The source may be
# any path, spaces and shell characters included.
set -euo pipefail

usage="usage: $0 <src> <dst>"
[[ $# -eq 2 ]] || { echo "$usage" >&2; exit 2; }
src=$1
dst=$2

[[ -f $src ]] || { echo "$0: not a file: $src" >&2; exit 1; }
size=$(stat -c %s -- "$src")
[[ $size -gt 0 ]] || { echo "$0: empty file: $src" >&2; exit 1; }
padded=$(( (size + 511) / 512 * 512 ))

# Built next to the destination and renamed, so a failure never leaves a
# half-written drive behind.
tmp="$dst.tmp"
trap 'rm -f -- "$tmp"' EXIT
cp -- "$src" "$tmp"
truncate -s "$padded" -- "$tmp"
mv -f -- "$tmp" "$dst"
trap - EXIT
