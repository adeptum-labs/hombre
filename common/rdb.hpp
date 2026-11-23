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

// The partitions of an Amiga hard disk image. A partitioned disk has a Rigid
// Disk Block ("RDSK") in one of its first 16 blocks, which heads a chain of
// partition blocks ("PART"), each with a DOS device name and an environment
// vector giving the partition's cylinders, its reserved blocks, its file
// system type and its boot priority. A disk without one that starts with a
// DOS bootblock is a single unpartitioned volume, as UAE's hardfiles are.
//
// Only 512 byte blocks are read: a disk or a partition with any other block
// size is skipped. Every block read is checked against the disk's size and
// every checksum is verified, and the partition chain is bounded, so a damaged
// disk gives fewer partitions or an error and never a stray read.

// Reads one 512 byte sector of the disk; false on an I/O error.
using SectorRead = bool (*)(void* ctx, u32 sector, u8* out);

enum class RdbError : u32 {
    Ok,
    Io,           // a sector could not be read
    NoPartitions, // neither a Rigid Disk Block nor a DOS bootblock
    BadChecksum,  // the Rigid Disk Block fails its checksum
    Unsupported,  // the disk's blocks are not 512 bytes
};

inline const char* rdb_error_name(RdbError error) {
    switch (error) {
    case RdbError::Ok: return "ok";
    case RdbError::Io: return "i/o error";
    case RdbError::NoPartitions: return "no partitions";
    case RdbError::BadChecksum: return "bad rigid disk block checksum";
    case RdbError::Unsupported: return "unsupported block size";
    }
    return "unknown";
}

// The environment vector's longs, as dos keeps them for a device node.
namespace envec {
inline constexpr u32 table_size = 0;
inline constexpr u32 size_block = 1;  // in longs
inline constexpr u32 surfaces = 3;
inline constexpr u32 sectors_per_block = 4;
inline constexpr u32 blocks_per_track = 5;
inline constexpr u32 reserved = 6;
inline constexpr u32 pre_alloc = 7;
inline constexpr u32 low_cyl = 9;
inline constexpr u32 high_cyl = 10;
inline constexpr u32 num_buffers = 11;
inline constexpr u32 buf_mem_type = 12;
inline constexpr u32 max_transfer = 13;
inline constexpr u32 mask = 14;
inline constexpr u32 boot_pri = 15;
inline constexpr u32 dos_type = 16;
inline constexpr u32 longs = 20;
}

struct RdbPartition {
    char name[32];       // the DOS device name, without a colon
    u32 first_block;     // the partition's first sector on the disk
    u32 block_count;
    u32 reserved;        // blocks at the start that hold no files
    u32 dos_type;        // 0x444F5300 to 0x444F5307
    i32 boot_pri;
    bool bootable;
    u32 env[envec::longs];  // the environment vector, missing longs zero
};

struct RdbDisk {
    static constexpr u32 kMaxPartitions = 8;
    bool has_rdb;
    u32 count;
    RdbPartition partition[kMaxPartitions];
    // Partitions that were skipped: marked not to mount, of an unsupported
    // block size, outside the disk, or past kMaxPartitions.
    u32 skipped;
};

namespace rdb_detail {

inline constexpr u32 sector_bytes = 512;
inline constexpr u32 sector_longs = sector_bytes / 4;
inline constexpr u32 search_sectors = 16;
inline constexpr u32 id_rdsk = 0x5244534Bu;
inline constexpr u32 id_part = 0x50415254u;
inline constexpr u32 end_of_list = 0xFFFFFFFFu;
inline constexpr u32 flag_bootable = 1;
inline constexpr u32 flag_no_mount = 2;
inline constexpr u32 dos_type_ofs = 0x444F5300u;

// Long indices in the blocks.
inline constexpr u32 l_id = 0;
inline constexpr u32 l_summed = 1;
inline constexpr u32 rdsk_block_bytes = 4;
inline constexpr u32 rdsk_partition_list = 7;
inline constexpr u32 part_next = 4;
inline constexpr u32 part_flags = 5;
inline constexpr u32 part_name_byte = 9 * 4;  // a BCPL string of up to 31 characters
inline constexpr u32 part_env = 32;

inline u32 be32(const u8* p) {
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

inline u32 long_at(const u8* block, u32 index) { return be32(block + index * 4); }

// The first SummedLongs longs add up to zero.
inline bool sums_to_zero(const u8* block) {
    const u32 summed = long_at(block, l_summed);
    if (summed < 3 || summed > sector_longs) {
        return false;
    }
    u32 sum = 0;
    for (u32 i = 0; i < summed; ++i) {
        sum += long_at(block, i);
    }
    return sum == 0;
}

inline void set_name(char* out, const char* base, u32 number) {
    u32 at = 0;
    for (; base[at] != '\0'; ++at) {
        out[at] = base[at];
    }
    if (number >= 10) {
        out[at++] = static_cast<char>('0' + number / 10);
    }
    out[at++] = static_cast<char>('0' + number % 10);
    out[at] = '\0';
}

// The partition's device name, or "DH<n>" when the block's name is empty or
// holds a character dos cannot take in a device name.
inline void copy_name(const u8* block, u32 index, char* out) {
    const u8* name = block + part_name_byte;
    const u32 length = name[0];
    bool usable = length != 0 && length <= 30;
    for (u32 i = 0; usable && i < length; ++i) {
        const u8 c = name[1 + i];
        usable = c > ' ' && c != ':' && c != '/' && c != 0x7F && c != 0xA0;
    }
    if (!usable) {
        set_name(out, "DH", index);
        return;
    }
    for (u32 i = 0; i < length; ++i) {
        out[i] = static_cast<char>(name[1 + i]);
    }
    out[length] = '\0';
}

// The partition a partition block describes, or false when it cannot be
// mounted here.
inline bool read_partition(const u8* block, u32 index, u32 disk_sectors, RdbPartition& out) {
    if ((long_at(block, part_flags) & flag_no_mount) != 0) {
        return false;
    }
    u32 env[envec::longs]{};
    const u32 table = long_at(block, part_env + envec::table_size);
    const u32 present = table + 1 < envec::longs ? table + 1 : envec::longs;
    for (u32 i = 0; i < present; ++i) {
        env[i] = long_at(block, part_env + i);
    }
    if (table < envec::high_cyl) {
        return false;
    }
    if (table < envec::dos_type) {
        env[envec::dos_type] = dos_type_ofs;
    }
    const u32 sectors_per_block = env[envec::sectors_per_block] == 0 ? 1 : env[envec::sectors_per_block];
    if (env[envec::size_block] != sector_longs || sectors_per_block != 1) {
        return false;
    }
    const u64 cylinder = u64{env[envec::surfaces]} * env[envec::blocks_per_track];
    const u32 low = env[envec::low_cyl];
    const u32 high = env[envec::high_cyl];
    if (cylinder == 0 || cylinder > disk_sectors || high < low) {
        return false;
    }
    // Each product is checked against the disk before the next is formed, so
    // nothing wraps: a cylinder is at most 2^32 sectors and the disk too.
    if (low >= disk_sectors / cylinder + 1) {
        return false;
    }
    const u64 first = u64{low} * cylinder;
    const u64 cylinders = u64{high - low} + 1;
    if (first >= disk_sectors || cylinders > (disk_sectors - first) / cylinder) {
        return false;
    }
    const u64 count = cylinders * cylinder;
    const u32 dos_type = env[envec::dos_type];
    if ((dos_type & 0xFFFFFF00u) != dos_type_ofs) {
        return false;
    }
    const u32 reserved = env[envec::reserved] < 2 ? 2 : env[envec::reserved];
    if (count <= u64{reserved} + 1) {
        return false;
    }
    copy_name(block, index, out.name);
    out.first_block = static_cast<u32>(first);
    out.block_count = static_cast<u32>(count);
    out.reserved = reserved;
    out.dos_type = dos_type;
    out.boot_pri = static_cast<i32>(env[envec::boot_pri]);
    out.bootable = (long_at(block, part_flags) & flag_bootable) != 0;
    for (u32 i = 0; i < envec::longs; ++i) {
        out.env[i] = env[i];
    }
    return true;
}

// A disk without a Rigid Disk Block that starts with a DOS bootblock: one
// partition over all of it, on one surface of one block per track.
inline void whole_disk(const u8* bootblock, u32 disk_sectors, RdbDisk& disk) {
    RdbPartition& p = disk.partition[0];
    set_name(p.name, "DH", 0);
    p.first_block = 0;
    p.block_count = disk_sectors;
    p.reserved = 2;
    p.dos_type = be32(bootblock);
    p.boot_pri = 0;
    p.bootable = true;
    for (u32 i = 0; i < envec::longs; ++i) {
        p.env[i] = 0;
    }
    p.env[envec::table_size] = envec::dos_type;
    p.env[envec::size_block] = sector_longs;
    p.env[envec::surfaces] = 1;
    p.env[envec::sectors_per_block] = 1;
    p.env[envec::blocks_per_track] = 1;
    p.env[envec::reserved] = 2;
    p.env[envec::high_cyl] = disk_sectors - 1;
    p.env[envec::num_buffers] = 30;
    p.env[envec::max_transfer] = 0x7FFFFFFFu;
    p.env[envec::mask] = 0xFFFFFFFEu;
    p.env[envec::dos_type] = p.dos_type;
    disk.count = 1;
}

}

// Finds the partitions of a disk of disk_sectors 512 byte sectors.
inline RdbError rdb_scan(SectorRead read, void* ctx, u32 disk_sectors, RdbDisk& disk) {
    using namespace rdb_detail;
    disk.has_rdb = false;
    disk.count = 0;
    disk.skipped = 0;
    u8 block[sector_bytes];
    u32 rdsk = end_of_list;
    for (u32 s = 0; s < search_sectors && s < disk_sectors; ++s) {
        if (!read(ctx, s, block)) {
            return RdbError::Io;
        }
        if (long_at(block, l_id) == id_rdsk) {
            if (!sums_to_zero(block)) {
                return RdbError::BadChecksum;
            }
            rdsk = s;
            break;
        }
    }
    if (rdsk == end_of_list) {
        if (disk_sectors < 4 || !read(ctx, 0, block)) {
            return disk_sectors < 4 ? RdbError::NoPartitions : RdbError::Io;
        }
        if ((be32(block) & 0xFFFFFF00u) != dos_type_ofs || block[3] > 7) {
            return RdbError::NoPartitions;
        }
        whole_disk(block, disk_sectors, disk);
        return RdbError::Ok;
    }
    disk.has_rdb = true;
    if (long_at(block, rdsk_block_bytes) != sector_bytes) {
        return RdbError::Unsupported;
    }
    // A chain longer than the partitions a disk can hold loops.
    u32 next = long_at(block, rdsk_partition_list);
    u32 index = 0;
    for (u32 step = 0; next != end_of_list && step < 128; ++step) {
        if (next >= disk_sectors || !read(ctx, next, block)) {
            ++disk.skipped;
            break;
        }
        if (long_at(block, l_id) != id_part || !sums_to_zero(block)) {
            ++disk.skipped;
            break;
        }
        if (disk.count < RdbDisk::kMaxPartitions &&
            read_partition(block, index, disk_sectors, disk.partition[disk.count])) {
            ++disk.count;
        } else {
            ++disk.skipped;
        }
        ++index;
        next = long_at(block, part_next);
    }
    return RdbError::Ok;
}

}
