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

#include "mem_init.hpp"

#include "../common/serial.hpp"

namespace hombre {

namespace {

// The allocator keeps a pointer to this, so it lives as long as the kernel.
constinit u64 direct_map_base = 0;

u8* direct_map_ptr(void* ctx, u64 phys) {
    return reinterpret_cast<u8*>(*static_cast<const u64*>(ctx) + phys);
}

// Page-table pages for the MMIO window come from the physical allocator.
u64 alloc_table_page(void* ctx) {
    return static_cast<PhysAllocator*>(ctx)->alloc_pages(1);
}

u64* table_page_ptr(void* ctx, u64 phys) {
    return reinterpret_cast<u64*>(static_cast<PhysAllocator*>(ctx)->to_ptr(phys));
}

// The MMIO window maps device memory no-execute, which needs EFER.NXE.
// Returns false when the CPU has no NX bit.
bool enable_nx() {
    constexpr u32 efer_msr = 0xC0000080;
    constexpr u32 efer_nxe = 1u << 11;
    u32 eax = 0x80000001, ebx, ecx = 0, edx;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if ((edx & (1u << 20)) == 0) {
        return false;
    }
    u32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(efer_msr));
    asm volatile("wrmsr" : : "a"(lo | efer_nxe), "d"(hi), "c"(efer_msr));
    return true;
}

}

bool mem_init(const BootInfo& info, PhysAllocator& pages, MmioMapper& mmio) {
    direct_map_base = info.direct_map_base;
    if (!pages.init(info.regions, info.region_count, &direct_map_ptr, &direct_map_base)) {
        serial::write("page allocator failed\n");
        return false;
    }
    if (!enable_nx()) {
        serial::write("no nx support\n");
        return false;
    }
    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    const PageAllocator tables{&alloc_table_page, &table_page_ptr, &pages};
    if (!mmio.init(PageTables::adopt(tables, cr3 & 0x000FFFFFFFFFF000))) {
        serial::write("mmio init failed\n");
        return false;
    }
    return true;
}

}
