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

namespace hombre {

// The stack as the entry stubs leave it, lowest address first: the general
// registers they push, the vector and error code, then what the CPU pushed.
struct TrapFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code;
    u64 rip, cs, rflags, rsp, ss;
};

static_assert(sizeof(TrapFrame) == 22 * 8);
static_assert(__builtin_offsetof(TrapFrame, vector) == 120);
static_assert(__builtin_offsetof(TrapFrame, rip) == 136);
static_assert(__builtin_offsetof(TrapFrame, ss) == 168);

inline constexpr u32 kExceptionCount = 32;

struct ExceptionInfo {
    const char* mnemonic;
    const char* name;
};

// The vectors for which the CPU itself pushes an error code.
constexpr bool exception_has_error_code(u8 vector) {
    switch (vector) {
    case 8: case 10: case 11: case 12: case 13: case 14: case 17: case 21: case 29: case 30:
        return true;
    default:
        return false;
    }
}

inline constexpr ExceptionInfo kReservedException{"#--", "reserved"};

// At namespace scope so the unoptimised kernel build does not copy the whole
// table onto the stack on every lookup, which the exception dump must avoid.
inline constexpr ExceptionInfo kExceptions[kExceptionCount] = {
    {"#DE", "divide error"},
    {"#DB", "debug"},
    {"NMI", "non-maskable interrupt"},
    {"#BP", "breakpoint"},
    {"#OF", "overflow"},
    {"#BR", "bound range exceeded"},
    {"#UD", "invalid opcode"},
    {"#NM", "device not available"},
    {"#DF", "double fault"},
    {"#CSO", "coprocessor segment overrun"},
    {"#TS", "invalid TSS"},
    {"#NP", "segment not present"},
    {"#SS", "stack-segment fault"},
    {"#GP", "general protection fault"},
    {"#PF", "page fault"},
    kReservedException,
    {"#MF", "x87 floating-point exception"},
    {"#AC", "alignment check"},
    {"#MC", "machine check"},
    {"#XM", "SIMD floating-point exception"},
    {"#VE", "virtualization exception"},
    {"#CP", "control protection exception"},
    kReservedException,
    kReservedException,
    kReservedException,
    kReservedException,
    kReservedException,
    kReservedException,
    {"#HV", "hypervisor injection exception"},
    {"#VC", "VMM communication exception"},
    {"#SX", "security exception"},
    kReservedException,
};

constexpr ExceptionInfo exception_info(u8 vector) {
    return vector < kExceptionCount ? kExceptions[vector] : ExceptionInfo{"#??", "unknown"};
}

}
