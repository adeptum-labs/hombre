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

// Host front end for the kernel's disk image code.
//
//   adftool unpack <in.dms> <out.adf>   unpack a DMS archive to a raw image
//   adftool info <image>                volume name, file system, bootblock checksum
//   adftool ls <image> [path]           list a directory, one entry per line
//   adftool cat <image> <path>          write a file to standard output
//
// An image is a DMS archive, a raw .adf, or a hard disk image, whose first
// Amiga partition is the volume read (an RDB disk, a bare DOS volume, or an
// RDB in an MBR or GPT partition). A listing line is the size in ten
// columns ("<dir>" for a directory), two blanks and the name, with a slash
// after the name of a directory.

#include "amiga_fs.hpp"
#include "dms.hpp"
#include "pc_partitions.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace hombre;

namespace {

constexpr usize kMaxImage = 80u * 22528u;  // an HD disk

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

bool write_file(const char* path, const u8* data, usize length) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::perror(path);
        return false;
    }
    const bool ok = std::fwrite(data, 1, length, f) == length;
    if (std::fclose(f) != 0 || !ok) {
        std::fprintf(stderr, "%s: write error\n", path);
        return false;
    }
    return true;
}

int unpack(const char* in_path, const char* out_path) {
    std::vector<u8> in;
    if (!read_file(in_path, in)) {
        return 1;
    }
    auto work = std::make_unique<DmsWork>();
    std::vector<u8> image(kMaxImage);
    DmsInfo info{};
    const DmsError error = dms_unpack(in.data(), in.size(), image.data(), image.size(), *work, info);
    if (error != DmsError::Ok) {
        std::fprintf(stderr, "%s: %s", in_path, dms_error_name(error));
        if (info.failed_track != 0xFFFFFFFFu) {
            std::fprintf(stderr, " (track %u)", info.failed_track);
        }
        std::fprintf(stderr, "\n");
        return 1;
    }
    return write_file(out_path, image.data(), info.image_bytes) ? 0 : 1;
}

// Reads a DMS archive or a raw image into `image`.
bool load_image(const char* path, std::vector<u8>& image) {
    std::vector<u8> in;
    if (!read_file(path, in)) {
        return false;
    }
    switch (detect_disk_format(in.data(), in.size())) {
    case DiskFormat::Adf:
        image = std::move(in);
        return true;
    case DiskFormat::Dms: {
        auto work = std::make_unique<DmsWork>();
        image.assign(kMaxImage, 0);
        DmsInfo info{};
        const DmsError error = dms_unpack(in.data(), in.size(), image.data(), image.size(), *work, info);
        if (error != DmsError::Ok) {
            std::fprintf(stderr, "%s: %s", path, dms_error_name(error));
            if (info.failed_track != 0xFFFFFFFFu) {
                std::fprintf(stderr, " (track %u)", info.failed_track);
            }
            std::fprintf(stderr, "\n");
            return false;
        }
        image.resize(info.image_bytes);
        return true;
    }
    case DiskFormat::Unknown:
        // Anything else is taken for a hard disk image.
        if (in.size() >= 4 * 512 && in.size() % 512 == 0) {
            image = std::move(in);
            return true;
        }
        break;
    }
    std::fprintf(stderr, "%s: neither a DMS archive nor a disk image\n", path);
    return false;
}

// The hard disk image a partition is read from, and where the partition starts.
struct HardDisk {
    const std::vector<u8>* image;
    u64 first;
};

HardDisk hard_disk{};

bool read_disk(void* ctx, u64 sector, u8* out) {
    const std::vector<u8>& image = *static_cast<const std::vector<u8>*>(ctx);
    if ((sector + 1) * 512 > image.size()) {
        return false;
    }
    std::memcpy(out, image.data() + sector * 512, 512);
    return true;
}

bool read_partition(void* ctx, u32 block, u8* out) {
    const HardDisk& disk = *static_cast<const HardDisk*>(ctx);
    return read_disk(const_cast<std::vector<u8>*>(disk.image), disk.first + block, out);
}

// Mounts the first Amiga partition of a hard disk image. An image of a disk
// with 4096 byte sectors has its partition table in those, so both sizes are
// tried.
FsError mount_hard_disk(const std::vector<u8>& image, AdfVolume& volume) {
    AmigaPartitions parts{};
    RdbError error = RdbError::NoPartitions;
    for (const u32 lba_bytes : {512u, 4096u}) {
        error = find_amiga_partitions(read_disk, const_cast<std::vector<u8>*>(&image),
                                      image.size() / 512, parts, lba_bytes);
        if (error == RdbError::Ok && parts.count > 0) {
            break;
        }
    }
    if (error != RdbError::Ok || parts.count == 0) {
        return FsError::NotDos;
    }
    const RdbPartition& part = parts.list[0].part;
    hard_disk = HardDisk{&image, parts.list[0].disk_first_sector};
    const BlockSource source{&hard_disk, read_partition, nullptr};
    return adf_mount_partition(source, 0, part.block_count, part.reserved, volume);
}

bool mount(const char* path, const std::vector<u8>& image, AdfVolume& volume) {
    const FsError error = detect_disk_format(image.data(), image.size()) == DiskFormat::Adf
                              ? adf_mount(image.data(), image.size(), volume)
                              : mount_hard_disk(image, volume);
    if (error != FsError::Ok) {
        std::fprintf(stderr, "%s: %s\n", path, fs_error_name(error));
        return false;
    }
    return true;
}

int info(const char* path) {
    std::vector<u8> image;
    AdfVolume volume{};
    if (!load_image(path, image) || !mount(path, image, volume)) {
        return 1;
    }
    std::printf("volume: %s\n", volume.name);
    std::printf("type: %s\n", volume.ffs ? "FFS" : "OFS");
    std::printf("intl: %s\n", volume.intl ? "yes" : "no");
    std::printf("bootblock checksum: %s\n", volume.bootblock_checksum_ok ? "ok" : "bad");
    return 0;
}

void print_entry(void*, const DirEntry& entry) {
    if (entry.is_dir) {
        std::printf("%10s  %s/\n", "<dir>", entry.name);
    } else {
        std::printf("%10u  %s\n", entry.size, entry.name);
    }
}

int list(const char* image_path, const char* path) {
    std::vector<u8> image;
    AdfVolume volume{};
    if (!load_image(image_path, image) || !mount(image_path, image, volume)) {
        return 1;
    }
    const FsError error = adf_list(volume, path, print_entry, nullptr);
    if (error != FsError::Ok) {
        std::fprintf(stderr, "%s: %s: %s\n", image_path, path, fs_error_name(error));
        return 1;
    }
    return 0;
}

int cat(const char* image_path, const char* path) {
    std::vector<u8> image;
    AdfVolume volume{};
    if (!load_image(image_path, image) || !mount(image_path, image, volume)) {
        return 1;
    }
    DirEntry file{};
    FsError error = adf_find(volume, path, file);
    if (error == FsError::Ok && file.is_dir) {
        error = FsError::NotFile;
    }
    // A multiple of both block payloads, so no block is read twice.
    constexpr u32 kChunk = 488u * 512u;
    std::vector<u8> chunk(kChunk);
    u32 offset = 0;
    u32 inconsistent = 0;
    while (error == FsError::Ok && offset < file.size) {
        u32 got = 0;
        u32 suspect = 0;
        error = adf_read(volume, file, offset, chunk.data(), kChunk, got, &suspect);
        inconsistent += suspect;
        if (error == FsError::Ok && got == 0) {
            error = FsError::BadBlock;
        }
        if (error == FsError::Ok) {
            if (std::fwrite(chunk.data(), 1, got, stdout) != got) {
                std::fprintf(stderr, "write error\n");
                return 1;
            }
            offset += got;
        }
    }
    if (error != FsError::Ok) {
        std::fprintf(stderr, "%s: %s: %s\n", image_path, path, fs_error_name(error));
        return 1;
    }
    if (inconsistent != 0) {
        std::fprintf(stderr, "%s: %s: warning: %u data blocks fail their checksum, key or sequence\n",
                     image_path, path, inconsistent);
    }
    return std::fflush(stdout) == 0 ? 0 : 1;
}

void usage() {
    std::fprintf(stderr,
                 "usage: adftool unpack <in.dms> <out.adf>\n"
                 "       adftool info <image>\n"
                 "       adftool ls <image> [path]\n"
                 "       adftool cat <image> <path>\n");
}

}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "unpack") == 0) {
        return unpack(argv[2], argv[3]);
    }
    if (argc == 3 && std::strcmp(argv[1], "info") == 0) {
        return info(argv[2]);
    }
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "ls") == 0) {
        return list(argv[2], argc == 4 ? argv[3] : "");
    }
    if (argc == 4 && std::strcmp(argv[1], "cat") == 0) {
        return cat(argv[2], argv[3]);
    }
    usage();
    return 2;
}
