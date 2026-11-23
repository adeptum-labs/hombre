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

#include "rdb.hpp"

namespace hombre {

// Amiga partitions on a PC disk. A Rigid Disk Block can sit at the start of
// the whole disk, inside an MBR partition of type 0x30 (AROS) or 0x76
// (Amithlon), or inside any GPT partition. A disk with a valid PC table is
// read through that table only, so an RDB elsewhere on it is not mistaken for
// the disk's own; a disk without one is scanned as a whole.
//
// MBR and GPT are little-endian, the Amiga blocks big-endian. Drivers present
// every disk as 512 byte sectors, but MBR and GPT count in the disk's own
// logical sectors: on a disk with 4096 byte sectors the GPT header sits at
// byte 4096 and every LBA in the tables is eight of our sectors. The RDB
// inside a partition is read in 512 byte sectors. Every table read is
// bounds-checked, both
// copies of a GPT are tried and checked with their CRC32s, and the MBR
// extended chain and the GPT entry array are bounded, so a damaged disk gives
// fewer partitions or an error and never a stray read.

// Reads one 512 byte sector of the whole disk, which may be larger than the
// 2 TiB a u32 sector number reaches; false on an I/O error.
using DiskRead = bool (*)(void* ctx, u64 sector, u8* out);

enum class PcTable : u8 {
    None,  // no partition table was found or understood
    Mbr,
    Gpt,
};

enum class PartitionSource : u8 {
    WholeDisk,
    Mbr,
    Gpt,
};

struct AmigaPartition {
    RdbPartition part;
    u64 disk_first_sector;  // part.first_block counted from the disk's start
    PartitionSource source; // the container the partition was found in
};

struct AmigaPartitions {
    static constexpr u32 kMax = 16;
    u32 count;
    AmigaPartition list[kMax];
    // Partitions skipped: of an unsupported block size, outside their
    // container, too large for a 32 bit sector number, or past kMax.
    u32 skipped;
    PcTable table;
};

namespace pc_partitions_detail {

inline constexpr u32 sector_bytes = 512;
inline constexpr u32 mbr_entry_base = 446;
inline constexpr u32 mbr_entry_bytes = 16;
inline constexpr u32 mbr_entries = 4;
inline constexpr u8 mbr_bootable = 0x80;
inline constexpr u8 mbr_type_gpt = 0xEE;
inline constexpr u8 mbr_type_aros = 0x30;
inline constexpr u8 mbr_type_amithlon = 0x76;
inline constexpr u8 mbr_type_extended = 0x05;
inline constexpr u8 mbr_type_extended_lba = 0x0F;
inline constexpr u8 mbr_type_linux_extended = 0x85;
inline constexpr u32 max_ebr_chain = 64;
inline constexpr u32 max_gpt_entries = 1024;
inline constexpr u32 min_gpt_entry_bytes = 128;
// An entry may straddle one sector boundary, which lets an entry be read
// without keeping the whole entry array in memory. Real tables use 128.
inline constexpr u32 max_gpt_entry_bytes = 512;
inline constexpr u32 gpt_header_bytes = 92;

// The disk as its partition tables see it: LBAs of `scale` 512 byte sectors.
struct TableDisk {
    DiskRead read;
    void* ctx;
    u32 scale;
    u64 lbas;

    // Reads the 512 byte sector `part` of logical sector `lba`.
    bool read_lba(u64 lba, u8* out, u64 part = 0) const {
        return read(ctx, lba * scale + part, out);
    }
};

enum class GptRead : u8 {
    Ok,
    Bad,
    Io,
};

// CRC32 as IEEE 802.3 uses it: reflected, polynomial 0xEDB88320, preset and
// final xor 0xFFFFFFFF. A table-free bit loop keeps this header small.
inline u32 crc32_update(u32 crc, const u8* data, usize bytes) {
    for (usize i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (u32 bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

inline u32 crc32(const u8* data, usize bytes) {
    return crc32_update(0xFFFFFFFFu, data, bytes) ^ 0xFFFFFFFFu;
}

inline u32 le32(const u8* p) {
    return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
}

inline u64 le64(const u8* p) { return u64{le32(p)} | (u64{le32(p + 4)} << 32); }

inline bool mbr_signature(const u8* sector) {
    return sector[510] == 0x55 && sector[511] == 0xAA;
}

inline bool mbr_amiga(const u8* entry) {
    return entry[4] == mbr_type_aros || entry[4] == mbr_type_amithlon;
}

inline bool mbr_extended(const u8* entry) {
    const u8 type = entry[4];
    return type == mbr_type_extended || type == mbr_type_extended_lba ||
           type == mbr_type_linux_extended;
}

inline bool mbr_protective(const u8* sector) {
    for (u32 i = 0; i < mbr_entries; ++i) {
        if (sector[mbr_entry_base + i * mbr_entry_bytes + 4] == mbr_type_gpt) {
            return true;
        }
    }
    return false;
}

// The table of a disk that says it has one: every used entry has a boot flag
// MBRs allow, a type, a nonzero start, and lies inside the disk. Unused
// entries are all zero and take no part.
inline bool valid_mbr(const u8* sector, u64 disk_lbas) {
    for (u32 i = 0; i < mbr_entries; ++i) {
        const u8* entry = sector + mbr_entry_base + i * mbr_entry_bytes;
        if (entry[4] == 0) {
            continue;
        }
        const u32 first = le32(entry + 8);
        if ((entry[0] != 0x00 && entry[0] != mbr_bootable) || first == 0 ||
            u64{first} + le32(entry + 12) > disk_lbas) {
            return false;
        }
    }
    return true;
}

// Sector 0 of a disk without a table may hold an RDB or a DOS bootblock; an
// RDB counts only when its checksum holds.
inline bool looks_like_amiga_volume(const u8* sector) {
    if (rdb_detail::long_at(sector, rdb_detail::l_id) == rdb_detail::id_rdsk) {
        return rdb_detail::sums_to_zero(sector);
    }
    return (rdb_detail::be32(sector) & 0xFFFFFF00u) == rdb_detail::dos_type_ofs &&
           sector[3] <= 7;
}

// Reads a PC partition as if it were a disk of its own.
struct ContainerRead {
    DiskRead read;
    void* ctx;
    u64 first;
    u32 sectors;
};

inline bool read_container(void* ctx, u32 sector, u8* out) {
    const ContainerRead& container = *static_cast<ContainerRead*>(ctx);
    if (sector >= container.sectors) {
        return false;
    }
    return container.read(container.ctx, container.first + sector, out);
}

// Partitions of one RDB, moved to their place on the disk. Partitions past
// kMax are counted as skipped.
inline void add_partitions(const RdbDisk& disk, u64 base, PartitionSource source,
                           AmigaPartitions& out) {
    for (u32 i = 0; i < disk.count; ++i) {
        if (out.count >= AmigaPartitions::kMax) {
            ++out.skipped;
            continue;
        }
        AmigaPartition& partition = out.list[out.count++];
        partition.part = disk.partition[i];
        partition.disk_first_sector = base + disk.partition[i].first_block;
        partition.source = source;
    }
}

// Looks for a Rigid Disk Block in one PC partition. Its partitions are
// relative to the partition's start, so the container's first sector is added
// to each. A bare DOS volume is not a partitioned disk and is ignored.
inline void scan_container(DiskRead read, void* ctx, u64 first, u64 sectors,
                           PartitionSource source, AmigaPartitions& out) {
    if (sectors == 0) {
        return;
    }
    if (sectors > 0xFFFFFFFFu) {
        ++out.skipped;
        return;
    }
    ContainerRead container{read, ctx, first, static_cast<u32>(sectors)};
    RdbDisk disk;
    const RdbError error = rdb_scan(read_container, &container, container.sectors, disk);
    if (error == RdbError::Io || error == RdbError::BadChecksum ||
        error == RdbError::Unsupported) {
        ++out.skipped;
        return;
    }
    if (error != RdbError::Ok) {
        return;
    }
    out.skipped += disk.skipped;
    if (disk.has_rdb) {
        add_partitions(disk, first, source, out);
    }
}

// A disk without a PC table: an RDB at its start, or a bare DOS volume, over
// as much of it as a 32 bit sector number reaches.
inline RdbError scan_whole_disk(DiskRead read, void* ctx, u64 disk_sectors,
                                AmigaPartitions& out) {
    // An RDB reaches only the first 2^32 sectors, so a larger disk is seen
    // as that much of it.
    const u32 sectors = disk_sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(disk_sectors);
    ContainerRead whole{read, ctx, 0, sectors};
    RdbDisk disk;
    const RdbError error = rdb_scan(read_container, &whole, sectors, disk);
    if (error != RdbError::Ok) {
        return error;
    }
    out.skipped += disk.skipped;
    add_partitions(disk, 0, PartitionSource::WholeDisk, out);
    return RdbError::Ok;
}

// The fields of a GPT header that the entry scan needs, once the header's
// signature, size and CRC have been checked.
struct GptHeader {
    u64 entry_lba;
    u32 entry_count;
    u32 entry_size;
    u32 array_crc;
};

inline bool gpt_signature(const u8* header) {
    constexpr char signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (u32 i = 0; i < 8; ++i) {
        if (header[i] != static_cast<u8>(signature[i])) {
            return false;
        }
    }
    return true;
}

// Reads and checks one GPT header. The CRC covers HeaderSize bytes with the
// CRC field itself read as zero. The entry array must lie inside the disk and
// be addressable with 32 bit sector numbers.
inline GptRead read_gpt_header(const TableDisk& disk, u64 lba, GptHeader& out) {
    u8 sector[sector_bytes];
    if (!disk.read_lba(lba, sector)) {
        return GptRead::Io;
    }
    if (!gpt_signature(sector)) {
        return GptRead::Bad;
    }
    const u32 header_bytes = le32(sector + 12);
    if (header_bytes < gpt_header_bytes || header_bytes > sector_bytes) {
        return GptRead::Bad;
    }
    const u32 stored_crc = le32(sector + 16);
    sector[16] = 0;
    sector[17] = 0;
    sector[18] = 0;
    sector[19] = 0;
    if (crc32(sector, header_bytes) != stored_crc) {
        return GptRead::Bad;
    }
    const u64 entry_lba = le64(sector + 72);
    const u32 entry_count = le32(sector + 80);
    const u32 entry_size = le32(sector + 84);
    if (entry_count > max_gpt_entries || entry_size < min_gpt_entry_bytes ||
        entry_size > max_gpt_entry_bytes || entry_size % min_gpt_entry_bytes != 0) {
        return GptRead::Bad;
    }
    if (entry_lba == 0 || entry_lba >= disk.lbas) {
        return GptRead::Bad;
    }
    const u64 array_bytes = u64{entry_count} * entry_size;
    const u64 lba_bytes = u64{sector_bytes} * disk.scale;
    const u64 array_lbas = (array_bytes + lba_bytes - 1) / lba_bytes;
    if (entry_lba + array_lbas > disk.lbas) {
        return GptRead::Bad;
    }
    out.entry_lba = entry_lba;
    out.entry_count = entry_count;
    out.entry_size = entry_size;
    out.array_crc = le32(sector + 88);
    return GptRead::Ok;
}

// The CRC32 of the entry array, read a sector at a time so the array never
// has to fit in memory.
inline GptRead gpt_array_crc(const TableDisk& disk, const GptHeader& header, u32& crc_out) {
    const u64 bytes = u64{header.entry_count} * header.entry_size;
    u32 crc = 0xFFFFFFFFu;
    u64 done = 0;
    u8 sector[sector_bytes];
    while (done < bytes) {
        if (!disk.read_lba(header.entry_lba, sector, done / sector_bytes)) {
            return GptRead::Io;
        }
        const u64 left = bytes - done;
        const usize take = static_cast<usize>(left < sector_bytes ? left : sector_bytes);
        crc = crc32_update(crc, sector, take);
        done += take;
    }
    crc_out = crc ^ 0xFFFFFFFFu;
    return GptRead::Ok;
}

// Scans one GPT copy: its header, its entry array, then every used entry that
// lies inside the disk. An entry crossing a sector boundary is assembled from
// the two sectors it touches.
inline GptRead scan_gpt_at(const TableDisk& disk, u64 header_lba, AmigaPartitions& out) {
    GptHeader header;
    const GptRead header_result = read_gpt_header(disk, header_lba, header);
    if (header_result != GptRead::Ok) {
        return header_result;
    }
    u32 array_crc = 0;
    const GptRead crc_result = gpt_array_crc(disk, header, array_crc);
    if (crc_result != GptRead::Ok) {
        return crc_result;
    }
    if (array_crc != header.array_crc) {
        return GptRead::Bad;
    }
    u8 block[2 * sector_bytes];
    for (u32 i = 0; i < header.entry_count; ++i) {
        const u64 offset = u64{i} * header.entry_size;
        const u64 array_sector = offset / sector_bytes;
        const u32 within = static_cast<u32>(offset % sector_bytes);
        if (!disk.read_lba(header.entry_lba, block, array_sector)) {
            return GptRead::Io;
        }
        if (within + header.entry_size > sector_bytes &&
            !disk.read_lba(header.entry_lba, block + sector_bytes, array_sector + 1)) {
            return GptRead::Io;
        }
        const u8* entry = block + within;
        bool used = false;
        for (u32 b = 0; b < 16; ++b) {
            used = used || entry[b] != 0;
        }
        if (!used) {
            continue;
        }
        const u64 first = le64(entry + 32);
        const u64 last = le64(entry + 40);
        if (first > last || last >= disk.lbas) {
            continue;
        }
        scan_container(disk.read, disk.ctx, first * disk.scale, (last - first + 1) * disk.scale,
                       PartitionSource::Gpt, out);
    }
    return GptRead::Ok;
}

// The primary GPT header, or the backup at the disk's last sector when the
// primary is damaged.
inline GptRead scan_gpt(const TableDisk& disk, AmigaPartitions& out) {
    if (disk.lbas < 2) {
        return GptRead::Bad;
    }
    GptRead result = scan_gpt_at(disk, 1, out);
    if (result == GptRead::Bad) {
        result = scan_gpt_at(disk, disk.lbas - 1, out);
    }
    return result;
}

// Walks the extended boot record chain of an extended partition. An EBR's
// first entry is a logical partition, relative to the EBR; its second entry
// links to the next EBR, relative to the extended partition's start. A link
// that does not move forward, or leaves the extended partition, ends the
// walk, as does the chain bound.
inline RdbError scan_ebr_chain(const TableDisk& disk, u64 extended_first,
                               u64 extended_sectors, bool& amiga, AmigaPartitions& out) {
    const u64 extended_end = extended_first + extended_sectors;
    u64 ebr = extended_first;
    u8 sector[sector_bytes];
    for (u32 hop = 0; hop < max_ebr_chain; ++hop) {
        if (ebr >= extended_end) {
            break;
        }
        if (!disk.read_lba(ebr, sector)) {
            return RdbError::Io;
        }
        if (!mbr_signature(sector)) {
            break;
        }
        const u8* logical = sector + mbr_entry_base;
        if (mbr_amiga(logical)) {
            amiga = true;
            const u64 first = ebr + le32(logical + 8);
            const u64 sectors = le32(logical + 12);
            if (first > ebr && first + sectors <= extended_end) {
                scan_container(disk.read, disk.ctx, first * disk.scale, sectors * disk.scale,
                               PartitionSource::Mbr, out);
            } else {
                ++out.skipped;
            }
        }
        const u8* link = sector + mbr_entry_base + mbr_entry_bytes;
        if (!mbr_extended(link)) {
            break;
        }
        const u32 relative = le32(link + 8);
        const u64 next = extended_first + relative;
        const u64 link_sectors = le32(link + 12);
        if (relative == 0 || next <= ebr || next >= extended_end ||
            next + link_sectors > extended_end) {
            break;
        }
        ebr = next;
    }
    return RdbError::Ok;
}

// The Amiga entries of a primary MBR: partitions of type 0x30 and 0x76
// directly, and the logical partitions of each extended partition.
inline RdbError scan_mbr(const TableDisk& disk, const u8* sector, bool& amiga,
                         AmigaPartitions& out) {
    for (u32 i = 0; i < mbr_entries; ++i) {
        const u8* entry = sector + mbr_entry_base + i * mbr_entry_bytes;
        if (mbr_amiga(entry)) {
            amiga = true;
            scan_container(disk.read, disk.ctx, u64{le32(entry + 8)} * disk.scale,
                           u64{le32(entry + 12)} * disk.scale, PartitionSource::Mbr, out);
        } else if (mbr_extended(entry)) {
            const RdbError error =
                scan_ebr_chain(disk, le32(entry + 8), le32(entry + 12), amiga, out);
            if (error != RdbError::Ok) {
                return error;
            }
        }
    }
    return RdbError::Ok;
}

}

// Finds the Amiga partitions of a disk of disk_sectors 512 byte sectors whose
// own logical sectors are lba_bytes long. The disk's MBR or GPT table decides
// where to look; a disk without a usable table is scanned as a whole, and a
// whole disk that is neither an RDB nor a DOS volume has no partitions.
inline RdbError find_amiga_partitions(DiskRead read, void* ctx, u64 disk_sectors,
                                      AmigaPartitions& out, u32 lba_bytes = 512) {
    using namespace pc_partitions_detail;
    out.count = 0;
    out.skipped = 0;
    out.table = PcTable::None;
    if (lba_bytes < sector_bytes || lba_bytes % sector_bytes != 0) {
        return RdbError::Unsupported;
    }
    const u32 scale = lba_bytes / sector_bytes;
    const TableDisk disk{read, ctx, scale, disk_sectors / scale};
    u8 sector[sector_bytes];
    if (!read(ctx, 0, sector)) {
        return RdbError::Io;
    }
    if (mbr_signature(sector) && mbr_protective(sector)) {
        const GptRead gpt = scan_gpt(disk, out);
        if (gpt == GptRead::Io) {
            return RdbError::Io;
        }
        if (gpt == GptRead::Ok) {
            out.table = PcTable::Gpt;
            return RdbError::Ok;
        }
        return scan_whole_disk(read, ctx, disk_sectors, out);
    }
    if (mbr_signature(sector) && valid_mbr(sector, disk.lbas)) {
        out.table = PcTable::Mbr;
        bool amiga = false;
        const RdbError error = scan_mbr(disk, sector, amiga, out);
        if (error != RdbError::Ok) {
            return error;
        }
        // A bare DOS volume may carry the 0x55AA signature by chance; it is
        // then a valid MBR without Amiga entries and a volume of its own.
        if (!amiga && looks_like_amiga_volume(sector)) {
            return scan_whole_disk(read, ctx, disk_sectors, out);
        }
        return RdbError::Ok;
    }
    return scan_whole_disk(read, ctx, disk_sectors, out);
}

}
