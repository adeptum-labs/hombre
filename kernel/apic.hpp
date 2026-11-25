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
#include "mmio.hpp"

namespace hombre {

inline constexpr u8 kApicTimerVector = 0x40;
inline constexpr u8 kApicSpuriousVector = 0xFF;
inline constexpr u32 kApicTimerHz = 1000;
inline constexpr u64 kApicReschedTicks = 4;

inline void irq_enable() {
    asm volatile("sti" : : : "memory");
}

inline void irq_disable() {
    asm volatile("cli" : : : "memory");
}

inline u64 irq_save() {
    u64 flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

inline void irq_restore(u64 flags) {
    if ((flags & (1ull << 9)) != 0) {
        irq_enable();
    }
}

// Remaps the legacy 8259 PICs to vectors 0x20..0x2F and masks every IRQ line.
void pic_mask_all();

// Detects and enables the local APIC (preferring x2APIC over xAPIC MMIO),
// calibrates its timer against the TSC clock, and starts a 1000 Hz periodic
// interrupt on vector 0x40. Leaves interrupts disabled; returns false when no
// APIC is present or calibration fails.
bool apic_init(MmioMapper& mmio);

// True once the LAPIC periodic timer has been started.
bool apic_active();

// Updates tick counters and sends EOI for the periodic timer vector.
void apic_handle_timer_irq();

u64 timer_ticks();

// Atomically reads and clears the reschedule flag set every four timer ticks.
bool take_need_resched();

// Enables interrupts and halts until the next interrupt when the APIC timer is
// active, or issues a single pause instruction otherwise.
void idle_wait();

// Whether the interrupted code waits halted in idle_wait.
bool cpu_halted();

}
