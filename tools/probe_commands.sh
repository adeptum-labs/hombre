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

# Boots build/hombre.img once per command line, types the line at the shell
# prompt and prints what became of it: the library vectors it was missing,
# the console output and how the run ended.
#
# usage: tools/probe_commands.sh [--disk1 <disk>] <disk> "<command line>"...
# <disk> is DF0:, the boot disk; --disk1 puts a second disk in DF1:.
# With PROBE_SCREENSHOT=<file.ppm> the screen is saved after each line.
# PROBE_MOUSE="<commands>" sends ';'-separated QEMU monitor commands, such
# as "mouse_move 10 10;mouse_button 1;mouse_button 0", to a USB mouse once
# the command has started (the pointer starts at 120,100), before that.
#
# Each boot runs on a private copy of the image, so a running "make run"
# does not hold it. A boot ends when the command's end shows on the serial
# console, or after 30 seconds. Lines are typed with the Swedish layout
# the shell uses; letters, digits, space and . : - / * # ? ( ) | are typed.

set -euo pipefail

usage="usage: $0 [--disk1 <disk>] <disk> \"<command line>\"..."
disk1_source=
if [[ ${1:-} == --disk1 ]]; then
    [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
    disk1_source=$2
    shift 2
fi
[[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
disk=$1
shift
command -v socat > /dev/null || { echo "$0: needs socat" >&2; exit 2; }

root=$(cd "$(dirname "$0")/.." && pwd)
image=$root/build/hombre.img
ovmf_dir=/usr/share/OVMF
[[ -f $image ]] || { echo "$0: $image is missing, run make first" >&2; exit 2; }

work=$(mktemp -d)
qemu_pid=
cleanup() {
    [[ -n $qemu_pid ]] && kill "$qemu_pid" 2> /dev/null || true
    [[ -n $qemu_pid ]] && wait "$qemu_pid" 2> /dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
cp "$image" "$work/hombre.img"

# Drive n sits in PCI slot 0x10 + n, as make run attaches them.
drive_args=(-drive "if=none,id=disk0,format=raw,readonly=on,file=$disk"
            -device virtio-blk-pci,disable-legacy=on,drive=disk0,addr=0x10)
if [[ -n $disk1_source ]]; then
    "$root/tools/make_disk.sh" "$disk1_source" "$work/disk1.img"
    drive_args+=(-drive "if=none,id=disk1,format=raw,readonly=on,file=$work/disk1.img"
                 -device virtio-blk-pci,disable-legacy=on,drive=disk1,addr=0x11)
fi

# The QEMU key names that type one character on the Swedish layout.
key_of() {
    local c=$1
    case $c in
        [a-z0-9]) echo "$c" ;;
        [A-Z]) echo "shift-${c,,}" ;;
        ' ') echo spc ;;
        .) echo dot ;;
        :) echo shift-dot ;;
        -) echo slash ;;
        /) echo shift-7 ;;
        '*') echo shift-backslash ;;
        '#') echo shift-3 ;;
        '?') echo shift-minus ;;
        '(') echo shift-8 ;;
        ')') echo shift-9 ;;
        '|') echo alt_r-less ;;
        *) echo "$0: cannot type '$c'" >&2; return 1 ;;
    esac
}

monitor() {
    echo "$1" | socat - "UNIX-CONNECT:$work/monitor.sock" > /dev/null
}

wait_for() {
    local pattern=$1 limit=$2 i
    for ((i = 0; i < limit * 4; i++)); do
        grep -aqE -- "$pattern" "$work/serial.log" 2> /dev/null && return 0
        kill -0 "$qemu_pid" 2> /dev/null || return 1
        sleep 0.25
    done
    return 1
}

for line in "$@"; do
    : > "$work/serial.log"
    cp "$ovmf_dir/OVMF_VARS_4M.fd" "$work/vars.fd"
    qemu-system-x86_64 -enable-kvm -cpu host,-pdpe1gb -machine q35 -m 256M \
        -display none -no-reboot -serial "file:$work/serial.log" \
        -monitor "unix:$work/monitor.sock,server,nowait" \
        -drive if=pflash,format=raw,readonly=on,file="$ovmf_dir/OVMF_CODE_4M.fd" \
        -drive if=pflash,format=raw,file="$work/vars.fd" \
        -drive format=raw,file="$work/hombre.img" \
        "${drive_args[@]}" \
        -device qemu-xhci -device usb-kbd -device usb-mouse > /dev/null 2>&1 &
    qemu_pid=$!
    echo "=== $line"
    if wait_for "shell: ready" 60; then
        for ((i = 0; i < ${#line}; i++)); do
            monitor "sendkey $(key_of "${line:i:1}")"
            sleep 0.1
        done
        monitor "sendkey ret"
        wait_for "m68k: .* (returned|at pc=)|could not load|budget exhausted|shell: unknown" 30 ||
            echo "(no end within 30 s)"
        sleep 0.5
        if [[ -n ${PROBE_MOUSE:-} ]]; then
            IFS=';' read -ra mouse_commands <<< "$PROBE_MOUSE"
            for command in "${mouse_commands[@]}"; do
                monitor "$command"
                sleep 0.25
            done
            sleep 1
        fi
        if [[ -n ${PROBE_SCREENSHOT:-} ]]; then
            # The screen as the command left it, for a program that stays.
            monitor "screendump $PROBE_SCREENSHOT"
            sleep 1
        fi
        sed -n '/^shell: line: /,$p' "$work/serial.log" | tr -d '\r' | head -60
    else
        echo "(the shell did not start)"
        tail -5 "$work/serial.log"
    fi
    kill "$qemu_pid" 2> /dev/null || true
    wait "$qemu_pid" 2> /dev/null || true
    qemu_pid=
done
