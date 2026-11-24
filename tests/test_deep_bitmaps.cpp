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

namespace {

constexpr u32 kBmaHeight = 0;
constexpr u32 kBmaDepth = 4;
constexpr u32 kBmaWidth = 8;
constexpr u32 kBmaFlags = 12;
constexpr u32 kBmfStandard = 1u << 3;
constexpr u32 kBmfSpecialFmt = 1u << 7;
constexpr u32 kPixFmtRgb16 = 5;
constexpr u32 kPixFmtArgb32 = 11;

u32 alloc_bitmap(GfxRig& r, u32 width, u32 height, u32 depth, u32 flags, u32 friend_bm) {
    r.cpu().d[0] = width;
    r.cpu().d[1] = height;
    r.cpu().d[2] = depth;
    r.cpu().d[3] = flags;
    r.cpu().a[0] = friend_bm;
    return r.result(Base::Graphics, hombre::graphics_lvo::AllocBitMap);
}

u32 attr(GfxRig& r, u32 bitmap, u32 which) {
    r.cpu().a[0] = bitmap;
    r.cpu().d[1] = which;
    return r.result(Base::Graphics, hombre::graphics_lvo::GetBitMapAttr);
}

void alloc_bitmap_specialfmt_rgb16() {
    GfxRig r;
    const u32 bm = alloc_bitmap(r, 100, 50, 16, kBmfSpecialFmt | (kPixFmtRgb16 << 24), 0);
    CHECK(bm != 0);
    CHECK_EQ(attr(r, bm, kBmaWidth), 100u);
    CHECK_EQ(attr(r, bm, kBmaHeight), 50u);
    CHECK_EQ(attr(r, bm, kBmaDepth), 16u);
    CHECK_EQ(attr(r, bm, kBmaFlags) & kBmfStandard, 0u);
    const hombre::DeepBitmap* deep = r.graphics.deep(bm);
    CHECK(deep != nullptr);
    CHECK(deep->fmt == PixFmt::Rgb16);
    CHECK_EQ(deep->bytes_per_row, 208u);
    CHECK_EQ(r.get16(bm + ndk::bm_BytesPerRow), 208u);
    CHECK_EQ(r.get32(bm + ndk::bm_Planes), deep->pixels);
    CHECK(deep->pixels >= kTestChipEnd);
}

void alloc_bitmap_with_deep_friend() {
    GfxRig r;
    const u32 screen = alloc_bitmap(r, 64, 32, 24, kBmfSpecialFmt | (kPixFmtArgb32 << 24), 0);
    CHECK_EQ(attr(r, screen, kBmaDepth), 24u);
    const u32 bm = alloc_bitmap(r, 10, 10, 8, 0, screen);
    const hombre::DeepBitmap* deep = r.graphics.deep(bm);
    CHECK(deep != nullptr);
    CHECK(deep->fmt == PixFmt::Argb32);
    CHECK_EQ(attr(r, bm, kBmaDepth), 24u);
    // A planar friend gives a planar bitmap, as before.
    const u32 planar = alloc_bitmap(r, 10, 10, 3, 0, 0);
    CHECK(r.graphics.deep(planar) == nullptr);
    CHECK(r.graphics.deep(alloc_bitmap(r, 10, 10, 2, 0, planar)) == nullptr);
}

void deep_depths_without_a_format() {
    GfxRig r;
    const u32 bm16 = alloc_bitmap(r, 8, 8, 16, 0, 0);
    CHECK(r.graphics.deep(bm16) != nullptr);
    CHECK(r.graphics.deep(bm16)->fmt == PixFmt::Rgb16);
    const u32 bm24 = alloc_bitmap(r, 8, 8, 24, 0, 0);
    CHECK(r.graphics.deep(bm24)->fmt == PixFmt::Argb32);
}

void alloc_deep_comes_from_fast_memory() {
    GfxRig r;
    const u32 before = r.exec.free_bytes(ndk::MEMF_FAST);
    const u32 bm = r.graphics.alloc_deep_bitmap(320, 200, PixFmt::Argb32);
    CHECK(bm != 0);
    CHECK(r.exec.free_bytes(ndk::MEMF_FAST) <= before - 320 * 200 * 4);
    r.cpu().a[0] = bm;
    r.call(Base::Graphics, hombre::graphics_lvo::FreeBitMap);
    CHECK(r.graphics.deep(bm) == nullptr);
    CHECK_EQ(r.exec.free_bytes(ndk::MEMF_FAST), before);
}

void forged_planar_bitmap_with_depth_24_is_planar() {
    GfxRig r;
    const u32 forged = kData + 0x100;
    for (u32 i = 0; i < ndk::BitMap_SIZEOF; ++i) {
        r.put8(forged + i, 0);
    }
    r.put16(forged + ndk::bm_BytesPerRow, 4);
    r.put16(forged + ndk::bm_Rows, 2);
    r.put8(forged + ndk::bm_Depth, 24);
    CHECK(r.graphics.deep(forged) == nullptr);
    CHECK_EQ(attr(r, forged, kBmaWidth), 32u);
}

void alloc_deep_out_of_memory_returns_0_and_leaks_nothing() {
    GfxRig r;
    const u32 any = r.exec.free_bytes();
    CHECK_EQ(r.graphics.alloc_deep_bitmap(30000, 30000, PixFmt::Argb32), 0u);
    CHECK_EQ(alloc_bitmap(r, 30000, 30000, 24, 0, 0), 0u);
    CHECK_EQ(r.exec.free_bytes(), any);
}

}

int main() {
    alloc_bitmap_specialfmt_rgb16();
    alloc_bitmap_with_deep_friend();
    deep_depths_without_a_format();
    alloc_deep_comes_from_fast_memory();
    forged_planar_bitmap_with_depth_24_is_planar();
    alloc_deep_out_of_memory_returns_0_and_leaks_nothing();
    return check::finish("test_deep_bitmaps");
}
