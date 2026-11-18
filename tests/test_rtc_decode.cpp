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
#include "rtc_decode.hpp"

using namespace hombre;

namespace {

void bcd_24h() {
    const RtcRegisters r{0x45, 0x30, 0x17, 0x18, 0x11, 0x26, 0x20, 0x02};
    CivilTime out{};
    CHECK(rtc_decode(r, out));
    CHECK_EQ(out.year, 2026u);
    CHECK_EQ(out.month, 11u);
    CHECK_EQ(out.day, 18u);
    CHECK_EQ(out.hour, 17u);
    CHECK_EQ(out.minute, 30u);
    CHECK_EQ(out.second, 45u);
}

void binary_24h() {
    const RtcRegisters r{59, 42, 23, 29, 2, 24, 20, 0x06};
    CivilTime out{};
    CHECK(rtc_decode(r, out));
    CHECK_EQ(out.year, 2024u);
    CHECK_EQ(out.month, 2u);
    CHECK_EQ(out.day, 29u);
    CHECK_EQ(out.hour, 23u);
    CHECK_EQ(out.minute, 42u);
    CHECK_EQ(out.second, 59u);
}

void bcd_12h_pm() {
    const RtcRegisters r{0x05, 0x09, 0x85, 0x01, 0x01, 0x26, 0x20, 0x00};
    CivilTime out{};
    CHECK(rtc_decode(r, out));
    CHECK_EQ(out.hour, 17u);
    CHECK_EQ(out.minute, 9u);
    CHECK_EQ(out.second, 5u);

    // 12 PM stays 12.
    const RtcRegisters noon{0x00, 0x00, 0x92, 0x01, 0x01, 0x26, 0x20, 0x00};
    CHECK(rtc_decode(noon, out));
    CHECK_EQ(out.hour, 12u);
}

void twelve_am_is_zero() {
    const RtcRegisters bcd{0x00, 0x00, 0x12, 0x01, 0x01, 0x26, 0x20, 0x00};
    CivilTime out{};
    CHECK(rtc_decode(bcd, out));
    CHECK_EQ(out.hour, 0u);

    const RtcRegisters bin{0, 0, 12, 1, 1, 26, 20, 0x04};
    CHECK(rtc_decode(bin, out));
    CHECK_EQ(out.hour, 0u);
}

void century_zero_is_2000s() {
    const RtcRegisters r{0x00, 0x15, 0x08, 0x07, 0x04, 0x26, 0x00, 0x02};
    CivilTime out{};
    CHECK(rtc_decode(r, out));
    CHECK_EQ(out.year, 2026u);
    CHECK_EQ(out.month, 4u);
    CHECK_EQ(out.day, 7u);
}

void invalid_bcd_digit_fails() {
    CivilTime out{1, 2, 3, 4, 5, 6};
    const RtcRegisters bad_sec{0x0A, 0x00, 0x12, 0x01, 0x01, 0x26, 0x20, 0x02};
    CHECK(!rtc_decode(bad_sec, out));
    CHECK_EQ(out.year, 1u);

    const RtcRegisters bad_hour{0x00, 0x00, 0x1F, 0x01, 0x01, 0x26, 0x20, 0x02};
    CHECK(!rtc_decode(bad_hour, out));

    const RtcRegisters bad_cent{0x00, 0x00, 0x12, 0x01, 0x01, 0x26, 0xA0, 0x02};
    CHECK(!rtc_decode(bad_cent, out));
}

void invalid_month_fails() {
    CivilTime out{};
    const RtcRegisters month_zero{0x00, 0x00, 0x12, 0x01, 0x00, 0x26, 0x20, 0x02};
    CHECK(!rtc_decode(month_zero, out));

    const RtcRegisters month_13{0x00, 0x00, 0x12, 0x01, 0x13, 0x26, 0x20, 0x02};
    CHECK(!rtc_decode(month_13, out));

    const RtcRegisters day_zero{0x00, 0x00, 0x12, 0x00, 0x01, 0x26, 0x20, 0x02};
    CHECK(!rtc_decode(day_zero, out));

    const RtcRegisters hour_24{0x00, 0x00, 0x24, 0x01, 0x01, 0x26, 0x20, 0x02};
    CHECK(!rtc_decode(hour_24, out));

    const RtcRegisters hour_12h_zero{0x00, 0x00, 0x00, 0x01, 0x01, 0x26, 0x20, 0x00};
    CHECK(!rtc_decode(hour_12h_zero, out));

    const RtcRegisters sec_60{60, 0, 12, 1, 1, 26, 20, 0x06};
    CHECK(!rtc_decode(sec_60, out));
}

}  // namespace

int main() {
    bcd_24h();
    binary_24h();
    bcd_12h_pm();
    twelve_am_is_zero();
    century_zero_is_2000s();
    invalid_bcd_digit_fails();
    invalid_month_fails();
    return check::finish("test_rtc_decode");
}
