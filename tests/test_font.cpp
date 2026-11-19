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
#include "font.hpp"

#include <cstring>

using namespace hombre;

namespace {

constexpr u8 kLatin1Codes[] = {
    0xC4, 0xC5, 0xD6, 0xE4, 0xE5, 0xF6, 0xC9, 0xE9, 0xDC, 0xFC, 0xA7, 0xA3, 0xA4, 0xBD, 0xB4, 0xA9,
};

constexpr usize kLatin1Count = sizeof(kLatin1Codes) / sizeof(kLatin1Codes[0]);

const u8* glyph_of(u8 code) {
    return glyph(static_cast<char>(code));
}

bool same_bitmap(const u8* a, const u8* b) {
    return std::memcmp(a, b, sizeof(kFont8x8[0])) == 0;
}

void latin1_letters_are_not_question_marks() {
    for (usize i = 0; i < kLatin1Count; ++i) {
        CHECK(glyph_of(kLatin1Codes[i]) == kFont8x8Latin1[i]);
        CHECK(!same_bitmap(glyph_of(kLatin1Codes[i]), glyph('?')));
    }
}

void latin1_glyphs_are_distinct_from_each_other() {
    for (usize i = 0; i < kLatin1Count; ++i) {
        for (usize j = i + 1; j < kLatin1Count; ++j) {
            CHECK(!same_bitmap(glyph_of(kLatin1Codes[i]), glyph_of(kLatin1Codes[j])));
        }
    }
}

void a_ring_differs_from_a_diaeresis() {
    CHECK(!same_bitmap(glyph_of(0xE5), glyph_of(0xE4)));
    CHECK(!same_bitmap(glyph_of(0xE5), glyph('a')));
    CHECK(!same_bitmap(glyph_of(0xE4), glyph('a')));
    CHECK(!same_bitmap(glyph_of(0xC5), glyph_of(0xC4)));
    CHECK(!same_bitmap(glyph_of(0xC5), glyph('A')));
    CHECK(!same_bitmap(glyph_of(0xC4), glyph('A')));
}

void unlisted_high_codes_are_question_marks() {
    CHECK(glyph_of(0x80) == glyph('?'));
    CHECK(glyph_of(0xA0) == glyph('?'));
    CHECK(glyph_of(0xFF) == glyph('?'));
}

void ascii_unchanged() {
    CHECK(glyph(' ') == kFont8x8[0]);
    CHECK(glyph('A') == kFont8x8['A' - 0x20]);
    CHECK(glyph('~') == kFont8x8['~' - 0x20]);
}

}

int main() {
    latin1_letters_are_not_question_marks();
    latin1_glyphs_are_distinct_from_each_other();
    a_ring_differs_from_a_diaeresis();
    unlisted_high_codes_are_question_marks();
    ascii_unchanged();
    return check::finish("test_font");
}
