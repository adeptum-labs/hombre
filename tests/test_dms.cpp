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
#include "dms.hpp"
#include "dms_writer.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

using namespace hombre;

namespace {

constexpr u16 kDd = 11264;
constexpr usize kRecord = 20;
constexpr usize kHeader = 56;

struct Prng {
    u32 state = 0x2545F491u;
    u8 next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<u8>(state >> 11);
    }
};

// Five tracks that exercise every branch of the run-length coder: an empty
// track, noise sprinkled with the escape byte, runs of every length up to
// past the 8-bit count limit, and a lone escape byte at the very end.
std::vector<u8> make_adf(u16 track_bytes = kDd) {
    std::vector<u8> adf;
    Prng rng;
    for (usize track = 0; track < 5; ++track) {
        std::vector<u8> t;
        switch (track) {
        case 0:
            t.assign(track_bytes, 0);
            break;
        case 1:
            for (usize i = 0; i < track_bytes; ++i) {
                u8 b = rng.next();
                t.push_back(i % 17 == 0 ? u8{0x90} : b);
            }
            break;
        case 2: {
            u8 value = 0x90;
            for (usize run = 1; t.size() < track_bytes; run = run % 300 + 1) {
                for (usize i = 0; i < run && t.size() < track_bytes; ++i) {
                    t.push_back(value);
                }
                value = (run % 3 == 0) ? u8{0x90} : static_cast<u8>(rng.next() | 1);
            }
            break;
        }
        case 3:
            for (usize i = 0; i < track_bytes; ++i) {
                t.push_back((i / 700) % 2 == 0 ? u8{0} : static_cast<u8>('A' + i % 26));
            }
            break;
        default:
            for (usize i = 0; i < track_bytes; ++i) {
                t.push_back(i < 3000 ? u8{0x90} : rng.next());
            }
            t.back() = 0x90;
            break;
        }
        adf.insert(adf.end(), t.begin(), t.end());
    }
    return adf;
}

struct Result {
    DmsError error;
    DmsInfo info;
    std::vector<u8> out;
};

Result run(const std::vector<u8>& dms, usize out_cap = 80u * 22528u) {
    Result r{};
    r.out.assign(out_cap, 0xEE);
    auto work = std::make_unique<DmsWork>();
    // Exactly sized copy, so the sanitizers see any read past the end.
    std::vector<u8> in(dms);
    r.error = dms_unpack(in.data(), in.size(), r.out.data(), r.out.size(), *work, r.info);
    return r;
}

u16 get16(const std::vector<u8>& v, usize at) { return static_cast<u16>(v[at] << 8 | v[at + 1]); }

void put16(std::vector<u8>& v, usize at, u16 x) {
    v[at] = static_cast<u8>(x >> 8);
    v[at + 1] = static_cast<u8>(x);
}

void refresh_header_crc(std::vector<u8>& v) {
    put16(v, 54, dms_crc16(v.data() + 4, 50));
}

// After records are inserted or removed: states the packed size the records
// now add up to, and signs the header again.
void fix_packed_size(std::vector<u8>& v) {
    u32 total = 0;
    for (usize at = kHeader; at + kRecord <= v.size(); at += kRecord + get16(v, at + 6)) {
        total += get16(v, at + 6);
    }
    put16(v, 20, static_cast<u16>(total >> 16));
    put16(v, 22, static_cast<u16>(total));
    refresh_header_crc(v);
}

void refresh_track_crc(std::vector<u8>& v, usize at) {
    put16(v, at + 18, dms_crc16(v.data() + at, 18));
}

usize nocomp_track_at(usize n) { return kHeader + n * (kRecord + kDd); }

bool same_prefix(const std::vector<u8>& out, const std::vector<u8>& adf) {
    return out.size() >= adf.size() && std::equal(adf.begin(), adf.end(), out.begin());
}

void crc16_known_vector() {
    CHECK_EQ(dms_crc16(reinterpret_cast<const u8*>("123456789"), 9), 0xBB3D);
    CHECK_EQ(dms_crc16(nullptr, 0), 0);
}

void names() {
    CHECK(dms_mode_name(0)[0] == 'N');
    const char* expected[] = {"NOCOMP", "SIMPLE", "QUICK", "MEDIUM", "DEEP", "HEAVY1", "HEAVY2"};
    for (u16 m = 0; m < 7; ++m) {
        const char* a = dms_mode_name(m);
        const char* b = expected[m];
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        CHECK_EQ(*a, *b);
    }
    CHECK(dms_mode_name(7)[0] == '?');
    CHECK(dms_error_name(DmsError::Ok)[0] == 'O');
    CHECK(dms_error_name(DmsError::Corrupt)[0] == 'C');
    CHECK(dms_error_name(static_cast<DmsError>(99))[0] == '?');
}

void nocomp_roundtrip() {
    const auto adf = make_adf();
    const auto dms = build_dms(adf, 0, kDd);
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK(same_prefix(r.out, adf));
    CHECK_EQ(r.info.mode, 0);
    CHECK_EQ(r.info.first_track, 0);
    CHECK_EQ(r.info.last_track, 4);
    CHECK_EQ(r.info.track_bytes, kDd);
    CHECK_EQ(r.info.image_bytes, 5u * kDd);
    CHECK_EQ(r.info.tracks_unpacked, 5u);
    // Nothing past the image is written.
    CHECK_EQ(r.out[adf.size()], 0xEE);
}

void high_density_roundtrip() {
    const auto adf = make_adf(22528);
    for (u16 mode : {u16{0}, u16{1}}) {
        const Result r = run(build_dms(adf, mode, 22528));
        CHECK_EQ(r.error, DmsError::Ok);
        CHECK(same_prefix(r.out, adf));
        CHECK_EQ(r.info.track_bytes, 22528u);
        CHECK_EQ(r.info.image_bytes, 5u * 22528u);
    }
}

void simple_roundtrip() {
    const auto adf = make_adf();
    const auto dms = build_dms(adf, 1, kDd);
    CHECK(dms.size() < build_dms(adf, 0, kDd).size());
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK(same_prefix(r.out, adf));
    CHECK_EQ(r.info.mode, 1);
    CHECK_EQ(r.info.image_bytes, 5u * kDd);
    CHECK_EQ(r.info.tracks_unpacked, 5u);
}

void missing_track_zero_filled() {
    const auto adf = make_adf();
    auto dms = build_dms(adf, 0, kDd);
    dms.erase(dms.begin() + nocomp_track_at(2), dms.begin() + nocomp_track_at(3));
    fix_packed_size(dms);
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK_EQ(r.info.tracks_unpacked, 4u);
    CHECK_EQ(r.info.image_bytes, 5u * kDd);
    for (usize i = 0; i < 5u * kDd; ++i) {
        const bool missing = i >= 2u * kDd && i < 3u * kDd;
        CHECK_EQ(r.out[i], missing ? u8{0} : adf[i]);
    }
}

void missing_leading_and_trailing_tracks_zero_filled() {
    const auto adf = make_adf();
    auto dms = build_dms(adf, 0, kDd);
    // Last track gone too: the header still says the image has five.
    dms.resize(nocomp_track_at(4));
    dms.erase(dms.begin() + nocomp_track_at(0), dms.begin() + nocomp_track_at(1));
    fix_packed_size(dms);
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK_EQ(r.info.tracks_unpacked, 3u);
    for (usize i = 0; i < 5u * kDd; ++i) {
        const bool present = i >= 1u * kDd && i < 4u * kDd;
        CHECK_EQ(r.out[i], present ? adf[i] : u8{0});
    }
}

void banner_track_skipped() {
    const auto adf = make_adf();
    auto dms = build_dms(adf, 0, kDd);

    std::vector<u8> banner(1024);
    for (usize i = 0; i < banner.size(); ++i) {
        banner[i] = static_cast<u8>('a' + i % 26);
    }
    const auto front = dms_track_record(0, 0, banner.data(), banner.size());
    dms.insert(dms.begin() + kHeader, front.begin(), front.end());

    // A record numbered 80 holds no disk data, even at full track size.
    std::vector<u8> extra(kDd, 0x55);
    const auto back = dms_track_record(80, 0, extra.data(), extra.size());
    dms.insert(dms.end(), back.begin(), back.end());
    fix_packed_size(dms);

    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK(same_prefix(r.out, adf));
    CHECK_EQ(r.info.tracks_unpacked, 5u);
    CHECK_EQ(r.out[5u * kDd], 0xEE);
}

void skipped_track_with_unsupported_mode_is_ignored() {
    const auto adf = make_adf();
    auto dms = build_dms(adf, 0, kDd);
    std::vector<u8> banner(1024, 'x');
    auto front = dms_track_record(0, 0, banner.data(), banner.size());
    front[13] = 6;  // a compressed banner we never decode
    put16(front, 18, dms_crc16(front.data(), 18));
    dms.insert(dms.begin() + kHeader, front.begin(), front.end());
    fix_packed_size(dms);
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK(same_prefix(r.out, adf));
}

void not_dms() {
    std::vector<u8> junk(200, 0x41);
    CHECK_EQ(run(junk).error, DmsError::NotDms);
    auto dms = build_dms(make_adf(), 0, kDd);
    dms[3] = '?';
    CHECK_EQ(run(dms).error, DmsError::NotDms);
}

void header_crc() {
    const auto good = build_dms(make_adf(), 0, kDd);
    for (usize at : {usize{5}, usize{11}, usize{19}, usize{50}, usize{53}, usize{54}, usize{55}}) {
        auto dms = good;
        dms[at] ^= 0x01;
        CHECK_EQ(run(dms).error, DmsError::HeaderCrc);
    }
}

void truncated_everywhere_in_the_front() {
    const auto dms = build_dms(make_adf(), 0, kDd);
    for (usize n = 0; n <= 200; ++n) {
        std::vector<u8> cut(dms.begin(), dms.begin() + n);
        const Result r = run(cut);
        CHECK_EQ(r.error, DmsError::Truncated);
    }
}

void truncated_mid_track() {
    for (u16 mode : {u16{0}, u16{1}}) {
        const auto dms = build_dms(make_adf(), mode, kDd);
        const usize last = dms.size();
        for (usize n : {last - 1, last - 100, last - 5000}) {
            std::vector<u8> cut(dms.begin(), dms.begin() + n);
            const Result r = run(cut);
            CHECK_EQ(r.error, DmsError::Truncated);
            // The header size check catches a cut that removes more than the
            // record headers' worth of slack; a smaller one the walk finds.
            CHECK_EQ(r.info.failed_track, n == last - 5000 ? 0xFFFFFFFFu : 4u);
        }

        // The header may understate the packed size; then the walk itself
        // finds the cut and names the track.
        auto understated = dms;
        put16(understated, 20, 0);
        put16(understated, 22, 0);
        refresh_header_crc(understated);
        for (usize n : {last - 1, last - 100, last - 5000}) {
            std::vector<u8> cut(understated.begin(), understated.begin() + n);
            const Result r = run(cut);
            CHECK_EQ(r.error, DmsError::Truncated);
            CHECK_EQ(r.info.failed_track, 4u);
        }
    }
}

void cut_at_a_track_boundary_is_truncated() {
    const auto adf = make_adf();
    for (u16 mode : {u16{0}, u16{1}}) {
        auto dms = build_dms(adf, mode, kDd);
        usize end = kHeader;
        for (int i = 0; i < 3; ++i) {
            end += kRecord + get16(dms, end + 6);
        }
        dms.resize(end);  // three whole records, the header still promises five
        CHECK_EQ(run(dms).error, DmsError::Truncated);
        dms.resize(end + 7);  // and no better with part of a header after them
        CHECK_EQ(run(dms).error, DmsError::Truncated);
    }
}

void header_packed_size_matches_the_records() {
    for (u16 mode : {u16{0}, u16{1}}) {
        const auto dms = build_dms(make_adf(), mode, kDd);
        u32 sum = 0;
        for (usize at = kHeader; at < dms.size(); at += kRecord + get16(dms, at + 6)) {
            sum += get16(dms, at + 6);
        }
        CHECK_EQ(static_cast<u32>(get16(dms, 20)) << 16 | get16(dms, 22), sum);
        CHECK_EQ(dms.size() - kHeader, sum + 5u * kRecord);
    }
    // Stored tracks pack to their own size.
    const auto stored = build_dms(make_adf(), 0, kDd);
    CHECK_EQ(get16(stored, 22), 5u * kDd);
    CHECK_EQ(get16(stored, 20), 0);
}

void bytes_after_the_last_record_are_tolerated() {
    const auto adf = make_adf();
    for (u16 mode : {u16{0}, u16{1}}) {
        for (usize extra : {usize{1}, usize{7}, usize{19}}) {
            auto dms = build_dms(adf, mode, kDd);
            dms.insert(dms.end(), extra, 0x5A);
            const Result r = run(dms);
            CHECK_EQ(r.error, DmsError::Ok);
            CHECK(same_prefix(r.out, adf));
            CHECK_EQ(r.info.tracks_unpacked, 5u);
        }
    }
}

// Text or padding after the last record ends the archive once the records
// hold the packed size the header states; before that it is a bad record.
void unmarked_bytes_after_the_records_end_the_archive() {
    const auto adf = make_adf();
    for (u16 mode : {u16{0}, u16{1}}) {
        for (usize extra : {usize{20}, usize{21}, usize{5000}}) {
            for (u8 fill : {u8{0x00}, u8{'D'}}) {
                auto dms = build_dms(adf, mode, kDd);
                dms.insert(dms.end(), extra, fill);
                const Result r = run(dms);
                CHECK_EQ(r.error, DmsError::Ok);
                CHECK(same_prefix(r.out, adf));
                CHECK_EQ(r.info.tracks_unpacked, 5u);
            }
        }
    }

    // The header promises one byte more than the records hold.
    auto dms = build_dms(adf, 0, kDd);
    put16(dms, 22, static_cast<u16>(get16(dms, 22) + 1));
    refresh_header_crc(dms);
    dms.insert(dms.end(), 64, 0x00);
    CHECK_EQ(run(dms).error, DmsError::TrackHeader);

    // A record that claims to be one still has to be valid.
    dms = build_dms(adf, 0, kDd);
    const u8 junk[kRecord] = {'T', 'R'};
    dms.insert(dms.end(), junk, junk + kRecord);
    CHECK_EQ(run(dms).error, DmsError::TrackHeader);
}

void duplicate_track_is_corrupt() {
    auto dms = build_dms(make_adf(), 0, kDd);
    const auto again(std::vector<u8>(dms.begin() + nocomp_track_at(1),
                                     dms.begin() + nocomp_track_at(2)));
    dms.insert(dms.end(), again.begin(), again.end());
    fix_packed_size(dms);
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Corrupt);
    CHECK_EQ(r.info.failed_track, 1u);
}

void track_checksum_is_the_16_bit_byte_sum() {
    // 300 * 255 = 76500 = 0x12AD4, kept to 16 bits.
    const std::vector<u8> ff(300, 0xFF);
    CHECK_EQ(get16(dms_track_record(0, 0, ff.data(), ff.size()), 14), 0x2AD4);
    const u8 small[] = {1, 2, 3, 250};
    CHECK_EQ(get16(dms_track_record(0, 0, small, sizeof(small)), 14), 256);
}

void track_header_errors() {
    const auto good = build_dms(make_adf(), 0, kDd);
    auto bad_magic = good;
    bad_magic[nocomp_track_at(2)] = 'X';
    refresh_track_crc(bad_magic, nocomp_track_at(2));
    Result r = run(bad_magic);
    CHECK_EQ(r.error, DmsError::TrackHeader);

    auto bad_crc = good;
    bad_crc[nocomp_track_at(1) + 7] ^= 0x01;  // header CRC left stale
    r = run(bad_crc);
    CHECK_EQ(r.error, DmsError::TrackHeader);
    CHECK_EQ(r.info.failed_track, 0xFFFFFFFFu);  // a bad header names no track
}

void track_data_crc() {
    for (u16 mode : {u16{0}, u16{1}}) {
        auto dms = build_dms(make_adf(), mode, kDd);
        // Find track 3 by walking the packed lengths.
        usize at = kHeader;
        for (int i = 0; i < 3; ++i) {
            at += kRecord + get16(dms, at + 6);
        }
        dms[at + kRecord + 1] ^= 0x04;
        const Result r = run(dms);
        CHECK_EQ(r.error, DmsError::TrackCrc);
        CHECK_EQ(r.info.failed_track, 3u);
    }
}

void track_checksum() {
    auto dms = build_dms(make_adf(), 0, kDd);
    const usize at = nocomp_track_at(2);
    put16(dms, at + 14, static_cast<u16>(get16(dms, at + 14) + 1));
    refresh_track_crc(dms, at);
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::TrackChecksum);
    CHECK_EQ(r.info.failed_track, 2u);
}

void unsupported_mode() {
    auto dms = build_dms(make_adf(), 0, kDd);
    const usize at = nocomp_track_at(1);
    for (u8 mode : {u8{2}, u8{3}, u8{4}, u8{5}, u8{7}, u8{9}, u8{255}}) {
        dms[at + 13] = mode;
        refresh_track_crc(dms, at);
        const Result r = run(dms);
        CHECK_EQ(r.error, DmsError::UnsupportedMode);
        CHECK_EQ(r.info.failed_track, 1u);
    }
}

void encrypted() {
    auto dms = build_dms(make_adf(), 0, kDd);
    dms[11] |= 0x02;
    refresh_header_crc(dms);
    CHECK_EQ(run(dms).error, DmsError::Encrypted);
    // 0x40 marks a DMS device archive, not encryption.
    dms[11] &= static_cast<u8>(~0x02);
    dms[11] |= 0x40;
    refresh_header_crc(dms);
    CHECK_EQ(run(dms).error, DmsError::Ok);
}

void bad_track_size() {
    auto dms = build_dms(make_adf(), 0, kDd);
    // Track 2 claims a different size from track 0.
    const usize at = nocomp_track_at(2);
    put16(dms, at + 10, 11000);
    refresh_track_crc(dms, at);
    Result r = run(dms);
    CHECK_EQ(r.error, DmsError::BadTrackSize);
    CHECK_EQ(r.info.failed_track, 2u);

    // The first disk track fixes the size, and only DD or HD are known.
    auto odd = build_dms(make_adf(), 0, kDd);
    put16(odd, nocomp_track_at(0) + 10, 4096);
    refresh_track_crc(odd, nocomp_track_at(0));
    CHECK_EQ(run(odd).error, DmsError::BadTrackSize);
}

// A one-track SIMPLE archive around hand-made packed bytes.
std::vector<u8> raw_simple_archive(const std::vector<u8>& packed) {
    std::vector<u8> rec(kRecord, 0);
    rec[0] = 'T';
    rec[1] = 'R';
    put16(rec, 6, static_cast<u16>(packed.size()));
    put16(rec, 8, static_cast<u16>(packed.size()));
    put16(rec, 10, kDd);
    rec[13] = 1;
    put16(rec, 16, dms_crc16(packed.data(), packed.size()));
    refresh_track_crc(rec, 0);
    rec.insert(rec.end(), packed.begin(), packed.end());
    auto dms = dms_header(1, 0, 0, static_cast<u32>(packed.size()), kDd);
    dms.insert(dms.end(), rec.begin(), rec.end());
    return dms;
}

void damaged_run_length_data() {
    // A run of 65535 zero bytes overruns the 11264 byte track.
    CHECK_EQ(run(raw_simple_archive({0x90, 0xFF, 0x00, 0xFF, 0xFF})).error, DmsError::Corrupt);
    // The packed data ends before the track is complete.
    CHECK_EQ(run(raw_simple_archive({0x41, 0x42, 0x43})).error, DmsError::Corrupt);
    // An escape with its count or fill byte missing.
    CHECK_EQ(run(raw_simple_archive({0x90})).error, DmsError::Corrupt);
    CHECK_EQ(run(raw_simple_archive({0x90, 0x05})).error, DmsError::Corrupt);
    CHECK_EQ(run(raw_simple_archive({0x90, 0xFF, 0x00, 0x01})).error, DmsError::Corrupt);
    // A valid single run fills the track exactly; the checksum of zeros is 0.
    CHECK_EQ(run(raw_simple_archive({0x90, 0xFF, 0x00, 0x2C, 0x00})).error, DmsError::Ok);
}

// HEAVY2 streams are written by hand: there is no encoder here. Codes are
// given as bit strings, most significant bit first.
class BitWriter {
public:
    void put(u32 value, unsigned bits) {
        while (bits-- > 0) {
            if (used_ % 8 == 0) {
                bytes_.push_back(0);
            }
            if ((value >> bits & 1u) != 0) {
                bytes_.back() |= static_cast<u8>(0x80u >> (used_ % 8));
            }
            ++used_;
        }
    }

    void code(const char* bits) {
        for (; *bits != '\0'; ++bits) {
            put(*bits == '1' ? 1 : 0, 1);
        }
    }

    const std::vector<u8>& bytes() const { return bytes_; }

private:
    std::vector<u8> bytes_;
    usize used_ = 0;
};

constexpr u8 kKeep = 0x01;
constexpr u8 kNewCodes = 0x02;
constexpr u8 kRle = 0x04;

std::vector<u8> heavy_record(u16 number, u8 flags, usize stage_len, const std::vector<u8>& packed,
                             const std::vector<u8>& track) {
    std::vector<u8> rec(kRecord, 0);
    rec[0] = 'T';
    rec[1] = 'R';
    put16(rec, 2, number);
    put16(rec, 6, static_cast<u16>(packed.size()));
    put16(rec, 8, static_cast<u16>(stage_len));
    put16(rec, 10, static_cast<u16>(track.size()));
    rec[12] = flags;
    rec[13] = 6;
    put16(rec, 14, dms_detail::checksum16(track.data(), track.size()));
    put16(rec, 16, dms_crc16(packed.data(), packed.size()));
    refresh_track_crc(rec, 0);
    rec.insert(rec.end(), packed.begin(), packed.end());
    return rec;
}

std::vector<u8> heavy_archive(const std::vector<std::vector<u8>>& records) {
    std::vector<u8> body;
    u32 packed = 0;
    for (const auto& rec : records) {
        packed += get16(rec, 6);
        body.insert(body.end(), rec.begin(), rec.end());
    }
    const u16 last = static_cast<u16>(records.size() - 1);
    auto dms = dms_header(6, 0, last, packed, (last + 1u) * kDd);
    dms.insert(dms.end(), body.begin(), body.end());
    return dms;
}

// The codes of the hand-packed tracks.
//   literals: 'a' 00, 0x2B 010, 'b' 011, 0x90 100, 0xF3 101, 0xFF 110,
//             257 (a match of 4) 111
//   offsets:  class 0 (distance 0) 0, class 2 (distance 2 or 3) 10,
//             class 14 (the last distance) 11
void put_codes(BitWriter& w) {
    w.put(258, 9);
    for (u32 s = 0; s < 258; ++s) {
        const bool three = s == 0x2B || s == 'b' || s == 0x90 || s == 0xF3 || s == 0xFF || s == 257;
        w.put(s == 'a' ? 2 : three ? 3 : 0, 5);
    }
    w.put(15, 5);
    for (u32 s = 0; s < 15; ++s) {
        w.put(s == 0 ? 1 : (s == 2 || s == 14) ? 2 : 0, 4);
    }
}

// The run that ends both hand-packed tracks: 0x90 0xFF 'a' 0x2B 0xF3 is
// 11251 bytes of 'a'.
void put_long_run_of_a(BitWriter& w) { w.code("100" "110" "00" "010" "101"); }

// "abaab", a match of 4 at distance 2, a match of 4 at the last distance,
// then the long run: 18 bytes out of the first stage.
std::vector<u8> first_track_bits() {
    BitWriter w;
    put_codes(w);
    w.code("00" "011" "00" "00" "011");
    w.code("111" "10" "0");  // class 2 with low bit 0: distance 2
    w.code("111" "11");
    put_long_run_of_a(w);
    return w.bytes();
}

std::vector<u8> first_track() {
    std::vector<u8> t;
    for (char c : std::string_view("abaabaabaabaa")) {
        t.push_back(static_cast<u8>(c));
    }
    t.resize(kDd, 'a');
    return t;
}

// No codes of its own: a match of 4 at the last distance, two matches of 4
// at distance 0, 'b', then the long run. 18 bytes out of the first stage.
std::vector<u8> second_track_bits() {
    BitWriter w;
    w.code("111" "11" "111" "0" "111" "0" "011");
    put_long_run_of_a(w);
    return w.bytes();
}

// The second track as decoded after the first: with the window kept, the
// first match copies the first track's last three stage bytes (a 0x2B 0xF3)
// and then its own first byte; with it reset, everything before 'b' is zero.
std::vector<u8> second_track(bool kept) {
    std::vector<u8> t;
    if (kept) {
        t = {'a', 0x2B, 0xF3, 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a'};
    } else {
        t.assign(12, 0);
    }
    t.push_back('b');
    t.resize(kDd, 'a');
    return t;
}

void heavy2_hand_packed_track() {
    const auto track = first_track();
    const auto dms =
        heavy_archive({heavy_record(0, kNewCodes | kRle | kKeep, 18, first_track_bits(), track)});
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK(same_prefix(r.out, track));
    CHECK_EQ(r.info.mode, 6u);
    CHECK_EQ(r.info.image_bytes, u32{kDd});
}

// Flag bit 0 keeps the window for the next track, a clear bit 0 resets it.
// The codes and the last distance survive either way.
void heavy2_track_flags() {
    for (bool kept : {true, false}) {
        const u8 keep = kept ? kKeep : 0;
        const auto dms = heavy_archive({
            heavy_record(0, kNewCodes | kRle | keep, 18, first_track_bits(), first_track()),
            heavy_record(1, kRle | kKeep, 18, second_track_bits(), second_track(kept)),
        });
        const Result r = run(dms);
        CHECK_EQ(r.error, DmsError::Ok);
        auto image = first_track();
        const auto second = second_track(kept);
        image.insert(image.end(), second.begin(), second.end());
        CHECK(same_prefix(r.out, image));
    }

    // A NOCOMP track in between resets the window unless it keeps it too.
    for (bool kept : {true, false}) {
        const std::vector<u8> plain(kDd, 0x11);
        auto middle = dms_track_record(1, 0, plain.data(), plain.size());
        middle[12] = kept ? kKeep : 0;
        refresh_track_crc(middle, 0);
        const auto dms = heavy_archive({
            heavy_record(0, kNewCodes | kRle | kKeep, 18, first_track_bits(), first_track()),
            middle,
            heavy_record(2, kRle | kKeep, 18, second_track_bits(), second_track(kept)),
        });
        const Result r = run(dms);
        CHECK_EQ(r.error, DmsError::Ok);
        CHECK(std::equal(plain.begin(), plain.end(), r.out.begin() + kDd));
        const auto second = second_track(kept);
        CHECK(std::equal(second.begin(), second.end(), r.out.begin() + 2 * kDd));
    }

    // A second archive in the same work area starts from a clean state.
    auto work = std::make_unique<DmsWork>();
    std::vector<u8> out(2u * kDd);
    DmsInfo info{};
    const auto first = heavy_archive(
        {heavy_record(0, kNewCodes | kRle | kKeep, 18, first_track_bits(), first_track())});
    CHECK_EQ(dms_unpack(first.data(), first.size(), out.data(), out.size(), *work, info),
             DmsError::Ok);
    const auto alone = heavy_archive({heavy_record(0, kRle | kKeep, 18, second_track_bits(),
                                                   second_track(false))});
    CHECK_EQ(dms_unpack(alone.data(), alone.size(), out.data(), out.size(), *work, info),
             DmsError::Corrupt);  // no codes yet

    // "abaab", then a match at the last distance, which is 0 in a fresh
    // archive (2 is left over from the first), then a match at distance 0.
    BitWriter w;
    put_codes(w);
    w.code("00" "011" "00" "00" "011" "111" "11" "111" "0");
    put_long_run_of_a(w);
    std::vector<u8> expected;
    for (char c : std::string_view("abaabbbbbbbbb")) {
        expected.push_back(static_cast<u8>(c));
    }
    expected.resize(kDd, 'a');
    const auto fresh =
        heavy_archive({heavy_record(0, kNewCodes | kRle | kKeep, 18, w.bytes(), expected)});
    CHECK_EQ(dms_unpack(first.data(), first.size(), out.data(), out.size(), *work, info),
             DmsError::Ok);
    CHECK_EQ(dms_unpack(fresh.data(), fresh.size(), out.data(), out.size(), *work, info),
             DmsError::Ok);
    CHECK(std::equal(expected.begin(), expected.end(), out.begin()));
}

// Single-symbol codes: every symbol is sent in zero bits.
std::vector<u8> single_codes(u16 literal, u16 offset_class) {
    BitWriter w;
    w.put(0, 9);
    w.put(literal, 9);
    w.put(0, 5);
    w.put(offset_class, 5);
    return w.bytes();
}

// A first stage shorter than the track is followed by zeros, with or without
// run-length decoding.
void heavy2_short_stage_is_zero_filled() {
    std::vector<u8> track(100, 'A');
    track.resize(kDd, 0);
    for (u8 flags : {kNewCodes, u8{kNewCodes | kRle}}) {
        const auto dms = heavy_archive({heavy_record(0, flags, 100, single_codes('A', 0), track)});
        const Result r = run(dms);
        CHECK_EQ(r.error, DmsError::Ok);
        CHECK(same_prefix(r.out, track));
    }
    // Run-length decoding reads into the zeros: 0x90 0x90 0x00 is a run of
    // 144 zero bytes.
    const std::vector<u8> zeros(kDd, 0);
    auto dms = heavy_archive({heavy_record(0, kNewCodes | kRle, 2, single_codes(0x90, 0), zeros)});
    Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK(same_prefix(r.out, zeros));
    // The zeros end at the track length: 0x90 0x00 and 11262 zeros leave the
    // track one byte short.
    std::vector<u8> escaped(1, 0x90);
    escaped.resize(kDd, 0);
    dms = heavy_archive({heavy_record(0, kNewCodes | kRle, 1, single_codes(0x90, 0), escaped)});
    r = run(dms);
    CHECK_EQ(r.error, DmsError::Corrupt);
}

void heavy2_errors() {
    const std::vector<u8> zeros(kDd, 0);
    auto corrupt = [&](u8 flags, usize stage_len, std::vector<u8> packed) {
        packed.resize(packed.size() + 400, 0);  // so that running out of bits is not the cause
        const Result r = run(heavy_archive({heavy_record(0, flags, stage_len, packed, zeros)}));
        CHECK_EQ(r.error, DmsError::Corrupt);
        CHECK_EQ(r.info.failed_track, 0u);
    };

    // No codes have been sent yet.
    corrupt(kRle, kDd, {});
    corrupt(0, kDd, {});
    // A single symbol outside either alphabet.
    corrupt(kNewCodes, kDd, single_codes(510, 0));
    corrupt(kNewCodes, kDd, single_codes(511, 0));
    corrupt(kNewCodes, kDd, single_codes(0, 15));
    corrupt(kNewCodes, kDd, single_codes(0, 31));
    // More code lengths than symbols.
    {
        BitWriter w;
        w.put(511, 9);
        corrupt(kNewCodes, kDd, w.bytes());
        BitWriter p;
        p.put(0, 9);
        p.put(0, 9);
        p.put(16, 5);
        corrupt(kNewCodes, kDd, p.bytes());
    }
    // Incomplete and oversubscribed codes, and a code with no symbol.
    for (std::vector<u32> lengths : {std::vector<u32>{1, 0}, std::vector<u32>{1, 1, 1},
                                     std::vector<u32>{2, 2, 2}, std::vector<u32>{0, 0, 0}}) {
        BitWriter w;
        w.put(static_cast<u32>(lengths.size()), 9);
        for (u32 len : lengths) {
            w.put(len, 5);
        }
        w.put(0, 5);
        w.put(0, 5);
        corrupt(kNewCodes, kDd, w.bytes());
    }
    // A match of 256 bytes that runs past the 100 byte first stage.
    corrupt(kNewCodes, 100, single_codes(509, 0));
    // A run past the end of the track: 0x90 0xFF 0xFF 0xFF 0xFF.
    {
        BitWriter w;
        put_codes(w);
        w.code("100" "110" "110" "110" "110");
        corrupt(kNewCodes | kRle, 5, w.bytes());
    }

    // The packed data ends early, wherever it is cut.
    auto bits = first_track_bits();
    for (usize cut = 0; cut < bits.size(); ++cut) {
        const std::vector<u8> packed(bits.begin(), bits.begin() + static_cast<long>(cut));
        const Result r = run(
            heavy_archive({heavy_record(0, kNewCodes | kRle, 18, packed, first_track())}));
        CHECK_EQ(r.error, DmsError::Corrupt);
    }
}

// Random packed data with a valid CRC never decodes, and never reads or
// writes outside its buffers.
void heavy2_rejects_garbage() {
    Prng rng;
    const std::vector<u8> zeros(kDd, 0);
    for (u32 seed = 0; seed < 1000; ++seed) {
        rng.state = 0x9E3779B9u * (seed + 1);
        std::vector<u8> packed(1 + (rng.next() << 4 | (rng.next() & 0x0F)));
        for (u8& b : packed) {
            b = rng.next();
        }
        const u8 flags = static_cast<u8>(seed & 7);
        const usize stage_len = (static_cast<usize>(rng.next()) << 8 | rng.next()) % (2 * kDd);
        const Result r = run(heavy_archive({heavy_record(0, flags, stage_len, packed, zeros)}));
        CHECK(r.error == DmsError::Corrupt || r.error == DmsError::Truncated);
    }

    // Random data behind valid codes runs the match and run-length paths.
    // It can decode, so a wrong checksum is a valid outcome as well.
    for (u32 seed = 0; seed < 1000; ++seed) {
        rng.state = 0x85EBCA6Bu * (seed + 1);
        BitWriter w;
        put_codes(w);
        std::vector<u8> packed = w.bytes();
        packed.resize(packed.size() + 1 + (rng.next() << 4 | (rng.next() & 0x0F)));
        for (usize i = w.bytes().size(); i < packed.size(); ++i) {
            packed[i] = rng.next();
        }
        const u8 flags = static_cast<u8>(kNewCodes | (seed & (kRle | kKeep)));
        const usize stage_len = (static_cast<usize>(rng.next()) << 8 | rng.next()) % (2 * kDd);
        const Result r = run(heavy_archive({heavy_record(0, flags, stage_len, packed, zeros)}));
        CHECK(r.error == DmsError::Corrupt || r.error == DmsError::TrackChecksum);
    }
}

void output_too_small() {
    const auto adf = make_adf();
    const auto dms = build_dms(adf, 0, kDd);
    Result r = run(dms, 5u * kDd - 1);
    CHECK_EQ(r.error, DmsError::OutputTooSmall);
    CHECK_EQ(r.info.failed_track, 0u);
    // Nothing is written when the image cannot fit.
    CHECK_EQ(r.out[0], 0xEE);
    r = run(dms, 5u * kDd);
    CHECK_EQ(r.error, DmsError::Ok);
    r = run(dms, 0);
    CHECK_EQ(r.error, DmsError::OutputTooSmall);
}

void bad_track_range() {
    auto dms = build_dms(make_adf(), 0, kDd);
    put16(dms, 18, 80);  // last track 80
    refresh_header_crc(dms);
    CHECK_EQ(run(dms).error, DmsError::Corrupt);
    dms = build_dms(make_adf(), 0, kDd);
    put16(dms, 16, 9);  // first above last
    refresh_header_crc(dms);
    CHECK_EQ(run(dms).error, DmsError::Corrupt);
}

void failed_track_is_none_on_success() {
    const Result r = run(build_dms(make_adf(), 0, kDd));
    CHECK_EQ(r.info.failed_track, 0xFFFFFFFFu);
}

void read_header_ok() {
    const auto dms = build_dms(make_adf(), 1, kDd);
    DmsInfo info{};
    CHECK_EQ(dms_read_header(dms.data(), dms.size(), info), DmsError::Ok);
    CHECK_EQ(info.mode, 1);
    CHECK_EQ(info.general_info, 0);
    CHECK_EQ(info.first_track, 0);
    CHECK_EQ(info.last_track, 4);
    CHECK_EQ(info.image_bytes, 5u * kDd);
    // The header alone is enough.
    CHECK_EQ(dms_read_header(dms.data(), kHeader, info), DmsError::Ok);
}

void read_header_errors() {
    const auto dms = build_dms(make_adf(), 0, kDd);
    DmsInfo info{};
    CHECK_EQ(dms_read_header(dms.data(), kHeader - 1, info), DmsError::Truncated);
    CHECK_EQ(dms_read_header(dms.data(), 0, info), DmsError::Truncated);
    CHECK_EQ(dms_read_header(dms.data(), 3, info), DmsError::Truncated);
    std::vector<u8> junk(kHeader, 0x41);
    CHECK_EQ(dms_read_header(junk.data(), junk.size(), info), DmsError::NotDms);
    auto bad = dms;
    bad[20] ^= 0x80;
    CHECK_EQ(dms_read_header(bad.data(), bad.size(), info), DmsError::HeaderCrc);
}

}

int main() {
    crc16_known_vector();
    names();
    nocomp_roundtrip();
    high_density_roundtrip();
    simple_roundtrip();
    missing_track_zero_filled();
    missing_leading_and_trailing_tracks_zero_filled();
    banner_track_skipped();
    skipped_track_with_unsupported_mode_is_ignored();
    not_dms();
    header_crc();
    truncated_everywhere_in_the_front();
    truncated_mid_track();
    cut_at_a_track_boundary_is_truncated();
    header_packed_size_matches_the_records();
    bytes_after_the_last_record_are_tolerated();
    unmarked_bytes_after_the_records_end_the_archive();
    duplicate_track_is_corrupt();
    track_checksum_is_the_16_bit_byte_sum();
    track_header_errors();
    track_data_crc();
    track_checksum();
    unsupported_mode();
    encrypted();
    bad_track_size();
    damaged_run_length_data();
    heavy2_hand_packed_track();
    heavy2_track_flags();
    heavy2_short_stage_is_zero_filled();
    heavy2_errors();
    heavy2_rejects_garbage();
    output_too_small();
    bad_track_range();
    failed_track_is_none_on_success();
    read_header_ok();
    read_header_errors();
    return check::finish("test_dms");
}
