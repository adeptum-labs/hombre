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

#include "keymap_se.hpp"
#include "types.hpp"

namespace hombre {

// The Amiga's view of a HID boot keyboard: raw key codes by physical key
// position, as the Keyboard chapter of the RKM lays them out, and the
// qualifier bits of devices/inputevent.h.

inline constexpr u8 kNoAmigaKey = 0xFF;
inline constexpr u8 kAmigaKeyUp = 0x80;
inline constexpr u8 kAmigaCapsLock = 0x62;

inline constexpr u16 kQualLShift = 0x0001;
inline constexpr u16 kQualRShift = 0x0002;
inline constexpr u16 kQualCapsLock = 0x0004;
inline constexpr u16 kQualControl = 0x0008;
inline constexpr u16 kQualLAlt = 0x0010;
inline constexpr u16 kQualRAlt = 0x0020;
inline constexpr u16 kQualLCommand = 0x0040;
inline constexpr u16 kQualRCommand = 0x0080;
inline constexpr u16 kQualNumericPad = 0x0100;

namespace hid_rawkey_detail {

// HID usages 0x04 (A) to 0x1D (Z).
inline constexpr u8 kLetters[26] = {
    0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24, 0x25, 0x17, 0x26, 0x27, 0x28, 0x37,
    0x36, 0x18, 0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11, 0x32, 0x15, 0x31,
};

// HID usages 0x59 (keypad 1) to 0x63 (keypad .).
inline constexpr u8 kKeypadLow = 0x59;
inline constexpr u8 kKeypadHigh = 0x63;
inline constexpr u8 kKeypadKeys[] = {
    0x1D, 0x1E, 0x1F, 0x2D, 0x2E, 0x2F, 0x3D, 0x3E, 0x3F, 0x0F, 0x3C,
};

// The modifier bits of report byte 0, low to high: left Ctrl, Shift, Alt,
// Amiga, then the same on the right.
inline constexpr u8 kModifierCodes[8] = {0x63, 0x60, 0x64, 0x66, 0x63, 0x61, 0x65, 0x67};

}

// The Amiga raw key code of a HID usage, kNoAmigaKey for a key the Amiga
// keyboard has no counterpart to. Insert types Help.
constexpr u8 hid_to_amiga(u8 usage) {
    using namespace hid_rawkey_detail;
    if (usage >= 0x04 && usage <= 0x1D) {
        return kLetters[usage - 0x04];
    }
    if (usage >= 0x1E && usage <= 0x27) {
        return static_cast<u8>(usage - 0x1D);  // 1..9, and 0 is 0x0A
    }
    if (usage >= 0x3A && usage <= 0x43) {
        return static_cast<u8>(0x50 + usage - 0x3A);  // F1..F10
    }
    if (usage >= kKeypadLow && usage <= kKeypadHigh) {
        return kKeypadKeys[usage - kKeypadLow];
    }
    switch (usage) {
    case 0x28: return 0x44;  // Enter
    case 0x29: return 0x45;  // Esc
    case 0x2A: return 0x41;  // Backspace
    case 0x2B: return 0x42;  // Tab
    case 0x2C: return 0x40;  // Space
    case 0x2D: return 0x0B;  // -
    case 0x2E: return 0x0C;  // =
    case 0x2F: return 0x1A;  // [
    case 0x30: return 0x1B;  // ]
    case 0x31: return 0x0D;  // backslash
    case 0x32: return 0x2B;  // non-US #, the ISO key left of Return
    case 0x33: return 0x29;  // ;
    case 0x34: return 0x2A;  // '
    case 0x35: return 0x00;  // `
    case 0x36: return 0x38;  // ,
    case 0x37: return 0x39;  // .
    case 0x38: return 0x3A;  // /
    case kUsageCapsLock: return kAmigaCapsLock;
    case 0x49: return 0x5F;  // Insert, as Help
    case 0x4C: return 0x46;  // Delete
    case 0x4F: return 0x4E;  // right
    case 0x50: return 0x4F;  // left
    case 0x51: return 0x4D;  // down
    case 0x52: return 0x4C;  // up
    case 0x54: return 0x5C;  // keypad /
    case 0x55: return 0x5D;  // keypad *
    case 0x56: return 0x4A;  // keypad -
    case 0x57: return 0x5E;  // keypad +
    case 0x58: return 0x43;  // keypad Enter
    case 0x64: return 0x30;  // non-US backslash, the ISO key left of Z
    default: return kNoAmigaKey;
    }
}

constexpr bool hid_is_keypad(u8 usage) { return usage >= 0x54 && usage <= 0x63; }

// The HID usage whose key has the Amiga raw key code (key-up bit ignored),
// 0 for none: the reverse of hid_to_amiga.
constexpr u8 amiga_to_hid(u8 code) {
    code = static_cast<u8>(code & ~kAmigaKeyUp);
    for (u32 usage = 0x04; usage <= 0x64; ++usage) {
        if (hid_to_amiga(static_cast<u8>(usage)) == code) {
            return static_cast<u8>(usage);
        }
    }
    return 0;
}

// A report's modifier byte that gives the keyboard qualifier bits: the left
// key of a pair, but for Shift and Alt, whose sides the qualifier tells.
constexpr u8 hid_modifiers(u16 qualifier) {
    u8 m = 0;
    m |= (qualifier & kQualLShift) != 0 ? kModLShift : 0;
    m |= (qualifier & kQualRShift) != 0 ? kModRShift : 0;
    m |= (qualifier & kQualControl) != 0 ? kModLCtrl : 0;
    m |= (qualifier & kQualLAlt) != 0 ? kModLAlt : 0;
    m |= (qualifier & kQualRAlt) != 0 ? kModAltGr : 0;
    m |= (qualifier & kQualLCommand) != 0 ? kModLGui : 0;
    m |= (qualifier & kQualRCommand) != 0 ? kModRGui : 0;
    return m;
}

// The qualifier bits of the modifier byte of a report, and of Caps Lock.
constexpr u16 hid_qualifier(u8 modifiers, bool caps_lock) {
    u16 q = 0;
    q |= (modifiers & kModLShift) != 0 ? kQualLShift : 0;
    q |= (modifiers & kModRShift) != 0 ? kQualRShift : 0;
    q |= (modifiers & (kModLCtrl | kModRCtrl)) != 0 ? kQualControl : 0;
    q |= (modifiers & kModLAlt) != 0 ? kQualLAlt : 0;
    q |= (modifiers & kModAltGr) != 0 ? kQualRAlt : 0;
    q |= (modifiers & kModLGui) != 0 ? kQualLCommand : 0;
    q |= (modifiers & kModRGui) != 0 ? kQualRCommand : 0;
    q |= caps_lock ? kQualCapsLock : 0;
    return q;
}

// A raw key event as the Amiga keyboard sends it: the code, with
// kAmigaKeyUp set for a release, and the qualifier in force.
struct RawKeyEvent {
    u8 code;
    u16 qualifier;
};

// Follows a HID boot keyboard report by report and turns what changed into
// Amiga raw key events: releases first, with the qualifier they were held
// under, then modifier changes, each with the qualifier it leaves behind,
// then presses. Caps Lock is a latching key on the Amiga: its press toggles
// the state and sends a key-down when the lock comes on and a key-up when
// it goes off, and its release sends nothing. The rollover error report, in
// which every slot is 0x01, is ignored.
class RawKeyTracker {
public:
    static constexpr usize kReportBytes = 8;
    static constexpr usize kKeySlots = 6;
    static constexpr usize kModifierBits = 8;
    // Every modifier changing, every held key released and every slot pressed.
    static constexpr usize kMaxEvents = kModifierBits + 2 * kKeySlots;

    // Writes the events of the report to out, at most capacity of them, and
    // returns how many.
    usize feed(const u8 report[kReportBytes], RawKeyEvent* out, usize capacity) {
        const u8* slots = report + 2;
        if (is_rollover(slots)) {
            return 0;
        }
        usize count = 0;
        const auto emit = [&](u8 code, bool down, u16 with_qualifier) {
            if (count < capacity) {
                out[count++] = RawKeyEvent{static_cast<u8>(down ? code : code | kAmigaKeyUp),
                                           with_qualifier};
            }
        };
        for (usize i = 0; i < kKeySlots; ++i) {
            const u8 amiga = hid_to_amiga(held_[i]);
            if (held_[i] != kUsageCapsLock && amiga != kNoAmigaKey &&
                !contains(slots, kKeySlots, held_[i])) {
                emit(amiga, false, key_qualifier(held_[i]));
            }
        }
        for (usize bit = 0; bit < kModifierBits; ++bit) {
            const u8 mask = static_cast<u8>(1u << bit);
            if (((report[0] ^ modifiers_) & mask) != 0) {
                modifiers_ ^= mask;
                emit(hid_rawkey_detail::kModifierCodes[bit], (modifiers_ & mask) != 0,
                     qualifier());
            }
        }
        for (usize i = 0; i < kKeySlots; ++i) {
            const u8 usage = slots[i];
            if (usage == 0 || contains(held_, kKeySlots, usage) || contains(slots, i, usage)) {
                continue;
            }
            if (usage == kUsageCapsLock) {
                caps_lock_ = !caps_lock_;
                emit(kAmigaCapsLock, caps_lock_, qualifier());
            } else if (hid_to_amiga(usage) != kNoAmigaKey) {
                emit(hid_to_amiga(usage), true, key_qualifier(usage));
            }
        }
        for (usize i = 0; i < kKeySlots; ++i) {
            held_[i] = slots[i];
        }
        return count;
    }

    // The qualifier in force after the reports fed so far.
    u16 qualifier() const { return hid_qualifier(modifiers_, caps_lock_); }

private:
    static bool is_rollover(const u8* slots) {
        for (usize i = 0; i < kKeySlots; ++i) {
            if (slots[i] != 1) {
                return false;
            }
        }
        return true;
    }

    static bool contains(const u8* slots, usize count, u8 usage) {
        for (usize i = 0; i < count; ++i) {
            if (slots[i] == usage) {
                return true;
            }
        }
        return false;
    }

    u16 key_qualifier(u8 usage) const {
        return static_cast<u16>(qualifier() | (hid_is_keypad(usage) ? kQualNumericPad : 0));
    }

    bool caps_lock_ = false;
    u8 modifiers_ = 0;
    u8 held_[kKeySlots] = {};
};

}
