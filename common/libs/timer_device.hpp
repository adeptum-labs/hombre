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

#include "../types.hpp"
#include "exec.hpp"

#include <vector>

// timer.device implemented as a native Exec device backed by a host-side
// deadline-ordered TimerQueue and the monotonic microsecond clock.

namespace hombre {

namespace timer_lvo {

constexpr u32 AddTime = 42;
constexpr u32 SubTime = 48;
constexpr u32 CmpTime = 54;
constexpr u32 ReadEClock = 60;
constexpr u32 GetSysTime = 66;

}

namespace ndk {

// struct timerequest (IORequest + tr_time)
constexpr u32 TR_TIME = 32;
constexpr u32 tv_secs = 32;
constexpr u32 tv_micro = 36;
constexpr u32 ev_hi = 32;
constexpr u32 ev_lo = 36;
constexpr u32 TR_SIZE = 40;

// Offsets within struct timeval / struct EClockVal
constexpr u32 TV_SECS = 0;
constexpr u32 TV_MICRO = 4;
constexpr u32 TV_SIZE = 8;
constexpr u32 EV_HI = 0;
constexpr u32 EV_LO = 4;

constexpr u32 UNIT_MICROHZ = 0;
constexpr u32 UNIT_VBLANK = 1;
constexpr u32 UNIT_ECLOCK = 2;
constexpr u32 UNIT_WAITUNTIL = 3;
constexpr u32 UNIT_WAITECLOCK = 4;

constexpr u16 TR_ADDREQUEST = 9;
constexpr u16 TR_GETSYSTIME = 10;
constexpr u16 TR_SETSYSTIME = 11;

constexpr u32 kEClockFrequency = 709379;

}

// Fixed-capacity deadline-ordered queue of pending timer requests.
// Holds no guest pointers so it can be unit tested directly on the host.
class TimerQueue {
public:
    static constexpr u32 kCapacity = 4096;

    struct Entry {
        u64 deadline_us = 0;
        u32 request = 0;
        u32 task = 0;
    };

    void clear() { entries_.clear(); }
    bool empty() const { return entries_.empty(); }
    bool full() const { return entries_.size() >= kCapacity; }
    u32 count() const { return static_cast<u32>(entries_.size()); }
    u64 next_deadline() const { return !entries_.empty() ? entries_.front().deadline_us : 0; }
    const Entry& entry(u32 i) const { return entries_[i]; }

    // Inserts (deadline_us, request, task) in ascending deadline order (FIFO
    // for equal deadlines). False when the queue is full.
    bool insert(u64 deadline_us, u32 request, u32 task = 0) {
        if (full()) {
            return false;
        }
        auto at = entries_.begin();
        while (at != entries_.end() && at->deadline_us <= deadline_us) {
            ++at;
        }
        entries_.insert(at, Entry{deadline_us, request, task});
        return true;
    }

    // Removes every entry of task.
    void remove_task(u32 task) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->task == task) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Removes the entry matching request. False if not present.
    bool remove(u32 request) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->request == request) {
                entries_.erase(it);
                return true;
            }
        }
        return false;
    }

    // Pops the earliest entry if its deadline_us <= now_us.
    bool pop_due(u64 now_us, Entry& out) {
        if (entries_.empty() || entries_.front().deadline_us > now_us) {
            return false;
        }
        out = entries_.front();
        entries_.erase(entries_.begin());
        return true;
    }

private:
    std::vector<Entry> entries_;
};

class TimerDevice {
public:
    static constexpr u16 kVersion = 40;

    // Creates "timer.device" version 40 on ExecBase->DeviceList and registers
    // its expiry and task-drop hooks with exec. exec must be initialised.
    bool init(m68k::Machine& m, Exec& exec);

    u32 device_base() const { return base_; }
    bool has_pending() const { return !queue_.empty(); }
    const TimerQueue& queue() const { return queue_; }

    // Sets the system time offset so that sys_time_us() equals `sys_us` at
    // monotonic timestamp `at_mono_us`.
    void set_sys_time_us(u64 sys_us, u64 at_mono_us);

    // Current system time in microseconds since 1978-01-01.
    u64 sys_time_us() const;

    // Overrides the monotonic microsecond clock source (defaults to exec's
    // TaskHost::now_us).
    void set_clock(u64 (*now_us)(void* ctx), void* ctx = nullptr);

    // Removes the pending requests the task issued, without replying.
    void drop_requests_of(u32 task) { queue_.remove_task(task); }

    // Replies to every queued request whose deadline_us <= now_us.
    void expire(u64 now_us);

private:
    friend struct TimerOps;

    static constexpr u32 kUnitCount = 5;
    static constexpr u32 kUnitSize = 8;

    u64 current_now_us() const;

    m68k::Machine* m_ = nullptr;
    Exec* exec_ = nullptr;
    u32 base_ = 0;
    u32 units_base_ = 0;
    i64 sys_offset_us_ = 0;
    u64 (*clock_fn_)(void* ctx) = nullptr;
    void* clock_ctx_ = nullptr;
    TimerQueue queue_{};
};

}
