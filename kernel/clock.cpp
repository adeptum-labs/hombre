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

#include "clock.hpp"

#include "../common/port_io.hpp"

namespace hombre {

namespace {

constexpr u16 kPitChannel2Port = 0x42;
constexpr u16 kPitCommandPort = 0x43;
// NMI status and control: bit 0 gates timer 2, bit 1 enables the speaker,
// bit 5 reads the timer 2 output.
constexpr u16 kPitStatusPort = 0x61;

constexpr u8 kPitGateOn = 1u << 0;
constexpr u8 kPitSpeakerOn = 1u << 1;
constexpr u8 kPitOutputHigh = 1u << 5;

// Channel 2, lobyte then hibyte, mode 0 (interrupt on terminal count).
constexpr u8 kPitOneShotCommand = 0xB0;

// 11932 counts of the 1193182 Hz input take a hair over 10 ms.
constexpr u16 kCalibrationTicks = 11932;

constexpr u64 kMinKhz = 100000;
constexpr u64 kMaxKhz = 10000000;

// A poll is a port read, slow enough to cover the 10 ms one-shot many times
// over while still ending on a timer that never fires.
constexpr u64 kOutputPollLimit = 1ull << 20;

constinit u64 tsc_khz = 0;
constinit u64 tsc_start = 0;

u64 rdtsc() {
    u32 low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<u64>(high) << 32) | low;
}

void set_pit_gate(bool open) {
    const u8 value = inb(kPitStatusPort);
    const u8 gate = open ? kPitGateOn : 0;
    outb(kPitStatusPort, static_cast<u8>((value & ~kPitSpeakerOn) | gate));
}

void start_pit_one_shot() {
    set_pit_gate(false);
    outb(kPitCommandPort, kPitOneShotCommand);
    outb(kPitChannel2Port, static_cast<u8>(kCalibrationTicks));
    outb(kPitChannel2Port, static_cast<u8>(kCalibrationTicks >> 8));
    set_pit_gate(true);
}

bool wait_for_pit_output() {
    for (u64 i = 0; i < kOutputPollLimit; ++i) {
        if ((inb(kPitStatusPort) & kPitOutputHigh) != 0) {
            return true;
        }
    }
    return false;
}

bool calibrate_once(u64& khz) {
    start_pit_one_shot();
    const u64 start = rdtsc();
    if (!wait_for_pit_output()) {
        return false;
    }
    khz = tsc_khz_from(kCalibrationTicks, rdtsc() - start);
    return true;
}

}

bool clock_init() {
    u64 khz = 0;
    // Each reading overshoots by up to one poll, so the lower one is closer.
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        u64 reading = 0;
        if (!calibrate_once(reading)) {
            return false;
        }
        khz = (attempt == 0 || reading < khz) ? reading : khz;
    }
    if (khz < kMinKhz || khz > kMaxKhz) {
        return false;
    }
    tsc_khz = khz;
    tsc_start = rdtsc();
    return true;
}

u64 clock_us() {
    if (tsc_khz == 0) {
        return 0;
    }
    return (rdtsc() - tsc_start) * 1000 / tsc_khz;
}

void delay_us(u64 us) {
    if (tsc_khz == 0) {
        return;
    }
    const u64 end = clock_us() + us;
    while (clock_us() < end) {
        asm volatile("pause");
    }
}

}
