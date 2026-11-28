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

#include "../types.hpp"

// IFF ILBM pictures as the EA IFF 85 standard lays them out: a FORM of
// type ILBM holding BMHD, the bitmap header, then CAMG (the display mode),
// CMAP (the colours as R, G, B bytes), NAME and BODY, the rows of every
// plane in turn, plain or packed with ByteRun1. Pure C++: the caller hands
// in the rows and takes the bytes; nothing is allocated.

namespace hombre::image::ilbm {

// BMHD's bmh_Compression.
constexpr u8 kCompressNone = 0;
constexpr u8 kCompressByteRun1 = 1;

// The largest picture side the writer takes, and the most planes.
constexpr u32 kMaxSide = 16383;
constexpr u32 kMaxDepth = 8;

// The bytes of one row of one plane: a whole number of 16-bit words.
constexpr u32 row_bytes(u32 width) { return ((width + 15) / 16) * 2; }

// The most bytes ByteRun1 makes of a row of n bytes: a count byte for every
// 128 bytes of literals.
constexpr u32 packed_bound(u32 n) { return n + (n + 127) / 128; }

// The BMHD chunk's fields.
struct Header {
    u16 width = 0;
    u16 height = 0;
    i16 left = 0;
    i16 top = 0;
    u8 depth = 0;
    u8 masking = 0;
    u8 compression = kCompressByteRun1;
    u16 transparent = 0;
    u8 x_aspect = 1;
    u8 y_aspect = 1;
    i16 page_width = 0;
    i16 page_height = 0;
};

// What a picture file says besides its rows: colours are colour_count
// R, G, B triplets; a CAMG chunk goes out when has_mode; a NAME chunk when
// name_length is not 0.
struct Picture {
    Header header;
    bool has_mode = false;
    u32 mode_id = 0;
    const u8* colours = nullptr;
    u32 colour_count = 0;
    const u8* name = nullptr;
    u32 name_length = 0;
};

// Packs the n bytes at src with ByteRun1 into out, which has room for
// packed_bound(n): runs of three or more equal bytes as a repeat, the
// rest as literals, neither longer than 128 bytes. The bytes made.
inline u32 pack_row(const u8* src, u32 n, u8* out) {
    u32 at = 0;
    u32 i = 0;
    while (i < n) {
        u32 run = 1;
        while (i + run < n && run < 128 && src[i + run] == src[i]) {
            ++run;
        }
        if (run >= 3) {
            out[at++] = static_cast<u8>(257 - run);
            out[at++] = src[i];
            i += run;
            continue;
        }
        // Literals up to the next run of three, at most 128.
        const u32 start = i;
        while (i < n && i - start < 128) {
            if (i + 2 < n && src[i] == src[i + 1] && src[i] == src[i + 2]) {
                break;
            }
            ++i;
        }
        const u32 count = i - start;
        out[at++] = static_cast<u8>(count - 1);
        for (u32 k = 0; k < count; ++k) {
            out[at++] = src[start + k];
        }
    }
    return at;
}

namespace detail {

inline void be32(u8* at, u32 v) {
    at[0] = static_cast<u8>(v >> 24);
    at[1] = static_cast<u8>(v >> 16);
    at[2] = static_cast<u8>(v >> 8);
    at[3] = static_cast<u8>(v);
}

inline void be16(u8* at, u32 v) {
    at[0] = static_cast<u8>(v >> 8);
    at[1] = static_cast<u8>(v);
}

inline u32 id(const char (&name)[5]) {
    return (u32{static_cast<u8>(name[0])} << 24) | (u32{static_cast<u8>(name[1])} << 16) |
           (u32{static_cast<u8>(name[2])} << 8) | static_cast<u8>(name[3]);
}

template <typename Sink>
bool chunk_header(Sink& sink, u32 chunk_id, u32 size) {
    u8 head[8];
    be32(head, chunk_id);
    be32(head + 4, size);
    return sink(head, 8);
}

template <typename Sink>
bool pad(Sink& sink, u32 size) {
    const u8 zero = 0;
    return (size & 1) == 0 || sink(&zero, 1);
}

}

// Whether the writer takes the picture: sides 1 to kMaxSide, 1 to
// kMaxDepth planes, a known compression and no mask plane of its own.
inline bool writable(const Picture& p) {
    const Header& h = p.header;
    return h.width != 0 && h.height != 0 && h.width <= kMaxSide && h.height <= kMaxSide &&
           h.depth != 0 && h.depth <= kMaxDepth &&
           (h.compression == kCompressNone || h.compression == kCompressByteRun1) &&
           h.masking != 1 && p.colour_count <= 256;
}

// Writes the picture as an ILBM file through sink(const u8*, u32), which
// returns false to stop. rows(plane, y) gives the row_bytes(width) bytes of
// a row of a plane, or null to stop. With ByteRun1 the rows are read twice:
// once to size the BODY, once to write it. False when the picture is not
// writable, rows or sink stopped.
template <typename Rows, typename Sink>
bool write(const Picture& p, Rows&& rows, Sink&& sink) {
    if (!writable(p)) {
        return false;
    }
    const Header& h = p.header;
    const u32 bytes = row_bytes(h.width);
    u8 packed[packed_bound(row_bytes(kMaxSide))];
    u64 body = 0;
    for (u32 y = 0; y < h.height; ++y) {
        for (u32 plane = 0; plane < h.depth; ++plane) {
            const u8* row = rows(plane, y);
            if (row == nullptr) {
                return false;
            }
            body += h.compression == kCompressByteRun1 ? pack_row(row, bytes, packed) : bytes;
        }
    }
    const u32 colours = p.colour_count * 3;
    u64 form = 4 + 8 + 20 + 8 + body + (body & 1);
    form += p.has_mode ? 8 + 4 : 0;
    form += colours != 0 ? 8 + colours + (colours & 1) : 0;
    form += p.name_length != 0 ? 8 + p.name_length + (p.name_length & 1) : 0;
    if (form > 0x7FFFFFFF) {
        return false;
    }
    u8 head[12];
    detail::be32(head, detail::id("FORM"));
    detail::be32(head + 4, static_cast<u32>(form));
    detail::be32(head + 8, detail::id("ILBM"));
    if (!sink(head, 12)) {
        return false;
    }
    u8 bmhd[20];
    detail::be16(bmhd, h.width);
    detail::be16(bmhd + 2, h.height);
    detail::be16(bmhd + 4, static_cast<u16>(h.left));
    detail::be16(bmhd + 6, static_cast<u16>(h.top));
    bmhd[8] = h.depth;
    bmhd[9] = h.masking;
    bmhd[10] = h.compression;
    bmhd[11] = 0;
    detail::be16(bmhd + 12, h.transparent);
    bmhd[14] = h.x_aspect;
    bmhd[15] = h.y_aspect;
    detail::be16(bmhd + 16, static_cast<u16>(h.page_width));
    detail::be16(bmhd + 18, static_cast<u16>(h.page_height));
    if (!detail::chunk_header(sink, detail::id("BMHD"), 20) || !sink(bmhd, 20)) {
        return false;
    }
    if (p.has_mode) {
        u8 camg[4];
        detail::be32(camg, p.mode_id);
        if (!detail::chunk_header(sink, detail::id("CAMG"), 4) || !sink(camg, 4)) {
            return false;
        }
    }
    if (colours != 0 && (!detail::chunk_header(sink, detail::id("CMAP"), colours) ||
                         !sink(p.colours, colours) || !detail::pad(sink, colours))) {
        return false;
    }
    if (p.name_length != 0 &&
        (!detail::chunk_header(sink, detail::id("NAME"), p.name_length) ||
         !sink(p.name, p.name_length) || !detail::pad(sink, p.name_length))) {
        return false;
    }
    if (!detail::chunk_header(sink, detail::id("BODY"), static_cast<u32>(body))) {
        return false;
    }
    for (u32 y = 0; y < h.height; ++y) {
        for (u32 plane = 0; plane < h.depth; ++plane) {
            const u8* row = rows(plane, y);
            if (row == nullptr) {
                return false;
            }
            if (h.compression == kCompressByteRun1) {
                if (!sink(packed, pack_row(row, bytes, packed))) {
                    return false;
                }
            } else if (!sink(row, bytes)) {
                return false;
            }
        }
    }
    return detail::pad(sink, static_cast<u32>(body));
}

}
