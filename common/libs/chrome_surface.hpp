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

#include "../types.hpp"
#include "gfx.hpp"

namespace hombre::chrome {

// What chrome draws on; coordinates are the window's.
struct Surface {
    void* ctx;
    u32 depth;
    void (*fill)(void* ctx, const Rect& box, u8 pen);
    u32 (*text_fit)(void* ctx, u32 font, const u8* chars, u32 count, i32 room);
    i32 (*text_width)(void* ctx, u32 font, const u8* chars, u32 count);
    void (*text)(void* ctx, u32 font, i32 x, i32 baseline, u8 pen, const u8* chars, u32 count);
};

struct Title {
    const u8* chars = nullptr;
    u32 length = 0;
};

}
