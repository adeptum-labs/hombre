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

// Host only: converts a linked m68k ELF executable to an AmigaOS hunk file.
//
//   elf2hunk <in.elf> <out.hunk>
//
// The input is big-endian ELF32, linked with --emit-relocs (see
// elf2hunk.hpp for what is accepted).

#include "elf2hunk.hpp"

#include <cstdio>
#include <vector>

using namespace hombre;

namespace {

bool read_file(const char* path, std::vector<u8>& data) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::perror(path);
        return false;
    }
    u8 chunk[65536];
    usize got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        data.insert(data.end(), chunk, chunk + got);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) {
        std::fprintf(stderr, "%s: read error\n", path);
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
    if (std::fclose(f) != 0 || !wrote) {
        std::fprintf(stderr, "%s: write error\n", path);
        std::remove(path);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: elf2hunk <in.elf> <out.hunk>\n");
        return 2;
    }
    std::vector<u8> elf;
    if (!read_file(argv[1], elf)) {
        return 1;
    }
    const ConversionResult result = elf_to_hunk(elf);
    if (!result.ok()) {
        std::fprintf(stderr, "elf2hunk: %s: %s\n", argv[1], result.error.c_str());
        return 1;
    }
    return write_file(argv[2], result.hunk) ? 0 : 1;
}
