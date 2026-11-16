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

#include "../common/boot_info.hpp"
#include "../common/types.hpp"
#include "font.hpp"

namespace hombre {

// Draws into a linear 32-bit framebuffer. Every access is bounds-checked
// against the width, height and pitch given at construction, so drawing
// outside the screen is clipped and never touches memory outside the buffer.
class Framebuffer {
public:
    // format is kFramebufferRgbx8 or, for any other value, kFramebufferBgrx8.
    // A null base, an empty screen or a pitch shorter than a row of pixels
    // gives a framebuffer that ignores all drawing.
    Framebuffer(u8* base, u32 width, u32 height, u32 pitch_bytes, u32 format)
        : base_(base),
          width_(width),
          height_(height),
          pitch_(pitch_bytes),
          rgbx_(format == kFramebufferRgbx8) {
        if (base == nullptr || static_cast<u64>(width) * 4 > pitch_bytes) {
            width_ = 0;
            height_ = 0;
        }
    }

    // rgb is 0xRRGGBB.
    void fill(u32 rgb) { fill_rect(0, 0, width_, height_, rgb); }

    // Draws text with its top left corner at (x, y), 8 * scale pixels per
    // character. Characters outside printable ASCII are drawn as '?'.
    void draw_text(u32 x, u32 y, const char* text, u32 rgb, u32 scale) {
        if (text == nullptr || scale == 0 || x >= width_ || y >= height_) {
            return;
        }
        const u64 step = static_cast<u64>(scale) * 8;
        u64 left = x;
        for (; *text != '\0' && left < width_; ++text, left += step) {
            const u8* rows = glyph(*text);
            for (u32 row = 0; row < 8; ++row) {
                const u64 top = y + static_cast<u64>(row) * scale;
                if (top >= height_) {
                    break;
                }
                for (u32 column = 0; column < 8; ++column) {
                    if ((rows[row] >> column) & 1) {
                        fill_rect(left + static_cast<u64>(column) * scale, top, scale, scale, rgb);
                    }
                }
            }
        }
    }

private:
    // The pixel value whose little endian bytes are in the framebuffer's order.
    u32 pixel_value(u32 rgb) const {
        if (!rgbx_) {
            return rgb & 0xFFFFFF;  // B, G, R, X in memory
        }
        const u32 red = (rgb >> 16) & 0xFF;
        const u32 green = (rgb >> 8) & 0xFF;
        const u32 blue = rgb & 0xFF;
        return red | (green << 8) | (blue << 16);  // R, G, B, X in memory
    }

    // Fills the part of the rectangle that lies on screen. The coordinates
    // are 64-bit so that adding a size to a position cannot wrap.
    void fill_rect(u64 left, u64 top, u64 width, u64 height, u32 rgb) {
        if (left >= width_ || top >= height_) {
            return;
        }
        const u64 right = width > width_ - left ? width_ : left + width;
        const u64 bottom = height > height_ - top ? height_ : top + height;
        const u32 value = pixel_value(rgb);
        for (u64 y = top; y < bottom; ++y) {
            u8* row = base_ + y * pitch_;
            for (u64 x = left; x < right; ++x) {
                // One 32-bit store; memcpy keeps it valid for any alignment.
                __builtin_memcpy(row + x * 4, &value, sizeof(value));
            }
        }
    }

    u8* base_;
    u32 width_, height_, pitch_;
    bool rgbx_;
};

}
