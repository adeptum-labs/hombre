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
#include "clock.hpp"

using namespace hombre;

namespace {

void tsc_khz_from_exact_values() {
    // 11932 counts of the PIT last 10.00015 ms, so 30 million cycles over
    // them are just under 3 GHz: within 1 MHz of it.
    const u64 khz = tsc_khz_from(11932, 30000000);
    CHECK(khz >= 2999000 && khz <= 3001000);
}

void zero_ticks_gives_zero() {
    CHECK_EQ(tsc_khz_from(0, 30000000), 0u);
    CHECK_EQ(tsc_khz_from(0, 0), 0u);
}

void rounding_is_stable() {
    // The rate is rounded to the nearest kHz, so a few cycles of calibration
    // noise do not move it.
    const u64 khz = tsc_khz_from(11932, 30000000);
    CHECK_EQ(tsc_khz_from(11932, 30000002), khz);
    CHECK_EQ(tsc_khz_from(11932, 29999998), khz);
    // One microsecond of PIT time is 1.193182 counts, and a rate of 11932
    // cycles per count is 11932 kHz.
    CHECK_EQ(tsc_khz_from(11932, 119320), 11932u);
}

void expired_compares_against_a_given_now() {
    const Deadline d{1000};
    CHECK(!expired_at(d, 0));
    CHECK(!expired_at(d, 999));
    CHECK(expired_at(d, 1000));
    CHECK(expired_at(d, 1001));
    CHECK(expired_at(Deadline{0}, 0));
}

}

int main() {
    tsc_khz_from_exact_values();
    zero_ticks_gives_zero();
    rounding_is_stable();
    expired_compares_against_a_given_now();
    return check::finish("test_clock");
}
