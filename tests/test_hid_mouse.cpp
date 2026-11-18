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
#include "hid_mouse.hpp"

using namespace hombre;

namespace {

void decode_reads_buttons_and_signed_deltas() {
    const u8 report[3] = {0x01, 0x05, 0xFB};
    MouseSample sample{};
    CHECK(decode_mouse_report(report, sizeof(report), sample));
    CHECK_EQ(sample.buttons, kMouseLeft);
    CHECK_EQ(sample.dx, 5);
    CHECK_EQ(sample.dy, -5);

    const u8 report2[3] = {0x02, 0x80, 0x7F};
    CHECK(decode_mouse_report(report2, sizeof(report2), sample));
    CHECK_EQ(sample.buttons, kMouseRight);
    CHECK_EQ(sample.dx, -128);
    CHECK_EQ(sample.dy, 127);

    const u8 report3[3] = {0x04, 0xFF, 0x01};
    CHECK(decode_mouse_report(report3, sizeof(report3), sample));
    CHECK_EQ(sample.buttons, kMouseMiddle);
    CHECK_EQ(sample.dx, -1);
    CHECK_EQ(sample.dy, 1);
}

void decode_ignores_wheel_and_extra_bytes() {
    const u8 report[5] = {kMouseLeft, 10, 20, 1, 99};
    MouseSample sample{};
    CHECK(decode_mouse_report(report, sizeof(report), sample));
    CHECK_EQ(sample.buttons, kMouseLeft);
    CHECK_EQ(sample.dx, 10);
    CHECK_EQ(sample.dy, 20);

    const u8 report4[4] = {kMouseRight, 0xFE, 0xFD, 0x7F};
    CHECK(decode_mouse_report(report4, sizeof(report4), sample));
    CHECK_EQ(sample.buttons, kMouseRight);
    CHECK_EQ(sample.dx, -2);
    CHECK_EQ(sample.dy, -3);
}

void decode_rejects_short_and_null_reports() {
    MouseSample sample{0xFF, 42, 42};
    CHECK(!decode_mouse_report(nullptr, 3, sample));
    CHECK(!decode_mouse_report(nullptr, 0, sample));

    const u8 report[2] = {1, 2};
    CHECK(!decode_mouse_report(report, 0, sample));
    CHECK(!decode_mouse_report(report, 1, sample));
    CHECK(!decode_mouse_report(report, 2, sample));
}

void decode_masks_the_button_bits() {
    const u8 report[3] = {0xFF, 0, 0};
    MouseSample sample{};
    CHECK(decode_mouse_report(report, sizeof(report), sample));
    CHECK_EQ(sample.buttons, static_cast<u8>(7));
    CHECK_EQ(sample.buttons, static_cast<u8>(kMouseLeft | kMouseRight | kMouseMiddle));

    const u8 report2[3] = {0xF8, 0, 0};
    CHECK(decode_mouse_report(report2, sizeof(report2), sample));
    CHECK_EQ(sample.buttons, static_cast<u8>(0));
}

void apply_moves_and_reports_moved() {
    PointerState state;
    state.init(640, 400, 100, 100);
    CHECK_EQ(state.x(), 100);
    CHECK_EQ(state.y(), 100);

    const MouseSample sample{0, 15, -20};
    const PointerState::Update u = state.apply(sample);
    CHECK(u.moved);
    CHECK_EQ(u.pressed, static_cast<u8>(0));
    CHECK_EQ(u.released, static_cast<u8>(0));
    CHECK_EQ(state.x(), 115);
    CHECK_EQ(state.y(), 80);

    const MouseSample zero{0, 0, 0};
    const PointerState::Update u2 = state.apply(zero);
    CHECK(!u2.moved);
    CHECK_EQ(state.x(), 115);
    CHECK_EQ(state.y(), 80);
}

void apply_clamps_at_every_edge_and_reports_not_moved_when_pinned() {
    PointerState state;
    state.init(640, 400, 10, 10);

    // Left edge
    PointerState::Update u = state.apply(MouseSample{0, -50, 0});
    CHECK(u.moved);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 10);
    u = state.apply(MouseSample{0, -10, 0});
    CHECK(!u.moved);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 10);

    // Top edge
    u = state.apply(MouseSample{0, 0, -50});
    CHECK(u.moved);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);
    u = state.apply(MouseSample{0, 0, -10});
    CHECK(!u.moved);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);

    // Right edge
    state.set_position(630, 200);
    u = state.apply(MouseSample{0, 50, 0});
    CHECK(u.moved);
    CHECK_EQ(state.x(), 639);
    CHECK_EQ(state.y(), 200);
    u = state.apply(MouseSample{0, 10, 0});
    CHECK(!u.moved);
    CHECK_EQ(state.x(), 639);
    CHECK_EQ(state.y(), 200);

    // Bottom edge
    state.set_position(200, 390);
    u = state.apply(MouseSample{0, 0, 50});
    CHECK(u.moved);
    CHECK_EQ(state.x(), 200);
    CHECK_EQ(state.y(), 399);
    u = state.apply(MouseSample{0, 0, 10});
    CHECK(!u.moved);
    CHECK_EQ(state.x(), 200);
    CHECK_EQ(state.y(), 399);

    // Bottom-right corner pinned
    state.set_position(639, 399);
    u = state.apply(MouseSample{0, 5, 5});
    CHECK(!u.moved);
    CHECK_EQ(state.x(), 639);
    CHECK_EQ(state.y(), 399);
}

void apply_reports_press_and_release_once() {
    PointerState state;
    state.init(640, 400, 50, 50);

    PointerState::Update u1 = state.apply(MouseSample{kMouseLeft, 0, 0});
    CHECK_EQ(u1.pressed, kMouseLeft);
    CHECK_EQ(u1.released, static_cast<u8>(0));
    CHECK_EQ(state.buttons(), kMouseLeft);

    PointerState::Update u2 = state.apply(MouseSample{kMouseLeft, 1, 1});
    CHECK_EQ(u2.pressed, static_cast<u8>(0));
    CHECK_EQ(u2.released, static_cast<u8>(0));
    CHECK_EQ(state.buttons(), kMouseLeft);

    PointerState::Update u3 = state.apply(MouseSample{kMouseLeft, 0, 0});
    CHECK_EQ(u3.pressed, static_cast<u8>(0));
    CHECK_EQ(u3.released, static_cast<u8>(0));
    CHECK_EQ(state.buttons(), kMouseLeft);

    PointerState::Update u4 = state.apply(MouseSample{0, 0, 0});
    CHECK_EQ(u4.pressed, static_cast<u8>(0));
    CHECK_EQ(u4.released, kMouseLeft);
    CHECK_EQ(state.buttons(), static_cast<u8>(0));

    PointerState::Update u5 = state.apply(MouseSample{0, 0, 0});
    CHECK_EQ(u5.pressed, static_cast<u8>(0));
    CHECK_EQ(u5.released, static_cast<u8>(0));
    CHECK_EQ(state.buttons(), static_cast<u8>(0));
}

void apply_two_buttons_in_one_sample() {
    PointerState state;
    state.init(640, 400, 50, 50);

    const u8 left_right = static_cast<u8>(kMouseLeft | kMouseRight);
    PointerState::Update u1 = state.apply(MouseSample{left_right, 0, 0});
    CHECK_EQ(u1.pressed, left_right);
    CHECK_EQ(u1.released, static_cast<u8>(0));
    CHECK_EQ(state.buttons(), left_right);

    PointerState::Update u2 = state.apply(MouseSample{kMouseMiddle, 0, 0});
    CHECK_EQ(u2.pressed, kMouseMiddle);
    CHECK_EQ(u2.released, left_right);
    CHECK_EQ(state.buttons(), kMouseMiddle);

    PointerState::Update u3 = state.apply(MouseSample{left_right, 0, 0});
    CHECK_EQ(u3.pressed, left_right);
    CHECK_EQ(u3.released, kMouseMiddle);
    CHECK_EQ(state.buttons(), left_right);

    PointerState::Update u4 = state.apply(MouseSample{0, 0, 0});
    CHECK_EQ(u4.pressed, static_cast<u8>(0));
    CHECK_EQ(u4.released, left_right);
    CHECK_EQ(state.buttons(), static_cast<u8>(0));
}

void init_clamps_a_start_position_outside_the_screen() {
    PointerState state;
    state.init(640, 400, -50, -100);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);
    CHECK_EQ(state.width(), 640u);
    CHECK_EQ(state.height(), 400u);

    state.init(640, 400, 1000, 800);
    CHECK_EQ(state.x(), 639);
    CHECK_EQ(state.y(), 399);

    state.init(640, 400, 200, 150);
    CHECK_EQ(state.x(), 200);
    CHECK_EQ(state.y(), 150);
}

void zero_size_screen_pins_to_zero() {
    PointerState state;
    state.init(0, 0, 100, 100);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);
    CHECK_EQ(state.width(), 0u);
    CHECK_EQ(state.height(), 0u);

    PointerState::Update u1 = state.apply(MouseSample{0, 50, 50});
    CHECK(!u1.moved);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);

    state.set_position(50, 50);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);

    state.init(0, 400, 10, 20);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);
    state.set_position(10, 20);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);

    state.init(640, 0, 10, 20);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);
    state.set_position(10, 20);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);
}

void set_position_clamps() {
    PointerState state;
    state.init(640, 400, 100, 100);

    state.set_position(-10, 200);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 200);

    state.set_position(700, 200);
    CHECK_EQ(state.x(), 639);
    CHECK_EQ(state.y(), 200);

    state.set_position(300, -20);
    CHECK_EQ(state.x(), 300);
    CHECK_EQ(state.y(), 0);

    state.set_position(300, 500);
    CHECK_EQ(state.x(), 300);
    CHECK_EQ(state.y(), 399);

    state.set_position(-5, -5);
    CHECK_EQ(state.x(), 0);
    CHECK_EQ(state.y(), 0);

    state.set_position(1000, 1000);
    CHECK_EQ(state.x(), 639);
    CHECK_EQ(state.y(), 399);

    state.set_position(320, 200);
    CHECK_EQ(state.x(), 320);
    CHECK_EQ(state.y(), 200);
}

}

int main() {
    decode_reads_buttons_and_signed_deltas();
    decode_ignores_wheel_and_extra_bytes();
    decode_rejects_short_and_null_reports();
    decode_masks_the_button_bits();
    apply_moves_and_reports_moved();
    apply_clamps_at_every_edge_and_reports_not_moved_when_pinned();
    apply_reports_press_and_release_once();
    apply_two_buttons_in_one_sample();
    init_clamps_a_start_position_outside_the_screen();
    zero_size_screen_pins_to_zero();
    set_position_clamps();
    return check::finish("test_hid_mouse");
}
