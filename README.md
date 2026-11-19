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
- `binutils-m68k-linux-gnu` (assembles the 68000 programs of the test disk,
  for `make run`, `make check` and `make test`)
- GNU make
- mtools and sgdisk (build the boot disk image)
- QEMU with KVM, and OVMF installed at `/usr/share/OVMF`
- `socat`, only for the `make check` boot that types on the USB keyboard
- `xdms` and `unadf`, only for `make test-dms`

Targets:

- `make` builds the loader, the kernel and the bootable disk image
  `build/hombre.img`.
- `make run` boots the image in QEMU with KVM. Kernel messages appear on the
  terminal, which is wired to the serial port COM1. The kernel gets a
  virtio-blk disk: a generated test disk by default, or the file given as
  `DISK=`, for example `make run DISK="path/to/disk.dms"`. The file may be
  a DMS archive or a raw ADF image, and its path may contain spaces.
  The QEMU window shows a Hombre Workbench screen with a HombreShell window
  that holds what the disk's `C:Version` prints and a `1.<volume>:>` prompt;
  the kernel's own messages stay on the serial port. The QEMU window's keyboard
  is a USB keyboard on an xHCI controller (Swedish layout): type at the
  prompt, and Enter runs `C:<word>` from the disk with the rest of the line
  as its arguments (`version` runs `C:Version`; an unknown word prints
  `Unknown command <word>`). Input needs the controller and keyboard; without
  them the window says why and the boot ends. The serial port logs `usb:` and
  `shell:` lines, with each submitted line as `shell: line: <text>`.
  Up and Down at the prompt bring back the last 16 lines entered; going
  down past the newest returns to the line being typed.
  The shell has the Workbench 3.1 builtins `Cd`, `Echo`, `Path`, `Why`,
  `Prompt`, `Ask`, `Failat`, `Set`, `Get`, `UnSet`, `SetEnv`, `GetEnv` and
  `UnSetEnv`; its prompt shows the current directory, and a
  command whose return code reaches the fail level prints
  `<word> failed returncode <n>`. Commands are searched in `C:` and the
  `Path` directories. On a Workbench 3.1 disk the read-only commands work:
  `Dir`, `List`, `Type`, `Info`, `Assign`, `Date` (from the PC's real-time
  clock), `Which`, `Avail`, `Status` and `Version`. The boot disk stays
  read-only, but `RAM:` (the volume "Ram Disk") can be written: `Copy`,
  `MakeDir`, `Rename`, `Delete`, `Protect`, `Filenote` and `SetDate` work
  there. Its contents live in memory and are lost when the machine
  restarts. At start `T:` and `ENV:` are assigned to `RAM:T` and `RAM:Env`,
  and `ENVARC:` is copied into `ENV:`, as the Workbench Startup-Sequence
  does; global variables are files in `ENV:`. There is no `Run` yet.
  `tools/probe_commands.sh <disk>
  "<line>"...` boots once per line and shows how each command ends, and
  `make KCXXEXTRA=-DHOMBRE_TRACE_NATIVES` logs every library call.
  A USB mouse on the same controller moves an Amiga pointer on the screen,
  and Amiga programs get mouse events in windows that ask for them; Ctrl+C
  stops a program waiting for events. On the test disk, `mousetest` opens a
  window and prints the mouse and window events it receives.
  Windows can be dragged by their title bar and resized live by the size
  gadget in the bottom-right corner. The HombreShell window rewraps its text
  when resized; it opens full screen, so shrink it first to move it.
  The HombreShell window keeps up to 1024 lines of scrollback and has a
  proportional scroll bar in its right border. Drag the knob, roll the mouse
  wheel over the window, or press Shift+Up/Down, Page Up/Page Down or
  Shift+Home/End to scroll through history; typing returns to the bottom, and
  scrolling the mouse wheel over another window sends NewMouse wheel events to
  its IDCMP port.
- `make check` boots the image headless and verifies the messages on the
  serial port. The main boot attaches the generated test disk and verifies
  the volume, the root directory and the startup sequence read from it, and
  what its `C:Version` prints when the kernel runs it. It also boots a
  drive filled with zero bytes, a test disk with a corrupt track and a
  machine without a disk, a boot with the kernel missing and a boot without
  a framebuffer. It also boots kernels that trigger a page
  fault, a double fault and an unexpected interrupt on purpose.
- `make check-workbench DISK="path/to/Workbench.dms"` boots a real
  Workbench 3.1 boot disk and verifies its volume, root directory and
  startup sequence, and that the disk's own `C:Version` runs and returns 0.
  Two more boots type commands at the shell: the read-only commands and
  builtins, then a session that makes, copies, renames, protects, lists
  and deletes files on `RAM:` and sets an environment variable.
  `DISK=` is required and the markers fit the 3.1 Workbench disk only.
- `make test` builds and runs the host unit tests under the address and
  undefined behaviour sanitizers. It builds the test disk as well, so it
  needs `binutils-m68k-linux-gnu` too.
- `make test-m68k M68K_TESTS_DIR=<directory>` runs the 68000 cases of the
  SingleStepTests/680x0 suite through the 68000 translator and compares the
  registers and memory each case leaves. The suite is not part of the
  repository: check it out elsewhere and give its directory.
  `M68K_FILES=<glob>` picks some of its files, for example `M68K_FILES=ADD*`.
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
5. The kernel runs `C:Version` from the boot disk as translated 68000 code.
   A hunk loader loads it, a 68000 JIT translates it block by block into
   x86-64 code in a write-xor-execute code region, and native exec,
   utility and dos libraries, written in C++ under `common/libs/`, answer
   its library calls. A library the program opens from the disk's `LIBS:`
   loads and runs as 68000 code too. The program's output and its return
   code appear on the serial port.
6. The kernel halts.

## The name

Hombre was the codename of the next-generation Amiga chipset Commodore was
developing when it went bankrupt in 1994. The chipset was never released;
this operating system borrows its name.
