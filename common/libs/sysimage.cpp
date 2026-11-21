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
constexpr u32 BarDetailPen = 9;
constexpr u32 BarBlockPen = 10;

constexpr u16 kDefaultPens[kPenCount] = {0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1};

bool selected(u32 state) {
    return state == IDS_SELECTED || state == IDS_INACTIVESELECTED;
}

bool inactive(u32 state) {
    return state == IDS_INACTIVENORMAL || state == IDS_INACTIVESELECTED;
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

Area shrink(const Area& area, i32 dx, i32 dy) {
    return Area{area.x0 + dx, area.y0 + dy, area.x1 - dx, area.y1 - dy};
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

// A box drawn in the outline pen and filled with the fill pen.
void box(Graphics& graphics, u32 rp, u32 outline, u32 inside, const Area& area) {
    fill(graphics, rp, inside, shrink(area, 1, 1));
    frame(graphics, rp, outline, area);
}

// A raised edge: light top and left, dark bottom and right; a pressed one
// the other way round.
void bevel(Graphics& graphics, u32 rp, const Area& area, u32 light, u32 dark) {
    line(graphics, rp, light, area.x0, area.y1, area.x0, area.y0);
    line(graphics, rp, light, area.x0, area.y0, area.x1, area.y0);
    line(graphics, rp, dark, area.x1, area.y0, area.x1, area.y1);
    line(graphics, rp, dark, area.x1, area.y1, area.x0 + 1, area.y1);
}

// The check mark of a checkbox or a menu: a short stroke down to the bottom
// and a long one up to the top right, two pixels wide as on a high
// resolution screen.
void check_mark(Graphics& graphics, u32 rp, u32 pen, const Area& area) {
    if (area.width() < 6 || area.height() < 3) {
        return;
    }
    const i32 w = area.width();
    const i32 bottom_x = area.x0 + w * 2 / 5;
    const i32 mid_y = area.y0 + area.height() / 2;
    for (i32 dx = 0; dx < 2; ++dx) {
        line(graphics, rp, pen, area.x0 + dx, mid_y, bottom_x + dx - 1, area.y1);
        line(graphics, rp, pen, bottom_x + dx - 1, area.y1, area.x1 - 1 + dx, area.y0);
    }
}

// Close: a small white square outlined in the dark pen, in the middle.
void draw_close(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    const i32 h = area.height() * 3 / 5;
    const i32 w = h + 2;
    if (w < 3 || h < 3) {
        return;
    }
    const i32 x0 = area.x0 + (area.width() - w) / 2;
    const i32 y0 = area.y0 + (area.height() - h) / 2;
    box(graphics, rp, pens[ShadowPen], pens[ShinePen], Area{x0, y0, x0 + w - 1, y0 + h - 1});
}

// Depth: an outlined window at the back, a white one overlapping it in
// front, lower and to the right.
void draw_depth(Graphics& graphics, u32 rp, const Area& area, const u16* pens, u32 face) {
    const Area inner = shrink(area, 2, 1);
    const i32 w = inner.width() * 3 / 5;
    const i32 h = inner.height() * 3 / 5;
    if (w < 3 || h < 3) {
        return;
    }
    box(graphics, rp, pens[ShadowPen], face,
        Area{inner.x0, inner.y0, inner.x0 + w - 1, inner.y0 + h - 1});
    box(graphics, rp, pens[ShadowPen], pens[ShinePen],
        Area{inner.x1 - w + 1, inner.y1 - h + 1, inner.x1, inner.y1});
}

// Zoom: an outlined window with a small white one in its top left corner.
void draw_zoom(Graphics& graphics, u32 rp, const Area& area, const u16* pens, u32 face) {
    const Area outer = shrink(area, 3, 1);
    if (outer.width() < 5 || outer.height() < 4) {
        return;
    }
    box(graphics, rp, pens[ShadowPen], face, outer);
    const i32 w = outer.width() / 2;
    const i32 h = outer.height() / 2 + 1;
    box(graphics, rp, pens[ShadowPen], pens[ShinePen],
        Area{outer.x0, outer.y0, outer.x0 + w - 1, outer.y0 + h - 1});
}

// Size: two corners, one inside the other, pointing to the bottom right.
void draw_size(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    const Area outer = shrink(area, 2, 1);
    if (outer.width() < 4 || outer.height() < 4) {
        return;
    }
    line(graphics, rp, pens[ShadowPen], outer.x1, outer.y0, outer.x1, outer.y1);
    line(graphics, rp, pens[ShadowPen], outer.x1, outer.y1, outer.x0, outer.y1);
    const i32 mx = (outer.x0 + outer.x1) / 2;
    const i32 my = (outer.y0 + outer.y1) / 2;
    box(graphics, rp, pens[ShadowPen], pens[ShinePen], Area{mx, my, outer.x1, outer.y1});
}

// A solid triangle in the dark pen pointing the way of which.
void draw_arrow(Graphics& graphics, u32 rp, const Area& area, const u16* pens, u32 which) {
    const bool horizontal = which == LEFTIMAGE || which == RIGHTIMAGE;
    // Along the arrow's axis it takes half the room, across it all of it.
    const i32 across = horizontal ? area.height() - 2 : area.width() - 4;
    i32 half = across / 2;
    if (half < 1) {
        return;
    }
    const i32 cx = (area.x0 + area.x1) / 2;
    const i32 cy = (area.y0 + area.y1) / 2;
    const u32 pen = pens[ShadowPen];
    if (horizontal) {
        const i32 depth = half;
        for (i32 d = 0; d <= depth; ++d) {
            const i32 reach = half * d / depth;
            const i32 x = which == LEFTIMAGE ? cx - depth / 2 + d : cx + depth / 2 - d;
            line(graphics, rp, pen, x, cy - reach, x, cy + reach);
        }
    } else {
        const i32 depth = half / 2 > 1 ? half / 2 : 1;
        for (i32 d = 0; d <= depth; ++d) {
            const i32 reach = half * d / depth;
            const i32 y = which == UPIMAGE ? cy - depth / 2 + d : cy + depth / 2 - d;
            line(graphics, rp, pen, cx - reach, y, cx + reach, y);
        }
    }
}

// A mutual exclude button: a sunken round-cornered well, filled when it is
// the one chosen.
void draw_mx(Graphics& graphics, u32 rp, const Area& area, const u16* pens, bool down) {
    if (area.width() < 6 || area.height() < 5) {
        return;
    }
    const u32 dark = pens[ShadowPen];
    const u32 light = pens[ShinePen];
    line(graphics, rp, dark, area.x0 + 2, area.y0, area.x1 - 2, area.y0);
    line(graphics, rp, dark, area.x0, area.y0 + 2, area.x0, area.y1 - 2);
    line(graphics, rp, dark, area.x0 + 1, area.y0 + 1, area.x0 + 1, area.y1 - 1);
    line(graphics, rp, light, area.x0 + 2, area.y1, area.x1 - 2, area.y1);
    line(graphics, rp, light, area.x1, area.y0 + 2, area.x1, area.y1 - 2);
    line(graphics, rp, light, area.x1 - 1, area.y0 + 1, area.x1 - 1, area.y1 - 1);
    const Area well = shrink(area, 3, 2);
    fill(graphics, rp, down ? pens[FillPen] : pens[BackgroundPen], well);
    if (down) {
        line(graphics, rp, dark, well.x0, well.y0, well.x1, well.y0);
    }
}

// The Amiga key: a box in the menu text pen with an "A" in the menu's
// own pen.
void draw_amigakey(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    if (area.width() < 7 || area.height() < 5) {
        return;
    }
    const Area keycap = shrink(area, 1, 0);
    fill(graphics, rp, pens[BarDetailPen], keycap);
    const i32 left = keycap.x0 + keycap.width() / 4;
    const i32 right = keycap.x1 - keycap.width() / 4;
    const i32 top = keycap.y0 + 1;
    const i32 bottom = keycap.y1 - 1;
    const i32 apex = (left + right) / 2;
    const u32 pen = pens[BarBlockPen];
    for (i32 dx = 0; dx < 2; ++dx) {
        line(graphics, rp, pen, left + dx, bottom, apex + dx, top);
        line(graphics, rp, pen, apex + dx, top, right + dx - 1, bottom);
    }
    const i32 bar = top + (bottom - top) * 2 / 3;
    line(graphics, rp, pen, left + (apex - left) / 2 + 1, bar, right - (right - apex) / 2, bar);
}

// The images that sit in a window's border, which take its colour.
bool border_gadget(u32 which) {
    return which == DEPTHIMAGE || which == ZOOMIMAGE || which == SDEPTHIMAGE ||
           which == CLOSEIMAGE || which == SIZEIMAGE || which == LEFTIMAGE ||
           which == RIGHTIMAGE || which == UPIMAGE || which == DOWNIMAGE;
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
        // A DrawInfo of fewer pens ends its list early; the rest keep
        // their defaults.
        if (!bus.ok() || pen == 0xFFFF) {
            return;
        }
        pens[i] = pen;
    }
}

void draw(Graphics& graphics, native::Bus&, u32 rp, u32 which, const Rect& box_rect, u32 state,
          const u16* pens) {
    if (rp == 0 || pens == nullptr || box_rect.x1 < box_rect.x0 || box_rect.y1 < box_rect.y0) {
        return;
    }
    const Area outer{box_rect.x0, box_rect.y0, box_rect.x1, box_rect.y1};
    const bool down = selected(state);
    // The menu check and the Amiga key are drawn on the menu, in its pens.
    if (which == MENUCHECK) {
        check_mark(graphics, rp, pens[BarDetailPen], shrink(outer, 1, 1));
        return;
    }
    if (which == AMIGAKEY) {
        draw_amigakey(graphics, rp, outer, pens);
        return;
    }
    if (which == MXIMAGE) {
        draw_mx(graphics, rp, outer, pens, down);
        return;
    }
    // The border gadgets, the arrows among them, take the colour of the
    // border they sit in: the fill pen of an active window, the background
    // of an inactive one. The checkbox is background coloured.
    const u32 face = border_gadget(which) && !inactive(state) ? pens[FillPen]
                                                              : pens[BackgroundPen];
    fill(graphics, rp, face, outer);
    if (which == CHECKIMAGE) {
        bevel(graphics, rp, outer, pens[ShinePen], pens[ShadowPen]);
        if (down) {
            check_mark(graphics, rp, pens[TextPen], shrink(outer, 4, 2));
        }
        return;
    }
    bevel(graphics, rp, outer, pens[down ? ShadowPen : ShinePen],
          pens[down ? ShinePen : ShadowPen]);
    const Area glyph = shrink(outer, 1, 1);
    switch (which) {
    case DEPTHIMAGE:
    case SDEPTHIMAGE:
        draw_depth(graphics, rp, glyph, pens, face);
        break;
    case ZOOMIMAGE:
        draw_zoom(graphics, rp, glyph, pens, face);
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
    default:
        break;
    }
}

}
