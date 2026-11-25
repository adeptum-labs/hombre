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
#include "bochs_vga.hpp"
#include "clock.hpp"
#include "ahci.hpp"
#include "disk.hpp"
#include "hard_disks.hpp"
#include "hda.hpp"
#include "nvme.hpp"
#include "usb_keyboard.hpp"

#include "../common/amiga_fs.hpp"
#include "../common/disk_browser.hpp"
#include "../common/dms.hpp"
#include "../common/libs/dos.hpp"
#include "../common/libs/exec.hpp"
#include "../common/monitor_ui.hpp"
#include "../common/printable.hpp"
#include "../common/serial.hpp"
#include "m68k_run.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"
#include "pci.hpp"
#include "splash.hpp"
#include "virtio_blk.hpp"
#include "virtio_disk.hpp"

namespace hombre {

namespace {

// Constant initialised, so they sit in .bss without a constructor run:
// the floppy drives DF0: to DF3:.
constinit VirtioBlk drive_devices[kFloppyDrives];

// The hard disks, the virtio ones indexed by their slot and the SATA and
// NVMe ones after them, and the volumes they carry, which stay mounted for
// dos as the floppy volumes do.
constinit VirtioBlk hard_disk_devices[kMaxHardDisks];
constinit Disk hard_disks[kMaxHardDisks]{};
constinit HardVolume hard_volumes[kMaxHardVolumes]{};
constinit u32 hard_volume_count = 0;
constinit DiskUnits disk_units{};
// The allocator the monitor's inserts take their image pages from.
PhysAllocator* disk_allocator = nullptr;

// The virtio-blk device in PCI slot kFirstDriveSlot + n is drive n; the one
// in slot kFirstHardDiskSlot + n is hard disk n. The rest of the hard disk
// range stays clear of the floppy fallback, although no disk is opened
// above kFirstHardDiskSlot + kMaxHardDisks - 1.
constexpr u8 kFirstDriveSlot = 0x10;
constexpr u8 kFirstHardDiskSlot = 0x18;
constexpr u8 kLastHardDiskSlot = 0x1F;

constexpr u64 page_size = 4096;
constexpr u64 max_drive_bytes = 8ull << 20;
constexpr u32 max_dms_image_bytes = 2u << 20;
constexpr u32 max_insert_bytes = 4u << 20;
// An insert reads this much per poll, so the machine stays responsive.
constexpr u32 insert_chunk_bytes = 64u << 10;
constexpr u32 startup_read_chunk = 512;
constexpr u32 startup_print_cap = 4096;

// A disk image held in memory that the direct map reaches. phys and pages
// are the allocator's, so the pages can be given back once dos no longer
// reads the image.
struct Image {
    const u8* data = nullptr;
    usize bytes = 0;
    u64 phys = 0;
    u64 pages = 0;
};

constexpr u64 pages_for(u64 bytes) {
    return (bytes + page_size - 1) / page_size;
}

void free_image(PhysAllocator& phys_pages, const Image& image) {
    if (image.phys != 0) {
        phys_pages.free_pages(image.phys, image.pages);
    }
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
    drive = Image{phys_pages.to_ptr(buffer), static_cast<usize>(bytes), buffer, page_count};
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
// returns false on failure, giving back any pages it took. The archive's
// pages stay the caller's to free.
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
        if (out_phys != 0) {
            phys_pages.free_pages(out_phys, out_pages);
        }
        if (work_phys != 0) {
            phys_pages.free_pages(work_phys, work_pages);
        }
        return false;
    }
    u8* out = phys_pages.to_ptr(out_phys);
    // The pages come back zeroed, as the unpacker needs.
    DmsWork& work = *reinterpret_cast<DmsWork*>(phys_pages.to_ptr(work_phys));
    const DmsError error =
        dms_unpack(drive.data, drive.bytes, out, info.image_bytes, work, info);
    // The work pages are always done with; the output goes back on failure.
    phys_pages.free_pages(work_phys, work_pages);
    if (error != DmsError::Ok) {
        write_dms_error(error, info, "");
        phys_pages.free_pages(out_phys, out_pages);
        return false;
    }
    serial::write("disk: DMS ");
    serial::write(dms_mode_name(info.mode));
    serial::write(", ");
    serial::write_dec(info.tracks_unpacked);
    serial::write(" tracks\n");
    image = Image{out, info.image_bytes, out_phys, out_pages};
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
    case DiskFormat::Dms: {
        const bool ok = unpack_dms(phys_pages, drive, prefix, image);
        free_image(phys_pages, drive);
        return ok;
    }
    case DiskFormat::Adf:
        serial::write(prefix);
        serial::write("disk: ADF\n");
        image = drive;
        return true;
    case DiskFormat::Unknown:
        serial::write(prefix);
        serial::write("disk: unknown format\n");
        free_image(phys_pages, drive);
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

// True for the device slots that hold hard disks, which the floppy
// discovery leaves alone.
bool in_hard_disk_slot(PciAddress address) {
    return address.bus == 0 && address.device >= kFirstHardDiskSlot &&
           address.device <= kLastHardDiskSlot;
}

bool same_address(PciAddress a, PciAddress b) {
    return a.bus == b.bus && a.device == b.device && a.function == b.function;
}

// The volumes stay mounted for as long as dos holds their disk, and the
// image pages behind them are given back once a replacement has dropped
// every lock and open file of the old disk.
constinit AdfVolume drive_volumes[kFloppyDrives]{};

struct DriveImage {
    u64 phys = 0;
    u64 pages = 0;
};

constinit DriveImage drive_images[kFloppyDrives]{};

void remember_image(u32 n, const Image& image) {
    drive_images[n] = DriveImage{image.phys, image.pages};
}

void forget_image(PhysAllocator& phys_pages, u32 n) {
    if (drive_images[n].phys != 0) {
        phys_pages.free_pages(drive_images[n].phys, drive_images[n].pages);
    }
    drive_images[n] = DriveImage{};
}

// --- floppy media ------------------------------------------------------------
//
// The host can put another disk in a floppy drive under a running system. A
// drive's medium is told by a few of its sectors: the start, where a DMS
// archive has its header and an ADF its boot block, and the root block of
// an ADF, where its name and dates are.

struct FloppyWatch {
    bool active;
    u64 signature;
};

constinit FloppyWatch floppy_watches[kFloppyDrives]{};
PhysAllocator* watch_pages = nullptr;
u64 watch_scratch = 0;  // a page the signature reads go to
u64 watch_next_us = 0;
constexpr u64 media_poll_us = 1000000;
constexpr u64 media_signature_sectors[] = {0, 1, 2, 880, 881};

// A hash of the drive's signature sectors and its size. False when a read
// fails.
bool floppy_signature(VirtioBlk& device, u64& out) {
    u64 hash = 0xCBF29CE484222325ull ^ device.sector_count();
    for (const u64 sector : media_signature_sectors) {
        if (sector >= device.sector_count()) {
            continue;
        }
        if (!device.read(sector, 1, watch_scratch)) {
            return false;
        }
        const u8* data = watch_pages->to_ptr(watch_scratch);
        for (u32 i = 0; i < kVirtioBlkSectorSize; ++i) {
            hash = (hash ^ data[i]) * 0x100000001B3ull;
        }
    }
    out = hash;
    return true;
}

// Starts watching the drives whose device came up.
void floppy_watch_start(PhysAllocator& phys_pages, const bool* present) {
    watch_pages = &phys_pages;
    watch_scratch = phys_pages.alloc_pages(1);
    if (watch_scratch == 0) {
        return;
    }
    for (u32 n = 0; n < kFloppyDrives; ++n) {
        FloppyWatch& watch = floppy_watches[n];
        watch = FloppyWatch{};
        if (present[n] && drive_devices[n].sector_count() != 0 &&
            floppy_signature(drive_devices[n], watch.signature)) {
            watch.active = true;
        }
    }
}

// Reads the drive's new disk and gives it to dos, or takes the disk out when
// the medium holds none dos can use.
void floppy_reload(Dos& dos, u32 n) {
    char prefix[] = "drive 0: ";
    prefix[6] = static_cast<char>('0' + n);
    Image drive;
    Image image;
    bool loaded = read_drive(*watch_pages, drive_devices[n], prefix, drive);
    if (loaded) {
        switch (detect_disk_format(drive.data, drive.bytes)) {
        case DiskFormat::Dms:
            loaded = unpack_dms(*watch_pages, drive, prefix, image);
            free_image(*watch_pages, drive);
            break;
        case DiskFormat::Adf:
            serial::write(prefix);
            serial::write("disk: ADF\n");
            image = drive;
            break;
        case DiskFormat::Unknown:
            serial::write(prefix);
            serial::write("disk: unknown format\n");
            free_image(*watch_pages, drive);
            loaded = false;
            break;
        }
    }
    if (loaded && mount_disk(image, prefix, drive_volumes[n])) {
        if (dos.replace_disk(n, &drive_volumes[n])) {
            forget_image(*watch_pages, n);
            remember_image(n, image);
        } else {
            free_image(*watch_pages, image);
        }
    } else {
        if (loaded) {
            free_image(*watch_pages, image);
        }
        if (dos.replace_disk(n, nullptr)) {
            forget_image(*watch_pages, n);
        }
    }
}

}

void boot_disk_run(const BootInfo& info, PhysAllocator& phys_pages, MmioMapper& mmio) {
    disk_allocator = &phys_pages;
    pci_scan(&log_pci_function, nullptr);
    splash_progress(3, kSplashKernelSteps);

    // Each floppy by its slot; a single device elsewhere is DF0: as before
    // drives had slots. The hard disks are found from the same scan.
    PciAddress found[kFloppyDrives + kMaxHardDisks];
    const u32 count = virtio_blk_find_all(found, kFloppyDrives + kMaxHardDisks);
    PciAddress addresses[kFloppyDrives]{};
    bool present[kFloppyDrives]{};
    for (u32 i = 0; i < count; ++i) {
        const u32 drive = drive_of(found[i]);
        if (drive < kFloppyDrives && !present[drive]) {
            addresses[drive] = found[i];
            present[drive] = true;
        }
    }
    if (!present[0] && count == 1 && drive_of(found[0]) == kFloppyDrives &&
        !in_hard_disk_slot(found[0])) {
        addresses[0] = found[0];
        present[0] = true;
    }
    for (u32 i = 0; i < count; ++i) {
        bool used = false;
        for (u32 n = 0; n < kFloppyDrives; ++n) {
            used = used || (present[n] && same_address(addresses[n], found[i]));
        }
        if (!used && !in_hard_disk_slot(found[i])) {
            serial::write("virtio-blk: device in no drive slot ignored\n");
        }
    }

    // Hard disks by their slot. Their lines are logged before the boot disk
    // is looked for, so a machine with only hard disks still says what it
    // found.
    bool disk_present[kMaxHardDisks]{};
    u32 hard_disk_slots = 0;
    for (u32 i = 0; i < count; ++i) {
        if (!in_hard_disk_slot(found[i]) || found[i].function != 0) {
            continue;
        }
        const u32 disk = found[i].device - kFirstHardDiskSlot;
        if (disk >= kMaxHardDisks || disk_present[disk]) {
            continue;
        }
        disk_present[disk] = true;
        if (hard_disk_slots <= disk) {
            hard_disk_slots = disk + 1;
        }
        char prefix[] = "hd0: ";
        prefix[2] = static_cast<char>('0' + disk);
        if (!virtio_disk_open(hard_disk_devices[disk], found[i], mmio, phys_pages,
                              hard_disks[disk])) {
            serial::write(prefix);
            serial::write("virtio-blk: init failed\n");
        }
    }
    // SATA and NVMe disks take the slots after the virtio ones.
    if (hard_disk_slots < kMaxHardDisks) {
        hard_disk_slots += ahci_open_disks(mmio, phys_pages, hard_disks + hard_disk_slots,
                                           kMaxHardDisks - hard_disk_slots);
    }
    if (hard_disk_slots < kMaxHardDisks) {
        nvme_open_disks(mmio, phys_pages, hard_disks + hard_disk_slots,
                        kMaxHardDisks - hard_disk_slots);
    }
    const u32 hard_count =
        hard_disks_mount(hard_disks, kMaxHardDisks, phys_pages, hard_volumes, kMaxHardVolumes,
                         disk_units);
    hard_volume_count = hard_count;
    bool hard_boot = false;
    for (u32 i = 0; i < hard_count; ++i) {
        hard_boot = hard_boot || hard_volumes[i].bootable;
    }

    // DF0: or a bootable hard disk partition holds the boot volume; without
    // either nothing starts. dos picks between them by boot priority.
    if (!present[0] && !hard_boot) {
        serial::write("no boot disk\n");
        return;
    }
    Image boot;
    const bool boot_loaded =
        present[0] && load_disk(phys_pages, mmio, drive_devices[0], addresses[0], "", boot);
    if (!boot_loaded && !hard_boot) {
        return;
    }
    splash_progress(4, kSplashKernelSteps);
    usb_keyboard_start(mmio, phys_pages);
    hda_init(mmio, phys_pages);
    splash_progress(5, kSplashKernelSteps);
    const bool df0 = boot_loaded && mount_disk(boot, "", drive_volumes[0]);
    if (boot_loaded && !df0) {
        free_image(phys_pages, boot);
    }
    if (df0) {
        remember_image(0, boot);
    }
    if (!df0 && !hard_boot) {
        return;
    }
    splash_progress(6, kSplashKernelSteps);
    if (df0) {
        print_root(drive_volumes[0]);
        print_startup_sequence(drive_volumes[0]);
    }

    // DF1: to DF3: are empty unless their disk reads and mounts.
    Drives drives;
    drives.units = &disk_units;
    if (df0) {
        drives.volume[0] = &drive_volumes[0];
    }
    for (u32 n = 1; n < kFloppyDrives; ++n) {
        char prefix[] = "drive 0: ";
        prefix[6] = static_cast<char>('0' + n);
        if (!present[n]) {
            serial::write(prefix);
            serial::write("no disk\n");
            continue;
        }
        Image image;
        if (load_disk(phys_pages, mmio, drive_devices[n], addresses[n], prefix, image)) {
            if (mount_disk(image, prefix, drive_volumes[n])) {
                drives.volume[n] = &drive_volumes[n];
                remember_image(n, image);
            } else {
                free_image(phys_pages, image);
            }
        }
    }
    // The hard disk partitions follow the floppy drives.
    for (u32 i = 0; i < hard_count && i < kHardVolumes; ++i) {
        const HardVolume& hard = hard_volumes[i];
        DriveInfo& drive = drives.info[kFloppyDrives + i];
        drives.volume[kFloppyDrives + i] = &hard.volume;
        __builtin_memcpy(drive.device, hard.device, sizeof drive.device);
        drive.boot_pri = hard.boot_pri;
        drive.bootable = hard.bootable;
        __builtin_memcpy(drive.env, hard.env, sizeof drive.env);
        drive.has_env = true;
        drive.unit = hard.unit;
    }
    floppy_watch_start(phys_pages, present);
    bochs_vga_init(mmio);
    m68k_run_command(info, phys_pages, drives, "C:Version", "");
}

void boot_disk_poll_media(Dos& dos) {
    if (watch_pages == nullptr || watch_scratch == 0) {
        return;
    }
    const u64 now = clock_us();
    if (now == 0 || now < watch_next_us) {
        return;
    }
    watch_next_us = now + media_poll_us;
    for (u32 n = 0; n < kFloppyDrives; ++n) {
        FloppyWatch& watch = floppy_watches[n];
        u64 signature = 0;
        if (!watch.active || !drive_devices[n].refresh_capacity() ||
            !floppy_signature(drive_devices[n], signature) || signature == watch.signature) {
            continue;
        }
        watch.signature = signature;
        floppy_reload(dos, n);
    }
}

namespace {

// The request the interrupt remembers and the results it copies: the
// generation counts are odd while their buffer is being written, so a
// reader that sees an odd or changed count keeps what it had.
inline void memory_barrier() {
    asm volatile("" ::: "memory");
}

constinit u32 request_seq = 0;
constinit MonitorRequest pending_request{};
constinit u32 handled_seq = 0;
constinit u32 publish_seq = 0;
constinit MonitorListing published_listing{};
constinit char published_status[sizeof(MonitorSnapshot::status)]{};
constinit MonitorListing shown_listing{};
constinit char shown_status[sizeof(MonitorSnapshot::status)]{};
constinit bool waiting_published = false;

// One file being read into the drive's next image, a chunk per poll.
struct InsertJob {
    bool active = false;
    u32 drive = 0;
    const AdfVolume* source = nullptr;
    DirEntry file{};
    u32 size = 0;
    u32 offset = 0;
    u64 phys = 0;
    u64 pages = 0;
};

constinit InsertJob insert{};

void copy_text(char* out, u32 capacity, const char* text) {
    u32 i = 0;
    for (; text[i] != '\0' && i + 1 < capacity; ++i) {
        out[i] = text[i];
    }
    out[i] = '\0';
}

void append_text(char* out, u32 capacity, u32& length, const char* text) {
    for (u32 i = 0; text[i] != '\0' && length + 1 < capacity; ++i) {
        out[length++] = text[i];
    }
    out[length] = '\0';
}

void append_dec(char* out, u32 capacity, u32& length, u32 value) {
    char digits[12];
    if (format_u64(digits, sizeof digits, value, 10) != 0) {
        append_text(out, capacity, length, digits);
    }
}

// Publishes the status and whether the listing is still loading; the
// entries and the path stay as they were.
void publish_status(const char* text, bool loading) {
    ++publish_seq;
    memory_barrier();
    published_listing.loading = loading;
    copy_text(published_status, sizeof published_status, text);
    memory_barrier();
    ++publish_seq;
}

void publish_error(const char* what, FsError error) {
    char text[sizeof published_status];
    u32 n = 0;
    append_text(text, sizeof text, n, what);
    append_text(text, sizeof text, n, ": ");
    append_text(text, sizeof text, n, fs_error_name(error));
    publish_status(text, false);
}

void abandon_insert() {
    free_image(*disk_allocator, Image{nullptr, 0, insert.phys, insert.pages});
    insert = InsertJob{};
}

void fail_insert(const char* why) {
    abandon_insert();
    publish_status(why, false);
}

bool take_request(MonitorRequest& out) {
    for (;;) {
        const u32 first = request_seq;
        memory_barrier();
        if ((first & 1) != 0) {
            continue;
        }
        if (first == handled_seq) {
            return false;
        }
        out = pending_request;
        memory_barrier();
        if (request_seq != first) {
            continue;
        }
        handled_seq = first;
        return true;
    }
}

bool same_device(const char* a, const char* b) {
    while (*a != '\0' &&
           disk_browser_detail::fold_ascii(*a) == disk_browser_detail::fold_ascii(*b)) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

const AdfVolume* partition_volume(const char* name) {
    for (u32 i = 0; i < hard_volume_count && i < kMaxHardVolumes; ++i) {
        if (same_device(hard_volumes[i].device, name)) {
            return &hard_volumes[i].volume;
        }
    }
    return nullptr;
}

void publish_listing_error(const char* path, const char* error) {
    ++publish_seq;
    memory_barrier();
    published_listing = MonitorListing{};
    copy_text(published_listing.path, sizeof published_listing.path, path);
    copy_text(published_listing.error, sizeof published_listing.error, error);
    memory_barrier();
    ++publish_seq;
}

void list_directory(const MonitorRequest& request) {
    if (request.path[0] == '\0') {
        const char* devices[kMaxHardVolumes];
        u32 count = 0;
        for (u32 i = 0; i < hard_volume_count && i < kMaxHardVolumes; ++i) {
            devices[count++] = hard_volumes[i].device;
        }
        ++publish_seq;
        memory_barrier();
        disk_browser_list_partitions(devices, count, published_listing);
        memory_barrier();
        ++publish_seq;
        return;
    }
    char partition[kDiskPartitionBytes];
    char rest[kDiskPathBytes];
    const AdfVolume* volume = nullptr;
    if (disk_browser_split(request.path, partition, rest)) {
        volume = partition_volume(partition);
    }
    if (volume == nullptr) {
        publish_listing_error(request.path, "no such partition");
        return;
    }
    ++publish_seq;
    memory_barrier();
    disk_browser_list_directory(*volume, rest, published_listing);
    copy_text(published_listing.path, sizeof published_listing.path, request.path);
    memory_barrier();
    ++publish_seq;
}

void start_insert(const MonitorRequest& request) {
    if (request.drive >= kFloppyDrives) {
        publish_status("insert: no such drive", false);
        return;
    }
    char partition[kDiskPartitionBytes];
    char rest[kDiskPathBytes];
    const AdfVolume* volume = nullptr;
    if (disk_browser_split(request.path, partition, rest)) {
        volume = partition_volume(partition);
    }
    if (volume == nullptr) {
        publish_status("insert: no such partition", false);
        return;
    }
    DirEntry file;
    const FsError error = adf_find(*volume, rest, file);
    if (error != FsError::Ok) {
        publish_error("insert", error);
        return;
    }
    if (file.is_dir) {
        publish_status("insert: not a file", false);
        return;
    }
    if (file.size > max_insert_bytes) {
        publish_status("insert: file larger than 4 MiB", false);
        return;
    }
    if (file.size == 0) {
        publish_status("insert: empty file", false);
        return;
    }
    const u64 pages = pages_for(file.size);
    const u64 phys = disk_allocator->alloc_pages(pages);
    if (phys == 0) {
        publish_status("insert: no memory", false);
        return;
    }
    insert = InsertJob{};
    insert.active = true;
    insert.drive = request.drive;
    insert.source = volume;
    insert.file = file;
    insert.size = file.size;
    insert.phys = phys;
    insert.pages = pages;
    publish_status("insert: reading", true);
}

void finish_insert(Dos& dos) {
    const u32 drive = insert.drive;
    Image image{disk_allocator->to_ptr(insert.phys), insert.size, insert.phys, insert.pages};
    insert = InsertJob{};
    switch (detect_disk_format(image.data, image.bytes)) {
    case DiskFormat::Dms: {
        Image unpacked;
        const bool ok = unpack_dms(*disk_allocator, image, "monitor: ", unpacked);
        free_image(*disk_allocator, image);
        if (!ok) {
            publish_status("insert: DMS unpack failed", false);
            return;
        }
        image = unpacked;
        break;
    }
    case DiskFormat::Adf:
        break;
    case DiskFormat::Unknown:
        free_image(*disk_allocator, image);
        publish_status("insert: unknown format", false);
        return;
    }
    AdfVolume volume{};
    const FsError error = adf_mount(image.data, image.bytes, volume);
    if (error != FsError::Ok) {
        free_image(*disk_allocator, image);
        publish_error("insert: mount", error);
        return;
    }
    if (!dos.replace_disk(drive, &volume)) {
        free_image(*disk_allocator, image);
        publish_status("insert: replace failed", false);
        return;
    }
    forget_image(*disk_allocator, drive);
    remember_image(drive, image);
    char text[sizeof published_status];
    u32 n = 0;
    append_text(text, sizeof text, n, "inserted DF");
    append_dec(text, sizeof text, n, drive);
    append_text(text, sizeof text, n, ": ");
    append_text(text, sizeof text, n, volume.name);
    publish_status(text, false);
}

void run_insert_step(Dos& dos) {
    const u32 left = insert.size - insert.offset;
    const u32 want = left < insert_chunk_bytes ? left : insert_chunk_bytes;
    u8* out = disk_allocator->to_ptr(insert.phys) + insert.offset;
    u32 got = 0;
    const FsError error = adf_read(*insert.source, insert.file, insert.offset, out, want, got);
    if (error != FsError::Ok) {
        publish_error("insert: read", error);
        abandon_insert();
        return;
    }
    if (got != want) {
        fail_insert("insert: short read");
        return;
    }
    insert.offset += got;
    if (insert.offset == insert.size) {
        finish_insert(dos);
    }
}

void eject(Dos& dos, const MonitorRequest& request) {
    if (request.drive >= kFloppyDrives) {
        publish_status("eject: no such drive", false);
        return;
    }
    if (!dos.replace_disk(request.drive, nullptr)) {
        publish_status("eject: replace failed", false);
        return;
    }
    forget_image(*disk_allocator, request.drive);
    char text[sizeof published_status];
    u32 n = 0;
    append_text(text, sizeof text, n, "ejected DF");
    append_dec(text, sizeof text, n, request.drive);
    publish_status(text, false);
}

void run_request(Dos& dos, const MonitorRequest& request) {
    switch (request.op) {
    case MonitorOp::ListDirectory:
        list_directory(request);
        break;
    case MonitorOp::Insert:
        start_insert(request);
        break;
    case MonitorOp::Eject:
        eject(dos, request);
        break;
    default:
        break;
    }
}

void note_waiting() {
    if (waiting_published || (request_seq == handled_seq && !insert.active)) {
        return;
    }
    waiting_published = true;
    publish_status("disk request waiting: Forbid held", true);
}

}

void boot_disk_request(const MonitorRequest& request) {
    ++request_seq;
    memory_barrier();
    pending_request = request;
    memory_barrier();
    ++request_seq;
}

void boot_disk_poll(Dos& dos, Exec& exec) {
    if (disk_allocator == nullptr) {
        return;
    }
    if (exec.forbidden()) {
        note_waiting();
        return;
    }
    waiting_published = false;
    MonitorRequest request;
    if (take_request(request)) {
        // Browsing goes on beside an insert; another change replaces it.
        if (request.op != MonitorOp::ListDirectory) {
            abandon_insert();
        }
        run_request(dos, request);
        return;
    }
    if (insert.active) {
        run_insert_step(dos);
    }
}

void boot_disk_snapshot(MonitorSnapshot& out) {
    const u32 first = publish_seq;
    memory_barrier();
    if ((first & 1) == 0) {
        out.listing = published_listing;
        copy_text(out.status, sizeof out.status, published_status);
        memory_barrier();
        if (publish_seq == first) {
            shown_listing = out.listing;
            copy_text(shown_status, sizeof shown_status, out.status);
        } else {
            out.listing = shown_listing;
            copy_text(out.status, sizeof out.status, shown_status);
        }
    } else {
        out.listing = shown_listing;
        copy_text(out.status, sizeof out.status, shown_status);
    }
    if (request_seq != handled_seq) {
        out.listing.loading = true;
    }
}

}
