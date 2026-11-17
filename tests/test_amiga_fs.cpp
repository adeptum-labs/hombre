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

#include "amiga_fs.hpp"
#include "check.hpp"
#include "ofs_writer.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace hombre;

namespace {

constexpr usize kDisk = 901120;
constexpr u32 kBlock = 512;

std::vector<u8> text(const char* s) {
    return std::vector<u8>(reinterpret_cast<const u8*>(s), reinterpret_cast<const u8*>(s) + std::strlen(s));
}

// Deterministic bytes that differ from block to block.
std::vector<u8> pattern(usize length, u32 seed) {
    std::vector<u8> v(length);
    u32 x = seed * 2654435761u + 12345u;
    for (usize i = 0; i < length; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = static_cast<u8>(x >> 24);
    }
    return v;
}

struct Listing {
    std::vector<DirEntry> entries;
};

void collect(void* ctx, const DirEntry& entry) {
    static_cast<Listing*>(ctx)->entries.push_back(entry);
}

std::vector<std::string> names(const Listing& l) {
    std::vector<std::string> out;
    for (const DirEntry& e : l.entries) {
        out.push_back(e.name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<AdfFile> sample_files() {
    return {
        {"S/Startup-Sequence", text("C:SetPatch QUIET\nEndCLI >NIL:\n")},
        {"S/Shell-Startup", text("; shell\n")},
        {"README", text("hello")},
        {"Devs/Keymaps/usa", pattern(1500, 7)},
        {"Empty", {}},
    };
}

AdfVolume mount_ok(const std::vector<u8>& image) {
    AdfVolume volume{};
    CHECK_EQ(adf_mount(image.data(), image.size(), volume), FsError::Ok);
    return volume;
}

std::vector<u8> read_all(const AdfVolume& volume, const DirEntry& file) {
    std::vector<u8> out(file.size);
    u32 got = 0xDEADu;
    CHECK_EQ(adf_read(volume, file, 0, out.data(), file.size, got), FsError::Ok);
    CHECK_EQ(got, file.size);
    return out;
}

void set32(std::vector<u8>& image, u32 block, u32 index, u32 value) {
    u8* p = image.data() + static_cast<usize>(block) * kBlock + index * 4;
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
}

u32 get32(const std::vector<u8>& image, u32 block, u32 index) {
    const u8* p = image.data() + static_cast<usize>(block) * kBlock + index * 4;
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

// The reference hash, written out again so the reader is not checked against
// itself.
u32 hash_of(const std::string& name) {
    u32 h = static_cast<u32>(name.size());
    for (char c : name) {
        const u32 up = (c >= 'a' && c <= 'z') ? static_cast<u32>(c - 32) : static_cast<u32>(c);
        h = (h * 13 + up) & 0x7FF;
    }
    return h % 72;
}

void mount_ofs_and_ffs() {
    for (const bool ffs : {false, true}) {
        const auto image = build_adf("Work", ffs, sample_files());
        CHECK_EQ(image.size(), kDisk);
        AdfVolume v = mount_ok(image);
        CHECK_EQ(v.ffs, ffs);
        CHECK(!v.intl);
        CHECK_EQ(v.root_block, 880u);
        CHECK_EQ(v.block_count, 1760u);
        CHECK(v.bootblock_checksum_ok);
        CHECK(v.image == image.data());
        CHECK(std::strcmp(v.name, "Work") == 0);
    }
    // A 30 character name is the longest that fits.
    const auto image = build_adf("123456789012345678901234567890", true, {});
    AdfVolume v = mount_ok(image);
    CHECK(std::strcmp(v.name, "123456789012345678901234567890") == 0);
}

void list_root_and_subdir() {
    for (const bool ffs : {false, true}) {
        const auto image = build_adf("Work", ffs, sample_files());
        AdfVolume v = mount_ok(image);

        Listing root;
        CHECK_EQ(adf_list(v, "", collect, &root), FsError::Ok);
        CHECK(names(root) == (std::vector<std::string>{"Devs", "Empty", "README", "S"}));
        for (const DirEntry& e : root.entries) {
            const std::string n = e.name;
            CHECK_EQ(e.is_dir, n == "Devs" || n == "S");
            if (n == "README") {
                CHECK_EQ(e.size, 5u);
            }
            if (n == "Empty") {
                CHECK_EQ(e.size, 0u);
            }
            CHECK(e.header_block >= 2 && e.header_block < v.block_count);
        }

        Listing s;
        CHECK_EQ(adf_list(v, "S", collect, &s), FsError::Ok);
        CHECK(names(s) == (std::vector<std::string>{"Shell-Startup", "Startup-Sequence"}));

        Listing again;
        CHECK_EQ(adf_list(v, "Work:", collect, &again), FsError::Ok);
        CHECK(names(again) == names(root));

        Listing deep;
        CHECK_EQ(adf_list(v, "devs/KEYMAPS/", collect, &deep), FsError::Ok);
        CHECK(names(deep) == (std::vector<std::string>{"usa"}));
        CHECK_EQ(deep.entries[0].size, 1500u);
    }
}

void empty_directory_lists_nothing() {
    // A directory only exists through a file below it, so empty the file's
    // directory by hand: zero its hash table entry and fix the checksum.
    auto image = build_adf("Work", true, {{"D/f", text("x")}});
    AdfVolume v = mount_ok(image);
    DirEntry d{};
    CHECK_EQ(adf_find(v, "D", d), FsError::Ok);
    for (u32 i = 6; i < 78; ++i) {
        set32(image, d.header_block, i, 0);
    }
    adf_fix_checksum(image, d.header_block);
    Listing l;
    CHECK_EQ(adf_list(v, "D", collect, &l), FsError::Ok);
    CHECK(l.entries.empty());
}

void find_path_variants() {
    for (const bool ffs : {false, true}) {
        const auto image = build_adf("Work", ffs, sample_files());
        AdfVolume v = mount_ok(image);
        DirEntry a{};
        DirEntry b{};
        DirEntry c{};
        DirEntry d{};
        CHECK_EQ(adf_find(v, "S/Startup-Sequence", a), FsError::Ok);
        CHECK_EQ(adf_find(v, "s/startup-sequence", b), FsError::Ok);
        CHECK_EQ(adf_find(v, "Work:S/Startup-Sequence", c), FsError::Ok);
        CHECK_EQ(adf_find(v, "WORK:S/STARTUP-SEQUENCE", d), FsError::Ok);
        CHECK(!a.is_dir);
        CHECK(std::strcmp(a.name, "Startup-Sequence") == 0);
        CHECK(std::strcmp(b.name, "Startup-Sequence") == 0);
        CHECK_EQ(a.size, 30u);
        CHECK_EQ(a.header_block, b.header_block);
        CHECK_EQ(a.header_block, c.header_block);
        CHECK_EQ(a.header_block, d.header_block);

        DirEntry root{};
        CHECK_EQ(adf_find(v, "", root), FsError::Ok);
        CHECK(root.is_dir);
        CHECK_EQ(root.header_block, 880u);
        CHECK(std::strcmp(root.name, "Work") == 0);
        DirEntry root2{};
        CHECK_EQ(adf_find(v, "Work:", root2), FsError::Ok);
        CHECK_EQ(root2.header_block, 880u);
        DirEntry root3{};
        CHECK_EQ(adf_find(v, ":", root3), FsError::Ok);
        CHECK_EQ(root3.header_block, 880u);

        DirEntry dir{};
        CHECK_EQ(adf_find(v, "s", dir), FsError::Ok);
        CHECK(dir.is_dir);
        CHECK_EQ(dir.size, 0u);
        DirEntry slash{};
        CHECK_EQ(adf_find(v, "S/", slash), FsError::Ok);
        CHECK_EQ(slash.header_block, dir.header_block);
    }
}

void intl_folds_latin1() {
    // 0xC5 0xE4 0xF6 (A ring, a diaeresis, o diaeresis) as ISO 8859-1.
    const std::string stored = "\xC5" "\xE4" "\xF6" ".txt";
    const std::string folded = "\xE5" "\xC4" "\xD6" ".TXT";
    const std::vector<AdfFile> files = {{stored, text("x")}};
    {
        const auto image = build_adf("Intl", true, files, true);
        AdfVolume v = mount_ok(image);
        CHECK(v.intl);
        DirEntry e{};
        CHECK_EQ(adf_find(v, stored.c_str(), e), FsError::Ok);
        CHECK_EQ(adf_find(v, folded.c_str(), e), FsError::Ok);
        // The division sign has no upper case partner.
        CHECK_EQ(adf_find(v, "\xF7.txt", e), FsError::NotFound);
    }
    {
        const auto image = build_adf("Plain", true, files, false);
        AdfVolume v = mount_ok(image);
        CHECK(!v.intl);
        DirEntry e{};
        CHECK_EQ(adf_find(v, stored.c_str(), e), FsError::Ok);
        CHECK_EQ(adf_find(v, folded.c_str(), e), FsError::NotFound);
    }
}

void not_found() {
    const auto image = build_adf("Work", true, sample_files());
    AdfVolume v = mount_ok(image);
    DirEntry e{};
    CHECK_EQ(adf_find(v, "S/Nope", e), FsError::NotFound);
    CHECK_EQ(adf_find(v, "Nope", e), FsError::NotFound);
    CHECK_EQ(adf_find(v, "Nope/Deeper", e), FsError::NotFound);
    CHECK_EQ(adf_find(v, "Other:S/Startup-Sequence", e), FsError::NotFound);
    Listing l;
    CHECK_EQ(adf_list(v, "Nope", collect, &l), FsError::NotFound);
    CHECK(l.entries.empty());
}

void not_file() {
    const auto image = build_adf("Work", true, sample_files());
    AdfVolume v = mount_ok(image);
    DirEntry dir{};
    CHECK_EQ(adf_find(v, "S", dir), FsError::Ok);
    u8 buf[16];
    u32 got = 99;
    CHECK_EQ(adf_read(v, dir, 0, buf, sizeof(buf), got), FsError::NotFile);
    CHECK_EQ(got, 0u);
}

void not_dir() {
    const auto image = build_adf("Work", true, sample_files());
    AdfVolume v = mount_ok(image);
    DirEntry e{};
    CHECK_EQ(adf_find(v, "README/x", e), FsError::NotDir);
    CHECK_EQ(adf_find(v, "S/Startup-Sequence/x", e), FsError::NotDir);
    Listing l;
    CHECK_EQ(adf_list(v, "README", collect, &l), FsError::NotDir);
    CHECK(l.entries.empty());
}

void read_small_file() {
    for (const bool ffs : {false, true}) {
        const auto image = build_adf("Work", ffs, sample_files());
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "S/Startup-Sequence", e), FsError::Ok);
        CHECK(read_all(v, e) == text("C:SetPatch QUIET\nEndCLI >NIL:\n"));
        CHECK_EQ(adf_find(v, "Devs/Keymaps/usa", e), FsError::Ok);
        CHECK(read_all(v, e) == pattern(1500, 7));
        CHECK_EQ(adf_find(v, "Empty", e), FsError::Ok);
        u8 buf[4];
        u32 got = 99;
        CHECK_EQ(adf_read(v, e, 0, buf, sizeof(buf), got), FsError::Ok);
        CHECK_EQ(got, 0u);
    }
}

void read_partial_with_offset() {
    for (const bool ffs : {false, true}) {
        const auto data = pattern(3000, 3);
        const auto image = build_adf("Work", ffs, {{"f", data}});
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "f", e), FsError::Ok);
        // Offsets and lengths that start and end inside blocks, straddle
        // block boundaries of both layouts, and run up to the end.
        const u32 offsets[] = {0, 1, 487, 488, 489, 511, 512, 513, 976, 1000, 2999};
        const u32 lengths[] = {1, 2, 100, 488, 512, 1000, 3000};
        for (const u32 off : offsets) {
            for (const u32 len : lengths) {
                std::vector<u8> out(len, 0xAA);
                u32 got = 0;
                CHECK_EQ(adf_read(v, e, off, out.data(), len, got), FsError::Ok);
                const u32 want = std::min<u32>(len, 3000 - off);
                CHECK_EQ(got, want);
                CHECK(std::equal(out.begin(), out.begin() + want, data.begin() + off));
                CHECK(std::all_of(out.begin() + want, out.end(), [](u8 b) { return b == 0xAA; }));
            }
        }
        u8 buf[8];
        u32 got = 99;
        CHECK_EQ(adf_read(v, e, 3000, buf, sizeof(buf), got), FsError::Ok);
        CHECK_EQ(got, 0u);
        got = 99;
        CHECK_EQ(adf_read(v, e, 5000, buf, sizeof(buf), got), FsError::Ok);
        CHECK_EQ(got, 0u);
        got = 99;
        CHECK_EQ(adf_read(v, e, 0, buf, 0, got), FsError::Ok);
        CHECK_EQ(got, 0u);
    }
}

void read_large_file_with_extension_block() {
    for (const bool ffs : {false, true}) {
        // 100 data blocks of 512 bytes: more than the 72 a header holds.
        const auto data = pattern(100 * 512, 11);
        const auto image = build_adf("Work", ffs, {{"big", data}, {"small", text("s")}});
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "big", e), FsError::Ok);
        CHECK_EQ(e.size, data.size());
        CHECK(read_all(v, e) == data);
        // A read that begins inside the extension block.
        std::vector<u8> out(2000);
        u32 got = 0;
        const u32 off = (ffs ? 512u : 488u) * 80 + 17;
        CHECK_EQ(adf_read(v, e, off, out.data(), 2000, got), FsError::Ok);
        CHECK_EQ(got, 2000u);
        CHECK(std::equal(out.begin(), out.end(), data.begin() + off));
    }
    // More than two extension blocks and a size that is not block aligned.
    const auto huge = pattern(300 * 512 + 123, 5);
    for (const bool ffs : {false, true}) {
        const auto image = build_adf("Work", ffs, {{"huge", huge}});
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "huge", e), FsError::Ok);
        CHECK(read_all(v, e) == huge);
    }
}

void read_capacity_smaller_than_file() {
    const auto data = pattern(5000, 9);
    for (const bool ffs : {false, true}) {
        const auto image = build_adf("Work", ffs, {{"f", data}});
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "f", e), FsError::Ok);
        std::vector<u8> out(700, 0);
        u32 got = 0;
        CHECK_EQ(adf_read(v, e, 100, out.data(), 600, got), FsError::Ok);
        CHECK_EQ(got, 600u);
        CHECK(std::equal(out.begin(), out.begin() + 600, data.begin() + 100));
        CHECK_EQ(out[600], 0u);  // nothing written past the capacity
        // Reading in small steps reassembles the file.
        std::vector<u8> whole;
        u32 offset = 0;
        for (;;) {
            u8 chunk[333];
            CHECK_EQ(adf_read(v, e, offset, chunk, sizeof(chunk), got), FsError::Ok);
            if (got == 0) {
                break;
            }
            whole.insert(whole.end(), chunk, chunk + got);
            offset += got;
        }
        CHECK(whole == data);
    }
}

void hash_collisions_chain() {
    // Find three names that share one hash bucket.
    std::vector<std::string> same;
    for (u32 i = 0; same.size() < 3 && i < 100000; ++i) {
        const std::string n = "File" + std::to_string(i);
        if (hash_of(n) == 17) {
            same.push_back(n);
        }
    }
    CHECK_EQ(same.size(), 3u);
    for (const bool ffs : {false, true}) {
        std::vector<AdfFile> files;
        for (usize i = 0; i < same.size(); ++i) {
            files.push_back({same[i], pattern(100 + i, static_cast<u32>(i))});
        }
        const auto image = build_adf("Work", ffs, files);
        AdfVolume v = mount_ok(image);
        for (usize i = 0; i < same.size(); ++i) {
            DirEntry e{};
            CHECK_EQ(adf_find(v, same[i].c_str(), e), FsError::Ok);
            CHECK(std::strcmp(e.name, same[i].c_str()) == 0);
            CHECK(read_all(v, e) == pattern(100 + i, static_cast<u32>(i)));
        }
        Listing l;
        CHECK_EQ(adf_list(v, "", collect, &l), FsError::Ok);
        CHECK_EQ(l.entries.size(), 3u);
        DirEntry e{};
        std::string missing = same[0] + "x";
        CHECK_EQ(adf_find(v, missing.c_str(), e), FsError::NotFound);
    }
}

void bootblock_checksum_bad_still_mounts() {
    auto image = build_adf("Work", true, sample_files());
    image[100] ^= 0x55;
    AdfVolume v{};
    CHECK_EQ(adf_mount(image.data(), image.size(), v), FsError::Ok);
    CHECK(!v.bootblock_checksum_ok);
    CHECK(v.ffs);
    DirEntry e{};
    CHECK_EQ(adf_find(v, "S/Startup-Sequence", e), FsError::Ok);
}

void bootblock_checksum_known_values() {
    // "DOS\1" and nothing else adds up to 0x444F5301, so the stored value is
    // its complement.
    auto image = build_adf("Work", true, {});
    std::fill(image.begin() + 4, image.begin() + 1024, u8{0});
    image[3] = 1;
    set32(image, 0, 1, 0xBBB0ACFEu);
    AdfVolume v{};
    CHECK_EQ(adf_mount(image.data(), image.size(), v), FsError::Ok);
    CHECK(v.bootblock_checksum_ok);
    // A second long that overflows the sum brings the carry round.
    set32(image, 0, 2, 0xFFFFFFFFu);
    CHECK_EQ(adf_mount(image.data(), image.size(), v), FsError::Ok);
    CHECK(v.bootblock_checksum_ok);
    // The second block counts too.
    set32(image, 0, 2, 0);
    set32(image, 0, 200, 0x00000010u);
    CHECK_EQ(adf_mount(image.data(), image.size(), v), FsError::Ok);
    CHECK(!v.bootblock_checksum_ok);
    set32(image, 0, 1, 0xBBB0ACEEu);
    CHECK_EQ(adf_mount(image.data(), image.size(), v), FsError::Ok);
    CHECK(v.bootblock_checksum_ok);
    // A bootblock that was never written has no checksum.
    std::fill(image.begin() + 4, image.begin() + 1024, u8{0});
    CHECK_EQ(adf_mount(image.data(), image.size(), v), FsError::Ok);
    CHECK(!v.bootblock_checksum_ok);
}

void dircache_disks_mount_through_the_hash_table() {
    // DOS\4 and DOS\5 only add a directory cache on top of the hash table.
    for (const u8 flag : {u8{4}, u8{5}}) {
        auto image = build_adf("Work", true, sample_files());
        image[3] = flag;
        AdfVolume v{};
        CHECK_EQ(adf_mount(image.data(), image.size(), v), FsError::Ok);
        CHECK_EQ(v.ffs, flag == 5);
        CHECK(v.intl);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "s/startup-sequence", e), FsError::Ok);
    }
}

void mount_errors() {
    const auto good = build_adf("Work", true, sample_files());
    AdfVolume v{};

    {  // not a DOS disk
        std::vector<u8> zero(kDisk, 0);
        CHECK_EQ(adf_mount(zero.data(), zero.size(), v), FsError::NotDos);
        auto ndos = good;
        ndos[0] = 'N';
        ndos[1] = 'D';
        ndos[2] = 'O';
        ndos[3] = 'S';
        CHECK_EQ(adf_mount(ndos.data(), ndos.size(), v), FsError::NotDos);
    }
    {  // file systems this reader does not know
        for (const u8 flag : {u8{6}, u8{7}, u8{8}, u8{0xFF}}) {
            auto img = good;
            img[3] = flag;
            CHECK_EQ(adf_mount(img.data(), img.size(), v), FsError::UnsupportedFs);
        }
    }
    {  // wrong image size
        CHECK_EQ(adf_mount(good.data(), good.size() - 1, v), FsError::ImageSize);
        CHECK_EQ(adf_mount(good.data(), 0, v), FsError::ImageSize);
        CHECK_EQ(adf_mount(good.data(), 512, v), FsError::ImageSize);
        std::vector<u8> big(kDisk + kBlock, 0);
        CHECK_EQ(adf_mount(big.data(), big.size(), v), FsError::ImageSize);
    }
    {  // a damaged root block, anywhere in it
        for (const usize at : {usize{0}, usize{13}, usize{100}, usize{300}, usize{432}, usize{511}}) {
            auto img = good;
            img[880 * kBlock + at] ^= 0x01;
            CHECK_EQ(adf_mount(img.data(), img.size(), v), FsError::BadChecksum);
        }
    }
    {  // a root block with a good checksum but the wrong contents
        auto img = good;
        set32(img, 880, 0, 8);
        adf_fix_checksum(img, 880);
        CHECK_EQ(adf_mount(img.data(), img.size(), v), FsError::BadBlock);
        img = good;
        set32(img, 880, 127, 2);  // a directory, not the root
        adf_fix_checksum(img, 880);
        CHECK_EQ(adf_mount(img.data(), img.size(), v), FsError::BadBlock);
        img = good;
        set32(img, 880, 3, 100);  // hash table size
        adf_fix_checksum(img, 880);
        CHECK_EQ(adf_mount(img.data(), img.size(), v), FsError::BadBlock);
        img = good;
        img[880 * kBlock + 432] = 31;  // volume name length
        adf_fix_checksum(img, 880);
        CHECK_EQ(adf_mount(img.data(), img.size(), v), FsError::BadBlock);
    }
}

void bad_header_checksum_in_a_file() {
    auto image = build_adf("Work", true, sample_files());
    AdfVolume v = mount_ok(image);
    DirEntry e{};
    CHECK_EQ(adf_find(v, "README", e), FsError::Ok);
    const DirEntry file = e;
    image[static_cast<usize>(file.header_block) * kBlock + 200] ^= 0x10;
    CHECK_EQ(adf_find(v, "README", e), FsError::BadChecksum);
    Listing l;
    CHECK_EQ(adf_list(v, "", collect, &l), FsError::BadChecksum);
    u8 buf[4];
    u32 got = 0;
    CHECK_EQ(adf_read(v, file, 0, buf, sizeof(buf), got), FsError::BadChecksum);
}

void bad_block_pointers() {
    const auto good = build_adf("Work", true, {{"f", pattern(2000, 1)}, {"g", pattern(2000, 2)}});
    AdfVolume gv = mount_ok(good);
    DirEntry f{};
    CHECK_EQ(adf_find(gv, "f", f), FsError::Ok);

    // A data pointer far outside the image.
    for (const u32 target : {99999u, 1760u, 0u, 1u, 0xFFFFFFFFu}) {
        auto image = good;
        set32(image, f.header_block, 77, target);  // the first data pointer
        adf_fix_checksum(image, f.header_block);
        AdfVolume v = mount_ok(image);
        std::vector<u8> out(2000);
        u32 got = 0;
        CHECK_EQ(adf_read(v, f, 0, out.data(), 2000, got), FsError::BadBlock);
    }
    // A later data pointer is only reached by a read that gets that far.
    {
        auto image = good;
        set32(image, f.header_block, 76, 99999);
        adf_fix_checksum(image, f.header_block);
        AdfVolume v = mount_ok(image);
        std::vector<u8> out(2000);
        u32 got = 0;
        CHECK_EQ(adf_read(v, f, 0, out.data(), 512, got), FsError::Ok);
        CHECK_EQ(adf_read(v, f, 0, out.data(), 2000, got), FsError::BadBlock);
    }
    // The size claims more data than the header has pointers for.
    {
        auto image = good;
        set32(image, f.header_block, 2, 2);  // two pointers instead of four
        adf_fix_checksum(image, f.header_block);
        AdfVolume v = mount_ok(image);
        std::vector<u8> out(2000);
        u32 got = 0;
        CHECK_EQ(adf_read(v, f, 0, out.data(), 2000, got), FsError::BadBlock);
    }
    // More pointers than a block holds.
    {
        auto image = good;
        set32(image, f.header_block, 2, 73);
        adf_fix_checksum(image, f.header_block);
        AdfVolume v = mount_ok(image);
        std::vector<u8> out(2000);
        u32 got = 0;
        CHECK_EQ(adf_read(v, f, 0, out.data(), 2000, got), FsError::BadBlock);
    }
    // A hash table entry that points outside the image.
    for (const u32 target : {99999u, 1u, 1760u}) {
        auto image = good;
        set32(image, 880, 6 + hash_of("f"), target);
        adf_fix_checksum(image, 880);
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "f", e), FsError::BadBlock);
        Listing l;
        CHECK_EQ(adf_list(v, "", collect, &l), FsError::BadBlock);
    }
    // A hash table entry that points at a block that is not a header.
    {
        auto image = good;
        set32(image, 880, 6 + hash_of("f"), 1500);  // an unused, all-zero block
        adf_fix_checksum(image, 880);
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "f", e), FsError::BadBlock);
    }
    // A header that names a different block as its own.
    {
        auto image = good;
        set32(image, f.header_block, 1, f.header_block + 1);
        adf_fix_checksum(image, f.header_block);
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "f", e), FsError::BadBlock);
    }
    // A file name length that does not fit.
    {
        auto image = good;
        image[static_cast<usize>(f.header_block) * kBlock + 432] = 31;
        adf_fix_checksum(image, f.header_block);
        AdfVolume v = mount_ok(image);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "f", e), FsError::BadBlock);
    }
}

void ofs_data_block_checks() {
    // 2000 bytes: five OFS data blocks, the last one short.
    const auto good = build_adf("Work", false, {{"f", pattern(2000, 1)}, {"g", pattern(900, 2)}});
    AdfVolume gv = mount_ok(good);
    DirEntry f{};
    DirEntry g{};
    CHECK_EQ(adf_find(gv, "f", f), FsError::Ok);
    CHECK_EQ(adf_find(gv, "g", g), FsError::Ok);
    std::vector<u8> out(2000);
    u32 got = 0;
    CHECK_EQ(adf_read(gv, f, 0, out.data(), 2000, got), FsError::Ok);

    // Data block k (from 0) of "f", with the damage applied and the block
    // checksum made good again unless the test is about the checksum.
    auto damaged = [&](u32 k, u32 index, u32 value, bool fix) {
        auto image = good;
        const u32 block = get32(image, f.header_block, 77 - k);
        set32(image, block, index, value);
        if (fix) {
            adf_fix_checksum(image, block);
        }
        return image;
    };
    u32 suspect = 99;
    auto read_f = [&](const std::vector<u8>& image, u32 offset) {
        AdfVolume v = mount_ok(image);
        return adf_read(v, f, offset, out.data(), 2000 - offset, got, &suspect);
    };
    // A clean file reports nothing, and the count is optional.
    CHECK_EQ(read_f(good, 0), FsError::Ok);
    CHECK_EQ(suspect, 0u);
    CHECK_EQ(adf_read(gv, f, 0, out.data(), 2000, got), FsError::Ok);

    for (u32 k = 0; k < 5; ++k) {
        const u32 at = k * 488;
        // A block that is not a data block is refused. Only a read that
        // reaches the block sees it.
        CHECK_EQ(read_f(damaged(k, 0, 2, true), at), FsError::BadBlock);
        CHECK_EQ(read_f(damaged(k, 0, 0, true), at), FsError::BadBlock);
        CHECK_EQ(read_f(damaged(k, 3, 488, true), at), FsError::Ok);
        CHECK_EQ(suspect, 0u);
        CHECK_EQ(read_f(damaged(k, 3, 0, true), at), FsError::Ok);  // a smaller size is not enforced
        CHECK_EQ(suspect, 0u);

        // A wrong checksum, header key, sequence number or data size is
        // counted and the data is still returned.
        CHECK_EQ(read_f(damaged(k, 3, 489, true), at), FsError::Ok);  // size
        CHECK_EQ(suspect, 1u);
        CHECK_EQ(read_f(damaged(k, 3, 0xFFFFFFFFu, true), at), FsError::Ok);
        CHECK_EQ(suspect, 1u);
        CHECK_EQ(read_f(damaged(k, 1, g.header_block, true), at), FsError::Ok);  // key
        CHECK_EQ(suspect, 1u);
        CHECK_EQ(read_f(damaged(k, 2, k + 2, true), at), FsError::Ok);  // sequence
        CHECK_EQ(suspect, 1u);
        CHECK_EQ(read_f(damaged(k, 2, 0, true), at), FsError::Ok);
        CHECK_EQ(suspect, 1u);
        CHECK_EQ(read_f(damaged(k, 5, 0x12345678u, false), at), FsError::Ok);  // checksum
        CHECK_EQ(suspect, 1u);
        CHECK(std::equal(out.begin(), out.begin() + std::min<u32>(got, 488),
                         pattern(2000, 1).begin() + at));
        auto flipped = good;
        const u32 block = get32(flipped, f.header_block, 77 - k);
        flipped[static_cast<usize>(block) * kBlock + 300] ^= 0x40;  // a data byte
        CHECK_EQ(read_f(flipped, at), FsError::Ok);
        CHECK_EQ(suspect, 1u);
        // Blocks before the damaged one are not counted.
        CHECK_EQ(read_f(damaged(k, 2, 0, true), 0), FsError::Ok);
        CHECK_EQ(suspect, 1u);
        if (k > 0) {
            const auto image = damaged(k, 2, 0, true);
            AdfVolume v = mount_ok(image);
            CHECK_EQ(adf_read(v, f, 0, out.data(), at, got, &suspect), FsError::Ok);  // stops before it
            CHECK_EQ(suspect, 0u);
        }
    }

    // Two blocks of one file swapped: every block is fine alone, but both
    // are in the wrong place, which the count shows.
    {
        auto image = good;
        const u32 a = get32(image, f.header_block, 77);
        const u32 b = get32(image, f.header_block, 76);
        set32(image, f.header_block, 77, b);
        set32(image, f.header_block, 76, a);
        adf_fix_checksum(image, f.header_block);
        CHECK_EQ(read_f(image, 0), FsError::Ok);
        CHECK_EQ(suspect, 2u);
        CHECK_EQ(read_f(image, 488), FsError::Ok);
        CHECK_EQ(suspect, 1u);
        CHECK_EQ(read_f(image, 976), FsError::Ok);
        CHECK_EQ(suspect, 0u);
    }
    // A block of another file in the pointer table: wrong key, wrong place.
    {
        auto image = good;
        set32(image, f.header_block, 77, get32(image, g.header_block, 77));
        adf_fix_checksum(image, f.header_block);
        CHECK_EQ(read_f(image, 0), FsError::Ok);
        CHECK_EQ(suspect, 1u);
    }
    // FFS has none of these fields: its blocks are all data.
    {
        const auto ffs = build_adf("Work", true, {{"f", pattern(2000, 1)}});
        AdfVolume v = mount_ok(ffs);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "f", e), FsError::Ok);
        CHECK_EQ(adf_read(v, e, 0, out.data(), 2000, got), FsError::Ok);
    }
}

void hash_chain_loop() {
    auto image = build_adf("Work", true, {{"aaa", text("1")}});
    AdfVolume v = mount_ok(image);
    DirEntry a{};
    CHECK_EQ(adf_find(v, "aaa", a), FsError::Ok);
    // The entry's hash chain successor is the entry itself.
    set32(image, a.header_block, 124, a.header_block);
    adf_fix_checksum(image, a.header_block);

    DirEntry e{};
    CHECK_EQ(adf_find(v, "aaa", e), FsError::Ok);  // found before the chain is walked on
    // A name in the same bucket that does not exist walks the chain.
    std::string other;
    for (u32 i = 0; other.empty(); ++i) {
        const std::string n = "x" + std::to_string(i);
        if (hash_of(n) == hash_of("aaa") && n.size() == 3) {
            other = n;
        }
    }
    CHECK_EQ(adf_find(v, other.c_str(), e), FsError::Loop);
    Listing l;
    CHECK_EQ(adf_list(v, "", collect, &l), FsError::Loop);
    CHECK(l.entries.empty());  // nothing is reported from a chain that never ends
}

void hash_chain_loop_of_two() {
    std::vector<std::string> same;
    for (u32 i = 0; same.size() < 2 && i < 100000; ++i) {
        const std::string n = "N" + std::to_string(i);
        if (hash_of(n) == 5) {
            same.push_back(n);
        }
    }
    auto image = build_adf("Work", true, {{same[0], text("1")}, {same[1], text("2")}});
    AdfVolume v = mount_ok(image);
    DirEntry first{};
    DirEntry second{};
    CHECK_EQ(adf_find(v, same[0].c_str(), first), FsError::Ok);
    CHECK_EQ(adf_find(v, same[1].c_str(), second), FsError::Ok);
    // Whichever is the head, point the tail back at the head.
    const u32 head = get32(image, 880, 6 + 5);
    const u32 tail = (head == first.header_block) ? second.header_block : first.header_block;
    set32(image, tail, 124, head);
    adf_fix_checksum(image, tail);
    DirEntry e{};
    CHECK_EQ(adf_find(v, "nothere", e), FsError::NotFound);  // another bucket
    Listing l;
    CHECK_EQ(adf_list(v, "", collect, &l), FsError::Loop);
}

void extension_chain_loop() {
    for (const bool ffs : {false, true}) {
        // 300 blocks of 512 bytes: a header and several extension blocks.
        const auto good = build_adf("Work", ffs, {{"big", pattern(300 * 512, 4)}});
        AdfVolume gv = mount_ok(good);
        DirEntry e{};
        CHECK_EQ(adf_find(gv, "big", e), FsError::Ok);
        const u32 ext1 = get32(good, e.header_block, 126);
        const u32 ext2 = get32(good, ext1, 126);
        CHECK(ext1 >= 2 && ext1 < 1760);
        CHECK(ext2 >= 2 && ext2 < 1760 && ext2 != ext1);
        std::vector<u8> out(e.size);
        u32 got = 0;
        CHECK_EQ(adf_read(gv, e, 0, out.data(), e.size, got), FsError::Ok);

        // An extension block that names itself as its successor.
        auto image = good;
        set32(image, ext1, 126, ext1);
        adf_fix_checksum(image, ext1);
        AdfVolume v = mount_ok(image);
        CHECK_EQ(adf_read(v, e, 0, out.data(), e.size, got), FsError::Loop);

        // Two extension blocks that name each other.
        image = good;
        set32(image, ext2, 126, ext1);
        adf_fix_checksum(image, ext2);
        AdfVolume w = mount_ok(image);
        CHECK_EQ(adf_read(w, e, 0, out.data(), e.size, got), FsError::Loop);

        // A far offset reaches the loop by skipping, not by reading, and
        // never takes more than block_count steps to give up.
        set32(image, e.header_block, 81, 0xF0000000u);
        adf_fix_checksum(image, e.header_block);
        AdfVolume x = mount_ok(image);
        DirEntry huge{};
        CHECK_EQ(adf_find(x, "big", huge), FsError::Ok);
        CHECK_EQ(adf_read(x, huge, 0xE0000000u, out.data(), 100, got), FsError::Loop);

        // An extension pointer outside the image, or one that is not an
        // extension block.
        image = good;
        set32(image, e.header_block, 126, 99999);
        adf_fix_checksum(image, e.header_block);
        AdfVolume y = mount_ok(image);
        CHECK_EQ(adf_read(y, e, 0, out.data(), e.size, got), FsError::BadBlock);
        image = good;
        set32(image, e.header_block, 126, e.header_block);  // a header is not a list
        adf_fix_checksum(image, e.header_block);
        AdfVolume z = mount_ok(image);
        CHECK_EQ(adf_read(z, e, 0, out.data(), e.size, got), FsError::BadBlock);
        // A damaged extension block.
        image = good;
        image[static_cast<usize>(ext1) * kBlock + 300] ^= 1;
        AdfVolume q = mount_ok(image);
        CHECK_EQ(adf_read(q, e, 0, out.data(), e.size, got), FsError::BadChecksum);
    }
}

void name_too_long() {
    const auto image = build_adf("Work", true, sample_files());
    AdfVolume v = mount_ok(image);
    DirEntry e{};
    const std::string ok30(30, 'a');
    const std::string bad31(31, 'a');
    CHECK_EQ(adf_find(v, ok30.c_str(), e), FsError::NotFound);
    CHECK_EQ(adf_find(v, bad31.c_str(), e), FsError::NameTooLong);
    CHECK_EQ(adf_find(v, ("S/" + bad31).c_str(), e), FsError::NameTooLong);
    CHECK_EQ(adf_find(v, (bad31 + "/x").c_str(), e), FsError::NameTooLong);
    Listing l;
    CHECK_EQ(adf_list(v, bad31.c_str(), collect, &l), FsError::NameTooLong);
}

void thirty_character_file_names_work() {
    const std::string name(30, 'q');
    const auto image = build_adf("Work", true, {{name, text("long")}});
    AdfVolume v = mount_ok(image);
    DirEntry e{};
    CHECK_EQ(adf_find(v, name.c_str(), e), FsError::Ok);
    CHECK(std::strcmp(e.name, name.c_str()) == 0);
    CHECK(read_all(v, e) == text("long"));
}

void detect_format() {
    const u8 dms[] = {'D', 'M', 'S', '!', 0, 0, 0, 0};
    CHECK(detect_disk_format(dms, sizeof(dms)) == DiskFormat::Dms);
    CHECK(detect_disk_format(dms, 4) == DiskFormat::Dms);
    CHECK(detect_disk_format(dms, 3) == DiskFormat::Unknown);
    std::vector<u8> dd(901120, 0);
    CHECK(detect_disk_format(dd.data(), dd.size()) == DiskFormat::Adf);
    std::vector<u8> hd(1802240, 0);
    CHECK(detect_disk_format(hd.data(), hd.size()) == DiskFormat::Adf);
    std::vector<u8> small(100, 0);
    CHECK(detect_disk_format(small.data(), small.size()) == DiskFormat::Unknown);
    CHECK(detect_disk_format(dd.data(), dd.size() - 1) == DiskFormat::Unknown);
    CHECK(detect_disk_format(dd.data(), 0) == DiskFormat::Unknown);
    // A DMS archive that happens to have a disk's size is still an archive.
    std::vector<u8> sized(901120, 0);
    sized[0] = 'D';
    sized[1] = 'M';
    sized[2] = 'S';
    sized[3] = '!';
    CHECK(detect_disk_format(sized.data(), sized.size()) == DiskFormat::Dms);
}

void error_names() {
    CHECK(std::strcmp(fs_error_name(FsError::Ok), "ok") == 0);
    const FsError all[] = {FsError::Ok,       FsError::ImageSize, FsError::NotDos,
                           FsError::UnsupportedFs, FsError::BadChecksum, FsError::BadBlock,
                           FsError::NotFound, FsError::NotFile,   FsError::NotDir,
                           FsError::Loop,     FsError::NameTooLong};
    for (usize i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        CHECK(fs_error_name(all[i])[0] != '\0');
        for (usize j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
            CHECK(std::strcmp(fs_error_name(all[i]), fs_error_name(all[j])) != 0);
        }
    }
    CHECK(std::strcmp(fs_error_name(static_cast<FsError>(999)), "unknown") == 0);
}

void random_damage_never_escapes() {
    // Flip bytes all over a disk with a rich directory tree. Whatever the
    // result, every call returns and the sanitizers see no stray access.
    std::vector<AdfFile> files = sample_files();
    files.push_back({"big", pattern(100 * 512, 4)});
    files.push_back({"D1/D2/D3/f", pattern(900, 2)});
    for (const bool ffs : {false, true}) {
        const auto good = build_adf("Work", ffs, files);
        u32 x = 99;
        for (u32 round = 0; round < 400; ++round) {
            auto image = good;
            for (u32 k = 0; k < 6; ++k) {
                x = x * 1664525u + 1013904223u;
                const u32 block = (k % 2 == 0) ? 880 + (x >> 20) % 16 : (x >> 8) % 1760;
                x = x * 1664525u + 1013904223u;
                image[static_cast<usize>(block) * kBlock + (x >> 16) % kBlock] = static_cast<u8>(x);
            }
            AdfVolume v{};
            if (adf_mount(image.data(), image.size(), v) != FsError::Ok) {
                continue;
            }
            Listing l;
            (void)adf_list(v, "", collect, &l);
            for (const char* path : {"S/Startup-Sequence", "big", "D1/D2/D3/f", "Devs/Keymaps/usa"}) {
                DirEntry e{};
                if (adf_find(v, path, e) == FsError::Ok) {
                    std::vector<u8> out(e.size + 1);
                    u32 got = 0;
                    (void)adf_read(v, e, 0, out.data(), e.size, got);
                    CHECK(got <= e.size);
                }
            }
            for (const DirEntry& e : l.entries) {
                u8 buf[600];
                u32 got = 0;
                (void)adf_read(v, e, 0, buf, sizeof(buf), got);
                Listing sub;
                if (e.is_dir) {
                    (void)adf_list(v, e.name, collect, &sub);
                }
            }
        }
    }
}

}

int main() {
    mount_ofs_and_ffs();
    list_root_and_subdir();
    empty_directory_lists_nothing();
    find_path_variants();
    intl_folds_latin1();
    not_found();
    not_file();
    not_dir();
    read_small_file();
    read_partial_with_offset();
    read_large_file_with_extension_block();
    read_capacity_smaller_than_file();
    hash_collisions_chain();
    bootblock_checksum_bad_still_mounts();
    bootblock_checksum_known_values();
    dircache_disks_mount_through_the_hash_table();
    mount_errors();
    bad_header_checksum_in_a_file();
    bad_block_pointers();
    ofs_data_block_checks();
    hash_chain_loop();
    hash_chain_loop_of_two();
    extension_chain_loop();
    name_too_long();
    thirty_character_file_names_work();
    detect_format();
    error_names();
    random_damage_never_escapes();
    return check::finish("test_amiga_fs");
}
