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

#include "cybergraphics.hpp"

#include "display_modes.hpp"
#include "graphics.hpp"
#include "raster.hpp"
#include "tags.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;

namespace {

// The low word of a register: CyberGraphX takes coordinates, sizes, row
// bytes and formats as UWORDs, and a caller may leave the upper word set.
u32 word(u32 value) { return value & 0xFFFF; }

using namespace cyber;
using namespace ndk;

namespace chunky = raster::chunky;

constexpr u32 kAllOnes = 0xFFFFFFFF;

u32 pixfmt_value(PixFmt fmt) {
    switch (fmt) {
    case PixFmt::Lut8:
        return PIXFMT_LUT8;
    case PixFmt::Rgb16:
        return PIXFMT_RGB16;
    case PixFmt::Argb32:
        return PIXFMT_ARGB32;
    }
    return PIXFMT_LUT8;
}

u32 depth_of(PixFmt fmt) {
    return fmt == PixFmt::Lut8 ? 8 : fmt == PixFmt::Rgb16 ? 16 : 24;
}

// CyberGraphX asks for 8, 15 or 16 and 24 or 32 bits; Hombre's deep modes
// have 8, 16 and 24.
u32 wanted_depth(u32 depth) {
    return depth >= 24 ? 24 : depth >= 15 ? 16 : 8;
}

u32 mode_area(const display_modes::Mode& mode) {
    return static_cast<u32>(mode.width) * mode.height;
}

struct Area {
    i32 x0 = 0;
    i32 y0 = 0;
    i32 x1 = -1;
    i32 y1 = -1;
};

// value - origin, well defined even when the two lie at opposite ends of
// the signed range.
u64 offset_of(i32 value, i32 origin) {
    return static_cast<u64>(static_cast<i64>(value) - origin);
}

// The part of [x, x + width) x [y, y + height) inside the RastPort's clip and
// its bitmap, in bitmap coordinates.
Area visible_area(const raster::RastPortView& v, i32 x, i32 y, u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return Area{};
    }
    const i64 bitmap_width = raster::pixel_width(v.bitmap);
    const i64 bitmap_height = v.bitmap.rows;
    i64 left = x > v.clip.x0 ? x : v.clip.x0;
    i64 top = y > v.clip.y0 ? y : v.clip.y0;
    i64 right = static_cast<i64>(x) + width - 1;
    i64 bottom = static_cast<i64>(y) + height - 1;
    if (right > v.clip.x1) {
        right = v.clip.x1;
    }
    if (bottom > v.clip.y1) {
        bottom = v.clip.y1;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right >= bitmap_width) {
        right = bitmap_width - 1;
    }
    if (bottom >= bitmap_height) {
        bottom = bitmap_height - 1;
    }
    if (left > right || top > bottom) {
        return Area{};
    }
    return Area{static_cast<i32>(left), static_cast<i32>(top), static_cast<i32>(right),
                static_cast<i32>(bottom)};
}

bool inside(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip, i32 x, i32 y) {
    return x >= 0 && y >= 0 && x < raster::pixel_width(bitmap) &&
           static_cast<u32>(y) < bitmap.rows && raster::in_clip(b, clip, x, y);
}

void put_xrgb(Bus& b, const raster::Bitmap& bitmap, i32 x, i32 y, u32 xrgb) {
    if (bitmap.deep) {
        chunky::write_value(b, bitmap, static_cast<u32>(x), static_cast<u32>(y),
                            chunky::rgb_value(bitmap, xrgb));
        return;
    }
    raster::write_pixel(b, bitmap, x, y,
                        chunky::nearest_pen(chunky::default_palette(),
                                            static_cast<u8>(xrgb >> 16),
                                            static_cast<u8>(xrgb >> 8),
                                            static_cast<u8>(xrgb)));
}

void copy_pixel(Bus& b, const raster::Bitmap& bitmap, i32 source_x, i32 source_y, i32 dest_x,
                i32 dest_y) {
    if (bitmap.deep) {
        chunky::write_value(b, bitmap, static_cast<u32>(dest_x), static_cast<u32>(dest_y),
                            chunky::read_value(b, bitmap, static_cast<u32>(source_x),
                                               static_cast<u32>(source_y)));
        return;
    }
    raster::write_pixel(b, bitmap, dest_x, dest_y,
                        raster::read_pixel(b, bitmap, source_x, source_y));
}

struct Pixel {
    u32 value = 0;
    u32 pen = 0;
    u32 xrgb = 0;
};

Pixel read_pixel(Bus& b, const raster::Bitmap& bitmap, i32 x, i32 y, bool want_pen) {
    Pixel pixel;
    if (!bitmap.deep) {
        pixel.pen = raster::read_pixel(b, bitmap, x, y);
        pixel.xrgb = chunky::default_palette()[pixel.pen & 0xFF] & 0xFFFFFF;
        return pixel;
    }
    pixel.value = chunky::read_value(b, bitmap, static_cast<u32>(x), static_cast<u32>(y));
    pixel.xrgb = chunky::value_rgb(bitmap, pixel.value);
    if (want_pen) {
        pixel.pen = chunky::read_pen(b, bitmap, static_cast<u32>(x), static_cast<u32>(y));
    }
    return pixel;
}

u8 grey_of(u8 r, u8 g, u8 b) {
    return static_cast<u8>((u32{r} + g + b) / 3);
}

u32 array_bytes(u32 format, const raster::Bitmap& bitmap) {
    switch (format) {
    case RECTFMT_RGB:
        return 3;
    case RECTFMT_RGBA:
    case RECTFMT_ARGB:
        return 4;
    case RECTFMT_LUT8:
    case RECTFMT_GREY8:
        return 1;
    case RECTFMT_RAW:
        return bitmap.deep ? bytes_per_pixel(bitmap.fmt) : 0;
    }
    return 0;
}

u8* array_row(Bus& b, u32 base, u32 modulo, u64 y, u64 x_bytes, u64 bytes) {
    if (y > 0xFFFFFFFFu || x_bytes > 0xFFFFFFFFu || bytes > 0xFFFFFFFFu) {
        return nullptr;
    }
    const u64 offset = y * modulo;
    if (offset > 0xFFFFFFFFu) {
        return nullptr;
    }
    const u64 at = u64{base} + offset + x_bytes;
    if (at > 0xFFFFFFFFu) {
        return nullptr;
    }
    return b.bytes_at(static_cast<u32>(at), static_cast<u32>(bytes));
}

void write_array_pixel(Bus& b, const raster::Bitmap& bitmap, i32 x, i32 y, const u8* at,
                       u32 format) {
    switch (format) {
    case RECTFMT_RGB:
    case RECTFMT_RGBA:
        put_xrgb(b, bitmap, x, y, (u32{at[0]} << 16) | (u32{at[1]} << 8) | at[2]);
        break;
    case RECTFMT_ARGB:
        put_xrgb(b, bitmap, x, y, (u32{at[1]} << 16) | (u32{at[2]} << 8) | at[3]);
        break;
    case RECTFMT_LUT8:
        raster::write_pixel(b, bitmap, x, y, at[0]);
        break;
    case RECTFMT_GREY8:
        put_xrgb(b, bitmap, x, y, u32{at[0]} * 0x010101u);
        break;
    case RECTFMT_RAW:
        chunky::write_value(b, bitmap, static_cast<u32>(x), static_cast<u32>(y),
                            load_pixel(bitmap.fmt, at));
        break;
    }
}

void store_array_pixel(u8* at, u32 format, const Pixel& pixel, const raster::Bitmap& bitmap) {
    const u8 r = static_cast<u8>(pixel.xrgb >> 16);
    const u8 g = static_cast<u8>(pixel.xrgb >> 8);
    const u8 b = static_cast<u8>(pixel.xrgb);
    switch (format) {
    case RECTFMT_RGB:
        at[0] = r;
        at[1] = g;
        at[2] = b;
        break;
    case RECTFMT_RGBA:
        at[0] = r;
        at[1] = g;
        at[2] = b;
        at[3] = 0;
        break;
    case RECTFMT_ARGB:
        at[0] = 0;
        at[1] = r;
        at[2] = g;
        at[3] = b;
        break;
    case RECTFMT_LUT8:
        at[0] = static_cast<u8>(pixel.pen);
        break;
    case RECTFMT_GREY8:
        at[0] = grey_of(r, g, b);
        break;
    case RECTFMT_RAW:
        store_pixel(bitmap.fmt, at, pixel.value);
        break;
    }
}

}

struct CyberGraphicsNatives {
    static CyberGraphics& of(void* ctx) { return *static_cast<CyberGraphics*>(ctx); }

    static void is_cyber_mode_id(void*, Machine& m) {
        const u32 id = m.cpu.d[0];
        const bool known = display_modes::is_hombre(id) && display_modes::find(id) != nullptr;
        native::set_result(m, known ? 1 : 0);
    }

    static void best_c_mode_id_tag_list(void*, Machine& m) {
        Bus b(m);
        const u32 list = m.cpu.a[0];
        const u32 depth = tags::data(b, CYBRBIDTG_Depth, list, 8);
        const u32 nominal_width = tags::data(b, CYBRBIDTG_NominalWidth, list, 640);
        const u32 nominal_height = tags::data(b, CYBRBIDTG_NominalHeight, list, 480);
        const u32 wanted = wanted_depth(depth);
        const display_modes::Mode* smallest = nullptr;
        const display_modes::Mode* largest = nullptr;
        for (u32 i = 0; i < display_modes::count(); ++i) {
            const display_modes::Mode& mode = display_modes::at(i);
            if (!mode.deep || static_cast<u32>(mode.max_depth) != wanted) {
                continue;
            }
            if (largest == nullptr || mode_area(mode) > mode_area(*largest)) {
                largest = &mode;
            }
            if (static_cast<u32>(mode.width) >= nominal_width &&
                static_cast<u32>(mode.height) >= nominal_height &&
                (smallest == nullptr || mode_area(mode) < mode_area(*smallest))) {
                smallest = &mode;
            }
        }
        const display_modes::Mode* best = smallest != nullptr ? smallest : largest;
        native::set_result(m, best != nullptr ? best->id : INVALID_ID);
    }

    static void c_mode_request_tag_list(void*, Machine& m) { native::set_result(m, 0); }

    static void alloc_c_mode_list_tag_list(void*, Machine& m) { native::set_result(m, 0); }

    static void free_c_mode_list(void*, Machine& m) { native::set_result(m, 0); }

    static void get_cyber_map_attr(void* ctx, Machine& m) {
        const DeepBitmap* deep = of(ctx).graphics_->deep(m.cpu.a[0]);
        Bus b(m);
        const u32 attr = m.cpu.d[0];
        u32 value = 0;
        if (deep != nullptr) {
            switch (attr) {
            case CYBRMATTR_XMOD:
                value = deep->bytes_per_row;
                break;
            case CYBRMATTR_BPPIX:
                value = bytes_per_pixel(deep->fmt);
                break;
            case CYBRMATTR_PIXFMT:
                value = pixfmt_value(deep->fmt);
                break;
            case CYBRMATTR_WIDTH:
                value = deep->width;
                break;
            case CYBRMATTR_HEIGHT:
                value = deep->height;
                break;
            case CYBRMATTR_DEPTH:
                value = depth_of(deep->fmt);
                break;
            case CYBRMATTR_ISCYBERGFX:
            case CYBRMATTR_ISLINEARMEM:
                value = kAllOnes;
                break;
            case CYBRMATTR_COLORMAP:
                value = deep->colormap;
                break;
            default:
                break;
            }
        } else if (m.cpu.a[0] != 0) {
            switch (attr) {
            case CYBRMATTR_DEPTH:
                value = b.r8(m.cpu.a[0] + bm_Depth);
                break;
            case CYBRMATTR_WIDTH:
                value = b.r16(m.cpu.a[0] + bm_BytesPerRow) * 8;
                break;
            case CYBRMATTR_HEIGHT:
                value = b.r16(m.cpu.a[0] + bm_Rows);
                break;
            default:
                break;
            }
        }
        native::set_result(m, value);
    }

    static void get_cyber_id_attr(void*, Machine& m) {
        const u32 attr = m.cpu.d[0];
        const u32 id = m.cpu.d[1];
        const display_modes::Mode* mode =
            display_modes::is_hombre(id) ? display_modes::find(id) : nullptr;
        u32 value = 0;
        if (mode != nullptr) {
            switch (attr) {
            case CYBRIDATTR_PIXFMT:
                value = pixfmt_value(mode->pixfmt);
                break;
            case CYBRIDATTR_WIDTH:
                value = mode->width;
                break;
            case CYBRIDATTR_HEIGHT:
                value = mode->height;
                break;
            case CYBRIDATTR_DEPTH:
                value = mode->max_depth;
                break;
            case CYBRIDATTR_BPPIX:
                value = bytes_per_pixel(mode->pixfmt);
                break;
            default:
                break;
            }
        }
        native::set_result(m, value);
    }

    static void read_rgb_pixel(void*, Machine& m) {
        Bus b(m);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const i32 x = static_cast<i32>(word(m.cpu.d[0])) + v.origin_x;
        const i32 y = static_cast<i32>(word(m.cpu.d[1])) + v.origin_y;
        if (!v.ok || !inside(b, v.bitmap, v.clip, x, y)) {
            native::set_result(m, kAllOnes);
            return;
        }
        native::set_result(m, read_pixel(b, v.bitmap, x, y, false).xrgb & 0xFFFFFF);
    }

    static void write_rgb_pixel(void* ctx, Machine& m) {
        CyberGraphics& c = of(ctx);
        Bus b(m);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const i32 x = static_cast<i32>(word(m.cpu.d[0])) + v.origin_x;
        const i32 y = static_cast<i32>(word(m.cpu.d[1])) + v.origin_y;
        if (v.ok && v.bitmap.deep && inside(b, v.bitmap, v.clip, x, y)) {
            chunky::write_value(b, v.bitmap, static_cast<u32>(x), static_cast<u32>(y),
                                chunky::rgb_value(v.bitmap, m.cpu.d[2]));
            if (b.ok()) {
                c.graphics_->report_bitmap(v.bitmap_addr, Rect{x, y, x, y});
            }
        }
        native::set_result(m, 0);
    }

    static void write_pixel_array(void* ctx, Machine& m) {
        CyberGraphics& c = of(ctx);
        Bus b(m);
        const u32 source = m.cpu.a[0];
        const u32 source_x = word(m.cpu.d[0]);
        const u32 source_y = word(m.cpu.d[1]);
        const u32 modulo = word(m.cpu.d[2]);
        const u32 size_x = word(m.cpu.d[5]);
        const u32 size_y = word(m.cpu.d[6]);
        const u32 format = word(m.cpu.d[7]);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const u32 bpp = array_bytes(format, v.bitmap);
        const i32 dest_x = static_cast<i32>(word(m.cpu.d[3])) + v.origin_x;
        const i32 dest_y = static_cast<i32>(word(m.cpu.d[4])) + v.origin_y;
        if (source == 0 || !v.ok || bpp == 0) {
            native::set_result(m, 0);
            return;
        }
        const Area area = visible_area(v, dest_x, dest_y, size_x, size_y);
        u32 done = 0;
        for (i32 y = area.y0; y <= area.y1 && b.ok(); ++y) {
            const u8* row = array_row(
                b, source, modulo, static_cast<u64>(source_y) + offset_of(y, dest_y),
                (static_cast<u64>(source_x) + offset_of(area.x0, dest_x)) * bpp,
                static_cast<u64>(area.x1 - area.x0 + 1) * bpp);
            if (row == nullptr) {
                break;
            }
            for (i32 x = area.x0; x <= area.x1; ++x) {
                if (!raster::in_clip(b, v.clip, x, y)) {
                    continue;
                }
                write_array_pixel(b, v.bitmap, x, y,
                                  row + static_cast<u64>(x - area.x0) * bpp, format);
                ++done;
            }
        }
        if (done != 0 && b.ok()) {
            c.graphics_->report_bitmap(v.bitmap_addr,
                                       Rect{area.x0, area.y0, area.x1, area.y1});
        }
        native::set_result(m, done);
    }

    static void read_pixel_array(void*, Machine& m) {
        Bus b(m);
        const u32 dest = m.cpu.a[0];
        const u32 dest_x = word(m.cpu.d[0]);
        const u32 dest_y = word(m.cpu.d[1]);
        const u32 modulo = word(m.cpu.d[2]);
        const u32 size_x = word(m.cpu.d[5]);
        const u32 size_y = word(m.cpu.d[6]);
        const u32 format = word(m.cpu.d[7]);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const u32 bpp = array_bytes(format, v.bitmap);
        const i32 source_x = static_cast<i32>(word(m.cpu.d[3])) + v.origin_x;
        const i32 source_y = static_cast<i32>(word(m.cpu.d[4])) + v.origin_y;
        if (dest == 0 || !v.ok || bpp == 0) {
            native::set_result(m, 0);
            return;
        }
        const Area area = visible_area(v, source_x, source_y, size_x, size_y);
        const bool want_pen = format == RECTFMT_LUT8;
        u32 done = 0;
        for (i32 y = area.y0; y <= area.y1 && b.ok(); ++y) {
            u8* row = array_row(
                b, dest, modulo, static_cast<u64>(dest_y) + offset_of(y, source_y),
                (static_cast<u64>(dest_x) + offset_of(area.x0, source_x)) * bpp,
                static_cast<u64>(area.x1 - area.x0 + 1) * bpp);
            if (row == nullptr) {
                break;
            }
            for (i32 x = area.x0; x <= area.x1; ++x) {
                if (!raster::in_clip(b, v.clip, x, y)) {
                    continue;
                }
                store_array_pixel(row + static_cast<u64>(x - area.x0) * bpp, format,
                                  read_pixel(b, v.bitmap, x, y, want_pen), v.bitmap);
                ++done;
            }
        }
        native::set_result(m, done);
    }

    static void write_lut_pixel_array(void* ctx, Machine& m) {
        CyberGraphics& c = of(ctx);
        Bus b(m);
        const u32 source = m.cpu.a[0];
        const u32 source_x = word(m.cpu.d[0]);
        const u32 source_y = word(m.cpu.d[1]);
        const u32 modulo = word(m.cpu.d[2]);
        const u32 table = m.cpu.a[2];
        const u32 size_x = word(m.cpu.d[5]);
        const u32 size_y = word(m.cpu.d[6]);
        const u32 format = word(m.cpu.d[7]);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const i32 dest_x = static_cast<i32>(word(m.cpu.d[3])) + v.origin_x;
        const i32 dest_y = static_cast<i32>(word(m.cpu.d[4])) + v.origin_y;
        if (source == 0 || table == 0 || format != CTABFMT_XRGB8 || !v.ok) {
            native::set_result(m, 0);
            return;
        }
        const Area area = visible_area(v, dest_x, dest_y, size_x, size_y);
        u32 done = 0;
        for (i32 y = area.y0; y <= area.y1 && b.ok(); ++y) {
            const u8* row = array_row(b, source, modulo,
                                      static_cast<u64>(source_y) + offset_of(y, dest_y),
                                      static_cast<u64>(source_x) + offset_of(area.x0, dest_x),
                                      static_cast<u64>(area.x1 - area.x0 + 1));
            if (row == nullptr) {
                break;
            }
            for (i32 x = area.x0; x <= area.x1; ++x) {
                if (!raster::in_clip(b, v.clip, x, y)) {
                    continue;
                }
                const u32 index = row[static_cast<u32>(x - area.x0)];
                put_xrgb(b, v.bitmap, x, y, b.r32(table + index * 4));
                ++done;
            }
        }
        if (done != 0 && b.ok()) {
            c.graphics_->report_bitmap(v.bitmap_addr,
                                       Rect{area.x0, area.y0, area.x1, area.y1});
        }
        native::set_result(m, done);
    }

    static void scale_pixel_array(void* ctx, Machine& m) {
        CyberGraphics& c = of(ctx);
        Bus b(m);
        const u32 source = m.cpu.a[0];
        const u32 source_w = word(m.cpu.d[0]);
        const u32 source_h = word(m.cpu.d[1]);
        const u32 modulo = word(m.cpu.d[2]);
        const u32 dest_w = word(m.cpu.d[5]);
        const u32 dest_h = word(m.cpu.d[6]);
        const u32 format = word(m.cpu.d[7]);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const u32 bpp = array_bytes(format, v.bitmap);
        const i32 dest_x = static_cast<i32>(word(m.cpu.d[3])) + v.origin_x;
        const i32 dest_y = static_cast<i32>(word(m.cpu.d[4])) + v.origin_y;
        if (source == 0 || !v.ok || bpp == 0 || source_w == 0 || source_h == 0 ||
            dest_w == 0 || dest_h == 0) {
            native::set_result(m, 0);
            return;
        }
        const Area area = visible_area(v, dest_x, dest_y, dest_w, dest_h);
        u32 done = 0;
        bool stopped = false;
        for (i32 y = area.y0; y <= area.y1 && !stopped; ++y) {
            const u64 source_row = offset_of(y, dest_y) * source_h / dest_h;
            for (i32 x = area.x0; x <= area.x1; ++x) {
                if (!raster::in_clip(b, v.clip, x, y)) {
                    continue;
                }
                const u64 source_column = offset_of(x, dest_x) * source_w / dest_w;
                const u8* at = array_row(b, source, modulo, source_row,
                                         source_column * bpp, bpp);
                if (at == nullptr) {
                    stopped = true;
                    break;
                }
                write_array_pixel(b, v.bitmap, x, y, at, format);
                ++done;
            }
        }
        if (done != 0 && b.ok()) {
            c.graphics_->report_bitmap(v.bitmap_addr,
                                       Rect{area.x0, area.y0, area.x1, area.y1});
        }
        native::set_result(m, done);
    }

    static void move_pixel_array(void* ctx, Machine& m) {
        CyberGraphics& c = of(ctx);
        Bus b(m);
        const u32 size_x = word(m.cpu.d[4]);
        const u32 size_y = word(m.cpu.d[5]);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const i32 source_x = static_cast<i32>(word(m.cpu.d[0])) + v.origin_x;
        const i32 source_y = static_cast<i32>(word(m.cpu.d[1])) + v.origin_y;
        const i32 dest_x = static_cast<i32>(word(m.cpu.d[2])) + v.origin_x;
        const i32 dest_y = static_cast<i32>(word(m.cpu.d[3])) + v.origin_y;
        if (!v.ok) {
            native::set_result(m, 0);
            return;
        }
        const Area area = visible_area(v, dest_x, dest_y, size_x, size_y);
        const i64 dx = static_cast<i64>(dest_x) - source_x;
        const i64 dy = static_cast<i64>(dest_y) - source_y;
        const bool downward = dy > 0;
        const bool rightward = dy == 0 && dx > 0;
        u32 done = 0;
        for (i32 row = 0; row <= area.y1 - area.y0 && b.ok(); ++row) {
            const i32 y = downward ? area.y1 - row : area.y0 + row;
            const i64 source_row = static_cast<i64>(y) - dy;
            if (source_row < 0 || source_row >= static_cast<i64>(v.bitmap.rows)) {
                continue;
            }
            for (i32 column = 0; column <= area.x1 - area.x0; ++column) {
                const i32 x = rightward ? area.x1 - column : area.x0 + column;
                if (!raster::in_clip(b, v.clip, x, y)) {
                    continue;
                }
                const i64 column_in = static_cast<i64>(x) - dx;
                if (column_in < 0 || column_in >= raster::pixel_width(v.bitmap)) {
                    continue;
                }
                copy_pixel(b, v.bitmap, static_cast<i32>(column_in),
                           static_cast<i32>(source_row), x, y);
                ++done;
            }
        }
        if (done != 0 && b.ok()) {
            c.graphics_->report_bitmap(v.bitmap_addr,
                                       Rect{area.x0, area.y0, area.x1, area.y1});
        }
        native::set_result(m, done);
    }

    static void invert_pixel_array(void* ctx, Machine& m) {
        CyberGraphics& c = of(ctx);
        Bus b(m);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const i32 x0 = static_cast<i32>(word(m.cpu.d[0])) + v.origin_x;
        const i32 y0 = static_cast<i32>(word(m.cpu.d[1])) + v.origin_y;
        const u32 width = word(m.cpu.d[2]);
        const u32 height = word(m.cpu.d[3]);
        if (!v.ok) {
            native::set_result(m, 0);
            return;
        }
        const Area area = visible_area(v, x0, y0, width, height);
        const u32 flip = v.bitmap.deep
                             ? v.bitmap.fmt == PixFmt::Lut8    ? 0xFFu
                               : v.bitmap.fmt == PixFmt::Rgb16 ? 0xFFFFu
                                                               : 0xFFFFFFu
                             : 0xFFu;
        u32 done = 0;
        for (i32 y = area.y0; y <= area.y1; ++y) {
            for (i32 x = area.x0; x <= area.x1; ++x) {
                if (!raster::in_clip(b, v.clip, x, y)) {
                    continue;
                }
                if (v.bitmap.deep) {
                    chunky::write_value(b, v.bitmap, static_cast<u32>(x),
                                        static_cast<u32>(y),
                                        chunky::read_value(b, v.bitmap, static_cast<u32>(x),
                                                           static_cast<u32>(y)) ^ flip);
                } else {
                    raster::write_pixel(b, v.bitmap, x, y,
                                        raster::read_pixel(b, v.bitmap, x, y) ^ 0xFF);
                }
                ++done;
            }
        }
        if (done != 0 && b.ok()) {
            c.graphics_->report_bitmap(v.bitmap_addr,
                                       Rect{area.x0, area.y0, area.x1, area.y1});
        }
        native::set_result(m, done);
    }

    static void fill_pixel_array(void* ctx, Machine& m) {
        CyberGraphics& c = of(ctx);
        Bus b(m);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[1]);
        const i32 x0 = static_cast<i32>(word(m.cpu.d[0])) + v.origin_x;
        const i32 y0 = static_cast<i32>(word(m.cpu.d[1])) + v.origin_y;
        const u32 width = word(m.cpu.d[2]);
        const u32 height = word(m.cpu.d[3]);
        const u32 argb = m.cpu.d[4];
        if (!v.ok) {
            native::set_result(m, 0);
            return;
        }
        const Area area = visible_area(v, x0, y0, width, height);
        const u32 value = v.bitmap.deep ? chunky::rgb_value(v.bitmap, argb) : 0;
        const u32 pen = v.bitmap.deep
                            ? 0
                            : chunky::nearest_pen(chunky::default_palette(),
                                                  static_cast<u8>(argb >> 16),
                                                  static_cast<u8>(argb >> 8),
                                                  static_cast<u8>(argb));
        u32 done = 0;
        for (i32 y = area.y0; y <= area.y1; ++y) {
            for (i32 x = area.x0; x <= area.x1; ++x) {
                if (!raster::in_clip(b, v.clip, x, y)) {
                    continue;
                }
                if (v.bitmap.deep) {
                    chunky::write_value(b, v.bitmap, static_cast<u32>(x),
                                        static_cast<u32>(y), value);
                } else {
                    raster::write_pixel(b, v.bitmap, x, y, pen);
                }
                ++done;
            }
        }
        if (done != 0 && b.ok()) {
            c.graphics_->report_bitmap(v.bitmap_addr,
                                       Rect{area.x0, area.y0, area.x1, area.y1});
        }
        native::set_result(m, done);
    }

    static void do_c_draw_method_tag_list(void*, Machine& m) { native::set_result(m, 0); }

    static void c_video_ctrl_tag_list(void*, Machine& m) { native::set_result(m, 0); }

    static void lock_bit_map_tag_list(void* ctx, Machine& m) {
        Bus b(m);
        const u32 bitmap = m.cpu.a[0];
        const DeepBitmap* deep = of(ctx).graphics_->deep(bitmap);
        if (deep == nullptr) {
            native::set_result(m, 0);
            return;
        }
        // A window's bitmap holds the window only, but a program locks it as
        // the screen's, as on a graphics card, and writes at the window's
        // place on the screen. The base moves back by that place, so those
        // writes land in the window's own pixels, and the size reaches to
        // the window's far edges.
        u32 base = deep->pixels;
        u32 width = deep->width;
        u32 height = deep->height;
        if (deep->layer != 0) {
            using i16 = __INT16_TYPE__;
            const i32 min_x = static_cast<i16>(b.r16(deep->layer + ndk::ly_MinX));
            const i32 min_y = static_cast<i16>(b.r16(deep->layer + ndk::ly_MinY));
            if (b.ok() && min_x >= 0 && min_y >= 0) {
                base -= static_cast<u32>(min_y) * deep->bytes_per_row +
                        static_cast<u32>(min_x) * bytes_per_pixel(deep->fmt);
                width += static_cast<u32>(min_x);
                height += static_cast<u32>(min_y);
            }
        }
        u32 item = m.cpu.a[1];
        u32 budget = tags::kMaxTagItems;
        while (const u32 at = tags::next(b, item, budget)) {
            const u32 tag = b.r32(at);
            u32 value = 0;
            switch (tag) {
            case LBMI_WIDTH:
                value = width;
                break;
            case LBMI_HEIGHT:
                value = height;
                break;
            case LBMI_DEPTH:
                value = depth_of(deep->fmt);
                break;
            case LBMI_PIXFMT:
                value = pixfmt_value(deep->fmt);
                break;
            case LBMI_BYTESPERPIX:
                value = bytes_per_pixel(deep->fmt);
                break;
            case LBMI_BYTESPERROW:
                value = deep->bytes_per_row;
                break;
            case LBMI_BASEADDRESS:
                value = base;
                break;
            default:
                continue;
            }
            b.w32(b.r32(at + 4), value);
        }
        native::set_result(m, bitmap);
    }

    static void unlock_bit_map(void* ctx, Machine& m) {
        CyberGraphics& c = of(ctx);
        const u32 handle = m.cpu.a[0];
        const DeepBitmap* deep = c.graphics_->deep(handle);
        if (deep != nullptr && deep->width != 0 && deep->height != 0) {
            c.graphics_->report_bitmap(handle, Rect{0, 0,
                                                    static_cast<i32>(deep->width) - 1,
                                                    static_cast<i32>(deep->height) - 1});
        }
        native::set_result(m, 0);
    }

    static void unlock_bit_map_tag_list(void* ctx, Machine& m) { unlock_bit_map(ctx, m); }

    static void extract_color(void*, Machine& m) {
        Bus b(m);
        const raster::RastPortView v = raster::load_rastport(b, m.cpu.a[0]);
        const u32 mask = m.cpu.a[1];
        const u32 colour = m.cpu.d[0] & 0xFFFFFF;
        const i32 x0 = static_cast<i32>(m.cpu.d[1]) + v.origin_x;
        const i32 y0 = static_cast<i32>(m.cpu.d[2]) + v.origin_y;
        const u32 width = m.cpu.d[3];
        const u32 height = m.cpu.d[4];
        if (!v.ok || mask == 0) {
            native::set_result(m, 1);
            return;
        }
        const u32 bytes_per_row = b.r16(mask + bm_BytesPerRow);
        const u32 rows = b.r16(mask + bm_Rows);
        const u32 plane = b.r32(mask + bm_Planes);
        if (!b.ok() || plane == 0 || bytes_per_row == 0) {
            native::set_result(m, 1);
            return;
        }
        const Area area = visible_area(v, x0, y0, width, height);
        const i64 mask_width = static_cast<i64>(bytes_per_row) * 8;
        for (i32 y = area.y0; y <= area.y1 && b.ok(); ++y) {
            if (static_cast<u32>(y) >= rows) {
                continue;
            }
            for (i32 x = area.x0; x <= area.x1; ++x) {
                if (x >= mask_width) {
                    continue;
                }
                const bool match = raster::in_clip(b, v.clip, x, y) &&
                                   read_pixel(b, v.bitmap, x, y, false).xrgb == colour;
                const u32 at = plane + static_cast<u32>(y) * bytes_per_row +
                               (static_cast<u32>(x) >> 3);
                const u32 bit = 0x80u >> (static_cast<u32>(x) & 7);
                const u32 byte = b.r8(at);
                b.w8(at, match ? (byte | bit) : (byte & ~bit));
            }
        }
        native::set_result(m, 1);
    }
};

namespace {

const NativeVector kCyberVectors[] = {
    {nullptr, "Open"},
    {nullptr, "Close"},
    {nullptr, "Expunge"},
    {nullptr, "Reserved"},
    {nullptr, "Private"},
    {nullptr, "Private"},
    {nullptr, "Private"},
    {nullptr, "Private"},
    {CyberGraphicsNatives::is_cyber_mode_id, "IsCyberModeID"},
    {CyberGraphicsNatives::best_c_mode_id_tag_list, "BestCModeIDTagList"},
    {CyberGraphicsNatives::c_mode_request_tag_list, "CModeRequestTagList"},
    {CyberGraphicsNatives::alloc_c_mode_list_tag_list, "AllocCModeListTagList"},
    {CyberGraphicsNatives::free_c_mode_list, "FreeCModeList"},
    {nullptr, "Private"},
    {CyberGraphicsNatives::scale_pixel_array, "ScalePixelArray"},
    {CyberGraphicsNatives::get_cyber_map_attr, "GetCyberMapAttr"},
    {CyberGraphicsNatives::get_cyber_id_attr, "GetCyberIDAttr"},
    {CyberGraphicsNatives::read_rgb_pixel, "ReadRGBPixel"},
    {CyberGraphicsNatives::write_rgb_pixel, "WriteRGBPixel"},
    {CyberGraphicsNatives::read_pixel_array, "ReadPixelArray"},
    {CyberGraphicsNatives::write_pixel_array, "WritePixelArray"},
    {CyberGraphicsNatives::move_pixel_array, "MovePixelArray"},
    {nullptr, "Private"},
    {CyberGraphicsNatives::invert_pixel_array, "InvertPixelArray"},
    {CyberGraphicsNatives::fill_pixel_array, "FillPixelArray"},
    {CyberGraphicsNatives::do_c_draw_method_tag_list, "DoCDrawMethodTagList"},
    {CyberGraphicsNatives::c_video_ctrl_tag_list, "CVideoCtrlTagList"},
    {CyberGraphicsNatives::lock_bit_map_tag_list, "LockBitMapTagList"},
    {CyberGraphicsNatives::unlock_bit_map, "UnLockBitMap"},
    {CyberGraphicsNatives::unlock_bit_map_tag_list, "UnLockBitMapTagList"},
    {CyberGraphicsNatives::extract_color, "ExtractColor"},
    {nullptr, "Private"},
    {CyberGraphicsNatives::write_lut_pixel_array, "WriteLUTPixelArray"},
};

}

bool CyberGraphics::init(Machine&, Exec& exec, Graphics& graphics) {
    graphics_ = &graphics;
    base_ = exec.make_native_library("cybergraphics.library", kVersion, kCyberVectors,
                                     sizeof kCyberVectors / sizeof kCyberVectors[0], this);
    if (base_ == 0) {
        graphics_ = nullptr;
        return false;
    }
    return true;
}

}
