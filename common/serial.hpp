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

}
