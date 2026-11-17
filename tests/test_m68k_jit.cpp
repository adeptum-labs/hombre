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
#include "m68k_jit.hpp"

#include <initializer_list>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;

namespace {

constexpr u32 kCode = 0x1000;
constexpr u32 kData = 0x4000;
constexpr u32 kStack = 0x8000;
constexpr u16 kX = 0x10;
constexpr u16 kN = 0x08;
constexpr u16 kZ = 0x04;
constexpr u16 kV = 0x02;
constexpr u16 kC = 0x01;

// JMP kHaltStub.w, the end of every test program.
constexpr u16 kJmpHalt[] = {0x4EF8, 0x0400};

// Static storage, as the kernel keeps it.
Jit g_jit;

// One memfd mapped twice, writable and executable, and 16 MiB of guest
// memory, with the JIT initialised over both.
class Rig {
public:
    explicit Rig(usize region_bytes = 1 << 20) : bytes_(region_bytes), memory_(kGuestBytes) {
        fd_ = memfd_create("m68k-jit", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(bytes_)) == 0);
        void* rw = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, bytes_, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        CHECK(rw != rx);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        m.cpu = CpuState{};
        m.mem = GuestMemory{memory_.data(), kGuestBytes};
        m.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, bytes_}, m));
        CHECK(m.jit == &g_jit);
        m.cpu.a[7] = kStack;
    }

    ~Rig() {
        munmap(rw_, bytes_);
        munmap(const_cast<u8*>(rx_), bytes_);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;

    CpuState& cpu() { return m.cpu; }

    void put8(u32 at, u32 v) { memory_[at] = static_cast<u8>(v); }

    void put16(u32 at, u32 v) {
        memory_[at] = static_cast<u8>(v >> 8);
        memory_[at + 1] = static_cast<u8>(v);
    }

    void put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
    }

    u32 get8(u32 at) const { return memory_[at]; }
    u32 get16(u32 at) const { return (get8(at) << 8) | get8(at + 1); }
    u32 get32(u32 at) const { return (get16(at) << 16) | get16(at + 2); }

    // Writes the words at the address and returns the address after them.
    u32 code(u32 at, std::initializer_list<u16> words) {
        for (u16 w : words) {
            put16(at, w);
            at += 2;
        }
        return at;
    }

    // The words, then JMP kHaltStub, at the address.
    u32 program(u32 at, std::initializer_list<u16> words) {
        at = code(at, words);
        put16(at, kJmpHalt[0]);
        put16(at + 2, kJmpHalt[1]);
        return at + 4;
    }

    // Each call gets a program of its own further up, so many small
    // programs share one translation cache without a flush.
    u32 place(std::initializer_list<u16> words) {
        const u32 at = next_;
        next_ = (program(at, words) + 15) & ~15u;
        return at;
    }

    ExitReason go(u32 pc = kCode, u64 max_blocks = 100000) {
        m.cpu.pc = pc;
        m.cpu.exit = ExitReason::None;
        return g_jit.run(max_blocks);
    }

private:
    usize bytes_;
    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
    u32 next_ = 0x20000;
};

u32 mask_of(int bits) {
    return bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
}

u32 msb_of(int bits) {
    return 1u << (bits - 1);
}

u16 nz(u32 r, int bits) {
    r &= mask_of(bits);
    return static_cast<u16>(((r & msb_of(bits)) != 0 ? kN : 0) | (r == 0 ? kZ : 0));
}

struct Result {
    u32 value;
    u16 ccr;
};

// The 68000's ADD, ADDX, SUB, SUBX and CMP, worked out with wide integers.
Result ref_add(u32 d, u32 s, bool extend, u16 ccr, int bits) {
    const u64 m = mask_of(bits);
    const u32 x = extend && (ccr & kX) != 0 ? 1 : 0;
    const u64 wide = (d & m) + (s & m) + static_cast<u64>(x);
    const u32 r = static_cast<u32>(wide & m);
    const bool c = wide > m;
    const bool v = ((s ^ r) & (d ^ r) & msb_of(bits)) != 0;
    u16 out = static_cast<u16>(nz(r, bits) | (v ? kV : 0) | (c ? kC | kX : 0));
    if (extend && r == 0 && (ccr & kZ) == 0) {
        out &= static_cast<u16>(~kZ);
    }
    return {r, out};
}

Result ref_sub(u32 d, u32 s, bool extend, u16 ccr, int bits) {
    const u64 m = mask_of(bits);
    const u32 x = extend && (ccr & kX) != 0 ? 1 : 0;
    const u32 r = static_cast<u32>(((d & m) - (s & m) - x) & m);
    const bool c = (s & m) + static_cast<u64>(x) > (d & m);
    const bool v = ((s ^ d) & (r ^ d) & msb_of(bits)) != 0;
    u16 out = static_cast<u16>(nz(r, bits) | (v ? kV : 0) | (c ? kC | kX : 0));
    if (extend && r == 0 && (ccr & kZ) == 0) {
        out &= static_cast<u16>(~kZ);
    }
    return {r, out};
}

enum class ShiftKind { Asl, Asr, Lsl, Lsr, Rol, Ror };

// One bit at a time, as the manual describes the shifts and rotates.
Result ref_shift(ShiftKind kind, u32 v, u32 count, u16 ccr, int bits) {
    const u32 m = mask_of(bits);
    const u32 msb = msb_of(bits);
    v &= m;
    bool c = false;
    bool overflow = false;
    bool x = (ccr & kX) != 0;
    for (u32 i = 0; i < count; ++i) {
        switch (kind) {
        case ShiftKind::Asl: {
            c = (v & msb) != 0;
            const u32 next = (v << 1) & m;
            overflow = overflow || ((next ^ v) & msb) != 0;
            v = next;
            x = c;
            break;
        }
        case ShiftKind::Asr:
            c = (v & 1) != 0;
            v = (v >> 1) | (v & msb);
            x = c;
            break;
        case ShiftKind::Lsl:
            c = (v & msb) != 0;
            v = (v << 1) & m;
            x = c;
            break;
        case ShiftKind::Lsr:
            c = (v & 1) != 0;
            v >>= 1;
            x = c;
            break;
        case ShiftKind::Rol:
            c = (v & msb) != 0;
            v = ((v << 1) | (c ? 1 : 0)) & m;
            break;
        case ShiftKind::Ror:
            c = (v & 1) != 0;
            v = (v >> 1) | (c ? msb : 0);
            break;
        }
    }
    return {v, static_cast<u16>((x ? kX : 0) | nz(v, bits) | (overflow ? kV : 0) | (c ? kC : 0))};
}

u16 logic_ccr(u32 r, u16 ccr, int bits) {
    return static_cast<u16>((ccr & kX) | nz(r, bits));
}

// Keeps the bits above the operand size from the old register value.
u32 merge(u32 old, u32 value, int bits) {
    const u32 m = mask_of(bits);
    return (old & ~m) | (value & m);
}

u16 size_bits(int bits) {
    return static_cast<u16>(bits == 8 ? 0 : (bits == 16 ? 1 : 2));
}

struct Lcg {
    u64 state = 0x2545F4914F6CDD1Dull;
    u32 next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(state >> 32);
    }
};

// Values that sit on the edges of every operand size.
const u32 kEdges[] = {0,          1,          2,          0x7F,       0x80,       0xFF,
                      0x100,      0x7FFF,     0x8000,     0xFFFF,     0x10000,    0x7FFFFFFF,
                      0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x12345678, 0x87654321, 0x00FF00FF};

// --- running, faults, natives and the cache ---------------------------

void moveq_addq_move_and_halt() {
    Rig r;
    r.program(kCode, {0x7005, 0x5680, 0x2200});
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 8u);
    CHECK_EQ(r.cpu().d[1], 8u);
    CHECK_EQ(r.cpu().exit, ExitReason::Halt);
    CHECK_EQ(r.cpu().pc, kHaltStub);
}

void add_word_sets_zero_carry_and_extend() {
    Rig r;
    r.program(kCode, {0x303C, 0xFFFF, 0xD07C, 0x0001});
    r.cpu().d[0] = 0x12340000;
    r.cpu().sr = 0x2700 | kN | kV;
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x12340000u);
    CHECK_EQ(r.cpu().sr, 0x2700 | kX | kZ | kC);
}

void dbf_loop_counts_down() {
    Rig r;
    // MOVE.W #999,D0 ; loop: ADDQ.L #1,D1 ; DBF D0,loop
    r.program(kCode, {0x303C, 999, 0x5281, 0x51C8, 0xFFFC});
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0xFFFFu);
    CHECK_EQ(r.cpu().d[1], 1000u);
}

void jsr_and_rts() {
    Rig r;
    // JSR $2000.l ; MOVE.L D2,D3
    r.program(kCode, {0x4EB9, 0x0000, 0x2000, 0x2602});
    // MOVEQ #7,D2 ; MOVE.L (A7),D4 ; RTS
    r.code(0x2000, {0x7407, 0x2817, 0x4E75});
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[3], 7u);
    CHECK_EQ(r.cpu().d[4], kCode + 6);
    CHECK_EQ(r.cpu().a[7], kStack);
}

void movem_round_trip() {
    Rig r;
    // MOVEM.L D0-D7/A0-A6,-(SP) ; clear them all ; MOVEM.L (SP)+,D0-D7/A0-A6
    u32 at = r.code(kCode, {0x48E7, 0xFFFE});
    for (u16 n = 0; n < 8; ++n) {
        at = r.code(at, {static_cast<u16>(0x7000 | (n << 9))});        // MOVEQ #0,Dn
        if (n < 7) {
            at = r.code(at, {static_cast<u16>(0x91C8 | (n << 9) | n)});  // SUBA.L An,An
        }
    }
    r.code(at, {0x4CDF, 0x7FFF, kJmpHalt[0], kJmpHalt[1]});
    for (u32 n = 0; n < 8; ++n) {
        r.cpu().d[n] = 0x11111111u * (n + 1);
    }
    for (u32 n = 0; n < 7; ++n) {
        r.cpu().a[n] = 0x01010101u * (n + 0x20);
    }
    CHECK_EQ(r.go(), ExitReason::Halt);
    for (u32 n = 0; n < 8; ++n) {
        CHECK_EQ(r.cpu().d[n], 0x11111111u * (n + 1));
    }
    for (u32 n = 0; n < 7; ++n) {
        CHECK_EQ(r.cpu().a[n], 0x01010101u * (n + 0x20));
    }
    CHECK_EQ(r.cpu().a[7], kStack);
    // D0 went to the lowest address, A6 to the highest.
    CHECK_EQ(r.get32(kStack - 60), 0x11111111u);
    CHECK_EQ(r.get32(kStack - 4), 0x01010101u * 0x26);
}

void bus_and_address_errors() {
    Rig r;
    r.program(kCode, {0x2010});  // MOVE.L (A0),D0
    r.cpu().a[0] = kGuestBytes - 2;
    r.cpu().d[0] = 0x5555;
    CHECK_EQ(r.go(), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes - 2);
    CHECK_EQ(r.cpu().pc, kCode);
    CHECK_EQ(r.cpu().d[0], 0x5555u);

    r.program(0x1100, {0x3010});  // MOVE.W (A0),D0
    r.cpu().a[0] = 0x4001;
    CHECK_EQ(r.go(0x1100), ExitReason::AddressError);
    CHECK_EQ(r.cpu().fault_addr, 0x4001u);
    CHECK_EQ(r.cpu().pc, 0x1100u);

    // No guest address reaches past the end of guest memory.
    const u32 wild[] = {kGuestBytes, kGuestBytes - 1, 0x7FFFFFFE, 0xFFFFFFFC, 0xFFFFFFFE};
    for (u32 a : wild) {
        r.cpu().a[0] = a;
        const ExitReason want = (a & 1) != 0 ? ExitReason::AddressError : ExitReason::BusError;
        CHECK_EQ(r.go(), want);
        CHECK_EQ(r.cpu().fault_addr, a);
    }
    // MOVE.B D0,(A0) and MOVE.L D0,(A0): stores are checked as well.
    const u32 store_b = r.place({0x1080});
    const u32 store_l = r.place({0x2080});
    r.cpu().a[0] = 0xFFFFFFFF;
    CHECK_EQ(r.go(store_b), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, 0xFFFFFFFFu);
    r.cpu().a[0] = kGuestBytes - 1;
    CHECK_EQ(r.go(store_b), ExitReason::Halt);
    CHECK_EQ(r.get8(kGuestBytes - 1), r.cpu().d[0] & 0xFF);
    r.cpu().a[0] = 0x4003;
    CHECK_EQ(r.go(store_l), ExitReason::AddressError);
    CHECK_EQ(r.cpu().fault_addr, 0x4003u);

    // A fetch from an odd address or past the end of memory.
    CHECK_EQ(r.go(0x1001), ExitReason::AddressError);
    CHECK_EQ(r.cpu().fault_addr, 0x1001u);
    CHECK_EQ(r.go(kGuestBytes), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);
    // MOVE.L #imm,D0 whose immediate runs off the end.
    r.put16(kGuestBytes - 4, 0x203C);
    r.put16(kGuestBytes - 2, 0x1234);
    CHECK_EQ(r.go(kGuestBytes - 4), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);
}

void endless_loop_runs_out_of_budget() {
    Rig r;
    r.code(kCode, {0x60FE});  // BRA.S *
    CHECK_EQ(r.go(kCode, 100), ExitReason::Budget);
    CHECK_EQ(r.cpu().pc, kCode);
    CHECK_EQ(r.go(kCode, 0), ExitReason::Budget);
}

int g_native_calls = 0;

void native_seven(void* ctx, Machine& m) {
    CHECK(ctx == &g_native_calls);
    ++g_native_calls;
    m.cpu.d[0] = 7;
}

void native_nested(void* ctx, Machine& m) {
    const u32 entry = *static_cast<u32*>(ctx);
    const u32 sp = m.cpu.a[7];
    CHECK_EQ(m.jit->call_guest(entry, 1000), ExitReason::Returned);
    CHECK_EQ(m.cpu.exit, ExitReason::None);
    CHECK_EQ(m.cpu.a[7], sp);
    m.cpu.d[3] += 1;
}

void natives_are_called_by_their_aline_opcode() {
    Rig r;
    g_native_calls = 0;
    const u16 id = g_jit.add_native(native_seven, &g_native_calls, "seven");
    CHECK_EQ(id, 2u);
    // The library vector: the A-line opcode and RTS.
    r.code(0x3000, {static_cast<u16>(0xA000 | id), 0x4E75});
    // JSR $3000.w ; MOVE.L D0,D1
    r.program(kCode, {0x4EB8, 0x3000, 0x2200});
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(g_native_calls, 1);
    CHECK_EQ(r.cpu().d[1], 7u);
    CHECK_EQ(r.cpu().a[7], kStack);

    // An id nobody registered stops the run at its opcode.
    r.program(0x1100, {0x7001, 0xA064});
    CHECK_EQ(r.go(0x1100), ExitReason::Unimplemented);
    CHECK_EQ(r.cpu().pc, 0x1102u);
    CHECK_EQ(r.cpu().d[0], 1u);
    // The reserved ids.
    r.code(0x1200, {0xA001});
    CHECK_EQ(r.go(0x1200), ExitReason::Returned);
    r.code(0x1300, {0xA000});
    CHECK_EQ(r.go(0x1300), ExitReason::Halt);

    // Ids run out at 0xFFF.
    for (u32 n = 3; n <= 0xFFF; ++n) {
        CHECK_EQ(g_jit.add_native(native_seven, &g_native_calls, "more"), n);
    }
    CHECK_EQ(g_jit.add_native(native_seven, &g_native_calls, "full"), 0u);
    CHECK_EQ(g_jit.add_native(nullptr, nullptr, "none"), 0u);
}

void flush_drops_translations() {
    Rig r;
    r.program(kCode, {0x7001});  // MOVEQ #1,D0
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 1u);
    r.code(kCode, {0x7002});     // MOVEQ #2,D0
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 1u);  // still the cached block
    g_jit.flush();
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 2u);
}

void small_region_flushes_and_completes() {
    // The program below translates to far more than 4 KiB, so the region
    // fills and is flushed several times on the way.
    Rig r(4096);
    u32 at = kCode;
    for (u32 n = 0; n < 400; ++n) {
        at = r.code(at, {0x5280, 0x20C0});  // ADDQ.L #1,D0 ; MOVE.L D0,(A0)+
    }
    r.program(at, {});
    r.cpu().a[0] = 0x40000;
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 400u);
    CHECK_EQ(r.cpu().a[0], 0x40000u + 1600);
    for (u32 n = 0; n < 400; ++n) {
        CHECK_EQ(r.get32(0x40000 + 4 * n), n + 1);
    }
    // And again, now from a region that holds the end of the program.
    r.cpu().d[0] = 0;
    r.cpu().a[0] = 0x40000;
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 400u);
}

void call_guest_returns_through_the_stub() {
    Rig r;
    r.code(0x2000, {0x7609, 0x4E75});  // MOVEQ #9,D3 ; RTS
    r.cpu().pc = 0x1234;
    CHECK_EQ(g_jit.call_guest(0x2000, 1000), ExitReason::Returned);
    CHECK_EQ(r.cpu().d[3], 9u);
    CHECK_EQ(r.cpu().a[7], kStack);
    CHECK_EQ(r.cpu().pc, 0x1234u);
    CHECK_EQ(r.cpu().exit, ExitReason::None);
    CHECK_EQ(r.get16(kReturnStub), 0xA001u);
    CHECK_EQ(r.get16(kReturnStub + 2), 0x4E75u);
    CHECK_EQ(r.get16(kHaltStub), 0xA000u);
    CHECK_EQ(r.get16(kHaltStub + 2), 0x4E75u);

    // From inside a native, during a run.
    static u32 entry = 0x2000;
    const u16 id = g_jit.add_native(native_nested, &entry, "nested");
    r.code(0x3000, {static_cast<u16>(0xA000 | id), 0x4E75});
    r.program(kCode, {0x4EB8, 0x3000, 0x2803});  // JSR $3000.w ; MOVE.L D3,D4
    r.cpu().d[3] = 0;
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[4], 10u);
    CHECK_EQ(r.cpu().a[7], kStack);

    // A fault inside the called code ends the outer run too.
    r.code(0x2000, {0x2010, 0x4E75});  // MOVE.L (A0),D0 ; RTS
    g_jit.flush();
    r.cpu().a[0] = 1;
    CHECK_EQ(g_jit.call_guest(0x2000, 1000), ExitReason::AddressError);

    // A stack the return address cannot be pushed on.
    r.cpu().a[7] = 3;
    CHECK_EQ(g_jit.call_guest(0x2000, 1000), ExitReason::AddressError);
    CHECK_EQ(r.cpu().a[7], 3u);
}

// --- the instruction families ------------------------------------------

void addressing_modes_read() {
    Rig r;
    r.put32(0x4000, 0x11111111);
    r.put32(0x4004, 0x22222222);
    r.put32(0x4010, 0x33333333);
    r.put32(0x4020, 0x44444444);
    r.put32(0x4030, 0x55555555);
    r.put32(0x4040, 0x66666666);
    r.put32(0x7FF0, 0x77777777);
    // MOVE.L (A0),D0 ; MOVE.L (A0)+,D1 ; MOVE.L -(A1),D2 ; MOVE.L 8(A2),D3
    // MOVE.L -4(A2,D4.W),D5 ; MOVE.L 2(A2,A3.L),D6 ; MOVE.L $4010.W,D7
    r.program(kCode, {0x2010, 0x2218, 0x2421, 0x262A, 0x0008, 0x2A32, 0x40FC, 0x2C32, 0xB802,
                      0x2E38, 0x4010});
    r.cpu().a[0] = 0x4000;
    r.cpu().a[1] = 0x4008;
    r.cpu().a[2] = 0x4008;
    r.cpu().d[4] = 0xABCD001C;  // the word index is 0x001C
    r.cpu().a[3] = 0x36;        // a long index
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x11111111u);
    CHECK_EQ(r.cpu().d[1], 0x11111111u);
    CHECK_EQ(r.cpu().a[0], 0x4004u);
    CHECK_EQ(r.cpu().d[2], 0x22222222u);
    CHECK_EQ(r.cpu().a[1], 0x4004u);
    CHECK_EQ(r.cpu().d[3], 0x33333333u);
    CHECK_EQ(r.cpu().d[5], 0x44444444u);
    CHECK_EQ(r.cpu().d[6], 0x66666666u);
    CHECK_EQ(r.cpu().d[7], 0x33333333u);

    // A negative word index: D4.W = -8.
    const u32 neg = r.place({0x2A32, 0x40FC});
    r.cpu().d[4] = 0x0000FFF8;
    r.cpu().a[2] = 0x4010;
    CHECK_EQ(r.go(neg), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[5], 0x22222222u);

    // MOVE.L $00004020.L,D0 ; MOVE.L (16,PC),D1 ; MOVE.L (12,PC,D4.W),D2 ;
    // MOVE.L #$12345678,D3 ; JMP halt ; then two longs of data.
    const u32 at = 0x1200;
    r.code(at, {0x2039, 0x0000, 0x4020, 0x223A, 0x0010, 0x243B, 0x400C, 0x263C, 0x1234, 0x5678,
                kJmpHalt[0], kJmpHalt[1], 0xCAFE, 0xF00D, 0xBEEF, 0x0001});
    // (16,PC) at 0x1208 reads 0x1218; (12,PC,D4.W) at 0x120C with D4 = 4
    // reads 0x121C.
    r.cpu().d[4] = 4;
    CHECK_EQ(r.go(at), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x44444444u);
    CHECK_EQ(r.cpu().d[1], 0xCAFEF00Du);
    CHECK_EQ(r.cpu().d[2], 0xBEEF0001u);
    CHECK_EQ(r.cpu().d[3], 0x12345678u);

    // Byte (A7)+ and -(A7) move the stack pointer by two.
    const u32 pop = r.place({0x101F});
    const u32 bytes = r.place({0x1227, 0x1418});
    r.put8(kStack, 0xAB);
    r.cpu().a[0] = 0x4000;
    CHECK_EQ(r.go(pop), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[7], kStack + 2);
    CHECK_EQ(r.go(bytes), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[7], kStack);
    CHECK_EQ(r.cpu().d[0] & 0xFF, 0xABu);
    CHECK_EQ(r.cpu().d[1] & 0xFF, 0xABu);
    CHECK_EQ(r.cpu().a[0], 0x4001u);
    CHECK_EQ(r.cpu().d[2] & 0xFF, 0x11u);

    // A word read of an address register.
    const u32 an = r.place({0x3608});  // MOVE.W A0,D3
    r.cpu().a[0] = 0x12348765;
    r.cpu().d[3] = 0xFFFFFFFF;
    r.cpu().sr = 0;
    CHECK_EQ(r.go(an), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[3], 0xFFFF8765u);
    CHECK_EQ(r.cpu().sr & 0x1F, kN);
}

void addressing_modes_write() {
    Rig r;
    // MOVE.L D0,(A0) ; MOVE.W D1,(A1)+ ; MOVE.B D2,-(A2) ; MOVE.L D3,16(A3)
    // MOVE.L D4,-2(A3,D5.L) ; MOVE.L D5,$5000.W ; MOVE.L D6,$5010.L
    // MOVE.L (A0)+,(A1)+
    r.program(kCode, {0x2080, 0x32C1, 0x1502, 0x2743, 0x0010, 0x2784, 0x58FE, 0x21C5, 0x5000,
                      0x23C6, 0x0000, 0x5010, 0x22D8});
    CpuState& c = r.cpu();
    c.d[0] = 0x01020304;
    c.d[1] = 0xAAAA0506;
    c.d[2] = 0xBBBBBB07;
    c.d[3] = 0x08090A0B;
    c.d[4] = 0x0C0D0E0F;
    c.d[5] = 0x00000022;
    c.d[6] = 0x10111213;
    c.a[0] = 0x4000;
    c.a[1] = 0x4100;
    c.a[2] = 0x4201;
    c.a[3] = 0x4300;
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.get32(0x4000), 0x01020304u);
    CHECK_EQ(r.get16(0x4100), 0x0506u);
    CHECK_EQ(c.a[2], 0x4200u);
    CHECK_EQ(r.get8(0x4200), 0x07u);
    CHECK_EQ(r.get32(0x4310), 0x08090A0Bu);
    CHECK_EQ(r.get32(0x4320), 0x0C0D0E0Fu);
    CHECK_EQ(r.get32(0x5000), 0x00000022u);
    CHECK_EQ(r.get32(0x5010), 0x10111213u);
    CHECK_EQ(r.get32(0x4102), 0x01020304u);
    CHECK_EQ(c.a[0], 0x4004u);
    CHECK_EQ(c.a[1], 0x4106u);
}

void moves_set_flags_and_keep_upper_bits() {
    Rig r;
    // MOVE.B #$80,D0 ; MOVE.W #0,D1
    r.program(kCode, {0x103C, 0x0080, 0x323C, 0x0000});
    r.cpu().d[0] = 0x12345678;
    r.cpu().d[1] = 0x9ABCDEF0;
    r.cpu().sr = kX | kV | kC;
    const u32 one = r.place({0x103C, 0x0080});
    CHECK_EQ(r.go(one), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x12345680u);
    CHECK_EQ(r.cpu().sr, kX | kN);
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[1], 0x9ABC0000u);
    CHECK_EQ(r.cpu().sr, kX | kZ);

    // MOVEA.W #$8000,A0 sign-extends and leaves the flags alone;
    // MOVEA.L D0,A1 ; MOVE.L A7,D0.
    const u32 movea = r.place({0x307C, 0x8000, 0x2240, 0x200F});
    r.cpu().sr = kC;
    CHECK_EQ(r.go(movea), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[0], 0xFFFF8000u);
    CHECK_EQ(r.cpu().a[1], 0x12345680u);
    CHECK_EQ(r.cpu().d[0], kStack);
    CHECK_EQ(r.cpu().sr, 0u);

    // MOVEQ #-1,D2 sign-extends to the whole register.
    const u32 moveq = r.place({0x74FF});
    CHECK_EQ(r.go(moveq), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[2], 0xFFFFFFFFu);
    CHECK_EQ(r.cpu().sr, kN);
}

void lea_pea_and_friends() {
    Rig r;
    // LEA 8(A0),A1 ; LEA (4,PC),A2 ; LEA 3(A0,D0.W),A3 ; PEA $1234.W ; PEA (A0)
    r.program(kCode, {0x43E8, 0x0008, 0x45FA, 0x0004, 0x47F0, 0x0003, 0x4878, 0x1234, 0x4850});
    r.cpu().a[0] = 0x4000;
    r.cpu().d[0] = 0x10;
    r.cpu().sr = kZ;
    CHECK_EQ(r.go(), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[1], 0x4008u);
    CHECK_EQ(r.cpu().a[2], kCode + 4 + 2 + 4);
    CHECK_EQ(r.cpu().a[3], 0x4013u);
    CHECK_EQ(r.cpu().a[7], kStack - 8);
    CHECK_EQ(r.get32(kStack - 4), 0x1234u);
    CHECK_EQ(r.get32(kStack - 8), 0x4000u);
    CHECK_EQ(r.cpu().sr, kZ);

    // EXT.W ; EXT.L ; SWAP ; EXG D0,D1 ; EXG A0,A1 ; EXG D0,A0 ; NOP
    const u32 ext = r.place({0x4880});
    r.cpu().d[0] = 0x12345680;
    CHECK_EQ(r.go(ext), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x1234FF80u);
    CHECK_EQ(r.cpu().sr, kN);
    const u32 extl = r.place({0x48C0});
    r.cpu().d[0] = 0xFFFF7FFF;
    CHECK_EQ(r.go(extl), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x00007FFFu);
    CHECK_EQ(r.cpu().sr, 0u);
    const u32 swap = r.place({0x4840});
    r.cpu().d[0] = 0x80001234;
    CHECK_EQ(r.go(swap), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x12348000u);
    CHECK_EQ(r.cpu().sr, 0u);
    const u32 exg = r.place({0xC141, 0xC149, 0xC188, 0x4E71});
    r.cpu().d[0] = 1;
    r.cpu().d[1] = 2;
    r.cpu().a[0] = 3;
    r.cpu().a[1] = 4;
    r.cpu().sr = kN | kC;
    CHECK_EQ(r.go(exg), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 4u);
    CHECK_EQ(r.cpu().d[1], 1u);
    CHECK_EQ(r.cpu().a[0], 2u);
    CHECK_EQ(r.cpu().a[1], 3u);
    CHECK_EQ(r.cpu().sr, kN | kC);

    // CLR.B D0 ; CLR.W (A0) ; CLR.L D1
    const u32 clr = r.place({0x4200, 0x4250, 0x4281});
    r.cpu().d[0] = 0x123456FF;
    r.cpu().d[1] = 0xFFFFFFFF;
    r.cpu().a[0] = 0x4000;
    r.put32(0x4000, 0xFFFFFFFF);
    r.cpu().sr = kX | kN | kV | kC;
    CHECK_EQ(r.go(clr), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x12345600u);
    CHECK_EQ(r.cpu().d[1], 0u);
    CHECK_EQ(r.get32(0x4000), 0x0000FFFFu);
    CHECK_EQ(r.cpu().sr, kX | kZ);
}

// One opcode on D0 and D1 against its reference over many operand pairs.
template <typename Ref>
void check_binary(Rig& r, u16 opcode, int bits, Ref ref, bool exhaustive) {
    const u32 at = r.place({opcode});
    Lcg lcg;
    auto one = [&](u32 d, u32 s, u16 ccr) {
        r.cpu().d[0] = d;
        r.cpu().d[1] = s;
        r.cpu().sr = static_cast<u16>(0x2700 | ccr);
        CHECK_EQ(r.go(at), ExitReason::Halt);
        const Result want = ref(d, s, ccr, bits);
        if (r.cpu().d[0] != merge(d, want.value, bits) ||
            r.cpu().sr != (0x2700 | want.ccr)) {
            std::fprintf(stderr, "opcode %04x d=%08x s=%08x ccr=%02x: got %08x/%02x, want %08x/%02x\n",
                         opcode, d, s, ccr, r.cpu().d[0], r.cpu().sr & 0xFF,
                         merge(d, want.value, bits), want.ccr);
        }
        CHECK_EQ(r.cpu().d[0], merge(d, want.value, bits));
        CHECK_EQ(r.cpu().sr, 0x2700 | want.ccr);
    };
    if (exhaustive) {
        for (u32 d = 0; d < 256; ++d) {
            for (u32 s = 0; s < 256; ++s) {
                const u16 ccr = static_cast<u16>(lcg.next() & 0x1F);
                one(0xA5A5A500 | d, 0x5A5A5A00 | s, ccr);
            }
        }
    }
    for (u32 d : kEdges) {
        for (u32 s : kEdges) {
            one(d, s, static_cast<u16>(lcg.next() & 0x1F));
            one(d, s, kX | kZ);
            one(d, s, 0);
        }
    }
    for (int n = 0; n < 2000; ++n) {
        one(lcg.next(), lcg.next(), static_cast<u16>(lcg.next() & 0x1F));
    }
}

void arithmetic_flags_match_the_reference() {
    Rig r;
    for (int bits : {8, 16, 32}) {
        const u16 sz = static_cast<u16>(size_bits(bits) << 6);
        const bool all = bits == 8;
        check_binary(r, 0xD001 | sz, bits,
                     [](u32 d, u32 s, u16 ccr, int b) { return ref_add(d, s, false, ccr, b); }, all);
        check_binary(r, 0x9001 | sz, bits,
                     [](u32 d, u32 s, u16 ccr, int b) { return ref_sub(d, s, false, ccr, b); }, all);
        check_binary(r, 0xD101 | sz, bits,
                     [](u32 d, u32 s, u16 ccr, int b) { return ref_add(d, s, true, ccr, b); }, all);
        check_binary(r, 0x9101 | sz, bits,
                     [](u32 d, u32 s, u16 ccr, int b) { return ref_sub(d, s, true, ccr, b); }, all);
        check_binary(r, 0xB001 | sz, bits,
                     [](u32 d, u32 s, u16 ccr, int b) {
                         Result x = ref_sub(d, s, false, ccr, b);
                         return Result{d, static_cast<u16>((x.ccr & 0x0F) | (ccr & kX))};
                     },
                     all);
        // NEG and NEGX take only D0; D1 plays no part.
        check_binary(r, 0x4400 | sz, bits,
                     [](u32 d, u32, u16 ccr, int b) { return ref_sub(0, d, false, ccr, b); }, all);
        check_binary(r, 0x4000 | sz, bits,
                     [](u32 d, u32, u16 ccr, int b) { return ref_sub(0, d, true, ccr, b); }, all);
        // AND, OR, EOR D1,D0 and NOT, TST.
        check_binary(r, 0xC001 | sz, bits,
                     [](u32 d, u32 s, u16 ccr, int b) {
                         return Result{d & s, logic_ccr(d & s, ccr, b)};
                     },
                     false);
        check_binary(r, 0x8001 | sz, bits,
                     [](u32 d, u32 s, u16 ccr, int b) {
                         return Result{d | s, logic_ccr(d | s, ccr, b)};
                     },
                     false);
        check_binary(r, 0xB300 | sz, bits,  // EOR D1,D0
                     [](u32 d, u32 s, u16 ccr, int b) {
                         return Result{d ^ s, logic_ccr(d ^ s, ccr, b)};
                     },
                     false);
        check_binary(r, 0x4600 | sz, bits,
                     [](u32 d, u32, u16 ccr, int b) { return Result{~d, logic_ccr(~d, ccr, b)}; },
                     false);
        check_binary(r, 0x4A00 | sz, bits,
                     [](u32 d, u32, u16 ccr, int b) { return Result{d, logic_ccr(d, ccr, b)}; },
                     false);
        // MOVE D1,D0.
        const u16 move = static_cast<u16>(bits == 8 ? 0x1001 : (bits == 16 ? 0x3001 : 0x2001));
        check_binary(r, move, bits,
                     [](u32, u32 s, u16 ccr, int b) { return Result{s, logic_ccr(s, ccr, b)}; },
                     false);
    }
    // MULU.W D1,D0 and MULS.W D1,D0.
    check_binary(r, 0xC0C1, 32,
                 [](u32 d, u32 s, u16 ccr, int) {
                     const u32 p = (d & 0xFFFF) * (s & 0xFFFF);
                     return Result{p, logic_ccr(p, ccr, 32)};
                 },
                 false);
    check_binary(r, 0xC1C1, 32,
                 [](u32 d, u32 s, u16 ccr, int) {
                     const u32 p = static_cast<u32>(static_cast<__INT32_TYPE__>(
                         static_cast<__INT16_TYPE__>(d) * static_cast<__INT16_TYPE__>(s)));
                     return Result{p, logic_ccr(p, ccr, 32)};
                 },
                 false);
}

void arithmetic_with_other_operands() {
    Rig r;
    // ADDA.W D1,A0 ; ADDA.L D1,A1 ; SUBA.W D1,A2 ; ADDQ.W #1,A3 ; SUBQ.L #8,A4
    const u32 adda = r.place({0xD0C1, 0xD3C1, 0x94C1, 0x524B, 0x518C});
    r.cpu().d[1] = 0x0001FFFF;
    r.cpu().a[0] = 0x10;
    r.cpu().a[1] = 0x10;
    r.cpu().a[2] = 0x10;
    r.cpu().a[3] = 0xFFFF;
    r.cpu().a[4] = 4;
    r.cpu().sr = kZ;
    CHECK_EQ(r.go(adda), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[0], 0x0Fu);
    CHECK_EQ(r.cpu().a[1], 0x2000Fu);
    CHECK_EQ(r.cpu().a[2], 0x11u);
    CHECK_EQ(r.cpu().a[3], 0x10000u);
    CHECK_EQ(r.cpu().a[4], 0xFFFFFFFCu);
    CHECK_EQ(r.cpu().sr, kZ);

    // CMPA.W D1,A0 compares the sign-extended word with the whole of A0.
    const u32 cmpa = r.place({0xB0C1});
    r.cpu().d[1] = 0xFFFF;
    r.cpu().a[0] = 0xFFFFFFFF;
    CHECK_EQ(r.go(cmpa), ExitReason::Halt);
    CHECK_EQ(r.cpu().sr, kZ);
    r.cpu().a[0] = 0x0000FFFF;
    CHECK_EQ(r.go(cmpa), ExitReason::Halt);
    CHECK_EQ(r.cpu().sr, kC);
    const u32 cmpal = r.place({0xB1C1});  // CMPA.L D1,A0
    r.cpu().d[1] = 1;
    r.cpu().a[0] = 0;
    CHECK_EQ(r.go(cmpal), ExitReason::Halt);
    CHECK_EQ(r.cpu().sr, kN | kC);

    // ADDI.L #$10000,(A0) ; SUBI.B #1,D0 ; CMPI.W #5,D1
    const u32 imm = r.place({0x0690, 0x0001, 0x0000, 0x0400, 0x0001, 0x0C41, 0x0005});
    r.cpu().a[0] = 0x4000;
    r.put32(0x4000, 0xFFFF0000);
    r.cpu().d[0] = 0x100;
    r.cpu().d[1] = 5;
    CHECK_EQ(r.go(imm), ExitReason::Halt);
    CHECK_EQ(r.get32(0x4000), 0u);
    CHECK_EQ(r.cpu().d[0], 0x1FFu);
    CHECK_EQ(r.cpu().sr, kX | kZ);  // X and C from SUBI, then Z from CMPI

    // CMPM.B (A0)+,(A1)+
    const u32 cmpm = r.place({0xB308});
    r.cpu().a[0] = 0x4000;
    r.cpu().a[1] = 0x4100;
    r.put8(0x4000, 0x01);
    r.put8(0x4100, 0x80);
    r.cpu().sr = 0;
    CHECK_EQ(r.go(cmpm), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[0], 0x4001u);
    CHECK_EQ(r.cpu().a[1], 0x4101u);
    CHECK_EQ(r.cpu().sr, kV);

    // ADDX.B -(A0),-(A1) ; SUBX.L -(A0),-(A1)
    const u32 addx = r.place({0xD308});
    r.cpu().a[0] = 0x4001;
    r.cpu().a[1] = 0x4101;
    r.put8(0x4000, 0xFF);
    r.put8(0x4100, 0x00);
    r.cpu().sr = kX | kZ;
    CHECK_EQ(r.go(addx), ExitReason::Halt);
    CHECK_EQ(r.get8(0x4100), 0u);
    CHECK_EQ(r.cpu().sr, kX | kZ | kC);
    CHECK_EQ(r.cpu().a[0], 0x4000u);
    CHECK_EQ(r.cpu().a[1], 0x4100u);
    const u32 subx = r.place({0x9388});
    r.cpu().a[0] = 0x4008;
    r.cpu().a[1] = 0x4108;
    r.put32(0x4004, 1);
    r.put32(0x4104, 3);
    r.cpu().sr = kX | kZ;
    CHECK_EQ(r.go(subx), ExitReason::Halt);
    CHECK_EQ(r.get32(0x4104), 1u);
    CHECK_EQ(r.cpu().sr, 0u);
    CHECK_EQ(r.cpu().a[1], 0x4104u);

    // NEG.W (A0) ; TST.L -(A0)
    const u32 neg = r.place({0x4450, 0x4AA0});
    r.cpu().a[0] = 0x4004;
    r.put32(0x4000, 0x80000000);
    r.put16(0x4004, 0x8000);
    CHECK_EQ(r.go(neg), ExitReason::Halt);
    CHECK_EQ(r.get16(0x4004), 0x8000u);
    CHECK_EQ(r.cpu().a[0], 0x4000u);
    CHECK_EQ(r.cpu().sr, kX | kN);  // NEG left X; TST sets N, clears V and C
}

void logic_with_other_operands() {
    Rig r;
    // ANDI.B #$0F,D0 ; ORI.W #$8000,(A0) ; EORI.L #-1,D1 ; AND.L D2,(A0)
    const u32 at = r.place({0x0200, 0x000F, 0x0050, 0x8000, 0x0A81, 0xFFFF, 0xFFFF, 0xC590});
    r.cpu().d[0] = 0x123456F3;
    r.cpu().d[1] = 0x0F0F0F0F;
    r.cpu().d[2] = 0xFFFF0000;
    r.cpu().a[0] = 0x4000;
    r.put32(0x4000, 0x00010001);
    r.cpu().sr = kX | kC | kV;
    CHECK_EQ(r.go(at), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x12345603u);
    CHECK_EQ(r.cpu().d[1], 0xF0F0F0F0u);
    CHECK_EQ(r.get32(0x4000), 0x80010000u);
    CHECK_EQ(r.cpu().sr, kX | kN);
}

void shifts_match_the_reference() {
    Rig r;
    const ShiftKind kinds[] = {ShiftKind::Asl, ShiftKind::Asr, ShiftKind::Lsl,
                               ShiftKind::Lsr, ShiftKind::Rol, ShiftKind::Ror};
    // Type in bits 4-3 (register form) and the direction bit 8.
    const u16 type[] = {0x0100, 0x0000, 0x0108, 0x0008, 0x0118, 0x0018};
    Lcg lcg;
    for (int k = 0; k < 6; ++k) {
        for (int bits : {8, 16, 32}) {
            const u16 sz = static_cast<u16>(size_bits(bits) << 6);
            // xxx.s D1,D0
            const u32 reg = r.place({static_cast<u16>(0xE220 | type[k] | sz)});
            auto check_one = [&](u32 at, u32 v, u32 count_reg, u32 count, u16 ccr) {
                r.cpu().d[0] = v;
                r.cpu().d[1] = count_reg;
                r.cpu().sr = static_cast<u16>(0x2700 | ccr);
                CHECK_EQ(r.go(at), ExitReason::Halt);
                const Result want = ref_shift(kinds[k], v, count, ccr, bits);
                if (r.cpu().d[0] != merge(v, want.value, bits) ||
                    r.cpu().sr != (0x2700 | want.ccr)) {
                    std::fprintf(stderr, "shift %d.%d v=%08x n=%u ccr=%02x: got %08x/%02x, want %08x/%02x\n",
                                 k, bits, v, count, ccr, r.cpu().d[0], r.cpu().sr & 0xFF,
                                 merge(v, want.value, bits), want.ccr);
                }
                CHECK_EQ(r.cpu().d[0], merge(v, want.value, bits));
                CHECK_EQ(r.cpu().sr, 0x2700 | want.ccr);
            };
            for (u32 count = 0; count < 70; ++count) {
                for (u32 v : kEdges) {
                    // Only the low six bits of the count register count.
                    const u32 count_reg = (lcg.next() & ~63u) | (count & 63);
                    check_one(reg, v, count_reg, count & 63, kX | kV | kC);
                    check_one(reg, v, count_reg, count & 63, kZ | kN);
                }
                check_one(reg, lcg.next(), count, count & 63, static_cast<u16>(lcg.next() & 0x1F));
            }
            // xxx.s #n,D0 for n = 1..8
            for (u32 n = 1; n <= 8; ++n) {
                const u32 imm = r.place({static_cast<u16>(0xE000 | ((n & 7) << 9) | type[k] | sz)});
                for (u32 v : kEdges) {
                    check_one(imm, v, 0, n, static_cast<u16>(lcg.next() & 0x1F));
                }
            }
        }
        // xxx.W (A0)
        const u16 mem_type[] = {0xE1D0, 0xE0D0, 0xE3D0, 0xE2D0, 0xE7D0, 0xE6D0};
        const u32 mem = r.place({mem_type[k]});
        for (u32 v : kEdges) {
            const u16 ccr = static_cast<u16>(lcg.next() & 0x1F);
            r.cpu().a[0] = 0x4000;
            r.put16(0x4000, v);
            r.cpu().sr = ccr;
            CHECK_EQ(r.go(mem), ExitReason::Halt);
            const Result want = ref_shift(kinds[k], v, 1, ccr, 16);
            CHECK_EQ(r.get16(0x4000), want.value);
            CHECK_EQ(r.cpu().sr, want.ccr);
        }
    }
    // LSR.W (A0)+ moves A0 on by one word.
    const u32 post = r.place({0xE2D8});
    r.cpu().a[0] = 0x4000;
    CHECK_EQ(r.go(post), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[0], 0x4002u);
}

void bit_operations() {
    Rig r;
    // BTST #3,D0 ; BCHG D1,D0
    const u32 reg = r.place({0x0800, 0x0003, 0x0340});
    r.cpu().d[0] = 0x00000008;
    r.cpu().d[1] = 35;  // bit 3 again: modulo 32 on a register
    r.cpu().sr = kX | kN | kV | kC;
    CHECK_EQ(r.go(reg), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);
    CHECK_EQ(r.cpu().sr, kX | kN | kV | kC);  // bit was set, so Z clear both times
    r.cpu().d[1] = 31;
    const u32 bchg = r.place({0x0340});
    CHECK_EQ(r.go(bchg), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x80000000u);
    CHECK_EQ(r.cpu().sr, kX | kN | kV | kC | kZ);

    // BCLR #1,(A0) ; BSET D1,(A0)+ (bit 9 modulo 8 is bit 1 on memory)
    const u32 mem = r.place({0x0890, 0x0001, 0x03D8});
    r.cpu().a[0] = 0x4000;
    r.put8(0x4000, 0x03);
    r.cpu().d[1] = 9;
    r.cpu().sr = 0;
    CHECK_EQ(r.go(mem), ExitReason::Halt);
    CHECK_EQ(r.get8(0x4000), 0x03u);
    CHECK_EQ(r.cpu().a[0], 0x4001u);
    CHECK_EQ(r.cpu().sr, kZ);  // BSET found bit 1 clear after BCLR

    // BTST D1,#$10
    const u32 imm = r.place({0x033C, 0x0010});
    r.cpu().d[1] = 4;
    CHECK_EQ(r.go(imm), ExitReason::Halt);
    CHECK_EQ(r.cpu().sr, 0u);
    r.cpu().d[1] = 5;
    CHECK_EQ(r.go(imm), ExitReason::Halt);
    CHECK_EQ(r.cpu().sr, kZ);

    // BSET #0,(d16,PC) is not allowed, but BTST #n,(d16,PC) is: its
    // displacement counts from the second extension word.
    const u32 pcrel = 0x1400;
    r.code(pcrel, {0x083A, 0x0007, 0x0006, kJmpHalt[0], kJmpHalt[1], 0x8000});
    r.cpu().sr = kZ;
    CHECK_EQ(r.go(pcrel), ExitReason::Halt);
    CHECK_EQ(r.cpu().sr, 0u);
}

void movem_forms() {
    Rig r;
    // MOVEM.W (A0)+,D0/A1 sign-extends both registers.
    const u32 w = r.place({0x4C98, 0x0201});
    r.cpu().a[0] = 0x4000;
    r.put16(0x4000, 0x8001);
    r.put16(0x4002, 0x7FFF);
    CHECK_EQ(r.go(w), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0xFFFF8001u);
    CHECK_EQ(r.cpu().a[1], 0x00007FFFu);
    CHECK_EQ(r.cpu().a[0], 0x4004u);

    // MOVEM.L D0/A0,-(A0) stores the value A0 had before.
    const u32 pre = r.place({0x48E0, 0x8080});
    r.cpu().a[0] = 0x4100;
    r.cpu().d[0] = 0xD0D0D0D0;
    CHECK_EQ(r.go(pre), ExitReason::Halt);
    CHECK_EQ(r.cpu().a[0], 0x40F8u);
    CHECK_EQ(r.get32(0x40F8), 0xD0D0D0D0u);
    CHECK_EQ(r.get32(0x40FC), 0x4100u);

    // MOVEM.L (A0)+,D0/A0 ends with A0 past the block.
    const u32 post = r.place({0x4CD8, 0x0101});
    r.cpu().a[0] = 0x4200;
    r.put32(0x4200, 0x11);
    r.put32(0x4204, 0x22);
    CHECK_EQ(r.go(post), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0x11u);
    CHECK_EQ(r.cpu().a[0], 0x4208u);

    // MOVEM.L D1-D2,8(A1)
    const u32 disp = r.place({0x48E9, 0x0006, 0x0008});
    r.cpu().a[1] = 0x4300;
    r.cpu().d[1] = 0x1111;
    r.cpu().d[2] = 0x2222;
    CHECK_EQ(r.go(disp), ExitReason::Halt);
    CHECK_EQ(r.get32(0x4308), 0x1111u);
    CHECK_EQ(r.get32(0x430C), 0x2222u);
    CHECK_EQ(r.cpu().a[1], 0x4300u);

    // MOVEM.W (6,PC),D3-D4: the displacement counts from the word after
    // the mask.
    const u32 pcrel = 0x1500;
    r.code(pcrel, {0x4CBA, 0x0018, 0x0006, kJmpHalt[0], kJmpHalt[1], 0x0005, 0xFFFE});
    CHECK_EQ(r.go(pcrel), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[3], 5u);
    CHECK_EQ(r.cpu().d[4], 0xFFFFFFFEu);

    // An odd address faults before anything moves.
    r.cpu().a[0] = 0x4001;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.go(post), ExitReason::AddressError);
    CHECK_EQ(r.cpu().fault_addr, 0x4001u);
    CHECK_EQ(r.cpu().d[0], 0u);
}

void link_and_unlink() {
    Rig r;
    // LINK A6,#-8 ; MOVE.L A7,D0 ; UNLK A6
    const u32 at = r.place({0x4E56, 0xFFF8, 0x200F, 0x4E5E});
    r.cpu().a[6] = 0xA6A6A6A6;
    CHECK_EQ(r.go(at), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], kStack - 12);
    CHECK_EQ(r.get32(kStack - 4), 0xA6A6A6A6u);
    CHECK_EQ(r.cpu().a[6], 0xA6A6A6A6u);
    CHECK_EQ(r.cpu().a[7], kStack);

    // LINK A7,#-4 pushes the decremented stack pointer; UNLK A7 then
    // loads A7 from the top of the stack.
    const u32 a7 = r.place({0x4E57, 0xFFFC, 0x200F, 0x4E5F});
    r.put32(kStack - 8, 0x1234);
    CHECK_EQ(r.go(a7), ExitReason::Halt);
    CHECK_EQ(r.get32(kStack - 4), kStack - 4);
    CHECK_EQ(r.cpu().d[0], kStack - 8);
    CHECK_EQ(r.cpu().a[7], 0x1234u);
}

bool cond_true(u32 cond, u16 ccr) {
    const bool c = (ccr & kC) != 0;
    const bool v = (ccr & kV) != 0;
    const bool z = (ccr & kZ) != 0;
    const bool n = (ccr & kN) != 0;
    switch (cond) {
    case 0: return true;
    case 1: return false;
    case 2: return !c && !z;
    case 3: return c || z;
    case 4: return !c;
    case 5: return c;
    case 6: return !z;
    case 7: return z;
    case 8: return !v;
    case 9: return v;
    case 10: return !n;
    case 11: return n;
    case 12: return n == v;
    case 13: return n != v;
    case 14: return !z && n == v;
    default: return z || n != v;
    }
}

void conditions() {
    Rig r;
    for (u16 cond = 0; cond < 16; ++cond) {
        // Bcc.S +2 ; ADDQ.W #1,A0 ; JMP halt (BSR when cond 1)
        const u32 bcc = r.place({static_cast<u16>(0x6002 | (cond << 8)), 0x5248});
        // Bcc.W +4 ; ADDQ.W #1,A0 ; NOP
        const u32 bccw = r.place({static_cast<u16>(0x6000 | (cond << 8)), 0x0004, 0x5248, 0x4E71});
        // Scc D1
        const u32 scc = r.place({static_cast<u16>(0x50C1 | (cond << 8))});
        // DBcc D2,* : counts D2 down to -1 unless the condition holds.
        const u32 dbcc = r.place({static_cast<u16>(0x50CA | (cond << 8)), 0xFFFE});
        for (u16 ccr = 0; ccr < 32; ++ccr) {
            const bool t = cond_true(cond, ccr);
            r.cpu().sr = ccr;
            r.cpu().a[7] = kStack;
            r.cpu().a[0] = 0;
            CHECK_EQ(r.go(bcc), ExitReason::Halt);
            CHECK_EQ(r.cpu().a[0], (t || cond == 1) ? 0u : 1u);
            CHECK_EQ(r.cpu().a[7], cond == 1 ? kStack - 4 : kStack);
            CHECK_EQ(r.cpu().sr, ccr);
            r.cpu().a[7] = kStack;
            r.cpu().a[0] = 0;
            CHECK_EQ(r.go(bccw), ExitReason::Halt);
            CHECK_EQ(r.cpu().a[0], (t || cond == 1) ? 0u : 1u);
            CHECK_EQ(r.cpu().sr, ccr);
            r.cpu().d[1] = 0x12345678;
            CHECK_EQ(r.go(scc), ExitReason::Halt);
            CHECK_EQ(r.cpu().d[1], t ? 0x123456FFu : 0x12345600u);
            CHECK_EQ(r.cpu().sr, ccr);
            r.cpu().d[2] = 0xABCD0003;
            CHECK_EQ(r.go(dbcc), ExitReason::Halt);
            CHECK_EQ(r.cpu().d[2], t ? 0xABCD0003u : 0xABCDFFFFu);
        }
    }
    // SEQ (A0)
    const u32 seq = r.place({0x57D0});
    r.cpu().a[0] = 0x4000;
    r.cpu().sr = kZ;
    CHECK_EQ(r.go(seq), ExitReason::Halt);
    CHECK_EQ(r.get8(0x4000), 0xFFu);
}

void jumps_and_returns() {
    Rig r;
    // JMP (A0)
    const u32 jmp = r.place({0x4ED0, 0x7001});
    r.cpu().a[0] = jmp + 4;
    r.cpu().d[0] = 0;
    CHECK_EQ(r.go(jmp), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 0u);

    // JSR (8,PC) ; MOVEQ #1,D0 ; JMP halt ; sub: MOVEQ #2,D1 ; RTS
    const u32 jsr = 0x1600;
    r.code(jsr, {0x4EBA, 0x0008, 0x7001, kJmpHalt[0], kJmpHalt[1], 0x7202, 0x4E75});
    CHECK_EQ(r.go(jsr), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 1u);
    CHECK_EQ(r.cpu().d[1], 2u);
    CHECK_EQ(r.cpu().a[7], kStack);

    // BSR.W to the same subroutine.
    const u32 bsr = 0x1700;
    r.code(bsr, {0x6100, 0x0008, 0x7003, kJmpHalt[0], kJmpHalt[1], 0x7204, 0x4E75});
    CHECK_EQ(r.go(bsr), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], 3u);
    CHECK_EQ(r.cpu().d[1], 4u);

    // RTR pops the CCR (five bits) and then the PC.
    const u32 rtr = r.place({0x4E77});
    r.cpu().a[7] = kStack - 6;
    r.put16(kStack - 6, 0xFFFF);
    r.put32(kStack - 4, jmp + 4);
    r.cpu().sr = 0x2700;
    CHECK_EQ(r.go(rtr), ExitReason::Halt);
    CHECK_EQ(r.cpu().sr, 0x271Fu);
    CHECK_EQ(r.cpu().a[7], kStack);

    // RTS to an odd address.
    const u32 rts = r.place({0x4E75});
    r.cpu().a[7] = kStack - 4;
    r.put32(kStack - 4, 0x2001);
    CHECK_EQ(r.go(rts), ExitReason::AddressError);
    CHECK_EQ(r.cpu().fault_addr, 0x2001u);
}

void unsupported_opcodes_stop_the_run() {
    Rig r;
    // The instructions that call out to helpers are not translated yet.
    const std::initializer_list<u16> later[] = {
        {0x80FC, 0x0000}, {0x81FC, 0x0001}, {0x40C0}, {0x4E41}, {0x4AC0},
        {0x41BC, 0x0005}, {0x0188, 0x0000}, {0xE4D0}, {0xE330}, {0x4800},
        {0x8300},         {0xC101},         {0x4E60}, {0x4E72, 0x2000}, {0x4E73},
        {0x4E70},         {0x4E76},         {0x003C, 0x0001}, {0x44C0}, {0x027C, 0xF8FF}};
    for (const auto& op : later) {
        const u32 at = r.place({0x7003});
        r.code(at + 2, op);
        r.cpu().d[0] = 0;
        CHECK_EQ(r.go(at), ExitReason::Unimplemented);
        CHECK_EQ(r.cpu().pc, at + 2);
        CHECK_EQ(r.cpu().d[0], 3u);
    }
    // ILLEGAL, an F-line opcode and a 68010 MOVEC.
    for (u16 op : {u16(0x4AFC), u16(0xF000), u16(0x4E7A)}) {
        const u32 at = r.place({0x7004});
        r.code(at + 2, {op, 0x0801});
        CHECK_EQ(r.go(at), ExitReason::Illegal);
        CHECK_EQ(r.cpu().pc, at + 2);
        CHECK_EQ(r.cpu().d[0], 4u);
    }
}

void block_limit_hook() {
    Rig r;
    g_jit.set_block_limit(1);
    // One instruction per block: two blocks run two instructions.
    r.program(kCode, {0x7001, 0x7202, 0x7403});
    CHECK_EQ(r.go(kCode, 2), ExitReason::Budget);
    CHECK_EQ(r.cpu().pc, kCode + 4);
    CHECK_EQ(r.cpu().d[1], 2u);
    CHECK_EQ(r.cpu().d[2], 0u);
    g_jit.set_block_limit(64);
    CHECK_EQ(r.go(kCode, 3), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[2], 3u);
}

void init_refuses_bad_setups() {
    Machine m{};
    std::vector<u8> small(0x400);
    m.mem = GuestMemory{small.data(), 0x400};
    u8 code[4096];
    CHECK(!g_jit.init(CodeRegion{code, code, 64}, m));
    CHECK(!g_jit.init(CodeRegion{code, code, sizeof code}, m));  // no room for the stubs
    CHECK(!g_jit.init(CodeRegion{nullptr, code, sizeof code}, m));
}

}

int main() {
    moveq_addq_move_and_halt();
    add_word_sets_zero_carry_and_extend();
    dbf_loop_counts_down();
    jsr_and_rts();
    movem_round_trip();
    bus_and_address_errors();
    endless_loop_runs_out_of_budget();
    natives_are_called_by_their_aline_opcode();
    flush_drops_translations();
    small_region_flushes_and_completes();
    call_guest_returns_through_the_stub();
    addressing_modes_read();
    addressing_modes_write();
    moves_set_flags_and_keep_upper_bits();
    lea_pea_and_friends();
    arithmetic_flags_match_the_reference();
    arithmetic_with_other_operands();
    logic_with_other_operands();
    shifts_match_the_reference();
    bit_operations();
    movem_forms();
    link_and_unlink();
    conditions();
    jumps_and_returns();
    unsupported_opcodes_stop_the_run();
    block_limit_hook();
    init_refuses_bad_setups();
    return check::finish("test_m68k_jit");
}
