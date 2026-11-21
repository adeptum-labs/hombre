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

#include "chrome_palette.hpp"

namespace hombre::chrome {

namespace {

constexpr u32 kText = 2;
constexpr u32 kShine = 3;
constexpr u32 kShadow = 4;
constexpr u32 kFill = 5;
constexpr u32 kFillText = 6;
constexpr u32 kBackground = 7;
constexpr u32 kBarDetail = 9;
constexpr u32 kBarBlock = 10;

constexpr u32 lighten(u32 rgb, u32 percent) {
    return detail::lerp_rgb(rgb, 0xFFFFFF, percent, 100);
}

constexpr u32 darken(u32 rgb, u32 percent) {
    return detail::lerp_rgb(rgb, 0x000000, percent, 100);
}

}

void palette_from_pens(const u32* pen_rgb, u32* out) {
    const u32 fill = pen_rgb[kFill];
    const u32 background = pen_rgb[kBackground];
    const u32 active_top = lighten(fill, 30);
    const u32 active_bottom = darken(fill, 30);
    for (u32 i = 0; i < 8; ++i) {
        out[kActiveBar0 - kDerivedBase + i] =
            detail::lerp_rgb(active_top, active_bottom, i, 7);
    }
    const u32 inactive_top = lighten(background, 15);
    const u32 inactive_bottom = darken(background, 10);
    for (u32 i = 0; i < 6; ++i) {
        out[kInactiveBar0 - kDerivedBase + i] =
            detail::lerp_rgb(inactive_top, inactive_bottom, i, 5);
    }
    out[kFrameDark - kDerivedBase] = pen_rgb[kShadow];
    out[kFrameLight - kDerivedBase] = pen_rgb[kShine];
    out[kCloseFill - kDerivedBase] = kChromePalette[kCloseFill - kChromeBase];
    out[kCloseRing - kDerivedBase] = kChromePalette[kCloseRing - kChromeBase];
    out[kKnob0 - kDerivedBase] = lighten(background, 40);
    out[kKnob1 - kDerivedBase] = lighten(background, 20);
    out[kKnob2 - kDerivedBase] = background;
    out[kTrack - kDerivedBase] = darken(background, 25);
    out[kTitleActive - kDerivedBase] = pen_rgb[kFillText];
    out[kTitleInactive - kDerivedBase] = pen_rgb[kText];
    for (u32 i = 0; i < kScreenBarSteps; ++i) {
        out[kScreenBar0 - kDerivedBase + i] = screen_bar_rgb(pen_rgb[kBarBlock], i);
    }
    out[kMenuPaperPen - kDerivedBase] = pen_rgb[kBarBlock];
    out[kMenuLabelPen - kDerivedBase] = pen_rgb[kBarDetail];
    out[kMenuLitPen - kDerivedBase] = pen_rgb[kFillText];
    out[kMenuGhostPen - kDerivedBase] =
        detail::lerp_rgb(pen_rgb[kBarBlock], pen_rgb[kBarDetail], 50, 100);
}

}