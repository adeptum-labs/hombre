#pragma once

#include "types.hpp"

namespace hombre {

enum class ElfError : u8 {
    Ok,
    TooSmall,
    BadMagic,
    NotElf64LittleEndian,
    NotX86_64,
    NotExecutable,
    BadProgramHeaders,
    SegmentOutsideFile,
    FileSizeExceedsMemSize,
    NotHigherHalf,
    TooManySegments,
};

struct Segment {
    u64 vaddr;
    u64 paddr;
    u64 file_offset;
    u64 file_size;
    u64 mem_size;
    u32 flags;
};

inline constexpr u32 max_elf_segments = 8;

struct ElfImage {
    u64 entry;
    u32 segment_count;
    Segment segments[max_elf_segments];
};

inline constexpr u64 higher_half_base = 0xFFFFFFFF80000000ull;

namespace elf_detail {

inline constexpr u64 header_size = 64;
inline constexpr u64 phdr_size = 56;
inline constexpr u16 type_exec = 2;
inline constexpr u16 machine_x86_64 = 62;
inline constexpr u32 pt_load = 1;

inline u64 read_le(const u8* p, unsigned bytes) {
    u64 value = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        value |= static_cast<u64>(p[i]) << (8 * i);
    }
    return value;
}

}

// Validates a statically linked x86-64 ELF64 executable held in
// data[0, size) and extracts its entry point and PT_LOAD segments. Every
// range check compares a length against the bytes remaining after its start
// (size - offset) so that hostile values cannot wrap around.
inline ElfError parse_elf(const u8* data, u64 size, ElfImage& out) {
    using namespace elf_detail;

    out.entry = 0;
    out.segment_count = 0;

    if (data == nullptr || size < header_size) {
        return ElfError::TooSmall;
    }
    if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
        return ElfError::BadMagic;
    }
    if (data[4] != 2 || data[5] != 1) {
        return ElfError::NotElf64LittleEndian;
    }
    if (read_le(data + 18, 2) != machine_x86_64) {
        return ElfError::NotX86_64;
    }
    if (read_le(data + 16, 2) != type_exec) {
        return ElfError::NotExecutable;
    }

    const u64 entry = read_le(data + 24, 8);
    const u64 phoff = read_le(data + 32, 8);
    const u64 phentsize = read_le(data + 54, 2);
    const u64 phnum = read_le(data + 56, 2);

    if (phentsize != phdr_size) {
        return ElfError::BadProgramHeaders;
    }
    // phnum is 16 bits, so the table length cannot overflow a u64.
    const u64 table_bytes = phnum * phdr_size;
    if (phoff > size || table_bytes > size - phoff) {
        return ElfError::BadProgramHeaders;
    }

    ElfImage image{};
    image.entry = entry;

    for (u64 i = 0; i < phnum; ++i) {
        const u8* ph = data + phoff + i * phdr_size;
        if (read_le(ph, 4) != pt_load) {
            continue;
        }
        if (image.segment_count == max_elf_segments) {
            return ElfError::TooManySegments;
        }

        Segment seg{};
        seg.flags = static_cast<u32>(read_le(ph + 4, 4));
        seg.file_offset = read_le(ph + 8, 8);
        seg.vaddr = read_le(ph + 16, 8);
        seg.paddr = read_le(ph + 24, 8);
        seg.file_size = read_le(ph + 32, 8);
        seg.mem_size = read_le(ph + 40, 8);

        if (seg.file_size > seg.mem_size) {
            return ElfError::FileSizeExceedsMemSize;
        }
        if (seg.file_offset > size || seg.file_size > size - seg.file_offset) {
            return ElfError::SegmentOutsideFile;
        }
        // The whole mapping must sit in the higher half without wrapping
        // past the top of the address space.
        if (seg.vaddr < higher_half_base || seg.mem_size > ~seg.vaddr + 1) {
            return ElfError::NotHigherHalf;
        }

        image.segments[image.segment_count++] = seg;
    }

    out = image;
    return ElfError::Ok;
}

inline const char* to_string(ElfError error) {
    switch (error) {
    case ElfError::Ok:
        return "ok";
    case ElfError::TooSmall:
        return "file too small";
    case ElfError::BadMagic:
        return "bad ELF magic";
    case ElfError::NotElf64LittleEndian:
        return "not a 64-bit little-endian ELF";
    case ElfError::NotX86_64:
        return "not an x86-64 ELF";
    case ElfError::NotExecutable:
        return "not an executable ELF";
    case ElfError::BadProgramHeaders:
        return "bad program header table";
    case ElfError::SegmentOutsideFile:
        return "segment outside file";
    case ElfError::FileSizeExceedsMemSize:
        return "segment file size exceeds memory size";
    case ElfError::NotHigherHalf:
        return "segment not in higher half";
    case ElfError::TooManySegments:
        return "too many loadable segments";
    }
    return "unknown error";
}

}
