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

#include "../common/libs/gfx.hpp"
#include "../common/libs/native.hpp"
#include "../common/pointer_image.hpp"
#include "../common/types.hpp"
#include "framebuffer.hpp"

namespace hombre {

namespace detail {

struct BitSpreadTable {
    u64 entries[256];
};

constexpr BitSpreadTable make_bit_spread_table() {
    BitSpreadTable t{};
    for (u32 v = 0; v < 256; ++v) {
        u64 out = 0;
        for (u32 k = 0; k < 8; ++k) {
            if (((v >> (7 - k)) & 1u) != 0) {
                out |= 1ULL << (k * 8);
            }
        }
        t.entries[v] = out;
    }
    return t;
}

inline constexpr BitSpreadTable kBitSpread = make_bit_spread_table();

}

// Draws the front screen's bitplanes into the framebuffer: the 640x400
// logical screen at the largest whole scale that fits, centred, and the rest
// of the framebuffer in the border colour.
class AmigaDisplay {
public:
    constexpr AmigaDisplay() = default;

    // Sizes the screen for the framebuffer and clears it to the border
    // colour.
    void init(Framebuffer fb) {
        fb_ = fb;
        const u64 width = fb_.width();
        const u64 height = fb_.height();
        const u64 fit_x = width / kScreenWidth;
        const u64 fit_y = height / kScreenHeight;
        const u64 fit = fit_x < fit_y ? fit_x : fit_y;
        scale_ = static_cast<u32>(fit > 0 ? fit : 1);
        origin_x_ = centred(width, kScreenWidth * scale_);
        origin_y_ = centred(height, kScreenHeight * scale_);
        fb_.fill(kBorder);
    }

    DisplaySink sink() { return DisplaySink{&AmigaDisplay::refresh, this}; }

    // Draws the pixels of dirty, clipped to the screen, as scale x scale
    // blocks. Screen 0 clears the framebuffer to the border colour, and a
    // screen larger than 4096 pixels on a side is ignored.
    static void refresh(void* ctx, m68k::Machine& m, u32 screen, const Rect& dirty) {
        AmigaDisplay& display = *static_cast<AmigaDisplay*>(ctx);
        if (screen == 0) {
            display.fb_.fill(kBorder);
            return;
        }
        native::Bus b(m);
        const u32 width = b.r16(screen + ndk::sc_Width);
        const u32 height = b.r16(screen + ndk::sc_Height);
        if (!b.ok() || width > kMaxSide || height > kMaxSide) {
            return;
        }
        const u32 bitmap = screen + ndk::sc_BitMap;
        const u32 bytes_per_row = b.r16(bitmap + ndk::bm_BytesPerRow);
        const u32 depth = b.r8(bitmap + ndk::bm_Depth);
        const u32 used = depth < kMaxPlanes ? depth : kMaxPlanes;
        u32 planes[kMaxPlanes]{};
        for (u32 plane = 0; plane < used; ++plane) {
            planes[plane] = b.r32(bitmap + ndk::bm_Planes + plane * 4);
        }
        const u32 colour_map = b.r32(screen + ndk::sc_ViewPort + ndk::vp_ColorMap);
        const u8 cm_type = static_cast<u8>(b.r8(colour_map + ndk::cm_Type));
        const u32 cm_count = b.r16(colour_map + ndk::cm_Count);
        const u32 table = b.r32(colour_map + ndk::cm_ColorTable);
        const u32 low_table =
            cm_type >= ndk::COLORMAP_TYPE_V39 ? b.r32(colour_map + ndk::cm_LowColorBits) : 0;
        const u32 palette_count = cm_count < kMaxColours ? cm_count : kMaxColours;
        u32 palette[kMaxColours]{};
        if (low_table != 0) {
            for (u32 pen = 0; pen < palette_count; ++pen) {
                palette[pen] = colour24(b.r16(table + pen * 2), b.r16(low_table + pen * 2));
            }
        } else {
            for (u32 pen = 0; pen < palette_count; ++pen) {
                palette[pen] = colour(b.r16(table + pen * 2));
            }
        }
        if (!b.ok()) {
            return;
        }
        const i32 last_x = at_most(dirty.x1, width);
        const i32 last_y = at_most(dirty.y1, height);
        const i32 first_x = dirty.x0 > 0 ? dirty.x0 : 0;
        const i32 first_y = dirty.y0 > 0 ? dirty.y0 : 0;
        if (first_x <= last_x && first_y <= last_y) {
            const u32 raw_count = static_cast<u32>(last_x - first_x + 1);
            const u32 count = raw_count < kScreenWidth ? raw_count : kScreenWidth;
            const u64 left = display.origin_x_ + static_cast<u64>(first_x) * display.scale_;
            const u32 first_bx = static_cast<u32>(first_x) >> 3;
            const u32 last_bx = (static_cast<u32>(first_x) + count - 1) >> 3;
            const u32 bx_count = last_bx - first_bx + 1;
            u32 row[kScreenWidth];
            for (i32 y = first_y; y <= last_y; ++y) {
                const u32 row_offset = static_cast<u32>(y) * bytes_per_row;
                const u8* plane_ptrs[kMaxPlanes]{};
                for (u32 plane = 0; plane < used; ++plane) {
                    plane_ptrs[plane] =
                        b.bytes_at(planes[plane] + row_offset + first_bx, bx_count);
                }
                if (!b.ok()) {
                    return;
                }
                u32 x = static_cast<u32>(first_x);
                u32 remaining = count;
                u32 out_i = 0;
                for (u32 bi = 0; bi < bx_count; ++bi) {
                    u64 packed = 0;
                    for (u32 plane = 0; plane < used; ++plane) {
                        packed |= detail::kBitSpread.entries[plane_ptrs[plane][bi]] << plane;
                    }
                    const u32 bit_off = x & 7;
                    const u32 avail = 8 - bit_off;
                    const u32 chunk = remaining < avail ? remaining : avail;
                    if (chunk == 8) {
                        row[out_i + 0] = palette[packed & 0xFF];
                        row[out_i + 1] = palette[(packed >> 8) & 0xFF];
                        row[out_i + 2] = palette[(packed >> 16) & 0xFF];
                        row[out_i + 3] = palette[(packed >> 24) & 0xFF];
                        row[out_i + 4] = palette[(packed >> 32) & 0xFF];
                        row[out_i + 5] = palette[(packed >> 40) & 0xFF];
                        row[out_i + 6] = palette[(packed >> 48) & 0xFF];
                        row[out_i + 7] = palette[(packed >> 56) & 0xFF];
                    } else {
                        u64 p = packed >> (bit_off * 8);
                        for (u32 k = 0; k < chunk; ++k) {
                            row[out_i + k] = palette[p & 0xFF];
                            p >>= 8;
                        }
                    }
                    x += chunk;
                    out_i += chunk;
                    remaining -= chunk;
                }
                const u64 top = display.origin_y_ + static_cast<u64>(y) * display.scale_;
                display.fb_.put_row(left, top, row, count, display.scale_);
            }
        }
        if (display.pointer_visible_) {
            const i32 ptr_first_x = display.pointer_x_ > first_x ? display.pointer_x_ : first_x;
            const i32 ptr_first_y = display.pointer_y_ > first_y ? display.pointer_y_ : first_y;
            const i32 ptr_end_x = display.pointer_x_ + static_cast<i32>(kPointerWidth) - 1;
            const i32 ptr_end_y = display.pointer_y_ + static_cast<i32>(kPointerHeight) - 1;
            const i32 ptr_last_x = ptr_end_x < last_x ? ptr_end_x : last_x;
            const i32 ptr_last_y = ptr_end_y < last_y ? ptr_end_y : last_y;
            if (ptr_first_x <= ptr_last_x && ptr_first_y <= ptr_last_y) {
                for (i32 y = ptr_first_y; y <= ptr_last_y; ++y) {
                    const u32 py = static_cast<u32>(y - display.pointer_y_);
                    for (i32 x = ptr_first_x; x <= ptr_last_x; ++x) {
                        const u32 px = static_cast<u32>(x - display.pointer_x_);
                        const u32 pixel = pointer_pixel(px, py);
                        if (pixel == 0) {
                            continue;
                        }
                        const u32 colour = pixel == 1 ? kPointerOutlineColour : kPointerFillColour;
                        const u64 left = display.origin_x_ + static_cast<u64>(x) * display.scale_;
                        const u64 top = display.origin_y_ + static_cast<u64>(y) * display.scale_;
                        display.fb_.fill_rect(left, top, display.scale_, display.scale_, colour);
                    }
                }
            }
        }
    }

    void set_pointer_visible(m68k::Machine& m, u32 screen, bool visible) {
        pointer_visible_ = visible;
        if (screen == 0) {
            return;
        }
        native::Bus b(m);
        const u32 width = b.r16(screen + ndk::sc_Width);
        const u32 height = b.r16(screen + ndk::sc_Height);
        if (!b.ok() || width > kMaxSide || height > kMaxSide) {
            return;
        }
        const Rect dirty =
            pointer_union(pointer_x_, pointer_y_, pointer_x_, pointer_y_, width, height);
        refresh(this, m, screen, dirty);
    }

    void set_pointer_visible(bool visible) { pointer_visible_ = visible; }

    bool pointer_visible() const { return pointer_visible_; }
    i32 pointer_x() const { return pointer_x_; }
    i32 pointer_y() const { return pointer_y_; }

    void move_pointer(m68k::Machine& m, u32 screen, i32 x, i32 y) {
        const i32 old_x = pointer_x_;
        const i32 old_y = pointer_y_;
        pointer_x_ = x;
        pointer_y_ = y;
        if (!pointer_visible_ || screen == 0) {
            return;
        }
        native::Bus b(m);
        const u32 width = b.r16(screen + ndk::sc_Width);
        const u32 height = b.r16(screen + ndk::sc_Height);
        if (!b.ok() || width > kMaxSide || height > kMaxSide) {
            return;
        }
        const Rect dirty = pointer_union(old_x, old_y, x, y, width, height);
        refresh(this, m, screen, dirty);
    }

    u32 scale() const { return scale_; }
    u32 origin_x() const { return origin_x_; }
    u32 origin_y() const { return origin_y_; }

private:
    static constexpr u32 kScreenWidth = 640;
    static constexpr u32 kScreenHeight = 400;
    static constexpr u32 kMaxSide = 4096;
    static constexpr u32 kMaxPlanes = 8;
    static constexpr u32 kMaxColours = 1u << kMaxPlanes;
    static constexpr u32 kBorder = 0xAAAAAA;
    static constexpr u32 kPointerOutlineColour = 0x000000;
    static constexpr u32 kPointerFillColour = 0xFFFFFF;

    // The union of two pointer rectangles clipped to the screen.
    static Rect pointer_union(i32 x0, i32 y0, i32 x1, i32 y1, u32 width, u32 height) {
        const i32 min_x = x0 < x1 ? x0 : x1;
        const i32 min_y = y0 < y1 ? y0 : y1;
        const i32 max_x = (x0 > x1 ? x0 : x1) + static_cast<i32>(kPointerWidth) - 1;
        const i32 max_y = (y0 > y1 ? y0 : y1) + static_cast<i32>(kPointerHeight) - 1;
        const i32 first_x = min_x > 0 ? min_x : 0;
        const i32 first_y = min_y > 0 ? min_y : 0;
        const i32 last_x = at_most(max_x, width);
        const i32 last_y = at_most(max_y, height);
        return Rect{first_x, first_y, last_x, last_y};
    }

    // The offset of a screen that fits wholly in the framebuffer, 0 when it
    // does not and is clipped at the top left.
    static u32 centred(u64 framebuffer_side, u64 screen_side) {
        return screen_side < framebuffer_side
                   ? static_cast<u32>((framebuffer_side - screen_side) / 2)
                   : 0;
    }

    static i32 at_most(i32 value, u32 side) {
        const i32 maximum = static_cast<i32>(side) - 1;
        return value < maximum ? value : maximum;
    }

    // The 0xRRGGBB colour of a colour table word, a nibble times 17.
    static constexpr u32 colour(u32 rgb) {
        const u32 red = ((rgb >> 8) & 0xF) * 0x11;
        const u32 green = ((rgb >> 4) & 0xF) * 0x11;
        const u32 blue = (rgb & 0xF) * 0x11;
        return (red << 16) | (green << 8) | blue;
    }

    // The 0xRRGGBB colour combining high nibbles (ColorTable) and low nibbles
    // (LowColorBits).
    static constexpr u32 colour24(u32 hi, u32 lo) {
        const u32 red = (((hi >> 8) & 0xF) << 4) | ((lo >> 8) & 0xF);
        const u32 green = (((hi >> 4) & 0xF) << 4) | ((lo >> 4) & 0xF);
        const u32 blue = ((hi & 0xF) << 4) | (lo & 0xF);
        return (red << 16) | (green << 8) | blue;
    }

    Framebuffer fb_{};
    u32 scale_ = 1;
    u32 origin_x_ = 0;
    u32 origin_y_ = 0;
    i32 pointer_x_ = 0;
    i32 pointer_y_ = 0;
    bool pointer_visible_ = false;
};

}
