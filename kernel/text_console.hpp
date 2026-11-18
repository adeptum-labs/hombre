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

#include "../common/types.hpp"
#include "framebuffer.hpp"

namespace hombre {

// A character console drawn into a framebuffer. Writes are mirrored into a
// shadow grid so that scrolling can repaint the region without losing text.
class TextConsole {
public:
    static constexpr u32 kMaxCols = 240;
    static constexpr u32 kMaxRows = 135;

    constexpr TextConsole() = default;

    // The console covers the full width from y = top to the bottom of the
    // screen. A zero scale, a zero width or a region too short for one cell
    // leaves no cells, and then put() does nothing.
    void init(Framebuffer fb, u32 top, u32 scale, u32 fg, u32 bg) {
        fb_ = fb;
        top_ = top;
        scale_ = scale;
        fg_ = fg;
        bg_ = bg;
        const u64 cell = cell_size();
        const u64 width = fb_.width();
        const u64 height = fb_.height() > top ? fb_.height() - top : 0;
        if (cell == 0 || cell > width || cell > height) {
            cols_ = 0;
            rows_ = 0;
        } else {
            cols_ = at_most(width / cell, kMaxCols);
            rows_ = at_most(height / cell, kMaxRows);
        }
        col_ = 0;
        row_ = 0;
        blank_grid();
        clear();
    }

    void put(char c) {
        if (cols_ == 0 || rows_ == 0) {
            return;
        }
        if (c == '\r') {
            return;
        }
        if (c == '\n') {
            col_ = 0;
            newline();
            return;
        }
        if (c == '\t') {
            tab();
            return;
        }
        grid_[row_][col_] = c;
        draw_cell(col_, row_);
        advance();
    }

private:
    static constexpr u32 at_most(u64 value, u32 limit) {
        return static_cast<u32>(value < limit ? value : limit);
    }

    void blank_grid() {
        for (u32 row = 0; row < kMaxRows; ++row) {
            for (u32 col = 0; col < kMaxCols; ++col) {
                grid_[row][col] = ' ';
            }
        }
    }

    u64 cell_size() const { return static_cast<u64>(scale_) * 8; }

    void clear() {
        const u64 height = fb_.height() > top_ ? fb_.height() - top_ : 0;
        fb_.fill_rect(0, top_, fb_.width(), height, bg_);
    }

    void draw_cell(u32 col, u32 row) {
        const u64 cell = cell_size();
        const u64 x = static_cast<u64>(col) * cell;
        const u64 y = static_cast<u64>(top_) + static_cast<u64>(row) * cell;
        fb_.fill_rect(x, y, cell, cell, bg_);
        const char text[2] = {grid_[row][col], '\0'};
        fb_.draw_text(static_cast<u32>(x), static_cast<u32>(y), text, fg_, scale_);
    }

    void advance() {
        if (col_ + 1 < cols_) {
            ++col_;
        } else {
            col_ = 0;
            newline();
        }
    }

    void newline() {
        if (row_ + 1 < rows_) {
            ++row_;
        } else {
            scroll();
        }
    }

    void tab() {
        const u32 target = (col_ / 8 + 1) * 8;
        if (target < cols_) {
            col_ = target;
        } else {
            col_ = 0;
            newline();
        }
    }

    // Moves the grid up one row and repaints it, leaving the cursor on the
    // new last row.
    void scroll() {
        for (u32 row = 0; row + 1 < rows_; ++row) {
            for (u32 col = 0; col < cols_; ++col) {
                grid_[row][col] = grid_[row + 1][col];
            }
        }
        for (u32 col = 0; col < cols_; ++col) {
            grid_[rows_ - 1][col] = ' ';
        }
        row_ = rows_ - 1;
        redraw();
    }

    void redraw() {
        clear();
        for (u32 row = 0; row < rows_; ++row) {
            for (u32 col = 0; col < cols_; ++col) {
                if (grid_[row][col] != ' ') {
                    draw_cell(col, row);
                }
            }
        }
    }

    Framebuffer fb_;
    u32 top_ = 0, scale_ = 0, fg_ = 0, bg_ = 0;
    u32 cols_ = 0, rows_ = 0;
    u32 col_ = 0, row_ = 0;
    char grid_[kMaxRows][kMaxCols] = {};
};

}
