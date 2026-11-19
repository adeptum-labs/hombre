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

// graphics.library written in C++ for code run by the 68000 translator.
//
// It draws on the planar BitMap of a RastPort and has the colour table
// calls, the raster allocator and the built-in 8x8 topaz font. The C++
// methods are the same drawing code the native vectors use, for the
// Intuition windows and the shell console. Every other vector logs
// "graphics: unimplemented LVO -<n>" and ends the run with Unimplemented.
//
// Every guest pointer goes through native::Bus, so a bad one ends the run
// with BusError or AddressError.

namespace hombre {

class Exec;

// Library vector offsets of graphics.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace graphics_lvo {

constexpr u32 ClearScreen = 48;
constexpr u32 TextLength = 54;
constexpr u32 Text = 60;
constexpr u32 SetFont = 66;
constexpr u32 OpenFont = 72;
constexpr u32 CloseFont = 78;
constexpr u32 LoadRGB4 = 192;
constexpr u32 InitRastPort = 198;
constexpr u32 DrawEllipse = 180;
constexpr u32 AreaEllipse = 186;
constexpr u32 WaitBlit = 228;
constexpr u32 SetRast = 234;
constexpr u32 Move = 240;
constexpr u32 Draw = 246;
constexpr u32 AreaMove = 252;
constexpr u32 AreaDraw = 258;
constexpr u32 AreaEnd = 264;
constexpr u32 InitArea = 282;
constexpr u32 SetRGB4 = 288;
constexpr u32 RectFill = 306;
constexpr u32 ReadPixel = 318;
constexpr u32 WritePixel = 324;
constexpr u32 SetAPen = 342;
constexpr u32 SetBPen = 348;
constexpr u32 SetDrMd = 354;
constexpr u32 InitBitMap = 390;
constexpr u32 ScrollRaster = 396;
constexpr u32 InitTmpRas = 468;
constexpr u32 AskFont = 474;
constexpr u32 AndRectRegion = 504;
constexpr u32 OrRectRegion = 510;
constexpr u32 NewRegion = 516;
constexpr u32 ClearRectRegion = 522;
constexpr u32 ClearRegion = 528;
constexpr u32 DisposeRegion = 534;
constexpr u32 XorRectRegion = 558;
constexpr u32 OrRegionRegion = 612;
constexpr u32 AndRegionRegion = 624;
constexpr u32 AllocRaster = 492;
constexpr u32 TextExtent = 690;
constexpr u32 FreeRaster = 498;
constexpr u32 GetRGB4 = 582;
constexpr u32 SetRGB32 = 852;
constexpr u32 LoadRGB32 = 882;
constexpr u32 GetRGB32 = 900;
constexpr u32 SetRGB32CM = 996;
// The last vector of the 3.9 library; the table reaches down to it.
constexpr u32 Last = 1056;

}

class Graphics {
public:
    // Makes graphics.library with its vectors and allocates the built-in
    // topaz font. exec must be initialised. False if exec runs out of
    // memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, DisplayState& display);

    u32 graphics_base() const { return base_; }
    u32 topaz_font() const { return topaz_; }
    // The TextAttr that names the built-in font, as Screen.Font points to one.
    u32 topaz_attr() const { return topaz_attr_; }

    void init_rastport(u32 rp);
    void set_pens(u32 rp, u32 fg, u32 bg, u32 mode);
    void move(u32 rp, i32 x, i32 y);
    void rect_fill(u32 rp, i32 x0, i32 y0, i32 x1, i32 y1);
    void draw_line(u32 rp, i32 x1, i32 y1);
    u32 text(u32 rp, const u8* chars, u32 count);
    void scroll(u32 rp, i32 dx, i32 dy, i32 x0, i32 y0, i32 x1, i32 y1);
    void set_font(u32 rp, u32 font);

private:
    friend struct GraphicsNatives;

    static constexpr u32 kVectors = graphics_lvo::Last / 6;

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    DisplayState* display_ = nullptr;
    u32 base_ = 0;
    u32 topaz_ = 0;
    u32 topaz_name_ = 0;
    u32 topaz_attr_ = 0;
    NativeVector vectors_[kVectors]{};
};

}
