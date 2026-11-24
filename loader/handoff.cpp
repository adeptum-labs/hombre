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

#include "handoff.hpp"

#include "../common/boot_info.hpp"
#include "../common/memmap.hpp"
#include "../common/paging.hpp"
#include "../common/serial.hpp"
#include "error_line.hpp"
#include "gop_choice.hpp"

namespace hombre {

namespace {

constexpr u64 large_page = 0x200000;
constexpr u64 gib = 1ull << 30;
constexpr u64 pdpt_span = 1ull << 39;
constexpr u64 stack_pages = 16;  // 64 KiB
constexpr u32 max_attempts = 8;
// Room in the memory-map buffer for the descriptors that the loader's own
// allocations add after the buffer has been sized.
constexpr u64 spare_descriptors = 32;
// Extra page-table pages beyond the estimate from the first memory map.
constexpr u64 spare_table_pages = 16;

[[noreturn]] void halt() {
    for (;;) {
        asm volatile("cli; hlt");
    }
}

// Pass st == nullptr once the firmware must no longer be called.
[[noreturn]] void fatal(EfiSystemTable* st, const char* text) {
    ErrorLine(text).emit(st);
    halt();
}

[[noreturn]] void fatal(EfiSystemTable* st, const char* text, EfiStatus status) {
    ErrorLine(text).append(" status=").append_hex(status).emit(st);
    halt();
}

u64 pages_for(u64 bytes) { return (bytes + efi_page_size - 1) / efi_page_size; }

u64 round_down_large(u64 value) { return value & ~(large_page - 1); }

u64 round_up_large(u64 value) { return round_down_large(value + large_page - 1); }

u64 allocate(EfiSystemTable* st, u64 pages, const char* what) {
    u64 phys = 0;
    const EfiStatus status = st->boot_services->allocate_pages(
        EfiAllocateType::AllocateAnyPages, EfiMemoryType::LoaderData, pages, &phys);
    if (status != efi_success) {
        ErrorLine("cannot allocate ").append(what).append(" status=").append_hex(status).emit(st);
        halt();
    }
    return phys;
}

bool same_guid(const EfiGuid& a, const EfiGuid& b) {
    if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (a.data4[i] != b.data4[i]) {
            return false;
        }
    }
    return true;
}

bool usable_format(EfiPixelFormat format) {
    return format == EfiPixelFormat::BlueGreenRedReserved8 ||
           format == EfiPixelFormat::RedGreenBlueReserved8;
}

// Switches the GOP to its mode with the most pixels and a 32-bit
// framebuffer, so the largest Hombre screen mode shows one to one. A mode
// that cannot be queried counts as unusable; a failed switch keeps the
// mode the firmware chose.
void choose_largest_mode(EfiSystemTable* st, EfiGraphicsOutput* gop) {
    constexpr u32 kMaxModes = 64;
    GopModeSize sizes[kMaxModes]{};
    const u32 count = gop->mode->max_mode < kMaxModes ? gop->mode->max_mode : kMaxModes;
    for (u32 i = 0; i < count; ++i) {
        usize size = 0;
        EfiGraphicsModeInfo* info = nullptr;
        if (gop->query_mode(gop, i, &size, &info) == efi_success && info != nullptr) {
            sizes[i] = GopModeSize{info->horizontal_resolution, info->vertical_resolution,
                                   usable_format(info->pixel_format)};
            st->boot_services->free_pool(info);
        }
    }
    const u32 best = best_gop_mode(sizes, count, gop->mode->mode);
    if (best != gop->mode->mode) {
        gop->set_mode(gop, best);
    }
}

// A missing GOP, or one without a linear 32-bit framebuffer, leaves
// fb.phys at 0.
FramebufferInfo find_framebuffer(EfiSystemTable* st, u64& fb_bytes) {
    FramebufferInfo fb{};
    fb_bytes = 0;

    EfiGraphicsOutput* gop = nullptr;
    const EfiStatus status = st->boot_services->locate_protocol(
        &efi_graphics_output_guid, nullptr, reinterpret_cast<void**>(&gop));
    if (status != efi_success || gop == nullptr || gop->mode == nullptr ||
        gop->mode->info == nullptr || gop->mode->frame_buffer_base == 0 ||
        gop->mode->frame_buffer_size == 0) {
        return fb;
    }

    choose_largest_mode(st, gop);
    if (gop->mode->info == nullptr || gop->mode->frame_buffer_base == 0) {
        return fb;
    }
    const EfiGraphicsModeInfo& info = *gop->mode->info;
    switch (info.pixel_format) {
    case EfiPixelFormat::BlueGreenRedReserved8:
        fb.format = kFramebufferBgrx8;
        break;
    case EfiPixelFormat::RedGreenBlueReserved8:
        fb.format = kFramebufferRgbx8;
        break;
    default:
        return fb;
    }

    fb.phys = gop->mode->frame_buffer_base;
    fb.width = info.horizontal_resolution;
    fb.height = info.vertical_resolution;
    fb.pitch_bytes = info.pixels_per_scan_line * 4;
    fb_bytes = gop->mode->frame_buffer_size;
    return fb;
}

// 0 when the firmware publishes no ACPI 2.0 table.
u64 find_rsdp(EfiSystemTable* st) {
    for (usize i = 0; i < st->number_of_table_entries; ++i) {
        const EfiConfigurationTable& table = st->configuration_table[i];
        if (same_guid(table.vendor_guid, efi_acpi_20_table_guid)) {
            return reinterpret_cast<u64>(table.vendor_table);
        }
    }
    return 0;
}

// The physical ranges that get identity mapped and direct mapped:
// [0, ram_end) and, when the framebuffer lies above it, [fb_start, fb_end).
// All bounds are 2 MiB aligned.
struct Coverage {
    u64 ram_end;
    u64 fb_start;
    u64 fb_end;
};

Coverage coverage_of(const MemoryRegion* regions, u32 count, const FramebufferInfo& fb,
                     u64 fb_bytes) {
    u64 top = 0;
    for (u32 i = 0; i < count; ++i) {
        if (regions[i].type == MemType::Mmio) {
            continue;
        }
        const u64 end = regions[i].phys + regions[i].pages * efi_page_size;
        if (end > top) {
            top = end;
        }
    }

    Coverage coverage{round_up_large(top), 0, 0};
    if (fb.phys != 0) {
        // Only the part above ram_end needs a mapping of its own.
        u64 start = round_down_large(fb.phys);
        const u64 end = round_up_large(fb.phys + fb_bytes);
        if (start < coverage.ram_end) {
            start = coverage.ram_end;
        }
        if (end > start) {
            coverage.fb_start = start;
            coverage.fb_end = end;
        }
    }
    return coverage;
}

// An upper bound on the PDPT and PD pages one range of `bytes` needs: a
// range that is not aligned to a table's span can touch one more table at
// each end.
u64 tables_for(u64 bytes) { return bytes / gib + 2 + bytes / pdpt_span + 2; }

u64 table_pages_for(const Coverage& coverage) {
    const u64 identity_and_direct =
        tables_for(coverage.ram_end) + tables_for(coverage.fb_end - coverage.fb_start);
    return 1 + 2 * identity_and_direct + tables_for(kKernelWindowSize) + spare_table_pages;
}

// Hands out the pages of a block allocated before boot services were exited.
// The firmware's identity mapping is still active while the tables are
// built, so physical addresses can be used as pointers.
struct TableArena {
    u64 next;
    u64 end;

    static u64 alloc(void* ctx) {
        TableArena* arena = static_cast<TableArena*>(ctx);
        if (arena->next == arena->end) {
            return 0;
        }
        const u64 phys = arena->next;
        arena->next += efi_page_size;
        u64* page = reinterpret_cast<u64*>(phys);
        for (u64 i = 0; i < efi_page_size / sizeof(u64); ++i) {
            page[i] = 0;
        }
        return phys;
    }

    static u64* to_ptr(void*, u64 phys) { return reinterpret_cast<u64*>(phys); }
};

u64 build_page_tables(const Coverage& coverage, TableArena& arena) {
    PageTables tables(PageAllocator{&TableArena::alloc, &TableArena::to_ptr, &arena});
    if (!tables.ok()) {
        return 0;
    }

    const u64 flags = Present | Writable;
    // The identity map keeps the loader running across the switch to these
    // tables; the kernel itself only uses the direct map and its window.
    bool ok = tables.map_2m(0, 0, coverage.ram_end, flags) &&
              tables.map_2m(kDirectMapBase, 0, coverage.ram_end, flags) &&
              tables.map_2m(kKernelBase, 0, kKernelWindowSize, flags);
    if (ok && coverage.fb_end > coverage.fb_start) {
        const u64 bytes = coverage.fb_end - coverage.fb_start;
        ok = tables.map_2m(coverage.fb_start, coverage.fb_start, bytes, flags) &&
             tables.map_2m(kDirectMapBase + coverage.fb_start, coverage.fb_start, bytes, flags);
    }
    return ok && tables.ok() ? tables.root() : 0;
}

// Interrupts must already be off. The kernel entry is entered as if called,
// with a zero return address on its stack, so the stack is aligned the way
// the SysV ABI expects at function entry.
[[noreturn]] void enter_kernel(u64 root, u64 stack_top, u64 boot_info, u64 entry) {
    asm volatile(
        "mov %0, %%cr3\n\t"
        "mov %1, %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "pushq $0\n\t"
        "jmp *%3"
        :
        : "c"(root), "d"(stack_top), "D"(boot_info), "a"(entry)
        : "memory");
    __builtin_unreachable();
}

}

void handoff_to_kernel(EfiHandle image, EfiSystemTable* st, const LoadedKernel& kernel) {
    EfiBootServices* bs = st->boot_services;

    u64 fb_bytes = 0;
    const FramebufferInfo fb = find_framebuffer(st, fb_bytes);
    const u64 rsdp_phys = find_rsdp(st);

    // Size the memory-map buffer, growing it for as long as the firmware
    // reports it too small.
    usize map_size = 0;
    usize map_key = 0;
    usize descriptor_size = 0;
    u32 descriptor_version = 0;
    EfiStatus status =
        bs->get_memory_map(&map_size, nullptr, &map_key, &descriptor_size, &descriptor_version);
    if (status != efi_buffer_too_small) {
        fatal(st, "cannot size the memory map", status);
    }
    if (descriptor_size < sizeof(EfiMemoryDescriptor)) {
        fatal(st, "unexpected memory map descriptor size");
    }

    u64 map_phys = 0;
    u64 map_pages = 0;
    for (u32 attempt = 0;; ++attempt) {
        if (attempt == max_attempts) {
            fatal(st, "memory map keeps outgrowing its buffer");
        }
        map_pages = pages_for(map_size + spare_descriptors * descriptor_size);
        map_phys = allocate(st, map_pages, "memory map buffer");
        map_size = map_pages * efi_page_size;
        status = bs->get_memory_map(&map_size, reinterpret_cast<void*>(map_phys), &map_key,
                                    &descriptor_size, &descriptor_version);
        if (status == efi_success) {
            break;
        }
        bs->free_pages(map_phys, map_pages);
        if (status != efi_buffer_too_small) {
            fatal(st, "cannot read the memory map", status);
        }
    }
    if (descriptor_size < sizeof(EfiMemoryDescriptor)) {
        fatal(st, "unexpected memory map descriptor size");
    }

    // Every descriptor the buffer can hold fits in the region array, so the
    // conversion of a later map into it cannot run out of room.
    const u64 map_capacity = map_pages * efi_page_size;
    const u32 region_capacity = static_cast<u32>(map_capacity / descriptor_size);
    const u64 regions_phys =
        allocate(st, pages_for(region_capacity * sizeof(MemoryRegion)), "memory region array");
    MemoryRegion* regions = reinterpret_cast<MemoryRegion*>(regions_phys);

    u32 region_count = convert_efi_map(reinterpret_cast<const u8*>(map_phys), map_size,
                                       descriptor_size, regions, region_capacity);
    if (region_count == 0) {
        fatal(st, "cannot convert the memory map");
    }
    const u64 table_pages = table_pages_for(coverage_of(regions, region_count, fb, fb_bytes));
    const u64 tables_phys = allocate(st, table_pages, "page tables");
    const u64 stack_phys = allocate(st, stack_pages, "kernel stack");
    const u64 boot_info_phys = allocate(st, 1, "boot info");

    // Firmware without a watchdog fails this call, which is harmless.
    bs->set_watchdog_timer(0, 0, 0, nullptr);

    // Nothing may allocate from here on: any allocation would change the
    // memory map and invalidate its key. A stale key only re-reads the map
    // into the buffer sized above.
    u32 attempts = 0;
    for (;;) {
        if (attempts == max_attempts) {
            fatal(nullptr, "cannot exit boot services: the memory map keeps changing");
        }
        ++attempts;
        map_size = map_capacity;
        status = bs->get_memory_map(&map_size, reinterpret_cast<void*>(map_phys), &map_key,
                                    &descriptor_size, &descriptor_version);
        if (status != efi_success) {
            fatal(nullptr, "cannot read the final memory map", status);
        }
        status = bs->exit_boot_services(image, map_key);
        if (status == efi_success) {
            break;
        }
        if (status != efi_invalid_parameter) {
            fatal(nullptr, "cannot exit boot services", status);
        }
    }

    // The firmware's interrupt handlers are gone with boot services.
    asm volatile("cli");

    if (attempts > 1) {
        serial::write("boot services exited after ");
        serial::write_dec(attempts);
        serial::write(" attempts\n");
    }

    region_count = convert_efi_map(reinterpret_cast<const u8*>(map_phys), map_size,
                                   descriptor_size, regions, region_capacity);
    if (region_count == 0) {
        fatal(nullptr, "cannot convert the final memory map");
    }

    TableArena arena{tables_phys, tables_phys + table_pages * efi_page_size};
    const u64 root =
        build_page_tables(coverage_of(regions, region_count, fb, fb_bytes), arena);
    if (root == 0) {
        fatal(nullptr, "cannot build the page tables");
    }

    BootInfo* info = reinterpret_cast<BootInfo*>(boot_info_phys);
    *info = BootInfo{};
    info->magic = boot_info_magic;
    info->version = boot_info_version;
    info->region_count = region_count;
    info->regions = reinterpret_cast<MemoryRegion*>(kDirectMapBase + regions_phys);
    info->fb = fb;
    info->rsdp_phys = rsdp_phys;
    info->direct_map_base = kDirectMapBase;
    info->kernel_phys_start = kernel.phys_start;
    info->kernel_phys_end = kernel.phys_end;

    enter_kernel(root, kDirectMapBase + stack_phys + stack_pages * efi_page_size,
                 kDirectMapBase + boot_info_phys, kernel.entry_virt);
}

}
