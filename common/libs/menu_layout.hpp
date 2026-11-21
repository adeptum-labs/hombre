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

// The geometry of a menu strip: the titles in the screen bar, the drop-down
// box of the open title and the submenu box beside a highlighted item.
// Positions in the Title and Item descriptions are the ones the menu
// structures carry; every rectangle this header returns is in screen
// coordinates. The drawing lives elsewhere: this is only arithmetic.

namespace hombre::menu_layout {

constexpr u32 kMaxTitles = 32;
constexpr u32 kMaxItems = 64;
constexpr u32 kMaxSubItems = 31;  // 31 in the menu number means no subitem
constexpr u32 kNoTitle = 0xFFFFFFFF;

// Menu.Flags and MenuItem.Flags, as intuition >= 2.0 numbers them.
constexpr u16 kTitleEnabled = 0x0001;
constexpr u16 kItemCheckIt = 0x0001;
constexpr u16 kItemText = 0x0002;
constexpr u16 kItemCommSeq = 0x0004;
constexpr u16 kItemMenuToggle = 0x0008;
constexpr u16 kItemEnabled = 0x0010;
constexpr u16 kItemChecked = 0x0100;

struct Title {
    i32 left = 0;
    i32 width = 0;
    u32 first = 0;
    u32 name = 0;
    u16 flags = 0;

    bool enabled() const { return (flags & kTitleEnabled) != 0; }
};

struct Item {
    i32 left = 0;
    i32 top = 0;
    i32 width = 0;
    i32 height = 0;
    u32 addr = 0;
    u32 label = 0;
    u32 sub = 0;
    u32 font = 0;  // the TextAttr its IntuiText names, 0 for the menu's own font
    u16 flags = 0;
    u8 command = 0;

    bool enabled() const { return (flags & kItemEnabled) != 0; }
    bool text() const { return (flags & kItemText) != 0; }
    bool checkit() const { return (flags & kItemCheckIt) != 0; }
    bool checked() const { return (flags & kItemChecked) != 0; }
    bool commseq() const { return (flags & kItemCommSeq) != 0; }
};

struct Geometry {
    Rect title[kMaxTitles]{};
    u32 title_count = 0;
    Rect box{0, 0, -1, -1};
    Rect item[kMaxItems]{};
    u32 item_count = 0;
    Rect sub_box{0, 0, -1, -1};
    Rect sub_item[kMaxItems]{};
    u32 sub_item_count = 0;
};

inline Rect empty_rect() { return Rect{0, 0, -1, -1}; }

inline bool valid(const Rect& r) { return r.x1 >= r.x0 && r.y1 >= r.y0; }

namespace detail {

struct Extent {
    i32 left;
    i32 top;
    i32 right;
    i32 bottom;
};

inline Extent extent(const Item* items, u32 count) {
    Extent e{items[0].left, items[0].top, items[0].left + items[0].width,
             items[0].top + items[0].height};
    for (u32 i = 1; i < count; ++i) {
        if (items[i].left < e.left) {
            e.left = items[i].left;
        }
        if (items[i].top < e.top) {
            e.top = items[i].top;
        }
        if (items[i].left + items[i].width > e.right) {
            e.right = items[i].left + items[i].width;
        }
        if (items[i].top + items[i].height > e.bottom) {
            e.bottom = items[i].top + items[i].height;
        }
    }
    return e;
}

}

// A drop-down box is its items' extent plus a frame of kBoxPadX columns
// and kBoxPadY rows: a one-pixel border and paper around the items. An
// item touching a side of the extent reaches across the paper on that side
// up to the border, so moving the pointer sideways through the paper keeps
// the item (and its submenu) highlighted.
constexpr i32 kBoxPadX = 3;
constexpr i32 kBoxPadY = 2;

// Titles start this many columns in from the bar's left end, so the first
// one and its drop-down box keep clear of the screen edge.
constexpr i32 kBarLeft = 4;

// The rectangle of a title in the bar, which spans the bar's height.
inline Rect title_rect(const Title& title, i32 bar_height) {
    const i32 left = kBarLeft + title.left;
    return Rect{left, 0, left + title.width - 1, bar_height - 1};
}

// The drop-down box of an open title, at the title's left edge just below
// the bar, large enough for its items and never narrower than the title.
// With a screen width it is moved left as far as needed to stay on screen.
inline Rect box_rect(i32 left, i32 bar_height, const Item* items, u32 count,
                     i32 min_width = 0, i32 screen_width = 0) {
    if (count == 0) {
        return empty_rect();
    }
    const detail::Extent e = detail::extent(items, count);
    if (e.right <= e.left || e.bottom <= e.top) {
        return empty_rect();
    }
    const i32 framed = e.right - e.left + 2 * kBoxPadX;
    const i32 width = framed > min_width ? framed : min_width;
    if (screen_width > 0 && left + width > screen_width) {
        left = screen_width - width > 0 ? screen_width - width : 0;
    }
    return Rect{left, bar_height, left + width - 1,
                bar_height + (e.bottom - e.top) + 2 * kBoxPadY - 1};
}

// The item rectangles inside box. Item positions are relative to the
// top-left corner of the box's content, inside its frame.
inline void item_rects(const Rect& box, const Item* items, u32 count, Rect* out) {
    if (count == 0 || !valid(box)) {
        return;
    }
    const detail::Extent e = detail::extent(items, count);
    for (u32 i = 0; i < count; ++i) {
        const i32 x0 = box.x0 + kBoxPadX + items[i].left - e.left;
        const i32 y0 = box.y0 + kBoxPadY + items[i].top - e.top;
        Rect r{x0, y0, x0 + items[i].width - 1, y0 + items[i].height - 1};
        if (items[i].left == e.left) {
            r.x0 = box.x0 + 1;
        }
        if (items[i].left + items[i].width == e.right) {
            r.x1 = box.x1 - 1;
        }
        out[i] = r;
    }
}

// The box that holds the subitems of the highlighted item: beside the
// drop-down box, sharing its right border, with its first row level with
// the item. With a screen width it opens on the left of the drop-down box
// when there is no room on the right.
inline Rect submenu_rect(const Rect& parent_box, const Rect& parent_item, const Item* items,
                         u32 count, i32 screen_width = 0) {
    if (count == 0 || !valid(parent_box) || !valid(parent_item)) {
        return empty_rect();
    }
    const detail::Extent e = detail::extent(items, count);
    if (e.right <= e.left || e.bottom <= e.top) {
        return empty_rect();
    }
    const i32 width = e.right - e.left + 2 * kBoxPadX;
    const i32 height = e.bottom - e.top + 2 * kBoxPadY;
    i32 x0 = parent_box.x1;
    if (screen_width > 0 && x0 + width > screen_width && parent_box.x0 - width + 1 >= 0) {
        x0 = parent_box.x0 - width + 1;
    }
    const i32 y0 = parent_item.y0 - kBoxPadY;
    return Rect{x0, y0, x0 + width - 1, y0 + height - 1};
}

inline Geometry compute(const Title* titles, u32 title_count, const Item* items, u32 item_count,
                        const Item* sub_items, u32 sub_item_count, i32 bar_height,
                        u32 open_title, i32 open_item, i32 screen_width = 0) {
    Geometry g;
    g.title_count = title_count < kMaxTitles ? title_count : kMaxTitles;
    for (u32 i = 0; i < g.title_count; ++i) {
        g.title[i] = title_rect(titles[i], bar_height);
    }
    if (open_title >= g.title_count || item_count == 0) {
        return g;
    }
    g.item_count = item_count < kMaxItems ? item_count : kMaxItems;
    g.box = box_rect(g.title[open_title].x0, bar_height, items, g.item_count,
                     titles[open_title].width, screen_width);
    item_rects(g.box, items, g.item_count, g.item);
    if (open_item < 0 || static_cast<u32>(open_item) >= g.item_count ||
        sub_item_count == 0 || items[open_item].sub == 0) {
        return g;
    }
    g.sub_item_count = sub_item_count < kMaxSubItems ? sub_item_count : kMaxSubItems;
    g.sub_box = submenu_rect(g.box, g.item[open_item], sub_items, g.sub_item_count,
                             screen_width);
    item_rects(g.sub_box, sub_items, g.sub_item_count, g.sub_item);
    return g;
}

inline i32 hit(const Rect* rects, u32 count, i32 x, i32 y) {
    for (u32 i = 0; i < count; ++i) {
        const Rect& r = rects[i];
        if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

}
