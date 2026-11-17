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
#include "m68k_decode.hpp"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;

namespace {

constexpr const char* kObjdump = "/usr/bin/m68k-linux-gnu-objdump";

struct Image {
    std::vector<u8> bytes;

    Image(std::initializer_list<u16> words) {
        for (u16 w : words) {
            bytes.push_back(static_cast<u8>(w >> 8));
            bytes.push_back(static_cast<u8>(w));
        }
    }

    GuestMemory memory() { return GuestMemory{bytes.data(), static_cast<u32>(bytes.size())}; }
};

Insn decode_words(std::initializer_list<u16> words) {
    Image image(words);
    Insn insn{};
    CHECK(decode(image.memory(), 0, insn));
    return insn;
}

void check_none(const Ea& ea) {
    CHECK(ea.mode == Mode::None);
    CHECK_EQ(ea.reg, 0u);
    CHECK_EQ(ea.value, 0u);
    CHECK_EQ(ea.ext, 0u);
}

void moveq_loads_a_sign_extended_byte() {
    Insn i = decode_words({0x7005});
    CHECK(i.op == Op::MOVEQ);
    CHECK(i.size == Size::Long);
    CHECK(i.src.mode == Mode::Imm);
    CHECK_EQ(i.src.value, 5u);
    CHECK(i.dst.mode == Mode::Dn);
    CHECK_EQ(i.dst.reg, 0u);
    CHECK_EQ(i.words, 1u);
    CHECK_EQ(i.opcode, 0x7005u);

    Insn n = decode_words({0x72FF});
    CHECK(n.op == Op::MOVEQ);
    CHECK_EQ(n.src.value, 0xFFFFFFFFu);
    CHECK_EQ(n.dst.reg, 1u);
}

void operand_free_instructions() {
    Insn rts = decode_words({0x4E75});
    CHECK(rts.op == Op::RTS);
    CHECK_EQ(rts.words, 1u);
    check_none(rts.src);
    check_none(rts.dst);
    CHECK(decode_words({0x4E71}).op == Op::NOP);
    CHECK(decode_words({0x4E73}).op == Op::RTE);
    CHECK(decode_words({0x4E77}).op == Op::RTR);
    CHECK(decode_words({0x4E70}).op == Op::RESET);
    CHECK(decode_words({0x4E76}).op == Op::TRAPV);

    Insn stop = decode_words({0x4E72, 0x2700});
    CHECK(stop.op == Op::STOP);
    CHECK_EQ(stop.words, 2u);
    CHECK(stop.src.mode == Mode::Imm);
    CHECK_EQ(stop.src.value, 0x2700u);

    Insn trap = decode_words({0x4E4F});
    CHECK(trap.op == Op::TRAP);
    CHECK_EQ(trap.count, 15u);
}

void move_with_immediate_source() {
    Insn i = decode_words({0x303C, 0x1234});
    CHECK(i.op == Op::MOVE);
    CHECK(i.size == Size::Word);
    CHECK(i.src.mode == Mode::Imm);
    CHECK_EQ(i.src.value, 0x1234u);
    CHECK(i.dst.mode == Mode::Dn);
    CHECK_EQ(i.dst.reg, 0u);
    CHECK_EQ(i.words, 2u);

    Insn b = decode_words({0x103C, 0x00FF});
    CHECK(b.size == Size::Byte);
    CHECK_EQ(b.src.value, 0xFFu);
    CHECK_EQ(b.words, 2u);

    Insn l = decode_words({0x203C, 0x1234, 0x5678});
    CHECK(l.size == Size::Long);
    CHECK_EQ(l.src.value, 0x12345678u);
    CHECK_EQ(l.words, 3u);
}

void movea_from_absolute_long() {
    Insn i = decode_words({0x2079, 0x0000, 0x1000});
    CHECK(i.op == Op::MOVEA);
    CHECK(i.size == Size::Long);
    CHECK(i.src.mode == Mode::AbsL);
    CHECK_EQ(i.src.value, 0x1000u);
    CHECK(i.dst.mode == Mode::An);
    CHECK_EQ(i.dst.reg, 0u);
    CHECK_EQ(i.words, 3u);
}

void move_with_two_extension_sets() {
    // move.l (4,A0,D1.L),$1234.w
    Insn i = decode_words({0x21F0, 0x1804, 0x1234});
    CHECK(i.op == Op::MOVE);
    CHECK(i.src.mode == Mode::AnIndex);
    CHECK_EQ(i.src.reg, 0u);
    CHECK_EQ(i.src.value, 4u);
    CHECK_EQ(i.src.ext, 0x1804u);
    CHECK(i.dst.mode == Mode::AbsW);
    CHECK_EQ(i.dst.value, 0x1234u);
    CHECK_EQ(i.words, 3u);

    Insn neg = decode_words({0x2038, 0x8000});
    CHECK(neg.src.mode == Mode::AbsW);
    CHECK_EQ(neg.src.value, 0xFFFF8000u);

    // move.w (-2,A3),(8,PC)... dst d(An) is a word displacement
    Insn d = decode_words({0x3B6B, 0xFFFE, 0x0010});
    CHECK(d.src.mode == Mode::AnDisp);
    CHECK_EQ(d.src.reg, 3u);
    CHECK_EQ(d.src.value, 0xFFFFFFFEu);
    CHECK(d.dst.mode == Mode::AnDisp);
    CHECK_EQ(d.dst.reg, 5u);
    CHECK_EQ(d.dst.value, 0x10u);
}

void address_modes_without_extension_words() {
    Insn i = decode_words({0x2299});  // move.l (A1)+,(A1)
    CHECK(i.src.mode == Mode::AnPost);
    CHECK_EQ(i.src.reg, 1u);
    CHECK(i.dst.mode == Mode::AnInd);
    CHECK_EQ(i.dst.reg, 1u);
    CHECK_EQ(i.words, 1u);

    Insn p = decode_words({0x2F00});  // move.l D0,-(A7)
    CHECK(p.dst.mode == Mode::AnPre);
    CHECK_EQ(p.dst.reg, 7u);
}

void jumps_and_load_effective_address() {
    Insn jsr = decode_words({0x4E90});
    CHECK(jsr.op == Op::JSR);
    CHECK(jsr.src.mode == Mode::AnInd);
    CHECK_EQ(jsr.src.reg, 0u);
    CHECK_EQ(jsr.words, 1u);

    Insn jmp = decode_words({0x4EFA, 0x0010});
    CHECK(jmp.op == Op::JMP);
    CHECK(jmp.src.mode == Mode::PcDisp);
    CHECK_EQ(jmp.src.value, 0x10u);
    CHECK_EQ(jmp.words, 2u);

    Insn lea = decode_words({0x43FA, 0x0010});
    CHECK(lea.op == Op::LEA);
    CHECK(lea.src.mode == Mode::PcDisp);
    CHECK(lea.dst.mode == Mode::An);
    CHECK_EQ(lea.dst.reg, 1u);

    Insn idx = decode_words({0x41FB, 0x8804});
    CHECK(idx.op == Op::LEA);
    CHECK(idx.src.mode == Mode::PcIndex);
    CHECK_EQ(idx.src.ext, 0x8804u);
    CHECK_EQ(idx.src.value, 4u);

    Insn pea = decode_words({0x4879, 0x0001, 0x0002});
    CHECK(pea.op == Op::PEA);
    CHECK(pea.src.mode == Mode::AbsL);
    CHECK_EQ(pea.src.value, 0x10002u);
    CHECK_EQ(pea.words, 3u);
}

void line_a_line_f_and_illegal() {
    Insn a = decode_words({0xA123});
    CHECK(a.op == Op::ALine);
    CHECK_EQ(a.words, 1u);
    CHECK_EQ(a.opcode, 0xA123u);
    Insn f = decode_words({0xF000});
    CHECK(f.op == Op::FLine);
    CHECK_EQ(f.words, 1u);
    Insn ill = decode_words({0x4AFC});
    CHECK(ill.op == Op::Illegal);
    CHECK_EQ(ill.words, 1u);
    check_none(ill.src);
    check_none(ill.dst);
}

void reserved_and_later_models_are_illegal() {
    CHECK(decode_words({0x1008}).op == Op::Illegal);        // move.b A0,D0
    CHECK(decode_words({0x1048}).op == Op::Illegal);        // movea.b
    CHECK(decode_words({0x41C0}).op == Op::Illegal);        // lea D0,A0
    CHECK(decode_words({0x4E7A, 0x0000}).op == Op::Illegal);  // movec
    CHECK(decode_words({0x4E74, 0x0000}).op == Op::Illegal);  // rtd
    CHECK(decode_words({0x42C0}).op == Op::Illegal);        // move from CCR
    CHECK(decode_words({0x4C00, 0x0000}).op == Op::Illegal);  // mulu.l
    CHECK(decode_words({0x4808}).op == Op::Illegal);        // link.l
    CHECK(decode_words({0x4900}).op == Op::Illegal);        // chk.l
    CHECK(decode_words({0x0E10, 0x0000}).op == Op::Illegal);  // moves
    CHECK(decode_words({0x06C0}).op == Op::Illegal);        // rtm/callm
    CHECK(decode_words({0xE8C0, 0x0000}).op == Op::Illegal);  // bfextu
    CHECK(decode_words({0x50FC}).op == Op::Illegal);        // trapt
    CHECK(decode_words({0x5008}).op == Op::Illegal);        // addq.b #8,A0
    CHECK(decode_words({0xC180}).op == Op::Illegal);        // and Dn,Dn is no instruction
    CHECK_EQ(decode_words({0x1008}).words, 1u);
}

void quick_arithmetic() {
    Insn a = decode_words({0x5641});  // addq.w #3,D1
    CHECK(a.op == Op::ADDQ);
    CHECK(a.size == Size::Word);
    CHECK_EQ(a.count, 3u);
    CHECK(a.src.mode == Mode::Imm);
    CHECK_EQ(a.src.value, 3u);
    CHECK(a.dst.mode == Mode::Dn);
    CHECK_EQ(a.dst.reg, 1u);

    Insn eight = decode_words({0x5041});  // addq.w #8,D1
    CHECK_EQ(eight.count, 8u);

    Insn s = decode_words({0x5392});  // subq.l #1,(A2)
    CHECK(s.op == Op::SUBQ);
    CHECK(s.size == Size::Long);
    CHECK_EQ(s.count, 1u);
    CHECK(s.dst.mode == Mode::AnInd);
    CHECK_EQ(s.dst.reg, 2u);

    Insn an = decode_words({0x548F});  // addq.l #2,A7
    CHECK(an.op == Op::ADDQ);
    CHECK(an.dst.mode == Mode::An);
}

void branches() {
    Insn s = decode_words({0x6004});
    CHECK(s.op == Op::BRA);
    CHECK(s.size == Size::Byte);
    CHECK(s.dst.mode == Mode::PcDisp);
    CHECK_EQ(s.dst.value, 4u);
    CHECK_EQ(s.words, 1u);

    Insn back = decode_words({0x60FA});
    CHECK_EQ(back.dst.value, 0xFFFFFFFAu);

    Insn w = decode_words({0x6000, 0xFFFE});
    CHECK(w.op == Op::BRA);
    CHECK(w.size == Size::Word);
    CHECK_EQ(w.dst.value, 0xFFFFFFFEu);
    CHECK_EQ(w.words, 2u);

    Insn bsr = decode_words({0x6100, 0x0100});
    CHECK(bsr.op == Op::BSR);
    CHECK_EQ(bsr.dst.value, 0x100u);

    Insn bne = decode_words({0x6610});
    CHECK(bne.op == Op::Bcc);
    CHECK_EQ(bne.cond, 6u);

    Insn db = decode_words({0x51C8, 0xFFFC});  // dbf D0,*-2
    CHECK(db.op == Op::DBcc);
    CHECK_EQ(db.cond, 1u);
    CHECK(db.src.mode == Mode::Dn);
    CHECK_EQ(db.src.reg, 0u);
    CHECK(db.dst.mode == Mode::PcDisp);
    CHECK_EQ(db.dst.value, 0xFFFFFFFCu);
    CHECK_EQ(db.words, 2u);

    Insn seq = decode_words({0x57C0});
    CHECK(seq.op == Op::Scc);
    CHECK_EQ(seq.cond, 7u);
    CHECK(seq.size == Size::Byte);
    CHECK(seq.dst.mode == Mode::Dn);
}

void bit_operations() {
    Insn s = decode_words({0x0800, 0x0003});  // btst #3,D0
    CHECK(s.op == Op::BTST);
    CHECK(s.size == Size::Long);
    CHECK(s.src.mode == Mode::Imm);
    CHECK_EQ(s.src.value, 3u);
    CHECK(s.dst.mode == Mode::Dn);
    CHECK_EQ(s.words, 2u);

    Insn d = decode_words({0x03D0});  // bset D1,(A0)
    CHECK(d.op == Op::BSET);
    CHECK(d.size == Size::Byte);
    CHECK(d.src.mode == Mode::Dn);
    CHECK_EQ(d.src.reg, 1u);
    CHECK(d.dst.mode == Mode::AnInd);

    Insn bc = decode_words({0x0868, 0x0007, 0x0004});  // bchg #7,(4,A0)
    CHECK(bc.op == Op::BCHG);
    CHECK_EQ(bc.src.value, 7u);
    CHECK(bc.dst.mode == Mode::AnDisp);
    CHECK_EQ(bc.dst.value, 4u);
    CHECK_EQ(bc.words, 3u);

    CHECK(decode_words({0x0880, 0x0001}).op == Op::BCLR);
}

void immediate_arithmetic_and_status_register() {
    Insn addi = decode_words({0x0683, 0x1234, 0x5678});
    CHECK(addi.op == Op::ADDI);
    CHECK(addi.size == Size::Long);
    CHECK_EQ(addi.src.value, 0x12345678u);
    CHECK(addi.dst.mode == Mode::Dn);
    CHECK_EQ(addi.dst.reg, 3u);
    CHECK_EQ(addi.words, 3u);

    Insn ori = decode_words({0x003C, 0x01FF});
    CHECK(ori.op == Op::ORI_CCR);
    CHECK(ori.size == Size::Byte);
    CHECK_EQ(ori.src.value, 0xFFu);
    CHECK_EQ(ori.words, 2u);
    CHECK(decode_words({0x023C, 0x0000}).op == Op::ANDI_CCR);
    CHECK(decode_words({0x0A3C, 0x0000}).op == Op::EORI_CCR);

    Insn andi = decode_words({0x027C, 0xF8FF});
    CHECK(andi.op == Op::ANDI_SR);
    CHECK(andi.size == Size::Word);
    CHECK_EQ(andi.src.value, 0xF8FFu);
    CHECK(decode_words({0x007C, 0x0700}).op == Op::ORI_SR);
    CHECK(decode_words({0x0A7C, 0x0700}).op == Op::EORI_SR);

    Insn cmpi = decode_words({0x0C40, 0x0010});
    CHECK(cmpi.op == Op::CMPI);
    CHECK(cmpi.size == Size::Word);

    Insn mv = decode_words({0x46FC, 0x2700});  // move #$2700,SR
    CHECK(mv.op == Op::MOVE_SR);
    CHECK(mv.src.mode == Mode::Imm);
    CHECK_EQ(mv.src.value, 0x2700u);
    CHECK(decode_words({0x44C0}).op == Op::MOVE_CCR);
    CHECK(decode_words({0x40C0}).op == Op::MOVE_FROM_SR);
    CHECK(decode_words({0x40C0}).dst.mode == Mode::Dn);
}

void multiple_register_moves() {
    Insn push = decode_words({0x48E7, 0xC080});  // movem.l D0-D1/A0,-(A7)
    CHECK(push.op == Op::MOVEM);
    CHECK(push.size == Size::Long);
    CHECK(push.src.mode == Mode::Imm);
    CHECK_EQ(push.src.value, 0xC080u);
    CHECK(push.dst.mode == Mode::AnPre);
    CHECK_EQ(push.dst.reg, 7u);
    CHECK_EQ(push.words, 2u);

    Insn pop = decode_words({0x4CDF, 0x0003});  // movem.l (A7)+,D0-D1
    CHECK(pop.op == Op::MOVEM);
    CHECK(pop.src.mode == Mode::AnPost);
    CHECK_EQ(pop.src.reg, 7u);
    CHECK(pop.dst.mode == Mode::Imm);
    CHECK_EQ(pop.dst.value, 3u);

    // The mask comes first even when the memory side has its own word.
    Insn disp = decode_words({0x4CA8, 0x0006, 0x0010});  // movem.w (16,A0),D1-D2
    CHECK(disp.size == Size::Word);
    CHECK(disp.dst.mode == Mode::Imm);
    CHECK_EQ(disp.dst.value, 6u);
    CHECK(disp.src.mode == Mode::AnDisp);
    CHECK_EQ(disp.src.value, 0x10u);
    CHECK_EQ(disp.words, 3u);

    Insn mp = decode_words({0x0189, 0x0004});  // movep.w D0,(4,A1)
    CHECK(mp.op == Op::MOVEP);
    CHECK(mp.size == Size::Word);
    CHECK(mp.src.mode == Mode::Dn);
    CHECK(mp.dst.mode == Mode::AnDisp);
    CHECK_EQ(mp.dst.reg, 1u);
    CHECK_EQ(mp.dst.value, 4u);
    Insn mpl = decode_words({0x0348, 0x0002});  // movep.l (2,A0),D1
    CHECK(mpl.size == Size::Long);
    CHECK(mpl.src.mode == Mode::AnDisp);
    CHECK(mpl.dst.mode == Mode::Dn);
    CHECK_EQ(mpl.dst.reg, 1u);
}

void register_instructions() {
    Insn link = decode_words({0x4E56, 0xFFF8});
    CHECK(link.op == Op::LINK);
    CHECK(link.src.mode == Mode::An);
    CHECK_EQ(link.src.reg, 6u);
    CHECK(link.dst.mode == Mode::Imm);
    CHECK_EQ(link.dst.value, 0xFFFFFFF8u);
    CHECK_EQ(link.words, 2u);

    Insn unlk = decode_words({0x4E5E});
    CHECK(unlk.op == Op::UNLK);
    CHECK(unlk.dst.mode == Mode::An);
    CHECK_EQ(unlk.dst.reg, 6u);

    Insn to = decode_words({0x4E62});  // move A2,USP
    CHECK(to.op == Op::MOVE_USP);
    CHECK(to.src.mode == Mode::An);
    CHECK_EQ(to.src.reg, 2u);
    CHECK(to.dst.mode == Mode::None);
    Insn from = decode_words({0x4E6B});  // move USP,A3
    CHECK(from.op == Op::MOVE_USP);
    CHECK(from.src.mode == Mode::None);
    CHECK(from.dst.mode == Mode::An);
    CHECK_EQ(from.dst.reg, 3u);

    Insn swap = decode_words({0x4843});
    CHECK(swap.op == Op::SWAP);
    CHECK(swap.dst.mode == Mode::Dn);
    CHECK_EQ(swap.dst.reg, 3u);
    Insn extw = decode_words({0x4880});
    CHECK(extw.op == Op::EXT);
    CHECK(extw.size == Size::Word);
    Insn extl = decode_words({0x48C2});
    CHECK(extl.size == Size::Long);
    CHECK_EQ(extl.dst.reg, 2u);

    Insn exg = decode_words({0xC189});  // exg D0,A1
    CHECK(exg.op == Op::EXG);
    CHECK(exg.src.mode == Mode::Dn);
    CHECK_EQ(exg.src.reg, 0u);
    CHECK(exg.dst.mode == Mode::An);
    CHECK_EQ(exg.dst.reg, 1u);
    Insn exd = decode_words({0xC541});  // exg D2,D1
    CHECK(exd.src.mode == Mode::Dn);
    CHECK_EQ(exd.src.reg, 2u);
    CHECK(exd.dst.mode == Mode::Dn);
    CHECK_EQ(exd.dst.reg, 1u);
    Insn exa = decode_words({0xC54A});  // exg A2,A2
    CHECK(exa.src.mode == Mode::An);
    CHECK(exa.dst.mode == Mode::An);
}

void arithmetic_forms() {
    Insn add = decode_words({0xD240});  // add.w D0,D1
    CHECK(add.op == Op::ADD);
    CHECK(add.src.mode == Mode::Dn);
    CHECK(add.dst.mode == Mode::Dn);
    CHECK_EQ(add.dst.reg, 1u);

    Insn rev = decode_words({0xD350});  // add.w D1,(A0)
    CHECK(rev.op == Op::ADD);
    CHECK(rev.src.mode == Mode::Dn);
    CHECK_EQ(rev.src.reg, 1u);
    CHECK(rev.dst.mode == Mode::AnInd);

    Insn adda = decode_words({0xD0FC, 0xFFFF});  // adda.w #-1,A0
    CHECK(adda.op == Op::ADDA);
    CHECK(adda.size == Size::Word);
    CHECK(adda.dst.mode == Mode::An);
    CHECK_EQ(adda.src.value, 0xFFFFu);

    Insn addx = decode_words({0xD308});  // addx.b -(A0),-(A1)
    CHECK(addx.op == Op::ADDX);
    CHECK(addx.size == Size::Byte);
    CHECK(addx.src.mode == Mode::AnPre);
    CHECK_EQ(addx.src.reg, 0u);
    CHECK(addx.dst.mode == Mode::AnPre);
    CHECK_EQ(addx.dst.reg, 1u);

    Insn subx = decode_words({0x9140});  // subx.w D0,D0
    CHECK(subx.op == Op::SUBX);
    CHECK(subx.src.mode == Mode::Dn);

    Insn cmpm = decode_words({0xB308});  // cmpm.b (A0)+,(A1)+
    CHECK(cmpm.op == Op::CMPM);
    CHECK(cmpm.src.mode == Mode::AnPost);
    CHECK_EQ(cmpm.src.reg, 0u);
    CHECK(cmpm.dst.mode == Mode::AnPost);
    CHECK_EQ(cmpm.dst.reg, 1u);

    Insn eor = decode_words({0xB340});  // eor.w D1,D0
    CHECK(eor.op == Op::EOR);
    CHECK(eor.src.mode == Mode::Dn);
    CHECK_EQ(eor.src.reg, 1u);
    CHECK(eor.dst.mode == Mode::Dn);

    CHECK(decode_words({0xB0C1}).op == Op::CMPA);
    CHECK(decode_words({0xB041}).op == Op::CMP);
    CHECK(decode_words({0x8041}).op == Op::OR);
    CHECK(decode_words({0xC041}).op == Op::AND);
    CHECK(decode_words({0x9041}).op == Op::SUB);
    CHECK(decode_words({0x90C1}).op == Op::SUBA);

    Insn mulu = decode_words({0xC0FC, 0x0005});
    CHECK(mulu.op == Op::MULU);
    CHECK(mulu.size == Size::Word);
    CHECK_EQ(mulu.words, 2u);
    CHECK(decode_words({0xC1C1}).op == Op::MULS);
    CHECK(decode_words({0x80C1}).op == Op::DIVU);
    CHECK(decode_words({0x81C1}).op == Op::DIVS);
    CHECK(decode_words({0x8101}).op == Op::SBCD);
    CHECK(decode_words({0xC101}).op == Op::ABCD);
    CHECK(decode_words({0x4801}).op == Op::NBCD);
    CHECK(decode_words({0x4A00}).op == Op::TST);
    CHECK(decode_words({0x4AC0}).op == Op::TAS);
    CHECK(decode_words({0x4280}).op == Op::CLR);
    CHECK(decode_words({0x4480}).op == Op::NEG);
    CHECK(decode_words({0x4080}).op == Op::NEGX);
    CHECK(decode_words({0x4680}).op == Op::NOT);

    Insn chk = decode_words({0x4181});
    CHECK(chk.op == Op::CHK);
    CHECK(chk.size == Size::Word);
    CHECK(chk.src.mode == Mode::Dn);
    CHECK_EQ(chk.src.reg, 1u);
    CHECK(chk.dst.mode == Mode::Dn);
    CHECK_EQ(chk.dst.reg, 0u);
}

void shifts_and_rotates() {
    Insn l = decode_words({0xE348});  // lsl.w #1,D0
    CHECK(l.op == Op::LSL);
    CHECK(l.size == Size::Word);
    CHECK_EQ(l.count, 1u);
    CHECK(l.src.mode == Mode::Imm);
    CHECK_EQ(l.src.value, 1u);
    CHECK(l.dst.mode == Mode::Dn);
    CHECK_EQ(l.dst.reg, 0u);

    Insn r = decode_words({0xE2A2});  // asr.l D1,D2
    CHECK(r.op == Op::ASR);
    CHECK(r.size == Size::Long);
    CHECK_EQ(r.count, 0u);
    CHECK(r.src.mode == Mode::Dn);
    CHECK_EQ(r.src.reg, 1u);
    CHECK_EQ(r.dst.reg, 2u);

    Insn eight = decode_words({0xE008});  // lsr.b #8,D0
    CHECK(eight.op == Op::LSR);
    CHECK_EQ(eight.count, 8u);

    Insn m = decode_words({0xE1D0});  // asl.w (A0)
    CHECK(m.op == Op::ASL);
    CHECK(m.size == Size::Word);
    CHECK_EQ(m.count, 1u);
    CHECK(m.src.mode == Mode::None);
    CHECK(m.dst.mode == Mode::AnInd);

    CHECK(decode_words({0xE1D0}).op == Op::ASL);
    CHECK(decode_words({0xE0D0}).op == Op::ASR);
    CHECK(decode_words({0xE3D0}).op == Op::LSL);
    CHECK(decode_words({0xE2D0}).op == Op::LSR);
    CHECK(decode_words({0xE5D0}).op == Op::ROXL);
    CHECK(decode_words({0xE4D0}).op == Op::ROXR);
    CHECK(decode_words({0xE7D0}).op == Op::ROL);
    CHECK(decode_words({0xE6D0}).op == Op::ROR);
}

void bad_fetches_fail() {
    Image image({0x303C});  // needs an immediate word that is not there
    Insn insn{};
    CHECK(!decode(image.memory(), 0, insn));
    CHECK(!decode(image.memory(), 1, insn));   // odd pc
    CHECK(!decode(image.memory(), 2, insn));   // beyond memory
    CHECK(!decode(image.memory(), 0xFFFFFFFEu, insn));
    GuestMemory empty{nullptr, 0};
    CHECK(!decode(empty, 0, insn));

    Image half({0x4E75});
    GuestMemory one{half.bytes.data(), 1};
    CHECK(!decode(one, 0, insn));
    Image tail({0x4E71, 0x4E75});
    CHECK(decode(tail.memory(), 2, insn));
    CHECK(insn.op == Op::RTS);
}

// Every table entry on its own: a sane length, operands only where there
// is an instruction, and no read outside a memory sized to the length.
void every_opcode_is_consistent() {
    decode_init();
    u8 pad[10] = {};
    for (u32 opcode = 0; opcode < 0x10000; ++opcode) {
        pad[0] = static_cast<u8>(opcode >> 8);
        pad[1] = static_cast<u8>(opcode);
        Insn insn{};
        CHECK(decode(GuestMemory{pad, 10}, 0, insn));
        CHECK_EQ(insn.opcode, opcode);
        CHECK(insn.words >= 1 && insn.words <= 5);
        if (insn.op == Op::Illegal || insn.op == Op::ALine || insn.op == Op::FLine) {
            CHECK_EQ(insn.words, 1u);
            CHECK(insn.src.mode == Mode::None);
            CHECK(insn.dst.mode == Mode::None);
        }
        if ((opcode >> 12) == 0xA) {
            CHECK(insn.op == Op::ALine);
        }
        if ((opcode >> 12) == 0xF) {
            CHECK(insn.op == Op::FLine);
        }

        // Exactly the instruction's own bytes decode; one word less does not.
        u32 bytes = insn.words * 2u;
        std::vector<u8> exact(pad, pad + bytes);
        Insn again{};
        CHECK(decode(GuestMemory{exact.data(), bytes}, 0, again));
        CHECK_EQ(again.words, insn.words);
        if (insn.words > 1) {
            Insn cut{};
            CHECK(!decode(GuestMemory{exact.data(), bytes - 2}, 0, cut));
        }
    }
}

u32 next_random(u32& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

struct Item {
    std::vector<u16> words;
};

// Runs objdump over the items and returns how many of them it agreed with
// before the first disagreement; sets bad to the disagreement, if any.
size_t run_objdump(const std::vector<Item>& items, size_t first, size_t count, std::string& bad) {
    char name[] = "/tmp/m68k_decode_XXXXXX";
    int fd = mkstemp(name);
    CHECK(fd >= 0);
    std::vector<u32> offsets;
    u32 offset = 0;
    std::vector<u8> bytes;
    for (size_t i = first; i < first + count; ++i) {
        offsets.push_back(offset);
        for (u16 w : items[i].words) {
            bytes.push_back(static_cast<u8>(w >> 8));
            bytes.push_back(static_cast<u8>(w));
            offset += 2;
        }
    }
    CHECK(write(fd, bytes.data(), bytes.size()) == static_cast<long>(bytes.size()));
    close(fd);

    std::string command = std::string("LC_ALL=C ") + kObjdump +
                          " -D -z -b binary -m m68k:68000 --no-show-raw-insn " + name;
    FILE* pipe = popen(command.c_str(), "r");
    CHECK(pipe != nullptr);
    std::vector<std::pair<u32, std::string>> printed;
    char line[512];
    while (fgets(line, sizeof line, pipe) != nullptr) {
        char* colon = nullptr;
        unsigned long address = std::strtoul(line, &colon, 16);
        if (colon == line || *colon != ':' || colon[1] != '\t') {
            continue;
        }
        printed.emplace_back(static_cast<u32>(address), std::string(colon + 2));
    }
    CHECK(pclose(pipe) == 0);
    unlink(name);

    for (size_t i = 0; i < count; ++i) {
        const Item& item = items[first + i];
        char note[160];
        if (i >= printed.size()) {
            std::snprintf(note, sizeof note, "opcode %04x: objdump stopped early", item.words[0]);
            bad = note;
            return i;
        }
        const std::string& text = printed[i].second;
        if (printed[i].first != offsets[i]) {
            std::snprintf(note, sizeof note, "after opcode %04x (%u words): objdump has the next "
                          "instruction at %x, we expect %x", items[first + i - (i > 0)].words[0],
                          static_cast<unsigned>(items[first + i - (i > 0)].words.size()),
                          printed[i].first, offsets[i]);
            bad = note;
            return i;
        }
        if (text.compare(0, 6, ".short") == 0 || text.compare(0, 5, ".word") == 0 ||
            text.compare(0, 5, "(bad)") == 0) {
            std::snprintf(note, sizeof note, "opcode %04x: objdump prints %s", item.words[0],
                          text.c_str());
            bad = note;
            return i;
        }
    }
    return count;
}

// Walks a seeded stream holding every opcode once and compares instruction
// boundaries with objdump. A disagreement ends the pass and the next one
// starts after the opcode involved, so one fault cannot hide another.
void lengths_agree_with_objdump() {
    if (access(kObjdump, X_OK) != 0) {
        std::fprintf(stderr, "test_m68k_decode: %s missing, objdump check skipped\n", kObjdump);
        return;
    }
    decode_init();
    u32 state = 0x68000u;
    std::vector<u32> order(0x10000);
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    for (u32 i = order.size() - 1; i > 0; --i) {
        u32 j = next_random(state) % (i + 1);
        u32 tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    std::vector<Item> items;
    for (u32 opcode : order) {
        // Index words with bit 8 set are the 68020 full format, which the
        // 68000 does not have, so the filler keeps that bit clear.
        u16 filler[5] = {static_cast<u16>(opcode)};
        for (int k = 1; k < 5; ++k) {
            filler[k] = static_cast<u16>(next_random(state)) & ~0x0100u;
        }
        u8 raw[10];
        for (int k = 0; k < 5; ++k) {
            raw[2 * k] = static_cast<u8>(filler[k] >> 8);
            raw[2 * k + 1] = static_cast<u8>(filler[k]);
        }
        Insn insn{};
        CHECK(decode(GuestMemory{raw, 10}, 0, insn));
        if (insn.op == Op::Illegal || insn.op == Op::ALine || insn.op == Op::FLine) {
            continue;
        }
        Item item;
        item.words.assign(filler, filler + insn.words);
        items.push_back(item);
    }
    CHECK(items.size() > 20000);

    constexpr size_t kChunk = 4096;
    size_t start = 0;
    size_t disagreements = 0;
    while (start < items.size()) {
        size_t count = items.size() - start < kChunk ? items.size() - start : kChunk;
        std::string bad;
        size_t agreed = run_objdump(items, start, count, bad);
        if (agreed == count) {
            start += count;
            continue;
        }
        if (disagreements++ < 20) {
            std::fprintf(stderr, "%s\n", bad.c_str());
        }
        start += agreed + 1;
    }
    CHECK_EQ(disagreements, 0u);
}

}

int main() {
    moveq_loads_a_sign_extended_byte();
    operand_free_instructions();
    move_with_immediate_source();
    movea_from_absolute_long();
    move_with_two_extension_sets();
    address_modes_without_extension_words();
    jumps_and_load_effective_address();
    line_a_line_f_and_illegal();
    reserved_and_later_models_are_illegal();
    quick_arithmetic();
    branches();
    bit_operations();
    immediate_arithmetic_and_status_register();
    multiple_register_moves();
    register_instructions();
    arithmetic_forms();
    shifts_and_rotates();
    bad_fetches_fail();
    every_opcode_is_consistent();
    lengths_agree_with_objdump();
    return check::finish("test_m68k_decode");
}
