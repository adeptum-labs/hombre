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

#include <algorithm>
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
constexpr u32 kSecondPlaneBase = kPlaneBase + kPlanesBytes;
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

    raster::Bitmap bitmap() const { return bitmap_at(kPlaneBase, kDepth); }

    raster::Bitmap bitmap_at(u32 base, u32 depth = kDepth) const {
        raster::Bitmap bm{};
        bm.bytes_per_row = kBytesPerRow;
        bm.rows = kRows;
        bm.depth = depth;
        bm.addr = base;
        for (u32 plane = 0; plane < 8; ++plane) {
            bm.planes[plane] = base + plane * kPlaneBytes;
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

    const std::vector<u8>& memory() const { return memory_; }

    // Fills both plane areas with bytes from a small generator.
    void scramble(u32 seed) {
        for (u32 at = kPlaneBase; at < kPlaneBase + 2 * kPlanesBytes; ++at) {
            seed = seed * 1103515245u + 12345u;
            memory_[at] = static_cast<u8>(seed >> 16);
        }
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
    r.put16(kStruct + ndk::bm_BytesPerRow, 512);
    r.put16(kStruct + ndk::bm_Rows, 4096);
    r.put8(kStruct + ndk::bm_Depth, 2);
    r.put32(kStruct + ndk::bm_Planes, kPlaneBase);
    r.put32(kStruct + ndk::bm_Planes + 4, kPlaneBase + 0x100);
    raster::Bitmap bm{};
    CHECK(!raster::load_bitmap(b, kStruct, bm));
    CHECK(!b.ok());
    CHECK(r.machine().cpu.exit == ExitReason::BusError);
}

void load_bitmap_rejects_areas_wider_or_taller_than_4096() {
    Rig r;
    native::Bus b = r.bus();
    r.put16(kStruct + ndk::bm_BytesPerRow, 513);
    r.put16(kStruct + ndk::bm_Rows, 1);
    r.put8(kStruct + ndk::bm_Depth, 0);
    raster::Bitmap bm{};
    CHECK(!raster::load_bitmap(b, kStruct, bm));
    r.put16(kStruct + ndk::bm_BytesPerRow, 1);
    r.put16(kStruct + ndk::bm_Rows, 4097);
    CHECK(!raster::load_bitmap(b, kStruct, bm));
    r.put16(kStruct + ndk::bm_BytesPerRow, 512);
    r.put16(kStruct + ndk::bm_Rows, 1);
    CHECK(raster::load_bitmap(b, kStruct, bm));
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
    r.put16(kStruct + ndk::bm_BytesPerRow, 512);
    r.put16(kStruct + ndk::bm_Rows, 4096);
    r.put8(kStruct + ndk::bm_Depth, 0);
    raster::Bitmap bm{};
    CHECK(!raster::load_bitmap(b, kStruct, bm));
    CHECK(!b.ok());
}

void copy_rect_aligned() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap src = r.bitmap_at(kSecondPlaneBase);
    const raster::Bitmap dst = r.bitmap();
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 48; ++x) {
            raster::write_pixel(b, src, x, y, static_cast<u32>((x * 3 + y * 5 + 1) & 3));
            raster::write_pixel(b, dst, x, y, static_cast<u32>((x + y) & 3));
        }
    }
    const std::vector<u8> before = r.snapshot();
    raster::copy_rect(b, src, 8, 2, dst, 16, 10, 16, 4);
    CHECK(b.ok());

    for (i32 j = 0; j < 4; ++j) {
        for (i32 i = 0; i < 16; ++i) {
            CHECK_EQ(raster::read_pixel(b, dst, 16 + i, 10 + j),
                     raster::read_pixel(b, src, 8 + i, 2 + j));
        }
    }
    for (i32 y = 9; y <= 14; ++y) {
        for (i32 x = 15; x <= 32; ++x) {
            if (x >= 16 && x < 32 && y >= 10 && y < 14) {
                continue;
            }
            CHECK_EQ(raster::read_pixel(b, dst, x, y), static_cast<u32>((x + y) & 3));
        }
    }
    const std::vector<u8> after = r.snapshot();
    for (u32 plane = 0; plane < 8; ++plane) {
        for (u32 row = 0; row < kRows; ++row) {
            for (u32 byte = 0; byte < kBytesPerRow; ++byte) {
                if (plane < kDepth && row >= 10 && row < 14 && (byte == 2 || byte == 3)) {
                    continue;
                }
                const u32 at = plane * kPlaneBytes + row * kBytesPerRow + byte;
                CHECK_EQ(after[at], before[at]);
            }
        }
    }
}

void copy_rect_unaligned_shift() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap src = r.bitmap_at(kSecondPlaneBase);
    const raster::Bitmap dst = r.bitmap();
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 48; ++x) {
            const u32 s_pen = static_cast<u32>((x * 7 + y * 3 + (x >> 1)) & 3);
            const u32 d_pen = static_cast<u32>(((x ^ y) + 2) & 3);
            raster::write_pixel(b, src, x, y, s_pen);
            raster::write_pixel(b, dst, x, y, d_pen);
        }
    }
    raster::copy_rect(b, src, 3, 1, dst, 21, 5, 13, 3);
    CHECK(b.ok());

    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 48; ++x) {
            if (x >= 21 && x < 34 && y >= 5 && y < 8) {
                CHECK_EQ(raster::read_pixel(b, dst, x, y),
                         raster::read_pixel(b, src, 3 + (x - 21), 1 + (y - 5)));
            } else {
                CHECK_EQ(raster::read_pixel(b, dst, x, y),
                         static_cast<u32>(((x ^ y) + 2) & 3));
            }
        }
    }

    // Opposite bit-shift direction (source bit offset > destination bit offset).
    raster::copy_rect(b, src, 6, 9, dst, 18, 11, 13, 3);
    CHECK(b.ok());
    for (i32 y = 10; y <= 14; ++y) {
        for (i32 x = 16; x < 34; ++x) {
            if (x >= 18 && x < 31 && y >= 11 && y < 14) {
                CHECK_EQ(raster::read_pixel(b, dst, x, y),
                         raster::read_pixel(b, src, 6 + (x - 18), 9 + (y - 11)));
            } else {
                CHECK_EQ(raster::read_pixel(b, dst, x, y),
                         static_cast<u32>(((x ^ y) + 2) & 3));
            }
        }
    }
}

void copy_rect_clips_to_both_bitmaps() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap src = r.bitmap_at(kSecondPlaneBase);
    const raster::Bitmap dst = r.bitmap();
    r.clear_planes();
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 630; x < static_cast<i32>(kWidth); ++x) {
            raster::write_pixel(b, src, x, y, static_cast<u32>((x + y * 3 + 1) & 3));
        }
        for (i32 x = 0; x < 16; ++x) {
            raster::write_pixel(b, dst, x, y, static_cast<u32>((x + y + 1) & 3));
        }
    }
    const std::vector<u8> before = r.snapshot();

    // Source [634, 646) hangs 6 pixels off the right edge (640); destination
    // starts at (-3, -2), so only offsets i in [3, 6) and j in [2, 6) overlap.
    raster::copy_rect(b, src, 634, 2, dst, -3, -2, 12, 6);
    CHECK(b.ok());

    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 16; ++x) {
            if (x < 3 && y < 4) {
                CHECK_EQ(raster::read_pixel(b, dst, x, y),
                         raster::read_pixel(b, src, 637 + x, 4 + y));
            } else {
                CHECK_EQ(raster::read_pixel(b, dst, x, y),
                         static_cast<u32>((x + y + 1) & 3));
            }
        }
    }
    const std::vector<u8> after = r.snapshot();
    for (u32 plane = 0; plane < 8; ++plane) {
        for (u32 row = 0; row < kRows; ++row) {
            for (u32 byte = 0; byte < kBytesPerRow; ++byte) {
                if (plane < kDepth && row < 4 && byte == 0) {
                    continue;
                }
                const u32 at = plane * kPlaneBytes + row * kBytesPerRow + byte;
                CHECK_EQ(after[at], before[at]);
            }
        }
    }

    // Degenerate and completely off-bitmap copies leave the destination untouched.
    const std::vector<u8> mid = r.snapshot();
    raster::copy_rect(b, src, 0, 0, dst, 0, 0, 0, 4);
    raster::copy_rect(b, src, 0, 0, dst, 0, 0, 4, -1);
    raster::copy_rect(b, src, 640, 0, dst, 0, 0, 4, 4);
    raster::copy_rect(b, src, 0, 0, dst, -8, -8, 4, 4);
    CHECK(b.ok());
    CHECK(r.snapshot() == mid);
}

void copy_rect_depth_mismatch() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap src = r.bitmap_at(kSecondPlaneBase, 1);
    const raster::Bitmap dst = r.bitmap_at(kPlaneBase, 2);
    r.clear_planes();
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 32; ++x) {
            raster::write_pixel(b, src, x, y, static_cast<u32>((x + y) & 1));
            raster::write_pixel(b, dst, x, y, static_cast<u32>(((x * 3 + y) & 3)));
        }
    }
    const std::vector<u8> before = r.snapshot();

    raster::copy_rect(b, src, 5, 1, dst, 11, 4, 14, 3);
    CHECK(b.ok());

    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 32; ++x) {
            const u32 old_pen = static_cast<u32>((x * 3 + y) & 3);
            if (x >= 11 && x < 25 && y >= 4 && y < 7) {
                const u32 src_bit0 = raster::read_pixel(b, src, 5 + (x - 11), 1 + (y - 4)) & 1u;
                const u32 expected = (old_pen & 2u) | src_bit0;
                CHECK_EQ(raster::read_pixel(b, dst, x, y), expected);
            } else {
                CHECK_EQ(raster::read_pixel(b, dst, x, y), old_pen);
            }
        }
    }
    for (u32 row = 0; row < kRows; ++row) {
        for (u32 byte = 0; byte < kBytesPerRow; ++byte) {
            const u32 at = 1 * kPlaneBytes + row * kBytesPerRow + byte;
            CHECK_EQ(r.plane_byte(1, row, byte), before[at]);
        }
    }
}

void copy_rect_skip_trims_top_corners_and_clips() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap src = r.bitmap_at(kSecondPlaneBase);
    const raster::Bitmap dst = r.bitmap();
    r.clear_planes();
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 40; ++x) {
            raster::write_pixel(b, src, x, y, static_cast<u32>(1 + ((x + y * 2) % 3)));
            raster::write_pixel(b, dst, x, y, 0);
        }
    }
    static constexpr u8 kInsets[3] = {3, 2, 1};
    raster::copy_rect_skip(b, src, 0, 0, dst, 10, 2, 16, 8, 16, kInsets, 3);
    CHECK(b.ok());

    auto is_skipped = [](i32 lx, i32 ly) {
        if (ly >= 0 && ly < 3) {
            const i32 inset = 3 - ly;
            return lx < inset || lx >= 16 - inset;
        }
        return false;
    };
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 40; ++x) {
            if (x >= 10 && x < 26 && y >= 2 && y < 10 && !is_skipped(x - 10, y - 2)) {
                CHECK_EQ(raster::read_pixel(b, dst, x, y),
                         raster::read_pixel(b, src, x - 10, y - 2));
            } else {
                CHECK_EQ(raster::read_pixel(b, dst, x, y), 0u);
            }
        }
    }

    // Partial sub-rect starting at (sx=1, sy=1) with negative destination offset.
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 40; ++x) {
            raster::write_pixel(b, dst, x, y, 3);
        }
    }
    raster::copy_rect_skip(b, src, 1, 1, dst, 4, -1, 14, 5, 16, kInsets, 3);
    CHECK(b.ok());
    for (i32 y = 0; y < 6; ++y) {
        for (i32 x = 0; x < 24; ++x) {
            const i32 sx = 1 + (x - 4);
            const i32 sy = 1 + (y + 1);
            if (x >= 4 && x < 18 && y >= 0 && y < 4 && !is_skipped(sx, sy)) {
                CHECK_EQ(raster::read_pixel(b, dst, x, y), raster::read_pixel(b, src, sx, sy));
            } else {
                CHECK_EQ(raster::read_pixel(b, dst, x, y), 3u);
            }
        }
    }
}

void remap_rect_remaps_masked_spans_and_clips() {
    Rig r;
    native::Bus b = r.bus();
    const raster::Bitmap bm = r.bitmap_at(kPlaneBase, 8);
    r.clear_planes();
    u8 table[256]{};
    for (u32 i = 0; i < 256; ++i) {
        table[i] = static_cast<u8>((i * 37 + 11) & 0xFF);
    }
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 40; ++x) {
            raster::write_pixel(b, bm, x, y, static_cast<u32>((x * 13 + y * 7) & 0xFF));
        }
        for (i32 x = 632; x < static_cast<i32>(kWidth); ++x) {
            raster::write_pixel(b, bm, x, y, static_cast<u32>((x + y * 5) & 0xFF));
        }
    }

    raster::remap_rect(b, bm, Rect{5, 2, 27, 6}, table, 256);
    CHECK(b.ok());
    for (i32 y = 0; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 0; x < 40; ++x) {
            const u32 orig = static_cast<u32>((x * 13 + y * 7) & 0xFF);
            const u32 want = (x >= 5 && x <= 27 && y >= 2 && y <= 6) ? table[orig] : orig;
            CHECK_EQ(raster::read_pixel(b, bm, x, y), want);
        }
    }

    // Clipping at top-left and bottom-right edges, plus empty rect.
    const std::vector<u8> mid = r.snapshot();
    raster::remap_rect(b, bm, Rect{20, 10, 10, 12}, table, 256);
    CHECK(r.snapshot() == mid);

    raster::remap_rect(b, bm, Rect{636, 13, 650, 20}, table, 256);
    CHECK(b.ok());
    for (i32 y = 12; y < static_cast<i32>(kRows); ++y) {
        for (i32 x = 632; x < static_cast<i32>(kWidth); ++x) {
            const u32 orig = static_cast<u32>((x + y * 5) & 0xFF);
            const u32 want = (x >= 636 && y >= 13) ? table[orig] : orig;
            CHECK_EQ(raster::read_pixel(b, bm, x, y), want);
        }
    }

    // Invalid plane pointer triggers BusError and stops cleanly.
    raster::Bitmap bad = bm;
    bad.planes[0] = kMemoryBytes + 16;
    raster::remap_rect(b, bad, Rect{0, 0, 7, 1}, table, 256);
    CHECK(!b.ok());
    CHECK_EQ(r.machine().cpu.exit, ExitReason::BusError);
}

}

// Pixel-at-a-time versions of the calls that work a plane byte at a time;
// the faster calls must leave exactly the same memory.
class Random {
public:
    explicit Random(u32 seed) : state_(seed) {}

    i32 between(i32 low, i32 high) {
        state_ = state_ * 1664525u + 1013904223u;
        return low + static_cast<i32>((state_ >> 8) % static_cast<u32>(high - low + 1));
    }

private:
    u32 state_;
};

bool reference_visible(const raster::Clip& clip, i64 x, i64 y) {
    return x >= clip.x0 && x <= clip.x1 && y >= clip.y0 && y <= clip.y1 && x >= 0 && y >= 0 &&
           x < static_cast<i64>(kWidth) && y < static_cast<i64>(kRows);
}

raster::Clip random_clip(Random& rnd) {
    return raster::Clip{rnd.between(-20, 300), rnd.between(-4, 8), rnd.between(200, 700),
                        rnd.between(4, 20)};
}

Rect random_rect(Random& rnd) {
    return Rect{rnd.between(-20, 640), rnd.between(-4, 16), rnd.between(-20, 680),
                rnd.between(-4, 20)};
}

void fill_rect_matches_pixel_reference() {
    Random rnd(7);
    for (u32 round = 0; round < 300; ++round) {
        Rig fast;
        Rig slow;
        fast.scramble(round);
        slow.scramble(round);
        const u32 depth = static_cast<u32>(rnd.between(1, 8));
        const raster::Clip clip = random_clip(rnd);
        const Rect rect = random_rect(rnd);
        const u32 pen = static_cast<u32>(rnd.between(0, 255));
        native::Bus fb = fast.bus();
        raster::fill_rect(fb, fast.bitmap_at(kPlaneBase, depth), clip, rect, pen);
        native::Bus sb = slow.bus();
        const raster::Bitmap bm = slow.bitmap_at(kPlaneBase, depth);
        for (i64 y = rect.y0; y <= rect.y1; ++y) {
            for (i64 x = rect.x0; x <= rect.x1; ++x) {
                if (reference_visible(clip, x, y)) {
                    raster::write_pixel(sb, bm, static_cast<i32>(x), static_cast<i32>(y), pen);
                }
            }
        }
        CHECK(fb.ok());
        CHECK(fast.memory() == slow.memory());
    }
}

void vertical_scroll_matches_pixel_reference() {
    Random rnd(11);
    for (u32 round = 0; round < 300; ++round) {
        Rig fast;
        Rig slow;
        fast.scramble(round);
        slow.scramble(round);
        const u32 depth = static_cast<u32>(rnd.between(1, 8));
        const raster::Clip clip = random_clip(rnd);
        const Rect area = random_rect(rnd);
        const i32 dy = rnd.between(-18, 18);
        const u32 bg = static_cast<u32>(rnd.between(0, 255));
        native::Bus fb = fast.bus();
        const Rect fast_dirty =
            raster::scroll_rect(fb, fast.bitmap_at(kPlaneBase, depth), clip, 0, dy, area, bg);
        native::Bus sb = slow.bus();
        const raster::Bitmap bm = slow.bitmap_at(kPlaneBase, depth);
        const i64 left = std::max<i64>({area.x0, clip.x0, 0});
        const i64 top = std::max<i64>({area.y0, clip.y0, 0});
        const i64 right = std::min<i64>({area.x1, clip.x1, kWidth - 1});
        const i64 bottom = std::min<i64>({area.y1, clip.y1, kRows - 1});
        for (i64 row = 0; row <= bottom - top; ++row) {
            const i64 y = dy > 0 ? top + row : bottom - row;
            for (i64 x = left; x <= right; ++x) {
                const i64 source_y = y + dy;
                const u32 pen = source_y >= top && source_y <= bottom
                                    ? raster::read_pixel(sb, bm, static_cast<i32>(x),
                                                         static_cast<i32>(source_y))
                                    : bg;
                raster::write_pixel(sb, bm, static_cast<i32>(x), static_cast<i32>(y), pen);
            }
        }
        CHECK(fb.ok());
        CHECK(fast.memory() == slow.memory());
        if (left <= right && top <= bottom) {
            CHECK_EQ(fast_dirty.x0, static_cast<i32>(left));
            CHECK_EQ(fast_dirty.y0, static_cast<i32>(top));
            CHECK_EQ(fast_dirty.x1, static_cast<i32>(right));
            CHECK_EQ(fast_dirty.y1, static_cast<i32>(bottom));
        } else {
            CHECK(fast_dirty.x1 < fast_dirty.x0);
        }
    }
}

void copy_rect_matches_pixel_reference() {
    Random rnd(13);
    for (u32 round = 0; round < 400; ++round) {
        Rig fast;
        Rig slow;
        fast.scramble(round);
        slow.scramble(round);
        const u32 depth = static_cast<u32>(rnd.between(1, 8));
        const i32 sx = rnd.between(-20, 650);
        const i32 sy = rnd.between(-4, 18);
        const i32 dx = rnd.between(-20, 650);
        const i32 dy = rnd.between(-4, 18);
        const i32 w = rnd.between(1, 660);
        const i32 h = rnd.between(1, 20);
        native::Bus fb = fast.bus();
        raster::copy_rect(fb, fast.bitmap_at(kPlaneBase, depth), sx, sy,
                          fast.bitmap_at(kSecondPlaneBase, depth), dx, dy, w, h);
        native::Bus sb = slow.bus();
        const raster::Bitmap src = slow.bitmap_at(kPlaneBase, depth);
        const raster::Bitmap dst = slow.bitmap_at(kSecondPlaneBase, depth);
        i64 s_x = sx;
        i64 s_y = sy;
        i64 d_x = dx;
        i64 d_y = dy;
        i64 cw = w;
        i64 ch = h;
        if (raster::clip_copy_bounds(src, dst, s_x, s_y, d_x, d_y, cw, ch)) {
            for (i64 j = 0; j < ch; ++j) {
                for (i64 i = 0; i < cw; ++i) {
                    const u32 pen = raster::read_pixel(sb, src, static_cast<i32>(s_x + i),
                                                       static_cast<i32>(s_y + j));
                    raster::write_pixel(sb, dst, static_cast<i32>(d_x + i),
                                        static_cast<i32>(d_y + j), pen);
                }
            }
        }
        CHECK(fb.ok());
        CHECK(fast.memory() == slow.memory());
    }
}

void jam2_text_matches_pixel_reference() {
    Random rnd(17);
    const u8 text[] = "Hombre 3.1 > dir C: {~}";
    const u32 length = sizeof(text) - 1;
    for (u32 round = 0; round < 300; ++round) {
        Rig fast;
        Rig slow;
        fast.scramble(round);
        slow.scramble(round);
        const u32 depth = static_cast<u32>(rnd.between(1, 8));
        const raster::Clip clip = random_clip(rnd);
        const i32 x = (round & 1) != 0 ? rnd.between(-3, 80) * 8 : rnd.between(-24, 650);
        const i32 baseline = rnd.between(-4, 22);
        const u8 inverse = (round & 2) != 0 ? static_cast<u8>(ndk::INVERSVID) : 0;
        const raster::Pens pens{static_cast<u8>(rnd.between(0, 255)),
                                static_cast<u8>(rnd.between(0, 255)),
                                static_cast<u8>(ndk::JAM2 | inverse)};
        native::Bus fb = fast.bus();
        u32 advance = 0;
        const Rect fast_dirty = raster::draw_text(fb, fast.bitmap_at(kPlaneBase, depth), clip,
                                                  pens, x, baseline, text, length, advance);
        native::Bus sb = slow.bus();
        const raster::Bitmap bm = slow.bitmap_at(kPlaneBase, depth);
        const u32 fg = inverse != 0 ? pens.bg : pens.fg;
        const u32 bg = inverse != 0 ? pens.fg : pens.bg;
        Rect dirty{0, 0, -1, -1};
        for (u32 i = 0; i < length; ++i) {
            const u8* rows = glyph(static_cast<char>(text[i]));
            for (u32 row = 0; row < 8; ++row) {
                for (u32 col = 0; col < 8; ++col) {
                    const i64 px = static_cast<i64>(x) + 8 * i + col;
                    const i64 py = static_cast<i64>(baseline) - 6 + row;
                    if (!reference_visible(clip, px, py)) {
                        continue;
                    }
                    const bool set = ((rows[row] >> col) & 1) != 0;
                    raster::write_pixel(sb, bm, static_cast<i32>(px), static_cast<i32>(py),
                                        set ? fg : bg);
                    if (dirty.x1 < dirty.x0) {
                        dirty = Rect{static_cast<i32>(px), static_cast<i32>(py),
                                     static_cast<i32>(px), static_cast<i32>(py)};
                    } else {
                        dirty.x0 = std::min(dirty.x0, static_cast<i32>(px));
                        dirty.y0 = std::min(dirty.y0, static_cast<i32>(py));
                        dirty.x1 = std::max(dirty.x1, static_cast<i32>(px));
                        dirty.y1 = std::max(dirty.y1, static_cast<i32>(py));
                    }
                }
            }
        }
        CHECK(fb.ok());
        CHECK(fast.memory() == slow.memory());
        CHECK_EQ(fast_dirty.x0, dirty.x0);
        CHECK_EQ(fast_dirty.y0, dirty.y0);
        CHECK_EQ(fast_dirty.x1, dirty.x1);
        CHECK_EQ(fast_dirty.y1, dirty.y1);
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
    load_bitmap_rejects_areas_wider_or_taller_than_4096();
    load_bitmap_rejects_a_depth_zero_bitmap_larger_than_guest_memory();
    copy_rect_aligned();
    copy_rect_unaligned_shift();
    copy_rect_clips_to_both_bitmaps();
    copy_rect_depth_mismatch();
    copy_rect_skip_trims_top_corners_and_clips();
    remap_rect_remaps_masked_spans_and_clips();
    fill_rect_matches_pixel_reference();
    vertical_scroll_matches_pixel_reference();
    copy_rect_matches_pixel_reference();
    jam2_text_matches_pixel_reference();
    return check::finish("test_raster");
}
