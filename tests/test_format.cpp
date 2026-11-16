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
#include "format.hpp"

#include <cstring>

using namespace hombre;

namespace {

// Formats into a buffer with guard bytes on both sides so that a write past
// either end is caught by the comparison (and by the sanitizers).
struct Guarded {
    static constexpr usize kGuard = 8;
    char bytes[64 + 2 * kGuard];
    usize capacity;

    explicit Guarded(usize cap) : capacity(cap) { std::memset(bytes, 0x5A, sizeof(bytes)); }

    char* data() { return bytes + kGuard; }

    bool guards_intact() const {
        for (usize i = 0; i < kGuard; ++i) {
            if (bytes[i] != 0x5A || bytes[kGuard + capacity + i] != 0x5A) {
                return false;
            }
        }
        return true;
    }
};

usize format(Guarded& g, u64 value, unsigned base) {
    return format_u64(g.data(), g.capacity, value, base);
}

void formats_zero() {
    Guarded g(8);
    CHECK_EQ(format(g, 0, 10), 1u);
    CHECK(std::strcmp(g.data(), "0") == 0);
    CHECK_EQ(format(g, 0, 16), 1u);
    CHECK(std::strcmp(g.data(), "0") == 0);
    CHECK(g.guards_intact());
}

void formats_decimal() {
    Guarded g(32);
    CHECK_EQ(format(g, 7, 10), 1u);
    CHECK(std::strcmp(g.data(), "7") == 0);
    CHECK_EQ(format(g, 10, 10), 2u);
    CHECK(std::strcmp(g.data(), "10") == 0);
    CHECK_EQ(format(g, 1234567890, 10), 10u);
    CHECK(std::strcmp(g.data(), "1234567890") == 0);
    CHECK(g.guards_intact());
}

void formats_hex_lowercase() {
    Guarded g(32);
    CHECK_EQ(format(g, 0xF, 16), 1u);
    CHECK(std::strcmp(g.data(), "f") == 0);
    CHECK_EQ(format(g, 0xDEADBEEF, 16), 8u);
    CHECK(std::strcmp(g.data(), "deadbeef") == 0);
    CHECK_EQ(format(g, 0xFFFFFFFF80100000ull, 16), 16u);
    CHECK(std::strcmp(g.data(), "ffffffff80100000") == 0);
    CHECK_EQ(format(g, 0x10, 16), 2u);
    CHECK(std::strcmp(g.data(), "10") == 0);
    CHECK(g.guards_intact());
}

void formats_u64_max_both_bases() {
    // Exactly the length plus the terminator.
    Guarded dec(21);
    CHECK_EQ(format(dec, ~0ull, 10), 20u);
    CHECK(std::strcmp(dec.data(), "18446744073709551615") == 0);
    CHECK(dec.guards_intact());

    Guarded hex(17);
    CHECK_EQ(format(hex, ~0ull, 16), 16u);
    CHECK(std::strcmp(hex.data(), "ffffffffffffffff") == 0);
    CHECK(hex.guards_intact());
}

// The serial writers and ErrorLine::append_hex print exactly these strings:
// the shortest form of the value, with no padding and no prefix.
void formats_what_the_writers_print() {
    struct Case {
        u64 value;
        const char* dec;
        const char* hex;
    };
    const Case cases[] = {
        {0, "0", "0"},
        {9, "9", "9"},
        {10, "10", "a"},
        {255, "255", "ff"},
        {0xDEADBEEF, "3735928559", "deadbeef"},
        {~0ull, "18446744073709551615", "ffffffffffffffff"},
    };
    // The buffer sizes the callers use: 21 for decimal, 17 for hex.
    for (const Case& c : cases) {
        Guarded dec(21);
        CHECK_EQ(format(dec, c.value, 10), std::strlen(c.dec));
        CHECK(std::strcmp(dec.data(), c.dec) == 0);
        CHECK(dec.guards_intact());

        Guarded hex(17);
        CHECK_EQ(format(hex, c.value, 16), std::strlen(c.hex));
        CHECK(std::strcmp(hex.data(), c.hex) == 0);
        CHECK(hex.guards_intact());
    }
}

void rejects_small_buffer() {
    // One byte short of the digits plus the terminator.
    Guarded dec(20);
    CHECK_EQ(format(dec, ~0ull, 10), 0u);
    CHECK(dec.guards_intact());
    Guarded hex(16);
    CHECK_EQ(format(hex, ~0ull, 16), 0u);
    CHECK(hex.guards_intact());

    Guarded one(1);
    CHECK_EQ(format(one, 0, 10), 0u);
    CHECK_EQ(format(one, 5, 16), 0u);
    CHECK(one.guards_intact());

    Guarded none(0);
    CHECK_EQ(format(none, 0, 10), 0u);
    CHECK(none.guards_intact());

    CHECK_EQ(format_u64(nullptr, 16, 5, 10), 0u);
    CHECK_EQ(format_u64(nullptr, 0, 5, 10), 0u);
}

void rejects_unsupported_bases() {
    Guarded g(32);
    const unsigned bases[] = {0, 1, 2, 8, 9, 11, 15, 17, 36, 100, 0xFFFFFFFFu};
    for (unsigned base : bases) {
        CHECK_EQ(format(g, 123, base), 0u);
    }
    CHECK(g.guards_intact());
}

}

int main() {
    formats_zero();
    formats_decimal();
    formats_hex_lowercase();
    formats_u64_max_both_bases();
    formats_what_the_writers_print();
    rejects_small_buffer();
    rejects_unsupported_bases();
    return check::finish("test_format");
}
