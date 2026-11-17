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

#include "types.hpp"

namespace hombre::x86 {

using i32 = __INT32_TYPE__;

enum Reg : u8 { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

enum class Alu : u8 { Add, Or, Adc, Sbb, And, Sub, Xor, Cmp };

enum class Shift : u8 { Rol, Ror, Rcl, Rcr, Shl, Shr, Sar = 7 };

enum class Cc : u8 { O, NO, B, AE, E, NE, BE, A, S, NS, P, NP, L, GE, LE, G };

class Emitter {
public:
    Emitter(u8* out, usize capacity) : out_(out), capacity_(capacity) {}

    bool ok() const { return ok_; }
    usize size() const { return size_; }
    u8* cursor() const { return out_ + size_; }

    void mov_rr(Reg d, Reg s, unsigned bits) {
        prefix_rr(bits, s, d);
        byte(bits == 8 ? 0x88 : 0x89);
        modrm_reg(s, d);
    }

    void mov_ri(Reg d, u64 imm, unsigned bits) {
        if (bits == 16) {
            byte(0x66);
        }
        if (bits == 64 || d >= 8 || (bits == 8 && d >= 4)) {
            byte(static_cast<u8>(0x40 | rex_w(bits) | (d >> 3)));
        }
        byte(static_cast<u8>((bits == 8 ? 0xB0 : 0xB8) + (d & 7)));
        immediate(imm, bits / 8);
    }

    void mov_load(Reg d, Reg base, i32 disp, unsigned bits) {
        prefix_mem(bits, d, base, 0);
        byte(bits == 8 ? 0x8A : 0x8B);
        modrm_mem(d, base, disp, false, 0);
    }

    void mov_store(Reg base, i32 disp, Reg s, unsigned bits) {
        prefix_mem(bits, s, base, 0);
        byte(bits == 8 ? 0x88 : 0x89);
        modrm_mem(s, base, disp, false, 0);
    }

    void mov_load_idx(Reg d, Reg base, Reg index, unsigned bits) {
        prefix_mem(bits, d, base, index);
        byte(bits == 8 ? 0x8A : 0x8B);
        modrm_mem(d, base, 0, true, index);
    }

    void mov_store_idx(Reg base, Reg index, Reg s, unsigned bits) {
        prefix_mem(bits, s, base, index);
        byte(bits == 8 ? 0x88 : 0x89);
        modrm_mem(s, base, 0, true, index);
    }

    void alu_rr(Alu op, Reg d, Reg s, unsigned bits) {
        prefix_rr(bits, s, d);
        byte(static_cast<u8>((static_cast<unsigned>(op) << 3) | (bits == 8 ? 0 : 1)));
        modrm_reg(s, d);
    }

    void alu_ri(Alu op, Reg d, i32 imm, unsigned bits) {
        const bool fits_imm8 = imm >= -128 && imm <= 127;
        const u8 opcode = bits == 8 ? 0x80 : (fits_imm8 ? 0x83 : 0x81);
        prefix_digit(bits, d);
        byte(opcode);
        modrm_reg(static_cast<unsigned>(op), d);
        immediate(static_cast<u32>(imm), bits == 8 || fits_imm8 ? 1 : (bits == 16 ? 2 : 4));
    }

    void shift_cl(Shift op, Reg d, unsigned bits) {
        prefix_digit(bits, d);
        byte(bits == 8 ? 0xD2 : 0xD3);
        modrm_reg(static_cast<unsigned>(op), d);
    }

    void shift_i(Shift op, Reg d, u8 n, unsigned bits) {
        prefix_digit(bits, d);
        byte(bits == 8 ? 0xC0 : 0xC1);
        modrm_reg(static_cast<unsigned>(op), d);
        byte(n);
    }

    void bswap(Reg r, unsigned bits) {
        if (bits == 16) {
            shift_i(Shift::Rol, r, 8, 16);
            return;
        }
        if (bits == 64 || r >= 8) {
            byte(static_cast<u8>(0x40 | rex_w(bits) | (r >> 3)));
        }
        byte(0x0F);
        byte(static_cast<u8>(0xC8 + (r & 7)));
    }

    void movzx(Reg d, Reg s, unsigned from_bits) {
        prefix_movx(from_bits, d, s);
        byte(0x0F);
        byte(from_bits == 8 ? 0xB6 : 0xB7);
        modrm_reg(d, s);
    }

    void movsx(Reg d, Reg s, unsigned from_bits) {
        if (from_bits == 32) {
            byte(static_cast<u8>(0x48 | ((d >> 3) << 2) | (s >> 3)));
            byte(0x63);
        } else {
            prefix_movx(from_bits, d, s);
            byte(0x0F);
            byte(from_bits == 8 ? 0xBE : 0xBF);
        }
        modrm_reg(d, s);
    }

    void setcc(Cc cc, Reg d) {
        prefix_digit(8, d);
        byte(0x0F);
        byte(static_cast<u8>(0x90 + static_cast<unsigned>(cc)));
        modrm_reg(0, d);
    }

    usize jcc_rel32(Cc cc) {
        const usize at = size_ + 2;
        byte(0x0F);
        byte(static_cast<u8>(0x80 + static_cast<unsigned>(cc)));
        immediate(0, 4);
        return at;
    }

    usize jmp_rel32() {
        const usize at = size_ + 1;
        byte(0xE9);
        immediate(0, 4);
        return at;
    }

    void patch(usize at) {
        if (at + 4 > size_) {
            return;
        }
        immediate_at(at, static_cast<u32>(size_ - (at + 4)), 4);
    }

    void call_abs(u64 target) {
        mov_ri(RAX, target, 64);
        byte(0xFF);
        byte(0xD0);
    }

    void push(Reg r) {
        if (r >= 8) {
            byte(0x41);
        }
        byte(static_cast<u8>(0x50 + (r & 7)));
    }

    void pop(Reg r) {
        if (r >= 8) {
            byte(0x41);
        }
        byte(static_cast<u8>(0x58 + (r & 7)));
    }

    void ret() { byte(0xC3); }

private:
    static unsigned rex_w(unsigned bits) { return bits == 64 ? 8 : 0; }

    void byte(u8 value) {
        if (size_ < capacity_) {
            out_[size_++] = value;
        } else {
            ok_ = false;
        }
    }

    void immediate(u64 value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            byte(static_cast<u8>(value >> (8 * i)));
        }
    }

    void immediate_at(usize at, u32 value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            out_[at + i] = static_cast<u8>(value >> (8 * i));
        }
    }

    // A register number 4..7 names AH/CH/DH/BH without REX but SPL/BPL/SIL/DIL with it.
    void prefix_rr(unsigned bits, unsigned reg, unsigned rm) {
        if (bits == 16) {
            byte(0x66);
        }
        if (bits == 64 || reg >= 8 || rm >= 8 || (bits == 8 && (reg >= 4 || rm >= 4))) {
            byte(static_cast<u8>(0x40 | rex_w(bits) | ((reg >> 3) << 2) | (rm >> 3)));
        }
    }

    void prefix_mem(unsigned bits, unsigned reg, unsigned base, unsigned index) {
        if (bits == 16) {
            byte(0x66);
        }
        if (bits == 64 || reg >= 8 || base >= 8 || index >= 8 || (bits == 8 && reg >= 4)) {
            byte(static_cast<u8>(0x40 | rex_w(bits) | ((reg >> 3) << 2) | ((index >> 3) << 1) |
                                 (base >> 3)));
        }
    }

    // The ModRM.reg field of these forms is an opcode digit, never a register.
    void prefix_digit(unsigned bits, unsigned rm) {
        if (bits == 16) {
            byte(0x66);
        }
        if (bits == 64 || rm >= 8 || (bits == 8 && rm >= 4)) {
            byte(static_cast<u8>(0x40 | rex_w(bits) | (rm >> 3)));
        }
    }

    void prefix_movx(unsigned from_bits, unsigned reg, unsigned rm) {
        if (reg >= 8 || rm >= 8 || (from_bits == 8 && rm >= 4)) {
            byte(static_cast<u8>(0x40 | ((reg >> 3) << 2) | (rm >> 3)));
        }
    }

    void modrm_reg(unsigned reg, unsigned rm) {
        byte(static_cast<u8>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    }

    void modrm_mem(unsigned reg, unsigned base, i32 disp, bool indexed, unsigned index) {
        const unsigned base_low = base & 7;
        const bool sib = indexed || base_low == 4;
        // RBP and R13 in mod 00 address RIP-relative, so disp 0 takes a disp8.
        const unsigned mod =
            disp == 0 && base_low != 5 ? 0 : (disp >= -128 && disp <= 127 ? 1 : 2);
        byte(static_cast<u8>((mod << 6) | ((reg & 7) << 3) | (sib ? 4 : base_low)));
        if (sib) {
            byte(static_cast<u8>(((indexed ? (index & 7) : 4) << 3) | base_low));
        }
        if (mod == 1) {
            byte(static_cast<u8>(disp));
        } else if (mod == 2) {
            immediate(static_cast<u32>(disp), 4);
        }
    }

    u8* out_;
    usize capacity_;
    usize size_ = 0;
    bool ok_ = true;
};

}
