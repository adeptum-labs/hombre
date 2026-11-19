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

constexpr u8 kChromeBase = 232;
constexpr u32 kChromeCount = 24;

constexpr u8 kActiveBar0 = 232;
constexpr u8 kActiveBar1 = 233;
constexpr u8 kActiveBar2 = 234;
constexpr u8 kActiveBar3 = 235;
constexpr u8 kActiveBar4 = 236;
constexpr u8 kActiveBar5 = 237;
constexpr u8 kActiveBar6 = 238;
constexpr u8 kActiveBar7 = 239;

constexpr u8 kInactiveBar0 = 240;
constexpr u8 kInactiveBar1 = 241;
constexpr u8 kInactiveBar2 = 242;
constexpr u8 kInactiveBar3 = 243;
constexpr u8 kInactiveBar4 = 244;
constexpr u8 kInactiveBar5 = 245;

constexpr u8 kFrameDark = 246;
constexpr u8 kFrameLight = 247;
constexpr u8 kCloseFill = 248;
constexpr u8 kCloseRing = 249;
constexpr u8 kKnob0 = 250;
constexpr u8 kKnob1 = 251;
constexpr u8 kKnob2 = 252;
constexpr u8 kTrack = 253;
constexpr u8 kTitleActive = 254;
constexpr u8 kTitleInactive = 255;

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

static_assert(kChromePalette[kActiveBar0 - kChromeBase] == 0x6D9BE0u);
static_assert(kChromePalette[kActiveBar7 - kChromeBase] == 0x2F5DA6u);
static_assert(kChromePalette[kInactiveBar0 - kChromeBase] == 0xE6E8ECu);
static_assert(kChromePalette[kInactiveBar5 - kChromeBase] == 0xBFC3CAu);
static_assert(kChromePalette[kTitleInactive - kChromeBase] == 0x4A4F57u);

}
