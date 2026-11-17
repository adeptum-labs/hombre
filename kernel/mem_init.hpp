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

// Early memory setup, once per boot and after the loader's lower-half
// mapping is gone: starts the physical page allocator on the loader's memory
// map, turns on no-execute (the MMIO window needs it) and starts the MMIO
// mapper on the live page tables. Logs one line and returns false when a step
// fails. Both objects have to stay alive for as long as they are used.
bool mem_init(const BootInfo& info, PhysAllocator& pages, MmioMapper& mmio);

}
