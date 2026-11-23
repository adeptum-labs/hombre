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

# A failed recipe must not leave a half-written target behind.
.DELETE_ON_ERROR:

BUILD        := build
IMAGE        := $(BUILD)/hombre.img
IMAGE_NOKERN := $(BUILD)/hombre-nokernel.img
OVMF         := /usr/share/OVMF

# The drives make run attaches as virtio-blk disks: DISK is DF0:, the boot
# disk, and the optional DISK1, DISK2 and DISK3 are DF1: to DF3:; a drive
# without one is empty. Each may be any path, spaces and & included; DISK
# defaults to the generated test disk. They reach the recipes through the
# environment, so no character in them is shell syntax. Drive n sits in PCI
# slot 0x10 + n, which is how the kernel tells the drives apart.
TEST_DISK := $(BUILD)/test-disk.dms
PIC_DISK  := $(BUILD)/pic-disk.adf
# A hard disk image whose one partition, DH0:, holds the test disk's volume.
TEST_HDF  := $(BUILD)/test.hdf
ADFTOOL   := $(BUILD)/tools/adftool
DISK      ?= $(TEST_DISK)
export DISK DISK1 DISK2 DISK3
DISK_IMG  := $(BUILD)/disk.img

# Hard disk images make run attaches after the floppies: HDF and the
# optional HDF1 to HDF3, each an RDB-partitioned image or a bare DOS volume,
# whose partitions become DH0: and on. Hard disk n sits in PCI slot 0x18 + n.
# They are attached as they are and written to; HDF_SNAPSHOT=1 keeps every
# write in QEMU instead. HDF_BUS=ahci puts them on the q35 machine's SATA
# controller and HDF_BUS=nvme on NVMe controllers instead of virtio-blk;
# HDF_SECTOR=4096 gives the NVMe disks 4096-byte sectors. With HDF given and
# DISK not, DF0: stays empty and Hombre boots from the hard disk.
export HDF HDF1 HDF2 HDF3 HDF_SNAPSHOT HDF_BUS HDF_SECTOR
ifneq ($(HDF),)
ifeq ($(origin DISK),file)
NO_DF0 := 1
endif
endif

# Drives for the negative boots: 1000 zero bytes, and the test disk with one
# byte changed inside the packed data of its first track.
ZERO_DISK    := $(BUILD)/zero-disk.bin
CORRUPT_DISK := $(BUILD)/corrupt-disk.dms

# The loader is a COFF object built by clang for the Windows target and
# linked into a PE32+ image by GNU ld, which is what UEFI firmware loads.
CXX      := clang++
LD       := ld.bfd
CXXFLAGS := -target x86_64-windows-gnu -ffreestanding -fno-exceptions -fno-rtti \
            -fshort-wchar -mno-red-zone -fno-stack-protector -std=c++20 \
            -Wall -Wextra -Werror -MMD -MP
LDFLAGS  := -m i386pep --subsystem 10 -e efi_main

LOADER_SRC := $(wildcard loader/*.cpp)
LOADER_OBJ := $(LOADER_SRC:%.cpp=$(BUILD)/%.o) $(BUILD)/loader/mem.o
LOADER     := $(BUILD)/BOOTX64.EFI

# The kernel is an ELF64 executable built by g++ and linked at its physical
# load address mapped into the higher half (see kernel/linker.ld).
#
# KCXXEXTRA adds flags to the kernel build, its copy of common/ included,
# and is empty by default. The objects do not depend on it, so change it
# only after make clean. make clean && make KCXXEXTRA=-DHOMBRE_M68K_TRACE
# builds a kernel that logs every translated 68000 block, its guest pc and
# first four opcode words, on the serial console.
KCXXEXTRA ?=
KCXX      := g++
KLD       := ld.bfd
KCXXFLAGS := -std=c++23 -O2 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector \
             -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx -nostdlib \
             -Wall -Wextra -Werror -MMD -MP $(KCXXEXTRA)
KLDFLAGS  := -nostdlib -T kernel/linker.ld -z max-page-size=0x1000
# The mem* routines are built with these: they keep the compiler from turning
# their loops back into calls to the very functions being defined.
KMEMFLAGS := -fno-builtin -fno-tree-loop-distribute-patterns

# The shared sources in common/ and the native AmigaOS libraries in
# common/libs/, other than the mem* routines, are built once for the kernel,
# into $(BUILD)/common/, and once for the host, into $(BUILD)/host/common/,
# where every test program and host tool links them.
COMMON_SRC  := $(filter-out common/mem.cpp,$(wildcard common/*.cpp common/libs/*.cpp))
COMMON_KOBJ := $(COMMON_SRC:%.cpp=$(BUILD)/%.o)
COMMON_HOBJ := $(COMMON_SRC:%.cpp=$(BUILD)/host/%.o)

KERNEL_SRC := $(wildcard kernel/*.cpp)
KERNEL_OBJ := $(KERNEL_SRC:%.cpp=$(BUILD)/%.o) $(BUILD)/kernel/mem.o $(COMMON_KOBJ)
KERNEL     := $(BUILD)/KERNEL.ELF

# Kernels that provoke a CPU fault on purpose, one per value of
# HOMBRE_FAULT_TEST. Each is built into its own directory.
FAULT_TESTS := 1 2 3 4

.PHONY: all image image-nokernel disk-image run check check-workbench check-wbpattern test \
        test-dms \
        test-m68k clean

all: image

image: $(IMAGE)

image-nokernel: $(IMAGE_NOKERN)

$(filter-out %/mem.o,$(LOADER_OBJ)): $(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(filter-out %/mem.o,$(KERNEL_OBJ)): $(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(KCXX) $(KCXXFLAGS) -c $< -o $@

# The splash image is read with .incbin, a dependency make cannot see.
$(BUILD)/kernel/splash.o: assets/splash.rle

# So are the ROM fonts, by the kernel and by the host programs.
$(BUILD)/common/libs/rom_fonts.o $(BUILD)/host/common/libs/rom_fonts.o: \
    assets/fonts/hasklig/hasklig.romfont

# The mem* routines are built once per target, without builtins (see
# KMEMFLAGS for the kernel).
$(BUILD)/loader/mem.o: common/mem.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fno-builtin -c $< -o $@

$(BUILD)/kernel/mem.o: common/mem.cpp
	@mkdir -p $(@D)
	$(KCXX) $(KCXXFLAGS) $(KMEMFLAGS) -c $< -o $@

$(LOADER): $(LOADER_OBJ)
	$(LD) $(LDFLAGS) $^ -o $@

$(KERNEL): $(KERNEL_OBJ) kernel/linker.ld
	$(KLD) $(KLDFLAGS) $(KERNEL_OBJ) -o $@

$(IMAGE): $(LOADER) $(KERNEL) tools/make_image.sh
	tools/make_image.sh $@ $(LOADER) $(KERNEL)

# Same disk without KERNEL.ELF, to exercise the loader's missing-file path.
$(IMAGE_NOKERN): $(LOADER) tools/make_image.sh
	tools/make_image.sh $@ $(LOADER)

# One fault-test image from the normal loader and a kernel built with
# -DHOMBRE_FAULT_TEST=<n> into build/fault<n>/. Called with the value n.
define FAULT_VARIANT
FAULT$(1)_DIR := $(BUILD)/fault$(1)
FAULT$(1)_OBJ := $(KERNEL_SRC:%.cpp=$$(FAULT$(1)_DIR)/%.o) $$(FAULT$(1)_DIR)/kernel/mem.o \
                 $$(COMMON_KOBJ)

.PHONY: image-fault$(1)
image-fault$(1): $(BUILD)/hombre-fault$(1).img

$$(filter-out %/mem.o $$(COMMON_KOBJ),$$(FAULT$(1)_OBJ)): $$(FAULT$(1)_DIR)/%.o: %.cpp
	@mkdir -p $$(@D)
	$$(KCXX) $$(KCXXFLAGS) -DHOMBRE_FAULT_TEST=$(1) -c $$< -o $$@

$$(FAULT$(1)_DIR)/kernel/splash.o: assets/splash.rle

$$(FAULT$(1)_DIR)/kernel/mem.o: common/mem.cpp
	@mkdir -p $$(@D)
	$$(KCXX) $$(KCXXFLAGS) $$(KMEMFLAGS) -c $$< -o $$@

$$(FAULT$(1)_DIR)/KERNEL.ELF: $$(FAULT$(1)_OBJ) kernel/linker.ld
	$$(KLD) $$(KLDFLAGS) $$(FAULT$(1)_OBJ) -o $$@

$(BUILD)/hombre-fault$(1).img: $$(LOADER) $$(FAULT$(1)_DIR)/KERNEL.ELF tools/make_image.sh
	tools/make_image.sh $$@ $$(LOADER) $$(FAULT$(1)_DIR)/KERNEL.ELF

-include $$(FAULT$(1)_OBJ:.o=.d)
endef

$(foreach n,$(FAULT_TESTS),$(eval $(call FAULT_VARIANT,$(n))))

# -pdpe1gb: with 1 GiB pages exposed, KVM on this host fails with an internal
# error while the firmware is still in 32-bit mode.
# The boot image opens as a snapshot and the firmware variables are run's
# own, so a boot check can run beside it.
# AUDIODEV is QEMU's sound backend for the HD Audio controller: pa
# (PulseAudio and PipeWire), alsa, sdl, or none for silence.
AUDIODEV ?= pa

run: image disk-image
	cp $(OVMF)/OVMF_VARS_4M.fd $(BUILD)/OVMF_VARS-run.fd
	extra=; \
	if [ -z "$(NO_DF0)" ]; then \
	    extra="-drive if=none,id=disk0,format=raw,readonly=on,file=$(DISK_IMG)"; \
	    extra="$$extra -device virtio-blk-pci,disable-legacy=on,drive=disk0,addr=0x10"; \
	fi; \
	for n in 1 2 3; do \
	    if [ -f $(BUILD)/disk$$n.img ]; then \
	        extra="$$extra -drive if=none,id=disk$$n,format=raw,readonly=on,file=$(BUILD)/disk$$n.img"; \
	        extra="$$extra -device virtio-blk-pci,disable-legacy=on,drive=disk$$n,addr=0x1$$n"; \
	    fi; \
	done; \
	snap=; if [ -n "$$HDF_SNAPSHOT" ]; then snap=",snapshot=on"; fi; \
	set --; n=0; \
	for f in "$$HDF" "$$HDF1" "$$HDF2" "$$HDF3"; do \
	    if [ -n "$$f" ]; then \
	        [ -f "$$f" ] || { echo "missing hard disk image $$f" >&2; exit 1; }; \
	        set -- "$$@" -drive "if=none,id=hd$$n,format=raw$$snap,file=$$(echo "$$f" | sed 's/,/,,/g')"; \
	        case "$$HDF_BUS" in \
	        ahci) set -- "$$@" -device ide-hd,drive=hd$$n,bus=ide.$$((n + 1)) ;; \
	        nvme) set -- "$$@" -device nvme,id=nvme$$n,serial=hombre$$n \
	                  -device nvme-ns,drive=hd$$n,bus=nvme$$n,logical_block_size=$${HDF_SECTOR:-512},physical_block_size=$${HDF_SECTOR:-512} ;; \
	        *) set -- "$$@" -device virtio-blk-pci,disable-legacy=on,drive=hd$$n,addr=0x1$$((8 + n)) ;; \
	        esac; \
	    fi; \
	    n=$$((n + 1)); \
	done; \
	qemu-system-x86_64 -enable-kvm -cpu host,-pdpe1gb -machine q35 -m 1G -no-reboot \
	    -serial stdio \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF)/OVMF_CODE_4M.fd \
	    -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS-run.fd \
	    -drive format=raw,snapshot=on,file=$(IMAGE) \
	    $$extra "$$@" \
	    -device qemu-xhci -device usb-kbd -device usb-mouse \
	    -audiodev $(AUDIODEV),id=snd0 -device ich9-intel-hda -device hda-output,audiodev=snd0

# The boots attach the generated test disk whatever DISK says, so they stay
# hermetic. The main boot expects its size in 512-byte sectors, rounded up.
check: image image-nokernel $(FAULT_TESTS:%=image-fault%) $(TEST_DISK) $(TEST_HDF) $(ADFTOOL) $(ZERO_DISK) $(CORRUPT_DISK)
	sectors=$$(( ($$(stat -c %s $(TEST_DISK)) + 511) / 512 )) && \
	tools/boot_check.sh --disk $(TEST_DISK) "Hombre loader" \
	    "kernel loaded: entry=0xffffffff8010" "Hombre kernel booted" "usable memory:" \
	    "gdt loaded" "idt loaded" "breakpoint ok" "lower half unmapped" "cpu0 percpu ok" \
	    "apic: timer 1000 Hz" \
	    "framebuffer: " "pci: " "virtio-blk: $$sectors sectors" "virtio-blk: read ok" \
	    "disk: DMS SIMPLE, 80 tracks" "volume: HombreTest (OFS)" "  S/" "  C/" "  Hello" \
	    "S:Startup-Sequence:" "  Echo \"Hombre test startup\"" "usb: no controller" \
	    "intuition: screen opened 640x400" \
	    "intuition: window opened HombreShell" "startup: running S:Startup-Sequence" \
	    "m68k: running C:Version" "Hombre test version 1" "m68k: C:Version returned 0" \
	    "startup: ended, returned 0" "halting"
	sectors=$$(( ($$(stat -c %s $(TEST_DISK)) + 511) / 512 )) && \
	tools/boot_check.sh --disk $(TEST_DISK) --disk1 $(TEST_DISK) \
	    "virtio-blk: read ok" "volume: HombreTest (OFS)" \
	    "drive 1: virtio-blk: $$sectors sectors" "drive 1: virtio-blk: read ok" \
	    "drive 1: disk: DMS SIMPLE, 80 tracks" "drive 1: volume: HombreTest (OFS)" \
	    "drive 2: no disk" "drive 3: no disk" "m68k: C:Version returned 0"
	cp $(TEST_HDF) $(BUILD)/check.hdf
	tools/boot_check.sh --disk $(TEST_DISK) --hdf $(BUILD)/check.hdf \
	    "hd0: virtio 16448 sectors" "hd0: table none, 1 partitions" \
	    "hd0: DH0: HombreTest (OFS), 16384 blocks, bootpri 0, bootable" \
	    "m68k: C:Version returned 0"
	tools/boot_check.sh --no-disk --hdf $(BUILD)/check.hdf \
	    --keys "e c h o spc h e j spc shift-less d h 0 shift-dot n o t e ret w h y ret" \
	    "hd0: DH0: HombreTest (OFS), 16384 blocks, bootpri 0, bootable" \
	    "startup: running S:Startup-Sequence" "m68k: C:Version returned 0" \
	    "startup: ended, returned 0" "shell: builtin echo returned 0" \
	    "shell: builtin why returned 0"
	test "$$($(ADFTOOL) cat $(BUILD)/check.hdf note)" = "hej"
	tools/boot_check.sh --no-disk --hdf $(BUILD)/check.hdf \
	    --keys "e c h o spc d a spc shift-less shift-less d h 0 shift-dot n o t e ret w h y ret" \
	    "shell: builtin echo returned 0" "shell: builtin why returned 0"
	test "$$($(ADFTOOL) cat $(BUILD)/check.hdf note)" = "$$(printf 'hej\nda')"
	cp $(TEST_HDF) $(BUILD)/check.hdf
	tools/boot_check.sh --no-disk --hdf $(BUILD)/check.hdf --hdf-bus ahci \
	    --keys "e c h o spc h e j spc shift-less d h 0 shift-dot n o t e ret w h y ret" \
	    "ahci: port 1: QEMU HARDDISK, 16448 sectors, 512-byte sectors" \
	    "hd1: DH0: HombreTest (OFS), 16384 blocks, bootpri 0, bootable" \
	    "startup: ended, returned 0" "shell: builtin why returned 0"
	test "$$($(ADFTOOL) cat $(BUILD)/check.hdf note)" = "hej"
	tools/boot_check.sh --disk $(TEST_DISK) \
	    --keys "v e r s i o n ret z z z ret bracket_left apostrophe semicolon ret" \
	    "usb: keyboard ready" "shell: ready" "shell: line: version" \
	    "m68k: C:version returned 0" "shell: line: zzz" "shell: unknown command zzz" \
	    "shell: line: \\xe5\\xe4\\xf6"
	tools/boot_check.sh --disk $(TEST_DISK) \
	    --keys "c d spc c ret c d ret c d spc shift-7 ret e c h o spc h e j ret p a t h ret w h y ret up up up spc a ret up down down spc y ret e c x o spc h i left left left left backspace h ret" \
	    "shell: builtin cd returned 0" "HombreTest:C" "hej" \
	    "shell: builtin echo returned 0" "Current_directory" \
	    "shell: builtin path returned 0" "The last command did not set a return code" \
	    "shell: builtin why returned 0" "shell: line: echo hej a" "shell: line:  y" \
	    "shell: line: echo hi"
	tools/boot_check.sh --disk $(TEST_DISK) --keys "m o u s e t e s t ret" \
	    --mouse-after "mousetest: ready" \
	    --mouse "mouse_move 10 10;mouse_button 1;mouse_button 0;sendkey ctrl-c" \
	    "usb: mouse ready" "mousetest: ready" "mousetest: active" \
	    "mousetest: move at 130,110" "mousetest: select down at 130,110" \
	    "mousetest: select up at 130,110" "mousetest: break" \
	    "mouse: down 1 at 330,210" "mouse: up 1 at 330,210" \
	    "m68k: C:mousetest returned 0"
	tools/boot_check.sh --disk $(TEST_DISK) --keys "m o u s e t e s t ret" \
	    --mouse-after "mousetest: ready" \
	    --mouse "mouse_move -100 -95;mouse_button 1;mouse_move 50 20;mouse_button 0;mouse_move 100 75;mouse_move 100 75;mouse_move 15 0;mouse_button 1;mouse_move -40 -30;mouse_button 0;sendkey ctrl-c" \
	    "mousetest: change at 250,120 size 240x160" "mousetest: newsize 200x130" \
	    "mousetest: change at 250,120 size 200x130" "mousetest: break" \
	    "m68k: C:mousetest returned 0"
	tools/boot_check.sh --disk $(TEST_DISK) --keys "t a s k t e s t ret t a s k t e s t spc 2 ret" \
	    "tasktest: signalled" "tasktest: preempted" "tasktest: blocked wait ok" \
	    "tasktest: run2" "tasktest: done" "m68k: C:tasktest returned 0"
	tools/boot_check.sh --disk $(TEST_DISK) --keys "t i m e r t e s t ret" \
	    "timertest: timer ok" "timertest: abort ok" "timertest: delay ok" \
	    "m68k: C:timertest returned 0"
	tools/boot_check.sh --disk $(TEST_DISK) --keys "r u n spc t i m e r t e s t ret v e r s i o n ret" \
	    "[CLI 2]" "m68k: C:version returned 0" "timertest: delay ok" \
	    "dos: CLI 2 ended, returned 0"
	bash tools/log_order.sh build/serial.log "shell: builtin run returned 0" \
	    "timertest: slept" "dos: CLI 2 ended, returned 0"
	tools/boot_check.sh --disk $(TEST_DISK) --keys "l o a d w b ret" \
	    "m68k: running C:loadwb" "m68k: C:loadwb returned 0" \
	    "workbench: started" "workbench: volume HombreTest" \
	    "workbench: started wbtest" "dos: process wbtest ended, returned 0" \
	    "workbench: wbtest ended"
	bash tools/log_order.sh build/serial.log "workbench: started wbtest" \
	    "dos: process wbtest ended, returned 0" "workbench: wbtest ended"
	tools/boot_check.sh --disk $(TEST_DISK) \
	    --keys "n e w s h e l l ret v e r s i o n ret" \
	    "shell: new shell 2" "shell: builtin newshell returned 0" \
	    "m68k: C:version returned 0"
	tools/boot_check.sh --disk $(TEST_DISK) --keys "t a s k t e s t spc f a u l t ret" \
	    --mouse-after "exec: task tasktest.fault ended with Illegal" \
	    --mouse "sendkey ctrl-c;sendkey v;sendkey e;sendkey r;sendkey s;sendkey i;sendkey o;sendkey n;sendkey ret" \
	    "exec: task tasktest.fault ended with Illegal" \
	    "exec: Wait interrupted by Ctrl+C" \
	    "m68k: Break at pc=" \
	    "m68k: C:version returned 0"
# Scrollback boot check: the full-screen shell shows 46 rows, so PageUp
# steps 45 rows. Enough empty lines pass one page; Shift+Up, PageUp and one
# wheel notch up (QEMU's mouse_move dz 1) scroll back, and a typed key
# returns to the bottom.
	tools/boot_check.sh --disk $(TEST_DISK) \
	    --keys "$(shell awk 'BEGIN { for (i = 0; i < 100; i++) printf "ret " }')shift-up pgup" \
	    --mouse-after "shell: view 46" \
	    --mouse "mouse_move 0 0 1;sendkey x" \
	    "shell: view 1" "shell: view 46" "shell: view 49" "shell: view 0"
	tools/boot_check.sh --disk $(ZERO_DISK) "virtio-blk: 2 sectors" "disk: unknown format" \
	    "halting"
	tools/boot_check.sh --disk $(CORRUPT_DISK) "disk: dms error: TrackCrc track 0" \
	    "halting"
	tools/boot_check.sh --no-disk "Hombre kernel booted" "framebuffer: " "pci: " \
	    "no boot disk" "halting"
	tools/boot_check.sh --no-disk --no-kernel "KERNEL.ELF not found"
	tools/boot_check.sh --no-disk --no-gop "no framebuffer" "halting"
	tools/boot_check.sh --no-disk --fault-test 1 "fault test: 1" \
	    "*** exception 14 #PF (page fault)" "error: 0x0" "cs: 0x8 rflags: 0x" "ss: 0x10" \
	    "cr2: 0x1000 cr3" "halting"
	tools/boot_check.sh --no-disk --fault-test 2 "fault test: 2" \
	    "*** exception 8 #DF (double fault)" "cs: 0x8 rflags: 0x" "rsp: 0x1000 ss: 0x10" \
	    "halting"
	tools/boot_check.sh --no-disk --fault-test 3 "fault test: 3" \
	    "*** unexpected interrupt 128" "cs: 0x8 rflags: 0x" "halting"
	tools/boot_check.sh --no-disk --fault-test 4 "fault test: 4" \
	    "fault test: guard page 0xffffc10000000000" \
	    "*** exception 8 #DF (double fault)" "rsp: 0xffffc10000001000 ss: 0x10" \
	    "cr2: 0xffffc10000000ff8" \
	    "halting"

# Boots a real Workbench 3.1 disk, given as DISK=<path to a .dms or .adf> on
# the command line or in the environment. The default test disk does not
# count. The markers are those of the Workbench disk and are pinned to it.
ifneq ($(and $(filter command line environment override,$(origin DISK)),$(strip $(DISK))),)
check-workbench: image
	tools/boot_check.sh --disk "$$DISK" "Hombre kernel booted" "pci: " \
	    "virtio-blk: read ok" "disk: DMS HEAVY2, 80 tracks" \
	    "volume: Workbench3.1 (FFS)" "  S/" "  C/" "S:Startup-Sequence:" \
	    "; Startup-Sequence for low-density floppies" "usb: no controller" \
	    "intuition: screen opened 640x400" \
	    "intuition: window opened HombreShell" "startup: running S:Startup-Sequence" \
	    "m68k: running C:Version" "m68k: C:Version returned 0" "m68k: C:Assign returned 0" \
	    "m68k: C:AddDataTypes returned 0" "m68k: C:IPrefs returned 0" "workbench: started" \
	    "intuition: window Workbench opened" "shell: builtin EndCLI returned 0" \
	    "startup: ended, returned 0" "shell: boot shell ended" "halting"
	bash tools/log_order.sh build/serial.log "startup: running S:Startup-Sequence" \
	    "workbench: started" "shell: boot shell ended"
	tools/boot_check.sh --no-startup --disk "$$DISK" \
	    --keys "d i r ret l i s t spc c ret t y p e spc s shift-dot s t a r t u p slash s e q u e n c e ret i n f o ret a s s i g n ret d a t e ret w h i c h spc d i r ret c d spc c ret c d spc shift-7 ret a s k spc q ret y ret w h y ret r u n spc w a i t spc 3 ret e c h o spc h i ret" \
	    "m68k: C:dir returned 0" "m68k: C:list returned 0" "Directory \"C\" on" \
	    "m68k: C:type returned 0" "Workbench3.1 [Mounted]" "m68k: C:info returned 0" \
	    "LIBS           Workbench3.1:Libs" "m68k: C:assign returned 0" \
	    "m68k: C:date returned 0" "m68k: C:which returned 0" \
	    "shell: builtin cd returned 0" "shell: builtin ask returned 5" \
	    "shell: builtin why returned 0" "[CLI 2]" \
	    "shell: builtin echo returned 0" "dos: CLI 2 ended, returned 0"
	bash tools/log_order.sh build/serial.log "shell: builtin echo returned 0" \
	    "dos: CLI 2 ended, returned 0"
	tools/boot_check.sh --no-startup --disk "$$DISK" \
	    --keys "n e w s h e l l ret s t a t u s ret r u n spc w a i t spc 6 0 ret b r e a k spc 3 ret s t a t u s ret" \
	    "shell: new shell 2" "Process  2:" "[CLI 3]" \
	    "m68k: C:break returned 0" "dos: CLI 3 ended"
	bash tools/log_order.sh build/serial.log "m68k: C:break returned 0" \
	    "dos: CLI 3 ended"
	tools/boot_check.sh --no-startup --disk "$$DISK" \
	    --keys "r u n spc u t i l i t i e s shift-7 c l o c k ret e c h o spc h i ret" \
	    "[CLI 2]" "intuition: window Clock opened" "shell: builtin echo returned 0"
	tools/boot_check.sh --no-startup --disk "$$DISK" --keys "l o a d w b ret" \
	    "m68k: running C:loadwb" "workbench: started" \
	    "workbench: volume Workbench3.1" "workbench: volume Ram Disk" \
	    "intuition: window Workbench opened"
	tools/boot_check.sh --no-startup --disk "$$DISK" --keys "l o a d w b ret" \
	    --mouse-after "workbench: volume Ram Disk" \
	    --mouse "mouse_move 95 -80;mouse_move 95 -80;mouse_move 70 0;mouse_button 1;mouse_button 0;mouse_button 1;mouse_button 0;mouse_move -75 12;mouse_move -100 0;mouse_move -100 0;mouse_move -100 0;mouse_move -55 0;mouse_button 1;mouse_button 0;mouse_button 1;mouse_button 0;mouse_move 125 40;mouse_button 1;mouse_button 0;mouse_button 1;mouse_button 0" \
	    "workbench: opened Workbench3.1" "workbench: opened Utilities" \
	    "workbench: started Clock" "intuition: window Clock opened"
	tools/boot_check.sh --no-startup --disk "$$DISK" \
	    --keys "m a k e d i r spc r a m shift-dot x ret c o p y spc s shift-dot s t a r t u p slash s e q u e n c e spc r a m shift-dot x ret t y p e spc r a m shift-dot x shift-7 s t a r t u p slash s e q u e n c e ret r e n a m e spc r a m shift-dot x spc r a m shift-dot y ret p r o t e c t spc r a m shift-dot y shift-7 s t a r t u p slash s e q u e n c e spc s spc a d d ret f i l e n o t e spc r a m shift-dot y shift-7 s t a r t u p slash s e q u e n c e spc h e l l o ret l i s t spc r a m shift-dot y ret d e l e t e spc r a m shift-dot y spc a l l ret d i r spc r a m shift-dot ret s e t e n v spc f o o spc b a r ret g e t e n v spc f o o ret t y p e spc e n v shift-dot f o o ret i n f o ret w a i t spc 1 ret" \
	    "m68k: C:makedir returned 0" "m68k: C:copy returned 0" \
	    "; Startup-Sequence for low-density floppies" "m68k: C:rename returned 0" \
	    "m68k: C:protect returned 0" "m68k: C:filenote returned 0" \
	    "Directory \"ram:y\" on" ": hello" "m68k: C:list returned 0" \
	    "ram:y  Deleted" "m68k: C:delete returned 0" "     T (dir)" \
	    "shell: builtin setenv returned 0" "shell: builtin getenv returned 0" \
	    "Read/Write Ram Disk" "m68k: C:info returned 0" "m68k: C:wait returned 0"
else
check-workbench:
	@echo "set DISK=<path to a Workbench .dms or .adf>" >&2; exit 1
endif

# Runs WBPattern from the 3.1 Extras disk in DF1:, given as DISK1 beside the
# Workbench disk in DISK: a preset and Use write its prefs, and the
# Workbench that LoadWB starts after it reads them for its backdrop.
# Both boots skip the Startup-Sequence and run AddDataTypes, which
# WBPattern needs, by hand. The second picks a picture through the file
# requester: Type to
# Picture, Select Picture, "Pics:" typed into the Drawer field, a double
# click into the Pics drawer, the scroller dragged to its end, Zz.ilbm (the
# last file, shown only scrolled) clicked, OK and Use. Each wait is a
# second of no-op moves.
WBP_CLICK := mouse_button 1;mouse_button 0
WBP_WAIT  := mouse_move 0 0;mouse_move 0 0;mouse_move 0 0;mouse_move 0 0
WBPATTERN_PICTURE_MOUSE := mouse_move -90 -139;$(WBP_CLICK);mouse_move 0 22;$(WBP_CLICK);\
    $(WBP_WAIT);$(WBP_WAIT);mouse_move 0 61;$(WBP_CLICK);\
    sendkey shift-p;sendkey i;sendkey c;sendkey s;sendkey shift-dot;sendkey ret;\
    $(WBP_WAIT);$(WBP_WAIT);mouse_move -160 -102;$(WBP_CLICK);$(WBP_CLICK);\
    $(WBP_WAIT);$(WBP_WAIT);$(WBP_WAIT);$(WBP_WAIT);\
    mouse_move 262 13;mouse_button 1;mouse_move 0 50;mouse_move 0 50;mouse_button 0;\
    $(WBP_WAIT);mouse_move -262 -32;$(WBP_CLICK);$(WBP_WAIT);\
    mouse_move 0 56;$(WBP_CLICK);$(WBP_WAIT);$(WBP_WAIT);\
    mouse_move 248 -36;$(WBP_CLICK)

ifneq ($(and $(strip $(DISK)),$(strip $(DISK1))),)
check-wbpattern: image $(PIC_DISK)
	tools/boot_check.sh --no-startup --disk "$$DISK" --disk1 "$$DISK1" \
	    --keys "a d d d a t a t y p e s spc r e f r e s h spc q u i e t ret e x t r a s 3 dot 1 shift-dot p r e f s shift-7 w b p a t t e r n ret l o a d w b ret" \
	    --mouse-after "intuition: window WBPattern Preferences opened" \
	    --mouse "mouse_move 50 -88;mouse_button 1;mouse_button 0;mouse_move -52 31;mouse_button 1;mouse_button 0" \
	    "intuition: window WBPattern Preferences opened" \
	    "m68k: extras3.1:prefs/wbpattern returned 0" \
	    "workbench: backdrop patterns read"
	bash tools/log_order.sh build/serial.log \
	    "m68k: extras3.1:prefs/wbpattern returned 0" "workbench: backdrop patterns read"
	tools/boot_check.sh --no-startup --disk "$$DISK" --disk1 "$$DISK1" --disk2 $(PIC_DISK) \
	    --keys "a d d d a t a t y p e s spc r e f r e s h spc q u i e t ret e x t r a s 3 dot 1 shift-dot p r e f s shift-7 w b p a t t e r n ret l o a d w b ret" \
	    --mouse-after "intuition: window WBPattern Preferences opened" \
	    --mouse "$(WBPATTERN_PICTURE_MOUSE)" \
	    "intuition: window Select Picture opened" \
	    "m68k: extras3.1:prefs/wbpattern returned 0" \
	    "workbench: backdrop picture Pics:Pics/Zz.ilbm"
	bash tools/log_order.sh build/serial.log \
	    "m68k: extras3.1:prefs/wbpattern returned 0" \
	    "workbench: backdrop picture Pics:Pics/Zz.ilbm"
else
check-wbpattern:
	@echo "set DISK=<Workbench disk> and DISK1=<Extras disk>" >&2; exit 1
endif

# Host unit tests: every tests/test_*.cpp is its own program, built with the
# host compiler under the sanitizers and run in turn.
HOSTCXX      := g++
HOSTCXXFLAGS := -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
                -fno-sanitize-recover=undefined -Icommon -Ikernel -I. -Itools -MMD -MP
TEST_SRC     := $(wildcard tests/test_*.cpp)
TEST_BIN     := $(TEST_SRC:tests/%.cpp=$(BUILD)/tests/%)

$(BUILD)/host/common/%.o: common/%.cpp
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.cpp $(COMMON_HOBJ)
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< $(COMMON_HOBJ) -o $@

# The test of the test disk needs the disk itself, which it finds through
# the environment.
test: $(TEST_BIN) $(TEST_DISK)
	@for t in $(TEST_BIN); do HOMBRE_TEST_DISK="$(TEST_DISK)" $$t || exit 1; done

# Host tools, built like the tests.

$(ADFTOOL): tools/adftool.cpp $(COMMON_HOBJ)
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< $(COMMON_HOBJ) -o $@

MKHDF := $(BUILD)/tools/mkhdf

$(MKHDF): tools/mkhdf.cpp $(COMMON_HOBJ)
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< $(COMMON_HOBJ) -o $@

$(TEST_HDF): $(MKHDF) $(TEST_DISK)
	$(MKHDF) $@ $(TEST_DISK)

MKDISK    := $(BUILD)/tools/mkdisk

$(MKDISK): tools/mkdisk.cpp $(COMMON_HOBJ)
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< $(COMMON_HOBJ) -o $@

MKPICDISK := $(BUILD)/tools/mkpicdisk

$(MKPICDISK): tools/mkpicdisk.cpp $(COMMON_HOBJ)
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< $(COMMON_HOBJ) -o $@

$(PIC_DISK): $(MKPICDISK)
	$(MKPICDISK) $@

ELF2HUNK := $(BUILD)/tools/elf2hunk

$(ELF2HUNK): tools/elf2hunk.cpp
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< -o $@

# The 68000 programs of the test disk: tools/m68k/<name>.s is assembled and
# linked with tools/m68k/hunk.ld, keeping the relocations, and elf2hunk
# turns the ELF executable into a hunk file.
M68K_AS       := m68k-linux-gnu-as
M68K_LD       := m68k-linux-gnu-ld
M68K_PROGRAMS := version hombretest mousetest tasktest timertest loadwb wbtest
M68K_HUNKS    := $(M68K_PROGRAMS:%=$(BUILD)/m68k/%.hunk)

define require_tool
@command -v $(1) >/dev/null || { \
    echo "make: $(1) not found: install binutils-m68k-linux-gnu" >&2; exit 1; }
endef

$(BUILD)/m68k/%.o: tools/m68k/%.s
	$(call require_tool,$(M68K_AS))
	@mkdir -p $(@D)
	$(M68K_AS) -m68000 $< -o $@

$(BUILD)/m68k/%.elf: $(BUILD)/m68k/%.o tools/m68k/hunk.ld
	$(call require_tool,$(M68K_LD))
	$(M68K_LD) -nostdlib -T tools/m68k/hunk.ld --emit-relocs $< -o $@

$(BUILD)/m68k/%.hunk: $(BUILD)/m68k/%.elf $(ELF2HUNK)
	$(ELF2HUNK) $< $@

# The object and ELF files of the chain are kept, not removed after the
# build as intermediates.
.SECONDARY: $(M68K_PROGRAMS:%=$(BUILD)/m68k/%.o) $(M68K_PROGRAMS:%=$(BUILD)/m68k/%.elf)

# The disk the boot check mounts: an OFS volume packed with SIMPLE tracks.
# It is rebuilt whenever a tool or a program on it changes, and the output
# is reproducible.
$(TEST_DISK): $(MKDISK) $(M68K_HUNKS)
	$(MKDISK) $@ $(M68K_HUNKS)

$(ZERO_DISK):
	@mkdir -p $(@D)
	head -c 1000 /dev/zero > $@

# The octal escape is what every sh printf understands. Byte 100 lies inside
# the first track's packed data, and the test disk has no 0xff there;
# test_testdisk checks both.
$(CORRUPT_DISK): $(TEST_DISK)
	cp $< $@
	printf '\377' | dd of=$@ bs=1 seek=100 conv=notrunc

# disk-image pads DISK to whole 512-byte sectors in DISK_IMG, and DISK1 to
# DISK3, when set, in $(BUILD)/disk1.img to disk3.img. When DISK is the
# default test disk, that is built first.
ifeq ($(origin DISK),file)
disk-image: $(TEST_DISK)
endif

disk-image:
	tools/make_disk.sh "$$DISK" $(DISK_IMG)
	for n in 1 2 3; do \
	    eval "src=\$${DISK$$n:-}"; \
	    rm -f $(BUILD)/disk$$n.img; \
	    if [ -n "$$src" ]; then tools/make_disk.sh "$$src" $(BUILD)/disk$$n.img || exit 1; fi; \
	done

# Unpacks every DMS archive below DMS_DIR with both xdms and adftool and
# compares the images. The path reaches the script through the environment,
# so spaces and shell characters in it are safe.
ifdef DMS_DIR
test-dms: $(ADFTOOL)
	tools/dms_compare.sh "$$DMS_DIR"
else
test-dms:
	@echo "skipping: set DMS_DIR"
endif

# Runs the 68000 single-step suite (a clone of SingleStepTests/680x0 in
# M68K_TESTS_DIR) through the translator, over the files of 68000/v1 that
# match M68K_FILES. Each is unpacked into M68K_SUITE once. Both variables
# reach the recipe through the environment, so spaces and shell characters
# in them are safe; M68K_FILES is a glob, expanded there.
M68K_SUITE := $(BUILD)/m68k-suite
M68K_FILES ?= *
export M68K_TESTS_DIR M68K_FILES

ifdef M68K_TESTS_DIR
test-m68k: $(BUILD)/tests/test_m68k_suite
	@mkdir -p $(M68K_SUITE)
	@set -e; set -- ; \
	for gz in "$$M68K_TESTS_DIR"/68000/v1/$$M68K_FILES.json.gz; do \
	    [ -f "$$gz" ] || { echo "no suite file matches $$gz" >&2; exit 1; }; \
	    json="$(M68K_SUITE)/$$(basename "$$gz" .gz)"; \
	    [ "$$json" -nt "$$gz" ] || { zcat "$$gz" > "$$json.part"; mv "$$json.part" "$$json"; }; \
	    set -- "$$@" "$$json"; \
	done; \
	$< "$$@"
else
test-m68k:
	@echo "skipping: set M68K_TESTS_DIR"
endif

clean:
	rm -rf $(BUILD)

-include $(LOADER_OBJ:.o=.d)
-include $(KERNEL_OBJ:.o=.d)
-include $(TEST_BIN:%=%.d)
-include $(ADFTOOL).d
-include $(MKHDF).d
-include $(MKDISK).d
-include $(MKPICDISK).d
-include $(ELF2HUNK).d
-include $(COMMON_HOBJ:.o=.d)
