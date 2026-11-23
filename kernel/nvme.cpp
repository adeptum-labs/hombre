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

#include "nvme.hpp"

#include "../common/printable.hpp"
#include "../common/serial.hpp"
#include "pci.hpp"

namespace hombre {

namespace {

constexpr u8 kPciClassOffset = 0x08;
constexpr u8 kPciBar0Offset = 0x10;
constexpr u32 kPciCommandInterruptDisable = 1u << 10;
constexpr u32 kClassShift = 8;

// Controller registers, at the start of BAR0. The doorbells follow at
// kNvmeDoorbellBase.
constexpr u32 kCap = 0x00;
constexpr u32 kIntms = 0x0C;
constexpr u32 kCc = 0x14;
constexpr u32 kCsts = 0x1C;
constexpr u32 kAqa = 0x24;
constexpr u32 kAsq = 0x28;
constexpr u32 kAcq = 0x30;

constexpr u32 kCcEnable = 1u << 0;
constexpr u32 kCcIosqesShift = 16;
constexpr u32 kCcIocqesShift = 20;
constexpr u32 kCcIosqes = 6;  // 64-byte submission entries
constexpr u32 kCcIocqes = 4;  // 16-byte completion entries
constexpr u32 kCstsReady = 1u << 0;
constexpr u32 kCstsFatal = 1u << 1;

constexpr u32 kMaxNvmeDisks = 8;
constexpr u32 kMaxNvmeQueues = 8;
constexpr u32 kNamespaceListEntries = kNvmePageBytes / 4;

// One timeout unit is 500 ms. A pause round takes roughly 10 to 150 cycles,
// so 2^27 rounds is about half a second to seven seconds at 3 GHz: the
// specification's timer with the same margin as the AHCI driver's fixed
// bound, so a controller that never answers is given up on. A controller
// that names no timeout gets four units, two seconds.
constexpr u32 kDefaultTimeoutUnits = 4;
constexpr u64 kSpinsPerTimeoutUnit = 1ull << 27;

constexpr u64 timeout_spins(u32 units) {
    return static_cast<u64>(units != 0 ? units : kDefaultTimeoutUnits) * kSpinsPerTimeoutUnit;
}

enum class NvmeResult : u8 {
    Ok,
    Failed,
    Timeout,
};

// The queue pair that serves one controller. All of its namespaces share
// it, and one command is in flight at a time.
struct NvmeQueue {
    volatile u8* regs;
    NvmeCommand* sq;
    NvmeCompletion* cq;
    u32 qid;
    u32 depth;
    u32 dstrd;
    u32 tail;
    u32 head;
    u32 phase;
    u32 cid;
    u64 spins;
    bool broken;
};

struct NvmeDisk {
    NvmeQueue* queue;
    u8* bounce;
    u64 bounce_phys;
    u64 sectors;
    u32 sector_bytes;
    u32 nsid;
    bool flush;
    bool failed;
};

constinit NvmeQueue queue_contexts[kMaxNvmeQueues]{};
constinit NvmeDisk disk_contexts[kMaxNvmeDisks]{};
constinit u32 queue_context_count = 0;
constinit u32 disk_context_count = 0;

void cpu_pause() {
    asm volatile("pause");
}

// Orders the queue writes before the doorbell write to the controller.
void memory_fence() {
    asm volatile("mfence" : : : "memory");
}

u32 read32(const volatile u8* base, u32 offset) {
    return *reinterpret_cast<const volatile u32*>(base + offset);
}

void write32(volatile u8* base, u32 offset, u32 value) {
    *reinterpret_cast<volatile u32*>(base + offset) = value;
}

u64 read64(const volatile u8* base, u32 offset) {
    const u64 low = read32(base, offset);
    const u64 high = read32(base, offset + 4);
    return low | (high << 32);
}

// A controller need not accept one 64-bit store, so the halves go out
// separately, low first.
void write_address(volatile u8* base, u32 offset, u64 value) {
    write32(base, offset, static_cast<u32>(value));
    write32(base, offset + 4, static_cast<u32>(value >> 32));
}

bool wait_bits(const volatile u8* base, u32 offset, u32 mask, u32 value, u64 limit) {
    for (u64 i = 0; i < limit; ++i) {
        if ((read32(base, offset) & mask) == value) {
            return true;
        }
        cpu_pause();
    }
    return false;
}

void write_controller_prefix(PciAddress address) {
    serial::write("nvme: ");
    serial::write_hex(address.bus);
    serial::write_char(':');
    serial::write_hex(address.device);
    serial::write_char('.');
    serial::write_hex(address.function);
}

void log_controller(PciAddress address, const char* model, u32 namespaces) {
    char text[kNvmeControllerModelBytes + 1];
    sanitize_text(text, sizeof(text), model, kNvmeControllerModelBytes);
    write_controller_prefix(address);
    serial::write_char(' ');
    serial::write(text);
    serial::write(", ");
    serial::write_dec(namespaces);
    serial::write(" namespaces\n");
}

void log_controller(PciAddress address, const char* reason) {
    write_controller_prefix(address);
    serial::write(": ");
    serial::write(reason);
    serial::write_char('\n');
}

void write_namespace_prefix(u32 nsid) {
    serial::write("nvme: ns ");
    serial::write_dec(nsid);
    serial::write(": ");
}

void log_namespace_skip(u32 nsid, const char* reason) {
    write_namespace_prefix(nsid);
    serial::write(reason);
    serial::write_char('\n');
}

void log_unsupported_sectors(u32 nsid, u32 sector_bytes) {
    write_namespace_prefix(nsid);
    serial::write_dec(sector_bytes);
    serial::write("-byte sectors not supported, skipped\n");
}

void log_namespace(u32 nsid, const NvmeNamespaceInfo& info, bool flush) {
    write_namespace_prefix(nsid);
    serial::write_dec(info.sectors);
    serial::write(" sectors, ");
    serial::write_dec(info.sector_bytes);
    serial::write("-byte sectors");
    if (!flush) {
        serial::write(", no flush");
    }
    serial::write_char('\n');
}

void log_io_failure(u32 nsid, const char* operation, NvmeResult result, u32 status) {
    write_namespace_prefix(nsid);
    serial::write(operation);
    if (result == NvmeResult::Timeout) {
        serial::write(" timed out, disk unusable\n");
    } else {
        serial::write(" failed, status 0x");
        serial::write_hex(status);
        serial::write_char('\n');
    }
}

void set_command(PciAddress address, u32 set, u32 clear) {
    const u32 command = pci_read32(address, kPciCommandOffset) & 0xFFFFu;
    pci_write32(address, kPciCommandOffset, (command & ~clear) | set);
}

// Maps the registers and the four doorbells the driver uses: the admin
// queue's and the I/O queue's.
volatile u8* map_registers(PciAddress address, MmioMapper& mmio, u32 dstrd) {
    const u32 bar_low = pci_read32(address, kPciBar0Offset);
    u32 bar_high = 0;
    if ((bar_low & 1u) == 0 && ((bar_low >> 1) & 3u) == 2) {
        bar_high = pci_read32(address, static_cast<u8>(kPciBar0Offset + 4));
    }
    const PciBar bar = pci_decode_bar(bar_low, bar_high);
    if (bar.is_io || bar.phys == 0) {
        return nullptr;
    }
    const u64 window = kNvmeDoorbellBase + 4ull * (4ull << dstrd);
    return static_cast<volatile u8*>(mmio.map(bar.phys, window));
}

void init_queue(NvmeQueue& queue, volatile u8* regs, u32 dstrd, u32 qid, u32 depth,
                NvmeCommand* sq, NvmeCompletion* cq, u64 spins) {
    queue = NvmeQueue{};
    queue.regs = regs;
    queue.sq = sq;
    queue.cq = cq;
    queue.qid = qid;
    queue.depth = depth;
    queue.dstrd = dstrd;
    queue.phase = 1;
    queue.spins = spins;
}

u32 next_cid(NvmeQueue& queue) {
    const u32 cid = queue.cid;
    queue.cid = (queue.cid + 1) & kNvmeCompletionIdMask;
    return cid;
}

void ring_sq(NvmeQueue& queue) {
    memory_fence();
    write32(queue.regs, nvme_doorbell_offset(queue.dstrd, queue.qid, false), queue.tail);
}

void ring_cq(NvmeQueue& queue) {
    write32(queue.regs, nvme_doorbell_offset(queue.dstrd, queue.qid, true), queue.head);
}

// Places one command and waits for its completion. A timeout leaves the
// queue out of step with the controller, so it is marked broken and every
// disk that shares it stops taking commands.
NvmeResult run_command(NvmeQueue& queue, const NvmeCommand& command, u32& status) {
    if (queue.broken) {
        return NvmeResult::Failed;
    }
    queue.sq[queue.tail] = command;
    queue.tail = queue.tail + 1 == queue.depth ? 0 : queue.tail + 1;
    ring_sq(queue);
    const u32 cid = command.cdw0 >> 16;
    for (u64 spins = 0;; ++spins) {
        const volatile NvmeCompletion* entry = &queue.cq[queue.head];
        const u32 dw3 = entry->status;
        if (nvme_completion_phase(dw3) == queue.phase) {
            memory_fence();
            status = nvme_completion_status(dw3);
            const u32 completed = nvme_completion_id(dw3);
            queue.head = queue.head + 1 == queue.depth ? 0 : queue.head + 1;
            if (queue.head == 0) {
                queue.phase ^= 1;
            }
            ring_cq(queue);
            if (completed != cid) {
                queue.broken = true;
                return NvmeResult::Failed;
            }
            return status == 0 ? NvmeResult::Ok : NvmeResult::Failed;
        }
        if (spins == queue.spins) {
            queue.broken = true;
            return NvmeResult::Timeout;
        }
        cpu_pause();
    }
}

bool disable_controller(volatile u8* regs, u64 spins) {
    const u32 cc = read32(regs, kCc);
    if ((cc & kCcEnable) == 0) {
        return true;
    }
    write32(regs, kCc, cc & ~kCcEnable);
    return wait_bits(regs, kCsts, kCstsReady, 0, spins);
}

u32 count_namespaces(const u8* list) {
    u32 count = 0;
    while (count < kNamespaceListEntries && nvme_read_u32(list, count * 4) != 0) {
        ++count;
    }
    return count;
}

bool run_io(NvmeDisk& disk, bool write, u64 logical) {
    NvmeQueue& queue = *disk.queue;
    if (disk.failed || queue.broken) {
        return false;
    }
    u32 status = 0;
    const NvmeResult result =
        run_command(queue, nvme_build_rw(write ? kNvmeCmdWrite : kNvmeCmdRead, disk.nsid,
                                         next_cid(queue), logical, 1, disk.bounce_phys),
                    status);
    if (result == NvmeResult::Ok) {
        return true;
    }
    if (result == NvmeResult::Timeout) {
        disk.failed = true;
    }
    log_io_failure(disk.nsid, write ? "write" : "read", result, status);
    return false;
}

bool read_sector(void* ctx, u64 sector, u8* out) {
    NvmeDisk& disk = *static_cast<NvmeDisk*>(ctx);
    if (sector >= disk.sectors) {
        return false;
    }
    const NvmeSectorPlace place = nvme_sector_place(sector, disk.sector_bytes);
    if (!run_io(disk, false, place.logical)) {
        return false;
    }
    __builtin_memcpy(out, disk.bounce + place.offset, kNvmeSectorBytes);
    return true;
}

bool write_sector(void* ctx, u64 sector, const u8* data) {
    NvmeDisk& disk = *static_cast<NvmeDisk*>(ctx);
    if (sector >= disk.sectors) {
        return false;
    }
    const NvmeSectorPlace place = nvme_sector_place(sector, disk.sector_bytes);
    if (disk.sector_bytes > kNvmeSectorBytes && !run_io(disk, false, place.logical)) {
        return false;
    }
    __builtin_memcpy(disk.bounce + place.offset, data, kNvmeSectorBytes);
    return run_io(disk, true, place.logical);
}

bool flush_disk(void* ctx) {
    NvmeDisk& disk = *static_cast<NvmeDisk*>(ctx);
    if (!disk.flush) {
        return true;
    }
    if (disk.failed || disk.queue->broken) {
        return false;
    }
    u32 status = 0;
    const NvmeResult result =
        run_command(*disk.queue, nvme_build_flush(disk.nsid, next_cid(*disk.queue)), status);
    if (result == NvmeResult::Ok) {
        return true;
    }
    if (result == NvmeResult::Timeout) {
        disk.failed = true;
    }
    log_io_failure(disk.nsid, "flush", result, status);
    return false;
}

struct NvmeCandidate {
    u32 nsid;
    NvmeNamespaceInfo info;
};

void log_identify_failure(u32 nsid, NvmeResult result) {
    log_namespace_skip(nsid, result == NvmeResult::Timeout ? "identify timed out, skipped"
                                                           : "identify failed, skipped");
}

u32 open_controller(PciAddress address, MmioMapper& mmio, PhysAllocator& pages, Disk* out,
                    u32 max) {
    if (disk_context_count >= kMaxNvmeDisks || queue_context_count >= kMaxNvmeQueues) {
        return 0;
    }
    // Memory decoding and bus mastering on so the registers can be reached
    // and a controller firmware left enabled can finish what it queued before
    // it is stopped below.
    set_command(address,
                kPciCommandMemorySpace | kPciCommandBusMaster | kPciCommandInterruptDisable, 0);
    volatile u8* regs = map_registers(address, mmio, 0);
    if (regs == nullptr) {
        log_controller(address, "cannot map its memory BAR");
        return 0;
    }
    const u64 cap = read64(regs, kCap);
    const u32 dstrd = nvme_cap_dstrd(cap);
    regs = map_registers(address, mmio, dstrd);
    if (regs == nullptr) {
        log_controller(address, "cannot map its memory BAR");
        return 0;
    }
    if (!nvme_cap_supports_4k_pages(cap)) {
        log_controller(address, "needs larger than 4 KiB pages, skipped");
        return 0;
    }
    const u64 spins = timeout_spins(nvme_cap_timeout(cap));
    if (!disable_controller(regs, spins)) {
        log_controller(address, "disable timed out");
        return 0;
    }
    write32(regs, kIntms, 0xFFFFFFFFu);

    // The controller is stopped, so these pages can be given back when one
    // of them cannot be had. The namespace list gets a page of its own, as
    // the Identify Namespace commands write over the other one.
    const u64 asq_phys = pages.alloc_pages(1);
    const u64 acq_phys = asq_phys != 0 ? pages.alloc_pages(1) : 0;
    const u64 identify_phys = acq_phys != 0 ? pages.alloc_pages(1) : 0;
    const u64 list_phys = identify_phys != 0 ? pages.alloc_pages(1) : 0;
    NvmeCommand* asq = asq_phys != 0 ? reinterpret_cast<NvmeCommand*>(pages.to_ptr(asq_phys))
                                     : nullptr;
    NvmeCompletion* acq =
        acq_phys != 0 ? reinterpret_cast<NvmeCompletion*>(pages.to_ptr(acq_phys)) : nullptr;
    u8* identify = identify_phys != 0 ? pages.to_ptr(identify_phys) : nullptr;
    u8* list = list_phys != 0 ? pages.to_ptr(list_phys) : nullptr;
    if (asq == nullptr || acq == nullptr || identify == nullptr || list == nullptr) {
        if (asq_phys != 0) {
            pages.free_pages(asq_phys, 1);
        }
        if (acq_phys != 0) {
            pages.free_pages(acq_phys, 1);
        }
        if (identify_phys != 0) {
            pages.free_pages(identify_phys, 1);
        }
        if (list_phys != 0) {
            pages.free_pages(list_phys, 1);
        }
        log_controller(address, "no memory");
        return 0;
    }

    write32(regs, kAqa, nvme_build_aqa(kNvmeAdminDepth, kNvmeAdminDepth));
    write_address(regs, kAsq, asq_phys);
    write_address(regs, kAcq, acq_phys);
    memory_fence();
    write32(regs, kCc, kCcEnable | (kCcIosqes << kCcIosqesShift) |
                           (kCcIocqes << kCcIocqesShift));
    if (!wait_bits(regs, kCsts, kCstsReady, kCstsReady, spins)) {
        log_controller(address, "enable timed out");
        return 0;
    }
    if ((read32(regs, kCsts) & kCstsFatal) != 0) {
        log_controller(address, "fatal status after enable");
        return 0;
    }

    NvmeQueue admin{};
    init_queue(admin, regs, dstrd, 0, kNvmeAdminDepth, asq, acq, spins);
    u32 status = 0;
    NvmeResult result =
        run_command(admin,
                    nvme_build_identify(next_cid(admin), kNvmeCnsController, 0, identify_phys),
                    status);
    if (result != NvmeResult::Ok) {
        log_controller(address, result == NvmeResult::Timeout ? "IDENTIFY timed out"
                                                              : "IDENTIFY failed");
        return 0;
    }
    char model[kNvmeControllerModelBytes + 1];
    nvme_model_string(identify, model, sizeof(model));
    const bool flush = nvme_write_cache_present(identify);

    result = run_command(
        admin, nvme_build_identify(next_cid(admin), kNvmeCnsActiveNamespaces, 0, list_phys),
        status);
    if (result != NvmeResult::Ok) {
        log_controller(address, result == NvmeResult::Timeout ? "namespace list timed out"
                                                              : "namespace list failed");
        return 0;
    }
    const u32 active = count_namespaces(list);
    log_controller(address, model, active);

    NvmeCandidate candidates[kMaxNvmeDisks]{};
    u32 wanted = max;
    const u32 free_contexts = kMaxNvmeDisks - disk_context_count;
    if (wanted > free_contexts) {
        wanted = free_contexts;
    }
    if (wanted > active) {
        wanted = active;
    }
    u32 candidate_count = 0;
    for (u32 i = 0; i < active && candidate_count < wanted; ++i) {
        const u32 nsid = nvme_read_u32(list, i * 4);
        result = run_command(admin,
                             nvme_build_identify(next_cid(admin), kNvmeCnsNamespace, nsid,
                                                 identify_phys),
                             status);
        if (result != NvmeResult::Ok) {
            log_identify_failure(nsid, result);
            if (result == NvmeResult::Timeout || admin.broken) {
                break;
            }
            continue;
        }
        const NvmeNamespaceInfo info = nvme_parse_namespace(identify);
        switch (info.problem) {
        case NvmeNamespaceProblem::None:
            candidates[candidate_count++] = NvmeCandidate{nsid, info};
            break;
        case NvmeNamespaceProblem::NoSectors:
            log_namespace_skip(nsid, "no sectors, skipped");
            break;
        case NvmeNamespaceProblem::Metadata:
            log_namespace_skip(nsid, "metadata, skipped");
            break;
        case NvmeNamespaceProblem::SectorSize:
            log_unsupported_sectors(nsid, info.sector_bytes);
            break;
        }
    }
    if (candidate_count == 0) {
        return 0;
    }

    const u32 depth = nvme_io_depth(nvme_cap_mqes(cap));
    const u64 sq_phys = pages.alloc_pages(1);
    const u64 cq_phys = sq_phys != 0 ? pages.alloc_pages(1) : 0;
    NvmeCommand* sq =
        sq_phys != 0 ? reinterpret_cast<NvmeCommand*>(pages.to_ptr(sq_phys)) : nullptr;
    NvmeCompletion* cq =
        cq_phys != 0 ? reinterpret_cast<NvmeCompletion*>(pages.to_ptr(cq_phys)) : nullptr;
    u64 bounce_phys[kMaxNvmeDisks]{};
    u32 bounce_count = 0;
    bool bounce_ok = true;
    while (bounce_count < candidate_count) {
        const u64 phys = pages.alloc_pages(1);
        bounce_phys[bounce_count] = phys;
        ++bounce_count;
        if (phys == 0 || pages.to_ptr(phys) == nullptr) {
            bounce_ok = false;
            break;
        }
    }
    if (sq == nullptr || cq == nullptr || !bounce_ok) {
        if (sq_phys != 0) {
            pages.free_pages(sq_phys, 1);
        }
        if (cq_phys != 0) {
            pages.free_pages(cq_phys, 1);
        }
        for (u32 i = 0; i < bounce_count; ++i) {
            pages.free_pages(bounce_phys[i], 1);
        }
        log_controller(address, "no memory");
        return 0;
    }

    NvmeQueue& queue = queue_contexts[queue_context_count];
    init_queue(queue, regs, dstrd, 1, depth, sq, cq, spins);
    result = run_command(admin,
                         nvme_build_create_cq(next_cid(admin), 1, depth, cq_phys), status);
    if (result != NvmeResult::Ok) {
        log_controller(address, result == NvmeResult::Timeout ? "I/O completion queue timed out"
                                                              : "I/O completion queue failed");
        return 0;
    }
    result = run_command(admin,
                         nvme_build_create_sq(next_cid(admin), 1, 1, depth, sq_phys), status);
    if (result != NvmeResult::Ok) {
        log_controller(address, result == NvmeResult::Timeout ? "I/O submission queue timed out"
                                                              : "I/O submission queue failed");
        return 0;
    }

    ++queue_context_count;
    u32 opened = 0;
    for (u32 i = 0; i < candidate_count; ++i) {
        NvmeDisk& disk = disk_contexts[disk_context_count];
        disk = NvmeDisk{};
        disk.queue = &queue;
        disk.bounce = pages.to_ptr(bounce_phys[i]);
        disk.bounce_phys = bounce_phys[i];
        disk.sectors =
            nvme_disk_sectors(candidates[i].info.sectors, candidates[i].info.sector_bytes);
        disk.sector_bytes = candidates[i].info.sector_bytes;
        disk.nsid = candidates[i].nsid;
        disk.flush = flush;
        log_namespace(disk.nsid, candidates[i].info, flush);
        out[opened].dev = SectorDevice{&disk, read_sector, write_sector, flush_disk};
        out[opened].sectors = disk.sectors;
        out[opened].read_only = false;
        out[opened].kind = "nvme";
        ++disk_context_count;
        ++opened;
    }
    return opened;
}

struct OpenState {
    MmioMapper* mmio;
    PhysAllocator* pages;
    Disk* out;
    u32 max;
    u32 opened;
};

bool open_controller_at(void* ctx, PciAddress address, u16, u16) {
    if ((pci_read32(address, kPciClassOffset) >> kClassShift) != kNvmeClassCode) {
        return false;
    }
    OpenState& state = *static_cast<OpenState*>(ctx);
    u32 room = state.max - state.opened;
    const u32 free_contexts = kMaxNvmeDisks - disk_context_count;
    if (room > free_contexts) {
        room = free_contexts;
    }
    if (room != 0) {
        state.opened +=
            open_controller(address, *state.mmio, *state.pages, state.out + state.opened, room);
    }
    return state.opened >= state.max || disk_context_count >= kMaxNvmeDisks;
}

}

u32 nvme_open_disks(MmioMapper& mmio, PhysAllocator& pages, Disk* out, u32 max) {
    if (out == nullptr || max == 0) {
        return 0;
    }
    OpenState state{&mmio, &pages, out, max, 0};
    pci_scan(&open_controller_at, &state);
    return state.opened;
}

}
