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

#include "dos.hpp"

// The datatype descriptors dos keeps in memory: IFF FORM DTYP files in the
// shape datatypes/datatypes.h gives them, which name the kinds of file
// DataTypes knows. They are built at compile time, so they cost nothing at
// boot.
namespace hombre::datatype_descriptors {

namespace detail {

// dth_Flags: a binary type whose name case counts.
constexpr u16 kDtfCase = 0x0010;
// dth_GroupID of a still picture.
constexpr u32 kGidPicture = 0x70696374;  // 'pict'
constexpr u32 kCapacity = 128;
constexpr u32 kHeaderBytes = 32;
constexpr u32 kNameBytes = 4;
constexpr u32 kChunkHeaderBytes = 8;

// The bytes of a descriptor under construction, big-endian as IFF is.
struct Buffer {
    u8 bytes[kCapacity]{};
    u32 size = 0;

    constexpr void put8(u8 value) { bytes[size++] = value; }

    constexpr void put16(u16 value) {
        put8(static_cast<u8>(value >> 8));
        put8(static_cast<u8>(value));
    }

    constexpr void put32(u32 value) {
        put16(static_cast<u16>(value >> 16));
        put16(static_cast<u16>(value));
    }

    constexpr void put16_at(u32 at, u16 value) {
        bytes[at] = static_cast<u8>(value >> 8);
        bytes[at + 1] = static_cast<u8>(value);
    }

    constexpr void put32_at(u32 at, u32 value) {
        put16_at(at, static_cast<u16>(value >> 16));
        put16_at(at + 2, static_cast<u16>(value));
    }

    constexpr void id(const char* text) {
        for (u32 i = 0; i < kNameBytes; ++i) {
            put8(static_cast<u8>(text[i]));
        }
    }

    constexpr void text(const char* value) {
        for (u32 i = 0; value[i] != '\0'; ++i) {
            put8(static_cast<u8>(value[i]));
        }
        put8(0);
    }

    constexpr void align() {
        if ((size & 1) != 0) {
            put8(0);
        }
    }
};

struct Descriptor {
    u8 data[kCapacity];
    u32 size;
};

// Builds FORM DTYP with a NAME chunk and a DTHD chunk. The DTHD chunk
// starts with a DataTypeHeader whose pointers are byte offsets from the
// chunk data; its strings and its mask follow the header.
constexpr Descriptor make(const char* display_name, const char* base_name,
                          const char* pattern, u32 group, u32 id, const u16* mask,
                          u32 mask_len) {
    Buffer b;
    b.id("FORM");
    const u32 form_size_at = b.size;
    b.put32(0);
    b.id("DTYP");

    b.id("NAME");
    const u32 name_size_at = b.size;
    b.put32(0);
    const u32 name_at = b.size;
    b.text(display_name);
    b.put32_at(name_size_at, b.size - name_at);
    b.align();

    b.id("DTHD");
    const u32 dthd_size_at = b.size;
    b.put32(0);
    const u32 dthd_at = b.size;
    for (u32 i = 0; i < kHeaderBytes; ++i) {
        b.put8(0);
    }
    const u32 display_at = b.size - dthd_at;
    b.text(display_name);
    b.align();
    const u32 base_at = b.size - dthd_at;
    b.text(base_name);
    b.align();
    const u32 pattern_at = b.size - dthd_at;
    b.text(pattern);
    b.align();
    const u32 mask_at = b.size - dthd_at;
    for (u32 i = 0; i < mask_len; ++i) {
        b.put16(mask[i]);
    }
    b.put32_at(dthd_at + 0, display_at);
    b.put32_at(dthd_at + 4, base_at);
    b.put32_at(dthd_at + 8, pattern_at);
    b.put32_at(dthd_at + 12, mask_at);
    b.put32_at(dthd_at + 16, group);
    b.put32_at(dthd_at + 20, id);
    b.put16_at(dthd_at + 24, static_cast<u16>(mask_len));
    b.put16_at(dthd_at + 26, 0);
    b.put16_at(dthd_at + 28, kDtfCase);
    b.put16_at(dthd_at + 30, 0);
    b.put32_at(dthd_size_at, b.size - dthd_at);
    b.align();
    b.put32_at(form_size_at, b.size - kChunkHeaderBytes);

    Descriptor descriptor{};
    descriptor.size = b.size;
    for (u32 i = 0; i < b.size; ++i) {
        descriptor.data[i] = b.bytes[i];
    }
    return descriptor;
}

// A mask word 0x00xx matches the byte xx.
inline constexpr u16 kPngMask[] = {0x0089, 0x0050, 0x004E, 0x0047,
                                   0x000D, 0x000A, 0x001A, 0x000A};
inline constexpr u16 kJpegMask[] = {0x00FF, 0x00D8, 0x00FF};

inline constexpr Descriptor kPng =
    make("PNG", "png", "#?", kGidPicture, 0x706E6720, kPngMask, 8);
inline constexpr Descriptor kJpeg =
    make("JFIF", "jpeg", "#?", kGidPicture, 0x6A666966, kJpegMask, 3);

}

// The files DEVS:DataTypes shows by these names.
inline constexpr u32 kCount = 2;
inline constexpr Dos::RomFile kFiles[kCount] = {
    {"PNG", detail::kPng.data, detail::kPng.size},
    {"JPEG", detail::kJpeg.data, detail::kJpeg.size},
};

// Registers the descriptors as built-in files in DEVS:DataTypes.
inline bool register_on(Dos& dos) {
    return dos.add_rom_overlay("DEVS:DataTypes", kFiles, kCount);
}

}
