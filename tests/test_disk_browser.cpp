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

#include "check.hpp"
#include "disk_browser.hpp"
#include "ofs_writer.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace hombre;

namespace {

constexpr u32 kBlock = 512;
constexpr u32 kBlocks = 1760;

AdfVolume mount(const std::vector<u8>& image) {
    AdfVolume volume{};
    CHECK_EQ(adf_mount(image.data(), image.size(), volume), FsError::Ok);
    return volume;
}

std::vector<std::string> names(const MonitorListing& listing) {
    std::vector<std::string> out;
    for (u32 i = 0; i < listing.count; ++i) {
        out.emplace_back(listing.entries[i].name);
    }
    return out;
}

std::vector<std::string> expected(std::initializer_list<const char*> list) {
    std::vector<std::string> out;
    for (const char* name : list) {
        out.emplace_back(name);
    }
    return out;
}

std::vector<AdfFile> sample_files() {
    return {
        {"Games/one.adf", {}},
        {"Games/Two.DMS", {}},
        {"Games/notes.txt", {}},
        {"Docs/readme", {}},
        {"Music/song.mod", {}},
        {"Beta.ADF", {}},
        {"alpha.dms", {}},
        {"Gamma.txt", {}},
    };
}

void lists_directories_first_then_names() {
    const std::vector<u8> image = build_adf("Work", false, sample_files());
    const AdfVolume volume = mount(image);
    MonitorListing listing;
    disk_browser_list_directory(volume, "", listing);

    CHECK_EQ(listing.count, 5u);
    CHECK(!listing.truncated);
    CHECK(listing.error[0] == '\0');
    CHECK(std::string(listing.path).empty());
    CHECK(names(listing) == expected({"Docs", "Games", "Music", "alpha.dms", "Beta.ADF"}));
    CHECK(listing.entries[0].is_dir);
    CHECK(listing.entries[1].is_dir);
    CHECK(listing.entries[2].is_dir);
    CHECK(!listing.entries[3].is_dir);
    CHECK(!listing.entries[4].is_dir);
}

void lists_one_directory() {
    const std::vector<u8> image = build_adf("Work", false, sample_files());
    const AdfVolume volume = mount(image);
    MonitorListing listing;
    disk_browser_list_directory(volume, "Games", listing);

    CHECK(std::string(listing.path) == std::string("Games"));
    CHECK(names(listing) == expected({"one.adf", "Two.DMS"}));
    CHECK(!listing.truncated);
    CHECK(listing.error[0] == '\0');
}

void reports_errors() {
    const std::vector<u8> image = build_adf("Work", false, sample_files());
    const AdfVolume volume = mount(image);

    MonitorListing listing;
    disk_browser_list_directory(volume, "Gamma.txt", listing);
    CHECK_EQ(listing.count, 0u);
    CHECK(std::string(listing.error) == std::string(fs_error_name(FsError::NotDir)));

    disk_browser_list_directory(volume, "Missing", listing);
    CHECK_EQ(listing.count, 0u);
    CHECK(std::string(listing.error) == std::string(fs_error_name(FsError::NotFound)));
}

struct Device {
    const std::vector<u8>& bytes;
    bool fail = false;
};

bool read_block(void* ctx, u32 block, u8* out) {
    Device& device = *static_cast<Device*>(ctx);
    if (device.fail || block >= device.bytes.size() / kBlock) {
        return false;
    }
    std::memcpy(out, device.bytes.data() + static_cast<usize>(block) * kBlock, kBlock);
    return true;
}

void reports_an_unreadable_directory() {
    const std::vector<u8> image = build_adf("Work", false, sample_files());
    Device device{image};
    const BlockSource source{&device, &read_block, nullptr, nullptr};
    AdfVolume volume{};
    CHECK_EQ(adf_mount_partition(source, 0, kBlocks, 2, volume), FsError::Ok);

    device.fail = true;
    MonitorListing listing;
    disk_browser_list_directory(volume, "", listing);
    CHECK_EQ(listing.count, 0u);
    CHECK(std::string(listing.error) == std::string(fs_error_name(FsError::Io)));
}

void marks_a_listing_it_cannot_hold_whole() {
    std::vector<AdfFile> files;
    for (u32 i = 0; i < MonitorSnapshot::kMaxEntries + 4; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "f%03u.adf", i);
        files.push_back({name, {}});
    }
    const std::vector<u8> image = build_adf("Work", false, files);
    const AdfVolume volume = mount(image);
    MonitorListing listing;
    disk_browser_list_directory(volume, "", listing);

    CHECK_EQ(listing.count, MonitorSnapshot::kMaxEntries);
    CHECK(listing.truncated);
    CHECK(std::string(listing.entries[0].name) == std::string("f000.adf"));
    CHECK(std::string(listing.entries[MonitorSnapshot::kMaxEntries - 1].name) ==
          std::string("f255.adf"));
}

void lists_partitions_as_directories() {
    const char* devices[] = {"DH0", "DH1", "", nullptr, "DH2"};
    MonitorListing listing;
    disk_browser_list_partitions(devices, 5, listing);

    CHECK_EQ(listing.count, 3u);
    CHECK(names(listing) == expected({"DH0:", "DH1:", "DH2:"}));
    CHECK(listing.entries[0].is_dir);
    CHECK(listing.entries[1].is_dir);
    CHECK(listing.entries[2].is_dir);
    CHECK(!listing.truncated);
    CHECK(listing.error[0] == '\0');
}

void splits_paths() {
    char partition[kDiskPartitionBytes];
    char rest[kDiskPathBytes];

    CHECK(disk_browser_split("DH0:Disks/x.adf", partition, rest));
    CHECK(std::string(partition) == std::string("DH0"));
    CHECK(std::string(rest) == std::string("Disks/x.adf"));

    CHECK(disk_browser_split("DH1:", partition, rest));
    CHECK(std::string(partition) == std::string("DH1"));
    CHECK(std::string(rest).empty());

    CHECK(disk_browser_split("DH2://Games//one.dms", partition, rest));
    CHECK(std::string(partition) == std::string("DH2"));
    CHECK(std::string(rest) == std::string("Games//one.dms"));

    CHECK(!disk_browser_split("", partition, rest));
    CHECK(!disk_browser_split("Disks/x.adf", partition, rest));
    CHECK(!disk_browser_split(":x", partition, rest));
}

}

int main() {
    lists_directories_first_then_names();
    lists_one_directory();
    reports_errors();
    reports_an_unreadable_directory();
    marks_a_listing_it_cannot_hold_whole();
    lists_partitions_as_directories();
    splits_paths();
    return check::finish("test_disk_browser");
}
