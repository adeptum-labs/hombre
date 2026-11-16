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

KERNEL_SRC := $(wildcard kernel/*.cpp)
KERNEL_OBJ := $(KERNEL_SRC:%.cpp=$(BUILD)/%.o) $(BUILD)/kernel/mem.o
KERNEL     := $(BUILD)/KERNEL.ELF

.PHONY: all image image-nokernel run check test clean

all: image

image: $(IMAGE)

image-nokernel: $(IMAGE_NOKERN)

$(filter-out %/mem.o,$(LOADER_OBJ)): $(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(filter-out %/mem.o,$(KERNEL_OBJ)): $(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(KCXX) $(KCXXFLAGS) -c $< -o $@

# The mem* routines are built once per target. -fno-builtin and
# -fno-tree-loop-distribute-patterns keep the compilers from turning their
# loops back into calls to the very functions being defined.
$(BUILD)/loader/mem.o: common/mem.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -fno-builtin -c $< -o $@

$(BUILD)/kernel/mem.o: common/mem.cpp
	@mkdir -p $(@D)
	$(KCXX) $(KCXXFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -c $< -o $@

$(LOADER): $(LOADER_OBJ)
	$(LD) $(LDFLAGS) $^ -o $@

$(KERNEL): $(KERNEL_OBJ) kernel/linker.ld
	$(KLD) $(KLDFLAGS) $(KERNEL_OBJ) -o $@

$(IMAGE): $(LOADER) $(KERNEL) tools/make_image.sh
	tools/make_image.sh $@ $(LOADER) $(KERNEL)

# Same disk without KERNEL.ELF, to exercise the loader's missing-file path.
$(IMAGE_NOKERN): $(LOADER) tools/make_image.sh
	tools/make_image.sh $@ $(LOADER)

# -pdpe1gb: with 1 GiB pages exposed, KVM on this host fails with an internal
# error while the firmware is still in 32-bit mode.
run: image
	cp $(OVMF)/OVMF_VARS_4M.fd $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 -enable-kvm -cpu host,-pdpe1gb -machine q35 -m 256M -no-reboot \
	    -serial stdio \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF)/OVMF_CODE_4M.fd \
	    -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	    -drive format=raw,file=$(IMAGE)

check: image image-nokernel
	tools/boot_check.sh "Hombre loader" "kernel loaded: entry=0xffffffff8010" \
	    "Hombre kernel booted" "usable memory:" "lower half unmapped" \
	    "cpu0 percpu ok" "halting"
	tools/boot_check.sh --no-kernel "KERNEL.ELF not found"
	tools/boot_check.sh --no-gop "no framebuffer" "halting"

# Host unit tests: every tests/test_*.cpp is its own program, built with the
# host compiler under the sanitizers and run in turn.
HOSTCXX      := g++
HOSTCXXFLAGS := -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
                -fno-sanitize-recover=undefined -Icommon -Ikernel -I. -MMD -MP
TEST_SRC     := $(wildcard tests/test_*.cpp)
TEST_BIN     := $(TEST_SRC:tests/%.cpp=$(BUILD)/tests/%)

$(BUILD)/tests/%: tests/%.cpp
	@mkdir -p $(@D)
	$(HOSTCXX) $(HOSTCXXFLAGS) $< -o $@

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do $$t || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(LOADER_OBJ:.o=.d)
-include $(KERNEL_OBJ:.o=.d)
-include $(TEST_BIN:%=%.d)
