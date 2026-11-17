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

#include "boot_disk.hpp"

#include "../common/amiga_fs.hpp"
#include "../common/dms.hpp"
#include "../common/printable.hpp"
#include "../common/serial.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"
#include "pci.hpp"
#include "virtio_blk.hpp"

namespace hombre {

namespace {

// Constant initialised, so they sit in .bss without a constructor run.
constinit VirtioBlk boot_drive;

constexpr u64 page_size = 4096;
constexpr u64 max_drive_bytes = 8ull << 20;
constexpr u32 max_dms_image_bytes = 2u << 20;
constexpr u32 startup_read_chunk = 512;
constexpr u32 startup_print_cap = 4096;

// A disk image held in memory that the direct map reaches.
struct Image {
    const u8* data;
    usize bytes;
};

constexpr u64 pages_for(u64 bytes) {
    return (bytes + page_size - 1) / page_size;
}

// Prints value in lowercase hex, zero padded to at least `digits` digits.
void write_hex_padded(u64 value, u32 digits) {
    char text[17];  // 16 hex digits for u64 max, plus the terminator
    const usize count = format_u64(text, sizeof(text), value, 16);
    for (usize i = count; i < digits; ++i) {
        serial::write_char('0');
    }
    serial::write(text);
}

bool log_pci_function(void*, PciAddress address, u16 vendor, u16 device) {
    serial::write("pci: ");
    write_hex_padded(address.bus, 2);
    serial::write(":");
    write_hex_padded(address.device, 2);
    serial::write(".");
    write_hex_padded(address.function, 1);
    serial::write(" ");
    write_hex_padded(vendor, 4);
    serial::write(":");
    write_hex_padded(device, 4);
    serial::write("\n");
    return false;
}

// Starts the drive on the PCI bus. Logs one line and returns false when a
// step fails or there is no drive.
bool open_drive(PhysAllocator& phys_pages, MmioMapper& mmio) {
    pci_scan(&log_pci_function, nullptr);

    PciAddress address;
    if (!virtio_blk_find(address)) {
        serial::write("no boot disk\n");
        return false;
    }
    if (!boot_drive.init(address, mmio, phys_pages)) {
        serial::write("virtio-blk: init failed\n");
        return false;
    }
    serial::write("virtio-blk: ");
    serial::write_dec(boot_drive.sector_count());
    serial::write(" sectors\n");
    return true;
}

// Reads the whole drive into fresh pages. Logs one line and returns false on
// failure. The pages are only given back when the device can no longer
// write to them.
bool read_drive(PhysAllocator& phys_pages, Image& drive) {
    const u64 sectors = boot_drive.sector_count();
    if (sectors == 0) {
        serial::write("disk: empty drive\n");
        return false;
    }
    if (sectors > max_drive_bytes / kVirtioBlkSectorSize) {
        serial::write("disk too large\n");
        return false;
    }
    const u64 bytes = sectors * kVirtioBlkSectorSize;
    const u64 page_count = pages_for(bytes);
    const u64 buffer = phys_pages.alloc_pages(page_count);
    if (buffer == 0) {
        serial::write("virtio-blk: no memory\n");
        return false;
    }
    if (!boot_drive.read(0, static_cast<u32>(sectors), buffer)) {
        serial::write("virtio-blk: read failed\n");
        if (!boot_drive.quiesced()) {
            serial::write("virtio-blk: device not reset, buffer kept\n");
        } else {
            phys_pages.free_pages(buffer, page_count);
        }
        return false;
    }
    serial::write("virtio-blk: read ok\n");
    drive = Image{phys_pages.to_ptr(buffer), static_cast<usize>(bytes)};
    return true;
}

void write_dms_error(DmsError error, const DmsInfo& info) {
    serial::write("disk: dms error: ");
    serial::write(dms_error_name(error));
    if (info.failed_track != dms_detail::no_track) {
        serial::write(" track ");
        serial::write_dec(info.failed_track);
    }
    serial::write("\n");
}

// Unpacks the DMS archive in `drive` into fresh pages. Logs one line and
// returns false on failure.
bool unpack_dms(PhysAllocator& phys_pages, const Image& drive, Image& image) {
    DmsInfo info;
    const DmsError header = dms_read_header(drive.data, drive.bytes, info);
    if (header != DmsError::Ok) {
        write_dms_error(header, info);
        return false;
    }
    if (info.image_bytes == 0 || info.image_bytes > max_dms_image_bytes) {
        serial::write("disk: bad image size\n");
        return false;
    }
    const u64 out_pages = pages_for(info.image_bytes);
    const u64 work_pages = pages_for(sizeof(DmsWork));
    const u64 out_phys = phys_pages.alloc_pages(out_pages);
    const u64 work_phys = phys_pages.alloc_pages(work_pages);
    if (out_phys == 0 || work_phys == 0) {
        serial::write("disk: no memory\n");
        return false;
    }
    u8* out = phys_pages.to_ptr(out_phys);
    // The pages come back zeroed, as the unpacker needs.
    DmsWork& work = *reinterpret_cast<DmsWork*>(phys_pages.to_ptr(work_phys));
    const DmsError error =
        dms_unpack(drive.data, drive.bytes, out, info.image_bytes, work, info);
    if (error != DmsError::Ok) {
        write_dms_error(error, info);
        return false;
    }
    serial::write("disk: DMS ");
    serial::write(dms_mode_name(info.mode));
    serial::write(", ");
    serial::write_dec(info.tracks_unpacked);
    serial::write(" tracks\n");
    image = Image{out, info.image_bytes};
    return true;
}

// Names come off the disk, so they go through the printable filter.
void write_name(const char* name, usize max) {
    char text[40];
    sanitize_text(text, sizeof(text), name, max);
    serial::write(text);
}

struct ListContext {
    u32 count;
};

void print_entry(void* ctx, const DirEntry& entry) {
    static_cast<ListContext*>(ctx)->count += 1;
    serial::write("  ");
    write_name(entry.name, sizeof(entry.name));
    if (entry.is_dir) {
        serial::write_char('/');
    }
    serial::write_char('\n');
}

void print_root(const AdfVolume& volume) {
    ListContext context{0};
    const FsError error = adf_list(volume, "", &print_entry, &context);
    if (error != FsError::Ok) {
        serial::write("disk: list error: ");
        serial::write(fs_error_name(error));
        serial::write("\n");
    } else if (context.count == 0) {
        serial::write("  (empty)\n");
    }
}

// Prints `count` bytes of text, two spaces in front of every line. CR is
// dropped, so CRLF and LF both end a line, and bytes that are not printable
// ASCII show as '.'.
void print_text(const u8* text, u32 count, bool& at_line_start) {
    for (u32 i = 0; i < count; ++i) {
        const u8 c = text[i];
        if (c == '\r') {
            continue;
        }
        if (at_line_start) {
            serial::write("  ");
            at_line_start = false;
        }
        if (c == '\n') {
            serial::write_char('\n');
            at_line_start = true;
        } else {
            serial::write_char(printable_char(c));
        }
    }
}

void print_startup_sequence(const AdfVolume& volume) {
    DirEntry file;
    if (adf_find(volume, "S/Startup-Sequence", file) != FsError::Ok || file.is_dir) {
        serial::write("S:Startup-Sequence: not found\n");
        return;
    }
    serial::write("S:Startup-Sequence:\n");

    const u32 limit = file.size < startup_print_cap ? file.size : startup_print_cap;
    u8 chunk[startup_read_chunk];
    bool at_line_start = true;
    u32 inconsistent_total = 0;
    FsError error = FsError::Ok;
    for (u32 offset = 0; offset < limit;) {
        const u32 want = limit - offset < startup_read_chunk ? limit - offset : startup_read_chunk;
        u32 got = 0;
        u32 inconsistent = 0;
        error = adf_read(volume, file, offset, chunk, want, got, &inconsistent);
        inconsistent_total += inconsistent;
        print_text(chunk, got, at_line_start);
        if (error != FsError::Ok || got == 0) {
            break;
        }
        offset += got;
    }
    if (!at_line_start) {
        serial::write_char('\n');
    }
    if (file.size > limit) {
        serial::write("  ...\n");
    }
    if (error != FsError::Ok) {
        serial::write("disk: read error: ");
        serial::write(fs_error_name(error));
        serial::write("\n");
    }
    if (inconsistent_total != 0) {
        serial::write("disk: ");
        serial::write_dec(inconsistent_total);
        serial::write(" inconsistent data blocks\n");
    }
}

void mount_and_print(const Image& image) {
    AdfVolume volume;
    const FsError error = adf_mount(image.data, image.bytes, volume);
    if (error != FsError::Ok) {
        serial::write("disk: mount error: ");
        serial::write(fs_error_name(error));
        serial::write("\n");
        return;
    }
    if (!volume.bootblock_checksum_ok) {
        serial::write("disk: bootblock checksum bad\n");
    }
    serial::write("volume: ");
    write_name(volume.name, sizeof(volume.name));
    serial::write(volume.ffs ? " (FFS)\n" : " (OFS)\n");

    print_root(volume);
    print_startup_sequence(volume);
}

}

void boot_disk_run(const BootInfo&, PhysAllocator& phys_pages, MmioMapper& mmio) {
    if (!open_drive(phys_pages, mmio)) {
        return;
    }
    Image drive;
    if (!read_drive(phys_pages, drive)) {
        return;
    }
    Image image;
    switch (detect_disk_format(drive.data, drive.bytes)) {
    case DiskFormat::Dms:
        if (!unpack_dms(phys_pages, drive, image)) {
            return;
        }
        break;
    case DiskFormat::Adf:
        serial::write("disk: ADF\n");
        image = drive;
        break;
    case DiskFormat::Unknown:
        serial::write("disk: unknown format\n");
        return;
    }
    mount_and_print(image);
}

}
