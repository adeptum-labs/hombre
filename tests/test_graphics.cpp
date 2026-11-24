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
#include "libs/chrome_palette.hpp"
#include "libs/display_modes.hpp"
#include "libs/rom_fonts.hpp"
#include "libs/text_blend.hpp"
#include "libs/utility.hpp"

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
constexpr u32 kAreaInfo = kData + 0x900;
constexpr u32 kAreaTable = kData + 0xA00;
constexpr u32 kExtent = kData + 0xB00;
constexpr u32 kTmpRas = kData + 0xC00;
constexpr u32 kTmpBuffer = kData + 0xD00;
constexpr u32 kWidth = 640;
constexpr u32 kHeight = 400;
constexpr u32 kBytesPerRow = 80;
constexpr u32 kPlaneBytes = kBytesPerRow * kHeight;

// struct AreaInfo
constexpr u32 ai_VctrPtr = 4;
constexpr u32 ai_FlagPtr = 12;
constexpr u32 ai_Count = 16;
constexpr u32 kAreaMax = 16;

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

void init_area(GfxRig& r, u32 rp, u32 max) {
    r.cpu().a[0] = kAreaInfo;
    r.cpu().a[1] = kAreaTable;
    r.cpu().d[0] = max;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitArea), ExitReason::Halt);
    r.put32(rp + rp_AreaInfo, kAreaInfo);
}

u32 area_move(GfxRig& r, u32 rp, i32 x, i32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    return r.result(Base::Graphics, graphics_lvo::AreaMove);
}

u32 area_draw(GfxRig& r, u32 rp, i32 x, i32 y) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(x);
    r.cpu().d[1] = static_cast<u32>(y);
    return r.result(Base::Graphics, graphics_lvo::AreaDraw);
}

void area_end(GfxRig& r, u32 rp) {
    r.cpu().a[1] = rp;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::AreaEnd), ExitReason::Halt);
}

u32 area_ellipse(GfxRig& r, u32 rp, i32 cx, i32 cy, i32 rx, i32 ry) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = static_cast<u32>(cx);
    r.cpu().d[1] = static_cast<u32>(cy);
    r.cpu().d[2] = static_cast<u32>(rx);
    r.cpu().d[3] = static_cast<u32>(ry);
    return r.result(Base::Graphics, graphics_lvo::AreaEllipse);
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

void rect_fill_complements_in_complement_mode() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.graphics.set_pens(s.rp, 2, 0, JAM2);
    rect_fill(r, s.rp, 0, 0, 3, 3);
    r.graphics.set_pens(s.rp, 1, 0, COMPLEMENT);
    rect_fill(r, s.rp, 2, 2, 5, 5);
    CHECK_EQ(read_pixel(r, s.rp, 0, 0), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 2, 2), 1u);  // 2 turned over in both planes
    CHECK_EQ(read_pixel(r, s.rp, 5, 5), 3u);  // 0 turned over
    CHECK_EQ(read_pixel(r, s.rp, 6, 6), 0u);
    // Twice puts every pixel back.
    rect_fill(r, s.rp, 2, 2, 5, 5);
    CHECK_EQ(read_pixel(r, s.rp, 2, 2), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 5, 5), 0u);
    // The RastPort's mask limits it to the planes it lets through.
    r.put8(s.rp + rp_Mask, 1);
    rect_fill(r, s.rp, 0, 0, 0, 0);
    CHECK_EQ(read_pixel(r, s.rp, 0, 0), 3u);
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

// OpenFont finds a font in memory by its name, whatever the letter case,
// and gives the one nearest the height asked for; a name no font in
// memory has gives 0.
void open_font_finds_the_nearest_font_of_the_name() {
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
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), r.graphics.topaz_font());

    r.put16(kTextAttr + ta_YSize, 8);
    r.str(kStrings, "chargen.font");
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), 0u);
    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), 0u);
}

// AddFont puts a font a program loaded among the fonts in memory, with an
// extension; OpenFont then finds it and counts who has it open, AskFont
// tells what a RastPort's font is, and RemFont takes it out again.
void add_font_and_rem_font_keep_the_fonts_in_memory() {
    constexpr u32 kFpfRemoved = FPF_REMOVED;
    constexpr u32 kFsfTagged = 0x40;
    GfxRig r;
    const u32 small = r.exec.alloc(TextFont_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 large = r.exec.alloc(TextFont_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(small != 0 && large != 0);
    r.str(kStrings, "ruby.font");
    r.str(kStrings + 0x20, "Ruby.font");
    r.put32(small + LN_NAME, kStrings);
    r.put16(small + tf_YSize, 8);
    r.put16(small + tf_Accessors, 7);
    r.put8(small + tf_Flags, 0x02 | kFpfRemoved);  // FPF_DISKFONT
    r.put32(large + LN_NAME, kStrings);
    r.put16(large + tf_YSize, 12);
    r.put8(large + tf_Flags, 0x02);

    r.put32(kTextAttr + ta_Name, kStrings + 0x20);
    r.put16(kTextAttr + ta_YSize, 12);
    r.put8(kTextAttr + ta_Style, 0);
    r.put8(kTextAttr + ta_Flags, 0);
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), 0u);

    r.cpu().a[1] = small;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::AddFont), ExitReason::Halt);
    r.cpu().a[1] = large;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::AddFont), ExitReason::Halt);
    CHECK_EQ(r.get16(small + tf_Accessors), 0u);
    CHECK_EQ(r.get8(small + tf_Flags), 0x02u);
    CHECK_EQ(r.get8(small + LN_TYPE), 12u);  // NT_FONT
    // Extended: FSF_TAGGED, and an extension that points back at the font.
    CHECK_EQ(r.get8(small + tf_Style) & kFsfTagged, kFsfTagged);
    const u32 extension = r.get32(small + mn_ReplyPort);
    CHECK(extension != 0);
    CHECK_EQ(r.get16(extension), 0xF100u);
    CHECK_EQ(r.get32(extension + 4), small);

    // The fonts in memory: the two new ones before topaz.
    const u32 fonts = r.graphics.graphics_base() + 140;
    CHECK_EQ(r.get32(fonts + LH_HEAD), large);
    CHECK_EQ(r.get32(large + LN_SUCC), small);
    CHECK_EQ(r.get32(small + LN_SUCC), r.graphics.topaz_font());
    CHECK_EQ(r.get32(r.graphics.topaz_font() + LN_PRED), small);

    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), large);
    CHECK_EQ(r.get16(large + tf_Accessors), 1u);
    r.put16(kTextAttr + ta_YSize, 9);
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), small);
    CHECK_EQ(r.get16(small + tf_Accessors), 1u);

    // A font the size asked for is an exact match, extended or not.
    r.put16(kTextAttr + ta_YSize, 8);
    r.cpu().a[0] = kTextAttr;
    r.cpu().a[1] = small + tf_YSize - ta_YSize;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::WeighTAMatch), 32767u);

    const u32 rp = r.exec.alloc(RastPort_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(rp != 0);
    r.graphics.set_font(rp, large);
    r.put32(kTextAttr + ta_Name, 0);
    r.cpu().a[1] = rp;
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::AskFont), ExitReason::Halt);
    CHECK_EQ(r.get32(kTextAttr + ta_Name), kStrings);
    CHECK_EQ(r.get16(kTextAttr + ta_YSize), 12u);
    CHECK_EQ(r.get8(kTextAttr + ta_Flags), 0x02u);

    r.cpu().a[1] = large;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::CloseFont), ExitReason::Halt);
    CHECK_EQ(r.get16(large + tf_Accessors), 0u);
    r.cpu().a[1] = large;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::CloseFont), ExitReason::Halt);
    CHECK_EQ(r.get16(large + tf_Accessors), 0u);

    // Removed, the large font is found no more and the small one answers.
    r.cpu().a[1] = large;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RemFont), ExitReason::Halt);
    CHECK_EQ(r.get8(large + tf_Flags) & kFpfRemoved, kFpfRemoved);
    CHECK_EQ(r.get32(fonts + LH_HEAD), small);
    CHECK_EQ(r.get32(small + LN_PRED), fonts + LH_HEAD);
    r.put32(kTextAttr + ta_Name, kStrings + 0x20);
    r.put16(kTextAttr + ta_YSize, 12);
    r.put8(kTextAttr + ta_Flags, 0);
    r.cpu().a[0] = kTextAttr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenFont), small);
    // A second RemFont leaves the list alone.
    r.cpu().a[1] = large;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::RemFont), ExitReason::Halt);
    CHECK_EQ(r.get32(fonts + LH_HEAD), small);
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

// WeighTAMatch rates the target by height, style and flags, never by name.
void weigh_ta_match_rates_by_size_style_and_flags() {
    GfxRig r;
    const u32 want = r.exec.alloc(16, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 have = r.exec.alloc(16, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(want != 0 && have != 0);
    r.put16(want + ta_YSize, 8);
    r.put16(have + ta_YSize, 8);
    const auto weigh = [&] {
        r.cpu().a[0] = want;
        r.cpu().a[1] = have;
        r.cpu().a[2] = 0;
        return r.result(Base::Graphics, graphics_lvo::WeighTAMatch);
    };
    CHECK_EQ(weigh(), 32767u);
    r.put8(have + ta_Style, 0x02);  // FSF_BOLD
    const u32 bold = weigh();
    CHECK(bold < 32767u && bold > 0u);
    r.put8(have + ta_Style, 0);
    r.put16(have + ta_YSize, 11);
    const u32 taller = weigh();
    CHECK(taller < bold && taller > 0u);
    r.put16(have + ta_YSize, 9);
    CHECK(weigh() > taller);
    r.cpu().a[0] = 0;
    r.cpu().a[1] = have;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::WeighTAMatch), 0u);
}

// SetSoftStyle sets the drawn styles enable lets through, never one the
// font has of its own.
void set_soft_style_keeps_the_drawn_styles() {
    GfxRig r;
    const u32 rp = r.exec.alloc(RastPort_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 font = r.exec.alloc(64, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(rp != 0 && font != 0);
    r.put32(rp + rp_Font, font);
    r.put8(font + tf_Style, 0x02);  // a bold font
    r.cpu().a[1] = rp;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::AskSoftStyle), 0x05u);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 0x07;
    r.cpu().d[1] = 0x01;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::SetSoftStyle), 0x03u);
    CHECK_EQ(r.get8(rp + rp_AlgoStyle), 0x01u);
    r.cpu().a[1] = rp;
    r.cpu().d[0] = 0x00;
    r.cpu().d[1] = 0xFF;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::SetSoftStyle), 0x02u);
    CHECK_EQ(r.get8(rp + rp_AlgoStyle), 0x00u);
}

// GfxBase holds the list of the fonts in memory: topaz 8 and hasklig in
// three sizes, all from the ROM and each with its glyph tables.
void gfx_base_lists_the_memory_fonts() {
    GfxRig r;
    const u32 list = r.base(Base::Graphics) + 140;  // gb_TextFonts
    struct Expected {
        const char* name;
        u32 y_size;
        u32 x_size;
    };
    const Expected expected[] = {
        {"topaz.font", 8, 8}, {"hasklig.font", 8, 6}, {"hasklig.font", 11, 6},
        {"hasklig.font", 13, 7}};
    u32 font = r.get32(list);
    u32 previous = list;
    for (const Expected& e : expected) {
        CHECK(font != 0 && (font & 1) == 0);
        CHECK_EQ(r.get32(font + LN_PRED), previous);
        CHECK_EQ(r.get8(font + LN_TYPE), 12u);  // NT_FONT
        CHECK(r.read_str(r.get32(font + LN_NAME)) == e.name);
        CHECK_EQ(r.get16(font + tf_YSize), e.y_size);
        CHECK_EQ(r.get16(font + tf_XSize), e.x_size);
        CHECK_EQ(r.get8(font + tf_Flags), FPF_ROMFONT | FPF_DESIGNED);
        CHECK_EQ(r.get8(font + tf_LoChar), 32u);
        CHECK_EQ(r.get8(font + tf_HiChar), 255u);
        CHECK(r.get16(font + tf_Baseline) < e.y_size);
        CHECK(r.get16(font + tf_Modulo) * 8u >= 225 * e.x_size);
        CHECK(r.get32(font + tf_CharData) != 0);
        CHECK_EQ(r.get32(font + tf_CharSpace), 0u);
        // Every glyph is a cell of the font's width, the cells side by side.
        const u32 loc = r.get32(font + tf_CharLoc);
        CHECK_EQ(r.get16(loc + 4 * 224), 224 * e.x_size);
        CHECK_EQ(r.get16(loc + 4 * 224 + 2), e.x_size);
        previous = font;
        font = r.get32(font + LN_SUCC);
    }
    CHECK_EQ(font, list + 4);
    CHECK_EQ(r.get32(list + 8), previous);
}

// The topaz TextFont holds the built-in glyphs in its own tables, the
// question mark standing in for the characters it has none for.
void topaz_font_holds_the_built_in_glyphs() {
    GfxRig r;
    const u32 font = r.graphics.topaz_font();
    const u32 data = r.get32(font + tf_CharData);
    const u32 modulo = r.get16(font + tf_Modulo);
    const u32 loc = r.get32(font + tf_CharLoc);
    const auto holds = [&](u32 index, const u8* rows) {
        CHECK_EQ(r.get16(loc + index * 4), index * 8);
        for (u32 row = 0; row < 8; ++row) {
            for (u32 col = 0; col < 8; ++col) {
                const u32 bit = (r.get8(data + row * modulo + index) >> (7 - col)) & 1;
                CHECK_EQ(bit, static_cast<u32>((rows[row] >> col) & 1));
            }
        }
    };
    holds('H' - 32, glyph('H'));
    holds(0xE5 - 32, glyph(static_cast<char>(0xE5)));
    holds(0x80 - 32, glyph('?'));
    holds(224, glyph('?'));
}

// A proportional font of five rows with the glyphs A, B and C and a box
// for every other character. Each glyph has its own width, space and
// kerning, so nothing about it is a multiple of eight.
struct TinyGlyph {
    const char* rows[5];
    i32 space;
    i32 kern;
};

constexpr TinyGlyph kTinyGlyphs[4] = {
    {{"###", "#.#", "###", "#.#", "#.#"}, 4, 0},
    {{"####.", "#...#", "####.", "#...#", "####."}, 6, 1},
    {{"##", "#.", "#.", "#.", "##"}, 3, 0},
    {{"####", "#..#", "#..#", "#..#", "####"}, 5, 0},
};
constexpr u32 kTinyRows = 5;
constexpr u32 kTinyBaseline = 3;
constexpr u32 kTinyModulo = 2;

u32 tiny_width(const TinyGlyph& g) { return static_cast<u32>(std::strlen(g.rows[0])); }

const TinyGlyph& tiny_glyph(u8 c) { return kTinyGlyphs[c >= 'A' && c <= 'C' ? c - 'A' : 3]; }

u32 make_tiny_font(GfxRig& r) {
    const u32 font = r.exec.alloc(TextFont_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 data = r.exec.alloc(kTinyModulo * kTinyRows, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 loc = r.exec.alloc(4 * 4, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 space = r.exec.alloc(4 * 2, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 kern = r.exec.alloc(4 * 2, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(font != 0 && data != 0 && loc != 0 && space != 0 && kern != 0);
    u32 offset = 0;
    for (u32 i = 0; i < 4; ++i) {
        const TinyGlyph& g = kTinyGlyphs[i];
        for (u32 row = 0; row < kTinyRows; ++row) {
            for (u32 col = 0; col < tiny_width(g); ++col) {
                if (g.rows[row][col] == '#') {
                    const u32 at = data + row * kTinyModulo + (offset + col) / 8;
                    r.put8(at, r.get8(at) | (0x80u >> ((offset + col) % 8)));
                }
            }
        }
        r.put16(loc + i * 4, offset);
        r.put16(loc + i * 4 + 2, tiny_width(g));
        r.put16(space + i * 2, static_cast<u16>(g.space));
        r.put16(kern + i * 2, static_cast<u16>(g.kern));
        offset += tiny_width(g);
    }
    r.put16(font + tf_YSize, kTinyRows);
    r.put8(font + tf_Flags, 0x20);  // FPF_PROPORTIONAL
    r.put16(font + tf_XSize, 5);
    r.put16(font + tf_Baseline, kTinyBaseline);
    r.put16(font + tf_BoldSmear, 1);
    r.put8(font + tf_LoChar, 'A');
    r.put8(font + tf_HiChar, 'C');
    r.put32(font + tf_CharData, data);
    r.put16(font + tf_Modulo, kTinyModulo);
    r.put32(font + tf_CharLoc, loc);
    r.put32(font + tf_CharSpace, space);
    r.put32(font + tf_CharKern, kern);
    return font;
}

// The pixels the tiny font sets for the text, row by row from the top of
// the font, column 0 where the pen starts; spacing is rp_TxSpacing.
struct TinyText {
    std::vector<std::vector<bool>> rows;
    i32 advance = 0;

    bool set(i32 x, i32 row) const {
        return row >= 0 && row < static_cast<i32>(kTinyRows) && x >= 0 && x < 64 &&
               rows[static_cast<u32>(row)][static_cast<u32>(x)];
    }
};

TinyText tiny_text(const char* text, i32 spacing) {
    TinyText t;
    t.rows.assign(kTinyRows, std::vector<bool>(64, false));
    for (const char* c = text; *c != 0; ++c) {
        const TinyGlyph& g = tiny_glyph(static_cast<u8>(*c));
        for (u32 row = 0; row < kTinyRows; ++row) {
            for (u32 col = 0; col < tiny_width(g); ++col) {
                if (g.rows[row][col] == '#') {
                    t.rows[row][static_cast<u32>(t.advance + g.kern) + col] = true;
                }
            }
        }
        t.advance += g.kern + g.space + spacing;
    }
    return t;
}

void set_font(GfxRig& r, u32 rp, u32 font) {
    r.cpu().a[1] = rp;
    r.cpu().a[0] = font;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetFont), ExitReason::Halt);
}

void draw_text(GfxRig& r, u32 rp, const char* text) {
    r.str(kStrings, text);
    r.cpu().a[1] = rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = static_cast<u32>(std::strlen(text));
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Text), ExitReason::Halt);
}

// The hasklig size y_size opened through the OpenFont vector, one accessor
// the caller holds.
u32 open_hasklig(GfxRig& r, u32 y_size) {
    r.str(kStrings + 0x40, "hasklig.font");
    r.put32(kTextAttr + ta_Name, kStrings + 0x40);
    r.put16(kTextAttr + ta_YSize, y_size);
    r.put8(kTextAttr + ta_Style, 0);
    r.put8(kTextAttr + ta_Flags, 0);
    r.cpu().a[0] = kTextAttr;
    const u32 font = r.result(Base::Graphics, graphics_lvo::OpenFont);
    CHECK(font != 0);
    return font;
}

// Text draws each glyph from the font's own tables, the pen moving by the
// glyph's kerning before it and its space after it, and draws the glyph
// past tf_HiChar for a character the font has none for.
void text_draws_a_proportional_font_from_its_tables() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    set_font(r, s.rp, make_tiny_font(r));
    CHECK_EQ(r.get16(s.rp + rp_TxHeight), kTinyRows);
    CHECK_EQ(r.get16(s.rp + rp_TxBaseline), kTinyBaseline);
    rect_fill(r, s.rp, 0, 0, 79, 19);  // pen 1 under the text
    r.graphics.set_pens(s.rp, 2, 0, JAM1);
    move(r, s.rp, 13, 10);
    draw_text(r, s.rp, "ABxCA");
    const TinyText t = tiny_text("ABxCA", 0);
    CHECK_EQ(t.advance, 4 + 7 + 5 + 3 + 4);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), static_cast<u32>(13 + t.advance));
    CHECK_EQ(r.get16(s.rp + rp_cp_y), 10u);
    for (i32 y = 0; y < 20; ++y) {
        for (i32 x = 0; x < 80; ++x) {
            const bool set = t.set(x - 13, y - (10 - static_cast<i32>(kTinyBaseline)));
            CHECK_EQ(read_pixel(r, s.rp, x, y), set ? 2u : 1u);
        }
    }
}

// The system's own text keeps to the built-in font and its 8 pixel cells
// in a RastPort a program gave another font.
void system_text_keeps_the_built_in_font() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    set_font(r, s.rp, make_tiny_font(r));
    r.put8(s.rp + rp_AlgoStyle, FSF_BOLD);
    r.graphics.set_pens(s.rp, 1, 0, JAM2);
    move(r, s.rp, 8, 8);
    CHECK_EQ(r.graphics.text(s.rp, reinterpret_cast<const u8*>("H"), 1), 8u);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 16u);
    const u8* rows = glyph('H');
    for (u32 row = 0; row < 8; ++row) {
        for (u32 col = 0; col < 8; ++col) {
            CHECK_EQ(read_pixel(r, s.rp, 8 + static_cast<i32>(col), 2 + static_cast<i32>(row)),
                     static_cast<u32>((rows[row] >> col) & 1));
        }
    }
}

// JAM2 fills the rest of the text's rectangle with the background pen,
// INVERSVID swaps the two, COMPLEMENT turns the set bits over, and
// rp_TxSpacing widens every character.
void text_draw_modes_and_spacing_with_a_proportional_font() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    set_font(r, s.rp, make_tiny_font(r));
    r.put16(s.rp + rp_TxSpacing, 2);
    const TinyText t = tiny_text("CBA", 2);
    CHECK_EQ(t.advance, 5 + 9 + 6);
    const auto set_at = [&](i32 x, i32 y) {
        return t.set(x - 21, y - (10 - static_cast<i32>(kTinyBaseline)));
    };
    const auto in_box = [&](i32 x, i32 y) {
        return x >= 21 && x < 21 + t.advance && y >= 7 && y <= 11;
    };
    for (const u32 mode : {JAM2, JAM2 | INVERSVID, JAM1 | INVERSVID, COMPLEMENT}) {
        r.graphics.set_pens(s.rp, 1, 0, JAM1);
        rect_fill(r, s.rp, 0, 0, 79, 19);
        r.graphics.set_pens(s.rp, 2, 3, mode);
        move(r, s.rp, 21, 10);
        draw_text(r, s.rp, "CBA");
        CHECK_EQ(r.get16(s.rp + rp_cp_x), static_cast<u32>(21 + t.advance));
        for (i32 y = 0; y < 20; ++y) {
            for (i32 x = 0; x < 80; ++x) {
                u32 expected = 1;
                if (mode == JAM2 && in_box(x, y)) {
                    expected = set_at(x, y) ? 2 : 3;
                } else if (mode == (JAM2 | INVERSVID) && in_box(x, y)) {
                    expected = set_at(x, y) ? 3 : 2;
                } else if (mode == (JAM1 | INVERSVID) && in_box(x, y)) {
                    expected = set_at(x, y) ? 1 : 2;
                } else if (mode == COMPLEMENT && set_at(x, y)) {
                    expected = 1 ^ 2;
                }
                CHECK_EQ(read_pixel(r, s.rp, x, y), expected);
            }
        }
    }
}

// TextLength, TextExtent and TextFit measure with the font's own widths.
void text_is_measured_with_the_fonts_own_widths() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    set_font(r, s.rp, make_tiny_font(r));
    r.str(kStrings, "BACx");
    r.cpu().a[1] = s.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 4;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextLength), 7u + 4 + 3 + 5);
    r.cpu().a[1] = s.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 3;
    r.cpu().a[2] = kExtent;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::TextExtent), ExitReason::Halt);
    CHECK_EQ(r.get16(kExtent), 14u);
    CHECK_EQ(r.get16(kExtent + 2), kTinyRows);
    CHECK_EQ(r.get16(kExtent + 4), 0u);
    CHECK_EQ(r.get16(kExtent + 6), 0xFFFDu);  // three rows above the baseline
    CHECK_EQ(r.get16(kExtent + 8), 13u);
    CHECK_EQ(r.get16(kExtent + 10), 1u);
    // The bold style smears a pixel past the last column.
    r.put8(s.rp + rp_AlgoStyle, FSF_BOLD);
    r.cpu().a[1] = s.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 3;
    r.cpu().a[2] = kExtent;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::TextExtent), ExitReason::Halt);
    CHECK_EQ(r.get16(kExtent), 14u);
    CHECK_EQ(r.get16(kExtent + 8), 14u);
    r.put8(s.rp + rp_AlgoStyle, 0);

    // "BA" is 11 pixels and "BAC" 14: 13 pixels hold two characters.
    const auto fit = [&](u32 width, u32 height, i32 direction) {
        r.cpu().a[1] = s.rp;
        r.cpu().a[0] = kStrings;
        r.cpu().d[0] = 4;
        r.cpu().a[2] = kExtent;
        r.cpu().a[3] = 0;
        r.cpu().d[1] = static_cast<u32>(direction);
        r.cpu().d[2] = width;
        r.cpu().d[3] = height;
        return r.result(Base::Graphics, graphics_lvo::TextFit);
    };
    CHECK_EQ(fit(13, 5, 1), 2u);
    CHECK_EQ(r.get16(kExtent), 11u);
    CHECK_EQ(fit(14, 5, 1), 3u);
    CHECK_EQ(fit(100, 5, 1), 4u);
    CHECK_EQ(fit(100, 4, 1), 0u);
    // From the end: "x" is 5 pixels, "Cx" 8, "ACx" 12.
    CHECK_EQ(fit(11, 5, -1), 2u);
    CHECK_EQ(r.get16(kExtent), 8u);
    CHECK_EQ(fit(12, 5, -1), 3u);
}

// The drawn styles: bold ORs the text over itself a pixel to the right,
// underlined fills the row below the baseline as wide as the pen moves,
// italic moves the rows above the baseline to the right.
void text_draws_the_algorithmic_styles() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    set_font(r, s.rp, make_tiny_font(r));
    const TinyText t = tiny_text("AC", 0);
    const auto draw_styled = [&](u32 style) {
        r.graphics.set_pens(s.rp, 0, 0, JAM1);
        rect_fill(r, s.rp, 0, 0, 79, 19);
        r.graphics.set_pens(s.rp, 1, 0, JAM1);
        r.put8(s.rp + rp_AlgoStyle, style);
        move(r, s.rp, 30, 10);
        draw_text(r, s.rp, "AC");
        CHECK_EQ(r.get16(s.rp + rp_cp_x), 37u);
    };
    const auto plain = [&](i32 x, i32 row) { return t.set(x, row); };
    draw_styled(FSF_BOLD);
    for (i32 row = 0; row < 5; ++row) {
        for (i32 x = -2; x < 12; ++x) {
            CHECK_EQ(read_pixel(r, s.rp, 30 + x, 7 + row),
                     plain(x, row) || plain(x - 1, row) ? 1u : 0u);
        }
    }
    draw_styled(FSF_UNDERLINED);
    for (i32 row = 0; row < 5; ++row) {
        for (i32 x = -2; x < 12; ++x) {
            const bool line = row == 4 && x >= 0 && x < 7;
            CHECK_EQ(read_pixel(r, s.rp, 30 + x, 7 + row), plain(x, row) || line ? 1u : 0u);
        }
    }
    // Baseline 3: the top two rows move one pixel right, the rest stay.
    draw_styled(FSF_ITALIC);
    for (i32 row = 0; row < 5; ++row) {
        for (i32 x = -2; x < 12; ++x) {
            const i32 shift = row < 2 ? 1 : 0;
            CHECK_EQ(read_pixel(r, s.rp, 30 + x, 7 + row), plain(x - shift, row) ? 1u : 0u);
        }
    }
}

// A font of the ROM opened by name draws its own glyphs at its own cell
// width, and clips like any other text.
void text_draws_hasklig_from_the_rom() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.str(kStrings + 0x40, "hasklig.font");
    r.put32(kTextAttr + ta_Name, kStrings + 0x40);
    r.put16(kTextAttr + ta_YSize, 13);
    r.put8(kTextAttr + ta_Style, 0);
    r.put8(kTextAttr + ta_Flags, 0);
    r.cpu().a[0] = kTextAttr;
    const u32 font = r.result(Base::Graphics, graphics_lvo::OpenFont);
    CHECK(font != 0);
    CHECK_EQ(r.get16(font + tf_YSize), 13u);
    const u32 width = r.get16(font + tf_XSize);
    const u32 baseline = r.get16(font + tf_Baseline);
    const u32 data = r.get32(font + tf_CharData);
    const u32 modulo = r.get16(font + tf_Modulo);
    set_font(r, s.rp, font);
    r.graphics.set_pens(s.rp, 1, 2, JAM2);
    move(r, s.rp, 3, 20);
    draw_text(r, s.rp, "Hg\xE5");
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 3 + 3 * width);
    const u8 text[] = {'H', 'g', 0xE5};
    u32 set_pixels = 0;
    for (u32 i = 0; i < 3; ++i) {
        for (u32 row = 0; row < 13; ++row) {
            for (u32 col = 0; col < width; ++col) {
                const u32 bit = (text[i] - 32u) * width + col;
                const u32 set = (r.get8(data + row * modulo + bit / 8) >> (7 - bit % 8)) & 1;
                set_pixels += set;
                CHECK_EQ(read_pixel(r, s.rp, static_cast<i32>(3 + i * width + col),
                                    static_cast<i32>(20 - baseline + row)),
                         set != 0 ? 1u : 2u);
            }
        }
    }
    CHECK(set_pixels > 40);
    CHECK_EQ(read_pixel(r, s.rp, 2, 20), 0u);
    CHECK_EQ(read_pixel(r, s.rp, static_cast<i32>(3 + 3 * width), 20), 0u);
    r.cpu().a[1] = s.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextLength), 3 * width);
}

// GfxBase holds the font InitRastPort gives a RastPort: topaz until
// set_default_font names another, with the accessors OpenFont and
// CloseFont count.
void default_font_is_topaz_until_set() {
    GfxRig r;
    const u32 topaz = r.graphics.topaz_font();
    CHECK_EQ(r.get32(r.graphics.graphics_base() + 154), topaz);
    CHECK_EQ(r.graphics.default_font(), topaz);

    const u32 font = open_hasklig(r, 13);
    r.graphics.set_default_font(font);
    CHECK_EQ(r.graphics.default_font(), font);
    CHECK_EQ(r.get32(r.graphics.graphics_base() + 154), font);
    CHECK_EQ(r.get16(font + tf_Accessors), 2u);  // the open and the default

    r.cpu().a[1] = kRastPort;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    CHECK_EQ(r.get32(kRastPort + rp_Font), font);
    CHECK_EQ(r.get16(kRastPort + rp_TxHeight), 13u);
    CHECK_EQ(r.get16(kRastPort + rp_TxBaseline), r.get16(font + tf_Baseline));

    r.graphics.set_default_font(0);
    CHECK_EQ(r.graphics.default_font(), topaz);
    CHECK_EQ(r.get32(r.graphics.graphics_base() + 154), topaz);
    CHECK_EQ(r.get16(font + tf_Accessors), 1u);
}

// text_in draws in the font it is given rather than the RastPort's, in
// the RastPort's pens, and text_width and font_metrics measure that font.
void text_in_draws_and_measures_an_explicit_font() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    const u32 font = open_hasklig(r, 13);
    const u32 width = r.get16(font + tf_XSize);
    const u32 y_size = r.get16(font + tf_YSize);
    const u32 baseline = r.get16(font + tf_Baseline);
    CHECK_EQ(r.graphics.default_font(), r.graphics.topaz_font());

    const Graphics::FontMetrics metrics = r.graphics.font_metrics(font);
    CHECK_EQ(metrics.y_size, static_cast<i32>(y_size));
    CHECK_EQ(metrics.baseline, static_cast<i32>(baseline));
    CHECK_EQ(metrics.x_size, static_cast<i32>(width));
    const Graphics::FontMetrics built_in = r.graphics.font_metrics(0);
    CHECK_EQ(built_in.y_size, 8);
    CHECK_EQ(built_in.baseline, 6);
    CHECK_EQ(built_in.x_size, 8);

    const u8 text[] = {'H', 'g'};
    CHECK_EQ(r.graphics.text_width(font, text, 2), 2 * width);
    CHECK_EQ(r.graphics.text_width(0, text, 2), 16u);

    // TextLength with the RastPort in the font agrees.
    r.str(kStrings, "Hg");
    r.graphics.set_font(s.rp, font);
    r.cpu().a[1] = s.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 2;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextLength), 2 * width);
    r.graphics.set_font(s.rp, r.graphics.topaz_font());

    // Drawn through text_in, its glyphs differ from the built-in font's
    // for the same text.
    r.graphics.set_pens(s.rp, 1, 2, JAM2);
    move(r, s.rp, 3, 20);
    CHECK_EQ(r.graphics.text_in(s.rp, font, text, 2), 2 * width);
    const i32 left = 3;
    const i32 top = 20 - static_cast<i32>(baseline);
    const i32 right = 3 + static_cast<i32>(2 * width);
    std::vector<u32> drawn;
    for (i32 y = top; y < top + static_cast<i32>(y_size); ++y) {
        for (i32 x = left; x < right; ++x) {
            drawn.push_back(read_pixel(r, s.rp, x, y));
        }
    }
    move(r, s.rp, 3, 20);
    CHECK_EQ(r.graphics.text(s.rp, text, 2), 16u);
    u32 different = 0;
    u32 at = 0;
    for (i32 y = top; y < top + static_cast<i32>(y_size); ++y) {
        for (i32 x = left; x < right; ++x) {
            different += drawn[at++] != read_pixel(r, s.rp, x, y) ? 1 : 0;
        }
    }
    CHECK(different > 0);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 3u + 16u);
}

// plain_text_in leaves the RastPort's spacing out, text_fit counts the
// whole characters a width holds, and hold_font is one more accessor.
void plain_text_fits_and_holds_a_font() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    const u32 font = open_hasklig(r, 13);
    const u32 width = r.get16(font + tf_XSize);
    const u8 text[] = {'H', 'g', 'H'};

    r.put16(s.rp + rp_TxSpacing, 2);
    r.graphics.set_pens(s.rp, 1, 0, JAM1);
    move(r, s.rp, 3, 20);
    CHECK_EQ(r.graphics.text_in(s.rp, font, text, 3), 3 * (width + 2));
    move(r, s.rp, 3, 40);
    CHECK_EQ(r.graphics.plain_text_in(s.rp, font, text, 3), 3 * width);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 3u + 3 * width);

    const i32 cell = static_cast<i32>(width);
    CHECK_EQ(r.graphics.text_fit(font, text, 3, 3 * cell), 3u);
    CHECK_EQ(r.graphics.text_fit(font, text, 3, 3 * cell - 1), 2u);
    CHECK_EQ(r.graphics.text_fit(font, text, 3, 0), 0u);
    CHECK_EQ(r.graphics.text_fit(0, text, 3, 17), 2u);

    const u32 accessors = r.get16(font + tf_Accessors);
    r.graphics.hold_font(font);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors + 1);
    r.graphics.close_font(font);
    CHECK_EQ(r.get16(font + tf_Accessors), accessors);
}

// ExtendFont hangs an extension holding the reply port and a copy of the
// tags on tf_Extension and marks the font FSF_TAGGED; StripFont undoes it.
void extend_font_and_strip_font() {
    GfxRig r;
    const u32 font = r.exec.alloc(64, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 tags = r.exec.alloc(32, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(font != 0 && tags != 0);
    r.put32(font + 14, 0x1234);  // mn_ReplyPort
    r.put8(font + tf_Style, 0x02);
    r.put32(tags, 0x80001234);
    r.put32(tags + 4, 7);
    r.cpu().a[0] = font;
    r.cpu().a[1] = tags;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::ExtendFont), 1u);
    const u32 tfe = r.get32(font + 14);
    CHECK(tfe != 0 && tfe != 0x1234);
    CHECK_EQ(r.get16(tfe), 0xF100u);
    CHECK_EQ(r.get32(tfe + 4), font);
    CHECK_EQ(r.get32(tfe + 8), 0x1234u);
    const u32 copy = r.get32(tfe + 12);
    CHECK(copy != 0 && copy != tags);
    CHECK_EQ(r.get32(copy), 0x80001234u);
    CHECK_EQ(r.get32(copy + 4), 7u);
    CHECK_EQ(r.get32(copy + 8), 0u);
    CHECK_EQ(r.get8(font + tf_Style), 0x42u);
    // A second ExtendFont keeps the one extension.
    r.cpu().a[0] = font;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::ExtendFont), 1u);
    CHECK_EQ(r.get32(font + 14), tfe);
    r.cpu().a[0] = font;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::StripFont), ExitReason::Halt);
    CHECK_EQ(r.get32(font + 14), 0x1234u);
    CHECK_EQ(r.get8(font + tf_Style), 0x02u);
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
    r.put32(kLayer + ly_SuperBitMap, kTestGuestBytes);
    r.put32(kRastPort + rp_Layer, kLayer);
    r.put32(kRastPort + rp_BitMap, kTestGuestBytes);

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

void text_extent_reports_five_characters_of_the_8x8_font() {
    GfxRig r;
    r.cpu().a[2] = kExtent;
    r.cpu().d[0] = 5;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::TextExtent), ExitReason::Halt);
    CHECK_EQ(r.get16(kExtent), 40u);
    CHECK_EQ(r.get16(kExtent + 2), 8u);
    CHECK_EQ(r.get16(kExtent + 4), 0u);
    CHECK_EQ(r.get16(kExtent + 6), 0xFFFAu);  // the baseline is 6 above the top
    CHECK_EQ(r.get16(kExtent + 8), 39u);
    CHECK_EQ(r.get16(kExtent + 10), 1u);
}

void area_fills_square_and_triangle_and_empties_the_table() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    init_area(r, s.rp, kAreaMax);

    CHECK_EQ(area_move(r, s.rp, 10, 10), 0u);
    CHECK_EQ(area_draw(r, s.rp, 20, 10), 0u);
    CHECK_EQ(area_draw(r, s.rp, 20, 20), 0u);
    CHECK_EQ(area_draw(r, s.rp, 10, 20), 0u);
    area_end(r, s.rp);

    CHECK_EQ(read_pixel(r, s.rp, 10, 10), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 15, 15), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 20, 20), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 9, 15), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 21, 15), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 15, 9), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 15, 21), 0u);
    CHECK_EQ(r.get16(kAreaInfo + ai_Count), 0u);
    CHECK_EQ(r.get32(kAreaInfo + ai_VctrPtr), kAreaTable);
    CHECK_EQ(r.get32(kAreaInfo + ai_FlagPtr), kAreaTable + kAreaMax * 4);

    CHECK_EQ(area_move(r, s.rp, 50, 50), 0u);
    CHECK_EQ(area_draw(r, s.rp, 70, 50), 0u);
    CHECK_EQ(area_draw(r, s.rp, 60, 70), 0u);
    area_end(r, s.rp);

    CHECK_EQ(read_pixel(r, s.rp, 60, 50), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 60, 55), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 60, 65), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 60, 70), 1u);  // the lowest point
    CHECK_EQ(read_pixel(r, s.rp, 60, 71), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 49, 55), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 69, 55), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 60, 49), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 60, 71), 0u);
}

void area_fill_of_a_shape_without_width_draws_its_whole_line() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    init_area(r, s.rp, kAreaMax);

    // Out along a shallow line, on to a tip and back the same way, as a
    // clock draws a thin hand.
    CHECK_EQ(area_move(r, s.rp, 10, 100), 0u);
    CHECK_EQ(area_draw(r, s.rp, 50, 108), 0u);
    CHECK_EQ(area_draw(r, s.rp, 60, 110), 0u);
    CHECK_EQ(area_draw(r, s.rp, 50, 108), 0u);
    CHECK_EQ(area_draw(r, s.rp, 10, 100), 0u);
    area_end(r, s.rp);

    // Every column from end to end holds a pixel of the line, and nothing
    // lies outside the box around it.
    for (i32 x = 10; x <= 60; ++x) {
        u32 set = 0;
        for (i32 y = 100; y <= 110; ++y) {
            set += read_pixel(r, s.rp, x, y) == 1 ? 1 : 0;
        }
        CHECK(set >= 1);
        CHECK_EQ(read_pixel(r, s.rp, x, 99), 0u);
        CHECK_EQ(read_pixel(r, s.rp, x, 111), 0u);
    }
    CHECK_EQ(read_pixel(r, s.rp, 10, 100), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 60, 110), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 9, 100), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 61, 110), 0u);

    // Filled again in pen 0 the same shape takes every pixel away.
    r.graphics.set_pens(s.rp, 0, 0, JAM1);
    CHECK_EQ(area_move(r, s.rp, 10, 100), 0u);
    CHECK_EQ(area_draw(r, s.rp, 50, 108), 0u);
    CHECK_EQ(area_draw(r, s.rp, 60, 110), 0u);
    CHECK_EQ(area_draw(r, s.rp, 50, 108), 0u);
    CHECK_EQ(area_draw(r, s.rp, 10, 100), 0u);
    area_end(r, s.rp);
    for (i32 y = 99; y <= 111; ++y) {
        for (i32 x = 9; x <= 61; ++x) {
            CHECK_EQ(read_pixel(r, s.rp, x, y), 0u);
        }
    }
}

void area_draw_past_max_count_returns_minus_one() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    init_area(r, s.rp, 2);

    CHECK_EQ(area_move(r, s.rp, 10, 10), 0u);
    CHECK_EQ(area_draw(r, s.rp, 20, 20), 0u);
    CHECK_EQ(area_draw(r, s.rp, 30, 30), 0xFFFFFFFFu);
    CHECK_EQ(r.get16(kAreaInfo + ai_Count), 2u);
}

void area_ellipse_fills_symmetrically_about_the_centre() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    init_area(r, s.rp, kAreaMax);

    CHECK_EQ(area_ellipse(r, s.rp, 100, 100, 30, 20), 0u);
    area_end(r, s.rp);

    CHECK_EQ(read_pixel(r, s.rp, 100, 100), 1u);
    for (i32 dy = -20; dy <= 20; ++dy) {
        CHECK_EQ(read_pixel(r, s.rp, 100 - 20, 100 + dy),
                 read_pixel(r, s.rp, 100 + 20, 100 + dy));
        CHECK_EQ(read_pixel(r, s.rp, 100 - 30, 100 + dy),
                 read_pixel(r, s.rp, 100 + 30, 100 + dy));
    }
    CHECK_EQ(read_pixel(r, s.rp, 130, 120), 0u);
}

// Two ellipses in one AreaEnd fill by the even-odd rule: a ring.
void area_ellipses_fill_together_as_a_ring() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    init_area(r, s.rp, kAreaMax);

    CHECK_EQ(area_ellipse(r, s.rp, 100, 100, 40, 40), 0u);
    CHECK_EQ(area_ellipse(r, s.rp, 100, 100, 20, 20), 0u);
    area_end(r, s.rp);

    CHECK_EQ(read_pixel(r, s.rp, 100, 100), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 130, 100), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 100, 70), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 145, 100), 0u);
}

// ObtainPen hands out free pens, exclusively when asked, and sets their
// colour; the system's pens are never free.
void obtain_pen_shares_and_reserves_pens() {
    GfxRig r;
    const u32 color_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 low_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    r.put8(kColorMap + cm_Type, COLORMAP_TYPE_V39);
    r.put16(kColorMap + cm_Count, 256);
    r.put32(kColorMap + cm_ColorTable, color_table);
    r.put32(kColorMap + cm_LowColorBits, low_table);
    const auto obtain = [&](u32 n, u32 flags) {
        r.cpu().a[0] = kColorMap;
        r.cpu().d[0] = n;
        r.cpu().d[1] = 0xFFFFFFFF;
        r.cpu().d[2] = 0;
        r.cpu().d[3] = 0;
        r.cpu().d[4] = flags;
        return r.result(Base::Graphics, graphics_lvo::ObtainPen);
    };
    constexpr u32 kExclusive = 1;
    const u32 pen = obtain(0xFFFFFFFF, kExclusive);
    CHECK(pen >= 8 && pen < chrome::kScreenBar0);
    CHECK_EQ(r.get16(color_table + pen * 2), 0x0F00u);
    CHECK_EQ(obtain(pen, 0), 0xFFFFFFFFu);
    CHECK_EQ(obtain(1, kExclusive), 0xFFFFFFFFu);
    CHECK_EQ(obtain(1, 0), 1u);
    CHECK_EQ(obtain(250, kExclusive), 0xFFFFFFFFu);
    r.cpu().a[0] = kColorMap;
    r.cpu().d[0] = pen;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::ReleasePen), ExitReason::Halt);
    CHECK_EQ(obtain(pen, kExclusive), pen);
}

// The system keeps the chrome's pens of a screen of 5, 6 or 7 planes where
// they lie at chrome::physical_pen: ObtainPen hands none out free or alone and
// the pens below them stay free, while ObtainBestPen may still share one as
// the nearest colour.
void obtain_pen_keeps_the_chrome_pens_of_shallow_screens() {
    constexpr u32 kExclusive = 1;
    for (const u32 depth : {4u, 5u, 6u, 7u}) {
        GfxRig r;
        const u32 pens = 1u << depth;
        const u32 color_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
        const u32 low_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
        r.put8(kColorMap + cm_Type, COLORMAP_TYPE_V39);
        r.put16(kColorMap + cm_Count, pens);
        r.put32(kColorMap + cm_ColorTable, color_table);
        r.put32(kColorMap + cm_LowColorBits, low_table);
        const auto obtain = [&](u32 n, u32 flags) {
            r.cpu().a[0] = kColorMap;
            r.cpu().d[0] = n;
            r.cpu().d[1] = 0xFFFFFFFF;
            r.cpu().d[2] = 0;
            r.cpu().d[3] = 0;
            r.cpu().d[4] = flags;
            return r.result(Base::Graphics, graphics_lvo::ObtainPen);
        };
        const auto best = [&](u32 red, u32 green, u32 blue) {
            r.cpu().a[0] = kColorMap;
            r.cpu().d[1] = red;
            r.cpu().d[2] = green;
            r.cpu().d[3] = blue;
            r.cpu().a[1] = 0;
            return r.result(Base::Graphics, graphics_lvo::ObtainBestPenA);
        };
        // Four planes have no chrome: every pen above the system's first
        // is free, the top one first.
        const u32 first = depth == 4 ? pens : (depth == 5 ? 16 : pens - chrome::kChromePens);
        const u32 pen = obtain(0xFFFFFFFF, kExclusive);
        CHECK_EQ(pen, first - 1);
        for (u32 chrome_pen = first; chrome_pen < pens; ++chrome_pen) {
            CHECK_EQ(obtain(chrome_pen, kExclusive), 0xFFFFFFFFu);
        }
        if (depth == 4) {
            continue;
        }
        // Shared, the system's pen is the program's too, with its colour as
        // it was.
        CHECK_EQ(obtain(first, 0), first);
        CHECK_EQ(r.get16(color_table + first * 2), 0u);
        CHECK_EQ(obtain(first - 2, kExclusive), first - 2);

        // A colour a chrome pen has exactly is that pen, shared; one no pen
        // has takes a free pen below the chrome.
        r.put16(color_table + (first + 3) * 2, 0x0F00);
        CHECK_EQ(best(0xF0000000u, 0, 0), first + 3);
        CHECK_EQ(best(0, 0xF0000000u, 0), first - 3);
        CHECK_EQ(r.get16(color_table + (first - 3) * 2), 0x00F0u);
    }
}

void area_outline_uses_the_outline_pen() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    init_area(r, s.rp, kAreaMax);
    r.put16(s.rp + rp_Flags, AREAOUTLINE);
    r.put8(s.rp + rp_AOlPen, 2);

    area_move(r, s.rp, 10, 10);
    area_draw(r, s.rp, 20, 10);
    area_draw(r, s.rp, 20, 20);
    area_draw(r, s.rp, 10, 20);
    area_end(r, s.rp);

    CHECK_EQ(read_pixel(r, s.rp, 10, 10), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 20, 10), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 20, 20), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 10, 20), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 15, 10), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 10, 15), 2u);
    CHECK_EQ(read_pixel(r, s.rp, 11, 15), 1u);
    CHECK_EQ(read_pixel(r, s.rp, 9, 15), 0u);
}

void draw_ellipse_outline_has_no_gaps_and_leaves_the_inside_clear() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    const i32 cx = 100;
    const i32 cy = 100;
    const i32 rx = 30;
    const i32 ry = 20;

    r.cpu().a[1] = s.rp;
    r.cpu().d[0] = static_cast<u32>(cx);
    r.cpu().d[1] = static_cast<u32>(cy);
    r.cpu().d[2] = static_cast<u32>(rx);
    r.cpu().d[3] = static_cast<u32>(ry);
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::DrawEllipse), ExitReason::Halt);

    CHECK_EQ(read_pixel(r, s.rp, cx, cy - ry), 1u);
    CHECK_EQ(read_pixel(r, s.rp, cx, cy + ry), 1u);
    for (i32 y = cy - ry + 1; y < cy + ry; ++y) {
        bool left = false;
        bool right = false;
        for (i32 x = cx - rx - 1; x < cx; ++x) {
            left = left || read_pixel(r, s.rp, x, y) != 0;
        }
        for (i32 x = cx + 1; x <= cx + rx + 1; ++x) {
            right = right || read_pixel(r, s.rp, x, y) != 0;
        }
        CHECK(left && right);
    }
    CHECK_EQ(read_pixel(r, s.rp, cx, cy), 0u);
    CHECK_EQ(read_pixel(r, s.rp, cx + 10, cy), 0u);
    CHECK_EQ(read_pixel(r, s.rp, cx, cy + 5), 0u);
    CHECK_EQ(read_pixel(r, s.rp, cx + rx + 1, cy), 0u);
    CHECK_EQ(read_pixel(r, s.rp, cx, cy - ry - 1), 0u);
}

void init_tmp_ras_records_the_buffer_and_wait_blit_is_a_no_op() {
    GfxRig r;
    r.cpu().a[0] = kTmpRas;
    r.cpu().a[1] = kTmpBuffer;
    r.cpu().d[0] = 1234;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::InitTmpRas), kTmpRas);
    CHECK_EQ(r.get32(kTmpRas), kTmpBuffer);
    CHECK_EQ(r.get32(kTmpRas + 4), 1234u);

    r.cpu().a[0] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::InitTmpRas), 0u);

    r.cpu().d[0] = 0x1234;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::WaitBlit), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x1234u);
}

}

// --- regions -----------------------------------------------------------------

constexpr u32 kRectangle = kData + 0xE00;

u32 new_region(GfxRig& r) {
    const u32 region = r.result(Base::Graphics, graphics_lvo::NewRegion);
    CHECK(region != 0);
    return region;
}

void dispose_region(GfxRig& r, u32 region) {
    r.cpu().a[0] = region;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::DisposeRegion), ExitReason::Halt);
}

// OrRectRegion, AndRectRegion, ClearRectRegion or XorRectRegion of the
// rectangle with the region; D0 is what the vector gave.
u32 rect_region(GfxRig& r, u32 lvo, u32 region, i32 x0, i32 y0, i32 x1, i32 y1) {
    r.put16(kRectangle, static_cast<u32>(x0));
    r.put16(kRectangle + 2, static_cast<u32>(y0));
    r.put16(kRectangle + 4, static_cast<u32>(x1));
    r.put16(kRectangle + 6, static_cast<u32>(y1));
    r.cpu().a[0] = region;
    r.cpu().a[1] = kRectangle;
    return r.result(Base::Graphics, lvo);
}

u32 region_region(GfxRig& r, u32 lvo, u32 source, u32 dest) {
    r.cpu().a[0] = source;
    r.cpu().a[1] = dest;
    return r.result(Base::Graphics, lvo);
}

i32 word_at(GfxRig& r, u32 at) { return static_cast<__INT16_TYPE__>(r.get16(at)); }

// The rectangles of the region in absolute coordinates, in list order.
std::vector<Rect> region_rects(GfxRig& r, u32 region) {
    std::vector<Rect> rects;
    const i32 min_x = word_at(r, region + rg_bounds);
    const i32 min_y = word_at(r, region + rg_bounds + 2);
    u32 prev = 0;
    for (u32 rr = r.get32(region + rg_RegionRectangle); rr != 0 && rects.size() < 1000;
         rr = r.get32(rr + rr_Next)) {
        CHECK_EQ(r.get32(rr + rr_Prev), prev);
        rects.push_back(Rect{min_x + word_at(r, rr + rr_bounds),
                             min_y + word_at(r, rr + rr_bounds + 2),
                             min_x + word_at(r, rr + rr_bounds + 4),
                             min_y + word_at(r, rr + rr_bounds + 6)});
        prev = rr;
    }
    return rects;
}

// How many of the region's rectangles hold the point: 1 inside a region
// whose rectangles do not overlap, 0 outside.
u32 region_cover(const std::vector<Rect>& rects, i32 x, i32 y) {
    u32 count = 0;
    for (const Rect& rect : rects) {
        if (x >= rect.x0 && x <= rect.x1 && y >= rect.y0 && y <= rect.y1) {
            ++count;
        }
    }
    return count;
}

// Checks the region against inside(x, y) over 0..59 in both directions,
// and that its bounds are the box around what it covers.
template <typename Inside>
void check_region(GfxRig& r, u32 region, Inside inside) {
    const std::vector<Rect> rects = region_rects(r, region);
    Rect box{0, 0, -1, -1};
    for (i32 y = 0; y < 60; ++y) {
        for (i32 x = 0; x < 60; ++x) {
            const u32 expected = inside(x, y) ? 1u : 0u;
            CHECK_EQ(region_cover(rects, x, y), expected);
            if (expected == 0) {
                continue;
            }
            if (box.x1 < box.x0) {
                box = Rect{x, y, x, y};
            }
            box.x0 = x < box.x0 ? x : box.x0;
            box.x1 = x > box.x1 ? x : box.x1;
            box.y0 = y < box.y0 ? y : box.y0;
            box.y1 = y > box.y1 ? y : box.y1;
        }
    }
    if (box.x1 < box.x0) {
        CHECK(rects.empty());
        return;
    }
    CHECK_EQ(word_at(r, region + rg_bounds), box.x0);
    CHECK_EQ(word_at(r, region + rg_bounds + 2), box.y0);
    CHECK_EQ(word_at(r, region + rg_bounds + 4), box.x1);
    CHECK_EQ(word_at(r, region + rg_bounds + 6), box.y1);
}

bool in_box(i32 x, i32 y, i32 x0, i32 y0, i32 x1, i32 y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

void new_region_is_empty_and_dispose_frees_every_rectangle() {
    GfxRig r;
    const u32 before = r.exec.free_bytes();
    const u32 region = new_region(r);
    CHECK_EQ(r.get32(region + rg_RegionRectangle), 0u);
    check_region(r, region, [](i32, i32) { return false; });

    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, region, 10, 10, 19, 19), 1u);
    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, region, 40, 30, 49, 39), 1u);
    CHECK(r.exec.free_bytes() < before);
    dispose_region(r, region);
    CHECK_EQ(r.exec.free_bytes(), before);

    // Disposing no region does nothing.
    dispose_region(r, 0);
    CHECK_EQ(r.exec.free_bytes(), before);
}

void or_rect_region_stores_rectangles_relative_to_the_bounds() {
    GfxRig r;
    const u32 region = new_region(r);
    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, region, 10, 20, 19, 24), 1u);
    CHECK_EQ(word_at(r, region + rg_bounds), 10);
    CHECK_EQ(word_at(r, region + rg_bounds + 2), 20);
    CHECK_EQ(word_at(r, region + rg_bounds + 4), 19);
    CHECK_EQ(word_at(r, region + rg_bounds + 6), 24);
    const u32 rr = r.get32(region + rg_RegionRectangle);
    CHECK(rr != 0);
    CHECK_EQ(r.get32(rr + rr_Next), 0u);
    CHECK_EQ(r.get32(rr + rr_Prev), 0u);
    CHECK_EQ(word_at(r, rr + rr_bounds), 0);
    CHECK_EQ(word_at(r, rr + rr_bounds + 2), 0);
    CHECK_EQ(word_at(r, rr + rr_bounds + 4), 9);
    CHECK_EQ(word_at(r, rr + rr_bounds + 6), 4);
    dispose_region(r, region);
}

void or_rect_region_joins_overlapping_rectangles_without_overlap() {
    GfxRig r;
    const u32 region = new_region(r);
    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, region, 10, 10, 29, 29), 1u);
    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, region, 20, 20, 39, 39), 1u);
    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, region, 50, 5, 55, 8), 1u);
    check_region(r, region, [](i32 x, i32 y) {
        return in_box(x, y, 10, 10, 29, 29) || in_box(x, y, 20, 20, 39, 39) ||
               in_box(x, y, 50, 5, 55, 8);
    });

    // A rectangle already inside changes nothing.
    const std::vector<Rect> rects = region_rects(r, region);
    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, region, 12, 12, 15, 15), 1u);
    CHECK_EQ(region_rects(r, region).size(), rects.size());
    dispose_region(r, region);
}

void and_rect_region_keeps_what_the_rectangle_covers() {
    GfxRig r;
    const u32 region = new_region(r);
    rect_region(r, graphics_lvo::OrRectRegion, region, 10, 10, 29, 29);
    rect_region(r, graphics_lvo::OrRectRegion, region, 40, 10, 49, 29);
    rect_region(r, graphics_lvo::AndRectRegion, region, 20, 15, 44, 19);
    check_region(r, region, [](i32 x, i32 y) {
        return in_box(x, y, 20, 15, 29, 19) || in_box(x, y, 40, 15, 44, 19);
    });

    // A rectangle that misses the region empties it.
    const u32 before = r.exec.free_bytes();
    rect_region(r, graphics_lvo::AndRectRegion, region, 0, 0, 5, 5);
    check_region(r, region, [](i32, i32) { return false; });
    CHECK(r.exec.free_bytes() > before);
    dispose_region(r, region);
}

void clear_rect_region_cuts_a_hole() {
    GfxRig r;
    const u32 region = new_region(r);
    rect_region(r, graphics_lvo::OrRectRegion, region, 10, 10, 39, 39);
    CHECK_EQ(rect_region(r, graphics_lvo::ClearRectRegion, region, 20, 20, 29, 29), 1u);
    check_region(r, region, [](i32 x, i32 y) {
        return in_box(x, y, 10, 10, 39, 39) && !in_box(x, y, 20, 20, 29, 29);
    });

    // Cutting past the edge takes the corner off.
    CHECK_EQ(rect_region(r, graphics_lvo::ClearRectRegion, region, 35, 0, 59, 14), 1u);
    check_region(r, region, [](i32 x, i32 y) {
        return in_box(x, y, 10, 10, 39, 39) && !in_box(x, y, 20, 20, 29, 29) &&
               !in_box(x, y, 35, 0, 59, 14);
    });
    dispose_region(r, region);
}

void xor_rect_region_keeps_what_only_one_of_them_covers() {
    GfxRig r;
    const u32 region = new_region(r);
    rect_region(r, graphics_lvo::OrRectRegion, region, 10, 10, 29, 29);
    CHECK_EQ(rect_region(r, graphics_lvo::XorRectRegion, region, 20, 20, 39, 39), 1u);
    check_region(r, region, [](i32 x, i32 y) {
        return in_box(x, y, 10, 10, 29, 29) != in_box(x, y, 20, 20, 39, 39);
    });

    // The same rectangle again puts the first one back.
    CHECK_EQ(rect_region(r, graphics_lvo::XorRectRegion, region, 20, 20, 39, 39), 1u);
    check_region(r, region, [](i32 x, i32 y) { return in_box(x, y, 10, 10, 29, 29); });
    dispose_region(r, region);
}

void region_region_operations_change_only_the_destination() {
    GfxRig r;
    const u32 source = new_region(r);
    const u32 dest = new_region(r);
    rect_region(r, graphics_lvo::OrRectRegion, source, 5, 5, 24, 24);
    rect_region(r, graphics_lvo::OrRectRegion, source, 40, 40, 49, 49);
    rect_region(r, graphics_lvo::OrRectRegion, dest, 15, 15, 44, 44);
    auto in_source = [](i32 x, i32 y) {
        return in_box(x, y, 5, 5, 24, 24) || in_box(x, y, 40, 40, 49, 49);
    };

    CHECK_EQ(region_region(r, graphics_lvo::AndRegionRegion, source, dest), 1u);
    check_region(r, dest, [&](i32 x, i32 y) {
        return in_source(x, y) && in_box(x, y, 15, 15, 44, 44);
    });
    check_region(r, source, in_source);

    CHECK_EQ(region_region(r, graphics_lvo::OrRegionRegion, source, dest), 1u);
    check_region(r, dest, in_source);
    check_region(r, source, in_source);

    // An empty source leaves an OR as it was and empties an AND.
    const u32 empty = new_region(r);
    CHECK_EQ(region_region(r, graphics_lvo::OrRegionRegion, empty, dest), 1u);
    check_region(r, dest, in_source);
    CHECK_EQ(region_region(r, graphics_lvo::AndRegionRegion, empty, dest), 1u);
    check_region(r, dest, [](i32, i32) { return false; });
    dispose_region(r, empty);
    dispose_region(r, source);
    dispose_region(r, dest);
}

void clear_region_empties_the_region_and_frees_its_rectangles() {
    GfxRig r;
    const u32 region = new_region(r);
    const u32 empty_bytes = r.exec.free_bytes();
    rect_region(r, graphics_lvo::OrRectRegion, region, 10, 10, 39, 39);
    rect_region(r, graphics_lvo::ClearRectRegion, region, 20, 20, 29, 29);
    r.cpu().a[0] = region;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::ClearRegion), ExitReason::Halt);
    CHECK_EQ(r.exec.free_bytes(), empty_bytes);
    CHECK_EQ(r.get32(region + rg_RegionRectangle), 0u);
    check_region(r, region, [](i32, i32) { return false; });

    // The cleared region takes rectangles again.
    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, region, 1, 2, 3, 4), 1u);
    check_region(r, region, [](i32 x, i32 y) { return in_box(x, y, 1, 2, 3, 4); });
    dispose_region(r, region);
}

void region_calls_without_a_region_or_rectangle_fail() {
    GfxRig r;
    const u32 region = new_region(r);
    CHECK_EQ(rect_region(r, graphics_lvo::OrRectRegion, 0, 1, 1, 2, 2), 0u);
    r.cpu().a[0] = region;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OrRectRegion), 0u);
    CHECK_EQ(region_region(r, graphics_lvo::OrRegionRegion, 0, region), 0u);
    CHECK_EQ(region_region(r, graphics_lvo::AndRegionRegion, region, 0), 0u);
    check_region(r, region, [](i32, i32) { return false; });
    dispose_region(r, region);
}

// The pen at a bitmap position, read from the planes themselves.
u32 plane_pixel(GfxRig& r, const Surface& s, u32 row_bytes, u32 x, u32 y) {
    const u32 at = y * row_bytes + (x >> 3);
    const u32 mask = 0x80u >> (x & 7);
    return ((r.get8(s.plane0 + at) & mask) != 0 ? 1u : 0u) |
           ((r.get8(s.plane1 + at) & mask) != 0 ? 2u : 0u);
}

void set_rast(GfxRig& r, u32 rp, u32 pen) {
    r.cpu().a[1] = rp;
    r.cpu().d[0] = pen;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRast), ExitReason::Halt);
}

// A layer at 100,50 to 179,99 of the screen bitmap whose clip region holds
// the layer's 10,10-19,19 and 30,10-39,19.
u32 install_two_boxes(GfxRig& r, const Surface& s) {
    r.put16(kLayer + ly_MinX, 100);
    r.put16(kLayer + ly_MinY, 50);
    r.put16(kLayer + ly_MaxX, 179);
    r.put16(kLayer + ly_MaxY, 99);
    r.put32(s.rp + rp_Layer, kLayer);
    const u32 region = new_region(r);
    rect_region(r, graphics_lvo::OrRectRegion, region, 10, 10, 19, 19);
    rect_region(r, graphics_lvo::OrRectRegion, region, 30, 10, 39, 19);
    r.put32(kLayer + ly_ClipRegion, region);
    return region;
}

bool in_two_boxes(i32 x, i32 y) {
    return in_box(x, y, 10, 10, 19, 19) || in_box(x, y, 30, 10, 39, 19);
}

void clip_region_keeps_fills_inside_its_rectangles() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    const u32 region = install_two_boxes(r, s);

    // SetRast clears to pen 3 only where the region is.
    set_rast(r, s.rp, 3);
    for (i32 y = 0; y < 50; ++y) {
        for (i32 x = 0; x < 80; ++x) {
            const u32 expected = in_two_boxes(x, y) ? 3u : 0u;
            CHECK_EQ(plane_pixel(r, s, kBytesPerRow, static_cast<u32>(100 + x),
                                 static_cast<u32>(50 + y)),
                     expected);
        }
    }

    // A RectFill across both rectangles and the gap between them.
    r.graphics.set_pens(s.rp, 1, 0, JAM2);
    rect_fill(r, s.rp, 0, 14, 79, 15);
    for (i32 x = 0; x < 80; ++x) {
        const u32 inside = in_two_boxes(x, 14) ? 1u : 0u;
        CHECK_EQ(plane_pixel(r, s, kBytesPerRow, static_cast<u32>(100 + x), 64), inside);
        CHECK_EQ(plane_pixel(r, s, kBytesPerRow, static_cast<u32>(100 + x), 66),
                 in_two_boxes(x, 16) ? 3u : 0u);
    }

    // Without the region the whole layer is drawn again.
    r.put32(kLayer + ly_ClipRegion, 0);
    set_rast(r, s.rp, 2);
    for (i32 y = 0; y < 50; ++y) {
        for (i32 x = 0; x < 80; ++x) {
            CHECK_EQ(plane_pixel(r, s, kBytesPerRow, static_cast<u32>(100 + x),
                                 static_cast<u32>(50 + y)),
                     2u);
        }
    }
    CHECK_EQ(plane_pixel(r, s, kBytesPerRow, 99, 60), 0u);
    CHECK_EQ(plane_pixel(r, s, kBytesPerRow, 180, 60), 0u);
    dispose_region(r, region);
}

void clip_region_keeps_lines_text_and_pixels_inside() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    const u32 region = install_two_boxes(r, s);
    r.graphics.set_pens(s.rp, 1, 0, JAM2);

    // A line through both rectangles shows only inside them, and the pen
    // position ends where the line does.
    move(r, s.rp, 0, 12);
    r.cpu().a[1] = s.rp;
    r.cpu().d[0] = 79;
    r.cpu().d[1] = 12;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Draw), ExitReason::Halt);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 79u);
    for (i32 x = 0; x < 80; ++x) {
        CHECK_EQ(plane_pixel(r, s, kBytesPerRow, static_cast<u32>(100 + x), 62),
                 in_two_boxes(x, 12) ? 1u : 0u);
    }

    // Text advances by its whole width once, however many rectangles the
    // region has, and each glyph pixel is drawn only inside them.
    move(r, s.rp, 8, 21);
    r.str(kStrings, "HHHH");
    r.cpu().a[1] = s.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 4;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::Text), ExitReason::Halt);
    CHECK_EQ(r.get16(s.rp + rp_cp_x), 40u);
    const u8* rows = glyph('H');
    for (i32 y = 15; y < 23; ++y) {
        for (i32 x = 8; x < 40; ++x) {
            const bool set = ((rows[y - 15] >> ((x - 8) & 7)) & 1) != 0;
            const u32 expected = in_two_boxes(x, y) ? (set ? 1u : 0u) : 0u;
            CHECK_EQ(plane_pixel(r, s, kBytesPerRow, static_cast<u32>(100 + x),
                                 static_cast<u32>(50 + y)),
                     expected);
        }
    }

    // WritePixel and ReadPixel refuse a place outside the region.
    r.graphics.set_pens(s.rp, 2, 0, JAM2);
    CHECK_EQ(write_pixel(r, s.rp, 35, 11), 0u);
    CHECK_EQ(read_pixel(r, s.rp, 35, 11), 2u);
    CHECK_EQ(write_pixel(r, s.rp, 25, 11), 0xFFFFFFFFu);
    CHECK_EQ(read_pixel(r, s.rp, 25, 11), 0xFFFFFFFFu);
    CHECK_EQ(plane_pixel(r, s, kBytesPerRow, 125, 61), 0u);
    dispose_region(r, region);
}

void an_empty_clip_region_lets_nothing_through() {
    GfxRig r;
    const Surface s = make_surface(r, kBitmap, kRastPort);
    r.put16(kLayer + ly_MinX, 100);
    r.put16(kLayer + ly_MinY, 50);
    r.put16(kLayer + ly_MaxX, 179);
    r.put16(kLayer + ly_MaxY, 99);
    r.put32(s.rp + rp_Layer, kLayer);
    const u32 region = new_region(r);
    r.put32(kLayer + ly_ClipRegion, region);
    const std::vector<u8> before = plane_snapshot(r, s);
    set_rast(r, s.rp, 3);
    rect_fill(r, s.rp, 0, 0, 79, 49);
    CHECK_EQ(write_pixel(r, s.rp, 5, 5), 0xFFFFFFFFu);
    CHECK(plane_snapshot(r, s) == before);
    dispose_region(r, region);
}

void clip_region_of_a_superbitmap_layer_is_window_relative() {
    GfxRig r;
    const Surface s = make_superbitmap_layer(r);
    const u32 region = new_region(r);
    rect_region(r, graphics_lvo::OrRectRegion, region, 20, 5, 29, 9);
    r.put32(kLayer + ly_ClipRegion, region);
    set_rast(r, s.rp, 1);
    for (i32 y = 0; y < 50; ++y) {
        for (i32 x = 0; x < 100; ++x) {
            CHECK_EQ(plane_pixel(r, s, 14, static_cast<u32>(x), static_cast<u32>(y)),
                     in_box(x, y, 20, 5, 29, 9) ? 1u : 0u);
        }
    }
    dispose_region(r, region);
}


// The pen at x, y of a two-plane surface.
u32 surface_pen(GfxRig& r, const Surface& s, u32 width, u32 x, u32 y) {
    const u32 at = y * ((width + 15) / 16 * 2) + x / 8;
    const u32 mask = 0x80u >> (x & 7);
    return ((r.get8(s.plane0 + at) & mask) != 0 ? 1u : 0u) |
           ((r.get8(s.plane1 + at) & mask) != 0 ? 2u : 0u);
}

void blt_bitmap_copies_through_the_minterm_and_mask() {
    GfxRig r;
    const Surface a = make_sized_surface(r, kBitmap, kRastPort, 32, 8);
    const Surface b = make_sized_surface(r, kBitmap + 0x40, kRastPort + 0x80, 32, 8);
    // Pen 3 at 2..5 of row 1 in a, pen 1 at 8 of row 1.
    for (u32 x = 2; x <= 5; ++x) {
        r.put8(a.plane0 + 4 + x / 8, r.get8(a.plane0 + 4 + x / 8) | (0x80u >> (x & 7)));
        r.put8(a.plane1 + 4 + x / 8, r.get8(a.plane1 + 4 + x / 8) | (0x80u >> (x & 7)));
    }
    r.put8(a.plane0 + 4 + 1, 0x80);

    // A copy (minterm 0xC0) of 8 by 2 from 2,1 to 10,3, both planes.
    CHECK_EQ(r.graphics.blt_bitmap(a.bitmap, 2, 1, b.bitmap, 10, 3, 8, 2, 0xC0, 0xFF), 2u);
    for (u32 x = 10; x <= 13; ++x) {
        CHECK_EQ(surface_pen(r, b, 32, x, 3), 3u);
    }
    CHECK_EQ(surface_pen(r, b, 32, 16, 3), 1u);
    CHECK_EQ(surface_pen(r, b, 32, 14, 3), 0u);
    CHECK_EQ(surface_pen(r, b, 32, 10, 4), 0u);
    CHECK_EQ(surface_pen(r, b, 32, 9, 3), 0u);

    // The mask leaves plane 1 alone; minterm 0x30 writes the inverse.
    CHECK_EQ(r.graphics.blt_bitmap(a.bitmap, 2, 1, b.bitmap, 10, 3, 4, 1, 0x30, 0x01), 1u);
    CHECK_EQ(surface_pen(r, b, 32, 10, 3), 2u);

    // Within one bitmap, overlapping: right by one keeps the source whole.
    CHECK_EQ(r.graphics.blt_bitmap(a.bitmap, 2, 1, a.bitmap, 3, 1, 4, 1, 0xC0, 0xFF), 2u);
    for (u32 x = 3; x <= 6; ++x) {
        CHECK_EQ(surface_pen(r, a, 32, x, 1), 3u);
    }
    // Down by one, through the same plane, also keeps it whole.
    CHECK_EQ(r.graphics.blt_bitmap(a.bitmap, 0, 1, a.bitmap, 0, 2, 32, 2, 0xC0, 0xFF), 2u);
    CHECK_EQ(surface_pen(r, a, 32, 4, 2), 3u);
    CHECK_EQ(surface_pen(r, a, 32, 4, 3), 0u);

    // Rectangles are cut to the bitmaps.
    CHECK_EQ(r.graphics.blt_bitmap(a.bitmap, 28, 0, b.bitmap, -2, 0, 8, 1, 0xC0, 0xFF), 2u);
    CHECK_EQ(r.graphics.blt_bitmap(a.bitmap, 40, 0, b.bitmap, 0, 0, 8, 1, 0xC0, 0xFF), 0u);

    // BltClear: bytes, and rows of bytes with flag bit 1.
    r.cpu().a[1] = a.plane0;
    r.cpu().d[0] = 4;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::BltClear), ExitReason::Halt);
    CHECK_EQ(r.get32(a.plane0), 0u);
    CHECK(r.get32(a.plane0 + 4) != 0u);
    r.cpu().a[1] = a.plane0;
    r.cpu().d[0] = (2u << 16) | 4;
    r.cpu().d[1] = 2;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::BltClear), ExitReason::Halt);
    CHECK_EQ(r.get32(a.plane0 + 4), 0u);
}

void blt_bitmap_rastport_draws_inside_the_rastport() {
    GfxRig r;
    const Surface src = make_sized_surface(r, kBitmap, kRastPort, 16, 4);
    const Surface dst = make_sized_surface(r, kBitmap + 0x40, kRastPort + 0x80, 32, 8);
    r.put16(src.plane0, 0xFFFF);
    // The RastPort's mask keeps plane 1 out.
    r.put8(dst.rp + rp_Mask, 0x01);
    r.cpu().a[0] = src.bitmap;
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    r.cpu().a[1] = dst.rp;
    r.cpu().d[2] = 4;
    r.cpu().d[3] = 2;
    r.cpu().d[4] = 8;
    r.cpu().d[5] = 1;
    r.cpu().d[6] = 0xC0;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::BltBitMapRastPort), ExitReason::Halt);
    CHECK_EQ(surface_pen(r, dst, 32, 4, 2), 1u);
    CHECK_EQ(surface_pen(r, dst, 32, 11, 2), 1u);
    CHECK_EQ(surface_pen(r, dst, 32, 12, 2), 0u);
    CHECK_EQ(surface_pen(r, dst, 32, 4, 3), 0u);
}

void text_fit_pens_and_bitmaps_of_v39() {
    GfxRig r;
    const Surface a = make_sized_surface(r, kBitmap, kRastPort, 32, 8);
    // TextFit: 10 of 12 characters fit 85 pixels; a constraining extent
    // of 40 pixels lets 5 fit.
    r.str(kStrings, "abcdefghijkl");
    r.cpu().a[1] = a.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 12;
    r.cpu().a[2] = kExtent;
    r.cpu().a[3] = 0;
    r.cpu().d[1] = 1;
    r.cpu().d[2] = 85;
    r.cpu().d[3] = 8;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextFit), 10u);
    CHECK_EQ(r.get16(kExtent), 80u);
    const u32 limit = kExtent + 0x20;
    r.put16(limit + 4, 0);
    r.put16(limit + 6, static_cast<u16>(-6));
    r.put16(limit + 8, 39);
    r.put16(limit + 10, 1);
    r.cpu().a[1] = a.rp;
    r.cpu().a[0] = kStrings;
    r.cpu().d[0] = 12;
    r.cpu().a[2] = 0;
    r.cpu().a[3] = limit;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextFit), 5u);
    // Too low for the font: nothing fits.
    r.cpu().a[3] = 0;
    r.cpu().d[2] = 85;
    r.cpu().d[3] = 7;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::TextFit), 0u);

    // The pens and mode, set together and read back.
    r.cpu().a[1] = a.rp;
    r.cpu().d[0] = 3;
    r.cpu().d[1] = 2;
    r.cpu().d[2] = JAM2;
    r.call(Base::Graphics, graphics_lvo::SetABPenDrMd);
    r.cpu().a[0] = a.rp;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetAPen), 3u);
    r.cpu().a[0] = a.rp;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetBPen), 2u);
    r.cpu().a[0] = a.rp;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDrMd), JAM2);
    r.cpu().a[0] = a.rp;
    r.cpu().d[0] = 0x01;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::SetWriteMask), 1u);
    CHECK_EQ(r.get8(a.rp + rp_Mask), 0x01u);
    r.cpu().a[0] = a.rp;
    r.cpu().d[0] = 2;
    r.call(Base::Graphics, graphics_lvo::SetOutlinePen);
    r.cpu().a[0] = a.rp;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetOutlinePen), 2u);

    // GetRPAttrsA and SetRPAttrsA.
    const u32 rp_tags = kTmpBuffer;
    r.put32(rp_tags, 0x80000002);  // RPTAG_APen
    r.put32(rp_tags + 4, 1);
    r.put32(rp_tags + 8, 0x80000004);  // RPTAG_DrMd
    r.put32(rp_tags + 12, JAM1);
    r.put32(rp_tags + 16, 0);  // TAG_DONE
    r.cpu().a[0] = a.rp;
    r.cpu().a[1] = rp_tags;
    r.call(Base::Graphics, graphics_lvo::SetRPAttrsA);
    CHECK_EQ(r.get8(a.rp + rp_FgPen), 1u);
    CHECK_EQ(r.get8(a.rp + rp_DrawMode), JAM1);
    r.put32(rp_tags + 4, kTmpBuffer + 0x40);
    r.put32(rp_tags + 12, kTmpBuffer + 0x44);
    r.cpu().a[0] = a.rp;
    r.cpu().a[1] = rp_tags;
    r.call(Base::Graphics, graphics_lvo::GetRPAttrsA);
    CHECK_EQ(r.get32(kTmpBuffer + 0x40), 1u);
    CHECK_EQ(r.get32(kTmpBuffer + 0x44), JAM1);

    // AllocBitMap and FreeBitMap balance; GetBitMapAttr reads it.
    const u32 before = r.exec.free_bytes();
    r.cpu().d[0] = 40;
    r.cpu().d[1] = 10;
    r.cpu().d[2] = 3;
    r.cpu().d[3] = 0;
    r.cpu().a[0] = 0;
    const u32 bm = r.result(Base::Graphics, graphics_lvo::AllocBitMap);
    CHECK(bm != 0);
    CHECK_EQ(r.get16(bm + bm_BytesPerRow), 6u);
    r.cpu().a[0] = bm;
    r.cpu().d[1] = 8;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetBitMapAttr), 48u);
    r.cpu().a[0] = bm;
    r.cpu().d[1] = 4;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetBitMapAttr), 3u);
    r.cpu().a[0] = bm;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetBitMapAttr), 10u);
    r.cpu().a[0] = bm;
    r.call(Base::Graphics, graphics_lvo::FreeBitMap);
    CHECK_EQ(r.exec.free_bytes(), before);
}

struct MapHook {
    u32 bitmap;
    u32 map;
};

u32 map_of(void* ctx, u32 bitmap) {
    const MapHook* hook = static_cast<const MapHook*>(ctx);
    return bitmap == hook->bitmap ? hook->map : 0;
}

void clear_planes(GfxRig& r, const u32 (&planes)[8], u32 plane_bytes) {
    for (u32 plane = 0; plane < 8; ++plane) {
        for (u32 i = 0; i < plane_bytes; ++i) {
            r.put8(planes[plane] + i, 0);
        }
    }
}

// Every ROM font with a coverage plane carries it in Hombre's two
// TextFontExtension tags, its bytes the same as rom_font()'s. Topaz, which
// has no coverage, has no extension either.
void rom_fonts_carry_their_coverage_plane() {
    GfxRig r;
    r.str(kStrings + 0x40, "hasklig.font");
    r.put32(kTextAttr + ta_Name, kStrings + 0x40);
    RomFont rom{};
    u32 checked = 0;
    for (u32 i = 0; rom_font(i, rom); ++i) {
        if (std::strcmp(rom.name, "hasklig.font") != 0) {
            continue;
        }
        r.put16(kTextAttr + ta_YSize, rom.y_size);
        r.cpu().a[0] = kTextAttr;
        const u32 font = r.result(Base::Graphics, graphics_lvo::OpenFont);
        CHECK(font != 0);
        CHECK_EQ(r.get16(font + tf_YSize), rom.y_size);
        const u32 tfe = r.get32(font + mn_ReplyPort);
        CHECK(tfe != 0 && (tfe & 1) == 0);
        CHECK((r.get8(font + tf_Style) & 0x40) != 0);
        const u32 tags = r.get32(tfe + 12);
        CHECK_EQ(r.get32(tags), kTagCoverage);
        const u32 plane = r.get32(tags + 4);
        CHECK(plane != 0);
        CHECK_EQ(r.get32(tags + 8), kTagCoverageModulo);
        const u32 modulo = r.get32(tags + 12);
        CHECK_EQ(r.get32(tags + 16), 0u);
        CHECK(rom.coverage != nullptr);
        CHECK_EQ(modulo, rom.coverage_modulo);
        for (u32 row = 0; row < rom.y_size; ++row) {
            for (u32 byte = 0; byte < rom.coverage_modulo; ++byte) {
                CHECK_EQ(r.get8(plane + row * modulo + byte),
                         static_cast<u32>(rom.coverage[row * rom.coverage_modulo + byte]));
            }
        }
        ++checked;
    }
    CHECK_EQ(checked, 3u);
    const u32 topaz = r.graphics.topaz_font();
    CHECK_EQ(r.get32(topaz + mn_ReplyPort), 0u);
    CHECK_EQ(r.get8(topaz + tf_Style) & 0x40, 0u);
}

// Text in a font with coverage on an eight-plane bitmap draws through the
// palette the hook names, level 0 as bg and level 15 as fg; without the
// hook it keeps the 1-bit glyphs, and a palette change refreshes the
// cached ramp.
void text_blends_on_an_eight_plane_palette() {
    GfxRig r;
    constexpr u32 kWidth8 = 64;
    constexpr u32 kHeight8 = 32;
    const u32 row_bytes = (kWidth8 + 15) / 16 * 2;
    const u32 plane_bytes = row_bytes * kHeight8;
    u32 planes[8]{};
    for (u32 plane = 0; plane < 8; ++plane) {
        planes[plane] = r.exec.alloc(plane_bytes, MEMF_CHIP | MEMF_CLEAR);
        CHECK(planes[plane] != 0);
    }
    r.cpu().a[0] = kBitmap;
    r.cpu().d[0] = 8;
    r.cpu().d[1] = kWidth8;
    r.cpu().d[2] = kHeight8;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitBitMap), ExitReason::Halt);
    for (u32 plane = 0; plane < 8; ++plane) {
        r.put32(kBitmap + bm_Planes + plane * 4, planes[plane]);
    }
    r.cpu().a[1] = kRastPort;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
    r.put32(kRastPort + rp_BitMap, kBitmap);

    const u32 color_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    const u32 low_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
    CHECK(color_table != 0 && low_table != 0);
    for (u32 pen = 0; pen < 256; ++pen) {
        r.put16(color_table + pen * 2,
                ((pen >> 4) << 8) | ((pen >> 4) << 4) | (pen >> 4));
        r.put16(low_table + pen * 2,
                ((pen & 0xF) << 8) | ((pen & 0xF) << 4) | (pen & 0xF));
    }
    r.put8(kColorMap + cm_Type, COLORMAP_TYPE_V39);
    r.put16(kColorMap + cm_Count, 256);
    r.put32(kColorMap + cm_ColorTable, color_table);
    r.put32(kColorMap + cm_LowColorBits, low_table);

    MapHook hook{kBitmap, kColorMap};
    r.display.palette = DisplayState::PaletteSource{map_of, &hook};

    r.str(kStrings + 0x40, "hasklig.font");
    r.put32(kTextAttr + ta_Name, kStrings + 0x40);
    r.put16(kTextAttr + ta_YSize, 13);
    r.cpu().a[0] = kTextAttr;
    const u32 font = r.result(Base::Graphics, graphics_lvo::OpenFont);
    CHECK(font != 0);
    set_font(r, kRastPort, font);
    const u32 width = r.get16(font + tf_XSize);
    const u32 baseline = r.get16(font + tf_Baseline);
    const u32 height = r.get16(font + tf_YSize);
    const u8 fg = 200;
    const u8 bg = 40;
    r.graphics.set_pens(kRastPort, fg, bg, JAM2);
    move(r, kRastPort, 3, 20);
    draw_text(r, kRastPort, "H");

    u8 palette[256][3];
    for (u32 pen = 0; pen < 256; ++pen) {
        palette[pen][0] = static_cast<u8>(pen);
        palette[pen][1] = static_cast<u8>(pen);
        palette[pen][2] = static_cast<u8>(pen);
    }
    text_blend::Cache expected;
    expected.set_palette(palette, 256);
    const u8* ramp = expected.ramp(fg, bg);

    const i32 left = 3;
    const i32 top = 20 - static_cast<i32>(baseline);
    std::vector<u32> blended;
    u32 intermediate = 0;
    for (u32 row = 0; row < height; ++row) {
        for (u32 col = 0; col < width; ++col) {
            const u32 pen = read_pixel(r, kRastPort, left + static_cast<i32>(col),
                                       top + static_cast<i32>(row));
            bool known = false;
            for (u32 level = 0; level < text_blend::kLevels; ++level) {
                known = known || pen == static_cast<u32>(ramp[level]);
            }
            CHECK(known);
            blended.push_back(pen);
            if (pen != static_cast<u32>(fg) && pen != static_cast<u32>(bg)) {
                ++intermediate;
            }
        }
    }
    CHECK(intermediate > 0);

    clear_planes(r, planes, plane_bytes);
    r.display.palette = DisplayState::PaletteSource{nullptr, nullptr};
    move(r, kRastPort, 3, 20);
    draw_text(r, kRastPort, "H");
    for (u32 row = 0; row < height; ++row) {
        for (u32 col = 0; col < width; ++col) {
            const u32 pen = read_pixel(r, kRastPort, left + static_cast<i32>(col),
                                       top + static_cast<i32>(row));
            CHECK(pen == static_cast<u32>(fg) || pen == static_cast<u32>(bg));
        }
    }

    clear_planes(r, planes, plane_bytes);
    r.display.palette = DisplayState::PaletteSource{map_of, &hook};
    const u32 gen = r.display.palette_gen;
    r.put32(kViewPort + vp_ColorMap, kColorMap);
    r.cpu().a[0] = kViewPort;
    r.cpu().d[0] = fg;
    r.cpu().d[1] = 0xFF000000u;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = 0;
    CHECK_EQ(r.call(Base::Graphics, graphics_lvo::SetRGB32), ExitReason::Halt);
    CHECK(r.display.palette_gen != gen);
    move(r, kRastPort, 3, 20);
    draw_text(r, kRastPort, "H");
    u32 changed = 0;
    u32 at = 0;
    for (u32 row = 0; row < height; ++row) {
        for (u32 col = 0; col < width; ++col) {
            const u32 pen = read_pixel(r, kRastPort, left + static_cast<i32>(col),
                                       top + static_cast<i32>(row));
            if (pen != blended[at]) {
                ++changed;
            }
            ++at;
        }
    }
    CHECK(changed > 0);
}

// The same on a bitmap of 5, 6 or 7 planes: the ramps are those of its own
// 2^depth colours, written into its own planes. With 4 planes text keeps
// its 1-bit glyphs, palette or not.
void text_blends_on_a_five_to_seven_plane_palette() {
    for (const u32 depth : {4u, 5u, 6u, 7u}) {
        GfxRig r;
        constexpr u32 kWidthN = 64;
        constexpr u32 kHeightN = 32;
        const u32 pens = 1u << depth;
        const u32 plane_bytes = (kWidthN + 15) / 16 * 2 * kHeightN;
        r.cpu().a[0] = kBitmap;
        r.cpu().d[0] = depth;
        r.cpu().d[1] = kWidthN;
        r.cpu().d[2] = kHeightN;
        CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitBitMap), ExitReason::Halt);
        for (u32 plane = 0; plane < depth; ++plane) {
            const u32 at = r.exec.alloc(plane_bytes, MEMF_CHIP | MEMF_CLEAR);
            CHECK(at != 0);
            r.put32(kBitmap + bm_Planes + plane * 4, at);
        }
        r.cpu().a[1] = kRastPort;
        CHECK_EQ(r.call(Base::Graphics, graphics_lvo::InitRastPort), ExitReason::Halt);
        r.put32(kRastPort + rp_BitMap, kBitmap);

        // The greys of the palette, from black to white over its pens.
        u8 palette[256][3]{};
        const u32 color_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
        const u32 low_table = r.exec.alloc(256 * 2, MEMF_CHIP | MEMF_CLEAR);
        CHECK(color_table != 0 && low_table != 0);
        for (u32 pen = 0; pen < pens; ++pen) {
            const u32 grey = pen * 255 / (pens - 1);
            for (u32 gun = 0; gun < 3; ++gun) {
                palette[pen][gun] = static_cast<u8>(grey);
            }
            r.put16(color_table + pen * 2, ((grey >> 4) << 8) | ((grey >> 4) << 4) | (grey >> 4));
            r.put16(low_table + pen * 2,
                    ((grey & 0xF) << 8) | ((grey & 0xF) << 4) | (grey & 0xF));
        }
        r.put8(kColorMap + cm_Type, COLORMAP_TYPE_V39);
        r.put16(kColorMap + cm_Count, pens);
        r.put32(kColorMap + cm_ColorTable, color_table);
        r.put32(kColorMap + cm_LowColorBits, low_table);
        MapHook hook{kBitmap, kColorMap};
        r.display.palette = DisplayState::PaletteSource{map_of, &hook};

        r.str(kStrings + 0x40, "hasklig.font");
        r.put32(kTextAttr + ta_Name, kStrings + 0x40);
        r.put16(kTextAttr + ta_YSize, 13);
        r.cpu().a[0] = kTextAttr;
        const u32 font = r.result(Base::Graphics, graphics_lvo::OpenFont);
        CHECK(font != 0);
        set_font(r, kRastPort, font);
        const u32 width = r.get16(font + tf_XSize);
        const u32 baseline = r.get16(font + tf_Baseline);
        const u32 height = r.get16(font + tf_YSize);
        const u8 fg = static_cast<u8>(pens - 8);
        const u8 bg = 4;
        r.graphics.set_pens(kRastPort, fg, bg, JAM2);
        move(r, kRastPort, 3, 20);
        draw_text(r, kRastPort, "H");

        text_blend::Cache expected;
        expected.set_palette(palette, pens);
        const u8* ramp = expected.ramp(fg, bg);
        CHECK(ramp != nullptr);
        const i32 top = 20 - static_cast<i32>(baseline);
        u32 intermediate = 0;
        for (u32 row = 0; row < height; ++row) {
            for (u32 col = 0; col < width; ++col) {
                const u32 pen = read_pixel(r, kRastPort, 3 + static_cast<i32>(col),
                                           top + static_cast<i32>(row));
                bool known = false;
                for (u32 level = 0; level < text_blend::kLevels; ++level) {
                    known = known || pen == static_cast<u32>(ramp[level]);
                }
                CHECK(known);
                if (pen != static_cast<u32>(fg) && pen != static_cast<u32>(bg)) {
                    ++intermediate;
                }
            }
        }
        // Four planes keep to the two pens, the others have shades between.
        CHECK_EQ(intermediate > 0, text_blend::blends_on(depth));
    }
}

// OpenMonitor hands out the default and the NTSC monitor's MonitorSpecs,
// counting their opens; CloseMonitor counts them down again.
void monitors_open_and_close() {
    GfxRig r;
    r.cpu().a[1] = 0;
    r.cpu().d[0] = 0x8004;
    const u32 spec = r.result(Base::Graphics, graphics_lvo::OpenMonitor);
    CHECK(spec != 0);
    CHECK_EQ(r.get32(spec + 26), 16u);  // ratioh, RATIO_UNITY
    CHECK_EQ(r.get32(spec + 30), 16u);  // ratiov
    CHECK_EQ(r.get16(spec + 48), 1u);   // ms_OpenCount
    r.cpu().a[1] = 0;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenMonitor), spec);
    CHECK_EQ(r.get16(spec + 48), 2u);
    r.cpu().a[0] = spec;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::CloseMonitor), 0u);
    CHECK_EQ(r.get16(spec + 48), 1u);
}

// The display database lists every standard PAL and NTSC mode.
void display_info_presents_ntsc() {
    GfxRig r;
    constexpr u32 kDisplayInfo = kData + 0xE00;
    constexpr u32 kMonitorName = kData + 0xF00;
    constexpr u32 kTags = kData + 0x1000;
    constexpr u32 kHandleBase = 0x10000;
    constexpr u32 kDisp = 0x80000000;
    constexpr u32 kDims = 0x80001000;
    constexpr u32 kMntr = 0x80002000;
    constexpr u32 kName = 0x80003000;
    constexpr u32 kInvalid = 0xFFFFFFFF;
    constexpr u32 kHiresLace = 0x8004;
    constexpr u32 kNtscHiresLace = 0x19004;
    constexpr u32 kPalLores = 0x21000;
    constexpr u32 kPalHam = 0x21800;
    constexpr u32 kPalHiresLace = 0x29004;
    constexpr u32 kMustHave = 0x80000001;
    constexpr u32 kNominalWidth = 0x80000004;
    constexpr u32 kNominalHeight = 0x80000005;
    constexpr u32 kDesiredWidth = 0x80000006;
    constexpr u32 kDesiredHeight = 0x80000007;
    constexpr u32 kDepth = 0x80000008;
    constexpr u32 kMonitorId = 0x80000009;

    r.cpu().d[0] = kInvalid;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::NextDisplayInfo), 0u);
    r.cpu().d[0] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::NextDisplayInfo), 0x4u);
    r.cpu().d[0] = 0x4;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::NextDisplayInfo), 0x8000u);
    r.cpu().d[0] = 0x8000;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::NextDisplayInfo), kHiresLace);
    u32 entries = 4;
    u32 last = kHiresLace;
    while (entries < 60) {
        r.cpu().d[0] = last;
        last = r.result(Base::Graphics, graphics_lvo::NextDisplayInfo);
        if (last == kInvalid) {
            break;
        }
        ++entries;
    }
    CHECK_EQ(entries, 54u);
    r.cpu().d[0] = 0x31004;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::NextDisplayInfo), kInvalid);

    r.cpu().d[0] = 0;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::FindDisplayInfo), kHandleBase);
    r.cpu().d[0] = kHiresLace;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::FindDisplayInfo), kHandleBase + 3);
    r.cpu().d[0] = kNtscHiresLace;
    const u32 ntsc_handle = r.result(Base::Graphics, graphics_lvo::FindDisplayInfo);
    CHECK_EQ(ntsc_handle, kHandleBase + 21);
    r.cpu().d[0] = 0x31004;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::FindDisplayInfo), 0u);

    r.cpu().a[0] = ntsc_handle;
    r.cpu().a[1] = kDisplayInfo;
    r.cpu().d[0] = 56;
    r.cpu().d[1] = kDisp;
    r.cpu().d[2] = kInvalid;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 56u);
    CHECK_EQ(r.get32(kDisplayInfo + 4), kNtscHiresLace);
    CHECK_EQ(r.get16(kDisplayInfo + 16), 0u);
    CHECK_EQ(r.get32(kDisplayInfo + 18), 0x10101u);

    r.cpu().a[0] = ntsc_handle;
    r.cpu().a[1] = kDisplayInfo;
    r.cpu().d[0] = 96;
    r.cpu().d[1] = kMntr;
    r.cpu().d[2] = kInvalid;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 96u);
    CHECK_EQ(r.get32(kDisplayInfo + 84), kNtscHiresLace);
    const u32 ntsc_spec = r.get32(kDisplayInfo + 16);
    CHECK(r.read_str(r.get32(ntsc_spec + LN_NAME)) == "ntsc.monitor");

    r.cpu().a[0] = ntsc_handle;
    r.cpu().a[1] = kDisplayInfo;
    r.cpu().d[0] = 56;
    r.cpu().d[1] = kName;
    r.cpu().d[2] = kInvalid;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 56u);
    CHECK(r.read_str(kDisplayInfo + 16) == "NTSC:High Res Laced");

    r.cpu().a[0] = 0;
    r.cpu().a[1] = kDisplayInfo;
    r.cpu().d[0] = 88;
    r.cpu().d[1] = kDims;
    r.cpu().d[2] = kPalHiresLace;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 88u);
    CHECK_EQ(r.get16(kDisplayInfo + 26), 0u);
    CHECK_EQ(r.get16(kDisplayInfo + 28), 0u);
    CHECK_EQ(r.get16(kDisplayInfo + 30), 639u);
    CHECK_EQ(r.get16(kDisplayInfo + 32), 511u);
    r.cpu().d[0] = 56;
    r.cpu().d[1] = kDisp;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 56u);
    CHECK((r.get32(kDisplayInfo + 18) & 0x20u) != 0);

    r.cpu().d[1] = kDisp;
    r.cpu().d[2] = kPalHam;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 56u);
    const u32 ham_flags = r.get32(kDisplayInfo + 18);
    CHECK((ham_flags & 0x8u) != 0);
    CHECK((ham_flags & 0x100u) == 0);
    r.cpu().d[1] = kName;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 56u);
    CHECK(r.read_str(kDisplayInfo + 16) == "PAL:Low Res HAM");

    r.cpu().a[0] = 0;
    r.cpu().a[1] = kDisplayInfo;
    r.cpu().d[0] = 96;
    r.cpu().d[1] = kMntr;
    r.cpu().d[2] = kPalHiresLace;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 96u);
    CHECK_EQ(r.get16(kDisplayInfo + 36), 625u);
    CHECK_EQ(r.get16(kDisplayInfo + 40), 29u);
    CHECK_EQ(r.get32(kDisplayInfo + 84), kPalHiresLace);
    const u32 pal_spec = r.get32(kDisplayInfo + 16);
    CHECK(r.read_str(r.get32(pal_spec + LN_NAME)) == "pal.monitor");

    r.cpu().d[0] = kPalHiresLace;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::ModeNotAvailable), 0u);
    r.cpu().d[0] = 0x31004;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::ModeNotAvailable), 2u);

    r.put16(kViewPort + vp_Modes, kHiresLace);
    r.cpu().a[0] = kViewPort;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetVPModeID), kHiresLace);
    r.graphics.set_vp_mode_id(kViewPort, kPalHiresLace);
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetVPModeID), kPalHiresLace);
    r.graphics.clear_vp_mode_id(kViewPort);
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetVPModeID), kHiresLace);

    r.cpu().a[1] = r.str(kMonitorName, "ntsc.monitor");
    r.cpu().d[0] = kNtscHiresLace;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenMonitor), ntsc_spec);
    r.cpu().a[1] = r.str(kMonitorName, "pal.monitor");
    r.cpu().d[0] = kPalHiresLace;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::OpenMonitor), pal_spec);
    r.cpu().a[1] = 0;
    r.cpu().d[0] = 0;
    const u32 default_spec = r.result(Base::Graphics, graphics_lvo::OpenMonitor);
    CHECK(default_spec != 0);
    CHECK(default_spec != ntsc_spec);
    CHECK(default_spec != pal_spec);

    r.tags(kTags, {{kNominalWidth, 320}, {kNominalHeight, 256}, {kMonitorId, 0x21000},
                   {TAG_DONE, 0}});
    r.cpu().a[0] = kTags;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::BestModeIDA), kPalLores);
    r.tags(kTags, {{kDesiredWidth, 640}, {kDesiredHeight, 400}, {kDepth, 8},
                   {TAG_DONE, 0}});
    r.cpu().a[0] = kTags;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::BestModeIDA), kHiresLace);
    r.tags(kTags, {{kMustHave, 0x8}, {kMonitorId, 0x21000}, {kDesiredWidth, 320},
                   {kDesiredHeight, 256}, {TAG_DONE, 0}});
    r.cpu().a[0] = kTags;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::BestModeIDA), kPalHam);
}

// The HOMBRE monitor's modes follow the Amiga ones once the kernel has
// listed them, with their own names, depths and monitor.
void display_info_presents_hombre_modes() {
    GfxRig r;
    constexpr u32 kDisplayInfo = kData + 0xE00;
    constexpr u32 kTags = kData + 0x1000;
    constexpr u32 kDisp = 0x80000000;
    constexpr u32 kDims = 0x80001000;
    constexpr u32 kMntr = 0x80002000;
    constexpr u32 kName = 0x80003000;
    constexpr u32 kInvalid = 0xFFFFFFFF;
    constexpr u32 kNominalWidth = 0x80000004;
    constexpr u32 kNominalHeight = 0x80000005;
    constexpr u32 kDepth = 0x80000008;
    const u32 k1080x24 = display_modes::hombre_id(12, 2);
    display_modes::configure_hombre(1280, 800, 512u * 1024 * 1024);

    r.cpu().a[0] = 0;
    r.cpu().a[1] = kDisplayInfo;
    r.cpu().d[0] = 56;
    r.cpu().d[1] = kName;
    r.cpu().d[2] = k1080x24;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 56u);
    CHECK(r.read_str(kDisplayInfo + 16) == "HOMBRE:1920x1080 24bit");
    r.cpu().d[1] = kDisp;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 56u);
    CHECK_EQ(r.get32(kDisplayInfo + 18), 0x80000100u);
    r.cpu().d[0] = 88;
    r.cpu().d[1] = kDims;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 88u);
    CHECK_EQ(r.get16(kDisplayInfo + 16), 24u);
    CHECK_EQ(r.get16(kDisplayInfo + 30), 1919u);
    CHECK_EQ(r.get16(kDisplayInfo + 32), 1079u);
    r.cpu().d[0] = 96;
    r.cpu().d[1] = kMntr;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::GetDisplayInfoData), 96u);
    const u32 spec = r.get32(kDisplayInfo + 16);
    CHECK(r.read_str(r.get32(spec + LN_NAME)) == "hombre.monitor");

    r.put32(kTags, kNominalWidth);
    r.put32(kTags + 4, 1920);
    r.put32(kTags + 8, kNominalHeight);
    r.put32(kTags + 12, 1080);
    r.put32(kTags + 16, kDepth);
    r.put32(kTags + 20, 24);
    r.put32(kTags + 24, 0);
    r.cpu().a[0] = kTags;
    // The best mode is the listed one of the size, whose depth picks the
    // format when a screen opens.
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::BestModeIDA),
             display_modes::hombre_id(12, display_modes::kAnyDepth));

    // The last Amiga mode now leads on to the first HOMBRE one, and that to
    // the next size: the modes of one format are not listed.
    r.cpu().d[0] = display_modes::at(display_modes::kAmigaModeCount - 1).id;
    const u32 after_amiga = r.result(Base::Graphics, graphics_lvo::NextDisplayInfo);
    CHECK_EQ(after_amiga, display_modes::hombre_id(0, display_modes::kAnyDepth));
    r.cpu().d[0] = after_amiga;
    CHECK_EQ(r.result(Base::Graphics, graphics_lvo::NextDisplayInfo),
             display_modes::hombre_id(1, display_modes::kAnyDepth));
    r.cpu().d[0] = kInvalid;
    display_modes::configure_hombre(0, 0, 0);
}

int main() {
    monitors_open_and_close();
    display_info_presents_ntsc();
    display_info_presents_hombre_modes();
    set_soft_style_keeps_the_drawn_styles();
    gfx_base_lists_the_memory_fonts();
    topaz_font_holds_the_built_in_glyphs();
    text_draws_a_proportional_font_from_its_tables();
    system_text_keeps_the_built_in_font();
    text_draw_modes_and_spacing_with_a_proportional_font();
    text_is_measured_with_the_fonts_own_widths();
    text_draws_the_algorithmic_styles();
    text_draws_hasklig_from_the_rom();
    default_font_is_topaz_until_set();
    text_in_draws_and_measures_an_explicit_font();
    plain_text_fits_and_holds_a_font();
    rom_fonts_carry_their_coverage_plane();
    text_blends_on_an_eight_plane_palette();
    text_blends_on_a_five_to_seven_plane_palette();
    extend_font_and_strip_font();
    weigh_ta_match_rates_by_size_style_and_flags();
    text_fit_pens_and_bitmaps_of_v39();
    blt_bitmap_copies_through_the_minterm_and_mask();
    blt_bitmap_rastport_draws_inside_the_rastport();
    init_bitmap_fills_the_fields();
    init_rastport_sets_default_pens();
    set_apen_bpen_drmd_store_fields();
    move_sets_cp();
    rect_fill_fills_pen_and_clips();
    rect_fill_complements_in_complement_mode();
    write_then_read_pixel_vectors();
    draw_moves_cp_and_sets_pixels();
    text_draws_glyphs_advances_cp_and_returns_nothing_unsafe();
    text_length_is_8_per_char();
    set_font_updates_txheight_txbaseline();
    open_font_finds_the_nearest_font_of_the_name();
    add_font_and_rem_font_keep_the_fonts_in_memory();
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
    text_extent_reports_five_characters_of_the_8x8_font();
    area_fills_square_and_triangle_and_empties_the_table();
    area_fill_of_a_shape_without_width_draws_its_whole_line();
    area_draw_past_max_count_returns_minus_one();
    area_ellipse_fills_symmetrically_about_the_centre();
    area_ellipses_fill_together_as_a_ring();
    obtain_pen_shares_and_reserves_pens();
    obtain_pen_keeps_the_chrome_pens_of_shallow_screens();
    area_outline_uses_the_outline_pen();
    draw_ellipse_outline_has_no_gaps_and_leaves_the_inside_clear();
    init_tmp_ras_records_the_buffer_and_wait_blit_is_a_no_op();
    new_region_is_empty_and_dispose_frees_every_rectangle();
    or_rect_region_stores_rectangles_relative_to_the_bounds();
    or_rect_region_joins_overlapping_rectangles_without_overlap();
    and_rect_region_keeps_what_the_rectangle_covers();
    clear_rect_region_cuts_a_hole();
    xor_rect_region_keeps_what_only_one_of_them_covers();
    region_region_operations_change_only_the_destination();
    clear_region_empties_the_region_and_frees_its_rectangles();
    region_calls_without_a_region_or_rectangle_fail();
    clip_region_keeps_fills_inside_its_rectangles();
    clip_region_keeps_lines_text_and_pixels_inside();
    an_empty_clip_region_lets_nothing_through();
    clip_region_of_a_superbitmap_layer_is_window_relative();
    return check::finish("test_graphics");
}
