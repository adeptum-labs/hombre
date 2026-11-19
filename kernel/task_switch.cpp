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

#include "task_switch.hpp"

#include "../common/serial.hpp"
#include "page_alloc.hpp"

extern "C" [[noreturn]] void task_entry_returned() {
    hombre::serial::write("task: entry returned\n");
    for (;;) {
        asm volatile("cli; hlt");
    }
}

asm(R"(
    .pushsection .text
    .global task_switch
    .type task_switch, @function
task_switch:
    pushfq
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popfq
    ret

    .global task_entry_trampoline
    .type task_entry_trampoline, @function
task_entry_trampoline:
    movq %r12, %rdi
    sti
    call *%r13
    call task_entry_returned
    .popsection
)");

namespace hombre {

namespace {

u64 alloc_stack_page(void* ctx) {
    return static_cast<PhysAllocator*>(ctx)->alloc_pages(1);
}

u64* stack_page_ptr(void* ctx, u64 phys) {
    return reinterpret_cast<u64*>(static_cast<PhysAllocator*>(ctx)->to_ptr(phys));
}

void free_stack_page(void* ctx, u64 phys) {
    static_cast<PhysAllocator*>(ctx)->free_pages(phys, 1);
}

void flush_stack_page(u64 virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

}

bool TaskStacks::init_kernel(PhysAllocator& pages) {
    u64 cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    const PageAllocator table_pages{&alloc_stack_page, &stack_page_ptr, &pages};
    return init(table_pages, cr3 & paging_detail::table_addr_mask, &free_stack_page,
                &flush_stack_page, &pages);
}

}
