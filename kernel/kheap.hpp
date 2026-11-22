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

#include "page_alloc.hpp"

namespace hombre {

// Starts the kernel heap behind operator new and delete. It grows in runs of
// pages from `pages`, reached through the direct map at direct_map_base, so
// both must outlive the kernel. Call it once, after the page allocator is
// initialised and before anything allocates; an allocation before that, or
// one that finds no memory, prints a message and halts the kernel.
void kheap_init(PhysAllocator& pages, u64 direct_map_base);

}
