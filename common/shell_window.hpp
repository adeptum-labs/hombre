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
#include "shell_text.hpp"

// The HombreShell console: a window with a character grid, drawn and
// scrolled through the Graphics C++ methods. write takes console output,
// write_prompt prints the CLI prompt and leaves a block cursor after it,
// echo draws a typed byte at the cursor, erase_last takes it back and
// print_line writes a message on a line of its own. The window is opened
// on intuition's default screen.

namespace hombre {

class Exec;
class Graphics;
class Intuition;

class ShellWindow {
public:
    static constexpr u32 kMaxPrompt = 64;

    // Opens the "HombreShell" window on intuition's default screen, at left,
    // top and the given size; a top of -1 places it below the screen's title
    // bar and a zero width or height fills the room there is. The window
    // stays open through a Workbench reset: it does not keep CloseWorkBench
    // from succeeding, and is fitted to the screen's new size and depth,
    // its text laid out again. False if there is no default screen or the
    // window cannot be opened.
    bool open(m68k::Machine& m, Exec& exec, Graphics& gfx, Intuition& intuition,
              i32 left = 0, i32 top = -1, u32 width = 0, u32 height = 0);

    void write(const u8* data, u32 bytes);
    void write_prompt(u32 cli_number, const char* volume);
    void write_prompt_text(const char* text);

    // Marks the current cursor position as the start of typed input and
    // draws the block cursor, so erase_last stops here.
    void begin_input();

    // Draws one printable byte at the cursor and moves the block cursor
    // past it, wrapping and scrolling like write.
    void echo(u8 ch);

    // Takes the cursor back one cell and clears the cells it leaves and
    // enters. Does nothing once the cursor is back at the prompt's end.
    void erase_last();

    // Shows the cursor back cells before the end of the typed input, as an
    // inverted cell over the byte there; 0 is the block after the input.
    // Clamped to the start of the input. echo, erase_last and end_line put
    // it back at the end first.
    void set_cursor_back(u32 back);

    // Clears the block cursor and moves to column 0 of the next row.
    void end_line();

    // Writes the text on a row of its own and ends that line.
    void print_line(const char* text);

    bool is_open() const;

    // The Intuition window of the shell, 0 while it is not open.
    u32 window() const { return window_; }
    u32 rows() const { return is_open() ? rows_ : 0; }
    void close();

    // Scrolls view rows back into history (negative moves toward the bottom).
    void scroll_by(i32 rows);
    void scroll_to_top();
    void scroll_to_bottom();
    u32 view_from_bottom() const { return view_; }
    void set_view_listener(void (*fn)(void* ctx, u32 view), void* ctx);

    // The ConsoleWrite sink of dos.hpp: writes the bytes to the window.
    static void sink(void* ctx, const u8* data, u32 bytes);

    // The Intuition WindowHook called after the window is resized.
    static void on_size(void* ctx, u32 window);

    // True once the close gadget was clicked since the window opened.
    bool close_requested() const { return close_requested_; }

    // The Intuition WindowHook called when the close gadget is clicked.
    static void on_close(void* ctx, u32 window);

    // The Intuition GadgetHook called when the scroll bar changes.
    static void on_gadget(void* ctx, u32 gadget, u32 vert_pot);

private:
    u32 max_view() const;
    void apply_user_view(u32 new_view);
    void redraw_viewport();
    void update_knob();
    void notify_view_listener();
    void put_byte(u8 c, bool draw);
    void advance_row();
    void draw_cell(u8 c);
    void draw_cursor();
    void hide_cursor();
    void cursor_to_end();
    bool cursor_cell(u32& row, u32& column) const;
    void draw_cell_at(u32 row, u32 column, u8 c);
    void mark_line_start();

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Intuition* intuition_ = nullptr;
    Graphics* graphics_ = nullptr;
    u32 window_ = 0;
    bool close_requested_ = false;
    u32 rp_ = 0;
    u32 font_ = 0;
    u32 cell_w_ = 8;
    u32 cell_h_ = 8;
    u32 baseline_ = 6;
    u32 gadget_ = 0;
    u32 prop_info_ = 0;
    i32 origin_x_ = 0;
    i32 origin_y_ = 0;
    i32 inner_w_ = 0;
    i32 inner_h_ = 0;
    u32 columns_ = 0;
    u32 rows_ = 0;
    u32 col_ = 0;
    u32 row_ = 0;
    u32 view_ = 0;
    bool cursor_visible_ = false;
    u32 cursor_back_ = 0;
    void (*view_listener_)(void* ctx, u32 view) = nullptr;
    void* view_ctx_ = nullptr;
    ShellText text_{};
};

}
