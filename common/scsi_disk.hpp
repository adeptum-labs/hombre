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

#include "disk_unit.hpp"
#include "types.hpp"

// The SCSI command set a DiskUnit answers as a direct access disk: the
// commands HDToolBox, filesystems and partitioning tools issue, run
// synchronously against the unit's sectors.

namespace hombre {

// The geometry HDToolBox shows for a unit: 16 heads of 63 sectors, up to
// the largest cylinder count an RDB can describe. A unit too small for a
// full track gets one head of one sector.
struct DiskGeometry {
    u32 heads;
    u32 track_sectors;
    u32 cylinders;
};

constexpr DiskGeometry disk_geometry(u64 sectors) {
    if (sectors < 1008) {
        return DiskGeometry{1, 1, static_cast<u32>(sectors)};
    }
    const u64 cylinders = sectors / 1008;
    return DiskGeometry{16, 63, static_cast<u32>(cylinders > 0xFFFFFF ? 0xFFFFFF : cylinders)};
}

// What a SCSI command answered: status 0 is GOOD, 2 CHECK CONDITION, and
// actual the bytes of data transferred.
struct ScsiResult {
    u8 status;
    u32 actual;
    u8 sense_key;
    u8 asc;
    u8 ascq;
};

// Writes fixed format sense data for result, at most len bytes, and sets
// written to the number of bytes it holds.
inline void scsi_sense(const ScsiResult& result, u8* out, u32 len, u32& written) {
    written = 0;
    if (out == nullptr || len == 0) {
        return;
    }
    u8 reply[18] = {};
    reply[0] = 0x70;
    reply[2] = result.sense_key;
    reply[7] = 10;
    reply[12] = result.asc;
    reply[13] = result.ascq;
    written = len < 18 ? len : 18;
    for (u32 i = 0; i < written; ++i) {
        out[i] = reply[i];
    }
}

namespace scsi_detail {

constexpr u8 kGood = 0;
constexpr u8 kCheckCondition = 2;
constexpr u32 kSectorSize = 512;

constexpr ScsiResult good(u32 actual = 0) {
    return ScsiResult{kGood, actual, 0, 0, 0};
}

constexpr ScsiResult check(u8 key, u8 asc, u32 actual = 0) {
    return ScsiResult{kCheckCondition, actual, key, asc, 0};
}

constexpr u32 be16(const u8* p) {
    return (static_cast<u32>(p[0]) << 8) | p[1];
}

constexpr u32 be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}

constexpr u64 be64(const u8* p) {
    return (static_cast<u64>(be32(p)) << 32) | be32(p + 4);
}

inline void put_be32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

inline void put_be64(u8* p, u64 v) {
    put_be32(p, static_cast<u32>(v >> 32));
    put_be32(p + 4, static_cast<u32>(v));
}

constexpr u32 cdb_size(u8 opcode) {
    switch (opcode) {
    case 0x00:
    case 0x03:
    case 0x08:
    case 0x0A:
    case 0x12:
    case 0x1A:
    case 0x1B:
    case 0x1E:
        return 6;
    case 0x25:
    case 0x28:
    case 0x2A:
    case 0x2F:
    case 0x35:
    case 0x5A:
        return 10;
    case 0x88:
    case 0x8A:
    case 0x91:
    case 0x9E:
        return 16;
    default:
        return 0;
    }
}

// Copies source at most allocation and data_len bytes to data and reports
// the number copied as the actual transfer length.
inline ScsiResult answer(u8* data, u32 data_len, const u8* source, u32 bytes, u32 allocation) {
    u32 count = bytes;
    if (allocation < count) {
        count = allocation;
    }
    if (data_len < count) {
        count = data_len;
    }
    for (u32 i = 0; i < count; ++i) {
        data[i] = source[i];
    }
    return good(count);
}

inline bool in_range(const DiskUnit& unit, u64 lba, u64 count) {
    return lba <= unit.sectors && count <= unit.sectors - lba;
}

inline ScsiResult transfer(const DiskUnit& unit, bool writing, u64 lba, u64 count, u8* data,
                           u32 data_len) {
    if (!in_range(unit, lba, count)) {
        return check(5, 0x21);
    }
    const u64 bytes = count * kSectorSize;
    if (data_len < bytes) {
        return check(5, 0x24);
    }
    if (writing && unit.write == nullptr) {
        return check(7, 0x27);
    }
    for (u64 i = 0; i < count; ++i) {
        const bool ok = writing ? unit.write(unit.ctx, lba + i, data + i * kSectorSize)
                                : unit.read(unit.ctx, lba + i, data + i * kSectorSize);
        if (!ok) {
            return check(3, writing ? 0x0C : 0x11, static_cast<u32>(i * kSectorSize));
        }
    }
    if (writing && unit.flush != nullptr && !unit.flush(unit.ctx)) {
        return check(3, 0x0C, static_cast<u32>(bytes));
    }
    return good(static_cast<u32>(bytes));
}

inline u32 put_page3(u8* buffer, u32 at, const DiskGeometry& geometry) {
    buffer[at] = 0x03;
    buffer[at + 1] = 0x16;
    buffer[at + 10] = static_cast<u8>(geometry.track_sectors >> 8);
    buffer[at + 11] = static_cast<u8>(geometry.track_sectors);
    buffer[at + 12] = static_cast<u8>(kSectorSize >> 8);
    buffer[at + 13] = static_cast<u8>(kSectorSize);
    return at + 24;
}

inline u32 put_page4(u8* buffer, u32 at, const DiskGeometry& geometry) {
    buffer[at] = 0x04;
    buffer[at + 1] = 0x16;
    buffer[at + 2] = static_cast<u8>(geometry.cylinders >> 16);
    buffer[at + 3] = static_cast<u8>(geometry.cylinders >> 8);
    buffer[at + 4] = static_cast<u8>(geometry.cylinders);
    buffer[at + 5] = static_cast<u8>(geometry.heads);
    buffer[at + 20] = 0x1C;  // 7200 rotations per minute
    buffer[at + 21] = 0x20;
    return at + 24;
}

inline ScsiResult mode_sense(const DiskUnit& unit, const u8* cdb, u8* data, u32 data_len,
                             bool ten) {
    const u8 page = cdb[2] & 0x3F;
    if (page != 0x03 && page != 0x04 && page != 0x3F) {
        return check(5, 0x24);
    }
    u8 buffer[64] = {};
    const bool no_descriptor = (cdb[1] & 8) != 0;
    u32 at = ten ? 8 : 4;
    if (!no_descriptor) {
        const u32 sectors = unit.sectors > 0xFFFFFF ? 0xFFFFFF : static_cast<u32>(unit.sectors);
        buffer[at + 1] = static_cast<u8>(sectors >> 16);
        buffer[at + 2] = static_cast<u8>(sectors >> 8);
        buffer[at + 3] = static_cast<u8>(sectors);
        buffer[at + 5] = static_cast<u8>(kSectorSize >> 16);
        buffer[at + 6] = static_cast<u8>(kSectorSize >> 8);
        buffer[at + 7] = static_cast<u8>(kSectorSize);
        at += 8;
    }
    const DiskGeometry geometry = disk_geometry(unit.sectors);
    if (page == 0x03 || page == 0x3F) {
        at = put_page3(buffer, at, geometry);
    }
    if (page == 0x04 || page == 0x3F) {
        at = put_page4(buffer, at, geometry);
    }
    const u8 device_specific = unit.write == nullptr ? 0x80 : 0x00;
    if (ten) {
        buffer[0] = static_cast<u8>((at - 2) >> 8);
        buffer[1] = static_cast<u8>(at - 2);
        buffer[3] = device_specific;
        buffer[6] = static_cast<u8>((no_descriptor ? 0 : 8) >> 8);
        buffer[7] = static_cast<u8>(no_descriptor ? 0 : 8);
    } else {
        buffer[0] = static_cast<u8>(at - 1);
        buffer[2] = device_specific;
        buffer[3] = static_cast<u8>(no_descriptor ? 0 : 8);
    }
    const u32 allocation = ten ? be16(cdb + 7) : cdb[4];
    return answer(data, data_len, buffer, at, allocation);
}

}

// Runs one SCSI command against unit. The command's data phase uses data
// as data_len bytes; a null data pointer means no data. Reports GOOD or
// CHECK CONDITION with the sense key, ASC and ASCQ of a failure.
inline ScsiResult scsi_execute(const DiskUnit& unit, const u8* cdb, u32 cdb_len, u8* data,
                               u32 data_len) {
    using namespace scsi_detail;
    if (data == nullptr) {
        data_len = 0;
    }
    if (cdb == nullptr || cdb_len == 0) {
        return check(5, 0x24);
    }
    const u8 opcode = cdb[0];
    const u32 needed = cdb_size(opcode);
    if (needed == 0) {
        return check(5, 0x20);
    }
    if (cdb_len < needed) {
        return check(5, 0x24);
    }
    switch (opcode) {
    case 0x00:  // TEST UNIT READY
    case 0x1B:  // START STOP UNIT
    case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
        return good();
    case 0x03: {  // REQUEST SENSE
        u32 written = 0;
        scsi_sense(good(), data, cdb[4] < data_len ? cdb[4] : data_len, written);
        return good(written);
    }
    case 0x12: {  // INQUIRY
        if ((cdb[1] & 1) != 0) {
            return check(5, 0x24);
        }
        u8 buffer[36] = {};
        buffer[2] = 2;
        buffer[3] = 2;
        buffer[4] = 31;
        const char vendor[] = "HOMBRE  ";
        for (u32 i = 0; i < 8; ++i) {
            buffer[8 + i] = static_cast<u8>(vendor[i]);
        }
        u32 product_len = 0;
        while (product_len < 16 && unit.product[product_len] != '\0') {
            ++product_len;
        }
        for (u32 i = 0; i < 16; ++i) {
            buffer[16 + i] =
                i < product_len ? static_cast<u8>(unit.product[i]) : static_cast<u8>(' ');
        }
        buffer[32] = '1';
        buffer[33] = '.';
        buffer[34] = '0';
        buffer[35] = ' ';
        return answer(data, data_len, buffer, 36, cdb[4]);
    }
    case 0x25: {  // READ CAPACITY(10)
        u8 buffer[8] = {};
        const u64 last = unit.sectors - 1;
        put_be32(buffer, last > 0xFFFFFFFF ? 0xFFFFFFFF : static_cast<u32>(last));
        put_be32(buffer + 4, kSectorSize);
        return answer(data, data_len, buffer, 8, 8);
    }
    case 0x9E: {  // READ CAPACITY(16)
        if ((cdb[1] & 0x1F) != 0x10) {
            return check(5, 0x20);
        }
        u8 buffer[32] = {};
        put_be64(buffer, unit.sectors - 1);
        put_be32(buffer + 8, kSectorSize);
        return answer(data, data_len, buffer, 32, be32(cdb + 10));
    }
    case 0x08:
    case 0x0A: {  // READ(6), WRITE(6)
        const u64 lba = (static_cast<u64>(cdb[1] & 0x1F) << 16) |
                        (static_cast<u64>(cdb[2]) << 8) | cdb[3];
        const u64 count = cdb[4] == 0 ? 256 : cdb[4];
        return transfer(unit, opcode == 0x0A, lba, count, data, data_len);
    }
    case 0x28:
    case 0x2A: {  // READ(10), WRITE(10)
        const u32 count = be16(cdb + 7);
        if (count == 0) {
            return good();
        }
        return transfer(unit, opcode == 0x2A, be32(cdb + 2), count, data, data_len);
    }
    case 0x88:
    case 0x8A: {  // READ(16), WRITE(16)
        const u32 count = be32(cdb + 10);
        if (count == 0) {
            return good();
        }
        return transfer(unit, opcode == 0x8A, be64(cdb + 2), count, data, data_len);
    }
    case 0x2F: {  // VERIFY(10)
        const u32 count = be16(cdb + 7);
        return in_range(unit, be32(cdb + 2), count) ? good() : check(5, 0x21);
    }
    case 0x35:  // SYNCHRONIZE CACHE(10)
    case 0x91:  // SYNCHRONIZE CACHE(16)
        if (unit.flush != nullptr && !unit.flush(unit.ctx)) {
            return check(3, 0x0C);
        }
        return good();
    case 0x1A:  // MODE SENSE(6)
        return mode_sense(unit, cdb, data, data_len, false);
    case 0x5A:  // MODE SENSE(10)
        return mode_sense(unit, cdb, data, data_len, true);
    default:
        return check(5, 0x20);
    }
}

}
