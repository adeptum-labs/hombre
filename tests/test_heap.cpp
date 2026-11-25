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

#include "heap.hpp"
#include "check.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

using namespace hombre;

namespace {

constexpr usize kMiB = 1024 * 1024;
constexpr usize kPage = 4096;

// Hands out page-aligned host buffers while the budget lasts, and checks that
// the heap asks the way its contract says.
struct Arenas {
    std::vector<void*> buffers;
    usize calls = 0;
    usize budget = ~usize{0};

    Arenas() = default;
    Arenas(const Arenas&) = delete;
    Arenas& operator=(const Arenas&) = delete;
    ~Arenas() {
        for (void* buffer : buffers) {
            std::free(buffer);
        }
    }

    static void* grow(void* ctx, usize min_bytes, usize& got_bytes) {
        Arenas& self = *static_cast<Arenas*>(ctx);
        ++self.calls;
        CHECK(min_bytes >= 2 * kMiB);
        CHECK_EQ(min_bytes % kPage, 0u);
        if (self.budget == 0) {
            return nullptr;
        }
        --self.budget;
        void* buffer = std::aligned_alloc(kPage, min_bytes);
        CHECK(buffer != nullptr);
        self.buffers.push_back(buffer);
        got_bytes = min_bytes;
        return buffer;
    }
};

struct Rig {
    Arenas arenas;
    Heap heap;

    Rig() { heap.init(&Arenas::grow, &arenas); }
};

// Two adjacent 2 MiB arenas cut from one buffer, then nothing.
struct AdjacentHalves {
    u8* base = static_cast<u8*>(std::aligned_alloc(kPage, 4 * kMiB));
    usize taken = 0;

    ~AdjacentHalves() { std::free(base); }

    static void* grow(void* ctx, usize min_bytes, usize& got_bytes) {
        AdjacentHalves& self = *static_cast<AdjacentHalves*>(ctx);
        if (min_bytes > 2 * kMiB || self.taken == 2) {
            return nullptr;
        }
        got_bytes = 2 * kMiB;
        return self.base + 2 * kMiB * self.taken++;
    }
};

bool is_aligned(const void* ptr, usize align) {
    return reinterpret_cast<usize>(ptr) % align == 0;
}

void fill(void* ptr, usize bytes, u8 seed) {
    u8* data = static_cast<u8*>(ptr);
    for (usize i = 0; i < bytes; ++i) {
        data[i] = static_cast<u8>(seed + i);
    }
}

bool holds(const void* ptr, usize bytes, u8 seed) {
    const u8* data = static_cast<const u8*>(ptr);
    for (usize i = 0; i < bytes; ++i) {
        if (data[i] != static_cast<u8>(seed + i)) {
            return false;
        }
    }
    return true;
}

constinit Heap g_constinit_heap;

void constinit_heap_allocates_once_initialised() {
    Arenas arenas;
    g_constinit_heap.init(&Arenas::grow, &arenas);
    void* block = g_constinit_heap.alloc(64);
    CHECK(block != nullptr);
    g_constinit_heap.free(block);
    CHECK_EQ(g_constinit_heap.bytes_in_use(), 0u);
}

void heap_figures_follow_the_blocks() {
    Rig rig;
    CHECK_EQ(rig.heap.bytes_in_use(), 0u);
    CHECK_EQ(rig.heap.arena_bytes(), 0u);

    void* block = rig.heap.alloc(1000);
    CHECK(block != nullptr);
    CHECK(rig.heap.bytes_in_use() >= 1000);
    const usize arena = rig.heap.arena_bytes();
    CHECK(rig.heap.bytes_in_use() <= arena);

    rig.heap.free(block);
    CHECK_EQ(rig.heap.bytes_in_use(), 0u);
    CHECK_EQ(rig.heap.arena_bytes(), arena);
}

void heap_without_a_callback_gives_nothing() {
    Heap heap;
    CHECK(heap.alloc(8) == nullptr);
    heap.free(nullptr);
    CHECK_EQ(heap.bytes_in_use(), 0u);
    CHECK_EQ(heap.arena_bytes(), 0u);
}

void every_result_has_the_requested_alignment() {
    Rig rig;
    std::vector<void*> blocks;
    for (usize align = 16; align <= 4096; align *= 2) {
        for (usize bytes : {0u, 1u, 15u, 16u, 17u, 100u, 5000u}) {
            void* block = rig.heap.alloc(bytes, align);
            CHECK(block != nullptr);
            CHECK(is_aligned(block, align));
            fill(block, bytes, static_cast<u8>(align));
            blocks.push_back(block);
        }
    }
    CHECK(is_aligned(rig.heap.alloc(1), 16));
    for (void* block : blocks) {
        rig.heap.free(block);
    }
}

void smaller_alignments_are_raised_to_16_and_bad_ones_refused() {
    Rig rig;
    CHECK(is_aligned(rig.heap.alloc(10, 1), 16));
    CHECK(is_aligned(rig.heap.alloc(10, 8), 16));
    CHECK(rig.heap.alloc(10, 24) == nullptr);
    CHECK(rig.heap.alloc(10, 8192) == nullptr);
    CHECK(rig.heap.alloc(~usize{0}) == nullptr);
}

void a_freed_block_is_reused() {
    Rig rig;
    void* first = rig.heap.alloc(100);
    void* guard = rig.heap.alloc(100);
    CHECK(first != nullptr && guard != nullptr);
    rig.heap.free(first);
    CHECK(rig.heap.alloc(100) == first);
    CHECK_EQ(rig.arenas.calls, 1u);
}

void free_neighbours_coalesce_into_one_block() {
    Rig rig;
    void* a = rig.heap.alloc(1000);
    void* b = rig.heap.alloc(1000);
    void* c = rig.heap.alloc(1000);
    void* guard = rig.heap.alloc(16);
    CHECK(guard != nullptr);
    rig.heap.free(a);
    rig.heap.free(c);
    rig.heap.free(b);
    CHECK(rig.heap.alloc(3000) == a);
    CHECK_EQ(rig.arenas.calls, 1u);
}

void the_heap_grows_into_a_second_arena() {
    Rig rig;
    void* first = rig.heap.alloc(3 * kMiB / 2);
    CHECK_EQ(rig.arenas.calls, 1u);
    void* second = rig.heap.alloc(3 * kMiB / 2);
    CHECK(first != nullptr && second != nullptr);
    CHECK_EQ(rig.arenas.calls, 2u);
    CHECK(rig.heap.arena_bytes() >= 4 * kMiB);
    fill(first, 3 * kMiB / 2, 1);
    fill(second, 3 * kMiB / 2, 2);
    CHECK(holds(first, 3 * kMiB / 2, 1));
    rig.heap.free(first);
    rig.heap.free(second);
    CHECK_EQ(rig.heap.bytes_in_use(), 0u);
}

void large_and_over_aligned_requests_get_an_arena_that_fits() {
    Rig rig;
    void* large = rig.heap.alloc(5 * kMiB);
    CHECK(large != nullptr);
    CHECK(rig.heap.arena_bytes() >= 5 * kMiB);
    fill(large, 5 * kMiB, 7);
    CHECK(holds(large, 5 * kMiB, 7));
    void* aligned_large = rig.heap.alloc(2 * kMiB, 4096);
    CHECK(aligned_large != nullptr && is_aligned(aligned_large, 4096));
    fill(aligned_large, 2 * kMiB, 9);
    CHECK(holds(large, 5 * kMiB, 7));
    rig.heap.free(large);
    rig.heap.free(aligned_large);
    CHECK_EQ(rig.heap.bytes_in_use(), 0u);
}

void nullptr_when_the_callback_refuses() {
    Rig rig;
    rig.arenas.budget = 0;
    CHECK(rig.heap.alloc(1) == nullptr);
    CHECK_EQ(rig.heap.arena_bytes(), 0u);

    rig.arenas.budget = 1;
    void* most = rig.heap.alloc(2 * kMiB - 1000);
    CHECK(most != nullptr);
    CHECK(rig.heap.alloc(2000) == nullptr);
    CHECK(rig.heap.alloc(100) != nullptr);
    rig.heap.free(most);
}

void blocks_of_adjacent_arenas_never_merge() {
    AdjacentHalves source;
    Heap heap;
    heap.init(&AdjacentHalves::grow, &source);
    void* first = heap.alloc(3 * kMiB / 2);
    void* second = heap.alloc(3 * kMiB / 2);
    CHECK(first != nullptr && second != nullptr);
    CHECK_EQ(source.taken, 2u);
    heap.free(first);
    heap.free(second);
    CHECK(heap.alloc(3 * kMiB) == nullptr);
    CHECK(heap.alloc(3 * kMiB / 2) != nullptr);
}

struct Slot {
    void* ptr = nullptr;
    usize bytes = 0;
    u8 seed = 0;
};

// Random sizes and alignments, every live block checked for damage before it
// is freed.
void random_alloc_and_free_keeps_every_block_intact() {
    Rig rig;
    constexpr usize slot_count = 256;
    constexpr usize alignments[] = {16, 16, 16, 32, 64, 256, 4096};
    std::vector<Slot> slots(slot_count);
    u64 state = 0x9E3779B97F4A7C15;
    auto next = [&state](u64 limit) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state % limit;
    };
    for (int step = 0; step < 20000; ++step) {
        Slot& slot = slots[next(slot_count)];
        if (slot.ptr != nullptr) {
            CHECK(holds(slot.ptr, slot.bytes, slot.seed));
            rig.heap.free(slot.ptr);
            slot = Slot{};
            continue;
        }
        const usize align = alignments[next(sizeof(alignments) / sizeof(alignments[0]))];
        slot.bytes = next(50) == 0 ? next(100000) : next(2000);
        slot.seed = static_cast<u8>(next(256));
        slot.ptr = rig.heap.alloc(slot.bytes, align);
        CHECK(slot.ptr != nullptr);
        CHECK(is_aligned(slot.ptr, align));
        fill(slot.ptr, slot.bytes, slot.seed);
    }
    for (Slot& slot : slots) {
        if (slot.ptr != nullptr) {
            CHECK(holds(slot.ptr, slot.bytes, slot.seed));
            rig.heap.free(slot.ptr);
        }
    }
    CHECK_EQ(rig.heap.bytes_in_use(), 0u);
    const usize calls = rig.arenas.calls;
    CHECK(rig.heap.alloc(kMiB) != nullptr);
    CHECK_EQ(rig.arenas.calls, calls);
}

}

int main() {
    constinit_heap_allocates_once_initialised();
    heap_figures_follow_the_blocks();
    heap_without_a_callback_gives_nothing();
    every_result_has_the_requested_alignment();
    smaller_alignments_are_raised_to_16_and_bad_ones_refused();
    a_freed_block_is_reused();
    free_neighbours_coalesce_into_one_block();
    the_heap_grows_into_a_second_arena();
    large_and_over_aligned_requests_get_an_arena_that_fits();
    nullptr_when_the_callback_refuses();
    blocks_of_adjacent_arenas_never_merge();
    random_alloc_and_free_keeps_every_block_intact();
    return check::finish("test_heap");
}
