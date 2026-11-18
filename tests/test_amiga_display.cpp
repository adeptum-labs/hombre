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

#include "amiga_display.hpp"
#include "check.hpp"
#include "framebuffer.hpp"

#include <cstring>
#include <vector>

using namespace hombre;
using hombre::m68k::CpuState;
using hombre::m68k::GuestMemory;
using hombre::m68k::Machine;

namespace {

constexpr u8 kGuardByte = 0xAA;
constexpr usize kGuard = 64;
constexpr u32 kBgrx = kFramebufferBgrx8;
constexpr u32 kBorder = 0xAAAAAA;

constexpr u32 kScreen = 0x0800;
constexpr u32 kColourMap = 0x1000;
constexpr u32 kColourTable = 0x1100;
constexpr u32 kPlaneBase = 0x2000;
constexpr u32 kScreenWidth = 640;
constexpr u32 kScreenHeight = 400;
constexpr u32 kBytesPerRow = kScreenWidth / 8;
constexpr u32 kPlanes = 2;
constexpr u32 kPlaneBytes = kBytesPerRow * kScreenHeight;
constexpr u32 kPlanesBytes = kPlanes * kPlaneBytes;
constexpr u32 kMemoryBytes = 1 << 18;

// Pixel memory on the heap with guard bytes before and after it, so that a
// write outside the buffer shows up in the guards (and in ASan).
class Screen {
public:
    Screen(u32 width, u32 height, u32 pitch_bytes, u32 format)
        : width_(width),
          height_(height),
          pitch_(pitch_bytes),
          size_(static_cast<usize>(pitch_bytes) * height),
          format_(format),
          memory_(kGuard + size_ + kGuard, kGuardByte) {
        // The pixel area itself starts out as a recognisable pattern.
        std::memset(base(), 0x11, size_);
    }

    u8* base() { return memory_.data() + kGuard; }
    Framebuffer framebuffer() { return Framebuffer(base(), width_, height_, pitch_, format_); }

    // The four bytes of one pixel, little endian: byte 0 is the lowest.
    u32 pixel(u32 x, u32 y) {
        u32 value;
        std::memcpy(&value, base() + static_cast<usize>(y) * pitch_ + static_cast<usize>(x) * 4, 4);
        return value;
    }

    bool guards_intact() const {
        for (usize i = 0; i < kGuard; ++i) {
            if (memory_[i] != kGuardByte || memory_[kGuard + size_ + i] != kGuardByte) {
                return false;
            }
        }
        return true;
    }

    std::vector<u8> snapshot() const {
        return std::vector<u8>(memory_.begin() + kGuard, memory_.begin() + kGuard + size_);
    }

private:
    u32 width_, height_, pitch_;
    usize size_;
    u32 format_;
    std::vector<u8> memory_;
};

// Guest memory holding a Screen, its BitMap, a ColorMap and two 640x400
// bitmap planes, and a Machine without a Jit.
class Guest {
public:
    Guest() : memory_(kMemoryBytes, kGuardByte) {
        m_.cpu = CpuState{};
        m_.mem = GuestMemory{memory_.data(), kMemoryBytes};
        m_.jit = nullptr;
        clear_planes();
        write_screen();
    }

    Machine& machine() { return m_; }

    void clear_planes() { std::memset(memory_.data() + kPlaneBase, 0, kPlanesBytes); }

    // Gives every pixel of the screen the pen.
    void set_all_pixels(u32 pen) {
        for (u32 plane = 0; plane < kPlanes; ++plane) {
            std::memset(memory_.data() + kPlaneBase + plane * kPlaneBytes,
                        ((pen >> plane) & 1) != 0 ? 0xFF : 0x00, kPlaneBytes);
        }
    }

    void set_pixel(u32 x, u32 y, u32 pen) {
        const u32 mask = 0x80 >> (x & 7);
        const u32 at = y * kBytesPerRow + (x >> 3);
        for (u32 plane = 0; plane < kPlanes; ++plane) {
            u8& byte = memory_[kPlaneBase + plane * kPlaneBytes + at];
            if (((pen >> plane) & 1) != 0) {
                byte = static_cast<u8>(byte | mask);
            } else {
                byte = static_cast<u8>(byte & ~mask);
            }
        }
    }

private:
    void put8(u32 at, u32 v) { memory_[at] = static_cast<u8>(v); }

    void put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
    }

    void put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
    }

    // Pen 0 is 0xAAAAAA, pen 1 black, pen 2 white and pen 3 0x6688BB.
    void write_screen() {
        put16(kScreen + ndk::sc_Width, kScreenWidth);
        put16(kScreen + ndk::sc_Height, kScreenHeight);
        put16(kScreen + ndk::sc_BitMap + ndk::bm_BytesPerRow, kBytesPerRow);
        put8(kScreen + ndk::sc_BitMap + ndk::bm_Depth, kPlanes);
        for (u32 plane = 0; plane < 8; ++plane) {
            put32(kScreen + ndk::sc_BitMap + ndk::bm_Planes + plane * 4,
                  kPlaneBase + plane * kPlaneBytes);
        }
        put32(kScreen + ndk::sc_ViewPort + ndk::vp_ColorMap, kColourMap);
        put32(kColourMap + ndk::cm_ColorTable, kColourTable);
        put16(kColourTable, 0x0AAA);
        put16(kColourTable + 2, 0x0000);
        put16(kColourTable + 4, 0x0FFF);
        put16(kColourTable + 6, 0x068B);
    }

    std::vector<u8> memory_;
    Machine m_;
};

void refresh(AmigaDisplay& display, Machine& m, u32 screen, Rect dirty) {
    const DisplaySink sink = display.sink();
    sink.refresh(sink.ctx, m, screen, dirty);
}

void scale_two_at_1280x800_and_no_border() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.scale(), 2u);
    CHECK_EQ(display.origin_x(), 0u);
    CHECK_EQ(display.origin_y(), 0u);

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    // The screen fills the framebuffer, so every corner is screen.
    CHECK_EQ(fb.pixel(0, 0), 0u);
    CHECK_EQ(fb.pixel(1279, 0), 0u);
    CHECK_EQ(fb.pixel(0, 799), 0u);
    CHECK_EQ(fb.pixel(1279, 799), 0u);
    CHECK(fb.guards_intact());
}

void larger_framebuffer_is_centred_with_border_colour() {
    Screen fb(1600, 1000, 1600 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.scale(), 2u);
    CHECK_EQ(display.origin_x(), 160u);
    CHECK_EQ(display.origin_y(), 100u);

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    // The screen covers x 160 to 1439 and y 100 to 899.
    CHECK_EQ(fb.pixel(160, 100), 0u);
    CHECK_EQ(fb.pixel(1439, 899), 0u);
    CHECK_EQ(fb.pixel(0, 0), kBorder);
    CHECK_EQ(fb.pixel(159, 100), kBorder);
    CHECK_EQ(fb.pixel(160, 99), kBorder);
    CHECK_EQ(fb.pixel(1440, 100), kBorder);
    CHECK_EQ(fb.pixel(160, 900), kBorder);
    CHECK(fb.guards_intact());
}

void below_1x_clips_top_left() {
    Screen fb(320, 200, 320 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.scale(), 1u);
    CHECK_EQ(display.origin_x(), 0u);
    CHECK_EQ(display.origin_y(), 0u);

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    CHECK_EQ(fb.pixel(0, 0), 0u);
    CHECK_EQ(fb.pixel(319, 199), 0u);
    CHECK(fb.guards_intact());
}

void refresh_draws_pens_through_the_palette() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());

    guest.set_pixel(0, 0, 0);
    guest.set_pixel(1, 0, 1);
    guest.set_pixel(2, 0, 2);
    guest.set_pixel(3, 0, 3);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 3, 0});

    CHECK_EQ(fb.pixel(0, 0), 0xAAAAAAu);
    CHECK_EQ(fb.pixel(1, 1), 0xAAAAAAu);
    CHECK_EQ(fb.pixel(2, 0), 0u);
    CHECK_EQ(fb.pixel(3, 1), 0u);
    CHECK_EQ(fb.pixel(4, 0), 0xFFFFFFu);
    CHECK_EQ(fb.pixel(5, 1), 0xFFFFFFu);
    CHECK_EQ(fb.pixel(6, 0), 0x6688BBu);
    CHECK_EQ(fb.pixel(7, 1), 0x6688BBu);
    CHECK(fb.guards_intact());
}

void dirty_rect_only_touches_its_blocks() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());

    guest.set_all_pixels(1);

    // Screen x 1 to 2 and y 1 to 2 are framebuffer x 2 to 5 and y 2 to 5.
    refresh(display, guest.machine(), kScreen, Rect{1, 1, 2, 2});

    CHECK_EQ(fb.pixel(2, 2), 0u);
    CHECK_EQ(fb.pixel(5, 5), 0u);
    CHECK_EQ(fb.pixel(1, 1), kBorder);
    CHECK_EQ(fb.pixel(6, 2), kBorder);
    CHECK_EQ(fb.pixel(2, 6), kBorder);
    CHECK(fb.guards_intact());
}

void dirty_rect_is_clipped_to_the_screen() {
    Screen fb(1600, 1000, 1600 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{600, 380, 10000, 10000});

    // Only screen x 600 to 639 and y 380 to 399 are drawn: framebuffer
    // x 1360 to 1439 and y 860 to 899.
    CHECK_EQ(fb.pixel(1360, 860), 0u);
    CHECK_EQ(fb.pixel(1439, 899), 0u);
    CHECK_EQ(fb.pixel(1359, 860), kBorder);
    CHECK_EQ(fb.pixel(1440, 860), kBorder);
    CHECK_EQ(fb.pixel(1500, 950), kBorder);
    CHECK(fb.guards_intact());
}

void screen_zero_clears_to_border_colour() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Screen border(1280, 800, 1280 * 4, kBgrx);
    border.framebuffer().fill(kBorder);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});
    CHECK_EQ(fb.pixel(0, 0), 0u);

    refresh(display, guest.machine(), 0, Rect{0, 0, 639, 399});
    CHECK(fb.snapshot() == border.snapshot());
    CHECK(fb.guards_intact());
}

void guards_intact() {
    Screen fb(320, 200, 320 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());

    guest.set_all_pixels(3);
    refresh(display, guest.machine(), kScreen, Rect{-1000, -1000, 100000, 100000});
    CHECK_EQ(fb.pixel(0, 0), 0x6688BBu);
    CHECK_EQ(fb.pixel(319, 199), 0x6688BBu);
    CHECK(fb.guards_intact());

    refresh(display, guest.machine(), 0, Rect{0, 0, 0, 0});
    CHECK(fb.guards_intact());
}

}

int main() {
    scale_two_at_1280x800_and_no_border();
    larger_framebuffer_is_centred_with_border_colour();
    below_1x_clips_top_left();
    refresh_draws_pens_through_the_palette();
    dirty_rect_only_touches_its_blocks();
    dirty_rect_is_clipped_to_the_screen();
    screen_zero_clears_to_border_colour();
    guards_intact();
    return check::finish("test_amiga_display");
}
