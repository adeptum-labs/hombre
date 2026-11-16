#include "check.hpp"
#include "paging.hpp"

#include <cstdint>
#include <cstring>

using namespace hombre;

namespace {

constexpr u64 k2M = 0x200000;
constexpr u64 kMiB = 0x100000;
constexpr u64 kHigherHalf = 0xFFFFFFFF80000000ull;
constexpr u64 kDirectMap = 0xFFFF800000000000ull;

constexpr usize kArenaPages = 128;

// Hands out 4 KiB-aligned pages from a static arena. Host pointers stand in
// for physical addresses, so phys_to_ptr is a plain cast.
struct Arena {
    alignas(4096) u8 memory[kArenaPages * 4096];
    usize allocated = 0;
    usize limit = kArenaPages;

    void reset(usize new_limit = kArenaPages) {
        allocated = 0;
        limit = new_limit;
    }

    static u64 alloc(void* ctx) {
        Arena* arena = static_cast<Arena*>(ctx);
        if (arena->allocated == arena->limit) {
            return 0;
        }
        u8* page = arena->memory + arena->allocated * 4096;
        ++arena->allocated;
        std::memset(page, 0, 4096);
        return reinterpret_cast<std::uintptr_t>(page);
    }

    static u64* to_ptr(void*, u64 phys) {
        return reinterpret_cast<u64*>(static_cast<std::uintptr_t>(phys));
    }

    PageAllocator allocator() { return PageAllocator{&Arena::alloc, &Arena::to_ptr, this}; }
};

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

void leaf_entries_carry_the_requested_flags() {
    arena.reset();
    PageTables tables(arena.allocator());
    CHECK(tables.map_2m(0, 0, k2M, Writable));
    CHECK(tables.map_2m(k2M, k2M, k2M, Present | NoExecute));

    const u64* first = paging_detail::find_pd_slot(arena.allocator(), tables.root(), 0);
    const u64* second = paging_detail::find_pd_slot(arena.allocator(), tables.root(), k2M);
    CHECK(first != nullptr && second != nullptr);
    CHECK_EQ(*first, 0x83ull);  // present, writable, 2 MiB page
    CHECK_EQ(*second, (1ull << 63) | k2M | 0x81);  // present, no-execute, 2 MiB page
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
    leaf_entries_carry_the_requested_flags();
    remapping_is_refused_without_touching_the_tables();
    allocation_failure_reports_not_ok();
    rejects_a_misbehaving_allocator();
    shared_upper_tables_are_reused();
    ranges_spanning_a_gigabyte_boundary_use_two_directories();
    return check::finish("test_paging");
}
