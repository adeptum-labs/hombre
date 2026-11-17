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

#include "code_region.hpp"

#include "../common/paging.hpp"

namespace hombre {

namespace {

constexpr u64 page_size = 4096;
constexpr u64 large_page_size = 2ull << 20;

// Page-table pages for the window come from the physical allocator.
u64 alloc_table_page(void* ctx) {
    return static_cast<PhysAllocator*>(ctx)->alloc_pages(1);
}

u64* table_page_ptr(void* ctx, u64 phys) {
    return reinterpret_cast<u64*>(static_cast<PhysAllocator*>(ctx)->to_ptr(phys));
}

// Drops the TLB entries of the 2 MiB pages of [virt, virt + bytes).
void flush_2m(u64 virt, u64 bytes) {
    for (u64 offset = 0; offset < bytes; offset += large_page_size) {
        asm volatile("invlpg (%0)" : : "r"(virt + offset) : "memory");
    }
}

// Makes the 2 MiB pages of [virt, virt + bytes) no-execute.
bool make_nx(PageTables& tables, u64 virt, u64 bytes) {
    if (!tables.protect_2m(virt, bytes, NoExecute, 0)) {
        return false;
    }
    flush_2m(virt, bytes);
    return true;
}

}

bool code_region_create(const BootInfo& info, PhysAllocator& pages, u64 bytes,
                        m68k::CodeRegion& out) {
    if (bytes == 0 || bytes > (1ull << 30)) {
        return false;
    }
    bytes = (bytes + large_page_size - 1) & ~(large_page_size - 1);
    const u64 phys = pages.alloc_pages_aligned(bytes / page_size, large_page_size / page_size);
    if (phys == 0) {
        return false;
    }

    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    const PageAllocator table_pages{&alloc_table_page, &table_page_ptr, &pages};
    PageTables tables = PageTables::adopt(table_pages, cr3 & 0x000FFFFFFFFFF000ull);

    // The kernel window maps physical memory from 0 up to its size, so the
    // part of the region below that is reachable there as well.
    const u64 window_end = phys + bytes < kKernelWindowSize ? phys + bytes : kKernelWindowSize;
    const bool in_window = phys < kKernelWindowSize;
    if (!make_nx(tables, info.direct_map_base + phys, bytes) ||
        (in_window && !make_nx(tables, kKernelBase + phys, window_end - phys))) {
        pages.free_pages(phys, bytes / page_size);
        return false;
    }
    // The window's addresses never held a translation, so no flush.
    if (!tables.map_2m(kCodeExecBase, phys, bytes, Present)) {
        return false;
    }

    out.write = reinterpret_cast<u8*>(info.direct_map_base + phys);
    out.exec = reinterpret_cast<const u8*>(kCodeExecBase);
    out.bytes = static_cast<usize>(bytes);
    return true;
}

}
