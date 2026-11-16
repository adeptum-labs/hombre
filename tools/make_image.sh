#!/usr/bin/env bash
# Builds a 64 MiB GPT disk image with one FAT32 EFI system partition that
# holds the loader as the removable-media boot file.
set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: $0 <image> <BOOTX64.EFI>" >&2; exit 2; }
image=$1
loader=$2

size_mib=64
partition_start_sector=2048
partition_offset=$((partition_start_sector * 512))

mkdir -p "$(dirname "$image")"
rm -f "$image"
truncate -s ${size_mib}M "$image"
sgdisk -n 1:${partition_start_sector}:0 -t 1:EF00 "$image" > /dev/null

partition_end_sector=$(sgdisk -i 1 "$image" | awk '/^Last sector:/ {print $3}')
partition_sectors=$((partition_end_sector - partition_start_sector + 1))

fat="$image@@$partition_offset"
mformat -i "$fat" -F -T "$partition_sectors" ::
mmd -i "$fat" ::/EFI ::/EFI/BOOT
mcopy -i "$fat" "$loader" ::/EFI/BOOT/BOOTX64.EFI
