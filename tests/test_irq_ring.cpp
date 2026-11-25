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

#include "../common/irq_ring.hpp"

#include "check.hpp"

using namespace hombre;

namespace {

void entries_come_out_in_order() {
    IrqRing<u32, 4> ring;
    u32 out = 0;
    CHECK(!ring.pop(out));
    CHECK(ring.push(1));
    CHECK(ring.push(2));
    CHECK(ring.pop(out));
    CHECK_EQ(out, 1u);
    CHECK(ring.pop(out));
    CHECK_EQ(out, 2u);
    CHECK(!ring.pop(out));
}

void a_full_ring_refuses_more() {
    IrqRing<u32, 4> ring;
    for (u32 i = 0; i < 4; ++i) {
        CHECK(ring.push(i));
    }
    CHECK(!ring.push(4));
    u32 out = 0;
    CHECK(ring.pop(out));
    CHECK_EQ(out, 0u);
    CHECK(ring.push(4));
}

void it_goes_round_many_times() {
    IrqRing<u32, 4> ring;
    for (u32 i = 0; i < 100; ++i) {
        u32 out = 0;
        CHECK(ring.push(i));
        CHECK(ring.pop(out));
        CHECK_EQ(out, i);
    }
    CHECK(ring.empty());
}

}

int main() {
    entries_come_out_in_order();
    a_full_ring_refuses_more();
    it_goes_round_many_times();
    return check::finish("test_irq_ring");
}
