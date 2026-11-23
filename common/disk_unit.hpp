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

#include "types.hpp"

namespace hombre {

// One unit of scsi.device: a run of 512 byte sectors that programs such as
// HDToolBox may read and, when `write` is set, write. A unit is a whole disk
// that holds an RDB, a bare DOS volume or nothing at all, or one Amiga
// partition of a PC disk; it never reaches past that, so a PC partition
// table and the partitions of other systems stay out of reach. Sector
// numbers are counted from the unit's start.
struct DiskUnit {
    void* ctx;
    bool (*read)(void* ctx, u64 sector, u8* out);
    // Null for a unit that may not be written.
    bool (*write)(void* ctx, u64 sector, const u8* data);
    // Puts the unit's writes on the medium.
    bool (*flush)(void* ctx);
    u64 sectors;
    // What SCSI INQUIRY reports as the product, up to 16 characters.
    char product[17];
};

struct DiskUnits {
    static constexpr u32 kMax = 8;
    u32 count;
    DiskUnit unit[kMax];
};

}
