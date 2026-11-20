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
#include "hunk.hpp"

#include <vector>

using namespace hombre;
using namespace hombre::m68k;

namespace {

constexpr u32 kHeader = 0x3F3;
constexpr u32 kName = 1000;
constexpr u32 kCode = 1001;
constexpr u32 kData = 1002;
constexpr u32 kBss = 1003;
constexpr u32 kReloc32 = 1004;
constexpr u32 kSymbol = 1008;
constexpr u32 kDebug = 1009;
constexpr u32 kEnd = 1010;

struct HunkFile {
    std::vector<u8> bytes;

    void push_long(u32 value) {
        bytes.push_back(static_cast<u8>(value >> 24));
        bytes.push_back(static_cast<u8>(value >> 16));
        bytes.push_back(static_cast<u8>(value >> 8));
        bytes.push_back(static_cast<u8>(value));
    }

    void push_word(u32 value) {
        bytes.push_back(static_cast<u8>(value >> 8));
        bytes.push_back(static_cast<u8>(value));
    }
};

struct BumpAllocator {
    u32 next = 0x1000;
    u32 used = 0;
    u32 fail_after = 0xFFFFFFFF;
};

u32 bump_alloc(void* ctx, u32 bytes) {
    auto& allocator = *static_cast<BumpAllocator*>(ctx);
    if (allocator.used == allocator.fail_after) {
        return 0;
    }
    const u32 address = allocator.next;
    allocator.next = (allocator.next + bytes + 7) & ~7u;
    ++allocator.used;
    return address;
}

struct Fixture {
    u8 memory[64 * 1024] = {};
    GuestMemory mem{memory, 64 * 1024};
    BumpAllocator bump;
};

struct Loaded {
    HunkError error;
    u32 seglist;
};

Loaded load(const std::vector<u8>& bytes, GuestMemory& mem, BumpAllocator& bump) {
    const HunkAllocator allocator{bump_alloc, &bump};
    u32 seglist = 0;
    const HunkError error = hunk_load(bytes.data(), bytes.size(), mem, allocator, seglist);
    return {error, seglist};
}

void put_header(HunkFile& file, const std::vector<u32>& size_longs) {
    file.push_long(kHeader);
    file.push_long(0);
    file.push_long(static_cast<u32>(size_longs.size()));
    file.push_long(0);
    file.push_long(static_cast<u32>(size_longs.size()) - 1);
    for (u32 size : size_longs) {
        file.push_long(size);
    }
}

u32 read_long(const GuestMemory& mem, u32 addr) {
    u32 value = 0;
    CHECK(mem.read32(addr, value));
    return value;
}

void loads_one_code_hunk() {
    HunkFile file;
    put_header(file, {2});
    file.push_long(kCode);
    file.push_long(2);
    file.push_long(0x11111111);
    file.push_long(0x11111112);
    file.push_long(kEnd);

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(loaded.seglist, 0x401u);
    CHECK_EQ(read_long(fixture.mem, 0x1000), 16u);
    CHECK_EQ(read_long(fixture.mem, 0x1004), 0u);
    const u8 expected[8] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12};
    for (u32 i = 0; i < 8; ++i) {
        CHECK_EQ(fixture.memory[0x1008 + i], expected[i]);
    }
}

void reloc32_adds_the_target_data_address() {
    HunkFile file;
    put_header(file, {2, 1});
    file.push_long(kCode);
    file.push_long(2);
    file.push_long(0x11111111);
    file.push_long(0x10);
    file.push_long(kReloc32);
    file.push_long(1);
    file.push_long(1);
    file.push_long(4);
    file.push_long(0);
    file.push_long(kEnd);
    file.push_long(kData);
    file.push_long(1);
    file.push_long(0x22222222);
    file.push_long(kEnd);

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(fixture.bump.next, 0x1020u);
    CHECK_EQ(read_long(fixture.mem, 0x100C), 0x1028u);
    CHECK_EQ(read_long(fixture.mem, 0x1018), 0x22222222u);
}

void copies_only_the_body_size_of_a_short_hunk() {
    HunkFile file;
    put_header(file, {4});
    file.push_long(kCode);
    file.push_long(1);
    file.push_long(0x44444444);
    file.push_long(kEnd);

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(read_long(fixture.mem, 0x1000), 24u);
    CHECK_EQ(read_long(fixture.mem, 0x1008), 0x44444444u);
    for (u32 i = 4; i < 16; ++i) {
        CHECK_EQ(fixture.memory[0x1008 + i], 0u);
    }
}

void bss_segment_is_zero_filled() {
    HunkFile file;
    put_header(file, {4});
    file.push_long(kBss);
    file.push_long(4);
    file.push_long(kEnd);

    Fixture fixture;
    for (u32 i = 0x1000; i < 0x1018; ++i) {
        fixture.memory[i] = 0xFF;
    }
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(read_long(fixture.mem, 0x1000), 24u);
    CHECK_EQ(read_long(fixture.mem, 0x1004), 0u);
    for (u32 i = 0; i < 16; ++i) {
        CHECK_EQ(fixture.memory[0x1008 + i], 0u);
    }
}

void segment_chain_links_head_to_tail() {
    HunkFile file;
    put_header(file, {1, 1, 1});
    for (u32 i = 0; i < 3; ++i) {
        file.push_long(kData);
        file.push_long(1);
        file.push_long(0x1000u + i);
        file.push_long(kEnd);
    }

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(loaded.seglist, (0x1000u + 4) >> 2);
    CHECK_EQ(read_long(fixture.mem, 0x1004), (0x1010u + 4) >> 2);
    CHECK_EQ(read_long(fixture.mem, 0x1014), (0x1020u + 4) >> 2);
    CHECK_EQ(read_long(fixture.mem, 0x1024), 0u);
}

void rejects_bad_magic() {
    HunkFile file;
    file.push_long(0x3F4);

    Fixture fixture;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::BadHeader);
}

void rejects_a_file_cut_inside_a_code_hunk() {
    HunkFile file;
    put_header(file, {2});
    file.push_long(kCode);
    file.push_long(2);
    file.push_long(0x11111111);
    file.push_long(0x11111112);
    file.push_long(kEnd);
    file.bytes.resize(file.bytes.size() - 8);

    Fixture fixture;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::Truncated);
}

void rejects_a_reloc_offset_beyond_the_hunk() {
    HunkFile file;
    put_header(file, {1});
    file.push_long(kCode);
    file.push_long(1);
    file.push_long(0);
    file.push_long(kReloc32);
    file.push_long(1);
    file.push_long(0);
    file.push_long(4);
    file.push_long(0);
    file.push_long(kEnd);

    Fixture fixture;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::BadReloc);
}

void rejects_a_reloc_to_a_hunk_that_does_not_exist() {
    HunkFile file;
    put_header(file, {1, 1});
    file.push_long(kCode);
    file.push_long(1);
    file.push_long(0);
    file.push_long(kReloc32);
    file.push_long(1);
    file.push_long(5);
    file.push_long(0);
    file.push_long(0);
    file.push_long(kEnd);
    file.push_long(kData);
    file.push_long(1);
    file.push_long(0);
    file.push_long(kEnd);

    Fixture fixture;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::BadReloc);
}

void rejects_a_huge_declared_size_without_overflow() {
    HunkFile file;
    put_header(file, {0x3FFFFFFF});

    Fixture fixture;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::OutOfMemory);
}

void rejects_too_many_hunks() {
    HunkFile file;
    file.push_long(kHeader);
    file.push_long(0);
    file.push_long(65);
    file.push_long(0);
    file.push_long(64);

    Fixture fixture;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::TooManyHunks);
}

void fails_when_the_second_allocation_fails() {
    HunkFile file;
    put_header(file, {1, 1});
    file.push_long(kCode);
    file.push_long(1);
    file.push_long(0);
    file.push_long(kEnd);
    file.push_long(kData);
    file.push_long(1);
    file.push_long(0);
    file.push_long(kEnd);

    Fixture fixture;
    fixture.bump.fail_after = 1;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::OutOfMemory);
    CHECK_EQ(fixture.bump.used, 1u);
}

void rejects_a_drel32_hunk() {
    HunkFile file;
    put_header(file, {1});
    file.push_long(1015);

    Fixture fixture;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::UnsupportedHunk);
}

void rejects_an_unknown_hunk_type() {
    HunkFile file;
    put_header(file, {1});
    file.push_long(0x1234);

    Fixture fixture;
    CHECK(load(file.bytes, fixture.mem, fixture.bump).error == HunkError::UnsupportedHunk);
}

void consumes_the_extra_long_when_both_size_flags_are_set() {
    HunkFile file;
    file.push_long(kHeader);
    file.push_long(0);
    file.push_long(1);
    file.push_long(0);
    file.push_long(0);
    file.push_long(0xC0000002);
    file.push_long(0);
    file.push_long(kCode);
    file.push_long(2);
    file.push_long(0x11111111);
    file.push_long(0x22222222);
    file.push_long(kEnd);

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(read_long(fixture.mem, 0x1000), 16u);
    CHECK_EQ(read_long(fixture.mem, 0x1008), 0x11111111u);
    CHECK_EQ(read_long(fixture.mem, 0x100C), 0x22222222u);
}

void a_single_size_flag_adds_no_extra_long() {
    HunkFile file;
    file.push_long(kHeader);
    file.push_long(0);
    file.push_long(1);
    file.push_long(0);
    file.push_long(0);
    file.push_long(0x80000001);
    file.push_long(kCode);
    file.push_long(1);
    file.push_long(0x33333333);
    file.push_long(kEnd);

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(read_long(fixture.mem, 0x1000), 12u);
    CHECK_EQ(read_long(fixture.mem, 0x1008), 0x33333333u);
}

void a_hunk_type_marked_for_chip_memory_loads() {
    HunkFile file;
    put_header(file, {0x40000001, 1});
    file.push_long(kCode | 0x40000000);
    file.push_long(1);
    file.push_long(0x44444444);
    file.push_long(kEnd);
    file.push_long(kData | 0x40000000);
    file.push_long(1);
    file.push_long(0x55555555);
    file.push_long(kEnd);

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(read_long(fixture.mem, 0x1008), 0x44444444u);
    const u32 next = read_long(fixture.mem, loaded.seglist << 2);
    CHECK_EQ(read_long(fixture.mem, (next << 2) + 4), 0x55555555u);
}

void reloc32short_applies_word_offsets_and_skips_the_padding() {
    HunkFile file;
    put_header(file, {2, 1});
    file.push_long(kCode);
    file.push_long(2);
    file.push_long(0x20);
    file.push_long(0x30);
    file.push_long(1020);
    file.push_word(2);
    file.push_word(1);
    file.push_word(0);
    file.push_word(4);
    file.push_word(0);
    file.push_word(0);
    file.push_long(kEnd);
    file.push_long(kData);
    file.push_long(1);
    file.push_long(0x22222222);
    file.push_long(kEnd);

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(read_long(fixture.mem, 0x1008), 0x20u + 0x1018u);
    CHECK_EQ(read_long(fixture.mem, 0x100C), 0x30u + 0x1018u);
}

void symbol_debug_and_name_blocks_are_skipped() {
    HunkFile file;
    put_header(file, {1});
    file.push_long(kCode);
    file.push_long(1);
    file.push_long(0x12345678);
    file.push_long(kSymbol);
    file.push_long(2);
    file.push_long(0x4E414D45);
    file.push_long(0);
    file.push_long(0xDEADBEEF);
    file.push_long(0);
    file.push_long(kDebug);
    file.push_long(1);
    file.push_long(0xCAFEBABE);
    file.push_long(kName);
    file.push_long(1);
    file.push_long(0x11223344);
    file.push_long(kEnd);

    Fixture fixture;
    const Loaded loaded = load(file.bytes, fixture.mem, fixture.bump);
    CHECK(loaded.error == HunkError::Ok);
    CHECK_EQ(read_long(fixture.mem, 0x1008), 0x12345678u);
}

void rejects_a_total_larger_than_guest_memory() {
    HunkFile file;
    put_header(file, {4});
    file.push_long(kCode);
    file.push_long(4);
    for (u32 i = 0; i < 4; ++i) {
        file.push_long(0x1000u + i);
    }
    file.push_long(kEnd);

    u8 memory[16] = {};
    GuestMemory mem{memory, 16};
    BumpAllocator bump;
    CHECK(load(file.bytes, mem, bump).error == HunkError::OutOfMemory);
}

}

int main() {
    a_hunk_type_marked_for_chip_memory_loads();
    loads_one_code_hunk();
    reloc32_adds_the_target_data_address();
    copies_only_the_body_size_of_a_short_hunk();
    bss_segment_is_zero_filled();
    segment_chain_links_head_to_tail();
    rejects_bad_magic();
    rejects_a_file_cut_inside_a_code_hunk();
    rejects_a_reloc_offset_beyond_the_hunk();
    rejects_a_reloc_to_a_hunk_that_does_not_exist();
    rejects_a_huge_declared_size_without_overflow();
    rejects_too_many_hunks();
    fails_when_the_second_allocation_fails();
    rejects_a_drel32_hunk();
    rejects_an_unknown_hunk_type();
    consumes_the_extra_long_when_both_size_flags_are_set();
    a_single_size_flag_adds_no_extra_long();
    reloc32short_applies_word_offsets_and_skips_the_padding();
    symbol_debug_and_name_blocks_are_skipped();
    rejects_a_total_larger_than_guest_memory();
    return check::finish("test_hunk");
}
