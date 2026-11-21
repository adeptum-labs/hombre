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
#include "hid_rawkey.hpp"

using namespace hombre;

namespace {

constexpr u8 kLShiftBit = 0x02;
constexpr u8 kLCtrlBit = 0x01;
constexpr u8 kUsageA = 0x04;
constexpr u8 kUsageKeypad1 = 0x59;

struct Fed {
    RawKeyEvent events[RawKeyTracker::kMaxEvents];
    usize count;
};

Fed feed(RawKeyTracker& tracker, u8 modifiers, u8 k0 = 0, u8 k1 = 0) {
    const u8 report[8] = {modifiers, 0, k0, k1};
    Fed fed{};
    fed.count = tracker.feed(report, fed.events, RawKeyTracker::kMaxEvents);
    return fed;
}

void letters_follow_the_physical_rows() {
    CHECK_EQ(hid_to_amiga(0x04), 0x20);  // A
    CHECK_EQ(hid_to_amiga(0x14), 0x10);  // Q
    CHECK_EQ(hid_to_amiga(0x13), 0x19);  // P
    CHECK_EQ(hid_to_amiga(0x1D), 0x31);  // Z
    CHECK_EQ(hid_to_amiga(0x10), 0x37);  // M
    CHECK_EQ(hid_to_amiga(0x0F), 0x28);  // L
}

void digits_and_punctuation() {
    CHECK_EQ(hid_to_amiga(0x1E), 0x01);  // 1
    CHECK_EQ(hid_to_amiga(0x27), 0x0A);  // 0
    CHECK_EQ(hid_to_amiga(0x2D), 0x0B);  // -
    CHECK_EQ(hid_to_amiga(0x2E), 0x0C);  // =
    CHECK_EQ(hid_to_amiga(0x31), 0x0D);  // backslash
    CHECK_EQ(hid_to_amiga(0x35), 0x00);  // `
    CHECK_EQ(hid_to_amiga(0x33), 0x29);  // ;
    CHECK_EQ(hid_to_amiga(0x34), 0x2A);  // '
    CHECK_EQ(hid_to_amiga(0x38), 0x3A);  // /
}

void iso_keys_have_their_own_codes() {
    CHECK_EQ(hid_to_amiga(0x32), 0x2B);  // key left of Return
    CHECK_EQ(hid_to_amiga(0x64), 0x30);  // key left of Z
}

void editing_cursor_and_function_keys() {
    CHECK_EQ(hid_to_amiga(0x28), 0x44);  // Enter
    CHECK_EQ(hid_to_amiga(0x29), 0x45);  // Esc
    CHECK_EQ(hid_to_amiga(0x2A), 0x41);  // Backspace
    CHECK_EQ(hid_to_amiga(0x2B), 0x42);  // Tab
    CHECK_EQ(hid_to_amiga(0x2C), 0x40);  // Space
    CHECK_EQ(hid_to_amiga(0x4C), 0x46);  // Delete
    CHECK_EQ(hid_to_amiga(0x49), 0x5F);  // Insert is Help
    CHECK_EQ(hid_to_amiga(0x52), 0x4C);  // up
    CHECK_EQ(hid_to_amiga(0x51), 0x4D);  // down
    CHECK_EQ(hid_to_amiga(0x4F), 0x4E);  // right
    CHECK_EQ(hid_to_amiga(0x50), 0x4F);  // left
    CHECK_EQ(hid_to_amiga(0x3A), 0x50);  // F1
    CHECK_EQ(hid_to_amiga(0x43), 0x59);  // F10
    CHECK_EQ(hid_to_amiga(0x39), 0x62);  // Caps Lock
}

void keypad_keys() {
    CHECK_EQ(hid_to_amiga(0x59), 0x1D);  // 1
    CHECK_EQ(hid_to_amiga(0x5C), 0x2D);  // 4
    CHECK_EQ(hid_to_amiga(0x5F), 0x3D);  // 7
    CHECK_EQ(hid_to_amiga(0x62), 0x0F);  // 0
    CHECK_EQ(hid_to_amiga(0x63), 0x3C);  // .
    CHECK_EQ(hid_to_amiga(0x58), 0x43);  // Enter
    CHECK_EQ(hid_to_amiga(0x54), 0x5C);  // /
    CHECK_EQ(hid_to_amiga(0x55), 0x5D);  // *
    CHECK_EQ(hid_to_amiga(0x56), 0x4A);  // -
    CHECK_EQ(hid_to_amiga(0x57), 0x5E);  // +
}

void unknown_usages_map_to_nothing() {
    CHECK_EQ(hid_to_amiga(0x00), kNoAmigaKey);
    CHECK_EQ(hid_to_amiga(0x01), kNoAmigaKey);
    CHECK_EQ(hid_to_amiga(0x53), kNoAmigaKey);  // Num Lock
    CHECK_EQ(hid_to_amiga(0xE0), kNoAmigaKey);
}

void qualifier_bits_of_the_modifiers() {
    CHECK_EQ(hid_qualifier(0, false), 0);
    CHECK_EQ(hid_qualifier(kModLShift, false), kQualLShift);
    CHECK_EQ(hid_qualifier(kModRShift, false), kQualRShift);
    CHECK_EQ(hid_qualifier(kModLCtrl, false), kQualControl);
    CHECK_EQ(hid_qualifier(kModRCtrl, false), kQualControl);
    CHECK_EQ(hid_qualifier(kModLAlt, false), kQualLAlt);
    CHECK_EQ(hid_qualifier(kModAltGr, false), kQualRAlt);
    CHECK_EQ(hid_qualifier(kModLGui, false), kQualLCommand);
    CHECK_EQ(hid_qualifier(kModRGui, false), kQualRCommand);
    CHECK_EQ(hid_qualifier(0, true), kQualCapsLock);
    CHECK_EQ(hid_qualifier(kModLShift, true), kQualLShift | kQualCapsLock);
}

void key_press_and_release() {
    RawKeyTracker tracker;
    Fed down = feed(tracker, 0, kUsageA);
    CHECK_EQ(down.count, 1u);
    CHECK_EQ(down.events[0].code, 0x20);
    CHECK_EQ(down.events[0].qualifier, 0);
    CHECK_EQ(feed(tracker, 0, kUsageA).count, 0u);  // held, no repeat
    Fed up = feed(tracker, 0);
    CHECK_EQ(up.count, 1u);
    CHECK_EQ(up.events[0].code, 0xA0);
}

void modifier_diffs_give_press_and_release() {
    RawKeyTracker tracker;
    Fed down = feed(tracker, kLShiftBit);
    CHECK_EQ(down.count, 1u);
    CHECK_EQ(down.events[0].code, 0x60);
    CHECK_EQ(down.events[0].qualifier, kQualLShift);
    Fed up = feed(tracker, 0);
    CHECK_EQ(up.count, 1u);
    CHECK_EQ(up.events[0].code, 0xE0);
    CHECK_EQ(up.events[0].qualifier, 0);
}

void every_modifier_has_its_code() {
    const u8 expected[8] = {0x63, 0x60, 0x64, 0x66, 0x63, 0x61, 0x65, 0x67};
    for (u32 bit = 0; bit < 8; ++bit) {
        RawKeyTracker tracker;
        Fed fed = feed(tracker, static_cast<u8>(1u << bit));
        CHECK_EQ(fed.count, 1u);
        CHECK_EQ(fed.events[0].code, expected[bit]);
    }
}

void a_key_pressed_with_shift_carries_the_qualifier() {
    RawKeyTracker tracker;
    Fed fed = feed(tracker, kLShiftBit, kUsageA);
    CHECK_EQ(fed.count, 2u);
    CHECK_EQ(fed.events[0].code, 0x60);  // the modifier before the key
    CHECK_EQ(fed.events[1].code, 0x20);
    CHECK_EQ(fed.events[1].qualifier, kQualLShift);
}

void a_key_released_with_shift_still_carries_it() {
    RawKeyTracker tracker;
    feed(tracker, kLShiftBit, kUsageA);
    Fed fed = feed(tracker, 0);
    CHECK_EQ(fed.count, 2u);
    CHECK_EQ(fed.events[0].code, 0xA0);  // the key goes up under Shift
    CHECK_EQ(fed.events[0].qualifier, kQualLShift);
    CHECK_EQ(fed.events[1].code, 0xE0);
    CHECK_EQ(fed.events[1].qualifier, 0);
}

void two_modifiers_in_one_report_build_up_the_qualifier() {
    RawKeyTracker tracker;
    Fed fed = feed(tracker, kLShiftBit | kLCtrlBit);
    CHECK_EQ(fed.count, 2u);
    CHECK_EQ(fed.events[0].code, 0x63);
    CHECK_EQ(fed.events[0].qualifier, kQualControl);
    CHECK_EQ(fed.events[1].code, 0x60);
    CHECK_EQ(fed.events[1].qualifier, kQualControl | kQualLShift);
    CHECK_EQ(tracker.qualifier(), kQualControl | kQualLShift);
}

void caps_lock_toggles_on_its_press() {
    RawKeyTracker tracker;
    Fed on = feed(tracker, 0, kUsageCapsLock);
    CHECK_EQ(on.count, 1u);
    CHECK_EQ(on.events[0].code, 0x62);
    CHECK_EQ(on.events[0].qualifier, kQualCapsLock);
    CHECK_EQ(tracker.qualifier(), kQualCapsLock);
    CHECK_EQ(feed(tracker, 0).count, 0u);  // releasing the key sends nothing

    Fed typed = feed(tracker, 0, kUsageA);
    CHECK_EQ(typed.events[0].qualifier, kQualCapsLock);
    feed(tracker, 0);

    Fed off = feed(tracker, 0, kUsageCapsLock);
    CHECK_EQ(off.count, 1u);
    CHECK_EQ(off.events[0].code, 0xE2);
    CHECK_EQ(off.events[0].qualifier, 0);
    CHECK_EQ(tracker.qualifier(), 0);
}

void keypad_keys_carry_numericpad() {
    RawKeyTracker tracker;
    Fed down = feed(tracker, 0, kUsageKeypad1);
    CHECK_EQ(down.events[0].code, 0x1D);
    CHECK_EQ(down.events[0].qualifier, kQualNumericPad);
    Fed up = feed(tracker, 0);
    CHECK_EQ(up.events[0].code, 0x9D);
    CHECK_EQ(up.events[0].qualifier, kQualNumericPad);
    Fed main_block = feed(tracker, 0, kUsageA);
    CHECK_EQ(main_block.events[0].qualifier, 0);
}

void unmapped_keys_give_no_event() {
    RawKeyTracker tracker;
    CHECK_EQ(feed(tracker, 0, 0x53).count, 0u);
    CHECK_EQ(feed(tracker, 0).count, 0u);
}

void two_keys_in_one_report_in_slot_order() {
    RawKeyTracker tracker;
    Fed fed = feed(tracker, 0, kUsageA, 0x05);
    CHECK_EQ(fed.count, 2u);
    CHECK_EQ(fed.events[0].code, 0x20);
    CHECK_EQ(fed.events[1].code, 0x35);
}

void rollover_error_report_is_ignored() {
    RawKeyTracker tracker;
    feed(tracker, 0, kUsageA);
    const u8 rollover[8] = {kLShiftBit, 0, 1, 1, 1, 1, 1, 1};
    RawKeyEvent events[RawKeyTracker::kMaxEvents];
    CHECK_EQ(tracker.feed(rollover, events, RawKeyTracker::kMaxEvents), 0u);
    CHECK_EQ(tracker.qualifier(), 0);
    CHECK_EQ(feed(tracker, 0, kUsageA).count, 0u);  // still held
}

void capacity_is_respected() {
    RawKeyTracker tracker;
    const u8 report[8] = {0xFF, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    RawKeyEvent events[3];
    CHECK_EQ(tracker.feed(report, events, 3), 3u);
}

}

int main() {
    letters_follow_the_physical_rows();
    digits_and_punctuation();
    iso_keys_have_their_own_codes();
    editing_cursor_and_function_keys();
    keypad_keys();
    unknown_usages_map_to_nothing();
    qualifier_bits_of_the_modifiers();
    key_press_and_release();
    modifier_diffs_give_press_and_release();
    every_modifier_has_its_code();
    a_key_pressed_with_shift_carries_the_qualifier();
    a_key_released_with_shift_still_carries_it();
    two_modifiers_in_one_report_build_up_the_qualifier();
    caps_lock_toggles_on_its_press();
    keypad_keys_carry_numericpad();
    unmapped_keys_give_no_event();
    two_keys_in_one_report_in_slot_order();
    rollover_error_report_is_ignored();
    capacity_is_respected();
    return check::finish("test_hid_rawkey");
}
