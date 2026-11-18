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

#include "types.hpp"

namespace hombre {

// Raw MC146818-compatible PC CMOS real-time clock register values.
struct RtcRegisters {
    u8 seconds;
    u8 minutes;
    u8 hours;
    u8 day;
    u8 month;
    u8 year;
    u8 century;
    u8 status_b;
};

// Broken-down calendar date and time of day.
struct CivilTime {
    u32 year;
    u32 month;
    u32 day;
    u32 hour;
    u32 minute;
    u32 second;
};

namespace detail {

inline bool decode_rtc_byte(u8 raw, bool binary, u32& out) {
    if (binary) {
        out = raw;
        return true;
    }
    const u8 hi = static_cast<u8>(raw >> 4);
    const u8 lo = static_cast<u8>(raw & 0x0Fu);
    if (hi > 9u || lo > 9u) {
        return false;
    }
    out = static_cast<u32>(hi) * 10u + static_cast<u32>(lo);
    return true;
}

}  // namespace detail

// Decodes CMOS RTC registers into out. status_b bit 2 selects binary (1) or
// BCD (0); bit 1 selects 24-hour (1) or 12-hour (0) with bit 7 of hours as PM.
// An invalid or out-of-range century register (outside 19..21) defaults to 20
// (20xx). Returns false on invalid data in any other field.
inline bool rtc_decode(const RtcRegisters& r, CivilTime& out) {
    const bool binary = (r.status_b & 0x04u) != 0u;
    const bool hour_24 = (r.status_b & 0x02u) != 0u;

    u32 second = 0;
    u32 minute = 0;
    u32 hour = 0;
    u32 day = 0;
    u32 month = 0;
    u32 year = 0;
    u32 century = 0;

    if (!detail::decode_rtc_byte(r.seconds, binary, second) ||
        !detail::decode_rtc_byte(r.minutes, binary, minute) ||
        !detail::decode_rtc_byte(r.day, binary, day) ||
        !detail::decode_rtc_byte(r.month, binary, month) ||
        !detail::decode_rtc_byte(r.year, binary, year)) {
        return false;
    }

    if (hour_24) {
        if (!detail::decode_rtc_byte(r.hours, binary, hour) || hour > 23u) {
            return false;
        }
    } else {
        const bool pm = (r.hours & 0x80u) != 0u;
        const u8 raw_hour = static_cast<u8>(r.hours & 0x7Fu);
        if (!detail::decode_rtc_byte(raw_hour, binary, hour) || hour < 1u || hour > 12u) {
            return false;
        }
        if (hour == 12u) {
            hour = pm ? 12u : 0u;
        } else if (pm) {
            hour += 12u;
        }
    }

    if (second > 59u || minute > 59u || month < 1u || month > 12u || day < 1u || day > 31u ||
        year > 99u) {
        return false;
    }

    if (!detail::decode_rtc_byte(r.century, binary, century) || century < 19u || century > 21u) {
        century = 20u;
    }

    out = CivilTime{century * 100u + year, month, day, hour, minute, second};
    return true;
}

}  // namespace hombre
