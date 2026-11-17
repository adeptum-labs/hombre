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

// Text that comes off a disk is not trusted, so before it reaches the serial
// console every byte outside printable ASCII (0x20 to 0x7E) is replaced by a
// dot. That keeps control characters, escape sequences and line breaks out of
// the log, where a name could otherwise forge a line.
constexpr char printable_char(u8 c) {
    return c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
}

// Copies the text at `in` into `out` through printable_char. The copy stops
// at a NUL, after `in_max` bytes, or when only the terminator would fit in
// `out`, and `out` is always terminated. Returns the number of characters
// copied. Does nothing when `out_cap` is 0.
inline usize sanitize_text(char* out, usize out_cap, const char* in, usize in_max) {
    if (out == nullptr || out_cap == 0) {
        return 0;
    }
    usize n = 0;
    while (n + 1 < out_cap && n < in_max && in[n] != '\0') {
        out[n] = printable_char(static_cast<u8>(in[n]));
        ++n;
    }
    out[n] = '\0';
    return n;
}

}
