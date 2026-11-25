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

#include "chrome_paint.hpp"

#include "chrome_palette.hpp"

namespace hombre::chrome {

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

namespace {

constexpr i32 kBarButtonRadius = 2;

void fill(const Surface& surface, i32 x0, i32 y0, i32 x1, i32 y1, u8 pen) {
    surface.fill(surface.ctx, Rect{x0, y0, x1, y1}, pen);
}

void draw_frame_box(const Surface& surface, const Rect& box, u8 fill_pen, u8 border_pen) {
    fill(surface, box.x0, box.y0, box.x1, box.y1, fill_pen);
    paint_outline_box(surface, box, border_pen);
}

// The size gadget, as on 3.1: a corner pointing to the bottom right with a
// small light box inside it.
void draw_size_gadget(const Surface& surface, const Look& look, i32 width, i32 height) {
    if (width < static_cast<i32>(kSizeBorderRight) ||
        height < static_cast<i32>(kSizeBorderBottom)) {
        return;
    }
    // The glyph sits in the middle of the corner the two borders share.
    const i32 right = width - 1 - (static_cast<i32>(kSizeBorderRight) - 7) / 2;
    const i32 bottom = height - 1 - (static_cast<i32>(kSizeBorderBottom) - 7) / 2;
    fill(surface, right, bottom - 6, right, bottom, look(kFrameDark));
    fill(surface, right - 6, bottom, right, bottom, look(kFrameDark));
    draw_frame_box(surface, Rect{right - 3, bottom - 3, right, bottom}, look(kFrameLight),
                   look(kFrameDark));
}

void draw_rounded_outline(const Surface& surface, const Rect& box, i32 radius, u8 pen) {
    for (i32 y = box.y0; y <= box.y1; ++y) {
        const i32 dy = min_i32(y - box.y0, box.y1 - y);
        const i32 ins = rounded_inset(dy, radius);
        const i32 lx = box.x0 + ins;
        const i32 rx = box.x1 - ins;
        if (dy == 0) {
            if (lx <= rx) {
                fill(surface, lx, y, rx, y, pen);
            }
        } else {
            const i32 prev_ins = rounded_inset(dy - 1, radius);
            const i32 inner_lx = box.x0 + prev_ins - 1;
            const i32 inner_rx = box.x1 - prev_ins + 1;
            const i32 left_end = max_i32(lx, inner_lx);
            const i32 right_start = min_i32(rx, inner_rx);
            fill(surface, lx, y, left_end, y, pen);
            fill(surface, right_start, y, rx, y, pen);
        }
    }
}

// The body of a title-bar button, in the look of the sliders' knobs:
// rounded, outlined and shaded from top to bottom. Held down, it takes the
// shading of a held knob.
void draw_bar_button_body(const Surface& surface, const Look& look, const Rect& box,
                          bool pressed) {
    const u8 shade = pressed ? kActiveBar2 : kKnob0;
    const i32 steps = pressed ? 4 : 3;
    const i32 inner_h = box.y1 - box.y0 - 1;
    for (i32 y = box.y0 + 1; y < box.y1; ++y) {
        const i32 dy = min_i32(y - box.y0, box.y1 - y);
        const i32 ins = max_i32(1, rounded_inset(dy, kBarButtonRadius));
        const u8 pen = look(static_cast<u8>(shade + ((y - box.y0 - 1) * steps) / inner_h));
        fill(surface, box.x0 + ins, y, box.x1 - ins, y, pen);
    }
    draw_rounded_outline(surface, box, kBarButtonRadius, look(kFrameDark));
}

// The close button: as on 3.1, a small light square outlined in the dark
// pen in the middle of the body. Held down, the two swap.
void draw_close_button(const Surface& surface, const Look& look, const Metrics& metrics,
                       bool active, bool pressed) {
    const Rect box = close_button_box(metrics);
    draw_bar_button_body(surface, look, box, pressed);
    u8 line = look(active ? kFrameDark : kTitleInactive);
    u8 face = look(kFrameLight);
    if (pressed) {
        const u8 swap = line;
        line = face;
        face = swap;
    }
    const i32 cx = (box.x0 + box.x1) / 2;
    const i32 cy = (box.y0 + box.y1) / 2;
    draw_frame_box(surface, Rect{cx - 3, cy - 2, cx + 3, cy + 2}, face, line);
}

// The depth button: two overlapping sheets, the light one in front of the
// shaded one.
void draw_depth_button(const Surface& surface, const Look& look, i32 width,
                       const Metrics& metrics, bool active, bool pressed) {
    const Rect box = depth_button_box(width, metrics);
    draw_bar_button_body(surface, look, box, pressed);
    const u8 line = look(pressed ? kFrameLight : (active ? kFrameDark : kTitleInactive));
    const i32 cx = (box.x0 + box.x1) / 2;
    const i32 cy = (box.y0 + box.y1) / 2;
    const Rect behind{cx - 3, cy - 3, cx + 1, cy + 1};
    if (pressed) {
        // The sheet behind lets the held body's shading show through.
        paint_outline_box(surface, behind, line);
    } else {
        draw_frame_box(surface, behind, look(kKnob2), line);
    }
    draw_frame_box(surface, Rect{cx - 1, cy - 1, cx + 3, cy + 3}, look(kFrameLight), line);
}

// The zoom button: a large sheet with a small light one in its top left
// corner.
void draw_zoom_button(const Surface& surface, const Look& look, const Rect& box, bool active,
                      bool pressed) {
    draw_bar_button_body(surface, look, box, pressed);
    const u8 line = look(pressed ? kFrameLight : (active ? kFrameDark : kTitleInactive));
    const i32 cx = (box.x0 + box.x1) / 2;
    const i32 cy = (box.y0 + box.y1) / 2;
    const Rect sheet{cx - 3, cy - 3, cx + 3, cy + 3};
    if (pressed) {
        // The sheet lets the held body's shading show through.
        paint_outline_box(surface, sheet, line);
    } else {
        draw_frame_box(surface, sheet, look(kKnob2), line);
    }
    draw_frame_box(surface, Rect{sheet.x0, sheet.y0, cx, cy}, look(kFrameLight), line);
}

void draw_knob(const Surface& surface, const Look& look, const Rect& knob, u16 pflags,
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
                    fill(surface, x, y0, x, y1, look(static_cast<u8>(grad_base + step)));
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
                    fill(surface, x0, y, x1, y, look(static_cast<u8>(grad_base + step)));
                }
            }
        }
    }

    draw_rounded_outline(surface, knob, radius, look(kFrameDark));

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
            fill(surface, gx0, cy - 2, gx1, cy - 2, look(kFrameDark));
            fill(surface, gx0, cy, gx1, cy, look(kFrameDark));
            fill(surface, gx0, cy + 2, gx1, cy + 2, look(kFrameDark));
        }
    } else if (horiz_only && kw >= 12) {
        i32 gy0 = knob.y0 + 2 <= knob.y1 - 2 ? knob.y0 + 2 : knob.y0 + 1;
        i32 gy1 = knob.y0 + 2 <= knob.y1 - 2 ? knob.y1 - 2 : knob.y1 - 1;
        if (gy1 - gy0 + 1 > 5) {
            gy0 = cy - 2;
            gy1 = cy + 2;
        }
        if (gy0 <= gy1) {
            fill(surface, cx - 2, gy0, cx - 2, gy1, look(kFrameDark));
            fill(surface, cx, gy0, cx, gy1, look(kFrameDark));
            fill(surface, cx + 2, gy0, cx + 2, gy1, look(kFrameDark));
        }
    }
}

void draw_modern_bar(const Surface& surface, const Look& look, const WindowChrome& w,
                     const Title& title) {
    const i32 left = 1;
    const i32 right = w.width - 2;
    const Metrics& metrics = w.metrics;
    const i32 line = metrics.bar_height();

    const bool rounded =
        (w.flags & (ndk::WFLG_BORDERLESS | ndk::WFLG_BACKDROP)) == 0 && w.width >= 7;
    if (rounded) {
        fill(surface, 2, 1, 2, 1, look(kFrameDark));
        fill(surface, w.width - 3, 1, w.width - 3, 1, look(kFrameDark));
        fill(surface, 1, 2, 1, 2, look(kFrameDark));
        fill(surface, w.width - 2, 2, w.width - 2, 2, look(kFrameDark));

        fill(surface, 3, 1, w.width - 4, 1, look(kFrameLight));
    } else {
        fill(surface, left, 1, right, 1, look(kFrameLight));
    }

    const u8 base = w.active ? kActiveBar0 : kInactiveBar0;
    const i32 steps = w.active ? 8 : 6;
    const i32 rows = line - 2;
    for (i32 y = 2; y < line; ++y) {
        const i32 step = ((y - 2) * steps) / rows;
        const i32 row_left = (rounded && y == 2) ? 2 : left;
        const i32 row_right = (rounded && y == 2) ? w.width - 3 : right;
        fill(surface, row_left, y, row_right, y, look(static_cast<u8>(base + step)));
    }
    fill(surface, left, line, right, line, look(kFrameDark));

    if ((w.flags & ndk::WFLG_CLOSEGADGET) != 0) {
        draw_close_button(surface, look, metrics, w.active, w.pressed == BarButton::Close);
    }
    if ((w.flags & ndk::WFLG_HASZOOM) != 0) {
        draw_zoom_button(surface, look, zoom_button_box(w.width, w.flags, metrics), w.active,
                         w.pressed == BarButton::Zoom);
    }
    if ((w.flags & ndk::WFLG_DEPTHGADGET) != 0) {
        draw_depth_button(surface, look, w.width, metrics, w.active,
                          w.pressed == BarButton::Depth);
    }
    paint_bar_title(surface, w, title, look(w.active ? kTitleActive : kTitleInactive));
}

// The frame of a window as the modern look has it: the outline, the bar
// and the borders, and the size gadget.
void draw_modern_frame(const Surface& surface, const Look& look, const WindowChrome& w,
                       const Title& title, bool rounded) {
    const i32 right = w.width - 1;
    const i32 bottom = w.height - 1;

    if (rounded) {
        fill(surface, 3, 0, right - 3, 0, look(kFrameDark));
        fill(surface, 0, bottom, right, bottom, look(kFrameDark));
        fill(surface, 0, 3, 0, bottom, look(kFrameDark));
        fill(surface, right, 3, right, bottom, look(kFrameDark));
        fill(surface, 2, 1, 2, 1, look(kFrameDark));
        fill(surface, right - 2, 1, right - 2, 1, look(kFrameDark));
        fill(surface, 1, 2, 1, 2, look(kFrameDark));
        fill(surface, right - 1, 2, right - 1, 2, look(kFrameDark));
    } else {
        paint_outline_box(surface, Rect{0, 0, right, bottom}, look(kFrameDark));
    }

    if ((w.flags & ndk::WFLG_BORDERLESS) == 0) {
        draw_modern_bar(surface, look, w, title);

        const i32 body_top = static_cast<i32>(w.border_top);
        const i32 body_bottom = bottom - 1;
        if (body_top <= body_bottom && w.width >= 3) {
            fill(surface, 1, body_top, 1, body_bottom, look(kFrameLight));

            if (w.border_left > 2) {
                const i32 lx1 = min_i32(static_cast<i32>(w.border_left) - 1, right - 1);
                if (2 <= lx1) {
                    fill(surface, 2, body_top, lx1, body_bottom, look(kInactiveBar5));
                }
            }
            if (w.border_right >= 2) {
                const i32 rx0 = max_i32(w.width - static_cast<i32>(w.border_right), 2);
                if (rx0 <= right - 1) {
                    fill(surface, rx0, body_top, right - 1, body_bottom, look(kInactiveBar5));
                }
            }
            if (w.border_bottom >= 2) {
                const i32 by0 = max_i32(w.height - static_cast<i32>(w.border_bottom), body_top);
                if (by0 <= body_bottom && 2 <= right - 1) {
                    fill(surface, 2, by0, right - 1, body_bottom, look(kInactiveBar5));
                }
            }
        }

        if ((w.flags & ndk::WFLG_SIZEGADGET) != 0) {
            draw_size_gadget(surface, look, w.width, w.height);
        }
    }
}

}

void paint_outline_box(const Surface& surface, const Rect& box, u8 pen) {
    fill(surface, box.x0, box.y0, box.x1, box.y0, pen);
    fill(surface, box.x0, box.y1, box.x1, box.y1, pen);
    fill(surface, box.x0, box.y0, box.x0, box.y1, pen);
    fill(surface, box.x1, box.y0, box.x1, box.y1, pen);
}

void paint_title(const Surface& surface, const Title& title, i32 x, i32 x1, u8 pen,
                 const Metrics& metrics) {
    const i32 room = x1 - x + 1;
    if (title.chars == nullptr || room <= 0) {
        return;
    }
    const u32 length =
        surface.text_fit(surface.ctx, metrics.font, title.chars, title.length, room);
    if (length != 0) {
        surface.text(surface.ctx, metrics.font, x, metrics.title_baseline(), pen, title.chars,
                     length);
    }
}

// The title between the bar buttons, or the frame where a button is missing.
void paint_bar_title(const Surface& surface, const WindowChrome& w, const Title& title,
                     u8 pen) {
    paint_title(surface, title, title_left(w.flags, w.metrics),
                title_right(w.width, w.flags, w.metrics), pen, w.metrics);
}

void paint_window_clear(const Surface& surface, const WindowChrome& w, const Rect* keep,
                        bool rounded) {
    const i32 right = w.width - 1;
    const i32 bottom = w.height - 1;
    if (keep != nullptr && keep->x0 <= keep->x1 && keep->y0 <= keep->y1) {
        // Only clear areas outside the preserved interior; borders are redrawn below.
        if (keep->x1 < right) {
            fill(surface, keep->x1 + 1, 0, right, bottom, 0);
        }
        if (keep->y1 < bottom) {
            fill(surface, 0, keep->y1 + 1, right, bottom, 0);
        }
        if (rounded) {
            fill(surface, 0, 0, 2, 0, 0);
            fill(surface, 0, 1, 1, 1, 0);
            fill(surface, 0, 2, 0, 2, 0);
            fill(surface, right - 2, 0, right, 0, 0);
            fill(surface, right - 1, 1, right, 1, 0);
            fill(surface, right, 2, right, 2, 0);
        }
    } else {
        fill(surface, 0, 0, right, bottom, 0);
    }
}

void paint_window_interior(const Surface& surface, const WindowChrome& w, const Rect* keep) {
    const i32 ix0 = static_cast<i32>(w.border_left);
    const i32 iy0 = static_cast<i32>(w.border_top);
    const i32 ix1 = w.width - static_cast<i32>(w.border_right) - 1;
    const i32 iy1 = w.height - static_cast<i32>(w.border_bottom) - 1;
    if (keep == nullptr && ix0 <= ix1 && iy0 <= iy1) {
        fill(surface, ix0, iy0, ix1, iy1, 0);
    }
}

void paint_modern_bar(const Surface& surface, const WindowChrome& w, const Title& title) {
    draw_modern_bar(surface, Look{surface.depth}, w, title);
}

void paint_modern_window(const Surface& surface, const WindowChrome& w, const Title& title,
                         const Rect* keep) {
    if (w.width <= 0 || w.height <= 0) {
        return;
    }
    const bool rounded =
        (w.flags & (ndk::WFLG_BORDERLESS | ndk::WFLG_BACKDROP)) == 0 &&
        w.width >= 7 && w.height >= 4;
    paint_window_clear(surface, w, keep, rounded);
    draw_modern_frame(surface, Look{surface.depth}, w, title, rounded);
    paint_window_interior(surface, w, keep);
}

void paint_modern_prop(const Surface& surface, const PropChrome& g) {
    if (g.box.x1 < g.box.x0 || g.box.y1 < g.box.y0) {
        return;
    }
    const Look look{surface.depth};
    const bool borderless = (g.pflags & ndk::PROPBORDERLESS) != 0;

    fill(surface, g.box.x0, g.box.y0, g.box.x1, g.box.y1, look(kTrack));
    if (!borderless) {
        fill(surface, g.box.x0, g.box.y0, g.box.x1, g.box.y0, look(kFrameDark));
        fill(surface, g.box.x0, g.box.y0, g.box.x0, g.box.y1, look(kFrameDark));
    }

    if (g.knob.x0 <= g.knob.x1 && g.knob.y0 <= g.knob.y1) {
        draw_knob(surface, look, g.knob, g.pflags, g.gflags);
    }
}

}
