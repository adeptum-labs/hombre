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

namespace hombre {

inline constexpr u32 kPointerWidth = 16;
inline constexpr u32 kPointerHeight = 16;

// The classic arrow pointer bitmap: an 11-pixel wide, 16-pixel high arrow with
// its tip at (0, 0). Bit 15 is the leftmost pixel of a row.
inline constexpr u16 kPointerOutline[16] = {
    0x8000,
    0xC000,
    0xA000,
    0x9000,
    0x8800,
    0x8400,
    0x8200,
    0x8100,
    0x8080,
    0x8040,
    0x80E0,
    0x9100,
    0xA900,
    0xC480,
    0x0240,
    0x0180,
};

inline constexpr u16 kPointerFill[16] = {
    0x0000,
    0x0000,
    0x4000,
    0x6000,
    0x7000,
    0x7800,
    0x7C00,
    0x7E00,
    0x7F00,
    0x7F80,
    0x7F00,
    0x6E00,
    0x4600,
    0x0300,
    0x0180,
    0x0000,
};

// Returns 0 for transparent, 1 for outline (black), 2 for fill (white).
constexpr u32 pointer_pixel(u32 x, u32 y) {
    if (x >= kPointerWidth || y >= kPointerHeight) {
        return 0;
    }
    const u16 mask = static_cast<u16>(1u << (15 - x));
    if ((kPointerOutline[y] & mask) != 0) {
        return 1;
    }
    if ((kPointerFill[y] & mask) != 0) {
        return 2;
    }
    return 0;
}

}
