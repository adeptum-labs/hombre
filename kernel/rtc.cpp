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

#include "rtc.hpp"

#include "../common/port_io.hpp"

namespace hombre {

namespace {

constexpr u16 kCmosIndexPort = 0x70;
constexpr u16 kCmosDataPort = 0x71;

constexpr u8 kRegSeconds = 0x00;
constexpr u8 kRegMinutes = 0x02;
constexpr u8 kRegHours = 0x04;
constexpr u8 kRegDay = 0x07;
constexpr u8 kRegMonth = 0x08;
constexpr u8 kRegYear = 0x09;
constexpr u8 kRegStatusA = 0x0A;
constexpr u8 kRegStatusB = 0x0B;
// Common CMOS century register; ACPI FADT may specify another on some systems.
constexpr u8 kRegCentury = 0x32;

constexpr u8 kStatusAUpdateInProgress = 0x80;

constexpr u32 kMaxUpdatePolls = 100000;
constexpr u32 kMaxReadRounds = 5;

// Selects a CMOS register using the plain register number (bit 7 = 0).
u8 cmos_read(u8 reg) {
    outb(kCmosIndexPort, reg);
    return inb(kCmosDataPort);
}

bool wait_not_updating() {
    for (u32 i = 0; i < kMaxUpdatePolls; ++i) {
        if ((cmos_read(kRegStatusA) & kStatusAUpdateInProgress) == 0u) {
            return true;
        }
    }
    return false;
}

RtcRegisters read_registers() {
    return RtcRegisters{
        cmos_read(kRegSeconds),
        cmos_read(kRegMinutes),
        cmos_read(kRegHours),
        cmos_read(kRegDay),
        cmos_read(kRegMonth),
        cmos_read(kRegYear),
        cmos_read(kRegCentury),
        cmos_read(kRegStatusB),
    };
}

bool registers_equal(const RtcRegisters& a, const RtcRegisters& b) {
    return a.seconds == b.seconds && a.minutes == b.minutes && a.hours == b.hours &&
           a.day == b.day && a.month == b.month && a.year == b.year &&
           a.century == b.century && a.status_b == b.status_b;
}

}  // namespace

bool rtc_read(CivilTime& out) {
    if (!wait_not_updating()) {
        return false;
    }
    RtcRegisters prev = read_registers();
    for (u32 round = 0; round < kMaxReadRounds; ++round) {
        if (!wait_not_updating()) {
            return false;
        }
        const RtcRegisters curr = read_registers();
        if (registers_equal(prev, curr)) {
            return rtc_decode(curr, out);
        }
        prev = curr;
    }
    return false;
}

}  // namespace hombre
