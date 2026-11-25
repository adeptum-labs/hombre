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

#include "../common/boot_info.hpp"
#include "../common/types.hpp"

namespace hombre {

// Hands out 4 KiB physical pages from the Usable regions of the boot memory
// map. Two bitmaps cover physical pages 0 up to the end of the highest usable
// region: one marks pages that are in use or not usable, the other marks the
// pages the allocator manages. Only managed pages can be allocated or freed.
//
// A page is managed when some Usable region covers it, no other region
// covers it, it lies at or above 1 MiB, and it does not hold the bitmaps.
// The regions may be unsorted and may overlap. Pages that a region covers
// only in part are not managed.
class PhysAllocator {
public:
    // Maps a physical address to a pointer the kernel can read and write.
    // The bitmaps are reached through one call, so the pointer for their
    // first page must be followed by the pages after it.
    using ToPtr = u8* (*)(void* ctx, u64 phys);

    // Builds the bitmaps. They are placed at the start of the first usable
    // run, in address order, that is long enough, and their pages count as
    // used. Returns false, leaving the allocator empty, when there is no
    // usable memory or no run can hold the bitmaps. The regions are only
    // read during this call.
    bool init(const MemoryRegion* regions, u32 count, ToPtr to_ptr, void* ctx) {
        *this = PhysAllocator{};
        if (regions == nullptr || count == 0 || to_ptr == nullptr) {
            return false;
        }

        u64 total = 0;
        for (u32 i = 0; i < count; ++i) {
            u64 lo, hi;
            if (regions[i].type == MemType::Usable && span_of(regions[i], lo, hi) && hi > total) {
                total = hi;
            }
        }
        if (total == 0) {
            return false;
        }

        const u64 bitmap_bytes = (total + 7) / 8;
        const u64 table_pages = (2 * bitmap_bytes + page_size - 1) / page_size;

        // First usable run, in address order, with room for the bitmaps.
        // Usability only changes at a region edge, so the walk visits the
        // stretches between edges.
        u64 table_page = 0;
        bool found = false;
        bool in_run = false;
        u64 run_start = 0;
        u64 run_pages = 0;
        for (u64 page = min_page; page < total && !found;) {
            const u64 next = next_edge(regions, count, page, total);
            if (is_usable(regions, count, page)) {
                if (!in_run) {
                    in_run = true;
                    run_start = page;
                    run_pages = 0;
                }
                run_pages += next - page;
                if (run_pages >= table_pages) {
                    table_page = run_start;
                    found = true;
                }
            } else {
                in_run = false;
            }
            page = next;
        }
        if (!found) {
            return false;
        }

        u8* table = to_ptr(ctx, table_page << page_shift);
        if (table == nullptr) {
            return false;
        }
        used_ = table;
        managed_ = table + bitmap_bytes;
        total_pages_ = total;
        to_ptr_ = to_ptr;
        ctx_ = ctx;

        // Everything is used and unmanaged until a usable stretch says
        // otherwise. The unused bits past the last page stay set in the
        // first bitmap and clear in the second.
        __builtin_memset(used_, 0xFF, bitmap_bytes);
        __builtin_memset(managed_, 0, bitmap_bytes);
        for (u64 page = min_page; page < total;) {
            const u64 next = next_edge(regions, count, page, total);
            if (is_usable(regions, count, page)) {
                for (u64 p = page; p < next; ++p) {
                    clear_bit(used_, p);
                    set_bit(managed_, p);
                }
                free_pages_ += next - page;
                managed_pages_ += next - page;
            }
            page = next;
        }
        for (u64 p = table_page; p < table_page + table_pages; ++p) {
            set_bit(used_, p);
            clear_bit(managed_, p);
        }
        free_pages_ -= table_pages;
        managed_pages_ -= table_pages;
        return true;
    }

    // Returns the physical address of the first of count contiguous pages,
    // all zeroed, or 0 when count is 0 or no free run is long enough. Takes
    // the lowest run that fits.
    u64 alloc_pages(u64 count) {
        if (count == 0 || count > free_pages_) {
            return 0;
        }

        u64 run = 0;
        for (u64 page = 0; page < total_pages_;) {
            if (run == 0 && (page & 7) == 0 && used_[page >> 3] == 0xFF) {
                page += 8;
                continue;
            }
            if (test_bit(used_, page)) {
                run = 0;
            } else if (++run == count) {
                return claim(page + 1 - count, count);
            }
            ++page;
        }
        return 0;
    }

    // Like alloc_pages, but the first page number is a multiple of
    // align_pages, which must be a power of two: 512 gives a run that
    // starts on a 2 MiB boundary. Takes the lowest such run that fits.
    // Returns 0 when count is 0, the alignment is not a power of two or no
    // aligned free run is long enough.
    u64 alloc_pages_aligned(u64 count, u64 align_pages) {
        if (count == 0 || count > free_pages_ || align_pages == 0 ||
            (align_pages & (align_pages - 1)) != 0) {
            return 0;
        }
        // first < total_pages_ <= 2^52, so the next candidate cannot wrap.
        for (u64 first = 0; first < total_pages_ && count <= total_pages_ - first;) {
            u64 page = first;
            while (page < first + count && !test_bit(used_, page)) {
                ++page;
            }
            if (page == first + count) {
                return claim(first, count);
            }
            first = (page + align_pages) & ~(align_pages - 1);
        }
        return 0;
    }

    // Returns count pages starting at phys. A call that names a misaligned
    // address, a range that leaves the managed memory, or any page that is
    // not currently allocated is ignored as a whole.
    void free_pages(u64 phys, u64 count) {
        if (count == 0 || (phys & (page_size - 1)) != 0) {
            return;
        }
        const u64 first = phys >> page_shift;
        if (first >= total_pages_ || count > total_pages_ - first) {
            return;
        }
        for (u64 p = first; p < first + count; ++p) {
            if (!test_bit(managed_, p) || !test_bit(used_, p)) {
                return;
            }
        }
        for (u64 p = first; p < first + count; ++p) {
            clear_bit(used_, p);
        }
        free_pages_ += count;
    }

    u64 free_page_count() const { return free_pages_; }

    // Pages the allocator manages: those in a usable region it covers
    // entirely, less the pages its bitmaps take and any holes between the
    // usable regions.
    u64 managed_page_count() const { return managed_pages_; }

    // The pointer for a physical address, through the ToPtr given to init,
    // so the owner of an allocation can fill it. nullptr before init.
    u8* to_ptr(u64 phys) const { return to_ptr_ != nullptr ? to_ptr_(ctx_, phys) : nullptr; }

private:
    static constexpr u64 page_shift = 12;
    static constexpr u64 page_size = 1ull << page_shift;
    static constexpr u64 min_page = (1ull << 20) >> page_shift;
    // One past the highest page number an address can have.
    static constexpr u64 max_page = 1ull << (64 - page_shift);

    // The pages [lo, hi) a region speaks for. A usable region is trimmed to
    // the pages it covers entirely and to 1 MiB and above, and one whose end
    // does not fit in 64 bits is dropped. Any other region is widened to the
    // pages it touches, and one that does not fit in 64 bits covers
    // everything above its start. Returns false for an empty span.
    static bool span_of(const MemoryRegion& r, u64& lo, u64& hi) {
        if (r.pages == 0) {
            return false;
        }
        const bool usable = r.type == MemType::Usable;
        const u64 first = r.phys >> page_shift;
        if (r.pages > (~r.phys >> page_shift)) {
            lo = first;
            hi = max_page;
            return !usable;
        }
        const u64 end = r.phys + (r.pages << page_shift);
        if (usable) {
            lo = first + ((r.phys & (page_size - 1)) != 0 ? 1 : 0);
            hi = end >> page_shift;
            if (lo < min_page) {
                lo = min_page;
            }
            return lo < hi;
        }
        lo = first;
        hi = (end >> page_shift) + ((end & (page_size - 1)) != 0 ? 1 : 0);
        return true;
    }

    // True when a usable region covers the page and no other region does.
    static bool is_usable(const MemoryRegion* regions, u32 count, u64 page) {
        bool usable = false;
        for (u32 i = 0; i < count; ++i) {
            u64 lo, hi;
            if (!span_of(regions[i], lo, hi) || page < lo || page >= hi) {
                continue;
            }
            if (regions[i].type != MemType::Usable) {
                return false;
            }
            usable = true;
        }
        return usable;
    }

    // The lowest page above `page` where some region starts or ends, or
    // limit when there is none below it.
    static u64 next_edge(const MemoryRegion* regions, u32 count, u64 page, u64 limit) {
        u64 next = limit;
        for (u32 i = 0; i < count; ++i) {
            u64 lo, hi;
            if (!span_of(regions[i], lo, hi)) {
                continue;
            }
            if (lo > page && lo < next) {
                next = lo;
            }
            if (hi > page && hi < next) {
                next = hi;
            }
        }
        return next;
    }

    // Marks the free pages [first, first + count) used, zeroes them and
    // returns the address of the first.
    u64 claim(u64 first, u64 count) {
        for (u64 p = first; p < first + count; ++p) {
            set_bit(used_, p);
            __builtin_memset(to_ptr_(ctx_, p << page_shift), 0, page_size);
        }
        free_pages_ -= count;
        return first << page_shift;
    }

    static bool test_bit(const u8* bits, u64 index) { return (bits[index >> 3] >> (index & 7)) & 1; }
    static void set_bit(u8* bits, u64 index) { bits[index >> 3] |= u8(1u << (index & 7)); }
    static void clear_bit(u8* bits, u64 index) { bits[index >> 3] &= u8(~(1u << (index & 7))); }

    u8* used_ = nullptr;
    u8* managed_ = nullptr;
    u64 total_pages_ = 0;
    u64 free_pages_ = 0;
    u64 managed_pages_ = 0;
    ToPtr to_ptr_ = nullptr;
    void* ctx_ = nullptr;
};

}
