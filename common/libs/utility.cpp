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

#include "utility.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;
using i32 = __INT32_TYPE__;

namespace {

constexpr char kUtilityName[] = "utility.library";
constexpr char kIdString[] = "utility 60.0 (Hombre)\r\n";

// Days from 1970-01-01 to 1978-01-01, the first day of Amiga time.
constexpr i64 kAmigaEpochDays = 2922;
constexpr u32 kSecondsPerDay = 86400;

// --- tag lists ---------------------------------------------------------------

// The next item of the list at item that is no control tag: TAG_IGNORE is
// passed over, TAG_MORE continues at the list in its ti_Data, TAG_SKIP
// passes over itself and ti_Data more items, and TAG_DONE or a null list
// ends it. item is left after the item returned. Every item visited takes
// one from budget, which the whole walk shares. 0 at the end, if a guest
// access faults, or when the budget runs out, which is logged.
u32 next_tag(Bus& b, u32& item, u32& budget) {
    for (; budget != 0; --budget) {
        if (item == 0) {
            return 0;
        }
        const u32 tag = b.r32(item + ti_Tag);
        if (!b.ok()) {
            return 0;
        }
        switch (tag) {
        case TAG_DONE:
            return 0;
        case TAG_IGNORE:
            item += ti_SIZEOF;
            break;
        case TAG_MORE:
            item = b.r32(item + ti_Data);
            break;
        case TAG_SKIP:
            item += ti_SIZEOF * (b.r32(item + ti_Data) + 1);
            break;
        default: {
            const u32 found = item;
            item += ti_SIZEOF;
            --budget;
            return found;
        }
        }
    }
    LogLine().text("utility: tag list at ").hex(item).text(" has no end").send();
    return 0;
}

// The first item of the list with the tag, within one walk of at most
// Utility::kMaxTagItems items.
u32 find_tag(Bus& b, u32 tag, u32 list) {
    u32 budget = Utility::kMaxTagItems;
    for (;;) {
        const u32 item = next_tag(b, list, budget);
        if (item == 0) {
            return 0;
        }
        if (b.r32(item + ti_Tag) == tag) {
            return b.ok() ? item : 0;
        }
    }
}

// --- dates -------------------------------------------------------------------

// Days since 1970-01-01 of a day of the proleptic Gregorian calendar.
i64 days_from_civil(i64 y, i64 m, i64 d) {
    y -= m <= 2 ? 1 : 0;
    const i64 era = (y >= 0 ? y : y - 399) / 400;
    const i64 yoe = y - era * 400;
    const i64 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The day, for 0 <= days (since 1970-01-01).
void civil_from_days(i64 days, u32& y, u32& m, u32& d) {
    const i64 z = days + 719468;
    const i64 era = z / 146097;
    const i64 doe = z - era * 146097;
    const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const i64 mp = (5 * doy + 2) / 153;
    d = static_cast<u32>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<u32>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<u32>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

// --- case ----------------------------------------------------------------------

// ISO 8859-1: a-z and the accented letters but the division sign; the
// lowercase y with diaeresis has no uppercase form in the set.
u32 upper(u32 c) {
    c &= 0xFF;
    return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? c - 0x20 : c;
}

u32 lower(u32 c) {
    c &= 0xFF;
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 0x20 : c;
}

// Compares at most count characters of two guest strings without regard to
// case: the difference of the first pair that differs, 0 if none does.
// Every step reads one byte further on, so the walk ends at the end of
// guest memory at the latest.
u32 compare(Bus& b, u32 s1, u32 s2, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const u32 c1 = upper(b.r8(s1 + i));
        const u32 c2 = upper(b.r8(s2 + i));
        if (!b.ok()) {
            return 0;
        }
        if (c1 != c2) {
            return c1 - c2;  // the sign survives in two's complement
        }
        if (c1 == 0) {
            break;
        }
    }
    return 0;
}

// --- the natives -------------------------------------------------------------

void find_tag_item(void*, Machine& m) {
    Bus b(m);
    native::set_result(m, find_tag(b, m.cpu.d[0], m.cpu.a[0]));
}

void get_tag_data(void*, Machine& m) {
    Bus b(m);
    const u32 item = find_tag(b, m.cpu.d[0], m.cpu.a[0]);
    const u32 data = item != 0 ? b.r32(item + ti_Data) : m.cpu.d[1];
    if (b.ok()) {
        native::set_result(m, data);
    }
}

// A0 points at the walk's position, which is moved on.
void next_tag_item(void*, Machine& m) {
    Bus b(m);
    const u32 at = m.cpu.a[0];
    u32 item = b.r32(at);
    u32 budget = Utility::kMaxTagItems;
    const u32 found = next_tag(b, item, budget);
    b.w32(at, found != 0 ? item : 0);
    if (b.ok()) {
        native::set_result(m, found);
    }
}

// Seconds since 1978-01-01 into the ClockData at A0.
void amiga2date(void*, Machine& m) {
    Bus b(m);
    const u32 seconds = m.cpu.d[0];
    const u32 at = m.cpu.a[0];
    const u32 days = seconds / kSecondsPerDay;
    const u32 rest = seconds % kSecondsPerDay;
    u32 y = 0;
    u32 mo = 0;
    u32 d = 0;
    civil_from_days(kAmigaEpochDays + days, y, mo, d);
    b.w16(at + cd_sec, rest % 60);
    b.w16(at + cd_min, rest / 60 % 60);
    b.w16(at + cd_hour, rest / 3600);
    b.w16(at + cd_mday, d);
    b.w16(at + cd_month, mo);
    b.w16(at + cd_year, y);
    b.w16(at + cd_wday, days % 7);  // 1978-01-01 was a Sunday, day 0
}

// The ClockData at A0 as seconds since 1978-01-01. Like the library, it
// does not check the date; CheckDate is there for that.
void date2amiga(void*, Machine& m) {
    Bus b(m);
    const u32 at = m.cpu.a[0];
    const i64 s = b.r16(at + cd_sec);
    const i64 mi = b.r16(at + cd_min);
    const i64 h = b.r16(at + cd_hour);
    const i64 d = b.r16(at + cd_mday);
    const i64 mo = b.r16(at + cd_month);
    const i64 y = b.r16(at + cd_year);
    if (!b.ok()) {
        return;
    }
    const i64 days = days_from_civil(y, mo, d) - kAmigaEpochDays;
    const i64 seconds = days * kSecondsPerDay + h * 3600 + mi * 60 + s;
    native::set_result(m, static_cast<u32>(static_cast<u64>(seconds)));
}

void stricmp(void*, Machine& m) {
    Bus b(m);
    const u32 result = compare(b, m.cpu.a[0], m.cpu.a[1], 0xFFFFFFFF);
    if (b.ok()) {
        native::set_result(m, result);
    }
}

void strnicmp(void*, Machine& m) {
    Bus b(m);
    const u32 result = compare(b, m.cpu.a[0], m.cpu.a[1], m.cpu.d[0]);
    if (b.ok()) {
        native::set_result(m, result);
    }
}

// The math vectors. A division by zero gives 0 and leaves the dividend as
// the remainder instead of trapping.
void smult32(void*, Machine& m) {
    const i64 product = static_cast<i64>(static_cast<i32>(m.cpu.d[0])) *
                        static_cast<i32>(m.cpu.d[1]);
    native::set_result(m, static_cast<u32>(product));
}

void umult32(void*, Machine& m) { native::set_result(m, m.cpu.d[0] * m.cpu.d[1]); }

void smult64(void*, Machine& m) {
    const u64 product = static_cast<u64>(static_cast<i64>(static_cast<i32>(m.cpu.d[0])) *
                                         static_cast<i32>(m.cpu.d[1]));
    m.cpu.d[1] = static_cast<u32>(product >> 32);
    native::set_result(m, static_cast<u32>(product));
}

void umult64(void*, Machine& m) {
    const u64 product = static_cast<u64>(m.cpu.d[0]) * m.cpu.d[1];
    m.cpu.d[1] = static_cast<u32>(product >> 32);
    native::set_result(m, static_cast<u32>(product));
}

void sdivmod32(void*, Machine& m) {
    const i32 dividend = static_cast<i32>(m.cpu.d[0]);
    const i32 divisor = static_cast<i32>(m.cpu.d[1]);
    if (divisor == 0 || (dividend == static_cast<i32>(0x80000000u) && divisor == -1)) {
        m.cpu.d[1] = divisor == 0 ? static_cast<u32>(dividend) : 0;
        native::set_result(m, divisor == 0 ? 0 : static_cast<u32>(dividend));
        return;
    }
    m.cpu.d[1] = static_cast<u32>(dividend % divisor);
    native::set_result(m, static_cast<u32>(dividend / divisor));
}

void udivmod32(void*, Machine& m) {
    const u32 dividend = m.cpu.d[0];
    const u32 divisor = m.cpu.d[1];
    if (divisor == 0) {
        m.cpu.d[1] = dividend;
        native::set_result(m, 0);
        return;
    }
    m.cpu.d[1] = dividend % divisor;
    native::set_result(m, dividend / divisor);
}

void to_upper(void*, Machine& m) { native::set_result(m, upper(m.cpu.d[0])); }
void to_lower(void*, Machine& m) { native::set_result(m, lower(m.cpu.d[0])); }

// struct Hook: h_Entry, the code CallHookPkt runs.
constexpr u32 h_Entry = 8;
// Blocks one hook call may run.
constexpr u64 kHookBlocks = 1ull << 24;

// Calls the hook at A0 with the object in A2 and the message in A1, as its
// h_Entry expects them, and returns its D0. Every other register is as it
// was. A null hook or entry returns 0.
void call_hook_pkt(void* ctx, Machine& m) {
    Exec& exec = static_cast<Utility*>(ctx)->exec();
    Bus b(m);
    const u32 hook = m.cpu.a[0];
    const u32 entry = hook != 0 ? b.r32(hook + h_Entry) : 0;
    if (!b.ok()) {
        return;
    }
    if (entry == 0) {
        native::set_result(m, 0);
        return;
    }
    if (!exec.enter_nested_run()) {
        native::set_result(m, 0);
        return;
    }
    u32 d[8];
    u32 a[8];
    for (u32 i = 0; i < 8; ++i) {
        d[i] = m.cpu.d[i];
        a[i] = m.cpu.a[i];
    }
    const m68k::ExitReason why = m.jit->call_guest(entry, kHookBlocks);
    exec.leave_nested_run();
    if (why != m68k::ExitReason::Returned) {
        return;  // the run ended inside the hook
    }
    const u32 result = m.cpu.d[0];
    for (u32 i = 0; i < 8; ++i) {
        m.cpu.d[i] = d[i];
        m.cpu.a[i] = a[i];
    }
    m.cpu.d[0] = result;
}

// --- tag list editing ------------------------------------------------------

// The tag lists AllocateTagItems and CloneTagItems hand out carry their
// item count in the long before them, for FreeTagItems.
constexpr u32 kTagHeader = 4;

u32 alloc_tags(Exec& exec, Bus& b, u32 count) {
    if (count >= Utility::kMaxTagItems) {
        return 0;
    }
    const u32 block = exec.alloc(kTagHeader + count * ti_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
    if (block == 0) {
        return 0;
    }
    b.w32(block, count);
    return block + kTagHeader;
}

bool in_array(Bus& b, u32 tag, u32 array) {
    for (u32 i = 0; array != 0 && i < Utility::kMaxTagItems; ++i) {
        const u32 entry = b.r32(array + i * 4);
        if (!b.ok() || entry == TAG_DONE) {
            return false;
        }
        if (entry == tag) {
            return true;
        }
    }
    return false;
}

// PackBoolTags(flags D0, tags A0, boolean map A1): each tag of the map
// that the list has sets its flag bits when its data is not 0, clears
// them when it is.
void pack_bool_tags(void*, Machine& m) {
    Bus b(m);
    u32 flags = m.cpu.d[0];
    u32 map = m.cpu.a[1];
    u32 budget = Utility::kMaxTagItems;
    for (;;) {
        const u32 entry = next_tag(b, map, budget);
        if (entry == 0) {
            break;
        }
        const u32 item = find_tag(b, b.r32(entry + ti_Tag), m.cpu.a[0]);
        if (item != 0) {
            const u32 bits = b.r32(entry + ti_Data);
            flags = b.r32(item + ti_Data) != 0 ? flags | bits : flags & ~bits;
        }
    }
    if (b.ok()) {
        native::set_result(m, flags);
    }
}

// TagInArray(tag D0, array A0): whether the TAG_DONE-ended array of tags
// holds it.
void tag_in_array(void*, Machine& m) {
    Bus b(m);
    const bool found = in_array(b, m.cpu.d[0], m.cpu.a[0]);
    if (b.ok()) {
        native::set_result(m, found ? 1 : 0);
    }
}

// FilterTagItems(tags A0, array A1, logic D0): with TAGFILTER_AND the
// items whose tags the array lacks become TAG_IGNORE, with TAGFILTER_NOT
// those it has. The number of items left.
void filter_tag_items(void*, Machine& m) {
    Bus b(m);
    u32 at = m.cpu.a[0];
    u32 budget = Utility::kMaxTagItems;
    u32 left = 0;
    for (;;) {
        const u32 item = next_tag(b, at, budget);
        if (item == 0) {
            break;
        }
        const bool listed = in_array(b, b.r32(item + ti_Tag), m.cpu.a[1]);
        if (listed == (m.cpu.d[0] == TAGFILTER_NOT)) {
            b.w32(item + ti_Tag, TAG_IGNORE);
        } else {
            ++left;
        }
    }
    if (b.ok()) {
        native::set_result(m, left);
    }
}

// MapTags(tags A0, map A1, type D0): each item takes the tag the map
// gives its own; one the map lacks becomes TAG_IGNORE, or stays with
// MAP_KEEP_NOT_FOUND.
void map_tags(void*, Machine& m) {
    Bus b(m);
    u32 at = m.cpu.a[0];
    u32 budget = Utility::kMaxTagItems;
    for (;;) {
        const u32 item = next_tag(b, at, budget);
        if (item == 0) {
            break;
        }
        const u32 to = find_tag(b, b.r32(item + ti_Tag), m.cpu.a[1]);
        if (to != 0) {
            b.w32(item + ti_Tag, b.r32(to + ti_Data));
        } else if (m.cpu.d[0] == MAP_REMOVE_NOT_FOUND) {
            b.w32(item + ti_Tag, TAG_IGNORE);
        }
    }
}

// AllocateTagItems(count D0): a cleared list of so many items.
void allocate_tag_items(void* ctx, Machine& m) {
    Bus b(m);
    native::set_result(m, alloc_tags(static_cast<Utility*>(ctx)->exec(), b, m.cpu.d[0]));
}

// CloneTagItems(tags A0): a new list of the items of the list, its control
// tags followed, ended with TAG_DONE.
void clone_tag_items(void* ctx, Machine& m) {
    Bus b(m);
    u32 at = m.cpu.a[0];
    u32 budget = Utility::kMaxTagItems;
    u32 count = 0;
    while (next_tag(b, at, budget) != 0) {
        ++count;
    }
    const u32 clone = b.ok() ? alloc_tags(static_cast<Utility*>(ctx)->exec(), b, count + 1) : 0;
    if (clone == 0) {
        native::set_result(m, 0);
        return;
    }
    at = m.cpu.a[0];
    budget = Utility::kMaxTagItems;
    for (u32 i = 0; i < count; ++i) {
        const u32 item = next_tag(b, at, budget);
        b.w32(clone + i * ti_SIZEOF + ti_Tag, b.r32(item + ti_Tag));
        b.w32(clone + i * ti_SIZEOF + ti_Data, b.r32(item + ti_Data));
    }
    b.w32(clone + count * ti_SIZEOF + ti_Tag, TAG_DONE);
    native::set_result(m, clone);
}

// RefreshTagItemClones(clone A0, original A1): the clone's items take the
// original's data again.
void refresh_tag_item_clones(void*, Machine& m) {
    Bus b(m);
    u32 at = m.cpu.a[1];
    u32 budget = Utility::kMaxTagItems;
    for (u32 i = 0; m.cpu.a[0] != 0; ++i) {
        const u32 item = next_tag(b, at, budget);
        const u32 clone = m.cpu.a[0] + i * ti_SIZEOF;
        if (item == 0 || b.r32(clone + ti_Tag) == TAG_DONE || !b.ok()) {
            break;
        }
        b.w32(clone + ti_Tag, b.r32(item + ti_Tag));
        b.w32(clone + ti_Data, b.r32(item + ti_Data));
    }
}

// FreeTagItems(tags A0): gives back a list AllocateTagItems or
// CloneTagItems made.
void free_tag_items(void* ctx, Machine& m) {
    const u32 list = m.cpu.a[0];
    if (list < kTagHeader) {
        return;
    }
    Bus b(m);
    const u32 count = b.r32(list - kTagHeader);
    if (b.ok()) {
        static_cast<Utility*>(ctx)->exec().free(list - kTagHeader,
                                                kTagHeader + count * ti_SIZEOF);
    }
}

// --- named objects -------------------------------------------------------------

Utility& of(void* ctx) { return *static_cast<Utility*>(ctx); }

void alloc_named_object(void* ctx, Machine& m) {
    native::set_result(m, of(ctx).alloc_named_object(m.cpu.a[0], m.cpu.a[1]));
}

void add_named_object(void* ctx, Machine& m) {
    native::set_result(m, of(ctx).add_named_object(m.cpu.a[0], m.cpu.a[1]) ? 1 : 0);
}

void attempt_rem_named_object(void* ctx, Machine& m) {
    native::set_result(m, of(ctx).attempt_rem_named_object(m.cpu.a[0]) ? 1 : 0);
}

void find_named_object(void* ctx, Machine& m) {
    native::set_result(m, of(ctx).find_named_object(m.cpu.a[0], m.cpu.a[1], m.cpu.a[2]));
}

void free_named_object(void* ctx, Machine& m) { of(ctx).free_named_object(m.cpu.a[0]); }

void named_object_name(void* ctx, Machine& m) {
    native::set_result(m, of(ctx).named_object_name(m.cpu.a[0]));
}

void release_named_object(void* ctx, Machine& m) { of(ctx).release_named_object(m.cpu.a[0]); }

void rem_named_object(void* ctx, Machine& m) {
    of(ctx).rem_named_object(m.cpu.a[0], m.cpu.a[1]);
}

void get_unique_id(void* ctx, Machine& m) { native::set_result(m, of(ctx).unique_id()); }

struct UtilityFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr UtilityFunction kUtilityFunctions[] = {
    {utility_lvo::FindTagItem, find_tag_item, "FindTagItem"},
    {utility_lvo::GetTagData, get_tag_data, "GetTagData"},
    {utility_lvo::NextTagItem, next_tag_item, "NextTagItem"},
    {utility_lvo::PackBoolTags, pack_bool_tags, "PackBoolTags"},
    {utility_lvo::MapTags, map_tags, "MapTags"},
    {utility_lvo::AllocateTagItems, allocate_tag_items, "AllocateTagItems"},
    {utility_lvo::CloneTagItems, clone_tag_items, "CloneTagItems"},
    {utility_lvo::FreeTagItems, free_tag_items, "FreeTagItems"},
    {utility_lvo::RefreshTagItemClones, refresh_tag_item_clones, "RefreshTagItemClones"},
    {utility_lvo::TagInArray, tag_in_array, "TagInArray"},
    {utility_lvo::FilterTagItems, filter_tag_items, "FilterTagItems"},
    {utility_lvo::CallHookPkt, call_hook_pkt, "CallHookPkt"},
    {utility_lvo::Amiga2Date, amiga2date, "Amiga2Date"},
    {utility_lvo::Date2Amiga, date2amiga, "Date2Amiga"},
    {utility_lvo::SMult32, smult32, "SMult32"},
    {utility_lvo::UMult32, umult32, "UMult32"},
    {utility_lvo::SDivMod32, sdivmod32, "SDivMod32"},
    {utility_lvo::UDivMod32, udivmod32, "UDivMod32"},
    {utility_lvo::SMult64, smult64, "SMult64"},
    {utility_lvo::UMult64, umult64, "UMult64"},
    {utility_lvo::Stricmp, stricmp, "Stricmp"},
    {utility_lvo::Strnicmp, strnicmp, "Strnicmp"},
    {utility_lvo::ToUpper, to_upper, "ToUpper"},
    {utility_lvo::ToLower, to_lower, "ToLower"},
    {utility_lvo::AddNamedObject, add_named_object, "AddNamedObject"},
    {utility_lvo::AllocNamedObjectA, alloc_named_object, "AllocNamedObjectA"},
    {utility_lvo::AttemptRemNamedObject, attempt_rem_named_object, "AttemptRemNamedObject"},
    {utility_lvo::FindNamedObject, find_named_object, "FindNamedObject"},
    {utility_lvo::FreeNamedObject, free_named_object, "FreeNamedObject"},
    {utility_lvo::NamedObjectName, named_object_name, "NamedObjectName"},
    {utility_lvo::ReleaseNamedObject, release_named_object, "ReleaseNamedObject"},
    {utility_lvo::RemNamedObject, rem_named_object, "RemNamedObject"},
    {utility_lvo::GetUniqueID, get_unique_id, "GetUniqueID"},
};

// The block of a named object: no_Object first, then this library's part,
// then the name and, 8-byte aligned, the user space.
constexpr u32 kNoNext = 4;        // the next object of the same name space
constexpr u32 kNoParent = 8;      // the object whose name space holds it, 0 if none
constexpr u32 kNoName = 12;       // the name, the copy in the block
constexpr u32 kNoFirst = 16;      // the first object of the name space it holds
constexpr u32 kNoSpace = 20;      // its name space's NSF_ flags, kHasSpace if it has one
constexpr u32 kNoUseCount = 24;   // the finds that hold it
constexpr u32 kNoPriority = 28;
constexpr u32 kNoRemoving = 32;   // the message RemNamedObject waits to reply, or kNoMessage
constexpr u32 kNoBytes = 36;      // the size of the block
constexpr u32 kNoNameAt = 40;
constexpr u32 kHasSpace = 0x80000000;
constexpr u32 kNotRemoving = 0;
constexpr u32 kRemovingNoMessage = 1;
constexpr u32 kMaxObjectName = 256;
// Objects one name space walk visits at most, so a list that loops ends.
constexpr u32 kMaxSpaceWalk = 65536;

}

bool Utility::init(Machine& m, Exec& exec) {
    base_ = 0;
    machine_ = &m;
    exec_ = &exec;
    root_ = 0;
    unique_id_ = 0;
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const UtilityFunction& f : kUtilityFunctions) {
        vectors_[f.lvo / LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }
    const u32 id_string = exec.alloc(sizeof kIdString, MEMF_PUBLIC);
    if (id_string == 0) {
        return false;
    }
    const u32 base = exec.make_native_library(kUtilityName, kHombreLibVersion, vectors_, kVectors,
                                              this, UtilityBase_SIZEOF);
    if (base == 0) {
        exec.free(id_string, sizeof kIdString);
        return false;
    }
    Bus b(m);
    for (u32 i = 0; i < sizeof kIdString; ++i) {
        b.w8(id_string + i, static_cast<u8>(kIdString[i]));
    }
    b.w16(base + LIB_REVISION, kHombreLibRevision);
    b.w32(base + LIB_IDSTRING, id_string);
    if (!b.ok()) {
        return false;
    }
    base_ = base;
    // The system's name space: names in it are case-blind and may repeat.
    root_ = alloc_named_object(0, 0);
    if (root_ == 0) {
        return false;
    }
    Bus rb(m);
    rb.w32(root_ + kNoSpace, kHasSpace);
    return rb.ok();
}

// AllocNamedObjectA: a new object named name, not yet in a name space.
// ANO_NameSpace gives it a name space of its own, with ANO_Flags as its
// NSF_ flags; ANO_UserSpace bytes of cleared memory become its no_Object.
u32 Utility::alloc_named_object(u32 name, u32 tags) {
    Bus b(*machine_);
    u32 length = 0;
    if (name != 0) {
        while (length < kMaxObjectName && b.r8(name + length) != 0) {
            ++length;
        }
        if (!b.ok() || length == kMaxObjectName) {
            return 0;
        }
    }
    auto tag = [&](u32 t, u32 fallback) {
        const u32 item = find_tag(b, t, tags);
        return item != 0 ? b.r32(item + ti_Data) : fallback;
    };
    const bool space = tag(ANO_NameSpace, 0) != 0;
    const u32 user = tag(ANO_UserSpace, 0);
    const u32 priority = tag(ANO_Priority, 0);
    const u32 flags = tag(ANO_Flags, 0) & (NSF_NODUPS | NSF_CASE);
    if (!b.ok() || user > 0x10000000) {
        return 0;
    }
    const u32 user_at = (kNoNameAt + length + 1 + 7) & ~7u;
    const u32 bytes = user_at + user;
    const u32 object = exec_->alloc(bytes, MEMF_PUBLIC | MEMF_CLEAR);
    if (object == 0) {
        return 0;
    }
    for (u32 i = 0; i < length; ++i) {
        b.w8(object + kNoNameAt + i, b.r8(name + i));
    }
    b.w32(object + no_Object, user != 0 ? object + user_at : 0);
    b.w32(object + kNoName, object + kNoNameAt);
    b.w32(object + kNoSpace, space ? kHasSpace | flags : 0);
    b.w32(object + kNoPriority, static_cast<u32>(static_cast<i32>(static_cast<i8>(priority))));
    b.w32(object + kNoBytes, bytes);
    if (!b.ok()) {
        exec_->free(object, bytes);
        return 0;
    }
    return object;
}

// The name space object that space names: the system's for 0. 0 if the
// object holds no name space.
static u32 space_of(Bus& b, u32 space, u32 root) {
    const u32 object = space != 0 ? space : root;
    return object != 0 && (b.r32(object + kNoSpace) & kHasSpace) != 0 ? object : 0;
}

static bool same_name(Bus& b, u32 a, u32 c, bool exact) {
    for (u32 i = 0; i < kMaxObjectName; ++i) {
        const u32 x = b.r8(a + i);
        const u32 y = b.r8(c + i);
        if (!b.ok()) {
            return false;
        }
        if (exact ? x != y : upper(x) != upper(y)) {
            return false;
        }
        if (x == 0) {
            return true;
        }
    }
    return false;
}

// AddNamedObject: puts the object into the name space, after the objects
// of its priority or higher. False if the object is in one already, the
// name space is none, or it takes no duplicates and has the name.
bool Utility::add_named_object(u32 space, u32 object) {
    Bus b(*machine_);
    const u32 holder = space_of(b, space, root_);
    if (holder == 0 || object == 0 || b.r32(object + kNoParent) != 0) {
        return false;
    }
    const u32 flags = b.r32(holder + kNoSpace);
    const u32 name = b.r32(object + kNoName);
    const i32 priority = static_cast<i32>(b.r32(object + kNoPriority));
    u32 previous = 0;
    u32 at = b.r32(holder + kNoFirst);
    u32 insert_after = 0;
    bool placed = false;
    for (u32 step = 0; at != 0 && step < kMaxSpaceWalk; ++step) {
        if ((flags & NSF_NODUPS) != 0 &&
            same_name(b, b.r32(at + kNoName), name, (flags & NSF_CASE) != 0)) {
            return false;
        }
        if (!placed && static_cast<i32>(b.r32(at + kNoPriority)) < priority) {
            insert_after = previous;
            placed = true;
        }
        previous = at;
        at = b.r32(at + kNoNext);
    }
    if (!b.ok()) {
        return false;
    }
    if (!placed) {
        insert_after = previous;
    }
    const u32 next = insert_after != 0 ? b.r32(insert_after + kNoNext) : b.r32(holder + kNoFirst);
    b.w32(object + kNoNext, next);
    b.w32(insert_after != 0 ? insert_after + kNoNext : holder + kNoFirst, object);
    b.w32(object + kNoParent, holder);
    return b.ok();
}

// Takes the object out of its name space, if it is in one.
void Utility::unlink_named_object(u32 object) {
    Bus b(*machine_);
    const u32 holder = b.r32(object + kNoParent);
    if (holder == 0 || !b.ok()) {
        return;
    }
    u32 link = holder + kNoFirst;
    for (u32 step = 0; step < kMaxSpaceWalk; ++step) {
        const u32 at = b.r32(link);
        if (at == 0 || !b.ok()) {
            break;
        }
        if (at == object) {
            b.w32(link, b.r32(object + kNoNext));
            break;
        }
        link = at + kNoNext;
    }
    b.w32(object + kNoNext, 0);
    b.w32(object + kNoParent, 0);
}

// FindNamedObject: the next object after last in the name space whose
// name matches, any object for a null name. A found object is held until
// ReleaseNamedObject; one being removed is passed over.
u32 Utility::find_named_object(u32 space, u32 name, u32 last) {
    Bus b(*machine_);
    const u32 holder = space_of(b, space, root_);
    if (holder == 0) {
        return 0;
    }
    const bool exact = (b.r32(holder + kNoSpace) & NSF_CASE) != 0;
    u32 at = last != 0 && b.r32(last + kNoParent) == holder ? b.r32(last + kNoNext)
                                                             : b.r32(holder + kNoFirst);
    for (u32 step = 0; at != 0 && step < kMaxSpaceWalk; ++step) {
        if (b.r32(at + kNoRemoving) == kNotRemoving &&
            (name == 0 || same_name(b, b.r32(at + kNoName), name, exact))) {
            b.w32(at + kNoUseCount, b.r32(at + kNoUseCount) + 1);
            return b.ok() ? at : 0;
        }
        at = b.r32(at + kNoNext);
    }
    return 0;
}

// ReleaseNamedObject: lets go of a find. The last one to let go of an
// object RemNamedObject is waiting for removes it and replies the message.
void Utility::release_named_object(u32 object) {
    if (object == 0) {
        return;
    }
    Bus b(*machine_);
    const u32 count = b.r32(object + kNoUseCount);
    if (!b.ok() || count == 0) {
        return;
    }
    b.w32(object + kNoUseCount, count - 1);
    const u32 removing = b.r32(object + kNoRemoving);
    if (count == 1 && removing != kNotRemoving) {
        unlink_named_object(object);
        b.w32(object + kNoRemoving, kNotRemoving);
        if (removing != kRemovingNoMessage) {
            exec_->reply_message(removing);
        }
    }
}

// AttemptRemNamedObject: removes the object the caller holds from its name
// space, unless some other find still holds it.
bool Utility::attempt_rem_named_object(u32 object) {
    if (object == 0) {
        return false;
    }
    Bus b(*machine_);
    const u32 count = b.r32(object + kNoUseCount);
    if (!b.ok() || count > 1) {
        return false;
    }
    b.w32(object + kNoUseCount, 0);
    unlink_named_object(object);
    return b.ok();
}

// RemNamedObject: removes the object from its name space once nothing
// holds it; the caller's own find is let go of. The message, if any, is
// replied when the object is out.
void Utility::rem_named_object(u32 object, u32 message) {
    if (object == 0) {
        return;
    }
    Bus b(*machine_);
    const u32 count = b.r32(object + kNoUseCount);
    if (!b.ok()) {
        return;
    }
    if (count <= 1) {
        b.w32(object + kNoUseCount, 0);
        unlink_named_object(object);
        if (message != 0) {
            exec_->reply_message(message);
        }
        return;
    }
    b.w32(object + kNoRemoving, message != 0 ? message : kRemovingNoMessage);
    b.w32(object + kNoUseCount, count - 1);
}

// FreeNamedObject: gives the object's memory back; it must be in no name
// space. The objects of its own name space stay as they are.
void Utility::free_named_object(u32 object) {
    if (object == 0 || object == root_) {
        return;
    }
    Bus b(*machine_);
    const u32 bytes = b.r32(object + kNoBytes);
    if (!b.ok() || bytes < kNoNameAt) {
        return;
    }
    unlink_named_object(object);
    exec_->free(object, bytes);
}

u32 Utility::named_object_name(u32 object) const {
    if (object == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 name = b.r32(object + kNoName);
    return b.ok() ? name : 0;
}

}
