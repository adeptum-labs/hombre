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

namespace hombre {

class Graphics;

namespace chrome {

constexpr u32 kBarHeight = 14;
constexpr u32 kBorderTop = 15;
constexpr u32 kBorderLeft = 2;
constexpr u32 kBorderRight = 2;
constexpr u32 kBorderBottom = 2;
constexpr u32 kSizeBorderRight = 12;
constexpr u32 kSizeBorderBottom = 10;

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
};

struct PropChrome {
    Rect box{};
    Rect knob{};
    u16 pflags = 0;
    u16 gflags = 0;
};

// One menu strip in menu mode: the bar titles, the open title's drop-down
// box and the submenu box, with the indices of the open title and the
// highlighted items. The shade tables darken the drop shadows and are null
// on screens of fewer than eight planes, which draw menus flat.
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
};

void draw_screen_bar(Graphics& graphics, native::Bus& b, u32 rp, u32 width, u32 title);
void draw_window_bar(Graphics& graphics, native::Bus& b, u32 rp, const WindowChrome& w);
void draw_window_chrome(Graphics& graphics, native::Bus& b, u32 rp, const WindowChrome& w,
                        const Rect* keep = nullptr);
void draw_prop_gadget(Graphics& graphics, native::Bus& b, u32 rp, const PropChrome& g);
void draw_menu(native::Bus& b, const raster::Bitmap& bitmap, const Rect& clip,
               const MenuChrome& menu);

}

}
