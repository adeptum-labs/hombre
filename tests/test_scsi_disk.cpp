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

#include "scsi_disk.hpp"
#include "check.hpp"

#include <cstring>
#include <initializer_list>
#include <vector>

using namespace hombre;

namespace {

constexpr u32 kSectorSize = 512;

struct MemoryDisk {
    u64 sectors = 0;
    bool writable = true;
    bool flush_ok = true;
    u32 flush_calls = 0;
    u64 fail_sector = ~0ull;
    std::vector<u8> bytes;

    static bool read(void* ctx, u64 sector, u8* out) {
        MemoryDisk& disk = *static_cast<MemoryDisk*>(ctx);
        if (sector >= disk.sectors || sector == disk.fail_sector) {
            return false;
        }
        std::memcpy(out, &disk.bytes[sector * kSectorSize], kSectorSize);
        return true;
    }

    static bool write(void* ctx, u64 sector, const u8* data) {
        MemoryDisk& disk = *static_cast<MemoryDisk*>(ctx);
        if (sector >= disk.sectors || sector == disk.fail_sector) {
            return false;
        }
        std::memcpy(&disk.bytes[sector * kSectorSize], data, kSectorSize);
        return true;
    }

    static bool flush(void* ctx) {
        MemoryDisk& disk = *static_cast<MemoryDisk*>(ctx);
        ++disk.flush_calls;
        return disk.flush_ok;
    }
};

MemoryDisk make_disk(u64 sectors, bool writable = true) {
    MemoryDisk disk;
    disk.sectors = sectors;
    disk.writable = writable;
    disk.bytes.resize(sectors * kSectorSize);
    for (u64 sector = 0; sector < sectors; ++sector) {
        for (u32 i = 0; i < kSectorSize; ++i) {
            disk.bytes[sector * kSectorSize + i] = static_cast<u8>(sector * 31 + i);
        }
    }
    return disk;
}

DiskUnit unit_of(MemoryDisk& disk) {
    DiskUnit unit{};
    unit.ctx = &disk;
    unit.read = MemoryDisk::read;
    unit.write = disk.writable ? MemoryDisk::write : nullptr;
    unit.flush = MemoryDisk::flush;
    unit.sectors = disk.sectors;
    std::strcpy(unit.product, "HOMBRE DISK");
    return unit;
}

ScsiResult run(const DiskUnit& unit, std::initializer_list<u8> cdb, std::vector<u8>& data) {
    return scsi_execute(unit, cdb.begin(), static_cast<u32>(cdb.size()),
                        data.empty() ? nullptr : data.data(), static_cast<u32>(data.size()));
}

ScsiResult run(const DiskUnit& unit, std::initializer_list<u8> cdb) {
    return scsi_execute(unit, cdb.begin(), static_cast<u32>(cdb.size()), nullptr, 0);
}

void expect(const ScsiResult& result, u8 status, u8 key, u8 asc, u32 actual) {
    CHECK_EQ(result.status, status);
    CHECK_EQ(result.sense_key, key);
    CHECK_EQ(result.asc, asc);
    CHECK_EQ(result.actual, actual);
}

void expect_good(const ScsiResult& result, u32 actual) {
    expect(result, 0, 0, 0, actual);
}

void expect_check(const ScsiResult& result, u8 key, u8 asc) {
    expect(result, 2, key, asc, 0);
}

void geometry_rounds_to_heads_and_clamps_cylinders() {
    DiskGeometry geometry = disk_geometry(0);
    CHECK_EQ(geometry.heads, 1u);
    CHECK_EQ(geometry.track_sectors, 1u);
    CHECK_EQ(geometry.cylinders, 0u);

    geometry = disk_geometry(1007);
    CHECK_EQ(geometry.heads, 1u);
    CHECK_EQ(geometry.track_sectors, 1u);
    CHECK_EQ(geometry.cylinders, 1007u);

    geometry = disk_geometry(1008);
    CHECK_EQ(geometry.heads, 16u);
    CHECK_EQ(geometry.track_sectors, 63u);
    CHECK_EQ(geometry.cylinders, 1u);

    geometry = disk_geometry(2048);
    CHECK_EQ(geometry.cylinders, 2u);

    geometry = disk_geometry(1008ull * 0x2000000ull);
    CHECK_EQ(geometry.cylinders, 0xFFFFFFu);
}

void simple_commands_report_good() {
    MemoryDisk disk = make_disk(8);
    const DiskUnit unit = unit_of(disk);
    expect_good(run(unit, {0x00, 0, 0, 0, 0, 0}), 0);  // TEST UNIT READY
    expect_good(run(unit, {0x1B, 0, 0, 0, 1, 0}), 0);  // START STOP UNIT
    expect_good(run(unit, {0x1E, 0, 0, 0, 0, 0}), 0);  // PREVENT ALLOW
}

void request_sense_is_a_fixed_format_that_caps_to_allocation() {
    MemoryDisk disk = make_disk(8);
    const DiskUnit unit = unit_of(disk);
    std::vector<u8> data(18, 0xAA);
    expect_good(run(unit, {0x03, 0, 0, 0, 18, 0}, data), 18);
    CHECK_EQ(data[0], 0x70);
    CHECK_EQ(data[2], 0u);
    CHECK_EQ(data[7], 10u);

    std::vector<u8> four(18, 0xAA);
    expect_good(run(unit, {0x03, 0, 0, 0, 4, 0}, four), 4);
    CHECK_EQ(four[3], 0u);

    std::vector<u8> two(2, 0xAA);
    expect_good(run(unit, {0x03, 0, 0, 0, 18, 0}, two), 2);
}

void inquiry_reports_vendor_product_and_rejects_evpd() {
    MemoryDisk disk = make_disk(8);
    const DiskUnit unit = unit_of(disk);
    std::vector<u8> data(36, 0xAA);
    expect_good(run(unit, {0x12, 0, 0, 0, 36, 0}, data), 36);
    CHECK_EQ(data[0], 0u);
    CHECK_EQ(data[1], 0u);
    CHECK_EQ(data[2], 2u);
    CHECK_EQ(data[3], 2u);
    CHECK_EQ(data[4], 31u);
    CHECK_EQ(data[8], static_cast<u8>('H'));
    CHECK_EQ(data[15], static_cast<u8>(' '));
    CHECK_EQ(data[16], static_cast<u8>('H'));
    CHECK_EQ(data[26], static_cast<u8>('K'));
    CHECK_EQ(data[27], static_cast<u8>(' '));
    CHECK_EQ(data[31], static_cast<u8>(' '));
    CHECK_EQ(data[32], static_cast<u8>('1'));
    CHECK_EQ(data[33], static_cast<u8>('.'));
    CHECK_EQ(data[34], static_cast<u8>('0'));
    CHECK_EQ(data[35], static_cast<u8>(' '));

    std::vector<u8> ten(36);
    expect_good(run(unit, {0x12, 0, 0, 0, 10, 0}, ten), 10);

    std::vector<u8> evpd(36);
    expect_check(run(unit, {0x12, 1, 0, 0, 36, 0}, evpd), 5, 0x24);
}

void read_capacity_10_and_16_report_the_last_lba() {
    MemoryDisk disk = make_disk(1000);
    const DiskUnit unit = unit_of(disk);
    std::vector<u8> data(8);
    expect_good(run(unit, {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, data), 8);
    CHECK_EQ(data[0], 0u);
    CHECK_EQ(data[1], 0u);
    CHECK_EQ(data[2], 0x03u);
    CHECK_EQ(data[3], 0xE7u);
    CHECK_EQ(data[4], 0x00u);
    CHECK_EQ(data[5], 0x00u);
    CHECK_EQ(data[6], 0x02u);
    CHECK_EQ(data[7], 0x00u);

    std::vector<u8> short_data(4);
    expect_good(run(unit, {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, short_data), 4);

    DiskUnit big{};
    big.sectors = 0x123456789ull;
    big.write = MemoryDisk::write;
    std::vector<u8> clamped(8);
    expect_good(run(big, {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, clamped), 8);
    CHECK_EQ(clamped[0], 0xFFu);
    CHECK_EQ(clamped[1], 0xFFu);
    CHECK_EQ(clamped[2], 0xFFu);
    CHECK_EQ(clamped[3], 0xFFu);

    std::vector<u8> wide(32);
    expect_good(run(big, {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}, wide),
                32);
    CHECK_EQ(wide[0], 0u);
    CHECK_EQ(wide[1], 0u);
    CHECK_EQ(wide[2], 0u);
    CHECK_EQ(wide[3], 1u);
    CHECK_EQ(wide[4], 0x23u);
    CHECK_EQ(wide[5], 0x45u);
    CHECK_EQ(wide[6], 0x67u);
    CHECK_EQ(wide[7], 0x88u);
    CHECK_EQ(wide[8], 0x00u);
    CHECK_EQ(wide[9], 0x00u);
    CHECK_EQ(wide[10], 0x02u);
    CHECK_EQ(wide[11], 0x00u);
    CHECK_EQ(wide[12], 0u);

    std::vector<u8> capped(32);
    expect_good(run(big, {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0}, capped),
                10);

    std::vector<u8> service(32);
    expect_check(run(big, {0x9E, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}, service),
                 5, 0x20);
}

void read_6_count_zero_reads_256_sectors() {
    MemoryDisk disk = make_disk(512);
    const DiskUnit unit = unit_of(disk);
    std::vector<u8> data(256 * kSectorSize);
    expect_good(run(unit, {0x08, 0, 0, 0, 0, 0}, data), 256 * kSectorSize);
    CHECK_EQ(data[0], 0u);
    CHECK_EQ(data[kSectorSize], 31u);
    CHECK_EQ(data[255 * kSectorSize + 511], static_cast<u8>(255 * 31 + 511));

    std::vector<u8> one(data.begin(), data.begin() + kSectorSize);
    expect_good(run(unit, {0x08, 0, 0, 2, 1, 0}, one), kSectorSize);
    CHECK_EQ(one[0], 62u);

    std::vector<u8> out_of_range(kSectorSize);
    expect_check(run(unit, {0x08, 1, 0, 0, 1, 0}, out_of_range), 5, 0x21);
}

void read_10_and_16_check_range_and_buffer_size() {
    MemoryDisk disk = make_disk(2048);
    const DiskUnit unit = unit_of(disk);
    std::vector<u8> data(2 * kSectorSize);
    expect_good(run(unit, {0x28, 0, 0, 0, 0, 2, 0, 0, 2, 0}, data), 2 * kSectorSize);
    CHECK_EQ(data[0], 62u);

    expect_good(run(unit, {0x28, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0}, data), 0);

    std::vector<u8> overflow(2 * kSectorSize);
    expect_check(run(unit, {0x28, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 2, 0}, overflow), 5,
                 0x21);

    std::vector<u8> short_buffer(kSectorSize - 1);
    expect_check(run(unit, {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0}, short_buffer), 5, 0x24);

    std::vector<u8> wide(kSectorSize);
    expect_good(run(unit, {0x88, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0}, wide),
                kSectorSize);
    expect_good(run(unit, {0x88, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, wide), 0);

    const u8 short_cdb[10] = {0x88, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    expect_check(scsi_execute(unit, short_cdb, 10, wide.data(), static_cast<u32>(wide.size())),
                 5, 0x24);
}

void write_commands_fill_the_unit_and_flush() {
    MemoryDisk disk = make_disk(2048);
    const DiskUnit unit = unit_of(disk);
    std::vector<u8> data(256 * kSectorSize, 0x5A);
    expect_good(run(unit, {0x0A, 0, 0, 0, 0, 0}, data), 256 * kSectorSize);
    CHECK_EQ(disk.bytes[0], 0x5A);
    CHECK_EQ(disk.bytes[255 * kSectorSize + 511], 0x5A);
    CHECK_EQ(disk.flush_calls, 1u);

    std::vector<u8> none(kSectorSize, 0xEE);
    expect_good(run(unit, {0x2A, 0, 0, 0, 0, 4, 0, 0, 0, 0}, none), 0);
    CHECK_EQ(disk.flush_calls, 1u);

    std::vector<u8> two(2 * kSectorSize, 0xA5);
    expect_good(run(unit, {0x2A, 0, 0, 0, 0, 6, 0, 0, 2, 0}, two), 2 * kSectorSize);
    CHECK_EQ(disk.bytes[6 * kSectorSize], 0xA5);
    CHECK_EQ(disk.bytes[7 * kSectorSize + 511], 0xA5);
    CHECK_EQ(disk.flush_calls, 2u);

    std::vector<u8> one(kSectorSize, 0x3C);
    expect_good(run(unit, {0x8A, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0}, one),
                kSectorSize);
    CHECK_EQ(disk.bytes[8 * kSectorSize], 0x3C);

    disk.flush_ok = false;
    std::vector<u8> failed(kSectorSize, 0x11);
    expect(run(unit, {0x2A, 0, 0, 0, 0, 9, 0, 0, 1, 0}, failed), 2, 3, 0x0C,
           kSectorSize);
}

void write_to_a_read_only_unit_is_protected() {
    MemoryDisk disk = make_disk(64, false);
    const DiskUnit unit = unit_of(disk);
    std::vector<u8> data(kSectorSize, 0x77);
    expect_check(run(unit, {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0}, data), 7, 0x27);
    CHECK_EQ(run(unit, {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0}, data).actual, 0u);
}

void failed_transfers_report_the_bytes_done() {
    MemoryDisk disk = make_disk(64);
    const DiskUnit unit = unit_of(disk);
    disk.fail_sector = 3;

    std::vector<u8> read_data(2 * kSectorSize);
    expect(run(unit, {0x28, 0, 0, 0, 0, 2, 0, 0, 2, 0}, read_data), 2, 3, 0x11,
           kSectorSize);

    std::vector<u8> write_data(2 * kSectorSize, 0x66);
    disk.flush_calls = 0;
    expect(run(unit, {0x2A, 0, 0, 0, 0, 2, 0, 0, 2, 0}, write_data), 2, 3, 0x0C,
           kSectorSize);
    CHECK_EQ(disk.flush_calls, 0u);
}

void verify_and_synchronize_cache() {
    MemoryDisk disk = make_disk(2048);
    const DiskUnit unit = unit_of(disk);
    expect_good(run(unit, {0x2F, 0, 0, 0, 0, 0, 0, 0, 1, 0}), 0);
    expect_check(run(unit, {0x2F, 0, 0, 0, 0x07, 0xFF, 0, 0, 2, 0}), 5, 0x21);

    disk.flush_calls = 0;
    expect_good(run(unit, {0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0}), 0);
    CHECK_EQ(disk.flush_calls, 1u);
    expect_good(run(unit, {0x91, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}), 0);
    CHECK_EQ(disk.flush_calls, 2u);

    disk.flush_ok = false;
    expect_check(run(unit, {0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0}), 3, 0x0C);
    expect_check(run(unit, {0x91, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}), 3, 0x0C);
}

void mode_sense_6_headers_descriptor_and_pages() {
    MemoryDisk disk = make_disk(2048);
    const DiskUnit unit = unit_of(disk);

    std::vector<u8> all(64);
    expect_good(run(unit, {0x1A, 0, 0x3F, 0, 64, 0}, all), 60);
    CHECK_EQ(all[0], 59u);
    CHECK_EQ(all[1], 0u);
    CHECK_EQ(all[2], 0u);
    CHECK_EQ(all[3], 8u);
    CHECK_EQ(all[4], 0u);
    CHECK_EQ(all[5], 0x00u);
    CHECK_EQ(all[6], 0x08u);
    CHECK_EQ(all[7], 0x00u);
    CHECK_EQ(all[8], 0u);
    CHECK_EQ(all[9], 0x00u);
    CHECK_EQ(all[10], 0x02u);
    CHECK_EQ(all[11], 0x00u);
    CHECK_EQ(all[12], 3u);
    CHECK_EQ(all[13], 0x16u);
    CHECK_EQ(all[22], 0u);
    CHECK_EQ(all[23], 63u);
    CHECK_EQ(all[24], 0x02u);
    CHECK_EQ(all[25], 0x00u);
    CHECK_EQ(all[36], 4u);
    CHECK_EQ(all[37], 0x16u);
    CHECK_EQ(all[38], 0x00u);
    CHECK_EQ(all[39], 0x00u);
    CHECK_EQ(all[40], 0x02u);
    CHECK_EQ(all[41], 16u);
    CHECK_EQ(all[56], 0x1Cu);
    CHECK_EQ(all[57], 0x20u);

    std::vector<u8> dbd(64);
    expect_good(run(unit, {0x1A, 8, 0x3F, 0, 64, 0}, dbd), 52);
    CHECK_EQ(dbd[0], 51u);
    CHECK_EQ(dbd[3], 0u);
    CHECK_EQ(dbd[4], 3u);
    CHECK_EQ(dbd[28], 4u);

    std::vector<u8> page3(64);
    expect_good(run(unit, {0x1A, 0, 0x03, 0, 64, 0}, page3), 36);
    CHECK_EQ(page3[0], 35u);
    CHECK_EQ(page3[3], 8u);
    CHECK_EQ(page3[12], 3u);
    CHECK_EQ(page3[13], 0x16u);
    CHECK_EQ(page3[22], 0u);
    CHECK_EQ(page3[23], 63u);
    CHECK_EQ(page3[24], 2u);
    CHECK_EQ(page3[25], 0u);

    std::vector<u8> page4(64);
    expect_good(run(unit, {0x1A, 0, 0x04, 0, 64, 0}, page4), 36);
    CHECK_EQ(page4[12], 4u);
    CHECK_EQ(page4[14], 0x00u);
    CHECK_EQ(page4[15], 0x00u);
    CHECK_EQ(page4[16], 0x02u);
    CHECK_EQ(page4[17], 16u);
    CHECK_EQ(page4[32], 0x1Cu);
    CHECK_EQ(page4[33], 0x20u);

    std::vector<u8> invalid(64);
    expect_check(run(unit, {0x1A, 0, 0x01, 0, 64, 0}, invalid), 5, 0x24);

    std::vector<u8> capped(64);
    expect_good(run(unit, {0x1A, 0, 0x3F, 0, 10, 0}, capped), 10);
    CHECK_EQ(capped[0], 59u);

    MemoryDisk protected_disk = make_disk(2048, false);
    const DiskUnit protected_unit = unit_of(protected_disk);
    std::vector<u8> protected_reply(64);
    expect_good(run(protected_unit, {0x1A, 0, 0x3F, 0, 64, 0}, protected_reply), 60);
    CHECK_EQ(protected_reply[2], 0x80u);

    DiskUnit big{};
    big.sectors = 0x400000000ull;
    big.write = MemoryDisk::write;
    std::vector<u8> clamped(64);
    expect_good(run(big, {0x1A, 0, 0x3F, 0, 64, 0}, clamped), 60);
    CHECK_EQ(clamped[5], 0xFFu);
    CHECK_EQ(clamped[6], 0xFFu);
    CHECK_EQ(clamped[7], 0xFFu);
    CHECK_EQ(clamped[38], 0xFFu);
    CHECK_EQ(clamped[39], 0xFFu);
    CHECK_EQ(clamped[40], 0xFFu);
}

void mode_sense_10_headers_descriptor_and_pages() {
    MemoryDisk disk = make_disk(2048);
    const DiskUnit unit = unit_of(disk);

    std::vector<u8> all(64);
    expect_good(run(unit, {0x5A, 0, 0x3F, 0, 0, 0, 0, 64, 0, 0}, all), 64);
    CHECK_EQ(all[0], 0u);
    CHECK_EQ(all[1], 62u);
    CHECK_EQ(all[3], 0u);
    CHECK_EQ(all[6], 0u);
    CHECK_EQ(all[7], 8u);
    CHECK_EQ(all[8], 0u);
    CHECK_EQ(all[9], 0x00u);
    CHECK_EQ(all[10], 0x08u);
    CHECK_EQ(all[11], 0x00u);
    CHECK_EQ(all[12], 0u);
    CHECK_EQ(all[13], 0x00u);
    CHECK_EQ(all[14], 0x02u);
    CHECK_EQ(all[16], 3u);
    CHECK_EQ(all[17], 0x16u);
    CHECK_EQ(all[40], 4u);
    CHECK_EQ(all[41], 0x16u);

    std::vector<u8> dbd(64);
    expect_good(run(unit, {0x5A, 8, 0x04, 0, 0, 0, 0, 64, 0, 0}, dbd), 32);
    CHECK_EQ(dbd[0], 0u);
    CHECK_EQ(dbd[1], 30u);
    CHECK_EQ(dbd[6], 0u);
    CHECK_EQ(dbd[7], 0u);
    CHECK_EQ(dbd[8], 4u);

    std::vector<u8> capped(64);
    expect_good(run(unit, {0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 8, 0}, capped), 8);

    std::vector<u8> invalid(64);
    expect_check(run(unit, {0x5A, 0, 0x02, 0, 0, 0, 0, 64, 0, 0}, invalid), 5, 0x24);

    MemoryDisk protected_disk = make_disk(2048, false);
    const DiskUnit protected_unit = unit_of(protected_disk);
    std::vector<u8> protected_reply(64);
    expect_good(run(protected_unit, {0x5A, 0, 0x3F, 0, 0, 0, 0, 64, 0, 0}, protected_reply),
                64);
    CHECK_EQ(protected_reply[3], 0x80u);
}

void unknown_opcodes_and_short_commands_are_rejected() {
    MemoryDisk disk = make_disk(8);
    const DiskUnit unit = unit_of(disk);
    expect_check(run(unit, {0xFF, 0, 0, 0, 0, 0}), 5, 0x20);
    expect_check(run(unit, {0x28, 0, 0, 0, 0, 0, 0, 0, 1}), 5, 0x24);

    const u8 cdb[6] = {0x00, 0, 0, 0, 0, 0};
    expect_check(scsi_execute(unit, cdb, 0, nullptr, 0), 5, 0x24);
}

void sense_data_is_eighteen_bytes_and_capped() {
    const ScsiResult result{2, 4, 5, 0x21, 0};
    u8 out[32] = {};
    u32 written = 99;
    scsi_sense(result, out, sizeof out, written);
    CHECK_EQ(written, 18u);
    CHECK_EQ(out[0], 0x70);
    CHECK_EQ(out[2], 5u);
    CHECK_EQ(out[7], 10u);
    CHECK_EQ(out[12], 0x21u);
    CHECK_EQ(out[13], 0u);
    CHECK_EQ(out[18], 0u);

    u8 four[4] = {};
    scsi_sense(result, four, sizeof four, written);
    CHECK_EQ(written, 4u);
    CHECK_EQ(four[3], 0u);

    scsi_sense(result, nullptr, 18, written);
    CHECK_EQ(written, 0u);
}

}

int main() {
    geometry_rounds_to_heads_and_clamps_cylinders();
    simple_commands_report_good();
    request_sense_is_a_fixed_format_that_caps_to_allocation();
    inquiry_reports_vendor_product_and_rejects_evpd();
    read_capacity_10_and_16_report_the_last_lba();
    read_6_count_zero_reads_256_sectors();
    read_10_and_16_check_range_and_buffer_size();
    write_commands_fill_the_unit_and_flush();
    write_to_a_read_only_unit_is_protected();
    failed_transfers_report_the_bytes_done();
    verify_and_synchronize_cache();
    mode_sense_6_headers_descriptor_and_pages();
    mode_sense_10_headers_descriptor_and_pages();
    unknown_opcodes_and_short_commands_are_rejected();
    sense_data_is_eighteen_bytes_and_capped();
    return check::finish("test_scsi_disk");
}
