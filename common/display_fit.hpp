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

// Where a screen of square pixels goes on the framebuffer: as large as it
// fits with its shape kept, centred, one to one when the sizes match.

namespace hombre {

struct Fit {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
};

constexpr Fit fit_square(u32 fb_width, u32 fb_height, u32 mode_width, u32 mode_height) {
    if (mode_width == 0 || mode_height == 0 || fb_width == 0 || fb_height == 0) {
        return Fit{0, 0, fb_width, fb_height};
    }
    // Full width, if the height that gives fits; else full height.
    u64 width = fb_width;
    u64 height = u64{fb_width} * mode_height / mode_width;
    if (height > fb_height) {
        height = fb_height;
        width = u64{fb_height} * mode_width / mode_height;
    }
    return Fit{static_cast<u32>((fb_width - width) / 2), static_cast<u32>((fb_height - height) / 2),
               static_cast<u32>(width), static_cast<u32>(height)};
}

}
