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

namespace hombre {

namespace m68k {
struct Machine;
}

// The signed 32-bit type the AmigaOS structures and the raster coordinates
// use.
using i32 = __INT32_TYPE__;

// An inclusive rectangle, as RectFill takes its corners.
struct Rect {
    i32 x0, y0, x1, y1;
};

// Where a draw reports the part of the displayed screen it changed.
struct DisplaySink {
    void (*refresh)(void* ctx, m68k::Machine& m, u32 screen, const Rect& dirty);
    void* ctx;
};

struct DisplayState {
    DisplaySink sink{nullptr, nullptr};
    u32 front_screen = 0;
};

// Offsets and values of the graphics and Intuition structures, named as the
// NDK assembler includes name them.
namespace ndk {

// struct BitMap
constexpr u32 bm_BytesPerRow = 0;
constexpr u32 bm_Rows = 2;
constexpr u32 bm_Flags = 4;
constexpr u32 bm_Depth = 5;
constexpr u32 bm_Planes = 8;
constexpr u32 BitMap_SIZEOF = 40;
static_assert(BitMap_SIZEOF == 40);

// struct RastPort
constexpr u32 rp_Layer = 0;
constexpr u32 rp_BitMap = 4;
constexpr u32 rp_Mask = 24;
constexpr u32 rp_FgPen = 25;
constexpr u32 rp_BgPen = 26;
constexpr u32 rp_AOlPen = 27;
constexpr u32 rp_DrawMode = 28;
constexpr u32 rp_cp_x = 36;
constexpr u32 rp_cp_y = 38;
constexpr u32 rp_Font = 52;
constexpr u32 rp_TxHeight = 58;
constexpr u32 rp_TxWidth = 60;
constexpr u32 rp_TxBaseline = 62;
constexpr u32 RastPort_SIZEOF = 100;
static_assert(RastPort_SIZEOF == 100);

// struct Layer
constexpr u32 ly_rp = 12;
constexpr u32 ly_MinX = 16;
constexpr u32 ly_MinY = 18;
constexpr u32 ly_MaxX = 20;
constexpr u32 ly_MaxY = 22;
constexpr u32 ly_Window = 40;
constexpr u32 Layer_SIZEOF = 160;
static_assert(Layer_SIZEOF == 160);

// struct ViewPort
constexpr u32 vp_ColorMap = 4;
constexpr u32 vp_DWidth = 24;
constexpr u32 vp_DHeight = 26;
constexpr u32 vp_Modes = 32;
constexpr u32 ViewPort_SIZEOF = 40;
static_assert(ViewPort_SIZEOF == 40);

// struct ColorMap
constexpr u32 cm_Count = 2;
constexpr u32 cm_ColorTable = 4;
constexpr u32 ColorMap_SIZEOF = 52;
static_assert(ColorMap_SIZEOF == 52);

// struct Screen
constexpr u32 sc_FirstWindow = 4;
constexpr u32 sc_LeftEdge = 8;
constexpr u32 sc_TopEdge = 10;
constexpr u32 sc_Width = 12;
constexpr u32 sc_Height = 14;
constexpr u32 sc_Flags = 20;
constexpr u32 sc_Title = 22;
constexpr u32 sc_DefaultTitle = 26;
constexpr u32 sc_BarHeight = 30;
constexpr u32 sc_WBorTop = 35;
constexpr u32 sc_WBorLeft = 36;
constexpr u32 sc_WBorRight = 37;
constexpr u32 sc_WBorBottom = 38;
constexpr u32 sc_Font = 40;
constexpr u32 sc_ViewPort = 44;
constexpr u32 sc_RastPort = 84;
constexpr u32 sc_BitMap = 184;
constexpr u32 sc_DetailPen = 330;
constexpr u32 sc_BlockPen = 331;
constexpr u32 Screen_SIZEOF = 346;
static_assert(Screen_SIZEOF == 346);

// struct NewScreen
constexpr u32 ns_LeftEdge = 0;
constexpr u32 ns_TopEdge = 2;
constexpr u32 ns_Width = 4;
constexpr u32 ns_Height = 6;
constexpr u32 ns_Depth = 8;
constexpr u32 ns_DetailPen = 10;
constexpr u32 ns_BlockPen = 11;
constexpr u32 ns_ViewModes = 12;
constexpr u32 ns_Type = 14;
constexpr u32 ns_Font = 16;
constexpr u32 ns_DefaultTitle = 20;
constexpr u32 ns_CustomBitMap = 28;
constexpr u32 NewScreen_SIZEOF = 32;
static_assert(NewScreen_SIZEOF == 32);

// struct Window
constexpr u32 wd_NextWindow = 0;
constexpr u32 wd_LeftEdge = 4;
constexpr u32 wd_TopEdge = 6;
constexpr u32 wd_Width = 8;
constexpr u32 wd_Height = 10;
constexpr u32 wd_MinWidth = 16;
constexpr u32 wd_MinHeight = 18;
constexpr u32 wd_MaxWidth = 20;
constexpr u32 wd_MaxHeight = 22;
constexpr u32 wd_Flags = 24;
constexpr u32 wd_Title = 32;
constexpr u32 wd_WScreen = 46;
constexpr u32 wd_RPort = 50;
constexpr u32 wd_BorderLeft = 54;
constexpr u32 wd_BorderTop = 55;
constexpr u32 wd_BorderRight = 56;
constexpr u32 wd_BorderBottom = 57;
constexpr u32 wd_IDCMPFlags = 82;
constexpr u32 wd_UserPort = 86;
constexpr u32 wd_WindowPort = 90;
constexpr u32 wd_MessageKey = 94;
constexpr u32 wd_DetailPen = 98;
constexpr u32 wd_BlockPen = 99;
constexpr u32 wd_WLayer = 124;
constexpr u32 wd_IFont = 128;
constexpr u32 wd_MoreFlags = 132;
constexpr u32 Window_SIZEOF = 136;
static_assert(Window_SIZEOF == 136);

// struct NewWindow
constexpr u32 nw_LeftEdge = 0;
constexpr u32 nw_TopEdge = 2;
constexpr u32 nw_Width = 4;
constexpr u32 nw_Height = 6;
constexpr u32 nw_DetailPen = 8;
constexpr u32 nw_BlockPen = 9;
constexpr u32 nw_IDCMPFlags = 10;
constexpr u32 nw_Flags = 14;
constexpr u32 nw_Title = 26;
constexpr u32 nw_Screen = 30;
constexpr u32 nw_MinWidth = 38;
constexpr u32 nw_MinHeight = 40;
constexpr u32 nw_MaxWidth = 42;
constexpr u32 nw_MaxHeight = 44;
constexpr u32 nw_Type = 46;
constexpr u32 NewWindow_SIZEOF = 48;
static_assert(NewWindow_SIZEOF == 48);

// struct IntuiMessage
constexpr u32 im_Class = 20;
constexpr u32 im_Code = 24;
constexpr u32 im_Qualifier = 26;
constexpr u32 im_IAddress = 28;
constexpr u32 im_MouseX = 32;
constexpr u32 im_MouseY = 34;
constexpr u32 im_Seconds = 36;
constexpr u32 im_Micros = 40;
constexpr u32 im_IDCMPWindow = 44;
constexpr u32 im_SpecialLink = 48;
constexpr u32 IntuiMessage_SIZEOF = 52;
static_assert(IntuiMessage_SIZEOF == 52);

// struct TextAttr and struct TextFont
constexpr u32 ta_Name = 0;
constexpr u32 ta_YSize = 4;
constexpr u32 ta_Style = 6;
constexpr u32 ta_Flags = 7;
constexpr u32 TextAttr_SIZEOF = 8;
static_assert(TextAttr_SIZEOF == 8);

constexpr u32 tf_YSize = 20;
constexpr u32 tf_XSize = 24;
constexpr u32 tf_Baseline = 26;
constexpr u32 tf_LoChar = 32;
constexpr u32 tf_HiChar = 33;
constexpr u32 tf_CharData = 34;
constexpr u32 TextFont_SIZEOF = 52;
static_assert(TextFont_SIZEOF == 52);

// Draw modes
constexpr u32 JAM1 = 0;
constexpr u32 JAM2 = 1;
constexpr u32 COMPLEMENT = 2;
constexpr u32 INVERSVID = 4;

constexpr u32 CUSTOMSCREEN = 0x000F;
constexpr u32 SHOWTITLE = 0x0010;

constexpr u32 WFLG_SIZEGADGET = 1;
constexpr u32 WFLG_DRAGBAR = 2;
constexpr u32 WFLG_DEPTHGADGET = 4;
constexpr u32 WFLG_CLOSEGADGET = 8;
constexpr u32 WFLG_REPORTMOUSE = 0x200;
constexpr u32 WFLG_BORDERLESS = 0x800;
constexpr u32 WFLG_ACTIVATE = 0x1000;

// IDCMP flags
constexpr u32 IDCMP_NEWSIZE = 0x2;
constexpr u32 IDCMP_MOUSEBUTTONS = 0x8;
constexpr u32 IDCMP_MOUSEMOVE = 0x10;
constexpr u32 IDCMP_CLOSEWINDOW = 0x200;
constexpr u32 IDCMP_ACTIVEWINDOW = 0x40000;
constexpr u32 IDCMP_INACTIVEWINDOW = 0x80000;

// Input codes
constexpr u32 SELECTDOWN = 0x68;
constexpr u32 SELECTUP = 0xE8;
constexpr u32 MENUDOWN = 0x69;
constexpr u32 MENUUP = 0xE9;
constexpr u32 MIDDLEDOWN = 0x6A;
constexpr u32 MIDDLEUP = 0xEA;

// Window and screen tags
constexpr u32 WA_Left = 0x80000064;
constexpr u32 WA_Top = 0x80000065;
constexpr u32 WA_Width = 0x80000066;
constexpr u32 WA_Height = 0x80000067;
constexpr u32 WA_DetailPen = 0x80000068;
constexpr u32 WA_BlockPen = 0x80000069;
constexpr u32 WA_IDCMP = 0x8000006A;
constexpr u32 WA_Flags = 0x8000006B;
constexpr u32 WA_Title = 0x8000006E;
constexpr u32 WA_ScreenTitle = 0x8000006F;
constexpr u32 WA_CustomScreen = 0x80000070;

constexpr u32 SA_Left = 0x80000021;
constexpr u32 SA_Top = 0x80000022;
constexpr u32 SA_Width = 0x80000023;
constexpr u32 SA_Height = 0x80000024;
constexpr u32 SA_Depth = 0x80000025;
constexpr u32 SA_DetailPen = 0x80000026;
constexpr u32 SA_BlockPen = 0x80000027;
constexpr u32 SA_Title = 0x80000028;

}

}
