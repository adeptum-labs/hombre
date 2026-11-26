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

#include "../m68k_cpu.hpp"
#include "../types.hpp"
#include "exec.hpp"
#include "input_device.hpp"
#include "native.hpp"
#include "version.hpp"

// keymap.library written in C++ for code run by the 68000 translator.
//
// The library carries the built-in "usa" keymap: init builds its tables and
// a KeyMapNode named "usa" in the list of a KeyMapResource named
// "keymap.resource", which it adds to exec's resource list, so that
// OpenResource("keymap.resource") finds the keymap without the disk
// KEYMAPS: directory. AskKeyMapDefault and SetKeyMapDefault read and write
// the KeyMap MapRawKey and MapANSI use when their keyMap argument is null.
// MapRawKey maps an InputEvent to the characters its raw key and qualifiers
// type; MapANSI maps an ANSI string back to raw key and qualifier pairs.
//
// Every guest pointer goes through native::Bus, so a bad one ends the run
// with BusError or AddressError.

namespace hombre {

// Library vector offsets of keymap.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace keymap_lvo {

constexpr u32 SetKeyMapDefault = 30;
constexpr u32 AskKeyMapDefault = 36;
constexpr u32 MapRawKey = 42;
constexpr u32 MapANSI = 48;
// The last vector of the 3.1 library; the table reaches down to it.
constexpr u32 Last = 48;

}

// Offsets and values of the structures keymap.library works with, named as
// devices/keymap.h names them.
namespace keymap_ndk {

// struct KeyMap
constexpr u32 km_LoKeyMapTypes = 0;
constexpr u32 km_LoKeyMap = 4;
constexpr u32 km_LoCapsable = 8;
constexpr u32 km_LoRepeatable = 12;
constexpr u32 km_HiKeyMapTypes = 16;
constexpr u32 km_HiKeyMap = 20;
constexpr u32 km_HiCapsable = 24;
constexpr u32 km_HiRepeatable = 28;
constexpr u32 KEYMAP_SIZE = 32;

// struct KeyMapNode
constexpr u32 kn_Node = 0;
constexpr u32 kn_KeyMap = 14;
constexpr u32 KEYMAPNODE_SIZE = 46;

// struct KeyMapResource
constexpr u32 kr_Node = 0;
constexpr u32 kr_List = 14;
constexpr u32 KEYMAPRESOURCE_SIZE = 28;

// The ln_Type of a KeyMapNode.
constexpr u8 NT_KEYMAP = 20;

// Key types, the bits of a km_*KeyMapTypes byte.
constexpr u8 KC_NOQUAL = 0;
constexpr u8 KCF_SHIFT = 1;
constexpr u8 KCF_ALT = 2;
constexpr u8 KCF_CONTROL = 4;
constexpr u8 KC_VANILLA = 7;
constexpr u8 KCF_DOWNUP = 8;
constexpr u8 KCF_DEAD = 0x20;
constexpr u8 KCF_STRING = 0x40;
constexpr u8 KCF_NOP = 0x80;

// The flags of a dead-key descriptor entry, and the part of its value that
// numbers the dead key.
constexpr u8 DPF_MOD = 0x01;
constexpr u8 DPF_DEAD = 0x08;
constexpr u8 DP_2DINDEXMASK = 0x0F;

// struct InputEvent (see input_device.hpp), and the qualifier bits it
// carries. The previous key down codes and qualifiers sit in the
// ie_position union.
constexpr u32 ie_Prev1DownCode = 14;
constexpr u32 ie_Prev1DownQual = 15;
constexpr u16 IEQUALIFIER_LSHIFT = 0x0001;
constexpr u16 IEQUALIFIER_RSHIFT = 0x0002;
constexpr u16 IEQUALIFIER_CAPSLOCK = 0x0004;
constexpr u16 IEQUALIFIER_CONTROL = 0x0008;
constexpr u16 IEQUALIFIER_LALT = 0x0010;
constexpr u16 IEQUALIFIER_RALT = 0x0020;

}

class KeymapLibrary {
public:
    // Builds the "usa" KeyMap and its KeyMapResource, adds the resource to
    // exec, and makes keymap.library with the keymap vectors. exec must be
    // initialised. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec);

    u32 base() const { return base_; }

    // The KeyMap of the "usa" KeyMapNode, which the library starts with.
    u32 keymap() const { return keymap_; }

    // The KeyMap MapRawKey and MapANSI use when their keyMap argument is
    // null. The library starts with keymap(); SetKeyMapDefault and
    // console.device change it.
    u32 default_keymap() const { return default_; }
    void set_default_keymap(u32 keymap) { default_ = keymap; }

    // The characters the raw key code types under the qualifier in the
    // default keymap, as MapRawKey would give them, copied to out. 0 for a
    // key that types nothing, a key-up code, or more than max characters;
    // max is at most kScratchBytes. The library keeps the key that went down
    // before, so a dead key followed by a letter types the accented letter.
    u32 map_vanilla(u16 code, u16 qualifier, u8* out, u32 max);

private:
    friend struct KeymapNatives;
    friend struct ConsoleOps;

    static constexpr u32 kVectors = keymap_lvo::Last / 6;
    // The guest buffer map_vanilla has map_code write the characters to.
    static constexpr u32 kScratchBytes = 8;

    // A raw key code and the qualifier it went down with.
    struct KeyDown {
        u32 code = 0;
        u32 qualifier = 0;
    };

    u32 alloc_string(const char* text);
    u32 map_raw(native::Bus& b, u32 event, u32 buffer, u32 length, u32 keymap);
    u32 map_code(native::Bus& b, KeyDown key, KeyDown previous, u32 buffer, u32 length,
                 u32 keymap);
    u32 map_ansi(native::Bus& b, u32 string, u32 count, u32 buffer, u32 length, u32 keymap);
    bool find_ansi(native::Bus& b, u32 keymap, u32 ch, u32& code, u32& qualifier);

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    u32 base_ = 0;
    u32 resource_ = 0;
    u32 keymap_node_ = 0;
    u32 keymap_ = 0;
    u32 default_ = 0;
    u32 scratch_ = 0;
    KeyDown last_down_{};
    NativeVector vectors_[kVectors]{};
};

}
