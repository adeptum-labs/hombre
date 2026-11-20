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

// What a script runs on: run_line runs one expanded command line and returns
// its return code; exists tells whether a file or directory is there;
// get_var reads a local, then a global variable; capture runs a command
// line and returns its output; break_requested is true once Ctrl+D asks
// the script to stop; fail_level is the CLI's FailAt.
struct ScriptHost {
    u32 (*run_line)(void* ctx, const char* line);
    bool (*exists)(void* ctx, const char* path);
    bool (*get_var)(void* ctx, const char* name, char* out, u32 size);
    bool (*capture)(void* ctx, const char* line, char* out, u32 size);
    bool (*break_requested)(void* ctx);
    u32 (*fail_level)(void* ctx);
    void (*message)(void* ctx, const char* text);
    void* ctx;
};

// Expands a command line into out: `command` becomes the command's output,
// its newlines turned into spaces, and $name or ${name} the variable's
// value; an unset variable stays as written. False when out is too small.
bool expand_line(const ScriptHost& host, const char* line, char* out, u32 size);

// Runs an AmigaDOS script: one command per line, ';' starting a comment,
// with IF [NOT] EXISTS | WARN | ERROR | FAIL | a EQ|GT|GE b [VAL], ELSE and
// ENDIF, SKIP label [BACK] and LAB label, QUIT [rc]. The script stops when
// a command returns at least the fail level, at QUIT, or on Ctrl+D.
class ScriptRunner {
public:
    static constexpr u32 kMaxLine = 512;
    static constexpr u32 kMaxDepth = 16;

    explicit ScriptRunner(const ScriptHost& host) : host_(host) {}

    // Runs length bytes of text; returns the return code of the last
    // command, or QUIT's.
    u32 run(const char* text, u32 length);

    // Why the script stopped early, if it did.
    enum class Stop : u8 { None, Failed, Quit, Break };
    Stop stopped() const { return stop_; }

private:
    bool condition(const char* args) const;
    bool skip_to(const char* text, u32 length, u32& at, const char* label, bool back);

    ScriptHost host_;
    Stop stop_ = Stop::None;
    u32 rc_ = 0;
};

}
