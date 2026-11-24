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


#include "gfx_rig.hpp"

using namespace gfx_test;
using hombre::PixFmt;
namespace ndk = hombre::ndk;
namespace lvo = hombre::graphics_lvo;

namespace {

constexpr u32 kRp = kData + 0x100;
constexpr u32 kSrcRp = kData + 0x200;
constexpr u32 kArray = kData + 0x400;
constexpr u32 kMap = kData + 0x800;
constexpr u32 kTable = kData + 0x900;
constexpr u32 kLow = kData + 0xB00;
constexpr u32 kVp = kData + 0xD00;
constexpr u32 kRgb32 = kData + 0xE00;

u32 rastport(GfxRig& r, u32 rp, u32 bitmap) {
    r.cpu().a[1] = rp;
    r.call(Base::Graphics, lvo::InitRastPort);
    r.put32(rp + ndk::rp_BitMap, bitmap);
    return rp;
}

void set_pen(GfxRig& r, u32 rp, u32 pen) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = pen;
    r.call(Base::Graphics, lvo::SetAPen);
}

void rect_fill(GfxRig& r, u32 rp, i32 x0, i32 y0, i32 x1, i32 y1) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x0);
    r.cpu().d[1] = static_cast<u32>(y0);
    r.cpu().d[2] = static_cast<u32>(x1);
    r.cpu().d[3] = static_cast<u32>(y1);
    r.call(Base::Graphics, lvo::RectFill);
}

u32 value_at(GfxRig& r, u32 bitmap, u32 x, u32 y) {
    const hombre::DeepBitmap* d = r.graphics.deep(bitmap);
    const u32 at = d->pixels + y * d->bytes_per_row + x * hombre::bytes_per_pixel(d->fmt);
    switch (d->fmt) {
    case PixFmt::Lut8:
        return r.get8(at);
    case PixFmt::Rgb16:
        return r.get16(at);
    case PixFmt::Argb32:
        return r.get32(at);
    }
    return 0;
}

u32 read_pixel(GfxRig& r, u32 rp, i32 x, i32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    return r.result(Base::Graphics, lvo::ReadPixel);
}

void rect_fill_and_read_pixel_on_deep_rastports() {
    GfxRig r;
    for (PixFmt fmt : {PixFmt::Rgb16, PixFmt::Argb32}) {
        const u32 bm = r.graphics.alloc_deep_bitmap(64, 32, fmt);
        rastport(r, kRp, bm);
        set_pen(r, kRp, 2);
        rect_fill(r, kRp, 4, 4, 9, 9);
        CHECK_EQ(value_at(r, bm, 4, 4), fmt == PixFmt::Rgb16 ? 0xFFFFu : 0xFFFFFFFFu);
        CHECK_EQ(value_at(r, bm, 10, 4), 0u);
        CHECK_EQ(read_pixel(r, kRp, 5, 5), 2u);
    }
}

void draw_and_text_on_deep_rastport() {
    GfxRig r;
    const u32 bm = r.graphics.alloc_deep_bitmap(64, 32, PixFmt::Argb32);
    rastport(r, kRp, bm);
    set_pen(r, kRp, 2);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 20;
    r.call(Base::Graphics, lvo::Move);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 30;
    r.cpu().d[1] = 20;
    r.call(Base::Graphics, lvo::Draw);
    CHECK_EQ(value_at(r, bm, 15, 20), 0xFFFFFFFFu);
    r.str(kArray, "Hi");
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 10;
    r.call(Base::Graphics, lvo::Move);
    r.cpu().a[1] = kRp;
    r.cpu().a[0] = kArray;
    r.cpu().d[0] = 2;
    r.call(Base::Graphics, lvo::Text);
    u32 inked = 0;
    for (u32 y = 0; y < 14; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            inked += value_at(r, bm, x, y) != 0;
        }
    }
    CHECK(inked > 8);
}

void blt_bitmap_between_deep_and_planar() {
    GfxRig r;
    const u32 src = r.graphics.alloc_deep_bitmap(32, 16, PixFmt::Argb32);
    const u32 dst = r.graphics.alloc_deep_bitmap(32, 16, PixFmt::Argb32);
    rastport(r, kRp, src);
    set_pen(r, kRp, 2);
    rect_fill(r, kRp, 0, 0, 3, 3);
    const auto blt = [&](u32 s, u32 sx, u32 d, u32 dx, u32 minterm) {
        r.cpu().a[0] = s;
        r.cpu().d[0] = sx;
        r.cpu().d[1] = 0;
        r.cpu().a[1] = d;
        r.cpu().d[2] = dx;
        r.cpu().d[3] = 0;
        r.cpu().d[4] = 4;
        r.cpu().d[5] = 4;
        r.cpu().d[6] = minterm;
        r.cpu().d[7] = 0xFF;
        r.cpu().a[2] = 0;
        return r.result(Base::Graphics, lvo::BltBitMap);
    };
    CHECK(blt(src, 0, dst, 10, 0xC0) != 0);
    CHECK_EQ(value_at(r, dst, 10, 0), 0xFFFFFFFFu);
    CHECK_EQ(value_at(r, dst, 14, 0), 0u);
    // An inverted copy turns the colour over.
    blt(src, 0, dst, 20, 0x30);
    CHECK_EQ(value_at(r, dst, 20, 0) & 0xFFFFFFu, 0u);
    // A planar source lands in the palette's colours.
    r.cpu().d[0] = 32;
    r.cpu().d[1] = 16;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 0;
    r.cpu().a[0] = 0;
    const u32 planar = r.result(Base::Graphics, lvo::AllocBitMap);
    rastport(r, kSrcRp, planar);
    set_pen(r, kSrcRp, 2);
    rect_fill(r, kSrcRp, 0, 0, 3, 3);
    blt(planar, 0, dst, 0, 0xC0);
    CHECK_EQ(value_at(r, dst, 1, 1), 0xFFFFFFFFu);
}

void write_chunky_pixels_maps_pens() {
    GfxRig r;
    const u32 bm = r.graphics.alloc_deep_bitmap(16, 8, PixFmt::Argb32);
    rastport(r, kRp, bm);
    const u8 pens[6] = {2, 1, 2, 1, 2, 1};
    for (u32 i = 0; i < 6; ++i) {
        r.put8(kArray + i, pens[i]);
    }
    r.cpu().a[0] = kRp;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 2;
    r.cpu().d[2] = 3;
    r.cpu().d[3] = 3;
    r.cpu().a[2] = kArray;
    r.cpu().d[4] = 3;
    r.call(Base::Graphics, lvo::WriteChunkyPixels);
    CHECK_EQ(value_at(r, bm, 1, 2), 0xFFFFFFFFu);
    CHECK_EQ(value_at(r, bm, 2, 2), 0xFF000000u);
    CHECK_EQ(value_at(r, bm, 2, 3), 0xFFFFFFFFu);
    CHECK_EQ(value_at(r, bm, 4, 2), 0u);
}

void pixel_array8_round_trip() {
    GfxRig r;
    for (bool deep : {true, false}) {
        u32 bm = 0;
        if (deep) {
            bm = r.graphics.alloc_deep_bitmap(32, 8, PixFmt::Lut8);
        } else {
            r.cpu().d[0] = 32;
            r.cpu().d[1] = 8;
            r.cpu().d[2] = 3;
            r.cpu().d[3] = 0;
            r.cpu().a[0] = 0;
            bm = r.result(Base::Graphics, lvo::AllocBitMap);
        }
        rastport(r, kRp, bm);
        // Rows of a pixel array are padded to a multiple of 16 pixels.
        for (u32 i = 0; i < 32; ++i) {
            r.put8(kArray + i, static_cast<u8>(i % 8));
        }
        r.cpu().a[0] = kRp;
        r.cpu().d[0] = 0;
        r.cpu().d[1] = 0;
        r.cpu().d[2] = 9;
        r.cpu().d[3] = 1;
        r.cpu().a[2] = kArray;
        r.cpu().a[1] = 0;
        CHECK_EQ(r.result(Base::Graphics, lvo::WritePixelArray8), 20u);
        CHECK_EQ(read_pixel(r, kRp, 3, 0), 3u);
        CHECK_EQ(read_pixel(r, kRp, 1, 1), 1u);
        for (u32 i = 0; i < 32; ++i) {
            r.put8(kArray + 0x80 + i, 0xEE);
        }
        r.cpu().a[0] = kRp;
        r.cpu().d[0] = 0;
        r.cpu().d[1] = 0;
        r.cpu().d[2] = 9;
        r.cpu().d[3] = 1;
        r.cpu().a[2] = kArray + 0x80;
        r.cpu().a[1] = 0;
        CHECK_EQ(r.result(Base::Graphics, lvo::ReadPixelArray8), 20u);
        CHECK_EQ(r.get8(kArray + 0x80 + 5), 5u);
        CHECK_EQ(r.get8(kArray + 0x80 + 16 + 2), 2u);
        // WritePixelLine8 writes one row.
        r.put8(kArray, 6);
        r.cpu().a[0] = kRp;
        r.cpu().d[0] = 0;
        r.cpu().d[1] = 5;
        r.cpu().d[2] = 1;
        r.cpu().a[2] = kArray;
        r.cpu().a[1] = 0;
        CHECK_EQ(r.result(Base::Graphics, lvo::WritePixelLine8), 1u);
        CHECK_EQ(read_pixel(r, kRp, 0, 5), 6u);
    }
}

// A ColorMap of 256 colours at kMap, all black, in a ViewPort at kVp.
void make_colormap(GfxRig& r) {
    for (u32 i = 0; i < ndk::ColorMap_SIZEOF; ++i) {
        r.put8(kMap + i, 0);
    }
    for (u32 i = 0; i < 512; ++i) {
        r.put8(kTable + i, 0);
        r.put8(kLow + i, 0);
    }
    r.put8(kMap + ndk::cm_Type, 1);
    r.put16(kMap + ndk::cm_Count, 256);
    r.put32(kMap + ndk::cm_ColorTable, kTable);
    r.put32(kMap + ndk::cm_LowColorBits, kLow);
    for (u32 i = 0; i < ndk::ViewPort_SIZEOF; ++i) {
        r.put8(kVp + i, 0);
    }
    r.put32(kVp + ndk::vp_ColorMap, kMap);
}

void setrgb32_does_not_recolour_drawn_pixels() {
    GfxRig r;
    make_colormap(r);
    const u32 bm = r.graphics.alloc_deep_bitmap(16, 8, PixFmt::Argb32);
    r.graphics.deep(bm)->colormap = kMap;
    r.graphics.deep(bm)->palette = r.graphics.palette_for(kMap);
    const auto set_rgb = [&](u32 pen, u32 red, u32 green, u32 blue) {
        r.cpu().a[0] = kVp;
        r.cpu().d[0] = pen;
        r.cpu().d[1] = red;
        r.cpu().d[2] = green;
        r.cpu().d[3] = blue;
        r.call(Base::Graphics, lvo::SetRGB32);
    };
    set_rgb(5, 0xFF000000u, 0, 0);
    rastport(r, kRp, bm);
    set_pen(r, kRp, 5);
    rect_fill(r, kRp, 0, 0, 0, 0);
    CHECK_EQ(value_at(r, bm, 0, 0), 0xFFFF0000u);
    set_rgb(5, 0, 0xFF000000u, 0);
    CHECK_EQ(value_at(r, bm, 0, 0), 0xFFFF0000u);
    rect_fill(r, kRp, 1, 0, 1, 0);
    CHECK_EQ(value_at(r, bm, 1, 0), 0xFF00FF00u);
    // LoadRGB32 reaches the palette too.
    r.put32(kRgb32, (1u << 16) | 7);
    r.put32(kRgb32 + 4, 0);
    r.put32(kRgb32 + 8, 0);
    r.put32(kRgb32 + 12, 0xFF000000u);
    r.put32(kRgb32 + 16, 0);
    r.cpu().a[0] = kVp;
    r.cpu().a[1] = kRgb32;
    r.call(Base::Graphics, lvo::LoadRGB32);
    set_pen(r, kRp, 7);
    rect_fill(r, kRp, 2, 0, 2, 0);
    CHECK_EQ(value_at(r, bm, 2, 0), 0xFF0000FFu);
}

}

int main() {
    rect_fill_and_read_pixel_on_deep_rastports();
    draw_and_text_on_deep_rastport();
    blt_bitmap_between_deep_and_planar();
    write_chunky_pixels_maps_pens();
    pixel_array8_round_trip();
    setrgb32_does_not_recolour_drawn_pixels();
    return check::finish("test_graphics_deep");
}
