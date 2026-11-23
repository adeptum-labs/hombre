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

#include "hard_disks.hpp"

#include "../common/block_cache.hpp"
#include "../common/pc_partitions.hpp"
#include "../common/printable.hpp"
#include "../common/serial.hpp"

namespace hombre {

namespace {

constexpr u32 kCacheEntries = 4096;
constexpr u64 kPageBytes = 4096;

constexpr u64 pages_for(u64 bytes) {
    return (bytes + kPageBytes - 1) / kPageBytes;
}

// The caches and the partition contexts outlive the call: the mounted
// volumes keep pointers to them.
constinit BlockCache disk_caches[kMaxHardDisks]{};

struct PartitionWindow {
    BlockCache* cache;
    u64 base;  // the disk sector of partition block 0
    u32 count;  // blocks in the partition
    bool writable;
};

constinit PartitionWindow partition_sources[kMaxHardVolumes]{};

struct CacheMemory {
    BlockCache::Entry* entries;
    u32* buckets;
    u8* data;
};

// Takes the pages for one disk's cache. Returns false when one of them is
// not free; the pages taken before that are kept.
bool alloc_cache_memory(PhysAllocator& pages, CacheMemory& out) {
    const u64 entries = pages.alloc_pages(pages_for(sizeof(BlockCache::Entry) * kCacheEntries));
    const u64 buckets =
        pages.alloc_pages(pages_for(sizeof(u32) * BlockCache::buckets_for(kCacheEntries)));
    const u64 data =
        pages.alloc_pages(pages_for(static_cast<u64>(BlockCache::kSectorBytes) * kCacheEntries));
    if (entries == 0 || buckets == 0 || data == 0) {
        return false;
    }
    out.entries = reinterpret_cast<BlockCache::Entry*>(pages.to_ptr(entries));
    out.buckets = reinterpret_cast<u32*>(pages.to_ptr(buckets));
    out.data = pages.to_ptr(data);
    return true;
}

bool read_through_cache(void* ctx, u64 sector, u8* out) {
    return static_cast<BlockCache*>(ctx)->read(sector, out);
}

// A partition's blocks are numbered from its own start, so the disk is
// reached at base + block. A block outside the partition is refused, and a
// write only exists when the partition may be written.
bool read_partition(void* ctx, u32 block, u8* out) {
    const PartitionWindow& source = *static_cast<PartitionWindow*>(ctx);
    if (block >= source.count) {
        return false;
    }
    return source.cache->read(source.base + block, out);
}

bool write_partition(void* ctx, u32 block, const u8* data) {
    const PartitionWindow& source = *static_cast<PartitionWindow*>(ctx);
    if (!source.writable || block >= source.count) {
        return false;
    }
    return source.cache->write(source.base + block, data);
}

void write_name(const char* name, usize max) {
    char text[40];
    sanitize_text(text, sizeof(text), name, max);
    serial::write(text);
}

void write_prefix(u32 disk) {
    serial::write("hd");
    serial::write_dec(disk);
    serial::write(": ");
}

void write_i32(i32 value) {
    if (value < 0) {
        serial::write_char('-');
        serial::write_dec(static_cast<u64>(-static_cast<i64>(value)));
    } else {
        serial::write_dec(static_cast<u64>(value));
    }
}

const char* table_name(PcTable table) {
    switch (table) {
    case PcTable::None: return "none";
    case PcTable::Mbr: return "MBR";
    case PcTable::Gpt: return "GPT";
    }
    return "none";
}

void log_disk(u32 index, const Disk& disk) {
    write_prefix(index);
    serial::write(disk.kind);
    serial::write_char(' ');
    serial::write_dec(disk.sectors);
    serial::write(" sectors");
    if (disk.read_only) {
        serial::write(" read-only");
    }
    serial::write_char('\n');
}

void log_volume(u32 index, const HardVolume& volume) {
    write_prefix(index);
    write_name(volume.device, sizeof(volume.device));
    serial::write(": ");
    write_name(volume.volume.name, sizeof(volume.volume.name));
    serial::write(volume.volume.ffs ? " (FFS), " : " (OFS), ");
    serial::write_dec(volume.volume.block_count);
    serial::write(" blocks, bootpri ");
    write_i32(volume.boot_pri);
    if (volume.bootable) {
        serial::write(", bootable");
    }
    if (!volume.writable) {
        serial::write(", read-only");
    }
    serial::write_char('\n');
}

char fold_device_char(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// Device names are case-insensitive, as they are in dos.
bool same_device_name(const char* a, const char* b) {
    while (*a != '\0' && fold_device_char(*a) == fold_device_char(*b)) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool device_taken(const HardVolume* volumes, u32 count, const char* name) {
    for (u32 i = 0; i < count; ++i) {
        if (same_device_name(volumes[i].device, name)) {
            return true;
        }
    }
    return false;
}

void copy_text(char* out, const char* text) {
    u32 i = 0;
    for (; text[i] != '\0'; ++i) {
        out[i] = text[i];
    }
    out[i] = '\0';
}

// The partition's own name, or DH<n> with the lowest n free when an earlier
// volume has it. At most kMaxHardVolumes - 1 names are in use, so one of
// DH0: to DH7: is free.
void assign_device(const HardVolume* volumes, u32 count, const char* name, char* device) {
    static_assert(kMaxHardVolumes <= 10u);
    copy_text(device, name);
    if (!device_taken(volumes, count, device)) {
        return;
    }
    for (u32 n = 0; n <= count; ++n) {
        device[0] = 'D';
        device[1] = 'H';
        device[2] = static_cast<char>('0' + n);
        device[3] = '\0';
        if (!device_taken(volumes, count, device)) {
            return;
        }
    }
}

}

u32 hard_disks_mount(const Disk* disks, u32 disk_count, PhysAllocator& pages, HardVolume* out,
                     u32 max) {
    const u32 limit = max < kMaxHardVolumes ? max : kMaxHardVolumes;
    const u32 count = disk_count < kMaxHardDisks ? disk_count : kMaxHardDisks;
    u32 mounted = 0;
    for (u32 n = 0; n < count; ++n) {
        const Disk& disk = disks[n];
        if (disk.dev.read == nullptr) {
            continue;
        }
        log_disk(n, disk);

        CacheMemory memory{};
        if (!alloc_cache_memory(pages, memory)) {
            write_prefix(n);
            serial::write("no memory\n");
            continue;
        }
        BlockCache& cache = disk_caches[n];
        cache.init(disk.dev, memory.entries, memory.buckets, memory.data, kCacheEntries);

        AmigaPartitions partitions{};
        const RdbError scan =
            find_amiga_partitions(&read_through_cache, &cache, disk.sectors, partitions);
        write_prefix(n);
        serial::write("table ");
        serial::write(table_name(partitions.table));
        serial::write(", ");
        serial::write_dec(partitions.count);
        serial::write(" partitions\n");
        if (scan != RdbError::Ok) {
            write_prefix(n);
            serial::write("scan error: ");
            serial::write(rdb_error_name(scan));
            serial::write_char('\n');
        }

        for (u32 i = 0; i < partitions.count; ++i) {
            if (mounted >= limit) {
                break;
            }
            const AmigaPartition& part = partitions.list[i];
            HardVolume& volume = out[mounted];
            assign_device(out, mounted, part.part.name, volume.device);

            // Directory cache and long name volumes stay read-only: this
            // reader does not know the extra blocks they keep.
            const bool writable =
                !disk.read_only && disk.dev.write != nullptr &&
                (part.part.dos_type & 0xFFu) <= 3u;
            PartitionWindow& source = partition_sources[mounted];
            source.cache = &cache;
            source.base = part.disk_first_sector;
            source.count = part.part.block_count;
            source.writable = writable;
            const BlockSource block_source{&source, read_partition,
                                           writable ? write_partition : nullptr};

            const FsError error = adf_mount_partition(block_source, 0, part.part.block_count,
                                                      part.part.reserved, volume.volume);
            if (error != FsError::Ok) {
                write_prefix(n);
                write_name(volume.device, sizeof(volume.device));
                serial::write(": mount error: ");
                serial::write(fs_error_name(error));
                serial::write_char('\n');
                continue;
            }
            volume.boot_pri = part.part.boot_pri;
            volume.bootable = part.part.bootable;
            volume.writable = block_source.write != nullptr;
            __builtin_memcpy(volume.env, part.part.env, sizeof(volume.env));
            log_volume(n, volume);
            ++mounted;
        }

        if (partitions.skipped != 0) {
            write_prefix(n);
            serial::write_dec(partitions.skipped);
            serial::write(" partitions skipped\n");
        }
    }
    return mounted;
}

}
