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

#include "../common/boot_info.hpp"
#include "framebuffer.hpp"

namespace hombre {

// The framebuffer the loader handed over, seen through the direct map. Only
// meaningful when info.fb.phys is not 0.
inline Framebuffer make_framebuffer(const BootInfo& info) {
    return Framebuffer(reinterpret_cast<u8*>(info.direct_map_base + info.fb.phys), info.fb.width,
                       info.fb.height, info.fb.pitch_bytes, info.fb.format);
}

}
