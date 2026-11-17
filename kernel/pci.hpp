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

#include "../common/port_io.hpp"
#include "../common/types.hpp"

namespace hombre {

inline constexpr u16 kPciConfigAddressPort = 0xCF8;
inline constexpr u16 kPciConfigDataPort = 0xCFC;

inline constexpr u8 kPciVendorOffset = 0x00;
inline constexpr u8 kPciCommandOffset = 0x04;
inline constexpr u8 kPciHeaderTypeOffset = 0x0C;

inline constexpr u32 kPciCommandMemorySpace = 1u << 1;
inline constexpr u32 kPciCommandBusMaster = 1u << 2;

struct PciAddress {
    u8 bus;
    u8 device;
    u8 function;
};

// Value for the 0xCF8 address port. The two low offset bits select nothing
// within a dword and are dropped.
constexpr u32 pci_config_address(PciAddress a, u8 offset) {
    return 0x80000000u | (static_cast<u32>(a.bus) << 16) | (static_cast<u32>(a.device) << 11) |
           (static_cast<u32>(a.function) << 8) | (offset & 0xFCu);
}

// Configuration mechanism 1. Not reentrant: the address and data ports are a
// pair, so callers must not interleave accesses from several CPUs.
inline u32 pci_read32(PciAddress a, u8 offset) {
    outl(kPciConfigAddressPort, pci_config_address(a, offset));
    return inl(kPciConfigDataPort);
}

inline void pci_write32(PciAddress a, u8 offset, u32 value) {
    outl(kPciConfigAddressPort, pci_config_address(a, offset));
    outl(kPciConfigDataPort, value);
}

inline u16 pci_read16(PciAddress a, u8 offset) {
    return static_cast<u16>(pci_read32(a, offset) >> ((offset & 2u) * 8));
}

inline u8 pci_read8(PciAddress a, u8 offset) {
    return static_cast<u8>(pci_read32(a, offset) >> ((offset & 3u) * 8));
}

struct PciBar {
    u64 phys;
    bool is_io;
    bool is_64bit;
};

// Decodes a base address register from its dword and, for a 64-bit memory
// BAR, the dword after it. The size is not probed: firmware has already
// assigned the address.
constexpr PciBar pci_decode_bar(u32 low, u32 high) {
    if ((low & 1u) != 0) {
        return PciBar{low & ~3u, true, false};
    }
    const bool wide = ((low >> 1) & 3u) == 2;
    u64 phys = low & ~0xFu;
    if (wide) {
        phys |= static_cast<u64>(high) << 32;
    }
    return PciBar{phys, false, wide};
}

// Called for every function found. Return true to stop the scan.
using PciVisitor = bool (*)(void* ctx, PciAddress address, u16 vendor, u16 device);

// Walks buses 0 to 255 and devices 0 to 31. Functions 1 to 7 are probed only
// when function 0 flags a multi-function device. Returns early when the
// visitor asks for it.
inline void pci_scan(PciVisitor visitor, void* ctx) {
    for (u32 bus = 0; bus < 256; ++bus) {
        for (u32 device = 0; device < 32; ++device) {
            const PciAddress first{static_cast<u8>(bus), static_cast<u8>(device), 0};
            if (pci_read16(first, kPciVendorOffset) == 0xFFFF) {
                continue;
            }
            const bool multi_function = (pci_read8(first, kPciHeaderTypeOffset + 2) & 0x80u) != 0;
            const u32 functions = multi_function ? 8 : 1;
            for (u32 function = 0; function < functions; ++function) {
                const PciAddress address{first.bus, first.device, static_cast<u8>(function)};
                const u32 ids = pci_read32(address, kPciVendorOffset);
                const u16 vendor = static_cast<u16>(ids);
                if (vendor == 0xFFFF) {
                    continue;
                }
                if (visitor(ctx, address, vendor, static_cast<u16>(ids >> 16))) {
                    return;
                }
            }
        }
    }
}

}
