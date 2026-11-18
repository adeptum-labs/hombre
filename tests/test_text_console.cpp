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

#include "check.hpp"
#include "framebuffer.hpp"
#include "text_console.hpp"

#include <cstring>
#include <vector>

using namespace hombre;

namespace {

constexpr u8 kGuardByte = 0xAA;
constexpr usize kGuard = 64;
constexpr u32 kFg = 0xE0E0FF;
constexpr u32 kBg = 0x000040;
constexpr u32 kBgrx = kFramebufferBgrx8;

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

void first_char_lands_in_the_top_left_cell() {
    constexpr u32 top = 16;
    constexpr u32 scale = 2;
    Screen s(80, 100, 80 * 4, kBgrx);
    s.framebuffer().fill(kBg);
    TextConsole console;
    console.init(s.framebuffer(), top, scale, kFg, kBg);
    console.put('H');

    Screen expected(80, 100, 80 * 4, kBgrx);
    expected.framebuffer().fill(kBg);
    expected.framebuffer().draw_text(0, top, "H", kFg, scale);

    CHECK(s.snapshot() == expected.snapshot());
    CHECK(s.guards_intact());
}

void newline_moves_to_the_next_row() {
    constexpr u32 top = 8;
    constexpr u32 scale = 2;
    constexpr u32 cell = 8 * scale;
    Screen s(80, 100, 80 * 4, kBgrx);
    s.framebuffer().fill(kBg);
    TextConsole console;
    console.init(s.framebuffer(), top, scale, kFg, kBg);
    console.put('\n');
    console.put('H');

    Screen expected(80, 100, 80 * 4, kBgrx);
    expected.framebuffer().fill(kBg);
    expected.framebuffer().draw_text(0, top + cell, "H", kFg, scale);

    CHECK(s.snapshot() == expected.snapshot());
    CHECK(s.guards_intact());
}

void tab_advances_to_the_next_eight_column_stop() {
    constexpr u32 top = 8;
    constexpr u32 cell = 8;
    Screen s(128, 64, 128 * 4, kBgrx);
    s.framebuffer().fill(kBg);
    TextConsole console;
    console.init(s.framebuffer(), top, 1, kFg, kBg);
    console.put('\t');
    console.put('H');

    Screen expected(128, 64, 128 * 4, kBgrx);
    expected.framebuffer().fill(kBg);
    expected.framebuffer().draw_text(8 * cell, top, "H", kFg, 1);

    CHECK(s.snapshot() == expected.snapshot());
    CHECK(s.guards_intact());
}

void a_line_longer_than_the_width_wraps() {
    constexpr u32 top = 8;
    constexpr u32 cell = 8;
    Screen s(32, 64, 32 * 4, kBgrx);
    s.framebuffer().fill(kBg);
    TextConsole console;
    console.init(s.framebuffer(), top, 1, kFg, kBg);
    for (const char* text = "ABCDE"; *text != '\0'; ++text) {
        console.put(*text);
    }

    Screen expected(32, 64, 32 * 4, kBgrx);
    expected.framebuffer().fill(kBg);
    expected.framebuffer().draw_text(0, top, "ABCD", kFg, 1);
    expected.framebuffer().draw_text(0, top + cell, "E", kFg, 1);

    CHECK(s.snapshot() == expected.snapshot());
    CHECK(s.guards_intact());
}

void overflow_scrolls_the_region() {
    Screen s(32, 16, 32 * 4, kBgrx);  // four columns and two rows of cells
    s.framebuffer().fill(kBg);
    TextConsole console;
    console.init(s.framebuffer(), 0, 1, kFg, kBg);
    for (const char* text = "ABCDEFGH"; *text != '\0'; ++text) {
        console.put(*text);
    }

    // The first line has scrolled away and the last row is blank again.
    Screen expected(32, 16, 32 * 4, kBgrx);
    expected.framebuffer().fill(kBg);
    expected.framebuffer().draw_text(0, 0, "EFGH", kFg, 1);

    CHECK(s.snapshot() == expected.snapshot());
    CHECK(s.guards_intact());
}

void carriage_return_changes_nothing() {
    constexpr u32 top = 8;
    Screen s(64, 32, 64 * 4, kBgrx);
    s.framebuffer().fill(kBg);
    TextConsole console;
    console.init(s.framebuffer(), top, 1, kFg, kBg);
    console.put('A');
    console.put('B');
    const auto before = s.snapshot();
    console.put('\r');
    CHECK(s.snapshot() == before);

    // The cursor stayed after B, so C follows it.
    console.put('C');
    Screen expected(64, 32, 64 * 4, kBgrx);
    expected.framebuffer().fill(kBg);
    expected.framebuffer().draw_text(0, top, "ABC", kFg, 1);
    CHECK(s.snapshot() == expected.snapshot());
    CHECK(s.guards_intact());
}

void degenerate_framebuffers_draw_nothing() {
    Screen s(32, 32, 32 * 4, kBgrx);
    s.framebuffer().fill(kBg);
    const auto before = s.snapshot();

    TextConsole null_base;
    null_base.init(Framebuffer(nullptr, 32, 32, 32 * 4, kBgrx), 0, 2, kFg, kBg);
    null_base.put('X');

    TextConsole zero_size;
    zero_size.init(Framebuffer(s.base(), 0, 32, 32 * 4, kBgrx), 0, 2, kFg, kBg);
    zero_size.put('X');

    TextConsole zero_scale;
    zero_scale.init(s.framebuffer(), 0, 0, kFg, kBg);
    zero_scale.put('X');

    TextConsole too_short;
    too_short.init(Framebuffer(s.base(), 32, 8, 32 * 4, kBgrx), 0, 2, kFg, kBg);
    too_short.put('X');

    CHECK(s.snapshot() == before);
    CHECK(s.guards_intact());
}

void a_large_screen_clamps_to_the_grid_size() {
    constexpr u32 cell = 8;
    constexpr u32 width = (TextConsole::kMaxCols + 1) * cell;
    constexpr u32 height = (TextConsole::kMaxRows + 1) * cell;
    Screen s(width, height, width * 4, kBgrx);
    s.framebuffer().fill(kBg);

    // The column past the last grid column exists on screen but never fills.
    TextConsole columns;
    columns.init(s.framebuffer(), 0, 1, kFg, kBg);
    for (u32 i = 0; i < TextConsole::kMaxCols; ++i) {
        columns.put('A');
    }
    columns.put('B');
    CHECK_EQ(s.pixel(TextConsole::kMaxCols * cell, 0), kBg);
    CHECK(s.pixel(0, cell) != kBg);

    // The row past the last grid row exists on screen but is never drawn in.
    TextConsole rows;
    rows.init(s.framebuffer(), 0, 1, kFg, kBg);
    for (u32 i = 0; i < TextConsole::kMaxRows * TextConsole::kMaxCols; ++i) {
        rows.put('A');
    }
    rows.put('B');
    CHECK_EQ(s.pixel(0, TextConsole::kMaxRows * cell), kBg);
    CHECK(s.pixel(0, (TextConsole::kMaxRows - 1) * cell) != kBg);
    CHECK(s.guards_intact());
}

}

int main() {
    first_char_lands_in_the_top_left_cell();
    newline_moves_to_the_next_row();
    tab_advances_to_the_next_eight_column_stop();
    a_line_longer_than_the_width_wraps();
    overflow_scrolls_the_region();
    carriage_return_changes_nothing();
    degenerate_framebuffers_draw_nothing();
    a_large_screen_clamps_to_the_grid_size();
    return check::finish("test_text_console");
}
