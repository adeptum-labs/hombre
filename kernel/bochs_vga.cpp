/*
 * The Hombre Operating System.
 * Copyright © 2026 Adam Waldenberg and Adeptum AB.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Website: https://www.adeptum.se
 * Contact: info@adeptum.se
 */

#include "bochs_vga.hpp"

#include "../common/port_io.hpp"
#include "../common/serial.hpp"
#include "pci.hpp"

namespace hombre {

namespace {

constexpr u16 kVendor = 0x1234;
constexpr u16 kDevice = 0x1111;
constexpr u8 kPciBar0 = 0x10;

// The DISPI index and data ports and registers (Bochs VBE).
constexpr u16 kIndexPort = 0x01CE;
constexpr u16 kDataPort = 0x01CF;
constexpr u16 kRegId = 0;
constexpr u16 kRegXres = 1;
constexpr u16 kRegYres = 2;
constexpr u16 kRegBpp = 3;
constexpr u16 kRegEnable = 4;
constexpr u16 kRegVirtWidth = 6;
constexpr u16 kRegXOffset = 8;
constexpr u16 kRegYOffset = 9;
constexpr u16 kRegVideoMemory64k = 10;
constexpr u16 kIdFirst = 0xB0C0;
constexpr u16 kIdLast = 0xB0CF;
constexpr u16 kEnabled = 0x01;
constexpr u16 kGetCaps = 0x02;
constexpr u16 kLinearFramebuffer = 0x40;

struct State {
    bool present = false;
    u8* vram = nullptr;
    u64 vram_bytes = 0;
    u32 max_width = 0;
    u32 max_height = 0;
};

constinit State state{};

void write_reg(u16 index, u16 value) {
    outw(kIndexPort, index);
    outw(kDataPort, value);
}

u16 read_reg(u16 index) {
    outw(kIndexPort, index);
    return inw(kDataPort);
}

struct Found {
    bool found = false;
    PciAddress address{};
};

bool find_adapter(void* ctx, PciAddress address, u16 vendor, u16 device) {
    if (vendor != kVendor || device != kDevice) {
        return false;
    }
    Found& found = *static_cast<Found*>(ctx);
    found.found = true;
    found.address = address;
    return true;
}

}

bool bochs_vga_init(MmioMapper& mmio) {
    Found found;
    pci_scan(&find_adapter, &found);
    if (!found.found) {
        return false;
    }
    const u16 id = read_reg(kRegId);
    if (id < kIdFirst || id > kIdLast) {
        return false;
    }
    const u32 bar_low = pci_read32(found.address, kPciBar0);
    u32 bar_high = 0;
    if ((bar_low & 1u) == 0 && ((bar_low >> 1) & 3u) == 2) {
        bar_high = pci_read32(found.address, static_cast<u8>(kPciBar0 + 4));
    }
    const PciBar bar = pci_decode_bar(bar_low, bar_high);
    const u64 vram_bytes = u64{read_reg(kRegVideoMemory64k)} * 64 * 1024;
    if (bar.is_io || bar.phys == 0 || vram_bytes == 0) {
        return false;
    }
    // The largest size and depth the adapter takes, read with GETCAPS; the
    // enable register then goes back as it was.
    const u16 enable = read_reg(kRegEnable);
    write_reg(kRegEnable, kGetCaps);
    const u32 max_width = read_reg(kRegXres);
    const u32 max_height = read_reg(kRegYres);
    write_reg(kRegEnable, enable);
    u8* vram = static_cast<u8*>(const_cast<void*>(mmio.map(bar.phys, vram_bytes)));
    if (vram == nullptr || max_width == 0 || max_height == 0) {
        return false;
    }
    state = State{true, vram, vram_bytes, max_width, max_height};
    serial::write("display: Bochs VGA, ");
    serial::write_dec(vram_bytes >> 20);
    serial::write(" MiB, up to ");
    serial::write_dec(max_width);
    serial::write("x");
    serial::write_dec(max_height);
    serial::write("\n");
    return true;
}

bool bochs_vga_present() { return state.present; }

bool bochs_vga_set_mode(u32 width, u32 height, Framebuffer& out) {
    if (!state.present ||
        !bochs_mode_fits(width, height, state.max_width, state.max_height, state.vram_bytes)) {
        return false;
    }
    write_reg(kRegEnable, 0);
    write_reg(kRegXres, static_cast<u16>(width));
    write_reg(kRegYres, static_cast<u16>(height));
    write_reg(kRegBpp, 32);
    write_reg(kRegVirtWidth, static_cast<u16>(width));
    write_reg(kRegXOffset, 0);
    write_reg(kRegYOffset, 0);
    write_reg(kRegEnable, kEnabled | kLinearFramebuffer);
    out = Framebuffer(state.vram, width, height, width * 4, kFramebufferBgrx8);
    serial::write("display: ");
    serial::write_dec(width);
    serial::write("x");
    serial::write_dec(height);
    serial::write("\n");
    return true;
}

}
