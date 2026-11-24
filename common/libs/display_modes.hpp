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

#include "../pixel_format.hpp"
#include "../types.hpp"

// The display modes graphics.library knows: every standard mode of the
// default, NTSC and PAL monitors, with the size, depth and property flags
// their DisplayInfos carry, and after them the modes of the HOMBRE monitor:
// PC sizes with square pixels in 8, 16 and 24-bit, as a graphics card has
// them, which exist once the kernel has called configure_hombre.

namespace hombre::display_modes {

constexpr u32 kMonitorIdMask = 0xFFFF1000;
constexpr u32 kDefaultMonitorId = 0x00000;
constexpr u32 kNtscMonitorId = 0x11000;
constexpr u32 kPalMonitorId = 0x21000;
constexpr u32 kHombreMonitorId = 0x51001000;

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
constexpr u32 kIsForeign = 0x80000000;

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
    // A deep (chunky) mode of the HOMBRE monitor, with square pixels, and
    // the pixel format of its bitmaps.
    bool deep = false;
    bool square = false;
    PixFmt pixfmt = PixFmt::Lut8;
    // Walked by NextDisplayInfo. The HOMBRE modes of one format each are
    // known but not listed: the list has one mode a size, any_depth, whose
    // screens take the format of the depth asked for.
    bool listed = true;
    bool any_depth = false;
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

constexpr Mode kAmigaModes[] = {
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

constexpr u32 kAmigaModeCount = sizeof kAmigaModes / sizeof kAmigaModes[0];

// The sizes of the HOMBRE monitor; the index is part of the ModeID, so the
// order is fixed. The native size takes index kNativeSize when the
// framebuffer has a size this list lacks.
struct HombreSize {
    u16 width;
    u16 height;
};

constexpr HombreSize kHombreSizes[] = {
    {800, 600},   {1024, 768},  {1600, 1200}, {1280, 1024}, {1280, 800},
    {1440, 900},  {1680, 1050}, {1920, 1200}, {2560, 1600}, {1280, 720},
    {1366, 768},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3840, 2160},
};
constexpr u32 kHombreSizeCount = sizeof kHombreSizes / sizeof kHombreSizes[0];
constexpr u32 kNativeSize = 15;
constexpr u32 kHombreDepths = 3;
// The depth code of the listed mode of a size, whose depth picks the format.
constexpr u32 kAnyDepth = 3;
constexpr u32 kMaxHombreModes = (kHombreSizeCount + 1) * (kHombreDepths + 1);

constexpr u8 kHombreDepth[kHombreDepths] = {8, 16, 24};
constexpr PixFmt kHombreFormat[kHombreDepths] = {PixFmt::Lut8, PixFmt::Rgb16, PixFmt::Argb32};

constexpr u32 hombre_id(u32 size_index, u32 depth_code) {
    return kHombreMonitorId | (size_index << 4) | depth_code;
}

constexpr bool is_hombre(u32 id) { return (id & kMonitorIdMask) == kHombreMonitorId; }

namespace detail {

struct HombreTable {
    Mode modes[kMaxHombreModes];
    char names[kMaxHombreModes][32];
    u32 count;
};

inline HombreTable hombre_table{};

inline void put_number(char* out, u32& at, u32 value) {
    char digits[10];
    u32 n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n != 0) {
        out[at++] = digits[--n];
    }
}

// "HOMBRE:<width>x<height>" into name; returns its length.
inline u32 put_size_name(char* name, u32 width, u32 height) {
    u32 at = 0;
    for (const char* p = "HOMBRE:"; *p != '\0'; ++p) {
        name[at++] = *p;
    }
    put_number(name, at, width);
    name[at++] = 'x';
    put_number(name, at, height);
    name[at] = '\0';
    return at;
}

inline bool hombre_fits(u32 width, u32 height, u32 depth, u64 fast_bytes) {
    return u64{width} * height * bytes_per_pixel(kHombreFormat[depth]) <= fast_bytes / 4;
}

inline void add_hombre(u32 size_index, u32 width, u32 height, u64 fast_bytes) {
    HombreTable& table = hombre_table;
    u32 deepest = kHombreDepths;
    for (u32 depth = 0; depth < kHombreDepths; ++depth) {
        if (hombre_fits(width, height, depth, fast_bytes)) {
            deepest = depth;
        }
    }
    if (deepest == kHombreDepths) {
        return;
    }
    char* listed_name = table.names[table.count];
    put_size_name(listed_name, width, height);
    Mode& listed = table.modes[table.count++];
    listed = Mode{hombre_id(size_index, kAnyDepth), static_cast<u16>(width),
                  static_cast<u16>(height), kHombreDepth[deepest], 22, 22,
                  kIsForeign | kIsWb, listed_name, true, true, kHombreFormat[deepest]};
    listed.any_depth = true;
    for (u32 depth = 0; depth < kHombreDepths; ++depth) {
        if (!hombre_fits(width, height, depth, fast_bytes)) {
            continue;
        }
        char* name = table.names[table.count];
        u32 at = put_size_name(name, width, height);
        name[at++] = ' ';
        put_number(name, at, kHombreDepth[depth]);
        for (const char* p = "bit"; *p != '\0'; ++p) {
            name[at++] = *p;
        }
        name[at] = '\0';
        Mode& mode = table.modes[table.count++];
        mode = Mode{hombre_id(size_index, depth), static_cast<u16>(width),
                    static_cast<u16>(height), kHombreDepth[depth], 22, 22,
                    kIsForeign | kIsWb, name, true, true, kHombreFormat[depth]};
        mode.listed = false;
    }
}

}

// Lists the HOMBRE modes for a framebuffer of fb_width by fb_height and
// fast_bytes of fast memory: every listed size, and the framebuffer's own
// when it is not listed, each in 8, 16 and 24-bit, leaving out the ones
// whose screen alone takes more than a quarter of fast memory. A
// framebuffer of 0 by 0 adds no native size.
inline void configure_hombre(u32 fb_width, u32 fb_height, u64 fast_bytes) {
    detail::HombreTable& table = detail::hombre_table;
    table.count = 0;
    bool listed = fb_width == 0 || fb_height == 0;
    for (u32 i = 0; i < kHombreSizeCount; ++i) {
        detail::add_hombre(i, kHombreSizes[i].width, kHombreSizes[i].height, fast_bytes);
        listed = listed ||
                 (kHombreSizes[i].width == fb_width && kHombreSizes[i].height == fb_height);
    }
    if (!listed && fb_width <= 0xFFFF && fb_height <= 0xFFFF) {
        detail::add_hombre(kNativeSize, fb_width, fb_height, fast_bytes);
    }
}

inline u32 count() { return kAmigaModeCount + detail::hombre_table.count; }

inline const Mode& at(u32 index) {
    return index < kAmigaModeCount ? kAmigaModes[index]
                                   : detail::hombre_table.modes[index - kAmigaModeCount];
}

inline u32 index_of(u32 id) {
    for (u32 i = 0; i < count(); ++i) {
        if (at(i).id == id) {
            return i;
        }
    }
    return count();
}

inline const Mode* find(u32 id) {
    const u32 index = index_of(id);
    return index < count() ? &at(index) : nullptr;
}

// The mode a screen of depth bits opens in for the mode id: for a listed
// HOMBRE mode, its mode of the format the depth needs (8-bit up to 8 bits,
// 16-bit up to 16, 24-bit beyond), or the deepest it has; id itself for
// any other mode.
inline u32 resolve(u32 id, u32 depth) {
    const Mode* mode = find(id);
    if (mode == nullptr || !mode->any_depth) {
        return id;
    }
    u32 code = depth <= 8 ? 0 : depth <= 16 ? 1 : 2;
    while (code > 0 && find((id & ~0xFu) | code) == nullptr) {
        --code;
    }
    return (id & ~0xFu) | code;
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
