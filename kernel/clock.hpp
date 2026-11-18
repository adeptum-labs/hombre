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

#pragma once

#include "../common/types.hpp"

namespace hombre {

// The PIT input clock, in Hz. One count lasts 1/1193182 s.
inline constexpr u64 kPitHz = 1193182;

// The TSC rate in kHz for `tsc_delta` cycles measured over `pit_ticks` counts
// of the PIT, 0 when no count was taken. The rate is rounded to the nearest
// kHz, so measurement noise of a few cycles does not move it.
constexpr u64 tsc_khz_from(u64 pit_ticks, u64 tsc_delta) {
    if (pit_ticks == 0) {
        return 0;
    }
    return (tsc_delta * kPitHz + pit_ticks * 500) / (pit_ticks * 1000);
}

struct Deadline {
    u64 end_us;
};

constexpr bool expired_at(Deadline d, u64 now_us) {
    return now_us >= d.end_us;
}

// Microseconds since the successful calibration, 0 while there is none.
u64 clock_us();

// Waits at least `us` microseconds. Returns at once when no rate is known.
void delay_us(u64 us);

// Measures the TSC against a 10 ms one-shot of PIT channel 2. Returns false,
// leaving the clock unset, when the timer does not fire or the rate measured
// twice lies outside 100 MHz to 10 GHz.
bool clock_init();

inline Deadline deadline_in_us(u64 us) {
    return Deadline{clock_us() + us};
}

inline bool expired(Deadline d) {
    return expired_at(d, clock_us());
}

}
