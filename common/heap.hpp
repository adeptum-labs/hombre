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

#pragma once

#include "types.hpp"

namespace hombre {

// A first-fit heap over arenas that a callback supplies on demand. Free blocks
// form one list in address order; each block has a 16-byte header in front of
// its payload, and freeing coalesces a block with both neighbours. Single
// threaded, and it never touches the C++ runtime heap, so a constinit Heap
// works in the kernel as well as under the host tests.
class Heap {
public:
    // Returns at least min_bytes of memory and sets got_bytes to the size
    // actually given, or returns nullptr when it has none. The memory stays
    // with the heap for good.
    using GrowFn = void* (*)(void* ctx, usize min_bytes, usize& got_bytes);

    static constexpr usize default_align = 16;
    static constexpr usize max_align = 4096;

    void init(GrowFn grow, void* ctx) {
        grow_ = grow;
        ctx_ = ctx;
    }

    // Returns a block of at least `bytes` whose address is a multiple of
    // align, a power of two up to max_align, or nullptr when align is not
    // one, the request cannot be met, or the callback gives nothing.
    void* alloc(usize bytes, usize align = default_align) {
        if (align < default_align) {
            align = default_align;
        }
        if (bytes > max_request || (align & (align - 1)) != 0 || align > max_align) {
            return nullptr;
        }
        const usize need = block_size_for(bytes);
        Placement fit = find_fit(need, align);
        if (fit.block == nullptr) {
            // The padding in front of an aligned payload is at most
            // align + header_size; the arena base may lose a header's worth
            // of bytes to alignment.
            if (!grow_arena(need + align + 2 * header_size)) {
                return nullptr;
            }
            fit = find_fit(need, align);
        }
        return fit.block == nullptr ? nullptr : take(fit, need);
    }

    // Accepts nullptr and any pointer alloc returned and has not had freed.
    void free(void* ptr) {
        if (ptr == nullptr) {
            return;
        }
        Block* block = reinterpret_cast<Block*>(static_cast<u8*>(ptr) - header_size);
        in_use_ -= block->size;
        insert_free(block);
    }

    // Bytes held by allocated blocks, headers included.
    usize bytes_in_use() const { return in_use_; }

    // Bytes taken from the callback so far, less the trimming for alignment.
    usize arena_bytes() const { return arena_bytes_; }

private:
    // The first two words are the header that precedes every payload. next
    // lies in the payload, so only a free block has it.
    struct Block {
        usize size;  // whole block, header included, a multiple of 16
        usize first_in_arena;
        Block* next;
    };

    // Where a request goes: the free block that holds it, the free block
    // before that in the list, and the bytes in front of the allocation that
    // stay behind as a free block of their own.
    struct Placement {
        Block* block;
        Block* prev;
        usize gap;
    };

    static constexpr usize header_size = 16;
    static constexpr usize min_block_size = 2 * header_size;
    static constexpr usize page_size = 4096;
    static constexpr usize min_arena_bytes = 2 * 1024 * 1024;
    // Far above any real arena, and keeps the size arithmetic from wrapping.
    static constexpr usize max_request = ~usize{0} / 4;

    static usize round_up(usize value, usize multiple) {
        return (value + multiple - 1) & ~(multiple - 1);
    }

    static usize address_of(const void* ptr) { return reinterpret_cast<usize>(ptr); }

    static const u8* end_of(const Block* block) {
        return reinterpret_cast<const u8*>(block) + block->size;
    }

    // Blocks of different arenas may sit side by side in memory, and must
    // still never merge.
    static bool touches(const Block* low, const Block* high) {
        return end_of(low) == reinterpret_cast<const u8*>(high) && high->first_in_arena == 0;
    }

    static usize block_size_for(usize bytes) {
        const usize size = round_up(bytes, default_align) + header_size;
        return size < min_block_size ? min_block_size : size;
    }

    Block*& link_after(Block* prev) { return prev == nullptr ? head_ : prev->next; }

    // The gap in front of the allocation is either empty or big enough to be
    // a free block, so a padded placement may skip one more alignment step.
    static usize gap_in_front(const Block* block, usize align) {
        const usize payload = round_up(address_of(block) + header_size, align);
        const usize gap = payload - header_size - address_of(block);
        return gap != 0 && gap < min_block_size ? gap + align : gap;
    }

    Placement find_fit(usize need, usize align) {
        Block* prev = nullptr;
        for (Block* block = head_; block != nullptr; prev = block, block = block->next) {
            const usize gap = gap_in_front(block, align);
            if (block->size >= gap + need) {
                return {block, prev, gap};
            }
        }
        return {nullptr, nullptr, 0};
    }

    void* take(const Placement& fit, usize need) {
        Block* block = fit.block;
        Block* prev = fit.prev;
        if (fit.gap != 0) {
            Block* padded = reinterpret_cast<Block*>(reinterpret_cast<u8*>(block) + fit.gap);
            *padded = {block->size - fit.gap, 0, block->next};
            *block = {fit.gap, block->first_in_arena, padded};
            prev = block;
            block = padded;
        }
        if (block->size - need >= min_block_size) {
            Block* rest = reinterpret_cast<Block*>(reinterpret_cast<u8*>(block) + need);
            *rest = {block->size - need, 0, block->next};
            block->size = need;
            link_after(prev) = rest;
        } else {
            link_after(prev) = block->next;
        }
        in_use_ += block->size;
        return reinterpret_cast<u8*>(block) + header_size;
    }

    // Puts the block into the address-ordered list and merges it with the
    // free blocks next to it.
    void insert_free(Block* block) {
        Block* prev = nullptr;
        Block* next = head_;
        while (next != nullptr && address_of(next) < address_of(block)) {
            prev = next;
            next = next->next;
        }
        block->next = next;
        link_after(prev) = block;
        if (next != nullptr && touches(block, next)) {
            block->size += next->size;
            block->next = next->next;
        }
        if (prev != nullptr && touches(prev, block)) {
            prev->size += block->size;
            prev->next = block->next;
        }
    }

    bool grow_arena(usize min_bytes) {
        if (grow_ == nullptr) {
            return false;
        }
        const usize want = round_up(min_bytes, page_size);
        usize got = 0;
        void* memory = grow_(ctx_, want < min_arena_bytes ? min_arena_bytes : want, got);
        if (memory == nullptr) {
            return false;
        }
        const usize start = round_up(address_of(memory), default_align);
        const usize skipped = start - address_of(memory);
        if (got < skipped + min_block_size) {
            return false;
        }
        got = (got - skipped) & ~(default_align - 1);
        *reinterpret_cast<Block*>(start) = {got, 1, nullptr};
        arena_bytes_ += got;
        insert_free(reinterpret_cast<Block*>(start));
        return true;
    }

    GrowFn grow_ = nullptr;
    void* ctx_ = nullptr;
    Block* head_ = nullptr;
    usize in_use_ = 0;
    usize arena_bytes_ = 0;
};

}
