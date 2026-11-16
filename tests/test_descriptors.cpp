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
#include "descriptors.hpp"

#include <cstddef>

using namespace hombre;

// The CPU reads these structures directly, so their layout is part of the
// hardware interface.
static_assert(sizeof(GdtEntry) == 8);
static_assert(sizeof(TssDescriptor) == 16);
static_assert(sizeof(Tss) == 104);
static_assert(offsetof(Tss, ist) == 36);
static_assert(offsetof(Tss, iomap_base) == 102);
static_assert(sizeof(IdtGate) == 16);

namespace {

void code_segment_matches_the_standard_long_mode_value() {
    CHECK_EQ(raw(make_code_segment64()), 0x00AF9A000000FFFFull);
}

void data_segment_matches() {
    CHECK_EQ(raw(make_data_segment()), 0x00CF92000000FFFFull);
}

void tss_descriptor_splits_a_high_base() {
    const TssDescriptor d = make_tss_descriptor(0xFFFFFFFF80123456ull, 0x67);

    CHECK_EQ(d.low.base_low, 0x3456u);
    CHECK_EQ(d.low.base_mid, 0x12u);
    CHECK_EQ(d.low.base_high, 0x80u);
    CHECK_EQ(d.base_upper, 0xFFFFFFFFu);
    CHECK_EQ(d.low.limit_low, 0x67u);
    CHECK_EQ(d.low.access, 0x89u);
    CHECK_EQ(d.low.flags_limit_high, 0u);
    CHECK_EQ(d.reserved, 0u);
}

void tss_limit_above_16_bits_uses_the_high_nibble() {
    const TssDescriptor d = make_tss_descriptor(0, 0x1ABCD);

    CHECK_EQ(d.low.limit_low, 0xABCDu);
    CHECK_EQ(d.low.flags_limit_high, 0x01u);
}

void interrupt_gate_splits_the_handler_address() {
    const IdtGate g = make_interrupt_gate(0xFFFFFFFF80101234ull, 0x08, 1);

    CHECK_EQ(g.offset_low, 0x1234u);
    CHECK_EQ(g.offset_mid, 0x8010u);
    CHECK_EQ(g.offset_high, 0xFFFFFFFFu);
    CHECK_EQ(g.selector, 0x08u);
    CHECK_EQ(g.type_attr, 0x8Eu);
    CHECK_EQ(g.ist, 1u);
    CHECK_EQ(g.reserved, 0u);
}

void gate_ist_is_masked_to_three_bits() {
    CHECK_EQ(make_interrupt_gate(0, 0x08, 0xFF).ist, 7u);
}

void layout_sizes_are_pinned() {
    CHECK_EQ(sizeof(GdtEntry), 8u);
    CHECK_EQ(sizeof(TssDescriptor), 16u);
    CHECK_EQ(sizeof(Tss), 104u);
    CHECK_EQ(sizeof(IdtGate), 16u);
}

void tss_packed_fields_read_back() {
    Tss tss{};
    tss.rsp[0] = 0x1122334455667788ull;
    tss.ist[6] = 0x8877665544332211ull;
    tss.iomap_base = 104;

    // Copies: a reference to a packed member is not allowed.
    const u64 rsp0 = tss.rsp[0];
    const u64 ist6 = tss.ist[6];
    const u16 iomap = tss.iomap_base;
    CHECK_EQ(rsp0, 0x1122334455667788ull);
    CHECK_EQ(ist6, 0x8877665544332211ull);
    CHECK_EQ(iomap, 104u);
}

}

int main() {
    code_segment_matches_the_standard_long_mode_value();
    data_segment_matches();
    tss_descriptor_splits_a_high_base();
    tss_limit_above_16_bits_uses_the_high_nibble();
    interrupt_gate_splits_the_handler_address();
    gate_ist_is_masked_to_three_bits();
    layout_sizes_are_pinned();
    tss_packed_fields_read_back();
    return check::finish("test_descriptors");
}
