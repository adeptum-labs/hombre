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

// Colour reduction for a destination of fewer colours than a true-colour
// source: a colour cube of the levels a depth can hold, ordered 4x4 Bayer
// dithering into it, the same dither into R5G6B5, and alpha blending.
// Pure integer arithmetic; nothing here calls the OS.

namespace hombre::image::remap {

// The levels each channel of the colour cube gets on a destination of
// depth planes: five from eight planes on, four from six, three from five,
// two below.
constexpr u32 levels(u32 depth) {
    return depth >= 8 ? 5 : depth >= 6 ? 4 : depth == 5 ? 3 : 2;
}

// The 4x4 ordered dither matrix, 0 to 15, repeated over the picture.
constexpr u32 threshold(u32 x, u32 y) {
    constexpr u8 kMatrix[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    return kMatrix[y & 3][x & 3];
}

// One channel to its level, 0 to levels - 1, with the threshold of the
// matrix turning a value between two levels more often into the upper the
// nearer it is; 0 stays the bottom level and 255 the top.
constexpr u32 level(u32 channel, u32 x, u32 y, u32 levels) {
    if (levels <= 1) {
        return 0;
    }
    const u32 top = levels - 1;
    return (channel * top * 16 + threshold(x, y) * 255 + 128) / (255 * 16);
}

// The colour cube index of red, green and blue at x, y: levels^3 entries,
// red changing slowest.
constexpr u32 cube_index(u8 red, u8 green, u8 blue, u32 x, u32 y, u32 levels) {
    const u32 r = level(red, x, y, levels);
    const u32 g = level(green, x, y, levels);
    const u32 b = level(blue, x, y, levels);
    return r * levels * levels + g * levels + b;
}

// The red, green and blue of a cube index, the levels spread over 0 to 255
// inclusive.
constexpr void cube_colour(u32 index, u32 levels, u8& red, u8& green, u8& blue) {
    if (levels <= 1) {
        red = green = blue = 0;
        return;
    }
    const u32 top = levels - 1;
    red = static_cast<u8>(((index / (levels * levels)) % levels) * 255 / top);
    green = static_cast<u8>(((index / levels) % levels) * 255 / top);
    blue = static_cast<u8>((index % levels) * 255 / top);
}

// R5G6B5 of a colour, each channel dithered into as many steps as its
// field holds; pure black and pure white stay exact.
constexpr u32 rgb16(u8 red, u8 green, u8 blue, u32 x, u32 y) {
    const u32 r = level(red, x, y, 32);
    const u32 g = level(green, x, y, 64);
    const u32 b = level(blue, x, y, 32);
    return (r << 11) | (g << 5) | b;
}

// One channel of foreground over background at alpha, 0 to 255, rounded:
// the background at 0 and the foreground at 255, exactly.
constexpr u8 blend(u8 foreground, u8 background, u32 alpha) {
    return static_cast<u8>((foreground * alpha + background * (255 - alpha) + 127) / 255);
}

// The grey of a colour, its luma: black at 0 and white at 255.
constexpr u8 luma(u8 red, u8 green, u8 blue) {
    return static_cast<u8>((299u * red + 587u * green + 114u * blue) / 1000);
}

}
