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

#include "audio_device.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;

namespace {

void fault_null(Bus& b) {
    b.bytes_at(0, b.machine().mem.size + 1);
}

// A fault on a request's memory belongs to the request, not to the run that
// happens to be executing when the device replies to it, so the run's state
// is put back when the guard ends.
class FaultGuard {
public:
    explicit FaultGuard(Machine& m)
        : cpu_(m.cpu), exit_(m.cpu.exit), fault_(m.cpu.fault_addr), trap_(m.cpu.trap_number) {
        cpu_.exit = m68k::ExitReason::None;
    }

    ~FaultGuard() {
        cpu_.exit = exit_;
        cpu_.fault_addr = fault_;
        cpu_.trap_number = trap_;
    }

    FaultGuard(const FaultGuard&) = delete;
    FaultGuard& operator=(const FaultGuard&) = delete;

private:
    m68k::CpuState& cpu_;
    m68k::ExitReason exit_;
    u32 fault_;
    u32 trap_;
};

// The lowest channel in the mask, PaulaMixer::kVoices when it is empty.
u32 lowest_channel(u32 mask) {
    u32 channel = 0;
    while (channel < PaulaMixer::kVoices && (mask & (1u << channel)) == 0) {
        ++channel;
    }
    return channel;
}

void put_allocation(Bus& b, u32 request, u32 channels, u16 key) {
    b.w32(request + io_Unit, channels);
    b.w16(request + ioa_AllocKey, key);
}

}

struct AudioOps {
    // What one request is being worked on with.
    struct Call {
        AudioDevice& a;
        Bus& b;
        u32 request;
        u8 flags;
        u32 mask;
        u16 key;
    };

    static AudioDevice& of(void* ctx) { return *static_cast<AudioDevice*>(ctx); }

    // Ends the request in the call, replying to it unless it was issued
    // with IOF_QUICK.
    static void done(Call& c, i8 error) {
        c.b.w8(c.request + io_Error, static_cast<u8>(error));
        if (c.b.ok() && (c.flags & IOF_QUICK) == 0) {
            c.a.exec_->reply_message(c.request);
        }
    }

    static void done_unit(Call& c, u32 channels, i8 error) {
        c.b.w32(c.request + io_Unit, channels);
        done(c, error);
    }

    // The request is answered later, so it is no longer a quick one.
    static void make_async(Call& c) {
        c.flags &= static_cast<u8>(~IOF_QUICK);
        c.b.w8(c.request + io_Flags, c.flags);
        c.b.w8(c.request + LN_TYPE, NT_MESSAGE);
    }

    static AudioDevice::Request issued_by_this_task(const Call& c) {
        AudioDevice::Request r;
        r.request = c.request;
        r.task = c.a.exec_->this_task();
        return r;
    }

    static bool read_allocation(Call& c, AudioDevice::Allocation& want) {
        want.request = c.request;
        want.task = c.a.exec_->this_task();
        want.key = c.key;
        want.precedence = static_cast<i8>(c.b.r8(c.request + LN_PRI));
        const u32 data = c.b.r32(c.request + ioa_Data);
        const u32 length = c.b.r32(c.request + ioa_Length);
        want.count = length < AudioDevice::kMaxMasks ? length : AudioDevice::kMaxMasks;
        for (u32 i = 0; i < want.count; ++i) {
            want.masks[i] = static_cast<u8>(c.b.r8(data + i));
        }
        return c.b.ok();
    }

    static void open(void* ctx, Machine& m) {
        AudioDevice& a = of(ctx);
        Bus b(m);
        const u32 request = native::guest_address(m, m.cpu.a[1]);
        const u32 dev = m.cpu.a[6];
        if (request == 0) {
            fault_null(b);
            return;
        }
        Call c{a, b, request, 0, 0, static_cast<u16>(b.r16(request + ioa_AllocKey))};
        AudioDevice::Allocation want;
        if (!read_allocation(c, want)) {
            return;
        }
        u32 granted = 0;
        if (!a.allocate(want, granted)) {
            b.w32(request + io_Unit, 0);
            b.w32(request + io_Device, 0xFFFFFFFF);
            b.w8(request + io_Error, static_cast<u8>(ADIOERR_ALLOCFAILED));
            if (b.ok()) {
                native::set_result(m, static_cast<u32>(static_cast<i32>(ADIOERR_ALLOCFAILED)));
            }
            return;
        }
        b.w16(dev + LIB_OPENCNT, b.r16(dev + LIB_OPENCNT) + 1);
        b.w8(dev + LIB_FLAGS, b.r8(dev + LIB_FLAGS) & ~LIBF_DELEXP);
        put_allocation(b, request, granted, want.key);
        b.w8(request + io_Error, 0);
        if (b.ok()) {
            native::set_result(m, 0);
        }
    }

    static void close(void* ctx, Machine& m) {
        AudioDevice& a = of(ctx);
        Bus b(m);
        const u32 request = native::guest_address(m, m.cpu.a[1]);
        const u32 dev = m.cpu.a[6];
        const u32 count = b.r16(dev + LIB_OPENCNT);
        if (count != 0) {
            b.w16(dev + LIB_OPENCNT, count - 1);
        }
        if (request != 0) {
            const u16 key = static_cast<u16>(b.r16(request + ioa_AllocKey));
            for (u32 channel = 0; channel < AudioDevice::kChannels; ++channel) {
                const AudioDevice::Channel& ch = a.channels_[channel];
                if (key != 0 && ch.allocated && ch.key == key) {
                    a.free_channel(channel);
                }
            }
            a.retry_allocations();
            b.w32(request + io_Unit, 0);
        }
        native::set_result(m, 0);
    }

    static void allocate_channels(Call& c) {
        AudioDevice::Allocation want;
        if (!read_allocation(c, want)) {
            return;
        }
        u32 granted = 0;
        if (c.a.allocate(want, granted)) {
            put_allocation(c.b, c.request, granted, want.key);
            done(c, 0);
            return;
        }
        c.b.w32(c.request + io_Unit, 0);
        if ((c.flags & ADIOF_NOWAIT) != 0 || c.a.waiting_allocations_.full()) {
            done(c, ADIOERR_ALLOCFAILED);
            return;
        }
        make_async(c);
        if (c.b.ok()) {
            c.a.waiting_allocations_.push(want);
        }
    }

    // A lock takes all of its channels or none, and is answered when they
    // are freed or stolen.
    static void lock_channels(Call& c, u32 channels) {
        make_async(c);
        c.b.w32(c.request + io_Unit, channels);
        if (!c.b.ok()) {
            return;
        }
        for (u32 channel = 0; channel < AudioDevice::kChannels; ++channel) {
            if ((channels & (1u << channel)) != 0) {
                c.a.unlock_channel(channel);
                c.a.channels_[channel].lock = issued_by_this_task(c);
            }
        }
    }

    static void apply(Call& c, u16 cmd, u32 channel, i8 precedence, u16 period, u16 volume) {
        AudioDevice& a = c.a;
        AudioDevice::Channel& ch = a.channels_[channel];
        const bool at_cycle_end = (c.flags & ADIOF_SYNCCYCLE) != 0;
        switch (cmd) {
        case ADCMD_FREE:
            a.free_channel(channel);
            break;
        case ADCMD_SETPREC:
            ch.precedence = precedence;
            break;
        case ADCMD_FINISH:
            if (ch.playing.request == 0) {
                break;
            }
            if (at_cycle_end) {
                ch.finishing = true;
                a.mixer_.finish_at_cycle_end(channel);
            } else {
                a.finish_write(channel, IOERR_ABORTED, true);
            }
            break;
        case ADCMD_PERVOL:
            if (ch.playing.request != 0) {
                a.mixer_.set_period_volume(channel, period, volume, at_cycle_end);
            }
            break;
        case CMD_RESET:
            a.reset_channel(channel, IOERR_ABORTED);
            break;
        case CMD_STOP:
            ch.stopped = true;
            a.mixer_.pause(1u << channel);
            break;
        case CMD_START:
            ch.stopped = false;
            a.mixer_.resume(1u << channel);
            a.feed(channel);
            break;
        case CMD_FLUSH:
            a.flush_channel(channel, IOERR_ABORTED);
            break;
        default:  // CMD_UPDATE and CMD_CLEAR have nothing to do
            break;
        }
    }

    // The commands that work on every channel of io_Unit the allocation key
    // fits and report the ones they worked on in io_Unit.
    static void multi_channel(Call& c, u16 cmd) {
        u32 hit = c.a.matching(c.mask, c.key);
        const i8 error = hit == c.mask ? 0 : ADIOERR_NOALLOCATION;
        const i8 precedence = static_cast<i8>(c.b.r8(c.request + LN_PRI));
        const u16 period = static_cast<u16>(c.b.r16(c.request + ioa_Period));
        const u16 volume = static_cast<u16>(c.b.r16(c.request + ioa_Volume));
        if (!c.b.ok()) {
            return;
        }
        if (cmd == ADCMD_LOCK) {
            if (error == 0 && hit != 0) {
                lock_channels(c, hit);
                return;
            }
            hit = 0;
        }
        for (u32 channel = 0; channel < AudioDevice::kChannels; ++channel) {
            if ((hit & (1u << channel)) != 0) {
                apply(c, cmd, channel, precedence, period, volume);
            }
        }
        if (cmd == ADCMD_FREE || cmd == ADCMD_SETPREC) {
            c.a.retry_allocations();
        }
        done_unit(c, hit, error);
    }

    static void write(Call& c, u32 channel) {
        AudioDevice& a = c.a;
        AudioDevice::Write w;
        w.request = c.request;
        w.task = a.exec_->this_task();
        w.buffer.address = c.b.r32(c.request + ioa_Data);
        w.buffer.length = c.b.r32(c.request + ioa_Length);
        w.buffer.cycles = c.b.r16(c.request + ioa_Cycles);
        w.buffer.period = static_cast<u16>(c.b.r16(c.request + ioa_Period));
        w.buffer.volume = static_cast<u16>(c.b.r16(c.request + ioa_Volume));
        w.buffer.keep_voice_settings = (c.flags & ADIOF_PERVOL) == 0;
        w.buffer.tag = c.request;
        w.write_message = (c.flags & ADIOF_WRITEMESSAGE) != 0;
        if (!c.b.ok()) {
            return;
        }
        AudioDevice::Channel& ch = a.channels_[channel];
        if (w.buffer.length < 2) {
            done(c, IOERR_BADLENGTH);
            return;
        }
        if (ch.writes.full()) {
            LogLine().text("audio.device: write queue full").send();
            done(c, IOERR_ABORTED);
            return;
        }
        make_async(c);
        if (c.b.ok()) {
            ch.writes.push(w);
            a.feed(channel);
        }
    }

    static void wait_cycle(Call& c, u32 channel) {
        AudioDevice::Channel& ch = c.a.channels_[channel];
        if (ch.playing.request == 0) {
            done(c, 0);
            return;
        }
        if (ch.cycle_waiters.full()) {
            done(c, IOERR_ABORTED);
            return;
        }
        make_async(c);
        if (c.b.ok()) {
            ch.cycle_waiters.push(issued_by_this_task(c));
        }
    }

    // The commands that work on the one channel io_Unit names.
    static void single_channel(Call& c, u16 cmd) {
        const u32 channel = lowest_channel(c.mask);
        if (channel >= AudioDevice::kChannels || c.a.matching(1u << channel, c.key) == 0) {
            done_unit(c, 0, ADIOERR_NOALLOCATION);
            return;
        }
        c.b.w32(c.request + io_Unit, 1u << channel);
        if (cmd == CMD_WRITE) {
            write(c, channel);
        } else if (cmd == ADCMD_WAITCYCLE) {
            wait_cycle(c, channel);
        } else {
            c.b.w32(c.request + ioa_Data, c.a.channels_[channel].playing.request);
            done(c, 0);
        }
    }

    static void begin_io(void* ctx, Machine& m) {
        Bus b(m);
        const u32 request = native::guest_address(m, m.cpu.a[1]);
        if (request == 0) {
            fault_null(b);
            return;
        }
        const u16 cmd = static_cast<u16>(b.r16(request + io_Command));
        Call c{of(ctx), b, request, static_cast<u8>(b.r8(request + io_Flags)),
               b.r32(request + io_Unit) & AudioDevice::kAllChannels,
               static_cast<u16>(b.r16(request + ioa_AllocKey))};
        if (!b.ok()) {
            return;
        }
        b.w8(request + io_Error, 0);
        switch (cmd) {
        case ADCMD_ALLOCATE:
            allocate_channels(c);
            break;
        case CMD_WRITE:
        case CMD_READ:
        case ADCMD_WAITCYCLE:
            single_channel(c, cmd);
            break;
        case ADCMD_FREE:
        case ADCMD_SETPREC:
        case ADCMD_FINISH:
        case ADCMD_PERVOL:
        case ADCMD_LOCK:
        case CMD_RESET:
        case CMD_UPDATE:
        case CMD_CLEAR:
        case CMD_STOP:
        case CMD_START:
        case CMD_FLUSH:
            multi_channel(c, cmd);
            break;
        default:
            done(c, IOERR_NOCMD);
            break;
        }
    }

    static void abort_io(void* ctx, Machine& m) {
        Bus b(m);
        const u32 request = native::guest_address(m, m.cpu.a[1]);
        if (request == 0) {
            fault_null(b);
            return;
        }
        native::set_result(m, of(ctx).abort_request(request) ? 0 : static_cast<u32>(-1));
    }

    // Sample bytes outside guest memory are silence.
    static i8 read_sample(void* ctx, u32 address) {
        const Machine& m = *of(ctx).m_;
        u32 value = 0;
        return m.mem.read8(native::guest_address(m, address), value) ? static_cast<i8>(value) : 0;
    }

    // The mixer is driven by pump, so there is nothing for the timer to do.
    static void expire_hook(void*, u64) {}

    static bool has_pending_hook(void* ctx) { return of(ctx).active(); }

    static void drop_hook(void* ctx, u32 task) { of(ctx).drop_requests_of(task); }
};

namespace {

constexpr NativeVector kAudioVectors[] = {
    {AudioOps::open, "Open"},
    {AudioOps::close, "Close"},
    {nullptr, "Expunge"},
    {nullptr, "Reserved"},
    {AudioOps::begin_io, "BeginIO"},
    {AudioOps::abort_io, "AbortIO"},
};

}

bool AudioDevice::init(Machine& m, Exec& exec) {
    m_ = &m;
    exec_ = &exec;
    next_key_ = 0;
    mixer_ = PaulaMixer{};
    mixer_.set_reader(AudioOps::read_sample, this);
    for (Channel& ch : channels_) {
        ch = Channel{};
    }
    waiting_allocations_ = List<Allocation, kMaxWaiters>{};

    base_ = exec.make_native_device("audio.device", kVersion, kAudioVectors,
                                    sizeof kAudioVectors / sizeof kAudioVectors[0], this);
    if (base_ == 0 ||
        !exec.add_timer_hook(
            TimerHook{AudioOps::expire_hook, AudioOps::has_pending_hook, this}) ||
        !exec.add_timer_drop_hook(AudioOps::drop_hook, this)) {
        m_ = nullptr;
        exec_ = nullptr;
        base_ = 0;
        return false;
    }
    return true;
}

u32 AudioDevice::pump(i16* out, u32 frames) {
    if (m_ == nullptr) {
        return 0;
    }
    u32 done = 0;
    while (done < frames) {
        PaulaMixer::Events events;
        const u32 left = frames - done;
        done += mixer_.render(out + 2 * done, left < kChunkFrames ? left : kChunkFrames, events);
        handle_events(events, 0, true);
    }
    return done;
}

void AudioDevice::pump_discard(u32 frames) {
    i16 scratch[2 * kChunkFrames];
    while (m_ != nullptr && frames > 0) {
        frames -= pump(scratch, frames < kChunkFrames ? frames : kChunkFrames);
    }
}

bool AudioDevice::active() const {
    for (const Channel& ch : channels_) {
        if (ch.playing.request != 0 || ch.queued.request != 0 || ch.writes.count > 0 ||
            ch.cycle_waiters.count > 0) {
            return true;
        }
    }
    return false;
}

void AudioDevice::drop_requests_of(u32 task) {
    if (m_ == nullptr) {
        return;
    }
    waiting_allocations_.remove_task(task);
    for (u32 channel = 0; channel < kChannels; ++channel) {
        Channel& ch = channels_[channel];
        ch.cycle_waiters.remove_task(task);
        ch.writes.remove_task(task);
        if (ch.lock.request != 0 && ch.lock.task == task) {
            release_lock(ch.lock.request);
        }
        if (ch.queued.request != 0 && ch.queued.task == task) {
            mixer_.drop_queued(channel);
            ch.queued = Write{};
        }
        if (ch.playing.request != 0 && ch.playing.task == task) {
            finish_write(channel, 0, false);
        }
    }
}

u16 AudioDevice::new_key() {
    if (++next_key_ == 0) {
        ++next_key_;
    }
    return next_key_;
}

u32 AudioDevice::matching(u32 mask, u16 key) const {
    u32 hit = 0;
    for (u32 channel = 0; channel < kChannels; ++channel) {
        const Channel& ch = channels_[channel];
        if ((mask & (1u << channel)) != 0 && ch.allocated && ch.key == key) {
            hit |= 1u << channel;
        }
    }
    return hit;
}

// Of the combinations that need no channel of equal or higher precedence,
// takes the one that steals the lowest precedence, the first of those that
// steal the same. An empty array always succeeds with no channels.
bool AudioDevice::allocate(Allocation& want, u32& granted) {
    constexpr i32 kNothingStolen = ndk::ADALLOC_MINPREC - 1;
    constexpr i32 kNoCandidate = 0x7FFF;
    granted = 0;
    i32 best_level = kNoCandidate;
    for (u32 i = 0; i < want.count; ++i) {
        const u32 mask = want.masks[i] & kAllChannels;
        i32 level = kNothingStolen;
        bool possible = mask != 0;
        for (u32 channel = 0; channel < kChannels && possible; ++channel) {
            const Channel& ch = channels_[channel];
            const bool ours = want.key != 0 && ch.key == want.key;
            if ((mask & (1u << channel)) == 0 || !ch.allocated || ours) {
                continue;
            }
            possible = ch.precedence < want.precedence;
            level = ch.precedence > level ? ch.precedence : level;
        }
        if (possible && level < best_level) {
            best_level = level;
            granted = mask;
        }
    }
    if (granted == 0 && want.count != 0) {
        return false;
    }
    if (want.key == 0) {
        want.key = new_key();
    }
    for (u32 channel = 0; channel < kChannels; ++channel) {
        if ((granted & (1u << channel)) == 0) {
            continue;
        }
        Channel& ch = channels_[channel];
        if (ch.allocated && ch.key != want.key) {
            steal_channel(channel);
        } else {
            reset_channel(channel, ndk::IOERR_ABORTED);
        }
        ch.allocated = true;
        ch.key = want.key;
        ch.precedence = want.precedence;
    }
    return true;
}

void AudioDevice::retry_allocations() {
    for (u32 i = 0; i < waiting_allocations_.count;) {
        Allocation want = waiting_allocations_.items[i];
        u32 granted = 0;
        if (!allocate(want, granted)) {
            ++i;
            continue;
        }
        waiting_allocations_.take(i);
        {
            const FaultGuard guard(*m_);
            Bus b(*m_);
            put_allocation(b, want.request, granted, want.key);
        }
        complete(want.request, 0);
    }
}

// Freeing changes the key, so that the one who held the channel cannot use
// it any more.
void AudioDevice::free_channel(u32 channel) {
    reset_channel(channel, ndk::IOERR_ABORTED);
    unlock_channel(channel);
    Channel& ch = channels_[channel];
    ch.allocated = false;
    ch.key = new_key();
    ch.precedence = ndk::ADALLOC_MINPREC;
}

void AudioDevice::steal_channel(u32 channel) {
    const u32 lock = channels_[channel].lock.request;
    if (lock != 0) {
        release_lock(lock);
        complete(lock, ndk::ADIOERR_CHANNELSTOLEN);
    }
    reset_channel(channel, kStolenError);
}

void AudioDevice::reset_channel(u32 channel, i8 error) {
    flush_channel(channel, error);
    channels_[channel].stopped = false;
    mixer_.resume(1u << channel);
    mixer_.set_period_volume(channel, PaulaMixer::kMinPeriod, 0);
}

void AudioDevice::flush_channel(u32 channel, i8 error) {
    Channel& ch = channels_[channel];
    const u32 playing = ch.playing.request;
    const u32 queued = ch.queued.request;
    mixer_.stop(channel);
    ch.playing = Write{};
    ch.queued = Write{};
    ch.finishing = false;
    if (playing != 0) {
        complete(playing, error);
    }
    if (queued != 0) {
        complete(queued, error);
    }
    while (ch.writes.count > 0) {
        complete(ch.writes.take(0).request, error);
    }
    while (ch.cycle_waiters.count > 0) {
        complete(ch.cycle_waiters.take(0).request, error);
    }
}

// Takes the channel out of its lock request, which is replied when no
// channel is left in it.
void AudioDevice::unlock_channel(u32 channel) {
    const u32 lock = channels_[channel].lock.request;
    if (lock == 0) {
        return;
    }
    channels_[channel].lock = Request{};
    u32 left = 0;
    bool readable = false;
    {
        const FaultGuard guard(*m_);
        Bus b(*m_);
        left = b.r32(lock + io_Unit) & ~(1u << channel);
        b.w32(lock + io_Unit, left);
        readable = b.ok();
    }
    if (readable && left == 0) {
        complete(lock, 0);
    }
}

void AudioDevice::release_lock(u32 request) {
    for (Channel& ch : channels_) {
        if (ch.lock.request == request) {
            ch.lock = Request{};
        }
    }
}

// Hands waiting writes to the mixer while it has room. A stopped channel
// that plays nothing starts nothing until it is started.
void AudioDevice::feed(u32 channel) {
    if (channel >= kChannels) {
        return;
    }
    Channel& ch = channels_[channel];
    while (ch.writes.count > 0 && !mixer_.has_queued(channel) &&
           (!ch.stopped || mixer_.playing(channel))) {
        const bool starts_now = !mixer_.playing(channel);
        if (!mixer_.queue(channel, ch.writes.items[0].buffer)) {
            return;
        }
        const Write next = ch.writes.take(0);
        if (starts_now) {
            ch.playing = next;
            announce_start(next);
        } else {
            ch.queued = next;
        }
    }
}

void AudioDevice::announce_start(const Write& write) {
    if (write.write_message) {
        exec_->reply_timer_message(write.request + ioa_WriteMsg);
    }
}

void AudioDevice::finish_write(u32 channel, i8 error, bool reply) {
    PaulaMixer::Events events;
    mixer_.finish(channel, events);
    handle_events(events, error, reply);
}

void AudioDevice::handle_events(const PaulaMixer::Events& events, i8 finish_error,
                                bool reply_finished) {
    for (u32 i = 0; i < events.count; ++i) {
        const PaulaMixer::Event& event = events.items[i];
        Channel& ch = channels_[event.voice];
        if (event.kind == PaulaMixer::EventKind::Started) {
            ch.playing = ch.queued;
            ch.queued = Write{};
            announce_start(ch.playing);
            continue;
        }
        while (ch.cycle_waiters.count > 0) {
            complete(ch.cycle_waiters.take(0).request, 0);
        }
        if (event.kind == PaulaMixer::EventKind::Finished) {
            const u32 request = ch.playing.request;
            const i8 error = ch.finishing ? ndk::IOERR_ABORTED : finish_error;
            ch.playing = Write{};
            ch.finishing = false;
            if (reply_finished && request != 0) {
                complete(request, error);
            }
        }
    }
    for (u32 channel = 0; channel < kChannels; ++channel) {
        feed(channel);
    }
}

bool AudioDevice::abort_request(u32 request) {
    i32 at = waiting_allocations_.find(request);
    if (at >= 0) {
        waiting_allocations_.take(static_cast<u32>(at));
        complete(request, ndk::IOERR_ABORTED);
        return true;
    }
    for (u32 channel = 0; channel < kChannels; ++channel) {
        Channel& ch = channels_[channel];
        if (ch.lock.request == request) {
            release_lock(request);
        } else if ((at = ch.cycle_waiters.find(request)) >= 0) {
            ch.cycle_waiters.take(static_cast<u32>(at));
        } else if ((at = ch.writes.find(request)) >= 0) {
            ch.writes.take(static_cast<u32>(at));
        } else if (ch.queued.request == request) {
            mixer_.drop_queued(channel);
            ch.queued = Write{};
        } else if (ch.playing.request == request) {
            finish_write(channel, ndk::IOERR_ABORTED, true);
            return true;
        } else {
            continue;
        }
        complete(request, ndk::IOERR_ABORTED);
        feed(channel);
        return true;
    }
    return false;
}

void AudioDevice::complete(u32 request, i8 error) {
    const FaultGuard guard(*m_);
    Bus b(*m_);
    b.w8(request + io_Error, static_cast<u8>(error));
    if (!b.ok()) {
        LogLine().text("audio.device: cannot reply to the request at ").hex(request).send();
        return;
    }
    exec_->reply_timer_message(request);
}

}
