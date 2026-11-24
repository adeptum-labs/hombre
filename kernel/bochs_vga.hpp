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

#include "framebuffer.hpp"
#include "mmio.hpp"

// The display adapter of QEMU's standard VGA and of VirtualBox, through the
// Bochs VBE ("DISPI") registers: unlike the firmware's framebuffer, its
// resolution can change while the system runs, and QEMU's window follows.

namespace hombre {

// A mode the adapter can show: within its largest size and its video
// memory at 4 bytes a pixel.
constexpr bool bochs_mode_fits(u32 width, u32 height, u32 max_width, u32 max_height,
                               u64 vram_bytes) {
    return width != 0 && height != 0 && width <= max_width && height <= max_height &&
           u64{width} * height * 4 <= vram_bytes;
}

static_assert(bochs_mode_fits(1920, 1080, 16000, 12000, 16u << 20));
static_assert(!bochs_mode_fits(3840, 2160, 16000, 12000, 16u << 20));
static_assert(!bochs_mode_fits(4096, 768, 2560, 1600, 64u << 20));
static_assert(!bochs_mode_fits(0, 768, 2560, 1600, 64u << 20));

// Finds the adapter (PCI 1234:1111), checks its DISPI interface and maps
// its video memory. False, leaving nothing set up, without one.
bool bochs_vga_init(MmioMapper& mmio);

bool bochs_vga_present();

// Switches to width by height in 32-bit pixels and gives the framebuffer
// that then shows. False, the mode unchanged, for a size it cannot show.
bool bochs_vga_set_mode(u32 width, u32 height, Framebuffer& out);

}
