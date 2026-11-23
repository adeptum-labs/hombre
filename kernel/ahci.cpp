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
#include "clock.hpp"
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
constexpr u32 kPortSctl = 0x2C;
constexpr u32 kPortSerr = 0x30;
constexpr u32 kPortCi = 0x38;

constexpr u32 kCapPortsMask = 0x1Fu;
constexpr u32 kCap64Bit = 1u << 31;
constexpr u32 kCapStaggeredSpinUp = 1u << 27;
constexpr u32 kCap2Boh = 1u << 0;
constexpr u32 kBohcBiosOwned = 1u << 0;
constexpr u32 kBohcOsOwned = 1u << 1;
constexpr u32 kBohcBiosBusy = 1u << 2;
constexpr u32 kGhcInterruptEnable = 1u << 1;
constexpr u32 kGhcAhciEnable = 1u << 31;
constexpr u32 kCmdStart = 1u << 0;
constexpr u32 kCmdSpinUp = 1u << 1;
constexpr u32 kCmdPowerOn = 1u << 2;
constexpr u32 kCmdFisReceive = 1u << 4;
constexpr u32 kCmdFisReceiveRunning = 1u << 14;
constexpr u32 kCmdListRunning = 1u << 15;
// PxIS bits that end a command with an error: task file, host bus fatal,
// host bus data and interface fatal errors.
constexpr u32 kIsCommandErrors = (1u << 30) | (1u << 29) | (1u << 28) | (1u << 27);
constexpr u32 kTfdError = 1u << 0;
constexpr u32 kTfdDrq = 1u << 3;
constexpr u32 kTfdBusy = 1u << 7;
constexpr u32 kSstsDetMask = 0x0Fu;
constexpr u32 kSstsDetPresent = 1;
constexpr u32 kSstsDetPhy = 2;
constexpr u32 kSstsDetReady = 3;
constexpr u32 kSstsDetOffline = 4;
constexpr u32 kSctlDetMask = 0x0Fu;
constexpr u32 kSctlDetReset = 1;
// Interface power management: partial and slumber states not allowed.
constexpr u32 kSctlIpmMask = 0x0Fu << 8;
constexpr u32 kSctlIpmNone = 3u << 8;
// One FIS receive area per port while the ports are probed.
constexpr u32 kProbeFisPages = kPortCount * kAhciFisReceiveBytes / kAhciPageBytes;

// How long to wait on the controller and the disks, in milliseconds.
constexpr u32 kCommandMs = 30000;  // a disk may spin up or empty a large cache
constexpr u32 kPortMs = 500;       // PxCMD.CR and FR, bounded by the specification
constexpr u32 kHandoffMs = 3000;   // the BIOS may stay busy for 2 s
constexpr u32 kResetHoldMs = 2;    // COMRESET is held for at least 1 ms
constexpr u32 kPresenceMs = 20;    // a device answers a COMRESET within 10 ms
constexpr u32 kLinkMs = 1000;      // PHY communication once a device is present
constexpr u32 kReadyMs = 10000;    // a hard disk clears BSY once spun up
// Without a calibrated clock a millisecond is counted as 2^16 pauses, which
// take 0.2 to 3 ms depending on the CPU.
constexpr u64 kSpinsPerMs = 1ull << 16;

constexpr u64 kFourGiB = 1ull << 32;
constexpr u32 kMaxAhciDisks = 8;

void cpu_pause() {
    asm volatile("pause");
}

// A wait bounded in time once the clock is calibrated, and by a pause count
// that stands in for the time before.
class Wait {
public:
    explicit Wait(u32 ms)
        : timed_(clock_us() != 0), end_(deadline_in_us(u64{ms} * 1000)),
          limit_(u64{ms} * kSpinsPerMs) {}

    // True once the time is up; pauses once otherwise.
    bool over() {
        if (timed_ ? expired(end_) : spins_ >= limit_) {
            return true;
        }
        ++spins_;
        cpu_pause();
        return false;
    }

private:
    bool timed_;
    Deadline end_;
    u64 limit_;
    u64 spins_ = 0;
};

void hold_ms(u32 ms) {
    Wait wait(ms);
    while (!wait.over()) {
    }
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

bool wait_bits(const volatile u8* base, u32 offset, u32 mask, u32 value, u32 ms) {
    Wait wait(ms);
    while ((read32(base, offset) & mask) != value) {
        if (wait.over()) {
            return (read32(base, offset) & mask) == value;
        }
    }
    return true;
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
    // Set after a command timed out: the device may still hold it and write
    // to the bounce page, so the disk takes no further commands.
    bool failed;
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
    if (!wait_bits(port, kPortCmd, kCmdListRunning, 0, kPortMs)) {
        return false;
    }
    cmd = read32(port, kPortCmd);
    if ((cmd & kCmdFisReceive) != 0) {
        write32(port, kPortCmd, cmd & ~kCmdFisReceive);
    }
    if (!wait_bits(port, kPortCmd, kCmdFisReceiveRunning, 0, kPortMs)) {
        return false;
    }
    write32(port, kPortIe, 0);
    write32(port, kPortIs, 0xFFFFFFFFu);
    write32(port, kPortSerr, 0xFFFFFFFFu);
    return true;
}

bool start_port(volatile u8* port, u64 page_phys) {
    write_address(port, kPortClb, page_phys + kAhciCmdListOffset);
    write_address(port, kPortFb, page_phys + kAhciFisOffset);
    const u32 cmd = read32(port, kPortCmd);
    write32(port, kPortCmd, (cmd & ~kCmdStart) | kCmdFisReceive | kCmdSpinUp | kCmdPowerOn);
    if (!wait_bits(port, kPortCmd, kCmdFisReceiveRunning, kCmdFisReceiveRunning, kPortMs)) {
        return false;
    }
    write32(port, kPortCmd, read32(port, kPortCmd) | kCmdStart);
    return wait_bits(port, kPortCmd, kCmdListRunning, kCmdListRunning, kPortMs);
}

// Stops and restarts the command engine without clearing the FIS receive
// area, which is where a timed out command is still allowed to write.
bool restart_port(volatile u8* port) {
    const u32 cmd = read32(port, kPortCmd);
    if ((cmd & kCmdStart) != 0) {
        write32(port, kPortCmd, cmd & ~kCmdStart);
    }
    if (!wait_bits(port, kPortCmd, kCmdListRunning, 0, kPortMs)) {
        return false;
    }
    // PxCI is cleared by the controller when start goes off; writing it
    // would only set bits and issue stale commands.
    write32(port, kPortIs, 0xFFFFFFFFu);
    write32(port, kPortSerr, 0xFFFFFFFFu);
    write32(port, kPortCmd, read32(port, kPortCmd) | kCmdStart);
    return wait_bits(port, kPortCmd, kCmdListRunning, kCmdListRunning, kPortMs);
}

bool run_command(AhciDisk& disk, u8 command, bool write, u64 lba, u16 count, u32 bytes) {
    if (disk.failed) {
        return false;
    }
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

    // CI stays set when the command fails, so the error bits end the wait
    // too. An error leaves the port restarted for the next command; a
    // timeout leaves the disk unused.
    Wait wait(kCommandMs);
    while ((read32(port, kPortCi) & 1u) != 0) {
        if ((read32(port, kPortIs) & kIsCommandErrors) != 0) {
            restart_port(port);
            return false;
        }
        if (wait.over() && (read32(port, kPortCi) & 1u) != 0) {
            disk.failed = true;
            serial::write("ahci: command timed out, disk no longer used\n");
            restart_port(port);
            return false;
        }
    }
    // The command is complete: the data the device wrote is visible before
    // the caller reads the bounce page.
    memory_fence();
    return (read32(port, kPortIs) & kIsCommandErrors) == 0 &&
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
    return wait_bits(regs, kBohc, kBohcBiosOwned, 0, kHandoffMs) &&
           wait_bits(regs, kBohc, kBohcBiosBusy, 0, kHandoffMs);
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

u32 link_state(const volatile u8* port) {
    return read32(port, kPortSsts) & kSstsDetMask;
}

// Waits while a device is present but its PHY link is not up yet.
u32 wait_link(const volatile u8* port, u32 ms) {
    Wait wait(ms);
    u32 det = link_state(port);
    while ((det == kSstsDetPresent || det == kSstsDetPhy) && !wait.over()) {
        det = link_state(port);
    }
    return det;
}

// Sends a COMRESET on a stopped port, as for a disk the firmware did not
// bring up: DET 1 held for over a millisecond, then 0, with low power
// states kept off. Returns the link state once a device answered and its
// link came up, or once none answered.
u32 reset_link(volatile u8* port) {
    const u32 sctl = read32(port, kPortSctl) & ~(kSctlDetMask | kSctlIpmMask);
    write32(port, kPortSctl, sctl | kSctlIpmNone | kSctlDetReset);
    hold_ms(kResetHoldMs);
    write32(port, kPortSctl, sctl | kSctlIpmNone);
    Wait presence(kPresenceMs);
    while (link_state(port) == 0 && !presence.over()) {
    }
    return wait_link(port, kLinkMs);
}

// Brings up the link of one port and reads what is behind it. Logs every
// device it skips; an empty port is skipped silently. The port is stopped
// with FIS receive on, so the device's first register FIS can arrive.
bool probe_port(volatile u8* port, u32 p) {
    u32 det = wait_link(port, kLinkMs);
    if (det == kSstsDetOffline) {
        log_port(p, "PHY offline, skipped");
        return false;
    }
    if (det != kSstsDetReady) {
        det = reset_link(port);
    }
    if (det != kSstsDetReady) {
        if (det != 0) {
            log_port(p, "device not ready, skipped");
        }
        return false;
    }
    write32(port, kPortSerr, 0xFFFFFFFFu);
    if (!wait_bits(port, kPortTfd, kTfdBusy | kTfdDrq, 0, kReadyMs)) {
        log_port(p, "device stays busy, skipped");
        return false;
    }
    const u32 signature = read32(port, kPortSig);
    switch (ahci_classify_signature(signature)) {
    case AhciSignature::Ata: return true;
    case AhciSignature::Atapi: log_port(p, "ATAPI, skipped"); break;
    case AhciSignature::PortMultiplier: log_port(p, "port multiplier, skipped"); break;
    case AhciSignature::Enclosure: log_port(p, "enclosure bridge, skipped"); break;
    case AhciSignature::Other: log_unknown_signature(p, signature); break;
    }
    return false;
}

// Stops every implemented port; false when one will not stop.
bool stop_ports(volatile u8* regs, u32 implemented) {
    for (u32 p = 0; p < kPortCount; ++p) {
        if ((implemented & (1u << p)) != 0 && !stop_port(regs + kPortBase + p * kPortBytes)) {
            log_port(p, "will not stop, controller not used");
            return false;
        }
    }
    return true;
}

// Points every implemented port's FIS receive area into `fis_phys` and
// turns FIS receive on; false when a port's receive engine does not start.
bool receive_fises(volatile u8* regs, u32 implemented, u64 fis_phys) {
    for (u32 p = 0; p < kPortCount; ++p) {
        if ((implemented & (1u << p)) == 0) {
            continue;
        }
        volatile u8* port = regs + kPortBase + p * kPortBytes;
        write_address(port, kPortFb, fis_phys + u64{p} * kAhciFisReceiveBytes);
        write32(port, kPortCmd, read32(port, kPortCmd) | kCmdFisReceive);
        if (!wait_bits(port, kPortCmd, kCmdFisReceiveRunning, kCmdFisReceiveRunning, kPortMs)) {
            log_port(p, "FIS receive will not start, controller not used");
            return false;
        }
    }
    return true;
}

// Probes the implemented ports and collects those with an ATA disk behind
// them. The ports must be stopped. Each gets a FIS receive area in a scratch
// area while it is probed, and is stopped again before that area goes back.
// Returns false, with bus mastering off and the scratch area kept, when a
// port does not obey; the controller is then not used.
bool find_ata_ports(PciAddress address, volatile u8* regs, u32 implemented, bool wide,
                    PhysAllocator& pages, u8* candidates, u32& count) {
    count = 0;
    const u64 fis_phys = pages.alloc_pages(kProbeFisPages);
    if (fis_phys == 0 || (!wide && fis_phys + kProbeFisPages * kAhciPageBytes > kFourGiB)) {
        if (fis_phys != 0) {
            pages.free_pages(fis_phys, kProbeFisPages);
        }
        log_controller(address, "no memory below 4 GiB to probe with");
        return false;
    }
    // The ports are stopped, so bus mastering can come on: the controller
    // fetches nothing, and FIS receive needs it to reach the scratch area.
    set_command(address, kPciCommandBusMaster, 0);
    if (!receive_fises(regs, implemented, fis_phys)) {
        stop_ports(regs, implemented);
        set_command(address, 0, kPciCommandBusMaster);
        return false;
    }
    for (u32 p = 0; p < kPortCount; ++p) {
        if ((implemented & (1u << p)) != 0 && probe_port(regs + kPortBase + p * kPortBytes, p)) {
            candidates[count] = static_cast<u8>(p);
            ++count;
        }
    }
    if (!stop_ports(regs, implemented)) {
        set_command(address, 0, kPciCommandBusMaster);
        return false;
    }
    pages.free_pages(fis_phys, kProbeFisPages);
    return true;
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

    // Every implemented port stops before bus mastering comes on: a port the
    // firmware left running would otherwise receive FISes into memory the
    // kernel has since handed out. A port that will not stop keeps bus
    // mastering off for the whole controller.
    const u32 implemented = read32(regs, kPi);
    if (!stop_ports(regs, implemented)) {
        return 0;
    }
    // With staggered spin-up the disks wait for their port to tell them.
    if ((cap & kCapStaggeredSpinUp) != 0) {
        for (u32 p = 0; p < kPortCount; ++p) {
            if ((implemented & (1u << p)) != 0) {
                volatile u8* port = regs + kPortBase + p * kPortBytes;
                write32(port, kPortCmd, read32(port, kPortCmd) | kCmdSpinUp | kCmdPowerOn);
            }
        }
    }

    u8 candidates[kPortCount]{};
    u32 found = 0;
    if (!find_ata_ports(address, regs, implemented, wide, pages, candidates, found)) {
        return 0;
    }
    const u32 wanted = found < max ? found : max;
    if (wanted == 0) {
        set_command(address, 0, kPciCommandBusMaster);
        return 0;
    }

    AhciPortMemory memory[kPortCount]{};
    const char* error = nullptr;
    if (!alloc_port_memory(pages, wide, wanted, memory, error)) {
        log_controller(address, error);
        set_command(address, 0, kPciCommandBusMaster);
        return 0;
    }

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
        out[opened].lba_bytes = disk.sector_bytes;
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
