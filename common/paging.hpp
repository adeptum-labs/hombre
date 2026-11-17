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

// Where page-table pages come from. alloc_zeroed_page returns the physical
// address of a zeroed, 4 KiB-aligned page, or 0 when none is left.
// phys_to_ptr turns a physical address into a pointer the caller can
// dereference while the tables are being built.
struct PageAllocator {
    u64 (*alloc_zeroed_page)(void* ctx);
    u64* (*phys_to_ptr)(void* ctx, u64 phys);
    void* ctx;
};

enum PageFlags : u64 {
    Present = 1,
    Writable = 2,
    WriteThrough = 1ull << 3,
    CacheDisable = 1ull << 4,
    NoExecute = 1ull << 63,
};

namespace paging_detail {

inline constexpr u64 page_size = 0x1000;
inline constexpr u64 large_page_size = 0x200000;
inline constexpr u64 large_page_mask = large_page_size - 1;
inline constexpr u64 entry_large = 1ull << 7;
inline constexpr u64 table_addr_mask = 0x000FFFFFFFFFF000ull;
inline constexpr u64 large_addr_mask = 0x000FFFFFFFE00000ull;
inline constexpr u64 phys_limit = 1ull << 52;
inline constexpr u64 allowed_flags =
    Present | Writable | WriteThrough | CacheDisable | NoExecute;

// Levels count down from the root: 3 is the PML4, 2 the PDPT, 1 the PD.
inline constexpr unsigned level_pml4 = 3;
inline constexpr unsigned level_pdpt = 2;
inline constexpr unsigned level_pd = 1;

// Bits 63:47 must all equal bit 47.
inline bool is_canonical(u64 address) {
    const u64 top = address >> 47;
    return top == 0 || top == 0x1FFFF;
}

inline u64 index_at(u64 virt, unsigned level) {
    return (virt >> (12 + 9 * level)) & 0x1FF;
}

inline u64* table_at(const PageAllocator& allocator, u64 phys) {
    if (allocator.phys_to_ptr == nullptr) {
        return nullptr;
    }
    return allocator.phys_to_ptr(allocator.ctx, phys);
}

// Finds the page-directory slot that covers virt without allocating.
// Returns nullptr when an upper-level table is missing.
inline u64* find_pd_slot(const PageAllocator& allocator, u64 root, u64 virt) {
    if (root == 0 || !is_canonical(virt)) {
        return nullptr;
    }
    u64* table = table_at(allocator, root);
    for (unsigned level = level_pml4; level >= level_pdpt; --level) {
        if (table == nullptr) {
            return nullptr;
        }
        const u64 entry = table[index_at(virt, level)];
        if ((entry & Present) == 0 || (entry & entry_large) != 0) {
            return nullptr;
        }
        table = table_at(allocator, entry & table_addr_mask);
    }
    if (table == nullptr) {
        return nullptr;
    }
    return &table[index_at(virt, level_pd)];
}

}

// Builds 4-level x86-64 page tables with 2 MiB leaf pages. Once an
// allocation fails the tables may be partly built, so ok() turns false for
// good and every later map_2m call is refused.
class PageTables {
public:
    // A builder with no tables: ok() is false and every map_2m call is
    // refused. It exists so a PageTables can be a member that is filled in
    // later.
    PageTables() = default;

    explicit PageTables(const PageAllocator& allocator) {
        allocator_.alloc_zeroed_page = allocator.alloc_zeroed_page;
        allocator_.phys_to_ptr = allocator.phys_to_ptr;
        allocator_.ctx = allocator.ctx;
        root_ = new_table();
    }

    // A builder that extends the tables already at `root`, such as the ones
    // in CR3, without allocating a new root. The result is not ok() unless
    // root is a non-zero, 4 KiB-aligned address below 2^52 that the
    // allocator's phys_to_ptr resolves. The existing mappings are kept and
    // count as occupied; only new table pages come from the allocator.
    static PageTables adopt(const PageAllocator& allocator, u64 root) {
        using namespace paging_detail;

        PageTables tables;
        tables.allocator_.alloc_zeroed_page = allocator.alloc_zeroed_page;
        tables.allocator_.phys_to_ptr = allocator.phys_to_ptr;
        tables.allocator_.ctx = allocator.ctx;
        if (root == 0 || (root & (page_size - 1)) != 0 || root >= phys_limit ||
            table_at(tables.allocator_, root) == nullptr) {
            tables.failed_ = true;
            return tables;
        }
        tables.root_ = root;
        return tables;
    }

    bool ok() const { return root_ != 0 && !failed_; }

    u64 root() const { return root_; }

    // Maps [virt, virt + bytes) to [phys, phys + bytes). All three values
    // must be 2 MiB aligned, bytes must be non-zero, virt must be canonical
    // and the range must stay in one half of the address space. A range that
    // overlaps an existing mapping is refused before any entry is changed.
    bool map_2m(u64 virt, u64 phys, u64 bytes, u64 flags) {
        using namespace paging_detail;

        if (!ok() || bytes == 0) {
            return false;
        }
        if (((virt | phys | bytes) & large_page_mask) != 0) {
            return false;
        }
        if ((flags & ~allowed_flags) != 0) {
            return false;
        }

        // bytes - 1 is the offset of the last byte; comparing it against the
        // room left above virt and phys catches wraparound.
        const u64 span = bytes - 1;
        if (span > ~virt || span > ~phys) {
            return false;
        }
        const u64 last_virt = virt + span;
        const u64 last_phys = phys + span;
        if (!is_canonical(virt) || !is_canonical(last_virt) ||
            (virt >> 63) != (last_virt >> 63)) {
            return false;
        }
        if (last_phys >= phys_limit) {
            return false;
        }

        for (u64 offset = 0; offset < bytes; offset += large_page_size) {
            const u64* slot = find_pd_slot(allocator_, root_, virt + offset);
            if (slot != nullptr && (*slot & Present) != 0) {
                return false;
            }
        }

        for (u64 offset = 0; offset < bytes; offset += large_page_size) {
            u64* slot = ensure_pd_slot(virt + offset);
            if (slot == nullptr) {
                failed_ = true;
                return false;
            }
            *slot = ((phys + offset) & large_addr_mask) | (flags & allowed_flags) | Present |
                    entry_large;
        }
        return true;
    }

    // Sets the flags in `set` and clears those in `clear` on every 2 MiB
    // page of [virt, virt + bytes). virt and bytes must be 2 MiB aligned,
    // bytes non-zero, and Present is not one of the flags. Unless every page
    // of the range is mapped as a present 2 MiB page, the call is refused
    // before any entry is changed. No table is allocated. The caller flushes
    // the old translations from the TLB.
    bool protect_2m(u64 virt, u64 bytes, u64 set, u64 clear) {
        using namespace paging_detail;

        if (!ok() || bytes == 0 || ((virt | bytes) & large_page_mask) != 0) {
            return false;
        }
        const u64 changeable = allowed_flags & ~u64{Present};
        if (((set | clear) & ~changeable) != 0) {
            return false;
        }
        if (bytes - 1 > ~virt) {
            return false;
        }
        for (u64 offset = 0; offset < bytes; offset += large_page_size) {
            const u64* slot = find_pd_slot(allocator_, root_, virt + offset);
            if (slot == nullptr || (*slot & (Present | entry_large)) != (Present | entry_large)) {
                return false;
            }
        }
        for (u64 offset = 0; offset < bytes; offset += large_page_size) {
            u64* slot = find_pd_slot(allocator_, root_, virt + offset);
            *slot = (*slot & ~clear) | set;
        }
        return true;
    }

private:
    // Returns the physical address of a fresh table, or 0 after marking the
    // builder as failed.
    u64 new_table() {
        using namespace paging_detail;

        if (allocator_.alloc_zeroed_page == nullptr) {
            failed_ = true;
            return 0;
        }
        const u64 phys = allocator_.alloc_zeroed_page(allocator_.ctx);
        if (phys == 0 || (phys & (page_size - 1)) != 0 || phys >= phys_limit ||
            table_at(allocator_, phys) == nullptr) {
            failed_ = true;
            return 0;
        }
        return phys;
    }

    // Like find_pd_slot, but creates missing PDPT and PD tables.
    u64* ensure_pd_slot(u64 virt) {
        using namespace paging_detail;

        u64* table = table_at(allocator_, root_);
        for (unsigned level = level_pml4; level >= level_pdpt; --level) {
            u64* entry = &table[index_at(virt, level)];
            if ((*entry & Present) == 0) {
                const u64 child = new_table();
                if (child == 0) {
                    return nullptr;
                }
                *entry = child | Present | Writable;
            } else if ((*entry & entry_large) != 0) {
                return nullptr;
            }
            table = table_at(allocator_, *entry & table_addr_mask);
            if (table == nullptr) {
                return nullptr;
            }
        }
        return &table[index_at(virt, level_pd)];
    }

    PageAllocator allocator_{nullptr, nullptr, nullptr};
    u64 root_ = 0;
    bool failed_ = false;
};

// Software page walk over tables built by PageTables. Returns false, leaving
// phys_out untouched, when virt is not covered by a present 2 MiB page.
inline bool translate(u64 root, const PageAllocator& allocator, u64 virt, u64& phys_out) {
    using namespace paging_detail;

    const u64* slot = find_pd_slot(allocator, root, virt);
    if (slot == nullptr) {
        return false;
    }
    const u64 entry = *slot;
    if ((entry & Present) == 0 || (entry & entry_large) == 0) {
        return false;
    }
    phys_out = (entry & large_addr_mask) | (virt & large_page_mask);
    return true;
}

}
