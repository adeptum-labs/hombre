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

#include "graphics.hpp"

#include "exec.hpp"
#include "raster.hpp"
#include "version.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using i16 = __INT16_TYPE__;

namespace {

constexpr char kGraphicsName[] = "graphics.library";
constexpr char kTopazName[] = "topaz.font";
constexpr u32 kMaxTextChars = 65536;
constexpr u32 kTextChunk = 64;

// A RastPort's BitMap and the rectangle drawing stays inside, both in
// bitmap coordinates. The layer origin is what RastPort coordinates are
// relative to.
struct View {
    raster::Bitmap bitmap;
    raster::Clip clip;
    i32 origin_x = 0;
    i32 origin_y = 0;
    u32 bitmap_addr = 0;
    bool ok = false;
};

View view(Bus& b, u32 rp) {
    View v{};
    if (rp == 0) {
        return v;
    }
    const u32 layer = b.r32(rp + ndk::rp_Layer);
    const u32 bitmap = b.r32(rp + ndk::rp_BitMap);
    if (bitmap == 0 || !raster::load_bitmap(b, bitmap, v.bitmap)) {
        return v;
    }
    v.bitmap_addr = bitmap;
    if (layer != 0) {
        const i32 min_x = static_cast<i16>(b.r16(layer + ndk::ly_MinX));
        const i32 min_y = static_cast<i16>(b.r16(layer + ndk::ly_MinY));
        const i32 max_x = static_cast<i16>(b.r16(layer + ndk::ly_MaxX));
        const i32 max_y = static_cast<i16>(b.r16(layer + ndk::ly_MaxY));
        if (b.r32(layer + ndk::ly_SuperBitMap) == bitmap) {
            v.clip = raster::Clip{0, 0, max_x - min_x, max_y - min_y};
        } else {
            v.origin_x = min_x;
            v.origin_y = min_y;
            v.clip = raster::Clip{min_x, min_y, max_x, max_y};
        }
    } else {
        v.clip = raster::Clip{0, 0, static_cast<i32>(v.bitmap.bytes_per_row << 3) - 1,
                              static_cast<i32>(v.bitmap.rows) - 1};
    }
    v.ok = b.ok();
    return v;
}

raster::Pens pens(Bus& b, u32 rp) {
    return raster::Pens{static_cast<u8>(b.r8(rp + ndk::rp_FgPen)),
                        static_cast<u8>(b.r8(rp + ndk::rp_BgPen)),
                        static_cast<u8>(b.r8(rp + ndk::rp_DrawMode))};
}

i32 cp_x(Bus& b, u32 rp) { return static_cast<i16>(b.r16(rp + ndk::rp_cp_x)); }

i32 cp_y(Bus& b, u32 rp) { return static_cast<i16>(b.r16(rp + ndk::rp_cp_y)); }

bool drawable(const View& v, i32 x, i32 y) {
    return x >= v.clip.x0 && x <= v.clip.x1 && y >= v.clip.y0 && y <= v.clip.y1 && x >= 0 &&
           y >= 0 && (static_cast<u32>(x) >> 3) < v.bitmap.bytes_per_row &&
           static_cast<u32>(y) < v.bitmap.rows;
}

Rect unite(Rect a, const Rect& b) {
    if (b.x1 < b.x0) {
        return a;
    }
    if (a.x1 < a.x0) {
        return b;
    }
    if (b.x0 < a.x0) {
        a.x0 = b.x0;
    }
    if (b.y0 < a.y0) {
        a.y0 = b.y0;
    }
    if (b.x1 > a.x1) {
        a.x1 = b.x1;
    }
    if (b.y1 > a.y1) {
        a.y1 = b.y1;
    }
    return a;
}

// Tells the display about a change in the front screen's bitmap, or the
// damage sink about a change in any other bitmap.
void report(const DisplayState* display, Machine& m, const View& v, const Rect& changed) {
    if (display == nullptr || changed.x1 < changed.x0) {
        return;
    }
    if (display->front_screen != 0 && v.bitmap_addr == display->front_screen + ndk::sc_BitMap) {
        if (display->sink.refresh != nullptr) {
            display->sink.refresh(display->sink.ctx, m, display->front_screen, changed);
        }
        return;
    }
    if (display->damage.damaged != nullptr) {
        display->damage.damaged(display->damage.ctx, m, v.bitmap_addr, changed);
    }
}

void refresh_viewport(const DisplayState* display, Machine& m, Bus& b, u32 vp) {
    if (display == nullptr || display->front_screen == 0 || display->sink.refresh == nullptr) {
        return;
    }
    if (native::guest_address(m, vp) !=
        native::guest_address(m, display->front_screen + ndk::sc_ViewPort)) {
        return;
    }
    const i32 width = static_cast<i32>(b.r16(display->front_screen + ndk::sc_Width));
    const i32 height = static_cast<i32>(b.r16(display->front_screen + ndk::sc_Height));
    if (b.ok() && width > 0 && height > 0) {
        display->sink.refresh(display->sink.ctx, m, display->front_screen,
                              Rect{0, 0, width - 1, height - 1});
    }
}

struct ColorMapTables {
    u32 limit = 0;
    u32 table = 0;
    u32 low_table = 0;
    bool ok = false;
};

ColorMapTables colormap_tables(Bus& b, u32 map) {
    ColorMapTables t;
    if (map == 0) {
        return t;
    }
    t.limit = b.r16(map + ndk::cm_Count);
    t.table = b.r32(map + ndk::cm_ColorTable);
    if (b.r8(map + ndk::cm_Type) >= ndk::COLORMAP_TYPE_V39) {
        t.low_table = b.r32(map + ndk::cm_LowColorBits);
    }
    t.ok = b.ok() && t.table != 0;
    return t;
}

bool write_rgb32_entry(Bus& b, const ColorMapTables& t, u32 index, u32 r, u32 g, u32 bl) {
    if (!t.ok || index >= t.limit) {
        return false;
    }
    const u32 r8 = (r >> 24) & 0xFF;
    const u32 g8 = (g >> 24) & 0xFF;
    const u32 b8 = (bl >> 24) & 0xFF;
    const u32 hi = ((r8 >> 4) << 8) | ((g8 >> 4) << 4) | (b8 >> 4);
    b.w16(t.table + index * 2, hi);
    if (t.low_table != 0) {
        const u32 lo = ((r8 & 0xF) << 8) | ((g8 & 0xF) << 4) | (b8 & 0xF);
        b.w16(t.low_table + index * 2, lo);
    }
    return b.ok();
}

// The bytes AllocRaster hands out for width and height, 0 if they do not
// fit in a guest block.
u32 raster_bytes(u32 width, u32 height) {
    const u64 row = (static_cast<u64>(width) + 15) / 16 * 2;
    const u64 total = row * height;
    return total <= 0xFFFFFFFF ? static_cast<u32>(total) : 0;
}

bool equals_ci(Bus& b, u32 addr, const char* host) {
    for (u32 i = 0;; ++i) {
        u32 c = b.r8(addr + i);
        if (!b.ok()) {
            return false;
        }
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (c != static_cast<u8>(host[i])) {
            return false;
        }
        if (c == 0) {
            return true;
        }
    }
}

}

struct GraphicsNatives {
    static Graphics& of(void* ctx) { return *static_cast<Graphics*>(ctx); }

    static void clear_screen(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 rp = m.cpu.a[1];
        if (rp == 0) {
            return;
        }
        Bus b(m);
        const View v = view(b, rp);
        if (!v.ok) {
            return;
        }
        const raster::Pens p = pens(b, rp);
        const i32 x = cp_x(b, rp) + v.origin_x;
        const i32 y = cp_y(b, rp) + v.origin_y;
        Rect dirty = raster::fill_rect(b, v.bitmap, v.clip, Rect{x, y, v.clip.x1, y}, p.bg);
        dirty = unite(dirty, raster::fill_rect(b, v.bitmap, v.clip,
                                               Rect{v.clip.x0, y + 1, v.clip.x1, v.clip.y1},
                                               p.bg));
        if (b.ok()) {
            report(g.display_, m, v, dirty);
        }
    }

    static void text_length(void*, Machine& m) { native::set_result(m, 8 * m.cpu.d[0]); }

    static void text(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 rp = m.cpu.a[1];
        const u32 addr = m.cpu.a[0];
        const u32 count = m.cpu.d[0];
        if (rp == 0 || addr == 0 || count == 0) {
            return;
        }
        u8 buffer[kTextChunk];
        u32 at = 0;
        for (; at < count && at < kMaxTextChars; at += kTextChunk) {
            u32 n = count - at;
            if (n > kTextChunk) {
                n = kTextChunk;
            }
            for (u32 i = 0; i < n; ++i) {
                buffer[i] = static_cast<u8>(b.r8(addr + at + i));
            }
            if (!b.ok()) {
                return;
            }
            g.text(rp, buffer, n);
            if (m.cpu.exit != m68k::ExitReason::None) {
                return;
            }
        }
        if (at < count) {
            Bus tail(m);
            tail.w16(rp + ndk::rp_cp_x, tail.r16(rp + ndk::rp_cp_x) + 8 * (count - at));
        }
    }

    static void set_font(void* ctx, Machine& m) { of(ctx).set_font(m.cpu.a[1], m.cpu.a[0]); }

    static void open_font(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 attr = m.cpu.a[0];
        u32 result = 0;
        if (attr != 0 && b.r16(attr + ndk::ta_YSize) == 8 &&
            equals_ci(b, b.r32(attr + ndk::ta_Name), kTopazName)) {
            result = g.topaz_font();
        }
        if (b.ok()) {
            native::set_result(m, result);
        }
    }

    static void close_font(void*, Machine&) {}

    static void load_rgb4(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 vp = m.cpu.a[0];
        const u32 colors = m.cpu.a[1];
        const u32 count = m.cpu.d[0];
        if (vp == 0 || colors == 0) {
            return;
        }
        const ColorMapTables t = colormap_tables(b, b.r32(vp + ndk::vp_ColorMap));
        if (!t.ok) {
            return;
        }
        const u32 n = count < t.limit ? count : t.limit;
        for (u32 i = 0; i < n; ++i) {
            const u32 rgb4 = b.r16(colors + i * 2);
            b.w16(t.table + i * 2, rgb4);
            if (t.low_table != 0) {
                b.w16(t.low_table + i * 2, rgb4 & 0x0FFF);
            }
        }
        if (n != 0 && b.ok()) {
            refresh_viewport(g.display_, m, b, vp);
        }
    }

    static void init_rastport(void* ctx, Machine& m) { of(ctx).init_rastport(m.cpu.a[1]); }

    static void set_rast(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 rp = m.cpu.a[1];
        if (rp == 0) {
            return;
        }
        Bus b(m);
        const View v = view(b, rp);
        if (!v.ok) {
            return;
        }
        const Rect dirty = raster::fill_rect(b, v.bitmap, v.clip,
                                        Rect{v.clip.x0, v.clip.y0, v.clip.x1, v.clip.y1},
                                        m.cpu.d[0]);
        if (b.ok()) {
            report(g.display_, m, v, dirty);
        }
    }

    static void move(void* ctx, Machine& m) {
        of(ctx).move(m.cpu.a[1], static_cast<i16>(m.cpu.d[0]), static_cast<i16>(m.cpu.d[1]));
    }

    static void draw(void* ctx, Machine& m) {
        of(ctx).draw_line(m.cpu.a[1], static_cast<i16>(m.cpu.d[0]),
                          static_cast<i16>(m.cpu.d[1]));
    }

    static void set_rgb4(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 vp = m.cpu.a[0];
        if (vp == 0) {
            return;
        }
        const u32 index = m.cpu.d[0];
        const ColorMapTables t = colormap_tables(b, b.r32(vp + ndk::vp_ColorMap));
        if (!t.ok || index >= t.limit) {
            return;
        }
        const u32 value =
            ((m.cpu.d[1] & 15) << 8) | ((m.cpu.d[2] & 15) << 4) | (m.cpu.d[3] & 15);
        b.w16(t.table + index * 2, value);
        if (t.low_table != 0) {
            b.w16(t.low_table + index * 2, value);
        }
        if (b.ok()) {
            refresh_viewport(g.display_, m, b, vp);
        }
    }

    static void rect_fill(void* ctx, Machine& m) {
        of(ctx).rect_fill(m.cpu.a[1], static_cast<i16>(m.cpu.d[0]), static_cast<i16>(m.cpu.d[1]),
                          static_cast<i16>(m.cpu.d[2]), static_cast<i16>(m.cpu.d[3]));
    }

    static void read_pixel(void*, Machine& m) {
        const u32 rp = m.cpu.a[1];
        if (rp == 0) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        Bus b(m);
        const View v = view(b, rp);
        if (!v.ok) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        const i32 x = static_cast<i16>(m.cpu.d[0]) + v.origin_x;
        const i32 y = static_cast<i16>(m.cpu.d[1]) + v.origin_y;
        if (!drawable(v, x, y)) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        const u32 pen = raster::read_pixel(b, v.bitmap, x, y);
        native::set_result(m, b.ok() ? pen : 0xFFFFFFFF);
    }

    static void write_pixel(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 rp = m.cpu.a[1];
        if (rp == 0) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        Bus b(m);
        const View v = view(b, rp);
        if (!v.ok) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        const i32 x = static_cast<i16>(m.cpu.d[0]) + v.origin_x;
        const i32 y = static_cast<i16>(m.cpu.d[1]) + v.origin_y;
        if (!drawable(v, x, y)) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        raster::write_pixel(b, v.bitmap, x, y, pens(b, rp).fg);
        if (b.ok()) {
            report(g.display_, m, v, Rect{x, y, x, y});
        }
        native::set_result(m, b.ok() ? 0 : 0xFFFFFFFF);
    }

    static void set_apen(void*, Machine& m) {
        const u32 rp = m.cpu.a[1];
        if (rp == 0) {
            return;
        }
        Bus b(m);
        b.w8(rp + ndk::rp_FgPen, m.cpu.d[0]);
    }

    static void set_bpen(void*, Machine& m) {
        const u32 rp = m.cpu.a[1];
        if (rp == 0) {
            return;
        }
        Bus b(m);
        b.w8(rp + ndk::rp_BgPen, m.cpu.d[0]);
    }

    static void set_drmd(void*, Machine& m) {
        const u32 rp = m.cpu.a[1];
        if (rp == 0) {
            return;
        }
        Bus b(m);
        b.w8(rp + ndk::rp_DrawMode, m.cpu.d[0]);
    }

    static void init_bitmap(void*, Machine& m) {
        const u32 bitmap = m.cpu.a[0];
        if (bitmap == 0) {
            return;
        }
        Bus b(m);
        const u64 row = (static_cast<u64>(m.cpu.d[1]) + 15) / 16 * 2;
        b.w16(bitmap + ndk::bm_BytesPerRow, static_cast<u32>(row));
        b.w16(bitmap + ndk::bm_Rows, m.cpu.d[2]);
        b.w8(bitmap + ndk::bm_Flags, 0);
        b.w8(bitmap + ndk::bm_Depth, m.cpu.d[0]);
        b.w16(bitmap + 6, 0);  // the pad word
    }

    static void scroll_raster(void* ctx, Machine& m) {
        of(ctx).scroll(m.cpu.a[1], static_cast<i16>(m.cpu.d[0]), static_cast<i16>(m.cpu.d[1]),
                       static_cast<i16>(m.cpu.d[2]), static_cast<i16>(m.cpu.d[3]),
                       static_cast<i16>(m.cpu.d[4]), static_cast<i16>(m.cpu.d[5]));
    }

    static void ask_font(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 rp = m.cpu.a[1];
        const u32 attr = m.cpu.a[0];
        if (rp == 0 || attr == 0) {
            return;
        }
        Bus b(m);
        const u32 font = b.r32(rp + ndk::rp_Font);
        b.w32(attr + ndk::ta_Name, font == g.topaz_ ? g.topaz_name_ : 0);
        b.w16(attr + ndk::ta_YSize, 8);
        b.w16(attr + ndk::ta_Style, 0);
        b.w8(attr + ndk::ta_Flags, 0);
    }

    static void alloc_raster(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 bytes = raster_bytes(m.cpu.d[0], m.cpu.d[1]);
        u32 result = 0;
        if (bytes != 0 && g.exec_ != nullptr) {
            result = g.exec_->alloc(bytes, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
        }
        native::set_result(m, result);
    }

    static void free_raster(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 p = m.cpu.a[0];
        const u32 bytes = raster_bytes(m.cpu.d[0], m.cpu.d[1]);
        if (p != 0 && bytes != 0 && g.exec_ != nullptr) {
            g.exec_->free(p, bytes);
        }
    }

    static void get_rgb4(void*, Machine& m) {
        Bus b(m);
        const u32 map = m.cpu.a[0];
        const u32 entry = m.cpu.d[0];
        u32 result = 0xFFFFFFFF;
        if (map != 0) {
            const u32 limit = b.r16(map + ndk::cm_Count);
            const u32 table = b.r32(map + ndk::cm_ColorTable);
            if (table != 0 && entry < limit) {
                result = b.r16(table + entry * 2);
            }
        }
        if (b.ok()) {
            native::set_result(m, result);
        }
    }

    static void set_rgb32(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 vp = m.cpu.a[0];
        if (vp == 0) {
            return;
        }
        const ColorMapTables t = colormap_tables(b, b.r32(vp + ndk::vp_ColorMap));
        if (write_rgb32_entry(b, t, m.cpu.d[0], m.cpu.d[1], m.cpu.d[2], m.cpu.d[3])) {
            refresh_viewport(g.display_, m, b, vp);
        }
    }

    static void load_rgb32(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 vp = m.cpu.a[0];
        u32 p = m.cpu.a[1];
        if (vp == 0 || p == 0) {
            return;
        }
        const ColorMapTables t = colormap_tables(b, b.r32(vp + ndk::vp_ColorMap));
        if (!t.ok) {
            return;
        }
        bool changed = false;
        constexpr u32 kMaxLoadRecords = 65536;
        for (u32 rec = 0; rec < kMaxLoadRecords; ++rec) {
            const u32 header = b.r32(p);
            p += 4;
            if (!b.ok()) {
                return;
            }
            const u32 count = header >> 16;
            if (count == 0) {
                break;
            }
            const u32 first = header & 0xFFFF;
            for (u32 i = 0; i < count; ++i) {
                const u32 r = b.r32(p);
                const u32 gr = b.r32(p + 4);
                const u32 bl = b.r32(p + 8);
                p += 12;
                if (!b.ok()) {
                    return;
                }
                if (write_rgb32_entry(b, t, first + i, r, gr, bl)) {
                    changed = true;
                }
            }
        }
        if (changed && b.ok()) {
            refresh_viewport(g.display_, m, b, vp);
        }
    }

    static void get_rgb32(void*, Machine& m) {
        Bus b(m);
        const u32 map = m.cpu.a[0];
        const u32 first = m.cpu.d[0];
        const u32 ncolors = m.cpu.d[1];
        const u32 out = m.cpu.a[1];
        if (map == 0 || out == 0 || ncolors == 0) {
            return;
        }
        const ColorMapTables t = colormap_tables(b, map);
        if (!t.ok || first >= t.limit) {
            return;
        }
        const u32 avail = t.limit - first;
        const u32 count = ncolors < avail ? ncolors : avail;
        for (u32 i = 0; i < count; ++i) {
            const u32 idx = first + i;
            const u32 hi = b.r16(t.table + idx * 2);
            const u32 lo = t.low_table != 0 ? b.r16(t.low_table + idx * 2) : hi;
            const u32 r8 = (((hi >> 8) & 0xF) << 4) | ((lo >> 8) & 0xF);
            const u32 g8 = (((hi >> 4) & 0xF) << 4) | ((lo >> 4) & 0xF);
            const u32 b8 = ((hi & 0xF) << 4) | (lo & 0xF);
            b.w32(out + i * 12, r8 * 0x01010101u);
            b.w32(out + i * 12 + 4, g8 * 0x01010101u);
            b.w32(out + i * 12 + 8, b8 * 0x01010101u);
        }
    }

    static void set_rgb32_cm(void*, Machine& m) {
        Bus b(m);
        const ColorMapTables t = colormap_tables(b, m.cpu.a[0]);
        write_rgb32_entry(b, t, m.cpu.d[0], m.cpu.d[1], m.cpu.d[2], m.cpu.d[3]);
    }
};

namespace {

struct GraphicsFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr GraphicsFunction kGraphicsFunctions[] = {
    {graphics_lvo::ClearScreen, GraphicsNatives::clear_screen, "ClearScreen"},
    {graphics_lvo::TextLength, GraphicsNatives::text_length, "TextLength"},
    {graphics_lvo::Text, GraphicsNatives::text, "Text"},
    {graphics_lvo::SetFont, GraphicsNatives::set_font, "SetFont"},
    {graphics_lvo::OpenFont, GraphicsNatives::open_font, "OpenFont"},
    {graphics_lvo::CloseFont, GraphicsNatives::close_font, "CloseFont"},
    {graphics_lvo::LoadRGB4, GraphicsNatives::load_rgb4, "LoadRGB4"},
    {graphics_lvo::InitRastPort, GraphicsNatives::init_rastport, "InitRastPort"},
    {graphics_lvo::SetRast, GraphicsNatives::set_rast, "SetRast"},
    {graphics_lvo::Move, GraphicsNatives::move, "Move"},
    {graphics_lvo::Draw, GraphicsNatives::draw, "Draw"},
    {graphics_lvo::SetRGB4, GraphicsNatives::set_rgb4, "SetRGB4"},
    {graphics_lvo::RectFill, GraphicsNatives::rect_fill, "RectFill"},
    {graphics_lvo::ReadPixel, GraphicsNatives::read_pixel, "ReadPixel"},
    {graphics_lvo::WritePixel, GraphicsNatives::write_pixel, "WritePixel"},
    {graphics_lvo::SetAPen, GraphicsNatives::set_apen, "SetAPen"},
    {graphics_lvo::SetBPen, GraphicsNatives::set_bpen, "SetBPen"},
    {graphics_lvo::SetDrMd, GraphicsNatives::set_drmd, "SetDrMd"},
    {graphics_lvo::InitBitMap, GraphicsNatives::init_bitmap, "InitBitMap"},
    {graphics_lvo::ScrollRaster, GraphicsNatives::scroll_raster, "ScrollRaster"},
    {graphics_lvo::AskFont, GraphicsNatives::ask_font, "AskFont"},
    {graphics_lvo::AllocRaster, GraphicsNatives::alloc_raster, "AllocRaster"},
    {graphics_lvo::FreeRaster, GraphicsNatives::free_raster, "FreeRaster"},
    {graphics_lvo::GetRGB4, GraphicsNatives::get_rgb4, "GetRGB4"},
    {graphics_lvo::SetRGB32, GraphicsNatives::set_rgb32, "SetRGB32"},
    {graphics_lvo::LoadRGB32, GraphicsNatives::load_rgb32, "LoadRGB32"},
    {graphics_lvo::GetRGB32, GraphicsNatives::get_rgb32, "GetRGB32"},
    {graphics_lvo::SetRGB32CM, GraphicsNatives::set_rgb32_cm, "SetRGB32CM"},
};

}

bool Graphics::init(Machine& m, Exec& exec, DisplayState& display) {
    machine_ = &m;
    exec_ = &exec;
    display_ = &display;
    base_ = 0;
    topaz_ = 0;
    topaz_name_ = 0;
    topaz_attr_ = 0;
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const GraphicsFunction& f : kGraphicsFunctions) {
        vectors_[f.lvo / ndk::LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }

    const u32 font = exec.alloc(ndk::TextFont_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    const u32 name = exec.alloc(sizeof kTopazName, ndk::MEMF_PUBLIC);
    const u32 attr = exec.alloc(ndk::TextAttr_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (font == 0 || name == 0 || attr == 0) {
        if (font != 0) {
            exec.free(font, ndk::TextFont_SIZEOF);
        }
        if (name != 0) {
            exec.free(name, sizeof kTopazName);
        }
        if (attr != 0) {
            exec.free(attr, ndk::TextAttr_SIZEOF);
        }
        return false;
    }
    Bus b(m);
    for (u32 i = 0; i < sizeof kTopazName; ++i) {
        b.w8(name + i, static_cast<u8>(kTopazName[i]));
    }
    b.w32(attr + ndk::ta_Name, name);
    b.w16(attr + ndk::ta_YSize, 8);
    b.w16(font + ndk::tf_YSize, 8);
    b.w16(font + ndk::tf_XSize, 8);
    b.w16(font + ndk::tf_Baseline, 6);
    b.w8(font + ndk::tf_LoChar, 32);
    b.w8(font + ndk::tf_HiChar, 126);
    if (!b.ok()) {
        exec.free(font, ndk::TextFont_SIZEOF);
        exec.free(name, sizeof kTopazName);
        exec.free(attr, ndk::TextAttr_SIZEOF);
        return false;
    }

    const u32 base = exec.make_native_library(kGraphicsName, kHombreLibVersion, vectors_, kVectors,
                                              this);
    if (base == 0) {
        exec.free(font, ndk::TextFont_SIZEOF);
        exec.free(name, sizeof kTopazName);
        exec.free(attr, ndk::TextAttr_SIZEOF);
        return false;
    }
    topaz_attr_ = attr;
    topaz_ = font;
    topaz_name_ = name;
    base_ = base;
    return true;
}

void Graphics::init_rastport(u32 rp) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    for (u32 i = 0; i < ndk::RastPort_SIZEOF; ++i) {
        b.w8(rp + i, 0);
    }
    b.w8(rp + ndk::rp_Mask, 0xFF);
    b.w8(rp + ndk::rp_FgPen, 1);
    b.w8(rp + ndk::rp_BgPen, 0);
    b.w8(rp + ndk::rp_AOlPen, 1);
    b.w8(rp + ndk::rp_DrawMode, ndk::JAM2);
    b.w32(rp + ndk::rp_Font, topaz_);
    b.w16(rp + ndk::rp_TxHeight, 8);
    b.w16(rp + ndk::rp_TxWidth, 8);
    b.w16(rp + ndk::rp_TxBaseline, 6);
}

void Graphics::set_pens(u32 rp, u32 fg, u32 bg, u32 mode) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    b.w8(rp + ndk::rp_FgPen, fg);
    b.w8(rp + ndk::rp_BgPen, bg);
    b.w8(rp + ndk::rp_DrawMode, mode);
}

void Graphics::move(u32 rp, i32 x, i32 y) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    b.w16(rp + ndk::rp_cp_x, static_cast<u32>(x));
    b.w16(rp + ndk::rp_cp_y, static_cast<u32>(y));
}

void Graphics::rect_fill(u32 rp, i32 x0, i32 y0, i32 x1, i32 y1) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    const View v = view(b, rp);
    if (!v.ok) {
        return;
    }
    const Rect area{x0 + v.origin_x, y0 + v.origin_y, x1 + v.origin_x, y1 + v.origin_y};
    const Rect dirty = raster::fill_rect(b, v.bitmap, v.clip, area, pens(b, rp).fg);
    if (b.ok()) {
        report(display_, *machine_, v, dirty);
    }
}

void Graphics::draw_line(u32 rp, i32 x1, i32 y1) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    const View v = view(b, rp);
    if (!v.ok) {
        return;
    }
    const i32 x0 = cp_x(b, rp) + v.origin_x;
    const i32 y0 = cp_y(b, rp) + v.origin_y;
    const Rect dirty = raster::draw_line(b, v.bitmap, v.clip, x0, y0, x1 + v.origin_x,
                                         y1 + v.origin_y, pens(b, rp).fg);
    b.w16(rp + ndk::rp_cp_x, static_cast<u32>(x1));
    b.w16(rp + ndk::rp_cp_y, static_cast<u32>(y1));
    if (b.ok()) {
        report(display_, *machine_, v, dirty);
    }
}

u32 Graphics::text(u32 rp, const u8* chars, u32 count) {
    if (rp == 0 || chars == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    const View v = view(b, rp);
    if (!v.ok) {
        return 0;
    }
    const i32 cursor_x = cp_x(b, rp);
    const i32 cursor_y = cp_y(b, rp);
    u32 advance = 0;
    const Rect dirty =
        raster::draw_text(b, v.bitmap, v.clip, pens(b, rp), cursor_x + v.origin_x,
                          cursor_y + v.origin_y, chars, count, advance);
    b.w16(rp + ndk::rp_cp_x, static_cast<u32>(cursor_x) + advance);
    if (b.ok()) {
        report(display_, *machine_, v, dirty);
    }
    return advance;
}

void Graphics::scroll(u32 rp, i32 dx, i32 dy, i32 x0, i32 y0, i32 x1, i32 y1) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    const View v = view(b, rp);
    if (!v.ok) {
        return;
    }
    const Rect area{x0 + v.origin_x, y0 + v.origin_y, x1 + v.origin_x, y1 + v.origin_y};
    const Rect dirty =
        raster::scroll_rect(b, v.bitmap, v.clip, dx, dy, area, b.r8(rp + ndk::rp_BgPen));
    if (b.ok()) {
        report(display_, *machine_, v, dirty);
    }
}

void Graphics::set_font(u32 rp, u32 font) {
    if (rp == 0 || font == 0) {
        return;
    }
    Bus b(*machine_);
    b.w32(rp + ndk::rp_Font, font);
    b.w16(rp + ndk::rp_TxHeight, b.r16(font + ndk::tf_YSize));
    b.w16(rp + ndk::rp_TxWidth, b.r16(font + ndk::tf_XSize));
    b.w16(rp + ndk::rp_TxBaseline, b.r16(font + ndk::tf_Baseline));
}

}
