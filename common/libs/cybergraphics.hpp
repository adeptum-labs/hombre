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
#include "exec.hpp"

// cybergraphics.library version 41 for programs written for CyberGraphX
// cards: it names Hombre's deep display modes, tells a bitmap's layout and
// reads and writes pixels on the deep bitmaps graphics.library made as well
// as on planar ones. A bitmap is deep only when graphics.library keeps it in
// its table; every other BitMap is planar, whatever its bm_Depth says.

namespace hombre {

class Graphics;

// Library vector offsets of cybergraphics.library, n of LVO -n, as the 41
// function descriptions number them.
namespace cybergraphics_lvo {

constexpr u32 IsCyberModeID = 54;
constexpr u32 BestCModeIDTagList = 60;
constexpr u32 CModeRequestTagList = 66;
constexpr u32 AllocCModeListTagList = 72;
constexpr u32 FreeCModeList = 78;
constexpr u32 ScalePixelArray = 90;
constexpr u32 GetCyberMapAttr = 96;
constexpr u32 GetCyberIDAttr = 102;
constexpr u32 ReadRGBPixel = 108;
constexpr u32 WriteRGBPixel = 114;
constexpr u32 ReadPixelArray = 120;
constexpr u32 WritePixelArray = 126;
constexpr u32 MovePixelArray = 132;
constexpr u32 InvertPixelArray = 144;
constexpr u32 FillPixelArray = 150;
constexpr u32 DoCDrawMethodTagList = 156;
constexpr u32 CVideoCtrlTagList = 162;
constexpr u32 LockBitMapTagList = 168;
constexpr u32 UnLockBitMap = 174;
constexpr u32 UnLockBitMapTagList = 180;
constexpr u32 ExtractColor = 186;
constexpr u32 WriteLUTPixelArray = 198;

}

// The attributes, tags, pixel formats and rectangle formats of
// cybergraphx/cybergraphics.h.
namespace cyber {

constexpr u32 INVALID_ID = 0xFFFFFFFF;

constexpr u32 CYBRMATTR_XMOD = 0x80000001;
constexpr u32 CYBRMATTR_BPPIX = 0x80000002;
constexpr u32 CYBRMATTR_PIXFMT = 0x80000004;
constexpr u32 CYBRMATTR_WIDTH = 0x80000005;
constexpr u32 CYBRMATTR_HEIGHT = 0x80000006;
constexpr u32 CYBRMATTR_DEPTH = 0x80000007;
constexpr u32 CYBRMATTR_ISCYBERGFX = 0x80000008;
constexpr u32 CYBRMATTR_ISLINEARMEM = 0x80000009;
constexpr u32 CYBRMATTR_COLORMAP = 0x8000000A;

constexpr u32 CYBRIDATTR_PIXFMT = 0x80000001;
constexpr u32 CYBRIDATTR_WIDTH = 0x80000002;
constexpr u32 CYBRIDATTR_HEIGHT = 0x80000003;
constexpr u32 CYBRIDATTR_DEPTH = 0x80000004;
constexpr u32 CYBRIDATTR_BPPIX = 0x80000005;

constexpr u32 TAG_USER = 0x80000000;
constexpr u32 CYBRBIDTG_TB = TAG_USER + 0x50000;
constexpr u32 CYBRBIDTG_Depth = CYBRBIDTG_TB + 0;
constexpr u32 CYBRBIDTG_NominalWidth = CYBRBIDTG_TB + 1;
constexpr u32 CYBRBIDTG_NominalHeight = CYBRBIDTG_TB + 2;
constexpr u32 CYBRBIDTG_MonitorID = CYBRBIDTG_TB + 3;

constexpr u32 LBMI_WIDTH = 0x84001001;
constexpr u32 LBMI_HEIGHT = 0x84001002;
constexpr u32 LBMI_DEPTH = 0x84001003;
constexpr u32 LBMI_PIXFMT = 0x84001004;
constexpr u32 LBMI_BYTESPERPIX = 0x84001005;
constexpr u32 LBMI_BYTESPERROW = 0x84001006;
constexpr u32 LBMI_BASEADDRESS = 0x84001007;

constexpr u32 PIXFMT_LUT8 = 0;
constexpr u32 PIXFMT_RGB16 = 5;
constexpr u32 PIXFMT_ARGB32 = 11;

constexpr u32 RECTFMT_RGB = 0;
constexpr u32 RECTFMT_RGBA = 1;
constexpr u32 RECTFMT_ARGB = 2;
constexpr u32 RECTFMT_LUT8 = 3;
constexpr u32 RECTFMT_GREY8 = 4;
constexpr u32 RECTFMT_RAW = 5;

constexpr u32 CTABFMT_XRGB8 = 0;

}

class CyberGraphics {
public:
    static constexpr u16 kVersion = 41;

    // Makes "cybergraphics.library" version 41 over the deep bitmaps of
    // graphics.library, which must be initialised, and registers its
    // vectors. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, Graphics& graphics);

    u32 base() const { return base_; }

private:
    friend struct CyberGraphicsNatives;

    Graphics* graphics_ = nullptr;
    u32 base_ = 0;
};

}
