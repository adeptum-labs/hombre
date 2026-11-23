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
#include "gfx.hpp"
#include "native.hpp"

// The glyphs of the sysiclass system images, drawn on a RastPort with
// graphics.library. BOOPSI has no drawing of its own, so intuition hands
// the box and the DrawInfo pens here.

namespace hombre {

class Graphics;

namespace sysimage {

// DETAILPEN through BARTRIMPEN.
constexpr u32 kPenCount = 12;

// The places of the pens in what read_pens fills, in DrawInfo's order.
constexpr u32 DetailPen = 0;
constexpr u32 BlockPen = 1;
constexpr u32 TextPen = 2;
constexpr u32 ShinePen = 3;
constexpr u32 ShadowPen = 4;
constexpr u32 FillPen = 5;
constexpr u32 FillTextPen = 6;
constexpr u32 BackgroundPen = 7;
constexpr u32 HighlightTextPen = 8;
constexpr u32 BarDetailPen = 9;
constexpr u32 BarBlockPen = 10;
constexpr u32 BarTrimPen = 11;

// The pens of the DrawInfo at draw_info, or the 3.1 defaults for no
// DrawInfo.
void read_pens(native::Bus& bus, u32 draw_info, u16 (&pens)[kPenCount]);

// Draws the system image of the given SYSIA_Which in box as state shows it,
// keeping its glyph inside the box.
void draw(Graphics& graphics, native::Bus& bus, u32 rp, u32 which, const Rect& box, u32 state,
          const u16* pens);

}

}
