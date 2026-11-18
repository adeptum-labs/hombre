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

// Logical lines of the AmigaShell console, kept in a ring and laid out at
// any column count on demand.
class ShellText {
public:
    static constexpr u32 kMaxLines = 256;
    static constexpr u32 kMaxLineBytes = 256;

    struct Row {
        const u8* text;
        u32 length;
    };

    void clear() {
        head_ = 0;
        count_ = 1;
        lengths_[0] = 0;
        input_start_ = 0;
    }

    void put(u8 c) {
        u32 cur = current_slot();
        if (lengths_[cur] == kMaxLineBytes) {
            new_line();
            cur = current_slot();
        }
        lines_[cur][lengths_[cur]++] = c;
    }

    void new_line() {
        if (count_ < kMaxLines) {
            ++count_;
        } else {
            head_ = (head_ + 1) % kMaxLines;
        }
        lengths_[current_slot()] = 0;
        input_start_ = 0;
    }

    void mark_input_start() {
        input_start_ = lengths_[current_slot()];
    }

    bool erase_last() {
        const u32 cur = current_slot();
        if (lengths_[cur] <= input_start_) {
            return false;
        }
        --lengths_[cur];
        return true;
    }

    u32 line_count() const {
        return count_;
    }

    // Layout at columns (0 means 1): a completed line takes
    // max(1, ceil(len/columns)) rows; the current line takes
    // len/columns + 1 rows, its last row is where the cursor is. Fills
    // out with up to rows of the last wrapped rows, oldest first, and
    // returns how many it filled (rows 0 fills nothing). Each Row points
    // into the store.
    u32 tail(u32 columns, u32 rows, Row* out) const {
        if (rows == 0 || out == nullptr) {
            return 0;
        }
        const u32 cols = columns == 0 ? 1 : columns;
        u32 total = 0;
        for (u32 back = 0; back < count_ && total < rows; ++back) {
            const u32 i = count_ - 1 - back;
            const u32 r = line_rows(i, cols);
            if (total + r >= rows) {
                total = rows;
                break;
            }
            total += r;
        }
        u32 pos = total;
        for (u32 back = 0; back < count_ && pos > 0; ++back) {
            const u32 i = count_ - 1 - back;
            const u32 slot = slot_of(i);
            const u32 len = lengths_[slot];
            const u32 r_count = line_rows(i, cols);
            for (u32 r = r_count; r > 0 && pos > 0; --r) {
                const u32 offset = (r - 1) * cols;
                const u32 rem = offset < len ? len - offset : 0;
                const u32 row_len = rem < cols ? rem : cols;
                const u8* ptr = lines_[slot] + (offset < kMaxLineBytes ? offset : 0);
                out[--pos] = Row{ptr, row_len};
            }
        }
        return total;
    }

    u32 cursor_column(u32 columns) const {
        const u32 cols = columns == 0 ? 1 : columns;
        return lengths_[current_slot()] % cols;
    }

private:
    u32 slot_of(u32 index) const {
        return (head_ + index) % kMaxLines;
    }

    u32 current_slot() const {
        return slot_of(count_ - 1);
    }

    u32 line_rows(u32 index, u32 cols) const {
        const u32 len = lengths_[slot_of(index)];
        if (index + 1 == count_) {
            return len / cols + 1;
        }
        return len == 0 ? 1 : (len - 1) / cols + 1;
    }

    u8 lines_[kMaxLines][kMaxLineBytes]{};
    u32 lengths_[kMaxLines]{};
    u32 head_ = 0;
    u32 count_ = 1;
    u32 input_start_ = 0;
};

}
