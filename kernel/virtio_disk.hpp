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

#include "disk.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"
#include "pci.hpp"
#include "virtio_blk.hpp"

namespace hombre {

// Brings a virtio-blk device up as a hard disk. The device copies one sector
// at a time through a bounce page taken from `pages`, which the caller keeps
// allocated for as long as the disk is used. Returns false when the device
// does not start or the bounce page cannot be had.
bool virtio_disk_open(VirtioBlk& blk, PciAddress address, MmioMapper& mmio,
                      PhysAllocator& pages, Disk& out);

}
