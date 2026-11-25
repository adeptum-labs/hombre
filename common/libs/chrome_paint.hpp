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

#pragma once

#include "../types.hpp"
#include "chrome.hpp"
#include "chrome_palette.hpp"
#include "chrome_surface.hpp"

namespace hombre::chrome {

constexpr i32 min_i32(i32 a, i32 b) { return a < b ? a : b; }

constexpr i32 max_i32(i32 a, i32 b) { return a > b ? a : b; }

// The pens the modern look draws in on a screen of depth planes: a chrome pen
// of chrome_palette.hpp through its place on that screen.
struct Look {
    u32 depth;

    constexpr u8 operator()(u8 chrome_pen) const { return physical_pen(chrome_pen, depth); }
};

i32 rounded_inset(i32 d, i32 radius);

void paint_outline_box(const Surface&, const Rect& box, u8 pen);
void paint_bar_title(const Surface&, const WindowChrome&, const Title&, u8 pen);
void paint_window_clear(const Surface&, const WindowChrome&, const Rect* keep, bool rounded);
void paint_window_interior(const Surface&, const WindowChrome&, const Rect* keep);

// The modern look: the clear of a whole window (all but keep when keep is
// given and valid), the frame, the bar, its buttons, the size gadget and,
// without keep, the cleared interior.
void paint_modern_window(const Surface&, const WindowChrome&, const Title&,
                         const Rect* keep = nullptr);
void paint_modern_bar(const Surface&, const WindowChrome&, const Title&);
// The modern track and knob of a prop gadget.
void paint_modern_prop(const Surface&, const PropChrome&);
// The characters of the title that fit from x to x1, on the title baseline.
void paint_title(const Surface&, const Title&, i32 x, i32 x1, u8 pen, const Metrics&);

// The pens of a requester on screens of the modern look, and the SHINEPEN,
// SHADOWPEN, BACKGROUNDPEN and TEXTPEN of the screen's DrawInfo on the
// others.
struct BoxPens {
    u8 light;
    u8 dark;
    u8 paper;
    u8 text;
};

BoxPens requester_pens(u32 depth, const DrawInfoPens&);

// Paper with a two-pixel bevel, the light edge top left unless recessed.
void paint_box(const Surface&, const Rect&, const BoxPens&, bool recessed);
// The text that fits, centred in the box.
void paint_label(const Surface&, const Rect&, const u8* chars, u32 length, u8 pen,
                 const Metrics&);

}
