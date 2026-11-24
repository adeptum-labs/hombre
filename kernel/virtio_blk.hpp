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

#include "../common/types.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"
#include "pci.hpp"

// A polled virtio-blk driver for modern (1.x) virtio over PCI. This header
// holds the layouts and the ring logic, which touch no hardware; the parts
// that talk to the device are in virtio_blk.cpp and are kernel only.

namespace hombre {

inline constexpr u16 kVirtioVendor = 0x1AF4;
inline constexpr u16 kVirtioBlkModernDevice = 0x1042;
inline constexpr u16 kVirtioBlkTransitionalDevice = 0x1001;

// Device status bits.
inline constexpr u8 kVirtioStatusAcknowledge = 1;
inline constexpr u8 kVirtioStatusDriver = 2;
inline constexpr u8 kVirtioStatusDriverOk = 4;
inline constexpr u8 kVirtioStatusFeaturesOk = 8;
inline constexpr u8 kVirtioStatusFailed = 128;

// VIRTIO_F_VERSION_1, the only feature the driver accepts.
inline constexpr u64 kVirtioFeatureVersion1 = 1ull << 32;
// The device refuses writes.
inline constexpr u64 kVirtioBlkFeatureRo = 1ull << 5;
// The device takes flush requests, so writes may sit in its cache until one.
inline constexpr u64 kVirtioBlkFeatureFlush = 1ull << 9;

// Vendor-specific PCI capability and the virtio structure types it names.
inline constexpr u8 kPciCapVendor = 0x09;
inline constexpr u8 kVirtioCapCommon = 1;
inline constexpr u8 kVirtioCapNotify = 2;
inline constexpr u8 kVirtioCapIsr = 3;
inline constexpr u8 kVirtioCapDevice = 4;

inline constexpr u16 kVirtqDescNext = 1;
inline constexpr u16 kVirtqDescWrite = 2;
inline constexpr u16 kVirtqAvailNoInterrupt = 1;

inline constexpr u32 kVirtioBlkTypeIn = 0;
inline constexpr u32 kVirtioBlkTypeOut = 1;
inline constexpr u32 kVirtioBlkTypeFlush = 4;
inline constexpr u8 kVirtioBlkStatusOk = 0;
inline constexpr u32 kVirtioBlkSectorSize = 512;
// Each request moves at most 64 KiB.
inline constexpr u32 kVirtioBlkMaxChunkSectors = 128;
// The queue is never made larger than this; one request uses three entries.
inline constexpr u16 kVirtqMaxSize = 16;

struct VirtqDesc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct VirtqUsedElem {
    u32 id;
    u32 len;
};

struct VirtioBlkRequestHeader {
    u32 type;
    u32 reserved;
    u64 sector;
};

// struct virtio_pci_common_cfg. The 64-bit queue addresses are split into
// halves, which is how the driver writes them.
struct VirtioPciCommonCfg {
    u32 device_feature_select;
    u32 device_feature;
    u32 driver_feature_select;
    u32 driver_feature;
    u16 msix_config;
    u16 num_queues;
    u8 device_status;
    u8 config_generation;
    u16 queue_select;
    u16 queue_size;
    u16 queue_msix_vector;
    u16 queue_enable;
    u16 queue_notify_off;
    u32 queue_desc_lo;
    u32 queue_desc_hi;
    u32 queue_driver_lo;
    u32 queue_driver_hi;
    u32 queue_device_lo;
    u32 queue_device_hi;
};

static_assert(sizeof(VirtqDesc) == 16);
static_assert(sizeof(VirtqUsedElem) == 8);
static_assert(sizeof(VirtioBlkRequestHeader) == 16);
static_assert(__builtin_offsetof(VirtioPciCommonCfg, device_status) == 0x14);
static_assert(__builtin_offsetof(VirtioPciCommonCfg, queue_select) == 0x16);
static_assert(__builtin_offsetof(VirtioPciCommonCfg, queue_size) == 0x18);
static_assert(__builtin_offsetof(VirtioPciCommonCfg, queue_enable) == 0x1C);
static_assert(__builtin_offsetof(VirtioPciCommonCfg, queue_notify_off) == 0x1E);
static_assert(__builtin_offsetof(VirtioPciCommonCfg, queue_desc_lo) == 0x20);
static_assert(__builtin_offsetof(VirtioPciCommonCfg, queue_driver_lo) == 0x28);
static_assert(__builtin_offsetof(VirtioPciCommonCfg, queue_device_lo) == 0x30);
static_assert(sizeof(VirtioPciCommonCfg) == 0x38);

// Fills descriptors 0..2 for one read or write: the request header (device
// reads it), the data buffer (device writes it for a read, reads it for a
// write) and the status byte (device writes it). Returns the head index, 0.
inline u16 virtio_build_data_chain(VirtqDesc* desc, u64 header_phys, u64 data_phys,
                                   u32 data_bytes, u64 status_phys, bool device_writes_data) {
    const u16 data_flags = device_writes_data ? kVirtqDescNext | kVirtqDescWrite : kVirtqDescNext;
    desc[0] = VirtqDesc{header_phys, sizeof(VirtioBlkRequestHeader), kVirtqDescNext, 1};
    desc[1] = VirtqDesc{data_phys, data_bytes, data_flags, 2};
    desc[2] = VirtqDesc{status_phys, 1, kVirtqDescWrite, 0};
    return 0;
}

inline u16 virtio_build_read_chain(VirtqDesc* desc, u64 header_phys, u64 data_phys,
                                   u32 data_bytes, u64 status_phys) {
    return virtio_build_data_chain(desc, header_phys, data_phys, data_bytes, status_phys, true);
}

// Fills descriptors 0..1 for a flush, which carries no data: the request
// header and the status byte. Returns the head index, 0.
inline u16 virtio_build_flush_chain(VirtqDesc* desc, u64 header_phys, u64 status_phys) {
    desc[0] = VirtqDesc{header_phys, sizeof(VirtioBlkRequestHeader), kVirtqDescNext, 1};
    desc[1] = VirtqDesc{status_phys, 1, kVirtqDescWrite, 0};
    return 0;
}

// True when [sector, sector + sectors) is a non-empty range inside a drive
// of `capacity` sectors. Written so that nothing can overflow.
constexpr bool virtio_blk_range_ok(u64 sector, u64 sectors, u64 capacity) {
    return sectors != 0 && sector < capacity && sectors <= capacity - sector;
}

// True when a buffer of `sectors` sectors at dest_phys is not at address 0
// and does not run past the end of the 64-bit address space.
constexpr bool virtio_blk_dest_ok(u64 dest_phys, u32 sectors) {
    const u64 bytes = static_cast<u64>(sectors) * kVirtioBlkSectorSize;
    return dest_phys != 0 && (bytes == 0 || bytes - 1 <= ~dest_phys);
}

constexpr u32 virtio_blk_chunk_sectors(u32 remaining) {
    return remaining < kVirtioBlkMaxChunkSectors ? remaining : kVirtioBlkMaxChunkSectors;
}

// A split ring needs a power-of-two size: the largest power of two that is
// at most both the device's maximum and `limit`, or 0 when the device has
// no queue.
constexpr u16 virtq_queue_size(u16 device_max, u16 limit) {
    const u16 cap = device_max < limit ? device_max : limit;
    if (cap == 0) {
        return 0;
    }
    u16 size = 1;
    while (static_cast<u32>(size) * 2 <= cap) {
        size = static_cast<u16>(size * 2);
    }
    return size;
}

// Where the three parts of a split ring of `size` entries go in one block:
// the descriptor table (16-byte aligned) first, then the available ring
// (flags, idx, ring[size], used_event; 2-byte aligned), then the used ring
// (flags, idx, ring[size] of id and len, avail_event; 4-byte aligned).
struct VirtqLayout {
    u32 desc_offset;
    u32 avail_offset;
    u32 used_offset;
    u32 bytes;
};

constexpr VirtqLayout virtq_layout(u16 size) {
    const u32 avail = 16u * size;
    const u32 used = (avail + 6u + 2u * size + 3u) & ~3u;
    return VirtqLayout{0, avail, used, used + 6u + 8u * size};
}

constexpr u16 virtq_slot(u16 index, u16 size) {
    return static_cast<u16>(index & (size - 1));
}

// The driver's view of one split ring. avail and used are read as u16
// arrays: [0] is flags, [1] is idx and the available ring's entries start
// at [2]. The indices run free over all of u16 and are reduced to a slot
// with virtq_slot.
struct VirtqRing {
    VirtqDesc* desc;
    volatile u16* avail;
    volatile u16* used;
    u16 size;
    u16 avail_idx;  // the idx the driver published last
    u16 last_used;  // used entries taken so far
};

// Keeps the compiler from moving memory accesses across it. On x86 the CPU
// keeps stores in order and loads in order, which is all the ring needs.
inline void virtq_compiler_barrier() {
    asm volatile("" : : : "memory");
}

// Points `ring` at a zeroed block laid out by virtq_layout and asks the
// device not to raise interrupts: the driver polls.
inline void virtq_ring_init(VirtqRing& ring, u8* base, u16 size) {
    const VirtqLayout layout = virtq_layout(size);
    ring.desc = reinterpret_cast<VirtqDesc*>(base + layout.desc_offset);
    ring.avail = reinterpret_cast<volatile u16*>(base + layout.avail_offset);
    ring.used = reinterpret_cast<volatile u16*>(base + layout.used_offset);
    ring.size = size;
    ring.avail_idx = 0;
    ring.last_used = 0;
    ring.avail[0] = kVirtqAvailNoInterrupt;
}

inline volatile VirtqUsedElem* virtq_used_elems(const VirtqRing& ring) {
    return reinterpret_cast<volatile VirtqUsedElem*>(ring.used + 2);
}

// Offers the chain at `head` to the device. The descriptors and the ring
// entry are written before idx moves, so the device never sees a
// half-built request.
inline void virtq_publish(VirtqRing& ring, u16 head) {
    ring.avail[2 + virtq_slot(ring.avail_idx, ring.size)] = head;
    virtq_compiler_barrier();
    ring.avail_idx = static_cast<u16>(ring.avail_idx + 1);
    ring.avail[1] = ring.avail_idx;
    virtq_compiler_barrier();
}

// Takes the next entry the device returned, if any. Does not wait.
inline bool virtq_poll_used(VirtqRing& ring, VirtqUsedElem& out) {
    if (ring.used[1] == ring.last_used) {
        return false;
    }
    virtq_compiler_barrier();
    const volatile VirtqUsedElem& elem = virtq_used_elems(ring)[virtq_slot(ring.last_used, ring.size)];
    out.id = elem.id;
    out.len = elem.len;
    ring.last_used = static_cast<u16>(ring.last_used + 1);
    return true;
}

// Requests published but not yet taken back.
inline u16 virtq_in_flight(const VirtqRing& ring) {
    return static_cast<u16>(ring.avail_idx - ring.last_used);
}

// One virtio structure found through a vendor capability: `length` bytes at
// `offset` into BAR `bar`.
struct VirtioCapRegion {
    bool present;
    u8 bar;
    u32 offset;
    u32 length;
};

struct VirtioPciCaps {
    VirtioCapRegion common;
    VirtioCapRegion notify;
    VirtioCapRegion isr;
    VirtioCapRegion device;
    u32 notify_off_multiplier;
};

// Reads the configuration space dword at `offset` (a multiple of 4).
using PciConfigRead32 = u32 (*)(void* ctx, u8 offset);

// Walks the capability list and keeps the first virtio capability of each
// type that names BAR 0 to 5. The walk ends at a pointer below 0x40 and
// after at most 48 entries, so a list that loops still ends. A capability
// too short, or too close to the end of the 256-byte space, for its fields
// is skipped.
inline VirtioPciCaps virtio_find_caps(PciConfigRead32 read32, void* ctx) {
    constexpr u8 status_offset = 0x04;  // upper half of the command dword
    constexpr u32 status_cap_list = 1u << 4;
    constexpr u8 cap_pointer_offset = 0x34;
    constexpr u32 max_caps = 48;

    VirtioPciCaps caps{};
    if (((read32(ctx, status_offset) >> 16) & status_cap_list) == 0) {
        return caps;
    }
    u8 at = static_cast<u8>(read32(ctx, cap_pointer_offset) & 0xFCu);
    for (u32 n = 0; n < max_caps && at >= 0x40; ++n) {
        const u32 head = read32(ctx, at);
        const u8 next = static_cast<u8>((head >> 8) & 0xFCu);
        const u8 cap_len = static_cast<u8>(head >> 16);
        const u8 type = static_cast<u8>(head >> 24);
        if ((head & 0xFFu) == kPciCapVendor && cap_len >= 16 && at <= 0xF0) {
            const u8 bar = static_cast<u8>(read32(ctx, static_cast<u8>(at + 4)));
            VirtioCapRegion region{true, bar, read32(ctx, static_cast<u8>(at + 8)),
                                   read32(ctx, static_cast<u8>(at + 12))};
            VirtioCapRegion* slot = nullptr;
            switch (type) {
                case kVirtioCapCommon: slot = &caps.common; break;
                case kVirtioCapIsr: slot = &caps.isr; break;
                case kVirtioCapDevice: slot = &caps.device; break;
                case kVirtioCapNotify:
                    if (cap_len >= 20 && at <= 0xEC) {
                        slot = &caps.notify;
                    }
                    break;
                default: break;
            }
            if (slot != nullptr && bar <= 5 && !slot->present) {
                *slot = region;
                if (type == kVirtioCapNotify) {
                    caps.notify_off_multiplier = read32(ctx, static_cast<u8>(at + 16));
                }
            }
        }
        at = next;
    }
    return caps;
}

// One virtio-blk device driven through queue 0 without interrupts. Only one
// request is in flight at a time.
class VirtioBlk {
public:
    // Brings the device up: maps its common, notify and device regions,
    // negotiates VIRTIO_F_VERSION_1, and the read-only and flush features
    // when the device offers them, and sets up queue 0 in pages from
    // `pages`. Returns false, with the device reset and the pages given
    // back, when a region is missing or the device refuses a step.
    bool init(PciAddress address, MmioMapper& mmio, PhysAllocator& pages);

    // The drive's size in 512-byte sectors, read from the device
    // configuration at init.
    u64 sector_count() const { return capacity_; }

    // Reads the size from the device configuration again, for a medium the
    // host has changed under the device. True when it is a size the device
    // gave twice the same; the size answered by sector_count is then new.
    bool refresh_capacity();

    // Reads `sectors` sectors from `sector` on into physical memory at
    // dest_phys, in requests of at most 64 KiB. Returns false for an empty
    // range or one past the end of the drive, a destination at 0 or one
    // that wraps, an error status from the device, or a request that does
    // not complete in time. A timeout or a used entry for an unknown
    // request resets the device, so every later read fails too.
    //
    // After a false return with quiesced() false, the device could not be
    // reset and may still write to dest_phys: the caller must not free or
    // reuse that memory.
    bool read(u64 sector, u32 sectors, u64 dest_phys);

    // Writes `sectors` sectors from physical memory at src_phys to the
    // drive from `sector` on, failing as read does, and always on a
    // read-only drive. The data may sit in the device's cache until flush.
    bool write(u64 sector, u32 sectors, u64 src_phys);

    // Asks the device to put every write it has taken on the medium. True
    // at once on a device without a cache to flush.
    bool flush();

    // True when the device refuses writes.
    bool read_only() const { return read_only_; }

    // False once a reset the driver asked for did not complete: the device
    // may still write to memory it was given.
    bool quiesced() const { return !wedged_; }

private:
    bool transfer(u32 type, u64 sector, u32 sectors, u64 phys);
    bool transfer_chunk(u32 type, u64 sector, u32 sectors, u64 phys);
    bool complete(u16 head);
    bool fail(PhysAllocator& pages);
    bool stop();

    PciAddress address_{0, 0, 0};
    volatile VirtioPciCommonCfg* common_ = nullptr;
    volatile u16* notify_ = nullptr;
    VirtqRing ring_{};
    u64 ring_phys_ = 0;
    u8* request_ = nullptr;  // the request header, then the status byte
    u64 request_phys_ = 0;
    u64 capacity_ = 0;
    volatile u32* config_ = nullptr;
    bool ready_ = false;
    bool wedged_ = false;
    bool read_only_ = false;
    bool has_flush_ = false;
};

// Finds the first virtio-blk function on the PCI buses: vendor 0x1AF4 with
// device 0x1042 (modern) or 0x1001 (transitional).
bool virtio_blk_find(PciAddress& found);

// Finds every virtio-blk function, in the order the PCI scan meets them,
// and stores at most max of their addresses in found. Returns how many it
// stored.
u32 virtio_blk_find_all(PciAddress* found, u32 max);

}
