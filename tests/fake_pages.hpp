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

#include "paging.hpp"

#include <cstdint>
#include <cstring>

namespace fake_pages {

inline constexpr hombre::usize kArenaPages = 128;

// Hands out 4 KiB-aligned pages from a static arena. Host pointers stand in
// for physical addresses, so phys_to_ptr is a plain cast.
struct Arena {
    alignas(4096) hombre::u8 memory[kArenaPages * 4096];
    hombre::usize allocated = 0;
    hombre::usize limit = kArenaPages;

    void reset(hombre::usize new_limit = kArenaPages) {
        allocated = 0;
        limit = new_limit;
    }

    static hombre::u64 alloc(void* ctx) {
        Arena* arena = static_cast<Arena*>(ctx);
        if (arena->allocated == arena->limit) {
            return 0;
        }
        hombre::u8* page = arena->memory + arena->allocated * 4096;
        ++arena->allocated;
        std::memset(page, 0, 4096);
        return reinterpret_cast<std::uintptr_t>(page);
    }

    static hombre::u64* to_ptr(void*, hombre::u64 phys) {
        return reinterpret_cast<hombre::u64*>(static_cast<std::uintptr_t>(phys));
    }

    hombre::PageAllocator allocator() {
        return hombre::PageAllocator{&Arena::alloc, &Arena::to_ptr, this};
    }
};

}
