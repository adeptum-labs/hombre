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

// Host front end for the kernel's disk image code.
//
//   adftool unpack <in.dms> <out.adf>   unpack a DMS archive to a raw image

#include "dms.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace hombre;

namespace {

constexpr usize kMaxImage = 80u * 22528u;  // an HD disk

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

bool write_file(const char* path, const u8* data, usize length) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::perror(path);
        return false;
    }
    const bool ok = std::fwrite(data, 1, length, f) == length;
    if (std::fclose(f) != 0 || !ok) {
        std::fprintf(stderr, "%s: write error\n", path);
        return false;
    }
    return true;
}

int unpack(const char* in_path, const char* out_path) {
    std::vector<u8> in;
    if (!read_file(in_path, in)) {
        return 1;
    }
    auto work = std::make_unique<DmsWork>();
    std::vector<u8> image(kMaxImage);
    DmsInfo info{};
    const DmsError error = dms_unpack(in.data(), in.size(), image.data(), image.size(), *work, info);
    if (error != DmsError::Ok) {
        std::fprintf(stderr, "%s: %s", in_path, dms_error_name(error));
        if (info.failed_track != 0xFFFFFFFFu) {
            std::fprintf(stderr, " (track %u)", info.failed_track);
        }
        std::fprintf(stderr, "\n");
        return 1;
    }
    return write_file(out_path, image.data(), info.image_bytes) ? 0 : 1;
}

void usage() { std::fprintf(stderr, "usage: adftool unpack <in.dms> <out.adf>\n"); }

}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "unpack") == 0) {
        return unpack(argv[2], argv[3]);
    }
    usage();
    return 2;
}
