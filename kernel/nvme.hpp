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

// A polled NVMe driver. This header holds the command and identify layouts
// and the parts that compute data without touching hardware; the parts that
// talk to the controller are in nvme.cpp and are kernel only.

namespace hombre {

// Class 0x01 (storage), subclass 0x08 (non-volatile memory) and programming
// interface 0x02 (NVM Express), above the revision byte.
inline constexpr u32 kNvmeClassCode = 0x010802;

inline constexpr u32 kNvmeSectorBytes = 512;
inline constexpr u32 kNvmeMaxSectorBytes = 4096;

// Commands.
inline constexpr u8 kNvmeCmdFlush = 0x00;
inline constexpr u8 kNvmeCmdCreateSq = 0x01;
inline constexpr u8 kNvmeCmdWrite = 0x01;
inline constexpr u8 kNvmeCmdRead = 0x02;
inline constexpr u8 kNvmeCmdCreateCq = 0x05;
inline constexpr u8 kNvmeCmdIdentify = 0x06;

// Identify CNS values.
inline constexpr u32 kNvmeCnsNamespace = 0;
inline constexpr u32 kNvmeCnsController = 1;
inline constexpr u32 kNvmeCnsActiveNamespaces = 2;

inline constexpr u32 kNvmePageBytes = 4096;
inline constexpr u32 kNvmeAdminDepth = 16;
inline constexpr u32 kNvmeIoDepthLimit = 16;
inline constexpr u32 kNvmeCommandBytes = 64;
inline constexpr u32 kNvmeCompletionBytes = 16;
inline constexpr u32 kNvmeQueuePhysicallyContiguous = 1u << 0;

inline constexpr u32 kNvmeDoorbellBase = 0x1000;
inline constexpr u32 kNvmeControllerModelOffset = 24;
inline constexpr u32 kNvmeControllerModelBytes = 40;
inline constexpr u32 kNvmeControllerVwcOffset = 525;
inline constexpr u32 kNvmeNamespaceFlbasOffset = 26;
inline constexpr u32 kNvmeNamespaceFormatMask = 0x0F;
inline constexpr u32 kNvmeNamespaceFormatOffset = 128;
inline constexpr u32 kNvmeLbaFormatBytes = 4;
inline constexpr u32 kNvmeNamespaceFormatCount = 16;

static_assert(kNvmeMaxSectorBytes <= kNvmePageBytes);
static_assert(kNvmeAdminDepth * kNvmeCommandBytes <= kNvmePageBytes);
static_assert(kNvmeAdminDepth * kNvmeCompletionBytes <= kNvmePageBytes);
static_assert(kNvmeIoDepthLimit * kNvmeCommandBytes <= kNvmePageBytes);
static_assert(kNvmeIoDepthLimit * kNvmeCompletionBytes <= kNvmePageBytes);
static_assert(kNvmeNamespaceFormatOffset + kNvmeNamespaceFormatCount * kNvmeLbaFormatBytes <=
              kNvmePageBytes);

// One submission queue entry.
struct NvmeCommand {
    u32 cdw0;  // opcode, fuse, data transfer and command id
    u32 nsid;
    u32 cdw2;
    u32 cdw3;
    u64 mptr;
    u64 prp1;
    u64 prp2;
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
};

static_assert(sizeof(NvmeCommand) == 64);
static_assert(__builtin_offsetof(NvmeCommand, cdw0) == 0);
static_assert(__builtin_offsetof(NvmeCommand, nsid) == 4);
static_assert(__builtin_offsetof(NvmeCommand, prp1) == 24);
static_assert(__builtin_offsetof(NvmeCommand, prp2) == 32);
static_assert(__builtin_offsetof(NvmeCommand, cdw10) == 40);
static_assert(__builtin_offsetof(NvmeCommand, cdw15) == 60);

// One completion queue entry. `status` holds the command id, the phase tag
// and the status field.
struct NvmeCompletion {
    u32 result;
    u32 reserved;
    u32 sq_head;
    u32 status;
};

static_assert(sizeof(NvmeCompletion) == 16);
static_assert(__builtin_offsetof(NvmeCompletion, result) == 0);
static_assert(__builtin_offsetof(NvmeCompletion, sq_head) == 8);
static_assert(__builtin_offsetof(NvmeCompletion, status) == 12);

inline constexpr u32 kNvmeCompletionPhaseShift = 16;
inline constexpr u32 kNvmeCompletionStatusShift = 17;
inline constexpr u32 kNvmeCompletionIdMask = 0xFFFF;

constexpr u32 nvme_completion_phase(u32 dw3) {
    return (dw3 >> kNvmeCompletionPhaseShift) & 1u;
}

// Status code, status code type, retry and more bits: zero means success.
constexpr u32 nvme_completion_status(u32 dw3) { return dw3 >> kNvmeCompletionStatusShift; }

constexpr u32 nvme_completion_id(u32 dw3) { return dw3 & kNvmeCompletionIdMask; }

// CAP fields.
inline constexpr u32 kNvmeCapMqesMask = 0xFFFF;
inline constexpr u32 kNvmeCapDstrdShift = 32;
inline constexpr u32 kNvmeCapTimeoutShift = 36;
inline constexpr u32 kNvmeCapTimeoutMask = 0xFF;
inline constexpr u32 kNvmeCapMpsminShift = 48;
inline constexpr u32 kNvmeCapMpsminMask = 0xF;
inline constexpr u32 kNvmeTimeoutUnitMs = 500;

constexpr u32 nvme_cap_mqes(u64 cap) { return static_cast<u32>(cap & kNvmeCapMqesMask); }

constexpr u32 nvme_cap_dstrd(u64 cap) {
    return static_cast<u32>((cap >> kNvmeCapDstrdShift) & 0xF);
}

// The controller's timeout in 500 ms units, zero when it names none.
constexpr u32 nvme_cap_timeout(u64 cap) {
    return static_cast<u32>((cap >> kNvmeCapTimeoutShift) & kNvmeCapTimeoutMask);
}

constexpr u32 nvme_cap_timeout_ms(u64 cap) {
    return nvme_cap_timeout(cap) * kNvmeTimeoutUnitMs;
}

constexpr u32 nvme_cap_mpsmin(u64 cap) {
    return static_cast<u32>((cap >> kNvmeCapMpsminShift) & kNvmeCapMpsminMask);
}

// The driver moves every buffer in 4 KiB pages, so a controller whose
// minimum page size is larger cannot be used.
constexpr bool nvme_cap_supports_4k_pages(u64 cap) { return nvme_cap_mpsmin(cap) == 0; }

// Offset of a doorbell: queue `qid`'s submission tail or completion head,
// with `4 << DSTRD` bytes between neighbouring doorbells.
constexpr u32 nvme_doorbell_offset(u32 dstrd, u32 qid, bool completion) {
    return kNvmeDoorbellBase + (2 * qid + (completion ? 1u : 0u)) * (4u << dstrd);
}

// The I/O queue depth: sixteen entries or the controller's maximum,
// whichever is smaller.
constexpr u32 nvme_io_depth(u32 mqes) {
    return mqes + 1 < kNvmeIoDepthLimit ? mqes + 1 : kNvmeIoDepthLimit;
}

constexpr u32 nvme_build_aqa(u32 sq_depth, u32 cq_depth) {
    return (sq_depth - 1) | ((cq_depth - 1) << 16);
}

constexpr u32 nvme_command_dword0(u8 opcode, u32 cid) {
    return static_cast<u32>(opcode) | (cid << 16);
}

constexpr NvmeCommand nvme_build_command(u8 opcode, u32 nsid, u32 cid) {
    NvmeCommand command{};
    command.cdw0 = nvme_command_dword0(opcode, cid);
    command.nsid = nsid;
    return command;
}

constexpr NvmeCommand nvme_build_identify(u32 cid, u32 cns, u32 nsid, u64 buffer_phys) {
    NvmeCommand command = nvme_build_command(kNvmeCmdIdentify, nsid, cid);
    command.prp1 = buffer_phys;
    command.cdw10 = cns;
    return command;
}

constexpr NvmeCommand nvme_build_create_cq(u32 cid, u32 qid, u32 depth, u64 queue_phys) {
    NvmeCommand command = nvme_build_command(kNvmeCmdCreateCq, 0, cid);
    command.prp1 = queue_phys;
    command.cdw10 = qid | ((depth - 1) << 16);
    command.cdw11 = kNvmeQueuePhysicallyContiguous;
    return command;
}

constexpr NvmeCommand nvme_build_create_sq(u32 cid, u32 qid, u32 cqid, u32 depth,
                                           u64 queue_phys) {
    NvmeCommand command = nvme_build_command(kNvmeCmdCreateSq, 0, cid);
    command.prp1 = queue_phys;
    command.cdw10 = qid | ((depth - 1) << 16);
    command.cdw11 = kNvmeQueuePhysicallyContiguous | (cqid << 16);
    return command;
}

// A read or write of `blocks` logical blocks, which is at least one,
// starting at `slba`, with the data in the single page at `buffer_phys`.
constexpr NvmeCommand nvme_build_rw(u8 opcode, u32 nsid, u32 cid, u64 slba, u32 blocks,
                                    u64 buffer_phys) {
    NvmeCommand command = nvme_build_command(opcode, nsid, cid);
    command.prp1 = buffer_phys;
    command.cdw10 = static_cast<u32>(slba);
    command.cdw11 = static_cast<u32>(slba >> 32);
    command.cdw12 = (blocks - 1) & 0xFFFFu;
    return command;
}

constexpr NvmeCommand nvme_build_flush(u32 nsid, u32 cid) {
    return nvme_build_command(kNvmeCmdFlush, nsid, cid);
}

constexpr u32 nvme_read_u32(const u8* data, u32 offset) {
    return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8) |
           (static_cast<u32>(data[offset + 2]) << 16) |
           (static_cast<u32>(data[offset + 3]) << 24);
}

constexpr u64 nvme_read_u64(const u8* data, u32 offset) {
    return static_cast<u64>(nvme_read_u32(data, offset)) |
           (static_cast<u64>(nvme_read_u32(data, offset + 4)) << 32);
}

// The controller model, bytes 24 to 63 of the Identify Controller data,
// trimmed of its trailing padding and terminated.
constexpr usize nvme_model_string(const u8* identify, char* out, usize cap) {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    usize count = 0;
    while (count + 1 < cap && count < kNvmeControllerModelBytes &&
           identify[kNvmeControllerModelOffset + count] != '\0') {
        out[count] = static_cast<char>(identify[kNvmeControllerModelOffset + count]);
        ++count;
    }
    while (count > 0 && out[count - 1] == ' ') {
        --count;
    }
    out[count] = '\0';
    return count;
}

// Byte 525 bit 0: the controller has a volatile write cache, which takes a
// flush command after writes.
constexpr bool nvme_write_cache_present(const u8* identify) {
    return (identify[kNvmeControllerVwcOffset] & 1u) != 0;
}

// True for a logical block size the driver can move through one page.
constexpr bool nvme_sector_size_ok(u32 bytes) {
    return bytes >= kNvmeSectorBytes && bytes <= kNvmeMaxSectorBytes &&
           (bytes % kNvmeSectorBytes) == 0;
}

enum class NvmeNamespaceProblem : u8 {
    None,
    NoSectors,
    Metadata,
    SectorSize,
};

// The parts of Identify Namespace data that the driver uses.
struct NvmeNamespaceInfo {
    u64 sectors;       // logical blocks
    u32 sector_bytes;  // from the active LBA format
    NvmeNamespaceProblem problem;

    bool usable() const { return problem == NvmeNamespaceProblem::None; }
};

constexpr NvmeNamespaceInfo nvme_parse_namespace(const u8* data) {
    NvmeNamespaceInfo info{};
    info.sectors = nvme_read_u64(data, 0);
    const u32 format_offset = kNvmeNamespaceFormatOffset +
                              (data[kNvmeNamespaceFlbasOffset] & kNvmeNamespaceFormatMask) *
                                  kNvmeLbaFormatBytes;
    const u32 metadata =
        static_cast<u32>(data[format_offset]) | (static_cast<u32>(data[format_offset + 1]) << 8);
    const u32 lbads = data[format_offset + 2];
    info.sector_bytes = lbads < 32 ? (1u << lbads) : 0xFFFFFFFFu;
    if (info.sectors == 0) {
        info.problem = NvmeNamespaceProblem::NoSectors;
    } else if (metadata != 0) {
        info.problem = NvmeNamespaceProblem::Metadata;
    } else if (!nvme_sector_size_ok(info.sector_bytes)) {
        info.problem = NvmeNamespaceProblem::SectorSize;
    } else {
        info.problem = NvmeNamespaceProblem::None;
    }
    return info;
}

// Where a 512-byte disk sector sits in the logical block that holds it.
struct NvmeSectorPlace {
    u64 logical;
    u32 offset;
};

constexpr NvmeSectorPlace nvme_sector_place(u64 sector, u32 sector_bytes) {
    const u32 per_logical = sector_bytes / kNvmeSectorBytes;
    return NvmeSectorPlace{sector / per_logical,
                           static_cast<u32>((sector % per_logical) * kNvmeSectorBytes)};
}

// The disk's size in 512-byte sectors from the namespace's logical block
// count.
constexpr u64 nvme_disk_sectors(u64 logical_sectors, u32 sector_bytes) {
    return logical_sectors * (sector_bytes / kNvmeSectorBytes);
}

// Finds every NVMe controller and opens each of its usable namespaces as a
// Disk, storing at most `max` in `out` and returning how many were stored.
// The DMA pages come from `pages` and stay allocated for as long as the
// disks are used.
u32 nvme_open_disks(MmioMapper& mmio, PhysAllocator& pages, Disk* out, u32 max);

}
