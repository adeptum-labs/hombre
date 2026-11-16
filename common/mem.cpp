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

// The mem* routines the compilers emit calls to. Neither the loader nor the
// kernel has a C library, so each links its own copy of this file. It is
// built with -fno-builtin (and -fno-tree-loop-distribute-patterns on GCC) so
// the loops below are not turned back into calls to themselves.

#include "types.hpp"

using hombre::u8;
using hombre::usize;

extern "C" {

void* memcpy(void* dest, const void* src, usize count) {
    u8* d = static_cast<u8*>(dest);
    const u8* s = static_cast<const u8*>(src);
    for (usize i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void* memset(void* dest, int value, usize count) {
    u8* d = static_cast<u8*>(dest);
    for (usize i = 0; i < count; ++i) {
        d[i] = static_cast<u8>(value);
    }
    return dest;
}

void* memmove(void* dest, const void* src, usize count) {
    u8* d = static_cast<u8*>(dest);
    const u8* s = static_cast<const u8*>(src);
    if (d < s) {
        for (usize i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        // Copy backwards so an overlapping tail is read before it is written.
        for (usize i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void* a, const void* b, usize count) {
    const u8* x = static_cast<const u8*>(a);
    const u8* y = static_cast<const u8*>(b);
    for (usize i = 0; i < count; ++i) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

}
