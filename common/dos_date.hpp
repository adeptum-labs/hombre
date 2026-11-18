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

#include "rtc_decode.hpp"
#include "types.hpp"

namespace hombre {

// AmigaDOS DateStamp constants and format identifiers (dos.library 3.1).
constexpr u32 TICKS_PER_SECOND = 50;
constexpr u32 LEN_DATSTRING = 16;

constexpr u8 FORMAT_DOS = 0;  // DD-MMM-YY
constexpr u8 FORMAT_INT = 1;  // YY-MM-DD
constexpr u8 FORMAT_USA = 2;  // MM-DD-YY
constexpr u8 FORMAT_CDN = 3;  // DD-MM-YY

constexpr u8 DTF_SUBST = 0x02;

// Days since 1978-01-01, minutes past midnight (0..1439), and ticks (50 Hz).
struct DosStamp {
    u32 days;
    u32 minutes;
    u32 ticks;
};

namespace detail {

// Days from 1970-01-01 to 1978-01-01, the first day of Amiga time.
constexpr i64 kAmigaEpochDays = 2922;
constexpr u32 kMinutesPerDay = 1440;
constexpr u32 kTicksPerMinute = 3000;
constexpr u32 kMaxDateText = 64;

inline constexpr const char* kMonthNames[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

inline constexpr const char* kWeekdayNames[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

inline i64 days_from_civil(i64 y, i64 m, i64 d) {
    y -= m <= 2 ? 1 : 0;
    const i64 era = (y >= 0 ? y : y - 399) / 400;
    const i64 yoe = y - era * 400;
    const i64 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

inline void civil_from_days(i64 days, u32& y, u32& m, u32& d) {
    const i64 z = days + 719468;
    const i64 era = z / 146097;
    const i64 doe = z - era * 146097;
    const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const i64 mp = (5 * doy + 2) / 153;
    d = static_cast<u32>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<u32>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<u32>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

inline bool is_leap_year(u32 year) {
    return (year % 4u == 0u && year % 100u != 0u) || (year % 400u == 0u);
}

inline u32 days_in_month(u32 year, u32 month) {
    static constexpr u8 kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1u || month > 12u) {
        return 0u;
    }
    if (month == 2u && is_leap_year(year)) {
        return 29u;
    }
    return kMonthDays[month - 1u];
}

inline char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool equals_ignore_case(const char* a, const char* b, u32 max_len) {
    for (u32 i = 0; i < max_len; ++i) {
        const char ca = to_lower_ascii(a[i]);
        const char cb = to_lower_ascii(b[i]);
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
    }
    return false;
}

inline void write_two_digits(char* dst, u32 value) {
    const u32 v = value % 100u;
    dst[0] = static_cast<char>('0' + v / 10u);
    dst[1] = static_cast<char>('0' + v % 10u);
}

// Copies up to dst_size - 1 bytes of src and NUL-terminates dst when dst_size
// is non-zero. Returns false if dst_size is 0 or src was truncated.
inline bool copy_bounded(char* dst, u32 dst_size, const char* src, u32 src_len) {
    if (dst == nullptr) {
        return true;
    }
    if (dst_size == 0u) {
        return false;
    }
    const u32 limit = dst_size - 1u;
    const u32 count = src_len < limit ? src_len : limit;
    for (u32 i = 0; i < count; ++i) {
        dst[i] = src[i];
    }
    dst[count] = '\0';
    return src_len < dst_size;
}

inline u32 str_len_bounded(const char* s, u32 max_len) {
    u32 n = 0;
    while (n < max_len && s[n] != '\0') {
        ++n;
    }
    return n;
}

inline bool parse_uint_digits(const char* s, u32& pos, u32 min_digits, u32 max_digits,
                              u32& out) {
    u32 val = 0;
    u32 count = 0;
    while (count < max_digits && pos < kMaxDateText) {
        const char c = s[pos];
        if (c < '0' || c > '9') {
            break;
        }
        val = val * 10u + static_cast<u32>(c - '0');
        ++pos;
        ++count;
    }
    if (count < min_digits) {
        return false;
    }
    out = val;
    return true;
}

inline bool expect_char(const char* s, u32& pos, char expected) {
    if (pos >= kMaxDateText || s[pos] != expected) {
        return false;
    }
    ++pos;
    return true;
}

inline bool parse_month_abbr(const char* s, u32& pos, u32& month_out) {
    if (pos + 3u > kMaxDateText) {
        return false;
    }
    const char c0 = to_lower_ascii(s[pos]);
    const char c1 = to_lower_ascii(s[pos + 1u]);
    const char c2 = to_lower_ascii(s[pos + 2u]);
    if (c0 == '\0' || c1 == '\0' || c2 == '\0') {
        return false;
    }
    for (u32 i = 0; i < 12u; ++i) {
        const char* m = kMonthNames[i];
        if (c0 == to_lower_ascii(m[0]) && c1 == to_lower_ascii(m[1]) &&
            c2 == to_lower_ascii(m[2])) {
            pos += 3u;
            month_out = i + 1u;
            return true;
        }
    }
    return false;
}

inline bool parse_date_fields(const char* date, u8 format, u32& year_out, u32& month_out,
                              u32& day_out) {
    u32 pos = 0;
    u32 day = 0;
    u32 month = 0;
    u32 year2 = 0;

    switch (format) {
    case FORMAT_DOS:
        if (!parse_uint_digits(date, pos, 1u, 2u, day) || !expect_char(date, pos, '-') ||
            !parse_month_abbr(date, pos, month) || !expect_char(date, pos, '-') ||
            !parse_uint_digits(date, pos, 2u, 2u, year2)) {
            return false;
        }
        break;
    case FORMAT_INT:
        if (!parse_uint_digits(date, pos, 2u, 2u, year2) || !expect_char(date, pos, '-') ||
            !parse_uint_digits(date, pos, 1u, 2u, month) || !expect_char(date, pos, '-') ||
            !parse_uint_digits(date, pos, 1u, 2u, day)) {
            return false;
        }
        break;
    case FORMAT_USA:
        if (!parse_uint_digits(date, pos, 1u, 2u, month) || !expect_char(date, pos, '-') ||
            !parse_uint_digits(date, pos, 1u, 2u, day) || !expect_char(date, pos, '-') ||
            !parse_uint_digits(date, pos, 2u, 2u, year2)) {
            return false;
        }
        break;
    case FORMAT_CDN:
        if (!parse_uint_digits(date, pos, 1u, 2u, day) || !expect_char(date, pos, '-') ||
            !parse_uint_digits(date, pos, 1u, 2u, month) || !expect_char(date, pos, '-') ||
            !parse_uint_digits(date, pos, 2u, 2u, year2)) {
            return false;
        }
        break;
    default:
        return false;
    }

    if (pos >= kMaxDateText || date[pos] != '\0') {
        return false;
    }

    const u32 full_year = year2 >= 78u ? 1900u + year2 : 2000u + year2;
    const u32 max_day = days_in_month(full_year, month);
    if (max_day == 0u || day < 1u || day > max_day) {
        return false;
    }

    year_out = full_year;
    month_out = month;
    day_out = day;
    return true;
}

inline bool parse_time_fields(const char* time, u32& minutes_out, u32& ticks_out) {
    u32 pos = 0;
    u32 hour = 0;
    u32 minute = 0;
    u32 second = 0;

    if (!parse_uint_digits(time, pos, 1u, 2u, hour) || !expect_char(time, pos, ':') ||
        !parse_uint_digits(time, pos, 2u, 2u, minute)) {
        return false;
    }

    if (pos < kMaxDateText && time[pos] == ':') {
        ++pos;
        if (!parse_uint_digits(time, pos, 2u, 2u, second)) {
            return false;
        }
    }

    if (pos >= kMaxDateText || time[pos] != '\0') {
        return false;
    }
    if (hour > 23u || minute > 59u || second > 59u) {
        return false;
    }

    minutes_out = hour * 60u + minute;
    ticks_out = second * TICKS_PER_SECOND;
    return true;
}

}  // namespace detail

// Day of week for an AmigaDOS day number (0 = Sunday; 1978-01-01 was Sunday).
inline u32 dos_weekday(u32 days) {
    return days % 7u;
}

// Converts a CivilTime to an AmigaDOS DateStamp; dates before 1978 clamp to 0.
inline DosStamp dos_stamp_from_civil(const CivilTime& c) {
    if (c.year < 1978u) {
        return DosStamp{0u, 0u, 0u};
    }
    const i64 days = detail::days_from_civil(static_cast<i64>(c.year), static_cast<i64>(c.month),
                                             static_cast<i64>(c.day)) -
                     detail::kAmigaEpochDays;
    if (days < 0) {
        return DosStamp{0u, 0u, 0u};
    }
    const u32 minutes = c.hour * 60u + c.minute;
    const u32 ticks = c.second * TICKS_PER_SECOND;
    return DosStamp{static_cast<u32>(days), minutes, ticks};
}

// Converts an AmigaDOS DateStamp back to CivilTime, normalizing overflow.
inline CivilTime dos_civil_from_stamp(const DosStamp& s) {
    const u64 extra_min = s.ticks / detail::kTicksPerMinute;
    const u64 rem_ticks = s.ticks % detail::kTicksPerMinute;
    const u64 total_min = static_cast<u64>(s.minutes) + extra_min;
    const u64 extra_days = total_min / detail::kMinutesPerDay;
    const u64 rem_min = total_min % detail::kMinutesPerDay;

    u32 year = 0;
    u32 month = 0;
    u32 day = 0;
    detail::civil_from_days(detail::kAmigaEpochDays + static_cast<i64>(s.days) +
                                static_cast<i64>(extra_days),
                            year, month, day);

    const u32 hour = static_cast<u32>(rem_min / 60u);
    const u32 minute = static_cast<u32>(rem_min % 60u);
    const u32 second = static_cast<u32>(rem_ticks / TICKS_PER_SECOND);
    return CivilTime{year, month, day, hour, minute, second};
}

// Formats stamp into optional NUL-terminated date, day, and time buffers using
// dos.library 3.1 English locale rules. Buffers that are too small receive a
// NUL-terminated truncated prefix and cause the function to return 0.
inline u32 dos_format_date(const DosStamp& stamp, u8 format, u8 flags, u32 today, char* date,
                           u32 date_size, char* day, u32 day_size, char* time, u32 time_size) {
    if (format > FORMAT_CDN || stamp.minutes >= detail::kMinutesPerDay ||
        stamp.ticks >= detail::kTicksPerMinute) {
        detail::copy_bounded(date, date_size, "", 0u);
        detail::copy_bounded(day, day_size, "", 0u);
        detail::copy_bounded(time, time_size, "", 0u);
        return 0u;
    }

    const CivilTime c = dos_civil_from_stamp(stamp);
    bool ok = true;

    if (date != nullptr) {
        char buf[16]{};
        u32 len = 0;
        const u32 yy = c.year % 100u;
        switch (format) {
        case FORMAT_DOS: {
            const char* mon = detail::kMonthNames[(c.month - 1u) % 12u];
            detail::write_two_digits(buf, c.day);
            buf[2] = '-';
            buf[3] = mon[0];
            buf[4] = mon[1];
            buf[5] = mon[2];
            buf[6] = '-';
            detail::write_two_digits(buf + 7, yy);
            len = 9u;
            break;
        }
        case FORMAT_INT:
            detail::write_two_digits(buf, yy);
            buf[2] = '-';
            detail::write_two_digits(buf + 3, c.month);
            buf[5] = '-';
            detail::write_two_digits(buf + 6, c.day);
            len = 8u;
            break;
        case FORMAT_USA:
            detail::write_two_digits(buf, c.month);
            buf[2] = '-';
            detail::write_two_digits(buf + 3, c.day);
            buf[5] = '-';
            detail::write_two_digits(buf + 6, yy);
            len = 8u;
            break;
        case FORMAT_CDN:
            detail::write_two_digits(buf, c.day);
            buf[2] = '-';
            detail::write_two_digits(buf + 3, c.month);
            buf[5] = '-';
            detail::write_two_digits(buf + 6, yy);
            len = 8u;
            break;
        default:
            return 0u;
        }
        if (!detail::copy_bounded(date, date_size, buf, len)) {
            ok = false;
        }
    }

    if (day != nullptr) {
        const char* day_str = detail::kWeekdayNames[dos_weekday(stamp.days)];
        if ((flags & DTF_SUBST) != 0u) {
            if (stamp.days == today) {
                day_str = "Today";
            } else if (today > 0u && stamp.days == today - 1u) {
                day_str = "Yesterday";
            } else if (today < 0xFFFFFFFFu && stamp.days == today + 1u) {
                day_str = "Tomorrow";
            }
        }
        const u32 day_len = detail::str_len_bounded(day_str, LEN_DATSTRING);
        if (!detail::copy_bounded(day, day_size, day_str, day_len)) {
            ok = false;
        }
    }

    if (time != nullptr) {
        char buf[16]{};
        detail::write_two_digits(buf, c.hour);
        buf[2] = ':';
        detail::write_two_digits(buf + 3, c.minute);
        buf[5] = ':';
        detail::write_two_digits(buf + 6, c.second);
        if (!detail::copy_bounded(time, time_size, buf, 8u)) {
            ok = false;
        }
    }

    return ok ? 1u : 0u;
}

// Parses optional date and time strings into out. Null date leaves out.days
// untouched; null time leaves out.minutes and out.ticks untouched. Returns
// false without modifying out if either input is invalid.
inline bool dos_parse_date(const char* date, const char* time, u8 format, u32 today,
                           DosStamp& out) {
    u32 parsed_days = out.days;
    u32 parsed_minutes = out.minutes;
    u32 parsed_ticks = out.ticks;

    if (date != nullptr) {
        if (format > FORMAT_CDN) {
            return false;
        }
        if (detail::equals_ignore_case(date, "Today", detail::kMaxDateText)) {
            parsed_days = today;
        } else if (detail::equals_ignore_case(date, "Yesterday", detail::kMaxDateText)) {
            if (today == 0u) {
                return false;
            }
            parsed_days = today - 1u;
        } else if (detail::equals_ignore_case(date, "Tomorrow", detail::kMaxDateText)) {
            if (today == 0xFFFFFFFFu) {
                return false;
            }
            parsed_days = today + 1u;
        } else {
            bool matched_wday = false;
            for (u32 w = 0; w < 7u; ++w) {
                if (detail::equals_ignore_case(date, detail::kWeekdayNames[w],
                                               detail::kMaxDateText)) {
                    const u32 today_wday = dos_weekday(today);
                    u32 delta = (today_wday + 7u - w) % 7u;
                    if (delta == 0u) {
                        delta = 7u;
                    }
                    if (today < delta) {
                        return false;
                    }
                    parsed_days = today - delta;
                    matched_wday = true;
                    break;
                }
            }
            if (!matched_wday) {
                u32 year = 0;
                u32 month = 0;
                u32 day = 0;
                if (!detail::parse_date_fields(date, format, year, month, day)) {
                    return false;
                }
                const i64 d =
                    detail::days_from_civil(static_cast<i64>(year), static_cast<i64>(month),
                                            static_cast<i64>(day)) -
                    detail::kAmigaEpochDays;
                if (d < 0) {
                    return false;
                }
                parsed_days = static_cast<u32>(d);
            }
        }
    }

    if (time != nullptr) {
        if (!detail::parse_time_fields(time, parsed_minutes, parsed_ticks)) {
            return false;
        }
    }

    out.days = parsed_days;
    out.minutes = parsed_minutes;
    out.ticks = parsed_ticks;
    return true;
}

}  // namespace hombre
