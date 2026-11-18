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
// true when handled), run starts a command, and message, prompt and
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
};

// One shell session: a fixed line buffer edited by the keyboard and parsed
// into a command path and its argument text when Enter is pressed.
class Shell {
public:
    static constexpr u32 kMaxLine = 512;

    void init(const ShellHost& host);
    void key(u8 ch);

private:
    void enter();
    void log_line();

    ShellHost host_{};
    u32 length_ = 0;
    char line_[kMaxLine + 1]{};
};

}
