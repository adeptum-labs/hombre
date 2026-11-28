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

#include "../format.hpp"
#include "../log.hpp"
#include "../m68k_cpu.hpp"
#include "../m68k_jit.hpp"
#include "../types.hpp"

// What the native AmigaOS libraries share: guest memory as a native sees
// it and the words their vectors are made of. They log through g_log
// (log.hpp).
//
// A native reads its arguments from the 68000 registers as the AmigaOS
// calling convention passes them (cpu.d[n], cpu.a[n], A6 the library base)
// and leaves its result in D0.

namespace hombre {

struct NativeVector {
    m68k::NativeFn fn;
    const char* name;
};

namespace native {

using hombre::LogLine;

constexpr u16 kALine = 0xA000;
constexpr u16 kRts = 0x4E75;
constexpr u16 kJmpAbsL = 0x4EF9;

// The offset into guest memory of the guest address addr: addr with the
// translator's address mask applied, as translated code applies it.
inline u32 guest_address(const m68k::Machine& m, u32 addr) {
    return m.jit != nullptr ? addr & m.jit->address_mask() : addr;
}

// Guest memory for a native. Every address goes through the translator's
// address mask first, so that a pointer with flags in its top byte reaches
// what it reaches in translated code. The first access the guest memory
// cannot hold ends the run as the 68000 would end it, BusError, or
// AddressError for a word or long at an odd address, with fault_addr set
// to the address as given; from then on every access is dropped and reads
// give 0, so a native can do its work and check ok() once before it writes
// its result.
class Bus {
public:
    explicit Bus(m68k::Machine& m)
        : m_(m), mask_(m.jit != nullptr ? m.jit->address_mask() : 0xFFFFFFFF) {}

    bool ok() const { return ok_; }

    // Drops every later access without a fault of its own, for when the run
    // has already ended inside a call into 68000 code.
    void stop() { ok_ = false; }

    m68k::Machine& machine() const { return m_; }

    u32 r8(u32 addr) {
        u32 v = 0;
        if (ok_ && !m_.mem.read8(addr & mask_, v)) {
            fault(addr, false);
        }
        return v;
    }

    u32 r16(u32 addr) {
        u32 v = 0;
        if (ok_ && ((addr & 1) != 0 || !m_.mem.read16(addr & mask_, v))) {
            fault(addr, true);
        }
        return v;
    }

    u32 r32(u32 addr) {
        u32 v = 0;
        if (ok_ && ((addr & 1) != 0 || !m_.mem.read32(addr & mask_, v))) {
            fault(addr, true);
        }
        return v;
    }

    void w8(u32 addr, u32 v) {
        if (ok_ && !m_.mem.write8(addr & mask_, v)) {
            fault(addr, false);
        }
    }

    void w16(u32 addr, u32 v) {
        if (ok_ && ((addr & 1) != 0 || !m_.mem.write16(addr & mask_, v))) {
            fault(addr, true);
        }
    }

    void w32(u32 addr, u32 v) {
        if (ok_ && ((addr & 1) != 0 || !m_.mem.write32(addr & mask_, v))) {
            fault(addr, true);
        }
    }

    // The bytes [addr, addr + bytes) of guest memory, addr masked, for bulk
    // work through the pointer returned; null, with a fault at the first
    // byte outside guest memory, if they are not all in it. Index only the
    // pointer returned: addr itself may carry bits the mask removes.
    u8* bytes_at(u32 addr, u32 bytes) {
        if (!ok_) {
            return nullptr;
        }
        const u32 at = addr & mask_;
        if (m_.mem.range_ok(at, bytes)) {
            return m_.mem.base + at;
        }
        fault(at >= m_.mem.size ? addr : addr + (m_.mem.size - at), false);
        return nullptr;
    }

    // Copies the NUL-terminated guest string into out. False if it faults
    // (ok() is then false) or does not fit in capacity bytes.
    bool string(u32 addr, char* out, u32 capacity) {
        for (u32 i = 0; i < capacity; ++i) {
            const u32 c = r8(addr + i);
            if (!ok_) {
                return false;
            }
            out[i] = static_cast<char>(c);
            if (c == 0) {
                return true;
            }
        }
        return false;
    }

    // Compares the guest string with a host string, byte by byte.
    bool equals(u32 addr, const char* host) {
        for (u32 i = 0;; ++i) {
            const u32 c = r8(addr + i);
            if (!ok_ || c != static_cast<u8>(host[i])) {
                return false;
            }
            if (c == 0) {
                return true;
            }
        }
    }

    // Compares two guest strings. Every step reads one byte further on, so
    // the walk ends at the end of guest memory at the latest.
    bool equals(u32 a, u32 b) {
        for (u32 i = 0;; ++i) {
            const u32 c = r8(a + i);
            const u32 d = r8(b + i);
            if (!ok_ || c != d) {
                return false;
            }
            if (c == 0) {
                return true;
            }
        }
    }

private:
    void fault(u32 addr, bool sized) {
        ok_ = false;
        m68k::CpuState& cpu = m_.cpu;
        if (cpu.exit != m68k::ExitReason::None) {
            return;  // the first reason stands
        }
        const bool odd = sized && (addr & 1) != 0;
        cpu.exit = odd ? m68k::ExitReason::AddressError : m68k::ExitReason::BusError;
        cpu.trap_number = odd ? 3 : 2;
        cpu.fault_addr = addr;
    }

    m68k::Machine& m_;
    u32 mask_;
    bool ok_ = true;
};

// Whether the running task's 68000 code has ended while a native was in
// the middle of it (a fault, a break, a refused call): nothing more may run
// for it, or the run would go on as if it had not ended. Outside 68000 code
// the exit of the last run is only left over and ends nothing.
inline bool run_ended(const m68k::Machine& m) {
    return m.jit != nullptr && m.jit->run_depth() > 0 && m.cpu.exit != m68k::ExitReason::None;
}

// Writes D0 unless the run has ended inside the native.
inline void set_result(m68k::Machine& m, u32 value) {
    if (m.cpu.exit == m68k::ExitReason::None) {
        m.cpu.d[0] = value;
    }
}

// Vector lvo of the library at base: the A-line opcode of native id, RTS.
inline void put_native_vector(Bus& bus, u32 base, u32 lvo, u16 id) {
    bus.w16(base - lvo, kALine | id);
    bus.w16(base - lvo + 2, kRts);
}

// Vector lvo of the library at base: JMP target.
inline void put_jump_vector(Bus& bus, u32 base, u32 lvo, u32 target) {
    bus.w16(base - lvo, kJmpAbsL);
    bus.w32(base - lvo + 2, target);
}

// Fills every word of the neg_size bytes below base with the A-line opcode
// of native id, so that a call to any of them, even one that is not a
// multiple of 6 bytes down, reaches that native.
inline void fill_with_native(Bus& bus, u32 base, u32 neg_size, u16 id) {
    for (u32 off = 2; off <= neg_size; off += 2) {
        bus.w16(base - off, kALine | id);
    }
}

// --- RawDoFmt formatting --------------------------------------------------

namespace format_detail {

constexpr u32 kMaxWidth = 10000;
constexpr u32 kMaxOutput = 1u << 20;  // characters one format gives at most

// Passes at most kMaxOutput characters on to out. The one after them is
// logged and stops the formatting.
template <typename Out>
struct Capped {
    Out& out;
    u32 left;

    bool put(u32 c) {
        if (left == 0) {
            LogLine().text("RawDoFmt: format output limit of ").dec(kMaxOutput)
                .text(" characters reached").send();
            return false;
        }
        --left;
        return out.put(c);
    }
};

struct Field {
    u32 width;
    bool left;
    bool zero;
};

template <typename Out>
bool repeat(Out& out, u32 c, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (!out.put(c)) {
            return false;
        }
    }
    return true;
}

template <typename Out>
bool put_number(Out& out, const Field& f, bool negative, const char* digits) {
    u32 length = negative ? 1 : 0;
    for (const char* p = digits; *p != '\0'; ++p) {
        ++length;
    }
    const u32 pad = f.width > length ? f.width - length : 0;
    if (!f.left && !f.zero && !repeat(out, ' ', pad)) {
        return false;
    }
    if (negative && !out.put('-')) {
        return false;
    }
    if (!f.left && f.zero && !repeat(out, '0', pad)) {
        return false;
    }
    for (const char* p = digits; *p != '\0'; ++p) {
        if (!out.put(static_cast<u8>(*p))) {
            return false;
        }
    }
    return !f.left || repeat(out, ' ', pad);
}

// count guest bytes from addr, padded to the field.
template <typename Out>
bool put_guest_text(Bus& b, Out& out, const Field& f, u32 addr, u32 count) {
    const u32 pad = f.width > count ? f.width - count : 0;
    if (!f.left && !repeat(out, f.zero ? '0' : ' ', pad)) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 c = b.r8(addr + i);
        if (!b.ok() || !out.put(c)) {
            return false;
        }
    }
    return !f.left || repeat(out, ' ', pad);
}

}

// Formats the guest string at fmt as exec's RawDoFmt does, taking the
// arguments from the data stream at data, and hands every character to
// out.put(c), which returns false to stop. The closing NUL is not passed.
//
// %[-][0][width][.limit][l]type with type d, u, x, X, c, s, b or %.
// Without l, d, u, x and c take a 16-bit word from the data stream.
// x and X both give uppercase hex digits, and a null %s or %b pointer
// prints as an empty string, as exec has done since V36.
//
// One format gives at most format_detail::kMaxOutput characters: past them
// it stops, logged, as if out had stopped, so that a format such as many
// %99999d cannot keep a native busy without end.
//
// Returns false if a guest access faulted (b.ok() is then false), out
// stopped or the output limit was reached; data is left past the last
// argument used.
template <typename Out>
bool raw_format(Bus& b, u32 fmt, u32& data, Out& sink) {
    using format_detail::Field;
    using format_detail::kMaxWidth;
    format_detail::Capped<Out> out{sink, format_detail::kMaxOutput};
    for (;;) {
        u32 c = b.r8(fmt++);
        if (!b.ok()) {
            return false;
        }
        if (c == 0) {
            return true;
        }
        if (c != '%') {
            if (!out.put(c)) {
                return false;
            }
            continue;
        }
        Field f{0, false, false};
        u32 limit = 0xFFFFFFFF;
        bool is_long = false;
        c = b.r8(fmt++);
        if (c == '-') {
            f.left = true;
            c = b.r8(fmt++);
        }
        if (c == '0') {
            f.zero = true;
            c = b.r8(fmt++);
        }
        while (c >= '0' && c <= '9' && b.ok()) {
            f.width = f.width < kMaxWidth ? f.width * 10 + (c - '0') : f.width;
            c = b.r8(fmt++);
        }
        if (c == '.') {
            limit = 0;
            c = b.r8(fmt++);
            while (c >= '0' && c <= '9' && b.ok()) {
                limit = limit < kMaxWidth ? limit * 10 + (c - '0') : limit;
                c = b.r8(fmt++);
            }
        }
        if (c == 'l') {
            is_long = true;
            c = b.r8(fmt++);
        }
        if (!b.ok()) {
            return false;
        }
        // %D and %U, the numbers locale.library groups, come out plain.
        if (c == 'D' || c == 'U') {
            c += 'a' - 'A';
        }
        bool ok = true;
        switch (c) {
        case 'd':
        case 'u':
        case 'x':
        case 'X':
        case 'c': {
            u32 v = is_long ? b.r32(data) : b.r16(data);
            data += is_long ? 4 : 2;
            if (!b.ok()) {
                return false;
            }
            if (c == 'c') {
                const u32 pad = f.width > 1 ? f.width - 1 : 0;
                ok = (f.left || format_detail::repeat(out, ' ', pad)) && out.put(v & 0xFF) &&
                     (!f.left || format_detail::repeat(out, ' ', pad));
                break;
            }
            char digits[12];
            bool negative = false;
            if (c == 'd') {
                if (!is_long && (v & 0x8000) != 0) {
                    v |= 0xFFFF0000;
                }
                if ((v & 0x80000000) != 0) {
                    negative = true;
                    v = 0u - v;
                }
            }
            format_u64(digits, sizeof digits, v, c == 'x' || c == 'X' ? 16 : 10);
            if (c == 'x' || c == 'X') {
                for (char* p = digits; *p != '\0'; ++p) {
                    *p = *p >= 'a' && *p <= 'f' ? static_cast<char>(*p - 'a' + 'A') : *p;
                }
            }
            ok = format_detail::put_number(out, f, negative, digits);
            break;
        }
        case 's':
        case 'b': {
            const u32 ptr = b.r32(data);
            data += 4;
            if (!b.ok()) {
                return false;
            }
            u32 text = 0;
            u32 length = 0;
            if (ptr != 0 && c == 's') {
                text = ptr;
                while (length < limit && b.r8(text + length) != 0 && b.ok()) {
                    ++length;
                }
            } else if (ptr != 0) {
                const u32 bstr = ptr << 2;
                text = bstr + 1;
                length = b.r8(bstr);
                length = length < limit ? length : limit;
            }
            ok = b.ok() && format_detail::put_guest_text(b, out, f, text, length);
            break;
        }
        case '%':
            ok = out.put('%');
            break;
        case 0:
            --fmt;  // the format ends inside the conversion
            break;
        default:
            ok = out.put(c);
            break;
        }
        if (!ok) {
            return false;
        }
    }
}

}

}
