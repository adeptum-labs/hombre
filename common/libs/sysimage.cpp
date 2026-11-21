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
#include "chrome.hpp"
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

// How far in from its sides row `d` of a round-cornered box starts, `d`
// rows from its nearest top or bottom edge.
i32 corner_inset(i32 d) {
    if (d == 0) {
        return 2;
    }
    return d == 1 ? 1 : 0;
}

// The body of a border gadget, as the window chrome draws its buttons: a
// round-cornered box outlined in the dark pen and filled with the face.
void button_body(Graphics& graphics, u32 rp, const Area& area, u32 face, u32 outline) {
    if (area.width() < 5 || area.height() < 5) {
        frame(graphics, rp, outline, area);
        return;
    }
    for (i32 y = area.y0; y <= area.y1; ++y) {
        const i32 d = y - area.y0 < area.y1 - y ? y - area.y0 : area.y1 - y;
        const i32 in = corner_inset(d);
        if (d == 0) {
            line(graphics, rp, outline, area.x0 + in, y, area.x1 - in, y);
            continue;
        }
        const i32 reach = corner_inset(d - 1) - 1;
        const i32 edge = reach > in ? reach : in;
        if (area.x0 + edge + 1 <= area.x1 - edge - 1) {
            line(graphics, rp, face, area.x0 + edge + 1, y, area.x1 - edge - 1, y);
        }
        line(graphics, rp, outline, area.x0 + in, y, area.x0 + edge, y);
        line(graphics, rp, outline, area.x1 - edge, y, area.x1 - in, y);
    }
}

// The middle of an area, rounded up and left as the window chrome does.
i32 mid(i32 a, i32 b) {
    return (a + b) / 2;
}

// The pens of a title bar button's glyph: its outlines, its light sheets
// and the sheet behind on a depth button.
struct GlyphPens {
    u32 edge;
    u32 light;
    u32 back;
};

// Close: a light square outlined in the dark pen, 7 by 5 as on the window
// chrome.
void draw_close(Graphics& graphics, u32 rp, const Area& area, const GlyphPens& g) {
    const i32 cx = mid(area.x0, area.x1);
    const i32 cy = mid(area.y0, area.y1);
    box(graphics, rp, g.edge, g.light, Area{cx - 3, cy - 2, cx + 3, cy + 2});
}

// Depth: a sheet at the back and a light one in front, lower and to the
// right, each 5 by 5.
void draw_depth(Graphics& graphics, u32 rp, const Area& area, const GlyphPens& g) {
    const i32 cx = mid(area.x0, area.x1);
    const i32 cy = mid(area.y0, area.y1);
    box(graphics, rp, g.edge, g.back, Area{cx - 3, cy - 3, cx + 1, cy + 1});
    box(graphics, rp, g.edge, g.light, Area{cx - 1, cy - 1, cx + 3, cy + 3});
}

// Zoom: a 7 by 7 sheet with a light one in its top left corner.
void draw_zoom(Graphics& graphics, u32 rp, const Area& area, const GlyphPens& g) {
    const i32 cx = mid(area.x0, area.x1);
    const i32 cy = mid(area.y0, area.y1);
    box(graphics, rp, g.edge, g.back, Area{cx - 3, cy - 3, cx + 3, cy + 3});
    box(graphics, rp, g.edge, g.light, Area{cx - 3, cy - 3, cx, cy});
}

// Size: a dark corner pointing to the bottom right with a small light box
// in it, three pixels in from the image's right and bottom edges, as on the
// window chrome.
void draw_size(Graphics& graphics, u32 rp, const Area& area, const u16* pens) {
    if (area.width() < 10 || area.height() < 10) {
        return;
    }
    const i32 right = area.x1 - 3;
    const i32 bottom = area.y1 - 3;
    const u32 dark = pens[ShadowPen];
    line(graphics, rp, dark, right, bottom - 6, right, bottom);
    line(graphics, rp, dark, right - 6, bottom, right, bottom);
    box(graphics, rp, dark, pens[ShinePen], Area{right - 3, bottom - 3, right, bottom});
}

// A triangle pointing the way of which, centred, as on the window
// chrome's arrow buttons: three rows, two to six pixels across, on a small
// image, more on a larger one.
void draw_arrow(Graphics& graphics, u32 rp, const Area& area, u32 pen, u32 which) {
    const i32 w = area.width();
    const i32 h = area.height();
    const bool vertical = which == UPIMAGE || which == DOWNIMAGE;
    const i32 rows = chrome::arrow_rows(vertical ? w : h, vertical ? h : w);
    const i32 mid_x0 = area.x0 + (w - 1) / 2;
    const i32 mid_x1 = area.x0 + w / 2;
    const i32 mid_y0 = area.y0 + (h - 1) / 2;
    const i32 mid_y1 = area.y0 + h / 2;
    const i32 top = area.y0 + (h - rows) / 2;
    const i32 left = area.x0 + (w - rows) / 2;
    for (i32 i = 0; i < rows; ++i) {
        if (vertical) {
            const i32 y = which == UPIMAGE ? top + i : top + rows - 1 - i;
            line(graphics, rp, pen, mid_x0 - i, y, mid_x1 + i, y);
        } else {
            const i32 x = which == LEFTIMAGE ? left + i : left + rows - 1 - i;
            line(graphics, rp, pen, x, mid_y0 - i, x, mid_y1 + i);
        }
    }
}

// A mutual exclude button: a round well, an oval on the tall pixels of a
// high resolution screen. Its rim is a row thick and two columns wide, dark
// along the top and left, light along the bottom and right, and the well
// inside it takes the background. The one chosen holds a dot in the fill
// pen: the same oval again, a row and two columns in from the rim. Nothing
// outside the oval is touched.
void draw_mx(Graphics& graphics, u32 rp, const Area& full, const u16* pens, bool down) {
    // A taller image, as a large reference font gives, keeps the oval about
    // twice as wide as it is high, in the middle.
    const i32 most = full.width() / 2 + 1;
    const i32 tall = full.height() < most ? full.height() : most;
    const i32 top = full.y0 + (full.height() - tall) / 2;
    const Area area{full.x0, top, full.x1, top + tall - 1};
    const i32 w = area.width();
    const i32 h = area.height();
    if (w < 6 || h < 5) {
        return;
    }
    const u32 dark = pens[ShadowPen];
    const u32 light = pens[ShinePen];
    // How far in from either side the oval starts on a row: the first
    // column whose middle lies inside the ellipse that fills the area. The
    // row is measured a quarter of a row nearer the centre, which keeps the
    // top and bottom rows flat. A row outside the area has no oval.
    const auto edge = [w, h](i32 row) {
        if (row < 0 || row >= h) {
            return w;
        }
        i32 dy = 4 * row - 2 * (h - 1);
        dy = (dy < 0 ? -dy : dy) - 1;
        if (dy < 0) {
            dy = 0;
        }
        const i64 tall = 4 * static_cast<i64>(h) * h;
        const i64 room = (tall - static_cast<i64>(dy) * dy) * w * w;
        i32 column = 0;
        while (column < (w - 1) / 2) {
            const i64 dx = w - 2 * column - 1;
            if (dx * dx * tall <= room) {
                break;
            }
            ++column;
        }
        return column;
    };
    // The same for the oval drawn inside itself depth times over, each step
    // taking a row off the top and bottom and two columns off either side.
    const auto inset = [&edge](i32 row, i32 depth) {
        i32 most = 0;
        for (i32 k = -depth; k <= depth; ++k) {
            const i32 at = edge(row + k) + 2 * (depth - (k < 0 ? -k : k));
            if (at > most) {
                most = at;
            }
        }
        return most;
    };
    const auto run = [&graphics, rp](u32 pen, i32 y, i32 left, i32 right) {
        fill(graphics, rp, pen, Area{left, y, right, y});
    };
    // A dot too narrow to see gives way to filling the whole well.
    const i32 dot_depth = w - 2 * inset(h / 2, 2) >= 3 ? 2 : 1;
    for (i32 row = 0; row < h; ++row) {
        const i32 y = area.y0 + row;
        const i32 rim = inset(row, 0);
        const i32 well = inset(row, 1);
        if (2 * well >= w) {
            // The top and bottom of the oval are rim from end to end.
            run(row < h / 2 ? dark : light, y, area.x0 + rim, area.x1 - rim);
            continue;
        }
        run(dark, y, area.x0 + rim, area.x0 + well - 1);
        run(light, y, area.x1 - well + 1, area.x1 - rim);
        run(pens[BackgroundPen], y, area.x0 + well, area.x1 - well);
        if (down) {
            const i32 dot = inset(row, dot_depth);
            run(pens[FillPen], y, area.x0 + dot, area.x1 - dot);
        }
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
    const u32 edge = down ? pens[ShinePen] : pens[ShadowPen];
    switch (which) {
    case DEPTHIMAGE:
    case SDEPTHIMAGE:
    case ZOOMIMAGE:
    case CLOSEIMAGE: {
        // A button body 13 wide in the middle of the image and a row in
        // from its top and bottom, as on the window chrome's title bar.
        // The screen's depth gadget takes the screen bar's pens.
        const bool screen = which == SDEPTHIMAGE;
        const u32 body_face = screen ? pens[BarBlockPen] : face;
        const u32 body_edge = screen ? pens[BarDetailPen] : pens[ShadowPen];
        if (screen) {
            fill(graphics, rp, body_face, outer);
        }
        const i32 cx = mid(outer.x0, outer.x1);
        const i32 inset = outer.height() >= 11 ? 1 : 0;
        const Area body{cx - 6 > outer.x0 ? cx - 6 : outer.x0, outer.y0 + inset,
                        cx + 6 < outer.x1 ? cx + 6 : outer.x1, outer.y1 - inset};
        button_body(graphics, rp, body, down ? body_edge : body_face, body_edge);
        // Held down, the body darkens and the outlines turn light.
        const GlyphPens g{down ? pens[ShinePen] : body_edge, pens[ShinePen],
                          down ? body_edge : pens[BackgroundPen]};
        if (which == CLOSEIMAGE) {
            draw_close(graphics, rp, body, g);
        } else if (which == ZOOMIMAGE) {
            draw_zoom(graphics, rp, body, g);
        } else {
            draw_depth(graphics, rp, body, g);
        }
        break;
    }
    case SIZEIMAGE:
        draw_size(graphics, rp, outer, pens);
        break;
    case LEFTIMAGE:
    case RIGHTIMAGE:
    case UPIMAGE:
    case DOWNIMAGE:
        button_body(graphics, rp, outer, down ? pens[ShadowPen] : face, pens[ShadowPen]);
        draw_arrow(graphics, rp, outer, edge, which);
        break;
    default:
        bevel(graphics, rp, outer, pens[down ? ShadowPen : ShinePen],
              pens[down ? ShinePen : ShadowPen]);
        break;
    }
}

}
