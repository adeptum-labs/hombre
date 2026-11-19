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

#include "../font.hpp"
#include "../types.hpp"
#include "gfx.hpp"
#include "native.hpp"

// Drawing on a planar Amiga BitMap. Every guest byte goes through
// native::Bus and every loop stays inside the bitmap and the clip, so a
// drawing call cannot touch guest memory outside the bitmap or run away.
//
// Plane p holds bit p of the pen of each pixel; its byte at row y is at
// planes[p] + y * bytes_per_row, and bit 0x80 >> (x & 7) is the pixel at x.

namespace hombre::raster {

struct Bitmap {
    u32 planes[8];
    u32 bytes_per_row;
    u32 rows;
    u32 depth;
    u32 addr;
};

// An inclusive rectangle the drawing stays inside.
struct Clip {
    i32 x0, y0, x1, y1;
};

// The pens and draw mode of one operation.
struct Pens {
    u8 fg, bg, mode;
};

inline void write_pixel(native::Bus& bus, const Bitmap& bitmap, i32 x, i32 y, u32 pen);

namespace {

constexpr u32 kAllOnes = 0xFFFFFFFF;
constexpr u32 kMaxLineSteps = 4096;

bool in_bitmap(const Bitmap& bitmap, i32 x, i32 y) {
    return x >= 0 && y >= 0 && (static_cast<u32>(x) >> 3) < bitmap.bytes_per_row &&
           static_cast<u32>(y) < bitmap.rows;
}

bool visible(const Bitmap& bitmap, const Clip& clip, i64 x, i64 y) {
    if (x < clip.x0 || x > clip.x1 || y < clip.y0 || y > clip.y1) {
        return false;
    }
    return x >= 0 && y >= 0 && (static_cast<u64>(x) >> 3) < bitmap.bytes_per_row &&
           static_cast<u64>(y) < bitmap.rows;
}

u32 plane_address(const Bitmap& bitmap, u32 plane, u32 x, u32 y) {
    return bitmap.planes[plane] + y * bitmap.bytes_per_row + (x >> 3);
}

Rect empty_rect() {
    return Rect{0, 0, -1, -1};
}

void grow(Rect& dirty, i32 x, i32 y) {
    if (dirty.x1 < dirty.x0) {
        dirty = Rect{x, y, x, y};
        return;
    }
    if (x < dirty.x0) {
        dirty.x0 = x;
    }
    if (x > dirty.x1) {
        dirty.x1 = x;
    }
    if (y < dirty.y0) {
        dirty.y0 = y;
    }
    if (y > dirty.y1) {
        dirty.y1 = y;
    }
}

void plot(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, i64 x, i64 y, u32 pen,
          Rect& dirty) {
    if (!visible(bitmap, clip, x, y)) {
        return;
    }
    write_pixel(bus, bitmap, static_cast<i32>(x), static_cast<i32>(y), pen);
    grow(dirty, static_cast<i32>(x), static_cast<i32>(y));
}

}

// The largest bitmap a draw works on: 4096 x 4096 pixels, which bounds the
// work one native call can do.
constexpr u32 kMaxBytesPerRow = 512;
constexpr u32 kMaxRows = 4096;

inline bool load_bitmap(native::Bus& bus, u32 bitmap_addr, Bitmap& out) {
    out.bytes_per_row = bus.r16(bitmap_addr + ndk::bm_BytesPerRow);
    out.rows = bus.r16(bitmap_addr + ndk::bm_Rows);
    out.depth = bus.r8(bitmap_addr + ndk::bm_Depth);
    for (u32 plane = 0; plane < 8; ++plane) {
        out.planes[plane] = bus.r32(bitmap_addr + ndk::bm_Planes + plane * 4);
    }
    out.addr = bitmap_addr;
    if (!bus.ok() || out.depth > 8) {
        return false;
    }
    // The area of one plane must fit in guest memory, whatever the depth,
    // and every plane in use must lie inside it. Together they bound what a
    // draw over the bitmap can visit.
    if (out.bytes_per_row > kMaxBytesPerRow || out.rows > kMaxRows) {
        return false;
    }
    const u32 plane_bytes = out.bytes_per_row * out.rows;
    if (bus.bytes_at(0, plane_bytes) == nullptr) {
        return false;
    }
    for (u32 plane = 0; plane < out.depth; ++plane) {
        if (bus.bytes_at(out.planes[plane], plane_bytes) == nullptr) {
            return false;
        }
    }
    return true;
}

inline u32 read_pixel(native::Bus& bus, const Bitmap& bitmap, i32 x, i32 y) {
    if (!in_bitmap(bitmap, x, y)) {
        return kAllOnes;
    }
    const u32 bit = 0x80u >> (static_cast<u32>(x) & 7);
    u32 pen = 0;
    for (u32 plane = 0; plane < bitmap.depth && plane < 8; ++plane) {
        const u32 at = plane_address(bitmap, plane, static_cast<u32>(x), static_cast<u32>(y));
        if ((bus.r8(at) & bit) != 0) {
            pen |= 1u << plane;
        }
    }
    return pen;
}

inline void write_pixel(native::Bus& bus, const Bitmap& bitmap, i32 x, i32 y, u32 pen) {
    if (!in_bitmap(bitmap, x, y)) {
        return;
    }
    const u32 bit = 0x80u >> (static_cast<u32>(x) & 7);
    for (u32 plane = 0; plane < bitmap.depth && plane < 8; ++plane) {
        const u32 at = plane_address(bitmap, plane, static_cast<u32>(x), static_cast<u32>(y));
        u32 byte = bus.r8(at);
        if (((pen >> plane) & 1) != 0) {
            byte |= bit;
        } else {
            byte &= ~bit;
        }
        bus.w8(at, static_cast<u8>(byte));
    }
}

inline Rect fill_rect(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, Rect r, u32 pen) {
    const i64 width = static_cast<i64>(static_cast<u64>(bitmap.bytes_per_row) << 3);
    const i64 height = static_cast<i64>(bitmap.rows);
    i64 left = r.x0 > clip.x0 ? r.x0 : clip.x0;
    i64 top = r.y0 > clip.y0 ? r.y0 : clip.y0;
    i64 right = r.x1 < clip.x1 ? r.x1 : clip.x1;
    i64 bottom = r.y1 < clip.y1 ? r.y1 : clip.y1;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right >= width) {
        right = width - 1;
    }
    if (bottom >= height) {
        bottom = height - 1;
    }
    if (left > right || top > bottom) {
        return empty_rect();
    }
    for (i64 y = top; y <= bottom && bus.ok(); ++y) {
        for (i64 x = left; x <= right && bus.ok(); ++x) {
            write_pixel(bus, bitmap, static_cast<i32>(x), static_cast<i32>(y), pen);
        }
    }
    return Rect{static_cast<i32>(left), static_cast<i32>(top), static_cast<i32>(right),
                static_cast<i32>(bottom)};
}

inline Rect draw_line(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, i32 x0, i32 y0,
                      i32 x1, i32 y1, u32 pen) {
    Rect dirty = empty_rect();
    i64 x = x0;
    i64 y = y0;
    const i64 dx = x1 > x0 ? static_cast<i64>(x1) - x0 : static_cast<i64>(x0) - x1;
    const i64 dy = -(y1 > y0 ? static_cast<i64>(y1) - y0 : static_cast<i64>(y0) - y1);
    const i64 step_x = x0 < x1 ? 1 : -1;
    const i64 step_y = y0 < y1 ? 1 : -1;
    i64 error = dx + dy;
    for (u32 step = 0; step < kMaxLineSteps; ++step) {
        plot(bus, bitmap, clip, x, y, pen, dirty);
        if (x == x1 && y == y1) {
            break;
        }
        const i64 twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x += step_x;
        }
        if (twice <= dx) {
            error += dx;
            y += step_y;
        }
    }
    return dirty;
}

inline Rect draw_text(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, const Pens& pens,
                      i32 x, i32 baseline_y, const u8* chars, u32 count, u32& advance) {
    advance = 8 * count;
    Rect dirty = empty_rect();
    const bool inverse = (pens.mode & ndk::INVERSVID) != 0;
    const u32 fg = inverse ? pens.bg : pens.fg;
    const u32 bg = inverse ? pens.fg : pens.bg;
    const u32 mode = pens.mode & ~ndk::INVERSVID;
    for (u32 i = 0; i < count; ++i) {
        const u8* rows = glyph(static_cast<char>(chars[i]));
        const i64 left = static_cast<i64>(x) + 8 * static_cast<i64>(i);
        for (u32 row = 0; row < 8; ++row) {
            const i64 py = static_cast<i64>(baseline_y) - 6 + static_cast<i64>(row);
            for (u32 col = 0; col < 8; ++col) {
                const i64 px = left + static_cast<i64>(col);
                if (!visible(bitmap, clip, px, py)) {
                    continue;
                }
                const i32 pixel_x = static_cast<i32>(px);
                const i32 pixel_y = static_cast<i32>(py);
                const bool set = ((rows[row] >> col) & 1) != 0;
                if (mode == ndk::COMPLEMENT) {
                    if (set) {
                        const u32 pixel = read_pixel(bus, bitmap, pixel_x, pixel_y);
                        write_pixel(bus, bitmap, pixel_x, pixel_y, pixel ^ fg);
                        grow(dirty, pixel_x, pixel_y);
                    }
                } else if (mode == ndk::JAM2) {
                    write_pixel(bus, bitmap, pixel_x, pixel_y, set ? fg : bg);
                    grow(dirty, pixel_x, pixel_y);
                } else if (set) {
                    write_pixel(bus, bitmap, pixel_x, pixel_y, fg);
                    grow(dirty, pixel_x, pixel_y);
                }
            }
        }
    }
    return dirty;
}

inline Rect scroll_rect(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, i32 dx, i32 dy,
                        Rect area, u32 bg_pen) {
    const i64 width = static_cast<i64>(static_cast<u64>(bitmap.bytes_per_row) << 3);
    const i64 height = static_cast<i64>(bitmap.rows);
    i64 left = area.x0 > clip.x0 ? area.x0 : clip.x0;
    i64 top = area.y0 > clip.y0 ? area.y0 : clip.y0;
    i64 right = area.x1 < clip.x1 ? area.x1 : clip.x1;
    i64 bottom = area.y1 < clip.y1 ? area.y1 : clip.y1;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right >= width) {
        right = width - 1;
    }
    if (bottom >= height) {
        bottom = height - 1;
    }
    if (left > right || top > bottom) {
        return empty_rect();
    }

    // Each source pixel is read before a write can reach it: a downward
    // move walks from the top, an upward one from the bottom, and the same
    // holds for the horizontal direction.
    for (i64 row = 0; row <= bottom - top && bus.ok(); ++row) {
        const i64 y = dy > 0 ? top + row : bottom - row;
        for (i64 column = 0; column <= right - left && bus.ok(); ++column) {
            const i64 x = dx > 0 ? left + column : right - column;
            const i64 source_x = x + dx;
            const i64 source_y = y + dy;
            u32 pen = bg_pen;
            if (source_x >= left && source_x <= right && source_y >= top && source_y <= bottom) {
                pen = read_pixel(bus, bitmap, static_cast<i32>(source_x),
                                 static_cast<i32>(source_y));
            }
            write_pixel(bus, bitmap, static_cast<i32>(x), static_cast<i32>(y), pen);
        }
    }
    return Rect{static_cast<i32>(left), static_cast<i32>(top), static_cast<i32>(right),
                static_cast<i32>(bottom)};
}

// Clips a copy rectangle against both bitmaps; false if nothing overlaps.
inline bool clip_copy_bounds(const Bitmap& src, const Bitmap& dst, i64& s_x, i64& s_y, i64& d_x,
                             i64& d_y, i64& w, i64& h) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (s_x < 0) {
        w += s_x;
        d_x -= s_x;
        s_x = 0;
    }
    if (d_x < 0) {
        w += d_x;
        s_x -= d_x;
        d_x = 0;
    }
    if (s_y < 0) {
        h += s_y;
        d_y -= s_y;
        s_y = 0;
    }
    if (d_y < 0) {
        h += d_y;
        s_y -= d_y;
        d_y = 0;
    }
    const i64 src_w = static_cast<i64>(static_cast<u64>(src.bytes_per_row) << 3);
    const i64 dst_w = static_cast<i64>(static_cast<u64>(dst.bytes_per_row) << 3);
    const i64 src_h = static_cast<i64>(src.rows);
    const i64 dst_h = static_cast<i64>(dst.rows);
    if (s_x >= src_w || d_x >= dst_w || s_y >= src_h || d_y >= dst_h) {
        return false;
    }
    if (s_x + w > src_w) {
        w = src_w - s_x;
    }
    if (d_x + w > dst_w) {
        w = dst_w - d_x;
    }
    if (s_y + h > src_h) {
        h = src_h - s_y;
    }
    if (d_y + h > dst_h) {
        h = dst_h - d_y;
    }
    return w > 0 && h > 0;
}

// Copies a rectangle between two distinct bitmaps; overlapping regions within
// the same bitmap are not handled. Each destination byte is built from the one
// or two source bytes it straddles and masked at the left and right edges.
inline void copy_rect(native::Bus& bus, const Bitmap& src, i32 sx, i32 sy, const Bitmap& dst,
                      i32 dx, i32 dy, i32 width, i32 height) {
    i64 s_x = sx;
    i64 s_y = sy;
    i64 d_x = dx;
    i64 d_y = dy;
    i64 w = width;
    i64 h = height;
    if (!clip_copy_bounds(src, dst, s_x, s_y, d_x, d_y, w, h)) {
        return;
    }

    u32 depth = src.depth < dst.depth ? src.depth : dst.depth;
    if (depth > 8) {
        depth = 8;
    }
    const u32 ux = static_cast<u32>(s_x);
    const u32 uy = static_cast<u32>(s_y);
    const u32 vx = static_cast<u32>(d_x);
    const u32 vy = static_cast<u32>(d_y);
    const u32 uw = static_cast<u32>(w);
    const u32 uh = static_cast<u32>(h);

    const u32 first_src_byte = ux >> 3;
    const u32 last_src_byte = (ux + uw - 1) >> 3;
    const u32 first_byte = vx >> 3;
    const u32 last_byte = (vx + uw - 1) >> 3;
    const u32 byte_count = last_byte - first_byte + 1;
    const u32 s_bit = ux & 7;
    const u32 d_bit = vx & 7;
    const u8 left_mask = static_cast<u8>(0xFFu >> d_bit);
    const u8 right_mask = static_cast<u8>(0xFFu << (7 - ((vx + uw - 1) & 7)));

    for (u32 plane = 0; plane < depth && bus.ok(); ++plane) {
        for (u32 row = 0; row < uh && bus.ok(); ++row) {
            const u32 src_row = src.planes[plane] + (uy + row) * src.bytes_per_row;
            const u32 dst_row = dst.planes[plane] + (vy + row) * dst.bytes_per_row;
            for (u32 i = 0; i < byte_count && bus.ok(); ++i) {
                const u32 db = first_byte + i;
                u8 mask = 0xFF;
                if (db == first_byte) {
                    mask &= left_mask;
                }
                if (db == last_byte) {
                    mask &= right_mask;
                }
                u8 src_byte = 0;
                if (s_bit >= d_bit) {
                    const u32 sb = first_src_byte + i;
                    const u32 shift = s_bit - d_bit;
                    src_byte = static_cast<u8>(bus.r8(src_row + sb) << shift);
                    if (shift != 0 && sb < last_src_byte) {
                        src_byte |= static_cast<u8>(bus.r8(src_row + sb + 1) >> (8 - shift));
                    }
                } else {
                    const u32 sb = first_src_byte + i;
                    const u32 shift = d_bit - s_bit;
                    if (i > 0) {
                        src_byte = static_cast<u8>(bus.r8(src_row + sb - 1) << (8 - shift));
                    }
                    if (sb <= last_src_byte) {
                        src_byte |= static_cast<u8>(bus.r8(src_row + sb) >> shift);
                    }
                }
                const u32 dst_at = dst_row + db;
                if (mask == 0xFF) {
                    bus.w8(dst_at, src_byte);
                } else {
                    const u8 dst_byte = bus.r8(dst_at);
                    bus.w8(dst_at, static_cast<u8>((dst_byte & ~mask) | (src_byte & mask)));
                }
            }
        }
    }
}

// Copies a rectangle between two distinct bitmaps, trimming up to top_rows
// top-left and top-right corner insets relative to [0, box_width - 1] in src.
inline void copy_rect_skip(native::Bus& bus, const Bitmap& src, i32 sx, i32 sy, const Bitmap& dst,
                           i32 dx, i32 dy, i32 width, i32 height, i32 box_width,
                           const u8* top_insets, u32 top_rows) {
    if (top_insets == nullptr || top_rows == 0 || box_width <= 0) {
        copy_rect(bus, src, sx, sy, dst, dx, dy, width, height);
        return;
    }
    i64 s_x = sx;
    i64 s_y = sy;
    i64 d_x = dx;
    i64 d_y = dy;
    i64 w = width;
    i64 h = height;
    if (!clip_copy_bounds(src, dst, s_x, s_y, d_x, d_y, w, h)) {
        return;
    }
    while (h > 0 && s_y < static_cast<i64>(top_rows) && bus.ok()) {
        const i64 inset = static_cast<i64>(top_insets[s_y]);
        const i64 row_x0 = s_x > inset ? s_x : inset;
        const i64 max_x1 = static_cast<i64>(box_width) - 1 - inset;
        const i64 s_x1 = s_x + w - 1;
        const i64 row_x1 = s_x1 < max_x1 ? s_x1 : max_x1;
        if (row_x0 <= row_x1) {
            copy_rect(bus, src, static_cast<i32>(row_x0), static_cast<i32>(s_y), dst,
                      static_cast<i32>(d_x + (row_x0 - s_x)), static_cast<i32>(d_y),
                      static_cast<i32>(row_x1 - row_x0 + 1), 1);
        }
        ++s_y;
        ++d_y;
        --h;
    }
    if (h > 0 && bus.ok()) {
        copy_rect(bus, src, static_cast<i32>(s_x), static_cast<i32>(s_y), dst,
                  static_cast<i32>(d_x), static_cast<i32>(d_y), static_cast<i32>(w),
                  static_cast<i32>(h));
    }
}

// Remaps each pixel inside r (clipped to bitmap) through table[pen] in place,
// processing one byte column at a time across the active bitplanes.
inline void remap_rect(native::Bus& bus, const Bitmap& bitmap, Rect r, const u8* table,
                       u32 table_size = 256) {
    if (table == nullptr || table_size == 0 || bitmap.depth == 0) {
        return;
    }
    const i64 width = static_cast<i64>(static_cast<u64>(bitmap.bytes_per_row) << 3);
    const i64 height = static_cast<i64>(bitmap.rows);
    i64 left = r.x0 > 0 ? r.x0 : 0;
    i64 top = r.y0 > 0 ? r.y0 : 0;
    i64 right = r.x1 < width ? r.x1 : width - 1;
    i64 bottom = r.y1 < height ? r.y1 : height - 1;
    if (left > right || top > bottom) {
        return;
    }
    const u32 depth = bitmap.depth < 8 ? bitmap.depth : 8;
    const u32 first_byte = static_cast<u32>(left) >> 3;
    const u32 last_byte = static_cast<u32>(right) >> 3;
    const u32 byte_count = last_byte - first_byte + 1;
    const u8 left_mask = static_cast<u8>(0xFFu >> (static_cast<u32>(left) & 7));
    const u8 right_mask = static_cast<u8>(0xFFu << (7 - (static_cast<u32>(right) & 7)));

    for (i64 y = top; y <= bottom && bus.ok(); ++y) {
        const u32 row_offset = static_cast<u32>(y) * bitmap.bytes_per_row + first_byte;
        u8* plane_ptrs[8]{};
        for (u32 plane = 0; plane < depth; ++plane) {
            plane_ptrs[plane] = bus.bytes_at(bitmap.planes[plane] + row_offset, byte_count);
        }
        if (!bus.ok()) {
            return;
        }
        for (u32 bi = 0; bi < byte_count; ++bi) {
            u8 mask = 0xFF;
            if (bi == 0) {
                mask &= left_mask;
            }
            if (bi == byte_count - 1) {
                mask &= right_mask;
            }
            u8 in_b[8]{};
            u8 out_b[8]{};
            for (u32 plane = 0; plane < depth; ++plane) {
                in_b[plane] = plane_ptrs[plane][bi];
                out_b[plane] = static_cast<u8>(in_b[plane] & ~mask);
            }
            for (u32 bit_i = 0; bit_i < 8; ++bit_i) {
                const u8 bit = static_cast<u8>(0x80u >> bit_i);
                if ((mask & bit) == 0) {
                    continue;
                }
                const u32 shift = 7 - bit_i;
                u32 pen = 0;
                for (u32 plane = 0; plane < depth; ++plane) {
                    pen |= static_cast<u32>((in_b[plane] >> shift) & 1u) << plane;
                }
                const u32 mapped = pen < table_size ? table[pen] : pen;
                for (u32 plane = 0; plane < depth; ++plane) {
                    if (((mapped >> plane) & 1u) != 0) {
                        out_b[plane] |= bit;
                    }
                }
            }
            for (u32 plane = 0; plane < depth; ++plane) {
                if (out_b[plane] != in_b[plane]) {
                    plane_ptrs[plane][bi] = out_b[plane];
                }
            }
        }
    }
}

}
