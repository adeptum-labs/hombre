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
    Exec& exec = *static_cast<Exec*>(ctx);
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

struct UtilityFunction {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr UtilityFunction kUtilityFunctions[] = {
    {utility_lvo::FindTagItem, find_tag_item, "FindTagItem"},
    {utility_lvo::GetTagData, get_tag_data, "GetTagData"},
    {utility_lvo::NextTagItem, next_tag_item, "NextTagItem"},
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
};

}

bool Utility::init(Machine& m, Exec& exec) {
    base_ = 0;
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
                                              &exec, UtilityBase_SIZEOF);
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
    return true;
}

}
