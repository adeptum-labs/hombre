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
// record per track) plus the NOCOMP, SIMPLE and HEAVY2 track modes. The
// result is the raw disk image the archive was made from, byte for byte what
// xDMS 1.3.2 (the format reference) writes for the same archive.

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

namespace dms_detail {

inline constexpr unsigned max_code_bits = 31;  // code lengths are 5 bit fields
inline constexpr u16 heavy_literal_symbols = 510;  // 256 bytes, then match lengths 3 to 256
inline constexpr u16 heavy2_offset_symbols = 15;   // 14 offset classes, then "the last offset"
inline constexpr usize heavy2_window = 8192;

// A canonical prefix code: shorter codes first, equal lengths in symbol
// order, read most significant bit first. A code with no coded symbols stands
// for `single`, sent in zero bits.
template <u16 Symbols>
struct PrefixCode {
    u16 count[max_code_bits + 1];  // number of codes of each length
    u16 symbol[Symbols];           // the coded symbols, ordered by code
    u16 coded;
    u16 single;
};

}

// Decoder state; the caller zero-fills it before the first use. It is large,
// so it does not belong on a small stack. dms_unpack resets the HEAVY2 state
// at the start of every archive.
struct DmsWork {
    u8 track[22528];  // one unpacked HD track

    // HEAVY2 state, which tracks may carry over to the next track.
    u8 window[dms_detail::heavy2_window];  // the last bytes the first stage produced
    u16 window_pos;                        // where the next byte goes, modulo the window
    u16 last_offset;                       // for the "repeat the last offset" symbol
    bool have_codes;                       // a track has sent the two codes
    dms_detail::PrefixCode<dms_detail::heavy_literal_symbols> literals;
    dms_detail::PrefixCode<dms_detail::heavy2_offset_symbols> offsets;
};

namespace dms_detail {

inline constexpr usize header_len = 56;
inline constexpr usize record_len = 20;
inline constexpr u32 no_track = 0xFFFFFFFFu;
inline constexpr u16 max_disk_track = 80;    // numbers from here up are not disk data
inline constexpr u16 min_disk_bytes = 2048;  // tracks up to this long are banners and notes
inline constexpr u16 dd_track_bytes = 11264;
inline constexpr u16 hd_track_bytes = 22528;
// The only general info bit that marks an encrypted archive. 0x40 is set by
// DMS device archives ("DMS_DEV_Fixed") and is not encryption.
inline constexpr u16 info_encrypted = 0x02;
inline constexpr u8 rle_escape = 0x90;

// Track flag bits.
inline constexpr u8 track_keep_state = 0x01;  // clear: reset the window after this track
inline constexpr u8 track_new_codes = 0x02;   // HEAVY: new prefix codes precede the data
inline constexpr u8 track_rle = 0x04;         // HEAVY: the first stage output is run-length coded

static_assert(sizeof(DmsWork::track) == hd_track_bytes);
static_assert(sizeof(DmsWork) <= 32768, "DmsWork is meant to fit in eight 4 KiB pages");

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
    u16 stage_len;  // HEAVY: bytes out of the first stage, before run-length decoding
    u16 unpacked_len;
    u8 flags;
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
    rec.stage_len = be16(h + 8);
    rec.unpacked_len = be16(h + 10);
    rec.flags = h[12];
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

// Run-length decoder fed one byte at a time, so it can sit behind the HEAVY
// first stage without a buffer between them. The coding: 0x90 0x00 is the
// byte 0x90 itself; 0x90 n b repeats b n times; 0x90 0xFF b hi lo repeats b
// (hi << 8 | lo) times; any other byte is itself. Once `out` is full, further
// input is ignored.
struct RleSink {
    enum State : u8 { Literal, Count, Fill, RunHigh, RunLow };

    u8* out;
    usize length;
    usize at = 0;
    State state = Literal;
    u8 count = 0;
    u8 fill = 0;
    u16 run = 0;
    bool failed = false;  // a run would overflow out

    bool done() const { return at == length; }

    void put(u8 byte) {
        if (failed || done()) {
            return;
        }
        switch (state) {
        case Literal:
            if (byte == rle_escape) {
                state = Count;
            } else {
                out[at++] = byte;
            }
            return;
        case Count:
            count = byte;
            if (count == 0) {  // an escape byte that is just itself
                out[at++] = rle_escape;
                state = Literal;
            } else {
                state = Fill;
            }
            return;
        case Fill:
            fill = byte;
            if (count == 0xFF) {  // long run: a 16 bit count follows
                state = RunHigh;
            } else {
                emit(count);
            }
            return;
        case RunHigh:
            run = static_cast<u16>(byte << 8);
            state = RunLow;
            return;
        case RunLow:
            emit(static_cast<u16>(run | byte));
            return;
        }
    }

    void emit(usize n) {
        state = Literal;
        if (n > length - at) {
            failed = true;
            return;
        }
        __builtin_memset(out + at, fill, n);
        at += n;
    }
};

// Run-length decoder for SIMPLE tracks. out gets exactly out_len bytes. False
// if the input ends early or a run would overflow out.
inline bool rle_decode(const u8* in, usize in_len, u8* out, usize out_len) {
    RleSink sink{out, out_len};
    for (usize i = 0; i < in_len && !sink.done() && !sink.failed; ++i) {
        sink.put(in[i]);
    }
    return sink.done() && !sink.failed;
}

// Reads a track's packed data most significant bit first. Reading past the
// end yields zero bits and sets `overrun`.
struct BitReader {
    const u8* data;
    usize bit_count;
    usize pos = 0;
    bool overrun = false;

    u32 bit() {
        if (pos >= bit_count) {
            overrun = true;
            return 0;
        }
        const u32 b = (data[pos >> 3] >> (7 - (pos & 7))) & 1u;
        ++pos;
        return b;
    }

    u32 bits(unsigned n) {
        u32 value = 0;
        while (n-- > 0) {
            value = value << 1 | bit();
        }
        return value;
    }
};

// Reads a prefix code as HEAVY tracks send it: a count n in count_bits bits,
// then the code lengths of symbols 0 to n-1 in length_bits bits each (0: no
// code). n = 0 instead sends a single symbol in count_bits bits, coded in
// zero bits. False unless the lengths make a complete code over at most
// Symbols symbols.
template <u16 Symbols>
inline bool read_code(BitReader& in, PrefixCode<Symbols>& code, unsigned count_bits,
                      unsigned length_bits) {
    const u32 n = in.bits(count_bits);
    if (n == 0) {
        const u32 single = in.bits(count_bits);
        if (single >= Symbols) {
            return false;
        }
        code.coded = 0;
        code.single = static_cast<u16>(single);
        return !in.overrun;
    }
    if (n > Symbols) {
        return false;
    }

    u8 length[Symbols];
    for (u16 i = 0; i < max_code_bits + 1; ++i) {
        code.count[i] = 0;
    }
    for (u32 s = 0; s < n; ++s) {
        length[s] = static_cast<u8>(in.bits(length_bits));
        ++code.count[length[s]];
    }
    if (in.overrun) {
        return false;
    }
    code.count[0] = 0;

    // The lengths must fill the code space exactly: no code is a prefix of
    // another and every bit string decodes.
    i64 left = 1;
    for (unsigned len = 1; len <= max_code_bits; ++len) {
        left = left * 2 - code.count[len];
        if (left < 0) {
            return false;
        }
    }
    if (left != 0) {
        return false;
    }

    u16 next[max_code_bits + 1];
    next[1] = 0;
    for (unsigned len = 1; len < max_code_bits; ++len) {
        next[len + 1] = static_cast<u16>(next[len] + code.count[len]);
    }
    code.coded = 0;
    for (u32 s = 0; s < n; ++s) {
        if (length[s] != 0) {
            code.symbol[next[length[s]]++] = static_cast<u16>(s);
            ++code.coded;
        }
    }
    return true;
}

// Reads one symbol of a code that read_code accepted.
template <u16 Symbols>
inline u16 read_symbol(BitReader& in, const PrefixCode<Symbols>& code) {
    if (code.coded == 0) {
        return code.single;
    }
    u32 value = 0;  // the bits read so far
    u32 first = 0;  // the first code of the current length
    u32 index = 0;  // the position of that code in symbol[]
    for (unsigned len = 1; len <= max_code_bits; ++len) {
        value |= in.bit();
        const u32 count = code.count[len];
        if (value - first < count) {
            return code.symbol[index + (value - first)];
        }
        index += count;
        first = (first + count) << 1;
        value <<= 1;
    }
    return code.symbol[0];  // not reached: the code is complete
}

// Window distance of a match: an offset class, then for class c > 0 the c-1
// low bits below an implied top bit. The last class repeats the previous
// distance, which survives from track to track.
inline u16 read_offset(BitReader& in, DmsWork& work) {
    const u16 cls = read_symbol(in, work.offsets);
    if (cls == heavy2_offset_symbols - 1) {
        return work.last_offset;
    }
    if (cls == 0) {
        work.last_offset = 0;
    } else {
        work.last_offset = static_cast<u16>(in.bits(cls - 1u) | 1u << (cls - 1u));
    }
    return work.last_offset;
}

inline void reset_window(DmsWork& work) {
    __builtin_memset(work.window, 0, sizeof(work.window));
    work.window_pos = 0;
}

// The state a fresh archive starts from.
inline void reset_decoder(DmsWork& work) {
    reset_window(work);
    work.last_offset = 0;
    work.have_codes = false;
}

// HEAVY2: LZ77 over an 8 KiB window with prefix-coded literals, match
// lengths and match offsets. The first stage yields stage_len bytes; when the
// track says so they are run-length decoded to the track bytes. Either way
// the stage output counts as followed by zero bytes up to unpacked_len: some
// archives leave the trailing zeros of a track out of the first stage.
inline DmsError decode_heavy2(const TrackRecord& rec, DmsWork& work) {
    BitReader in{rec.data, usize{rec.packed_len} * 8};
    if ((rec.flags & track_new_codes) != 0) {
        if (!read_code(in, work.literals, 9, 5) || !read_code(in, work.offsets, 5, 4)) {
            return DmsError::Corrupt;
        }
        work.have_codes = true;
    }
    if (!work.have_codes) {
        return DmsError::Corrupt;
    }
    const bool rle = (rec.flags & track_rle) != 0;
    if (!rle) {
        __builtin_memset(work.track, 0, rec.unpacked_len);
    }

    constexpr u16 mask = heavy2_window - 1;
    RleSink sink{work.track, rec.unpacked_len};
    usize produced = 0;
    auto put = [&](u8 byte) {
        work.window[work.window_pos++ & mask] = byte;
        if (rle) {
            sink.put(byte);
        } else if (produced < rec.unpacked_len) {
            work.track[produced] = byte;
        }
        ++produced;
    };

    while (produced < rec.stage_len) {
        const u16 symbol = read_symbol(in, work.literals);
        if (symbol < 256) {
            put(static_cast<u8>(symbol));
        } else {
            const usize length = symbol - 253u;
            if (length > rec.stage_len - produced) {
                return DmsError::Corrupt;
            }
            u16 from = static_cast<u16>(work.window_pos - read_offset(in, work) - 1u);
            for (usize i = 0; i < length; ++i) {
                put(work.window[from++ & mask]);
            }
        }
        if (in.overrun || sink.failed) {
            return DmsError::Corrupt;
        }
    }
    if (rle) {
        for (; produced < rec.unpacked_len && !sink.done() && !sink.failed; ++produced) {
            sink.put(0);
        }
        if (!sink.done() || sink.failed) {
            return DmsError::Corrupt;
        }
    }
    return DmsError::Ok;
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
        return decode_heavy2(rec, work);
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
    if ((info.general_info & info_encrypted) != 0) {
        return DmsError::Encrypted;
    }
    if (info.last_track >= max_disk_track || info.first_track > info.last_track) {
        return DmsError::Corrupt;
    }

    // The header states how much packed data follows it. Real archives
    // disagree with the records' own lengths by a few bytes, so this is only
    // a lower bound: a file shorter than that has been cut off, and one with
    // extra bytes at the end is let through.
    const u32 stated_packed = be32(in + 20);
    if (in_len - header_len < stated_packed) {
        return DmsError::Truncated;
    }

    // Fewer bytes than a record header at the end are not a record, whatever
    // they hold; the check above has already caught a cut-off file. Some
    // archives carry text or padding after their tracks: bytes without the
    // "TR" mark end the archive once the records hold as much packed data as
    // the header states, and are a damaged record before that.
    reset_decoder(work);
    u8 seen[max_disk_track] = {};
    u64 packed_seen = 0;
    usize pos = header_len;
    while (in_len - pos >= record_len) {
        if ((in[pos] != 'T' || in[pos + 1] != 'R') && packed_seen >= stated_packed) {
            break;
        }
        TrackRecord rec{};
        const DmsError read = read_record(in, in_len, pos, rec, info.failed_track);
        if (read != DmsError::Ok) {
            return read;
        }
        packed_seen += rec.packed_len;
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

        // Banners and other skipped records leave the decoder state alone;
        // every disk track, whatever its mode, may reset the window.
        const DmsError decoded = decode_track(rec, work);
        if (decoded != DmsError::Ok) {
            return decoded;
        }
        if ((rec.flags & track_keep_state) == 0) {
            reset_window(work);
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
