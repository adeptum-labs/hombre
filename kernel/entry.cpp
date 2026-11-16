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

using namespace hombre;

namespace {

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

}

extern "C" [[noreturn]] void kmain(BootInfo* info) {
    serial::init();
    if (info == nullptr || info->magic != boot_info_magic ||
        info->version != boot_info_version) {
        serial::write("kernel: unexpected boot info\n");
        halt();
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
    halt();
}
