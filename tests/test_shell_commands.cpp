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
#include "libs/dos.hpp"
#include "libs/timer_device.hpp"
#include "ofs_writer.hpp"
#include "shell_commands.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;

namespace {

constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;

Jit g_jit;
Exec g_exec;
TimerDevice g_timer;
Dos g_dos;

void print_sink(void* ctx, const char* text) {
    *static_cast<std::string*>(ctx) += text;
}

// HUNK_HEADER with one HUNK_CODE holding RTS.
std::vector<u8> rts_hunk() {
    return {
        0x00, 0x00, 0x03, 0xF3,  // HUNK_HEADER
        0x00, 0x00, 0x00, 0x00,  // no names
        0x00, 0x00, 0x00, 0x01,  // one hunk
        0x00, 0x00, 0x00, 0x00,  // first hunk 0
        0x00, 0x00, 0x00, 0x00,  // last hunk 0
        0x00, 0x00, 0x00, 0x01,  // hunk 0 size in longs
        0x00, 0x00, 0x03, 0xE9,  // HUNK_CODE
        0x00, 0x00, 0x00, 0x01,  // one long
        0x00, 0x00, 0x4E, 0x75,  // RTS
        0x00, 0x00, 0x03, 0xF2,  // HUNK_END
    };
}

class Rig {
public:
    Rig()
        : memory_(kGuestBytes),
          image_(build_adf("HombreTest", false,
                           {
                               {"C/Echo", {'x'}},
                               {"C/RunMe", rts_hunk()},
                               {"S/Startup-Sequence", {'e', 'c', 'h', 'o', '\n'}},
                               {"Devs/Sub/Deep", {'d'}},
                           })) {
        fd_ = memfd_create("shell_commands", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        m_.cpu = CpuState{};
        m_.mem = GuestMemory{memory_.data(), kGuestBytes};
        m_.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, kRegion}, m_));
        CHECK(g_exec.init(m_, kFreeStart, kFreeEnd));
        CHECK(g_timer.init(m_, g_exec));
        CHECK(adf_mount(image_.data(), image_.size(), volume_) == FsError::Ok);
        CHECK(g_dos.init(m_, g_exec, volume_, nullptr, nullptr));
    }

    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_segment_loader(SegmentLoader{nullptr, nullptr});
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine& machine() { return m_; }

private:
    static constexpr usize kRegion = 1 << 20;
    std::vector<u8> memory_;
    std::vector<u8> image_;
    Machine m_{};
    AdfVolume volume_{};
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

struct FakeReader {
    std::vector<std::string> lines;
    usize index = 0;
};

u32 read_fake_line(void* ctx, u8* buf, u32 size) {
    FakeReader& r = *static_cast<FakeReader*>(ctx);
    if (r.index >= r.lines.size() || size == 0) {
        return 0;
    }
    const std::string& line = r.lines[r.index++];
    const u32 n = static_cast<u32>(std::min<usize>(line.size(), size));
    std::memcpy(buf, line.data(), n);
    return n;
}

void cd_prints_and_changes() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("cd", ""));
    CHECK(out == "HombreTest:\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    out.clear();
    CHECK(cmds.run("cd", "c"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);

    CHECK(cmds.run("CD", ""));
    CHECK(out == "HombreTest:C\n");

    out.clear();
    CHECK(cmds.run("cd", "/"));
    CHECK(cmds.run("cd", ""));
    CHECK(out == "HombreTest:\n");

    out.clear();
    CHECK(cmds.run("Cd", "Devs/Sub"));
    CHECK(cmds.run("cd", ""));
    CHECK(out == "HombreTest:Devs/Sub\n");

    out.clear();
    CHECK(cmds.run("cd", ":"));
    CHECK(cmds.run("cd", ""));
    CHECK(out == "HombreTest:\n");
}

void cd_errors() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("cd", "Missing"));
    CHECK(out == "Can't find Missing\n");
    CHECK_EQ(g_dos.last_return_code(), 20u);
    CHECK_EQ(g_dos.last_error(), ERROR_OBJECT_NOT_FOUND);

    out.clear();
    CHECK(cmds.run("cd", "C:Echo"));
    CHECK(out == "C:Echo is not a directory\n");
    CHECK_EQ(g_dos.last_return_code(), 20u);
    CHECK_EQ(g_dos.last_error(), ERROR_OBJECT_WRONG_TYPE);

    CHECK(!cmds.run("version", ""));
}

void echo_noline_and_escapes() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("echo", ""));
    CHECK(out == "\n");

    out.clear();
    CHECK(cmds.run("ECHO", "hello world"));
    CHECK(out == "hello world\n");

    out.clear();
    CHECK(cmds.run("echo", "\"hello world\""));
    CHECK(out == "hello world\n");

    out.clear();
    CHECK(cmds.run("echo", "\"a*Nb**c*\"d\""));
    CHECK(out == "a\nb*c\"d\n");

    out.clear();
    CHECK(cmds.run("echo", "hello NOLINE"));
    CHECK(out == "hello");

    out.clear();
    CHECK(cmds.run("echo", "\"hello\" noline"));
    CHECK(out == "hello");

    out.clear();
    CHECK(cmds.run("echo", "\"hello NOLINE\""));
    CHECK(out == "hello NOLINE\n");

    out.clear();
    CHECK(cmds.run("echo", "NOLINE"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
}

void echo_star_outside_quotes_is_literal() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("echo", "a*nb"));
    CHECK(out == "a*nb\n");

    out.clear();
    CHECK(cmds.run("echo", "a*eb **"));
    CHECK(out == "a*eb **\n");

    out.clear();
    CHECK(cmds.run("echo", "a*nb \"c*nd\" e**f NOLINE"));
    CHECK(out == "a*nb c\nd e**f");
}

void path_show_add_reset() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("path", ""));
    CHECK(out == "Current_directory\nC:\n");

    out.clear();
    CHECK(cmds.run("PATH", "SHOW"));
    CHECK(out == "Current_directory\nC:\n");

    out.clear();
    CHECK(cmds.run("path", "ADD S:"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);

    CHECK(cmds.run("path", "Devs/Sub"));
    CHECK(cmds.run("path", ""));
    CHECK(out == "Current_directory\nHombreTest:S\nHombreTest:Devs/Sub\nC:\n");

    out.clear();
    CHECK(cmds.run("path", "NoSuchDir"));
    CHECK(out == "NoSuchDir not found\n");
    CHECK_EQ(g_dos.last_return_code(), 10u);

    out.clear();
    CHECK(cmds.run("path", "RESET"));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK(cmds.run("path", ""));
    CHECK(out == "Current_directory\nC:\n");
}

void path_keywords_not_directories() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("path", "ADD"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 0u);

    CHECK(cmds.run("path", "HEAD"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 0u);

    CHECK(cmds.run("path", "QUIET"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 0u);

    CHECK(cmds.run("path", "QUIET NoSuchDir"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 10u);

    CHECK(cmds.run("path", "NoSuchDir QUIET"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 10u);

    // S: is not in the path yet.
    CHECK(cmds.run("path", "REMOVE S:"));
    CHECK(out == "S: not found\n");
    CHECK_EQ(g_dos.last_return_code(), 10u);
    CHECK_EQ(g_dos.path_count(), 0u);

    out.clear();
    CHECK(cmds.run("path", "QUIET REMOVE S:"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 10u);
    CHECK_EQ(g_dos.path_count(), 0u);

    CHECK(cmds.run("path", "HEAD S: SHOW"));
    CHECK(out == "Current_directory\nHombreTest:S\nC:\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 1u);

    out.clear();
    CHECK(cmds.run("path", "SHOW Devs/Sub"));
    CHECK(out == "Current_directory\nHombreTest:S\nHombreTest:Devs/Sub\nC:\n");

    out.clear();
    CHECK(cmds.run("path", "REMOVE S: SHOW"));
    CHECK(out == "Current_directory\nHombreTest:Devs/Sub\nC:\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 1u);

    out.clear();
    CHECK(cmds.run("path", "RESET"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 0u);
}

void path_add_duplicate_once() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("path", "ADD S:"));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 1u);

    CHECK(cmds.run("path", "S:"));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 1u);

    CHECK(cmds.run("path", "HombreTest:s"));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 1u);

    CHECK(cmds.run("path", "Devs/Sub devs/sub"));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.path_count(), 2u);

    CHECK(cmds.run("path", "SHOW"));
    CHECK(out == "Current_directory\nHombreTest:S\nHombreTest:Devs/Sub\nC:\n");
}

void why_messages() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    g_dos.set_return_code(0);
    g_dos.set_last_error(0);
    CHECK(cmds.run("why", ""));
    CHECK(out == "The last command did not set a return code\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    out.clear();
    g_dos.set_return_code(20);
    g_dos.set_last_error(0);
    CHECK(cmds.run("WHY", ""));
    CHECK(out == "The last command did not set a reason for failure\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    out.clear();
    g_dos.set_return_code(20);
    g_dos.set_last_error(ERROR_OBJECT_NOT_FOUND);
    CHECK(cmds.run("why", ""));
    CHECK(out == "Last command failed because no such file or directory\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);
}

void prompt_format() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    char buf[128];
    format_prompt(g_dos, 1, buf, sizeof buf);
    CHECK(std::string(buf) == "1.HombreTest:> ");

    CHECK(cmds.run("cd", "c"));
    format_prompt(g_dos, 1, buf, sizeof buf);
    CHECK(std::string(buf) == "1.HombreTest:C> ");

    CHECK(cmds.run("prompt", "\"[%N:%S]%% %X \""));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    format_prompt(g_dos, 4, buf, sizeof buf);
    CHECK(std::string(buf) == "[4:HombreTest:C]% %X ");

    CHECK(cmds.run("PROMPT", ""));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    format_prompt(g_dos, 1, buf, sizeof buf);
    CHECK(std::string(buf) == "1.HombreTest:C> ");
}

void ask_yes_and_no() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    FakeReader reader;
    reader.lines = {"y\n", "YES\n", "n\n", "no\n", "\n"};
    g_dos.set_console_input(read_fake_line, nullptr, &reader);

    CHECK(cmds.run("ask", "\"Continue?\""));
    CHECK(out == "Continue? ");
    CHECK_EQ(g_dos.last_return_code(), 5u);

    out.clear();
    CHECK(cmds.run("ASK", "\"Sure?\""));
    CHECK(out == "Sure? ");
    CHECK_EQ(g_dos.last_return_code(), 5u);

    for (int i = 0; i < 4; ++i) {
        out.clear();
        CHECK(cmds.run("ask", "\"Go?\""));
        CHECK(out == "Go? ");
        CHECK_EQ(g_dos.last_return_code(), 0u);
    }
}

void failat() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("failat", ""));
    CHECK(out == "Fail limit: 10\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    out.clear();
    CHECK(cmds.run("FAILAT", "20"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.fail_level(), 20u);

    CHECK(cmds.run("failat", ""));
    CHECK(out == "Fail limit: 20\n");

    out.clear();
    CHECK(cmds.run("failat", "bad"));
    CHECK_EQ(g_dos.last_return_code(), 20u);
    CHECK_EQ(g_dos.last_error(), ERROR_BAD_NUMBER);
    CHECK_EQ(g_dos.fail_level(), 20u);
}

void failat_minimum_is_one() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("failat", "0"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK_EQ(g_dos.fail_level(), 1u);

    CHECK(cmds.run("failat", ""));
    CHECK(out == "Fail limit: 1\n");
}

void env_variable_builtins() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("setenv", "Greeting \"hello world\""));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);

    CHECK(cmds.run("GETENV", "greeting"));
    CHECK(out == "hello world\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    // Readable through ENV:<name> via dos Open/Read.
    u32 node = RamFs::kNone;
    CHECK_EQ(g_dos.ram().find_path("Env/Greeting", node), 0u);
    std::vector<u8> buf(g_dos.ram().node(node).size);
    u32 got = 0;
    CHECK_EQ(g_dos.ram().read(node, 0, buf.data(), static_cast<u32>(buf.size()), got), 0u);
    CHECK(std::string(reinterpret_cast<const char*>(buf.data()), got) == "hello world");

    // Empty value when STRING is omitted; listing prints top-level ENV: names.
    out.clear();
    CHECK(cmds.run("setenv", "EmptyVar"));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK(cmds.run("getenv", "EmptyVar"));
    CHECK(out == "\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    out.clear();
    CHECK(cmds.run("setenv", ""));
    CHECK(out == "Greeting\nEmptyVar\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    // Missing variable: no output, rc 5, IoErr ERROR_OBJECT_NOT_FOUND so WHY explains it.
    out.clear();
    CHECK(cmds.run("getenv", "NoSuchVar"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 5u);
    CHECK_EQ(g_dos.last_error(), ERROR_OBJECT_NOT_FOUND);
    CHECK(cmds.run("why", ""));
    CHECK(out == "Last command failed because no such file or directory\n");

    // UNSETENV removes the variable and ignores missing ones silently.
    out.clear();
    CHECK(cmds.run("unsetenv", "Greeting"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK(cmds.run("getenv", "Greeting"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 5u);

    CHECK(cmds.run("unsetenv", "Greeting"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);
}

void env_variable_readable_via_dos_open() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("setenv", "Editor \"C:Ed *\"fast*\"\""));
    CHECK_EQ(g_dos.last_return_code(), 0u);

    // Call dos.library Open/Read on ENV:Editor from m68k code via Rig's Machine.
    Machine& m = rig.machine();
    const u32 block = g_exec.alloc(0x1000, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(block != 0);
    const u32 name_addr = block;
    const u32 buf_addr = block + 0x100;
    const u32 code_addr = block + 0x200;
    const u32 sp = block + 0xF00;
    const char env_name[] = "ENV:Editor";
    for (u32 i = 0; i < sizeof env_name; ++i) {
        m.mem.write8(name_addr + i, static_cast<u8>(env_name[i]));
    }

    // Minimal 68k snippet: Open("ENV:Editor", MODE_OLDFILE), Read(fh, buf, 32), Close(fh), RTS.
    std::vector<u16> opcodes;
    auto emit16 = [&](u16 w) { opcodes.push_back(w); };
    auto emit32 = [&](u32 v) {
        emit16(static_cast<u16>(v >> 16));
        emit16(static_cast<u16>(v));
    };
    // move.l g_dos.dos_base(), a6
    emit16(0x2C7C);
    emit32(g_dos.dos_base());
    // move.l #name_addr, d1; move.l #MODE_OLDFILE, d2; jsr -30(a6) (Open)
    emit16(0x223C);
    emit32(name_addr);
    emit16(0x243C);
    emit32(MODE_OLDFILE);
    emit16(0x4EAE);
    emit16(static_cast<u16>(-static_cast<i32>(dos_lvo::Open)));
    // move.l d0, d4 (save fh); move.l d0, d1; move.l #buf_addr, d2; moveq #32, d3; jsr -42(a6) (Read)
    emit16(0x2800);
    emit16(0x2200);
    emit16(0x243C);
    emit32(buf_addr);
    emit16(0x7620);
    emit16(0x4EAE);
    emit16(static_cast<u16>(-static_cast<i32>(dos_lvo::Read)));
    // move.l d0, d5 (save count); move.l d4, d1; jsr -36(a6) (Close)
    emit16(0x2A00);
    emit16(0x2204);
    emit16(0x4EAE);
    emit16(static_cast<u16>(-static_cast<i32>(dos_lvo::Close)));
    // move.l d5, d0; rts
    emit16(0x2005);
    emit16(0x4E75);

    for (usize i = 0; i < opcodes.size(); ++i) {
        m.mem.write16(code_addr + static_cast<u32>(i * 2), opcodes[i]);
    }
    m.cpu.a[7] = sp;
    g_jit.flush();
    CHECK(g_jit.call_guest(code_addr, 10000) == ExitReason::Returned);
    CHECK_EQ(m.cpu.d[0], 11u);
    std::string read_back;
    for (u32 i = 0; i < 11; ++i) {
        u32 ch = 0;
        CHECK(m.mem.read8(buf_addr + i, ch));
        read_back.push_back(static_cast<char>(ch));
    }
    CHECK(read_back == "C:Ed \"fast\"");
    g_exec.free(block, 0x1000);
}

void local_variable_builtins() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("setenv", "OnlyGlobal gval"));
    CHECK(cmds.run("get", "OnlyGlobal"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 5u);
    CHECK_EQ(g_dos.last_error(), ERROR_OBJECT_NOT_FOUND);

    CHECK(cmds.run("set", "Foo \"bar baz\""));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK(cmds.run("set", "Empty"));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK(cmds.run("set", "Multi \"first*Nsecond\""));
    CHECK_EQ(g_dos.last_return_code(), 0u);

    CHECK(cmds.run("get", "foo"));
    CHECK(out == "bar baz\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    out.clear();
    CHECK(cmds.run("get", "Empty"));
    CHECK(out == "\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    out.clear();
    CHECK(cmds.run("SET", ""));
    CHECK(out == "Foo bar baz\nEmpty \nMulti first\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);

    out.clear();
    CHECK(cmds.run("unset", "Foo"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);

    CHECK(cmds.run("get", "Foo"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 5u);

    CHECK(cmds.run("unset", "Foo"));
    CHECK(out.empty());
    CHECK_EQ(g_dos.last_return_code(), 0u);

    // Unsetting local does not remove global OnlyGlobal.
    CHECK(cmds.run("unset", "OnlyGlobal"));
    CHECK_EQ(g_dos.last_return_code(), 0u);
    CHECK(cmds.run("getenv", "OnlyGlobal"));
    CHECK(out == "gval\n");
}

void run_starts_a_background_cli() {
    Rig rig;
    std::string out;
    ShellCommands cmds(g_dos, print_sink, &out);

    CHECK(cmds.run("run", ""));
    CHECK(out == "Run: no command\n");
    CHECK_EQ(g_dos.last_return_code(), 10u);

    out.clear();
    CHECK(cmds.run("run", "nosuch"));
    CHECK(out == "Unknown command nosuch\n");
    CHECK_EQ(g_dos.last_return_code(), 10u);

    out.clear();
    CHECK(cmds.run("run", "echo hi"));
    CHECK(out == "Unknown command echo\n");
    CHECK_EQ(g_dos.last_return_code(), 10u);

    out.clear();
    CHECK(cmds.run("run", "RunMe one"));
    CHECK(out == "[CLI 2]\n");
    CHECK_EQ(g_dos.last_return_code(), 0u);
}

}

int main() {
    cd_prints_and_changes();
    cd_errors();
    echo_noline_and_escapes();
    echo_star_outside_quotes_is_literal();
    path_show_add_reset();
    path_keywords_not_directories();
    path_add_duplicate_once();
    why_messages();
    prompt_format();
    ask_yes_and_no();
    failat();
    failat_minimum_is_one();
    env_variable_builtins();
    env_variable_readable_via_dos_open();
    local_variable_builtins();
    run_starts_a_background_cli();
    return check::finish("test_shell_commands");
}
