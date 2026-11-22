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

// The display modes graphics.library knows: every standard mode of the
// default, NTSC and PAL monitors, with the size, depth and property flags
// their DisplayInfos carry.

namespace hombre::display_modes {

constexpr u32 kMonitorIdMask = 0xFFFF1000;
constexpr u32 kDefaultMonitorId = 0x00000;
constexpr u32 kNtscMonitorId = 0x11000;
constexpr u32 kPalMonitorId = 0x21000;

constexpr u32 kLores = 0x0000;
constexpr u32 kLace = 0x0004;
constexpr u32 kLoresLace = kLores | kLace;
constexpr u32 kHires = 0x8000;
constexpr u32 kHiresLace = kHires | kLace;
constexpr u32 kSuperHires = 0x0020;
constexpr u32 kSuper = kHires | kSuperHires;
constexpr u32 kSuperLace = kSuper | kLace;
constexpr u32 kHam = 0x0800;
constexpr u32 kHamLace = kHam | kLace;
constexpr u32 kHiresHam = kHires | kHam;
constexpr u32 kHiresHamLace = kHiresHam | kLace;
constexpr u32 kSuperHam = kSuper | kHam;
constexpr u32 kSuperHamLace = kSuperHam | kLace;
constexpr u32 kExtraHalfBrite = 0x0080;
constexpr u32 kExtraHalfBriteLace = kExtraHalfBrite | kLace;
constexpr u32 kHiresEhb = kHires | kExtraHalfBrite;
constexpr u32 kHiresEhbLace = kHiresEhb | kLace;
constexpr u32 kSuperEhb = kSuper | kExtraHalfBrite;
constexpr u32 kSuperEhbLace = kSuperEhb | kLace;

constexpr u32 kIsLace = 0x1;
constexpr u32 kIsHam = 0x8;
constexpr u32 kIsEcs = 0x10;
constexpr u32 kIsPal = 0x20;
constexpr u32 kIsWb = 0x100;
constexpr u32 kIsExtraHalfBrite = 0x1000;
constexpr u32 kIsAa = 0x10000;

constexpr u32 kViewHires = 0x8000;
constexpr u32 kViewLace = 0x0004;
constexpr u32 kViewSuperHires = kSuperHires;
constexpr u32 kViewHam = 0x0800;
constexpr u32 kViewExtraHalfBrite = 0x0080;

struct Mode {
    u32 id;
    u16 width;
    u16 height;
    u8 max_depth;
    u16 res_x;
    u16 res_y;
    u32 flags;
    const char* name;
};

constexpr u32 mode_flags(u32 monitor, u32 key) {
    u32 flags = kIsAa;
    if ((key & kLace) != 0) {
        flags |= kIsLace;
    }
    if ((key & kHam) != 0) {
        flags |= kIsHam;
    }
    if ((key & kExtraHalfBrite) != 0) {
        flags |= kIsExtraHalfBrite;
    }
    if ((key & kSuperHires) != 0) {
        flags |= kIsEcs;
    }
    if ((key & (kHam | kExtraHalfBrite)) == 0) {
        flags |= kIsWb;
    }
    if (monitor == kPalMonitorId) {
        flags |= kIsPal;
    }
    return flags;
}

constexpr Mode make_mode(u32 monitor, u32 key, u16 width, u16 height, u8 max_depth,
                         u16 res_x, u16 res_y, const char* name) {
    return {monitor | key, width, height, max_depth, res_x, res_y, mode_flags(monitor, key),
            name};
}

constexpr Mode kModes[] = {
    make_mode(kDefaultMonitorId, kLores, 320, 200, 8, 44, 44, "NTSC:Low Res"),
    make_mode(kDefaultMonitorId, kLoresLace, 320, 400, 8, 44, 22, "NTSC:Low Res Laced"),
    make_mode(kDefaultMonitorId, kHires, 640, 200, 8, 22, 44, "NTSC:High Res"),
    make_mode(kDefaultMonitorId, kHiresLace, 640, 400, 8, 22, 22, "NTSC:High Res Laced"),
    make_mode(kDefaultMonitorId, kSuper, 1280, 200, 8, 11, 44, "NTSC:Super-High Res"),
    make_mode(kDefaultMonitorId, kSuperLace, 1280, 400, 8, 11, 22,
              "NTSC:Super-High Res Laced"),
    make_mode(kDefaultMonitorId, kHam, 320, 200, 8, 44, 44, "NTSC:Low Res HAM"),
    make_mode(kDefaultMonitorId, kHamLace, 320, 400, 8, 44, 22, "NTSC:Low Res Laced HAM"),
    make_mode(kDefaultMonitorId, kHiresHam, 640, 200, 8, 22, 44, "NTSC:High Res HAM"),
    make_mode(kDefaultMonitorId, kHiresHamLace, 640, 400, 8, 22, 22,
              "NTSC:High Res Laced HAM"),
    make_mode(kDefaultMonitorId, kSuperHam, 1280, 200, 8, 11, 44, "NTSC:Super-High Res HAM"),
    make_mode(kDefaultMonitorId, kSuperHamLace, 1280, 400, 8, 11, 22,
              "NTSC:Super-High Res Laced HAM"),
    make_mode(kDefaultMonitorId, kExtraHalfBrite, 320, 200, 6, 44, 44, "NTSC:Low Res EHB"),
    make_mode(kDefaultMonitorId, kExtraHalfBriteLace, 320, 400, 6, 44, 22,
              "NTSC:Low Res Laced EHB"),
    make_mode(kDefaultMonitorId, kHiresEhb, 640, 200, 6, 22, 44, "NTSC:High Res EHB"),
    make_mode(kDefaultMonitorId, kHiresEhbLace, 640, 400, 6, 22, 22,
              "NTSC:High Res Laced EHB"),
    make_mode(kDefaultMonitorId, kSuperEhb, 1280, 200, 6, 11, 44, "NTSC:Super-High Res EHB"),
    make_mode(kDefaultMonitorId, kSuperEhbLace, 1280, 400, 6, 11, 22,
              "NTSC:Super-High Res Laced EHB"),

    make_mode(kNtscMonitorId, kLores, 320, 200, 8, 44, 44, "NTSC:Low Res"),
    make_mode(kNtscMonitorId, kLoresLace, 320, 400, 8, 44, 22, "NTSC:Low Res Laced"),
    make_mode(kNtscMonitorId, kHires, 640, 200, 8, 22, 44, "NTSC:High Res"),
    make_mode(kNtscMonitorId, kHiresLace, 640, 400, 8, 22, 22, "NTSC:High Res Laced"),
    make_mode(kNtscMonitorId, kSuper, 1280, 200, 8, 11, 44, "NTSC:Super-High Res"),
    make_mode(kNtscMonitorId, kSuperLace, 1280, 400, 8, 11, 22, "NTSC:Super-High Res Laced"),
    make_mode(kNtscMonitorId, kHam, 320, 200, 8, 44, 44, "NTSC:Low Res HAM"),
    make_mode(kNtscMonitorId, kHamLace, 320, 400, 8, 44, 22, "NTSC:Low Res Laced HAM"),
    make_mode(kNtscMonitorId, kHiresHam, 640, 200, 8, 22, 44, "NTSC:High Res HAM"),
    make_mode(kNtscMonitorId, kHiresHamLace, 640, 400, 8, 22, 22, "NTSC:High Res Laced HAM"),
    make_mode(kNtscMonitorId, kSuperHam, 1280, 200, 8, 11, 44, "NTSC:Super-High Res HAM"),
    make_mode(kNtscMonitorId, kSuperHamLace, 1280, 400, 8, 11, 22,
              "NTSC:Super-High Res Laced HAM"),
    make_mode(kNtscMonitorId, kExtraHalfBrite, 320, 200, 6, 44, 44, "NTSC:Low Res EHB"),
    make_mode(kNtscMonitorId, kExtraHalfBriteLace, 320, 400, 6, 44, 22,
              "NTSC:Low Res Laced EHB"),
    make_mode(kNtscMonitorId, kHiresEhb, 640, 200, 6, 22, 44, "NTSC:High Res EHB"),
    make_mode(kNtscMonitorId, kHiresEhbLace, 640, 400, 6, 22, 22, "NTSC:High Res Laced EHB"),
    make_mode(kNtscMonitorId, kSuperEhb, 1280, 200, 6, 11, 44, "NTSC:Super-High Res EHB"),
    make_mode(kNtscMonitorId, kSuperEhbLace, 1280, 400, 6, 11, 22,
              "NTSC:Super-High Res Laced EHB"),

    make_mode(kPalMonitorId, kLores, 320, 256, 8, 44, 44, "PAL:Low Res"),
    make_mode(kPalMonitorId, kLoresLace, 320, 512, 8, 44, 22, "PAL:Low Res Laced"),
    make_mode(kPalMonitorId, kHires, 640, 256, 8, 22, 44, "PAL:High Res"),
    make_mode(kPalMonitorId, kHiresLace, 640, 512, 8, 22, 22, "PAL:High Res Laced"),
    make_mode(kPalMonitorId, kSuper, 1280, 256, 8, 11, 44, "PAL:Super-High Res"),
    make_mode(kPalMonitorId, kSuperLace, 1280, 512, 8, 11, 22, "PAL:Super-High Res Laced"),
    make_mode(kPalMonitorId, kHam, 320, 256, 8, 44, 44, "PAL:Low Res HAM"),
    make_mode(kPalMonitorId, kHamLace, 320, 512, 8, 44, 22, "PAL:Low Res Laced HAM"),
    make_mode(kPalMonitorId, kHiresHam, 640, 256, 8, 22, 44, "PAL:High Res HAM"),
    make_mode(kPalMonitorId, kHiresHamLace, 640, 512, 8, 22, 22, "PAL:High Res Laced HAM"),
    make_mode(kPalMonitorId, kSuperHam, 1280, 256, 8, 11, 44, "PAL:Super-High Res HAM"),
    make_mode(kPalMonitorId, kSuperHamLace, 1280, 512, 8, 11, 22,
              "PAL:Super-High Res Laced HAM"),
    make_mode(kPalMonitorId, kExtraHalfBrite, 320, 256, 6, 44, 44, "PAL:Low Res EHB"),
    make_mode(kPalMonitorId, kExtraHalfBriteLace, 320, 512, 6, 44, 22, "PAL:Low Res Laced EHB"),
    make_mode(kPalMonitorId, kHiresEhb, 640, 256, 6, 22, 44, "PAL:High Res EHB"),
    make_mode(kPalMonitorId, kHiresEhbLace, 640, 512, 6, 22, 22, "PAL:High Res Laced EHB"),
    make_mode(kPalMonitorId, kSuperEhb, 1280, 256, 6, 11, 44, "PAL:Super-High Res EHB"),
    make_mode(kPalMonitorId, kSuperEhbLace, 1280, 512, 6, 11, 22,
              "PAL:Super-High Res Laced EHB"),
};

constexpr u32 count() { return sizeof kModes / sizeof kModes[0]; }

constexpr const Mode& at(u32 index) { return kModes[index]; }

constexpr u32 index_of(u32 id) {
    for (u32 i = 0; i < count(); ++i) {
        if (kModes[i].id == id) {
            return i;
        }
    }
    return count();
}

constexpr const Mode* find(u32 id) {
    const u32 index = index_of(id);
    return index < count() ? &kModes[index] : nullptr;
}

constexpr u32 from_view_modes(u16 view_modes) {
    u32 key = kLores;
    if ((view_modes & kViewSuperHires) != 0) {
        key = kSuper;
    } else if ((view_modes & kViewHires) != 0) {
        key = kHires;
    }
    if ((view_modes & kViewLace) != 0) {
        key |= kLace;
    }
    if ((view_modes & kViewHam) != 0) {
        key |= kHam;
    }
    if ((view_modes & kViewExtraHalfBrite) != 0) {
        key |= kExtraHalfBrite;
    }
    return kDefaultMonitorId | key;
}

constexpr u32 preferred(u32 monitor) { return monitor | kHiresLace; }

}
