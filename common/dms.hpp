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

namespace hombre {

// Unpacker for DMS disk archives: the container (a 56 byte header and one
// record per track) plus the NOCOMP and SIMPLE track modes. The result is the
// raw disk image the archive was made from.

enum class DmsError : u32 {
    Ok,
    NotDms,           // the first bytes are not "DMS!"
    HeaderCrc,        // the archive header fails its CRC
    Truncated,        // shorter than its header says, ends inside a track, or has no disk track
    TrackHeader,      // a track record has no "TR", a bad header CRC or a stray number
    TrackCrc,         // a track's packed data fails its CRC
    TrackChecksum,    // a track's unpacked bytes fail their checksum
    Encrypted,        // the archive is encrypted
    UnsupportedMode,  // a track uses a mode other than NOCOMP, SIMPLE or HEAVY2
    BadTrackSize,     // a track is not a whole DD or HD cylinder, or differs from the first
    OutputTooSmall,   // the image does not fit the output buffer
    Corrupt,          // a track's packed data does not decode, or a track appears twice
};

struct DmsInfo {
    u16 mode;          // compression mode named by the archive header
    u16 general_info;  // header flag bits
    u16 disk_type;
    u16 first_track;
    u16 last_track;
    u32 track_bytes;  // bytes in one track, 0 until the first disk track is seen
    // dms_read_header: the unpacked size the header states. dms_unpack:
    // (last_track + 1) * track_bytes once track_bytes is known.
    u32 image_bytes;
    u32 tracks_unpacked;  // disk tracks decoded
    u32 failed_track;     // number of the track an error came from, else 0xFFFFFFFF
};

// Decoder state; the caller zero-fills it before the first use. It is large,
// so it does not belong on a small stack.
struct DmsWork {
    u8 track[22528];  // one unpacked HD track
};

namespace dms_detail {

inline constexpr usize header_len = 56;
inline constexpr usize record_len = 20;
inline constexpr u32 no_track = 0xFFFFFFFFu;
inline constexpr u16 max_disk_track = 80;    // numbers from here up are not disk data
inline constexpr u16 min_disk_bytes = 2048;  // tracks up to this long are banners and notes
inline constexpr u16 dd_track_bytes = 11264;
inline constexpr u16 hd_track_bytes = 22528;
inline constexpr u16 info_encrypted = 0x02;
inline constexpr u16 info_dev_encrypted = 0x40;
inline constexpr u8 rle_escape = 0x90;

static_assert(sizeof(DmsWork::track) == hd_track_bytes);

inline u16 be16(const u8* p) { return static_cast<u16>(p[0] << 8 | p[1]); }

inline u32 be32(const u8* p) {
    return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 |
           static_cast<u32>(p[2]) << 8 | p[3];
}

// CRC-16/ARC, one table lookup per byte.
struct Crc16Table {
    u16 entry[256];

    constexpr Crc16Table() : entry{} {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1) != 0 ? (c >> 1) ^ 0xA001u : c >> 1;
            }
            entry[i] = static_cast<u16>(c);
        }
    }
};

inline constexpr Crc16Table crc16_table{};

}

inline u16 dms_crc16(const u8* data, usize length) {
    u16 crc = 0;
    for (usize i = 0; i < length; ++i) {
        crc = static_cast<u16>((crc >> 8) ^ dms_detail::crc16_table.entry[(crc ^ data[i]) & 0xFF]);
    }
    return crc;
}

inline const char* dms_error_name(DmsError error) {
    switch (error) {
    case DmsError::Ok:
        return "Ok";
    case DmsError::NotDms:
        return "NotDms";
    case DmsError::HeaderCrc:
        return "HeaderCrc";
    case DmsError::Truncated:
        return "Truncated";
    case DmsError::TrackHeader:
        return "TrackHeader";
    case DmsError::TrackCrc:
        return "TrackCrc";
    case DmsError::TrackChecksum:
        return "TrackChecksum";
    case DmsError::Encrypted:
        return "Encrypted";
    case DmsError::UnsupportedMode:
        return "UnsupportedMode";
    case DmsError::BadTrackSize:
        return "BadTrackSize";
    case DmsError::OutputTooSmall:
        return "OutputTooSmall";
    case DmsError::Corrupt:
        return "Corrupt";
    }
    return "?";
}

inline const char* dms_mode_name(u16 mode) {
    switch (mode) {
    case 0:
        return "NOCOMP";
    case 1:
        return "SIMPLE";
    case 2:
        return "QUICK";
    case 3:
        return "MEDIUM";
    case 4:
        return "DEEP";
    case 5:
        return "HEAVY1";
    case 6:
        return "HEAVY2";
    default:
        return "?";
    }
}

// Validates the archive header and fills the header fields of info.
inline DmsError dms_read_header(const u8* in, usize in_len, DmsInfo& info) {
    using namespace dms_detail;
    info = DmsInfo{};
    info.failed_track = no_track;

    const char magic[4] = {'D', 'M', 'S', '!'};
    for (usize i = 0; i < 4 && i < in_len; ++i) {
        if (in[i] != static_cast<u8>(magic[i])) {
            return DmsError::NotDms;
        }
    }
    if (in_len < header_len) {
        return DmsError::Truncated;
    }
    if (be16(in + 54) != dms_crc16(in + 4, 50)) {
        return DmsError::HeaderCrc;
    }
    info.general_info = be16(in + 10);
    info.first_track = be16(in + 16);
    info.last_track = be16(in + 18);
    info.image_bytes = be32(in + 24);
    info.disk_type = be16(in + 50);
    info.mode = be16(in + 52);
    return DmsError::Ok;
}

namespace dms_detail {

// One track record, its fields as the file states them.
struct TrackRecord {
    u16 number;
    u16 packed_len;
    u16 unpacked_len;
    u8 mode;
    u16 checksum;
    u16 data_crc;
    const u8* data;  // packed_len bytes inside the input
};

inline u16 checksum16(const u8* data, usize length) {
    u16 sum = 0;
    for (usize i = 0; i < length; ++i) {
        sum = static_cast<u16>(sum + data[i]);
    }
    return sum;
}

// Reads the record at in[pos]; on success pos moves past its packed data.
// failed_track is set to the record's number once its header CRC has passed.
inline DmsError read_record(const u8* in, usize in_len, usize& pos, TrackRecord& rec,
                            u32& failed_track) {
    failed_track = no_track;
    if (in_len - pos < record_len) {
        return DmsError::Truncated;
    }
    const u8* h = in + pos;
    if (h[0] != 'T' || h[1] != 'R') {
        return DmsError::TrackHeader;
    }
    if (be16(h + 18) != dms_crc16(h, 18)) {
        return DmsError::TrackHeader;
    }
    rec.number = be16(h + 2);
    failed_track = rec.number;
    rec.packed_len = be16(h + 6);
    rec.unpacked_len = be16(h + 10);
    rec.mode = h[13];
    rec.checksum = be16(h + 14);
    rec.data_crc = be16(h + 16);
    if (in_len - pos - record_len < rec.packed_len) {
        return DmsError::Truncated;
    }
    rec.data = h + record_len;
    pos += record_len + rec.packed_len;
    return DmsError::Ok;
}

// Run-length decoder for SIMPLE tracks. out gets exactly out_len bytes. False
// if the input ends early or a run would overflow out.
inline bool rle_decode(const u8* in, usize in_len, u8* out, usize out_len) {
    usize i = 0;
    usize o = 0;
    while (o < out_len) {
        if (i >= in_len) {
            return false;
        }
        const u8 byte = in[i++];
        if (byte != rle_escape) {
            out[o++] = byte;
            continue;
        }
        if (i >= in_len) {
            return false;
        }
        const u8 count = in[i++];
        if (count == 0) {  // an escape byte that is just itself
            out[o++] = rle_escape;
            continue;
        }
        if (i >= in_len) {
            return false;
        }
        const u8 fill = in[i++];
        usize run = count;
        if (count == 0xFF) {  // long run: a 16 bit count follows
            if (in_len - i < 2) {
                return false;
            }
            run = be16(in + i);
            i += 2;
        }
        if (run > out_len - o) {
            return false;
        }
        __builtin_memset(out + o, fill, run);
        o += run;
    }
    return true;
}

// Decodes one disk track into work.track (unpacked_len bytes).
inline DmsError decode_track(const TrackRecord& rec, DmsWork& work) {
    switch (rec.mode) {
    case 0:  // NOCOMP
        if (rec.packed_len != rec.unpacked_len) {
            return DmsError::Corrupt;
        }
        __builtin_memcpy(work.track, rec.data, rec.packed_len);
        return DmsError::Ok;
    case 1:  // SIMPLE
        return rle_decode(rec.data, rec.packed_len, work.track, rec.unpacked_len)
                   ? DmsError::Ok
                   : DmsError::Corrupt;
    case 6:  // HEAVY2
        return DmsError::UnsupportedMode;
    default:
        return DmsError::UnsupportedMode;
    }
}

inline bool is_disk_track(const TrackRecord& rec) {
    return rec.number < max_disk_track && rec.unpacked_len > min_disk_bytes;
}

}

// Unpacks an archive into out, the raw disk image. Tracks the archive lacks
// are zero-filled. out is only written once the image is known to fit; after
// an error its contents are not to be trusted. work must be zero-filled by
// the caller before its first use.
inline DmsError dms_unpack(const u8* in, usize in_len, u8* out, usize out_cap, DmsWork& work,
                           DmsInfo& info) {
    using namespace dms_detail;
    const DmsError header = dms_read_header(in, in_len, info);
    if (header != DmsError::Ok) {
        return header;
    }
    if ((info.general_info & (info_encrypted | info_dev_encrypted)) != 0) {
        return DmsError::Encrypted;
    }
    if (info.last_track >= max_disk_track || info.first_track > info.last_track) {
        return DmsError::Corrupt;
    }

    // The header states how much packed data follows it. Real archives
    // disagree with the records' own lengths by a few bytes, so this is only
    // a lower bound: a file shorter than that has been cut off, and one with
    // extra bytes at the end is let through.
    if (in_len - header_len < be32(in + 20)) {
        return DmsError::Truncated;
    }

    // Fewer bytes than a record header at the end are not a record, whatever
    // they hold; the check above has already caught a cut-off file.
    u8 seen[max_disk_track] = {};
    usize pos = header_len;
    while (in_len - pos >= record_len) {
        TrackRecord rec{};
        const DmsError read = read_record(in, in_len, pos, rec, info.failed_track);
        if (read != DmsError::Ok) {
            return read;
        }
        if (dms_crc16(rec.data, rec.packed_len) != rec.data_crc) {
            return DmsError::TrackCrc;
        }
        if (!is_disk_track(rec)) {
            continue;
        }

        if (rec.number > info.last_track) {
            return DmsError::TrackHeader;
        }
        if (seen[rec.number] != 0) {
            return DmsError::Corrupt;
        }
        if (info.track_bytes == 0) {
            if (rec.unpacked_len != dd_track_bytes && rec.unpacked_len != hd_track_bytes) {
                return DmsError::BadTrackSize;
            }
            info.track_bytes = rec.unpacked_len;
            info.image_bytes = (info.last_track + 1u) * info.track_bytes;
            if (info.image_bytes > out_cap) {
                return DmsError::OutputTooSmall;
            }
            __builtin_memset(out, 0, info.image_bytes);
        } else if (rec.unpacked_len != info.track_bytes) {
            return DmsError::BadTrackSize;
        }

        const DmsError decoded = decode_track(rec, work);
        if (decoded != DmsError::Ok) {
            return decoded;
        }
        if (checksum16(work.track, rec.unpacked_len) != rec.checksum) {
            return DmsError::TrackChecksum;
        }
        __builtin_memcpy(out + static_cast<usize>(rec.number) * info.track_bytes, work.track,
                         rec.unpacked_len);
        seen[rec.number] = 1;
        ++info.tracks_unpacked;
    }

    info.failed_track = no_track;
    if (info.tracks_unpacked == 0) {
        return DmsError::Truncated;
    }
    return DmsError::Ok;
}

}
