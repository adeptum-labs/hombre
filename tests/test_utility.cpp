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

#include "libs/utility.hpp"
#include "check.hpp"

#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;

namespace {

constexpr u32 kCode = 0x1000;
constexpr u32 kCall = 0x1800;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;  // test structures up to 0xFFFF
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;
constexpr u32 kTagUser = 0x80000000;

// Static storage, as the kernel keeps them.
Jit g_jit;
Exec g_exec;
Utility g_utility;
std::vector<std::string> g_lines;

void capture(const char* line) {
    g_lines.push_back(line);
}

bool logged(const std::string& text) {
    for (const std::string& line : g_lines) {
        if (line == text) {
            return true;
        }
    }
    return false;
}

bool logged_containing(const std::string& text) {
    for (const std::string& line : g_lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

u16 disp(u32 offset) {
    return static_cast<u16>(0x10000 - offset);
}

// One memfd mapped twice for the code region, 16 MiB of guest memory, the
// Jit, exec and utility initialised over them, the log captured.
class Rig {
public:
    Rig() : memory_(kGuestBytes) {
        fd_ = memfd_create("utility", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        m.cpu = CpuState{};
        m.mem = GuestMemory{memory_.data(), kGuestBytes};
        m.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, kRegion}, m));
        g_lines.clear();
        g_log = capture;
        CHECK(g_exec.init(m, kFreeStart, kFreeEnd));
        CHECK(g_utility.init(m, g_exec));
        m.cpu.a[7] = kStack;
    }

    ~Rig() {
        g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;

    CpuState& cpu() { return m.cpu; }
    u32 base() const { return g_utility.utility_base(); }

    void put8(u32 at, u32 v) { memory_[at] = static_cast<u8>(v); }

    void put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
    }

    void put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
    }

    u32 get8(u32 at) const { return memory_[at]; }
    u32 get16(u32 at) const { return (get8(at) << 8) | get8(at + 1); }
    u32 get32(u32 at) const { return (get16(at) << 16) | get16(at + 2); }

    u32 str(u32 at, const char* s) {
        for (usize i = 0; i <= std::strlen(s); ++i) {
            put8(at + static_cast<u32>(i), static_cast<u8>(s[i]));
        }
        return at;
    }

    std::string read_str(u32 at) const {
        std::string s;
        while (memory_[at] != 0) {
            s += static_cast<char>(memory_[at++]);
        }
        return s;
    }

    // Tag items (tag, data) from at on.
    u32 tags(u32 at, std::initializer_list<std::pair<u32, u32>> items) {
        u32 p = at;
        for (const auto& [tag, data] : items) {
            put32(p, tag);
            put32(p + 4, data);
            p += 8;
        }
        return at;
    }

    // JSR -offset(A6) with A6 = UtilityBase, from hand-written 68000 code.
    ExitReason call(u32 offset) {
        put16(kCall, 0x4EAE);
        put16(kCall + 2, disp(offset));
        put16(kCall + 4, 0x4EF8);  // JMP $400.w
        put16(kCall + 6, 0x0400);
        g_jit.flush();
        m.cpu.a[6] = base();
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCall;
        return g_jit.run(1000000);
    }

    // Calls the vector, which must return; D0 is what it gave.
    u32 result(u32 offset) {
        CHECK_EQ(call(offset), ExitReason::Halt);
        return m.cpu.d[0];
    }

    u32 get_tag_data(u32 tag, u32 fallback, u32 list) {
        m.cpu.d[0] = tag;
        m.cpu.d[1] = fallback;
        m.cpu.a[0] = list;
        return result(utility_lvo::GetTagData);
    }

    u32 find_tag_item(u32 tag, u32 list) {
        m.cpu.d[0] = tag;
        m.cpu.a[0] = list;
        return result(utility_lvo::FindTagItem);
    }

    void clock(u32 at, u32 y, u32 mo, u32 d, u32 h, u32 mi, u32 s) {
        put16(at + cd_sec, s);
        put16(at + cd_min, mi);
        put16(at + cd_hour, h);
        put16(at + cd_mday, d);
        put16(at + cd_month, mo);
        put16(at + cd_year, y);
        put16(at + cd_wday, 0);
    }

    u32 date2amiga(u32 y, u32 mo, u32 d, u32 h, u32 mi, u32 s) {
        clock(kData, y, mo, d, h, mi, s);
        m.cpu.a[0] = kData;
        return result(utility_lvo::Date2Amiga);
    }

    u32 compare(const char* a, const char* b, u32 lvo = utility_lvo::Stricmp, u32 count = 0) {
        m.cpu.a[0] = str(kData, a);
        m.cpu.a[1] = str(kData + 0x100, b);
        m.cpu.d[0] = count;
        return result(lvo);
    }

private:
    static constexpr usize kRegion = 1 << 20;

    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

void utility_base_is_built() {
    Rig r;
    const u32 lib = r.base();
    CHECK(lib != 0);
    CHECK_EQ(lib % 2, 0u);
    CHECK(r.read_str(r.get32(lib + LN_NAME)) == "utility.library");
    CHECK_EQ(r.get8(lib + LN_TYPE), static_cast<u32>(NT_LIBRARY));
    CHECK_EQ(r.get16(lib + LIB_VERSION), kHombreLibVersion);
    CHECK_EQ(r.get16(lib + LIB_REVISION), kHombreLibRevision);
    CHECK(r.read_str(r.get32(lib + LIB_IDSTRING)) == "utility 60.0 (Hombre)\r\n");
    CHECK_EQ(r.get16(lib + LIB_POSSIZE), UtilityBase_SIZEOF);
    CHECK_EQ(r.get16(lib + LIB_NEGSIZE), (utility_lvo::GetUniqueID + 3) & ~3u);
    CHECK(g_lines.empty());

    // OpenLibrary finds it in exec's list.
    r.str(kData, "utility.library");
    r.cpu().a[1] = kData;
    r.cpu().d[0] = 37;
    r.put16(kCall, 0x4EAE);
    r.put16(kCall + 2, disp(exec_lvo::OpenLibrary));
    r.put16(kCall + 4, 0x4EF8);
    r.put16(kCall + 6, 0x0400);
    g_jit.flush();
    r.cpu().a[6] = g_exec.exec_base();
    r.cpu().pc = kCall;
    CHECK_EQ(g_jit.run(1000000), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], lib);
    CHECK_EQ(r.get16(lib + LIB_OPENCNT), 1u);
}

void unimplemented_vectors_are_logged() {
    Rig r;
    CHECK_EQ(r.call(42), ExitReason::Unimplemented);  // PackBoolTags
    CHECK(logged("utility: unimplemented LVO -42"));
    CHECK_EQ(r.cpu().pc, r.base() - 42);
    g_lines.clear();
    CHECK_EQ(r.call(utility_lvo::GetUniqueID), ExitReason::Unimplemented);
    CHECK(logged("utility: unimplemented LVO -270"));
}

void get_tag_data_and_find_tag_item() {
    Rig r;
    const u32 more = r.tags(kData + 0x200, {{kTagUser + 7, 70}, {TAG_DONE, 0}});
    const u32 list = r.tags(kData, {{kTagUser + 1, 10},
                                    {TAG_IGNORE, kTagUser + 2},
                                    {TAG_SKIP, 1},
                                    {kTagUser + 3, 30},  // skipped
                                    {kTagUser + 4, 40},
                                    {TAG_MORE, more},
                                    {kTagUser + 9, 90}});  // never reached
    CHECK_EQ(r.get_tag_data(kTagUser + 1, 5, list), 10u);
    CHECK_EQ(r.get_tag_data(kTagUser + 4, 5, list), 40u);
    CHECK_EQ(r.get_tag_data(kTagUser + 7, 5, list), 70u);
    CHECK_EQ(r.get_tag_data(kTagUser + 3, 5, list), 5u);
    CHECK_EQ(r.get_tag_data(kTagUser + 9, 5, list), 5u);
    // The ti_Data of TAG_IGNORE is no tag; a control tag is never found.
    CHECK_EQ(r.get_tag_data(kTagUser + 2, 5, list), 5u);
    CHECK_EQ(r.get_tag_data(TAG_IGNORE, 5, list), 5u);
    CHECK_EQ(r.get_tag_data(kTagUser + 1, 6, 0), 6u);

    CHECK_EQ(r.find_tag_item(kTagUser + 4, list), list + 32);
    CHECK_EQ(r.find_tag_item(kTagUser + 7, list), more);
    CHECK_EQ(r.find_tag_item(kTagUser + 3, list), 0u);
    CHECK_EQ(r.find_tag_item(kTagUser + 1, 0), 0u);
    // A TAG_MORE to a null list ends the walk.
    const u32 cut = r.tags(kData + 0x300, {{kTagUser + 1, 11}, {TAG_MORE, 0}});
    CHECK_EQ(r.get_tag_data(kTagUser + 7, 5, cut), 5u);
    CHECK_EQ(r.get_tag_data(kTagUser + 1, 5, cut), 11u);
    CHECK(g_lines.empty());
}

void next_tag_item_moves_the_walk_on() {
    Rig r;
    const u32 more = r.tags(kData + 0x200, {{kTagUser + 7, 70}, {TAG_DONE, 0}});
    const u32 list = r.tags(kData, {{TAG_IGNORE, 0},
                                    {kTagUser + 1, 10},
                                    {TAG_SKIP, 0},
                                    {TAG_MORE, more}});
    const u32 walk = kData + 0x400;
    r.put32(walk, list);
    r.cpu().a[0] = walk;
    CHECK_EQ(r.result(utility_lvo::NextTagItem), list + 8);
    CHECK_EQ(r.get32(walk), list + 16);
    r.cpu().a[0] = walk;
    CHECK_EQ(r.result(utility_lvo::NextTagItem), more);
    CHECK_EQ(r.get32(walk), more + 8);
    r.cpu().a[0] = walk;
    CHECK_EQ(r.result(utility_lvo::NextTagItem), 0u);
    CHECK_EQ(r.get32(walk), 0u);
    r.cpu().a[0] = walk;
    CHECK_EQ(r.result(utility_lvo::NextTagItem), 0u);
}

void tag_walks_are_bounded() {
    Rig r;
    // A TAG_MORE that points back at itself.
    const u32 loop = r.tags(kData, {{TAG_MORE, kData}});
    CHECK_EQ(r.get_tag_data(kTagUser + 1, 5, loop), 5u);
    CHECK(logged("utility: tag list at $9000 has no end"));

    // The whole walk of one search shares one budget: a loop of control
    // tags around a tag that does not match ends after kMaxTagItems items,
    // not after kMaxTagItems for every item found.
    constexpr u32 kLong = 0x500000;  // above exec's memory
    constexpr u32 kIgnores = 1000;
    u32 at = kLong;
    for (u32 i = 0; i < kIgnores; ++i, at += ti_SIZEOF) {
        r.put32(at + ti_Tag, TAG_IGNORE);
    }
    r.put32(at + ti_Tag, kTagUser + 1);
    r.put32(at + ti_SIZEOF + ti_Tag, TAG_MORE);
    r.put32(at + ti_SIZEOF + ti_Data, kLong);
    g_lines.clear();
    CHECK_EQ(r.find_tag_item(kTagUser + 5, kLong), 0u);
    CHECK(logged_containing("has no end"));
    // A tag past kMaxTagItems items in all is not found, though no part of
    // the list between two items is that long.
    const u32 half = Utility::kMaxTagItems / 2 + 1;
    at = kLong;
    for (u32 part = 0; part < 2; ++part) {
        for (u32 i = 0; i < half; ++i, at += ti_SIZEOF) {
            r.put32(at + ti_Tag, TAG_IGNORE);
        }
        r.put32(at + ti_Tag, kTagUser + 1 + part);
        r.put32(at + ti_Data, 7);
        at += ti_SIZEOF;
    }
    r.put32(at + ti_Tag, TAG_DONE);
    g_lines.clear();
    CHECK_EQ(r.get_tag_data(kTagUser + 1, 5, kLong), 7u);
    CHECK(g_lines.empty());
    CHECK_EQ(r.get_tag_data(kTagUser + 2, 5, kLong), 5u);
    CHECK(logged_containing("has no end"));

    // A list that leaves guest memory ends the run.
    CHECK_EQ(r.find_tag_item(kTagUser + 1, kGuestBytes - 4), 0u);
    r.cpu().d[0] = kTagUser + 1;
    r.cpu().a[0] = kGuestBytes;
    CHECK_EQ(r.call(utility_lvo::FindTagItem), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);
    r.cpu().d[0] = kTagUser + 1;
    r.cpu().a[0] = kData + 1;
    CHECK_EQ(r.call(utility_lvo::GetTagData), ExitReason::AddressError);
}

void dates() {
    Rig r;
    CHECK_EQ(r.date2amiga(1978, 1, 1, 0, 0, 0), 0u);
    CHECK_EQ(r.date2amiga(1978, 1, 1, 23, 59, 59), 86399u);
    CHECK_EQ(r.date2amiga(1979, 1, 1, 0, 0, 0), 365u * 86400);
    CHECK_EQ(r.date2amiga(2000, 1, 1, 0, 0, 0), 694224000u);
    CHECK_EQ(r.date2amiga(2000, 3, 1, 0, 0, 0), 694224000u + 60 * 86400);  // a leap year
    CHECK_EQ(r.date2amiga(2100, 3, 1, 0, 0, 0) - r.date2amiga(2100, 2, 28, 0, 0, 0),
             86400u);  // and a century that is not one
    CHECK_EQ(r.date2amiga(1977, 12, 31, 0, 0, 0), static_cast<u32>(-86400));

    const struct {
        u32 seconds, y, mo, d, h, mi, s, wday;
    } cases[] = {
        {0, 1978, 1, 1, 0, 0, 0, 0},
        {86399, 1978, 1, 1, 23, 59, 59, 0},
        {86400, 1978, 1, 2, 0, 0, 0, 1},
        {694224000, 2000, 1, 1, 0, 0, 0, 6},
        {694224000 + 59 * 86400 + 45296, 2000, 2, 29, 12, 34, 56, 2},
        {0xFFFFFFFF, 2114, 2, 7, 6, 28, 15, 3},
    };
    for (const auto& c : cases) {
        for (u32 i = 0; i < CD_SIZE; ++i) {
            r.put8(kData + i, 0xAA);
        }
        r.cpu().d[0] = c.seconds;
        r.cpu().a[0] = kData;
        CHECK_EQ(r.call(utility_lvo::Amiga2Date), ExitReason::Halt);
        CHECK_EQ(r.get16(kData + cd_year), c.y);
        CHECK_EQ(r.get16(kData + cd_month), c.mo);
        CHECK_EQ(r.get16(kData + cd_mday), c.d);
        CHECK_EQ(r.get16(kData + cd_hour), c.h);
        CHECK_EQ(r.get16(kData + cd_min), c.mi);
        CHECK_EQ(r.get16(kData + cd_sec), c.s);
        CHECK_EQ(r.get16(kData + cd_wday), c.wday);
        CHECK_EQ(r.date2amiga(c.y, c.mo, c.d, c.h, c.mi, c.s), c.seconds);
    }
    r.cpu().a[0] = kGuestBytes;
    CHECK_EQ(r.call(utility_lvo::Date2Amiga), ExitReason::BusError);
}

void strings_without_regard_to_case() {
    Rig r;
    CHECK_EQ(r.compare("Hombre", "hOMBRE"), 0u);
    CHECK_EQ(r.compare("", ""), 0u);
    CHECK(static_cast<i64>(static_cast<__INT32_TYPE__>(r.compare("abc", "abd"))) < 0);
    CHECK(static_cast<i64>(static_cast<__INT32_TYPE__>(r.compare("abd", "ABC"))) > 0);
    CHECK(static_cast<i64>(static_cast<__INT32_TYPE__>(r.compare("ab", "abc"))) < 0);
    CHECK(static_cast<i64>(static_cast<__INT32_TYPE__>(r.compare("abc", "ab"))) > 0);
    CHECK_EQ(r.compare("\xE9t\xE9", "\xC9T\xC9"), 0u);
    CHECK(r.compare("\xF7", "\xD7") != 0);  // the division and multiplication signs

    CHECK_EQ(r.compare("abcX", "ABCy", utility_lvo::Strnicmp, 3), 0u);
    CHECK(r.compare("abcX", "ABCy", utility_lvo::Strnicmp, 4) != 0);
    CHECK_EQ(r.compare("abc", "xyz", utility_lvo::Strnicmp, 0), 0u);
    CHECK_EQ(r.compare("same", "SAME", utility_lvo::Strnicmp, 100), 0u);

    const struct {
        u32 in, up, low;
    } chars[] = {
        {'a', 'A', 'a'},   {'Z', 'Z', 'z'},   {'0', '0', '0'},   {0xE9, 0xC9, 0xE9},
        {0xC9, 0xC9, 0xE9}, {0xF7, 0xF7, 0xF7}, {0xD7, 0xD7, 0xD7}, {0xFF, 0xFF, 0xFF},
        {0xDF, 0xDF, 0xDF}, {0x1E0, 0xC0, 0xE0},
    };
    for (const auto& c : chars) {
        r.cpu().d[0] = c.in;
        CHECK_EQ(r.result(utility_lvo::ToUpper), c.up);
        r.cpu().d[0] = c.in;
        CHECK_EQ(r.result(utility_lvo::ToLower), c.low);
    }

    // A string that runs off the end of guest memory ends the run.
    r.put8(kGuestBytes - 1, 'x');
    r.cpu().a[0] = kGuestBytes - 1;
    r.cpu().a[1] = kGuestBytes - 1;
    CHECK_EQ(r.call(utility_lvo::Stricmp), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);
}

}

// The 32-bit math vectors: products in D0 (64-bit ones high in D1, low in
// D0), quotient in D0 and remainder in D1.
void math_vectors() {
    Rig r;
    r.cpu().d[0] = 0xFFFFFFFE;  // -2
    r.cpu().d[1] = 7;
    CHECK_EQ(r.result(utility_lvo::SMult32), 0xFFFFFFF2u);
    r.cpu().d[0] = 0x10000;
    r.cpu().d[1] = 0x10001;
    CHECK_EQ(r.result(utility_lvo::UMult32), 0x10000u);
    r.cpu().d[0] = 0xFFFFFFF9;  // -7
    r.cpu().d[1] = 2;
    CHECK_EQ(r.result(utility_lvo::SDivMod32), 0xFFFFFFFDu);
    CHECK_EQ(r.cpu().d[1], 0xFFFFFFFFu);
    r.cpu().d[0] = 100;
    r.cpu().d[1] = 7;
    CHECK_EQ(r.result(utility_lvo::UDivMod32), 14u);
    CHECK_EQ(r.cpu().d[1], 2u);
    r.cpu().d[0] = 0xFFFFFFFF;
    r.cpu().d[1] = 0xFFFFFFFF;
    CHECK_EQ(r.result(utility_lvo::UMult64), 0x00000001u);
    CHECK_EQ(r.cpu().d[1], 0xFFFFFFFEu);
    r.cpu().d[0] = 0xFFFFFFFF;  // -1
    r.cpu().d[1] = 3;
    CHECK_EQ(r.result(utility_lvo::SMult64), 0xFFFFFFFDu);
    CHECK_EQ(r.cpu().d[1], 0xFFFFFFFFu);
    // Division by zero gives 0 and leaves the dividend as the remainder.
    r.cpu().d[0] = 5;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.result(utility_lvo::UDivMod32), 0u);
    CHECK_EQ(r.cpu().d[1], 5u);
}

// struct Hook
constexpr u32 h_Entry = 8;
constexpr u32 kHook = kData + 0x300;
constexpr u32 kSeen = kData + 0x400;  // where a test hook stores what it got

// Writes the words at addr and returns the address after them.
u32 put_code(Rig& r, u32 addr, std::initializer_list<u32> words) {
    for (u32 w : words) {
        r.put16(addr, w);
        addr += 2;
    }
    return addr;
}

u32 make_hook(Rig& r, u32 at, u32 entry) {
    for (u32 i = 0; i < 20; ++i) {
        r.put8(at + i, 0);
    }
    r.put32(at + h_Entry, entry);
    return at;
}

void call_hook_pkt_runs_the_entry_with_hook_object_and_message() {
    Rig r;
    CHECK_EQ(utility_lvo::CallHookPkt, 102u);
    // The entry stores A0, A2 and A1, spoils D1, D7, A1 and A3 and returns
    // $12345678.
    put_code(r, kCode,
             {0x23C8, kSeen >> 16, kSeen & 0xFFFF,              // move.l a0,kSeen
              0x23CA, kSeen >> 16, (kSeen + 4) & 0xFFFF,        // move.l a2,kSeen+4
              0x23C9, kSeen >> 16, (kSeen + 8) & 0xFFFF,        // move.l a1,kSeen+8
              0x72FF,                                           // moveq #-1,d1
              0x7EFF,                                           // moveq #-1,d7
              0x93C9,                                           // suba.l a1,a1
              0x97CB,                                           // suba.l a3,a3
              0x203C, 0x1234, 0x5678,                           // move.l #$12345678,d0
              0x4E75});                                         // rts
    const u32 hook = make_hook(r, kHook, kCode);
    r.cpu().a[0] = hook;
    r.cpu().a[2] = 0x00AB0000;
    r.cpu().a[1] = 0x00CD0000;
    r.cpu().d[0] = 0x0D0D0D0D;
    r.cpu().d[1] = 0x11111111;
    r.cpu().d[7] = 0x77777777;
    r.cpu().a[3] = 0x33333333;
    CHECK_EQ(r.result(utility_lvo::CallHookPkt), 0x12345678u);
    CHECK_EQ(r.get32(kSeen), hook);
    CHECK_EQ(r.get32(kSeen + 4), 0x00AB0000u);
    CHECK_EQ(r.get32(kSeen + 8), 0x00CD0000u);
    // Only D0 comes back changed.
    CHECK_EQ(r.cpu().d[1], 0x11111111u);
    CHECK_EQ(r.cpu().d[7], 0x77777777u);
    CHECK_EQ(r.cpu().a[0], hook);
    CHECK_EQ(r.cpu().a[1], 0x00CD0000u);
    CHECK_EQ(r.cpu().a[2], 0x00AB0000u);
    CHECK_EQ(r.cpu().a[3], 0x33333333u);
    CHECK_EQ(r.cpu().a[7], kStack);
}

void call_hook_pkt_without_a_hook_or_an_entry_returns_zero() {
    Rig r;
    r.cpu().d[0] = 0x55555555;
    r.cpu().a[0] = 0;
    r.cpu().a[1] = kData;
    r.cpu().a[2] = kData;
    CHECK_EQ(r.result(utility_lvo::CallHookPkt), 0u);

    r.cpu().d[0] = 0x55555555;
    r.cpu().a[0] = make_hook(r, kHook, 0);
    CHECK_EQ(r.result(utility_lvo::CallHookPkt), 0u);
}

void a_hook_may_call_another_hook() {
    Rig r;
    constexpr u32 kInner = kCode + 0x100;
    constexpr u32 kInnerHook = kHook + 0x40;
    // The outer entry calls the inner hook through A6 and adds one to what
    // it returned; the inner one returns 41.
    put_code(r, kCode,
             {0x41F9, kInnerHook >> 16, kInnerHook & 0xFFFF,   // lea kInnerHook,a0
              0x4EAE, 0x10000 - utility_lvo::CallHookPkt,     // jsr CallHookPkt(a6)
              0x5280,                                          // addq.l #1,d0
              0x4E75});                                        // rts
    put_code(r, kInner, {0x7029, 0x4E75});                     // moveq #41,d0 ; rts
    make_hook(r, kInnerHook, kInner);
    r.cpu().a[0] = make_hook(r, kHook, kCode);
    r.cpu().a[1] = 0;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.result(utility_lvo::CallHookPkt), 42u);
    CHECK_EQ(r.cpu().a[7], kStack);
}

void a_hook_that_calls_itself_stops_at_the_nesting_limit() {
    Rig r;
    // The entry calls its own hook again and counts the levels on the way
    // back; the call exec refuses returns 0.
    put_code(r, kCode,
             {0x4EAE, 0x10000 - utility_lvo::CallHookPkt,     // jsr CallHookPkt(a6)
              0x5280,                                          // addq.l #1,d0
              0x4E75});                                        // rts
    r.cpu().a[0] = make_hook(r, kHook, kCode);
    r.cpu().a[1] = 0;
    r.cpu().a[2] = 0;
    const u32 levels = r.result(utility_lvo::CallHookPkt);
    CHECK(levels >= 1);
    CHECK(levels < 64);
    CHECK(logged("exec: calls into 68000 code nested too deep"));
    CHECK_EQ(r.cpu().a[7], kStack);

    // The levels were all given back: the same call nests as deep again.
    g_lines.clear();
    r.cpu().a[0] = kHook;
    CHECK_EQ(r.result(utility_lvo::CallHookPkt), levels);
}

void a_hook_that_faults_ends_the_run_and_frees_its_nesting_level() {
    Rig r;
    put_code(r, kCode, {0x4AFC});  // illegal
    r.cpu().a[0] = make_hook(r, kHook, kCode);
    CHECK_EQ(r.call(utility_lvo::CallHookPkt), ExitReason::Illegal);

    // A hook that returns runs as before afterwards, as deep as before.
    r.cpu() = CpuState{};
    put_code(r, kCode,
             {0x4EAE, 0x10000 - utility_lvo::CallHookPkt, 0x5280, 0x4E75});
    r.cpu().a[0] = kHook;
    const u32 levels = r.result(utility_lvo::CallHookPkt);
    put_code(r, kCode, {0x4AFC});
    r.cpu().a[0] = kHook;
    CHECK_EQ(r.call(utility_lvo::CallHookPkt), ExitReason::Illegal);
    r.cpu() = CpuState{};
    put_code(r, kCode,
             {0x4EAE, 0x10000 - utility_lvo::CallHookPkt, 0x5280, 0x4E75});
    r.cpu().a[0] = kHook;
    CHECK_EQ(r.result(utility_lvo::CallHookPkt), levels);
}

int main() {
    utility_base_is_built();
    unimplemented_vectors_are_logged();
    math_vectors();
    get_tag_data_and_find_tag_item();
    next_tag_item_moves_the_walk_on();
    tag_walks_are_bounded();
    dates();
    strings_without_regard_to_case();
    call_hook_pkt_runs_the_entry_with_hook_object_and_message();
    call_hook_pkt_without_a_hook_or_an_entry_returns_zero();
    a_hook_may_call_another_hook();
    a_hook_that_calls_itself_stops_at_the_nesting_limit();
    a_hook_that_faults_ends_the_run_and_frees_its_nesting_level();
    return check::finish("test_utility");
}
