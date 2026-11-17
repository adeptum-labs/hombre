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
#include "printable.hpp"

#include <cstring>

using namespace hombre;

namespace {

void keeps_printable_ascii() {
    char out[40];
    CHECK_EQ(sanitize_text(out, sizeof(out), "Startup-Sequence ~!", 31), 19u);
    CHECK(std::strcmp(out, "Startup-Sequence ~!") == 0);
}

void replaces_control_and_high_bytes() {
    char out[40];
    const char text[] = "x\nhalting\x1b[2J\r\t\x7f\x80\xff";
    const usize n = sanitize_text(out, sizeof(out), text, sizeof(text));
    CHECK_EQ(n, std::strlen(text));
    CHECK(std::strcmp(out, "x.halting.[2J.....") == 0);
    for (usize i = 0; i < n; ++i) {
        CHECK(static_cast<u8>(out[i]) >= 0x20 && static_cast<u8>(out[i]) < 0x7F);
    }
}

void stops_at_nul_and_at_limits() {
    char out[8];
    CHECK_EQ(sanitize_text(out, sizeof(out), "abcdefghij", 31), 7u);
    CHECK(std::strcmp(out, "abcdefg") == 0);
    CHECK_EQ(sanitize_text(out, sizeof(out), "abcdefghij", 3), 3u);
    CHECK(std::strcmp(out, "abc") == 0);
    CHECK_EQ(sanitize_text(out, sizeof(out), "", 31), 0u);
    CHECK(out[0] == '\0');
    // A name without a terminator is cut at in_max and never read past it.
    const char raw[4] = {'a', 'b', 'c', 'd'};
    CHECK_EQ(sanitize_text(out, sizeof(out), raw, sizeof(raw)), 4u);
    CHECK(std::strcmp(out, "abcd") == 0);
}

void tiny_and_missing_buffers() {
    char out[1] = {'z'};
    CHECK_EQ(sanitize_text(out, 1, "abc", 3), 0u);
    CHECK(out[0] == '\0');
    out[0] = 'z';
    CHECK_EQ(sanitize_text(out, 0, "abc", 3), 0u);
    CHECK(out[0] == 'z');
    CHECK_EQ(sanitize_text(nullptr, 8, "abc", 3), 0u);
}

}

int main() {
    keeps_printable_ascii();
    replaces_control_and_high_bytes();
    stops_at_nul_and_at_limits();
    tiny_and_missing_buffers();
    return check::finish("test_printable");
}
