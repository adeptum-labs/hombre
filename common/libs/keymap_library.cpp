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

// A vanilla entry, normal in the low byte and alt+shift in the high one.
// Alt types the unshifted character, as the letters do without a dead key.
constexpr u32 vanilla(u8 normal, u8 shift) {
    return static_cast<u32>(normal) | (static_cast<u32>(shift) << 8) |
           (static_cast<u32>(normal) << 16) | (static_cast<u32>(shift) << 24);
}

constexpr u32 letter(u8 lower) { return vanilla(lower, lower - 0x20); }

constexpr u32 plain(u8 c) { return c; }

// The USA keymap's raw keys 0x00-0x3F: the top row, the letter rows, and
// the numeric pad. A zero entry belongs to a key that maps nothing.
constexpr u32 kUsaLoMap[kLoKeys] = {
    vanilla('`', '~'), vanilla('1', '!'), vanilla('2', '@'), vanilla('3', '#'),
    vanilla('4', '$'), vanilla('5', '%'), vanilla('6', '^'), vanilla('7', '&'),
    vanilla('8', '*'), vanilla('9', '('), vanilla('0', ')'), vanilla('-', '_'),
    vanilla('=', '+'), vanilla('\\', '|'), 0, plain('0'),
    letter('q'), letter('w'), letter('e'), letter('r'),
    letter('t'), letter('y'), letter('u'), letter('i'),
    letter('o'), letter('p'), vanilla('[', '{'), vanilla(']', '}'),
    0, plain('1'), plain('2'), plain('3'),
    letter('a'), letter('s'), letter('d'), letter('f'),
    letter('g'), letter('h'), letter('j'), letter('k'),
    letter('l'), vanilla(';', ':'), vanilla('\'', '"'), 0,
    0, plain('4'), plain('5'), plain('6'),
    0, letter('z'), letter('x'), letter('c'),
    letter('v'), letter('b'), letter('n'), letter('m'),
    vanilla(',', '<'), vanilla('.', '>'), vanilla('/', '?'), 0,
    plain('.'), plain('7'), plain('8'), plain('9'),
};

constexpr u8 kUsaLoTypes[kLoKeys] = {
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // ` 1 2 3
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // 4 5 6 7
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // 8 9 0 -
    KC_VANILLA, KC_VANILLA, KCF_NOP, KC_NOQUAL,      // = \ 0x0E pad 0
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // q w e r
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // t y u i
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // o p [ ]
    KCF_NOP, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,        // 0x1C pad 1 2 3
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // a s d f
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // g h j k
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KCF_NOP,     // l ; ' 0x2B
    KCF_NOP, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,        // 0x2C pad 4 5 6
    KCF_NOP, KC_VANILLA, KC_VANILLA, KC_VANILLA,     // 0x30 z x c
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KC_VANILLA,  // v b n m
    KC_VANILLA, KC_VANILLA, KC_VANILLA, KCF_NOP,     // , . / 0x3B
    KC_NOQUAL, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,      // pad . 7 8 9
};

// The USA keymap's raw keys 0x40-0x77: Space, the editing keys, the
// cursor keys, the function keys and the rest of the numeric pad. The
// strings and the map entries of the string keys are filled in by init.
constexpr u32 kUsaHiMap[kHiKeys] = {
    plain(' '), plain(0x08), plain(0x09), plain(0x0D),       // space bs tab pad enter
    plain(0x0D), plain(0x1B), plain(0x7F), 0,                // return esc del
    0, 0, plain('-'), 0,                                     // 0x48 0x49 pad - 0x4B
    0, 0, 0, 0,                                              // cursor strings
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                            // F1-F10 strings
    plain('('), plain(')'), plain('/'), plain('*'), plain('+'),  // pad ( ) / * +
    0,                                                       // help string
    0, 0, 0, 0, 0, 0, 0, 0,                                  // qualifier keys
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // 0x68-0x77
};

constexpr u8 kUsaHiTypes[kHiKeys] = {
    KC_NOQUAL, KC_NOQUAL, KC_NOQUAL, KC_NOQUAL,              // space bs tab pad enter
    KC_NOQUAL, KC_NOQUAL, KC_NOQUAL, KCF_NOP,                // return esc del 0x47
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

// The letter keys, whose Caps Lock state swaps their normal and shifted
// characters.
constexpr bool usa_capsable(u32 code) {
    return (code >= 0x10 && code <= 0x19) || (code >= 0x20 && code <= 0x28) ||
           (code >= 0x31 && code <= 0x37);
}

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

// Sets the bit of key in a capsable or repeatable table.
void set_key_bit(Bus& b, u32 table, u32 key) {
    const u32 at = table + key / 8;
    b.w8(at, b.r8(at) | (1u << (key % 8)));
}

}

struct KeymapNatives {
    static void set_default(void* ctx, Machine& m) {
        static_cast<KeymapLibrary*>(ctx)->default_ = m.cpu.a[0];
    }

    static void ask_default(void* ctx, Machine& m) {
        native::set_result(m, static_cast<KeymapLibrary*>(ctx)->default_);
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
    for (u32 code = 0; code < kLoKeys; ++code) {
        if (usa_capsable(code)) {
            set_key_bit(b, lo_caps, code);
        }
        set_key_bit(b, lo_repeat, code);  // every key below 0x4B repeats
    }
    for (u32 key = 0x40; key <= 0x4A; ++key) {
        set_key_bit(b, hi_repeat, key - 0x40);
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

u32 KeymapLibrary::map_raw(Bus& b, u32 event, u32 buffer, u32 length, u32 keymap) {
    if (keymap == 0) {
        keymap = default_;
    }
    if (event == 0 || keymap == 0) {
        return 0;
    }
    const u32 qualifier = b.r16(event + ie_Qualifier);
    const u32 code = b.r16(event + ie_Code) & 0xFF;
    if (!b.ok() || (code & kKeyUp) != 0 || code > 0x77) {
        return 0;  // a key-up code, or no raw key, types nothing
    }
    const bool hi = code >= 0x40;
    const u32 index = hi ? code - 0x40 : code;
    const u32 types = b.r32(keymap + (hi ? km_HiKeyMapTypes : km_LoKeyMapTypes));
    const u32 map = b.r32(keymap + (hi ? km_HiKeyMap : km_LoKeyMap));
    if (!b.ok() || types == 0 || map == 0) {
        return 0;
    }
    const u32 type = b.r8(types + index);
    const bool shift = (qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
    const bool caps = (qualifier & IEQUALIFIER_CAPSLOCK) != 0;
    const bool control = (qualifier & IEQUALIFIER_CONTROL) != 0;
    const bool alt = (qualifier & (IEQUALIFIER_LALT | IEQUALIFIER_RALT)) != 0;
    if (!b.ok() || (type & KCF_NOP) != 0) {
        return 0;
    }
    if ((type & KCF_STRING) != 0) {
        const u32 table = b.r32(map + index * 4);
        if (!b.ok() || table == 0) {
            return 0;
        }
        u32 pair = 0;
        u32 bit = 1;
        if ((type & KCF_SHIFT) != 0) {
            if (shift) {
                pair |= bit;
            }
            bit <<= 1;
        }
        if ((type & KCF_ALT) != 0) {
            if (alt) {
                pair |= bit;
            }
            bit <<= 1;
        }
        if ((type & KCF_CONTROL) != 0 && control) {
            pair |= bit;
        }
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
    const u32 entry = b.r32(map + index * 4);
    const u32 caps_table = b.r32(keymap + (hi ? km_HiCapsable : km_LoCapsable));
    const bool capsable =
        caps_table != 0 && (b.r8(caps_table + index / 8) & (1u << (index % 8))) != 0;
    if (!b.ok()) {
        return 0;
    }
    bool use_shift = shift && (type & KCF_SHIFT) != 0;
    const bool use_alt = alt && (type & KCF_ALT) != 0;
    if (caps && capsable && (type & KCF_SHIFT) != 0) {
        use_shift = !use_shift;
    }
    u32 character = entry;
    if (use_alt && use_shift) {
        character = entry >> 24;
    } else if (use_alt) {
        character = entry >> 16;
    } else if (use_shift) {
        character = entry >> 8;
    }
    character &= 0xFF;
    if (control && (type & KCF_CONTROL) != 0) {
        character &= kControlMask;
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
            if ((type & (KCF_NOP | KCF_STRING | KCF_DEAD)) != 0) {
                continue;
            }
            const u32 entry = b.r32(map + i * 4);
            if (!b.ok()) {
                return false;
            }
            if ((entry & 0xFF) == ch) {
                code = first + i;
                qualifier = 0;
                return true;
            }
            if ((type & KCF_SHIFT) != 0 && ((entry >> 8) & 0xFF) == ch) {
                code = first + i;
                qualifier = IEQUALIFIER_LSHIFT;
                return true;
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
