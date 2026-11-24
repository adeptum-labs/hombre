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
#
# build/disk.img (made by make disk-image) is attached as a read-only
# virtio-blk drive. --disk <file> attaches a copy of that file padded to
# whole sectors instead, and --no-disk attaches no drive.
# --disk1, --disk2 and --disk3 <file> attach copies of more disks as DF1:,
# DF2: and DF3:. --hdf <file> attaches that hard disk image itself, writable, as
# the first hard disk, so what the boot writes stays in the file.
# --hdf-bus ahci|nvme|nvme4k attaches it to the SATA controller, to an NVMe
# controller, or to one with 4096-byte sectors, instead of virtio-blk.
#
# --usb-kbd attaches an xHCI controller with a USB keyboard. --keys "<names>"
# implies it and, once "shell: ready" shows on the serial console, types the
# space-separated QEMU key names (for example "v e r s i o n ret") through
# the QEMU monitor, which needs socat. Key names are key positions: on the
# Swedish layout the keys to the right of p, l and the apostrophe key are
# bracket_left (å), semicolon (ö) and apostrophe (ä).
#
# --no-startup implies --usb-kbd and types Ctrl+D from QEMU's start until
# the Startup-Sequence ends, which stops the disk's S:Startup-Sequence and
# leaves the boot shell open.
#
# --mouse-after <marker> and --mouse "<commands>" must be given together.
# --sound <wav> attaches an Intel HD Audio controller with an output codec
# whose sound QEMU writes to the WAV file <wav>.
# --mouse implies the USB devices of --usb-kbd and also attaches a USB mouse
# (-device usb-mouse); once <marker> shows on the serial console and any
# --keys have been sent, the ';'-separated HMP commands (for example
# "mouse_move 10 10;mouse_button 1") are sent to the QEMU monitor 0.25 s
# apart.
set -euo pipefail

usage="usage: $0 [--no-kernel] [--no-gop] [--fault-test <n>] [--disk <file> | --no-disk] [--disk1 <file>] [--disk2 <file>] [--disk3 <file>] [--hdf <file> [--hdf-bus ahci|nvme|nvme4k]] [--usb-kbd] [--no-startup] [--keys <names>] [--mouse-after <marker> --mouse <commands>] [--sound <wav>] <marker>..."
timeout_seconds=60
ovmf_dir=/usr/share/OVMF
tools_dir="$(dirname "$0")"
build_dir="$tools_dir/../build"

image_name=hombre.img
vga_args=()
usb_args=()
sound_args=()
usb_kbd=false
usb_mouse=false
keys=
mouse_after=
mouse=
disk_mode=default
disk_source=
disk1_source=
disk2_source=
disk3_source=
hdf=
hdf_bus=virtio
no_startup=false
while [[ ${1:-} == --* ]]; do
    case $1 in
        --disk)
            [[ $# -ge 2 && $2 != --* ]] || { echo "$usage" >&2; exit 2; }
            disk_mode=file
            disk_source=$2
            shift
            ;;
        --no-disk) disk_mode=none ;;
        --disk1)
            [[ $# -ge 2 && $2 != --* ]] || { echo "$usage" >&2; exit 2; }
            disk1_source=$2
            shift
            ;;
        --disk2)
            [[ $# -ge 2 && $2 != --* ]] || { echo "$usage" >&2; exit 2; }
            disk2_source=$2
            shift
            ;;
        --disk3)
            [[ $# -ge 2 && $2 != --* ]] || { echo "$usage" >&2; exit 2; }
            disk3_source=$2
            shift
            ;;
        --hdf)
            [[ $# -ge 2 && $2 != --* ]] || { echo "$usage" >&2; exit 2; }
            hdf=$2
            shift
            ;;
        --hdf-bus)
            [[ $# -ge 2 && $2 != --* ]] || { echo "$usage" >&2; exit 2; }
            hdf_bus=$2
            shift
            ;;
        --usb-kbd) usb_kbd=true ;;
        --no-startup)
            usb_kbd=true
            no_startup=true
            ;;
        --keys)
            [[ $# -ge 2 && $2 != --* ]] || { echo "$usage" >&2; exit 2; }
            usb_kbd=true
            keys=$2
            shift
            ;;
        --mouse-after)
            [[ $# -ge 2 && $2 != --* && -n $2 ]] || { echo "$usage" >&2; exit 2; }
            mouse_after=$2
            shift
            ;;
        --mouse)
            [[ $# -ge 2 && $2 != --* && -n $2 ]] || { echo "$usage" >&2; exit 2; }
            usb_kbd=true
            usb_mouse=true
            mouse=$2
            shift
            ;;
        --sound)
            [[ $# -ge 2 && $2 != --* && -n $2 ]] || { echo "$usage" >&2; exit 2; }
            sound_args=(-audiodev "wav,id=snd0,path=$2" -device ich9-intel-hda
                        -device hda-output,audiodev=snd0)
            shift
            ;;
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
[[ (-z $mouse_after && -z $mouse) || (-n $mouse_after && -n $mouse) ]] || { echo "$usage" >&2; exit 2; }

if [[ $usb_kbd == true ]]; then
    usb_args=(-device qemu-xhci -device usb-kbd)
fi
if [[ $usb_mouse == true ]]; then
    usb_args+=(-device usb-mouse)
fi

image=$build_dir/$image_name
[[ -f $image ]] || { echo "missing $image (run make image, image-nokernel or image-fault<n>)" >&2; exit 1; }

disk_args=()
case $disk_mode in
    default)
        disk=$build_dir/disk.img
        [[ -f $disk ]] || { echo "missing $disk (run make disk-image)" >&2; exit 1; }
        ;;
    file)
        disk=$build_dir/boot-check-disk.img
        "$tools_dir/make_disk.sh" "$disk_source" "$disk"
        ;;
    none) disk= ;;
esac
# Drive n sits in PCI slot 0x10 + n: DF0: the boot disk, DF1: the second.
if [[ -n $disk ]]; then
    disk_args=(-drive "if=none,id=disk0,format=raw,readonly=on,file=$disk"
               -device virtio-blk-pci,disable-legacy=on,drive=disk0,addr=0x10)
fi
if [[ -n $disk1_source ]]; then
    disk1=$build_dir/boot-check-disk1.img
    "$tools_dir/make_disk.sh" "$disk1_source" "$disk1"
    disk_args+=(-drive "if=none,id=disk1,format=raw,readonly=on,file=$disk1"
                -device virtio-blk-pci,disable-legacy=on,drive=disk1,addr=0x11)
fi
if [[ -n $disk2_source ]]; then
    disk2=$build_dir/boot-check-disk2.img
    "$tools_dir/make_disk.sh" "$disk2_source" "$disk2"
    disk_args+=(-drive "if=none,id=disk2,format=raw,readonly=on,file=$disk2"
                -device virtio-blk-pci,disable-legacy=on,drive=disk2,addr=0x12)
fi
if [[ -n $disk3_source ]]; then
    disk3=$build_dir/boot-check-disk3.img
    "$tools_dir/make_disk.sh" "$disk3_source" "$disk3"
    disk_args+=(-drive "if=none,id=disk3,format=raw,readonly=on,file=$disk3"
                -device virtio-blk-pci,disable-legacy=on,drive=disk3,addr=0x13)
fi
# A virtio hard disk sits in PCI slot 0x18; the others go on the q35
# machine's SATA controller or an NVMe controller of their own.
if [[ -n $hdf ]]; then
    [[ -f $hdf ]] || { echo "missing hard disk image $hdf" >&2; exit 1; }
    disk_args+=(-drive "if=none,id=hd0,format=raw,file=${hdf//,/,,}")
    case $hdf_bus in
        virtio) disk_args+=(-device virtio-blk-pci,disable-legacy=on,drive=hd0,addr=0x18) ;;
        ahci) disk_args+=(-device ide-hd,drive=hd0,bus=ide.1) ;;
        nvme) disk_args+=(-device nvme,serial=hombre0,drive=hd0) ;;
        nvme4k) disk_args+=(-device nvme,id=nvme0,serial=hombre0
                            -device nvme-ns,drive=hd0,bus=nvme0,logical_block_size=4096,physical_block_size=4096) ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
fi

serial_log=$build_dir/serial.log
monitor_socket=
monitor_args=()
if [[ -n $keys || -n $mouse || $no_startup == true ]]; then
    command -v socat > /dev/null || { echo "$0: --keys, --mouse and --no-startup need socat" >&2; exit 1; }
    monitor_socket=$build_dir/monitor.$$.sock
    monitor_args=(-monitor "unix:$monitor_socket,server,nowait")
fi
: > "$serial_log"
# Firmware variables of this boot's own, and the boot image as a snapshot,
# so that make run can boot beside it.
ovmf_vars=$build_dir/OVMF_VARS.$$.fd
cp "$ovmf_dir/OVMF_VARS_4M.fd" "$ovmf_vars"

# -pdpe1gb: with 1 GiB pages exposed, KVM on this host fails with an internal
# error while the firmware is still in 32-bit mode.
qemu-system-x86_64 -enable-kvm -cpu host,-pdpe1gb -machine q35 -m 1G \
    "${vga_args[@]}" -display none -no-reboot -serial "file:$serial_log" \
    -drive if=pflash,format=raw,readonly=on,file="$ovmf_dir/OVMF_CODE_4M.fd" \
    -drive if=pflash,format=raw,file="$ovmf_vars" \
    -drive format=raw,snapshot=on,file="$image" "${disk_args[@]}" "${usb_args[@]}" \
    "${sound_args[@]}" "${monitor_args[@]}" &
qemu_pid=$!
trap 'kill "$qemu_pid" 2>/dev/null || true; wait "$qemu_pid" 2>/dev/null || true; rm -f "$monitor_socket" "$ovmf_vars"' EXIT

# Sends one HMP command, then waits 0.25 s. Every command goes over one
# connection to the monitor, opened by the first: a connection per command
# can be accepted only after its client gave up and closed it, when QEMU's
# main loop stalls, and QEMU then drops the command unread.
monitor_fd=
monitor_send() {
    if [[ -z $monitor_fd ]]; then
        exec {monitor_fd}> >(exec socat - "UNIX-CONNECT:$monitor_socket" > /dev/null)
    fi
    echo "$1" >&"$monitor_fd" || return 1
    sleep 0.25
}
trap '' PIPE

# Types the keys once the shell reads the keyboard. Best effort: a missing
# marker is reported with the others at the end.
send_keys() {
    local key
    for key in $keys; do
        monitor_send "sendkey $key" || return 1
    done
}
keys_sent=false

# Sends the ';'-separated HMP mouse commands once --mouse-after shows up.
send_mouse() {
    local cmds cmd
    IFS=';' read -ra cmds <<< "$mouse"
    for cmd in "${cmds[@]}"; do
        [[ -n $cmd ]] || continue
        monitor_send "$cmd" || return 1
    done
}
mouse_sent=false

all_markers_present() {
    local marker
    for marker in "$@"; do
        grep -qF -- "$marker" "$serial_log" || return 1
    done
}

for ((i = 0; i < timeout_seconds * 4; i++)); do
    all_markers_present "$@" && exit 0
    # Ctrl+D again and again from QEMU's start until the Startup-Sequence is
    # over. QEMU queues the keys until the kernel reads the keyboard, so one
    # is waiting when the script starts, however quickly the boot gets
    # there; keys sent before the kernel resets the keyboard are lost.
    if [[ $no_startup == true && -S $monitor_socket ]] &&
       ! grep -qF -- "startup: ended" "$serial_log"; then
        monitor_send "sendkey ctrl-d" || true
    fi
    if [[ -n $keys && $keys_sent == false ]] && grep -qF -- "shell: ready" "$serial_log"; then
        keys_sent=true
        send_keys || true
    fi
    if [[ -n $mouse && $mouse_sent == false && ( -z $keys || $keys_sent == true ) ]] &&
       grep -qF -- "$mouse_after" "$serial_log"; then
        mouse_sent=true
        send_mouse || true
    fi
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 0.25
done

all_markers_present "$@" && exit 0
echo "markers missing from $serial_log:" >&2
for marker in "$@"; do
    grep -qF -- "$marker" "$serial_log" || echo "  $marker" >&2
done
exit 1
