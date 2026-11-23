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

#include "rdb.hpp"
#include "check.hpp"
#include "rdb_writer.hpp"

#include <cstring>
#include <vector>

using namespace hombre;

namespace {

struct Disk {
    std::vector<u8> image;
    u32 reads = 0;
    bool fail = false;

    u32 sectors() const { return static_cast<u32>(image.size() / 512); }
};

bool read_sector(void* ctx, u32 sector, u8* out) {
    Disk& d = *static_cast<Disk*>(ctx);
    ++d.reads;
    if (d.fail || sector >= d.sectors()) {
        return false;
    }
    std::memcpy(out, d.image.data() + static_cast<usize>(sector) * 512, 512);
    return true;
}

RdbError scan(Disk& d, RdbDisk& out) { return rdb_scan(read_sector, &d, d.sectors(), out); }

RdbGeometry geometry() {
    RdbGeometry g;
    g.heads = 2;
    g.sectors = 16;
    g.cylinders = 100;
    return g;
}

void put32(std::vector<u8>& image, u32 offset, u32 value) {
    image[offset] = static_cast<u8>(value >> 24);
    image[offset + 1] = static_cast<u8>(value >> 16);
    image[offset + 2] = static_cast<u8>(value >> 8);
    image[offset + 3] = static_cast<u8>(value);
}

void two_partitions_are_found_in_order() {
    RdbPartitionSpec work{"Work", 52, 99, 0x444F5301u, 0, false};
    RdbPartitionSpec sys{"DH0", 2, 51, 0x444F5303u, 1, true};
    Disk d{build_rdb_disk(geometry(), {sys, work})};
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK(disk.has_rdb);
    CHECK_EQ(disk.count, 2u);
    CHECK_EQ(disk.skipped, 0u);

    const RdbPartition& a = disk.partition[0];
    CHECK(std::strcmp(a.name, "DH0") == 0);
    CHECK_EQ(a.first_block, 2u * 32);
    CHECK_EQ(a.block_count, 50u * 32);
    CHECK_EQ(a.reserved, 2u);
    CHECK_EQ(a.dos_type, 0x444F5303u);
    CHECK_EQ(a.boot_pri, 1);
    CHECK(a.bootable);
    CHECK_EQ(a.env[envec::surfaces], 2u);
    CHECK_EQ(a.env[envec::blocks_per_track], 16u);
    CHECK_EQ(a.env[envec::low_cyl], 2u);
    CHECK_EQ(a.env[envec::high_cyl], 51u);

    const RdbPartition& b = disk.partition[1];
    CHECK(std::strcmp(b.name, "Work") == 0);
    CHECK_EQ(b.first_block, 52u * 32);
    CHECK_EQ(b.block_count, 48u * 32);
    CHECK(!b.bootable);
}

void the_rigid_disk_block_may_sit_in_a_later_block() {
    RdbGeometry g = geometry();
    g.rdsk_block = 3;
    Disk d{build_rdb_disk(g, {RdbPartitionSpec{"DH0", 2, 99}})};
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK_EQ(disk.count, 1u);
    CHECK_EQ(disk.partition[0].first_block, 64u);
}

void a_bad_rigid_disk_block_checksum_is_an_error() {
    Disk d{build_rdb_disk(geometry(), {RdbPartitionSpec{}})};
    d.image[20] ^= 1;
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::BadChecksum);
    CHECK_EQ(disk.count, 0u);
}

void a_damaged_partition_block_ends_the_chain() {
    RdbPartitionSpec a{"DH0", 2, 50};
    RdbPartitionSpec b{"DH1", 51, 99};
    Disk d{build_rdb_disk(geometry(), {a, b})};
    d.image[2 * 512 + 100] ^= 1;
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK_EQ(disk.count, 1u);
    CHECK_EQ(disk.skipped, 1u);
}

void a_looping_chain_ends() {
    Disk d{build_rdb_disk(geometry(), {RdbPartitionSpec{"DH0", 2, 99}})};
    // The partition block points back at itself.
    put32(d.image, 512 + 16, 1);
    put32(d.image, 512 + 8, 0);
    u32 sum = 0;
    for (u32 i = 0; i < 64; ++i) {
        const u8* p = d.image.data() + 512 + i * 4;
        sum += (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
    }
    put32(d.image, 512 + 8, 0u - sum);
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK_EQ(disk.count, RdbDisk::kMaxPartitions);
    CHECK(d.reads < 200u);
}

void unmountable_partitions_are_skipped() {
    RdbPartitionSpec hidden{"DH0", 2, 30};
    hidden.no_mount = true;
    RdbPartitionSpec foreign{"DH1", 31, 60, 0x50465303u};  // PFS3
    RdbPartitionSpec good{"DH2", 61, 99};
    Disk d{build_rdb_disk(geometry(), {hidden, foreign, good})};
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK_EQ(disk.count, 1u);
    CHECK_EQ(disk.skipped, 2u);
    CHECK(std::strcmp(disk.partition[0].name, "DH2") == 0);
}

void a_partition_past_the_end_of_the_disk_is_skipped() {
    Disk d{build_rdb_disk(geometry(), {RdbPartitionSpec{"DH0", 2, 99}})};
    d.image.resize(d.image.size() / 2);
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK_EQ(disk.count, 0u);
    CHECK_EQ(disk.skipped, 1u);
}

void a_larger_block_size_is_skipped() {
    Disk d{build_rdb_disk(geometry(), {RdbPartitionSpec{"DH0", 2, 99}})};
    // de_SizeBlock of 256 longs, 1024 byte blocks.
    put32(d.image, 512 + (32 + 1) * 4, 256);
    put32(d.image, 512 + 8, 0);
    u32 sum = 0;
    for (u32 i = 0; i < 64; ++i) {
        const u8* p = d.image.data() + 512 + i * 4;
        sum += (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
    }
    put32(d.image, 512 + 8, 0u - sum);
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK_EQ(disk.count, 0u);
    CHECK_EQ(disk.skipped, 1u);
}

void geometry_that_wraps_is_skipped() {
    Disk d{build_rdb_disk(geometry(), {RdbPartitionSpec{"DH0", 2, 99}})};
    // Products that wrap 64 bits and would otherwise pass the size check.
    put32(d.image, 512 + (32 + 3) * 4, 3);
    put32(d.image, 512 + (32 + 5) * 4, 2007567422u);
    put32(d.image, 512 + (32 + 9) * 4, 1);
    put32(d.image, 512 + (32 + 10) * 4, 3062868336u);
    put32(d.image, 512 + 8, 0);
    u32 sum = 0;
    for (u32 i = 0; i < 64; ++i) {
        const u8* p = d.image.data() + 512 + i * 4;
        sum += (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
    }
    put32(d.image, 512 + 8, 0u - sum);
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK_EQ(disk.count, 0u);
    CHECK_EQ(disk.skipped, 1u);
}

void unusable_names_become_dh_and_the_index() {
    RdbPartitionSpec a{"", 2, 50};
    RdbPartitionSpec b{"Bad:Name", 51, 99};
    Disk d{build_rdb_disk(geometry(), {a, b})};
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK_EQ(disk.count, 2u);
    CHECK(std::strcmp(disk.partition[0].name, "DH0") == 0);
    CHECK(std::strcmp(disk.partition[1].name, "DH1") == 0);
}

void a_bare_dos_volume_is_one_partition() {
    Disk d{std::vector<u8>(4096 * 512, 0)};
    d.image[0] = 'D';
    d.image[1] = 'O';
    d.image[2] = 'S';
    d.image[3] = 1;
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Ok);
    CHECK(!disk.has_rdb);
    CHECK_EQ(disk.count, 1u);
    const RdbPartition& p = disk.partition[0];
    CHECK(std::strcmp(p.name, "DH0") == 0);
    CHECK_EQ(p.first_block, 0u);
    CHECK_EQ(p.block_count, 4096u);
    CHECK_EQ(p.reserved, 2u);
    CHECK_EQ(p.dos_type, 0x444F5301u);
    CHECK_EQ(p.env[envec::high_cyl], 4095u);
}

void a_blank_disk_has_no_partitions() {
    Disk d{std::vector<u8>(4096 * 512, 0)};
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::NoPartitions);
    CHECK_EQ(disk.count, 0u);
}

void a_read_error_is_reported() {
    Disk d{build_rdb_disk(geometry(), {RdbPartitionSpec{}})};
    d.fail = true;
    RdbDisk disk;
    CHECK_EQ(scan(d, disk), RdbError::Io);
}

}

int main() {
    two_partitions_are_found_in_order();
    the_rigid_disk_block_may_sit_in_a_later_block();
    a_bad_rigid_disk_block_checksum_is_an_error();
    a_damaged_partition_block_ends_the_chain();
    a_looping_chain_ends();
    unmountable_partitions_are_skipped();
    a_partition_past_the_end_of_the_disk_is_skipped();
    a_larger_block_size_is_skipped();
    geometry_that_wraps_is_skipped();
    unusable_names_become_dh_and_the_index();
    a_bare_dos_volume_is_one_partition();
    a_blank_disk_has_no_partitions();
    a_read_error_is_reported();
    return check::finish("test_rdb");
}
