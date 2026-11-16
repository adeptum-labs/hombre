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

#include "port_io.hpp"
#include "types.hpp"

namespace hombre::serial {

constexpr u16 com1 = 0x3F8;
constexpr u8 line_status_transmit_empty = 0x20;

inline void init() {
    outb(com1 + 1, 0x00);  // no interrupts
    outb(com1 + 3, 0x80);  // unlock the baud rate divisor
    outb(com1 + 0, 0x01);  // divisor 1: 115200 baud
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x03);  // 8 data bits, no parity, 1 stop bit
    outb(com1 + 2, 0xC7);  // enable and clear the FIFOs
    outb(com1 + 4, 0x0B);  // DTR, RTS and OUT2
}

inline void write_char(char c) {
    while (!(inb(com1 + 5) & line_status_transmit_empty)) {
    }
    outb(com1, static_cast<u8>(c));
}

inline void write(const char* text) {
    for (; *text; ++text) {
        write_char(*text);
    }
}

// Lowercase, unpadded and without a "0x" prefix; callers print any prefix.
inline void write_hex(u64 value) {
    char digits[16];
    int count = 0;
    do {
        digits[count++] = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    } while (value);
    while (count) {
        write_char(digits[--count]);
    }
}

inline void write_dec(u64 value) {
    char digits[20];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) {
        write_char(digits[--count]);
    }
}

}
