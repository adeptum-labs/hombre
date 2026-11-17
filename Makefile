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
KCXX      := g++
KLD       := ld.bfd
KCXXFLAGS := -std=c++23 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
             -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx -nostdlib \
             -Wall -Wextra -Werror -MMD -MP
KLDFLAGS  := -nostdlib -T kernel/linker.ld -z max-page-size=0x1000
# The mem* routines are built with these: they keep the compiler from turning
# their loops back into calls to the very functions being defined.
KMEMFLAGS := -fno-builtin -fno-tree-loop-distribute-patterns

KERNEL_SRC := $(wildcard kernel/*.cpp)
KERNEL_OBJ := $(KERNEL_SRC:%.cpp=$(BUILD)/%.o) $(BUILD)/kernel/mem.o
KERNEL     := $(BUILD)/KERNEL.ELF

# Kernels that provoke a CPU fault on purpose, one per value of
# HOMBRE_FAULT_TEST. Each is built into its own directory.
FAULT_TESTS := 1 2 3

.PHONY: all image image-nokernel run check test test-dms clean

all: image

image: $(IMAGE)

image-nokernel: $(IMAGE_NOKERN)

$(filter-out %/mem.o,$(LOADER_OBJ)): $(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(filter-out %/mem.o,$(KERNEL_OBJ)): $(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(KCXX) $(KCXXFLAGS) -c $< -o $@

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
FAULT$(1)_OBJ := $(KERNEL_SRC:%.cpp=$$(FAULT$(1)_DIR)/%.o) $$(FAULT$(1)_DIR)/kernel/mem.o

.PHONY: image-fault$(1)
image-fault$(1): $(BUILD)/hombre-fault$(1).img

$$(filter-out %/mem.o,$$(FAULT$(1)_OBJ)): $$(FAULT$(1)_DIR)/%.o: %.cpp
	@mkdir -p $$(@D)
	$$(KCXX) $$(KCXXFLAGS) -DHOMBRE_FAULT_TEST=$(1) -c $$< -o $$@

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
run: image
	cp $(OVMF)/OVMF_VARS_4M.fd $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 -enable-kvm -cpu host,-pdpe1gb -machine q35 -m 256M -no-reboot \
	    -serial stdio \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF)/OVMF_CODE_4M.fd \
	    -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	    -drive format=raw,file=$(IMAGE)

check: image image-nokernel $(FAULT_TESTS:%=image-fault%)
	tools/boot_check.sh "Hombre loader" "kernel loaded: entry=0xffffffff8010" \
	    "Hombre kernel booted" "usable memory:" "gdt loaded" "idt loaded" "breakpoint ok" \
	    "lower half unmapped" "cpu0 percpu ok" "framebuffer: " "halting"
	tools/boot_check.sh --no-kernel "KERNEL.ELF not found"
	tools/boot_check.sh --no-gop "no framebuffer" "halting"
	tools/boot_check.sh --fault-test 1 "fault test: 1" "*** exception 14 #PF (page fault)" \
	    "error: 0x0" "cs: 0x8 rflags: 0x" "ss: 0x10" "cr2: 0x1000 cr3" "halting"
	tools/boot_check.sh --fault-test 2 "fault test: 2" "*** exception 8 #DF (double fault)" \
	    "cs: 0x8 rflags: 0x" "rsp: 0x1000 ss: 0x10" "halting"
	tools/boot_check.sh --fault-test 3 "fault test: 3" "*** unexpected interrupt 128" \
	    "cs: 0x8 rflags: 0x" "halting"

# Host unit tests: every tests/test_*.cpp is its own program, built with the
# host compiler under the sanitizers and run in turn.
HOSTCXX      := g++
HOSTCXXFLAGS := -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
                -fno-sanitize-recover=undefined -Icommon -Ikernel -I. -Itools -MMD -MP
TEST_SRC     := $(wildcard tests/test_*.cpp)
TEST_BIN     := $(TEST_SRC:tests/%.cpp=$(BUILD)/tests/%)

$(BUILD)/tests/%: tests/%.cpp
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< -o $@

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do $$t || exit 1; done

# Host tools, built like the tests.
ADFTOOL := $(BUILD)/tools/adftool

$(ADFTOOL): tools/adftool.cpp
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< -o $@

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

clean:
	rm -rf $(BUILD)

-include $(LOADER_OBJ:.o=.d)
-include $(KERNEL_OBJ:.o=.d)
-include $(TEST_BIN:%=%.d)
-include $(ADFTOOL).d
