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
#include "hid_keyboard.hpp"

#include <initializer_list>

using namespace hombre;

namespace {

void single_key_press_gives_one_char() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 press_a[8] = {0, 0, 0x04};
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], 'a');
}

void release_gives_nothing() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 press_a[8] = {0, 0, 0x04};
    const u8 released[8] = {};
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);
    CHECK_EQ(out[1], 0);
}

void held_key_does_not_repeat() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 press_a[8] = {0, 0, 0x04};
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 0u);
}

void two_keys_in_one_report_in_order() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 press_ab[8] = {0, 0, 0x04, 0x05};
    CHECK_EQ(decoder.feed(press_ab, out, sizeof(out)), 2u);
    CHECK_EQ(out[0], 'a');
    CHECK_EQ(out[1], 'b');
}

void modifier_alone_gives_nothing() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 shift_only[8] = {kModLShift, 0};
    CHECK_EQ(decoder.feed(shift_only, out, sizeof(out)), 0u);
}

void shift_applies_to_the_key_in_the_same_report() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 shift_a[8] = {kModLShift, 0, 0x04};
    CHECK_EQ(decoder.feed(shift_a, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], 'A');
}

void caps_lock_toggles_on_each_press() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 caps[8] = {0, 0, 0x39};
    const u8 released[8] = {};
    const u8 press_a[8] = {0, 0, 0x04};
    CHECK_EQ(decoder.feed(caps, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], 'A');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(caps, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], 'a');
}

void rollover_error_report_is_ignored() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 press_a[8] = {0, 0, 0x04};
    const u8 rollover[8] = {0, 0, 1, 1, 1, 1, 1, 1};
    const u8 press_b[8] = {0, 0, 0x05};
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(decoder.feed(rollover, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(press_b, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], 'b');
}

void capacity_is_respected() {
    KeyDecoder decoder;
    u8 out[2] = {0xEE, 0xEE};
    const u8 press_ab[8] = {0, 0, 0x04, 0x05};
    CHECK_EQ(decoder.feed(press_ab, out, 1), 1u);
    CHECK_EQ(out[0], 'a');
    CHECK_EQ(out[1], 0xEE);
    CHECK_EQ(decoder.feed(press_ab, out, 1), 0u);
}

void reset_clears_caps_lock() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 caps[8] = {0, 0, 0x39};
    const u8 released[8] = {};
    const u8 press_a[8] = {0, 0, 0x04};
    CHECK_EQ(decoder.feed(caps, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], 'A');
    decoder.reset();
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], 'a');
}

void release_all_lets_a_held_key_type_again() {
    // A command ran while Enter went down and up: its release report was
    // read and dropped elsewhere, so the decoder still thinks Enter is held.
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 press_enter[8] = {0, 0, 0x28};
    CHECK_EQ(decoder.feed(press_enter, out, sizeof(out)), 1u);
    CHECK_EQ(decoder.feed(press_enter, out, sizeof(out)), 0u);
    decoder.release_all();
    CHECK_EQ(decoder.feed(press_enter, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], '\n');
}

void release_all_keeps_caps_lock() {
    KeyDecoder decoder;
    u8 out[8] = {};
    const u8 caps[8] = {0, 0, 0x39};
    const u8 released[8] = {};
    const u8 press_a[8] = {0, 0, 0x04};
    CHECK_EQ(decoder.feed(caps, out, sizeof(out)), 0u);
    decoder.release_all();
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);
    CHECK_EQ(decoder.feed(press_a, out, sizeof(out)), 1u);
    CHECK_EQ(out[0], 'A');
}

void arrows_give_csi_sequences() {
    KeyDecoder decoder;
    u8 out[KeyDecoder::kKeySlots * KeyDecoder::kMaxBytesPerKey] = {};
    const u8 released[8] = {};

    const u8 up[8] = {0, 0, 0x52};
    CHECK_EQ(decoder.feed(up, out, sizeof(out)), 2u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], 'A');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 down[8] = {0, 0, 0x51};
    CHECK_EQ(decoder.feed(down, out, sizeof(out)), 2u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], 'B');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 right[8] = {0, 0, 0x4F};
    CHECK_EQ(decoder.feed(right, out, sizeof(out)), 2u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], 'C');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 left[8] = {0, 0, 0x50};
    CHECK_EQ(decoder.feed(left, out, sizeof(out)), 2u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], 'D');
}

void shift_up_down_give_t_and_s() {
    KeyDecoder decoder;
    u8 out[KeyDecoder::kKeySlots * KeyDecoder::kMaxBytesPerKey] = {};
    const u8 released[8] = {};

    const u8 lshift_up[8] = {kModLShift, 0, 0x52};
    CHECK_EQ(decoder.feed(lshift_up, out, sizeof(out)), 2u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], 'T');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 rshift_down[8] = {kModRShift, 0, 0x51};
    CHECK_EQ(decoder.feed(rshift_down, out, sizeof(out)), 2u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], 'S');
}

void page_home_end_sequences() {
    KeyDecoder decoder;
    u8 out[KeyDecoder::kKeySlots * KeyDecoder::kMaxBytesPerKey] = {};
    const u8 released[8] = {};

    const u8 page_up[8] = {0, 0, 0x4B};
    CHECK_EQ(decoder.feed(page_up, out, sizeof(out)), 4u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], '4');
    CHECK_EQ(out[2], '1');
    CHECK_EQ(out[3], '~');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 shift_page_up[8] = {kModLShift, 0, 0x4B};
    CHECK_EQ(decoder.feed(shift_page_up, out, sizeof(out)), 4u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], '4');
    CHECK_EQ(out[2], '1');
    CHECK_EQ(out[3], '~');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 page_down[8] = {0, 0, 0x4E};
    CHECK_EQ(decoder.feed(page_down, out, sizeof(out)), 4u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], '4');
    CHECK_EQ(out[2], '2');
    CHECK_EQ(out[3], '~');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 shift_page_down[8] = {kModRShift, 0, 0x4E};
    CHECK_EQ(decoder.feed(shift_page_down, out, sizeof(out)), 4u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], '4');
    CHECK_EQ(out[2], '2');
    CHECK_EQ(out[3], '~');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 home[8] = {0, 0, 0x4A};
    CHECK_EQ(decoder.feed(home, out, sizeof(out)), 4u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], '4');
    CHECK_EQ(out[2], '4');
    CHECK_EQ(out[3], '~');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 end[8] = {0, 0, 0x4D};
    CHECK_EQ(decoder.feed(end, out, sizeof(out)), 4u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], '4');
    CHECK_EQ(out[2], '5');
    CHECK_EQ(out[3], '~');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 shift_home[8] = {kModLShift, 0, 0x4A};
    CHECK_EQ(decoder.feed(shift_home, out, sizeof(out)), 4u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], '5');
    CHECK_EQ(out[2], '4');
    CHECK_EQ(out[3], '~');
    CHECK_EQ(decoder.feed(released, out, sizeof(out)), 0u);

    const u8 shift_end[8] = {kModRShift, 0, 0x4D};
    CHECK_EQ(decoder.feed(shift_end, out, sizeof(out)), 4u);
    CHECK_EQ(out[0], 0x9B);
    CHECK_EQ(out[1], '5');
    CHECK_EQ(out[2], '5');
    CHECK_EQ(out[3], '~');
}

void six_keys_fit_the_buffer() {
    KeyDecoder decoder;
    u8 out[KeyDecoder::kKeySlots * KeyDecoder::kMaxBytesPerKey] = {};
    CHECK_EQ(sizeof(out), 24u);

    const u8 six[8] = {0, 0, 0x4A, 0x4B, 0x4D, 0x4E, 0x52, 0x51};
    const usize n = decoder.feed(six, out, sizeof(out));
    CHECK_EQ(n, 20u);

    const u8 expected[20] = {
        0x9B, '4', '4', '~',
        0x9B, '4', '1', '~',
        0x9B, '4', '5', '~',
        0x9B, '4', '2', '~',
        0x9B, 'A',
        0x9B, 'B',
    };
    for (usize i = 0; i < n; ++i) {
        CHECK_EQ(out[i], expected[i]);
    }
}

}

// A raw key event types what the report of the same key under the same
// modifiers types, whichever Ctrl key: input.device's events reach the
// shells as the reports do.
void raw_key_events_type_what_the_reports_type() {
    u32 compared = 0;
    for (u32 usage = 0x04; usage <= 0x64; ++usage) {
        const u8 code = hid_to_amiga(static_cast<u8>(usage));
        if (code == kNoAmigaKey || usage == kUsageCapsLock) {
            continue;
        }
        for (u32 mods = 0; mods < 256; ++mods) {
            for (const bool caps : {false, true}) {
                u8 want[KeyDecoder::kMaxBytesPerKey] = {};
                u8 got[KeyDecoder::kMaxBytesPerKey] = {};
                const usize want_count = KeyDecoder::decode(static_cast<u8>(usage),
                                                            static_cast<u8>(mods), caps, want);
                const u16 qualifier = static_cast<u16>(
                    hid_qualifier(static_cast<u8>(mods), caps) |
                    (hid_is_keypad(static_cast<u8>(usage)) ? kQualNumericPad : 0));
                const usize got_count = raw_key_bytes(code, qualifier, got);
                CHECK_EQ(got_count, want_count);
                for (usize i = 0; i < want_count; ++i) {
                    CHECK_EQ(got[i], want[i]);
                }
                ++compared;
            }
        }
    }
    CHECK(compared > 80u * 512u);

    // A release, Caps Lock and a code no key has type nothing.
    u8 out[KeyDecoder::kMaxBytesPerKey] = {};
    CHECK_EQ(raw_key_bytes(0x20 | kAmigaKeyUp, 0, out), 0u);
    CHECK_EQ(raw_key_bytes(kAmigaCapsLock, 0, out), 0u);
    CHECK_EQ(raw_key_bytes(0x77, 0, out), 0u);
    CHECK_EQ(raw_key_bytes(0x20, 0, out), 1u);
    CHECK_EQ(out[0], 'a');
}

int main() {
    single_key_press_gives_one_char();
    release_gives_nothing();
    held_key_does_not_repeat();
    two_keys_in_one_report_in_order();
    modifier_alone_gives_nothing();
    shift_applies_to_the_key_in_the_same_report();
    caps_lock_toggles_on_each_press();
    rollover_error_report_is_ignored();
    capacity_is_respected();
    reset_clears_caps_lock();
    release_all_lets_a_held_key_type_again();
    release_all_keeps_caps_lock();
    arrows_give_csi_sequences();
    shift_up_down_give_t_and_s();
    page_home_end_sequences();
    six_keys_fit_the_buffer();
    raw_key_events_type_what_the_reports_type();
    return check::finish("test_hid_keyboard");
}
