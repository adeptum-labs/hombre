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

#include "kernel_load.hpp"
#include "uefi.hpp"

namespace hombre {

// Collects the memory map, the GOP framebuffer and the ACPI RSDP into a
// BootInfo, exits boot services, switches to page tables that identity map
// and direct map physical memory and map the kernel window into the higher
// half, and jumps to the kernel entry on a fresh stack with the BootInfo
// pointer in RDI. Every failure is logged and halts the machine.
[[noreturn]] void handoff_to_kernel(EfiHandle image, EfiSystemTable* st,
                                    const LoadedKernel& kernel);

}
