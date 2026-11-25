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

#include "idt.hpp"

#include "../common/serial.hpp"
#include "apic.hpp"
#include "descriptors.hpp"
#include "gdt.hpp"
#include "monitor.hpp"
#include "trap_frame.hpp"

// Bit n is set when the CPU pushes an error code for vector n. It is a macro
// so the entry stubs below can use it as an assembler constant.
#define ERROR_CODE_VECTORS 0x60227D00
#define STRINGIFY(x) #x
#define EXPAND_TO_STRING(x) STRINGIFY(x)

namespace hombre {

namespace {

constexpr bool error_code_vectors_match() {
    for (u32 vector = 0; vector < kExceptionCount; ++vector) {
        const bool has_error_code = (ERROR_CODE_VECTORS >> vector) & 1;
        if (has_error_code != exception_has_error_code(static_cast<u8>(vector))) {
            return false;
        }
    }
    return true;
}

static_assert(error_code_vectors_match());

}

}

// One stub per vector. Where the CPU pushes no error code the stub pushes a
// zero in its place, so every vector leaves the same TrapFrame. The common
// path saves the general registers, calls trap_dispatch on a 16-byte aligned
// stack and restores everything for iretq, so a returning handler resumes
// the interrupted code untouched. trap_stub_table holds each stub's address.
asm(R"(
    .pushsection .text

    .macro trap_stub vector, needs_dummy
    trap_stub_\vector:
    .if \needs_dummy
        pushq $0
    .endif
        pushq $\vector
        jmp trap_common
    .endm

    .macro trap_stub_address vector
        .quad trap_stub_\vector
    .endm

    # Alternate macro mode only around the calls: it turns %symbol into the
    # symbol's value, and it would misread the shift and & elsewhere.
    .set trap_error_code_vectors, )" EXPAND_TO_STRING(ERROR_CODE_VECTORS) R"(
    .set trap_vector, 0
    .rept 256
        .set trap_has_error_code, (trap_error_code_vectors >> (trap_vector & 31)) & 1
        .set trap_needs_dummy, trap_vector >= 32 || trap_has_error_code == 0
        .altmacro
        trap_stub %trap_vector, %trap_needs_dummy
        .noaltmacro
        .set trap_vector, trap_vector + 1
    .endr

trap_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    movq %rsp, %rbx
    # The frame is 176 bytes and long mode already aligns the stack on entry,
    # so the and is only defensive.
    andq $-16, %rsp
    cld
    call trap_dispatch
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

    .section .rodata
    .balign 8
trap_stub_table:
    .set trap_vector, 0
    .rept 256
        .altmacro
        trap_stub_address %trap_vector
        .noaltmacro
        .set trap_vector, trap_vector + 1
    .endr

    .popsection
)");

extern "C" const hombre::u64 trap_stub_table[256];

namespace hombre {

namespace {

constexpr u64 vector_nmi = 2;
constexpr u64 vector_breakpoint = 3;
constexpr u64 vector_double_fault = 8;
constexpr u64 vector_machine_check = 18;
constexpr u32 vector_count = 256;

// In .bss in the higher half, so it stays valid after the lower half is
// unmapped.
alignas(16) IdtGate idt[vector_count];
u64 breakpoint_count;

u8 ist_for(u32 vector) {
    switch (vector) {
    case vector_nmi:
        return kIstNmi;
    case vector_double_fault:
        return kIstDoubleFault;
    case vector_machine_check:
        return kIstMachineCheck;
    case kApicTimerVector:
        return kIstTimer;
    default:
        return 0;
    }
}

u64 read_cr2() {
    u64 value;
    asm volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

u64 read_cr3() {
    u64 value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

void write_fields() {
    serial::write("\n");
}

// Writes "label: 0x<value>" pairs separated by spaces, then a newline.
template <typename... Rest>
void write_fields(const char* label, u64 value, Rest... rest) {
    serial::write(label);
    serial::write(": 0x");
    serial::write_hex(value);
    if constexpr (sizeof...(rest) > 0) {
        serial::write(" ");
    }
    write_fields(rest...);
}

void write_heading(u64 vector) {
    if (vector < kExceptionCount) {
        const ExceptionInfo info = exception_info(static_cast<u8>(vector));
        serial::write("*** exception ");
        serial::write_dec(vector);
        serial::write(" ");
        serial::write(info.mnemonic);
        serial::write(" (");
        serial::write(info.name);
        serial::write(")\n");
    } else {
        serial::write("*** unexpected interrupt ");
        serial::write_dec(vector);
        serial::write("\n");
    }
}

// Only port I/O and under 1 KiB of stack (0x200 bytes was too little, 0x400
// enough): a fault in here must not bury the dump already on the wire.
[[noreturn]] void dump_and_halt(const TrapFrame& frame) {
    write_heading(frame.vector);
    write_fields("error", frame.error_code);
    write_fields("rip", frame.rip, "cs", frame.cs, "rflags", frame.rflags);
    write_fields("rsp", frame.rsp, "ss", frame.ss);
    write_fields("cr2", read_cr2(), "cr3", read_cr3());
    write_fields("rax", frame.rax, "rbx", frame.rbx, "rcx", frame.rcx, "rdx", frame.rdx);
    write_fields("rsi", frame.rsi, "rdi", frame.rdi, "rbp", frame.rbp);
    write_fields("r8", frame.r8, "r9", frame.r9, "r10", frame.r10, "r11", frame.r11);
    write_fields("r12", frame.r12, "r13", frame.r13, "r14", frame.r14, "r15", frame.r15);
    serial::write("halting\n");
    for (;;) {
        asm volatile("cli; hlt");
    }
}

}

extern "C" void trap_dispatch(TrapFrame* frame) {
    if (frame->vector == kApicTimerVector) {
        apic_handle_timer_irq();
        monitor_timer_tick(*frame);
        return;
    }
    if (frame->vector == kApicSpuriousVector) {
        return;
    }
    if (frame->vector != vector_breakpoint) {
        dump_and_halt(*frame);
    }
    serial::write("breakpoint trap\n");
    ++breakpoint_count;
}

void idt_init_bsp() {
    for (u32 vector = 0; vector < vector_count; ++vector) {
        idt[vector] = make_interrupt_gate(trap_stub_table[vector], kKernelCodeSelector,
                                          ist_for(vector));
    }
    const DescriptorTablePointer pointer{sizeof(idt) - 1, reinterpret_cast<u64>(&idt)};
    asm volatile("lidt %0" : : "m"(pointer) : "memory");
}

bool idt_loaded() {
    DescriptorTablePointer pointer;
    asm volatile("sidt %0" : "=m"(pointer));
    return pointer.limit == sizeof(idt) - 1 && pointer.base == reinterpret_cast<u64>(&idt);
}

u64 trap_breakpoint_count() {
    return breakpoint_count;
}

}
