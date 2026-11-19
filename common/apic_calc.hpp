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

#include "types.hpp"

namespace hombre {

inline constexpr u32 kApicMinInitialCount = 1;
inline constexpr u32 kApicMaxInitialCount = 0xFFFFFFFFu;
inline constexpr u64 kMicrosecondsPerSecond = 1000000ull;

namespace apic_calc_detail {

constexpr u32 clamp_count(u64 count, u32 min_count, u32 max_count) {
    if (count < min_count) {
        return min_count;
    }
    if (count > max_count) {
        return max_count;
    }
    return static_cast<u32>(count);
}

}  // namespace apic_calc_detail

// Computes the LAPIC timer initial count for `target_hz` interrupts per
// second from `elapsed_ticks` observed over `interval_us` microseconds,
// rounded to the nearest integer and clamped to [min_count, max_count].
// Returns 0 on invalid input (any zero parameter or min_count > max_count).
constexpr u32 apic_timer_initial_count(u64 elapsed_ticks, u64 interval_us, u32 target_hz,
                                       u32 min_count = kApicMinInitialCount,
                                       u32 max_count = kApicMaxInitialCount) {
    if (elapsed_ticks == 0 || interval_us == 0 || target_hz == 0 || min_count > max_count) {
        return 0;
    }

    // Scale both elapsed_ticks and interval_us together if interval_us * target_hz
    // would overflow u64.
    while (interval_us > (~u64{0}) / target_hz) {
        elapsed_ticks >>= 1;
        interval_us >>= 1;
        if (elapsed_ticks == 0) {
            return min_count;
        }
        if (interval_us == 0) {
            return max_count;
        }
    }

    const u64 denom = interval_us * target_hz;
    const u64 q = elapsed_ticks / denom;
    if (q > max_count / kMicrosecondsPerSecond) {
        return max_count;
    }

    const u64 base = q * kMicrosecondsPerSecond;
    const u64 r = elapsed_ticks % denom;
    u64 rem = 0;
    if (r <= (~u64{0} - denom / 2) / kMicrosecondsPerSecond) {
        rem = (r * kMicrosecondsPerSecond + denom / 2) / denom;
    } else {
        // Only reachable when denom > ~1.84e13; shifting by 20 bits keeps the
        // denominator large while avoiding 64-bit multiplication overflow.
        const u64 d = denom >> 20;
        const u64 n = r >> 20;
        rem = (n * kMicrosecondsPerSecond + d / 2) / d;
    }

    if (base > (~u64{0}) - rem) {
        return max_count;
    }
    return apic_calc_detail::clamp_count(base + rem, min_count, max_count);
}

}
