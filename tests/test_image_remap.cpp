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

#include "image/remap.hpp"
#include "check.hpp"

using namespace hombre;
using namespace hombre::image;

namespace {

void cube_levels_follow_the_depth() {
    CHECK_EQ(remap::levels(0), 2u);
    CHECK_EQ(remap::levels(1), 2u);
    CHECK_EQ(remap::levels(4), 2u);
    CHECK_EQ(remap::levels(5), 3u);
    CHECK_EQ(remap::levels(6), 4u);
    CHECK_EQ(remap::levels(7), 4u);
    CHECK_EQ(remap::levels(8), 5u);
    CHECK_EQ(remap::levels(24), 5u);
}

void cube_colours_spread_over_the_whole_range() {
    u8 r = 0, g = 0, b = 0;
    remap::cube_colour(0, 2, r, g, b);
    CHECK_EQ(r, 0u);
    CHECK_EQ(g, 0u);
    CHECK_EQ(b, 0u);
    remap::cube_colour(1, 2, r, g, b);
    CHECK_EQ(r, 0u);
    CHECK_EQ(g, 0u);
    CHECK_EQ(b, 255u);
    remap::cube_colour(2, 2, r, g, b);
    CHECK_EQ(g, 255u);
    remap::cube_colour(7, 2, r, g, b);
    CHECK_EQ(r, 255u);
    CHECK_EQ(g, 255u);
    CHECK_EQ(b, 255u);
    remap::cube_colour(0, 5, r, g, b);
    CHECK_EQ(r, 0u);
    CHECK_EQ(g, 0u);
    CHECK_EQ(b, 0u);
    remap::cube_colour(124, 5, r, g, b);
    CHECK_EQ(r, 255u);
    CHECK_EQ(g, 255u);
    CHECK_EQ(b, 255u);
    // Four levels have an even step; five do not, and keep white exact.
    remap::cube_colour(1, 4, r, g, b);
    CHECK_EQ(b, 85u);
    remap::cube_colour(1, 5, r, g, b);
    CHECK_EQ(b, 63u);
}

void the_dither_reaches_the_levels_and_keeps_the_ends() {
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            CHECK_EQ(remap::level(0, x, y, 2), 0u);
            CHECK_EQ(remap::level(255, x, y, 2), 1u);
            CHECK_EQ(remap::level(255, x, y, 5), 4u);
        }
    }
    // Half grey is the upper level on half of the matrix.
    u32 upper = 0;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            upper += remap::level(128, x, y, 2);
        }
    }
    CHECK_EQ(upper, 8u);
    // A quarter grey is the upper level on a quarter of it.
    upper = 0;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            upper += remap::level(64, x, y, 2);
        }
    }
    CHECK_EQ(upper, 4u);
}

void cube_indexes_name_the_colours() {
    CHECK_EQ(remap::cube_index(0, 0, 0, 0, 0, 2), 0u);
    CHECK_EQ(remap::cube_index(255, 255, 255, 0, 0, 2), 7u);
    // Red changes slowest, blue fastest.
    CHECK_EQ(remap::cube_index(255, 0, 0, 0, 0, 2), 4u);
    CHECK_EQ(remap::cube_index(0, 255, 0, 0, 0, 2), 2u);
    // Half grey at the first matrix cell is the bottom level, at the
    // second the top one.
    CHECK_EQ(remap::cube_index(128, 128, 128, 0, 0, 2), 0u);
    CHECK_EQ(remap::cube_index(128, 128, 128, 1, 0, 2), 7u);
    CHECK(remap::cube_index(10, 20, 30, 1, 2, 5) < 125u);
}

void rgb16_dithers_and_keeps_black_and_white() {
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            CHECK_EQ(remap::rgb16(0, 0, 0, x, y), 0u);
            CHECK_EQ(remap::rgb16(255, 255, 255, x, y), 0xFFFFu);
        }
    }
    CHECK_EQ(remap::rgb16(128, 128, 128, 0, 0), 0x7BEFu);
    CHECK_EQ(remap::rgb16(128, 128, 128, 1, 0), 0x8410u);
}

void blending_is_rounded_and_exact_at_the_ends() {
    CHECK_EQ(remap::blend(200, 50, 0), 50u);
    CHECK_EQ(remap::blend(200, 50, 255), 200u);
    CHECK_EQ(remap::blend(200, 50, 128), 125u);
    CHECK_EQ(remap::blend(0, 255, 128), 127u);
    CHECK_EQ(remap::blend(255, 0, 127), 127u);
}

void luma_weighs_the_channels() {
    CHECK_EQ(remap::luma(0, 0, 0), 0u);
    CHECK_EQ(remap::luma(255, 255, 255), 255u);
    CHECK_EQ(remap::luma(255, 0, 0), 76u);
    CHECK_EQ(remap::luma(0, 255, 0), 149u);
    CHECK_EQ(remap::luma(0, 0, 255), 29u);
}

}

int main() {
    cube_levels_follow_the_depth();
    cube_colours_spread_over_the_whole_range();
    the_dither_reaches_the_levels_and_keeps_the_ends();
    cube_indexes_name_the_colours();
    rgb16_dithers_and_keeps_black_and_white();
    blending_is_rounded_and_exact_at_the_ends();
    luma_weighs_the_channels();
    return check::finish("test_image_remap");
}
