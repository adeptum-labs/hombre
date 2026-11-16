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
#include "uefi.hpp"

namespace hombre {

struct LoadedKernel {
    u64 entry_virt;
    // Page-rounded physical range claimed for the kernel: [phys_start, phys_end).
    u64 phys_start;
    u64 phys_end;
};

// Reads \KERNEL.ELF from the volume the loader was started from and places
// its PT_LOAD segments at their physical addresses. Every failure is logged
// to the serial port and the firmware console, and its status is returned.
EfiStatus load_kernel(EfiHandle image, EfiSystemTable* st, LoadedKernel& out);

}
