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

// The ILBM writer: ByteRun1 rows and the chunks of a whole file.

#include "check.hpp"
#include "image/ilbm.hpp"

#include <vector>

using namespace hombre;
using namespace hombre::image;

namespace {

// ByteRun1 unpacked again, for the round trips.
std::vector<u8> unpack(const u8* src, u32 n) {
    std::vector<u8> out;
    u32 i = 0;
    while (i < n) {
        const i8 count = static_cast<i8>(src[i++]);
        if (count >= 0) {
            for (i32 k = 0; k <= count; ++k) {
                out.push_back(src[i++]);
            }
        } else if (count != -128) {
            for (i32 k = 0; k < 1 - count; ++k) {
                out.push_back(src[i]);
            }
            ++i;
        }
    }
    return out;
}

u32 be32(const std::vector<u8>& v, u32 at) {
    return (u32{v[at]} << 24) | (u32{v[at + 1]} << 16) | (u32{v[at + 2]} << 8) | v[at + 3];
}

void packs_runs_and_literals() {
    const u8 row[] = {1, 2, 3, 3, 3, 3, 4, 4, 5};
    u8 out[ilbm::packed_bound(sizeof row)];
    const u32 n = ilbm::pack_row(row, sizeof row, out);
    const u8 expect[] = {1, 1, 2, 0xFD, 3, 2, 4, 4, 5};
    CHECK_EQ(n, sizeof expect);
    for (u32 i = 0; i < n; ++i) {
        CHECK_EQ(out[i], expect[i]);
    }
}

void packs_long_rows_in_runs_of_at_most_128() {
    std::vector<u8> row(300, 7);
    for (u32 i = 200; i < 300; ++i) {
        row[i] = static_cast<u8>(i);
    }
    std::vector<u8> out(ilbm::packed_bound(300));
    const u32 n = ilbm::pack_row(row.data(), 300, out.data());
    CHECK(n <= ilbm::packed_bound(300));
    CHECK_EQ(out[0], 0x81u);  // 128 sevens
    CHECK(unpack(out.data(), n) == row);
    // Literals only, more than 128 of them.
    std::vector<u8> mixed(260);
    for (u32 i = 0; i < 260; ++i) {
        mixed[i] = static_cast<u8>(i * 7);
    }
    const u32 m = ilbm::pack_row(mixed.data(), 260, out.data());
    CHECK_EQ(m, ilbm::packed_bound(260));
    CHECK(unpack(out.data(), m) == mixed);
}

void writes_the_chunks_in_order() {
    // 20 by 2 pixels, 2 planes: rows of 4 bytes.
    u8 planes[2][2][4] = {{{0xFF, 0xFF, 0xFF, 0xFF}, {1, 2, 3, 4}},
                          {{0, 0, 0, 0}, {9, 9, 9, 9}}};
    const u8 colours[] = {0, 0, 0, 255, 255, 255, 255, 0, 0};
    const u8 name[] = {'p', 'i', 'c'};
    ilbm::Picture p;
    p.header.width = 20;
    p.header.height = 2;
    p.header.depth = 2;
    p.header.x_aspect = 10;
    p.header.y_aspect = 11;
    p.header.page_width = 320;
    p.header.page_height = 200;
    p.has_mode = true;
    p.mode_id = 0x8004;
    p.colours = colours;
    p.colour_count = 3;
    p.name = name;
    p.name_length = 3;
    std::vector<u8> file;
    u32 rows_asked = 0;
    const bool ok = ilbm::write(
        p,
        [&](u32 plane, u32 y) -> const u8* {
            ++rows_asked;
            return planes[plane][y];
        },
        [&](const u8* bytes, u32 count) {
            file.insert(file.end(), bytes, bytes + count);
            return true;
        });
    CHECK(ok);
    CHECK_EQ(rows_asked, 8u);
    CHECK_EQ(be32(file, 0), 0x464F524Du);  // FORM
    CHECK_EQ(be32(file, 4), file.size() - 8);
    CHECK_EQ(be32(file, 8), 0x494C424Du);   // ILBM
    CHECK_EQ(be32(file, 12), 0x424D4844u);  // BMHD
    CHECK_EQ(be32(file, 16), 20u);
    CHECK_EQ(file[20], 0u);
    CHECK_EQ(file[21], 20u);
    CHECK_EQ(file[28], 2u);   // depth
    CHECK_EQ(file[30], 1u);   // ByteRun1
    CHECK_EQ(file[34], 10u);  // aspect
    CHECK_EQ(file[35], 11u);
    CHECK_EQ(be32(file, 36), (320u << 16) | 200u);
    CHECK_EQ(be32(file, 40), 0x43414D47u);  // CAMG
    CHECK_EQ(be32(file, 48), 0x8004u);
    CHECK_EQ(be32(file, 52), 0x434D4150u);  // CMAP
    CHECK_EQ(be32(file, 56), 9u);
    CHECK_EQ(file[60 + 3], 255u);
    CHECK_EQ(file[69], 0u);                 // pad
    CHECK_EQ(be32(file, 70), 0x4E414D45u);  // NAME
    CHECK_EQ(be32(file, 74), 3u);
    CHECK_EQ(file[78], 'p');
    CHECK_EQ(file[81], 0u);
    CHECK_EQ(be32(file, 82), 0x424F4459u);  // BODY
    const u32 body = be32(file, 86);
    CHECK_EQ(file.size(), 90u + body + (body & 1));
    const std::vector<u8> unpacked = unpack(file.data() + 90, body);
    CHECK_EQ(unpacked.size(), 16u);
    CHECK_EQ(unpacked[0], 0xFFu);  // row 0, plane 0
    CHECK_EQ(unpacked[4], 0u);     // row 0, plane 1
    CHECK_EQ(unpacked[9], 2u);     // row 1, plane 0
    CHECK_EQ(unpacked[12], 9u);    // row 1, plane 1
}

void writes_plain_rows_without_compression() {
    u8 row[2] = {0xA5, 0x5A};
    ilbm::Picture p;
    p.header.width = 16;
    p.header.height = 1;
    p.header.depth = 1;
    p.header.compression = ilbm::kCompressNone;
    std::vector<u8> file;
    CHECK(ilbm::write(
        p, [&](u32, u32) -> const u8* { return row; },
        [&](const u8* bytes, u32 count) {
            file.insert(file.end(), bytes, bytes + count);
            return true;
        }));
    CHECK_EQ(file.size(), 12u + 28u + 10u);
    CHECK_EQ(be32(file, 40), 0x424F4459u);
    CHECK_EQ(be32(file, 44), 2u);
    CHECK_EQ(file[48], 0xA5u);
}

void refuses_what_it_cannot_write_and_stops_with_the_sink() {
    u8 row[2] = {};
    const auto rows = [&](u32, u32) -> const u8* { return row; };
    const auto take = [](const u8*, u32) { return true; };
    ilbm::Picture p;
    p.header.width = 16;
    p.header.height = 1;
    p.header.depth = 0;
    CHECK(!ilbm::write(p, rows, take));
    p.header.depth = 9;
    CHECK(!ilbm::write(p, rows, take));
    p.header.depth = 1;
    p.header.masking = 1;
    CHECK(!ilbm::write(p, rows, take));
    p.header.masking = 0;
    p.header.compression = 2;
    CHECK(!ilbm::write(p, rows, take));
    p.header.compression = 1;
    p.header.width = ilbm::kMaxSide + 1;
    CHECK(!ilbm::write(p, rows, take));
    p.header.width = 16;
    CHECK(ilbm::write(p, rows, take));
    u32 calls = 0;
    CHECK(!ilbm::write(p, rows, [&](const u8*, u32) { return ++calls < 3; }));
    CHECK_EQ(calls, 3u);
    CHECK(!ilbm::write(p, [](u32, u32) -> const u8* { return nullptr; }, take));
}

}

int main() {
    packs_runs_and_literals();
    packs_long_rows_in_runs_of_at_most_128();
    writes_the_chunks_in_order();
    writes_plain_rows_without_compression();
    refuses_what_it_cannot_write_and_stops_with_the_sink();
    return check::finish("test_ilbm_write");
}
