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

// What the shell line editor drives: echo, erase and end_line update the
// visible line, builtin handles internal commands when non-null (returning
// true when handled), run starts a command, scroll_to_bottom returns the
// viewport to the bottom when a key is handled, and message, prompt and
// log_line receive the shell's replies. run returns false when the command
// cannot be loaded.
struct ShellHost {
    void (*echo)(void* ctx, u8 ch);
    void (*erase)(void* ctx);
    void (*end_line)(void* ctx);
    bool (*run)(void* ctx, const char* path, const char* args);
    void (*message)(void* ctx, const char* text);
    void (*prompt)(void* ctx);
    void (*log_line)(void* ctx, const char* text);
    void* ctx;
    bool (*builtin)(void* ctx, const char* word, const char* args) = nullptr;
    void (*scroll_to_bottom)(void* ctx) = nullptr;
    // Shows the cursor back bytes before the end of the line; without it
    // the cursor stays at the end and Left and Right do nothing.
    void (*cursor_back)(void* ctx, u32 back) = nullptr;
};

// One shell session: a fixed line buffer edited by the keyboard and parsed
// into a command path and its argument text when Enter is pressed. The
// last kHistory lines entered can be brought back into the buffer.
class Shell {
public:
    static constexpr u32 kMaxLine = 512;
    static constexpr u32 kHistory = 16;

    void init(const ShellHost& host);
    void key(u8 ch);

    // Replace the line with the previous or next history entry; going down
    // past the newest one brings back the line that was being typed.
    void history_up();
    void history_down();

    // Move the cursor within the line; typing and Backspace then work at
    // the cursor.
    void cursor_left();
    void cursor_right();

private:
    void enter();
    void log_line();
    void remember(u32 begin, u32 end);
    void replace_line(const char* text, u32 length);
    void stop_browsing();
    void insert(const char* text, u32 count);
    void show_cursor();

    ShellHost host_{};
    u32 length_ = 0;
    char line_[kMaxLine + 1]{};
    u32 cursor_ = 0;  // index in line_ where typing goes

    char history_[kHistory][kMaxLine]{};
    u32 history_length_[kHistory]{};
    u32 history_head_ = 0;   // the slot the next line goes to
    u32 history_count_ = 0;
    u32 browse_ = 0;         // 0: not browsing; n: the nth newest entry
    u32 draft_length_ = 0;
    char draft_[kMaxLine]{};
};

}
