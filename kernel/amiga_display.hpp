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

#include "../common/libs/gfx.hpp"
#include "../common/libs/native.hpp"
#include "../common/pointer_image.hpp"
#include "../common/types.hpp"
#include "framebuffer.hpp"

namespace hombre {

// Draws the front screen's bitplanes into the framebuffer: the 640x400
// logical screen at the largest whole scale that fits, centred, and the rest
// of the framebuffer in the border colour.
class AmigaDisplay {
public:
    constexpr AmigaDisplay() = default;

    // Sizes the screen for the framebuffer and clears it to the border
    // colour.
    void init(Framebuffer fb) {
        fb_ = fb;
        const u64 width = fb_.width();
        const u64 height = fb_.height();
        const u64 fit_x = width / kScreenWidth;
        const u64 fit_y = height / kScreenHeight;
        const u64 fit = fit_x < fit_y ? fit_x : fit_y;
        scale_ = static_cast<u32>(fit > 0 ? fit : 1);
        origin_x_ = centred(width, kScreenWidth * scale_);
        origin_y_ = centred(height, kScreenHeight * scale_);
        fb_.fill(kBorder);
    }

    DisplaySink sink() { return DisplaySink{&AmigaDisplay::refresh, this}; }

    // Draws the pixels of dirty, clipped to the screen, as scale x scale
    // blocks. Screen 0 clears the framebuffer to the border colour, and a
    // screen larger than 4096 pixels on a side is ignored.
    static void refresh(void* ctx, m68k::Machine& m, u32 screen, const Rect& dirty) {
        AmigaDisplay& display = *static_cast<AmigaDisplay*>(ctx);
        if (screen == 0) {
            display.fb_.fill(kBorder);
            return;
        }
        native::Bus b(m);
        const u32 width = b.r16(screen + ndk::sc_Width);
        const u32 height = b.r16(screen + ndk::sc_Height);
        if (!b.ok() || width > kMaxSide || height > kMaxSide) {
            return;
        }
        const u32 bitmap = screen + ndk::sc_BitMap;
        const u32 bytes_per_row = b.r16(bitmap + ndk::bm_BytesPerRow);
        const u32 depth = b.r8(bitmap + ndk::bm_Depth);
        u32 planes[kPlanes];
        for (u32 plane = 0; plane < kPlanes; ++plane) {
            planes[plane] = b.r32(bitmap + ndk::bm_Planes + plane * 4);
        }
        const u32 colour_map = b.r32(screen + ndk::sc_ViewPort + ndk::vp_ColorMap);
        const u32 table = b.r32(colour_map + ndk::cm_ColorTable);
        u32 palette[kColours];
        for (u32 pen = 0; pen < kColours; ++pen) {
            palette[pen] = colour(b.r16(table + pen * 2));
        }
        if (!b.ok()) {
            return;
        }
        const i32 last_x = at_most(dirty.x1, width);
        const i32 last_y = at_most(dirty.y1, height);
        const i32 first_x = dirty.x0 > 0 ? dirty.x0 : 0;
        const i32 first_y = dirty.y0 > 0 ? dirty.y0 : 0;
        for (i32 y = first_y; y <= last_y; ++y) {
            for (i32 x = first_x; x <= last_x; ++x) {
                const u32 pen = pen_at(b, planes, depth, bytes_per_row, static_cast<u32>(x),
                                       static_cast<u32>(y));
                if (!b.ok()) {
                    return;
                }
                const u64 left = display.origin_x_ + static_cast<u64>(x) * display.scale_;
                const u64 top = display.origin_y_ + static_cast<u64>(y) * display.scale_;
                display.fb_.fill_rect(left, top, display.scale_, display.scale_, palette[pen]);
            }
        }
        if (display.pointer_visible_) {
            const i32 ptr_first_x = display.pointer_x_ > first_x ? display.pointer_x_ : first_x;
            const i32 ptr_first_y = display.pointer_y_ > first_y ? display.pointer_y_ : first_y;
            const i32 ptr_end_x = display.pointer_x_ + static_cast<i32>(kPointerWidth) - 1;
            const i32 ptr_end_y = display.pointer_y_ + static_cast<i32>(kPointerHeight) - 1;
            const i32 ptr_last_x = ptr_end_x < last_x ? ptr_end_x : last_x;
            const i32 ptr_last_y = ptr_end_y < last_y ? ptr_end_y : last_y;
            if (ptr_first_x <= ptr_last_x && ptr_first_y <= ptr_last_y) {
                for (i32 y = ptr_first_y; y <= ptr_last_y; ++y) {
                    const u32 py = static_cast<u32>(y - display.pointer_y_);
                    for (i32 x = ptr_first_x; x <= ptr_last_x; ++x) {
                        const u32 px = static_cast<u32>(x - display.pointer_x_);
                        const u32 pixel = pointer_pixel(px, py);
                        if (pixel == 0) {
                            continue;
                        }
                        const u32 colour = pixel == 1 ? kPointerOutlineColour : kPointerFillColour;
                        const u64 left = display.origin_x_ + static_cast<u64>(x) * display.scale_;
                        const u64 top = display.origin_y_ + static_cast<u64>(y) * display.scale_;
                        display.fb_.fill_rect(left, top, display.scale_, display.scale_, colour);
                    }
                }
            }
        }
    }

    void set_pointer_visible(m68k::Machine& m, u32 screen, bool visible) {
        pointer_visible_ = visible;
        if (screen == 0) {
            return;
        }
        native::Bus b(m);
        const u32 width = b.r16(screen + ndk::sc_Width);
        const u32 height = b.r16(screen + ndk::sc_Height);
        if (!b.ok() || width > kMaxSide || height > kMaxSide) {
            return;
        }
        const Rect dirty =
            pointer_union(pointer_x_, pointer_y_, pointer_x_, pointer_y_, width, height);
        refresh(this, m, screen, dirty);
    }

    void set_pointer_visible(bool visible) { pointer_visible_ = visible; }

    bool pointer_visible() const { return pointer_visible_; }
    i32 pointer_x() const { return pointer_x_; }
    i32 pointer_y() const { return pointer_y_; }

    void move_pointer(m68k::Machine& m, u32 screen, i32 x, i32 y) {
        const i32 old_x = pointer_x_;
        const i32 old_y = pointer_y_;
        pointer_x_ = x;
        pointer_y_ = y;
        if (!pointer_visible_ || screen == 0) {
            return;
        }
        native::Bus b(m);
        const u32 width = b.r16(screen + ndk::sc_Width);
        const u32 height = b.r16(screen + ndk::sc_Height);
        if (!b.ok() || width > kMaxSide || height > kMaxSide) {
            return;
        }
        const Rect dirty = pointer_union(old_x, old_y, x, y, width, height);
        refresh(this, m, screen, dirty);
    }

    u32 scale() const { return scale_; }
    u32 origin_x() const { return origin_x_; }
    u32 origin_y() const { return origin_y_; }

private:
    static constexpr u32 kScreenWidth = 640;
    static constexpr u32 kScreenHeight = 400;
    static constexpr u32 kMaxSide = 4096;
    // The colour table holds four entries, so the first two planes choose a
    // pen and the rest of a deeper bitmap is not drawn.
    static constexpr u32 kPlanes = 2;
    static constexpr u32 kColours = 1 << kPlanes;
    static constexpr u32 kBorder = 0xAAAAAA;
    static constexpr u32 kPointerOutlineColour = 0x000000;
    static constexpr u32 kPointerFillColour = 0xFFFFFF;

    // The union of two pointer rectangles clipped to the screen.
    static Rect pointer_union(i32 x0, i32 y0, i32 x1, i32 y1, u32 width, u32 height) {
        const i32 min_x = x0 < x1 ? x0 : x1;
        const i32 min_y = y0 < y1 ? y0 : y1;
        const i32 max_x = (x0 > x1 ? x0 : x1) + static_cast<i32>(kPointerWidth) - 1;
        const i32 max_y = (y0 > y1 ? y0 : y1) + static_cast<i32>(kPointerHeight) - 1;
        const i32 first_x = min_x > 0 ? min_x : 0;
        const i32 first_y = min_y > 0 ? min_y : 0;
        const i32 last_x = at_most(max_x, width);
        const i32 last_y = at_most(max_y, height);
        return Rect{first_x, first_y, last_x, last_y};
    }

    // The offset of a screen that fits wholly in the framebuffer, 0 when it
    // does not and is clipped at the top left.
    static u32 centred(u64 framebuffer_side, u64 screen_side) {
        return screen_side < framebuffer_side
                   ? static_cast<u32>((framebuffer_side - screen_side) / 2)
                   : 0;
    }

    static i32 at_most(i32 value, u32 side) {
        const i32 maximum = static_cast<i32>(side) - 1;
        return value < maximum ? value : maximum;
    }

    // The pen of screen pixel (x, y): one bit from each plane, the first
    // plane the lowest.
    static u32 pen_at(native::Bus& b, const u32 (&planes)[kPlanes], u32 depth, u32 bytes_per_row,
                      u32 x, u32 y) {
        const u32 mask = 0x80 >> (x & 7);
        const u32 at = y * bytes_per_row + (x >> 3);
        const u32 used = depth < kPlanes ? depth : kPlanes;
        u32 pen = 0;
        for (u32 plane = 0; plane < used && b.ok(); ++plane) {
            if ((b.r8(planes[plane] + at) & mask) != 0) {
                pen |= 1u << plane;
            }
        }
        return pen;
    }

    // The 0xRRGGBB colour of a colour table word, a nibble times 17.
    static u32 colour(u32 rgb) {
        const u32 red = ((rgb >> 8) & 0xF) * 17;
        const u32 green = ((rgb >> 4) & 0xF) * 17;
        const u32 blue = (rgb & 0xF) * 17;
        return (red << 16) | (green << 8) | blue;
    }

    Framebuffer fb_{};
    u32 scale_ = 1;
    u32 origin_x_ = 0;
    u32 origin_y_ = 0;
    i32 pointer_x_ = 0;
    i32 pointer_y_ = 0;
    bool pointer_visible_ = false;
};

}
