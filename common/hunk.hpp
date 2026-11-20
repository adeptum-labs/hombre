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
#include "m68k_cpu.hpp"

namespace hombre {

enum class HunkError : u32 {
    Ok,
    BadHeader,
    Truncated,
    UnsupportedHunk,
    BadReloc,
    OutOfMemory,
    TooManyHunks
};

struct HunkAllocator {
    u32 (*alloc)(void* ctx, u32 bytes);  // returns a guest address, 8-byte aligned, or 0 on failure
    void* ctx;
};

inline constexpr u32 kMaxHunks = 64;

namespace hunk_detail {

inline constexpr u32 kHunkName = 1000;
inline constexpr u32 kHunkCode = 1001;
inline constexpr u32 kHunkData = 1002;
inline constexpr u32 kHunkBss = 1003;
inline constexpr u32 kHunkReloc32 = 1004;
inline constexpr u32 kHunkSymbol = 1008;
inline constexpr u32 kHunkDebug = 1009;
inline constexpr u32 kHunkEnd = 1010;
inline constexpr u32 kHunkHeader = 1011;
inline constexpr u32 kHunkReloc32Short = 1020;
inline constexpr u32 kSizeMask = 0x3FFFFFFF;
inline constexpr u32 kSizeFlags = 0xC0000000;

// Sequential reader: pos never moves past size, so every subtraction below is
// safe and a short file fails instead of reading beyond it.
struct Reader {
    const u8* data;
    u64 size;
    u64 pos;

    bool read_long(u32& out) {
        if (size - pos < 4) {
            return false;
        }
        out = (static_cast<u32>(data[pos]) << 24) | (static_cast<u32>(data[pos + 1]) << 16) |
              (static_cast<u32>(data[pos + 2]) << 8) | data[pos + 3];
        pos += 4;
        return true;
    }

    bool read_word(u32& out) {
        if (size - pos < 2) {
            return false;
        }
        out = (static_cast<u32>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        return true;
    }

    bool take(u64 count, const u8*& out) {
        if (size - pos < count) {
            return false;
        }
        out = data + pos;
        pos += count;
        return true;
    }

    bool skip(u64 count) {
        if (size - pos < count) {
            return false;
        }
        pos += count;
        return true;
    }

    bool skip_longs(u32 longs) {
        return skip(static_cast<u64>(longs) * 4);
    }
};

struct SegmentSlot {
    u32 data_addr;
    u32 data_bytes;
};

inline bool read_be32(const m68k::GuestMemory& mem, u32 addr, u32& out) {
    if (!mem.range_ok(addr, 4)) {
        return false;
    }
    out = (static_cast<u32>(mem.base[addr]) << 24) | (static_cast<u32>(mem.base[addr + 1]) << 16) |
          (static_cast<u32>(mem.base[addr + 2]) << 8) | mem.base[addr + 3];
    return true;
}

inline bool write_be32(m68k::GuestMemory& mem, u32 addr, u32 value) {
    if (!mem.range_ok(addr, 4)) {
        return false;
    }
    mem.base[addr] = static_cast<u8>(value >> 24);
    mem.base[addr + 1] = static_cast<u8>(value >> 16);
    mem.base[addr + 2] = static_cast<u8>(value >> 8);
    mem.base[addr + 3] = static_cast<u8>(value);
    return true;
}

inline bool zero_fill(m68k::GuestMemory& mem, u32 addr, u32 bytes) {
    if (!mem.range_ok(addr, bytes)) {
        return false;
    }
    for (u32 i = 0; i < bytes; ++i) {
        mem.base[addr + i] = 0;
    }
    return true;
}

inline bool copy_in(m68k::GuestMemory& mem, u32 addr, const u8* src, u32 bytes) {
    if (!mem.range_ok(addr, bytes)) {
        return false;
    }
    for (u32 i = 0; i < bytes; ++i) {
        mem.base[addr + i] = src[i];
    }
    return true;
}

inline bool relocate(m68k::GuestMemory& mem, const SegmentSlot& current, u32 offset,
                     u32 target_addr) {
    if (static_cast<u64>(offset) + 4 > current.data_bytes) {
        return false;
    }
    const u32 addr = current.data_addr + offset;
    u32 value = 0;
    if (!read_be32(mem, addr, value)) {
        return false;
    }
    return write_be32(mem, addr, value + target_addr);
}

inline HunkError apply_reloc32(Reader& reader, m68k::GuestMemory& mem, const SegmentSlot* segments,
                               u32 hunk_count, u32 current) {
    for (;;) {
        u32 groups = 0;
        if (!reader.read_long(groups)) {
            return HunkError::Truncated;
        }
        if (groups == 0) {
            return HunkError::Ok;
        }
        u32 target = 0;
        if (!reader.read_long(target)) {
            return HunkError::Truncated;
        }
        if (target >= hunk_count) {
            return HunkError::BadReloc;
        }
        for (u32 i = 0; i < groups; ++i) {
            u32 offset = 0;
            if (!reader.read_long(offset)) {
                return HunkError::Truncated;
            }
            if (!relocate(mem, segments[current], offset, segments[target].data_addr)) {
                return HunkError::BadReloc;
            }
        }
    }
}

inline HunkError apply_reloc32_short(Reader& reader, m68k::GuestMemory& mem,
                                     const SegmentSlot* segments, u32 hunk_count, u32 current) {
    for (;;) {
        u32 groups = 0;
        if (!reader.read_word(groups)) {
            return HunkError::Truncated;
        }
        if (groups == 0) {
            break;
        }
        u32 target = 0;
        if (!reader.read_word(target)) {
            return HunkError::Truncated;
        }
        if (target >= hunk_count) {
            return HunkError::BadReloc;
        }
        for (u32 i = 0; i < groups; ++i) {
            u32 offset = 0;
            if (!reader.read_word(offset)) {
                return HunkError::Truncated;
            }
            if (!relocate(mem, segments[current], offset, segments[target].data_addr)) {
                return HunkError::BadReloc;
            }
        }
    }
    const u64 aligned = (reader.pos + 3) & ~static_cast<u64>(3);
    return reader.skip(aligned - reader.pos) ? HunkError::Ok : HunkError::Truncated;
}

inline HunkError skip_symbol(Reader& reader) {
    for (;;) {
        u32 name_longs = 0;
        if (!reader.read_long(name_longs)) {
            return HunkError::Truncated;
        }
        if (name_longs == 0) {
            return HunkError::Ok;
        }
        if (!reader.skip_longs(name_longs) || !reader.skip_longs(1)) {
            return HunkError::Truncated;
        }
    }
}

}

// Loads an AmigaOS hunk executable image held in file[0, bytes) into guest
// memory and builds the LoadSeg seglist. Every file read and guest write is
// bounds checked; sizes are computed in u64 so hostile values cannot wrap.
inline HunkError hunk_load(const u8* file, usize bytes, m68k::GuestMemory& mem,
                           const HunkAllocator& allocator, u32& seglist_bptr) {
    using namespace hunk_detail;

    seglist_bptr = 0;
    Reader reader{file, static_cast<u64>(bytes), 0};

    u32 magic = 0;
    if (!reader.read_long(magic) || magic != kHunkHeader) {
        return HunkError::BadHeader;
    }

    for (;;) {
        u32 name_longs = 0;
        if (!reader.read_long(name_longs)) {
            return HunkError::Truncated;
        }
        if (name_longs == 0) {
            break;
        }
        if (!reader.skip_longs(name_longs)) {
            return HunkError::Truncated;
        }
    }

    u32 table_size = 0;
    u32 first_hunk = 0;
    u32 last_hunk = 0;
    if (!reader.read_long(table_size) || !reader.read_long(first_hunk) ||
        !reader.read_long(last_hunk)) {
        return HunkError::Truncated;
    }
    if (first_hunk != 0 || last_hunk < first_hunk) {
        return HunkError::BadHeader;
    }
    const u64 hunk_count64 = static_cast<u64>(last_hunk) - first_hunk + 1;
    if (hunk_count64 > kMaxHunks) {
        return HunkError::TooManyHunks;
    }
    const u32 hunk_count = static_cast<u32>(hunk_count64);
    if (table_size < hunk_count) {
        return HunkError::BadHeader;
    }

    SegmentSlot segments[kMaxHunks] = {};
    u64 total_bytes = 0;
    for (u32 i = 0; i < hunk_count; ++i) {
        u32 raw = 0;
        if (!reader.read_long(raw)) {
            return HunkError::Truncated;
        }
        if ((raw & kSizeFlags) == kSizeFlags && !reader.skip(4)) {
            return HunkError::Truncated;
        }
        const u64 data_bytes = static_cast<u64>(raw & kSizeMask) * 4;
        segments[i].data_bytes = static_cast<u32>(data_bytes);
        total_bytes += data_bytes + 8;
    }
    if (total_bytes > mem.size) {
        return HunkError::OutOfMemory;
    }

    u32 previous_segment = 0;
    for (u32 i = 0; i < hunk_count; ++i) {
        // total_bytes already proved every segment fits in mem, so this
        // addition cannot overflow.
        const u32 total = segments[i].data_bytes + 8;
        const u32 segment = allocator.alloc(allocator.ctx, total);
        if (segment == 0 || (segment & 3) != 0 || !mem.range_ok(segment, total)) {
            return HunkError::OutOfMemory;
        }
        if (!zero_fill(mem, segment + 8, segments[i].data_bytes) ||
            !mem.write32(segment, total) || !mem.write32(segment + 4, 0)) {
            return HunkError::OutOfMemory;
        }
        if (previous_segment == 0) {
            seglist_bptr = (segment + 4) >> 2;
        } else if (!mem.write32(previous_segment + 4, (segment + 4) >> 2)) {
            return HunkError::OutOfMemory;
        }
        previous_segment = segment;
        segments[i].data_addr = segment + 8;
    }

    u32 current = 0;
    while (current < hunk_count) {
        u32 type = 0;
        if (!reader.read_long(type)) {
            return HunkError::Truncated;
        }
        // A hunk's type word may carry the memory flags its size word in
        // the header has, as a HUNK_DATA marked for chip memory does.
        type &= kSizeMask;
        if (type == kHunkCode || type == kHunkData) {
            u32 raw = 0;
            if (!reader.read_long(raw)) {
                return HunkError::Truncated;
            }
            const u64 data_bytes = static_cast<u64>(raw & kSizeMask) * 4;
            if (data_bytes > segments[current].data_bytes) {
                return HunkError::BadHeader;
            }
            const u8* data = nullptr;
            if (!reader.take(data_bytes, data)) {
                return HunkError::Truncated;
            }
            if (!copy_in(mem, segments[current].data_addr, data, static_cast<u32>(data_bytes))) {
                return HunkError::BadHeader;
            }
        } else if (type == kHunkBss) {
            if (!reader.skip(4)) {
                return HunkError::Truncated;
            }
        } else {
            return HunkError::UnsupportedHunk;
        }

        for (;;) {
            u32 block = 0;
            if (!reader.read_long(block)) {
                return HunkError::Truncated;
            }
            if (block == kHunkEnd) {
                break;
            }
            HunkError error = HunkError::Ok;
            if (block == kHunkReloc32) {
                error = apply_reloc32(reader, mem, segments, hunk_count, current);
            } else if (block == kHunkReloc32Short) {
                error = apply_reloc32_short(reader, mem, segments, hunk_count, current);
            } else if (block == kHunkSymbol) {
                error = skip_symbol(reader);
            } else if (block == kHunkDebug || block == kHunkName) {
                u32 longs = 0;
                error = reader.read_long(longs) && reader.skip_longs(longs) ? HunkError::Ok
                                                                            : HunkError::Truncated;
            } else {
                return HunkError::UnsupportedHunk;
            }
            if (error != HunkError::Ok) {
                return error;
            }
        }
        ++current;
    }
    return HunkError::Ok;
}

}
