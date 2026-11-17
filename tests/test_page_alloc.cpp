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

#include "boot_info.hpp"
#include "check.hpp"
#include "page_alloc.hpp"

#include <vector>

using namespace hombre;

namespace {

constexpr u64 kPage = 0x1000;
constexpr u64 kRamSize = 4ull << 20;

// Physical memory as the allocator sees it: phys 0 is the start of an
// exactly sized heap array, so ASan flags any access past 4 MiB. Accesses
// outside [base, base + size) fail the test instead of being served.
struct Ram {
    u64 base;
    std::vector<u8> bytes;

    explicit Ram(u64 size = kRamSize, u64 base_phys = 0) : base(base_phys), bytes(size) {}

    static u8* to_ptr(void* ctx, u64 phys) {
        Ram* ram = static_cast<Ram*>(ctx);
        CHECK(phys >= ram->base);
        CHECK(phys - ram->base + kPage <= ram->bytes.size());
        return ram->bytes.data() + (phys - ram->base);
    }

    void fill(u8 value) { bytes.assign(bytes.size(), value); }
    u8* at(u64 phys) { return bytes.data() + (phys - base); }
};

// Usable 0 to 640 KiB, the legacy hole, then three usable runs above 1 MiB
// separated by a Loader range and a one-page Reserved hole, plus memory
// mapped I/O far above the RAM.
//
//   0x000000 +160 Usable     (below 1 MiB, never handed out)
//   0x0A0000 + 96 Reserved
//   0x100000 +128 Usable     (the bitmap takes its first page)
//   0x180000 + 16 Loader
//   0x190000 +367 Usable
//   0x2FF000 +  1 Reserved
//   0x300000 +256 Usable
//   0xFEE00000 +1 Mmio
constexpr u32 kRegionCount = 8;
const MemoryRegion kRegions[kRegionCount] = {
    {0x000000, 160, MemType::Usable, 0},      {0x0A0000, 96, MemType::Reserved, 0},
    {0x100000, 128, MemType::Usable, 0},      {0x180000, 16, MemType::Loader, 0},
    {0x190000, 367, MemType::Usable, 0},      {0x2FF000, 1, MemType::Reserved, 0},
    {0x300000, 256, MemType::Usable, 0},      {0xFEE00000, 1, MemType::Mmio, 0},
};
constexpr u64 kBitmapPhys = 0x100000;
constexpr u64 kFreeAfterInit = 128 + 367 + 256 - 1;

bool allowed_page(u64 phys) {
    return (phys >= 0x101000 && phys < 0x180000) || (phys >= 0x190000 && phys < 0x2FF000) ||
           (phys >= 0x300000 && phys < 0x400000);
}

bool init_default(PhysAllocator& alloc, Ram& ram) {
    return alloc.init(kRegions, kRegionCount, Ram::to_ptr, &ram);
}

// Allocates single pages until the allocator runs dry.
std::vector<u64> drain(PhysAllocator& alloc) {
    std::vector<u64> pages;
    while (u64 page = alloc.alloc_pages(1)) {
        pages.push_back(page);
        CHECK(pages.size() <= kRamSize / kPage);
    }
    return pages;
}

void init_ok_and_counts() {
    Ram ram;
    ram.fill(0xAA);
    PhysAllocator alloc;
    CHECK_EQ(alloc.free_page_count(), 0u);
    CHECK(init_default(alloc, ram));
    CHECK_EQ(alloc.free_page_count(), kFreeAfterInit);
}

void alloc_is_zeroed() {
    Ram ram;
    ram.fill(0xAA);
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));

    const u64 phys = alloc.alloc_pages(4);
    CHECK(phys != 0);
    CHECK_EQ(phys % kPage, 0u);
    for (u64 i = 0; i < 4 * kPage; ++i) {
        CHECK_EQ(ram.at(phys)[i], 0u);
    }
    // Nothing beside the allocation is touched.
    CHECK_EQ(ram.at(phys + 4 * kPage)[0], 0xAAu);
    CHECK_EQ(ram.at(phys - 1)[0], 0xAAu);
    CHECK_EQ(alloc.free_page_count(), kFreeAfterInit - 4);

    // A freed page comes back zeroed even after the owner wrote to it.
    ram.at(phys)[17] = 0x55;
    alloc.free_pages(phys, 4);
    const u64 again = alloc.alloc_pages(4);
    CHECK_EQ(again, phys);
    CHECK_EQ(ram.at(phys)[17], 0u);
}

void alloc_contiguous_across_free_holes() {
    Ram ram;
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));

    const u64 a = alloc.alloc_pages(1);
    const u64 b = alloc.alloc_pages(2);
    const u64 c = alloc.alloc_pages(1);
    CHECK_EQ(a, 0x101000u);
    CHECK_EQ(b, a + kPage);
    CHECK_EQ(c, b + 2 * kPage);

    // A two-page hole at b. Three pages do not fit there.
    alloc.free_pages(b, 2);
    const u64 d = alloc.alloc_pages(3);
    CHECK_EQ(d, c + kPage);
    // Two pages do.
    CHECK_EQ(alloc.alloc_pages(2), b);

    // No run spans a Loader range or a Reserved hole: the longest free run
    // run is 367 pages, so 400 pages cannot fit although the allocator
    // holds more than that in total.
    CHECK(alloc.free_page_count() > 400);
    CHECK_EQ(alloc.alloc_pages(400), 0u);
}

void alloc_fills_runs_exactly() {
    Ram ram;
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));

    // The three runs hold 127, 367 and 256 pages. The 367 and 256 page
    // requests fit them exactly, with no neighbouring Loader or Reserved
    // page included.
    CHECK_EQ(alloc.alloc_pages(367), 0x190000u);
    CHECK_EQ(alloc.alloc_pages(256), 0x300000u);
    CHECK_EQ(alloc.alloc_pages(128), 0u);
    CHECK_EQ(alloc.alloc_pages(127), 0x101000u);
    CHECK_EQ(alloc.free_page_count(), 0u);
}

void alloc_never_returns_below_1mib_or_in_reserved() {
    Ram ram;
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));

    const std::vector<u64> pages = drain(alloc);
    CHECK_EQ(pages.size(), kFreeAfterInit);
    std::vector<bool> seen(kRamSize / kPage, false);
    for (u64 phys : pages) {
        CHECK(allowed_page(phys));
        CHECK(!seen[phys / kPage]);
        seen[phys / kPage] = true;
    }
    CHECK_EQ(alloc.free_page_count(), 0u);
}

void free_then_realloc_same_page() {
    Ram ram;
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));

    const u64 a = alloc.alloc_pages(1);
    const u64 b = alloc.alloc_pages(1);
    CHECK(a != b);
    alloc.free_pages(a, 1);
    CHECK_EQ(alloc.free_page_count(), kFreeAfterInit - 1);
    CHECK_EQ(alloc.alloc_pages(1), a);
    CHECK_EQ(alloc.free_page_count(), kFreeAfterInit - 2);
}

void double_free_ignored() {
    Ram ram;
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));

    const u64 a = alloc.alloc_pages(2);
    alloc.free_pages(a, 2);
    CHECK_EQ(alloc.free_page_count(), kFreeAfterInit);
    alloc.free_pages(a, 2);
    CHECK_EQ(alloc.free_page_count(), kFreeAfterInit);
    // Half of a range already free ignores the whole call.
    const u64 b = alloc.alloc_pages(1);
    CHECK_EQ(b, a);
    alloc.free_pages(a, 2);
    CHECK_EQ(alloc.free_page_count(), kFreeAfterInit - 1);
    // The page that was freed once is allocated exactly once.
    CHECK_EQ(alloc.alloc_pages(1), a + kPage);
    CHECK(alloc.alloc_pages(1) != a + kPage);
}

void free_ignores_invalid_arguments() {
    Ram ram;
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));
    const u64 a = alloc.alloc_pages(1);
    const u64 before = alloc.free_page_count();

    alloc.free_pages(a + 1, 1);                   // misaligned
    alloc.free_pages(a, 0);                       // empty
    alloc.free_pages(0, 1);                       // below 1 MiB
    alloc.free_pages(0x9F000, 1);                 // usable but below 1 MiB
    alloc.free_pages(0xA0000, 1);                 // reserved
    alloc.free_pages(0x180000, 1);                // loader
    alloc.free_pages(0x2FF000, 1);                // hole between usable runs
    alloc.free_pages(kBitmapPhys, 1);             // the bitmap itself
    alloc.free_pages(0x400000, 1);                // one past the last usable page
    alloc.free_pages(0xFEE00000, 1);              // mmio
    alloc.free_pages(~0ull & ~(kPage - 1), 1);    // top of the address space
    alloc.free_pages(a, ~0ull);                   // count overflows
    alloc.free_pages(a, 1ull << 52);              // count far past the end
    alloc.free_pages(0x3FF000, 2);                // runs off the end
    CHECK_EQ(alloc.free_page_count(), before);

    // Everything is still allocated exactly once.
    const std::vector<u64> pages = drain(alloc);
    CHECK_EQ(pages.size(), before);
    for (u64 phys : pages) {
        CHECK(phys != a);
        CHECK(allowed_page(phys));
    }
}

void exhaustion_returns_zero() {
    Ram ram;
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));

    CHECK_EQ(alloc.alloc_pages(0), 0u);
    CHECK_EQ(alloc.alloc_pages(kFreeAfterInit + 1), 0u);
    CHECK_EQ(alloc.alloc_pages(~0ull), 0u);
    CHECK_EQ(alloc.alloc_pages(1ull << 52), 0u);
    CHECK_EQ(alloc.free_page_count(), kFreeAfterInit);

    CHECK_EQ(drain(alloc).size(), kFreeAfterInit);
    CHECK_EQ(alloc.alloc_pages(1), 0u);
    CHECK_EQ(alloc.alloc_pages(2), 0u);
    CHECK_EQ(alloc.free_page_count(), 0u);
}

void bitmap_does_not_overlap_allocations() {
    Ram ram;
    PhysAllocator alloc;
    CHECK(init_default(alloc, ram));

    // Every page is handed out and written to; the bitmap page is never
    // among them, so the allocator keeps working afterwards.
    const std::vector<u64> pages = drain(alloc);
    for (u64 phys : pages) {
        CHECK(phys != kBitmapPhys);
        for (u64 i = 0; i < kPage; ++i) {
            ram.at(phys)[i] = 0xFF;
        }
    }
    CHECK_EQ(alloc.alloc_pages(1), 0u);
    alloc.free_pages(pages[10], 1);
    alloc.free_pages(pages[11], 1);
    CHECK_EQ(alloc.free_page_count(), 2u);
    CHECK_EQ(alloc.alloc_pages(2), pages[10]);
    alloc.free_pages(kBitmapPhys, 1);
    CHECK_EQ(alloc.free_page_count(), 0u);
}

void init_fails_without_usable_memory() {
    Ram ram;
    PhysAllocator alloc;

    const MemoryRegion none[] = {
        {0x100000, 100, MemType::Reserved, 0},
        {0x200000, 100, MemType::Loader, 0},
        {0x300000, 100, MemType::AcpiReclaimable, 0},
        {0x340000, 100, MemType::AcpiNvs, 0},
        {0x380000, 100, MemType::Mmio, 0},
    };
    CHECK(!alloc.init(none, 5, Ram::to_ptr, &ram));
    CHECK_EQ(alloc.alloc_pages(1), 0u);
    CHECK_EQ(alloc.free_page_count(), 0u);

    // Usable memory only below 1 MiB, or zero length, is no usable memory.
    const MemoryRegion low[] = {
        {0, 256, MemType::Usable, 0},
        {0x100000, 0, MemType::Usable, 0},
    };
    CHECK(!alloc.init(low, 2, Ram::to_ptr, &ram));
    CHECK_EQ(alloc.alloc_pages(1), 0u);

    CHECK(!alloc.init(nullptr, 3, Ram::to_ptr, &ram));
    CHECK(!alloc.init(kRegions, 0, Ram::to_ptr, &ram));
    CHECK(!alloc.init(kRegions, kRegionCount, nullptr, &ram));

    // One usable page holds the bitmap and leaves nothing to allocate.
    const MemoryRegion tiny[] = {{0x100000, 1, MemType::Usable, 0}};
    CHECK(alloc.init(tiny, 1, Ram::to_ptr, &ram));
    CHECK_EQ(alloc.free_page_count(), 0u);
    CHECK_EQ(alloc.alloc_pages(1), 0u);

    // No run is long enough for the two-page bitmap that 20481 pages need.
    const MemoryRegion scattered[] = {
        {0x100000, 1, MemType::Usable, 0},
        {0x101000, 1, MemType::Reserved, 0},
        {0x102000, 1, MemType::Usable, 0},
        {0x103000, 1, MemType::Reserved, 0},
        {0x5000000, 1, MemType::Usable, 0},
    };
    CHECK(!alloc.init(scattered, 5, Ram::to_ptr, &ram));
    CHECK_EQ(alloc.alloc_pages(1), 0u);
    CHECK_EQ(alloc.free_page_count(), 0u);

    // A failed init after a good one leaves nothing to allocate.
    CHECK(init_default(alloc, ram));
    CHECK(!alloc.init(none, 5, Ram::to_ptr, &ram));
    CHECK_EQ(alloc.alloc_pages(1), 0u);
    CHECK_EQ(alloc.free_page_count(), 0u);
}

void non_usable_regions_win_over_overlapping_usable_ones() {
    Ram ram;
    PhysAllocator alloc;
    // Unsorted, and a Reserved range sits inside a Usable one.
    const MemoryRegion regions[] = {
        {0x300000, 256, MemType::Usable, 0},
        {0x200000, 0x40, MemType::Reserved, 0},
        {0x100000, 512, MemType::Usable, 0},
        {0x1F0000, 0x20, MemType::AcpiNvs, 0},
        {0x1F8000, 0x20, MemType::Usable, 0},
    };
    CHECK(alloc.init(regions, 5, Ram::to_ptr, &ram));
    // The AcpiNvs range covers 0x1F0000 to 0x210000 and the Reserved one
    // 0x200000 to 0x240000. That leaves 0x100000 to 0x1F0000 and 0x240000
    // to 0x400000, less the bitmap page.
    const u64 expected = (0x1F0 - 0x100) + (0x400 - 0x240) - 1;
    CHECK_EQ(alloc.free_page_count(), expected);

    for (u64 phys : drain(alloc)) {
        CHECK(phys >= 0x101000);
        CHECK(phys < 0x1F0000 || phys >= 0x240000);
        CHECK(phys < 0x400000);
    }
}

void unaligned_regions_are_trimmed_inwards() {
    Ram ram;
    PhysAllocator alloc;
    // The usable range loses its partial first and last pages; the reserved
    // range loses every page it touches.
    const MemoryRegion regions[] = {
        {0x100800, 0x20, MemType::Usable, 0},       // 0x101000 up to 0x120000, 31 pages
        {0x110800, 1, MemType::Reserved, 0},        // reaches 0x110000 and 0x111000
    };
    CHECK(alloc.init(regions, 2, Ram::to_ptr, &ram));
    // 31 pages from 0x101000, minus the bitmap page and the reserved pages
    // 0x110000 and 0x111000.
    CHECK_EQ(alloc.free_page_count(), 31u - 1 - 2);
    for (u64 phys : drain(alloc)) {
        CHECK(phys >= 0x102000);
        CHECK(phys < 0x120000);
        CHECK(phys != 0x110000 && phys != 0x111000);
    }
}

void region_end_overflow_is_safe() {
    Ram ram;
    PhysAllocator alloc;
    const MemoryRegion regions[] = {
        // A usable region whose end wraps is dropped.
        {0xFFFFFFFFFFFFE000ull, 4, MemType::Usable, 0},
        {0x100000, 1ull << 60, MemType::Usable, 0},
        // A reserved region whose end wraps covers the rest of memory.
        {0x300000, ~0ull, MemType::Reserved, 0},
        {0x100000, 256, MemType::Usable, 0},
    };
    CHECK(alloc.init(regions, 4, Ram::to_ptr, &ram));
    CHECK_EQ(alloc.free_page_count(), 255u);
    for (u64 phys : drain(alloc)) {
        CHECK(phys >= 0x101000);
        CHECK(phys < 0x200000);
    }
}

void bitmap_goes_into_first_run_that_fits() {
    // 20000 pages need a bitmap of 2 * 2500 bytes, two pages. The first run
    // is one page, the second three, the third the rest. The test RAM only
    // backs the first 16 pages, which is enough for the pages used here.
    Ram ram(16 * kPage, 0x100000);
    PhysAllocator alloc;
    const MemoryRegion regions[] = {
        {0x100000, 1, MemType::Usable, 0},
        {0x101000, 2, MemType::Reserved, 0},
        {0x103000, 3, MemType::Usable, 0},
        {0x106000, 1, MemType::Reserved, 0},
        {0x107000, 20000 - 0x107, MemType::Usable, 0},
    };
    CHECK(alloc.init(regions, 5, Ram::to_ptr, &ram));
    CHECK_EQ(alloc.free_page_count(), 1u + 3 - 2 + (20000 - 0x107));
    // The first allocation is the leftover page after the bitmap.
    CHECK_EQ(alloc.alloc_pages(1), 0x100000u);
    CHECK_EQ(alloc.alloc_pages(1), 0x105000u);
}

void bitmap_run_may_span_adjacent_regions() {
    Ram ram(16 * kPage, 0x100000);
    PhysAllocator alloc;
    // 20000 pages again; the two-page run is split across two Usable
    // regions that touch each other.
    const MemoryRegion regions[] = {
        {0x100000, 1, MemType::Usable, 0},
        {0x101000, 1, MemType::Usable, 0},
        {0x102000, 1, MemType::Reserved, 0},
        {0x103000, 20000 - 0x103, MemType::Usable, 0},
    };
    CHECK(alloc.init(regions, 4, Ram::to_ptr, &ram));
    CHECK_EQ(alloc.free_page_count(), 20000u - 0x103);
}

}

int main() {
    init_ok_and_counts();
    alloc_is_zeroed();
    alloc_contiguous_across_free_holes();
    alloc_fills_runs_exactly();
    alloc_never_returns_below_1mib_or_in_reserved();
    free_then_realloc_same_page();
    double_free_ignored();
    free_ignores_invalid_arguments();
    exhaustion_returns_zero();
    bitmap_does_not_overlap_allocations();
    init_fails_without_usable_memory();
    non_usable_regions_win_over_overlapping_usable_ones();
    unaligned_regions_are_trimmed_inwards();
    region_end_overflow_is_safe();
    bitmap_goes_into_first_run_that_fits();
    bitmap_run_may_span_adjacent_regions();
    return check::finish("test_page_alloc");
}
