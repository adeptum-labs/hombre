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
#include "pointer_image.hpp"

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
constexpr u32 kLowColourTable = 0x1400;
constexpr u32 kPlaneBase = 0x2000;
constexpr u32 kScreenWidth = 640;
constexpr u32 kScreenHeight = 400;
constexpr u32 kBytesPerRow = kScreenWidth / 8;
constexpr u32 kPlanes = 2;
constexpr u32 kPlaneBytes = kBytesPerRow * kScreenHeight;
constexpr u32 kPlanesBytes = kPlanes * kPlaneBytes;
constexpr u32 kMemoryBytes = 1 << 19;

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

// Guest memory holding a Screen, its BitMap, a ColorMap and the bitmap planes
// of a screen of up to 8 planes (640x400 unless the size is given), and a
// Machine without a Jit.
class Guest {
public:
    explicit Guest(u32 depth = kPlanes, u32 width = kScreenWidth, u32 height = kScreenHeight)
        : depth_(depth),
          width_(width),
          height_(height),
          bytes_per_row_(width / 8),
          plane_bytes_(bytes_per_row_ * height),
          memory_(kMemoryBytes, kGuardByte) {
        m_.cpu = CpuState{};
        m_.mem = GuestMemory{memory_.data(), kMemoryBytes};
        m_.jit = nullptr;
        clear_planes();
        write_screen();
    }

    Machine& machine() { return m_; }

    void clear_planes() { std::memset(memory_.data() + kPlaneBase, 0, depth_ * plane_bytes_); }

    // Gives every pixel of the screen the pen.
    void set_all_pixels(u32 pen) {
        for (u32 plane = 0; plane < depth_; ++plane) {
            std::memset(memory_.data() + kPlaneBase + plane * plane_bytes_,
                        ((pen >> plane) & 1) != 0 ? 0xFF : 0x00, plane_bytes_);
        }
    }

    void set_pixel(u32 x, u32 y, u32 pen) {
        const u32 mask = 0x80 >> (x & 7);
        const u32 at = y * bytes_per_row_ + (x >> 3);
        for (u32 plane = 0; plane < depth_; ++plane) {
            u8& byte = memory_[kPlaneBase + plane * plane_bytes_ + at];
            if (((pen >> plane) & 1) != 0) {
                byte = static_cast<u8>(byte | mask);
            } else {
                byte = static_cast<u8>(byte & ~mask);
            }
        }
    }

    void set_rgb24(u32 pen, u32 rgb) {
        const u32 r = (rgb >> 16) & 0xFF;
        const u32 g = (rgb >> 8) & 0xFF;
        const u32 b = rgb & 0xFF;
        const u32 hi = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
        const u32 lo = ((r & 0xF) << 8) | ((g & 0xF) << 4) | (b & 0xF);
        put16(kColourTable + pen * 2, hi);
        put16(kLowColourTable + pen * 2, lo);
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
        put16(kScreen + ndk::sc_Width, width_);
        put16(kScreen + ndk::sc_Height, height_);
        put16(kScreen + ndk::sc_BitMap + ndk::bm_BytesPerRow, bytes_per_row_);
        put8(kScreen + ndk::sc_BitMap + ndk::bm_Depth, depth_);
        for (u32 plane = 0; plane < 8; ++plane) {
            put32(kScreen + ndk::sc_BitMap + ndk::bm_Planes + plane * 4,
                  plane < depth_ ? kPlaneBase + plane * plane_bytes_ : 0);
        }
        put32(kScreen + ndk::sc_ViewPort + ndk::vp_ColorMap, kColourMap);
        put8(kColourMap + ndk::cm_Flags, 0);
        put8(kColourMap + ndk::cm_Type,
             depth_ > 2 ? ndk::COLORMAP_TYPE_V39 : ndk::COLORMAP_TYPE_V1_2);
        put16(kColourMap + ndk::cm_Count, 1u << depth_);
        put32(kColourMap + ndk::cm_ColorTable, kColourTable);
        put32(kColourMap + ndk::cm_LowColorBits, depth_ > 2 ? kLowColourTable : 0);
        put16(kColourTable, 0x0AAA);
        put16(kColourTable + 2, 0x0000);
        put16(kColourTable + 4, 0x0FFF);
        put16(kColourTable + 6, 0x068B);
    }

    u32 depth_;
    u32 width_;
    u32 height_;
    u32 bytes_per_row_;
    u32 plane_bytes_;
    std::vector<u8> memory_;
    Machine m_;
};

// Guest memory holding the Screen of a deep ARGB32 screen of width by
// height, its pixels at kPlaneBase all black.
class DeepGuest {
public:
    DeepGuest(u32 width, u32 height) : width_(width), memory_(kMemoryBytes, kGuardByte) {
        m_.cpu = CpuState{};
        m_.mem = GuestMemory{memory_.data(), kMemoryBytes};
        m_.jit = nullptr;
        std::memset(memory_.data() + kPlaneBase, 0, width * 4 * height);
        put16(kScreen + ndk::sc_Width, width);
        put16(kScreen + ndk::sc_Height, height);
        put16(kScreen + ndk::sc_BitMap + ndk::bm_BytesPerRow, width * 4);
        put16(kScreen + ndk::sc_BitMap + ndk::bm_Rows, height);
        put8(kScreen + ndk::sc_BitMap + ndk::bm_Depth, 24);
        put32(kScreen + ndk::sc_BitMap + ndk::bm_Planes, kPlaneBase);
        put32(kScreen + ndk::sc_ViewPort + ndk::vp_ColorMap, kColourMap);
        put8(kColourMap + ndk::cm_Type, ndk::COLORMAP_TYPE_V39);
        put16(kColourMap + ndk::cm_Count, 256);
        put32(kColourMap + ndk::cm_ColorTable, kColourTable);
        put32(kColourMap + ndk::cm_LowColorBits, kLowColourTable);
    }

    Machine& machine() { return m_; }

    void set_pixel(u32 x, u32 y, u32 argb) { put32(kPlaneBase + (y * width_ + x) * 4, argb); }

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

    u32 width_;
    std::vector<u8> memory_;
    Machine m_;
};

void refresh(AmigaDisplay& display, Machine& m, u32 screen, Rect dirty) {
    const DisplaySink sink = display.sink();
    sink.refresh(sink.ctx, m, screen, dirty);
}

// A deep screen of a 4:3 HOMBRE mode on a 16:10 framebuffer keeps its
// shape: black bars at the sides, its pixels in their own colours, and the
// Amiga box with its border comes back for an Amiga screen.
void deep_screen_is_square_and_pillarboxed() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    DeepGuest guest(80, 60);
    guest.set_pixel(0, 0, 0xFFFF0000u);
    guest.set_pixel(79, 59, 0xFF00FF00u);
    AmigaDisplay display;
    display.init(fb.framebuffer());
    DisplayState state;
    state.mode_width = 80;
    state.mode_height = 60;
    state.mode_deep = true;
    state.mode_fmt = PixFmt::Argb32;
    display.follow_pointer_shape(state);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 79, 59});
    // 80x60 at 800 rows high is 1066 wide, from x 107.
    CHECK_EQ(display.box_x(), 107u);
    CHECK_EQ(display.box_width(), 1066u);
    CHECK_EQ(display.box_height(), 800u);
    CHECK_EQ(fb.pixel(107, 0), 0x00FF0000u);
    CHECK_EQ(fb.pixel(107 + 1065, 799), 0x0000FF00u);
    CHECK_EQ(fb.pixel(0, 400), 0u);
    CHECK_EQ(fb.pixel(1279, 400), 0u);
    CHECK_EQ(fb.pixel(500, 400), 0u);
    CHECK(fb.guards_intact());

    Guest amiga;
    state.mode_width = 640;
    state.mode_height = 400;
    state.mode_deep = false;
    amiga.set_all_pixels(1);
    refresh(display, amiga.machine(), kScreen, Rect{0, 0, 639, 399});
    CHECK_EQ(display.box_x(), 0u);
    CHECK_EQ(display.box_width(), 1280u);
    CHECK(fb.guards_intact());
}

void scale_two_at_1280x800_and_no_border() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.box_width(), 1280u);
    CHECK_EQ(display.box_height(), 800u);
    CHECK_EQ(display.box_x(), 0u);
    CHECK_EQ(display.box_y(), 0u);

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    // The screen fills the framebuffer, so every corner is screen.
    CHECK_EQ(fb.pixel(0, 0), 0u);
    CHECK_EQ(fb.pixel(1279, 0), 0u);
    CHECK_EQ(fb.pixel(0, 799), 0u);
    CHECK_EQ(fb.pixel(1279, 799), 0u);
    CHECK(fb.guards_intact());
}

void wide_framebuffer_gets_a_centred_box_with_border_colour_beside_it() {
    Screen fb(1920, 1000, 1920 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.box_width(), 1600u);
    CHECK_EQ(display.box_height(), 1000u);
    CHECK_EQ(display.box_x(), 160u);
    CHECK_EQ(display.box_y(), 0u);

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    // The box covers x 160 to 1759.
    CHECK_EQ(fb.pixel(160, 0), 0u);
    CHECK_EQ(fb.pixel(1759, 999), 0u);
    CHECK_EQ(fb.pixel(0, 0), kBorder);
    CHECK_EQ(fb.pixel(159, 0), kBorder);
    CHECK_EQ(fb.pixel(1760, 0), kBorder);
    CHECK_EQ(fb.pixel(1919, 999), kBorder);
    CHECK(fb.guards_intact());
}

void tall_framebuffer_gets_a_centred_box_with_border_colour_above_and_below_it() {
    Screen fb(1600, 1200, 1600 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.box_width(), 1600u);
    CHECK_EQ(display.box_height(), 1000u);
    CHECK_EQ(display.box_x(), 0u);
    CHECK_EQ(display.box_y(), 100u);

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    CHECK_EQ(fb.pixel(0, 100), 0u);
    CHECK_EQ(fb.pixel(1599, 1099), 0u);
    CHECK_EQ(fb.pixel(0, 99), kBorder);
    CHECK_EQ(fb.pixel(0, 1100), kBorder);
    CHECK(fb.guards_intact());
}

void framebuffer_smaller_than_the_screen_shows_all_of_it_downscaled() {
    Screen fb(320, 200, 320 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.box_width(), 320u);
    CHECK_EQ(display.box_height(), 200u);

    guest.set_all_pixels(1);
    guest.set_pixel(2, 0, 2);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    CHECK_EQ(fb.pixel(0, 0), 0u);
    CHECK_EQ(fb.pixel(319, 199), 0u);
    // Host column 1 shows screen column 2.
    CHECK_EQ(fb.pixel(1, 0), 0xFFFFFFu);
    CHECK_EQ(fb.pixel(2, 0), 0u);
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
    // x 1500 to 1599 and y 950 to 999, at 2.5 host pixels per screen pixel.
    CHECK_EQ(fb.pixel(1500, 950), 0u);
    CHECK_EQ(fb.pixel(1599, 999), 0u);
    CHECK_EQ(fb.pixel(1499, 950), kBorder);
    CHECK_EQ(fb.pixel(1500, 949), kBorder);
    CHECK_EQ(fb.pixel(1000, 500), kBorder);
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

void pointer_image_has_a_tip_at_the_origin() {
    CHECK(pointer_pixel(0, 0) != 0);
    CHECK_EQ(pointer_pixel(0, 0), 1u);
    CHECK_EQ(pointer_pixel(15, 15), 0u);
    CHECK_EQ(pointer_pixel(15, 0), 0u);
    CHECK_EQ(pointer_pixel(0, 15), 0u);
    CHECK_EQ(pointer_pixel(16, 0), 0u);
    CHECK_EQ(pointer_pixel(0, 16), 0u);

    CHECK_EQ(kDefaultPointerShape.hot_x, 0);
    CHECK_EQ(kDefaultPointerShape.hot_y, 0);
    CHECK_EQ(kDefaultPointerShape.pixels[2][1], 2u);
    CHECK_EQ(kDefaultPointerShape.pixels[0][1], 3u);
    CHECK_EQ(kDefaultPointerShape.pixels[10][10], 1u);
    CHECK_EQ(kDefaultPointerShape.rgb[1], 0xE04040u);
    CHECK_EQ(kDefaultPointerShape.rgb[2], 0x000000u);
    CHECK_EQ(kDefaultPointerShape.rgb[3], 0xE0E0C0u);
}

void pointer_is_drawn_over_the_screen_on_refresh() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    guest.set_all_pixels(3);

    display.move_pointer(guest.machine(), kScreen, 10, 10);
    display.set_pointer_visible(guest.machine(), kScreen, true);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    // Tip at (10, 10) scaled by 2 is at framebuffer (20, 20), painted red.
    CHECK_EQ(fb.pixel(20, 20), 0xE04040u);
    // Pointer (1, 0) is cream and (1, 2) black, scaled by 2 to (22, 20) and
    // (22, 24).
    CHECK_EQ(fb.pixel(22, 20), 0xE0E0C0u);
    CHECK_EQ(fb.pixel(22, 24), 0x000000u);
    // Pixel outside pointer is screen colour (pen 3: 0x6688BB).
    CHECK_EQ(fb.pixel(18, 18), 0x6688BBu);
    CHECK(fb.guards_intact());
}

void published_pointer_shape_is_drawn_offset_by_its_hot_spot_in_its_colours() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    guest.set_all_pixels(3);

    PointerShape shape{};
    shape.width = 3;
    shape.height = 3;
    shape.hot_x = 1;
    shape.hot_y = 1;
    shape.pixels[0][0] = 1;
    shape.pixels[1][1] = 2;
    shape.pixels[2][2] = 3;
    shape.rgb[1] = 0x112233;
    shape.rgb[2] = 0x445566;
    shape.rgb[3] = 0x778899;
    DisplayState state;
    state.pointer = &shape;
    display.follow_pointer_shape(state);

    display.move_pointer(guest.machine(), kScreen, 10, 10);
    display.set_pointer_visible(guest.machine(), kScreen, true);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    // The image starts at (9, 9), the hot spot pixel is on (10, 10); scaled by 2.
    CHECK_EQ(fb.pixel(18, 18), 0x112233u);
    CHECK_EQ(fb.pixel(20, 20), 0x445566u);
    CHECK_EQ(fb.pixel(22, 22), 0x778899u);
    CHECK_EQ(fb.pixel(20, 18), 0x6688BBu);
    CHECK_EQ(fb.pixel(16, 16), 0x6688BBu);
    CHECK_EQ(fb.pixel(24, 24), 0x6688BBu);

    // Moving redraws the old area, including the part left of the position.
    display.move_pointer(guest.machine(), kScreen, 30, 40);
    CHECK_EQ(fb.pixel(18, 18), 0x6688BBu);
    CHECK_EQ(fb.pixel(22, 22), 0x6688BBu);
    CHECK_EQ(fb.pixel(58, 78), 0x112233u);
    CHECK_EQ(fb.pixel(60, 80), 0x445566u);
    CHECK(fb.guards_intact());

    // Without a published shape the default pointer is back, red at its tip.
    state.pointer = nullptr;
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});
    CHECK_EQ(fb.pixel(60, 80), 0xE04040u);
    CHECK_EQ(fb.pixel(58, 78), 0x6688BBu);
}

void pointer_is_not_drawn_when_hidden() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    guest.set_all_pixels(3);

    display.move_pointer(guest.machine(), kScreen, 10, 10);
    CHECK(!display.pointer_visible());
    CHECK_EQ(display.pointer_x(), 10);
    CHECK_EQ(display.pointer_y(), 10);

    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    CHECK_EQ(fb.pixel(20, 20), 0x6688BBu);
    CHECK_EQ(fb.pixel(22, 24), 0x6688BBu);
    CHECK(fb.guards_intact());
}

void move_pointer_restores_the_old_area_and_draws_the_new_one() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    guest.set_all_pixels(3);

    display.move_pointer(guest.machine(), kScreen, 10, 10);
    display.set_pointer_visible(guest.machine(), kScreen, true);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    CHECK_EQ(fb.pixel(20, 20), 0xE04040u);
    CHECK_EQ(fb.pixel(22, 24), 0x000000u);

    display.move_pointer(guest.machine(), kScreen, 30, 40);
    CHECK_EQ(display.pointer_x(), 30);
    CHECK_EQ(display.pointer_y(), 40);

    // Old position is restored to the guest screen pen colour.
    CHECK_EQ(fb.pixel(20, 20), 0x6688BBu);
    CHECK_EQ(fb.pixel(22, 24), 0x6688BBu);

    // New position (30, 40) is drawn: tip (60, 80) is black, fill (62, 84) is white.
    CHECK_EQ(fb.pixel(60, 80), 0xE04040u);
    CHECK_EQ(fb.pixel(62, 84), 0x000000u);
    CHECK(fb.guards_intact());
}

void pointer_is_clipped_at_the_screen_edge_and_never_writes_outside_the_framebuffer() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    guest.set_all_pixels(3);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    display.set_pointer_visible(guest.machine(), kScreen, true);
    display.move_pointer(guest.machine(), kScreen, 635, 395);

    // Tip at (635*2, 395*2) = (1270, 790) is red.
    CHECK_EQ(fb.pixel(1270, 790), 0xE04040u);
    CHECK(fb.guards_intact());

    Screen small_fb(320, 200, 320 * 4, kBgrx);
    AmigaDisplay small_display;
    small_display.init(small_fb.framebuffer());
    small_display.set_pointer_visible(guest.machine(), kScreen, true);
    small_display.move_pointer(guest.machine(), kScreen, 635, 395);
    CHECK(small_fb.guards_intact());

    small_display.move_pointer(guest.machine(), kScreen, -10, -10);
    CHECK(small_fb.guards_intact());
}

void refresh_of_a_rectangle_not_touching_the_pointer_leaves_it_untouched() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    guest.set_all_pixels(3);

    display.move_pointer(guest.machine(), kScreen, 10, 10);
    display.set_pointer_visible(guest.machine(), kScreen, true);
    refresh(display, guest.machine(), kScreen, Rect{10, 10, 25, 25});

    CHECK_EQ(fb.pixel(20, 20), 0xE04040u);
    CHECK_EQ(fb.pixel(22, 24), 0x000000u);

    guest.set_pixel(100, 100, 2);
    refresh(display, guest.machine(), kScreen, Rect{100, 100, 100, 100});

    CHECK_EQ(fb.pixel(200, 200), 0xFFFFFFu);
    CHECK_EQ(fb.pixel(20, 20), 0xE04040u);
    CHECK_EQ(fb.pixel(22, 24), 0x000000u);
    CHECK(fb.guards_intact());
}

void hiding_the_pointer_erases_it() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    guest.set_all_pixels(3);

    display.move_pointer(guest.machine(), kScreen, 10, 10);
    display.set_pointer_visible(guest.machine(), kScreen, true);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    CHECK_EQ(fb.pixel(20, 20), 0xE04040u);
    CHECK_EQ(fb.pixel(22, 24), 0x000000u);

    display.set_pointer_visible(guest.machine(), kScreen, false);
    CHECK(!display.pointer_visible());

    CHECK_EQ(fb.pixel(20, 20), 0x6688BBu);
    CHECK_EQ(fb.pixel(22, 24), 0x6688BBu);
    CHECK(fb.guards_intact());
}

void pointer_overlay_respects_the_box_position_and_its_pixel_size() {
    Screen fb(1600, 1200, 1600 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.box_x(), 0u);
    CHECK_EQ(display.box_y(), 100u);

    guest.set_all_pixels(3);

    display.move_pointer(guest.machine(), kScreen, 10, 10);
    display.set_pointer_visible(guest.machine(), kScreen, true);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});

    // The box is 1600x1000, so a pointer pixel is 2x2 host pixels. The tip at
    // screen (10, 10) maps to framebuffer (10 * 2.5, 100 + 10 * 2.5) = (25, 125).
    CHECK_EQ(fb.pixel(25, 125), 0xE04040u);
    CHECK_EQ(fb.pixel(26, 125), 0xE04040u);
    CHECK_EQ(fb.pixel(25, 126), 0xE04040u);
    CHECK_EQ(fb.pixel(26, 126), 0xE04040u);
    CHECK_EQ(fb.pixel(27, 125), 0xE0E0C0u);

    // The black edge at pointer pixel (1, 2) is 2 pointer pixels down and 1 across.
    CHECK_EQ(fb.pixel(27, 129), 0x000000u);
    CHECK_EQ(fb.pixel(28, 129), 0x000000u);
    CHECK_EQ(fb.pixel(27, 130), 0x000000u);
    CHECK_EQ(fb.pixel(28, 130), 0x000000u);

    CHECK_EQ(fb.pixel(0, 0), kBorder);
    CHECK_EQ(fb.pixel(800, 600), 0x6688BBu);
    CHECK(fb.guards_intact());
}

void refresh_matches_pixel_reference() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.box_width(), 1280u);

    u8* mem = guest.machine().mem.base;
    u32 rng = 0x13579BDFu;
    for (u32 i = 0; i < kPlanesBytes; ++i) {
        rng = rng * 1664525u + 1013904223u;
        mem[kPlaneBase + i] = static_cast<u8>(rng >> 24);
    }

    refresh(display, guest.machine(), kScreen, Rect{3, 5, 77, 41});

    u32 palette[4];
    for (u32 pen = 0; pen < 4; ++pen) {
        const u32 word =
            (static_cast<u32>(mem[kColourTable + pen * 2]) << 8) | mem[kColourTable + pen * 2 + 1];
        const u32 r = ((word >> 8) & 0xF) * 17;
        const u32 g = ((word >> 4) & 0xF) * 17;
        const u32 b = (word & 0xF) * 17;
        palette[pen] = (r << 16) | (g << 8) | b;
    }

    for (u32 fy = 0; fy < 100; ++fy) {
        for (u32 fx = 0; fx < 170; ++fx) {
            const u32 lx = fx / 2;
            const u32 ly = fy / 2;
            u32 expected = kBorder;
            if (lx >= 3 && lx <= 77 && ly >= 5 && ly <= 41) {
                const u32 at = ly * kBytesPerRow + (lx >> 3);
                const u32 mask = 0x80u >> (lx & 7);
                u32 pen = 0;
                for (u32 plane = 0; plane < kPlanes; ++plane) {
                    if ((mem[kPlaneBase + plane * kPlaneBytes + at] & mask) != 0) {
                        pen |= 1u << plane;
                    }
                }
                expected = palette[pen];
            }
            CHECK_EQ(fb.pixel(fx, fy), expected);
        }
    }
    CHECK(fb.guards_intact());
}

void depth8_pens_show_24bit_colours_at_scale_1_and_2() {
    Guest guest(8);
    constexpr u32 c0 = 0x123456;
    constexpr u32 c5 = 0xA0B1C2;
    constexpr u32 c200 = 0x7E8D9C;
    constexpr u32 c255 = 0xF0E1D2;
    guest.set_rgb24(0, c0);
    guest.set_rgb24(5, c5);
    guest.set_rgb24(200, c200);
    guest.set_rgb24(255, c255);

    guest.set_pixel(0, 0, 0);
    guest.set_pixel(1, 0, 5);
    guest.set_pixel(9, 0, 200);
    guest.set_pixel(15, 0, 255);

    Screen fb1(640, 400, 640 * 4, kBgrx);
    AmigaDisplay d1;
    d1.init(fb1.framebuffer());
    CHECK_EQ(d1.box_width(), 640u);
    refresh(d1, guest.machine(), kScreen, Rect{0, 0, 15, 0});
    CHECK_EQ(fb1.pixel(0, 0), c0);
    CHECK_EQ(fb1.pixel(1, 0), c5);
    CHECK_EQ(fb1.pixel(9, 0), c200);
    CHECK_EQ(fb1.pixel(15, 0), c255);
    CHECK(fb1.guards_intact());

    Screen fb2(1280, 800, 1280 * 4, kBgrx);
    AmigaDisplay d2;
    d2.init(fb2.framebuffer());
    CHECK_EQ(d2.box_width(), 1280u);
    refresh(d2, guest.machine(), kScreen, Rect{0, 0, 15, 0});
    CHECK_EQ(fb2.pixel(0, 0), c0);
    CHECK_EQ(fb2.pixel(1, 1), c0);
    CHECK_EQ(fb2.pixel(2, 0), c5);
    CHECK_EQ(fb2.pixel(3, 1), c5);
    CHECK_EQ(fb2.pixel(18, 0), c200);
    CHECK_EQ(fb2.pixel(19, 1), c200);
    CHECK_EQ(fb2.pixel(30, 0), c255);
    CHECK_EQ(fb2.pixel(31, 1), c255);
    CHECK(fb2.guards_intact());
}

void depth8_dirty_rect_matches_pixel_reference() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest(8);
    AmigaDisplay display;
    display.init(fb.framebuffer());
    CHECK_EQ(display.box_width(), 1280u);

    u8* mem = guest.machine().mem.base;
    u32 rng = 0xCAFEBABEu;
    for (u32 pen = 0; pen < 256; ++pen) {
        rng = rng * 1664525u + 1013904223u;
        guest.set_rgb24(pen, (rng >> 8) & 0xFFFFFFu);
    }
    for (u32 i = 0; i < 8 * kPlaneBytes; ++i) {
        rng = rng * 1664525u + 1013904223u;
        mem[kPlaneBase + i] = static_cast<u8>(rng >> 24);
    }

    refresh(display, guest.machine(), kScreen, Rect{3, 5, 77, 41});

    u32 palette[256];
    for (u32 pen = 0; pen < 256; ++pen) {
        const u32 hi =
            (static_cast<u32>(mem[kColourTable + pen * 2]) << 8) | mem[kColourTable + pen * 2 + 1];
        const u32 lo = (static_cast<u32>(mem[kLowColourTable + pen * 2]) << 8) |
                       mem[kLowColourTable + pen * 2 + 1];
        const u32 r = (((hi >> 8) & 0xF) << 4) | ((lo >> 8) & 0xF);
        const u32 g = (((hi >> 4) & 0xF) << 4) | ((lo >> 4) & 0xF);
        const u32 b = ((hi & 0xF) << 4) | (lo & 0xF);
        palette[pen] = (r << 16) | (g << 8) | b;
    }

    for (u32 fy = 0; fy < 100; ++fy) {
        for (u32 fx = 0; fx < 170; ++fx) {
            const u32 lx = fx / 2;
            const u32 ly = fy / 2;
            u32 expected = kBorder;
            if (lx >= 3 && lx <= 77 && ly >= 5 && ly <= 41) {
                const u32 at = ly * kBytesPerRow + (lx >> 3);
                const u32 mask = 0x80u >> (lx & 7);
                u32 pen = 0;
                for (u32 plane = 0; plane < 8; ++plane) {
                    if ((mem[kPlaneBase + plane * kPlaneBytes + at] & mask) != 0) {
                        pen |= 1u << plane;
                    }
                }
                expected = palette[pen];
            }
            CHECK_EQ(fb.pixel(fx, fy), expected);
        }
    }
    CHECK(fb.guards_intact());
}

DisplayState mode_state(u16 width, u16 height) {
    DisplayState state;
    state.mode_width = width;
    state.mode_height = height;
    return state;
}

void low_res_screen_is_drawn_four_by_four() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest(kPlanes, 320, 200);
    AmigaDisplay display;
    display.init(fb.framebuffer());
    const DisplayState state = mode_state(320, 200);
    display.follow_pointer_shape(state);

    guest.set_all_pixels(1);
    guest.set_pixel(1, 0, 2);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 319, 199});

    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 4; x < 8; ++x) {
            CHECK_EQ(fb.pixel(x, y), 0xFFFFFFu);
        }
    }
    CHECK_EQ(fb.pixel(3, 0), 0u);
    CHECK_EQ(fb.pixel(8, 0), 0u);
    CHECK_EQ(fb.pixel(4, 4), 0u);
    CHECK_EQ(fb.pixel(1279, 799), 0u);
    CHECK(fb.guards_intact());
}

void pal_low_res_screen_rows_are_three_or_four_host_rows_covering_the_box() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest(kPlanes, 320, 256);
    AmigaDisplay display;
    display.init(fb.framebuffer());
    const DisplayState state = mode_state(320, 256);
    display.follow_pointer_shape(state);

    // Even screen rows are black and odd rows white in the first column, so
    // every run of one colour down the first host column is one screen row.
    guest.set_all_pixels(1);
    for (u32 y = 1; y < 256; y += 2) {
        guest.set_pixel(0, y, 2);
    }
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 319, 255});

    u32 runs = 0;
    u32 run_length = 0;
    for (u32 y = 0; y < 800; ++y) {
        const u32 colour = fb.pixel(0, y);
        CHECK(colour == 0u || colour == 0xFFFFFFu);
        if (y > 0 && colour != fb.pixel(0, y - 1)) {
            CHECK(run_length == 3 || run_length == 4);
            run_length = 0;
        }
        if (run_length == 0) {
            ++runs;
        }
        ++run_length;
    }
    CHECK(run_length == 3 || run_length == 4);
    CHECK_EQ(runs, 256u);
    CHECK(fb.guards_intact());
}

void super_high_res_laced_screen_is_one_by_two() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest(kPlanes, 1280, 400);
    AmigaDisplay display;
    display.init(fb.framebuffer());
    const DisplayState state = mode_state(1280, 400);
    display.follow_pointer_shape(state);

    guest.set_all_pixels(1);
    guest.set_pixel(1, 0, 2);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 1279, 399});

    CHECK_EQ(fb.pixel(1, 0), 0xFFFFFFu);
    CHECK_EQ(fb.pixel(1, 1), 0xFFFFFFu);
    CHECK_EQ(fb.pixel(0, 0), 0u);
    CHECK_EQ(fb.pixel(2, 0), 0u);
    CHECK_EQ(fb.pixel(1, 2), 0u);
    CHECK_EQ(fb.pixel(1279, 799), 0u);
    CHECK(fb.guards_intact());
}

void screen_smaller_than_its_mode_leaves_the_rest_of_the_box_in_border_colour() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest(kPlanes, 160, 100);
    AmigaDisplay display;
    display.init(fb.framebuffer());
    const DisplayState state = mode_state(320, 200);
    display.follow_pointer_shape(state);

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 159, 99});

    CHECK_EQ(fb.pixel(0, 0), 0u);
    CHECK_EQ(fb.pixel(639, 399), 0u);
    CHECK_EQ(fb.pixel(640, 0), kBorder);
    CHECK_EQ(fb.pixel(0, 400), kBorder);
    CHECK_EQ(fb.pixel(1279, 799), kBorder);
    CHECK(fb.guards_intact());
}

void changing_the_mode_clears_the_box_and_a_larger_screen_shows_its_top_left() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest;
    AmigaDisplay display;
    display.init(fb.framebuffer());
    DisplayState state = mode_state(640, 400);
    display.follow_pointer_shape(state);

    guest.set_all_pixels(1);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});
    CHECK_EQ(fb.pixel(1279, 799), 0u);

    // Only the first screen pixel is redrawn, so the rest of the box shows
    // that the mode change cleared it.
    state.mode_width = 320;
    state.mode_height = 200;
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 0, 0});
    CHECK_EQ(fb.pixel(3, 3), 0u);
    CHECK_EQ(fb.pixel(4, 0), kBorder);
    CHECK_EQ(fb.pixel(1279, 799), kBorder);

    // The 640x400 screen shows its top left 320x200 pixels in the box.
    guest.set_pixel(319, 199, 2);
    guest.set_pixel(320, 0, 2);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 639, 399});
    CHECK_EQ(fb.pixel(1276, 796), 0xFFFFFFu);
    CHECK_EQ(fb.pixel(1279, 799), 0xFFFFFFu);
    CHECK_EQ(fb.pixel(1279, 0), 0u);
    CHECK(fb.guards_intact());
}

void pointer_keeps_its_host_size_in_a_low_res_mode_and_its_place_when_the_mode_changes() {
    Screen fb(1280, 800, 1280 * 4, kBgrx);
    Guest guest(kPlanes, 320, 200);
    AmigaDisplay display;
    display.init(fb.framebuffer());
    DisplayState state = mode_state(320, 200);
    display.follow_pointer_shape(state);
    guest.set_all_pixels(3);
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 319, 199});

    display.move_pointer(guest.machine(), kScreen, 10, 10);
    display.set_pointer_visible(guest.machine(), kScreen, true);

    // Screen (10, 10) is host (40, 40); a pointer pixel is still 2x2 host pixels.
    CHECK_EQ(fb.pixel(40, 40), 0xE04040u);
    CHECK_EQ(fb.pixel(41, 41), 0xE04040u);
    CHECK_EQ(fb.pixel(42, 40), 0xE0E0C0u);
    CHECK_EQ(fb.pixel(43, 41), 0xE0E0C0u);
    CHECK_EQ(fb.pixel(44, 40), 0x6688BBu);
    CHECK_EQ(fb.pixel(39, 39), 0x6688BBu);

    // Moving restores the screen under the old position.
    display.move_pointer(guest.machine(), kScreen, 160, 100);
    CHECK_EQ(fb.pixel(40, 40), 0x6688BBu);
    CHECK_EQ(fb.pixel(640, 400), 0xE04040u);

    // A 640x400 mode puts the pointer on the same place of the monitor.
    state.mode_width = 640;
    state.mode_height = 400;
    refresh(display, guest.machine(), kScreen, Rect{0, 0, 0, 0});
    CHECK_EQ(display.pointer_x(), 320);
    CHECK_EQ(display.pointer_y(), 200);
    CHECK(fb.guards_intact());
}

}

int main() {
    deep_screen_is_square_and_pillarboxed();
    scale_two_at_1280x800_and_no_border();
    wide_framebuffer_gets_a_centred_box_with_border_colour_beside_it();
    tall_framebuffer_gets_a_centred_box_with_border_colour_above_and_below_it();
    framebuffer_smaller_than_the_screen_shows_all_of_it_downscaled();
    refresh_draws_pens_through_the_palette();
    dirty_rect_only_touches_its_blocks();
    dirty_rect_is_clipped_to_the_screen();
    screen_zero_clears_to_border_colour();
    guards_intact();
    pointer_image_has_a_tip_at_the_origin();
    pointer_is_drawn_over_the_screen_on_refresh();
    published_pointer_shape_is_drawn_offset_by_its_hot_spot_in_its_colours();
    pointer_is_not_drawn_when_hidden();
    move_pointer_restores_the_old_area_and_draws_the_new_one();
    pointer_is_clipped_at_the_screen_edge_and_never_writes_outside_the_framebuffer();
    refresh_of_a_rectangle_not_touching_the_pointer_leaves_it_untouched();
    hiding_the_pointer_erases_it();
    pointer_overlay_respects_the_box_position_and_its_pixel_size();
    refresh_matches_pixel_reference();
    depth8_pens_show_24bit_colours_at_scale_1_and_2();
    depth8_dirty_rect_matches_pixel_reference();
    low_res_screen_is_drawn_four_by_four();
    pal_low_res_screen_rows_are_three_or_four_host_rows_covering_the_box();
    super_high_res_laced_screen_is_one_by_two();
    screen_smaller_than_its_mode_leaves_the_rest_of_the_box_in_border_colour();
    changing_the_mode_clears_the_box_and_a_larger_screen_shows_its_top_left();
    pointer_keeps_its_host_size_in_a_low_res_mode_and_its_place_when_the_mode_changes();
    return check::finish("test_amiga_display");
}
