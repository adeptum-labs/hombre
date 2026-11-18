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

#include "format.hpp"
#include "port_io.hpp"
#include "types.hpp"

namespace hombre::serial {

constexpr u16 com1 = 0x3F8;
constexpr u8 line_status_transmit_empty = 0x20;

// Receives every character written to the port, when set.
inline void (*mirror)(char) = nullptr;

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
    if (mirror != nullptr) {
        mirror(c);
    }
}

inline void write(const char* text) {
    for (; *text; ++text) {
        write_char(*text);
    }
}

// Both writers print through format_u64: lowercase hex without padding and
// without a "0x" prefix (callers print any prefix), or plain decimal.
inline void write_number(u64 value, unsigned base) {
    char text[21];  // 20 decimal digits for u64 max, plus the terminator
    if (format_u64(text, sizeof(text), value, base) != 0) {
        write(text);
    }
}

inline void write_hex(u64 value) {
    write_number(value, 16);
}

inline void write_dec(u64 value) {
    write_number(value, 10);
}

}
