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

#include "format.hpp"
#include "types.hpp"

// The diagnostics sink of the 68000 translator and the native libraries.

namespace hombre {

// Where the diagnostics go, one line per call without a newline. Null
// drops them.
inline void (*g_log)(const char*) = nullptr;

// Builds one log line in a fixed buffer and hands it to g_log; text that
// does not fit is cut off.
class LogLine {
public:
    LogLine& text(const char* s) {
        while (s != nullptr && *s != '\0' && length_ + 1 < sizeof buffer_) {
            buffer_[length_++] = *s++;
        }
        return *this;
    }

    // The text up to the first '.', so "dos.library" gives "dos".
    LogLine& stem(const char* s) {
        while (s != nullptr && *s != '\0' && *s != '.' && length_ + 1 < sizeof buffer_) {
            buffer_[length_++] = *s++;
        }
        return *this;
    }

    LogLine& dec(u32 value) { return number(value, 10); }
    LogLine& hex(u32 value) { return text("$").number(value, 16); }

    void send() {
        buffer_[length_] = '\0';
        if (g_log != nullptr) {
            g_log(buffer_);
        }
    }

private:
    LogLine& number(u32 value, unsigned base) {
        char digits[12];
        if (format_u64(digits, sizeof digits, value, base) != 0) {
            text(digits);
        }
        return *this;
    }

    char buffer_[160];
    usize length_ = 0;
};

}
