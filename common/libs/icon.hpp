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

#include "../m68k_cpu.hpp"
#include "../types.hpp"
#include "exec.hpp"
#include "native.hpp"
#include "version.hpp"

// icon.library written in C++ for code run by the 68000 translator.
//
// GetDiskObject reads a Workbench icon ("<name>.info") from the volumes dos
// serves and builds a DiskObject in guest memory with the AmigaOS layout:
// the gadget, DrawerData, both Images and their plane data, the strings and
// the tool type array all live in one exec block, which FreeDiskObject
// returns. GetDiskObjectNew falls back to GetDefDiskObject, whose built-in
// shapes cover the standard types. PutDiskObject writes the same format to
// RAM:, and DeleteDiskObject removes an icon. FindToolType, MatchToolValue
// and BumpRevision work on names and tool types as their autodocs describe.
//
// A missing or malformed icon leaves D0 0 with IoErr ERROR_OBJECT_NOT_FOUND
// or ERROR_OBJECT_WRONG_TYPE. The obsolete GetIcon vector logs
// "icon: unimplemented LVO -42" and ends the run; the obsolete free-list
// calls accept their arguments and free nothing.

namespace hombre {

// Library vector offsets of icon.library, n of LVO -n, as the 3.1 function
// description numbers them.
namespace icon_lvo {

constexpr u32 GetIcon = 42;
constexpr u32 PutIcon = 48;
constexpr u32 FreeFreeList = 54;
constexpr u32 AddFreeList = 72;
constexpr u32 GetDiskObject = 78;
constexpr u32 PutDiskObject = 84;
constexpr u32 FreeDiskObject = 90;
constexpr u32 FindToolType = 96;
constexpr u32 MatchToolValue = 102;
constexpr u32 BumpRevision = 108;
constexpr u32 GetDefDiskObject = 120;
constexpr u32 PutDefDiskObject = 126;
constexpr u32 GetDiskObjectNew = 132;
constexpr u32 DeleteDiskObject = 138;
// The last vector of the 3.1 library; the table reaches down to it.
constexpr u32 Last = 138;

}

class Dos;

namespace icon_detail {
struct IconFile;
}

class Icon {
public:
    // Makes icon.library and adds it to exec's library list. exec and dos
    // must be initialised. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, Dos& dos);

    static constexpr u32 kMaxInfoBytes = 64 * 1024;
    static constexpr u32 kMaxToolTypes = 64;

    u32 base() const { return base_; }

private:
    friend struct IconNatives;

    static constexpr u32 kVectors = icon_lvo::Last / 6;
    static constexpr u32 kMaxObjects = 256;
    static constexpr u32 kMaxNameChars = 255;

    // One DiskObject this library handed out, with the bytes of its block.
    struct Block {
        u32 addr = 0;
        u32 bytes = 0;
    };

    bool record(u32 addr, u32 bytes);
    void free_disk_object(u32 addr);
    u32 build_object(const icon_detail::IconFile& info);
    u32 get_disk_object(native::Bus& bus, u32 name);
    u32 get_disk_object_new(native::Bus& bus, u32 name);
    u32 put_disk_object(native::Bus& bus, u32 name, u32 disk_object);
    u32 delete_disk_object(native::Bus& bus, u32 name);
    u32 get_default_disk_object(u32 type);
    u32 find_tool_type(native::Bus& bus, u32 array, u32 name);
    u32 match_tool_value(native::Bus& bus, u32 types, u32 value);
    u32 bump_revision(native::Bus& bus, u32 new_name, u32 old_name);
    void set_io_error(u32 code);

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Dos* dos_ = nullptr;
    u32 base_ = 0;
    // What the last GetDiskObject failed with, for GetDiskObjectNew's
    // fallback, whichever process is running.
    u32 last_error_ = 0;
    Block blocks_[kMaxObjects]{};
    // The raw bytes of the .info file a call is working on.
    u8 file_[kMaxInfoBytes]{};
    NativeVector vectors_[kVectors]{};
};

}
