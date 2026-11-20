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

// Host only: writes the picture disk the WBPattern boot check mounts.
//
//   mkpicdisk <out.adf>
//
// An OFS volume named Pics whose drawer Pics holds sixteen small IFF ILBM
// pictures, Pic01.ilbm to Pic16.ilbm, plain, and Zz.ilbm, rings of three
// planes packed with ByteRun1. Zz.ilbm sorts last, so a file requester of
// ten rows shows it only when scrolled to its end.

#include "ofs_writer.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

using namespace hombre;

namespace {

void be32(std::vector<u8>& v, u32 x) {
    for (int i = 3; i >= 0; --i) {
        v.push_back(static_cast<u8>(x >> (i * 8)));
    }
}

void be16(std::vector<u8>& v, u32 x) {
    v.push_back(static_cast<u8>(x >> 8));
    v.push_back(static_cast<u8>(x));
}

void chunk(std::vector<u8>& f, const char* id, const std::vector<u8>& data) {
    f.insert(f.end(), id, id + 4);
    be32(f, static_cast<u32>(data.size()));
    f.insert(f.end(), data.begin(), data.end());
    if (data.size() % 2 != 0) {
        f.push_back(0);
    }
}

// ByteRun1: runs of three or more as repeats, the rest as literals.
std::vector<u8> byte_run(const std::vector<u8>& row) {
    std::vector<u8> out;
    usize i = 0;
    while (i < row.size()) {
        usize run = 1;
        while (i + run < row.size() && row[i + run] == row[i] && run < 128) {
            ++run;
        }
        if (run >= 3) {
            out.push_back(static_cast<u8>(257 - run));
            out.push_back(row[i]);
            i += run;
            continue;
        }
        usize literal = 0;
        while (i + literal < row.size() && literal < 128) {
            usize same = 1;
            while (i + literal + same < row.size() &&
                   row[i + literal + same] == row[i + literal] && same < 3) {
                ++same;
            }
            if (same >= 3) {
                break;
            }
            ++literal;
        }
        out.push_back(static_cast<u8>(literal - 1));
        out.insert(out.end(), row.begin() + static_cast<long>(i),
                   row.begin() + static_cast<long>(i + literal));
        i += literal;
    }
    return out;
}

// An ILBM of width by height pixels of planes planes, its pen at x, y
// from pen(x, y), plain or packed.
template <typename Pen>
std::vector<u8> ilbm(u32 width, u32 height, u32 planes, bool packed, Pen pen) {
    std::vector<u8> header;
    be16(header, width);
    be16(header, height);
    be16(header, 0);
    be16(header, 0);
    header.push_back(static_cast<u8>(planes));
    header.push_back(0);                   // no mask
    header.push_back(packed ? 1 : 0);      // ByteRun1 or none
    header.push_back(0);
    be16(header, 0);
    header.push_back(10);
    header.push_back(11);
    be16(header, 320);
    be16(header, 200);
    std::vector<u8> colors;
    for (u32 c = 0; c < (1u << planes); ++c) {
        colors.push_back(static_cast<u8>((c * 53) & 0xF0));
        colors.push_back(static_cast<u8>((c * 97) & 0xF0));
        colors.push_back(static_cast<u8>((255 - c * 37) & 0xF0));
    }
    const u32 row_bytes = (width + 15) / 16 * 2;
    std::vector<u8> body;
    for (u32 y = 0; y < height; ++y) {
        for (u32 p = 0; p < planes; ++p) {
            std::vector<u8> row(row_bytes, 0);
            for (u32 x = 0; x < width; ++x) {
                if (((pen(x, y) >> p) & 1) != 0) {
                    row[x / 8] = static_cast<u8>(row[x / 8] | (0x80 >> (x & 7)));
                }
            }
            if (packed) {
                const std::vector<u8> run = byte_run(row);
                body.insert(body.end(), run.begin(), run.end());
            } else {
                body.insert(body.end(), row.begin(), row.end());
            }
        }
    }
    std::vector<u8> form = {'I', 'L', 'B', 'M'};
    chunk(form, "BMHD", header);
    chunk(form, "CMAP", colors);
    chunk(form, "BODY", body);
    std::vector<u8> file = {'F', 'O', 'R', 'M'};
    be32(file, static_cast<u32>(form.size()));
    file.insert(file.end(), form.begin(), form.end());
    return file;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: mkpicdisk <out.adf>\n");
        return 2;
    }
    try {
        std::vector<AdfFile> files;
        for (u32 n = 1; n <= 16; ++n) {
            char name[32];
            std::snprintf(name, sizeof name, "Pics/Pic%02u.ilbm", n);
            files.push_back({name, ilbm(16, 16, 2, false, [n](u32 x, u32 y) {
                                 return (x / 4 + y / 4 + n) % 4;
                             })});
        }
        files.push_back({"Pics/Zz.ilbm", ilbm(48, 48, 3, true, [](u32 x, u32 y) {
                             const i32 dx = static_cast<i32>(x) - 24;
                             const i32 dy = static_cast<i32>(y) - 24;
                             return static_cast<u32>((dx * dx + dy * dy) / 40) % 8;
                         })});
        const std::vector<u8> image = build_adf("Pics", false, files);
        std::FILE* out = std::fopen(argv[1], "wb");
        if (out == nullptr || std::fwrite(image.data(), 1, image.size(), out) != image.size()) {
            std::fprintf(stderr, "mkpicdisk: cannot write %s\n", argv[1]);
            return 1;
        }
        std::fclose(out);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mkpicdisk: %s\n", e.what());
        return 1;
    }
    return 0;
}
