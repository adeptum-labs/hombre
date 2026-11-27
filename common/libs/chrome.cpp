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

#include "chrome.hpp"

#include "boopsi.hpp"
#include "chrome_amiga.hpp"
#include "chrome_paint.hpp"
#include "chrome_palette.hpp"
#include "graphics.hpp"
#include "intuition.hpp"

namespace hombre::chrome {

using native::Bus;
using sysimage::BackgroundPen;
using sysimage::BarBlockPen;
using sysimage::BarDetailPen;
using sysimage::BarTrimPen;
using sysimage::FillPen;
using sysimage::FillTextPen;
using sysimage::ShadowPen;
using sysimage::ShinePen;
using sysimage::TextPen;

namespace {

constexpr u32 kMaxTitleChars = 255;

struct RpState {
    u8 fg = 0;
    u8 bg = 0;
    u8 mode = 0;
    u16 cx = 0;
    u16 cy = 0;
    u32 pattern = 0;
};

// The chrome is drawn solid: an area pattern the program left in the
// RastPort is put aside while it is.
RpState save_rp(Bus& b, u32 rp) {
    const RpState s{
        static_cast<u8>(b.r8(rp + ndk::rp_FgPen)),
        static_cast<u8>(b.r8(rp + ndk::rp_BgPen)),
        static_cast<u8>(b.r8(rp + ndk::rp_DrawMode)),
        static_cast<u16>(b.r16(rp + ndk::rp_cp_x)),
        static_cast<u16>(b.r16(rp + ndk::rp_cp_y)),
        b.r32(rp + ndk::rp_AreaPtrn),
    };
    b.w32(rp + ndk::rp_AreaPtrn, 0);
    return s;
}

void restore_rp(Bus& b, u32 rp, const RpState& s) {
    b.w8(rp + ndk::rp_FgPen, s.fg);
    b.w8(rp + ndk::rp_BgPen, s.bg);
    b.w8(rp + ndk::rp_DrawMode, s.mode);
    b.w16(rp + ndk::rp_cp_x, s.cx);
    b.w16(rp + ndk::rp_cp_y, s.cy);
    b.w32(rp + ndk::rp_AreaPtrn, s.pattern);
}

u32 rastport_depth(Bus& b, u32 rp) {
    const u32 bitmap = b.r32(rp + ndk::rp_BitMap);
    if (bitmap == 0 || !b.ok()) {
        return 0;
    }
    const u32 depth = b.r8(bitmap + ndk::bm_Depth);
    return b.ok() ? depth : 0;
}

// The guest string at text, at most kMaxTitleChars of it. How many bytes it
// holds, or none when the bus cannot read it.
u32 read_title(Bus& b, u32 text, u8* out) {
    u32 length = 0;
    while (text != 0 && length < kMaxTitleChars) {
        const u8 c = static_cast<u8>(b.r8(text + length));
        if (!b.ok() || c == 0) {
            break;
        }
        out[length++] = c;
    }
    return b.ok() ? length : 0;
}

// A one-pixel edge round the box: raised when light is the pen of its top
// and left sides and dark that of its bottom and right ones, sunken the
// other way round. As on the border images, the right side owns the top
// right corner and the left side the bottom left one.
void draw_bevel(Graphics& graphics, u32 rp, const Rect& box, u32 light, u32 dark) {
    if (box.x1 < box.x0 || box.y1 < box.y0) {
        return;
    }
    graphics.set_pens(rp, light, 0, ndk::JAM2);
    graphics.rect_fill(rp, box.x0, box.y0, box.x1, box.y0);
    graphics.rect_fill(rp, box.x0, box.y0, box.x0, box.y1);
    graphics.set_pens(rp, dark, 0, ndk::JAM2);
    graphics.rect_fill(rp, box.x1, box.y0, box.x1, box.y1);
    graphics.rect_fill(rp, box.x0 + 1, box.y1, box.x1, box.y1);
}

// The knob of a classic scroll gadget: a raised box in the background pen,
// or in the fill pen while it is dragged.
void draw_classic_knob(Graphics& graphics, u32 rp, const Rect& knob, bool selected,
                       const u16* pens) {
    graphics.set_pens(rp, selected ? pens[FillPen] : pens[BackgroundPen], 0, ndk::JAM2);
    graphics.rect_fill(rp, knob.x0, knob.y0, knob.x1, knob.y1);
    draw_bevel(graphics, rp, knob, pens[ShinePen], pens[ShadowPen]);
}

// How the system images show a gadget of a window that is active or not,
// held down or not.
u32 gadget_state(bool active, bool pressed) {
    if (pressed) {
        return ndk::IDS_SELECTED;
    }
    return active ? ndk::IDS_NORMAL : ndk::IDS_INACTIVENORMAL;
}

// The frame of a window as on 3.1: the title bar and the borders in the
// fill pen of an active window and the background pen of an inactive one, a
// raised edge round the window and a sunken one round its inside, whose top
// line is the one under the bar. The gadgets are the system images, each in
// a cell as high as the bar, from the window's top line to the line under
// the bar. An outer cell reaches the window's edge.
void draw_classic_frame(Graphics& graphics, Bus& b, u32 rp, const Surface& surface,
                        const WindowChrome& w) {
    const u16* pens = w.pens.pen;
    const i32 right = w.width - 1;
    const i32 bottom = w.height - 1;
    const i32 inside_x0 = static_cast<i32>(w.border_left);
    const i32 inside_y0 = static_cast<i32>(w.border_top);
    const i32 inside_x1 = right - static_cast<i32>(w.border_right);
    const i32 inside_y1 = bottom - static_cast<i32>(w.border_bottom);

    graphics.set_pens(rp, w.active ? pens[FillPen] : pens[BackgroundPen], 0, ndk::JAM2);
    graphics.rect_fill(rp, 0, 0, right, inside_y0 - 1);
    graphics.rect_fill(rp, 0, inside_y0, inside_x0 - 1, bottom);
    graphics.rect_fill(rp, inside_x1 + 1, inside_y0, right, bottom);
    graphics.rect_fill(rp, inside_x0, inside_y1 + 1, inside_x1, bottom);
    draw_bevel(graphics, rp, Rect{0, 0, right, bottom}, pens[ShinePen], pens[ShadowPen]);
    draw_bevel(graphics, rp, Rect{inside_x0 - 1, inside_y0 - 1, inside_x1 + 1, inside_y1 + 1},
               pens[ShadowPen], pens[ShinePen]);

    const i32 bar = w.metrics.bar_height();
    const bool has_depth = (w.flags & ndk::WFLG_DEPTHGADGET) != 0;
    if ((w.flags & ndk::WFLG_CLOSEGADGET) != 0) {
        // The close cell's last column is the light line that starts the
        // title.
        const Rect cell{0, 0, close_button_box(w.metrics).x1 + 1, bar};
        sysimage::draw(graphics, b, rp, ndk::CLOSEIMAGE, cell,
                       gadget_state(w.active, w.pressed == BarButton::Close), pens);
    }
    // The zoom and depth gadgets share a column, so the one held is drawn
    // last for its sunken edge to show.
    const bool zoom_held = w.pressed == BarButton::Zoom;
    const auto draw_zoom = [&] {
        if ((w.flags & ndk::WFLG_HASZOOM) != 0) {
            // Beside a depth gadget the zoom cell takes a column of the
            // title's gap for the dark line that ends the title; in the
            // depth gadget's place it reaches the window's edge.
            const Rect box = zoom_button_box(w.width, w.flags, w.metrics);
            const Rect cell = has_depth ? Rect{box.x0 - 1, 0, box.x1, bar}
                                        : Rect{box.x0, 0, right, bar};
            sysimage::draw(graphics, b, rp, ndk::ZOOMIMAGE, cell,
                           gadget_state(w.active, zoom_held), pens);
            if (!has_depth) {
                // The image's top line ends in its last column; at the
                // window's edge that corner is the right edge's.
                graphics.set_pens(rp, pens[zoom_held ? ShinePen : ShadowPen], 0, ndk::JAM2);
                graphics.rect_fill(rp, right, 0, right, 0);
            }
        }
    };
    if (!zoom_held) {
        draw_zoom();
    }
    if (has_depth) {
        const Rect cell{depth_button_box(w.width, w.metrics).x0, 0, right, bar};
        sysimage::draw(graphics, b, rp, ndk::DEPTHIMAGE, cell,
                       gadget_state(w.active, w.pressed == BarButton::Depth), pens);
    }
    if (zoom_held) {
        draw_zoom();
    }
    if ((w.flags & ndk::WFLG_SIZEGADGET) != 0 && w.width >= static_cast<i32>(kSizeBorderRight) &&
        w.height >= static_cast<i32>(kSizeBorderBottom)) {
        const Rect corner{w.width - static_cast<i32>(kSizeBorderRight),
                          w.height - static_cast<i32>(kSizeBorderBottom), right, bottom};
        sysimage::draw(graphics, b, rp, ndk::SIZEIMAGE, corner, gadget_state(w.active, false),
                       pens);
    }
    u8 chars[kMaxTitleChars];
    const Title title{chars, read_title(b, w.title, chars)};
    paint_bar_title(surface, w, title,
                    static_cast<u8>(w.active ? pens[FillTextPen] : pens[TextPen]));
}

// The menu separator on screens of eight planes: a dark line over a light
// one, as the window frames use.
constexpr u8 kMenuBarDark = kKnob2;
constexpr u8 kMenuBarLight = kTitleActive;

constexpr i32 kMenuTextInset = 4;
// How far a title's name starts in from the left of its box.
constexpr i32 kMenuTitleInset = 8;
constexpr i32 kMenuCheckColumn = 16;
constexpr i32 kMenuCornerRadius = 2;

// Small pictures, one string per row, 'X' for a set pixel.
constexpr const char* kCheckGlyph[] = {
    "........X",
    ".......XX",
    "X.....XX.",
    "XX...XX..",
    ".XX.XX...",
    "..XXX....",
    "...X.....",
};
constexpr i32 kCheckGlyphWidth = 9;
constexpr i32 kCheckGlyphHeight = 7;

constexpr const char* kAmigaKeyGlyph[] = {
    ".XXXXXXXX.",
    "X........X",
    "X...XX...X",
    "X..X..X..X",
    "X..XXXX..X",
    "X..X..X..X",
    "X..X..X..X",
    "X........X",
    ".XXXXXXXX.",
};
constexpr i32 kAmigaKeyGlyphWidth = 10;
constexpr i32 kAmigaKeyGlyphHeight = 9;

constexpr const char* kSubmenuGlyph[] = {
    "X...",
    "XX..",
    "XXX.",
    "XXXX",
    "XXX.",
    "XX..",
    "X...",
};
constexpr i32 kSubmenuGlyphWidth = 4;
constexpr i32 kSubmenuGlyphHeight = 7;

Rect clipped(const Rect& r, const raster::Clip& clip) {
    return Rect{
        r.x0 > clip.x0 ? r.x0 : clip.x0,
        r.y0 > clip.y0 ? r.y0 : clip.y0,
        r.x1 < clip.x1 ? r.x1 : clip.x1,
        r.y1 < clip.y1 ? r.y1 : clip.y1,
    };
}

void fill(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip, i32 x0, i32 y0, i32 x1,
          i32 y1, u8 pen) {
    if (x0 <= x1 && y0 <= y1) {
        raster::fill_rect(b, bitmap, clip, Rect{x0, y0, x1, y1}, pen);
    }
}

void draw_glyph(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                const char* const* rows, i32 width, i32 height, i32 x, i32 y, u8 pen) {
    for (i32 row = 0; row < height; ++row) {
        for (i32 col = 0; col < width; ++col) {
            if (rows[row][col] == 'X') {
                fill(b, bitmap, clip, x + col, y + row, x + col, y + row, pen);
            }
        }
    }
}

// Every other pixel of r in the paper pen: the classic ghosting of a
// disabled entry where no grey pen exists.
void ghost(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip, const Rect& r,
           u8 paper) {
    const Rect c = clipped(r, clip);
    for (i32 y = c.y0; y <= c.y1; ++y) {
        for (i32 x = c.x0 + ((c.x0 + y) & 1); x <= c.x1; x += 2) {
            raster::write_pixel(b, bitmap, x, y, paper);
        }
    }
}

// Where menus are drawn: the bitmap, its address for the palette text
// blends with, and the clip.
struct MenuTarget {
    Graphics& graphics;
    Bus& b;
    const raster::Bitmap& bitmap;
    u32 bitmap_addr;
    const raster::Clip& clip;
};

// The guest string at text in font, cut to the whole characters that fit
// before right, with its baseline at baseline.
void draw_guest_text(const MenuTarget& t, u8 pen, u32 font, i32 x, i32 baseline, u32 text,
                     i32 right) {
    u8 chars[kMaxTitleChars];
    const i32 room = right - x + 1;
    const u32 length = read_title(t.b, text, chars);
    const u32 fitting = t.b.ok() && room > 0 ? t.graphics.text_fit(font, chars, length, room) : 0;
    if (fitting != 0) {
        t.graphics.text_on(t.b, t.bitmap, t.bitmap_addr, t.clip, pen, font, x, baseline, chars,
                           fitting);
    }
}

// A rectangle with corners cut to the given radius, in one chrome pen or,
// with steps, a top-to-bottom gradient of that many chrome pens from pen.
void fill_rounded(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip, const Rect& r,
                  i32 radius, const Look& look, u8 pen, i32 steps = 1) {
    const i32 rows = r.y1 - r.y0 + 1;
    if (rows <= 0 || r.x1 < r.x0) {
        return;
    }
    for (i32 y = r.y0; y <= r.y1; ++y) {
        const i32 ins = rounded_inset(min_i32(y - r.y0, r.y1 - y), radius);
        const i32 step = steps > 1 ? ((y - r.y0) * steps) / rows : 0;
        fill(b, bitmap, clip, r.x0 + ins, y, r.x1 - ins, y, look(static_cast<u8>(pen + step)));
    }
}

// The one-pixel frame of a box. Rounded corners cut the bottom ones, and
// the top ones too unless the box hangs from the bar, whose bottom line
// then closes the box.
void draw_box_frame(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                    const Rect& box, bool hangs, bool rounded, u8 pen) {
    const i32 r = rounded ? kMenuCornerRadius : 0;
    const i32 top = hangs ? 0 : r;
    const i32 bottom = r;
    for (i32 y = box.y0; y <= box.y1; ++y) {
        const i32 from_top = y - box.y0;
        const i32 from_bottom = box.y1 - y;
        i32 ins = 0;
        i32 prev = 0;
        if (from_bottom < bottom) {
            ins = rounded_inset(from_bottom, r);
            prev = from_bottom > 0 ? rounded_inset(from_bottom - 1, r) : ins;
        } else if (from_top < top) {
            ins = rounded_inset(from_top, r);
            prev = from_top > 0 ? rounded_inset(from_top - 1, r) : ins;
        }
        const bool edge = (from_bottom == 0) || (!hangs && from_top == 0);
        if (edge) {
            fill(b, bitmap, clip, box.x0 + ins, y, box.x1 - ins, y, pen);
            continue;
        }
        // A corner row draws from its inset to the previous row's, so the
        // diagonal stays connected.
        fill(b, bitmap, clip, box.x0 + ins, y, box.x0 + max_i32(ins, prev - 1), y, pen);
        fill(b, bitmap, clip, box.x1 - max_i32(ins, prev - 1), y, box.x1 - ins, y, pen);
    }
}

// Window-style shadow: two dark bands and two light ones outside the box.
void draw_menu_shadow(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                      const Rect& box, const u8* strong, const u8* light) {
    if (!menu_layout::valid(box) || strong == nullptr || light == nullptr) {
        return;
    }
    struct Band {
        Rect rect;
        const u8* table;
        u32 percent;
    };
    const Band bands[4] = {
        {Rect{box.x1 + 1, box.y0 + 4, box.x1 + 2, box.y1}, strong, 70},
        {Rect{box.x1 + 3, box.y0 + 4, box.x1 + 4, box.y1 + 2}, light, 85},
        {Rect{box.x0 + 4, box.y1 + 1, box.x1 + 2, box.y1 + 2}, strong, 70},
        {Rect{box.x0 + 4, box.y1 + 3, box.x1 + 4, box.y1 + 4}, light, 85},
    };
    for (const Band& band : bands) {
        const Rect r = clipped(band.rect, clip);
        if (menu_layout::valid(r)) {
            raster::shade_rect(b, bitmap, r, band.table, band.percent);
        }
    }
}

// The pens of a menu strip: the paper and the frame of the boxes, the text
// of titles and items, its pen on the highlight and the line under the bar,
// the text of a disabled entry, and how the modern look finds its other pens.
struct MenuPens {
    bool modern;
    u8 paper;
    u8 label;
    u8 shortcut;
    u8 lit;
    u8 frame;
    u8 rule;
    u8 ghost;
    Look look;
};

// The classic menu is the screen bar's pens: BARBLOCKPEN paper, BARDETAILPEN
// text and frames, BARTRIMPEN under the bar. Its highlight inverts the box,
// so the text keeps its pen on it.
MenuPens menu_pens(Style style, u32 depth, const DrawInfoPens& drawinfo) {
    const Look look{depth};
    if (style == Style::Modern) {
        return MenuPens{true,
                        look(kMenuPaperPen),
                        look(kMenuLabelPen),
                        look(kMenuLabelPen),
                        look(kMenuLitPen),
                        look(kFrameDark),
                        look(kMenuLabelPen),
                        look(kMenuGhostPen),
                        look};
    }
    const u8 block = static_cast<u8>(drawinfo.pen[BarBlockPen]);
    const u8 detail = static_cast<u8>(drawinfo.pen[BarDetailPen]);
    const u8 trim = static_cast<u8>(drawinfo.pen[BarTrimPen]);
    return MenuPens{false, block, detail, detail, detail, detail, trim, detail, look};
}

// The highlight under an item or the open title: the active title bar
// gradient. The classic highlight is made once the text is drawn.
void draw_highlight(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                    const Rect& r, const MenuPens& pens) {
    if (pens.modern) {
        fill_rounded(b, bitmap, clip, r, kMenuCornerRadius, pens.look, kActiveBar0, 8);
    }
}

// What an entry of a classic menu gets after its text: the highlighted one
// is inverted, as an item of Intuition's HIGHCOMP is, and a disabled one is
// ghosted.
void finish_classic_entry(const MenuTarget& t, const Rect& r, bool highlighted, bool ghosted,
                          const MenuPens& pens) {
    if (pens.modern) {
        return;
    }
    if (highlighted) {
        raster::complement_rect(t.b, t.bitmap, t.clip, r, (1u << t.bitmap.depth) - 1);
    } else if (ghosted) {
        ghost(t.b, t.bitmap, t.clip, r, pens.paper);
    }
}

void draw_separator(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                    const Rect& row, const MenuPens& pens) {
    const i32 x0 = row.x0 + 3;
    const i32 x1 = row.x1 - 3;
    const i32 y = (row.y0 + row.y1) / 2;
    if (pens.modern) {
        fill(b, bitmap, clip, x0, y, x1, y, pens.look(kMenuBarDark));
        fill(b, bitmap, clip, x0, y + 1, x1, y + 1, pens.look(kMenuBarLight));
    } else {
        fill(b, bitmap, clip, x0, y, x1, y, pens.label);
    }
}

// Where a glyph of the given height starts to sit in the middle of a text
// cell that starts at top.
constexpr i32 centred(i32 top, i32 cell, i32 glyph) { return top + ((cell - glyph) >> 1); }

void draw_menu_items(const MenuTarget& t, const menu_layout::Item* items, const Rect* rects,
                     u32 count, i32 selected, const MenuPens& pens, const Metrics& metrics) {
    Bus& b = t.b;
    const raster::Bitmap& bitmap = t.bitmap;
    const raster::Clip& clip = t.clip;
    bool checks = false;
    for (u32 i = 0; i < count; ++i) {
        checks = checks || items[i].checkit();
    }
    for (u32 i = 0; i < count; ++i) {
        const menu_layout::Item& item = items[i];
        const Rect& row = rects[i];
        if (!menu_layout::valid(row)) {
            continue;
        }
        if (!item.text()) {
            draw_separator(b, bitmap, clip, row, pens);
            continue;
        }
        // The highlight keeps clear of the box frame; the row itself reaches
        // the frame so the pointer can cross the paper without losing it.
        const Rect lit{row.x0 + 2, row.y0, row.x1 - 2, row.y1};
        const bool highlighted = static_cast<i32>(i) == selected && item.enabled();
        const bool ghosted = !item.enabled();
        u8 label = pens.label;
        u8 shortcut = pens.shortcut;
        if (highlighted) {
            draw_highlight(b, bitmap, clip, lit, pens);
            label = pens.lit;
            shortcut = pens.lit;
        } else if (ghosted && pens.modern) {
            label = pens.ghost;
            shortcut = pens.ghost;
        }
        // An item whose IntuiText names a font in memory is drawn in it.
        const u32 own = item.font != 0 ? t.graphics.open_font(item.font) : 0;
        const u32 font = own != 0 ? own : metrics.font;
        Graphics::FontMetrics cell{metrics.font_height, metrics.font_baseline,
                                   metrics.font_width};
        if (own != 0) {
            cell = t.graphics.font_metrics(own);
        }
        const i32 height = row.y1 - row.y0 + 1;
        const i32 text_top = row.y0 + max_i32(0, (height - cell.y_size) / 2);
        const i32 baseline = text_top + cell.baseline;
        if (item.checkit() && item.checked()) {
            draw_glyph(b, bitmap, clip, kCheckGlyph, kCheckGlyphWidth, kCheckGlyphHeight,
                       lit.x0 + kMenuTextInset, centred(text_top, cell.y_size, kCheckGlyphHeight),
                       label);
        }
        const i32 text_x = lit.x0 + kMenuTextInset + (checks ? kMenuCheckColumn : 0);
        i32 text_right = lit.x1 - kMenuTextInset;
        if (item.commseq()) {
            const i32 letter_x = lit.x1 - kMenuTextInset - cell.x_size + 1;
            const i32 key_x = letter_x - 2 - kAmigaKeyGlyphWidth;
            draw_glyph(b, bitmap, clip, kAmigaKeyGlyph, kAmigaKeyGlyphWidth, kAmigaKeyGlyphHeight,
                       key_x, centred(text_top, cell.y_size, kAmigaKeyGlyphHeight), shortcut);
            if (item.command != 0) {
                t.graphics.text_on(b, bitmap, t.bitmap_addr, clip, shortcut, font, letter_x,
                                   baseline, &item.command, 1);
            }
            text_right = key_x - 4;
        } else if (item.sub != 0) {
            const i32 arrow_x = lit.x1 - kMenuTextInset - kSubmenuGlyphWidth + 1;
            draw_glyph(b, bitmap, clip, kSubmenuGlyph, kSubmenuGlyphWidth, kSubmenuGlyphHeight,
                       arrow_x, centred(text_top, cell.y_size, kSubmenuGlyphHeight), shortcut);
            text_right = arrow_x - 4;
        }
        draw_guest_text(t, label, font, text_x, baseline, item.label, text_right);
        if (own != 0) {
            t.graphics.close_font(own);
        }
        finish_classic_entry(t, row, highlighted, ghosted, pens);
    }
}

void draw_menu_box(const MenuTarget& t, const Rect& box, bool hangs,
                   const menu_layout::Item* items, const Rect* rects, u32 count, i32 selected,
                   const MenuPens& pens, const MenuChrome& menu) {
    if (count == 0 || !menu_layout::valid(box)) {
        return;
    }
    Bus& b = t.b;
    const raster::Bitmap& bitmap = t.bitmap;
    const raster::Clip& clip = t.clip;
    if (pens.modern) {
        draw_menu_shadow(b, bitmap, clip, box, menu.strong_shade, menu.light_shade);
    }
    const i32 r = pens.modern ? kMenuCornerRadius : 0;
    for (i32 y = box.y0; y <= box.y1; ++y) {
        const i32 from_bottom = box.y1 - y;
        const i32 from_top = hangs ? r : y - box.y0;
        const i32 ins = rounded_inset(min_i32(from_top, from_bottom), r);
        fill(b, bitmap, clip, box.x0 + ins, y, box.x1 - ins, y, pens.paper);
    }
    draw_menu_items(t, items, rects, count, selected, pens, menu.metrics);
    draw_box_frame(b, bitmap, clip, box, hangs, pens.modern, pens.frame);
}

void amiga_fill(void* ctx, const Rect& box, u8 pen) {
    AmigaSurface& state = *static_cast<AmigaSurface*>(ctx);
    state.graphics.set_pens(state.rp, pen, 0, ndk::JAM2);
    state.graphics.rect_fill(state.rp, box.x0, box.y0, box.x1, box.y1);
}

u32 amiga_text_fit(void* ctx, u32 font, const u8* chars, u32 count, i32 room) {
    return static_cast<AmigaSurface*>(ctx)->graphics.text_fit(font, chars, count, room);
}

i32 amiga_text_width(void* ctx, u32 font, const u8* chars, u32 count) {
    return static_cast<i32>(
        static_cast<AmigaSurface*>(ctx)->graphics.text_width(font, chars, count));
}

void amiga_text(void* ctx, u32 font, i32 x, i32 baseline, u8 pen, const u8* chars, u32 count) {
    if (count == 0) {
        return;
    }
    AmigaSurface& state = *static_cast<AmigaSurface*>(ctx);
    state.graphics.set_pens(state.rp, pen, 0, ndk::JAM1);
    state.graphics.move(state.rp, x, baseline);
    state.graphics.plain_text_in(state.rp, font, chars, count);
}

}

Surface amiga_surface(AmigaSurface& state, u32 depth) {
    return Surface{&state, depth, amiga_fill, amiga_text_fit, amiga_text_width, amiga_text};
}

// The pen of row y of a screen title bar of bar_height rows, shaded from
// top to bottom.
u8 screen_bar_pen(i32 y, i32 bar_height) {
    return static_cast<u8>(kScreenBar0 + (y * static_cast<i32>(kScreenBarSteps)) / bar_height);
}

void draw_screen_bar(Graphics& graphics, Bus& b, u32 rp, u32 width, u32 title,
                     const DrawInfoPens& pens, const Metrics& metrics) {
    if (rp == 0 || width == 0) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const u32 depth = rastport_depth(b, rp);
    const Look look{depth};
    const bool modern = style_for_depth(depth) == Style::Modern;
    const i32 right = static_cast<i32>(width) - 1;
    const i32 line = metrics.bar_height();

    if (modern) {
        for (i32 y = 0; y < line; ++y) {
            graphics.set_pens(rp, look(screen_bar_pen(y, line)), 0, ndk::JAM2);
            graphics.rect_fill(rp, 0, y, right, y);
        }
        graphics.set_pens(rp, look(kMenuLabelPen), 0, ndk::JAM2);
        graphics.rect_fill(rp, 0, line, right, line);
    } else {
        graphics.set_pens(rp, pens.pen[BarBlockPen], 0, ndk::JAM2);
        graphics.rect_fill(rp, 0, 0, right, line - 1);
        graphics.set_pens(rp, pens.pen[BarTrimPen], 0, ndk::JAM2);
        graphics.rect_fill(rp, 0, line, right, line);
    }

    if (title != 0) {
        u8 chars[kMaxTitleChars];
        AmigaSurface state{graphics, rp};
        const Title text{chars, read_title(b, title, chars)};
        paint_title(amiga_surface(state, depth), text, 1, right,
                    modern ? look(kMenuLabelPen) : static_cast<u8>(pens.pen[BarDetailPen]),
                    metrics);
    }
    restore_rp(b, rp, saved);
}

void draw_window_bar(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w) {
    if (rp == 0 || w.width < 2 || w.height <= w.metrics.bar_height()) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const u32 depth = rastport_depth(b, rp);
    AmigaSurface state{graphics, rp};
    const Surface surface = amiga_surface(state, depth);
    if (style_for_depth(depth) == Style::Modern) {
        u8 chars[kMaxTitleChars];
        const Title title{chars, read_title(b, w.title, chars)};
        paint_modern_bar(surface, w, title);
    } else {
        draw_classic_frame(graphics, b, rp, surface, w);
    }
    restore_rp(b, rp, saved);
}

void draw_window_chrome(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w,
                        const Rect* keep) {
    if (rp == 0 || w.width <= 0 || w.height <= 0) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const u32 depth = rastport_depth(b, rp);
    AmigaSurface state{graphics, rp};
    const Surface surface = amiga_surface(state, depth);
    const bool shows_title = (w.flags & ndk::WFLG_BORDERLESS) == 0;
    if (style_for_depth(depth) == Style::Modern) {
        u8 chars[kMaxTitleChars];
        const Title title{chars, shows_title ? read_title(b, w.title, chars) : 0};
        paint_modern_window(surface, w, title, keep);
    } else {
        paint_window_clear(surface, w, keep, false);
        if (shows_title) {
            draw_classic_frame(graphics, b, rp, surface, w);
        }
        paint_window_interior(surface, w, keep);
    }

    restore_rp(b, rp, saved);
}

void draw_menu(Graphics& graphics, Bus& b, const raster::Bitmap& bitmap, const Rect& area,
               const MenuChrome& menu) {
    if (menu.width <= 0 || !menu_layout::valid(area)) {
        return;
    }
    const raster::Clip clip{area.x0, area.y0, area.x1, area.y1};
    const MenuTarget target{graphics, b, bitmap, menu.bitmap_addr, clip};
    const MenuPens pens = menu_pens(style_for_depth(menu.depth), menu.depth, menu.pens);
    const i32 right = menu.width - 1;
    const i32 line = menu.bar_height;

    // The bar is drawn in the same pens as the screen title bar it replaces.
    if (pens.modern) {
        for (i32 y = 0; y < line; ++y) {
            fill(b, bitmap, clip, 0, y, right, y, pens.look(screen_bar_pen(y, line)));
        }
    } else {
        fill(b, bitmap, clip, 0, 0, right, line - 1, pens.paper);
    }
    fill(b, bitmap, clip, 0, line, right, line, pens.rule);

    // The titles sit in the middle of a bar higher than their own font's.
    const i32 baseline =
        menu.metrics.title_baseline() + (line - menu.metrics.bar_height()) / 2;
    for (u32 i = 0; i < menu.title_count && i < menu_layout::kMaxTitles; ++i) {
        const Rect& title = menu.title_rects[i];
        if (!menu_layout::valid(title)) {
            continue;
        }
        const bool enabled = menu.titles[i].enabled();
        const bool open = static_cast<i32>(i) == menu.selected_title && enabled;
        u8 pen = pens.label;
        if (open) {
            draw_highlight(b, bitmap, clip, Rect{title.x0, 2, title.x1, line - 2}, pens);
            pen = pens.lit;
        } else if (!enabled && pens.modern) {
            pen = pens.ghost;
        }
        draw_guest_text(target, pen, menu.metrics.font, title.x0 + kMenuTitleInset, baseline,
                        menu.titles[i].name, title.x1 - 4);
        finish_classic_entry(target, Rect{title.x0, 0, title.x1, line - 1}, open, !enabled, pens);
    }

    draw_menu_box(target, menu.box, true, menu.items, menu.item_rects, menu.item_count,
                  menu.selected_item, pens, menu);
    draw_menu_box(target, menu.sub_box, false, menu.sub_items, menu.sub_item_rects,
                  menu.sub_item_count, menu.selected_sub, pens, menu);
}

void draw_prop_gadget(Graphics& graphics, Bus& b, u32 rp, const PropChrome& g) {
    if (rp == 0 || g.box.x1 < g.box.x0 || g.box.y1 < g.box.y0) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const u32 depth = rastport_depth(b, rp);
    if (style_for_depth(depth) == Style::Modern) {
        AmigaSurface state{graphics, rp};
        paint_modern_prop(amiga_surface(state, depth), g);
    } else {
        const u16* pens = g.pens.pen;
        const bool borderless = (g.pflags & ndk::PROPBORDERLESS) != 0;
        graphics.set_pens(rp, pens[BackgroundPen], 0, ndk::JAM2);
        graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x1, g.box.y1);
        if (!borderless) {
            draw_bevel(graphics, rp, g.box, pens[ShadowPen], pens[ShinePen]);
        }
        if (g.knob.x0 <= g.knob.x1 && g.knob.y0 <= g.knob.y1) {
            draw_classic_knob(graphics, rp, g.knob, (g.gflags & ndk::GFLG_SELECTED) != 0, pens);
        }
    }
    restore_rp(b, rp, saved);
}

namespace {

void put_arrow_pen(u8* out, i32 width, i32 height, i32 x, i32 y, u8 pen) {
    if (x >= 0 && x < width && y >= 0 && y < height) {
        out[y * width + x] = pen;
    }
}

// The body of the modern look's button: rounded, outlined and shaded from top
// to bottom as a vertical knob is, on the track. The pen of the triangle.
u8 paint_modern_arrow_body(u8* out, i32 width, i32 height, bool pressed, const Look& look) {
    const auto put = [&](i32 x, i32 y, u8 pen) { put_arrow_pen(out, width, height, x, y, pen); };
    for (i32 i = 0; i < width * height; ++i) {
        out[i] = look(kTrack);
    }
    const i32 radius = min_i32(2, min_i32((width - 1) / 2, (height - 1) / 2));
    const u8 shade = pressed ? kActiveBar2 : kKnob0;
    const i32 steps = pressed ? 4 : 3;
    const i32 inner_h = height - 2;
    for (i32 y = 1; y < height - 1; ++y) {
        const i32 dy = min_i32(y, height - 1 - y);
        const i32 ins = max_i32(1, rounded_inset(dy, radius));
        const u8 pen = look(static_cast<u8>(shade + ((y - 1) * steps) / inner_h));
        for (i32 x = ins; x <= width - 1 - ins; ++x) {
            put(x, y, pen);
        }
    }
    const u8 line = look(kFrameDark);
    for (i32 y = 0; y < height; ++y) {
        const i32 dy = min_i32(y, height - 1 - y);
        const i32 ins = rounded_inset(dy, radius);
        if (dy == 0) {
            for (i32 x = ins; x <= width - 1 - ins; ++x) {
                put(x, y, line);
            }
            continue;
        }
        const i32 prev = rounded_inset(dy - 1, radius);
        for (i32 x = ins; x <= max_i32(ins, prev - 1); ++x) {
            put(x, y, line);
            put(width - 1 - x, y, line);
        }
    }
    return look(pressed ? kFrameLight : kFrameDark);
}

// The body of the classic button, as sysimage draws an arrow image of an
// active window's border: the fill pen with a raised edge, light along the
// top and left and dark along the bottom and right, which a held button has
// the other way round. The window's edge runs on past the right side of a
// vertical arrow and the bottom of a horizontal one. The pen of the triangle.
u8 paint_classic_arrow_body(u8* out, i32 width, i32 height, bool vertical, bool pressed,
                            const DrawInfoPens& drawinfo) {
    const u16* pens = drawinfo.pen;
    const auto put = [&](i32 x, i32 y, u16 pen) {
        put_arrow_pen(out, width, height, x, y, static_cast<u8>(pen));
    };
    for (i32 i = 0; i < width * height; ++i) {
        out[i] = static_cast<u8>(pens[FillPen]);
    }
    if (width >= 2 && height >= 2) {
        const u16 light = pens[pressed ? ShadowPen : ShinePen];
        const u16 dark = pens[pressed ? ShinePen : ShadowPen];
        for (i32 x = 0; x < width; ++x) {
            put(x, 0, light);
        }
        for (i32 y = 0; y < height; ++y) {
            put(0, y, light);
        }
        for (i32 y = vertical ? 0 : 1; y < height; ++y) {
            put(width - 1, y, dark);
        }
        for (i32 x = vertical ? 1 : 0; x < width; ++x) {
            put(x, height - 1, dark);
        }
    }
    return static_cast<u8>(pens[ShadowPen]);
}

}

void paint_arrow_button(u8* out, i32 width, i32 height, Arrow arrow, bool pressed, Style style,
                        u32 depth, const DrawInfoPens& drawinfo) {
    if (out == nullptr || width <= 0 || height <= 0) {
        return;
    }
    const bool vertical = arrow == Arrow::Up || arrow == Arrow::Down;
    const u8 mark = style == Style::Modern
                        ? paint_modern_arrow_body(out, width, height, pressed, Look{depth})
                        : paint_classic_arrow_body(out, width, height, vertical, pressed,
                                                   drawinfo);
    const auto put = [&](i32 x, i32 y, u8 pen) { put_arrow_pen(out, width, height, x, y, pen); };
    // The triangle, centred: three rows, two to six pixels across, on a
    // small button, more on a larger one.
    const i32 rows = arrow_rows(vertical ? width : height, vertical ? height : width);
    const i32 mid_x0 = (width - 1) / 2;
    const i32 mid_x1 = width / 2;
    const i32 mid_y0 = (height - 1) / 2;
    const i32 mid_y1 = height / 2;
    const i32 top = (height - rows) / 2;
    const i32 left = (width - rows) / 2;
    for (i32 i = 0; i < rows; ++i) {
        if (vertical) {
            const i32 y = arrow == Arrow::Up ? top + i : top + rows - 1 - i;
            for (i32 x = mid_x0 - i; x <= mid_x1 + i; ++x) {
                put(x, y, mark);
            }
        } else {
            const i32 x = arrow == Arrow::Left ? left + i : left + rows - 1 - i;
            for (i32 y = mid_y0 - i; y <= mid_y1 + i; ++y) {
                put(x, y, mark);
            }
        }
    }
}

namespace {

// The classic gauge's marks: long and short ones in turn, each a dark row
// over a light one.
constexpr i32 kLongMark = 4;
constexpr i32 kShortMark = 2;
// The modern gauge's marks, a single row each.
constexpr i32 kModernLongMark = 3;
constexpr i32 kModernShortMark = 2;
// The fewest rows from one mark to the next for a scale to be drawn.
constexpr i32 kMarkPitch = 4;

// The length of the scale's mark whose first row is y, in a gauge whose marks
// are `rows` rows each and end above row `limit`; 0 where y is not on a mark.
// second tells the mark's second row from its first.
i32 mark_length(const FuelGauge& g, i32 y, i32 rows, i32 limit, i32 long_mark, i32 short_mark,
                bool& second) {
    if ((g.height - 1) / kFuelGaugeShares < kMarkPitch) {
        return 0;
    }
    for (i32 share = 1; share < kFuelGaugeShares; ++share) {
        const i32 row = fuel_gauge_mark_row(g.height, share);
        if (row + rows > limit) {
            break;
        }
        if (y >= row && y < row + rows) {
            second = y != row;
            return (share & 1) != 0 ? long_mark : short_mark;
        }
    }
    return 0;
}

// The classic gauge. Both the track's sunken edge and the box's raised one
// leave their top right and bottom left corners the face's.
u8 classic_gauge_pen(const FuelGauge& g, i32 x, i32 y) {
    const u16* pens = g.pens.pen;
    const u8 face = static_cast<u8>(pens[g.active ? FillPen : BackgroundPen]);
    const u8 shine = static_cast<u8>(pens[ShinePen]);
    const u8 shadow = static_cast<u8>(pens[ShadowPen]);
    const i32 x1 = g.width - 1 - kFuelGaugeMargin;
    const i32 y1 = g.height - 1;
    if (x > x1 || x1 < 1 || y1 < 1) {
        return face;
    }
    if (y == 0 || x == 0) {
        return (y == 0 ? x < x1 : y < y1) ? shadow : face;
    }
    if (y == y1 || x == x1) {
        return shine;
    }
    // The box of the used share, as wide as the track's inside.
    const i32 box_top = y1 - g.filled;
    const i32 box_bottom = y1 - 1;
    if (y >= box_top) {
        if (y == box_top || x == 1) {
            return (y == box_top ? x < x1 - 1 : y < box_bottom) ? shine : face;
        }
        return y == box_bottom || x == x1 - 1 ? shadow : face;
    }
    bool second = false;
    const i32 length = mark_length(g, y, 2, box_top, kLongMark, kShortMark, second);
    if (x <= length && x < x1) {
        return second ? shine : shadow;
    }
    return face;
}

// The modern gauge: the track with its outline rounded as a knob's, the
// border's face outside its corners. The used share ends in a line of the
// outline's pen across the track.
u8 modern_gauge_pen(const FuelGauge& g, i32 x, i32 y) {
    const Look look{g.depth};
    const u8 face = look(kInactiveBar5);
    const i32 x1 = g.width - 1;
    const i32 y1 = g.height - 1;
    if (x1 < 2 || y1 < 2) {
        return face;
    }
    const i32 radius = min_i32(2, min_i32(x1 / 2, y1 / 2));
    const i32 dx = min_i32(x, x1 - x);
    const i32 dy = min_i32(y, y1 - y);
    const i32 inset = rounded_inset(dy, radius);
    if (dx < inset) {
        return face;
    }
    const u8 line = look(kFrameDark);
    if (dy == 0 || dx <= max_i32(inset, rounded_inset(dy - 1, radius) - 1)) {
        return line;
    }
    const i32 fill_top = y1 - g.filled;
    if (y >= fill_top) {
        constexpr i32 kShades = 4;
        return look(static_cast<u8>(kActiveBar2 + ((y - fill_top) * kShades) / g.filled));
    }
    if (g.filled > 0 && y == fill_top - 1) {
        return line;
    }
    bool second = false;
    const i32 length =
        mark_length(g, y, 1, fill_top - 1, kModernLongMark, kModernShortMark, second);
    return x <= length ? look(kKnob2) : look(kTrack);
}

}

u8 fuel_gauge_pen(const FuelGauge& gauge, i32 x, i32 y) {
    if (x < 0 || y < 0 || x >= gauge.width || y >= gauge.height) {
        return 0;
    }
    return gauge.style == Style::Modern ? modern_gauge_pen(gauge, x, y)
                                        : classic_gauge_pen(gauge, x, y);
}

}
