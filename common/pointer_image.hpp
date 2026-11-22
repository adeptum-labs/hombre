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

inline constexpr u32 kPointerWidth = 16;
inline constexpr u32 kPointerHeight = 16;

// The 3.1 default pointer, row by row from its tip at (0, 0): '.' is
// transparent, 'r' the red colour 1, 'k' the black colour 2 and 'c' the
// cream colour 3.
inline constexpr const char* kPointerRows[] = {
    "rc",
    "krcc",
    ".krrcc",
    ".krrrrcc",
    "..krrrrrcc",
    "..krrrrrrr",
    "...krrrc",
    "...krrkrc",
    "....kr.krc",
    "....kr..krc",
    ".........kr",
};

inline constexpr u32 kPointerRowCount = sizeof kPointerRows / sizeof kPointerRows[0];

// The colour of the default pointer at x, y: 0 transparent, 1 red, 2 black,
// 3 cream.
constexpr u32 pointer_pixel(u32 x, u32 y) {
    if (x >= kPointerWidth || y >= kPointerRowCount) {
        return 0;
    }
    const char* row = kPointerRows[y];
    for (u32 i = 0; i < x; ++i) {
        if (row[i] == '\0') {
            return 0;
        }
    }
    switch (row[x]) {
    case 'r':
        return 1;
    case 'k':
        return 2;
    case 'c':
        return 3;
    default:
        return 0;
    }
}

inline constexpr u32 kMaxPointerWidth = 64;
inline constexpr u32 kMaxPointerHeight = 64;

// A pointer image the display draws: pixels hold 0 for transparent and 1 to 3
// for the colours of rgb, 0xRRGGBB. The hot spot is the pixel of the image
// that lies on the pointer's position.
struct PointerShape {
    u16 width;
    u16 height;
    i16 hot_x;
    i16 hot_y;
    u8 pixels[kMaxPointerHeight][kMaxPointerWidth];
    u32 rgb[4];
};

// The default pointer in the colours 3.1 gives it, tip at the hot spot.
constexpr PointerShape default_pointer_shape() {
    PointerShape shape{};
    shape.width = kPointerWidth;
    shape.height = kPointerHeight;
    for (u32 y = 0; y < kPointerHeight; ++y) {
        for (u32 x = 0; x < kPointerWidth; ++x) {
            shape.pixels[y][x] = static_cast<u8>(pointer_pixel(x, y));
        }
    }
    shape.rgb[1] = 0xE04040;
    shape.rgb[2] = 0x000000;
    shape.rgb[3] = 0xE0E0C0;
    return shape;
}

inline constexpr PointerShape kDefaultPointerShape = default_pointer_shape();

}
