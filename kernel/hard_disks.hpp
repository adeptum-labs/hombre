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
#include "../common/disk_unit.hpp"
#include "../common/rdb.hpp"
#include "disk.hpp"
#include "page_alloc.hpp"

namespace hombre {

// A mounted hard disk partition: its volume, the dos device name it is
// given, and the partition data dos needs to make a device node of it later.
struct HardVolume {
    AdfVolume volume;
    char device[32];
    i32 boot_pri;
    bool bootable;
    bool writable;
    u32 env[envec::longs];
    // The scsi.device unit that holds the partition, ~0u for none.
    u32 unit;
};

constexpr u32 kMaxHardDisks = 8;
constexpr u32 kMaxHardVolumes = 8;

// Scans each disk for Amiga partitions, reads them through a sector cache
// and mounts their volumes, logging what it finds. At most `max` volumes, and
// never more than kMaxHardVolumes, are stored in `out`; the disk's caches
// stay allocated for as long as the volumes are used. Each stretch of a disk
// that belongs to the Amiga becomes a scsi.device unit in `units`, read and
// written through the same cache. Disk entries without a read device, such
// as slots that hold no disk, are skipped. Returns the number of volumes
// mounted.
u32 hard_disks_mount(const Disk* disks, u32 disk_count, PhysAllocator& pages, HardVolume* out,
                     u32 max, DiskUnits& units);

}
