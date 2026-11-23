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

#include "ahci.hpp"

#include "../common/printable.hpp"
#include "../common/serial.hpp"
#include "pci.hpp"

namespace hombre {

namespace {

constexpr u8 kPciClassOffset = 0x08;
constexpr u8 kPciBar5Offset = 0x24;
constexpr u32 kPciCommandInterruptDisable = 1u << 10;
constexpr u32 kClassShift = 8;

// Generic host control registers, at the start of ABAR.
constexpr u32 kCap = 0x00;
constexpr u32 kGhc = 0x04;
constexpr u32 kPi = 0x0C;
constexpr u32 kCap2 = 0x24;
constexpr u32 kBohc = 0x28;
constexpr u32 kPortBase = 0x100;
constexpr u32 kPortBytes = 0x80;
constexpr u32 kPortCount = 32;
constexpr u32 kRegisterWindow = kPortBase + kPortCount * kPortBytes;

// Port registers.
constexpr u32 kPortClb = 0x00;
constexpr u32 kPortFb = 0x08;
constexpr u32 kPortIs = 0x10;
constexpr u32 kPortIe = 0x14;
constexpr u32 kPortCmd = 0x18;
constexpr u32 kPortTfd = 0x20;
constexpr u32 kPortSig = 0x24;
constexpr u32 kPortSsts = 0x28;
constexpr u32 kPortSerr = 0x30;
constexpr u32 kPortCi = 0x38;

constexpr u32 kCapPortsMask = 0x1Fu;
constexpr u32 kCap64Bit = 1u << 31;
constexpr u32 kCap2Boh = 1u << 0;
constexpr u32 kBohcBiosOwned = 1u << 0;
constexpr u32 kBohcOsOwned = 1u << 1;
constexpr u32 kBohcBiosBusy = 1u << 2;
constexpr u32 kGhcInterruptEnable = 1u << 1;
constexpr u32 kGhcAhciEnable = 1u << 31;
constexpr u32 kCmdStart = 1u << 0;
constexpr u32 kCmdPowerOn = 1u << 1;
constexpr u32 kCmdFisReceive = 1u << 4;
constexpr u32 kCmdFisReceiveRunning = 1u << 14;
constexpr u32 kCmdListRunning = 1u << 15;
constexpr u32 kIsTaskFileError = 1u << 28;
constexpr u32 kTfdError = 1u << 0;
constexpr u32 kTfdBusy = 1u << 7;
constexpr u32 kSstsDetMask = 0x0Fu;
constexpr u32 kSstsDetPresent = 1;
constexpr u32 kSstsDetPhy = 2;
constexpr u32 kSstsDetReady = 3;
constexpr u32 kSstsDetOffline = 4;

// How long to spin on the controller. One pause takes roughly 10 to 150
// cycles depending on the CPU, so 2^29 rounds is about 2 to 25 seconds at
// 3 GHz: far beyond any request a working disk needs, short enough that a
// dead one does not hang the boot for good. The port waits are shorter, as
// the specifications bound them in time.
constexpr u64 kPollLimit = 1ull << 29;
constexpr u64 kPortLimit = 1ull << 28;
constexpr u64 kHandoffLimit = 1ull << 29;
constexpr u64 kDetectLimit = 1ull << 26;

constexpr u64 kFourGiB = 1ull << 32;
constexpr u32 kMaxAhciDisks = 8;

void cpu_pause() {
    asm volatile("pause");
}

// Orders the command and the data it names before the command issue write.
void memory_fence() {
    asm volatile("mfence" : : : "memory");
}

u32 read32(const volatile u8* base, u32 offset) {
    return *reinterpret_cast<const volatile u32*>(base + offset);
}

void write32(volatile u8* base, u32 offset, u32 value) {
    *reinterpret_cast<volatile u32*>(base + offset) = value;
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
    serial::write("ahci: ");
    serial::write_hex(address.bus);
    serial::write_char(':');
    serial::write_hex(address.device);
    serial::write_char('.');
    serial::write_hex(address.function);
}

void log_controller(PciAddress address, u32 ports, bool wide) {
    write_controller_prefix(address);
    serial::write_char(' ');
    serial::write_dec(ports);
    serial::write(wide ? " ports, 64-bit\n" : " ports, 32-bit\n");
}

void log_controller(PciAddress address, const char* reason) {
    write_controller_prefix(address);
    serial::write(": ");
    serial::write(reason);
    serial::write_char('\n');
}

void log_port(u32 port, const char* reason) {
    serial::write("ahci: port ");
    serial::write_dec(port);
    serial::write(": ");
    serial::write(reason);
    serial::write_char('\n');
}

void log_unknown_signature(u32 port, u32 signature) {
    serial::write("ahci: port ");
    serial::write_dec(port);
    serial::write(": signature 0x");
    serial::write_hex(signature);
    serial::write(", skipped\n");
}

void log_disk(u32 port, const AhciIdentify& info) {
    char model[sizeof(info.model)];
    sanitize_text(model, sizeof(model), info.model, sizeof(info.model) - 1);
    serial::write("ahci: port ");
    serial::write_dec(port);
    serial::write(": ");
    serial::write(model);
    serial::write(", ");
    serial::write_dec(info.sectors);
    serial::write(" sectors, ");
    serial::write_dec(info.sector_bytes);
    serial::write("-byte sectors");
    if (!info.flush) {
        serial::write(", no flush");
    }
    serial::write_char('\n');
}

struct AhciPortMemory {
    u8* page;
    u64 page_phys;
    u8* bounce;
    u64 bounce_phys;
};

struct AhciDisk {
    volatile u8* regs;
    AhciPortMemory memory;
    u64 sectors;
    u32 sector_bytes;
    bool flush;
};

constinit AhciDisk disk_contexts[kMaxAhciDisks]{};
constinit u32 disk_context_count = 0;

// Clears start, then the FIS receive engine, and waits for both to leave
// their running states before the port is given new memory.
bool stop_port(volatile u8* port) {
    u32 cmd = read32(port, kPortCmd);
    if ((cmd & kCmdStart) != 0) {
        write32(port, kPortCmd, cmd & ~kCmdStart);
    }
    if (!wait_bits(port, kPortCmd, kCmdListRunning, 0, kPortLimit)) {
        return false;
    }
    cmd = read32(port, kPortCmd);
    if ((cmd & kCmdFisReceive) != 0) {
        write32(port, kPortCmd, cmd & ~kCmdFisReceive);
    }
    if (!wait_bits(port, kPortCmd, kCmdFisReceiveRunning, 0, kPortLimit)) {
        return false;
    }
    write32(port, kPortCi, 0xFFFFFFFFu);
    write32(port, kPortIe, 0);
    write32(port, kPortIs, 0xFFFFFFFFu);
    write32(port, kPortSerr, 0xFFFFFFFFu);
    return true;
}

bool start_port(volatile u8* port, u64 page_phys) {
    write_address(port, kPortClb, page_phys + kAhciCmdListOffset);
    write_address(port, kPortFb, page_phys + kAhciFisOffset);
    const u32 cmd = read32(port, kPortCmd);
    write32(port, kPortCmd, (cmd & ~kCmdStart) | kCmdFisReceive | kCmdPowerOn);
    if (!wait_bits(port, kPortCmd, kCmdFisReceiveRunning, kCmdFisReceiveRunning, kPortLimit)) {
        return false;
    }
    write32(port, kPortCmd, read32(port, kPortCmd) | kCmdStart);
    return wait_bits(port, kPortCmd, kCmdListRunning, kCmdListRunning, kPortLimit);
}

// Stops and restarts the command engine without clearing the FIS receive
// area, which is where a timed out command is still allowed to write.
bool restart_port(volatile u8* port) {
    const u32 cmd = read32(port, kPortCmd);
    if ((cmd & kCmdStart) != 0) {
        write32(port, kPortCmd, cmd & ~kCmdStart);
    }
    if (!wait_bits(port, kPortCmd, kCmdListRunning, 0, kPortLimit)) {
        return false;
    }
    write32(port, kPortCi, 0xFFFFFFFFu);
    write32(port, kPortIs, 0xFFFFFFFFu);
    write32(port, kPortSerr, 0xFFFFFFFFu);
    write32(port, kPortCmd, read32(port, kPortCmd) | kCmdStart);
    return wait_bits(port, kPortCmd, kCmdListRunning, kCmdListRunning, kPortLimit);
}

bool run_command(AhciDisk& disk, u8 command, bool write, u64 lba, u16 count, u32 bytes) {
    volatile u8* port = disk.regs;
    u8* table = disk.memory.page + kAhciCmdTableOffset;
    __builtin_memset(table, 0, kAhciCmdTableBytes);
    ahci_build_h2d_fis(table, command, lba, count);
    const u32 prdt_count = ahci_build_prdt(
        reinterpret_cast<AhciPrdtEntry*>(table + kAhciPrdtOffset), disk.memory.bounce_phys, bytes);
    *reinterpret_cast<AhciCmdHeader*>(disk.memory.page + kAhciCmdListOffset) =
        ahci_build_cmd_header(disk.memory.page_phys + kAhciCmdTableOffset, write, prdt_count);

    write32(port, kPortIe, 0);
    write32(port, kPortIs, 0xFFFFFFFFu);
    write32(port, kPortSerr, 0xFFFFFFFFu);
    memory_fence();
    write32(port, kPortCi, 1u);

    for (u64 spins = 0; (read32(port, kPortCi) & 1u) != 0; ++spins) {
        if (spins == kPollLimit) {
            restart_port(port);
            return false;
        }
        cpu_pause();
    }
    // The command is complete: the data the device wrote is visible before
    // the caller reads the bounce page.
    memory_fence();
    return (read32(port, kPortIs) & kIsTaskFileError) == 0 &&
           (read32(port, kPortTfd) & (kTfdError | kTfdBusy)) == 0;
}

bool transfer(AhciDisk& disk, bool write, u64 lba) {
    return run_command(disk, write ? kAhciCmdWriteDmaExt : kAhciCmdReadDmaExt, write, lba, 1,
                       disk.sector_bytes);
}

bool read_sector(void* ctx, u64 sector, u8* out) {
    AhciDisk& disk = *static_cast<AhciDisk*>(ctx);
    if (sector >= disk.sectors) {
        return false;
    }
    const AhciSectorPlace place = ahci_sector_place(sector, disk.sector_bytes);
    if (!transfer(disk, false, place.logical)) {
        return false;
    }
    __builtin_memcpy(out, disk.memory.bounce + place.offset, kAhciSectorBytes);
    return true;
}

bool write_sector(void* ctx, u64 sector, const u8* data) {
    AhciDisk& disk = *static_cast<AhciDisk*>(ctx);
    if (sector >= disk.sectors) {
        return false;
    }
    const AhciSectorPlace place = ahci_sector_place(sector, disk.sector_bytes);
    if (disk.sector_bytes > kAhciSectorBytes && !transfer(disk, false, place.logical)) {
        return false;
    }
    __builtin_memcpy(disk.memory.bounce + place.offset, data, kAhciSectorBytes);
    return transfer(disk, true, place.logical);
}

bool flush_disk(void* ctx) {
    AhciDisk& disk = *static_cast<AhciDisk*>(ctx);
    return !disk.flush || run_command(disk, kAhciCmdFlushCacheExt, false, 0, 0, 0);
}

bool take_ownership(volatile u8* regs) {
    write32(regs, kBohc, read32(regs, kBohc) | kBohcOsOwned);
    return wait_bits(regs, kBohc, kBohcBiosOwned, 0, kHandoffLimit) &&
           wait_bits(regs, kBohc, kBohcBiosBusy, 0, kHandoffLimit);
}

void set_command(PciAddress address, u32 set, u32 clear) {
    const u32 command = pci_read32(address, kPciCommandOffset) & 0xFFFFu;
    pci_write32(address, kPciCommandOffset, (command & ~clear) | set);
}

volatile u8* map_registers(PciAddress address, MmioMapper& mmio) {
    const u32 bar_low = pci_read32(address, kPciBar5Offset);
    u32 bar_high = 0;
    if ((bar_low & 1u) == 0 && ((bar_low >> 1) & 3u) == 2) {
        bar_high = pci_read32(address, static_cast<u8>(kPciBar5Offset + 4));
    }
    const PciBar bar = pci_decode_bar(bar_low, bar_high);
    if (bar.is_io || bar.phys == 0) {
        return nullptr;
    }
    return static_cast<volatile u8*>(mmio.map(bar.phys, kRegisterWindow));
}

// Walks the implemented ports and collects those with an ATA disk behind
// them. Ports with another kind of device and ports whose PHY never comes
// up are logged and left alone.
u32 find_ata_ports(volatile u8* regs, u8* candidates) {
    const u32 implemented = read32(regs, kPi);
    u32 count = 0;
    for (u32 p = 0; p < kPortCount; ++p) {
        if ((implemented & (1u << p)) == 0) {
            continue;
        }
        volatile u8* port = regs + kPortBase + p * kPortBytes;
        u32 det = read32(port, kPortSsts) & kSstsDetMask;
        for (u64 spins = 0; (det == kSstsDetPresent || det == kSstsDetPhy) && spins < kDetectLimit;
             ++spins) {
            cpu_pause();
            det = read32(port, kPortSsts) & kSstsDetMask;
        }
        if (det != kSstsDetReady) {
            if (det == kSstsDetPresent || det == kSstsDetPhy) {
                log_port(p, "device not ready, skipped");
            } else if (det == kSstsDetOffline) {
                log_port(p, "PHY offline, skipped");
            }
            continue;
        }
        const u32 signature = read32(port, kPortSig);
        switch (ahci_classify_signature(signature)) {
        case AhciSignature::Ata:
            candidates[count] = static_cast<u8>(p);
            ++count;
            break;
        case AhciSignature::Atapi:
            log_port(p, "ATAPI, skipped");
            break;
        case AhciSignature::PortMultiplier:
            log_port(p, "port multiplier, skipped");
            break;
        case AhciSignature::Enclosure:
            log_port(p, "enclosure bridge, skipped");
            break;
        case AhciSignature::Other:
            log_unknown_signature(p, signature);
            break;
        }
    }
    return count;
}

// Allocates one page for the command list, FIS area and command table, and
// one bounce page, for each of `count` ports. Returns false, with every page
// it took given back, when a page cannot be had or a 32-bit controller gets
// one above 4 GiB.
bool alloc_port_memory(PhysAllocator& pages, bool wide, u32 count, AhciPortMemory* out,
                       const char*& error) {
    u32 allocated = 0;
    error = "no memory";
    for (u32 i = 0; i < count; ++i) {
        const u64 page_phys = pages.alloc_pages(1);
        const u64 bounce_phys = page_phys != 0 ? pages.alloc_pages(1) : 0;
        if (page_phys == 0 || bounce_phys == 0) {
            if (page_phys != 0) {
                pages.free_pages(page_phys, 1);
            }
            break;
        }
        if (!wide && (page_phys >= kFourGiB || bounce_phys >= kFourGiB)) {
            pages.free_pages(page_phys, 1);
            pages.free_pages(bounce_phys, 1);
            error = "needs memory below 4 GiB";
            break;
        }
        u8* page = pages.to_ptr(page_phys);
        u8* bounce = pages.to_ptr(bounce_phys);
        if (page == nullptr || bounce == nullptr) {
            pages.free_pages(page_phys, 1);
            pages.free_pages(bounce_phys, 1);
            break;
        }
        out[i] = AhciPortMemory{page, page_phys, bounce, bounce_phys};
        allocated = i + 1;
    }
    if (allocated == count) {
        return true;
    }
    for (u32 i = 0; i < allocated; ++i) {
        pages.free_pages(out[i].page_phys, 1);
        pages.free_pages(out[i].bounce_phys, 1);
    }
    return false;
}

void log_unsupported_sectors(u32 port, u32 sector_bytes) {
    serial::write("ahci: port ");
    serial::write_dec(port);
    serial::write(": ");
    serial::write_dec(sector_bytes);
    serial::write("-byte sectors not supported, skipped\n");
}

u32 open_controller(PciAddress address, MmioMapper& mmio, PhysAllocator& pages, Disk* out,
                    u32 max) {
    // Memory decoding on so the registers can be reached; bus mastering stays
    // off until there is memory to give the controller.
    set_command(address, kPciCommandMemorySpace | kPciCommandInterruptDisable,
                kPciCommandBusMaster);
    volatile u8* regs = map_registers(address, mmio);
    if (regs == nullptr) {
        log_controller(address, "cannot map its memory BAR");
        return 0;
    }

    const u32 cap = read32(regs, kCap);
    const bool wide = (cap & kCap64Bit) != 0;
    log_controller(address, (cap & kCapPortsMask) + 1, wide);

    if ((read32(regs, kCap2) & kCap2Boh) != 0 && !take_ownership(regs)) {
        log_controller(address, "BIOS did not hand over the controller");
        return 0;
    }
    write32(regs, kGhc, (read32(regs, kGhc) | kGhcAhciEnable) & ~kGhcInterruptEnable);

    u8 candidates[kPortCount]{};
    const u32 found = find_ata_ports(regs, candidates);
    const u32 wanted = found < max ? found : max;
    if (wanted == 0) {
        return 0;
    }

    AhciPortMemory memory[kPortCount]{};
    const char* error = nullptr;
    if (!alloc_port_memory(pages, wide, wanted, memory, error)) {
        log_controller(address, error);
        return 0;
    }

    set_command(address, kPciCommandBusMaster, 0);

    u32 opened = 0;
    for (u32 i = 0; i < wanted; ++i) {
        const u32 p = candidates[i];
        volatile u8* port = regs + kPortBase + p * kPortBytes;
        if (!stop_port(port)) {
            log_port(p, "stop failed");
            continue;
        }
        if (!start_port(port, memory[i].page_phys)) {
            log_port(p, "start failed");
            continue;
        }
        AhciDisk disk{};
        disk.regs = port;
        disk.memory = memory[i];
        if (!run_command(disk, kAhciCmdIdentify, false, 0, 0, kAhciSectorBytes)) {
            log_port(p, "IDENTIFY failed");
            continue;
        }
        AhciIdentify info{};
        if (!ahci_parse_identify(reinterpret_cast<const u16*>(disk.memory.bounce), info)) {
            if (info.sectors == 0) {
                log_port(p, "no sectors, skipped");
            } else {
                log_unsupported_sectors(p, info.sector_bytes);
            }
            continue;
        }
        disk.sector_bytes = info.sector_bytes;
        disk.sectors = ahci_disk_sectors(info.sectors, info.sector_bytes);
        disk.flush = info.flush;
        log_disk(p, info);
        disk_contexts[disk_context_count] = disk;
        out[opened].dev =
            SectorDevice{&disk_contexts[disk_context_count], read_sector, write_sector, flush_disk};
        out[opened].sectors = disk.sectors;
        out[opened].read_only = false;
        out[opened].kind = "ahci";
        ++disk_context_count;
        ++opened;
    }
    if (opened == 0) {
        set_command(address, 0, kPciCommandBusMaster);
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
    if ((pci_read32(address, kPciClassOffset) >> kClassShift) != kAhciClassCode) {
        return false;
    }
    OpenState& state = *static_cast<OpenState*>(ctx);
    u32 room = state.max - state.opened;
    const u32 free_contexts = kMaxAhciDisks - disk_context_count;
    if (room > free_contexts) {
        room = free_contexts;
    }
    if (room != 0) {
        state.opened +=
            open_controller(address, *state.mmio, *state.pages, state.out + state.opened, room);
    }
    return state.opened >= state.max || disk_context_count >= kMaxAhciDisks;
}

}

u32 ahci_open_disks(MmioMapper& mmio, PhysAllocator& pages, Disk* out, u32 max) {
    if (out == nullptr || max == 0) {
        return 0;
    }
    OpenState state{&mmio, &pages, out, max, 0};
    pci_scan(&open_controller_at, &state);
    return state.opened;
}

}
