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

#include "check.hpp"
#include "common/splash.hpp"

#include <vector>

using namespace hombre;

namespace {

using Rows = std::vector<std::vector<u32>>;

constexpr u32 kBlack = 0x000000;
constexpr u32 kRed = 0xFF0000;

void put16(std::vector<u8>& data, u32 value) {
    data.push_back(static_cast<u8>(value & 0xFF));
    data.push_back(static_cast<u8>((value >> 8) & 0xFF));
}

// A 4x2 image: rows of two black then two red pixels and the reverse, over
// a bar box of the whole image split into two blocks.
std::vector<u8> make_image() {
    std::vector<u8> data{'H', 'S', 'P', 'L'};
    put16(data, 4);  // width
    put16(data, 2);  // height
    put16(data, 0);  // bar x0
    put16(data, 0);  // bar y0
    put16(data, 3);  // bar x1
    put16(data, 1);  // bar y1
    data.push_back(2);  // blocks
    data.push_back(2);  // colours
    const u8 palette[] = {0, 0, 0, 255, 0, 0};
    data.insert(data.end(), palette, palette + sizeof palette);
    const u8 runs[] = {1, 0, 3, 1, 1, 0};  // two black, four red, two black
    data.insert(data.end(), runs, runs + sizeof runs);
    return data;
}

Rows draw(const SplashImage& image, u32 width, u32 height) {
    Rows rows;
    image.draw(width, height, [&rows, width](u32 y, const u32* rgb) {
        CHECK_EQ(y, static_cast<u32>(rows.size()));
        rows.emplace_back(rgb, rgb + width);
    });
    CHECK_EQ(rows.size(), height);
    for (const std::vector<u32>& row : rows) {
        CHECK_EQ(row.size(), width);
    }
    return rows;
}

void parses_a_valid_image() {
    const std::vector<u8> data = make_image();
    SplashImage image;
    CHECK(image.parse(data.data(), static_cast<u32>(data.size())));
    CHECK_EQ(image.width(), 4u);
    CHECK_EQ(image.height(), 2u);
    CHECK_EQ(image.bar_x0(), 0u);
    CHECK_EQ(image.bar_y0(), 0u);
    CHECK_EQ(image.bar_x1(), 3u);
    CHECK_EQ(image.bar_y1(), 1u);
    CHECK_EQ(image.block_count(), 2u);
}

void rejects_bad_images() {
    const std::vector<u8> good = make_image();
    SplashImage image;

    std::vector<u8> data = good;
    data[0] = 'X';
    CHECK(!image.parse(data.data(), static_cast<u32>(data.size())));

    data = good;
    CHECK(!image.parse(data.data(), static_cast<u32>(data.size()) - 1));

    data = good;
    data.resize(10);
    CHECK(!image.parse(data.data(), static_cast<u32>(data.size())));

    data = good;
    CHECK(!image.parse(data.data(), 20));  // the palette is cut short

    data = good;
    data.insert(data.end(), {0, 0});
    CHECK(!image.parse(data.data(), static_cast<u32>(data.size())));

    data = good;
    data[data.size() - 2] = 3;  // the last run grows past the image
    CHECK(!image.parse(data.data(), static_cast<u32>(data.size())));

    data = good;
    data[data.size() - 1] = 2;  // an index past the palette
    CHECK(!image.parse(data.data(), static_cast<u32>(data.size())));
}

void draws_at_the_source_size() {
    const std::vector<u8> data = make_image();
    SplashImage image;
    CHECK(image.parse(data.data(), static_cast<u32>(data.size())));
    const Rows rows = draw(image, 4, 2);
    CHECK(rows[0] == (std::vector<u32>{kBlack, kBlack, kRed, kRed}));
    CHECK(rows[1] == (std::vector<u32>{kRed, kRed, kBlack, kBlack}));
}

void doubles_with_nearest_neighbour() {
    const std::vector<u8> data = make_image();
    SplashImage image;
    CHECK(image.parse(data.data(), static_cast<u32>(data.size())));
    const Rows rows = draw(image, 8, 4);
    CHECK(rows[0] == (std::vector<u32>{kBlack, kBlack, kBlack, kBlack,
                                       kRed, kRed, kRed, kRed}));
    CHECK(rows[1] == rows[0]);
    CHECK(rows[2] == (std::vector<u32>{kRed, kRed, kRed, kRed,
                                       kBlack, kBlack, kBlack, kBlack}));
    CHECK(rows[3] == rows[2]);
}

void shrinks_to_half_the_width() {
    const std::vector<u8> data = make_image();
    SplashImage image;
    CHECK(image.parse(data.data(), static_cast<u32>(data.size())));
    const Rows rows = draw(image, 2, 1);
    CHECK(rows[0] == (std::vector<u32>{kBlack, kRed}));
}

void centres_with_black_margins() {
    const std::vector<u8> data = make_image();
    SplashImage image;
    CHECK(image.parse(data.data(), static_cast<u32>(data.size())));
    const Rows rows = draw(image, 10, 2);
    CHECK(rows[0] == (std::vector<u32>{kBlack, kBlack, kBlack, kBlack, kBlack, kRed, kRed,
                                       kBlack, kBlack, kBlack}));
    CHECK(rows[1] == (std::vector<u32>{kBlack, kBlack, kBlack, kRed, kRed, kBlack, kBlack,
                                       kBlack, kBlack, kBlack}));
}

void block_boxes_follow_the_scaling() {
    const std::vector<u8> data = make_image();
    SplashImage image;
    CHECK(image.parse(data.data(), static_cast<u32>(data.size())));
    i32 x0 = 0;
    i32 y0 = 0;
    i32 x1 = 0;
    i32 y1 = 0;

    image.bar_rect(4, 2, 0, x0, y0, x1, y1);
    CHECK_EQ(x0, 0);
    CHECK_EQ(y0, 0);
    CHECK_EQ(x1, 1);
    CHECK_EQ(y1, 1);
    image.bar_rect(4, 2, 1, x0, y0, x1, y1);
    CHECK_EQ(x0, 2);
    CHECK_EQ(y0, 0);
    CHECK_EQ(x1, 3);
    CHECK_EQ(y1, 1);

    image.bar_rect(8, 4, 0, x0, y0, x1, y1);
    CHECK_EQ(x0, 0);
    CHECK_EQ(y0, 0);
    CHECK_EQ(x1, 3);
    CHECK_EQ(y1, 3);
    image.bar_rect(8, 4, 1, x0, y0, x1, y1);
    CHECK_EQ(x0, 4);
    CHECK_EQ(x1, 7);

    image.bar_rect(10, 2, 0, x0, y0, x1, y1);
    CHECK_EQ(x0, 3);
    CHECK_EQ(y0, 0);
    CHECK_EQ(x1, 4);
    CHECK_EQ(y1, 1);
    image.bar_rect(10, 2, 1, x0, y0, x1, y1);
    CHECK_EQ(x0, 5);
    CHECK_EQ(x1, 6);

    image.bar_rect(10, 2, 2, x0, y0, x1, y1);
    CHECK_EQ(x0, 0);
    CHECK_EQ(y0, 0);
    CHECK_EQ(x1, 0);
    CHECK_EQ(y1, 0);
}

}

int main() {
    parses_a_valid_image();
    rejects_bad_images();
    draws_at_the_source_size();
    doubles_with_nearest_neighbour();
    shrinks_to_half_the_width();
    centres_with_black_margins();
    block_boxes_follow_the_scaling();
    return check::finish("test_splash");
}
