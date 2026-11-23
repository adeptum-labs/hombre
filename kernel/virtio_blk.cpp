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

#include "virtio_blk.hpp"

namespace hombre {

namespace {

constexpr u8 kPciBar0Offset = 0x10;
constexpr u32 kPciCommandInterruptDisable = 1u << 10;
constexpr u8 kVirtioStatusNeedsReset = 64;

// How long to spin on the device. One pause takes roughly 10 to 150 cycles
// depending on the CPU, so 2^29 rounds is about 2 to 25 seconds at 3 GHz:
// far beyond any request a working device needs, short enough that a dead
// one does not hang the boot for good.
constexpr u64 kPollLimit = 1ull << 29;
// Device status changes, such as the read-back after a reset, are quick.
constexpr u64 kStatusPollLimit = 1ull << 20;
// Rereads of the device configuration while it keeps changing under us.
constexpr u32 kConfigReadAttempts = 8;

constexpr u32 kRequestStatusOffset = sizeof(VirtioBlkRequestHeader);

static_assert(virtq_layout(kVirtqMaxSize).bytes <= 4096);

inline void cpu_pause() {
    asm volatile("pause");
}

// Orders the ring writes before the doorbell write to device memory.
inline void memory_fence() {
    asm volatile("mfence" : : : "memory");
}

u32 config_read32(void* ctx, u8 offset) {
    return pci_read32(*static_cast<const PciAddress*>(ctx), offset);
}

// Maps the structure a capability describes. The BAR must be a memory BAR
// that firmware has placed; a 64-bit BAR takes the next slot as its high
// half. Returns nullptr when the region cannot be used.
volatile u8* map_region(PciAddress address, const VirtioCapRegion& region, u32 min_length,
                        MmioMapper& mmio) {
    if (!region.present || region.length < min_length) {
        return nullptr;
    }
    const u8 bar_offset = static_cast<u8>(kPciBar0Offset + 4 * region.bar);
    const u32 low = pci_read32(address, bar_offset);
    u32 high = 0;
    if ((low & 1u) == 0 && ((low >> 1) & 3u) == 2) {
        if (region.bar > 4) {
            return nullptr;
        }
        high = pci_read32(address, static_cast<u8>(bar_offset + 4));
    }
    const PciBar bar = pci_decode_bar(low, high);
    if (bar.is_io || bar.phys == 0 || region.offset > ~bar.phys) {
        return nullptr;
    }
    return static_cast<volatile u8*>(mmio.map(bar.phys + region.offset, region.length));
}

// Sets and clears bits in the PCI command register. The status register
// shares the dword and its bits clear when written as 1, so the upper half
// is written as 0.
void set_command(PciAddress address, u32 set, u32 clear) {
    const u32 command = pci_read32(address, kPciCommandOffset) & 0xFFFFu;
    pci_write32(address, kPciCommandOffset, (command & ~clear) | set);
}

void write_queue_address(volatile u32& lo, volatile u32& hi, u64 phys) {
    lo = static_cast<u32>(phys);
    hi = static_cast<u32>(phys >> 32);
}

// Writes 0 to the status and waits for the device to read back 0, which
// tells that the reset is complete.
bool reset_device(volatile VirtioPciCommonCfg* common) {
    common->device_status = 0;
    for (u64 i = 0; i < kStatusPollLimit; ++i) {
        if (common->device_status == 0) {
            return true;
        }
        cpu_pause();
    }
    return false;
}

struct FindState {
    PciAddress address;
    bool found;
};

bool match_block_device(void* ctx, PciAddress address, u16 vendor, u16 device) {
    if (vendor != kVirtioVendor ||
        (device != kVirtioBlkModernDevice && device != kVirtioBlkTransitionalDevice)) {
        return false;
    }
    FindState* state = static_cast<FindState*>(ctx);
    state->address = address;
    state->found = true;
    return true;
}

}

bool VirtioBlk::init(PciAddress address, MmioMapper& mmio, PhysAllocator& pages) {
    *this = VirtioBlk{};

    const VirtioPciCaps caps = virtio_find_caps(&config_read32, &address);

    // Memory decoding on, so the registers can be reached, but bus mastering
    // off: the device must not DMA before it has been reset and given rings.
    // The driver polls, so the legacy interrupt line stays quiet too.
    address_ = address;
    set_command(address, kPciCommandMemorySpace | kPciCommandInterruptDisable,
                kPciCommandBusMaster);

    common_ = reinterpret_cast<volatile VirtioPciCommonCfg*>(
        map_region(address, caps.common, sizeof(VirtioPciCommonCfg), mmio));
    volatile u8* notify_base = map_region(address, caps.notify, 2, mmio);
    volatile u8* device = map_region(address, caps.device, sizeof(u64), mmio);
    if (common_ == nullptr || notify_base == nullptr || device == nullptr) {
        common_ = nullptr;
        return false;
    }

    if (!reset_device(common_)) {
        return fail(pages);
    }
    common_->device_status = kVirtioStatusAcknowledge;
    common_->device_status = kVirtioStatusAcknowledge | kVirtioStatusDriver;

    common_->device_feature_select = 0;
    u64 offered = common_->device_feature;
    common_->device_feature_select = 1;
    offered |= static_cast<u64>(common_->device_feature) << 32;
    if ((offered & kVirtioFeatureVersion1) == 0) {
        return fail(pages);
    }
    const u64 wanted =
        kVirtioFeatureVersion1 | (offered & (kVirtioBlkFeatureRo | kVirtioBlkFeatureFlush));
    common_->driver_feature_select = 0;
    common_->driver_feature = static_cast<u32>(wanted);
    common_->driver_feature_select = 1;
    common_->driver_feature = static_cast<u32>(wanted >> 32);

    u8 status = kVirtioStatusAcknowledge | kVirtioStatusDriver | kVirtioStatusFeaturesOk;
    common_->device_status = status;
    if ((common_->device_status & kVirtioStatusFeaturesOk) == 0) {
        return fail(pages);
    }

    // One read takes three descriptors, so a queue of 1 or 2 is no use.
    common_->queue_select = 0;
    const u16 size = virtq_queue_size(common_->queue_size, kVirtqMaxSize);
    if (size < 4) {
        return fail(pages);
    }
    common_->queue_size = size;

    ring_phys_ = pages.alloc_pages(1);
    request_phys_ = pages.alloc_pages(1);
    u8* ring_memory = ring_phys_ != 0 ? pages.to_ptr(ring_phys_) : nullptr;
    request_ = request_phys_ != 0 ? pages.to_ptr(request_phys_) : nullptr;
    if (ring_memory == nullptr || request_ == nullptr) {
        return fail(pages);
    }
    virtq_ring_init(ring_, ring_memory, size);
    const VirtqLayout layout = virtq_layout(size);
    write_queue_address(common_->queue_desc_lo, common_->queue_desc_hi,
                        ring_phys_ + layout.desc_offset);
    write_queue_address(common_->queue_driver_lo, common_->queue_driver_hi,
                        ring_phys_ + layout.avail_offset);
    write_queue_address(common_->queue_device_lo, common_->queue_device_hi,
                        ring_phys_ + layout.used_offset);

    const u64 notify_offset =
        static_cast<u64>(common_->queue_notify_off) * caps.notify_off_multiplier;
    if (notify_offset + 2 > caps.notify.length) {
        return fail(pages);
    }
    notify_ = reinterpret_cast<volatile u16*>(notify_base + notify_offset);

    // Everything the driver can check up front has passed: let the device
    // reach the rings.
    set_command(address, kPciCommandBusMaster, 0);
    common_->queue_enable = 1;
    memory_fence();
    status |= kVirtioStatusDriverOk;
    common_->device_status = status;
    if ((common_->device_status & (kVirtioStatusFailed | kVirtioStatusNeedsReset)) != 0) {
        return fail(pages);
    }

    // The capacity is 64 bits wide but read as two halves, so it is read
    // again whenever the configuration generation moves in between.
    volatile u32* config = reinterpret_cast<volatile u32*>(device);
    bool stable = false;
    for (u32 attempt = 0; attempt < kConfigReadAttempts && !stable; ++attempt) {
        const u8 generation = common_->config_generation;
        const u32 lo = config[0];
        const u32 hi = config[1];
        stable = generation == common_->config_generation;
        capacity_ = (static_cast<u64>(hi) << 32) | lo;
    }
    if (!stable) {
        return fail(pages);
    }

    read_only_ = (wanted & kVirtioBlkFeatureRo) != 0;
    has_flush_ = (wanted & kVirtioBlkFeatureFlush) != 0;
    ready_ = true;
    return true;
}

// Tells the device it failed, resets it so it stops using the queue, turns
// bus mastering off and gives the pages back. Returns false for the caller
// to pass on.
bool VirtioBlk::fail(PhysAllocator& pages) {
    common_->device_status = common_->device_status | kVirtioStatusFailed;
    const bool quiet = reset_device(common_);
    set_command(address_, 0, kPciCommandBusMaster);
    // A device that does not finish its reset may still write to the
    // rings, so their pages are then never handed out again.
    if (quiet) {
        if (ring_phys_ != 0) {
            pages.free_pages(ring_phys_, 1);
        }
        if (request_phys_ != 0) {
            pages.free_pages(request_phys_, 1);
        }
    }
    *this = VirtioBlk{};
    wedged_ = !quiet;
    return false;
}

bool VirtioBlk::read(u64 sector, u32 sectors, u64 dest_phys) {
    return transfer(kVirtioBlkTypeIn, sector, sectors, dest_phys);
}

bool VirtioBlk::write(u64 sector, u32 sectors, u64 src_phys) {
    return !read_only_ && transfer(kVirtioBlkTypeOut, sector, sectors, src_phys);
}

bool VirtioBlk::flush() {
    if (!ready_) {
        return false;
    }
    if (!has_flush_) {
        return true;
    }
    VirtioBlkRequestHeader* header = reinterpret_cast<VirtioBlkRequestHeader*>(request_);
    header->type = kVirtioBlkTypeFlush;
    header->reserved = 0;
    header->sector = 0;
    request_[kRequestStatusOffset] = 0xFF;
    return complete(virtio_build_flush_chain(ring_.desc, request_phys_,
                                             request_phys_ + kRequestStatusOffset));
}

bool VirtioBlk::transfer(u32 type, u64 sector, u32 sectors, u64 phys) {
    if (!ready_ || !virtio_blk_range_ok(sector, sectors, capacity_) ||
        !virtio_blk_dest_ok(phys, sectors)) {
        return false;
    }
    while (sectors != 0) {
        const u32 chunk = virtio_blk_chunk_sectors(sectors);
        if (!transfer_chunk(type, sector, chunk, phys)) {
            return false;
        }
        sector += chunk;
        sectors -= chunk;
        phys += static_cast<u64>(chunk) * kVirtioBlkSectorSize;
    }
    return true;
}

bool VirtioBlk::transfer_chunk(u32 type, u64 sector, u32 sectors, u64 phys) {
    VirtioBlkRequestHeader* header = reinterpret_cast<VirtioBlkRequestHeader*>(request_);
    header->type = type;
    header->reserved = 0;
    header->sector = sector;
    // Any value but OK: a device that never writes the status fails the request.
    request_[kRequestStatusOffset] = 0xFF;
    return complete(virtio_build_data_chain(ring_.desc, request_phys_, phys,
                                            sectors * kVirtioBlkSectorSize,
                                            request_phys_ + kRequestStatusOffset,
                                            type == kVirtioBlkTypeIn));
}

// Hands the chain at `head` to the device and waits for it to come back.
bool VirtioBlk::complete(u16 head) {
    volatile u8* status = request_ + kRequestStatusOffset;
    virtq_publish(ring_, head);
    memory_fence();
    *notify_ = 0;  // the queue index

    VirtqUsedElem used{};
    u64 spins = 0;
    while (!virtq_poll_used(ring_, used)) {
        if (++spins == kPollLimit) {
            return stop();
        }
        cpu_pause();
    }
    if (used.id != head) {
        return stop();
    }
    return *status == kVirtioBlkStatusOk;
}

// The queue is out of step with the device. Resetting the device keeps a
// late completion from writing into a buffer the caller has moved on from;
// when the reset does not complete, the driver is wedged and quiesced()
// tells the caller to keep the buffer. Bus mastering goes off either way.
// The ring pages stay allocated, as the device may still use them.
bool VirtioBlk::stop() {
    ready_ = false;
    wedged_ = !reset_device(common_);
    set_command(address_, 0, kPciCommandBusMaster);
    return false;
}

namespace {

struct FindAllState {
    PciAddress* out;
    u32 max;
    u32 count;
};

bool collect_block_device(void* ctx, PciAddress address, u16 vendor, u16 device) {
    FindState one{{0, 0, 0}, false};
    if (!match_block_device(&one, address, vendor, device)) {
        return false;
    }
    FindAllState* state = static_cast<FindAllState*>(ctx);
    if (state->count < state->max) {
        state->out[state->count] = address;
    }
    ++state->count;
    return false;
}

}

u32 virtio_blk_find_all(PciAddress* found, u32 max) {
    FindAllState state{found, max, 0};
    pci_scan(&collect_block_device, &state);
    return state.count < max ? state.count : max;
}

bool virtio_blk_find(PciAddress& found) {
    FindState state{{0, 0, 0}, false};
    pci_scan(&match_block_device, &state);
    if (state.found) {
        found = state.address;
    }
    return state.found;
}

}
