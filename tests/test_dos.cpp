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
        : memory_(kGuestBytes), image_(build_adf("HombreTest", ffs, files)) {
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
        CHECK(g_exec.init(m, kFreeStart, kFreeEnd));
        CHECK(adf_mount(image_.data(), image_.size(), volume) == FsError::Ok);
        CHECK(g_dos.init(m, g_exec, volume, console, &g_console));
        m.cpu.a[7] = kStack;
    }

    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_segment_loader(SegmentLoader{nullptr, nullptr});
        g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;
    AdfVolume volume{};

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

    // D1..D4 set, the vector called, D0 returned; the run must halt.
    u32 call(u32 lvo, u32 d1 = 0, u32 d2 = 0, u32 d3 = 0, u32 d4 = 0) {
        m.cpu.d[1] = d1;
        m.cpu.d[2] = d2;
        m.cpu.d[3] = d3;
        m.cpu.d[4] = d4;
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
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

i32 as_signed(u32 v) {
    return static_cast<i32>(v);
}

// --- layout -------------------------------------------------------------

void layout_matches_the_ndk() {
    CHECK_EQ(dos_lvo::Open, 30u);
    CHECK_EQ(dos_lvo::Close, 36u);
    CHECK_EQ(dos_lvo::Read, 42u);
    CHECK_EQ(dos_lvo::Write, 48u);
    CHECK_EQ(dos_lvo::Input, 54u);
    CHECK_EQ(dos_lvo::Output, 60u);
    CHECK_EQ(dos_lvo::Seek, 66u);
    CHECK_EQ(dos_lvo::Lock, 84u);
    CHECK_EQ(dos_lvo::UnLock, 90u);
    CHECK_EQ(dos_lvo::DupLock, 96u);
    CHECK_EQ(dos_lvo::Examine, 102u);
    CHECK_EQ(dos_lvo::ExNext, 108u);
    CHECK_EQ(dos_lvo::Info, 114u);
    CHECK_EQ(dos_lvo::CurrentDir, 126u);
    CHECK_EQ(dos_lvo::IoErr, 132u);
    CHECK_EQ(dos_lvo::Exit, 144u);
    CHECK_EQ(dos_lvo::LoadSeg, 150u);
    CHECK_EQ(dos_lvo::UnLoadSeg, 156u);
    CHECK_EQ(dos_lvo::DateStamp, 192u);
    CHECK_EQ(dos_lvo::Delay, 198u);
    CHECK_EQ(dos_lvo::WaitForChar, 204u);
    CHECK_EQ(dos_lvo::SelectInput, 294u);
    CHECK_EQ(dos_lvo::SelectOutput, 300u);
    CHECK_EQ(dos_lvo::FPutC, 312u);
    CHECK_EQ(dos_lvo::FPuts, 342u);
    CHECK_EQ(dos_lvo::VFPrintf, 354u);
    CHECK_EQ(dos_lvo::Flush, 360u);
    CHECK_EQ(dos_lvo::SetMode, 426u);
    CHECK_EQ(dos_lvo::SetIoErr, 462u);
    CHECK_EQ(dos_lvo::Fault, 468u);
    CHECK_EQ(dos_lvo::PrintFault, 474u);
    CHECK_EQ(dos_lvo::Cli, 492u);
    CHECK_EQ(dos_lvo::NewLoadSeg, 768u);
    CHECK_EQ(dos_lvo::ReadArgs, 798u);
    CHECK_EQ(dos_lvo::StrToLong, 816u);
    CHECK_EQ(dos_lvo::FreeArgs, 858u);
    CHECK_EQ(dos_lvo::SetVar, 900u);
    CHECK_EQ(dos_lvo::GetVar, 906u);
    CHECK_EQ(dos_lvo::PutStr, 948u);
    CHECK_EQ(dos_lvo::VPrintf, 954u);
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
    CHECK_EQ(dol_Name, dol_VolumeDate + 6 * 4);
    CHECK_EQ(DosList_SIZEOF, dol_Name + 4);

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
    CHECK_EQ(r.get32(vol + dol_Next), 0u);
    CHECK(r.read_bstr(r.get32(vol + dol_Name)) == "HombreTest");
    CHECK_EQ(r.get32(vol + dol_DiskType), ID_DOS_DISK);
    const u32 tasks = r.get32(root + rn_TaskArray) << 2;
    CHECK_EQ(r.get32(tasks), 1u);
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
    CHECK_EQ(r.dos(72), ExitReason::Unimplemented);  // DeleteFile
    CHECK(logged("dos: unimplemented LVO -72"));
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

    // There are no global variables.
    CHECK_EQ(get_var(r, "Planet", 32, GVF_GLOBAL_ONLY), -1);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.get8(kData + 0x100), 0u);
    CHECK_EQ(set_var(r, "Moon", "Io", GVF_GLOBAL_ONLY), DOSFALSE);
    CHECK_EQ(r.io_err(), ERROR_DEVICE_NOT_MOUNTED);
    CHECK_EQ(get_var(r, "Moon", 32), -1);

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
    const u32 at = fh << 2;
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
    CHECK_EQ(again, handles[3]);

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
        r.put32((handles[0] << 2) + i, 0xDEADBEEF);
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
    CHECK(logged("dos: lock table full, 32 entries in use"));
    g_lines.clear();
    CHECK_EQ(r.call(dos_lvo::DupLock, held), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_FREE_STORE);
    CHECK(logged("dos: lock table full, 32 entries in use"));
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

}

int main() {
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
    return check::finish("test_dos");
}
