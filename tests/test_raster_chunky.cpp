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


#include "libs/deep_bitmaps.hpp"
#include "libs/raster_text.hpp"
#include "check.hpp"

#include <cstring>
#include <vector>

using namespace hombre;
using hombre::m68k::CpuState;
using hombre::m68k::GuestMemory;
using hombre::m68k::Machine;

namespace {

constexpr u8 kGuard = 0xA5;
constexpr u32 kMemoryBytes = 1 << 20;
constexpr u32 kPixels = 0x10000;
constexpr u32 kPlanar = 0x80000;
constexpr u32 kWidth = 40;
constexpr u32 kHeight = 10;

class Rig {
public:
    Rig() : memory_(kMemoryBytes, kGuard) {
        m_.cpu = CpuState{};
        m_.mem = GuestMemory{memory_.data(), kMemoryBytes};
        m_.jit = nullptr;
        palette_[0] = 0x000000;
        palette_[1] = 0xFFFFFF;
        palette_[2] = 0xFF0000;
        palette_[3] = 0x0000FF;
    }

    native::Bus bus() { return native::Bus(m_); }

    raster::Bitmap deep(PixFmt fmt, u32 base = kPixels) {
        raster::Bitmap bm{};
        bm.deep = true;
        bm.fmt = fmt;
        bm.width = kWidth;
        bm.rows = kHeight;
        bm.bytes_per_row = (kWidth * bytes_per_pixel(fmt) + 15) & ~15u;
        bm.depth = fmt == PixFmt::Lut8 ? 8 : fmt == PixFmt::Rgb16 ? 16 : 24;
        bm.pixels = base;
        bm.addr = base;
        bm.palette = palette_;
        std::memset(memory_.data() + base, 0, bm.bytes_per_row * kHeight);
        return bm;
    }

    // A two-plane planar bitmap of kWidth pixels, cleared.
    raster::Bitmap planar() {
        raster::Bitmap bm{};
        bm.bytes_per_row = kWidth / 8;
        bm.rows = kHeight;
        bm.depth = 2;
        bm.addr = kPlanar;
        for (u32 p = 0; p < 8; ++p) {
            bm.planes[p] = kPlanar + p * 0x100;
        }
        std::memset(memory_.data() + kPlanar, 0, 0x800);
        return bm;
    }

    u32 at(const raster::Bitmap& bm, u32 x, u32 y) const {
        return load_pixel(bm.fmt, memory_.data() + bm.pixels + y * bm.bytes_per_row +
                                      x * bytes_per_pixel(bm.fmt));
    }

    u8 byte(u32 address) const { return memory_[address]; }
    u8* raw(u32 address) { return memory_.data() + address; }
    u32 palette_[256]{};

private:
    std::vector<u8> memory_;
    Machine m_{};
};

constexpr raster::Clip kClip{0, 0, kWidth - 1, kHeight - 1};

void fill_rect_each_format() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap lut = r.deep(PixFmt::Lut8);
    raster::fill_rect(b, lut, kClip, Rect{2, 1, 5, 3}, 7);
    CHECK_EQ(r.at(lut, 2, 1), 7u);
    CHECK_EQ(r.at(lut, 5, 3), 7u);
    CHECK_EQ(r.at(lut, 6, 3), 0u);
    CHECK_EQ(r.at(lut, 2, 4), 0u);
    const raster::Bitmap rgb = r.deep(PixFmt::Rgb16);
    raster::fill_rect(b, rgb, kClip, Rect{0, 0, 3, 0}, 2);
    CHECK_EQ(r.at(rgb, 3, 0), 0xF800u);
    CHECK_EQ(r.at(rgb, 4, 0), 0u);
    const raster::Bitmap argb = r.deep(PixFmt::Argb32);
    const Rect dirty = raster::fill_rect(b, argb, kClip, Rect{1, 1, 1, 1}, 3);
    CHECK_EQ(r.at(argb, 1, 1), 0xFF0000FFu);
    CHECK_EQ(dirty.x0, 1);
    CHECK_EQ(dirty.y1, 1);
}

void clip_keeps_writes_inside_bitmap() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap argb = r.deep(PixFmt::Argb32);
    const u32 end = argb.pixels + argb.bytes_per_row * kHeight;
    raster::fill_rect(b, argb, raster::Clip{-50, -50, 500, 500}, Rect{-10, -10, 100, 100}, 1);
    CHECK_EQ(r.at(argb, kWidth - 1, kHeight - 1), 0xFFFFFFFFu);
    CHECK_EQ(r.byte(end), kGuard);
    CHECK_EQ(r.byte(argb.pixels - 1), kGuard);
    raster::draw_line(b, argb, kClip, -20, 5, 200, 5, 2);
    CHECK_EQ(r.byte(end), kGuard);
    raster::write_pixel(b, argb, kWidth, 0, 2);
    raster::write_pixel(b, argb, -1, 0, 2);
    CHECK_EQ(r.at(argb, 0, 0), 0xFFFFFFFFu);
}

void draw_line_and_pixels() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap rgb = r.deep(PixFmt::Rgb16);
    raster::draw_line(b, rgb, kClip, 0, 0, 4, 4, 1);
    CHECK_EQ(r.at(rgb, 2, 2), 0xFFFFu);
    CHECK_EQ(r.at(rgb, 2, 3), 0u);
    CHECK_EQ(raster::read_pixel(b, rgb, 2, 2), 1u);
    CHECK_EQ(raster::read_pixel(b, rgb, 2, 3), 0u);
    const raster::Bitmap lut = r.deep(PixFmt::Lut8);
    raster::write_pixel(b, lut, 3, 3, 200);
    CHECK_EQ(raster::read_pixel(b, lut, 3, 3), 200u);
}

void complement_each_format() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap lut = r.deep(PixFmt::Lut8);
    raster::write_pixel(b, lut, 0, 0, 5);
    raster::complement_rect(b, lut, kClip, Rect{0, 0, 0, 0}, 0xFF);
    CHECK_EQ(r.at(lut, 0, 0), 0xFAu);
    const raster::Bitmap argb = r.deep(PixFmt::Argb32);
    raster::write_pixel(b, argb, 0, 0, 2);
    raster::complement_rect(b, argb, kClip, Rect{0, 0, 0, 0}, 0xFF);
    CHECK_EQ(r.at(argb, 0, 0) & 0xFFFFFFu, 0x00FFFFu);
}

void copy_rect_same_format_overlapping() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap argb = r.deep(PixFmt::Argb32);
    for (u32 x = 0; x < 6; ++x) {
        raster::write_pixel(b, argb, static_cast<i32>(x), 0, x % 4);
    }
    // Right by two inside one row: the source is read before it is written.
    raster::copy_rect(b, argb, 0, 0, argb, 2, 0, 6, 1);
    CHECK_EQ(r.at(argb, 2, 0), 0xFF000000u);
    CHECK_EQ(r.at(argb, 3, 0), 0xFFFFFFFFu);
    CHECK_EQ(r.at(argb, 4, 0), 0xFFFF0000u);
    // Rows 0 and 1 down by one: row 1 takes row 0, row 2 the old row 1.
    raster::copy_rect(b, argb, 0, 0, argb, 0, 1, 8, 2);
    CHECK_EQ(r.at(argb, 3, 1), 0xFFFFFFFFu);
    CHECK_EQ(r.at(argb, 3, 2), 0u);
}

void copy_rect_planar_to_argb32_uses_palette() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap planar = r.planar();
    raster::write_pixel(b, planar, 1, 0, 2);
    raster::write_pixel(b, planar, 2, 0, 3);
    const raster::Bitmap argb = r.deep(PixFmt::Argb32);
    raster::copy_rect(b, planar, 0, 0, argb, 10, 5, 4, 1);
    CHECK_EQ(r.at(argb, 10, 5), 0xFF000000u);
    CHECK_EQ(r.at(argb, 11, 5), 0xFFFF0000u);
    CHECK_EQ(r.at(argb, 12, 5), 0xFF0000FFu);
    // And a Lut8 bitmap takes the pens themselves.
    const raster::Bitmap lut = r.deep(PixFmt::Lut8, kPixels + 0x4000);
    raster::copy_rect(b, planar, 0, 0, lut, 0, 0, 4, 1);
    CHECK_EQ(r.at(lut, 2, 0), 3u);
    // Deep to planar goes by the nearest pen.
    raster::copy_rect(b, argb, 10, 5, planar, 20, 3, 4, 1);
    CHECK_EQ(raster::read_pixel(b, planar, 21, 3), 2u);
    CHECK_EQ(raster::read_pixel(b, planar, 22, 3), 3u);
}

void scroll_rect_each_direction() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap rgb = r.deep(PixFmt::Rgb16);
    raster::write_pixel(b, rgb, 5, 5, 1);
    raster::scroll_rect(b, rgb, kClip, 0, 2, Rect{0, 0, kWidth - 1, kHeight - 1}, 0);
    CHECK_EQ(r.at(rgb, 5, 3), 0xFFFFu);
    CHECK_EQ(r.at(rgb, 5, 5), 0u);
    raster::scroll_rect(b, rgb, kClip, 3, 0, Rect{0, 0, kWidth - 1, kHeight - 1}, 2);
    CHECK_EQ(r.at(rgb, 2, 3), 0xFFFFu);
    CHECK_EQ(r.at(rgb, kWidth - 1, 3), 0xF800u);
}

void text_each_format() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Font font = raster::builtin_font();
    for (PixFmt fmt : {PixFmt::Lut8, PixFmt::Rgb16, PixFmt::Argb32}) {
        const raster::Bitmap bm = r.deep(fmt);
        u32 advance = 0;
        const raster::Pens pens{1, 0, ndk::JAM1};
        const Rect dirty = raster::draw_text(b, bm, kClip, pens, font, raster::TextStyle{}, 0,
                                             font.baseline, reinterpret_cast<const u8*>("I"),
                                             1, advance);
        CHECK(dirty.x1 >= dirty.x0);
        u32 inked = 0;
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                inked += raster::read_pixel(b, bm, static_cast<i32>(x), static_cast<i32>(y)) == 1;
            }
        }
        CHECK(inked >= 6);
    }
}

void load_bitmap_finds_registered_deep_bitmaps() {
    Rig r;
    native::Bus b = r.bus();
    DeepBitmaps registry;
    const u32 bitmap = 0x2000;
    // A BitMap whose depth says 24, but planar until registered.
    std::memset(r.raw(bitmap), 0, 40);
    r.raw(bitmap)[0] = 0;
    r.raw(bitmap)[1] = 160;
    r.raw(bitmap)[3] = kHeight;
    r.raw(bitmap)[5] = 24;
    raster::set_deep_lookup(&registry);
    raster::Bitmap out{};
    CHECK(!raster::load_bitmap(b, bitmap, out));
    DeepBitmap entry{bitmap, kPixels, 160 * kHeight, kWidth, kHeight, 160, PixFmt::Argb32};
    entry.palette = r.palette_;
    CHECK(registry.add(entry));
    CHECK(raster::load_bitmap(b, bitmap, out));
    CHECK(out.deep);
    CHECK_EQ(out.width, kWidth);
    CHECK(out.fmt == PixFmt::Argb32);
    raster::set_deep_lookup(nullptr);
}

}

int main() {
    fill_rect_each_format();
    clip_keeps_writes_inside_bitmap();
    draw_line_and_pixels();
    complement_each_format();
    copy_rect_same_format_overlapping();
    copy_rect_planar_to_argb32_uses_palette();
    scroll_rect_each_direction();
    text_each_format();
    load_bitmap_finds_registered_deep_bitmaps();
    return check::finish("test_raster_chunky");
}
