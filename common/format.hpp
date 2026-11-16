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

// The one number formatter: serial output and loader diagnostics are built on it.
//
// Writes `value` in base 10 or 16 (lowercase, no prefix) to buf as a NUL
// terminated string and returns the number of digits. Returns 0, leaving buf
// alone, for any other base, a null buf, or a capacity too small for the
// digits and the terminator.
inline usize format_u64(char* buf, usize capacity, u64 value, unsigned base) {
    if (buf == nullptr || (base != 10 && base != 16)) {
        return 0;
    }

    char digits[20];  // u64 max is 20 digits in decimal, 16 in hex
    usize count = 0;
    do {
        digits[count++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value != 0);

    if (count >= capacity) {
        return 0;
    }
    for (usize i = 0; i < count; ++i) {
        buf[i] = digits[count - 1 - i];
    }
    buf[count] = '\0';
    return count;
}

}
