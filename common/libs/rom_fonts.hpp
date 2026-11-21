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

#include "../types.hpp"

// The bitmap fonts kept in the ROM beside the built-in topaz, as
// tools/mkfont.py writes them. graphics.library puts them in its list of
// fonts in memory, so they can be opened whatever disks are in the drives.

namespace hombre {

// One size of a font. Its glyphs are cells x_size bits wide, side by side
// in the one bit plane at data, modulo bytes a row: a cell for every
// character from lo_char to hi_char, then the one drawn for all others.
struct RomFont {
    const char* name;
    u32 y_size;
    u32 x_size;
    u32 baseline;
    u32 bold_smear;
    u32 lo_char;
    u32 hi_char;
    u32 modulo;
    const u8* data;
};

u32 rom_font_count();

// The font at index, counted from 0. False past the last one.
bool rom_font(u32 index, RomFont& out);

}
