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
// start of a hard disk image, and writes the MBR and GPT tables that point at
// them, for tests and tools. The kernel never includes this file.

#include "pc_partitions.hpp"
#include "types.hpp"

#include <cstring>
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

// A primary or extended partition table entry: only the fields needed to
// place an image.
struct MbrPartitionSpec {
    u8 type = 0;
    u32 first_lba = 0;
    u32 sectors = 0;
    bool bootable = false;
};

struct GptPartitionSpec {
    u64 first_lba = 0;
    u64 last_lba = 0;
    bool used = true;
};

namespace rdb_writer_detail {

inline constexpr u32 sector_bytes = 512;
inline constexpr u32 summed_longs = 64;
inline constexpr u32 mbr_entry_base = 446;
inline constexpr u32 mbr_entry_bytes = 16;
inline constexpr u32 mbr_entries = 4;
inline constexpr u32 gpt_entry_bytes = 128;
inline constexpr u32 gpt_header_bytes = 92;

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

inline void store_le32(u8* p, u32 value) {
    p[0] = static_cast<u8>(value);
    p[1] = static_cast<u8>(value >> 8);
    p[2] = static_cast<u8>(value >> 16);
    p[3] = static_cast<u8>(value >> 24);
}

inline void store_le64(u8* p, u64 value) {
    store_le32(p, static_cast<u32>(value));
    store_le32(p + 4, static_cast<u32>(value >> 32));
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

// Writes up to four partition entries and the 0x55AA signature at the end of
// the given sector, for a master boot record or an extended boot record.
inline void write_mbr_sector(std::vector<u8>& image, u64 lba,
                             const std::vector<MbrPartitionSpec>& entries,
                             u32 lba_bytes = rdb_writer_detail::sector_bytes) {
    using namespace rdb_writer_detail;
    if (entries.size() > usize{mbr_entries}) {
        throw std::runtime_error("too many MBR entries");
    }
    const usize base = static_cast<usize>(lba) * lba_bytes;
    if (base + sector_bytes > image.size()) {
        throw std::runtime_error("MBR sector outside the image");
    }
    for (u32 slot = 0; slot < mbr_entries; ++slot) {
        u8* entry = image.data() + base + mbr_entry_base + slot * mbr_entry_bytes;
        for (u32 b = 0; b < mbr_entry_bytes; ++b) {
            entry[b] = 0;
        }
        if (slot >= entries.size()) {
            continue;
        }
        const MbrPartitionSpec& spec = entries[slot];
        entry[0] = spec.bootable ? 0x80 : 0x00;
        entry[4] = spec.type;
        store_le32(entry + 8, spec.first_lba);
        store_le32(entry + 12, spec.sectors);
    }
    image[base + 510] = 0x55;
    image[base + 511] = 0xAA;
}

inline void write_mbr(std::vector<u8>& image, const std::vector<MbrPartitionSpec>& entries,
                      u32 lba_bytes = rdb_writer_detail::sector_bytes) {
    write_mbr_sector(image, 0, entries, lba_bytes);
}

inline void write_gpt_header(std::vector<u8>& image, u64 lba, u64 entries_lba,
                             u32 entry_count, u32 entries_crc, u64 my_lba,
                             u64 alternate_lba,
                             u32 lba_bytes = rdb_writer_detail::sector_bytes) {
    using namespace rdb_writer_detail;
    const usize base = static_cast<usize>(lba) * lba_bytes;
    if (base + sector_bytes > image.size()) {
        throw std::runtime_error("GPT header outside the image");
    }
    for (usize i = 0; i < sector_bytes; ++i) {
        image[base + i] = 0;
    }
    const char signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (u32 i = 0; i < 8; ++i) {
        image[base + i] = static_cast<u8>(signature[i]);
    }
    store_le32(image.data() + base + 8, 0x00010000u);
    store_le32(image.data() + base + 12, gpt_header_bytes);
    store_le64(image.data() + base + 24, my_lba);
    store_le64(image.data() + base + 32, alternate_lba);
    store_le64(image.data() + base + 72, entries_lba);
    store_le32(image.data() + base + 80, entry_count);
    store_le32(image.data() + base + 84, gpt_entry_bytes);
    store_le32(image.data() + base + 88, entries_crc);
    store_le32(image.data() + base + 16,
               pc_partitions_detail::crc32(image.data() + base, gpt_header_bytes));
}

// A protective MBR and both GPT headers with their entry arrays: the primary
// header at LBA 1 and its array at LBA 2, the backup header in the last
// sector and its array in the one before it. The entries must fit the first
// entry array sector. LBAs are lba_bytes long, as on a disk with 4096 byte
// sectors.
inline void write_gpt(std::vector<u8>& image, const std::vector<GptPartitionSpec>& entries,
                      u32 lba_bytes = rdb_writer_detail::sector_bytes) {
    using namespace rdb_writer_detail;
    const u64 sectors = image.size() / lba_bytes;
    if (sectors < 8) {
        throw std::runtime_error("disk too small for a GPT");
    }
    if (entries.size() > usize{4}) {
        throw std::runtime_error("too many GPT entries");
    }
    MbrPartitionSpec protective;
    protective.type = 0xEE;
    protective.first_lba = 1;
    protective.sectors =
        static_cast<u32>(sectors - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : sectors - 1);
    write_mbr(image, {protective}, lba_bytes);

    std::vector<u8> array(entries.size() * gpt_entry_bytes, 0);
    for (usize i = 0; i < entries.size(); ++i) {
        if (!entries[i].used) {
            continue;
        }
        u8* entry = array.data() + i * gpt_entry_bytes;
        entry[0] = 0x01;  // Any non-zero type GUID marks the entry as used.
        store_le64(entry + 32, entries[i].first_lba);
        store_le64(entry + 40, entries[i].last_lba);
    }
    const u32 array_crc = pc_partitions_detail::crc32(array.data(), array.size());
    const u64 backup_header = sectors - 1;
    const u64 backup_array = sectors - 2;
    if (!array.empty()) {
        std::memcpy(image.data() + 2 * lba_bytes, array.data(), array.size());
        std::memcpy(image.data() + static_cast<usize>(backup_array) * lba_bytes,
                    array.data(), array.size());
    }
    write_gpt_header(image, 1, 2, static_cast<u32>(entries.size()), array_crc, 1,
                     backup_header, lba_bytes);
    write_gpt_header(image, backup_header, backup_array,
                     static_cast<u32>(entries.size()), array_crc, backup_header, 1, lba_bytes);
}

}
