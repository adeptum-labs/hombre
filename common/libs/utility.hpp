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

// utility.library written in C++ for code run by the 68000 translator.
//
// It has the tag list walkers and editors (FindTagItem, GetTagData,
// NextTagItem, PackBoolTags, MapTags, FilterTagItems, TagInArray) and the
// tag list allocators (AllocateTagItems, CloneTagItems,
// RefreshTagItemClones, FreeTagItems), the
// date conversions (Amiga2Date, Date2Amiga), the case-blind string
// functions (Stricmp, Strnicmp, ToUpper, ToLower), the named objects and
// their name spaces, and GetUniqueID. Case follows ISO 8859-1, as the
// library does without a locale. Every other vector logs "utility:
// unimplemented LVO -<n>" and ends the run with Unimplemented.
//
// A NamedObject's public part is no_Object, the user space it was made
// with; the rest of its block is this library's: its name, its place in a
// name space, the name space it holds, and how many finds hold it.
//
// Every guest pointer goes through native::Bus, so a bad one ends the run
// with BusError or AddressError. A tag list walk stops after a bounded
// number of items, so a list that loops cannot hang the host.

namespace hombre {

// Library vector offsets of utility.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace utility_lvo {

constexpr u32 FindTagItem = 30;
constexpr u32 GetTagData = 36;
constexpr u32 PackBoolTags = 42;
constexpr u32 NextTagItem = 48;
constexpr u32 MapTags = 60;
constexpr u32 AllocateTagItems = 66;
constexpr u32 CloneTagItems = 72;
constexpr u32 FreeTagItems = 78;
constexpr u32 RefreshTagItemClones = 84;
constexpr u32 TagInArray = 90;
constexpr u32 FilterTagItems = 96;
constexpr u32 CallHookPkt = 102;
constexpr u32 Amiga2Date = 120;
constexpr u32 Date2Amiga = 126;
constexpr u32 SMult32 = 138;
constexpr u32 UMult32 = 144;
constexpr u32 SDivMod32 = 150;
constexpr u32 UDivMod32 = 156;
constexpr u32 Stricmp = 162;
constexpr u32 Strnicmp = 168;
constexpr u32 ToUpper = 174;
constexpr u32 ToLower = 180;
constexpr u32 SMult64 = 198;
constexpr u32 UMult64 = 204;
constexpr u32 AddNamedObject = 222;
constexpr u32 AllocNamedObjectA = 228;
constexpr u32 AttemptRemNamedObject = 234;
constexpr u32 FindNamedObject = 240;
constexpr u32 FreeNamedObject = 246;
constexpr u32 NamedObjectName = 252;
constexpr u32 ReleaseNamedObject = 258;
constexpr u32 RemNamedObject = 264;
// The last vector of the 3.1 library; the table reaches down to it.
constexpr u32 GetUniqueID = 270;

}

// Offsets and values of the structures utility works with, named as the
// NDK assembler includes name them.
namespace ndk {

// struct TagItem
constexpr u32 ti_Tag = 0;
constexpr u32 ti_Data = 4;
constexpr u32 ti_SIZEOF = 8;

constexpr u32 TAG_DONE = 0;
constexpr u32 TAG_IGNORE = 1;
constexpr u32 TAG_MORE = 2;
constexpr u32 TAG_SKIP = 3;

// FilterTagItems' logic and MapTags' map type.
constexpr u32 TAGFILTER_AND = 0;
constexpr u32 TAGFILTER_NOT = 1;
constexpr u32 MAP_REMOVE_NOT_FOUND = 0;
constexpr u32 MAP_KEEP_NOT_FOUND = 1;

// struct ClockData, whose fields the includes name sec, min, hour, mday,
// month, year and wday; the cd_ prefix keeps them from clashing.
constexpr u32 cd_sec = 0;
constexpr u32 cd_min = 2;
constexpr u32 cd_hour = 4;
constexpr u32 cd_mday = 6;
constexpr u32 cd_month = 8;
constexpr u32 cd_year = 10;
constexpr u32 cd_wday = 12;
constexpr u32 CD_SIZE = 14;

// struct NamedObject and the AllocNamedObjectA tags and name space flags
// (utility/name.h).
constexpr u32 no_Object = 0;
constexpr u32 ANO_NameSpace = 4000;
constexpr u32 ANO_UserSpace = 4001;
constexpr u32 ANO_Priority = 4002;
constexpr u32 ANO_Flags = 4003;
constexpr u32 NSF_NODUPS = 1;
constexpr u32 NSF_CASE = 2;

// struct UtilityBase
constexpr u32 ub_Language = 34;
constexpr u32 ub_Reserved = 35;
constexpr u32 UtilityBase_SIZEOF = 36;

}

class Utility {
public:
    // Tag list items one walk visits at most.
    static constexpr u32 kMaxTagItems = 65536;

    // Makes utility.library and adds it to exec's library list. exec must
    // be initialised. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec);

    u32 utility_base() const { return base_; }

    // The named object vectors. A null name space is the system's.
    u32 alloc_named_object(u32 name, u32 tags);
    bool add_named_object(u32 space, u32 object);
    bool attempt_rem_named_object(u32 object);
    u32 find_named_object(u32 space, u32 name, u32 last);
    void free_named_object(u32 object);
    u32 named_object_name(u32 object) const;
    void release_named_object(u32 object);
    void rem_named_object(u32 object, u32 message);
    u32 unique_id() { return ++unique_id_; }

    Exec& exec() { return *exec_; }

private:
    static constexpr u32 kVectors = utility_lvo::GetUniqueID / 6;

    void unlink_named_object(u32 object);

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    u32 root_ = 0;  // the object holding the system's name space
    u32 unique_id_ = 0;
    u32 base_ = 0;
    NativeVector vectors_[kVectors]{};
};

}
