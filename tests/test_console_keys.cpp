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
#include "console_keys.hpp"

using namespace hombre;

namespace {

ConsoleKey feed_seq(CsiFilter& filter, const u8* bytes, usize count, u8& out) {
    ConsoleKey result = ConsoleKey::None;
    for (usize i = 0; i < count; ++i) {
        result = filter.feed(bytes[i], out);
        if (i + 1 < count) {
            CHECK_EQ(result, ConsoleKey::None);
        }
    }
    return result;
}

void plain_bytes_pass() {
    CsiFilter filter;
    const u8 inputs[] = {'a', 'Z', '0', '\n', 0x08, 0x1B, 0xE5, 0xF6};
    for (u8 b : inputs) {
        u8 out = 0;
        CHECK_EQ(filter.feed(b, out), ConsoleKey::Byte);
        CHECK_EQ(out, b);
    }
}

void scroll_sequences_map_to_actions() {
    CsiFilter filter;
    u8 out = 0;

    const u8 row_up[] = {0x9B, 'T'};
    CHECK_EQ(feed_seq(filter, row_up, sizeof(row_up), out), ConsoleKey::RowUp);

    const u8 row_down[] = {0x9B, 'S'};
    CHECK_EQ(feed_seq(filter, row_down, sizeof(row_down), out), ConsoleKey::RowDown);

    const u8 page_up[] = {0x9B, '4', '1', '~'};
    CHECK_EQ(feed_seq(filter, page_up, sizeof(page_up), out), ConsoleKey::PageUp);

    const u8 page_down[] = {0x9B, '4', '2', '~'};
    CHECK_EQ(feed_seq(filter, page_down, sizeof(page_down), out), ConsoleKey::PageDown);

    const u8 top[] = {0x9B, '5', '4', '~'};
    CHECK_EQ(feed_seq(filter, top, sizeof(top), out), ConsoleKey::Top);

    const u8 bottom[] = {0x9B, '5', '5', '~'};
    CHECK_EQ(feed_seq(filter, bottom, sizeof(bottom), out), ConsoleKey::Bottom);

    CHECK_EQ(filter.feed('k', out), ConsoleKey::Byte);
    CHECK_EQ(out, 'k');
}

void other_sequences_are_swallowed() {
    CsiFilter filter;
    u8 out = 0;

    const u8 up[] = {0x9B, 'A'};
    CHECK_EQ(feed_seq(filter, up, sizeof(up), out), ConsoleKey::Swallowed);

    const u8 home[] = {0x9B, '4', '4', '~'};
    CHECK_EQ(feed_seq(filter, home, sizeof(home), out), ConsoleKey::Swallowed);

    const u8 max_len[] = {0x9B, '1', '2', '3', '4', '5', 'm'};
    CHECK_EQ(feed_seq(filter, max_len, sizeof(max_len), out), ConsoleKey::Swallowed);

    CHECK_EQ(filter.feed('y', out), ConsoleKey::Byte);
    CHECK_EQ(out, 'y');
}

void partial_sequence_is_dropped() {
    CsiFilter filter;
    u8 out = 0;

    CHECK_EQ(filter.feed(0x9B, out), ConsoleKey::None);
    CHECK_EQ(filter.feed('4', out), ConsoleKey::None);
    filter.reset();
    CHECK_EQ(filter.feed('x', out), ConsoleKey::Byte);
    CHECK_EQ(out, 'x');

    CHECK_EQ(filter.feed(0x9B, out), ConsoleKey::None);
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(filter.feed('1', out), ConsoleKey::None);
    }
    CHECK_EQ(filter.feed('2', out), ConsoleKey::Swallowed);

    CHECK_EQ(filter.feed('z', out), ConsoleKey::Byte);
    CHECK_EQ(out, 'z');
}

}

int main() {
    plain_bytes_pass();
    scroll_sequences_map_to_actions();
    other_sequences_are_swallowed();
    partial_sequence_is_dropped();
    return check::finish("test_console_keys");
}
