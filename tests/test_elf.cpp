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
#include "elf.hpp"

#include <cstring>
#include <vector>

using namespace hombre;

namespace {

constexpr u64 kEntry = 0xFFFFFFFF80100000ull;
constexpr u64 kHeaderSize = 64;
constexpr u64 kPhdrSize = 56;

struct Load {
    u32 type = 1;
    u32 flags = 5;
    u64 offset = 0;
    u64 vaddr = 0xFFFFFFFF80100000ull;
    u64 paddr = 0x100000;
    u64 filesz = 0x10;
    u64 memsz = 0x20;
};

void put(std::vector<u8>& b, u64 at, u64 value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        b[at + i] = static_cast<u8>(value >> (8 * i));
    }
}

// Builds an ELF file with `loads` program headers right after the ELF header
// and 0x1000 bytes of payload after the table.
std::vector<u8> build(const std::vector<Load>& loads) {
    const u64 table_end = kHeaderSize + kPhdrSize * loads.size();
    std::vector<u8> b(table_end + 0x1000, 0);
    b[0] = 0x7F;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = 2;  // ELFCLASS64
    b[5] = 1;  // little endian
    b[6] = 1;  // EV_CURRENT
    put(b, 16, 2, 2);  // ET_EXEC
    put(b, 18, 62, 2);  // EM_X86_64
    put(b, 20, 1, 4);
    put(b, 24, kEntry, 8);
    put(b, 32, kHeaderSize, 8);
    put(b, 52, kHeaderSize, 2);
    put(b, 54, kPhdrSize, 2);
    put(b, 56, loads.size(), 2);
    for (u64 i = 0; i < loads.size(); ++i) {
        const Load& l = loads[i];
        const u64 at = kHeaderSize + kPhdrSize * i;
        put(b, at + 0, l.type, 4);
        put(b, at + 4, l.flags, 4);
        put(b, at + 8, l.offset, 8);
        put(b, at + 16, l.vaddr, 8);
        put(b, at + 24, l.paddr, 8);
        put(b, at + 32, l.filesz, 8);
        put(b, at + 40, l.memsz, 8);
        put(b, at + 48, 0x1000, 8);
    }
    return b;
}

std::vector<Load> two_loads() {
    Load text;
    text.offset = kHeaderSize + kPhdrSize * 2;
    Load data;
    data.flags = 6;
    data.offset = text.offset + 0x10;
    data.vaddr = 0xFFFFFFFF80200000ull;
    data.paddr = 0x200000;
    data.filesz = 0x8;
    data.memsz = 0x1000;
    return {text, data};
}

ElfError parse(const std::vector<u8>& b, ElfImage& out) {
    return parse_elf(b.data(), b.size(), out);
}

ElfError parse(const std::vector<u8>& b) {
    ElfImage out{};
    return parse(b, out);
}

void valid_image_parses() {
    const std::vector<u8> b = build(two_loads());
    ElfImage img{};
    CHECK_EQ(parse(b, img), ElfError::Ok);
    CHECK_EQ(img.entry, kEntry);
    CHECK_EQ(img.segment_count, 2u);
    CHECK_EQ(img.segments[0].vaddr, 0xFFFFFFFF80100000ull);
    CHECK_EQ(img.segments[0].paddr, 0x100000ull);
    CHECK_EQ(img.segments[0].file_offset, kHeaderSize + kPhdrSize * 2);
    CHECK_EQ(img.segments[0].file_size, 0x10ull);
    CHECK_EQ(img.segments[0].mem_size, 0x20ull);
    CHECK_EQ(img.segments[0].flags, 5u);
    CHECK_EQ(img.segments[1].vaddr, 0xFFFFFFFF80200000ull);
    CHECK_EQ(img.segments[1].paddr, 0x200000ull);
    CHECK_EQ(img.segments[1].file_offset, kHeaderSize + kPhdrSize * 2 + 0x10);
    CHECK_EQ(img.segments[1].file_size, 8ull);
    CHECK_EQ(img.segments[1].mem_size, 0x1000ull);
    CHECK_EQ(img.segments[1].flags, 6u);
}

void rejects_short_buffer() {
    const std::vector<u8> b = build(two_loads());
    ElfImage img{};
    CHECK_EQ(parse_elf(b.data(), 0, img), ElfError::TooSmall);
    CHECK_EQ(parse_elf(b.data(), kHeaderSize - 1, img), ElfError::TooSmall);
    CHECK_EQ(parse_elf(nullptr, 0, img), ElfError::TooSmall);
}

void rejects_bad_magic() {
    std::vector<u8> b = build(two_loads());
    b[1] = 'X';
    CHECK_EQ(parse(b), ElfError::BadMagic);
}

void rejects_32bit() {
    std::vector<u8> b = build(two_loads());
    b[4] = 1;
    CHECK_EQ(parse(b), ElfError::NotElf64LittleEndian);
}

void rejects_big_endian() {
    std::vector<u8> b = build(two_loads());
    b[5] = 2;
    CHECK_EQ(parse(b), ElfError::NotElf64LittleEndian);
}

void rejects_non_x86_64() {
    std::vector<u8> b = build(two_loads());
    put(b, 18, 183, 2);  // EM_AARCH64
    CHECK_EQ(parse(b), ElfError::NotX86_64);
}

void rejects_non_executable() {
    std::vector<u8> b = build(two_loads());
    put(b, 16, 3, 2);  // ET_DYN
    CHECK_EQ(parse(b), ElfError::NotExecutable);
}

void rejects_bad_phentsize() {
    std::vector<u8> b = build(two_loads());
    put(b, 54, kPhdrSize - 1, 2);
    CHECK_EQ(parse(b), ElfError::BadProgramHeaders);
}

void rejects_phdr_table_past_eof() {
    std::vector<u8> b = build(two_loads());
    put(b, 32, b.size() - kPhdrSize, 8);  // room for one entry, two declared
    CHECK_EQ(parse(b), ElfError::BadProgramHeaders);
    put(b, 32, b.size() + 1, 8);
    CHECK_EQ(parse(b), ElfError::BadProgramHeaders);
}

void rejects_huge_phoff() {
    std::vector<u8> b = build(two_loads());
    put(b, 32, 0xFFFFFFFFFFFFFFF0ull, 8);
    CHECK_EQ(parse(b), ElfError::BadProgramHeaders);
    put(b, 32, 0xFFFFFFFFFFFFFFFFull, 8);
    CHECK_EQ(parse(b), ElfError::BadProgramHeaders);
}

void rejects_huge_phnum() {
    std::vector<u8> b = build(two_loads());
    put(b, 56, 0xFFFF, 2);
    CHECK_EQ(parse(b), ElfError::BadProgramHeaders);
}

void rejects_segment_past_eof() {
    std::vector<Load> loads = two_loads();
    loads[1].filesz = 0x2000;
    loads[1].memsz = 0x2000;
    CHECK_EQ(parse(build(loads)), ElfError::SegmentOutsideFile);
}

void rejects_segment_offset_past_eof() {
    std::vector<Load> loads = two_loads();
    loads[0].offset = 0x100000;
    CHECK_EQ(parse(build(loads)), ElfError::SegmentOutsideFile);
}

void rejects_segment_range_wraparound() {
    std::vector<Load> loads = two_loads();
    loads[0].offset = 0xFFFFFFFFFFFFFFF0ull;
    loads[0].filesz = 0x20;
    loads[0].memsz = 0x20;
    CHECK_EQ(parse(build(loads)), ElfError::SegmentOutsideFile);
    loads = two_loads();
    loads[0].offset = 0x80;
    loads[0].filesz = 0xFFFFFFFFFFFFFFF0ull;
    loads[0].memsz = 0xFFFFFFFFFFFFFFF0ull;
    CHECK_EQ(parse(build(loads)), ElfError::SegmentOutsideFile);
}

void rejects_filesz_gt_memsz() {
    std::vector<Load> loads = two_loads();
    loads[0].filesz = 0x30;
    loads[0].memsz = 0x20;
    CHECK_EQ(parse(build(loads)), ElfError::FileSizeExceedsMemSize);
}

void rejects_lower_half_vaddr() {
    std::vector<Load> loads = two_loads();
    loads[1].vaddr = 0x200000;
    CHECK_EQ(parse(build(loads)), ElfError::NotHigherHalf);
    loads[1].vaddr = 0xFFFFFFFF7FFFFFFFull;
    CHECK_EQ(parse(build(loads)), ElfError::NotHigherHalf);
}

void rejects_vaddr_range_wraparound() {
    std::vector<Load> loads = two_loads();
    loads[0].vaddr = 0xFFFFFFFFFFFFF000ull;
    loads[0].memsz = 0x2000;
    CHECK_EQ(parse(build(loads)), ElfError::NotHigherHalf);
}

void accepts_highest_vaddr_range() {
    std::vector<Load> loads = two_loads();
    loads[0].vaddr = 0xFFFFFFFFFFFFF000ull;
    loads[0].memsz = 0x1000;
    CHECK_EQ(parse(build(loads)), ElfError::Ok);
}

void accepts_exactly_eight_load_segments() {
    std::vector<Load> loads(8);
    for (u64 i = 0; i < loads.size(); ++i) {
        loads[i].offset = kHeaderSize + kPhdrSize * loads.size();
        loads[i].vaddr += 0x1000 * i;
    }
    ElfImage img{};
    CHECK_EQ(parse(build(loads), img), ElfError::Ok);
    CHECK_EQ(img.segment_count, 8u);
}

void rejects_nine_load_segments() {
    std::vector<Load> loads(9);
    for (u64 i = 0; i < loads.size(); ++i) {
        loads[i].offset = kHeaderSize + kPhdrSize * loads.size();
        loads[i].vaddr += 0x1000 * i;
    }
    CHECK_EQ(parse(build(loads)), ElfError::TooManySegments);
}

void ignores_non_load_segments() {
    std::vector<Load> loads = two_loads();
    Load note;
    note.type = 4;  // PT_NOTE: bogus bounds must not matter
    note.offset = 0xFFFFFFFFFFFFFFF0ull;
    note.vaddr = 0x1000;
    note.filesz = 0x1000;
    note.memsz = 0;
    Load stack;
    stack.type = 0x6474E551;  // PT_GNU_STACK
    loads.insert(loads.begin() + 1, note);
    loads.push_back(stack);
    ElfImage img{};
    CHECK_EQ(parse(build(loads), img), ElfError::Ok);
    CHECK_EQ(img.segment_count, 2u);
    CHECK_EQ(img.segments[0].vaddr, 0xFFFFFFFF80100000ull);
    CHECK_EQ(img.segments[1].vaddr, 0xFFFFFFFF80200000ull);
}

void non_load_segments_do_not_count_toward_limit() {
    std::vector<Load> loads(12);
    for (u64 i = 0; i < loads.size(); ++i) {
        loads[i].type = i < 8 ? 1 : 4;
        loads[i].offset = kHeaderSize + kPhdrSize * loads.size();
        loads[i].vaddr += 0x1000 * i;
    }
    ElfImage img{};
    CHECK_EQ(parse(build(loads), img), ElfError::Ok);
    CHECK_EQ(img.segment_count, 8u);
}

void to_string_names_every_error() {
    CHECK(std::strcmp(to_string(ElfError::Ok), "ok") == 0);
    CHECK(std::strcmp(to_string(ElfError::TooSmall), "file too small") == 0);
    CHECK(std::strcmp(to_string(ElfError::TooManySegments), "too many loadable segments") == 0);
    CHECK(std::strcmp(to_string(static_cast<ElfError>(200)), "unknown error") == 0);
    for (u8 e = 0; e <= static_cast<u8>(ElfError::TooManySegments); ++e) {
        CHECK(std::strcmp(to_string(static_cast<ElfError>(e)), "unknown error") != 0);
    }
}

}

int main() {
    valid_image_parses();
    rejects_short_buffer();
    rejects_bad_magic();
    rejects_32bit();
    rejects_big_endian();
    rejects_non_x86_64();
    rejects_non_executable();
    rejects_bad_phentsize();
    rejects_phdr_table_past_eof();
    rejects_huge_phoff();
    rejects_huge_phnum();
    rejects_segment_past_eof();
    rejects_segment_offset_past_eof();
    rejects_segment_range_wraparound();
    rejects_filesz_gt_memsz();
    rejects_lower_half_vaddr();
    rejects_vaddr_range_wraparound();
    accepts_highest_vaddr_range();
    accepts_exactly_eight_load_segments();
    rejects_nine_load_segments();
    ignores_non_load_segments();
    non_load_segments_do_not_count_toward_limit();
    to_string_names_every_error();
    return check::finish("test_elf");
}
