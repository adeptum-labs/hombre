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
#include "elf2hunk.hpp"

#include <string>
#include <vector>

using namespace hombre;

namespace {

constexpr u32 kShtProgbits = 1;
constexpr u32 kShtSymtab = 2;
constexpr u32 kShtRela = 4;
constexpr u32 kShtNobits = 8;
constexpr u32 kShfWrite = 1;
constexpr u32 kShfAlloc = 2;
constexpr u32 kShfExec = 4;
constexpr u32 kRelocNone = 0;
constexpr u32 kReloc32 = 1;
constexpr u32 kReloc16 = 2;
constexpr u32 kRelocPc16 = 5;

using Bytes = std::vector<u8>;

void put16(Bytes& b, u32 v) {
    b.push_back(static_cast<u8>(v >> 8));
    b.push_back(static_cast<u8>(v));
}

void put32(Bytes& b, u32 v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}

void patch32(Bytes& b, usize at, u32 v) {
    b[at] = static_cast<u8>(v >> 24);
    b[at + 1] = static_cast<u8>(v >> 16);
    b[at + 2] = static_cast<u8>(v >> 8);
    b[at + 3] = static_cast<u8>(v);
}

struct Section {
    u32 type = 0;
    u32 flags = 0;
    u32 addr = 0;
    Bytes data;
    u32 nobits_size = 0;
    u32 link = 0;
    u32 info = 0;
    u32 entsize = 0;
};

// A big-endian ELF32 m68k executable: the header, the section contents one
// after the other and the section header table last.
Bytes build_elf(const std::vector<Section>& sections, bool little_endian = false) {
    Bytes out = {0x7F, 'E', 'L', 'F', 1, static_cast<u8>(little_endian ? 1 : 2), 1};
    out.resize(16, 0);
    put16(out, 2);   // ET_EXEC
    put16(out, 4);   // EM_68K
    put32(out, 1);   // version
    put32(out, 0);   // entry
    put32(out, 0);   // phoff
    put32(out, 0);   // shoff, patched below
    put32(out, 0);   // flags
    put16(out, 52);  // ehsize
    put16(out, 0);   // phentsize
    put16(out, 0);   // phnum
    put16(out, 40);  // shentsize
    put16(out, static_cast<u32>(sections.size()));
    put16(out, 0);   // shstrndx
    std::vector<u32> offsets;
    for (const Section& s : sections) {
        offsets.push_back(static_cast<u32>(out.size()));
        out.insert(out.end(), s.data.begin(), s.data.end());
    }
    patch32(out, 32, static_cast<u32>(out.size()));
    for (usize i = 0; i < sections.size(); ++i) {
        const Section& s = sections[i];
        const u32 size = s.type == kShtNobits ? s.nobits_size : static_cast<u32>(s.data.size());
        for (u32 v : {0u, s.type, s.flags, s.addr, offsets[i], size, s.link, s.info, 4u, s.entsize}) {
            put32(out, v);
        }
    }
    return out;
}

Section code(const Bytes& data) {
    return {kShtProgbits, kShfAlloc | kShfExec, 0, data};
}

Section data_section(u32 addr, const Bytes& data) {
    return {kShtProgbits, kShfAlloc | kShfWrite, addr, data};
}

Section bss_section(u32 addr, u32 size) {
    Section s{kShtNobits, kShfAlloc | kShfWrite, addr, {}};
    s.nobits_size = size;
    return s;
}

struct Symbol {
    u32 value;
    u32 section;
};

Section symbols(const std::vector<Symbol>& syms) {
    Section s{kShtSymtab, 0, 0, {}};
    s.entsize = 16;
    s.data.assign(16, 0);  // the null symbol
    for (const Symbol& sym : syms) {
        put32(s.data, 0);
        put32(s.data, sym.value);
        put32(s.data, 0);
        s.data.push_back(0);
        s.data.push_back(0);
        put16(s.data, sym.section);
    }
    return s;
}

struct Rela {
    u32 offset;
    u32 type;
    u32 symbol;
};

Section relocations(u32 symtab, u32 target, const std::vector<Rela>& relas) {
    Section s{kShtRela, 0, 0, {}};
    s.link = symtab;
    s.info = target;
    s.entsize = 12;
    for (const Rela& r : relas) {
        put32(s.data, r.offset);
        put32(s.data, (r.symbol << 8) | r.type);
        put32(s.data, 0);
    }
    return s;
}

Bytes longs(std::initializer_list<u32> values) {
    Bytes out;
    for (u32 v : values) {
        put32(out, v);
    }
    return out;
}

// JSR <long>.l ; RTS, the long at offset 2.
Bytes jsr_text(u32 target) {
    Bytes out = {0x4E, 0xB9};
    put32(out, target);
    put16(out, 0x4E75);
    return out;
}

// Sections: 0 null, 1 .text, 2 .symtab (null, then .text), 3 .rela.text.
Bytes text_with_reloc(u32 type) {
    return build_elf({{}, code(jsr_text(0)), symbols({{0, 1}}),
                      relocations(2, 1, {{2, type, 1}})});
}

void test_code_with_relocation() {
    const ConversionResult r = elf_to_hunk(text_with_reloc(kReloc32));
    CHECK(r.ok());
    CHECK(r.hunk == longs({0x3F3, 0, 1, 0, 0, 2,
                           0x3E9, 2, 0x4EB90000, 0x00004E75,
                           0x3EC, 1, 0, 2, 0,
                           0x3F2}));
}

void test_code_without_relocations() {
    const ConversionResult r = elf_to_hunk(build_elf({{}, code(longs({0x4E714E75, 0x4E75}))}));
    CHECK(r.ok());
    CHECK(r.hunk == longs({0x3F3, 0, 1, 0, 0, 2, 0x3E9, 2, 0x4E714E75, 0x00004E75, 0x3F2}));
}

void test_code_size_rounded_up_to_longs() {
    const ConversionResult r = elf_to_hunk(build_elf({{}, code({0x4E, 0x75, 0x4E, 0x75, 0x4E})}));
    CHECK(r.ok());
    CHECK(r.hunk == longs({0x3F3, 0, 1, 0, 0, 2, 0x3E9, 2, 0x4E754E75, 0x4E000000, 0x3F2}));
}

// .text at 0 (8 bytes), .data at 8 (8 bytes), .bss at 16 (6 bytes). The
// long at offset 2 of .text points into .data; the long at offset 4 of .data
// points into .text. Section symbols: 1 .text, 2 .data, 3 .bss.
Bytes three_sections(u32 text_to_data, u32 data_to_text) {
    return build_elf({{}, code(jsr_text(text_to_data)), data_section(8, longs({0, data_to_text})),
                      bss_section(16, 6), symbols({{0, 1}, {8, 2}, {16, 3}}),
                      relocations(4, 1, {{2, kReloc32, 2}}),
                      relocations(4, 2, {{12, kReloc32, 1}})});
}

void test_data_and_bss_hunks() {
    const ConversionResult r = elf_to_hunk(three_sections(12, 2));
    CHECK(r.ok());
    CHECK(r.hunk == longs({0x3F3, 0, 3, 0, 2, 2, 2, 2,
                           0x3E9, 2, 0x4EB90000, 0x00044E75,
                           0x3EC, 1, 1, 2, 0,
                           0x3F2,
                           0x3EA, 2, 0, 2,
                           0x3EC, 1, 0, 4, 0,
                           0x3F2,
                           0x3EB, 2,
                           0x3F2}));
}

void test_bss_target() {
    // A long in .text pointing 2 bytes into .bss gets hunk 2 and offset 2.
    const Bytes elf = build_elf({{}, code(jsr_text(18)), data_section(8, longs({1, 2})),
                                 bss_section(16, 6), symbols({{16, 3}}),
                                 relocations(4, 1, {{2, kReloc32, 1}})});
    const ConversionResult r = elf_to_hunk(elf);
    CHECK(r.ok());
    CHECK(r.hunk == longs({0x3F3, 0, 3, 0, 2, 2, 2, 2,
                           0x3E9, 2, 0x4EB90000, 0x00024E75,
                           0x3EC, 1, 2, 2, 0,
                           0x3F2,
                           0x3EA, 2, 1, 2,
                           0x3F2,
                           0x3EB, 2,
                           0x3F2}));
}

void test_relocation_groups() {
    Bytes text_bytes = longs({0, 0, 0, 0});
    patch32(text_bytes, 0, 16);  // -> .data + 0
    patch32(text_bytes, 4, 2);   // -> .text + 2
    patch32(text_bytes, 8, 20);  // -> .data + 4
    patch32(text_bytes, 12, 0);  // -> .text + 0
    const Bytes elf = build_elf({{}, code(text_bytes), data_section(16, longs({0, 0})),
                                 symbols({{0, 1}, {16, 2}}),
                                 relocations(3, 1, {{12, kReloc32, 1}, {4, kReloc32, 1},
                                                    {8, kReloc32, 2}, {0, kReloc32, 2}})});
    const ConversionResult r = elf_to_hunk(elf);
    CHECK(r.ok());
    CHECK(r.hunk == longs({0x3F3, 0, 2, 0, 1, 4, 2,
                           0x3E9, 4, 0, 2, 4, 0,
                           0x3EC, 2, 0, 4, 12,
                           2, 1, 0, 8,
                           0,
                           0x3F2,
                           0x3EA, 2, 0, 0,
                           0x3F2}));
}

void test_relocations_to_absolute_symbol_and_none_are_dropped() {
    const Bytes elf = build_elf({{}, code(jsr_text(0x1234)), symbols({{0x1234, 0xFFF1}}),
                                 relocations(2, 1, {{2, kReloc32, 1}, {0, kRelocNone, 0}})});
    const ConversionResult r = elf_to_hunk(elf);
    CHECK(r.ok());
    CHECK(r.hunk == longs({0x3F3, 0, 1, 0, 0, 2, 0x3E9, 2, 0x4EB90000, 0x12344E75, 0x3F2}));
}

void test_pc_relative_inside_one_section() {
    const Bytes elf = build_elf({{}, code(jsr_text(0)), symbols({{4, 1}}),
                                 relocations(2, 1, {{2, kRelocPc16, 1}})});
    const ConversionResult r = elf_to_hunk(elf);
    CHECK(r.ok());
    CHECK(r.hunk == longs({0x3F3, 0, 1, 0, 0, 2, 0x3E9, 2, 0x4EB90000, 0x00004E75, 0x3F2}));
}

void test_pc_relative_across_sections_rejected() {
    const Bytes elf = build_elf({{}, code(jsr_text(0)), data_section(8, longs({1, 2})),
                                 symbols({{8, 2}}), relocations(3, 1, {{2, kRelocPc16, 1}})});
    CHECK(!elf_to_hunk(elf).ok());
}

void test_unsupported_relocation_type_rejected() {
    const ConversionResult r = elf_to_hunk(text_with_reloc(kReloc16));
    CHECK(!r.ok());
    CHECK(r.error.find("relocation type 2") != std::string::npos);
    CHECK(r.hunk.empty());
}

void test_little_endian_rejected() {
    CHECK(!elf_to_hunk(build_elf({{}, code(jsr_text(0))}, true)).ok());
}

void test_other_machine_rejected() {
    Bytes elf = text_with_reloc(kReloc32);
    elf[19] = 3;
    CHECK(!elf_to_hunk(elf).ok());
}

void test_not_an_executable_rejected() {
    Bytes elf = text_with_reloc(kReloc32);
    elf[17] = 1;  // ET_REL
    CHECK(!elf_to_hunk(elf).ok());
}

void test_bad_magic_and_class_rejected() {
    Bytes elf = text_with_reloc(kReloc32);
    Bytes bad_magic = elf;
    bad_magic[1] = 'X';
    CHECK(!elf_to_hunk(bad_magic).ok());
    Bytes elf64 = elf;
    elf64[4] = 2;
    CHECK(!elf_to_hunk(elf64).ok());
}

void test_no_code_rejected() {
    CHECK(!elf_to_hunk(build_elf({{}, data_section(0, longs({1}))})).ok());
    CHECK(!elf_to_hunk(build_elf({{}})).ok());
}

void test_second_code_section_rejected() {
    CHECK(!elf_to_hunk(build_elf({{}, code(longs({1})), code(longs({2}))})).ok());
}

void test_relocation_outside_section_rejected() {
    // The long would end past the 8 bytes of .text.
    Bytes elf = build_elf({{}, code(jsr_text(0)), symbols({{0, 1}}),
                           relocations(2, 1, {{6, kReloc32, 1}})});
    CHECK(!elf_to_hunk(elf).ok());
    elf = build_elf({{}, code(jsr_text(0)), symbols({{0, 1}}),
                     relocations(2, 1, {{0xFFFFFFFE, kReloc32, 1}})});
    CHECK(!elf_to_hunk(elf).ok());
}

void test_relocation_value_outside_target_rejected() {
    CHECK(!elf_to_hunk(build_elf({{}, code(jsr_text(0x100)), symbols({{0, 1}}),
                                  relocations(2, 1, {{2, kReloc32, 1}})})).ok());
}

void test_duplicate_relocation_rejected() {
    CHECK(!elf_to_hunk(build_elf({{}, code(jsr_text(0)), symbols({{0, 1}}),
                                  relocations(2, 1, {{2, kReloc32, 1}, {2, kReloc32, 1}})}))
               .ok());
}

void test_bad_symbol_rejected() {
    // Symbol 5 does not exist; symbol 1 is undefined.
    CHECK(!elf_to_hunk(build_elf({{}, code(jsr_text(0)), symbols({{0, 1}}),
                                  relocations(2, 1, {{2, kReloc32, 5}})})).ok());
    CHECK(!elf_to_hunk(build_elf({{}, code(jsr_text(0)), symbols({{0, 0}}),
                                  relocations(2, 1, {{2, kReloc32, 1}})})).ok());
}

void test_relocation_in_bss_rejected() {
    CHECK(!elf_to_hunk(build_elf({{}, code(jsr_text(0)), bss_section(8, 8), symbols({{0, 1}}),
                                  relocations(3, 2, {{0, kReloc32, 1}})})).ok());
}

void test_debug_relocations_ignored() {
    Section debug{kShtProgbits, 0, 0, longs({0, 0})};
    const Bytes elf = build_elf({{}, code(jsr_text(0)), debug, symbols({{0, 1}}),
                                 relocations(3, 2, {{0, kReloc16, 1}})});
    CHECK(elf_to_hunk(elf).ok());
}

void test_truncated_files_rejected() {
    const Bytes elf = three_sections(12, 2);
    CHECK(elf_to_hunk(elf).ok());
    for (usize n = 0; n < elf.size(); ++n) {
        CHECK(!elf_to_hunk(Bytes(elf.begin(), elf.begin() + static_cast<long>(n))).ok());
    }
}

// Every byte of a valid file set to a hostile value, one at a time: the
// result may be either, but never a crash or an overrun (the sanitizers
// are what fail).
void test_corrupted_files_do_not_crash() {
    const Bytes elf = three_sections(12, 2);
    for (usize i = 0; i < elf.size(); ++i) {
        for (u8 v : {u8{0x00}, u8{0x01}, u8{0x7F}, u8{0x80}, u8{0xFF}}) {
            Bytes bad = elf;
            bad[i] = v;
            const ConversionResult r = elf_to_hunk(bad);
            CHECK(r.ok() == r.error.empty());
            CHECK(r.ok() || r.hunk.empty());
        }
    }
}

}  // namespace

int main() {
    test_code_with_relocation();
    test_code_without_relocations();
    test_code_size_rounded_up_to_longs();
    test_data_and_bss_hunks();
    test_bss_target();
    test_relocation_groups();
    test_relocations_to_absolute_symbol_and_none_are_dropped();
    test_pc_relative_inside_one_section();
    test_pc_relative_across_sections_rejected();
    test_unsupported_relocation_type_rejected();
    test_little_endian_rejected();
    test_other_machine_rejected();
    test_not_an_executable_rejected();
    test_bad_magic_and_class_rejected();
    test_no_code_rejected();
    test_second_code_section_rejected();
    test_relocation_outside_section_rejected();
    test_relocation_value_outside_target_rejected();
    test_duplicate_relocation_rejected();
    test_bad_symbol_rejected();
    test_relocation_in_bss_rejected();
    test_debug_relocations_ignored();
    test_truncated_files_rejected();
    test_corrupted_files_do_not_crash();
    return check::finish("test_elf2hunk");
}
