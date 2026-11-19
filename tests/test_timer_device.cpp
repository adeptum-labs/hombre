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

#include "libs/timer_device.hpp"
#include "check.hpp"

#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;

namespace {

constexpr u32 kCode = 0x1000;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;
constexpr u32 kNames = 0x9800;
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;
constexpr usize kRegion = 1 << 20;

Jit g_jit;
Exec g_exec;
TimerDevice g_timer;
std::vector<std::string> g_lines;

void capture(const char* line) {
    g_lines.push_back(line);
}

bool logged(const std::string& text) {
    for (const std::string& line : g_lines) {
        if (line == text) {
            return true;
        }
    }
    return false;
}

struct FakeTimerHost {
    u64 now_us = 0;
    u64 idle_step_us = 0;
    u32 idle_calls = 0;
    bool idle_ok = false;
};

bool fake_create(void*, u32) { return true; }
void fake_switch(void*, u32, u32) {}
void fake_destroy(void*, u32) {}
bool fake_slice_over(void*) { return false; }
void fake_poll(void*) {}
bool fake_idle(void* ctx) {
    FakeTimerHost& h = *static_cast<FakeTimerHost*>(ctx);
    ++h.idle_calls;
    h.now_us += h.idle_step_us;
    return h.idle_ok;
}
u64 fake_now_us(void* ctx) { return static_cast<FakeTimerHost*>(ctx)->now_us; }

class Rig {
public:
    Rig() : memory_(kGuestBytes) {
        fd_ = memfd_create("timer_device", 0);
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
        g_log = capture;
        g_exec.set_task_host(TaskHost{fake_create, fake_switch, fake_destroy, fake_slice_over,
                                      fake_poll, fake_idle, fake_now_us, &host});
        CHECK(g_exec.init(m, kFreeStart, kFreeEnd));
        CHECK(g_timer.init(m, g_exec));
        m.cpu.a[7] = kStack;
        make_task();
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
    FakeTimerHost host{};

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

    ExitReason call_exec(u32 lvo) {
        return call_vec(g_exec.exec_base(), lvo);
    }

    ExitReason call_timer(u32 lvo) {
        return call_vec(g_timer.device_base(), lvo);
    }

private:
    void make_task() {
        const u32 task = kData;
        for (u32 i = 0; i < TC_SIZE; ++i) {
            put8(task + i, 0);
        }
        put8(task + LN_TYPE, NT_TASK);
        put32(task + TC_SIGALLOC, 0x0000FFFF);
        g_exec.set_this_task(task);
    }

    ExitReason call_vec(u32 base, u32 lvo) {
        put16(kCode, 0x4EA9);  // JSR d16(A1) — use A6 via 0x4EAE
        put16(kCode, 0x4EAE);
        put16(kCode + 2, static_cast<u16>(0x10000 - lvo));
        put16(kCode + 4, 0x4E75);
        g_jit.flush();
        m.cpu.a[6] = base;
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCode;
        put32(kStack - 4, kReturnStub);
        m.cpu.a[7] = kStack - 4;
        return g_jit.run(100000);
    }

    std::vector<u8> memory_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

void timer_queue_ordering_exact_deadline_abort_capacity() {
    TimerQueue q;
    CHECK(q.empty());
    CHECK(!q.full());
    CHECK_EQ(q.count(), 0u);

    CHECK(q.insert(500, 0x100));
    CHECK(q.insert(200, 0x200));
    CHECK(q.insert(300, 0x300));
    CHECK(q.insert(200, 0x400));  // tie preserves FIFO
    CHECK_EQ(q.count(), 4u);
    CHECK_EQ(q.next_deadline(), 200u);

    TimerQueue::Entry e{};
    // Not due before 200.
    CHECK(!q.pop_due(199, e));
    // Due at exact deadline 200 (FIFO order: 0x200 then 0x400).
    CHECK(q.pop_due(200, e));
    CHECK_EQ(e.request, 0x200u);
    CHECK(q.pop_due(200, e));
    CHECK_EQ(e.request, 0x400u);
    CHECK(!q.pop_due(200, e));

    // Abort middle entry 0x300.
    CHECK(q.remove(0x300));
    CHECK(!q.remove(0x300));
    CHECK_EQ(q.count(), 1u);
    CHECK(q.pop_due(500, e));
    CHECK_EQ(e.request, 0x100u);
    CHECK(q.empty());

    // Capacity 64.
    for (u32 i = 0; i < TimerQueue::kCapacity; ++i) {
        CHECK(q.insert(1000 + (TimerQueue::kCapacity - i), 0x1000 + i));
    }
    CHECK(q.full());
    CHECK(!q.insert(500, 0x9999));
    CHECK_EQ(q.count(), TimerQueue::kCapacity);
    // Earliest inserted (deadline 1001, request 0x1000 + 63) pops first.
    CHECK(q.pop_due(1001, e));
    CHECK_EQ(e.deadline_us, 1001u);
    CHECK_EQ(e.request, 0x1000u + 63u);
    CHECK(!q.full());
}

void add_sub_cmp_time_carry_borrow_and_signs() {
    Rig r;
    const u32 dest = kData + 0x200;
    const u32 src = kData + 0x210;

    // AddTime with micro carry.
    r.put32(dest + TV_SECS, 10);
    r.put32(dest + TV_MICRO, 750000);
    r.put32(src + TV_SECS, 2);
    r.put32(src + TV_MICRO, 500000);
    r.m.cpu.a[0] = dest;
    r.m.cpu.a[1] = src;
    CHECK_EQ(r.call_timer(timer_lvo::AddTime), ExitReason::Returned);
    CHECK_EQ(r.get32(dest + TV_SECS), 13u);
    CHECK_EQ(r.get32(dest + TV_MICRO), 250000u);

    // AddTime exact 1,000,000 carry.
    r.put32(dest + TV_SECS, 1);
    r.put32(dest + TV_MICRO, 999999);
    r.put32(src + TV_SECS, 0);
    r.put32(src + TV_MICRO, 1);
    r.m.cpu.a[0] = dest;
    r.m.cpu.a[1] = src;
    CHECK_EQ(r.call_timer(timer_lvo::AddTime), ExitReason::Returned);
    CHECK_EQ(r.get32(dest + TV_SECS), 2u);
    CHECK_EQ(r.get32(dest + TV_MICRO), 0u);

    // SubTime with borrow on tv_micro.
    r.put32(dest + TV_SECS, 5);
    r.put32(dest + TV_MICRO, 200000);
    r.put32(src + TV_SECS, 2);
    r.put32(src + TV_MICRO, 700000);
    r.m.cpu.a[0] = dest;
    r.m.cpu.a[1] = src;
    CHECK_EQ(r.call_timer(timer_lvo::SubTime), ExitReason::Returned);
    CHECK_EQ(r.get32(dest + TV_SECS), 2u);
    CHECK_EQ(r.get32(dest + TV_MICRO), 500000u);

    // SubTime without borrow.
    r.put32(dest + TV_SECS, 5);
    r.put32(dest + TV_MICRO, 800000);
    r.put32(src + TV_SECS, 1);
    r.put32(src + TV_MICRO, 300000);
    r.m.cpu.a[0] = dest;
    r.m.cpu.a[1] = src;
    CHECK_EQ(r.call_timer(timer_lvo::SubTime), ExitReason::Returned);
    CHECK_EQ(r.get32(dest + TV_SECS), 4u);
    CHECK_EQ(r.get32(dest + TV_MICRO), 500000u);

    // CmpTime signs: 0 equal, -1 dest > src, +1 dest < src.
    r.put32(dest + TV_SECS, 10);
    r.put32(dest + TV_MICRO, 500);
    r.put32(src + TV_SECS, 10);
    r.put32(src + TV_MICRO, 500);
    r.m.cpu.a[0] = dest;
    r.m.cpu.a[1] = src;
    CHECK_EQ(r.call_timer(timer_lvo::CmpTime), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    r.put32(dest + TV_SECS, 10);
    r.put32(dest + TV_MICRO, 501);
    r.m.cpu.a[0] = dest;
    r.m.cpu.a[1] = src;
    CHECK_EQ(r.call_timer(timer_lvo::CmpTime), ExitReason::Returned);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), -1);

    r.put32(dest + TV_SECS, 9);
    r.put32(dest + TV_MICRO, 999999);
    r.m.cpu.a[0] = dest;
    r.m.cpu.a[1] = src;
    CHECK_EQ(r.call_timer(timer_lvo::CmpTime), ExitReason::Returned);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), 1);
}

void sys_time_setsystime_and_readeclock() {
    Rig r;
    CHECK_EQ(r.get32(g_exec.exec_base() + ex_EClockFrequency), kEClockFrequency);
    CHECK_EQ(r.get16(g_timer.device_base() + LIB_VERSION), 40u);

    // Boot offset + monotonic progression.
    g_timer.set_sys_time_us(1000ull * 1000000ull + 250000ull, 50000ull);
    r.host.now_us = 150000ull;  // +100 ms since capture
    const u32 tv = kData + 0x200;
    r.m.cpu.a[0] = tv;
    CHECK_EQ(r.call_timer(timer_lvo::GetSysTime), ExitReason::Returned);
    CHECK_EQ(r.get32(tv + TV_SECS), 1000u);
    CHECK_EQ(r.get32(tv + TV_MICRO), 350000u);

    // ReadEClock frequency and monotonic ticks.
    r.host.now_us = 2000000ull;  // 2.0 s -> 2 * 709379 = 1418758 ticks
    const u32 ev = kData + 0x210;
    r.m.cpu.a[0] = ev;
    CHECK_EQ(r.call_timer(timer_lvo::ReadEClock), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], kEClockFrequency);
    CHECK_EQ(r.get32(ev + EV_HI), 0u);
    CHECK_EQ(r.get32(ev + EV_LO), 2u * kEClockFrequency);

    // OpenDevice + TR_SETSYSTIME + TR_GETSYSTIME.
    CHECK_EQ(r.call_exec(exec_lvo::CreateMsgPort), ExitReason::Returned);
    const u32 port = r.m.cpu.d[0];
    CHECK(port != 0);
    r.m.cpu.a[0] = port;
    r.m.cpu.d[0] = TR_SIZE;
    CHECK_EQ(r.call_exec(exec_lvo::CreateIORequest), ExitReason::Returned);
    const u32 req = r.m.cpu.d[0];
    CHECK(req != 0);

    // Unknown unit fails OpenDevice with IOERR_OPENFAIL.
    r.m.cpu.a[0] = r.str(kNames, "timer.device");
    r.m.cpu.d[0] = 9;
    r.m.cpu.a[1] = req;
    r.m.cpu.d[1] = 0;
    CHECK_EQ(r.call_exec(exec_lvo::OpenDevice), ExitReason::Returned);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), IOERR_OPENFAIL);
    CHECK_EQ(static_cast<i8>(r.get8(req + io_Error)), IOERR_OPENFAIL);

    // Valid unit opens.
    r.m.cpu.a[0] = r.str(kNames, "timer.device");
    r.m.cpu.d[0] = UNIT_MICROHZ;
    r.m.cpu.a[1] = req;
    r.m.cpu.d[1] = 0;
    CHECK_EQ(r.call_exec(exec_lvo::OpenDevice), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    // TR_SETSYSTIME changes offset.
    r.put16(req + io_Command, TR_SETSYSTIME);
    r.put32(req + tv_secs, 5000);
    r.put32(req + tv_micro, 123456);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DoIO), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    r.host.now_us += 500000;
    r.put16(req + io_Command, TR_GETSYSTIME);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DoIO), ExitReason::Returned);
    CHECK_EQ(r.get32(req + tv_secs), 5000u);
    CHECK_EQ(r.get32(req + tv_micro), 623456u);

    // Unknown command returns IOERR_NOCMD.
    r.put16(req + io_Command, 99);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DoIO), ExitReason::Returned);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), IOERR_NOCMD);

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::CloseDevice), ExitReason::Returned);
    r.m.cpu.a[0] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DeleteIORequest), ExitReason::Returned);
    r.m.cpu.a[0] = port;
    CHECK_EQ(r.call_exec(exec_lvo::DeleteMsgPort), ExitReason::Returned);
}

void requests_idle_until_deadline_abort_and_queue_full() {
    Rig r;
    r.host.now_us = 10000;
    r.host.idle_step_us = 5000;
    r.host.idle_ok = false;  // Wait must NOT abort while a timer is pending

    CHECK_EQ(r.call_exec(exec_lvo::CreateMsgPort), ExitReason::Returned);
    const u32 port = r.m.cpu.d[0];
    r.m.cpu.a[0] = port;
    r.m.cpu.d[0] = TR_SIZE;
    CHECK_EQ(r.call_exec(exec_lvo::CreateIORequest), ExitReason::Returned);
    const u32 req = r.m.cpu.d[0];

    r.m.cpu.a[0] = r.str(kNames, "timer.device");
    r.m.cpu.d[0] = UNIT_MICROHZ;
    r.m.cpu.a[1] = req;
    r.m.cpu.d[1] = 0;
    CHECK_EQ(r.call_exec(exec_lvo::OpenDevice), ExitReason::Returned);

    // DoIO of 25000 us idles 5 times (5 * 5000 us) even with idle_ok = false.
    r.put16(req + io_Command, TR_ADDREQUEST);
    r.put32(req + tv_secs, 0);
    r.put32(req + tv_micro, 25000);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DoIO), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.host.idle_calls, 5u);
    CHECK_EQ(r.host.now_us, 35000u);

    // Past deadline (0, 0) completes quick in DoIO without idling.
    r.host.idle_calls = 0;
    r.put16(req + io_Command, TR_ADDREQUEST);
    r.put32(req + tv_secs, 0);
    r.put32(req + tv_micro, 0);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DoIO), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.host.idle_calls, 0u);
    CHECK_EQ(r.get8(req + io_Flags) & IOF_QUICK, IOF_QUICK);

    // SendIO + CheckIO + AbortIO + WaitIO.
    r.put16(req + io_Command, TR_ADDREQUEST);
    r.put32(req + tv_secs, 5);
    r.put32(req + tv_micro, 0);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::SendIO), ExitReason::Returned);
    CHECK(g_timer.has_pending());

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::CheckIO), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::AbortIO), ExitReason::Returned);
    CHECK(!g_timer.has_pending());

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::WaitIO), ExitReason::Returned);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), IOERR_ABORTED);

    // Queue full at 64 pending requests logs and completes the 65th with -1.
    std::vector<u32> extra;
    for (u32 i = 0; i < TimerQueue::kCapacity; ++i) {
        r.m.cpu.a[0] = port;
        r.m.cpu.d[0] = TR_SIZE;
        CHECK_EQ(r.call_exec(exec_lvo::CreateIORequest), ExitReason::Returned);
        const u32 r2 = r.m.cpu.d[0];
        CHECK(r2 != 0);
        r.put32(r2 + io_Device, g_timer.device_base());
        r.put32(r2 + io_Unit, r.get32(req + io_Unit));
        r.put16(r2 + io_Command, TR_ADDREQUEST);
        r.put32(r2 + tv_secs, 10 + i);
        r.put32(r2 + tv_micro, 0);
        r.m.cpu.a[1] = r2;
        CHECK_EQ(r.call_exec(exec_lvo::SendIO), ExitReason::Returned);
        extra.push_back(r2);
    }
    CHECK(g_timer.queue().full());

    g_lines.clear();
    r.put16(req + io_Command, TR_ADDREQUEST);
    r.put32(req + tv_secs, 1);
    r.put32(req + tv_micro, 0);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DoIO), ExitReason::Returned);
    CHECK_EQ(static_cast<i32>(r.m.cpu.d[0]), -1);
    CHECK(logged("timer.device: request queue full"));

    for (u32 r2 : extra) {
        r.m.cpu.a[1] = r2;
        CHECK_EQ(r.call_exec(exec_lvo::AbortIO), ExitReason::Returned);
        r.m.cpu.a[1] = r2;
        CHECK_EQ(r.call_exec(exec_lvo::WaitIO), ExitReason::Returned);
        r.m.cpu.a[0] = r2;
        CHECK_EQ(r.call_exec(exec_lvo::DeleteIORequest), ExitReason::Returned);
    }

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::CloseDevice), ExitReason::Returned);
    r.m.cpu.a[0] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DeleteIORequest), ExitReason::Returned);
    r.m.cpu.a[0] = port;
    CHECK_EQ(r.call_exec(exec_lvo::DeleteMsgPort), ExitReason::Returned);
}

void waituntil_and_eclock_units() {
    Rig r;
    r.host.now_us = 1000000;
    r.host.idle_step_us = 100000;
    r.host.idle_ok = false;
    g_timer.set_sys_time_us(2000ull * 1000000ull, 1000000ull);

    CHECK_EQ(r.call_exec(exec_lvo::CreateMsgPort), ExitReason::Returned);
    const u32 port = r.m.cpu.d[0];
    r.m.cpu.a[0] = port;
    r.m.cpu.d[0] = TR_SIZE;
    CHECK_EQ(r.call_exec(exec_lvo::CreateIORequest), ExitReason::Returned);
    const u32 req = r.m.cpu.d[0];

    // UNIT_WAITUNTIL: wait until sys time 2000.3 s -> 300 ms from now.
    r.m.cpu.a[0] = r.str(kNames, "timer.device");
    r.m.cpu.d[0] = UNIT_WAITUNTIL;
    r.m.cpu.a[1] = req;
    r.m.cpu.d[1] = 0;
    CHECK_EQ(r.call_exec(exec_lvo::OpenDevice), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    r.put16(req + io_Command, TR_ADDREQUEST);
    r.put32(req + tv_secs, 2000);
    r.put32(req + tv_micro, 300000);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DoIO), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.host.idle_calls, 3u);
    CHECK_EQ(r.host.now_us, 1300000u);

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::CloseDevice), ExitReason::Returned);

    // UNIT_WAITECLOCK: wait until eclock = 2 * 709379 (2.0 s monotonic).
    r.m.cpu.a[0] = r.str(kNames, "timer.device");
    r.m.cpu.d[0] = UNIT_WAITECLOCK;
    r.m.cpu.a[1] = req;
    r.m.cpu.d[1] = 0;
    CHECK_EQ(r.call_exec(exec_lvo::OpenDevice), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);

    r.put16(req + io_Command, TR_ADDREQUEST);
    r.put32(req + ev_hi, 0);
    r.put32(req + ev_lo, 2 * kEClockFrequency);
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DoIO), ExitReason::Returned);
    CHECK_EQ(r.m.cpu.d[0], 0u);
    CHECK_EQ(r.host.now_us, 2000000u);

    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::CloseDevice), ExitReason::Returned);
    r.m.cpu.a[0] = req;
    CHECK_EQ(r.call_exec(exec_lvo::DeleteIORequest), ExitReason::Returned);
    r.m.cpu.a[0] = port;
    CHECK_EQ(r.call_exec(exec_lvo::DeleteMsgPort), ExitReason::Returned);
}

}  // namespace

int main() {
    timer_queue_ordering_exact_deadline_abort_capacity();
    add_sub_cmp_time_carry_borrow_and_signs();
    sys_time_setsystime_and_readeclock();
    requests_idle_until_deadline_abort_and_queue_full();
    waituntil_and_eclock_units();
    return check::finish("test_timer_device");
}
