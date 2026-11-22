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

#include "types.hpp"

namespace hombre {

using i8 = __INT8_TYPE__;
using i32 = __INT32_TYPE__;

inline constexpr u8 kMouseLeft = 1;
inline constexpr u8 kMouseRight = 2;
inline constexpr u8 kMouseMiddle = 4;

struct MouseSample {
    u8 buttons = 0;
    i32 dx = 0;
    i32 dy = 0;
    i8 wheel = 0;
};

// Decodes a USB HID mouse report. Byte 0 contains the button bitmask (left,
// right, middle in bits 0..2; bits 3..7 are ignored). Bytes 1 and 2 provide
// signed 8-bit X and Y relative displacements. Byte 3, when present, provides
// a signed 8-bit wheel delta (positive away from the user); any bytes beyond
// the fourth are ignored.
inline bool decode_mouse_report(const u8* report, usize length, MouseSample& out) {
    if (report == nullptr || length < 3u) {
        return false;
    }
    out.buttons = static_cast<u8>(report[0] & 7u);
    out.dx = static_cast<i32>(static_cast<i8>(report[1]));
    out.dy = static_cast<i32>(static_cast<i8>(report[2]));
    out.wheel = (length >= 4u) ? static_cast<i8>(report[3]) : static_cast<i8>(0);
    return true;
}

// The grid the pointer moves on: the picture box of the display, whatever
// screen mode is in front. A High Res Laced screen of 640x400 pixels is two
// units per pixel, so a USB count moves it one pixel.
inline constexpr u32 kPointerGridWidth = 1280;
inline constexpr u32 kPointerGridHeight = 800;
inline constexpr i32 kPointerUnitsPerCount = 2;

// Tracks pointer position and button state within a bounded area. The area
// is the pointer grid when the pointer drives a display; a screen of any size
// then reads the position through screen_x and screen_y.
class PointerState {
public:
    struct Update {
        bool moved;
        u8 pressed;
        u8 released;
        i32 wheel;
    };

    PointerState() = default;
    PointerState(u32 width, u32 height, i32 x = 0, i32 y = 0);

    void init(u32 width, u32 height, i32 x, i32 y);
    Update apply(const MouseSample& sample);

    i32 x() const { return x_; }
    i32 y() const { return y_; }
    u8 buttons() const { return buttons_; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    // The position in the pixels of a screen screen_width x screen_height
    // that fills the area.
    i32 screen_x(u32 screen_width) const { return scaled(x_, screen_width, width_); }
    i32 screen_y(u32 screen_height) const { return scaled(y_, screen_height, height_); }

    void set_position(i32 x, i32 y);

private:
    static i32 scaled(i32 position, u32 screen_side, u32 area_side) {
        const i64 scaled_position = static_cast<i64>(position) * screen_side;
        return area_side == 0 ? 0 : static_cast<i32>(scaled_position / area_side);
    }

    i32 clamp_x(i64 v) const;
    i32 clamp_y(i64 v) const;

    u32 width_ = 0;
    u32 height_ = 0;
    i32 x_ = 0;
    i32 y_ = 0;
    u8 buttons_ = 0;
};

inline PointerState::PointerState(u32 width, u32 height, i32 x, i32 y) {
    init(width, height, x, y);
}

inline void PointerState::init(u32 width, u32 height, i32 x, i32 y) {
    width_ = width;
    height_ = height;
    buttons_ = 0;
    x_ = clamp_x(x);
    y_ = clamp_y(y);
}

inline PointerState::Update PointerState::apply(const MouseSample& sample) {
    const i32 new_x = clamp_x(static_cast<i64>(x_) + sample.dx * kPointerUnitsPerCount);
    const i32 new_y = clamp_y(static_cast<i64>(y_) + sample.dy * kPointerUnitsPerCount);
    const bool moved = (new_x != x_) || (new_y != y_);
    x_ = new_x;
    y_ = new_y;

    const u8 prev = buttons_;
    const u8 cur = sample.buttons;
    const u8 pressed = static_cast<u8>(cur & ~prev);
    const u8 released = static_cast<u8>(prev & ~cur);
    buttons_ = cur;

    return Update{moved, pressed, released, static_cast<i32>(sample.wheel)};
}

inline void PointerState::set_position(i32 x, i32 y) {
    x_ = clamp_x(x);
    y_ = clamp_y(y);
}

inline i32 PointerState::clamp_x(i64 v) const {
    if (width_ == 0 || height_ == 0 || v < 0) {
        return 0;
    }
    const i64 limit_x = (width_ > 0x7FFFFFFFu) ? 0x7FFFFFFFll : static_cast<i64>(width_ - 1);
    if (v > limit_x) {
        return static_cast<i32>(limit_x);
    }
    return static_cast<i32>(v);
}

inline i32 PointerState::clamp_y(i64 v) const {
    if (width_ == 0 || height_ == 0 || v < 0) {
        return 0;
    }
    const i64 limit_y = (height_ > 0x7FFFFFFFu) ? 0x7FFFFFFFll : static_cast<i64>(height_ - 1);
    if (v > limit_y) {
        return static_cast<i32>(limit_y);
    }
    return static_cast<i32>(v);
}

}
