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

#include "virtio_disk.hpp"

namespace hombre {

namespace {

// The disks a caller may open. The contexts must outlive the call, as each
// Disk's device carries a pointer to one.
constexpr u32 kMaxVirtioDisks = 4;

struct VirtioDisk {
    VirtioBlk* blk;
    u8* bounce;
    u64 bounce_phys;
};

constinit VirtioDisk contexts[kMaxVirtioDisks]{};
constinit u32 context_count = 0;

bool read_sector(void* ctx, u64 sector, u8* out) {
    const VirtioDisk& disk = *static_cast<VirtioDisk*>(ctx);
    if (!disk.blk->read(sector, 1, disk.bounce_phys)) {
        return false;
    }
    __builtin_memcpy(out, disk.bounce, kVirtioBlkSectorSize);
    return true;
}

bool write_sector(void* ctx, u64 sector, const u8* data) {
    const VirtioDisk& disk = *static_cast<VirtioDisk*>(ctx);
    __builtin_memcpy(disk.bounce, data, kVirtioBlkSectorSize);
    return disk.blk->write(sector, 1, disk.bounce_phys);
}

bool flush_disk(void* ctx) {
    return static_cast<VirtioDisk*>(ctx)->blk->flush();
}

}

bool virtio_disk_open(VirtioBlk& blk, PciAddress address, MmioMapper& mmio,
                      PhysAllocator& pages, Disk& out) {
    if (context_count >= kMaxVirtioDisks || !blk.init(address, mmio, pages)) {
        return false;
    }
    const u64 bounce_phys = pages.alloc_pages(1);
    if (bounce_phys == 0) {
        return false;
    }
    u8* bounce = pages.to_ptr(bounce_phys);
    if (bounce == nullptr) {
        return false;
    }

    VirtioDisk& disk = contexts[context_count];
    ++context_count;
    disk.blk = &blk;
    disk.bounce = bounce;
    disk.bounce_phys = bounce_phys;

    const bool read_only = blk.read_only();
    out.dev = SectorDevice{&disk, read_sector, read_only ? nullptr : write_sector, flush_disk};
    out.sectors = blk.sector_count();
    out.read_only = read_only;
    out.kind = "virtio";
    return true;
}

}
