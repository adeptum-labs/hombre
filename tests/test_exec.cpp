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

#include "libs/exec.hpp"
#include "check.hpp"

#include <cstdio>
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
constexpr u32 kSub = 0x3000;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;  // test structures up to 0xFFFF
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;

// Static storage, as the kernel keeps them.
Jit g_jit;
Exec g_exec;
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
// Jit and exec initialised over them, the log captured.
class Rig {
public:
    Rig() : memory_(kGuestBytes) {
        fd_ = memfd_create("exec", 0);
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
        m.cpu.a[7] = kStack;
    }

    // exec keeps the task, the loader and the idle hook across init: the next
    // rig starts without them.
    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_segment_loader(SegmentLoader{nullptr, nullptr});
        g_exec.set_idle_hook(IdleHook{nullptr, nullptr});
        g_exec.set_reply_handler(0, nullptr, nullptr);
        g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;

    CpuState& cpu() { return m.cpu; }
    u32 base() const { return g_exec.exec_base(); }

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

    u32 code(u32 at, std::initializer_list<u16> words) {
        for (u16 w : words) {
            put16(at, w);
            at += 2;
        }
        return at;
    }

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

    ExitReason go(u32 pc = kCode, u64 max_blocks = 1000000) {
        m.cpu.pc = pc;
        return g_jit.run(max_blocks);
    }

    // JSR -offset(A6) with A6 = lib, from hand-written 68000 code.
    ExitReason call(u32 lib, u32 offset) {
        code(kCall, {0x4EAE, disp(offset), 0x4EF8, 0x0400});  // JSR -offset(A6) ; JMP $400.w
        g_jit.flush();
        m.cpu.a[6] = lib;
        return go(kCall);
    }

    ExitReason exec(u32 offset) { return call(base(), offset); }

    // The free chunks of exec's memory header as (address, bytes).
    std::vector<std::pair<u32, u32>> chunks() const {
        const u32 header = get32(g_exec.exec_base() + MemList + LH_HEAD);
        std::vector<std::pair<u32, u32>> out;
        for (u32 c = get32(header + MH_FIRST); c != 0 && out.size() < 1000; c = get32(c)) {
            out.emplace_back(c, get32(c + MC_BYTES));
        }
        return out;
    }

    // Builds an empty List at the address.
    void new_list(u32 list) {
        put32(list + LH_HEAD, list + LH_TAIL);
        put32(list + LH_TAIL, 0);
        put32(list + LH_TAILPRED, list + LH_HEAD);
        put8(list + LH_TYPE, 0);
    }

    // A Node with the priority and a name stored at name_at.
    void node(u32 at, i64 pri, u32 name_at, const char* name) {
        for (u32 i = 0; i < LN_SIZE; ++i) {
            put8(at + i, 0);
        }
        put8(at + LN_PRI, static_cast<u32>(pri) & 0xFF);
        put32(at + LN_NAME, str(name_at, name));
    }

    // The node addresses from head to tail, checking the back links.
    std::vector<u32> walk(u32 list) const {
        std::vector<u32> out;
        u32 pred = list;
        for (u32 n = get32(list + LH_HEAD); get32(n + LN_SUCC) != 0 && out.size() < 100;
             n = get32(n + LN_SUCC)) {
            CHECK_EQ(get32(n + LN_PRED), pred);
            out.push_back(n);
            pred = n;
        }
        CHECK_EQ(get32(list + LH_TAILPRED), pred);
        return out;
    }

private:
    static constexpr usize kRegion = 1 << 20;
    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

// --- layout -------------------------------------------------------------

void version_is_hombres_own() {
    CHECK_EQ(kHombreLibVersion, 60u);
    CHECK_EQ(kHombreLibRevision, 0u);
}

void layout_matches_the_ndk() {
    CHECK_EQ(exec_lvo::InitStruct, 78u);
    CHECK_EQ(exec_lvo::MakeLibrary, 84u);
    CHECK_EQ(exec_lvo::MakeFunctions, 90u);
    CHECK_EQ(exec_lvo::InitResident, 102u);
    CHECK_EQ(exec_lvo::Disable, 120u);
    CHECK_EQ(exec_lvo::Enable, 126u);
    CHECK_EQ(exec_lvo::Forbid, 132u);
    CHECK_EQ(exec_lvo::Permit, 138u);
    CHECK_EQ(exec_lvo::AllocMem, 198u);
    CHECK_EQ(exec_lvo::FreeMem, 210u);
    CHECK_EQ(exec_lvo::AvailMem, 216u);
    CHECK_EQ(exec_lvo::Insert, 234u);
    CHECK_EQ(exec_lvo::AddHead, 240u);
    CHECK_EQ(exec_lvo::AddTail, 246u);
    CHECK_EQ(exec_lvo::Remove, 252u);
    CHECK_EQ(exec_lvo::RemHead, 258u);
    CHECK_EQ(exec_lvo::RemTail, 264u);
    CHECK_EQ(exec_lvo::Enqueue, 270u);
    CHECK_EQ(exec_lvo::FindName, 276u);
    CHECK_EQ(exec_lvo::FindTask, 294u);
    CHECK_EQ(exec_lvo::SetSignal, 306u);
    CHECK_EQ(exec_lvo::Wait, 318u);
    CHECK_EQ(exec_lvo::Signal, 324u);
    CHECK_EQ(exec_lvo::AllocSignal, 330u);
    CHECK_EQ(exec_lvo::FreeSignal, 336u);
    CHECK_EQ(exec_lvo::AddPort, 354u);
    CHECK_EQ(exec_lvo::RemPort, 360u);
    CHECK_EQ(exec_lvo::PutMsg, 366u);
    CHECK_EQ(exec_lvo::GetMsg, 372u);
    CHECK_EQ(exec_lvo::ReplyMsg, 378u);
    CHECK_EQ(exec_lvo::WaitPort, 384u);
    CHECK_EQ(exec_lvo::FindPort, 390u);
    CHECK_EQ(exec_lvo::AddLibrary, 396u);
    CHECK_EQ(exec_lvo::OldOpenLibrary, 408u);
    CHECK_EQ(exec_lvo::CloseLibrary, 414u);
    CHECK_EQ(exec_lvo::SumLibrary, 426u);
    CHECK_EQ(exec_lvo::RawDoFmt, 522u);
    CHECK_EQ(exec_lvo::OpenLibrary, 552u);
    CHECK_EQ(exec_lvo::CopyMem, 624u);
    CHECK_EQ(exec_lvo::CopyMemQuick, 630u);
    CHECK_EQ(exec_lvo::CacheClearU, 636u);
    CHECK_EQ(exec_lvo::CacheClearE, 642u);
    CHECK_EQ(exec_lvo::CreateMsgPort, 666u);
    CHECK_EQ(exec_lvo::DeleteMsgPort, 672u);
    CHECK_EQ(exec_lvo::AllocVec, 684u);
    CHECK_EQ(exec_lvo::FreeVec, 690u);
    CHECK_EQ(lib_lvo::Open, 6u);
    CHECK_EQ(lib_lvo::Close, 12u);
    CHECK_EQ(lib_lvo::Expunge, 18u);
    CHECK_EQ(lib_lvo::Reserved, 24u);

    // Node and List.
    CHECK_EQ(LN_SUCC, 0u);
    CHECK_EQ(LN_PRED, 4u);
    CHECK_EQ(LN_TYPE, 8u);
    CHECK_EQ(LN_PRI, 9u);
    CHECK_EQ(LN_NAME, 10u);
    CHECK_EQ(LN_SIZE, 14u);
    CHECK_EQ(LH_HEAD, 0u);
    CHECK_EQ(LH_TAIL, 4u);
    CHECK_EQ(LH_TAILPRED, 8u);
    CHECK_EQ(LH_TYPE, 12u);
    CHECK_EQ(LH_SIZE, 14u);

    // Library: a Node, then UBYTE flags, UBYTE pad, four UWORDs, APTR, ULONG, UWORD.
    u32 at = LN_SIZE;
    CHECK_EQ(LIB_FLAGS, at);
    at += 2;
    CHECK_EQ(LIB_NEGSIZE, at);
    CHECK_EQ(LIB_POSSIZE, at + 2);
    CHECK_EQ(LIB_VERSION, at + 4);
    CHECK_EQ(LIB_REVISION, at + 6);
    at += 8;
    CHECK_EQ(LIB_IDSTRING, at);
    CHECK_EQ(LIB_SUM, at + 4);
    CHECK_EQ(LIB_OPENCNT, at + 8);
    CHECK_EQ(LIB_SIZE, at + 10);
    CHECK_EQ(LIB_SIZE, 34u);

    // ExecBase, field by field from the Library node on.
    constexpr u32 kIntVector = 12;
    constexpr u32 kSoftIntList = 16;
    at = LIB_SIZE;
    CHECK_EQ(SoftVer, at);
    at += 2 + 2 + 4 + 5 * 4;  // SoftVer, LowMemChkSum, ChkBase, captures and stack bounds
    CHECK_EQ(MaxLocMem, at);
    at += 5 * 4 + 2 + 16 * kIntVector;  // MaxLocMem..MaxExtMem, ChkSum, IntVects
    CHECK_EQ(ThisTask, at);
    at += 4 + 4 + 4 + 2 + 2 + 2;  // ThisTask, IdleCount, DispCount, Quantum, Elapsed, SysFlags
    CHECK_EQ(IDNestCnt, at);
    CHECK_EQ(TDNestCnt, at + 1);
    at += 2;
    CHECK_EQ(AttnFlags, at);
    at += 2 + 2 + 4 + 4 + 4 + 4 + 4 + 2;  // AttnFlags..TaskTrapAlloc
    CHECK_EQ(MemList, at);
    CHECK_EQ(ResourceList, at + 1 * LH_SIZE);
    CHECK_EQ(DeviceList, at + 2 * LH_SIZE);
    CHECK_EQ(IntrList, at + 3 * LH_SIZE);
    CHECK_EQ(LibList, at + 4 * LH_SIZE);
    CHECK_EQ(PortList, at + 5 * LH_SIZE);
    CHECK_EQ(TaskReady, at + 6 * LH_SIZE);
    CHECK_EQ(TaskWait, at + 7 * LH_SIZE);
    at += 8 * LH_SIZE;
    CHECK_EQ(SoftInts, at);
    at += 5 * kSoftIntList + 4 * 4;  // SoftInts, LastAlert
    CHECK_EQ(VBlankFrequency, at);
    CHECK_EQ(PowerSupplyFrequency, at + 1);
    at += 2;
    CHECK_EQ(SemaphoreList, at);
    at += LH_SIZE + 4 + 4 + 4 + 2 + 4 + 4;  // ..KickCheckSum, ex_Pad0, LaunchPoint, RamLibPrivate
    CHECK_EQ(ex_EClockFrequency, at);
    at += 4 + 4 + 4 + 5 * 4 + 4 + 3 * 4 + 12 + 4;  // ..ex_MemHandler
    CHECK_EQ(SYSBASESIZE, at);
    // The well-known absolute offsets.
    CHECK_EQ(ThisTask, 0x114u);
    CHECK_EQ(AttnFlags, 0x128u);
    CHECK_EQ(LibList, 0x17Au);
    CHECK_EQ(VBlankFrequency, 0x212u);
    CHECK_EQ(ex_EClockFrequency, 0x238u);
    CHECK_EQ(SYSBASESIZE, 0x278u);

    // MemHeader, MemChunk, Resident, Task.
    CHECK_EQ(MH_ATTRIBUTES, LN_SIZE);
    CHECK_EQ(MH_FIRST, 16u);
    CHECK_EQ(MH_LOWER, 20u);
    CHECK_EQ(MH_UPPER, 24u);
    CHECK_EQ(MH_FREE, 28u);
    CHECK_EQ(MH_SIZE, 32u);
    CHECK_EQ(MC_NEXT, 0u);
    CHECK_EQ(MC_BYTES, 4u);
    CHECK_EQ(MC_SIZE, 8u);
    CHECK_EQ(RT_MATCHWORD, 0u);
    CHECK_EQ(RT_MATCHTAG, 2u);
    CHECK_EQ(RT_ENDSKIP, 6u);
    CHECK_EQ(RT_FLAGS, 10u);
    CHECK_EQ(RT_VERSION, 11u);
    CHECK_EQ(RT_TYPE, 12u);
    CHECK_EQ(RT_PRI, 13u);
    CHECK_EQ(RT_NAME, 14u);
    CHECK_EQ(RT_IDSTRING, 18u);
    CHECK_EQ(RT_INIT, 22u);
    CHECK_EQ(RT_SIZE, 26u);
    CHECK_EQ(TC_SIGALLOC, LN_SIZE + 4);
    CHECK_EQ(TC_SIGWAIT, 22u);
    CHECK_EQ(TC_SIGRECVD, 26u);
    CHECK_EQ(TC_SIZE, 92u);
    CHECK_EQ(mp_Flags, 14u);
    CHECK_EQ(mp_SigBit, 15u);
    CHECK_EQ(mp_SigTask, 16u);
    CHECK_EQ(mp_MsgList, 20u);
    CHECK_EQ(MP_SIZE, 34u);
    CHECK_EQ(mn_ReplyPort, 14u);
    CHECK_EQ(mn_Length, 18u);
    CHECK_EQ(MN_SIZE, 20u);
    CHECK_EQ(PA_SIGNAL, 0u);
    CHECK_EQ(PA_SOFTINT, 1u);
    CHECK_EQ(PA_IGNORE, 2u);

    CHECK_EQ(RTC_MATCHWORD, 0x4AFCu);
    CHECK_EQ(RTF_AUTOINIT, 0x80u);
    CHECK_EQ(MEMF_PUBLIC, 1u);
    CHECK_EQ(MEMF_CHIP, 2u);
    CHECK_EQ(MEMF_FAST, 4u);
    CHECK_EQ(MEMF_CLEAR, 0x10000u);
    CHECK_EQ(MEMF_LARGEST, 0x20000u);
    CHECK_EQ(MEMF_TOTAL, 0x80000u);
    CHECK_EQ(NT_TASK, 1u);
    CHECK_EQ(NT_DEVICE, 3u);
    CHECK_EQ(NT_MSGPORT, 4u);
    CHECK_EQ(NT_MESSAGE, 5u);
    CHECK_EQ(NT_FREEMSG, 6u);
    CHECK_EQ(NT_REPLYMSG, 7u);
    CHECK_EQ(NT_RESOURCE, 8u);
    CHECK_EQ(NT_LIBRARY, 9u);
    CHECK_EQ(NT_MEMORY, 10u);
    CHECK_EQ(LIBF_SUMUSED, 4u);
    CHECK_EQ(LIBF_DELEXP, 8u);
}

// --- ExecBase -----------------------------------------------------------

void exec_base_is_built() {
    Rig r;
    const u32 base = r.base();
    CHECK(base >= kFreeStart && base < kFreeEnd);
    CHECK_EQ(base & 3, 0u);
    CHECK_EQ(r.get32(4), base);
    CHECK_EQ(r.get8(base + LN_TYPE), NT_LIBRARY);
    CHECK(r.read_str(r.get32(base + LN_NAME)) == "exec.library");
    CHECK_EQ(r.get16(base + LIB_VERSION), 60u);
    CHECK_EQ(r.get16(base + LIB_REVISION), 0u);
    CHECK_EQ(r.get16(base + SoftVer), 0u);
    CHECK_EQ(r.get16(base + AttnFlags), 0u);
    CHECK_EQ(r.get16(base + LIB_POSSIZE), SYSBASESIZE);
    CHECK_EQ(r.get8(base + IDNestCnt), 0xFFu);
    CHECK_EQ(r.get8(base + TDNestCnt), 0xFFu);
    CHECK_EQ(r.get32(base + ThisTask), 0u);
    CHECK(r.read_str(r.get32(base + LIB_IDSTRING)) == "exec 60.0 (Hombre)\r\n");

    // exec is on its own library list, the memory header on the memory list.
    CHECK(r.walk(base + LibList) == std::vector<u32>{base});
    const std::vector<u32> headers = r.walk(base + MemList);
    CHECK_EQ(headers.size(), 1u);
    const u32 header = headers[0];
    CHECK_EQ(r.get8(header + LN_TYPE), NT_MEMORY);
    CHECK_EQ(r.get32(header + MH_UPPER), kFreeEnd);
    CHECK(r.get32(header + MH_LOWER) >= kFreeStart);
    CHECK(base > r.get32(header + MH_LOWER));
    // The other lists are empty.
    for (u32 list : {ResourceList, DeviceList, IntrList, PortList,
                     TaskReady, TaskWait, SemaphoreList}) {
        CHECK(r.walk(base + list).empty());
    }
    // Every vector of the whole d16(A6) range is a word of a native.
    const u32 neg = r.get16(base + LIB_NEGSIZE);
    CHECK(neg >= 32766);
    for (u32 off = 2; off <= neg; off += 2) {
        const u32 word = r.get16(base - off);
        CHECK(word == 0x4E75 || (word & 0xF000) == 0xA000);
    }
    for (u32 lvo : {lib_lvo::Open, lib_lvo::Close, exec_lvo::AllocMem, exec_lvo::OpenLibrary,
                    exec_lvo::FreeVec}) {
        CHECK_EQ(r.get16(base - lvo) & 0xF000, 0xA000u);
        CHECK_EQ(r.get16(base - lvo + 2), 0x4E75u);
    }
    CHECK(g_lines.empty());
}

void init_rejects_bad_ranges() {
    Rig r;
    CHECK(!g_exec.init(r.m, 0x100, 0x100000));         // over page 0 and the stubs
    CHECK(!g_exec.init(r.m, 0x20000, 0x10000));        // reversed
    CHECK(!g_exec.init(r.m, 0x20000, kGuestBytes + 8));  // past guest memory
    CHECK(!g_exec.init(r.m, 0x20000, 0x21000));        // too small for ExecBase
    Machine bare{};
    CHECK(!g_exec.init(bare, 0x20000, 0x100000));      // no Jit
    CHECK(g_exec.init(r.m, 0x20003, 0x200000));
    CHECK(r.base() > 0x20003);
    CHECK_EQ(r.get32(4), r.base());
}

// --- memory -------------------------------------------------------------

void alloc_is_aligned_and_disjoint() {
    Rig r;
    const u32 a = g_exec.alloc(100, MEMF_ANY);
    const u32 b = g_exec.alloc(100, MEMF_PUBLIC | MEMF_CHIP | MEMF_FAST);
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK_EQ(a & 7, 0u);
    CHECK_EQ(b & 7, 0u);
    CHECK(a + 100 <= b || b + 100 <= a);
    CHECK(a >= kFreeStart && a + 100 <= kFreeEnd);
    CHECK_EQ(g_exec.alloc(0, MEMF_ANY), 0u);
    CHECK_EQ(g_exec.alloc(0xFFFFFFFF, MEMF_ANY), 0u);
    CHECK_EQ(g_exec.alloc(0xFFFFFFF9, MEMF_ANY), 0u);
    CHECK_EQ(g_exec.alloc(kFreeEnd, MEMF_ANY), 0u);  // more than the free space
    const u32 header = r.get32(r.base() + MemList);
    u32 total = 0;
    for (auto [at, bytes] : r.chunks()) {
        total += bytes;
    }
    CHECK_EQ(r.get32(header + MH_FREE), total);
}

void clear_zeroes_a_dirty_block() {
    Rig r;
    const u32 a = g_exec.alloc(64, MEMF_ANY);
    for (u32 i = 0; i < 64; ++i) {
        r.put8(a + i, 0xEE);
    }
    g_exec.free(a, 64);
    const u32 b = g_exec.alloc(64, MEMF_CLEAR);
    CHECK_EQ(b, a);  // first fit gives the same block back
    for (u32 i = 0; i < 64; ++i) {
        CHECK_EQ(r.get8(b + i), 0u);
    }
}

void free_coalesces_and_survives_double_free() {
    Rig r;
    const auto before = r.chunks();
    CHECK_EQ(before.size(), 1u);
    const u32 a = g_exec.alloc(40, MEMF_ANY);
    const u32 b = g_exec.alloc(24, MEMF_ANY);
    const u32 c = g_exec.alloc(1000, MEMF_ANY);
    g_exec.free(b, 24);
    CHECK_EQ(r.chunks().size(), 2u);
    g_exec.free(a, 40);  // merges with b
    CHECK_EQ(r.chunks().size(), 2u);
    CHECK(r.chunks()[0] == std::make_pair(a, 64u));
    CHECK(g_lines.empty());
    g_exec.free(a, 40);  // already free
    CHECK(logged_containing("exec: FreeMem"));
    CHECK_EQ(r.chunks().size(), 2u);
    g_exec.free(b + 8, 8);  // inside a free chunk
    CHECK_EQ(r.chunks().size(), 2u);
    g_exec.free(c, 1000);  // merges both sides
    CHECK(r.chunks() == before);
    g_exec.free(c, 1000);  // a double free once merged
    CHECK(r.chunks() == before);

    // Addresses and sizes exec never handed out.
    g_lines.clear();
    g_exec.free(c + 3, 8);
    g_exec.free(0x100, 8);
    g_exec.free(kFreeEnd - 8, 16);
    g_exec.free(kFreeEnd + 0x1000, 8);
    g_exec.free(c, 0xFFFFFFFF);
    CHECK_EQ(g_lines.size(), 5u);
    CHECK(r.chunks() == before);
    g_exec.free(0, 100);  // null is ignored quietly
    g_exec.free(c, 0);
    CHECK_EQ(g_lines.size(), 5u);
}

void corrupt_free_list_fails_safely() {
    Rig r;
    const u32 a = g_exec.alloc(64, MEMF_ANY);
    g_exec.alloc(64, MEMF_ANY);
    g_exec.free(a, 64);
    // The guest points the first chunk at itself.
    r.put32(a + MC_NEXT, a);
    CHECK_EQ(g_exec.alloc(4096, MEMF_ANY), 0u);
    CHECK(logged_containing("exec: memory list"));
    g_exec.free(a + 128, 8);  // walks the same list
    // Odd and out of range links.
    r.put32(a + MC_NEXT, 0x00FFFFF1);
    CHECK_EQ(g_exec.alloc(4096, MEMF_ANY), 0u);
    r.put32(a + MC_NEXT, 0);
    r.put32(a + MC_BYTES, 0xFFFFFFF8);  // runs past the region
    CHECK_EQ(g_exec.alloc(128, MEMF_ANY), 0u);
    r.put32(a + MC_BYTES, 64);
    CHECK_EQ(g_exec.alloc(64, MEMF_ANY), a);  // repaired
}

void memory_functions_through_vectors() {
    Rig r;
    r.cpu().d[0] = 100;
    r.cpu().d[1] = MEMF_CLEAR;
    CHECK_EQ(r.exec(exec_lvo::AllocMem), ExitReason::Halt);
    const u32 a = r.cpu().d[0];
    CHECK(a != 0);
    CHECK_EQ(a & 7, 0u);
    CHECK_EQ(r.cpu().a[7], kStack);

    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::AvailMem), ExitReason::Halt);
    const u32 avail = r.cpu().d[0];
    r.cpu().d[1] = MEMF_TOTAL;
    CHECK_EQ(r.exec(exec_lvo::AvailMem), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], kFreeEnd - r.get32(r.get32(r.base() + MemList) + MH_LOWER));
    r.cpu().d[1] = MEMF_LARGEST;
    CHECK_EQ(r.exec(exec_lvo::AvailMem), ExitReason::Halt);
    CHECK(r.cpu().d[0] <= avail && r.cpu().d[0] > 0);

    r.cpu().a[1] = a;
    r.cpu().d[0] = 100;
    CHECK_EQ(r.exec(exec_lvo::FreeMem), ExitReason::Halt);
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::AvailMem), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], avail + 104);

    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::AllocMem), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    // AllocVec keeps the size, FreeVec gives it all back.
    r.cpu().d[0] = 30;
    r.cpu().d[1] = MEMF_CLEAR;
    CHECK_EQ(r.exec(exec_lvo::AllocVec), ExitReason::Halt);
    const u32 v = r.cpu().d[0];
    CHECK(v != 0);
    CHECK_EQ(v & 7, 0u);
    CHECK(r.get32(v - 4) >= 30);
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::AvailMem), ExitReason::Halt);
    CHECK(r.cpu().d[0] < avail + 104);
    r.cpu().a[1] = v;
    CHECK_EQ(r.exec(exec_lvo::FreeVec), ExitReason::Halt);
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::AvailMem), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], avail + 104);
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::FreeVec), ExitReason::Halt);  // FreeVec(NULL)
    r.cpu().d[0] = 0xFFFFFFFC;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::AllocVec), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    // FreeVec of a pointer whose hidden size is out of the guest's reach.
    r.cpu().a[1] = 0x00FFFFFE;
    CHECK_EQ(r.exec(exec_lvo::FreeVec), ExitReason::Halt);
    r.cpu().a[1] = 2;
    CHECK_EQ(r.exec(exec_lvo::FreeVec), ExitReason::Halt);
    CHECK(logged_containing("exec: FreeMem"));
}

// --- lists --------------------------------------------------------------

constexpr u32 kList = kData;
constexpr u32 kNodeA = kData + 0x100;
constexpr u32 kNodeB = kData + 0x120;
constexpr u32 kNodeC = kData + 0x140;
constexpr u32 kNodeD = kData + 0x160;
constexpr u32 kNames = kData + 0x400;

void lists_through_vectors() {
    Rig r;
    r.new_list(kList);
    r.node(kNodeA, 0, kNames, "alpha");
    r.node(kNodeB, 5, kNames + 0x20, "beta");
    r.node(kNodeC, -3, kNames + 0x40, "gamma");
    r.node(kNodeD, 5, kNames + 0x60, "delta");

    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNodeA;
    CHECK_EQ(r.exec(exec_lvo::AddHead), ExitReason::Halt);
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNodeB;
    CHECK_EQ(r.exec(exec_lvo::AddHead), ExitReason::Halt);
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNodeC;
    CHECK_EQ(r.exec(exec_lvo::AddTail), ExitReason::Halt);
    CHECK(r.walk(kList) == (std::vector<u32>{kNodeB, kNodeA, kNodeC}));

    r.cpu().a[1] = kNodeA;
    CHECK_EQ(r.exec(exec_lvo::Remove), ExitReason::Halt);
    CHECK(r.walk(kList) == (std::vector<u32>{kNodeB, kNodeC}));

    // Insert after a node, after nothing (the head) and after the tail.
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNodeA;
    r.cpu().a[2] = kNodeB;
    CHECK_EQ(r.exec(exec_lvo::Insert), ExitReason::Halt);
    CHECK(r.walk(kList) == (std::vector<u32>{kNodeB, kNodeA, kNodeC}));

    r.cpu().a[0] = kList;
    CHECK_EQ(r.exec(exec_lvo::RemHead), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], kNodeB);
    r.cpu().a[0] = kList;
    CHECK_EQ(r.exec(exec_lvo::RemTail), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], kNodeC);
    CHECK(r.walk(kList) == (std::vector<u32>{kNodeA}));
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNodeC;
    r.cpu().a[2] = 0;
    CHECK_EQ(r.exec(exec_lvo::Insert), ExitReason::Halt);
    CHECK(r.walk(kList) == (std::vector<u32>{kNodeC, kNodeA}));
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNodeB;
    r.cpu().a[2] = kList + LH_TAIL;
    CHECK_EQ(r.exec(exec_lvo::Insert), ExitReason::Halt);
    CHECK(r.walk(kList) == (std::vector<u32>{kNodeC, kNodeA, kNodeB}));

    // Empty the list; RemHead and RemTail of an empty list give 0.
    for (int i = 0; i < 3; ++i) {
        r.cpu().a[0] = kList;
        CHECK_EQ(r.exec(exec_lvo::RemHead), ExitReason::Halt);
        CHECK(r.cpu().d[0] != 0);
    }
    r.cpu().a[0] = kList;
    CHECK_EQ(r.exec(exec_lvo::RemHead), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    r.cpu().a[0] = kList;
    CHECK_EQ(r.exec(exec_lvo::RemTail), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK(r.walk(kList).empty());

    // Enqueue keeps the priorities falling, equal ones in arrival order.
    for (u32 n : {kNodeA, kNodeB, kNodeC, kNodeD}) {
        r.cpu().a[0] = kList;
        r.cpu().a[1] = n;
        CHECK_EQ(r.exec(exec_lvo::Enqueue), ExitReason::Halt);
    }
    CHECK(r.walk(kList) == (std::vector<u32>{kNodeB, kNodeD, kNodeA, kNodeC}));

    // FindName from the list, then on from a node.
    r.str(kNames + 0x200, "delta");
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNames + 0x200;
    CHECK_EQ(r.exec(exec_lvo::FindName), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], kNodeD);
    r.str(kNames + 0x200, "beta");
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNames + 0x200;
    CHECK_EQ(r.exec(exec_lvo::FindName), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], kNodeB);
    r.cpu().a[0] = kNodeB;  // searching on after beta does not find it again
    r.cpu().a[1] = kNames + 0x200;
    CHECK_EQ(r.exec(exec_lvo::FindName), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    r.str(kNames + 0x200, "Beta");
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNames + 0x200;
    CHECK_EQ(r.exec(exec_lvo::FindName), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);  // case matters
    r.str(kNames + 0x200, "bet");
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNames + 0x200;
    CHECK_EQ(r.exec(exec_lvo::FindName), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK(g_lines.empty());
}

void list_functions_on_bad_pointers() {
    Rig r;
    r.node(kNodeA, 0, kNames, "alpha");
    r.cpu().a[0] = kList + 1;
    r.cpu().a[1] = kNodeA;
    CHECK_EQ(r.exec(exec_lvo::AddHead), ExitReason::AddressError);
    CHECK_EQ(r.cpu().fault_addr, kList + 1);
    CHECK_EQ(r.cpu().trap_number, 3u);

    r.cpu().a[0] = 0xFFFFFFF0;
    r.cpu().a[1] = kNodeA;
    CHECK_EQ(r.exec(exec_lvo::AddTail), ExitReason::BusError);
    CHECK_EQ(r.cpu().trap_number, 2u);

    r.cpu().a[1] = kGuestBytes - 2;
    CHECK_EQ(r.exec(exec_lvo::Remove), ExitReason::BusError);
    r.cpu().a[0] = kGuestBytes;
    CHECK_EQ(r.exec(exec_lvo::RemHead), ExitReason::BusError);

    // A name that runs off the end of guest memory.
    r.new_list(kList);
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNodeA;
    CHECK_EQ(r.exec(exec_lvo::AddTail), ExitReason::Halt);
    for (u32 i = 0; i < 16; ++i) {
        r.put8(kGuestBytes - 16 + i, 'a');
    }
    r.put32(kNodeA + LN_NAME, kGuestBytes - 16);
    r.str(kNames + 0x200, "aaaaaaaaaaaaaaaaaaaa");
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNames + 0x200;
    CHECK_EQ(r.exec(exec_lvo::FindName), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);

    // A list whose node points back at itself ends the walk.
    r.put32(kNodeA + LN_NAME, kNames);
    r.put32(kNodeA + LN_SUCC, kNodeA);
    r.str(kNames + 0x200, "zeta");
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNames + 0x200;
    CHECK_EQ(r.exec(exec_lvo::FindName), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK(logged_containing("exec: list"));
    r.node(kNodeB, 9, kNames + 0x20, "beta");
    r.put8(kNodeA + LN_PRI, 10);
    r.cpu().a[0] = kList;
    r.cpu().a[1] = kNodeB;
    CHECK_EQ(r.exec(exec_lvo::Enqueue), ExitReason::Halt);
    CHECK_EQ(r.get32(kNodeA + LN_SUCC), kNodeA);  // left alone
}

// --- libraries ----------------------------------------------------------

void open_exec_library() {
    Rig r;
    const u32 name = r.str(kNames, "exec.library");
    // MOVEA.L 4.W,A6 ; LEA name,A1 ; MOVEQ #0,D0 ; JSR -552(A6) ; JMP $400.w
    r.code(kCode, {0x2C78, 0x0004, 0x43F9, static_cast<u16>(name >> 16), static_cast<u16>(name),
                   0x7000, 0x4EAE, disp(exec_lvo::OpenLibrary), 0x4EF8, 0x0400});
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], r.base());
    CHECK_EQ(r.cpu().a[1], name);  // the arguments are not clobbered
    CHECK_EQ(r.cpu().a[7], kStack);
    CHECK_EQ(r.get16(r.base() + LIB_OPENCNT), 1u);

    r.cpu().a[1] = name;
    r.cpu().d[0] = 60;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], r.base());
    r.cpu().a[1] = name;
    r.cpu().d[0] = 61;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    r.cpu().a[1] = name;
    r.cpu().d[0] = 0xFFFF003C;  // only the low word is the version
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], r.base());
    r.cpu().a[1] = name;
    CHECK_EQ(r.exec(exec_lvo::OldOpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], r.base());
    CHECK_EQ(r.get16(r.base() + LIB_OPENCNT), 4u);

    r.cpu().a[1] = r.base();
    CHECK_EQ(r.exec(exec_lvo::CloseLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK_EQ(r.get16(r.base() + LIB_OPENCNT), 3u);
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::CloseLibrary), ExitReason::Halt);  // CloseLibrary(NULL)
    CHECK_EQ(r.get16(r.base() + LIB_OPENCNT), 3u);

    // A name out of reach.
    r.cpu().a[1] = kGuestBytes + 100;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::BusError);
}

void unknown_library_without_loader() {
    Rig r;
    r.cpu().a[1] = r.str(kNames, "nothere.library");
    r.cpu().d[0] = 0;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK(g_lines.empty());
}

u32 g_native_d1 = 0;
void native_triple(void* ctx, Machine& m) {
    CHECK(ctx == &g_native_d1);
    g_native_d1 = m.cpu.d[1];
    m.cpu.d[0] = m.cpu.d[1] * 3;
}

void native_library() {
    Rig r;
    const NativeVector fns[] = {{nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr},
                                {nullptr, nullptr}, {native_triple, "Triple"},
                                {nullptr, nullptr}};
    const u32 lib =
        g_exec.make_native_library("test.library", kHombreLibVersion, fns, 6, &g_native_d1);
    CHECK(lib != 0);
    CHECK_EQ(lib & 3, 0u);
    CHECK(r.read_str(r.get32(lib + LN_NAME)) == "test.library");
    CHECK_EQ(r.get16(lib + LIB_VERSION), 60u);
    CHECK_EQ(r.get8(lib + LN_TYPE), NT_LIBRARY);
    CHECK(r.get16(lib + LIB_NEGSIZE) >= 36);
    CHECK_EQ(r.get16(lib - 30) & 0xF000, 0xA000u);
    CHECK_EQ(r.get16(lib - 28), 0x4E75u);

    // Opened by name from 68000 code, then called through its vector.
    const u32 name = r.str(kNames, "test.library");
    // MOVEA.L 4.W,A6 ; LEA name,A1 ; MOVEQ #60,D0 ; JSR -552(A6) ; MOVEA.L D0,A6 ;
    // MOVEQ #14,D1 ; JSR -30(A6) ; JMP $400.w
    r.code(kCode, {0x2C78, 0x0004, 0x43F9, static_cast<u16>(name >> 16), static_cast<u16>(name),
                   0x703C, 0x4EAE, disp(exec_lvo::OpenLibrary), 0x2C40, 0x720E, 0x4EAE,
                   disp(30), 0x4EF8, 0x0400});
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[6], lib);
    CHECK_EQ(g_native_d1, 14u);
    CHECK_EQ(r.cpu().d[0], 42u);
    CHECK_EQ(r.cpu().a[7], kStack);
    CHECK_EQ(r.get16(lib + LIB_OPENCNT), 1u);

    r.cpu().a[1] = name;
    r.cpu().d[0] = kHombreLibVersion + 1;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    r.cpu().a[1] = lib;
    CHECK_EQ(r.exec(exec_lvo::CloseLibrary), ExitReason::Halt);
    CHECK_EQ(r.get16(lib + LIB_OPENCNT), 0u);
    r.cpu().a[1] = lib;
    CHECK_EQ(r.exec(exec_lvo::CloseLibrary), ExitReason::Halt);
    CHECK_EQ(r.get16(lib + LIB_OPENCNT), 0u);  // never below zero

    // The vector without a native.
    CHECK_EQ(r.call(lib, 36), ExitReason::Unimplemented);
    CHECK(logged("test: unimplemented LVO -36"));
    CHECK_EQ(r.cpu().pc, lib - 36);
    // Past the last vector of the table: whatever memory is there.
    CHECK(g_exec.make_native_library(nullptr, 1, fns, 1) == 0);
    CHECK(g_exec.make_native_library("x.library", 1, nullptr, 3) == 0);
}

void native_library_with_fields_past_the_node() {
    Rig r;
    // Dirty the memory the library will get, to see that it is cleared.
    const u32 probe = g_exec.alloc(4096, 0);
    for (u32 i = 0; i < 4096; ++i) {
        r.put8(probe + i, 0xEE);
    }
    g_exec.free(probe, 4096);
    const u32 lib = g_exec.make_native_library("wide.library", 1, nullptr, 0, nullptr, 70);
    CHECK(lib != 0);
    CHECK_EQ(r.get16(lib + LIB_POSSIZE), 70u);
    CHECK_EQ(r.get32(lib + LN_NAME), lib + 70);
    CHECK(r.read_str(lib + 70) == "wide.library");
    for (u32 at = LIB_SIZE; at < 70; ++at) {
        CHECK_EQ(r.get8(lib + at), 0u);
    }
    // Less than a Library node gives a Library node; more than a UWORD fails.
    const u32 small = g_exec.make_native_library("small.library", 1, nullptr, 0, nullptr, 4);
    CHECK_EQ(r.get16(small + LIB_POSSIZE), LIB_SIZE);
    CHECK_EQ(r.get32(small + LN_NAME), small + LIB_SIZE);
    CHECK_EQ(g_exec.make_native_library("huge.library", 1, nullptr, 0, nullptr, 0x10000), 0u);
}

void unknown_exec_vector() {
    Rig r;
    // MOVEA.L 4.W,A6 ; JSR -2000(A6) ; JMP $400.w
    r.code(kCode, {0x2C78, 0x0004, 0x4EAE, disp(2000), 0x4EF8, 0x0400});
    CHECK_EQ(r.go(), ExitReason::Unimplemented);
    CHECK(logged("exec: unimplemented LVO -2000"));
    CHECK_EQ(r.cpu().pc, r.base() - 2000);
    // The far end of the d16(A6) range and a real but unimplemented vector.
    g_lines.clear();
    CHECK_EQ(r.exec(32766), ExitReason::Unimplemented);
    CHECK(logged("exec: unimplemented LVO -32766"));
    CHECK_EQ(r.exec(30), ExitReason::Unimplemented);  // Supervisor
    CHECK(logged("exec: unimplemented LVO -30"));
}

// A hand-assembled 68000 library in one segment.
//
//   +0   Resident        RTF_AUTOINIT, version 2, NT_LIBRARY
//   +28  init table      {40, vectors, init struct, init routine}
//   +44  vectors         word offsets from +44: Open Close Expunge Null Double, -1
//   +58  init struct     INITWORD(lib_Revision, 5), INITBYTE(34, 0x5A), end
//   +70  init routine    MOVEA.L D0,A1 ; MOVE.W #3,20(A1) ; MOVE.L A0,36(A1) ; RTS
//   +84  Open            ADDQ.W #1,32(A6) ; MOVEQ #-1,D2 ; MOVEA.L D2,A2 ; MOVE.L A6,D0 ; RTS
//   +96  Close           SUBQ.W #1,32(A6) ; MOVEQ #0,D0 ; RTS
//   +104 Expunge, Null   MOVEQ #0,D0 ; RTS
//   +108 Double          MOVE.L D1,D0 ; ADD.L D0,D0 ; RTS
//   +116 name, +140 id string
//
// When `self_open` is set the init routine opens the library's own name
// first, which makes OpenLibrary load the file again from inside the init.
std::vector<u8> hand_library(u32 at, const char* name, bool self_open) {
    std::vector<u8> out(160, 0);
    auto w16 = [&](u32 off, u32 v) {
        out[off] = static_cast<u8>(v >> 8);
        out[off + 1] = static_cast<u8>(v);
    };
    auto w32 = [&](u32 off, u32 v) {
        w16(off, v >> 16);
        w16(off + 2, v);
    };
    const u32 name_off = 116;
    const u32 id_off = 140;
    w16(0, RTC_MATCHWORD);
    w32(RT_MATCHTAG, at);
    w32(RT_ENDSKIP, at + 160);
    out[RT_FLAGS] = RTF_AUTOINIT;
    out[RT_VERSION] = 2;
    out[RT_TYPE] = NT_LIBRARY;
    w32(RT_NAME, at + name_off);
    w32(RT_IDSTRING, at + id_off);
    w32(RT_INIT, at + 28);
    w32(28, 40);
    w32(32, at + 44);
    w32(36, at + 58);
    w32(40, self_open ? at + 150 : at + 70);
    const u16 vectors[] = {0xFFFF, 84 - 44, 96 - 44, 104 - 44, 104 - 44, 108 - 44, 0xFFFF};
    for (u32 i = 0; i < 7; ++i) {
        w16(44 + 2 * i, vectors[i]);
    }
    const u16 init_struct[] = {0xD000, 0x0016, 0x0005, 0xA022, 0x5A00, 0x0000};
    for (u32 i = 0; i < 6; ++i) {
        w16(58 + 2 * i, init_struct[i]);
    }
    const u16 code[] = {0x2240, 0x337C, 0x0003, 0x0014, 0x2348, 0x0024, 0x4E75,  // init
                        0x526E, 0x0020, 0x74FF, 0x2442, 0x200E, 0x4E75,          // Open
                        0x536E, 0x0020, 0x7000, 0x4E75,                          // Close
                        0x7000, 0x4E75,                                          // Null
                        0x2001, 0xD080, 0x4E75};                                 // Double
    for (u32 i = 0; i < sizeof code / 2; ++i) {
        w16(70 + 2 * i, code[i]);
    }
    std::strcpy(reinterpret_cast<char*>(&out[name_off]), name);
    std::strcpy(reinterpret_cast<char*>(&out[id_off]), "hand 3.5");
    if (self_open) {
        // +150: MOVE.L D0,-(SP) ; LEA name(PC),A1 ; MOVEQ #0,D0 ; JSR -552(A6) ;
        //       MOVE.L (SP)+,D0 ; BRA init routine
        const u16 disp_name = static_cast<u16>(name_off - (150 + 4));
        const u16 opener[] = {0x2F00, 0x43FA, disp_name, 0x7000, 0x4EAE,
                              disp(exec_lvo::OpenLibrary), 0x201F, 0x6000,
                              static_cast<u16>(70 - (150 + 16))};
        out.resize(170);
        for (u32 i = 0; i < 9; ++i) {
            w16(150 + 2 * i, opener[i]);
        }
    }
    return out;
}

struct FakeLoader {
    Rig* rig;
    u32 calls;
    std::string path;
    bool self_open;
    u32 last_seglist;
};

bool fake_load(void* ctx, const char* path, u32& seglist) {
    FakeLoader& f = *static_cast<FakeLoader*>(ctx);
    ++f.calls;
    f.path = path;
    std::string name = path;
    if (name != "LIBS:hand.library" && name != "LIBS:self.library" &&
        name != "LIBS:empty.library" && name != "LIBS:loop.library") {
        return false;
    }
    const u32 total = 8 + 192;
    const u32 segment = g_exec.alloc(total, MEMF_CLEAR);
    CHECK(segment != 0);
    f.rig->put32(segment, total);
    f.rig->put32(segment + 4, 0);
    const u32 data = segment + 8;
    if (name != "LIBS:empty.library" && name != "LIBS:loop.library") {
        const std::vector<u8> image = hand_library(data + 8, name.c_str() + 5, f.self_open);
        for (u32 i = 0; i < image.size(); ++i) {
            f.rig->put8(data + 8 + i, image[i]);
        }
    }
    seglist = (segment + 4) >> 2;
    if (name == "LIBS:loop.library") {
        f.rig->put32(segment + 4, seglist);  // the segment is its own successor
    }
    f.last_seglist = seglist;
    return true;
}

void library_loaded_through_the_segment_loader() {
    Rig r;
    FakeLoader loader{&r, 0, "", false, 0};
    g_exec.set_segment_loader(SegmentLoader{fake_load, &loader});
    const u32 name = r.str(kNames, "hand.library");
    r.cpu().a[1] = name;
    r.cpu().d[0] = 2;
    r.cpu().d[2] = 0x1234;
    r.cpu().a[2] = 0x5678;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(loader.calls, 1u);
    CHECK(loader.path == "LIBS:hand.library");
    const u32 lib = r.cpu().d[0];
    CHECK(lib != 0);
    // The registers the library's Open clobbered are back.
    CHECK_EQ(r.cpu().d[2], 0x1234u);
    CHECK_EQ(r.cpu().a[2], 0x5678u);
    CHECK_EQ(r.cpu().a[1], name);
    CHECK_EQ(r.cpu().a[6], r.base());
    CHECK_EQ(r.cpu().a[7], kStack);
    // Built by InitResident: the tag's fields, then InitStruct, then the init routine.
    CHECK(r.read_str(r.get32(lib + LN_NAME)) == "hand.library");
    CHECK(r.read_str(r.get32(lib + LIB_IDSTRING)) == "hand 3.5");
    CHECK_EQ(r.get8(lib + LN_TYPE), NT_LIBRARY);
    CHECK_EQ(r.get16(lib + LIB_VERSION), 3u);
    CHECK_EQ(r.get16(lib + LIB_REVISION), 5u);
    CHECK_EQ(r.get8(lib + 34), 0x5Au);
    CHECK_EQ(r.get32(lib + 36), loader.last_seglist);
    CHECK_EQ(r.get16(lib + LIB_NEGSIZE), 32u);
    CHECK_EQ(r.get16(lib + LIB_POSSIZE), 40u);
    CHECK_EQ(r.get16(lib + LIB_OPENCNT), 1u);  // counted by its Open, not twice
    CHECK_EQ(r.get16(lib - 30), 0x4EF9u);
    CHECK_EQ(r.get16(lib - 6), 0x4EF9u);
    CHECK(r.walk(r.base() + LibList).size() == 2);

    // A second open finds it without the loader; a newer version is refused
    // without trying to load it again.
    r.cpu().a[1] = name;
    r.cpu().d[0] = 3;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], lib);
    r.cpu().a[1] = name;
    r.cpu().d[0] = 4;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK_EQ(loader.calls, 1u);
    CHECK_EQ(r.get16(lib + LIB_OPENCNT), 2u);

    // Its function, through its JMP vector.
    r.cpu().d[1] = 21;
    CHECK_EQ(r.call(lib, 30), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 42u);

    r.cpu().a[1] = lib;
    CHECK_EQ(r.exec(exec_lvo::CloseLibrary), ExitReason::Halt);
    CHECK_EQ(r.get16(lib + LIB_OPENCNT), 1u);
    CHECK_EQ(r.cpu().a[6], r.base());

    // A file without a resident, and a file the loader cannot find.
    r.cpu().a[1] = r.str(kNames + 0x40, "empty.library");
    r.cpu().d[0] = 0;
    const u32 free_before = r.get32(r.get32(r.base() + MemList) + MH_FREE);
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK(logged("exec: no resident in LIBS:empty.library"));
    CHECK_EQ(r.get32(r.get32(r.base() + MemList) + MH_FREE), free_before);  // unloaded
    // A seglist that never ends.
    r.cpu().a[1] = r.str(kNames + 0x80, "loop.library");
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK(logged_containing("exec: seglist $"));
    CHECK(logged_containing(" has more than 64 segments"));
    r.cpu().a[1] = r.str(kNames + 0x60, "missing.library");
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK(loader.path == "LIBS:missing.library");
}

void library_that_opens_itself_from_its_init() {
    Rig r;
    FakeLoader loader{&r, 0, "", true, 0};
    g_exec.set_segment_loader(SegmentLoader{fake_load, &loader});
    r.cpu().a[1] = r.str(kNames, "self.library");
    r.cpu().d[0] = 0;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK(r.cpu().d[0] != 0);
    // Three loads deep the inner OpenLibrary gives 0 instead of loading again.
    CHECK_EQ(loader.calls, 3u);
    CHECK(loader.path == "LIBS:self.library");
    CHECK(!logged_containing("cannot load"));
    CHECK(logged("exec: libraries nested too deep to load self.library"));
    CHECK_EQ(r.walk(r.base() + LibList).size(), 4u);
    CHECK_EQ(r.cpu().a[7], kStack);
}

void init_resident_and_make_library_through_vectors() {
    Rig r;
    // A Resident whose match tag does not point at itself.
    const u32 res = kData;
    r.put16(res, RTC_MATCHWORD);
    r.put32(res + RT_MATCHTAG, res + 2);
    r.cpu().a[1] = res;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::InitResident), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    // A plain (not RTF_AUTOINIT) resident: its init is called with D0 = 0,
    // A0 = seglist, A6 = ExecBase and its D0 is the result.
    const u32 init = kSub;
    r.code(init, {0x200E, 0x4E75});  // MOVE.L A6,D0 ; RTS
    r.put32(res + RT_MATCHTAG, res);
    r.put8(res + RT_FLAGS, 0);
    r.put32(res + RT_INIT, init);
    r.cpu().a[1] = res;
    r.cpu().d[1] = 0x1234;
    CHECK_EQ(r.exec(exec_lvo::InitResident), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], r.base());

    // MakeLibrary with an absolute vector table and no init struct, then
    // AddLibrary and SumLibrary.
    const u32 table = kData + 0x100;
    r.put32(table, 0x00123456);
    r.put32(table + 4, 0x00ABCDEF);
    r.put32(table + 8, 0xFFFFFFFF);
    r.cpu().a[0] = table;
    r.cpu().a[1] = 0;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = 50;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::MakeLibrary), ExitReason::Halt);
    const u32 lib = r.cpu().d[0];
    CHECK(lib != 0);
    CHECK_EQ(r.get16(lib + LIB_NEGSIZE), 12u);
    CHECK_EQ(r.get16(lib + LIB_POSSIZE), 50u);
    CHECK_EQ(r.get16(lib - 6), 0x4EF9u);
    CHECK_EQ(r.get32(lib - 4), 0x00123456u);
    CHECK_EQ(r.get16(lib - 12), 0x4EF9u);
    CHECK_EQ(r.get32(lib - 10), 0x00ABCDEFu);
    r.put32(lib + LN_NAME, r.str(kNames, "made.library"));
    r.put8(lib + LIB_FLAGS, LIBF_SUMUSED);
    r.cpu().a[1] = lib;
    CHECK_EQ(r.exec(exec_lvo::AddLibrary), ExitReason::Halt);
    CHECK_EQ(r.get8(lib + LN_TYPE), NT_LIBRARY);
    CHECK_EQ(r.get32(lib + LIB_SUM),
             r.get32(lib - 12) + r.get32(lib - 8) + r.get32(lib - 4));
    CHECK(r.walk(r.base() + LibList).size() == 2);
    r.put32(lib - 4, 0x00222222);
    r.cpu().a[1] = lib;
    CHECK_EQ(r.exec(exec_lvo::SumLibrary), ExitReason::Halt);
    CHECK(logged_containing("exec: checksum"));

    // A data size below the Library structure gets a whole Library, and
    // lib_PosSize says so.
    r.put32(table, 0x00123456);
    r.put32(table + 4, 0xFFFFFFFF);
    r.cpu().a[0] = table;
    r.cpu().a[1] = 0;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = 10;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::MakeLibrary), ExitReason::Halt);
    CHECK(r.cpu().d[0] != 0);
    CHECK_EQ(r.get16(r.cpu().d[0] + LIB_POSSIZE), LIB_SIZE);

    // MakeFunctions with word offsets from a base; returns the table size.
    const u32 target = kData + 0x300;
    r.put16(table, 0x0010);
    r.put16(table + 2, 0x0020);
    r.put16(table + 4, 0xFFFF);
    r.cpu().a[0] = target;
    r.cpu().a[1] = table;
    r.cpu().a[2] = 0x5000;
    CHECK_EQ(r.exec(exec_lvo::MakeFunctions), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 12u);
    CHECK_EQ(r.get32(target - 4), 0x5010u);
    CHECK_EQ(r.get32(target - 10), 0x5020u);

    // InitStruct: clears, then copies, repeats and places bytes, words, longs.
    const u32 mem = kData + 0x400;
    for (u32 i = 0; i < 32; ++i) {
        r.put8(mem + i, 0xEE);
    }
    const u32 it = kData + 0x500;
    r.code(it, {0x1101, 0x2222, 0x3333,          // 2 words to the start
                0x5100, 0x7777,                  // the word 0x7777 twice more
                0x8008, 0x1122, 0x3344,          // a long at offset 8
                0xA00E, 0x5500,                  // the byte 0x55 at offset 14
                0xD000, 0x0010, 0xABCD,          // the word 0xABCD at offset 16
                0x2201, 0x0203, 0x0000});        // bytes 1, 2, 3 after it (offset 18)
    r.cpu().a[1] = it;
    r.cpu().a[2] = mem;
    r.cpu().d[0] = 24;
    CHECK_EQ(r.exec(exec_lvo::InitStruct), ExitReason::Halt);
    CHECK_EQ(r.get16(mem), 0x2222u);
    CHECK_EQ(r.get16(mem + 2), 0x3333u);
    CHECK_EQ(r.get16(mem + 4), 0x7777u);
    CHECK_EQ(r.get16(mem + 6), 0x7777u);
    CHECK_EQ(r.get32(mem + 8), 0x11223344u);
    CHECK_EQ(r.get8(mem + 12), 0u);
    CHECK_EQ(r.get8(mem + 14), 0x55u);
    CHECK_EQ(r.get16(mem + 16), 0xABCDu);
    CHECK_EQ(r.get8(mem + 18), 1u);
    CHECK_EQ(r.get8(mem + 19), 2u);
    CHECK_EQ(r.get8(mem + 20), 3u);
    CHECK_EQ(r.get8(mem + 23), 0u);
    CHECK_EQ(r.get8(mem + 24), 0xEEu);  // past the cleared size
    // The size field 3 has no meaning on the 68000.
    r.code(it, {0x3000, 0x0000});
    r.cpu().a[1] = it;
    r.cpu().a[2] = mem;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.exec(exec_lvo::InitStruct), ExitReason::Unimplemented);
    CHECK(logged_containing("exec: InitStruct"));

    // A vector table that runs off the end of guest memory.
    r.put32(kGuestBytes - 4, 0x1000);
    r.cpu().a[0] = kGuestBytes - 4;
    r.cpu().a[1] = 0;
    r.cpu().a[2] = 0;
    r.cpu().d[0] = 40;
    CHECK_EQ(r.exec(exec_lvo::MakeLibrary), ExitReason::BusError);
}

char g_library_names[40][16];

void native_library_table_fills() {
    Rig r;
    const NativeVector fns[] = {{nullptr, nullptr}};
    u32 made = 0;
    for (u32 i = 0; i < 40; ++i) {
        std::snprintf(g_library_names[i], sizeof g_library_names[i], "n%u.library", i);
        if (g_exec.make_native_library(g_library_names[i], kHombreLibVersion, fns, 1) != 0) {
            ++made;
        }
    }
    CHECK_EQ(made, 31u);  // exec takes the first of the 32 entries
    CHECK(logged("exec: native library table full, n31.library not made"));
    CHECK(logged("exec: native library table full, n39.library not made"));
    CHECK_EQ(r.walk(r.base() + LibList).size(), 32u);
}

void native_library_limits_are_logged() {
    // Over-long names and vector tables are refused with a line of their own.
    Rig r;
    const NativeVector fns[] = {{nullptr, nullptr}};
    std::string long_name(300, 'x');
    CHECK_EQ(g_exec.make_native_library(long_name.c_str(), 1, fns, 1), 0u);
    CHECK(logged("exec: library name longer than 255 bytes"));
    CHECK_EQ(g_exec.make_native_library("big.library", 1, fns, 20000), 0u);
    CHECK(logged("exec: big.library has more than 10922 vectors"));
}

void task_set_before_init_is_kept() {
    Rig r;
    const u32 task = kData;
    for (u32 i = 0; i < TC_SIZE; ++i) {
        r.put8(task + i, 0);
    }
    g_exec.set_this_task(task);
    CHECK(g_exec.init(r.m, kFreeStart, kFreeEnd));
    CHECK_EQ(r.get32(r.base() + ThisTask), task);
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::FindTask), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], task);
    r.cpu().d[0] = 0x10;
    r.cpu().d[1] = 0x10;
    CHECK_EQ(r.exec(exec_lvo::SetSignal), ExitReason::Halt);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0x10u);
}

void raw_do_fmt_hex_case_and_null_strings() {
    Rig r;
    const u32 data = kData + 0x200;
    r.put32(data, 0);
    r.put32(data + 4, 0);
    r.put32(data + 8, 0x00ABCDEF);
    r.put16(data + 12, 0xFF);
    r.put32(data + 14, 0xBEEF);
    r.cpu().a[0] = r.str(kData, "[%s][%b][%lx][%x][%lX][%5s]");
    r.cpu().a[1] = data;
    r.cpu().a[2] = 0;
    r.cpu().a[3] = kData + 0x400;
    r.put32(data + 18, 0);
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::Halt);
    CHECK(r.read_str(kData + 0x400) == "[][][ABCDEF][FF][BEEF][     ]");
    CHECK_EQ(r.cpu().d[0], data + 22);
}

// --- tasks and signals --------------------------------------------------

void tasks_and_signals() {
    Rig r;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::FindTask), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    // Without a task the signals are still kept.
    r.cpu().d[0] = 0x100;
    r.cpu().d[1] = 0x100;
    CHECK_EQ(r.exec(exec_lvo::SetSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    r.cpu().d[0] = 0x300;
    CHECK_EQ(r.exec(exec_lvo::Wait), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x100u);

    const u32 task = kData;
    for (u32 i = 0; i < TC_SIZE; ++i) {
        r.put8(task + i, 0);
    }
    r.put8(task + LN_TYPE, NT_TASK);
    r.put32(task + LN_NAME, r.str(kNames, "shell"));
    g_exec.set_this_task(task);
    CHECK_EQ(r.get32(r.base() + ThisTask), task);
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::FindTask), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], task);
    r.cpu().a[1] = r.str(kNames + 0x20, "shell");
    CHECK_EQ(r.exec(exec_lvo::FindTask), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], task);
    r.cpu().a[1] = r.str(kNames + 0x20, "other");
    CHECK_EQ(r.exec(exec_lvo::FindTask), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    // SetSignal sets the masked bits and returns the old ones.
    r.cpu().d[0] = 0xF0F0;
    r.cpu().d[1] = 0x00FF;
    CHECK_EQ(r.exec(exec_lvo::SetSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0x00F0u);
    r.cpu().d[0] = 0;
    r.cpu().d[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::SetSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x00F0u);

    // Wait takes the pending signals of its set and clears them.
    r.cpu().d[0] = 0x0030;
    CHECK_EQ(r.exec(exec_lvo::Wait), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x0030u);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0x00C0u);
    CHECK(g_lines.empty());
    // Nothing else can signal: waiting for anything not pending ends the run.
    r.cpu().d[0] = 0x0001;
    CHECK_EQ(r.exec(exec_lvo::Wait), ExitReason::Unimplemented);
    CHECK(logged("exec: Wait with no signals pending"));

    // A task whose name is out of reach.
    r.put32(task + LN_NAME, kGuestBytes);
    r.cpu().a[1] = r.str(kNames + 0x20, "shell");
    CHECK_EQ(r.exec(exec_lvo::FindTask), ExitReason::BusError);
}

void forbid_and_disable_count() {
    Rig r;
    CHECK_EQ(r.exec(exec_lvo::Forbid), ExitReason::Halt);
    CHECK_EQ(r.exec(exec_lvo::Forbid), ExitReason::Halt);
    CHECK_EQ(r.get8(r.base() + TDNestCnt), 1u);
    CHECK_EQ(r.exec(exec_lvo::Permit), ExitReason::Halt);
    CHECK_EQ(r.get8(r.base() + TDNestCnt), 0u);
    CHECK_EQ(r.exec(exec_lvo::Disable), ExitReason::Halt);
    CHECK_EQ(r.get8(r.base() + IDNestCnt), 0u);
    CHECK_EQ(r.exec(exec_lvo::Enable), ExitReason::Halt);
    CHECK_EQ(r.get8(r.base() + IDNestCnt), 0xFFu);
    CHECK_EQ(r.exec(exec_lvo::Permit), ExitReason::Halt);
    CHECK_EQ(r.get8(r.base() + TDNestCnt), 0xFFu);
}

// --- memory copies and caches -------------------------------------------

void copy_mem() {
    Rig r;
    for (u32 i = 0; i < 16; ++i) {
        r.put8(kData + i, i + 1);
    }
    r.cpu().a[0] = kData;
    r.cpu().a[1] = kData + 0x101;  // odd is fine for CopyMem
    r.cpu().d[0] = 16;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::Halt);
    for (u32 i = 0; i < 16; ++i) {
        CHECK_EQ(r.get8(kData + 0x101 + i), i + 1);
    }
    r.cpu().a[0] = kData;
    r.cpu().a[1] = kData + 0x200;
    r.cpu().d[0] = 16;
    CHECK_EQ(r.exec(exec_lvo::CopyMemQuick), ExitReason::Halt);
    CHECK_EQ(r.get32(kData + 0x20C), 0x0D0E0F10u);
    // Overlapping copies move the bytes as they were.
    r.cpu().a[0] = kData;
    r.cpu().a[1] = kData + 4;
    r.cpu().d[0] = 12;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::Halt);
    CHECK_EQ(r.get32(kData + 4), 0x01020304u);
    CHECK_EQ(r.get32(kData + 12), 0x090A0B0Cu);
    r.cpu().a[0] = kData;
    r.cpu().a[1] = kData + 0x300;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::Halt);

    // Out of reach on either side, and lengths that wrap.
    r.cpu().a[0] = kGuestBytes - 8;
    r.cpu().a[1] = kData;
    r.cpu().d[0] = 16;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);
    r.cpu().a[0] = kData;
    r.cpu().a[1] = 0xFFFFFFF0;
    r.cpu().d[0] = 0x20;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::BusError);
    r.cpu().a[0] = kData;
    r.cpu().a[1] = kData + 0x100;
    r.cpu().d[0] = 0xFFFFFFFF;
    CHECK_EQ(r.exec(exec_lvo::CopyMemQuick), ExitReason::BusError);
}

// --- the address mask ---------------------------------------------------

// With the 68000's 24-bit mask a pointer with flags in its top byte reaches
// what its low 24 bits reach, in exec's natives as in translated code.
void high_byte_pointers_reach_their_24_bit_alias() {
    Rig r;
    g_jit.set_address_mask(0x00FFFFFF);
    const u32 name = r.str(kData, "exec.library");
    r.cpu().a[1] = 0x80000000 | name;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], r.base());

    for (u32 i = 0; i < 16; ++i) {
        r.put8(kData + 0x100 + i, 0xA0 + i);
    }
    r.cpu().a[0] = 0x80000000 | (kData + 0x100);
    r.cpu().a[1] = 0xFF000000 | (kData + 0x200);
    r.cpu().d[0] = 16;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::Halt);
    CHECK_EQ(r.get32(kData + 0x20C), 0xACADAEAFu);
    // An overlapping copy through two aliases picks its direction from
    // the addresses guest memory sees.
    r.cpu().a[0] = 0x01000000 | (kData + 0x200);
    r.cpu().a[1] = kData + 0x204;
    r.cpu().d[0] = 12;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::Halt);
    CHECK_EQ(r.get32(kData + 0x204), 0xA0A1A2A3u);
    CHECK_EQ(r.get32(kData + 0x20C), 0xA8A9AAABu);

    // RawDoFmt: format, data stream, %s argument and buffer all tagged.
    const u32 fmt = r.str(kData + 0x300, "%s=%ld");
    const u32 data = kData + 0x400;
    r.put32(data, 0x40000000 | r.str(kData + 0x500, "n"));
    r.put32(data + 4, 42);
    r.cpu().a[0] = 0xC0000000 | fmt;
    r.cpu().a[1] = 0x80000000 | data;
    r.cpu().a[2] = 0;
    r.cpu().a[3] = 0x7F000000 | (kData + 0x600);
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::Halt);
    CHECK(r.read_str(kData + 0x600) == "n=42");
    CHECK_EQ(r.cpu().d[0], (0x80000000 | data) + 8);

    // FreeMem of a tagged pointer frees the block.
    const auto before = r.chunks();
    r.cpu().d[0] = 64;
    r.cpu().d[1] = MEMF_PUBLIC;
    CHECK_EQ(r.exec(exec_lvo::AllocMem), ExitReason::Halt);
    const u32 block = r.cpu().d[0];
    CHECK(block != 0);
    r.cpu().a[1] = 0x80000000 | block;
    r.cpu().d[0] = 64;
    CHECK_EQ(r.exec(exec_lvo::FreeMem), ExitReason::Halt);
    CHECK(r.chunks() == before);

    // A range whose masked start is in memory but which runs past the end
    // still faults, at the first byte outside, as the pointer was given.
    r.cpu().a[0] = 0x80000000 | (kGuestBytes - 8);
    r.cpu().a[1] = kData;
    r.cpu().d[0] = 16;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, 0x80000000 | kGuestBytes);

    // Under a wider mask the high bits it keeps still fault cleanly.
    g_jit.set_address_mask(0x0FFFFFFF);
    r.cpu().a[1] = 0x81000000 | name;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, 0x81000000 | name);
    r.cpu().a[0] = kData;
    r.cpu().a[1] = 0x02000000;
    r.cpu().d[0] = 4;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, 0x02000000u);

    // Without a mask, the default, a tagged pointer is out of reach.
    g_jit.set_address_mask(0xFFFFFFFF);
    r.cpu().a[1] = 0x80000000 | name;
    CHECK_EQ(r.exec(exec_lvo::OpenLibrary), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, 0x80000000 | name);
    r.cpu().a[0] = 0x80000000 | kData;
    r.cpu().a[1] = kData + 0x100;
    r.cpu().d[0] = 4;
    CHECK_EQ(r.exec(exec_lvo::CopyMem), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, 0x80000000 | kData);
}

// JSR $3000.w ; MOVE.W #$7002,$3000.w ; <two words> ; JSR $3000.w ; JMP $400.w
// with $3000: MOVEQ #1,D0 ; RTS. The middle words either clear the caches
// or do nothing.
u32 self_modifying_run(u16 first, u16 second) {
    Rig r;
    r.code(kSub, {0x7001, 0x4E75});
    r.code(kCode, {0x2C78, 0x0004, 0x4EB8, kSub, 0x31FC, 0x7002, kSub, first, second, 0x4EB8,
                   kSub, 0x4EF8, 0x0400});
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[7], kStack);
    return r.cpu().d[0];
}

void cache_clear_flushes_the_translations() {
    CHECK_EQ(self_modifying_run(0x4E71, 0x4E71), 1u);  // NOPs: the stale block runs
    CHECK_EQ(self_modifying_run(0x4EAE, disp(exec_lvo::CacheClearU)), 2u);
    CHECK_EQ(self_modifying_run(0x4EAE, disp(exec_lvo::CacheClearE)), 2u);
}

// --- RawDoFmt -----------------------------------------------------------

void raw_do_fmt_into_a_buffer() {
    Rig r;
    const u32 fmt = kData;
    const u32 data = kData + 0x200;
    const u32 buf = kData + 0x400;
    const u32 abc = r.str(kData + 0x600, "abc");
    const u32 abcdef = r.str(kData + 0x610, "abcdef");
    const u32 bstr = kData + 0x620;
    r.put8(bstr, 3);
    r.put8(bstr + 1, 'x');
    r.put8(bstr + 2, 'y');
    r.put8(bstr + 3, 'z');
    r.str(fmt, "%s|%ld|%lu|%lx|%lc|%%|%d|%u|%x|%c|%5ld|%-5ld|%05lx|%.3s|%b|%06ld|%s|%lX!");
    u32 at = data;
    auto l = [&](u32 v) { r.put32(at, v); at += 4; };
    auto w = [&](u32 v) { r.put16(at, v); at += 2; };
    l(abc);
    l(static_cast<u32>(-42));
    l(4000000000u);
    l(0xBEEF);
    l('Z');
    w(static_cast<u16>(-7));
    w(65535);
    w(0xAB);
    w('q');
    l(42);
    l(42);
    l(0x1F);
    l(abcdef);
    l(bstr >> 2);
    l(static_cast<u32>(-42));
    l(0);
    l(0xC0DE);
    r.cpu().a[0] = fmt;
    r.cpu().a[1] = data;
    r.cpu().a[2] = 0;
    r.cpu().a[3] = buf;
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::Halt);
    CHECK(r.read_str(buf) ==
          "abc|-42|4000000000|BEEF|Z|%|-7|65535|AB|q|   42|42   |0001F|abc|xyz|-00042||C0DE!");
    CHECK_EQ(r.cpu().d[0], at);
    CHECK_EQ(r.cpu().a[3], buf);  // A3 is the caller's again
    CHECK_EQ(r.cpu().a[0], fmt);
}

void raw_do_fmt_through_a_character_routine() {
    Rig r;
    const u32 fmt = r.str(kData, "n=%ld, s=%s");
    const u32 data = kData + 0x200;
    r.put32(data, 1234);
    r.put32(data + 4, r.str(kData + 0x300, "ok"));
    const u32 buf = kData + 0x400;
    for (u32 i = 0; i < 32; ++i) {
        r.put8(buf + i, 0xEE);
    }
    r.code(kSub, {0x16C0, 0x4E75});  // MOVE.B D0,(A3)+ ; RTS
    r.cpu().a[0] = fmt;
    r.cpu().a[1] = data;
    r.cpu().a[2] = kSub;
    r.cpu().a[3] = buf;
    r.cpu().d[5] = 0x5555;
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::Halt);
    CHECK(r.read_str(buf) == "n=1234, s=ok");
    CHECK_EQ(r.get8(buf + 12), 0u);      // the NUL went through the routine too
    CHECK_EQ(r.get8(buf + 13), 0xEEu);
    CHECK_EQ(r.cpu().d[0], data + 8);
    CHECK_EQ(r.cpu().a[3], buf);
    CHECK_EQ(r.cpu().d[5], 0x5555u);
    CHECK_EQ(r.cpu().a[7], kStack);
}

void raw_do_fmt_on_bad_input() {
    Rig r;
    // A word argument at an odd address.
    r.cpu().a[0] = r.str(kData, "%d");
    r.cpu().a[1] = kData + 0x201;
    r.cpu().a[2] = 0;
    r.cpu().a[3] = kData + 0x400;
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::AddressError);
    CHECK_EQ(r.cpu().fault_addr, kData + 0x201);
    // A format string out of reach.
    r.cpu().a[0] = kGuestBytes;
    r.cpu().a[1] = kData + 0x200;
    r.cpu().a[3] = kData + 0x400;
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::BusError);
    // A buffer that runs off the end of memory.
    r.cpu().a[0] = r.str(kData, "hello");
    r.cpu().a[3] = kGuestBytes - 2;
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);
    // A character routine that faults.
    r.code(kSub, {0x16C0, 0x4E75});  // MOVE.B D0,(A3)+ ; RTS
    r.cpu().a[0] = r.str(kData, "hello");
    r.cpu().a[2] = kSub;
    r.cpu().a[3] = kGuestBytes - 1;
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::BusError);
    // A character routine that calls RawDoFmt again, without end.
    r.code(kSub, {0x2C78, 0x0004, 0x4EAE, disp(exec_lvo::RawDoFmt), 0x4E75});
    g_lines.clear();
    r.cpu().a[0] = r.str(kData, "x");
    r.cpu().a[2] = kSub;
    r.cpu().a[3] = kData + 0x400;
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::Unimplemented);
    CHECK(logged("exec: calls into 68000 code nested too deep"));
}

// RawDoFmt of 50 characters through a routine that runs 100 blocks for
// each, from a run allowed max_blocks.
ExitReason raw_do_fmt_with_a_slow_routine(Rig& r, u64 max_blocks) {
    r.code(kSub, {0x7263, 0x51C9, 0xFFFE, 0x4E75});  // MOVEQ #99,D1 ; DBRA D1,* ; RTS
    r.cpu().a[0] = r.str(kData, std::string(50, 'x').c_str());
    r.cpu().a[1] = kData + 0x200;
    r.cpu().a[2] = kSub;
    r.cpu().a[3] = kData + 0x400;
    r.code(kCall, {0x4EAE, disp(exec_lvo::RawDoFmt), 0x4EF8, 0x0400});
    g_jit.flush();
    r.cpu().a[6] = r.base();
    r.cpu().a[7] = kStack;
    return r.go(kCall, max_blocks);
}

// The blocks a character routine runs count against the budget of the run
// that called RawDoFmt: nested calls cannot multiply it.
void calls_from_natives_share_the_block_budget() {
    Rig r;
    CHECK_EQ(raw_do_fmt_with_a_slow_routine(r, 100000), ExitReason::Halt);
    CHECK(g_lines.empty());
    CHECK_EQ(raw_do_fmt_with_a_slow_routine(r, 2000), ExitReason::Budget);
    CHECK(logged("m68k: block budget used up in a call from a native"));
    // The next run starts with a budget of its own.
    g_lines.clear();
    CHECK_EQ(raw_do_fmt_with_a_slow_routine(r, 100000), ExitReason::Halt);
    CHECK(g_lines.empty());
}

// A format's output is capped: twelve %99999d would give 1199988
// characters, and the buffer gets the first MiB of them and no NUL.
void raw_do_fmt_output_is_capped() {
    Rig r;
    constexpr u32 kBuffer = 0x500000;  // above exec's memory
    constexpr u32 kCap = 1u << 20;
    std::string fmt;
    for (u32 i = 0; i < 12; ++i) {
        fmt += "%99999d";
    }
    const u32 data = kData + 0x200;
    for (u32 i = 0; i < 12; ++i) {
        r.put16(data + 2 * i, i);
    }
    r.put8(kBuffer + kCap, 0xEE);
    r.cpu().a[0] = r.str(kData, fmt.c_str());
    r.cpu().a[1] = data;
    r.cpu().a[2] = 0;
    r.cpu().a[3] = kBuffer;
    r.cpu().d[0] = 0x5A5A5A5A;
    CHECK_EQ(r.exec(exec_lvo::RawDoFmt), ExitReason::Halt);
    CHECK(logged("RawDoFmt: format output limit of 1048576 characters reached"));
    CHECK_EQ(r.get8(kBuffer), static_cast<u32>(' '));
    CHECK_EQ(r.get8(kBuffer + 99998), static_cast<u32>('0'));
    CHECK_EQ(r.get8(kBuffer + kCap - 1), static_cast<u32>(' '));
    CHECK_EQ(r.get8(kBuffer + kCap), 0xEEu);
    CHECK_EQ(r.cpu().d[0], 0x5A5A5A5Au);  // stopped: no result
}

// --- message ports and signals ------------------------------------------

u32 make_test_task(Rig& r, u32 at = kData) {
    for (u32 i = 0; i < TC_SIZE; ++i) {
        r.put8(at + i, 0);
    }
    r.put8(at + LN_TYPE, NT_TASK);
    r.put32(at + TC_SIGALLOC, 0x0000FFFF);
    g_exec.set_this_task(at);
    return at;
}

void create_and_delete_port_balance_memory_and_signals() {
    Rig r;
    const u32 task = make_test_task(r);
    const auto before = r.chunks();
    CHECK_EQ(r.exec(exec_lvo::CreateMsgPort), ExitReason::Halt);
    const u32 port = r.cpu().d[0];
    CHECK(port != 0);
    CHECK_EQ(r.get8(port + LN_TYPE), NT_MSGPORT);
    CHECK_EQ(r.get8(port + mp_Flags), PA_SIGNAL);
    CHECK_EQ(r.get8(port + mp_SigBit), 16u);
    CHECK_EQ(r.get32(port + mp_SigTask), task);
    CHECK_EQ(r.get8(port + mp_MsgList + LH_TYPE), NT_MESSAGE);
    CHECK(r.walk(port + mp_MsgList).empty());
    CHECK_EQ(r.get32(task + TC_SIGALLOC), 0x0001FFFFu);

    r.cpu().a[0] = port;
    CHECK_EQ(r.exec(exec_lvo::DeleteMsgPort), ExitReason::Halt);
    CHECK(r.chunks() == before);
    CHECK_EQ(r.get32(task + TC_SIGALLOC), 0x0000FFFFu);

    // DeleteMsgPort(0) is a no-op.
    r.cpu().a[0] = 0;
    CHECK_EQ(r.exec(exec_lvo::DeleteMsgPort), ExitReason::Halt);
    CHECK(r.chunks() == before);

    // C++ helpers balance memory and signals too.
    const u32 cpp_port = g_exec.create_port();
    CHECK(cpp_port != 0);
    CHECK_EQ(r.get8(cpp_port + mp_SigBit), 16u);
    g_exec.delete_port(cpp_port);
    g_exec.delete_port(0);
    CHECK(r.chunks() == before);
    CHECK_EQ(r.get32(task + TC_SIGALLOC), 0x0000FFFFu);

    // When no signal is free, CreateMsgPort frees the port block and returns 0.
    r.put32(task + TC_SIGALLOC, 0xFFFFFFFF);
    CHECK_EQ(r.exec(exec_lvo::CreateMsgPort), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK(r.chunks() == before);
}

void alloc_signal_picks_bit_16_first_and_free_signal_releases_it() {
    Rig r;
    const u32 task = make_test_task(r);
    r.put32(task + TC_SIGRECVD, 1u << 16);

    r.cpu().d[0] = 0xFFFFFFFF;
    CHECK_EQ(r.exec(exec_lvo::AllocSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 16u);
    CHECK_EQ(r.get32(task + TC_SIGALLOC), 0x0001FFFFu);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0u);  // cleared on allocation

    r.cpu().d[0] = 0xFFFFFFFF;
    CHECK_EQ(r.exec(exec_lvo::AllocSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 17u);

    r.cpu().d[0] = 16;
    CHECK_EQ(r.exec(exec_lvo::FreeSignal), ExitReason::Halt);
    CHECK_EQ(r.get32(task + TC_SIGALLOC), 0x0002FFFFu);

    r.cpu().d[0] = 0xFFFFFFFF;
    CHECK_EQ(r.exec(exec_lvo::AllocSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 16u);
}

void alloc_signal_specific_bit_and_taken_bit_fails() {
    Rig r;
    const u32 task = make_test_task(r);

    r.cpu().d[0] = 20;
    CHECK_EQ(r.exec(exec_lvo::AllocSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 20u);

    // Allocating the same bit again fails with -1.
    r.cpu().d[0] = 20;
    CHECK_EQ(r.exec(exec_lvo::AllocSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0xFFFFFFFFu);

    // Bits 0..15 are reserved in tc_SigAlloc.
    r.cpu().d[0] = 5;
    CHECK_EQ(r.exec(exec_lvo::AllocSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0xFFFFFFFFu);

    // Out-of-range bit numbers fail cleanly.
    r.cpu().d[0] = 32;
    CHECK_EQ(r.exec(exec_lvo::AllocSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0xFFFFFFFFu);

    // Exhausting all 16 user signals leaves none for AllocSignal(-1).
    r.put32(task + TC_SIGALLOC, 0xFFFFFFFF);
    r.cpu().d[0] = 0xFFFFFFFF;
    CHECK_EQ(r.exec(exec_lvo::AllocSignal), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0xFFFFFFFFu);
}

void put_and_get_msg_preserve_order_and_signal_the_task() {
    Rig r;
    const u32 task = make_test_task(r);
    const u32 port = g_exec.create_port();
    CHECK(port != 0);
    const u32 sig_bit = r.get8(port + mp_SigBit);
    const u32 msg1 = kData + 0x200;
    const u32 msg2 = kData + 0x240;
    for (u32 i = 0; i < MN_SIZE; ++i) {
        r.put8(msg1 + i, 0);
        r.put8(msg2 + i, 0);
    }

    r.cpu().a[0] = port;
    r.cpu().a[1] = msg1;
    CHECK_EQ(r.exec(exec_lvo::PutMsg), ExitReason::Halt);
    CHECK_EQ(r.get8(msg1 + LN_TYPE), NT_MESSAGE);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 1u << sig_bit);

    r.put32(task + TC_SIGRECVD, 0);
    r.cpu().a[0] = port;
    r.cpu().a[1] = msg2;
    CHECK_EQ(r.exec(exec_lvo::PutMsg), ExitReason::Halt);
    CHECK_EQ(r.get8(msg2 + LN_TYPE), NT_MESSAGE);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 1u << sig_bit);

    r.cpu().a[0] = port;
    CHECK_EQ(r.exec(exec_lvo::GetMsg), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], msg1);
    r.cpu().a[0] = port;
    CHECK_EQ(r.exec(exec_lvo::GetMsg), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], msg2);
    r.cpu().a[0] = port;
    CHECK_EQ(r.exec(exec_lvo::GetMsg), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    // PA_IGNORE queues the message without signaling the task.
    r.put32(task + TC_SIGRECVD, 0);
    r.put8(port + mp_Flags, PA_IGNORE);
    CHECK(g_exec.post_message(port, msg1));
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0u);
    CHECK_EQ(g_exec.get_message(port), msg1);
    g_exec.delete_port(port);
}

void get_msg_on_empty_port_returns_zero() {
    Rig r;
    make_test_task(r);
    const u32 port = g_exec.create_port();
    CHECK(port != 0);
    r.cpu().a[0] = port;
    CHECK_EQ(r.exec(exec_lvo::GetMsg), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK_EQ(g_exec.get_message(port), 0u);
    g_exec.delete_port(port);
}

void waitport_returns_a_queued_message_without_removing_it() {
    Rig r;
    const u32 task = make_test_task(r);
    const u32 port = g_exec.create_port();
    CHECK(port != 0);
    const u32 sig_bit = r.get8(port + mp_SigBit);
    const u32 msg1 = kData + 0x200;
    const u32 msg2 = kData + 0x240;
    for (u32 i = 0; i < MN_SIZE; ++i) {
        r.put8(msg1 + i, 0);
        r.put8(msg2 + i, 0);
    }
    CHECK(g_exec.post_message(port, msg1));
    CHECK(g_exec.post_message(port, msg2));
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 1u << sig_bit);

    r.cpu().a[0] = port;
    CHECK_EQ(r.exec(exec_lvo::WaitPort), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], msg1);
    // The signal bit is not consumed when a message was already queued.
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 1u << sig_bit);
    // The head message is still in the port.
    CHECK_EQ(g_exec.get_message(port), msg1);
    CHECK_EQ(g_exec.get_message(port), msg2);
    g_exec.delete_port(port);
}

struct HookPostState {
    u32 calls = 0;
    u32 port = 0;
    u32 msg = 0;
};

bool hook_posts_on_third_call(void* ctx) {
    HookPostState& s = *static_cast<HookPostState*>(ctx);
    ++s.calls;
    if (s.calls == 3) {
        CHECK(g_exec.post_message(s.port, s.msg));
    }
    return true;
}

void wait_with_idle_hook_returns_when_the_hook_posts_a_message() {
    Rig r;
    const u32 task = make_test_task(r);
    const u32 port = g_exec.create_port();
    CHECK(port != 0);
    const u32 sig_bit = r.get8(port + mp_SigBit);
    const u32 msg = kData + 0x200;
    for (u32 i = 0; i < MN_SIZE; ++i) {
        r.put8(msg + i, 0);
    }
    HookPostState state{0, port, msg};
    g_exec.set_idle_hook(IdleHook{hook_posts_on_third_call, &state});

    r.cpu().a[0] = port;
    CHECK_EQ(r.exec(exec_lvo::WaitPort), ExitReason::Halt);
    CHECK_EQ(state.calls, 3u);
    CHECK_EQ(r.cpu().d[0], msg);
    CHECK_EQ(r.get32(task + TC_SIGRECVD) & (1u << sig_bit), 0u);
    CHECK_EQ(g_exec.get_message(port), msg);
    g_exec.delete_port(port);
}

bool hook_returns_false(void* ctx) {
    u32& calls = *static_cast<u32*>(ctx);
    ++calls;
    return false;
}

void wait_with_a_hook_that_returns_false_ends_the_run() {
    Rig r;
    make_test_task(r);
    u32 calls = 0;
    g_exec.set_idle_hook(IdleHook{hook_returns_false, &calls});
    r.cpu().d[0] = 1u << 16;
    CHECK_EQ(r.exec(exec_lvo::Wait), ExitReason::Unimplemented);
    CHECK_EQ(calls, 1u);
    CHECK(logged("exec: Wait with no signals pending"));
}

void wait_without_a_hook_is_unchanged() {
    Rig r;
    const u32 task = make_test_task(r);
    r.put32(task + TC_SIGRECVD, 0x0030);
    r.cpu().d[0] = 0x0010;
    CHECK_EQ(r.exec(exec_lvo::Wait), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x0010u);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0x0020u);
    CHECK(g_lines.empty());

    r.cpu().d[0] = 0x0001;
    CHECK_EQ(r.exec(exec_lvo::Wait), ExitReason::Unimplemented);
    CHECK(logged("exec: Wait with no signals pending"));
}

void reply_msg_to_a_reply_port_queues_it_as_replymsg() {
    Rig r;
    const u32 task = make_test_task(r);
    const u32 reply_port = g_exec.create_port();
    CHECK(reply_port != 0);
    const u32 sig_bit = r.get8(reply_port + mp_SigBit);
    const u32 msg = kData + 0x200;
    for (u32 i = 0; i < MN_SIZE; ++i) {
        r.put8(msg + i, 0);
    }
    r.put8(msg + LN_TYPE, NT_MESSAGE);
    r.put32(msg + mn_ReplyPort, reply_port);

    r.cpu().a[1] = msg;
    CHECK_EQ(r.exec(exec_lvo::ReplyMsg), ExitReason::Halt);
    CHECK_EQ(r.get8(msg + LN_TYPE), NT_REPLYMSG);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 1u << sig_bit);
    CHECK_EQ(g_exec.get_message(reply_port), msg);
    g_exec.delete_port(reply_port);
}

void reply_msg_without_a_reply_port_marks_the_message_free() {
    Rig r;
    make_test_task(r);
    const u32 msg = kData + 0x200;
    for (u32 i = 0; i < MN_SIZE; ++i) {
        r.put8(msg + i, 0);
    }
    r.put8(msg + LN_TYPE, NT_MESSAGE);
    r.put32(msg + mn_ReplyPort, 0);

    r.cpu().a[1] = msg;
    CHECK_EQ(r.exec(exec_lvo::ReplyMsg), ExitReason::Halt);
    CHECK_EQ(r.get8(msg + LN_TYPE), NT_FREEMSG);
}

struct ReplyCapture {
    u32 calls = 0;
    u32 last_msg = 0;
};

void capture_reply(void* ctx, u32 message) {
    ReplyCapture& c = *static_cast<ReplyCapture*>(ctx);
    ++c.calls;
    c.last_msg = message;
}

void reply_msg_runs_the_registered_handler_instead_of_queueing() {
    Rig r;
    const u32 task = make_test_task(r);
    const u32 reply_port = g_exec.create_port();
    CHECK(reply_port != 0);
    ReplyCapture cap{};
    g_exec.set_reply_handler(reply_port, capture_reply, &cap);

    const u32 msg = kData + 0x200;
    for (u32 i = 0; i < MN_SIZE; ++i) {
        r.put8(msg + i, 0);
    }
    r.put8(msg + LN_TYPE, NT_MESSAGE);
    r.put32(msg + mn_ReplyPort, reply_port);

    r.cpu().a[1] = msg;
    CHECK_EQ(r.exec(exec_lvo::ReplyMsg), ExitReason::Halt);
    CHECK_EQ(cap.calls, 1u);
    CHECK_EQ(cap.last_msg, msg);
    CHECK_EQ(g_exec.get_message(reply_port), 0u);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0u);
    g_exec.delete_port(reply_port);
}

void add_find_remove_port_by_name() {
    Rig r;
    make_test_task(r);
    const u32 port1 = g_exec.create_port();
    const u32 port2 = g_exec.create_port();
    CHECK(port1 != 0 && port2 != 0);
    r.put32(port1 + LN_NAME, r.str(kNames, "port.one"));
    r.put32(port2 + LN_NAME, r.str(kNames + 0x20, "port.two"));

    r.cpu().a[1] = port1;
    CHECK_EQ(r.exec(exec_lvo::AddPort), ExitReason::Halt);
    r.cpu().a[1] = port2;
    CHECK_EQ(r.exec(exec_lvo::AddPort), ExitReason::Halt);

    r.cpu().a[1] = r.str(kNames + 0x40, "port.one");
    CHECK_EQ(r.exec(exec_lvo::FindPort), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], port1);

    r.cpu().a[1] = r.str(kNames + 0x40, "port.two");
    CHECK_EQ(r.exec(exec_lvo::FindPort), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], port2);

    r.cpu().a[1] = r.str(kNames + 0x40, "port.three");
    CHECK_EQ(r.exec(exec_lvo::FindPort), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    r.cpu().a[1] = port1;
    CHECK_EQ(r.exec(exec_lvo::RemPort), ExitReason::Halt);
    r.cpu().a[1] = r.str(kNames + 0x40, "port.one");
    CHECK_EQ(r.exec(exec_lvo::FindPort), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    r.cpu().a[1] = port2;
    CHECK_EQ(r.exec(exec_lvo::RemPort), ExitReason::Halt);
    g_exec.delete_port(port1);
    g_exec.delete_port(port2);
}

void signal_sets_the_bit_for_the_current_task_only() {
    Rig r;
    const u32 task = make_test_task(r);

    // Null and foreign task pointers are ignored.
    r.cpu().a[1] = 0;
    r.cpu().d[0] = 0x0100;
    CHECK_EQ(r.exec(exec_lvo::Signal), ExitReason::Halt);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0u);

    r.cpu().a[1] = task + 0x100;
    r.cpu().d[0] = 0x0100;
    CHECK_EQ(r.exec(exec_lvo::Signal), ExitReason::Halt);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0u);

    r.cpu().a[1] = task;
    r.cpu().d[0] = 0x0100;
    CHECK_EQ(r.exec(exec_lvo::Signal), ExitReason::Halt);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0x0100u);

    r.cpu().a[1] = task;
    r.cpu().d[0] = 0x0040;
    CHECK_EQ(r.exec(exec_lvo::Signal), ExitReason::Halt);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0x0140u);

    g_exec.signal_current_task(0x0008);
    CHECK_EQ(r.get32(task + TC_SIGRECVD), 0x0148u);
}

void null_and_unreadable_port_pointers_fault_cleanly() {
    Rig r;
    make_test_task(r);
    const u32 port = g_exec.create_port();
    CHECK(port != 0);
    const u32 msg = kData + 0x200;
    for (u32 i = 0; i < MN_SIZE; ++i) {
        r.put8(msg + i, 0);
    }

    CHECK(!g_exec.post_message(0, msg));
    CHECK(!g_exec.post_message(port, 0));
    CHECK_EQ(g_exec.get_message(0), 0u);

    r.cpu().a[0] = 0;
    r.cpu().a[1] = msg;
    CHECK_EQ(r.exec(exec_lvo::PutMsg), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[0] = port;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::PutMsg), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[0] = kGuestBytes;
    r.cpu().a[1] = msg;
    CHECK_EQ(r.exec(exec_lvo::PutMsg), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[0] = 0;
    CHECK_EQ(r.exec(exec_lvo::GetMsg), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[0] = kGuestBytes;
    CHECK_EQ(r.exec(exec_lvo::GetMsg), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[0] = 0;
    CHECK_EQ(r.exec(exec_lvo::WaitPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[0] = kGuestBytes;
    CHECK_EQ(r.exec(exec_lvo::WaitPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::ReplyMsg), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[1] = kGuestBytes;
    CHECK_EQ(r.exec(exec_lvo::ReplyMsg), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::AddPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[1] = kGuestBytes;
    CHECK_EQ(r.exec(exec_lvo::AddPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::RemPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[1] = kGuestBytes;
    CHECK_EQ(r.exec(exec_lvo::RemPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[1] = 0;
    CHECK_EQ(r.exec(exec_lvo::FindPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[1] = kGuestBytes;
    CHECK_EQ(r.exec(exec_lvo::FindPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    r.cpu().a[0] = kGuestBytes;
    CHECK_EQ(r.exec(exec_lvo::DeleteMsgPort), ExitReason::BusError);

    r.cpu().exit = ExitReason::None;
    g_exec.delete_port(port);
}

}

int main() {
    version_is_hombres_own();
    layout_matches_the_ndk();
    exec_base_is_built();
    init_rejects_bad_ranges();
    alloc_is_aligned_and_disjoint();
    clear_zeroes_a_dirty_block();
    free_coalesces_and_survives_double_free();
    corrupt_free_list_fails_safely();
    memory_functions_through_vectors();
    lists_through_vectors();
    list_functions_on_bad_pointers();
    open_exec_library();
    unknown_library_without_loader();
    native_library();
    native_library_with_fields_past_the_node();
    unknown_exec_vector();
    library_loaded_through_the_segment_loader();
    library_that_opens_itself_from_its_init();
    init_resident_and_make_library_through_vectors();
    native_library_table_fills();
    native_library_limits_are_logged();
    task_set_before_init_is_kept();
    raw_do_fmt_hex_case_and_null_strings();
    tasks_and_signals();
    forbid_and_disable_count();
    copy_mem();
    high_byte_pointers_reach_their_24_bit_alias();
    cache_clear_flushes_the_translations();
    raw_do_fmt_into_a_buffer();
    raw_do_fmt_through_a_character_routine();
    raw_do_fmt_on_bad_input();
    calls_from_natives_share_the_block_budget();
    raw_do_fmt_output_is_capped();
    create_and_delete_port_balance_memory_and_signals();
    alloc_signal_picks_bit_16_first_and_free_signal_releases_it();
    alloc_signal_specific_bit_and_taken_bit_fails();
    put_and_get_msg_preserve_order_and_signal_the_task();
    get_msg_on_empty_port_returns_zero();
    waitport_returns_a_queued_message_without_removing_it();
    wait_with_idle_hook_returns_when_the_hook_posts_a_message();
    wait_with_a_hook_that_returns_false_ends_the_run();
    wait_without_a_hook_is_unchanged();
    reply_msg_to_a_reply_port_queues_it_as_replymsg();
    reply_msg_without_a_reply_port_marks_the_message_free();
    reply_msg_runs_the_registered_handler_instead_of_queueing();
    add_find_remove_port_by_name();
    signal_sets_the_bit_for_the_current_task_only();
    null_and_unreadable_port_pointers_fault_cleanly();
    return check::finish("test_exec");
}
