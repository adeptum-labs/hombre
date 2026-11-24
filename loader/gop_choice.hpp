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

#include "../common/types.hpp"

// Which of the firmware's graphics modes the loader asks for: the one with
// the most pixels, the wider one of two equal, among those with a 32-bit
// framebuffer Hombre can draw in. Kept free of firmware calls so the host
// tests can check it.

namespace hombre {

struct GopModeSize {
    u32 width;
    u32 height;
    bool usable;  // a linear BGRX or RGBX framebuffer
};

// A shape monitors have: from 5:4 to 21:9. Firmware also lists square and
// other odd sizes no screen shows natively.
constexpr bool monitor_shaped(const GopModeSize& mode) {
    return u64{mode.width} * 4 >= u64{mode.height} * 5 && u64{mode.width} * 9 <= u64{mode.height} * 21;
}

// The index of the best usable mode of count, current when none is usable:
// the largest monitor-shaped mode, or the largest of any shape when there is
// none.
inline u32 best_gop_mode(const GopModeSize* modes, u32 count, u32 current) {
    bool any_shaped = false;
    for (u32 i = 0; i < count; ++i) {
        any_shaped = any_shaped || (modes[i].usable && monitor_shaped(modes[i]));
    }
    u32 best = current;
    u64 best_pixels = 0;
    u32 best_width = 0;
    for (u32 i = 0; i < count; ++i) {
        if (!modes[i].usable || (any_shaped && !monitor_shaped(modes[i]))) {
            continue;
        }
        const u64 pixels = u64{modes[i].width} * modes[i].height;
        if (pixels > best_pixels || (pixels == best_pixels && modes[i].width > best_width)) {
            best = i;
            best_pixels = pixels;
            best_width = modes[i].width;
        }
    }
    return best;
}

}
