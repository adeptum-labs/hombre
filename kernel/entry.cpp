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
#include "framebuffer.hpp"
#include "gdt.hpp"
#include "idt.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"
#include "pci.hpp"
#include "percpu.hpp"
#include "virtio_blk.hpp"

using namespace hombre;

namespace {

PerCpu bsp_cpu;

// Constant initialised, so they sit in .bss without a constructor run.
constinit u64 direct_map_base = 0;
constinit PhysAllocator phys_pages;
constinit MmioMapper mmio;
constinit VirtioBlk boot_disk;

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

void draw_banner(const BootInfo& info) {
    constexpr const char* text = "Hombre";
    constexpr u32 scale = 4;
    constexpr u32 text_width = 6 * 8 * scale;
    Framebuffer fb(reinterpret_cast<u8*>(info.direct_map_base + info.fb.phys), info.fb.width,
                   info.fb.height, info.fb.pitch_bytes, info.fb.format);
    fb.fill(0x000040);
    const u32 x = info.fb.width > text_width ? (info.fb.width - text_width) / 2 : 0;
    fb.draw_text(x, info.fb.height / 4, text, 0xE0E0FF, scale);
}

u8* direct_map_ptr(void* ctx, u64 phys) {
    return reinterpret_cast<u8*>(*static_cast<const u64*>(ctx) + phys);
}

// Page-table pages for the MMIO window come from the physical allocator.
u64 alloc_table_page(void* ctx) {
    return static_cast<PhysAllocator*>(ctx)->alloc_pages(1);
}

u64* table_page_ptr(void* ctx, u64 phys) {
    return reinterpret_cast<u64*>(static_cast<PhysAllocator*>(ctx)->to_ptr(phys));
}

// The MMIO window maps device memory no-execute, which needs EFER.NXE.
// Returns false when the CPU has no NX bit.
bool enable_nx() {
    constexpr u32 efer_msr = 0xC0000080;
    constexpr u32 efer_nxe = 1u << 11;
    u32 eax = 0x80000001, ebx, ecx = 0, edx;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if ((edx & (1u << 20)) == 0) {
        return false;
    }
    u32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(efer_msr));
    asm volatile("wrmsr" : : "a"(lo | efer_nxe), "d"(hi), "c"(efer_msr));
    return true;
}

// Prints value in lowercase hex, zero padded to at least `digits` digits.
void write_hex_padded(u64 value, u32 digits) {
    char text[17];  // 16 hex digits for u64 max, plus the terminator
    const usize count = format_u64(text, sizeof(text), value, 16);
    for (usize i = count; i < digits; ++i) {
        serial::write_char('0');
    }
    serial::write(text);
}

bool log_pci_function(void*, PciAddress address, u16 vendor, u16 device) {
    serial::write("pci: ");
    write_hex_padded(address.bus, 2);
    serial::write(":");
    write_hex_padded(address.device, 2);
    serial::write(".");
    write_hex_padded(address.function, 1);
    serial::write(" ");
    write_hex_padded(vendor, 4);
    serial::write(":");
    write_hex_padded(device, 4);
    serial::write("\n");
    return false;
}

// Brings up the page allocator, the MMIO window and the boot disk, and
// reads the start of the drive. Every failure logs one line and returns.
void boot_disk_check(const BootInfo& info) {
    direct_map_base = info.direct_map_base;
    if (!phys_pages.init(info.regions, info.region_count, &direct_map_ptr, &direct_map_base)) {
        serial::write("page allocator failed\n");
        return;
    }
    if (!enable_nx()) {
        serial::write("no nx support\n");
        return;
    }
    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    const PageAllocator tables{&alloc_table_page, &table_page_ptr, &phys_pages};
    mmio.init(PageTables::adopt(tables, cr3 & 0x000FFFFFFFFFF000));

    pci_scan(&log_pci_function, nullptr);

    PciAddress disk;
    if (!virtio_blk_find(disk)) {
        serial::write("no boot disk\n");
        return;
    }
    if (!boot_disk.init(disk, mmio, phys_pages)) {
        serial::write("virtio-blk: init failed\n");
        return;
    }
    serial::write("virtio-blk: ");
    serial::write_dec(boot_disk.sector_count());
    serial::write(" sectors\n");

    // Reads up to the first 8 MiB of the drive.
    constexpr u64 max_sectors = (8ull << 20) / kVirtioBlkSectorSize;
    const u32 sectors = static_cast<u32>(
        boot_disk.sector_count() < max_sectors ? boot_disk.sector_count() : max_sectors);
    if (sectors == 0) {
        return;
    }
    const u64 bytes = static_cast<u64>(sectors) * kVirtioBlkSectorSize;
    const u64 page_count = (bytes + 4095) / 4096;
    const u64 buffer = phys_pages.alloc_pages(page_count);
    if (buffer == 0) {
        serial::write("virtio-blk: no memory\n");
        return;
    }
    if (boot_disk.read(0, sectors, buffer)) {
        serial::write("virtio-blk: read ok\n");
    } else {
        serial::write("virtio-blk: read failed\n");
        // The device may still write there, so the pages are never reused.
        if (!boot_disk.quiesced()) {
            serial::write("virtio-blk: device not reset, buffer kept\n");
            return;
        }
    }
    phys_pages.free_pages(buffer, page_count);
}

#if defined(HOMBRE_FAULT_TEST)
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
#error "HOMBRE_FAULT_TEST must be 1, 2 or 3"
#endif
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

#if defined(HOMBRE_FAULT_TEST)
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
        draw_banner(*info);
    } else {
        serial::write("no framebuffer\n");
    }
    boot_disk_check(*info);
    serial::write("halting\n");
    halt();
}
