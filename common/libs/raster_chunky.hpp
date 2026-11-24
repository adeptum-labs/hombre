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

// Drawing on deep (chunky) bitmaps, for raster.hpp: one pixel after another
// in the bitmap's PixFmt. A pen becomes the colour the bitmap's palette
// gives it on RGB16 and ARGB32, and stays a pen on LUT8. Callers clip to
// the bitmap first; every row is reached through Bus::bytes_at.
//
// Included by raster.hpp after Bitmap, Clip and its small helpers.

namespace hombre::raster {

namespace chunky {

// The palette of a deep bitmap without one: the Workbench's first four
// colours, then a grey ramp.
inline const u32* default_palette() {
    static u32 palette[256] = {};
    static bool made = false;
    if (!made) {
        for (u32 i = 0; i < 256; ++i) {
            palette[i] = (i << 16) | (i << 8) | i;
        }
        palette[0] = 0xAAAAAA;
        palette[1] = 0x000000;
        palette[2] = 0xFFFFFF;
        palette[3] = 0x6688BB;
        made = true;
    }
    return palette;
}

inline const u32* palette_of(const Bitmap& bm) {
    return bm.palette != nullptr ? bm.palette : default_palette();
}

inline u32 bpp(const Bitmap& bm) { return bytes_per_pixel(bm.fmt); }

// The stored value of a pen.
inline u32 pen_value(const Bitmap& bm, u32 pen) {
    if (bm.fmt == PixFmt::Lut8) {
        return pen & 0xFF;
    }
    const u32 xrgb = palette_of(bm)[pen & 0xFF];
    return encode_rgb(bm.fmt, static_cast<u8>(xrgb >> 16), static_cast<u8>(xrgb >> 8),
                      static_cast<u8>(xrgb));
}

// The pen of palette whose colour is nearest r, g, b.
inline u32 nearest_pen(const u32* palette, u8 r, u8 g, u8 b) {
    u32 best = 0;
    u32 best_distance = 0xFFFFFFFF;
    for (u32 pen = 0; pen < 256; ++pen) {
        const i32 dr = static_cast<i32>((palette[pen] >> 16) & 0xFF) - r;
        const i32 dg = static_cast<i32>((palette[pen] >> 8) & 0xFF) - g;
        const i32 db = static_cast<i32>(palette[pen] & 0xFF) - b;
        const u32 distance = static_cast<u32>(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best = pen;
            best_distance = distance;
            if (distance == 0) {
                break;
            }
        }
    }
    return best;
}

// The colour of a stored value, as 0x00RRGGBB.
inline u32 value_rgb(const Bitmap& bm, u32 value) {
    if (bm.fmt == PixFmt::Lut8) {
        return palette_of(bm)[value & 0xFF] & 0xFFFFFF;
    }
    u8 r = 0, g = 0, b = 0;
    decode_rgb(bm.fmt, value, r, g, b);
    return (u32{r} << 16) | (u32{g} << 8) | b;
}

// The stored value of a colour, a pen by the palette on LUT8.
inline u32 rgb_value(const Bitmap& bm, u32 xrgb) {
    const u8 r = static_cast<u8>(xrgb >> 16);
    const u8 g = static_cast<u8>(xrgb >> 8);
    const u8 b = static_cast<u8>(xrgb);
    return bm.fmt == PixFmt::Lut8 ? nearest_pen(palette_of(bm), r, g, b)
                                  : encode_rgb(bm.fmt, r, g, b);
}

// count pixels of row y from x, or null when they are not all in guest
// memory.
inline u8* row(native::Bus& bus, const Bitmap& bm, u32 x, u32 y, u32 count) {
    return bus.bytes_at(bm.pixels + y * bm.bytes_per_row + x * bpp(bm), count * bpp(bm));
}

inline u32 read_value(native::Bus& bus, const Bitmap& bm, u32 x, u32 y) {
    const u8* at = row(bus, bm, x, y, 1);
    return at != nullptr ? load_pixel(bm.fmt, at) : 0;
}

inline void write_value(native::Bus& bus, const Bitmap& bm, u32 x, u32 y, u32 value) {
    if (u8* at = row(bus, bm, x, y, 1)) {
        store_pixel(bm.fmt, at, value);
    }
}

// A pen read back: itself on LUT8, the nearest one of the palette else.
inline u32 read_pen(native::Bus& bus, const Bitmap& bm, u32 x, u32 y) {
    const u32 value = read_value(bus, bm, x, y);
    if (bm.fmt == PixFmt::Lut8) {
        return value;
    }
    const u32 xrgb = value_rgb(bm, value);
    return nearest_pen(palette_of(bm), static_cast<u8>(xrgb >> 16), static_cast<u8>(xrgb >> 8),
                       static_cast<u8>(xrgb));
}

// Fills [left, right] of rows top to bottom with value; on LUT8 only the
// bits of plane_mask change.
inline void fill(native::Bus& bus, const Bitmap& bm, i64 left, i64 top, i64 right, i64 bottom,
                 u32 value, u8 plane_mask) {
    const u32 count = static_cast<u32>(right - left + 1);
    for (i64 y = top; y <= bottom; ++y) {
        u8* at = row(bus, bm, static_cast<u32>(left), static_cast<u32>(y), count);
        if (at == nullptr) {
            return;
        }
        if (bm.fmt == PixFmt::Lut8) {
            const u8 v = static_cast<u8>(value);
            for (u32 i = 0; i < count; ++i) {
                at[i] = plane_mask == 0xFF ? v : static_cast<u8>((at[i] & ~plane_mask) |
                                                                 (v & plane_mask));
            }
            continue;
        }
        const u32 step = bpp(bm);
        store_pixel(bm.fmt, at, value);
        for (u32 i = 1; i < count; ++i) {
            __builtin_memcpy(at + i * step, at, step);
        }
    }
}

// Turns over the pens' mask bits on LUT8, and the colour bits on RGB.
inline void complement(native::Bus& bus, const Bitmap& bm, i64 left, i64 top, i64 right,
                       i64 bottom, u32 mask) {
    const u32 count = static_cast<u32>(right - left + 1);
    const u32 flip = bm.fmt == PixFmt::Lut8 ? (mask & 0xFF)
                     : bm.fmt == PixFmt::Rgb16 ? 0xFFFFu
                                               : 0x00FFFFFFu;
    for (i64 y = top; y <= bottom; ++y) {
        u8* at = row(bus, bm, static_cast<u32>(left), static_cast<u32>(y), count);
        if (at == nullptr) {
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            u8* p = at + i * bpp(bm);
            store_pixel(bm.fmt, p, load_pixel(bm.fmt, p) ^ flip);
        }
    }
}

// Moves the pixels of [left, right] x [top, bottom] by -dx, -dy inside that
// area, as ScrollRaster does; what would come from outside is fill.
inline void scroll(native::Bus& bus, const Bitmap& bm, i64 left, i64 top, i64 right, i64 bottom,
                   i32 dx, i32 dy, u32 fill_value) {
    const u32 count = static_cast<u32>(right - left + 1);
    const u32 step = bpp(bm);
    for (i64 i = 0; i <= bottom - top; ++i) {
        const i64 y = dy > 0 ? top + i : bottom - i;
        const i64 source_y = y + dy;
        u8* out = row(bus, bm, static_cast<u32>(left), static_cast<u32>(y), count);
        if (out == nullptr) {
            return;
        }
        if (source_y < top || source_y > bottom) {
            for (u32 x = 0; x < count; ++x) {
                store_pixel(bm.fmt, out + x * step, fill_value);
            }
            continue;
        }
        const u8* in = row(bus, bm, static_cast<u32>(left), static_cast<u32>(source_y), count);
        if (in == nullptr) {
            return;
        }
        // Columns whose source lies inside the area, then the rest filled.
        const i64 kept = static_cast<i64>(count) - (dx > 0 ? dx : -dx);
        if (kept <= 0) {
            for (u32 x = 0; x < count; ++x) {
                store_pixel(bm.fmt, out + x * step, fill_value);
            }
            continue;
        }
        const u32 kept_bytes = static_cast<u32>(kept) * step;
        if (dx >= 0) {
            __builtin_memmove(out, in + static_cast<u32>(dx) * step, kept_bytes);
            for (u32 x = static_cast<u32>(kept); x < count; ++x) {
                store_pixel(bm.fmt, out + x * step, fill_value);
            }
        } else {
            __builtin_memmove(out + static_cast<u32>(-dx) * step, in, kept_bytes);
            for (u32 x = 0; x < static_cast<u32>(-dx); ++x) {
                store_pixel(bm.fmt, out + x * step, fill_value);
            }
        }
    }
}

}

}
