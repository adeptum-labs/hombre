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
#include "check.hpp"
#include "framebuffer.hpp"

#include <cstring>
#include <vector>

using namespace hombre;

namespace {

constexpr u8 kGuardByte = 0xAA;
constexpr usize kGuard = 64;
constexpr u32 kWhite = 0xFFFFFF;
constexpr u32 kBlack = 0x000000;

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

    u32 width() const { return width_; }
    u32 height() const { return height_; }

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

    // Row padding between width * 4 and pitch must never be written.
    bool padding_is(u8 value) {
        for (u32 y = 0; y < height_; ++y) {
            for (usize b = static_cast<usize>(width_) * 4; b < pitch_; ++b) {
                if (base()[static_cast<usize>(y) * pitch_ + b] != value) {
                    return false;
                }
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

// Pixels are 0x00RRGGBB when the format is BGRX8.
constexpr u32 kBgrx = kFramebufferBgrx8;
constexpr u32 kRgbx = kFramebufferRgbx8;

// Glyph bitmaps written out here independently of the font table's own
// access path: one byte per row, bit 0 is the leftmost pixel.
constexpr u8 kGlyphH[8] = {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00};
constexpr u8 kGlyphO[8] = {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00};
constexpr u8 kGlyphM[8] = {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00};
constexpr u8 kGlyphB[8] = {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00};
constexpr u8 kGlyphR[8] = {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00};
constexpr u8 kGlyphE[8] = {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00};
constexpr u8 kGlyphQuestion[8] = {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00};

bool glyph_is(char c, const u8 (&expected)[8]) {
    const u8* g = glyph(c);
    for (int row = 0; row < 8; ++row) {
        if (g[row] != expected[row]) {
            return false;
        }
    }
    return true;
}

bool glyph_pixel(const u8 (&bitmap)[8], u32 x, u32 y) {
    return x < 8 && y < 8 && ((bitmap[y] >> x) & 1);
}

// Checks that the w x h area at (ox, oy) shows `bitmap` scaled by `scale`
// in `on`, and everything else in the screen is `off`.
bool shows_glyph(Screen& s, const u8 (&bitmap)[8], u32 ox, u32 oy, u32 scale, u32 on, u32 off) {
    for (u32 y = 0; y < s.height(); ++y) {
        for (u32 x = 0; x < s.width(); ++x) {
            bool set = false;
            if (x >= ox && y >= oy) {
                set = glyph_pixel(bitmap, (x - ox) / scale, (y - oy) / scale);
            }
            if (s.pixel(x, y) != (set ? on : off)) {
                return false;
            }
        }
    }
    return true;
}

usize count_pixels(Screen& s, u32 value) {
    usize n = 0;
    for (u32 y = 0; y < s.height(); ++y) {
        for (u32 x = 0; x < s.width(); ++x) {
            n += s.pixel(x, y) == value;
        }
    }
    return n;
}

void fill_sets_every_pixel() {
    Screen s(7, 5, 7 * 4, kBgrx);
    s.framebuffer().fill(0x123456);
    CHECK_EQ(count_pixels(s, 0x123456), 35u);
    CHECK(s.guards_intact());
}

void bgrx_and_rgbx_byte_order() {
    Screen bgrx(3, 2, 12, kBgrx);
    bgrx.framebuffer().fill(0x112233);
    const u8 expected_bgrx[4] = {0x33, 0x22, 0x11, 0x00};
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 3; ++x) {
            CHECK(std::memcmp(bgrx.base() + y * 12 + x * 4, expected_bgrx, 4) == 0);
        }
    }

    Screen rgbx(3, 2, 12, kRgbx);
    rgbx.framebuffer().fill(0x112233);
    const u8 expected_rgbx[4] = {0x11, 0x22, 0x33, 0x00};
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 3; ++x) {
            CHECK(std::memcmp(rgbx.base() + y * 12 + x * 4, expected_rgbx, 4) == 0);
        }
    }

    // Text goes through the same conversion.
    Screen text(8, 8, 32, kRgbx);
    text.framebuffer().fill(kBlack);
    text.framebuffer().draw_text(0, 0, "H", 0x112233, 1);
    CHECK(std::memcmp(text.base(), expected_rgbx, 4) == 0);  // H has pixel (0, 0) set
    CHECK(shows_glyph(text, kGlyphH, 0, 0, 1, 0x332211, kBlack));

    CHECK(bgrx.guards_intact());
    CHECK(rgbx.guards_intact());
    CHECK(text.guards_intact());
}

void draw_text_sets_some_pixels_for_H() {
    Screen s(16, 16, 64, kBgrx);
    s.framebuffer().fill(kBlack);
    s.framebuffer().draw_text(3, 2, "H", kWhite, 1);
    CHECK(count_pixels(s, kWhite) > 0);
    CHECK(shows_glyph(s, kGlyphH, 3, 2, 1, kWhite, kBlack));
    CHECK(s.guards_intact());
}

void draws_successive_characters_eight_pixels_apart() {
    Screen s(24, 8, 96, kBgrx);
    s.framebuffer().fill(kBlack);
    s.framebuffer().draw_text(0, 0, "HOM", kWhite, 1);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 24; ++x) {
            const u8(&bitmap)[8] = x < 8 ? kGlyphH : x < 16 ? kGlyphO : kGlyphM;
            CHECK_EQ(s.pixel(x, y), glyph_pixel(bitmap, x % 8, y) ? kWhite : kBlack);
        }
    }
    CHECK(s.guards_intact());
}

void space_draws_nothing() {
    Screen s(16, 16, 64, kBgrx);
    s.framebuffer().fill(0x445566);
    const auto before = s.snapshot();
    s.framebuffer().draw_text(0, 0, " ", kWhite, 1);
    s.framebuffer().draw_text(4, 4, "   ", kWhite, 3);
    CHECK(s.snapshot() == before);
    CHECK(s.guards_intact());
}

void scale_two_doubles_glyph_size() {
    Screen s(32, 32, 128, kBgrx);
    s.framebuffer().fill(kBlack);
    s.framebuffer().draw_text(5, 3, "H", kWhite, 2);
    CHECK(shows_glyph(s, kGlyphH, 5, 3, 2, kWhite, kBlack));

    Screen one(32, 32, 128, kBgrx);
    one.framebuffer().fill(kBlack);
    one.framebuffer().draw_text(5, 3, "H", kWhite, 1);
    CHECK_EQ(count_pixels(s, kWhite), 4 * count_pixels(one, kWhite));

    // The next character starts 16 pixels further right.
    Screen two(32, 16, 128, kBgrx);
    two.framebuffer().fill(kBlack);
    two.framebuffer().draw_text(0, 0, "HO", kWhite, 2);
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 32; ++x) {
            const u8(&bitmap)[8] = x < 16 ? kGlyphH : kGlyphO;
            CHECK_EQ(two.pixel(x, y), glyph_pixel(bitmap, (x % 16) / 2, y / 2) ? kWhite : kBlack);
        }
    }
    CHECK(s.guards_intact());
    CHECK(one.guards_intact());
    CHECK(two.guards_intact());
}

void clips_at_right_and_bottom_edges_without_overflow() {
    // Pitch larger than the row, so a write past the right edge would land in
    // the padding, and the bottom row ends exactly at the end of the buffer.
    Screen s(20, 12, 96, kBgrx);
    s.framebuffer().fill(kBlack);
    s.framebuffer().draw_text(16, 8, "HHH", kWhite, 3);
    CHECK(count_pixels(s, kWhite) > 0);
    CHECK(shows_glyph(s, kGlyphH, 16, 8, 3, kWhite, kBlack));
    CHECK(s.guards_intact());

    // Partly visible at the right edge only.
    Screen right(10, 8, 40, kBgrx);
    right.framebuffer().fill(kBlack);
    right.framebuffer().draw_text(6, 0, "H", kWhite, 1);
    CHECK(shows_glyph(right, kGlyphH, 6, 0, 1, kWhite, kBlack));
    CHECK(right.guards_intact());

    // Partly visible at the bottom edge only.
    Screen bottom(8, 10, 32, kBgrx);
    bottom.framebuffer().fill(kBlack);
    bottom.framebuffer().draw_text(0, 6, "H", kWhite, 1);
    CHECK(shows_glyph(bottom, kGlyphH, 0, 6, 1, kWhite, kBlack));
    CHECK(bottom.guards_intact());

    // Text that is entirely off screen, or sized to overflow 32-bit
    // arithmetic, changes nothing.
    Screen off(20, 12, 96, kBgrx);
    off.framebuffer().fill(0x445566);
    const auto before = off.snapshot();
    Framebuffer fb = off.framebuffer();
    fb.draw_text(20, 0, "HHH", kWhite, 1);
    fb.draw_text(0, 12, "HHH", kWhite, 1);
    fb.draw_text(20, 12, "HHH", kWhite, 1);
    fb.draw_text(0xFFFFFFFFu, 0, "HHH", kWhite, 1);
    fb.draw_text(0, 0xFFFFFFFFu, "HHH", kWhite, 1);
    fb.draw_text(0xFFFFFFFFu, 0xFFFFFFFFu, "HHH", kWhite, 0xFFFFFFFFu);
    fb.draw_text(0x80000000u, 0x80000000u, "HHH", kWhite, 0x80000000u);
    CHECK(off.snapshot() == before);
    CHECK(off.guards_intact());

    // A huge scale from a visible origin still clips to the screen.
    Screen huge(20, 12, 96, kBgrx);
    huge.framebuffer().fill(kBlack);
    huge.framebuffer().draw_text(0, 0, "HHH", kWhite, 0xFFFFFFFFu);
    CHECK_EQ(count_pixels(huge, kWhite), 20u * 12u);  // the top left pixel of H covers it all
    CHECK(huge.guards_intact());

    // Only the leftmost column of the first character fits on screen: the
    // left stem of H is seven rows tall.
    Screen edge(24, 8, 96, kBgrx);
    edge.framebuffer().fill(kBlack);
    edge.framebuffer().draw_text(23, 0, "HH", kWhite, 1);
    CHECK_EQ(count_pixels(edge, kWhite), 7u);
    CHECK(shows_glyph(edge, kGlyphH, 23, 0, 1, kWhite, kBlack));
    CHECK(edge.guards_intact());
}

void degenerate_arguments_draw_nothing() {
    Screen s(16, 16, 64, kBgrx);
    s.framebuffer().fill(0x445566);
    const auto before = s.snapshot();
    Framebuffer fb = s.framebuffer();
    fb.draw_text(0, 0, nullptr, kWhite, 1);
    fb.draw_text(0, 0, "HOMBRE", kWhite, 0);
    fb.draw_text(0, 0, "", kWhite, 1);
    CHECK(s.snapshot() == before);
    CHECK(s.guards_intact());

    // No buffer, no size, or a pitch too small for the row: nothing is
    // written and nothing crashes.
    Framebuffer none(nullptr, 16, 16, 64, kBgrx);
    none.fill(kWhite);
    none.draw_text(0, 0, "H", kWhite, 1);
    Framebuffer empty(s.base(), 0, 16, 64, kBgrx);
    empty.fill(kWhite);
    empty.draw_text(0, 0, "H", kWhite, 1);
    Framebuffer short_pitch(s.base(), 16, 16, 60, kBgrx);
    short_pitch.fill(kWhite);
    short_pitch.draw_text(0, 0, "H", kWhite, 1);
    CHECK(s.snapshot() == before);
    CHECK(s.guards_intact());
}

void out_of_range_char_draws_question_mark() {
    Screen question(8, 8, 32, kBgrx);
    question.framebuffer().fill(kBlack);
    question.framebuffer().draw_text(0, 0, "?", kWhite, 1);
    CHECK(shows_glyph(question, kGlyphQuestion, 0, 0, 1, kWhite, kBlack));

    const char out_of_range[] = {'\x01', '\x1F', '\x7F', '\x80', '\x9F', '\n'};
    for (char c : out_of_range) {
        Screen s(8, 8, 32, kBgrx);
        s.framebuffer().fill(kBlack);
        const char text[2] = {c, '\0'};
        s.framebuffer().draw_text(0, 0, text, kWhite, 1);
        CHECK(shows_glyph(s, kGlyphQuestion, 0, 0, 1, kWhite, kBlack));
        CHECK(s.guards_intact());
    }
}

void pitch_larger_than_width_is_respected() {
    // 5 pixels per row but 32 bytes between rows: 12 bytes of padding.
    Screen s(5, 4, 32, kBgrx);
    CHECK(s.padding_is(0x11));
    s.framebuffer().fill(kWhite);
    CHECK_EQ(count_pixels(s, kWhite), 20u);
    CHECK(s.padding_is(0x11));
    CHECK(s.guards_intact());

    // The second row starts at byte 32, not at byte 20.
    CHECK_EQ(s.pixel(0, 1), kWhite);
    CHECK(std::memcmp(s.base() + 20, "\x11\x11\x11\x11", 4) == 0);
    CHECK_EQ(s.base()[32], 0xFFu);

    Screen t(8, 8, 48, kRgbx);
    t.framebuffer().fill(kBlack);
    t.framebuffer().draw_text(0, 0, "H", kWhite, 1);
    CHECK(shows_glyph(t, kGlyphH, 0, 0, 1, kWhite, kBlack));
    CHECK(t.padding_is(0x11));
    CHECK(t.guards_intact());
}

void put_row_scales_and_clips() {
    const u32 row[3] = {0x112233, 0x445566, 0x778899};

    // 3 pixels at scale 2 give a 6x2 block at (2, 1) in BGRX.
    Screen bgrx(10, 6, 40, kBgrx);
    bgrx.framebuffer().fill(kBlack);
    bgrx.framebuffer().put_row(2, 1, row, 3, 2);
    for (u32 y = 0; y < 6; ++y) {
        for (u32 x = 0; x < 10; ++x) {
            const bool inside = x >= 2 && x < 8 && y >= 1 && y < 3;
            const u32 expected = inside ? row[(x - 2) / 2] : kBlack;
            CHECK_EQ(bgrx.pixel(x, y), expected);
        }
    }
    CHECK(bgrx.guards_intact());

    // The same row in RGBX swaps red and blue bytes in memory.
    const u32 expected_rgbx[3] = {0x332211, 0x665544, 0x998877};
    Screen rgbx(10, 6, 40, kRgbx);
    rgbx.framebuffer().fill(kBlack);
    rgbx.framebuffer().put_row(2, 1, row, 3, 2);
    for (u32 y = 0; y < 6; ++y) {
        for (u32 x = 0; x < 10; ++x) {
            const bool inside = x >= 2 && x < 8 && y >= 1 && y < 3;
            const u32 expected = inside ? expected_rgbx[(x - 2) / 2] : kBlack;
            CHECK_EQ(rgbx.pixel(x, y), expected);
        }
    }
    CHECK(rgbx.guards_intact());

    // A row running off the right and bottom edges is clipped without touching padding.
    Screen clipped(5, 3, 32, kBgrx);
    clipped.framebuffer().fill(kBlack);
    clipped.framebuffer().put_row(2, 2, row, 3, 2);
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 0; x < 5; ++x) {
            const u32 expected = (y == 2 && x >= 2) ? row[(x - 2) / 2] : kBlack;
            CHECK_EQ(clipped.pixel(x, y), expected);
        }
    }
    CHECK(clipped.padding_is(0x11));
    CHECK(clipped.guards_intact());
}

void put_mapped_row_repeats_source_pixels_by_column_and_clips() {
    const u32 row[3] = {0x112233, 0x445566, 0x778899};
    // Source pixel 0 is shown once, 1 three times and 2 twice.
    const u16 columns[6] = {0, 1, 1, 1, 2, 2};

    Screen screen(8, 5, 40, kBgrx);
    screen.framebuffer().fill(kBlack);
    screen.framebuffer().put_mapped_row(1, 1, 2, row, columns, 6);
    for (u32 y = 0; y < 5; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool inside = x >= 1 && x < 7 && y >= 1 && y < 3;
            const u32 expected = inside ? row[columns[x - 1]] : kBlack;
            CHECK_EQ(screen.pixel(x, y), expected);
        }
    }
    CHECK(screen.padding_is(0x11));
    CHECK(screen.guards_intact());

    // A row running off the right and bottom edges is clipped.
    Screen clipped(4, 3, 24, kBgrx);
    clipped.framebuffer().fill(kBlack);
    clipped.framebuffer().put_mapped_row(2, 2, 5, row, columns, 6);
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 expected = (y == 2 && x >= 2) ? row[columns[x - 2]] : kBlack;
            CHECK_EQ(clipped.pixel(x, y), expected);
        }
    }
    CHECK(clipped.padding_is(0x11));
    CHECK(clipped.guards_intact());
}

void font_has_the_hombre_letters() {
    CHECK(glyph_is('H', kGlyphH));
    CHECK(glyph_is('O', kGlyphO));
    CHECK(glyph_is('M', kGlyphM));
    CHECK(glyph_is('B', kGlyphB));
    CHECK(glyph_is('R', kGlyphR));
    CHECK(glyph_is('E', kGlyphE));
    CHECK(glyph_is('?', kGlyphQuestion));
}

void font_glyphs_are_present_and_distinct() {
    CHECK_EQ(sizeof(kFont8x8) / sizeof(kFont8x8[0]), 95u);

    // Space is empty and every other glyph has at least one set pixel.
    for (int row = 0; row < 8; ++row) {
        CHECK_EQ(kFont8x8[0][row], 0u);
    }
    for (int i = 1; i < 95; ++i) {
        bool any = false;
        for (int row = 0; row < 8; ++row) {
            any = any || kFont8x8[i][row] != 0;
        }
        CHECK(any);
    }

    // No two printable glyphs are the same, which catches a copy and paste
    // slip in the table.
    for (int a = 1; a < 95; ++a) {
        for (int b = a + 1; b < 95; ++b) {
            CHECK(std::memcmp(kFont8x8[a], kFont8x8[b], 8) != 0);
        }
    }

    // glyph() indexes the table by character code.
    for (int c = 0x20; c <= 0x7E; ++c) {
        CHECK(glyph(static_cast<char>(c)) == kFont8x8[c - 0x20]);
    }
    CHECK(glyph('\x7F') == kFont8x8['?' - 0x20]);
    CHECK(glyph('\x1F') == kFont8x8['?' - 0x20]);
    CHECK(glyph('\x9F') == kFont8x8['?' - 0x20]);
    CHECK(glyph('\xFF') == kFont8x8ExtLatin1[0xFF - 0xA0]);
}

}

int main() {
    fill_sets_every_pixel();
    bgrx_and_rgbx_byte_order();
    draw_text_sets_some_pixels_for_H();
    draws_successive_characters_eight_pixels_apart();
    space_draws_nothing();
    scale_two_doubles_glyph_size();
    clips_at_right_and_bottom_edges_without_overflow();
    degenerate_arguments_draw_nothing();
    out_of_range_char_draws_question_mark();
    pitch_larger_than_width_is_respected();
    put_row_scales_and_clips();
    put_mapped_row_repeats_source_pixels_by_column_and_clips();
    font_has_the_hombre_letters();
    font_glyphs_are_present_and_distinct();
    return check::finish("test_framebuffer");
}
