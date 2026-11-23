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
// first one bootable, laid out on one surface of 32 blocks a track. Each
// partition is formatted with the floppy's file system and volume name, is
// at least 8 MiB, and gets a copy of the floppy's files, so it has a valid
// bitmap and room to write to.

#include "amiga_fs.hpp"
#include "amiga_fs_write.hpp"
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
constexpr u32 kMinPartitionBlocks = 16384;

// A partition of the disk image being built, read and written in place.
struct Window {
    std::vector<u8>* disk;
    u32 first;
};

bool window_read(void* ctx, u32 block, u8* out) {
    const Window& w = *static_cast<Window*>(ctx);
    std::memcpy(out, w.disk->data() + (static_cast<usize>(w.first) + block) * 512, 512);
    return true;
}

bool window_write(void* ctx, u32 block, const u8* data) {
    const Window& w = *static_cast<Window*>(ctx);
    std::memcpy(w.disk->data() + (static_cast<usize>(w.first) + block) * 512, data, 512);
    return true;
}

void collect(void* ctx, const DirEntry& entry) {
    static_cast<std::vector<DirEntry>*>(ctx)->push_back(entry);
}

// Copies the directory at `path` of the floppy into the directory block
// `dir` of the partition, and everything below it.
bool copy_tree(const AdfVolume& from, const std::string& path, const AdfVolume& to, u32 dir) {
    std::vector<DirEntry> entries;
    if (adf_list(from, path.c_str(), collect, &entries) != FsError::Ok) {
        std::fprintf(stderr, "cannot list %s\n", path.c_str());
        return false;
    }
    const AmigaDate now{0, 0, 0};
    for (const DirEntry& e : entries) {
        const std::string child = path.empty() ? e.name : path + "/" + e.name;
        DirEntry made;
        if (e.is_dir) {
            if (adf_create_dir(to, dir, e.name, now, made) != FsError::Ok ||
                !copy_tree(from, child, to, made.header_block)) {
                std::fprintf(stderr, "cannot copy %s\n", child.c_str());
                return false;
            }
            continue;
        }
        std::vector<u8> data(e.size);
        u32 got = 0;
        if (adf_read(from, e, 0, data.data(), e.size, got) != FsError::Ok || got != e.size) {
            std::fprintf(stderr, "cannot read %s\n", child.c_str());
            return false;
        }
        u32 written = 0;
        if (adf_create_file(to, dir, e.name, now, made) != FsError::Ok ||
            adf_write(to, made.header_block, 0, data.data(), e.size, now, written) !=
                FsError::Ok) {
            std::fprintf(stderr, "cannot write %s\n", child.c_str());
            return false;
        }
    }
    return true;
}

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

// The RDB disk as one GPT partition of a disk with 4096 byte sectors, as an
// NVMe drive formatted that way has it. The partition starts at 1 MiB.
std::vector<u8> wrap_in_gpt4k(const std::vector<u8>& rdb) {
    constexpr u64 kLba = 4096;
    constexpr u64 kFirstLba = 256;
    const u64 rdb_lbas = (rdb.size() + kLba - 1) / kLba;
    // Room behind the partition for the backup entry array and header.
    const u64 disk_lbas = kFirstLba + rdb_lbas + 8;
    std::vector<u8> disk(disk_lbas * kLba, 0);
    std::memcpy(disk.data() + kFirstLba * kLba, rdb.data(), rdb.size());
    write_gpt(disk, {{kFirstLba, kFirstLba + rdb_lbas - 1, true}}, kLba);
    return disk;
}

}

int main(int argc, char** argv) {
    bool gpt4k = false;
    if (argc > 1 && std::strcmp(argv[1], "--gpt4k") == 0) {
        gpt4k = true;
        --argc;
        ++argv;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: mkhdf [--gpt4k] <out.hdf> <adf>...\n");
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
        u32 blocks = static_cast<u32>(floppies[i].size() / 512);
        if (blocks < kMinPartitionBlocks) {
            blocks = kMinPartitionBlocks;
        }
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
        AdfVolume from{};
        if (adf_mount(floppies[i].data(), floppies[i].size(), from) != FsError::Ok) {
            std::fprintf(stderr, "%s: cannot mount\n", argv[2 + i]);
            return 1;
        }
        Window window{&disk, parts[i].low_cyl * kSectorsPerTrack};
        const BlockSource source{&window, window_read, window_write};
        const u32 blocks = (parts[i].high_cyl - parts[i].low_cyl + 1) * kSectorsPerTrack;
        AdfVolume to{};
        if (adf_format(source, 0, blocks, 2, from.dos_type, from.name, AmigaDate{0, 0, 0}) !=
                FsError::Ok ||
            adf_mount_partition(source, 0, blocks, 2, to) != FsError::Ok ||
            !copy_tree(from, "", to, to.root_block)) {
            std::fprintf(stderr, "%s: cannot build its partition\n", argv[2 + i]);
            return 1;
        }
    }
    if (gpt4k) {
        disk = wrap_in_gpt4k(disk);
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
