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

#include "keymap_library.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using namespace keymap_ndk;
using namespace ndk;

namespace {

constexpr char kName[] = "keymap.library";
constexpr char kIdString[] = "keymap 40.0 (Hombre)\r\n";
constexpr char kResourceName[] = "keymap.resource";
constexpr char kUsaName[] = "usa";
constexpr char kUsa1Name[] = "usa1";
constexpr u16 kVersion = 40;
constexpr u32 kBlockFlags = MEMF_PUBLIC | MEMF_CLEAR;
constexpr u32 kTooSmall = 0xFFFFFFFF;  // MapRawKey's -1
constexpr u32 kControlMask = ~0x60u;   // Control clears bits 5 and 6
constexpr u32 kLoKeys = 64;
constexpr u32 kHiKeys = 56;
constexpr u8 kKeyUp = 0x80;
constexpr u32 kFirstQualifierKey = 0x60;  // the shift, caps, control and alt keys
constexpr u32 kLastQualifierKey = 0x67;
constexpr u8 kShiftAlt = KCF_SHIFT | KCF_ALT;
constexpr u8 kDeadVanilla = KCF_DEAD | KC_VANILLA;

// A map entry holds the byte of each qualifier state, plain in the low byte,
// then shift, alt and alt+shift.
constexpr u32 mapped(u8 normal, u8 shift, u8 alt, u8 alt_shift) {
    return static_cast<u32>(normal) | (static_cast<u32>(shift) << 8) |
           (static_cast<u32>(alt) << 16) | (static_cast<u32>(alt_shift) << 24);
}

// Alt types the unshifted character, as the punctuation keys do.
constexpr u32 vanilla(u8 normal, u8 shift) { return mapped(normal, shift, normal, shift); }

constexpr u32 letter(u8 lower, u8 alt, u8 alt_shift) {
    return mapped(lower, lower - 0x20, alt, alt_shift);
}

constexpr u32 plain(u8 c) { return c; }

// The entry of a key with one qualifier bit: plain, then qualified.
constexpr u32 qualified(u8 normal, u8 with_qualifier) {
    return mapped(normal, with_qualifier, 0, 0);
}

// The USA keymap's raw keys 0x00-0x3F: the top row, the letter rows, and
// the numeric pad. A zero entry belongs to a key that maps nothing, or to a
// dead-capable key, whose entry init points at its descriptor table.
constexpr u32 kUsaLoMap[kLoKeys] = {
    vanilla('`', '~'), mapped('1', '!', 0xB9, '!'), mapped('2', '@', 0xB2, '@'),
    mapped('3', '#', 0xB3, '#'),
    mapped('4', '$', 0xA2, '$'), mapped('5', '%', 0xBC, '%'), mapped('6', '^', 0xBD, '^'),
    mapped('7', '&', 0xBE, '&'),
    mapped('8', '*', 0xB7, '*'), mapped('9', '(', 0xAB, '('), mapped('0', ')', 0xBB, ')'),
    vanilla('-', '_'),
    vanilla('=', '+'), vanilla('\\', '|'), 0, plain('0'),
    letter('q', 0xE5, 0xC5), letter('w', 0xB0, 0xB0), 0, letter('r', 0xAE, 0xAE),
    letter('t', 0xFE, 0xDE), 0, 0, 0,
    0, letter('p', 0xB6, 0xB6), vanilla('[', '{'), vanilla(']', '}'),
    0, plain('1'), plain('2'), plain('3'),
    0, letter('s', 0xDF, 0xA7), letter('d', 0xF0, 0xD0), 0,
    0, 0, 0, 0,
    letter('l', 0xA3, 0xA3), vanilla(';', ':'), vanilla('\'', '"'), 0,
    0, plain('4'), plain('5'), plain('6'),
    mapped('<', '>', 0xAB, 0xBB), letter('z', 0xB1, 0xAC), letter('x', 0xD7, 0xF7),
    letter('c', 0xE7, 0xC7),
    letter('v', 0xAA, 0xAA), letter('b', 0xBA, 0xBA), 0, letter('m', 0xB8, 0xBF),
    vanilla(',', '<'), vanilla('.', '>'), vanilla('/', '?'), 0,
    plain('.'), plain('7'), plain('8'), plain('9'),
};

constexpr u8 kUsaLoTypes[kLoKeys] = {
    KC_VANILLA, kShiftAlt, KC_VANILLA, kShiftAlt,        // ` 1 2 3
    kShiftAlt, kShiftAlt, KC_VANILLA, kShiftAlt,         // 4 5 6 7
    kShiftAlt, kShiftAlt, kShiftAlt, KC_VANILLA,         // 8 9 0 -
    KCF_SHIFT, KC_VANILLA, KCF_NOP, KC_NOQUAL,           // = \ 0x0E pad 0
    KC_VANILLA, KC_VANILLA, kDeadVanilla, KC_VANILLA,    // q w e r
    KC_VANILLA, kDeadVanilla, kDeadVanilla, kDeadVanilla,  // t y u i
    kDeadVanilla, KC_VANILLA, KC_VANILLA, KC_VANILLA,    // o p [ ]
    KCF_NOP, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,            // 0x1C pad 1 2 3
    kDeadVanilla, KC_VANILLA, KC_VANILLA, kDeadVanilla,  // a s d f
    kDeadVanilla, kDeadVanilla, kDeadVanilla, kDeadVanilla,  // g h j k
    KC_VANILLA, KCF_SHIFT, KCF_SHIFT, KCF_NOP,           // l ; ' 0x2B
    KCF_NOP, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,            // 0x2C pad 4 5 6
    kShiftAlt, KC_VANILLA, KC_VANILLA, KC_VANILLA,       // < z x c
    KC_VANILLA, KC_VANILLA, kDeadVanilla, KC_VANILLA,    // v b n m
    KCF_SHIFT, KCF_SHIFT, KCF_SHIFT, KCF_NOP,            // , . / 0x3B
    KC_NOQUAL, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,          // pad . 7 8 9
};

// Which keys repeat and which Caps Lock shifts, one bit per key, key 0 in
// the low bit of the first byte.
constexpr u8 kUsaLoCapsable[kLoKeys / 8] = {0x00, 0x00, 0xFF, 0x03, 0xFF, 0x01, 0xFE, 0x00};
constexpr u8 kUsaLoRepeatable[kLoKeys / 8] = {0xFF, 0xBF, 0xFF, 0xEF, 0xFF, 0xEF, 0xFF, 0xF7};
constexpr u8 kUsaHiRepeatable[kHiKeys / 8] = {0x47, 0xFF, 0xFF, 0x7F, 0x00, 0x80, 0x00};

// The USA keymap's raw keys 0x40-0x77: Space, the editing keys, the
// cursor keys, the function keys and the rest of the numeric pad. The
// strings and the map entries of the string keys and of Space, a dead-capable
// key, are filled in by init.
constexpr u32 kUsaHiMap[kHiKeys] = {
    0, plain(0x08), plain(0x09), plain(0x0D),                // space bs tab pad enter
    qualified(0x0D, 0x0A), qualified(0x1B, 0x9B), plain(0x7F), 0,  // return esc del
    0, 0, plain('-'), 0,                                     // 0x48 0x49 pad - 0x4B
    0, 0, 0, 0,                                              // cursor strings
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                            // F1-F10 strings
    plain('('), plain(')'), plain('/'), plain('*'), plain('+'),  // pad ( ) / * +
    0,                                                       // help string
    0, 0, 0, 0, 0, 0, 0, 0,                                  // qualifier keys
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // 0x68-0x77
};

constexpr u8 kUsaHiTypes[kHiKeys] = {
    KCF_DEAD | KCF_ALT, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,     // space bs tab pad enter
    KCF_CONTROL, KCF_ALT, KC_NOQUAL, KCF_NOP,                // return esc del 0x47
    KCF_NOP, KCF_NOP, KC_NOQUAL, KCF_NOP,                    // 0x48 0x49 pad - 0x4B
    KCF_STRING | KCF_SHIFT, KCF_STRING | KCF_SHIFT,          // up down
    KCF_STRING | KCF_SHIFT, KCF_STRING | KCF_SHIFT,          // right left
    KCF_STRING | KCF_SHIFT, KCF_STRING | KCF_SHIFT,          // F1 F2
    KCF_STRING | KCF_SHIFT, KCF_STRING | KCF_SHIFT,          // F3 F4
    KCF_STRING | KCF_SHIFT, KCF_STRING | KCF_SHIFT,          // F5 F6
    KCF_STRING | KCF_SHIFT, KCF_STRING | KCF_SHIFT,          // F7 F8
    KCF_STRING | KCF_SHIFT, KCF_STRING | KCF_SHIFT,          // F9 F10
    KC_NOQUAL, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,   // pad ( ) / * +
    KCF_STRING,                                              // help
    KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP,                      // shifts, caps
    KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP,                      // controls, alts
    KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP,    // 0x68-0x6D
    KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP,    // 0x6E-0x73
    KCF_NOP, KCF_NOP, KCF_NOP, KCF_NOP,                      // 0x74-0x77
};

// A string key's text for the unqualified and the shifted state.
struct UsaString {
    u8 code;
    const char* normal;
    const char* shifted;
};

// The cursor and function keys send ANSI CSI sequences; the numeric pad
// keys do not shift.
constexpr UsaString kUsaStrings[] = {
    {0x4C, "\x9B" "A", "\x9B" "T"},    // up
    {0x4D, "\x9B" "B", "\x9B" "S"},    // down
    {0x4E, "\x9B" "C", "\x9B" " @"},   // right
    {0x4F, "\x9B" "D", "\x9B" " A"},   // left
    {0x50, "\x9B" "0~", "\x9B" "10~"},  // F1
    {0x51, "\x9B" "1~", "\x9B" "11~"},  // F2
    {0x52, "\x9B" "2~", "\x9B" "12~"},  // F3
    {0x53, "\x9B" "3~", "\x9B" "13~"},  // F4
    {0x54, "\x9B" "4~", "\x9B" "14~"},  // F5
    {0x55, "\x9B" "5~", "\x9B" "15~"},  // F6
    {0x56, "\x9B" "6~", "\x9B" "16~"},  // F7
    {0x57, "\x9B" "7~", "\x9B" "17~"},  // F8
    {0x58, "\x9B" "8~", "\x9B" "18~"},  // F9
    {0x59, "\x9B" "9~", "\x9B" "19~"},  // F10
    {0x5F, "\x9B" "?~", nullptr},       // help
};

// The descriptor tables of the dead-capable keys: a (flag, value) pair for
// each combination of the shift, alt and control bits of the key's type, in
// the order none, shift, alt, shift+alt, ctrl and so on. A plain pair has
// flag 0 and the character as value. A DPF_MOD pair has the offset of its
// translation table, which follows the pairs and holds the character
// typed after no dead key, then after dead key 1 to 5. A DPF_DEAD pair has
// the dead key number as value and types nothing. Alt-f, g, h, j and k are
// the dead keys acute, grave, circumflex, tilde and diaeresis.
constexpr u8 kDeadE[] = {
    DPF_MOD, 16, DPF_MOD, 22, 0, 0xA9, 0, 0xA9, 0, 0x05, 0, 0x05, 0, 0x85, 0, 0x85,
    'e', 0xE9, 0xE8, 0xEA, 'e', 0xEB,
    'E', 0xC9, 0xC8, 0xCA, 'E', 0xCB,
};
constexpr u8 kDeadY[] = {
    DPF_MOD, 16, DPF_MOD, 22, 0, 0xA4, 0, 0xA5, 0, 0x19, 0, 0x19, 0, 0x99, 0, 0x99,
    'y', 0xFD, 'y', 'y', 'y', 0xFF,
    'Y', 0xDD, 'Y', 'Y', 'Y', 'Y',
};
constexpr u8 kDeadU[] = {
    DPF_MOD, 16, DPF_MOD, 22, 0, 0xB5, 0, 0xB5, 0, 0x15, 0, 0x15, 0, 0x95, 0, 0x95,
    'u', 0xFA, 0xF9, 0xFB, 'u', 0xFC,
    'U', 0xDA, 0xD9, 0xDB, 'U', 0xDC,
};
constexpr u8 kDeadI[] = {
    DPF_MOD, 16, DPF_MOD, 22, 0, 0xA1, 0, 0xA6, 0, 0x09, 0, 0x09, 0, 0x89, 0, 0x89,
    'i', 0xED, 0xEC, 0xEE, 'i', 0xEF,
    'I', 0xCD, 0xCC, 0xCE, 'I', 0xCF,
};
constexpr u8 kDeadO[] = {
    DPF_MOD, 16, DPF_MOD, 22, 0, 0xF8, 0, 0xD8, 0, 0x0F, 0, 0x0F, 0, 0x8F, 0, 0x8F,
    'o', 0xF3, 0xF2, 0xF4, 0xF5, 0xF6,
    'O', 0xD3, 0xD2, 0xD4, 0xD5, 0xD6,
};
constexpr u8 kDeadA[] = {
    DPF_MOD, 16, DPF_MOD, 22, 0, 0xE6, 0, 0xC6, 0, 0x01, 0, 0x01, 0, 0x81, 0, 0x81,
    'a', 0xE1, 0xE0, 0xE2, 0xE3, 0xE4,
    'A', 0xC1, 0xC0, 0xC2, 0xC3, 0xC4,
};
constexpr u8 kDeadN[] = {
    DPF_MOD, 16, DPF_MOD, 22, 0, 0xAD, 0, 0xAF, 0, 0x0E, 0, 0x0E, 0, 0x8E, 0, 0x8E,
    'n', 'n', 'n', 'n', 0xF1, 'n',
    'N', 'N', 'N', 'N', 0xD1, 'N',
};
constexpr u8 kDeadF[] = {
    0, 'f', 0, 'F', DPF_DEAD, 1, DPF_DEAD, 1, 0, 0x06, 0, 0x06, 0, 0x86, 0, 0x86,
};
constexpr u8 kDeadG[] = {
    0, 'g', 0, 'G', DPF_DEAD, 2, DPF_DEAD, 2, 0, 0x07, 0, 0x07, 0, 0x87, 0, 0x87,
};
constexpr u8 kDeadH[] = {
    0, 'h', 0, 'H', DPF_DEAD, 3, DPF_DEAD, 3, 0, 0x08, 0, 0x08, 0, 0x88, 0, 0x88,
};
constexpr u8 kDeadJ[] = {
    0, 'j', 0, 'J', DPF_DEAD, 4, DPF_DEAD, 4, 0, 0x0A, 0, 0x0A, 0, 0x8A, 0, 0x8A,
};
constexpr u8 kDeadK[] = {
    0, 'k', 0, 'K', DPF_DEAD, 5, DPF_DEAD, 5, 0, 0x0B, 0, 0x0B, 0, 0x8B, 0, 0x8B,
};
// Space has only the none and alt combinations; alt types a no-break space.
constexpr u8 kDeadSpace[] = {DPF_MOD, 4, 0, 0xA0, ' ', 0xB4, '`', '^', '~', 0xA8};

struct UsaDead {
    u8 code;
    const u8* bytes;
    u32 size;
};

constexpr UsaDead kUsaDeadKeys[] = {
    {0x12, kDeadE, sizeof kDeadE},   {0x15, kDeadY, sizeof kDeadY},
    {0x16, kDeadU, sizeof kDeadU},   {0x17, kDeadI, sizeof kDeadI},
    {0x18, kDeadO, sizeof kDeadO},   {0x20, kDeadA, sizeof kDeadA},
    {0x23, kDeadF, sizeof kDeadF},   {0x24, kDeadG, sizeof kDeadG},
    {0x25, kDeadH, sizeof kDeadH},   {0x26, kDeadJ, sizeof kDeadJ},
    {0x27, kDeadK, sizeof kDeadK},   {0x36, kDeadN, sizeof kDeadN},
    {0x40, kDeadSpace, sizeof kDeadSpace},
};

u32 length_of(const char* text) {
    u32 n = 0;
    while (text[n] != '\0') {
        ++n;
    }
    return n;
}

void put_text(Bus& b, u32 at, const char* text) {
    for (u32 i = 0; text[i] != '\0'; ++i) {
        b.w8(at + i, static_cast<u8>(text[i]));
    }
}

void put_u32s(Bus& b, u32 at, const u32* values, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        b.w32(at + i * 4, values[i]);
    }
}

void put_bytes(Bus& b, u32 at, const u8* bytes, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        b.w8(at + i, bytes[i]);
    }
}

// Where a raw key sits in a keymap. The type is KCF_NOP for a key that has
// no entry: a key-up code, a code past the last key, or a bad keymap.
struct KeyRef {
    u32 type = KCF_NOP;
    u32 entry = 0;
    bool capsable = false;
};

KeyRef locate_key(Bus& b, u32 keymap, u32 code) {
    if ((code & kKeyUp) != 0 || code >= kLoKeys + kHiKeys) {
        return {};
    }
    const bool hi = code >= kLoKeys;
    const u32 index = hi ? code - kLoKeys : code;
    const u32 types = b.r32(keymap + (hi ? km_HiKeyMapTypes : km_LoKeyMapTypes));
    const u32 map = b.r32(keymap + (hi ? km_HiKeyMap : km_LoKeyMap));
    const u32 caps_table = b.r32(keymap + (hi ? km_HiCapsable : km_LoCapsable));
    if (!b.ok() || types == 0 || map == 0) {
        return {};
    }
    KeyRef key;
    key.type = b.r8(types + index);
    key.entry = b.r32(map + index * 4);
    key.capsable = caps_table != 0 && (b.r8(caps_table + index / 8) & (1u << (index % 8))) != 0;
    return b.ok() ? key : KeyRef{};
}

// The position of the entry a key of the type has for the qualifier: one per
// combination of the shift, alt and control bits the type has, in that
// order. Caps Lock swaps the shift state of a capsable key.
u32 pair_index(u32 type, u32 qualifier, bool capsable) {
    const bool shift = (qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
    const bool caps = capsable && (qualifier & IEQUALIFIER_CAPSLOCK) != 0;
    const bool states[] = {shift != caps, (qualifier & (IEQUALIFIER_LALT | IEQUALIFIER_RALT)) != 0,
                           (qualifier & IEQUALIFIER_CONTROL) != 0};
    const u32 bits[] = {KCF_SHIFT, KCF_ALT, KCF_CONTROL};
    u32 index = 0;
    u32 weight = 1;
    for (u32 i = 0; i < 3; ++i) {
        if ((type & bits[i]) != 0) {
            if (states[i]) {
                index |= weight;
            }
            weight <<= 1;
        }
    }
    return index;
}

// The number of the dead key the key down makes, 0 if it makes none.
u32 dead_number(Bus& b, u32 keymap, u32 code, u32 qualifier) {
    const KeyRef key = locate_key(b, keymap, code);
    if ((key.type & KCF_DEAD) == 0 || key.entry == 0) {
        return 0;
    }
    const u32 at = key.entry + pair_index(key.type, qualifier, key.capsable) * 2;
    return (b.r8(at) & DPF_DEAD) != 0 ? b.r8(at + 1) & DP_2DINDEXMASK : 0;
}

constexpr u32 kNoCharacter = 0xFFFFFFFF;  // a dead key types nothing
constexpr u32 kDeadKeyCount = 5;

// The character a dead-capable key types, after the key down before it.
u32 dead_capable_character(Bus& b, u32 keymap, const KeyRef& key, u32 qualifier,
                           u32 previous_code, u32 previous_qualifier) {
    if (key.entry == 0) {
        return kNoCharacter;
    }
    const u32 at = key.entry + pair_index(key.type, qualifier, key.capsable) * 2;
    const u32 flag = b.r8(at);
    const u32 value = b.r8(at + 1);
    if ((flag & DPF_DEAD) != 0) {
        return kNoCharacter;
    }
    if ((flag & DPF_MOD) == 0) {
        return value;
    }
    // TODO: a dead key typed after a dead key (DP_2DFACMASK) is not combined.
    const u32 dead = dead_number(b, keymap, previous_code, previous_qualifier);
    return b.r8(key.entry + value + (dead <= kDeadKeyCount ? dead : 0));
}

// The character a key types plain or shifted with no dead key before it, or
// kNoCharacter if the key has no such state or makes a dead key.
u32 ansi_character(Bus& b, u32 type, u32 entry, bool shifted) {
    if (shifted && (type & KCF_SHIFT) == 0) {
        return kNoCharacter;
    }
    if ((type & KCF_DEAD) == 0) {
        return (entry >> (shifted ? 8 : 0)) & 0xFF;
    }
    if (entry == 0) {
        return kNoCharacter;
    }
    const u32 at = entry + (shifted ? 2 : 0);
    const u32 flag = b.r8(at);
    const u32 value = b.r8(at + 1);
    if ((flag & DPF_DEAD) != 0) {
        return kNoCharacter;
    }
    return (flag & DPF_MOD) != 0 ? b.r8(entry + value) : value;
}

}

struct KeymapNatives {
    static void set_default(void* ctx, Machine& m) {
        static_cast<KeymapLibrary*>(ctx)->set_default_keymap(m.cpu.a[0]);
    }

    static void ask_default(void* ctx, Machine& m) {
        native::set_result(m, static_cast<KeymapLibrary*>(ctx)->default_keymap());
    }

    static void map_raw_key(void* ctx, Machine& m) {
        KeymapLibrary& k = *static_cast<KeymapLibrary*>(ctx);
        Bus b(m);
        native::set_result(m, k.map_raw(b, m.cpu.a[0], m.cpu.a[1], m.cpu.d[1], m.cpu.a[2]));
    }

    static void map_ansi(void* ctx, Machine& m) {
        KeymapLibrary& k = *static_cast<KeymapLibrary*>(ctx);
        Bus b(m);
        native::set_result(m, k.map_ansi(b, m.cpu.a[0], m.cpu.d[0], m.cpu.a[1], m.cpu.d[1],
                                          m.cpu.a[2]));
    }
};

namespace {

struct KeymapFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr KeymapFunction kKeymapFunctions[] = {
    {keymap_lvo::SetKeyMapDefault, KeymapNatives::set_default, "SetKeyMapDefault"},
    {keymap_lvo::AskKeyMapDefault, KeymapNatives::ask_default, "AskKeyMapDefault"},
    {keymap_lvo::MapRawKey, KeymapNatives::map_raw_key, "MapRawKey"},
    {keymap_lvo::MapANSI, KeymapNatives::map_ansi, "MapANSI"},
};

}

bool KeymapLibrary::init(Machine& m, Exec& exec) {
    base_ = 0;
    machine_ = &m;
    exec_ = &exec;
    resource_ = 0;
    keymap_node_ = 0;
    keymap_ = 0;
    default_ = 0;
    last_down_ = KeyDown{};
    scratch_ = exec.alloc(kScratchBytes, kBlockFlags);
    if (scratch_ == 0) {
        return false;
    }
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const KeymapFunction& f : kKeymapFunctions) {
        vectors_[f.lvo / LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }

    const u32 lo_types = exec.alloc(sizeof kUsaLoTypes, kBlockFlags);
    const u32 lo_map = exec.alloc(sizeof kUsaLoMap, kBlockFlags);
    const u32 lo_caps = exec.alloc(kLoKeys / 8, kBlockFlags);
    const u32 lo_repeat = exec.alloc(kLoKeys / 8, kBlockFlags);
    const u32 hi_types = exec.alloc(sizeof kUsaHiTypes, kBlockFlags);
    const u32 hi_map = exec.alloc(sizeof kUsaHiMap, kBlockFlags);
    const u32 hi_caps = exec.alloc(kHiKeys / 8, kBlockFlags);
    const u32 hi_repeat = exec.alloc(kHiKeys / 8, kBlockFlags);
    const u32 usa_name = alloc_string(kUsaName);
    const u32 resource_name = alloc_string(kResourceName);
    const u32 id_string = alloc_string(kIdString);
    if (lo_types == 0 || lo_map == 0 || lo_caps == 0 || lo_repeat == 0 || hi_types == 0 ||
        hi_map == 0 || hi_caps == 0 || hi_repeat == 0 || usa_name == 0 ||
        resource_name == 0 || id_string == 0) {
        return false;
    }

    Bus b(m);
    put_bytes(b, lo_types, kUsaLoTypes, sizeof kUsaLoTypes);
    put_u32s(b, lo_map, kUsaLoMap, kLoKeys);
    put_bytes(b, hi_types, kUsaHiTypes, sizeof kUsaHiTypes);
    put_u32s(b, hi_map, kUsaHiMap, kHiKeys);
    put_bytes(b, lo_caps, kUsaLoCapsable, sizeof kUsaLoCapsable);
    put_bytes(b, lo_repeat, kUsaLoRepeatable, sizeof kUsaLoRepeatable);
    put_bytes(b, hi_repeat, kUsaHiRepeatable, sizeof kUsaHiRepeatable);
    // No high key is capsable: hi_caps stays as MEMF_CLEAR left it.
    for (const UsaDead& d : kUsaDeadKeys) {
        const u32 table = exec.alloc(d.size, kBlockFlags);
        if (table == 0) {
            return false;
        }
        put_bytes(b, table, d.bytes, d.size);
        b.w32(d.code < kLoKeys ? lo_map + d.code * 4 : hi_map + (d.code - kLoKeys) * 4, table);
    }
    for (const UsaString& s : kUsaStrings) {
        const bool shifted = s.shifted != nullptr;
        const u32 normal_len = length_of(s.normal);
        const u32 shifted_len = shifted ? length_of(s.shifted) : 0;
        const u32 pairs = shifted ? 2 : 1;
        const u32 table = exec.alloc(pairs * 2 + normal_len + shifted_len, kBlockFlags);
        if (table == 0) {
            return false;
        }
        b.w8(table, normal_len);
        b.w8(table + 1, pairs * 2);
        if (shifted) {
            b.w8(table + 2, shifted_len);
            b.w8(table + 3, pairs * 2 + normal_len);
        }
        put_text(b, table + pairs * 2, s.normal);
        if (shifted) {
            put_text(b, table + pairs * 2 + normal_len, s.shifted);
        }
        b.w32(hi_map + (s.code - 0x40) * 4, table);
    }

    keymap_node_ = exec.alloc(KEYMAPNODE_SIZE, kBlockFlags);
    if (keymap_node_ == 0) {
        return false;
    }
    keymap_ = keymap_node_ + kn_KeyMap;
    b.w8(keymap_node_ + LN_TYPE, NT_KEYMAP);
    b.w32(keymap_node_ + LN_NAME, usa_name);
    b.w32(keymap_ + km_LoKeyMapTypes, lo_types);
    b.w32(keymap_ + km_LoKeyMap, lo_map);
    b.w32(keymap_ + km_LoCapsable, lo_caps);
    b.w32(keymap_ + km_LoRepeatable, lo_repeat);
    b.w32(keymap_ + km_HiKeyMapTypes, hi_types);
    b.w32(keymap_ + km_HiKeyMap, hi_map);
    b.w32(keymap_ + km_HiCapsable, hi_caps);
    b.w32(keymap_ + km_HiRepeatable, hi_repeat);

    resource_ = exec.alloc(KEYMAPRESOURCE_SIZE, kBlockFlags);
    if (resource_ == 0) {
        return false;
    }
    const u32 list = resource_ + kr_List;
    b.w8(resource_ + LN_TYPE, NT_RESOURCE);
    b.w32(resource_ + LN_NAME, resource_name);
    b.w32(list + LH_HEAD, list + LH_TAIL);
    b.w32(list + LH_TAIL, 0);
    b.w32(list + LH_TAILPRED, list + LH_HEAD);
    b.w8(list + LH_TYPE, NT_KEYMAP);
    // Input prefs knows the built-in USA keymap as "usa1" (American) and
    // looks for that name first: a second node carries the same tables.
    const u32 usa1_name = alloc_string(kUsa1Name);
    const u32 usa1_node = exec.alloc(KEYMAPNODE_SIZE, kBlockFlags);
    if (usa1_name == 0 || usa1_node == 0) {
        return false;
    }
    b.w8(usa1_node + LN_TYPE, NT_KEYMAP);
    b.w32(usa1_node + LN_NAME, usa1_name);
    for (u32 i = 0; i < KEYMAP_SIZE; i += 4) {
        b.w32(usa1_node + kn_KeyMap + i, b.r32(keymap_ + i));
    }
    b.w32(keymap_node_ + LN_SUCC, usa1_node);
    b.w32(keymap_node_ + LN_PRED, list + LH_HEAD);
    b.w32(usa1_node + LN_SUCC, list + LH_TAIL);
    b.w32(usa1_node + LN_PRED, keymap_node_);
    b.w32(list + LH_HEAD, keymap_node_);
    b.w32(list + LH_TAILPRED, usa1_node);

    const u32 base = exec.make_native_library(kName, kVersion, vectors_, kVectors, this);
    if (base == 0) {
        return false;
    }
    b.w16(base + LIB_REVISION, kHombreLibRevision);
    b.w32(base + LIB_IDSTRING, id_string);
    if (!b.ok()) {
        return false;
    }
    exec.add_resource(resource_);
    default_ = keymap_;
    base_ = base;
    return true;
}

u32 KeymapLibrary::alloc_string(const char* text) {
    const u32 bytes = length_of(text) + 1;
    const u32 at = exec_->alloc(bytes, kBlockFlags);
    if (at == 0) {
        return 0;
    }
    Bus b(*machine_);
    put_text(b, at, text);
    return b.ok() ? at : 0;
}

u32 KeymapLibrary::map_vanilla(u16 code, u16 qualifier, u8* out, u32 max) {
    if (scratch_ == 0 || max > kScratchBytes) {
        return 0;
    }
    Bus b(*machine_);
    const KeyDown key{code, qualifier};
    const u32 count = map_code(b, key, last_down_, scratch_, max, default_);
    // A qualifier key between a dead key and its letter does not break the pair.
    if ((code & kKeyUp) == 0 && (code < kFirstQualifierKey || code > kLastQualifierKey)) {
        last_down_ = key;
    }
    if (!b.ok() || count == 0 || count > max) {
        return 0;
    }
    for (u32 i = 0; i < count; ++i) {
        out[i] = static_cast<u8>(b.r8(scratch_ + i));
    }
    return b.ok() ? count : 0;
}

u32 KeymapLibrary::map_raw(Bus& b, u32 event, u32 buffer, u32 length, u32 keymap) {
    if (event == 0) {
        return 0;
    }
    const u32 qualifier = b.r16(event + ie_Qualifier);
    const u32 code = b.r16(event + ie_Code) & 0xFF;
    const KeyDown previous{b.r8(event + ie_Prev1DownCode), b.r8(event + ie_Prev1DownQual)};
    return b.ok() ? map_code(b, {code, qualifier}, previous, buffer, length, keymap) : 0;
}

u32 KeymapLibrary::map_code(Bus& b, KeyDown key, KeyDown previous, u32 buffer, u32 length,
                            u32 keymap) {
    if (keymap == 0) {
        keymap = default_;
    }
    if (keymap == 0) {
        return 0;
    }
    const u32 qualifier = key.qualifier;
    const KeyRef ref = locate_key(b, keymap, key.code);
    const u32 type = ref.type;
    if (!b.ok() || (type & KCF_NOP) != 0) {
        return 0;
    }
    if ((type & KCF_STRING) != 0) {
        const u32 table = ref.entry;
        if (table == 0) {
            return 0;
        }
        const u32 pair = pair_index(type, qualifier, false);
        const u32 chars = b.r8(table + pair * 2);
        const u32 offset = b.r8(table + pair * 2 + 1);
        if (!b.ok()) {
            return 0;
        }
        if (chars > length) {
            return kTooSmall;
        }
        for (u32 i = 0; i < chars; ++i) {
            b.w8(buffer + i, b.r8(table + offset + i));
        }
        return b.ok() ? chars : 0;
    }
    u32 character;
    if ((type & KCF_DEAD) != 0) {
        character = dead_capable_character(b, keymap, ref, qualifier, previous.code,
                                           previous.qualifier);
        if (!b.ok() || character == kNoCharacter) {
            return 0;
        }
    } else {
        // A vanilla key has four characters, and Control strips the character
        // of the state instead of choosing one; the other types have an
        // entry for each combination of their bits, as the string keys do.
        const bool vanilla_type = (type & KC_VANILLA) == KC_VANILLA;
        const u32 state = pair_index(vanilla_type ? type & ~KCF_CONTROL : type, qualifier,
                                     ref.capsable);
        character = (ref.entry >> (state * 8)) & 0xFF;
        if (vanilla_type && (qualifier & IEQUALIFIER_CONTROL) != 0) {
            character &= kControlMask;
        }
    }
    if (length == 0) {
        return kTooSmall;
    }
    b.w8(buffer, character);
    return b.ok() ? 1 : 0;
}

bool KeymapLibrary::find_ansi(Bus& b, u32 keymap, u32 ch, u32& code, u32& qualifier) {
    for (u32 table = 0; table < 2; ++table) {
        const bool hi = table != 0;
        const u32 types = b.r32(keymap + (hi ? km_HiKeyMapTypes : km_LoKeyMapTypes));
        const u32 map = b.r32(keymap + (hi ? km_HiKeyMap : km_LoKeyMap));
        const u32 count = hi ? kHiKeys : kLoKeys;
        const u32 first = hi ? 0x40 : 0x00;
        if (!b.ok()) {
            return false;
        }
        if (types == 0 || map == 0) {
            continue;
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 type = b.r8(types + i);
            if ((type & (KCF_NOP | KCF_STRING)) != 0) {
                continue;
            }
            const u32 entry = b.r32(map + i * 4);
            if (!b.ok()) {
                return false;
            }
            for (u32 shifted = 0; shifted < 2; ++shifted) {
                if (ansi_character(b, type, entry, shifted != 0) == ch) {
                    code = first + i;
                    qualifier = shifted != 0 ? IEQUALIFIER_LSHIFT : 0;
                    return true;
                }
            }
        }
    }
    return false;
}

u32 KeymapLibrary::map_ansi(Bus& b, u32 string, u32 count, u32 buffer, u32 length, u32 keymap) {
    if (keymap == 0) {
        keymap = default_;
    }
    if (keymap == 0) {
        return 0;
    }
    u32 pairs = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 ch = b.r8(string + i);
        if (!b.ok()) {
            return 0;
        }
        u32 code = 0;
        u32 qualifier = 0;
        if (!find_ansi(b, keymap, ch, code, qualifier)) {
            if (!b.ok()) {
                return 0;
            }
            continue;  // a character the keymap cannot type is skipped
        }
        if ((pairs + 1) * 2 > length) {
            return 0;
        }
        b.w8(buffer + pairs * 2, code);
        b.w8(buffer + pairs * 2 + 1, qualifier);
        ++pairs;
    }
    return b.ok() ? pairs : 0;
}

}
