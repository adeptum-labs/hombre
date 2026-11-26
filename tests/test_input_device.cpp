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

#include "libs/input_device.hpp"
#include "check.hpp"
#include "test_guest.hpp"

#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;

namespace {

constexpr u32 kCode = 0x1000;
constexpr u32 kCall = 0x1100;
constexpr u32 kSegment = 0x1800;  // the program's code, up to 0x2000
constexpr u32 kHandlers = 0x3000;  // handler code no program holds
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;
constexpr u32 kNames = 0x9800;
constexpr u32 kFreeStart = 0x10000;
constexpr usize kRegion = 1 << 20;

// The handlers, as m68k-linux-gnu-as assembles them. Each is called with
// the events in A0 and is_Data in A1 and returns the events in D0.
//
// changer: every RAWKEY event with code $32 (X) gets code $15 (Y).
const std::initializer_list<u16> kChanger = {
    0x2008, 0x2248, 0x2209, 0x671A, 0x0C29, 0x0001, 0x0004, 0x660E, 0x0C69, 0x0032,
    0x0006, 0x6606, 0x337C, 0x0015, 0x0006, 0x2251, 0x60E2, 0x4E75};
// swallower: every RAWKEY event with code $10 (Q) is taken out of the list.
const std::initializer_list<u16> kSwallower = {
    0x2F08, 0x224F, 0x2051, 0x2008, 0x6718, 0x0C28, 0x0001, 0x0004, 0x660C, 0x0C68,
    0x0010, 0x0006, 0x6604, 0x2290, 0x60E6, 0x2248, 0x60E2, 0x201F, 0x4E75};
// counter: adds 1 to the long at is_Data and passes the events on.
const std::initializer_list<u16> kCounter = {0x5291, 0x2008, 0x4E75};
// crasher: an illegal instruction.
const std::initializer_list<u16> kCrasher = {0x4AFC};
// forbidder: Forbid() with A6 = ExecBase, and returns with it held.
const std::initializer_list<u16> kForbidder = {0x4EAE, 0xFF7C, 0x2008, 0x4E75};

Jit g_jit;
Exec g_exec;
InputDevice g_input;
std::vector<std::string> g_lines;
std::vector<InputEventData> g_sunk;

void capture(const char* line) {
    g_lines.push_back(line);
}

bool logged_containing(const std::string& text) {
    for (const std::string& line : g_lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

u32 logged_count(const std::string& text) {
    u32 count = 0;
    for (const std::string& line : g_lines) {
        if (line.find(text) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

void sink(void*, const InputEventData& e) {
    g_sunk.push_back(e);
}

struct PassWatch {
    bool in_pass = false;
    bool had_pending = false;
};

void watch_pass(void* ctx, const InputEventData&) {
    PassWatch& watch = *static_cast<PassWatch*>(ctx);
    watch.in_pass = g_input.in_pass();
    watch.had_pending = g_input.has_pending();
}

struct Ending {
    u32 program = 0;
    bool done = false;
};

void end_program_sink(void* ctx, const InputEventData& e) {
    g_sunk.push_back(e);
    Ending& ending = *static_cast<Ending*>(ctx);
    if (!ending.done) {
        ending.done = true;
        g_input.program_ended(ending.program);
    }
}

bool fake_create(void*, u32) { return true; }
void fake_destroy(void*, u32) {}
bool fake_slice_over(void*) { return false; }
void fake_poll(void*) {}
bool fake_idle(void*) { return false; }
u64 fake_now_us(void* ctx) { return *static_cast<u64*>(ctx); }

InputEventData key(u16 code, u16 qualifier = 0) {
    InputEventData e{};
    e.ie_class = IECLASS_RAWKEY;
    e.code = code;
    e.qualifier = qualifier;
    return e;
}

InputEventData move(i16 x, i16 y) {
    InputEventData e{};
    e.ie_class = IECLASS_RAWMOUSE;
    e.code = IECODE_NOBUTTON;
    e.qualifier = IEQUALIFIER_RELATIVEMOUSE;
    e.x = x;
    e.y = y;
    return e;
}

// Guest memory, the Jit, exec on a host whose tasks never switch, and an
// open input.device with its sink captured. The test runs as the boot task.
class Rig {
public:
    Rig() : memory_(kTestGuestBytes) {
        fd_ = memfd_create("input_device", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        m.cpu = CpuState{};
        m.mem = GuestMemory{memory_.data(), kTestGuestBytes};
        m.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, kRegion}, m));
        g_lines.clear();
        g_sunk.clear();
        g_log = capture;
        g_exec.set_task_host(TaskHost{fake_create, nullptr, fake_destroy, fake_slice_over,
                                      fake_poll, fake_idle, fake_now_us, &now_us});
        CHECK(g_exec.init(m, kFreeStart, kTestChipEnd, kTestGuestBytes));
        CHECK(g_input.init(m, g_exec));
        g_input.set_sink(sink, nullptr);
        m.cpu.a[7] = kStack;
        for (u32 i = 0; i < TC_SIZE; ++i) {
            put8(kData + i, 0);
        }
        put8(kData + LN_TYPE, NT_TASK);
        put32(kData + TC_SIGALLOC, 0x0000FFFF);
        g_exec.set_this_task(kData);
        open_device();
    }

    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_task_host(TaskHost{});
        g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;
    u64 now_us = 5'250'000;
    u32 request = 0;

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

    u32 code(u32 at, std::initializer_list<u16> words) {
        for (u16 w : words) {
            put16(at, w);
            at += 2;
        }
        g_jit.flush();
        return at;
    }

    u32 str(u32 at, const char* s) {
        for (usize i = 0; i <= std::strlen(s); ++i) {
            put8(at + static_cast<u32>(i), static_cast<u8>(s[i]));
        }
        return at;
    }

    // JSR -offset(A6) with A6 = lib, then the Halt stub.
    ExitReason call(u32 lib, u32 offset) {
        code(kCall, {0x4EAE, static_cast<u16>(0x10000 - offset), 0x4EF8, 0x0400});
        m.cpu.a[6] = lib;
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCall;
        return g_jit.run(1000000);
    }

    ExitReason exec(u32 offset) { return call(g_exec.exec_base(), offset); }

    // An Interrupt at at for the handler at code, with its name.
    u32 interrupt(u32 at, i8 pri, u32 handler, u32 data, const char* name, u32 name_at) {
        for (u32 i = 0; i < IS_SIZE; ++i) {
            put8(at + i, 0);
        }
        put8(at + LN_TYPE, 2);  // NT_INTERRUPT
        put8(at + LN_PRI, static_cast<u8>(pri));
        put32(at + LN_NAME, str(name_at, name));
        put32(at + IS_DATA, data);
        put32(at + IS_CODE, handler);
        return at;
    }

    // DoIO of the request with the command and io_Data; returns io_Error.
    i8 do_io(u16 command, u32 data) {
        put16(request + io_Command, command);
        put32(request + io_Data, data);
        m.cpu.a[1] = request;
        CHECK_EQ(exec(exec_lvo::DoIO), ExitReason::Halt);
        return static_cast<i8>(get8(request + io_Error));
    }

private:
    void open_device() {
        CHECK_EQ(exec(exec_lvo::CreateMsgPort), ExitReason::Halt);
        const u32 port = m.cpu.d[0];
        CHECK(port != 0);
        m.cpu.a[0] = port;
        m.cpu.d[0] = IOSTD_SIZE;
        CHECK_EQ(exec(exec_lvo::CreateIORequest), ExitReason::Halt);
        request = m.cpu.d[0];
        CHECK(request != 0);
        m.cpu.a[0] = str(kNames, "input.device");
        m.cpu.d[0] = 0;
        m.cpu.a[1] = request;
        m.cpu.d[1] = 0;
        CHECK_EQ(exec(exec_lvo::OpenDevice), ExitReason::Halt);
        CHECK_EQ(m.cpu.d[0], 0u);
        CHECK_EQ(get32(request + io_Device), g_input.device_base());
    }

    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

void with_no_handler_events_go_straight_to_the_sink() {
    Rig r;
    CHECK(!g_input.chain_active());
    g_input.submit(key(0x20, 0x0001));
    g_input.submit(move(3, -2));
    // Nothing waits for a task, and there is none.
    CHECK_EQ(g_sunk.size(), 2u);
    CHECK_EQ(g_sunk[0].ie_class, IECLASS_RAWKEY);
    CHECK_EQ(g_sunk[0].code, 0x20u);
    CHECK_EQ(g_sunk[0].qualifier, 0x0001u);
    CHECK_EQ(g_sunk[1].ie_class, IECLASS_RAWMOUSE);
    CHECK_EQ(g_sunk[1].x, 3);
    CHECK_EQ(g_sunk[1].y, -2);
    CHECK_EQ(g_input.task(), 0u);
    CHECK(!g_input.has_pending());

    // IND_WRITEEVENT goes the same way and stamps the event.
    constexpr u32 event = kData + 0x200;
    for (u32 i = 0; i < ie_SIZEOF; ++i) {
        r.put8(event + i, 0);
    }
    r.put8(event + ie_Class, IECLASS_RAWKEY);
    r.put16(event + ie_Code, 0x45);
    r.put16(event + ie_Qualifier, 0x0008);
    CHECK_EQ(r.do_io(IND_WRITEEVENT, event), 0);
    CHECK_EQ(g_sunk.size(), 3u);
    CHECK_EQ(g_sunk[2].code, 0x45u);
    CHECK_EQ(g_sunk[2].qualifier, 0x0008u);
    CHECK_EQ(r.get32(event + ie_TimeStamp), 5u);
    CHECK_EQ(r.get32(event + ie_TimeStamp + 4), 250000u);
}

void the_settings_commands_and_peek_qualifier() {
    Rig r;
    r.put32(r.request + 32, 0);
    r.put32(r.request + 36, 500000);
    CHECK_EQ(r.do_io(IND_SETTHRESH, 0), 0);
    r.put32(r.request + 32, 0);
    r.put32(r.request + 36, 40000);
    CHECK_EQ(r.do_io(IND_SETPERIOD, 0), 0);
    CHECK_EQ(g_input.threshold_us(), 500000u);
    CHECK_EQ(g_input.period_us(), 40000u);
    r.put8(kData + 0x300, 1);
    CHECK_EQ(r.do_io(IND_SETMPORT, kData + 0x300), 0);
    CHECK_EQ(r.do_io(IND_SETMTYPE, kData + 0x300), 0);
    CHECK_EQ(r.do_io(IND_SETMTRIG, kData + 0x300), 0);
    CHECK_EQ(r.do_io(CMD_NONSTD + 20, 0), IOERR_NOCMD);
    CHECK(g_sunk.empty());

    g_input.set_qualifier(0x4009);
    r.m.cpu.d[0] = 0;
    CHECK_EQ(r.call(g_input.device_base(), input_lvo::PeekQualifier), ExitReason::Halt);
    CHECK_EQ(r.m.cpu.d[0], 0x4009u);
}

void handlers_change_swallow_and_see_events_by_priority() {
    Rig r;
    r.code(kHandlers, kChanger);
    r.code(kHandlers + 0x100, kSwallower);
    r.code(kHandlers + 0x200, kCounter);
    constexpr u32 count = kData + 0x400;
    r.put32(count, 0);
    const u32 counter = r.interrupt(kData + 0x500, 10, kHandlers + 0x200, count, "counter",
                                    kNames + 0x20);
    const u32 changer = r.interrupt(kData + 0x540, 60, kHandlers, 0, "changer", kNames + 0x40);
    const u32 swallower =
        r.interrupt(kData + 0x580, 55, kHandlers + 0x100, 0, "swallower", kNames + 0x60);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, counter), 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, swallower), 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, changer), 0);
    CHECK(logged_containing("input.device: added the handler changer at $9540, priority 60"));
    CHECK_EQ(g_input.handler_count(), 3u);
    CHECK_EQ(g_input.handler(0), changer);
    CHECK_EQ(g_input.handler(1), swallower);
    CHECK_EQ(g_input.handler(2), counter);
    CHECK(g_input.task() != 0);
    CHECK(g_input.chain_active());

    // The events wait for the device's task, in order, host keys too.
    g_input.submit(key(0x32));
    g_input.submit(key(0x10));
    g_input.submit_host_key(0x4B, 0x02);
    g_input.submit(key(0x20, 0x0001));
    g_input.submit(key(0x10 | IECODE_UP_PREFIX));
    CHECK(g_sunk.empty());
    CHECK(g_input.has_pending());
    // The device's task has its wake signal.
    CHECK((r.get32(g_input.task() + TC_SIGRECVD) & (1u << InputDevice::kWakeSignal)) != 0);

    r.m.cpu.d[2] = 0x12345678;
    r.m.cpu.a[7] = kStack - 0x40;
    g_input.process();
    CHECK(!g_input.has_pending());
    CHECK_EQ(g_sunk.size(), 4u);
    CHECK_EQ(g_sunk[0].code, 0x15u);  // changed
    CHECK_EQ(g_sunk[1].ie_class, InputDevice::kClassHostKey);
    CHECK_EQ(g_sunk[1].code, 0x4Bu);
    CHECK_EQ(g_sunk[1].qualifier, 0x02u);
    CHECK_EQ(g_sunk[2].code, 0x20u);
    CHECK_EQ(g_sunk[2].qualifier, 0x0001u);
    CHECK_EQ(g_sunk[3].code, 0x10u | IECODE_UP_PREFIX);  // only the press is swallowed
    // The handler below the sink ran once for each pass: before and after
    // the host key.
    CHECK_EQ(r.get32(count), 2u);
    // The machine is as the handlers found it.
    CHECK_EQ(r.m.cpu.d[2], 0x12345678u);
    CHECK_EQ(r.m.cpu.a[7], kStack - 0x40);
    CHECK(!g_exec.forbidden());

    // Taken out again, the chain is empty and events go straight on.
    CHECK_EQ(r.do_io(IND_REMHANDLER, changer), 0);
    CHECK_EQ(r.do_io(IND_REMHANDLER, swallower), 0);
    CHECK_EQ(r.do_io(IND_REMHANDLER, counter), 0);
    CHECK(logged_containing("input.device: removed the handler swallower at $9580"));
    CHECK_EQ(g_input.handler_count(), 0u);
    CHECK(!g_input.chain_active());
    g_sunk.clear();
    g_input.submit(key(0x32));
    CHECK_EQ(g_sunk.size(), 1u);
    CHECK_EQ(g_sunk[0].code, 0x32u);
    CHECK_EQ(r.get32(count), 2u);
}

void queued_mouse_moves_join() {
    Rig r;
    r.code(kHandlers, kCounter);
    constexpr u32 count = kData + 0x400;
    r.put32(count, 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER,
                     r.interrupt(kData + 0x500, 60, kHandlers, count, "counter", kNames + 0x20)),
             0);
    g_input.submit(move(1, 2));
    g_input.submit(move(3, -5));
    InputEventData held = move(1, 1);
    held.qualifier |= IEQUALIFIER_LEFTBUTTON;
    g_input.submit(held);
    g_input.process();
    CHECK_EQ(g_sunk.size(), 2u);
    CHECK_EQ(g_sunk[0].x, 4);
    CHECK_EQ(g_sunk[0].y, -3);
    CHECK_EQ(g_sunk[1].qualifier, IEQUALIFIER_RELATIVEMOUSE | IEQUALIFIER_LEFTBUTTON);
}

void a_handler_that_crashes_is_taken_out() {
    Rig r;
    r.code(kHandlers, kChanger);
    r.code(kHandlers + 0x100, kCrasher);
    const u32 crasher =
        r.interrupt(kData + 0x500, 70, kHandlers + 0x100, 0, "crasher", kNames + 0x20);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, crasher), 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER,
                     r.interrupt(kData + 0x540, 60, kHandlers, 0, "changer", kNames + 0x40)),
             0);
    g_input.submit(key(0x32));
    r.m.cpu.exit = ExitReason::None;
    g_input.process();
    CHECK(logged_containing("input.device: removed the handler crasher at $9500, Illegal"));
    CHECK_EQ(g_input.handler_count(), 1u);
    // The rest of the chain still ran.
    CHECK_EQ(g_sunk.size(), 1u);
    CHECK_EQ(g_sunk[0].code, 0x15u);
    CHECK_EQ(r.m.cpu.exit, ExitReason::None);
}

void forbid_a_handler_leaves_is_undone() {
    Rig r;
    r.code(kHandlers, kForbidder);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, r.interrupt(kData + 0x500, 60, kHandlers, 0, "forbidder",
                                                 kNames + 0x20)),
             0);
    g_input.submit(key(0x20));
    g_input.process();
    CHECK(!g_exec.forbidden());
    CHECK_EQ(r.get8(g_input.task() + TC_TDNESTCNT), 0xFFu);
    CHECK(logged_containing("input.device: ended Forbid or Disable left by the handler"));
    CHECK_EQ(g_input.handler_count(), 1u);
    CHECK_EQ(g_sunk.size(), 1u);
}

// The program's code is the segment at kSegment, 0x1800 to 0x2000.
u32 program_segment(Rig& r) {
    r.put32(kSegment - 8, 0x808);
    r.put32(kSegment - 4, 0);
    return (kSegment - 4) >> 2;
}

void a_program_ended_from_the_monitor_loses_its_handler() {
    Rig r;
    const u32 seglist = program_segment(r);
    CHECK(g_exec.begin_program(kData, seglist, true) != 0);
    // The program's handler, its code and Interrupt in its segment, and a
    // library's, which stays.
    r.code(kSegment + 0x100, kCounter);
    r.code(kHandlers, kCounter);
    constexpr u32 count = kData + 0x400;
    r.put32(count, 0);
    const u32 own = r.interrupt(kSegment + 0x200, 60, kSegment + 0x100, count, "program",
                                kSegment + 0x240);
    const u32 library =
        r.interrupt(kData + 0x500, 53, kHandlers, count, "library", kNames + 0x20);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, own), 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, library), 0);
    CHECK_EQ(g_input.handler_count(), 2u);

    // The monitor ends the program while it spins.
    r.code(kCode, {0x60FE});
    g_exec.request_rescue(Rescue::End, 0, kData);
    r.m.cpu.pc = kCode;
    CHECK_EQ(g_jit.run(100), ExitReason::Stopped);
    CHECK(logged_containing(
        "input.device: removed the handler program at $1a00, its program was ended"));
    CHECK_EQ(g_input.handler_count(), 1u);
    CHECK_EQ(g_input.handler(0), library);
    g_exec.end_program(kData);

    g_input.submit(key(0x20));
    g_input.process();
    CHECK_EQ(r.get32(count), 1u);
    CHECK_EQ(g_sunk.size(), 1u);
}

// A program that ends by itself leaves its handler in, as on an Amiga.
void a_program_that_ends_by_itself_keeps_its_handler() {
    Rig r;
    const u32 seglist = program_segment(r);
    CHECK(g_exec.begin_program(kData, seglist, true) != 0);
    r.code(kSegment + 0x100, kCounter);
    const u32 own = r.interrupt(kSegment + 0x200, 60, kSegment + 0x100, 0, "program",
                                kSegment + 0x240);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, own), 0);
    g_exec.end_program(kData);
    CHECK_EQ(g_input.handler_count(), 1u);
}

void a_removal_waits_for_the_pass_to_end() {
    Rig r;
    constexpr u32 event = kData + 0x600;
    constexpr u32 mark = kData + 0x640;
    for (u32 i = 0; i < ie_SIZEOF; ++i) {
        r.put8(event + i, 0);
    }
    r.put8(event + ie_Class, IECLASS_RAWKEY);
    r.put16(event + ie_Code, 0x77);
    r.put8(mark, 0);
    const u32 dev = g_input.device_base();
    const u32 req = r.request;
    // links: MOVE.L A0,(event).L, MOVEA.L #event,A0, MOVE.L A0,D0, RTS.
    r.code(kHandlers, {0x23C8, static_cast<u16>(event >> 16), static_cast<u16>(event), 0x207C,
                       static_cast<u16>(event >> 16), static_cast<u16>(event), 0x2008, 0x4E75});
    // remover: BeginIO of the removal request, then the request's node type.
    r.code(kHandlers + 0x100,
           {0x2F08, 0x227C, static_cast<u16>(req >> 16), static_cast<u16>(req), 0x2C7C,
            static_cast<u16>(dev >> 16), static_cast<u16>(dev), 0x4EAE, 0xFFE2, 0x1239,
            static_cast<u16>((req + LN_TYPE) >> 16), static_cast<u16>(req + LN_TYPE), 0x13C1,
            static_cast<u16>(mark >> 16), static_cast<u16>(mark), 0x201F, 0x4E75});
    const u32 adder = r.interrupt(kData + 0x500, 60, kHandlers, 0, "adder", kNames + 0x20);
    const u32 remover =
        r.interrupt(kData + 0x540, 55, kHandlers + 0x100, 0, "remover", kNames + 0x40);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, adder), 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, remover), 0);
    r.put16(req + io_Command, IND_REMHANDLER);
    r.put32(req + io_Data, adder);
    r.put8(req + io_Flags, 0);
    r.put8(req + LN_TYPE, NT_MESSAGE);
    g_input.submit(key(0x20));
    g_input.process();
    // The handler saw the request unanswered inside the pass.
    CHECK_EQ(r.get8(mark), NT_MESSAGE);
    CHECK_EQ(g_input.handler_count(), 1u);
    CHECK_EQ(g_input.handler(0), remover);
    CHECK_EQ(r.get8(req + io_Error), 0u);
    CHECK_EQ(r.get8(req + LN_TYPE), NT_REPLYMSG);
    CHECK_EQ(g_sunk.size(), 2u);
    CHECK_EQ(g_sunk[0].code, 0x77u);
    CHECK_EQ(g_sunk[1].code, 0x20u);
}

void a_full_queue_keeps_releases() {
    Rig r;
    r.code(kHandlers, kCounter);
    constexpr u32 count = kData + 0x400;
    r.put32(count, 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER,
                     r.interrupt(kData + 0x500, 10, kHandlers, count, "counter", kNames + 0x20)),
             0);
    // A press, a move, then presses up to the full queue.
    g_input.submit(key(0x20));
    g_input.submit(move(3, 4));
    for (u32 i = 2; i < InputDevice::kQueueCapacity; ++i) {
        g_input.submit(key(0x30, static_cast<u16>(i)));
    }
    CHECK_EQ(logged_count("events lost"), 0u);
    g_input.submit(key(0x20 | IECODE_UP_PREFIX, 0xFFFF));
    CHECK_EQ(logged_count("input.device: events lost, the queue is full"), 1u);
    g_input.process();
    CHECK_EQ(g_sunk.size(), InputDevice::kQueueCapacity);
    CHECK_EQ(g_sunk[0].code, 0x20u);
    CHECK_EQ(g_sunk[0].qualifier, 0u);
    // The move went, everything else stayed in order, the release last.
    for (u32 i = 1; i + 1 < g_sunk.size(); ++i) {
        CHECK_EQ(g_sunk[i].code, 0x30u);
        CHECK_EQ(g_sunk[i].qualifier, i + 1);
    }
    CHECK_EQ(g_sunk.back().code, 0x20u | IECODE_UP_PREFIX);
    CHECK_EQ(g_sunk.back().qualifier, 0xFFFFu);

    // The queue drained, so the next overflow is logged again. With no
    // move left, the newest press goes.
    g_sunk.clear();
    g_input.submit(key(0x21));
    for (u32 i = 1; i < InputDevice::kQueueCapacity; ++i) {
        g_input.submit(key(0x31, static_cast<u16>(i)));
    }
    g_input.submit(key(0x21 | IECODE_UP_PREFIX, 0xFFFF));
    CHECK_EQ(logged_count("input.device: events lost, the queue is full"), 2u);
    g_input.process();
    CHECK_EQ(g_sunk.size(), InputDevice::kQueueCapacity);
    CHECK_EQ(g_sunk[0].code, 0x21u);
    for (u32 i = 1; i + 1 < g_sunk.size(); ++i) {
        CHECK_EQ(g_sunk[i].code, 0x31u);
        CHECK_EQ(g_sunk[i].qualifier, i);
    }
    CHECK_EQ(g_sunk.back().code, 0x21u | IECODE_UP_PREFIX);
}

void the_device_says_when_it_is_in_a_pass() {
    Rig r;
    r.code(kHandlers, kCounter);
    constexpr u32 count = kData + 0x400;
    r.put32(count, 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER,
                     r.interrupt(kData + 0x500, 10, kHandlers, count, "counter", kNames + 0x20)),
             0);
    PassWatch watch;
    g_input.set_sink(watch_pass, &watch);
    g_input.submit(key(0x20));
    CHECK(!g_input.in_pass());
    g_input.process();
    // The batch is off the queue, but the pass still runs.
    CHECK(watch.in_pass);
    CHECK(!watch.had_pending);
    CHECK(!g_input.in_pass());
}

void an_ended_program_drops_its_events_from_the_pass() {
    Rig r;
    const u32 seglist = program_segment(r);
    const u32 program = g_exec.begin_program(kData, seglist, true);
    CHECK(program != 0);
    constexpr u32 event = kSegment + 0x300;
    constexpr u32 seen = kData + 0x480;
    for (u32 i = 0; i < ie_SIZEOF; ++i) {
        r.put8(event + i, 0);
    }
    r.put8(event + ie_Class, IECLASS_RAWKEY);
    r.put16(event + ie_Code, 0x77);
    r.put32(seen, 0);
    // The program's handler links its own event in.
    r.code(kSegment + 0x100,
           {0x23C8, static_cast<u16>(event >> 16), static_cast<u16>(event), 0x207C,
            static_cast<u16>(event >> 16), static_cast<u16>(event), 0x2008, 0x4E75});
    const u32 owner = r.interrupt(kSegment + 0x200, 60, kSegment + 0x100, 0, "program",
                                  kSegment + 0x240);
    // Below the sink, this one records the list it was given.
    r.code(kHandlers, {0x23C8, static_cast<u16>(seen >> 16), static_cast<u16>(seen), 0x2008,
                       0x4E75});
    const u32 lower =
        r.interrupt(kData + 0x540, 40, kHandlers, 0, "lower", kNames + 0x20);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, owner), 0);
    CHECK_EQ(r.do_io(IND_ADDHANDLER, lower), 0);
    Ending ending{program};
    g_input.set_sink(end_program_sink, &ending);
    g_input.submit(key(0x20));
    g_input.process();
    CHECK(ending.done);
    CHECK_EQ(g_input.handler_count(), 1u);
    CHECK_EQ(g_input.handler(0), lower);
    // The sink was handed the event before the program ended; the lower
    // handler only got the key behind it.
    CHECK_EQ(g_sunk.size(), 2u);
    CHECK_EQ(g_sunk[0].code, 0x77u);
    CHECK_EQ(g_sunk[1].code, 0x20u);
    const u32 rest = r.get32(seen);
    CHECK(rest != 0);
    CHECK(rest != event);
    CHECK_EQ(r.get8(rest + ie_Class), IECLASS_RAWKEY);
    CHECK_EQ(r.get16(rest + ie_Code), 0x20u);
}

}

int main() {
    with_no_handler_events_go_straight_to_the_sink();
    the_settings_commands_and_peek_qualifier();
    handlers_change_swallow_and_see_events_by_priority();
    queued_mouse_moves_join();
    a_handler_that_crashes_is_taken_out();
    forbid_a_handler_leaves_is_undone();
    a_program_ended_from_the_monitor_loses_its_handler();
    a_program_that_ends_by_itself_keeps_its_handler();
    a_removal_waits_for_the_pass_to_end();
    a_full_queue_keeps_releases();
    the_device_says_when_it_is_in_a_pass();
    an_ended_program_drops_its_events_from_the_pass();
    return check::finish("test_input_device");
}
