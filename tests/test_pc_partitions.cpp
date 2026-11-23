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

#include "pc_partitions.hpp"
#include "check.hpp"
#include "rdb_writer.hpp"

#include <cstring>
#include <vector>

using namespace hombre;

namespace {

constexpr u32 sector_bytes = 512;

struct Disk {
    std::vector<u8> image;
    bool fail = false;
    u32 reads = 0;

    u64 sectors() const { return image.size() / sector_bytes; }
};

bool read_sector(void* ctx, u64 sector, u8* out) {
    Disk& disk = *static_cast<Disk*>(ctx);
    ++disk.reads;
    if (disk.fail || sector >= disk.sectors()) {
        return false;
    }
    std::memcpy(out, disk.image.data() + static_cast<usize>(sector) * sector_bytes,
                sector_bytes);
    return true;
}

RdbError scan(Disk& disk, AmigaPartitions& out) {
    return find_amiga_partitions(read_sector, &disk, disk.sectors(), out);
}

RdbGeometry geometry(u32 cylinders) {
    RdbGeometry g;
    g.heads = 2;
    g.sectors = 16;
    g.cylinders = cylinders;
    return g;
}

RdbGeometry small_geometry() {
    RdbGeometry g;
    g.heads = 1;
    g.sectors = 16;
    g.cylinders = 32;
    return g;
}

void place(std::vector<u8>& disk, u64 lba, const std::vector<u8>& image) {
    std::memcpy(disk.data() + static_cast<usize>(lba) * sector_bytes, image.data(),
                image.size());
}

void a_whole_disk_rdb_is_found() {
    RdbPartitionSpec hidden{"DH9", 2, 25};
    hidden.no_mount = true;
    RdbPartitionSpec sys{"DH0", 26, 51, 0x444F5303u, 1, true};
    RdbPartitionSpec work{"Work", 52, 99, 0x444F5301u, 0, false};
    Disk d{build_rdb_disk(geometry(100), {hidden, sys, work})};
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::None);
    CHECK_EQ(parts.count, 2u);
    CHECK_EQ(parts.skipped, 1u);

    CHECK_EQ(parts.list[0].source, PartitionSource::WholeDisk);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{26} * 32);
    CHECK(std::strcmp(parts.list[0].part.name, "DH0") == 0);
    CHECK_EQ(parts.list[0].part.boot_pri, 1);
    CHECK_EQ(parts.list[0].part.dos_type, 0x444F5303u);

    CHECK_EQ(parts.list[1].source, PartitionSource::WholeDisk);
    CHECK_EQ(parts.list[1].disk_first_sector, u64{52} * 32);
    CHECK(std::strcmp(parts.list[1].part.name, "Work") == 0);

    CHECK_EQ(parts.container_count, 1u);
    CHECK_EQ(parts.containers[0].first, 0u);
    CHECK_EQ(parts.containers[0].sectors, d.sectors());
    CHECK_EQ(parts.containers[0].source, PartitionSource::WholeDisk);
}

void a_bare_dos_volume_is_one_partition() {
    Disk d{std::vector<u8>(2048 * sector_bytes, 0)};
    d.image[0] = 'D';
    d.image[1] = 'O';
    d.image[2] = 'S';
    d.image[3] = 3;
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::None);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.skipped, 0u);
    CHECK_EQ(parts.list[0].source, PartitionSource::WholeDisk);
    CHECK_EQ(parts.list[0].disk_first_sector, 0u);
    CHECK_EQ(parts.list[0].part.dos_type, 0x444F5303u);
    CHECK(std::strcmp(parts.list[0].part.name, "DH0") == 0);
    CHECK_EQ(parts.container_count, 1u);
}

void an_rdb_inside_an_mbr_partition_is_found() {
    std::vector<u8> rdb = build_rdb_disk(geometry(100), {RdbPartitionSpec{"Work", 52, 99}});
    Disk d{std::vector<u8>(8192 * sector_bytes, 0)};
    place(d.image, 100, rdb);
    const u32 sectors = static_cast<u32>(rdb.size() / sector_bytes);
    write_mbr(d.image, {{0x30, 100, sectors, false}, {0x07, 5000, 100, false}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Mbr);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.skipped, 0u);
    CHECK_EQ(parts.list[0].source, PartitionSource::Mbr);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{100} + u64{52} * 32);
    CHECK(std::strcmp(parts.list[0].part.name, "Work") == 0);
    // The NTFS partition is no container.
    CHECK_EQ(parts.container_count, 1u);
    CHECK_EQ(parts.containers[0].first, 100u);
    CHECK_EQ(parts.containers[0].sectors, u64{sectors});
    CHECK_EQ(parts.containers[0].source, PartitionSource::Mbr);
}

void an_empty_amiga_mbr_partition_is_a_container() {
    Disk d{std::vector<u8>(4096 * sector_bytes, 0)};
    write_mbr(d.image, {{0x76, 64, 2000, false}, {0x83, 2064, 1000, false}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.count, 0u);
    CHECK_EQ(parts.container_count, 1u);
    CHECK_EQ(parts.containers[0].first, 64u);
    CHECK_EQ(parts.containers[0].sectors, 2000u);
}

void a_damaged_rdb_in_an_amiga_mbr_partition_is_no_container() {
    std::vector<u8> rdb = build_rdb_disk(geometry(100), {RdbPartitionSpec{"Work", 52, 99}});
    rdb[20] ^= 1;
    Disk d{std::vector<u8>(8192 * sector_bytes, 0)};
    place(d.image, 100, rdb);
    write_mbr(d.image, {{0x30, 100, static_cast<u32>(rdb.size() / sector_bytes), false}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.container_count, 0u);
}

void only_gpt_partitions_with_an_rdb_are_containers() {
    std::vector<u8> rdb = build_rdb_disk(geometry(100), {RdbPartitionSpec{"GPT0", 52, 99}});
    Disk d{std::vector<u8>(8192 * sector_bytes, 0)};
    place(d.image, 4096, rdb);
    write_gpt(d.image, {{64, 2047, true}, {4096, 7295, true}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.container_count, 1u);
    CHECK_EQ(parts.containers[0].first, 4096u);
    CHECK_EQ(parts.containers[0].sectors, 3200u);
    CHECK_EQ(parts.containers[0].source, PartitionSource::Gpt);
}

void a_disk_with_unknown_data_is_no_container() {
    Disk d{std::vector<u8>(2048 * sector_bytes, 0)};
    d.image[127 * sector_bytes + 5] = 1;
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::NoPartitions);
    CHECK_EQ(parts.container_count, 0u);
}

void logical_amiga_partitions_are_found_through_the_ebr_chain() {
    std::vector<u8> first = build_rdb_disk(small_geometry(), {RdbPartitionSpec{"Log0", 2, 31}});
    std::vector<u8> second = build_rdb_disk(small_geometry(), {RdbPartitionSpec{"Log1", 2, 31}});
    Disk d{std::vector<u8>(8192 * sector_bytes, 0)};
    place(d.image, 2112, first);
    place(d.image, 3136, second);
    write_mbr(d.image, {{0x0F, 2048, 2048, false}});
    write_mbr_sector(d.image, 2048, {{0x76, 64, 512, false}, {0x0F, 1024, 1024, false}});
    write_mbr_sector(d.image, 3072, {{0x30, 64, 512, false}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Mbr);
    CHECK_EQ(parts.count, 2u);
    CHECK_EQ(parts.skipped, 0u);
    CHECK_EQ(parts.list[0].source, PartitionSource::Mbr);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{2112} + 32);
    CHECK(std::strcmp(parts.list[0].part.name, "Log0") == 0);
    CHECK_EQ(parts.list[1].source, PartitionSource::Mbr);
    CHECK_EQ(parts.list[1].disk_first_sector, u64{3136} + 32);
    CHECK(std::strcmp(parts.list[1].part.name, "Log1") == 0);
}

void an_ebr_chain_that_loops_ends() {
    std::vector<u8> rdb = build_rdb_disk(small_geometry(), {RdbPartitionSpec{"Log0", 2, 31}});
    Disk d{std::vector<u8>(8192 * sector_bytes, 0)};
    place(d.image, 2112, rdb);
    write_mbr(d.image, {{0x05, 2048, 2048, false}});
    write_mbr_sector(d.image, 2048, {{0x76, 64, 512, false}, {0x05, 1024, 1024, false}});
    // The second EBR's link points back at the start of the extended partition.
    write_mbr_sector(d.image, 3072, {{0, 0, 0, false}, {0x05, 0, 1024, false}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{2112} + 32);
    CHECK(d.reads < 100u);
}

void an_rdb_inside_a_gpt_partition_is_found() {
    std::vector<u8> rdb = build_rdb_disk(geometry(100), {RdbPartitionSpec{"GPT0", 52, 99}});
    Disk d{std::vector<u8>(8192 * sector_bytes, 0)};
    place(d.image, 4096, rdb);
    write_gpt(d.image, {{64, 2047, true}, {4096, 7295, true}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Gpt);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.skipped, 0u);
    CHECK_EQ(parts.list[0].source, PartitionSource::Gpt);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{4096} + u64{52} * 32);
    CHECK(std::strcmp(parts.list[0].part.name, "GPT0") == 0);
}

void a_gpt_with_4096_byte_sectors_is_found() {
    // A 4 MiB disk of 1024 sectors of 4096 bytes. The partition starts at
    // LBA 256, which is our sector 2048.
    std::vector<u8> rdb = build_rdb_disk(geometry(100), {RdbPartitionSpec{"GPT0", 52, 99}});
    Disk d{std::vector<u8>(1024 * 4096, 0)};
    place(d.image, 2048, rdb);
    write_gpt(d.image, {{256, 1021, true}}, 4096);
    AmigaPartitions parts;
    CHECK_EQ(find_amiga_partitions(read_sector, &d, d.sectors(), parts, 4096), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Gpt);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.skipped, 0u);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{2048} + u64{52} * 32);

    // Read as 512 byte LBAs the GPT header is not at LBA 1, and the RDB is
    // not at the disk's start, so nothing is found.
    CHECK_EQ(scan(d, parts), RdbError::NoPartitions);
    CHECK_EQ(parts.count, 0u);
}

void an_mbr_with_4096_byte_sectors_is_found() {
    std::vector<u8> rdb = build_rdb_disk(geometry(100), {RdbPartitionSpec{"MBR0", 52, 99}});
    Disk d{std::vector<u8>(1024 * 4096, 0)};
    place(d.image, 800, rdb);
    write_mbr(d.image, {{0x30, 100, 800, false}}, 4096);
    AmigaPartitions parts;
    CHECK_EQ(find_amiga_partitions(read_sector, &d, d.sectors(), parts, 4096), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Mbr);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{800} + u64{52} * 32);
}

void a_logical_sector_size_that_is_not_a_multiple_of_512_is_refused() {
    Disk d{std::vector<u8>(64 * sector_bytes, 0)};
    AmigaPartitions parts;
    CHECK_EQ(find_amiga_partitions(read_sector, &d, d.sectors(), parts, 520),
             RdbError::Unsupported);
}

// A disk of 4 TiB: its first 4096 sectors and 4096 sectors from `high` on
// are kept, and every other sector reads as zeroes.
struct SparseDisk {
    std::vector<u8> low;
    std::vector<u8> high_part;
    u64 high;
    u64 sectors;
};

bool read_sparse(void* ctx, u64 sector, u8* out) {
    const SparseDisk& d = *static_cast<SparseDisk*>(ctx);
    if (sector >= d.sectors) {
        return false;
    }
    std::memset(out, 0, sector_bytes);
    if (sector < d.low.size() / sector_bytes) {
        std::memcpy(out, d.low.data() + sector * sector_bytes, sector_bytes);
    } else if (sector >= d.high && sector - d.high < d.high_part.size() / sector_bytes) {
        std::memcpy(out, d.high_part.data() + (sector - d.high) * sector_bytes, sector_bytes);
    }
    return true;
}

void a_gpt_partition_past_2_tib_is_found() {
    const u64 high = 5000000000ull;
    SparseDisk d{std::vector<u8>(4096 * sector_bytes, 0),
                 build_rdb_disk(geometry(100), {RdbPartitionSpec{"Big0", 52, 99}}), high,
                 8000000000ull};
    write_gpt(d.low, {{high, high + 3199, true}});
    AmigaPartitions parts;
    CHECK_EQ(find_amiga_partitions(read_sparse, &d, d.sectors, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Gpt);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.list[0].disk_first_sector, high + u64{52} * 32);
    CHECK(std::strcmp(parts.list[0].part.name, "Big0") == 0);
}

void a_bad_primary_gpt_header_falls_back_to_the_backup() {
    std::vector<u8> rdb = build_rdb_disk(geometry(100), {RdbPartitionSpec{"GPT0", 52, 99}});
    Disk d{std::vector<u8>(8192 * sector_bytes, 0)};
    place(d.image, 4096, rdb);
    write_gpt(d.image, {{64, 2047, true}, {4096, 7295, true}});
    d.image[sector_bytes + 24] ^= 1;  // MyLBA, covered by the header CRC.
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Gpt);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.list[0].source, PartitionSource::Gpt);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{4096} + u64{52} * 32);
    CHECK(std::strcmp(parts.list[0].part.name, "GPT0") == 0);
}

void a_gpt_with_bad_entry_arrays_falls_back_to_the_whole_disk() {
    RdbGeometry g = geometry(100);
    g.rdsk_block = 8;
    Disk d{build_rdb_disk(g, {RdbPartitionSpec{"Disk", 52, 99}})};
    const u64 sectors = d.sectors();
    write_gpt(d.image, {{100, 200, true}});
    d.image[2 * sector_bytes] ^= 1;
    d.image[(sectors - 2) * sector_bytes] ^= 1;
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::None);
    CHECK_EQ(parts.count, 1u);
    CHECK_EQ(parts.list[0].source, PartitionSource::WholeDisk);
    CHECK_EQ(parts.list[0].disk_first_sector, u64{52} * 32);
    CHECK(std::strcmp(parts.list[0].part.name, "Disk") == 0);
}

void a_bare_dos_volume_inside_a_gpt_partition_is_ignored() {
    Disk d{std::vector<u8>(4096 * sector_bytes, 0)};
    d.image[100 * sector_bytes] = 'D';
    d.image[100 * sector_bytes + 1] = 'O';
    d.image[100 * sector_bytes + 2] = 'S';
    d.image[100 * sector_bytes + 3] = 1;
    write_gpt(d.image, {{100, 200, true}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Gpt);
    CHECK_EQ(parts.count, 0u);
    CHECK_EQ(parts.skipped, 0u);
}

void an_mbr_without_amiga_partitions_is_understood() {
    Disk d{std::vector<u8>(4096 * sector_bytes, 0)};
    write_mbr(d.image, {{0x07, 100, 1000, false}});
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Ok);
    CHECK_EQ(parts.table, PcTable::Mbr);
    CHECK_EQ(parts.count, 0u);
}

void a_blank_disk_has_no_partitions() {
    Disk d{std::vector<u8>(2048 * sector_bytes, 0)};
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::NoPartitions);
    CHECK_EQ(parts.table, PcTable::None);
    CHECK_EQ(parts.count, 0u);
    // Blank, so the Amiga may set it up.
    CHECK_EQ(parts.container_count, 1u);
    CHECK_EQ(parts.containers[0].sectors, d.sectors());
}

void a_read_error_on_sector_0_is_io() {
    Disk d{build_rdb_disk(geometry(100), {RdbPartitionSpec{"DH0", 2, 99}})};
    d.fail = true;
    AmigaPartitions parts;
    CHECK_EQ(scan(d, parts), RdbError::Io);
}

}

int main() {
    a_whole_disk_rdb_is_found();
    a_bare_dos_volume_is_one_partition();
    an_rdb_inside_an_mbr_partition_is_found();
    an_empty_amiga_mbr_partition_is_a_container();
    a_damaged_rdb_in_an_amiga_mbr_partition_is_no_container();
    only_gpt_partitions_with_an_rdb_are_containers();
    a_disk_with_unknown_data_is_no_container();
    logical_amiga_partitions_are_found_through_the_ebr_chain();
    an_ebr_chain_that_loops_ends();
    an_rdb_inside_a_gpt_partition_is_found();
    a_gpt_with_4096_byte_sectors_is_found();
    an_mbr_with_4096_byte_sectors_is_found();
    a_logical_sector_size_that_is_not_a_multiple_of_512_is_refused();
    a_gpt_partition_past_2_tib_is_found();
    a_bad_primary_gpt_header_falls_back_to_the_backup();
    a_gpt_with_bad_entry_arrays_falls_back_to_the_whole_disk();
    a_bare_dos_volume_inside_a_gpt_partition_is_ignored();
    an_mbr_without_amiga_partitions_is_understood();
    a_blank_disk_has_no_partitions();
    a_read_error_on_sector_0_is_io();
    return check::finish("test_pc_partitions");
}
