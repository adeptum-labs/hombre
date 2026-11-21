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

#include "../common/boot_info.hpp"
#include "../common/serial.hpp"
#include "apic.hpp"
#include "boot_disk.hpp"
#include "boot_framebuffer.hpp"
#include "clock.hpp"
#include "framebuffer.hpp"
#include "gdt.hpp"
#include "idt.hpp"
#include "mem_init.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"
#include "percpu.hpp"
#include "splash.hpp"
#include "task_switch.hpp"

using namespace hombre;

namespace {

PerCpu bsp_cpu;

// Constant initialised, so they sit in .bss without a constructor run.
constinit PhysAllocator phys_pages;
constinit MmioMapper mmio;

[[noreturn]] void halt() {
    for (;;) {
        asm volatile("cli; hlt");
    }
}

u64 usable_mib(const BootInfo& info) {
    u64 pages = 0;
    for (u32 i = 0; i < info.region_count; ++i) {
        if (info.regions[i].type == MemType::Usable) {
            pages += info.regions[i].pages;
        }
    }
    return pages / 256;  // 256 pages of 4 KiB per MiB
}

// The loader's identity map is still in the page tables. Clearing the lower
// half of the PML4 removes it, and reloading CR3 flushes the TLB. Nothing may
// use a lower-half address afterwards.
void unmap_lower_half(u64 direct_map_base) {
    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    volatile u64* pml4 =
        reinterpret_cast<volatile u64*>(direct_map_base + (cr3 & 0x000FFFFFFFFFF000));
    for (u32 i = 0; i < 256; ++i) {
        pml4[i] = 0;
    }
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

#if defined(HOMBRE_FAULT_TEST) && HOMBRE_FAULT_TEST != 4
// Provokes one deliberate CPU fault so the boot check can look for its dump.
// The exception handler halts, so this never returns.
[[noreturn]] void fault_test() {
    serial::write("fault test: ");
    serial::write_dec(HOMBRE_FAULT_TEST);
    serial::write("\n");
#if HOMBRE_FAULT_TEST == 1
    // The lower half is unmapped, so this read raises a page fault.
    *reinterpret_cast<volatile u64*>(0x1000);
#elif HOMBRE_FAULT_TEST == 2
    // Delivering the breakpoint pushes its frame onto the unmapped stack,
    // which faults; that escalates to a double fault on its own IST stack.
    asm volatile("mov %0, %%rsp; int3" : : "r"(u64{0x1000}) : "memory");
#elif HOMBRE_FAULT_TEST == 3
    // Vector 128 is not an exception, so it gets the unexpected-interrupt dump.
    asm volatile("int $0x80");
#else
#error "HOMBRE_FAULT_TEST must be 1, 2, 3 or 4"
#endif
    halt();
}
#endif

#if defined(HOMBRE_FAULT_TEST) && HOMBRE_FAULT_TEST == 4
constinit TaskStacks guard_test_stacks{};

// Pushes at the lowest address of its task stack, so the write lands on
// the unmapped guard page below it. The page fault cannot be delivered on
// the same stack either, which escalates to a double fault on its IST
// stack.
void overflow_task(void*) {
    const u64 bottom = guard_test_stacks.stack_bottom(1);
    serial::write("fault test: guard page 0x");
    serial::write_hex(guard_test_stacks.guard_page(1));
    serial::write("\n");
    asm volatile("mov %0, %%rsp; pushq $0" : : "r"(bottom) : "memory");
}

[[noreturn]] void guard_fault_test(PhysAllocator& pages) {
    serial::write("fault test: 4\n");
    if (!guard_test_stacks.init_kernel(pages) ||
        !guard_test_stacks.create(1, &overflow_task, nullptr)) {
        serial::write("fault test: no task stack\n");
        halt();
    }
    guard_test_stacks.switch_to(0, 1);
    halt();
}
#endif

}

extern "C" [[noreturn]] void kmain(BootInfo* info) {
    serial::init();
    if (info == nullptr || info->magic != boot_info_magic ||
        info->version != boot_info_version) {
        serial::write("kernel: unexpected boot info\n");
        halt();
    }

    if (info->fb.phys != 0) {
        Framebuffer fb = make_framebuffer(*info);
        if (!splash_show(fb)) {
            fb.fill(0x000000);
        }
    }

    // The firmware's IDT gates use selectors outside the new GDT, so no
    // exception can be taken between loading the GDT and loading the IDT.
    gdt_init_bsp();
    idt_init_bsp();

    if (read_cs() != kKernelCodeSelector || read_tr() != kTssSelector) {
        serial::write("gdt failed\n");
        halt();
    }
    serial::write("gdt loaded\n");

    if (!idt_loaded()) {
        serial::write("idt failed\n");
        halt();
    }
    serial::write("idt loaded\n");

    asm volatile("int3" : : : "memory");
    if (trap_breakpoint_count() != 1) {
        serial::write("breakpoint failed\n");
        halt();
    }
    serial::write("breakpoint ok\n");

    unmap_lower_half(info->direct_map_base);
    serial::write("lower half unmapped\n");

    percpu_init(bsp_cpu, 0);
    if (this_cpu().cpu_id == 0) {
        serial::write("cpu0 percpu ok\n");
    }

#if defined(HOMBRE_FAULT_TEST) && HOMBRE_FAULT_TEST != 4
    fault_test();
#endif

    serial::write("Hombre kernel booted\n");
    serial::write("usable memory: ");
    serial::write_dec(usable_mib(*info));
    serial::write(" MiB\n");
    if (info->rsdp_phys == 0) {
        serial::write("rsdp: none\n");
    } else {
        serial::write("rsdp: 0x");
        serial::write_hex(info->rsdp_phys);
        serial::write("\n");
    }
    if (info->fb.phys != 0) {
        serial::write("framebuffer: ");
        serial::write_dec(info->fb.width);
        serial::write("x");
        serial::write_dec(info->fb.height);
        serial::write("\n");
    } else {
        serial::write("no framebuffer\n");
    }
    if (mem_init(*info, phys_pages, mmio)) {
#if defined(HOMBRE_FAULT_TEST) && HOMBRE_FAULT_TEST == 4
        guard_fault_test(phys_pages);
#endif
        splash_progress(1, kSplashKernelSteps);
        pic_mask_all();
        clock_init();
        if (apic_init(mmio)) {
            irq_enable();
            splash_progress(2, kSplashKernelSteps);
        }
        boot_disk_run(*info, phys_pages, mmio);
    }
    serial::write("halting\n");
    halt();
}
