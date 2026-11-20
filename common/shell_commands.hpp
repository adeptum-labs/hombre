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

class Dos;

// Output callback for shell builtins; text may contain '\n'.
using ShellPrint = void (*)(void* ctx, const char* text);

// Internal AmigaDOS Shell commands (CD, ECHO, PATH, WHY, PROMPT, ASK, FAILAT,
// SET, GET, UNSET, SETENV, GETENV, UNSETENV, RUN, NEWSHELL, ENDCLI, RESIDENT)
// backed by a Dos instance and a text output callback.
class ShellCommands {
public:
    constexpr ShellCommands() = default;
    ShellCommands(Dos& dos, ShellPrint print, void* ctx)
        : dos_(&dos), print_(print), ctx_(ctx) {}

    void init(Dos& dos, ShellPrint print, void* ctx) {
        dos_ = &dos;
        print_ = print;
        ctx_ = ctx;
        ended_ = false;
    }

    // The process whose CLI the builtins act on. 0 is the boot shell's
    // process.
    void set_process(u32 process) { process_ = process; }

    // Sets the NewShell hook: it opens a new shell and returns false when
    // none can be opened.
    void set_new_shell(bool (*open)(void* ctx), void* ctx) {
        new_shell_ = open;
        new_shell_ctx_ = ctx;
    }

    // True once EndCLI has ended the shell.
    bool ended() const { return ended_; }

    // Runs word with args if word matches an internal command (case-insensitive).
    // Returns false for non-builtin commands.
    bool run(const char* word, const char* args);

private:
    void emit(const char* text) const;
    void emit_line(const char* text) const;
    void cmd_cd(const char* args);
    void cmd_echo(const char* args);
    void cmd_path(const char* args);
    void cmd_why();
    void cmd_prompt(const char* args);
    void cmd_ask(const char* args);
    void cmd_failat(const char* args);
    void cmd_run(const char* args);
    void cmd_newshell();
    void cmd_resident();
    void cmd_endcli();
    void cmd_set_var(const char* args, bool global);
    void cmd_get_var(const char* args, bool global);
    void cmd_unset_var(const char* args, bool global);

    Dos* dos_ = nullptr;
    ShellPrint print_ = nullptr;
    void* ctx_ = nullptr;
    u32 process_ = 0;
    bool (*new_shell_)(void* ctx) = nullptr;
    void* new_shell_ctx_ = nullptr;
    bool ended_ = false;
};

// Formats the CLI prompt into out using cli_Prompt (%N, %S, %%; unknown %
// sequences copied as is). Returns the length written, excluding NUL. process
// 0 is the boot shell's process.
u32 format_prompt(const Dos& dos, u32 cli_number, char* out, u32 size, u32 process = 0);

}
