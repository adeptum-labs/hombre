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
#include "native.hpp"

// Tag list walks the native libraries share.
//
// TAG_IGNORE is passed over, TAG_MORE continues at the list in its ti_Data,
// TAG_SKIP passes over itself and ti_Data more items, and TAG_DONE or a
// null list ends the list. A walk visits at most kMaxTagItems items, so a
// list that loops cannot hang the host.

namespace hombre::tags {

constexpr u32 kTagDone = 0;
constexpr u32 kTagIgnore = 1;
constexpr u32 kTagMore = 2;
constexpr u32 kTagSkip = 3;
constexpr u32 kItemBytes = 8;
constexpr u32 kMaxTagItems = 65536;

// The next item from item on that is no control tag, with item left after
// it; 0 at the end, on a bad guest address or when budget runs out.
inline u32 next(native::Bus& b, u32& item, u32& budget) {
    for (; budget != 0; --budget) {
        if (item == 0) {
            return 0;
        }
        const u32 tag = b.r32(item);
        if (!b.ok()) {
            return 0;
        }
        switch (tag) {
        case kTagDone:
            return 0;
        case kTagIgnore:
            item += kItemBytes;
            break;
        case kTagMore:
            item = b.r32(item + 4);
            break;
        case kTagSkip:
            item += kItemBytes * (b.r32(item + 4) + 1);
            break;
        default: {
            const u32 found = item;
            item += kItemBytes;
            --budget;
            return found;
        }
        }
    }
    return 0;
}

// The first item of the list with the tag, 0 if it has none.
inline u32 find(native::Bus& b, u32 tag, u32 list) {
    u32 budget = kMaxTagItems;
    for (;;) {
        const u32 item = next(b, list, budget);
        if (item == 0) {
            return 0;
        }
        if (b.r32(item) == tag) {
            return b.ok() ? item : 0;
        }
    }
}

// The data of the tag in the list, fallback if it has none.
inline u32 data(native::Bus& b, u32 tag, u32 list, u32 fallback) {
    const u32 item = find(b, tag, list);
    return item != 0 ? b.r32(item + 4) : fallback;
}

}
