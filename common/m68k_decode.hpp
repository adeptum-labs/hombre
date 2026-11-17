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

#include "m68k_cpu.hpp"
#include "types.hpp"

// Decoder for the 68000 instruction set, one instruction at a time.
//
// The opcode word alone fixes the operation, the sizes, every register
// number, every addressing mode and the number of extension words, so the
// 65536 opcode words are classified once into a table and decode() only
// reads the extension words that follow. 68010 and later opcodes, FPU and
// MMU opcodes decode as Op::Illegal; the A-line and F-line opcode groups
// decode as Op::ALine and Op::FLine.
//
// The table is a plain array in static storage, filled by decode_init().
// There is no function-local static and so no initialisation guard. Call
// decode_init() once before the first decode(); decode() also calls it if
// nobody has, which is fine from one thread but not safe to race.
//
// decode() is total: it reads only inside the memory it is given, reads at
// most five words, and has no unbounded loops.
//
// How an Insn is laid out
// -----------------------
// op, size   The operation and the size it works on. Size::None for
//            instructions without a size. For ADDA, SUBA, CMPA and MOVEA the
//            size is that of the source (Word or Long). MULU, MULS, DIVU,
//            DIVS and CHK are Word, LEA and PEA are Long, SWAP is Word, EXT
//            is Word or Long, Scc, TAS, NBCD, ABCD and SBCD are Byte. MOVEQ
//            and EXG are Long. Bcc, BRA and BSR are Byte for the 8-bit
//            displacement and Word for the 16-bit one. BTST, BCHG, BCLR and
//            BSET are Long on a data register and Byte on memory. Shifts
//            and rotates of memory are Word. LINK, STOP and the moves to and
//            from SR and CCR are Word, except ORI/ANDI/EORI to CCR (Byte).
//            MOVE_USP is Long. UNLK, TRAP, RESET, NOP, RTE, RTR, RTS, TRAPV,
//            JMP, JSR and the Illegal, ALine and FLine entries are None.
// opcode     The first word as fetched.
// words      The length in 16-bit words, opcode included: 1 to 5. Illegal,
//            ALine and FLine are always 1.
// cond       The condition field (bits 11-8) of Bcc, DBcc and Scc, 0 to 15
//            as in the opcode (0 true, 1 false, 2 HI ... 15 LE). BRA is 0
//            and BSR is 1 by the same rule. 0 for everything else.
// count      The data that sits inside the opcode word: 1 to 8 for ADDQ,
//            SUBQ and the immediate-count shifts and rotates (a count field
//            of 0 means 8), the vector 0 to 15 for TRAP, and 1 for a shift
//            or rotate of memory, which moves by one bit. 0 for everything
//            else, including register-count shifts.
//
// Operands. src and dst are given in assembly order, <src>,<dst>. An
// instruction with a single operand keeps it in dst, except JMP, JSR and
// PEA (the operand is only an address) and MOVE_SR / MOVE_CCR (the operand
// is read), which keep it in src. An unused operand has Mode::None and
// zeroes in reg, value and ext.
//
// Ea::reg    The register number 0 to 7 for Dn, An, AnInd, AnPost, AnPre,
//            AnDisp and AnIndex. 0 for every other mode.
// Ea::value  By mode:
//            Dn, An, AnInd, AnPost, AnPre  not used (0)
//            AnDisp, PcDisp   the 16-bit displacement, sign-extended
//            AnIndex, PcIndex the 8-bit displacement of the index word,
//                             sign-extended
//            AbsW             the 16-bit address, sign-extended
//            AbsL             the 32-bit address
//            Imm              the immediate or the field the operand stands
//                             for, see below
//            A PC-relative displacement (PcDisp, PcIndex, and the branch
//            forms below) counts from the address of the extension word
//            that holds it. For a branch, DBcc, or a PC-relative operand
//            right after the opcode that is the instruction address + 2;
//            after another extension word (MOVEM's mask) it is that much
//            further on. The 8-bit displacement inside a short branch
//            counts from instruction address + 2 as well.
// Ea::ext    For AnIndex and PcIndex the index word as fetched: bit 15
//            selects An (1) or Dn (0), bits 14-12 the index register, bit
//            11 a long (1) or sign-extended word (0) index, bits 7-0 the
//            displacement already in value. Bits 10-9 (scale) and 8 (full
//            format) are 68020 fields and are left as fetched; the 68000
//            ignores them. 0 for every other mode.
//
// Mode::Imm operands are used for more than immediate data:
//  - An immediate operand of an arithmetic or move instruction (MOVE, ADDI,
//    SUBI, CMPI, ORI, ANDI, EORI, ADD ... #, a source #<data>): value is the
//    data zero-extended to the instruction's size, so MOVE.B #-1 has value
//    0xFF; the translator sign-extends where the operation needs it.
//  - MOVEQ: src.value is the 8-bit data sign-extended to 32 bits.
//  - ADDQ, SUBQ, and the shifts and rotates with an immediate count: src
//    is Imm with value 1 to 8, equal to count.
//  - ORI/ANDI/EORI to CCR: src.value is the low byte of the extension
//    word. To SR and for STOP: src.value is the 16-bit word.
//  - BTST, BCHG, BCLR, BSET with a bit number: src.value is the low byte
//    of the extension word.
//  - MOVEM: the register mask word. Reg to memory: src is the mask and dst
//    the address. Memory to reg: src is the address and dst is the mask.
//    The mask word comes before the address's own extension words in the
//    stream. Bit 0 is D0 and bit 15 is A7, except for the -(An) form where
//    bit 0 is A7 and bit 15 is D0.
//  - LINK: src is An, dst is Imm with the 16-bit displacement sign-extended.
//
// Operand pairs by operation:
//  - Branches: Bcc, BRA and BSR have only dst, Mode::PcDisp, value the
//    sign-extended displacement; the target is instruction address + 2 +
//    value. A displacement byte of 0 in the opcode means a 16-bit
//    displacement follows (words 2, size Word); 0xFF is simply -1 on a
//    68000. DBcc has src Dn (the counter) and dst PcDisp (16-bit
//    displacement, same target rule).
//  - Shifts and rotates: dst is Dn, src is Imm 1 to 8 (count the same) or
//    Dn holding the count (count 0). Shifts of memory have only dst and
//    count 1.
//  - MOVE_USP: either src An (A -> USP) with dst None, or src None with
//    dst An (USP -> A).
//  - MOVEP: reg to memory has src Dn, dst AnDisp; memory to reg has src
//    AnDisp, dst Dn.
//  - EXG: src and dst are the two registers (Dn or An), src the one in
//    bits 11-9 and dst the one in bits 2-0.
//  - CMPM: both operands AnPost. ABCD, SBCD, ADDX, SUBX: both Dn or both
//    AnPre, src the register in bits 2-0 and dst the one in bits 11-9.
//  - MOVE_FROM_SR has dst only; MOVE_SR and MOVE_CCR have src only.
//  - SWAP, EXT, UNLK: dst only.

namespace hombre::m68k {

enum class Op : u8 { Illegal, ABCD, ADD, ADDA, ADDI, ADDQ, ADDX, AND, ANDI, ANDI_CCR, ANDI_SR,
  ASL, ASR, Bcc, BCHG, BCLR, BRA, BSET, BSR, BTST, CHK, CLR, CMP, CMPA, CMPI, CMPM, DBcc, DIVS,
  DIVU, EOR, EORI, EORI_CCR, EORI_SR, EXG, EXT, JMP, JSR, LEA, LINK, LSL, LSR, MOVE, MOVEA,
  MOVE_CCR, MOVE_FROM_SR, MOVE_SR, MOVE_USP, MOVEM, MOVEP, MOVEQ, MULS, MULU, NBCD, NEG, NEGX,
  NOP, NOT, OR, ORI, ORI_CCR, ORI_SR, PEA, RESET, ROL, ROR, ROXL, ROXR, RTE, RTR, RTS, SBCD,
  Scc, STOP, SUB, SUBA, SUBI, SUBQ, SUBX, SWAP, TAS, TRAP, TRAPV, TST, UNLK, ALine, FLine };

enum class Size : u8 { None, Byte, Word, Long };

enum class Mode : u8 { Dn, An, AnInd, AnPost, AnPre, AnDisp, AnIndex, AbsW, AbsL, PcDisp, PcIndex,
  Imm, None };

struct Ea {
    Mode mode;
    u8 reg;
    u32 value;  // displacement, address or immediate, see above
    u16 ext;    // index word of AnIndex and PcIndex
};

struct Insn {
    Op op;
    Size size;
    Ea src;
    Ea dst;
    u8 cond;
    u8 count;
    u16 opcode;
    u8 words;
};

// Fills the opcode table. Runs once; later calls return at once.
inline void decode_init();

// Decodes the instruction at pc. Returns false, with out set to an Illegal
// instruction of 0 words, if pc is odd or any word of the instruction lies
// outside memory. Otherwise returns true, and an opcode that is no 68000
// instruction yields Op::Illegal, Op::ALine or Op::FLine with words 1.
inline bool decode(const GuestMemory& mem, u32 pc, Insn& out);

namespace detail {

// How one operand's value is found. Ea reads whatever its mode needs.
enum class Ext : u8 { None, Opc, Ea, Word, WordS, Byte };

// What the opcode word says about the instruction, before the extension
// words are read.
struct Spec {
    Op op = Op::Illegal;
    Size size = Size::None;
    Mode src_mode = Mode::None;
    Mode dst_mode = Mode::None;
    u8 src_reg = 0;
    u8 dst_reg = 0;
    Ext src_ext = Ext::None;
    Ext dst_ext = Ext::None;
    u8 cond = 0;
    u8 imm = 0;             // data held in the opcode word, used by Ext::Opc
    bool dst_first = false;  // the destination's words precede the source's
    bool imm_count = false;  // imm is also reported as Insn::count
};

// A Spec in eight bytes.
struct Entry {
    u8 op;
    u8 size_flags;  // Size in bits 0-1, dst_first in bit 2, imm_count in bit 3
    u8 src;         // mode << 3 | reg
    u8 dst;
    u8 ext;         // source Ext in the low nibble, destination Ext in the high
    u8 cond;
    u8 imm;
    u8 words;
};

constexpr u32 mode_bit(Mode m) {
    return 1u << static_cast<u32>(m);
}

constexpr u32 kDn = mode_bit(Mode::Dn);
constexpr u32 kAn = mode_bit(Mode::An);
constexpr u32 kAll = (1u << 12) - 1;
constexpr u32 kData = kAll & ~kAn;
constexpr u32 kDataAlt = kDn | mode_bit(Mode::AnInd) | mode_bit(Mode::AnPost) |
                         mode_bit(Mode::AnPre) | mode_bit(Mode::AnDisp) | mode_bit(Mode::AnIndex) |
                         mode_bit(Mode::AbsW) | mode_bit(Mode::AbsL);
constexpr u32 kMemAlt = kDataAlt & ~kDn;
constexpr u32 kControl = mode_bit(Mode::AnInd) | mode_bit(Mode::AnDisp) |
                         mode_bit(Mode::AnIndex) | mode_bit(Mode::AbsW) | mode_bit(Mode::AbsL) |
                         mode_bit(Mode::PcDisp) | mode_bit(Mode::PcIndex);
constexpr u32 kMovemStore = (kControl & kDataAlt) | mode_bit(Mode::AnPre);
constexpr u32 kMovemLoad = kControl | mode_bit(Mode::AnPost);

// The 6-bit mode and register field of an opcode, or false if reserved.
inline bool ea_mode(u32 field, Mode& mode) {
    u32 m = (field >> 3) & 7;
    u32 r = field & 7;
    switch (m) {
    case 0: mode = Mode::Dn; return true;
    case 1: mode = Mode::An; return true;
    case 2: mode = Mode::AnInd; return true;
    case 3: mode = Mode::AnPost; return true;
    case 4: mode = Mode::AnPre; return true;
    case 5: mode = Mode::AnDisp; return true;
    case 6: mode = Mode::AnIndex; return true;
    default: break;
    }
    switch (r) {
    case 0: mode = Mode::AbsW; return true;
    case 1: mode = Mode::AbsL; return true;
    case 2: mode = Mode::PcDisp; return true;
    case 3: mode = Mode::PcIndex; return true;
    case 4: mode = Mode::Imm; return true;
    default: return false;
    }
}

inline bool uses_register(Mode m) {
    return static_cast<u32>(m) <= static_cast<u32>(Mode::AnIndex);
}

inline bool take_src(Spec& s, u32 field, u32 allowed) {
    Mode m = Mode::None;
    if (!ea_mode(field, m) || (allowed & mode_bit(m)) == 0) {
        return false;
    }
    s.src_mode = m;
    s.src_reg = uses_register(m) ? static_cast<u8>(field & 7) : 0;
    s.src_ext = Ext::Ea;
    return true;
}

inline bool take_dst(Spec& s, u32 field, u32 allowed) {
    Mode m = Mode::None;
    if (!ea_mode(field, m) || (allowed & mode_bit(m)) == 0) {
        return false;
    }
    s.dst_mode = m;
    s.dst_reg = uses_register(m) ? static_cast<u8>(field & 7) : 0;
    s.dst_ext = Ext::Ea;
    return true;
}

inline void set_src(Spec& s, Mode m, u32 reg, Ext e = Ext::None) {
    s.src_mode = m;
    s.src_reg = static_cast<u8>(reg);
    s.src_ext = e;
}

inline void set_dst(Spec& s, Mode m, u32 reg, Ext e = Ext::None) {
    s.dst_mode = m;
    s.dst_reg = static_cast<u8>(reg);
    s.dst_ext = e;
}

// The size field of bits 7-6 of the arithmetic, logic and shift groups.
inline bool size_field(u32 bits, Size& size) {
    switch (bits) {
    case 0: size = Size::Byte; return true;
    case 1: size = Size::Word; return true;
    case 2: size = Size::Long; return true;
    default: return false;
    }
}

inline Spec simple(Op op) {
    Spec s;
    s.op = op;
    return s;
}

inline Spec classify_bits(u32 w) {
    Spec s;
    if ((w & 0x0100) != 0) {
        u32 opmode = (w >> 6) & 7;
        if (((w >> 3) & 7) == 1) {  // MOVEP
            if (opmode < 4) {
                return Spec{};
            }
            s.op = Op::MOVEP;
            s.size = (opmode & 1) != 0 ? Size::Long : Size::Word;
            if (opmode < 6) {
                set_src(s, Mode::AnDisp, w & 7, Ext::Ea);
                set_dst(s, Mode::Dn, (w >> 9) & 7);
            } else {
                set_src(s, Mode::Dn, (w >> 9) & 7);
                set_dst(s, Mode::AnDisp, w & 7, Ext::Ea);
            }
            return s;
        }
        static const Op kOps[4] = {Op::BTST, Op::BCHG, Op::BCLR, Op::BSET};
        u32 type = (w >> 6) & 3;
        s.op = kOps[type];
        set_src(s, Mode::Dn, (w >> 9) & 7);
        if (!take_dst(s, w & 0x3F, type == 0 ? kData : kDataAlt)) {
            return Spec{};
        }
        s.size = s.dst_mode == Mode::Dn ? Size::Long : Size::Byte;
        return s;
    }

    u32 group = (w >> 9) & 7;
    if (group == 7) {
        return Spec{};
    }
    if (group == 4) {  // static bit operations
        static const Op kOps[4] = {Op::BTST, Op::BCHG, Op::BCLR, Op::BSET};
        u32 type = (w >> 6) & 3;
        s.op = kOps[type];
        set_src(s, Mode::Imm, 0, Ext::Byte);
        if (!take_dst(s, w & 0x3F, type == 0 ? (kData & ~mode_bit(Mode::Imm)) : kDataAlt)) {
            return Spec{};
        }
        s.size = s.dst_mode == Mode::Dn ? Size::Long : Size::Byte;
        return s;
    }

    static const Op kOps[8] = {Op::ORI, Op::ANDI, Op::SUBI, Op::ADDI, Op::Illegal, Op::EORI,
                               Op::CMPI, Op::Illegal};
    Size size = Size::None;
    if (!size_field((w >> 6) & 3, size)) {
        return Spec{};
    }
    Op op = kOps[group];
    if ((w & 0x3F) == 0x3C && (group == 0 || group == 1 || group == 5)) {
        if (size == Size::Long) {
            return Spec{};
        }
        static const Op kCcr[8] = {Op::ORI_CCR, Op::ANDI_CCR, Op::Illegal, Op::Illegal,
                                   Op::Illegal, Op::EORI_CCR, Op::Illegal, Op::Illegal};
        static const Op kSr[8] = {Op::ORI_SR, Op::ANDI_SR, Op::Illegal, Op::Illegal,
                                  Op::Illegal, Op::EORI_SR, Op::Illegal, Op::Illegal};
        s.size = size;
        if (size == Size::Byte) {
            s.op = kCcr[group];
            set_src(s, Mode::Imm, 0, Ext::Byte);
        } else {
            s.op = kSr[group];
            set_src(s, Mode::Imm, 0, Ext::Word);
        }
        return s;
    }
    s.op = op;
    s.size = size;
    set_src(s, Mode::Imm, 0, Ext::Ea);
    if (!take_dst(s, w & 0x3F, kDataAlt)) {
        return Spec{};
    }
    return s;
}

inline Spec classify_move(u32 w) {
    Spec s;
    u32 line = w >> 12;
    s.size = line == 1 ? Size::Byte : (line == 3 ? Size::Word : Size::Long);
    if (!take_src(s, w & 0x3F, s.size == Size::Byte ? kData : kAll)) {
        return Spec{};
    }
    u32 dst_mode = (w >> 6) & 7;
    u32 dst_reg = (w >> 9) & 7;
    if (dst_mode == 1) {
        if (s.size == Size::Byte) {
            return Spec{};
        }
        s.op = Op::MOVEA;
        set_dst(s, Mode::An, dst_reg);
        return s;
    }
    s.op = Op::MOVE;
    if (!take_dst(s, (dst_mode << 3) | dst_reg, kDataAlt)) {
        return Spec{};
    }
    return s;
}

inline Spec classify_misc(u32 w) {
    Spec s;
    u32 ea = w & 0x3F;
    if ((w & 0x0100) != 0) {
        u32 opmode = (w >> 6) & 7;
        if (opmode == 7) {
            s.op = Op::LEA;
            s.size = Size::Long;
            if (!take_src(s, ea, kControl)) {
                return Spec{};
            }
            set_dst(s, Mode::An, (w >> 9) & 7);
            return s;
        }
        if (opmode == 6) {
            s.op = Op::CHK;
            s.size = Size::Word;
            if (!take_src(s, ea, kData)) {
                return Spec{};
            }
            set_dst(s, Mode::Dn, (w >> 9) & 7);
            return s;
        }
        return Spec{};
    }

    u32 size_bits = (w >> 6) & 3;
    switch ((w >> 9) & 7) {
    case 0:
    case 1:
    case 2:
    case 3: {
        u32 which = (w >> 9) & 3;
        if (size_bits == 3) {
            // MOVE from SR, then (68010+) from CCR, to CCR, to SR.
            if (which == 1) {
                return Spec{};
            }
            s.size = Size::Word;
            if (which == 0) {
                s.op = Op::MOVE_FROM_SR;
                return take_dst(s, ea, kDataAlt) ? s : Spec{};
            }
            s.op = which == 2 ? Op::MOVE_CCR : Op::MOVE_SR;
            return take_src(s, ea, kData) ? s : Spec{};
        }
        static const Op kOps[4] = {Op::NEGX, Op::CLR, Op::NEG, Op::NOT};
        s.op = kOps[which];
        size_field(size_bits, s.size);
        return take_dst(s, ea, kDataAlt) ? s : Spec{};
    }
    case 4:
        switch (size_bits) {
        case 0:
            s.op = Op::NBCD;
            s.size = Size::Byte;
            return take_dst(s, ea, kDataAlt) ? s : Spec{};
        case 1:
            if ((ea >> 3) == 0) {
                s.op = Op::SWAP;
                s.size = Size::Word;
                set_dst(s, Mode::Dn, w & 7);
                return s;
            }
            s.op = Op::PEA;
            s.size = Size::Long;
            return take_src(s, ea, kControl) ? s : Spec{};
        default:
            s.size = size_bits == 2 ? Size::Word : Size::Long;
            if ((ea >> 3) == 0) {
                s.op = Op::EXT;
                set_dst(s, Mode::Dn, w & 7);
                return s;
            }
            s.op = Op::MOVEM;
            set_src(s, Mode::Imm, 0, Ext::Word);
            return take_dst(s, ea, kMovemStore) ? s : Spec{};
        }
    case 5:
        if (size_bits == 3) {
            s.op = Op::TAS;
            s.size = Size::Byte;
        } else {
            s.op = Op::TST;
            size_field(size_bits, s.size);
        }
        return take_dst(s, ea, kDataAlt) ? s : Spec{};
    case 6:
        if (size_bits < 2) {
            return Spec{};
        }
        s.op = Op::MOVEM;
        s.size = size_bits == 2 ? Size::Word : Size::Long;
        s.dst_first = true;
        set_dst(s, Mode::Imm, 0, Ext::Word);
        return take_src(s, ea, kMovemLoad) ? s : Spec{};
    default:
        break;
    }

    // 0x4Exx
    if (w >= 0x4E40 && w <= 0x4E4F) {
        s.op = Op::TRAP;
        s.imm = static_cast<u8>(w & 15);
        s.imm_count = true;
        return s;
    }
    if (w >= 0x4E50 && w <= 0x4E57) {
        s.op = Op::LINK;
        s.size = Size::Word;
        set_src(s, Mode::An, w & 7);
        set_dst(s, Mode::Imm, 0, Ext::WordS);
        return s;
    }
    if (w >= 0x4E58 && w <= 0x4E5F) {
        s.op = Op::UNLK;
        set_dst(s, Mode::An, w & 7);
        return s;
    }
    if (w >= 0x4E60 && w <= 0x4E6F) {
        s.op = Op::MOVE_USP;
        s.size = Size::Long;
        if ((w & 8) == 0) {
            set_src(s, Mode::An, w & 7);
        } else {
            set_dst(s, Mode::An, w & 7);
        }
        return s;
    }
    switch (w) {
    case 0x4E70: return simple(Op::RESET);
    case 0x4E71: return simple(Op::NOP);
    case 0x4E72:
        s.op = Op::STOP;
        s.size = Size::Word;
        set_src(s, Mode::Imm, 0, Ext::Word);
        return s;
    case 0x4E73: return simple(Op::RTE);
    case 0x4E75: return simple(Op::RTS);
    case 0x4E76: return simple(Op::TRAPV);
    case 0x4E77: return simple(Op::RTR);
    default: break;
    }
    if ((w & 0xFF80) == 0x4E80) {
        s.op = (w & 0x40) != 0 ? Op::JMP : Op::JSR;
        return take_src(s, ea, kControl) ? s : Spec{};
    }
    return Spec{};
}

inline Spec classify_quick(u32 w) {
    Spec s;
    u32 size_bits = (w >> 6) & 3;
    if (size_bits == 3) {
        u32 ea = w & 0x3F;
        s.cond = static_cast<u8>((w >> 8) & 15);
        if ((ea >> 3) == 1) {
            s.op = Op::DBcc;
            s.size = Size::Word;
            set_src(s, Mode::Dn, w & 7);
            set_dst(s, Mode::PcDisp, 0, Ext::WordS);
            return s;
        }
        s.op = Op::Scc;
        s.size = Size::Byte;
        return take_dst(s, ea, kDataAlt) ? s : Spec{};
    }
    size_field(size_bits, s.size);
    s.op = (w & 0x0100) != 0 ? Op::SUBQ : Op::ADDQ;
    u32 data = (w >> 9) & 7;
    s.imm = static_cast<u8>(data == 0 ? 8 : data);
    s.imm_count = true;
    set_src(s, Mode::Imm, 0, Ext::Opc);
    u32 allowed = s.size == Size::Byte ? kDataAlt : (kDataAlt | kAn);
    return take_dst(s, w & 0x3F, allowed) ? s : Spec{};
}

inline Spec classify_branch(u32 w) {
    Spec s;
    u32 cond = (w >> 8) & 15;
    s.op = cond == 0 ? Op::BRA : (cond == 1 ? Op::BSR : Op::Bcc);
    s.cond = static_cast<u8>(cond);
    if ((w & 0xFF) == 0) {
        s.size = Size::Word;
        set_dst(s, Mode::PcDisp, 0, Ext::WordS);
    } else {
        s.size = Size::Byte;
        s.imm = static_cast<u8>(w);
        set_dst(s, Mode::PcDisp, 0, Ext::Opc);
    }
    return s;
}

// Lines 8, 9, 12 and 13: OR, SUB, AND and ADD and their relatives.
inline Spec classify_arith(u32 w) {
    Spec s;
    u32 line = w >> 12;
    u32 opmode = (w >> 6) & 7;
    u32 reg = (w >> 9) & 7;
    u32 ea = w & 0x3F;
    u32 ea_mode_bits = (w >> 3) & 7;
    bool pair = ea_mode_bits < 2;  // register-to-register forms
    Size size = Size::None;

    if (line == 8 || line == 12) {  // OR / AND, with their relatives
        bool is_or = line == 8;
        if (opmode == 3 || opmode == 7) {
            s.op = is_or ? (opmode == 3 ? Op::DIVU : Op::DIVS)
                         : (opmode == 3 ? Op::MULU : Op::MULS);
            s.size = Size::Word;
            if (!take_src(s, ea, kData)) {
                return Spec{};
            }
            set_dst(s, Mode::Dn, reg);
            return s;
        }
        if (opmode >= 4 && pair) {
            if (opmode == 4) {  // SBCD / ABCD
                s.op = is_or ? Op::SBCD : Op::ABCD;
                s.size = Size::Byte;
                Mode m = ea_mode_bits == 0 ? Mode::Dn : Mode::AnPre;
                set_src(s, m, w & 7);
                set_dst(s, m, reg);
                return s;
            }
            if (!is_or && (opmode == 5 || opmode == 6)) {
                // EXG: 01000 Dn,Dn / 01001 An,An / 10001 Dn,An
                if (opmode == 5) {
                    Mode m = ea_mode_bits == 0 ? Mode::Dn : Mode::An;
                    s.op = Op::EXG;
                    s.size = Size::Long;
                    set_src(s, m, reg);
                    set_dst(s, m, w & 7);
                    return s;
                }
                if (ea_mode_bits == 1) {
                    s.op = Op::EXG;
                    s.size = Size::Long;
                    set_src(s, Mode::Dn, reg);
                    set_dst(s, Mode::An, w & 7);
                    return s;
                }
            }
            return Spec{};
        }
        s.op = is_or ? Op::OR : Op::AND;
        size_field(opmode & 3, s.size);
        if (opmode < 3) {
            if (!take_src(s, ea, kData)) {
                return Spec{};
            }
            set_dst(s, Mode::Dn, reg);
        } else {
            set_src(s, Mode::Dn, reg);
            if (!take_dst(s, ea, kMemAlt)) {
                return Spec{};
            }
        }
        return s;
    }

    bool add = line == 13;
    if (opmode == 3 || opmode == 7) {
        s.op = add ? Op::ADDA : Op::SUBA;
        s.size = opmode == 3 ? Size::Word : Size::Long;
        if (!take_src(s, ea, kAll)) {
            return Spec{};
        }
        set_dst(s, Mode::An, reg);
        return s;
    }
    size_field(opmode & 3, size);
    s.size = size;
    if (opmode < 3) {
        s.op = add ? Op::ADD : Op::SUB;
        if (!take_src(s, ea, size == Size::Byte ? kData : kAll)) {
            return Spec{};
        }
        set_dst(s, Mode::Dn, reg);
        return s;
    }
    if (pair) {
        s.op = add ? Op::ADDX : Op::SUBX;
        Mode m = ea_mode_bits == 0 ? Mode::Dn : Mode::AnPre;
        set_src(s, m, w & 7);
        set_dst(s, m, reg);
        return s;
    }
    s.op = add ? Op::ADD : Op::SUB;
    set_src(s, Mode::Dn, reg);
    return take_dst(s, ea, kMemAlt) ? s : Spec{};
}

inline Spec classify_compare(u32 w) {
    Spec s;
    u32 opmode = (w >> 6) & 7;
    u32 reg = (w >> 9) & 7;
    u32 ea = w & 0x3F;
    if (opmode == 3 || opmode == 7) {
        s.op = Op::CMPA;
        s.size = opmode == 3 ? Size::Word : Size::Long;
        if (!take_src(s, ea, kAll)) {
            return Spec{};
        }
        set_dst(s, Mode::An, reg);
        return s;
    }
    size_field(opmode & 3, s.size);
    if (opmode < 3) {
        s.op = Op::CMP;
        if (!take_src(s, ea, s.size == Size::Byte ? kData : kAll)) {
            return Spec{};
        }
        set_dst(s, Mode::Dn, reg);
        return s;
    }
    if (((ea >> 3) & 7) == 1) {
        s.op = Op::CMPM;
        set_src(s, Mode::AnPost, w & 7);
        set_dst(s, Mode::AnPost, reg);
        return s;
    }
    s.op = Op::EOR;
    set_src(s, Mode::Dn, reg);
    return take_dst(s, ea, kDataAlt) ? s : Spec{};
}

inline Spec classify_shift(u32 w) {
    Spec s;
    u32 type;
    if (((w >> 6) & 3) == 3) {  // memory, one bit
        if ((w & 0x0800) != 0) {
            return Spec{};  // bit-field instructions
        }
        type = (w >> 9) & 3;
        s.size = Size::Word;
        s.imm = 1;
        s.imm_count = true;
        if (!take_dst(s, w & 0x3F, kMemAlt)) {
            return Spec{};
        }
    } else {
        type = (w >> 3) & 3;
        size_field((w >> 6) & 3, s.size);
        u32 count = (w >> 9) & 7;
        if ((w & 0x20) != 0) {
            set_src(s, Mode::Dn, count);
        } else {
            s.imm = static_cast<u8>(count == 0 ? 8 : count);
            s.imm_count = true;
            set_src(s, Mode::Imm, 0, Ext::Opc);
        }
        set_dst(s, Mode::Dn, w & 7);
    }
    static const Op kLeft[4] = {Op::ASL, Op::LSL, Op::ROXL, Op::ROL};
    static const Op kRight[4] = {Op::ASR, Op::LSR, Op::ROXR, Op::ROR};
    s.op = (w & 0x0100) != 0 ? kLeft[type] : kRight[type];
    return s;
}

inline Spec classify(u32 w) {
    switch (w >> 12) {
    case 0x0: return classify_bits(w);
    case 0x1:
    case 0x2:
    case 0x3: return classify_move(w);
    case 0x4: return classify_misc(w);
    case 0x5: return classify_quick(w);
    case 0x6: return classify_branch(w);
    case 0x7: {
        if ((w & 0x0100) != 0) {
            return Spec{};
        }
        Spec s;
        s.op = Op::MOVEQ;
        s.size = Size::Long;
        s.imm = static_cast<u8>(w);
        set_src(s, Mode::Imm, 0, Ext::Opc);
        set_dst(s, Mode::Dn, (w >> 9) & 7);
        return s;
    }
    case 0x8:
    case 0x9:
    case 0xC:
    case 0xD: return classify_arith(w);
    case 0xA: return simple(Op::ALine);
    case 0xB: return classify_compare(w);
    case 0xE: return classify_shift(w);
    default: return simple(Op::FLine);
    }
}

// The extension words an operand brings with it.
inline u32 ext_words(Mode m, Ext e, Size size) {
    switch (e) {
    case Ext::None:
    case Ext::Opc:
        return 0;
    case Ext::Word:
    case Ext::WordS:
    case Ext::Byte:
        return 1;
    case Ext::Ea:
        switch (m) {
        case Mode::AnDisp:
        case Mode::AnIndex:
        case Mode::AbsW:
        case Mode::PcDisp:
        case Mode::PcIndex:
            return 1;
        case Mode::AbsL:
            return 2;
        case Mode::Imm:
            return size == Size::Long ? 2 : 1;
        default:
            return 0;
        }
    }
    return 0;
}

inline Entry pack(const Spec& s) {
    Entry e;
    e.op = static_cast<u8>(s.op);
    e.size_flags = static_cast<u8>(static_cast<u32>(s.size) | (s.dst_first ? 4u : 0u) |
                                   (s.imm_count ? 8u : 0u));
    e.src = static_cast<u8>((static_cast<u32>(s.src_mode) << 3) | s.src_reg);
    e.dst = static_cast<u8>((static_cast<u32>(s.dst_mode) << 3) | s.dst_reg);
    e.ext = static_cast<u8>(static_cast<u32>(s.src_ext) | (static_cast<u32>(s.dst_ext) << 4));
    e.cond = s.cond;
    e.imm = s.imm;
    e.words = static_cast<u8>(1 + ext_words(s.src_mode, s.src_ext, s.size) +
                              ext_words(s.dst_mode, s.dst_ext, s.size));
    return e;
}

// All zero is an Illegal entry, so an unfilled table decodes safely.
inline Entry g_table[0x10000];
inline bool g_ready = false;

inline u32 sign16(u32 w) {
    return (w & 0x8000) != 0 ? w | 0xFFFF0000u : w;
}

// Reads one operand's extension words at pc and advances pc past them.
inline bool fetch(const GuestMemory& mem, u32& pc, Ea& ea, Ext kind, Size size, u8 imm) {
    u32 w = 0;
    u32 w2 = 0;
    switch (kind) {
    case Ext::None:
        return true;
    case Ext::Opc:
        ea.value = (imm & 0x80u) != 0 ? imm | 0xFFFFFF00u : imm;
        return true;
    case Ext::Word:
    case Ext::WordS:
    case Ext::Byte:
        if (!mem.read16(pc, w)) {
            return false;
        }
        pc += 2;
        ea.value = kind == Ext::Word ? w : (kind == Ext::WordS ? sign16(w) : (w & 0xFF));
        return true;
    case Ext::Ea:
        break;
    }
    switch (ea.mode) {
    case Mode::AnDisp:
    case Mode::PcDisp:
    case Mode::AbsW:
        if (!mem.read16(pc, w)) {
            return false;
        }
        pc += 2;
        ea.value = sign16(w);
        return true;
    case Mode::AnIndex:
    case Mode::PcIndex:
        if (!mem.read16(pc, w)) {
            return false;
        }
        pc += 2;
        ea.ext = static_cast<u16>(w);
        ea.value = (w & 0x80) != 0 ? (w & 0xFF) | 0xFFFFFF00u : (w & 0xFF);
        return true;
    case Mode::AbsL:
        if (!mem.read16(pc, w) || !mem.read16(pc + 2, w2)) {
            return false;
        }
        pc += 4;
        ea.value = (w << 16) | w2;
        return true;
    case Mode::Imm:
        if (!mem.read16(pc, w)) {
            return false;
        }
        pc += 2;
        if (size == Size::Long) {
            if (!mem.read16(pc, w2)) {
                return false;
            }
            pc += 2;
            ea.value = (w << 16) | w2;
        } else {
            ea.value = size == Size::Byte ? (w & 0xFF) : w;
        }
        return true;
    default:
        return true;
    }
}

inline void clear(Insn& out) {
    out.op = Op::Illegal;
    out.size = Size::None;
    out.src = Ea{Mode::None, 0, 0, 0};
    out.dst = Ea{Mode::None, 0, 0, 0};
    out.cond = 0;
    out.count = 0;
    out.opcode = 0;
    out.words = 0;
}

}

inline void decode_init() {
    if (detail::g_ready) {
        return;
    }
    for (u32 w = 0; w < 0x10000; ++w) {
        detail::g_table[w] = detail::pack(detail::classify(w));
    }
    detail::g_ready = true;
}

inline bool decode(const GuestMemory& mem, u32 pc, Insn& out) {
    detail::clear(out);
    u32 w = 0;
    if (!mem.read16(pc, w)) {
        return false;
    }
    decode_init();
    const detail::Entry& e = detail::g_table[w];
    out.opcode = static_cast<u16>(w);
    out.words = 1;
    if (e.op == static_cast<u8>(Op::Illegal)) {
        return true;
    }

    Ea src{static_cast<Mode>(e.src >> 3), static_cast<u8>(e.src & 7), 0, 0};
    Ea dst{static_cast<Mode>(e.dst >> 3), static_cast<u8>(e.dst & 7), 0, 0};
    Size size = static_cast<Size>(e.size_flags & 3);
    detail::Ext src_kind = static_cast<detail::Ext>(e.ext & 15);
    detail::Ext dst_kind = static_cast<detail::Ext>(e.ext >> 4);

    u32 next = pc + 2;
    bool ok;
    if ((e.size_flags & 4) != 0) {
        ok = detail::fetch(mem, next, dst, dst_kind, size, e.imm) &&
             detail::fetch(mem, next, src, src_kind, size, e.imm);
    } else {
        ok = detail::fetch(mem, next, src, src_kind, size, e.imm) &&
             detail::fetch(mem, next, dst, dst_kind, size, e.imm);
    }
    if (!ok) {
        out.opcode = 0;
        out.words = 0;
        return false;
    }

    out.op = static_cast<Op>(e.op);
    out.size = size;
    out.src = src;
    out.dst = dst;
    out.cond = e.cond;
    out.count = (e.size_flags & 8) != 0 ? e.imm : 0;
    out.words = e.words;
    return true;
}

}
