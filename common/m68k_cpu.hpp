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

#include "types.hpp"

namespace hombre::m68k {

enum class ExitReason : u32 {
    None,
    Halt,
    Returned,
    Budget,
    Illegal,
    Trap,
    DivideByZero,
    Chk,
    BusError,
    AddressError,
    Privilege,
    Unimplemented
};

struct CpuState {
    u32 d[8];
    u32 a[8];
    u32 pc;
    u16 sr;  // the CCR is its low byte; X is bit 4
    ExitReason exit;
    u32 fault_addr;
    u32 trap_number;
};

// Big-endian, as the 68000 sees it; word and long accesses need an even address.
struct GuestMemory {
    u8* base;
    u32 size;

    bool read8(u32 addr, u32& out) const {
        if (!range_ok(addr, 1)) {
            return false;
        }
        out = base[addr];
        return true;
    }

    bool read16(u32 addr, u32& out) const {
        if ((addr & 1) != 0 || !range_ok(addr, 2)) {
            return false;
        }
        out = (static_cast<u32>(base[addr]) << 8) | base[addr + 1];
        return true;
    }

    bool read32(u32 addr, u32& out) const {
        if ((addr & 1) != 0 || !range_ok(addr, 4)) {
            return false;
        }
        out = (static_cast<u32>(base[addr]) << 24) | (static_cast<u32>(base[addr + 1]) << 16) |
              (static_cast<u32>(base[addr + 2]) << 8) | base[addr + 3];
        return true;
    }

    bool write8(u32 addr, u32 v) {
        if (!range_ok(addr, 1)) {
            return false;
        }
        base[addr] = static_cast<u8>(v);
        return true;
    }

    bool write16(u32 addr, u32 v) {
        if ((addr & 1) != 0 || !range_ok(addr, 2)) {
            return false;
        }
        base[addr] = static_cast<u8>(v >> 8);
        base[addr + 1] = static_cast<u8>(v);
        return true;
    }

    bool write32(u32 addr, u32 v) {
        if ((addr & 1) != 0 || !range_ok(addr, 4)) {
            return false;
        }
        base[addr] = static_cast<u8>(v >> 24);
        base[addr + 1] = static_cast<u8>(v >> 16);
        base[addr + 2] = static_cast<u8>(v >> 8);
        base[addr + 3] = static_cast<u8>(v);
        return true;
    }

    bool range_ok(u32 addr, u32 bytes) const {
        return static_cast<u64>(addr) + bytes <= size;
    }
};

constexpr u32 kGuestBytes = 16u << 20;

}
