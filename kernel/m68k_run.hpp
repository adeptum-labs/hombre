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

#include "../common/amiga_fs.hpp"
#include "../common/boot_info.hpp"
#include "page_alloc.hpp"

namespace hombre {

// Runs the 68000 program at `path` on the mounted volume with the argument
// line `args`, as translated code over exec and dos. The guest gets 16 MiB
// of memory from `pages`, seen through a 24-bit address bus as on a 68000,
// and the run stops after 2^24 blocks.
//
// The serial console shows "m68k: running <path>", the program's output
// (bytes outside printable ASCII other than newline and tab show as '.'),
// then "m68k: <path> returned <d0>", or "m68k: <exit reason> at pc=0x<pc>"
// ("budget exhausted" for the block budget), or "m68k: could not load
// <path>". Diagnostics of the native libraries appear as lines of their
// own. A setup step that fails logs one "m68k: " line. Always returns.
//
// Call it once per boot, after mem_init and after the lower half is gone.
// The volume must stay mounted, and its image in memory, for good.
void m68k_run_command(const BootInfo& info, PhysAllocator& pages, const AdfVolume& volume,
                      const char* path, const char* args);

}
