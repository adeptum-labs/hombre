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

inline constexpr u16 kKernelCodeSelector = 0x08;
inline constexpr u16 kKernelDataSelector = 0x10;
inline constexpr u16 kTssSelector = 0x18;

// IST slots, numbered from 1 as in an IDT gate.
inline constexpr u8 kIstDoubleFault = 1;
inline constexpr u8 kIstNmi = 2;
inline constexpr u8 kIstMachineCheck = 3;

// Replaces the firmware's GDT with the kernel's own and loads the TSS.
// Segment registers are reloaded except FS and GS, so a GS base set before
// the call survives. The GDT and TSS are shared and a second ltr faults on a
// busy TSS: call once, on the boot CPU, with interrupts off.
void gdt_init_bsp();

inline u16 read_cs() {
    u16 selector;
    asm volatile("mov %%cs, %0" : "=r"(selector));
    return selector;
}

inline u16 read_tr() {
    u16 selector;
    asm volatile("str %0" : "=r"(selector));
    return selector;
}

}
