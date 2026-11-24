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

#include "shell_commands.hpp"

#include "format.hpp"
#include "libs/dos.hpp"

namespace hombre {

namespace {

constexpr u32 kLineBuf = 600;
constexpr char kDefaultPrompt[] = "%N.%S> ";

char ascii_upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
}

bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

bool eq_nocase(const char* a, const char* b) {
    u32 i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (ascii_upper(a[i]) != ascii_upper(b[i])) {
            return false;
        }
        ++i;
    }
    return a[i] == '\0' && b[i] == '\0';
}

bool slice_eq_nocase(const char* a, u32 len, const char* b) {
    for (u32 i = 0; i < len; ++i) {
        if (b[i] == '\0' || ascii_upper(a[i]) != ascii_upper(b[i])) {
            return false;
        }
    }
    return b[len] == '\0';
}

u32 append_str(char* out, u32 cap, u32 pos, const char* text) {
    if (out == nullptr || cap == 0 || text == nullptr) {
        return pos;
    }
    while (*text != '\0' && pos + 1 < cap) {
        out[pos++] = *text++;
    }
    out[pos] = '\0';
    return pos;
}

u32 append_char(char* out, u32 cap, u32 pos, char c) {
    if (out != nullptr && pos + 1 < cap) {
        out[pos++] = c;
        out[pos] = '\0';
    }
    return pos;
}

// Copies p[0..end) into out, stripping '"' quotes and expanding '*' escapes
// inside quotes just as AmigaDOS ECHO does.
u32 append_unquoted(const char* p, u32 end, char* out, u32 cap, u32 pos = 0) {
    bool in_quote = false;
    for (u32 i = 0; i < end; ++i) {
        const char c = p[i];
        if (c == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (in_quote && c == '*' && i + 1 < end) {
            const char next = p[++i];
            if (next == 'n' || next == 'N') {
                pos = append_char(out, cap, pos, '\n');
            } else if (next == 'e' || next == 'E') {
                pos = append_char(out, cap, pos, 0x1B);
            } else {
                pos = append_char(out, cap, pos, next);
            }
            continue;
        }
        pos = append_char(out, cap, pos, c);
    }
    if (out != nullptr && cap > 0) {
        out[pos] = '\0';
    }
    return pos;
}

// Reads one argument token from p (skipping leading blanks, stripping
// quotes and AmigaDOS '*' escapes when quoted). Returns false at end.
bool next_token(const char*& p, char* out, u32 cap) {
    if (p == nullptr || out == nullptr || cap == 0) {
        return false;
    }
    while (is_blank(*p)) {
        ++p;
    }
    if (*p == '\0') {
        out[0] = '\0';
        return false;
    }
    u32 pos = 0;
    if (*p == '"') {
        ++p;
        while (*p != '\0') {
            if (*p == '"') {
                ++p;
                break;
            }
            if (*p == '*' && p[1] != '\0') {
                const char next = p[1];
                p += 2;
                if (next == 'n' || next == 'N') {
                    pos = append_char(out, cap, pos, '\n');
                } else if (next == 'e' || next == 'E') {
                    pos = append_char(out, cap, pos, 0x1B);
                } else {
                    pos = append_char(out, cap, pos, next);
                }
                continue;
            }
            pos = append_char(out, cap, pos, *p++);
        }
    } else {
        while (*p != '\0' && !is_blank(*p)) {
            pos = append_char(out, cap, pos, *p++);
        }
    }
    out[pos] = '\0';
    return true;
}

bool is_builtin_word(const char* word) {
    constexpr const char* kNames[] = {"CD",       "ECHO",   "PATH",   "WHY",   "PROMPT",
                                      "ASK",      "FAILAT", "SET",    "GET",   "UNSET",
                                      "SETENV",   "GETENV", "UNSETENV", "RUN", "NEWSHELL",
                                      "ENDCLI",   "RESIDENT"};
    for (const char* name : kNames) {
        if (eq_nocase(word, name)) {
            return true;
        }
    }
    return false;
}

bool parse_u32_decimal(const char* s, u32& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    u64 val = 0;
    for (u32 i = 0; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        val = val * 10 + static_cast<u32>(s[i] - '0');
        if (val > 0xFFFFFFFFull) {
            return false;
        }
    }
    out = static_cast<u32>(val);
    return true;
}

}

void ShellCommands::emit(const char* text) const {
    if (print_ != nullptr && text != nullptr) {
        print_(ctx_, text);
    }
}

void ShellCommands::emit_line(const char* text) const {
    char buf[kLineBuf];
    u32 pos = append_str(buf, sizeof buf, 0, text);
    pos = append_char(buf, sizeof buf, pos, '\n');
    buf[pos] = '\0';
    emit(buf);
}

bool ShellCommands::run(const char* word, const char* args) {
    if (dos_ == nullptr || word == nullptr) {
        return false;
    }
    const char* a = args != nullptr ? args : "";
    if (eq_nocase(word, "RUN")) {
        cmd_run(a);
        return true;
    }
    if (eq_nocase(word, "NEWSHELL")) {
        cmd_newshell();
        return true;
    }
    if (eq_nocase(word, "ENDCLI")) {
        cmd_endcli();
        return true;
    }
    if (eq_nocase(word, "RESIDENT")) {
        cmd_resident(a);
        return true;
    }
    if (eq_nocase(word, "CD")) {
        cmd_cd(a);
        return true;
    }
    if (eq_nocase(word, "ECHO")) {
        cmd_echo(a);
        return true;
    }
    if (eq_nocase(word, "PATH")) {
        cmd_path(a);
        return true;
    }
    if (eq_nocase(word, "WHY")) {
        cmd_why();
        return true;
    }
    if (eq_nocase(word, "PROMPT")) {
        cmd_prompt(a);
        return true;
    }
    if (eq_nocase(word, "ASK")) {
        cmd_ask(a);
        return true;
    }
    if (eq_nocase(word, "FAILAT")) {
        cmd_failat(a);
        return true;
    }
    if (eq_nocase(word, "SETENV")) {
        cmd_set_var(a, true);
        return true;
    }
    if (eq_nocase(word, "GETENV")) {
        cmd_get_var(a, true);
        return true;
    }
    if (eq_nocase(word, "UNSETENV")) {
        cmd_unset_var(a, true);
        return true;
    }
    if (eq_nocase(word, "SET")) {
        cmd_set_var(a, false);
        return true;
    }
    if (eq_nocase(word, "GET")) {
        cmd_get_var(a, false);
        return true;
    }
    if (eq_nocase(word, "UNSET")) {
        cmd_unset_var(a, false);
        return true;
    }
    return false;
}

void ShellCommands::cmd_cd(const char* args) {
    const char* p = args;
    char dir[Dos::kMaxPath];
    if (!next_token(p, dir, sizeof dir)) {
        char cur[Dos::kMaxPath];
        if (!dos_->current_dir_name(cur, sizeof cur, process_)) {
            cur[0] = '\0';
        }
        emit_line(cur);
        dos_->set_return_code(0);
        return;
    }
    u32 err = 0;
    if (dos_->change_dir(dir, err, process_)) {
        dos_->set_return_code(0);
        return;
    }
    dos_->set_last_error(err);
    dos_->set_return_code(20);
    char msg[kLineBuf];
    u32 pos = 0;
    if (err == ndk::ERROR_OBJECT_WRONG_TYPE) {
        pos = append_str(msg, sizeof msg, pos, dir);
        pos = append_str(msg, sizeof msg, pos, " is not a directory\n");
    } else {
        pos = append_str(msg, sizeof msg, pos, "Can't find ");
        pos = append_str(msg, sizeof msg, pos, dir);
        pos = append_char(msg, sizeof msg, pos, '\n');
    }
    msg[pos] = '\0';
    emit(msg);
}

void ShellCommands::cmd_echo(const char* args) {
    const char* p = args;
    while (is_blank(*p)) {
        ++p;
    }
    u32 end = 0;
    while (p[end] != '\0') {
        ++end;
    }
    while (end > 0 && is_blank(p[end - 1])) {
        --end;
    }

    // Detect an unquoted trailing NOLINE keyword.
    bool in_quote = false;
    bool in_tok = false;
    bool last_has_quote = false;
    u32 last_start = 0;
    for (u32 i = 0; i < end; ++i) {
        const char c = p[i];
        if (in_quote) {
            if (c == '*' && i + 1 < end) {
                ++i;
            } else if (c == '"') {
                in_quote = false;
            }
        } else if (is_blank(c)) {
            in_tok = false;
        } else {
            if (!in_tok) {
                in_tok = true;
                last_start = i;
                last_has_quote = false;
            }
            if (c == '"') {
                in_quote = true;
                last_has_quote = true;
            }
        }
    }

    bool noline = false;
    if (in_tok && !last_has_quote &&
        slice_eq_nocase(p + last_start, end - last_start, "NOLINE")) {
        noline = true;
        end = last_start;
        while (end > 0 && is_blank(p[end - 1])) {
            --end;
        }
    }

    char out[kLineBuf];
    u32 pos = append_unquoted(p, end, out, sizeof out);
    if (!noline) {
        pos = append_char(out, sizeof out, pos, '\n');
    }
    out[pos] = '\0';
    emit(out);
    dos_->set_return_code(0);
}

void ShellCommands::cmd_path(const char* args) {
    auto show_path = [&]() {
        emit_line("Current_directory");
        const u32 count = dos_->path_count();
        for (u32 i = 0; i < count; ++i) {
            char dir[Dos::kMaxPath];
            if (dos_->path_name(i, dir, sizeof dir)) {
                emit_line(dir);
            }
        }
        emit_line("C:");
    };

    const char* scan = args;
    char tok[Dos::kMaxPath];
    bool has_tokens = false;
    bool quiet = false;
    bool show = false;
    bool has_remove = false;
    while (next_token(scan, tok, sizeof tok)) {
        has_tokens = true;
        if (eq_nocase(tok, "QUIET")) {
            quiet = true;
        } else if (eq_nocase(tok, "SHOW")) {
            show = true;
        } else if (eq_nocase(tok, "REMOVE")) {
            has_remove = true;
        }
    }

    if (!has_tokens) {
        show_path();
        dos_->set_return_code(0);
        return;
    }
    // A directory that is not there is reported, and the others still go
    // into the path.
    bool failed = false;
    const char* p = args;
    while (next_token(p, tok, sizeof tok)) {
        if (eq_nocase(tok, "SHOW") || eq_nocase(tok, "ADD") || eq_nocase(tok, "QUIET") ||
            eq_nocase(tok, "HEAD") || eq_nocase(tok, "REMOVE")) {
            continue;
        }
        if (eq_nocase(tok, "RESET")) {
            dos_->path_reset();
            continue;
        }
        // path_add keeps a directory that is already in the path once.
        u32 err = 0;
        const bool done =
            has_remove ? dos_->path_remove(tok, err) : dos_->path_add(tok, err);
        if (!done) {
            dos_->set_last_error(err);
            failed = true;
            if (!quiet) {
                char msg[kLineBuf];
                u32 pos = append_str(msg, sizeof msg, 0, tok);
                pos = append_str(msg, sizeof msg, pos, " not found\n");
                msg[pos] = '\0';
                emit(msg);
            }
        }
    }

    if (show) {
        show_path();
    }
    dos_->set_return_code(failed ? 10 : 0);
}

void ShellCommands::cmd_why() {
    const u32 rc = dos_->last_return_code(process_);
    if (rc == 0) {
        emit_line("The last command did not set a return code");
    } else {
        const u32 err = dos_->last_error(process_);
        if (err == 0) {
            emit_line("The last command did not set a reason for failure");
        } else {
            char fault_buf[256];
            dos_->fault(err, fault_buf, sizeof fault_buf);
            char msg[kLineBuf];
            u32 pos = append_str(msg, sizeof msg, 0, "Last command failed because ");
            pos = append_str(msg, sizeof msg, pos, fault_buf);
            pos = append_char(msg, sizeof msg, pos, '\n');
            msg[pos] = '\0';
            emit(msg);
        }
    }
    dos_->set_return_code(0);
}

void ShellCommands::cmd_prompt(const char* args) {
    const char* p = args;
    while (is_blank(*p)) {
        ++p;
    }
    u32 end = 0;
    while (p[end] != '\0') {
        ++end;
    }
    while (end > 0 && is_blank(p[end - 1])) {
        --end;
    }
    if (end == 0) {
        dos_->set_prompt(kDefaultPrompt, process_);
        dos_->set_return_code(0);
        return;
    }
    char buf[260];
    if (p[0] == '"') {
        next_token(p, buf, sizeof buf);
    } else {
        const u32 copy = end < sizeof buf - 1 ? end : static_cast<u32>(sizeof buf - 1);
        for (u32 i = 0; i < copy; ++i) {
            buf[i] = p[i];
        }
        buf[copy] = '\0';
    }
    dos_->set_prompt(buf, process_);
    dos_->set_return_code(0);
}

void ShellCommands::cmd_ask(const char* args) {
    const char* p = args;
    char prompt_str[kLineBuf];
    if (next_token(p, prompt_str, sizeof prompt_str) && prompt_str[0] != '\0') {
        char out[kLineBuf];
        u32 pos = append_str(out, sizeof out, 0, prompt_str);
        pos = append_char(out, sizeof out, pos, ' ');
        out[pos] = '\0';
        emit(out);
    }
    char line[256];
    if (!dos_->read_console_line(line, sizeof line)) {
        dos_->set_return_code(0);
        return;
    }
    u32 start = 0;
    while (is_blank(line[start])) {
        ++start;
    }
    u32 end = start;
    while (line[end] != '\0') {
        ++end;
    }
    while (end > start && is_blank(line[end - 1])) {
        --end;
    }
    line[end] = '\0';
    const char* ans = line + start;
    dos_->set_return_code(eq_nocase(ans, "y") || eq_nocase(ans, "yes") ? 5u : 0u);
}

void ShellCommands::cmd_failat(const char* args) {
    const char* p = args;
    char tok[64];
    if (!next_token(p, tok, sizeof tok)) {
        char digits[16];
        format_u64(digits, sizeof digits, dos_->fail_level(process_), 10);
        char msg[64];
        u32 pos = append_str(msg, sizeof msg, 0, "Fail limit: ");
        pos = append_str(msg, sizeof msg, pos, digits);
        pos = append_char(msg, sizeof msg, pos, '\n');
        msg[pos] = '\0';
        emit(msg);
        dos_->set_return_code(0);
        return;
    }
    u32 level = 0;
    if (!parse_u32_decimal(tok, level)) {
        dos_->set_last_error(ndk::ERROR_BAD_NUMBER);
        dos_->set_return_code(20);
        emit_line("Bad number");
        return;
    }
    if (level < 1u) {
        level = 1u;
    }
    dos_->set_fail_level(level);
    dos_->set_return_code(0);
}

void ShellCommands::cmd_run(const char* args) {
    auto unknown = [&](const char* word) {
        char msg[kLineBuf];
        u32 pos = append_str(msg, sizeof msg, 0, "Unknown command ");
        pos = append_str(msg, sizeof msg, pos, word);
        pos = append_char(msg, sizeof msg, pos, '\n');
        msg[pos] = '\0';
        emit(msg);
        dos_->set_return_code(10);
    };
    const char* p = args;
    char word[Dos::kMaxPath];
    if (!next_token(p, word, sizeof word)) {
        emit_line("Run: no command");
        dos_->set_return_code(10);
        return;
    }
    if (is_builtin_word(word)) {
        unknown(word);
        return;
    }
    char cmd[Dos::kMaxPath];
    if (!dos_->find_command(word, cmd, sizeof cmd, process_)) {
        unknown(word);
        return;
    }
    while (is_blank(*p)) {
        ++p;
    }
    const u32 cli = dos_->start_background(cmd, p);
    if (cli == 0) {
        unknown(word);
        return;
    }
    char digits[16];
    format_u64(digits, sizeof digits, cli, 10);
    char msg[kLineBuf];
    u32 pos = append_str(msg, sizeof msg, 0, "[CLI ");
    pos = append_str(msg, sizeof msg, pos, digits);
    pos = append_str(msg, sizeof msg, pos, "]\n");
    msg[pos] = '\0';
    emit(msg);
    dos_->set_return_code(0);
}

void ShellCommands::cmd_newshell() {
    if (new_shell_ != nullptr && new_shell_(new_shell_ctx_)) {
        dos_->set_return_code(0);
        return;
    }
    emit_line("NewShell: cannot open a new shell");
    dos_->set_return_code(20);
}

// Ends the shell; the boot shell too, as the Startup-Sequence ends it.
void ShellCommands::cmd_endcli() {
    if (process_ == 0 || process_ == dos_->process()) {
        emit_line("Process 1 ending");
    }
    ended_ = true;
    dos_->set_return_code(0);
}

// Resident: dos keeps the resident commands, which Execute and System find
// as well.
void ShellCommands::cmd_resident(const char* args) {
    dos_->set_return_code(dos_->resident_command(args));
}

void ShellCommands::cmd_set_var(const char* args, bool global) {
    const char* p = args;
    char name[Dos::kMaxPath];
    if (!next_token(p, name, sizeof name)) {
        if (global) {
            const bool ok = dos_->list_env_vars(
                [](void* ctx, const char* entry_name) {
                    static_cast<const ShellCommands*>(ctx)->emit_line(entry_name);
                },
                this);
            dos_->set_return_code(ok ? 0u : 20u);
        } else {
            dos_->list_local_vars(
                ndk::LV_VAR,
                [](void* ctx, const char* var_name, const char* value) {
                    const ShellCommands* self = static_cast<const ShellCommands*>(ctx);
                    char line[kLineBuf];
                    u32 pos = append_str(line, sizeof line, 0, var_name);
                    pos = append_char(line, sizeof line, pos, ' ');
                    pos = append_str(line, sizeof line, pos, value);
                    pos = append_char(line, sizeof line, pos, '\n');
                    line[pos] = '\0';
                    self->emit(line);
                },
                this, process_);
            dos_->set_return_code(0);
        }
        return;
    }
    while (is_blank(*p)) {
        ++p;
    }
    u32 end = 0;
    while (p[end] != '\0') {
        ++end;
    }
    while (end > 0 && is_blank(p[end - 1])) {
        --end;
    }
    char value[kLineBuf];
    append_unquoted(p, end, value, sizeof value);
    const u32 flags = ndk::LV_VAR | (global ? ndk::GVF_GLOBAL_ONLY : 0u);
    if (dos_->set_var(name, value, flags)) {
        dos_->set_return_code(0);
        return;
    }
    dos_->set_last_error(dos_->last_error(process_));
    dos_->set_return_code(20);
}

void ShellCommands::cmd_get_var(const char* args, bool global) {
    const char* p = args;
    char name[Dos::kMaxPath];
    if (!next_token(p, name, sizeof name)) {
        dos_->set_last_error(ndk::ERROR_REQUIRED_ARG_MISSING);
        dos_->set_return_code(20);
        return;
    }
    char value[kLineBuf];
    const u32 flags = ndk::LV_VAR | (global ? ndk::GVF_GLOBAL_ONLY : ndk::GVF_LOCAL_ONLY);
    if (dos_->get_var(name, value, sizeof value, flags) < 0) {
        dos_->set_last_error(dos_->last_error(process_));
        dos_->set_return_code(5);
        return;
    }
    emit(value);
    emit("\n");
    dos_->set_return_code(0);
}

void ShellCommands::cmd_unset_var(const char* args, bool global) {
    const char* p = args;
    char name[Dos::kMaxPath];
    if (!next_token(p, name, sizeof name)) {
        dos_->set_last_error(ndk::ERROR_REQUIRED_ARG_MISSING);
        dos_->set_return_code(20);
        return;
    }
    const u32 flags = ndk::LV_VAR | (global ? ndk::GVF_GLOBAL_ONLY : ndk::GVF_LOCAL_ONLY);
    if (!dos_->delete_var(name, flags)) {
        const u32 err = dos_->last_error(process_);
        if (err != ndk::ERROR_OBJECT_NOT_FOUND) {
            dos_->set_last_error(err);
            dos_->set_return_code(20);
            return;
        }
    }
    dos_->set_return_code(0);
}

u32 format_prompt(const Dos& dos, u32 cli_number, char* out, u32 size, u32 process) {
    if (out == nullptr || size == 0) {
        return 0;
    }
    char fmt[260];
    if (!dos.prompt(fmt, sizeof fmt, process) || fmt[0] == '\0') {
        append_str(fmt, sizeof fmt, 0, kDefaultPrompt);
    }
    char dir[260];
    if (!dos.current_dir_name(dir, sizeof dir, process)) {
        dir[0] = '\0';
    }
    char num[16];
    format_u64(num, sizeof num, cli_number, 10);

    u32 pos = 0;
    for (u32 i = 0; fmt[i] != '\0' && pos + 1 < size; ++i) {
        if (fmt[i] != '%') {
            out[pos++] = fmt[i];
            continue;
        }
        const char next = fmt[i + 1];
        if (next == '\0') {
            out[pos++] = '%';
            break;
        }
        ++i;
        if (next == 'N' || next == 'n') {
            pos = append_str(out, size, pos, num);
        } else if (next == 'S' || next == 's') {
            pos = append_str(out, size, pos, dir);
        } else if (next == '%') {
            out[pos++] = '%';
        } else {
            out[pos++] = '%';
            if (pos + 1 < size) {
                out[pos++] = next;
            }
        }
    }
    out[pos] = '\0';
    return pos;
}

}
