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

#include "chrome_palette.hpp"
#include "graphics.hpp"
#include "intuition.hpp"

namespace hombre::chrome {

using native::Bus;

namespace {

constexpr u32 kTitleChunk = 64;
constexpr u32 kMaxTitleChars = 255;
constexpr i32 kTitleBaseline = 9;
constexpr i32 kBarButtonRadius = 2;
// The gaps between a bar button and the first and last pixel of the title.
constexpr i32 kTitleGapLeft = 4;
constexpr i32 kTitleGapRight = 3;

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

void draw_title(Graphics& graphics, Bus& b, u32 rp, u32 title, i32 x, i32 baseline, i32 x1) {
    if (title == 0 || x1 < x) {
        return;
    }
    u32 chars = static_cast<u32>((x1 - x + 1) / 8);
    if (chars > kMaxTitleChars) {
        chars = kMaxTitleChars;
    }
    graphics.move(rp, x, baseline);
    u8 buffer[kTitleChunk];
    for (u32 at = 0; at < chars; at += kTitleChunk) {
        u32 n = chars - at;
        if (n > kTitleChunk) {
            n = kTitleChunk;
        }
        for (u32 i = 0; i < n; ++i) {
            buffer[i] = static_cast<u8>(b.r8(title + at + i));
        }
        if (!b.ok()) {
            return;
        }
        u32 length = 0;
        while (length < n && buffer[length] != 0) {
            ++length;
        }
        if (length != 0) {
            graphics.text(rp, buffer, length);
        }
        if (length < n) {
            return;
        }
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
void draw_size_gadget(Graphics& graphics, u32 rp, i32 width, i32 height, bool high_color) {
    if (width < static_cast<i32>(kSizeBorderRight) ||
        height < static_cast<i32>(kSizeBorderBottom)) {
        return;
    }
    const u8 dark = high_color ? kFrameDark : 1;
    const u8 light = high_color ? kFrameLight : 2;
    const i32 right = width - 4;
    const i32 bottom = height - 4;
    graphics.set_pens(rp, dark, 0, ndk::JAM2);
    graphics.rect_fill(rp, right, bottom - 6, right, bottom);
    graphics.rect_fill(rp, right - 6, bottom, right, bottom);
    draw_frame_box(graphics, rp, Rect{right - 3, bottom - 3, right, bottom}, light, dark);
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
// shading of a held knob. On screens of fewer than eight planes the body is
// flat: the background pen, or the fill pen while held.
void draw_bar_button_body(Graphics& graphics, u32 rp, const Rect& box, bool pressed,
                          bool high_color) {
    const u8 shade = pressed ? kActiveBar2 : kKnob0;
    const i32 steps = pressed ? 4 : 3;
    const i32 inner_h = box.y1 - box.y0 - 1;
    for (i32 y = box.y0 + 1; y < box.y1; ++y) {
        const i32 dy = min_i32(y - box.y0, box.y1 - y);
        const i32 ins = max_i32(1, rounded_inset(dy, kBarButtonRadius));
        const u8 pen = high_color
                           ? static_cast<u8>(shade + ((y - box.y0 - 1) * steps) / inner_h)
                           : static_cast<u8>(pressed ? 3 : 0);
        graphics.set_pens(rp, pen, 0, ndk::JAM2);
        graphics.rect_fill(rp, box.x0 + ins, y, box.x1 - ins, y);
    }
    draw_rounded_outline(graphics, rp, box, kBarButtonRadius, high_color ? kFrameDark : 1);
}

// The close button: as on 3.1, a small light square outlined in the dark
// pen in the middle of the body. Held down, the two swap.
void draw_close_button(Graphics& graphics, u32 rp, bool active, bool pressed, bool high_color) {
    const Rect box = close_button_box();
    draw_bar_button_body(graphics, rp, box, pressed, high_color);
    u8 line = 1;
    u8 face = 2;
    if (high_color) {
        line = active ? kFrameDark : kTitleInactive;
        face = kFrameLight;
    }
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
void draw_depth_button(Graphics& graphics, u32 rp, i32 width, bool active, bool pressed,
                       bool high_color) {
    const Rect box = depth_button_box(width);
    draw_bar_button_body(graphics, rp, box, pressed, high_color);
    u8 line = pressed ? 2 : 1;
    u8 back = 0;
    u8 front = 2;
    if (high_color) {
        line = pressed ? kFrameLight : (active ? kFrameDark : kTitleInactive);
        back = kKnob2;
        front = kFrameLight;
    }
    const i32 cx = (box.x0 + box.x1) / 2;
    const i32 cy = (box.y0 + box.y1) / 2;
    const Rect behind{cx - 3, cy - 3, cx + 1, cy + 1};
    if (pressed) {
        // The sheet behind lets the held body's shading show through.
        draw_outline_box(graphics, rp, behind.x0, behind.y0, behind.x1, behind.y1, line);
    } else {
        draw_frame_box(graphics, rp, behind, back, line);
    }
    draw_frame_box(graphics, rp, Rect{cx - 1, cy - 1, cx + 3, cy + 3}, front, line);
}

void draw_knob(Graphics& graphics, u32 rp, const Rect& knob, u16 pflags, u16 gflags,
               bool high_color) {
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

    if (!high_color) {
        graphics.set_pens(rp, 2, 0, ndk::JAM2);
        for (i32 y = knob.y0 + 1; y <= knob.y1 - 1; ++y) {
            const i32 dy = min_i32(y - knob.y0, knob.y1 - y);
            const i32 ins = max_i32(1, rounded_inset(dy, radius));
            if (knob.x0 + ins <= knob.x1 - ins) {
                graphics.rect_fill(rp, knob.x0 + ins, y, knob.x1 - ins, y);
            }
        }
        draw_rounded_outline(graphics, rp, knob, radius, 1);
        return;
    }

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
                    graphics.set_pens(rp, grad_base + static_cast<u8>(step), 0, ndk::JAM2);
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
                    graphics.set_pens(rp, grad_base + static_cast<u8>(step), 0, ndk::JAM2);
                    graphics.rect_fill(rp, x0, y, x1, y);
                }
            }
        }
    }

    draw_rounded_outline(graphics, rp, knob, radius, kFrameDark);

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
            graphics.set_pens(rp, kFrameDark, 0, ndk::JAM2);
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
            graphics.set_pens(rp, kFrameDark, 0, ndk::JAM2);
            graphics.rect_fill(rp, cx - 2, gy0, cx - 2, gy1);
            graphics.rect_fill(rp, cx, gy0, cx, gy1);
            graphics.rect_fill(rp, cx + 2, gy0, cx + 2, gy1);
        }
    }
}

// The menu separator on screens of eight planes: a dark line over a light
// one, as the window frames use.
constexpr u8 kMenuBarDark = kKnob2;
constexpr u8 kMenuBarLight = kTitleActive;

// The same roles on screens of fewer planes, in the classic pens: white
// paper, black labels and the blue fill under light text for the open
// title and the highlighted item.
constexpr u8 kFlatPaper = 2;
constexpr u8 kFlatLabel = 1;
constexpr u8 kFlatLit = 2;
constexpr u8 kFlatFill = 3;

constexpr i32 kMenuTextInset = 4;
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

// The guest string at text, cut to the whole characters that fit before
// right, with its baseline at baseline.
void draw_guest_text(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip, u8 pen,
                     i32 x, i32 baseline, u32 text, i32 right) {
    if (text == 0 || right < x) {
        return;
    }
    u32 chars = static_cast<u32>((right - x + 1) / 8);
    if (chars > kMaxTitleChars) {
        chars = kMaxTitleChars;
    }
    const raster::Pens pens{pen, 0, static_cast<u8>(ndk::JAM1)};
    u8 buffer[kTitleChunk];
    for (u32 at = 0; at < chars; at += kTitleChunk) {
        u32 n = chars - at;
        if (n > kTitleChunk) {
            n = kTitleChunk;
        }
        for (u32 i = 0; i < n; ++i) {
            buffer[i] = static_cast<u8>(b.r8(text + at + i));
        }
        if (!b.ok()) {
            return;
        }
        u32 length = 0;
        while (length < n && buffer[length] != 0) {
            ++length;
        }
        if (length != 0) {
            u32 advance = 0;
            raster::draw_text(b, bitmap, clip, pens, x + static_cast<i32>(at) * 8, baseline,
                              buffer, length, advance);
        }
        if (length < n) {
            return;
        }
    }
}

// A rectangle with corners cut to the given radius, in one pen or, with
// steps, a top-to-bottom gradient of that many pens from pen.
void fill_rounded(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip, const Rect& r,
                  i32 radius, u8 pen, i32 steps = 1) {
    const i32 rows = r.y1 - r.y0 + 1;
    if (rows <= 0 || r.x1 < r.x0) {
        return;
    }
    for (i32 y = r.y0; y <= r.y1; ++y) {
        const i32 ins = rounded_inset(min_i32(y - r.y0, r.y1 - y), radius);
        const i32 step = steps > 1 ? ((y - r.y0) * steps) / rows : 0;
        fill(b, bitmap, clip, r.x0 + ins, y, r.x1 - ins, y, static_cast<u8>(pen + step));
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

struct MenuPens {
    bool high_color;
    u8 paper;
    u8 label;
    u8 shortcut;
    u8 lit;
    u8 frame;
};

MenuPens menu_pens(bool high_color) {
    if (high_color) {
        return MenuPens{true, kMenuPaperPen, kMenuLabelPen, kMenuLabelPen, kMenuLitPen,
                        kFrameDark};
    }
    return MenuPens{false, kFlatPaper, kFlatLabel, kFlatLabel, kFlatLit, kFlatLabel};
}

// The highlight under an item or the open title: the active title bar
// gradient, or the blue pen on screens of fewer planes.
void draw_highlight(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                    const Rect& r, const MenuPens& pens) {
    if (pens.high_color) {
        fill_rounded(b, bitmap, clip, r, kMenuCornerRadius, kActiveBar0, 8);
    } else {
        fill_rounded(b, bitmap, clip, r, 1, kFlatFill);
    }
}

void draw_separator(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                    const Rect& row, const MenuPens& pens) {
    const i32 x0 = row.x0 + 3;
    const i32 x1 = row.x1 - 3;
    const i32 y = (row.y0 + row.y1) / 2;
    if (pens.high_color) {
        fill(b, bitmap, clip, x0, y, x1, y, kMenuBarDark);
        fill(b, bitmap, clip, x0, y + 1, x1, y + 1, kMenuBarLight);
    } else {
        fill(b, bitmap, clip, x0, y, x1, y, pens.label);
    }
}

void draw_menu_items(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                     const menu_layout::Item* items, const Rect* rects, u32 count, i32 selected,
                     const MenuPens& pens) {
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
        } else if (ghosted && pens.high_color) {
            label = kMenuGhostPen;
            shortcut = kMenuGhostPen;
        }
        const i32 height = row.y1 - row.y0 + 1;
        const i32 baseline = row.y0 + max_i32(0, (height - 8) / 2) + 6;
        if (item.checkit() && item.checked()) {
            draw_glyph(b, bitmap, clip, kCheckGlyph, kCheckGlyphWidth, kCheckGlyphHeight,
                       lit.x0 + kMenuTextInset, baseline - 6, label);
        }
        const i32 text_x = lit.x0 + kMenuTextInset + (checks ? kMenuCheckColumn : 0);
        i32 text_right = lit.x1 - kMenuTextInset;
        if (item.commseq()) {
            const i32 letter_x = lit.x1 - kMenuTextInset - 8 + 1;
            const i32 key_x = letter_x - 2 - kAmigaKeyGlyphWidth;
            draw_glyph(b, bitmap, clip, kAmigaKeyGlyph, kAmigaKeyGlyphWidth, kAmigaKeyGlyphHeight,
                       key_x, baseline - 7, shortcut);
            if (item.command != 0) {
                const raster::Pens text_pens{shortcut, 0, static_cast<u8>(ndk::JAM1)};
                u32 advance = 0;
                raster::draw_text(b, bitmap, clip, text_pens, letter_x, baseline, &item.command,
                                  1, advance);
            }
            text_right = key_x - 4;
        } else if (item.sub != 0) {
            const i32 arrow_x = lit.x1 - kMenuTextInset - kSubmenuGlyphWidth + 1;
            draw_glyph(b, bitmap, clip, kSubmenuGlyph, kSubmenuGlyphWidth, kSubmenuGlyphHeight,
                       arrow_x, baseline - 6, shortcut);
            text_right = arrow_x - 4;
        }
        draw_guest_text(b, bitmap, clip, label, text_x, baseline, item.label, text_right);
        if (ghosted && !pens.high_color) {
            ghost(b, bitmap, clip, lit, pens.paper);
        }
    }
}

void draw_menu_box(Bus& b, const raster::Bitmap& bitmap, const raster::Clip& clip,
                   const Rect& box, bool hangs, const menu_layout::Item* items, const Rect* rects,
                   u32 count, i32 selected, const MenuPens& pens, const u8* strong,
                   const u8* light) {
    if (count == 0 || !menu_layout::valid(box)) {
        return;
    }
    draw_menu_shadow(b, bitmap, clip, box, strong, light);
    const i32 r = pens.high_color ? kMenuCornerRadius : 0;
    for (i32 y = box.y0; y <= box.y1; ++y) {
        const i32 from_bottom = box.y1 - y;
        const i32 from_top = hangs ? r : y - box.y0;
        const i32 ins = rounded_inset(min_i32(from_top, from_bottom), r);
        fill(b, bitmap, clip, box.x0 + ins, y, box.x1 - ins, y, pens.paper);
    }
    draw_menu_items(b, bitmap, clip, items, rects, count, selected, pens);
    draw_box_frame(b, bitmap, clip, box, hangs, pens.high_color, pens.frame);
}

void draw_bar_contents(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w, bool high_color) {
    const i32 left = 1;
    const i32 right = w.width - 2;
    const i32 line = static_cast<i32>(kBarHeight);

    if (high_color) {
        const bool rounded =
            (w.flags & (ndk::WFLG_BORDERLESS | ndk::WFLG_BACKDROP)) == 0 && w.width >= 7;
        if (rounded) {
            graphics.set_pens(rp, kFrameDark, 0, ndk::JAM2);
            graphics.rect_fill(rp, 2, 1, 2, 1);
            graphics.rect_fill(rp, w.width - 3, 1, w.width - 3, 1);
            graphics.rect_fill(rp, 1, 2, 1, 2);
            graphics.rect_fill(rp, w.width - 2, 2, w.width - 2, 2);

            graphics.set_pens(rp, kFrameLight, 0, ndk::JAM2);
            graphics.rect_fill(rp, 3, 1, w.width - 4, 1);
        } else {
            graphics.set_pens(rp, kFrameLight, 0, ndk::JAM2);
            graphics.rect_fill(rp, left, 1, right, 1);
        }

        const u8 base = w.active ? kActiveBar0 : kInactiveBar0;
        const i32 steps = w.active ? 8 : 6;
        const i32 rows = line - 2;
        for (i32 y = 2; y < line; ++y) {
            const i32 step = ((y - 2) * steps) / rows;
            const i32 row_left = (rounded && y == 2) ? 2 : left;
            const i32 row_right = (rounded && y == 2) ? w.width - 3 : right;
            graphics.set_pens(rp, base + static_cast<u8>(step), 0, ndk::JAM2);
            graphics.rect_fill(rp, row_left, y, row_right, y);
        }
        graphics.set_pens(rp, kFrameDark, 0, ndk::JAM2);
        graphics.rect_fill(rp, left, line, right, line);
    } else {
        graphics.set_pens(rp, w.active ? 3 : 0, 0, ndk::JAM2);
        graphics.rect_fill(rp, left, 1, right, line - 1);
        graphics.set_pens(rp, 1, 0, ndk::JAM2);
        graphics.rect_fill(rp, left, line, right, line);
    }

    if ((w.flags & ndk::WFLG_CLOSEGADGET) != 0) {
        draw_close_button(graphics, rp, w.active, w.pressed == BarButton::Close, high_color);
    }
    if ((w.flags & ndk::WFLG_DEPTHGADGET) != 0) {
        draw_depth_button(graphics, rp, w.width, w.active, w.pressed == BarButton::Depth,
                          high_color);
    }

    const i32 text_x = (w.flags & ndk::WFLG_CLOSEGADGET) != 0
                           ? close_button_box().x1 + kTitleGapLeft
                           : 4;
    const i32 text_x1 = (w.flags & ndk::WFLG_DEPTHGADGET) != 0
                            ? depth_button_box(w.width).x0 - kTitleGapRight
                            : w.width - 4;
    const u8 text_pen = high_color ? (w.active ? kTitleActive : kTitleInactive) : (w.active ? 2 : 1);
    graphics.set_pens(rp, text_pen, 0, ndk::JAM1);
    draw_title(graphics, b, rp, w.title, text_x, kTitleBaseline, text_x1);
}

}

void draw_screen_bar(Graphics& graphics, Bus& b, u32 rp, u32 width, u32 title) {
    if (rp == 0 || width == 0) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const bool high_color = rastport_depth(b, rp) >= 8;
    const i32 right = static_cast<i32>(width) - 1;
    const i32 line = static_cast<i32>(kBarHeight);

    if (high_color) {
        graphics.set_pens(rp, kMenuPaperPen, 0, ndk::JAM2);
        graphics.rect_fill(rp, 0, 0, right, line - 1);
        graphics.set_pens(rp, kMenuLabelPen, 0, ndk::JAM2);
        graphics.rect_fill(rp, 0, line, right, line);
    } else {
        graphics.set_pens(rp, 0, 0, ndk::JAM2);
        graphics.rect_fill(rp, 0, 0, right, line - 1);
        graphics.set_pens(rp, 1, 0, ndk::JAM2);
        graphics.rect_fill(rp, 0, line, right, line);
    }

    if (title != 0) {
        graphics.set_pens(rp, high_color ? kMenuLabelPen : 1, 0, ndk::JAM1);
        draw_title(graphics, b, rp, title, 1, kTitleBaseline, right);
    }
    restore_rp(b, rp, saved);
}

void draw_window_bar(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w) {
    if (rp == 0 || w.width < 2 || w.height <= static_cast<i32>(kBarHeight)) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const bool high_color = rastport_depth(b, rp) >= 8;
    draw_bar_contents(graphics, b, rp, w, high_color);
    restore_rp(b, rp, saved);
}

void draw_window_chrome(Graphics& graphics, Bus& b, u32 rp, const WindowChrome& w,
                        const Rect* keep) {
    if (rp == 0 || w.width <= 0 || w.height <= 0) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const bool high_color = rastport_depth(b, rp) >= 8;
    const i32 right = w.width - 1;
    const i32 bottom = w.height - 1;

    const bool rounded = high_color &&
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

    const u8 frame_pen = high_color ? kFrameDark : 1;
    if (rounded) {
        graphics.set_pens(rp, frame_pen, 0, ndk::JAM2);
        graphics.rect_fill(rp, 3, 0, right - 3, 0);
        graphics.rect_fill(rp, 0, bottom, right, bottom);
        graphics.rect_fill(rp, 0, 3, 0, bottom);
        graphics.rect_fill(rp, right, 3, right, bottom);
        graphics.rect_fill(rp, 2, 1, 2, 1);
        graphics.rect_fill(rp, right - 2, 1, right - 2, 1);
        graphics.rect_fill(rp, 1, 2, 1, 2);
        graphics.rect_fill(rp, right - 1, 2, right - 1, 2);
    } else {
        draw_outline_box(graphics, rp, 0, 0, right, bottom, frame_pen);
    }

    if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
        draw_bar_contents(graphics, b, rp, w, high_color);

        const i32 body_top = static_cast<i32>(w.border_top);
        const i32 body_bottom = bottom - 1;
        if (body_top <= body_bottom && w.width >= 3) {
            if (high_color) {
                graphics.set_pens(rp, kFrameLight, 0, ndk::JAM2);
                graphics.rect_fill(rp, 1, body_top, 1, body_bottom);

                graphics.set_pens(rp, kInactiveBar5, 0, ndk::JAM2);
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
            } else {
                graphics.set_pens(rp, 0, 0, ndk::JAM2);
                if (w.border_right >= 2) {
                    const i32 rx0 = max_i32(w.width - static_cast<i32>(w.border_right), 1);
                    if (rx0 <= right - 1) {
                        graphics.rect_fill(rp, rx0, body_top, right - 1, body_bottom);
                    }
                }
                if (w.border_bottom >= 2) {
                    const i32 by0 = max_i32(w.height - static_cast<i32>(w.border_bottom), body_top);
                    if (by0 <= body_bottom && 1 <= right - 1) {
                        graphics.rect_fill(rp, 1, by0, right - 1, body_bottom);
                    }
                }
                graphics.set_pens(rp, 1, 0, ndk::JAM2);
                if (w.border_right > 2 && w.width >= static_cast<i32>(w.border_right)) {
                    const i32 x = w.width - static_cast<i32>(w.border_right);
                    graphics.rect_fill(rp, x, body_top, x, bottom);
                }
                if (w.border_bottom > 2 && w.height >= static_cast<i32>(w.border_bottom)) {
                    const i32 y = w.height - static_cast<i32>(w.border_bottom);
                    graphics.rect_fill(rp, 0, y, right, y);
                }
            }
        }

        if ((w.flags & ndk::WFLG_SIZEGADGET) != 0) {
            draw_size_gadget(graphics, rp, w.width, w.height, high_color);
        }
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

void draw_menu(Bus& b, const raster::Bitmap& bitmap, const Rect& area, const MenuChrome& menu) {
    if (menu.width <= 0 || !menu_layout::valid(area)) {
        return;
    }
    const raster::Clip clip{area.x0, area.y0, area.x1, area.y1};
    const MenuPens pens = menu_pens(menu.depth >= 8);
    const i32 right = menu.width - 1;
    const i32 line = static_cast<i32>(kBarHeight);

    // The bar is drawn in the same pens as the screen title bar it replaces.
    if (pens.high_color) {
        fill(b, bitmap, clip, 0, 0, right, line - 1, kMenuPaperPen);
        fill(b, bitmap, clip, 0, line, right, line, kMenuLabelPen);
    } else {
        fill(b, bitmap, clip, 0, 0, right, line - 1, kFlatPaper);
        fill(b, bitmap, clip, 0, line, right, line, kFlatLabel);
    }

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
        } else if (!enabled && pens.high_color) {
            pen = kMenuGhostPen;
        }
        draw_guest_text(b, bitmap, clip, pen, title.x0 + 8, kTitleBaseline, menu.titles[i].name,
                        title.x1 - 4);
        if (!enabled && !pens.high_color) {
            ghost(b, bitmap, clip, Rect{title.x0, 0, title.x1, line - 1}, kFlatPaper);
        }
    }

    draw_menu_box(b, bitmap, clip, menu.box, true, menu.items, menu.item_rects, menu.item_count,
                  menu.selected_item, pens, menu.strong_shade, menu.light_shade);
    draw_menu_box(b, bitmap, clip, menu.sub_box, false, menu.sub_items, menu.sub_item_rects,
                  menu.sub_item_count, menu.selected_sub, pens, menu.strong_shade,
                  menu.light_shade);
}

void draw_prop_gadget(Graphics& graphics, Bus& b, u32 rp, const PropChrome& g) {
    if (rp == 0 || g.box.x1 < g.box.x0 || g.box.y1 < g.box.y0) {
        return;
    }
    const RpState saved = save_rp(b, rp);
    const bool high_color = rastport_depth(b, rp) >= 8;

    if (high_color) {
        graphics.set_pens(rp, kTrack, 0, ndk::JAM2);
        graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x1, g.box.y1);
        if ((g.pflags & ndk::PROPBORDERLESS) == 0) {
            graphics.set_pens(rp, kFrameDark, 0, ndk::JAM2);
            graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x1, g.box.y0);
            graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x0, g.box.y1);
        }
    } else {
        if ((g.pflags & ndk::PROPBORDERLESS) != 0) {
            graphics.set_pens(rp, 0, 0, ndk::JAM2);
            graphics.rect_fill(rp, g.box.x0, g.box.y0, g.box.x1, g.box.y1);
        } else {
            draw_frame_box(graphics, rp, g.box, 0, 1);
        }
    }

    if (g.knob.x0 <= g.knob.x1 && g.knob.y0 <= g.knob.y1) {
        draw_knob(graphics, rp, g.knob, g.pflags, g.gflags, high_color);
    }
    restore_rp(b, rp, saved);
}

void paint_arrow_button(u8* pens, i32 width, i32 height, Arrow arrow, bool pressed,
                        bool high_color) {
    if (pens == nullptr || width <= 0 || height <= 0) {
        return;
    }
    auto put = [&](i32 x, i32 y, u8 pen) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            pens[y * width + x] = pen;
        }
    };
    const u8 back = high_color ? kTrack : 0;
    for (i32 i = 0; i < width * height; ++i) {
        pens[i] = back;
    }
    const i32 radius = min_i32(2, min_i32((width - 1) / 2, (height - 1) / 2));
    // The body, shaded from top to bottom as a vertical knob is.
    const u8 shade = pressed ? kActiveBar2 : kKnob0;
    const i32 steps = pressed ? 4 : 3;
    const i32 inner_h = height - 2;
    for (i32 y = 1; y < height - 1; ++y) {
        const i32 dy = min_i32(y, height - 1 - y);
        const i32 ins = max_i32(1, rounded_inset(dy, radius));
        const u8 pen = high_color ? static_cast<u8>(shade + ((y - 1) * steps) / inner_h)
                                  : (pressed ? 3 : 0);
        for (i32 x = ins; x <= width - 1 - ins; ++x) {
            put(x, y, pen);
        }
    }
    // The rounded outline.
    const u8 line = high_color ? kFrameDark : 1;
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
    // The triangle: three rows, two to six pixels across, centred.
    const u8 mark = high_color ? (pressed ? kFrameLight : kFrameDark) : (pressed ? 2 : 1);
    const i32 mid_x0 = (width - 1) / 2;
    const i32 mid_x1 = width / 2;
    const i32 mid_y0 = (height - 1) / 2;
    const i32 mid_y1 = height / 2;
    const i32 top = (height - 3) / 2;
    const i32 left = (width - 3) / 2;
    for (i32 i = 0; i < 3; ++i) {
        if (arrow == Arrow::Up || arrow == Arrow::Down) {
            const i32 y = arrow == Arrow::Up ? top + i : top + 2 - i;
            for (i32 x = mid_x0 - i; x <= mid_x1 + i; ++x) {
                put(x, y, mark);
            }
        } else {
            const i32 x = arrow == Arrow::Left ? left + i : left + 2 - i;
            for (i32 y = mid_y0 - i; y <= mid_y1 + i; ++y) {
                put(x, y, mark);
            }
        }
    }
}

}
