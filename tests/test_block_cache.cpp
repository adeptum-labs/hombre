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


#include "block_cache.hpp"
#include "check.hpp"

#include <cstring>
#include <vector>

using namespace hombre;

namespace {

struct Disk {
    std::vector<u8> data;
    u32 reads = 0;
    u32 writes = 0;
    u32 flushes = 0;
    bool fail_reads = false;
    bool fail_writes = false;

    explicit Disk(u32 sectors) : data(static_cast<usize>(sectors) * 512) {
        for (usize i = 0; i < data.size(); ++i) {
            data[i] = static_cast<u8>(i / 512 * 7 + i);
        }
    }
};

bool disk_read(void* ctx, u64 sector, u8* out) {
    Disk& d = *static_cast<Disk*>(ctx);
    ++d.reads;
    if (d.fail_reads || (sector + 1) * 512 > d.data.size()) {
        return false;
    }
    std::memcpy(out, d.data.data() + sector * 512, 512);
    return true;
}

bool disk_write(void* ctx, u64 sector, const u8* in) {
    Disk& d = *static_cast<Disk*>(ctx);
    ++d.writes;
    if (d.fail_writes || (sector + 1) * 512 > d.data.size()) {
        return false;
    }
    std::memcpy(d.data.data() + sector * 512, in, 512);
    return true;
}

bool disk_flush(void* ctx) {
    ++static_cast<Disk*>(ctx)->flushes;
    return true;
}

struct Rig {
    Disk disk;
    std::vector<BlockCache::Entry> entries;
    std::vector<u32> buckets;
    std::vector<u8> data;
    BlockCache cache;

    Rig(u32 sectors, u32 count, bool writable = true)
        : disk(sectors), entries(count), buckets(BlockCache::buckets_for(count)),
          data(static_cast<usize>(count) * 512) {
        SectorDevice dev{&disk, disk_read, writable ? disk_write : nullptr, disk_flush};
        cache.init(dev, entries.data(), buckets.data(), data.data(), count);
    }

    bool matches(u64 sector) {
        u8 buf[512];
        return cache.read(sector, buf) && std::memcmp(buf, disk.data.data() + sector * 512, 512) == 0;
    }
};

void a_second_read_is_a_hit() {
    Rig r(64, 4);
    CHECK(r.matches(5));
    CHECK(r.matches(5));
    CHECK_EQ(r.disk.reads, 1u);
    CHECK_EQ(r.cache.hits(), 1u);
    CHECK_EQ(r.cache.misses(), 1u);
}

void the_least_recently_used_sector_goes() {
    Rig r(64, 3);
    CHECK(r.matches(1));
    CHECK(r.matches(2));
    CHECK(r.matches(3));
    CHECK(r.matches(1));  // 2 is now the oldest
    CHECK(r.matches(4));  // and goes
    CHECK_EQ(r.disk.reads, 4u);
    CHECK(r.matches(1));
    CHECK(r.matches(3));
    CHECK(r.matches(4));
    CHECK_EQ(r.disk.reads, 4u);
    CHECK(r.matches(2));
    CHECK_EQ(r.disk.reads, 5u);
}

void writes_go_through_and_are_cached() {
    Rig r(64, 4);
    u8 buf[512];
    std::memset(buf, 0x5A, sizeof buf);
    CHECK(r.cache.write(9, buf));
    CHECK_EQ(r.disk.writes, 1u);
    CHECK_EQ(r.disk.data[9 * 512 + 100], 0x5A);
    u8 back[512];
    CHECK(r.cache.read(9, back));
    CHECK_EQ(r.disk.reads, 0u);
    CHECK(std::memcmp(back, buf, 512) == 0);
    // Writing a cached sector again replaces it.
    CHECK(r.matches(9));
    std::memset(buf, 0x33, sizeof buf);
    CHECK(r.cache.write(9, buf));
    CHECK(r.cache.read(9, back));
    CHECK_EQ(back[0], 0x33);
    CHECK_EQ(r.disk.reads, 0u);
}

void a_failed_write_leaves_the_cache_alone() {
    Rig r(64, 4);
    CHECK(r.matches(2));
    r.disk.fail_writes = true;
    u8 buf[512];
    std::memset(buf, 0x77, sizeof buf);
    CHECK(!r.cache.write(2, buf));
    CHECK(r.matches(2));
    CHECK(!r.cache.write(3, buf));
    CHECK_EQ(r.disk.reads, 1u);
}

void a_failed_read_caches_nothing() {
    Rig r(64, 2);
    CHECK(r.matches(1));
    CHECK(r.matches(2));
    r.disk.fail_reads = true;
    u8 buf[512];
    CHECK(!r.cache.read(3, buf));
    r.disk.fail_reads = false;
    // The entry the failed read took held sector 1, which is read again;
    // sector 2 is still there.
    CHECK(r.matches(2));
    CHECK_EQ(r.disk.reads, 3u);
    CHECK(r.matches(1));
    CHECK_EQ(r.disk.reads, 4u);
    CHECK(r.matches(3));
    CHECK_EQ(r.disk.reads, 5u);
}

void a_read_only_disk_refuses_writes() {
    Rig r(64, 4, false);
    u8 buf[512]{};
    CHECK(!r.cache.writable());
    CHECK(!r.cache.write(1, buf));
    CHECK_EQ(r.disk.writes, 0u);
}

void flush_reaches_the_disk() {
    Rig r(64, 4);
    CHECK(r.cache.flush());
    CHECK_EQ(r.disk.flushes, 1u);
}

void one_entry_and_many_sectors() {
    Rig one(64, 1);
    for (u64 s = 0; s < 64; ++s) {
        CHECK(one.matches(s));
        CHECK(one.matches(s));
    }
    CHECK_EQ(one.disk.reads, 64u);

    // Sectors far apart and colliding buckets all come back right.
    Rig r(1 << 16, 37);
    for (u32 round = 0; round < 3; ++round) {
        for (u64 s = 0; s < (1 << 16); s += 977) {
            CHECK(r.matches(s));
        }
    }
    for (u32 i = 0; i < 2000; ++i) {
        CHECK(r.matches((i * 7919u) % (1 << 16)));
    }
}

}

int main() {
    a_second_read_is_a_hit();
    the_least_recently_used_sector_goes();
    writes_go_through_and_are_cached();
    a_failed_write_leaves_the_cache_alone();
    a_failed_read_caches_nothing();
    a_read_only_disk_refuses_writes();
    flush_reaches_the_disk();
    one_entry_and_many_sectors();
    return check::finish("test_block_cache");
}
