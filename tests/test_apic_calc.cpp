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

#include "apic_calc.hpp"
#include "check.hpp"

using namespace hombre;

namespace {

void invalid_inputs_return_zero() {
    CHECK_EQ(apic_timer_initial_count(0, 10000, 1000), 0u);
    CHECK_EQ(apic_timer_initial_count(100000, 0, 1000), 0u);
    CHECK_EQ(apic_timer_initial_count(100000, 10000, 0), 0u);
    CHECK_EQ(apic_timer_initial_count(100000, 10000, 1000, 100, 50), 0u);
}

void exact_1000hz_calculation() {
    // 62.5 MHz timer (1 GHz bus / 16): 625,000 ticks in 10 ms -> 62,500 per 1 ms tick.
    CHECK_EQ(apic_timer_initial_count(625000, 10000, 1000), 62500u);
    // 100 MHz bus / 16 = 6.25 MHz: 62,500 ticks in 10 ms -> 6,250 per 1 ms tick.
    CHECK_EQ(apic_timer_initial_count(62500, 10000, 1000), 6250u);
    // 1 Hz target over 1 s interval.
    CHECK_EQ(apic_timer_initial_count(123456, 1000000, 1), 123456u);
}

void rounds_to_nearest_count() {
    // interval_us = 1000, target_hz = 1000 -> denom = 1,000,000, so count == elapsed_ticks.
    // Test half-up rounding with interval_us = 2000, target_hz = 1000 -> count = elapsed_ticks / 2.
    CHECK_EQ(apic_timer_initial_count(200, 2000, 1000), 100u);
    CHECK_EQ(apic_timer_initial_count(201, 2000, 1000), 101u);

    // With interval_us = 3000, target_hz = 1000 -> denom = 3,000,000 -> count = elapsed_ticks / 3.
    // 301 / 3 = 100.333... -> 100; 302 / 3 = 100.666... -> 101.
    CHECK_EQ(apic_timer_initial_count(301, 3000, 1000), 100u);
    CHECK_EQ(apic_timer_initial_count(302, 3000, 1000), 101u);
}

void clamps_to_min_and_max() {
    // Very small tick rate rounds below min_count -> clamped to default min (1).
    CHECK_EQ(apic_timer_initial_count(1, 1000000, 1000), 1u);
    // Custom min clamp.
    CHECK_EQ(apic_timer_initial_count(50, 10000, 1000, 100, 10000), 100u);
    // Custom max clamp.
    CHECK_EQ(apic_timer_initial_count(500000, 10000, 1000, 1, 10000), 10000u);
    // Overflow past 32-bit max -> clamped to 0xFFFFFFFF.
    CHECK_EQ(apic_timer_initial_count(0xFFFFFFFFull, 10, 1), 0xFFFFFFFFu);
}

void handles_large_values_without_overflow() {
    // Huge elapsed_ticks that would overflow elapsed_ticks * 1e6.
    CHECK_EQ(apic_timer_initial_count(1ull << 50, 10000, 1000), 0xFFFFFFFFu);
    // Both elapsed_ticks and interval_us huge with reasonable ratio.
    const u64 large = 1ull << 54;
    CHECK_EQ(apic_timer_initial_count(large, large / 1000, 1000), 1000000u);
    // Huge interval_us that would overflow interval_us * target_hz.
    CHECK_EQ(apic_timer_initial_count(1000, ~u64{0} / 2, 1000), 1u);
}

}  // namespace

int main() {
    invalid_inputs_return_zero();
    exact_1000hz_calculation();
    rounds_to_nearest_count();
    clamps_to_min_and_max();
    handles_large_values_without_overflow();
    return check::finish("test_apic_calc");
}
