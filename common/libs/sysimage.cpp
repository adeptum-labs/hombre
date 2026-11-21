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

#include "sysimage.hpp"

#include "boopsi.hpp"
#include "graphics.hpp"

namespace hombre::sysimage {

using namespace ndk;

namespace {

constexpr u32 TextPen = 2;
constexpr u32 ShinePen = 3;
constexpr u32 ShadowPen = 4;
constexpr u32 FillPen = 5;
constexpr u32 BackgroundPen = 7;

constexpr u16 kDefaultPens[kPenCount] = {0, 1, 1, 2, 1, 3, 1, 0, 2};

constexpr i32 kMinGlyph = 3;

bool selected(u32 state) {
    return state == IDS_SELECTED || state == IDS_INACTIVESELECTED;
}

// An inclusive area of the image; an empty one has x1 or y1 before its
// start.
struct Area {
    i32 x0;
    i32 y0;
    i32 x1;
    i32 y1;

    i32 width() const { return x1 >= x0 ? x1 - x0 + 1 : 0; }
    i32 height() const { return y1 >= y0 ? y1 - y0 + 1 : 0; }
    bool empty() const { return x1 < x0 || y1 < y0; }
};

Area shrink(const Area& area, i32 by) {
    return Area{area.x0 + by, area.y0 + by, area.x1 - by, area.y1 - by};
}

void fill(Graphics& graphics, u32 rp, u32 pen, const Area& area) {
    if (area.empty()) {
        return;
    }
    graphics.set_pens(rp, pen, 0, JAM1);
    graphics.rect_fill(rp, area.x0, area.y0, area.x1, area.y1);
}

void line(Graphics& graphics, u32 rp, u32 pen, i32 x0, i32 y0, i32 x1, i32 y1) {
    graphics.set_pens(rp, pen, 0, JAM1);
    graphics.move(rp, x0, y0);
    graphics.draw_line(rp, x1, y1);
}

void frame(Graphics& graphics, u32 rp, u32 pen, const Area& area) {
    if (area.empty()) {
        return;
    }
    line(graphics, rp, pen, area.x0, area.y0, area.x1, area.y0);
    line(graphics, rp, pen, area.x1, area.y0, area.x1, area.y1);
    line(graphics, rp, pen, area.x1, area.y1, area.x0, area.y1);
    line(graphics, rp, pen, area.x0, area.y1, area.x0, area.y0);
}

void bevel(Graphics& graphics, u32 rp, const Area& area, u32 light, u32 dark) {
    line(graphics, rp, light, area.x0, area.y0, area.x1, area.y0);
    line(graphics, rp, light, area.x0, area.y0, area.x0, area.y1);
    line(graphics, rp, dark, area.x0, area.y1, area.x1, area.y1);
    line(graphics, rp, dark, area.x1, area.y0, area.x1, area.y1);
}

// A small square with a lit top-left edge.
void draw_close(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    const i32 side = (area.width() < area.height() ? area.width() : area.height()) - 2;
    if (side < kMinGlyph) {
        return;
    }
    const i32 x0 = area.x0 + (area.width() - side) / 2;
    const i32 y0 = area.y0 + (area.height() - side) / 2;
    const Area square{x0, y0, x0 + side - 1, y0 + side - 1};
    frame(graphics, rp, pens[ShadowPen], square);
    line(graphics, rp, pens[TextPen], square.x0, square.y0, square.x1, square.y0);
    line(graphics, rp, pens[TextPen], square.x0, square.y0, square.x0, square.y1);
}

// Two rectangles overlapping at their corners.
void draw_depth(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    if (area.width() < 8 || area.height() < 6) {
        draw_close(graphics, rp, area, pens);
        return;
    }
    const i32 back_w = area.width() - 4;
    const i32 back_h = area.height() - 4;
    frame(graphics, rp, pens[ShadowPen],
          Area{area.x0, area.y0, area.x0 + back_w - 1, area.y0 + back_h - 1});
    frame(graphics, rp, pens[TextPen], Area{area.x0 + 2, area.y0 + 2, area.x1, area.y1});
}

// A rectangle inside a rectangle.
void draw_zoom(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    frame(graphics, rp, pens[ShadowPen], area);
    const Area inner = shrink(area, 2);
    if (inner.empty()) {
        return;
    }
    frame(graphics, rp, pens[TextPen], inner);
}

// Two lines forming the bottom-right corner.
void draw_size(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    line(graphics, rp, pens[TextPen], area.x1, area.y0, area.x1, area.y1);
    line(graphics, rp, pens[TextPen], area.x1, area.y1, area.x0, area.y1);
    if (area.width() > 2 && area.height() > 2) {
        line(graphics, rp, pens[ShadowPen], area.x1 - 1, area.y0, area.x1 - 1, area.y1 - 1);
        line(graphics, rp, pens[ShadowPen], area.x1 - 1, area.y1 - 1, area.x0, area.y1 - 1);
    }
}

// A filled triangle pointing the way of which.
void draw_arrow(Graphics& graphics, u32 rp, const Area& area, const u16* pens, u32 which) {
    i32 span = area.width() < area.height() ? area.width() : area.height();
    if (span < kMinGlyph) {
        return;
    }
    if ((span & 1) == 0) {
        --span;
    }
    const i32 half = span / 2;
    const i32 cx = (area.x0 + area.x1) / 2;
    const i32 cy = (area.y0 + area.y1) / 2;
    for (i32 d = -half; d <= half; ++d) {
        const i32 depth = d < 0 ? -d : d;
        switch (which) {
        case LEFTIMAGE:
            line(graphics, rp, pens[TextPen], cx - half + depth, cy + d, cx + half, cy + d);
            break;
        case RIGHTIMAGE:
            line(graphics, rp, pens[TextPen], cx - half, cy + d, cx + half - depth, cy + d);
            break;
        case UPIMAGE:
            line(graphics, rp, pens[TextPen], cx + d, cy - half + depth, cx + d, cy + half);
            break;
        default:
            line(graphics, rp, pens[TextPen], cx + d, cy - half, cx + d, cy + half - depth);
            break;
        }
    }
}

// A check mark from the left, down to the middle and up to the right.
void draw_check(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    if (area.width() < 6 || area.height() < 4) {
        return;
    }
    const i32 cx = (area.x0 + area.x1) / 2;
    const i32 cy = (area.y0 + area.y1) / 2;
    line(graphics, rp, pens[TextPen], area.x0 + 1, cy, cx - 1, area.y1 - 1);
    line(graphics, rp, pens[TextPen], cx - 1, area.y1 - 1, area.x1 - 1, area.y0 + 1);
}

// A small rounded button, filled when selected and framed otherwise.
void draw_mx(Graphics& graphics, u32 rp, const Area& area, const u16* pens, bool down) {
    i32 size = area.width() < area.height() ? area.width() : area.height();
    if (size < kMinGlyph) {
        return;
    }
    if ((size & 1) == 0) {
        --size;
    }
    const i32 half = size / 2;
    const i32 cx = (area.x0 + area.x1) / 2;
    const i32 cy = (area.y0 + area.y1) / 2;
    if (!down) {
        frame(graphics, rp, pens[ShadowPen],
              Area{cx - half, cy - half, cx + half, cy + half});
        line(graphics, rp, pens[TextPen], cx - half + 1, cy - half + 1, cx + half - 1,
             cy - half + 1);
        return;
    }
    for (i32 d = -half; d <= half; ++d) {
        const i32 cut = (d == -half || d == half) ? 1 : 0;
        line(graphics, rp, pens[TextPen], cx - half + cut, cy + d, cx + half - cut, cy + d);
    }
}

// The stylised Amiga key: an A within a box.
void draw_amigakey(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    if (area.width() < 6 || area.height() < 4) {
        return;
    }
    frame(graphics, rp, pens[TextPen], area);
    const i32 left = area.x0 + 3;
    const i32 right = area.x1 - 3;
    const i32 top = area.y0 + 1;
    const i32 bottom = area.y1 - 1;
    if (right <= left || bottom <= top) {
        return;
    }
    const i32 apex = (left + right) / 2;
    line(graphics, rp, pens[TextPen], left, bottom, apex, top);
    line(graphics, rp, pens[TextPen], apex, top, right, bottom);
    const i32 bar = top + (bottom - top) * 2 / 3;
    line(graphics, rp, pens[TextPen], left + (right - left) / 4, bar,
         right - (right - left) / 4, bar);
}

}

void read_pens(native::Bus& bus, u32 draw_info, u16 (&pens)[kPenCount]) {
    for (u32 i = 0; i < kPenCount; ++i) {
        pens[i] = kDefaultPens[i];
    }
    if (draw_info == 0) {
        return;
    }
    const u32 at = bus.r32(draw_info + 4);
    if (at == 0 || !bus.ok()) {
        return;
    }
    for (u32 i = 0; i < kPenCount; ++i) {
        const u16 pen = bus.r16(at + i * 2);
        if (!bus.ok()) {
            return;
        }
        pens[i] = pen;
    }
}

void draw(Graphics& graphics, native::Bus&, u32 rp, u32 which, const Rect& box, u32 state,
          const u16* pens) {
    if (rp == 0 || pens == nullptr || box.x1 < box.x0 || box.y1 < box.y0) {
        return;
    }
    const Area outer{box.x0, box.y0, box.x1, box.y1};
    // The menu check and Amiga key show only their glyph.
    if (which == MENUCHECK) {
        draw_check(graphics, rp, shrink(outer, 1), pens);
        return;
    }
    if (which == AMIGAKEY) {
        draw_amigakey(graphics, rp, outer, pens);
        return;
    }
    const bool down = selected(state);
    fill(graphics, rp, pens[down ? FillPen : BackgroundPen], outer);
    bevel(graphics, rp, outer, pens[down ? ShadowPen : ShinePen],
          pens[down ? ShinePen : ShadowPen]);
    const Area glyph = shrink(outer, 2);
    if (glyph.empty()) {
        return;
    }
    switch (which) {
    case DEPTHIMAGE:
        draw_depth(graphics, rp, glyph, pens);
        break;
    case ZOOMIMAGE:
        draw_zoom(graphics, rp, glyph, pens);
        break;
    case SDEPTHIMAGE:
        draw_depth(graphics, rp, glyph, pens);
        break;
    case CLOSEIMAGE:
        draw_close(graphics, rp, glyph, pens);
        break;
    case SIZEIMAGE:
        draw_size(graphics, rp, glyph, pens);
        break;
    case LEFTIMAGE:
    case RIGHTIMAGE:
    case UPIMAGE:
    case DOWNIMAGE:
        draw_arrow(graphics, rp, glyph, pens, which);
        break;
    case CHECKIMAGE:
        if (down) {
            draw_check(graphics, rp, glyph, pens);
        }
        break;
    case MXIMAGE:
        draw_mx(graphics, rp, glyph, pens, down);
        break;
    default:
        break;
    }
}

}
