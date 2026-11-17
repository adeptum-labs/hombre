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

// Host only: builds DMS archives for tests and tools. The kernel never
// includes this file.

#include "dms.hpp"

#include <vector>

namespace hombre {

namespace dms_writer_detail {

inline void put16(std::vector<u8>& v, usize at, u16 x) {
    v[at] = static_cast<u8>(x >> 8);
    v[at + 1] = static_cast<u8>(x);
}

inline void put32(std::vector<u8>& v, usize at, u32 x) {
    put16(v, at, static_cast<u16>(x >> 16));
    put16(v, at + 2, static_cast<u16>(x));
}

// SIMPLE: runs of four or more equal bytes, and any run of the escape byte
// that is longer than one, become escape + count + fill; a lone escape byte
// becomes escape + 0. Counts of 255 and more use the 16 bit form.
inline std::vector<u8> rle_encode(const u8* data, usize length) {
    std::vector<u8> out;
    usize i = 0;
    while (i < length) {
        const u8 value = data[i];
        usize run = 1;
        while (i + run < length && data[i + run] == value && run < 0xFFFF) {
            ++run;
        }
        if (value == 0x90 && run == 1) {
            out.push_back(0x90);
            out.push_back(0x00);
        } else if (run >= 4 || (value == 0x90 && run > 1)) {
            out.push_back(0x90);
            if (run < 0xFF) {
                out.push_back(static_cast<u8>(run));
                out.push_back(value);
            } else {
                out.push_back(0xFF);
                out.push_back(value);
                out.push_back(static_cast<u8>(run >> 8));
                out.push_back(static_cast<u8>(run));
            }
        } else {
            for (usize k = 0; k < run; ++k) {
                out.push_back(value);
            }
        }
        i += run;
    }
    return out;
}

}

// The 56 byte archive header. Packed and unpacked are the sizes it states.
inline std::vector<u8> dms_header(u16 mode, u16 first_track, u16 last_track, u32 packed_size,
                                  u32 unpacked_size, u16 general_info = 0, u16 disk_type = 0) {
    using namespace dms_writer_detail;
    std::vector<u8> h(dms_detail::header_len, 0);
    h[0] = 'D';
    h[1] = 'M';
    h[2] = 'S';
    h[3] = '!';
    put16(h, 10, general_info);
    put16(h, 16, first_track);
    put16(h, 18, last_track);
    put32(h, 20, packed_size);
    put32(h, 24, unpacked_size);
    put16(h, 50, disk_type);
    put16(h, 52, mode);
    put16(h, 54, dms_crc16(h.data() + 4, 50));
    return h;
}

// One track record around `length` unpacked bytes, stored as mode 0 or 1.
inline std::vector<u8> dms_track_record(u16 number, u16 mode, const u8* data, usize length) {
    using namespace dms_writer_detail;
    const std::vector<u8> packed =
        mode == 1 ? rle_encode(data, length) : std::vector<u8>(data, data + length);
    std::vector<u8> rec(dms_detail::record_len, 0);
    rec[0] = 'T';
    rec[1] = 'R';
    put16(rec, 2, number);
    put16(rec, 6, static_cast<u16>(packed.size()));
    put16(rec, 8, static_cast<u16>(length));
    put16(rec, 10, static_cast<u16>(length));
    rec[13] = static_cast<u8>(mode);
    put16(rec, 14, dms_detail::checksum16(data, length));
    put16(rec, 16, dms_crc16(packed.data(), packed.size()));
    put16(rec, 18, dms_crc16(rec.data(), 18));
    rec.insert(rec.end(), packed.begin(), packed.end());
    return rec;
}

// An archive of the whole image, one record per track. A short last track is
// padded with zeros.
inline std::vector<u8> build_dms(const std::vector<u8>& adf, u16 mode, u16 track_bytes) {
    const usize tracks = (adf.size() + track_bytes - 1) / track_bytes;
    std::vector<u8> padded(adf);
    padded.resize(tracks * track_bytes, 0);

    std::vector<u8> body;
    usize packed_bytes = 0;  // the packed data only, as the header counts it
    for (usize t = 0; t < tracks; ++t) {
        const auto rec =
            dms_track_record(static_cast<u16>(t), mode, padded.data() + t * track_bytes, track_bytes);
        packed_bytes += rec.size() - dms_detail::record_len;
        body.insert(body.end(), rec.begin(), rec.end());
    }
    std::vector<u8> dms = dms_header(mode, 0, static_cast<u16>(tracks - 1),
                                     static_cast<u32>(packed_bytes),
                                     static_cast<u32>(padded.size()));
    dms.insert(dms.end(), body.begin(), body.end());
    return dms;
}

}
