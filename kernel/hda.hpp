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

#include "../common/types.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"

// A polled Intel High Definition Audio output driver. This header holds the
// parts that touch no hardware: verb encoding, connection list parsing, ring
// sizes, amplifier settings and the arithmetic that keeps the stream fed.
// The parts that talk to the controller are in hda.cpp and are kernel only.

namespace hombre {

// The stream plays 48 kHz 16-bit stereo frames from a ring of four buffers.
inline constexpr u32 kHdaChannels = 2;
inline constexpr u32 kHdaFrameBytes = kHdaChannels * sizeof(i16);
inline constexpr u32 kHdaBufferFrames = 1024;
inline constexpr u32 kHdaBuffers = 4;
inline constexpr u32 kHdaRingFrames = kHdaBuffers * kHdaBufferFrames;
// Frames kept written ahead of the play position, about 43 ms.
inline constexpr u32 kHdaLeadFrames = 2048;
// Frames just past the play position that the controller may already have
// fetched, so they are never written.
inline constexpr u32 kHdaGuardFrames = kHdaBufferFrames;
// 48 kHz base rate without multiplier or divisor, 16 bits, two channels.
inline constexpr u16 kHdaStreamFormat = 0x0011;
inline constexpr u8 kHdaStreamTag = 1;
inline constexpr u32 kHdaMaxConnections = 32;

static_assert(kHdaGuardFrames < kHdaLeadFrames && kHdaLeadFrames < kHdaRingFrames);

// A command for node `node` (0 to 127) of the codec at address `codec` (0 to
// 15) with a 12-bit verb and an 8-bit payload.
constexpr u32 hda_verb(u32 codec, u32 node, u32 verb, u32 payload) {
    return ((codec & 0xFu) << 28) | ((node & 0x7Fu) << 20) | ((verb & 0xFFFu) << 8) |
           (payload & 0xFFu);
}

// A command with a 4-bit verb and a 16-bit payload, the form of
// SET_CONVERTER_FORMAT and SET_AMP_GAIN_MUTE.
constexpr u32 hda_verb16(u32 codec, u32 node, u32 verb, u32 payload) {
    return ((codec & 0xFu) << 28) | ((node & 0x7Fu) << 20) | ((verb & 0xFu) << 16) |
           (payload & 0xFFFFu);
}

struct HdaConnections {
    u16 nodes[kHdaMaxConnections];
    u32 count;
};

// Appends `entries` connection list entries packed into one
// GET_CONNECTION_LIST_ENTRY response, the first in the low bits: up to four
// 8-bit entries in the short form, up to two 16-bit entries in the long
// form. An entry flagged as a range stands for every node after the
// previous entry up to its own. Nodes past kHdaMaxConnections are dropped.
constexpr void hda_add_connections(HdaConnections& list, u32 response, u32 entries,
                                   bool long_form) {
    const u32 width = long_form ? 16 : 8;
    const u32 range_flag = 1u << (width - 1);
    for (u32 i = 0; i < entries && i * width < 32; ++i) {
        const u32 entry = (response >> (i * width)) & ((1u << width) - 1);
        const u32 node = entry & (range_flag - 1);
        const bool range = (entry & range_flag) != 0 && list.count != 0;
        for (u32 next = range ? list.nodes[list.count - 1] + 1u : node;
             next <= node && list.count < kHdaMaxConnections; ++next) {
            list.nodes[list.count++] = static_cast<u16>(next);
        }
    }
}

// The SET_AMP_GAIN_MUTE payload that unmutes both channels of the output
// amplifier, or of input amplifier `index`, at 0 dB: the step that the
// amplifier capabilities name as their offset, capped at the last step.
constexpr u32 hda_amp_unmute(u32 amp_caps, bool output, u32 index) {
    const u32 offset = amp_caps & 0x7Fu;
    const u32 last_step = (amp_caps >> 8) & 0x7Fu;
    const u32 gain = offset < last_step ? offset : last_step;
    return (output ? 1u << 15 : 1u << 14) | (3u << 12) | ((index & 0xFu) << 8) | gain;
}

struct HdaRingSize {
    u8 code;
    u32 entries;
};

// The size to select for the CORB or the RIRB from CORBSIZE or RIRBSIZE,
// whose bits 4 to 6 offer 2, 16 and 256 entries and whose bits 1:0 select
// one. The largest offer wins; a register that offers none keeps its
// selection.
constexpr HdaRingSize hda_ring_size(u8 size_register) {
    constexpr u32 entries[] = {2, 16, 256};
    const u8 selected = size_register & 3u;
    u8 code = selected < 3 ? selected : 0;
    for (u8 offer = 0; offer < 3; ++offer) {
        if ((size_register & (0x10u << offer)) != 0) {
            code = offer;
        }
    }
    return HdaRingSize{code, entries[code]};
}

struct HdaSpan {
    u32 start;
    u32 frames;
};

// What to write next, from the ring frame the controller plays (`play`) and
// the ring frame the last write ended before (`write`). Writing runs up to
// kHdaLeadFrames past `play`. When fewer than kHdaGuardFrames written
// frames are left ahead of `play`, or `play` has overtaken `write`, writing
// restarts kHdaGuardFrames past `play` and the frames in between play
// stale.
constexpr HdaSpan hda_fill_span(u32 play, u32 write) {
    const u32 ahead = (write + kHdaRingFrames - play) % kHdaRingFrames;
    if (ahead >= kHdaGuardFrames && ahead <= kHdaLeadFrames) {
        return HdaSpan{write, kHdaLeadFrames - ahead};
    }
    return HdaSpan{(play + kHdaGuardFrames) % kHdaRingFrames, kHdaLeadFrames - kHdaGuardFrames};
}

// Brings up the first High Definition Audio controller (PCI class 0x04,
// subclass 0x03) for polling, finds a path from an output pin of its first
// codec to a converter and starts the output stream on the ring, silent at
// first. Returns false, logging why on the serial line, when there is no
// controller, codec or path or a step fails; the controller is then left
// in reset and its pages are given back.
bool hda_init(MmioMapper& mmio, PhysAllocator& pages);

// True once hda_init has started the stream.
bool hda_ready();

// Keeps the ring written kHdaLeadFrames ahead of the play position. `fill`
// writes up to `frames` interleaved stereo frames to `out` and returns how
// many it wrote; the rest is written as silence. Call it every few
// milliseconds: the ring holds 85 ms. Does nothing unless hda_ready().
void hda_pump(u32 (*fill)(void* ctx, i16* out, u32 frames), void* ctx);

}
