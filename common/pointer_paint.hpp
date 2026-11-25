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

#include "libs/gfx.hpp"
#include "pointer_image.hpp"

namespace hombre {

// Calls fill(x, y, width, height, rgb) for each opaque pixel of the
// shape, its top left corner at left, top, each pixel pixel_width by
// pixel_height, clipped to clip (inclusive). Sides beyond 64 are cut.
template <typename Fill>
void paint_pointer(const PointerShape& shape, i64 left, i64 top, i64 pixel_width,
                   i64 pixel_height, const Rect& clip, Fill fill) {
    if (pixel_width <= 0 || pixel_height <= 0) {
        return;
    }
    const u32 width =
        static_cast<u32>(shape.width) < kMaxPointerWidth ? shape.width : kMaxPointerWidth;
    const u32 height =
        static_cast<u32>(shape.height) < kMaxPointerHeight ? shape.height : kMaxPointerHeight;
    for (u32 py = 0; py < height; ++py) {
        const i64 block_top = top + static_cast<i64>(py) * pixel_height;
        const i64 first_row = block_top > clip.y0 ? block_top : clip.y0;
        const i64 last_row = block_top + pixel_height - 1 < clip.y1 ? block_top + pixel_height - 1
                                                                   : clip.y1;
        for (u32 px = 0; px < width && first_row <= last_row; ++px) {
            const u32 pixel = shape.pixels[py][px];
            if (pixel == 0) {
                continue;
            }
            const i64 block_left = left + static_cast<i64>(px) * pixel_width;
            const i64 first_column = block_left > clip.x0 ? block_left : clip.x0;
            const i64 last_column = block_left + pixel_width - 1 < clip.x1
                                        ? block_left + pixel_width - 1
                                        : clip.x1;
            if (first_column <= last_column) {
                fill(first_column, first_row, last_column - first_column + 1,
                     last_row - first_row + 1, shape.rgb[pixel & 3]);
            }
        }
    }
}

}
