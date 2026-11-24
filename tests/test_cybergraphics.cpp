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
#include "libs/cybergraphics.hpp"
#include "libs/display_modes.hpp"

using namespace gfx_test;
using hombre::CyberGraphics;
using hombre::PixFmt;

namespace {

namespace lvo = hombre::cybergraphics_lvo;
namespace cyber = hombre::cyber;
namespace gfx_lvo = hombre::graphics_lvo;
namespace ndk = hombre::ndk;

constexpr u32 kRp = kData + 0x100;
constexpr u32 kRp2 = kData + 0x300;
constexpr u32 kArray = kData + 0x400;
constexpr u32 kTable = kData + 0x1000;
constexpr u32 kTagsAt = kData + 0x2000;
constexpr u32 kValues = kData + 0x2800;

// JSR -lvo(A6) with A6 set to the library CyberGraphics made.
ExitReason call_cyber(GfxRig& r, CyberGraphics& c, u32 offset) {
    r.put16(kCall, 0x4EAE);
    r.put16(kCall + 2, static_cast<u16>(0x10000 - offset));
    r.put16(kCall + 4, 0x4EF8);  // JMP $400.w
    r.put16(kCall + 6, 0x0400);
    r.jit.flush();
    r.machine.cpu.a[6] = c.base();
    r.machine.cpu.a[7] = kStack;
    r.machine.cpu.pc = kCall;
    return r.jit.run(1000000);
}

u32 result_cyber(GfxRig& r, CyberGraphics& c, u32 offset) {
    CHECK_EQ(call_cyber(r, c, offset), ExitReason::Halt);
    return r.machine.cpu.d[0];
}

u32 make_rastport(GfxRig& r, u32 rp, u32 bitmap) {
    r.cpu().a[1] = rp;
    r.call(Base::Graphics, gfx_lvo::InitRastPort);
    r.put32(rp + ndk::rp_BitMap, bitmap);
    return rp;
}

u32 alloc_bitmap(GfxRig& r, u32 width, u32 height, u32 depth) {
    r.cpu().d[0] = width;
    r.cpu().d[1] = height;
    r.cpu().d[2] = depth;
    r.cpu().d[3] = 0;
    r.cpu().a[0] = 0;
    const u32 bitmap = r.result(Base::Graphics, gfx_lvo::AllocBitMap);
    CHECK(bitmap != 0);
    return bitmap;
}

u32 deep_bitmap(GfxRig& r, u32 width, u32 height, PixFmt fmt) {
    const u32 bitmap = r.graphics.alloc_deep_bitmap(width, height, fmt);
    CHECK(bitmap != 0);
    return bitmap;
}

u32 value_at(GfxRig& r, u32 bitmap, u32 x, u32 y) {
    const hombre::DeepBitmap* deep = r.graphics.deep(bitmap);
    const u32 at = deep->pixels + y * deep->bytes_per_row + x * hombre::bytes_per_pixel(deep->fmt);
    switch (deep->fmt) {
    case PixFmt::Lut8:
        return r.get8(at);
    case PixFmt::Rgb16:
        return r.get16(at);
    case PixFmt::Argb32:
        return r.get32(at);
    }
    return 0;
}

u32 read_pen(GfxRig& r, u32 rp, u32 x, u32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = x;
    r.cpu().d[1] = y;
    return r.result(Base::Graphics, gfx_lvo::ReadPixel);
}

void set_pen(GfxRig& r, u32 rp, u32 pen) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = pen;
    r.call(Base::Graphics, gfx_lvo::SetAPen);
}

void rect_fill(GfxRig& r, u32 rp, u32 x0, u32 y0, u32 x1, u32 y1) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = x0;
    r.cpu().d[1] = y0;
    r.cpu().d[2] = x1;
    r.cpu().d[3] = y1;
    r.call(Base::Graphics, gfx_lvo::RectFill);
}

u32 map_attr(GfxRig& r, CyberGraphics& c, u32 bitmap, u32 attr) {
    r.cpu().a[0] = bitmap;
    r.cpu().d[0] = attr;
    return result_cyber(r, c, lvo::GetCyberMapAttr);
}

u32 id_attr(GfxRig& r, CyberGraphics& c, u32 attr, u32 id) {
    r.cpu().d[0] = attr;
    r.cpu().d[1] = id;
    return result_cyber(r, c, lvo::GetCyberIDAttr);
}

u32 is_cyber(GfxRig& r, CyberGraphics& c, u32 id) {
    r.cpu().d[0] = id;
    return result_cyber(r, c, lvo::IsCyberModeID);
}

u32 best_mode(GfxRig& r, CyberGraphics& c, u32 tags_at) {
    r.cpu().a[0] = tags_at;
    return result_cyber(r, c, lvo::BestCModeIDTagList);
}

void lock_returns_base_address_and_format() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 bm = deep_bitmap(r, 64, 32, PixFmt::Rgb16);
    const hombre::DeepBitmap* deep = r.graphics.deep(bm);
    r.tags(kTagsAt, {{cyber::LBMI_WIDTH, kValues + 0},
                     {cyber::LBMI_HEIGHT, kValues + 4},
                     {cyber::LBMI_DEPTH, kValues + 8},
                     {cyber::LBMI_PIXFMT, kValues + 12},
                     {cyber::LBMI_BYTESPERPIX, kValues + 16},
                     {cyber::LBMI_BYTESPERROW, kValues + 20},
                     {cyber::LBMI_BASEADDRESS, kValues + 24},
                     {0, 0}});
    r.put32(kValues + 0, 0);
    r.cpu().a[0] = bm;
    r.cpu().a[1] = kTagsAt;
    CHECK_EQ(result_cyber(r, c, lvo::LockBitMapTagList), bm);
    CHECK_EQ(r.get32(kValues + 0), 64u);
    CHECK_EQ(r.get32(kValues + 4), 32u);
    CHECK_EQ(r.get32(kValues + 8), 16u);
    CHECK_EQ(r.get32(kValues + 12), cyber::PIXFMT_RGB16);
    CHECK_EQ(r.get32(kValues + 16), 2u);
    CHECK_EQ(r.get32(kValues + 20), deep->bytes_per_row);
    CHECK_EQ(r.get32(kValues + 24), deep->pixels);
    r.cpu().a[0] = bm;
    CHECK_EQ(result_cyber(r, c, lvo::UnLockBitMap), 0u);
    r.cpu().a[0] = bm;
    r.cpu().a[1] = kTagsAt;
    CHECK_EQ(result_cyber(r, c, lvo::UnLockBitMapTagList), 0u);
    r.cpu().a[0] = bm + 4;
    CHECK_EQ(result_cyber(r, c, lvo::UnLockBitMap), 0u);
}

void lock_planar_bitmap_fails() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 planar = alloc_bitmap(r, 32, 8, 3);
    r.tags(kTagsAt, {{cyber::LBMI_WIDTH, kValues}, {0, 0}});
    r.put32(kValues, 0x12345678);
    r.cpu().a[0] = planar;
    r.cpu().a[1] = kTagsAt;
    CHECK_EQ(result_cyber(r, c, lvo::LockBitMapTagList), 0u);
    CHECK_EQ(r.get32(kValues), 0x12345678u);
    r.cpu().a[0] = 0x1234;
    CHECK_EQ(result_cyber(r, c, lvo::LockBitMapTagList), 0u);
}

void get_cyber_map_attr_pixfmt_and_planar() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 argb = deep_bitmap(r, 16, 8, PixFmt::Argb32);
    const hombre::DeepBitmap* deep = r.graphics.deep(argb);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_PIXFMT), 11u);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_BPPIX), 4u);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_XMOD), deep->bytes_per_row);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_WIDTH), 16u);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_HEIGHT), 8u);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_DEPTH), 24u);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_ISCYBERGFX), 0xFFFFFFFFu);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_ISLINEARMEM), 0xFFFFFFFFu);
    CHECK_EQ(map_attr(r, c, argb, cyber::CYBRMATTR_COLORMAP), 0u);
    CHECK_EQ(map_attr(r, c, argb, 0x80000003), 0u);

    const u32 lut = deep_bitmap(r, 16, 8, PixFmt::Lut8);
    CHECK_EQ(map_attr(r, c, lut, cyber::CYBRMATTR_PIXFMT), 0u);
    CHECK_EQ(map_attr(r, c, lut, cyber::CYBRMATTR_DEPTH), 8u);
    CHECK_EQ(map_attr(r, c, lut, cyber::CYBRMATTR_BPPIX), 1u);

    const u32 planar = alloc_bitmap(r, 32, 8, 3);
    CHECK_EQ(map_attr(r, c, planar, cyber::CYBRMATTR_ISCYBERGFX), 0u);
    CHECK_EQ(map_attr(r, c, planar, cyber::CYBRMATTR_ISLINEARMEM), 0u);
    CHECK_EQ(map_attr(r, c, planar, cyber::CYBRMATTR_DEPTH), 3u);
    CHECK_EQ(map_attr(r, c, planar, cyber::CYBRMATTR_WIDTH),
             r.get16(planar + ndk::bm_BytesPerRow) * 8);
    CHECK_EQ(map_attr(r, c, planar, cyber::CYBRMATTR_HEIGHT), 8u);
    CHECK_EQ(map_attr(r, c, planar, cyber::CYBRMATTR_XMOD), 0u);
}

void get_cyber_id_attr() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    hombre::display_modes::configure_hombre(1920, 1080, 512ull << 20);
    const u32 rgb16 = hombre::display_modes::hombre_id(12, 1);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_PIXFMT, rgb16), 5u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_WIDTH, rgb16), 1920u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_HEIGHT, rgb16), 1080u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_DEPTH, rgb16), 16u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_BPPIX, rgb16), 2u);
    const u32 argb = hombre::display_modes::hombre_id(0, 2);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_PIXFMT, argb), 11u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_DEPTH, argb), 24u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_BPPIX, argb), 4u);
    const u32 lut = hombre::display_modes::hombre_id(0, 0);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_PIXFMT, lut), 0u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_DEPTH, lut), 8u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_BPPIX, lut), 1u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_WIDTH, 0x1234), 0u);
    CHECK_EQ(id_attr(r, c, 0x81234567, rgb16), 0u);
    CHECK_EQ(id_attr(r, c, cyber::CYBRIDATTR_WIDTH, 0x8000), 0u);
    hombre::display_modes::configure_hombre(0, 0, 0);
}

void is_cyber_mode_id() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    hombre::display_modes::configure_hombre(1920, 1080, 512ull << 20);
    CHECK_EQ(is_cyber(r, c, hombre::display_modes::hombre_id(0, 1)), 1u);
    CHECK_EQ(is_cyber(r, c, hombre::display_modes::hombre_id(12, 2)), 1u);
    CHECK_EQ(is_cyber(r, c, 0x8000), 0u);
    CHECK_EQ(is_cyber(r, c, hombre::display_modes::hombre_id(15, 0)), 0u);
    hombre::display_modes::configure_hombre(0, 0, 0);
    CHECK_EQ(is_cyber(r, c, hombre::display_modes::hombre_id(0, 1)), 0u);
}

void best_cmode_id_1920x1080_16() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    hombre::display_modes::configure_hombre(1920, 1080, 512ull << 20);
    r.tags(kTagsAt, {{cyber::CYBRBIDTG_Depth, 16},
                     {cyber::CYBRBIDTG_NominalWidth, 1920},
                     {cyber::CYBRBIDTG_NominalHeight, 1080},
                     {0, 0}});
    CHECK_EQ(best_mode(r, c, kTagsAt), hombre::display_modes::hombre_id(12, 1));
    r.tags(kTagsAt, {{cyber::CYBRBIDTG_Depth, 32},
                     {cyber::CYBRBIDTG_NominalWidth, 1920},
                     {cyber::CYBRBIDTG_NominalHeight, 1080},
                     {0, 0}});
    CHECK_EQ(best_mode(r, c, kTagsAt), hombre::display_modes::hombre_id(12, 2));
    CHECK_EQ(best_mode(r, c, 0), hombre::display_modes::hombre_id(0, 0));
    r.tags(kTagsAt, {{cyber::CYBRBIDTG_Depth, 16},
                     {cyber::CYBRBIDTG_NominalWidth, 4000},
                     {cyber::CYBRBIDTG_NominalHeight, 3000},
                     {0, 0}});
    CHECK_EQ(best_mode(r, c, kTagsAt), hombre::display_modes::hombre_id(14, 1));
    hombre::display_modes::configure_hombre(0, 0, 0);
    CHECK_EQ(best_mode(r, c, 0), cyber::INVALID_ID);
}

void write_pixel_array_rectfmt_argb_to_rgb16() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 bm = deep_bitmap(r, 8, 4, PixFmt::Rgb16);
    make_rastport(r, kRp, bm);
    const u32 colours[4] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00808080};
    constexpr u32 kMod = 32;
    for (u32 row = 0; row < 2; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            r.put32(kArray + row * kMod + (1 + col) * 4, colours[col]);
        }
    }
    const auto write = [&](u32 dest_x, u32 size_x) {
        r.cpu().a[0] = kArray;
        r.cpu().d[0] = 1;
        r.cpu().d[1] = 0;
        r.cpu().d[2] = kMod;
        r.cpu().a[1] = kRp;
        r.cpu().d[3] = dest_x;
        r.cpu().d[4] = 1;
        r.cpu().d[5] = size_x;
        r.cpu().d[6] = 2;
        r.cpu().d[7] = cyber::RECTFMT_ARGB;
        return result_cyber(r, c, lvo::WritePixelArray);
    };
    CHECK_EQ(write(2, 4), 8u);
    for (u32 row = 0; row < 2; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            const u32 argb = colours[col];
            CHECK_EQ(value_at(r, bm, 2 + col, 1 + row),
                     hombre::encode_rgb(PixFmt::Rgb16, static_cast<u8>(argb >> 16),
                                        static_cast<u8>(argb >> 8), static_cast<u8>(argb)));
        }
    }
    CHECK_EQ(value_at(r, bm, 1, 1), 0u);
    CHECK_EQ(write(6, 4), 4u);
    CHECK_EQ(value_at(r, bm, 7, 2), hombre::encode_rgb(PixFmt::Rgb16, 0, 0xFF, 0));
}

void read_pixel_array_rgb() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 bm = deep_bitmap(r, 8, 4, PixFmt::Argb32);
    make_rastport(r, kRp, bm);
    const hombre::DeepBitmap* deep = r.graphics.deep(bm);
    const u32 row = deep->pixels + deep->bytes_per_row;
    r.put32(row + 2 * 4, 0xFF102030);
    r.put32(row + 3 * 4, 0xFF405060);
    r.put32(row + 4 * 4, 0xFF708090);
    const auto read = [&](u32 source_x, u32 size_x, u32 dest_x) {
        r.cpu().a[0] = kArray;
        r.cpu().d[0] = dest_x;
        r.cpu().d[1] = 0;
        r.cpu().d[2] = 16;
        r.cpu().a[1] = kRp;
        r.cpu().d[3] = source_x;
        r.cpu().d[4] = 1;
        r.cpu().d[5] = size_x;
        r.cpu().d[6] = 1;
        r.cpu().d[7] = cyber::RECTFMT_RGB;
        return result_cyber(r, c, lvo::ReadPixelArray);
    };
    CHECK_EQ(read(2, 3, 1), 3u);
    CHECK_EQ(r.get8(kArray + 3), 0x10u);
    CHECK_EQ(r.get8(kArray + 4), 0x20u);
    CHECK_EQ(r.get8(kArray + 5), 0x30u);
    CHECK_EQ(r.get8(kArray + 6), 0x40u);
    CHECK_EQ(r.get8(kArray + 7), 0x50u);
    CHECK_EQ(r.get8(kArray + 8), 0x60u);
    CHECK_EQ(r.get8(kArray + 9), 0x70u);
    CHECK_EQ(r.get8(kArray + 10), 0x80u);
    CHECK_EQ(r.get8(kArray + 11), 0x90u);
    CHECK_EQ(r.get8(kArray + 12), 0u);
    CHECK_EQ(read(6, 4, 1), 2u);
    CHECK_EQ(r.get8(kArray + 9), 0x70u);
}

void write_lut_pixel_array() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 bm = deep_bitmap(r, 8, 4, PixFmt::Rgb16);
    make_rastport(r, kRp, bm);
    for (u32 i = 0; i < 256; ++i) {
        r.put32(kTable + i * 4, 0);
    }
    r.put32(kTable + 1 * 4, 0x00FF0000);
    r.put32(kTable + 2 * 4, 0x0000FF00);
    r.put32(kTable + 3 * 4, 0x000000FF);
    r.put32(kTable + 4 * 4, 0x00FFFFFF);
    constexpr u32 kMod = 16;
    for (u32 row = 0; row < 2; ++row) {
        for (u32 col = 0; col < 3; ++col) {
            r.put8(kArray + row * kMod + col, 1 + col);
        }
    }
    const auto write = [&](u32 format) {
        r.cpu().a[0] = kArray;
        r.cpu().d[0] = 0;
        r.cpu().d[1] = 0;
        r.cpu().d[2] = kMod;
        r.cpu().a[1] = kRp;
        r.cpu().a[2] = kTable;
        r.cpu().d[3] = 1;
        r.cpu().d[4] = 2;
        r.cpu().d[5] = 3;
        r.cpu().d[6] = 2;
        r.cpu().d[7] = format;
        return result_cyber(r, c, lvo::WriteLUTPixelArray);
    };
    CHECK_EQ(write(cyber::CTABFMT_XRGB8), 6u);
    CHECK_EQ(value_at(r, bm, 1, 2), hombre::encode_rgb(PixFmt::Rgb16, 0xFF, 0, 0));
    CHECK_EQ(value_at(r, bm, 2, 2), hombre::encode_rgb(PixFmt::Rgb16, 0, 0xFF, 0));
    CHECK_EQ(value_at(r, bm, 3, 2), hombre::encode_rgb(PixFmt::Rgb16, 0, 0, 0xFF));
    CHECK_EQ(value_at(r, bm, 1, 3), hombre::encode_rgb(PixFmt::Rgb16, 0xFF, 0, 0));
    CHECK_EQ(value_at(r, bm, 4, 2), 0u);
    CHECK_EQ(write(1), 0u);
}

void fill_and_invert() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 argb = deep_bitmap(r, 8, 4, PixFmt::Argb32);
    make_rastport(r, kRp, argb);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 1;
    r.cpu().d[2] = 4;
    r.cpu().d[3] = 2;
    r.cpu().d[4] = 0x00112233;
    CHECK_EQ(result_cyber(r, c, lvo::FillPixelArray), 8u);
    CHECK_EQ(value_at(r, argb, 1, 1), 0xFF112233u);
    CHECK_EQ(value_at(r, argb, 0, 0), 0u);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 2;
    r.cpu().d[1] = 1;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 2;
    CHECK_EQ(result_cyber(r, c, lvo::InvertPixelArray), 4u);
    CHECK_EQ(value_at(r, argb, 2, 1), 0xFFEEDDCCu);
    CHECK_EQ(value_at(r, argb, 4, 1), 0xFF112233u);

    const u32 rgb16 = deep_bitmap(r, 8, 4, PixFmt::Rgb16);
    make_rastport(r, kRp2, rgb16);
    r.cpu().a[1] = kRp2;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 2;
    r.cpu().d[4] = 0x00FF0000;
    CHECK_EQ(result_cyber(r, c, lvo::FillPixelArray), 4u);
    CHECK_EQ(value_at(r, rgb16, 0, 0), 0xF800u);
    r.cpu().a[1] = kRp2;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 2;
    CHECK_EQ(result_cyber(r, c, lvo::InvertPixelArray), 4u);
    CHECK_EQ(value_at(r, rgb16, 0, 0), 0x07FFu);

    const u32 planar = alloc_bitmap(r, 8, 4, 3);
    make_rastport(r, kRp, planar);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 2;
    r.cpu().d[4] = 0x00000000;
    CHECK_EQ(result_cyber(r, c, lvo::FillPixelArray), 4u);
    CHECK_EQ(read_pen(r, kRp, 0, 0), 1u);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 2;
    CHECK_EQ(result_cyber(r, c, lvo::InvertPixelArray), 4u);
    CHECK_EQ(read_pen(r, kRp, 0, 0), 6u);
}

void read_write_rgb_pixel() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 argb = deep_bitmap(r, 8, 4, PixFmt::Argb32);
    make_rastport(r, kRp, argb);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 5;
    r.cpu().d[1] = 2;
    r.cpu().d[2] = 0x00123456;
    CHECK_EQ(result_cyber(r, c, lvo::WriteRGBPixel), 0u);
    CHECK_EQ(value_at(r, argb, 5, 2), 0xFF123456u);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 5;
    r.cpu().d[1] = 2;
    CHECK_EQ(result_cyber(r, c, lvo::ReadRGBPixel), 0x00123456u);
    // The coordinates are words: a caller may leave junk in the upper half.
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 0xABCD0006;
    r.cpu().d[1] = 0x12340002;
    r.cpu().d[2] = 0x00654321;
    CHECK_EQ(result_cyber(r, c, lvo::WriteRGBPixel), 0u);
    CHECK_EQ(value_at(r, argb, 6, 2), 0xFF654321u);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 8;
    r.cpu().d[1] = 2;
    CHECK_EQ(result_cyber(r, c, lvo::ReadRGBPixel), 0xFFFFFFFFu);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 8;
    r.cpu().d[1] = 2;
    r.cpu().d[2] = 0x00FFFFFF;
    CHECK_EQ(result_cyber(r, c, lvo::WriteRGBPixel), 0u);

    const u32 rgb16 = deep_bitmap(r, 8, 4, PixFmt::Rgb16);
    make_rastport(r, kRp2, rgb16);
    r.cpu().a[1] = kRp2;
    r.cpu().d[0] = 5;
    r.cpu().d[1] = 2;
    r.cpu().d[2] = 0x00FF0000;
    CHECK_EQ(result_cyber(r, c, lvo::WriteRGBPixel), 0u);
    CHECK_EQ(value_at(r, rgb16, 5, 2), 0xF800u);
    r.cpu().a[1] = kRp2;
    r.cpu().d[0] = 5;
    r.cpu().d[1] = 2;
    CHECK_EQ(result_cyber(r, c, lvo::ReadRGBPixel), 0x00FF0000u);

    const u32 planar = alloc_bitmap(r, 8, 4, 3);
    make_rastport(r, kRp2, planar);
    set_pen(r, kRp2, 2);
    rect_fill(r, kRp2, 0, 0, 0, 0);
    r.cpu().a[1] = kRp2;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    CHECK_EQ(result_cyber(r, c, lvo::ReadRGBPixel), 0x00FFFFFFu);
    r.cpu().a[1] = kRp2;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 0x00000000;
    CHECK_EQ(result_cyber(r, c, lvo::WriteRGBPixel), 0u);
    r.cpu().a[1] = kRp2;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    CHECK_EQ(result_cyber(r, c, lvo::ReadRGBPixel), 0x00FFFFFFu);
}

void move_pixel_array_overlap() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 bm = deep_bitmap(r, 8, 4, PixFmt::Lut8);
    make_rastport(r, kRp, bm);
    const hombre::DeepBitmap* deep = r.graphics.deep(bm);
    for (u32 x = 0; x < 8; ++x) {
        r.put8(deep->pixels + x, 10 + x);
        r.put8(deep->pixels + deep->bytes_per_row + x, 20 + x);
    }
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 2;
    r.cpu().d[3] = 0;
    r.cpu().d[4] = 6;
    r.cpu().d[5] = 1;
    CHECK_EQ(result_cyber(r, c, lvo::MovePixelArray), 6u);
    for (u32 x = 2; x < 8; ++x) {
        CHECK_EQ(value_at(r, bm, x, 0), 10u + (x - 2));
    }
    CHECK_EQ(value_at(r, bm, 0, 0), 10u);
    CHECK_EQ(value_at(r, bm, 1, 0), 11u);
    r.cpu().a[1] = kRp;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 1;
    r.cpu().d[3] = 1;
    r.cpu().d[4] = 4;
    r.cpu().d[5] = 2;
    CHECK_EQ(result_cyber(r, c, lvo::MovePixelArray), 8u);
    CHECK_EQ(value_at(r, bm, 1, 1), 10u);
    CHECK_EQ(value_at(r, bm, 2, 1), 11u);
    CHECK_EQ(value_at(r, bm, 3, 1), 10u);
    CHECK_EQ(value_at(r, bm, 4, 1), 11u);
    CHECK_EQ(value_at(r, bm, 1, 2), 20u);
    CHECK_EQ(value_at(r, bm, 4, 2), 23u);
}

void scale_pixel_array_doubles() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 bm = deep_bitmap(r, 8, 8, PixFmt::Rgb16);
    make_rastport(r, kRp, bm);
    constexpr u32 kMod = 8;
    r.put8(kArray + 0, 0xFF);
    r.put8(kArray + 1, 0);
    r.put8(kArray + 2, 0);
    r.put8(kArray + 3, 0);
    r.put8(kArray + 4, 0xFF);
    r.put8(kArray + 5, 0);
    r.put8(kArray + kMod + 0, 0);
    r.put8(kArray + kMod + 1, 0);
    r.put8(kArray + kMod + 2, 0xFF);
    r.put8(kArray + kMod + 3, 0xFF);
    r.put8(kArray + kMod + 4, 0xFF);
    r.put8(kArray + kMod + 5, 0xFF);
    r.cpu().a[0] = kArray;
    r.cpu().d[0] = 2;
    r.cpu().d[1] = 2;
    r.cpu().d[2] = kMod;
    r.cpu().a[1] = kRp;
    r.cpu().d[3] = 2;
    r.cpu().d[4] = 2;
    r.cpu().d[5] = 4;
    r.cpu().d[6] = 4;
    r.cpu().d[7] = cyber::RECTFMT_RGB;
    CHECK_EQ(result_cyber(r, c, lvo::ScalePixelArray), 16u);
    CHECK_EQ(value_at(r, bm, 2, 2), 0xF800u);
    CHECK_EQ(value_at(r, bm, 3, 2), 0xF800u);
    CHECK_EQ(value_at(r, bm, 4, 2), 0x07E0u);
    CHECK_EQ(value_at(r, bm, 5, 3), 0x07E0u);
    CHECK_EQ(value_at(r, bm, 2, 4), 0x001Fu);
    CHECK_EQ(value_at(r, bm, 3, 5), 0x001Fu);
    CHECK_EQ(value_at(r, bm, 4, 4), 0xFFFFu);
    CHECK_EQ(value_at(r, bm, 5, 5), 0xFFFFu);
    CHECK_EQ(value_at(r, bm, 1, 1), 0u);
}

void extract_color() {
    GfxRig r;
    CyberGraphics c;
    CHECK(c.init(r.machine, r.exec, r.graphics));
    const u32 bm = deep_bitmap(r, 8, 4, PixFmt::Argb32);
    make_rastport(r, kRp, bm);
    const hombre::DeepBitmap* deep = r.graphics.deep(bm);
    for (u32 y = 1; y <= 2; ++y) {
        for (u32 x = 2; x <= 3; ++x) {
            r.put32(deep->pixels + y * deep->bytes_per_row + x * 4, 0xFFFF0000);
        }
    }
    const u32 mask = alloc_bitmap(r, 8, 4, 1);
    const u32 mask_mod = r.get16(mask + ndk::bm_BytesPerRow);
    const u32 mask_plane = r.get32(mask + ndk::bm_Planes);
    CHECK_EQ(mask_mod, 2u);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 b = 0; b < mask_mod; ++b) {
            r.put8(mask_plane + y * mask_mod + b, 0xFF);
        }
    }
    r.cpu().a[0] = kRp;
    r.cpu().a[1] = mask;
    r.cpu().d[0] = 0x00FF0000;
    r.cpu().d[1] = 1;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = 4;
    r.cpu().d[4] = 3;
    CHECK_EQ(result_cyber(r, c, lvo::ExtractColor), 1u);
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 1; x < 5; ++x) {
            const u32 bit =
                (r.get8(mask_plane + y * mask_mod + (x >> 3)) >> (7 - (x & 7))) & 1;
            const bool red = x >= 2 && x <= 3 && y >= 1 && y <= 2;
            CHECK_EQ(bit, red ? 1u : 0u);
        }
    }
    CHECK_EQ(r.get8(mask_plane), 0x80u | 0x07u);
    CHECK_EQ(r.get8(mask_plane + 3 * mask_mod), 0xFFu);
}

}

int main() {
    lock_returns_base_address_and_format();
    lock_planar_bitmap_fails();
    get_cyber_map_attr_pixfmt_and_planar();
    get_cyber_id_attr();
    is_cyber_mode_id();
    best_cmode_id_1920x1080_16();
    write_pixel_array_rectfmt_argb_to_rgb16();
    read_pixel_array_rgb();
    write_lut_pixel_array();
    fill_and_invert();
    read_write_rgb_pixel();
    move_pixel_array_overlap();
    scale_pixel_array_doubles();
    extract_color();
    return check::finish("test_cybergraphics");
}
