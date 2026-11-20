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

#include "shell_script.hpp"

namespace hombre {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

bool same_word(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (lower(*a) != lower(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// Case-insensitive ordering of two strings: negative, zero or positive.
i32 compare_nocase(const char* a, const char* b) {
    while (*a != '\0' && lower(*a) == lower(*b)) {
        ++a;
        ++b;
    }
    return static_cast<i32>(static_cast<u8>(lower(*a))) -
           static_cast<i32>(static_cast<u8>(lower(*b)));
}

bool parse_number(const char* s, i64& out) {
    while (is_space(*s)) {
        ++s;
    }
    bool negative = false;
    if (*s == '-' || *s == '+') {
        negative = *s == '-';
        ++s;
    }
    if (*s < '0' || *s > '9') {
        return false;
    }
    i64 value = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        ++s;
    }
    while (is_space(*s)) {
        ++s;
    }
    if (*s != '\0') {
        return false;
    }
    out = negative ? -value : value;
    return true;
}

// Splits text into at most max words, a quoted word losing its quotes.
// Returns how many words were found.
u32 split_words(const char* text, char words[][ScriptRunner::kMaxLine], u32 max) {
    u32 count = 0;
    const char* p = text;
    while (count < max) {
        while (is_space(*p)) {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        u32 n = 0;
        if (*p == '"') {
            ++p;
            while (*p != '\0' && *p != '"') {
                if (n + 1 < ScriptRunner::kMaxLine) {
                    words[count][n++] = *p;
                }
                ++p;
            }
            if (*p == '"') {
                ++p;
            }
        } else {
            while (*p != '\0' && !is_space(*p)) {
                if (n + 1 < ScriptRunner::kMaxLine) {
                    words[count][n++] = *p;
                }
                ++p;
            }
        }
        words[count][n] = '\0';
        ++count;
    }
    return count;
}

class Out {
public:
    Out(char* buf, u32 size) : buf_(buf), size_(size) {}
    bool put(char c) {
        if (len_ + 1 >= size_) {
            ok_ = false;
            return false;
        }
        buf_[len_++] = c;
        buf_[len_] = '\0';
        return true;
    }
    void put(const char* s) {
        while (*s != '\0') {
            put(*s++);
        }
    }
    bool ok() const { return ok_; }

private:
    char* buf_;
    u32 size_;
    u32 len_ = 0;
    bool ok_ = true;
};

// The line from at up to its end, without the newline, with a ';'
// outside quotes and backquotes cutting off a comment, trimmed.
void take_line(const char* text, u32 length, u32& at, char* line) {
    u32 n = 0;
    bool quoted = false;
    bool backquoted = false;
    bool comment = false;
    while (at < length && text[at] != '\n') {
        const char c = text[at++];
        if (comment) {
            continue;
        }
        if (c == '"' && !backquoted) {
            quoted = !quoted;
        } else if (c == '`') {
            backquoted = !backquoted;
        } else if (c == ';' && !quoted && !backquoted) {
            comment = true;
            continue;
        }
        if (n + 1 < ScriptRunner::kMaxLine) {
            line[n++] = c;
        }
    }
    if (at < length) {
        ++at;
    }
    while (n > 0 && is_space(line[n - 1])) {
        --n;
    }
    line[n] = '\0';
    u32 lead = 0;
    while (is_space(line[lead])) {
        ++lead;
    }
    if (lead > 0) {
        for (u32 i = 0; i + lead <= n; ++i) {
            line[i] = line[i + lead];
        }
    }
}

// Splits line into its first word, lower case, and the rest.
const char* first_word(const char* line, char* word, u32 size) {
    const char* p = line;
    while (is_space(*p)) {
        ++p;
    }
    u32 n = 0;
    while (*p != '\0' && !is_space(*p)) {
        if (n + 1 < size) {
            word[n++] = lower(*p);
        }
        ++p;
    }
    word[n] = '\0';
    while (is_space(*p)) {
        ++p;
    }
    return p;
}

}

bool expand_line(const ScriptHost& host, const char* line, char* out, u32 size) {
    if (size == 0) {
        return false;
    }
    out[0] = '\0';
    Out o(out, size);
    const char* p = line;
    while (*p != '\0') {
        if (*p == '`') {
            const char* end = p + 1;
            while (*end != '\0' && *end != '`') {
                ++end;
            }
            if (*end != '`' || host.capture == nullptr) {
                o.put(*p++);
                continue;
            }
            char command[ScriptRunner::kMaxLine];
            u32 n = 0;
            for (const char* q = p + 1; q < end && n + 1 < sizeof command; ++q) {
                command[n++] = *q;
            }
            command[n] = '\0';
            char output[ScriptRunner::kMaxLine];
            output[0] = '\0';
            if (host.capture(host.ctx, command, output, sizeof output)) {
                u32 len = 0;
                while (output[len] != '\0') {
                    if (output[len] == '\n' || output[len] == '\r') {
                        output[len] = ' ';
                    }
                    ++len;
                }
                while (len > 0 && output[len - 1] == ' ') {
                    output[--len] = '\0';
                }
                o.put(output);
            }
            p = end + 1;
            continue;
        }
        if (*p == '$' && host.get_var != nullptr) {
            const char* start = p + 1;
            const bool braced = *start == '{';
            if (braced) {
                ++start;
            }
            const char* end = start;
            while ((*end >= 'a' && *end <= 'z') || (*end >= 'A' && *end <= 'Z') ||
                   (*end >= '0' && *end <= '9') || *end == '_' || *end == '.') {
                ++end;
            }
            if (end == start || (braced && *end != '}')) {
                o.put(*p++);
                continue;
            }
            char name[ScriptRunner::kMaxLine];
            u32 n = 0;
            for (const char* q = start; q < end && n + 1 < sizeof name; ++q) {
                name[n++] = *q;
            }
            name[n] = '\0';
            char value[ScriptRunner::kMaxLine];
            const char* after = braced ? end + 1 : end;
            if (host.get_var(host.ctx, name, value, sizeof value)) {
                o.put(value);
            } else {
                for (const char* q = p; q < after; ++q) {
                    o.put(*q);
                }
            }
            p = after;
            continue;
        }
        o.put(*p++);
    }
    return o.ok();
}

bool ScriptRunner::condition(const char* args) const {
    char words[6][kMaxLine];
    u32 count = split_words(args, words, 6);
    u32 i = 0;
    bool negate = false;
    if (i < count && same_word(words[i], "not")) {
        negate = true;
        ++i;
    }
    bool result = false;
    if (i < count && same_word(words[i], "exists")) {
        result = i + 1 < count && host_.exists != nullptr && host_.exists(host_.ctx, words[i + 1]);
    } else if (i < count && same_word(words[i], "warn")) {
        result = rc_ >= 5;
    } else if (i < count && same_word(words[i], "error")) {
        result = rc_ >= 10;
    } else if (i < count && same_word(words[i], "fail")) {
        result = rc_ >= 20;
    } else if (i + 2 < count) {
        const char* a = words[i];
        const char* op = words[i + 1];
        const char* b = words[i + 2];
        const bool val = i + 3 < count && same_word(words[i + 3], "val");
        i32 order = 0;
        i64 x = 0;
        i64 y = 0;
        if (val && parse_number(a, x) && parse_number(b, y)) {
            order = x < y ? -1 : x > y ? 1 : 0;
        } else if (val) {
            // A value that is no number counts as 0, as AmigaDOS does.
            if (!parse_number(a, x)) {
                x = 0;
            }
            if (!parse_number(b, y)) {
                y = 0;
            }
            order = x < y ? -1 : x > y ? 1 : 0;
        } else {
            order = compare_nocase(a, b);
        }
        if (same_word(op, "eq")) {
            result = order == 0;
        } else if (same_word(op, "gt")) {
            result = order > 0;
        } else if (same_word(op, "ge")) {
            result = order >= 0;
        }
    }
    return negate ? !result : result;
}

bool ScriptRunner::skip_to(const char* text, u32 length, u32& at, const char* label, bool back) {
    u32 pos = back ? 0 : at;
    char line[kMaxLine];
    char word[16];
    while (pos < length) {
        take_line(text, length, pos, line);
        const char* rest = first_word(line, word, sizeof word);
        if (same_word(word, "lab") && same_word(rest, label)) {
            at = pos;
            return true;
        }
    }
    return false;
}

u32 ScriptRunner::run(const char* text, u32 length) {
    stop_ = Stop::None;
    rc_ = 0;
    bool active[kMaxDepth] = {};
    bool taken[kMaxDepth] = {};
    u32 depth = 0;
    u32 inactive = 0;  // IFs opened inside a branch that does not run
    char line[kMaxLine];
    char word[16];
    u32 at = 0;
    while (at < length) {
        take_line(text, length, at, line);
        if (line[0] == '\0' || line[0] == '.') {
            // TODO: handle .KEY and the other dot directives.
            continue;
        }
        if (host_.break_requested != nullptr && host_.break_requested(host_.ctx)) {
            if (host_.message != nullptr) {
                host_.message(host_.ctx, "***Break\n");
            }
            stop_ = Stop::Break;
            return rc_;
        }
        const char* rest = first_word(line, word, sizeof word);
        const bool running = inactive == 0 && (depth == 0 || active[depth - 1]);
        if (same_word(word, "if")) {
            if (!running || depth == kMaxDepth) {
                ++inactive;
                continue;
            }
            char expanded[kMaxLine];
            if (!expand_line(host_, rest, expanded, sizeof expanded)) {
                expanded[0] = '\0';
            }
            const bool c = condition(expanded);
            active[depth] = c;
            taken[depth] = c;
            ++depth;
            continue;
        }
        if (same_word(word, "else")) {
            if (inactive == 0 && depth > 0) {
                active[depth - 1] = !taken[depth - 1];
                taken[depth - 1] = true;
            }
            continue;
        }
        if (same_word(word, "endif")) {
            if (inactive > 0) {
                --inactive;
            } else if (depth > 0) {
                --depth;
            }
            continue;
        }
        if (!running || same_word(word, "lab")) {
            continue;
        }
        if (same_word(word, "skip")) {
            char words[2][kMaxLine];
            const u32 count = split_words(rest, words, 2);
            const bool back = count > 1 && same_word(words[1], "back");
            if (count == 0 || !skip_to(text, length, at, words[0], back)) {
                if (host_.message != nullptr) {
                    host_.message(host_.ctx, "SKIP: label not found\n");
                }
                rc_ = 10;
                stop_ = Stop::Failed;
                return rc_;
            }
            depth = 0;
            inactive = 0;
            continue;
        }
        if (same_word(word, "quit")) {
            i64 value = 0;
            rc_ = parse_number(rest, value) && value >= 0 ? static_cast<u32>(value) : 0;
            stop_ = Stop::Quit;
            return rc_;
        }
        char expanded[kMaxLine];
        if (!expand_line(host_, line, expanded, sizeof expanded)) {
            if (host_.message != nullptr) {
                host_.message(host_.ctx, "Line too long\n");
            }
            rc_ = 10;
        } else {
            rc_ = host_.run_line(host_.ctx, expanded);
        }
        const u32 level = host_.fail_level != nullptr ? host_.fail_level(host_.ctx) : 10;
        if (rc_ >= level) {
            stop_ = Stop::Failed;
            return rc_;
        }
    }
    return rc_;
}

}
