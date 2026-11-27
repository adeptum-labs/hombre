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

// The top four pens of a screen of 16 colours or more, 252..255 on eight
// planes, take the colours of pens 4..7: an icon of eight colours names
// them as its colours 4..7, as the eight-colour icons of 3.x do.
constexpr u8 kTopPenBase = 252;
constexpr u32 kTopPens = 4;

// The top pen of the four, n of 0..3, which has the colour of pen 4 + n.
constexpr u8 top_pen(u8 pen) {
    return static_cast<u8>(kTopPenBase + pen);
}

// The chrome pens above, kScreenBar0 to 255, are the pens of a screen of
// eight planes. A screen of fewer planes draws them through physical_pen,
// which gives each its place in the screen's 2^planes pens. Seven and six
// planes keep the whole range at the top of the ColorMap. Five have room for
// the four top pens there and 12 pens below them,
// which the 32 other chrome pens share. The pens below kScreenBar0 are the
// base palette's and stay where they are.
constexpr u32 kChromePens = 256 - kScreenBar0;

namespace five_planes {

// The 12 pens: the two halves of the screen bar; the menu's paper, which the
// frame's light edge shares, and its ink, which the dark edge and every title
// share; the menu's ghost; three of the active bar and two of the inactive
// one, which the track shares; the light and the plain knob.
constexpr u8 kBarLight = 16;
constexpr u8 kBarDark = 17;
constexpr u8 kPaper = 18;
constexpr u8 kInk = 19;
constexpr u8 kGhost = 20;
constexpr u8 kActive0 = 21;
constexpr u8 kActive1 = 22;
constexpr u8 kActive2 = 23;
constexpr u8 kInactive0 = 24;
constexpr u8 kInactive1 = 25;
constexpr u8 kKnobLight = 26;
constexpr u8 kKnobBase = 27;
constexpr u32 kFirstPen = kBarLight;
constexpr u32 kPens = kKnobBase - kFirstPen + 1;

// The pen of each chrome pen from kScreenBar0 to kTitleInactive. The screen
// bar goes two and two, the active bar by 3, 2 and 3 steps, the inactive bar
// by 3 and 3, and the knob by 2 and 1.
constexpr u8 kPen[kTopPenBase - kScreenBar0] = {
    // The screen bar.
    kBarLight, kBarLight, kBarDark, kBarDark,
    // The menu's paper, label, lit and ghost.
    kPaper, kInk, kInk, kGhost,
    // The active bar.
    kActive0, kActive0, kActive0, kActive1, kActive1, kActive2, kActive2, kActive2,
    // The inactive bar.
    kInactive0, kInactive0, kInactive0, kInactive1, kInactive1, kInactive1,
    // The frame's dark and light edges, the close button's fill and ring.
    kInk, kPaper, kPaper, kInk,
    // The knob, the track and the active and inactive titles.
    kKnobLight, kKnobLight, kKnobBase, kInactive1, kInk, kInk,
};

// The chrome pen whose colour each pen takes: the middle one of the pens it
// stands for, the later of two, or the only one of a menu's.
constexpr u8 kSource[kPens] = {
    kScreenBar1, kScreenBar3, kMenuPaperPen, kMenuLabelPen, kMenuGhostPen, kActiveBar1,
    kActiveBar4, kActiveBar6, kInactiveBar1, kInactiveBar4, kKnob1, kKnob2,
};

}

// The pen of the chrome pen logical on a screen of depth planes.
constexpr u8 physical_pen(u8 logical, u32 depth) {
    if (logical < kScreenBar0 || depth < 5) {
        return logical;
    }
    if (depth == 5 && logical < kTopPenBase) {
        return five_planes::kPen[logical - kScreenBar0];
    }
    const u32 pens = depth < 8 ? 1u << depth : 256;
    return static_cast<u8>(logical - (256 - pens));
}

// The chrome pen whose colour the pen physical holds on a screen of depth
// planes; 0 for a pen below the chrome range, where the screen keeps its base
// and colour cube palette.
constexpr u32 logical_pen(u32 physical, u32 depth) {
    if (depth < 5 || depth > 8 || physical >= (1u << depth)) {
        return 0;
    }
    const u32 pens = 1u << depth;
    const bool top = physical + kTopPens >= pens;
    if (top || (depth >= 6 && physical + kChromePens >= pens)) {
        return physical + (256 - pens);
    }
    if (depth == 5 && physical >= five_planes::kFirstPen) {
        return five_planes::kSource[physical - five_planes::kFirstPen];
    }
    return 0;
}

namespace detail {

constexpr bool pen_map_holds() {
    for (u32 depth = 5; depth <= 8; ++depth) {
        const u32 pens = 1u << depth;
        const u32 first = depth == 5 ? five_planes::kFirstPen : pens - kChromePens;
        for (u32 logical = kScreenBar0; logical < 256; ++logical) {
            const u32 physical = physical_pen(static_cast<u8>(logical), depth);
            // Every chrome pen lands in the chrome range of the screen, the
            // top pens on its top four pens, and takes the colour of a chrome
            // pen that lands on the same pen.
            const bool top = logical >= kTopPenBase;
            if (physical < first || physical >= pens ||
                (physical + kTopPens >= pens) != top ||
                physical_pen(static_cast<u8>(logical_pen(physical, depth)), depth) != physical) {
                return false;
            }
        }
        for (u32 pen = 0; pen < kTopPens; ++pen) {
            if (physical_pen(top_pen(static_cast<u8>(pen)), depth) !=
                pens - kTopPens + pen) {
                return false;
            }
        }
    }
    return true;
}

}

static_assert(detail::pen_map_holds());
static_assert(physical_pen(kScreenBar0, 7) == 92 && physical_pen(255, 7) == 127);
static_assert(physical_pen(kScreenBar0, 6) == 28 && physical_pen(255, 6) == 63);
static_assert(physical_pen(kScreenBar0, 8) == kScreenBar0 && physical_pen(255, 8) == 255);
static_assert(physical_pen(top_pen(0), 5) == 28 && physical_pen(top_pen(3), 5) == 31);
static_assert(physical_pen(3, 5) == 3 && physical_pen(15, 8) == 15);

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
// through the window chrome, pens kDerivedBase to kTopPenBase - 1.
constexpr u32 kDerivedBase = kScreenBar0;
constexpr u32 kDerivedCount = kTopPenBase - kDerivedBase;

static_assert(kChromePalette[kActiveBar0 - kChromeBase] == 0x6D9BE0u);
static_assert(kChromePalette[kActiveBar7 - kChromeBase] == 0x2F5DA6u);
static_assert(kChromePalette[kInactiveBar0 - kChromeBase] == 0xE6E8ECu);
static_assert(kChromePalette[kInactiveBar5 - kChromeBase] == 0xBFC3CAu);
static_assert(kChromePalette[kTitleInactive - kChromeBase] == 0x4A4F57u);

// Computes the colours of pens kDerivedBase..kTopPenBase - 1 from
// the colours of a screen's DrawInfo pens, in DrawInfo's order in pen_rgb:
// the bars shade the FILLPEN and BACKGROUNDPEN colours, the frames take
// SHADOWPEN and SHINEPEN, and the screen bar and menu take BARBLOCKPEN and
// BARDETAILPEN.
void palette_from_pens(const u32* pen_rgb, u32* out);

}
