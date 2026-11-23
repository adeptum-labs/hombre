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
constexpr i32 kBarButtonRadius = 2;

struct RpState {
    u8 fg = 0;
    u8 bg = 0;
    u8 mode = 0;
    u16 cx = 0;
    u16 cy = 0;
};

RpState save_rp(Bus& b, u32 rp) {
    return RpState{
        static_cast<u8>(b.r8(rp + ndk::rp_FgPen)),
        static_cast<u8>(b.r8(rp + ndk::rp_BgPen)),
        static_cast<u8>(b.r8(rp + ndk::rp_DrawMode)),
        static_cast<u16>(b.r16(rp + ndk::rp_cp_x)),
        static_cast<u16>(b.r16(rp + ndk::rp_cp_y)),
    };
}

void restore_rp(Bus& b, u32 rp, const RpState& s) {
    b.w8(rp + ndk::rp_FgPen, s.fg);
    b.w8(rp + ndk::rp_BgPen, s.bg);
    b.w8(rp + ndk::rp_DrawMode, s.mode);
    b.w16(rp + ndk::rp_cp_x, s.cx);
    b.w16(rp + ndk::rp_cp_y, s.cy);
}

u32 rastport_depth(Bus& b, u32 rp) {
    const u32 bitmap = b.r32(rp + ndk::rp_BitMap);
    if (bitmap == 0 || !b.ok()) {
        return 0;
    }
    const u32 depth = b.r8(bitmap + ndk::bm_Depth);
    return b.ok() ? depth : 0;
}

constexpr i32 min_i32(i32 a, i32 b) { return a < b ? a : b; }

constexpr i32 max_i32(i32 a, i32 b) { return a > b ? a : b; }

// The pens the modern look draws in on a screen of depth planes: a chrome pen
// of chrome_palette.hpp through its place on that screen.
struct Look {
    u32 depth;

    constexpr u8 operator()(u8 chrome_pen) const { return physical_pen(chrome_pen, depth); }
};

// The guest string at text, at most kMaxTitleChars of it, cut to the whole
// characters that fit in room pixels in font. How many there are.
u32 read_fitting(Graphics& graphics, Bus& b, u32 font, u32 text, i32 room, u8* out) {
    u32 length = 0;
    while (text != 0 && length < kMaxTitleChars) {
        const u8 c = static_cast<u8>(b.r8(text + length));
        if (!b.ok() || c == 0) {
            break;
        }
        out[length++] = c;
    }
    return b.ok() && room > 0 ? graphics.text_fit(font, out, length, room) : 0;
}

void draw_title(Graphics& graphics, Bus& b, u32 rp, u32 title, i32 x, i32 x1,
                const Metrics& metrics) {
    u8 chars[kMaxTitleChars];
    const u32 length = read_fitting(graphics, b, metrics.font, title, x1 - x + 1, chars);
    if (length != 0) {
        graphics.move(rp, x, metrics.title_baseline());
        graphics.plain_text_in(rp, metrics.font, chars, length);
    }
}

void draw_outline_box(Graphics& graphics, u32 rp, i32 x0, i32 y0, i32 x1, i32 y1, u8 pen) {
    graphics.set_pens(rp, pen, 0, ndk::JAM2);
    graphics.rect_fill(rp, x0, y0, x1, y0);
    graphics.rect_fill(rp, x0, y1, x1, y1);
    graphics.rect_fill(rp, x0, y0, x0, y1);
    graphics.rect_fill(rp, x1, y0, x1, y1);
}

void draw_frame_box(Graphics& graphics, u32 rp, const Rect& box, u8 fill_pen, u8 border_pen) {
    graphics.set_pens(rp, fill_pen, 0, ndk::JAM2);
    graphics.rect_fill(rp, box.x0, box.y0, box.x1, box.y1);
    draw_outline_box(graphics, rp, box.x0, box.y0, box.x1, box.y1, border_pen);
}

// The size gadget, as on 3.1: a corner pointing to the bottom right with a
// small light box inside it.
void draw_size_gadget(Graphics& graphics, u32 rp, const Look& look, i32 width, i32 height) {
    if (width < static_cast<i32>(kSizeBorderRight) ||
        height < static_cast<i32>(kSizeBorderBottom)) {
        return;
    }
    // The glyph sits in the middle of the corner the two borders share.
    const i32 right = width - 1 - (static_cast<i32>(kSizeBorderRight) - 7) / 2;
    const i32 bottom = height - 1 - (static_cast<i32>(kSizeBorderBottom) - 7) / 2;
    graphics.set_pens(rp, look(kFrameDark), 0, ndk::JAM2);
    graphics.rect_fill(rp, right, bottom - 6, right, bottom);
    graphics.rect_fill(rp, right - 6, bottom, right, bottom);
    draw_frame_box(graphics, rp, Rect{right - 3, bottom - 3, right, bottom}, look(kFrameLight),
                   look(kFrameDark));
}

i32 rounded_inset(i32 d, i32 radius) {
    if (radius >= 2) {
        if (d == 0) {
            return 2;
        }
        if (d == 1) {
            return 1;
        }
        return 0;
    }
    if (radius == 1 && d == 0) {
        return 1;
    }
    return 0;
}

void draw_rounded_outline(Graphics& graphics, u32 rp, const Rect& box, i32 radius, u8 pen) {
    graphics.set_pens(rp, pen, 0, ndk::JAM2);
    for (i32 y = box.y0; y <= box.y1; ++y) {
        const i32 dy = min_i32(y - box.y0, box.y1 - y);
        const i32 ins = rounded_inset(dy, radius);
        const i32 lx = box.x0 + ins;
        const i32 rx = box.x1 - ins;
        if (dy == 0) {
            if (lx <= rx) {
                graphics.rect_fill(rp, lx, y, rx, y);
            }
        } else {
            const i32 prev_ins = rounded_inset(dy - 1, radius);
            const i32 inner_lx = box.x0 + prev_ins - 1;
            const i32 inner_rx = box.x1 - prev_ins + 1;
            const i32 left_end = max_i32(lx, inner_lx);
            const i32 right_start = min_i32(rx, inner_rx);
            graphics.rect_fill(rp, lx, y, left_end, y);
            graphics.rect_fill(rp, right_start, y, rx, y);
        }
    }
}

// The body of a title-bar button, in the look of the sliders' knobs:
// rounded, outlined and shaded from top to bottom. Held down, it takes the
// shading of a held knob.
void draw_bar_button_body(Graphics& graphics, u32 rp, const Look& look, const Rect& box,
                          bool pressed) {
    const u8 shade = pressed ? kActiveBar2 : kKnob0;
    const i32 steps = pressed ? 4 : 3;
    const i32 inner_h = box.y1 - box.y0 - 1;
    for (i32 y = box.y0 + 1; y < box.y1; ++y) {
        const i32 dy = min_i32(y - box.y0, box.y1 - y);
        const i32 ins = max_i32(1, rounded_inset(dy, kBarButtonRadius));
        const u8 pen = look(static_cast<u8>(shade + ((y - box.y0 - 1) * steps) / inner_h));
        graphics.set_pens(rp, pen, 0, ndk::JAM2);
        graphics.rect_fill(rp, box.x0 + ins, y, box.x1 - ins, y);
    }
    draw_rounded_outline(graphics, rp, box, kBarButtonRadius, look(kFrameDark));
}

// The close button: as on 3.1, a small light square outlined in the dark
// pen in the middle of the body. Held down, the two swap.
void draw_close_button(Graphics& graphics, u32 rp, const Look& look, const Metrics& metrics,
                       bool active, bool pressed) {
    const Rect box = close_button_box(metrics);
    draw_bar_button_body(graphics, rp, look, box, pressed);
    u8 line = look(active ? kFrameDark : kTitleInactive);
    u8 face = look(kFrameLight);
    if (pressed) {
        const u8 swap = line;
        line = face;
        face = swap;
    }
    const i32 cx = (box.x0 + box.x1) / 2;
    const i32 cy = (box.y0 + box.y1) / 2;
    draw_frame_box(graphics, rp, Rect{cx - 3, cy - 2, cx + 3, cy + 2}, face, line);
}

// The depth button: two overlapping sheets, the light one in front of the
// shaded one.
void draw_depth_button(Graphics& graphics, u32 rp, const Look& look, i32 width,
                       const Metrics& metrics, bool active, bool pressed) {
    const Rect box = depth_button_box(width, metrics);
    draw_bar_button_body(graphics, rp, look, box, pressed);
    const u8 line = look(pressed ? kFrameLight : (active ? kFrameDark : kTitleInactive));
    const i32 cx = (box.x0 + box.x1) / 2;
    const i32 cy = (box.y0 + box.y1) / 2;
    const Rect behind{cx - 3, cy - 3, cx + 1, cy + 1};
    if (pressed) {
        // The sheet behind lets the held body's shading show through.
        draw_outline_box(graphics, rp, behind.x0, behind.y0, behind.x1, behind.y1, line);
    } else {
        draw_frame_box(graphics, rp, behind, look(kKnob2), line);
    }
    draw_frame_box(graphics, rp, Rect{cx - 1, cy - 1, cx + 3, cy + 3}, look(kFrameLight), line);
}

// The zoom button: a large sheet with a small light one in its top left
// corner.
void draw_zoom_button(Graphics& graphics, u32 rp, const Look& look, const Rect& box, bool active,
                      bool pressed) {
    draw_bar_button_body(graphics, rp, look, box, pressed);
    const u8 line = look(pressed ? kFrameLight : (active ? kFrameDark : kTitleInactive));
    const i32 cx = (box.x0 + box.x1) / 2;
    const i32 cy = (box.y0 + box.y1) / 2;
    const Rect sheet{cx - 3, cy - 3, cx + 3, cy + 3};
    if (pressed) {
        // The sheet lets the held body's shading show through.
        draw_outline_box(graphics, rp, sheet.x0, sheet.y0, sheet.x1, sheet.y1, line);
    } else {
        draw_frame_box(graphics, rp, sheet, look(kKnob2), line);
    }
    draw_frame_box(graphics, rp, Rect{sheet.x0, sheet.y0, cx, cy}, look(kFrameLight), line);
}

void draw_knob(Graphics& graphics, u32 rp, const Look& look, const Rect& knob, u16 pflags,
               u16 gflags) {
    const i32 kw = knob.x1 - knob.x0 + 1;
    const i32 kh = knob.y1 - knob.y0 + 1;
    if (kw <= 0 || kh <= 0) {
        return;
    }
    const i32 max_rx = (kw - 1) / 2;
    const i32 max_ry = (kh - 1) / 2;
    const i32 radius = min_i32(2, min_i32(max_rx, max_ry));

    const bool horiz_only = (pflags & ndk::FREEHORIZ) != 0 && (pflags & ndk::FREEVERT) == 0;
    const bool selected = (gflags & ndk::GFLG_SELECTED) != 0;

    const u8 grad_base = selected ? kActiveBar2 : kKnob0;
    const i32 grad_steps = selected ? 4 : 3;

    if (horiz_only) {
        const i32 inner_w = kw - 2;
        if (inner_w > 0) {
            for (i32 x = knob.x0 + 1; x <= knob.x1 - 1; ++x) {
                const i32 dx = min_i32(x - knob.x0, knob.x1 - x);
                const i32 ins = max_i32(1, rounded_inset(dx, radius));
                const i32 y0 = knob.y0 + ins;
                const i32 y1 = knob.y1 - ins;
                if (y0 <= y1) {
                    const i32 step = ((x - (knob.x0 + 1)) * grad_steps) / inner_w;
                    graphics.set_pens(rp, look(static_cast<u8>(grad_base + step)), 0, ndk::JAM2);
                    graphics.rect_fill(rp, x, y0, x, y1);
                }
            }
        }
    } else {
        const i32 inner_h = kh - 2;
        if (inner_h > 0) {
            for (i32 y = knob.y0 + 1; y <= knob.y1 - 1; ++y) {
                const i32 dy = min_i32(y - knob.y0, knob.y1 - y);
                const i32 ins = max_i32(1, rounded_inset(dy, radius));
                const i32 x0 = knob.x0 + ins;
                const i32 x1 = knob.x1 - ins;
                if (x0 <= x1) {
                    const i32 step = ((y - (knob.y0 + 1)) * grad_steps) / inner_h;
                    graphics.set_pens(rp, look(static_cast<u8>(grad_base + step)), 0, ndk::JAM2);
                    graphics.rect_fill(rp, x0, y, x1, y);
                }
            }
        }
    }

    draw_rounded_outline(graphics, rp, knob, radius, look(kFrameDark));

    const i32 cx = (knob.x0 + knob.x1) / 2;
    const i32 cy = (knob.y0 + knob.y1) / 2;
    if (!horiz_only && kh >= 12) {
        i32 gx0 = knob.x0 + 2 <= knob.x1 - 2 ? knob.x0 + 2 : knob.x0 + 1;
        i32 gx1 = knob.x0 + 2 <= knob.x1 - 2 ? knob.x1 - 2 : knob.x1 - 1;
        if (gx1 - gx0 + 1 > 5) {
            gx0 = cx - 2;
            gx1 = cx + 2;
        }
        if (gx0 <= gx1) {
            graphics.set_pens(rp, look(kFrameDark), 0, ndk::JAM2);
            graphics.rect_fill(rp, gx0, cy - 2, gx1, cy - 2);
            graphics.rect_fill(rp, gx0, cy, gx1, cy);
            graphics.rect_fill(rp, gx0, cy + 2, gx1, cy + 2);
        }
    } else if (horiz_only && kw >= 12) {
        i32 gy0 = knob.y0 + 2 <= knob.y1 - 2 ? knob.y0 + 2 : knob.y0 + 1;
        i32 gy1 = knob.y0 + 2 <= knob.y1 - 2 ? knob.y1 - 2 : knob.y1 - 1;
        if (gy1 - gy0 + 1 > 5) {
            gy0 = cy - 2;
            gy1 = cy + 2;
        }
        if (gy0 <= gy1) {
            graphics.set_pens(rp, look(kFrameDark), 0, ndk::JAM2);
            graphics.rect_fill(rp, cx - 2, gy0, cx - 2, gy1);
            graphics.rect_fill(rp, cx, gy0, cx, gy1);
            graphics.rect_fill(rp, cx + 2, gy0, cx + 2, gy1);
        }
    }
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

// The title between the bar buttons, or the frame where a button is missing.
void draw_bar_title(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w, u32 pen) {
    const Metrics& metrics = w.metrics;
    graphics.set_pens(rp, pen, 0, ndk::JAM1);
    draw_title(graphics, b, rp, w.title, title_left(w.flags, metrics),
               title_right(w.width, w.flags, metrics), metrics);
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
// line is the one under the bar. The gadgets are the system images.
void draw_classic_frame(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w) {
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

    if ((w.flags & ndk::WFLG_CLOSEGADGET) != 0) {
        sysimage::draw(graphics, b, rp, ndk::CLOSEIMAGE, close_button_box(w.metrics),
                       gadget_state(w.active, w.pressed == BarButton::Close), pens);
    }
    // The zoom and depth gadgets share a column, so the one held is drawn
    // last for its sunken edge to show.
    const bool zoom_held = w.pressed == BarButton::Zoom;
    const auto draw_zoom = [&] {
        if ((w.flags & ndk::WFLG_HASZOOM) != 0) {
            sysimage::draw(graphics, b, rp, ndk::ZOOMIMAGE,
                           zoom_button_box(w.width, w.flags, w.metrics),
                           gadget_state(w.active, zoom_held), pens);
        }
    };
    if (!zoom_held) {
        draw_zoom();
    }
    if ((w.flags & ndk::WFLG_DEPTHGADGET) != 0) {
        sysimage::draw(graphics, b, rp, ndk::DEPTHIMAGE, depth_button_box(w.width, w.metrics),
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
    draw_bar_title(graphics, b, rp, w, w.active ? pens[FillTextPen] : pens[TextPen]);
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
    const u32 length = read_fitting(t.graphics, t.b, font, text, right - x + 1, chars);
    if (length != 0) {
        t.graphics.text_on(t.b, t.bitmap, t.bitmap_addr, t.clip, pen, font, x, baseline, chars,
                           length);
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
    };
    const Band bands[4] = {
        {Rect{box.x1 + 1, box.y0 + 4, box.x1 + 2, box.y1}, strong},
        {Rect{box.x1 + 3, box.y0 + 4, box.x1 + 4, box.y1 + 2}, light},
        {Rect{box.x0 + 4, box.y1 + 1, box.x1 + 2, box.y1 + 2}, strong},
        {Rect{box.x0 + 4, box.y1 + 3, box.x1 + 4, box.y1 + 4}, light},
    };
    for (const Band& band : bands) {
        const Rect r = clipped(band.rect, clip);
        if (menu_layout::valid(r)) {
            raster::remap_rect(b, bitmap, r, band.table, 256);
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

void draw_modern_bar(Graphics& graphics, Bus& b, u32 rp, const Look& look,
                     const WindowChrome& w) {
    const i32 left = 1;
    const i32 right = w.width - 2;
    const Metrics& metrics = w.metrics;
    const i32 line = metrics.bar_height();

    const bool rounded =
        (w.flags & (ndk::WFLG_BORDERLESS | ndk::WFLG_BACKDROP)) == 0 && w.width >= 7;
    if (rounded) {
        graphics.set_pens(rp, look(kFrameDark), 0, ndk::JAM2);
        graphics.rect_fill(rp, 2, 1, 2, 1);
        graphics.rect_fill(rp, w.width - 3, 1, w.width - 3, 1);
        graphics.rect_fill(rp, 1, 2, 1, 2);
        graphics.rect_fill(rp, w.width - 2, 2, w.width - 2, 2);

        graphics.set_pens(rp, look(kFrameLight), 0, ndk::JAM2);
        graphics.rect_fill(rp, 3, 1, w.width - 4, 1);
    } else {
        graphics.set_pens(rp, look(kFrameLight), 0, ndk::JAM2);
        graphics.rect_fill(rp, left, 1, right, 1);
    }

    const u8 base = w.active ? kActiveBar0 : kInactiveBar0;
    const i32 steps = w.active ? 8 : 6;
    const i32 rows = line - 2;
    for (i32 y = 2; y < line; ++y) {
        const i32 step = ((y - 2) * steps) / rows;
        const i32 row_left = (rounded && y == 2) ? 2 : left;
        const i32 row_right = (rounded && y == 2) ? w.width - 3 : right;
        graphics.set_pens(rp, look(static_cast<u8>(base + step)), 0, ndk::JAM2);
        graphics.rect_fill(rp, row_left, y, row_right, y);
    }
    graphics.set_pens(rp, look(kFrameDark), 0, ndk::JAM2);
    graphics.rect_fill(rp, left, line, right, line);

    if ((w.flags & ndk::WFLG_CLOSEGADGET) != 0) {
        draw_close_button(graphics, rp, look, metrics, w.active, w.pressed == BarButton::Close);
    }
    if ((w.flags & ndk::WFLG_HASZOOM) != 0) {
        draw_zoom_button(graphics, rp, look, zoom_button_box(w.width, w.flags, metrics), w.active,
                         w.pressed == BarButton::Zoom);
    }
    if ((w.flags & ndk::WFLG_DEPTHGADGET) != 0) {
        draw_depth_button(graphics, rp, look, w.width, metrics, w.active,
                          w.pressed == BarButton::Depth);
    }
    draw_bar_title(graphics, b, rp, w, look(w.active ? kTitleActive : kTitleInactive));
}

// The frame of a window as the modern look has it: the outline, the bar
// and the borders, and the size gadget.
void draw_modern_frame(Graphics& graphics, Bus& b, u32 rp, const Look& look,
                       const WindowChrome& w, bool rounded) {
    const i32 right = w.width - 1;
    const i32 bottom = w.height - 1;

    if (rounded) {
        graphics.set_pens(rp, look(kFrameDark), 0, ndk::JAM2);
        graphics.rect_fill(rp, 3, 0, right - 3, 0);
        graphics.rect_fill(rp, 0, bottom, right, bottom);
        graphics.rect_fill(rp, 0, 3, 0, bottom);
        graphics.rect_fill(rp, right, 3, right, bottom);
        graphics.rect_fill(rp, 2, 1, 2, 1);
        graphics.rect_fill(rp, right - 2, 1, right - 2, 1);
        graphics.rect_fill(rp, 1, 2, 1, 2);
        graphics.rect_fill(rp, right - 1, 2, right - 1, 2);
    } else {
        draw_outline_box(graphics, rp, 0, 0, right, bottom, look(kFrameDark));
    }

    if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
        draw_modern_bar(graphics, b, rp, look, w);

        const i32 body_top = static_cast<i32>(w.border_top);
        const i32 body_bottom = bottom - 1;
        if (body_top <= body_bottom && w.width >= 3) {
            graphics.set_pens(rp, look(kFrameLight), 0, ndk::JAM2);
            graphics.rect_fill(rp, 1, body_top, 1, body_bottom);

            graphics.set_pens(rp, look(kInactiveBar5), 0, ndk::JAM2);
            if (w.border_left > 2) {
                const i32 lx1 = min_i32(static_cast<i32>(w.border_left) - 1, right - 1);
                if (2 <= lx1) {
                    graphics.rect_fill(rp, 2, body_top, lx1, body_bottom);
                }
            }
            if (w.border_right >= 2) {
                const i32 rx0 = max_i32(w.width - static_cast<i32>(w.border_right), 2);
                if (rx0 <= right - 1) {
                    graphics.rect_fill(rp, rx0, body_top, right - 1, body_bottom);
                }
            }
            if (w.border_bottom >= 2) {
                const i32 by0 = max_i32(w.height - static_cast<i32>(w.border_bottom), body_top);
                if (by0 <= body_bottom && 2 <= right - 1) {
                    graphics.rect_fill(rp, 2, by0, right - 1, body_bottom);
                }
            }
        }

        if ((w.flags & ndk::WFLG_SIZEGADGET) != 0) {
            draw_size_gadget(graphics, rp, look, w.width, w.height);
        }
    }
}

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
    const Look look{rastport_depth(b, rp)};
    const bool modern = style_for_depth(look.depth) == Style::Modern;
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
        graphics.set_pens(rp, modern ? look(kMenuLabelPen) : pens.pen[BarDetailPen], 0,
                          ndk::JAM1);
        draw_title(graphics, b, rp, title, 1, right, metrics);
    }
    restore_rp(b, rp, saved);
}

void draw_window_bar(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w) {
    if (rp == 0 || w.width < 2 || w.height <= w.metrics.bar_height()) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const Look look{rastport_depth(b, rp)};
    if (style_for_depth(look.depth) == Style::Modern) {
        draw_modern_bar(graphics, b, rp, look, w);
    } else {
        draw_classic_frame(graphics, b, rp, w);
    }
    restore_rp(b, rp, saved);
}

void draw_window_chrome(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w,
                        const Rect* keep) {
    if (rp == 0 || w.width <= 0 || w.height <= 0) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const Look look{rastport_depth(b, rp)};
    const bool modern = style_for_depth(look.depth) == Style::Modern;
    const i32 right = w.width - 1;
    const i32 bottom = w.height - 1;

    const bool rounded = modern &&
                         (w.flags & (ndk::WFLG_BORDERLESS | ndk::WFLG_BACKDROP)) == 0 &&
                         w.width >= 7 && w.height >= 4;
    graphics.set_pens(rp, 0, 0, ndk::JAM2);
    if (keep != nullptr && keep->x0 <= keep->x1 && keep->y0 <= keep->y1) {
        // Only clear areas outside the preserved interior; borders are redrawn below.
        if (keep->x1 < right) {
            graphics.rect_fill(rp, keep->x1 + 1, 0, right, bottom);
        }
        if (keep->y1 < bottom) {
            graphics.rect_fill(rp, 0, keep->y1 + 1, right, bottom);
        }
        if (rounded) {
            graphics.rect_fill(rp, 0, 0, 2, 0);
            graphics.rect_fill(rp, 0, 1, 1, 1);
            graphics.rect_fill(rp, 0, 2, 0, 2);
            graphics.rect_fill(rp, right - 2, 0, right, 0);
            graphics.rect_fill(rp, right - 1, 1, right, 1);
            graphics.rect_fill(rp, right, 2, right, 2);
        }
    } else {
        graphics.rect_fill(rp, 0, 0, right, bottom);
    }

    if (modern) {
        draw_modern_frame(graphics, b, rp, look, w, rounded);
    } else if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
        draw_classic_frame(graphics, b, rp, w);
    }

    const i32 ix0 = static_cast<i32>(w.border_left);
    const i32 iy0 = static_cast<i32>(w.border_top);
    const i32 ix1 = w.width - static_cast<i32>(w.border_right) - 1;
    const i32 iy1 = w.height - static_cast<i32>(w.border_bottom) - 1;
    if (keep == nullptr && ix0 <= ix1 && iy0 <= iy1) {
        graphics.set_pens(rp, 0, 0, ndk::JAM2);
        graphics.rect_fill(rp, ix0, iy0, ix1, iy1);
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
    const Look look{rastport_depth(b, rp)};
    const bool modern = style_for_depth(look.depth) == Style::Modern;
    const u16* pens = g.pens.pen;
    const bool borderless = (g.pflags & ndk::PROPBORDERLESS) != 0;

    if (modern) {
        graphics.set_pens(rp, look(kTrack), 0, ndk::JAM2);
        graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x1, g.box.y1);
        if (!borderless) {
            graphics.set_pens(rp, look(kFrameDark), 0, ndk::JAM2);
            graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x1, g.box.y0);
            graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x0, g.box.y1);
        }
    } else {
        graphics.set_pens(rp, pens[BackgroundPen], 0, ndk::JAM2);
        graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x1, g.box.y1);
        if (!borderless) {
            draw_bevel(graphics, rp, g.box, pens[ShadowPen], pens[ShinePen]);
        }
    }

    if (g.knob.x0 <= g.knob.x1 && g.knob.y0 <= g.knob.y1) {
        if (modern) {
            draw_knob(graphics, rp, look, g.knob, g.pflags, g.gflags);
        } else {
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

}
