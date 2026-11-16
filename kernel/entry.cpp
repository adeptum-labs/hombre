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

#include "../common/boot_info.hpp"
#include "../common/serial.hpp"
#include "framebuffer.hpp"
#include "percpu.hpp"

using namespace hombre;

namespace {

PerCpu bsp_cpu;

[[noreturn]] void halt() {
    for (;;) {
        asm volatile("cli; hlt");
    }
}

u64 usable_mib(const BootInfo& info) {
    u64 pages = 0;
    for (u32 i = 0; i < info.region_count; ++i) {
        if (info.regions[i].type == MemType::Usable) {
            pages += info.regions[i].pages;
        }
    }
    return pages / 256;  // 256 pages of 4 KiB per MiB
}

// The loader's identity map is still in the page tables. Clearing the lower
// half of the PML4 removes it, and reloading CR3 flushes the TLB. Nothing may
// use a lower-half address afterwards.
void unmap_lower_half(u64 direct_map_base) {
    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    volatile u64* pml4 =
        reinterpret_cast<volatile u64*>(direct_map_base + (cr3 & 0x000FFFFFFFFFF000));
    for (u32 i = 0; i < 256; ++i) {
        pml4[i] = 0;
    }
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

void draw_banner(const BootInfo& info) {
    constexpr const char* text = "Hombre";
    constexpr u32 scale = 4;
    constexpr u32 text_width = 6 * 8 * scale;
    Framebuffer fb(reinterpret_cast<u8*>(info.direct_map_base + info.fb.phys), info.fb.width,
                   info.fb.height, info.fb.pitch_bytes, info.fb.format);
    fb.fill(0x000040);
    const u32 x = info.fb.width > text_width ? (info.fb.width - text_width) / 2 : 0;
    fb.draw_text(x, info.fb.height / 4, text, 0xE0E0FF, scale);
}

}

extern "C" [[noreturn]] void kmain(BootInfo* info) {
    serial::init();
    if (info == nullptr || info->magic != boot_info_magic ||
        info->version != boot_info_version) {
        serial::write("kernel: unexpected boot info\n");
        halt();
    }

    unmap_lower_half(info->direct_map_base);
    serial::write("lower half unmapped\n");

    percpu_init(bsp_cpu, 0);
    if (this_cpu().cpu_id == 0) {
        serial::write("cpu0 percpu ok\n");
    }

    serial::write("Hombre kernel booted\n");
    serial::write("usable memory: ");
    serial::write_dec(usable_mib(*info));
    serial::write(" MiB\n");
    if (info->rsdp_phys == 0) {
        serial::write("rsdp: none\n");
    } else {
        serial::write("rsdp: 0x");
        serial::write_hex(info->rsdp_phys);
        serial::write("\n");
    }
    if (info->fb.phys != 0) {
        draw_banner(*info);
    } else {
        serial::write("no framebuffer\n");
    }
    serial::write("halting\n");
    halt();
}
