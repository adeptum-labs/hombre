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
            CHECK_EQ(r.info.failed_track, 4u);
        }
    }
}

void cut_at_a_track_boundary_keeps_the_earlier_tracks() {
    const auto adf = make_adf();
    auto dms = build_dms(adf, 0, kDd);
    dms.resize(nocomp_track_at(3));
    const Result r = run(dms);
    CHECK_EQ(r.error, DmsError::Ok);
    CHECK_EQ(r.info.tracks_unpacked, 3u);
    CHECK(std::equal(adf.begin(), adf.begin() + 3 * kDd, r.out.begin()));
    CHECK_EQ(r.out[3u * kDd], 0);
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
    CHECK_EQ(r.info.failed_track, 1u);
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
    for (u8 mode : {u8{2}, u8{3}, u8{4}, u8{5}, u8{6}, u8{9}, u8{255}}) {
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
    dms[11] &= static_cast<u8>(~0x02);
    dms[11] |= 0x40;
    refresh_header_crc(dms);
    CHECK_EQ(run(dms).error, DmsError::Encrypted);
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
    auto dms = dms_header(1, 0, 0, static_cast<u32>(rec.size()), kDd);
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
    cut_at_a_track_boundary_keeps_the_earlier_tracks();
    track_header_errors();
    track_data_crc();
    track_checksum();
    unsupported_mode();
    encrypted();
    bad_track_size();
    damaged_run_length_data();
    output_too_small();
    bad_track_range();
    failed_track_is_none_on_success();
    read_header_ok();
    read_header_errors();
    return check::finish("test_dms");
}
