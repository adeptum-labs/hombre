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
#include "trap_frame.hpp"

#include <cstddef>
#include <cstring>

using namespace hombre;

namespace {

bool info_is(u8 vector, const char* mnemonic, const char* name) {
    const ExceptionInfo info = exception_info(vector);
    return std::strcmp(info.mnemonic, mnemonic) == 0 && std::strcmp(info.name, name) == 0;
}

// The entry stubs build this frame on the stack, so its layout is shared
// with the assembly.
void trap_frame_layout_is_pinned() {
    CHECK_EQ(sizeof(TrapFrame), 22u * 8u);
    CHECK_EQ(offsetof(TrapFrame, vector), 120u);
    CHECK_EQ(offsetof(TrapFrame, rip), 136u);
    CHECK_EQ(offsetof(TrapFrame, ss), 168u);
}

void error_code_vectors_match_the_cpu_list() {
    for (unsigned vector = 0; vector < 32; ++vector) {
        const bool expected = vector == 8 || vector == 10 || vector == 11 || vector == 12 ||
                              vector == 13 || vector == 14 || vector == 17 || vector == 21 ||
                              vector == 29 || vector == 30;
        CHECK_EQ(exception_has_error_code(static_cast<u8>(vector)), expected);
    }
}

void exception_names_for_the_common_faults() {
    CHECK(info_is(0, "#DE", "divide error"));
    CHECK(info_is(2, "NMI", "non-maskable interrupt"));
    CHECK(info_is(3, "#BP", "breakpoint"));
    CHECK(info_is(8, "#DF", "double fault"));
    CHECK(info_is(13, "#GP", "general protection fault"));
    CHECK(info_is(14, "#PF", "page fault"));
    CHECK(info_is(18, "#MC", "machine check"));
}

void reserved_vectors_have_a_placeholder() {
    CHECK(info_is(15, "#--", "reserved"));
    CHECK(info_is(31, "#--", "reserved"));
}

void vectors_above_31_are_unknown() {
    CHECK(info_is(32, "#??", "unknown"));
    CHECK(info_is(255, "#??", "unknown"));
}

}

int main() {
    trap_frame_layout_is_pinned();
    error_code_vectors_match_the_cpu_list();
    exception_names_for_the_common_faults();
    reserved_vectors_have_a_placeholder();
    vectors_above_31_are_unknown();
    return check::finish("test_traps");
}
