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

#include "check.hpp"
#include "shell.hpp"
#include "shell_text.hpp"
#include "shell_window.hpp"

#include <string>

using namespace hombre;

namespace {

constinit ShellText constinit_probe;

void put_str(ShellText& text, const char* s) {
    for (const char* p = s; *p != '\0'; ++p) {
        text.put(static_cast<u8>(*p));
    }
}

std::string row_str(const ShellText::Row& row) {
    return std::string(reinterpret_cast<const char*>(row.text), row.length);
}

void wraps_at_columns() {
    ShellText text;
    put_str(text, "abcdefghijklmnopqrstuvwxy");
    text.new_line();
    CHECK_EQ(text.line_count(), 2u);
    ShellText::Row rows[8]{};
    CHECK_EQ(text.tail(10, 8, rows), 4u);
    CHECK_EQ(rows[0].length, 10u);
    CHECK(row_str(rows[0]) == "abcdefghij");
    CHECK_EQ(rows[1].length, 10u);
    CHECK(row_str(rows[1]) == "klmnopqrst");
    CHECK_EQ(rows[2].length, 5u);
    CHECK(row_str(rows[2]) == "uvwxy");
    CHECK_EQ(rows[3].length, 0u);
}

void rewrap_wider_joins_rows() {
    ShellText text;
    put_str(text, "abcdefghijklmnopqrstuvwxy");
    text.new_line();
    ShellText::Row rows[8]{};
    CHECK_EQ(text.tail(30, 8, rows), 2u);
    CHECK_EQ(rows[0].length, 25u);
    CHECK(row_str(rows[0]) == "abcdefghijklmnopqrstuvwxy");
    CHECK_EQ(rows[1].length, 0u);
}

void current_line_cursor_row() {
    ShellText text;
    put_str(text, "0123456789");
    ShellText::Row rows[8]{};
    CHECK_EQ(text.tail(10, 8, rows), 2u);
    CHECK_EQ(rows[0].length, 10u);
    CHECK(row_str(rows[0]) == "0123456789");
    CHECK_EQ(rows[1].length, 0u);
    CHECK_EQ(text.cursor_column(10), 0u);
}

void erase_stops_at_input_start() {
    ShellText text;
    put_str(text, "1> ");
    text.mark_input_start();
    text.put('a');
    text.put('b');
    CHECK(text.erase_last());
    CHECK(text.erase_last());
    CHECK(!text.erase_last());
    ShellText::Row rows[4]{};
    CHECK_EQ(text.tail(10, 4, rows), 1u);
    CHECK(row_str(rows[0]) == "1> ");
    CHECK_EQ(text.cursor_column(10), 3u);
}

void drops_oldest_lines() {
    ShellText text;
    for (u32 i = 0; i < 1200; ++i) {
        const std::string line = "L" + std::to_string(i);
        put_str(text, line.c_str());
        text.new_line();
    }
    CHECK_EQ(text.line_count(), ShellText::kMaxLines);
    ShellText::Row rows[ShellText::kMaxLines]{};
    CHECK_EQ(text.tail(16, ShellText::kMaxLines, rows), ShellText::kMaxLines);
    CHECK(row_str(rows[0]) == "L177");
    CHECK(row_str(rows[ShellText::kMaxLines - 2]) == "L1199");
    CHECK_EQ(rows[ShellText::kMaxLines - 1].length, 0u);
}

void long_line_breaks() {
    constexpr u32 kMax = ShellText::kMaxLineBytes;
    ShellText text;
    text.mark_input_start();
    for (u32 i = 0; i < kMax + 44; ++i) {
        text.put(static_cast<u8>('a' + i % 26));
    }
    CHECK_EQ(text.line_count(), 2u);
    ShellText::Row rows[4]{};
    CHECK_EQ(text.tail(kMax, 4, rows), 2u);
    CHECK_EQ(rows[0].length, kMax);
    CHECK_EQ(rows[1].length, 44u);
    CHECK_EQ(text.cursor_column(kMax), 44u);
    for (u32 i = 0; i < 44; ++i) {
        CHECK(text.erase_last());
    }
    CHECK(!text.erase_last());
}

// The longest line the shell's editor takes, after the longest prompt, stays
// one logical line that erase can take back to the prompt.
void longest_typed_line_is_one_line() {
    ShellText text;
    for (u32 i = 0; i < ShellWindow::kMaxPrompt + 16; ++i) {
        text.put('p');
    }
    text.mark_input_start();
    for (u32 i = 0; i < Shell::kMaxLine; ++i) {
        text.put('a');
    }
    CHECK_EQ(text.line_count(), 1u);
    for (u32 i = 0; i < Shell::kMaxLine; ++i) {
        CHECK(text.erase_last());
    }
    CHECK(!text.erase_last());
}

void rewrap_long_input_at_minimum() {
    ShellText text;
    put_str(text, "1> ");
    text.mark_input_start();
    for (u32 i = 0; i < 40; ++i) {
        text.put(static_cast<u8>('a' + i % 26));
    }
    ShellText::Row rows[5]{};
    const u32 count = text.tail(19, 5, rows);
    CHECK_EQ(count, 3u);
    CHECK_EQ(rows[0].length, 19u);
    CHECK_EQ(rows[1].length, 19u);
    CHECK_EQ(text.cursor_column(19), 43u % 19u);
    CHECK_EQ(rows[count - 1].length, text.cursor_column(19));
    for (u32 i = 0; i < 40; ++i) {
        CHECK(text.erase_last());
    }
    CHECK(!text.erase_last());
}

void tail_with_more_rows_than_text() {
    ShellText text;
    put_str(text, "one");
    text.new_line();
    put_str(text, "two");
    text.new_line();
    ShellText::Row rows[20]{};
    CHECK_EQ(text.tail(10, 20, rows), 3u);
    CHECK(row_str(rows[0]) == "one");
    CHECK(row_str(rows[1]) == "two");
    CHECK_EQ(rows[2].length, 0u);
}

void total_rows_matches_tail() {
    ShellText text;
    ShellText::Row rows[48]{};
    CHECK_EQ(text.total_rows(10), 1u);
    CHECK_EQ(text.tail(10, 48, rows), 1u);

    put_str(text, "abcdefghijklmnopqrstuvwxy");
    text.new_line();
    text.new_line();
    put_str(text, "0123456789");

    const u32 total_10 = text.total_rows(10);
    CHECK_EQ(total_10, 6u);
    CHECK_EQ(text.tail(10, 48, rows), total_10);

    const u32 total_0 = text.total_rows(0);
    CHECK_EQ(total_0, 37u);
    CHECK_EQ(text.tail(0, 48, rows), total_0);
}

void rows_from_top_and_middle() {
    ShellText text;
    put_str(text, "abcdefgh");
    text.new_line();
    text.new_line();
    put_str(text, "0123456789");

    CHECK_EQ(text.total_rows(5), 6u);

    ShellText::Row out[6]{};
    CHECK_EQ(text.rows_from(5, 0, 3, out), 3u);
    CHECK(row_str(out[0]) == "abcde");
    CHECK(row_str(out[1]) == "fgh");
    CHECK_EQ(out[2].length, 0u);

    CHECK_EQ(text.rows_from(5, 1, 4, out), 4u);
    CHECK(row_str(out[0]) == "fgh");
    CHECK_EQ(out[1].length, 0u);
    CHECK(row_str(out[2]) == "01234");
    CHECK(row_str(out[3]) == "56789");

    ShellText::Row tail_out[3]{};
    CHECK_EQ(text.tail(5, 3, tail_out), 3u);
    CHECK_EQ(text.rows_from(5, 3, 3, out), 3u);
    for (u32 i = 0; i < 3; ++i) {
        CHECK(row_str(out[i]) == row_str(tail_out[i]));
    }
}

void rows_from_clamps_past_end() {
    ShellText text;
    put_str(text, "abcdef");
    text.new_line();
    put_str(text, "gh");

    const u32 total = text.total_rows(4);
    CHECK_EQ(total, 3u);

    ShellText::Row out[4]{};
    CHECK_EQ(text.rows_from(4, 1, 10, out), 2u);
    CHECK(row_str(out[0]) == "ef");
    CHECK(row_str(out[1]) == "gh");

    CHECK_EQ(text.rows_from(4, total, 4, out), 0u);
    CHECK_EQ(text.rows_from(4, total + 10, 4, out), 0u);
    CHECK_EQ(text.rows_from(4, 0, 0, out), 0u);
    CHECK_EQ(text.rows_from(4, 0, 4, nullptr), 0u);
    CHECK_EQ(text.rows_from(4, 1, 0xffffffffu, out), 2u);
    CHECK(row_str(out[0]) == "ef");
    CHECK(row_str(out[1]) == "gh");
}

void keeps_1024_lines() {
    ShellText text;
    for (u32 i = 0; i < 1100; ++i) {
        if (i > 0) {
            text.new_line();
        }
        const std::string line = "L" + std::to_string(i);
        put_str(text, line.c_str());
    }
    CHECK_EQ(text.line_count(), 1024u);
    CHECK_EQ(text.total_rows(16), 1024u);
    ShellText::Row rows[ShellText::kMaxLines]{};
    CHECK_EQ(text.rows_from(16, 0, ShellText::kMaxLines, rows), 1024u);
    CHECK(row_str(rows[0]) == "L76");
    CHECK(row_str(rows[1023]) == "L1099");
}

}

int main() {
    CHECK_EQ(constinit_probe.line_count(), 1u);
    wraps_at_columns();
    rewrap_wider_joins_rows();
    current_line_cursor_row();
    erase_stops_at_input_start();
    drops_oldest_lines();
    long_line_breaks();
    longest_typed_line_is_one_line();
    rewrap_long_input_at_minimum();
    tail_with_more_rows_than_text();
    total_rows_matches_tail();
    rows_from_top_and_middle();
    rows_from_clamps_past_end();
    keeps_1024_lines();
    return check::finish("test_shell_text");
}
