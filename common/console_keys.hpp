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

// Output classification for bytes fed through CsiFilter.
enum class ConsoleKey : u8 {
    None,
    Byte,
    RowUp,
    RowDown,
    PageUp,
    PageDown,
    Top,
    Bottom,
    Swallowed,
};

// Filters Amiga CSI sequences (0x9B followed by up to 6 bytes ending at a
// final byte in 0x40..0x7E) out of a byte stream, turning scroll sequences into
// ConsoleKey actions and swallowing other complete or over-long sequences.
class CsiFilter {
public:
    static constexpr usize kMaxSeqBytes = 6;

    ConsoleKey feed(u8 byte, u8& out);
    void reset();

private:
    ConsoleKey match() const;

    bool active_ = false;
    u8 len_ = 0;
    u8 buf_[kMaxSeqBytes]{};
};

inline ConsoleKey CsiFilter::feed(u8 byte, u8& out) {
    if (!active_) {
        if (byte == 0x9B) {
            active_ = true;
            len_ = 0;
            return ConsoleKey::None;
        }
        out = byte;
        return ConsoleKey::Byte;
    }
    if (len_ >= kMaxSeqBytes) {
        reset();
        return ConsoleKey::Swallowed;
    }
    buf_[len_++] = byte;
    if (byte >= 0x40 && byte <= 0x7E) {
        const ConsoleKey key = match();
        reset();
        return key;
    }
    return ConsoleKey::None;
}

inline void CsiFilter::reset() {
    active_ = false;
    len_ = 0;
}

inline ConsoleKey CsiFilter::match() const {
    if (len_ == 1) {
        if (buf_[0] == 'T') {
            return ConsoleKey::RowUp;
        }
        if (buf_[0] == 'S') {
            return ConsoleKey::RowDown;
        }
        return ConsoleKey::Swallowed;
    }
    if (len_ == 3 && buf_[2] == '~') {
        if (buf_[0] == '4' && buf_[1] == '1') {
            return ConsoleKey::PageUp;
        }
        if (buf_[0] == '4' && buf_[1] == '2') {
            return ConsoleKey::PageDown;
        }
        if (buf_[0] == '5' && buf_[1] == '4') {
            return ConsoleKey::Top;
        }
        if (buf_[0] == '5' && buf_[1] == '5') {
            return ConsoleKey::Bottom;
        }
    }
    return ConsoleKey::Swallowed;
}

}
