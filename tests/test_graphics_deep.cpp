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

// A bitmap made with a deep friend draws in the friend's palette.
void friend_bitmap_keeps_the_palette() {
    GfxRig r;
    make_colormap(r);
    const u32 screen = r.graphics.alloc_deep_bitmap(32, 16, PixFmt::Argb32);
    r.graphics.deep(screen)->colormap = kMap;
    r.graphics.deep(screen)->palette = r.graphics.palette_for(kMap);
    r.cpu().d[0] = 16;
    r.cpu().d[1] = 8;
    r.cpu().d[2] = 8;
    r.cpu().d[3] = 0;
    r.cpu().a[0] = screen;
    const u32 bm = r.result(Base::Graphics, lvo::AllocBitMap);
    CHECK(r.graphics.deep(bm) != nullptr);
    CHECK(r.graphics.deep(bm)->palette == r.graphics.deep(screen)->palette);
    CHECK_EQ(r.graphics.deep(bm)->colormap, kMap);
}

// A copy to the right inside one row of a deep RastPort reads each source
// pixel before it is overwritten.
void blt_bitmap_rastport_overlapping_to_the_right() {
    GfxRig r;
    const u32 bm = r.graphics.alloc_deep_bitmap(16, 4, PixFmt::Lut8);
    rastport(r, kRp, bm);
    for (u32 x = 0; x < 6; ++x) {
        set_pen(r, kRp, x + 1);
        rect_fill(r, kRp, static_cast<i32>(x), 0, static_cast<i32>(x), 0);
    }
    r.cpu().a[0] = bm;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().a[1] = kRp;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 0;
    r.cpu().d[4] = 6;
    r.cpu().d[5] = 1;
    r.cpu().d[6] = 0xC0;
    r.call(Base::Graphics, lvo::BltBitMapRastPort);
    for (u32 x = 0; x < 6; ++x) {
        CHECK_EQ(value_at(r, bm, x + 2, 0), x + 1);
    }
}

// A mask stencils a blit from a deep source: its row is as wide as a
// bitmap of the source's width, and only the set bits reach the
// destination.
void blt_mask_bitmap_rastport_on_deep() {
    GfxRig r;
    const u32 src = r.graphics.alloc_deep_bitmap(16, 4, PixFmt::Argb32);
    const u32 dst = r.graphics.alloc_deep_bitmap(16, 4, PixFmt::Argb32);
    rastport(r, kRp, dst);
    const hombre::DeepBitmap* s = r.graphics.deep(src);
    for (u32 x = 0; x < 16; ++x) {
        r.put32(s->pixels + x * 4, 0xFF000000u | x);
        r.put32(s->pixels + s->bytes_per_row + x * 4, 0xFF000010u | x);
    }
    // Grey all over, so what is drawn and what is left alone both show. The
    // destination keeps its alpha, as with any blit into a RastPort.
    const hombre::DeepBitmap* d = r.graphics.deep(dst);
    for (u32 i = 0; i < 16 * 4; ++i) {
        r.put32(d->pixels + (i / 16) * d->bytes_per_row + (i % 16) * 4, 0xFF808080u);
    }
    r.put8(kArray, 0xAA);  // row 0: even pixels
    r.put8(kArray + 1, 0x00);
    r.put8(kArray + 2, 0xF0);  // row 1: the first four
    r.put8(kArray + 3, 0x00);
    const auto blt = [&](u32 minterm, u32 mask, u32 dest_y) {
        r.cpu().a[0] = src;
        r.cpu().d[0] = 0;
        r.cpu().d[1] = 0;
        r.cpu().a[1] = kRp;
        r.cpu().d[2] = 0;
        r.cpu().d[3] = dest_y;
        r.cpu().d[4] = 8;
        r.cpu().d[5] = 2;
        r.cpu().d[6] = minterm;
        r.cpu().a[2] = mask;
        return r.call(Base::Graphics, lvo::BltMaskBitMapRastPort);
    };
    CHECK_EQ(blt(0xE0, kArray, 0), ExitReason::Halt);
    CHECK_EQ(value_at(r, dst, 0, 0), 0xFF000000u);
    CHECK_EQ(value_at(r, dst, 1, 0), 0xFF808080u);
    CHECK_EQ(value_at(r, dst, 2, 0), 0xFF000002u);
    CHECK_EQ(value_at(r, dst, 0, 1), 0xFF000010u);
    CHECK_EQ(value_at(r, dst, 3, 1), 0xFF000013u);
    CHECK_EQ(value_at(r, dst, 4, 1), 0xFF808080u);
    // Minterm 0x20 inverts the source's colour bits.
    r.put32(r.graphics.deep(dst)->pixels + 2 * r.graphics.deep(dst)->bytes_per_row,
            0xFF000000u);
    r.put8(kArray, 0x80);
    CHECK_EQ(blt(0x20, kArray, 2), ExitReason::Halt);
    CHECK_EQ(value_at(r, dst, 0, 2), 0xFFFFFFFFu);
    CHECK_EQ(value_at(r, dst, 1, 2), 0xFF808080u);
    // A null mask changes nothing.
    r.put32(r.graphics.deep(dst)->pixels + 3 * r.graphics.deep(dst)->bytes_per_row + 5 * 4,
            0x12345678u);
    CHECK_EQ(blt(0xE0, 0, 3), ExitReason::Halt);
    CHECK_EQ(value_at(r, dst, 5, 3), 0x12345678u);
}

// The alpha blit blends a deep ARGB32 source over ARGB32 and RGB16, and
// copies the pixels of at least half opacity to LUT8 and planar ones.
void alpha_blit_blends_and_copies() {
    GfxRig r;
    u32 palette[256] = {};
    palette[0] = 0x000000;
    palette[1] = 0xFF0000;
    palette[2] = 0x00FF00;
    palette[3] = 0xFFFFFF;
    const u32 src = r.graphics.alloc_deep_bitmap(4, 1, PixFmt::Argb32);
    r.graphics.deep(src)->palette = palette;
    const u32 pixels = r.graphics.deep(src)->pixels;
    r.put32(pixels, 0x80FF0000u);
    r.put32(pixels + 4, 0x000000FFu);
    r.put32(pixels + 8, 0xFF00FF00u);
    r.put32(pixels + 12, 0x40FFFFFFu);

    const u32 argb = r.graphics.alloc_deep_bitmap(4, 1, PixFmt::Argb32);
    for (u32 x = 0; x < 4; ++x) {
        r.put32(r.graphics.deep(argb)->pixels + x * 4, 0xFF0000FFu);
    }
    rastport(r, kRp, argb);
    r.graphics.blt_alpha_rastport(src, 0, 0, kRp, 0, 0, 4, 1);
    CHECK_EQ(value_at(r, argb, 0, 0), 0xFF80007Fu);
    CHECK_EQ(value_at(r, argb, 1, 0), 0xFF0000FFu);
    CHECK_EQ(value_at(r, argb, 2, 0), 0xFF00FF00u);
    CHECK_EQ(value_at(r, argb, 3, 0), 0xFF4040FFu);

    // Over blue, so that the destination's own colour shows in the blend:
    // half red over blue is 128, 0, 127, a quarter of white 64, 64, 255.
    const u32 rgb16 = r.graphics.alloc_deep_bitmap(4, 1, PixFmt::Rgb16);
    for (u32 x = 0; x < 4; ++x) {
        r.put16(r.graphics.deep(rgb16)->pixels + x * 2, 0x001F);
    }
    rastport(r, kRp, rgb16);
    r.graphics.blt_alpha_rastport(src, 0, 0, kRp, 0, 0, 4, 1);
    CHECK_EQ(value_at(r, rgb16, 0, 0), (16u << 11) | 15u);
    CHECK_EQ(value_at(r, rgb16, 1, 0), 0x001Fu);
    CHECK_EQ(value_at(r, rgb16, 2, 0), 0x07E0u);
    CHECK_EQ(value_at(r, rgb16, 3, 0), (8u << 11) | (16u << 5) | 31u);

    const u32 lut = r.graphics.alloc_deep_bitmap(4, 1, PixFmt::Lut8);
    r.graphics.deep(lut)->palette = palette;
    for (u32 x = 0; x < 4; ++x) {
        r.put8(r.graphics.deep(lut)->pixels + x, 5);
    }
    rastport(r, kRp, lut);
    r.graphics.blt_alpha_rastport(src, 0, 0, kRp, 0, 0, 4, 1);
    CHECK_EQ(value_at(r, lut, 0, 0), 1u);
    CHECK_EQ(value_at(r, lut, 1, 0), 5u);
    CHECK_EQ(value_at(r, lut, 2, 0), 2u);
    CHECK_EQ(value_at(r, lut, 3, 0), 5u);

    r.cpu().d[0] = 4;
    r.cpu().d[1] = 1;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 0;
    r.cpu().a[0] = 0;
    const u32 planar = r.result(Base::Graphics, lvo::AllocBitMap);
    rastport(r, kSrcRp, planar);
    set_pen(r, kSrcRp, 3);
    rect_fill(r, kSrcRp, 0, 0, 3, 0);
    // A planar destination has no colours the source could name, so
    // nothing is drawn there.
    r.graphics.blt_alpha_rastport(src, 0, 0, kSrcRp, 0, 0, 4, 1);
    for (i32 x = 0; x < 4; ++x) {
        CHECK_EQ(read_pixel(r, kSrcRp, x, 0), 3u);
    }
}

// The pen of pixel (x, y) in the large bitmaps below.
u32 big_pen(u32 x, u32 y) { return (x * 3 + y) % 4; }

u32 big_value(PixFmt fmt, u32 x, u32 y) {
    const u32 v = x * 7 + y * 13;
    switch (fmt) {
    case PixFmt::Lut8:
        return v & 0xFF;
    case PixFmt::Rgb16:
        return v & 0xFFFF;
    case PixFmt::Argb32:
        return 0xFF000000u | (v & 0xFFFFFF);
    }
    return 0;
}

void put_value(GfxRig& r, u32 bitmap, u32 x, u32 y, u32 v) {
    const hombre::DeepBitmap* d = r.graphics.deep(bitmap);
    const u32 at = d->pixels + y * d->bytes_per_row + x * hombre::bytes_per_pixel(d->fmt);
    switch (d->fmt) {
    case PixFmt::Lut8:
        r.put8(at, v);
        break;
    case PixFmt::Rgb16:
        r.put16(at, v);
        break;
    case PixFmt::Argb32:
        r.put32(at, v);
        break;
    }
}

u32 alloc_planar(GfxRig& r, u32 width, u32 height, u32 depth) {
    r.cpu().d[0] = width;
    r.cpu().d[1] = height;
    r.cpu().d[2] = depth;
    r.cpu().d[3] = 1;
    r.cpu().a[0] = 0;
    return r.result(Base::Graphics, lvo::AllocBitMap);
}

void put_planar(GfxRig& r, u32 bitmap, u32 x, u32 y, u32 pen) {
    const u32 bpr = r.get16(bitmap + ndk::bm_BytesPerRow);
    for (u32 plane = 0; plane < r.get8(bitmap + ndk::bm_Depth); ++plane) {
        const u32 at = r.get32(bitmap + ndk::bm_Planes + plane * 4) + y * bpr + x / 8;
        const u32 bit = 0x80u >> (x & 7);
        r.put8(at, ((pen >> plane) & 1) != 0 ? r.get8(at) | bit : r.get8(at) & ~bit);
    }
}

u32 planar_at(GfxRig& r, u32 bitmap, u32 x, u32 y) {
    const u32 bpr = r.get16(bitmap + ndk::bm_BytesPerRow);
    u32 pen = 0;
    for (u32 plane = 0; plane < r.get8(bitmap + ndk::bm_Depth); ++plane) {
        const u32 at = r.get32(bitmap + ndk::bm_Planes + plane * 4) + y * bpr + x / 8;
        pen |= ((r.get8(at) >> (7 - (x & 7))) & 1) << plane;
    }
    return pen;
}

void blt_rastport(GfxRig& r, u32 src, u32 sx, u32 sy, u32 rp, u32 width, u32 height,
                  u32 mask = 0) {
    r.cpu().a[0] = src;
    r.cpu().d[0] = sx;
    r.cpu().d[1] = sy;
    r.cpu().a[1] = rp;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = 0;
    r.cpu().d[4] = width;
    r.cpu().d[5] = height;
    r.cpu().d[6] = 0xC0;
    r.cpu().a[2] = mask;
    r.call(Base::Graphics, mask != 0 ? lvo::BltMaskBitMapRastPort : lvo::BltBitMapRastPort);
}

// Bitmaps wider or taller than 4096 pixels are blitted from, a rectangle
// far inside them included: planar ones, through a mask too, and every
// deep format.
void blits_read_bitmaps_past_4096_pixels() {
    // An ARGB32 row of 16383 pixels would not fit BytesPerRow, so the
    // widest deep bitmap is a little narrower.
    struct Size {
        u32 width, height, sx, sy, deep_width, deep_sx;
    };
    const Size sizes[] = {{6000, 4, 5000, 1, 6000, 5000},
                          {16, 6000, 8, 5000, 16, 8},
                          {16383, 2, 16370, 0, 16380, 16370}};
    for (const Size& s : sizes) {
        GfxRig r;
        const u32 src = alloc_planar(r, s.width, s.height, 2);
        CHECK(src != 0);
        for (u32 y = s.sy; y < s.sy + 2 && y < s.height; ++y) {
            for (u32 x = s.sx; x < s.sx + 8; ++x) {
                put_planar(r, src, x, y, big_pen(x, y));
            }
        }
        const u32 dst = alloc_planar(r, 16, 4, 2);
        rastport(r, kRp, dst);
        const u32 rows = s.height < 2 ? s.height : 2;
        blt_rastport(r, src, s.sx, s.sy, kRp, 8, rows);
        for (u32 y = 0; y < rows; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                CHECK_EQ(read_pixel(r, kRp, static_cast<i32>(x), static_cast<i32>(y)),
                         big_pen(s.sx + x, s.sy + y));
            }
        }
        // Through a mask of the source's planes, which lets every other
        // pixel through onto pen 0.
        const u32 bpr = r.get16(src + ndk::bm_BytesPerRow);
        const u32 mask = r.exec.alloc(bpr * s.height, ndk::MEMF_CLEAR);
        CHECK(mask != 0);
        for (u32 y = s.sy; y < s.sy + rows; ++y) {
            r.put8(mask + y * bpr + s.sx / 8, 0xAA);
        }
        set_pen(r, kRp, 0);
        rect_fill(r, kRp, 0, 0, 15, 3);
        blt_rastport(r, src, s.sx, s.sy, kRp, 8, rows, mask);
        for (u32 x = 0; x < 8; ++x) {
            CHECK_EQ(read_pixel(r, kRp, static_cast<i32>(x), 0),
                     (x & 1) == 0 ? big_pen(s.sx + x, s.sy) : 0u);
        }
        for (const PixFmt fmt : {PixFmt::Lut8, PixFmt::Rgb16, PixFmt::Argb32}) {
            const u32 deep = r.graphics.alloc_deep_bitmap(s.deep_width, s.height, fmt);
            CHECK(deep != 0);
            for (u32 y = s.sy; y < s.sy + rows; ++y) {
                for (u32 x = s.deep_sx; x < s.deep_sx + 8; ++x) {
                    put_value(r, deep, x, y, big_value(fmt, x, y));
                }
            }
            const u32 to = r.graphics.alloc_deep_bitmap(16, 4, fmt);
            rastport(r, kSrcRp, to);
            // The RastPort's ARGB32 bitmap keeps its own alpha.
            blt_rastport(r, deep, s.deep_sx, s.sy, kSrcRp, 8, rows);
            const u32 bits = fmt == PixFmt::Argb32 ? 0x00FFFFFFu : 0xFFFFFFFFu;
            for (u32 y = 0; y < rows; ++y) {
                for (u32 x = 0; x < 8; ++x) {
                    CHECK_EQ(value_at(r, to, x, y),
                             big_value(fmt, s.deep_sx + x, s.sy + y) & bits);
                }
            }
            if (fmt == PixFmt::Argb32) {
                for (u32 x = 0; x < 8; ++x) {
                    put_value(r, to, x, 0, 0xFF000000u);
                }
                r.graphics.blt_alpha_rastport(deep, s.deep_sx, s.sy, kSrcRp, 0, 0, 8, rows);
                for (u32 x = 0; x < 8; ++x) {
                    CHECK_EQ(value_at(r, to, x, 0), big_value(fmt, s.deep_sx + x, s.sy));
                }
            }
            r.graphics.free_deep_bitmap(to);
            r.graphics.free_deep_bitmap(deep);
        }
    }
}

// A copy across itself wider than the row the blit works on at a time
// reads every pixel before it is overwritten, to the right and the left,
// planar and deep.
void wide_copies_across_themselves() {
    GfxRig r;
    const u32 width = 6000;
    const auto pattern = [](u32 x) { return (x % 7 == 0 || x % 11 == 3) ? 1u : 0u; };
    const u32 planar = alloc_planar(r, width, 1, 1);
    CHECK(planar != 0);
    for (const i32 shift : {5, -5}) {
        for (u32 x = 0; x < width; ++x) {
            put_planar(r, planar, x, 0, pattern(x));
        }
        const u32 from = shift > 0 ? 0 : 5;
        r.cpu().a[0] = planar;
        r.cpu().d[0] = from;
        r.cpu().d[1] = 0;
        r.cpu().a[1] = planar;
        r.cpu().d[2] = static_cast<u32>(static_cast<i32>(from) + shift);
        r.cpu().d[3] = 0;
        r.cpu().d[4] = 5500;
        r.cpu().d[5] = 1;
        r.cpu().d[6] = 0xC0;
        r.cpu().d[7] = 0xFF;
        r.cpu().a[2] = 0;
        r.call(Base::Graphics, lvo::BltBitMap);
        u32 wrong = 0;
        for (u32 x = 0; x < 5500; ++x) {
            const u32 to = static_cast<u32>(static_cast<i32>(from + x) + shift);
            wrong += planar_at(r, planar, to, 0) != pattern(from + x) ? 1 : 0;
        }
        CHECK_EQ(wrong, 0u);
    }
    const u32 deep = r.graphics.alloc_deep_bitmap(width, 1, PixFmt::Lut8);
    rastport(r, kRp, deep);
    for (const i32 shift : {5, -5}) {
        for (u32 x = 0; x < width; ++x) {
            put_value(r, deep, x, 0, x & 0xFF);
        }
        const u32 from = shift > 0 ? 0 : 5;
        r.cpu().a[0] = deep;
        r.cpu().d[0] = from;
        r.cpu().d[1] = 0;
        r.cpu().a[1] = kRp;
        r.cpu().d[2] = static_cast<u32>(static_cast<i32>(from) + shift);
        r.cpu().d[3] = 0;
        r.cpu().d[4] = 5500;
        r.cpu().d[5] = 1;
        r.cpu().d[6] = 0xC0;
        r.call(Base::Graphics, lvo::BltBitMapRastPort);
        u32 wrong = 0;
        for (u32 x = 0; x < 5500; ++x) {
            const u32 to = static_cast<u32>(static_cast<i32>(from + x) + shift);
            wrong += value_at(r, deep, to, 0) != ((from + x) & 0xFF) ? 1 : 0;
        }
        CHECK_EQ(wrong, 0u);
    }
}

}

int main() {
    rect_fill_and_read_pixel_on_deep_rastports();
    draw_and_text_on_deep_rastport();
    blt_bitmap_between_deep_and_planar();
    write_chunky_pixels_maps_pens();
    pixel_array8_round_trip();
    setrgb32_does_not_recolour_drawn_pixels();
    friend_bitmap_keeps_the_palette();
    blt_bitmap_rastport_overlapping_to_the_right();
    blt_mask_bitmap_rastport_on_deep();
    alpha_blit_blends_and_copies();
    blits_read_bitmaps_past_4096_pixels();
    wide_copies_across_themselves();
    return check::finish("test_graphics_deep");
}
