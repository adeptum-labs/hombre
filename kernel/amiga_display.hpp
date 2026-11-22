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

namespace detail {

struct BitSpreadTable {
    u64 entries[256];
};

constexpr BitSpreadTable make_bit_spread_table() {
    BitSpreadTable t{};
    for (u32 v = 0; v < 256; ++v) {
        u64 out = 0;
        for (u32 k = 0; k < 8; ++k) {
            if (((v >> (7 - k)) & 1u) != 0) {
                out |= 1ULL << (k * 8);
            }
        }
        t.entries[v] = out;
    }
    return t;
}

inline constexpr BitSpreadTable kBitSpread = make_bit_spread_table();

}

// Draws the front screen's bitplanes into the framebuffer. The nominal area
// of the screen's display mode fills the box, the largest 16:10 rectangle the
// framebuffer holds, centred. Each host pixel shows the screen pixel it falls
// in, so a mode that does not divide the box evenly has some pixels one host
// pixel wider than others. The rest of the framebuffer, and the part of the
// box a smaller screen does not cover, is in the border colour.
class AmigaDisplay {
public:
    constexpr AmigaDisplay() = default;

    // Fits the box to the framebuffer and clears the framebuffer to the
    // border colour.
    void init(Framebuffer fb) {
        fb_ = fb;
        const u64 width = fb_.width();
        const u64 height = fb_.height();
        u64 box_width = width;
        u64 box_height = width * kBoxHeightUnits / kBoxWidthUnits;
        if (box_height > height) {
            box_height = height;
            box_width = height * kBoxWidthUnits / kBoxHeightUnits;
        }
        if (box_width > kMaxBoxWidth) {
            box_width = kMaxBoxWidth;
            box_height = kMaxBoxHeight;
        }
        box_width_ = static_cast<u32>(box_width);
        box_height_ = static_cast<u32>(box_height);
        box_x_ = centred(width, box_width);
        box_y_ = centred(height, box_height);
        pointer_pixel_width_ = at_least_one(box_width_ / kPointerPixelsAcross);
        pointer_pixel_height_ = at_least_one(box_height_ / kPointerPixelsDown);
        applied_screen_ = 0;
        applied_width_ = kDefaultModeWidth;
        applied_height_ = kDefaultModeHeight;
        map_mode();
        fb_.fill(kBorder);
    }

    DisplaySink sink() { return DisplaySink{&AmigaDisplay::refresh, this}; }

    // Draws the pointer in the shape state publishes, without it the arrow,
    // and fits the mode state publishes for the front screen to the box.
    void follow_pointer_shape(const DisplayState& state) { state_ = &state; }

    // Draws the pixels of dirty, clipped to the part of the screen inside its
    // mode's nominal area. A change of screen or mode first clears the box to
    // the border colour, so the caller redraws the whole screen then. Screen 0
    // clears the framebuffer to the border colour, and a screen larger than
    // 4096 pixels on a side is ignored.
    static void refresh(void* ctx, m68k::Machine& m, u32 screen, const Rect& dirty) {
        AmigaDisplay& display = *static_cast<AmigaDisplay*>(ctx);
        if (screen == 0) {
            display.fb_.fill(kBorder);
            display.applied_screen_ = 0;
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
        const u32 used = depth < kMaxPlanes ? depth : kMaxPlanes;
        u32 planes[kMaxPlanes]{};
        for (u32 plane = 0; plane < used; ++plane) {
            planes[plane] = b.r32(bitmap + ndk::bm_Planes + plane * 4);
        }
        const u32 colour_map = b.r32(screen + ndk::sc_ViewPort + ndk::vp_ColorMap);
        const u8 cm_type = static_cast<u8>(b.r8(colour_map + ndk::cm_Type));
        const u32 cm_count = b.r16(colour_map + ndk::cm_Count);
        const u32 table = b.r32(colour_map + ndk::cm_ColorTable);
        const u32 low_table =
            cm_type >= ndk::COLORMAP_TYPE_V39 ? b.r32(colour_map + ndk::cm_LowColorBits) : 0;
        const u32 palette_count = cm_count < kMaxColours ? cm_count : kMaxColours;
        u32 palette[kMaxColours]{};
        if (low_table != 0) {
            for (u32 pen = 0; pen < palette_count; ++pen) {
                palette[pen] = colour24(b.r16(table + pen * 2), b.r16(low_table + pen * 2));
            }
        } else {
            for (u32 pen = 0; pen < palette_count; ++pen) {
                palette[pen] = colour(b.r16(table + pen * 2));
            }
        }
        if (!b.ok()) {
            return;
        }
        display.adopt_front(screen);
        // TODO: scroll a screen larger than its mode's nominal area (autoscroll).
        const u32 shown_width = width < display.applied_width_ ? width : display.applied_width_;
        const u32 shown_height =
            height < display.applied_height_ ? height : display.applied_height_;
        const i32 last_x = at_most(dirty.x1, shown_width);
        const i32 last_y = at_most(dirty.y1, shown_height);
        const i32 first_x = dirty.x0 > 0 ? dirty.x0 : 0;
        const i32 first_y = dirty.y0 > 0 ? dirty.y0 : 0;
        if (first_x > last_x || first_y > last_y) {
            return;
        }
        const u32 host_left = display.host_column(first_x);
        const u32 host_right = display.host_column(last_x + 1);
        const u32 host_top = display.host_row(first_y);
        const u32 host_bottom = display.host_row(last_y + 1);
        if (host_left == host_right || host_top == host_bottom) {
            return;
        }
        const u32 count = static_cast<u32>(last_x - first_x + 1);
        const u32 first_bx = static_cast<u32>(first_x) >> 3;
        const u32 last_bx = (static_cast<u32>(first_x) + count - 1) >> 3;
        const u32 bx_count = last_bx - first_bx + 1;
        u32 row[kMaxModeWidth];
        for (i32 y = first_y; y <= last_y; ++y) {
            const u32 rows_top = display.host_row(y);
            const u32 rows_bottom = display.host_row(y + 1);
            if (rows_top == rows_bottom) {
                continue;
            }
            const u32 row_offset = static_cast<u32>(y) * bytes_per_row;
            const u8* plane_ptrs[kMaxPlanes]{};
            for (u32 plane = 0; plane < used; ++plane) {
                plane_ptrs[plane] = b.bytes_at(planes[plane] + row_offset + first_bx, bx_count);
            }
            if (!b.ok()) {
                return;
            }
            chunky_row(row, plane_ptrs, used, palette, static_cast<u32>(first_x), count);
            display.fb_.put_mapped_row(display.box_x_ + host_left, display.box_y_ + rows_top,
                                       rows_bottom - rows_top, row, display.column_x_ + host_left,
                                       host_right - host_left);
        }
        if (display.pointer_visible_) {
            display.draw_pointer(display.box_x_ + host_left, display.box_y_ + host_top,
                                 display.box_x_ + host_right, display.box_y_ + host_bottom);
        }
    }

    void set_pointer_visible(m68k::Machine& m, u32 screen, bool visible) {
        pointer_visible_ = visible;
        if (screen == 0) {
            return;
        }
        refresh(this, m, screen, pointer_dirty(pointer_x_, pointer_y_, pointer_x_, pointer_y_));
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
        refresh(this, m, screen, pointer_dirty(old_x, old_y, x, y));
    }

    u32 box_x() const { return box_x_; }
    u32 box_y() const { return box_y_; }
    u32 box_width() const { return box_width_; }
    u32 box_height() const { return box_height_; }

private:
    static constexpr u32 kBoxWidthUnits = 16;
    static constexpr u32 kBoxHeightUnits = 10;
    // The tables below bound the box; framebuffers beyond it get a box of this size.
    static constexpr u32 kMaxBoxWidth = 4096;
    static constexpr u32 kMaxBoxHeight = kMaxBoxWidth * kBoxHeightUnits / kBoxWidthUnits;
    // The widest nominal mode size, which the row buffer holds.
    static constexpr u32 kMaxModeWidth = 1280;
    static constexpr u32 kDefaultModeWidth = 640;
    static constexpr u32 kDefaultModeHeight = 400;
    // The pointer pixel is a High Res Laced pixel of the box.
    static constexpr u32 kPointerPixelsAcross = 640;
    static constexpr u32 kPointerPixelsDown = 400;
    static constexpr u32 kMaxSide = 4096;
    static constexpr u32 kMaxPlanes = 8;
    static constexpr u32 kMaxColours = 1u << kMaxPlanes;
    static constexpr u32 kBorder = 0xAAAAAA;

    static u32 at_least_one(u32 value) { return value > 0 ? value : 1; }

    // The nominal size of the front screen's mode as state publishes it, bounded
    // so that it fits the row buffer and the tables.
    u32 mode_width() const {
        const u32 width = state_ != nullptr ? state_->mode_width : kDefaultModeWidth;
        return at_least_one(width < kMaxModeWidth ? width : kMaxModeWidth);
    }

    u32 mode_height() const {
        const u32 height = state_ != nullptr ? state_->mode_height : kDefaultModeHeight;
        return at_least_one(height < kMaxSide ? height : kMaxSide);
    }

    // Makes the box show screen in the mode of the state. A change of either
    // clears the box to the border colour, and the pointer keeps its place on
    // the monitor, not its screen pixel.
    void adopt_front(u32 screen) {
        const u32 width = mode_width();
        const u32 height = mode_height();
        if (screen == applied_screen_ && width == applied_width_ && height == applied_height_) {
            return;
        }
        pointer_x_ = static_cast<i32>(static_cast<i64>(pointer_x_) * width / applied_width_);
        pointer_y_ = static_cast<i32>(static_cast<i64>(pointer_y_) * height / applied_height_);
        applied_screen_ = screen;
        applied_width_ = width;
        applied_height_ = height;
        map_mode();
        fb_.fill_rect(box_x_, box_y_, box_width_, box_height_, kBorder);
    }

    // Looks up, for every host column and row of the box, the screen pixel it
    // shows in the applied mode.
    void map_mode() {
        for (u32 column = 0; column < box_width_; ++column) {
            column_x_[column] = static_cast<u16>(column * applied_width_ / box_width_);
        }
        for (u32 row = 0; row < box_height_; ++row) {
            row_y_[row] = static_cast<u16>(row * applied_height_ / box_height_);
        }
    }

    // The first host column of the box that shows screen column x, or the
    // column after the last when x is the screen width. A column no host
    // column shows has the same value as the next, so that its range is empty.
    u32 host_column(i32 x) const {
        return (static_cast<u32>(x) * box_width_ + applied_width_ - 1) / applied_width_;
    }

    u32 host_row(i32 y) const {
        return (static_cast<u32>(y) * box_height_ + applied_height_ - 1) / applied_height_;
    }

    // The position of a screen pixel as host pixels from the top left of the box.
    static i64 host_offset(i32 screen_pixel, u32 box_side, u32 mode_side) {
        return static_cast<i64>(screen_pixel) * box_side / mode_side;
    }

    // Converts count pixels of one screen row, starting at x, into row[x...],
    // from the bytes of each plane that hold them.
    static void chunky_row(u32* row, const u8* const* plane_ptrs, u32 used, const u32* palette,
                           u32 x, u32 count) {
        u32 remaining = count;
        for (u32 bi = 0; remaining > 0; ++bi) {
            u64 packed = 0;
            for (u32 plane = 0; plane < used; ++plane) {
                packed |= detail::kBitSpread.entries[plane_ptrs[plane][bi]] << plane;
            }
            const u32 bit_off = x & 7;
            const u32 avail = 8 - bit_off;
            const u32 chunk = remaining < avail ? remaining : avail;
            if (chunk == 8) {
                row[x + 0] = palette[packed & 0xFF];
                row[x + 1] = palette[(packed >> 8) & 0xFF];
                row[x + 2] = palette[(packed >> 16) & 0xFF];
                row[x + 3] = palette[(packed >> 24) & 0xFF];
                row[x + 4] = palette[(packed >> 32) & 0xFF];
                row[x + 5] = palette[(packed >> 40) & 0xFF];
                row[x + 6] = palette[(packed >> 48) & 0xFF];
                row[x + 7] = palette[(packed >> 56) & 0xFF];
            } else {
                u64 p = packed >> (bit_off * 8);
                for (u32 k = 0; k < chunk; ++k) {
                    row[x + k] = palette[p & 0xFF];
                    p >>= 8;
                }
            }
            x += chunk;
            remaining -= chunk;
        }
    }

    // The shape the pointer is drawn in: the one the state publishes, else the arrow.
    const PointerShape& pointer_shape() const {
        return state_ != nullptr && state_->pointer != nullptr ? *state_->pointer
                                                               : kDefaultPointerShape;
    }

    // The screen pixels under the pointer rectangles at two positions, empty
    // when the rectangles lie outside the box.
    Rect pointer_dirty(i32 x0, i32 y0, i32 x1, i32 y1) const {
        const PointerShape& shape = pointer_shape();
        const i64 hot_x = static_cast<i64>(shape.hot_x) * pointer_pixel_width_;
        const i64 hot_y = static_cast<i64>(shape.hot_y) * pointer_pixel_height_;
        const i64 image_width = static_cast<i64>(shape.width) * pointer_pixel_width_;
        const i64 image_height = static_cast<i64>(shape.height) * pointer_pixel_height_;
        const i64 left_a = host_offset(x0, box_width_, applied_width_);
        const i64 left_b = host_offset(x1, box_width_, applied_width_);
        const i64 top_a = host_offset(y0, box_height_, applied_height_);
        const i64 top_b = host_offset(y1, box_height_, applied_height_);
        const i64 left = (left_a < left_b ? left_a : left_b) - hot_x;
        const i64 top = (top_a < top_b ? top_a : top_b) - hot_y;
        const i64 right = (left_a > left_b ? left_a : left_b) - hot_x + image_width - 1;
        const i64 bottom = (top_a > top_b ? top_a : top_b) - hot_y + image_height - 1;
        const i64 first_column = left > 0 ? left : 0;
        const i64 first_row = top > 0 ? top : 0;
        const i64 box_right = static_cast<i64>(box_width_) - 1;
        const i64 box_bottom = static_cast<i64>(box_height_) - 1;
        const i64 last_column = right < box_right ? right : box_right;
        const i64 last_row = bottom < box_bottom ? bottom : box_bottom;
        if (first_column > last_column || first_row > last_row) {
            return Rect{0, 0, -1, -1};
        }
        return Rect{column_x_[first_column], row_y_[first_row], column_x_[last_column],
                    row_y_[last_row]};
    }

    // Draws the pointer, each of its pixels a block of the pointer pixel size,
    // where it lies between (left, top) and (right, bottom) of the framebuffer,
    // the end excluded. The hot spot is at the host position of the pointer's
    // screen position.
    void draw_pointer(i64 left, i64 top, i64 right, i64 bottom) {
        const PointerShape& shape = pointer_shape();
        const i64 image_left = box_x_ +
                               host_offset(pointer_x_, box_width_, applied_width_) -
                               static_cast<i64>(shape.hot_x) * pointer_pixel_width_;
        const i64 image_top = box_y_ +
                              host_offset(pointer_y_, box_height_, applied_height_) -
                              static_cast<i64>(shape.hot_y) * pointer_pixel_height_;
        for (u32 py = 0; py < shape.height; ++py) {
            const i64 block_top = image_top + static_cast<i64>(py) * pointer_pixel_height_;
            const i64 y0 = block_top > top ? block_top : top;
            const i64 y1 = block_top + pointer_pixel_height_ < bottom
                               ? block_top + pointer_pixel_height_
                               : bottom;
            for (u32 px = 0; px < shape.width && y0 < y1; ++px) {
                const u32 pixel = shape.pixels[py][px];
                const i64 block_left = image_left + static_cast<i64>(px) * pointer_pixel_width_;
                const i64 x0 = block_left > left ? block_left : left;
                const i64 x1 = block_left + pointer_pixel_width_ < right
                                   ? block_left + pointer_pixel_width_
                                   : right;
                if (pixel != 0 && x0 < x1) {
                    fb_.fill_rect(static_cast<u64>(x0), static_cast<u64>(y0),
                                  static_cast<u64>(x1 - x0), static_cast<u64>(y1 - y0),
                                  shape.rgb[pixel & 3]);
                }
            }
        }
    }

    // The offset of a box that fits wholly in the framebuffer, 0 when it
    // does not.
    static u32 centred(u64 framebuffer_side, u64 screen_side) {
        return screen_side < framebuffer_side
                   ? static_cast<u32>((framebuffer_side - screen_side) / 2)
                   : 0;
    }

    static i32 at_most(i32 value, u32 side) {
        const i32 maximum = static_cast<i32>(side) - 1;
        return value < maximum ? value : maximum;
    }

    // The 0xRRGGBB colour of a colour table word, a nibble times 17.
    static constexpr u32 colour(u32 rgb) {
        const u32 red = ((rgb >> 8) & 0xF) * 0x11;
        const u32 green = ((rgb >> 4) & 0xF) * 0x11;
        const u32 blue = (rgb & 0xF) * 0x11;
        return (red << 16) | (green << 8) | blue;
    }

    // The 0xRRGGBB colour combining high nibbles (ColorTable) and low nibbles
    // (LowColorBits).
    static constexpr u32 colour24(u32 hi, u32 lo) {
        const u32 red = (((hi >> 8) & 0xF) << 4) | ((lo >> 8) & 0xF);
        const u32 green = (((hi >> 4) & 0xF) << 4) | ((lo >> 4) & 0xF);
        const u32 blue = ((hi & 0xF) << 4) | (lo & 0xF);
        return (red << 16) | (green << 8) | blue;
    }

    Framebuffer fb_{};
    u32 box_x_ = 0;
    u32 box_y_ = 0;
    u32 box_width_ = 0;
    u32 box_height_ = 0;
    u32 pointer_pixel_width_ = 1;
    u32 pointer_pixel_height_ = 1;
    // What the box shows: the front screen and its mode's nominal size, and
    // the screen pixel under each host column and row of the box.
    u32 applied_screen_ = 0;
    u32 applied_width_ = kDefaultModeWidth;
    u32 applied_height_ = kDefaultModeHeight;
    u16 column_x_[kMaxBoxWidth]{};
    u16 row_y_[kMaxBoxHeight]{};
    // The pointer position in pixels of the applied mode.
    i32 pointer_x_ = 0;
    i32 pointer_y_ = 0;
    bool pointer_visible_ = false;
    const DisplayState* state_ = nullptr;
};

}
