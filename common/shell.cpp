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

void Shell::init(const ShellHost& host) {
    host_ = host;
    length_ = 0;
    line_[0] = '\0';
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
        enter();
    } else if (ch == '\b') {
        to_bottom();
        if (length_ > 0) {
            --length_;
            host_.erase(host_.ctx);
        }
    } else if (ch == '\t') {
        to_bottom();
        const u32 spaces = kTabColumns - length_ % kTabColumns;
        for (u32 i = 0; i < spaces && length_ < kMaxLine; ++i) {
            line_[length_++] = ' ';
            host_.echo(host_.ctx, ' ');
        }
    } else if (is_printable(ch)) {
        to_bottom();
        if (length_ < kMaxLine) {
            line_[length_++] = static_cast<char>(ch);
            host_.echo(host_.ctx, ch);
        }
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

    u32 word_end = begin;
    while (word_end < end && line_[word_end] != ' ') {
        ++word_end;
    }
    u32 args_begin = word_end;
    while (args_begin < end && line_[args_begin] == ' ') {
        ++args_begin;
    }
    line_[end] = '\0';
    line_[word_end] = '\0';

    if (host_.builtin != nullptr &&
        host_.builtin(host_.ctx, line_ + begin, line_ + args_begin)) {
        host_.prompt(host_.ctx);
        length_ = 0;
        line_[0] = '\0';
        return;
    }

    bool needs_prefix = true;
    for (u32 i = begin; i < word_end; ++i) {
        if (line_[i] == ':' || line_[i] == '/') {
            needs_prefix = false;
            break;
        }
    }

    char prefixed[sizeof "C:" + kMaxLine];
    const char* path = line_ + begin;
    if (needs_prefix) {
        usize n = append_text(prefixed, sizeof prefixed, 0, "C:");
        n = append_text(prefixed, sizeof prefixed, n, path);
        prefixed[n] = '\0';
        path = prefixed;
    }

    if (!host_.run(host_.ctx, path, line_ + args_begin)) {
        char text[sizeof kUnknownLog + kMaxLine];
        usize n = append_text(text, sizeof text, 0, kUnknownMessage);
        n = append_text(text, sizeof text, n, line_ + begin);
        text[n] = '\0';
        host_.message(host_.ctx, text);
        n = append_text(text, sizeof text, 0, kUnknownLog);
        n = append_text(text, sizeof text, n, line_ + begin);
        text[n] = '\0';
        host_.log_line(host_.ctx, text);
    }
    host_.prompt(host_.ctx);

    length_ = 0;
    line_[0] = '\0';
}

}
