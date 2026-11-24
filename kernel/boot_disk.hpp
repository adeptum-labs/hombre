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
#include "mmio.hpp"
#include "page_alloc.hpp"

namespace hombre {

class Dos;

// Finds the boot drive on the PCI bus (the allocator and the MMIO mapper are
// already running, see mem_init), reads the drive, unpacks it when it is a
// DMS archive, mounts the Amiga volume, prints the root directory and
// S/Startup-Sequence on the serial console and runs C:Version from it as
// translated 68000 code (see m68k_run_command). Every failure logs one line
// and returns, so boot continues.
// Call it after the loader's lower-half mapping is gone: it uses the direct
// map only.
void boot_disk_run(const BootInfo& info, PhysAllocator& pages, MmioMapper& mmio);

// Looks at the floppy drives, at most once a second, and gives dos the disk
// of a drive whose medium has changed since it was last read: a different
// disk, none, or one that does not mount. Call it from the kernel's polling;
// it does nothing before boot_disk_run has set the drives up.
void boot_disk_poll_media(Dos& dos);

}
