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

#include "shell.hpp"

namespace hombre {

namespace {

constexpr char kLinePrefix[] = "shell: line: ";
constexpr char kUnknownMessage[] = "Unknown command ";
constexpr char kUnknownLog[] = "shell: unknown command ";
constexpr u32 kTabColumns = 8;

constexpr bool is_printable(u8 c) {
    return (c >= 0x20 && c <= 0x7e) || c >= 0xa0;
}

// Copies text into out at position at, keeping room for the terminator.
usize append_text(char* out, usize cap, usize at, const char* text) {
    while (*text != '\0' && at + 1 < cap) {
        out[at++] = *text++;
    }
    return at;
}

}

namespace {

bool is_blank(char c) { return c == ' ' || c == '\t'; }

// Takes <name, >name and >>name, with or without a space before the name,
// out of line outside quotes. The last of each kind wins.
void take_redirections(char* line, char* input, char* output, bool& append, u32 size) {
    input[0] = '\0';
    output[0] = '\0';
    append = false;
    u32 to = 0;
    bool quoted = false;
    for (u32 at = 0; line[at] != '\0';) {
        const char c = line[at];
        if (c == '"') {
            quoted = !quoted;
        }
        const bool start = to == 0 || is_blank(line[at - 1]);
        if (quoted || (c != '<' && c != '>') || !start) {
            line[to++] = line[at++];
            continue;
        }
        char* name = c == '<' ? input : output;
        if (c == '>') {
            append = line[at + 1] == '>';
            at += append ? 2 : 1;
        } else {
            ++at;
        }
        while (is_blank(line[at])) {
            ++at;
        }
        u32 n = 0;
        bool name_quoted = false;
        while (line[at] != '\0' && (name_quoted || !is_blank(line[at]))) {
            if (line[at] == '"') {
                name_quoted = !name_quoted;
            } else if (n + 1 < size) {
                name[n++] = line[at];
            }
            ++at;
        }
        name[n] = '\0';
    }
    line[to] = '\0';
}

}

void run_command_line(const ShellHost& host, const char* text) {
    char line[Shell::kMaxLine + 1];
    if (host.expand == nullptr || !host.expand(host.ctx, text, line, sizeof line)) {
        line[append_text(line, sizeof line, 0, text)] = '\0';
    }
    char input[Shell::kMaxLine + 1];
    char output[Shell::kMaxLine + 1];
    bool append = false;
    take_redirections(line, input, output, append, sizeof input);

    u32 begin = 0;
    while (is_blank(line[begin])) {
        ++begin;
    }
    u32 end = begin;
    while (line[end] != '\0') {
        ++end;
    }
    while (end > begin && is_blank(line[end - 1])) {
        --end;
    }
    if (begin == end) {
        return;
    }
    u32 word_end = begin;
    while (word_end < end && !is_blank(line[word_end])) {
        ++word_end;
    }
    u32 args_begin = word_end;
    while (args_begin < end && is_blank(line[args_begin])) {
        ++args_begin;
    }
    line[end] = '\0';
    line[word_end] = '\0';
    const char* word = line + begin;
    const char* args = line + args_begin;

    if (host.redirect != nullptr) {
        host.redirect(host.ctx, input[0] != '\0' ? input : nullptr,
                      output[0] != '\0' ? output : nullptr, append);
    }
    if (host.builtin != nullptr && host.builtin(host.ctx, word, args)) {
        if (host.redirect != nullptr) {
            host.redirect(host.ctx, nullptr, nullptr, false);
        }
        return;
    }

    bool needs_prefix = true;
    for (const char* p = word; *p != '\0'; ++p) {
        if (*p == ':' || *p == '/') {
            needs_prefix = false;
            break;
        }
    }
    char prefixed[sizeof "C:" + Shell::kMaxLine];
    const char* path = word;
    if (needs_prefix) {
        usize n = append_text(prefixed, sizeof prefixed, 0, "C:");
        n = append_text(prefixed, sizeof prefixed, n, word);
        prefixed[n] = '\0';
        path = prefixed;
    }
    const bool ran = host.run(host.ctx, path, args);
    if (host.redirect != nullptr) {
        host.redirect(host.ctx, nullptr, nullptr, false);
    }
    if (!ran) {
        char message[sizeof kUnknownLog + Shell::kMaxLine];
        usize n = append_text(message, sizeof message, 0, kUnknownMessage);
        n = append_text(message, sizeof message, n, word);
        message[n] = '\0';
        host.message(host.ctx, message);
        n = append_text(message, sizeof message, 0, kUnknownLog);
        n = append_text(message, sizeof message, n, word);
        message[n] = '\0';
        host.log_line(host.ctx, message);
    }
}

void Shell::init(const ShellHost& host) {
    host_ = host;
    length_ = 0;
    line_[0] = '\0';
    cursor_ = 0;
    history_head_ = 0;
    history_count_ = 0;
    browse_ = 0;
    draft_length_ = 0;
}

void Shell::history_up() {
    if (browse_ == history_count_) {
        return;
    }
    if (browse_ == 0) {
        for (u32 i = 0; i < length_; ++i) {
            draft_[i] = line_[i];
        }
        draft_length_ = length_;
    }
    ++browse_;
    const u32 slot = (history_head_ + kHistory - browse_) % kHistory;
    replace_line(history_[slot], history_length_[slot]);
}

void Shell::history_down() {
    if (browse_ == 0) {
        return;
    }
    --browse_;
    if (browse_ == 0) {
        replace_line(draft_, draft_length_);
        return;
    }
    const u32 slot = (history_head_ + kHistory - browse_) % kHistory;
    replace_line(history_[slot], history_length_[slot]);
}

void Shell::replace_line(const char* text, u32 length) {
    if (host_.scroll_to_bottom != nullptr) {
        host_.scroll_to_bottom(host_.ctx);
    }
    for (; length_ > 0; --length_) {
        host_.erase(host_.ctx);
    }
    for (u32 i = 0; i < length; ++i) {
        line_[i] = text[i];
        host_.echo(host_.ctx, static_cast<u8>(text[i]));
    }
    length_ = length;
    cursor_ = length;
}

void Shell::stop_browsing() {
    browse_ = 0;
}

// Keeps line_[begin, end) as the newest history entry unless it repeats
// the one before it.
void Shell::remember(u32 begin, u32 end) {
    const u32 length = end - begin;
    if (history_count_ != 0) {
        const u32 newest = (history_head_ + kHistory - 1) % kHistory;
        bool same = history_length_[newest] == length;
        for (u32 i = 0; same && i < length; ++i) {
            same = history_[newest][i] == line_[begin + i];
        }
        if (same) {
            return;
        }
    }
    for (u32 i = 0; i < length; ++i) {
        history_[history_head_][i] = line_[begin + i];
    }
    history_length_[history_head_] = length;
    history_head_ = (history_head_ + 1) % kHistory;
    if (history_count_ < kHistory) {
        ++history_count_;
    }
}

void Shell::key(u8 ch) {
    auto to_bottom = [&]() {
        if (host_.scroll_to_bottom != nullptr) {
            host_.scroll_to_bottom(host_.ctx);
        }
    };
    if (ch == '\n' || ch == '\b' || ch == '\t' || is_printable(ch)) {
        stop_browsing();
    }
    if (ch == '\n') {
        to_bottom();
        if (cursor_ != length_) {
            cursor_ = length_;
            show_cursor();
        }
        enter();
        cursor_ = 0;
    } else if (ch == '\b') {
        to_bottom();
        if (cursor_ == 0) {
            return;
        }
        const u32 tail = length_ - cursor_;
        for (u32 i = 0; i <= tail; ++i) {
            host_.erase(host_.ctx);
        }
        for (u32 i = cursor_; i < length_; ++i) {
            line_[i - 1] = line_[i];
        }
        --length_;
        --cursor_;
        for (u32 i = cursor_; i < length_; ++i) {
            host_.echo(host_.ctx, static_cast<u8>(line_[i]));
        }
        if (tail != 0) {
            show_cursor();
        }
    } else if (ch == '\t') {
        to_bottom();
        constexpr char kSpaces[kTabColumns] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        insert(kSpaces, kTabColumns - cursor_ % kTabColumns);
    } else if (is_printable(ch)) {
        to_bottom();
        const char c = static_cast<char>(ch);
        insert(&c, 1);
    }
}

void Shell::cursor_left() {
    if (host_.cursor_back == nullptr || cursor_ == 0) {
        return;
    }
    --cursor_;
    show_cursor();
}

void Shell::cursor_right() {
    if (host_.cursor_back == nullptr || cursor_ == length_) {
        return;
    }
    ++cursor_;
    show_cursor();
}

// Inserts count bytes at the cursor, as many as fit, and redraws the rest
// of the line after them.
void Shell::insert(const char* text, u32 count) {
    if (count > kMaxLine - length_) {
        count = kMaxLine - length_;
    }
    if (count == 0) {
        return;
    }
    const u32 tail = length_ - cursor_;
    for (u32 i = 0; i < tail; ++i) {
        host_.erase(host_.ctx);
    }
    for (u32 i = length_; i-- > cursor_;) {
        line_[i + count] = line_[i];
    }
    for (u32 i = 0; i < count; ++i) {
        line_[cursor_ + i] = text[i];
    }
    length_ += count;
    for (u32 i = cursor_; i < length_; ++i) {
        host_.echo(host_.ctx, static_cast<u8>(line_[i]));
    }
    cursor_ += count;
    if (tail != 0) {
        show_cursor();
    }
}

void Shell::show_cursor() {
    if (host_.cursor_back != nullptr) {
        host_.cursor_back(host_.ctx, length_ - cursor_);
    }
}

// The raw line goes to the log, with bytes above printable ASCII escaped so
// that Latin-1 text cannot pass for anything else there.
void Shell::log_line() {
    constexpr char kHex[] = "0123456789abcdef";
    char text[kMaxLine * 4 + sizeof kLinePrefix];
    usize n = append_text(text, sizeof text, 0, kLinePrefix);
    for (u32 i = 0; i < length_; ++i) {
        const u8 c = static_cast<u8>(line_[i]);
        if (c > 0x7e) {
            text[n++] = '\\';
            text[n++] = 'x';
            text[n++] = kHex[c >> 4];
            text[n++] = kHex[c & 0x0f];
        } else {
            text[n++] = static_cast<char>(c);
        }
    }
    text[n] = '\0';
    host_.log_line(host_.ctx, text);
}

void Shell::enter() {
    host_.end_line(host_.ctx);
    log_line();

    u32 begin = 0;
    while (begin < length_ && line_[begin] == ' ') {
        ++begin;
    }
    u32 end = length_;
    while (end > begin && line_[end - 1] == ' ') {
        --end;
    }
    if (begin != end) {
        remember(begin, end);
    }
    if (begin == end) {
        host_.prompt(host_.ctx);
        length_ = 0;
        line_[0] = '\0';
        return;
    }

    line_[end] = '\0';
    run_command_line(host_, line_ + begin);
    host_.prompt(host_.ctx);

    length_ = 0;
    line_[0] = '\0';
}

}
