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

#include "../common/types.hpp"

namespace hombre {

// One 8-byte GDT entry, split the way the CPU reads it.
struct GdtEntry {
    u16 limit_low;
    u16 base_low;
    u8 base_mid;
    u8 access;
    u8 flags_limit_high;
    u8 base_high;
};

// A 64-bit system descriptor takes two GDT slots.
struct TssDescriptor {
    GdtEntry low;
    u32 base_upper;
    u32 reserved;
};

// 64-bit task state segment. Packed because the rsp and ist entries sit at
// offsets that are not multiples of 8.
struct __attribute__((packed)) Tss {
    u32 reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

struct IdtGate {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
};

static_assert(sizeof(GdtEntry) == 8);
static_assert(sizeof(TssDescriptor) == 16);
static_assert(sizeof(Tss) == 104);
static_assert(__builtin_offsetof(Tss, ist) == 36);
static_assert(__builtin_offsetof(Tss, iomap_base) == 102);
static_assert(sizeof(IdtGate) == 16);

// Present, DPL 0, 64-bit code segment: access 0x9A, flags L|G, limit 0xFFFF.
constexpr GdtEntry make_code_segment64() {
    return GdtEntry{0xFFFF, 0, 0, 0x9A, 0xAF, 0};
}

// Present, DPL 0, writable data segment: access 0x92, flags D|G.
constexpr GdtEntry make_data_segment() {
    return GdtEntry{0xFFFF, 0, 0, 0x92, 0xCF, 0};
}

// Available 64-bit TSS (type 0x9). Only the low 20 bits of limit are used.
constexpr TssDescriptor make_tss_descriptor(u64 base, u32 limit) {
    return TssDescriptor{
        GdtEntry{static_cast<u16>(limit), static_cast<u16>(base), static_cast<u8>(base >> 16),
                 0x89, static_cast<u8>((limit >> 16) & 0x0F), static_cast<u8>(base >> 24)},
        static_cast<u32>(base >> 32), 0};
}

// Present, DPL 0, 64-bit interrupt gate. ist selects an IST slot (1-7), or 0
// for none.
constexpr IdtGate make_interrupt_gate(u64 handler, u16 selector, u8 ist) {
    return IdtGate{static_cast<u16>(handler),
                   selector,
                   static_cast<u8>(ist & 0x07),
                   0x8E,
                   static_cast<u16>(handler >> 16),
                   static_cast<u32>(handler >> 32),
                   0};
}

// The entry as the 64-bit value the CPU sees (little endian).
constexpr u64 raw(const GdtEntry& e) {
    return static_cast<u64>(e.limit_low) | static_cast<u64>(e.base_low) << 16 |
           static_cast<u64>(e.base_mid) << 32 | static_cast<u64>(e.access) << 40 |
           static_cast<u64>(e.flags_limit_high) << 48 | static_cast<u64>(e.base_high) << 56;
}

}
