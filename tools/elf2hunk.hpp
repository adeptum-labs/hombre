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

// Host only: converts a big-endian ELF32 m68k executable, linked with
// --emit-relocs, into an AmigaOS hunk executable.
//
// Every allocated section becomes one hunk, in the order code, data, bss:
// the one executable section, the one other section with contents and the
// one without. Sections of size 0 are dropped. A R_68K_32 relocation
// becomes a RELOC32 entry in the hunk holding the long, aimed at the hunk
// of its symbol's section. ld has already put the linked address in the
// long; it is rebased to an offset into the target hunk, which is what the
// loader adds the hunk's address to. Relocations of sections that are not
// allocated (debug information) are ignored. A PC-relative relocation
// needs no entry when it stays inside one section and is an error when it
// crosses sections, as the hunks are placed apart.

#include "types.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace hombre {

struct ConversionResult {
    std::vector<u8> hunk;
    std::string error;  // empty when the conversion worked

    bool ok() const { return error.empty(); }
};

namespace elf2hunk_detail {

inline constexpr u64 kHeaderBytes = 52;
inline constexpr u64 kSectionHeaderBytes = 40;
inline constexpr u64 kSymbolBytes = 16;
inline constexpr u64 kRelaBytes = 12;
inline constexpr u32 kMachine68k = 4;
inline constexpr u32 kTypeExec = 2;
inline constexpr u32 kShtSymtab = 2;
inline constexpr u32 kShtRela = 4;
inline constexpr u32 kShtRel = 9;
inline constexpr u32 kShtNobits = 8;
inline constexpr u32 kShtProgbits = 1;
inline constexpr u32 kShfAlloc = 2;
inline constexpr u32 kShfExec = 4;
inline constexpr u32 kShnAbs = 0xFFF1;
inline constexpr u32 kRel68kNone = 0;
inline constexpr u32 kRel68k32 = 1;
inline constexpr u32 kRel68kPc32 = 6;
inline constexpr u32 kRel68kPc16 = 5;
inline constexpr u32 kRel68kPc8 = 4;
inline constexpr u64 kMaxSectionBytes = 1u << 24;
inline constexpr u32 kHunkCode = 1001;
inline constexpr u32 kHunkData = 1002;
inline constexpr u32 kHunkBss = 1003;
inline constexpr u32 kHunkReloc32 = 1004;
inline constexpr u32 kHunkEnd = 1010;
inline constexpr u32 kHunkHeader = 1011;

struct Fail {
    std::string message;
};

// Reads the file big-endian, with every range checked against its size in
// u64 so that a hostile offset or count cannot wrap.
class Image {
public:
    explicit Image(const std::vector<u8>& bytes) : bytes_(bytes) {}

    u64 size() const { return bytes_.size(); }

    bool contains(u64 offset, u64 length) const {
        return offset <= size() && length <= size() - offset;
    }

    u32 u8_at(u64 offset) const { return at(offset, 1); }
    u32 u16_at(u64 offset) const { return at(offset, 2); }
    u32 u32_at(u64 offset) const { return at(offset, 4); }

private:
    u32 at(u64 offset, u64 length) const {
        if (!contains(offset, length)) {
            throw Fail{"the file ends inside a structure"};
        }
        u32 value = 0;
        for (u64 i = 0; i < length; ++i) {
            value = (value << 8) | bytes_[offset + i];
        }
        return value;
    }

    const std::vector<u8>& bytes_;
};

struct Section {
    u32 type;
    u32 flags;
    u32 addr;
    u32 offset;
    u32 size;
    u32 link;
    u32 info;
    u32 align;
};

enum class Kind : u8 { None, Code, Data, Bss };

// Where a section ended up: its hunk, and the address it was linked at.
struct Placed {
    Kind kind = Kind::None;
    u32 hunk = 0;
};

struct Relocation {
    u32 target_hunk;
    u32 offset;
};

struct Hunk {
    Kind kind;
    u32 section;
    std::vector<u8> contents;
    std::vector<Relocation> relocations;
};

inline void put32(std::vector<u8>& out, u32 value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<u8>(value >> shift));
    }
}

inline void patch32(std::vector<u8>& bytes, u64 at, u32 value) {
    for (int i = 0; i < 4; ++i) {
        bytes[at + static_cast<u64>(i)] = static_cast<u8>(value >> (24 - 8 * i));
    }
}

inline std::string number(u64 value) {
    return std::to_string(value);
}

inline std::vector<Section> read_sections(const Image& image) {
    if (image.size() < kHeaderBytes) {
        throw Fail{"not an ELF file: too small"};
    }
    if (image.u32_at(0) != 0x7F454C46) {
        throw Fail{"not an ELF file"};
    }
    if (image.u8_at(4) != 1) {
        throw Fail{"not a 32-bit ELF file"};
    }
    if (image.u8_at(5) != 2) {
        throw Fail{"a little-endian ELF file is not supported, expected big-endian"};
    }
    if (image.u16_at(16) != kTypeExec) {
        throw Fail{"not an executable: link with ld, not as a relocatable object"};
    }
    if (image.u16_at(18) != kMachine68k) {
        throw Fail{"not an m68k ELF file (machine " + number(image.u16_at(18)) + ")"};
    }
    const u64 table = image.u32_at(32);
    const u64 count = image.u16_at(48);
    if (image.u16_at(46) != kSectionHeaderBytes || count == 0 ||
        !image.contains(table, count * kSectionHeaderBytes)) {
        throw Fail{"the section header table is missing or outside the file"};
    }
    std::vector<Section> sections;
    for (u64 i = 0; i < count; ++i) {
        const u64 at = table + i * kSectionHeaderBytes;
        sections.push_back({image.u32_at(at + 4), image.u32_at(at + 8), image.u32_at(at + 12),
                            image.u32_at(at + 16), image.u32_at(at + 20), image.u32_at(at + 24),
                            image.u32_at(at + 28), image.u32_at(at + 32)});
    }
    return sections;
}

// One hunk per allocated section that has contents, with the contents of
// code and data copied in.
inline std::vector<Hunk> place_sections(const Image& image, const std::vector<Section>& sections,
                                        std::vector<Placed>& placed) {
    std::vector<Hunk> hunks;
    bool seen[4] = {};
    for (usize i = 0; i < sections.size(); ++i) {
        const Section& s = sections[i];
        if ((s.flags & kShfAlloc) == 0 || s.size == 0) {
            continue;
        }
        const bool bits = s.type == kShtProgbits;
        if (!bits && s.type != kShtNobits) {
            throw Fail{"section " + number(i) + " is allocated but neither contents nor bss"};
        }
        if (s.size > kMaxSectionBytes || s.align > 4) {
            throw Fail{"section " + number(i) + " is too large or aligned above 4"};
        }
        const Kind kind = !bits ? Kind::Bss : (s.flags & kShfExec) != 0 ? Kind::Code : Kind::Data;
        if (seen[static_cast<u8>(kind)]) {
            throw Fail{"more than one code, data or bss section: section " + number(i)};
        }
        seen[static_cast<u8>(kind)] = true;
        Hunk hunk{kind, static_cast<u32>(i), {}, {}};
        if (bits) {
            if (!image.contains(s.offset, s.size)) {
                throw Fail{"section " + number(i) + " lies outside the file"};
            }
            for (u32 b = 0; b < s.size; ++b) {
                hunk.contents.push_back(static_cast<u8>(image.u8_at(u64{s.offset} + b)));
            }
        }
        hunks.push_back(std::move(hunk));
    }
    if (!seen[static_cast<u8>(Kind::Code)]) {
        throw Fail{"no code section"};
    }
    std::sort(hunks.begin(), hunks.end(),
              [](const Hunk& a, const Hunk& b) { return a.kind < b.kind; });
    placed.assign(sections.size(), Placed{});
    for (u32 h = 0; h < hunks.size(); ++h) {
        placed[hunks[h].section] = {hunks[h].kind, h};
    }
    return hunks;
}

// The section a symbol of the symbol table at index `symtab` lives in, or
// kShnAbs for an absolute one.
inline u32 symbol_section(const Image& image, const std::vector<Section>& sections, u32 symtab,
                          u32 symbol) {
    if (symtab >= sections.size() || sections[symtab].type != kShtSymtab) {
        throw Fail{"a relocation section names no symbol table"};
    }
    const Section& table = sections[symtab];
    if (!image.contains(table.offset, table.size) ||
        u64{symbol} >= table.size / kSymbolBytes) {
        throw Fail{"a relocation names symbol " + number(symbol) + " outside the symbol table"};
    }
    const u32 index = image.u16_at(u64{table.offset} + u64{symbol} * kSymbolBytes + 14);
    if (index == 0 || (index >= sections.size() && index != kShnAbs)) {
        throw Fail{"a relocation names an undefined symbol or a section that does not exist"};
    }
    return index;
}

inline void convert_relocations(const Image& image, const std::vector<Section>& sections,
                                const std::vector<Placed>& placed, std::vector<Hunk>& hunks) {
    for (const Section& rela : sections) {
        if (rela.type != kShtRela && rela.type != kShtRel) {
            continue;
        }
        if (rela.info >= sections.size() || (sections[rela.info].flags & kShfAlloc) == 0) {
            continue;  // debug information
        }
        const Section& from = sections[rela.info];
        const Placed& source = placed[rela.info];
        if (from.size == 0) {
            continue;
        }
        if (rela.type == kShtRel || source.kind == Kind::Bss) {
            throw Fail{"relocations of section " + number(rela.info) + " are not supported"};
        }
        if (!image.contains(rela.offset, rela.size)) {
            throw Fail{"a relocation section lies outside the file"};
        }
        Hunk& hunk = hunks[source.hunk];
        for (u64 at = 0; at + kRelaBytes <= rela.size; at += kRelaBytes) {
            const u64 where = u64{rela.offset} + at;
            const u32 info = image.u32_at(where + 4);
            const u32 type = info & 0xFF;
            if (type == kRel68kNone) {
                continue;
            }
            const bool pc_relative =
                type == kRel68kPc8 || type == kRel68kPc16 || type == kRel68kPc32;
            if (type != kRel68k32 && !pc_relative) {
                throw Fail{"relocation type " + number(type) + " is not supported"};
            }
            const u32 target = symbol_section(image, sections, rela.link, info >> 8);
            if (pc_relative) {
                if (target != rela.info) {
                    throw Fail{"a PC-relative relocation reaches another section"};
                }
                continue;
            }
            if (target == kShnAbs) {
                continue;
            }
            if (placed[target].kind == Kind::None) {
                throw Fail{"a relocation targets section " + number(target) +
                           ", which is not a code, data or bss hunk"};
            }
            const u32 address = image.u32_at(where);
            if (address < from.addr || hunk.contents.size() - (address - from.addr) < 4 ||
                address - from.addr > hunk.contents.size()) {
                throw Fail{"a relocation lies outside section " + number(rela.info)};
            }
            const u32 place = address - from.addr;
            const Section& to = sections[target];
            const u32 linked = image.u32_at(u64{from.offset} + place);
            if (linked < to.addr || linked - to.addr > to.size) {
                throw Fail{"the address in a relocated long lies outside its target section"};
            }
            patch32(hunk.contents, place, linked - to.addr);
            hunk.relocations.push_back({placed[target].hunk, place});
        }
    }
}

inline u32 longs_for(u64 bytes) {
    return static_cast<u32>((bytes + 3) / 4);
}

inline std::vector<u8> write_hunks(const std::vector<Hunk>& hunks,
                                   const std::vector<Section>& sections) {
    std::vector<u8> out;
    for (u32 v : {kHunkHeader, 0u, static_cast<u32>(hunks.size()), 0u,
                  static_cast<u32>(hunks.size() - 1)}) {
        put32(out, v);
    }
    for (const Hunk& h : hunks) {
        put32(out, longs_for(sections[h.section].size));
    }
    for (const Hunk& h : hunks) {
        const u32 longs = longs_for(sections[h.section].size);
        put32(out, h.kind == Kind::Code ? kHunkCode : h.kind == Kind::Data ? kHunkData : kHunkBss);
        put32(out, longs);
        if (h.kind != Kind::Bss) {
            out.insert(out.end(), h.contents.begin(), h.contents.end());
            out.resize(out.size() + (u64{longs} * 4 - h.contents.size()), 0);
        }
        std::map<u32, std::vector<u32>> by_target;
        std::vector<u32> places;
        for (const Relocation& r : h.relocations) {
            by_target[r.target_hunk].push_back(r.offset);
            places.push_back(r.offset);
        }
        std::sort(places.begin(), places.end());
        if (std::adjacent_find(places.begin(), places.end()) != places.end()) {
            throw Fail{"two relocations at one place in section " + number(h.section)};
        }
        if (!by_target.empty()) {
            put32(out, kHunkReloc32);
            for (auto& [target, offsets] : by_target) {
                std::sort(offsets.begin(), offsets.end());
                put32(out, static_cast<u32>(offsets.size()));
                put32(out, target);
                for (u32 offset : offsets) {
                    put32(out, offset);
                }
            }
            put32(out, 0);
        }
        put32(out, kHunkEnd);
    }
    return out;
}

}

inline ConversionResult elf_to_hunk(const std::vector<u8>& elf) {
    using namespace elf2hunk_detail;
    ConversionResult result;
    try {
        const Image image(elf);
        const std::vector<Section> sections = read_sections(image);
        std::vector<Placed> placed;
        std::vector<Hunk> hunks = place_sections(image, sections, placed);
        convert_relocations(image, sections, placed, hunks);
        result.hunk = write_hunks(hunks, sections);
    } catch (const Fail& fail) {
        result.hunk.clear();
        result.error = fail.message;
    }
    return result;
}

}
