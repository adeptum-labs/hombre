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

#include "../pixel_format.hpp"
#include "../types.hpp"

// The deep (chunky) bitmaps graphics.library made. A deep BitMap looks like
// a graphics card's to a program: bm_Planes[0] holds the address of its
// pixels and bm_BytesPerRow the bytes of one row. Hombre knows a bitmap is
// deep only by finding it here, so a BitMap a program builds itself, with
// whatever bm_Depth, stays planar.

namespace hombre {

struct DeepBitmap {
    u32 bitmap = 0;
    u32 pixels = 0;
    u32 pixel_bytes = 0;
    u32 width = 0;
    u32 height = 0;
    u32 bytes_per_row = 0;
    PixFmt fmt = PixFmt::Lut8;
    // The ColorMap its pens take their colours from, 0 for the default
    // palette.
    u32 colormap = 0;
    // The 256 colours its pens draw in, as 0x00RRGGBB: the screen's, shared
    // by its windows. Null for the default palette.
    const u32* palette = nullptr;
};

// The bytes of a row and of the whole of a deep bitmap of width by height
// pixels in fmt; rows are 16-byte aligned. False when it would not fit a
// BitMap's 16-bit row size or 2 GiB.
inline bool deep_size(u32 width, u32 height, PixFmt fmt, u32& bytes_per_row, u32& bytes) {
    if (width == 0 || height == 0 || width > 0x7FFF || height > 0x7FFF) {
        return false;
    }
    const u32 row = (width * bytes_per_pixel(fmt) + 15) & ~15u;
    const u64 total = u64{row} * height;
    if (row > 0xFFFF || total > 0x7FFFFFFF) {
        return false;
    }
    bytes_per_row = row;
    bytes = static_cast<u32>(total);
    return true;
}

class DeepBitmaps {
public:
    static constexpr u32 kCapacity = 256;

    bool add(const DeepBitmap& bitmap) {
        for (DeepBitmap& slot : slots_) {
            if (slot.bitmap == 0) {
                slot = bitmap;
                return true;
            }
        }
        return false;
    }

    const DeepBitmap* find(u32 bitmap) const {
        if (bitmap == 0) {
            return nullptr;
        }
        for (const DeepBitmap& slot : slots_) {
            if (slot.bitmap == bitmap) {
                return &slot;
            }
        }
        return nullptr;
    }

    DeepBitmap* find(u32 bitmap) {
        return const_cast<DeepBitmap*>(static_cast<const DeepBitmaps&>(*this).find(bitmap));
    }

    void remove(u32 bitmap) {
        if (DeepBitmap* slot = find(bitmap)) {
            *slot = DeepBitmap{};
        }
    }

private:
    DeepBitmap slots_[kCapacity]{};
};

}
