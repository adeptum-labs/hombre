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
#include "keymap_se.hpp"

using namespace hombre;

namespace {

constexpr u8 kShift = kModLShift;
constexpr u8 kAltGr = kModAltGr;

struct KeyCase {
    u8 usage;
    u8 normal;
    u8 shifted;
    u8 alt_gr;
};

constexpr KeyCase kDigitRow[] = {
    {0x1E, '1', '!', 0},
    {0x1F, '2', '"', '@'},
    {0x20, '3', '#', 0xA3},
    {0x21, '4', 0xA4, '$'},
    {0x22, '5', '%', 0},
    {0x23, '6', '&', 0},
    {0x24, '7', '/', '{'},
    {0x25, '8', '(', '['},
    {0x26, '9', ')', ']'},
    {0x27, '0', '=', '}'},
};

constexpr KeyCase kPunctuation[] = {
    {0x2D, '+', '?', '\\'},
    {0x31, '\'', '*', 0},
    {0x32, '\'', '*', 0},
    {0x35, 0xA7, 0xBD, 0},
    {0x36, ',', ';', 0},
    {0x37, '.', ':', 0},
    {0x38, '-', '_', 0},
    {0x64, '<', '>', '|'},
};

void letters_lower_upper_and_caps() {
    CHECK_EQ(keymap_se(0x04, 0, false), 'a');
    CHECK_EQ(keymap_se(0x04, kShift, false), 'A');
    CHECK_EQ(keymap_se(0x04, 0, true), 'A');
    CHECK_EQ(keymap_se(0x04, kShift, true), 'a');
    CHECK_EQ(keymap_se(0x1D, 0, false), 'z');
    CHECK_EQ(keymap_se(0x1D, kModRShift, false), 'Z');
    CHECK_EQ(keymap_se(0x1D, kModRShift, true), 'z');
}

void swedish_letters() {
    CHECK_EQ(keymap_se(0x2F, 0, false), 0xE5);
    CHECK_EQ(keymap_se(0x2F, kShift, false), 0xC5);
    CHECK_EQ(keymap_se(0x2F, 0, true), 0xC5);
    CHECK_EQ(keymap_se(0x2F, kShift, true), 0xE5);
    CHECK_EQ(keymap_se(0x34, 0, false), 0xE4);
    CHECK_EQ(keymap_se(0x34, kShift, false), 0xC4);
    CHECK_EQ(keymap_se(0x34, 0, true), 0xC4);
    CHECK_EQ(keymap_se(0x34, kShift, true), 0xE4);
    CHECK_EQ(keymap_se(0x33, 0, false), 0xF6);
    CHECK_EQ(keymap_se(0x33, kShift, false), 0xD6);
    CHECK_EQ(keymap_se(0x33, 0, true), 0xD6);
    CHECK_EQ(keymap_se(0x33, kShift, true), 0xF6);
}

void digit_row_normal_shift_altgr() {
    for (const KeyCase& c : kDigitRow) {
        CHECK_EQ(keymap_se(c.usage, 0, false), c.normal);
        CHECK_EQ(keymap_se(c.usage, kShift, false), c.shifted);
        CHECK_EQ(keymap_se(c.usage, kAltGr, false), c.alt_gr != 0 ? c.alt_gr : c.normal);
    }
    CHECK_EQ(keymap_se(0x1E, 0, true), '1');
    CHECK_EQ(keymap_se(0x1E, kAltGr, true), '1');
}

void punctuation_keys() {
    for (const KeyCase& c : kPunctuation) {
        CHECK_EQ(keymap_se(c.usage, 0, false), c.normal);
        CHECK_EQ(keymap_se(c.usage, kShift, false), c.shifted);
        CHECK_EQ(keymap_se(c.usage, kAltGr, false), c.alt_gr != 0 ? c.alt_gr : c.normal);
    }
}

void dead_keys_give_nothing() {
    CHECK_EQ(keymap_se(0x2E, 0, false), 0);
    CHECK_EQ(keymap_se(0x2E, kShift, false), 0);
    CHECK_EQ(keymap_se(0x2E, kAltGr, false), 0);
    CHECK_EQ(keymap_se(0x30, 0, false), 0);
    CHECK_EQ(keymap_se(0x30, kShift, false), 0);
    CHECK_EQ(keymap_se(0x30, kAltGr, false), '~');
    CHECK_EQ(keymap_se(0x30, kAltGr, true), '~');
}

void control_keys() {
    CHECK_EQ(keymap_se(0x28, 0, false), '\n');
    CHECK_EQ(keymap_se(0x28, kShift, false), '\n');
    CHECK_EQ(keymap_se(0x29, 0, false), 0x1B);
    CHECK_EQ(keymap_se(0x2A, 0, false), 0x08);
    CHECK_EQ(keymap_se(0x2B, 0, false), '\t');
    CHECK_EQ(keymap_se(0x2C, 0, false), ' ');
    CHECK_EQ(keymap_se(0x4C, 0, false), 0x7F);
}

void keypad() {
    CHECK_EQ(keymap_se(0x54, 0, false), '/');
    CHECK_EQ(keymap_se(0x55, 0, false), '*');
    CHECK_EQ(keymap_se(0x56, 0, false), '-');
    CHECK_EQ(keymap_se(0x57, 0, false), '+');
    CHECK_EQ(keymap_se(0x58, 0, false), '\n');
    CHECK_EQ(keymap_se(0x59, 0, false), '1');
    CHECK_EQ(keymap_se(0x5A, 0, false), '2');
    CHECK_EQ(keymap_se(0x5B, 0, false), '3');
    CHECK_EQ(keymap_se(0x5C, 0, false), '4');
    CHECK_EQ(keymap_se(0x5D, 0, false), '5');
    CHECK_EQ(keymap_se(0x5E, 0, false), '6');
    CHECK_EQ(keymap_se(0x5F, 0, false), '7');
    CHECK_EQ(keymap_se(0x60, 0, false), '8');
    CHECK_EQ(keymap_se(0x61, 0, false), '9');
    CHECK_EQ(keymap_se(0x62, 0, false), '0');
    CHECK_EQ(keymap_se(0x63, 0, false), ',');
}

void unmapped_usages_give_zero() {
    for (u8 usage = 0x4F; usage <= 0x52; ++usage) {
        CHECK_EQ(keymap_se(usage, 0, false), 0);
        CHECK_EQ(keymap_se(usage, kShift, false), 0);
    }
    CHECK_EQ(keymap_se(0x3A, 0, false), 0);
    CHECK_EQ(keymap_se(0x3A, kShift, false), 0);
    CHECK_EQ(keymap_se(0x00, 0, false), 0);
    CHECK_EQ(keymap_se(0x39, 0, false), 0);
    CHECK_EQ(keymap_se(0x39, 0, true), 0);
}

}

int main() {
    letters_lower_upper_and_caps();
    swedish_letters();
    digit_row_normal_shift_altgr();
    punctuation_keys();
    dead_keys_give_nothing();
    control_keys();
    keypad();
    unmapped_usages_give_zero();
    return check::finish("test_keymap_se");
}
