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

#include "hid_rawkey.hpp"
#include "keymap_se.hpp"
#include "types.hpp"

namespace hombre {

// Decodes HID boot keyboard reports. A report holds the modifiers in byte 0,
// a reserved byte and six key slots; feed appends the bytes of every key that
// is in the new report but was not in the one before, in slot order. Editing
// and cursor keys emit Amiga CSI sequences (0x9B ...), and Caps Lock is tracked
// as state and handed to keymap_se. The rollover error report, in which every
// slot is 0x01, is ignored without disturbing that state.
class KeyDecoder {
public:
    static constexpr usize kReportBytes = 8;
    static constexpr usize kKeySlots = 6;
    static constexpr usize kMaxBytesPerKey = 4;

    // Appends the new bytes to out and returns how many were written. Nothing is
    // written past capacity; keys whose sequence does not fit are dropped.
    usize feed(const u8 report[kReportBytes], u8* out, usize capacity);

    // Forgets the keys held and turns Caps Lock off.
    void reset();

    // Forgets the keys held and keeps Caps Lock: for when reports were read
    // elsewhere, so the decoder no longer knows what is down.
    void release_all();

    // The bytes a press of the key at usage types under the modifier byte
    // and Caps Lock, as feed writes them; returns how many.
    static usize decode(u8 usage, u8 modifiers, bool caps_lock, u8 out[kMaxBytesPerKey]) {
        return decode_key(usage, modifiers, caps_lock, out);
    }

private:
    static bool contains(const u8* slots, usize count, u8 usage);
    static usize write_csi(u8* out, u8 b0, u8 b1 = 0, u8 b2 = 0);
    static usize decode_key(u8 usage, u8 modifiers, bool caps_lock, u8 out[kMaxBytesPerKey]);

    bool caps_lock_ = false;
    u8 held_[kKeySlots] = {};
};

inline bool KeyDecoder::contains(const u8* slots, usize count, u8 usage) {
    for (usize i = 0; i < count; ++i) {
        if (slots[i] == usage) {
            return true;
        }
    }
    return false;
}

inline usize KeyDecoder::write_csi(u8* out, u8 b0, u8 b1, u8 b2) {
    out[0] = 0x9B;
    out[1] = b0;
    if (b1 == 0) {
        return 2;
    }
    out[2] = b1;
    out[3] = b2;
    return 4;
}

inline usize KeyDecoder::decode_key(u8 usage, u8 modifiers, bool caps_lock,
                                    u8 out[kMaxBytesPerKey]) {
    const bool shifted = (modifiers & (kModLShift | kModRShift)) != 0;
    switch (usage) {
    case 0x4A:
        return write_csi(out, shifted ? '5' : '4', '4', '~');
    case 0x4B:
        return write_csi(out, '4', '1', '~');
    case 0x4D:
        return write_csi(out, shifted ? '5' : '4', '5', '~');
    case 0x4E:
        return write_csi(out, '4', '2', '~');
    case 0x4F:
        return write_csi(out, 'C');
    case 0x50:
        return write_csi(out, 'D');
    case 0x51:
        return write_csi(out, shifted ? 'S' : 'B');
    case 0x52:
        return write_csi(out, shifted ? 'T' : 'A');
    default: {
        const u8 c = keymap_se(usage, modifiers, caps_lock);
        if (c == 0) {
            return 0;
        }
        out[0] = c;
        return 1;
    }
    }
}

inline usize KeyDecoder::feed(const u8 report[kReportBytes], u8* out, usize capacity) {
    const u8* slots = report + 2;
    bool rollover = true;
    for (usize i = 0; i < kKeySlots; ++i) {
        rollover = rollover && slots[i] == 1;
    }
    if (rollover) {
        return 0;
    }
    usize written = 0;
    for (usize i = 0; i < kKeySlots; ++i) {
        const u8 usage = slots[i];
        if (usage == 0 || contains(held_, kKeySlots, usage) ||
            contains(slots, i, usage)) {
            continue;
        }
        if (usage == kUsageCapsLock) {
            caps_lock_ = !caps_lock_;
            continue;
        }
        u8 seq[kMaxBytesPerKey]{};
        const usize len = decode_key(usage, report[0], caps_lock_, seq);
        if (len != 0 && out != nullptr && len <= capacity && written <= capacity - len) {
            for (usize j = 0; j < len; ++j) {
                out[written++] = seq[j];
            }
        }
    }
    for (usize i = 0; i < kKeySlots; ++i) {
        held_[i] = slots[i];
    }
    return written;
}

inline void KeyDecoder::reset() {
    caps_lock_ = false;
    release_all();
}

inline void KeyDecoder::release_all() {
    for (usize i = 0; i < kKeySlots; ++i) {
        held_[i] = 0;
    }
}

// The bytes the key press of a raw key event types, as KeyDecoder writes them
// for the same key under the same qualifier; nothing for a release or a key
// that types none.
inline usize raw_key_bytes(u8 code, u16 qualifier, u8 out[KeyDecoder::kMaxBytesPerKey]) {
    if ((code & kAmigaKeyUp) != 0) {
        return 0;
    }
    const u8 usage = amiga_to_hid(code);
    if (usage == 0 || usage == kUsageCapsLock) {
        return 0;
    }
    return KeyDecoder::decode(usage, hid_modifiers(qualifier), (qualifier & kQualCapsLock) != 0,
                              out);
}

}
