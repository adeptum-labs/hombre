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

// The MMIO window starts at PML4 index 384. The loader's layout leaves it
// free: the direct map is at 0xFFFF800000000000 and the kernel window at
// 0xFFFFFFFF80000000.
inline constexpr u64 kMmioBase = 0xFFFFC00000000000ull;

// How many 2 MiB pages the mapper tracks, so the window spans 32 MiB.
inline constexpr u32 kMmioMaxMappings = 16;

// Maps device memory into the MMIO window as uncached (PCD and PWT set),
// writable, no-execute 2 MiB pages. Pages are handed out upward from
// kMmioBase and are never unmapped.
//
// No TLB flush is needed after map(): the window's virtual addresses are
// handed out once and never held a translation before. A caller that ever
// replaces or removes an entry must run invlpg on the old address.
class MmioMapper {
public:
    // `tables` extends the live tables, as made by PageTables::adopt from the
    // CR3 root. Call it once per boot: the window's first slot is handed out
    // again by a fresh mapper, but its entry is still present in the tables.
    // A second call is refused, returns false and changes nothing.
    bool init(PageTables tables) {
        if (initialised_) {
            return false;
        }
        tables_ = tables;
        count_ = 0;
        initialised_ = true;
        return true;
    }

    // Maps [phys, phys + bytes) and returns the virtual address of phys, or
    // nullptr. The range is rounded out to whole 2 MiB pages. A request that
    // lies in pages that are already mapped reuses them; a range that
    // crosses a 2 MiB boundary gets two or more pages that are contiguous in
    // the window, and when the pages are mapped but not next to each other a
    // fresh run is mapped. Returns nullptr for an empty or wrapping range, a
    // range past the 52-bit physical limit, a range that needs more pages
    // than are left in the table, and when no page-table page can be
    // allocated. Out of table pages is final for new pages: the builder
    // stays failed, but ranges that are already mapped keep resolving.
    volatile void* map(u64 phys, u64 bytes) {
        using namespace paging_detail;

        if (bytes == 0 || bytes - 1 > ~phys) {
            return nullptr;
        }
        const u64 first = phys & ~large_page_mask;
        const u64 last = (phys + (bytes - 1)) & ~large_page_mask;
        const u64 pages = ((last - first) >> large_page_shift) + 1;
        if (pages > kMmioMaxMappings) {
            return nullptr;
        }

        u32 slot = 0;
        if (!find_run(first, static_cast<u32>(pages), slot)) {
            if (count_ + pages > kMmioMaxMappings) {
                return nullptr;
            }
            slot = count_;
            const u64 virt = kMmioBase + (static_cast<u64>(slot) << large_page_shift);
            if (!tables_.map_2m(virt, first, pages << large_page_shift,
                                Present | Writable | WriteThrough | CacheDisable | NoExecute)) {
                return nullptr;
            }
            for (u64 i = 0; i < pages; ++i) {
                entries_[slot + i].phys = first + (i << large_page_shift);
                entries_[slot + i].virt = virt + (i << large_page_shift);
            }
            count_ += static_cast<u32>(pages);
        }
        return reinterpret_cast<volatile void*>(entries_[slot].virt + (phys - first));
    }

private:
    struct Entry {
        u64 phys;  // 2 MiB-aligned physical base
        u64 virt;  // its address in the window
    };

    static constexpr unsigned large_page_shift = 21;

    // Looks for `pages` mapped entries whose physical bases run on from
    // `first` in steps of 2 MiB. Entries are laid out in window order, so
    // neighbours in the array are neighbours in the window.
    bool find_run(u64 first, u32 pages, u32& slot) const {
        if (pages > count_) {
            return false;
        }
        for (u32 start = 0; start + pages <= count_; ++start) {
            u32 i = 0;
            while (i < pages &&
                   entries_[start + i].phys == first + (static_cast<u64>(i) << large_page_shift)) {
                ++i;
            }
            if (i == pages) {
                slot = start;
                return true;
            }
        }
        return false;
    }

    PageTables tables_;
    Entry entries_[kMmioMaxMappings] = {};
    u32 count_ = 0;
    bool initialised_ = false;
};

}
