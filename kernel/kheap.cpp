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

#include "kheap.hpp"

#include "../common/heap.hpp"
#include "../common/serial.hpp"

#include <new>

namespace hombre {

namespace {

constexpr u64 page_size = 4096;

struct PageSource {
    PhysAllocator* pages = nullptr;
    u64 direct_map_base = 0;
};

// Constant initialised, so they sit in .bss without a constructor run.
constinit PageSource page_source;
constinit Heap heap;

void* grow(void* ctx, usize min_bytes, usize& got_bytes) {
    const PageSource& source = *static_cast<const PageSource*>(ctx);
    const u64 count = (min_bytes + page_size - 1) / page_size;
    const u64 phys = source.pages == nullptr ? 0 : source.pages->alloc_pages(count);
    if (phys == 0) {
        return nullptr;
    }
    got_bytes = count * page_size;
    return reinterpret_cast<void*>(source.direct_map_base + phys);
}

void* try_alloc(usize bytes, usize align = Heap::default_align) {
    return heap.alloc(bytes, align);
}

[[noreturn]] void out_of_memory() {
    serial::write("kernel: out of heap memory\n");
    for (;;) {
        asm volatile("cli; hlt");
    }
}

void* must_alloc(usize bytes, usize align = Heap::default_align) {
    void* block = try_alloc(bytes, align);
    if (block == nullptr) {
        out_of_memory();
    }
    return block;
}

usize as_bytes(std::align_val_t align) {
    return static_cast<usize>(align);
}

}

void kheap_init(PhysAllocator& pages, u64 direct_map_base) {
    page_source = {&pages, direct_map_base};
    heap.init(&grow, &page_source);
}

}

using namespace hombre;

void* operator new(std::size_t bytes) { return must_alloc(bytes); }
void* operator new[](std::size_t bytes) { return must_alloc(bytes); }
void* operator new(std::size_t bytes, std::align_val_t align) {
    return must_alloc(bytes, as_bytes(align));
}
void* operator new[](std::size_t bytes, std::align_val_t align) {
    return must_alloc(bytes, as_bytes(align));
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { return try_alloc(bytes); }
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { return try_alloc(bytes); }
void* operator new(std::size_t bytes, std::align_val_t align, const std::nothrow_t&) noexcept {
    return try_alloc(bytes, as_bytes(align));
}
void* operator new[](std::size_t bytes, std::align_val_t align, const std::nothrow_t&) noexcept {
    return try_alloc(bytes, as_bytes(align));
}

void operator delete(void* ptr) noexcept { heap.free(ptr); }
void operator delete[](void* ptr) noexcept { heap.free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { heap.free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { heap.free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { heap.free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { heap.free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { heap.free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { heap.free(ptr); }

void operator delete(void* ptr, const std::nothrow_t&) noexcept { heap.free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { heap.free(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    heap.free(ptr);
}
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    heap.free(ptr);
}
