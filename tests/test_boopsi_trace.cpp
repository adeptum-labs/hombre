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

// The BOOPSI trace's lines, run on the host whether or not the kernel is
// built with HOMBRE_BOOPSI_TRACE: guest memory it cannot read never ends a
// run, each task keeps its own depth, and long lists and lines are marked.

#include "check.hpp"
#include "libs/boopsi_trace.hpp"

#include <string>
#include <vector>

using namespace hombre;
namespace bt = hombre::boopsi_trace;

namespace {

constexpr u32 kMemory = 0x10000;
constexpr u32 kClass = 0x1000;
constexpr u32 kSuper = 0x1100;
constexpr u32 kNames = 0x1200;
constexpr u32 kObject = 0x2000 + ndk::Object_SIZEOF;
constexpr u32 kMsg = 0x3000;
constexpr u32 kTags = 0x4000;
constexpr u32 kTaskA = 0x5000;
constexpr u32 kTaskB = 0x5100;

std::vector<std::string> g_lines;

void capture(const char* line) { g_lines.emplace_back(line); }

bool logged(const std::string& text) {
    for (const std::string& line : g_lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct Rig {
    std::vector<u8> memory = std::vector<u8>(kMemory);
    m68k::Machine machine{};
    BoopsiTraceState state;

    Rig() {
        machine.mem = m68k::GuestMemory{memory.data(), kMemory};
        machine.jit = nullptr;
        machine.cpu.exit = m68k::ExitReason::None;
        g_lines.clear();
        g_log = capture;
        str(kNames, "picture.datatype");
        // kClass is a private class over the public kSuper.
        put32(kSuper + ndk::cl_ID, kNames);
        put32(kClass + ndk::cl_Super, kSuper);
        put32(kObject - ndk::Object_SIZEOF + ndk::o_Class, kClass);
    }
    ~Rig() { g_log = nullptr; }

    void put32(u32 at, u32 v) {
        for (u32 i = 0; i < 4; ++i) {
            memory[at + i] = static_cast<u8>(v >> (24 - 8 * i));
        }
    }
    void str(u32 at, const char* s) {
        u32 i = 0;
        for (; s[i] != '\0'; ++i) {
            memory[at + i] = static_cast<u8>(s[i]);
        }
        memory[at + i] = 0;
    }
    bt::Frame call(u32 task, u32 cl, u32 object, u32 msg) {
        return bt::call(state, machine, task, "host", cl, object, msg);
    }
};

void bad_pointers_never_end_the_run() {
    Rig r;
    r.put32(kMsg, ndk::OM_NOTIFY);
    r.put32(kMsg + 4, 0xFFFFFFF0);
    r.call(kTaskA, kClass, kObject, kMsg);
    CHECK(logged("$5000 0 host $1000<picture.datatype obj $200c OM_NOTIFY ginfo $0 flags $0"));
    CHECK(logged("? at $fffffff0"));
    // An odd tag list.
    r.put32(kMsg + 4, kTags + 1);
    r.call(kTaskA, kClass, kObject, kMsg);
    CHECK(logged("? at $4001"));
    // A message whose arguments lie past the end of guest memory.
    r.put32(kMemory - 4, 0x606);
    r.call(kTaskA, kClass, kObject, kMemory - 4);
    CHECK(logged("$606 raw ? ? ?"));
    // A message the dispatcher cannot even read.
    r.call(kTaskA, kClass, kObject, 0xFFFFFFF0);
    CHECK(logged("msg $fffffff0 ?"));
    // A gadget's Hook at the end of memory: no class of the object.
    const u32 hook = kMemory - 20;
    r.put32(kMsg, ndk::GM_RENDER);
    r.put32(kMsg + 4, 0);
    r.call(kTaskB, hook, 0x8000, kMsg);
    CHECK(logged("hook $ffec obj $8000 GM_RENDER $0 $0 $0"));
    CHECK(r.machine.cpu.exit == m68k::ExitReason::None);
}

void each_task_keeps_its_depth() {
    Rig r;
    r.put32(kMsg, ndk::OM_DISPOSE);
    const bt::Frame a0 = r.call(kTaskA, kClass, kObject, kMsg);
    r.call(kTaskA, kClass, kObject, kMsg);
    r.call(kTaskB, kClass, kObject, kMsg);
    CHECK(logged("$5000 1 host"));
    CHECK(logged("$5100 0 host"));
    CHECK_EQ(bt::depth(r.state, kTaskA), 2u);
    bt::ret(r.state, r.machine, a0, 7);
    CHECK(logged("$5000 1 = $7"));
    // A removed task leaves nothing open behind it.
    bt::drop_task(r.state, kTaskA);
    CHECK_EQ(bt::depth(r.state, kTaskA), 0u);
    CHECK_EQ(bt::depth(r.state, kTaskB), 1u);
    bt::reset(r.state);
    CHECK_EQ(bt::depth(r.state, kTaskB), 0u);
}

void om_get_shows_what_it_stored() {
    Rig r;
    r.put32(kMsg, ndk::OM_GET);
    r.put32(kMsg + 4, 0x80001074);
    r.put32(kMsg + 8, 0x3100);
    r.put32(0x3100, 0x1234);
    const bt::Frame f = r.call(kTaskA, kClass, kObject, kMsg);
    CHECK(logged("OM_GET attr $80001074 storage $3100"));
    bt::ret(r.state, r.machine, f, 1);
    CHECK(logged("$5000 0 = $1 stored $1234"));
}

void long_lists_and_lines_are_marked() {
    Rig r;
    r.put32(kMsg, ndk::OM_SET);
    r.put32(kMsg + 4, kTags);
    for (u32 i = 0; i < 20; ++i) {
        r.put32(kTags + i * 8, 0x80030001 + i);
        r.put32(kTags + i * 8 + 4, i);
    }
    r.call(kTaskA, kClass, kObject, kMsg);
    CHECK(logged("$80030010=$f"));
    CHECK(!logged("$80030011=$10"));
    CHECK(logged(" ..."));
    bt::Line line;
    for (u32 i = 0; i < 40; ++i) {
        line.text("word ");
    }
    line.send();
    CHECK(logged("[cut]"));
}

void the_watch_sees_68000_dispatch() {
    Rig r;
    constexpr u32 kEntry = 0x6000;
    r.put32(kClass + ndk::h_Entry, kEntry);
    bt::made_class(r.state, r.machine, kClass);
    CHECK(logged("made class $1000<picture.datatype over picture.datatype inst 0"));
    r.put32(kMsg, ndk::OM_DISPOSE);
    r.machine.cpu.pc = kEntry;
    r.machine.cpu.a[0] = kClass;
    r.machine.cpu.a[2] = kObject;
    r.machine.cpu.a[1] = kMsg;
    bt::watch(r.state, r.machine, kTaskA);
    CHECK(logged("$5000 0 68k $1000<picture.datatype obj $200c OM_DISPOSE"));
    // The dispatch C++ starts itself is logged once, as host.
    g_lines.clear();
    r.state.expected_entry = kEntry;
    bt::watch(r.state, r.machine, kTaskA);
    CHECK(g_lines.empty());
    CHECK_EQ(r.state.expected_entry, 0u);
    bt::freed_class(r.state, kClass);
    bt::watch(r.state, r.machine, kTaskA);
    CHECK(g_lines.empty());
    bt::missing_class(r.machine, kNames);
    CHECK(logged("no class picture.datatype"));
}

}

int main() {
    bad_pointers_never_end_the_run();
    each_task_keeps_its_depth();
    om_get_shows_what_it_stored();
    long_lists_and_lines_are_marked();
    the_watch_sees_68000_dispatch();
    return check::finish("test_boopsi_trace");
}
