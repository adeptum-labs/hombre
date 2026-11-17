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

// Loads the kernel's IDT on the calling CPU. Every vector reaches the trap
// dispatcher: a breakpoint returns, anything else dumps the CPU state on
// serial and halts. The kernel GDT and TSS must already be loaded, since the
// gates use the kernel code selector and the IST stacks.
void idt_init_bsp();

// True when the CPU's IDT register points at the kernel's table.
bool idt_loaded();

// How many breakpoint traps the dispatcher has handled so far.
u64 trap_breakpoint_count();

}
