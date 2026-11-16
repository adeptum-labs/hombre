#pragma once

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
        char digits[16];
        int count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 0xF];
            value >>= 4;
        } while (value);
        append("0x");
        while (count && length_ < capacity - 1) {
            buffer_[length_++] = digits[--count];
        }
        return *this;
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
