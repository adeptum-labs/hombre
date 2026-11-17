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
#include "m68k_cpu.hpp"

using namespace hombre;
using namespace hombre::m68k;

namespace {

void reads_are_big_endian() {
    u8 memory[64] = {0x12, 0x34, 0x56, 0x78};
    GuestMemory guest{memory, 64};
    u32 value = 0;
    CHECK(guest.read32(0, value));
    CHECK_EQ(value, 0x12345678u);
    CHECK(guest.read16(0, value));
    CHECK_EQ(value, 0x1234u);
}

void writes_are_big_endian() {
    u8 memory[64] = {};
    GuestMemory guest{memory, 64};
    CHECK(guest.write16(2, 0xABCD));
    CHECK_EQ(memory[2], 0xABu);
    CHECK_EQ(memory[3], 0xCDu);
    CHECK(guest.write32(4, 0xDEADBEEF));
    CHECK_EQ(memory[4], 0xDEu);
    CHECK_EQ(memory[5], 0xADu);
    CHECK_EQ(memory[6], 0xBEu);
    CHECK_EQ(memory[7], 0xEFu);
}

void misaligned_accesses_fail_without_touching_anything() {
    u8 memory[64] = {};
    GuestMemory guest{memory, 64};
    u32 value = 0xDEADBEEFu;
    CHECK(!guest.read16(1, value));
    CHECK_EQ(value, 0xDEADBEEFu);
    CHECK(!guest.read32(3, value));
    CHECK_EQ(value, 0xDEADBEEFu);
    CHECK(!guest.write16(3, 1));
    CHECK(!guest.write32(7, 1));
    for (u32 i = 0; i < 64; ++i) {
        CHECK_EQ(memory[i], 0u);
    }
}

void accesses_stop_at_the_end_of_memory() {
    u8 memory[64] = {};
    GuestMemory guest{memory, 64};
    u32 value = 0;
    CHECK(guest.read8(63, value));
    CHECK(!guest.read8(64, value));
    CHECK(!guest.read32(62, value));
    CHECK(guest.read16(62, value));
}

void ranges_are_checked_without_wrapping() {
    u8 memory[64] = {};
    GuestMemory guest{memory, 64};
    CHECK(!guest.range_ok(0xFFFFFFFEu, 4));
    CHECK(guest.range_ok(0, 64));
    CHECK(!guest.range_ok(0, 65));
    CHECK(guest.range_ok(64, 0));
}

}

int main() {
    reads_are_big_endian();
    writes_are_big_endian();
    misaligned_accesses_fail_without_touching_anything();
    accesses_stop_at_the_end_of_memory();
    ranges_are_checked_without_wrapping();
    return check::finish("test_m68k_cpu");
}
