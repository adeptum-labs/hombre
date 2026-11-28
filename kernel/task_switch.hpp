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

#include "../common/paging.hpp"
#include "../common/types.hpp"

namespace hombre {

class PhysAllocator;

// Host stack window at PML4 index 386, adjacent to the MMIO window (384)
// and the code execution window (385). Uses 4 KiB pages for this window only.
inline constexpr u64 kTaskStackBase = 0xFFFFC10000000000ull;

// Keep 64 KiB per stack: ample for JIT/native call chains, while the unmapped
// 4 KiB guard page immediately below catches any stack overflow with a page
// fault instead of silently corrupting adjacent memory.
inline constexpr u64 kTaskStackBytes = 64 * 1024;
inline constexpr u64 kTaskGuardBytes = 4096;
inline constexpr u64 kTaskStackPages = kTaskStackBytes / 4096;
inline constexpr u64 kTaskStackStride = 128 * 1024;
// The most a stack of one slot can hold, above its guard page.
inline constexpr u64 kTaskStackMaxBytes = kTaskStackStride - kTaskGuardBytes;
inline constexpr u32 kMaxTaskStacks = 32;
inline constexpr u64 kTaskStackEnd = kTaskStackBase + kMaxTaskStacks * kTaskStackStride;

extern "C" void task_switch(u64* save_rsp, u64 next_rsp);
extern "C" void task_entry_trampoline();

// Manages per-task 64 KiB host stacks with an unmapped 4 KiB guard page below
// each stack inside kTaskStackBase..kTaskStackEnd.
class TaskStacks {
public:
    using FreePageFn = void (*)(void* ctx, u64 phys);
    using FlushPageFn = void (*)(u64 virt);

    constexpr TaskStacks() = default;

    // Host-testable setup over an explicit PageAllocator and CR3 root.
    bool init(const PageAllocator& tables, u64 root, FreePageFn free_page, FlushPageFn flush_page,
              void* free_ctx) {
        if (root == 0 || (root & (paging_detail::page_size - 1)) != 0 ||
            root >= paging_detail::phys_limit || tables.alloc_zeroed_page == nullptr ||
            tables.phys_to_ptr == nullptr ||
            paging_detail::table_at(tables, root) == nullptr) {
            return false;
        }
        tables_ = tables;
        root_ = root;
        free_page_ = free_page;
        flush_page_ = flush_page;
        free_ctx_ = free_ctx;
        for (u32 i = 0; i < kMaxTaskStacks; ++i) {
            rsp_[i] = 0;
            vslot_of_[i] = kNoSlot;
            vslot_used_[i] = false;
        }
        initialised_ = true;
        return true;
    }

    // Kernel setup using the physical page allocator and live CR3.
    bool init_kernel(PhysAllocator& pages);

    // Allocates a virtual stack slot and 64 KiB (or bytes) of physical pages for task id,
    // mapping them above an unmapped guard page and preparing the initial
    // task_switch frame to enter entry(arg). False if out of slots or pages.
    bool create(u32 id, void (*entry)(void*), void* arg, u64 bytes = kTaskStackBytes) {
        return create_with_trampoline(id, entry, arg,
                                      reinterpret_cast<u64>(&task_entry_trampoline), bytes);
    }

    // bytes, a whole number of pages up to kTaskStackMaxBytes, sizes the
    // stack; its guard page stays below it.
    bool create_with_trampoline(u32 id, void (*entry)(void*), void* arg, u64 trampoline_addr,
                                u64 bytes = kTaskStackBytes) {
        if (!initialised_ || id >= kMaxTaskStacks || vslot_of_[id] != kNoSlot ||
            entry == nullptr || bytes == 0 || bytes > kTaskStackMaxBytes ||
            (bytes & (paging_detail::page_size - 1)) != 0) {
            return false;
        }
        const u64 stack_pages = bytes / paging_detail::page_size;
        u32 vslot = kNoSlot;
        for (u32 s = 0; s < kMaxTaskStacks; ++s) {
            if (!vslot_used_[s]) {
                vslot = s;
                break;
            }
        }
        if (vslot == kNoSlot) {
            return false;
        }

        const u64 bottom = slot_bottom(vslot);
        u64 top_page_phys = 0;
        u64 mapped_pages = 0;
        for (; mapped_pages < stack_pages; ++mapped_pages) {
            const u64 phys = tables_.alloc_zeroed_page(tables_.ctx);
            if (phys == 0 || (phys & (paging_detail::page_size - 1)) != 0 ||
                phys >= paging_detail::phys_limit) {
                rollback(bottom, mapped_pages);
                return false;
            }
            const u64 virt = bottom + mapped_pages * paging_detail::page_size;
            if (!map_4k(virt, phys, Present | Writable | NoExecute)) {
                if (free_page_ != nullptr) {
                    free_page_(free_ctx_, phys);
                }
                rollback(bottom, mapped_pages);
                return false;
            }
            if (mapped_pages + 1 == stack_pages) {
                top_page_phys = phys;
            }
        }

        u64* top_page = paging_detail::table_at(tables_, top_page_phys);
        if (top_page == nullptr) {
            rollback(bottom, stack_pages);
            return false;
        }

        // Write the initial switch frame at the top of the highest stack page
        // through phys_to_ptr so both kernel and host tests can inspect it.
        constexpr u64 kFrameWords = 8;
        constexpr u64 kWordsPerPage = paging_detail::page_size / sizeof(u64);
        u64* frame = top_page + (kWordsPerPage - kFrameWords);
        frame[0] = 0;                                  // r15
        frame[1] = 0;                                  // r14
        frame[2] = reinterpret_cast<u64>(entry);       // r13
        frame[3] = reinterpret_cast<u64>(arg);         // r12
        frame[4] = 0;                                  // rbp
        frame[5] = 0;                                  // rbx
        frame[6] = 0x2;                                // rflags (IF=0; trampoline sets IF)
        frame[7] = trampoline_addr;                    // ret target

        vslot_used_[vslot] = true;
        vslot_of_[id] = vslot;
        bytes_[id] = bytes;
        rsp_[id] = bottom + bytes - kFrameWords * sizeof(u64);
        return true;
    }

    // Saves the current host context of `from` and switches to `to`.
    void switch_to(u32 from, u32 to) {
        if (from < kMaxTaskStacks && to < kMaxTaskStacks) {
            task_switch(&rsp_[from], rsp_[to]);
        }
    }

    // Unmaps and frees the stack of a non-running task and returns its virtual
    // slot to the pool.
    void destroy(u32 id) {
        if (!initialised_ || id >= kMaxTaskStacks || vslot_of_[id] == kNoSlot) {
            return;
        }
        const u32 vslot = vslot_of_[id];
        rollback(slot_bottom(vslot), bytes_[id] / paging_detail::page_size);
        vslot_used_[vslot] = false;
        vslot_of_[id] = kNoSlot;
        rsp_[id] = 0;
    }

    // Translates a 4 KiB page inside the stack window; false if unmapped.
    bool translate_4k(u64 virt, u64& phys_out) const {
        if (!initialised_ || virt < kTaskStackBase || virt >= kTaskStackEnd) {
            return false;
        }
        const u64* pte = find_pte(virt);
        if (pte == nullptr || (*pte & Present) == 0) {
            return false;
        }
        phys_out = (*pte & paging_detail::table_addr_mask) |
                   (virt & (paging_detail::page_size - 1));
        return true;
    }

    u64 stack_bottom(u32 id) const {
        if (id >= kMaxTaskStacks || vslot_of_[id] == kNoSlot) {
            return 0;
        }
        return slot_bottom(vslot_of_[id]);
    }

    u64 stack_top(u32 id) const {
        const u64 b = stack_bottom(id);
        return b != 0 ? b + bytes_[id] : 0;
    }

    u64 guard_page(u32 id) const {
        const u64 b = stack_bottom(id);
        return b != 0 ? b - kTaskGuardBytes : 0;
    }

    u64 saved_rsp(u32 id) const { return id < kMaxTaskStacks ? rsp_[id] : 0; }

private:
    static constexpr u32 kNoSlot = 0xFFFFFFFFu;

    static u64 slot_bottom(u32 vslot) {
        return kTaskStackBase + static_cast<u64>(vslot) * kTaskStackStride + kTaskGuardBytes;
    }

    bool in_stack_area(u64 virt) const {
        if (virt < kTaskStackBase || virt >= kTaskStackEnd ||
            (virt & (paging_detail::page_size - 1)) != 0) {
            return false;
        }
        const u64 off = (virt - kTaskStackBase) % kTaskStackStride;
        return off >= kTaskGuardBytes && off < kTaskGuardBytes + kTaskStackMaxBytes;
    }

    bool map_4k(u64 virt, u64 phys, u64 flags) {
        using namespace paging_detail;

        if (!in_stack_area(virt) || (flags & ~allowed_flags) != 0) {
            return false;
        }
        u64* table = table_at(tables_, root_);
        for (unsigned level = level_pml4; level >= level_pd; --level) {
            if (table == nullptr) {
                return false;
            }
            u64* entry = &table[index_at(virt, level)];
            if ((*entry & Present) == 0) {
                const u64 child = tables_.alloc_zeroed_page(tables_.ctx);
                if (child == 0 || (child & (page_size - 1)) != 0 || child >= phys_limit ||
                    table_at(tables_, child) == nullptr) {
                    return false;
                }
                *entry = child | Present | Writable;
            } else if ((*entry & entry_large) != 0) {
                return false;
            }
            table = table_at(tables_, *entry & table_addr_mask);
        }
        if (table == nullptr) {
            return false;
        }
        u64* pte = &table[index_at(virt, 0)];
        if ((*pte & Present) != 0) {
            return false;
        }
        *pte = (phys & table_addr_mask) | (flags & allowed_flags) | Present;
        return true;
    }

    const u64* find_pte(u64 virt) const {
        using namespace paging_detail;

        const u64* table = table_at(tables_, root_);
        for (unsigned level = level_pml4; level >= level_pd; --level) {
            if (table == nullptr) {
                return nullptr;
            }
            const u64 entry = table[index_at(virt, level)];
            if ((entry & Present) == 0 || (entry & entry_large) != 0) {
                return nullptr;
            }
            table = table_at(tables_, entry & table_addr_mask);
        }
        if (table == nullptr) {
            return nullptr;
        }
        return &table[index_at(virt, 0)];
    }

    u64* find_pte_mut(u64 virt) {
        return const_cast<u64*>(find_pte(virt));
    }

    void rollback(u64 bottom, u64 pages) {
        for (u64 i = 0; i < pages; ++i) {
            const u64 virt = bottom + i * paging_detail::page_size;
            u64* pte = find_pte_mut(virt);
            if (pte != nullptr && (*pte & Present) != 0) {
                const u64 phys = *pte & paging_detail::table_addr_mask;
                *pte = 0;
                if (flush_page_ != nullptr) {
                    flush_page_(virt);
                }
                if (free_page_ != nullptr) {
                    free_page_(free_ctx_, phys);
                }
            }
        }
    }

    PageAllocator tables_{nullptr, nullptr, nullptr};
    u64 root_ = 0;
    FreePageFn free_page_ = nullptr;
    FlushPageFn flush_page_ = nullptr;
    void* free_ctx_ = nullptr;
    u64 rsp_[kMaxTaskStacks]{};
    u32 vslot_of_[kMaxTaskStacks]{};
    u64 bytes_[kMaxTaskStacks]{};
    bool vslot_used_[kMaxTaskStacks]{};
    bool initialised_ = false;
};

}
