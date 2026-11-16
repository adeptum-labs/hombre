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

#pragma once

#include "boot_info.hpp"
#include "types.hpp"

namespace hombre {

// One entry of a UEFI memory map. The firmware may space entries further
// apart than sizeof(EfiMemoryDescriptor); the real stride is the descriptor
// size GetMemoryMap returns.
struct EfiMemoryDescriptor {
    u32 type;
    u32 pad;
    u64 phys;
    u64 virt;
    u64 pages;
    u64 attribute;
};

static_assert(sizeof(EfiMemoryDescriptor) == 40);

namespace memmap_detail {

inline constexpr u64 page_shift = 12;

inline MemType convert_type(u32 efi_type) {
    switch (efi_type) {
    case 1:  // EfiLoaderCode
    case 2:  // EfiLoaderData
        return MemType::Loader;
    case 3:  // EfiBootServicesCode
    case 4:  // EfiBootServicesData
    case 7:  // EfiConventionalMemory
        return MemType::Usable;
    case 9:  // EfiACPIReclaimMemory
        return MemType::AcpiReclaimable;
    case 10:  // EfiACPIMemoryNVS
        return MemType::AcpiNvs;
    case 11:  // EfiMemoryMappedIO
    case 12:  // EfiMemoryMappedIOPortSpace
        return MemType::Mmio;
    default:
        return MemType::Reserved;
    }
}

}

// Converts the UEFI memory map in map[0, map_bytes), whose entries are
// descriptor_size bytes apart, into out[0, capacity) and returns the number
// of regions written. A trailing partial descriptor is ignored, as are
// zero-length descriptors and descriptors that run past the top of the
// address space. A descriptor of the same type that starts exactly where the
// previous region ends is merged into it. Regions keep the input order: the
// firmware sorts its map, and unsorted or overlapping descriptors are passed
// through without being sorted or merged across. Returns 0 when the arguments
// are invalid or the regions do not fit in capacity.
inline u32 convert_efi_map(const u8* map, u64 map_bytes, u64 descriptor_size, MemoryRegion* out,
                           u32 capacity) {
    using namespace memmap_detail;

    if (map == nullptr || out == nullptr || descriptor_size < sizeof(EfiMemoryDescriptor)) {
        return 0;
    }

    u32 count = 0;
    for (u64 offset = 0; descriptor_size <= map_bytes - offset; offset += descriptor_size) {
        // Copied out because entries need not be 8-byte aligned.
        EfiMemoryDescriptor d;
        __builtin_memcpy(&d, map + offset, sizeof(d));

        // The last byte, phys + (pages << 12) - 1, must not wrap.
        if (d.pages == 0 || d.pages > (~d.phys >> page_shift)) {
            continue;
        }
        const MemType type = convert_type(d.type);

        if (count > 0) {
            MemoryRegion& last = out[count - 1];
            if (last.type == type && last.phys + (last.pages << page_shift) == d.phys) {
                last.pages += d.pages;
                continue;
            }
        }
        if (count == capacity) {
            return 0;
        }
        out[count++] = MemoryRegion{d.phys, d.pages, type, 0};
    }
    return count;
}

}
