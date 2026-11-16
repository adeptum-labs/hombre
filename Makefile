BUILD := build
IMAGE := $(BUILD)/hombre.img
OVMF  := /usr/share/OVMF

# The loader is a COFF object built by clang for the Windows target and
# linked into a PE32+ image by GNU ld, which is what UEFI firmware loads.
CXX      := clang++
LD       := ld.bfd
CXXFLAGS := -target x86_64-windows-gnu -ffreestanding -fno-exceptions -fno-rtti \
            -fshort-wchar -mno-red-zone -fno-stack-protector -std=c++20 \
            -Wall -Wextra -Werror -MMD -MP
LDFLAGS  := -m i386pep --subsystem 10 -e efi_main

LOADER_SRC := $(wildcard loader/*.cpp)
LOADER_OBJ := $(LOADER_SRC:%.cpp=$(BUILD)/%.o)
LOADER     := $(BUILD)/BOOTX64.EFI

.PHONY: all image run check test clean

all: image

image: $(IMAGE)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LOADER): $(LOADER_OBJ)
	$(LD) $(LDFLAGS) $^ -o $@

$(IMAGE): $(LOADER) tools/make_image.sh
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

check: image
	tools/boot_check.sh "Hombre loader"

test:

clean:
	rm -rf $(BUILD)

-include $(LOADER_OBJ:.o=.d)
