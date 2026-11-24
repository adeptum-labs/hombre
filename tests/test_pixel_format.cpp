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


#include "pixel_format.hpp"
#include "check.hpp"

using namespace hombre;

namespace {

void rgb16_round_trip() {
    CHECK_EQ(encode_rgb(PixFmt::Rgb16, 0xFF, 0x80, 0x00), 0xFC00u);
    u8 r = 0, g = 0, b = 0;
    decode_rgb(PixFmt::Rgb16, 0xFC00, r, g, b);
    CHECK_EQ(r, 0xFFu);
    CHECK_EQ(g, 0x82u);
    CHECK_EQ(b, 0x00u);
    decode_rgb(PixFmt::Rgb16, 0xFFFF, r, g, b);
    CHECK_EQ(r, 0xFFu);
    CHECK_EQ(g, 0xFFu);
    CHECK_EQ(b, 0xFFu);
    CHECK_EQ(encode_rgb(PixFmt::Argb32, 0x11, 0x22, 0x33), 0xFF112233u);
    CHECK_EQ(encode_rgb(PixFmt::Lut8, 1, 2, 3), 0u);
    CHECK_EQ(bytes_per_pixel(PixFmt::Lut8), 1u);
    CHECK_EQ(bytes_per_pixel(PixFmt::Rgb16), 2u);
    CHECK_EQ(bytes_per_pixel(PixFmt::Argb32), 4u);
}

void argb32_store_is_big_endian() {
    u8 bytes[4] = {};
    store_pixel(PixFmt::Argb32, bytes, 0x00112233u);
    CHECK_EQ(bytes[0], 0x00u);
    CHECK_EQ(bytes[1], 0x11u);
    CHECK_EQ(bytes[2], 0x22u);
    CHECK_EQ(bytes[3], 0x33u);
    CHECK_EQ(load_pixel(PixFmt::Argb32, bytes), 0x00112233u);
    store_pixel(PixFmt::Rgb16, bytes, 0xABCDu);
    CHECK_EQ(bytes[0], 0xABu);
    CHECK_EQ(bytes[1], 0xCDu);
    CHECK_EQ(load_pixel(PixFmt::Rgb16, bytes), 0xABCDu);
    store_pixel(PixFmt::Lut8, bytes, 0x1F7u);
    CHECK_EQ(bytes[0], 0xF7u);
    CHECK_EQ(load_pixel(PixFmt::Lut8, bytes), 0xF7u);
}

void lut8_row_uses_palette() {
    u32 lut[256] = {};
    lut[0] = 0x000000u;
    lut[3] = 0x102030u;
    const u8 src[3] = {3, 0, 3};
    u32 out[3] = {};
    row_to_framebuffer(PixFmt::Lut8, src, 3, lut, true, out);
    // BGRX in memory is the little-endian u32 0x00RRGGBB.
    CHECK_EQ(out[0], 0x00102030u);
    CHECK_EQ(out[1], 0u);
    CHECK_EQ(out[2], 0x00102030u);
    row_to_framebuffer(PixFmt::Lut8, src, 1, lut, false, out);
    // RGBX in memory is the little-endian u32 0x00BBGGRR.
    CHECK_EQ(out[0], 0x00302010u);
}

void rgb16_row_to_bgrx() {
    const u8 src[4] = {0xF8, 0x00, 0x07, 0xE0};  // red, green
    u32 out[2] = {};
    row_to_framebuffer(PixFmt::Rgb16, src, 2, nullptr, true, out);
    CHECK_EQ(out[0], 0x00FF0000u);
    CHECK_EQ(out[1], 0x0000FF00u);
}

void argb32_row_to_rgbx() {
    const u8 src[8] = {0xFF, 0x11, 0x22, 0x33, 0x00, 0xAA, 0xBB, 0xCC};
    u32 out[2] = {};
    row_to_framebuffer(PixFmt::Argb32, src, 2, nullptr, false, out);
    CHECK_EQ(out[0], 0x00332211u);
    CHECK_EQ(out[1], 0x00CCBBAAu);
    row_to_framebuffer(PixFmt::Argb32, src, 2, nullptr, true, out);
    CHECK_EQ(out[0], 0x00112233u);
    CHECK_EQ(out[1], 0x00AABBCCu);
}

}

int main() {
    rgb16_round_trip();
    argb32_store_is_big_endian();
    lut8_row_uses_palette();
    rgb16_row_to_bgrx();
    argb32_row_to_rgbx();
    return check::finish("test_pixel_format");
}
