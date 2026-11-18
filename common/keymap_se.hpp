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

#pragma once

#include "types.hpp"

namespace hombre {

// Modifier bits of byte 0 of the HID boot keyboard report. The right Alt key
// is what the Swedish layout uses for AltGr.
inline constexpr u8 kModLCtrl = 0x01;
inline constexpr u8 kModLShift = 0x02;
inline constexpr u8 kModLAlt = 0x04;
inline constexpr u8 kModLGui = 0x08;
inline constexpr u8 kModRCtrl = 0x10;
inline constexpr u8 kModRShift = 0x20;
inline constexpr u8 kModAltGr = 0x40;
inline constexpr u8 kModRGui = 0x80;

inline constexpr u8 kUsageCapsLock = 0x39;

// The characters a key types in its normal, Shift and AltGr state. A zero
// AltGr character leaves the normal or shifted one in place, so a key only
// loses its character when every state is dead.
struct KeyFace {
    u8 normal;
    u8 shifted;
    u8 alt_gr;
};

// The face of a HID usage id. Letters are absent because keymap_se derives
// them from the usage id; dead keys and unmapped usages have an empty face.
constexpr KeyFace key_face(u8 usage) {
    switch (usage) {
    case 0x1E:
        return KeyFace{'1', '!', 0};
    case 0x1F:
        return KeyFace{'2', '"', '@'};
    case 0x20:
        return KeyFace{'3', '#', 0xA3};
    case 0x21:
        return KeyFace{'4', 0xA4, '$'};
    case 0x22:
        return KeyFace{'5', '%', 0};
    case 0x23:
        return KeyFace{'6', '&', 0};
    case 0x24:
        return KeyFace{'7', '/', '{'};
    case 0x25:
        return KeyFace{'8', '(', '['};
    case 0x26:
        return KeyFace{'9', ')', ']'};
    case 0x27:
        return KeyFace{'0', '=', '}'};
    case 0x28:
        return KeyFace{'\n', '\n', 0};
    case 0x29:
        return KeyFace{0x1B, 0x1B, 0};
    case 0x2A:
        return KeyFace{0x08, 0x08, 0};
    case 0x2B:
        return KeyFace{'\t', '\t', 0};
    case 0x2C:
        return KeyFace{' ', ' ', 0};
    case 0x2D:
        return KeyFace{'+', '?', '\\'};
    case 0x30:
        return KeyFace{0, 0, '~'};
    case 0x31:
        return KeyFace{'\'', '*', 0};
    case 0x32:
        return KeyFace{'\'', '*', 0};
    case 0x35:
        return KeyFace{0xA7, 0xBD, 0};
    case 0x36:
        return KeyFace{',', ';', 0};
    case 0x37:
        return KeyFace{'.', ':', 0};
    case 0x38:
        return KeyFace{'-', '_', 0};
    case 0x4C:
        return KeyFace{0x7F, 0x7F, 0};
    case 0x54:
        return KeyFace{'/', '/', 0};
    case 0x55:
        return KeyFace{'*', '*', 0};
    case 0x56:
        return KeyFace{'-', '-', 0};
    case 0x57:
        return KeyFace{'+', '+', 0};
    case 0x58:
        return KeyFace{'\n', '\n', 0};
    case 0x59:
        return KeyFace{'1', '1', 0};
    case 0x5A:
        return KeyFace{'2', '2', 0};
    case 0x5B:
        return KeyFace{'3', '3', 0};
    case 0x5C:
        return KeyFace{'4', '4', 0};
    case 0x5D:
        return KeyFace{'5', '5', 0};
    case 0x5E:
        return KeyFace{'6', '6', 0};
    case 0x5F:
        return KeyFace{'7', '7', 0};
    case 0x60:
        return KeyFace{'8', '8', 0};
    case 0x61:
        return KeyFace{'9', '9', 0};
    case 0x62:
        return KeyFace{'0', '0', 0};
    case 0x63:
        return KeyFace{',', ',', 0};
    case 0x64:
        return KeyFace{'<', '>', '|'};
    default:
        return KeyFace{0, 0, 0};
    }
}

// The lower-case Latin-1 letter of a usage id, or zero if the key is not a
// letter. The Swedish letters sit outside the a..z run.
constexpr u8 keymap_letter(u8 usage) {
    if (usage >= 0x04 && usage <= 0x1D) {
        return static_cast<u8>('a' + (usage - 0x04));
    }
    switch (usage) {
    case 0x2F:
        return 0xE5;
    case 0x33:
        return 0xF6;
    case 0x34:
        return 0xE4;
    default:
        return 0;
    }
}

// The Swedish layout in Latin-1: the character the key types, a control code
// for the editing keys, or zero for a dead key, an unmapped key or a key that
// only changes state. A letter is upper case when exactly one of Shift and
// Caps Lock is set.
constexpr u8 keymap_se(u8 usage, u8 modifiers, bool caps_lock) {
    const bool shifted = (modifiers & (kModLShift | kModRShift)) != 0;
    const u8 lower = keymap_letter(usage);
    if (lower != 0) {
        return shifted != caps_lock ? static_cast<u8>(lower - 0x20) : lower;
    }
    const KeyFace face = key_face(usage);
    if ((modifiers & kModAltGr) != 0 && face.alt_gr != 0) {
        return face.alt_gr;
    }
    return shifted ? face.shifted : face.normal;
}

}
