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
#include "libs/gfx.hpp"

// The AmigaShell console: a window with a character grid, drawn and
// scrolled through the Graphics C++ methods. write takes console output,
// write_prompt prints the CLI prompt and leaves a block cursor after it.
// The window is opened on the front screen.

namespace hombre {

class Exec;
class Graphics;
class Intuition;

class ShellWindow {
public:
    static constexpr u32 kMaxPrompt = 64;

    // Opens the "AmigaShell" window on display.front_screen. False if
    // there is no front screen or the window cannot be opened.
    bool open(m68k::Machine& m, Exec& exec, Graphics& gfx, Intuition& intuition,
              DisplayState& display);

    void write(const u8* data, u32 bytes);
    void write_prompt(u32 cli_number, const char* volume);
    bool is_open() const;
    void close();

    // The ConsoleWrite sink of dos.hpp: writes the bytes to the window.
    static void sink(void* ctx, const u8* data, u32 bytes);

private:
    void put_byte(u8 c);
    void advance_row();
    void draw_cell(u8 c);
    void draw_cursor();

    Intuition* intuition_ = nullptr;
    Graphics* graphics_ = nullptr;
    u32 window_ = 0;
    u32 rp_ = 0;
    i32 origin_x_ = 0;
    i32 origin_y_ = 0;
    i32 inner_w_ = 0;
    i32 inner_h_ = 0;
    u32 columns_ = 0;
    u32 rows_ = 0;
    u32 col_ = 0;
    u32 row_ = 0;
};

}
