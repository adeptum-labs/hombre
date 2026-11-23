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
#include "amiga_fs_write.hpp"
#include "check.hpp"
#include "fs_check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace hombre;

namespace {

namespace layout = amiga_fs_write_detail;

constexpr u32 kBlock = 512;
constexpr u32 kFirst = 16;  // the device block each test volume starts at
constexpr u32 kReserved = 2;
constexpr u32 kFloppy = 1760;
constexpr AmigaDate kFormatDay{100, 10, 1};
constexpr AmigaDate kToday{200, 20, 2};

// A device held in memory; a block never written reads as zeros.
struct MemoryDevice {
    u32 blocks = 0;
    std::map<u32, std::vector<u8>> data;
    u32 writes = 0;
};

bool read_memory(void* ctx, u32 block, u8* out) {
    const MemoryDevice& d = *static_cast<const MemoryDevice*>(ctx);
    if (block >= d.blocks) {
        return false;
    }
    const auto it = d.data.find(block);
    if (it == d.data.end()) {
        std::memset(out, 0, kBlock);
    } else {
        std::memcpy(out, it->second.data(), kBlock);
    }
    return true;
}

bool write_memory(void* ctx, u32 block, const u8* data) {
    MemoryDevice& d = *static_cast<MemoryDevice*>(ctx);
    if (block >= d.blocks) {
        return false;
    }
    d.data[block].assign(data, data + kBlock);
    ++d.writes;
    return true;
}

// A volume formatted on a memory device from block kFirst on and mounted
// through it. The volume points at the device, so a disk stays where it is.
struct Disk {
    MemoryDevice device;
    AdfVolume v{};

    explicit Disk(u8 dos_type, u32 blocks = kFloppy) {
        device.blocks = kFirst + blocks;
        CHECK_EQ(adf_format(source(), kFirst, blocks, kReserved, dos_type, "Work", kFormatDay),
                 FsError::Ok);
        CHECK_EQ(adf_mount_partition(source(), kFirst, blocks, kReserved, v), FsError::Ok);
    }
    Disk(const Disk&) = delete;
    Disk& operator=(const Disk&) = delete;

    BlockSource source() { return BlockSource{&device, read_memory, write_memory}; }

    std::vector<u8> raw(u32 number) {
        std::vector<u8> block(kBlock);
        CHECK(read_memory(&device, kFirst + number, block.data()));
        return block;
    }

    // Sets a long of a volume block and the checksum long `sum` to match.
    void patch(u32 number, u32 index, u32 value, u32 sum) {
        std::vector<u8> block = raw(number);
        layout::set_long(block.data(), index, value);
        layout::seal(block.data(), sum);
        CHECK(write_memory(&device, kFirst + number, block.data()));
    }

    // The whole volume as a floppy image.
    std::vector<u8> image() {
        std::vector<u8> bytes;
        for (u32 number = 0; number < v.block_count; ++number) {
            const std::vector<u8> block = raw(number);
            bytes.insert(bytes.end(), block.begin(), block.end());
        }
        return bytes;
    }
};

u32 at(const std::vector<u8>& block, u32 index) { return layout::long_at(block.data(), index); }

bool dated(const std::vector<u8>& block, u32 index, const AmigaDate& date) {
    return at(block, index) == date.days && at(block, index + 1) == date.minutes &&
           at(block, index + 2) == date.ticks;
}

bool marked_free(const std::vector<u8>& page, u32 block) {
    const u32 bit = block - kReserved;
    return (at(page, 1 + bit / 32) & (1u << (bit % 32))) != 0;
}

void expect_consistent(const AdfVolume& v, int line) {
    const std::vector<std::string> problems = fs_check(v);
    for (const std::string& problem : problems) {
        std::fprintf(stderr, "line %d: %s\n", line, problem.c_str());
    }
    CHECK(problems.empty());
}

#define CHECK_CONSISTENT(v) expect_consistent((v), __LINE__)

std::vector<u8> text(const char* s) {
    const u8* p = reinterpret_cast<const u8*>(s);
    return std::vector<u8>(p, p + std::strlen(s));
}

// Deterministic bytes that differ from block to block.
std::vector<u8> pattern(usize length, u32 seed) {
    std::vector<u8> bytes(length);
    u32 x = seed * 2654435761u + 12345u;
    for (usize i = 0; i < length; ++i) {
        x = x * 1664525u + 1013904223u;
        bytes[i] = static_cast<u8>(x >> 24);
    }
    return bytes;
}

u32 payload(const AdfVolume& v) { return v.ffs ? 512 : 488; }

// The blocks a file of `size` bytes takes: header, data and extension blocks.
u32 file_blocks(const AdfVolume& v, usize size) {
    const u32 data = static_cast<u32>((size + payload(v) - 1) / payload(v));
    return 1 + data + (data > 72 ? (data - 1) / 72 : 0);
}

DirEntry create_file(const AdfVolume& v, u32 dir, const char* name) {
    DirEntry entry{};
    CHECK_EQ(adf_create_file(v, dir, name, kToday, entry), FsError::Ok);
    return entry;
}

DirEntry create_dir(const AdfVolume& v, u32 dir, const char* name) {
    DirEntry entry{};
    CHECK_EQ(adf_create_dir(v, dir, name, kToday, entry), FsError::Ok);
    return entry;
}

void write_all(const AdfVolume& v, u32 file, usize offset, const std::vector<u8>& data) {
    u32 written = 0xDEADu;
    CHECK_EQ(adf_write(v, file, static_cast<u32>(offset), data.data(),
                       static_cast<u32>(data.size()), kToday, written),
             FsError::Ok);
    CHECK_EQ(written, static_cast<u32>(data.size()));
}

// The file read back through the reader in pieces, no OFS block suspect.
std::vector<u8> contents(const AdfVolume& v, u32 file) {
    DirEntry entry{};
    entry.header_block = file;
    std::vector<u8> out;
    for (;;) {
        u8 buf[5000];
        u32 got = 0;
        u32 suspect = 99;
        CHECK_EQ(adf_read(v, entry, static_cast<u32>(out.size()), buf, sizeof(buf), got, &suspect),
                 FsError::Ok);
        CHECK_EQ(suspect, 0u);
        out.insert(out.end(), buf, buf + got);
        if (got < sizeof(buf)) {
            return out;
        }
    }
}

DirEntry lookup(const AdfVolume& v, const std::string& path) {
    DirEntry entry{};
    CHECK_EQ(adf_find(v, path.c_str(), entry), FsError::Ok);
    return entry;
}

void collect(void* ctx, const DirEntry& entry) {
    static_cast<std::vector<std::string>*>(ctx)->push_back(entry.name);
}

std::vector<std::string> names_in(const AdfVolume& v, const std::string& path) {
    std::vector<std::string> names;
    CHECK_EQ(adf_list(v, path.c_str(), collect, &names), FsError::Ok);
    std::sort(names.begin(), names.end());
    return names;
}

using Names = std::vector<std::string>;

void format_every_type() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        CHECK_EQ(v.dos_type, type);
        CHECK_EQ(v.ffs, (type & 1) != 0);
        CHECK_EQ(v.intl, (type & 2) != 0);
        CHECK(std::strcmp(v.name, "Work") == 0);
        CHECK_EQ(v.root_block, 880u);
        CHECK_EQ(adf_used_blocks(v), 2u);
        CHECK(names_in(v, "").empty());
        CHECK_CONSISTENT(v);

        const std::vector<u8> boot = d.raw(0);
        CHECK(boot[0] == 'D' && boot[1] == 'O' && boot[2] == 'S' && boot[3] == type);
        CHECK(std::all_of(boot.begin() + 4, boot.end(), [](u8 b) { return b == 0; }));
        const std::vector<u8> second = d.raw(1);
        CHECK(std::all_of(second.begin(), second.end(), [](u8 b) { return b == 0; }));

        const std::vector<u8> root = d.raw(880);
        CHECK_EQ(at(root, layout::l_hash_size), 72u);
        CHECK_EQ(at(root, layout::l_bm_flag), layout::bm_valid);
        CHECK_EQ(at(root, layout::l_bm_pages), 881u);
        CHECK_EQ(at(root, layout::l_bm_pages + 1), 0u);
        CHECK_EQ(at(root, layout::l_bm_ext), 0u);
        CHECK(dated(root, layout::l_date, kFormatDay));
        CHECK(dated(root, layout::l_volume_date, kFormatDay));
        CHECK(dated(root, layout::l_creation_date, kFormatDay));

        const std::vector<u8> page = d.raw(881);
        CHECK(marked_free(page, 2));
        CHECK(marked_free(page, 879));
        CHECK(!marked_free(page, 880));
        CHECK(!marked_free(page, 881));
        CHECK(marked_free(page, 882));
        CHECK(marked_free(page, 1759));
        // The bits past the end of the volume are left clear.
        CHECK(!marked_free(page, 1760));
    }
}

void format_refusals() {
    MemoryDevice device;
    device.blocks = 2000;
    const BlockSource source{&device, read_memory, write_memory};
    const BlockSource read_only{&device, read_memory, nullptr};
    CHECK_EQ(adf_format(read_only, 0, kFloppy, 2, 1, "W", kToday), FsError::ReadOnly);
    CHECK_EQ(adf_format(source, 0, kFloppy, 2, 4, "W", kToday), FsError::UnsupportedFs);
    CHECK_EQ(adf_format(source, 0, kFloppy, 2, 5, "W", kToday), FsError::UnsupportedFs);
    CHECK_EQ(adf_format(source, 0, kFloppy, 2, 1, "", kToday), FsError::BadName);
    CHECK_EQ(adf_format(source, 0, kFloppy, 2, 1, "a:b", kToday), FsError::BadName);
    CHECK_EQ(adf_format(source, 0, kFloppy, 2, 1, "0123456789012345678901234567890", kToday),
             FsError::BadName);
    CHECK_EQ(adf_format(source, 0, kFloppy, 0, 1, "W", kToday), FsError::BadBlock);
    // Three blocks leave no room for a bitmap page after the root.
    CHECK_EQ(adf_format(source, 0, 3, 2, 1, "W", kToday), FsError::BadBlock);
    CHECK_EQ(device.writes, 0u);
    // A device that ends before the volume does.
    device.blocks = 100;
    CHECK_EQ(adf_format(source, 0, kFloppy, 2, 1, "W", kToday), FsError::Io);
}

void format_large_volume_with_bitmap_extensions() {
    constexpr u32 kBlocks = 700000;
    Disk d(1, kBlocks);
    const AdfVolume& v = d.v;
    const u32 pages = (kBlocks - kReserved - 1) / (127 * 32) + 1;
    CHECK_EQ(pages, 173u);  // 25 in the root, 127 in one extension block, 21 in a second
    const u32 first_page = v.root_block + 1;
    const u32 first_extension = first_page + pages;
    CHECK_EQ(adf_used_blocks(v), 1 + pages + 2);
    CHECK_EQ(at(d.raw(v.root_block), layout::l_bm_pages + 24), first_page + 24);
    CHECK_EQ(at(d.raw(v.root_block), layout::l_bm_ext), first_extension);
    const std::vector<u8> extension = d.raw(first_extension);
    CHECK_EQ(at(extension, 0), first_page + 25);
    CHECK_EQ(at(extension, 126), first_page + 151);
    CHECK_EQ(at(extension, 127), first_extension + 1);
    const std::vector<u8> last = d.raw(first_extension + 1);
    CHECK_EQ(at(last, 0), first_page + 152);
    CHECK_EQ(at(last, 20), first_page + 172);
    CHECK_EQ(at(last, 21), 0u);
    CHECK_EQ(at(last, 127), 0u);
    CHECK_CONSISTENT(v);

    // The root's page is reached through an extension block, and the file
    // takes the blocks right after the bitmap.
    const DirEntry file = create_file(v, v.root_block, "big");
    CHECK_EQ(file.header_block, first_extension + 2);
    const std::vector<u8> data = pattern(100000, 3);
    write_all(v, file.header_block, 0, data);
    CHECK(contents(v, file.header_block) == data);
    CHECK_EQ(adf_used_blocks(v), 1 + pages + 2 + file_blocks(v, data.size()));
    CHECK_CONSISTENT(v);
}

void floppy_image_mounts_after_format() {
    MemoryDevice device;
    device.blocks = kFloppy;
    const BlockSource source{&device, read_memory, write_memory};
    CHECK_EQ(adf_format(source, 0, kFloppy, 2, 0, "Empty", kFormatDay), FsError::Ok);
    AdfVolume part{};
    CHECK_EQ(adf_mount_partition(source, 0, kFloppy, 2, part), FsError::Ok);
    const DirEntry file = create_file(part, part.root_block, "hello");
    write_all(part, file.header_block, 0, text("Hello, floppy\n"));

    std::vector<u8> image(static_cast<usize>(kFloppy) * kBlock, 0);
    for (const auto& [number, data] : device.data) {
        std::copy(data.begin(), data.end(), image.begin() + static_cast<long>(number) * kBlock);
    }
    AdfVolume v{};
    CHECK_EQ(adf_mount(image.data(), image.size(), v), FsError::Ok);
    CHECK(std::strcmp(v.name, "Empty") == 0);
    CHECK_EQ(v.root_block, 880u);
    CHECK(!v.ffs);
    CHECK_EQ(adf_used_blocks(v), 4u);
    CHECK(names_in(v, "") == Names{"hello"});
    CHECK(contents(v, lookup(v, "Empty:HELLO").header_block) == text("Hello, floppy\n"));
    CHECK_CONSISTENT(v);
}

// Every change refused, and nothing written.
void expect_read_only(const AdfVolume& v, MemoryDevice& device, u32 file, u32 dir) {
    const u32 writes = device.writes;
    DirEntry entry{};
    u32 written = 7;
    const u8 byte = 1;
    CHECK_EQ(adf_create_file(v, v.root_block, "new", kToday, entry), FsError::ReadOnly);
    CHECK_EQ(adf_create_dir(v, v.root_block, "newdir", kToday, entry), FsError::ReadOnly);
    CHECK_EQ(adf_write(v, file, 0, &byte, 1, kToday, written), FsError::ReadOnly);
    CHECK_EQ(written, 0u);
    CHECK_EQ(adf_set_size(v, file, 0, kToday), FsError::ReadOnly);
    CHECK_EQ(adf_delete(v, file, kToday), FsError::ReadOnly);
    CHECK_EQ(adf_rename(v, file, dir, "moved", kToday), FsError::ReadOnly);
    CHECK_EQ(adf_set_protection(v, file, 0xF, kToday), FsError::ReadOnly);
    CHECK_EQ(adf_set_comment(v, file, "note", kToday), FsError::ReadOnly);
    CHECK_EQ(adf_set_date(v, file, kFormatDay, kToday), FsError::ReadOnly);
    CHECK_EQ(device.writes, writes);
}

void read_only_volumes() {
    Disk d(1);
    const u32 dir = create_dir(d.v, d.v.root_block, "Dir").header_block;
    const u32 file = create_file(d.v, d.v.root_block, "File").header_block;
    write_all(d.v, file, 0, pattern(3000, 1));

    const std::vector<u8> image = d.image();
    AdfVolume held{};
    CHECK_EQ(adf_mount(image.data(), image.size(), held), FsError::Ok);
    expect_read_only(held, d.device, file, dir);

    AdfVolume no_write = d.v;
    no_write.source.write = nullptr;
    expect_read_only(no_write, d.device, file, dir);

    for (const u8 type : {u8{4}, u8{5}}) {
        d.device.data[kFirst][3] = type;
        AdfVolume cache{};
        CHECK_EQ(adf_mount_partition(d.source(), kFirst, kFloppy, kReserved, cache), FsError::Ok);
        CHECK_EQ(cache.dos_type, type);
        expect_read_only(cache, d.device, file, dir);
    }
    d.device.data[kFirst][3] = 1;

    // A bitmap not marked valid, as a volume that needs validating has.
    d.patch(d.v.root_block, layout::l_bm_flag, 0, layout::l_checksum);
    expect_read_only(d.v, d.device, file, dir);
    d.patch(d.v.root_block, layout::l_bm_flag, layout::bm_valid, layout::l_checksum);
    create_file(d.v, dir, "writable again");
    CHECK(contents(d.v, file) == pattern(3000, 1));
    CHECK_CONSISTENT(d.v);
}

void create_files_and_dirs() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 root = v.root_block;
        const AmigaDate made{300, 30, 3};
        DirEntry s{};
        CHECK_EQ(adf_create_dir(v, root, "S", made, s), FsError::Ok);
        CHECK(s.is_dir);
        CHECK(std::strcmp(s.name, "S") == 0);
        CHECK_EQ(s.size, 0u);
        // The first free block from the directory up.
        CHECK_EQ(s.header_block, 882u);
        CHECK(dated(d.raw(s.header_block), layout::l_date, made));
        CHECK(dated(d.raw(root), layout::l_date, made));
        CHECK(dated(d.raw(root), layout::l_volume_date, made));
        CHECK(dated(d.raw(root), layout::l_creation_date, kFormatDay));

        const DirEntry startup = create_file(v, s.header_block, "Startup-Sequence");
        CHECK(!startup.is_dir);
        CHECK_EQ(startup.header_block, 883u);
        CHECK(dated(d.raw(s.header_block), layout::l_date, kToday));
        CHECK(dated(d.raw(root), layout::l_date, made));
        CHECK(dated(d.raw(root), layout::l_volume_date, kToday));
        const std::vector<u8> header = d.raw(startup.header_block);
        CHECK_EQ(at(header, layout::l_type), layout::t_header);
        CHECK_EQ(at(header, layout::l_own), startup.header_block);
        CHECK_EQ(at(header, layout::l_parent), s.header_block);
        CHECK_EQ(at(header, layout::l_sec_type), layout::st_file);
        CHECK_EQ(at(header, layout::l_size), 0u);
        CHECK(dated(header, layout::l_date, kToday));

        const DirEntry readme = create_file(v, root, "README");
        CHECK(names_in(v, "") == (Names{"README", "S"}));
        CHECK(names_in(v, "S") == Names{"Startup-Sequence"});
        CHECK_EQ(lookup(v, "s/startup-sequence").header_block, startup.header_block);
        CHECK_EQ(lookup(v, "Work:Readme").header_block, readme.header_block);

        DirEntry e{};
        CHECK_EQ(adf_create_file(v, root, "readme", kToday, e), FsError::Exists);
        CHECK_EQ(adf_create_dir(v, root, "README", kToday, e), FsError::Exists);
        CHECK_EQ(adf_create_file(v, root, "s", kToday, e), FsError::Exists);
        CHECK_EQ(adf_create_file(v, root, "", kToday, e), FsError::BadName);
        CHECK_EQ(adf_create_file(v, root, "a:b", kToday, e), FsError::BadName);
        CHECK_EQ(adf_create_dir(v, root, "a/b", kToday, e), FsError::BadName);
        CHECK_EQ(adf_create_file(v, root, "0123456789012345678901234567890", kToday, e),
                 FsError::BadName);
        const DirEntry longest = create_file(v, root, "012345678901234567890123456789");
        CHECK(std::strcmp(longest.name, "012345678901234567890123456789") == 0);
        CHECK_EQ(adf_create_file(v, readme.header_block, "x", kToday, e), FsError::NotDir);
        CHECK_EQ(adf_create_file(v, 5000, "x", kToday, e), FsError::BadBlock);

        // INTL folds the Latin-1 letters, so these are one name there.
        create_file(v, root, "\xE4pple");
        CHECK_EQ(adf_create_file(v, root, "\xC4PPLE", kToday, e),
                 v.intl ? FsError::Exists : FsError::Ok);
        CHECK_EQ(adf_used_blocks(v), v.intl ? 7u : 8u);
        CHECK_CONSISTENT(v);
    }
}

void write_small_files() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 file = create_file(v, v.root_block, "f").header_block;
        CHECK(contents(v, file).empty());
        u32 written = 9;
        CHECK_EQ(adf_write(v, file, 0, nullptr, 0, kToday, written), FsError::Ok);
        CHECK_EQ(written, 0u);
        CHECK_EQ(lookup(v, "f").size, 0u);

        const AmigaDate later{400, 40, 4};
        CHECK_EQ(adf_write(v, file, 0, text("hello").data(), 5, later, written), FsError::Ok);
        CHECK_EQ(written, 5u);
        CHECK(contents(v, file) == text("hello"));
        std::vector<u8> header = d.raw(file);
        CHECK_EQ(at(header, layout::l_size), 5u);
        CHECK_EQ(at(header, layout::l_count), 1u);
        CHECK_EQ(at(header, layout::l_first_data), at(header, layout::l_last_pointer));
        CHECK_EQ(file, 882u);
        CHECK_EQ(at(header, layout::l_first_data), 883u);
        CHECK(dated(header, layout::l_date, later));
        // A write changes the file and the volume, not the directory.
        CHECK(dated(d.raw(v.root_block), layout::l_volume_date, later));
        CHECK(dated(d.raw(v.root_block), layout::l_date, kToday));

        write_all(v, file, 1, text("ELL"));
        CHECK(contents(v, file) == text("hELLo"));
        write_all(v, file, 3, text("p me"));
        CHECK(contents(v, file) == text("hELp me"));
        header = d.raw(file);
        if (!v.ffs) {
            const std::vector<u8> data = d.raw(at(header, layout::l_first_data));
            CHECK_EQ(at(data, layout::l_type), layout::t_data);
            CHECK_EQ(at(data, layout::l_own), file);
            CHECK_EQ(at(data, layout::l_sequence), 1u);
            CHECK_EQ(at(data, layout::l_data_bytes), 7u);
            CHECK_EQ(at(data, layout::l_next_data), 0u);
        }
        CHECK_EQ(adf_used_blocks(v), 4u);
        CHECK_CONSISTENT(v);

        const u32 dir = create_dir(v, v.root_block, "d").header_block;
        CHECK_EQ(adf_write(v, dir, 0, text("x").data(), 1, kToday, written), FsError::NotFile);
        CHECK_EQ(adf_set_size(v, dir, 0, kToday), FsError::NotFile);
    }
}

void files_crossing_extension_blocks() {
    for (const u8 type : {u8{0}, u8{1}}) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 p = payload(v);
        u32 used = 2;
        u32 seed = 1;
        u32 file = 0;
        for (const u32 size : {72 * p, 72 * p + 1, 144 * p, 144 * p + 1, 145 * p - 3}) {
            const std::string name = "f" + std::to_string(size);
            file = create_file(v, v.root_block, name.c_str()).header_block;
            const std::vector<u8> data = pattern(size, seed++);
            write_all(v, file, 0, data);
            CHECK(contents(v, file) == data);
            CHECK_EQ(lookup(v, name).size, size);
            used += file_blocks(v, size);
            CHECK_EQ(adf_used_blocks(v), used);
            CHECK_CONSISTENT(v);
        }
        // The last file: 72 pointers in the header, 72 in the first extension
        // block and one in the second.
        const std::vector<u8> header = d.raw(file);
        CHECK_EQ(at(header, layout::l_count), 72u);
        const u32 first = at(header, layout::l_extension);
        const std::vector<u8> extension = d.raw(first);
        CHECK_EQ(at(extension, layout::l_type), layout::t_list);
        CHECK_EQ(at(extension, layout::l_own), first);
        CHECK_EQ(at(extension, layout::l_parent), file);
        CHECK_EQ(at(extension, layout::l_sec_type), layout::st_file);
        CHECK_EQ(at(extension, layout::l_count), 72u);
        const std::vector<u8> second = d.raw(at(extension, layout::l_extension));
        CHECK_EQ(at(second, layout::l_count), 1u);
        CHECK_EQ(at(second, layout::l_extension), 0u);
    }
}

void appends_in_odd_sizes() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 file = create_file(v, v.root_block, "log").header_block;
        std::vector<u8> model;
        u32 seed = 10;
        // Over 150 blocks in the end, in pieces that end on and off block
        // boundaries.
        for (const u32 size : {1u, 487u, 1u, 488u, 489u, 511u, 512u, 513u, 1000u, 3u, 4096u, 7u,
                               35000u, 9999u, 24000u}) {
            const std::vector<u8> piece = pattern(size, seed++);
            write_all(v, file, model.size(), piece);
            model.insert(model.end(), piece.begin(), piece.end());
            CHECK(contents(v, file) == model);
            CHECK_EQ(adf_used_blocks(v), 2 + file_blocks(v, model.size()));
            CHECK_CONSISTENT(v);
        }
        CHECK(model.size() > 150u * payload(v));
    }
}

void writes_in_the_middle() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 file = create_file(v, v.root_block, "data").header_block;
        std::vector<u8> model = pattern(160 * 488, 5);
        write_all(v, file, 0, model);
        const u32 used = adf_used_blocks(v);
        // Inside a block, across blocks, across the second and third tables,
        // the first byte, and over the end.
        const std::pair<u32, u32> writes[] = {{10, 5},
                                              {480, 40},
                                              {511, 2},
                                              {70 * 488 + 100, 3 * 512},
                                              {143 * 488, 2 * 512 + 9},
                                              {0, 1},
                                              {static_cast<u32>(model.size()) - 100, 300}};
        u32 seed = 50;
        for (const auto& [offset, length] : writes) {
            const std::vector<u8> piece = pattern(length, seed++);
            if (offset + length > model.size()) {
                CHECK_EQ(adf_used_blocks(v), used);  // the overwrites took no blocks
                model.resize(offset + length);
            }
            write_all(v, file, offset, piece);
            std::copy(piece.begin(), piece.end(), model.begin() + offset);
            CHECK(contents(v, file) == model);
            CHECK_CONSISTENT(v);
        }
        CHECK_EQ(adf_used_blocks(v), 2 + file_blocks(v, model.size()));
    }
}

void gaps_fill_with_zeros() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 root = v.root_block;

        const u32 sparse = create_file(v, root, "sparse").header_block;
        write_all(v, sparse, 0, text("abc"));
        write_all(v, sparse, 5000, text("xyz"));
        std::vector<u8> model(5003, 0);
        std::memcpy(model.data(), "abc", 3);
        std::memcpy(model.data() + 5000, "xyz", 3);
        CHECK(contents(v, sparse) == model);

        // From an empty file to past the header's 72 blocks.
        const u32 far = create_file(v, root, "far").header_block;
        write_all(v, far, 40000, text("end"));
        std::vector<u8> far_model(40003, 0);
        std::memcpy(far_model.data() + 40000, "end", 3);
        CHECK(contents(v, far) == far_model);
        CHECK_EQ(adf_used_blocks(v), 2 + file_blocks(v, 5003) + file_blocks(v, 40003));

        // Bytes cut off do not come back when the file grows again.
        const u32 cut = create_file(v, root, "cut").header_block;
        std::vector<u8> cut_model = pattern(1000, 9);
        write_all(v, cut, 0, cut_model);
        CHECK_EQ(adf_set_size(v, cut, 300, kToday), FsError::Ok);
        CHECK_EQ(adf_set_size(v, cut, 700, kToday), FsError::Ok);
        write_all(v, cut, 900, text("!"));
        cut_model.resize(300);
        cut_model.resize(901, 0);
        cut_model[900] = '!';
        CHECK(contents(v, cut) == cut_model);
        CHECK_CONSISTENT(v);
    }
}

void truncate_across_extension_blocks() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 p = payload(v);
        const u32 file = create_file(v, v.root_block, "shrink").header_block;
        const std::vector<u8> data = pattern(150 * p + 10, 77);
        write_all(v, file, 0, data);
        for (const u32 size : {150 * p + 9, 150 * p, 145 * p, 144 * p + 1, 144 * p, 144 * p - 1,
                               73 * p, 72 * p + 1, 72 * p, 72 * p - 1, p + 1, p, 1u, 0u}) {
            CHECK_EQ(adf_set_size(v, file, size, kToday), FsError::Ok);
            CHECK(contents(v, file) == std::vector<u8>(data.begin(), data.begin() + size));
            CHECK_EQ(lookup(v, "shrink").size, size);
            CHECK_EQ(adf_used_blocks(v), 2 + file_blocks(v, size));
            CHECK_CONSISTENT(v);
        }
        CHECK_EQ(at(d.raw(file), layout::l_first_data), 0u);
        CHECK_EQ(at(d.raw(file), layout::l_count), 0u);
        CHECK_EQ(at(d.raw(file), layout::l_extension), 0u);

        // Back up with zeros, past both extension blocks.
        CHECK_EQ(adf_set_size(v, file, 146 * p, kToday), FsError::Ok);
        CHECK(contents(v, file) == std::vector<u8>(146 * p, 0));
        CHECK_EQ(adf_used_blocks(v), 2 + file_blocks(v, 146 * p));
        CHECK_CONSISTENT(v);

        // The same size again changes the date only.
        const AmigaDate later{500, 1, 2};
        CHECK_EQ(adf_set_size(v, file, 146 * p, later), FsError::Ok);
        CHECK(dated(d.raw(file), layout::l_date, later));
        CHECK_EQ(lookup(v, "shrink").size, 146 * p);
        CHECK_CONSISTENT(v);
    }
}

u32 bucket_of(const std::string& name, bool intl) {
    return layout::hash_name(reinterpret_cast<const u8*>(name.data()),
                             static_cast<u32>(name.size()), intl);
}

std::vector<u32> chain_of(Disk& d, u32 dir, u32 slot) {
    std::vector<u32> chain;
    for (u32 n = at(d.raw(dir), layout::l_table + slot); n != 0 && chain.size() < 100;
         n = at(d.raw(n), layout::l_hash_next)) {
        chain.push_back(n);
    }
    return chain;
}

void many_files_in_one_bucket() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 root = v.root_block;
        const u32 bucket = bucket_of("f0", v.intl);
        Names names;
        for (u32 i = 0; names.size() < 16; ++i) {
            const std::string name = "f" + std::to_string(i);
            if (bucket_of(name, v.intl) == bucket) {
                names.push_back(name);
            }
        }
        // The filler's blocks, freed after the first eight names, are below
        // theirs: the last eight go in front of and between them.
        const u32 filler = create_file(v, root, "filler").header_block;
        write_all(v, filler, 0, pattern(20 * payload(v), 1));
        std::map<u32, std::string> by_block;
        for (u32 i = 0; i < 16; ++i) {
            if (i == 8) {
                CHECK_EQ(adf_delete(v, filler, kToday), FsError::Ok);
            }
            by_block[create_file(v, root, names[i].c_str()).header_block] = names[i];
            CHECK_CONSISTENT(v);
        }
        std::vector<u32> chain = chain_of(d, root, bucket);
        CHECK_EQ(chain.size(), 16u);
        CHECK(std::is_sorted(chain.begin(), chain.end()));
        CHECK(by_block.at(chain[0]) == names[8]);
        CHECK(by_block.at(chain[15]) == names[7]);
        for (const std::string& name : names) {
            CHECK(by_block.at(lookup(v, name).header_block) == name);
        }

        // From the middle, the front and the end of the chain.
        for (const usize index : {usize{7}, usize{0}, usize{13}}) {
            const u32 gone = chain[index];
            CHECK_EQ(adf_delete(v, gone, kToday), FsError::Ok);
            chain.erase(chain.begin() + static_cast<long>(index));
            CHECK(chain_of(d, root, bucket) == chain);
            DirEntry e{};
            CHECK_EQ(adf_find(v, by_block.at(gone).c_str(), e), FsError::NotFound);
            for (const u32 left : chain) {
                CHECK_EQ(lookup(v, by_block.at(left)).header_block, left);
            }
            CHECK_CONSISTENT(v);
        }
        CHECK_EQ(names_in(v, "").size(), 13u);
        CHECK_EQ(adf_used_blocks(v), 2 + 13u);
    }
}

void delete_files_and_dirs() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 root = v.root_block;
        const u32 dir = create_dir(v, root, "Dir").header_block;
        const u32 sub = create_dir(v, dir, "Sub").header_block;
        const u32 big = create_file(v, dir, "big").header_block;
        write_all(v, big, 0, pattern(150 * payload(v), 4));
        const u32 small = create_file(v, sub, "small").header_block;
        write_all(v, small, 0, text("tiny"));

        CHECK_EQ(adf_delete(v, root, kToday), FsError::BadName);
        CHECK_EQ(adf_delete(v, dir, kToday), FsError::NotEmpty);
        CHECK_EQ(adf_delete(v, sub, kToday), FsError::NotEmpty);
        CHECK_EQ(adf_delete(v, 5000, kToday), FsError::BadBlock);
        CHECK_CONSISTENT(v);

        const AmigaDate later{600, 6, 6};
        CHECK_EQ(adf_delete(v, big, later), FsError::Ok);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "Dir/big", e), FsError::NotFound);
        CHECK(dated(d.raw(dir), layout::l_date, later));
        CHECK(dated(d.raw(root), layout::l_volume_date, later));
        CHECK_EQ(adf_used_blocks(v), 2 + 2 + file_blocks(v, 4));
        CHECK_CONSISTENT(v);

        CHECK_EQ(adf_delete(v, small, kToday), FsError::Ok);
        CHECK_EQ(adf_delete(v, sub, kToday), FsError::Ok);
        CHECK_EQ(adf_delete(v, dir, kToday), FsError::Ok);
        CHECK(names_in(v, "").empty());
        CHECK_EQ(adf_used_blocks(v), 2u);
        CHECK_CONSISTENT(v);
        // Every block came back: the next one taken is the first above the
        // root and its bitmap again.
        CHECK_EQ(create_file(v, root, "again").header_block, 882u);
    }
}

void rename_objects() {
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 root = v.root_block;
        const u32 a = create_dir(v, root, "A").header_block;
        const u32 b = create_dir(v, a, "B").header_block;
        const u32 c = create_dir(v, root, "C").header_block;
        const u32 file = create_file(v, root, "readme").header_block;
        write_all(v, file, 0, text("moving"));
        const u32 other = create_file(v, root, "other").header_block;

        CHECK_EQ(adf_rename(v, file, root, "notes", kToday), FsError::Ok);
        CHECK_EQ(lookup(v, "notes").header_block, file);
        DirEntry e{};
        CHECK_EQ(adf_find(v, "readme", e), FsError::NotFound);
        // A change of case alone is no clash with itself.
        CHECK_EQ(adf_rename(v, file, root, "NOTES", kToday), FsError::Ok);
        CHECK(names_in(v, "") == (Names{"A", "C", "NOTES", "other"}));
        CHECK_EQ(adf_rename(v, file, root, "Other", kToday), FsError::Exists);
        CHECK_EQ(adf_rename(v, file, root, "a", kToday), FsError::Exists);
        CHECK_EQ(adf_rename(v, file, root, "", kToday), FsError::BadName);
        CHECK_EQ(adf_rename(v, file, root, "x/y", kToday), FsError::BadName);
        CHECK_EQ(adf_rename(v, root, a, "Root", kToday), FsError::BadName);
        CHECK_EQ(adf_rename(v, file, other, "x", kToday), FsError::NotDir);
        CHECK_CONSISTENT(v);

        // Across directories: both are dated and the file names its new parent.
        const AmigaDate later{700, 7, 7};
        CHECK_EQ(adf_rename(v, file, b, "moved", later), FsError::Ok);
        CHECK(contents(v, lookup(v, "A/B/moved").header_block) == text("moving"));
        CHECK_EQ(at(d.raw(file), layout::l_parent), b);
        CHECK(dated(d.raw(root), layout::l_date, later));
        CHECK(dated(d.raw(b), layout::l_date, later));
        CHECK(dated(d.raw(root), layout::l_volume_date, later));
        CHECK(names_in(v, "") == (Names{"A", "C", "other"}));
        CHECK(names_in(v, "A/B") == Names{"moved"});
        CHECK_CONSISTENT(v);

        // A directory moves with what is in it, never into itself or below.
        CHECK_EQ(adf_rename(v, a, a, "A2", kToday), FsError::Loop);
        CHECK_EQ(adf_rename(v, a, b, "A2", kToday), FsError::Loop);
        CHECK_EQ(adf_rename(v, a, c, "A", kToday), FsError::Ok);
        CHECK_EQ(lookup(v, "C/A/B/moved").header_block, file);
        CHECK_EQ(at(d.raw(a), layout::l_parent), c);
        CHECK_EQ(adf_rename(v, c, b, "C", kToday), FsError::Loop);
        CHECK(names_in(v, "") == (Names{"C", "other"}));
        CHECK_EQ(adf_used_blocks(v), 2 + 3 + 1 + file_blocks(v, 6));
        CHECK_CONSISTENT(v);
    }
}

void protection_comment_and_date() {
    for (const u8 type : {u8{0}, u8{3}}) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 root = v.root_block;
        const u32 file = create_file(v, root, "f").header_block;
        const u32 dir = create_dir(v, root, "d").header_block;
        const AmigaDate later{800, 8, 8};

        CHECK_EQ(adf_set_protection(v, file, 0x0F, later), FsError::Ok);
        CHECK_EQ(at(d.raw(file), layout::l_protection), 0x0Fu);
        CHECK(dated(d.raw(root), layout::l_volume_date, later));
        CHECK(dated(d.raw(file), layout::l_date, kToday));
        CHECK_EQ(adf_set_protection(v, dir, 0x10, kToday), FsError::Ok);
        CHECK_EQ(at(d.raw(dir), layout::l_protection), 0x10u);
        CHECK_EQ(adf_set_protection(v, root, 0, kToday), FsError::BadName);

        const u32 comment_at = layout::l_comment * 4;
        CHECK_EQ(adf_set_comment(v, file, "a comment", kToday), FsError::Ok);
        std::vector<u8> header = d.raw(file);
        CHECK(header[comment_at] == 9);
        CHECK(std::memcmp(header.data() + comment_at + 1, "a comment", 9) == 0);
        const std::string longest(79, 'c');
        CHECK_EQ(adf_set_comment(v, file, longest.c_str(), kToday), FsError::Ok);
        CHECK(d.raw(file)[comment_at] == 79);
        CHECK_EQ(adf_set_comment(v, file, std::string(80, 'c').c_str(), kToday), FsError::BadName);
        CHECK_EQ(adf_set_comment(v, file, "", kToday), FsError::Ok);
        header = d.raw(file);
        CHECK(std::all_of(header.begin() + comment_at, header.begin() + comment_at + 80,
                          [](u8 b) { return b == 0; }));
        CHECK_EQ(adf_set_comment(v, root, "x", kToday), FsError::BadName);
        CHECK_EQ(adf_set_comment(v, dir, "a drawer", kToday), FsError::Ok);

        const AmigaDate old{10, 20, 30};
        CHECK_EQ(adf_set_date(v, file, old, later), FsError::Ok);
        CHECK(dated(d.raw(file), layout::l_date, old));
        CHECK_EQ(adf_set_date(v, root, old, kToday), FsError::Ok);
        CHECK(dated(d.raw(root), layout::l_date, old));
        CHECK(dated(d.raw(root), layout::l_volume_date, kToday));
        CHECK_EQ(adf_set_date(v, 5000, old, kToday), FsError::BadBlock);

        // Nothing else of the headers moved.
        CHECK_EQ(at(d.raw(root), layout::l_bm_pages), 881u);
        CHECK_EQ(lookup(v, "f").header_block, file);
        CHECK_EQ(lookup(v, "d").header_block, dir);
        CHECK_EQ(adf_used_blocks(v), 4u);
        CHECK_CONSISTENT(v);
    }
}

void disk_full_mid_write() {
    for (u8 type = 0; type <= 3; ++type) {
        // 118 blocks after the bootblocks: the root, its bitmap page, the
        // file's header and 115 more.
        Disk d(type, 120);
        const AdfVolume& v = d.v;
        const u32 p = payload(v);
        const u32 file = create_file(v, v.root_block, "big").header_block;
        const std::vector<u8> data = pattern(200 * p, 8);
        u32 written = 0;
        CHECK_EQ(adf_write(v, file, 0, data.data(), 200 * p, kToday, written), FsError::DiskFull);
        CHECK_EQ(written, 114 * p);  // 72 data blocks, an extension block and 42 more
        CHECK_EQ(lookup(v, "big").size, written);
        CHECK(contents(v, file) == std::vector<u8>(data.begin(), data.begin() + written));
        CHECK_EQ(adf_used_blocks(v), 118u);
        CHECK_CONSISTENT(v);

        DirEntry e{};
        CHECK_EQ(adf_create_file(v, v.root_block, "more", kToday, e), FsError::DiskFull);
        CHECK_EQ(adf_set_size(v, file, 120 * p, kToday), FsError::DiskFull);
        CHECK_EQ(adf_write(v, file, 114 * p, data.data(), 10, kToday, written), FsError::DiskFull);
        CHECK_EQ(written, 0u);
        CHECK_EQ(lookup(v, "big").size, 114 * p);
        CHECK(contents(v, file) == std::vector<u8>(data.begin(), data.begin() + 114 * p));
        CHECK_CONSISTENT(v);

        // A cut makes room again.
        CHECK_EQ(adf_set_size(v, file, 50 * p, kToday), FsError::Ok);
        CHECK_EQ(adf_used_blocks(v), 2 + file_blocks(v, 50 * p));
        write_all(v, file, 50 * p, std::vector<u8>(data.begin() + 50 * p, data.begin() + 100 * p));
        CHECK(contents(v, file) == std::vector<u8>(data.begin(), data.begin() + 100 * p));
        CHECK_CONSISTENT(v);

        // With room for the 72 blocks of the header and an extension block
        // but no data block after it, the extension block is given back.
        Disk tight(type, 78);
        const u32 squeezed = create_file(tight.v, tight.v.root_block, "big").header_block;
        CHECK_EQ(adf_write(tight.v, squeezed, 0, data.data(), 100 * p, kToday, written),
                 FsError::DiskFull);
        CHECK_EQ(written, 72 * p);
        CHECK_EQ(at(tight.raw(squeezed), layout::l_extension), 0u);
        CHECK_EQ(adf_used_blocks(tight.v), 75u);
        CHECK_CONSISTENT(tight.v);
    }
}

void error_names_are_distinct() {
    const FsError all[] = {FsError::Ok,       FsError::ImageSize, FsError::NotDos,
                           FsError::UnsupportedFs, FsError::BadChecksum, FsError::BadBlock,
                           FsError::NotFound, FsError::NotFile,   FsError::NotDir,
                           FsError::Loop,     FsError::NameTooLong, FsError::Io,
                           FsError::ReadOnly, FsError::Exists,    FsError::DiskFull,
                           FsError::NotEmpty, FsError::BadName};
    for (usize i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        CHECK(std::strcmp(fs_error_name(all[i]), "unknown") != 0);
        for (usize j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
            CHECK(std::strcmp(fs_error_name(all[i]), fs_error_name(all[j])) != 0);
        }
    }
}

// The tree a random run expects: every object by its path from the root.
struct Node {
    bool dir;
    u32 block;
    std::vector<u8> data;
};

using Model = std::map<std::string, Node>;

std::string join(const std::string& dir, const std::string& name) {
    return dir.empty() ? name : dir + "/" + name;
}

bool inside(const std::string& path, const std::string& dir) {
    return path == dir || path.rfind(dir + "/", 0) == 0;
}

u32 model_used(const AdfVolume& v, const Model& model) {
    u32 used = 2;
    for (const auto& [path, node] : model) {
        used += node.dir ? 1 : file_blocks(v, node.data.size());
    }
    return used;
}

void verify(const AdfVolume& v, const Model& model) {
    std::map<std::string, Names> children{{"", {}}};
    for (const auto& [path, node] : model) {
        const DirEntry entry = lookup(v, path);
        CHECK_EQ(entry.header_block, node.block);
        CHECK_EQ(entry.is_dir, node.dir);
        if (node.dir) {
            children[path];
        } else {
            CHECK(contents(v, node.block) == node.data);
        }
        const usize slash = path.rfind('/');
        children[slash == std::string::npos ? "" : path.substr(0, slash)].push_back(
            slash == std::string::npos ? path : path.substr(slash + 1));
    }
    for (auto& [dir, names] : children) {
        std::sort(names.begin(), names.end());
        CHECK(names_in(v, dir) == names);
    }
}

// Creates, writes, cuts, grows, renames and deletes at random, the volume
// checked after every step and its tree compared with the model.
void random_operations() {
    const char* const pool[] = {"alpha", "Beta", "gamma", "Delta", "eps", "zeta", "eta", "theta",
                                "Iota", "a_name_of_thirty_characters_xy"};
    constexpr u32 kPool = sizeof(pool) / sizeof(pool[0]);
    for (u8 type = 0; type <= 3; ++type) {
        Disk d(type);
        const AdfVolume& v = d.v;
        const u32 all_blocks = v.block_count - v.reserved;
        Model model;
        u32 state = 12345u + type;
        const auto pick = [&](usize bound) {
            state = state * 1664525u + 1013904223u;
            return static_cast<u32>((state >> 8) % bound);
        };
        for (u32 step = 0; step < 300; ++step) {
            Names dirs{""};
            Names files;
            Names all;
            for (const auto& [path, node] : model) {
                (node.dir ? dirs : files).push_back(path);
                all.push_back(path);
            }
            const auto block_of = [&](const std::string& dir) {
                return dir.empty() ? v.root_block : model.at(dir).block;
            };
            const u32 op = pick(10);
            if (op < 3 || all.empty()) {
                const std::string dir = dirs[pick(dirs.size())];
                const char* name = pool[pick(kPool)];
                const std::string path = join(dir, name);
                const bool is_dir = pick(4) == 0;
                DirEntry entry{};
                const u32 parent = block_of(dir);
                const FsError result = is_dir ? adf_create_dir(v, parent, name, kToday, entry)
                                              : adf_create_file(v, parent, name, kToday, entry);
                if (model.count(path) != 0) {
                    CHECK_EQ(result, FsError::Exists);
                } else if (result == FsError::DiskFull) {
                    CHECK_EQ(adf_used_blocks(v), all_blocks);
                } else {
                    CHECK_EQ(result, FsError::Ok);
                    model[path] = Node{is_dir, entry.header_block, {}};
                }
            } else if (op < 6 && !files.empty()) {
                const std::string path = files[pick(files.size())];
                Node& node = model.at(path);
                const u32 offset = pick(node.data.size() + 3000);
                const std::vector<u8> piece = pattern(pick(12000), step);
                u32 written = 0;
                const FsError result = adf_write(v, node.block, offset, piece.data(),
                                                 static_cast<u32>(piece.size()), kToday, written);
                // A write that runs out gives back an extension block it
                // found no data block for.
                if (result == FsError::DiskFull) {
                    CHECK(written < piece.size());
                    CHECK(adf_used_blocks(v) + 1 >= all_blocks);
                } else {
                    CHECK_EQ(result, FsError::Ok);
                    CHECK_EQ(written, static_cast<u32>(piece.size()));
                }
                node.data.resize(std::max<usize>(node.data.size(), lookup(v, path).size), 0);
                if (written > 0) {
                    std::copy_n(piece.begin(), written, node.data.begin() + offset);
                }
            } else if (op < 7 && !files.empty()) {
                const std::string path = files[pick(files.size())];
                Node& node = model.at(path);
                const u32 size = pick(node.data.size() * 2 + 2000);
                const FsError result = adf_set_size(v, node.block, size, kToday);
                if (result == FsError::DiskFull) {
                    CHECK(adf_used_blocks(v) + 1 >= all_blocks);
                } else {
                    CHECK_EQ(result, FsError::Ok);
                    CHECK_EQ(lookup(v, path).size, size);
                }
                node.data.resize(lookup(v, path).size, 0);
            } else if (op < 9) {
                const std::string path = all[pick(all.size())];
                const std::string dir = dirs[pick(dirs.size())];
                const char* name = pool[pick(kPool)];
                const std::string target = join(dir, name);
                const Node node = model.at(path);
                const FsError result = adf_rename(v, node.block, block_of(dir), name, kToday);
                if (model.count(target) != 0 && target != path) {
                    CHECK_EQ(result, FsError::Exists);
                } else if (node.dir && inside(dir, path)) {
                    CHECK_EQ(result, FsError::Loop);
                } else {
                    CHECK_EQ(result, FsError::Ok);
                    Model moved;
                    for (auto it = model.begin(); it != model.end();) {
                        if (inside(it->first, path)) {
                            moved[target + it->first.substr(path.size())] = it->second;
                            it = model.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    model.insert(moved.begin(), moved.end());
                }
            } else {
                const std::string path = all[pick(all.size())];
                const Node& node = model.at(path);
                const bool has_entries =
                    std::any_of(all.begin(), all.end(), [&](const std::string& other) {
                        return other != path && inside(other, path);
                    });
                const FsError result = adf_delete(v, node.block, kToday);
                if (node.dir && has_entries) {
                    CHECK_EQ(result, FsError::NotEmpty);
                } else {
                    CHECK_EQ(result, FsError::Ok);
                    model.erase(path);
                }
            }
            CHECK_CONSISTENT(v);
            CHECK_EQ(adf_used_blocks(v), model_used(v, model));
            if (step % 5 == 0) {
                verify(v, model);
            }
        }
        verify(v, model);
    }
}

}

int main() {
    format_every_type();
    format_refusals();
    format_large_volume_with_bitmap_extensions();
    floppy_image_mounts_after_format();
    read_only_volumes();
    create_files_and_dirs();
    write_small_files();
    files_crossing_extension_blocks();
    appends_in_odd_sizes();
    writes_in_the_middle();
    gaps_fill_with_zeros();
    truncate_across_extension_blocks();
    many_files_in_one_bucket();
    delete_files_and_dirs();
    rename_objects();
    protection_comment_and_date();
    disk_full_mid_write();
    error_names_are_distinct();
    random_operations();
    return check::finish("test_amiga_fs_write");
}
