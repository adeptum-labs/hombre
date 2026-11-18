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

#include "libs/raster.hpp"
#include "check.hpp"

#include <cstring>
#include <vector>

using namespace hombre;
using hombre::m68k::CpuState;
using hombre::m68k::ExitReason;
using hombre::m68k::GuestMemory;
using hombre::m68k::Machine;

namespace {

constexpr u8 kGuardByte = 0xA5;
constexpr u32 kMemoryBytes = 1 << 16;
constexpr u32 kStruct = 0x0800;
constexpr u32 kPlaneBase = 0x1000;
constexpr u32 kWidth = 640;
constexpr u32 kBytesPerRow = kWidth / 8;
constexpr u32 kRows = 16;
constexpr u32 kDepth = 2;
constexpr u32 kPlaneBytes = kBytesPerRow * kRows;
constexpr u32 kPlanesBytes = 8 * kPlaneBytes;
constexpr raster::Clip kFullClip{0, 0, 639, 15};

// A flat guest memory of guard bytes and a Machine without a Jit, whose
// bitmap planes lie at kPlaneBase.
class Rig {
public:
    Rig() : memory_(kMemoryBytes, kGuardByte) {
        m_.cpu = CpuState{};
        m_.mem = GuestMemory{memory_.data(), kMemoryBytes};
        m_.jit = nullptr;
    }

    Machine& machine() { return m_; }
    native::Bus bus() { return native::Bus(m_); }

    raster::Bitmap bitmap() const {
        raster::Bitmap bm{};
        bm.bytes_per_row = kBytesPerRow;
        bm.rows = kRows;
        bm.depth = kDepth;
        bm.addr = kPlaneBase;
        for (u32 plane = 0; plane < 8; ++plane) {
            bm.planes[plane] = kPlaneBase + plane * kPlaneBytes;
        }
        return bm;
    }

    void clear_planes() { std::memset(memory_.data() + kPlaneBase, 0, kPlanesBytes); }

    std::vector<u8> snapshot() const {
        return std::vector<u8>(memory_.begin() + kPlaneBase,
                               memory_.begin() + kPlaneBase + kPlanesBytes);
    }

    u8 plane_byte(u32 plane, u32 row, u32 byte) const {
        return memory_[kPlaneBase + plane * kPlaneBytes + row * kBytesPerRow + byte];
    }

    void put8(u32 at, u32 v) { memory_[at] = static_cast<u8>(v); }

    void put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
    }

    void put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
    }

private:
    std::vector<u8> memory_;
    Machine m_;
};

void write_then_read_pixel_roundtrips_every_pen() {
    Rig r;
    r.clear_planes();
    const raster::Bitmap bm = r.bitmap();
    const i32 xs[] = {0, 1, 7, 8, 9, 63, 64, 639};
    const i32 ys[] = {0, 1, 5, 15};
    for (u32 pen = 0; pen <= 3; ++pen) {
        for (i32 y : ys) {
            for (i32 x : xs) {
                native::Bus b = r.bus();
                raster::write_pixel(b, bm, x, y, pen);
                CHECK(b.ok());
                CHECK_EQ(raster::read_pixel(b, bm, x, y), pen);
            }
        }
    }
}

void read_outside_gives_all_ones() {
    Rig r;
    r.clear_planes();
    const raster::Bitmap bm = r.bitmap();
    native::Bus b = r.bus();
    CHECK_EQ(raster::read_pixel(b, bm, -1, 0), 0xFFFFFFFFu);
    CHECK_EQ(raster::read_pixel(b, bm, 0, -1), 0xFFFFFFFFu);
    CHECK_EQ(raster::read_pixel(b, bm, 640, 0), 0xFFFFFFFFu);
    CHECK_EQ(raster::read_pixel(b, bm, 0, 16), 0xFFFFFFFFu);
    CHECK_EQ(raster::read_pixel(b, bm, -1000, -1000), 0xFFFFFFFFu);
    CHECK(b.ok());
}

void fill_rect_clips_to_clip_and_reports_dirty() {
    Rig r;
    const raster::Bitmap bm = r.bitmap();
    const raster::Clip clip{8, 2, 15, 5};
    const std::vector<u8> before = r.snapshot();
    native::Bus b = r.bus();
    const Rect dirty = raster::fill_rect(b, bm, clip, Rect{4, 0, 12, 3}, 1);
    CHECK(b.ok());
    CHECK_EQ(dirty.x0, 8);
    CHECK_EQ(dirty.y0, 2);
    CHECK_EQ(dirty.x1, 12);
    CHECK_EQ(dirty.y1, 3);

    constexpr u8 kColumns = 0xF8;  // x 8 to 12 of the byte that holds them
    for (u32 plane = 0; plane < 8; ++plane) {
        for (u32 row = 0; row < kRows; ++row) {
            for (u32 byte = 0; byte < kBytesPerRow; ++byte) {
                u8 expected = before[plane * kPlaneBytes + row * kBytesPerRow + byte];
                if ((row == 2 || row == 3) && byte == 1) {
                    if (plane == 0) {
                        expected = static_cast<u8>(expected | kColumns);
                    } else if (plane == 1) {
                        expected = static_cast<u8>(expected & ~kColumns);
                    }
                }
                CHECK_EQ(r.plane_byte(plane, row, byte), expected);
            }
        }
    }
    for (i32 y = 2; y <= 3; ++y) {
        for (i32 x = 8; x <= 12; ++x) {
            CHECK_EQ(raster::read_pixel(b, bm, x, y), 1u);
        }
    }
}

void fill_rect_empty_when_outside() {
    Rig r;
    const raster::Bitmap bm = r.bitmap();
    const raster::Clip clip{8, 2, 15, 5};
    const std::vector<u8> before = r.snapshot();
    native::Bus b = r.bus();
    const Rect rects[] = {Rect{-20, -20, -1, -1}, Rect{16, 2, 20, 5}, Rect{8, 6, 15, 9},
                          Rect{8, -10, 15, 1}};
    for (const Rect& rect : rects) {
        const Rect dirty = raster::fill_rect(b, bm, clip, rect, 2);
        CHECK(dirty.x1 < dirty.x0);
    }
    CHECK(b.ok());
    CHECK(r.snapshot() == before);
}

void draw_line_horizontal_vertical_diagonal() {
    Rig r;
    const raster::Bitmap bm = r.bitmap();
    native::Bus b = r.bus();

    r.clear_planes();
    Rect dirty = raster::draw_line(b, bm, kFullClip, 2, 3, 10, 3, 1);
    CHECK(b.ok());
    CHECK_EQ(dirty.x0, 2);
    CHECK_EQ(dirty.y0, 3);
    CHECK_EQ(dirty.x1, 10);
    CHECK_EQ(dirty.y1, 3);
    for (i32 x = 0; x < 12; ++x) {
        CHECK_EQ(raster::read_pixel(b, bm, x, 3), x >= 2 && x <= 10 ? 1u : 0u);
    }

    r.clear_planes();
    dirty = raster::draw_line(b, bm, kFullClip, 5, 0, 5, 7, 2);
    CHECK_EQ(dirty.x0, 5);
    CHECK_EQ(dirty.y0, 0);
    CHECK_EQ(dirty.x1, 5);
    CHECK_EQ(dirty.y1, 7);
    for (i32 y = 0; y < 9; ++y) {
        CHECK_EQ(raster::read_pixel(b, bm, 5, y), y <= 7 ? 2u : 0u);
    }

    r.clear_planes();
    dirty = raster::draw_line(b, bm, kFullClip, 0, 0, 5, 5, 3);
    CHECK_EQ(dirty.x0, 0);
    CHECK_EQ(dirty.y0, 0);
    CHECK_EQ(dirty.x1, 5);
    CHECK_EQ(dirty.y1, 5);
    for (i32 i = 0; i <= 5; ++i) {
        CHECK_EQ(raster::read_pixel(b, bm, i, i), 3u);
    }
    CHECK_EQ(raster::read_pixel(b, bm, 1, 0), 0u);
    CHECK_EQ(raster::read_pixel(b, bm, 0, 1), 0u);
}

void draw_text_jam1_sets_only_fg() {
    Rig r;
    r.clear_planes();
    const raster::Bitmap bm = r.bitmap();
    native::Bus b = r.bus();
    const raster::Pens pens{1, 2, ndk::JAM1};
    const u8 text[] = {'H'};
    u32 advance = 0;
    const Rect dirty = raster::draw_text(b, bm, kFullClip, pens, 8, 8, text, 1, advance);
    CHECK(b.ok());
    CHECK_EQ(advance, 8u);
    const u8* rows = glyph('H');
    for (u32 row = 0; row < 8; ++row) {
        for (u32 col = 0; col < 8; ++col) {
            const u32 expected = ((rows[row] >> col) & 1) != 0 ? 1u : 0u;
            CHECK_EQ(raster::read_pixel(b, bm, 8 + static_cast<i32>(col),
                                        2 + static_cast<i32>(row)),
                     expected);
        }
    }
    CHECK_EQ(raster::read_pixel(b, bm, 7, 2), 0u);
    CHECK_EQ(raster::read_pixel(b, bm, 16, 2), 0u);
    CHECK(dirty.x1 >= dirty.x0);
}

void draw_text_jam2_fills_bg() {
    Rig r;
    r.clear_planes();
    const raster::Bitmap bm = r.bitmap();
    native::Bus b = r.bus();
    raster::fill_rect(b, bm, kFullClip, Rect{4, 2, 19, 9}, 3);
    const raster::Pens pens{1, 2, ndk::JAM2};
    const u8 text[] = {'H'};
    u32 advance = 0;
    const Rect dirty = raster::draw_text(b, bm, kFullClip, pens, 8, 8, text, 1, advance);
    CHECK(b.ok());
    CHECK_EQ(advance, 8u);
    const u8* rows = glyph('H');
    for (u32 row = 0; row < 8; ++row) {
        for (u32 col = 0; col < 8; ++col) {
            const u32 expected = ((rows[row] >> col) & 1) != 0 ? 1u : 2u;
            CHECK_EQ(raster::read_pixel(b, bm, 8 + static_cast<i32>(col),
                                        2 + static_cast<i32>(row)),
                     expected);
        }
    }
    CHECK_EQ(raster::read_pixel(b, bm, 7, 2), 3u);
    CHECK_EQ(raster::read_pixel(b, bm, 16, 2), 3u);
    CHECK_EQ(dirty.x0, 8);
    CHECK_EQ(dirty.y0, 2);
    CHECK_EQ(dirty.x1, 15);
    CHECK_EQ(dirty.y1, 9);
}

void draw_text_complement_twice_restores() {
    Rig r;
    r.clear_planes();
    const raster::Bitmap bm = r.bitmap();
    native::Bus b = r.bus();
    raster::fill_rect(b, bm, kFullClip, Rect{8, 2, 15, 9}, 2);
    const std::vector<u8> before = r.snapshot();
    const raster::Pens pens{1, 2, ndk::COMPLEMENT};
    const u8 text[] = {'H'};
    u32 advance = 0;
    raster::draw_text(b, bm, kFullClip, pens, 8, 8, text, 1, advance);
    CHECK(b.ok());
    CHECK_EQ(advance, 8u);
    CHECK(r.snapshot() != before);
    CHECK_EQ(raster::read_pixel(b, bm, 10, 2), 2u);
    CHECK_EQ(raster::read_pixel(b, bm, 8, 2), 3u);
    raster::draw_text(b, bm, kFullClip, pens, 8, 8, text, 1, advance);
    CHECK(r.snapshot() == before);
}

void draw_text_clips_at_clip_edge() {
    Rig r;
    r.clear_planes();
    const raster::Bitmap bm = r.bitmap();
    const raster::Clip clip{0, 0, 11, 15};
    native::Bus b = r.bus();
    const raster::Pens pens{1, 0, ndk::JAM1};
    const u8 text[] = {'H', 'H'};
    u32 advance = 0;
    const Rect dirty = raster::draw_text(b, bm, clip, pens, 8, 8, text, 2, advance);
    CHECK(b.ok());
    CHECK_EQ(advance, 16u);
    CHECK(dirty.x1 <= 11);
    const u8* rows = glyph('H');
    for (u32 row = 0; row < 8; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            const u32 expected = ((rows[row] >> col) & 1) != 0 ? 1u : 0u;
            CHECK_EQ(raster::read_pixel(b, bm, 8 + static_cast<i32>(col),
                                        2 + static_cast<i32>(row)),
                     expected);
        }
    }
    for (i32 x = 12; x < 24; ++x) {
        CHECK_EQ(raster::read_pixel(b, bm, x, 2), 0u);
    }
}

void scroll_rect_up_one_row_moves_and_clears_bottom() {
    Rig r;
    r.clear_planes();
    const raster::Bitmap bm = r.bitmap();
    native::Bus b = r.bus();
    const Rect area{0, 0, 15, 5};
    for (i32 y = 0; y <= 5; ++y) {
        for (i32 x = 0; x <= 15; ++x) {
            raster::write_pixel(b, bm, x, y, static_cast<u32>((x + y) & 3));
        }
    }
    const std::vector<u8> before = r.snapshot();
    const Rect dirty = raster::scroll_rect(b, bm, kFullClip, 0, 1, area, 0);
    CHECK(b.ok());
    CHECK_EQ(dirty.x0, 0);
    CHECK_EQ(dirty.y0, 0);
    CHECK_EQ(dirty.x1, 15);
    CHECK_EQ(dirty.y1, 5);
    for (i32 y = 0; y <= 4; ++y) {
        for (i32 x = 0; x <= 15; ++x) {
            CHECK_EQ(raster::read_pixel(b, bm, x, y), static_cast<u32>((x + y + 1) & 3));
        }
    }
    for (i32 x = 0; x <= 15; ++x) {
        CHECK_EQ(raster::read_pixel(b, bm, x, 5), 0u);
    }
    const std::vector<u8> after = r.snapshot();
    for (u32 plane = 0; plane < 8; ++plane) {
        for (u32 row = 0; row < kRows; ++row) {
            for (u32 byte = 0; byte < kBytesPerRow; ++byte) {
                if (row > 5 || byte >= 2) {
                    const u32 at = plane * kPlaneBytes + row * kBytesPerRow + byte;
                    CHECK_EQ(after[at], before[at]);
                }
            }
        }
    }
}

void load_bitmap_rejects_depth_over_8() {
    Rig r;
    native::Bus b = r.bus();
    raster::Bitmap bm{};
    r.put16(kStruct + ndk::bm_BytesPerRow, 80);
    r.put16(kStruct + ndk::bm_Rows, 16);
    r.put8(kStruct + ndk::bm_Flags, 0);
    r.put8(kStruct + ndk::bm_Depth, 2);
    for (u32 plane = 0; plane < 8; ++plane) {
        r.put32(kStruct + ndk::bm_Planes + plane * 4, 0x2000 + plane * 0x100);
    }
    CHECK(raster::load_bitmap(b, kStruct, bm));
    CHECK(b.ok());
    CHECK_EQ(bm.bytes_per_row, 80u);
    CHECK_EQ(bm.rows, 16u);
    CHECK_EQ(bm.depth, 2u);
    CHECK_EQ(bm.addr, kStruct);
    for (u32 plane = 0; plane < 8; ++plane) {
        CHECK_EQ(bm.planes[plane], 0x2000u + plane * 0x100u);
    }

    r.put8(kStruct + ndk::bm_Depth, 9);
    CHECK(!raster::load_bitmap(b, kStruct, bm));
    CHECK(b.ok());
    CHECK_EQ(bm.depth, 9u);

    r.put8(kStruct + ndk::bm_Depth, 8);
    CHECK(raster::load_bitmap(b, kStruct, bm));
}

void bad_bitmap_address_faults_the_bus() {
    Rig r;
    native::Bus b = r.bus();
    raster::Bitmap bm{};
    CHECK(!raster::load_bitmap(b, kMemoryBytes, bm));
    CHECK(!b.ok());
    CHECK(r.machine().cpu.exit == ExitReason::BusError);
    CHECK_EQ(r.machine().cpu.fault_addr, kMemoryBytes);
}

void load_bitmap_rejects_planes_outside_guest_memory() {
    Rig r;
    native::Bus b = r.bus();
    r.put16(kStruct + ndk::bm_BytesPerRow, 0xFFFF);
    r.put16(kStruct + ndk::bm_Rows, 0xFFFF);
    r.put8(kStruct + ndk::bm_Depth, 2);
    r.put32(kStruct + ndk::bm_Planes, kPlaneBase);
    r.put32(kStruct + ndk::bm_Planes + 4, kPlaneBase + 0x100);
    raster::Bitmap bm{};
    CHECK(!raster::load_bitmap(b, kStruct, bm));
    CHECK(!b.ok());
    CHECK(r.machine().cpu.exit == ExitReason::BusError);
}

void huge_fill_stops_at_the_first_fault() {
    Rig r;
    native::Bus b = r.bus();
    raster::Bitmap bm{};
    bm.bytes_per_row = 0xFFFF;
    bm.rows = 0xFFFF;
    bm.depth = 2;
    bm.planes[0] = kPlaneBase;
    bm.planes[1] = kPlaneBase;
    const raster::Clip clip{0, 0, 0x7FFFFFF, 0x7FFFFFF};
    raster::fill_rect(b, bm, clip, Rect{0, 0, 0x7FFFFFF, 0x7FFFFFF}, 3);
    CHECK(!b.ok());
    CHECK(r.machine().cpu.exit == ExitReason::BusError);
    raster::scroll_rect(b, bm, clip, 0, 1, Rect{0, 0, 0x7FFFFFF, 0x7FFFFFF}, 0);
    CHECK(!b.ok());
}

void load_bitmap_rejects_a_depth_zero_bitmap_larger_than_guest_memory() {
    Rig r;
    native::Bus b = r.bus();
    r.put16(kStruct + ndk::bm_BytesPerRow, 0xFFFF);
    r.put16(kStruct + ndk::bm_Rows, 0xFFFF);
    r.put8(kStruct + ndk::bm_Depth, 0);
    raster::Bitmap bm{};
    CHECK(!raster::load_bitmap(b, kStruct, bm));
    CHECK(!b.ok());
}

}

int main() {
    write_then_read_pixel_roundtrips_every_pen();
    read_outside_gives_all_ones();
    fill_rect_clips_to_clip_and_reports_dirty();
    fill_rect_empty_when_outside();
    draw_line_horizontal_vertical_diagonal();
    draw_text_jam1_sets_only_fg();
    draw_text_jam2_fills_bg();
    draw_text_complement_twice_restores();
    draw_text_clips_at_clip_edge();
    scroll_rect_up_one_row_moves_and_clears_bottom();
    load_bitmap_rejects_depth_over_8();
    bad_bitmap_address_faults_the_bus();
    load_bitmap_rejects_planes_outside_guest_memory();
    huge_fill_stops_at_the_first_fault();
    load_bitmap_rejects_a_depth_zero_bitmap_larger_than_guest_memory();
    return check::finish("test_raster");
}
