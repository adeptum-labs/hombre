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

#include "amiga_fs.hpp"
#include "amiga_fs_write.hpp"
#include "check.hpp"
#include "fs_check.hpp"
#include "mkhdf_add.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace hombre;
using namespace mkhdf_add;

namespace {

constexpr u32 kBlock = amiga_fs_detail::block_bytes;
constexpr u32 kBlocks = 4096;
constexpr u32 kReserved = 2;
constexpr AmigaDate kNow{0, 0, 0};

struct Device {
    std::vector<u8> bytes;
    explicit Device(u32 blocks) : bytes(static_cast<usize>(blocks) * kBlock, 0) {}
};

bool device_read(void* ctx, u32 block, u8* out) {
    const Device& d = *static_cast<Device*>(ctx);
    if (static_cast<usize>(block) + 1 > d.bytes.size() / kBlock) {
        return false;
    }
    std::memcpy(out, d.bytes.data() + static_cast<usize>(block) * kBlock, kBlock);
    return true;
}

bool device_write(void* ctx, u32 block, const u8* data) {
    Device& d = *static_cast<Device*>(ctx);
    if (static_cast<usize>(block) + 1 > d.bytes.size() / kBlock) {
        return false;
    }
    std::memcpy(d.bytes.data() + static_cast<usize>(block) * kBlock, data, kBlock);
    return true;
}

// A volume formatted on a memory device and mounted through it.
struct Partition {
    Device device;
    AdfVolume volume{};

    explicit Partition(u8 dos_type, u32 blocks = kBlocks) : device(blocks) {
        const BlockSource source{&device, device_read, device_write};
        CHECK_EQ(adf_format(source, 0, blocks, kReserved, dos_type, "Work", kNow), FsError::Ok);
        CHECK_EQ(adf_mount_partition(source, 0, blocks, kReserved, volume), FsError::Ok);
    }
};

std::vector<u8> pattern(usize length, u32 seed) {
    std::vector<u8> bytes(length);
    u32 x = seed * 2654435761u + 12345u;
    for (usize i = 0; i < length; ++i) {
        x = x * 1664525u + 1013904223u;
        bytes[i] = static_cast<u8>(x >> 24);
    }
    return bytes;
}

std::vector<u8> read_back(const AdfVolume& v, const std::string& path) {
    DirEntry entry{};
    CHECK_EQ(adf_find(v, path.c_str(), entry), FsError::Ok);
    CHECK(!entry.is_dir);
    std::vector<u8> out;
    for (;;) {
        u8 buf[4096];
        u32 got = 0;
        u32 suspect = 1;
        CHECK_EQ(adf_read(v, entry, static_cast<u32>(out.size()), buf, sizeof(buf), got, &suspect),
                 FsError::Ok);
        CHECK_EQ(suspect, 0u);
        out.insert(out.end(), buf, buf + got);
        if (got < sizeof(buf)) {
            return out;
        }
    }
}

AmigaDate date_at(const Partition& p, u32 block) {
    u8 header_buf[kBlock];
    FsError error = FsError::Ok;
    const u8* header = amiga_fs_detail::load_header(p.volume, block, header_buf, error);
    CHECK(header != nullptr);
    if (header == nullptr) {
        return AmigaDate{};
    }
    const u32 at = amiga_fs_write_detail::l_date;
    return AmigaDate{amiga_fs_detail::long_at(header, at),
                     amiga_fs_detail::long_at(header, at + 1),
                     amiga_fs_detail::long_at(header, at + 2)};
}

bool same_date(const AmigaDate& a, const AmigaDate& b) {
    return a.days == b.days && a.minutes == b.minutes && a.ticks == b.ticks;
}

u32 block_of(const Partition& p, const std::string& path) {
    DirEntry entry{};
    CHECK_EQ(adf_find(p.volume, path.c_str(), entry), FsError::Ok);
    return entry.header_block;
}

void add_into_new_tree() {
    Partition p(0);
    const std::vector<u8> data = pattern(5000, 7);
    CHECK_EQ(add_file(p.volume, {"Disks", "Games", "one.adf"}, data, kNow), AddError::Ok);
    DirEntry entry{};
    CHECK_EQ(adf_find(p.volume, "Disks", entry), FsError::Ok);
    CHECK(entry.is_dir);
    CHECK_EQ(adf_find(p.volume, "Disks/Games", entry), FsError::Ok);
    CHECK(entry.is_dir);
    CHECK(read_back(p.volume, "Disks/Games/one.adf") == data);
    CHECK(fs_check(p.volume).empty());
}

void add_into_existing_folder() {
    Partition p(0);
    const std::vector<u8> first = pattern(1000, 1);
    const std::vector<u8> second = pattern(2000, 2);
    CHECK_EQ(add_file(p.volume, {"Disks", "Games", "one.adf"}, first, kNow), AddError::Ok);
    CHECK_EQ(add_file(p.volume, {"Disks", "Games", "two.adf"}, second, kNow), AddError::Ok);
    CHECK(read_back(p.volume, "Disks/Games/one.adf") == first);
    CHECK(read_back(p.volume, "Disks/Games/two.adf") == second);
    CHECK(fs_check(p.volume).empty());
}

void add_large_file() {
    Partition p(1);  // FFS, so a data block holds a whole 512 bytes.
    constexpr usize kSize = 200000;
    // Well past the 72 pointers of a file header, so extension blocks hold it.
    CHECK(kSize / kBlock > amiga_fs_detail::hash_slots);
    const std::vector<u8> data = pattern(kSize, 3);
    CHECK_EQ(add_file(p.volume, {"big", "big.bin"}, data, kNow), AddError::Ok);
    CHECK(read_back(p.volume, "big/big.bin") == data);
    CHECK(fs_check(p.volume).empty());
}

void added_files_and_folders_carry_the_date_passed() {
    Partition p(0);
    const AmigaDate file_date{1234, 567, 42};
    const AmigaDate dir_date{2345, 678, 43};
    CHECK_EQ(add_file(p.volume, {"DEVS", "DataTypes", "one.datatype"}, pattern(10, 1),
                      file_date, dir_date),
             AddError::Ok);
    CHECK(same_date(date_at(p, p.volume.root_block), dir_date));
    CHECK(same_date(date_at(p, block_of(p, "DEVS")), dir_date));
    CHECK(same_date(date_at(p, block_of(p, "DEVS/DataTypes")), dir_date));
    CHECK(same_date(date_at(p, block_of(p, "DEVS/DataTypes/one.datatype")), file_date));
    CHECK(fs_check(p.volume).empty());
}

void an_empty_file_also_takes_the_file_date() {
    Partition p(0);
    const AmigaDate file_date{3456, 789, 44};
    const AmigaDate dir_date{4567, 890, 45};
    CHECK_EQ(add_file(p.volume, {"empty"}, {}, file_date, dir_date), AddError::Ok);
    CHECK(same_date(date_at(p, block_of(p, "empty")), file_date));
}

void a_zero_date_leaves_the_dates_zero() {
    Partition p(0);
    CHECK_EQ(add_file(p.volume, {"a", "b"}, pattern(4, 2), kNow), AddError::Ok);
    CHECK(same_date(date_at(p, p.volume.root_block), kNow));
    CHECK(same_date(date_at(p, block_of(p, "a")), kNow));
    CHECK(same_date(date_at(p, block_of(p, "a/b")), kNow));
}

void unix_times_count_amiga_days_minutes_and_ticks() {
    constexpr long long kEpoch = 2922 * 86400;
    const AmigaDate start = date_of_time(static_cast<std::time_t>(kEpoch));
    CHECK_EQ(start.days, 0u);
    CHECK_EQ(start.minutes, 0u);
    CHECK_EQ(start.ticks, 0u);
    const AmigaDate later = date_of_time(static_cast<std::time_t>(kEpoch + 86400 + 61));
    CHECK_EQ(later.days, 1u);
    CHECK_EQ(later.minutes, 1u);
    CHECK_EQ(later.ticks, 50u);
}

void error_cases() {
    AddSpec spec;
    CHECK_EQ(parse_add("DH0:Disks/Games/one.adf=/tmp/one.adf", spec), AddError::Ok);
    CHECK(spec.volume == "DH0");
    CHECK(spec.path.size() == 3);
    CHECK(spec.path[0] == "Disks" && spec.path[1] == "Games" && spec.path[2] == "one.adf");
    CHECK(spec.host_file == "/tmp/one.adf");
    CHECK_EQ(parse_add("DH0:" + std::string(30, 'n') + "=/tmp/x", spec), AddError::Ok);
    CHECK_EQ(parse_add("DH0:" + std::string(31, 'n') + "=/tmp/x", spec), AddError::NameTooLong);
    CHECK_EQ(parse_add("DH0:Disks:bad=/tmp/x", spec), AddError::ColonInPath);
    CHECK_EQ(parse_add("DH0:path", spec), AddError::BadArgument);
    CHECK_EQ(parse_add("DH0:=/tmp/x", spec), AddError::BadArgument);

    CHECK_EQ(find_volume({"DH0", "DH1"}, "dh1"), 1);
    CHECK_EQ(find_volume({"DH0"}, "DH9"), -1);

    std::vector<u8> data;
    CHECK_EQ(read_host_file("/nonexistent/mkhdf_add_test_file", data),
             AddError::HostUnreadable);

    Partition p(1);
    const std::vector<u8> bytes = pattern(10, 4);
    CHECK_EQ(add_file(p.volume, {"a", "b.txt"}, bytes, kNow), AddError::Ok);
    CHECK_EQ(add_file(p.volume, {"a", "b.txt"}, bytes, kNow), AddError::Exists);
    CHECK_EQ(add_file(p.volume, {"a", "b.txt", "c"}, bytes, kNow), AddError::NotDir);

    Partition tiny(1, 64);
    CHECK_EQ(add_file(tiny.volume, {"big"}, pattern(200000, 5), kNow), AddError::DiskFull);
}

}

int main() {
    add_into_new_tree();
    add_into_existing_folder();
    add_large_file();
    added_files_and_folders_carry_the_date_passed();
    an_empty_file_also_takes_the_file_date();
    a_zero_date_leaves_the_dates_zero();
    unix_times_count_amiga_days_minutes_and_ticks();
    error_cases();
    return check::finish("test_mkhdf_add");
}
