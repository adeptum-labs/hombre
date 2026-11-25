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

#include "m68k_jit.hpp"

#include "log.hpp"
#include "m68k_decode.hpp"
#include "x86_emit.hpp"

namespace hombre::m68k {

namespace {

using namespace hombre::x86;

constexpr i32 kOffD = __builtin_offsetof(CpuState, d);
constexpr i32 kOffA = __builtin_offsetof(CpuState, a);
constexpr i32 kOffPc = __builtin_offsetof(CpuState, pc);
constexpr i32 kOffSr = __builtin_offsetof(CpuState, sr);
constexpr i32 kOffExit = __builtin_offsetof(CpuState, exit);
constexpr i32 kOffFault = __builtin_offsetof(CpuState, fault_addr);
constexpr i32 kOffTrap = __builtin_offsetof(CpuState, trap_number);
constexpr i32 kOffUsp = __builtin_offsetof(CpuState, usp);

// Room kept free for the code that ends a block, and the least room a
// region must have for blocks beyond the shared routines.
constexpr usize kTailBytes = 16;
constexpr usize kMinBlockBytes = 1024;

// Above this the rel32 jumps inside the region could not reach.
constexpr usize kMaxRegionBytes = 0x40000000;

constexpr u32 kNoAddressMask = 0xFFFFFFFF;

constexpr u32 kCcrX = 0x10;
constexpr u32 kCcrZ = 0x04;
// T, S, the interrupt mask and the CCR: the bits of SR a 68000 has.
constexpr u32 kSrBits = 0xA71F;

i32 dreg(u32 n) {
    return kOffD + static_cast<i32>(4 * n);
}

i32 areg(u32 n) {
    return kOffA + static_cast<i32>(4 * n);
}

// D0-D7 then A0-A7, the order of a MOVEM mask.
i32 any_reg(u32 n) {
    return n < 8 ? dreg(n) : areg(n - 8);
}

unsigned bits_of(Size s) {
    switch (s) {
    case Size::Byte: return 8;
    case Size::Word: return 16;
    default: return 32;
    }
}

u32 bytes_of(Size s) {
    return bits_of(s) / 8;
}

u32 size_index(Size s) {
    switch (s) {
    case Size::Byte: return 0;
    case Size::Word: return 1;
    default: return 2;
    }
}

// Whether a condition holds for a CCR nibble (C bit 0, V 1, Z 2, N 3).
constexpr bool cond_holds(u32 cond, u32 ccr) {
    const bool c = (ccr & 1) != 0;
    const bool v = (ccr & 2) != 0;
    const bool z = (ccr & 4) != 0;
    const bool n = (ccr & 8) != 0;
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

// Bit k of entry c is set when condition c holds for the CCR nibble k.
constexpr u32 cond_table(u32 cond) {
    u32 table = 0;
    for (u32 ccr = 0; ccr < 16; ++ccr) {
        if (cond_holds(cond, ccr)) {
            table |= 1u << ccr;
        }
    }
    return table;
}

constexpr u32 kCondTable[16] = {
    cond_table(0),  cond_table(1),  cond_table(2),  cond_table(3),
    cond_table(4),  cond_table(5),  cond_table(6),  cond_table(7),
    cond_table(8),  cond_table(9),  cond_table(10), cond_table(11),
    cond_table(12), cond_table(13), cond_table(14), cond_table(15)};

// --- the shared routines ------------------------------------------------

// Merges the CCR bits in ECX into sr under mask. With sticky_z, Z can only
// be cleared: it stays set only if it was set before and ECX has it set.
void emit_ccr_merge(Emitter& e, u32 mask, bool sticky_z) {
    e.mov_load(RDX, RBX, kOffSr, 16);
    e.movzx(RDX, RDX, 16);
    if (sticky_z) {
        e.mov_rr(RAX, RDX, 32);
        e.alu_ri(Alu::Or, RAX, static_cast<i32>(~kCcrZ), 32);
        e.alu_rr(Alu::And, RCX, RAX, 32);
    }
    e.alu_ri(Alu::And, RDX, static_cast<i32>(~mask), 32);
    e.alu_rr(Alu::Or, RDX, RCX, 32);
    e.mov_store(RBX, kOffSr, RDX, 16);
}

// Called right after an x86 instruction whose flags match the 68000's (a
// call leaves the flags alone): sets N, Z, V and C from SF, ZF, OF and CF,
// and X from CF when asked. Clobbers RAX, RCX and RDX.
usize emit_ccr_from_flags(Emitter& e, bool set_x, bool sticky_z) {
    const usize at = e.size();
    e.pushfq();
    e.pop(RAX);
    e.mov_rr(RCX, RAX, 32);
    e.alu_ri(Alu::And, RCX, 1, 32);  // CF, bit 0, is C
    e.mov_rr(RDX, RAX, 32);
    e.shift_i(Shift::Shr, RDX, 4, 32);
    e.alu_ri(Alu::And, RDX, 0x0C, 32);  // SF, bit 7, is N; ZF, bit 6, is Z
    e.alu_rr(Alu::Or, RCX, RDX, 32);
    e.shift_i(Shift::Shr, RAX, 10, 32);
    e.alu_ri(Alu::And, RAX, 2, 32);  // OF, bit 11, is V
    e.alu_rr(Alu::Or, RCX, RAX, 32);
    if (set_x) {
        e.mov_rr(RAX, RCX, 32);
        e.shift_i(Shift::Shl, RAX, 4, 32);
        e.alu_ri(Alu::And, RAX, static_cast<i32>(kCcrX), 32);
        e.alu_rr(Alu::Or, RCX, RAX, 32);
    }
    emit_ccr_merge(e, set_x ? 0x1F : 0x0F, sticky_z);
    e.ret();
    return at;
}

// For shifts and rotates: N and Z from the flags of an OR of the result
// with itself, C from R10D and V from R11D (each 0 or 1), X from C when
// asked. Clobbers RAX, RCX and RDX.
usize emit_ccr_shift(Emitter& e, bool set_x) {
    const usize at = e.size();
    e.pushfq();
    e.pop(RAX);
    e.mov_rr(RCX, RAX, 32);
    e.shift_i(Shift::Shr, RCX, 4, 32);
    e.alu_ri(Alu::And, RCX, 0x0C, 32);
    e.alu_rr(Alu::Or, RCX, R10, 32);
    e.mov_rr(RAX, R11, 32);
    e.alu_rr(Alu::Add, RAX, RAX, 32);
    e.alu_rr(Alu::Or, RCX, RAX, 32);
    if (set_x) {
        e.mov_rr(RAX, R10, 32);
        e.shift_i(Shift::Shl, RAX, 4, 32);
        e.alu_rr(Alu::Or, RCX, RAX, 32);
    }
    emit_ccr_merge(e, set_x ? 0x1F : 0x0F, false);
    e.ret();
    return at;
}

// Ends the run with a fault at the address in R8D, for the instruction at
// R15D. Reached by a jump from any depth of calls: the stack is unwound
// from RBP.
usize emit_fault(Emitter& e, const detail::Prelude& p, ExitReason why, u32 vector) {
    const usize at = e.size();
    e.mov_rr(RSP, RBP, 64);
    e.mov_store(RBX, kOffFault, R8, 32);
    e.mov_store(RBX, kOffPc, R15, 32);
    e.mov_ri(RAX, vector, 32);
    e.mov_store(RBX, kOffTrap, RAX, 32);
    e.mov_ri(RAX, static_cast<u32>(why), 32);
    e.mov_store(RBX, kOffExit, RAX, 32);
    e.alu_rr(Alu::Xor, RAX, RAX, 32);
    e.jmp_to(p.leave);
    return at;
}

// Alignment, then bounds, of an access of n bytes at R8D. Leaves the masked
// address, the offset into guest memory, in RCX.
void emit_access_check(Emitter& e, const detail::Prelude& p, u32 n, u32 mask) {
    if (n > 1) {
        e.mov_rr(RCX, R8, 32);
        e.alu_ri(Alu::And, RCX, 1, 32);
        e.jcc_to(Cc::NE, p.address_error);
    }
    e.mov_rr(RCX, R8, 32);
    if (mask != kNoAddressMask) {
        e.alu_ri(Alu::And, RCX, static_cast<i32>(mask), 32);
    }
    e.alu_ri(Alu::Add, RCX, static_cast<i32>(n), 64);
    e.alu_rr(Alu::Cmp, RCX, R13, 64);
    e.jcc_to(Cc::A, p.bus_error);
    e.alu_ri(Alu::Sub, RCX, static_cast<i32>(n), 64);
}

// The entry used from C++:
//   u32 enter(CpuState* cpu, u8* base, u64 size, const u8* block)
// returns 0, or the A-line opcode the block stopped at. Six pushes keep the
// stack 16-byte aligned at every call made from block level.
void emit_prelude(Emitter& e, detail::Prelude& p, u32 mask) {
    p.enter = e.size();
    e.push(RBX);
    e.push(RBP);
    e.push(R12);
    e.push(R13);
    e.push(R14);
    e.push(R15);
    e.mov_rr(RBX, RDI, 64);
    e.mov_rr(R12, RSI, 64);
    e.mov_rr(R13, RDX, 64);
    e.mov_rr(RBP, RSP, 64);
    e.call_r(RCX);
    p.leave = e.size();
    e.pop(R15);
    e.pop(R14);
    e.pop(R13);
    e.pop(R12);
    e.pop(RBP);
    e.pop(RBX);
    e.ret();

    p.bus_error = emit_fault(e, p, ExitReason::BusError, 2);
    p.address_error = emit_fault(e, p, ExitReason::AddressError, 3);

    // A helper that ended the run has set the exit already.
    p.helper_exit = e.size();
    e.mov_rr(RSP, RBP, 64);
    e.alu_rr(Alu::Xor, RAX, RAX, 32);
    e.jmp_to(p.leave);

    // Reads return the value zero-extended in EAX; writes store EAX. Both
    // take the address in R8D and clobber RCX.
    for (u32 i = 0; i < 3; ++i) {
        const u32 n = 1u << i;
        p.read[i] = e.size();
        emit_access_check(e, p, n, mask);
        e.mov_load_idx(RAX, R12, RCX, 8 * n);
        if (n == 1) {
            e.movzx(RAX, RAX, 8);
        } else if (n == 2) {
            e.bswap(RAX, 16);
            e.movzx(RAX, RAX, 16);
        } else {
            e.bswap(RAX, 32);
        }
        e.ret();
    }
    for (u32 i = 0; i < 3; ++i) {
        const u32 n = 1u << i;
        p.write[i] = e.size();
        emit_access_check(e, p, n, mask);
        if (n > 1) {
            e.bswap(RAX, 8 * n);
        }
        e.mov_store_idx(R12, RCX, RAX, 8 * n);
        e.ret();
    }

    p.ccr_nzvc = emit_ccr_from_flags(e, false, false);
    p.ccr_nzvcx = emit_ccr_from_flags(e, true, false);
    p.ccr_extend = emit_ccr_from_flags(e, true, true);
    p.ccr_shift = emit_ccr_shift(e, false);
    p.ccr_shift_x = emit_ccr_shift(e, true);
    p.end = e.size();
}

// --- helpers called from generated code -------------------------------
//
// Plain SysV functions: the CpuState comes first and the result is in EAX.
// Generated code calls them from block level through mov rax, imm64; call
// rax, with the stack 16-byte aligned. They touch no guest memory; the
// translated code reads and writes the operands through the checked
// access routines.

constexpr u32 kCcrN = 0x08;
constexpr u32 kCcrV = 0x02;
constexpr u32 kCcrC = 0x01;

void set_ccr(CpuState* cpu, u32 ccr) {
    cpu->sr = static_cast<u16>((cpu->sr & ~0x1Fu) | ccr);
}

// The exception exits that stop after the instruction, as the 68000 stacks
// the address of the next one.
u32 take_exception(CpuState* cpu, ExitReason why, u32 vector, u32 next_pc) {
    cpu->exit = why;
    cpu->trap_number = vector;
    cpu->pc = next_pc;
    return 1;
}

// ABCD: dst + src + X. SBCD: dst - src - X; NBCD is SBCD from 0. The
// binary result is corrected by 6 in each digit that carried (or, adding,
// came out above 9). V is set where the correction turned bit 7 on
// (adding) or off (subtracting), N is bit 7 of the result, and Z is only
// ever cleared.
u32 helper_bcd(CpuState* cpu, u32 src, u32 dst, u32 subtract) {
    const u32 x = (cpu->sr & kCcrX) != 0 ? 1 : 0;
    src &= 0xFF;
    dst &= 0xFF;
    u32 result;
    u32 carry;
    u32 overflow;
    if (subtract == 0) {
        const u32 sum = dst + src + x;
        const u32 binary_carry = ((dst & src) | (~sum & dst) | (~sum & src)) & 0x88;
        const u32 decimal_carry = (((sum + 0x66) ^ sum) & 0x110) >> 1;
        const u32 digits = binary_carry | decimal_carry;
        result = sum + (digits - (digits >> 2));
        carry = (binary_carry | (sum & ~result)) >> 7 & 1;
        overflow = (~sum & result) >> 7 & 1;
    } else {
        const u32 diff = dst - src - x;
        const u32 borrow = ((~dst & src) | (diff & ~dst) | (diff & src)) & 0x88;
        result = diff - (borrow - (borrow >> 2));
        carry = (borrow | (~diff & result)) >> 7 & 1;
        overflow = (diff & ~result) >> 7 & 1;
    }
    result &= 0xFF;
    u32 ccr = (carry != 0 ? kCcrX | kCcrC : 0) | (overflow != 0 ? kCcrV : 0) |
              ((result & 0x80) != 0 ? kCcrN : 0);
    if (result == 0) {
        ccr |= cpu->sr & kCcrZ;
    }
    set_ccr(cpu, ccr);
    return result;
}

// ROXL and ROXR of a bits-wide value (kind: bits, plus 0x100 for left)
// through X, by count modulo 64. C is the last bit out, or X for a count
// of 0; X takes the last bit out; V is cleared.
u32 helper_rox(CpuState* cpu, u32 value, u32 count, u32 kind) {
    const u32 bits = kind & 0xFF;
    const bool left = (kind & 0x100) != 0;
    const u32 mask = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    const u32 msb = 1u << (bits - 1);
    u32 v = value & mask;
    u32 x = (cpu->sr & kCcrX) != 0 ? 1 : 0;
    // A rotation through X comes back round after bits + 1 single turns.
    const u32 turns = (count & 63) % (bits + 1);
    for (u32 i = 0; i < turns; ++i) {
        const u32 out = left ? (v & msb) != 0 : v & 1;
        v = left ? ((v << 1) | x) & mask : (v >> 1) | (x != 0 ? msb : 0);
        x = out;
    }
    set_ccr(cpu, (x != 0 ? kCcrX | kCcrC : 0) | ((v & msb) != 0 ? kCcrN : 0) |
                     (v == 0 ? kCcrZ : 0));
    return v;
}

// DIVU and DIVS of D[kind & 7] by a word (kind bit 8: signed). The
// quotient goes to the low word and the remainder, with the sign of the
// dividend, to the high word. A quotient too large for 16 bits leaves the
// register alone, sets V, clears C and keeps N and Z. Division by zero
// ends the run.
u32 helper_divide(CpuState* cpu, u32 divisor, u32 kind, u32 next_pc) {
    const u32 reg = kind & 7;
    const bool is_signed = (kind & 0x100) != 0;
    const u32 dividend = cpu->d[reg];
    const u32 x = cpu->sr & kCcrX;
    divisor &= 0xFFFF;
    if (divisor == 0) {
        set_ccr(cpu, x);
        return take_exception(cpu, ExitReason::DivideByZero, 5, next_pc);
    }
    u32 quotient;
    u32 remainder;
    bool overflow;
    if (is_signed) {
        const i64 n = static_cast<__INT32_TYPE__>(dividend);
        const i64 d = static_cast<__INT16_TYPE__>(divisor);
        const i64 q = n / d;
        overflow = q < -32768 || q > 32767;
        quotient = static_cast<u32>(q);
        remainder = static_cast<u32>(n % d);
    } else {
        quotient = dividend / divisor;
        remainder = dividend % divisor;
        overflow = quotient > 0xFFFF;
    }
    if (overflow) {
        set_ccr(cpu, (cpu->sr & (kCcrX | kCcrN | kCcrZ)) | kCcrV);
        return 0;
    }
    quotient &= 0xFFFF;
    cpu->d[reg] = (remainder << 16) | quotient;
    set_ccr(cpu, x | ((quotient & 0x8000) != 0 ? kCcrN : 0) | (quotient == 0 ? kCcrZ : 0));
    return 0;
}

// CHK: the run ends when the word in D[reg] is below 0 (N set) or above
// the bound (N cleared).
u32 helper_chk(CpuState* cpu, u32 bound, u32 reg, u32 next_pc) {
    const i64 value = static_cast<__INT16_TYPE__>(cpu->d[reg & 7]);
    const i64 limit = static_cast<__INT16_TYPE__>(bound);
    const u32 kept = cpu->sr & (kCcrX | kCcrN);
    const u32 zero = value == 0 ? kCcrZ : 0;
    if (value < 0) {
        set_ccr(cpu, (kept & kCcrX) | kCcrN | zero);
        return take_exception(cpu, ExitReason::Chk, 6, next_pc);
    }
    if (value > limit) {
        set_ccr(cpu, (kept & kCcrX) | zero);
        return take_exception(cpu, ExitReason::Chk, 6, next_pc);
    }
    set_ccr(cpu, kept | zero);
    return 0;
}

template <typename F>
u64 helper_address(F* fn) {
    return reinterpret_cast<u64>(fn);
}

// --- one instruction --------------------------------------------------

bool is_shift(Op op) {
    switch (op) {
    case Op::ASL:
    case Op::ASR:
    case Op::LSL:
    case Op::LSR:
    case Op::ROL:
    case Op::ROR:
    case Op::ROXL:
    case Op::ROXR:
        return true;
    default:
        return false;
    }
}

// The extension words an operand of this instruction takes.
u32 operand_words(const Insn& in, const Ea& ea) {
    switch (ea.mode) {
    case Mode::AnDisp:
    case Mode::AnIndex:
    case Mode::AbsW:
    case Mode::PcDisp:
    case Mode::PcIndex:
        return 1;
    case Mode::AbsL:
        return 2;
    case Mode::Imm:
        if (in.op == Op::ADDQ || in.op == Op::SUBQ || in.op == Op::MOVEQ || is_shift(in.op)) {
            return 0;
        }
        if (in.op == Op::BTST || in.op == Op::BCHG || in.op == Op::BCLR || in.op == Op::BSET ||
            in.op == Op::MOVEM) {
            return 1;
        }
        return in.size == Size::Long ? 2 : 1;
    default:
        return 0;
    }
}

// Emits the code of one instruction at a time into a block.
//
// Register use inside an instruction: ESI the source value, EDI the
// destination value or result, R8D the address of the memory access, R10D
// and R11D temporaries, RAX, RCX and RDX scratch for the shared routines.
class Translator {
public:
    Translator(Emitter& e, const detail::Prelude& p) : e_(e), p_(p) {}

    // Returns true if the instruction ends the block.
    bool insn(const Insn& in, u32 pc);

    // Leaves the block for the guest address target.
    void end_at(u32 target) {
        e_.mov_ri(RAX, target, 32);
        e_.mov_store(RBX, kOffPc, RAX, 32);
        e_.alu_rr(Alu::Xor, RAX, RAX, 32);
        e_.ret();
    }

private:
    void end_reg(Reg target) {
        e_.mov_store(RBX, kOffPc, target, 32);
        e_.alu_rr(Alu::Xor, RAX, RAX, 32);
        e_.ret();
    }

    // Ends the run with pc at the given address and the vector of the
    // exception the 68000 would take (0 for none) in trap_number.
    bool stop(ExitReason why, u32 at, u32 vector) {
        e_.mov_ri(RAX, at, 32);
        e_.mov_store(RBX, kOffPc, RAX, 32);
        e_.mov_ri(RAX, vector, 32);
        e_.mov_store(RBX, kOffTrap, RAX, 32);
        e_.mov_ri(RAX, static_cast<u32>(why), 32);
        e_.mov_store(RBX, kOffExit, RAX, 32);
        e_.alu_rr(Alu::Xor, RAX, RAX, 32);
        e_.ret();
        return true;
    }

    // The faulting-instruction address the fault routines report.
    void fault_pc() {
        if (!pc_loaded_) {
            e_.mov_ri(R15, pc_, 32);
            pc_loaded_ = true;
        }
    }

    void read_mem(Size s) {
        fault_pc();
        e_.call_to(p_.read[size_index(s)]);
    }

    void write_mem(Size s) {
        fault_pc();
        e_.call_to(p_.write[size_index(s)]);
    }

    void add_r8(u32 value) {
        if (value != 0) {
            e_.alu_ri(Alu::Add, R8, static_cast<i32>(value), 32);
        }
    }

    void index(const Ea& ea) {
        const u32 r = (ea.ext >> 12) & 7;
        e_.mov_load(RCX, RBX, (ea.ext & 0x8000) != 0 ? areg(r) : dreg(r), 32);
        if ((ea.ext & 0x0800) == 0) {
            e_.movsx(RCX, RCX, 16);
        }
        e_.alu_rr(Alu::Add, R8, RCX, 32);
    }

    // (An)+ and -(An) of a byte move A7 by two to keep it even.
    static u32 increment(const Ea& ea, Size s) {
        return s == Size::Byte && ea.reg == 7 ? 2 : bytes_of(s);
    }

    // The address of a memory operand into R8D, with the side effect of
    // (An)+ and -(An). ext_pc is where the operand's extension word sits.
    void address(const Ea& ea, Size s, u32 ext_pc) {
        switch (ea.mode) {
        case Mode::AnInd:
            e_.mov_load(R8, RBX, areg(ea.reg), 32);
            break;
        case Mode::AnPost:
            e_.mov_load(R8, RBX, areg(ea.reg), 32);
            e_.mov_rr(RCX, R8, 32);
            e_.alu_ri(Alu::Add, RCX, static_cast<i32>(increment(ea, s)), 32);
            e_.mov_store(RBX, areg(ea.reg), RCX, 32);
            break;
        case Mode::AnPre:
            e_.mov_load(R8, RBX, areg(ea.reg), 32);
            e_.alu_ri(Alu::Sub, R8, static_cast<i32>(increment(ea, s)), 32);
            e_.mov_store(RBX, areg(ea.reg), R8, 32);
            break;
        case Mode::AnDisp:
            e_.mov_load(R8, RBX, areg(ea.reg), 32);
            add_r8(ea.value);
            break;
        case Mode::AnIndex:
            e_.mov_load(R8, RBX, areg(ea.reg), 32);
            index(ea);
            add_r8(ea.value);
            break;
        case Mode::AbsW:
        case Mode::AbsL:
            e_.mov_ri(R8, ea.value, 32);
            break;
        case Mode::PcDisp:
            e_.mov_ri(R8, ext_pc + ea.value, 32);
            break;
        case Mode::PcIndex:
            e_.mov_ri(R8, ext_pc + ea.value, 32);
            index(ea);
            break;
        default:
            break;
        }
    }

    void widen(Reg r, Size s) {
        if (s != Size::Long) {
            e_.movzx(r, r, bits_of(s));
        }
    }

    // The operand's value, zero-extended to 32 bits, into out. A memory
    // operand leaves its address in R8D.
    void read(const Ea& ea, Size s, Reg out, u32 ext_pc) {
        switch (ea.mode) {
        case Mode::Dn:
            e_.mov_load(out, RBX, dreg(ea.reg), 32);
            widen(out, s);
            return;
        case Mode::An:
            e_.mov_load(out, RBX, areg(ea.reg), 32);
            widen(out, s);
            return;
        case Mode::Imm:
            e_.mov_ri(out, ea.value, 32);
            return;
        default:
            address(ea, s, ext_pc);
            read_mem(s);
            e_.mov_rr(out, RAX, 32);
            return;
        }
    }

    // Stores the low part of val. With have_address, a memory operand's
    // address is already in R8D from the read before.
    void write(const Ea& ea, Size s, Reg val, u32 ext_pc, bool have_address) {
        switch (ea.mode) {
        case Mode::Dn:
            e_.mov_store(RBX, dreg(ea.reg), val, bits_of(s));
            return;
        case Mode::An:
            e_.mov_store(RBX, areg(ea.reg), val, 32);
            return;
        default:
            if (!have_address) {
                address(ea, s, ext_pc);
            }
            e_.mov_rr(RAX, val, 32);
            write_mem(s);
            return;
        }
    }

    void logic_flags(Reg r, Size s) {
        e_.alu_rr(Alu::Or, r, r, bits_of(s));
        e_.call_to(p_.ccr_nzvc);
    }

    // CF = X.
    void load_x() {
        e_.mov_load(RCX, RBX, kOffSr, 16);
        e_.shift_i(Shift::Shr, RCX, 5, 16);
    }

    // EAX = 1 if the condition holds, else 0, with ZF set when it does not.
    void condition(u32 cond) {
        e_.mov_load(RCX, RBX, kOffSr, 16);
        e_.alu_ri(Alu::And, RCX, 15, 32);
        e_.mov_ri(RAX, kCondTable[cond & 15], 32);
        e_.shift_cl(Shift::Shr, RAX, 32);
        e_.alu_ri(Alu::And, RAX, 1, 32);
    }

    void push_imm(u32 value) {
        e_.mov_load(R8, RBX, areg(7), 32);
        e_.alu_ri(Alu::Sub, R8, 4, 32);
        e_.mov_ri(RAX, value, 32);
        write_mem(Size::Long);
        e_.mov_store(RBX, areg(7), R8, 32);
    }

    void push_reg(Reg r) {
        e_.mov_load(R8, RBX, areg(7), 32);
        e_.alu_ri(Alu::Sub, R8, 4, 32);
        e_.mov_rr(RAX, r, 32);
        write_mem(Size::Long);
        e_.mov_store(RBX, areg(7), R8, 32);
    }

    // dst = dst op src with the given CCR routine; CMP stores nothing.
    void binary(const Insn& in, Alu op, usize ccr) {
        read(in.src, in.size, RSI, src_pc_);
        read(in.dst, in.size, RDI, dst_pc_);
        e_.alu_rr(op, RDI, RSI, bits_of(in.size));
        e_.call_to(ccr);
        if (op != Alu::Cmp) {
            write(in.dst, in.size, RDI, dst_pc_, true);
        }
    }

    // ADDX and SUBX: Z is only ever cleared.
    void extended(const Insn& in, Alu op) {
        read(in.src, in.size, RSI, src_pc_);
        read(in.dst, in.size, RDI, dst_pc_);
        load_x();
        e_.alu_rr(op, RDI, RSI, bits_of(in.size));
        e_.call_to(p_.ccr_extend);
        write(in.dst, in.size, RDI, dst_pc_, true);
    }

    // ADDA, SUBA, CMPA and ADDQ/SUBQ to An: the whole register, with the
    // source sign-extended from a word.
    void address_arith(const Insn& in, Alu op) {
        read(in.src, in.size, RSI, src_pc_);
        if (in.size == Size::Word) {
            e_.movsx(RSI, RSI, 16);
        }
        e_.mov_load(RDI, RBX, areg(in.dst.reg), 32);
        e_.alu_rr(op, RDI, RSI, 32);
        if (op == Alu::Cmp) {
            e_.call_to(p_.ccr_nzvc);
        } else {
            e_.mov_store(RBX, areg(in.dst.reg), RDI, 32);
        }
    }

    void negate(const Insn& in, bool extend) {
        read(in.dst, in.size, RDI, dst_pc_);
        e_.mov_ri(RSI, 0, 32);
        if (extend) {
            load_x();
            e_.alu_rr(Alu::Sbb, RSI, RDI, bits_of(in.size));
            e_.call_to(p_.ccr_extend);
        } else {
            e_.alu_rr(Alu::Sub, RSI, RDI, bits_of(in.size));
            e_.call_to(p_.ccr_nzvcx);
        }
        write(in.dst, in.size, RSI, dst_pc_, true);
    }

    void clear(const Insn& in) {
        e_.mov_ri(RSI, 0, 32);
        write(in.dst, in.size, RSI, dst_pc_, false);
        e_.mov_load(RCX, RBX, kOffSr, 16);
        e_.alu_ri(Alu::And, RCX, ~0x0F, 32);
        e_.alu_ri(Alu::Or, RCX, static_cast<i32>(kCcrZ), 32);
        e_.mov_store(RBX, kOffSr, RCX, 16);
    }

    void multiply(const Insn& in, bool is_signed) {
        read(in.src, Size::Word, RSI, src_pc_);
        e_.mov_load(RDI, RBX, dreg(in.dst.reg), 32);
        if (is_signed) {
            e_.movsx(RSI, RSI, 16);
            e_.movsx(RDI, RDI, 16);
        } else {
            e_.movzx(RDI, RDI, 16);
        }
        e_.imul_rr(RDI, RSI, 32);
        e_.mov_store(RBX, dreg(in.dst.reg), RDI, 32);
        logic_flags(RDI, Size::Long);
    }

    // ASL, ASR, LSL, LSR, ROL and ROR of a register (count 1-8 or Dn modulo
    // 64) or of a memory word (count 1). The shifts run on 64-bit registers
    // so every count up to 63 gives the 68000 result: left shifts with the
    // operand at the top, right shifts with it at the bottom.
    void shift(const Insn& in) {
        const unsigned w = bits_of(in.size);
        const bool by_register = in.src.mode == Mode::Dn;
        read(in.dst, in.size, RDI, dst_pc_);
        if (by_register) {
            e_.mov_load(RCX, RBX, dreg(in.src.reg), 32);
        } else {
            e_.mov_ri(RCX, in.src.mode == Mode::Imm ? in.src.value : 1, 32);
        }
        const bool left = in.op == Op::ASL || in.op == Op::LSL;
        if (left) {
            e_.shift_i(Shift::Shl, RDI, static_cast<u8>(64 - w), 64);
            if (in.op == Op::ASL) {
                e_.mov_rr(R11, RDI, 64);
            }
        } else if (in.op == Op::ASR) {
            if (w < 32) {
                e_.movsx(RDI, RDI, w);
            }
            e_.movsx(RDI, RDI, 32);
        }
        e_.alu_ri(Alu::And, RCX, 63, 32);  // also clears CF for a count of 0
        switch (in.op) {
        case Op::ASL:
        case Op::LSL:
            e_.shift_cl(Shift::Shl, RDI, 64);
            e_.setcc(Cc::B, R10);
            e_.movzx(R10, R10, 8);
            if (in.op == Op::ASL) {
                // V: the bits shifted through the top were not all alike.
                e_.mov_rr(RDX, RDI, 64);
                e_.shift_cl(Shift::Sar, RDX, 64);
                e_.alu_rr(Alu::Cmp, RDX, R11, 64);
                e_.setcc(Cc::NE, R11);
                e_.movzx(R11, R11, 8);
            } else {
                e_.mov_ri(R11, 0, 32);
            }
            e_.shift_i(Shift::Shr, RDI, static_cast<u8>(64 - w), 64);
            break;
        case Op::LSR:
        case Op::ASR:
            e_.shift_cl(in.op == Op::LSR ? Shift::Shr : Shift::Sar, RDI, 64);
            e_.setcc(Cc::B, R10);
            e_.movzx(R10, R10, 8);
            e_.mov_ri(R11, 0, 32);
            e_.mov_rr(RDI, RDI, 32);
            if (w < 32) {
                e_.movzx(RDI, RDI, w);
            }
            break;
        default: {
            // ROL and ROR: C is the bit that came round last, if any did.
            e_.shift_cl(in.op == Op::ROL ? Shift::Rol : Shift::Ror, RDI, w);
            e_.mov_rr(R10, RDI, 32);
            if (in.op == Op::ROR) {
                e_.shift_i(Shift::Shr, R10, static_cast<u8>(w - 1), 32);
            }
            e_.alu_ri(Alu::And, R10, 1, 32);
            e_.alu_rr(Alu::Or, RCX, RCX, 32);
            e_.setcc(Cc::NE, RAX);
            e_.movzx(RAX, RAX, 8);
            e_.alu_rr(Alu::And, R10, RAX, 32);
            e_.mov_ri(R11, 0, 32);
            break;
        }
        }
        const bool rotate = in.op == Op::ROL || in.op == Op::ROR;
        if (rotate) {
            logic_shift(w, p_.ccr_shift);
        } else if (!by_register) {
            logic_shift(w, p_.ccr_shift_x);
        } else {
            // X stays as it was when the count is 0.
            e_.alu_rr(Alu::Or, RCX, RCX, 32);
            const usize zero = e_.jcc_rel32(Cc::E);
            logic_shift(w, p_.ccr_shift_x);
            const usize done = e_.jmp_rel32();
            e_.patch(zero);
            logic_shift(w, p_.ccr_shift);
            e_.patch(done);
        }
        write(in.dst, in.size, RDI, dst_pc_, true);
    }

    void logic_shift(unsigned w, usize routine) {
        e_.alu_rr(Alu::Or, RDI, RDI, w);
        e_.call_to(routine);
    }

    // BTST, BCHG, BCLR and BSET: the bit number is taken modulo 32 on a
    // register and modulo 8 on memory; Z is the inverse of the old bit.
    void bit(const Insn& in) {
        const u32 w = in.dst.mode == Mode::Dn ? 32 : 8;
        read(in.dst, in.size, RDI, dst_pc_);
        if (in.src.mode == Mode::Imm) {
            e_.mov_ri(RCX, in.src.value & (w - 1), 32);
        } else {
            e_.mov_load(RCX, RBX, dreg(in.src.reg), 32);
            e_.alu_ri(Alu::And, RCX, static_cast<i32>(w - 1), 32);
        }
        e_.mov_rr(RAX, RDI, 32);
        e_.shift_cl(Shift::Shr, RAX, 32);
        e_.alu_ri(Alu::And, RAX, 1, 32);
        e_.alu_ri(Alu::Xor, RAX, 1, 32);
        e_.shift_i(Shift::Shl, RAX, 2, 32);
        e_.mov_load(RDX, RBX, kOffSr, 16);
        e_.alu_ri(Alu::And, RDX, static_cast<i32>(~kCcrZ), 32);
        e_.alu_rr(Alu::Or, RDX, RAX, 32);
        e_.mov_store(RBX, kOffSr, RDX, 16);
        if (in.op == Op::BTST) {
            return;
        }
        e_.mov_ri(RAX, 1, 32);
        e_.shift_cl(Shift::Shl, RAX, 32);
        if (in.op == Op::BCHG) {
            e_.alu_rr(Alu::Xor, RDI, RAX, 32);
        } else if (in.op == Op::BSET) {
            e_.alu_rr(Alu::Or, RDI, RAX, 32);
        } else {
            e_.alu_ri(Alu::Xor, RAX, -1, 32);
            e_.alu_rr(Alu::And, RDI, RAX, 32);
        }
        write(in.dst, in.size, RDI, dst_pc_, true);
    }

    // MOVEM. The -(An) form takes its mask reversed (bit 0 is A7) and
    // stores A7 first, downwards; a stored An is its value before the
    // instruction. In the (An)+ form the final address wins over a value
    // loaded into An. Words load sign-extended into data registers too.
    void movem(const Insn& in) {
        const u32 n = bytes_of(in.size);
        if (in.dst.mode == Mode::Imm) {
            const u32 mask = in.dst.value;
            if (in.src.mode == Mode::AnPost) {
                e_.mov_load(R8, RBX, areg(in.src.reg), 32);
            } else {
                address(in.src, in.size, src_pc_);
            }
            for (u32 i = 0; i < 16; ++i) {
                if ((mask >> i & 1) == 0) {
                    continue;
                }
                read_mem(in.size);
                if (in.size == Size::Word) {
                    e_.movsx(RAX, RAX, 16);
                }
                e_.mov_store(RBX, any_reg(i), RAX, 32);
                add_r8(n);
            }
            if (in.src.mode == Mode::AnPost) {
                e_.mov_store(RBX, areg(in.src.reg), R8, 32);
            }
            return;
        }
        const u32 mask = in.src.value;
        if (in.dst.mode == Mode::AnPre) {
            e_.mov_load(R8, RBX, areg(in.dst.reg), 32);
            for (u32 i = 0; i < 16; ++i) {
                if ((mask >> i & 1) == 0) {
                    continue;
                }
                e_.alu_ri(Alu::Sub, R8, static_cast<i32>(n), 32);
                e_.mov_load(RAX, RBX, any_reg(15 - i), 32);
                write_mem(in.size);
            }
            e_.mov_store(RBX, areg(in.dst.reg), R8, 32);
            return;
        }
        address(in.dst, in.size, dst_pc_);
        for (u32 i = 0; i < 16; ++i) {
            if ((mask >> i & 1) == 0) {
                continue;
            }
            e_.mov_load(RAX, RBX, any_reg(i), 32);
            write_mem(in.size);
            add_r8(n);
        }
    }

    // LINK An,#d: push An, An = SP, SP += d. LINK A7 pushes the decremented
    // A7.
    void link(const Insn& in) {
        const u32 r = in.src.reg;
        e_.mov_load(R8, RBX, areg(7), 32);
        e_.alu_ri(Alu::Sub, R8, 4, 32);
        if (r == 7) {
            e_.mov_rr(RAX, R8, 32);
        } else {
            e_.mov_load(RAX, RBX, areg(r), 32);
        }
        write_mem(Size::Long);
        e_.mov_store(RBX, areg(r), R8, 32);
        e_.mov_rr(RAX, R8, 32);
        add_value(RAX, in.dst.value);
        e_.mov_store(RBX, areg(7), RAX, 32);
    }

    // UNLK An: SP = An, An = pop. UNLK A7 leaves A7 = the popped value.
    void unlink(const Insn& in) {
        const u32 r = in.dst.reg;
        e_.mov_load(R8, RBX, areg(r), 32);
        read_mem(Size::Long);
        e_.alu_ri(Alu::Add, R8, 4, 32);
        e_.mov_store(RBX, areg(7), R8, 32);
        e_.mov_store(RBX, areg(r), RAX, 32);
    }

    void add_value(Reg r, u32 value) {
        if (value != 0) {
            e_.alu_ri(Alu::Add, r, static_cast<i32>(value), 32);
        }
    }

    // EXG: swaps two whole registers and sets no flags.
    void exchange(const Insn& in) {
        const i32 a = in.src.mode == Mode::Dn ? dreg(in.src.reg) : areg(in.src.reg);
        const i32 b = in.dst.mode == Mode::Dn ? dreg(in.dst.reg) : areg(in.dst.reg);
        e_.mov_load(RSI, RBX, a, 32);
        e_.mov_load(RDI, RBX, b, 32);
        e_.mov_store(RBX, a, RDI, 32);
        e_.mov_store(RBX, b, RSI, 32);
    }

    bool branch(const Insn& in) {
        const u32 target = pc_ + 2 + in.dst.value;
        switch (in.op) {
        case Op::BRA:
            end_at(target);
            return true;
        case Op::BSR:
            push_imm(next_);
            end_at(target);
            return true;
        case Op::Bcc: {
            condition(in.cond);
            const usize not_taken = e_.jcc_rel32(Cc::E);
            end_at(target);
            e_.patch(not_taken);
            end_at(next_);
            return true;
        }
        default: {  // DBcc
            condition(in.cond);
            const usize holds = e_.jcc_rel32(Cc::NE);
            const i32 counter = dreg(in.src.reg);
            e_.mov_load(RSI, RBX, counter, 32);
            e_.alu_ri(Alu::Sub, RSI, 1, 16);
            e_.mov_store(RBX, counter, RSI, 16);
            e_.alu_ri(Alu::Cmp, RSI, -1, 16);
            const usize expired = e_.jcc_rel32(Cc::E);
            end_at(target);
            e_.patch(holds);
            e_.patch(expired);
            end_at(next_);
            return true;
        }
        }
    }

    void call_helper(u64 fn) {
        e_.mov_rr(RDI, RBX, 64);
        e_.call_abs(fn);
    }

    // After a helper that may end the run.
    void leave_if_stopped() {
        e_.alu_rr(Alu::Or, RAX, RAX, 32);
        e_.jcc_to(Cc::NE, p_.helper_exit);
    }

    // The privileged instructions end the run with S clear.
    void require_supervisor() {
        e_.mov_load(RCX, RBX, kOffSr, 16);
        e_.alu_ri(Alu::And, RCX, 0x2000, 32);
        const usize supervisor = e_.jcc_rel32(Cc::NE);
        stop(ExitReason::Privilege, pc_, 8);
        e_.patch(supervisor);
    }

    // ABCD, SBCD and NBCD. A memory destination's address survives the
    // helper call in R14.
    void decimal(const Insn& in) {
        if (in.op == Op::NBCD) {
            read(in.dst, Size::Byte, RSI, dst_pc_);
            e_.mov_ri(RDX, 0, 32);
        } else {
            read(in.src, Size::Byte, RSI, src_pc_);
            read(in.dst, Size::Byte, RDX, dst_pc_);
        }
        e_.mov_rr(R14, R8, 32);
        e_.mov_ri(RCX, in.op == Op::ABCD ? 0 : 1, 32);
        call_helper(helper_address(helper_bcd));
        e_.mov_rr(RDI, RAX, 32);
        e_.mov_rr(R8, R14, 32);
        write(in.dst, Size::Byte, RDI, dst_pc_, true);
    }

    void rotate_extend(const Insn& in) {
        read(in.dst, in.size, RSI, dst_pc_);
        if (in.src.mode == Mode::Dn) {
            e_.mov_load(RDX, RBX, dreg(in.src.reg), 32);
        } else {
            e_.mov_ri(RDX, in.src.mode == Mode::Imm ? in.src.value : 1, 32);
        }
        e_.mov_rr(R14, R8, 32);
        e_.mov_ri(RCX, bits_of(in.size) | (in.op == Op::ROXL ? 0x100u : 0u), 32);
        call_helper(helper_address(helper_rox));
        e_.mov_rr(RDI, RAX, 32);
        e_.mov_rr(R8, R14, 32);
        write(in.dst, in.size, RDI, dst_pc_, true);
    }

    void divide(const Insn& in) {
        read(in.src, Size::Word, RSI, src_pc_);
        e_.mov_ri(RDX, in.dst.reg | (in.op == Op::DIVS ? 0x100u : 0u), 32);
        e_.mov_ri(RCX, next_, 32);
        call_helper(helper_address(helper_divide));
        leave_if_stopped();
    }

    void check(const Insn& in) {
        read(in.src, Size::Word, RSI, src_pc_);
        e_.mov_ri(RDX, in.dst.reg, 32);
        e_.mov_ri(RCX, next_, 32);
        call_helper(helper_address(helper_chk));
        leave_if_stopped();
    }

    // MOVEP: every other byte from the address on, high byte first.
    void move_peripheral(const Insn& in) {
        const u32 n = bytes_of(in.size);
        if (in.dst.mode == Mode::Dn) {
            address(in.src, in.size, src_pc_);
            for (u32 i = 0; i < n; ++i) {
                read_mem(Size::Byte);
                if (i == 0) {
                    e_.mov_rr(R10, RAX, 32);
                } else {
                    e_.shift_i(Shift::Shl, R10, 8, 32);
                    e_.alu_rr(Alu::Or, R10, RAX, 32);
                }
                add_r8(2);
            }
            e_.mov_store(RBX, dreg(in.dst.reg), R10, bits_of(in.size));
            return;
        }
        address(in.dst, in.size, dst_pc_);
        e_.mov_load(R10, RBX, dreg(in.src.reg), 32);
        for (u32 i = 0; i < n; ++i) {
            e_.mov_rr(RAX, R10, 32);
            if (i + 1 < n) {
                e_.shift_i(Shift::Shr, RAX, static_cast<u8>(8 * (n - 1 - i)), 32);
            }
            write_mem(Size::Byte);
            add_r8(2);
        }
    }

    // ORI, ANDI and EORI to CCR and to SR. Only the bits a 68000 has in SR
    // can be set.
    void status_immediate(const Insn& in) {
        u32 value = in.src.value;
        Alu op = Alu::Or;
        switch (in.op) {
        case Op::ANDI_CCR: op = Alu::And; value = (value & 0x1F) | 0xFF00; break;
        case Op::EORI_CCR: op = Alu::Xor; value &= 0x1F; break;
        case Op::ORI_CCR: value &= 0x1F; break;
        case Op::ANDI_SR: op = Alu::And; value &= kSrBits; break;
        case Op::EORI_SR: op = Alu::Xor; value &= kSrBits; break;
        default: value &= kSrBits; break;
        }
        e_.mov_load(RDX, RBX, kOffSr, 16);
        e_.alu_ri(op, RDX, static_cast<i32>(value), 32);
        e_.mov_store(RBX, kOffSr, RDX, 16);
    }

    Emitter& e_;
    const detail::Prelude& p_;
    u32 pc_ = 0;
    u32 next_ = 0;
    u32 src_pc_ = 0;
    u32 dst_pc_ = 0;
    bool pc_loaded_ = false;
};

bool Translator::insn(const Insn& in, u32 pc) {
    pc_ = pc;
    next_ = pc + 2u * in.words;
    pc_loaded_ = false;
    // A PC-relative operand counts from its own extension word. Only the
    // MOVEM load puts another word (its mask) before the source's.
    src_pc_ = pc + 2 + (in.op == Op::MOVEM && in.dst.mode == Mode::Imm ? 2 : 0);
    dst_pc_ = pc + 2 + 2 * operand_words(in, in.src);

    switch (in.op) {
    case Op::MOVE:
        read(in.src, in.size, RSI, src_pc_);
        write(in.dst, in.size, RSI, dst_pc_, false);
        logic_flags(RSI, in.size);
        return false;
    case Op::MOVEA:
        read(in.src, in.size, RSI, src_pc_);
        if (in.size == Size::Word) {
            e_.movsx(RSI, RSI, 16);
        }
        e_.mov_store(RBX, areg(in.dst.reg), RSI, 32);
        return false;
    case Op::MOVEQ:
        e_.mov_ri(RSI, in.src.value, 32);
        e_.mov_store(RBX, dreg(in.dst.reg), RSI, 32);
        logic_flags(RSI, Size::Long);
        return false;
    case Op::LEA:
        address(in.src, Size::Long, src_pc_);
        e_.mov_store(RBX, areg(in.dst.reg), R8, 32);
        return false;
    case Op::PEA:
        address(in.src, Size::Long, src_pc_);
        e_.mov_rr(R10, R8, 32);
        push_reg(R10);
        return false;
    case Op::CLR:
        clear(in);
        return false;
    case Op::EXT:
        e_.mov_load(RSI, RBX, dreg(in.dst.reg), 32);
        if (in.size == Size::Word) {
            e_.movsx(RSI, RSI, 8);
        } else {
            e_.movsx(RSI, RSI, 16);
        }
        e_.mov_store(RBX, dreg(in.dst.reg), RSI, bits_of(in.size));
        logic_flags(RSI, in.size);
        return false;
    case Op::SWAP:
        e_.mov_load(RSI, RBX, dreg(in.dst.reg), 32);
        e_.shift_i(Shift::Rol, RSI, 16, 32);
        e_.mov_store(RBX, dreg(in.dst.reg), RSI, 32);
        logic_flags(RSI, Size::Long);
        return false;
    case Op::EXG:
        exchange(in);
        return false;
    case Op::NOP:
        return false;

    case Op::ADD:
    case Op::ADDI:
        binary(in, Alu::Add, p_.ccr_nzvcx);
        return false;
    case Op::SUB:
    case Op::SUBI:
        binary(in, Alu::Sub, p_.ccr_nzvcx);
        return false;
    case Op::ADDQ:
    case Op::SUBQ: {
        const Alu op = in.op == Op::ADDQ ? Alu::Add : Alu::Sub;
        if (in.dst.mode == Mode::An) {
            address_arith(in, op);
        } else {
            binary(in, op, p_.ccr_nzvcx);
        }
        return false;
    }
    case Op::ADDA:
        address_arith(in, Alu::Add);
        return false;
    case Op::SUBA:
        address_arith(in, Alu::Sub);
        return false;
    case Op::CMPA:
        address_arith(in, Alu::Cmp);
        return false;
    case Op::CMP:
    case Op::CMPI:
    case Op::CMPM:
        binary(in, Alu::Cmp, p_.ccr_nzvc);
        return false;
    case Op::ADDX:
        extended(in, Alu::Adc);
        return false;
    case Op::SUBX:
        extended(in, Alu::Sbb);
        return false;
    case Op::AND:
    case Op::ANDI:
        binary(in, Alu::And, p_.ccr_nzvc);
        return false;
    case Op::OR:
    case Op::ORI:
        binary(in, Alu::Or, p_.ccr_nzvc);
        return false;
    case Op::EOR:
    case Op::EORI:
        binary(in, Alu::Xor, p_.ccr_nzvc);
        return false;
    case Op::NEG:
        negate(in, false);
        return false;
    case Op::NEGX:
        negate(in, true);
        return false;
    case Op::NOT:
        read(in.dst, in.size, RDI, dst_pc_);
        e_.alu_ri(Alu::Xor, RDI, -1, bits_of(in.size));
        e_.call_to(p_.ccr_nzvc);
        write(in.dst, in.size, RDI, dst_pc_, true);
        return false;
    case Op::TST:
        read(in.dst, in.size, RDI, dst_pc_);
        logic_flags(RDI, in.size);
        return false;
    case Op::MULU:
        multiply(in, false);
        return false;
    case Op::MULS:
        multiply(in, true);
        return false;

    case Op::ASL:
    case Op::ASR:
    case Op::LSL:
    case Op::LSR:
    case Op::ROL:
    case Op::ROR:
        shift(in);
        return false;
    case Op::BTST:
    case Op::BCHG:
    case Op::BCLR:
    case Op::BSET:
        bit(in);
        return false;
    case Op::MOVEM:
        movem(in);
        return false;
    case Op::LINK:
        link(in);
        return false;
    case Op::UNLK:
        unlink(in);
        return false;
    case Op::Scc:
        condition(in.cond);
        e_.mov_ri(RSI, 0, 32);
        e_.alu_rr(Alu::Sub, RSI, RAX, 32);
        write(in.dst, Size::Byte, RSI, dst_pc_, false);
        return false;

    case Op::BRA:
    case Op::BSR:
    case Op::Bcc:
    case Op::DBcc:
        return branch(in);
    case Op::JMP:
        address(in.src, Size::Long, src_pc_);
        end_reg(R8);
        return true;
    case Op::JSR:
        address(in.src, Size::Long, src_pc_);
        e_.mov_rr(R10, R8, 32);
        push_imm(next_);
        end_reg(R10);
        return true;
    case Op::RTS:
        e_.mov_load(R8, RBX, areg(7), 32);
        read_mem(Size::Long);
        e_.alu_ri(Alu::Add, R8, 4, 32);
        e_.mov_store(RBX, areg(7), R8, 32);
        end_reg(RAX);
        return true;
    case Op::RTR:
        e_.mov_load(R8, RBX, areg(7), 32);
        read_mem(Size::Word);
        e_.alu_ri(Alu::And, RAX, 0x1F, 32);
        e_.mov_load(RDX, RBX, kOffSr, 16);
        e_.alu_ri(Alu::And, RDX, 0xFF00, 32);
        e_.alu_rr(Alu::Or, RDX, RAX, 32);
        e_.mov_store(RBX, kOffSr, RDX, 16);
        e_.alu_ri(Alu::Add, R8, 2, 32);
        read_mem(Size::Long);
        e_.alu_ri(Alu::Add, R8, 4, 32);
        e_.mov_store(RBX, areg(7), R8, 32);
        end_reg(RAX);
        return true;
    case Op::ALine:
        // Back to run(), which calls the native and goes on after the
        // opcode.
        e_.mov_ri(RAX, next_, 32);
        e_.mov_store(RBX, kOffPc, RAX, 32);
        e_.mov_ri(RAX, in.opcode, 32);
        e_.ret();
        return true;

    case Op::ABCD:
    case Op::SBCD:
    case Op::NBCD:
        decimal(in);
        return false;
    case Op::ROXL:
    case Op::ROXR:
        rotate_extend(in);
        return false;
    case Op::DIVU:
    case Op::DIVS:
        divide(in);
        return false;
    case Op::CHK:
        check(in);
        return false;
    case Op::TAS:
        read(in.dst, Size::Byte, RDI, dst_pc_);
        logic_flags(RDI, Size::Byte);
        e_.alu_ri(Alu::Or, RDI, 0x80, 32);
        write(in.dst, Size::Byte, RDI, dst_pc_, true);
        return false;
    case Op::MOVEP:
        move_peripheral(in);
        return false;
    case Op::MOVE_FROM_SR:
        // Not privileged on the 68000.
        e_.mov_load(RSI, RBX, kOffSr, 16);
        e_.movzx(RSI, RSI, 16);
        write(in.dst, Size::Word, RSI, dst_pc_, false);
        return false;
    case Op::MOVE_CCR:
        read(in.src, Size::Word, RSI, src_pc_);
        e_.alu_ri(Alu::And, RSI, 0x1F, 32);
        e_.mov_load(RDX, RBX, kOffSr, 16);
        e_.alu_ri(Alu::And, RDX, 0xFF00, 32);
        e_.alu_rr(Alu::Or, RDX, RSI, 32);
        e_.mov_store(RBX, kOffSr, RDX, 16);
        return false;
    case Op::MOVE_SR:
        require_supervisor();
        read(in.src, Size::Word, RSI, src_pc_);
        e_.alu_ri(Alu::And, RSI, static_cast<i32>(kSrBits), 32);
        e_.mov_store(RBX, kOffSr, RSI, 16);
        return false;
    case Op::ORI_CCR:
    case Op::ANDI_CCR:
    case Op::EORI_CCR:
        status_immediate(in);
        return false;
    case Op::ORI_SR:
    case Op::ANDI_SR:
    case Op::EORI_SR:
        require_supervisor();
        status_immediate(in);
        return false;
    case Op::MOVE_USP:
        require_supervisor();
        if (in.src.mode == Mode::An) {
            e_.mov_load(RAX, RBX, areg(in.src.reg), 32);
            e_.mov_store(RBX, kOffUsp, RAX, 32);
        } else {
            e_.mov_load(RAX, RBX, kOffUsp, 32);
            e_.mov_store(RBX, areg(in.dst.reg), RAX, 32);
        }
        return false;
    case Op::TRAP:
        return stop(ExitReason::Trap, next_, 32 + in.count);
    case Op::TRAPV: {
        e_.mov_load(RCX, RBX, kOffSr, 16);
        e_.alu_ri(Alu::And, RCX, 2, 32);
        const usize clear = e_.jcc_rel32(Cc::E);
        stop(ExitReason::Trap, next_, 7);
        e_.patch(clear);
        return false;
    }
    case Op::RESET:
    case Op::STOP:
    case Op::RTE:
        // Without exceptions delivered to the guest these mean nothing.
        require_supervisor();
        return stop(ExitReason::Unimplemented, pc_, 0);
    case Op::FLine:
        return stop(ExitReason::Illegal, pc_, 11);
    default:  // Op::Illegal: ILLEGAL and every 68010 and later opcode
        return stop(ExitReason::Illegal, pc_, 4);
    }
}

using Enter = u32 (*)(CpuState* cpu, u8* base, u64 size, const u8* block);

u32 slot_of(u32 pc, u32 bits) {
    return ((pc >> 1) * 0x9E3779B1u) >> (32 - bits);
}

// The exit for an instruction that cannot be fetched: an odd pc, or the
// first of its words that lies outside memory.
void fetch_fault(CpuState& cpu, const GuestMemory& mem, u32 pc, u32 mask) {
    if ((pc & 1) != 0) {
        cpu.exit = ExitReason::AddressError;
        cpu.trap_number = 3;
        cpu.fault_addr = pc;
        return;
    }
    u32 at = pc;
    for (u32 n = 0; n < 5 && mem.range_ok(at & mask, 2); ++n) {
        at += 2;
    }
    cpu.exit = ExitReason::BusError;
    cpu.trap_number = 2;
    cpu.fault_addr = at;
}

#if defined(HOMBRE_M68K_TRACE)
// Logs a translated block: its guest pc and its first four opcode words, as
// far as guest memory holds them.
void trace_block(const GuestMemory& mem, u32 pc, u32 mask) {
    LogLine line;
    line.text("m68k: block ").hex(pc).text(":");
    for (u32 n = 0; n < 4; ++n) {
        u32 word = 0;
        if (!mem.read16((pc + 2 * n) & mask, word)) {
            break;
        }
        line.text(" ").hex(word);
    }
    line.send();
}
#endif

}

bool Jit::init(const CodeRegion& region, Machine& machine) {
    if (region.write == nullptr || region.exec == nullptr || region.bytes > kMaxRegionBytes) {
        return false;
    }
    GuestMemory& mem = machine.mem;
    if (mem.base == nullptr || !mem.range_ok(kReturnStub, 4)) {
        return false;
    }
    Emitter e(region.write, region.bytes);
    detail::Prelude prelude{};
    emit_prelude(e, prelude, kNoAddressMask);
    if (!e.ok() || region.bytes < prelude.end + kMinBlockBytes) {
        return false;
    }
    decode_init();
    machine_ = &machine;
    machine.jit = this;
    region_ = region;
    prelude_ = prelude;
    mem.write16(kHaltStub, 0xA000);
    mem.write16(kHaltStub + 2, 0x4E75);
    mem.write16(kReturnStub, 0xA001);
    mem.write16(kReturnStub + 2, 0x4E75);
    for (NativeSlot& slot : natives_) {
        slot = NativeSlot{nullptr, nullptr, nullptr};
    }
    next_native_ = 2;
    block_limit_ = 64;
    address_mask_ = kNoAddressMask;
    budget_left_ = 0;
    run_depth_ = 0;
    preempt_hook_ = nullptr;
    preempt_ctx_ = nullptr;
    native_ = nullptr;
    stop_ = false;
    attention_ = false;
    attention_hook_ = nullptr;
    attention_ctx_ = nullptr;
    flush();
    return true;
}

u16 Jit::add_native(NativeFn fn, void* ctx, const char* name) {
    if (fn == nullptr || next_native_ >= kNatives) {
        return 0;
    }
    natives_[next_native_] = NativeSlot{fn, ctx, name};
    return static_cast<u16>(next_native_++);
}

void Jit::flush() {
    for (Block& b : cache_) {
        b = Block{0, 0};
    }
    blocks_ = 0;
    used_ = prelude_.end;
}

void Jit::set_block_limit(u32 instructions) {
    block_limit_ = instructions == 0 ? 1 : instructions;
    flush();
}

// The mask costs at most one instruction in each of the six access
// routines, far less than the room init keeps beyond the prelude, so the
// prelude written again always fits.
void Jit::set_address_mask(u32 mask) {
    if (machine_ == nullptr) {
        return;  // before init there is no prelude to rewrite
    }
    Emitter e(region_.write, region_.bytes);
    emit_prelude(e, prelude_, mask);
    address_mask_ = mask;
    flush();
}

const u8* Jit::lookup(u32 pc) const {
    for (u32 i = slot_of(pc, kSlotBits), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), ++n) {
        if (cache_[i].offset == 0) {
            return nullptr;
        }
        if (cache_[i].pc == pc) {
            return region_.exec + cache_[i].offset;
        }
    }
    return nullptr;
}

const u8* Jit::translate(u32 pc) {
    CpuState& cpu = machine_->cpu;
    const GuestMemory& mem = machine_->mem;
    if (blocks_ >= kSlots - kSlots / 4) {
        flush();
    }
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        const usize begin = used_;
        Emitter e(region_.write, region_.bytes - kTailBytes, begin);
        Translator t(e, prelude_);
        u32 at = pc;
        u32 count = 0;
        usize good = begin;
        bool ended = false;
        while (count < block_limit_) {
            Insn in;
            if (!decode(mem, at & address_mask_, in)) {
                if (count == 0) {
                    fetch_fault(cpu, mem, at, address_mask_);
                    return nullptr;
                }
                break;  // the next block reports it
            }
            const bool ends = t.insn(in, at);
            if (!e.ok()) {
                break;  // the region is full: the block ends before it
            }
            good = e.size();
            ++count;
            if (ends) {
                ended = true;
                break;
            }
            at += 2u * in.words;
        }
        if (count == 0) {
            if (attempt == 0 && used_ != prelude_.end) {
                flush();
                continue;
            }
            cpu.exit = ExitReason::Unimplemented;  // one instruction is larger than the region
            return nullptr;
        }
        usize end = good;
        if (!ended) {
            Emitter tail(region_.write, region_.bytes, good);
            Translator(tail, prelude_).end_at(at);
            end = tail.size();
        }
        used_ = end;
        u32 i = slot_of(pc, kSlotBits);
        while (cache_[i].offset != 0) {
            i = (i + 1) & (kSlots - 1);
        }
        cache_[i] = Block{pc, static_cast<u32>(begin)};
        ++blocks_;
#if defined(HOMBRE_M68K_TRACE)
        trace_block(mem, pc, address_mask_);
#endif
        return region_.exec + begin;
    }
    return nullptr;
}

void Jit::run_native(const NativeSlot& slot) {
    const char* const outer = native_;
    native_ = slot.name;
    slot.fn(slot.ctx, *machine_);
    native_ = outer;
}

bool Jit::call_native(u16 id) {
    if (machine_ == nullptr || id < 2 || id >= kNatives || natives_[id].fn == nullptr) {
        return false;
    }
    const NativeSlot slot = natives_[id];
#ifdef HOMBRE_TRACE_NATIVES
    CpuState& cpu = machine_->cpu;
    LogLine()
        .text("call ")
        .text(slot.name != nullptr ? slot.name : "?")
        .text(" d1=")
        .hex(cpu.d[1])
        .text(" d2=")
        .hex(cpu.d[2])
        .text(" d3=")
        .hex(cpu.d[3])
        .text(" a0=")
        .hex(cpu.a[0])
        .text(" a1=")
        .hex(cpu.a[1])
        .send();
#endif
    run_native(slot);
#ifdef HOMBRE_TRACE_NATIVES
    LogLine().text("  -> d0=").hex(cpu.d[0]).send();
#endif
    return true;
}

void Jit::dispatch(u32 opcode) {
    CpuState& cpu = machine_->cpu;
    const u32 id = opcode & 0x0FFF;
    const u32 resume = cpu.pc;
    if (id == 0 || id == 1) {
        cpu.pc = resume - 2;
        cpu.exit = id == 0 ? ExitReason::Halt : ExitReason::Returned;
        return;
    }
    const NativeSlot slot = natives_[id];
    if (slot.fn == nullptr) {
        LogLine().text("native: unknown A-line id ").dec(id).send();
        cpu.pc = resume - 2;
        cpu.exit = ExitReason::Unimplemented;
        cpu.trap_number = 10;
        return;
    }
#ifdef HOMBRE_TRACE_NATIVES
    // Built with -DHOMBRE_TRACE_NATIVES: every native call is logged with
    // its argument registers and its result.
    LogLine()
        .text("call ")
        .text(slot.name != nullptr ? slot.name : "?")
        .text(" d1=")
        .hex(cpu.d[1])
        .text(" d2=")
        .hex(cpu.d[2])
        .text(" d3=")
        .hex(cpu.d[3])
        .text(" a0=")
        .hex(cpu.a[0])
        .text(" a1=")
        .hex(cpu.a[1])
        .send();
#endif
    run_native(slot);
#ifdef HOMBRE_TRACE_NATIVES
    LogLine().text("  -> d0=").hex(cpu.d[0]).send();
#endif
    if (cpu.exit == ExitReason::None) {
        cpu.pc = resume;
    }
}

ExitReason Jit::run(u64 max_blocks) {
    if (machine_ == nullptr) {
        return ExitReason::Unimplemented;  // not initialised
    }
    CpuState& cpu = machine_->cpu;
    const Enter enter =
        reinterpret_cast<Enter>(reinterpret_cast<usize>(region_.exec + prelude_.enter));
    if (run_depth_ == 0) {
        budget_left_ = max_blocks;
    }
    ++run_depth_;
    cpu.exit = ExitReason::None;
    u64 count = 0;
    while (cpu.exit == ExitReason::None) {
        if (attention_) {
            attention_ = false;
            if (attention_hook_ != nullptr) {
                attention_hook_(attention_ctx_);
            }
        }
        if (stop_) {
            cpu.exit = ExitReason::Stopped;
        }
        if (cpu.exit != ExitReason::None) {
            break;
        }
        if (count == max_blocks || budget_left_ == 0) {
            if (run_depth_ > 1) {
                LogLine().text("m68k: block budget used up in a call from a native").send();
            }
            cpu.exit = ExitReason::Budget;
            break;
        }
        if (run_depth_ == 1 && preempt_hook_ != nullptr) {
            preempt_hook_(preempt_ctx_);
            if (cpu.exit != ExitReason::None) {
                break;
            }
        }
        const u8* code = lookup(cpu.pc);
        if (code == nullptr) {
            code = translate(cpu.pc);
            if (code == nullptr) {
                break;
            }
        }
        ++count;
        --budget_left_;
        const u32 aline = enter(&cpu, machine_->mem.base, machine_->mem.size, code);
        if (aline != 0 && cpu.exit == ExitReason::None) {
            dispatch(aline);
        }
    }
    if (--run_depth_ == 0) {
        stop_ = false;
    }
    return cpu.exit;
}

ExitReason Jit::call_guest(u32 entry, u64 max_blocks) {
    if (machine_ == nullptr) {
        return ExitReason::Unimplemented;  // not initialised
    }
    CpuState& cpu = machine_->cpu;
    const u32 caller_pc = cpu.pc;
    const u16 caller_sr = cpu.sr;
    const u32 sp = cpu.a[7] - 4;
    if (!machine_->mem.write32(sp & address_mask_, kReturnStub)) {
        cpu.exit = (sp & 1) != 0 ? ExitReason::AddressError : ExitReason::BusError;
        cpu.trap_number = (sp & 1) != 0 ? 3 : 2;
        cpu.fault_addr = sp;
        return cpu.exit;
    }
    cpu.a[7] = sp;
    cpu.pc = entry;
    const ExitReason why = run(max_blocks);
    if (why == ExitReason::Returned) {
        cpu.exit = ExitReason::None;
        cpu.pc = caller_pc;
        // The called code may run between two instructions of the caller,
        // as an input event's hook does: the flags are the caller's.
        cpu.sr = caller_sr;
    }
    return why;
}

}
