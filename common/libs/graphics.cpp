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

// The BitMap, the clip and the origin of a RastPort's drawing.
using View = raster::RastPortView;

raster::Pens pens(Bus& b, u32 rp) {
    return raster::Pens{static_cast<u8>(b.r8(rp + ndk::rp_FgPen)),
                        static_cast<u8>(b.r8(rp + ndk::rp_BgPen)),
                        static_cast<u8>(b.r8(rp + ndk::rp_DrawMode))};
}

i32 cp_x(Bus& b, u32 rp) { return static_cast<i16>(b.r16(rp + ndk::rp_cp_x)); }

i32 cp_y(Bus& b, u32 rp) { return static_cast<i16>(b.r16(rp + ndk::rp_cp_y)); }

bool drawable(Bus& b, const View& v, i32 x, i32 y) {
    return x >= 0 && y >= 0 && (static_cast<u32>(x) >> 3) < v.bitmap.bytes_per_row &&
           static_cast<u32>(y) < v.bitmap.rows && raster::in_clip(b, v.clip, x, y);
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

void bump_palette(DisplayState* display) {
    if (display != nullptr) {
        ++display->palette_gen;
    }
}

void mark_viewport_dirty(DisplayState* display, Machine& m, u32 vp) {
    if (display == nullptr || display->front_screen == 0 || vp == 0) {
        return;
    }
    if (native::guest_address(m, vp) ==
        native::guest_address(m, display->front_screen + ndk::sc_ViewPort)) {
        display->palette_dirty = true;
    }
}

void mark_colormap_dirty(DisplayState* display, Machine& m, u32 map) {
    if (display == nullptr || display->front_screen == 0 || map == 0) {
        return;
    }
    Bus fb(m);
    const u32 front_map = fb.r32(display->front_screen + ndk::sc_ViewPort + ndk::vp_ColorMap);
    if (fb.ok() && front_map != 0 &&
        native::guest_address(m, map) == native::guest_address(m, front_map)) {
        display->palette_dirty = true;
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
    if (t.limit > 256) {
        t.limit = 256;
    }
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

namespace {

// --- area fill ---------------------------------------------------------------

// struct AreaInfo
constexpr u32 ai_VctrTbl = 0;
constexpr u32 ai_VctrPtr = 4;
constexpr u32 ai_FlagTbl = 8;
constexpr u32 ai_FlagPtr = 12;
constexpr u32 ai_Count = 16;
constexpr u32 ai_MaxCount = 18;
constexpr u32 ai_FirstX = 20;
constexpr u32 ai_FirstY = 22;

// What each vector of the table is: the first point of a polygon, a
// further point, or the centre and then the radii of an ellipse.
constexpr u8 kAreaMove = 1;
constexpr u8 kAreaDraw = 0;
constexpr u8 kAreaEllipse = 2;
constexpr u8 kAreaRadii = 3;

// The most vectors one AreaEnd fills, and the longest polygon it takes.
constexpr u32 kMaxAreaVectors = 1024;
constexpr u32 kMaxPolygon = 256;

void fill_span(Bus& b, const View& v, i32 y, i32 x0, i32 x1, u32 pen, Rect& dirty) {
    if (x1 < x0) {
        return;
    }
    const Rect r = raster::fill_rect(b, v.bitmap, v.clip, Rect{x0, y, x1, y}, pen);
    dirty = unite(dirty, r);
}

// Fills the polygon of n points (bitmap coordinates) by the even-odd rule,
// its edges included: a shape too thin to hold a pixel between its edges,
// or one that runs out and back along the same line, still shows whole.
void fill_polygon(Bus& b, const View& v, const i32* xs, const i32* ys, u32 n, u32 pen,
                  Rect& dirty) {
    if (n < 2) {
        return;
    }
    for (u32 i = 0; i < n; ++i) {
        const u32 j = (i + 1) % n;
        dirty = unite(dirty, raster::draw_line(b, v.bitmap, v.clip, xs[i], ys[i], xs[j], ys[j],
                                               pen));
    }
    if (n < 3) {
        return;
    }
    i32 top = ys[0];
    i32 bottom = ys[0];
    for (u32 i = 1; i < n; ++i) {
        top = ys[i] < top ? ys[i] : top;
        bottom = ys[i] > bottom ? ys[i] : bottom;
    }
    top = top < v.clip.y0 ? v.clip.y0 : top;
    bottom = bottom > v.clip.y1 ? v.clip.y1 : bottom;
    for (i32 y = top; y <= bottom; ++y) {
        i32 cross[kMaxPolygon];
        u32 count = 0;
        for (u32 i = 0; i < n && count < kMaxPolygon; ++i) {
            const u32 j = (i + 1) % n;
            const i32 ya = ys[i];
            const i32 yb = ys[j];
            if (ya == yb) {
                continue;
            }
            const bool upward = ya < yb;
            const i32 y0 = upward ? ya : yb;
            const i32 y1 = upward ? yb : ya;
            if (y < y0 || y >= y1) {
                continue;
            }
            const i32 x0 = upward ? xs[i] : xs[j];
            const i32 x1 = upward ? xs[j] : xs[i];
            cross[count++] = x0 + static_cast<i32>((static_cast<i64>(x1 - x0) * (y - y0)) /
                                                   (y1 - y0));
        }
        for (u32 i = 1; i < count; ++i) {
            const i32 key = cross[i];
            u32 k = i;
            while (k > 0 && cross[k - 1] > key) {
                cross[k] = cross[k - 1];
                --k;
            }
            cross[k] = key;
        }
        for (u32 i = 0; i + 1 < count; i += 2) {
            fill_span(b, v, y, cross[i], cross[i + 1], pen, dirty);
        }
    }
    // The bottom edge rows and lowest points the half-open rule leaves
    // out, so the outline and the fill meet.
    for (u32 i = 0; i < n; ++i) {
        if (ys[i] >= v.clip.y0 && ys[i] <= v.clip.y1) {
            fill_span(b, v, ys[i], xs[i], xs[i], pen, dirty);
        }
        const u32 j = (i + 1) % n;
        if (ys[i] == ys[j] && ys[i] >= v.clip.y0 && ys[i] <= v.clip.y1) {
            const i32 a = xs[i] < xs[j] ? xs[i] : xs[j];
            const i32 c = xs[i] < xs[j] ? xs[j] : xs[i];
            fill_span(b, v, ys[i], a, c, pen, dirty);
        }
    }
}

// The points of an ellipse with radii a and b around (cx, cy), one call of
// visit(y, x_left, x_right) per row of the outline, top to bottom.
template <typename Visit>
void ellipse_rows(i32 cx, i32 cy, i32 a, i32 rb, Visit visit) {
    if (a < 0 || rb < 0) {
        return;
    }
    if (rb == 0) {
        visit(cy, cx - a, cx + a);
        return;
    }
    const i64 a2 = static_cast<i64>(a) * a;
    const i64 b2 = static_cast<i64>(rb) * rb;
    for (i32 dy = -rb; dy <= rb; ++dy) {
        // The widest x with x^2/a^2 + dy^2/b^2 <= 1.
        const i64 rest = a2 * (b2 - static_cast<i64>(dy) * dy);
        i64 x = 0;
        while ((x + 1) * (x + 1) * b2 <= rest) {
            ++x;
        }
        visit(cy + dy, cx - static_cast<i32>(x), cx + static_cast<i32>(x));
    }
}

void outline_polygon(Bus& b, const View& v, const i32* xs, const i32* ys, u32 n, u32 pen,
                     Rect& dirty) {
    for (u32 i = 0; i + 1 < n || (n > 2 && i < n); ++i) {
        const u32 j = (i + 1) % n;
        dirty = unite(dirty, raster::draw_line(b, v.bitmap, v.clip, xs[i], ys[i], xs[j], ys[j],
                                               pen));
        if (n <= 2) {
            break;
        }
    }
}

}

namespace {

// --- regions -----------------------------------------------------------------

using ndk::Region_SIZEOF;
using ndk::RegionRectangle_SIZEOF;
using ndk::rg_bounds;
using ndk::rg_RegionRectangle;
using ndk::rr_bounds;
using ndk::rr_Next;
using ndk::rr_Prev;

// The most rectangles one region keeps; an operation that would need more
// fails and leaves the region as it was.
constexpr u32 kMaxRegionRects = 256;

// A region's area as disjoint rectangles in absolute coordinates.
struct RectSet {
    Rect rects[kMaxRegionRects]{};
    u32 count = 0;
    bool overflow = false;

    void add(const Rect& r) {
        if (r.x1 < r.x0 || r.y1 < r.y0) {
            return;
        }
        if (count == kMaxRegionRects) {
            overflow = true;
            return;
        }
        rects[count++] = r;
    }
};

Rect read_rect(Bus& b, u32 at) {
    return Rect{static_cast<i16>(b.r16(at)), static_cast<i16>(b.r16(at + 2)),
                static_cast<i16>(b.r16(at + 4)), static_cast<i16>(b.r16(at + 6))};
}

void write_rect(Bus& b, u32 at, const Rect& r) {
    b.w16(at, static_cast<u32>(r.x0));
    b.w16(at + 2, static_cast<u32>(r.y0));
    b.w16(at + 4, static_cast<u32>(r.x1));
    b.w16(at + 6, static_cast<u32>(r.y1));
}

bool load_region(Bus& b, u32 region, RectSet& out) {
    out.count = 0;
    const Rect bounds = read_rect(b, region + rg_bounds);
    u32 rr = b.r32(region + rg_RegionRectangle);
    for (u32 i = 0; rr != 0 && i < kMaxRegionRects && b.ok(); ++i) {
        const Rect r = read_rect(b, rr + rr_bounds);
        out.add(Rect{r.x0 + bounds.x0, r.y0 + bounds.y0, r.x1 + bounds.x0, r.y1 + bounds.y0});
        rr = b.r32(rr + rr_Next);
    }
    return b.ok();
}

Rect intersect(const Rect& a, const Rect& c) {
    return Rect{a.x0 > c.x0 ? a.x0 : c.x0, a.y0 > c.y0 ? a.y0 : c.y0,
                a.x1 < c.x1 ? a.x1 : c.x1, a.y1 < c.y1 ? a.y1 : c.y1};
}

// The parts of a outside cut, added to out.
void subtract(const Rect& a, const Rect& cut, RectSet& out) {
    const Rect c = intersect(a, cut);
    if (c.x1 < c.x0 || c.y1 < c.y0) {
        out.add(a);
        return;
    }
    out.add(Rect{a.x0, a.y0, a.x1, c.y0 - 1});
    out.add(Rect{a.x0, c.y1 + 1, a.x1, a.y1});
    out.add(Rect{a.x0, c.y0, c.x0 - 1, c.y1});
    out.add(Rect{c.x1 + 1, c.y0, a.x1, c.y1});
}

// The set minus r.
void subtract_rect(RectSet& set, const Rect& r) {
    static constinit RectSet scratch{};
    scratch.count = 0;
    scratch.overflow = false;
    for (u32 i = 0; i < set.count; ++i) {
        subtract(set.rects[i], r, scratch);
    }
    set.overflow |= scratch.overflow;
    set.count = 0;
    for (u32 i = 0; i < scratch.count; ++i) {
        set.add(scratch.rects[i]);
    }
}

// The set with r added, the rectangles kept disjoint.
void add_rect(RectSet& set, const Rect& r) {
    static constinit RectSet pieces{};
    pieces.count = 0;
    pieces.overflow = false;
    pieces.add(r);
    for (u32 i = 0; i < set.count; ++i) {
        subtract_rect(pieces, set.rects[i]);
    }
    set.overflow |= pieces.overflow;
    for (u32 i = 0; i < pieces.count; ++i) {
        set.add(pieces.rects[i]);
    }
}

}  // namespace

namespace {
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
        const View v = raster::load_rastport(b, rp);
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

    // struct TextExtent for count characters of the 8x8 font with its
    // baseline at 6: te_Width, te_Height, then te_Extent relative to the
    // pen position.
    static void text_extent(void*, Machine& m) {
        Bus b(m);
        const u32 extent = m.cpu.a[2];
        const u32 count = m.cpu.d[0] & 0xFFFF;
        if (extent == 0) {
            return;
        }
        const u32 width = 8 * count;
        b.w16(extent + 0, width);
        b.w16(extent + 2, 8);
        b.w16(extent + 4, 0);
        b.w16(extent + 6, static_cast<u16>(-6));
        b.w16(extent + 8, static_cast<u16>(width == 0 ? 0 : width - 1));
        b.w16(extent + 10, 1);
    }

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
            bump_palette(g.display_);
            mark_viewport_dirty(g.display_, m, vp);
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
        const View v = raster::load_rastport(b, rp);
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
            bump_palette(g.display_);
            mark_viewport_dirty(g.display_, m, vp);
        }
    }



    // --- regions -------------------------------------------------------------

    // Frees the RegionRectangles of region and writes set as its new list
    // and bounds. False, with the region unchanged, if memory runs out.
    static bool store_region(Graphics& g, Bus& b, u32 region, const RectSet& set) {
        if (set.overflow) {
            return false;
        }
        Rect bounds{0, 0, -1, -1};
        for (u32 i = 0; i < set.count; ++i) {
            bounds = unite(bounds, set.rects[i]);
        }
        if (set.count == 0) {
            bounds = Rect{0, 0, 0, 0};
        }
        u32 first = 0;
        u32 prev = 0;
        for (u32 i = 0; i < set.count; ++i) {
            const u32 rr = g.exec_->alloc(RegionRectangle_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
            if (rr == 0) {
                for (u32 at = first; at != 0;) {
                    const u32 next = b.r32(at + rr_Next);
                    g.exec_->free(at, RegionRectangle_SIZEOF);
                    at = next;
                }
                return false;
            }
            const Rect& r = set.rects[i];
            write_rect(b, rr + rr_bounds,
                       Rect{r.x0 - bounds.x0, r.y0 - bounds.y0, r.x1 - bounds.x0, r.y1 - bounds.y0});
            b.w32(rr + rr_Prev, prev);
            if (prev != 0) {
                b.w32(prev + rr_Next, rr);
            } else {
                first = rr;
            }
            prev = rr;
        }
        clear_rects(g, b, region);
        write_rect(b, region + rg_bounds, bounds);
        b.w32(region + rg_RegionRectangle, first);
        return b.ok();
    }

    static void clear_rects(Graphics& g, Bus& b, u32 region) {
        u32 rr = b.r32(region + rg_RegionRectangle);
        for (u32 i = 0; rr != 0 && i < kMaxRegionRects && b.ok(); ++i) {
            const u32 next = b.r32(rr + rr_Next);
            g.exec_->free(rr, RegionRectangle_SIZEOF);
            rr = next;
        }
        b.w32(region + rg_RegionRectangle, 0);
    }

    static Rect rect_arg(Bus& b, u32 at) { return read_rect(b, at); }

    static void new_region(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).exec_->alloc(Region_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR));
    }

    static void dispose_region(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 region = m.cpu.a[0];
        if (region == 0) {
            return;
        }
        clear_rects(g, b, region);
        g.exec_->free(region, Region_SIZEOF);
    }

    static void clear_region(void* ctx, Machine& m) {
        Bus b(m);
        const u32 region = m.cpu.a[0];
        if (region == 0) {
            return;
        }
        clear_rects(of(ctx), b, region);
        write_rect(b, region + rg_bounds, Rect{0, 0, 0, 0});
    }

    // OrRectRegion, AndRectRegion, ClearRectRegion and XorRectRegion: a0
    // region, a1 rectangle; TRUE unless memory runs out.
    static void rect_op(void* ctx, Machine& m, int op) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 region = m.cpu.a[0];
        if (region == 0 || m.cpu.a[1] == 0) {
            native::set_result(m, 0);
            return;
        }
        const Rect r = rect_arg(b, m.cpu.a[1]);
        static constinit RectSet set{};
        set.overflow = false;
        if (!load_region(b, region, set)) {
            return;
        }
        switch (op) {
        case 0:  // or
            add_rect(set, r);
            break;
        case 1: {  // and
            static constinit RectSet kept{};
            kept.count = 0;
            kept.overflow = false;
            for (u32 i = 0; i < set.count; ++i) {
                kept.add(intersect(set.rects[i], r));
            }
            set = kept;
            break;
        }
        case 2:  // clear
            subtract_rect(set, r);
            break;
        default: {  // xor: what one of them covers and not the other
            static constinit RectSet outside{};
            outside.count = 0;
            outside.overflow = false;
            outside.add(r);
            for (u32 i = 0; i < set.count; ++i) {
                subtract_rect(outside, set.rects[i]);
            }
            subtract_rect(set, r);
            for (u32 i = 0; i < outside.count; ++i) {
                set.add(outside.rects[i]);
            }
            set.overflow |= outside.overflow;
            break;
        }
        }
        native::set_result(m, store_region(g, b, region, set) ? 1 : 0);
    }

    static void or_rect_region(void* ctx, Machine& m) { rect_op(ctx, m, 0); }
    static void and_rect_region(void* ctx, Machine& m) { rect_op(ctx, m, 1); }
    static void clear_rect_region(void* ctx, Machine& m) { rect_op(ctx, m, 2); }
    static void xor_rect_region(void* ctx, Machine& m) { rect_op(ctx, m, 3); }

    // OrRegionRegion and AndRegionRegion: a0 source, a1 destination.
    static void region_op(void* ctx, Machine& m, bool both) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 source = m.cpu.a[0];
        const u32 dest = m.cpu.a[1];
        if (source == 0 || dest == 0) {
            native::set_result(m, 0);
            return;
        }
        static constinit RectSet a{};
        static constinit RectSet d{};
        a.overflow = false;
        d.overflow = false;
        if (!load_region(b, source, a) || !load_region(b, dest, d)) {
            return;
        }
        if (both) {
            static constinit RectSet kept{};
            kept.count = 0;
            kept.overflow = false;
            for (u32 i = 0; i < d.count; ++i) {
                for (u32 j = 0; j < a.count; ++j) {
                    kept.add(intersect(d.rects[i], a.rects[j]));
                }
            }
            d = kept;
        } else {
            for (u32 j = 0; j < a.count; ++j) {
                add_rect(d, a.rects[j]);
            }
        }
        native::set_result(m, store_region(g, b, dest, d) ? 1 : 0);
    }

    static void or_region_region(void* ctx, Machine& m) { region_op(ctx, m, false); }
    static void and_region_region(void* ctx, Machine& m) { region_op(ctx, m, true); }

    // --- area fill -----------------------------------------------------------

    static void init_area(void*, Machine& m) {
        Bus b(m);
        const u32 ai = m.cpu.a[0];
        const u32 table = m.cpu.a[1];
        const u32 max = m.cpu.d[0] & 0xFFFF;
        if (ai == 0) {
            return;
        }
        b.w32(ai + ai_VctrTbl, table);
        b.w32(ai + ai_VctrPtr, table);
        b.w32(ai + ai_FlagTbl, table + max * 4);
        b.w32(ai + ai_FlagPtr, table + max * 4);
        b.w16(ai + ai_Count, 0);
        b.w16(ai + ai_MaxCount, max);
        b.w16(ai + ai_FirstX, 0);
        b.w16(ai + ai_FirstY, 0);
    }

    // Appends a vector to the AreaInfo of rp: 0, or -1 when it is full.
    static i32 add_vector(Bus& b, u32 rp, i32 x, i32 y, u8 kind) {
        const u32 ai = b.r32(rp + ndk::rp_AreaInfo);
        if (ai == 0 || !b.ok()) {
            return -1;
        }
        const u32 count = b.r16(ai + ai_Count);
        if (count >= b.r16(ai + ai_MaxCount)) {
            return -1;
        }
        const u32 table = b.r32(ai + ai_VctrTbl);
        const u32 flags = b.r32(ai + ai_FlagTbl);
        b.w16(table + count * 4, static_cast<u32>(x));
        b.w16(table + count * 4 + 2, static_cast<u32>(y));
        b.w8(flags + count, kind);
        b.w16(ai + ai_Count, count + 1);
        b.w32(ai + ai_VctrPtr, table + (count + 1) * 4);
        b.w32(ai + ai_FlagPtr, flags + count + 1);
        if (kind == kAreaMove) {
            b.w16(ai + ai_FirstX, static_cast<u32>(x));
            b.w16(ai + ai_FirstY, static_cast<u32>(y));
        }
        b.w16(rp + ndk::rp_cp_x, static_cast<u32>(x));
        b.w16(rp + ndk::rp_cp_y, static_cast<u32>(y));
        return b.ok() ? 0 : -1;
    }

    static void area_move(void*, Machine& m) {
        Bus b(m);
        native::set_result(m, static_cast<u32>(add_vector(b, m.cpu.a[1],
                                                          static_cast<i16>(m.cpu.d[0]),
                                                          static_cast<i16>(m.cpu.d[1]), kAreaMove)));
    }

    static void area_draw(void*, Machine& m) {
        Bus b(m);
        native::set_result(m, static_cast<u32>(add_vector(b, m.cpu.a[1],
                                                          static_cast<i16>(m.cpu.d[0]),
                                                          static_cast<i16>(m.cpu.d[1]), kAreaDraw)));
    }

    static void area_ellipse(void*, Machine& m) {
        Bus b(m);
        const u32 rp = m.cpu.a[1];
        const u32 ai = b.r32(rp + ndk::rp_AreaInfo);
        if (ai == 0 || b.r16(ai + ai_Count) + 2 > b.r16(ai + ai_MaxCount)) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        add_vector(b, rp, static_cast<i16>(m.cpu.d[0]), static_cast<i16>(m.cpu.d[1]),
                   kAreaEllipse);
        const i32 r = add_vector(b, rp, static_cast<i16>(m.cpu.d[2]),
                                 static_cast<i16>(m.cpu.d[3]), kAreaRadii);
        native::set_result(m, static_cast<u32>(r));
    }

    // Fills every polygon and ellipse gathered since the last AreaEnd with
    // the A pen, outlines them with the outline pen when AREAOUTLINE is
    // set, and empties the table.
    static void area_end(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 rp = m.cpu.a[1];
        const u32 ai = b.r32(rp + ndk::rp_AreaInfo);
        if (rp == 0 || ai == 0 || !b.ok()) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        const View v = raster::load_rastport(b, rp);
        u32 count = b.r16(ai + ai_Count);
        count = count > kMaxAreaVectors ? kMaxAreaVectors : count;
        const u32 table = b.r32(ai + ai_VctrTbl);
        const u32 flags = b.r32(ai + ai_FlagTbl);
        const u32 pen = b.r8(rp + ndk::rp_FgPen);
        const u32 outline = b.r8(rp + ndk::rp_AOlPen);
        const bool outlined = (b.r16(rp + ndk::rp_Flags) & ndk::AREAOUTLINE) != 0;
        Rect dirty{0, 0, -1, -1};
        i32 xs[kMaxPolygon];
        i32 ys[kMaxPolygon];
        u32 n = 0;
        auto flush = [&]() {
            if (n > 0 && v.ok) {
                fill_polygon(b, v, xs, ys, n, pen, dirty);
                if (outlined) {
                    outline_polygon(b, v, xs, ys, n, outline, dirty);
                }
            }
            n = 0;
        };
        for (u32 i = 0; i < count && b.ok(); ++i) {
            const u8 kind = static_cast<u8>(b.r8(flags + i));
            const i32 x = static_cast<i16>(b.r16(table + i * 4)) + v.origin_x;
            const i32 y = static_cast<i16>(b.r16(table + i * 4 + 2)) + v.origin_y;
            if (kind == kAreaEllipse && i + 1 < count) {
                flush();
                const i32 ra = static_cast<i16>(b.r16(table + (i + 1) * 4));
                const i32 rb = static_cast<i16>(b.r16(table + (i + 1) * 4 + 2));
                if (v.ok) {
                    ellipse_rows(x, y, ra, rb, [&](i32 row, i32 x0, i32 x1) {
                        fill_span(b, v, row, x0, x1, pen, dirty);
                    });
                    if (outlined) {
                        ellipse_rows(x, y, ra, rb, [&](i32 row, i32 x0, i32 x1) {
                            fill_span(b, v, row, x0, x0, outline, dirty);
                            fill_span(b, v, row, x1, x1, outline, dirty);
                        });
                    }
                }
                ++i;
                continue;
            }
            if (kind == kAreaMove) {
                flush();
            }
            if (n < kMaxPolygon) {
                xs[n] = x;
                ys[n] = y;
                ++n;
            }
        }
        flush();
        b.w16(ai + ai_Count, 0);
        b.w32(ai + ai_VctrPtr, table);
        b.w32(ai + ai_FlagPtr, flags);
        if (b.ok() && v.ok) {
            report(g.display_, m, v, dirty);
        }
        native::set_result(m, 0);
    }

    // The outline of an ellipse in the A pen.
    static void draw_ellipse(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 rp = m.cpu.a[1];
        const View v = raster::load_rastport(b, rp);
        if (!v.ok) {
            return;
        }
        const i32 cx = static_cast<i16>(m.cpu.d[0]) + v.origin_x;
        const i32 cy = static_cast<i16>(m.cpu.d[1]) + v.origin_y;
        const i32 ra = static_cast<i16>(m.cpu.d[2]);
        const i32 rb = static_cast<i16>(m.cpu.d[3]);
        const u32 pen = b.r8(rp + ndk::rp_FgPen);
        Rect dirty{0, 0, -1, -1};
        i32 prev_left = 0;
        i32 prev_right = 0;
        bool first = true;
        ellipse_rows(cx, cy, ra, rb, [&](i32 row, i32 x0, i32 x1) {
            if (first || row == cy + rb) {
                // The top and bottom rows are drawn whole.
                fill_span(b, v, row, x0, x1, pen, dirty);
            } else {
                // Each side runs from this row's edge to the one above, so a
                // steep part of the outline has no gaps.
                fill_span(b, v, row, x0 < prev_left ? x0 : prev_left,
                          x0 < prev_left ? prev_left - 1 : x0, pen, dirty);
                fill_span(b, v, row, x1 > prev_right ? prev_right + 1 : x1,
                          x1 > prev_right ? x1 : prev_right, pen, dirty);
            }
            prev_left = x0;
            prev_right = x1;
            first = false;
        });
        if (b.ok()) {
            report(g.display_, m, v, dirty);
        }
    }

    // Drawing finishes before each call returns: there is no blitter to
    // wait for.
    static void wait_blit(void*, Machine&) {}

    // BltBitMapRastPort(src A0, x D0, y D1, rp A1, x D2, y D3, width D4,
    // height D5, minterm D6).
    static void blt_bitmap_rastport(void* ctx, Machine& m) {
        const auto word = [&](u32 r) { return static_cast<i32>(static_cast<i16>(r & 0xFFFF)); };
        of(ctx).blt_bitmap_rastport(m.cpu.a[0], word(m.cpu.d[0]), word(m.cpu.d[1]), m.cpu.a[1],
                                    word(m.cpu.d[2]), word(m.cpu.d[3]), word(m.cpu.d[4]),
                                    word(m.cpu.d[5]), m.cpu.d[6] & 0xFF);
    }

    // BltBitMap(src A0, x D0, y D1, dst A1, x D2, y D3, width D4, height
    // D5, minterm D6, mask D7, tempA A2): WORD positions and sizes.
    static void blt_bitmap(void* ctx, Machine& m) {
        const auto word = [&](u32 r) { return static_cast<i32>(static_cast<i16>(r & 0xFFFF)); };
        native::set_result(m, of(ctx).blt_bitmap(m.cpu.a[0], word(m.cpu.d[0]), word(m.cpu.d[1]),
                                                 m.cpu.a[1], word(m.cpu.d[2]), word(m.cpu.d[3]),
                                                 word(m.cpu.d[4]), word(m.cpu.d[5]),
                                                 m.cpu.d[6] & 0xFF, m.cpu.d[7] & 0xFF));
    }

    // BltClear: clears the memory block, D0 bytes long, or, with bit 1 of
    // the flags in D1, D0's upper word rows of its lower word bytes. The
    // blit is always done when it returns.
    static void blt_clear(void*, Machine& m) {
        const u32 count = m.cpu.d[0];
        const u32 bytes = (m.cpu.d[1] & 2) != 0 ? (count >> 16) * (count & 0xFFFF) : count;
        Bus b(m);
        const u32 memory = m.cpu.a[1];
        for (u32 i = 0; i < bytes && b.ok(); ++i) {
            b.w8(memory + i, 0);
        }
    }

    // A TmpRas only records its buffer: fills need no scratch plane here.
    static void init_tmp_ras(void*, Machine& m) {
        Bus b(m);
        const u32 tr = m.cpu.a[0];
        if (tr != 0) {
            b.w32(tr, m.cpu.a[1]);
            b.w32(tr + 4, m.cpu.d[0]);
        }
        native::set_result(m, tr);
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
        const View v = raster::load_rastport(b, rp);
        if (!v.ok) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        const i32 x = static_cast<i16>(m.cpu.d[0]) + v.origin_x;
        const i32 y = static_cast<i16>(m.cpu.d[1]) + v.origin_y;
        if (!drawable(b, v, x, y)) {
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
        const View v = raster::load_rastport(b, rp);
        if (!v.ok) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        const i32 x = static_cast<i16>(m.cpu.d[0]) + v.origin_x;
        const i32 y = static_cast<i16>(m.cpu.d[1]) + v.origin_y;
        if (!drawable(b, v, x, y)) {
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
            bump_palette(g.display_);
            mark_viewport_dirty(g.display_, m, vp);
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
        // Guest addresses wrap, so a table without its terminator would never
        // fault: the records and the entries read from each are both capped.
        constexpr u32 kMaxLoadRecords = 256;
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
            const u32 room = first < t.limit ? t.limit - first : 0;
            const u32 used = count < room ? count : room;
            const u32 next = p + count * 12;
            for (u32 i = 0; i < used; ++i) {
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
            p = next;
        }
        if (changed && b.ok()) {
            bump_palette(g.display_);
            mark_viewport_dirty(g.display_, m, vp);
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

    static void set_rgb32_cm(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const ColorMapTables t = colormap_tables(b, m.cpu.a[0]);
        if (write_rgb32_entry(b, t, m.cpu.d[0], m.cpu.d[1], m.cpu.d[2], m.cpu.d[3])) {
            bump_palette(g.display_);
            mark_colormap_dirty(g.display_, m, m.cpu.a[0]);
        }
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
    {graphics_lvo::TextExtent, GraphicsNatives::text_extent, "TextExtent"},
    {graphics_lvo::DrawEllipse, GraphicsNatives::draw_ellipse, "DrawEllipse"},
    {graphics_lvo::AreaEllipse, GraphicsNatives::area_ellipse, "AreaEllipse"},
    {graphics_lvo::AreaMove, GraphicsNatives::area_move, "AreaMove"},
    {graphics_lvo::AreaDraw, GraphicsNatives::area_draw, "AreaDraw"},
    {graphics_lvo::AreaEnd, GraphicsNatives::area_end, "AreaEnd"},
    {graphics_lvo::InitArea, GraphicsNatives::init_area, "InitArea"},
    {graphics_lvo::InitTmpRas, GraphicsNatives::init_tmp_ras, "InitTmpRas"},
    {graphics_lvo::WaitBlit, GraphicsNatives::wait_blit, "WaitBlit"},
    {graphics_lvo::BltBitMap, GraphicsNatives::blt_bitmap, "BltBitMap"},
    {graphics_lvo::BltBitMapRastPort, GraphicsNatives::blt_bitmap_rastport,
     "BltBitMapRastPort"},
    {graphics_lvo::BltClear, GraphicsNatives::blt_clear, "BltClear"},
    {graphics_lvo::NewRegion, GraphicsNatives::new_region, "NewRegion"},
    {graphics_lvo::DisposeRegion, GraphicsNatives::dispose_region, "DisposeRegion"},
    {graphics_lvo::ClearRegion, GraphicsNatives::clear_region, "ClearRegion"},
    {graphics_lvo::OrRectRegion, GraphicsNatives::or_rect_region, "OrRectRegion"},
    {graphics_lvo::AndRectRegion, GraphicsNatives::and_rect_region, "AndRectRegion"},
    {graphics_lvo::ClearRectRegion, GraphicsNatives::clear_rect_region, "ClearRectRegion"},
    {graphics_lvo::XorRectRegion, GraphicsNatives::xor_rect_region, "XorRectRegion"},
    {graphics_lvo::OrRegionRegion, GraphicsNatives::or_region_region, "OrRegionRegion"},
    {graphics_lvo::AndRegionRegion, GraphicsNatives::and_region_region, "AndRegionRegion"},
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

u32 Graphics::blt_bitmap(u32 src_addr, i32 sx, i32 sy, u32 dst_addr, i32 dx, i32 dy,
                         i32 width, i32 height, u32 minterm, u32 mask) {
    Bus b(*machine_);
    raster::Bitmap dst{};
    if (dst_addr == 0 || !raster::load_bitmap(b, dst_addr, dst)) {
        return 0;
    }
    View v{};
    v.bitmap = dst;
    v.bitmap_addr = dst_addr;
    return blit(b, src_addr, sx, sy, v, nullptr, dx, dy, width, height, minterm, mask);
}

// BltBitMapRastPort: the same into the RastPort's bitmap, inside its
// layer and clip, at the RastPort's coordinates.
void Graphics::blt_bitmap_rastport(u32 src_addr, i32 sx, i32 sy, u32 rp, i32 dx, i32 dy,
                                   i32 width, i32 height, u32 minterm) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    const View v = raster::load_rastport(b, rp);
    if (!v.ok) {
        return;
    }
    blit(b, src_addr, sx, sy, v, &v.clip, dx + v.origin_x, dy + v.origin_y, width, height,
         minterm, b.r8(rp + ndk::rp_Mask));
}

u32 Graphics::blit(Bus& b, u32 src_addr, i32 sx, i32 sy, const View& v, const raster::Clip* clip,
                   i32 dx, i32 dy, i32 width, i32 height, u32 minterm, u32 mask) {
    raster::Bitmap src{};
    if (src_addr == 0 || !raster::load_bitmap(b, src_addr, src)) {
        return 0;
    }
    const raster::Bitmap& dst = v.bitmap;
    // Both rectangles inside their bitmaps.
    if (sx < 0) {
        width += sx;
        dx -= sx;
        sx = 0;
    }
    if (sy < 0) {
        height += sy;
        dy -= sy;
        sy = 0;
    }
    if (dx < 0) {
        width += dx;
        sx -= dx;
        dx = 0;
    }
    if (dy < 0) {
        height += dy;
        sy -= dy;
        dy = 0;
    }
    const i32 src_w = static_cast<i32>(src.bytes_per_row * 8);
    const i32 dst_w = static_cast<i32>(dst.bytes_per_row * 8);
    width = sx + width > src_w ? src_w - sx : width;
    width = dx + width > dst_w ? dst_w - dx : width;
    height = sy + height > static_cast<i32>(src.rows) ? static_cast<i32>(src.rows) - sy : height;
    height = dy + height > static_cast<i32>(dst.rows) ? static_cast<i32>(dst.rows) - dy : height;
    if (width <= 0 || height <= 0) {
        return 0;
    }
    // With A always set, the minterm's upper four bits give the result for
    // each pair of source (B) and destination (C) bits.
    static_assert(sizeof blit_row_ >= raster::kMaxBytesPerRow * 8);
    const u32 planes = src.depth < dst.depth ? src.depth : dst.depth;
    u32 written = 0;
    u8* row = blit_row_;
    for (u32 plane = 0; plane < planes; ++plane) {
        if ((mask & (1u << plane)) == 0) {
            continue;
        }
        ++written;
        // A copy down the same plane goes from the bottom, so no row is
        // read after it was written.
        const bool upward = src.planes[plane] == dst.planes[plane] && dy > sy;
        for (i32 n = 0; n < height && b.ok(); ++n) {
            const i32 r = upward ? height - 1 - n : n;
            const u32 src_row = src.planes[plane] + static_cast<u32>(sy + r) * src.bytes_per_row;
            const u32 dst_row = dst.planes[plane] + static_cast<u32>(dy + r) * dst.bytes_per_row;
            for (i32 x = 0; x < width; ++x) {
                const u32 at = static_cast<u32>(sx + x);
                row[x] = static_cast<u8>((b.r8(src_row + (at >> 3)) >> (7 - (at & 7))) & 1);
            }
            for (i32 x = 0; x < width; ++x) {
                if (clip != nullptr && !raster::in_clip(b, *clip, dx + x, dy + r)) {
                    continue;
                }
                const u32 at = static_cast<u32>(dx + x);
                const u32 byte = b.r8(dst_row + (at >> 3));
                const u32 bit = 0x80u >> (at & 7);
                const u32 c = (byte & bit) != 0 ? 1 : 0;
                const u32 result = (minterm >> (4 + 2 * row[x] + c)) & 1;
                if (result != c) {
                    b.w8(dst_row + (at >> 3), result != 0 ? byte | bit : byte & ~bit);
                }
            }
        }
    }
    if (b.ok()) {
        report(display_, *machine_, v, Rect{dx, dy, dx + width - 1, dy + height - 1});
    }
    return written;
}

void Graphics::rect_fill(u32 rp, i32 x0, i32 y0, i32 x1, i32 y1) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    const View v = raster::load_rastport(b, rp);
    if (!v.ok) {
        return;
    }
    const Rect area{x0 + v.origin_x, y0 + v.origin_y, x1 + v.origin_x, y1 + v.origin_y};
    const raster::Pens p = pens(b, rp);
    // COMPLEMENT turns over the pen bits the RastPort's mask lets through.
    const Rect dirty = (p.mode & ~ndk::INVERSVID) == ndk::COMPLEMENT
                           ? raster::complement_rect(b, v.bitmap, v.clip, area,
                                                     b.r8(rp + ndk::rp_Mask))
                           : raster::fill_rect(b, v.bitmap, v.clip, area, p.fg);
    if (b.ok()) {
        report(display_, *machine_, v, dirty);
    }
}

void Graphics::draw_line(u32 rp, i32 x1, i32 y1) {
    if (rp == 0) {
        return;
    }
    Bus b(*machine_);
    const View v = raster::load_rastport(b, rp);
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
    const View v = raster::load_rastport(b, rp);
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
    const View v = raster::load_rastport(b, rp);
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
