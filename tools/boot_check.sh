#!/usr/bin/env bash
# Boots build/hombre.img headless under KVM and succeeds only when every
# marker given on the command line shows up on the serial console.
set -euo pipefail

usage="usage: $0 [--no-kernel] [--no-gop] <marker>..."
timeout_seconds=20
ovmf_dir=/usr/share/OVMF
build_dir="$(dirname "$0")/../build"

# TODO: select the kernel-less and GOP-less machine variants.
while [[ ${1:-} == --* ]]; do
    case $1 in
        --no-kernel | --no-gop) ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
    shift
done
[[ $# -gt 0 ]] || { echo "$usage" >&2; exit 2; }

[[ -f $build_dir/hombre.img ]] || { echo "missing $build_dir/hombre.img" >&2; exit 1; }

serial_log=$build_dir/serial.log
: > "$serial_log"
cp "$ovmf_dir/OVMF_VARS_4M.fd" "$build_dir/OVMF_VARS.fd"

# -pdpe1gb: with 1 GiB pages exposed, KVM on this host fails with an internal
# error while the firmware is still in 32-bit mode.
qemu-system-x86_64 -enable-kvm -cpu host,-pdpe1gb -machine q35 -m 256M \
    -display none -no-reboot -serial "file:$serial_log" \
    -drive if=pflash,format=raw,readonly=on,file="$ovmf_dir/OVMF_CODE_4M.fd" \
    -drive if=pflash,format=raw,file="$build_dir/OVMF_VARS.fd" \
    -drive format=raw,file="$build_dir/hombre.img" &
qemu_pid=$!
trap 'kill "$qemu_pid" 2>/dev/null || true' EXIT

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
