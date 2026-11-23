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

#pragma once

#include "types.hpp"

namespace hombre {

// A disk read and written a 512 byte sector at a time.
struct SectorDevice {
    void* ctx = nullptr;
    bool (*read)(void* ctx, u64 sector, u8* out) = nullptr;
    // Null for a read-only disk.
    bool (*write)(void* ctx, u64 sector, const u8* data) = nullptr;
    // Puts every write on the medium; null when writes need no flush.
    bool (*flush)(void* ctx) = nullptr;
};

// Keeps the sectors of a disk read last, so a file system that reads the
// same root, directory and bitmap blocks over and over goes to the disk
// once. The least recently used sector makes room for a new one.
//
// Writes go through to the disk at once, and only a sector the disk took is
// cached, so the cache never holds anything the disk does not: losing it
// loses nothing. A failed read or write leaves the cache as it was.
//
// The caller gives the memory: count entries and count * 512 bytes of data,
// with count at least 1. Lookups go through a hash of the sector number
// over a table of a power of two buckets, at least count of them.
class BlockCache {
public:
    static constexpr u32 kSectorBytes = 512;

    struct Entry {
        u64 sector;
        u32 prev;       // towards the most recently used
        u32 next;       // towards the least recently used
        u32 hash_next;  // the next entry in the same bucket
        bool used;
    };

    // The buckets the table needs for count entries.
    static constexpr u32 buckets_for(u32 count) {
        u32 n = 1;
        while (n < count) {
            n *= 2;
        }
        return n;
    }

    void init(const SectorDevice& device, Entry* entries, u32* buckets, u8* data, u32 count) {
        device_ = device;
        entries_ = entries;
        buckets_ = buckets;
        data_ = data;
        count_ = count;
        bucket_mask_ = buckets_for(count) - 1;
        for (u32 i = 0; i <= bucket_mask_; ++i) {
            buckets_[i] = kNone;
        }
        // Every entry starts free, chained from most to least recently used
        // in index order, so the first misses fill entries 0, 1, 2...
        for (u32 i = 0; i < count; ++i) {
            entries_[i] = Entry{0, i == 0 ? kNone : i - 1, i + 1 == count ? kNone : i + 1, kNone, false};
        }
        head_ = 0;
        tail_ = count - 1;
        hits_ = 0;
        misses_ = 0;
    }

    bool writable() const { return device_.write != nullptr; }

    bool read(u64 sector, u8* out) {
        u32 at = find(sector);
        if (at != kNone) {
            ++hits_;
        } else {
            ++misses_;
            at = tail_;
            if (!device_.read(device_.ctx, sector, data_of(tail_))) {
                // The entry may have held another sector, now overwritten.
                drop(at);
                return false;
            }
            take(at, sector);
        }
        touch(at);
        copy(out, data_of(at));
        return true;
    }

    bool write(u64 sector, const u8* data) {
        if (device_.write == nullptr || !device_.write(device_.ctx, sector, data)) {
            return false;
        }
        u32 at = find(sector);
        if (at == kNone) {
            at = tail_;
            take(at, sector);
        }
        touch(at);
        copy(data_of(at), data);
        return true;
    }

    bool flush() { return device_.flush == nullptr || device_.flush(device_.ctx); }

    u64 hits() const { return hits_; }
    u64 misses() const { return misses_; }

private:
    static constexpr u32 kNone = 0xFFFFFFFFu;

    u8* data_of(u32 at) { return data_ + static_cast<usize>(at) * kSectorBytes; }

    static void copy(u8* to, const u8* from) {
        for (u32 i = 0; i < kSectorBytes; ++i) {
            to[i] = from[i];
        }
    }

    u32 bucket_of(u64 sector) const {
        return static_cast<u32>((sector * 0x9E3779B97F4A7C15ull) >> 40) & bucket_mask_;
    }

    u32 find(u64 sector) const {
        for (u32 at = buckets_[bucket_of(sector)]; at != kNone; at = entries_[at].hash_next) {
            if (entries_[at].sector == sector) {
                return at;
            }
        }
        return kNone;
    }

    void unhash(u32 at) {
        Entry& e = entries_[at];
        if (!e.used) {
            return;
        }
        u32* link = &buckets_[bucket_of(e.sector)];
        while (*link != at) {
            link = &entries_[*link].hash_next;
        }
        *link = e.hash_next;
        e.hash_next = kNone;
        e.used = false;
    }

    // Gives the entry over to `sector`.
    void take(u32 at, u64 sector) {
        unhash(at);
        Entry& e = entries_[at];
        e.sector = sector;
        e.used = true;
        const u32 bucket = bucket_of(sector);
        e.hash_next = buckets_[bucket];
        buckets_[bucket] = at;
    }

    // Frees the entry and makes it the next to be taken.
    void drop(u32 at) {
        unhash(at);
        unlink(at);
        Entry& e = entries_[at];
        e.prev = tail_;
        e.next = kNone;
        if (tail_ != kNone) {
            entries_[tail_].next = at;
        } else {
            head_ = at;
        }
        tail_ = at;
    }

    void unlink(u32 at) {
        Entry& e = entries_[at];
        if (e.prev != kNone) {
            entries_[e.prev].next = e.next;
        } else {
            head_ = e.next;
        }
        if (e.next != kNone) {
            entries_[e.next].prev = e.prev;
        } else {
            tail_ = e.prev;
        }
    }

    // Makes the entry the most recently used.
    void touch(u32 at) {
        if (head_ == at) {
            return;
        }
        unlink(at);
        Entry& e = entries_[at];
        e.prev = kNone;
        e.next = head_;
        entries_[head_].prev = at;
        head_ = at;
    }

    SectorDevice device_{};
    Entry* entries_ = nullptr;
    u32* buckets_ = nullptr;
    u8* data_ = nullptr;
    u32 count_ = 0;
    u32 bucket_mask_ = 0;
    u32 head_ = kNone;
    u32 tail_ = kNone;
    u64 hits_ = 0;
    u64 misses_ = 0;
};

}
