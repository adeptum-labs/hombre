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

#include "../types.hpp"

// The pens of an indexed screen a 4-bit coverage font's levels map to.
//
// For a foreground and a background pen, the levels blend their colours in
// linear light, and each level takes the palette pen nearest to its blend.
// A straight blend of the pens' 8-bit values would darken the middle of
// the ramp, so the colours are squared, mixed and taken back to 8 bits
// with an exact integer square root.

namespace hombre::text_blend {

constexpr u32 kLevels = 16;

// The pen of the first count colours of rgb nearest to r, g, b by the sum
// of their squared channel differences, the lower pen when two are as
// near. 0 when there are no colours.
u8 nearest(const u8 (*rgb)[3], u32 count, u32 r, u32 g, u32 b);

// The ramps of a screen's palette, computed on first use and kept until
// the palette changes. It holds nothing but its own members, so a screen
// can carry one.
class Cache {
public:
    // Copies count colours of rgb, up to 256, and drops every ramp.
    void set_palette(const u8 (*rgb)[3], u32 count);
    bool has_palette() const;
    // The kLevels pens from bg at level 0 to fg at level kLevels - 1, or
    // nullptr for a pen outside the palette.
    const u8* ramp(u8 fg, u8 bg);
    // Drops every ramp, keeping the palette.
    void invalidate();

private:
    static constexpr u32 kPens = 256;
    static constexpr u32 kSlots = 256;

    // A direct-mapped entry: the key of the pair whose ramp it holds, so a
    // collision recomputes rather than returning another pair's ramp.
    struct Slot {
        u16 key;
        bool filled;
        u8 pens[kLevels];
    };

    u8 palette_[kPens][3]{};
    Slot slots_[kSlots]{};
    u32 count_ = 0;
};

}
