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

#include "types.hpp"

// A four-voice mixer that plays 8-bit samples the way the Amiga's Paula chip
// does and renders them as 16-bit signed stereo at 48000 Hz. Voices 0 and 3
// go to the left channel and voices 1 and 2 to the right one, as on the
// Amiga. Sample bytes are fetched through a callback, so the mixer holds no
// guest pointers and renders the same output for the same calls every time.

namespace hombre {

class PaulaMixer {
public:
    static constexpr u32 kVoices = 4;
    static constexpr u32 kOutputRate = 48000;
    static constexpr u32 kPaulaClock = 3579545;
    static constexpr u16 kMinPeriod = 124;
    static constexpr u16 kMaxVolume = 64;

    // A voice crosses at most one buffer boundary per frame, and a boundary
    // gives two events: the end of one buffer and the start of the next.
    static constexpr u32 kEventsPerFrame = kVoices * 2;

    using ReadFn = i8 (*)(void* ctx, u32 address);

    // One waveform. A cycles value of 0 plays it forever. When
    // keep_voice_settings is set the voice keeps the period and volume it
    // has, instead of loading the ones given here when the buffer starts.
    struct Buffer {
        u32 address = 0;
        u32 length = 0;
        u16 period = kMinPeriod;
        u16 volume = 0;
        u32 cycles = 1;
        u32 tag = 0;
        bool keep_voice_settings = false;
    };

    // Started: a queued buffer became the current one. Only render and
    // finish report that; queue starts an idle voice without an event, as
    // its caller can see that for itself. Cycle: one cycle of a buffer that
    // goes on is done. Finished: the last cycle of a buffer is done.
    enum class EventKind : u8 { Started, Cycle, Finished };

    struct Event {
        u8 voice = 0;
        EventKind kind = EventKind::Started;
        u32 tag = 0;
    };

    struct Events {
        static constexpr u32 kCapacity = 32;

        Event items[kCapacity]{};
        u32 count = 0;

        void clear() { count = 0; }

        void add(u32 voice, EventKind kind, u32 tag) {
            if (count < kCapacity) {
                items[count++] = Event{static_cast<u8>(voice), kind, tag};
            }
        }
    };

    void set_reader(ReadFn read, void* ctx) {
        read_ = read;
        read_ctx_ = ctx;
    }

    // Plays buffer at once on an idle voice and otherwise keeps it as the
    // queued buffer. False when the voice already has a queued buffer, or
    // the buffer is shorter than one sample word. An odd length loses its
    // last byte.
    bool queue(u32 voice, const Buffer& buffer) {
        if (voice >= kVoices || buffer.length < 2) {
            return false;
        }
        Voice& v = voices_[voice];
        Buffer even = buffer;
        even.length &= ~1u;
        if (!v.playing) {
            v.current = even;
            v.position = 0;
            begin(v);
            return true;
        }
        if (v.has_queued) {
            return false;
        }
        v.queued = even;
        v.has_queued = true;
        return true;
    }

    // Loads a period and volume now, or at the end of the current cycle of a
    // voice that is playing.
    void set_period_volume(u32 voice, u16 period, u16 volume, bool at_cycle_end = false) {
        if (voice >= kVoices) {
            return;
        }
        Voice& v = voices_[voice];
        if (at_cycle_end && v.playing) {
            v.has_pending = true;
            v.pending_period = period;
            v.pending_volume = volume;
            return;
        }
        load_settings(v, period, volume);
    }

    // Drops the current and the queued buffer without events.
    void stop(u32 voice) {
        if (voice < kVoices) {
            Voice& v = voices_[voice];
            v.playing = false;
            v.has_queued = false;
            v.has_pending = false;
            v.position = 0;
        }
    }

    // Drops the queued buffer and leaves the current one playing.
    void drop_queued(u32 voice) {
        if (voice < kVoices) {
            voices_[voice].has_queued = false;
        }
    }

    // Ends the current buffer at once, as if its last byte had been passed,
    // and starts the queued buffer if there is one.
    void finish(u32 voice, Events& events) {
        if (voice < kVoices && voices_[voice].playing) {
            voices_[voice].position = 0;
            finish_current(voice, events);
        }
    }

    // Lets the current buffer end after the cycle it is in.
    void finish_at_cycle_end(u32 voice) {
        if (voice < kVoices && voices_[voice].playing) {
            voices_[voice].current.cycles = 1;
            voices_[voice].cycles_left = 1;
        }
    }

    // Voices in the mask keep their place and make no sound until resumed.
    void pause(u32 mask) { paused_ |= mask & kAllVoices; }
    void resume(u32 mask) { paused_ &= ~mask; }

    bool playing(u32 voice) const { return voice < kVoices && voices_[voice].playing; }
    bool has_queued(u32 voice) const { return voice < kVoices && voices_[voice].has_queued; }
    bool paused(u32 voice) const { return voice < kVoices && (paused_ & (1u << voice)) != 0; }
    u16 period(u32 voice) const { return voices_[voice % kVoices].period; }
    u16 volume(u32 voice) const { return voices_[voice % kVoices].volume; }

    // How many whole bytes of the current buffer the voice has passed.
    u32 byte_position(u32 voice) const {
        return static_cast<u32>(voices_[voice % kVoices].position >> 16);
    }

    // Renders up to frames stereo frames into out, left sample first. Stops
    // early, so that events cannot be lost, when events has no room for the
    // ones one more frame can give. Returns the number of frames rendered.
    u32 render(i16* out, u32 frames, Events& events) {
        for (u32 frame = 0; frame < frames; ++frame) {
            if (events.count + kEventsPerFrame > Events::kCapacity) {
                return frame;
            }
            i32 left = 0;
            i32 right = 0;
            for (u32 voice = 0; voice < kVoices; ++voice) {
                Voice& v = voices_[voice];
                if (!v.playing || (paused_ & (1u << voice)) != 0) {
                    continue;
                }
                const u32 at = v.current.address + static_cast<u32>(v.position >> 16);
                const i32 sample = read_(read_ctx_, at) * static_cast<i32>(v.volume);
                (voice == 0 || voice == 3 ? left : right) += sample;
                advance(voice, events);
            }
            out[2 * frame] = clamp(left * kVoiceGain);
            out[2 * frame + 1] = clamp(right * kVoiceGain);
        }
        return frames;
    }

private:
    static constexpr u32 kAllVoices = (1u << kVoices) - 1;
    // Two voices at full volume and amplitude on one side give 2 * 127 * 64
    // before the gain, which brings that close to full scale.
    static constexpr i32 kVoiceGain = 2;

    struct Voice {
        Buffer current{};
        Buffer queued{};
        bool playing = false;
        bool has_queued = false;
        bool has_pending = false;
        u16 pending_period = kMinPeriod;
        u16 pending_volume = 0;
        u16 period = kMinPeriod;
        u16 volume = 0;
        u32 step = 0;      // 16.16 source bytes per output frame
        u32 cycles_left = 0;
        u64 position = 0;  // 16.16 offset into the current buffer
    };

    static i16 clamp(i32 sample) {
        return static_cast<i16>(sample > 32767 ? 32767 : (sample < -32768 ? -32768 : sample));
    }

    static void load_settings(Voice& v, u16 period, u16 volume) {
        v.period = period < kMinPeriod ? kMinPeriod : period;
        v.volume = volume > kMaxVolume ? kMaxVolume : volume;
        v.step = static_cast<u32>((static_cast<u64>(kPaulaClock) << 16) /
                                  (static_cast<u64>(v.period) * kOutputRate));
    }

    static void begin(Voice& v) {
        v.cycles_left = v.current.cycles;
        if (!v.current.keep_voice_settings) {
            load_settings(v, v.current.period, v.current.volume);
        }
        v.playing = true;
    }

    void advance(u32 voice, Events& events) {
        Voice& v = voices_[voice];
        v.position += v.step;
        const u64 end = static_cast<u64>(v.current.length) << 16;
        if (v.position < end) {
            return;
        }
        // The part of the frame past the end carries over into whatever
        // plays next, which keeps a repeat or a queued buffer seamless.
        v.position -= end;
        if (v.has_pending) {
            load_settings(v, v.pending_period, v.pending_volume);
            v.has_pending = false;
        }
        if (v.current.cycles == 0 || --v.cycles_left > 0) {
            events.add(voice, EventKind::Cycle, v.current.tag);
            return;
        }
        finish_current(voice, events);
    }

    void finish_current(u32 voice, Events& events) {
        Voice& v = voices_[voice];
        events.add(voice, EventKind::Finished, v.current.tag);
        if (!v.has_queued) {
            v.playing = false;
            return;
        }
        v.current = v.queued;
        v.has_queued = false;
        begin(v);
        events.add(voice, EventKind::Started, v.current.tag);
    }

    Voice voices_[kVoices]{};
    u32 paused_ = 0;
    ReadFn read_ = nullptr;
    void* read_ctx_ = nullptr;
};

}
