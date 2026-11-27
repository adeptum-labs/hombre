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

#include "libs/exec.hpp"
#include "libs/graphics.hpp"
#include "libs/intuition.hpp"
#include "check.hpp"
#include "test_guest.hpp"

#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

// A machine, translator, exec and graphics.library over 16 MiB of guest
// memory, with the display sink recording every refresh. The log is
// captured in g_lines.

namespace gfx_test {

using hombre::DisplaySink;
using hombre::DisplayState;
using hombre::Exec;
using hombre::Graphics;
using hombre::Intuition;
using hombre::Rect;
using hombre::u16;
using hombre::u32;
using hombre::u8;
using hombre::usize;
using hombre::i32;
using hombre::m68k::CodeRegion;
using hombre::m68k::CpuState;
using hombre::m68k::ExitReason;
using hombre::m68k::GuestMemory;
using hombre::m68k::Jit;
using hombre::m68k::Machine;
using hombre::native::Bus;

constexpr u32 kCall = 0x1800;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;  // test structures up to 0xFFFF
constexpr u32 kFreeStart = 0x10000;

enum class Base { Exec, Graphics, Intuition };

inline std::vector<std::string> g_lines;

inline void capture(const char* line) { g_lines.push_back(line); }

inline bool logged(const std::string& text) {
    for (const std::string& line : g_lines) {
        if (line == text) {
            return true;
        }
    }
    return false;
}

class GfxRig {
public:
    GfxRig() : memory_(kTestGuestBytes) {
        fd_ = memfd_create("graphics", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        machine.cpu = CpuState{};
        machine.mem = GuestMemory{memory_.data(), kTestGuestBytes};
        machine.jit = nullptr;
        CHECK(jit.init(CodeRegion{rw_, rx_, kRegion}, machine));
        g_lines.clear();
        hombre::g_log = capture;
        CHECK(exec.init(machine, kFreeStart, kTestChipEnd, kTestGuestBytes));
        display.sink = DisplaySink{record, this};
        display.front_screen = 0;
        CHECK(graphics.init(machine, exec, display));
        CHECK(intuition.init(machine, exec, graphics, display));
        machine.cpu.a[7] = kStack;
    }

    ~GfxRig() {
        hombre::g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    GfxRig(const GfxRig&) = delete;
    GfxRig& operator=(const GfxRig&) = delete;

    Machine machine;
    Jit jit;
    Exec exec;
    Graphics graphics;
    Intuition intuition;
    DisplayState display;
    std::vector<Rect> refreshes;
    std::vector<u32> refreshed_screens;

    CpuState& cpu() { return machine.cpu; }

    u32 base(Base which) const {
        switch (which) {
        case Base::Exec:
            return exec.exec_base();
        case Base::Graphics:
            return graphics.graphics_base();
        case Base::Intuition:
            return intuition.intuition_base();
        }
        return 0;
    }

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

    // Tag items (tag, data) from at on.
    u32 tags(u32 at, std::initializer_list<std::pair<u32, u32>> items) {
        u32 p = at;
        for (const auto& [tag, data] : items) {
            put32(p, tag);
            put32(p + 4, data);
            p += 8;
        }
        return at;
    }

    // JSR -lvo(A6) with A6 = the library base, from hand-written 68000 code.
    ExitReason call(Base which, u32 lvo) { return call_at(base(which), lvo); }

    // The same for a library the rig itself does not hold.
    ExitReason call_at(u32 library, u32 lvo) {
        put16(kCall, 0x4EAE);
        put16(kCall + 2, disp(lvo));
        put16(kCall + 4, 0x4EF8);  // JMP $400.w
        put16(kCall + 6, 0x0400);
        jit.flush();
        machine.cpu.a[6] = library;
        machine.cpu.a[7] = kStack;
        machine.cpu.pc = kCall;
        return jit.run(1000000);
    }

    // Calls the vector, which must return; D0 is what it gave.
    u32 result(Base which, u32 lvo) {
        CHECK_EQ(call(which, lvo), ExitReason::Halt);
        return machine.cpu.d[0];
    }

private:
    static constexpr usize kRegion = 1 << 20;

    static void record(void* ctx, Machine&, u32 screen, const Rect& dirty) {
        GfxRig* rig = static_cast<GfxRig*>(ctx);
        rig->refreshes.push_back(dirty);
        rig->refreshed_screens.push_back(screen);
    }

    static u16 disp(u32 offset) { return static_cast<u16>(0x10000 - offset); }

    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

}
