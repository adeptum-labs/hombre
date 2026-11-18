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

#include "check.hpp"
#include "dos_date.hpp"

#include <cstring>
#include <string>

using namespace hombre;

namespace {

constexpr u32 kSampleDay = 17853u;  // 2026-11-18 (Wednesday)

void epoch_is_day_zero() {
    const CivilTime epoch{1978, 1, 1, 0, 0, 0};
    const DosStamp stamp = dos_stamp_from_civil(epoch);
    CHECK_EQ(stamp.days, 0u);
    CHECK_EQ(stamp.minutes, 0u);
    CHECK_EQ(stamp.ticks, 0u);
    CHECK_EQ(dos_weekday(stamp.days), 0u);

    const CivilTime back = dos_civil_from_stamp(stamp);
    CHECK_EQ(back.year, 1978u);
    CHECK_EQ(back.month, 1u);
    CHECK_EQ(back.day, 1u);
    CHECK_EQ(back.hour, 0u);
    CHECK_EQ(back.minute, 0u);
    CHECK_EQ(back.second, 0u);

    const DosStamp clamped = dos_stamp_from_civil(CivilTime{1977, 12, 31, 23, 59, 59});
    CHECK_EQ(clamped.days, 0u);
    CHECK_EQ(clamped.minutes, 0u);
    CHECK_EQ(clamped.ticks, 0u);
}

void round_trip() {
    // 1978..2025 is 48 years with 12 leap years (1980..2024): 48*365 + 12 = 17532 days to
    // 2026-01-01, plus 304 days for Jan..Oct in 2026, plus 17 days to Nov 18 = 17853.
    const CivilTime t{2026, 11, 18, 17, 30, 45};
    const DosStamp s = dos_stamp_from_civil(t);
    CHECK_EQ(s.days, kSampleDay);
    CHECK_EQ(s.minutes, 17u * 60u + 30u);
    CHECK_EQ(s.ticks, 45u * 50u);
    CHECK_EQ(dos_weekday(s.days), 3u);

    const CivilTime back = dos_civil_from_stamp(s);
    CHECK_EQ(back.year, t.year);
    CHECK_EQ(back.month, t.month);
    CHECK_EQ(back.day, t.day);
    CHECK_EQ(back.hour, t.hour);
    CHECK_EQ(back.minute, t.minute);
    CHECK_EQ(back.second, t.second);
}

void leap_years() {
    const CivilTime leap_2000{2000, 2, 29, 12, 0, 0};
    const DosStamp s2000 = dos_stamp_from_civil(leap_2000);
    const CivilTime b2000 = dos_civil_from_stamp(s2000);
    CHECK_EQ(b2000.year, 2000u);
    CHECK_EQ(b2000.month, 2u);
    CHECK_EQ(b2000.day, 29u);
    const CivilTime next_2000 = dos_civil_from_stamp(DosStamp{s2000.days + 1u, 0, 0});
    CHECK_EQ(next_2000.month, 3u);
    CHECK_EQ(next_2000.day, 1u);

    const CivilTime leap_2024{2024, 2, 29, 23, 59, 59};
    const DosStamp s2024 = dos_stamp_from_civil(leap_2024);
    const CivilTime b2024 = dos_civil_from_stamp(s2024);
    CHECK_EQ(b2024.year, 2024u);
    CHECK_EQ(b2024.month, 2u);
    CHECK_EQ(b2024.day, 29u);
    CHECK_EQ(b2024.hour, 23u);
    CHECK_EQ(b2024.minute, 59u);
    CHECK_EQ(b2024.second, 59u);

    // 2100 is not a leap year.
    const DosStamp s2100 = dos_stamp_from_civil(CivilTime{2100, 2, 28, 0, 0, 0});
    const CivilTime next_2100 = dos_civil_from_stamp(DosStamp{s2100.days + 1u, 0, 0});
    CHECK_EQ(next_2100.year, 2100u);
    CHECK_EQ(next_2100.month, 3u);
    CHECK_EQ(next_2100.day, 1u);
}

void format_dos() {
    const DosStamp s = dos_stamp_from_civil(CivilTime{2026, 11, 18, 17, 30, 45});
    char date[LEN_DATSTRING]{};
    char day[LEN_DATSTRING]{};
    char time[LEN_DATSTRING]{};
    CHECK(dos_format_date(s, FORMAT_DOS, 0, 0, date, sizeof date, day, sizeof day, time,
                          sizeof time) != 0u);
    CHECK(std::string(date) == "18-Nov-26");
    CHECK(std::string(day) == "Wednesday");
    CHECK(std::string(time) == "17:30:45");
}

void format_int_usa_cdn() {
    const DosStamp s = dos_stamp_from_civil(CivilTime{2026, 11, 18, 5, 4, 3});
    char date[LEN_DATSTRING]{};
    char time[LEN_DATSTRING]{};

    CHECK(dos_format_date(s, FORMAT_INT, 0, 0, date, sizeof date, nullptr, 0, time,
                          sizeof time) != 0u);
    CHECK(std::string(date) == "26-11-18");
    CHECK(std::string(time) == "05:04:03");

    CHECK(dos_format_date(s, FORMAT_USA, 0, 0, date, sizeof date, nullptr, 0, nullptr, 0) != 0u);
    CHECK(std::string(date) == "11-18-26");

    CHECK(dos_format_date(s, FORMAT_CDN, 0, 0, date, sizeof date, nullptr, 0, nullptr, 0) != 0u);
    CHECK(std::string(date) == "18-11-26");

    const DosStamp early = dos_stamp_from_civil(CivilTime{1978, 1, 5, 0, 0, 0});
    CHECK(dos_format_date(early, FORMAT_DOS, 0, 0, date, sizeof date, nullptr, 0, nullptr, 0) !=
          0u);
    CHECK(std::string(date) == "05-Jan-78");
}

void dtf_values_match_ndk() {
    CHECK_EQ(DTF_SUBST, 1u);
    CHECK_EQ(DTF_FUTURE, 2u);
}

void subst_today_yesterday_tomorrow() {
    char date[LEN_DATSTRING]{};
    char day[LEN_DATSTRING]{};

    CHECK(dos_format_date(DosStamp{kSampleDay, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay, date,
                          sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "Today");
    CHECK(std::string(day) == "Wednesday");

    CHECK(dos_format_date(DosStamp{kSampleDay - 1u, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay,
                          date, sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "Yesterday");
    CHECK(std::string(day) == "Tuesday");

    CHECK(dos_format_date(DosStamp{kSampleDay + 1u, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay,
                          date, sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "Tomorrow");
    CHECK(std::string(day) == "Thursday");

    // Day 0 with today == 0 must not underflow when checking Yesterday.
    CHECK(dos_format_date(DosStamp{0, 0, 0}, FORMAT_DOS, DTF_SUBST, 0, date, sizeof date, day,
                          sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "Today");
    CHECK(std::string(day) == "Sunday");
}

void subst_weekday_within_week() {
    char date[LEN_DATSTRING]{};
    char day[LEN_DATSTRING]{};

    // 2 days before Wednesday 2026-11-18 is Monday 2026-11-16.
    CHECK(dos_format_date(DosStamp{kSampleDay - 2u, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay, date,
                          sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "Monday");
    CHECK(std::string(day) == "Monday");

    // 6 days before Wednesday is Thursday.
    CHECK(dos_format_date(DosStamp{kSampleDay - 6u, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay,
                          date, sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "Thursday");
    CHECK(std::string(day) == "Thursday");
}

void subst_goes_to_the_date_string() {
    char date[LEN_DATSTRING]{};
    char day[LEN_DATSTRING]{};

    // Today, Yesterday, Tomorrow, and recent weekdays go to date; day keeps weekday.
    CHECK(dos_format_date(DosStamp{kSampleDay, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay, date,
                          sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "Today");
    CHECK(std::string(day) == "Wednesday");

    CHECK(dos_format_date(DosStamp{kSampleDay - 2u, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay, date,
                          sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "Monday");
    CHECK(std::string(day) == "Monday");

    // 7 days before today and 2 days after today fall back to numeric date.
    CHECK(dos_format_date(DosStamp{kSampleDay - 7u, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay, date,
                          sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "11-Nov-26");
    CHECK(std::string(day) == "Wednesday");

    CHECK(dos_format_date(DosStamp{kSampleDay + 2u, 0, 0}, FORMAT_DOS, DTF_SUBST, kSampleDay, date,
                          sizeof date, day, sizeof day, nullptr, 0) != 0u);
    CHECK(std::string(date) == "20-Nov-26");
    CHECK(std::string(day) == "Friday");
}

void format_def_is_dos() {
    CHECK_EQ(FORMAT_DEF, 4u);
    const DosStamp s = dos_stamp_from_civil(CivilTime{2026, 11, 18, 17, 30, 45});
    char date[LEN_DATSTRING]{};
    char day[LEN_DATSTRING]{};
    char time[LEN_DATSTRING]{};
    CHECK(dos_format_date(s, FORMAT_DEF, 0, 0, date, sizeof date, day, sizeof day, time,
                          sizeof time) != 0u);
    CHECK(std::string(date) == "18-Nov-26");
    CHECK(std::string(day) == "Wednesday");
    CHECK(std::string(time) == "17:30:45");

    DosStamp out{};
    CHECK(dos_parse_date("18-Nov-26", nullptr, FORMAT_DEF, 0, out));
    CHECK_EQ(out.days, kSampleDay);

    CHECK_EQ(dos_format_date(s, FORMAT_DEF + 1u, 0, 0, date, sizeof date, nullptr, 0, nullptr, 0),
             0u);
    CHECK(!dos_parse_date("18-Nov-26", nullptr, FORMAT_DEF + 1u, 0, out));
}

void parse_dos_date_and_time() {
    DosStamp out{};
    CHECK(dos_parse_date("18-Nov-26", "17:30:45", FORMAT_DOS, 0, out));
    CHECK_EQ(out.days, kSampleDay);
    CHECK_EQ(out.minutes, 1050u);
    CHECK_EQ(out.ticks, 2250u);

    // Null date keeps out.days; HH:MM sets ticks to 0.
    CHECK(dos_parse_date(nullptr, "9:05", FORMAT_DOS, 0, out));
    CHECK_EQ(out.days, kSampleDay);
    CHECK_EQ(out.minutes, 9u * 60u + 5u);
    CHECK_EQ(out.ticks, 0u);

    // Null time keeps out.minutes and out.ticks.
    CHECK(dos_parse_date("26-11-18", nullptr, FORMAT_INT, 0, out));
    CHECK_EQ(out.days, kSampleDay);
    CHECK_EQ(out.minutes, 545u);
    CHECK_EQ(out.ticks, 0u);

    CHECK(dos_parse_date("11-18-26", nullptr, FORMAT_USA, 0, out));
    CHECK_EQ(out.days, kSampleDay);
    CHECK(dos_parse_date("18-11-26", nullptr, FORMAT_CDN, 0, out));
    CHECK_EQ(out.days, kSampleDay);

    // Two-digit year pivot: 78-99 -> 19xx, 00-77 -> 20xx.
    CHECK(dos_parse_date("01-Jan-78", nullptr, FORMAT_DOS, 0, out));
    CHECK_EQ(out.days, 0u);

    CHECK(dos_parse_date("01-Jan-00", nullptr, FORMAT_DOS, 0, out));
    CHECK_EQ(dos_civil_from_stamp(out).year, 2000u);

    CHECK(dos_parse_date("31-Dec-77", nullptr, FORMAT_DOS, 0, out));
    CHECK_EQ(dos_civil_from_stamp(out).year, 2077u);
}

void parse_words_today_and_weekday() {
    DosStamp out{};

    CHECK(dos_parse_date("today", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK_EQ(out.days, kSampleDay);

    CHECK(dos_parse_date("YESTERDAY", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK_EQ(out.days, kSampleDay - 1u);

    CHECK(dos_parse_date("Tomorrow", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK_EQ(out.days, kSampleDay + 1u);

    // kSampleDay is Wednesday: most recent past Monday is -2, Thursday is -6,
    // and Wednesday is -7.
    CHECK(dos_parse_date("monday", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK_EQ(out.days, kSampleDay - 2u);

    CHECK(dos_parse_date("Thursday", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK_EQ(out.days, kSampleDay - 6u);

    CHECK(dos_parse_date("wednesday", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK_EQ(out.days, kSampleDay - 7u);
}

void parse_weekday_future() {
    DosStamp out{};

    // kSampleDay is Wednesday: next Thursday is +1, next Monday is +5, next Wednesday is +7.
    CHECK(dos_parse_date("thursday", nullptr, FORMAT_DOS, kSampleDay, out, DTF_FUTURE));
    CHECK_EQ(out.days, kSampleDay + 1u);

    CHECK(dos_parse_date("monday", nullptr, FORMAT_DOS, kSampleDay, out, DTF_FUTURE));
    CHECK_EQ(out.days, kSampleDay + 5u);

    CHECK(dos_parse_date("wednesday", nullptr, FORMAT_DOS, kSampleDay, out, DTF_FUTURE));
    CHECK_EQ(out.days, kSampleDay + 7u);

    // Without DTF_FUTURE, Wednesday resolves to 7 days earlier.
    CHECK(dos_parse_date("wednesday", nullptr, FORMAT_DOS, kSampleDay, out, 0));
    CHECK_EQ(out.days, kSampleDay - 7u);

    CHECK(!dos_parse_date("thursday", nullptr, FORMAT_DOS, 0xFFFFFFFFu, out, DTF_FUTURE));
}

void parse_bad_text_fails() {
    const DosStamp sentinel{123u, 456u, 789u};
    DosStamp out = sentinel;

    CHECK(!dos_parse_date("", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date(nullptr, "", FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date("18-Foo-26", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date("32-Nov-26", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date("31-Apr-26", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date("29-Feb-25", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date("18-Nov-2026", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date("18-Nov-26x", nullptr, FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date("18-Nov-26", nullptr, 99, kSampleDay, out));
    CHECK(!dos_parse_date(nullptr, "24:00:00", FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date(nullptr, "12:60:00", FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date(nullptr, "12:00:60", FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date(nullptr, "12:3:00", FORMAT_DOS, kSampleDay, out));
    CHECK(!dos_parse_date("18-Nov-26", "bad", FORMAT_DOS, kSampleDay, out));

    CHECK_EQ(out.days, sentinel.days);
    CHECK_EQ(out.minutes, sentinel.minutes);
    CHECK_EQ(out.ticks, sentinel.ticks);
}

void small_buffers_truncate_safely() {
    const DosStamp s{kSampleDay, 1050u, 2250u};

    // Size 4 cannot hold "18-Nov-26" (9 chars + NUL); dos_format_date writes the
    // NUL-terminated 3-character prefix "18-" and returns 0.
    char tiny[4]{'x', 'x', 'x', 'x'};
    CHECK_EQ(dos_format_date(s, FORMAT_DOS, 0, 0, tiny, sizeof tiny, nullptr, 0, nullptr, 0), 0u);
    CHECK(std::strcmp(tiny, "18-") == 0);

    char one[1]{'x'};
    CHECK_EQ(dos_format_date(s, FORMAT_DOS, 0, 0, nullptr, 0, one, sizeof one, nullptr, 0), 0u);
    CHECK_EQ(one[0], '\0');

    char zero_guard[2]{'a', 'b'};
    CHECK_EQ(dos_format_date(s, FORMAT_DOS, 0, 0, zero_guard, 0, nullptr, 0, nullptr, 0), 0u);
    CHECK_EQ(zero_guard[0], 'a');
    CHECK_EQ(zero_guard[1], 'b');
}

}  // namespace

int main() {
    epoch_is_day_zero();
    round_trip();
    leap_years();
    format_dos();
    format_int_usa_cdn();
    dtf_values_match_ndk();
    subst_today_yesterday_tomorrow();
    subst_weekday_within_week();
    subst_goes_to_the_date_string();
    format_def_is_dos();
    parse_dos_date_and_time();
    parse_words_today_and_weekday();
    parse_weekday_future();
    parse_bad_text_fails();
    small_buffers_truncate_safely();
    return check::finish("test_dos_date");
}
