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
//   mkdisk <out.dms> <version.hunk> <hombretest.hunk> <mousetest.hunk> <tasktest.hunk>
//          <timertest.hunk> <loadwb.hunk> <wbtest.hunk> <cgxtest.hunk>
//
// The disk is an OFS volume named HombreTest, packed as a DMS archive with
// SIMPLE tracks. It holds S/Startup-Sequence, C/Echo, Hello and the eight
// hunk files, as C/Version, LIBS/hombretest.library, C/mousetest,
// C/tasktest, C/timertest, C/LoadWB and C/cgxtest, and wbtest with its icon in
// WBStartup, which SYS: names. The output depends on nothing but this
// source and the hunk files, so two runs give identical files.

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

// The DiskObject of the wbtest icon: a tool with a two-plane image, no
// default tool and no position of its own, so the Workbench lays it out.
std::vector<u8> wbtest_info() {
    std::vector<u8> bytes;
    auto word = [&](u32 v) {
        bytes.push_back(static_cast<u8>(v >> 8));
        bytes.push_back(static_cast<u8>(v));
    };
    auto longv = [&](u32 v) {
        word(v >> 16);
        word(v);
    };
    constexpr u32 kImageWidth = 32;
    constexpr u32 kImageHeight = 12;
    constexpr u32 kDepth = 2;
    word(0xE310);  // do_Magic
    word(1);       // do_Version
    // do_Gadget
    longv(0);  // NextGadget
    word(0);   // LeftEdge
    word(0);   // TopEdge
    word(kImageWidth);
    word(kImageHeight);
    word(0);  // Flags
    word(1);  // Activation: RELVERIFY
    word(1);  // GadgetType: BOOLGADGET
    longv(1);  // GadgetRender
    longv(0);  // SelectRender
    longv(0);  // GadgetText
    longv(0);  // MutualExclude
    longv(0);  // SpecialInfo
    word(0);   // GadgetID
    longv(1);  // UserData: WB_DISKREVISION 1
    bytes.push_back(3);  // do_Type: WBTOOL
    bytes.push_back(0);  // do_Pad
    longv(0);            // do_DefaultTool
    longv(0);            // do_ToolTypes
    longv(0x80000000);   // do_CurrentX: NO_ICON_POSITION
    longv(0x80000000);   // do_CurrentY: NO_ICON_POSITION
    longv(0);            // do_DrawerData
    longv(0);            // do_ToolWindow
    longv(4096);         // do_StackSize
    // The render Image and its plane data, rows top to bottom per plane.
    word(0);  // LeftEdge
    word(0);  // TopEdge
    word(kImageWidth);
    word(kImageHeight);
    word(kDepth);
    longv(1);         // ImageData
    bytes.push_back(3);  // PlanePick
    bytes.push_back(0);  // PlaneOnOff
    longv(0);            // NextImage
    const u32 row_bytes = (kImageWidth + 15) / 16 * 2;
    for (u32 plane = 0; plane < kDepth; ++plane) {
        for (u32 row = 0; row < kImageHeight; ++row) {
            for (u32 column = 0; column < row_bytes; ++column) {
                bytes.push_back(static_cast<u8>((row * 7 + column * 13 + plane * 29) & 0xFF));
            }
        }
    }
    return bytes;
}

std::vector<AdfFile> test_files(const std::vector<u8>& version, const std::vector<u8>& library,
                                const std::vector<u8>& mousetest,
                                const std::vector<u8>& tasktest,
                                const std::vector<u8>& timertest,
                                const std::vector<u8>& loadwb,
                                const std::vector<u8>& wbtest,
                                const std::vector<u8>& cgxtest) {
    std::vector<u8> echo(kEchoBytes);
    for (usize i = 0; i < echo.size(); ++i) {
        // A repeating run A..Z that shows where a block boundary was crossed.
        echo[i] = static_cast<u8>('A' + i % 26);
    }
    return {
        {"S/Startup-Sequence", text("Echo \"Hombre test startup\"\nC:Version\n")},
        {"C/Echo", echo},
        {"Hello", text("Hello from the Hombre test disk.\n")},
        {"C/Version", version},
        {"LIBS/hombretest.library", library},
        {"C/mousetest", mousetest},
        {"C/tasktest", tasktest},
        {"C/timertest", timertest},
        {"C/LoadWB", loadwb},
        {"C/cgxtest", cgxtest},
        {"WBStartup/wbtest", wbtest},
        {"WBStartup/wbtest.info", wbtest_info()},
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
    if (argc != 10) {
        std::fprintf(stderr,
                     "usage: mkdisk <out.dms> <version.hunk> <hombretest.hunk> <mousetest.hunk>"
                     " <tasktest.hunk> <timertest.hunk> <loadwb.hunk> <wbtest.hunk>"
                     " <cgxtest.hunk>\n");
        return 2;
    }
    std::vector<u8> version;
    std::vector<u8> library;
    std::vector<u8> mousetest;
    std::vector<u8> tasktest;
    std::vector<u8> timertest;
    std::vector<u8> loadwb;
    std::vector<u8> wbtest;
    std::vector<u8> cgxtest;
    if (!read_file(argv[2], version) || !read_file(argv[3], library) ||
        !read_file(argv[4], mousetest) || !read_file(argv[5], tasktest) ||
        !read_file(argv[6], timertest) || !read_file(argv[7], loadwb) ||
        !read_file(argv[8], wbtest) || !read_file(argv[9], cgxtest)) {
        return 1;
    }
    try {
        const std::vector<u8> adf =
            build_adf("HombreTest", false,
                      test_files(version, library, mousetest, tasktest, timertest, loadwb,
                                 wbtest, cgxtest));
        const std::vector<u8> dms = build_dms(adf, kSimple, kDdTrackBytes);
        return write_file(argv[1], dms) ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mkdisk: %s\n", e.what());
        return 1;
    }
}
