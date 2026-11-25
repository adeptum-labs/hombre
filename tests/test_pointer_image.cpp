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

#include "pointer_paint.hpp"
#include "check.hpp"

#include <vector>

using namespace hombre;

namespace {

struct Fill {
    i64 x;
    i64 y;
    i64 width;
    i64 height;
    u32 rgb;
};

PointerShape shaped_with_one_transparent_pixel() {
    PointerShape shape{};
    shape.width = 2;
    shape.height = 2;
    shape.pixels[0][0] = 1;
    shape.pixels[0][1] = 2;
    shape.pixels[1][0] = 3;
    shape.rgb[1] = 0x112233;
    shape.rgb[2] = 0x445566;
    shape.rgb[3] = 0x778899;
    return shape;
}

std::vector<Fill> fills_of(const PointerShape& shape, const Rect& clip) {
    std::vector<Fill> fills;
    paint_pointer(shape, 10, 20, 3, 2, clip,
                  [&fills](i64 x, i64 y, i64 width, i64 height, u32 rgb) {
                      fills.push_back(Fill{x, y, width, height, rgb});
                  });
    return fills;
}

void opaque_pixels_become_fills() {
    const std::vector<Fill> fills =
        fills_of(shaped_with_one_transparent_pixel(), Rect{0, 0, 100, 100});
    CHECK_EQ(fills.size(), 3u);
    CHECK_EQ(fills[0].x, 10);
    CHECK_EQ(fills[0].y, 20);
    CHECK_EQ(fills[0].width, 3);
    CHECK_EQ(fills[0].height, 2);
    CHECK_EQ(fills[0].rgb, 0x112233u);
    CHECK_EQ(fills[1].x, 13);
    CHECK_EQ(fills[1].y, 20);
    CHECK_EQ(fills[1].width, 3);
    CHECK_EQ(fills[1].height, 2);
    CHECK_EQ(fills[1].rgb, 0x445566u);
    CHECK_EQ(fills[2].x, 10);
    CHECK_EQ(fills[2].y, 22);
    CHECK_EQ(fills[2].width, 3);
    CHECK_EQ(fills[2].height, 2);
    CHECK_EQ(fills[2].rgb, 0x778899u);
}

void clip_cuts_pixel_widths() {
    const std::vector<Fill> fills =
        fills_of(shaped_with_one_transparent_pixel(), Rect{11, 18, 13, 30});
    CHECK_EQ(fills.size(), 3u);
    CHECK_EQ(fills[0].x, 11);
    CHECK_EQ(fills[0].width, 2);
    CHECK_EQ(fills[0].height, 2);
    CHECK_EQ(fills[1].x, 13);
    CHECK_EQ(fills[1].width, 1);
    CHECK_EQ(fills[1].height, 2);
    CHECK_EQ(fills[2].x, 11);
    CHECK_EQ(fills[2].width, 2);
    CHECK_EQ(fills[2].height, 2);
}

void oversized_shape_is_cut_to_64() {
    PointerShape shape{};
    shape.width = 200;
    shape.height = 200;
    for (u32 y = 0; y < kMaxPointerHeight; ++y) {
        for (u32 x = 0; x < kMaxPointerWidth; ++x) {
            shape.pixels[y][x] = 1;
        }
    }
    u32 count = 0;
    i64 max_x = -1;
    i64 max_y = -1;
    paint_pointer(shape, 0, 0, 1, 1, Rect{0, 0, 4000, 4000},
                  [&count, &max_x, &max_y](i64 x, i64 y, i64, i64, u32) {
                      ++count;
                      if (x > max_x) {
                          max_x = x;
                      }
                      if (y > max_y) {
                          max_y = y;
                      }
                  });
    CHECK_EQ(count, 64u * 64u);
    CHECK_EQ(max_x, 63);
    CHECK_EQ(max_y, 63);
}

void copy_pointer_shape_falls_back_and_cuts() {
    PointerShape copy{};
    copy.width = 7;
    copy.height = 7;
    copy_pointer_shape(nullptr, copy);
    CHECK_EQ(copy.width, kDefaultPointerShape.width);
    CHECK_EQ(copy.height, kDefaultPointerShape.height);

    PointerShape empty_side{};
    empty_side.width = 0;
    empty_side.height = 16;
    copy_pointer_shape(&empty_side, copy);
    CHECK_EQ(copy.width, kDefaultPointerShape.width);
    CHECK_EQ(copy.height, kDefaultPointerShape.height);

    PointerShape large{};
    large.width = 200;
    large.height = 200;
    copy_pointer_shape(&large, copy);
    CHECK_EQ(copy.width, 64u);
    CHECK_EQ(copy.height, 64u);
}

}

int main() {
    opaque_pixels_become_fills();
    clip_cuts_pixel_widths();
    oversized_shape_is_cut_to_64();
    copy_pointer_shape_falls_back_and_cuts();
    return check::finish("test_pointer_image");
}