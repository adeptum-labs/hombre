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

// The pixel formats of deep bitmaps, the way programs written for Amiga
// graphics cards see them: one byte a pixel through the palette, R5G6B5,
// and A,R,G,B, all in 68000 byte order. The display turns rows of them
// into the 32-bit pixels of the PC's framebuffer.

namespace hombre {

enum class PixFmt : u8 {
    Lut8,
    Rgb16,
    Argb32,
};

constexpr u32 bytes_per_pixel(PixFmt fmt) {
    return fmt == PixFmt::Lut8 ? 1u : fmt == PixFmt::Rgb16 ? 2u : 4u;
}

// The stored value of a colour; 0 for Lut8, whose pixels are pens.
constexpr u32 encode_rgb(PixFmt fmt, u8 r, u8 g, u8 b) {
    switch (fmt) {
    case PixFmt::Rgb16:
        return (u32{r} >> 3 << 11) | (u32{g} >> 2 << 5) | (u32{b} >> 3);
    case PixFmt::Argb32:
        return 0xFF000000u | (u32{r} << 16) | (u32{g} << 8) | b;
    case PixFmt::Lut8:
        break;
    }
    return 0;
}

// The colour of a stored value. The 5 and 6 bit fields of R5G6B5 are
// widened by repeating their top bits, so full intensity stays 0xFF.
constexpr void decode_rgb(PixFmt fmt, u32 value, u8& r, u8& g, u8& b) {
    if (fmt == PixFmt::Rgb16) {
        const u32 r5 = (value >> 11) & 0x1F;
        const u32 g6 = (value >> 5) & 0x3F;
        const u32 b5 = value & 0x1F;
        r = static_cast<u8>((r5 << 3) | (r5 >> 2));
        g = static_cast<u8>((g6 << 2) | (g6 >> 4));
        b = static_cast<u8>((b5 << 3) | (b5 >> 2));
        return;
    }
    r = static_cast<u8>(value >> 16);
    g = static_cast<u8>(value >> 8);
    b = static_cast<u8>(value);
}

inline void store_pixel(PixFmt fmt, u8* at, u32 value) {
    switch (fmt) {
    case PixFmt::Lut8:
        at[0] = static_cast<u8>(value);
        break;
    case PixFmt::Rgb16:
        at[0] = static_cast<u8>(value >> 8);
        at[1] = static_cast<u8>(value);
        break;
    case PixFmt::Argb32:
        at[0] = static_cast<u8>(value >> 24);
        at[1] = static_cast<u8>(value >> 16);
        at[2] = static_cast<u8>(value >> 8);
        at[3] = static_cast<u8>(value);
        break;
    }
}

inline u32 load_pixel(PixFmt fmt, const u8* at) {
    switch (fmt) {
    case PixFmt::Lut8:
        return at[0];
    case PixFmt::Rgb16:
        return (u32{at[0]} << 8) | at[1];
    case PixFmt::Argb32:
        return (u32{at[0]} << 24) | (u32{at[1]} << 16) | (u32{at[2]} << 8) | at[3];
    }
    return 0;
}

// Converts count pixels at src to framebuffer pixels at dst. lut_xrgb holds
// the 256 colours of a Lut8 row as 0x00RRGGBB. A BGRX framebuffer stores
// blue first, which is the little-endian u32 0x00RRGGBB; an RGBX one stores
// red first, 0x00BBGGRR.
inline void row_to_framebuffer(PixFmt fmt, const u8* src, u32 count, const u32* lut_xrgb,
                               bool bgrx, u32* dst) {
    const u32 step = bytes_per_pixel(fmt);
    for (u32 i = 0; i < count; ++i, src += step) {
        u32 xrgb = 0;
        if (fmt == PixFmt::Lut8) {
            xrgb = lut_xrgb[src[0]] & 0x00FFFFFFu;
        } else {
            u8 r = 0, g = 0, b = 0;
            decode_rgb(fmt, load_pixel(fmt, src), r, g, b);
            xrgb = (u32{r} << 16) | (u32{g} << 8) | b;
        }
        dst[i] = bgrx ? xrgb
                      : ((xrgb & 0xFF) << 16) | (xrgb & 0xFF00) | ((xrgb >> 16) & 0xFF);
    }
}

}
