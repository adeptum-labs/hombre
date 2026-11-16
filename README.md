# The Hombre Operating System

Hombre is an operating system for x86-64 PCs that is compatible with AmigaOS
and MorphOS. It aims to run classic Amiga 680x0 applications alongside native
software, on modern hardware and in virtual machines.

## Goals

- Compatibility with the AmigaOS and MorphOS programming interfaces: exec, dos,
  intuition, graphics and the rest of the familiar libraries.
- Run unmodified Amiga 680x0 applications through a built-in 68k CPU
  translation layer.
- A modern 64-bit foundation, built for today's hardware.
- Boot on real x86-64 machines and run under KVM/QEMU.

## Building and running

Requirements:

- clang 14 or newer (builds the UEFI loader)
- g++ 12 or newer (builds the kernel and the host tests)
- GNU binutils with the i386pep target (links the loader)
- GNU make
- mtools and sgdisk (build the boot disk image)
- QEMU with KVM, and OVMF installed at `/usr/share/OVMF`

Targets:

- `make` builds the loader, the kernel and the bootable disk image
  `build/hombre.img`.
- `make run` boots the image in QEMU with KVM. Kernel messages appear on the
  terminal, which is wired to the serial port COM1.
- `make check` boots the image headless and verifies the messages on the
  serial port, including a boot with the kernel missing and a boot without a
  framebuffer.
- `make test` builds and runs the host unit tests under the address and
  undefined behaviour sanitizers.

## Status

Early development. The machine boots as follows:

1. The UEFI loader, `BOOTX64.EFI`, loads `KERNEL.ELF` from the boot disk.
2. It builds page tables, with all memory mapped in the higher half, and
   passes the kernel a `BootInfo` structure holding the memory map, the
   framebuffer and the ACPI RSDP.
3. The kernel reports on serial port COM1, drops the loader's identity
   mapping, sets up per-CPU data for the boot processor and draws the
   `Hombre` banner on the framebuffer.
4. The kernel halts.

## The name

Hombre was the codename of the next-generation Amiga chipset Commodore was
developing when it went bankrupt in 1994. The chipset was never released;
this operating system borrows its name.
