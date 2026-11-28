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
//   mkpicdisk --large <out.adf>
//   mkpicdisk --image-datatypes <out.adf>
//
// An OFS volume named Pics whose drawer Pics holds sixteen small IFF ILBM
// pictures, Pic01.ilbm to Pic16.ilbm, plain, and Zz.ilbm, rings of three
// planes packed with ByteRun1. Zz.ilbm sorts last, so a file requester of
// ten rows shows it only when scrolled to its end.
//
// With --large, the disk the MultiView boot check mounts: an OFS volume
// named BigPics holding Large.ilbm, 1024 by 768 pixels of five planes
// packed with ByteRun1, larger than the screen either way: bands of 32
// pens across, crossed by a diagonal stripe and a ring about its middle.
//
// With --image-datatypes, the disk the png.datatype and jpeg.datatype boot
// check mounts: an OFS volume named Images holding Gradient.png, 640 by 480
// pixels of a 256 colour palette whose first colour the tRNS chunk makes
// clear, and Photo.jpg, a baseline JPEG. The PNG's zlib stream is stored
// blocks, and only its bottom right corner, 48 columns by 32 rows, is
// clear, so that loading it yields while the file is read, while its
// pixels are scanned for alpha and while they are written into the object.

#include "ofs_writer.hpp"
#include "../tests/image_fixtures.hpp"

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

u32 crc32(const u8* data, usize bytes) {
    u32 crc = 0xFFFFFFFFu;
    for (usize i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (u32 bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
        }
    }
    return ~crc;
}

u32 adler32(const std::vector<u8>& data) {
    u32 a = 1;
    u32 b = 0;
    for (const u8 c : data) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void chunk_png(std::vector<u8>& out, const char* id, const std::vector<u8>& data) {
    be32(out, static_cast<u32>(data.size()));
    const usize crc_at = out.size();
    out.insert(out.end(), id, id + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc32(out.data() + crc_at, out.size() - crc_at));
}

// A 640 by 480 gradient over a palette of 15 by 17 colours and colour 0,
// which is clear and fills the bottom right corner, 48 columns by 32 rows.
// The zlib stream is made of stored deflate blocks.
std::vector<u8> gradient_png() {
    constexpr u32 width = 640;
    constexpr u32 height = 480;
    constexpr u32 across = 15;
    constexpr u32 down = 17;
    std::vector<u8> raw;
    raw.reserve(height * (1 + width));
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        for (u32 x = 0; x < width; ++x) {
            const bool clear = x >= width - 48 && y >= height - 32;
            raw.push_back(clear ? 0 : static_cast<u8>(1 + x * across / width * down +
                                                      y * down / height));
        }
    }
    std::vector<u8> zlib = {0x78, 0x01};
    for (usize at = 0; at < raw.size();) {
        const usize part = raw.size() - at < 65535 ? raw.size() - at : 65535;
        const u32 length = static_cast<u32>(part);
        const u32 complement = ~length;
        zlib.push_back(at + part == raw.size() ? 1 : 0);
        zlib.push_back(static_cast<u8>(length));
        zlib.push_back(static_cast<u8>(length >> 8));
        zlib.push_back(static_cast<u8>(complement));
        zlib.push_back(static_cast<u8>(complement >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<long>(at),
                    raw.begin() + static_cast<long>(at + part));
        at += part;
    }
    const u32 sum = adler32(raw);
    be32(zlib, sum);

    std::vector<u8> ihdr;
    be32(ihdr, width);
    be32(ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(3);  // colour type: palette
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);

    std::vector<u8> palette = {0, 0, 0};
    for (u32 i = 0; i < across * down; ++i) {
        const u32 column = i / down;
        const u32 row = i % down;
        palette.push_back(static_cast<u8>(column * 255 / (across - 1)));
        palette.push_back(static_cast<u8>(row * 255 / (down - 1)));
        palette.push_back(static_cast<u8>((column * 3 + row * 5) * 255 / (3 * 14 + 5 * 16)));
    }

    std::vector<u8> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    chunk_png(png, "IHDR", ihdr);
    chunk_png(png, "PLTE", palette);
    // Colour 0 is clear, the others opaque.
    chunk_png(png, "tRNS", std::vector<u8>{0});
    chunk_png(png, "IDAT", zlib);
    chunk_png(png, "IEND", std::vector<u8>());
    return png;
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
    const bool large = argc == 3 && std::string(argv[1]) == "--large";
    const bool images = argc == 3 && std::string(argv[1]) == "--image-datatypes";
    if (argc != 2 && !large && !images) {
        std::fprintf(stderr, "usage: mkpicdisk [--large|--image-datatypes] <out.adf>\n");
        return 2;
    }
    const char* path = argv[argc - 1];
    try {
        std::vector<AdfFile> files;
        if (large) {
            files.push_back({"Large.ilbm", ilbm(1024, 768, 5, true, [](u32 x, u32 y) {
                                 const i32 dx = static_cast<i32>(x) - 512;
                                 const i32 dy = static_cast<i32>(y) - 384;
                                 const i32 d2 = dx * dx + dy * dy;
                                 if (d2 > 200 * 200 && d2 < 216 * 216) {
                                     return 31u;
                                 }
                                 if ((x + y) % 256 < 12) {
                                     return 1u;
                                 }
                                 return (x / 32) % 32;
                             })});
        }
        if (images) {
            files.push_back({"Gradient.png", gradient_png()});
            files.push_back({"Photo.jpg",
                             std::vector<u8>(image_fixtures::b444_jpg_file,
                                             image_fixtures::b444_jpg_file +
                                                 sizeof image_fixtures::b444_jpg_file)});
        }
        for (u32 n = 1; !large && !images && n <= 16; ++n) {
            char name[32];
            std::snprintf(name, sizeof name, "Pics/Pic%02u.ilbm", n);
            files.push_back({name, ilbm(16, 16, 2, false, [n](u32 x, u32 y) {
                                 return (x / 4 + y / 4 + n) % 4;
                             })});
        }
        if (!large && !images) {
            files.push_back({"Pics/Zz.ilbm", ilbm(48, 48, 3, true, [](u32 x, u32 y) {
                                 const i32 dx = static_cast<i32>(x) - 24;
                                 const i32 dy = static_cast<i32>(y) - 24;
                                 return static_cast<u32>((dx * dx + dy * dy) / 40) % 8;
                             })});
        }
        const std::vector<u8> image =
            build_adf(large ? "BigPics" : images ? "Images" : "Pics", false, files);
        std::FILE* out = std::fopen(path, "wb");
        if (out == nullptr || std::fwrite(image.data(), 1, image.size(), out) != image.size()) {
            std::fprintf(stderr, "mkpicdisk: cannot write %s\n", path);
            return 1;
        }
        std::fclose(out);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mkpicdisk: %s\n", e.what());
        return 1;
    }
    return 0;
}
