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

#include "../common/format.hpp"
#include "../common/serial.hpp"
#include "../common/types.hpp"
#include "uefi.hpp"

namespace hombre {

// Builds one diagnostic line and sends it to the serial port and, while boot
// services are still available, the firmware console.
class ErrorLine {
public:
    explicit ErrorLine(const char* text) { append(text); }

    ErrorLine& append(const char* text) {
        for (; *text && length_ < capacity - 1; ++text) {
            buffer_[length_++] = *text;
        }
        return *this;
    }

    ErrorLine& append_hex(u64 value) {
        char digits[17];  // 16 hex digits and the terminator
        format_u64(digits, sizeof(digits), value, 16);
        return append("0x").append(digits);
    }

    // With st == nullptr the line goes to the serial port only, which is all
    // that is left once the firmware may no longer be called.
    void emit(EfiSystemTable* st) const {
        char16_t wide[capacity + 2];
        usize i = 0;
        for (; i < length_; ++i) {
            serial::write_char(buffer_[i]);
            wide[i] = static_cast<char16_t>(buffer_[i]);
        }
        serial::write_char('\n');
        if (st == nullptr) {
            return;
        }
        wide[i++] = u'\r';
        wide[i++] = u'\n';
        wide[i] = u'\0';
        st->con_out->output_string(st->con_out, wide);
    }

private:
    static constexpr usize capacity = 160;

    char buffer_[capacity];
    usize length_ = 0;
};

}
