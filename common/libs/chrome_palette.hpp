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

namespace hombre::chrome {

// The screen title bar's shading, from light at its top to dark at its
// bottom, round the BARBLOCKPEN colour.
constexpr u8 kScreenBar0 = 220;
constexpr u8 kScreenBar1 = 221;
constexpr u8 kScreenBar2 = 222;
constexpr u8 kScreenBar3 = 223;
constexpr u32 kScreenBarSteps = 4;

constexpr u8 kMenuPaperPen = 224;
constexpr u8 kMenuLabelPen = 225;
constexpr u8 kMenuLitPen = 226;
constexpr u8 kMenuGhostPen = 227;

constexpr u8 kChromeBase = 228;
constexpr u32 kChromeCount = 24;

// The pens of a DrawInfo, in its order: DETAILPEN, BLOCKPEN, TEXTPEN,
// SHINEPEN, SHADOWPEN, FILLPEN, FILLTEXTPEN, BACKGROUNDPEN,
// HIGHLIGHTTEXTPEN, BARDETAILPEN, BARBLOCKPEN and BARTRIMPEN.
constexpr u32 kDrawInfoPens = 12;

constexpr u8 kActiveBar0 = 228;
constexpr u8 kActiveBar1 = 229;
constexpr u8 kActiveBar2 = 230;
constexpr u8 kActiveBar3 = 231;
constexpr u8 kActiveBar4 = 232;
constexpr u8 kActiveBar5 = 233;
constexpr u8 kActiveBar6 = 234;
constexpr u8 kActiveBar7 = 235;

constexpr u8 kInactiveBar0 = 236;
constexpr u8 kInactiveBar1 = 237;
constexpr u8 kInactiveBar2 = 238;
constexpr u8 kInactiveBar3 = 239;
constexpr u8 kInactiveBar4 = 240;
constexpr u8 kInactiveBar5 = 241;

constexpr u8 kFrameDark = 242;
constexpr u8 kFrameLight = 243;
constexpr u8 kCloseFill = 244;
constexpr u8 kCloseRing = 245;
constexpr u8 kKnob0 = 246;
constexpr u8 kKnob1 = 247;
constexpr u8 kKnob2 = 248;
constexpr u8 kTrack = 249;
constexpr u8 kTitleActive = 250;
constexpr u8 kTitleInactive = 251;

// COMPLEMENT inverts every bit of an eight-plane pixel, so pen n draws in
// pen 0xFF - n. Pens 252..255 take the colours of 0..3, so pen 0 turns the
// colour of 3 and pen 1 that of 2, as a complemented four-colour image
// does on two planes, where 0 and 3 swap and 1 and 2 swap.
constexpr u8 kComplementMirrorBase = 252;
constexpr u32 kComplementMirrorPens = 4;

// The top pen that keeps the colour of pen, one of 0..3.
constexpr u8 mirror_pen(u8 pen) {
    return static_cast<u8>(kComplementMirrorBase + pen);
}

namespace detail {

constexpr u32 lerp_channel(u32 a, u32 b, u32 step, u32 steps) {
    return (a * (steps - step) + b * step + steps / 2) / steps;
}

constexpr u32 lerp_rgb(u32 a, u32 b, u32 step, u32 steps) {
    const u32 r = lerp_channel((a >> 16) & 0xFF, (b >> 16) & 0xFF, step, steps);
    const u32 g = lerp_channel((a >> 8) & 0xFF, (b >> 8) & 0xFF, step, steps);
    const u32 bl = lerp_channel(a & 0xFF, b & 0xFF, step, steps);
    return (r << 16) | (g << 8) | bl;
}

}

// How far the screen bar's shading reaches above and below the BARBLOCKPEN
// colour, in percent towards white and black.
constexpr u32 kScreenBarLighten = 20;
constexpr u32 kScreenBarDarken = 18;

// The screen bar's colours while a screen still has the 3.1 default pens,
// round BARBLOCKPEN, pen 2 of the base palette.
constexpr u32 screen_bar_rgb(u32 block, u32 step) {
    const u32 top = detail::lerp_rgb(block, 0xFFFFFF, kScreenBarLighten, 100);
    const u32 bottom = detail::lerp_rgb(block, 0x000000, kScreenBarDarken, 100);
    return detail::lerp_rgb(top, bottom, step, kScreenBarSteps - 1);
}

// The menu pens' colours while a screen still has the 3.1 default pens:
// BARBLOCKPEN and BARDETAILPEN are pens 2 and 1 of the base palette, and
// the ghost is half of each.
constexpr u32 kMenuPalette[4] = {
    0xFFFFFF,
    0x1C1E22,
    0x1C1E22,
    detail::lerp_rgb(0xFFFFFF, 0x1C1E22, 50, 100),
};

constexpr u32 kChromePalette[kChromeCount] = {
    detail::lerp_rgb(0x6D9BE0, 0x2F5DA6, 0, 7),
    detail::lerp_rgb(0x6D9BE0, 0x2F5DA6, 1, 7),
    detail::lerp_rgb(0x6D9BE0, 0x2F5DA6, 2, 7),
    detail::lerp_rgb(0x6D9BE0, 0x2F5DA6, 3, 7),
    detail::lerp_rgb(0x6D9BE0, 0x2F5DA6, 4, 7),
    detail::lerp_rgb(0x6D9BE0, 0x2F5DA6, 5, 7),
    detail::lerp_rgb(0x6D9BE0, 0x2F5DA6, 6, 7),
    detail::lerp_rgb(0x6D9BE0, 0x2F5DA6, 7, 7),
    detail::lerp_rgb(0xE6E8EC, 0xBFC3CA, 0, 5),
    detail::lerp_rgb(0xE6E8EC, 0xBFC3CA, 1, 5),
    detail::lerp_rgb(0xE6E8EC, 0xBFC3CA, 2, 5),
    detail::lerp_rgb(0xE6E8EC, 0xBFC3CA, 3, 5),
    detail::lerp_rgb(0xE6E8EC, 0xBFC3CA, 4, 5),
    detail::lerp_rgb(0xE6E8EC, 0xBFC3CA, 5, 5),
    0x2A2D33,
    0xF4F5F7,
    0xE0605A,
    0x9A3330,
    0xF2F4F7,
    0xD7DBE1,
    0xB8BEC7,
    0x8E949D,
    0xFFFFFF,
    0x4A4F57,
};

// The colours palette_from_pens computes: the screen bar and menu pens
// through the window chrome, pens kDerivedBase to kComplementMirrorBase - 1.
constexpr u32 kDerivedBase = kScreenBar0;
constexpr u32 kDerivedCount = kComplementMirrorBase - kDerivedBase;

static_assert(kChromePalette[kActiveBar0 - kChromeBase] == 0x6D9BE0u);
static_assert(kChromePalette[kActiveBar7 - kChromeBase] == 0x2F5DA6u);
static_assert(kChromePalette[kInactiveBar0 - kChromeBase] == 0xE6E8ECu);
static_assert(kChromePalette[kInactiveBar5 - kChromeBase] == 0xBFC3CAu);
static_assert(kChromePalette[kTitleInactive - kChromeBase] == 0x4A4F57u);

// Computes the colours of pens kDerivedBase..kComplementMirrorBase - 1 from
// the colours of a screen's DrawInfo pens, in DrawInfo's order in pen_rgb:
// the bars shade the FILLPEN and BACKGROUNDPEN colours, the frames take
// SHADOWPEN and SHINEPEN, and the screen bar and menu take BARBLOCKPEN and
// BARDETAILPEN.
void palette_from_pens(const u32* pen_rgb, u32* out);

}
