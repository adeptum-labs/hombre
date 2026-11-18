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

// Host only: writes the test disk the boot check mounts.
//
//   mkdisk <out.dms> <version.hunk> <hombretest.hunk> <mousetest.hunk>
//
// The disk is an OFS volume named HombreTest, packed as a DMS archive with
// SIMPLE tracks. It holds S/Startup-Sequence, C/Echo, Hello and the three
// hunk files, as C/Version, LIBS/hombretest.library and C/mousetest. The
// output depends on nothing but this source and the hunk files, so two runs
// give identical files.

#include "dms.hpp"
#include "dms_writer.hpp"
#include "ofs_writer.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

using namespace hombre;

namespace {

constexpr u16 kSimple = 1;
constexpr u16 kDdTrackBytes = 11264;  // 11 sectors of 512 bytes, two sides
constexpr usize kEchoBytes = 600;

std::vector<u8> text(const std::string& s) {
    return std::vector<u8>(s.begin(), s.end());
}

std::vector<AdfFile> test_files(const std::vector<u8>& version, const std::vector<u8>& library,
                                const std::vector<u8>& mousetest) {
    std::vector<u8> echo(kEchoBytes);
    for (usize i = 0; i < echo.size(); ++i) {
        // A repeating run A..Z that shows where a block boundary was crossed.
        echo[i] = static_cast<u8>('A' + i % 26);
    }
    return {
        {"S/Startup-Sequence", text("Hombre test startup\n")},
        {"C/Echo", echo},
        {"Hello", text("Hello from the Hombre test disk.\n")},
        {"C/Version", version},
        {"LIBS/hombretest.library", library},
        {"C/mousetest", mousetest},
    };
}

bool read_file(const char* path, std::vector<u8>& data) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::perror(path);
        return false;
    }
    u8 chunk[4096];
    usize got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) {
        data.insert(data.end(), chunk, chunk + got);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) {
        std::fprintf(stderr, "mkdisk: cannot read %s\n", path);
    }
    return ok;
}

bool write_file(const char* path, const std::vector<u8>& data) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::perror(path);
        return false;
    }
    const bool wrote = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    const bool flushed = std::fflush(f) == 0;
    const bool closed = std::fclose(f) == 0;
    if (!(wrote && flushed && closed)) {
        std::fprintf(stderr, "mkdisk: cannot write %s\n", path);
        std::remove(path);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: mkdisk <out.dms> <version.hunk> <hombretest.hunk> <mousetest.hunk>\n");
        return 2;
    }
    std::vector<u8> version;
    std::vector<u8> library;
    std::vector<u8> mousetest;
    if (!read_file(argv[2], version) || !read_file(argv[3], library) ||
        !read_file(argv[4], mousetest)) {
        return 1;
    }
    try {
        const std::vector<u8> adf =
            build_adf("HombreTest", false, test_files(version, library, mousetest));
        const std::vector<u8> dms = build_dms(adf, kSimple, kDdTrackBytes);
        return write_file(argv[1], dms) ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mkdisk: %s\n", e.what());
        return 1;
    }
}
