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
#include "menu_layout.hpp"
#include "native.hpp"
#include "raster.hpp"
#include "sysimage.hpp"

namespace hombre {

class Graphics;

namespace ndk {

// WFLG_HASZOOM (intuition/intuition.h): the window has a zoom gadget, which
// only Intuition sets, for a window opened with WA_Zoom.
constexpr u32 WFLG_HASZOOM = 0x20000000;

}

namespace chrome {

// The two looks of the window frames, the screen title bar, the border
// scroll gadgets and the menus. Classic is the AmigaOS 3.1 look, drawn in
// the pens of the screen's DrawInfo. Modern is the shaded look, drawn in the
// chrome pens of chrome_palette.hpp, which a screen of five or more planes
// has at physical_pen.
enum class Style : u8 { Classic, Modern };

constexpr Style style_for_depth(u32 depth) { return depth >= 5 ? Style::Modern : Style::Classic; }

// The pens of a DrawInfo, DETAILPEN through BARTRIMPEN, indexed as
// sysimage.hpp has them. Only the classic look draws in them.
struct DrawInfoPens {
    u16 pen[sysimage::kPenCount]{};
};

// The screen's WBorTop: programs size a title bar as WBorTop plus the
// height of the screen's font plus one.
constexpr u32 kWBorTop = 6;
constexpr u32 kBorderLeft = 2;
constexpr u32 kBorderRight = 2;
constexpr u32 kBorderBottom = 2;
constexpr u32 kSizeBorderRight = 14;
constexpr u32 kSizeBorderBottom = 14;

// The tallest font a title bar is made for: sc_BarHeight and wd_BorderTop
// are bytes.
constexpr i32 kMaxTitleFontHeight = 100;

// What the font of a screen or window makes of its title bar: the font
// the title is drawn in, 0 for the built-in one, and the sizes that follow
// from its height. The built-in topaz 8 gives a bar of 14 rows over its
// line, with buttons of 13 by 11.
struct Metrics {
    u32 font = 0;
    i32 font_height = 8;
    i32 font_baseline = 6;
    i32 font_width = 8;

    // The row of the line under the bar, which is the screen's
    // sc_BarHeight, and the first row below it, a window's BorderTop.
    constexpr i32 bar_height() const { return static_cast<i32>(kWBorTop) + font_height; }
    constexpr i32 border_top() const { return bar_height() + 1; }
    // The title's first row and its baseline.
    constexpr i32 title_top() const { return 3; }
    constexpr i32 title_baseline() const { return title_top() + font_baseline; }
    // The close, depth and zoom buttons: all the same size and centred in
    // the bar, a row clear of its frame and its line.
    constexpr i32 button_top() const { return 2; }
    constexpr i32 button_height() const { return bar_height() - 3; }
    constexpr i32 button_width() const { return button_height() + 2; }
};

// How far in from the window's left and right edges the outer buttons are.
constexpr i32 kBarButtonInset = 3;
// The gaps between a bar button and the first and last pixel of the title.
constexpr i32 kTitleGapLeft = 4;
constexpr i32 kTitleGapRight = 3;

enum class BarButton : u8 { None = 0, Close = 1, Depth = 2, Zoom = 3 };

// The box of the close button, in window coordinates.
constexpr Rect close_button_box(const Metrics& m = {}) {
    return Rect{kBarButtonInset, m.button_top(), kBarButtonInset + m.button_width() - 1,
                m.button_top() + m.button_height() - 1};
}

// The box of the depth button of a window `width` pixels wide, in window
// coordinates.
constexpr Rect depth_button_box(i32 width, const Metrics& m = {}) {
    const i32 x1 = width - 1 - kBarButtonInset;
    return Rect{x1 - m.button_width() + 1, m.button_top(), x1,
                m.button_top() + m.button_height() - 1};
}

// The box of the zoom button of a window `width` pixels wide with the given
// flags, in window coordinates. It is as large as the depth button and ends
// in the depth button's first column, so the two share one; without a depth
// button it takes the depth button's place.
constexpr Rect zoom_button_box(i32 width, u32 flags, const Metrics& m = {}) {
    const Rect depth = depth_button_box(width, m);
    if ((flags & ndk::WFLG_DEPTHGADGET) == 0) {
        return depth;
    }
    return Rect{depth.x0 - m.button_width() + 1, depth.y0, depth.x0, depth.y1};
}

// The first column of a title: after the close button, or near the left
// edge when there is none.
constexpr i32 title_left(u32 flags, const Metrics& m = {}) {
    return (flags & ndk::WFLG_CLOSEGADGET) != 0 ? close_button_box(m).x1 + kTitleGapLeft : 4;
}

// The last column of a title in a window `width` pixels wide: before the
// leftmost of the zoom and depth buttons, or near the right edge when there
// is none.
constexpr i32 title_right(i32 width, u32 flags, const Metrics& m = {}) {
    if ((flags & ndk::WFLG_HASZOOM) != 0) {
        return zoom_button_box(width, flags, m).x0 - kTitleGapRight;
    }
    return (flags & ndk::WFLG_DEPTHGADGET) != 0 ? depth_button_box(width, m).x0 - kTitleGapRight
                                                 : width - 4;
}

// The narrowest window with the flags' buttons whose title bar holds a
// title text_width pixels wide.
constexpr i32 title_bar_width(i32 text_width, u32 flags, const Metrics& m = {}) {
    return title_left(flags, m) + text_width - 1 - title_right(0, flags, m);
}

struct WindowChrome {
    i32 width = 0;
    i32 height = 0;
    u32 flags = 0;
    u32 title = 0;
    u32 border_left = 0;
    u32 border_top = 0;
    u32 border_right = 0;
    u32 border_bottom = 0;
    bool active = false;
    BarButton pressed = BarButton::None;  // the bar button the pointer holds down
    Metrics metrics{};
    DrawInfoPens pens{};
};

struct PropChrome {
    Rect box{};
    Rect knob{};
    u16 pflags = 0;
    u16 gflags = 0;
    DrawInfoPens pens{};
};

// One menu strip in menu mode: the bar titles, the open title's drop-down
// box and the submenu box, with the indices of the open title and the
// highlighted items. The shade tables darken the drop shadows of the modern
// look and are null on screens of the classic look, whose menus have none.
struct MenuChrome {
    i32 width = 0;
    const menu_layout::Title* titles = nullptr;
    const Rect* title_rects = nullptr;
    u32 title_count = 0;
    i32 selected_title = -1;
    Rect box{0, 0, -1, -1};
    const menu_layout::Item* items = nullptr;
    const Rect* item_rects = nullptr;
    u32 item_count = 0;
    i32 selected_item = -1;
    Rect sub_box{0, 0, -1, -1};
    const menu_layout::Item* sub_items = nullptr;
    const Rect* sub_item_rects = nullptr;
    u32 sub_item_count = 0;
    i32 selected_sub = -1;
    u32 depth = 0;
    const u8* strong_shade = nullptr;
    const u8* light_shade = nullptr;
    // The bar is as high as the screen's, the titles and items are drawn
    // in the font of the window the strip belongs to. bitmap_addr is the
    // BitMap menus are drawn on, whose palette the text blends with.
    i32 bar_height = Metrics{}.bar_height();
    Metrics metrics{};
    u32 bitmap_addr = 0;
    DrawInfoPens pens{};
};

void draw_screen_bar(Graphics& graphics, native::Bus& b, u32 rp, u32 width, u32 title,
                     const DrawInfoPens& pens, const Metrics& metrics = {});
// The bar and what changes with the window's state: the classic look takes
// the whole frame, whose borders change colour with the bar.
void draw_window_bar(Graphics& graphics, native::Bus& b, u32 rp, const WindowChrome& w);
void draw_window_chrome(Graphics& graphics, native::Bus& b, u32 rp, const WindowChrome& w,
                        const Rect* keep = nullptr);
void draw_prop_gadget(Graphics& graphics, native::Bus& b, u32 rp, const PropChrome& g);
void draw_menu(Graphics& graphics, native::Bus& b, const raster::Bitmap& bitmap,
               const Rect& clip, const MenuChrome& menu);

enum class Arrow : u8 { Up = 0, Down = 1, Left = 2, Right = 3 };

// The rows of an arrow button's triangle, from the button's size across
// and along the arrow: three on a small button, more on a larger one.
constexpr i32 arrow_rows(i32 across, i32 along) {
    const i32 by_across = across / 4;
    const i32 by_along = along / 3;
    const i32 rows = by_across < by_along ? by_across : by_along;
    return rows > 3 ? rows : 3;
}

// A scroll arrow button as width x height pens of a screen of depth planes
// in out, row by row, with a triangle pointing the arrow's way. The modern
// look is that of the sliders' knobs: rounded, shaded and outlined, in the
// chrome pens at physical_pen(depth); pressed, it takes the shading of a held
// knob. The classic look is the 3.1 arrow image of an active window's border
// in the DrawInfo pens: the fill pen with a raised edge, a sunken one when
// pressed.
void paint_arrow_button(u8* out, i32 width, i32 height, Arrow arrow, bool pressed, Style style,
                        u32 depth, const DrawInfoPens& drawinfo);

}

}
