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

#include "../paula_mixer.hpp"
#include "../types.hpp"
#include "exec.hpp"

// audio.device implemented as a native Exec device on top of a PaulaMixer.
// The four channels of the device are the four voices of the mixer. Time
// passes only when pump or pump_discard is called: they render the mixer and
// turn what it reports into replies to the requests that wait for it.

namespace hombre {

namespace ndk {

// struct IOAudio (IORequest + ioa_*)
constexpr u32 ioa_AllocKey = 32;
constexpr u32 ioa_Data = 34;
constexpr u32 ioa_Length = 38;
constexpr u32 ioa_Period = 42;
constexpr u32 ioa_Volume = 44;
constexpr u32 ioa_Cycles = 46;
constexpr u32 ioa_WriteMsg = 48;
constexpr u32 IOA_SIZE = 68;

constexpr u32 ADHARD_CHANNELS = 4;

constexpr i8 ADALLOC_MINPREC = -128;

constexpr u16 ADCMD_FREE = CMD_NONSTD + 0;
constexpr u16 ADCMD_SETPREC = CMD_NONSTD + 1;
constexpr u16 ADCMD_FINISH = CMD_NONSTD + 2;
constexpr u16 ADCMD_PERVOL = CMD_NONSTD + 3;
constexpr u16 ADCMD_LOCK = CMD_NONSTD + 4;
constexpr u16 ADCMD_WAITCYCLE = CMD_NONSTD + 5;
constexpr u16 ADCMD_ALLOCATE = 32;

constexpr u8 ADIOF_PERVOL = 1 << 4;
constexpr u8 ADIOF_SYNCCYCLE = 1 << 5;
constexpr u8 ADIOF_NOWAIT = 1 << 6;
constexpr u8 ADIOF_WRITEMESSAGE = 1 << 7;

constexpr i8 ADIOERR_NOALLOCATION = -10;
constexpr i8 ADIOERR_ALLOCFAILED = -11;
constexpr i8 ADIOERR_CHANNELSTOLEN = -12;

}

class AudioDevice {
public:
    static constexpr u16 kVersion = 37;

    // Creates "audio.device" version 37 on ExecBase->DeviceList and registers
    // its hooks with exec. exec must be initialised.
    bool init(m68k::Machine& m, Exec& exec);

    u32 device_base() const { return base_; }
    const PaulaMixer& mixer() const { return mixer_; }

    // Renders frames stereo frames of 16-bit samples, left first, into out
    // and replies to the requests whose buffers started or finished on the
    // way. Plays silence when nothing is written. Returns the frames
    // rendered, which is frames once the device is initialised.
    u32 pump(i16* out, u32 frames);

    // Lets frames frames of time pass for a system without a sound card.
    void pump_discard(u32 frames);

    // True while a write is playing or waiting, or a request waits for the end
    // of a cycle, so that the system keeps calling pump.
    bool active() const;

    // Removes the requests the task issued, without replying.
    void drop_requests_of(u32 task);

private:
    friend struct AudioOps;

    static constexpr u32 kChannels = PaulaMixer::kVoices;
    static constexpr u32 kAllChannels = (1u << kChannels) - 1;
    static constexpr u32 kMaxWrites = 16;
    static constexpr u32 kMaxWaiters = 8;
    static constexpr u32 kMaxMasks = 16;
    // The mixer is rendered this many frames at a time, so that a write that
    // waits for the mixer is handed over soon after a slot frees up.
    static constexpr u32 kChunkFrames = 32;

    // What a stolen channel's writes and waits end with. The autodoc has
    // them end as aborted; only a lock ends with ADIOERR_CHANNELSTOLEN.
    static constexpr i8 kStolenError = ndk::IOERR_ABORTED;

    struct Request {
        u32 request = 0;
        u32 task = 0;
    };

    // A write, with what the mixer needs of it read when it was issued.
    struct Write : Request {
        PaulaMixer::Buffer buffer{};
        bool write_message = false;
    };

    // An allocation that waits for channels to be freed.
    struct Allocation : Request {
        u16 key = 0;
        i8 precedence = 0;
        u32 count = 0;
        u8 masks[kMaxMasks]{};
    };

    template <typename T, u32 N>
    struct List {
        T items[N]{};
        u32 count = 0;

        bool full() const { return count == N; }

        bool push(const T& item) {
            if (full()) {
                return false;
            }
            items[count++] = item;
            return true;
        }

        T take(u32 i) {
            const T item = items[i];
            for (u32 j = i + 1; j < count; ++j) {
                items[j - 1] = items[j];
            }
            --count;
            return item;
        }

        i32 find(u32 request) const {
            for (u32 i = 0; i < count; ++i) {
                if (items[i].request == request) {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        void remove_task(u32 task) {
            for (u32 i = 0; i < count;) {
                if (items[i].task == task) {
                    take(i);
                } else {
                    ++i;
                }
            }
        }
    };

    struct Channel {
        bool allocated = false;
        u16 key = 0;
        i8 precedence = 0;
        bool stopped = false;
        bool finishing = false;  // the write in progress ends with its cycle, aborted
        Request lock{};
        Write playing{};  // the write the mixer plays
        Write queued{};   // the write in the mixer's queue slot
        List<Write, kMaxWrites> writes{};  // writes the mixer has no room for yet
        List<Request, kMaxWaiters> cycle_waiters{};
    };

    u16 new_key();
    u32 matching(u32 mask, u16 key) const;
    bool allocate(Allocation& want, u32& granted);
    void retry_allocations();
    void free_channel(u32 channel);
    void steal_channel(u32 channel);
    void reset_channel(u32 channel, i8 error);
    void flush_channel(u32 channel, i8 error);
    void unlock_channel(u32 channel);
    void release_lock(u32 request);
    void feed(u32 channel);
    void announce_start(const Write& write);
    void finish_write(u32 channel, i8 error, bool reply);
    void handle_events(const PaulaMixer::Events& events, i8 finish_error, bool reply_finished);
    bool abort_request(u32 request);
    void complete(u32 request, i8 error);

    m68k::Machine* m_ = nullptr;
    Exec* exec_ = nullptr;
    u32 base_ = 0;
    u16 next_key_ = 0;
    PaulaMixer mixer_{};
    Channel channels_[kChannels]{};
    List<Allocation, kMaxWaiters> waiting_allocations_{};
};

}
