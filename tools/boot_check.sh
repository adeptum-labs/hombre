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

# Boots build/hombre.img headless under KVM and succeeds only when every
# marker given on the command line shows up on the serial console. With
# --fault-test <n> it boots build/hombre-fault<n>.img instead.
set -euo pipefail

usage="usage: $0 [--no-kernel] [--no-gop] [--fault-test <n>] <marker>..."
timeout_seconds=60
ovmf_dir=/usr/share/OVMF
build_dir="$(dirname "$0")/../build"

image_name=hombre.img
vga_args=()
while [[ ${1:-} == --* ]]; do
    case $1 in
        --no-kernel) image_name=hombre-nokernel.img ;;
        # Without a display adapter the q35 machine has no GOP, so the
        # loader hands over a BootInfo without a framebuffer.
        --no-gop) vga_args=(-vga none) ;;
        --fault-test)
            [[ ${2:-} =~ ^[0-9]+$ ]] || { echo "$usage" >&2; exit 2; }
            image_name=hombre-fault$2.img
            shift
            ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
    shift
done
[[ $# -gt 0 ]] || { echo "$usage" >&2; exit 2; }

image=$build_dir/$image_name
[[ -f $image ]] || { echo "missing $image (run make image, image-nokernel or image-fault<n>)" >&2; exit 1; }

serial_log=$build_dir/serial.log
: > "$serial_log"
cp "$ovmf_dir/OVMF_VARS_4M.fd" "$build_dir/OVMF_VARS.fd"

# -pdpe1gb: with 1 GiB pages exposed, KVM on this host fails with an internal
# error while the firmware is still in 32-bit mode.
qemu-system-x86_64 -enable-kvm -cpu host,-pdpe1gb -machine q35 -m 256M \
    "${vga_args[@]}" -display none -no-reboot -serial "file:$serial_log" \
    -drive if=pflash,format=raw,readonly=on,file="$ovmf_dir/OVMF_CODE_4M.fd" \
    -drive if=pflash,format=raw,file="$build_dir/OVMF_VARS.fd" \
    -drive format=raw,file="$image" &
qemu_pid=$!
trap 'kill "$qemu_pid" 2>/dev/null || true; wait "$qemu_pid" 2>/dev/null || true' EXIT

all_markers_present() {
    local marker
    for marker in "$@"; do
        grep -qF -- "$marker" "$serial_log" || return 1
    done
}

for ((i = 0; i < timeout_seconds * 4; i++)); do
    all_markers_present "$@" && exit 0
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 0.25
done

all_markers_present "$@" && exit 0
echo "markers missing from $serial_log:" >&2
for marker in "$@"; do
    grep -qF -- "$marker" "$serial_log" || echo "  $marker" >&2
done
exit 1
