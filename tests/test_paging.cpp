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
#include "paging.hpp"

#include <cstdint>
#include <cstring>

using namespace hombre;

namespace {

constexpr u64 k2M = 0x200000;
constexpr u64 kMiB = 0x100000;
constexpr u64 kHigherHalf = 0xFFFFFFFF80000000ull;
constexpr u64 kDirectMap = 0xFFFF800000000000ull;

using fake_pages::Arena;

Arena arena;

bool translates_to(const PageTables& tables, u64 virt, u64 expected) {
    u64 phys = 0;
    return translate(tables.root(), arena.allocator(), virt, phys) && phys == expected;
}

bool is_mapped(const PageTables& tables, u64 virt) {
    u64 phys = 0;
    return translate(tables.root(), arena.allocator(), virt, phys);
}

void maps_higher_half_kernel() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.ok());
    CHECK(tables.map_2m(kHigherHalf, 0, 16 * kMiB, Present | Writable));

    u64 phys = 0;
    CHECK(translate(tables.root(), arena.allocator(), 0xFFFFFFFF80123456ull, phys));
    CHECK_EQ(phys, 0x123456ull);
    CHECK(translates_to(tables, kHigherHalf, 0));
    CHECK(translates_to(tables, kHigherHalf + 16 * kMiB - 1, 16 * kMiB - 1));
    CHECK(!is_mapped(tables, kHigherHalf + 16 * kMiB));
}

void maps_direct_map_and_identity() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.ok());
    CHECK(tables.map_2m(0, 0, 16 * kMiB, Present | Writable));
    CHECK(tables.map_2m(kDirectMap, 0, 16 * kMiB, Present | Writable | NoExecute));

    CHECK(translates_to(tables, 0x1000, 0x1000));
    CHECK(translates_to(tables, 16 * kMiB - 1, 16 * kMiB - 1));
    CHECK(translates_to(tables, kDirectMap + 0xABCDEF, 0xABCDEF));
    CHECK(translates_to(tables, kDirectMap + 16 * kMiB - 1, 16 * kMiB - 1));
    CHECK(!is_mapped(tables, 16 * kMiB));
    CHECK(!is_mapped(tables, kDirectMap + 16 * kMiB));
}

void maps_to_nonzero_physical_base() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(kHigherHalf, 0x40000000, 2 * k2M, Present));
    CHECK(translates_to(tables, kHigherHalf + 0x1FFFFF, 0x401FFFFF));
    CHECK(translates_to(tables, kHigherHalf + k2M + 5, 0x40200005));
}

void unmapped_address_fails_translation() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(kHigherHalf, 0, 4 * kMiB, Present | Writable));

    u64 phys = 0x77;
    CHECK(!translate(tables.root(), arena.allocator(), 0, phys));
    CHECK(!translate(tables.root(), arena.allocator(), kDirectMap, phys));
    CHECK(!translate(tables.root(), arena.allocator(), kHigherHalf + 4 * kMiB, phys));
    CHECK(!translate(tables.root(), arena.allocator(), 0x0000800000000000ull, phys));
    CHECK(!translate(0, arena.allocator(), kHigherHalf, phys));
    CHECK_EQ(phys, 0x77ull);
}

void rejects_unaligned_ranges() {
    arena.reset();
    PageTables tables(arena.allocator());
    const u64 flags = Present | Writable;
    CHECK(!tables.map_2m(0x1000, 0, k2M, flags));
    CHECK(!tables.map_2m(0, 0x1000, k2M, flags));
    CHECK(!tables.map_2m(0, 0, 0x1000, flags));
    CHECK(!tables.map_2m(0, 0, k2M + 0x1000, flags));
    CHECK(!tables.map_2m(0, 0, 0, flags));
    CHECK(tables.ok());
    CHECK(!is_mapped(tables, 0));
    CHECK(tables.map_2m(0, 0, k2M, flags));
}

void rejects_non_canonical_virtual_addresses() {
    arena.reset();
    PageTables tables(arena.allocator());
    const u64 flags = Present | Writable;
    CHECK(!tables.map_2m(0x0000800000000000ull, 0, k2M, flags));
    CHECK(!tables.map_2m(0xFFFF7FFFFFE00000ull, 0, k2M, flags));
    CHECK(!tables.map_2m(0x1234000000000000ull, 0, k2M, flags));
    CHECK(tables.ok());
    // The last page below the hole and the first page above it.
    CHECK(tables.map_2m(0x00007FFFFFE00000ull, 0, k2M, flags));
    CHECK(tables.map_2m(kDirectMap, 0, k2M, flags));
    CHECK(translates_to(tables, 0x00007FFFFFE00010ull, 0x10));
    CHECK(translates_to(tables, kDirectMap + 0x10, 0x10));
}

void rejects_ranges_that_cross_the_hole_or_wrap() {
    arena.reset();
    PageTables tables(arena.allocator());
    const u64 flags = Present | Writable;
    // Runs from the last low page into the hole.
    CHECK(!tables.map_2m(0x00007FFFFFE00000ull, 0, 2 * k2M, flags));
    // Starts below the hole, ends above it.
    CHECK(!tables.map_2m(0x00007FFFFFE00000ull, 0, 0xFFFF800000200000ull, flags));
    // virt + bytes wraps past 2^64.
    CHECK(!tables.map_2m(0xFFFFFFFFFFC00000ull, 0, 4 * k2M, flags));
    CHECK(!tables.map_2m(kHigherHalf, 0, 0xFFFFFFFFFFE00000ull, flags));
    // phys + bytes wraps.
    CHECK(!tables.map_2m(0, 0xFFFFFFFFFFC00000ull, 4 * k2M, flags));
    // Physical range beyond 52 bits.
    CHECK(!tables.map_2m(0, 1ull << 52, k2M, flags));
    CHECK(!tables.map_2m(0, (1ull << 52) - k2M, 2 * k2M, flags));
    CHECK(tables.ok());
    CHECK(!is_mapped(tables, 0));
}

void maps_up_to_the_top_of_the_address_space() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(0xFFFFFFFFFFC00000ull, 0, 2 * k2M, Present));
    CHECK(translates_to(tables, 0xFFFFFFFFFFFFFFFFull, k2M + 0x1FFFFF));
    CHECK(translates_to(tables, 0xFFFFFFFFFFC00000ull, 0));
}

void rejects_unknown_flag_bits() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(!tables.map_2m(0, 0, k2M, Present | (1ull << 7)));
    CHECK(!tables.map_2m(0, 0, k2M, Present | (1ull << 12)));
    CHECK(tables.ok());
}

// The raw-walk tests below read the tables with literal indices and masks
// and do not use anything from paging_detail, so a wrong shift or mask in the
// builder cannot be mirrored by the checker.

// Physical address of the n-th page the arena handed out.
u64 arena_page(usize n) {
    return static_cast<u64>(reinterpret_cast<std::uintptr_t>(arena.memory)) + n * 4096;
}

u64* raw_table(u64 phys) { return Arena::to_ptr(nullptr, phys); }

// Every entry of the table at `phys` except the listed indices must be zero.
bool zero_except(u64 phys, u64 a, u64 b = 512) {
    const u64* table = raw_table(phys);
    for (u64 i = 0; i < 512; ++i) {
        if (i != a && i != b && table[i] != 0) {
            return false;
        }
    }
    return true;
}

void builds_the_expected_higher_half_layout() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(0xFFFFFFFF80000000ull, 0, 2 * k2M, Present | Writable));

    // Allocation order: page 0 is the PML4, page 1 the PDPT, page 2 the PD.
    CHECK_EQ(tables.root(), arena_page(0));
    const u64* pml4 = raw_table(arena_page(0));
    const u64* pdpt = raw_table(arena_page(1));
    const u64* pd = raw_table(arena_page(2));
    CHECK_EQ(pml4[511], arena_page(1) | 3);  // present, writable, no PS, no NX
    CHECK_EQ(pdpt[510], arena_page(2) | 3);
    CHECK_EQ(pd[0], 0x83ull);  // present, writable, 2 MiB page, phys 0
    CHECK_EQ(pd[1], 0x200083ull);
    CHECK(zero_except(arena_page(0), 511));
    CHECK(zero_except(arena_page(1), 510));
    CHECK(zero_except(arena_page(2), 0, 1));
}

void builds_the_expected_direct_map_layout() {
    arena.reset();
    PageTables tables(arena.allocator());
    // Direct-map base plus five 2 MiB pages, no-execute, physical 0x40000000.
    CHECK(tables.map_2m(0xFFFF800000000000ull + 5 * k2M, 0x40000000, k2M,
                        Present | Writable | NoExecute));

    const u64* pml4 = raw_table(arena_page(0));
    const u64* pdpt = raw_table(arena_page(1));
    const u64* pd = raw_table(arena_page(2));
    CHECK_EQ(pml4[256], arena_page(1) | 3);
    CHECK_EQ(pdpt[0], arena_page(2) | 3);
    CHECK_EQ(pd[5], (1ull << 63) | 0x40000000ull | 0x83);
    CHECK(zero_except(arena_page(0), 256));
    CHECK(zero_except(arena_page(1), 0));
    CHECK(zero_except(arena_page(2), 5));
}

void builds_the_expected_low_layout_with_all_indices_nonzero() {
    arena.reset();
    PageTables tables(arena.allocator());
    // PML4 index 1, PDPT index 3, PD index 7.
    const u64 virt = (1ull << 39) + 3 * 0x40000000ull + 7 * k2M;
    CHECK(tables.map_2m(virt, 0x1200000, k2M, Present));

    const u64* pml4 = raw_table(arena_page(0));
    const u64* pdpt = raw_table(arena_page(1));
    const u64* pd = raw_table(arena_page(2));
    CHECK_EQ(pml4[1], arena_page(1) | 3);
    CHECK_EQ(pdpt[3], arena_page(2) | 3);
    CHECK_EQ(pd[7], 0x1200000ull | 0x81);  // present, read-only, 2 MiB page
    CHECK(zero_except(arena_page(0), 1));
    CHECK(zero_except(arena_page(1), 3));
    CHECK(zero_except(arena_page(2), 7));
}

void leaf_entries_carry_the_requested_flags() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(0, 0, k2M, Writable));
    CHECK(tables.map_2m(k2M, k2M, k2M, Present | NoExecute));

    const u64* pd = raw_table(arena_page(2));
    CHECK_EQ(pd[0], 0x83ull);  // present, writable, 2 MiB page
    CHECK_EQ(pd[1], (1ull << 63) | k2M | 0x81);  // present, no-execute, 2 MiB page
}

void remapping_is_refused_without_touching_the_tables() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(4 * k2M, 0, k2M, Present | Writable));

    // Same page again, with the same and with different targets.
    CHECK(!tables.map_2m(4 * k2M, 0, k2M, Present | Writable));
    CHECK(!tables.map_2m(4 * k2M, 8 * k2M, k2M, Present | Writable));
    CHECK(translates_to(tables, 4 * k2M + 0x10, 0x10));

    // A range whose last page is taken must not leave its first pages mapped.
    CHECK(!tables.map_2m(2 * k2M, 16 * k2M, 3 * k2M, Present | Writable));
    CHECK(!is_mapped(tables, 2 * k2M));
    CHECK(!is_mapped(tables, 3 * k2M));
    CHECK(translates_to(tables, 4 * k2M + 0x10, 0x10));
    CHECK(tables.ok());
}

void allocation_failure_reports_not_ok() {
    // No page for the root.
    arena.reset(0);
    PageTables empty(arena.allocator());
    CHECK(!empty.ok());
    CHECK_EQ(empty.root(), 0ull);
    CHECK(!empty.map_2m(0, 0, k2M, Present));

    // Root only: the PDPT allocation fails.
    arena.reset(1);
    PageTables root_only(arena.allocator());
    CHECK(root_only.ok());
    CHECK(!root_only.map_2m(0, 0, k2M, Present));
    CHECK(!root_only.ok());

    // Allocation fails halfway through a call: the first PD exists, the
    // second one (past the 1 GiB boundary) cannot be built.
    arena.reset(3);
    PageTables partial(arena.allocator());
    CHECK(partial.ok());
    CHECK(!partial.map_2m(0x40000000 - 2 * k2M, 0, 4 * k2M, Present | Writable));
    CHECK(!partial.ok());
    CHECK(!partial.map_2m(0x80000000, 0, k2M, Present));
}

void rejects_a_misbehaving_allocator() {
    PageAllocator none{nullptr, &Arena::to_ptr, &arena};
    PageTables no_alloc(none);
    CHECK(!no_alloc.ok());

    PageAllocator unaligned{
        [](void*) -> u64 { return 0x1234; },
        [](void*, u64) -> u64* { return nullptr; },
        nullptr,
    };
    PageTables bad_page(unaligned);
    CHECK(!bad_page.ok());
}

void shared_upper_tables_are_reused() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK_EQ(arena.allocated, 1ull);  // PML4

    CHECK(tables.map_2m(kHigherHalf, 0, k2M, Present | Writable));
    CHECK_EQ(arena.allocated, 3ull);  // + PDPT + PD

    CHECK(tables.map_2m(kHigherHalf + k2M, k2M, k2M, Present | Writable));
    CHECK_EQ(arena.allocated, 3ull);  // adjacent range shares the PD

    // A new 1 GiB region under the same PDPT needs only a new PD.
    CHECK(tables.map_2m(kHigherHalf + 0x40000000, 0, k2M, Present | Writable));
    CHECK_EQ(arena.allocated, 4ull);

    CHECK(translates_to(tables, kHigherHalf + k2M + 0x42, k2M + 0x42));
}

void ranges_spanning_a_gigabyte_boundary_use_two_directories() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(0x40000000 - 2 * k2M, 0x10000000, 4 * k2M, Present | Writable));
    CHECK_EQ(arena.allocated, 4ull);  // PML4 + PDPT + two PDs
    CHECK(translates_to(tables, 0x40000000 - 2 * k2M, 0x10000000));
    CHECK(translates_to(tables, 0x40000000 + 2 * k2M - 1, 0x10000000 + 4 * k2M - 1));
    CHECK(!is_mapped(tables, 0x40000000 + 2 * k2M));
}

void adopt_extends_existing_root() {
    arena.reset();
    PageTables built(arena.allocator());
    CHECK(built.map_2m(kHigherHalf, 0, 4 * kMiB, Present | Writable));
    const usize pages_before = arena.allocated;

    PageTables adopted = PageTables::adopt(arena.allocator(), built.root());
    CHECK(adopted.ok());
    CHECK_EQ(adopted.root(), built.root());
    CHECK_EQ(arena.allocated, pages_before);  // adopting allocates nothing

    // Same PD: no new table. Another half of the address space: new tables.
    CHECK(adopted.map_2m(kHigherHalf + 4 * kMiB, 0x40000000, k2M, Present | Writable));
    CHECK_EQ(arena.allocated, pages_before);
    CHECK(adopted.map_2m(kDirectMap, 0x1000000, k2M, Present | Writable | NoExecute));
    CHECK_EQ(arena.allocated, pages_before + 2);

    // Old and new mappings are visible through either builder's root.
    CHECK(translates_to(adopted, kHigherHalf + 0x1234, 0x1234));
    CHECK(translates_to(adopted, kHigherHalf + 4 * kMiB + 0x10, 0x40000010));
    CHECK(translates_to(adopted, kDirectMap + 0x20, 0x1000020));
    CHECK(translates_to(built, kDirectMap + 0x20, 0x1000020));
    CHECK(translates_to(built, kHigherHalf + 4 * kMiB + 0x10, 0x40000010));

    // The existing mappings count as occupied.
    CHECK(!adopted.map_2m(kHigherHalf, 8 * kMiB, k2M, Present));
    CHECK(translates_to(adopted, kHigherHalf, 0));
    CHECK(adopted.ok());
}

void adopt_rejects_zero_root() {
    arena.reset();
    PageTables built(arena.allocator());
    CHECK(built.ok());

    PageTables zero = PageTables::adopt(arena.allocator(), 0);
    CHECK(!zero.ok());
    CHECK_EQ(zero.root(), 0ull);
    CHECK(!zero.map_2m(0, 0, k2M, Present));
    CHECK(!is_mapped(zero, 0));

    // Unaligned, beyond 52 bits, or not reachable through phys_to_ptr.
    CHECK(!PageTables::adopt(arena.allocator(), built.root() + 8).ok());
    CHECK(!PageTables::adopt(arena.allocator(), 1ull << 52).ok());
    PageAllocator unreachable{&Arena::alloc, [](void*, u64) -> u64* { return nullptr; }, &arena};
    CHECK(!PageTables::adopt(unreachable, built.root()).ok());
    PageAllocator no_translation{&Arena::alloc, nullptr, &arena};
    CHECK(!PageTables::adopt(no_translation, built.root()).ok());

    // A rejected adopt allocates nothing and leaves the tables alone.
    CHECK_EQ(arena.allocated, 1ull);
    CHECK(built.ok());
}

void protect_changes_flags_of_mapped_pages() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(0, 0, 4 * k2M, Present | Writable));
    const usize pages_before = arena.allocated;

    // Pages 1 and 2 become no-execute and read-only; 0 and 3 stay.
    CHECK(tables.protect_2m(k2M, 2 * k2M, NoExecute, Writable));
    const u64* pd = raw_table(arena_page(2));
    CHECK_EQ(pd[0], 0x83ull);
    CHECK_EQ(pd[1], (1ull << 63) | k2M | 0x81);
    CHECK_EQ(pd[2], (1ull << 63) | (2 * k2M) | 0x81);
    CHECK_EQ(pd[3], (3 * k2M) | 0x83);
    CHECK(translates_to(tables, k2M + 0x10, k2M + 0x10));
    CHECK_EQ(arena.allocated, pages_before);  // no table is allocated

    // And back again.
    CHECK(tables.protect_2m(k2M, k2M, Writable, NoExecute));
    CHECK_EQ(pd[1], k2M | 0x83);
    CHECK(tables.ok());
}

void protect_is_refused_without_touching_the_tables() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(0, 0, 2 * k2M, Present | Writable));
    const u64* pd = raw_table(arena_page(2));

    // The last page of the range is not mapped: nothing changes.
    CHECK(!tables.protect_2m(0, 3 * k2M, NoExecute, 0));
    CHECK_EQ(pd[0], 0x83ull);
    CHECK_EQ(pd[1], k2M | 0x83);
    // No tables at all for the address.
    CHECK(!tables.protect_2m(kHigherHalf, k2M, NoExecute, 0));
    // Unaligned or empty ranges, Present, unknown bits.
    CHECK(!tables.protect_2m(0x1000, k2M, NoExecute, 0));
    CHECK(!tables.protect_2m(0, k2M + 0x1000, NoExecute, 0));
    CHECK(!tables.protect_2m(0, 0, NoExecute, 0));
    CHECK(!tables.protect_2m(0, k2M, 0, Present));
    CHECK(!tables.protect_2m(0, k2M, Present, 0));
    CHECK(!tables.protect_2m(0, k2M, 1ull << 5, 0));
    CHECK(!tables.protect_2m(0, k2M, 0, 1ull << 7));
    CHECK(!tables.protect_2m(~(k2M - 1), 2 * k2M, NoExecute, 0));  // wraps
    CHECK_EQ(pd[0], 0x83ull);
    CHECK_EQ(pd[1], k2M | 0x83);
    // A builder without tables refuses too.
    PageTables none;
    CHECK(!none.protect_2m(0, k2M, NoExecute, 0));
}

void cache_flags_are_encoded() {
    arena.reset();
    PageTables tables(arena.allocator());
    const u64 uncached = Present | Writable | WriteThrough | CacheDisable | NoExecute;
    CHECK(tables.map_2m(0, 0, k2M, Present | Writable | WriteThrough));
    CHECK(tables.map_2m(k2M, k2M, k2M, Present | CacheDisable));
    CHECK(tables.map_2m(2 * k2M, 2 * k2M, k2M, uncached));
    CHECK(tables.ok());

    const u64* pd = raw_table(arena_page(2));
    CHECK_EQ(pd[0], 0x8Bull);  // present, writable, bit 3, 2 MiB page
    CHECK_EQ(pd[1], k2M | 0x91);  // present, bit 4, 2 MiB page
    CHECK_EQ(pd[2], (1ull << 63) | (2 * k2M) | 0x9B);
    CHECK_EQ(WriteThrough, 1ull << 3);
    CHECK_EQ(CacheDisable, 1ull << 4);
    // Other bits stay refused.
    CHECK(!tables.map_2m(3 * k2M, 0, k2M, Present | (1ull << 5)));
}

}

int main() {
    maps_higher_half_kernel();
    maps_direct_map_and_identity();
    maps_to_nonzero_physical_base();
    unmapped_address_fails_translation();
    rejects_unaligned_ranges();
    rejects_non_canonical_virtual_addresses();
    rejects_ranges_that_cross_the_hole_or_wrap();
    maps_up_to_the_top_of_the_address_space();
    rejects_unknown_flag_bits();
    builds_the_expected_higher_half_layout();
    builds_the_expected_direct_map_layout();
    builds_the_expected_low_layout_with_all_indices_nonzero();
    leaf_entries_carry_the_requested_flags();
    remapping_is_refused_without_touching_the_tables();
    allocation_failure_reports_not_ok();
    rejects_a_misbehaving_allocator();
    shared_upper_tables_are_reused();
    ranges_spanning_a_gigabyte_boundary_use_two_directories();
    adopt_extends_existing_root();
    adopt_rejects_zero_root();
    cache_flags_are_encoded();
    protect_changes_flags_of_mapped_pages();
    protect_is_refused_without_touching_the_tables();
    return check::finish("test_paging");
}
