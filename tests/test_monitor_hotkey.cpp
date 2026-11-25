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

#include "../common/monitor_hotkey.hpp"

#include "check.hpp"

using namespace hombre;

namespace {

void f12_is_taken_out_and_counts_once_a_press() {
    MonitorHotkey hotkey;
    u8 report[8] = {0, 0, 0x04, MonitorHotkey::kUsageF12, 0x05};
    CHECK(hotkey.take(report));
    CHECK_EQ(report[2], 0x04);
    CHECK_EQ(report[3], 0);
    CHECK_EQ(report[4], 0x05);
    // Held on, it is still kept back but presses nothing again.
    u8 held[8] = {0, 0, MonitorHotkey::kUsageF12};
    CHECK(!hotkey.take(held));
    CHECK_EQ(held[2], 0);
    u8 released[8] = {};
    CHECK(!hotkey.take(released));
    u8 again[8] = {0, 0, MonitorHotkey::kUsageF12};
    CHECK(hotkey.take(again));
}

void other_keys_stay() {
    MonitorHotkey hotkey;
    u8 report[8] = {0x01, 0, 0x44, 0x46};
    CHECK(!hotkey.take(report));
    CHECK_EQ(report[0], 0x01);
    CHECK_EQ(report[2], 0x44);
    CHECK_EQ(report[3], 0x46);
}

}

int main() {
    f12_is_taken_out_and_counts_once_a_press();
    other_keys_stay();
    return check::finish("test_monitor_hotkey");
}
