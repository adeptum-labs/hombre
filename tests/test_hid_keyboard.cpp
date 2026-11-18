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
    return check::finish("test_hid_keyboard");
}
