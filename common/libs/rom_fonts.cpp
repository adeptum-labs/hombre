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

#include "rom_fonts.hpp"

// The fonts tools/mkfont.py writes, read straight from the object file.
asm(".section .rodata\n"
    ".balign 16\n"
    ".global rom_fonts_start\n"
    "rom_fonts_start:\n"
    ".incbin \"assets/fonts/hasklig/hasklig.romfont\"\n"
    ".global rom_fonts_end\n"
    "rom_fonts_end:\n"
    ".previous\n");

extern "C" {
extern const hombre::u8 rom_fonts_start[];
extern const hombre::u8 rom_fonts_end[];
}

namespace hombre {

namespace {

// The file: "HRF1" and the number of fonts, then for each font a header
// of its name, sizes and character range, followed by its glyph data.
constexpr u32 kFileHeaderBytes = 6;
constexpr u32 kNameBytes = 32;
constexpr u32 kFontHeaderBytes = kNameBytes + 12;

u32 le16(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8);
}

}

u32 rom_font_count() {
    const u8* file = rom_fonts_start;
    const usize bytes = static_cast<usize>(rom_fonts_end - rom_fonts_start);
    if (bytes < kFileHeaderBytes || file[0] != 'H' || file[1] != 'R' || file[2] != 'F' ||
        file[3] != '1') {
        return 0;
    }
    return le16(file + 4);
}

bool rom_font(u32 index, RomFont& out) {
    if (index >= rom_font_count()) {
        return false;
    }
    const u8* at = rom_fonts_start + kFileHeaderBytes;
    for (u32 i = 0;; ++i) {
        if (static_cast<usize>(rom_fonts_end - at) < kFontHeaderBytes ||
            at[kNameBytes - 1] != 0) {
            return false;
        }
        const u8* fields = at + kNameBytes;
        out.name = reinterpret_cast<const char*>(at);
        out.y_size = le16(fields);
        out.x_size = le16(fields + 2);
        out.baseline = le16(fields + 4);
        out.bold_smear = le16(fields + 6);
        out.lo_char = fields[8];
        out.hi_char = fields[9];
        out.modulo = le16(fields + 10);
        out.data = at + kFontHeaderBytes;
        const usize data_bytes = static_cast<usize>(out.modulo) * out.y_size;
        if (static_cast<usize>(rom_fonts_end - out.data) < data_bytes ||
            out.hi_char < out.lo_char ||
            (out.hi_char - out.lo_char + 2) * out.x_size > out.modulo * 8) {
            return false;
        }
        if (i == index) {
            return true;
        }
        at = out.data + data_bytes;
    }
}

}
