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

#include "apic.hpp"

#include "../common/apic_calc.hpp"
#include "../common/port_io.hpp"
#include "../common/serial.hpp"
#include "clock.hpp"
#include "percpu.hpp"

namespace hombre {

namespace {

enum class ApicMode {
    None,
    XApic,
    X2Apic,
};

// 8259 PIC ports and initialization commands.
constexpr u16 kPic1Cmd = 0x20;
constexpr u16 kPic1Data = 0x21;
constexpr u16 kPic2Cmd = 0xA0;
constexpr u16 kPic2Data = 0xA1;
constexpr u8 kPicIcw1Init = 0x11;
constexpr u8 kPicIcw4_8086 = 0x01;
constexpr u8 kPic1Offset = 0x20;
constexpr u8 kPic2Offset = 0x28;

// IA32_APIC_BASE MSR and its bits.
constexpr u32 kMsrApicBase = 0x1B;
constexpr u64 kApicBaseX2Apic = 1ull << 10;
constexpr u64 kApicBaseEnable = 1ull << 11;
constexpr u64 kApicBaseAddrMask = 0x000FFFFFFFFFF000ull;
constexpr u64 kApicPageBytes = 4096;

// xAPIC register byte offsets (x2APIC MSR = 0x800 + (reg >> 4)).
constexpr u32 kRegId = 0x020;
constexpr u32 kRegTpr = 0x080;
constexpr u32 kRegEoi = 0x0B0;
constexpr u32 kRegSvr = 0x0F0;
constexpr u32 kRegEsr = 0x280;
constexpr u32 kRegLvtTimer = 0x320;
constexpr u32 kRegLvtLint0 = 0x350;
constexpr u32 kRegLvtLint1 = 0x360;
constexpr u32 kRegLvtError = 0x370;
constexpr u32 kRegTimerInitial = 0x380;
constexpr u32 kRegTimerCurrent = 0x390;
constexpr u32 kRegTimerDivide = 0x3E0;

constexpr u32 kMsrX2ApicBase = 0x800;

constexpr u32 kSvrEnable = 1u << 8;
constexpr u32 kLvtMask = 1u << 16;
constexpr u32 kLvtTimerPeriodic = 1u << 17;
constexpr u32 kTimerDivide16 = 0x3;

constexpr u64 kCalibrationUs = 10000;

constinit ApicMode mode = ApicMode::None;
constinit volatile u32* mmio_base = nullptr;
constinit bool timer_running = false;
constinit u64 ticks = 0;
constinit bool need_resched = false;

void io_wait() {
    outb(0x80, 0);
}

u64 rdmsr(u32 msr) {
    u32 lo = 0;
    u32 hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<u64>(hi) << 32) | lo;
}

void wrmsr(u32 msr, u64 value) {
    asm volatile("wrmsr"
                 :
                 : "c"(msr), "a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32))
                 : "memory");
}

void cpuid(u32 leaf, u32 subleaf, u32& eax, u32& ebx, u32& ecx, u32& edx) {
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(leaf), "c"(subleaf));
}

u32 read_reg(u32 reg) {
    if (mode == ApicMode::X2Apic) {
        return static_cast<u32>(rdmsr(kMsrX2ApicBase + (reg >> 4)));
    }
    return mmio_base[reg / sizeof(u32)];
}

void write_reg(u32 reg, u32 value) {
    if (mode == ApicMode::X2Apic) {
        wrmsr(kMsrX2ApicBase + (reg >> 4), value);
        return;
    }
    mmio_base[reg / sizeof(u32)] = value;
}

void send_eoi() {
    if (mode != ApicMode::None) {
        write_reg(kRegEoi, 0);
    }
}

}  // namespace

void pic_mask_all() {
    outb(kPic1Cmd, kPicIcw1Init);
    io_wait();
    outb(kPic2Cmd, kPicIcw1Init);
    io_wait();

    outb(kPic1Data, kPic1Offset);
    io_wait();
    outb(kPic2Data, kPic2Offset);
    io_wait();

    // Master has slave on IRQ2 (bit 2); slave cascade identity is 2.
    outb(kPic1Data, 0x04);
    io_wait();
    outb(kPic2Data, 0x02);
    io_wait();

    outb(kPic1Data, kPicIcw4_8086);
    io_wait();
    outb(kPic2Data, kPicIcw4_8086);
    io_wait();

    outb(kPic1Data, 0xFF);
    io_wait();
    outb(kPic2Data, 0xFF);
    io_wait();
}

bool apic_init(MmioMapper& mmio) {
    mode = ApicMode::None;
    mmio_base = nullptr;
    timer_running = false;
    ticks = 0;
    need_resched = false;

    u32 eax = 0;
    u32 ebx = 0;
    u32 ecx = 0;
    u32 edx = 0;
    cpuid(0, 0, eax, ebx, ecx, edx);
    if (eax < 1) {
        serial::write("apic: none\n");
        return false;
    }
    cpuid(1, 0, eax, ebx, ecx, edx);
    const bool has_apic = (edx & (1u << 9)) != 0;
    if (!has_apic) {
        serial::write("apic: none\n");
        return false;
    }
    const bool has_x2apic = (ecx & (1u << 21)) != 0;

    u64 apic_base = rdmsr(kMsrApicBase);
    if ((apic_base & kApicBaseEnable) == 0) {
        apic_base |= kApicBaseEnable;
        wrmsr(kMsrApicBase, apic_base);
    }

    if (has_x2apic) {
        if ((apic_base & kApicBaseX2Apic) == 0) {
            apic_base |= kApicBaseX2Apic;
            wrmsr(kMsrApicBase, apic_base);
        }
        mode = ApicMode::X2Apic;
        serial::write("apic: x2apic\n");
    } else {
        const u64 phys_base = apic_base & kApicBaseAddrMask;
        if (phys_base == 0) {
            serial::write("apic: none\n");
            return false;
        }
        volatile void* mapped = mmio.map(phys_base, kApicPageBytes);
        if (mapped == nullptr) {
            serial::write("apic: none\n");
            return false;
        }
        mmio_base = static_cast<volatile u32*>(mapped);
        mode = ApicMode::XApic;
        serial::write("apic: xapic\n");
    }

    write_reg(kRegSvr, kSvrEnable | kApicSpuriousVector);
    write_reg(kRegTpr, 0);
    write_reg(kRegLvtLint0, read_reg(kRegLvtLint0) | kLvtMask);
    write_reg(kRegLvtLint1, read_reg(kRegLvtLint1) | kLvtMask);
    write_reg(kRegLvtError, read_reg(kRegLvtError) | kLvtMask);
    write_reg(kRegEsr, 0);

    this_cpu().apic_id = mode == ApicMode::X2Apic ? read_reg(kRegId) : (read_reg(kRegId) >> 24);

    // Measure the LAPIC counter decrease over a TSC-timed interval with the
    // timer masked in one-shot mode.
    write_reg(kRegTimerDivide, kTimerDivide16);
    write_reg(kRegLvtTimer, kLvtMask);
    const u64 start_us = clock_us();
    write_reg(kRegTimerInitial, 0xFFFFFFFFu);
    delay_us(kCalibrationUs);
    const u32 remaining = read_reg(kRegTimerCurrent);
    const u64 end_us = clock_us();
    write_reg(kRegTimerInitial, 0);

    const u64 elapsed_us = end_us - start_us;
    if (remaining == 0 || remaining >= 0xFFFFFFFFu || elapsed_us == 0) {
        serial::write("apic: timer calibration failed\n");
        return false;
    }

    const u64 elapsed_ticks = 0xFFFFFFFFu - remaining;
    const u32 initial_count = apic_timer_initial_count(elapsed_ticks, elapsed_us, kApicTimerHz);
    if (initial_count == 0) {
        serial::write("apic: timer calibration failed\n");
        return false;
    }

    write_reg(kRegTimerDivide, kTimerDivide16);
    write_reg(kRegLvtTimer, kLvtTimerPeriodic | kApicTimerVector);
    write_reg(kRegTimerInitial, initial_count);
    timer_running = true;
    serial::write("apic: timer 1000 Hz\n");
    return true;
}

bool apic_active() {
    return timer_running;
}

void apic_handle_timer_irq() {
    ++ticks;
    if ((ticks % kApicReschedTicks) == 0) {
        need_resched = true;
    }
    send_eoi();
}

u64 timer_ticks() {
    asm volatile("" : : "m"(ticks));
    return ticks;
}

bool take_need_resched() {
    if (!__atomic_load_n(&need_resched, __ATOMIC_RELAXED)) {
        return false;
    }
    const u64 flags = irq_save();
    const bool pending = need_resched;
    need_resched = false;
    irq_restore(flags);
    return pending;
}

void idle_wait() {
    if (!timer_running) {
        asm volatile("pause");
        return;
    }
    asm volatile("sti; hlt" : : : "memory");
}

}
