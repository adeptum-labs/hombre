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


// Builds an Amiga hard disk image with a Rigid Disk Block.
//
//   mkhdf <out.hdf> <adf> [<adf>...]
//
// Each ADF or DMS floppy image becomes a partition of its own, DH0: on, the
// first one bootable, laid out on one surface of 32 blocks a track. A
// partition of exactly a floppy's blocks keeps the floppy's root block where
// a partition puts it, so the floppy's volume mounts unchanged.

#include "amiga_fs.hpp"
#include "dms.hpp"
#include "rdb_writer.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace hombre;

namespace {

constexpr u32 kSectorsPerTrack = 32;
constexpr u32 kRdbCylinders = 2;

bool read_file(const char* path, std::vector<u8>& data) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::perror(path);
        return false;
    }
    u8 chunk[65536];
    usize got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        data.insert(data.end(), chunk, chunk + got);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) {
        std::fprintf(stderr, "%s: read error\n", path);
    }
    return ok;
}

// The floppy image in the file, unpacked when it is a DMS archive.
bool load_floppy(const char* path, std::vector<u8>& image) {
    std::vector<u8> data;
    if (!read_file(path, data)) {
        return false;
    }
    if (detect_disk_format(data.data(), data.size()) == DiskFormat::Dms) {
        DmsInfo info;
        if (dms_read_header(data.data(), data.size(), info) != DmsError::Ok) {
            std::fprintf(stderr, "%s: bad DMS header\n", path);
            return false;
        }
        image.assign(info.image_bytes, 0);
        auto work = std::make_unique<DmsWork>();
        if (dms_unpack(data.data(), data.size(), image.data(), image.size(), *work, info) !=
            DmsError::Ok) {
            std::fprintf(stderr, "%s: DMS unpack failed\n", path);
            return false;
        }
        image.resize(info.image_bytes);
        return true;
    }
    if (detect_disk_format(data.data(), data.size()) != DiskFormat::Adf) {
        std::fprintf(stderr, "%s: not a floppy image\n", path);
        return false;
    }
    image = std::move(data);
    return true;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: mkhdf <out.hdf> <adf>...\n");
        return 2;
    }
    std::vector<std::vector<u8>> floppies;
    for (int i = 2; i < argc; ++i) {
        std::vector<u8> image;
        if (!load_floppy(argv[i], image)) {
            return 1;
        }
        floppies.push_back(std::move(image));
    }
    RdbGeometry geometry;
    geometry.heads = 1;
    geometry.sectors = kSectorsPerTrack;
    geometry.rdb_cylinders = kRdbCylinders;
    std::vector<RdbPartitionSpec> parts;
    u32 cylinder = kRdbCylinders;
    for (usize i = 0; i < floppies.size(); ++i) {
        const u32 blocks = static_cast<u32>(floppies[i].size() / 512);
        const u32 cylinders = blocks / kSectorsPerTrack;
        RdbPartitionSpec spec;
        spec.name = "DH" + std::to_string(i);
        spec.low_cyl = cylinder;
        spec.high_cyl = cylinder + cylinders - 1;
        spec.dos_type = 0x444F5300u | floppies[i][3];
        spec.bootable = i == 0;
        parts.push_back(spec);
        cylinder += cylinders;
    }
    geometry.cylinders = cylinder;
    std::vector<u8> disk = build_rdb_disk(geometry, parts);
    for (usize i = 0; i < floppies.size(); ++i) {
        const usize at = static_cast<usize>(parts[i].low_cyl) * kSectorsPerTrack * 512;
        std::memcpy(disk.data() + at, floppies[i].data(), floppies[i].size());
    }
    std::FILE* f = std::fopen(argv[1], "wb");
    if (f == nullptr) {
        std::perror(argv[1]);
        return 1;
    }
    const bool ok = std::fwrite(disk.data(), 1, disk.size(), f) == disk.size();
    if (std::fclose(f) != 0 || !ok) {
        std::fprintf(stderr, "%s: write error\n", argv[1]);
        return 1;
    }
    return 0;
}
