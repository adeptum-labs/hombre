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

// A queue between an interrupt and the code it interrupts, on one processor:
// one side only pushes and the other only pops, so neither has to turn
// interrupts off.
template <typename T, u32 N>
class IrqRing {
public:
    static_assert(N != 0 && (N & (N - 1)) == 0, "the counters wrap with N");

    // False when the ring is full.
    bool push(const T& item) {
        const u32 head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        if (head - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE) == N) {
            return false;
        }
        items_[head % N] = item;
        __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
        return true;
    }

    bool pop(T& out) {
        const u32 tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        if (tail == __atomic_load_n(&head_, __ATOMIC_ACQUIRE)) {
            return false;
        }
        out = items_[tail % N];
        __atomic_store_n(&tail_, tail + 1, __ATOMIC_RELEASE);
        return true;
    }

    bool empty() const {
        return __atomic_load_n(&tail_, __ATOMIC_RELAXED) ==
               __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
    }

private:
    T items_[N]{};
    u32 head_ = 0;
    u32 tail_ = 0;
};

}
