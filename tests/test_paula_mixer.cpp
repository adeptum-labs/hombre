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

#include "paula_mixer.hpp"
#include "check.hpp"

#include <vector>

using namespace hombre;
using Mixer = PaulaMixer;

namespace {

// The step the mixer should take per frame, worked out from the definition
// (Paula clock / period / output rate, in 16.16) rather than from its code.
u32 step_of(u32 period) {
    return static_cast<u32>((u64{3579545} << 16) / (u64{period} * 48000));
}

// The first frame, counted from 1, by which a voice has passed bytes bytes.
u32 frame_of(u64 bytes, u32 step) {
    return static_cast<u32>(((bytes << 16) + step - 1) / step);
}

struct Rig {
    Rig() { mixer.set_reader(read, this); }

    static i8 read(void* ctx, u32 address) {
        Rig& r = *static_cast<Rig*>(ctx);
        r.reads.push_back(address);
        return r.memory[address];
    }

    void fill(u32 address, u32 length, i8 value) {
        for (u32 i = 0; i < length; ++i) {
            memory[address + i] = value;
        }
    }

    Mixer mixer;
    std::vector<i8> memory = std::vector<i8>(0x10000);
    std::vector<u32> reads;
};

Mixer::Buffer wave(u32 address, u32 length, u16 period, u16 volume, u32 cycles = 1,
                   u32 tag = 0) {
    Mixer::Buffer b;
    b.address = address;
    b.length = length;
    b.period = period;
    b.volume = volume;
    b.cycles = cycles;
    b.tag = tag;
    return b;
}

struct Frame {
    i16 left;
    i16 right;
};

Frame render_one(Mixer& mixer) {
    Mixer::Events events;
    i16 out[2];
    CHECK_EQ(mixer.render(out, 1, events), 1u);
    CHECK_EQ(events.count, 0u);
    return {out[0], out[1]};
}

struct Seen {
    u32 frame;
    Mixer::Event event;
};

// Renders frames frames one at a time and lists the events with the frame
// they came in, counted from 1.
std::vector<Seen> run_frames(Mixer& mixer, u32 frames) {
    std::vector<Seen> seen;
    Mixer::Events events;
    for (u32 f = 1; f <= frames; ++f) {
        events.clear();
        i16 out[2];
        CHECK_EQ(mixer.render(out, 1, events), 1u);
        for (u32 i = 0; i < events.count; ++i) {
            seen.push_back({f, events.items[i]});
        }
    }
    return seen;
}

void source_bytes_consumed_follow_the_period() {
    for (const u32 period : {428u, 254u}) {
        Rig r;
        Mixer::Events events;
        CHECK(r.mixer.queue(0, wave(0, 20000, static_cast<u16>(period), 64)));
        std::vector<i16> out(2 * 48000);
        CHECK_EQ(r.mixer.render(out.data(), 48000, events), 48000u);
        CHECK_EQ(events.count, 0u);
        // 3579545 / period bytes a second, less what the fixed-point step drops.
        const u32 ideal = 3579545 / period;
        CHECK(r.mixer.byte_position(0) <= ideal);
        CHECK(r.mixer.byte_position(0) + 1 >= ideal);
    }
}

void periods_below_the_minimum_are_raised() {
    Rig r;
    CHECK(r.mixer.queue(0, wave(0, 4000, 50, 64)));
    CHECK_EQ(r.mixer.period(0), 124u);
    r.mixer.set_period_volume(0, 1, 64);
    CHECK_EQ(r.mixer.period(0), 124u);
    run_frames(r.mixer, 1000);
    CHECK_EQ(r.mixer.byte_position(0), static_cast<u32>(u64{1000} * step_of(124) >> 16));
}

void volume_scales_the_sample() {
    Rig r;
    r.fill(0, 64, 100);
    CHECK(r.mixer.queue(0, wave(0, 64, 428, 64, 0)));
    Frame f = render_one(r.mixer);
    CHECK_EQ(f.left, 100 * 64 * 2);
    CHECK_EQ(f.right, 0);
    r.mixer.set_period_volume(0, 428, 32);
    CHECK_EQ(render_one(r.mixer).left, 100 * 32 * 2);
    r.mixer.set_period_volume(0, 428, 0);
    CHECK_EQ(render_one(r.mixer).left, 0);
    r.mixer.set_period_volume(0, 428, 200);
    CHECK_EQ(r.mixer.volume(0), 64u);
    CHECK_EQ(render_one(r.mixer).left, 100 * 64 * 2);
}

void two_full_voices_per_side_reach_full_scale_without_overflow() {
    Rig loud;
    loud.fill(0, 64, 127);
    CHECK(loud.mixer.queue(0, wave(0, 64, 428, 64, 0)));
    CHECK(loud.mixer.queue(3, wave(0, 64, 428, 64, 0)));
    CHECK_EQ(render_one(loud.mixer).left, 127 * 64 * 2 * 2);

    Rig low;
    low.fill(0, 64, -128);
    CHECK(low.mixer.queue(1, wave(0, 64, 428, 64, 0)));
    CHECK(low.mixer.queue(2, wave(0, 64, 428, 64, 0)));
    const Frame f = render_one(low.mixer);
    CHECK_EQ(f.right, -32768);
    CHECK_EQ(f.left, 0);
}

void voices_go_to_the_channel_they_have_on_the_amiga() {
    for (u32 voice = 0; voice < Mixer::kVoices; ++voice) {
        Rig r;
        r.fill(0, 64, 5);
        CHECK(r.mixer.queue(voice, wave(0, 64, 428, 64, 0)));
        const Frame f = render_one(r.mixer);
        const bool left = voice == 0 || voice == 3;
        CHECK_EQ(f.left, left ? 5 * 128 : 0);
        CHECK_EQ(f.right, left ? 0 : 5 * 128);
    }

    Rig r;
    r.fill(0x000, 64, 1);
    r.fill(0x100, 64, 2);
    r.fill(0x200, 64, 4);
    r.fill(0x300, 64, 8);
    for (u32 voice = 0; voice < Mixer::kVoices; ++voice) {
        CHECK(r.mixer.queue(voice, wave(voice * 0x100, 64, 428, 64, 0)));
    }
    const Frame f = render_one(r.mixer);
    CHECK_EQ(f.left, (1 + 8) * 128);
    CHECK_EQ(f.right, (2 + 4) * 128);
}

void two_cycles_play_the_buffer_twice() {
    Rig r;
    constexpr u32 length = 1000;
    const u32 step = step_of(428);
    CHECK(r.mixer.queue(0, wave(0, length, 428, 64, 2, 7)));
    const u32 cycle_frame = frame_of(length, step);
    const u32 finish_frame = frame_of(2 * length, step);
    const std::vector<Seen> seen = run_frames(r.mixer, finish_frame + 10);

    CHECK_EQ(seen.size(), 2u);
    CHECK(seen[0].event.kind == Mixer::EventKind::Cycle);
    CHECK_EQ(seen[0].frame, cycle_frame);
    CHECK_EQ(seen[0].event.tag, 7u);
    CHECK(seen[1].event.kind == Mixer::EventKind::Finished);
    CHECK_EQ(seen[1].frame, finish_frame);
    CHECK_EQ(seen[1].event.tag, 7u);
    CHECK(!r.mixer.playing(0));

    // The second cycle starts again at the first byte, carrying the part
    // of the step that went past the end.
    CHECK_EQ(r.reads.size(), static_cast<usize>(finish_frame));
    for (u32 k = 0; k < finish_frame; ++k) {
        CHECK_EQ(r.reads[k], static_cast<u32>((u64{k} * step >> 16) % length));
    }
}

void a_queued_buffer_follows_without_a_gap() {
    Rig r;
    constexpr u32 length = 500;
    const u32 step = step_of(428);
    CHECK(r.mixer.queue(0, wave(0, length, 428, 64, 1, 1)));
    CHECK(r.mixer.queue(0, wave(0x1000, length, 428, 64, 1, 2)));
    CHECK(r.mixer.has_queued(0));
    CHECK(!r.mixer.queue(0, wave(0x2000, length, 428, 64, 1, 3)));

    const u32 first_end = frame_of(length, step);
    const u32 second_end = frame_of(2 * length, step);
    const std::vector<Seen> seen = run_frames(r.mixer, second_end + 10);

    CHECK_EQ(seen.size(), 3u);
    CHECK(seen[0].event.kind == Mixer::EventKind::Finished);
    CHECK_EQ(seen[0].event.tag, 1u);
    CHECK_EQ(seen[0].frame, first_end);
    CHECK(seen[1].event.kind == Mixer::EventKind::Started);
    CHECK_EQ(seen[1].event.tag, 2u);
    CHECK_EQ(seen[1].frame, first_end);
    CHECK(seen[2].event.kind == Mixer::EventKind::Finished);
    CHECK_EQ(seen[2].event.tag, 2u);
    CHECK_EQ(seen[2].frame, second_end);

    CHECK_EQ(r.reads.size(), static_cast<usize>(second_end));
    for (u32 k = 0; k < second_end; ++k) {
        const u32 passed = static_cast<u32>(u64{k} * step >> 16);
        CHECK_EQ(r.reads[k], k < first_end ? passed : 0x1000 + passed - length);
    }
    CHECK(!r.mixer.has_queued(0));
}

void events_come_in_the_order_they_happen() {
    Rig r;
    CHECK(r.mixer.queue(0, wave(0, 100, 428, 64, 1, 10)));
    CHECK(r.mixer.queue(1, wave(0, 100, 428, 64, 1, 30)));
    CHECK(r.mixer.queue(2, wave(0, 50, 428, 64, 1, 20)));
    Mixer::Events events;
    std::vector<i16> out(2 * 2000);
    CHECK_EQ(r.mixer.render(out.data(), 2000, events), 2000u);
    CHECK_EQ(events.count, 3u);
    // The shortest buffer ends first, and two that end in the same frame
    // are reported in voice order.
    CHECK_EQ(events.items[0].tag, 20u);
    CHECK_EQ(events.items[0].voice, 2u);
    CHECK_EQ(events.items[1].tag, 10u);
    CHECK_EQ(events.items[1].voice, 0u);
    CHECK_EQ(events.items[2].tag, 30u);
    CHECK_EQ(events.items[2].voice, 1u);
}

void render_stops_before_events_could_be_lost() {
    Rig r;
    CHECK(r.mixer.queue(0, wave(0, 2, 124, 64, 0)));
    Mixer::Events events;
    std::vector<i16> out(2 * 1000);
    const u32 done = r.mixer.render(out.data(), 1000, events);
    CHECK(done < 1000);
    CHECK_EQ(events.count, 25u);
    events.clear();
    CHECK(r.mixer.render(out.data(), 1000 - done, events) > 0);
}

void stop_drops_both_buffers() {
    Rig r;
    r.fill(0, 64, 100);
    CHECK(r.mixer.queue(0, wave(0, 64, 428, 64, 0, 1)));
    CHECK(r.mixer.queue(0, wave(0, 64, 428, 64, 1, 2)));
    r.mixer.stop(0);
    CHECK(!r.mixer.playing(0));
    CHECK(!r.mixer.has_queued(0));
    const Frame f = render_one(r.mixer);
    CHECK_EQ(f.left, 0);
    CHECK_EQ(f.right, 0);
    CHECK(run_frames(r.mixer, 10).empty());
    CHECK(r.mixer.queue(0, wave(0, 64, 428, 64, 0, 3)));
    CHECK_EQ(render_one(r.mixer).left, 100 * 64 * 2);
}

void drop_queued_leaves_the_current_buffer_playing() {
    Rig r;
    CHECK(r.mixer.queue(0, wave(0, 100, 428, 64, 1, 1)));
    CHECK(r.mixer.queue(0, wave(0, 100, 428, 64, 1, 2)));
    r.mixer.drop_queued(0);
    CHECK(!r.mixer.has_queued(0));
    const std::vector<Seen> seen = run_frames(r.mixer, 2000);
    CHECK_EQ(seen.size(), 1u);
    CHECK_EQ(seen[0].event.tag, 1u);
    CHECK(!r.mixer.playing(0));
}

void a_paused_voice_keeps_its_place() {
    Rig r;
    r.fill(0, 4000, 100);
    CHECK(r.mixer.queue(0, wave(0, 4000, 428, 64, 0)));
    run_frames(r.mixer, 100);
    const u32 place = r.mixer.byte_position(0);
    CHECK(place > 0);

    r.mixer.pause(1);
    CHECK(r.mixer.paused(0));
    CHECK_EQ(render_one(r.mixer).left, 0);
    run_frames(r.mixer, 100);
    CHECK_EQ(r.mixer.byte_position(0), place);

    r.mixer.resume(1);
    CHECK(!r.mixer.paused(0));
    CHECK_EQ(render_one(r.mixer).left, 100 * 64 * 2);
    CHECK(r.mixer.playing(0));
}

void finish_ends_the_current_buffer_at_once() {
    Rig r;
    CHECK(r.mixer.queue(0, wave(0, 1000, 428, 64, 1, 1)));
    CHECK(r.mixer.queue(0, wave(0, 1000, 428, 64, 1, 2)));
    run_frames(r.mixer, 50);

    Mixer::Events events;
    r.mixer.finish(0, events);
    CHECK_EQ(events.count, 2u);
    CHECK(events.items[0].kind == Mixer::EventKind::Finished);
    CHECK_EQ(events.items[0].tag, 1u);
    CHECK(events.items[1].kind == Mixer::EventKind::Started);
    CHECK_EQ(events.items[1].tag, 2u);
    CHECK_EQ(r.mixer.byte_position(0), 0u);

    events.clear();
    r.mixer.finish(0, events);
    CHECK_EQ(events.count, 1u);
    CHECK_EQ(events.items[0].tag, 2u);
    CHECK(!r.mixer.playing(0));

    events.clear();
    r.mixer.finish(0, events);
    CHECK_EQ(events.count, 0u);
}

void finish_at_cycle_end_lets_the_cycle_run_out() {
    Rig r;
    const u32 step = step_of(428);
    CHECK(r.mixer.queue(0, wave(0, 100, 428, 64, 0, 4)));
    run_frames(r.mixer, 10);
    r.mixer.finish_at_cycle_end(0);
    const std::vector<Seen> seen = run_frames(r.mixer, 2000);
    CHECK_EQ(seen.size(), 1u);
    CHECK(seen[0].event.kind == Mixer::EventKind::Finished);
    CHECK_EQ(seen[0].frame + 10, frame_of(100, step));
    CHECK(!r.mixer.playing(0));
}

void period_and_volume_can_wait_for_the_end_of_the_cycle() {
    Rig r;
    CHECK(r.mixer.queue(0, wave(0, 100, 428, 64, 3, 5)));
    r.mixer.set_period_volume(0, 254, 10, true);
    CHECK_EQ(r.mixer.period(0), 428u);
    CHECK_EQ(r.mixer.volume(0), 64u);
    const std::vector<Seen> seen = run_frames(r.mixer, frame_of(100, step_of(428)));
    CHECK_EQ(seen.size(), 1u);
    CHECK(seen[0].event.kind == Mixer::EventKind::Cycle);
    CHECK_EQ(r.mixer.period(0), 254u);
    CHECK_EQ(r.mixer.volume(0), 10u);
}

void a_buffer_loads_period_and_volume_unless_told_to_keep_them() {
    Rig r;
    CHECK(r.mixer.queue(0, wave(0, 100, 254, 30, 1, 1)));
    CHECK_EQ(r.mixer.period(0), 254u);
    CHECK_EQ(r.mixer.volume(0), 30u);

    Mixer::Buffer keeper = wave(0, 100, 428, 64, 1, 2);
    keeper.keep_voice_settings = true;
    CHECK(r.mixer.queue(0, keeper));
    run_frames(r.mixer, frame_of(100, step_of(254)));
    CHECK_EQ(r.mixer.period(0), 254u);
    CHECK_EQ(r.mixer.volume(0), 30u);
}

void unusable_buffers_and_voices_are_refused() {
    Rig r;
    CHECK(!r.mixer.queue(0, wave(0, 0, 428, 64)));
    CHECK(!r.mixer.queue(0, wave(0, 1, 428, 64)));
    CHECK(!r.mixer.queue(Mixer::kVoices, wave(0, 64, 428, 64)));
    CHECK(!r.mixer.playing(0));
    CHECK(r.mixer.queue(0, wave(0, 101, 428, 64)));
    CHECK(r.mixer.playing(0));
}

}  // namespace

int main() {
    source_bytes_consumed_follow_the_period();
    periods_below_the_minimum_are_raised();
    volume_scales_the_sample();
    two_full_voices_per_side_reach_full_scale_without_overflow();
    voices_go_to_the_channel_they_have_on_the_amiga();
    two_cycles_play_the_buffer_twice();
    a_queued_buffer_follows_without_a_gap();
    events_come_in_the_order_they_happen();
    render_stops_before_events_could_be_lost();
    stop_drops_both_buffers();
    drop_queued_leaves_the_current_buffer_playing();
    a_paused_voice_keeps_its_place();
    finish_ends_the_current_buffer_at_once();
    finish_at_cycle_end_lets_the_cycle_run_out();
    period_and_volume_can_wait_for_the_end_of_the_cycle();
    a_buffer_loads_period_and_volume_unless_told_to_keep_them();
    unusable_buffers_and_voices_are_refused();
    return check::finish("test_paula_mixer");
}
