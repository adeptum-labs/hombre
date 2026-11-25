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

#include "gdt.hpp"

#include "descriptors.hpp"

namespace hombre {

namespace {

struct Gdt {
    GdtEntry null_entry;
    GdtEntry code;
    GdtEntry data;
    TssDescriptor tss;
};

static_assert(sizeof(Gdt) == 40);
static_assert(__builtin_offsetof(Gdt, code) == kKernelCodeSelector);
static_assert(__builtin_offsetof(Gdt, data) == kKernelDataSelector);
static_assert(__builtin_offsetof(Gdt, tss) == kTssSelector);

constexpr u64 ist_stack_size = 32 * 1024;
constexpr u32 ist_stack_count = 4;

// All of this lives in .bss in the higher half, so it stays valid after the
// lower half is unmapped.
alignas(8) Gdt gdt;
Tss tss;
alignas(16) u8 ist_stacks[ist_stack_count][ist_stack_size];

}

void gdt_init_bsp() {
    for (u32 i = 0; i < ist_stack_count; ++i) {
        tss.ist[i] = reinterpret_cast<u64>(&ist_stacks[i][ist_stack_size]);
    }
    tss.iomap_base = sizeof(Tss);

    gdt.null_entry = GdtEntry{};
    gdt.code = make_code_segment64();
    gdt.data = make_data_segment();
    gdt.tss = make_tss_descriptor(reinterpret_cast<u64>(&tss), sizeof(Tss) - 1);

    const DescriptorTablePointer pointer{sizeof(Gdt) - 1, reinterpret_cast<u64>(&gdt)};
    asm volatile("lgdt %0" : : "m"(pointer) : "memory");

    // CS only changes through a far transfer: push the new selector and the
    // address of the next instruction, then return across them.
    asm volatile(
        "pushq %[code]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:"
        :
        : [code] "i"(kKernelCodeSelector)
        : "rax", "memory");

    // FS and GS are left alone: loading a selector into GS would clear its
    // base. SS must be reloaded: iretq after an exception loads SS and
    // faults with #GP (error 0x30) when it holds a selector outside this GDT.
    const u16 data_selector = kKernelDataSelector;
    asm volatile(
        "mov %0, %%ds\n\t"
        "mov %0, %%es\n\t"
        "mov %0, %%ss"
        :
        : "r"(data_selector)
        : "memory");

    // ltr marks the TSS descriptor busy, so a second load would fault.
    const u16 tss_selector = kTssSelector;
    asm volatile("ltr %0" : : "r"(tss_selector) : "memory");
}

}
