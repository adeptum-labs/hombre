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
#include "x86_emit.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::x86;

namespace {

constexpr const char* kObjdump = "/usr/bin/objdump";

std::string normalise(const std::string& text) {
    std::string out;
    bool pending_space = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pending_space = true;
            continue;
        }
        if (c == ',') {
            out.push_back(',');
            pending_space = false;
            continue;
        }
        if (pending_space && !out.empty() && out.back() != ',') {
            out.push_back(' ');
        }
        pending_space = false;
        out.push_back(c);
    }
    return out;
}

bool same_bytes(const u8* actual, const u8* expected, usize count) {
    for (usize i = 0; i < count; ++i) {
        if (actual[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

// One call into the emitter and the instruction text objdump should print for
// it. A call that emits two instructions gives the second text too.
struct Step {
    const char* first;
    void (*emit)(Emitter&);
    const char* second = nullptr;
};

const Step kSteps[] = {
    {"je 0x6", [](Emitter& e) { e.jcc_rel32(Cc::E); }},
    {"jmp 0xb", [](Emitter& e) { e.jmp_rel32(); }},
    {"mov rax,rcx", [](Emitter& e) { e.mov_rr(RAX, RCX, 64); }},
    {"mov rsp,rbp", [](Emitter& e) { e.mov_rr(RSP, RBP, 64); }},
    {"mov r13,r15", [](Emitter& e) { e.mov_rr(R13, R15, 64); }},
    {"mov eax,ecx", [](Emitter& e) { e.mov_rr(RAX, RCX, 32); }},
    {"mov r12d,r8d", [](Emitter& e) { e.mov_rr(R12, R8, 32); }},
    {"mov bp,si", [](Emitter& e) { e.mov_rr(RBP, RSI, 16); }},
    {"mov r8w,r13w", [](Emitter& e) { e.mov_rr(R8, R13, 16); }},
    {"mov al,cl", [](Emitter& e) { e.mov_rr(RAX, RCX, 8); }},
    {"mov spl,bpl", [](Emitter& e) { e.mov_rr(RSP, RBP, 8); }},
    {"mov r15b,r12b", [](Emitter& e) { e.mov_rr(R15, R12, 8); }},
    {"mov eax,0x12345678", [](Emitter& e) { e.mov_ri(RAX, 0x12345678, 32); }},
    {"mov r8d,0xaabbccdd", [](Emitter& e) { e.mov_ri(R8, 0xAABBCCDD, 32); }},
    {"movabs rax,0x1122334455667788",
     [](Emitter& e) { e.mov_ri(RAX, 0x1122334455667788ull, 64); }},
    {"movabs rcx,0x1122334455667788",
     [](Emitter& e) { e.mov_ri(RCX, 0x1122334455667788ull, 64); }},
    {"movabs r15,0x1122334455667788",
     [](Emitter& e) { e.mov_ri(R15, 0x1122334455667788ull, 64); }},
    {"mov ax,0x1234", [](Emitter& e) { e.mov_ri(RAX, 0x1234, 16); }},
    {"mov r13w,0xabcd", [](Emitter& e) { e.mov_ri(R13, 0xABCD, 16); }},
    {"mov al,0x7f", [](Emitter& e) { e.mov_ri(RAX, 0x7F, 8); }},
    {"mov spl,0x80", [](Emitter& e) { e.mov_ri(RSP, 0x80, 8); }},
    {"mov r15b,0xfe", [](Emitter& e) { e.mov_ri(R15, 0xFE, 8); }},
    {"mov rax,QWORD PTR [rcx]", [](Emitter& e) { e.mov_load(RAX, RCX, 0, 64); }},
    {"mov rbp,QWORD PTR [rax]", [](Emitter& e) { e.mov_load(RBP, RAX, 0, 64); }},
    {"mov r13,QWORD PTR [rbp+0x0]", [](Emitter& e) { e.mov_load(R13, RBP, 0, 64); }},
    {"mov rsp,QWORD PTR [rsp]", [](Emitter& e) { e.mov_load(RSP, RSP, 0, 64); }},
    {"mov r12,QWORD PTR [r12]", [](Emitter& e) { e.mov_load(R12, R12, 0, 64); }},
    {"mov eax,DWORD PTR [rax+0x8]", [](Emitter& e) { e.mov_load(RAX, RAX, 8, 32); }},
    {"mov cx,WORD PTR [rbp-0x8]", [](Emitter& e) { e.mov_load(RCX, RBP, -8, 16); }},
    {"mov r8b,BYTE PTR [r12+0x40]", [](Emitter& e) { e.mov_load(R8, R12, 0x40, 8); }},
    {"mov esi,DWORD PTR [rax+0x12345678]",
     [](Emitter& e) { e.mov_load(RSI, RAX, 0x12345678, 32); }},
    {"mov eax,DWORD PTR [rbp+0x1234]", [](Emitter& e) { e.mov_load(RAX, RBP, 0x1234, 32); }},
    {"mov ecx,DWORD PTR [rsp+0x1234]", [](Emitter& e) { e.mov_load(RCX, RSP, 0x1234, 32); }},
    {"mov r15,QWORD PTR [r13-0x4]", [](Emitter& e) { e.mov_load(R15, R13, -4, 64); }},
    {"mov r8,QWORD PTR [r12+0x12345678]",
     [](Emitter& e) { e.mov_load(R8, R12, 0x12345678, 64); }},
    {"mov eax,DWORD PTR [r13+0x0]", [](Emitter& e) { e.mov_load(RAX, R13, 0, 32); }},
    {"mov QWORD PTR [rcx],rax", [](Emitter& e) { e.mov_store(RCX, 0, RAX, 64); }},
    {"mov DWORD PTR [rax+0x8],ecx", [](Emitter& e) { e.mov_store(RAX, 8, RCX, 32); }},
    {"mov WORD PTR [rbp+0x0],si", [](Emitter& e) { e.mov_store(RBP, 0, RSI, 16); }},
    {"mov WORD PTR [rsp+0x10],r8w", [](Emitter& e) { e.mov_store(RSP, 0x10, R8, 16); }},
    {"mov BYTE PTR [r12],r15b", [](Emitter& e) { e.mov_store(R12, 0, R15, 8); }},
    {"mov BYTE PTR [r13-0x1],al", [](Emitter& e) { e.mov_store(R13, -1, RAX, 8); }},
    {"mov DWORD PTR [rsi+0x12345678],esp",
     [](Emitter& e) { e.mov_store(RSI, 0x12345678, RSP, 32); }},
    {"mov QWORD PTR [r13+0x12345678],rax",
     [](Emitter& e) { e.mov_store(R13, 0x12345678, RAX, 64); }},
    {"mov rax,QWORD PTR [rcx+rdx*1]", [](Emitter& e) { e.mov_load_idx(RAX, RCX, RDX, 64); }},
    {"mov r8d,DWORD PTR [rsi+r12*1]", [](Emitter& e) { e.mov_load_idx(R8, RSI, R12, 32); }},
    {"mov r15,QWORD PTR [r13+r8*1+0x0]", [](Emitter& e) { e.mov_load_idx(R15, R13, R8, 64); }},
    {"mov cx,WORD PTR [rsp+rbp*1]", [](Emitter& e) { e.mov_load_idx(RCX, RSP, RBP, 16); }},
    {"mov al,BYTE PTR [r12+r15*1]", [](Emitter& e) { e.mov_load_idx(RAX, R12, R15, 8); }},
    {"mov rbp,QWORD PTR [rbp+rcx*1+0x0]",
     [](Emitter& e) { e.mov_load_idx(RBP, RBP, RCX, 64); }},
    {"mov r13d,DWORD PTR [r13+r8*1+0x0]",
     [](Emitter& e) { e.mov_load_idx(R13, R13, R8, 32); }},
    {"mov QWORD PTR [rcx+rdx*1],rax", [](Emitter& e) { e.mov_store_idx(RCX, RDX, RAX, 64); }},
    {"mov DWORD PTR [rax+rcx*1],esi", [](Emitter& e) { e.mov_store_idx(RAX, RCX, RSI, 32); }},
    {"mov WORD PTR [rsp+rbp*1],r8w", [](Emitter& e) { e.mov_store_idx(RSP, RBP, R8, 16); }},
    {"mov BYTE PTR [r12+r15*1],r13b",
     [](Emitter& e) { e.mov_store_idx(R12, R15, R13, 8); }},
    {"mov QWORD PTR [rbp+rax*1+0x0],rcx",
     [](Emitter& e) { e.mov_store_idx(RBP, RAX, RCX, 64); }},
    {"mov DWORD PTR [r13+rcx*1+0x0],r12d",
     [](Emitter& e) { e.mov_store_idx(R13, RCX, R12, 32); }},
    {"add rax,rcx", [](Emitter& e) { e.alu_rr(Alu::Add, RAX, RCX, 64); }},
    {"or esp,ebp", [](Emitter& e) { e.alu_rr(Alu::Or, RSP, RBP, 32); }},
    {"adc r13w,r15w", [](Emitter& e) { e.alu_rr(Alu::Adc, R13, R15, 16); }},
    {"sbb r8b,r12b", [](Emitter& e) { e.alu_rr(Alu::Sbb, R8, R12, 8); }},
    {"and rax,rsi", [](Emitter& e) { e.alu_rr(Alu::And, RAX, RSI, 64); }},
    {"sub ebp,eax", [](Emitter& e) { e.alu_rr(Alu::Sub, RBP, RAX, 32); }},
    {"xor r15w,r8w", [](Emitter& e) { e.alu_rr(Alu::Xor, R15, R8, 16); }},
    {"cmp sil,spl", [](Emitter& e) { e.alu_rr(Alu::Cmp, RSI, RSP, 8); }},
    {"add rax,0x1", [](Emitter& e) { e.alu_ri(Alu::Add, RAX, 1, 64); }},
    {"add ecx,0x12345678", [](Emitter& e) { e.alu_ri(Alu::Add, RCX, 0x12345678, 32); }},
    {"or r8,0x7f", [](Emitter& e) { e.alu_ri(Alu::Or, R8, 0x7F, 64); }},
    {"adc esp,0x7f", [](Emitter& e) { e.alu_ri(Alu::Adc, RSP, 0x7F, 32); }},
    {"sbb r13w,0x1234", [](Emitter& e) { e.alu_ri(Alu::Sbb, R13, 0x1234, 16); }},
    {"sbb rbp,0x1234", [](Emitter& e) { e.alu_ri(Alu::Sbb, RBP, 0x1234, 64); }},
    {"and r12b,0x55", [](Emitter& e) { e.alu_ri(Alu::And, R12, 0x55, 8); }},
    {"sub al,0x64", [](Emitter& e) { e.alu_ri(Alu::Sub, RAX, 0x64, 8); }},
    {"xor r15d,0x12345678", [](Emitter& e) { e.alu_ri(Alu::Xor, R15, 0x12345678, 32); }},
    {"cmp rsi,0x7f", [](Emitter& e) { e.alu_ri(Alu::Cmp, RSI, 0x7F, 64); }},
    {"shl rax,0x1", [](Emitter& e) { e.shift_i(Shift::Shl, RAX, 1, 64); }},
    {"shr r13d,0x4", [](Emitter& e) { e.shift_i(Shift::Shr, R13, 4, 32); }},
    {"sar r8w,0x3", [](Emitter& e) { e.shift_i(Shift::Sar, R8, 3, 16); }},
    {"rol sp,0x8", [](Emitter& e) { e.shift_i(Shift::Rol, RSP, 8, 16); }},
    {"ror r15b,0x2", [](Emitter& e) { e.shift_i(Shift::Ror, R15, 2, 8); }},
    {"rcl sil,0x1", [](Emitter& e) { e.shift_i(Shift::Rcl, RSI, 1, 8); }},
    {"rcr r12,0x5", [](Emitter& e) { e.shift_i(Shift::Rcr, R12, 5, 64); }},
    {"shl rax,cl", [](Emitter& e) { e.shift_cl(Shift::Shl, RAX, 64); }},
    {"shr r13d,cl", [](Emitter& e) { e.shift_cl(Shift::Shr, R13, 32); }},
    {"sar r12w,cl", [](Emitter& e) { e.shift_cl(Shift::Sar, R12, 16); }},
    {"rol spl,cl", [](Emitter& e) { e.shift_cl(Shift::Rol, RSP, 8); }},
    {"ror r15b,cl", [](Emitter& e) { e.shift_cl(Shift::Ror, R15, 8); }},
    {"bswap eax", [](Emitter& e) { e.bswap(RAX, 32); }},
    {"bswap r13", [](Emitter& e) { e.bswap(R13, 64); }},
    {"rol ax,0x8", [](Emitter& e) { e.bswap(RAX, 16); }},
    {"movzx eax,cl", [](Emitter& e) { e.movzx(RAX, RCX, 8); }},
    {"movzx r8d,r12b", [](Emitter& e) { e.movzx(R8, R12, 8); }},
    {"movzx esi,spl", [](Emitter& e) { e.movzx(RSI, RSP, 8); }},
    {"movzx ecx,bp", [](Emitter& e) { e.movzx(RCX, RBP, 16); }},
    {"movzx r15d,r13w", [](Emitter& e) { e.movzx(R15, R13, 16); }},
    {"movsx eax,cl", [](Emitter& e) { e.movsx(RAX, RCX, 8); }},
    {"movsx r12d,r8b", [](Emitter& e) { e.movsx(R12, R8, 8); }},
    {"movsx ebp,spl", [](Emitter& e) { e.movsx(RBP, RSP, 8); }},
    {"movsx esi,r15w", [](Emitter& e) { e.movsx(RSI, R15, 16); }},
    {"movsxd r15,r13d", [](Emitter& e) { e.movsx(R15, R13, 32); }},
    {"sete al", [](Emitter& e) { e.setcc(Cc::E, RAX); }},
    {"setne r8b", [](Emitter& e) { e.setcc(Cc::NE, R8); }},
    {"setl spl", [](Emitter& e) { e.setcc(Cc::L, RSP); }},
    {"setae r13b", [](Emitter& e) { e.setcc(Cc::AE, R13); }},
    {"setg sil", [](Emitter& e) { e.setcc(Cc::G, RSI); }},
    {"movabs rax,0x1122334455667788", [](Emitter& e) { e.call_abs(0x1122334455667788ull); },
     "call rax"},
    {"push rax", [](Emitter& e) { e.push(RAX); }},
    {"push r12", [](Emitter& e) { e.push(R12); }},
    {"pop r15", [](Emitter& e) { e.pop(R15); }},
    {"pop rbp", [](Emitter& e) { e.pop(RBP); }},
    {"ret", [](Emitter& e) { e.ret(); }},
};

void known_bytes() {
    u8 bytes[16] = {};

    const u8 movabs[] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    Emitter e(bytes, sizeof bytes);
    e.mov_ri(RAX, 0x1122334455667788ull, 64);
    CHECK(e.ok());
    CHECK(e.cursor() == bytes + e.size());
    CHECK(same_bytes(bytes, movabs, sizeof movabs));
    CHECK_EQ(e.size(), sizeof movabs);

    const u8 push_r12[] = {0x41, 0x54};
    e = Emitter(bytes, sizeof bytes);
    e.push(R12);
    CHECK(same_bytes(bytes, push_r12, sizeof push_r12));
    CHECK_EQ(e.size(), sizeof push_r12);

    const u8 ret_code[] = {0xC3};
    e = Emitter(bytes, sizeof bytes);
    e.ret();
    CHECK(same_bytes(bytes, ret_code, sizeof ret_code));
    CHECK_EQ(e.size(), sizeof ret_code);

    const u8 call[] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xD0};
    e = Emitter(bytes, sizeof bytes);
    e.call_abs(0x1122334455667788ull);
    CHECK(e.ok());
    CHECK(same_bytes(bytes, call, sizeof call));
    CHECK_EQ(e.size(), sizeof call);
}

void relative_fields() {
    u8 bytes[32] = {};
    Emitter e(bytes, sizeof bytes);
    const usize at = e.jcc_rel32(Cc::E);
    CHECK_EQ(at, usize(2));
    e.mov_ri(RAX, 0, 32);  // five bytes between the field and the cursor
    e.patch(at);
    const u8 patched[] = {0x0F, 0x84, 0x05, 0x00, 0x00, 0x00};
    CHECK(same_bytes(bytes, patched, sizeof patched));

    Emitter j(bytes, sizeof bytes);
    CHECK_EQ(j.jmp_rel32(), usize(1));
}

void overflow_is_sticky() {
    u8 guard[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    Emitter e(guard, 3);
    e.mov_ri(RAX, 0x1122334455667788ull, 64);
    CHECK(!e.ok());
    CHECK_EQ(guard[3], 0xAAu);
    e.ret();
    CHECK(!e.ok());
    CHECK_EQ(guard[3], 0xAAu);
}

void disassembly_matches_objdump() {
    if (access(kObjdump, X_OK) != 0) {
        std::fprintf(stderr, "test_x86_emit: %s missing, objdump check skipped\n", kObjdump);
        return;
    }

    std::vector<u8> memory(4096);
    Emitter emitter(memory.data(), memory.size());
    std::vector<std::string> expected;
    for (const Step& step : kSteps) {
        step.emit(emitter);
        CHECK(emitter.ok());
        expected.push_back(normalise(step.first));
        if (step.second != nullptr) {
            expected.push_back(normalise(step.second));
        }
    }

    char name[] = "/tmp/x86_emit_XXXXXX";
    int fd = mkstemp(name);
    CHECK(fd >= 0);
    CHECK(write(fd, memory.data(), emitter.size()) == static_cast<long>(emitter.size()));
    close(fd);

    std::string command =
        std::string("LC_ALL=C ") + kObjdump + " -D -b binary -m i386:x86-64 -M intel " + name;
    FILE* pipe = popen(command.c_str(), "r");
    CHECK(pipe != nullptr);
    std::vector<std::string> printed;
    char line[512];
    while (std::fgets(line, sizeof line, pipe) != nullptr) {
        char* bytes_field = std::strchr(line, '\t');
        if (bytes_field == nullptr) {
            continue;
        }
        char* text = std::strchr(bytes_field + 1, '\t');
        if (text == nullptr) {
            continue;  // a continuation line carrying only bytes
        }
        printed.push_back(normalise(text + 1));
    }
    CHECK(pclose(pipe) == 0);
    unlink(name);

    for (usize i = 0; i < expected.size(); ++i) {
        const bool same = i < printed.size() && printed[i] == expected[i];
        if (!same) {
            std::fprintf(stderr, "instruction %zu: objdump \"%s\", expected \"%s\"\n", i,
                         i < printed.size() ? printed[i].c_str() : "<none>", expected[i].c_str());
        }
        CHECK(same);
    }
    CHECK_EQ(printed.size(), expected.size());
}

}

int main() {
    known_bytes();
    relative_fields();
    overflow_is_sticky();
    disassembly_matches_objdump();
    return check::finish("test_x86_emit");
}
