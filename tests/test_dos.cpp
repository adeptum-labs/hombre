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

#include "libs/dos.hpp"
#include "libs/timer_device.hpp"
#include "check.hpp"
#include "ofs_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;

namespace {

using i32 = std::int32_t;

constexpr u32 kCall = 0x1800;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;    // test structures up to 0xBFFF
constexpr u32 kArray = 0xC000;   // ReadArgs result arrays
constexpr u32 kOut = 0xC800;     // what test commands store
constexpr u32 kTemplate = 0xD000;
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;
constexpr u64 kBlocks = 1000000;

// Static storage, as the kernel keeps them.
Jit g_jit;
Exec g_exec;
TimerDevice g_timer;
Dos g_dos;
std::vector<std::string> g_lines;
std::string g_console;

void capture(const char* line) {
    g_lines.push_back(line);
}

void console(void* ctx, const u8* data, u32 bytes) {
    CHECK(ctx == &g_console);
    g_console.append(reinterpret_cast<const char*>(data), bytes);
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

// A little 68000 assembler for the test commands: code, then the strings
// it takes the address of with LEA d16(PC).
class Asm {
public:
    Asm& op(std::initializer_list<u16> words) {
        for (u16 w : words) {
            words_.push_back(w);
        }
        return *this;
    }

    Asm& label(const std::string& name) {
        labels_[name] = static_cast<u32>(words_.size() * 2);
        return *this;
    }

    // LEA label(PC),An
    Asm& lea(unsigned areg, const std::string& name) {
        words_.push_back(static_cast<u16>(0x41FA | (areg << 9)));
        fixups_.emplace_back(words_.size(), name);
        words_.push_back(0);
        return *this;
    }

    // LEA label(PC),A0 ; MOVE.L A0,Dn
    Asm& address(unsigned dreg, const std::string& name) {
        lea(0, name);
        return op({static_cast<u16>(0x2008 | (dreg << 9))});
    }

    Asm& bsr(const std::string& name) {
        words_.push_back(0x6100);
        fixups_.emplace_back(words_.size(), name);
        words_.push_back(0);
        return *this;
    }

    Asm& moveq(i32 value, unsigned dreg) {
        return op({static_cast<u16>(0x7000 | (dreg << 9) | (static_cast<u32>(value) & 0xFF))});
    }

    Asm& move_l(u32 value, unsigned dreg) {
        return op({static_cast<u16>(0x203C | (dreg << 9)), static_cast<u16>(value >> 16),
                   static_cast<u16>(value)});
    }

    // MOVE.L D0,Dn
    Asm& from_d0(unsigned dreg) { return op({static_cast<u16>(0x2000 | (dreg << 9))}); }

    // MOVE.L D0,addr.L
    Asm& store_d0(u32 addr) {
        return op({0x23C0, static_cast<u16>(addr >> 16), static_cast<u16>(addr)});
    }

    // MOVE.L #value,addr.L
    Asm& store_imm(u32 value, u32 addr) {
        return op({0x23FC, static_cast<u16>(value >> 16), static_cast<u16>(value),
                   static_cast<u16>(addr >> 16), static_cast<u16>(addr)});
    }

    // A6 = OpenLibrary("dos.library", 0).
    Asm& open_dos() {
        string("dosname", "dos.library");
        op({0x2C78, 0x0004});
        lea(1, "dosname");
        moveq(0, 0);
        return op({0x4EAE, disp(552), 0x2C40});
    }

    Asm& dos(u32 lvo) { return op({0x4EAE, disp(lvo)}); }
    Asm& rts() { return op({0x4E75}); }

    Asm& string(const std::string& name, const std::string& text) {
        if (strings_.count(name) == 0) {
            strings_[name] = text;
        }
        return *this;
    }

    std::vector<u8> build() const {
        std::vector<u8> out;
        for (u16 w : words_) {
            out.push_back(static_cast<u8>(w >> 8));
            out.push_back(static_cast<u8>(w));
        }
        std::map<std::string, u32> where = labels_;
        for (const auto& [name, text] : strings_) {
            where[name] = static_cast<u32>(out.size());
            out.insert(out.end(), text.begin(), text.end());
            out.push_back(0);
            if (out.size() % 2 != 0) {
                out.push_back(0);
            }
        }
        for (const auto& [index, name] : fixups_) {
            CHECK(where.count(name) == 1);
            const i64 d = static_cast<i64>(where[name]) - static_cast<i64>(index * 2);
            out[index * 2] = static_cast<u8>(static_cast<u16>(d) >> 8);
            out[index * 2 + 1] = static_cast<u8>(d);
        }
        return out;
    }

private:
    std::vector<u16> words_;
    std::map<std::string, u32> labels_;
    std::map<std::string, std::string> strings_;
    std::vector<std::pair<usize, std::string>> fixups_;
};

void put_be32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v >> 24));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

// A one-hunk executable: HUNK_HEADER, HUNK_CODE, HUNK_END.
std::vector<u8> hunk_file(std::vector<u8> code) {
    while (code.size() % 4 != 0) {
        code.push_back(0);
    }
    const u32 longs = static_cast<u32>(code.size() / 4);
    std::vector<u8> out;
    for (u32 v : {1011u, 0u, 1u, 0u, 0u, longs, 1001u, longs}) {
        put_be32(out, v);
    }
    out.insert(out.end(), code.begin(), code.end());
    put_be32(out, 1010);
    return out;
}

std::vector<u8> text(const std::string& s) {
    return std::vector<u8>(s.begin(), s.end());
}

// The bytes of C/Echo on the test volume: more than one OFS data block.
std::vector<u8> echo_bytes() {
    std::vector<u8> out;
    for (u32 i = 0; i < 1000; ++i) {
        out.push_back(static_cast<u8>(i * 7 + 3));
    }
    return out;
}

// Writes "hi\n" to Output() and returns 3.
std::vector<u8> hello_command() {
    Asm a;
    a.open_dos().dos(dos_lvo::Output).from_d0(1).address(2, "hi").moveq(3, 3).dos(dos_lvo::Write);
    a.moveq(3, 0).rts().string("hi", "hi\n");
    return hunk_file(a.build());
}

// ReadArgs(kTemplate, kArray, 0) on the command line: stores the result
// at kOut and IoErr at kOut + 4, returns 0.
std::vector<u8> args_command() {
    Asm a;
    a.open_dos().move_l(kTemplate, 1).move_l(kArray, 2).moveq(0, 3).dos(dos_lvo::ReadArgs);
    a.store_d0(kOut).dos(dos_lvo::IoErr).store_d0(kOut + 4).moveq(0, 0).rts();
    return hunk_file(a.build());
}

// Calls Exit(7) from a subroutine, and would return 1.
std::vector<u8> exit_command() {
    Asm a;
    a.open_dos().bsr("sub").moveq(1, 0).rts();
    a.label("sub").moveq(7, 1).dos(dos_lvo::Exit).moveq(2, 0).rts();
    return hunk_file(a.build());
}

// Calls a private dos vector.
std::vector<u8> unknown_command() {
    Asm a;
    a.open_dos().dos(162).moveq(0, 0).rts();
    return hunk_file(a.build());
}

// Stores A0, D0, 4(A7), A7 and (A7) at kOut.
std::vector<u8> entry_command() {
    Asm a;
    static_assert(kOut == 0xC800);
    a.op({0x23C8, 0x0000, 0xC800});          // MOVE.L A0,kOut
    a.store_d0(kOut + 4);                    // D0
    a.op({0x23EF, 0x0004, 0x0000, 0xC808});  // MOVE.L 4(A7),kOut+8
    a.op({0x23CF, 0x0000, 0xC80C});          // MOVE.L A7,kOut+12
    a.op({0x23EF, 0x0000, 0x0000, 0xC810});  // MOVE.L 0(A7),kOut+16
    a.moveq(0, 0).rts();
    return hunk_file(a.build());
}

// Lowers its stack pointer over a 64-byte live frame of pattern, then runs
// C:Hello through RunCommand; the frame address goes to kOut + 8 and the
// result to kOut + 4.
std::vector<u8> run_stack_command() {
    Asm a;
    a.open_dos();
    a.op({0x4FEF, 0xFFC0});  // LEA -64(A7),A7
    a.op({0x23CF, static_cast<u16>((kOut + 8) >> 16), static_cast<u16>(kOut + 8)});  // A7
    a.op({0x41EF, 0x0000});  // LEA (A7),A0
    for (u32 i = 0; i < 16; ++i) {
        const u32 v = 0xC0DE0000u | i;
        a.op({0x20FC, static_cast<u16>(v >> 16), static_cast<u16>(v)});  // MOVE.L #v,(A0)+
    }
    a.address(1, "hello").dos(dos_lvo::LoadSeg).from_d0(1);
    a.address(2, "args").moveq(0, 3).dos(dos_lvo::RunCommand);
    a.store_d0(kOut + 4);
    a.op({0x4FEF, 0x0040});  // LEA 64(A7),A7
    a.moveq(0, 0).rts().string("hello", "C:Hello").string("args", "");
    return hunk_file(a.build());
}

// Runs itself through RunCommand until exec refuses to nest any deeper.
std::vector<u8> nest_command() {
    Asm a;
    a.open_dos();
    a.address(1, "nest").dos(dos_lvo::LoadSeg).from_d0(1);
    a.address(2, "args").moveq(0, 3).dos(dos_lvo::RunCommand);
    a.rts().string("nest", "C:Nest").string("args", "");
    return hunk_file(a.build());
}

// Opens a console handle and starts a child on it with the close flag set,
// so the handle belongs to the child from then on.
std::vector<u8> spawn_with_own_handle_command() {
    const u32 tags = kData + 0x700;
    Asm a;
    a.open_dos();
    a.address(1, "star").move_l(MODE_OLDFILE, 2).dos(dos_lvo::Open);  // D0 = handle
    a.store_d0(kOut);
    a.store_d0(tags + 20);  // NP_Output's data
    a.address(1, "hello").dos(dos_lvo::LoadSeg);  // D0 = seglist
    a.store_d0(kOut + 4);
    a.store_d0(tags + 4);  // NP_Seglist's data
    a.store_imm(NP_Seglist, tags);
    a.store_imm(NP_FreeSeglist, tags + 8);
    a.store_imm(DOSTRUE, tags + 12);
    a.store_imm(NP_Output, tags + 16);
    a.store_imm(NP_CloseOutput, tags + 24);
    a.store_imm(DOSTRUE, tags + 28);
    a.store_imm(TAG_DONE, tags + 32);
    a.store_imm(0, tags + 36);
    a.move_l(tags, 1).dos(dos_lvo::CreateNewProc);
    a.store_d0(kOut + 8);
    a.moveq(0, 0).rts().string("star", "*").string("hello", "C:Hello");
    return hunk_file(a.build());
}

std::vector<AdfFile> standard_files() {
    return {
        {"C/Echo", echo_bytes()},
        {"C/Hello", hello_command()},
        {"C/Args", args_command()},
        {"C/Exit", exit_command()},
        {"C/Unknown", unknown_command()},
        {"C/Entry", entry_command()},
        {"C/Empty", {}},
        {"LIBS/x", text("not a hunk file at all")},
        {"LIBS/x.library", text("not a hunk file at all")},
        {"S/Startup-Sequence", text("echo hello\n")},
        {"Devs/Sub/Deep", text("deep")},
    };
}

// One memfd mapped twice for the code region, 16 MiB of guest memory, the
// Jit, exec and dos initialised over them, the log and the console
// captured.
class Rig {
public:
    explicit Rig(const std::vector<AdfFile>& files = standard_files(), bool ffs = false)
        : Rig(build_adf("HombreTest", ffs, files)) {}

    // The first image in DF0:, the second in drive second_drive.
    Rig(std::vector<u8> image, std::vector<u8> second, u32 second_drive)
        : Rig(std::move(image), kFreeEnd, std::move(second), second_drive) {}

    explicit Rig(std::vector<u8> image, u32 free_end = kFreeEnd, std::vector<u8> second = {},
                 u32 second_drive = 1)
        : memory_(kGuestBytes), image_(std::move(image)), second_image_(std::move(second)) {
        fd_ = memfd_create("dos", 0);
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
        g_console.clear();
        g_log = capture;
        CHECK(g_exec.init(m, kFreeStart, free_end));
        CHECK(g_timer.init(m, g_exec));
        CHECK(adf_mount(image_.data(), image_.size(), volume) == FsError::Ok);
        if (second_image_.empty()) {
            CHECK(g_dos.init(m, g_exec, volume, console, &g_console));
        } else {
            CHECK(adf_mount(second_image_.data(), second_image_.size(), second_volume) ==
                  FsError::Ok);
            Drives drives;
            drives.volume[0] = &volume;
            drives.volume[second_drive] = &second_volume;
            CHECK(g_dos.init(m, g_exec, drives, console, &g_console));
        }
        m.cpu.a[7] = kStack;
    }

    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_segment_loader(SegmentLoader{nullptr, nullptr});
        g_exec.set_task_host(TaskHost{});
        g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;
    AdfVolume volume{};
    AdfVolume second_volume{};

    CpuState& cpu() { return m.cpu; }
    u32 base() const { return g_dos.dos_base(); }
    u32 proc() const { return g_dos.process(); }

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

    u32 str(u32 at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            put8(at + static_cast<u32>(i), static_cast<u8>(s[i]));
        }
        put8(at + static_cast<u32>(s.size()), 0);
        return at;
    }

    std::string read_str(u32 at) const {
        std::string s;
        while (memory_[at] != 0 && s.size() < 4096) {
            s += static_cast<char>(memory_[at++]);
        }
        return s;
    }

    std::string read_bstr(u32 bptr) const {
        const u32 at = bptr << 2;
        std::string s;
        for (u32 i = 0; i < get8(at); ++i) {
            s += static_cast<char>(get8(at + 1 + i));
        }
        return s;
    }

    std::vector<u8> bytes(u32 at, u32 count) const {
        return std::vector<u8>(memory_.begin() + at, memory_.begin() + at + count);
    }

    // JSR -lvo(A6) with A6 = DOSBase, from hand-written 68000 code.
    ExitReason dos(u32 lvo) {
        put16(kCall, 0x4EAE);
        put16(kCall + 2, disp(lvo));
        put16(kCall + 4, 0x4EF8);  // JMP $400.w
        put16(kCall + 6, 0x0400);
        g_jit.flush();
        m.cpu.a[6] = base();
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCall;
        return g_jit.run(kBlocks);
    }

    ExitReason exec(u32 lvo) {
        put16(kCall, 0x4EAE);
        put16(kCall + 2, disp(lvo));
        put16(kCall + 4, 0x4EF8);  // JMP $400.w
        put16(kCall + 6, 0x0400);
        g_jit.flush();
        m.cpu.a[6] = g_exec.exec_base();
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCall;
        return g_jit.run(kBlocks);
    }

    // D1..D7 set, the vector called, D0 returned; the run must halt.
    u32 call(u32 lvo, u32 d1 = 0, u32 d2 = 0, u32 d3 = 0, u32 d4 = 0, u32 d5 = 0, u32 d6 = 0,
             u32 d7 = 0) {
        m.cpu.d[1] = d1;
        m.cpu.d[2] = d2;
        m.cpu.d[3] = d3;
        m.cpu.d[4] = d4;
        m.cpu.d[5] = d5;
        m.cpu.d[6] = d6;
        m.cpu.d[7] = d7;
        CHECK_EQ(dos(lvo), ExitReason::Halt);
        return m.cpu.d[0];
    }

    u32 io_err() { return call(dos_lvo::IoErr); }

    void set_io_err(u32 code) { call(dos_lvo::SetIoErr, code); }

    // The free chunks of exec's memory header as (address, bytes).
    std::vector<std::pair<u32, u32>> chunks() const {
        const u32 header = get32(g_exec.exec_base() + MemList + LH_HEAD);
        std::vector<std::pair<u32, u32>> out;
        for (u32 c = get32(header + MH_FIRST); c != 0 && out.size() < 1000; c = get32(c)) {
            out.emplace_back(c, get32(c + MC_BYTES));
        }
        return out;
    }

private:
    static constexpr usize kRegion = 1 << 20;
    std::vector<u8> memory_;
    std::vector<u8> image_;
    std::vector<u8> second_image_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

i32 as_signed(u32 v) {
    return static_cast<i32>(v);
}

// Where the FileHandle a BPTR names sits; the generation is above bit 22.
u32 handle_at(u32 bptr) { return (bptr & 0x003FFFFFu) << 2; }

// --- layout -------------------------------------------------------------

void layout_matches_the_ndk() {
    CHECK_EQ(dos_lvo::Open, 30u);
    CHECK_EQ(dos_lvo::Close, 36u);
    CHECK_EQ(dos_lvo::Read, 42u);
    CHECK_EQ(dos_lvo::Write, 48u);
    CHECK_EQ(dos_lvo::Input, 54u);
    CHECK_EQ(dos_lvo::Output, 60u);
    CHECK_EQ(dos_lvo::Seek, 66u);
    CHECK_EQ(dos_lvo::DeleteFile, 72u);
    CHECK_EQ(dos_lvo::Rename, 78u);
    CHECK_EQ(dos_lvo::Lock, 84u);
    CHECK_EQ(dos_lvo::UnLock, 90u);
    CHECK_EQ(dos_lvo::DupLock, 96u);
    CHECK_EQ(dos_lvo::Examine, 102u);
    CHECK_EQ(dos_lvo::ExNext, 108u);
    CHECK_EQ(dos_lvo::Info, 114u);
    CHECK_EQ(dos_lvo::CreateDir, 120u);
    CHECK_EQ(dos_lvo::CurrentDir, 126u);
    CHECK_EQ(dos_lvo::IoErr, 132u);
    CHECK_EQ(dos_lvo::CreateProc, 138u);
    CHECK_EQ(dos_lvo::Exit, 144u);
    CHECK_EQ(dos_lvo::LoadSeg, 150u);
    CHECK_EQ(dos_lvo::UnLoadSeg, 156u);
    CHECK_EQ(dos_lvo::SetComment, 180u);
    CHECK_EQ(dos_lvo::SetProtection, 186u);
    CHECK_EQ(dos_lvo::DateStamp, 192u);
    CHECK_EQ(dos_lvo::Delay, 198u);
    CHECK_EQ(dos_lvo::WaitForChar, 204u);
    CHECK_EQ(dos_lvo::ParentDir, 210u);
    CHECK_EQ(dos_lvo::IsInteractive, 216u);
    CHECK_EQ(dos_lvo::Execute, 222u);
    CHECK_EQ(dos_lvo::AllocDosObject, 228u);
    CHECK_EQ(dos_lvo::FreeDosObject, 234u);
    CHECK_EQ(dos_lvo::DoPkt, 240u);
    CHECK_EQ(dos_lvo::SelectInput, 294u);
    CHECK_EQ(dos_lvo::SelectOutput, 300u);
    CHECK_EQ(dos_lvo::FGetC, 306u);
    CHECK_EQ(dos_lvo::FPutC, 312u);
    CHECK_EQ(dos_lvo::UnGetC, 318u);
    CHECK_EQ(dos_lvo::FRead, 324u);
    CHECK_EQ(dos_lvo::FWrite, 330u);
    CHECK_EQ(dos_lvo::FGets, 336u);
    CHECK_EQ(dos_lvo::FPuts, 342u);
    CHECK_EQ(dos_lvo::VFWritef, 348u);
    CHECK_EQ(dos_lvo::VFPrintf, 354u);
    CHECK_EQ(dos_lvo::Flush, 360u);
    CHECK_EQ(dos_lvo::SetVBuf, 366u);
    CHECK_EQ(dos_lvo::DupLockFromFH, 372u);
    CHECK_EQ(dos_lvo::OpenFromLock, 378u);
    CHECK_EQ(dos_lvo::ParentOfFH, 384u);
    CHECK_EQ(dos_lvo::ExamineFH, 390u);
    CHECK_EQ(dos_lvo::SetFileDate, 396u);
    CHECK_EQ(dos_lvo::NameFromLock, 402u);
    CHECK_EQ(dos_lvo::NameFromFH, 408u);
    CHECK_EQ(dos_lvo::SameLock, 420u);
    CHECK_EQ(dos_lvo::SetMode, 426u);
    CHECK_EQ(dos_lvo::ExAll, 432u);
    CHECK_EQ(dos_lvo::SetFileSize, 456u);
    CHECK_EQ(dos_lvo::SetIoErr, 462u);
    CHECK_EQ(dos_lvo::Fault, 468u);
    CHECK_EQ(dos_lvo::PrintFault, 474u);
    CHECK_EQ(dos_lvo::ErrorReport, 480u);
    CHECK_EQ(dos_lvo::Cli, 492u);
    CHECK_EQ(dos_lvo::CreateNewProc, 498u);
    CHECK_EQ(dos_lvo::RunCommand, 504u);
    CHECK_EQ(dos_lvo::GetFileSysTask, 522u);
    CHECK_EQ(dos_lvo::SetFileSysTask, 528u);
    CHECK_EQ(dos_lvo::GetArgStr, 534u);
    CHECK_EQ(dos_lvo::SetArgStr, 540u);
    CHECK_EQ(dos_lvo::FindCliProc, 546u);
    CHECK_EQ(dos_lvo::MaxCli, 552u);
    CHECK_EQ(dos_lvo::SetCurrentDirName, 558u);
    CHECK_EQ(dos_lvo::GetCurrentDirName, 564u);
    CHECK_EQ(dos_lvo::SetProgramName, 570u);
    CHECK_EQ(dos_lvo::GetProgramName, 576u);
    CHECK_EQ(dos_lvo::SetPrompt, 582u);
    CHECK_EQ(dos_lvo::GetPrompt, 588u);
    CHECK_EQ(dos_lvo::SystemTagList, 606u);
    CHECK_EQ(dos_lvo::AssignLock, 612u);
    CHECK_EQ(dos_lvo::AssignLate, 618u);
    CHECK_EQ(dos_lvo::AssignPath, 624u);
    CHECK_EQ(dos_lvo::AssignAdd, 630u);
    CHECK_EQ(dos_lvo::RemAssignList, 636u);
    CHECK_EQ(dos_lvo::GetDeviceProc, 642u);
    CHECK_EQ(dos_lvo::FreeDeviceProc, 648u);
    CHECK_EQ(dos_lvo::LockDosList, 654u);
    CHECK_EQ(dos_lvo::UnLockDosList, 660u);
    CHECK_EQ(dos_lvo::AttemptLockDosList, 666u);
    CHECK_EQ(dos_lvo::FindDosEntry, 684u);
    CHECK_EQ(dos_lvo::NextDosEntry, 690u);
    CHECK_EQ(dos_lvo::IsFileSystem, 708u);
    CHECK_EQ(dos_lvo::CompareDates, 738u);
    CHECK_EQ(dos_lvo::DateToStr, 744u);
    CHECK_EQ(dos_lvo::StrToDate, 750u);
    CHECK_EQ(dos_lvo::NewLoadSeg, 768u);
    CHECK_EQ(dos_lvo::FindSegment, 780u);
    CHECK_EQ(dos_lvo::CheckSignal, 792u);
    CHECK_EQ(dos_lvo::ReadArgs, 798u);
    CHECK_EQ(dos_lvo::FindArg, 804u);
    CHECK_EQ(dos_lvo::ReadItem, 810u);
    CHECK_EQ(dos_lvo::StrToLong, 816u);
    CHECK_EQ(dos_lvo::MatchFirst, 822u);
    CHECK_EQ(dos_lvo::MatchNext, 828u);
    CHECK_EQ(dos_lvo::MatchEnd, 834u);
    CHECK_EQ(dos_lvo::ParsePattern, 840u);
    CHECK_EQ(dos_lvo::MatchPattern, 846u);
    CHECK_EQ(dos_lvo::FreeArgs, 858u);
    CHECK_EQ(dos_lvo::FilePart, 870u);
    CHECK_EQ(dos_lvo::PathPart, 876u);
    CHECK_EQ(dos_lvo::AddPart, 882u);
    CHECK_EQ(dos_lvo::SetVar, 900u);
    CHECK_EQ(dos_lvo::GetVar, 906u);
    CHECK_EQ(dos_lvo::DeleteVar, 912u);
    CHECK_EQ(dos_lvo::FindVar, 918u);
    CHECK_EQ(dos_lvo::WriteChars, 942u);
    CHECK_EQ(dos_lvo::PutStr, 948u);
    CHECK_EQ(dos_lvo::VPrintf, 954u);
    CHECK_EQ(dos_lvo::ParsePatternNoCase, 966u);
    CHECK_EQ(dos_lvo::MatchPatternNoCase, 972u);
    CHECK_EQ(dos_lvo::ExAllEnd, 990u);
    CHECK_EQ(dos_lvo::SetOwner, 996u);

    // Task, MsgPort, MinList, SignalSemaphore, DateStamp.
    CHECK_EQ(TC_STATE, 15u);
    CHECK_EQ(TC_SPREG, 54u);
    CHECK_EQ(TC_SPLOWER, 58u);
    CHECK_EQ(TC_SPUPPER, 62u);
    CHECK_EQ(TC_MEMENTRY, 74u);
    CHECK_EQ(MP_FLAGS, LN_SIZE);
    CHECK_EQ(MP_SIGBIT, MP_FLAGS + 1);
    CHECK_EQ(MP_SIGTASK, MP_SIGBIT + 1);
    CHECK_EQ(MP_MSGLIST, MP_SIGTASK + 4);
    CHECK_EQ(MP_SIZE, MP_MSGLIST + LH_SIZE);
    CHECK_EQ(MLH_SIZE, 12u);
    CHECK_EQ(SS_NESTCOUNT, LN_SIZE);
    CHECK_EQ(SS_WAITQUEUE, SS_NESTCOUNT + 2);
    CHECK_EQ(SS_MULTIPLELINK, SS_WAITQUEUE + MLH_SIZE);
    CHECK_EQ(SS_OWNER, SS_MULTIPLELINK + 8 + 4);  // a MinNode and a Task pointer
    CHECK_EQ(SS_QUEUECOUNT, SS_OWNER + 4);
    CHECK_EQ(SS_SIZE, SS_QUEUECOUNT + 2);
    CHECK_EQ(ds_SIZEOF, 12u);

    // Process: a Task, a MsgPort, a WORD, then longwords to the end.
    CHECK_EQ(pr_MsgPort, TC_SIZE);
    CHECK_EQ(pr_Pad, pr_MsgPort + MP_SIZE);
    const u32 longs[] = {pr_SegList,     pr_StackSize,      pr_GlobVec,   pr_TaskNum,
                         pr_StackBase,   pr_Result2,        pr_CurrentDir, pr_CIS,
                         pr_COS,         pr_ConsoleTask,    pr_FileSystemTask, pr_CLI,
                         pr_ReturnAddr,  pr_PktWait,        pr_WindowPtr, pr_HomeDir,
                         pr_Flags,       pr_ExitCode,       pr_ExitData,  pr_Arguments};
    u32 at = pr_Pad + 2;
    for (u32 field : longs) {
        CHECK_EQ(field, at);
        at += 4;
    }
    CHECK_EQ(pr_LocalVars, at);
    CHECK_EQ(pr_ShellPrivate, at + MLH_SIZE);
    CHECK_EQ(pr_CES, at + MLH_SIZE + 4);
    CHECK_EQ(pr_SIZEOF, at + MLH_SIZE + 8);
    // The well-known absolute offsets.
    CHECK_EQ(pr_Result2, 0x94u);
    CHECK_EQ(pr_CIS, 0x9Cu);
    CHECK_EQ(pr_COS, 0xA0u);
    CHECK_EQ(pr_CLI, 0xACu);
    CHECK_EQ(pr_WindowPtr, 0xB8u);
    CHECK_EQ(pr_SIZEOF, 228u);

    // CommandLineInterface, FileHandle, FileLock: all longwords.
    const u32 cli[] = {cli_Result2,       cli_SetName,      cli_CommandDir,   cli_ReturnCode,
                       cli_CommandName,   cli_FailLevel,    cli_Prompt,       cli_StandardInput,
                       cli_CurrentInput,  cli_CommandFile,  cli_Interactive,  cli_Background,
                       cli_CurrentOutput, cli_DefaultStack, cli_StandardOutput, cli_Module};
    for (u32 i = 0; i < 16; ++i) {
        CHECK_EQ(cli[i], i * 4);
    }
    CHECK_EQ(cli_SIZEOF, 64u);
    const u32 fh[] = {fh_Link, fh_Interactive, fh_Type,  fh_Buf,  fh_Pos, fh_End,
                      fh_Funcs, fh_Func2,      fh_Func3, fh_Args, fh_Arg2};
    for (u32 i = 0; i < 11; ++i) {
        CHECK_EQ(fh[i], i * 4);
    }
    CHECK_EQ(fh_SIZEOF, 44u);
    const u32 fl[] = {fl_Link, fl_Key, fl_Access, fl_Task, fl_Volume};
    for (u32 i = 0; i < 5; ++i) {
        CHECK_EQ(fl[i], i * 4);
    }
    CHECK_EQ(fl_SIZEOF, 20u);
    // LocalVar: a Node, a UWORD, a pointer and a length.
    CHECK_EQ(lv_Node, 0u);
    CHECK_EQ(lv_Flags, LN_SIZE);
    CHECK_EQ(lv_Value, LN_SIZE + 2);
    CHECK_EQ(lv_Len, LN_SIZE + 6);
    CHECK_EQ(LocalVar_SIZEOF, LN_SIZE + 10);
    CHECK_EQ(GVF_GLOBAL_ONLY, 1u << 8);
    CHECK_EQ(GVF_LOCAL_ONLY, 1u << 9);
    CHECK_EQ(GVF_BINARY_VAR, 1u << 10);
    CHECK_EQ(GVF_DONT_NULL_TERM, 1u << 11);
    CHECK_EQ(GVF_SAVE_VAR, 1u << 12);

    // DosLibrary: a Library, then longwords.
    const u32 dl[] = {dl_Root,   dl_GV,      dl_A2,          dl_A5,           dl_A6,
                      dl_Errors, dl_TimeReq, dl_UtilityBase, dl_IntuitionBase, dl_SIZEOF};
    for (u32 i = 0; i < 10; ++i) {
        CHECK_EQ(dl[i], LIB_SIZE + i * 4);
    }

    // RootNode and DosInfo.
    CHECK_EQ(rn_TaskArray, 0u);
    CHECK_EQ(rn_ConsoleSegment, 4u);
    CHECK_EQ(rn_Time, 8u);
    CHECK_EQ(rn_RestartSeg, rn_Time + ds_SIZEOF);
    CHECK_EQ(rn_Info, rn_RestartSeg + 4);
    CHECK_EQ(rn_FileHandlerSegment, rn_Info + 4);
    CHECK_EQ(rn_CliList, rn_FileHandlerSegment + 4);
    CHECK_EQ(rn_BootProc, rn_CliList + MLH_SIZE);
    CHECK_EQ(rn_ShellSegment, rn_BootProc + 4);
    CHECK_EQ(rn_Flags, rn_ShellSegment + 4);
    CHECK_EQ(rn_SIZEOF, rn_Flags + 4);
    CHECK_EQ(di_McName, 0u);
    CHECK_EQ(di_DevInfo, 4u);
    CHECK_EQ(di_Devices, 8u);
    CHECK_EQ(di_Handlers, 12u);
    CHECK_EQ(di_NetHand, 16u);
    CHECK_EQ(di_DevLock, 20u);
    CHECK_EQ(di_EntryLock, di_DevLock + SS_SIZE);
    CHECK_EQ(di_DeleteLock, di_EntryLock + SS_SIZE);
    CHECK_EQ(di_SIZEOF, di_DeleteLock + SS_SIZE);

    // DosList for a volume: the handler union is six longwords.
    CHECK_EQ(dol_Next, 0u);
    CHECK_EQ(dol_Type, 4u);
    CHECK_EQ(dol_Task, 8u);
    CHECK_EQ(dol_Lock, 12u);
    CHECK_EQ(dol_VolumeDate, 16u);
    CHECK_EQ(dol_LockList, dol_VolumeDate + ds_SIZEOF);
    CHECK_EQ(dol_DiskType, dol_LockList + 4);
    CHECK_EQ(dol_AssignName, 16u);
    CHECK_EQ(dol_List, 20u);
    CHECK_EQ(dol_Name, dol_VolumeDate + 6 * 4);
    CHECK_EQ(DosList_SIZEOF, dol_Name + 4);
    CHECK_EQ(al_Next, 0u);
    CHECK_EQ(al_Lock, 4u);
    CHECK_EQ(AssignList_SIZEOF, 8u);
    CHECK_EQ(dvp_Port, 0u);
    CHECK_EQ(dvp_Lock, 4u);
    CHECK_EQ(dvp_Flags, 8u);
    CHECK_EQ(dvp_DevNode, 12u);
    CHECK_EQ(DevProc_SIZEOF, 16u);
    CHECK_EQ(DVPF_UNLOCK, 1u);
    CHECK_EQ(DVPF_ASSIGN, 2u);
    CHECK_EQ(LDF_READ, 1u);
    CHECK_EQ(LDF_WRITE, 2u);
    CHECK_EQ(LDF_DEVICES, 4u);
    CHECK_EQ(LDF_VOLUMES, 8u);
    CHECK_EQ(LDF_ASSIGNS, 16u);
    CHECK_EQ(LDF_ENTRY, 32u);
    CHECK_EQ(LDF_DELETE, 64u);
    CHECK_EQ(LDF_ALL, 28u);
    CHECK_EQ(DLT_DEVICE, 0u);
    CHECK_EQ(DLT_DIRECTORY, 1u);
    CHECK_EQ(DLT_VOLUME, 2u);
    CHECK_EQ(DLT_LATE, 3u);
    CHECK_EQ(DLT_NONBINDING, 4u);

    // FileInfoBlock and InfoData.
    CHECK_EQ(fib_DiskKey, 0u);
    CHECK_EQ(fib_DirEntryType, 4u);
    CHECK_EQ(fib_FileName, 8u);
    CHECK_EQ(fib_Protection, fib_FileName + 108);
    CHECK_EQ(fib_EntryType, fib_Protection + 4);
    CHECK_EQ(fib_Size, fib_EntryType + 4);
    CHECK_EQ(fib_NumBlocks, fib_Size + 4);
    CHECK_EQ(fib_DateStamp, fib_NumBlocks + 4);
    CHECK_EQ(fib_Comment, fib_DateStamp + ds_SIZEOF);
    CHECK_EQ(fib_OwnerUID, fib_Comment + 80);
    CHECK_EQ(fib_OwnerGID, fib_OwnerUID + 2);
    CHECK_EQ(fib_Reserved, fib_OwnerGID + 2);
    CHECK_EQ(fib_SIZEOF, fib_Reserved + 32);
    CHECK_EQ(fib_SIZEOF, 260u);
    const u32 id[] = {id_NumSoftErrors, id_UnitNumber, id_DiskState,  id_NumBlocks, id_NumBlocksUsed,
                      id_BytesPerBlock, id_DiskType,   id_VolumeNode, id_InUse,     id_SIZEOF};
    for (u32 i = 0; i < 10; ++i) {
        CHECK_EQ(id[i], i * 4);
    }

    // CSource and RDArgs.
    CHECK_EQ(CS_Buffer, 0u);
    CHECK_EQ(CS_Length, 4u);
    CHECK_EQ(CS_CurChr, 8u);
    CHECK_EQ(RDA_Source, 0u);
    CHECK_EQ(RDA_DAList, 12u);
    CHECK_EQ(RDA_Buffer, 16u);
    CHECK_EQ(RDA_BufSiz, 20u);
    CHECK_EQ(RDA_ExtHelp, 24u);
    CHECK_EQ(RDA_Flags, 28u);
    CHECK_EQ(RDA_SIZEOF, 32u);

    // Values.
    CHECK_EQ(MODE_OLDFILE, 1005u);
    CHECK_EQ(MODE_NEWFILE, 1006u);
    CHECK_EQ(MODE_READWRITE, 1004u);
    CHECK_EQ(as_signed(OFFSET_BEGINNING), -1);
    CHECK_EQ(OFFSET_CURRENT, 0u);
    CHECK_EQ(OFFSET_END, 1u);
    CHECK_EQ(as_signed(SHARED_LOCK), -2);
    CHECK_EQ(as_signed(EXCLUSIVE_LOCK), -1);
    CHECK_EQ(as_signed(ST_FILE), -3);
    CHECK_EQ(ST_ROOT, 1u);
    CHECK_EQ(ST_USERDIR, 2u);
    CHECK_EQ(DLT_VOLUME, 2u);
    CHECK_EQ(ID_WRITE_PROTECTED, 80u);
    CHECK_EQ(NT_PROCESS, 13u);
    CHECK_EQ(ERROR_NO_FREE_STORE, 103u);
    CHECK_EQ(ERROR_BAD_TEMPLATE, 114u);
    CHECK_EQ(ERROR_BAD_NUMBER, 115u);
    CHECK_EQ(ERROR_REQUIRED_ARG_MISSING, 116u);
    CHECK_EQ(ERROR_KEY_NEEDS_ARG, 117u);
    CHECK_EQ(ERROR_TOO_MANY_ARGS, 118u);
    CHECK_EQ(ERROR_LINE_TOO_LONG, 120u);
    CHECK_EQ(ERROR_FILE_NOT_OBJECT, 121u);
    CHECK_EQ(ERROR_OBJECT_NOT_FOUND, 205u);
    CHECK_EQ(ERROR_ACTION_NOT_KNOWN, 209u);
    CHECK_EQ(ERROR_INVALID_COMPONENT_NAME, 210u);
    CHECK_EQ(ERROR_INVALID_LOCK, 211u);
    CHECK_EQ(ERROR_OBJECT_WRONG_TYPE, 212u);
    CHECK_EQ(ERROR_DISK_WRITE_PROTECTED, 214u);
    CHECK_EQ(ERROR_DEVICE_NOT_MOUNTED, 218u);
    CHECK_EQ(ERROR_SEEK_ERROR, 219u);
    CHECK_EQ(ERROR_NOT_A_DOS_DISK, 225u);
    CHECK_EQ(ERROR_NO_MORE_ENTRIES, 232u);
    CHECK_EQ(ERROR_BAD_HUNK, 235u);
    CHECK_EQ(LOCK_SAME, 0u);
    CHECK_EQ(LOCK_SAME_VOLUME, 1u);
    CHECK_EQ(as_signed(LOCK_DIFFERENT), -1);
    CHECK_EQ(as_signed(ITEM_EQUAL), -2);
    CHECK_EQ(as_signed(ITEM_ERROR), -1);
    CHECK_EQ(ITEM_NOTHING, 0u);
    CHECK_EQ(ITEM_UNQUOTED, 1u);
    CHECK_EQ(ITEM_QUOTED, 2u);
    CHECK_EQ(DOS_FILEHANDLE, 0u);
    CHECK_EQ(DOS_EXALLCONTROL, 1u);
    CHECK_EQ(DOS_FIB, 2u);
    CHECK_EQ(DOS_STDPKT, 3u);
    CHECK_EQ(DOS_CLI, 4u);
    CHECK_EQ(DOS_RDARGS, 5u);
    CHECK_EQ(ACTION_DELETE_OBJECT, 16u);
    CHECK_EQ(ACTION_RENAME_OBJECT, 17u);
    CHECK_EQ(ACTION_SET_PROTECT, 21u);
    CHECK_EQ(ACTION_CREATE_DIR, 22u);
    CHECK_EQ(ACTION_SET_COMMENT, 28u);
    CHECK_EQ(ACTION_SET_DATE, 34u);
    CHECK_EQ(ED_NAME, 1u);
    CHECK_EQ(ED_TYPE, 2u);
    CHECK_EQ(ED_SIZE, 3u);
    CHECK_EQ(ED_PROTECTION, 4u);
    CHECK_EQ(ED_DATE, 5u);
    CHECK_EQ(ED_COMMENT, 6u);
    CHECK_EQ(ed_Next, 0u);
    CHECK_EQ(ed_Name, 4u);
    CHECK_EQ(ed_Type, 8u);
    CHECK_EQ(ed_Size, 12u);
    CHECK_EQ(ed_Prot, 16u);
    CHECK_EQ(ed_Days, 20u);
    CHECK_EQ(ed_Mins, 24u);
    CHECK_EQ(ed_Ticks, 28u);
    CHECK_EQ(ed_Comment, 32u);
    CHECK_EQ(eac_Entries, 0u);
    CHECK_EQ(eac_LastKey, 4u);
    CHECK_EQ(eac_MatchString, 8u);
    CHECK_EQ(eac_MatchFunc, 12u);
    CHECK_EQ(eac_SIZEOF, 16u);
    CHECK_EQ(sp_Msg, 0u);
    CHECK_EQ(sp_Pkt, 20u);
    CHECK_EQ(sp_SIZEOF, 68u);

    // DateTime and AnchorPath.
    CHECK_EQ(dat_Stamp, 0u);
    CHECK_EQ(dat_Format, 12u);
    CHECK_EQ(dat_Flags, 13u);
    CHECK_EQ(dat_StrDay, 14u);
    CHECK_EQ(dat_StrDate, 18u);
    CHECK_EQ(dat_StrTime, 22u);
    CHECK_EQ(dat_SIZEOF, 26u);
    CHECK_EQ(ap_Base, 0u);
    CHECK_EQ(ap_Last, 4u);
    CHECK_EQ(ap_Current, 4u);
    CHECK_EQ(ap_BreakBits, 8u);
    CHECK_EQ(ap_FoundBreak, 12u);
    CHECK_EQ(ap_Flags, 16u);
    CHECK_EQ(ap_Reserved, 17u);
    CHECK_EQ(ap_Strlen, 18u);
    CHECK_EQ(ap_Info, 20u);
    CHECK_EQ(ap_Buf, 280u);
    CHECK_EQ(APF_DOWILD, 1u);
    CHECK_EQ(APF_ITSWILD, 2u);
    CHECK_EQ(APF_DODIR, 4u);
    CHECK_EQ(APF_DIDDIR, 8u);
    CHECK_EQ(APF_NOMEMERR, 16u);
    CHECK_EQ(APF_DODOT, 32u);
    CHECK_EQ(APF_DirChanged, 64u);
    CHECK_EQ(APF_FollowHLinks, 128u);
    CHECK_EQ(an_Child, 0u);
    CHECK_EQ(an_Parent, 4u);
    CHECK_EQ(an_Lock, 8u);
    CHECK_EQ(an_Info, 12u);
    CHECK_EQ(an_Flags, 272u);
    CHECK_EQ(an_String, 273u);
    CHECK_EQ(AChain_SIZEOF, 274u);
    CHECK_EQ(DDF_PatternBit, 1u);
    CHECK_EQ(DDF_ExaminedBit, 2u);
    CHECK_EQ(DDF_Completed, 4u);
    CHECK_EQ(DDF_AllBit, 8u);
    CHECK_EQ(DDF_Single, 16u);
    CHECK_EQ(ACTION_CURRENT_VOLUME, 7u);
    CHECK_EQ(ACTION_LOCATE_OBJECT, 8u);
    CHECK_EQ(ACTION_FREE_LOCK, 15u);
    CHECK_EQ(ACTION_COPY_DIR, 19u);
    CHECK_EQ(ACTION_EXAMINE_OBJECT, 23u);
    CHECK_EQ(ACTION_EXAMINE_NEXT, 24u);
    CHECK_EQ(ACTION_DISK_INFO, 25u);
    CHECK_EQ(ACTION_INFO, 26u);
    CHECK_EQ(ACTION_PARENT, 29u);
    CHECK_EQ(ACTION_SAME_LOCK, 40u);
    CHECK_EQ(ACTION_IS_FILESYSTEM, 1027u);
}

// --- the library and its structures -------------------------------------

void dos_library_is_built() {
    Rig r;
    const u32 dos = r.base();
    CHECK(dos != 0);
    CHECK(r.read_str(r.get32(dos + LN_NAME)) == "dos.library");
    CHECK_EQ(r.get8(dos + LN_TYPE), NT_LIBRARY);
    CHECK_EQ(r.get16(dos + LIB_VERSION), kHombreLibVersion);
    CHECK_EQ(r.get16(dos + LIB_REVISION), kHombreLibRevision);
    CHECK(r.read_str(r.get32(dos + LIB_IDSTRING)) == "dos 60.0 (Hombre)\r\n");
    CHECK(r.get16(dos + LIB_POSSIZE) >= dl_SIZEOF);
    CHECK(r.get16(dos + LIB_NEGSIZE) >= dos_lvo::SetOwner);

    // Found by name, and opened, from 68000 code.
    r.str(kData, "dos.library");
    r.cpu().a[1] = kData;
    r.cpu().d[0] = kHombreLibVersion;
    r.put16(kCall, 0x4EAE);
    r.put16(kCall + 2, disp(exec_lvo::OpenLibrary));
    r.put16(kCall + 4, 0x4EF8);
    r.put16(kCall + 6, 0x0400);
    g_jit.flush();
    r.cpu().a[6] = g_exec.exec_base();
    r.cpu().a[7] = kStack;
    r.cpu().pc = kCall;
    CHECK_EQ(g_jit.run(kBlocks), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], dos);

    // RootNode, DosInfo and the volume node.
    const u32 root = r.get32(dos + dl_Root);
    CHECK(root != 0);
    const u32 info = r.get32(root + rn_Info) << 2;
    CHECK(info != 0);
    const u32 vol = r.get32(info + di_DevInfo) << 2;
    CHECK(vol != 0);
    CHECK_EQ(r.get32(vol + dol_Type), DLT_VOLUME);
    CHECK(r.read_bstr(r.get32(vol + dol_Name)) == "HombreTest");
    CHECK_EQ(r.get32(vol + dol_DiskType), ID_DOS_DISK);
    const u32 ram_vol = r.get32(vol + dol_Next) << 2;
    CHECK(ram_vol != 0);
    CHECK_EQ(r.get32(ram_vol + dol_Type), DLT_VOLUME);
    CHECK(r.read_bstr(r.get32(ram_vol + dol_Name)) == "Ram Disk");
    CHECK_EQ(r.get32(ram_vol + dol_DiskType), ID_DOS_DISK);
    const u32 dev = r.get32(ram_vol + dol_Next) << 2;
    CHECK(dev != 0);
    CHECK_EQ(r.get32(dev + dol_Type), DLT_DEVICE);
    CHECK(r.read_bstr(r.get32(dev + dol_Name)) == "DF0");
    // DF1: to DF3: are there with no disk in them.
    u32 last_drive = dev;
    for (const char* name : {"DF1", "DF2", "DF3"}) {
        last_drive = r.get32(last_drive + dol_Next) << 2;
        CHECK(last_drive != 0);
        CHECK_EQ(r.get32(last_drive + dol_Type), DLT_DEVICE);
        CHECK(r.read_bstr(r.get32(last_drive + dol_Name)) == name);
    }
    const u32 ram_dev = r.get32(last_drive + dol_Next) << 2;
    CHECK(ram_dev != 0);
    CHECK_EQ(r.get32(ram_dev + dol_Type), DLT_DEVICE);
    CHECK(r.read_bstr(r.get32(ram_dev + dol_Name)) == "RAM");
    const u32 tasks = r.get32(root + rn_TaskArray) << 2;
    CHECK_EQ(r.get32(tasks), Dos::kMaxProcs);
    CHECK_EQ(r.get32(tasks + 4), r.proc() + pr_MsgPort);
    // The semaphores are initialised: their wait queues are empty lists.
    const u32 queue = info + di_DevLock + SS_WAITQUEUE;
    CHECK_EQ(r.get32(queue + MLH_HEAD), queue + MLH_TAIL);
    CHECK_EQ(r.get32(queue + MLH_TAILPRED), queue + MLH_HEAD);

    // The Process is exec's task.
    const u32 p = r.proc();
    CHECK(p != 0);
    CHECK_EQ(r.get32(g_exec.exec_base() + ThisTask), p);
    CHECK_EQ(r.get8(p + LN_TYPE), NT_PROCESS);
    CHECK_EQ(r.get8(p + pr_MsgPort + LN_TYPE), NT_MSGPORT);
    CHECK_EQ(r.get32(p + pr_MsgPort + MP_SIGTASK), p);
    const u32 list = p + pr_MsgPort + MP_MSGLIST;
    CHECK_EQ(r.get32(list + LH_HEAD), list + LH_TAIL);
    CHECK_EQ(r.get32(p + pr_TaskNum), 1u);
    CHECK_EQ(r.get32(p + pr_CurrentDir), 0u);
    CHECK(r.get32(p + pr_CIS) != 0);
    CHECK(r.get32(p + pr_COS) != 0);
    CHECK(r.get32(p + pr_CIS) != r.get32(p + pr_COS));
    CHECK(r.get32(p + pr_FileSystemTask) != 0);
    const u32 cli = r.get32(p + pr_CLI) << 2;
    CHECK(cli != 0);
    CHECK_EQ(r.get32(cli + cli_StandardInput), r.get32(p + pr_CIS));
    CHECK_EQ(r.get32(cli + cli_CurrentInput), r.get32(p + pr_CIS));
    CHECK_EQ(r.get32(cli + cli_StandardOutput), r.get32(p + pr_COS));
    CHECK_EQ(r.get32(cli + cli_CurrentOutput), r.get32(p + pr_COS));
    CHECK_EQ(r.get32(cli + cli_DefaultStack), Dos::kStackBytes / 4);
    CHECK_EQ(r.get32(cli + cli_Interactive), DOSTRUE);
    CHECK_EQ(r.get32(cli + cli_FailLevel), 10u);
    CHECK(r.read_bstr(r.get32(cli + cli_SetName)) == "HombreTest:");
    CHECK(r.read_bstr(r.get32(cli + cli_CommandName)).empty());

    // The console handles say they are interactive.
    const u32 out = r.get32(p + pr_COS) << 2;
    CHECK_EQ(r.get32(out + fh_Interactive), DOSTRUE);

    // FindTask(0) and Cli().
    r.cpu().a[1] = 0;
    r.put16(kCall + 2, disp(exec_lvo::FindTask));
    g_jit.flush();
    r.cpu().a[6] = g_exec.exec_base();
    r.cpu().a[7] = kStack;
    r.cpu().pc = kCall;
    CHECK_EQ(g_jit.run(kBlocks), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], p);
    CHECK_EQ(r.call(dos_lvo::Cli), cli);
}

// --- running commands ---------------------------------------------------

void command_writes_to_the_console() {
    Rig r;
    u32 result = 0;
    CHECK(g_dos.run_command("C:Hello", "", result, kBlocks));
    CHECK_EQ(result, 3u);
    CHECK(g_console == "hi\n");
    CHECK_EQ(r.cpu().exit, ExitReason::None);
    // The command was unloaded: it can run again, and memory is as it was.
    const auto before = r.chunks();
    CHECK(g_dos.run_command("c:hello", "", result, kBlocks));
    CHECK(g_console == "hi\nhi\n");
    CHECK(r.chunks() == before);
}

void command_entry_state() {
    Rig r;
    u32 result = 1;
    CHECK(g_dos.run_command("C:Entry", "one two", result, kBlocks));
    CHECK_EQ(result, 0u);
    const u32 args = r.get32(kOut);
    CHECK(r.read_str(args) == "one two\n");
    CHECK_EQ(r.get32(kOut + 4), 8u);
    CHECK_EQ(r.get32(kOut + 8), Dos::kStackBytes);
    CHECK_EQ(r.get32(kOut + 16), kReturnStub);
    const u32 p = r.proc();
    const u32 sp = r.get32(kOut + 12);
    CHECK_EQ(r.get32(p + pr_ReturnAddr), sp + 4);
    CHECK_EQ(r.get32(p + pr_StackSize), Dos::kStackBytes);
    CHECK_EQ(r.get32(p + TC_SPUPPER), sp + 8);
    CHECK_EQ(r.get32(p + TC_SPLOWER), sp + 8 - Dos::kStackBytes);
    CHECK_EQ(r.get32(p + pr_Arguments), args);
    const u32 cli = r.get32(p + pr_CLI) << 2;
    CHECK(r.read_bstr(r.get32(cli + cli_CommandName)) == "C:Entry");
    CHECK_EQ(r.get32(cli + cli_Module), 0u);  // cleared once unloaded
    // An argument line that does not fit.
    g_lines.clear();
    CHECK(!g_dos.run_command("C:Entry", std::string(Dos::kMaxArgs + 1, 'a').c_str(), result,
                             kBlocks));
    CHECK(logged_containing("argument line"));
    CHECK(g_dos.run_command("C:Entry", std::string(Dos::kMaxArgs, 'a').c_str(), result, kBlocks));
    // An argument line that already ends in a newline keeps just that one.
    CHECK(g_dos.run_command("C:Entry", "x\n", result, kBlocks));
    CHECK(r.read_str(r.get32(kOut)) == "x\n");
}

void exit_ends_the_command() {
    Rig r;
    u32 result = 0;
    CHECK(g_dos.run_command("C:Exit", "", result, kBlocks));
    CHECK_EQ(result, 7u);
    CHECK_EQ(r.cpu().exit, ExitReason::None);
    // Exit with no command running ends the run, logged.
    g_lines.clear();
    CHECK_EQ(r.dos(dos_lvo::Exit), ExitReason::Halt);
    CHECK(logged("dos: Exit with no command running"));
}

void unknown_vector_ends_the_run() {
    Rig r;
    u32 result = 0;
    CHECK(!g_dos.run_command("C:Unknown", "", result, kBlocks));
    CHECK_EQ(r.cpu().exit, ExitReason::Unimplemented);
    CHECK(logged("dos: unimplemented LVO -162"));
    CHECK(logged_containing("C:Unknown"));
    // The last vector of the table, and one between two implemented ones.
    g_lines.clear();
    CHECK_EQ(r.dos(dos_lvo::SetOwner), ExitReason::Unimplemented);
    CHECK(logged("dos: unimplemented LVO -996"));
    CHECK_EQ(r.dos(246), ExitReason::Unimplemented);
    CHECK(logged("dos: unimplemented LVO -246"));
}

void commands_that_cannot_run() {
    u32 result = 99;
    {
        Rig r;
        const auto before = r.chunks();
        CHECK(!g_dos.run_command("C:Missing", "", result, kBlocks));
        CHECK(logged_containing("C:Missing"));
        CHECK(!g_dos.run_command("LIBS:x", "", result, kBlocks));
        CHECK(!g_dos.run_command("C:Empty", "", result, kBlocks));
        CHECK(!g_dos.run_command("C", "", result, kBlocks));
        CHECK(!g_dos.run_command("Nowhere:Hello", "", result, kBlocks));
        CHECK_EQ(result, 99u);
        CHECK(r.chunks() == before);
    }
    {
        // A command that runs out of blocks.
        Asm a;
        a.label("loop").op({0x60FE});  // BRA.S loop
        Rig r({{"C/Spin", hunk_file(a.build())}});
        CHECK(!g_dos.run_command("C:Spin", "", result, 100));
        CHECK_EQ(r.cpu().exit, ExitReason::Budget);
        CHECK(logged_containing("Budget"));
    }
    {
        // A command that faults.
        Asm a;
        a.op({0x2038, 0x0001});  // MOVE.L $1.W,D0
        Rig r({{"C/Fault", hunk_file(a.build())}});
        CHECK(!g_dos.run_command("C:Fault", "", result, kBlocks));
        CHECK_EQ(r.cpu().exit, ExitReason::AddressError);
    }
}

// --- console --------------------------------------------------------------

void console_input_serves_the_arguments_once() {
    // Read(Input(), buf, 4) twice, then once more at the end, from 68000
    // code run as a command; the counts and the bytes are stored at kOut.
    Asm a;
    a.open_dos();
    for (u32 i = 0; i < 3; ++i) {
        a.dos(dos_lvo::Input).from_d0(1).move_l(kData + i * 16, 2).moveq(4, 3).dos(dos_lvo::Read);
        a.store_d0(kOut + i * 4);
    }
    a.moveq(0, 0).rts();
    Rig rr({{"C/Reader", hunk_file(a.build())}});
    u32 result = 1;
    CHECK(g_dos.run_command("C:Reader", "abcdef", result, kBlocks));
    CHECK_EQ(rr.get32(kOut), 4u);
    CHECK_EQ(rr.get32(kOut + 4), 3u);
    CHECK_EQ(rr.get32(kOut + 8), 0u);
    CHECK(rr.bytes(kData, 4) == text("abcd"));
    CHECK(rr.bytes(kData + 16, 3) == text("ef\n"));
    // Outside a command the console reads end of file.
    CHECK_EQ(rr.call(dos_lvo::Read, rr.call(dos_lvo::Input), kData, 10), 0u);
}

void console_output_functions() {
    Rig r;
    const u32 out = r.call(dos_lvo::Output);
    const u32 in = r.call(dos_lvo::Input);
    CHECK_EQ(out, r.get32(r.proc() + pr_COS));
    CHECK_EQ(in, r.get32(r.proc() + pr_CIS));

    r.str(kData, "hello");
    CHECK_EQ(r.call(dos_lvo::Write, out, kData, 5), 5u);
    CHECK_EQ(r.call(dos_lvo::Write, out, kData, 0), 0u);
    CHECK_EQ(r.call(dos_lvo::PutStr, kData), 0u);
    CHECK_EQ(r.call(dos_lvo::FPutC, out, 0x1255), 0x55u);  // the low byte
    CHECK_EQ(r.call(dos_lvo::FPuts, out, kData), 0u);
    CHECK_EQ(r.call(dos_lvo::Flush, out), DOSTRUE);
    // The input handle writes to the console too.
    CHECK_EQ(r.call(dos_lvo::Write, in, kData, 1), 1u);
    CHECK(g_console == "hellohello" + std::string(1, static_cast<char>(0x55)) + "helloh");

    // VPrintf and VFPrintf format as RawDoFmt does.
    g_console.clear();
    r.str(kData, "%s=%ld,%d,%lx,%c!");
    r.str(kData + 0x100, "n");
    r.put32(kData + 0x200, kData + 0x100);
    r.put32(kData + 0x204, 0xFFFFFFF6);  // -10
    r.put16(kData + 0x208, 7);
    r.put32(kData + 0x20A, 0xBEEF);
    r.put16(kData + 0x20E, 'z');
    CHECK_EQ(r.call(dos_lvo::VPrintf, kData, kData + 0x200), 15u);
    CHECK(g_console == "n=-10,7,BEEF,z!");
    g_console.clear();
    CHECK_EQ(r.call(dos_lvo::VFPrintf, out, kData, kData + 0x200), 15u);
    CHECK(g_console == "n=-10,7,BEEF,z!");
    // Long output goes through in pieces and is counted whole.
    g_console.clear();
    r.str(kData, "%300s|");
    r.put32(kData + 0x200, r.str(kData + 0x100, "x"));
    CHECK_EQ(r.call(dos_lvo::VPrintf, kData, kData + 0x200), 301u);
    CHECK_EQ(g_console.size(), 301u);
    CHECK(g_console.substr(298) == " x|");
    // The output of one format is capped at a MiB; past it VPrintf fails.
    g_console.clear();
    g_lines.clear();
    std::string many;
    for (u32 i = 0; i < 12; ++i) {
        many += "%99999d";
        r.put16(kData + 0x200 + 2 * i, 0);
    }
    r.str(kData, many);
    CHECK_EQ(as_signed(r.call(dos_lvo::VPrintf, kData, kData + 0x200)), -1);
    CHECK_EQ(g_console.size(), 1u << 20);
    CHECK(logged_containing("format output limit"));
}

void select_input_and_output() {
    Rig r;
    const u32 out = r.call(dos_lvo::Output);
    const u32 in = r.call(dos_lvo::Input);
    r.str(kData, "*");
    const u32 star = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    CHECK(star != 0);
    CHECK_EQ(r.call(dos_lvo::SelectOutput, star), out);
    CHECK_EQ(r.call(dos_lvo::Output), star);
    CHECK_EQ(r.get32(r.proc() + pr_COS), star);
    CHECK_EQ(r.call(dos_lvo::SelectInput, star), in);
    CHECK_EQ(r.call(dos_lvo::Input), star);
    r.str(kData, "ok");
    CHECK_EQ(r.call(dos_lvo::PutStr, kData), 0u);
    CHECK(g_console == "ok");
    CHECK_EQ(r.call(dos_lvo::SelectOutput, out), star);
    CHECK_EQ(r.call(dos_lvo::SelectInput, in), star);
    CHECK_EQ(r.call(dos_lvo::Close, star), DOSTRUE);
    // CONSOLE: is the console too.
    r.str(kData, "console:");
    CHECK(r.call(dos_lvo::Open, kData, MODE_OLDFILE) != 0);
}

void set_mode_on_the_console() {
    Rig r;
    CHECK_EQ(r.call(dos_lvo::SetMode, r.call(dos_lvo::Input), 1), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::SetMode, r.call(dos_lvo::Input), 0), DOSTRUE);
    r.str(kData, "C:Echo");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK_EQ(r.call(dos_lvo::SetMode, fh, 1), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_ACTION_NOT_KNOWN);
    g_lines.clear();
    CHECK_EQ(r.call(dos_lvo::SetMode, 0, 1), DOSFALSE);
    CHECK(logged_containing("dos: SetMode on a bad file handle"));
}

void output_to_bad_handles() {
    Rig r;
    r.str(kData, "x");
    g_lines.clear();
    // A handle dos never made: a failed write, logged, nothing printed.
    CHECK_EQ(as_signed(r.call(dos_lvo::Write, (kData + 0x100) >> 2, kData, 1)), -1);
    CHECK(logged_containing("dos: Write to a bad file handle"));
    CHECK_EQ(as_signed(r.call(dos_lvo::FPutC, 0, 'a')), -1);
    CHECK_EQ(as_signed(r.call(dos_lvo::FPuts, 0x3FFFFFFF, kData)), -1);
    CHECK_EQ(as_signed(r.call(dos_lvo::VFPrintf, 1, kData, kData)), -1);
    CHECK_EQ(as_signed(r.call(dos_lvo::Read, 0, kData, 1)), -1);
    // An Output() that is not a handle.
    r.put32(r.proc() + pr_COS, 12345);
    CHECK(r.call(dos_lvo::PutStr, kData) != 0);
    CHECK(g_console.empty());
    // A bad buffer ends the run.
    r.put32(r.proc() + pr_COS, r.get32(r.proc() + pr_CIS));
    r.cpu().d[1] = r.call(dos_lvo::Output);
    r.cpu().d[2] = kGuestBytes - 2;
    r.cpu().d[3] = 4;
    CHECK_EQ(r.dos(dos_lvo::Write), ExitReason::BusError);
    // A negative length writes nothing.
    CHECK_EQ(as_signed(r.call(dos_lvo::Write, r.call(dos_lvo::Output), kData, 0xFFFFFFFF)), -1);
    // A string that runs to the end of memory is printed up to there.
    g_console.clear();
    r.str(kGuestBytes - 3, "ab");
    r.put8(kGuestBytes - 1, 'c');
    r.cpu().d[1] = kGuestBytes - 3;
    CHECK_EQ(r.dos(dos_lvo::PutStr), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, kGuestBytes);
    CHECK(g_console == "abc");
}

// --- errors -------------------------------------------------------------

void io_err_and_faults() {
    Rig r;
    r.set_io_err(205);
    CHECK_EQ(r.io_err(), 205u);
    CHECK_EQ(r.get32(r.proc() + pr_Result2), 205u);
    CHECK_EQ(r.call(dos_lvo::SetIoErr, 0), 205u);
    CHECK_EQ(r.io_err(), 0u);

    // Fault: header, ": ", message, cut to the buffer.
    r.str(kData, "Copy");
    CHECK_EQ(r.call(dos_lvo::Fault, 205, kData, kData + 0x100, 80), 31u);
    CHECK(r.read_str(kData + 0x100) == "Copy: no such file or directory");
    CHECK_EQ(r.call(dos_lvo::Fault, 118, 0, kData + 0x100, 80), 18u);
    CHECK(r.read_str(kData + 0x100) == "too many arguments");
    CHECK_EQ(r.call(dos_lvo::Fault, 205, kData, kData + 0x100, 7), 6u);
    CHECK(r.read_str(kData + 0x100) == "Copy: ");
    r.put8(kData + 0x100, 0xEE);
    CHECK_EQ(r.call(dos_lvo::Fault, 205, kData, kData + 0x100, 0), 0u);
    CHECK_EQ(r.get8(kData + 0x100), 0xEEu);
    CHECK_EQ(r.call(dos_lvo::Fault, 999, 0, kData + 0x100, 80), 9u);
    CHECK(r.read_str(kData + 0x100) == "Error 999");
    CHECK_EQ(r.call(dos_lvo::Fault, 0, kData, kData + 0x100, 80), 0u);
    CHECK(r.read_str(kData + 0x100).empty());
    // Every code dos itself sets has a message.
    for (u32 code : {103u, 114u, 115u, 116u, 117u, 118u, 119u, 120u, 121u, 205u, 209u, 210u,
                     211u, 212u, 214u, 218u, 219u, 225u, 232u, 235u}) {
        r.call(dos_lvo::Fault, code, 0, kData + 0x100, 80);
        CHECK(r.read_str(kData + 0x100).rfind("Error ", 0) != 0);
    }

    // PrintFault prints to Output() and leaves the code in IoErr.
    CHECK_EQ(r.call(dos_lvo::PrintFault, 214, kData), DOSTRUE);
    CHECK(g_console == "Copy: volume is read-only\n");
    CHECK_EQ(r.io_err(), 214u);
    g_console.clear();
    CHECK_EQ(r.call(dos_lvo::PrintFault, 116, 0), DOSTRUE);
    CHECK(g_console == "mandatory argument not given\n");
    g_console.clear();
    CHECK_EQ(r.call(dos_lvo::PrintFault, 0, kData), DOSTRUE);
    CHECK(g_console.empty());

    // Bad pointers end the run.
    r.cpu().d[1] = 205;
    r.cpu().d[2] = 0;
    r.cpu().d[3] = kGuestBytes - 4;
    r.cpu().d[4] = 80;
    CHECK_EQ(r.dos(dos_lvo::Fault), ExitReason::BusError);
    r.cpu().d[1] = 205;
    r.cpu().d[2] = kGuestBytes;
    CHECK_EQ(r.dos(dos_lvo::PrintFault), ExitReason::BusError);
}

void get_var_finds_nothing() {
    Rig r;
    r.str(kData, "Language");
    r.put8(kData + 0x100, 0xEE);
    CHECK_EQ(as_signed(r.call(dos_lvo::GetVar, kData, kData + 0x100, 32, 0)), -1);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.get8(kData + 0x100), 0u);
    CHECK_EQ(as_signed(r.call(dos_lvo::GetVar, kData, 0, 0, 0)), -1);
}

// GetVar into kData + 0x100 with 0xEE beyond what it writes.
i32 get_var(Rig& r, const std::string& name, u32 size, u32 flags = 0) {
    r.str(kData, name);
    for (u32 i = 0; i < 64; ++i) {
        r.put8(kData + 0x100 + i, 0xEE);
    }
    return as_signed(r.call(dos_lvo::GetVar, kData, kData + 0x100, size, flags));
}

// SetVar of the text in kData + 0x200, its length or -1 as given.
u32 set_var(Rig& r, const std::string& name, const std::string& value, u32 flags = 0,
            u32 size = 0xFFFFFFFF) {
    r.str(kData, name);
    r.str(kData + 0x200, value);
    return r.call(dos_lvo::SetVar, kData, kData + 0x200, size, flags);
}

// The names on pr_LocalVars, in order, with their types.
std::vector<std::pair<std::string, u32>> local_vars(Rig& r) {
    std::vector<std::pair<std::string, u32>> out;
    const u32 list = r.proc() + pr_LocalVars;
    for (u32 n = r.get32(list + LH_HEAD); r.get32(n + LN_SUCC) != 0 && out.size() < 100;
         n = r.get32(n + LN_SUCC)) {
        out.emplace_back(r.read_str(r.get32(n + LN_NAME)), r.get8(n + LN_TYPE));
    }
    return out;
}

void local_variables() {
    Rig r;
    const auto before = r.chunks();
    CHECK_EQ(set_var(r, "Planet", "Mars"), DOSTRUE);
    CHECK(g_lines.empty());
    CHECK_EQ(get_var(r, "planet", 32), 4);
    CHECK(r.read_str(kData + 0x100) == "Mars");
    CHECK_EQ(r.io_err(), 4u);
    CHECK_EQ(get_var(r, "PLANET", 32, GVF_LOCAL_ONLY), 4);

    // The node is a LocalVar on the Process's list.
    const u32 node = r.get32(r.proc() + pr_LocalVars + LH_HEAD);
    CHECK(r.read_str(r.get32(node + LN_NAME)) == "Planet");
    CHECK_EQ(r.get8(node + LN_TYPE), LV_VAR);
    CHECK_EQ(r.get32(node + lv_Len), 4u);
    CHECK(r.bytes(r.get32(node + lv_Value), 4) == text("Mars"));
    CHECK_EQ(r.get32(node + LN_SUCC), r.proc() + pr_LocalVars + LH_TAIL);

    // A short buffer takes what fits and a NUL; IoErr is the whole size.
    CHECK_EQ(set_var(r, "planet", "Jupiter"), DOSTRUE);
    CHECK_EQ(local_vars(r).size(), 1u);
    CHECK_EQ(get_var(r, "Planet", 4), 3);
    CHECK(r.read_str(kData + 0x100) == "Jup");
    CHECK_EQ(r.get8(kData + 0x104), 0xEEu);
    CHECK_EQ(r.io_err(), 7u);
    CHECK_EQ(get_var(r, "Planet", 4, GVF_DONT_NULL_TERM), 4);
    CHECK(r.bytes(kData + 0x100, 5) == (std::vector<u8>{'J', 'u', 'p', 'i', 0xEE}));
    CHECK_EQ(get_var(r, "Planet", 1), 0);
    CHECK_EQ(r.get8(kData + 0x100), 0u);

    // A newline ends the value unless it is binary; the size is given.
    CHECK_EQ(set_var(r, "Two", "one\ntwo", 0, 7), DOSTRUE);
    CHECK_EQ(get_var(r, "two", 32), 3);
    CHECK(r.read_str(kData + 0x100) == "one");
    CHECK_EQ(r.io_err(), 7u);
    CHECK_EQ(get_var(r, "two", 32, GVF_BINARY_VAR), 7);
    CHECK(r.read_str(kData + 0x100) == "one\ntwo");
    CHECK_EQ(set_var(r, "Part", "abcdef", 0, 2), DOSTRUE);
    CHECK_EQ(get_var(r, "Part", 32), 2);
    CHECK(r.read_str(kData + 0x100) == "ab");
    CHECK_EQ(set_var(r, "Empty", "", 0, 0), DOSTRUE);
    CHECK_EQ(get_var(r, "Empty", 32), 0);
    CHECK_EQ(r.get8(kData + 0x100), 0u);

    // An alias is another type of the same name.
    CHECK_EQ(set_var(r, "Planet", "Venus", LV_ALIAS), DOSTRUE);
    CHECK_EQ(get_var(r, "Planet", 32), 7);
    CHECK_EQ(get_var(r, "Planet", 32, LV_ALIAS), 5);
    CHECK(r.read_str(kData + 0x100) == "Venus");
    CHECK((local_vars(r) == std::vector<std::pair<std::string, u32>>{
                                {"Planet", LV_VAR}, {"Two", LV_VAR}, {"Part", LV_VAR},
                                {"Empty", LV_VAR}, {"Planet", LV_ALIAS}}));
    // A node marked LVF_IGNORE is passed over.
    const u32 alias = r.get32(r.proc() + pr_LocalVars + LH_TAILPRED);
    r.put8(alias + LN_TYPE, LV_ALIAS | LVF_IGNORE);
    CHECK_EQ(get_var(r, "Planet", 32, LV_ALIAS), -1);
    r.put8(alias + LN_TYPE, LV_ALIAS);

    // GVF_GLOBAL_ONLY does not see local variables.
    CHECK_EQ(get_var(r, "Planet", 32, GVF_GLOBAL_ONLY), -1);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.get8(kData + 0x100), 0u);

    // A null buffer deletes, and every block goes back to exec.
    for (const char* name : {"planet", "TWO", "Part", "Empty"}) {
        r.str(kData, name);
        CHECK_EQ(r.call(dos_lvo::SetVar, kData, 0, 0, 0), DOSTRUE);
    }
    r.str(kData, "Planet");
    CHECK_EQ(r.call(dos_lvo::SetVar, kData, 0, 0, LV_ALIAS), DOSTRUE);
    CHECK(local_vars(r).empty());
    CHECK(r.chunks() == before);
    CHECK_EQ(r.call(dos_lvo::SetVar, kData, 0, 0, 0), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(get_var(r, "Planet", 32), -1);
    CHECK(g_lines.empty());
}

void variable_arguments_are_checked() {
    Rig r;
    CHECK_EQ(set_var(r, "Name", "value"), DOSTRUE);
    // No buffer or no room: ERROR_BAD_NUMBER, and nothing written.
    r.str(kData, "Name");
    CHECK_EQ(as_signed(r.call(dos_lvo::GetVar, kData, 0, 32, 0)), -1);
    CHECK_EQ(r.io_err(), ERROR_BAD_NUMBER);
    CHECK_EQ(get_var(r, "Name", 0), -1);
    CHECK_EQ(r.io_err(), ERROR_BAD_NUMBER);
    CHECK_EQ(get_var(r, "Name", 0x80000000), -1);
    CHECK_EQ(r.get8(kData + 0x100), 0xEEu);
    // No name, or one too long.
    CHECK_EQ(as_signed(r.call(dos_lvo::GetVar, 0, kData + 0x100, 32, 0)), -1);
    CHECK_EQ(r.io_err(), ERROR_REQUIRED_ARG_MISSING);
    CHECK_EQ(r.call(dos_lvo::SetVar, 0, kData + 0x200, 1, 0), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_REQUIRED_ARG_MISSING);
    const std::string long_name(300, 'n');
    CHECK_EQ(set_var(r, long_name, "x"), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_INVALID_COMPONENT_NAME);
    CHECK_EQ(get_var(r, long_name, 32), -1);
    CHECK_EQ(r.io_err(), ERROR_INVALID_COMPONENT_NAME);
    // Bad pointers end the run.
    r.str(kData, "Name");
    r.cpu().d[1] = kData;
    r.cpu().d[2] = kGuestBytes - 2;
    r.cpu().d[3] = 4;
    r.cpu().d[4] = 0;
    CHECK_EQ(r.dos(dos_lvo::SetVar), ExitReason::BusError);
    r.cpu().d[1] = kGuestBytes;
    r.cpu().d[2] = kData + 0x100;
    r.cpu().d[3] = 32;
    CHECK_EQ(r.dos(dos_lvo::GetVar), ExitReason::BusError);
    // A list that loops is walked a bounded number of steps.
    const u32 list = r.proc() + pr_LocalVars;
    const u32 node = r.get32(list + LH_HEAD);
    r.put32(node + LN_SUCC, node);
    CHECK_EQ(get_var(r, "Other", 32), -1);
    CHECK(logged("dos: the local variable list has no end"));
}

void str_to_long() {
    Rig r;
    const struct {
        const char* text;
        i32 taken;
        u32 value;
    } cases[] = {
        {"1234", 4, 1234},        {"  \t42 rest", 5, 42},   {"-17", 3, 0xFFFFFFEF},
        {"+8x", 2, 8},            {"007", 3, 7},            {"", -1, 0},
        {"   ", -1, 0},           {"-", -1, 0},             {"x12", -1, 0},
        {"- 5", -1, 0},           {"4294967297", 10, 1},    {"2147483648", 10, 0x80000000},
    };
    for (const auto& c : cases) {
        r.str(kData, c.text);
        r.put32(kData + 0x100, 0xDEADBEEF);
        CHECK_EQ(as_signed(r.call(dos_lvo::StrToLong, kData, kData + 0x100)), c.taken);
        CHECK_EQ(r.get32(kData + 0x100), c.value);
    }
    r.str(kData, "12");
    r.cpu().d[1] = kData;
    r.cpu().d[2] = kData + 0x101;
    CHECK_EQ(r.dos(dos_lvo::StrToLong), ExitReason::AddressError);
    r.put8(kGuestBytes - 1, '5');
    r.cpu().d[1] = kGuestBytes - 1;
    r.cpu().d[2] = kData + 0x100;
    CHECK_EQ(r.dos(dos_lvo::StrToLong), ExitReason::BusError);
}

void time_and_waiting() {
    Rig r;
    for (u32 i = 0; i < 12; ++i) {
        r.put8(kData + i, 0xEE);
    }
    CHECK_EQ(r.call(dos_lvo::DateStamp, kData), kData);
    for (u32 i = 0; i < 12; ++i) {
        CHECK_EQ(r.get8(kData + i), 0u);
    }
    r.call(dos_lvo::Delay, 50);
    CHECK_EQ(r.call(dos_lvo::WaitForChar, r.call(dos_lvo::Input), 1000000), DOSFALSE);
    r.cpu().d[1] = 1;
    CHECK_EQ(r.dos(dos_lvo::DateStamp), ExitReason::AddressError);
}

// --- files ----------------------------------------------------------------

void open_read_seek_close() {
    Rig r;
    const std::vector<u8> echo = echo_bytes();
    r.str(kData, "C:Echo");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    const u32 at = handle_at(fh);
    CHECK_EQ(r.get32(at + fh_Interactive), 0u);
    CHECK_EQ(r.call(dos_lvo::Read, fh, kData + 0x100, 600), 600u);
    CHECK(r.bytes(kData + 0x100, 600) == std::vector<u8>(echo.begin(), echo.begin() + 600));
    // Seek returns the old position.
    CHECK_EQ(r.call(dos_lvo::Seek, fh, 10, OFFSET_BEGINNING), 600u);
    CHECK_EQ(r.call(dos_lvo::Read, fh, kData + 0x100, 4), 4u);
    CHECK(r.bytes(kData + 0x100, 4) == std::vector<u8>(echo.begin() + 10, echo.begin() + 14));
    CHECK_EQ(r.call(dos_lvo::Seek, fh, 0xFFFFFFFE, OFFSET_CURRENT), 14u);
    CHECK_EQ(r.call(dos_lvo::Read, fh, kData + 0x100, 1), 1u);
    CHECK_EQ(r.get8(kData + 0x100), echo[12]);
    CHECK_EQ(r.call(dos_lvo::Seek, fh, 0xFFFFFFFC, OFFSET_END), 13u);
    CHECK_EQ(r.call(dos_lvo::Read, fh, kData + 0x100, 100), 4u);  // up to the end
    CHECK(r.bytes(kData + 0x100, 4) == std::vector<u8>(echo.end() - 4, echo.end()));
    // Reads past the end give 0.
    CHECK_EQ(r.call(dos_lvo::Read, fh, kData + 0x100, 100), 0u);
    CHECK_EQ(r.call(dos_lvo::Seek, fh, 1000, OFFSET_BEGINNING), 1000u);
    CHECK_EQ(r.call(dos_lvo::Read, fh, kGuestBytes - 1, 100), 0u);  // nothing to read: no fault
    // A position outside the file fails and stays where it was.
    CHECK_EQ(as_signed(r.call(dos_lvo::Seek, fh, 1001, OFFSET_BEGINNING)), -1);
    CHECK_EQ(r.io_err(), ERROR_SEEK_ERROR);
    CHECK_EQ(as_signed(r.call(dos_lvo::Seek, fh, 0xFFFFFFFF, OFFSET_BEGINNING)), -1);
    CHECK_EQ(as_signed(r.call(dos_lvo::Seek, fh, 0, 5)), -1);
    CHECK_EQ(r.call(dos_lvo::Seek, fh, 0, OFFSET_CURRENT), 1000u);
    // A read into memory that ends before the bytes do.
    r.call(dos_lvo::Seek, fh, 0, OFFSET_BEGINNING);
    r.cpu().d[1] = fh;
    r.cpu().d[2] = kGuestBytes - 2;
    r.cpu().d[3] = 4;
    CHECK_EQ(r.dos(dos_lvo::Read), ExitReason::BusError);
    // The volume is read-only.
    CHECK_EQ(as_signed(r.call(dos_lvo::Write, fh, kData, 1)), -1);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    CHECK_EQ(as_signed(r.call(dos_lvo::FPutC, fh, 'a')), -1);
    CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);

    // Missing files, directories, and files that would be written.
    r.str(kData, "C:Missing");
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    r.str(kData, "C:");
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    r.str(kData, "C:Echo");
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_READWRITE), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    r.str(kData, "C:New");
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    r.str(kData, "C:Echo");
    CHECK_EQ(r.call(dos_lvo::Open, kData, 42), 0u);
    CHECK_EQ(r.io_err(), ERROR_ACTION_NOT_KNOWN);
    // The console does not seek.
    CHECK_EQ(as_signed(r.call(dos_lvo::Seek, r.call(dos_lvo::Output), 0, OFFSET_BEGINNING)), -1);
    CHECK_EQ(r.io_err(), ERROR_ACTION_NOT_KNOWN);
}

void paths_resolve_like_amigados() {
    Rig r;
    struct Case {
        const char* path;
        u32 error;  // 0: opens
    };
    const Case cases[] = {
        {"C:Echo", 0},
        {"c:ECHO", 0},
        {"SYS:C/Echo", 0},
        {"sys:c/echo", 0},
        {"HombreTest:C/Echo", 0},
        {"hombretest:C/Echo", 0},
        {":C/Echo", 0},
        {"C/Echo", 0},                // relative to the root
        {"Devs/Sub/Deep", 0},
        {"Devs/Sub//Sub/Deep", 0},     // "//" goes up one level
        {"C:/S/Startup-Sequence", 0},  // a leading '/' goes up from C
        {"S:Startup-Sequence", 0},
        {"LIBS:x.library", 0},
        {"C:Echo/x", ERROR_OBJECT_WRONG_TYPE},
        {"/C/Echo", ERROR_OBJECT_NOT_FOUND},  // nothing above the root
        {"Other:C/Echo", ERROR_DEVICE_NOT_MOUNTED},
        {"Work:x", ERROR_DEVICE_NOT_MOUNTED},
        {"C:Ec:ho", ERROR_INVALID_COMPONENT_NAME},
        {"C:0123456789012345678901234567890", ERROR_INVALID_COMPONENT_NAME},
        {"0123456789012345678901234567890:x", ERROR_DEVICE_NOT_MOUNTED},
        {"", ERROR_OBJECT_WRONG_TYPE},  // the root itself
    };
    for (const Case& c : cases) {
        r.str(kData, c.path);
        r.set_io_err(0);
        const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
        if (c.error == 0) {
            CHECK(fh != 0);
            CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);
        } else {
            CHECK_EQ(fh, 0u);
            CHECK_EQ(r.io_err(), c.error);
        }
    }
    // A path of 30-character names longer than dos keeps.
    std::string deep;
    while (deep.size() < Dos::kMaxPath + 40) {
        deep += "abcdefghijklmnopqrstuvwxyz0123/";
    }
    r.str(kData, deep);
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_COMPONENT_NAME);
    // A name longer than dos reads.
    r.str(kData, std::string(2000, 'a'));
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_COMPONENT_NAME);
    // A name pointer out of reach.
    r.cpu().d[1] = kGuestBytes;
    r.cpu().d[2] = MODE_OLDFILE;
    CHECK_EQ(r.dos(dos_lvo::Open), ExitReason::BusError);
}

// With the 68000's 24-bit mask a pointer with flags in its top byte reaches
// what its low 24 bits reach, in dos's natives as in translated code.
void high_byte_pointers_reach_their_24_bit_alias() {
    Rig r;
    g_jit.set_address_mask(0x00FFFFFF);
    const u32 out = r.call(dos_lvo::Output);
    r.str(kData, "hello");
    CHECK_EQ(r.call(dos_lvo::Write, out, 0x80000000 | kData, 5), 5u);
    CHECK_EQ(r.call(dos_lvo::PutStr, 0xFF000000 | kData), 0u);
    r.str(kData + 0x100, "%s!");
    r.put32(kData + 0x200, 0x40000000 | kData);
    CHECK_EQ(r.call(dos_lvo::VPrintf, 0x80000000 | (kData + 0x100), 0x01000000 | (kData + 0x200)),
             6u);
    CHECK(g_console == "hellohellohello!");

    // Open and Read through tagged pointers.
    r.str(kData, "C:Echo");
    const u32 fh = r.call(dos_lvo::Open, 0x80000000 | kData, MODE_OLDFILE);
    CHECK(fh != 0);
    for (u32 i = 0; i < 8; ++i) {
        r.put8(kData + 0x300 + i, 0xEE);
    }
    CHECK_EQ(r.call(dos_lvo::Read, fh, 0x80000000 | (kData + 0x300), 4), 4u);
    const std::vector<u8> echo = echo_bytes();
    CHECK(r.bytes(kData + 0x300, 4) == std::vector<u8>(echo.begin(), echo.begin() + 4));
    CHECK_EQ(r.get8(kData + 0x304), 0xEEu);
    CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);

    // ReadArgs: template, array, RDArgs and its buffer all tagged.
    r.str(kTemplate, "FROM/A,TO");
    const u32 rda = kData + 0x800;
    for (u32 j = 0; j < RDA_SIZEOF; ++j) {
        r.put8(rda + j, 0);
    }
    const std::string line = "a b\n";
    r.str(kData + 0x1000, line);
    r.put32(rda + CS_Buffer, 0x80000000 | (kData + 0x1000));
    r.put32(rda + CS_Length, static_cast<u32>(line.size()));
    const u32 got =
        r.call(dos_lvo::ReadArgs, 0x80000000 | kTemplate, 0x80000000 | kArray, 0x80000000 | rda);
    CHECK_EQ(got, 0x80000000 | rda);
    CHECK(r.read_str(r.get32(kArray)) == "a");
    CHECK(r.read_str(r.get32(kArray + 4)) == "b");
    r.call(dos_lvo::FreeArgs, got);

    // A read that runs past the end of memory still faults, at the first
    // byte outside, as the pointer was given.
    r.str(kData, "C:Echo");
    r.cpu().d[1] = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    r.cpu().d[2] = 0x80000000 | (kGuestBytes - 2);
    r.cpu().d[3] = 8;
    CHECK_EQ(r.dos(dos_lvo::Read), ExitReason::BusError);
    CHECK_EQ(r.cpu().fault_addr, 0x80000000 | kGuestBytes);
}

void file_table_is_bounded() {
    Rig r;
    r.str(kData, "C:Echo");
    std::vector<u32> handles;
    for (u32 i = 0; i < Dos::kMaxFiles; ++i) {
        const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
        CHECK(fh != 0);
        handles.push_back(fh);
    }
    g_lines.clear();
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_FREE_STORE);
    CHECK(logged("dos: file handle table full, 16 entries in use"));
    // The console takes a slot too.
    g_lines.clear();
    r.str(kData + 0x200, "*");
    CHECK_EQ(r.call(dos_lvo::Open, kData + 0x200, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_FREE_STORE);
    CHECK(logged("dos: file handle table full, 16 entries in use"));
    // Each handle reads on its own.
    CHECK_EQ(r.call(dos_lvo::Read, handles[0], kData + 0x100, 10), 10u);
    CHECK_EQ(r.call(dos_lvo::Read, handles[1], kData + 0x100, 1), 1u);
    CHECK_EQ(r.get8(kData + 0x100), echo_bytes()[0]);
    CHECK_EQ(r.call(dos_lvo::Close, handles[3]), DOSTRUE);
    const u32 again = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK_EQ(handle_at(again), handle_at(handles[3]));
    CHECK(again != handles[3]);  // a new generation

    // Closing what is not open is logged and ignored.
    g_lines.clear();
    CHECK_EQ(r.call(dos_lvo::Close, again), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::Close, again), DOSFALSE);
    CHECK(logged_containing("dos: Close of a bad file handle"));
    CHECK_EQ(r.call(dos_lvo::Close, again + 1), DOSFALSE);  // inside a FileHandle
    CHECK_EQ(r.call(dos_lvo::Close, 0x3FFFFFFF), DOSFALSE);
    CHECK_EQ(r.call(dos_lvo::Close, 0xFFFFFFFF), DOSFALSE);
    CHECK_EQ(r.call(dos_lvo::Close, 0), DOSTRUE);  // Close(0) does nothing
    // A closed handle reads nothing.
    CHECK_EQ(as_signed(r.call(dos_lvo::Read, again, kData + 0x100, 1)), -1);
    // Closing the console leaves it working.
    const u32 out = r.call(dos_lvo::Output);
    CHECK_EQ(r.call(dos_lvo::Close, out), DOSTRUE);
    r.str(kData, "still");
    CHECK_EQ(r.call(dos_lvo::PutStr, kData), 0u);
    CHECK(g_console == "still");
    // The guest scribbling over a FileHandle changes nothing dos relies on.
    for (u32 i = 0; i < fh_SIZEOF; i += 4) {
        r.put32(handle_at(handles[0]) + i, 0xDEADBEEF);
    }
    CHECK_EQ(r.call(dos_lvo::Read, handles[0], kData + 0x100, 2), 2u);
    CHECK_EQ(r.get8(kData + 0x100), echo_bytes()[10]);
}

void ffs_volumes_read_too() {
    Rig r(standard_files(), true);
    r.str(kData, "C:Echo");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    CHECK_EQ(r.call(dos_lvo::Read, fh, kData + 0x100, 1000), 1000u);
    CHECK(r.bytes(kData + 0x100, 1000) == echo_bytes());
    u32 result = 0;
    CHECK(g_dos.run_command("C:Hello", "", result, kBlocks));
    CHECK(g_console == "hi\n");
}

// --- locks ----------------------------------------------------------------

void locks_and_the_current_directory() {
    Rig r;
    r.str(kData, "C:");
    const u32 c = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(c != 0);
    const u32 at = c << 2;
    CHECK_EQ(r.get32(at + fl_Access), SHARED_LOCK);
    CHECK(r.get32(at + fl_Key) != 0);
    CHECK(r.get32(at + fl_Task) != 0);
    CHECK_EQ(r.get32(at + fl_Volume) << 2,
             r.get32((r.get32(r.get32(r.base() + dl_Root) + rn_Info) << 2) + di_DevInfo) << 2);
    // CurrentDir returns the old directory; relative names follow it.
    CHECK_EQ(r.call(dos_lvo::CurrentDir, c), 0u);
    CHECK_EQ(r.get32(r.proc() + pr_CurrentDir), c);
    r.str(kData, "Echo");
    u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    r.call(dos_lvo::Close, fh);
    r.str(kData, "/S/Startup-Sequence");
    fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    r.call(dos_lvo::Close, fh);
    u32 result = 0;
    CHECK(g_dos.run_command("Hello", "", result, kBlocks));  // LoadSeg follows it too
    // DupLock gives a second lock on the same object.
    const u32 dup = r.call(dos_lvo::DupLock, c);
    CHECK(dup != 0);
    CHECK(dup != c);
    CHECK_EQ(r.get32((dup << 2) + fl_Key), r.get32(at + fl_Key));
    CHECK_EQ(r.call(dos_lvo::DupLock, 0), 0u);
    CHECK_EQ(r.call(dos_lvo::CurrentDir, dup), c);
    r.call(dos_lvo::UnLock, c);
    r.str(kData, "Echo");
    fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    r.call(dos_lvo::Close, fh);
    // A current directory that is not a lock.
    r.call(dos_lvo::CurrentDir, c);  // unlocked above
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);
    r.call(dos_lvo::CurrentDir, 0x12345);
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);
    // Absolute names do not care.
    r.str(kData, "C:Echo");
    fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    r.call(dos_lvo::Close, fh);
    r.call(dos_lvo::CurrentDir, 0);

    // Lock of files, of missing objects, and of "" (the current directory).
    r.str(kData, "C:Echo");
    const u32 file = r.call(dos_lvo::Lock, kData, EXCLUSIVE_LOCK);
    CHECK(file != 0);
    r.str(kData, "C:Missing");
    CHECK_EQ(r.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    r.str(kData, "");
    const u32 root = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(root != 0);
    CHECK_EQ(r.get32((root << 2) + fl_Key), r.volume.root_block);

    // UnLock of what is not a lock is logged and ignored; UnLock(0) is quiet.
    g_lines.clear();
    r.call(dos_lvo::UnLock, 0);
    CHECK(g_lines.empty());
    r.call(dos_lvo::UnLock, file);
    r.call(dos_lvo::UnLock, file);
    CHECK(logged_containing("dos: UnLock of a bad lock"));
    r.call(dos_lvo::UnLock, kData >> 2);
    r.call(dos_lvo::UnLock, root + 1);
    CHECK_EQ(r.call(dos_lvo::DupLock, file), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);

    // The table is bounded.
    r.call(dos_lvo::UnLock, root);
    r.call(dos_lvo::UnLock, dup);
    r.str(kData, "C:");
    u32 held = 0;
    for (u32 i = 0; i < Dos::kMaxLocks; ++i) {
        held = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(held != 0);
    }
    g_lines.clear();
    CHECK_EQ(r.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_FREE_STORE);
    CHECK(logged("dos: lock table full, 128 entries in use"));
    g_lines.clear();
    CHECK_EQ(r.call(dos_lvo::DupLock, held), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_FREE_STORE);
    CHECK(logged("dos: lock table full, 128 entries in use"));
}

struct Fib {
    u32 key;
    i32 type;
    std::string name;
    u32 size;
    u32 blocks;
};

Fib read_fib(const Rig& r, u32 at) {
    return Fib{r.get32(at + fib_DiskKey), as_signed(r.get32(at + fib_DirEntryType)),
               r.read_str(at + fib_FileName), r.get32(at + fib_Size), r.get32(at + fib_NumBlocks)};
}

void examine_and_ex_next() {
    Rig r;
    const u32 fib = kData + 0x400;
    r.str(kData, "C:Echo");
    const u32 file = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::Examine, file, fib), DOSTRUE);
    Fib f = read_fib(r, fib);
    CHECK(f.name == "Echo");
    CHECK_EQ(f.type, -3);
    CHECK_EQ(as_signed(r.get32(fib + fib_EntryType)), -3);
    CHECK_EQ(f.size, 1000u);
    CHECK_EQ(f.blocks, 3u);  // 488 bytes to an OFS block
    CHECK_EQ(f.key, r.get32((file << 2) + fl_Key));
    CHECK(r.read_str(fib + fib_Comment).empty());

    // A directory, then its entries one by one.
    r.str(kData, "C:");
    const u32 dir = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::Examine, dir, fib), DOSTRUE);
    f = read_fib(r, fib);
    CHECK(f.name == "C");
    CHECK_EQ(f.type, 2);
    std::vector<std::string> names;
    for (u32 i = 0; i < 20 && r.call(dos_lvo::ExNext, dir, fib) == DOSTRUE; ++i) {
        names.push_back(read_fib(r, fib).name);
    }
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"Args", "Echo", "Empty", "Entry", "Exit", "Hello",
                                             "Unknown"}));
    // The root by lock 0, with the volume's name.
    CHECK_EQ(r.call(dos_lvo::Examine, 0, fib), DOSTRUE);
    f = read_fib(r, fib);
    CHECK(f.name == "HombreTest");
    CHECK_EQ(f.type, 1);
    names.clear();
    for (u32 i = 0; i < 20 && r.call(dos_lvo::ExNext, 0, fib) == DOSTRUE; ++i) {
        names.push_back(read_fib(r, fib).name);
        CHECK_EQ(read_fib(r, fib).type, 2);
    }
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"C", "Devs", "LIBS", "S"}));

    // ExNext on a file, after a key that is not in the directory, on a
    // bad lock, and with bad pointers.
    CHECK_EQ(r.call(dos_lvo::Examine, file, fib), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::ExNext, file, fib), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    r.put32(fib + fib_DiskKey, 1500);  // no header there
    CHECK_EQ(r.call(dos_lvo::ExNext, dir, fib), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK_EQ(r.call(dos_lvo::Examine, 0x777, fib), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);
    r.cpu().d[1] = dir;
    r.cpu().d[2] = kGuestBytes - 8;
    CHECK_EQ(r.dos(dos_lvo::Examine), ExitReason::BusError);
    r.cpu().d[1] = dir;
    r.cpu().d[2] = fib + 1;
    CHECK_EQ(r.dos(dos_lvo::ExNext), ExitReason::AddressError);
}

// --- drives -----------------------------------------------------------------

std::vector<u8> second_disk(const std::string& label) {
    return build_adf(label.c_str(), false, {{"Docs/Readme", text("on the second disk")},
                                   {"Notes", text("notes")}});
}

std::string read_whole(Rig& r, const std::string& name) {
    r.str(kData + 0x600, name);
    const u32 fh = r.call(dos_lvo::Open, kData + 0x600, MODE_OLDFILE);
    CHECK(fh != 0);
    if (fh == 0) {
        return "";
    }
    const u32 n = r.call(dos_lvo::Read, fh, kData + 0x700, 64);
    r.call(dos_lvo::Close, fh);
    std::string text;
    for (u32 i = 0; i < n && i < 64; ++i) {
        text.push_back(static_cast<char>(r.get8(kData + 0x700 + i)));
    }
    return text;
}

void a_second_drive_is_its_own_volume() {
    Rig r(build_adf("HombreTest", false, standard_files()), second_disk("Second"), 1);

    // By device and by volume name, read from the second disk.
    CHECK(read_whole(r, "DF1:Docs/Readme") == "on the second disk");
    CHECK(read_whole(r, "Second:Notes") == "notes");
    CHECK(read_whole(r, "DF0:S/Startup-Sequence") == "echo hello\n");

    // Locks and handles carry the second drive's port, not RAM:'s.
    r.str(kData, "DF1:Docs");
    const u32 docs = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(docs != 0);
    r.str(kData + 0x80, "DF1:");
    const u32 dp = r.call(dos_lvo::GetDeviceProc, kData + 0x80, 0);
    CHECK(dp != 0);
    const u32 port1 = r.get32(dp + dvp_Port);
    r.call(dos_lvo::FreeDeviceProc, dp);
    const u32 port0 = r.call(dos_lvo::GetFileSysTask);
    CHECK(port1 != 0);
    CHECK(port1 != port0);
    CHECK_EQ(r.get32((docs << 2) + fl_Task), port1);
    r.str(kData + 0x100, "DF1:Notes");
    const u32 fh = r.call(dos_lvo::Open, kData + 0x100, MODE_OLDFILE);
    CHECK(fh != 0);
    CHECK_EQ(r.get32(handle_at(fh) + fh_Type), port1);
    r.call(dos_lvo::Close, fh);

    // The directory lists the second disk's entries.
    const u32 fib = kData + 0x800;
    r.str(kData, "Second:");
    const u32 root1 = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::Examine, root1, fib), DOSTRUE);
    CHECK(r.read_str(fib + fib_FileName) == "Second");
    std::vector<std::string> names;
    for (u32 i = 0; i < 10 && r.call(dos_lvo::ExNext, root1, fib) == DOSTRUE; ++i) {
        names.push_back(r.read_str(fib + fib_FileName));
    }
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"Docs", "Notes"}));

    // Two roots at the same block of different disks are not the same.
    r.str(kData, "DF0:");
    const u32 root0 = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::SameLock, root0, root1), LOCK_DIFFERENT);

    // Info names the unit and the volume; a packet to the drive's port
    // answers for that drive.
    const u32 id = kData + 0x400;
    CHECK_EQ(r.call(dos_lvo::Info, root1, id), DOSTRUE);
    CHECK_EQ(r.get32(id + id_UnitNumber), 1u);
    CHECK_EQ(r.get32(id + id_DiskType), ID_DOS_DISK);
    const u32 volume1 = r.get32(id + id_VolumeNode);
    CHECK(r.read_bstr(r.get32((volume1 << 2) + dol_Name)) == "Second");
    CHECK_EQ(r.call(dos_lvo::Info, root0, id), DOSTRUE);
    CHECK_EQ(r.get32(id + id_UnitNumber), 0u);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port1, ACTION_DISK_INFO, id >> 2), DOSTRUE);
    CHECK_EQ(r.get32(id + id_UnitNumber), 1u);
    CHECK_EQ(r.get32(id + id_VolumeNode), volume1);

    // The second disk is read-only like the first.
    r.str(kData, "DF1:New");
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);

    // The default assigns stay on the boot disk.
    r.str(kData, "C:");
    const u32 c = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(c != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, c, kData + 0x200, 64), DOSTRUE);
    CHECK(r.read_str(kData + 0x200) == "HombreTest:C");

    // The DosList lists both volumes and every drive.
    const u32 head = r.call(dos_lvo::LockDosList, LDF_ALL | LDF_READ);
    std::vector<std::string> volumes;
    u32 cur = head;
    for (u32 i = 0; i < 64; ++i) {
        cur = r.call(dos_lvo::NextDosEntry, cur, LDF_VOLUMES);
        if (cur == 0) {
            break;
        }
        volumes.push_back(r.read_bstr(r.get32(cur + dol_Name)));
    }
    r.call(dos_lvo::UnLockDosList, LDF_ALL | LDF_READ);
    CHECK(volumes == (std::vector<std::string>{"HombreTest", "Second", "Ram Disk"}));
    for (u32 lock : {docs, root1, root0, c}) {
        r.call(dos_lvo::UnLock, lock);
    }
}

void a_drive_without_a_disk_says_so() {
    Rig r(build_adf("HombreTest", false, standard_files()), second_disk("Third"), 2);

    // DF1: is there but empty; DF2: holds Third.
    r.str(kData, "DF1:");
    CHECK_EQ(r.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_DISK);
    r.str(kData, "DF1:Anything");
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_DISK);
    r.str(kData, "DF1:New");
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_DISK);
    CHECK(read_whole(r, "DF2:Notes") == "notes");
    r.str(kData, "DF3:");
    CHECK_EQ(r.call(dos_lvo::IsFileSystem, kData), DOSTRUE);
    r.str(kData, "DF4:");
    CHECK_EQ(r.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_DEVICE_NOT_MOUNTED);

    // Its port answers DISK_INFO with no disk present.
    r.str(kData + 0x80, "DF1:");
    const u32 dp = r.call(dos_lvo::GetDeviceProc, kData + 0x80, 0);
    CHECK(dp != 0);
    const u32 port = r.get32(dp + dvp_Port);
    CHECK(r.read_bstr(r.get32(r.get32(dp + dvp_DevNode) + dol_Name)) == "DF1");
    r.call(dos_lvo::FreeDeviceProc, dp);
    const u32 id = kData + 0x400;
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_DISK_INFO, id >> 2), DOSTRUE);
    CHECK_EQ(r.get32(id + id_DiskType), ID_NO_DISK_PRESENT);
    CHECK_EQ(r.get32(id + id_UnitNumber), 1u);
    CHECK_EQ(r.get32(id + id_VolumeNode), 0u);
}

void the_first_disk_wins_a_shared_name() {
    {
        Rig r(build_adf("HombreTest", false, standard_files()), second_disk("HombreTest"), 1);
        CHECK(read_whole(r, "HombreTest:S/Startup-Sequence") == "echo hello\n");
        CHECK(read_whole(r, "DF1:Notes") == "notes");
    }
    // A disk named RAM is reached before the Ram Disk's device name.
    Rig named_ram(build_adf("HombreTest", false, standard_files()), second_disk("RAM"), 3);
    CHECK(read_whole(named_ram, "RAM:Notes") == "notes");
    CHECK(read_whole(named_ram, "DF3:Docs/Readme") == "on the second disk");
}

void info_on_the_volume() {
    Rig r;
    const u32 id = kData + 0x400;
    CHECK_EQ(r.call(dos_lvo::Info, 0, id), DOSTRUE);
    CHECK_EQ(r.get32(id + id_DiskState), ID_WRITE_PROTECTED);
    CHECK_EQ(r.get32(id + id_NumBlocks), 1758u);
    CHECK_EQ(r.get32(id + id_BytesPerBlock), 488u);
    CHECK_EQ(r.get32(id + id_DiskType), ID_DOS_DISK);
    const u32 vol =
        r.get32((r.get32(r.get32(r.base() + dl_Root) + rn_Info) << 2) + di_DevInfo);
    CHECK_EQ(r.get32(id + id_VolumeNode), vol);
    r.str(kData, "C:");
    const u32 c = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::Info, c, id), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::Info, 0x777, id), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);
    r.cpu().d[1] = c;
    r.cpu().d[2] = kGuestBytes - 2;
    CHECK_EQ(r.dos(dos_lvo::Info), ExitReason::BusError);
}

// --- loading ----------------------------------------------------------------

void load_seg_and_unload_seg() {
    Rig r;
    const auto before = r.chunks();
    r.str(kData, "C:Hello");
    const u32 seg = r.call(dos_lvo::LoadSeg, kData);
    CHECK(seg != 0);
    // The code is the first segment's data.
    CHECK_EQ(r.get16((seg << 2) + 4), 0x2C78u);
    CHECK(r.chunks() != before);
    r.call(dos_lvo::UnLoadSeg, seg);
    CHECK(r.chunks() == before);
    r.call(dos_lvo::UnLoadSeg, 0);
    // NewLoadSeg ignores its tags.
    const u32 again = r.call(dos_lvo::NewLoadSeg, kData, 0);
    CHECK(again != 0);
    r.call(dos_lvo::UnLoadSeg, again);
    CHECK(r.chunks() == before);
    // Through the host interface too.
    const u32 host = g_dos.load_seg("C:Hello");
    CHECK(host != 0);
    r.call(dos_lvo::UnLoadSeg, host);
    CHECK(r.chunks() == before);
    // Missing.
    r.str(kData, "C:Missing");
    CHECK_EQ(r.call(dos_lvo::LoadSeg, kData), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    r.str(kData, "C:");
    CHECK_EQ(r.call(dos_lvo::LoadSeg, kData), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    CHECK(r.chunks() == before);
}

std::vector<u8> longs(std::initializer_list<u32> values) {
    std::vector<u8> out;
    for (u32 v : values) {
        put_be32(out, v);
    }
    return out;
}

void load_seg_of_bad_hunks() {
    // Each of these must give 0 with IoErr set, leave memory as it was and
    // not end the run.
    const std::vector<std::pair<std::vector<u8>, u32>> bad = {
        {text("not a hunk file at all"), ERROR_FILE_NOT_OBJECT},
        {{}, ERROR_FILE_NOT_OBJECT},
        {longs({1011}), ERROR_BAD_HUNK},                          // truncated header
        {longs({1011, 0, 1, 0, 0, 2, 1001, 2, 0}), ERROR_BAD_HUNK},  // truncated code
        {longs({1011, 0, 2, 0, 1, 1, 1, 1001, 1, 0x4E75, 1010}), ERROR_BAD_HUNK},  // hunk 2 missing
        {longs({1011, 0, 1, 0, 0, 1, 1001, 1, 0x4E75, 1004, 1, 0, 100, 0, 1010}),
         ERROR_BAD_HUNK},  // a relocation outside the hunk
        {longs({1011, 0, 1, 0, 0, 1, 1001, 1, 0x4E75, 1004, 1, 7, 0, 0, 1010}),
         ERROR_BAD_HUNK},  // a relocation to a hunk that is not there
        {longs({1011, 0, 1, 0, 0, 1, 1005, 1, 0x4E75, 1010}), ERROR_BAD_HUNK},  // unknown hunk
        {longs({1011, 0, 100, 0, 99}), ERROR_BAD_HUNK},                       // too many hunks
        {longs({1011, 0, 1, 0, 0, 0x00FFFFFF, 1001, 0, 1010}), ERROR_NO_FREE_STORE},
    };
    for (const auto& [file, error] : bad) {
        Rig r({{"LIBS/x", file}, {"LIBS/x.library", file}});
        const auto before = r.chunks();
        r.str(kData, "LIBS:x");
        CHECK_EQ(r.call(dos_lvo::LoadSeg, kData), 0u);
        CHECK_EQ(r.io_err(), error);
        CHECK(r.chunks() == before);
        CHECK_EQ(g_dos.load_seg("LIBS:x"), 0u);
        CHECK(r.chunks() == before);
        // OpenLibrary loads LIBS:x.library through dos, which fails cleanly.
        r.str(kData, "x.library");
        r.cpu().a[1] = kData;
        r.cpu().d[0] = 0;
        r.put16(kCall, 0x4EAE);
        r.put16(kCall + 2, disp(exec_lvo::OpenLibrary));
        r.put16(kCall + 4, 0x4EF8);
        r.put16(kCall + 6, 0x0400);
        g_jit.flush();
        r.cpu().a[6] = g_exec.exec_base();
        r.cpu().a[7] = kStack;
        r.cpu().pc = kCall;
        CHECK_EQ(g_jit.run(kBlocks), ExitReason::Halt);
        CHECK_EQ(r.cpu().d[0], 0u);
        CHECK(logged("exec: cannot load LIBS:x.library"));
        CHECK(r.chunks() == before);
    }
    // A two-hunk file whose second hunk is bad leaves no segment behind.
    Rig r({{"LIBS/x", longs({1011, 0, 2, 0, 1, 1, 1, 1001, 1, 0x4E75, 1010, 1001, 1, 0, 1005})}});
    const auto before = r.chunks();
    r.str(kData, "LIBS:x");
    CHECK_EQ(r.call(dos_lvo::LoadSeg, kData), 0u);
    CHECK(r.chunks() == before);
}

void unload_seg_of_bad_seglists() {
    Rig r;
    const auto before = r.chunks();
    g_lines.clear();
    // A seglist that points into nowhere, at itself, and at memory exec
    // never handed out: logged by exec, nothing freed, no crash.
    r.call(dos_lvo::UnLoadSeg, 0x3FFFFFFF);
    r.put32(kData, 16);
    r.put32(kData + 4, (kData + 4) >> 2);
    r.call(dos_lvo::UnLoadSeg, (kData + 4) >> 2);
    CHECK(r.chunks() == before);
}

// --- ReadArgs -------------------------------------------------------------

struct Parsed {
    u32 rdargs;
    u32 error;
};

// ReadArgs(tmpl, kArray, rdargs) on line through a CSource, with the
// array set to fill first.
Parsed read_args(Rig& r, const std::string& tmpl, const std::string& line,
                 std::initializer_list<u32> fill = {}) {
    for (u32 i = 0; i < 64; ++i) {
        r.put32(kArray + i * 4, 0x0BADBAD0);
    }
    u32 i = 0;
    for (u32 v : fill) {
        r.put32(kArray + 4 * i++, v);
    }
    r.str(kTemplate, tmpl);
    const u32 rda = kData + 0x800;
    for (u32 j = 0; j < RDA_SIZEOF; ++j) {
        r.put8(rda + j, 0);
    }
    const u32 buf = kData + 0x1000;
    for (usize j = 0; j < line.size(); ++j) {
        r.put8(buf + static_cast<u32>(j), static_cast<u8>(line[j]));
    }
    r.put32(rda + CS_Buffer, buf);
    r.put32(rda + CS_Length, static_cast<u32>(line.size()));
    r.put32(rda + CS_CurChr, 0);
    r.set_io_err(0);
    const u32 got = r.call(dos_lvo::ReadArgs, kTemplate, kArray, rda);
    return Parsed{got, r.io_err()};
}

std::string arg(const Rig& r, u32 i) {
    const u32 p = r.get32(kArray + 4 * i);
    return p == 0 ? "<null>" : r.read_str(p);
}

std::vector<std::string> multi(const Rig& r, u32 i) {
    std::vector<std::string> out;
    const u32 list = r.get32(kArray + 4 * i);
    for (u32 p = list; p != 0 && r.get32(p) != 0 && out.size() < 100; p += 4) {
        out.push_back(r.read_str(r.get32(p)));
    }
    return out;
}

i32 number(const Rig& r, u32 i) {
    return as_signed(r.get32(r.get32(kArray + 4 * i)));
}

void read_args_from_the_command_line() {
    Rig r;
    u32 result = 1;
    // Positional arguments.
    r.str(kTemplate, "X,Y");
    CHECK(g_dos.run_command("C:Args", "a b", result, kBlocks));
    CHECK(r.get32(kOut) != 0);
    CHECK_EQ(r.get32(kOut + 4), 0u);
    CHECK(arg(r, 0) == "a");
    CHECK(arg(r, 1) == "b");
    // A switch set by its keyword, before or after the name.
    r.str(kTemplate, "NAME/A,FLAG/S");
    r.put32(kArray + 4, 0);
    CHECK(g_dos.run_command("C:Args", "file FLAG", result, kBlocks));
    CHECK(arg(r, 0) == "file");
    CHECK_EQ(r.get32(kArray + 4), DOSTRUE);
    r.put32(kArray + 4, 0);
    CHECK(g_dos.run_command("C:Args", "flag file", result, kBlocks));
    CHECK(arg(r, 0) == "file");
    CHECK_EQ(r.get32(kArray + 4), DOSTRUE);
    r.put32(kArray + 4, 0);
    CHECK(g_dos.run_command("C:Args", "file", result, kBlocks));
    CHECK_EQ(r.get32(kArray + 4), 0u);
    // A missing /A argument.
    CHECK(g_dos.run_command("C:Args", "FLAG", result, kBlocks));
    CHECK_EQ(r.get32(kOut), 0u);
    CHECK_EQ(r.get32(kOut + 4), ERROR_REQUIRED_ARG_MISSING);
    CHECK(g_dos.run_command("C:Args", "", result, kBlocks));
    CHECK_EQ(r.get32(kOut + 4), ERROR_REQUIRED_ARG_MISSING);
    // A quoted argument is one argument.
    r.str(kTemplate, "X,Y");
    CHECK(g_dos.run_command("C:Args", "\"a b\" c", result, kBlocks));
    CHECK(arg(r, 0) == "a b");
    CHECK(arg(r, 1) == "c");
    // ReadArgs took the line from the console: nothing is left to read.
    // A lone "?" prints the template and reads the next line: there is none.
    r.str(kTemplate, "NAME/A");
    g_console.clear();
    CHECK(g_dos.run_command("C:Args", "?", result, kBlocks));
    CHECK(g_console == "NAME/A: ");
    CHECK_EQ(r.get32(kOut + 4), ERROR_REQUIRED_ARG_MISSING);
}

void read_args_positional_and_keywords() {
    Rig r;
    Parsed p = read_args(r, "X,Y", "a b\n");
    CHECK(p.rdargs != 0);
    CHECK(arg(r, 0) == "a");
    CHECK(arg(r, 1) == "b");
    // Fewer arguments leave the rest as they were.
    p = read_args(r, "X,Y", "a\n");
    CHECK(arg(r, 0) == "a");
    CHECK_EQ(r.get32(kArray + 4), 0x0BADBAD0u);
    // Keywords, with "=" or a space, in any case, out of turn.
    p = read_args(r, "X,Y", "Y=2 1\n");
    CHECK(arg(r, 0) == "1");
    CHECK(arg(r, 1) == "2");
    p = read_args(r, "X,Y", "y 2 1\n");
    CHECK(arg(r, 0) == "1");
    CHECK(arg(r, 1) == "2");
    p = read_args(r, "X,Y", "Y = 2 1\n");
    CHECK(arg(r, 1) == "2");
    // Aliases.
    p = read_args(r, "N=NAME/K,REST", "name foo bar\n");
    CHECK(arg(r, 0) == "foo");
    CHECK(arg(r, 1) == "bar");
    p = read_args(r, "N=NAME/K,REST", "N=foo\n");
    CHECK(arg(r, 0) == "foo");
    // A quoted keyword is a value.
    p = read_args(r, "X,Y", "\"y\" 2\n");
    CHECK(arg(r, 0) == "y");
    CHECK(arg(r, 1) == "2");
    // A keyword for an item already filled is a value.
    p = read_args(r, "X,Y", "a x\n");
    CHECK(arg(r, 1) == "x");
    // /K only by keyword.
    p = read_args(r, "X/K,Y", "foo\n");
    CHECK(arg(r, 1) == "foo");
    CHECK_EQ(r.get32(kArray), 0x0BADBAD0u);
    p = read_args(r, "X/K,Y", "foo bar\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_TOO_MANY_ARGS);
    p = read_args(r, "X/K,Y", "x foo bar\n");
    CHECK(arg(r, 0) == "foo");
    CHECK(arg(r, 1) == "bar");
    // A keyword without its value.
    p = read_args(r, "X/K", "x\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_KEY_NEEDS_ARG);
    p = read_args(r, "X/K", "x=\n");
    CHECK_EQ(p.error, ERROR_KEY_NEEDS_ARG);
    // /A/K that is not given.
    p = read_args(r, "X/A/K,Y", "foo\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_TOO_MANY_ARGS);
    // A stray "=".
    p = read_args(r, "X", "= a\n");
    CHECK_EQ(p.error, ERROR_BAD_TEMPLATE);
    // Too many arguments.
    p = read_args(r, "X", "a b\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_TOO_MANY_ARGS);
    // Tabs separate, ";" ends the line, and so does the end of the source.
    p = read_args(r, "X,Y", "\ta\t b ; c d\n");
    CHECK(arg(r, 0) == "a");
    CHECK(arg(r, 1) == "b");
    p = read_args(r, "X,Y", "a b");
    CHECK(arg(r, 1) == "b");
    p = read_args(r, "X", "");
    CHECK(p.rdargs != 0);
    CHECK_EQ(r.get32(kArray), 0x0BADBAD0u);
}

void read_args_switches_and_toggles() {
    Rig r;
    Parsed p = read_args(r, "Q=QUIET/S,ALL/S,NAME", "q x\n", {0, 0, 0});
    CHECK(p.rdargs != 0);
    CHECK_EQ(r.get32(kArray), DOSTRUE);
    CHECK_EQ(r.get32(kArray + 4), 0u);
    CHECK(arg(r, 2) == "x");
    // A switch is never filled by position.
    p = read_args(r, "A/S,B", "x\n", {0, 0});
    CHECK_EQ(r.get32(kArray), 0u);
    CHECK(arg(r, 1) == "x");
    // A switch cannot take "=value".
    p = read_args(r, "A/S,B", "A=1\n", {0, 0});
    CHECK_EQ(r.get32(kArray), DOSTRUE);
    CHECK(arg(r, 1) == "1");
    // /T inverts what the array held.
    p = read_args(r, "T/T", "t\n", {0});
    CHECK_EQ(r.get32(kArray), DOSTRUE);
    p = read_args(r, "T/T", "t\n", {DOSTRUE});
    CHECK_EQ(r.get32(kArray), 0u);
    p = read_args(r, "T/T", "\n", {5});
    CHECK_EQ(r.get32(kArray), 5u);
}

void read_args_numbers() {
    Rig r;
    struct Case {
        const char* text;
        i32 value;
    };
    const Case good[] = {{"42", 42},     {"-17", -17},       {"+5", 5},          {"0", 0},
                         {"$1F", 31},    {"$ff", 255},       {"0x1f", 31},       {"0XA0", 160},
                         {"-$10", -16},  {"2147483647", 2147483647}, {"-2147483648", INT32_MIN}};
    for (const Case& c : good) {
        Parsed p = read_args(r, "N/N", std::string(c.text) + "\n");
        CHECK(p.rdargs != 0);
        CHECK_EQ(number(r, 0), c.value);
    }
    for (const char* bad : {"abc", "12a", "-", "$", "0x", "$g", "1 2x", "--1", "\"\""}) {
        Parsed p = read_args(r, "N/N,M/N", std::string(bad) + "\n");
        CHECK_EQ(p.rdargs, 0u);
        CHECK_EQ(p.error, ERROR_BAD_NUMBER);
    }
    // A number argument not given stays as it was.
    Parsed p = read_args(r, "N/N,X", "\n");
    CHECK(p.rdargs != 0);
    CHECK_EQ(r.get32(kArray), 0x0BADBAD0u);
    // /N/K and /N/A.
    p = read_args(r, "SIZE/N/K,X/N/A", "size 7 9\n");
    CHECK_EQ(number(r, 0), 7);
    CHECK_EQ(number(r, 1), 9);
    p = read_args(r, "SIZE/N/K,X/N/A", "size=7\n");
    CHECK_EQ(p.error, ERROR_REQUIRED_ARG_MISSING);
}

void read_args_multiple() {
    Rig r;
    Parsed p = read_args(r, "FILES/M", "a b c\n");
    CHECK(p.rdargs != 0);
    CHECK(multi(r, 0) == (std::vector<std::string>{"a", "b", "c"}));
    // No arguments: the /M entry is null.
    p = read_args(r, "FILES/M", "\n");
    CHECK(p.rdargs != 0);
    CHECK_EQ(r.get32(kArray), 0u);
    p = read_args(r, "FILES/M/A", "\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_REQUIRED_ARG_MISSING);
    // /A items after the /M take the last ones.
    p = read_args(r, "FROM/M/A,TO/A", "a b c\n");
    CHECK(multi(r, 0) == (std::vector<std::string>{"a", "b"}));
    CHECK(arg(r, 1) == "c");
    p = read_args(r, "FROM/M/A,TO/A", "a\n");
    CHECK_EQ(p.error, ERROR_REQUIRED_ARG_MISSING);
    p = read_args(r, "FROM/M,TO/A,ALL/S", "a b ALL\n", {0, 0, 0});
    CHECK(multi(r, 0) == (std::vector<std::string>{"a"}));
    CHECK(arg(r, 1) == "b");
    CHECK_EQ(r.get32(kArray + 8), DOSTRUE);
    // A keyword for the /M.
    p = read_args(r, "X,FILES/M", "files a b\n");
    CHECK(multi(r, 1) == (std::vector<std::string>{"a", "b"}));
    CHECK_EQ(r.get32(kArray), 0x0BADBAD0u);
    // /M/N gives pointers to numbers.
    p = read_args(r, "N/M/N", "1 -2 $10\n");
    CHECK(p.rdargs != 0);
    const u32 list = r.get32(kArray);
    CHECK_EQ(as_signed(r.get32(r.get32(list))), 1);
    CHECK_EQ(as_signed(r.get32(r.get32(list + 4))), -2);
    CHECK_EQ(as_signed(r.get32(r.get32(list + 8))), 16);
    CHECK_EQ(r.get32(list + 12), 0u);
    p = read_args(r, "N/M/N", "1 x\n");
    CHECK_EQ(p.error, ERROR_BAD_NUMBER);
    // Many arguments.
    std::string many;
    for (u32 i = 0; i < 200; ++i) {
        many += "a ";
    }
    p = read_args(r, "F/M", many + "\n");
    CHECK(p.rdargs != 0);
    CHECK_EQ(multi(r, 0).size(), 100u);  // multi() stops at 100
    CHECK_EQ(r.get32(r.get32(kArray) + 200 * 4), 0u);
}

void read_args_rest_of_line() {
    Rig r;
    Parsed p = read_args(r, "CMD/F", "  echo  hi=there \"x\"\n");
    CHECK(p.rdargs != 0);
    CHECK(arg(r, 0) == "echo  hi=there \"x\"");
    p = read_args(r, "X,REST/F", "a b c d\n");
    CHECK(arg(r, 0) == "a");
    CHECK(arg(r, 1) == "b c d");
    p = read_args(r, "X/S,REST/F", "x y z\n", {0});
    CHECK_EQ(r.get32(kArray), DOSTRUE);
    CHECK(arg(r, 1) == "y z");
    // A quoted start loses its quotes.
    p = read_args(r, "REST/F", "\"a b\" c\n");
    CHECK(arg(r, 0) == "a b c");
    p = read_args(r, "REST/F/A", "\n");
    CHECK_EQ(p.error, ERROR_REQUIRED_ARG_MISSING);
}

void read_args_quotes() {
    Rig r;
    Parsed p = read_args(r, "X,Y", "\"x*\"y*Nz**w*e\" \"\"\n");
    CHECK(p.rdargs != 0);
    CHECK(arg(r, 0) == "x\"y\nz*w\x1b");
    CHECK(arg(r, 1).empty());
    p = read_args(r, "X,Y", "\"a=b\" c\n");
    CHECK(arg(r, 0) == "a=b");
    p = read_args(r, "X", "\"abc\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_LINE_TOO_LONG);
    p = read_args(r, "X", "\"abc*\n");
    CHECK_EQ(p.error, ERROR_LINE_TOO_LONG);
    // Other characters after '*' are kept as they are.
    p = read_args(r, "X", "\"*x\"\n");
    CHECK(arg(r, 0) == "x");
}

void read_args_templates_and_limits() {
    Rig r;
    // More items than dos takes.
    std::string tmpl = "A0";
    for (u32 i = 1; i < 100; ++i) {
        tmpl += ",A" + std::to_string(i);
    }
    Parsed p = read_args(r, tmpl, "x\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_BAD_TEMPLATE);
    // A template longer than dos reads.
    p = read_args(r, std::string(2000, 'A'), "x\n");
    CHECK_EQ(p.error, ERROR_BAD_TEMPLATE);
    // Unknown modifiers are ignored, as AmigaDOS does.
    p = read_args(r, "X/Q", "a\n");
    CHECK(arg(r, 0) == "a");
    // An empty template takes nothing.
    p = read_args(r, "", "\n");
    CHECK(p.rdargs != 0);
    // A line longer than dos reads.
    p = read_args(r, "X/F", std::string(2000, 'a') + "\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_LINE_TOO_LONG);
    // A CSource whose position is past its length reads an empty line.
    r.str(kTemplate, "X");
    const u32 rda = kData + 0x800;
    r.put32(rda + CS_CurChr, 50);
    r.put32(rda + CS_Length, 10);
    r.set_io_err(0);
    CHECK_EQ(r.call(dos_lvo::ReadArgs, kTemplate, kArray, rda), rda);
    CHECK_EQ(r.io_err(), 0u);
    // A CSource of absurd length is too long, not a fault.
    r.put32(rda + CS_CurChr, 0);
    r.put32(rda + CS_Length, 0x7FFFFFFF);
    CHECK_EQ(r.call(dos_lvo::ReadArgs, kTemplate, kArray, rda), 0u);
    CHECK_EQ(r.io_err(), ERROR_LINE_TOO_LONG);
    // The CSource position moves past what was read.
    p = read_args(r, "X", "abc");
    CHECK_EQ(r.get32(kData + 0x800 + CS_CurChr), 3u);

    // Bad pointers end the run.
    r.cpu().d[1] = kGuestBytes;
    r.cpu().d[2] = kArray;
    r.cpu().d[3] = 0;
    CHECK_EQ(r.dos(dos_lvo::ReadArgs), ExitReason::BusError);
    r.str(kTemplate, "X");
    r.put32(rda + CS_Buffer, kData + 0x1000);
    r.put32(rda + CS_Length, 2);
    r.str(kData + 0x1000, "a\n");
    r.put32(rda + CS_CurChr, 0);
    r.cpu().d[1] = kTemplate;
    r.cpu().d[2] = kArray + 1;
    r.cpu().d[3] = rda;
    CHECK_EQ(r.dos(dos_lvo::ReadArgs), ExitReason::AddressError);
    r.put32(rda + CS_CurChr, 0);
    r.cpu().d[1] = kTemplate;
    r.cpu().d[2] = kGuestBytes - 2;
    r.cpu().d[3] = rda;
    CHECK_EQ(r.dos(dos_lvo::ReadArgs), ExitReason::BusError);
    r.cpu().d[1] = kTemplate;
    r.cpu().d[2] = kArray;
    r.cpu().d[3] = kGuestBytes - 4;
    CHECK_EQ(r.dos(dos_lvo::ReadArgs), ExitReason::BusError);
    r.put32(rda + CS_Buffer, kGuestBytes - 1);
    r.put32(rda + CS_Length, 10);
    r.cpu().d[1] = kTemplate;
    r.cpu().d[2] = kArray;
    r.cpu().d[3] = rda;
    CHECK_EQ(r.dos(dos_lvo::ReadArgs), ExitReason::BusError);
    // A template that runs to the end of memory.
    for (u32 i = 0; i < 16; ++i) {
        r.put8(kGuestBytes - 16 + i, 'A');
    }
    r.cpu().d[1] = kGuestBytes - 16;
    r.cpu().d[2] = kArray;
    r.cpu().d[3] = 0;
    CHECK_EQ(r.dos(dos_lvo::ReadArgs), ExitReason::BusError);
}

void free_args_returns_the_memory() {
    Rig r;
    const auto before = r.chunks();
    Parsed p = read_args(r, "FILES/M,N/N", "a b c\n");
    CHECK(p.rdargs == kData + 0x800);
    CHECK(r.chunks() != before);
    r.call(dos_lvo::FreeArgs, p.rdargs);
    CHECK(r.chunks() == before);
    // ReadArgs with no RDArgs makes one, and FreeArgs frees it too.
    r.str(kTemplate, "X");
    r.str(kData + 0x1000, "a\n");  // unused: the console is read
    const u32 made = r.call(dos_lvo::ReadArgs, kTemplate, kArray, 0);
    CHECK(made != 0);
    CHECK(made != kData + 0x800);
    r.call(dos_lvo::FreeArgs, made);
    CHECK(r.chunks() == before);
    // A failed ReadArgs keeps nothing.
    p = read_args(r, "X/A", "\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK(r.chunks() == before);
    r.str(kTemplate, "X");
    CHECK_EQ(r.call(dos_lvo::ReadArgs, kTemplate, kArray, 0), made);  // the console is empty
    r.call(dos_lvo::FreeArgs, made);
    CHECK(r.chunks() == before);
    // FreeArgs of what ReadArgs did not return, twice, and of 0.
    g_lines.clear();
    r.call(dos_lvo::FreeArgs, 0);
    CHECK(g_lines.empty());
    r.call(dos_lvo::FreeArgs, made);
    CHECK(logged_containing("dos: FreeArgs of"));
    r.call(dos_lvo::FreeArgs, 0x12345678);
    CHECK(r.chunks() == before);
    // Two ReadArgs on one RDArgs: one FreeArgs frees both.
    read_args(r, "X", "a\n");
    r.call(dos_lvo::ReadArgs, kTemplate, kArray, kData + 0x800);
    r.call(dos_lvo::FreeArgs, kData + 0x800);
    CHECK(r.chunks() == before);
    // The table of results is bounded.
    for (u32 i = 0; i < Dos::kMaxArgBlocks; ++i) {
        CHECK(read_args(r, "X", "a\n").rdargs != 0);
    }
    g_lines.clear();
    p = read_args(r, "X", "a\n");
    CHECK_EQ(p.rdargs, 0u);
    CHECK_EQ(p.error, ERROR_NO_FREE_STORE);
    CHECK(logged("dos: ReadArgs result table full, 16 entries in use"));
    r.call(dos_lvo::FreeArgs, kData + 0x800);
    CHECK(r.chunks() == before);
}

// Random templates and lines, through the CSource: every outcome is a
// result or an error code, never a fault or a stray write.
void read_args_survives_any_input() {
    Rig r;
    const auto before = r.chunks();
    const char alphabet[] = "aAxX ,/=\"*\n\t;$-+0123456789MAKSNFT?";
    u32 seed = 12345;
    auto next = [&seed]() {
        seed = seed * 1103515245 + 12345;
        return (seed >> 16) & 0x7FFF;
    };
    for (u32 round = 0; round < 3000; ++round) {
        std::string tmpl;
        std::string line;
        const u32 tl = next() % 24;
        const u32 ll = next() % 40;
        for (u32 i = 0; i < tl; ++i) {
            tmpl += alphabet[next() % (sizeof alphabet - 1)];
        }
        for (u32 i = 0; i < ll; ++i) {
            line += alphabet[next() % (sizeof alphabet - 1)];
        }
        // Guard bytes around the array: only its items may change.
        r.put32(kArray - 4, 0x5A5A5A5A);
        Parsed p = read_args(r, tmpl, line);
        u32 items = 1;
        for (char c : tmpl) {
            items += c == ',' ? 1 : 0;
        }
        CHECK_EQ(r.get32(kArray - 4), 0x5A5A5A5Au);
        for (u32 i = items; i < 64; ++i) {
            CHECK_EQ(r.get32(kArray + 4 * i), 0x0BADBAD0u);
        }
        if (p.rdargs != 0) {
            r.call(dos_lvo::FreeArgs, p.rdargs);
        } else {
            CHECK(p.error != 0);
        }
        CHECK(r.chunks() == before);
    }
}

u32 exec_call(Rig& r, u32 lvo, u32 d0 = 0, u32 d1 = 0, u32 a1 = 0) {
    r.cpu().d[0] = d0;
    r.cpu().d[1] = d1;
    r.cpu().a[1] = a1;
    r.put16(kCall, 0x4EAE);
    r.put16(kCall + 2, disp(lvo));
    r.put16(kCall + 4, 0x4EF8);
    r.put16(kCall + 6, 0x0400);
    g_jit.flush();
    r.cpu().a[6] = g_exec.exec_base();
    r.cpu().a[7] = kStack;
    r.cpu().pc = kCall;
    CHECK_EQ(g_jit.run(kBlocks), ExitReason::Halt);
    return r.cpu().d[0];
}

// --- names, locks and ExAll -----------------------------------------------

void add_part_file_part_path_part() {
    Rig r;
    const u32 dir = kData;
    const u32 file = kData + 0x100;

    r.str(dir, "SYS:");
    r.str(file, "C");
    CHECK_EQ(r.call(dos_lvo::AddPart, dir, file, 64), DOSTRUE);
    CHECK(r.read_str(dir) == "SYS:C");

    r.str(file, "Dir");
    CHECK_EQ(r.call(dos_lvo::AddPart, dir, file, 64), DOSTRUE);
    CHECK(r.read_str(dir) == "SYS:C/Dir");

    r.str(dir, "SYS:C/");
    r.str(file, "Dir");
    CHECK_EQ(r.call(dos_lvo::AddPart, dir, file, 64), DOSTRUE);
    CHECK(r.read_str(dir) == "SYS:C/Dir");

    r.str(dir, "");
    r.str(file, "Dir");
    CHECK_EQ(r.call(dos_lvo::AddPart, dir, file, 64), DOSTRUE);
    CHECK(r.read_str(dir) == "Dir");

    r.str(dir, "SYS:C");
    r.str(file, "a:b");
    CHECK_EQ(r.call(dos_lvo::AddPart, dir, file, 64), DOSTRUE);
    CHECK(r.read_str(dir) == "a:b");

    // Too small leaves dirname unchanged and sets ERROR_LINE_TOO_LONG.
    r.str(dir, "SYS:C");
    r.str(file, "Dir");
    CHECK_EQ(r.call(dos_lvo::AddPart, dir, file, 9), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_LINE_TOO_LONG);
    CHECK(r.read_str(dir) == "SYS:C");

    // FilePart and PathPart return pointers into the guest string.
    r.str(dir, "SYS:C/Dir");
    CHECK_EQ(r.call(dos_lvo::FilePart, dir), dir + 6);
    CHECK(r.read_str(dir + 6) == "Dir");
    CHECK_EQ(r.call(dos_lvo::PathPart, dir), dir + 5);
    CHECK(r.read_str(dir + 5) == "/Dir");

    r.str(dir, "SYS:C");
    CHECK_EQ(r.call(dos_lvo::FilePart, dir), dir + 4);
    CHECK_EQ(r.call(dos_lvo::PathPart, dir), dir + 4);

    r.str(dir, "SYS:");
    CHECK_EQ(r.call(dos_lvo::FilePart, dir), dir + 4);
    CHECK_EQ(r.call(dos_lvo::PathPart, dir), dir + 4);

    r.str(dir, "Dir");
    CHECK_EQ(r.call(dos_lvo::FilePart, dir), dir);
    CHECK_EQ(r.call(dos_lvo::PathPart, dir), dir);
}

void name_from_lock_and_parent_dir() {
    Rig r;
    const u32 buf = kData + 0x100;

    // Root via 0 and via Lock("").
    CHECK_EQ(r.call(dos_lvo::NameFromLock, 0, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:");
    r.str(kData, "");
    const u32 root = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(root != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, root, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:");
    r.set_io_err(ERROR_INVALID_LOCK);
    CHECK_EQ(r.call(dos_lvo::ParentDir, root), 0u);
    CHECK_EQ(r.io_err(), 0u);
    CHECK_EQ(r.call(dos_lvo::ParentDir, 0), 0u);
    CHECK_EQ(r.io_err(), 0u);
    r.call(dos_lvo::UnLock, root);

    // Subdirectory and walking up to root.
    r.str(kData, "Devs/Sub/Deep");
    const u32 deep = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(deep != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, deep, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:Devs/Sub/Deep");
    CHECK_EQ(r.call(dos_lvo::NameFromLock, deep, buf, 10), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_LINE_TOO_LONG);

    const u32 p1 = r.call(dos_lvo::ParentDir, deep);
    CHECK(p1 != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, p1, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:Devs/Sub");
    const u32 p2 = r.call(dos_lvo::ParentDir, p1);
    CHECK(p2 != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, p2, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:Devs");
    const u32 p3 = r.call(dos_lvo::ParentDir, p2);
    CHECK(p3 != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, p3, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:");
    CHECK_EQ(r.call(dos_lvo::ParentDir, p3), 0u);
    CHECK_EQ(r.io_err(), 0u);

    // OpenFromLock consumes the lock on success, keeps it on failure.
    const u32 dir_fh = r.call(dos_lvo::OpenFromLock, p1);
    CHECK_EQ(dir_fh, 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    const u32 fh = r.call(dos_lvo::OpenFromLock, deep);
    CHECK(fh != 0);
    CHECK_EQ(r.call(dos_lvo::DupLock, deep), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);
    CHECK_EQ(r.call(dos_lvo::NameFromFH, fh, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:Devs/Sub/Deep");
    const u32 pfh = r.call(dos_lvo::ParentOfFH, fh);
    CHECK(pfh != 0);
    CHECK_EQ(r.call(dos_lvo::SameLock, pfh, p1), LOCK_SAME);
    r.call(dos_lvo::Close, fh);
    r.call(dos_lvo::UnLock, pfh);
    r.call(dos_lvo::UnLock, p1);
    r.call(dos_lvo::UnLock, p2);
    r.call(dos_lvo::UnLock, p3);

    // IsFileSystem.
    for (const char* name : {"SYS:", "HombreTest:", "C:", "LIBS:", "S:", ":", "C/Echo"}) {
        r.str(kData, name);
        CHECK_EQ(r.call(dos_lvo::IsFileSystem, kData), DOSTRUE);
    }
    for (const char* name : {"CONSOLE:", "*", "Work:"}) {
        r.str(kData, name);
        CHECK_EQ(r.call(dos_lvo::IsFileSystem, kData), DOSFALSE);
    }
}

void same_lock_and_dup_lock_from_fh() {
    Rig r;
    r.str(kData, "C:Echo");
    const u32 lock1 = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    const u32 lock2 = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    r.str(kData + 0x40, "S:Startup-Sequence");
    const u32 other = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::SameLock, lock1, lock2), LOCK_SAME);
    CHECK_EQ(r.call(dos_lvo::SameLock, lock1, other), LOCK_SAME_VOLUME);
    CHECK_EQ(r.call(dos_lvo::SameLock, 0x777, lock1), LOCK_DIFFERENT);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);

    r.str(kData, "");
    const u32 root = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::SameLock, 0, root), LOCK_SAME);

    const u32 fh = r.call(dos_lvo::Open, kData + 0x40, MODE_OLDFILE);
    CHECK(fh != 0);
    const u32 from_fh = r.call(dos_lvo::DupLockFromFH, fh);
    CHECK(from_fh != 0);
    CHECK_EQ(r.call(dos_lvo::SameLock, from_fh, other), LOCK_SAME);

    const u32 fib = kData + 0x200;
    CHECK_EQ(r.call(dos_lvo::ExamineFH, fh, fib), DOSTRUE);
    CHECK(read_fib(r, fib).name == "Startup-Sequence");
    CHECK_EQ(read_fib(r, fib).size, 11u);

    r.call(dos_lvo::Close, fh);
    r.call(dos_lvo::UnLock, from_fh);
    r.call(dos_lvo::UnLock, root);
    r.call(dos_lvo::UnLock, other);
    r.call(dos_lvo::UnLock, lock2);
    r.call(dos_lvo::UnLock, lock1);
}

void fgets_fgetc_ungetc_on_a_file() {
    Rig r({{"S/Lines", text("ab\ncde\nf")}});
    CHECK_EQ(r.call(dos_lvo::IsInteractive, r.call(dos_lvo::Input)), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::IsInteractive, r.call(dos_lvo::Output)), DOSTRUE);

    r.str(kData, "S:Lines");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    CHECK_EQ(r.call(dos_lvo::IsInteractive, fh), DOSFALSE);
    CHECK_EQ(r.call(dos_lvo::SetVBuf, fh, 0, 0, 256), 0u);

    // UnGetC(-1) fails before any read; after FGetC it pushes the byte back.
    CHECK_EQ(r.call(dos_lvo::UnGetC, fh, 0xFFFFFFFF), DOSFALSE);
    CHECK_EQ(r.call(dos_lvo::FGetC, fh), static_cast<u32>('a'));
    CHECK_EQ(r.call(dos_lvo::UnGetC, fh, 0xFFFFFFFF), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::UnGetC, fh, 'x'), DOSFALSE);
    CHECK_EQ(r.call(dos_lvo::FGetC, fh), static_cast<u32>('a'));
    CHECK_EQ(r.call(dos_lvo::UnGetC, fh, 'Z'), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::FGetC, fh), static_cast<u32>('Z'));

    const u32 buf = kData + 0x100;
    CHECK_EQ(r.call(dos_lvo::FGets, fh, buf, 16), buf);
    CHECK(r.read_str(buf) == "b\n");
    CHECK_EQ(r.call(dos_lvo::FGets, fh, buf, 4), buf);
    CHECK(r.read_str(buf) == "cde");
    CHECK_EQ(r.call(dos_lvo::FGets, fh, buf, 16), buf);
    CHECK(r.read_str(buf) == "\n");
    CHECK_EQ(r.call(dos_lvo::FGets, fh, buf, 16), buf);
    CHECK(r.read_str(buf) == "f");
    CHECK_EQ(r.call(dos_lvo::FGets, fh, buf, 16), 0u);
    CHECK_EQ(r.io_err(), 0u);
    CHECK_EQ(as_signed(r.call(dos_lvo::FGetC, fh)), -1);
    CHECK_EQ(r.io_err(), 0u);

    r.call(dos_lvo::Close, fh);
}

void fread_fwrite_counts() {
    Rig r;
    const std::vector<u8> echo = echo_bytes();
    r.str(kData, "C:Echo");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);

    const u32 buf = kData + 0x100;
    CHECK_EQ(r.call(dos_lvo::FRead, fh, buf, 10, 5), 5u);
    CHECK(r.bytes(buf, 50) == std::vector<u8>(echo.begin(), echo.begin() + 50));
    // 950 bytes remain: 3 full blocks of 300 bytes, plus 50 trailing bytes.
    CHECK_EQ(r.call(dos_lvo::FRead, fh, buf, 300, 4), 3u);
    CHECK(r.bytes(buf, 950) == std::vector<u8>(echo.begin() + 50, echo.end()));
    CHECK_EQ(r.call(dos_lvo::FRead, fh, buf, 10, 1), 0u);
    CHECK_EQ(r.call(dos_lvo::FWrite, fh, buf, 4, 2), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    r.call(dos_lvo::Close, fh);

    const u32 out = r.call(dos_lvo::Output);
    r.str(kData, "abcdef");
    CHECK_EQ(r.call(dos_lvo::FWrite, out, kData, 2, 3), 3u);
    CHECK(g_console == "abcdef");

    // VFWritef formats BCPL-style from a 32-bit argument array.
    g_console.clear();
    r.str(kData, "%S:%5N:%4X:%C:%6T:%%");
    r.str(kData + 0x100, "hi");
    r.str(kData + 0x120, "tag");
    r.put32(kData + 0x200, kData + 0x100);
    r.put32(kData + 0x204, 0xFFFFFFF9);  // -7
    r.put32(kData + 0x208, 0x2A);
    r.put32(kData + 0x20C, 'Q');
    r.put32(kData + 0x210, kData + 0x120);
    CHECK_EQ(r.call(dos_lvo::VFWritef, out, kData, kData + 0x200), 24u);
    CHECK(g_console == "hi:   -7:002A:Q:tag   :%");
}

void read_item_and_find_arg() {
    Asm a;
    a.open_dos();
    a.move_l(kOut, 1).moveq(64, 2).moveq(0, 3).dos(dos_lvo::ReadItem).store_d0(kOut + 128);
    a.move_l(kOut + 64, 1).moveq(64, 2).moveq(0, 3).dos(dos_lvo::ReadItem).store_d0(kOut + 132);
    a.moveq(0, 0).rts();
    Rig r({{"C/ItemCmd", hunk_file(a.build())}});

    // FindArg with aliases.
    r.str(kTemplate, "N=NAME/K,ALL/S,TO/A");
    r.str(kData, "name");
    CHECK_EQ(r.call(dos_lvo::FindArg, kTemplate, kData), 0u);
    r.str(kData, "n");
    CHECK_EQ(r.call(dos_lvo::FindArg, kTemplate, kData), 0u);
    r.str(kData, "ALL");
    CHECK_EQ(r.call(dos_lvo::FindArg, kTemplate, kData), 1u);
    r.str(kData, "to");
    CHECK_EQ(r.call(dos_lvo::FindArg, kTemplate, kData), 2u);
    r.str(kData, "other");
    CHECK_EQ(as_signed(r.call(dos_lvo::FindArg, kTemplate, kData)), -1);

    // ReadItem from a CSource.
    const std::string line = "key=\"a*\"b*Nc\" ; rest\n";
    const u32 cs = kData + 0x400;
    const u32 sbuf = kData + 0x500;
    const u32 item = kData + 0x600;
    r.str(sbuf, line);
    r.put32(cs + CS_Buffer, sbuf);
    r.put32(cs + CS_Length, static_cast<u32>(line.size()));
    r.put32(cs + CS_CurChr, 0);
    CHECK_EQ(r.call(dos_lvo::ReadItem, item, 64, cs), ITEM_UNQUOTED);
    CHECK(r.read_str(item) == "key");
    CHECK_EQ(r.call(dos_lvo::ReadItem, item, 64, cs), ITEM_EQUAL);
    CHECK_EQ(r.call(dos_lvo::ReadItem, item, 64, cs), ITEM_QUOTED);
    CHECK(r.read_str(item) == "a\"b\nc");
    CHECK_EQ(r.call(dos_lvo::ReadItem, item, 64, cs), ITEM_NOTHING);

    // Unclosed quote and buffer overflow return ITEM_ERROR.
    const std::string bad = "\"unclosed\n";
    r.str(sbuf, bad);
    r.put32(cs + CS_Length, static_cast<u32>(bad.size()));
    r.put32(cs + CS_CurChr, 0);
    CHECK_EQ(r.call(dos_lvo::ReadItem, item, 64, cs), ITEM_ERROR);

    const std::string long_item = "toolong\n";
    r.str(sbuf, long_item);
    r.put32(cs + CS_Length, static_cast<u32>(long_item.size()));
    r.put32(cs + CS_CurChr, 0);
    CHECK_EQ(r.call(dos_lvo::ReadItem, item, 4, cs), ITEM_ERROR);

    // ReadItem from Input() inside a 68000 command.
    u32 result = 1;
    CHECK(g_dos.run_command("C:ItemCmd", "one \"two three\"", result, kBlocks));
    CHECK_EQ(r.get32(kOut + 128), ITEM_UNQUOTED);
    CHECK(r.read_str(kOut) == "one");
    CHECK_EQ(r.get32(kOut + 132), ITEM_QUOTED);
    CHECK(r.read_str(kOut + 64) == "two three");
}

void alloc_dos_object_fib_and_free() {
    Rig r;
    const auto before = r.chunks();
    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);

    const u32 fib = r.call(dos_lvo::AllocDosObject, DOS_FIB, 0);
    CHECK(fib != 0);
    CHECK_EQ(fib & 3u, 0u);
    for (u32 i = 0; i < fib_SIZEOF; i += 4) {
        CHECK_EQ(r.get32(fib + i), 0u);
    }
    CHECK_EQ(r.call(dos_lvo::Examine, 0, fib), DOSTRUE);
    CHECK(read_fib(r, fib).name == "HombreTest");
    r.call(dos_lvo::FreeDosObject, DOS_FIB, fib);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);

    for (u32 type : {DOS_FILEHANDLE, DOS_EXALLCONTROL, DOS_STDPKT, DOS_CLI, DOS_RDARGS}) {
        const u32 obj = r.call(dos_lvo::AllocDosObject, type, 0);
        CHECK(obj != 0);
        CHECK_EQ(obj & 3u, 0u);
        r.call(dos_lvo::FreeDosObject, type, obj);
    }
    r.call(dos_lvo::FreeDosObject, DOS_FIB, 0);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);
}

void exall_lists_a_directory() {
    Rig r;
    r.str(kData, "C:");
    const u32 dir = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(dir != 0);
    const u32 eac = r.call(dos_lvo::AllocDosObject, DOS_EXALLCONTROL, 0);
    CHECK(eac != 0);

    // Small buffer forces multiple ExAll calls before ERROR_NO_MORE_ENTRIES.
    const u32 buf = kData + 0x200;
    std::vector<std::string> names;
    u32 more = DOSTRUE;
    for (u32 round = 0; round < 20 && more == DOSTRUE; ++round) {
        r.cpu().d[5] = eac;
        more = r.call(dos_lvo::ExAll, dir, buf, 48, ED_NAME, eac);
        if (more == DOSFALSE) {
            CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
        }
        const u32 count = r.get32(eac + eac_Entries);
        u32 got = 0;
        for (u32 cur = buf; cur != 0 && got < count; cur = r.get32(cur + ed_Next)) {
            names.push_back(r.read_str(r.get32(cur + ed_Name)));
            ++got;
        }
        CHECK_EQ(got, count);
    }
    CHECK_EQ(more, DOSFALSE);
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"Args", "Echo", "Empty", "Entry", "Exit", "Hello",
                                             "Unknown"}));

    // ED_COMMENT records include type, size, protection, date and comment.
    r.put32(eac + eac_LastKey, 0);
    r.cpu().d[5] = eac;
    CHECK_EQ(r.call(dos_lvo::ExAll, dir, buf, 512, ED_COMMENT, eac), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK_EQ(r.get32(eac + eac_Entries), 7u);
    bool saw_echo = false;
    for (u32 cur = buf; cur != 0; cur = r.get32(cur + ed_Next)) {
        if (r.read_str(r.get32(cur + ed_Name)) == "Echo") {
            CHECK_EQ(as_signed(r.get32(cur + ed_Type)), -3);
            CHECK_EQ(r.get32(cur + ed_Size), 1000u);
            CHECK(r.read_str(r.get32(cur + ed_Comment)).empty());
            saw_echo = true;
        }
    }
    CHECK(saw_echo);

    r.cpu().d[5] = eac;
    r.call(dos_lvo::ExAllEnd, dir, buf, 512, ED_COMMENT, eac);
    r.call(dos_lvo::FreeDosObject, DOS_EXALLCONTROL, eac);
    r.call(dos_lvo::UnLock, dir);
}

void cli_queries() {
    Asm a;
    a.open_dos();
    a.move_l(kOut, 1).moveq(64, 2).dos(dos_lvo::GetProgramName).store_d0(kOut + 64);
    a.dos(dos_lvo::GetArgStr).store_d0(kOut + 68);
    a.moveq(0, 0).rts();
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/CliInfo", hunk_file(a.build())});
    Rig r(files);

    CHECK_EQ(r.call(dos_lvo::MaxCli), 1u);
    CHECK_EQ(r.call(dos_lvo::FindCliProc, 1), r.proc());
    CHECK_EQ(r.call(dos_lvo::FindCliProc, 0), 0u);
    CHECK_EQ(r.call(dos_lvo::FindCliProc, 2), 0u);
    r.str(kData, "Resident");
    CHECK_EQ(r.call(dos_lvo::FindSegment, kData, 0, 0), 0u);

    const u32 buf = kData + 0x100;
    CHECK_EQ(r.call(dos_lvo::GetPrompt, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "%N.%S> ");
    r.str(kData, "Hombre> ");
    CHECK_EQ(r.call(dos_lvo::SetPrompt, kData), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::GetPrompt, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "Hombre> ");

    CHECK_EQ(r.call(dos_lvo::GetCurrentDirName, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:");
    r.str(kData, "HombreTest:C");
    CHECK_EQ(r.call(dos_lvo::SetCurrentDirName, kData), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::GetCurrentDirName, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C");

    r.str(kData, "MyProg");
    CHECK_EQ(r.call(dos_lvo::SetProgramName, kData), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::GetProgramName, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "MyProg");

    r.str(kData, "first args");
    const u32 old_arg = r.call(dos_lvo::SetArgStr, kData);
    CHECK_EQ(r.call(dos_lvo::GetArgStr), kData);
    CHECK_EQ(r.call(dos_lvo::SetArgStr, old_arg), kData);

    // GetProgramName and GetArgStr inside a command.
    u32 result = 1;
    CHECK(g_dos.run_command("C:CliInfo", "alpha beta", result, kBlocks));
    CHECK_EQ(r.get32(kOut + 64), DOSTRUE);
    CHECK(r.read_str(kOut) == "C:CliInfo");
    CHECK(r.read_str(r.get32(kOut + 68)) == "alpha beta\n");

    // CheckSignal uses exec's task signal state.
    exec_call(r, exec_lvo::Signal, SIGBREAKF_CTRL_C | (1u << 13), 0, r.proc());
    CHECK_EQ(r.call(dos_lvo::CheckSignal, SIGBREAKF_CTRL_C), SIGBREAKF_CTRL_C);
    CHECK_EQ(r.call(dos_lvo::CheckSignal, SIGBREAKF_CTRL_C), 0u);
    CHECK_EQ(r.call(dos_lvo::CheckSignal, 1u << 13), 1u << 13);

    // ErrorReport prints the fault text to the console and returns DOSTRUE.
    g_console.clear();
    CHECK_EQ(r.call(dos_lvo::ErrorReport, ERROR_DISK_WRITE_PROTECTED, 0, 0, 0), DOSTRUE);
    CHECK(g_console == "volume is read-only\n");
}

void find_var_and_delete_var() {
    Rig r;
    const auto before = r.chunks();
    CHECK_EQ(set_var(r, "Shell", "Hombre"), DOSTRUE);
    CHECK_EQ(set_var(r, "Shell", "Alias", LV_ALIAS), DOSTRUE);

    r.str(kData, "shell");
    const u32 var_node = r.call(dos_lvo::FindVar, kData, LV_VAR);
    CHECK(var_node != 0);
    CHECK(r.read_str(r.get32(var_node + LN_NAME)) == "Shell");
    CHECK_EQ(r.get32(var_node + lv_Len), 6u);

    const u32 alias_node = r.call(dos_lvo::FindVar, kData, LV_ALIAS);
    CHECK(alias_node != 0);
    CHECK(alias_node != var_node);

    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, LV_VAR), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::FindVar, kData, LV_VAR), 0u);
    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, LV_VAR), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, LV_ALIAS), DOSTRUE);
    CHECK(r.chunks() == before);
}

// --- patterns, anchors and dates -----------------------------------------

void clear_anchor(Rig& r, u32 anchor, u16 buf_len, u32 break_bits = 0) {
    for (u32 i = 0; i < ap_Buf + buf_len; ++i) {
        r.put8(anchor + i, 0);
    }
    r.put32(anchor + ap_BreakBits, break_bits);
    r.put16(anchor + ap_Strlen, buf_len);
}

void parse_and_match_pattern_lvos() {
    Rig r;
    const u32 pat = kData;
    const u32 tok = kData + 0x200;
    const u32 str = kData + 0x500;

    // Case-sensitive wild pattern.
    r.str(pat, "#?.info");
    CHECK_EQ(r.call(dos_lvo::ParsePattern, pat, tok, 64), 1u);
    r.str(str, "Disk.info");
    CHECK_EQ(r.call(dos_lvo::MatchPattern, tok, str), DOSTRUE);
    r.str(str, "Disk.INFO");
    CHECK_EQ(r.call(dos_lvo::MatchPattern, tok, str), DOSFALSE);

    // Case-sensitive plain pattern returns 0.
    r.str(pat, "Readme");
    CHECK_EQ(r.call(dos_lvo::ParsePattern, pat, tok, 64), 0u);
    r.str(str, "Readme");
    CHECK_EQ(r.call(dos_lvo::MatchPattern, tok, str), DOSTRUE);
    r.str(str, "readme");
    CHECK_EQ(r.call(dos_lvo::MatchPattern, tok, str), DOSFALSE);

    // Case-insensitive variants.
    r.str(pat, "#?.info");
    CHECK_EQ(r.call(dos_lvo::ParsePatternNoCase, pat, tok, 64), 1u);
    r.str(str, "Disk.INFO");
    CHECK_EQ(r.call(dos_lvo::MatchPatternNoCase, tok, str), DOSTRUE);
    r.str(str, "disk.info");
    CHECK_EQ(r.call(dos_lvo::MatchPatternNoCase, tok, str), DOSTRUE);
    r.str(str, "Disk.inf");
    CHECK_EQ(r.call(dos_lvo::MatchPatternNoCase, tok, str), DOSFALSE);

    r.str(pat, "(a|b)?[0-9]");
    CHECK_EQ(r.call(dos_lvo::ParsePatternNoCase, pat, tok, 64), 1u);
    r.str(str, "Bx7");
    CHECK_EQ(r.call(dos_lvo::MatchPatternNoCase, tok, str), DOSTRUE);
    r.str(str, "Cx7");
    CHECK_EQ(r.call(dos_lvo::MatchPatternNoCase, tok, str), DOSFALSE);

    // Buffer too small -> -1 with ERROR_LINE_TOO_LONG.
    r.str(pat, "#?.info");
    CHECK_EQ(as_signed(r.call(dos_lvo::ParsePattern, pat, tok, 4)), -1);
    CHECK_EQ(r.io_err(), ERROR_LINE_TOO_LONG);
    CHECK_EQ(as_signed(r.call(dos_lvo::ParsePatternNoCase, pat, tok, 0)), -1);
    CHECK_EQ(r.io_err(), ERROR_LINE_TOO_LONG);

    // Syntax error -> -1 with ERROR_BAD_TEMPLATE.
    r.str(pat, "(unclosed");
    CHECK_EQ(as_signed(r.call(dos_lvo::ParsePattern, pat, tok, 64)), -1);
    CHECK_EQ(r.io_err(), ERROR_BAD_TEMPLATE);

    // Bad pointer faults.
    r.cpu().d[1] = kGuestBytes;
    r.cpu().d[2] = tok;
    r.cpu().d[3] = 64;
    CHECK_EQ(r.dos(dos_lvo::ParsePattern), ExitReason::BusError);
}

void match_first_lists_wild_entries() {
    Rig r;
    const u32 pat = kData;
    const u32 anchor = kData + 0x200;

    // Wild pattern over C: with ap_Strlen set.
    clear_anchor(r, anchor, 64);
    r.str(pat, "C:E#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    CHECK((r.get8(anchor + ap_Flags) & APF_ITSWILD) != 0);

    std::vector<std::string> fib_names;
    std::vector<std::string> buf_paths;
    fib_names.push_back(read_fib(r, anchor + ap_Info).name);
    buf_paths.push_back(r.read_str(anchor + ap_Buf));
    for (u32 i = 0; i < 20; ++i) {
        const u32 rc = r.call(dos_lvo::MatchNext, anchor);
        if (rc != 0) {
            CHECK_EQ(rc, ERROR_NO_MORE_ENTRIES);
            break;
        }
        fib_names.push_back(read_fib(r, anchor + ap_Info).name);
        buf_paths.push_back(r.read_str(anchor + ap_Buf));
    }
    r.call(dos_lvo::MatchEnd, anchor);
    std::sort(fib_names.begin(), fib_names.end());
    std::sort(buf_paths.begin(), buf_paths.end());
    CHECK(fib_names == (std::vector<std::string>{"Echo", "Empty", "Entry", "Exit"}));
    CHECK(buf_paths == (std::vector<std::string>{"C:Echo", "C:Empty", "C:Entry", "C:Exit"}));

    // Subdirectory prefix and case-insensitive extension match.
    clear_anchor(r, anchor, 64);
    r.str(pat, "libs:#?.LIBRARY");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    CHECK(read_fib(r, anchor + ap_Info).name == "x.library");
    CHECK(r.read_str(anchor + ap_Buf) == "libs:x.library");
    CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), ERROR_NO_MORE_ENTRIES);
    r.call(dos_lvo::MatchEnd, anchor);

    // Relative wild pattern after CurrentDir.
    r.str(pat, "Devs/Sub");
    const u32 sub = r.call(dos_lvo::Lock, pat, SHARED_LOCK);
    CHECK(sub != 0);
    const u32 old_dir = r.call(dos_lvo::CurrentDir, sub);
    clear_anchor(r, anchor, 64);
    r.str(pat, "#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    CHECK(read_fib(r, anchor + ap_Info).name == "Deep");
    CHECK(r.read_str(anchor + ap_Buf) == "Deep");
    CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), ERROR_NO_MORE_ENTRIES);
    r.call(dos_lvo::MatchEnd, anchor);
    r.call(dos_lvo::CurrentDir, old_dir);
    r.call(dos_lvo::UnLock, sub);

    // ap_Strlen == 0 leaves ap_Buf untouched.
    clear_anchor(r, anchor, 0);
    r.put8(anchor + ap_Buf, 0xEE);
    r.str(pat, "S:#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    CHECK(read_fib(r, anchor + ap_Info).name == "Startup-Sequence");
    CHECK_EQ(r.get8(anchor + ap_Buf), 0xEEu);
    r.call(dos_lvo::MatchEnd, anchor);

    // ap_Strlen too small fails with ERROR_BUFFER_OVERFLOW.
    clear_anchor(r, anchor, 4);
    r.str(pat, "S:#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), ERROR_BUFFER_OVERFLOW);

    // Bad directory prefix fails with ERROR_DIR_NOT_FOUND.
    clear_anchor(r, anchor, 64);
    r.str(pat, "NoSuchDir/#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), ERROR_DIR_NOT_FOUND);

    // ap_BreakBits returns ERROR_BREAK and clears pending matching signals.
    exec_call(r, exec_lvo::Signal, SIGBREAKF_CTRL_C | (1u << 13), 0, r.proc());
    clear_anchor(r, anchor, 64, SIGBREAKF_CTRL_C);
    r.str(pat, "C:#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), ERROR_BREAK);
    CHECK_EQ(r.get32(anchor + ap_FoundBreak), SIGBREAKF_CTRL_C);
    CHECK_EQ(r.call(dos_lvo::CheckSignal, SIGBREAKF_CTRL_C), 0u);
    CHECK_EQ(r.call(dos_lvo::CheckSignal, 1u << 13), 1u << 13);
}

void match_first_plain_name() {
    Rig r;
    const u32 pat = kData;
    const u32 anchor = kData + 0x200;

    clear_anchor(r, anchor, 64);
    r.str(pat, "S:Startup-Sequence");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    CHECK_EQ(r.get8(anchor + ap_Flags) & APF_ITSWILD, 0u);
    const Fib f = read_fib(r, anchor + ap_Info);
    CHECK(f.name == "Startup-Sequence");
    CHECK_EQ(f.size, 11u);
    CHECK(r.read_str(anchor + ap_Buf) == "S:Startup-Sequence");
    CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), ERROR_NO_MORE_ENTRIES);
    r.call(dos_lvo::MatchEnd, anchor);

    // Missing plain object returns ERROR_OBJECT_NOT_FOUND.
    clear_anchor(r, anchor, 64);
    r.str(pat, "C:Missing");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), ERROR_OBJECT_NOT_FOUND);
    r.call(dos_lvo::MatchEnd, anchor);
}

void match_end_releases_anchor() {
    Rig r;
    const u32 pat = kData;
    r.str(pat, "C:#?");

    // 9 MatchFirst/MatchEnd pairs in a row (with distinct addresses) all work.
    for (u32 i = 0; i < 9; ++i) {
        const u32 anchor = kData + 0x200 + i * 0x200;
        clear_anchor(r, anchor, 32);
        CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
        r.call(dos_lvo::MatchEnd, anchor);
    }

    // Holding kMaxAnchors active anchors fills the table until one is ended.
    for (u32 i = 0; i < Dos::kMaxAnchors; ++i) {
        const u32 anchor = kData + 0x200 + i * 0x200;
        clear_anchor(r, anchor, 32);
        CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    }
    const u32 extra = kData + 0x200 + Dos::kMaxAnchors * 0x200;
    clear_anchor(r, extra, 32);
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, extra), ERROR_NO_FREE_STORE);

    // Reusing an active anchor resets its slot.
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, kData + 0x200), 0u);
    r.call(dos_lvo::MatchEnd, kData + 0x200);
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, extra), 0u);
    for (u32 i = 1; i < Dos::kMaxAnchors; ++i) {
        r.call(dos_lvo::MatchEnd, kData + 0x200 + i * 0x200);
    }
    r.call(dos_lvo::MatchEnd, extra);
}

void match_dodir_enters_directory() {
    Rig r;
    const u32 pat = kData;
    const u32 anchor = kData + 0x200;

    clear_anchor(r, anchor, 64);
    r.str(pat, "C");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    CHECK_EQ(r.get8(anchor + ap_Flags) & APF_ITSWILD, 0u);
    const Fib c_fib = read_fib(r, anchor + ap_Info);
    CHECK(c_fib.name == "C");
    CHECK(c_fib.type > 0);
    const u32 base_chain = r.get32(anchor + ap_Base);
    CHECK(base_chain != 0);
    CHECK_EQ(r.get32(anchor + ap_Current), base_chain);

    // Ask MatchNext to enter C.
    r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) | APF_DODIR);
    std::vector<std::string> entries;
    bool saw_diddir = false;
    for (u32 i = 0; i < 20; ++i) {
        const u32 rc = r.call(dos_lvo::MatchNext, anchor);
        if (rc != 0) {
            CHECK_EQ(rc, ERROR_NO_MORE_ENTRIES);
            break;
        }
        const u32 flags = r.get8(anchor + ap_Flags);
        CHECK_EQ(flags & APF_DODIR, 0u);
        if ((flags & APF_DIDDIR) != 0) {
            CHECK(!saw_diddir);
            saw_diddir = true;
            CHECK(read_fib(r, anchor + ap_Info).name == "C");
            CHECK_EQ(r.get32(anchor + ap_Current), base_chain);
            r.put8(anchor + ap_Flags, flags & ~APF_DIDDIR);
            continue;
        }
        if (entries.empty()) {
            CHECK((flags & APF_DirChanged) != 0);
            const u32 cur_chain = r.get32(anchor + ap_Current);
            CHECK(cur_chain != base_chain);
            CHECK_EQ(r.get32(cur_chain + an_Parent), base_chain);
            CHECK_EQ(r.get32(base_chain + an_Child), cur_chain);
        } else {
            CHECK_EQ(flags & APF_DirChanged, 0u);
        }
        entries.push_back(read_fib(r, anchor + ap_Info).name);
    }
    CHECK(saw_diddir);
    std::sort(entries.begin(), entries.end());
    CHECK(entries ==
          (std::vector<std::string>{"Args", "Echo", "Empty", "Entry", "Exit", "Hello", "Unknown"}));
    r.call(dos_lvo::MatchEnd, anchor);
    CHECK_EQ(r.get32(anchor + ap_Base), 0u);
    CHECK_EQ(r.get32(anchor + ap_Current), 0u);

    // Wild pattern entering a subdirectory keeps the wild pattern at depth 1.
    clear_anchor(r, anchor, 64);
    r.str(pat, "Devs/#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    CHECK(read_fib(r, anchor + ap_Info).name == "Sub");
    r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) | APF_DODIR);
    CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), 0u);
    CHECK((r.get8(anchor + ap_Flags) & APF_DirChanged) != 0);
    CHECK(read_fib(r, anchor + ap_Info).name == "Deep");
    CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), 0u);
    CHECK((r.get8(anchor + ap_Flags) & APF_DIDDIR) != 0);
    CHECK(read_fib(r, anchor + ap_Info).name == "Sub");
    r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) & ~APF_DIDDIR);
    CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), ERROR_NO_MORE_ENTRIES);
    r.call(dos_lvo::MatchEnd, anchor);
}

void match_wild_sets_achain_lock() {
    Rig r;
    const u32 pat = kData;
    const u32 anchor = kData + 0x200;
    const u32 name_buf = kData + 0x500;
    const u32 read_buf = kData + 0x600;

    clear_anchor(r, anchor, 64);
    r.str(pat, "S:#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    const u32 cur = r.get32(anchor + ap_Current);
    CHECK(cur != 0);
    CHECK_EQ(cur, r.get32(anchor + ap_Base));
    const u32 dir_lock = r.get32(cur + an_Lock);
    CHECK(dir_lock != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, dir_lock, name_buf, 64), DOSTRUE);
    CHECK(r.read_str(name_buf) == "HombreTest:S");
    CHECK(read_fib(r, cur + an_Info).name == "S");

    // CurrentDir to an_Lock + Open(fib_FileName) opens the matched file.
    const u32 old_dir = r.call(dos_lvo::CurrentDir, dir_lock);
    const u32 fh = r.call(dos_lvo::Open, anchor + ap_Info + fib_FileName, MODE_OLDFILE);
    CHECK(fh != 0);
    CHECK_EQ(r.call(dos_lvo::Read, fh, read_buf, 11), 11u);
    CHECK(r.read_str(read_buf) == "echo hello\n");
    r.call(dos_lvo::Close, fh);
    r.call(dos_lvo::CurrentDir, old_dir);
    r.call(dos_lvo::MatchEnd, anchor);
}

void match_buf_has_full_path_after_dodir() {
    Rig r;
    const u32 pat = kData;
    const u32 anchor = kData + 0x200;

    clear_anchor(r, anchor, 64);
    r.str(pat, "C");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    CHECK(r.read_str(anchor + ap_Buf) == "C");

    r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) | APF_DODIR);
    std::vector<std::string> paths;
    for (u32 i = 0; i < 20; ++i) {
        const u32 rc = r.call(dos_lvo::MatchNext, anchor);
        if (rc != 0) {
            CHECK_EQ(rc, ERROR_NO_MORE_ENTRIES);
            break;
        }
        const u32 flags = r.get8(anchor + ap_Flags);
        if ((flags & APF_DIDDIR) != 0) {
            CHECK(r.read_str(anchor + ap_Buf) == "C");
            r.put8(anchor + ap_Flags, flags & ~APF_DIDDIR);
            continue;
        }
        paths.push_back(r.read_str(anchor + ap_Buf));
    }
    std::sort(paths.begin(), paths.end());
    CHECK(paths == (std::vector<std::string>{"C/Args", "C/Echo", "C/Empty", "C/Entry", "C/Exit",
                                            "C/Hello", "C/Unknown"}));
    r.call(dos_lvo::MatchEnd, anchor);
}

void match_end_frees_everything() {
    Rig r;
    const u32 pat = kData;
    const u32 anchor = kData + 0x200;
    const auto chunks_before = r.chunks();
    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);

    // Complete walk with APF_DODIR.
    clear_anchor(r, anchor, 64);
    r.str(pat, "C");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) | APF_DODIR);
    for (u32 i = 0; i < 20; ++i) {
        const u32 rc = r.call(dos_lvo::MatchNext, anchor);
        if (rc != 0) {
            CHECK_EQ(rc, ERROR_NO_MORE_ENTRIES);
            break;
        }
        const u32 flags = r.get8(anchor + ap_Flags);
        if ((flags & APF_DIDDIR) != 0) {
            r.put8(anchor + ap_Flags, flags & ~APF_DIDDIR);
        }
    }
    r.call(dos_lvo::MatchEnd, anchor);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == chunks_before);

    // Aborted walk inside a subdirectory still frees all AChains and locks.
    clear_anchor(r, anchor, 64);
    r.str(pat, "C");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), 0u);
    r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) | APF_DODIR);
    CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), 0u);
    CHECK(r.get32(anchor + ap_Current) != r.get32(anchor + ap_Base));
    r.call(dos_lvo::MatchEnd, anchor);
    CHECK_EQ(r.get32(anchor + ap_Base), 0u);
    CHECK_EQ(r.get32(anchor + ap_Current), 0u);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == chunks_before);

    // Garbage ap_Base on an anchor not in the host table is ignored safely.
    clear_anchor(r, anchor, 64);
    r.put32(anchor + ap_Base, 0xDEAD0001u);
    r.str(pat, "C:Missing");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, pat, anchor), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.get32(anchor + ap_Base), 0u);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == chunks_before);
}

void put_bstr_at(Rig& r, u32 at, const std::string& s) {
    r.put8(at, static_cast<u8>(s.size()));
    for (usize i = 0; i < s.size(); ++i) {
        r.put8(at + 1 + static_cast<u32>(i), static_cast<u8>(s[i]));
    }
    r.put8(at + 1 + static_cast<u32>(s.size()), 0);
}

void dopkt_disk_info_and_unknown_action() {
    Rig r;
    const u32 port = r.call(dos_lvo::GetFileSysTask);
    CHECK(port != 0);

    const u32 id = kData + 0x100;
    const u32 id2 = kData + 0x200;
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_DISK_INFO, id >> 2), DOSTRUE);
    CHECK_EQ(r.io_err(), 0u);
    CHECK_EQ(r.call(dos_lvo::Info, 0, id2), DOSTRUE);
    for (u32 off = 0; off < id_SIZEOF; off += 4) {
        CHECK_EQ(r.get32(id + off), r.get32(id2 + off));
    }

    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_IS_FILESYSTEM), DOSTRUE);
    CHECK_EQ(r.io_err(), 0u);
    const u32 root = r.get32(r.base() + dl_Root);
    const u32 vol_bptr = r.get32((r.get32(root + rn_Info) << 2) + di_DevInfo);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_CURRENT_VOLUME), vol_bptr);
    CHECK_EQ(r.io_err(), 0u);

    // Locate, Info, CopyDir, SameLock, Parent, Examine, ExamineNext, FreeLock.
    const u32 bstr1 = kData + 0x300;
    const u32 bstr2 = kData + 0x340;
    const u32 fib = kData + 0x400;
    put_bstr_at(r, bstr1, "Devs");
    put_bstr_at(r, bstr2, "Sub");
    const u32 devs = r.call(dos_lvo::DoPkt, port, ACTION_LOCATE_OBJECT, 0, bstr1 >> 2, SHARED_LOCK);
    CHECK(devs != 0);
    const u32 sub =
        r.call(dos_lvo::DoPkt, port, ACTION_LOCATE_OBJECT, devs, bstr2 >> 2, SHARED_LOCK);
    CHECK(sub != 0);

    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_INFO, devs, id >> 2), DOSTRUE);
    CHECK_EQ(r.get32(id + id_DiskState), ID_WRITE_PROTECTED);

    const u32 dup = r.call(dos_lvo::DoPkt, port, ACTION_COPY_DIR, devs);
    CHECK(dup != 0);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_SAME_LOCK, devs, dup), LOCK_SAME);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_SAME_LOCK, devs, sub), LOCK_SAME_VOLUME);

    const u32 parent = r.call(dos_lvo::DoPkt, port, ACTION_PARENT, sub);
    CHECK(parent != 0);
    CHECK_EQ(r.call(dos_lvo::SameLock, parent, devs), LOCK_SAME);

    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_EXAMINE_OBJECT, sub, fib >> 2), DOSTRUE);
    CHECK(read_fib(r, fib).name == "Sub");
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_EXAMINE_NEXT, sub, fib >> 2), DOSTRUE);
    CHECK(read_fib(r, fib).name == "Deep");
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_EXAMINE_NEXT, sub, fib >> 2), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);

    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_FREE_LOCK, parent), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_FREE_LOCK, dup), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_FREE_LOCK, sub), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_FREE_LOCK, devs), DOSTRUE);

    // Unknown action fails with ERROR_ACTION_NOT_KNOWN and logs once.
    g_lines.clear();
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, 999), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_ACTION_NOT_KNOWN);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, 999), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_ACTION_NOT_KNOWN);
    u32 count = 0;
    for (const std::string& line : g_lines) {
        if (line == "dos: DoPkt action 999 not handled") {
            ++count;
        }
    }
    CHECK_EQ(count, 1u);

    // Another port fails with ERROR_ACTION_NOT_KNOWN.
    CHECK_EQ(r.call(dos_lvo::DoPkt, kData + 0x10, ACTION_DISK_INFO, id >> 2), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_ACTION_NOT_KNOWN);
}

void write_chars_and_get_file_sys_task() {
    Rig r;
    g_console.clear();
    r.str(kData, "hello world");
    CHECK_EQ(r.call(dos_lvo::WriteChars, kData, 5), 5u);
    CHECK(g_console == "hello");
    CHECK_EQ(r.call(dos_lvo::WriteChars, kData + 6, 5), 5u);
    CHECK(g_console == "helloworld");
    CHECK_EQ(r.call(dos_lvo::WriteChars, kData, 0), 0u);
    CHECK_EQ(r.call(dos_lvo::WriteChars, kData, 0xFFFFFFFFu), 0xFFFFFFFFu);
    CHECK_EQ(r.io_err(), ERROR_BAD_NUMBER);

    const u32 port = r.call(dos_lvo::GetFileSysTask);
    CHECK(port != 0);
    CHECK_EQ(port, r.get32(r.proc() + pr_FileSystemTask));
    const u32 fake_port = kData + 0x100;
    CHECK_EQ(r.call(dos_lvo::SetFileSysTask, fake_port), port);
    CHECK_EQ(r.call(dos_lvo::GetFileSysTask), fake_port);
    CHECK_EQ(r.call(dos_lvo::SetFileSysTask, port), fake_port);
}

void exall_honours_match_string() {
    Rig r;
    r.str(kData, "C:");
    const u32 dir = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(dir != 0);
    const u32 eac = r.call(dos_lvo::AllocDosObject, DOS_EXALLCONTROL, 0);
    CHECK(eac != 0);

    const u32 pat = kData + 0x100;
    const u32 tok = kData + 0x180;
    const u32 buf = kData + 0x200;
    r.str(pat, "e#?");
    CHECK_EQ(r.call(dos_lvo::ParsePatternNoCase, pat, tok, 64), 1u);
    r.put32(eac + eac_MatchString, tok);

    std::vector<std::string> names;
    u32 more = DOSTRUE;
    for (u32 round = 0; round < 10 && more == DOSTRUE; ++round) {
        r.cpu().d[5] = eac;
        more = r.call(dos_lvo::ExAll, dir, buf, 32, ED_NAME, eac);
        if (more == DOSFALSE) {
            CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
        }
        const u32 count = r.get32(eac + eac_Entries);
        u32 got = 0;
        for (u32 cur = buf; cur != 0 && got < count; cur = r.get32(cur + ed_Next)) {
            names.push_back(r.read_str(r.get32(cur + ed_Name)));
            ++got;
        }
        CHECK_EQ(got, count);
    }
    CHECK_EQ(more, DOSFALSE);
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"Echo", "Empty", "Entry", "Exit"}));

    // A pattern matching no entries ends immediately with 0 entries.
    r.str(pat, "zzz#?");
    CHECK_EQ(r.call(dos_lvo::ParsePatternNoCase, pat, tok, 64), 1u);
    r.put32(eac + eac_LastKey, 0);
    r.cpu().d[5] = eac;
    CHECK_EQ(r.call(dos_lvo::ExAll, dir, buf, 256, ED_NAME, eac), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK_EQ(r.get32(eac + eac_Entries), 0u);

    r.call(dos_lvo::FreeDosObject, DOS_EXALLCONTROL, eac);
    r.call(dos_lvo::UnLock, dir);
}

u64 fake_clock_2026() {
    const DosStamp s = dos_stamp_from_civil(CivilTime{2026, 11, 18, 17, 30, 45});
    return static_cast<u64>(s.days) * 1440ull * 3000ull +
           static_cast<u64>(s.minutes) * 3000ull + static_cast<u64>(s.ticks);
}

void date_stamp_uses_the_clock() {
    Rig r;
    CHECK_EQ(r.call(dos_lvo::DateStamp, kData), kData);
    CHECK_EQ(r.get32(kData + ds_Days), 0u);
    CHECK_EQ(r.get32(kData + ds_Minute), 0u);
    CHECK_EQ(r.get32(kData + ds_Tick), 0u);

    g_dos.set_clock(fake_clock_2026);
    CHECK_EQ(r.call(dos_lvo::DateStamp, kData), kData);
    CHECK_EQ(r.get32(kData + ds_Days), 17853u);
    CHECK_EQ(r.get32(kData + ds_Minute), 17u * 60u + 30u);
    CHECK_EQ(r.get32(kData + ds_Tick), 45u * 50u);

    g_dos.set_clock(nullptr);
    CHECK_EQ(r.call(dos_lvo::DateStamp, kData), kData);
    CHECK_EQ(r.get32(kData + ds_Days), 0u);
    CHECK_EQ(r.get32(kData + ds_Minute), 0u);
    CHECK_EQ(r.get32(kData + ds_Tick), 0u);
}

void date_to_str_and_str_to_date() {
    Rig r;
    g_dos.set_clock(fake_clock_2026);

    const u32 dt = kData;
    const u32 day_buf = kData + 0x40;
    const u32 date_buf = kData + 0x60;
    const u32 time_buf = kData + 0x80;

    // 2026-11-18 17:30:45 -> "18-Nov-26", "17:30:45", "Wednesday".
    r.put32(dt + dat_Stamp + ds_Days, 17853u);
    r.put32(dt + dat_Stamp + ds_Minute, 1050u);
    r.put32(dt + dat_Stamp + ds_Tick, 2250u);
    r.put8(dt + dat_Format, FORMAT_DOS);
    r.put8(dt + dat_Flags, 0);
    r.put32(dt + dat_StrDay, day_buf);
    r.put32(dt + dat_StrDate, date_buf);
    r.put32(dt + dat_StrTime, time_buf);
    CHECK_EQ(r.call(dos_lvo::DateToStr, dt), DOSTRUE);
    CHECK(r.read_str(day_buf) == "Wednesday");
    CHECK(r.read_str(date_buf) == "18-Nov-26");
    CHECK(r.read_str(time_buf) == "17:30:45");

    // Round-trip back through StrToDate.
    r.put32(dt + dat_Stamp + ds_Days, 0u);
    r.put32(dt + dat_Stamp + ds_Minute, 0u);
    r.put32(dt + dat_Stamp + ds_Tick, 0u);
    r.put32(dt + dat_StrDay, 0u);
    CHECK_EQ(r.call(dos_lvo::StrToDate, dt), DOSTRUE);
    CHECK_EQ(r.get32(dt + dat_Stamp + ds_Days), 17853u);
    CHECK_EQ(r.get32(dt + dat_Stamp + ds_Minute), 1050u);
    CHECK_EQ(r.get32(dt + dat_Stamp + ds_Tick), 2250u);

    // DTF_SUBST uses the clock's current day as today.
    r.put8(dt + dat_Flags, DTF_SUBST);
    r.put32(dt + dat_StrDay, day_buf);
    r.put32(dt + dat_StrDate, date_buf);
    r.put32(dt + dat_StrTime, 0u);
    CHECK_EQ(r.call(dos_lvo::DateToStr, dt), DOSTRUE);
    CHECK(r.read_str(date_buf) == "Today");
    CHECK(r.read_str(day_buf) == "Wednesday");

    // StrToDate accepts "today" and rejects bad text.
    r.str(date_buf, "today");
    r.put32(dt + dat_Stamp + ds_Days, 0u);
    r.put32(dt + dat_StrDay, 0u);
    r.put32(dt + dat_StrDate, date_buf);
    CHECK_EQ(r.call(dos_lvo::StrToDate, dt), DOSTRUE);
    CHECK_EQ(r.get32(dt + dat_Stamp + ds_Days), 17853u);

    r.str(date_buf, "99-Bad-26");
    CHECK_EQ(r.call(dos_lvo::StrToDate, dt), DOSFALSE);

    r.put8(dt + dat_Format, 99);
    CHECK_EQ(r.call(dos_lvo::DateToStr, dt), DOSFALSE);
}

void compare_dates_sign() {
    Rig r;
    const u32 a = kData;
    const u32 b = kData + 0x20;
    auto put_ds = [&](u32 addr, u32 d, u32 min, u32 t) {
        r.put32(addr + ds_Days, d);
        r.put32(addr + ds_Minute, min);
        r.put32(addr + ds_Tick, t);
    };

    put_ds(a, 100, 500, 1000);
    put_ds(b, 100, 500, 1000);
    CHECK_EQ(as_signed(r.call(dos_lvo::CompareDates, a, b)), 0);

    // date1 earlier than date2 -> > 0.
    put_ds(b, 101, 0, 0);
    CHECK(as_signed(r.call(dos_lvo::CompareDates, a, b)) > 0);
    put_ds(b, 100, 501, 0);
    CHECK(as_signed(r.call(dos_lvo::CompareDates, a, b)) > 0);
    put_ds(b, 100, 500, 1001);
    CHECK(as_signed(r.call(dos_lvo::CompareDates, a, b)) > 0);

    // date1 later than date2 -> < 0.
    put_ds(b, 99, 1400, 2999);
    CHECK(as_signed(r.call(dos_lvo::CompareDates, a, b)) < 0);
    put_ds(b, 100, 499, 2999);
    CHECK(as_signed(r.call(dos_lvo::CompareDates, a, b)) < 0);
    put_ds(b, 100, 500, 999);
    CHECK(as_signed(r.call(dos_lvo::CompareDates, a, b)) < 0);
}

// --- assigns and DosList --------------------------------------------------

void default_assigns_resolve() {
    {
        Rig r;
        const u32 fib = kData + 0x200;
        const u32 buf = kData + 0x400;

        struct Expected {
            const char* path;
            const char* name;
            i32 type;
        };
        const Expected present[] = {
            {"SYS:", "HombreTest", 1},
            {"DF0:", "HombreTest", 1},
            {"C:", "C", 2},
            {"S:", "S", 2},
            {"LIBS:", "LIBS", 2},
            {"DEVS:", "Devs", 2},
        };
        for (const Expected& e : present) {
            r.str(kData, e.path);
            const u32 lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
            CHECK(lock != 0);
            CHECK_EQ(r.call(dos_lvo::Examine, lock, fib), DOSTRUE);
            const Fib f = read_fib(r, fib);
            CHECK(f.name == e.name);
            CHECK_EQ(f.type, e.type);
            r.call(dos_lvo::UnLock, lock);
        }

        // Info and NameFromLock work on DF0: and inside an assigned directory.
        r.str(kData, "DF0:C/Echo");
        const u32 df0_echo = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(df0_echo != 0);
        CHECK_EQ(r.call(dos_lvo::NameFromLock, df0_echo, buf, 64), DOSTRUE);
        CHECK(r.read_str(buf) == "HombreTest:C/Echo");
        CHECK_EQ(r.call(dos_lvo::Info, df0_echo, buf), DOSTRUE);
        CHECK_EQ(r.get32(buf + id_DiskState), ID_WRITE_PROTECTED);
        r.call(dos_lvo::UnLock, df0_echo);

        r.str(kData, "C:Echo");
        const u32 c_echo = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(c_echo != 0);
        CHECK_EQ(r.call(dos_lvo::NameFromLock, c_echo, buf, 64), DOSTRUE);
        CHECK(r.read_str(buf) == "HombreTest:C/Echo");
        r.call(dos_lvo::UnLock, c_echo);

        // Absent directories have no assigns; T: and ENV: point to RAM:.
        for (const char* missing : {"FONTS:", "L:", "ENVARC:"}) {
            r.str(kData, missing);
            CHECK_EQ(r.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
            CHECK_EQ(r.io_err(), ERROR_DEVICE_NOT_MOUNTED);
        }
        for (const char* ram_assign : {"T:", "ENV:"}) {
            r.str(kData, ram_assign);
            const u32 lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
            CHECK(lock != 0);
            r.call(dos_lvo::UnLock, lock);
        }
    }

    // ENVARC and other standard directories appear when present on the volume.
    std::vector<AdfFile> wb_files = standard_files();
    wb_files.push_back({"Prefs/Env-Archive/sys.prefs", text("ok")});
    wb_files.push_back({"FONTS/topaz.font", text("font")});
    wb_files.push_back({"ENV/var", text("1")});
    wb_files.push_back({"T/tmp", text("1")});
    Rig wb(wb_files);
    const u32 buf = kData + 0x400;
    wb.str(kData, "ENVARC:sys.prefs");
    const u32 envarc = wb.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(envarc != 0);
    CHECK_EQ(wb.call(dos_lvo::NameFromLock, envarc, buf, 64), DOSTRUE);
    CHECK(wb.read_str(buf) == "HombreTest:Prefs/Env-Archive/sys.prefs");
    wb.call(dos_lvo::UnLock, envarc);
    wb.str(kData, "FONTS:topaz.font");
    const u32 font = wb.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(font != 0);
    wb.call(dos_lvo::UnLock, font);
    // ENV: and T: still resolve on RAM:, not DF0:ENV / DF0:T.
    for (const char* on_adf_only : {"ENV:var", "T:tmp"}) {
        wb.str(kData, on_adf_only);
        CHECK_EQ(wb.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
        CHECK_EQ(wb.io_err(), ERROR_OBJECT_NOT_FOUND);
    }
}

void assign_lock_and_remove() {
    Rig r;
    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);
    const auto before = r.chunks();

    r.str(kData, "C:");
    const u32 lock_c = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(lock_c != 0);
    r.str(kData, "FOO");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, lock_c), DOSTRUE);

    r.str(kData + 0x40, "FOO:Echo");
    const u32 echo_lock = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK(echo_lock != 0);
    const u32 buf = kData + 0x200;
    CHECK_EQ(r.call(dos_lvo::NameFromLock, echo_lock, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C/Echo");
    r.call(dos_lvo::UnLock, echo_lock);

    // Removing the assign frees its node and restores AvailMem.
    r.str(kData, "FOO");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, 0), DOSTRUE);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);

    r.str(kData + 0x40, "FOO:Echo");
    CHECK_EQ(r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_DEVICE_NOT_MOUNTED);
}

void assign_add_multi_directory() {
    Rig r;
    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);
    const auto before = r.chunks();

    r.str(kData, "C:");
    const u32 lock_c = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    r.str(kData + 0x40, "S:");
    const u32 lock_s = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK(lock_c != 0 && lock_s != 0);

    r.str(kData, "FOO");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, lock_c), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::AssignAdd, kData, lock_s), DOSTRUE);

    // First directory and second directory both resolve.
    r.str(kData + 0x80, "FOO:Echo");
    const u32 l1 = r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK);
    CHECK(l1 != 0);
    r.call(dos_lvo::UnLock, l1);

    r.str(kData + 0x80, "FOO:Startup-Sequence");
    const u32 l2 = r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK);
    CHECK(l2 != 0);
    const u32 buf = kData + 0x200;
    CHECK_EQ(r.call(dos_lvo::NameFromLock, l2, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:S/Startup-Sequence");
    r.call(dos_lvo::UnLock, l2);

    const u32 fh = r.call(dos_lvo::Open, kData + 0x80, MODE_OLDFILE);
    CHECK(fh != 0);
    r.call(dos_lvo::Close, fh);

    // RemAssignList removes one directory by lock match.
    r.str(kData + 0x40, "C:");
    const u32 cmp_c = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK(cmp_c != 0);
    CHECK_EQ(r.call(dos_lvo::RemAssignList, kData, cmp_c), DOSTRUE);
    r.call(dos_lvo::UnLock, cmp_c);

    r.str(kData + 0x80, "FOO:Echo");
    CHECK_EQ(r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    r.str(kData + 0x80, "FOO:Startup-Sequence");
    const u32 l3 = r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK);
    CHECK(l3 != 0);
    r.call(dos_lvo::UnLock, l3);

    // Removing the last directory removes the assign itself.
    r.str(kData + 0x40, "S:");
    const u32 cmp_s = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::RemAssignList, kData, cmp_s), DOSTRUE);
    r.call(dos_lvo::UnLock, cmp_s);
    CHECK_EQ(r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_DEVICE_NOT_MOUNTED);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);
}

void assign_late_and_path() {
    Rig r;
    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);

    // Late assign stays DLT_LATE until first use, then resolves to DLT_DIRECTORY.
    r.str(kData, "LATE");
    r.str(kData + 0x40, "SYS:S");
    CHECK_EQ(r.call(dos_lvo::AssignLate, kData, kData + 0x40), DOSTRUE);

    u32 dl = r.call(dos_lvo::LockDosList, LDF_ASSIGNS | LDF_READ);
    u32 node = r.call(dos_lvo::FindDosEntry, dl, kData, LDF_ASSIGNS);
    CHECK(node != 0);
    CHECK_EQ(r.get32(node + dol_Type), DLT_LATE);
    CHECK(r.read_str(r.get32(node + dol_AssignName)) == "SYS:S");
    r.call(dos_lvo::UnLockDosList, LDF_ASSIGNS | LDF_READ);

    r.str(kData + 0x80, "LATE:Startup-Sequence");
    const u32 late_lock = r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK);
    CHECK(late_lock != 0);
    r.call(dos_lvo::UnLock, late_lock);

    dl = r.call(dos_lvo::LockDosList, LDF_ASSIGNS | LDF_READ);
    node = r.call(dos_lvo::FindDosEntry, dl, kData, LDF_ASSIGNS);
    CHECK(node != 0);
    CHECK_EQ(r.get32(node + dol_Type), DLT_DIRECTORY);
    CHECK(r.get32(node + dol_Lock) != 0);
    r.call(dos_lvo::UnLockDosList, LDF_ASSIGNS | LDF_READ);

    // Non-binding path assign re-evaluates its target path on each use.
    r.str(kData, "TARGET");
    r.str(kData + 0x40, "C:");
    const u32 c_lock = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, c_lock), DOSTRUE);

    r.str(kData + 0x20, "PATH");
    r.str(kData + 0x60, "TARGET:");
    CHECK_EQ(r.call(dos_lvo::AssignPath, kData + 0x20, kData + 0x60), DOSTRUE);

    r.str(kData + 0x80, "PATH:Echo");
    const u32 p1 = r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK);
    CHECK(p1 != 0);
    r.call(dos_lvo::UnLock, p1);

    // Redirecting TARGET: redirects PATH: too, while PATH stays DLT_NONBINDING.
    r.str(kData + 0x40, "S:");
    const u32 s_lock = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, s_lock), DOSTRUE);
    r.str(kData + 0x80, "PATH:Startup-Sequence");
    const u32 p2 = r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK);
    CHECK(p2 != 0);
    r.call(dos_lvo::UnLock, p2);

    dl = r.call(dos_lvo::LockDosList, LDF_ASSIGNS | LDF_READ);
    node = r.call(dos_lvo::FindDosEntry, dl, kData + 0x20, LDF_ASSIGNS);
    CHECK(node != 0);
    CHECK_EQ(r.get32(node + dol_Type), DLT_NONBINDING);
    r.call(dos_lvo::UnLockDosList, LDF_ASSIGNS | LDF_READ);

    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, 0), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData + 0x20, 0), DOSTRUE);
    r.str(kData, "LATE");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, 0), DOSTRUE);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
}

void dos_list_walk() {
    Rig r;
    const u32 head = r.call(dos_lvo::LockDosList, LDF_ALL | LDF_READ);
    CHECK(head != 0);
    CHECK_EQ(r.call(dos_lvo::AttemptLockDosList, LDF_ALL | LDF_READ), head);

    std::vector<std::pair<u32, std::string>> entries;
    u32 cur = head;
    for (u32 i = 0; i < 64; ++i) {
        cur = r.call(dos_lvo::NextDosEntry, cur, LDF_ALL);
        if (cur == 0) {
            break;
        }
        entries.emplace_back(r.get32(cur + dol_Type), r.read_bstr(r.get32(cur + dol_Name)));
    }
    CHECK_EQ(cur, 0u);
    CHECK(entries == (std::vector<std::pair<u32, std::string>>{
                         {DLT_VOLUME, "HombreTest"},
                         {DLT_VOLUME, "Ram Disk"},
                         {DLT_DEVICE, "DF0"},
                         {DLT_DEVICE, "DF1"},
                         {DLT_DEVICE, "DF2"},
                         {DLT_DEVICE, "DF3"},
                         {DLT_DEVICE, "RAM"},
                         {DLT_DIRECTORY, "SYS"},
                         {DLT_DIRECTORY, "C"},
                         {DLT_DIRECTORY, "S"},
                         {DLT_DIRECTORY, "LIBS"},
                         {DLT_DIRECTORY, "DEVS"},
                         {DLT_DIRECTORY, "T"},
                         {DLT_DIRECTORY, "ENV"},
                     }));

    // FindDosEntry is case-insensitive and respects flags.
    r.str(kData, "c");
    const u32 c_node = r.call(dos_lvo::FindDosEntry, head, kData, LDF_ASSIGNS);
    CHECK(c_node != 0);
    CHECK_EQ(r.get32(c_node + dol_Type), DLT_DIRECTORY);
    CHECK(r.read_bstr(r.get32(c_node + dol_Name)) == "C");
    CHECK_EQ(r.call(dos_lvo::FindDosEntry, head, kData, LDF_DEVICES), 0u);

    r.str(kData, "df0");
    const u32 df0 = r.call(dos_lvo::FindDosEntry, head, kData, LDF_DEVICES);
    CHECK(df0 != 0);
    CHECK_EQ(r.get32(df0 + dol_Type), DLT_DEVICE);

    r.str(kData, "hombretest");
    const u32 vol = r.call(dos_lvo::FindDosEntry, head, kData, LDF_VOLUMES);
    CHECK(vol != 0);
    CHECK_EQ(r.get32(vol + dol_Type), DLT_VOLUME);

    r.call(dos_lvo::UnLockDosList, LDF_ALL | LDF_READ);
}

void get_device_proc_walks_assign_list() {
    Rig r;
    r.str(kData, "C:");
    const u32 lock_c = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    r.str(kData + 0x40, "S:");
    const u32 lock_s = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    r.str(kData, "FOO");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, lock_c), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::AssignAdd, kData, lock_s), DOSTRUE);

    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);
    const auto before = r.chunks();
    const u32 buf = kData + 0x200;

    // Walk every directory of FOO: using the previous DevProc.
    r.str(kData + 0x80, "FOO:Missing");
    u32 dp = r.call(dos_lvo::GetDeviceProc, kData + 0x80, 0);
    CHECK(dp != 0);
    CHECK((r.get32(dp + dvp_Flags) & DVPF_ASSIGN) != 0);
    CHECK(r.get32(dp + dvp_Port) != 0);
    CHECK(r.get32(dp + dvp_DevNode) != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, r.get32(dp + dvp_Lock), buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C");

    dp = r.call(dos_lvo::GetDeviceProc, kData + 0x80, dp);
    CHECK(dp != 0);
    CHECK((r.get32(dp + dvp_Flags) & DVPF_ASSIGN) != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, r.get32(dp + dvp_Lock), buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:S");

    CHECK_EQ(r.call(dos_lvo::GetDeviceProc, kData + 0x80, dp), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);

    // FreeDeviceProc releases a DevProc freed early too.
    r.str(kData + 0x80, "DF0:C/Echo");
    const u32 df0_dp = r.call(dos_lvo::GetDeviceProc, kData + 0x80, 0);
    CHECK(df0_dp != 0);
    CHECK_EQ(r.get32(df0_dp + dvp_Flags), 0u);
    r.call(dos_lvo::FreeDeviceProc, df0_dp);
    r.call(dos_lvo::FreeDeviceProc, 0);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);

    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, 0), DOSTRUE);
}

void is_file_system_for_assigns_and_df0() {
    Rig r;
    for (const char* name : {"DF0:", "df0:C/Echo", "DEVS:", "C:", "SYS:", "HombreTest:"}) {
        r.str(kData, name);
        CHECK_EQ(r.call(dos_lvo::IsFileSystem, kData), DOSTRUE);
    }
    for (const char* name : {"FONTS:", "FOO:", "CONSOLE:", "*"}) {
        r.str(kData, name);
        CHECK_EQ(r.call(dos_lvo::IsFileSystem, kData), DOSFALSE);
    }

    r.str(kData, "C:");
    const u32 lock_c = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    r.str(kData, "FOO");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, lock_c), DOSTRUE);
    r.str(kData + 0x40, "FOO:");
    CHECK_EQ(r.call(dos_lvo::IsFileSystem, kData + 0x40), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, 0), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::IsFileSystem, kData + 0x40), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DEVICE_NOT_MOUNTED);
}

void change_dir_and_current_dir_name() {
    Rig r;
    char buf[128];
    CHECK(g_dos.current_dir_name(buf, sizeof buf));
    CHECK(std::string(buf) == "HombreTest:");

    u32 err = 1;
    CHECK(g_dos.change_dir("C", err));
    CHECK_EQ(err, 0u);
    CHECK(g_dos.current_dir_name(buf, sizeof buf));
    CHECK(std::string(buf) == "HombreTest:C");

    // Up from C, then a subdirectory, with case normalisation.
    CHECK(g_dos.change_dir("/devs/sub", err));
    CHECK(g_dos.current_dir_name(buf, sizeof buf));
    CHECK(std::string(buf) == "HombreTest:Devs/Sub");

    // Parent ("/") and volume root (":").
    CHECK(g_dos.change_dir("/", err));
    CHECK(g_dos.current_dir_name(buf, sizeof buf));
    CHECK(std::string(buf) == "HombreTest:Devs");
    CHECK(g_dos.change_dir(":", err));
    CHECK(g_dos.current_dir_name(buf, sizeof buf));
    CHECK(std::string(buf) == "HombreTest:");

    // Assigns work too.
    CHECK(g_dos.change_dir("S:", err));
    CHECK(g_dos.current_dir_name(buf, sizeof buf));
    CHECK(std::string(buf) == "HombreTest:S");

    // Files and missing directories fail without disturbing the current directory.
    CHECK(!g_dos.change_dir("C:Echo", err));
    CHECK_EQ(err, ERROR_OBJECT_WRONG_TYPE);
    CHECK_EQ(g_dos.last_error(), ERROR_OBJECT_WRONG_TYPE);
    CHECK(g_dos.current_dir_name(buf, sizeof buf));
    CHECK(std::string(buf) == "HombreTest:S");

    CHECK(!g_dos.change_dir("NoSuchDir", err));
    CHECK_EQ(err, ERROR_OBJECT_NOT_FOUND);
    CHECK(g_dos.current_dir_name(buf, sizeof buf));
    CHECK(std::string(buf) == "HombreTest:S");

    // "/" at the root fails.
    CHECK(g_dos.change_dir(":", err));
    CHECK(!g_dos.change_dir("/", err));
    CHECK_EQ(err, ERROR_OBJECT_NOT_FOUND);
}

void find_command_uses_path() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/CmdC", hello_command()});
    files.push_back({"S/CmdS", hello_command()});
    files.push_back({"Devs/Sub/CmdD", hello_command()});
    files.push_back({"TopCmd", hello_command()});
    Rig r(files);

    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);
    const u32 cli = r.call(dos_lvo::Cli);
    CHECK_EQ(r.get32(cli + cli_CommandDir), 0u);
    CHECK_EQ(g_dos.path_count(), 0u);

    char out[128];
    CHECK(g_dos.find_command("CmdC", out, sizeof out));
    CHECK(std::string(out) == "C:CmdC");
    CHECK(g_dos.find_command("S:CmdS", out, sizeof out));
    CHECK(std::string(out) == "S:CmdS");
    CHECK(g_dos.find_command("Devs/Sub/CmdD", out, sizeof out));
    CHECK(std::string(out) == "Devs/Sub/CmdD");
    CHECK(!g_dos.find_command("CmdS", out, sizeof out));
    CHECK(!g_dos.find_command("C:", out, sizeof out));

    u32 err = 0;
    CHECK(g_dos.path_add("S:", err));
    CHECK_EQ(err, 0u);
    CHECK(g_dos.path_add("Devs/Sub", err));
    CHECK(g_dos.path_add(":", err));
    CHECK_EQ(g_dos.path_count(), 3u);
    CHECK(g_dos.path_name(0, out, sizeof out));
    CHECK(std::string(out) == "HombreTest:S");
    CHECK(g_dos.path_name(1, out, sizeof out));
    CHECK(std::string(out) == "HombreTest:Devs/Sub");
    CHECK(g_dos.path_name(2, out, sizeof out));
    CHECK(std::string(out) == "HombreTest:");

    // cli_CommandDir mirrors the path as {next BPTR, lock BPTR}.
    const u32 n0 = r.get32(cli + cli_CommandDir) << 2;
    CHECK(n0 != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, r.get32(n0 + 4), kData, 64), DOSTRUE);
    CHECK(r.read_str(kData) == "HombreTest:S");
    const u32 n1 = r.get32(n0) << 2;
    CHECK(n1 != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, r.get32(n1 + 4), kData, 64), DOSTRUE);
    CHECK(r.read_str(kData) == "HombreTest:Devs/Sub");
    const u32 n2 = r.get32(n1) << 2;
    CHECK(n2 != 0);
    CHECK_EQ(r.get32(n2), 0u);

    CHECK(g_dos.find_command("CmdS", out, sizeof out));
    CHECK(std::string(out) == "HombreTest:S/CmdS");
    CHECK(g_dos.find_command("CmdD", out, sizeof out));
    CHECK(std::string(out) == "HombreTest:Devs/Sub/CmdD");
    CHECK(g_dos.find_command("TopCmd", out, sizeof out));
    CHECK(std::string(out) == "TopCmd");
    CHECK(g_dos.change_dir("LIBS", err));
    CHECK(g_dos.find_command("TopCmd", out, sizeof out));
    CHECK(std::string(out) == "HombreTest:TopCmd");
    CHECK(g_dos.change_dir(":", err));

    // A directory already in the path is not added again.
    const u32 count = g_dos.path_count();
    for (u32 i = 0; i < Dos::kMaxPathDirs + 2; ++i) {
        CHECK(g_dos.path_add("C:", err));
    }
    CHECK_EQ(g_dos.path_count(), count + 1);

    g_dos.path_reset();
    CHECK_EQ(g_dos.path_count(), 0u);
    CHECK_EQ(r.get32(cli + cli_CommandDir), 0u);
    CHECK(!g_dos.find_command("CmdS", out, sizeof out));
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
}

struct FakeConsole {
    std::vector<std::string> lines;
    usize line_idx = 0;
    std::vector<bool> ready_seq;
    usize ready_idx = 0;
    std::vector<u32> timeouts;
};

u32 fake_console_read(void* ctx, u8* buf, u32 size) {
    FakeConsole& fc = *static_cast<FakeConsole*>(ctx);
    if (fc.line_idx >= fc.lines.size() || size == 0) {
        return 0;
    }
    const std::string& s = fc.lines[fc.line_idx++];
    const u32 n = static_cast<u32>(std::min<usize>(s.size(), size));
    std::memcpy(buf, s.data(), n);
    return n;
}

bool fake_console_ready(void* ctx, u32 timeout_us) {
    FakeConsole& fc = *static_cast<FakeConsole*>(ctx);
    fc.timeouts.push_back(timeout_us);
    if (fc.ready_idx >= fc.ready_seq.size()) {
        return false;
    }
    return fc.ready_seq[fc.ready_idx++];
}

void console_read_uses_the_reader_after_the_arguments() {
    // 1. FGets(Input(), kData, 32) -> "first\n" (from args)
    // 2. FGets(Input(), kData + 32, 32) -> "alpha\n" (from fake reader)
    // 3. FGetC(Input()) x3 -> 'x', 'y', '\n' (from fake reader line 2)
    // 4. FGets(Input(), kData + 64, 32) -> 0 (EOF from fake reader)
    Asm a;
    a.open_dos();
    a.dos(dos_lvo::Input).from_d0(1).move_l(kData, 2).moveq(32, 3).dos(dos_lvo::FGets);
    a.store_d0(kOut);
    a.dos(dos_lvo::Input).from_d0(1).move_l(kData + 32, 2).moveq(32, 3).dos(dos_lvo::FGets);
    a.store_d0(kOut + 4);
    for (u32 i = 0; i < 3; ++i) {
        a.dos(dos_lvo::Input).from_d0(1).dos(dos_lvo::FGetC);
        a.store_d0(kOut + 8 + i * 4);
    }
    a.dos(dos_lvo::Input).from_d0(1).move_l(kData + 64, 2).moveq(32, 3).dos(dos_lvo::FGets);
    a.store_d0(kOut + 20);
    a.moveq(0, 0).rts();

    Rig r({{"C/ConReader", hunk_file(a.build())}});
    FakeConsole fc;
    fc.lines = {"alpha\n", "xy\n"};
    g_dos.set_console_input(fake_console_read, fake_console_ready, &fc);

    u32 result = 1;
    CHECK(g_dos.run_command("C:ConReader", "first", result, kBlocks));
    CHECK_EQ(result, 0u);
    CHECK_EQ(r.get32(kOut), kData);
    CHECK(r.read_str(kData) == "first\n");
    CHECK_EQ(r.get32(kOut + 4), kData + 32);
    CHECK(r.read_str(kData + 32) == "alpha\n");
    CHECK_EQ(r.get32(kOut + 8), static_cast<u32>('x'));
    CHECK_EQ(r.get32(kOut + 12), static_cast<u32>('y'));
    CHECK_EQ(r.get32(kOut + 16), static_cast<u32>('\n'));
    CHECK_EQ(r.get32(kOut + 20), 0u);

    // read_console_line uses the same reader and strips trailing newline.
    fc.lines = {"yes\n"};
    fc.line_idx = 0;
    char line[32];
    CHECK(g_dos.read_console_line(line, sizeof line));
    CHECK(std::string(line) == "yes");
    CHECK(!g_dos.read_console_line(line, sizeof line));
}

void wait_for_char_uses_ready() {
    Rig r;
    FakeConsole fc;
    fc.ready_seq = {true, false};
    g_dos.set_console_input(fake_console_read, fake_console_ready, &fc);

    const u32 in = r.call(dos_lvo::Input);
    CHECK_EQ(r.call(dos_lvo::WaitForChar, in, 12345), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::WaitForChar, in, 67890), DOSFALSE);
    CHECK_EQ(fc.timeouts.size(), 2u);
    CHECK_EQ(fc.timeouts[0], 12345u);
    CHECK_EQ(fc.timeouts[1], 67890u);
}

void fault_text_api() {
    Rig r;
    char buf[64];
    const u32 len = g_dos.fault(ERROR_OBJECT_NOT_FOUND, buf, sizeof buf);
    CHECK(std::string(buf) == "no such file or directory");
    CHECK_EQ(len, std::string("no such file or directory").size());

    CHECK_EQ(g_dos.fault(0, buf, sizeof buf), 0u);
    CHECK(std::string(buf).empty());

    CHECK_EQ(g_dos.fault(999, buf, sizeof buf), 9u);
    CHECK(std::string(buf) == "Error 999");

    CHECK_EQ(g_dos.fault(ERROR_OBJECT_NOT_FOUND, buf, 8), 7u);
    CHECK(std::string(buf) == "no such");
}

void fail_level_and_return_code() {
    Rig r;
    const u32 cli = r.call(dos_lvo::Cli);
    CHECK_EQ(g_dos.fail_level(), 10u);
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.last_error(), 0u);

    g_dos.set_fail_level(20);
    CHECK_EQ(g_dos.fail_level(), 20u);
    CHECK_EQ(r.get32(cli + cli_FailLevel), 20u);

    g_dos.set_return_code(5);
    CHECK_EQ(g_dos.last_return_code(), 5u);
    CHECK_EQ(r.get32(cli + cli_ReturnCode), 5u);

    g_dos.set_last_error(ERROR_OBJECT_WRONG_TYPE);
    CHECK_EQ(g_dos.last_error(), ERROR_OBJECT_WRONG_TYPE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    CHECK_EQ(r.get32(cli + cli_Result2), ERROR_OBJECT_WRONG_TYPE);
}

void leaked_match_anchor_is_reclaimed_after_command() {
    auto build_leak = [](int mode) {
        Asm a;
        a.open_dos();
        a.address(1, "pat").move_l(kData + 0x200, 2).dos(dos_lvo::MatchFirst).store_d0(kOut);
        if (mode == 0) {
            a.moveq(0, 0).rts();
        } else if (mode == 1) {
            a.moveq(5, 1).dos(dos_lvo::Exit);
        } else if (mode == 2) {
            a.op({0x2038, 0x0001});
        } else {
            a.label("spin").op({0x60FE});
        }
        a.string("pat", "C:#?");
        return hunk_file(a.build());
    };
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/LeakMatch", build_leak(0)});
    files.push_back({"C/LeakExit", build_leak(1)});
    files.push_back({"C/LeakFault", build_leak(2)});
    files.push_back({"C/LeakBudget", build_leak(3)});
    Rig r(files);

    const auto before = r.chunks();
    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);
    const u32 anchor = kData + 0x200;

    for (u32 i = 0; i < 9; ++i) {
        clear_anchor(r, anchor, 64);
        u32 result = 1;
        CHECK(g_dos.run_command("C:LeakMatch", "", result, kBlocks));
        CHECK_EQ(result, 0u);
        CHECK_EQ(r.get32(kOut), 0u);
        CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
        CHECK(r.chunks() == before);
    }

    // Reclaim works on Exit, fault and budget exhaustion too.
    u32 result = 0;
    clear_anchor(r, anchor, 64);
    CHECK(g_dos.run_command("C:LeakExit", "", result, kBlocks));
    CHECK_EQ(result, 5u);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);

    clear_anchor(r, anchor, 64);
    CHECK(!g_dos.run_command("C:LeakFault", "", result, kBlocks));
    CHECK_EQ(r.cpu().exit, ExitReason::AddressError);
    r.cpu().exit = ExitReason::None;
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);

    clear_anchor(r, anchor, 64);
    CHECK(!g_dos.run_command("C:LeakBudget", "", result, 100));
    CHECK_EQ(r.cpu().exit, ExitReason::Budget);
    r.cpu().exit = ExitReason::None;
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);

    clear_anchor(r, anchor, 64);
    r.str(kData, "C:#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, kData, anchor), 0u);
    r.call(dos_lvo::MatchEnd, anchor);
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
    CHECK(r.chunks() == before);
}

void leaked_lock_and_file_are_reclaimed() {
    auto build_leak = [](bool exit_via_dos) {
        Asm a;
        a.open_dos();
        a.address(1, "f").move_l(MODE_OLDFILE, 2).dos(dos_lvo::Open).store_d0(kOut);
        a.address(1, "f").move_l(MODE_OLDFILE, 2).dos(dos_lvo::Open).store_d0(kOut + 4);
        a.address(1, "d1").move_l(SHARED_LOCK, 2).dos(dos_lvo::Lock).store_d0(kOut + 8);
        a.address(1, "d2").move_l(SHARED_LOCK, 2).dos(dos_lvo::Lock).store_d0(kOut + 12);
        a.address(1, "d3").move_l(SHARED_LOCK, 2).dos(dos_lvo::Lock).store_d0(kOut + 16);
        a.address(1, "aname").from_d0(2).dos(dos_lvo::AssignLock);
        if (exit_via_dos) {
            a.moveq(0, 1).dos(dos_lvo::Exit);
        } else {
            a.moveq(0, 0).rts();
        }
        a.string("f", "C:Echo").string("d1", "C:").string("d2", "S:").string("d3", "DEVS:");
        a.string("aname", "CMDASS");
        return hunk_file(a.build());
    };
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/LeakRes", build_leak(false)});
    files.push_back({"C/LeakResExit", build_leak(true)});
    Rig r(files);

    // Hold a shell file handle, a shell lock and an assign before the command.
    r.str(kData, "C:Echo");
    const u32 shell_fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(shell_fh != 0);
    r.str(kData, "C:");
    const u32 shell_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(shell_lock != 0);
    r.str(kData, "S:");
    const u32 ass_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(ass_lock != 0);
    r.str(kData, "MYASS");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, ass_lock), DOSTRUE);

    for (const char* cmd : {"C:LeakRes", "C:LeakResExit"}) {
        u32 result = 1;
        CHECK(g_dos.run_command(cmd, "", result, kBlocks));
        CHECK_EQ(result, 0u);
        const u32 fh1 = r.get32(kOut);
        const u32 fh2 = r.get32(kOut + 4);
        const u32 l1 = r.get32(kOut + 8);
        const u32 l2 = r.get32(kOut + 12);
        CHECK(fh1 != 0 && fh2 != 0 && l1 != 0 && l2 != 0);
        CHECK_EQ(r.call(dos_lvo::Close, fh1), DOSFALSE);
        CHECK_EQ(r.call(dos_lvo::Close, fh2), DOSFALSE);
        CHECK_EQ(r.call(dos_lvo::DupLock, l1), 0u);
        CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);
        CHECK_EQ(r.call(dos_lvo::DupLock, l2), 0u);
        CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);
    }

    // Shell's own handle, shell's lock and both assigns stay valid.
    CHECK_EQ(r.call(dos_lvo::Read, shell_fh, kData + 0x100, 4), 4u);
    const std::vector<u8> echo = echo_bytes();
    CHECK(r.bytes(kData + 0x100, 4) == std::vector<u8>(echo.begin(), echo.begin() + 4));
    const u32 dup = r.call(dos_lvo::DupLock, shell_lock);
    CHECK(dup != 0);
    r.call(dos_lvo::UnLock, dup);
    r.str(kData, "MYASS:Startup-Sequence");
    const u32 my_l = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(my_l != 0);
    r.call(dos_lvo::UnLock, my_l);
    r.str(kData, "CMDASS:Sub/Deep");
    const u32 cmd_l = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(cmd_l != 0);
    r.call(dos_lvo::UnLock, cmd_l);

    // All remaining kMaxFiles - 1 file handle slots are available.
    r.str(kData, "C:Echo");
    std::vector<u32> handles;
    for (u32 i = 1; i < Dos::kMaxFiles; ++i) {
        const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
        CHECK(fh != 0);
        handles.push_back(fh);
    }
    for (u32 fh : handles) {
        r.call(dos_lvo::Close, fh);
    }
    r.call(dos_lvo::Close, shell_fh);
    r.call(dos_lvo::UnLock, shell_lock);
    r.str(kData, "MYASS");
    r.call(dos_lvo::AssignLock, kData, 0);
    r.str(kData, "CMDASS");
    r.call(dos_lvo::AssignLock, kData, 0);
}

void current_dir_restored_after_command() {
    auto build_chdir = [](int mode) {
        Asm a;
        a.open_dos();
        a.address(1, "dir").move_l(SHARED_LOCK, 2).dos(dos_lvo::Lock).store_d0(kOut);
        a.from_d0(1).dos(dos_lvo::CurrentDir).store_d0(kOut + 4);
        if (mode == 0) {
            a.moveq(0, 0).rts();
        } else if (mode == 1) {
            a.moveq(3, 1).dos(dos_lvo::Exit);
        } else {
            a.op({0x2038, 0x0001});
        }
        a.string("dir", ":Devs/Sub");
        return hunk_file(a.build());
    };
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/ChDirRet", build_chdir(0)});
    files.push_back({"C/ChDirExit", build_chdir(1)});
    files.push_back({"C/ChDirFault", build_chdir(2)});
    Rig r(files);

    u32 err = 0;
    CHECK(g_dos.change_dir("S", err));
    const u32 before_dir = r.get32(r.proc() + pr_CurrentDir);
    CHECK(before_dir != 0);

    u32 result = 1;
    CHECK(g_dos.run_command("C:ChDirRet", "", result, kBlocks));
    CHECK_EQ(result, 0u);
    CHECK_EQ(r.get32(kOut + 4), before_dir);
    CHECK_EQ(r.get32(r.proc() + pr_CurrentDir), before_dir);
    CHECK_EQ(r.call(dos_lvo::DupLock, r.get32(kOut)), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);

    CHECK(g_dos.run_command("C:ChDirExit", "", result, kBlocks));
    CHECK_EQ(result, 3u);
    CHECK_EQ(r.get32(r.proc() + pr_CurrentDir), before_dir);

    CHECK(!g_dos.run_command("C:ChDirFault", "", result, kBlocks));
    CHECK_EQ(r.cpu().exit, ExitReason::AddressError);
    r.cpu().exit = ExitReason::None;
    CHECK_EQ(r.get32(r.proc() + pr_CurrentDir), before_dir);

    r.str(kData, "Startup-Sequence");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    r.call(dos_lvo::Close, fh);
}

void many_locks_with_assigns_and_path() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"D1/D2/D3/D4/D5/D6/D7/Leaf", text("ok")});
    Rig r(files);

    u32 err = 0;
    const char* path_dirs[Dos::kMaxPathDirs] = {
        "D1", "D1/D2", "D1/D2/D3", "D1/D2/D3/D4", "D1/D2/D3/D4/D5", "D1/D2/D3/D4/D5/D6",
        "D1/D2/D3/D4/D5/D6/D7", "C:",
    };
    for (u32 i = 0; i < Dos::kMaxPathDirs; ++i) {
        CHECK(g_dos.path_add(path_dirs[i], err));
    }

    for (u32 i = 0; i < 10; ++i) {
        r.str(kData, "D1");
        const u32 lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(lock != 0);
        const std::string name = "ASS" + std::to_string(i);
        r.str(kData + 0x40, name);
        CHECK_EQ(r.call(dos_lvo::AssignLock, kData + 0x40, lock), DOSTRUE);
    }

    CHECK(g_dos.change_dir("D1", err));

    std::vector<u32> extra;
    r.str(kData, "DF0:C");
    for (u32 i = 0; i < 10; ++i) {
        const u32 lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(lock != 0);
        extra.push_back(lock);
    }

    const u32 anchor = kData + 0x200;
    clear_anchor(r, anchor, 128);
    r.str(kData, "DF0:D1");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, kData, anchor), 0u);
    for (u32 lvl = 1; lvl <= 7; ++lvl) {
        r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) | APF_DODIR);
        CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), 0u);
    }
    CHECK(read_fib(r, anchor + ap_Info).name == "Leaf");
    CHECK(r.read_str(anchor + ap_Buf) == "DF0:D1/D2/D3/D4/D5/D6/D7/Leaf");
    r.call(dos_lvo::MatchEnd, anchor);

    for (u32 lock : extra) {
        r.call(dos_lvo::UnLock, lock);
    }
    for (u32 i = 0; i < 10; ++i) {
        r.str(kData + 0x40, "ASS" + std::to_string(i));
        CHECK_EQ(r.call(dos_lvo::AssignLock, kData + 0x40, 0), DOSTRUE);
    }
    g_dos.path_reset();
}

// Adding a directory that is already in the path keeps one entry; removing
// one unlinks it from cli_CommandDir and frees its node and lock.
void path_add_duplicate_and_remove() {
    Rig r;
    const u32 avail_before = exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC);
    u32 err = 0;
    CHECK(g_dos.path_add("S", err));
    CHECK(g_dos.path_add("Devs", err));
    CHECK(g_dos.path_add("s:", err));
    CHECK_EQ(g_dos.path_count(), 2u);
    CHECK(g_dos.path_remove("S:", err));
    CHECK_EQ(g_dos.path_count(), 1u);
    char name[128];
    CHECK(g_dos.path_name(0, name, sizeof name));
    CHECK(std::string(name) == "HombreTest:Devs");
    const u32 cli = r.call(dos_lvo::Cli);
    const u32 first = r.get32(cli + cli_CommandDir) << 2;
    CHECK(first != 0);
    CHECK_EQ(r.get32(first), 0u);
    CHECK(!g_dos.path_remove("C", err));
    CHECK_EQ(err, ERROR_OBJECT_NOT_FOUND);
    g_dos.path_reset();
    CHECK_EQ(exec_call(r, exec_lvo::AvailMem, 0, MEMF_PUBLIC), avail_before);
}

void find_command_order() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/Tool", hello_command()});
    files.push_back({"DirA/Tool", hello_command()});
    files.push_back({"DirB/Tool", hello_command()});
    files.push_back({"C/PathWin", hello_command()});
    files.push_back({"DirB/PathWin", hello_command()});
    files.push_back({"DirA/OrderWin", hello_command()});
    files.push_back({"DirB/OrderWin", hello_command()});
    files.push_back({"C/OnlyC", hello_command()});
    Rig r(files);

    char out[128];
    CHECK(g_dos.find_command("Tool", out, sizeof out));
    CHECK(std::string(out) == "C:Tool");
    CHECK(g_dos.find_command("DirB/Tool", out, sizeof out));
    CHECK(std::string(out) == "DirB/Tool");
    CHECK(g_dos.find_command("C:Tool", out, sizeof out));
    CHECK(std::string(out) == "C:Tool");

    u32 err = 0;
    CHECK(g_dos.path_add("DirB", err));
    CHECK(g_dos.path_add("DirA", err));
    CHECK(g_dos.find_command("PathWin", out, sizeof out));
    CHECK(std::string(out) == "HombreTest:DirB/PathWin");
    CHECK(g_dos.find_command("OrderWin", out, sizeof out));
    CHECK(std::string(out) == "HombreTest:DirB/OrderWin");
    CHECK(g_dos.find_command("OnlyC", out, sizeof out));
    CHECK(std::string(out) == "C:OnlyC");

    // Current directory beats both the path list and C:.
    CHECK(g_dos.change_dir("DirA", err));
    CHECK(g_dos.find_command("Tool", out, sizeof out));
    CHECK(std::string(out) == "Tool");

    g_dos.path_reset();
    CHECK(g_dos.find_command("Tool", out, sizeof out));
    CHECK(std::string(out) == "Tool");

    CHECK(g_dos.change_dir(":", err));
    CHECK(g_dos.find_command("Tool", out, sizeof out));
    CHECK(std::string(out) == "C:Tool");
}

void name_from_lock_uses_disk_case() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/Dir/File", text("hi")});
    Rig r(files);
    const u32 buf = kData + 0x200;
    const u32 fib = kData + 0x300;

    r.str(kData, "c:dir");
    const u32 dir_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(dir_lock != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, dir_lock, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C/Dir");
    CHECK_EQ(r.call(dos_lvo::Examine, dir_lock, fib), DOSTRUE);
    CHECK(read_fib(r, fib).name == "Dir");
    r.call(dos_lvo::UnLock, dir_lock);

    r.str(kData, "df0:c/dir/file");
    const u32 file_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(file_lock != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, file_lock, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C/Dir/File");
    r.call(dos_lvo::UnLock, file_lock);

    r.str(kData, "hombretest:devs/sub/deep");
    const u32 deep_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(deep_lock != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, deep_lock, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:Devs/Sub/Deep");
    r.call(dos_lvo::UnLock, deep_lock);

    r.str(kData, "c:dir/file");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromFH, fh, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C/Dir/File");
    r.call(dos_lvo::Close, fh);

    const u32 port = r.call(dos_lvo::GetFileSysTask);
    const u32 bstr = kData + 0x100;
    put_bstr_at(r, bstr, "c/dir");
    const u32 pkt_lock =
        r.call(dos_lvo::DoPkt, port, ACTION_LOCATE_OBJECT, 0, bstr >> 2, SHARED_LOCK);
    CHECK(pkt_lock != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, pkt_lock, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C/Dir");
    r.call(dos_lvo::UnLock, pkt_lock);

    u32 err = 0;
    CHECK(g_dos.change_dir("c/dir", err));
    char cwd[64];
    CHECK(g_dos.current_dir_name(cwd, sizeof cwd));
    CHECK(std::string(cwd) == "HombreTest:C/Dir");
    CHECK(g_dos.change_dir(":", err));

    const u32 anchor = kData + 0x500;
    clear_anchor(r, anchor, 64);
    r.str(kData, "c:dir/#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, kData, anchor), 0u);
    const u32 cur = r.get32(anchor + ap_Current);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, r.get32(cur + an_Lock), buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C/Dir");
    CHECK(read_fib(r, anchor + ap_Info).name == "File");
    r.call(dos_lvo::MatchEnd, anchor);

    r.str(kData, "LATE");
    r.str(kData + 0x40, "SYS:c/dir");
    CHECK_EQ(r.call(dos_lvo::AssignLate, kData, kData + 0x40), DOSTRUE);
    r.str(kData + 0x80, "LATE:file");
    const u32 late_lock = r.call(dos_lvo::Lock, kData + 0x80, SHARED_LOCK);
    CHECK(late_lock != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, late_lock, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "HombreTest:C/Dir/File");
    r.call(dos_lvo::UnLock, late_lock);
    r.str(kData, "LATE");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, 0), DOSTRUE);
}

void match_first_sets_d0_on_failure() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"D1/D2/D3/D4/D5/D6/D7/D8/Leaf", text("ok")});
    Rig r(files);

    const u32 anchor = kData + 0x200;
    clear_anchor(r, anchor, 128);
    r.str(kData, "D1");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, kData, anchor), 0u);
    for (u32 lvl = 1; lvl <= 7; ++lvl) {
        r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) | APF_DODIR);
        CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), 0u);
    }
    // Exceeding kMaxAnchorDepth fails and sets D0 to ERROR_TOO_MANY_LEVELS.
    r.put8(anchor + ap_Flags, r.get8(anchor + ap_Flags) | APF_DODIR);
    r.cpu().d[0] = 0;
    CHECK_EQ(r.call(dos_lvo::MatchNext, anchor), ERROR_TOO_MANY_LEVELS);
    CHECK_EQ(r.io_err(), ERROR_TOO_MANY_LEVELS);
    r.call(dos_lvo::MatchEnd, anchor);

    struct FailCase {
        const char* pat;
        u16 buf_len;
        u32 expected;
    };
    const FailCase cases[] = {
        {"C:Missing", 64, ERROR_OBJECT_NOT_FOUND},
        {"NoDir/#?", 64, ERROR_DIR_NOT_FOUND},
        {"C:NoSuch#?", 64, ERROR_NO_MORE_ENTRIES},
        {"S:#?", 2, ERROR_BUFFER_OVERFLOW},
        {"(", 64, ERROR_BAD_TEMPLATE},
    };
    for (const FailCase& c : cases) {
        clear_anchor(r, anchor, c.buf_len);
        r.str(kData, c.pat);
        r.cpu().d[0] = 0;
        CHECK_EQ(r.call(dos_lvo::MatchFirst, kData, anchor), c.expected);
        CHECK_EQ(r.io_err(), c.expected);
    }

    // Lock table exhaustion in alloc_achain_node sets APF_NOMEMERR and D0.
    std::vector<u32> locks;
    r.str(kData, "C:");
    for (u32 i = 0; i < Dos::kMaxLocks; ++i) {
        const u32 l = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        if (l == 0) {
            break;
        }
        locks.push_back(l);
    }
    clear_anchor(r, anchor, 64);
    r.str(kData, "C:#?");
    r.cpu().d[0] = 0;
    CHECK_EQ(r.call(dos_lvo::MatchFirst, kData, anchor), ERROR_NO_FREE_STORE);
    CHECK((r.get8(anchor + ap_Flags) & APF_NOMEMERR) != 0);
    CHECK_EQ(r.io_err(), ERROR_NO_FREE_STORE);
    for (u32 l : locks) {
        r.call(dos_lvo::UnLock, l);
    }
}

void ram_volume_read_operations() {
    Rig r;
    RamFs& ram = g_dos.ram();

    u32 dir = RamFs::kNone;
    u32 sub_file = RamFs::kNone;
    u32 top_file = RamFs::kNone;
    CHECK_EQ(ram.create_dir(0, "Dir", dir), 0u);
    CHECK_EQ(ram.create_file(dir, "File", sub_file), 0u);
    CHECK_EQ(ram.create_file(0, "Top", top_file), 0u);

    const std::string payload = "hello from ram disk!";
    CHECK_EQ(ram.write(sub_file, 0, reinterpret_cast<const u8*>(payload.data()),
                       static_cast<u32>(payload.size())),
             0u);
    CHECK_EQ(ram.set_protection(sub_file, 0x03), 0u);
    CHECK_EQ(ram.set_comment(sub_file, "ram note"), 0u);
    CHECK_EQ(ram.set_date(sub_file, 123, 456, 789), 0u);

    const u32 fib = kData + 0x400;
    const u32 buf = kData + 0x600;

    // Lock("RAM:") + Examine -> name "Ram Disk", DirEntryType 1.
    r.str(kData, "RAM:");
    const u32 ram_root = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(ram_root != 0);
    CHECK_EQ(r.call(dos_lvo::Examine, ram_root, fib), DOSTRUE);
    const Fib root_fib = read_fib(r, fib);
    CHECK(root_fib.name == "Ram Disk");
    CHECK_EQ(root_fib.type, static_cast<i32>(ST_ROOT));
    CHECK_EQ(as_signed(r.get32(fib + fib_EntryType)), static_cast<i32>(ST_ROOT));
    CHECK_EQ(r.call(dos_lvo::NameFromLock, ram_root, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "Ram Disk:");

    // ExNext lists RAM root children in order, then ERROR_NO_MORE_ENTRIES.
    std::vector<std::string> root_children;
    while (r.call(dos_lvo::ExNext, ram_root, fib) == DOSTRUE) {
        root_children.push_back(read_fib(r, fib).name);
    }
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK(root_children == (std::vector<std::string>{"T", "Env", "Clipboards", "Dir", "Top"}));

    // ParentDir of RAM root -> 0 with IoErr 0.
    r.set_io_err(ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.call(dos_lvo::ParentDir, ram_root), 0u);
    CHECK_EQ(r.io_err(), 0u);

    // Lock("Ram Disk:Dir/File") case-insensitive + Examine metadata + NameFromLock + ParentDir.
    r.str(kData, "ram disk:dir/file");
    const u32 file_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(file_lock != 0);
    CHECK_EQ(r.call(dos_lvo::Examine, file_lock, fib), DOSTRUE);
    const Fib file_fib = read_fib(r, fib);
    CHECK(file_fib.name == "File");
    CHECK_EQ(file_fib.type, static_cast<i32>(ST_FILE));
    CHECK_EQ(file_fib.size, payload.size());
    CHECK_EQ(file_fib.blocks, 1u);
    CHECK_EQ(r.get32(fib + fib_Protection), 0x03u);
    CHECK(r.read_str(fib + fib_Comment) == "ram note");
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Days), 123u);
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Minute), 456u);
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Tick), 789u);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, file_lock, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "Ram Disk:Dir/File");

    const u32 parent = r.call(dos_lvo::ParentDir, file_lock);
    CHECK(parent != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, parent, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "Ram Disk:Dir");
    CHECK_EQ(r.call(dos_lvo::Examine, parent, fib), DOSTRUE);
    CHECK_EQ(read_fib(r, fib).type, static_cast<i32>(ST_USERDIR));
    CHECK_EQ(r.call(dos_lvo::SameLock, file_lock, parent), LOCK_SAME_VOLUME);

    // Open MODE_OLDFILE + Read + live size after writing more via ram().
    r.str(kData, "RAM:Dir/File");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromFH, fh, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "Ram Disk:Dir/File");
    CHECK_EQ(r.call(dos_lvo::Read, fh, buf, 5), 5u);
    CHECK(std::string(reinterpret_cast<const char*>(r.bytes(buf, 5).data()), 5) == "hello");

    const std::string extra = "+more";
    CHECK_EQ(ram.write(sub_file, static_cast<u32>(payload.size()),
                       reinterpret_cast<const u8*>(extra.data()), static_cast<u32>(extra.size())),
             0u);
    CHECK_EQ(as_signed(r.call(dos_lvo::Seek, fh, 0, OFFSET_END)), 5);
    CHECK_EQ(as_signed(r.call(dos_lvo::Seek, fh, -5, OFFSET_END)),
             static_cast<i32>(payload.size() + extra.size()));
    CHECK_EQ(r.call(dos_lvo::Read, fh, buf, 16), extra.size());
    CHECK(std::string(reinterpret_cast<const char*>(
                          r.bytes(buf, static_cast<u32>(extra.size())).data()),
                      extra.size()) == extra);
    r.call(dos_lvo::Close, fh);

    // CurrentDir on a RAM lock -> relative Lock("x") and Lock(":") -> RAM root.
    const u32 prev_cd = r.call(dos_lvo::CurrentDir, parent);
    CHECK_EQ(prev_cd, 0u);
    r.str(kData, "file");
    const u32 rel = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(rel != 0);
    CHECK_EQ(r.call(dos_lvo::SameLock, rel, file_lock), LOCK_SAME);
    r.call(dos_lvo::UnLock, rel);
    r.str(kData, ":");
    const u32 colon_root = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(colon_root != 0);
    CHECK_EQ(r.call(dos_lvo::SameLock, colon_root, ram_root), LOCK_SAME);
    r.call(dos_lvo::UnLock, colon_root);

    // ACTION_CURRENT_VOLUME reflects RAM when CurrentDir is on RAM.
    const u32 port = r.call(dos_lvo::GetFileSysTask);
    const u32 head = r.call(dos_lvo::LockDosList, LDF_VOLUMES | LDF_READ);
    r.str(kData, "Ram Disk");
    const u32 ram_vol_node = r.call(dos_lvo::FindDosEntry, head, kData, LDF_VOLUMES);
    CHECK(ram_vol_node != 0);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_CURRENT_VOLUME), ram_vol_node >> 2);
    r.call(dos_lvo::UnLockDosList, LDF_VOLUMES | LDF_READ);
    r.call(dos_lvo::CurrentDir, prev_cd);

    // Info on RAM lock -> ID_VALIDATED, 512 bytes/block, Ram Disk volume node.
    const u32 id = kData + 0x800;
    CHECK_EQ(r.call(dos_lvo::Info, ram_root, id), DOSTRUE);
    CHECK_EQ(r.get32(id + id_DiskState), ID_VALIDATED);
    CHECK_EQ(r.get32(id + id_BytesPerBlock), 512u);
    CHECK_EQ(r.get32(id + id_DiskType), ID_DOS_DISK);
    CHECK_EQ(r.get32(id + id_UnitNumber), 0u);
    CHECK(r.get32(id + id_NumBlocks) >= 1u);
    CHECK_EQ(r.get32(id + id_NumBlocksUsed), r.get32(id + id_NumBlocks));
    CHECK_EQ(r.get32(id + id_VolumeNode), ram_vol_node >> 2);

    // Assigns pointing into RAM work.
    r.str(kData, "T");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, parent), DOSTRUE);
    r.str(kData + 0x40, "T:File");
    const u32 t_lock = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK(t_lock != 0);
    CHECK_EQ(r.call(dos_lvo::SameLock, t_lock, file_lock), LOCK_SAME);
    r.call(dos_lvo::UnLock, t_lock);
    r.str(kData, "T");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, 0), DOSTRUE);

    // SameLock RAM vs ADF with equal keys -> LOCK_DIFFERENT.
    CHECK_EQ(r.call(dos_lvo::Examine, 0, fib), DOSTRUE);
    const u32 adf_root_key = r.get32(fib + fib_DiskKey);
    CHECK(adf_root_key < RamFs::kMaxNodes);
    u32 dummy = RamFs::kNone;
    while (ram.used_nodes() < adf_root_key) {
        char name[16];
        std::snprintf(name, sizeof name, "k%u", ram.used_nodes());
        CHECK_EQ(ram.create_file(0, name, dummy), 0u);
    }
    CHECK_EQ(dummy + 1, adf_root_key);
    r.str(kData, std::string("RAM:") + ram.node(dummy).name);
    const u32 same_key_ram = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(same_key_ram != 0);
    CHECK_EQ(r.call(dos_lvo::Examine, same_key_ram, fib), DOSTRUE);
    CHECK_EQ(r.get32(fib + fib_DiskKey), adf_root_key);
    CHECK_EQ(r.call(dos_lvo::SameLock, same_key_ram, 0), LOCK_DIFFERENT);
    r.call(dos_lvo::UnLock, same_key_ram);

    r.call(dos_lvo::UnLock, file_lock);
    r.call(dos_lvo::UnLock, ram_root);
}

void ram_dos_list_match_and_exall() {
    Rig r;
    RamFs& ram = g_dos.ram();
    u32 d1 = RamFs::kNone;
    u32 f1 = RamFs::kNone;
    u32 f2 = RamFs::kNone;
    CHECK_EQ(ram.create_dir(0, "Alpha", d1), 0u);
    CHECK_EQ(ram.create_file(0, "Beta", f1), 0u);
    CHECK_EQ(ram.create_file(d1, "Inner", f2), 0u);
    const u8 bytes[4] = {'t', 'e', 's', 't'};
    CHECK_EQ(ram.write(f1, 0, bytes, sizeof bytes), 0u);
    CHECK_EQ(ram.set_protection(f1, 0x05), 0u);
    CHECK_EQ(ram.set_comment(f1, "beta comment"), 0u);
    CHECK_EQ(ram.set_date(f1, 50, 60, 70), 0u);

    // DosList walk with LDF_VOLUMES finds both volumes.
    const u32 head = r.call(dos_lvo::LockDosList, LDF_VOLUMES | LDF_READ);
    std::vector<std::string> volumes;
    u32 cur = head;
    for (u32 i = 0; i < 16; ++i) {
        cur = r.call(dos_lvo::NextDosEntry, cur, LDF_VOLUMES);
        if (cur == 0) {
            break;
        }
        volumes.push_back(r.read_bstr(r.get32(cur + dol_Name)));
    }
    CHECK(volumes == (std::vector<std::string>{"HombreTest", "Ram Disk"}));
    r.call(dos_lvo::UnLockDosList, LDF_VOLUMES | LDF_READ);

    // MatchFirst("RAM:#?") lists RAM entries.
    const u32 anchor = kData + 0x200;
    clear_anchor(r, anchor, 64);
    r.str(kData, "RAM:#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, kData, anchor), 0u);
    std::vector<std::string> matched;
    matched.push_back(read_fib(r, anchor + ap_Info).name);
    while (r.call(dos_lvo::MatchNext, anchor) == 0u) {
        matched.push_back(read_fib(r, anchor + ap_Info).name);
    }
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK(matched == (std::vector<std::string>{"T", "Env", "Clipboards", "Alpha", "Beta"}));
    r.call(dos_lvo::MatchEnd, anchor);

    // ExAll on RAM root with ED_COMMENT.
    r.str(kData, "RAM:");
    const u32 ram_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(ram_lock != 0);
    const u32 eac = r.call(dos_lvo::AllocDosObject, DOS_EXALLCONTROL, 0);
    CHECK(eac != 0);
    const u32 ex_buf = kData + 0x400;
    CHECK_EQ(r.call(dos_lvo::ExAll, ram_lock, ex_buf, 512, ED_COMMENT, eac), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK_EQ(r.get32(eac + eac_Entries), 5u);

    std::vector<std::string> ex_names;
    u32 alpha_node = 0;
    u32 beta_node = 0;
    for (u32 cur = ex_buf, i = 0; i < 5 && cur != 0; ++i, cur = r.get32(cur + ed_Next)) {
        const std::string name = r.read_str(r.get32(cur + ed_Name));
        ex_names.push_back(name);
        if (name == "Alpha") {
            alpha_node = cur;
        } else if (name == "Beta") {
            beta_node = cur;
        }
    }
    CHECK(ex_names == (std::vector<std::string>{"T", "Env", "Clipboards", "Alpha", "Beta"}));
    CHECK(alpha_node != 0);
    CHECK_EQ(as_signed(r.get32(alpha_node + ed_Type)), static_cast<i32>(ST_USERDIR));
    CHECK(beta_node != 0);
    CHECK_EQ(r.get32(beta_node + ed_Next), 0u);
    CHECK_EQ(as_signed(r.get32(beta_node + ed_Type)), static_cast<i32>(ST_FILE));
    CHECK_EQ(r.get32(beta_node + ed_Size), 4u);
    CHECK_EQ(r.get32(beta_node + ed_Prot), 0x05u);
    CHECK_EQ(r.get32(beta_node + ed_Days), 50u);
    CHECK_EQ(r.get32(beta_node + ed_Mins), 60u);
    CHECK_EQ(r.get32(beta_node + ed_Ticks), 70u);
    CHECK(r.read_str(r.get32(beta_node + ed_Comment)) == "beta comment");

    r.call(dos_lvo::ExAllEnd, ram_lock, ex_buf, 512, ED_COMMENT, eac);
    r.call(dos_lvo::FreeDosObject, DOS_EXALLCONTROL, eac);
    r.call(dos_lvo::UnLock, ram_lock);
}

void ram_deep_paths_stay_valid() {
    Rig r;
    const u32 buf = kData + 0x200;

    // A chain deeper than 64 directories keeps a correct name.
    std::string path = "RAM:a";
    for (u32 i = 0; i < 70; ++i) {
        r.str(kData, path.c_str());
        const u32 lock = r.call(dos_lvo::CreateDir, kData);
        CHECK(lock != 0);
        if (i + 1 == 70) {
            CHECK_EQ(r.call(dos_lvo::NameFromLock, lock, buf, 256), DOSTRUE);
            CHECK(r.read_str(buf) == "Ram Disk:" + path.substr(4));
        }
        r.call(dos_lvo::UnLock, lock);
        path += "/a";
    }

    // A rename that would push a held descendant past the path limit is
    // refused and changes nothing.
    const std::string part(30, 'p');
    std::string deep = "RAM:d";
    r.str(kData, deep.c_str());
    r.call(dos_lvo::UnLock, r.call(dos_lvo::CreateDir, kData));
    for (u32 i = 0; i < 8; ++i) {
        deep += "/" + part;
        r.str(kData, deep.c_str());
        r.call(dos_lvo::UnLock, r.call(dos_lvo::CreateDir, kData));
    }
    r.str(kData, deep.c_str());
    const u32 held = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(held != 0);
    r.str(kData, "RAM:d");
    r.str(kData + 0x100, ("RAM:" + std::string(25, 'n')).c_str());
    CHECK_EQ(r.call(dos_lvo::Rename, kData, kData + 0x100), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_INVALID_COMPONENT_NAME);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, held, buf, 320), DOSTRUE);
    CHECK(r.read_str(buf) == "Ram Disk:" + deep.substr(4));
    const u32 still = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(still != 0);
    r.call(dos_lvo::UnLock, still);
    r.call(dos_lvo::UnLock, held);
}

void ram_protection_bits_are_enforced() {
    Rig r;
    const u32 buf = kData + 0x200;
    r.str(kData, "RAM:locked");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    CHECK(fh != 0);
    r.str(buf, "abc");
    CHECK_EQ(r.call(dos_lvo::Write, fh, buf, 3), 3u);
    CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);

    // Without the write bit: no new file over it, no read-write open, and
    // an open handle can neither write nor change the size.
    const u32 reader = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(reader != 0);
    CHECK_EQ(r.call(dos_lvo::SetProtection, kData, 0x4), DOSTRUE);
    CHECK_EQ(as_signed(r.call(dos_lvo::Write, reader, buf, 3)), -1);
    CHECK_EQ(r.io_err(), ERROR_WRITE_PROTECTED);
    CHECK_EQ(as_signed(r.call(dos_lvo::SetFileSize, reader, 0, OFFSET_BEGINNING)), -1);
    CHECK_EQ(r.io_err(), ERROR_WRITE_PROTECTED);
    CHECK_EQ(r.call(dos_lvo::Close, reader), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_WRITE_PROTECTED);
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_READWRITE), 0u);
    CHECK_EQ(r.io_err(), ERROR_WRITE_PROTECTED);

    // Without the read bit an old file does not open.
    CHECK_EQ(r.call(dos_lvo::SetProtection, kData, 0x8), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_READ_PROTECTED);

    CHECK_EQ(r.call(dos_lvo::SetProtection, kData, 0), DOSTRUE);
    const u32 again = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(again != 0);
    CHECK_EQ(r.call(dos_lvo::Close, again), DOSTRUE);
}

void ram_open_write_read_and_seek() {
    Rig r;
    const u32 buf = kData + 0x200;

    // MODE_NEWFILE + Write + Close, then MODE_OLDFILE + Read.
    r.str(kData, "RAM:hello.txt");
    const u32 fh1 = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    CHECK(fh1 != 0);
    r.str(buf, "hello ram");
    CHECK_EQ(r.call(dos_lvo::Write, fh1, buf, 9), 9u);

    // MODE_NEWFILE while another handle has it open -> ERROR_OBJECT_IN_USE.
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);

    // Read after write on same handle sees the bytes; Seek past end fails.
    CHECK_EQ(r.call(dos_lvo::Seek, fh1, 0, OFFSET_BEGINNING), 9u);
    for (u32 i = 0; i < 16; ++i) {
        r.put8(buf + 0x40 + i, 0);
    }
    CHECK_EQ(r.call(dos_lvo::Read, fh1, buf + 0x40, 16), 9u);
    CHECK(r.read_str(buf + 0x40) == "hello ram");
    CHECK_EQ(as_signed(r.call(dos_lvo::Seek, fh1, 10, OFFSET_BEGINNING)), -1);
    CHECK_EQ(r.io_err(), ERROR_SEEK_ERROR);
    CHECK_EQ(r.call(dos_lvo::Seek, fh1, 0, OFFSET_CURRENT), 9u);
    CHECK_EQ(r.call(dos_lvo::Close, fh1), DOSTRUE);

    // Reopen with MODE_OLDFILE and verify contents.
    const u32 fh2 = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh2 != 0);
    for (u32 i = 0; i < 16; ++i) {
        r.put8(buf + 0x40 + i, 0);
    }
    CHECK_EQ(r.call(dos_lvo::Read, fh2, buf + 0x40, 16), 9u);
    CHECK(r.read_str(buf + 0x40) == "hello ram");
    CHECK_EQ(r.call(dos_lvo::Close, fh2), DOSTRUE);

    // MODE_NEWFILE over an existing file truncates it.
    const u32 fh3 = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    CHECK(fh3 != 0);
    r.str(buf, "xy");
    CHECK_EQ(r.call(dos_lvo::Write, fh3, buf, 2), 2u);
    CHECK_EQ(r.call(dos_lvo::Close, fh3), DOSTRUE);
    const u32 fh4 = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh4 != 0);
    for (u32 i = 0; i < 16; ++i) {
        r.put8(buf + 0x40 + i, 0);
    }
    CHECK_EQ(r.call(dos_lvo::Read, fh4, buf + 0x40, 16), 2u);
    CHECK(r.read_str(buf + 0x40) == "xy");
    CHECK_EQ(r.call(dos_lvo::Close, fh4), DOSTRUE);

    // Missing parent -> ERROR_DIR_NOT_FOUND.
    r.str(kData + 0x80, "RAM:NoSuchDir/file");
    CHECK_EQ(r.call(dos_lvo::Open, kData + 0x80, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_DIR_NOT_FOUND);

    // MODE_READWRITE creates when missing and opens shared when present.
    r.str(kData + 0x80, "RAM:rw.dat");
    const u32 rw1 = r.call(dos_lvo::Open, kData + 0x80, MODE_READWRITE);
    CHECK(rw1 != 0);
    r.str(buf, "abcd");
    CHECK_EQ(r.call(dos_lvo::Write, rw1, buf, 4), 4u);
    const u32 rw2 = r.call(dos_lvo::Open, kData + 0x80, MODE_READWRITE);
    CHECK(rw2 != 0);
    CHECK_EQ(r.call(dos_lvo::Read, rw2, buf + 0x40, 8), 4u);
    CHECK(std::string(reinterpret_cast<const char*>(r.bytes(buf + 0x40, 4).data()), 4) == "abcd");
    CHECK_EQ(r.call(dos_lvo::Close, rw2), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::Close, rw1), DOSTRUE);

    // ADF stays read-only for both write modes and Write.
    r.str(kData + 0x80, "DF0:NewFile");
    CHECK_EQ(r.call(dos_lvo::Open, kData + 0x80, MODE_NEWFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    CHECK_EQ(r.call(dos_lvo::Open, kData + 0x80, MODE_READWRITE), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);

    // Disk full -> ERROR_DISK_FULL, return -1.
    r.str(kData + 0x80, "RAM:big");
    const u32 big = r.call(dos_lvo::Open, kData + 0x80, MODE_NEWFILE);
    CHECK(big != 0);
    CHECK_EQ(as_signed(r.call(dos_lvo::Seek, big, 0, OFFSET_BEGINNING)), 0);
    // Write 1 byte first, then SetFileSize beyond guest memory capacity.
    CHECK_EQ(as_signed(r.call(dos_lvo::SetFileSize, big, 0x01000000u, OFFSET_BEGINNING)), -1);
    CHECK_EQ(r.io_err(), ERROR_DISK_FULL);
    CHECK_EQ(as_signed(r.call(dos_lvo::Write, big, kData, 0x00800000u)), -1);
    CHECK_EQ(r.io_err(), ERROR_DISK_FULL);
    CHECK_EQ(r.call(dos_lvo::Close, big), DOSTRUE);
}

void ram_buffered_output_and_set_file_size() {
    Rig r;
    r.str(kData, "RAM:stream.txt");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    CHECK(fh != 0);

    // FPuts, FPutC, FWrite, VFPrintf, VFWritef, PutStr, WriteChars.
    r.str(kData + 0x100, "line1\n");
    CHECK_EQ(r.call(dos_lvo::FPuts, fh, kData + 0x100), 0u);
    CHECK_EQ(r.call(dos_lvo::FPutC, fh, '>'), static_cast<u32>('>'));
    r.str(kData + 0x120, "AB");
    CHECK_EQ(r.call(dos_lvo::FWrite, fh, kData + 0x120, 1, 2), 2u);

    r.str(kData + 0x140, ":%ld:");
    r.put32(kData + 0x160, 42);
    CHECK_EQ(r.call(dos_lvo::VFPrintf, fh, kData + 0x140, kData + 0x160), 4u);

    r.str(kData + 0x180, "%S");
    r.str(kData + 0x190, "end");
    r.put32(kData + 0x1A0, kData + 0x190);
    CHECK_EQ(r.call(dos_lvo::VFWritef, fh, kData + 0x180, kData + 0x1A0), 3u);

    const u32 old_out = r.call(dos_lvo::SelectOutput, fh);
    r.str(kData + 0x1C0, "!");
    CHECK_EQ(r.call(dos_lvo::PutStr, kData + 0x1C0), 0u);
    r.str(kData + 0x1D0, "ok");
    CHECK_EQ(r.call(dos_lvo::WriteChars, kData + 0x1D0, 2), 2u);
    r.call(dos_lvo::SelectOutput, old_out);

    CHECK_EQ(r.call(dos_lvo::Flush, fh), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);

    const u32 rd = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(rd != 0);
    const u32 out_buf = kData + 0x300;
    for (u32 i = 0; i < 64; ++i) {
        r.put8(out_buf + i, 0);
    }
    const u32 got = r.call(dos_lvo::Read, rd, out_buf, 64);
    CHECK_EQ(got, 19u);
    CHECK(r.read_str(out_buf) == "line1\n>AB:42:end!ok");
    CHECK_EQ(r.call(dos_lvo::Close, rd), DOSTRUE);

    // SetFileSize grow and shrink, clamping pos.
    const u32 sz_fh = r.call(dos_lvo::Open, kData, MODE_READWRITE);
    CHECK(sz_fh != 0);
    CHECK_EQ(r.call(dos_lvo::SetFileSize, sz_fh, 24, OFFSET_BEGINNING), 24u);
    CHECK_EQ(r.call(dos_lvo::Seek, sz_fh, 22, OFFSET_BEGINNING), 0u);
    CHECK_EQ(r.call(dos_lvo::SetFileSize, sz_fh, 5, OFFSET_BEGINNING), 5u);
    // Position was clamped to 5.
    CHECK_EQ(r.call(dos_lvo::Seek, sz_fh, 0, OFFSET_CURRENT), 5u);
    CHECK_EQ(r.call(dos_lvo::SetFileSize, sz_fh, 2, OFFSET_CURRENT), 7u);
    CHECK_EQ(r.call(dos_lvo::SetFileSize, sz_fh, static_cast<u32>(-4), OFFSET_END), 3u);
    CHECK_EQ(r.call(dos_lvo::Seek, sz_fh, 0, OFFSET_CURRENT), 3u);
    // Negative target fails with ERROR_SEEK_ERROR.
    CHECK_EQ(as_signed(r.call(dos_lvo::SetFileSize, sz_fh, static_cast<u32>(-10), OFFSET_END)), -1);
    CHECK_EQ(r.io_err(), ERROR_SEEK_ERROR);
    CHECK_EQ(r.call(dos_lvo::Seek, sz_fh, 0, OFFSET_END), 3u);
    CHECK_EQ(r.call(dos_lvo::Close, sz_fh), DOSTRUE);

    // SetFileSize on ADF fails with ERROR_DISK_WRITE_PROTECTED.
    r.str(kData + 0x40, "C:Echo");
    const u32 adf_fh = r.call(dos_lvo::Open, kData + 0x40, MODE_OLDFILE);
    CHECK(adf_fh != 0);
    CHECK_EQ(as_signed(r.call(dos_lvo::SetFileSize, adf_fh, 10, OFFSET_BEGINNING)), -1);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    CHECK_EQ(r.call(dos_lvo::Close, adf_fh), DOSTRUE);
}

void ram_delete_createdir_rename_and_metadata() {
    Rig r;
    const u32 fib = kData + 0x400;
    const u32 buf = kData + 0x600;

    // CreateDir returns an EXCLUSIVE_LOCK; duplicate fails; missing parent fails; ADF fails.
    r.str(kData, "RAM:DirA");
    const u32 d_lock = r.call(dos_lvo::CreateDir, kData);
    CHECK(d_lock != 0);
    CHECK_EQ(r.get32((d_lock << 2) + fl_Access), EXCLUSIVE_LOCK);
    // Duplicate CreateDir fails (even after unlock, and while locked).
    CHECK_EQ(r.call(dos_lvo::CreateDir, kData), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_EXISTS);
    // Exclusive lock blocks shared Lock on DirA until unlocked.
    CHECK_EQ(r.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    r.call(dos_lvo::UnLock, d_lock);

    CHECK_EQ(r.call(dos_lvo::CreateDir, kData), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_EXISTS);
    r.str(kData + 0x40, "RAM:MissingParent/Sub");
    CHECK_EQ(r.call(dos_lvo::CreateDir, kData + 0x40), 0u);
    CHECK_EQ(r.io_err(), ERROR_DIR_NOT_FOUND);
    r.str(kData + 0x40, "DF0:NewDir");
    CHECK_EQ(r.call(dos_lvo::CreateDir, kData + 0x40), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);

    // Create file inside DirA.
    r.str(kData + 0x40, "RAM:DirA/Item");
    const u32 fh = r.call(dos_lvo::Open, kData + 0x40, MODE_NEWFILE);
    CHECK(fh != 0);
    r.str(buf, "data");
    CHECK_EQ(r.call(dos_lvo::Write, fh, buf, 4), 4u);
    CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);

    // DeleteFile: non-empty dir fails, open/locked file fails, root fails, ADF fails.
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DIRECTORY_NOT_EMPTY);
    r.str(kData + 0x80, "RAM:");
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x80), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    r.str(kData + 0x80, "C:Echo");
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x80), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);

    const u32 item_lock = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK(item_lock != 0);
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x40), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    r.call(dos_lvo::UnLock, item_lock);

    // SetProtection, SetComment, SetFileDate visible via Examine.
    CHECK_EQ(r.call(dos_lvo::SetProtection, kData + 0x40, 0x0F), DOSTRUE);
    r.str(kData + 0x80, "hello comment");
    CHECK_EQ(r.call(dos_lvo::SetComment, kData + 0x40, kData + 0x80), DOSTRUE);
    const u32 ds = kData + 0x100;
    r.put32(ds + ds_Days, 111);
    r.put32(ds + ds_Minute, 222);
    r.put32(ds + ds_Tick, 333);
    CHECK_EQ(r.call(dos_lvo::SetFileDate, kData + 0x40, ds), DOSTRUE);

    const u32 check_lock = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK(check_lock != 0);
    CHECK_EQ(r.call(dos_lvo::Examine, check_lock, fib), DOSTRUE);
    CHECK_EQ(r.get32(fib + fib_Protection), 0x0Fu);
    CHECK(r.read_str(fib + fib_Comment) == "hello comment");
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Days), 111u);
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Minute), 222u);
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Tick), 333u);
    r.call(dos_lvo::UnLock, check_lock);

    // Delete-protected file refuses delete; clearing protection allows delete.
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x40), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DELETE_PROTECTED);
    CHECK_EQ(r.call(dos_lvo::SetProtection, kData + 0x40, 0), DOSTRUE);

    // Rename within RAM, into a subdir, across volumes, onto existing, and locked parent.
    r.str(kData + 0x80, "RAM:DirB");
    const u32 dir_b = r.call(dos_lvo::CreateDir, kData + 0x80);
    CHECK(dir_b != 0);
    r.call(dos_lvo::UnLock, dir_b);

    // Lock DirA/Item, then rename DirA -> DirX and verify NameFromLock updates!
    const u32 locked_child = r.call(dos_lvo::Lock, kData + 0x40, SHARED_LOCK);
    CHECK(locked_child != 0);
    r.str(kData + 0xC0, "RAM:DirX");
    CHECK_EQ(r.call(dos_lvo::Rename, kData, kData + 0xC0), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, locked_child, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "Ram Disk:DirX/Item");

    // Rename into DirB while child is locked.
    r.str(kData + 0x100, "RAM:DirX/Item");
    r.str(kData + 0x140, "RAM:DirB/Moved");
    CHECK_EQ(r.call(dos_lvo::Rename, kData + 0x100, kData + 0x140), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, locked_child, buf, 64), DOSTRUE);
    CHECK(r.read_str(buf) == "Ram Disk:DirB/Moved");
    r.call(dos_lvo::UnLock, locked_child);

    // Rename across volumes, onto existing, on ADF.
    r.str(kData + 0x180, "C:Echo");
    CHECK_EQ(r.call(dos_lvo::Rename, kData + 0x140, kData + 0x180), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_RENAME_ACROSS_DEVICES);
    CHECK_EQ(r.call(dos_lvo::Rename, kData + 0x140, kData + 0xC0), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_EXISTS);
    r.str(kData + 0x1C0, "DF0:Other");
    CHECK_EQ(r.call(dos_lvo::Rename, kData + 0x180, kData + 0x1C0), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);

    // Delete moved file and empty dirs.
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x140), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x80), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0xC0), DOSTRUE);
}

void ram_exclusive_locks_and_dopkt() {
    Rig r;
    const u32 buf = kData + 0x200;
    const u32 fib = kData + 0x400;

    r.str(kData, "RAM:target");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    CHECK(fh != 0);
    // MODE_NEWFILE handle is exclusive: blocks shared and exclusive locks and opens.
    CHECK_EQ(r.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    CHECK_EQ(r.call(dos_lvo::DupLockFromFH, fh), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    r.call(dos_lvo::Close, fh);

    // Shared lock blocks EXCLUSIVE_LOCK; EXCLUSIVE_LOCK blocks shared lock & Open.
    const u32 sh = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(sh != 0);
    CHECK_EQ(r.call(dos_lvo::Lock, kData, EXCLUSIVE_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    r.call(dos_lvo::UnLock, sh);

    const u32 ex = r.call(dos_lvo::Lock, kData, EXCLUSIVE_LOCK);
    CHECK(ex != 0);
    CHECK_EQ(r.call(dos_lvo::Lock, kData, SHARED_LOCK), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    CHECK_EQ(r.call(dos_lvo::DupLock, ex), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    r.call(dos_lvo::UnLock, ex);
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData), DOSTRUE);

    // DoPkt: ACTION_CREATE_DIR, ACTION_SET_PROTECT, ACTION_SET_COMMENT,
    // ACTION_SET_DATE, ACTION_RENAME_OBJECT, ACTION_DELETE_OBJECT.
    const u32 port = r.call(dos_lvo::GetFileSysTask);
    r.str(kData, "RAM:");
    const u32 ram_root = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(ram_root != 0);

    const u32 b1 = kData + 0x80;
    const u32 b2 = kData + 0x100;
    const u32 b3 = kData + 0x180;
    put_bstr_at(r, b1, "PktDir");
    const u32 pkt_dir = r.call(dos_lvo::DoPkt, port, ACTION_CREATE_DIR, ram_root, b1 >> 2);
    CHECK(pkt_dir != 0);
    CHECK_EQ(r.get32((pkt_dir << 2) + fl_Access), EXCLUSIVE_LOCK);
    r.call(dos_lvo::UnLock, pkt_dir);

    const u32 pkt_dir_sh =
        r.call(dos_lvo::DoPkt, port, ACTION_LOCATE_OBJECT, ram_root, b1 >> 2, SHARED_LOCK);
    CHECK(pkt_dir_sh != 0);

    r.str(kData + 0x20, "RAM:PktDir/Leaf");
    const u32 leaf_fh = r.call(dos_lvo::Open, kData + 0x20, MODE_NEWFILE);
    CHECK(leaf_fh != 0);
    r.call(dos_lvo::Close, leaf_fh);

    put_bstr_at(r, b2, "Leaf");
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_SET_PROTECT, 0, pkt_dir_sh, b2 >> 2, 0x0A),
             DOSTRUE);
    put_bstr_at(r, b3, "pkt note");
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_SET_COMMENT, 0, pkt_dir_sh, b2 >> 2, b3 >> 2),
             DOSTRUE);
    const u32 ds = kData + 0x300;
    r.put32(ds + ds_Days, 7);
    r.put32(ds + ds_Minute, 8);
    r.put32(ds + ds_Tick, 9);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_SET_DATE, 0, pkt_dir_sh, b2 >> 2, ds), DOSTRUE);

    const u32 leaf_lock =
        r.call(dos_lvo::DoPkt, port, ACTION_LOCATE_OBJECT, pkt_dir_sh, b2 >> 2, SHARED_LOCK);
    CHECK(leaf_lock != 0);
    CHECK_EQ(r.call(dos_lvo::Examine, leaf_lock, fib), DOSTRUE);
    CHECK_EQ(r.get32(fib + fib_Protection), 0x0Au);
    CHECK(r.read_str(fib + fib_Comment) == "pkt note");
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Days), 7u);
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Minute), 8u);
    CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Tick), 9u);
    r.call(dos_lvo::UnLock, leaf_lock);

    put_bstr_at(r, b3, "RenamedLeaf");
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_RENAME_OBJECT, pkt_dir_sh, b2 >> 2, ram_root,
                    b3 >> 2),
             DOSTRUE);
    r.str(buf, "RAM:RenamedLeaf");
    const u32 moved = r.call(dos_lvo::Lock, buf, SHARED_LOCK);
    CHECK(moved != 0);
    r.call(dos_lvo::UnLock, moved);

    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_DELETE_OBJECT, ram_root, b3 >> 2), DOSTRUE);
    r.call(dos_lvo::UnLock, pkt_dir_sh);
    CHECK_EQ(r.call(dos_lvo::DoPkt, port, ACTION_DELETE_OBJECT, ram_root, b1 >> 2), DOSTRUE);
    r.call(dos_lvo::UnLock, ram_root);
}

void ram_enumeration_after_delete_info_and_cleanup() {
    auto leak_ram_cmd = []() {
        Asm a;
        a.open_dos()
            .address(1, "name")
            .move_l(MODE_NEWFILE, 2)
            .dos(dos_lvo::Open)
            .store_d0(kOut)
            .from_d0(1)
            .address(2, "data")
            .moveq(3, 3)
            .dos(dos_lvo::Write)
            .moveq(0, 0)
            .rts()
            .string("name", "RAM:leaked")
            .string("data", "abc");
        return hunk_file(a.build());
    };
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/LeakRam", leak_ram_cmd()});
    Rig r(files);

    // Info NumBlocksUsed grows after a write.
    r.str(kData, "RAM:");
    const u32 ram_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(ram_lock != 0);
    const u32 id = kData + 0x200;
    CHECK_EQ(r.call(dos_lvo::Info, ram_lock, id), DOSTRUE);
    CHECK_EQ(r.get32(id + id_DiskState), ID_VALIDATED);
    const u32 initial_used = r.get32(id + id_NumBlocksUsed);

    for (const char* name : {"RAM:A", "RAM:B", "RAM:C"}) {
        r.str(kData + 0x40, name);
        const u32 fh = r.call(dos_lvo::Open, kData + 0x40, MODE_NEWFILE);
        CHECK(fh != 0);
        r.str(kData + 0x80, std::string(1024, 'x'));
        CHECK_EQ(r.call(dos_lvo::Write, fh, kData + 0x80, 1024), 1024u);
        r.call(dos_lvo::Close, fh);
    }
    CHECK_EQ(r.call(dos_lvo::Info, ram_lock, id), DOSTRUE);
    CHECK_EQ(r.get32(id + id_DiskState), ID_VALIDATED);
    CHECK(r.get32(id + id_NumBlocksUsed) > initial_used);

    // ExNext continues after deleting the current entry.
    const u32 fib = kData + 0x500;
    CHECK_EQ(r.call(dos_lvo::Examine, ram_lock, fib), DOSTRUE);
    std::vector<std::string> ex_names;
    while (r.call(dos_lvo::ExNext, ram_lock, fib) == DOSTRUE) {
        const std::string cur = read_fib(r, fib).name;
        ex_names.push_back(cur);
        if (cur == "A") {
            r.str(kData + 0x40, "RAM:A");
            CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x40), DOSTRUE);
        }
    }
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK(ex_names == (std::vector<std::string>{"T", "Env", "Clipboards", "A", "B", "C"}));

    // ExAll continues after deleting the last returned entry.
    r.str(kData + 0x40, "RAM:A");
    const u32 recreate_a = r.call(dos_lvo::Open, kData + 0x40, MODE_NEWFILE);
    CHECK(recreate_a != 0);
    r.call(dos_lvo::Close, recreate_a);
    const u32 eac = r.call(dos_lvo::AllocDosObject, DOS_EXALLCONTROL, 0);
    CHECK(eac != 0);
    const u32 ex_buf = kData + 0x700;
    std::vector<std::string> exall_names;
    for (;;) {
        const u32 more = r.call(dos_lvo::ExAll, ram_lock, ex_buf, 20, ED_NAME, eac);
        const u32 count = r.get32(eac + eac_Entries);
        for (u32 cur = ex_buf, i = 0; i < count && cur != 0; ++i, cur = r.get32(cur + ed_Next)) {
            exall_names.push_back(r.read_str(r.get32(cur + ed_Name)));
        }
        if (more == DOSFALSE) {
            CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
            break;
        }
        if (!exall_names.empty() && exall_names.back() == "B") {
            r.str(kData + 0x40, "RAM:B");
            CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x40), DOSTRUE);
        }
    }
    CHECK(exall_names == (std::vector<std::string>{"T", "Env", "Clipboards", "A", "B", "C"}));
    r.call(dos_lvo::ExAllEnd, ram_lock, ex_buf, 20, ED_NAME, eac);
    r.call(dos_lvo::FreeDosObject, DOS_EXALLCONTROL, eac);

    // MatchNext continues after deleting the current entry.
    const u32 anchor = kData + 0x800;
    clear_anchor(r, anchor, 64);
    r.str(kData + 0x40, "RAM:#?");
    CHECK_EQ(r.call(dos_lvo::MatchFirst, kData + 0x40, anchor), 0u);
    std::vector<std::string> match_names;
    match_names.push_back(read_fib(r, anchor + ap_Info).name);
    while (r.call(dos_lvo::MatchNext, anchor) == 0u) {
        const std::string cur = read_fib(r, anchor + ap_Info).name;
        match_names.push_back(cur);
        if (cur == "A") {
            r.str(kData + 0x40, "RAM:A");
            CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x40), DOSTRUE);
        }
    }
    CHECK_EQ(r.io_err(), ERROR_NO_MORE_ENTRIES);
    CHECK(match_names == (std::vector<std::string>{"T", "Env", "Clipboards", "A", "C"}));
    r.call(dos_lvo::MatchEnd, anchor);
    r.call(dos_lvo::UnLock, ram_lock);

    // cleanup_command releases a command's leftover RAM handle so the file can be deleted.
    u32 result = 99;
    CHECK(g_dos.run_command("C:LeakRam", "", result, kBlocks));
    CHECK_EQ(result, 0u);
    r.str(kData + 0x40, "RAM:leaked");
    const u32 leaked_rd = r.call(dos_lvo::Open, kData + 0x40, MODE_OLDFILE);
    CHECK(leaked_rd != 0);
    for (u32 i = 0; i < 8; ++i) {
        r.put8(kData + 0x80 + i, 0);
    }
    CHECK_EQ(r.call(dos_lvo::Read, leaked_rd, kData + 0x80, 8), 3u);
    CHECK(r.read_str(kData + 0x80) == "abc");
    CHECK_EQ(r.call(dos_lvo::Close, leaked_rd), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::DeleteFile, kData + 0x40), DOSTRUE);
}

void t_and_env_assigns_and_envarc_copy() {
    // Default assigns T: and ENV: and RAM:Clipboards exist on Ram Disk.
    {
        Rig r;
        const u32 fib = kData + 0x200;
        const u32 buf = kData + 0x400;

        struct RamDir {
            const char* lock_path;
            const char* name;
            const char* full_path;
        };
        const RamDir dirs[] = {
            {"T:", "T", "Ram Disk:T"},
            {"ENV:", "Env", "Ram Disk:Env"},
            {"RAM:Clipboards", "Clipboards", "Ram Disk:Clipboards"},
        };
        for (const RamDir& d : dirs) {
            r.str(kData, d.lock_path);
            const u32 lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
            CHECK(lock != 0);
            CHECK_EQ(r.call(dos_lvo::Examine, lock, fib), DOSTRUE);
            const Fib f = read_fib(r, fib);
            CHECK(f.name == d.name);
            CHECK_EQ(f.type, static_cast<i32>(ST_USERDIR));
            CHECK_EQ(r.call(dos_lvo::NameFromLock, lock, buf, 64), DOSTRUE);
            CHECK(r.read_str(buf) == d.full_path);
            r.call(dos_lvo::UnLock, lock);
        }

        // Assign locks keep T: and ENV: from being deleted.
        for (const char* busy : {"RAM:T", "RAM:Env"}) {
            r.str(kData, busy);
            CHECK_EQ(r.call(dos_lvo::DeleteFile, kData), DOSFALSE);
            CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
        }
    }

    // ENVARC: tree (contents, protection, date, comment, subdirs) copies into ENV:.
    {
        std::vector<AdfFile> files = standard_files();
        files.push_back({"Prefs/Env-Archive/greeting", text("hello from envarc")});
        files.push_back({"Prefs/Env-Archive/Empty", {}});
        files.push_back({"Prefs/Env-Archive/Sys/def_disk", {'a', 'b', '\n', 'c'}});
        files.push_back({"Prefs/Env-Archive/Sys/Sub/deep", text("nested")});
        std::vector<u8> image = build_adf("HombreTest", false, files);

        AdfVolume vol{};
        CHECK(adf_mount(image.data(), image.size(), vol) == FsError::Ok);
        DirEntry greeting{};
        CHECK(adf_find(vol, "Prefs/Env-Archive/greeting", greeting) == FsError::Ok);
        const auto put_be32_at = [&](u32 block, u32 long_idx, u32 value) {
            const usize off = static_cast<usize>(block) * 512 + long_idx * 4;
            image[off + 0] = static_cast<u8>(value >> 24);
            image[off + 1] = static_cast<u8>(value >> 16);
            image[off + 2] = static_cast<u8>(value >> 8);
            image[off + 3] = static_cast<u8>(value);
        };
        put_be32_at(greeting.header_block, 80, 0x03);   // protection
        put_be32_at(greeting.header_block, 105, 42);    // days
        put_be32_at(greeting.header_block, 106, 100);   // minute
        put_be32_at(greeting.header_block, 107, 200);   // tick
        const std::string note = "env note";
        const usize c_off = static_cast<usize>(greeting.header_block) * 512 + 82 * 4;
        image[c_off] = static_cast<u8>(note.size());
        for (usize i = 0; i < note.size(); ++i) {
            image[c_off + 1 + i] = static_cast<u8>(note[i]);
        }
        adf_fix_checksum(image, greeting.header_block);

        Rig r(std::move(image));
        CHECK(g_lines.empty());

        const u32 buf = kData + 0x200;
        const u32 fib = kData + 0x400;

        r.str(kData, "ENV:greeting");
        const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
        CHECK(fh != 0);
        CHECK_EQ(r.call(dos_lvo::Read, fh, buf, 32), 17u);
        CHECK(r.bytes(buf, 17) == text("hello from envarc"));
        CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);

        const u32 g_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(g_lock != 0);
        CHECK_EQ(r.call(dos_lvo::Examine, g_lock, fib), DOSTRUE);
        CHECK_EQ(r.get32(fib + fib_Protection), 0x03u);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Days), 42u);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Minute), 100u);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Tick), 200u);
        CHECK(r.read_str(fib + fib_Comment) == "env note");
        r.call(dos_lvo::UnLock, g_lock);

        r.str(kData, "ENV:Empty");
        const u32 empty_fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
        CHECK(empty_fh != 0);
        CHECK_EQ(r.call(dos_lvo::Read, empty_fh, buf, 16), 0u);
        CHECK_EQ(r.call(dos_lvo::Close, empty_fh), DOSTRUE);

        CHECK_EQ(get_var(r, "Sys/def_disk", 32, GVF_GLOBAL_ONLY | GVF_BINARY_VAR), 4);
        CHECK(r.bytes(kData + 0x100, 4) == (std::vector<u8>{'a', 'b', '\n', 'c'}));

        r.str(kData, "ENV:Sys/Sub/deep");
        const u32 deep_fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
        CHECK(deep_fh != 0);
        CHECK_EQ(r.call(dos_lvo::Read, deep_fh, buf, 16), 6u);
        CHECK(r.bytes(buf, 6) == text("nested"));
        CHECK_EQ(r.call(dos_lvo::Close, deep_fh), DOSTRUE);
    }

    // A copy failure from ENVARC: (e.g. disk full) is logged once and non-fatal.
    {
        std::vector<AdfFile> files = standard_files();
        files.push_back({"Prefs/Env-Archive/huge1", std::vector<u8>(32768, 'A')});
        files.push_back({"Prefs/Env-Archive/huge2", std::vector<u8>(32768, 'B')});
        Rig r(build_adf("HombreTest", false, files), kFreeStart + 65536);
        CHECK_EQ(g_lines.size(), 1u);
        CHECK(logged_containing("ENVARC:"));
        r.str(kData, "ENV:");
        const u32 env_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(env_lock != 0);
        r.call(dos_lvo::UnLock, env_lock);
    }
}

void global_variables() {
    Rig r;
    const u32 buf = kData + 0x400;

    // SetVar GVF_GLOBAL_ONLY creates ENV:<name> readable via Open.
    CHECK_EQ(set_var(r, "Foo", "hello world", GVF_GLOBAL_ONLY), DOSTRUE);
    r.str(kData, "ENV:Foo");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    CHECK_EQ(r.call(dos_lvo::Read, fh, buf, 32), 11u);
    CHECK(r.bytes(buf, 11) == text("hello world"));
    CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);

    // GVF_SAVE_VAR ignores read-only ENVARC: and succeeds if ENV: worked.
    CHECK_EQ(set_var(r, "Foo", "next", GVF_GLOBAL_ONLY | GVF_SAVE_VAR), DOSTRUE);
    CHECK_EQ(get_var(r, "Foo", 32, GVF_GLOBAL_ONLY), 4);
    CHECK(r.read_str(kData + 0x100) == "next");

    // GetVar GVF_GLOBAL_ONLY round trip, newline truncation, BINARY and DONT_NULL_TERM.
    CHECK_EQ(set_var(r, "Lines", "first\nsecond", GVF_GLOBAL_ONLY, 12), DOSTRUE);
    CHECK_EQ(get_var(r, "lines", 32, GVF_GLOBAL_ONLY), 5);
    CHECK(r.read_str(kData + 0x100) == "first");
    CHECK_EQ(r.io_err(), 12u);

    CHECK_EQ(get_var(r, "Lines", 32, GVF_GLOBAL_ONLY | GVF_BINARY_VAR), 12);
    CHECK(r.bytes(kData + 0x100, 12) == text("first\nsecond"));
    CHECK_EQ(r.io_err(), 12u);

    CHECK_EQ(get_var(r, "Lines", 4, GVF_GLOBAL_ONLY), 3);
    CHECK(r.read_str(kData + 0x100) == "fir");
    CHECK_EQ(r.get8(kData + 0x104), 0xEEu);
    CHECK_EQ(r.io_err(), 12u);

    CHECK_EQ(get_var(r, "Lines", 4, GVF_GLOBAL_ONLY | GVF_DONT_NULL_TERM), 4);
    CHECK(r.bytes(kData + 0x100, 5) == (std::vector<u8>{'f', 'i', 'r', 's', 0xEE}));
    CHECK_EQ(r.io_err(), 12u);

    // Fallback when no local, GVF_LOCAL_ONLY stays local, FindVar stays local-only.
    CHECK_EQ(get_var(r, "Lines", 32, 0), 5);
    CHECK(r.read_str(kData + 0x100) == "first");
    CHECK_EQ(get_var(r, "Lines", 32, GVF_LOCAL_ONLY), -1);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    r.str(kData, "Lines");
    CHECK_EQ(r.call(dos_lvo::FindVar, kData, LV_VAR), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);

    // Local shadows global; GVF_GLOBAL_ONLY ignores local.
    CHECK_EQ(set_var(r, "Lines", "localval", 0), DOSTRUE);
    CHECK_EQ(get_var(r, "Lines", 32, 0), 8);
    CHECK(r.read_str(kData + 0x100) == "localval");
    CHECK_EQ(get_var(r, "Lines", 32, GVF_GLOBAL_ONLY), 5);
    CHECK(r.read_str(kData + 0x100) == "first");

    // DeleteVar without flags deletes local first, then global on the next call.
    r.str(kData, "Lines");
    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, 0), DOSTRUE);
    CHECK_EQ(get_var(r, "Lines", 32, GVF_LOCAL_ONLY), -1);
    CHECK_EQ(get_var(r, "Lines", 32, GVF_GLOBAL_ONLY), 5);
    r.str(kData, "Lines");
    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, 0), DOSTRUE);
    CHECK_EQ(get_var(r, "Lines", 32, GVF_GLOBAL_ONLY), -1);

    // DeleteVar GVF_GLOBAL_ONLY removes ENV:<name>; SetVar with null buffer also deletes.
    r.str(kData, "Foo");
    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, GVF_GLOBAL_ONLY), DOSTRUE);
    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, GVF_GLOBAL_ONLY), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    r.str(kData, "ENV:Foo");
    CHECK_EQ(r.call(dos_lvo::Open, kData, MODE_OLDFILE), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);

    CHECK_EQ(set_var(r, "Temp", "123", GVF_GLOBAL_ONLY), DOSTRUE);
    r.str(kData, "Temp");
    CHECK_EQ(r.call(dos_lvo::SetVar, kData, 0, 0, GVF_GLOBAL_ONLY), DOSTRUE);
    CHECK_EQ(get_var(r, "Temp", 32, GVF_GLOBAL_ONLY), -1);

    // Subdirectory variable: missing ENV:a fails; works after CreateDir("ENV:a").
    CHECK_EQ(set_var(r, "a/b", "sub", GVF_GLOBAL_ONLY), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DIR_NOT_FOUND);
    r.str(kData, "ENV:a");
    const u32 sub_lock = r.call(dos_lvo::CreateDir, kData);
    CHECK(sub_lock != 0);
    r.call(dos_lvo::UnLock, sub_lock);
    CHECK_EQ(set_var(r, "a/b", "sub", GVF_GLOBAL_ONLY), DOSTRUE);
    CHECK_EQ(get_var(r, "a/b", 32, 0), 3);
    CHECK(r.read_str(kData + 0x100) == "sub");

    // Reassigning ENV: follows the assign; pointing ENV: to ADF rejects writes.
    r.str(kData, "RAM:AltEnv");
    const u32 alt_dir = r.call(dos_lvo::CreateDir, kData);
    CHECK(alt_dir != 0);
    r.call(dos_lvo::UnLock, alt_dir);
    const u32 alt_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(alt_lock != 0);
    r.str(kData, "ENV");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, alt_lock), DOSTRUE);
    CHECK_EQ(set_var(r, "Alt", "val", GVF_GLOBAL_ONLY), DOSTRUE);
    r.str(kData, "RAM:AltEnv/Alt");
    const u32 alt_fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(alt_fh != 0);
    CHECK_EQ(r.call(dos_lvo::Read, alt_fh, buf, 16), 3u);
    CHECK(r.bytes(buf, 3) == text("val"));
    CHECK_EQ(r.call(dos_lvo::Close, alt_fh), DOSTRUE);
    r.str(kData, "Alt");
    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, GVF_GLOBAL_ONLY), DOSTRUE);

    r.str(kData, "DF0:S");
    const u32 adf_s = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(adf_s != 0);
    r.str(kData, "ENV");
    CHECK_EQ(r.call(dos_lvo::AssignLock, kData, adf_s), DOSTRUE);
    CHECK_EQ(get_var(r, "Startup-Sequence", 32, GVF_GLOBAL_ONLY), 10);
    CHECK(r.read_str(kData + 0x100) == "echo hello");
    CHECK_EQ(r.io_err(), 11u);
    CHECK_EQ(set_var(r, "ReadOnly", "x", GVF_GLOBAL_ONLY), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    r.str(kData, "Startup-Sequence");
    CHECK_EQ(r.call(dos_lvo::DeleteVar, kData, GVF_GLOBAL_ONLY), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
}

void ram_handler_port_and_zero_dates() {
    // RAM: has its own MsgPort on DosList nodes, locks, handles, GetDeviceProc and DoPkt.
    {
        Rig r;
        const u32 adf_port = r.call(dos_lvo::GetFileSysTask);
        CHECK(adf_port != 0);

        const u32 head = r.call(dos_lvo::LockDosList, LDF_ALL | LDF_READ);
        r.str(kData, "RAM");
        const u32 ram_dev = r.call(dos_lvo::FindDosEntry, head, kData, LDF_DEVICES);
        CHECK(ram_dev != 0);
        const u32 ram_port = r.get32(ram_dev + dol_Task);
        CHECK(ram_port != 0);
        CHECK(ram_port != adf_port);

        r.str(kData, "Ram Disk");
        const u32 ram_vol = r.call(dos_lvo::FindDosEntry, head, kData, LDF_VOLUMES);
        CHECK(ram_vol != 0);
        CHECK_EQ(r.get32(ram_vol + dol_Task), ram_port);

        r.str(kData, "DF0");
        const u32 df0_dev = r.call(dos_lvo::FindDosEntry, head, kData, LDF_DEVICES);
        CHECK(df0_dev != 0);
        CHECK_EQ(r.get32(df0_dev + dol_Task), adf_port);

        r.str(kData, "HombreTest");
        const u32 adf_vol = r.call(dos_lvo::FindDosEntry, head, kData, LDF_VOLUMES);
        CHECK(adf_vol != 0);
        CHECK_EQ(r.get32(adf_vol + dol_Task), adf_port);
        r.call(dos_lvo::UnLockDosList, LDF_ALL | LDF_READ);

        const u32 id = kData + 0x100;
        CHECK_EQ(r.call(dos_lvo::DoPkt, ram_port, ACTION_DISK_INFO, id >> 2), DOSTRUE);
        CHECK_EQ(r.get32(id + id_DiskState), ID_VALIDATED);
        CHECK_EQ(r.get32(id + id_VolumeNode), ram_vol >> 2);

        CHECK_EQ(r.call(dos_lvo::DoPkt, adf_port, ACTION_DISK_INFO, id >> 2), DOSTRUE);
        CHECK_EQ(r.get32(id + id_DiskState), ID_WRITE_PROTECTED);
        CHECK_EQ(r.get32(id + id_VolumeNode), adf_vol >> 2);

        CHECK_EQ(r.call(dos_lvo::DoPkt, ram_port, ACTION_CURRENT_VOLUME), ram_vol >> 2);

        for (const char* ram_name : {"RAM:", "Ram Disk:", "T:", "ENV:"}) {
            r.str(kData, ram_name);
            const u32 dp = r.call(dos_lvo::GetDeviceProc, kData, 0);
            CHECK(dp != 0);
            CHECK_EQ(r.get32(dp + dvp_Port), ram_port);
            r.call(dos_lvo::FreeDeviceProc, dp);
        }
        for (const char* adf_name : {"SYS:", "DF0:", "HombreTest:"}) {
            r.str(kData, adf_name);
            const u32 dp = r.call(dos_lvo::GetDeviceProc, kData, 0);
            CHECK(dp != 0);
            CHECK_EQ(r.get32(dp + dvp_Port), adf_port);
            r.call(dos_lvo::FreeDeviceProc, dp);
        }

        r.str(kData, "RAM:T");
        const u32 ram_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(ram_lock != 0);
        CHECK_EQ(r.get32((ram_lock << 2) + fl_Task), ram_port);

        r.str(kData, "RAM:probe");
        const u32 ram_fh = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
        CHECK(ram_fh != 0);
        CHECK_EQ(r.get32(handle_at(ram_fh) + fh_Type), ram_port);
        r.call(dos_lvo::Close, ram_fh);

        const u32 bstr = kData + 0x200;
        const u32 buf = kData + 0x300;
        put_bstr_at(r, bstr, "T");
        const u32 pkt_t =
            r.call(dos_lvo::DoPkt, ram_port, ACTION_LOCATE_OBJECT, 0, bstr >> 2, SHARED_LOCK);
        CHECK(pkt_t != 0);
        CHECK_EQ(r.call(dos_lvo::SameLock, pkt_t, ram_lock), LOCK_SAME);
        CHECK_EQ(r.call(dos_lvo::NameFromLock, pkt_t, buf, 64), DOSTRUE);
        CHECK(r.read_str(buf) == "Ram Disk:T");
        r.call(dos_lvo::UnLock, pkt_t);
        r.call(dos_lvo::UnLock, ram_lock);
    }

    // Boot-time RAM nodes with zero dates receive the clock date when set_clock runs;
    // ENVARC copies keep their non-zero source dates.
    {
        std::vector<AdfFile> files = standard_files();
        files.push_back({"Prefs/Env-Archive/kept", text("v")});
        std::vector<u8> image = build_adf("HombreTest", false, files);
        AdfVolume vol{};
        CHECK(adf_mount(image.data(), image.size(), vol) == FsError::Ok);
        DirEntry kept{};
        CHECK(adf_find(vol, "Prefs/Env-Archive/kept", kept) == FsError::Ok);
        const auto put_be32_at = [&](u32 block, u32 long_idx, u32 value) {
            const usize off = static_cast<usize>(block) * 512 + long_idx * 4;
            image[off + 0] = static_cast<u8>(value >> 24);
            image[off + 1] = static_cast<u8>(value >> 16);
            image[off + 2] = static_cast<u8>(value >> 8);
            image[off + 3] = static_cast<u8>(value);
        };
        put_be32_at(kept.header_block, 105, 42);
        put_be32_at(kept.header_block, 106, 100);
        put_be32_at(kept.header_block, 107, 200);
        adf_fix_checksum(image, kept.header_block);

        Rig r(std::move(image));
        g_dos.set_clock(fake_clock_2026);

        const u32 fib = kData + 0x200;
        r.str(kData, "RAM:T");
        const u32 t_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(t_lock != 0);
        CHECK_EQ(r.call(dos_lvo::Examine, t_lock, fib), DOSTRUE);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Days), 17853u);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Minute), 17u * 60u + 30u);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Tick), 45u * 50u);
        r.call(dos_lvo::UnLock, t_lock);

        r.str(kData, "ENV:kept");
        const u32 kept_lock = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
        CHECK(kept_lock != 0);
        CHECK_EQ(r.call(dos_lvo::Examine, kept_lock, fib), DOSTRUE);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Days), 42u);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Minute), 100u);
        CHECK_EQ(r.get32(fib + fib_DateStamp + ds_Tick), 200u);
        r.call(dos_lvo::UnLock, kept_lock);
    }
}

struct FakeDelayHost {
    u64 now_us = 100000;
    u64 step_us = 20000;
    u32 idle_calls = 0;
    bool idle_ok = false;
    bool send_ctrl_c_at_idle = false;
    u32 ctrl_c_on_call = 0;
    std::vector<std::pair<u32, u32>> switches;
};

bool fake_delay_create(void*, u32) { return true; }
void fake_delay_switch(void* ctx, u32 from, u32 to) {
    static_cast<FakeDelayHost*>(ctx)->switches.push_back({from, to});
}
void fake_delay_destroy(void*, u32) {}
bool fake_delay_slice_over(void*) { return false; }
void fake_delay_poll(void*) {}
bool fake_delay_idle(void* ctx) {
    FakeDelayHost& h = *static_cast<FakeDelayHost*>(ctx);
    ++h.idle_calls;
    if (h.send_ctrl_c_at_idle && h.idle_calls == h.ctrl_c_on_call) {
        g_exec.signal_current_task(SIGBREAKF_CTRL_C);
    }
    h.now_us += h.step_us;
    return h.idle_ok;
}
u64 fake_delay_now(void* ctx) { return static_cast<FakeDelayHost*>(ctx)->now_us; }

void delay_blocks_and_wakes_with_fake_host() {
    // Delay(0) returns immediately without idling, and Delay(ticks) idles
    // until now_us reaches deadline even when idle returns false.
    {
        Rig r;
        FakeDelayHost host{};
        g_exec.set_task_host(TaskHost{fake_delay_create, fake_delay_switch, fake_delay_destroy,
                                      fake_delay_slice_over, fake_delay_poll, fake_delay_idle,
                                      fake_delay_now, &host});
        CHECK_EQ(r.call(dos_lvo::Delay, 0), 0u);
        CHECK_EQ(host.idle_calls, 0u);

        CHECK_EQ(r.call(dos_lvo::Delay, 5), 0u);
        CHECK_EQ(host.idle_calls, 5u);
        CHECK_EQ(host.now_us, 200000u);
    }

    // With a child task ready, Delay marks the caller Waiting with a deadline
    // and switches away; Signal does not wake it early, and passing the
    // deadline wakes it back to Ready.
    {
        Rig r;
        FakeDelayHost host{};
        g_exec.set_task_host(TaskHost{fake_delay_create, fake_delay_switch, fake_delay_destroy,
                                      fake_delay_slice_over, fake_delay_poll, fake_delay_idle,
                                      fake_delay_now, &host});
        const u32 boot = r.proc();
        const u32 child = kData + 0x200;
        for (u32 i = 0; i < TC_SIZE; ++i) {
            r.put8(child + i, 0);
        }
        r.put8(child + LN_TYPE, NT_TASK);
        r.put8(child + LN_PRI, 0);
        r.put32(child + TC_SPLOWER, kData + 0x1000);
        r.put32(child + TC_SPUPPER, kData + 0x2000);
        r.put32(child + TC_SPREG, kData + 0x2000);
        r.cpu().a[1] = child;
        r.cpu().a[2] = 0x2000;
        r.cpu().a[3] = 0;
        CHECK_EQ(r.exec(exec_lvo::AddTask), ExitReason::Halt);

        CHECK_EQ(r.call(dos_lvo::Delay, 10), 0u);
        CHECK_EQ(g_exec.scheduler().current(), 1);
        CHECK_EQ(r.get8(boot + TC_STATE), TS_WAIT);
        CHECK(g_exec.scheduler().slot(0).state == TaskState::Waiting);
        CHECK_EQ(host.switches.size(), 1u);
        CHECK_EQ(host.switches[0].first, 0u);
        CHECK_EQ(host.switches[0].second, 1u);

        // Sending a signal (even Ctrl+C) does not wake a sleeping task early.
        r.cpu().a[1] = boot;
        r.cpu().d[0] = SIGBREAKF_CTRL_C;
        CHECK_EQ(r.exec(exec_lvo::Signal), ExitReason::Halt);
        CHECK(g_exec.scheduler().slot(0).state == TaskState::Waiting);

        // Advancing before the deadline keeps it Waiting.
        r.cpu().exit = ExitReason::None;
        host.now_us = 299999;
        CHECK(!g_exec.yield_in_native());
        CHECK(g_exec.scheduler().slot(0).state == TaskState::Waiting);

        // Reaching the deadline wakes boot and switches back on yield.
        host.now_us = 300000;
        CHECK(g_exec.yield_in_native());
        CHECK_EQ(g_exec.scheduler().current(), 0);
        CHECK_EQ(r.get8(boot + TC_STATE), TS_RUN);
        g_exec.end_child_tasks();
    }

    // Ctrl+C arriving on the boot task during Delay ends the run with Break
    // once the delay finishes.
    {
        Rig r;
        FakeDelayHost host{};
        host.send_ctrl_c_at_idle = true;
        host.ctrl_c_on_call = 2;
        g_exec.set_task_host(TaskHost{fake_delay_create, fake_delay_switch, fake_delay_destroy,
                                      fake_delay_slice_over, fake_delay_poll, fake_delay_idle,
                                      fake_delay_now, &host});
        r.cpu().d[1] = 4;
        CHECK_EQ(r.dos(dos_lvo::Delay), ExitReason::Break);
        CHECK_EQ(host.idle_calls, 4u);
        CHECK_EQ(host.now_us, 180000u);
    }
}

bool noop_create(void*, u32) { return true; }
void noop_switch(void*, u32, u32) {}
void noop_destroy(void*, u32) {}

void use_noop_task_host() {
    g_exec.set_task_host(TaskHost{noop_create, noop_switch, noop_destroy, nullptr, nullptr,
                                  nullptr, nullptr, nullptr});
}

void idle_task(void*) {}

u32 load_echo_seg(Rig& r) {
    r.str(kData, "C:Hello");
    const u32 seglist = r.call(dos_lvo::LoadSeg, kData);
    CHECK(seglist != 0);
    return seglist;
}

u32 create_cli(Rig& r, u32 seglist, const char* name, const char* args, u32 stack = 0) {
    const u32 tags = kData + 0x400;
    const u32 name_at = kData + 0x500;
    const u32 args_at = kData + 0x600;
    r.str(name_at, name);
    r.str(args_at, args);
    u32 n = 0;
    auto tag = [&](u32 tag_value, u32 data) {
        r.put32(tags + n, tag_value);
        r.put32(tags + n + 4, data);
        n += 8;
    };
    tag(NP_Seglist, seglist);
    tag(NP_FreeSeglist, 0);
    tag(NP_Name, name_at);
    tag(NP_Cli, DOSTRUE);
    tag(NP_Arguments, args_at);
    if (stack != 0) {
        tag(NP_StackSize, stack);
    }
    tag(TAG_DONE, 0);
    return r.call(dos_lvo::CreateNewProc, tags);
}

void contexts_keep_their_state_apart() {
    Rig r;
    use_noop_task_host();
    const u32 seglist = load_echo_seg(r);
    const u32 p2 = create_cli(r, seglist, "two", "alpha beta");
    const u32 p3 = create_cli(r, seglist, "three", "gamma");
    CHECK(p2 != 0);
    CHECK(p3 != 0);
    CHECK_EQ(r.call(dos_lvo::MaxCli), 3u);
    CHECK_EQ(r.call(dos_lvo::FindCliProc, 2), p2);
    CHECK_EQ(r.call(dos_lvo::FindCliProc, 3), p3);
    CHECK_EQ(r.call(dos_lvo::FindCliProc, 4), 0u);

    const u32 root = r.get32(r.base() + dl_Root);
    const u32 tasks = r.get32(root + rn_TaskArray) << 2;
    CHECK_EQ(r.get32(tasks), Dos::kMaxProcs);
    CHECK_EQ(r.get32(tasks + 2 * 4), p2 + pr_MsgPort);
    CHECK_EQ(r.get32(tasks + 3 * 4), p3 + pr_MsgPort);

    g_exec.set_this_task(p2);
    CHECK(r.read_str(r.call(dos_lvo::GetArgStr)).find("alpha beta") == 0);
    r.str(kData, "RAM:ctx2");
    const u32 h2 = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    r.str(kData, "RAM:T");
    const u32 l2 = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    g_exec.set_this_task(p3);
    CHECK(r.read_str(r.call(dos_lvo::GetArgStr)).find("gamma") == 0);
    r.str(kData, "RAM:ctx3");
    const u32 h3 = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    r.str(kData, "RAM:T");
    const u32 l3 = r.call(dos_lvo::Lock, kData, SHARED_LOCK);
    CHECK(h2 != 0);
    CHECK(h3 != 0);
    CHECK(h2 != h3);
    CHECK(l2 != 0);
    CHECK(l3 != 0);
    CHECK(l2 != l3);

    g_exec.set_this_task(p2);
    CHECK_EQ(r.call(dos_lvo::Exit, 7), 7u);
    r.m.cpu.exit = ExitReason::None;
    g_exec.set_this_task(r.proc());
    r.cpu().a[1] = p2;
    CHECK_EQ(r.exec(exec_lvo::RemTask), ExitReason::Halt);
    CHECK(logged("dos: CLI 2 ended, returned 7"));
    CHECK_EQ(r.call(dos_lvo::FindCliProc, 2), 0u);

    CHECK_EQ(r.call(dos_lvo::Read, h3, kData + 0x300, 4), 0u);
    CHECK_EQ(r.call(dos_lvo::Read, h2, kData + 0x300, 4), 0xFFFFFFFFu);

    g_exec.set_this_task(p3);
    CHECK_EQ(r.call(dos_lvo::Exit, 3), 3u);
    r.m.cpu.exit = ExitReason::None;
    g_exec.set_this_task(r.proc());
    r.cpu().a[1] = p3;
    CHECK_EQ(r.exec(exec_lvo::RemTask), ExitReason::Halt);
    CHECK(logged("dos: CLI 3 ended, returned 3"));
}

void nil_reads_end_of_file_and_swallows_writes() {
    Rig r;
    r.str(kData, "NIL:");
    const u32 handle = r.call(dos_lvo::Open, kData, MODE_NEWFILE);
    CHECK(handle != 0);
    r.str(kData + 0x100, "abc");
    CHECK_EQ(r.call(dos_lvo::Write, handle, kData + 0x100, 3), 3u);
    CHECK_EQ(r.call(dos_lvo::Read, handle, kData + 0x200, 8), 0u);
    CHECK_EQ(r.call(dos_lvo::Close, handle), DOSTRUE);
}

void create_new_proc_builds_a_process() {
    Rig r;
    use_noop_task_host();
    const u32 seglist = load_echo_seg(r);
    const u32 process = create_cli(r, seglist, "newproc", "arg", 2048);
    CHECK(process != 0);
    CHECK_EQ(r.get8(process + LN_TYPE), NT_PROCESS);
    CHECK(r.read_str(r.get32(process + LN_NAME)) == "newproc");
    CHECK_EQ(r.get32(process + pr_TaskNum), 2u);
    CHECK_EQ(r.get32(process + pr_StackSize), 2048u);
    CHECK(r.read_str(r.get32(process + pr_Arguments)).find("arg") == 0);
    const u32 cli = r.get32(process + pr_CLI) << 2;
    CHECK(cli != 0);
    CHECK_EQ(r.get32(cli + cli_Background), DOSTRUE);
    CHECK_EQ(r.get32(cli + cli_DefaultStack), 512u);
    CHECK_EQ(r.get32(cli + cli_StandardInput), r.get32(process + pr_CIS));
    CHECK_EQ(r.get32(cli + cli_CurrentOutput), r.get32(process + pr_COS));

    const u32 root = r.get32(r.base() + dl_Root);
    const u32 tasks = r.get32(root + rn_TaskArray) << 2;
    CHECK_EQ(r.get32(tasks + 2 * 4), process + pr_MsgPort);

    g_exec.set_this_task(process);
    const u32 in = r.call(dos_lvo::Input);
    CHECK_EQ(in, r.get32(process + pr_CIS));
    r.str(kData + 0x300, "abc");
    CHECK_EQ(r.call(dos_lvo::Read, in, kData + 0x380, 8), 0u);
    CHECK_EQ(r.call(dos_lvo::Write, in, kData + 0x300, 3), 3u);
    g_exec.set_this_task(r.proc());
}

// --- multiple consoles ----------------------------------------------------

struct TestConsole {
    std::string written;
    std::vector<std::string> lines;
    usize next_line = 0;
    std::vector<u32> timeouts;
    bool ready = false;
};

void test_console_write(void* ctx, const u8* data, u32 bytes) {
    static_cast<TestConsole*>(ctx)->written.append(reinterpret_cast<const char*>(data), bytes);
}

u32 test_console_read(void* ctx, u8* buf, u32 size) {
    TestConsole& c = *static_cast<TestConsole*>(ctx);
    if (c.next_line >= c.lines.size() || size == 0) {
        return 0;
    }
    const std::string& s = c.lines[c.next_line++];
    const u32 n = static_cast<u32>(std::min<usize>(s.size(), size));
    std::memcpy(buf, s.data(), n);
    return n;
}

bool test_console_ready(void* ctx, u32 timeout_us) {
    TestConsole& c = *static_cast<TestConsole*>(ctx);
    c.timeouts.push_back(timeout_us);
    return c.ready;
}

void console_handles_write_to_their_console() {
    Rig r;
    TestConsole one;
    g_dos.set_console(1, test_console_write, test_console_read, test_console_ready, &one);

    u32 in0 = 0;
    u32 out0 = 0;
    u32 in1 = 0;
    u32 out1 = 0;
    CHECK(g_dos.open_console_handles(0, in0, out0));
    CHECK(g_dos.open_console_handles(1, in1, out1));
    CHECK(in0 != 0 && out0 != 0 && in1 != 0 && out1 != 0);
    CHECK(in0 != out0 && in1 != out1);

    r.str(kData, "boot");
    CHECK_EQ(r.call(dos_lvo::Write, out0, kData, 4), 4u);
    CHECK_EQ(r.call(dos_lvo::Write, in0, kData, 4), 4u);
    CHECK(g_console == "bootboot");

    r.str(kData + 0x100, "one");
    CHECK_EQ(r.call(dos_lvo::Write, out1, kData + 0x100, 3), 3u);
    CHECK_EQ(r.call(dos_lvo::Write, in1, kData + 0x100, 3), 3u);
    CHECK(one.written == "oneone");
    CHECK(g_console == "bootboot");

    // WaitForChar asks the readiness of the handle's console.
    one.ready = true;
    CHECK_EQ(r.call(dos_lvo::WaitForChar, in1, 1234), DOSTRUE);
    CHECK_EQ(one.timeouts.size(), 1u);
    CHECK_EQ(one.timeouts[0], 1234u);

    u32 bad_in = 1;
    u32 bad_out = 2;
    CHECK(!g_dos.open_console_handles(Dos::kMaxConsoles, bad_in, bad_out));
    CHECK_EQ(bad_in, 0u);
    CHECK_EQ(bad_out, 0u);
}

void a_process_reads_and_writes_its_console() {
    Rig r;
    TestConsole one;
    one.lines = {"line\n"};
    g_dos.set_console(1, test_console_write, test_console_read, nullptr, &one);

    u32 process = 0;
    u32 cli_number = 0;
    CHECK(g_dos.create_shell_process(1, process, cli_number));
    const u32 cis = r.get32(process + pr_CIS);
    const u32 cos = r.get32(process + pr_COS);
    CHECK(cis != 0 && cos != 0);

    g_exec.set_this_task(process);
    r.str(kData, "out");
    CHECK_EQ(r.call(dos_lvo::Write, cos, kData, 3), 3u);
    CHECK(one.written == "out");
    CHECK(g_console.empty());

    // The argument line first, then the console's own reader.
    CHECK_EQ(r.call(dos_lvo::Read, cis, kData + 0x100, 8), 1u);
    CHECK_EQ(r.get8(kData + 0x100), static_cast<u32>('\n'));
    CHECK_EQ(r.call(dos_lvo::Read, cis, kData + 0x100, 8), 5u);
    CHECK(r.bytes(kData + 0x100, 5) == text("line\n"));
    g_exec.set_this_task(r.proc());
}

// A background process writes to the console of the shell that started it
// and shares that shell's output handle; its end must not close the handle.
void background_process_uses_and_leaves_its_shell_output() {
    Rig r;
    use_noop_task_host();
    TestConsole one;
    g_dos.set_console(1, test_console_write, test_console_read, test_console_ready, &one);
    u32 shell = 0;
    u32 shell_cli = 0;
    CHECK(g_dos.create_shell_process(1, shell, shell_cli));
    g_exec.set_this_task(shell);
    const u32 shell_out = r.call(dos_lvo::Output);

    const u32 cli = g_dos.start_background("C:Hello", "");
    CHECK(cli != 0);
    const u32 process = r.call(dos_lvo::FindCliProc, cli);
    CHECK(process != 0);
    CHECK_EQ(r.get32(process + pr_COS), shell_out);
    r.cpu().a[1] = process;
    CHECK_EQ(r.exec(exec_lvo::RemTask), ExitReason::Halt);
    CHECK(logged_containing(" ended, returned"));

    r.str(kData, "still");
    CHECK_EQ(r.call(dos_lvo::Write, shell_out, kData, 5), 5u);
    CHECK(one.written == "still");
}

void create_shell_process_builds_a_cli() {
    Rig r;
    TestConsole one;
    g_dos.set_console(1, test_console_write, test_console_read, nullptr, &one);

    u32 err = 0;
    CHECK(g_dos.change_dir("S:", err));
    g_dos.set_prompt("[shell] ");

    u32 process = 0;
    u32 cli_number = 0;
    CHECK(g_dos.create_shell_process(1, process, cli_number));
    CHECK(process != 0);
    CHECK(process != r.proc());
    CHECK_EQ(cli_number, 2u);
    CHECK_EQ(r.get8(process + LN_TYPE), NT_PROCESS);
    CHECK(r.read_str(r.get32(process + LN_NAME)) == "HombreShell");
    CHECK_EQ(r.get32(process + pr_TaskNum), 2u);
    CHECK_EQ(r.get32(process + pr_StackSize), Dos::kStackBytes);

    const u32 cli = r.get32(process + pr_CLI) << 2;
    const u32 cis = r.get32(process + pr_CIS);
    const u32 cos = r.get32(process + pr_COS);
    CHECK(cli != 0);
    CHECK(cis != 0 && cos != 0 && cis != cos);
    CHECK_EQ(r.get32(cli + cli_StandardInput), cis);
    CHECK_EQ(r.get32(cli + cli_CurrentInput), cis);
    CHECK_EQ(r.get32(cli + cli_StandardOutput), cos);
    CHECK_EQ(r.get32(cli + cli_CurrentOutput), cos);
    CHECK_EQ(r.get32(cli + cli_Interactive), DOSTRUE);
    CHECK_EQ(r.get32(cli + cli_Background), DOSFALSE);
    CHECK(r.read_bstr(r.get32(cli + cli_Prompt)) == "[shell] ");

    // The current directory is a copy of the boot shell's.
    const u32 dir = r.get32(process + pr_CurrentDir);
    CHECK(dir != 0);
    CHECK_EQ(r.call(dos_lvo::NameFromLock, dir, kData, 64), DOSTRUE);
    CHECK(r.read_str(kData) == "HombreTest:S");

    const u32 root = r.get32(r.base() + dl_Root);
    const u32 tasks = r.get32(root + rn_TaskArray) << 2;
    CHECK_EQ(r.get32(tasks + cli_number * 4), process + pr_MsgPort);

    // Its console handles are console 1's.
    g_exec.set_this_task(process);
    r.str(kData + 0x200, "shell");
    CHECK_EQ(r.call(dos_lvo::Write, cos, kData + 0x200, 5), 5u);
    CHECK(one.written == "shell");
    CHECK(g_console.empty());
    g_exec.set_this_task(r.proc());
}

void run_command_in_runs_commands_in_the_process() {
    Rig r;
    TestConsole one;
    g_dos.set_console(1, test_console_write, test_console_read, nullptr, &one);
    g_dos.set_fail_level(25);

    u32 process = 0;
    u32 cli_number = 0;
    CHECK(g_dos.create_shell_process(1, process, cli_number));
    g_exec.set_this_task(process);

    u32 result = 0;
    CHECK(g_dos.run_command_in(process, "C:Hello", "", result, kBlocks));
    CHECK_EQ(result, 3u);
    CHECK(one.written == "hi\n");
    CHECK(g_console.empty());
    CHECK_EQ(g_dos.last_return_code(process), 3u);
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.fail_level(process), 10u);
    CHECK_EQ(g_dos.fail_level(), 25u);

    // The command's cleanup leaves the process's own console handles open.
    r.str(kData + 0x300, "after");
    CHECK_EQ(r.call(dos_lvo::Write, r.get32(process + pr_COS), kData + 0x300, 5), 5u);
    CHECK(one.written == "hi\nafter");

    r.call(dos_lvo::SetIoErr, ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(g_dos.last_error(process), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(g_dos.last_error(), 0u);

    CHECK(!g_dos.run_command_in(r.proc() + 4, "C:Hello", "", result, kBlocks));
    g_exec.set_this_task(r.proc());
}

void run_command_keeps_the_callers_stack() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/RunStack", run_stack_command()});
    Rig r(files);
    u32 result = 1;
    CHECK(g_dos.run_command("C:RunStack", "", result, kBlocks));
    CHECK_EQ(result, 0u);
    const u32 frame = r.get32(kOut + 8);
    for (u32 i = 0; i < 16; ++i) {
        CHECK_EQ(r.get32(frame + i * 4), 0xC0DE0000u | i);
    }
    CHECK_EQ(r.get32(kOut + 4), 3u);  // C:Hello's return code
    CHECK(g_console == "hi\n");
}

void nested_runs_stop_at_the_limit() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/Nest", nest_command()});
    Rig r(files);
    u32 result = 0;
    CHECK(g_dos.run_command("C:Nest", "", result, kBlocks));
    CHECK_EQ(result, 0xFFFFFFFFu);
    CHECK_EQ(g_dos.last_error(), ERROR_NO_FREE_STORE);
    CHECK(logged_containing("nested too deep"));
}

// CreateProc, CreateNewProc and an asynchronous System with no output of
// their own share the starter's output and leave it open when they end.
void processes_leave_the_parents_output_open() {
    Rig r;
    use_noop_task_host();
    const u32 original = r.call(dos_lvo::Output);
    r.str(kData + 0x800, "*");
    const u32 shared = r.call(dos_lvo::Open, kData + 0x800, MODE_OLDFILE);
    CHECK(shared != 0);
    CHECK_EQ(r.call(dos_lvo::SelectOutput, shared), original);

    auto end_process = [&](u32 process) {
        CHECK(process != 0);
        r.cpu().a[1] = process;
        CHECK_EQ(r.exec(exec_lvo::RemTask), ExitReason::Halt);
    };
    auto write_still_open = [&](const char* s) {
        r.str(kData + 0x900, s);
        const u32 n = static_cast<u32>(std::strlen(s));
        CHECK_EQ(r.call(dos_lvo::Write, shared, kData + 0x900, n), n);
    };

    const u32 seglist = load_echo_seg(r);
    r.str(kData + 0x400, "createproc");
    const u32 port = r.call(dos_lvo::CreateProc, kData + 0x400, 0, seglist, 4096);
    CHECK(port != 0);
    end_process(port - pr_MsgPort);
    write_still_open("one");

    const u32 child = create_cli(r, seglist, "newproc", "", 0);
    CHECK(child != 0);
    CHECK_EQ(r.get32(child + pr_COS), shared);
    end_process(child);
    write_still_open("two");

    const u32 tags = kData + 0x400;
    r.put32(tags, SYS_Asynch);
    r.put32(tags + 4, DOSTRUE);
    r.put32(tags + 8, TAG_DONE);
    r.put32(tags + 12, 0);
    r.str(kData, "C:Hello");
    CHECK_EQ(r.call(dos_lvo::SystemTagList, kData, tags), 0u);
    const u32 async = r.call(dos_lvo::FindCliProc, 2);
    CHECK(async != 0);
    CHECK_EQ(r.get32(async + pr_COS), shared);
    end_process(async);
    write_still_open("three");

    CHECK(g_console == "onetwothree");
    CHECK_EQ(r.call(dos_lvo::SelectOutput, original), shared);
    r.call(dos_lvo::Close, shared);
    r.call(dos_lvo::UnLoadSeg, seglist);
}

void a_passed_handle_belongs_to_the_child() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"C/SpawnOwn", spawn_with_own_handle_command()});
    Rig r(files);
    use_noop_task_host();
    u32 result = 1;
    CHECK(g_dos.run_command("C:SpawnOwn", "", result, kBlocks));
    CHECK_EQ(result, 0u);
    const u32 handle = r.get32(kOut);
    const u32 child = r.get32(kOut + 8);
    CHECK(handle != 0);
    CHECK(child != 0);
    CHECK_EQ(r.get32(child + pr_COS), handle);

    // The command's cleanup left the handle to the child.
    r.str(kData + 0x100, "live");
    CHECK_EQ(r.call(dos_lvo::Write, handle, kData + 0x100, 4), 4u);

    // The child closes it when it ends.
    r.cpu().a[1] = child;
    CHECK_EQ(r.exec(exec_lvo::RemTask), ExitReason::Halt);
    CHECK_EQ(as_signed(r.call(dos_lvo::Write, handle, kData + 0x100, 4)), -1);
}

void stale_handle_bptrs_are_rejected() {
    Rig r;
    r.str(kData, "C:Echo");
    const u32 first = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(first != 0);
    CHECK_EQ(r.call(dos_lvo::Close, first), DOSTRUE);
    const u32 second = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(second != 0);
    CHECK_EQ(handle_at(second), handle_at(first));
    CHECK(second != first);

    g_lines.clear();
    CHECK_EQ(r.call(dos_lvo::Close, first), DOSFALSE);
    CHECK(logged_containing("dos: Close of a bad file handle"));
    CHECK_EQ(as_signed(r.call(dos_lvo::Read, first, kData + 0x100, 1)), -1);
    CHECK_EQ(r.io_err(), ERROR_INVALID_LOCK);
    CHECK_EQ(r.call(dos_lvo::Read, second, kData + 0x100, 1), 1u);
    r.call(dos_lvo::Close, second);
}

// A shell's console pair closes only when the last process using it ends.
void a_shell_console_pair_serves_its_children() {
    Rig r;
    use_noop_task_host();
    TestConsole one;
    g_dos.set_console(1, test_console_write, test_console_read, nullptr, &one);
    u32 shell = 0;
    u32 shell_cli = 0;
    CHECK(g_dos.create_shell_process(1, shell, shell_cli));
    CHECK(g_exec.spawn_native(shell, idle_task, nullptr, 0, false) != 0);
    const u32 cos = r.get32(shell + pr_COS);

    g_exec.set_this_task(shell);
    const u32 child_cli = g_dos.start_background("C:Hello", "");
    CHECK(child_cli != 0);
    const u32 child = r.call(dos_lvo::FindCliProc, child_cli);
    CHECK(child != 0);
    CHECK_EQ(r.get32(child + pr_COS), cos);

    // The shell ends while the child still writes to its output.
    g_exec.set_this_task(r.proc());
    g_exec.end_tasks_of(r.proc());
    CHECK(logged_containing("ended task HombreShell"));
    r.str(kData, "live");
    CHECK_EQ(r.call(dos_lvo::Write, cos, kData, 4), 4u);
    CHECK(one.written == "live");

    // The child's end closes the pair.
    r.cpu().a[1] = child;
    CHECK_EQ(r.exec(exec_lvo::RemTask), ExitReason::Halt);
    CHECK_EQ(as_signed(r.call(dos_lvo::Write, cos, kData, 4)), -1);
}

// A command run in a shell ends the shell's non-detached child tasks and
// resets its Forbid count.
void run_command_in_ends_the_shells_children() {
    Rig r;
    TestConsole one;
    g_dos.set_console(1, test_console_write, test_console_read, nullptr, &one);
    u32 shell = 0;
    u32 shell_cli = 0;
    CHECK(g_dos.create_shell_process(1, shell, shell_cli));
    g_exec.set_this_task(shell);

    const u32 child = kData + 0x1000;
    for (u32 i = 0; i < TC_SIZE; ++i) {
        r.put8(child + i, 0);
    }
    r.put8(child + LN_TYPE, NT_TASK);
    r.put8(child + LN_PRI, 0xFF);
    r.put32(child + TC_SPLOWER, kData + 0x2000);
    r.put32(child + TC_SPUPPER, kData + 0x3000);
    r.put32(child + TC_SPREG, kData + 0x3000);
    r.cpu().a[1] = child;
    r.cpu().a[2] = 0x400;
    r.cpu().a[3] = 0;
    CHECK_EQ(r.exec(exec_lvo::AddTask), ExitReason::Halt);
    CHECK_EQ(r.cpu().d[0], child);

    CHECK_EQ(r.exec(exec_lvo::Forbid), ExitReason::Halt);
    CHECK_EQ(r.exec(exec_lvo::Forbid), ExitReason::Halt);
    CHECK_EQ(r.get8(g_exec.exec_base() + TDNestCnt), 0x01u);

    u32 result = 0;
    CHECK(g_dos.run_command_in(shell, "C:Hello", "", result, kBlocks));
    CHECK_EQ(result, 3u);
    CHECK_EQ(r.get8(child + TC_STATE), TS_REMOVED);
    CHECK_EQ(r.get8(g_exec.exec_base() + TDNestCnt), 0xFFu);
    CHECK_EQ(r.get8(shell + TC_TDNESTCNT), 0xFFu);
    g_exec.set_this_task(r.proc());
}

void system_uses_the_shell_process_directory() {
    std::vector<AdfFile> files = standard_files();
    files.push_back({"S/CmdS", hello_command()});
    Rig r(files);
    TestConsole one;
    g_dos.set_console(1, test_console_write, test_console_read, nullptr, &one);
    u32 shell = 0;
    u32 shell_cli = 0;
    CHECK(g_dos.create_shell_process(1, shell, shell_cli));
    u32 err = 0;
    CHECK(g_dos.change_dir("S", err, shell));
    g_exec.set_this_task(shell);
    CHECK_EQ(r.call(dos_lvo::SystemTagList, r.str(kData, "CmdS"), 0), 3u);
    CHECK(one.written == "hi\n");
    CHECK(g_console.empty());
    g_exec.set_this_task(r.proc());
}

void console_star_follows_the_process_console() {
    Rig r;
    use_noop_task_host();
    TestConsole one;
    g_dos.set_console(1, test_console_write, test_console_read, nullptr, &one);
    u32 shell = 0;
    u32 shell_cli = 0;
    CHECK(g_dos.create_shell_process(1, shell, shell_cli));
    g_exec.set_this_task(shell);
    const u32 seglist = load_echo_seg(r);
    const u32 child = create_cli(r, seglist, "child", "", 0);
    CHECK(child != 0);

    g_exec.set_this_task(child);
    r.str(kData, "*");
    const u32 out = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(out != 0);
    r.str(kData + 0x100, "kid");
    CHECK_EQ(r.call(dos_lvo::Write, out, kData + 0x100, 3), 3u);
    CHECK(one.written == "kid");
    CHECK(g_console.empty());
    g_exec.set_this_task(r.proc());
}

void file_helpers_follow_the_volume_rules() {
    Rig r;
    u8 buf[64] = {};
    u32 got = 99;
    u32 error = 99;

    // examine: what a name is.
    const Dos::Target file = g_dos.examine("Devs/Sub/Deep");
    CHECK(file.kind == Dos::TargetKind::File);
    CHECK_EQ(file.size, 4u);
    CHECK(g_dos.examine("Devs/Sub").kind == Dos::TargetKind::Dir);
    CHECK(g_dos.examine("HombreTest:").kind == Dos::TargetKind::VolumeRoot);
    CHECK(g_dos.examine(":").kind == Dos::TargetKind::VolumeRoot);
    CHECK(g_dos.examine("RAM:").kind == Dos::TargetKind::VolumeRoot);
    CHECK(g_dos.examine("Devs/Nope").kind == Dos::TargetKind::Missing);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    CHECK(g_dos.examine("Nowhere:x").kind == Dos::TargetKind::Missing);
    CHECK_EQ(r.io_err(), ERROR_DEVICE_NOT_MOUNTED);
    CHECK(g_dos.examine(nullptr).kind == Dos::TargetKind::Missing);

    // read_file: the whole file or nothing.
    CHECK(g_dos.read_file("Devs/Sub/Deep", buf, sizeof buf, got, error));
    CHECK_EQ(got, 4u);
    CHECK_EQ(error, 0u);
    CHECK(std::string(buf, buf + 4) == "deep");
    CHECK(g_dos.read_file("C/Empty", buf, sizeof buf, got, error));
    CHECK_EQ(got, 0u);
    CHECK(!g_dos.read_file("Devs/Sub/Deep", buf, 3, got, error));
    CHECK_EQ(got, 0u);
    CHECK_EQ(error, ERROR_OBJECT_TOO_LARGE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_TOO_LARGE);
    CHECK(!g_dos.read_file("Devs/Sub", buf, sizeof buf, got, error));
    CHECK_EQ(error, ERROR_OBJECT_WRONG_TYPE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    CHECK(!g_dos.read_file("Devs/Nope", buf, sizeof buf, got, error));
    CHECK_EQ(error, ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);

    // write_file: RAM: only, into a directory that is there.
    const u8 first[] = {'o', 'n', 'e', ' ', 'l', 'o', 'n', 'g'};
    const u8 second[] = {'t', 'w', 'o'};
    CHECK(g_dos.write_file("RAM:Note", first, sizeof first, error));
    CHECK_EQ(error, 0u);
    CHECK_EQ(g_dos.examine("RAM:Note").size, 8u);
    CHECK(g_dos.write_file("RAM:Note", second, sizeof second, error));
    CHECK(g_dos.read_file("RAM:Note", buf, sizeof buf, got, error));
    CHECK_EQ(got, 3u);
    CHECK(std::string(buf, buf + 3) == "two");
    CHECK(!g_dos.write_file("S/New", second, sizeof second, error));
    CHECK_EQ(error, ERROR_DISK_WRITE_PROTECTED);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    CHECK(!g_dos.write_file("RAM:NoDir/New", second, sizeof second, error));
    CHECK_EQ(error, ERROR_DIR_NOT_FOUND);
    CHECK_EQ(r.io_err(), ERROR_DIR_NOT_FOUND);
    CHECK(!g_dos.write_file("RAM:Note/New", second, sizeof second, error));
    CHECK_EQ(error, ERROR_DIR_NOT_FOUND);
    CHECK(!g_dos.write_file("RAM:", second, sizeof second, error));
    CHECK_EQ(error, ERROR_OBJECT_WRONG_TYPE);
    CHECK(!g_dos.write_file("Nowhere:x", second, sizeof second, error));
    CHECK_EQ(error, ERROR_DEVICE_NOT_MOUNTED);

    // A file open elsewhere is neither replaced nor deleted.
    r.str(kData, "RAM:Note");
    const u32 fh = r.call(dos_lvo::Open, kData, MODE_OLDFILE);
    CHECK(fh != 0);
    CHECK(!g_dos.write_file("RAM:Note", first, sizeof first, error));
    CHECK_EQ(error, ERROR_OBJECT_IN_USE);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_IN_USE);
    CHECK(!g_dos.delete_file("RAM:Note", error));
    CHECK_EQ(error, ERROR_OBJECT_IN_USE);
    CHECK_EQ(r.call(dos_lvo::Close, fh), DOSTRUE);
    CHECK_EQ(g_dos.examine("RAM:Note").size, 3u);

    // The protection bits count as they do for Open and DeleteFile.
    CHECK_EQ(r.call(dos_lvo::SetProtection, kData, 0x4), DOSTRUE);
    CHECK(!g_dos.write_file("RAM:Note", first, sizeof first, error));
    CHECK_EQ(error, ERROR_WRITE_PROTECTED);
    CHECK_EQ(r.io_err(), ERROR_WRITE_PROTECTED);
    CHECK_EQ(g_dos.examine("RAM:Note").size, 3u);
    CHECK_EQ(r.call(dos_lvo::SetProtection, kData, 0x8), DOSTRUE);
    CHECK(!g_dos.read_file("RAM:Note", buf, sizeof buf, got, error));
    CHECK_EQ(error, ERROR_READ_PROTECTED);
    CHECK_EQ(r.io_err(), ERROR_READ_PROTECTED);
    CHECK_EQ(r.call(dos_lvo::SetProtection, kData, 0x1), DOSTRUE);
    CHECK(!g_dos.delete_file("RAM:Note", error));
    CHECK_EQ(error, ERROR_DELETE_PROTECTED);
    CHECK_EQ(r.io_err(), ERROR_DELETE_PROTECTED);
    CHECK_EQ(r.call(dos_lvo::SetProtection, kData, 0), DOSTRUE);

    // delete_file: files on RAM: only.
    CHECK(!g_dos.delete_file("S/Startup-Sequence", error));
    CHECK_EQ(error, ERROR_DISK_WRITE_PROTECTED);
    CHECK(!g_dos.delete_file("RAM:Nope", error));
    CHECK_EQ(error, ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    CHECK(!g_dos.delete_file("RAM:", error));
    CHECK_EQ(error, ERROR_OBJECT_WRONG_TYPE);
    r.str(kData + 0x40, "RAM:Dir");
    const u32 dir_lock = r.call(dos_lvo::CreateDir, kData + 0x40);
    CHECK(dir_lock != 0);
    r.call(dos_lvo::UnLock, dir_lock);
    CHECK(!g_dos.delete_file("RAM:Dir", error));
    CHECK_EQ(error, ERROR_OBJECT_WRONG_TYPE);
    CHECK(g_dos.examine("RAM:Dir").kind == Dos::TargetKind::Dir);
    CHECK(g_dos.delete_file("RAM:Note", error));
    CHECK_EQ(error, 0u);
    CHECK(g_dos.examine("RAM:Note").kind == Dos::TargetKind::Missing);
}

void paths_belong_to_each_process() {
    Rig r;
    u32 err = 0;
    CHECK(g_dos.path_add("S:", err));
    u32 shell = 0;
    u32 shell_cli = 0;
    CHECK(g_dos.create_shell_process(1, shell, shell_cli));

    // The shell copies the boot shell's path and adds to its own.
    g_exec.set_this_task(shell);
    CHECK_EQ(g_dos.path_count(), 1u);
    char name[128];
    CHECK(g_dos.path_name(0, name, sizeof name));
    CHECK(std::string(name) == "HombreTest:S");
    CHECK(g_dos.path_add("Devs", err));
    CHECK_EQ(g_dos.path_count(), 2u);
    const u32 shell_cli_at = r.get32(shell + pr_CLI) << 2;
    const u32 shell_nodes = r.get32(shell_cli_at + cli_CommandDir);
    const u32 shell_node1 = r.get32(shell_nodes << 2) << 2;
    CHECK(shell_node1 != 0);
    CHECK_EQ(r.get32(shell_node1), 0u);

    g_exec.set_this_task(r.proc());
    CHECK_EQ(g_dos.path_count(), 1u);
    CHECK(g_dos.path_name(0, name, sizeof name));
    CHECK(std::string(name) == "HombreTest:S");
    const u32 boot_nodes = r.get32(r.call(dos_lvo::Cli) + cli_CommandDir);
    CHECK(boot_nodes != 0);
    CHECK(shell_nodes != 0);
    CHECK(boot_nodes != shell_nodes);

    // Resetting one shell leaves the other's path alone.
    g_exec.set_this_task(shell);
    g_dos.path_reset();
    CHECK_EQ(g_dos.path_count(), 0u);
    CHECK_EQ(r.get32(shell_cli_at + cli_CommandDir), 0u);
    g_exec.set_this_task(r.proc());
    CHECK_EQ(g_dos.path_count(), 1u);
    CHECK(g_dos.path_name(0, name, sizeof name));
    CHECK(std::string(name) == "HombreTest:S");
    CHECK_EQ(r.get32(r.call(dos_lvo::Cli) + cli_CommandDir), boot_nodes);
    g_dos.path_reset();
}

}

int main() {
    a_second_drive_is_its_own_volume();
    a_drive_without_a_disk_says_so();
    the_first_disk_wins_a_shared_name();
    layout_matches_the_ndk();
    dos_library_is_built();
    command_writes_to_the_console();
    command_entry_state();
    exit_ends_the_command();
    unknown_vector_ends_the_run();
    commands_that_cannot_run();
    console_input_serves_the_arguments_once();
    console_output_functions();
    select_input_and_output();
    set_mode_on_the_console();
    output_to_bad_handles();
    io_err_and_faults();
    get_var_finds_nothing();
    local_variables();
    variable_arguments_are_checked();
    str_to_long();
    time_and_waiting();
    open_read_seek_close();
    paths_resolve_like_amigados();
    file_table_is_bounded();
    high_byte_pointers_reach_their_24_bit_alias();
    ffs_volumes_read_too();
    locks_and_the_current_directory();
    examine_and_ex_next();
    info_on_the_volume();
    load_seg_and_unload_seg();
    load_seg_of_bad_hunks();
    unload_seg_of_bad_seglists();
    read_args_from_the_command_line();
    read_args_positional_and_keywords();
    read_args_switches_and_toggles();
    read_args_numbers();
    read_args_multiple();
    read_args_rest_of_line();
    read_args_quotes();
    read_args_templates_and_limits();
    free_args_returns_the_memory();
    read_args_survives_any_input();
    add_part_file_part_path_part();
    name_from_lock_and_parent_dir();
    same_lock_and_dup_lock_from_fh();
    fgets_fgetc_ungetc_on_a_file();
    fread_fwrite_counts();
    read_item_and_find_arg();
    alloc_dos_object_fib_and_free();
    exall_lists_a_directory();
    cli_queries();
    find_var_and_delete_var();
    parse_and_match_pattern_lvos();
    match_first_lists_wild_entries();
    match_first_plain_name();
    match_end_releases_anchor();
    match_dodir_enters_directory();
    match_wild_sets_achain_lock();
    match_buf_has_full_path_after_dodir();
    match_end_frees_everything();
    exall_honours_match_string();
    dopkt_disk_info_and_unknown_action();
    write_chars_and_get_file_sys_task();
    date_stamp_uses_the_clock();
    date_to_str_and_str_to_date();
    compare_dates_sign();
    default_assigns_resolve();
    assign_lock_and_remove();
    assign_add_multi_directory();
    assign_late_and_path();
    dos_list_walk();
    get_device_proc_walks_assign_list();
    is_file_system_for_assigns_and_df0();
    change_dir_and_current_dir_name();
    find_command_uses_path();
    path_add_duplicate_and_remove();
    console_read_uses_the_reader_after_the_arguments();
    wait_for_char_uses_ready();
    fault_text_api();
    fail_level_and_return_code();
    leaked_match_anchor_is_reclaimed_after_command();
    leaked_lock_and_file_are_reclaimed();
    current_dir_restored_after_command();
    many_locks_with_assigns_and_path();
    find_command_order();
    name_from_lock_uses_disk_case();
    match_first_sets_d0_on_failure();
    ram_volume_read_operations();
    ram_dos_list_match_and_exall();
    ram_open_write_read_and_seek();
    ram_protection_bits_are_enforced();
    ram_deep_paths_stay_valid();
    ram_buffered_output_and_set_file_size();
    ram_delete_createdir_rename_and_metadata();
    ram_exclusive_locks_and_dopkt();
    ram_enumeration_after_delete_info_and_cleanup();
    t_and_env_assigns_and_envarc_copy();
    global_variables();
    ram_handler_port_and_zero_dates();
    delay_blocks_and_wakes_with_fake_host();
    contexts_keep_their_state_apart();
    background_process_uses_and_leaves_its_shell_output();
    nil_reads_end_of_file_and_swallows_writes();
    create_new_proc_builds_a_process();
    console_handles_write_to_their_console();
    a_process_reads_and_writes_its_console();
    create_shell_process_builds_a_cli();
    run_command_in_runs_commands_in_the_process();
    run_command_keeps_the_callers_stack();
    nested_runs_stop_at_the_limit();
    processes_leave_the_parents_output_open();
    a_passed_handle_belongs_to_the_child();
    stale_handle_bptrs_are_rejected();
    a_shell_console_pair_serves_its_children();
    run_command_in_ends_the_shells_children();
    system_uses_the_shell_process_directory();
    console_star_follows_the_process_console();
    paths_belong_to_each_process();
    file_helpers_follow_the_volume_rules();
    return check::finish("test_dos");
}
