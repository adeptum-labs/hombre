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

// Host only: lays a Rigid Disk Block and its partition blocks out at the
// start of a hard disk image, for tests and tools. The kernel never
// includes this file.

#include "types.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace hombre {

struct RdbPartitionSpec {
    std::string name = "DH0";
    u32 low_cyl = 0;
    u32 high_cyl = 0;
    u32 dos_type = 0x444F5301u;
    i32 boot_pri = 0;
    bool bootable = true;
    bool no_mount = false;
    u32 reserved = 2;
};

struct RdbGeometry {
    u32 heads = 1;
    u32 sectors = 32;      // per track
    u32 cylinders = 0;     // of the whole disk
    u32 rdsk_block = 0;    // where the Rigid Disk Block goes, 0 to 15
    u32 rdb_cylinders = 2; // cylinders kept for the RDB, before any partition
};

namespace rdb_writer_detail {

inline constexpr u32 sector_bytes = 512;
inline constexpr u32 summed_longs = 64;

inline void put32(std::vector<u8>& image, u32 block, u32 index, u32 value) {
    u8* p = image.data() + static_cast<usize>(block) * sector_bytes + index * 4;
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
}

inline u32 get32(const std::vector<u8>& image, u32 block, u32 index) {
    const u8* p = image.data() + static_cast<usize>(block) * sector_bytes + index * 4;
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

// Stores the checksum that makes the block's first summed_longs longs add up
// to zero.
inline void seal(std::vector<u8>& image, u32 block) {
    put32(image, block, 2, 0);
    u32 sum = 0;
    for (u32 i = 0; i < summed_longs; ++i) {
        sum += get32(image, block, i);
    }
    put32(image, block, 2, 0u - sum);
}

}

inline usize rdb_disk_bytes(const RdbGeometry& g) {
    return static_cast<usize>(g.cylinders) * g.heads * g.sectors * 512;
}

// A zeroed image of the geometry's size with a Rigid Disk Block and one
// partition block per spec, chained in order, in the blocks after it.
inline std::vector<u8> build_rdb_disk(const RdbGeometry& g,
                                      const std::vector<RdbPartitionSpec>& parts) {
    using namespace rdb_writer_detail;
    const u32 cylinder = g.heads * g.sectors;
    if (g.rdsk_block > 15 || g.rdsk_block + 1 + parts.size() > u64{g.rdb_cylinders} * cylinder) {
        throw std::runtime_error("rdb blocks do not fit the reserved cylinders");
    }
    std::vector<u8> image(rdb_disk_bytes(g), 0);
    const u32 rdsk = g.rdsk_block;
    put32(image, rdsk, 0, 0x5244534Bu);
    put32(image, rdsk, 1, summed_longs);
    put32(image, rdsk, 3, 7);
    put32(image, rdsk, 4, sector_bytes);
    put32(image, rdsk, 5, 0x17);
    put32(image, rdsk, 6, 0xFFFFFFFFu);
    put32(image, rdsk, 7, parts.empty() ? 0xFFFFFFFFu : rdsk + 1);
    put32(image, rdsk, 8, 0xFFFFFFFFu);
    put32(image, rdsk, 9, 0xFFFFFFFFu);
    for (u32 i = 10; i < 16; ++i) {
        put32(image, rdsk, i, 0xFFFFFFFFu);
    }
    put32(image, rdsk, 16, g.cylinders);
    put32(image, rdsk, 17, g.sectors);
    put32(image, rdsk, 18, g.heads);
    put32(image, rdsk, 19, 1);
    put32(image, rdsk, 20, g.cylinders);
    put32(image, rdsk, 32, 0);
    put32(image, rdsk, 33, g.rdb_cylinders * cylinder - 1);
    put32(image, rdsk, 34, g.rdb_cylinders);
    put32(image, rdsk, 35, g.cylinders - 1);
    put32(image, rdsk, 36, cylinder);
    put32(image, rdsk, 38, rdsk + static_cast<u32>(parts.size()));
    seal(image, rdsk);

    for (usize n = 0; n < parts.size(); ++n) {
        const RdbPartitionSpec& p = parts[n];
        if (p.name.size() > 30 || p.high_cyl >= g.cylinders || p.low_cyl > p.high_cyl) {
            throw std::runtime_error("bad partition " + p.name);
        }
        const u32 block = rdsk + 1 + static_cast<u32>(n);
        put32(image, block, 0, 0x50415254u);
        put32(image, block, 1, summed_longs);
        put32(image, block, 3, 7);
        put32(image, block, 4, n + 1 < parts.size() ? block + 1 : 0xFFFFFFFFu);
        put32(image, block, 5, (p.bootable ? 1u : 0u) | (p.no_mount ? 2u : 0u));
        u8* name = image.data() + static_cast<usize>(block) * sector_bytes + 36;
        name[0] = static_cast<u8>(p.name.size());
        for (usize i = 0; i < p.name.size(); ++i) {
            name[1 + i] = static_cast<u8>(p.name[i]);
        }
        const u32 env = 32;
        put32(image, block, env + 0, 16);
        put32(image, block, env + 1, 128);
        put32(image, block, env + 3, g.heads);
        put32(image, block, env + 4, 1);
        put32(image, block, env + 5, g.sectors);
        put32(image, block, env + 6, p.reserved);
        put32(image, block, env + 9, p.low_cyl);
        put32(image, block, env + 10, p.high_cyl);
        put32(image, block, env + 11, 30);
        put32(image, block, env + 13, 0x7FFFFFFFu);
        put32(image, block, env + 14, 0xFFFFFFFEu);
        put32(image, block, env + 15, static_cast<u32>(p.boot_pri));
        put32(image, block, env + 16, p.dos_type);
        seal(image, block);
    }
    return image;
}

}
