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

inline constexpr u32 msr_ia32_gs_base = 0xC0000101;

// Data private to one CPU. `self` must stay the first member: this_cpu()
// reads it through %gs:0.
struct PerCpu {
    PerCpu* self;
    u32 cpu_id;
    u32 apic_id;
};

// Points the calling CPU's GS base at cpu so this_cpu() finds it.
inline void percpu_init(PerCpu& cpu, u32 cpu_id) {
    cpu.self = &cpu;
    cpu.cpu_id = cpu_id;
    cpu.apic_id = 0;
    const u64 base = reinterpret_cast<u64>(&cpu);
    asm volatile("wrmsr"
                 :
                 : "c"(msr_ia32_gs_base), "a"(static_cast<u32>(base)),
                   "d"(static_cast<u32>(base >> 32))
                 : "memory");
}

inline PerCpu& this_cpu() {
    PerCpu* cpu;
    asm volatile("mov %%gs:0, %0" : "=r"(cpu));
    return *cpu;
}

}
