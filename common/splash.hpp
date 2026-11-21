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

#include "types.hpp"

namespace hombre {

// A splash image as tools/mksplash.py writes it: a palette, then runs of
// palette indices covering the image row by row. draw scales the image to
// the destination with nearest neighbour sampling and centres it; bar_rect
// gives the boxes of the progress bar's blocks in the same geometry.
class SplashImage {
public:
    static constexpr u32 kMaxWidth = 2048;
    static constexpr u32 kMaxRowWidth = 4096;
    static constexpr u32 kMaxColours = 128;
    static constexpr u32 kHeaderBytes = 18;

    constexpr SplashImage() = default;

    // Reads the header, palette and runs. False when data is too short, the
    // magic or a field is wrong, or the runs do not cover the image exactly.
    bool parse(const u8* data, u32 size) {
        valid_ = false;
        if (data == nullptr || size < kHeaderBytes) {
            return false;
        }
        if (data[0] != 'H' || data[1] != 'S' || data[2] != 'P' || data[3] != 'L') {
            return false;
        }
        width_ = read16(data + 4);
        height_ = read16(data + 6);
        bar_x0_ = read16(data + 8);
        bar_y0_ = read16(data + 10);
        bar_x1_ = read16(data + 12);
        bar_y1_ = read16(data + 14);
        blocks_ = data[16];
        const u32 colours = data[17];
        if (width_ == 0 || height_ == 0 || width_ > kMaxWidth || colours == 0 ||
            colours > kMaxColours || blocks_ == 0) {
            return false;
        }
        if (bar_x0_ > bar_x1_ || bar_y0_ > bar_y1_ || bar_x1_ >= width_ ||
            bar_y1_ >= height_) {
            return false;
        }
        const u64 palette_bytes = static_cast<u64>(colours) * 3;
        if (kHeaderBytes + palette_bytes > size) {
            return false;
        }
        palette_ = data + kHeaderBytes;
        runs_ = palette_ + static_cast<usize>(palette_bytes);
        run_end_ = data + size;
        const u8* p = runs_;
        const u64 pixels = static_cast<u64>(width_) * height_;
        u64 covered = 0;
        while (covered < pixels) {
            if (run_end_ - p < 2) {
                return false;
            }
            if (static_cast<u32>(p[1]) >= colours) {
                return false;
            }
            covered += static_cast<u32>(p[0]) + 1;
            p += 2;
        }
        if (covered != pixels || p != run_end_) {
            return false;
        }
        valid_ = true;
        return true;
    }

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    u32 block_count() const { return blocks_; }
    u32 bar_x0() const { return bar_x0_; }
    u32 bar_y0() const { return bar_y0_; }
    u32 bar_x1() const { return bar_x1_; }
    u32 bar_y1() const { return bar_y1_; }

    // Calls put_row(y, rgb_row) for each destination row 0..dst_h-1 with
    // dst_w 0xRRGGBB pixels, black outside the image. Destinations wider
    // than kMaxRowWidth are clipped.
    template <class PutRow>
    void draw(u32 dst_w, u32 dst_h, PutRow put_row) const {
        if (!valid_) {
            return;
        }
        Layout l{};
        layout(dst_w, dst_h, l);
        if (l.columns == 0 || l.rows == 0) {
            return;
        }
        u8 source[kMaxWidth];
        u32 row[kMaxRowWidth];
        RowDecoder decoder(*this);
        const u64 right = static_cast<u64>(l.left) + l.image_width;
        const u64 bottom = static_cast<u64>(l.top) + l.image_height;
        for (u32 y = 0; y < l.rows; ++y) {
            const bool in_image_row = y >= l.top && y < bottom;
            if (in_image_row &&
                !decoder.row(scale(y - l.top, l.image_height, height_), source)) {
                return;
            }
            for (u32 x = 0; x < l.columns; ++x) {
                row[x] = in_image_row && x >= l.left && x < right
                             ? colour(source[scale(x - l.left, l.image_width, width_)])
                             : 0;
            }
            put_row(y, row);
        }
    }

    // The box of block i where draw puts it. x1 before x0 or y1 before y0
    // means the block has no pixels at that size.
    void bar_rect(u32 dst_w, u32 dst_h, u32 block, i32& x0, i32& y0, i32& x1,
                  i32& y1) const {
        x0 = 0;
        y0 = 0;
        x1 = 0;
        y1 = 0;
        if (!valid_ || block >= blocks_) {
            return;
        }
        Layout l{};
        layout(dst_w, dst_h, l);
        if (l.columns == 0 || l.rows == 0) {
            return;
        }
        const u32 span = bar_x1_ - bar_x0_ + 1;
        const u32 first =
            bar_x0_ + static_cast<u32>(static_cast<u64>(block) * span / blocks_);
        const u32 after =
            bar_x0_ + static_cast<u32>(static_cast<u64>(block + 1) * span / blocks_);
        x0 = static_cast<i32>(l.left + scale(first, width_, l.image_width));
        x1 = static_cast<i32>(l.left + scale(after, width_, l.image_width)) - 1;
        y0 = static_cast<i32>(l.top + scale(bar_y0_, height_, l.image_height));
        y1 = static_cast<i32>(l.top + scale(bar_y1_ + 1, height_, l.image_height)) - 1;
    }

private:
    // Where the scaled image sits in the destination.
    struct Layout {
        u32 columns;
        u32 rows;
        u32 image_width;
        u32 image_height;
        u32 left;
        u32 top;
    };

    // The runs holding the source row decode_ has reached.
    class RowDecoder {
    public:
        explicit RowDecoder(const SplashImage& image) : image_(image), next_(image.runs_) {}

        // Fills out with the indices of source row target, decoding rows in
        // order. A target already decoded needs no more runs.
        bool row(u32 target, u8* out) {
            while (decoded_ < static_cast<i64>(target)) {
                for (u32 x = 0; x < image_.width_; ++x) {
                    if (left_ == 0) {
                        if (image_.run_end_ - next_ < 2) {
                            return false;
                        }
                        left_ = static_cast<u32>(next_[0]) + 1;
                        index_ = next_[1];
                        next_ += 2;
                    }
                    out[x] = index_;
                    --left_;
                }
                ++decoded_;
            }
            return true;
        }

    private:
        const SplashImage& image_;
        const u8* next_;
        u32 left_ = 0;
        u8 index_ = 0;
        i64 decoded_ = -1;
    };

    static u32 read16(const u8* p) {
        return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8);
    }

    static u32 scale(u32 value, u32 from, u32 to) {
        return static_cast<u32>(static_cast<u64>(value) * to / from);
    }

    void layout(u32 dst_w, u32 dst_h, Layout& l) const {
        l.columns = dst_w > kMaxRowWidth ? kMaxRowWidth : dst_w;
        l.rows = dst_h;
        l.image_width = 0;
        l.image_height = 0;
        l.left = 0;
        l.top = 0;
        if (l.columns == 0 || dst_h == 0) {
            return;
        }
        if (static_cast<u64>(l.columns) * height_ <=
            static_cast<u64>(dst_h) * width_) {
            l.image_width = l.columns;
            l.image_height =
                static_cast<u32>(static_cast<u64>(l.columns) * height_ / width_);
        } else {
            l.image_height = dst_h;
            l.image_width =
                static_cast<u32>(static_cast<u64>(dst_h) * width_ / height_);
        }
        if (l.image_width == 0) {
            l.image_width = 1;
        }
        if (l.image_height == 0) {
            l.image_height = 1;
        }
        l.left = (l.columns - l.image_width) / 2;
        l.top = (dst_h - l.image_height) / 2;
    }

    u32 colour(u32 index) const {
        const u8* entry = palette_ + index * 3;
        return (static_cast<u32>(entry[0]) << 16) |
               (static_cast<u32>(entry[1]) << 8) | entry[2];
    }

    const u8* palette_ = nullptr;
    const u8* runs_ = nullptr;
    const u8* run_end_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    u32 bar_x0_ = 0;
    u32 bar_y0_ = 0;
    u32 bar_x1_ = 0;
    u32 bar_y1_ = 0;
    u32 blocks_ = 0;
    bool valid_ = false;
};

}
