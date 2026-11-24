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

#include "chrome_palette.hpp"
#include "display_modes.hpp"
#include "tags.hpp"

#include "exec.hpp"
#include "raster_text.hpp"
#include "rom_fonts.hpp"
#include "version.hpp"

#include <algorithm>

namespace hombre {

using m68k::Machine;
using native::Bus;
using i16 = __INT16_TYPE__;

namespace {

// BitMap flags (graphics/gfx.h) and the CyberGraphX pixel formats a
// BMF_SPECIALFMT AllocBitMap names in its top byte (cybergraphx).
constexpr u32 kBmfStandard = 1u << 3;
constexpr u32 kBmfSpecialFmt = 1u << 7;
constexpr u32 kPixFmtLut8 = 0;
constexpr u32 kPixFmtRgb16 = 5;
constexpr u32 kPixFmtArgb32 = 11;
// The depth a deep bitmap reports, by PixFmt.
constexpr u32 kDeepDepth[] = {8, 16, 24};

constexpr char kGraphicsName[] = "graphics.library";
constexpr char kTopazName[] = "topaz.font";
// struct GfxBase (graphics/gfxbase.h): its size as of V40, the list of the
// fonts in memory, which diskfont.library's AvailFonts walks, and the font
// InitRastPort gives a RastPort.
constexpr u32 kGfxBaseSize = 0x228;
constexpr u32 gb_TextFonts = 140;
constexpr u32 gb_DefaultFont = 154;
constexpr u32 kNtFont = 12;
constexpr u32 kMaxFonts = 1024;
constexpr i32 kExactFontMatch = 32767;  // MAXFONTMATCHWEIGHT
constexpr u32 kMaxTextChars = 65536;

// The BitMap, the clip and the origin of a RastPort's drawing.
using View = raster::RastPortView;

raster::Pens pens(Bus& b, u32 rp) {
    return raster::Pens{static_cast<u8>(b.r8(rp + ndk::rp_FgPen)),
                        static_cast<u8>(b.r8(rp + ndk::rp_BgPen)),
                        static_cast<u8>(b.r8(rp + ndk::rp_DrawMode))};
}

bool same_string(const char* one, const char* other) {
    for (; *one == *other; ++one, ++other) {
        if (*one == 0) {
            return true;
        }
    }
    return false;
}

// A copy of the string in exec's memory, 0 if it has none left.
u32 guest_string(Exec& exec, Bus& b, const char* text) {
    u32 bytes = 1;
    while (text[bytes - 1] != 0) {
        ++bytes;
    }
    const u32 copy = exec.alloc(bytes, ndk::MEMF_PUBLIC);
    for (u32 i = 0; copy != 0 && i < bytes; ++i) {
        b.w8(copy + i, static_cast<u8>(text[i]));
    }
    return copy;
}

i32 cp_x(Bus& b, u32 rp) { return static_cast<i16>(b.r16(rp + ndk::rp_cp_x)); }

i32 cp_y(Bus& b, u32 rp) { return static_cast<i16>(b.r16(rp + ndk::rp_cp_y)); }

bool drawable(Bus& b, const View& v, i32 x, i32 y) {
    return x >= 0 && y >= 0 && x < raster::pixel_width(v.bitmap) &&
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

// Reads or writes a width by height block of pens, one byte a pen, rows
// modulo bytes apart, at x, y of the RastPort. Pixels outside the bitmap or
// its clip are skipped; read ones there are left as they are in the array.
// The number of pixels the block holds.
u32 pixel_array(Bus& b, const DisplayState* display, Machine& m, u32 rp, i32 x0, i32 y0,
                i32 width, i32 height, u32 array, u32 modulo, bool write) {
    if (rp == 0 || array == 0 || width <= 0 || height <= 0) {
        return 0;
    }
    const View v = raster::load_rastport(b, rp);
    if (!v.ok) {
        return 0;
    }
    Rect changed{0, 0, -1, -1};
    for (i32 row = 0; row < height && b.ok(); ++row) {
        u8* bytes = b.bytes_at(array + static_cast<u32>(row) * modulo, static_cast<u32>(width));
        if (bytes == nullptr) {
            break;
        }
        const i32 y = y0 + row + v.origin_y;
        for (i32 col = 0; col < width; ++col) {
            const i32 x = x0 + col + v.origin_x;
            if (!drawable(b, v, x, y)) {
                continue;
            }
            if (write) {
                raster::write_pixel(b, v.bitmap, x, y, bytes[col]);
                changed = unite(changed, Rect{x, y, x, y});
            } else {
                bytes[col] = static_cast<u8>(raster::read_pixel(b, v.bitmap, x, y));
            }
        }
    }
    if (write && changed.x1 >= changed.x0) {
        report(display, m, v, changed);
    }
    return static_cast<u32>(width) * static_cast<u32>(height);
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

// The ellipses one AreaEnd fills together.
struct AreaEllipseShape {
    i32 cx, cy, a, b;
};
constexpr u32 kMaxAreaEllipses = 64;

// Fills the ellipses together by the even-odd rule, as one area: an
// ellipse inside another leaves a hole, so two make a ring.
void fill_ellipses(Bus& b, const View& v, const AreaEllipseShape* shapes, u32 count, u32 pen,
                   Rect& dirty) {
    if (count == 0) {
        return;
    }
    i32 top = shapes[0].cy - shapes[0].b;
    i32 bottom = shapes[0].cy + shapes[0].b;
    for (u32 i = 1; i < count; ++i) {
        top = shapes[i].cy - shapes[i].b < top ? shapes[i].cy - shapes[i].b : top;
        bottom = shapes[i].cy + shapes[i].b > bottom ? shapes[i].cy + shapes[i].b : bottom;
    }
    top = top < v.clip.y0 ? v.clip.y0 : top;
    bottom = bottom > v.clip.y1 ? v.clip.y1 : bottom;
    for (i32 y = top; y <= bottom; ++y) {
        // Each ellipse the row crosses gives where its span starts and
        // where the pixel after it is; filling between alternate ones is
        // the even-odd rule.
        i32 edges[kMaxAreaEllipses * 2];
        u32 n = 0;
        for (u32 i = 0; i < count; ++i) {
            const AreaEllipseShape& e = shapes[i];
            const i32 dy = y - e.cy;
            if (dy < -e.b || dy > e.b) {
                continue;
            }
            i32 x = e.a;
            if (e.b != 0) {
                const i64 a2 = static_cast<i64>(e.a) * e.a;
                const i64 b2 = static_cast<i64>(e.b) * e.b;
                const i64 rest = a2 * (b2 - static_cast<i64>(dy) * dy);
                i64 w = 0;
                while ((w + 1) * (w + 1) * b2 <= rest) {
                    ++w;
                }
                x = static_cast<i32>(w);
            }
            edges[n++] = e.cx - x;
            edges[n++] = e.cx + x + 1;
        }
        for (u32 i = 1; i < n; ++i) {
            const i32 key = edges[i];
            u32 k = i;
            while (k > 0 && edges[k - 1] > key) {
                edges[k] = edges[k - 1];
                --k;
            }
            edges[k] = key;
        }
        for (u32 i = 0; i + 1 < n; i += 2) {
            fill_span(b, v, y, edges[i], edges[i + 1] - 1, pen, dirty);
        }
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

namespace {

// struct TextFontExtension (graphics/text.h), which ExtendFont hangs on a
// font's tf_Extension.
constexpr u32 kTfeMatchWord = 0;
constexpr u32 kTfeBackPtr = 4;
constexpr u32 kTfeOrigReplyPort = 8;
constexpr u32 kTfeTags = 12;
constexpr u32 kTfeSize = 24;
constexpr u32 kTeMatchWord = 0xF100;
constexpr u32 kFsfTagged = 0x40;
constexpr u32 kMaxFontTags = 64;
constexpr u32 kSoftStyles = 0x07;  // FSF_UNDERLINED, FSF_BOLD and FSF_ITALIC

// The font's extension, 0 when it has none: FSF_TAGGED set and an
// extension that points back at the font.
u32 font_extension(Bus& b, u32 font) {
    if ((b.r8(font + ndk::tf_Style) & kFsfTagged) == 0) {
        return 0;
    }
    const u32 tfe = b.r32(font + ndk::mn_ReplyPort);
    if (tfe == 0 || (tfe & 1) != 0 || b.r16(tfe + kTfeMatchWord) != kTeMatchWord ||
        b.r32(tfe + kTfeBackPtr) != font || !b.ok()) {
        return 0;
    }
    return tfe;
}

// The data of tag in the font's extension, 0 when the font has no
// extension or no such tag. The tag list is walked the way ExtendFont
// copies it, TAG_MORE and TAG_SKIP included.
u32 extension_tag(Bus& b, u32 font, u32 wanted) {
    const u32 tfe = font_extension(b, font);
    u32 tags = tfe != 0 ? b.r32(tfe + kTfeTags) : 0;
    for (u32 step = 0; tags != 0 && step < 4 * kMaxFontTags; ++step) {
        const u32 tag = b.r32(tags);
        const u32 data = b.r32(tags + 4);
        if (!b.ok() || tag == 0) {
            break;
        }
        if (tag == 2) {  // TAG_MORE
            tags = data;
            continue;
        }
        if (tag == 3) {  // TAG_SKIP
            tags += 8 * (data + 1);
            continue;
        }
        if (tag == wanted) {
            return data;
        }
        tags += 8;
    }
    return 0;
}

// Two names of the guest's alike, letter case aside.
bool names_equal_ci(Bus& b, u32 one, u32 other) {
    if (one == 0 || other == 0) {
        return false;
    }
    for (u32 i = 0; i < 256; ++i) {
        u32 c = b.r8(one + i);
        u32 d = b.r8(other + i);
        if (!b.ok()) {
            return false;
        }
        c = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        d = d >= 'A' && d <= 'Z' ? d + ('a' - 'A') : d;
        if (c != d) {
            return false;
        }
        if (c == 0) {
            return true;
        }
    }
    return false;
}

// How well a font answers a request: dy is how far their heights are
// apart, style and flags the bits their styles and flags differ in.
// kExactFontMatch when they are the same, less for each way they differ,
// the height most. Only the styles a font is drawn in and the flags that
// tell its shape count: where a font came from, and whether it carries an
// extension, makes it no worse a match.
i32 font_match_weight(i32 dy, u32 style, u32 flags) {
    constexpr u32 kDrawnStyles = 0x0F;  // underlined, bold, italic, extended
    constexpr u32 kShapeFlags = 0x3C;   // REVPATH, TALLDOT, WIDEDOT, PROPORTIONAL
    style &= kDrawnStyles;
    flags &= kShapeFlags;
    i32 weight = kExactFontMatch;
    if (dy != 0) {
        weight -= 1024 + (dy < 0 ? -dy : dy) * 512;
    }
    for (u32 bit = 0; bit < 8; ++bit) {
        weight -= (style >> bit) & 1 ? 256 : 0;
        weight -= (flags >> bit) & 1 ? 64 : 0;
    }
    return weight < 1 ? 1 : weight;
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

    // TextLength(rp A1, string A0, count D0): how far Text would move the
    // pen for the characters.
    static void text_length(void* ctx, Machine& m) {
        Bus b(m);
        const u32 count = m.cpu.d[0] & 0xFFFF;
        const u8* chars = b.bytes_at(m.cpu.a[0], count);
        raster::Font font;
        raster::TextStyle style;
        if (chars == nullptr || !of(ctx).text_font(b, m.cpu.a[1], font, style)) {
            return;
        }
        native::set_result(
            m, static_cast<u32>(raster::measure_text(font, style, chars, count).advance));
    }

    // struct TextExtent for a span of text in the font: te_Width, the pen's
    // advance, te_Height, then te_Extent, the rectangle of its pixels
    // relative to the pen position.
    static void write_extent(Bus& b, u32 extent, const raster::Font& font,
                             const raster::TextSpan& span) {
        b.w16(extent + 0, static_cast<u32>(span.advance));
        b.w16(extent + 2, static_cast<u32>(font.y_size));
        b.w16(extent + 4, static_cast<u32>(span.min_x));
        b.w16(extent + 6, static_cast<u32>(-font.baseline));
        b.w16(extent + 8, static_cast<u32>(span.max_x < span.min_x ? span.min_x : span.max_x));
        b.w16(extent + 10, static_cast<u32>(font.y_size - font.baseline - 1));
    }

    // TextExtent(rp A1, string A0, count D0, textExtent A2).
    static void text_extent(void* ctx, Machine& m) {
        Bus b(m);
        const u32 extent = m.cpu.a[2];
        const u32 count = m.cpu.d[0] & 0xFFFF;
        if (extent == 0) {
            return;
        }
        const u8* chars = b.bytes_at(m.cpu.a[0], count);
        raster::Font font;
        raster::TextStyle style;
        if (chars == nullptr || !of(ctx).text_font(b, m.cpu.a[1], font, style)) {
            return;
        }
        write_extent(b, extent, font, raster::measure_text(font, style, chars, count));
    }

    // TextFit(rp A1, string A0, length D0, extent A2, limit A3, direction
    // D1, width D2, height D3): how many characters, from the start of the
    // string or with a negative direction from its end, fit in width by
    // height pixels, with their TextExtent at A2.
    static void text_fit(void* ctx, Machine& m) {
        Bus b(m);
        const u32 length = m.cpu.d[0] & 0xFFFF;
        u32 width = m.cpu.d[2] & 0xFFFF;
        u32 height = m.cpu.d[3] & 0xFFFF;
        // A constraining TextExtent limits the text to its box as well.
        const u32 limit = m.cpu.a[3];
        if (limit != 0) {
            const i32 box_w = static_cast<i16>(b.r16(limit + 8)) -
                              static_cast<i16>(b.r16(limit + 4)) + 1;
            const i32 box_h = static_cast<i16>(b.r16(limit + 10)) -
                              static_cast<i16>(b.r16(limit + 6)) + 1;
            const u32 w = box_w > 0 ? static_cast<u32>(box_w) : 0;
            const u32 h = box_h > 0 ? static_cast<u32>(box_h) : 0;
            width = width == 0 || w < width ? w : width;
            height = height == 0 || h < height ? h : height;
        }
        const u8* chars = b.bytes_at(m.cpu.a[0], length);
        raster::Font font;
        raster::TextStyle style;
        if (chars == nullptr || !of(ctx).text_font(b, m.cpu.a[1], font, style)) {
            return;
        }
        const bool from_end = static_cast<i16>(m.cpu.d[1]) < 0;
        raster::TextSpan fitted{};
        u32 fit = 0;
        for (; fit < length && height >= static_cast<u32>(font.y_size); ++fit) {
            raster::TextSpan longer = fitted;
            raster::extend_span(longer, font, style, chars[from_end ? length - 1 - fit : fit]);
            const raster::TextSpan drawn = raster::styled_span(longer, font, style);
            if (drawn.max_x - drawn.min_x + 1 > static_cast<i64>(width)) {
                break;
            }
            fitted = longer;
        }
        const u32 extent = m.cpu.a[2];
        if (extent != 0) {
            write_extent(b, extent, font, raster::styled_span(fitted, font, style));
        }
        if (b.ok()) {
            native::set_result(m, fit);
        }
    }

    // Text(rp A1, string A0, count D0).
    static void text(void* ctx, Machine& m) {
        Bus b(m);
        const u32 rp = m.cpu.a[1];
        const u32 addr = m.cpu.a[0];
        const u32 count = m.cpu.d[0] < kMaxTextChars ? m.cpu.d[0] : kMaxTextChars;
        if (rp == 0 || addr == 0 || count == 0) {
            return;
        }
        const u8* chars = b.bytes_at(addr, count);
        if (chars != nullptr) {
            of(ctx).draw_text(rp, chars, count, true);
        }
    }

    static void set_font(void* ctx, Machine& m) { of(ctx).set_font(m.cpu.a[1], m.cpu.a[0]); }

    // OpenFont(textAttr A0): the font in memory of the name asked for
    // that comes nearest the height, style and flags asked for, with one
    // more accessor; 0 when no font in memory has the name.
    static void open_font(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).open_font(m.cpu.a[0]));
    }

    // CloseFont(textFont A1): one accessor fewer. The font stays in memory.
    static void close_font(void* ctx, Machine& m) { of(ctx).close_font(m.cpu.a[1]); }

    // AddFont(textFont A1): the font, which its program loaded and keeps
    // in memory, joins the fonts OpenFont finds, extended as ExtendFont
    // extends one.
    static void add_font(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 font = m.cpu.a[1];
        if (font == 0) {
            return;
        }
        const u32 fonts = g.base_ + gb_TextFonts;
        const u32 head = b.r32(fonts + ndk::LH_HEAD);
        b.w8(font + ndk::LN_TYPE, kNtFont);
        b.w8(font + ndk::tf_Flags, b.r8(font + ndk::tf_Flags) & ~ndk::FPF_REMOVED & 0xFF);
        b.w16(font + ndk::tf_Accessors, 0);
        b.w32(font + ndk::LN_SUCC, head);
        b.w32(font + ndk::LN_PRED, fonts + ndk::LH_HEAD);
        b.w32(head + ndk::LN_PRED, font);
        b.w32(fonts + ndk::LH_HEAD, font);
        if (b.ok()) {
            extend(g, b, font, 0);
        }
    }

    // RemFont(textFont A1): the font leaves the fonts OpenFont finds and
    // is marked FPF_REMOVED; those that have it open keep using it.
    static void rem_font(void*, Machine& m) {
        Bus b(m);
        const u32 font = m.cpu.a[1];
        if (font == 0 || (b.r8(font + ndk::tf_Flags) & ndk::FPF_REMOVED) != 0) {
            return;
        }
        const u32 next = b.r32(font + ndk::LN_SUCC);
        const u32 previous = b.r32(font + ndk::LN_PRED);
        if (next == 0 || previous == 0 || !b.ok()) {
            return;
        }
        b.w32(previous + ndk::LN_SUCC, next);
        b.w32(next + ndk::LN_PRED, previous);
        b.w8(font + ndk::tf_Flags, b.r8(font + ndk::tf_Flags) | ndk::FPF_REMOVED);
    }

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
            g.palettes_changed();
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

    // PolyDraw(rp A1, count D0, array A0): lines from the pen position
    // through count points, WORD x and y pairs.
    static void poly_draw(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 rp = m.cpu.a[1];
        const u32 array = m.cpu.a[0];
        const u32 count = m.cpu.d[0] & 0xFFFF;
        for (u32 i = 0; i < count && b.ok(); ++i) {
            const i32 x = static_cast<i16>(b.r16(array + i * 4));
            const i32 y = static_cast<i16>(b.r16(array + i * 4 + 2));
            if (b.ok()) {
                g.draw_line(rp, x, y);
            }
        }
    }

    // ClipBlit(src rp A0, x D0, y D1, dst rp A1, x D2, y D3, width D4,
    // height D5, minterm D6): from the source rastport's bitmap, offset by
    // its layer. Parts of the source that other windows cover are copied
    // as they are on the screen.
    static void clip_blit(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const auto word = [&](u32 r) { return static_cast<i32>(static_cast<i16>(r & 0xFFFF)); };
        const u32 src = m.cpu.a[0];
        const u32 bitmap = b.r32(src + ndk::rp_BitMap);
        const u32 layer = b.r32(src + ndk::rp_Layer);
        i32 x = word(m.cpu.d[0]);
        i32 y = word(m.cpu.d[1]);
        // A layer over a bitmap of its own, as a window's is, starts at that
        // bitmap's corner; any other sits at its place on the screen.
        if (layer != 0 && b.r32(layer + ndk::ly_SuperBitMap) != bitmap) {
            x += static_cast<i16>(b.r16(layer + ndk::ly_MinX));
            y += static_cast<i16>(b.r16(layer + ndk::ly_MinY));
        }
        if (!b.ok() || bitmap == 0) {
            return;
        }
        g.blt_bitmap_rastport(bitmap, x, y, m.cpu.a[1], word(m.cpu.d[2]), word(m.cpu.d[3]),
                              word(m.cpu.d[4]), word(m.cpu.d[5]), m.cpu.d[6] & 0xFF);
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
            g.palettes_changed();
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
        AreaEllipseShape ellipses[kMaxAreaEllipses];
        u32 ellipse_count = 0;
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
                if (ellipse_count < kMaxAreaEllipses && ra >= 0 && rb >= 0) {
                    ellipses[ellipse_count++] = AreaEllipseShape{x, y, ra, rb};
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
        if (v.ok) {
            fill_ellipses(b, v, ellipses, ellipse_count, pen, dirty);
            for (u32 e = 0; outlined && e < ellipse_count; ++e) {
                const AreaEllipseShape& s = ellipses[e];
                ellipse_rows(s.cx, s.cy, s.a, s.b, [&](i32 row, i32 x0, i32 x1) {
                    fill_span(b, v, row, x0, x0, outline, dirty);
                    fill_span(b, v, row, x1, x1, outline, dirty);
                });
            }
        }
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

    // WritePixelArray8 and ReadPixelArray8(rp A0, xstart D0, ystart D1,
    // xstop D2, ystop D3, array A2, temprp A1): a block of pens, rows padded
    // to a multiple of 16; the pixels it holds.
    static void pixel_array8(void* ctx, Machine& m, bool write) {
        Graphics& g = of(ctx);
        Bus b(m);
        const i32 x0 = static_cast<i32>(m.cpu.d[0] & 0xFFFF);
        const i32 y0 = static_cast<i32>(m.cpu.d[1] & 0xFFFF);
        const i32 width = static_cast<i32>(m.cpu.d[2] & 0xFFFF) - x0 + 1;
        const i32 height = static_cast<i32>(m.cpu.d[3] & 0xFFFF) - y0 + 1;
        const u32 modulo = width > 0 ? (static_cast<u32>(width) + 15) & ~15u : 0;
        native::set_result(m, pixel_array(b, g.display_, m, m.cpu.a[0], x0, y0, width, height,
                                          m.cpu.a[2], modulo, write));
    }

    static void write_pixel_array8(void* ctx, Machine& m) { pixel_array8(ctx, m, true); }
    static void read_pixel_array8(void* ctx, Machine& m) { pixel_array8(ctx, m, false); }

    // WritePixelLine8 and ReadPixelLine8(rp A0, xstart D0, ystart D1,
    // width D2, array A2, temprp A1): one row of pens.
    static void pixel_line8(void* ctx, Machine& m, bool write) {
        Graphics& g = of(ctx);
        Bus b(m);
        const i32 width = static_cast<i32>(m.cpu.d[2] & 0xFFFF);
        native::set_result(m, pixel_array(b, g.display_, m, m.cpu.a[0],
                                          static_cast<i32>(m.cpu.d[0] & 0xFFFF),
                                          static_cast<i32>(m.cpu.d[1] & 0xFFFF), width, 1,
                                          m.cpu.a[2], static_cast<u32>(width), write));
    }

    static void write_pixel_line8(void* ctx, Machine& m) { pixel_line8(ctx, m, true); }
    static void read_pixel_line8(void* ctx, Machine& m) { pixel_line8(ctx, m, false); }

    // WriteChunkyPixels(rp A0, xstart D0, ystart D1, xstop D2, ystop D3,
    // array A2, bytesperrow D4).
    static void write_chunky_pixels(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const i32 x0 = static_cast<i32>(m.cpu.d[0] & 0xFFFF);
        const i32 y0 = static_cast<i32>(m.cpu.d[1] & 0xFFFF);
        pixel_array(b, g.display_, m, m.cpu.a[0], x0, y0,
                    static_cast<i32>(m.cpu.d[2] & 0xFFFF) - x0 + 1,
                    static_cast<i32>(m.cpu.d[3] & 0xFFFF) - y0 + 1, m.cpu.a[2], m.cpu.d[4], true);
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

    // BltTemplate(source A0, bit x D0, modulo D1, rp A1, x D2, y D3, width
    // D4, height D5): a one-bit template drawn with the rastport's pens and
    // draw mode, as text is: set bits in the foreground pen, clear ones in
    // the background pen under JAM2, COMPLEMENT flipping instead.
    static void blt_template(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 rp = m.cpu.a[1];
        const u32 source = m.cpu.a[0];
        if (rp == 0 || source == 0) {
            return;
        }
        Bus b(m);
        const View v = raster::load_rastport(b, rp);
        if (!v.ok) {
            return;
        }
        const raster::Pens p = pens(b, rp);
        const u32 first_bit = m.cpu.d[0] & 0xF;
        const i32 modulo = static_cast<i16>(m.cpu.d[1]);
        const i32 x0 = static_cast<i16>(m.cpu.d[2]) + v.origin_x;
        const i32 y0 = static_cast<i16>(m.cpu.d[3]) + v.origin_y;
        const i32 width = static_cast<i16>(m.cpu.d[4]);
        const i32 height = static_cast<i16>(m.cpu.d[5]);
        const bool inverse = (p.mode & ndk::INVERSVID) != 0;
        for (i32 row = 0; row < height && b.ok(); ++row) {
            const u32 line = source + static_cast<u32>(row * modulo);
            for (i32 col = 0; col < width && b.ok(); ++col) {
                const u32 bit = first_bit + static_cast<u32>(col);
                const bool set = ((b.r8(line + bit / 8) >> (7 - bit % 8)) & 1) != 0;
                const bool on = set != inverse;
                const i32 x = x0 + col;
                const i32 y = y0 + row;
                if (!drawable(b, v, x, y)) {
                    continue;
                }
                if ((p.mode & ndk::COMPLEMENT) != 0) {
                    if (on) {
                        raster::write_pixel(b, v.bitmap, x, y,
                                            ~raster::read_pixel(b, v.bitmap, x, y) & 0xFF);
                    }
                } else if (on) {
                    raster::write_pixel(b, v.bitmap, x, y, p.fg);
                } else if ((p.mode & ndk::JAM2) != 0) {
                    raster::write_pixel(b, v.bitmap, x, y, p.bg);
                }
            }
        }
        if (b.ok() && width > 0 && height > 0) {
            report(g.display_, m, v, Rect{x0, y0, x0 + width - 1, y0 + height - 1});
        }
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

    // The RPTAG_ attributes of a RastPort: its font, pens, mode, write mask
    // and largest pen, and for GetRPAttrsA the bounds drawing may reach.
    static constexpr u32 RPTAG_Font = 0x80000000;
    static constexpr u32 RPTAG_APen = 0x80000002;
    static constexpr u32 RPTAG_BPen = 0x80000003;
    static constexpr u32 RPTAG_DrMd = 0x80000004;
    static constexpr u32 RPTAG_OutlinePen = 0x80000005;
    static constexpr u32 RPTAG_WriteMask = 0x80000006;
    static constexpr u32 RPTAG_MaxPen = 0x80000007;
    static constexpr u32 RPTAG_DrawBounds = 0x80000008;

    static void rp_attrs(Machine& m, u32 rp, u32 tags, bool set) {
        Bus b(m);
        u32 budget = tags::kMaxTagItems;
        u32 at = tags;
        for (;;) {
            const u32 item = tags::next(b, at, budget);
            if (item == 0 || rp == 0) {
                return;
            }
            const u32 tag = b.r32(item);
            const u32 data = b.r32(item + 4);
            u32 offset = 0;
            switch (tag) {
            case RPTAG_Font:
                if (set) {
                    b.w32(rp + ndk::rp_Font, data);
                } else {
                    b.w32(data, b.r32(rp + ndk::rp_Font));
                }
                continue;
            case RPTAG_APen:
                offset = ndk::rp_FgPen;
                break;
            case RPTAG_BPen:
                offset = ndk::rp_BgPen;
                break;
            case RPTAG_DrMd:
                offset = ndk::rp_DrawMode;
                break;
            case RPTAG_OutlinePen:
                offset = ndk::rp_AOlPen;
                break;
            case RPTAG_WriteMask:
                offset = ndk::rp_Mask;
                break;
            case RPTAG_MaxPen:
                if (!set) {
                    b.w32(data, 255);
                }
                continue;
            case RPTAG_DrawBounds:
                // The whole bitmap: drawing is not recorded.
                if (!set) {
                    const u32 bitmap = b.r32(rp + ndk::rp_BitMap);
                    const u32 width = bitmap != 0 ? b.r16(bitmap + ndk::bm_BytesPerRow) * 8 : 1;
                    const u32 height = bitmap != 0 ? b.r16(bitmap + ndk::bm_Rows) : 1;
                    b.w16(data, 0);
                    b.w16(data + 2, 0);
                    b.w16(data + 4, width - 1);
                    b.w16(data + 6, height - 1);
                }
                continue;
            default:
                continue;
            }
            if (set) {
                b.w8(rp + offset, data & 0xFF);
            } else {
                b.w32(data, b.r8(rp + offset));
            }
        }
    }

    static void set_rp_attrs(void*, Machine& m) { rp_attrs(m, m.cpu.a[0], m.cpu.a[1], true); }
    static void get_rp_attrs(void*, Machine& m) { rp_attrs(m, m.cpu.a[0], m.cpu.a[1], false); }

    // ScrollRasterBF: ScrollRaster; the uncovered part takes pen 0, as the
    // layer's default backfill gives it.
    static void scroll_raster_bf(void* ctx, Machine& m) { scroll_raster(ctx, m); }

    static void ask_font(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 rp = m.cpu.a[1];
        const u32 attr = m.cpu.a[0];
        if (rp == 0 || attr == 0) {
            return;
        }
        Bus b(m);
        const u32 font = b.r32(rp + ndk::rp_Font);
        if (font == 0) {
            b.w32(attr + ndk::ta_Name, g.topaz_name_);
            b.w16(attr + ndk::ta_YSize, raster::kBuiltinSize);
            b.w8(attr + ndk::ta_Style, 0);
            b.w8(attr + ndk::ta_Flags, 0);
            return;
        }
        b.w32(attr + ndk::ta_Name, b.r32(font + ndk::LN_NAME));
        b.w16(attr + ndk::ta_YSize, b.r16(font + ndk::tf_YSize));
        b.w8(attr + ndk::ta_Style, b.r8(font + ndk::tf_Style));
        b.w8(attr + ndk::ta_Flags, b.r8(font + ndk::tf_Flags));
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

    // The pen and mode readers of V39, and their setters.
    static void get_a_pen(void*, Machine& m) { rp_byte(m, ndk::rp_FgPen); }
    static void get_b_pen(void*, Machine& m) { rp_byte(m, ndk::rp_BgPen); }
    static void get_dr_md(void*, Machine& m) { rp_byte(m, ndk::rp_DrawMode); }
    static void get_outline_pen(void*, Machine& m) { rp_byte(m, ndk::rp_AOlPen); }

    static void rp_byte(Machine& m, u32 offset) {
        Bus b(m);
        const u32 value = b.r8(m.cpu.a[0] + offset);
        if (b.ok()) {
            native::set_result(m, value);
        }
    }

    // SetABPenDrMd(rp A1, apen D0, bpen D1, mode D2).
    static void set_ab_pen_dr_md(void* ctx, Machine& m) {
        of(ctx).set_pens(m.cpu.a[1], m.cpu.d[0] & 0xFF, m.cpu.d[1] & 0xFF, m.cpu.d[2] & 0xFF);
    }

    // SetOutlinePen(rp A0, pen D0): the old outline pen.
    static void set_outline_pen(void*, Machine& m) {
        Bus b(m);
        const u32 rp = m.cpu.a[0];
        const u32 old = b.r8(rp + ndk::rp_AOlPen);
        b.w8(rp + ndk::rp_AOlPen, m.cpu.d[0] & 0xFF);
        b.w16(rp + ndk::rp_Flags, b.r16(rp + ndk::rp_Flags) | ndk::AREAOUTLINE);
        if (b.ok()) {
            native::set_result(m, old);
        }
    }

    // SetWriteMask(rp A0, mask D0): the planes drawing may change.
    static void set_write_mask(void*, Machine& m) {
        Bus b(m);
        b.w8(m.cpu.a[0] + ndk::rp_Mask, m.cpu.d[0] & 0xFF);
        if (b.ok()) {
            native::set_result(m, 1);
        }
    }

    // SetMaxPen(rp A0, pen D0) only tells which pens drawing will use.
    static void set_max_pen(void*, Machine&) {}

    // GetBitMapAttr(bitmap A0, attribute D1): BMA_HEIGHT, BMA_DEPTH,
    // BMA_WIDTH or BMA_FLAGS.
    static void get_bitmap_attr(void* ctx, Machine& m) {
        Bus b(m);
        const u32 bitmap = m.cpu.a[0];
        u32 value = 0;
        // A deep bitmap reports its pixel width and depth, and is not a
        // standard planar one.
        if (const DeepBitmap* deep = of(ctx).deep(bitmap)) {
            const u32 attribute = m.cpu.d[1];
            value = attribute == 0   ? deep->height
                    : attribute == 4 ? kDeepDepth[static_cast<u32>(deep->fmt)]
                    : attribute == 8 ? deep->width
                    : attribute == 12 ? (b.r8(bitmap + ndk::bm_Flags) & ~kBmfStandard)
                                      : 0;
            if (b.ok()) {
                native::set_result(m, value);
            }
            return;
        }
        switch (m.cpu.d[1]) {
        case 0:
            value = b.r16(bitmap + ndk::bm_Rows);
            break;
        case 4:
            value = b.r8(bitmap + ndk::bm_Depth);
            break;
        case 8:
            value = b.r16(bitmap + ndk::bm_BytesPerRow) * 8;
            break;
        case 12:
            value = b.r8(bitmap + ndk::bm_Flags);
            break;
        default:
            break;
        }
        if (b.ok()) {
            native::set_result(m, value);
        }
    }

    // AllocBitMap(width D0, height D1, depth D2, flags D3, friend A0): a
    // BitMap and its planes, all of them cleared.
    static void alloc_bitmap(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 width = m.cpu.d[0];
        const u32 height = m.cpu.d[1];
        const u32 depth = m.cpu.d[2];
        const u32 flags = m.cpu.d[3];
        const u32 row_bytes = ((width + 15) / 16) * 2;
        native::set_result(m, 0);
        // A deep friend, a CyberGraphX pixel format or a depth past 8 asks
        // for a deep bitmap, as on a graphics card.
        PixFmt fmt = PixFmt::Lut8;
        bool deep = true;
        if (const DeepBitmap* friend_bm = g.deep(m.cpu.a[0])) {
            fmt = friend_bm->fmt;
        } else if ((flags & kBmfSpecialFmt) != 0) {
            const u32 pixfmt = flags >> 24;
            if (pixfmt != kPixFmtLut8 && pixfmt != kPixFmtRgb16 && pixfmt != kPixFmtArgb32) {
                return;
            }
            fmt = pixfmt == kPixFmtLut8    ? PixFmt::Lut8
                  : pixfmt == kPixFmtRgb16 ? PixFmt::Rgb16
                                           : PixFmt::Argb32;
        } else if (depth > 8 && depth <= 16) {
            fmt = PixFmt::Rgb16;
        } else if (depth > 16 && depth <= 32) {
            fmt = PixFmt::Argb32;
        } else {
            deep = false;
        }
        if (deep) {
            native::set_result(m, g.alloc_deep_bitmap(width, height, fmt));
            return;
        }
        if (g.exec_ == nullptr || depth == 0 || depth > 8 || row_bytes == 0 || height == 0 ||
            row_bytes > 0xFFFF || height > 0xFFFF) {
            return;
        }
        const u32 bitmap = g.exec_->alloc(ndk::BitMap_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (bitmap == 0) {
            return;
        }
        Bus b(m);
        b.w16(bitmap + ndk::bm_BytesPerRow, row_bytes);
        b.w16(bitmap + ndk::bm_Rows, height);
        b.w8(bitmap + ndk::bm_Flags, m.cpu.d[3] & 0xFF);
        b.w8(bitmap + ndk::bm_Depth, depth);
        for (u32 plane = 0; plane < depth; ++plane) {
            const u32 at = g.exec_->alloc(row_bytes * height, ndk::MEMF_CHIP | ndk::MEMF_CLEAR);
            if (at == 0) {
                free_bitmap_planes(g, b, bitmap, plane);
                return;
            }
            b.w32(bitmap + ndk::bm_Planes + plane * 4, at);
        }
        if (b.ok()) {
            native::set_result(m, bitmap);
        }
    }

    static void free_bitmap_planes(Graphics& g, Bus& b, u32 bitmap, u32 planes) {
        const u32 bytes = b.r16(bitmap + ndk::bm_BytesPerRow) * b.r16(bitmap + ndk::bm_Rows);
        for (u32 plane = 0; plane < planes; ++plane) {
            const u32 at = b.r32(bitmap + ndk::bm_Planes + plane * 4);
            if (at != 0 && b.ok()) {
                g.exec_->free(at, bytes);
            }
        }
        g.exec_->free(bitmap, ndk::BitMap_SIZEOF);
    }

    // FreeBitMap(bitmap A0): one AllocBitMap made.
    static void free_bitmap(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 bitmap = m.cpu.a[0];
        if (bitmap == 0 || g.exec_ == nullptr) {
            return;
        }
        if (g.deep(bitmap) != nullptr) {
            g.free_deep_bitmap(bitmap);
            return;
        }
        Bus b(m);
        const u32 depth = b.r8(bitmap + ndk::bm_Depth);
        if (b.ok() && depth <= 8) {
            free_bitmap_planes(g, b, bitmap, depth);
        }
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
            g.palettes_changed();
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
            g.palettes_changed();
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

    // The planes of a screen whose colour map has this many colours, 0 when
    // the count is not a power of two.
    static u32 planes_of(u32 colors) {
        u32 planes = 0;
        while ((1u << planes) < colors) {
            ++planes;
        }
        return (1u << planes) == colors ? planes : 0;
    }

    // How many hold pen of a colour map of this many colours: the programs
    // that obtained it, and the system, which keeps the first kSystemPens
    // and the chrome's pens of a screen of that many planes. Programs may
    // share those but never hold one alone.
    static u32 pen_holders(const Graphics& g, u32 pen, u32 colors) {
        const bool system = pen < Graphics::kSystemPens ||
                            chrome::logical_pen(pen, planes_of(colors)) != 0;
        return g.pen_users_[pen] + (system ? 1u : 0u);
    }

    // The highest pen of a colour map of this many colours that nobody
    // holds, -1 when there is none.
    static i32 free_pen(const Graphics& g, u32 colors) {
        for (u32 pen = colors < Graphics::kPens ? colors : Graphics::kPens;
             pen-- > Graphics::kSystemPens;) {
            if (pen_holders(g, pen, colors) == 0) {
                return static_cast<i32>(pen);
            }
        }
        return -1;
    }

    // The pen of the colour map closest to r, g, b (32-bit components),
    // among the first limit pens of its colors and those that may be shared
    // when shared_only.
    static i32 nearest_pen(Graphics& g, u32 map, u32 colors, u32 limit, u32 r, u32 gr, u32 bl,
                           bool shared_only) {
        i32 best = -1;
        u32 best_distance = 0xFFFFFFFF;
        for (u32 pen = 0; pen < limit && pen < Graphics::kPens; ++pen) {
            if (shared_only && (g.pen_exclusive_[pen] || pen_holders(g, pen, colors) == 0)) {
                continue;
            }
            u8 pr = 0;
            u8 pg = 0;
            u8 pb = 0;
            if (!g.color_of(map, pen, pr, pg, pb)) {
                continue;
            }
            const i32 dr = static_cast<i32>(pr) - static_cast<i32>(r >> 24);
            const i32 dg = static_cast<i32>(pg) - static_cast<i32>(gr >> 24);
            const i32 db = static_cast<i32>(pb) - static_cast<i32>(bl >> 24);
            const u32 distance = static_cast<u32>(dr * dr + dg * dg + db * db);
            if (distance < best_distance) {
                best_distance = distance;
                best = static_cast<i32>(pen);
            }
        }
        return best;
    }

    // ObtainPen(cm A0, n D0, r D1, g D2, b D3, flags D4): pen n, or any free
    // one when n is -1, for the caller; PENF_EXCLUSIVE keeps it from being
    // shared, and its colour is set unless PENF_NO_SETCOLOR. -1 when none.
    static void obtain_pen(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        constexpr u32 kExclusive = 1;
        constexpr u32 kNoSetColor = 2;
        const u32 map = m.cpu.a[0];
        const ColorMapTables t = colormap_tables(b, map);
        const u32 count = t.limit < Graphics::kPens ? t.limit : Graphics::kPens;
        const u32 flags = m.cpu.d[4];
        const bool exclusive = (flags & kExclusive) != 0;
        i32 pen = -1;
        const u32 wanted = m.cpu.d[0];
        if (wanted == 0xFFFFFFFF) {
            pen = free_pen(g, count);
        } else if (wanted < count && !g.pen_exclusive_[wanted] &&
                   (pen_holders(g, wanted, count) == 0 || !exclusive)) {
            pen = static_cast<i32>(wanted);
        }
        if (pen < 0) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        const bool fresh = pen_holders(g, static_cast<u32>(pen), count) == 0;
        ++g.pen_users_[pen];
        g.pen_exclusive_[pen] = exclusive;
        if (fresh && (flags & kNoSetColor) == 0 &&
            write_rgb32_entry(b, t, static_cast<u32>(pen), m.cpu.d[1], m.cpu.d[2], m.cpu.d[3])) {
            g.palettes_changed();
            mark_colormap_dirty(g.display_, m, map);
        }
        native::set_result(m, static_cast<u32>(pen));
    }

    // ReleasePen(cm A0, n D0): one user fewer of pen n.
    static void release_pen(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        const u32 pen = m.cpu.d[0];
        if (pen < Graphics::kPens && g.pen_users_[pen] > 0) {
            if (--g.pen_users_[pen] == 0) {
                g.pen_exclusive_[pen] = false;
            }
        }
    }

    // ObtainBestPenA(cm A0, r D1, g D2, b D3, tags A1): the shared pen
    // closest to the colour, or a free one set to it when none is close.
    static void obtain_best_pen(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 map = m.cpu.a[0];
        const ColorMapTables t = colormap_tables(b, map);
        const u32 count = t.limit < Graphics::kPens ? t.limit : Graphics::kPens;
        const u32 r = m.cpu.d[1];
        const u32 gr = m.cpu.d[2];
        const u32 bl = m.cpu.d[3];
        i32 pen = nearest_pen(g, map, count, count, r, gr, bl, true);
        u8 pr = 0;
        u8 pg = 0;
        u8 pb = 0;
        const bool exact = pen >= 0 && g.color_of(map, static_cast<u32>(pen), pr, pg, pb) &&
                           pr == (r >> 24) && pg == (gr >> 24) && pb == (bl >> 24);
        if (!exact) {
            const i32 spare = free_pen(g, count);
            if (spare >= 0 && write_rgb32_entry(b, t, static_cast<u32>(spare), r, gr, bl)) {
                g.palettes_changed();
                mark_colormap_dirty(g.display_, m, map);
                pen = spare;
            }
        }
        if (pen < 0) {
            native::set_result(m, 0xFFFFFFFF);
            return;
        }
        ++g.pen_users_[pen];
        native::set_result(m, static_cast<u32>(pen));
    }

    // FindColor(cm A3, r D1, g D2, b D3, maxpen D4): the closest pen up to
    // maxpen, or of the whole map when maxpen is -1. Nothing is obtained.
    static void find_color(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 map = m.cpu.a[3];
        const ColorMapTables t = colormap_tables(b, map);
        u32 count = t.limit < Graphics::kPens ? t.limit : Graphics::kPens;
        const u32 max_pen = m.cpu.d[4];
        if (max_pen != 0xFFFFFFFF && max_pen + 1 < count) {
            count = max_pen + 1;
        }
        const i32 pen =
            nearest_pen(g, map, t.limit, count, m.cpu.d[1], m.cpu.d[2], m.cpu.d[3], false);
        native::set_result(m, pen < 0 ? 0 : static_cast<u32>(pen));
    }

    // The display mode of a ViewPort: the one registered for it, or the
    // mode its vp_Modes word asks for. INVALID_ID for no ViewPort.
    static u32 mode_id_of_viewport(Graphics& g, Bus& b, u32 vp) {
        if (vp == 0) {
            return 0xFFFFFFFF;
        }
        u32 id = 0;
        if (g.vp_mode_id(vp, id)) {
            return id;
        }
        return display_modes::from_view_modes(static_cast<u16>(b.r16(vp + ndk::vp_Modes)));
    }

    // GetVPModeID(vp A0): the display mode its vp_Modes word asks for, or
    // the one registered for it.
    static void get_vp_mode_id(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 id = mode_id_of_viewport(g, b, m.cpu.a[0]);
        native::set_result(m, b.ok() ? id : 0xFFFFFFFF);
    }

    // struct MonitorSpec (graphics/monitor.h): its ExtendedNode, the ratios
    // of the display to a standard one, its open count and its size.
    static constexpr u32 xln_Subsystem = 14;
    static constexpr u32 xln_Subtype = 15;
    static constexpr u32 ms_ratioh = 26;
    static constexpr u32 ms_ratiov = 30;
    static constexpr u32 ms_total_rows = 34;
    static constexpr u32 ms_total_colorclocks = 36;
    static constexpr u32 ms_OpenCount = 48;
    static constexpr u32 kMonitorSpecBytes = 160;
    static constexpr u32 kRatioUnity = 16;
    static constexpr u32 kNtGraphics = 9;
    static constexpr u32 kSsGraphics = 2;
    static constexpr u32 kMonitorSpecType = 0;

    // A MonitorSpec named name, with total_rows raster lines of colorclocks
    // clocks, in a block of exec's memory; 0 if exec has no memory for it.
    static u32 make_monitor_spec(Graphics& g, Bus& b, const char* name, u32 total_rows,
                                 u32 colorclocks) {
        u32 name_bytes = 1;
        while (name[name_bytes - 1] != 0) {
            ++name_bytes;
        }
        const u32 block = g.exec_->alloc(kMonitorSpecBytes + name_bytes,
                                         ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (block == 0) {
            return 0;
        }
        for (u32 i = 0; i < name_bytes; ++i) {
            b.w8(block + kMonitorSpecBytes + i, static_cast<u8>(name[i]));
        }
        b.w8(block + ndk::LN_TYPE, kNtGraphics);
        b.w32(block + ndk::LN_NAME, block + kMonitorSpecBytes);
        b.w8(block + xln_Subsystem, kSsGraphics);
        b.w8(block + xln_Subtype, kMonitorSpecType);
        b.w32(block + ms_ratioh, kRatioUnity);
        b.w32(block + ms_ratiov, kRatioUnity);
        b.w16(block + ms_total_rows, total_rows);
        b.w16(block + ms_total_colorclocks, colorclocks);
        return block;
    }

    // The MonitorSpec of the default, NTSC or PAL monitor, made on first
    // use.
    static u32 monitor_spec(Graphics& g, Bus& b, u32 monitor) {
        if (monitor == display_modes::kHombreMonitorId) {
            if (g.hombre_monitor_ == 0) {
                g.hombre_monitor_ = make_monitor_spec(g, b, "hombre.monitor", 1080, 226);
            }
            return g.hombre_monitor_;
        }
        const bool ntsc = monitor == display_modes::kNtscMonitorId;
        const bool pal = monitor == display_modes::kPalMonitorId;
        u32& spec = pal ? g.pal_monitor_ : ntsc ? g.ntsc_monitor_ : g.monitor_;
        if (spec == 0) {
            spec = make_monitor_spec(
                g, b,
                pal ? "pal.monitor" : ntsc ? "ntsc.monitor" : "default.monitor",
                pal ? 312 : 262, pal || ntsc ? 227 : 226);
        }
        return spec;
    }

    // OpenMonitor(name A1, displayID D0): the MonitorSpec of the monitor
    // named or of the display ID's monitor, the default monitor's for
    // anything else, its open count one up.
    static void open_monitor(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 name = m.cpu.a[1];
        u32 monitor = display_modes::kDefaultMonitorId;
        if (name != 0) {
            if (b.equals(name, "ntsc.monitor")) {
                monitor = display_modes::kNtscMonitorId;
            } else if (b.equals(name, "pal.monitor")) {
                monitor = display_modes::kPalMonitorId;
            } else if (b.equals(name, "hombre.monitor")) {
                monitor = display_modes::kHombreMonitorId;
            }
        } else {
            const u32 display_monitor = m.cpu.d[0] & display_modes::kMonitorIdMask;
            if (display_monitor == display_modes::kNtscMonitorId ||
                display_monitor == display_modes::kPalMonitorId ||
                display_monitor == display_modes::kHombreMonitorId) {
                monitor = display_monitor;
            }
        }
        const u32 spec = monitor_spec(g, b, monitor);
        if (spec == 0) {
            native::set_result(m, 0);
            return;
        }
        b.w16(spec + ms_OpenCount, b.r16(spec + ms_OpenCount) + 1);
        native::set_result(m, b.ok() ? spec : 0);
    }

    // CloseMonitor(monitorSpec A0): its open count one down; the spec
    // itself lives on. FALSE back, as for success.
    static void close_monitor(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 spec = m.cpu.a[0];
        if (spec != 0 &&
            (spec == g.monitor_ || spec == g.ntsc_monitor_ || spec == g.pal_monitor_ ||
             spec == g.hombre_monitor_) &&
            b.r16(spec + ms_OpenCount) != 0) {
            b.w16(spec + ms_OpenCount, b.r16(spec + ms_OpenCount) - 1);
        }
        native::set_result(m, 0);
    }

    // The monitor a mode belongs to: HOMBRE, PAL, else NTSC, else the
    // default one.
    static u32 monitor_of(u32 id) {
        if (display_modes::is_hombre(id)) {
            return display_modes::kHombreMonitorId;
        }
        if ((id & display_modes::kPalMonitorId) == display_modes::kPalMonitorId) {
            return display_modes::kPalMonitorId;
        }
        if ((id & display_modes::kNtscMonitorId) == display_modes::kNtscMonitorId) {
            return display_modes::kNtscMonitorId;
        }
        return display_modes::kDefaultMonitorId;
    }

    // FindDisplayInfo(ID D0): the handle of a known display mode, 0 for
    // any other.
    static void find_display_info(void*, Machine& m) {
        constexpr u32 kHandleBase = 0x10000;
        const u32 index = display_modes::index_of(m.cpu.d[0]);
        native::set_result(m, index < display_modes::count() ? kHandleBase + index : 0);
    }

    // NextDisplayInfo(last ID D0): the display modes in turn, then
    // INVALID_ID for the end of the list.
    static void next_display_info(void*, Machine& m) {
        constexpr u32 kInvalidId = 0xFFFFFFFF;
        const u32 last = m.cpu.d[0];
        u32 next = kInvalidId;
        if (last == kInvalidId) {
            next = display_modes::at(0).id;
        } else {
            const u32 index = display_modes::index_of(last);
            if (index + 1 < display_modes::count()) {
                next = display_modes::at(index + 1).id;
            }
        }
        native::set_result(m, next);
    }

    // GetDisplayInfoData(handle A0, buf A1, size D0, tagID D1, ID D2): the
    // DisplayInfo, DimensionInfo, MonitorInfo or NameInfo of the display
    // mode, the handle's mode when ID is INVALID_ID; the number of bytes
    // written, 0 for other tags or IDs.
    static void get_display_info_data(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        constexpr u32 kHandleBase = 0x10000;
        constexpr u32 kInvalidId = 0xFFFFFFFF;
        constexpr u32 kDisp = 0x80000000;
        constexpr u32 kDims = 0x80001000;
        constexpr u32 kMntr = 0x80002000;
        constexpr u32 kName = 0x80003000;
        const u32 buf = m.cpu.a[1];
        const u32 tag = m.cpu.d[1];
        const display_modes::Mode* mode = nullptr;
        if (m.cpu.d[2] != kInvalidId) {
            mode = display_modes::find(m.cpu.d[2]);
        } else {
            const u32 handle = m.cpu.a[0];
            if (handle >= kHandleBase && handle - kHandleBase < display_modes::count()) {
                mode = &display_modes::at(handle - kHandleBase);
            }
        }
        if (mode == nullptr) {
            native::set_result(m, 0);
            return;
        }
        u8 data[96] = {};
        u32 bytes = 0;
        const auto put16 = [&](u32 at, u32 v) {
            data[at] = static_cast<u8>(v >> 8);
            data[at + 1] = static_cast<u8>(v);
        };
        const auto put32 = [&](u32 at, u32 v) {
            put16(at, v >> 16);
            put16(at + 2, v & 0xFFFF);
        };
        const auto rect = [&](u32 at) {
            put16(at, 0);
            put16(at + 2, 0);
            put16(at + 4, mode->width - 1);
            put16(at + 6, mode->height - 1);
        };
        if (tag == kDisp) {
            bytes = 56;
            put16(16, 0);            // NotAvailable
            put32(18, mode->flags);  // PropertyFlags
            put16(22, mode->res_x);  // Resolution: ticks a pixel across
            put16(24, mode->res_y);  // and down
            const u32 pixel_speed = mode->width == 1280 ? 18 : mode->width == 640 ? 35 : 70;
            put16(26, pixel_speed);  // PixelSpeed, ns
            put16(28, 1);            // NumStdSprites
            put16(30, 0xFFFF);       // PaletteRange: 24-bit colour
            put16(32, 22);
            put16(34, 22);
            data[40] = 8;  // RedBits
            data[41] = 8;  // GreenBits
            data[42] = 8;  // BlueBits
        } else if (tag == kDims) {
            bytes = 88;
            put16(16, mode->max_depth);  // MaxDepth
            put16(18, 16);               // MinRasterWidth
            put16(20, 16);               // MinRasterHeight
            put16(22, 16368);            // MaxRasterWidth
            put16(24, 16384);            // MaxRasterHeight
            rect(26);                    // Nominal
            rect(34);                    // MaxOScan
            rect(42);                    // VideoOScan
            rect(50);                    // TxtOScan
            rect(58);                    // StdOScan
        } else if (tag == kMntr) {
            const u32 monitor = monitor_of(mode->id);
            const u32 spec = monitor_spec(g, b, monitor);
            if (spec == 0 || !b.ok()) {
                native::set_result(m, 0);
                return;
            }
            const bool pal = monitor == display_modes::kPalMonitorId;
            const bool hombre = monitor == display_modes::kHombreMonitorId;
            bytes = 96;
            put32(16, spec);  // Mspc
            put16(24, 22);    // ViewResolution across
            put16(26, 22);    // and down
            put16(36, hombre ? mode->height : pal ? 625 : 525);  // TotalRows
            put16(38, 227);                                       // TotalColorClocks
            put16(40, hombre ? 0 : pal ? 29 : 21);                // MinRow
            put16(76, 1);                // MouseTicks across
            put16(78, 1);                // and down
            put32(84, hombre ? mode->id : display_modes::preferred(monitor));  // PreferredModeID
        } else if (tag == kName) {
            bytes = 56;
            for (u32 i = 0; mode->name[i] != '\0'; ++i) {
                data[16 + i] = static_cast<u8>(mode->name[i]);
            }
        }
        if (bytes == 0 || buf == 0) {
            native::set_result(m, 0);
            return;
        }
        put32(0, tag);
        put32(4, mode->id);
        put32(8, 0);
        put32(12, (bytes + 7) / 8);  // QueryHeader Length, in double longwords
        const u32 n = m.cpu.d[0] < bytes ? m.cpu.d[0] : bytes;
        for (u32 i = 0; i < n && b.ok(); ++i) {
            b.w8(buf + i, data[i]);
        }
        native::set_result(m, n);
    }

    // BestModeIDA(tags A0): the display mode nearest the size, depth and
    // property flags the tags ask for. INVALID_ID when no mode fits.
    static void best_mode_id(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        constexpr u32 kInvalidId = 0xFFFFFFFF;
        constexpr u32 kMustHave = 0x80000001;
        constexpr u32 kMustNotHave = 0x80000002;
        constexpr u32 kViewPort = 0x80000003;
        constexpr u32 kNominalWidth = 0x80000004;
        constexpr u32 kNominalHeight = 0x80000005;
        constexpr u32 kDesiredWidth = 0x80000006;
        constexpr u32 kDesiredHeight = 0x80000007;
        constexpr u32 kDepth = 0x80000008;
        constexpr u32 kMonitorId = 0x80000009;
        constexpr u32 kSourceId = 0x8000000A;
        const u32 tag_list = m.cpu.a[0];
        const u32 nominal_width = tags::data(b, kNominalWidth, tag_list, 640);
        const u32 nominal_height = tags::data(b, kNominalHeight, tag_list, 200);
        const u32 desired_width = tags::data(b, kDesiredWidth, tag_list, nominal_width);
        const u32 desired_height = tags::data(b, kDesiredHeight, tag_list, nominal_height);
        const u32 depth = tags::data(b, kDepth, tag_list, 1);
        u32 must_have = tags::data(b, kMustHave, tag_list, 0);
        u32 source = tags::data(b, kSourceId, tag_list, kInvalidId);
        const u32 vp_item = tags::find(b, kViewPort, tag_list);
        if (vp_item != 0) {
            source = mode_id_of_viewport(g, b, b.r32(vp_item + 4));
        }
        u32 monitor = display_modes::kDefaultMonitorId;
        bool monitor_any = true;
        const u32 monitor_item = tags::find(b, kMonitorId, tag_list);
        if (monitor_item != 0) {
            monitor = monitor_of(b.r32(monitor_item + 4));
            monitor_any = false;
        }
        if (const display_modes::Mode* source_mode = display_modes::find(source)) {
            must_have |= source_mode->flags & (display_modes::kIsHam |
                                               display_modes::kIsExtraHalfBrite |
                                               display_modes::kIsLace);
            if (monitor_any) {
                monitor = monitor_of(source_mode->id);
                monitor_any = false;
            }
        }
        const u32 must_not = tags::data(
            b, kMustNotHave, tag_list,
            (display_modes::kIsHam | display_modes::kIsExtraHalfBrite) & ~must_have);
        const display_modes::Mode* best = nullptr;
        u32 best_area = 0;
        bool best_fits = false;
        for (u32 i = 0; i < display_modes::count(); ++i) {
            const display_modes::Mode& mode = display_modes::at(i);
            if (!monitor_any && monitor_of(mode.id) != monitor) {
                continue;
            }
            if ((mode.flags & must_have) != must_have || (mode.flags & must_not) != 0) {
                continue;
            }
            if (mode.max_depth < depth) {
                continue;
            }
            const bool fits = mode.width >= desired_width && mode.height >= desired_height;
            const u32 area = static_cast<u32>(mode.width) * mode.height;
            if (best == nullptr || (fits && !best_fits) ||
                (fits == best_fits && (fits ? area < best_area : area > best_area))) {
                best = &mode;
                best_area = area;
                best_fits = fits;
            }
        }
        native::set_result(m, best != nullptr ? best->id : kInvalidId);
    }

    // ModeNotAvailable(modeID D0): 0 for a known mode, DI_AVAIL_NOMONITOR
    // for any other.
    static void mode_not_available(void*, Machine& m) {
        constexpr u32 kDiAvailNoMonitor = 2;
        native::set_result(m, display_modes::find(m.cpu.d[0]) != nullptr ? 0 : kDiAvailNoMonitor);
    }

    // A copy of a tag list as one block, TAG_MORE followed and TAG_IGNORE
    // and TAG_SKIP left out, at most kMaxFontTags items; the block's size
    // sits in the long before the list. 0 for no tags.
    static u32 copy_tags(Graphics& g, Bus& b, u32 tags) {
        if (tags == 0) {
            return 0;
        }
        u32 items[kMaxFontTags][2];
        u32 count = 0;
        u32 at = tags;
        for (u32 step = 0; at != 0 && step < 4 * kMaxFontTags && count < kMaxFontTags; ++step) {
            const u32 tag = b.r32(at);
            const u32 data = b.r32(at + 4);
            if (!b.ok() || tag == 0) {
                break;
            }
            if (tag == 2) {  // TAG_MORE
                at = data;
                continue;
            }
            if (tag == 3) {  // TAG_SKIP
                at += 8 * (data + 1);
                continue;
            }
            if (tag != 1) {  // TAG_IGNORE
                items[count][0] = tag;
                items[count][1] = data;
                ++count;
            }
            at += 8;
        }
        const u32 bytes = 4 + (count + 1) * 8;
        const u32 block = g.exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (block == 0) {
            return 0;
        }
        b.w32(block, bytes);
        for (u32 i = 0; i < count; ++i) {
            b.w32(block + 4 + i * 8, items[i][0]);
            b.w32(block + 8 + i * 8, items[i][1]);
        }
        return block + 4;
    }

    static void free_tags(Graphics& g, Bus& b, u32 list) {
        if (list != 0) {
            g.exec_->free(list - 4, b.r32(list - 4));
        }
    }

    // ExtendFont(font A0, fontTags A1): gives the font a TextFontExtension
    // in tf_Extension, the field that otherwise holds its message's reply
    // port, keeping the port in the extension, and marks the font
    // FSF_TAGGED. The extension holds a copy of the tags. A font extended
    // already stays as it is. TRUE when the font has its extension.
    static void extend_font(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        native::set_result(m, extend(g, b, m.cpu.a[0], m.cpu.a[1]) ? 1 : 0);
    }

    static bool extend(Graphics& g, Bus& b, u32 font, u32 tags) {
        if (font == 0) {
            return false;
        }
        if (font_extension(b, font) != 0) {
            return true;
        }
        const u32 tfe = g.exec_->alloc(kTfeSize, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (tfe == 0) {
            return false;
        }
        b.w16(tfe + kTfeMatchWord, kTeMatchWord);
        b.w32(tfe + kTfeBackPtr, font);
        b.w32(tfe + kTfeOrigReplyPort, b.r32(font + ndk::mn_ReplyPort));
        b.w32(tfe + kTfeTags, copy_tags(g, b, tags));
        b.w32(font + ndk::mn_ReplyPort, tfe);
        b.w8(font + ndk::tf_Style, b.r8(font + ndk::tf_Style) | kFsfTagged);
        return b.ok();
    }

    // StripFont(font A0): takes an ExtendFont extension off again, with
    // the reply port back in its place.
    static void strip_font(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const u32 font = m.cpu.a[0];
        const u32 tfe = font != 0 ? font_extension(b, font) : 0;
        if (tfe == 0) {
            return;
        }
        b.w32(font + ndk::mn_ReplyPort, b.r32(tfe + kTfeOrigReplyPort));
        b.w8(font + ndk::tf_Style, b.r8(font + ndk::tf_Style) & ~kFsfTagged & 0xFF);
        free_tags(g, b, b.r32(tfe + kTfeTags));
        g.exec_->free(tfe, kTfeSize);
    }

    // AskSoftStyle(rp A1): the styles that can be made for the
    // RastPort's font by drawing: bold, italic and underlined, less those
    // the font has of its own.
    static u32 soft_styles(Bus& b, u32 rp) {
        const u32 font = rp != 0 ? b.r32(rp + ndk::rp_Font) : 0;
        const u32 own = font != 0 ? b.r8(font + ndk::tf_Style) : 0;
        return kSoftStyles & ~own;
    }

    static void ask_soft_style(void*, Machine& m) {
        Bus b(m);
        native::set_result(m, soft_styles(b, m.cpu.a[1]));
    }

    // SetSoftStyle(rp A1, style D0, enable D1): sets the drawn styles that
    // enable lets through and AskSoftStyle offers; the answer is the style
    // the text now has, the font's own included. Text draws those styles
    // over the font's glyphs.
    static void set_soft_style(void*, Machine& m) {
        Bus b(m);
        const u32 rp = m.cpu.a[1];
        if (rp == 0) {
            native::set_result(m, 0);
            return;
        }
        const u32 enable = m.cpu.d[1] & soft_styles(b, rp);
        const u32 algo = (b.r8(rp + ndk::rp_AlgoStyle) & ~enable) | (m.cpu.d[0] & enable);
        b.w8(rp + ndk::rp_AlgoStyle, algo & 0xFF);
        const u32 font = b.r32(rp + ndk::rp_Font);
        const u32 own = font != 0 ? b.r8(font + ndk::tf_Style) : 0;
        native::set_result(m, (algo | own) & 0xFF);
    }

    // WeighTAMatch(reqTextAttr A0, targetTextAttr A1, targetTags A2): how
    // well the target font answers the request, MAXFONTMATCHWEIGHT for one
    // of the same height, style and flags, less for each way it differs,
    // the height most. The names are not compared.
    static void weigh_ta_match(void*, Machine& m) {
        Bus b(m);
        const u32 want = m.cpu.a[0];
        const u32 have = m.cpu.a[1];
        if (want == 0 || have == 0) {
            native::set_result(m, 0);
            return;
        }
        const i32 dy = static_cast<i32>(b.r16(want + ndk::ta_YSize)) -
                       static_cast<i32>(b.r16(have + ndk::ta_YSize));
        const u32 style = b.r8(want + ndk::ta_Style) ^ b.r8(have + ndk::ta_Style);
        const u32 flags = b.r8(want + ndk::ta_Flags) ^ b.r8(have + ndk::ta_Flags);
        if (!b.ok()) {
            native::set_result(m, 0);
            return;
        }
        native::set_result(m, static_cast<u32>(font_match_weight(dy, style, flags)));
    }

    static void set_rgb32_cm(void* ctx, Machine& m) {
        Graphics& g = of(ctx);
        Bus b(m);
        const ColorMapTables t = colormap_tables(b, m.cpu.a[0]);
        if (write_rgb32_entry(b, t, m.cpu.d[0], m.cpu.d[1], m.cpu.d[2], m.cpu.d[3])) {
            g.palettes_changed();
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
    {graphics_lvo::ScrollRasterBF, GraphicsNatives::scroll_raster_bf, "ScrollRasterBF"},
    {graphics_lvo::SetRPAttrsA, GraphicsNatives::set_rp_attrs, "SetRPAttrsA"},
    {graphics_lvo::GetRPAttrsA, GraphicsNatives::get_rp_attrs, "GetRPAttrsA"},
    {graphics_lvo::TextFit, GraphicsNatives::text_fit, "TextFit"},
    {graphics_lvo::GetAPen, GraphicsNatives::get_a_pen, "GetAPen"},
    {graphics_lvo::GetBPen, GraphicsNatives::get_b_pen, "GetBPen"},
    {graphics_lvo::GetDrMd, GraphicsNatives::get_dr_md, "GetDrMd"},
    {graphics_lvo::GetOutlinePen, GraphicsNatives::get_outline_pen, "GetOutlinePen"},
    {graphics_lvo::SetABPenDrMd, GraphicsNatives::set_ab_pen_dr_md, "SetABPenDrMd"},
    {graphics_lvo::AllocBitMap, GraphicsNatives::alloc_bitmap, "AllocBitMap"},
    {graphics_lvo::FreeBitMap, GraphicsNatives::free_bitmap, "FreeBitMap"},
    {graphics_lvo::GetBitMapAttr, GraphicsNatives::get_bitmap_attr, "GetBitMapAttr"},
    {graphics_lvo::SetOutlinePen, GraphicsNatives::set_outline_pen, "SetOutlinePen"},
    {graphics_lvo::SetWriteMask, GraphicsNatives::set_write_mask, "SetWriteMask"},
    {graphics_lvo::SetMaxPen, GraphicsNatives::set_max_pen, "SetMaxPen"},
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
    {graphics_lvo::AddFont, GraphicsNatives::add_font, "AddFont"},
    {graphics_lvo::RemFont, GraphicsNatives::rem_font, "RemFont"},
    {graphics_lvo::AllocRaster, GraphicsNatives::alloc_raster, "AllocRaster"},
    {graphics_lvo::FreeRaster, GraphicsNatives::free_raster, "FreeRaster"},
    {graphics_lvo::GetRGB4, GraphicsNatives::get_rgb4, "GetRGB4"},
    {graphics_lvo::SetRGB32, GraphicsNatives::set_rgb32, "SetRGB32"},
    {graphics_lvo::LoadRGB32, GraphicsNatives::load_rgb32, "LoadRGB32"},
    {graphics_lvo::GetRGB32, GraphicsNatives::get_rgb32, "GetRGB32"},
    {graphics_lvo::SetRGB32CM, GraphicsNatives::set_rgb32_cm, "SetRGB32CM"},
    {graphics_lvo::ObtainPen, GraphicsNatives::obtain_pen, "ObtainPen"},
    {graphics_lvo::GetVPModeID, GraphicsNatives::get_vp_mode_id, "GetVPModeID"},
    {graphics_lvo::BestModeIDA, GraphicsNatives::best_mode_id, "BestModeIDA"},
    {graphics_lvo::WritePixelArray8, GraphicsNatives::write_pixel_array8, "WritePixelArray8"},
    {graphics_lvo::ReadPixelArray8, GraphicsNatives::read_pixel_array8, "ReadPixelArray8"},
    {graphics_lvo::WritePixelLine8, GraphicsNatives::write_pixel_line8, "WritePixelLine8"},
    {graphics_lvo::ReadPixelLine8, GraphicsNatives::read_pixel_line8, "ReadPixelLine8"},
    {graphics_lvo::WriteChunkyPixels, GraphicsNatives::write_chunky_pixels, "WriteChunkyPixels"},
    {graphics_lvo::PolyDraw, GraphicsNatives::poly_draw, "PolyDraw"},
    {graphics_lvo::BltTemplate, GraphicsNatives::blt_template, "BltTemplate"},
    {graphics_lvo::ClipBlit, GraphicsNatives::clip_blit, "ClipBlit"},
    {graphics_lvo::OpenMonitor, GraphicsNatives::open_monitor, "OpenMonitor"},
    {graphics_lvo::CloseMonitor, GraphicsNatives::close_monitor, "CloseMonitor"},
    {graphics_lvo::FindDisplayInfo, GraphicsNatives::find_display_info, "FindDisplayInfo"},
    {graphics_lvo::NextDisplayInfo, GraphicsNatives::next_display_info, "NextDisplayInfo"},
    {graphics_lvo::GetDisplayInfoData, GraphicsNatives::get_display_info_data,
     "GetDisplayInfoData"},
    {graphics_lvo::ModeNotAvailable, GraphicsNatives::mode_not_available, "ModeNotAvailable"},
    {graphics_lvo::WeighTAMatch, GraphicsNatives::weigh_ta_match, "WeighTAMatch"},
    {graphics_lvo::AskSoftStyle, GraphicsNatives::ask_soft_style, "AskSoftStyle"},
    {graphics_lvo::SetSoftStyle, GraphicsNatives::set_soft_style, "SetSoftStyle"},
    {graphics_lvo::ExtendFont, GraphicsNatives::extend_font, "ExtendFont"},
    {graphics_lvo::StripFont, GraphicsNatives::strip_font, "StripFont"},
    {graphics_lvo::ReleasePen, GraphicsNatives::release_pen, "ReleasePen"},
    {graphics_lvo::ObtainBestPenA, GraphicsNatives::obtain_best_pen, "ObtainBestPenA"},
    {graphics_lvo::FindColor, GraphicsNatives::find_color, "FindColor"},
};

}

const u32* Graphics::palette_for(u32 colormap) {
    if (colormap == 0) {
        return nullptr;
    }
    HostPalette* free_slot = nullptr;
    for (HostPalette& palette : palettes_) {
        if (palette.colormap == colormap) {
            return palette.rgb;
        }
        if (palette.colormap == 0 && free_slot == nullptr) {
            free_slot = &palette;
        }
    }
    if (free_slot == nullptr) {
        return nullptr;
    }
    free_slot->colormap = colormap;
    sync_palette(*free_slot);
    return free_slot->rgb;
}

void Graphics::release_palette(u32 colormap) {
    for (HostPalette& palette : palettes_) {
        if (palette.colormap == colormap) {
            palette = HostPalette{};
        }
    }
}

void Graphics::sync_palette(HostPalette& palette) {
    for (u32 i = 0; i < 256; ++i) {
        u8 r = 0, g = 0, b = 0;
        palette.rgb[i] = color_of(palette.colormap, i, r, g, b)
                             ? (u32{r} << 16) | (u32{g} << 8) | b
                             : 0;
    }
}

void Graphics::palettes_changed() {
    bump_palette(display_);
    for (HostPalette& palette : palettes_) {
        if (palette.colormap != 0) {
            sync_palette(palette);
        }
    }
}

u32 Graphics::alloc_deep_bitmap(u32 width, u32 height, PixFmt fmt) {
    if (exec_ == nullptr || machine_ == nullptr || width == 0 || height == 0 ||
        width > 0x7FFF || height > 0x7FFF) {
        return 0;
    }
    const u32 row_bytes = (width * bytes_per_pixel(fmt) + 15) & ~15u;
    const u64 pixel_bytes = u64{row_bytes} * height;
    if (row_bytes > 0xFFFF || pixel_bytes > 0x7FFFFFFF) {
        return 0;
    }
    const u32 bitmap = exec_->alloc(ndk::BitMap_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (bitmap == 0) {
        return 0;
    }
    const u32 pixels =
        exec_->alloc(static_cast<u32>(pixel_bytes), ndk::MEMF_FAST | ndk::MEMF_CLEAR);
    DeepBitmap entry{bitmap, pixels, static_cast<u32>(pixel_bytes), width, height, row_bytes,
                     fmt, 0};
    if (pixels == 0 || !deep_bitmaps_.add(entry)) {
        if (pixels != 0) {
            exec_->free(pixels, static_cast<u32>(pixel_bytes));
        }
        exec_->free(bitmap, ndk::BitMap_SIZEOF);
        return 0;
    }
    Bus b(*machine_);
    b.w16(bitmap + ndk::bm_BytesPerRow, row_bytes);
    b.w16(bitmap + ndk::bm_Rows, height);
    b.w8(bitmap + ndk::bm_Depth, kDeepDepth[static_cast<u32>(fmt)]);
    b.w32(bitmap + ndk::bm_Planes, pixels);
    return bitmap;
}

void Graphics::free_deep_bitmap(u32 bitmap) {
    const DeepBitmap* deep = deep_bitmaps_.find(bitmap);
    if (deep == nullptr || exec_ == nullptr) {
        return;
    }
    exec_->free(deep->pixels, deep->pixel_bytes);
    exec_->free(bitmap, ndk::BitMap_SIZEOF);
    deep_bitmaps_.remove(bitmap);
}

bool Graphics::init(Machine& m, Exec& exec, DisplayState& display) {
    machine_ = &m;
    deep_bitmaps_ = DeepBitmaps{};
    for (HostPalette& palette : palettes_) {
        palette = HostPalette{};
    }
    raster::set_deep_lookup(&deep_bitmaps_);
    exec_ = &exec;
    display_ = &display;
    base_ = 0;
    topaz_ = 0;
    topaz_name_ = 0;
    topaz_attr_ = 0;
    vp_mode_ids_.clear();
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const GraphicsFunction& f : kGraphicsFunctions) {
        vectors_[f.lvo / ndk::LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }

    const u32 base = exec.make_native_library(kGraphicsName, kHombreLibVersion, vectors_, kVectors,
                                              this, kGfxBaseSize);
    const u32 attr = exec.alloc(ndk::TextAttr_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (base == 0 || attr == 0) {
        return false;
    }
    base_ = base;
    Bus b(m);
    const u32 fonts = base + gb_TextFonts;
    b.w32(fonts + ndk::LH_HEAD, fonts + ndk::LH_TAIL);
    b.w32(fonts + ndk::LH_TAIL, 0);
    b.w32(fonts + ndk::LH_TAILPRED, fonts + ndk::LH_HEAD);
    b.w8(fonts + ndk::LH_TYPE, kNtFont);

    // The fonts in memory: topaz 8, drawn from the built-in glyphs, then
    // the fonts kept in the ROM.
    RomFont topaz{};
    topaz.name = kTopazName;
    topaz.y_size = raster::kBuiltinSize;
    topaz.x_size = raster::kBuiltinSize;
    topaz.baseline = raster::kBuiltinBaseline;
    topaz.bold_smear = 1;
    topaz.lo_char = raster::kBuiltinLoChar;
    topaz.hi_char = raster::kBuiltinHiChar;
    topaz.modulo = raster::kBuiltinModulo;
    topaz.data = raster::kBuiltinTables.data;
    topaz_name_ = guest_string(exec, b, kTopazName);
    topaz_ = topaz_name_ != 0 ? add_rom_font(b, topaz, topaz_name_) : 0;
    if (topaz_ == 0) {
        return false;
    }
    b.w32(attr + ndk::ta_Name, topaz_name_);
    b.w16(attr + ndk::ta_YSize, raster::kBuiltinSize);
    topaz_attr_ = attr;
    // The default font holds a reference of its own, given back when
    // another font takes its place.
    b.w32(base + gb_DefaultFont, topaz_);
    acquire_font(b, topaz_);

    const char* named = nullptr;
    u32 name = 0;
    RomFont font{};
    for (u32 i = 0; rom_font(i, font); ++i) {
        if (named == nullptr || !same_string(named, font.name)) {
            named = font.name;
            name = guest_string(exec, b, named);
        }
        if (name == 0 || add_rom_font(b, font, name) == 0) {
            return false;
        }
    }
    return b.ok();
}

u32 Graphics::add_rom_font(Bus& b, const RomFont& font, u32 name) {
    const u32 glyphs = font.hi_char - font.lo_char + 2;
    const u32 loc_bytes = glyphs * 4;
    const u32 data_bytes = font.modulo * font.y_size;
    const u32 coverage_bytes = font.coverage != nullptr ? font.coverage_modulo * font.y_size : 0;
    const u32 tf = exec_->alloc(
        ndk::TextFont_SIZEOF + loc_bytes + data_bytes + coverage_bytes,
        ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (tf == 0) {
        return 0;
    }
    const u32 loc = tf + ndk::TextFont_SIZEOF;
    const u32 data = loc + loc_bytes;
    const u32 coverage = data + data_bytes;
    u8* loc_at = b.bytes_at(loc, loc_bytes);
    u8* data_at = b.bytes_at(data, data_bytes);
    u8* coverage_at = coverage_bytes != 0 ? b.bytes_at(coverage, coverage_bytes) : nullptr;
    if (loc_at == nullptr || data_at == nullptr ||
        (coverage_bytes != 0 && coverage_at == nullptr)) {
        return 0;
    }
    raster::build_cell_loc(loc_at, glyphs, font.x_size);
    for (u32 i = 0; i < data_bytes; ++i) {
        data_at[i] = font.data[i];
    }
    for (u32 i = 0; i < coverage_bytes; ++i) {
        coverage_at[i] = font.coverage[i];
    }
    b.w8(tf + ndk::LN_TYPE, kNtFont);
    b.w32(tf + ndk::LN_NAME, name);
    b.w16(tf + ndk::tf_YSize, font.y_size);
    b.w8(tf + ndk::tf_Flags, ndk::FPF_ROMFONT | ndk::FPF_DESIGNED);
    b.w16(tf + ndk::tf_XSize, font.x_size);
    b.w16(tf + ndk::tf_Baseline, font.baseline);
    b.w16(tf + ndk::tf_BoldSmear, font.bold_smear);
    b.w8(tf + ndk::tf_LoChar, font.lo_char);
    b.w8(tf + ndk::tf_HiChar, font.hi_char);
    b.w32(tf + ndk::tf_CharData, data);
    b.w16(tf + ndk::tf_Modulo, font.modulo);
    b.w32(tf + ndk::tf_CharLoc, loc);
    if (coverage_bytes != 0) {
        // The tag list ExtendFont copies onto the font holds Hombre's two
        // coverage tags and their terminator.
        constexpr u32 kTagItems = 3;
        const u32 tags = exec_->alloc(kTagItems * 8, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (tags == 0) {
            return 0;
        }
        b.w32(tags, kTagCoverage);
        b.w32(tags + 4, coverage);
        b.w32(tags + 8, kTagCoverageModulo);
        b.w32(tags + 12, font.coverage_modulo);
        const bool extended = GraphicsNatives::extend(*this, b, tf, tags);
        exec_->free(tags, kTagItems * 8);
        if (!extended) {
            return 0;
        }
    }
    const u32 fonts = base_ + gb_TextFonts;
    const u32 last = b.r32(fonts + ndk::LH_TAILPRED);
    b.w32(tf + ndk::LN_SUCC, fonts + ndk::LH_TAIL);
    b.w32(tf + ndk::LN_PRED, last);
    b.w32(last + ndk::LN_SUCC, tf);
    b.w32(fonts + ndk::LH_TAILPRED, tf);
    return b.ok() ? tf : 0;
}

bool Graphics::text_font(Bus& b, u32 rp, raster::Font& font, raster::TextStyle& style) {
    return text_font(b, rp, rp != 0 ? b.r32(rp + ndk::rp_Font) : 0, font, style);
}

bool Graphics::text_font(Bus& b, u32 rp, u32 own, raster::Font& font,
                         raster::TextStyle& style) {
    if (!raster::load_font(b, own, font)) {
        font = raster::builtin_font();
    } else if (own != 0) {
        const u32 plane = extension_tag(b, own, kTagCoverage);
        const u32 modulo = extension_tag(b, own, kTagCoverageModulo);
        const u64 coverage_bytes = static_cast<u64>(modulo) * static_cast<u32>(font.y_size);
        const u64 cell_pixels =
            static_cast<u64>(font.glyphs) * static_cast<u32>(font.x_size);
        if (plane != 0 && modulo != 0 && coverage_bytes <= 0xFFFFFFFF &&
            static_cast<u64>(modulo) * 2 >= cell_pixels) {
            const u8* coverage =
                b.bytes_at(plane, static_cast<u32>(coverage_bytes));
            if (coverage != nullptr) {
                font.coverage = coverage;
                font.coverage_modulo = modulo;
            }
        }
    }
    style.spacing = rp != 0 ? static_cast<i16>(b.r16(rp + ndk::rp_TxSpacing)) : 0;
    style.algo = rp != 0 ? b.r8(rp + ndk::rp_AlgoStyle) : 0;
    return b.ok();
}

void Graphics::set_vp_mode_id(u32 vp, u32 id) {
    if (vp == 0) {
        return;
    }
    for (VpModeId& pair : vp_mode_ids_) {
        if (pair.vp == vp) {
            pair.id = id;
            return;
        }
    }
    if (vp_mode_ids_.size() < kVpModeIds) {
        vp_mode_ids_.push_back(VpModeId{vp, id});
    }
}

void Graphics::clear_vp_mode_id(u32 vp) {
    std::erase_if(vp_mode_ids_, [vp](const VpModeId& pair) { return pair.vp == vp; });
}

bool Graphics::vp_mode_id(u32 vp, u32& id) const {
    if (vp == 0) {
        return false;
    }
    for (const VpModeId& pair : vp_mode_ids_) {
        if (pair.vp == vp) {
            id = pair.id;
            return true;
        }
    }
    return false;
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
    const u32 font = default_font();
    b.w32(rp + ndk::rp_Font, font);
    b.w16(rp + ndk::rp_TxHeight, b.r16(font + ndk::tf_YSize));
    b.w16(rp + ndk::rp_TxWidth, b.r16(font + ndk::tf_XSize));
    b.w16(rp + ndk::rp_TxBaseline, b.r16(font + ndk::tf_Baseline));
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
    const i32 src_w = static_cast<i32>(raster::pixel_width(src));
    const i32 dst_w = static_cast<i32>(raster::pixel_width(dst));
    width = sx + width > src_w ? src_w - sx : width;
    width = dx + width > dst_w ? dst_w - dx : width;
    height = sy + height > static_cast<i32>(src.rows) ? static_cast<i32>(src.rows) - sy : height;
    height = dy + height > static_cast<i32>(dst.rows) ? static_cast<i32>(dst.rows) - dy : height;
    if (width <= 0 || height <= 0) {
        return 0;
    }
    if (src.deep || dst.deep) {
        blit_deep(b, src, sx, sy, dst, clip, dx, dy, width, height, minterm, mask);
        if (b.ok()) {
            report(display_, *machine_, v, Rect{dx, dy, dx + width - 1, dy + height - 1});
        }
        return 1;
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

// The minterm's result for source B and destination C, bit by bit, as the
// blitter computes it with A set.
static u32 apply_minterm(u32 minterm, u32 src, u32 dst) {
    u32 out = 0;
    out |= (minterm & 0x80) != 0 ? (src & dst) : 0;
    out |= (minterm & 0x40) != 0 ? (src & ~dst) : 0;
    out |= (minterm & 0x20) != 0 ? (~src & dst) : 0;
    out |= (minterm & 0x10) != 0 ? (~src & ~dst) : 0;
    return out;
}

// A blit where either bitmap is deep. A plain copy without a clip region
// moves rows; any other goes pixel by pixel. On LUT8 and planar targets the
// minterm works on the pens' bits under mask, on RGB on the colour bits;
// a source of another kind comes over as the target's colour or pen first.
void Graphics::blit_deep(Bus& b, const raster::Bitmap& src, i32 sx, i32 sy,
                         const raster::Bitmap& dst, const raster::Clip* clip, i32 dx, i32 dy,
                         i32 width, i32 height, u32 minterm, u32 mask) {
    const bool pens = !dst.deep || dst.fmt == PixFmt::Lut8;
    if ((minterm & 0xF0) == 0xC0 && clip == nullptr && (!pens || (mask & 0xFF) == 0xFF)) {
        raster::copy_rect(b, src, sx, sy, dst, dx, dy, width, height);
        return;
    }
    const u32 bits = !pens ? (dst.fmt == PixFmt::Rgb16 ? 0xFFFFu : 0x00FFFFFFu) : (mask & 0xFF);
    const bool upward = src.addr == dst.addr && dy > sy;
    for (i32 n = 0; n < height && b.ok(); ++n) {
        const i32 r = upward ? height - 1 - n : n;
        for (i32 x = 0; x < width; ++x) {
            const i32 tx = dx + x;
            const i32 ty = dy + r;
            if (clip != nullptr && !raster::in_clip(b, *clip, tx, ty)) {
                continue;
            }
            const u32 ux = static_cast<u32>(tx);
            const u32 uy = static_cast<u32>(ty);
            u32 source = 0;
            u32 old = 0;
            if (dst.deep) {
                old = raster::chunky::read_value(b, dst, ux, uy);
                if (src.deep && src.fmt == dst.fmt) {
                    source = raster::chunky::read_value(b, src, static_cast<u32>(sx + x),
                                                        static_cast<u32>(sy + r));
                } else if (src.deep) {
                    source = raster::chunky::rgb_value(
                        dst, raster::chunky::value_rgb(
                                 src, raster::chunky::read_value(b, src, static_cast<u32>(sx + x),
                                                                 static_cast<u32>(sy + r))));
                } else {
                    source = raster::chunky::pen_value(
                        dst, raster::read_pixel(b, src, sx + x, sy + r));
                }
            } else {
                old = raster::read_pixel(b, dst, tx, ty);
                source = raster::read_pixel(b, src, sx + x, sy + r);
            }
            const u32 result = (old & ~bits) | (apply_minterm(minterm, source, old) & bits);
            if (dst.deep) {
                raster::chunky::write_value(b, dst, ux, uy, result);
            } else {
                raster::write_pixel(b, dst, tx, ty, result);
            }
        }
    }
}

bool Graphics::color_of(u32 colormap, u32 index, u8& red, u8& green, u8& blue) {
    if (machine_ == nullptr || colormap == 0) {
        return false;
    }
    Bus b(*machine_);
    const ColorMapTables t = colormap_tables(b, colormap);
    if (!t.ok || index >= t.limit) {
        return false;
    }
    const u32 hi = b.r16(t.table + index * 2);
    const u32 lo = t.low_table != 0 ? b.r16(t.low_table + index * 2) : hi;
    red = static_cast<u8>((((hi >> 8) & 0xF) << 4) | ((lo >> 8) & 0xF));
    green = static_cast<u8>((((hi >> 4) & 0xF) << 4) | ((lo >> 4) & 0xF));
    blue = static_cast<u8>(((hi & 0xF) << 4) | (lo & 0xF));
    return b.ok();
}

text_blend::Cache* Graphics::blend_for(u32 bitmap_addr, u32 depth) {
    if (!text_blend::blends_on(depth) || bitmap_addr == 0 || machine_ == nullptr ||
        display_ == nullptr || display_->palette.colormap_of == nullptr) {
        return nullptr;
    }
    const u32 map = display_->palette.colormap_of(display_->palette.ctx, bitmap_addr);
    if (map == 0) {
        return nullptr;
    }
    BlendCache* entry = nullptr;
    for (u32 i = 0; i < kBlendCaches; ++i) {
        if (blend_caches_[i].map == map) {
            entry = &blend_caches_[i];
            break;
        }
    }
    if (entry == nullptr) {
        entry = &blend_caches_[0];
        for (u32 i = 1; i < kBlendCaches; ++i) {
            const BlendCache& other = blend_caches_[i];
            if (other.map == 0 || (entry->map != 0 && other.used < entry->used)) {
                entry = &blend_caches_[i];
            }
        }
    }
    entry->used = ++blend_clock_;
    if (entry->map == map && entry->palette_gen == display_->palette_gen) {
        return &entry->cache;
    }
    Bus b(*machine_);
    const ColorMapTables t = colormap_tables(b, map);
    if (!t.ok || t.limit == 0) {
        return nullptr;
    }
    const u32 count = t.limit < 256 ? t.limit : 256;
    u8 rgb[256][3]{};
    for (u32 pen = 0; pen < count; ++pen) {
        if (!color_of(map, pen, rgb[pen][0], rgb[pen][1], rgb[pen][2])) {
            return nullptr;
        }
    }
    entry->cache.set_palette(rgb, count);
    entry->map = map;
    entry->palette_gen = display_->palette_gen;
    return &entry->cache;
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
                           : raster::fill_rect(b, v.bitmap, v.clip, area, p.fg,
                                               b.r8(rp + ndk::rp_Mask));
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

u32 Graphics::default_font() const {
    if (machine_ == nullptr || base_ == 0) {
        return topaz_;
    }
    Bus b(*machine_);
    const u32 font = b.r32(base_ + gb_DefaultFont);
    return font != 0 ? font : topaz_;
}

void Graphics::set_default_font(u32 font) {
    if (machine_ == nullptr || base_ == 0) {
        return;
    }
    Bus b(*machine_);
    const u32 previous = b.r32(base_ + gb_DefaultFont);
    if (font == 0) {
        font = topaz_;
    }
    if (font == previous || font == 0) {
        return;
    }
    acquire_font(b, font);
    release_font(b, previous);
    b.w32(base_ + gb_DefaultFont, font);
}

u32 Graphics::open_font(u32 text_attr) {
    if (machine_ == nullptr || base_ == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 name = text_attr != 0 ? b.r32(text_attr + ndk::ta_Name) : 0;
    u32 result = 0;
    i32 best = 0;
    u32 font = name != 0 ? b.r32(base_ + gb_TextFonts + ndk::LH_HEAD) : 0;
    for (u32 i = 0; font != 0 && i < kMaxFonts && b.ok(); ++i) {
        const u32 next = b.r32(font + ndk::LN_SUCC);
        if (next == 0) {
            break;
        }
        if ((b.r8(font + ndk::tf_Flags) & ndk::FPF_REMOVED) == 0 &&
            names_equal_ci(b, name, b.r32(font + ndk::LN_NAME))) {
            const i32 weight = font_match_weight(
                static_cast<i32>(b.r16(text_attr + ndk::ta_YSize)) -
                    static_cast<i32>(b.r16(font + ndk::tf_YSize)),
                b.r8(text_attr + ndk::ta_Style) ^ b.r8(font + ndk::tf_Style),
                b.r8(text_attr + ndk::ta_Flags) ^ b.r8(font + ndk::tf_Flags));
            if (weight > best) {
                best = weight;
                result = font;
            }
        }
        font = next;
    }
    if (result != 0) {
        acquire_font(b, result);
    }
    return b.ok() ? result : 0;
}

void Graphics::close_font(u32 font) {
    if (machine_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    release_font(b, font);
}

void Graphics::acquire_font(Bus& b, u32 font) {
    if (font != 0) {
        b.w16(font + ndk::tf_Accessors, b.r16(font + ndk::tf_Accessors) + 1);
    }
}

void Graphics::release_font(Bus& b, u32 font) {
    if (font != 0 && b.r16(font + ndk::tf_Accessors) != 0) {
        b.w16(font + ndk::tf_Accessors, b.r16(font + ndk::tf_Accessors) - 1);
    }
}

u32 Graphics::text(u32 rp, const u8* chars, u32 count) {
    return draw_text(rp, chars, count, false);
}

u32 Graphics::text_in(u32 rp, u32 font, const u8* chars, u32 count) {
    if (rp == 0 || chars == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    raster::Font loaded = raster::builtin_font();
    raster::TextStyle style;
    if (!text_font(b, rp, font, loaded, style)) {
        return 0;
    }
    return draw_run(b, rp, loaded, style, chars, count, true);
}

u32 Graphics::plain_text_in(u32 rp, u32 font, const u8* chars, u32 count) {
    if (rp == 0 || chars == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    raster::Font loaded = raster::builtin_font();
    raster::TextStyle style;
    if (!text_font(b, 0, font, loaded, style)) {
        return 0;
    }
    return draw_run(b, rp, loaded, style, chars, count, true);
}

u32 Graphics::text_on(Bus& b, const raster::Bitmap& bitmap, u32 bitmap_addr,
                      const raster::Clip& clip, u8 pen, u32 font, i32 x, i32 baseline,
                      const u8* chars, u32 count) {
    raster::Font loaded = raster::builtin_font();
    raster::TextStyle style;
    if (chars == nullptr || !text_font(b, 0, font, loaded, style)) {
        return 0;
    }
    const raster::Pens pens{pen, 0, static_cast<u8>(ndk::JAM1)};
    u32 advance = 0;
    raster::draw_text(b, bitmap, clip, pens, loaded, style, x, baseline, chars, count, advance,
                      blend_for(bitmap_addr, bitmap.depth));
    return advance;
}

u32 Graphics::draw_text(u32 rp, const u8* chars, u32 count, bool own_font) {
    if (rp == 0 || chars == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    raster::Font font = raster::builtin_font();
    raster::TextStyle style;
    if (own_font && !text_font(b, rp, font, style)) {
        return 0;
    }
    return draw_run(b, rp, font, style, chars, count, own_font);
}

u32 Graphics::draw_run(Bus& b, u32 rp, const raster::Font& font,
                       const raster::TextStyle& style, const u8* chars, u32 count,
                       bool blend) {
    const View v = raster::load_rastport(b, rp);
    if (!v.ok) {
        return 0;
    }
    const i32 cursor_x = cp_x(b, rp);
    const i32 cursor_y = cp_y(b, rp);
    text_blend::Cache* cache = blend ? blend_for(v.bitmap_addr, v.bitmap.depth) : nullptr;
    u32 advance = 0;
    const Rect dirty =
        raster::draw_text(b, v.bitmap, v.clip, pens(b, rp), font, style, cursor_x + v.origin_x,
                          cursor_y + v.origin_y, chars, count, advance, cache);
    b.w16(rp + ndk::rp_cp_x, static_cast<u32>(cursor_x) + advance);
    if (b.ok()) {
        report(display_, *machine_, v, dirty);
    }
    return advance;
}

u32 Graphics::text_width(u32 font, const u8* chars, u32 count) {
    if (machine_ == nullptr || chars == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    raster::Font loaded;
    if (!raster::load_font(b, font, loaded)) {
        loaded = raster::builtin_font();
    }
    const raster::TextSpan span = raster::measure_text(loaded, raster::TextStyle{}, chars, count);
    return static_cast<u32>(span.advance);
}

u32 Graphics::text_fit(u32 font, const u8* chars, u32 count, i32 width) {
    if (machine_ == nullptr || chars == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    raster::Font loaded;
    if (!raster::load_font(b, font, loaded)) {
        loaded = raster::builtin_font();
    }
    i64 used = 0;
    u32 fits = 0;
    while (fits < count) {
        used += raster::measure_text(loaded, raster::TextStyle{}, chars + fits, 1).advance;
        if (used > width) {
            break;
        }
        ++fits;
    }
    return fits;
}

void Graphics::hold_font(u32 font) {
    if (machine_ == nullptr) {
        return;
    }
    Bus b(*machine_);
    acquire_font(b, font);
}

Graphics::FontMetrics Graphics::font_metrics(u32 font) {
    raster::Font loaded;
    if (machine_ != nullptr) {
        Bus b(*machine_);
        if (!raster::load_font(b, font, loaded)) {
            loaded = raster::builtin_font();
        }
    } else {
        loaded = raster::builtin_font();
    }
    return FontMetrics{loaded.y_size, loaded.baseline, loaded.x_size};
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
