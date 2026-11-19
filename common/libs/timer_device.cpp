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

#include "timer_device.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;

namespace {

constexpr u64 kMicroPerSec = 1000000ull;

void fault_null(Bus& b) {
    b.bytes_at(0, b.machine().mem.size + 1);
}

u64 eclock_to_us(u64 ticks) {
    const u64 whole = ticks / kEClockFrequency;
    const u64 rem = ticks % kEClockFrequency;
    const u64 extra = (rem * kMicroPerSec + kEClockFrequency - 1) / kEClockFrequency;
    if (whole > (~u64(0) - extra) / kMicroPerSec) {
        return ~u64(0);
    }
    return whole * kMicroPerSec + extra;
}

u64 us_to_eclock(u64 us) {
    const u64 whole = us / kMicroPerSec;
    const u64 rem = us % kMicroPerSec;
    return whole * kEClockFrequency + (rem * kEClockFrequency) / kMicroPerSec;
}

void normalize_timeval(u32 secs, u32 micro, u64& out_secs, u32& out_micro) {
    out_secs = static_cast<u64>(secs) + micro / kMicroPerSec;
    out_micro = static_cast<u32>(micro % kMicroPerSec);
}

}

struct TimerOps {
    static TimerDevice& of(void* ctx) { return *static_cast<TimerDevice*>(ctx); }

    static bool decode_unit(const TimerDevice& t, u32 unit_val, u32& unit_out) {
        if (unit_val >= t.units_base_ &&
            unit_val < t.units_base_ + TimerDevice::kUnitCount * TimerDevice::kUnitSize &&
            ((unit_val - t.units_base_) % TimerDevice::kUnitSize) == 0) {
            unit_out = (unit_val - t.units_base_) / TimerDevice::kUnitSize;
            return true;
        }
        if (unit_val < TimerDevice::kUnitCount) {
            unit_out = unit_val;
            return true;
        }
        return false;
    }

    static void complete_now(TimerDevice& t, Bus& b, u32 io_raw, u8 flags, i8 err) {
        b.w8(io_raw + io_Error, static_cast<u8>(err));
        if (!b.ok()) {
            return;
        }
        if ((flags & IOF_QUICK) != 0) {
            return;
        }
        t.exec_->reply_message(io_raw);
    }

    static void open(void* ctx, Machine& m) {
        TimerDevice& t = of(ctx);
        Bus b(m);
        const u32 unit = m.cpu.d[0];
        const u32 io_raw = m.cpu.a[1];
        const u32 dev = m.cpu.a[6];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        if (unit >= TimerDevice::kUnitCount) {
            b.w32(io_raw + io_Unit, 0);
            b.w32(io_raw + io_Device, 0);
            b.w8(io_raw + io_Error, static_cast<u8>(IOERR_OPENFAIL));
            if (b.ok()) {
                native::set_result(m, static_cast<u32>(-1));
            }
            return;
        }
        b.w16(dev + LIB_OPENCNT, b.r16(dev + LIB_OPENCNT) + 1);
        b.w8(dev + LIB_FLAGS, b.r8(dev + LIB_FLAGS) & ~LIBF_DELEXP);
        b.w32(io_raw + io_Unit, t.units_base_ + unit * TimerDevice::kUnitSize);
        b.w8(io_raw + io_Error, 0);
        if (b.ok()) {
            native::set_result(m, 0);
        }
    }

    static void close(void*, Machine& m) {
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        const u32 dev = m.cpu.a[6];
        const u32 count = b.r16(dev + LIB_OPENCNT);
        if (count != 0) {
            b.w16(dev + LIB_OPENCNT, count - 1);
        }
        if (native::guest_address(m, io_raw) != 0) {
            b.w32(io_raw + io_Unit, 0);
        }
        native::set_result(m, 0);
    }

    static void begin_io(void* ctx, Machine& m) {
        TimerDevice& t = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        const u16 cmd = static_cast<u16>(b.r16(io_raw + io_Command));
        const u8 flags = static_cast<u8>(b.r8(io_raw + io_Flags));
        const u32 unit_val = b.r32(io_raw + io_Unit);
        if (!b.ok()) {
            return;
        }
        b.w8(io_raw + io_Error, 0);

        if (cmd == TR_GETSYSTIME) {
            const u64 sys_us = t.sys_time_us();
            b.w32(io_raw + tv_secs, static_cast<u32>(sys_us / kMicroPerSec));
            b.w32(io_raw + tv_micro, static_cast<u32>(sys_us % kMicroPerSec));
            complete_now(t, b, io_raw, flags, 0);
            return;
        }
        if (cmd == TR_SETSYSTIME) {
            const u32 secs = b.r32(io_raw + tv_secs);
            const u32 micro = b.r32(io_raw + tv_micro);
            if (!b.ok()) {
                return;
            }
            const u64 target_us = static_cast<u64>(secs) * kMicroPerSec + micro;
            t.set_sys_time_us(target_us, t.current_now_us());
            complete_now(t, b, io_raw, flags, 0);
            return;
        }
        if (cmd != TR_ADDREQUEST) {
            complete_now(t, b, io_raw, flags, IOERR_NOCMD);
            return;
        }

        u32 unit = 0;
        if (!decode_unit(t, unit_val, unit)) {
            complete_now(t, b, io_raw, flags, IOERR_OPENFAIL);
            return;
        }
        const u32 f0 = b.r32(io_raw + TR_TIME);
        const u32 f1 = b.r32(io_raw + TR_TIME + 4);
        if (!b.ok()) {
            return;
        }

        const u64 now = t.current_now_us();
        u64 deadline_us = 0;
        bool already_past = false;

        switch (unit) {
        case UNIT_MICROHZ:
        case UNIT_VBLANK: {
            const u64 delta = static_cast<u64>(f0) * kMicroPerSec + f1;
            already_past = delta == 0;
            deadline_us = (~u64(0) - now < delta) ? ~u64(0) : now + delta;
            break;
        }
        case UNIT_ECLOCK: {
            const u64 ticks = (static_cast<u64>(f0) << 32) | f1;
            const u64 delta = eclock_to_us(ticks);
            already_past = delta == 0;
            deadline_us = (~u64(0) - now < delta) ? ~u64(0) : now + delta;
            break;
        }
        case UNIT_WAITUNTIL: {
            const u64 target_sys = static_cast<u64>(f0) * kMicroPerSec + f1;
            const i64 mono = static_cast<i64>(target_sys) - t.sys_offset_us_;
            if (mono <= static_cast<i64>(now)) {
                already_past = true;
            } else {
                deadline_us = static_cast<u64>(mono);
            }
            break;
        }
        case UNIT_WAITECLOCK: {
            const u64 target_ticks = (static_cast<u64>(f0) << 32) | f1;
            deadline_us = eclock_to_us(target_ticks);
            already_past = deadline_us <= now;
            break;
        }
        default:
            complete_now(t, b, io_raw, flags, IOERR_OPENFAIL);
            return;
        }

        if (already_past) {
            complete_now(t, b, io_raw, flags, 0);
            return;
        }
        if (t.queue_.full()) {
            LogLine().text("timer.device: request queue full").send();
            complete_now(t, b, io_raw, flags, -1);
            return;
        }
        b.w8(io_raw + io_Flags, flags & ~IOF_QUICK);
        b.w8(io_raw + LN_TYPE, NT_MESSAGE);
        if (!b.ok()) {
            return;
        }
        t.queue_.insert(deadline_us, native::guest_address(m, io_raw));
    }

    static void abort_io(void* ctx, Machine& m) {
        TimerDevice& t = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        const u32 req = native::guest_address(m, io_raw);
        if (t.queue_.remove(req)) {
            b.w8(req + io_Error, static_cast<u8>(IOERR_ABORTED));
            if (b.ok()) {
                t.exec_->reply_message(req);
            }
            if (b.ok()) {
                native::set_result(m, 0);
            }
            return;
        }
        native::set_result(m, static_cast<u32>(-1));
    }

    static void add_time(void*, Machine& m) {
        Bus b(m);
        const u32 dest = m.cpu.a[0];
        const u32 src = m.cpu.a[1];
        if (native::guest_address(m, dest) == 0 || native::guest_address(m, src) == 0) {
            fault_null(b);
            return;
        }
        const u32 d_sec = b.r32(dest + TV_SECS);
        const u32 d_mic = b.r32(dest + TV_MICRO);
        const u32 s_sec = b.r32(src + TV_SECS);
        const u32 s_mic = b.r32(src + TV_MICRO);
        if (!b.ok()) {
            return;
        }
        const u64 total_micro = static_cast<u64>(d_mic) + static_cast<u64>(s_mic);
        const u64 total_secs =
            static_cast<u64>(d_sec) + static_cast<u64>(s_sec) + total_micro / kMicroPerSec;
        b.w32(dest + TV_SECS, static_cast<u32>(total_secs));
        b.w32(dest + TV_MICRO, static_cast<u32>(total_micro % kMicroPerSec));
    }

    static void sub_time(void*, Machine& m) {
        Bus b(m);
        const u32 dest = m.cpu.a[0];
        const u32 src = m.cpu.a[1];
        if (native::guest_address(m, dest) == 0 || native::guest_address(m, src) == 0) {
            fault_null(b);
            return;
        }
        u64 d_sec = 0;
        u32 d_mic = 0;
        u64 s_sec = 0;
        u32 s_mic = 0;
        normalize_timeval(b.r32(dest + TV_SECS), b.r32(dest + TV_MICRO), d_sec, d_mic);
        normalize_timeval(b.r32(src + TV_SECS), b.r32(src + TV_MICRO), s_sec, s_mic);
        if (!b.ok()) {
            return;
        }
        if (d_mic < s_mic) {
            d_mic += static_cast<u32>(kMicroPerSec);
            --d_sec;
        }
        d_mic -= s_mic;
        d_sec -= s_sec;
        b.w32(dest + TV_SECS, static_cast<u32>(d_sec));
        b.w32(dest + TV_MICRO, d_mic);
    }

    static void cmp_time(void*, Machine& m) {
        Bus b(m);
        const u32 dest = m.cpu.a[0];
        const u32 src = m.cpu.a[1];
        if (native::guest_address(m, dest) == 0 || native::guest_address(m, src) == 0) {
            fault_null(b);
            return;
        }
        u64 d_sec = 0;
        u32 d_mic = 0;
        u64 s_sec = 0;
        u32 s_mic = 0;
        normalize_timeval(b.r32(dest + TV_SECS), b.r32(dest + TV_MICRO), d_sec, d_mic);
        normalize_timeval(b.r32(src + TV_SECS), b.r32(src + TV_MICRO), s_sec, s_mic);
        if (!b.ok()) {
            return;
        }
        u32 res = 0;
        if (d_sec > s_sec || (d_sec == s_sec && d_mic > s_mic)) {
            res = static_cast<u32>(-1);
        } else if (d_sec < s_sec || (d_sec == s_sec && d_mic < s_mic)) {
            res = 1;
        }
        native::set_result(m, res);
    }

    static void read_eclock(void* ctx, Machine& m) {
        TimerDevice& t = of(ctx);
        Bus b(m);
        const u32 dest = m.cpu.a[0];
        if (native::guest_address(m, dest) == 0) {
            fault_null(b);
            return;
        }
        const u64 ticks = us_to_eclock(t.current_now_us());
        b.w32(dest + EV_HI, static_cast<u32>(ticks >> 32));
        b.w32(dest + EV_LO, static_cast<u32>(ticks));
        if (b.ok()) {
            native::set_result(m, kEClockFrequency);
        }
    }

    static void get_sys_time(void* ctx, Machine& m) {
        TimerDevice& t = of(ctx);
        Bus b(m);
        const u32 dest = m.cpu.a[0];
        if (native::guest_address(m, dest) == 0) {
            fault_null(b);
            return;
        }
        const u64 sys_us = t.sys_time_us();
        b.w32(dest + TV_SECS, static_cast<u32>(sys_us / kMicroPerSec));
        b.w32(dest + TV_MICRO, static_cast<u32>(sys_us % kMicroPerSec));
    }

    static void expire_hook(void* ctx, u64 now_us) {
        of(ctx).expire(now_us);
    }

    static bool has_pending_hook(void* ctx) {
        return of(ctx).has_pending();
    }
};

namespace {

constexpr NativeVector kTimerVectors[] = {
    {TimerOps::open, "Open"},
    {TimerOps::close, "Close"},
    {nullptr, "Expunge"},
    {nullptr, "Reserved"},
    {TimerOps::begin_io, "BeginIO"},
    {TimerOps::abort_io, "AbortIO"},
    {TimerOps::add_time, "AddTime"},
    {TimerOps::sub_time, "SubTime"},
    {TimerOps::cmp_time, "CmpTime"},
    {TimerOps::read_eclock, "ReadEClock"},
    {TimerOps::get_sys_time, "GetSysTime"},
};

}

bool TimerDevice::init(Machine& m, Exec& exec) {
    m_ = &m;
    exec_ = &exec;
    base_ = 0;
    units_base_ = 0;
    sys_offset_us_ = 0;
    clock_fn_ = nullptr;
    clock_ctx_ = nullptr;
    queue_.clear();

    constexpr u32 pos_size = LIB_SIZE + kUnitCount * kUnitSize;
    base_ = exec.make_native_device("timer.device", kVersion, kTimerVectors,
                                    sizeof kTimerVectors / sizeof kTimerVectors[0], this, pos_size);
    if (base_ == 0) {
        m_ = nullptr;
        exec_ = nullptr;
        return false;
    }
    units_base_ = base_ + LIB_SIZE;

    Bus b(m);
    const u32 exec_base = exec.exec_base();
    if (exec_base != 0 && b.r32(exec_base + ex_EClockFrequency) == 0) {
        b.w32(exec_base + ex_EClockFrequency, kEClockFrequency);
    }
    if (!b.ok()) {
        m_ = nullptr;
        exec_ = nullptr;
        base_ = 0;
        return false;
    }
    exec.set_timer_hook(TimerHook{TimerOps::expire_hook, TimerOps::has_pending_hook, this});
    return true;
}

void TimerDevice::set_sys_time_us(u64 sys_us, u64 at_mono_us) {
    sys_offset_us_ = static_cast<i64>(sys_us) - static_cast<i64>(at_mono_us);
}

u64 TimerDevice::sys_time_us() const {
    const i64 val = static_cast<i64>(current_now_us()) + sys_offset_us_;
    return val > 0 ? static_cast<u64>(val) : 0;
}

void TimerDevice::set_clock(u64 (*now_us)(void* ctx), void* ctx) {
    clock_fn_ = now_us;
    clock_ctx_ = ctx;
}

u64 TimerDevice::current_now_us() const {
    if (clock_fn_ != nullptr) {
        return clock_fn_(clock_ctx_);
    }
    return exec_ != nullptr ? exec_->now_us() : 0;
}

void TimerDevice::expire(u64 now_us) {
    if (m_ == nullptr || exec_ == nullptr || m_->cpu.exit != m68k::ExitReason::None) {
        return;
    }
    TimerQueue::Entry entry{};
    while (queue_.pop_due(now_us, entry)) {
        Bus b(*m_);
        b.w8(entry.request + io_Error, 0);
        if (b.ok()) {
            exec_->reply_message(entry.request);
        }
        if (m_->cpu.exit != m68k::ExitReason::None) {
            break;
        }
    }
}

}
