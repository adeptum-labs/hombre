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
#include "deep_bitmaps.hpp"
#include "gfx.hpp"
#include "native.hpp"

// Drawing on a planar Amiga BitMap. Every guest byte goes through
// native::Bus and every loop stays inside the bitmap and the clip, so a
// drawing call cannot touch guest memory outside the bitmap or run away.
//
// Plane p holds bit p of the pen of each pixel; its byte at row y is at
// planes[p] + y * bytes_per_row, and bit 0x80 >> (x & 7) is the pixel at x.

namespace hombre::raster {

// A planar BitMap, or with deep set a chunky one: pixels holds the address
// of its rows of width pixels in fmt, and palette the colours of its pens.
struct Bitmap {
    u32 planes[8];
    u32 bytes_per_row;
    u32 rows;
    u32 depth;
    u32 addr;
    bool deep = false;
    PixFmt fmt = PixFmt::Lut8;
    u32 width = 0;
    u32 pixels = 0;
    const u32* palette = nullptr;
};

// The deep bitmaps load_bitmap knows; graphics.library sets it.
inline const DeepBitmaps* g_deep_lookup = nullptr;

inline void set_deep_lookup(const DeepBitmaps* lookup) { g_deep_lookup = lookup; }

// The pixels across a row.
inline i64 pixel_width(const Bitmap& bitmap) {
    return bitmap.deep ? static_cast<i64>(bitmap.width)
                       : static_cast<i64>(static_cast<u64>(bitmap.bytes_per_row) << 3);
}

// An inclusive rectangle the drawing stays inside. With region not 0 the
// drawing also stays inside the rectangles of that Region, whose
// coordinates are relative to region_x, region_y.
struct Clip {
    i32 x0, y0, x1, y1;
    u32 region = 0;
    i32 region_x = 0;
    i32 region_y = 0;
};

// The pens and draw mode of one operation.
struct Pens {
    u8 fg, bg, mode;
};

// A RastPort's BitMap and the rectangle drawing through it stays inside,
// with the origin its coordinates are relative to.
struct RastPortView {
    Bitmap bitmap;
    Clip clip{0, 0, -1, -1};
    i32 origin_x = 0;
    i32 origin_y = 0;
    u32 bitmap_addr = 0;
    bool ok = false;
};

inline void write_pixel(native::Bus& bus, const Bitmap& bitmap, i32 x, i32 y, u32 pen);

namespace {

constexpr u32 kAllOnes = 0xFFFFFFFF;
constexpr u32 kMaxLineSteps = 4096;

bool in_bitmap(const Bitmap& bitmap, i32 x, i32 y) {
    return x >= 0 && y >= 0 && x < pixel_width(bitmap) && static_cast<u32>(y) < bitmap.rows;
}

bool visible(const Bitmap& bitmap, const Clip& clip, i64 x, i64 y) {
    if (x < clip.x0 || x > clip.x1 || y < clip.y0 || y > clip.y1) {
        return false;
    }
    return x >= 0 && y >= 0 && x < pixel_width(bitmap) && static_cast<u64>(y) < bitmap.rows;
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

Rect unite_rects(const Rect& a, const Rect& b) {
    if (b.x1 < b.x0 || b.y1 < b.y0) {
        return a;
    }
    if (a.x1 < a.x0 || a.y1 < a.y0) {
        return b;
    }
    return Rect{a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1,
                a.y1 > b.y1 ? a.y1 : b.y1};
}

// The most rectangles of a clip region a draw walks.
constexpr u32 kMaxClipRects = 256;

// Calls visit(part) for the clip itself, or with a region for the part of
// the clip inside each of the region's rectangles, until visit returns
// false.
template <typename Visit>
void walk_clip(native::Bus& bus, const Clip& clip, Visit visit) {
    if (clip.region == 0) {
        visit(clip);
        return;
    }
    using i16 = __INT16_TYPE__;
    const i32 base_x = clip.region_x + static_cast<i16>(bus.r16(clip.region + ndk::rg_bounds));
    const i32 base_y =
        clip.region_y + static_cast<i16>(bus.r16(clip.region + ndk::rg_bounds + 2));
    u32 rr = bus.r32(clip.region + ndk::rg_RegionRectangle);
    for (u32 i = 0; rr != 0 && i < kMaxClipRects && bus.ok(); ++i) {
        const i32 x0 = base_x + static_cast<i16>(bus.r16(rr + ndk::rr_bounds));
        const i32 y0 = base_y + static_cast<i16>(bus.r16(rr + ndk::rr_bounds + 2));
        const i32 x1 = base_x + static_cast<i16>(bus.r16(rr + ndk::rr_bounds + 4));
        const i32 y1 = base_y + static_cast<i16>(bus.r16(rr + ndk::rr_bounds + 6));
        rr = bus.r32(rr + ndk::rr_Next);
        if (!bus.ok()) {
            return;
        }
        const Clip part{x0 > clip.x0 ? x0 : clip.x0, y0 > clip.y0 ? y0 : clip.y0,
                        x1 < clip.x1 ? x1 : clip.x1, y1 < clip.y1 ? y1 : clip.y1};
        if (part.x0 <= part.x1 && part.y0 <= part.y1 && !visit(part)) {
            return;
        }
    }
}

// The bits of a plane byte from pixel x to the byte's right end, and from
// its left end to pixel x.
u8 left_mask(i64 x) {
    return static_cast<u8>(0xFFu >> (x & 7));
}

u8 right_mask(i64 x) {
    return static_cast<u8>(0xFFu << (7 - (x & 7)));
}

u8 merge(u8 old, u8 value, u8 mask) {
    return static_cast<u8>((old & ~mask) | (value & mask));
}

// Sets the masked bits of the span bytes to value; lmask applies to the
// first byte and rmask to the last, both to a span of one byte.
void fill_span(u8* row, u32 span, u8 lmask, u8 rmask, u8 value) {
    if (span == 1) {
        row[0] = merge(row[0], value, static_cast<u8>(lmask & rmask));
        return;
    }
    row[0] = merge(row[0], value, lmask);
    for (u32 i = 1; i + 1 < span; ++i) {
        row[i] = value;
    }
    row[span - 1] = merge(row[span - 1], value, rmask);
}

// Copies the masked bits of the span bytes from src, masked as in fill_span.
void copy_span(u8* dst, const u8* src, u32 span, u8 lmask, u8 rmask) {
    if (span == 1) {
        dst[0] = merge(dst[0], src[0], static_cast<u8>(lmask & rmask));
        return;
    }
    dst[0] = merge(dst[0], src[0], lmask);
    for (u32 i = 1; i + 1 < span; ++i) {
        dst[i] = src[i];
    }
    dst[span - 1] = merge(dst[span - 1], src[span - 1], rmask);
}

}

}

#include "raster_chunky.hpp"

namespace hombre::raster {

// The largest bitmap a draw works on: 4096 x 4096 pixels, which bounds the
// work one native call can do.
constexpr u32 kMaxBytesPerRow = 512;
constexpr u32 kMaxRows = 4096;

inline bool load_bitmap(native::Bus& bus, u32 bitmap_addr, Bitmap& out) {
    if (g_deep_lookup != nullptr) {
        if (const DeepBitmap* deep = g_deep_lookup->find(bitmap_addr)) {
            out = Bitmap{};
            out.deep = true;
            out.fmt = deep->fmt;
            out.width = deep->width;
            out.rows = deep->height;
            out.bytes_per_row = deep->bytes_per_row;
            out.depth = out.fmt == PixFmt::Lut8 ? 8 : out.fmt == PixFmt::Rgb16 ? 16 : 24;
            out.pixels = deep->pixels;
            out.palette = deep->palette;
            out.addr = bitmap_addr;
            return out.rows <= kMaxRows &&
                   u64{out.width} * bytes_per_pixel(out.fmt) <= out.bytes_per_row &&
                   bus.bytes_at(out.pixels, out.bytes_per_row * out.rows) != nullptr;
        }
    }
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

// Reads the RastPort's BitMap and, when it has a layer, the layer's clip: a
// layer over the bitmap itself clips to the layer's size, any other layer
// clips at its screen position. A region installed in the layer clips
// further, its coordinates relative to the layer's top left corner.
inline RastPortView load_rastport(native::Bus& bus, u32 rp) {
    RastPortView view{};
    if (rp == 0) {
        return view;
    }
    using i16 = __INT16_TYPE__;
    const u32 layer = bus.r32(rp + ndk::rp_Layer);
    const u32 bitmap = bus.r32(rp + ndk::rp_BitMap);
    if (bitmap == 0 || !load_bitmap(bus, bitmap, view.bitmap)) {
        return view;
    }
    view.bitmap_addr = bitmap;
    if (layer != 0) {
        const i32 min_x = static_cast<i16>(bus.r16(layer + ndk::ly_MinX));
        const i32 min_y = static_cast<i16>(bus.r16(layer + ndk::ly_MinY));
        const i32 max_x = static_cast<i16>(bus.r16(layer + ndk::ly_MaxX));
        const i32 max_y = static_cast<i16>(bus.r16(layer + ndk::ly_MaxY));
        if (bus.r32(layer + ndk::ly_SuperBitMap) == bitmap) {
            view.clip = Clip{0, 0, max_x - min_x, max_y - min_y};
        } else {
            view.origin_x = min_x;
            view.origin_y = min_y;
            view.clip = Clip{min_x, min_y, max_x, max_y};
        }
        view.clip.region = bus.r32(layer + ndk::ly_ClipRegion);
        view.clip.region_x = view.origin_x;
        view.clip.region_y = view.origin_y;
    } else {
        view.clip = Clip{0, 0, static_cast<i32>(pixel_width(view.bitmap)) - 1,
                         static_cast<i32>(view.bitmap.rows) - 1};
    }
    view.ok = bus.ok();
    return view;
}

inline u32 read_pixel(native::Bus& bus, const Bitmap& bitmap, i32 x, i32 y) {
    if (!in_bitmap(bitmap, x, y)) {
        return kAllOnes;
    }
    if (bitmap.deep) {
        return chunky::read_pen(bus, bitmap, static_cast<u32>(x), static_cast<u32>(y));
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
    if (bitmap.deep) {
        chunky::write_value(bus, bitmap, static_cast<u32>(x), static_cast<u32>(y),
                            chunky::pen_value(bitmap, pen));
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

// True for a pixel inside the clip and, when it has one, its region.
inline bool in_clip(native::Bus& bus, const Clip& clip, i32 x, i32 y) {
    bool inside = false;
    walk_clip(bus, clip, [&](const Clip& part) {
        inside = x >= part.x0 && x <= part.x1 && y >= part.y0 && y <= part.y1;
        return !inside;
    });
    return inside;
}

namespace {

Rect fill_rect_part(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, Rect r, u32 pen,
                    u8 plane_mask) {
    const i64 width = pixel_width(bitmap);
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
    if (bitmap.deep) {
        chunky::fill(bus, bitmap, left, top, right, bottom, chunky::pen_value(bitmap, pen),
                     plane_mask);
        return Rect{static_cast<i32>(left), static_cast<i32>(top), static_cast<i32>(right),
                    static_cast<i32>(bottom)};
    }
    // A plane byte at a time: whole bytes inside the span, masked at its ends.
    const u32 first = static_cast<u32>(left) >> 3;
    const u32 span = (static_cast<u32>(right) >> 3) - first + 1;
    const u8 lmask = left_mask(left);
    const u8 rmask = right_mask(right);
    for (u32 plane = 0; plane < bitmap.depth && plane < 8; ++plane) {
        if ((plane_mask & (1u << plane)) == 0) {
            continue;
        }
        const u8 value = ((pen >> plane) & 1) != 0 ? 0xFF : 0x00;
        for (i64 y = top; y <= bottom; ++y) {
            u8* row = bus.bytes_at(bitmap.planes[plane] +
                                       static_cast<u32>(y) * bitmap.bytes_per_row + first,
                                   span);
            if (row == nullptr) {
                break;
            }
            fill_span(row, span, lmask, rmask, value);
        }
    }
    return Rect{static_cast<i32>(left), static_cast<i32>(top), static_cast<i32>(right),
                static_cast<i32>(bottom)};
}

Rect draw_line_part(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, i32 x0, i32 y0,
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

Rect scroll_rect_part(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, i32 dx, i32 dy,
                      Rect area, u32 bg_pen) {
    const i64 width = pixel_width(bitmap);
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
    const Rect area_done{static_cast<i32>(left), static_cast<i32>(top), static_cast<i32>(right),
                         static_cast<i32>(bottom)};
    if (bitmap.deep) {
        chunky::scroll(bus, bitmap, left, top, right, bottom, dx, dy,
                       chunky::pen_value(bitmap, bg_pen));
        return area_done;
    }

    if (dx == 0) {
        // A vertical move keeps every pixel in its byte column: whole plane
        // rows move, masked at the ends of the span. Rows are walked in the
        // same order as below, so a source row is read before it is written.
        const u32 first = static_cast<u32>(left) >> 3;
        const u32 span = (static_cast<u32>(right) >> 3) - first + 1;
        const u8 lmask = left_mask(left);
        const u8 rmask = right_mask(right);
        for (u32 plane = 0; plane < bitmap.depth && plane < 8; ++plane) {
            const u8 fill = ((bg_pen >> plane) & 1) != 0 ? 0xFF : 0x00;
            for (i64 row = 0; row <= bottom - top; ++row) {
                const i64 y = dy > 0 ? top + row : bottom - row;
                const i64 source_y = y + dy;
                u8* out = bus.bytes_at(bitmap.planes[plane] +
                                           static_cast<u32>(y) * bitmap.bytes_per_row + first,
                                       span);
                if (out == nullptr) {
                    return area_done;
                }
                if (source_y >= top && source_y <= bottom) {
                    const u8* in = bus.bytes_at(
                        bitmap.planes[plane] +
                            static_cast<u32>(source_y) * bitmap.bytes_per_row + first,
                        span);
                    if (in == nullptr) {
                        return area_done;
                    }
                    copy_span(out, in, span, lmask, rmask);
                } else {
                    fill_span(out, span, lmask, rmask, fill);
                }
            }
        }
        return area_done;
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

}

// Each drawing call below draws inside the clip; where the clip has a
// region, inside every rectangle of the region in turn. The rectangle it
// returns holds every pixel it changed.

// fill_rect writes only the planes plane_mask has a bit for, as a
// RastPort's Mask limits drawing.
inline Rect fill_rect(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, Rect r, u32 pen,
                      u8 plane_mask = 0xFF) {
    Rect dirty = empty_rect();
    walk_clip(bus, clip, [&](const Clip& part) {
        dirty = unite_rects(dirty, fill_rect_part(bus, bitmap, part, r, pen, plane_mask));
        return true;
    });
    return dirty;
}

// Complements the rectangle: every pixel's pen has the bits of mask turned
// over, as a fill in the COMPLEMENT draw mode does.
inline Rect complement_rect(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, Rect r,
                            u32 mask) {
    Rect dirty = empty_rect();
    const i64 width = pixel_width(bitmap);
    const i64 height = static_cast<i64>(bitmap.rows);
    walk_clip(bus, clip, [&](const Clip& part) {
        const i64 left = r.x0 > part.x0 ? (r.x0 > 0 ? r.x0 : 0) : (part.x0 > 0 ? part.x0 : 0);
        const i64 top = r.y0 > part.y0 ? (r.y0 > 0 ? r.y0 : 0) : (part.y0 > 0 ? part.y0 : 0);
        i64 right = r.x1 < part.x1 ? r.x1 : part.x1;
        i64 bottom = r.y1 < part.y1 ? r.y1 : part.y1;
        right = right < width ? right : width - 1;
        bottom = bottom < height ? bottom : height - 1;
        if (bitmap.deep) {
            if (left <= right && top <= bottom) {
                chunky::complement(bus, bitmap, left, top, right, bottom, mask);
                dirty = unite_rects(dirty, Rect{static_cast<i32>(left), static_cast<i32>(top),
                                                static_cast<i32>(right),
                                                static_cast<i32>(bottom)});
            }
            return true;
        }
        for (i64 y = top; y <= bottom && bus.ok(); ++y) {
            for (i64 x = left; x <= right; ++x) {
                const i32 px = static_cast<i32>(x);
                const i32 py = static_cast<i32>(y);
                write_pixel(bus, bitmap, px, py, read_pixel(bus, bitmap, px, py) ^ mask);
                grow(dirty, px, py);
            }
        }
        return true;
    });
    return dirty;
}

inline Rect draw_line(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, i32 x0, i32 y0,
                      i32 x1, i32 y1, u32 pen) {
    Rect dirty = empty_rect();
    walk_clip(bus, clip, [&](const Clip& part) {
        dirty = unite_rects(dirty, draw_line_part(bus, bitmap, part, x0, y0, x1, y1, pen));
        return true;
    });
    return dirty;
}

// Each rectangle of a region scrolls on its own: what would come from
// outside it is filled with bg_pen.
inline Rect scroll_rect(native::Bus& bus, const Bitmap& bitmap, const Clip& clip, i32 dx, i32 dy,
                        Rect area, u32 bg_pen) {
    Rect dirty = empty_rect();
    walk_clip(bus, clip, [&](const Clip& part) {
        dirty = unite_rects(dirty, scroll_rect_part(bus, bitmap, part, dx, dy, area, bg_pen));
        return true;
    });
    return dirty;
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
    const i64 src_w = pixel_width(src);
    const i64 dst_w = pixel_width(dst);
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

// Copies a clipped rectangle where either bitmap is deep. Rows of the same
// format move whole, in the order that reads an overlapping source row before
// it is written; other pairs go pixel by pixel through colour or pen.
inline void copy_deep(native::Bus& bus, const Bitmap& src, u32 sx, u32 sy, const Bitmap& dst,
                      u32 dx, u32 dy, u32 w, u32 h) {
    const bool same_format = src.deep && dst.deep && src.fmt == dst.fmt;
    const bool downward = src.pixels == dst.pixels && dy > sy;
    for (u32 i = 0; i < h && bus.ok(); ++i) {
        const u32 row_index = downward ? h - 1 - i : i;
        const u32 y_in = sy + row_index;
        const u32 y_out = dy + row_index;
        if (same_format) {
            const u8* in = chunky::row(bus, src, sx, y_in, w);
            u8* out = chunky::row(bus, dst, dx, y_out, w);
            if (in == nullptr || out == nullptr) {
                return;
            }
            __builtin_memmove(out, in, w * chunky::bpp(dst));
            continue;
        }
        for (u32 x = 0; x < w; ++x) {
            const i32 xi = static_cast<i32>(sx + x);
            const i32 xo = static_cast<i32>(dx + x);
            if (!dst.deep) {
                write_pixel(bus, dst, xo, static_cast<i32>(y_out),
                            read_pixel(bus, src, xi, static_cast<i32>(y_in)));
            } else if (!src.deep) {
                chunky::write_value(bus, dst, dx + x, y_out,
                                    chunky::pen_value(dst, read_pixel(bus, src, xi,
                                                                      static_cast<i32>(y_in))));
            } else {
                const u32 xrgb = chunky::value_rgb(src, chunky::read_value(bus, src, sx + x, y_in));
                chunky::write_value(bus, dst, dx + x, y_out, chunky::rgb_value(dst, xrgb));
            }
        }
    }
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
    if (src.deep || dst.deep) {
        copy_deep(bus, src, static_cast<u32>(s_x), static_cast<u32>(s_y), dst,
                  static_cast<u32>(d_x), static_cast<u32>(d_y), static_cast<u32>(w),
                  static_cast<u32>(h));
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

    const u32 src_count = last_src_byte - first_src_byte + 1;

    for (u32 plane = 0; plane < depth; ++plane) {
        for (u32 row = 0; row < uh; ++row) {
            const u8* in = bus.bytes_at(
                src.planes[plane] + (uy + row) * src.bytes_per_row + first_src_byte, src_count);
            u8* out = bus.bytes_at(
                dst.planes[plane] + (vy + row) * dst.bytes_per_row + first_byte, byte_count);
            if (in == nullptr || out == nullptr) {
                return;
            }
            if (s_bit == d_bit) {
                copy_span(out, in, byte_count, left_mask, right_mask);
                continue;
            }
            for (u32 i = 0; i < byte_count; ++i) {
                u8 mask = 0xFF;
                if (i == 0) {
                    mask &= left_mask;
                }
                if (i + 1 == byte_count) {
                    mask &= right_mask;
                }
                u8 src_byte = 0;
                if (s_bit > d_bit) {
                    const u32 shift = s_bit - d_bit;
                    src_byte = static_cast<u8>(in[i] << shift);
                    if (i + 1 < src_count) {
                        src_byte |= static_cast<u8>(in[i + 1] >> (8 - shift));
                    }
                } else {
                    const u32 shift = d_bit - s_bit;
                    if (i > 0) {
                        src_byte = static_cast<u8>(in[i - 1] << (8 - shift));
                    }
                    if (i < src_count) {
                        src_byte |= static_cast<u8>(in[i] >> shift);
                    }
                }
                out[i] = merge(out[i], src_byte, mask);
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
    const i64 width = pixel_width(bitmap);
    if (bitmap.deep) {
        // Pens are only kept on LUT8; an RGB pixel has no pen to remap.
        if (bitmap.fmt != PixFmt::Lut8) {
            return;
        }
        const i64 l = r.x0 > 0 ? r.x0 : 0;
        const i64 t = r.y0 > 0 ? r.y0 : 0;
        const i64 rr = r.x1 < width ? r.x1 : width - 1;
        const i64 bb = r.y1 < static_cast<i64>(bitmap.rows) ? r.y1 : bitmap.rows - 1;
        for (i64 y = t; y <= bb && l <= rr; ++y) {
            u8* at = chunky::row(bus, bitmap, static_cast<u32>(l), static_cast<u32>(y),
                                 static_cast<u32>(rr - l + 1));
            if (at == nullptr) {
                return;
            }
            for (i64 x = 0; x <= rr - l; ++x) {
                if (at[x] < table_size) {
                    at[x] = table[at[x]];
                }
            }
        }
        return;
    }
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
