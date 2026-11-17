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

#include "check.hpp"
#include "fake_pages.hpp"
#include "mmio.hpp"
#include "paging.hpp"

#include <cstdint>

using namespace hombre;

namespace {

constexpr u64 k2M = 0x200000;
constexpr u64 kHigherHalf = 0xFFFFFFFF80000000ull;
constexpr u64 kDirectMap = 0xFFFF800000000000ull;

using fake_pages::Arena;

Arena arena;

// A stand-in for the loader's live tables: kernel window and direct map, no
// MMIO window. The mapper adopts this root.
struct Fixture {
    PageTables live;
    MmioMapper mapper;

    // `spare` is how many more table pages the allocator hands out once the
    // live tables stand.
    explicit Fixture(usize spare = 64) {
        arena.reset();
        live = PageTables(arena.allocator());
        live.map_2m(kHigherHalf, 0, 16 * k2M, Present | Writable);
        live.map_2m(kDirectMap, 0, 16 * k2M, Present | Writable | NoExecute);
        arena.limit = arena.allocated + spare;
        mapper.init(PageTables::adopt(arena.allocator(), live.root()));
    }
};

u64 as_u64(volatile void* pointer) {
    return static_cast<u64>(reinterpret_cast<std::uintptr_t>(pointer));
}

bool maps_to(const Fixture& f, u64 virt, u64 expected) {
    u64 phys = 0;
    return translate(f.live.root(), arena.allocator(), virt, phys) && phys == expected;
}

u64* raw_table(u64 phys) { return Arena::to_ptr(nullptr, phys); }

// The raw walk to the MMIO window's page directory uses literal indices and
// masks, so it does not share anything with the builder: PML4 index 384,
// PDPT index 0.
u64* window_directory(const Fixture& f) {
    const u64 pml4_entry = raw_table(f.live.root())[384];
    const u64* pdpt = raw_table(pml4_entry & 0x000FFFFFFFFFF000ull);
    return raw_table(pdpt[0] & 0x000FFFFFFFFFF000ull);
}

constexpr u64 kUncachedLeaf = (1ull << 63) | 0x9B;  // NX, PCD, PWT, PS, W, P

void window_base_is_in_an_unused_slot() {
    CHECK_EQ(kMmioBase, 0xFFFFC00000000000ull);
    CHECK_EQ((kMmioBase >> 39) & 0x1FF, 384ull);
    CHECK_EQ(kMmioMaxMappings, 16u);
    Fixture f;
    CHECK_EQ(raw_table(f.live.root())[384], 0ull);
}

void map_returns_offset_inside_window() {
    Fixture f;
    const u64 phys = 0xFEBD1000;  // 2 MiB page 0xFEA00000, offset 0x1D1000
    const u64 virt = as_u64(f.mapper.map(phys, 0x4000));
    CHECK_EQ(virt, kMmioBase + 0x1D1000);
    CHECK(maps_to(f, virt, phys));
    CHECK(maps_to(f, virt + 0x3FFF, phys + 0x3FFF));
    CHECK(maps_to(f, kMmioBase, 0xFEA00000ull));
    CHECK(maps_to(f, kMmioBase + k2M - 1, 0xFEBFFFFFull));

    const u64* pd = window_directory(f);
    CHECK_EQ(pd[0], 0xFEA00000ull | kUncachedLeaf);  // uncached, writable, no-execute
    CHECK_EQ(pd[1], 0ull);

    // The loader's mappings are untouched.
    CHECK(maps_to(f, kHigherHalf + 0x1234, 0x1234));
    CHECK(maps_to(f, kDirectMap + 0x5678, 0x5678));
}

void same_2m_page_reused() {
    Fixture f;
    volatile void* first = f.mapper.map(0xFEBD0000, 0x1000);
    const usize pages_after_first = arena.allocated;
    volatile void* second = f.mapper.map(0xFEBD4000, 0x1000);  // 16 KiB further on
    CHECK(first != nullptr);
    CHECK_EQ(as_u64(second) - as_u64(first), 0x4000ull);
    CHECK_EQ(arena.allocated, pages_after_first);  // no new table

    // The same request again, and a third BAR elsewhere in the same 2 MiB.
    CHECK_EQ(as_u64(f.mapper.map(0xFEBD0000, 0x1000)), as_u64(first));
    CHECK_EQ(as_u64(f.mapper.map(0xFEA00000, 0x10)), kMmioBase);
    const u64* pd = window_directory(f);
    CHECK(pd[0] != 0);
    CHECK_EQ(pd[1], 0ull);  // only one page is mapped

    // A different 2 MiB page takes the next slot of the window.
    CHECK_EQ(as_u64(f.mapper.map(0xFC000000, 0x1000)), kMmioBase + k2M);
    CHECK_EQ(pd[1], 0xFC000000ull | kUncachedLeaf);
}

void unaligned_phys_keeps_offset() {
    Fixture f;
    const u64 phys = 0x12345677;
    const u64 virt = as_u64(f.mapper.map(phys, 1));
    CHECK_EQ(virt & 0x1FFFFF, phys & 0x1FFFFF);
    CHECK_EQ(virt, kMmioBase + 0x145677);
    CHECK(maps_to(f, virt, phys));
    // The page base is 2 MiB aligned: the first byte of the page maps too.
    CHECK(maps_to(f, kMmioBase, 0x12200000ull));
}

void range_crossing_2m_boundary_maps_two_pages() {
    Fixture f;
    const u64 phys = 0xC0200000 - 0x1000;
    const u64 virt = as_u64(f.mapper.map(phys, 0x3000));
    CHECK_EQ(virt, kMmioBase + k2M - 0x1000);
    for (u64 offset = 0; offset < 0x3000; offset += 0x800) {
        CHECK(maps_to(f, virt + offset, phys + offset));  // contiguous in the window
    }
    CHECK(maps_to(f, virt + 0x2FFF, phys + 0x2FFF));
    const u64* pd = window_directory(f);
    CHECK_EQ(pd[0], 0xC0000000ull | kUncachedLeaf);
    CHECK_EQ(pd[1], 0xC0200000ull | kUncachedLeaf);
    CHECK_EQ(pd[2], 0ull);

    // A repeat request reuses the pair.
    CHECK_EQ(as_u64(f.mapper.map(phys, 0x3000)), virt);
    CHECK_EQ(as_u64(f.mapper.map(0xC0200010, 4)), kMmioBase + k2M + 0x10);
    CHECK_EQ(pd[2], 0ull);
}

void crossing_range_with_first_page_mapped_elsewhere_gets_a_fresh_pair() {
    Fixture f;
    // Page 0xC0000000 is mapped alone at slot 0, page 0xD0000000 at slot 1.
    CHECK_EQ(as_u64(f.mapper.map(0xC0000000, 0x1000)), kMmioBase);
    CHECK_EQ(as_u64(f.mapper.map(0xD0000000, 0x1000)), kMmioBase + k2M);

    // A range crossing from 0xC0000000's neighbour: the pair needs slots
    // that are adjacent, and slot 0 is followed by an unrelated page.
    const u64 phys = 0xC0200000 - 0x10;
    const u64 virt = as_u64(f.mapper.map(phys, 0x20));
    CHECK_EQ(virt, kMmioBase + 3 * k2M - 0x10);  // fresh pair in slots 2 and 3
    CHECK(maps_to(f, virt, phys));
    CHECK(maps_to(f, virt + 0x1F, phys + 0x1F));
    const u64* pd = window_directory(f);
    CHECK_EQ(pd[0], 0xC0000000ull | kUncachedLeaf);
    CHECK_EQ(pd[2], 0xC0000000ull | kUncachedLeaf);
    CHECK_EQ(pd[3], 0xC0200000ull | kUncachedLeaf);

    // The pair is found again, and a single-page request still resolves.
    CHECK_EQ(as_u64(f.mapper.map(phys, 0x20)), virt);
    CHECK_EQ(as_u64(f.mapper.map(0xC0000100, 4)), kMmioBase + 0x100);
}

void above_4gib_phys() {
    Fixture f;
    const u64 phys = 0x8000001000ull;  // 512 GiB + 4 KiB, a 64-bit BAR
    const u64 virt = as_u64(f.mapper.map(phys, 0x2000));
    CHECK_EQ(virt, kMmioBase + 0x1000);
    CHECK(maps_to(f, virt, phys));
    CHECK(maps_to(f, virt + 0x1FFF, phys + 0x1FFF));
    CHECK_EQ(window_directory(f)[0], 0x8000000000ull | kUncachedLeaf);

    const u64 high = (1ull << 51) - 0x1000;  // last 4 KiB below 2^51
    CHECK_EQ(as_u64(f.mapper.map(high, 0x1000)), kMmioBase + k2M + 0x1FF000);
    CHECK(maps_to(f, kMmioBase + k2M + 0x1FF000, high));

    // Beyond the 52-bit physical limit nothing is mapped.
    CHECK(f.mapper.map(1ull << 52, 0x1000) == nullptr);
    CHECK(f.mapper.map((1ull << 52) - 0x1000, 0x2000) == nullptr);
}

void invalid_requests_return_nullptr() {
    Fixture f;
    CHECK(f.mapper.map(0xFEBD0000, 0) == nullptr);
    CHECK(f.mapper.map(~0ull, 2) == nullptr);                 // phys + bytes wraps
    CHECK(f.mapper.map(0xFEBD0000, ~0ull) == nullptr);
    CHECK(f.mapper.map(0, 17 * k2M) == nullptr);              // more than the table holds
    CHECK(f.mapper.map(0x1000, 16 * k2M) == nullptr);         // 17 pages
    CHECK_EQ(raw_table(f.live.root())[384], 0ull);            // nothing was mapped
    CHECK(f.mapper.map(0xFEBD0000, 0x1000) != nullptr);

    // A mapper that was never given tables maps nothing.
    MmioMapper idle;
    CHECK(idle.map(0xFEBD0000, 0x1000) == nullptr);
    idle.init(PageTables::adopt(arena.allocator(), 0));
    CHECK(idle.map(0xFEBD0000, 0x1000) == nullptr);
}

void second_init_is_refused() {
    Fixture f;
    volatile void* first = f.mapper.map(0xFEBD0000, 0x1000);
    CHECK(first != nullptr);
    // A second init would hand out slot 0 again while its entry is present.
    CHECK(!f.mapper.init(PageTables::adopt(arena.allocator(), f.live.root())));
    CHECK_EQ(as_u64(f.mapper.map(0xFEBD0000, 0x1000)), as_u64(first));
    CHECK_EQ(as_u64(f.mapper.map(0xFC000000, 0x1000)), kMmioBase + k2M);
}

void table_exhaustion_returns_nullptr() {
    // One spare page: the window's PDPT is built, its PD cannot be.
    Fixture starved(1);
    CHECK(starved.mapper.map(0xFEBD0000, 0x1000) == nullptr);
    CHECK(starved.mapper.map(0xFEBD0000, 0x1000) == nullptr);
    CHECK(starved.mapper.map(0xFC000000, 0x1000) == nullptr);

    // With room for the window's tables the mapper works, and keeps serving
    // mapped ranges once the allocator has run dry.
    Fixture f;
    volatile void* mapped = f.mapper.map(0xFEBD0000, 0x1000);
    CHECK(mapped != nullptr);
    arena.limit = arena.allocated;
    CHECK_EQ(as_u64(f.mapper.map(0xFEBD0000, 0x1000)), as_u64(mapped));
    CHECK_EQ(as_u64(f.mapper.map(0xFEBD8000, 0x100)), as_u64(mapped) + 0x8000);
    // A new page in the same directory needs no new table.
    CHECK(f.mapper.map(0xFC000000, 0x1000) != nullptr);
}

void mapping_table_full_returns_nullptr() {
    Fixture f;
    for (u64 i = 0; i < kMmioMaxMappings; ++i) {
        const u64 phys = 0x100000000ull + i * 0x10000000ull;
        CHECK_EQ(as_u64(f.mapper.map(phys + 0x40, 8)), kMmioBase + i * k2M + 0x40);
    }
    // Slot 17 and beyond.
    CHECK(f.mapper.map(0x300000000ull, 8) == nullptr);
    CHECK(f.mapper.map(0x300000000ull, 8) == nullptr);

    // Everything mapped earlier is still served, first to last.
    CHECK_EQ(as_u64(f.mapper.map(0x100000000ull + 0x80, 8)), kMmioBase + 0x80);
    const u64 last = 0x100000000ull + 15 * 0x10000000ull;
    CHECK_EQ(as_u64(f.mapper.map(last + 0x1FFFF8, 8)), kMmioBase + 15 * k2M + 0x1FFFF8);
    const u64* pd = window_directory(f);
    CHECK_EQ(pd[15], last | kUncachedLeaf);
    CHECK_EQ(pd[16], 0ull);
}

void pair_does_not_fit_in_the_last_slot() {
    Fixture f;
    for (u64 i = 0; i < kMmioMaxMappings - 1; ++i) {
        CHECK(f.mapper.map(0x100000000ull + i * 0x10000000ull, 8) != nullptr);
    }
    // One slot is left; a range that needs two is refused whole.
    CHECK(f.mapper.map(0x300000000ull + k2M - 4, 8) == nullptr);
    CHECK_EQ(window_directory(f)[15], 0ull);
    CHECK(f.mapper.map(0x300000000ull, 8) != nullptr);
}

}

int main() {
    window_base_is_in_an_unused_slot();
    map_returns_offset_inside_window();
    same_2m_page_reused();
    unaligned_phys_keeps_offset();
    range_crossing_2m_boundary_maps_two_pages();
    crossing_range_with_first_page_mapped_elsewhere_gets_a_fresh_pair();
    above_4gib_phys();
    invalid_requests_return_nullptr();
    second_init_is_refused();
    table_exhaustion_returns_nullptr();
    mapping_table_full_returns_nullptr();
    pair_does_not_fit_in_the_last_slot();
    return check::finish("test_mmio");
}
