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
#include "virtio_blk.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace hombre;

namespace {

void struct_layouts() {
    CHECK_EQ(sizeof(VirtqDesc), 16u);
    CHECK_EQ(offsetof(VirtqDesc, len), 8u);
    CHECK_EQ(offsetof(VirtqDesc, flags), 12u);
    CHECK_EQ(offsetof(VirtqDesc, next), 14u);
    CHECK_EQ(sizeof(VirtioBlkRequestHeader), 16u);
    CHECK_EQ(offsetof(VirtioBlkRequestHeader, sector), 8u);
    CHECK_EQ(sizeof(VirtqUsedElem), 8u);
    CHECK_EQ(offsetof(VirtioPciCommonCfg, device_status), 0x14u);
    CHECK_EQ(offsetof(VirtioPciCommonCfg, queue_select), 0x16u);
    CHECK_EQ(offsetof(VirtioPciCommonCfg, queue_size), 0x18u);
    CHECK_EQ(offsetof(VirtioPciCommonCfg, queue_enable), 0x1Cu);
    CHECK_EQ(offsetof(VirtioPciCommonCfg, queue_notify_off), 0x1Eu);
    CHECK_EQ(offsetof(VirtioPciCommonCfg, queue_desc_lo), 0x20u);
    CHECK_EQ(offsetof(VirtioPciCommonCfg, queue_driver_lo), 0x28u);
    CHECK_EQ(offsetof(VirtioPciCommonCfg, queue_device_hi), 0x34u);
}

void read_chain() {
    VirtqDesc desc[4];
    std::memset(desc, 0xAB, sizeof(desc));
    const u16 head = virtio_build_read_chain(desc, 0x10000, 0x200000, 65536, 0x10010);
    CHECK_EQ(head, 0);

    CHECK_EQ(desc[0].addr, 0x10000ull);
    CHECK_EQ(desc[0].len, 16u);
    CHECK_EQ(desc[0].flags, kVirtqDescNext);
    CHECK_EQ(desc[0].next, 1);

    CHECK_EQ(desc[1].addr, 0x200000ull);
    CHECK_EQ(desc[1].len, 65536u);
    CHECK_EQ(desc[1].flags, kVirtqDescNext | kVirtqDescWrite);
    CHECK_EQ(desc[1].next, 2);

    CHECK_EQ(desc[2].addr, 0x10010ull);
    CHECK_EQ(desc[2].len, 1u);
    CHECK_EQ(desc[2].flags, kVirtqDescWrite);
    CHECK_EQ(desc[2].next, 0);

    // Only the three chain entries are written.
    CHECK_EQ(desc[3].addr, 0xABABABABABABABABull);
    CHECK_EQ(desc[3].flags, 0xABAB);

    CHECK_EQ(kVirtqDescNext, 1);
    CHECK_EQ(kVirtqDescWrite, 2);
}

void write_chain() {
    VirtqDesc desc[4];
    std::memset(desc, 0xAB, sizeof(desc));
    const u16 head = virtio_build_data_chain(desc, 0x10000, 0x200000, 1024, 0x10010, false);
    CHECK_EQ(head, 0);
    CHECK_EQ(desc[0].flags, kVirtqDescNext);
    // The device only reads the data of a write.
    CHECK_EQ(desc[1].addr, 0x200000ull);
    CHECK_EQ(desc[1].len, 1024u);
    CHECK_EQ(desc[1].flags, kVirtqDescNext);
    CHECK_EQ(desc[1].next, 2);
    CHECK_EQ(desc[2].flags, kVirtqDescWrite);
    CHECK_EQ(desc[3].flags, 0xABAB);
    CHECK_EQ(kVirtioBlkTypeOut, 1u);
}

void flush_chain() {
    VirtqDesc desc[3];
    std::memset(desc, 0xAB, sizeof(desc));
    const u16 head = virtio_build_flush_chain(desc, 0x10000, 0x10010);
    CHECK_EQ(head, 0);
    CHECK_EQ(desc[0].addr, 0x10000ull);
    CHECK_EQ(desc[0].len, 16u);
    CHECK_EQ(desc[0].flags, kVirtqDescNext);
    CHECK_EQ(desc[0].next, 1);
    CHECK_EQ(desc[1].addr, 0x10010ull);
    CHECK_EQ(desc[1].len, 1u);
    CHECK_EQ(desc[1].flags, kVirtqDescWrite);
    CHECK_EQ(desc[1].next, 0);
    CHECK_EQ(desc[2].flags, 0xABAB);
    CHECK_EQ(kVirtioBlkTypeFlush, 4u);
}

void queue_size_choice() {
    CHECK_EQ(virtq_queue_size(256, 16), 16);
    CHECK_EQ(virtq_queue_size(16, 16), 16);
    CHECK_EQ(virtq_queue_size(17, 16), 16);
    CHECK_EQ(virtq_queue_size(8, 16), 8);
    CHECK_EQ(virtq_queue_size(12, 16), 8);
    CHECK_EQ(virtq_queue_size(3, 16), 2);
    CHECK_EQ(virtq_queue_size(1, 16), 1);
    CHECK_EQ(virtq_queue_size(0, 16), 0);
    CHECK_EQ(virtq_queue_size(0xFFFF, 16), 16);
}

void ring_layout() {
    const VirtqLayout l16 = virtq_layout(16);
    CHECK_EQ(l16.desc_offset, 0u);
    CHECK_EQ(l16.avail_offset, 256u);
    // The available ring is 6 + 2 * 16 = 38 bytes; the used ring is 4-aligned.
    CHECK_EQ(l16.used_offset, 296u);
    CHECK_EQ(l16.bytes, 296u + 6 + 8 * 16);

    const VirtqLayout l1 = virtq_layout(1);
    CHECK_EQ(l1.avail_offset, 16u);
    CHECK_EQ(l1.used_offset, 24u);  // 16 + 6 + 2 = 24
    CHECK_EQ(l1.bytes, 24u + 6 + 8);

    for (u16 size = 1; size <= 16; size = static_cast<u16>(size * 2)) {
        const VirtqLayout l = virtq_layout(size);
        CHECK_EQ(l.desc_offset % 16, 0u);
        CHECK_EQ(l.avail_offset % 2, 0u);
        CHECK_EQ(l.used_offset % 4, 0u);
        CHECK(l.avail_offset >= l.desc_offset + 16u * size);
        CHECK(l.used_offset >= l.avail_offset + 6u + 2u * size);
        CHECK(l.bytes <= 4096u);
    }
}

void range_checks() {
    CHECK(virtio_blk_range_ok(0, 1, 6));
    CHECK(virtio_blk_range_ok(0, 6, 6));
    CHECK(virtio_blk_range_ok(5, 1, 6));
    CHECK(!virtio_blk_range_ok(0, 0, 6));
    CHECK(!virtio_blk_range_ok(6, 1, 6));
    CHECK(!virtio_blk_range_ok(5, 2, 6));
    CHECK(!virtio_blk_range_ok(0, 7, 6));
    CHECK(!virtio_blk_range_ok(0, 1, 0));
    // No wraparound past the end of the u64 sector space.
    CHECK(!virtio_blk_range_ok(~0ull, 2, ~0ull));
    CHECK(!virtio_blk_range_ok(~0ull - 1, 0xFFFFFFFFu, 100));

    CHECK(virtio_blk_dest_ok(0x200000, 1));
    CHECK(virtio_blk_dest_ok(0x200000, 0xFFFFFFFFu));
    CHECK(!virtio_blk_dest_ok(0, 1));
    // The last byte may sit at the top of the address space, not past it.
    CHECK(virtio_blk_dest_ok(~0ull - 511, 1));
    CHECK(!virtio_blk_dest_ok(~0ull - 510, 1));
    CHECK(!virtio_blk_dest_ok(~0ull - 511, 2));
    CHECK(virtio_blk_dest_ok(~0ull - (0xFFFFFFFFull * 512 - 1), 0xFFFFFFFFu));
    CHECK(!virtio_blk_dest_ok(~0ull - (0xFFFFFFFFull * 512 - 2), 0xFFFFFFFFu));

    CHECK_EQ(kVirtioBlkMaxChunkSectors * 512u, 65536u);
    CHECK_EQ(virtio_blk_chunk_sectors(1), 1u);
    CHECK_EQ(virtio_blk_chunk_sectors(128), 128u);
    CHECK_EQ(virtio_blk_chunk_sectors(129), 128u);
    CHECK_EQ(virtio_blk_chunk_sectors(0xFFFFFFFFu), 128u);
}

void driver_starts_quiesced() {
    // A driver that never touched a device has nothing that could still
    // write to memory, and no drive.
    VirtioBlk blk;
    CHECK(blk.quiesced());
    CHECK_EQ(blk.sector_count(), 0u);
}

// A device that completes every request it finds in the available ring, in
// order, with the length it is told.
struct FakeDevice {
    VirtqRing* ring;
    u16 last_avail;

    // Takes one entry from the available ring and returns it in the used
    // ring. Returns false when the driver has published nothing new.
    bool complete_one(u32 len) {
        const u16 avail_idx = ring->avail[1];
        if (avail_idx == last_avail) {
            return false;
        }
        const u16 head = ring->avail[2 + virtq_slot(last_avail, ring->size)];
        ++last_avail;
        volatile u16* used = ring->used;
        const u16 used_idx = used[1];
        volatile VirtqUsedElem* elems = virtq_used_elems(*ring);
        elems[virtq_slot(used_idx, ring->size)].id = head;
        elems[virtq_slot(used_idx, ring->size)].len = len;
        used[1] = static_cast<u16>(used_idx + 1);
        return true;
    }
};

struct FakeQueue {
    alignas(4096) u8 memory[4096];
    VirtqRing ring;
    FakeDevice device;

    explicit FakeQueue(u16 size) {
        std::memset(memory, 0, sizeof(memory));
        virtq_ring_init(ring, memory, size);
        device = FakeDevice{&ring, 0};
    }

    // Starts the free-running indices at `start` on both sides, as if that
    // many requests had already completed.
    void start_at(u16 start) {
        ring.avail_idx = start;
        ring.last_used = start;
        ring.avail[1] = start;
        ring.used[1] = start;
        device.last_avail = start;
    }
};

void ring_init_suppresses_interrupts() {
    FakeQueue q(16);
    CHECK_EQ(q.ring.size, 16);
    CHECK(reinterpret_cast<u8*>(q.ring.desc) == q.memory);
    CHECK(reinterpret_cast<volatile u8*>(q.ring.avail) == q.memory + 256);
    CHECK(reinterpret_cast<volatile u8*>(q.ring.used) == q.memory + 296);
    CHECK_EQ(q.ring.avail[0], kVirtqAvailNoInterrupt);
    CHECK_EQ(q.ring.avail[1], 0);
    CHECK_EQ(q.ring.avail_idx, 0);
    CHECK_EQ(q.ring.last_used, 0);
}

void used_entry_completes_request() {
    FakeQueue q(16);
    const u16 head = virtio_build_read_chain(q.ring.desc, 0x1000, 0x2000, 512, 0x1010);

    VirtqUsedElem elem{};
    CHECK(!virtq_poll_used(q.ring, elem));
    virtq_publish(q.ring, head);
    CHECK_EQ(q.ring.avail[1], 1);
    CHECK_EQ(q.ring.avail[2], head);
    CHECK_EQ(virtq_in_flight(q.ring), 1);

    // Published but not yet returned: still nothing to take.
    CHECK(!virtq_poll_used(q.ring, elem));
    CHECK(q.device.complete_one(513));
    CHECK(virtq_poll_used(q.ring, elem));
    CHECK_EQ(elem.id, head);
    CHECK_EQ(elem.len, 513u);
    CHECK_EQ(q.ring.last_used, 1);
    CHECK_EQ(virtq_in_flight(q.ring), 0);
    CHECK(!virtq_poll_used(q.ring, elem));
    CHECK(!q.device.complete_one(0));
}

void indices_wrap_over_u16() {
    for (u16 size = 1; size <= 16; size = static_cast<u16>(size * 4)) {
        FakeQueue q(size);
        q.start_at(0xFFFD);
        for (u32 i = 0; i < 40; ++i) {
            const u16 before = q.ring.avail_idx;
            const u16 head = static_cast<u16>(i % size);
            virtq_publish(q.ring, head);
            CHECK_EQ(q.ring.avail[1], static_cast<u16>(before + 1));
            CHECK_EQ(q.ring.avail[2 + (before & (size - 1))], head);
            CHECK_EQ(virtq_in_flight(q.ring), 1);

            VirtqUsedElem elem{};
            CHECK(!virtq_poll_used(q.ring, elem));
            CHECK(q.device.complete_one(i));
            CHECK(virtq_poll_used(q.ring, elem));
            CHECK_EQ(elem.id, head);
            CHECK_EQ(elem.len, i);
            CHECK_EQ(q.ring.last_used, q.ring.avail_idx);
        }
        // 0xFFFD + 40 wrapped past zero.
        CHECK_EQ(q.ring.avail_idx, static_cast<u16>(0xFFFD + 40));
        CHECK_EQ(q.ring.avail[1], 37);
        CHECK_EQ(q.ring.used[1], 37);
    }
}

void in_flight_across_wrap() {
    FakeQueue q(4);
    q.start_at(0xFFFE);
    virtq_publish(q.ring, 0);
    virtq_publish(q.ring, 1);
    virtq_publish(q.ring, 2);
    CHECK_EQ(q.ring.avail_idx, 1);
    CHECK_EQ(virtq_in_flight(q.ring), 3);
    // Slots 2, 3 and 0 hold the heads in order.
    CHECK_EQ(q.ring.avail[2 + 2], 0);
    CHECK_EQ(q.ring.avail[2 + 3], 1);
    CHECK_EQ(q.ring.avail[2 + 0], 2);

    VirtqUsedElem elem{};
    CHECK(q.device.complete_one(1));
    CHECK(q.device.complete_one(2));
    CHECK(virtq_poll_used(q.ring, elem));
    CHECK_EQ(elem.id, 0u);
    CHECK(virtq_poll_used(q.ring, elem));
    CHECK_EQ(elem.id, 1u);
    CHECK_EQ(virtq_in_flight(q.ring), 1);
    CHECK(!virtq_poll_used(q.ring, elem));
    CHECK(q.device.complete_one(3));
    CHECK(virtq_poll_used(q.ring, elem));
    CHECK_EQ(elem.id, 2u);
    CHECK_EQ(q.ring.last_used, 1);
}

// 256 bytes of PCI configuration space with a capability list.
struct FakeConfig {
    u8 bytes[256];

    FakeConfig() {
        std::memset(bytes, 0, sizeof(bytes));
        bytes[0x06] = 0x10;  // status: capabilities list
    }

    void put32(u8 offset, u32 value) { std::memcpy(&bytes[offset], &value, 4); }

    void vendor_cap(u8 at, u8 next, u8 len, u8 type, u8 bar, u32 offset, u32 length) {
        bytes[at] = 0x09;
        bytes[at + 1] = next;
        bytes[at + 2] = len;
        bytes[at + 3] = type;
        bytes[at + 4] = bar;
        put32(static_cast<u8>(at + 8), offset);
        put32(static_cast<u8>(at + 12), length);
    }

    static u32 read32(void* ctx, u8 offset) {
        const FakeConfig* config = static_cast<const FakeConfig*>(ctx);
        CHECK_EQ(offset & 3u, 0u);
        u32 value;
        std::memcpy(&value, &config->bytes[offset], 4);
        return value;
    }
};

void caps_found() {
    FakeConfig c;
    c.bytes[0x34] = 0x40;
    // An MSI-X capability first, then the four virtio regions on BAR 4.
    c.bytes[0x40] = 0x11;
    c.bytes[0x41] = 0x84;
    c.vendor_cap(0x84, 0x70, 16, 1, 4, 0x0000, 0x1000);
    c.vendor_cap(0x70, 0x60, 16, 3, 4, 0x1000, 0x1000);
    c.vendor_cap(0x60, 0xA0, 16, 4, 4, 0x2000, 0x1000);
    c.vendor_cap(0xA0, 0x00, 20, 2, 4, 0x3000, 0x1000);
    c.put32(0xA0 + 16, 4);

    const VirtioPciCaps caps = virtio_find_caps(&FakeConfig::read32, &c);
    CHECK(caps.common.present);
    CHECK_EQ(caps.common.bar, 4);
    CHECK_EQ(caps.common.offset, 0u);
    CHECK_EQ(caps.common.length, 0x1000u);
    CHECK(caps.isr.present);
    CHECK_EQ(caps.isr.offset, 0x1000u);
    CHECK(caps.device.present);
    CHECK_EQ(caps.device.offset, 0x2000u);
    CHECK(caps.notify.present);
    CHECK_EQ(caps.notify.offset, 0x3000u);
    CHECK_EQ(caps.notify_off_multiplier, 4u);
}

void caps_first_of_each_type_wins() {
    FakeConfig c;
    c.bytes[0x34] = 0x40;
    c.vendor_cap(0x40, 0x50, 16, 1, 6, 0x9000, 0x100);  // BAR 6 does not exist
    c.vendor_cap(0x50, 0x60, 16, 1, 2, 0x1000, 0x100);
    c.vendor_cap(0x60, 0x70, 16, 1, 3, 0x2000, 0x100);
    c.vendor_cap(0x70, 0x80, 16, 7, 1, 0x3000, 0x100);  // unknown type
    c.vendor_cap(0x80, 0x00, 12, 4, 1, 0x4000, 0x100);  // too short
    const VirtioPciCaps caps = virtio_find_caps(&FakeConfig::read32, &c);
    CHECK(caps.common.present);
    CHECK_EQ(caps.common.bar, 2);
    CHECK_EQ(caps.common.offset, 0x1000u);
    CHECK(!caps.device.present);
    CHECK(!caps.notify.present);
    CHECK(!caps.isr.present);
}

void caps_notify_needs_multiplier_room() {
    FakeConfig c;
    c.bytes[0x34] = 0x40;
    c.vendor_cap(0x40, 0x00, 16, 2, 0, 0x3000, 0x1000);  // no multiplier field
    const VirtioPciCaps caps = virtio_find_caps(&FakeConfig::read32, &c);
    CHECK(!caps.notify.present);
}

void caps_none_without_list() {
    FakeConfig c;
    c.bytes[0x06] = 0;
    c.bytes[0x34] = 0x40;
    c.vendor_cap(0x40, 0x00, 16, 1, 0, 0, 0x100);
    const VirtioPciCaps caps = virtio_find_caps(&FakeConfig::read32, &c);
    CHECK(!caps.common.present);
}

void caps_walk_is_bounded() {
    // A list that points back at itself must end, and low pointer bits
    // are ignored.
    FakeConfig c;
    c.bytes[0x34] = 0x43;
    c.vendor_cap(0x40, 0x53, 16, 3, 0, 0, 0x100);
    c.vendor_cap(0x50, 0x40, 16, 3, 0, 0, 0x100);
    const VirtioPciCaps caps = virtio_find_caps(&FakeConfig::read32, &c);
    CHECK(caps.isr.present);
    CHECK(!caps.common.present);

    // A pointer below 0x40 ends the walk.
    FakeConfig low;
    low.bytes[0x34] = 0x20;
    low.vendor_cap(0x20, 0x00, 16, 1, 0, 0, 0x100);
    CHECK(!virtio_find_caps(&FakeConfig::read32, &low).common.present);

    // A capability too near the end of the space to hold its fields is
    // skipped, not read past offset 0xFF.
    FakeConfig end;
    end.bytes[0x34] = 0xF4;
    end.bytes[0xF4] = 0x09;
    end.bytes[0xF5] = 0x00;
    end.bytes[0xF6] = 16;
    end.bytes[0xF7] = 1;
    CHECK(!virtio_find_caps(&FakeConfig::read32, &end).common.present);
}

}

int main() {
    struct_layouts();
    read_chain();
    write_chain();
    flush_chain();
    queue_size_choice();
    ring_layout();
    range_checks();
    driver_starts_quiesced();
    ring_init_suppresses_interrupts();
    used_entry_completes_request();
    indices_wrap_over_u16();
    in_flight_across_wrap();
    caps_found();
    caps_first_of_each_type_wins();
    caps_notify_needs_multiplier_room();
    caps_none_without_list();
    caps_walk_is_bounded();
    return check::finish("test_virtio_blk");
}
