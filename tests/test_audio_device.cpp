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

#include "libs/audio_device.hpp"
#include "check.hpp"

#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;

namespace {

constexpr u32 kCode = 0x1000;
constexpr u32 kSamples = 0x2000;  // up to 0x7800, every byte 100
constexpr u32 kSampleBytes = 6000;
constexpr u32 kStack = 0x8000;
constexpr u32 kTask = 0x9000;
constexpr u32 kOtherTask = 0x9100;
constexpr u32 kName = 0x9200;
constexpr u32 kRequests = 0x9400;
constexpr u32 kRequestStride = 0x80;
constexpr u32 kMasks = 0x9E00;
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;
constexpr usize kRegion = 1 << 20;

constexpr u32 kPeriod = 428;
constexpr u32 kNoWrite = 0;

Jit g_jit;
Exec g_exec;
AudioDevice g_audio;

class Rig {
public:
    Rig() : memory_(kGuestBytes) {
        fd_ = memfd_create("audio_device", 0);
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
        g_log = nullptr;
        CHECK(g_exec.init(m, kFreeStart, kFreeEnd));
        CHECK(g_audio.init(m, g_exec));
        make_task();
        port = g_exec.create_port();
        message_port = g_exec.create_port();
        CHECK(port != 0);
        CHECK(message_port != 0);
        str(kName, "audio.device");
        for (u32 i = 0; i < kSampleBytes; ++i) {
            put8(kSamples + i, 100);
        }
    }

    ~Rig() {
        g_exec.set_this_task(0);
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;
    u32 port = 0;          // where requests are replied
    u32 message_port = 0;  // where write messages are replied

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

    void str(u32 at, const char* s) {
        std::memcpy(&memory_[at], s, std::strlen(s) + 1);
    }

    ExitReason call_exec(u32 lvo) { return call_vec(g_exec.exec_base(), lvo); }
    ExitReason call_audio(u32 lvo) { return call_vec(g_audio.device_base(), lvo); }

private:
    void make_task() {
        for (u32 i = 0; i < TC_SIZE; ++i) {
            put8(kTask + i, 0);
        }
        put8(kTask + LN_TYPE, NT_TASK);
        put32(kTask + TC_SIGALLOC, 0x0000FFFF);
        g_exec.set_this_task(kTask);
    }

    ExitReason call_vec(u32 base, u32 lvo) {
        put16(kCode, 0x4EAE);
        put16(kCode + 2, static_cast<u16>(0x10000 - lvo));
        put16(kCode + 4, 0x4E75);
        g_jit.flush();
        m.cpu.a[6] = base;
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

// --- helpers to build and issue requests ------------------------------------

u32 request_at(u32 n) {
    return kRequests + n * kRequestStride;
}

// Request number n, cleared and set up the way every command needs.
u32 prepare(Rig& r, u32 n, u16 cmd, u32 unit, u16 key, u8 flags) {
    const u32 req = request_at(n);
    for (u32 i = 0; i < kRequestStride; ++i) {
        r.put8(req + i, 0);
    }
    r.put8(req + LN_TYPE, NT_MESSAGE);
    r.put32(req + mn_ReplyPort, r.port);
    r.put32(req + io_Device, g_audio.device_base());
    r.put16(req + io_Command, cmd);
    r.put32(req + io_Unit, unit);
    r.put8(req + io_Flags, flags);
    r.put16(req + ioa_AllocKey, key);
    return req;
}

void begin(Rig& r, u32 req) {
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_audio(dev_lvo::BeginIO), ExitReason::Returned);
}

u32 issue(Rig& r, u32 n, u16 cmd, u32 unit, u16 key, u8 flags = IOF_QUICK) {
    const u32 req = prepare(r, n, cmd, unit, key, flags);
    begin(r, req);
    return req;
}

u32 abort_io(Rig& r, u32 req) {
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_audio(dev_lvo::AbortIO), ExitReason::Returned);
    return r.m.cpu.d[0];
}

i8 error_of(const Rig& r, u32 req) {
    return static_cast<i8>(r.get8(req + io_Error));
}

bool replied(const Rig& r, u32 req) {
    return r.get8(req + LN_TYPE) == NT_REPLYMSG;
}

bool quick(const Rig& r, u32 req) {
    return (r.get8(req + io_Flags) & IOF_QUICK) != 0;
}

void put_masks(Rig& r, u32 req, u32 n, std::initializer_list<u8> masks) {
    const u32 list = kMasks + n * 0x10;
    u32 i = 0;
    for (const u8 mask : masks) {
        r.put8(list + i++, mask);
    }
    r.put8(req + LN_PRI, 0);
    r.put32(req + ioa_Data, list);
    r.put32(req + ioa_Length, static_cast<u32>(masks.size()));
}

struct Granted {
    u32 request;
    u32 unit;
    u16 key;
    i8 error;
};

Granted allocate(Rig& r, u32 n, i8 precedence, std::initializer_list<u8> masks, u16 key = 0,
                 u8 flags = IOF_QUICK) {
    const u32 req = prepare(r, n, ADCMD_ALLOCATE, 0, key, flags);
    put_masks(r, req, n, masks);
    r.put8(req + LN_PRI, static_cast<u8>(precedence));
    begin(r, req);
    return {req, r.get32(req + io_Unit), static_cast<u16>(r.get16(req + ioa_AllocKey)),
            error_of(r, req)};
}

u16 hold(Rig& r, u32 n, u8 channels, i8 precedence = 0) {
    const Granted g = allocate(r, n, precedence, {channels});
    CHECK_EQ(g.unit, static_cast<u32>(channels));
    CHECK_EQ(g.error, 0);
    return g.key;
}

u32 open_device(Rig& r, u32 n, i8 precedence, std::initializer_list<u8> masks) {
    const u32 req = prepare(r, n, 0, 0, 0, 0);
    put_masks(r, req, n, masks);
    r.put8(req + LN_PRI, static_cast<u8>(precedence));
    r.m.cpu.a[0] = kName;
    r.m.cpu.d[0] = 0;
    r.m.cpu.a[1] = req;
    r.m.cpu.d[1] = 0;
    CHECK_EQ(r.call_exec(exec_lvo::OpenDevice), ExitReason::Returned);
    return req;
}

void close_device(Rig& r, u32 req) {
    r.m.cpu.a[1] = req;
    CHECK_EQ(r.call_exec(exec_lvo::CloseDevice), ExitReason::Returned);
}

// CMD_WRITE of length bytes of kSamples on one channel.
u32 write(Rig& r, u32 n, u32 channels, u16 key, u32 length, u16 cycles,
          u8 flags = ADIOF_PERVOL) {
    const u32 req = prepare(r, n, CMD_WRITE, channels, key, flags);
    r.put32(req + ioa_Data, kSamples);
    r.put32(req + ioa_Length, length);
    r.put16(req + ioa_Period, kPeriod);
    r.put16(req + ioa_Volume, 64);
    r.put16(req + ioa_Cycles, cycles);
    r.put32(req + ioa_WriteMsg + mn_ReplyPort, r.message_port);
    begin(r, req);
    return req;
}

u32 pervol(Rig& r, u32 n, u32 channels, u16 key, u16 period, u16 volume, u8 flags = IOF_QUICK) {
    const u32 req = prepare(r, n, ADCMD_PERVOL, channels, key, flags);
    r.put16(req + ioa_Period, period);
    r.put16(req + ioa_Volume, volume);
    begin(r, req);
    return req;
}

// --- time ---------------------------------------------------------------------

u32 step_of(u32 period) {
    return static_cast<u32>((u64{3579545} << 16) / (u64{period} * 48000));
}

// The frames a voice needs to pass bytes bytes at the period.
u32 frames_for(u64 bytes, u32 period) {
    const u32 step = step_of(period);
    return static_cast<u32>(((bytes << 16) + step - 1) / step);
}

std::vector<i16> pump(u32 frames) {
    std::vector<i16> out(2 * static_cast<usize>(frames));
    CHECK_EQ(g_audio.pump(out.data(), frames), frames);
    return out;
}

// --- tests ------------------------------------------------------------------

void open_and_close_without_channels() {
    Rig r;
    const u32 base = g_audio.device_base();
    const u32 req = open_device(r, 0, 0, {});
    CHECK_EQ(error_of(r, req), 0);
    CHECK_EQ(r.get32(req + io_Device), base);
    CHECK_EQ(r.get32(req + io_Unit), 0u);
    CHECK(r.get16(req + ioa_AllocKey) != 0);
    CHECK_EQ(r.get16(base + LIB_OPENCNT), 1u);
    close_device(r, req);
    CHECK_EQ(r.get16(base + LIB_OPENCNT), 0u);
    CHECK(!g_audio.active());
}

void open_allocates_and_a_failed_open_stays_closed() {
    Rig r;
    const u32 base = g_audio.device_base();
    const u32 first = open_device(r, 0, 0, {0x3, 0x4});
    CHECK_EQ(error_of(r, first), 0);
    CHECK_EQ(r.get32(first + io_Unit), 3u);
    CHECK(r.get16(first + ioa_AllocKey) != 0);

    // 0x3 is held at the same precedence, which cannot be stolen.
    const u32 second = open_device(r, 1, 0, {0x3, 0xC});
    CHECK_EQ(r.get32(second + io_Unit), 0xCu);
    CHECK(r.get16(second + ioa_AllocKey) != r.get16(first + ioa_AllocKey));

    const u32 third = open_device(r, 2, 0, {0x3});
    CHECK_EQ(error_of(r, third), ADIOERR_ALLOCFAILED);
    CHECK_EQ(r.get32(third + io_Unit), 0u);
    CHECK_EQ(r.get32(third + io_Device), 0xFFFFFFFFu);
    CHECK_EQ(r.get16(base + LIB_OPENCNT), 2u);

    // Closing frees what the request's key holds.
    close_device(r, first);
    const u32 fourth = open_device(r, 3, 0, {0x3});
    CHECK_EQ(error_of(r, fourth), 0);
    CHECK_EQ(r.get32(fourth + io_Unit), 3u);
    CHECK_EQ(r.get16(base + LIB_OPENCNT), 2u);
}

void allocate_uses_keys_precedence_and_nowait() {
    Rig r;
    const Granted first = allocate(r, 0, 0, {1});
    CHECK_EQ(first.unit, 1u);
    CHECK_EQ(first.error, 0);
    CHECK(first.key != 0);
    CHECK(quick(r, first.request));
    CHECK(!replied(r, first.request));

    // The first mask that does not need a channel of equal precedence.
    const Granted second = allocate(r, 1, 0, {1, 2});
    CHECK_EQ(second.unit, 2u);
    CHECK(second.key != 0 && second.key != first.key);

    // The same key allocates more channels without a new key.
    const Granted more = allocate(r, 2, 0, {4}, first.key);
    CHECK_EQ(more.unit, 4u);
    CHECK_EQ(more.key, first.key);

    const Granted failed = allocate(r, 3, 0, {1}, 0, IOF_QUICK | ADIOF_NOWAIT);
    CHECK_EQ(failed.unit, 0u);
    CHECK_EQ(failed.error, ADIOERR_ALLOCFAILED);
    CHECK(quick(r, failed.request));
    CHECK(!replied(r, failed.request));

    // Without IOF_QUICK a request is replied even when it is answered at once.
    const Granted replied_now = allocate(r, 4, 0, {8}, 0, 0);
    CHECK_EQ(replied_now.unit, 8u);
    CHECK(replied(r, replied_now.request));

    // An empty array always succeeds, with no channel.
    const Granted none = allocate(r, 5, 0, {});
    CHECK_EQ(none.unit, 0u);
    CHECK_EQ(none.error, 0);
    CHECK(none.key != 0);
}

void allocate_prefers_the_combination_that_steals_the_least() {
    Rig r;
    const u16 low = hold(r, 0, 1, -5);
    const u16 high = hold(r, 1, 2, 3);
    const Granted g = allocate(r, 2, 10, {2, 1});
    CHECK_EQ(g.unit, 1u);  // channel 0 is held at the lower precedence
    CHECK(g.key != low && g.key != high);
    CHECK_EQ(error_of(r, issue(r, 3, CMD_UPDATE, 2, high)), 0);
    CHECK_EQ(error_of(r, issue(r, 4, CMD_UPDATE, 1, low)), ADIOERR_NOALLOCATION);
}

void a_waiting_allocation_is_replied_when_channels_are_freed() {
    Rig r;
    const u16 holder = hold(r, 0, 1);
    const Granted waiter = allocate(r, 1, 0, {1}, 0, IOF_QUICK);
    CHECK(!replied(r, waiter.request));
    CHECK(!quick(r, waiter.request));
    CHECK_EQ(waiter.unit, 0u);

    const u32 free_request = issue(r, 2, ADCMD_FREE, 1, holder);
    CHECK_EQ(error_of(r, free_request), 0);
    CHECK_EQ(r.get32(free_request + io_Unit), 1u);
    CHECK(replied(r, waiter.request));
    CHECK_EQ(error_of(r, waiter.request), 0);
    CHECK_EQ(r.get32(waiter.request + io_Unit), 1u);
    CHECK(r.get16(waiter.request + ioa_AllocKey) != 0);
    CHECK_EQ(g_exec.get_message(r.port), waiter.request);
}

void free_needs_the_right_key_and_changes_it() {
    Rig r;
    const u16 key = hold(r, 0, 3);
    const u32 wrong = issue(r, 1, ADCMD_FREE, 1, 0x4000);
    CHECK_EQ(error_of(r, wrong), ADIOERR_NOALLOCATION);
    CHECK_EQ(r.get32(wrong + io_Unit), 0u);

    const u32 ok = issue(r, 2, ADCMD_FREE, 1, key);
    CHECK_EQ(error_of(r, ok), 0);
    // The key no longer fits channel 0, but still fits channel 1.
    CHECK_EQ(error_of(r, issue(r, 3, CMD_UPDATE, 1, key)), ADIOERR_NOALLOCATION);
    CHECK_EQ(error_of(r, issue(r, 4, CMD_UPDATE, 2, key)), 0);
    // A mask with a channel the key does not fit does the others and reports the error.
    const u32 partial = issue(r, 5, ADCMD_FREE, 3, key);
    CHECK_EQ(error_of(r, partial), ADIOERR_NOALLOCATION);
    CHECK_EQ(r.get32(partial + io_Unit), 2u);
}

void setprec_lets_a_waiting_allocation_in() {
    Rig r;
    const u16 holder = hold(r, 0, 1, 10);
    const Granted waiter = allocate(r, 1, 5, {1}, 0, IOF_QUICK);
    CHECK(!replied(r, waiter.request));

    const u32 set = prepare(r, 2, ADCMD_SETPREC, 1, holder, IOF_QUICK);
    r.put8(set + LN_PRI, 0);
    begin(r, set);
    CHECK_EQ(error_of(r, set), 0);
    CHECK_EQ(r.get32(set + io_Unit), 1u);
    CHECK(replied(r, waiter.request));
    CHECK_EQ(r.get32(waiter.request + io_Unit), 1u);
    CHECK_EQ(error_of(r, issue(r, 3, CMD_UPDATE, 1, holder)), ADIOERR_NOALLOCATION);
}

void a_higher_precedence_steals_and_ends_what_the_victim_had() {
    Rig r;
    const u16 holder = hold(r, 0, 1);
    const u32 lock = issue(r, 1, ADCMD_LOCK, 1, holder);
    CHECK(!replied(r, lock));
    CHECK(!quick(r, lock));
    const u32 playing = write(r, 2, 1, holder, 1000, 0);
    const u32 waiting_cycle = issue(r, 3, ADCMD_WAITCYCLE, 1, holder);
    CHECK(!replied(r, waiting_cycle));

    const Granted thief = allocate(r, 4, 5, {1});
    CHECK_EQ(thief.unit, 1u);
    CHECK_EQ(thief.error, 0);
    CHECK(thief.key != holder);
    CHECK(replied(r, lock));
    CHECK_EQ(error_of(r, lock), ADIOERR_CHANNELSTOLEN);
    CHECK(replied(r, playing));
    CHECK_EQ(error_of(r, playing), IOERR_ABORTED);
    CHECK(replied(r, waiting_cycle));
    CHECK_EQ(error_of(r, waiting_cycle), IOERR_ABORTED);
    CHECK(!g_audio.mixer().playing(0));
    CHECK(!g_audio.active());

    CHECK_EQ(error_of(r, issue(r, 5, CMD_UPDATE, 1, holder)), ADIOERR_NOALLOCATION);
    CHECK_EQ(error_of(r, issue(r, 6, CMD_UPDATE, 1, thief.key)), 0);
    CHECK_EQ(error_of(r, write(r, 7, 1, holder, 100, 1, 0)), ADIOERR_NOALLOCATION);
}

void a_lock_is_replied_when_all_its_channels_are_freed() {
    Rig r;
    const u16 key = hold(r, 0, 3);
    const u32 wrong = issue(r, 1, ADCMD_LOCK, 3, 0x4000);
    CHECK_EQ(error_of(r, wrong), ADIOERR_NOALLOCATION);
    CHECK(quick(r, wrong));
    CHECK(!replied(r, wrong));

    const u32 lock = issue(r, 2, ADCMD_LOCK, 3, key);
    CHECK(!replied(r, lock));
    issue(r, 3, ADCMD_FREE, 1, key);
    CHECK(!replied(r, lock));
    CHECK_EQ(r.get32(lock + io_Unit), 2u);
    issue(r, 4, ADCMD_FREE, 2, key);
    CHECK(replied(r, lock));
    CHECK_EQ(error_of(r, lock), 0);
    CHECK_EQ(r.get32(lock + io_Unit), 0u);
}

void a_write_plays_and_is_replied_when_its_cycles_are_done() {
    Rig r;
    const u16 key = hold(r, 0, 3);
    const u32 req = write(r, 1, 3, key, 1000, 1, ADIOF_PERVOL | IOF_QUICK);
    // Of the channels in io_Unit the lowest one is written.
    CHECK_EQ(r.get32(req + io_Unit), 1u);
    CHECK(!quick(r, req));
    CHECK(g_audio.mixer().playing(0));
    CHECK(!g_audio.mixer().playing(1));
    CHECK(g_audio.active());
    CHECK(g_exec.has_pending_timers());

    const u32 frames = frames_for(1000, kPeriod);
    const std::vector<i16> out = pump(frames - 1);
    CHECK_EQ(out[0], 100 * 64 * 2);
    CHECK_EQ(out[1], 0);
    CHECK(!replied(r, req));
    pump(1);
    CHECK(replied(r, req));
    CHECK_EQ(error_of(r, req), 0);
    CHECK_EQ(g_exec.get_message(r.port), req);
    CHECK(!g_audio.active());
    CHECK(!g_exec.has_pending_timers());
    CHECK_EQ(pump(10)[0], 0);
}

void cycles_repeat_the_write() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 req = write(r, 1, 1, key, 1000, 3);
    const u32 frames = frames_for(3000, kPeriod);
    pump(frames - 1);
    CHECK(!replied(r, req));
    pump(1);
    CHECK(replied(r, req));
}

void a_write_needs_the_key_and_a_length() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 wrong = write(r, 1, 1, 0x4000, 100, 1, ADIOF_PERVOL | IOF_QUICK);
    CHECK_EQ(error_of(r, wrong), ADIOERR_NOALLOCATION);
    CHECK_EQ(r.get32(wrong + io_Unit), 0u);
    CHECK(quick(r, wrong));
    CHECK(!replied(r, wrong));

    // Without IOF_QUICK the error is replied.
    const u32 wrong_replied = write(r, 2, 1, 0x4000, 100, 1);
    CHECK(replied(r, wrong_replied));

    const u32 empty = write(r, 3, 1, key, 0, 1, ADIOF_PERVOL | IOF_QUICK);
    CHECK_EQ(error_of(r, empty), IOERR_BADLENGTH);
    CHECK(quick(r, empty));
    const u32 no_channel = write(r, 4, 0, key, 100, 1);
    CHECK_EQ(error_of(r, no_channel), ADIOERR_NOALLOCATION);
    CHECK(!g_audio.active());
}

void a_write_without_pervol_keeps_the_period_and_volume() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    write(r, 1, 1, key, 1000, 0);
    CHECK_EQ(g_audio.mixer().period(0), kPeriod);
    CHECK_EQ(g_audio.mixer().volume(0), 64u);
    pervol(r, 2, 1, key, 254, 10);
    // The next write does not load the period and volume in it.
    write(r, 3, 1, key, 1000, 0, 0);
    issue(r, 4, ADCMD_FINISH, 1, key);
    CHECK_EQ(g_audio.mixer().period(0), 254u);
    CHECK_EQ(g_audio.mixer().volume(0), 10u);
}

void writes_wait_for_the_mixer_and_are_fed_as_it_frees_up() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    u32 reqs[4];
    for (u32 i = 0; i < 4; ++i) {
        reqs[i] = write(r, 1 + i, 1, key, 500, 1);
    }
    u32 played = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 until = frames_for(500 * (i + 1), kPeriod);
        pump(until - played - 1);
        CHECK(!replied(r, reqs[i]));
        pump(1);
        CHECK(replied(r, reqs[i]));
        CHECK_EQ(error_of(r, reqs[i]), 0);
        played = until;
    }
    CHECK(!g_audio.active());
}

void a_write_message_is_replied_when_the_write_starts() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 first = write(r, 1, 1, key, 500, 1, ADIOF_PERVOL | ADIOF_WRITEMESSAGE);
    CHECK_EQ(g_exec.get_message(r.message_port), first + ioa_WriteMsg);
    CHECK_EQ(r.get8(first + ioa_WriteMsg + LN_TYPE), NT_REPLYMSG);

    // The second write starts when the first one is done, and not before.
    const u32 second = write(r, 2, 1, key, 500, 1, ADIOF_PERVOL | ADIOF_WRITEMESSAGE);
    const u32 third = write(r, 3, 1, key, 500, 1, ADIOF_PERVOL);
    pump(frames_for(500, kPeriod) - 1);
    CHECK_EQ(g_exec.get_message(r.message_port), kNoWrite);
    pump(1);
    CHECK_EQ(g_exec.get_message(r.message_port), second + ioa_WriteMsg);
    pump(frames_for(1500, kPeriod) - frames_for(500, kPeriod));
    CHECK(replied(r, second));
    CHECK(replied(r, third));
    CHECK_EQ(g_exec.get_message(r.message_port), kNoWrite);
}

void pervol_changes_now_or_at_the_end_of_the_cycle() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 idle = pervol(r, 1, 1, key, 300, 20);
    CHECK_EQ(error_of(r, idle), 0);
    CHECK_EQ(g_audio.mixer().period(0), PaulaMixer::kMinPeriod);  // no write in progress

    write(r, 2, 1, key, 1000, 0);
    const u32 now = pervol(r, 3, 1, key, 254, 10);
    CHECK_EQ(error_of(r, now), 0);
    CHECK_EQ(r.get32(now + io_Unit), 1u);
    CHECK_EQ(g_audio.mixer().period(0), 254u);
    CHECK_EQ(g_audio.mixer().volume(0), 10u);

    pervol(r, 4, 1, key, 200, 20, IOF_QUICK | ADIOF_SYNCCYCLE);
    CHECK_EQ(g_audio.mixer().period(0), 254u);
    pump(frames_for(1000, 254) - 1);
    CHECK_EQ(g_audio.mixer().period(0), 254u);
    pump(1);
    CHECK_EQ(g_audio.mixer().period(0), 200u);
    CHECK_EQ(g_audio.mixer().volume(0), 20u);

    CHECK_EQ(error_of(r, pervol(r, 5, 1, 0x4000, 100, 30)), ADIOERR_NOALLOCATION);
}

void finish_ends_the_write_now_or_at_the_end_of_the_cycle() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 first = write(r, 1, 1, key, 1000, 0);
    const u32 second = write(r, 2, 1, key, 1000, 0);
    pump(100);

    const u32 now = issue(r, 3, ADCMD_FINISH, 1, key);
    CHECK_EQ(error_of(r, now), 0);
    CHECK_EQ(r.get32(now + io_Unit), 1u);
    CHECK(replied(r, first));
    CHECK_EQ(error_of(r, first), IOERR_ABORTED);
    CHECK(!replied(r, second));
    CHECK_EQ(g_audio.mixer().byte_position(0), 0u);

    issue(r, 4, ADCMD_FINISH, 1, key, IOF_QUICK | ADIOF_SYNCCYCLE);
    pump(frames_for(1000, kPeriod) - 1);
    CHECK(!replied(r, second));
    pump(1);
    CHECK(replied(r, second));
    CHECK_EQ(error_of(r, second), IOERR_ABORTED);
    CHECK(!g_audio.active());

    CHECK_EQ(error_of(r, issue(r, 5, ADCMD_FINISH, 1, 0x4000)), ADIOERR_NOALLOCATION);
}

void waitcycle_is_replied_at_the_end_of_a_cycle() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 idle = issue(r, 1, ADCMD_WAITCYCLE, 1, key);
    CHECK_EQ(error_of(r, idle), 0);
    CHECK(quick(r, idle));
    CHECK(!replied(r, idle));

    write(r, 2, 1, key, 1000, 0);
    const u32 waiting = issue(r, 3, ADCMD_WAITCYCLE, 1, key);
    CHECK(!quick(r, waiting));
    CHECK(g_audio.active());
    pump(frames_for(1000, kPeriod) - 1);
    CHECK(!replied(r, waiting));
    pump(1);
    CHECK(replied(r, waiting));
    CHECK_EQ(error_of(r, waiting), 0);

    CHECK_EQ(error_of(r, issue(r, 4, ADCMD_WAITCYCLE, 1, 0x4000)), ADIOERR_NOALLOCATION);
}

void flush_aborts_every_write_and_wait_of_the_channel() {
    Rig r;
    const u16 key = hold(r, 0, 3);
    const u32 playing = write(r, 1, 1, key, 1000, 1);
    const u32 queued = write(r, 2, 1, key, 1000, 1);
    const u32 waiting = write(r, 3, 1, key, 1000, 1);
    const u32 cycle = issue(r, 4, ADCMD_WAITCYCLE, 1, key);
    const u32 other = write(r, 5, 2, key, 1000, 1);
    CHECK(g_audio.active());

    const u32 flush = issue(r, 6, CMD_FLUSH, 1, key);
    CHECK_EQ(error_of(r, flush), 0);
    for (const u32 req : {playing, queued, waiting, cycle}) {
        CHECK(replied(r, req));
        CHECK_EQ(error_of(r, req), IOERR_ABORTED);
    }
    CHECK(!replied(r, other));
    CHECK(!g_audio.mixer().playing(0));
    CHECK(g_audio.mixer().playing(1));

    CHECK_EQ(error_of(r, issue(r, 7, CMD_FLUSH, 1, 0x4000)), ADIOERR_NOALLOCATION);
}

void abortio_ends_queued_requests_of_every_kind() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 playing = write(r, 1, 1, key, 1000, 1);
    const u32 queued = write(r, 2, 1, key, 1000, 1);
    const u32 waiting = write(r, 3, 1, key, 1000, 1);
    const u32 cycle = issue(r, 4, ADCMD_WAITCYCLE, 1, key);

    CHECK_EQ(abort_io(r, waiting), 0u);
    CHECK(replied(r, waiting));
    CHECK_EQ(error_of(r, waiting), IOERR_ABORTED);
    CHECK_EQ(abort_io(r, cycle), 0u);
    CHECK_EQ(error_of(r, cycle), IOERR_ABORTED);
    CHECK_EQ(abort_io(r, queued), 0u);
    CHECK_EQ(error_of(r, queued), IOERR_ABORTED);
    CHECK(!g_audio.mixer().has_queued(0));
    CHECK(!replied(r, playing));
    CHECK_EQ(abort_io(r, queued), 0xFFFFFFFFu);  // it is not waiting any more

    const u32 next = write(r, 5, 1, key, 1000, 1);
    CHECK_EQ(abort_io(r, playing), 0u);
    CHECK(replied(r, playing));
    CHECK_EQ(error_of(r, playing), IOERR_ABORTED);
    CHECK(!replied(r, next));
    // The write that was queued behind it plays now, from its start.
    pump(frames_for(1000, kPeriod));
    CHECK(replied(r, next));
    CHECK_EQ(error_of(r, next), 0);

    const u32 lock = issue(r, 6, ADCMD_LOCK, 1, key);
    CHECK_EQ(abort_io(r, lock), 0u);
    CHECK(replied(r, lock));
    CHECK_EQ(error_of(r, lock), IOERR_ABORTED);
    const Granted waiter = allocate(r, 7, 0, {1}, 0, IOF_QUICK);
    CHECK(!replied(r, waiter.request));
    CHECK_EQ(abort_io(r, waiter.request), 0u);
    CHECK_EQ(error_of(r, waiter.request), IOERR_ABORTED);
}

void read_gives_the_write_in_progress() {
    Rig r;
    const u16 key = hold(r, 0, 3);
    const u32 none = issue(r, 1, CMD_READ, 3, key);
    CHECK_EQ(error_of(r, none), 0);
    CHECK_EQ(r.get32(none + io_Unit), 1u);
    CHECK_EQ(r.get32(none + ioa_Data), 0u);

    const u32 req = write(r, 2, 2, key, 1000, 1);
    const u32 read = issue(r, 3, CMD_READ, 2, key);
    CHECK_EQ(r.get32(read + ioa_Data), req);
    CHECK_EQ(r.get32(read + io_Unit), 2u);

    const u32 wrong = issue(r, 4, CMD_READ, 2, 0x4000);
    CHECK_EQ(error_of(r, wrong), ADIOERR_NOALLOCATION);
    CHECK_EQ(r.get32(wrong + io_Unit), 0u);
}

void stop_and_start_pause_and_resume_the_channel() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    CHECK_EQ(error_of(r, issue(r, 1, CMD_STOP, 1, key)), 0);
    // A stopped channel keeps its writes until it is started.
    const u32 first = write(r, 2, 1, key, 1000, 1);
    CHECK(!g_audio.mixer().playing(0));
    CHECK(g_audio.active());
    pump(200);
    CHECK(!replied(r, first));
    CHECK_EQ(error_of(r, issue(r, 3, CMD_START, 1, key)), 0);
    CHECK(g_audio.mixer().playing(0));
    pump(frames_for(1000, kPeriod));
    CHECK(replied(r, first));

    const u32 second = write(r, 4, 1, key, 1000, 1);
    pump(100);
    const u32 place = g_audio.mixer().byte_position(0);
    issue(r, 5, CMD_STOP, 1, key);
    pump(500);
    CHECK_EQ(g_audio.mixer().byte_position(0), place);
    CHECK(!replied(r, second));
    issue(r, 6, CMD_START, 1, key);
    pump(frames_for(1000, kPeriod) - 100);
    CHECK(replied(r, second));

    CHECK_EQ(error_of(r, issue(r, 7, CMD_STOP, 1, 0x4000)), ADIOERR_NOALLOCATION);
}

void reset_flushes_and_starts_a_stopped_channel() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 playing = write(r, 1, 1, key, 1000, 1);
    const u32 queued = write(r, 2, 1, key, 1000, 1);
    issue(r, 3, CMD_STOP, 1, key);
    CHECK(g_audio.mixer().paused(0));

    CHECK_EQ(error_of(r, issue(r, 4, CMD_RESET, 1, key)), 0);
    CHECK(replied(r, playing));
    CHECK_EQ(error_of(r, playing), IOERR_ABORTED);
    CHECK(replied(r, queued));
    CHECK(!g_audio.mixer().paused(0));
    CHECK(!g_audio.mixer().playing(0));

    const u32 req = write(r, 5, 1, key, 1000, 1);
    pump(frames_for(1000, kPeriod));
    CHECK(replied(r, req));
}

void update_and_clear_do_nothing_and_unknown_commands_fail() {
    Rig r;
    const u16 key = hold(r, 0, 3);
    for (const u16 cmd : {CMD_UPDATE, CMD_CLEAR}) {
        const u32 ok = issue(r, 1, cmd, 3, key);
        CHECK_EQ(error_of(r, ok), 0);
        CHECK_EQ(r.get32(ok + io_Unit), 3u);
        const u32 wrong = issue(r, 2, cmd, 3, 0x4000);
        CHECK_EQ(error_of(r, wrong), ADIOERR_NOALLOCATION);
        CHECK_EQ(r.get32(wrong + io_Unit), 0u);
    }
    const u32 unknown = issue(r, 3, 99, 3, key);
    CHECK_EQ(error_of(r, unknown), IOERR_NOCMD);
}

void quick_requests_are_replied_only_without_the_quick_flag() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 quick_update = issue(r, 1, CMD_UPDATE, 1, key, IOF_QUICK);
    CHECK(quick(r, quick_update));
    CHECK(!replied(r, quick_update));
    const u32 slow_update = issue(r, 2, CMD_UPDATE, 1, key, 0);
    CHECK(replied(r, slow_update));
    CHECK_EQ(g_exec.get_message(r.port), slow_update);
}

void requests_of_an_ended_task_are_dropped_without_a_reply() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    g_exec.set_this_task(kOtherTask);
    const u32 first = write(r, 1, 1, key, 1000, 1);
    const u32 second = write(r, 2, 1, key, 1000, 1);
    const u32 third = write(r, 3, 1, key, 1000, 1);
    const u32 cycle = issue(r, 4, ADCMD_WAITCYCLE, 1, key);
    g_exec.set_this_task(kTask);
    const u32 mine = write(r, 5, 1, key, 1000, 1);

    g_audio.drop_requests_of(kOtherTask);
    for (const u32 req : {first, second, third, cycle}) {
        CHECK(!replied(r, req));
    }
    // Mine was behind them and plays now, from its start.
    CHECK(g_audio.mixer().playing(0));
    CHECK(!g_audio.mixer().has_queued(0));
    pump(frames_for(1000, kPeriod));
    CHECK(replied(r, mine));
    CHECK(!replied(r, first));
    CHECK(!g_audio.active());
}

void the_exec_hook_reports_activity_and_drops_tasks() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    CHECK(!g_exec.has_pending_timers());
    g_exec.set_this_task(kOtherTask);
    const u32 req = write(r, 1, 1, key, 1000, 1);
    g_exec.set_this_task(kTask);
    CHECK(g_exec.has_pending_timers());

    g_audio.drop_requests_of(kOtherTask);
    CHECK(!replied(r, req));
    CHECK(!g_exec.has_pending_timers());
}

void pump_discard_lets_the_time_of_a_write_pass() {
    Rig r;
    const u16 key = hold(r, 0, 1);
    const u32 req = write(r, 1, 1, key, 1000, 1);
    const u32 frames = frames_for(1000, kPeriod);
    g_audio.pump_discard(frames - 1);
    CHECK(!replied(r, req));
    g_audio.pump_discard(1);
    CHECK(replied(r, req));
    CHECK_EQ(error_of(r, req), 0);
}

}  // namespace

int main() {
    open_and_close_without_channels();
    open_allocates_and_a_failed_open_stays_closed();
    allocate_uses_keys_precedence_and_nowait();
    allocate_prefers_the_combination_that_steals_the_least();
    a_waiting_allocation_is_replied_when_channels_are_freed();
    free_needs_the_right_key_and_changes_it();
    setprec_lets_a_waiting_allocation_in();
    a_higher_precedence_steals_and_ends_what_the_victim_had();
    a_lock_is_replied_when_all_its_channels_are_freed();
    a_write_plays_and_is_replied_when_its_cycles_are_done();
    cycles_repeat_the_write();
    a_write_needs_the_key_and_a_length();
    a_write_without_pervol_keeps_the_period_and_volume();
    writes_wait_for_the_mixer_and_are_fed_as_it_frees_up();
    a_write_message_is_replied_when_the_write_starts();
    pervol_changes_now_or_at_the_end_of_the_cycle();
    finish_ends_the_write_now_or_at_the_end_of_the_cycle();
    waitcycle_is_replied_at_the_end_of_a_cycle();
    flush_aborts_every_write_and_wait_of_the_channel();
    abortio_ends_queued_requests_of_every_kind();
    read_gives_the_write_in_progress();
    stop_and_start_pause_and_resume_the_channel();
    reset_flushes_and_starts_a_stopped_channel();
    update_and_clear_do_nothing_and_unknown_commands_fail();
    quick_requests_are_replied_only_without_the_quick_flag();
    requests_of_an_ended_task_are_dropped_without_a_reply();
    the_exec_hook_reports_activity_and_drops_tasks();
    pump_discard_lets_the_time_of_a_write_pass();
    return check::finish("test_audio_device");
}
