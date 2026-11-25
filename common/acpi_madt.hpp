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

// Returns a pointer to `bytes` of physical memory at `phys`, or nullptr when
// there is none. The ACPI tables are named by physical address, so the caller
// says how it reaches them: through the direct map in the kernel, through a
// byte buffer in a test.
using AcpiRead = const u8* (*)(u64 phys, u32 bytes);

struct AcpiProcessorList {
    static constexpr u32 kMax = 64;

    u32 apic_id[kMax]{};
    u32 count = 0;

    void add(u32 id) {
        if (count < kMax) {
            apic_id[count++] = id;
        }
    }
};

namespace acpi_detail {

// Every table starts with the same 36 byte header: signature, length,
// revision and checksum.
inline constexpr u32 kSdtHeaderBytes = 36;
inline constexpr u32 kRsdpBytes = 20;
inline constexpr u32 kRsdpExtendedBytes = 36;
// The MADT keeps the local APIC address and its flags between its header and
// its entries.
inline constexpr u32 kMadtEntries = kSdtHeaderBytes + 8;
// Far above any real table of interest, so a malformed length cannot make a
// walk read its way far past the table.
inline constexpr u32 kMaxTableBytes = 64 * 1024;

inline constexpr u32 kEntryHeaderBytes = 2;
inline constexpr u32 kLocalApicType = 0;
inline constexpr u32 kX2ApicType = 9;
inline constexpr u32 kEnabled = 1;
inline constexpr u32 kOnlineCapable = 2;
// A type 0 entry holds the processor id, the APIC id and the flags; a type 9
// entry holds two reserved bytes, the x2APIC id, the flags and the processor
// uid.
inline constexpr u32 kLocalApicBytes = 8;
inline constexpr u32 kX2ApicBytes = 12;

inline u32 load_u32(const u8* bytes) {
    return static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 |
           static_cast<u32>(bytes[2]) << 16 | static_cast<u32>(bytes[3]) << 24;
}

inline u64 load_u64(const u8* bytes) {
    return static_cast<u64>(load_u32(bytes)) | static_cast<u64>(load_u32(bytes + 4)) << 32;
}

inline bool signature_is(const u8* bytes, const char* signature, u32 length) {
    for (u32 i = 0; i < length; ++i) {
        if (bytes[i] != static_cast<u8>(signature[i])) {
            return false;
        }
    }
    return true;
}

inline bool checksum_holds(const u8* bytes, u32 length) {
    u8 sum = 0;
    for (u32 i = 0; i < length; ++i) {
        sum = static_cast<u8>(sum + bytes[i]);
    }
    return sum == 0;
}

// The table at `phys` when its header names it `signature` and its length
// and checksum hold, or nullptr.
inline const u8* find_table(u64 phys, AcpiRead read, const char* signature) {
    const u8* header = read(phys, kSdtHeaderBytes);
    if (header == nullptr || !signature_is(header, signature, 4)) {
        return nullptr;
    }
    const u32 length = load_u32(header + 4);
    if (length < kSdtHeaderBytes || length > kMaxTableBytes) {
        return nullptr;
    }
    const u8* table = read(phys, length);
    return table != nullptr && checksum_holds(table, length) ? table : nullptr;
}

// The root table the RSDP names: the XSDT from revision 2 on, the RSDT
// before it.
struct Root {
    u64 phys = 0;
    bool extended = false;
};

inline bool read_root(u64 rsdp_phys, AcpiRead read, Root& root) {
    const u8* rsdp = read(rsdp_phys, kRsdpBytes);
    if (rsdp == nullptr || !signature_is(rsdp, "RSD PTR ", 8) ||
        !checksum_holds(rsdp, kRsdpBytes)) {
        return false;
    }
    if (rsdp[15] < 2) {
        root = Root{load_u32(rsdp + 16), false};
        return root.phys != 0;
    }
    const u8* extended = read(rsdp_phys, kRsdpExtendedBytes);
    if (extended == nullptr) {
        return false;
    }
    const u32 length = load_u32(extended + 20);
    if (length < kRsdpExtendedBytes || length > kMaxTableBytes) {
        return false;
    }
    const u8* whole = read(rsdp_phys, length);
    if (whole == nullptr || !checksum_holds(whole, length)) {
        return false;
    }
    root = Root{load_u64(whole + 24), true};
    return root.phys != 0;
}

inline void add_if_usable(u32 apic_id, u32 flags, AcpiProcessorList& list) {
    if ((flags & (kEnabled | kOnlineCapable)) != 0) {
        list.add(apic_id);
    }
}

// The entries end where the table's length does, and at the first entry that
// cannot be read whole: what was listed before it stands.
inline void add_processors(const u8* madt, AcpiProcessorList& list) {
    const u32 length = load_u32(madt + 4);
    for (u32 offset = kMadtEntries; offset + kEntryHeaderBytes <= length;) {
        const u32 type = madt[offset];
        const u32 entry_bytes = madt[offset + 1];
        if (entry_bytes < kEntryHeaderBytes || offset + entry_bytes > length) {
            return;
        }
        if (type == kLocalApicType && entry_bytes >= kLocalApicBytes) {
            add_if_usable(madt[offset + 3], load_u32(madt + offset + 4), list);
        } else if (type == kX2ApicType && entry_bytes >= kX2ApicBytes) {
            add_if_usable(load_u32(madt + offset + 4), load_u32(madt + offset + 8), list);
        }
        offset += entry_bytes;
    }
}

}

// The processors the firmware's MADT lists as enabled or online capable, in
// table order. Empty when there is no RSDP, or a table fails its length or
// its checksum.
inline AcpiProcessorList acpi_read_processors(u64 rsdp_phys, AcpiRead read) {
    AcpiProcessorList list{};
    acpi_detail::Root root{};
    if (rsdp_phys == 0 || read == nullptr || !acpi_detail::read_root(rsdp_phys, read, root)) {
        return list;
    }
    const u32 entry_bytes = root.extended ? 8 : 4;
    const u8* table = acpi_detail::find_table(root.phys, read, root.extended ? "XSDT" : "RSDT");
    if (table == nullptr) {
        return list;
    }
    const u32 length = acpi_detail::load_u32(table + 4);
    for (u32 offset = acpi_detail::kSdtHeaderBytes; offset + entry_bytes <= length;
         offset += entry_bytes) {
        const u64 phys = root.extended ? acpi_detail::load_u64(table + offset)
                                       : acpi_detail::load_u32(table + offset);
        const u8* madt = phys != 0 ? acpi_detail::find_table(phys, read, "APIC") : nullptr;
        if (madt != nullptr) {
            acpi_detail::add_processors(madt, list);
            break;
        }
    }
    return list;
}

}
