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

// F12 opens and closes the system monitor. No Amiga keyboard has it, so it
// is kept from everything else.
class MonitorHotkey {
public:
    static constexpr u8 kUsageF12 = 0x45;

    // Takes F12 out of a boot keyboard report; true when it was not held in
    // the report before.
    bool take(u8 (&report)[8]) {
        bool held = false;
        for (u32 i = 2; i < 8; ++i) {
            if (report[i] == kUsageF12) {
                held = true;
                report[i] = 0;
            }
        }
        const bool pressed = held && !held_;
        held_ = held;
        return pressed;
    }

private:
    bool held_ = false;
};

}
