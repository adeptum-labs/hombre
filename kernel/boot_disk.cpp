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
#include "usb_keyboard.hpp"

#include "../common/amiga_fs.hpp"
#include "../common/dms.hpp"
#include "../common/printable.hpp"
#include "../common/serial.hpp"
#include "m68k_run.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"
#include "pci.hpp"
#include "virtio_blk.hpp"

namespace hombre {

namespace {

// Constant initialised, so they sit in .bss without a constructor run:
// the floppy drives DF0: to DF3:.
constinit VirtioBlk drive_devices[kFloppyDrives];

// The virtio-blk device in PCI slot kFirstDriveSlot + n is drive n.
constexpr u8 kFirstDriveSlot = 0x10;

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

// Starts a drive's device. Logs one line, after the prefix, and returns
// false when a step fails.
bool open_drive(PhysAllocator& phys_pages, MmioMapper& mmio, VirtioBlk& device,
                PciAddress address, const char* prefix) {
    serial::write(prefix);
    if (!device.init(address, mmio, phys_pages)) {
        serial::write("virtio-blk: init failed\n");
        return false;
    }
    serial::write("virtio-blk: ");
    serial::write_dec(device.sector_count());
    serial::write(" sectors\n");
    return true;
}

// Reads the whole drive into fresh pages. Logs one line and returns false on
// failure. The pages are only given back when the device can no longer
// write to them.
bool read_drive(PhysAllocator& phys_pages, VirtioBlk& device, const char* prefix,
                Image& drive) {
    const u64 sectors = device.sector_count();
    serial::write(prefix);
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
    if (!device.read(0, static_cast<u32>(sectors), buffer)) {
        serial::write("virtio-blk: read failed\n");
        if (!device.quiesced()) {
            serial::write(prefix);
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

void write_dms_error(DmsError error, const DmsInfo& info, const char* prefix) {
    serial::write(prefix);
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
bool unpack_dms(PhysAllocator& phys_pages, const Image& drive, const char* prefix,
                Image& image) {
    DmsInfo info;
    const DmsError header = dms_read_header(drive.data, drive.bytes, info);
    if (header != DmsError::Ok) {
        write_dms_error(header, info, prefix);
        return false;
    }
    serial::write(prefix);
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
        write_dms_error(error, info, "");
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

// Reads the drive's disk and unpacks it when it is a DMS archive. Logs,
// after the prefix, and returns false when a step fails.
bool load_disk(PhysAllocator& phys_pages, MmioMapper& mmio, VirtioBlk& device,
               PciAddress address, const char* prefix, Image& image) {
    if (!open_drive(phys_pages, mmio, device, address, prefix)) {
        return false;
    }
    Image drive;
    if (!read_drive(phys_pages, device, prefix, drive)) {
        return false;
    }
    switch (detect_disk_format(drive.data, drive.bytes)) {
    case DiskFormat::Dms:
        return unpack_dms(phys_pages, drive, prefix, image);
    case DiskFormat::Adf:
        serial::write(prefix);
        serial::write("disk: ADF\n");
        image = drive;
        return true;
    case DiskFormat::Unknown:
        serial::write(prefix);
        serial::write("disk: unknown format\n");
        return false;
    }
    return false;
}

// Mounts the disk's volume. Logs, after the prefix, the volume or why it
// does not mount.
bool mount_disk(const Image& image, const char* prefix, AdfVolume& volume) {
    const FsError error = adf_mount(image.data, image.bytes, volume);
    serial::write(prefix);
    if (error != FsError::Ok) {
        serial::write("disk: mount error: ");
        serial::write(fs_error_name(error));
        serial::write("\n");
        return false;
    }
    if (!volume.bootblock_checksum_ok) {
        serial::write("disk: bootblock checksum bad\n");
        serial::write(prefix);
    }
    serial::write("volume: ");
    write_name(volume.name, sizeof(volume.name));
    serial::write(volume.ffs ? " (FFS)\n" : " (OFS)\n");
    return true;
}

// The drive a virtio-blk device at the address is: its PCI slot on bus 0,
// or kFloppyDrives for none.
u32 drive_of(PciAddress address) {
    if (address.bus != 0 || address.function != 0 || address.device < kFirstDriveSlot ||
        address.device >= kFirstDriveSlot + kFloppyDrives) {
        return kFloppyDrives;
    }
    return address.device - kFirstDriveSlot;
}

bool same_address(PciAddress a, PciAddress b) {
    return a.bus == b.bus && a.device == b.device && a.function == b.function;
}

// The volumes stay mounted, and their images in memory, for good, as dos
// needs them.
constinit AdfVolume drive_volumes[kFloppyDrives]{};

}

void boot_disk_run(const BootInfo& info, PhysAllocator& phys_pages, MmioMapper& mmio) {
    pci_scan(&log_pci_function, nullptr);

    // Each drive by its slot; a single device elsewhere is DF0: as before
    // drives had slots.
    PciAddress found[kFloppyDrives * 2];
    const u32 count = virtio_blk_find_all(found, kFloppyDrives * 2);
    PciAddress addresses[kFloppyDrives]{};
    bool present[kFloppyDrives]{};
    for (u32 i = 0; i < count; ++i) {
        const u32 drive = drive_of(found[i]);
        if (drive < kFloppyDrives && !present[drive]) {
            addresses[drive] = found[i];
            present[drive] = true;
        }
    }
    if (!present[0] && count == 1 && drive_of(found[0]) == kFloppyDrives) {
        addresses[0] = found[0];
        present[0] = true;
    }
    for (u32 i = 0; i < count; ++i) {
        bool used = false;
        for (u32 n = 0; n < kFloppyDrives; ++n) {
            used = used || (present[n] && same_address(addresses[n], found[i]));
        }
        if (!used) {
            serial::write("virtio-blk: device in no drive slot ignored\n");
        }
    }
    if (!present[0]) {
        serial::write("no boot disk\n");
        return;
    }

    // DF0: holds the boot volume; without it nothing starts.
    Image boot;
    if (!load_disk(phys_pages, mmio, drive_devices[0], addresses[0], "", boot)) {
        return;
    }
    usb_keyboard_start(mmio, phys_pages);
    if (!mount_disk(boot, "", drive_volumes[0])) {
        return;
    }
    print_root(drive_volumes[0]);
    print_startup_sequence(drive_volumes[0]);

    // DF1: to DF3: are empty unless their disk reads and mounts.
    Drives drives;
    drives.volume[0] = &drive_volumes[0];
    for (u32 n = 1; n < kFloppyDrives; ++n) {
        char prefix[] = "drive 0: ";
        prefix[6] = static_cast<char>('0' + n);
        if (!present[n]) {
            serial::write(prefix);
            serial::write("no disk\n");
            continue;
        }
        Image image;
        if (load_disk(phys_pages, mmio, drive_devices[n], addresses[n], prefix, image) &&
            mount_disk(image, prefix, drive_volumes[n])) {
            drives.volume[n] = &drive_volumes[n];
        }
    }
    m68k_run_command(info, phys_pages, drives, "C:Version", "");
}

}
