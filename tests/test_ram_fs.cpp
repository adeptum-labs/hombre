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
#include "ram_fs.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace hombre;

namespace {

constinit RamFs constinit_probe;

constexpr u32 kClockDays = 100;
constexpr u32 kClockMinute = 200;
constexpr u32 kClockTick = 300;

struct FakeStore {
    std::vector<u8> mem = std::vector<u8>(1u << 20);
    u32 bump = 16;
    u32 allocs = 0;
    u32 frees = 0;
    u32 live = 0;
    bool fail_alloc = false;
    u32 days = kClockDays;
    u32 minute = kClockMinute;
    u32 tick = kClockTick;
};

u32 fake_alloc(void* ctx, u32 bytes) {
    FakeStore& store = *static_cast<FakeStore*>(ctx);
    if (store.fail_alloc) {
        return 0;
    }
    if (static_cast<std::size_t>(store.bump) + bytes > store.mem.size()) {
        return 0;
    }
    const u32 addr = store.bump;
    store.bump += bytes;
    ++store.allocs;
    store.live += bytes;
    return addr;
}

void fake_free(void* ctx, u32, u32 bytes) {
    FakeStore& store = *static_cast<FakeStore*>(ctx);
    CHECK(store.live >= bytes);
    store.live -= bytes;
    ++store.frees;
}

void fake_read(void* ctx, u32 addr, u8* dst, u32 n) {
    const FakeStore& store = *static_cast<FakeStore*>(ctx);
    CHECK(static_cast<std::size_t>(addr) + n <= store.mem.size());
    std::memcpy(dst, store.mem.data() + addr, n);
}

void fake_write(void* ctx, u32 addr, const u8* src, u32 n) {
    FakeStore& store = *static_cast<FakeStore*>(ctx);
    CHECK(static_cast<std::size_t>(addr) + n <= store.mem.size());
    std::memcpy(store.mem.data() + addr, src, n);
}

void fake_now(void* ctx, u32& days, u32& minute, u32& tick) {
    const FakeStore& store = *static_cast<FakeStore*>(ctx);
    days = store.days;
    minute = store.minute;
    tick = store.tick;
}

RamFs::Store wire(FakeStore& store) {
    RamFs::Store wired;
    wired.ctx = &store;
    wired.alloc = fake_alloc;
    wired.free = fake_free;
    wired.read = fake_read;
    wired.write = fake_write;
    wired.now = fake_now;
    return wired;
}

struct Fixture {
    FakeStore fake;
    RamFs fs;

    Fixture() { fs.init(wire(fake)); }
};

u32 make_file(RamFs& fs, u32 dir, const char* name) {
    u32 node = RamFs::kNone;
    CHECK_EQ(fs.create_file(dir, name, node), 0u);
    return node;
}

u32 make_dir(RamFs& fs, u32 dir, const char* name) {
    u32 node = RamFs::kNone;
    CHECK_EQ(fs.create_dir(dir, name, node), 0u);
    return node;
}

u32 find_node(const RamFs& fs, u32 dir, const char* name) {
    u32 node = RamFs::kNone;
    CHECK_EQ(fs.find(dir, name, node), 0u);
    return node;
}

u32 path_node(const RamFs& fs, const char* path) {
    u32 node = RamFs::kNone;
    CHECK_EQ(fs.find_path(path, node), 0u);
    return node;
}

void write_bytes(RamFs& fs, u32 node, u32 pos, const std::vector<u8>& bytes) {
    CHECK_EQ(fs.write(node, pos, bytes.data(), static_cast<u32>(bytes.size())), 0u);
}

void write_text(RamFs& fs, u32 node, u32 pos, const char* text) {
    CHECK_EQ(fs.write(node, pos, reinterpret_cast<const u8*>(text),
                      static_cast<u32>(std::strlen(text))), 0u);
}

std::vector<u8> read_all(const RamFs& fs, u32 node) {
    std::vector<u8> out(fs.node(node).size);
    u32 got = 0;
    CHECK_EQ(fs.read(node, 0, out.data(), static_cast<u32>(out.size()), got), 0u);
    CHECK_EQ(got, static_cast<u32>(out.size()));
    return out;
}

void expect_bytes(const std::vector<u8>& actual, const std::vector<u8>& expected) {
    CHECK_EQ(actual.size(), expected.size());
    CHECK(actual == expected);
}

void root_is_ram_disk() {
    Fixture f;
    CHECK(f.fs.valid(0));
    CHECK(f.fs.node(0).used);
    CHECK(f.fs.node(0).is_dir);
    CHECK_EQ(f.fs.node(0).parent, RamFs::kNone);
    CHECK_EQ(f.fs.parent(0), RamFs::kNone);
    CHECK(std::strcmp(f.fs.node(0).name, "Ram Disk") == 0);
    CHECK_EQ(f.fs.used_nodes(), 1u);
    CHECK_EQ(f.fs.bytes_used(), 0u);
}

void find_path_nested_case_insensitive() {
    Fixture f;
    const u32 a = make_dir(f.fs, 0, "A");
    const u32 b = make_dir(f.fs, a, "b");
    const u32 c = make_file(f.fs, b, "C");
    const u32 d = make_file(f.fs, a, "D");

    CHECK_EQ(find_node(f.fs, 0, "a"), a);
    CHECK_EQ(find_node(f.fs, 0, "A"), a);
    CHECK_EQ(path_node(f.fs, "A/b/C"), c);
    CHECK_EQ(path_node(f.fs, "a/B/c"), c);
    CHECK_EQ(path_node(f.fs, "A/D"), d);

    u32 out = RamFs::kNone;
    CHECK_EQ(f.fs.find_path("A/D/X", out), ERROR_DIR_NOT_FOUND);
    CHECK_EQ(f.fs.find_path("A/nope", out), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(f.fs.find_path("", out), 0u);
    CHECK_EQ(out, 0u);
}

void invalid_names_rejected() {
    Fixture f;
    u32 out = RamFs::kNone;
    const std::string long31(31, 'n');
    CHECK_EQ(f.fs.create_file(0, "", out), ERROR_INVALID_COMPONENT_NAME);
    CHECK_EQ(f.fs.create_dir(0, "", out), ERROR_INVALID_COMPONENT_NAME);
    CHECK_EQ(f.fs.create_file(0, long31.c_str(), out), ERROR_INVALID_COMPONENT_NAME);
    CHECK_EQ(f.fs.create_file(0, "a:b", out), ERROR_INVALID_COMPONENT_NAME);
    CHECK_EQ(f.fs.create_file(0, "a/b", out), ERROR_INVALID_COMPONENT_NAME);
    CHECK_EQ(f.fs.find(0, long31.c_str(), out), ERROR_INVALID_COMPONENT_NAME);

    const std::string name30(30, 'n');
    const u32 file = make_file(f.fs, 0, name30.c_str());
    CHECK_EQ(find_node(f.fs, 0, name30.c_str()), file);
}

void write_in_pieces_reads_back() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "data");
    std::vector<u8> expected;
    for (u32 piece = 0; piece < 100; ++piece) {
        std::vector<u8> chunk(100);
        for (u32 i = 0; i < 100; ++i) {
            chunk[i] = static_cast<u8>(piece * 100u + i);
        }
        write_bytes(f.fs, file, piece * 100u, chunk);
        expected.insert(expected.end(), chunk.begin(), chunk.end());
    }
    CHECK_EQ(f.fs.node(file).size, 10000u);
    CHECK(f.fs.bytes_used() >= 10000u);
    expect_bytes(read_all(f.fs, file), expected);
}

void write_past_end_zero_fills() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "gap");
    write_text(f.fs, file, 0, "AB");
    write_text(f.fs, file, 6, "CD");
    CHECK_EQ(f.fs.node(file).size, 8u);
    const std::vector<u8> expected = {'A', 'B', 0, 0, 0, 0, 'C', 'D'};
    expect_bytes(read_all(f.fs, file), expected);
}

void set_size_shrink_then_grow() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "resize");
    write_bytes(f.fs, file, 0, std::vector<u8>(8, 0x5A));
    CHECK_EQ(f.fs.set_size(file, 4), 0u);
    CHECK_EQ(f.fs.node(file).size, 4u);
    CHECK_EQ(f.fs.set_size(file, 8), 0u);
    CHECK_EQ(f.fs.node(file).size, 8u);
    const std::vector<u8> expected = {0x5A, 0x5A, 0x5A, 0x5A, 0, 0, 0, 0};
    expect_bytes(read_all(f.fs, file), expected);
}

void create_file_truncates() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "t");
    write_bytes(f.fs, file, 0, std::vector<u8>(300, 1));
    CHECK(f.fs.node(file).capacity > 0);
    const u32 frees_before = f.fake.frees;
    u32 again = RamFs::kNone;
    CHECK_EQ(f.fs.create_file(0, "t", again), 0u);
    CHECK_EQ(again, file);
    CHECK_EQ(f.fs.node(file).size, 0u);
    CHECK_EQ(f.fs.node(file).capacity, 0u);
    CHECK_EQ(f.fs.node(file).data, 0u);
    CHECK(f.fake.frees > frees_before);
    CHECK_EQ(f.fs.bytes_used(), 0u);
}

void create_file_over_dir() {
    Fixture f;
    const u32 dir = make_dir(f.fs, 0, "dir");
    u32 out = RamFs::kNone;
    CHECK_EQ(f.fs.create_file(0, "dir", out), ERROR_OBJECT_EXISTS);
    CHECK(f.fs.node(dir).is_dir);
}

void create_dir_duplicate() {
    Fixture f;
    const u32 dir = make_dir(f.fs, 0, "same");
    u32 out = RamFs::kNone;
    CHECK_EQ(f.fs.create_dir(0, "same", out), ERROR_OBJECT_EXISTS);
    CHECK_EQ(f.fs.create_dir(0, "SAME", out), ERROR_OBJECT_EXISTS);
    CHECK_EQ(find_node(f.fs, 0, "same"), dir);
}

void create_file_in_use() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "busy");
    write_text(f.fs, file, 0, "payload");
    f.fs.retain(file);
    u32 out = RamFs::kNone;
    CHECK_EQ(f.fs.create_file(0, "busy", out), ERROR_OBJECT_IN_USE);
    CHECK_EQ(f.fs.node(file).size, 7u);
    f.fs.release(file);
    CHECK_EQ(f.fs.create_file(0, "busy", out), 0u);
    CHECK_EQ(f.fs.node(file).size, 0u);
}

void remove_rules() {
    Fixture f;
    const u32 dir = make_dir(f.fs, 0, "d");
    const u32 child = make_file(f.fs, dir, "f");
    CHECK_EQ(f.fs.remove(dir), ERROR_DIRECTORY_NOT_EMPTY);
    CHECK_EQ(f.fs.remove(child), 0u);
    CHECK_EQ(f.fs.remove(dir), 0u);

    const u32 open = make_file(f.fs, 0, "open");
    f.fs.retain(open);
    CHECK_EQ(f.fs.remove(open), ERROR_OBJECT_IN_USE);
    f.fs.release(open);
    CHECK_EQ(f.fs.remove(open), 0u);

    const u32 prot = make_file(f.fs, 0, "prot");
    CHECK_EQ(f.fs.set_protection(prot, 1), 0u);
    CHECK_EQ(f.fs.remove(prot), ERROR_DELETE_PROTECTED);
    CHECK_EQ(f.fs.set_protection(prot, 0), 0u);
    CHECK_EQ(f.fs.remove(prot), 0u);

    CHECK_EQ(f.fs.remove(0), ERROR_OBJECT_IN_USE);
    CHECK_EQ(f.fs.remove(RamFs::kNone), ERROR_OBJECT_NOT_FOUND);
}

void removing_files_releases_bytes() {
    Fixture f;
    const u32 a = make_file(f.fs, 0, "a");
    const u32 b = make_file(f.fs, 0, "b");
    write_bytes(f.fs, a, 0, std::vector<u8>(700, 7));
    write_bytes(f.fs, b, 0, std::vector<u8>(2000, 9));
    CHECK(f.fs.bytes_used() > 0);
    CHECK(f.fake.live > 0);
    CHECK_EQ(f.fs.remove(a), 0u);
    CHECK_EQ(f.fs.remove(b), 0u);
    CHECK_EQ(f.fs.bytes_used(), 0u);
    CHECK_EQ(f.fake.live, 0u);
    CHECK_EQ(f.fake.allocs, f.fake.frees);
}

void rename_rules() {
    Fixture f;
    const u32 dir1 = make_dir(f.fs, 0, "one");
    const u32 dir2 = make_dir(f.fs, 0, "two");
    const u32 file = make_file(f.fs, dir1, "old");
    const u32 moved = make_file(f.fs, dir1, "moved");
    const u32 clash = make_file(f.fs, dir2, "taken");

    CHECK_EQ(f.fs.rename(file, dir1, "new"), 0u);
    CHECK_EQ(f.fs.node(file).parent, dir1);
    CHECK(std::strcmp(f.fs.node(file).name, "new") == 0);
    u32 out = RamFs::kNone;
    CHECK_EQ(f.fs.find(dir1, "old", out), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(find_node(f.fs, dir1, "new"), file);

    CHECK_EQ(f.fs.rename(moved, dir2, "moved"), 0u);
    CHECK_EQ(f.fs.node(moved).parent, dir2);
    CHECK_EQ(find_node(f.fs, dir2, "moved"), moved);

    CHECK_EQ(f.fs.rename(moved, dir2, "TAKEN"), ERROR_OBJECT_EXISTS);
    CHECK_EQ(f.fs.rename(clash, dir2, "taken"), 0u);

    const u32 child = make_dir(f.fs, dir1, "child");
    CHECK_EQ(f.fs.rename(dir1, child, "x"), ERROR_OBJECT_IN_USE);
}

void rename_case_only_keeps_node() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "Mixed");
    CHECK_EQ(f.fs.rename(file, 0, "mIXED"), 0u);
    CHECK_EQ(find_node(f.fs, 0, "mixed"), file);
    CHECK(std::strcmp(f.fs.node(file).name, "mIXED") == 0);
    CHECK_EQ(f.fs.used_nodes(), 2u);
}

void alloc_failure_keeps_file() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "keep");
    write_text(f.fs, file, 0, "abc");
    f.fake.fail_alloc = true;
    const std::vector<u8> more(10, 0xAA);
    CHECK_EQ(f.fs.write(file, 1000, more.data(), static_cast<u32>(more.size())),
             ERROR_DISK_FULL);
    CHECK_EQ(f.fs.node(file).size, 3u);
    const std::vector<u8> kept = read_all(f.fs, file);
    expect_bytes(kept, {'a', 'b', 'c'});
    const u32 empty = make_file(f.fs, 0, "empty");
    CHECK_EQ(f.fs.write(empty, 0, more.data(), static_cast<u32>(more.size())),
             ERROR_DISK_FULL);
    CHECK_EQ(f.fs.node(empty).size, 0u);
    CHECK_EQ(f.fs.node(empty).data, 0u);
}

void node_table_full() {
    Fixture f;
    for (u32 i = 0; i + 1 < RamFs::kMaxNodes; ++i) {
        const std::string name = "f" + std::to_string(i);
        u32 out = RamFs::kNone;
        CHECK_EQ(f.fs.create_file(0, name.c_str(), out), 0u);
    }
    CHECK_EQ(f.fs.used_nodes(), RamFs::kMaxNodes);
    u32 out = RamFs::kNone;
    CHECK_EQ(f.fs.create_file(0, "overflow", out), ERROR_DISK_FULL);
    CHECK_EQ(f.fs.used_nodes(), RamFs::kMaxNodes);
}

void comment_limit() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "c");
    const std::string ok(79, 'x');
    CHECK_EQ(f.fs.set_comment(file, ok.c_str()), 0u);
    CHECK(std::strcmp(f.fs.node(file).comment, ok.c_str()) == 0);
    const std::string big(80, 'y');
    CHECK_EQ(f.fs.set_comment(file, big.c_str()), ERROR_COMMENT_TOO_BIG);
    CHECK(std::strcmp(f.fs.node(file).comment, ok.c_str()) == 0);
    CHECK_EQ(f.fs.set_comment(RamFs::kNone, ok.c_str()), ERROR_OBJECT_NOT_FOUND);
}

void protection_and_date_stored() {
    Fixture f;
    const u32 file = make_file(f.fs, 0, "attrs");
    CHECK_EQ(f.fs.set_protection(file, 0x15), 0u);
    CHECK_EQ(f.fs.node(file).protection, 0x15u);
    CHECK_EQ(f.fs.set_date(file, 11, 22, 33), 0u);
    CHECK_EQ(f.fs.node(file).date.days, 11u);
    CHECK_EQ(f.fs.node(file).date.minute, 22u);
    CHECK_EQ(f.fs.node(file).date.tick, 33u);
    CHECK_EQ(f.fs.set_protection(RamFs::kNone, 1), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(f.fs.set_date(RamFs::kNone, 1, 2, 3), ERROR_OBJECT_NOT_FOUND);
}

void new_nodes_stamped() {
    Fixture f;
    CHECK_EQ(f.fs.node(0).date.days, kClockDays);
    CHECK_EQ(f.fs.node(0).date.minute, kClockMinute);
    CHECK_EQ(f.fs.node(0).date.tick, kClockTick);
    f.fake.days = 400;
    f.fake.minute = 500;
    f.fake.tick = 600;
    const u32 file = make_file(f.fs, 0, "stamped");
    CHECK_EQ(f.fs.node(file).date.days, 400u);
    CHECK_EQ(f.fs.node(file).date.minute, 500u);
    CHECK_EQ(f.fs.node(file).date.tick, 600u);
    const u32 dir = make_dir(f.fs, 0, "dir");
    CHECK_EQ(f.fs.node(dir).date.days, 400u);
    CHECK_EQ(f.fs.node(dir).date.minute, 500u);
    CHECK_EQ(f.fs.node(dir).date.tick, 600u);
}

void next_child_in_index_order() {
    Fixture f;
    const u32 a = make_file(f.fs, 0, "a");
    const u32 b = make_dir(f.fs, 0, "b");
    const u32 c = make_file(f.fs, 0, "c");
    const u32 expected[3] = {a, b, c};
    u32 node = f.fs.next_child(0, RamFs::kNone);
    for (u32 i = 0; i < 3; ++i) {
        CHECK_EQ(node, expected[i]);
        node = f.fs.next_child(0, node);
    }
    CHECK_EQ(node, RamFs::kNone);
    CHECK_EQ(f.fs.next_child(a, RamFs::kNone), RamFs::kNone);
    CHECK_EQ(f.fs.next_child(b, RamFs::kNone), RamFs::kNone);
}

void read_dir_wrong_type() {
    Fixture f;
    const u32 dir = make_dir(f.fs, 0, "dir");
    u8 buf[8]{};
    u32 got = 123;
    CHECK_EQ(f.fs.read(dir, 0, buf, 8, got), ERROR_OBJECT_WRONG_TYPE);
    CHECK_EQ(got, 0u);
    CHECK_EQ(f.fs.read(RamFs::kNone, 0, buf, 8, got), ERROR_OBJECT_NOT_FOUND);
}

void init_twice_frees_data() {
    Fixture f;
    const u32 a = make_file(f.fs, 0, "a");
    const u32 b = make_file(f.fs, 0, "b");
    write_bytes(f.fs, a, 0, std::vector<u8>(500, 1));
    write_bytes(f.fs, b, 0, std::vector<u8>(900, 2));
    const u32 frees_before = f.fake.frees;
    CHECK(f.fake.live > 0);
    f.fs.init(wire(f.fake));
    CHECK(f.fake.frees > frees_before);
    CHECK_EQ(f.fake.live, 0u);
    CHECK_EQ(f.fs.used_nodes(), 1u);
    CHECK_EQ(f.fs.bytes_used(), 0u);
    CHECK(f.fs.node(0).is_dir);
    CHECK(std::strcmp(f.fs.node(0).name, "Ram Disk") == 0);
}

}

int main() {
    CHECK(!constinit_probe.valid(0));
    CHECK_EQ(constinit_probe.used_nodes(), 0u);
    root_is_ram_disk();
    find_path_nested_case_insensitive();
    invalid_names_rejected();
    write_in_pieces_reads_back();
    write_past_end_zero_fills();
    set_size_shrink_then_grow();
    create_file_truncates();
    create_file_over_dir();
    create_dir_duplicate();
    create_file_in_use();
    remove_rules();
    removing_files_releases_bytes();
    rename_rules();
    rename_case_only_keeps_node();
    alloc_failure_keeps_file();
    node_table_full();
    comment_limit();
    protection_and_date_stored();
    new_nodes_stamped();
    next_child_in_index_order();
    read_dir_wrong_type();
    init_twice_frees_data();
    return check::finish("test_ram_fs");
}
