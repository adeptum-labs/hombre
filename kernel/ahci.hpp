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

#pragma once

#include "disk.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"

// A polled AHCI (SATA) driver. This header holds the command layouts and the
// parts that compute command data without touching hardware; the parts that
// talk to the controller are in ahci.cpp and are kernel only.

namespace hombre {

// Class 0x01 (storage), subclass 0x06 (SATA) and programming interface 0x01
// (AHCI), above the revision byte.
inline constexpr u32 kAhciClassCode = 0x010601;

inline constexpr u32 kAhciSectorBytes = 512;
inline constexpr u32 kAhciMaxSectorBytes = 4096;

// ATA commands.
inline constexpr u8 kAhciCmdIdentify = 0xEC;
inline constexpr u8 kAhciCmdReadDmaExt = 0x25;
inline constexpr u8 kAhciCmdWriteDmaExt = 0x35;
inline constexpr u8 kAhciCmdFlushCacheExt = 0xEA;

// Register Host to Device FIS.
inline constexpr u8 kAhciFisTypeH2d = 0x27;
inline constexpr u8 kAhciFisC = 1u << 7;
inline constexpr u8 kAhciFisDeviceLba = 1u << 6;
inline constexpr u32 kAhciFisBytes = 20;
inline constexpr u32 kAhciFisDwords = kAhciFisBytes / 4;

// The command list, the FIS receive area and the command table share one
// page per port: the command list is 1 KiB and must be 1 KiB aligned, the
// FIS receive area is 256 bytes and must be 256-byte aligned, and the
// command table must be 128-byte aligned.
inline constexpr u32 kAhciPageBytes = 4096;
inline constexpr u32 kAhciCmdListOffset = 0;
inline constexpr u32 kAhciCmdListBytes = 1024;
inline constexpr u32 kAhciFisOffset = 1024;
inline constexpr u32 kAhciFisReceiveBytes = 256;
inline constexpr u32 kAhciCmdTableOffset = 1280;
inline constexpr u32 kAhciPrdtOffset = 128;
inline constexpr u32 kAhciPrdtBytes = 16;
inline constexpr u32 kAhciCmdTableBytes = kAhciPrdtOffset + kAhciPrdtBytes;

static_assert(kAhciMaxSectorBytes <= kAhciPageBytes);
static_assert(kAhciCmdListOffset + kAhciCmdListBytes <= kAhciFisOffset);
static_assert(kAhciFisOffset + kAhciFisReceiveBytes <= kAhciCmdTableOffset);
static_assert(kAhciCmdTableOffset + kAhciCmdTableBytes <= kAhciPageBytes);
static_assert((kAhciFisOffset & 0xFFu) == 0);
static_assert((kAhciCmdTableOffset & 0x7Fu) == 0);

// One command list entry. The first dword holds the command FIS length in
// dwords, the direction bit and the descriptor count; the command table
// address follows.
struct AhciCmdHeader {
    u16 flags;
    u16 prdt_count;
    u32 prd_bytes;
    u32 table_low;
    u32 table_high;
    u32 reserved[4];
};

static_assert(sizeof(AhciCmdHeader) == 32);
static_assert(__builtin_offsetof(AhciCmdHeader, prdt_count) == 2);
static_assert(__builtin_offsetof(AhciCmdHeader, prd_bytes) == 4);
static_assert(__builtin_offsetof(AhciCmdHeader, table_low) == 8);
static_assert(__builtin_offsetof(AhciCmdHeader, table_high) == 12);

// One physical region descriptor: where the data is and how many bytes less
// one it holds.
struct AhciPrdtEntry {
    u32 base_low;
    u32 base_high;
    u32 reserved;
    u32 byte_count;
};

static_assert(sizeof(AhciPrdtEntry) == 16);
static_assert(__builtin_offsetof(AhciPrdtEntry, byte_count) == 12);

inline constexpr u16 kAhciCmdHeaderWrite = 1u << 6;
inline constexpr u32 kAhciPrdtCountMask = (1u << 22) - 1;

// Fills the 20-byte Register H2D FIS with `command`, LBA `lba` and `count`
// sectors, and returns its length in dwords.
constexpr u32 ahci_build_h2d_fis(u8* fis, u8 command, u64 lba, u16 count) {
    for (u32 i = 0; i < kAhciFisBytes; ++i) {
        fis[i] = 0;
    }
    fis[0] = kAhciFisTypeH2d;
    fis[1] = kAhciFisC;
    fis[2] = command;
    fis[4] = static_cast<u8>(lba);
    fis[5] = static_cast<u8>(lba >> 8);
    fis[6] = static_cast<u8>(lba >> 16);
    fis[7] = kAhciFisDeviceLba;
    fis[8] = static_cast<u8>(lba >> 24);
    fis[9] = static_cast<u8>(lba >> 32);
    fis[10] = static_cast<u8>(lba >> 40);
    fis[12] = static_cast<u8>(count);
    fis[13] = static_cast<u8>(count >> 8);
    return kAhciFisDwords;
}

// The command header of a command whose table is at `table_phys`, with
// `prdt_count` descriptors; `write` selects a host-to-device transfer.
constexpr AhciCmdHeader ahci_build_cmd_header(u64 table_phys, bool write, u32 prdt_count) {
    AhciCmdHeader header{};
    header.flags = static_cast<u16>(kAhciFisDwords | (write ? kAhciCmdHeaderWrite : 0));
    header.prdt_count = static_cast<u16>(prdt_count);
    header.table_low = static_cast<u32>(table_phys);
    header.table_high = static_cast<u32>(table_phys >> 32);
    return header;
}

// Fills the descriptor at `prdt` for `bytes` at `data_phys` and returns the
// descriptor count, 0 for an empty transfer.
constexpr u32 ahci_build_prdt(AhciPrdtEntry* prdt, u64 data_phys, u32 bytes) {
    if (bytes == 0) {
        return 0;
    }
    prdt[0] = AhciPrdtEntry{static_cast<u32>(data_phys), static_cast<u32>(data_phys >> 32), 0,
                            (bytes - 1) & kAhciPrdtCountMask};
    return 1;
}

// The kinds of device a port signature names.
enum class AhciSignature : u8 {
    Ata,
    Atapi,
    PortMultiplier,
    Enclosure,
    Other,
};

constexpr AhciSignature ahci_classify_signature(u32 signature) {
    switch (signature) {
    case 0x00000101u: return AhciSignature::Ata;
    case 0xEB140101u: return AhciSignature::Atapi;
    case 0x96690101u: return AhciSignature::PortMultiplier;
    case 0xC33C0101u: return AhciSignature::Enclosure;
    default: return AhciSignature::Other;
    }
}

// The parts of IDENTIFY DEVICE data that the driver uses.
struct AhciIdentify {
    u64 sectors;       // logical sectors
    u32 sector_bytes;  // logical sector size as the drive reports it
    bool flush;        // FLUSH CACHE EXT is supported
    char model[41];    // words 27 to 46, with the padding trimmed
};

// True for a logical sector size the driver can move through one page.
constexpr bool ahci_sector_size_ok(u32 bytes) {
    return bytes >= kAhciSectorBytes && bytes <= kAhciMaxSectorBytes &&
           (bytes % kAhciSectorBytes) == 0;
}

// Fills `info` from the 256 words of IDENTIFY DEVICE data. Returns false
// when the drive reports no sectors or a logical sector size this driver
// cannot serve; every field is filled either way.
constexpr bool ahci_parse_identify(const u16* words, AhciIdentify& info) {
    const bool lba48 = (words[83] & (1u << 10)) != 0;
    info.sectors =
        lba48 ? (static_cast<u64>(words[100]) | (static_cast<u64>(words[101]) << 16) |
                 (static_cast<u64>(words[102]) << 32) | (static_cast<u64>(words[103]) << 48))
              : (static_cast<u64>(words[60]) | (static_cast<u64>(words[61]) << 16));
    const u16 sector_words = words[106];
    if ((sector_words & 0x4000u) != 0 && (sector_words & 0x8000u) == 0 &&
        (sector_words & 0x1000u) != 0) {
        const u64 words_per_sector =
            static_cast<u64>(words[117]) | (static_cast<u64>(words[118]) << 16);
        const u64 bytes = words_per_sector * 2;
        info.sector_bytes = bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(bytes);
    } else {
        info.sector_bytes = kAhciSectorBytes;
    }
    info.flush = (words[83] & (1u << 13)) != 0;
    for (u32 i = 0; i < 20; ++i) {
        info.model[i * 2] = static_cast<char>(words[27 + i] >> 8);
        info.model[i * 2 + 1] = static_cast<char>(words[27 + i] & 0xFF);
    }
    info.model[40] = '\0';
    for (u32 end = 40; end > 0 && (info.model[end - 1] == ' ' || info.model[end - 1] == '\0');
         --end) {
        info.model[end - 1] = '\0';
    }
    return info.sectors != 0 && ahci_sector_size_ok(info.sector_bytes);
}

// Where a 512-byte disk sector sits in the logical sector that holds it.
struct AhciSectorPlace {
    u64 logical;
    u32 offset;
};

constexpr AhciSectorPlace ahci_sector_place(u64 sector, u32 sector_bytes) {
    const u32 per_logical = sector_bytes / kAhciSectorBytes;
    return AhciSectorPlace{sector / per_logical,
                           static_cast<u32>((sector % per_logical) * kAhciSectorBytes)};
}

// The disk's size in 512-byte sectors from the drive's logical sector count.
constexpr u64 ahci_disk_sectors(u64 logical_sectors, u32 sector_bytes) {
    return logical_sectors * (sector_bytes / kAhciSectorBytes);
}

// Finds every AHCI controller and opens each ATA disk on it as a Disk,
// storing at most `max` in `out` and returning how many were stored. The
// per-disk DMA pages come from `pages` and stay allocated for as long as the
// disks are used.
u32 ahci_open_disks(MmioMapper& mmio, PhysAllocator& pages, Disk* out, u32 max);

}
