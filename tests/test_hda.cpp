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

#include "check.hpp"
#include "hda.hpp"

using namespace hombre;

namespace {

void verbs_pack_their_fields() {
    CHECK_EQ(hda_verb(0, 0, 0xF00, 0x00), 0x000F0000u);
    CHECK_EQ(hda_verb(2, 0x14, 0x707, 0x40), 0x21470740u);
    CHECK_EQ(hda_verb16(0, 2, 0x3, 0xB04A), 0x0023B04Au);
    CHECK_EQ(hda_verb16(1, 3, 0x2, kHdaStreamFormat), 0x10320011u);
}

void verbs_mask_oversized_fields() {
    CHECK_EQ(hda_verb(0x1F, 0xFF, 0x1F00, 0x1FF), 0xF7FF00FFu);
    CHECK_EQ(hda_verb16(0x1F, 0xFF, 0x13, 0x1FFFF), 0xF7F3FFFFu);
}

void connections_short_form() {
    HdaConnections list{};
    hda_add_connections(list, 0x05040302, 4, false);
    CHECK_EQ(list.count, 4u);
    CHECK_EQ(list.nodes[0], 2);
    CHECK_EQ(list.nodes[3], 5);

    // Only the entries the list length leaves are taken.
    hda_add_connections(list, 0x7F0A0908, 2, false);
    CHECK_EQ(list.count, 6u);
    CHECK_EQ(list.nodes[4], 8);
    CHECK_EQ(list.nodes[5], 9);
}

void connections_long_form() {
    HdaConnections list{};
    hda_add_connections(list, 0x00150014, 2, true);
    CHECK_EQ(list.count, 2u);
    CHECK_EQ(list.nodes[0], 0x14);
    CHECK_EQ(list.nodes[1], 0x15);

    // At most two entries fit in a long-form response.
    HdaConnections capped{};
    hda_add_connections(capped, 0x00030002, 4, true);
    CHECK_EQ(capped.count, 2u);
}

void connections_ranges() {
    HdaConnections list{};
    hda_add_connections(list, 0x8602, 2, false);
    CHECK_EQ(list.count, 5u);
    for (u32 i = 0; i < 5; ++i) {
        CHECK_EQ(list.nodes[i], static_cast<u16>(2 + i));
    }

    HdaConnections wide{};
    hda_add_connections(wide, 0x80130010, 2, true);
    CHECK_EQ(wide.count, 4u);
    CHECK_EQ(wide.nodes[3], 0x13);

    // A range with nothing before it is a single node.
    HdaConnections first{};
    hda_add_connections(first, 0x85, 1, false);
    CHECK_EQ(first.count, 1u);
    CHECK_EQ(first.nodes[0], 5);

    // A range that ends below the previous entry adds nothing.
    HdaConnections backwards{};
    hda_add_connections(backwards, 0x8510, 2, false);
    CHECK_EQ(backwards.count, 1u);
    CHECK_EQ(backwards.nodes[0], 0x10);
}

void connections_keep_the_first_nodes() {
    HdaConnections list{};
    hda_add_connections(list, 0xFF01, 2, false);
    CHECK_EQ(list.count, kHdaMaxConnections);
    CHECK_EQ(list.nodes[kHdaMaxConnections - 1], static_cast<u16>(kHdaMaxConnections));
    hda_add_connections(list, 0x02, 1, false);
    CHECK_EQ(list.count, kHdaMaxConnections);
}

void amplifiers_unmute_at_zero_db() {
    // Offset 0x4A of 0x4A steps.
    const u32 caps = (0x4Au << 8) | 0x4Au;
    CHECK_EQ(hda_amp_unmute(caps, true, 0), 0xB04Au);
    CHECK_EQ(hda_amp_unmute(caps, false, 2), 0x724Au);
    // An offset past the last step is capped.
    CHECK_EQ(hda_amp_unmute((0x10u << 8) | 0x20u, true, 0), 0xB010u);
    // A fixed amplifier only loses its mute.
    CHECK_EQ(hda_amp_unmute(0, true, 0), 0xB000u);
}

void ring_sizes() {
    CHECK_EQ(hda_ring_size(0x42).code, 2);
    CHECK_EQ(hda_ring_size(0x42).entries, 256u);
    CHECK_EQ(hda_ring_size(0x70).entries, 256u);
    CHECK_EQ(hda_ring_size(0x30).code, 1);
    CHECK_EQ(hda_ring_size(0x30).entries, 16u);
    CHECK_EQ(hda_ring_size(0x11).code, 0);
    CHECK_EQ(hda_ring_size(0x11).entries, 2u);
    // Without offers the selection stands.
    CHECK_EQ(hda_ring_size(0x01).entries, 16u);
    CHECK_EQ(hda_ring_size(0x02).entries, 256u);
    CHECK_EQ(hda_ring_size(0x03).entries, 2u);
}

void fill_starts_past_the_guard() {
    const HdaSpan span = hda_fill_span(0, 0);
    CHECK_EQ(span.start, kHdaGuardFrames);
    CHECK_EQ(span.frames, kHdaLeadFrames - kHdaGuardFrames);
}

void fill_tops_up_to_the_lead() {
    HdaSpan span = hda_fill_span(100, 2048);
    CHECK_EQ(span.start, 2048u);
    CHECK_EQ(span.frames, 100u);

    span = hda_fill_span(0, 2048);
    CHECK_EQ(span.frames, 0u);

    span = hda_fill_span(1024, 2048);
    CHECK_EQ(span.start, 2048u);
    CHECK_EQ(span.frames, 1024u);
}

void fill_wraps_around_the_ring() {
    HdaSpan span = hda_fill_span(4000, 1852);
    CHECK_EQ(span.start, 1852u);
    CHECK_EQ(span.frames, 100u);

    span = hda_fill_span(3500, 904);
    CHECK_EQ(span.start, 904u);
    CHECK_EQ(span.frames, 548u);

    span = hda_fill_span(3900, 1852);
    CHECK_EQ(span.frames, 0u);
}

void fill_restarts_when_too_close() {
    HdaSpan span = hda_fill_span(1500, 2048);
    CHECK_EQ(span.start, 2524u);
    CHECK_EQ(span.frames, 1024u);

    span = hda_fill_span(3500, 3600);
    CHECK_EQ(span.start, 428u);
    CHECK_EQ(span.frames, 1024u);
}

void fill_restarts_after_an_underrun() {
    HdaSpan span = hda_fill_span(2100, 2048);
    CHECK_EQ(span.start, 3124u);
    CHECK_EQ(span.frames, 1024u);

    span = hda_fill_span(100, 4000);
    CHECK_EQ(span.start, 1124u);
    CHECK_EQ(span.frames, 1024u);
}

void fill_never_reaches_the_guard() {
    for (u32 play = 0; play < kHdaRingFrames; play += 97) {
        for (u32 write = 0; write < kHdaRingFrames; write += 89) {
            const HdaSpan span = hda_fill_span(play, write);
            const u32 offset = (span.start + kHdaRingFrames - play) % kHdaRingFrames;
            CHECK(offset >= kHdaGuardFrames);
            CHECK(offset + span.frames <= kHdaLeadFrames);
        }
    }
}

}

int main() {
    verbs_pack_their_fields();
    verbs_mask_oversized_fields();
    connections_short_form();
    connections_long_form();
    connections_ranges();
    connections_keep_the_first_nodes();
    amplifiers_unmute_at_zero_db();
    ring_sizes();
    fill_starts_past_the_guard();
    fill_tops_up_to_the_lead();
    fill_wraps_around_the_ring();
    fill_restarts_when_too_close();
    fill_restarts_after_an_underrun();
    fill_never_reaches_the_guard();
    return check::finish("test_hda");
}
