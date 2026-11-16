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

# Builds a 64 MiB GPT disk image with one FAT32 EFI system partition that
# holds the loader as the removable-media boot file and, when given, the
# kernel as KERNEL.ELF in the root directory.
set -euo pipefail

[[ $# -eq 2 || $# -eq 3 ]] || { echo "usage: $0 <image> <BOOTX64.EFI> [KERNEL.ELF]" >&2; exit 2; }
image=$1
loader=$2
kernel=${3:-}

size_mib=64
partition_start_sector=2048
partition_offset=$((partition_start_sector * 512))

mkdir -p "$(dirname "$image")"
rm -f "$image"
truncate -s ${size_mib}M "$image"
sgdisk -n 1:${partition_start_sector}:0 -t 1:EF00 "$image" > /dev/null

partition_end_sector=$(LC_ALL=C sgdisk -i 1 "$image" | awk '/^Last sector:/ {print $3}')
partition_sectors=$((partition_end_sector - partition_start_sector + 1))

fat="$image@@$partition_offset"
mformat -i "$fat" -F -T "$partition_sectors" ::
mmd -i "$fat" ::/EFI ::/EFI/BOOT
mcopy -i "$fat" "$loader" ::/EFI/BOOT/BOOTX64.EFI
if [[ -n $kernel ]]; then
    mcopy -i "$fat" "$kernel" ::/KERNEL.ELF
fi
