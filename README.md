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
- `xdms` and `unadf`, only for `make test-dms`

Targets:

- `make` builds the loader, the kernel and the bootable disk image
  `build/hombre.img`.
- `make run` boots the image in QEMU with KVM. Kernel messages appear on the
  terminal, which is wired to the serial port COM1. The kernel gets a
  virtio-blk disk: a generated test disk by default, or the file given as
  `DISK=`, for example `make run DISK="path/to/disk.dms"`. The file may be
  a DMS archive or a raw ADF image, and its path may contain spaces.
- `make check` boots the image headless and verifies the messages on the
  serial port. The main boot attaches the generated test disk and verifies
  the volume, the root directory and the startup sequence read from it. It
  also boots a drive filled with zero bytes, a test disk with a corrupt
  track and a machine without a disk, a boot with the kernel missing and a
  boot without a framebuffer. It also boots kernels that trigger a page
  fault, a double fault and an unexpected interrupt on purpose.
- `make check-workbench DISK="path/to/Workbench.dms"` boots a real
  Workbench 3.1 boot disk and verifies its volume, root directory and
  startup sequence. `DISK=` is required and the markers fit the 3.1
  Workbench disk only.
- `make test` builds and runs the host unit tests under the address and
  undefined behaviour sanitizers.
- `make test-dms DMS_DIR=<directory>` unpacks every DMS archive below the
  directory with both `xdms` and the project's own unpacker and compares the
  disk images and what `unadf` reads from them.

## Status

Early development. The machine boots as follows:

1. The UEFI loader, `BOOTX64.EFI`, loads `KERNEL.ELF` from the boot disk.
2. It builds page tables, with all memory mapped in the higher half, and
   passes the kernel a `BootInfo` structure holding the memory map, the
   framebuffer and the ACPI RSDP.
3. The kernel reports on serial port COM1 and installs its own GDT, TSS and
   IDT. A CPU exception prints a register dump on the serial port. The kernel
   then drops the loader's identity mapping, sets up per-CPU data for the boot
   processor and draws the `Hombre` banner on the framebuffer.
4. The kernel scans the PCI bus and drives a virtio-blk disk by polling. It
   unpacks a DMS archive natively (no compression, SIMPLE and HEAVY2),
   mounts the OFS or FFS volume read-only and prints the volume name, the
   root directory and `S:Startup-Sequence` on the serial port. Without a
   disk, or with a disk it cannot read, it logs why and carries on.
5. The kernel halts.

## The name

Hombre was the codename of the next-generation Amiga chipset Commodore was
developing when it went bankrupt in 1994. The chipset was never released;
this operating system borrows its name.
