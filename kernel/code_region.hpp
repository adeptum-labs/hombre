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

#include "../common/boot_info.hpp"
#include "../common/m68k_jit.hpp"
#include "page_alloc.hpp"

namespace hombre {

// The window the translated code runs from starts at PML4 index 385, next
// to the MMIO window at 384. The loader's layout leaves it free: the direct
// map is at index 256 and the kernel window at 511.
inline constexpr u64 kCodeExecBase = 0xFFFFC08000000000ull;

// Makes a code region for the 68000 translator: bytes, rounded up to whole
// 2 MiB pages, of zeroed physical memory that starts on a 2 MiB boundary.
// Generated code is written through the direct map and run from
// kCodeExecBase, where the pages are mapped present, read-only and
// executable. The loader maps the direct map and the kernel window without
// NX, so the region's 2 MiB pages in both are made no-execute first: no
// address is ever both writable and executable for these pages.
//
// Call it once per boot, after mem_init (NX must be on) and after the lower
// half is gone. Returns false when bytes is 0 or above 1 GiB, no aligned
// run is free or the tables cannot be changed. The pages go back to the
// allocator unless the window may already map some of them.
bool code_region_create(const BootInfo& info, PhysAllocator& pages, u64 bytes,
                        m68k::CodeRegion& out);

}
