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

#include "../common/font.hpp"
#include "gfx_rig.hpp"

#include <cstring>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;
using gfx_test::Base;
using gfx_test::GfxRig;
using gfx_test::kData;
using gfx_test::logged;

namespace {

constexpr u32 kScreen = kData;             // 346 bytes, BitMap at +184
constexpr u32 kRastPort = kData + 0x200;
constexpr u32 kLayer = kData + 0x300;
constexpr u32 kViewPort = kData + 0x400;
constexpr u32 kColorMap = kData + 0x440;
constexpr u32 kColorTable = kData + 0x480;
constexpr u32 kTextAttr = kData + 0x500;
constexpr u32 kOtherFont = kData + 0x600;
constexpr u32 kStrings = kData + 0x700;
constexpr u32 kBitmap = kData + 0x800;
constexpr u32 kWidth = 640;
constexpr u32 kHeight = 400;
constexpr u32 kBytesPerRow = 80;
constexpr u32 kPlaneBytes = kBytesPerRow * kHeight;

struct Surface {
    u32 bitmap;
    u32 rp;
    u32 plane0;
    u32 plane1;
};

// A 2-plane width x height bitmap at bitmap, an initialised RastPort at rp
// that draws on it, and the two cleared planes.
Surface make_sized_surface(GfxRig& r, u32 bitmap, u32 rp, u32 width, u32 height) {
    const u32 row_bytes = (width + 15) / 16 * 2;
    const u32 plane_bytes = row_bytes * height;
    const u32 plane0 = r.exec.alloc(plane_bytes, MEMF_CHIP | MEMF_CLEAR);
    const u32 plane1 = r.exec.alloc(plane_bytes, MEMF_CHIP | MEMF_CLEAR);
    CHECK(plane0 != 0);
    CHECK(plane1 != 0);
    r.cpu().a[0] = bitmap;
    r.cpu().d[0] = 2;
    r.cpu().d[1] = width;
    r.cpu().d[2] = height;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitBitMap), ExitReason::Halt);
    r.put32(bitmap + bm_Planes, plane0);
    r.put32(bitmap + bm_Planes + 4, plane1);
    r.cpu().a[1] = rp;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    r.put32(rp + rp_BitMap, bitmap);
    return {bitmap, rp, plane0, plane1};
}

Surface make_surface(GfxRig& r, u32 bitmap, u32 rp) {
    return make_sized_surface(r, bitmap, rp, kWidth, kHeight);
}

void move(GfxRig& r, u32 rp, i32 x, i32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Move), ExitReason::Halt);
}

void rect_fill(GfxRig& r, u32 rp, i32 x0, i32 y0, i32 x1, i32 y1) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x0);
    r.cpu().d[1] = static_cast<u32>(y0);
    r.cpu().d[2] = static_cast<u32>(x1);
    r.cpu().d[3] = static_cast<u32>(y1);
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
}

u32 read_pixel(GfxRig& r, u32 rp, i32 x, i32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    return r.result(Base::Graphics, graphics_lvo::ReadPixel);
}

u32 write_pixel(GfxRig& r, u32 rp, i32 x, i32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    return r.result(Base::Graphics, graphics_lvo::WritePixel);
}

std::vector<u8> plane_snapshot(GfxRig& r, const Surface& s) {
    std::vector<u8> bytes(kPlaneBytes * 2);
    for (u32 i = 0; i < kPlaneBytes; ++i) {
        bytes[i] = static_cast<u8>(r.get8(s.plane0 + i));
        bytes[kPlaneBytes + i] = static_cast<u8>(r.get8(s.plane1 + i));
    }
    return bytes;
}

void init_bitmap_fills_the_fields() {
    GfxRig r;
    // The plane pointers belong to the caller: InitBitMap leaves them alone.
    for (u32 plane = 0; plane < 8; ++plane) {
        r.put32(kBitmap + bm_Planes + plane * 4, 0x1234 + plane);
    }
    r.cpu().a[0] = kBitmap;
    r.cpu().d[0] = 2;
    r.cpu().d[1] = kWidth;
    r.cpu().d[2] = kHeight;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitBitMap), ExitReason::Halt);
    CHECK_EQ(r.get16(kBitmap + bm_BytesPerRow), 80u);
    CHECK_EQ(r.get16(kBitmap + bm_Rows), 400u);
    CHECK_EQ(r.get8(kBitmap + bm_Depth), 2u);
    CHECK_EQ(r.get8(kBitmap + bm_Flags), 0u);
    for (u32 plane = 0; plane < 8; ++plane) {
        CHECK_EQ(r.get32(kBitmap + bm_Planes + plane * 4), 0x1234u + plane);
    }

    // A width that is not a multiple of 16 is rounded up to whole words.
    r.cpu().a[0] = kBitmap;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 33;
    r.cpu().d[2] = 7;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitBitMap), ExitReason::Halt);
    CHECK_EQ(r.get16(kBitmap + bm_BytesPerRow), 6u);
    CHECK_EQ(r.get16(kBitmap + bm_Rows), 7u);
    CHECK_EQ(r.get8(kBitmap + bm_Depth), 1u);
}

void init_rastport_sets_default_pens() {
    GfxRig r;
    r.cpu().a[1] = kRastPort;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    CHECK_EQ(r.get8(kRastPort + rp_Mask), 0xFFu);
    CHECK_EQ(r.get8(kRastPort + rp_FgPen), 1u);
    CHECK_EQ(r.get8(kRastPort + rp_BgPen), 0u);
    CHECK_EQ(r.get8(kRastPort + rp_AOlPen), 1u);
    CHECK_EQ(r.get8(kRastPort + rp_DrawMode), JAM2);
    CHECK_EQ(r.get32(kRastPort + rp_Font), r.graphics.topaz_font());
    CHECK_EQ(r.get16(kRastPort + rp_TxHeight), 8u);
    CHECK_EQ(r.get16(kRastPort + rp_TxWidth), 8u);
    CHECK_EQ(r.get16(kRastPort + rp_TxBaseline), 6u);
    CHECK_EQ(r.get16(kRastPort + rp_cp_x), 0u);
    CHECK_EQ(r.get16(kRastPort + rp_cp_y), 0u);
    CHECK_EQ(r.get32(kRastPort + rp_BitMap), 0u);
}

void set_apen_bpen_drmd_store_fields() {
    GfxRig r;
    r.cpu().a[1] = kRastPort;
    r.cpu().d[0] = 0x1203;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().d[0] = 0x1234;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetBPen), ExitReason::Halt);
    r.cpu().d[0] = INVERSVID;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetDrMd), ExitReason::Halt);
    CHECK_EQ(r.get8(kRastPort + rp_FgPen), 3u);
    CHECK_EQ(r.get8(kRastPort + rp_BgPen), 0x34u);
    CHECK_EQ(r.get8(kRastPort + rp_DrawMode), INVERSVID);
}

void move_sets_cp() {
    GfxRig r;
    move(r, kRastPort, -5, 300);
    CHECK_EQ(r.get16(kRastPort + rp_cp_x), 0xFFFBu);
    CHECK_EQ(r.get16(kRastPort + rp_cp_y), 300u);
}

void rect_fill_fills_pen_and_clips() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.graphics.set_pens(s.rp, 2, 0, JAM2);
    rect_fill(r, s.rp, -10, -10, 5, 5);
    CHECK_EQ(read_pixel(r, s.rp, 0, 0), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 5, 5), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 6, 5), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 5, 6), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 0, 6), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 6, 0), 0u);
    CHECK_EQ(read_pixel(r, s.rp, -1, 0), 0xFFFFFFFFu);
    CHECK(r.refreshes.empty());
}

void write_then_read_pixel_vectors() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.graphics.set_pens(s.rp, 3, 0, JAM1);
    CHECK_EQ(write_pixel(r, s.rp, 7, 8), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 7, 8), 3u);
    CHECK_EQ(write_pixel(r, s.rp, -1, 0), 0xFFFFFFFFu);
    CHECK_EQ(write_pixel(r, s.rp, 640, 0), 0xFFFFFFFFu);
    CHECK_EQ(read_pixel(r, s.rp, -1, 0), 0xFFFFFFFFu);
    CHECK_EQ(read_pixel(r, s.rp, 640, 0), 0xFFFFFFFFu);
    CHECK_EQ(read_pixel(r, s.rp, 0, 400), 0xFFFFFFFFu);
}

void draw_moves_cp_and_sets_pixels() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.graphics.set_pens(s.rp, 1, 0, JAM2);
    move(r, s.rp, 2, 3);
    r.cpu().a[1] = s.rp;
    r.cpu().d[0] = 10;
    r.cpu().d[1] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Draw), ExitReason::Halt);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 10u);
    CHECK_EQ(r.get16(s.rp + rp_cp_y), 3u);
    for (i32 x = 2; x <= 10; ++x) {
        CHECK_EQ(read_pixel(r, s.rp, x, 3), 1u);
    }
    CHECK_EQ(read_pixel(r, s.rp, 1, 3), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 11, 3), 0u);
}

void text_draws_glyphs_advances_cp_and_returns_nothing_unsafe() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.graphics.set_pens(s.rp, 1, 0, JAM2);
    move(r, s.rp, 8, 8);
    r.str(kStrings, "H");
    r.cpu().a[1] = s.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 1;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Text), ExitReason::Halt);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 16u);
    CHECK_EQ(r.get16(s.rp + rp_cp_y), 8u);
    const u8* rows = glyph('H');
    for (u32 row = 0; row < 8; ++row) {
        for (u32 col = 0; col < 8; ++col) {
            const u32 expected = ((rows[row] >> col) & 1) != 0 ? 1u : 0u;
            CHECK_EQ(read_pixel(r, s.rp, 8 + static_cast<i32>(col),
                                2 + static_cast<i32>(row)),
                     expected);
        }
    }
}

void text_length_is_8_per_char() {
    GfxRig r;
    r.cpu().a[1] = kRastPort;
    r.cpu().d[0] = 7;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextLength), 56u);
    r.cpu().d[0] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextLength), 0u);
    r.cpu().d[0] = 100;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextLength), 800u);
}

void set_font_updates_txheight_txbaseline() {
    GfxRig r;
    r.put16(kOtherFont + tf_YSize, 10);
    r.put16(kOtherFont + tf_XSize, 9);
    r.put16(kOtherFont + tf_Baseline, 7);
    r.cpu().a[1] = kRastPort;
    r.cpu().a[0] = kOtherFont;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetFont), ExitReason::Halt);
    CHECK_EQ(r.get32(kRastPort + rp_Font), kOtherFont);
    CHECK_EQ(r.get16(kRastPort + rp_TxHeight), 10u);
    CHECK_EQ(r.get16(kRastPort + rp_TxWidth), 9u);
    CHECK_EQ(r.get16(kRastPort + rp_TxBaseline), 7u);
}

void open_font_topaz_8_returns_the_builtin_font_and_other_names_give_0() {
    GfxRig r;
    r.str(kStrings, "topaz.font");
    r.put32(kTextAttr + ta_Name, kStrings);
    r.put16(kTextAttr + ta_YSize, 8);
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), r.graphics.topaz_font());

    r.str(kStrings, "TOPAZ.FONT");
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), r.graphics.topaz_font());

    r.put16(kTextAttr + ta_YSize, 9);
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), 0u);

    r.put16(kTextAttr + ta_YSize, 8);
    r.str(kStrings, "chargen.font");
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), 0u);
}

void scroll_raster_moves_pixels() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.graphics.set_pens(s.rp, 2, 0, JAM2);
    rect_fill(r, s.rp, 0, 0, 15, 5);
    r.graphics.set_pens(s.rp, 1, 0, JAM1);
    CHECK_EQ(write_pixel(r, s.rp, 0, 5), 0u);
    r.cpu().a[1] = s.rp;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 1;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = 0;
    r.cpu().d[4] = 15;
    r.cpu().d[5] = 5;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::ScrollRaster), ExitReason::Halt);
    CHECK_EQ(read_pixel(r, s.rp, 0, 4), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 0, 5), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 1, 4), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 1, 5), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 0, 6), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 16, 0), 0u);
}

void set_rgb4_and_load_rgb4_write_the_colour_table() {
    GfxRig r;
    r.put32(kViewPort + vp_ColorMap, kColorMap);
    r.put16(kColorMap + cm_Count, 4);
    r.put32(kColorMap + cm_ColorTable, kColorTable);
    for (u32 i = 0; i < 4; ++i) {
        r.put16(kColorTable + i * 2, 0x1111);
    }

    r.cpu().a[0] = kViewPort;
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 0xA;
    r.cpu().d[2] = 0xB;
    r.cpu().d[3] = 0xC;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB4), ExitReason::Halt);
    CHECK_EQ(r.get16(kColorTable + 2), 0xABCu);

    // An entry past the colour count leaves the table alone.
    r.cpu().a[0] = kViewPort;
    r.cpu().d[0] = 9;
    r.cpu().d[1] = 0xF;
    r.cpu().d[2] = 0xF;
    r.cpu().d[3] = 0xF;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB4), ExitReason::Halt);
    CHECK_EQ(r.get16(kColorTable), 0x1111u);

    r.put16(kStrings, 0x0111);
    r.put16(kStrings + 2, 0x0222);
    r.put16(kStrings + 4, 0x0333);
    r.cpu().a[0] = kViewPort;
    r.cpu().a[1] = kStrings;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::LoadRGB4), ExitReason::Halt);
    CHECK_EQ(r.get16(kColorTable), 0x0111u);
    CHECK_EQ(r.get16(kColorTable + 2), 0x0222u);
    CHECK_EQ(r.get16(kColorTable + 4), 0x0333u);
    CHECK_EQ(r.get16(kColorTable + 6), 0x1111u);

    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 2;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetRGB4), 0x0333u);
    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 9;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetRGB4), 0xFFFFFFFFu);
}

void set_rgb32_get_rgb32_and_load_rgb32_v39_palette() {
    GfxRig r;
    const u32 color_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 low_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 load_table = r.exec.alloc(512, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 out_table = r.exec.alloc(512, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(color_table != 0 && low_table != 0 && load_table != 0 && out_table != 0);

    const u32 vp = kScreen + sc_ViewPort;
    r.put16(kScreen + sc_Width, 320);
    r.put16(kScreen + sc_Height, 200);
    r.put32(vp + vp_ColorMap, kColorMap);
    r.put8(kColorMap + cm_Type, COLORMAP_TYPE_V39);
    r.put16(kColorMap + cm_Count, 256);
    r.put32(kColorMap + cm_ColorTable, color_table);
    r.put32(kColorMap + cm_LowColorBits, low_table);
    r.display.front_screen = kScreen;

    // SetRGB32 stores high/low nibbles and defers the front-screen refresh.
    r.cpu().a[0] = vp;
    r.cpu().d[0] = 42;
    r.cpu().d[1] = 0x12345678u;
    r.cpu().d[2] = 0xABCDEF01u;
    r.cpu().d[3] = 0x0F112233u;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32), ExitReason::Halt);
    CHECK_EQ(r.get16(color_table + 42 * 2), 0x01A0u);
    CHECK_EQ(r.get16(low_table + 42 * 2), 0x02BFu);
    CHECK(r.refreshes.empty());
    r.intuition.flush_palette();
    CHECK_EQ(r.refreshes.size(), 1u);
    CHECK_EQ(r.refreshes[0].x0, 0);
    CHECK_EQ(r.refreshes[0].y0, 0);
    CHECK_EQ(r.refreshes[0].x1, 319);
    CHECK_EQ(r.refreshes[0].y1, 199);

    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 42;
    r.cpu().d[1] = 1;
    r.cpu().a[1] = out_table;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::GetRGB32), ExitReason::Halt);
    CHECK_EQ(r.get32(out_table), 0x12121212u);
    CHECK_EQ(r.get32(out_table + 4), 0xABABABABu);
    CHECK_EQ(r.get32(out_table + 8), 0x0F0F0F0Fu);

    // SetRGB4 replicates the high nibble into LowColorBits.
    r.cpu().a[0] = vp;
    r.cpu().d[0] = 7;
    r.cpu().d[1] = 0xA;
    r.cpu().d[2] = 0x3;
    r.cpu().d[3] = 0xE;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB4), ExitReason::Halt);
    CHECK_EQ(r.get16(color_table + 7 * 2), 0x0A3Eu);
    CHECK_EQ(r.get16(low_table + 7 * 2), 0x0A3Eu);
    CHECK_EQ(r.refreshes.size(), 1u);
    r.intuition.flush_palette();
    CHECK_EQ(r.refreshes.size(), 2u);

    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 7;
    r.cpu().d[1] = 1;
    r.cpu().a[1] = out_table;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::GetRGB32), ExitReason::Halt);
    CHECK_EQ(r.get32(out_table), 0xAAAAAAAAu);
    CHECK_EQ(r.get32(out_table + 4), 0x33333333u);
    CHECK_EQ(r.get32(out_table + 8), 0xEEEEEEEEu);

    // LoadRGB32 multi-entry table with two blocks and a 0-count terminator.
    u32 p = load_table;
    r.put32(p, (2u << 16) | 10u);
    p += 4;
    r.put32(p, 0x10000000u);
    r.put32(p + 4, 0x20000000u);
    r.put32(p + 8, 0x30000000u);
    p += 12;
    r.put32(p, (255u << 24) | 0x112233u);
    r.put32(p + 4, 0x80000000u);
    r.put32(p + 8, 0x456789ABu);
    p += 12;
    r.put32(p, (1u << 16) | 200u);
    p += 4;
    r.put32(p, 0x6D000000u);
    r.put32(p + 4, 0x9B000000u);
    r.put32(p + 8, 0xE0000000u);
    p += 12;
    r.put32(p, 0u);

    r.cpu().a[0] = vp;
    r.cpu().a[1] = load_table;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::LoadRGB32), ExitReason::Halt);
    CHECK_EQ(r.refreshes.size(), 2u);
    r.intuition.flush_palette();
    CHECK_EQ(r.refreshes.size(), 3u);

    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 10;
    r.cpu().d[1] = 2;
    r.cpu().a[1] = out_table;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::GetRGB32), ExitReason::Halt);
    CHECK_EQ(r.get32(out_table), 0x10101010u);
    CHECK_EQ(r.get32(out_table + 4), 0x20202020u);
    CHECK_EQ(r.get32(out_table + 8), 0x30303030u);
    CHECK_EQ(r.get32(out_table + 12), 0xFFFFFFFFu);
    CHECK_EQ(r.get32(out_table + 16), 0x80808080u);
    CHECK_EQ(r.get32(out_table + 20), 0x45454545u);

    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 200;
    r.cpu().d[1] = 1;
    r.cpu().a[1] = out_table;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::GetRGB32), ExitReason::Halt);
    CHECK_EQ(r.get32(out_table), 0x6D6D6D6Du);
    CHECK_EQ(r.get32(out_table + 4), 0x9B9B9B9Bu);
    CHECK_EQ(r.get32(out_table + 8), 0xE0E0E0E0u);

    // SetRGB32CM on the front screen's ColorMap also defers until flush.
    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 255;
    r.cpu().d[1] = 0x5A000000u;
    r.cpu().d[2] = 0x6B000000u;
    r.cpu().d[3] = 0x7C000000u;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32CM), ExitReason::Halt);
    CHECK_EQ(r.refreshes.size(), 3u);
    r.intuition.flush_palette();
    CHECK_EQ(r.refreshes.size(), 4u);
    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 255;
    r.cpu().d[1] = 1;
    r.cpu().a[1] = out_table;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::GetRGB32), ExitReason::Halt);
    CHECK_EQ(r.get32(out_table), 0x5A5A5A5Au);
    CHECK_EQ(r.get32(out_table + 4), 0x6B6B6B6Bu);
    CHECK_EQ(r.get32(out_table + 8), 0x7C7C7C7Cu);
}

void rect_fill_and_text_pen_200_on_8_planes() {
    GfxRig r;
    const u32 width = 64;
    const u32 height = 32;
    const u32 row_bytes = (width + 15) / 16 * 2;
    const u32 plane_bytes = row_bytes * height;
    u32 planes[8]{};
    for (u32 plane = 0; plane < 8; ++plane) {
        planes[plane] = r.exec.alloc(plane_bytes, MEMF_CHIP | MEMF_CLEAR);
        CHECK(planes[plane] != 0);
    }
    r.cpu().a[0] = kBitmap;
    r.cpu().d[0] = 8;
    r.cpu().d[1] = width;
    r.cpu().d[2] = height;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitBitMap), ExitReason::Halt);
    for (u32 plane = 0; plane < 8; ++plane) {
        r.put32(kBitmap + bm_Planes + plane * 4, planes[plane]);
    }
    r.cpu().a[1] = kRastPort;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    r.put32(kRastPort + rp_BitMap, kBitmap);

    r.cpu().a[1] = kRastPort;
    r.cpu().d[0] = 200;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    rect_fill(r, kRastPort, 2, 2, 10, 10);
    for (i32 y = 2; y <= 10; ++y) {
        for (i32 x = 2; x <= 10; ++x) {
            CHECK_EQ(read_pixel(r, kRastPort, x, y), 200u);
        }
    }
    CHECK_EQ(read_pixel(r, kRastPort, 1, 2), 0u);
    CHECK_EQ(read_pixel(r, kRastPort, 11, 2), 0u);

    // 200 == 0b11001000: planes 3, 6, 7 have bit set at (5, 5); others clear.
    const u32 at = 5 * row_bytes + (5 >> 3);
    const u32 mask = 0x80u >> (5 & 7);
    for (u32 plane = 0; plane < 8; ++plane) {
        const bool want = ((200u >> plane) & 1u) != 0;
        CHECK_EQ((r.get8(planes[plane] + at) & mask) != 0, want);
    }

    r.graphics.set_pens(kRastPort, 200, 5, JAM2);
    move(r, kRastPort, 16, 20);
    r.str(kStrings, "H");
    r.cpu().a[1] = kRastPort;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 1;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Text), ExitReason::Halt);
    const u8* rows = glyph('H');
    for (u32 row = 0; row < 8; ++row) {
        for (u32 col = 0; col < 8; ++col) {
            const u32 expected = ((rows[row] >> col) & 1) != 0 ? 200u : 5u;
            CHECK_EQ(read_pixel(r, kRastPort, 16 + static_cast<i32>(col),
                                14 + static_cast<i32>(row)),
                     expected);
        }
    }
}

void clear_screen_clears_from_cp() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.graphics.set_pens(s.rp, 3, 1, JAM2);
    r.graphics.rect_fill(s.rp, 0, 0, 639, 399);
    move(r, s.rp, 100, 10);
    r.cpu().a[1] = s.rp;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::ClearScreen), ExitReason::Halt);
    CHECK_EQ(read_pixel(r, s.rp, 0, 9), 3u);
    CHECK_EQ(read_pixel(r, s.rp, 0, 10), 3u);
    CHECK_EQ(read_pixel(r, s.rp, 99, 10), 3u);
    CHECK_EQ(read_pixel(r, s.rp, 100, 10), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 639, 10), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 0, 11), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 639, 399), 1u);
}

void refresh_called_only_for_front_screen_bitmap() {
    GfxRig r;
    const Surface s = make_surface(r, kScreen + sc_BitMap, kRastPort);
    r.display.front_screen = kScreen;
    r.graphics.set_pens(s.rp, 2, 0, JAM2);
    CHECK(r.refreshes.empty());

    rect_fill(r, s.rp, 2, 3, 7, 9);
    CHECK_EQ(r.refreshes.size(), 1u);
    CHECK_EQ(r.refreshes[0].x0, 2);
    CHECK_EQ(r.refreshes[0].y0, 3);
    CHECK_EQ(r.refreshes[0].x1, 7);
    CHECK_EQ(r.refreshes[0].y1, 9);
    CHECK_EQ(r.refreshed_screens.size(), 1u);
    CHECK_EQ(r.refreshed_screens[0], kScreen);

    // The changed rectangle is the clipped one.
    rect_fill(r, s.rp, 630, 390, 700, 500);
    CHECK_EQ(r.refreshes.size(), 2u);
    CHECK_EQ(r.refreshes[1].x0, 630);
    CHECK_EQ(r.refreshes[1].y0, 390);
    CHECK_EQ(r.refreshes[1].x1, 639);
    CHECK_EQ(r.refreshes[1].y1, 399);

    // Another screen, or no front screen, means no refresh.
    r.display.front_screen = kScreen + 0x100;
    rect_fill(r, s.rp, 0, 0, 1, 1);
    CHECK_EQ(r.refreshes.size(), 2u);
    r.display.front_screen = 0;
    rect_fill(r, s.rp, 0, 0, 1, 1);
    CHECK_EQ(r.refreshes.size(), 2u);

    // Moving and setting pens draw nothing.
    move(r, s.rp, 5, 5);
    r.cpu().a[1] = s.rp;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    CHECK_EQ(r.refreshes.size(), 2u);
}

void text_clips_to_layer_bounds() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.put16(kLayer + ly_MinX, 100);
    r.put16(kLayer + ly_MinY, 50);
    r.put16(kLayer + ly_MaxX, 179);
    r.put16(kLayer + ly_MaxY, 99);
    r.put32(s.rp + rp_Layer, kLayer);
    r.graphics.set_pens(s.rp, 1, 0, JAM2);
    const std::vector<u8> before = plane_snapshot(r, s);

    move(r, s.rp, 0, 6);
    r.str(kStrings, "HHHH");
    r.cpu().a[1] = s.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 4;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Text), ExitReason::Halt);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 32u);

    const u8* rows = glyph('H');
    for (u32 row = 0; row < 8; ++row) {
        for (u32 col = 0; col < 8; ++col) {
            const u32 expected = ((rows[row] >> col) & 1) != 0 ? 1u : 0u;
            CHECK_EQ(read_pixel(r, s.rp, static_cast<i32>(col), static_cast<i32>(row)), expected);
        }
    }
    CHECK_EQ(read_pixel(r, s.rp, -1, 0), 0xFFFFFFFFu);
    CHECK_EQ(read_pixel(r, s.rp, 80, 0), 0xFFFFFFFFu);
    CHECK_EQ(read_pixel(r, s.rp, 0, -1), 0xFFFFFFFFu);
    CHECK_EQ(read_pixel(r, s.rp, 0, 50), 0xFFFFFFFFu);

    // Only the four glyph cells in the layer changed: bitmap x 100..131,
    // y 50..57, bytes 12..16.
    const std::vector<u8> after = plane_snapshot(r, s);
    for (u32 plane = 0; plane < 2; ++plane) {
        for (u32 row = 0; row < kHeight; ++row) {
            for (u32 byte = 0; byte < kBytesPerRow; ++byte) {
                const u32 at = plane * kPlaneBytes + row * kBytesPerRow + byte;
                if (after[at] != before[at]) {
                    CHECK(row >= 50 && row <= 57);
                    CHECK(byte >= 12 && byte <= 16);
                }
            }
        }
    }
}

void unimplemented_vector_ends_run() {
    GfxRig r;
    CHECK_EQ(r.call(Base::Graphics, 0x2A0), ExitReason::Unimplemented);
    CHECK(logged("graphics: unimplemented LVO -672"));
    CHECK_EQ(r.cpu().pc, r.graphics.graphics_base() - 0x2A0);
}

void null_rastport_faults_cleanly() {
    GfxRig r;
    std::vector<u8> before;
    for (u32 i = 0; i < 100; ++i) {
        before.push_back(static_cast<u8>(r.get8(i)));
    }

    r.cpu().a[1] = 0;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetAPen), ExitReason::Halt);
    r.cpu().a[1] = 0;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 1;
    r.cpu().d[3] = 1;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::Halt);
    r.cpu().a[1] = 0;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::ReadPixel), 0xFFFFFFFFu);

    for (u32 i = 0; i < 100; ++i) {
        CHECK_EQ(r.get8(i), before[i]);
    }
    CHECK(gfx_test::g_lines.empty());
}

Surface make_superbitmap_layer(GfxRig& r) {
    const Surface s = make_sized_surface(r, kBitmap, kRastPort, 100, 50);
    r.put16(kLayer + ly_MinX, 100);
    r.put16(kLayer + ly_MinY, 50);
    r.put16(kLayer + ly_MaxX, 199);
    r.put16(kLayer + ly_MaxY, 99);
    r.put32(kLayer + ly_SuperBitMap, s.bitmap);
    r.put32(s.rp + rp_Layer, kLayer);
    r.graphics.set_pens(s.rp, 1, 0, JAM2);
    return s;
}

void superbitmap_layer_draws_window_relative() {
    GfxRig r;
    const Surface screen = make_surface(r, kScreen + sc_BitMap, kData + 0x280);
    r.display.front_screen = kScreen;
    const std::vector<u8> screen_before = plane_snapshot(r, screen);
    const Surface s = make_superbitmap_layer(r);

    rect_fill(r, s.rp, 0, 0, 9, 9);
    for (i32 y = 0; y < 50; ++y) {
        for (i32 x = 0; x < 100; ++x) {
            const u32 expected = (x <= 9 && y <= 9) ? 1u : 0u;
            CHECK_EQ(read_pixel(r, s.rp, x, y), expected);
        }
    }
    CHECK(plane_snapshot(r, screen) == screen_before);
}

void superbitmap_layer_clips_to_size() {
    GfxRig r;
    const Surface s = make_superbitmap_layer(r);

    rect_fill(r, s.rp, 90, 40, 120, 60);
    for (i32 y = 0; y < 50; ++y) {
        for (i32 x = 0; x < 100; ++x) {
            const u32 expected = (x >= 90 && y >= 40) ? 1u : 0u;
            CHECK_EQ(read_pixel(r, s.rp, x, y), expected);
        }
        for (u32 x = 100; x < 112; ++x) {
            const u32 byte = static_cast<u32>(y) * 14 + (x >> 3);
            const u32 mask = 0x80u >> (x & 7);
            CHECK_EQ(r.get8(s.plane0 + byte) & mask, 0u);
            CHECK_EQ(r.get8(s.plane1 + byte) & mask, 0u);
        }
    }
    CHECK_EQ(read_pixel(r, s.rp, 100, 40), 0xFFFFFFFFu);
    CHECK_EQ(read_pixel(r, s.rp, 90, 50), 0xFFFFFFFFu);
}

struct RecordedDamage {
    u32 bitmap;
    Rect rect;
};

void record_damage(void* ctx, Machine&, u32 bitmap, const Rect& rect) {
    static_cast<std::vector<RecordedDamage>*>(ctx)->push_back({bitmap, rect});
}

void damage_sink_gets_private_bitmap_rect() {
    GfxRig r;
    make_surface(r, kScreen + sc_BitMap, kData + 0x280);
    r.display.front_screen = kScreen;
    std::vector<RecordedDamage> damages;
    r.display.damage = DamageSink{record_damage, &damages};
    const Surface s = make_superbitmap_layer(r);

    rect_fill(r, s.rp, 0, 0, 9, 9);
    CHECK(r.refreshes.empty());
    CHECK_EQ(damages.size(), 1u);
    CHECK_EQ(damages[0].bitmap, s.bitmap);
    CHECK_EQ(damages[0].rect.x0, 0);
    CHECK_EQ(damages[0].rect.y0, 0);
    CHECK_EQ(damages[0].rect.x1, 9);
    CHECK_EQ(damages[0].rect.y1, 9);
}

void superbitmap_bad_pointer_is_bus_error() {
    GfxRig r;
    r.cpu().a[1] = kRastPort;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    r.put16(kLayer + ly_MinX, 100);
    r.put16(kLayer + ly_MinY, 50);
    r.put16(kLayer + ly_MaxX, 199);
    r.put16(kLayer + ly_MaxY, 99);
    r.put32(kLayer + ly_SuperBitMap, kGuestBytes);
    r.put32(kRastPort + rp_Layer, kLayer);
    r.put32(kRastPort + rp_BitMap, kGuestBytes);

    r.cpu().a[1] = kRastPort;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().d[2] = 9;
    r.cpu().d[3] = 9;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RectFill), ExitReason::BusError);
    CHECK(r.refreshes.empty());
}

void palette_calls_defer_until_flush() {
    GfxRig r;
    const u32 color_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 low_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    CHECK(color_table != 0 && low_table != 0);

    const u32 vp = kScreen + sc_ViewPort;
    r.put16(kScreen + sc_Width, 320);
    r.put16(kScreen + sc_Height, 200);
    r.put32(vp + vp_ColorMap, kColorMap);
    r.put8(kColorMap + cm_Type, COLORMAP_TYPE_V39);
    r.put16(kColorMap + cm_Count, 256);
    r.put32(kColorMap + cm_ColorTable, color_table);
    r.put32(kColorMap + cm_LowColorBits, low_table);
    r.display.front_screen = kScreen;

    for (u32 pen = 0; pen < 256; ++pen) {
        r.cpu().a[0] = vp;
        r.cpu().d[0] = pen;
        r.cpu().d[1] = pen * 0x01010101u;
        r.cpu().d[2] = pen * 0x01010101u;
        r.cpu().d[3] = pen * 0x01010101u;
        CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32), ExitReason::Halt);
    }
    CHECK(r.refreshes.empty());
    CHECK(r.display.palette_dirty);

    r.intuition.flush_palette();
    CHECK_EQ(r.refreshes.size(), 1u);
    CHECK(!r.display.palette_dirty);

    r.intuition.flush_palette();
    CHECK_EQ(r.refreshes.size(), 1u);
}

void set_rgb32_cm_refreshes_front_map_only_at_flush() {
    GfxRig r;
    const u32 front_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 front_low = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 other_map = r.exec.alloc(ColorMap_SIZEOF, MEMF_CHIP | MEMF_CLEAR);
    const u32 other_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 other_low = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    CHECK(front_table != 0 && front_low != 0 && other_map != 0 && other_table != 0 &&
          other_low != 0);

    const u32 vp = kScreen + sc_ViewPort;
    r.put16(kScreen + sc_Width, 320);
    r.put16(kScreen + sc_Height, 200);
    r.put32(vp + vp_ColorMap, kColorMap);
    r.put8(kColorMap + cm_Type, COLORMAP_TYPE_V39);
    r.put16(kColorMap + cm_Count, 256);
    r.put32(kColorMap + cm_ColorTable, front_table);
    r.put32(kColorMap + cm_LowColorBits, front_low);

    r.put8(other_map + cm_Type, COLORMAP_TYPE_V39);
    r.put16(other_map + cm_Count, 256);
    r.put32(other_map + cm_ColorTable, other_table);
    r.put32(other_map + cm_LowColorBits, other_low);
    r.display.front_screen = kScreen;

    r.cpu().a[0] = other_map;
    r.cpu().d[0] = 5;
    r.cpu().d[1] = 0x11000000u;
    r.cpu().d[2] = 0x22000000u;
    r.cpu().d[3] = 0x33000000u;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32CM), ExitReason::Halt);
    CHECK(!r.display.palette_dirty);
    r.intuition.flush_palette();
    CHECK(r.refreshes.empty());

    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = 5;
    r.cpu().d[1] = 0x44000000u;
    r.cpu().d[2] = 0x55000000u;
    r.cpu().d[3] = 0x66000000u;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32CM), ExitReason::Halt);
    CHECK(r.refreshes.empty());
    CHECK(r.display.palette_dirty);
    r.intuition.flush_palette();
    CHECK_EQ(r.refreshes.size(), 1u);
}

void load_rgb32_caps_colormap_count_at_256() {
    GfxRig r;
    constexpr u32 kAllocEntries = 600;
    const u32 color_table = r.exec.alloc(kAllocEntries * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 low_table = r.exec.alloc(kAllocEntries * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 load_table = r.exec.alloc(512, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(color_table != 0 && low_table != 0 && load_table != 0);

    for (u32 i = 0; i < kAllocEntries; ++i) {
        r.put16(color_table + i * 2, 0xBEEFu);
        r.put16(low_table + i * 2, 0xBEEFu);
    }

    const u32 vp = kScreen + sc_ViewPort;
    r.put32(vp + vp_ColorMap, kColorMap);
    r.put8(kColorMap + cm_Type, COLORMAP_TYPE_V39);
    r.put16(kColorMap + cm_Count, 65535);
    r.put32(kColorMap + cm_ColorTable, color_table);
    r.put32(kColorMap + cm_LowColorBits, low_table);

    u32 p = load_table;
    r.put32(p, (10u << 16) | 250u);
    p += 4;
    for (u32 i = 0; i < 10; ++i) {
        r.put32(p, 0x12000000u);
        r.put32(p + 4, 0x34000000u);
        r.put32(p + 8, 0x56000000u);
        p += 12;
    }
    r.put32(p, (4u << 16) | 256u);
    p += 4;
    for (u32 i = 0; i < 4; ++i) {
        r.put32(p, 0x78000000u);
        r.put32(p + 4, 0x9A000000u);
        r.put32(p + 8, 0xBC000000u);
        p += 12;
    }
    r.put32(p, 0u);

    r.cpu().a[0] = vp;
    r.cpu().a[1] = load_table;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::LoadRGB32), ExitReason::Halt);

    for (u32 i = 250; i < 256; ++i) {
        CHECK_EQ(r.get16(color_table + i * 2), 0x0135u);
        CHECK_EQ(r.get16(low_table + i * 2), 0x0246u);
    }
    for (u32 i = 256; i < kAllocEntries; ++i) {
        CHECK_EQ(r.get16(color_table + i * 2), 0xBEEFu);
        CHECK_EQ(r.get16(low_table + i * 2), 0xBEEFu);
    }
}

}

int main() {
    init_bitmap_fills_the_fields();
    init_rastport_sets_default_pens();
    set_apen_bpen_drmd_store_fields();
    move_sets_cp();
    rect_fill_fills_pen_and_clips();
    write_then_read_pixel_vectors();
    draw_moves_cp_and_sets_pixels();
    text_draws_glyphs_advances_cp_and_returns_nothing_unsafe();
    text_length_is_8_per_char();
    set_font_updates_txheight_txbaseline();
    open_font_topaz_8_returns_the_builtin_font_and_other_names_give_0();
    scroll_raster_moves_pixels();
    set_rgb4_and_load_rgb4_write_the_colour_table();
    set_rgb32_get_rgb32_and_load_rgb32_v39_palette();
    rect_fill_and_text_pen_200_on_8_planes();
    clear_screen_clears_from_cp();
    refresh_called_only_for_front_screen_bitmap();
    text_clips_to_layer_bounds();
    unimplemented_vector_ends_run();
    null_rastport_faults_cleanly();
    superbitmap_layer_draws_window_relative();
    superbitmap_layer_clips_to_size();
    damage_sink_gets_private_bitmap_rect();
    superbitmap_bad_pointer_is_bus_error();
    palette_calls_defer_until_flush();
    set_rgb32_cm_refreshes_front_map_only_at_flush();
    load_rgb32_caps_colormap_count_at_256();
    return check::finish("test_graphics");
}
