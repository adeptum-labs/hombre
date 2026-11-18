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

// Decodes HID boot keyboard reports. A report holds the modifiers in byte 0,
// a reserved byte and six key slots; feed appends the character of every key
// that is in the new report but was not in the one before, in slot order.
// Caps Lock is state rather than a character, so it is tracked here and
// handed to keymap_se. The rollover error report, in which every slot is
// 0x01, is ignored without disturbing that state.
class KeyDecoder {
public:
    static constexpr usize kReportBytes = 8;
    static constexpr usize kKeySlots = 6;

    // Appends the new characters to out and returns how many were written.
    // Nothing is written past capacity; characters left out are gone.
    usize feed(const u8 report[kReportBytes], u8* out, usize capacity);

    // Forgets the keys held and turns Caps Lock off.
    void reset();

private:
    static bool contains(const u8* slots, usize count, u8 usage);

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
        const u8 c = keymap_se(usage, report[0], caps_lock_);
        if (c != 0 && out != nullptr && written < capacity) {
            out[written++] = c;
        }
    }
    for (usize i = 0; i < kKeySlots; ++i) {
        held_[i] = slots[i];
    }
    return written;
}

inline void KeyDecoder::reset() {
    caps_lock_ = false;
    for (usize i = 0; i < kKeySlots; ++i) {
        held_[i] = 0;
    }
}

}
