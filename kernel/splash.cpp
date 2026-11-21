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

#include "splash.hpp"

#include "../common/splash.hpp"
#include "framebuffer.hpp"

// The image tools/mksplash.py writes, read straight from the object file.
asm(".section .rodata\n"
    ".balign 16\n"
    ".global splash_rle_start\n"
    "splash_rle_start:\n"
    ".incbin \"assets/splash.rle\"\n"
    ".global splash_rle_end\n"
    "splash_rle_end:\n"
    ".previous\n");

extern "C" {
extern const hombre::u8 splash_rle_start[];
extern const hombre::u8 splash_rle_end[];
}

namespace hombre {

namespace {

constexpr u32 kBlue = 0x3871C0;
constexpr u32 kOrange = 0xF08C1E;
constexpr u32 kEmpty = 0x282830;

constinit SplashImage image{};
constinit Framebuffer screen{};
constinit u32 shown = 0;
constinit bool active = false;

void paint_block(u32 block, bool filled) {
    i32 x0 = 0;
    i32 y0 = 0;
    i32 x1 = 0;
    i32 y1 = 0;
    image.bar_rect(screen.width(), screen.height(), block, x0, y0, x1, y1);
    // A quarter of each block's width is the gap before the next, as in the
    // drawn bar.
    x1 -= (x1 - x0 + 1) / 4;
    if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
        return;
    }
    const u32 colour = filled ? ((block & 1) == 0 ? kBlue : kOrange) : kEmpty;
    screen.fill_rect(static_cast<u64>(x0), static_cast<u64>(y0),
                     static_cast<u64>(x1 - x0 + 1), static_cast<u64>(y1 - y0 + 1),
                     colour);
}

void paint_blocks(u32 first, u32 end, bool filled) {
    for (u32 block = first; block < end; ++block) {
        paint_block(block, filled);
    }
}

}

bool splash_show(Framebuffer& fb) {
    const u8* const start = splash_rle_start;
    const u8* const end = splash_rle_end;
    const usize bytes = static_cast<usize>(end - start);
    if (!image.parse(start, static_cast<u32>(bytes))) {
        return false;
    }
    fb.fill(0x000000);
    const u32 columns =
        fb.width() < SplashImage::kMaxRowWidth ? fb.width() : SplashImage::kMaxRowWidth;
    image.draw(fb.width(), fb.height(), [&fb, columns](u32 y, const u32* row) {
        fb.put_row(0, y, row, columns, 1);
    });
    screen = fb;
    shown = 0;
    active = true;
    paint_blocks(0, image.block_count(), false);
    return true;
}

void splash_progress(u32 done, u32 total) {
    if (!active || total == 0) {
        return;
    }
    const u32 blocks = image.block_count();
    u32 filled = static_cast<u32>(static_cast<u64>(done) * blocks / total);
    if (filled > blocks) {
        filled = blocks;
    }
    if (filled > shown) {
        paint_blocks(shown, filled, true);
    } else {
        paint_blocks(filled, shown, false);
    }
    shown = filled;
}

void splash_end() { active = false; }

bool splash_active() { return active; }

}
