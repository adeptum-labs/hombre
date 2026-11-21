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

#include "text_blend.hpp"

namespace hombre::text_blend {

namespace {

constexpr u32 kMaxPens = 256;

constexpr u32 linear(u32 v) {
    return v * v;
}

// floor(sqrt(value)), exact for every u32.
constexpr u32 isqrt(u32 value) {
    u32 root = 0;
    u32 bit = 1u << 30;
    while (bit > value) {
        bit >>= 2;
    }
    for (; bit != 0; bit >>= 2) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
    }
    return root;
}

// One channel of the blend from bg at level 0 to fg at the last level,
// through linear light and back to 8 bits.
constexpr u32 blend(u32 fg, u32 bg, u32 level) {
    const u32 mixed = (linear(fg) * level + linear(bg) * (kLevels - 1 - level)) / (kLevels - 1);
    return isqrt(mixed);
}

constexpr u16 key_of(u8 fg, u8 bg) {
    return static_cast<u16>((static_cast<u32>(fg) << 8) | bg);
}

// Knuth's multiplicative hash, spread over the 256 slots.
constexpr u32 slot_of(u16 key) {
    return (static_cast<u32>(key) * 2654435761u) >> 24;
}

// The pen for one level of a ramp: the nearest to the blend, counting a
// pen's distance off the line from bg to fg four times over, so that a
// palette without the shade in between gives a lighter or darker shade of
// the same hue rather than a near colour of another.
u8 nearest_on_ramp(const u8 (*rgb)[3], u32 count, const u32 (&target)[3], const u8* fg,
                   const u8* bg) {
    constexpr i64 kOffLineWeight = 4;
    i64 line[3];
    i64 line_length = 0;
    for (u32 c = 0; c < 3; ++c) {
        line[c] = static_cast<i64>(fg[c]) - static_cast<i64>(bg[c]);
        line_length += line[c] * line[c];
    }
    u8 best = 0;
    i64 best_score = -1;
    for (u32 pen = 0; pen < count; ++pen) {
        i64 distance = 0;
        i64 along = 0;
        i64 from_bg = 0;
        for (u32 c = 0; c < 3; ++c) {
            const i64 d = static_cast<i64>(rgb[pen][c]) - static_cast<i64>(target[c]);
            const i64 v = static_cast<i64>(rgb[pen][c]) - static_cast<i64>(bg[c]);
            distance += d * d;
            along += v * line[c];
            from_bg += v * v;
        }
        const i64 off_line =
            line_length != 0 ? from_bg - along * along / line_length : from_bg;
        const i64 score = distance + kOffLineWeight * off_line;
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best = static_cast<u8>(pen);
        }
    }
    return best;
}

}

u8 nearest(const u8 (*rgb)[3], u32 count, u32 r, u32 g, u32 b) {
    const u32 pens = count > kMaxPens ? kMaxPens : count;
    const i32 tr = static_cast<i32>(r > 255 ? 255 : r);
    const i32 tg = static_cast<i32>(g > 255 ? 255 : g);
    const i32 tb = static_cast<i32>(b > 255 ? 255 : b);
    u8 best = 0;
    u32 best_distance = 0xFFFFFFFF;
    for (u32 pen = 0; pen < pens; ++pen) {
        const i32 dr = static_cast<i32>(rgb[pen][0]) - tr;
        const i32 dg = static_cast<i32>(rgb[pen][1]) - tg;
        const i32 db = static_cast<i32>(rgb[pen][2]) - tb;
        const u32 distance = static_cast<u32>(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<u8>(pen);
        }
    }
    return best;
}

void Cache::set_palette(const u8 (*rgb)[3], u32 count) {
    count_ = count > kPens ? kPens : count;
    if (rgb == nullptr) {
        count_ = 0;
    }
    for (u32 i = 0; i < count_; ++i) {
        palette_[i][0] = rgb[i][0];
        palette_[i][1] = rgb[i][1];
        palette_[i][2] = rgb[i][2];
    }
    invalidate();
}

bool Cache::has_palette() const {
    return count_ != 0;
}

const u8* Cache::ramp(u8 fg, u8 bg) {
    if (static_cast<u32>(fg) >= count_ || static_cast<u32>(bg) >= count_) {
        return nullptr;
    }
    const u16 key = key_of(fg, bg);
    Slot& slot = slots_[slot_of(key)];
    if (slot.filled && slot.key == key) {
        return slot.pens;
    }
    const u8* fg_rgb = palette_[fg];
    const u8* bg_rgb = palette_[bg];
    for (u32 level = 0; level < kLevels; ++level) {
        const u32 target[3] = {blend(fg_rgb[0], bg_rgb[0], level),
                               blend(fg_rgb[1], bg_rgb[1], level),
                               blend(fg_rgb[2], bg_rgb[2], level)};
        if (level == 0) {
            slot.pens[level] = bg;
        } else if (level == kLevels - 1) {
            slot.pens[level] = fg;
        } else {
            slot.pens[level] = nearest_on_ramp(palette_, count_, target, fg_rgb, bg_rgb);
        }
    }
    slot.key = key;
    slot.filled = true;
    return slot.pens;
}

void Cache::invalidate() {
    for (u32 i = 0; i < kSlots; ++i) {
        slots_[i].filled = false;
    }
}

}
