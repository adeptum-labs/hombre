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

namespace hombre {

// Brings up the page allocator, the MMIO window and the boot drive, reads
// the drive, unpacks it when it is a DMS archive, mounts the Amiga volume
// and prints the root directory and S/Startup-Sequence on the serial
// console. Every failure logs one line and returns, so boot continues.
// Call it after the loader's lower-half mapping is gone: it uses the direct
// map only.
void boot_disk_run(const BootInfo& info);

}
