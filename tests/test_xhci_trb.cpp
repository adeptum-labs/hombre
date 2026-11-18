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
#include "xhci_trb.hpp"

using namespace hombre;

namespace {

void trb_builders_set_type_and_fields() {
    const xhci::Trb noop = xhci::make_noop_command();
    CHECK_EQ(xhci::trb_type(noop.control), 23u);
    CHECK_EQ(noop.parameter, 0u);
    CHECK_EQ(noop.status, 0u);

    CHECK_EQ(xhci::trb_type(xhci::make_enable_slot().control), 9u);

    const xhci::Trb address = xhci::make_address_device(0x2000, 5, true);
    CHECK_EQ(xhci::trb_type(address.control), 11u);
    CHECK_EQ(address.parameter, 0x2000u);
    CHECK_EQ((address.control >> 24) & 0xFFu, 5u);
    CHECK(address.control & (1u << 9));
    CHECK(!(xhci::make_address_device(0x2000, 5, false).control & (1u << 9)));

    const xhci::Trb configure = xhci::make_configure_endpoint(0x3000, 7);
    CHECK_EQ(xhci::trb_type(configure.control), 12u);
    CHECK_EQ(configure.parameter, 0x3000u);
    CHECK_EQ((configure.control >> 24) & 0xFFu, 7u);

    const xhci::Trb link = xhci::make_link(0x4000, true);
    CHECK_EQ(xhci::trb_type(link.control), 6u);
    CHECK_EQ(link.parameter, 0x4000u);
    CHECK(link.control & 2u);
    CHECK(!(xhci::make_link(0x4000, false).control & 2u));

    const xhci::Trb setup = xhci::make_setup_stage(0x21, 0x0A, 0x0200, 0, 8, 3);
    CHECK_EQ(xhci::trb_type(setup.control), 2u);
    CHECK_EQ(setup.parameter, 0x0008000002000A21ull);
    CHECK_EQ(setup.status, 8u);
    CHECK(setup.control & (1u << 6));
    CHECK_EQ((setup.control >> 16) & 3u, 3u);

    const xhci::Trb data_in = xhci::make_data_stage(0x5000, 64, true);
    CHECK_EQ(xhci::trb_type(data_in.control), 3u);
    CHECK_EQ(data_in.parameter, 0x5000u);
    CHECK_EQ(data_in.status, 64u);
    CHECK(data_in.control & (1u << 16));
    CHECK(!(xhci::make_data_stage(0x5000, 64, false).control & (1u << 16)));

    const xhci::Trb status_in = xhci::make_status_stage(true);
    CHECK_EQ(xhci::trb_type(status_in.control), 4u);
    CHECK(status_in.control & (1u << 16));
    CHECK(status_in.control & (1u << 5));
    CHECK(!(xhci::make_status_stage(false).control & (1u << 16)));

    const xhci::Trb normal = xhci::make_normal(0x6000, 32);
    CHECK_EQ(xhci::trb_type(normal.control), 1u);
    CHECK_EQ(normal.parameter, 0x6000u);
    CHECK_EQ(normal.status, 32u);
    CHECK(normal.control & (1u << 5));
}

void context_and_endpoint_builders_set_type_and_fields() {
    const xhci::Trb evaluate = xhci::make_evaluate_context(0x7000, 6);
    CHECK_EQ(xhci::trb_type(evaluate.control), 13u);
    CHECK_EQ(evaluate.parameter, 0x7000u);
    CHECK_EQ((evaluate.control >> 24) & 0xFFu, 6u);

    const xhci::Trb reset = xhci::make_reset_endpoint(4, 3);
    CHECK_EQ(xhci::trb_type(reset.control), 14u);
    CHECK_EQ(reset.parameter, 0u);
    CHECK_EQ((reset.control >> 24) & 0xFFu, 4u);
    CHECK_EQ((reset.control >> 16) & 0x1Fu, 3u);

    const xhci::Trb dequeue = xhci::make_set_tr_dequeue_pointer(0x9001, 4, 3);
    CHECK_EQ(xhci::trb_type(dequeue.control), 16u);
    CHECK_EQ(dequeue.parameter, 0x9001u);
    CHECK_EQ((dequeue.control >> 24) & 0xFFu, 4u);
    CHECK_EQ((dequeue.control >> 16) & 0x1Fu, 3u);
}

void scratchpad_count_swaps_hi_and_lo_correctly() {
    CHECK_EQ(xhci::scratchpad_count(1u << 21), 32u);
    CHECK_EQ(xhci::scratchpad_count(31u << 27), 31u);
    CHECK_EQ(xhci::scratchpad_count(0), 0u);
    CHECK_EQ(xhci::scratchpad_count((31u << 27) | (31u << 21)), 1023u);
}

void next_extended_capability_is_relative() {
    CHECK_EQ(xhci::next_extended_capability(0x20, 4u << 8), 0x30u);
    CHECK_EQ(xhci::next_extended_capability(0x20, 0), 0u);
    CHECK_EQ(xhci::next_extended_capability(0x40, 2u << 8), 0x48u);
}

void producer_ring_writes_cycle_bits_and_wraps() {
    xhci::Trb ring[8] = {};
    xhci::ProducerRing producer;
    producer.init(ring, 8, 0x8000);
    CHECK(producer.cycle());
    CHECK_EQ(producer.enqueue_index(), 0u);

    // Seven data slots and one Link TRB, so the seventh push closes the lap.
    for (u32 i = 0; i < 7; ++i) {
        CHECK(producer.push(xhci::make_normal(0x1000 + i, 8)));
    }
    CHECK_EQ(producer.enqueue_index(), 0u);
    CHECK(!producer.cycle());
    for (u32 i = 0; i < 7; ++i) {
        CHECK_EQ(ring[i].control & 1u, 1u);
        CHECK_EQ(ring[i].parameter, 0x1000u + i);
    }
    CHECK_EQ(xhci::trb_type(ring[7].control), 6u);
    // The Link TRB holds the physical base the ring was given, not the pointer.
    CHECK_EQ(ring[7].parameter, 0x8000u);
    CHECK_EQ(ring[7].control & 1u, 1u);
    CHECK(ring[7].control & 2u);

    for (u32 i = 7; i < 20; ++i) {
        CHECK(producer.push(xhci::make_normal(0x1000 + i, 8)));
    }
    CHECK_EQ(producer.enqueue_index(), 6u);
    CHECK(producer.cycle());
    for (u32 i = 0; i < 6; ++i) {
        CHECK_EQ(ring[i].control & 1u, 1u);
        CHECK_EQ(ring[i].parameter, 0x1000u + 14 + i);
    }
    CHECK_EQ(ring[6].control & 1u, 0u);
    CHECK_EQ(ring[6].parameter, 0x1000u + 13);
    CHECK_EQ(ring[7].control & 1u, 0u);
}

void producer_ring_rejects_tiny_rings() {
    xhci::Trb ring[4] = {};
    xhci::ProducerRing producer;
    producer.init(ring, 3, 0x8000);
    CHECK(!producer.push(xhci::make_noop_command()));
    producer.init(nullptr, 0, 0);
    CHECK(!producer.push(xhci::make_noop_command()));
    CHECK_EQ(producer.enqueue_index(), 0u);
}

void event_ring_pops_in_order_and_wraps() {
    xhci::Trb ring[4] = {};
    xhci::EventRing consumer;
    consumer.init(ring, 4);
    xhci::Trb event = {};

    // The slot still belongs to the controller's previous lap.
    ring[0].control = xhci::kTrbTransferEvent << 10;
    CHECK(!consumer.pop(event));
    CHECK_EQ(consumer.dequeue_index(), 0u);

    for (u32 i = 0; i < 4; ++i) {
        ring[i].parameter = i;
        ring[i].control = (xhci::kTrbTransferEvent << 10) | 1u;
    }
    for (u32 i = 0; i < 4; ++i) {
        CHECK(consumer.pop(event));
        CHECK_EQ(event.parameter, i);
        CHECK_EQ(xhci::event_type(event), 32u);
    }
    CHECK_EQ(consumer.dequeue_index(), 0u);

    // The consumer cycle flipped with the wrap, so the old lap is not read.
    CHECK(!consumer.pop(event));
    ring[0].control &= ~1u;
    CHECK(consumer.pop(event));
    CHECK_EQ(event.parameter, 0u);
    CHECK_EQ(consumer.dequeue_index(), 1u);
    CHECK(!consumer.pop(event));
}

void event_decoding() {
    xhci::Trb transfer = {};
    transfer.control = (xhci::kTrbTransferEvent << 10) | (3u << 24) | (2u << 16);
    transfer.status = 13u << 24;
    CHECK_EQ(xhci::event_type(transfer), 32u);
    CHECK_EQ(xhci::completion_code(transfer), 13u);
    CHECK_EQ(static_cast<u32>(xhci::event_slot(transfer)), 3u);
    CHECK_EQ(static_cast<u32>(xhci::event_endpoint(transfer)), 2u);

    xhci::Trb port = {};
    port.parameter = 5u << 24;
    port.control = xhci::kTrbPortStatusChangeEvent << 10;
    CHECK_EQ(xhci::event_type(port), 34u);
    CHECK_EQ(static_cast<u32>(xhci::port_id(port)), 5u);

    xhci::Trb command = {};
    command.control = xhci::kTrbCommandCompletionEvent << 10;
    command.status = 1u << 24;
    CHECK_EQ(xhci::event_type(command), 33u);
    CHECK_EQ(xhci::completion_code(command), 1u);
}

constexpr u8 kQemuKeyboard[] = {
    9, 2, 34, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 63, 0,
    7, 5, 0x81, 3, 8, 0, 10,
};

void find_boot_keyboard_in_a_qemu_style_descriptor() {
    xhci::KeyboardInterface keyboard = {};
    CHECK(xhci::find_boot_keyboard(kQemuKeyboard, sizeof(kQemuKeyboard), keyboard));
    CHECK_EQ(keyboard.configuration_value, 1);
    CHECK_EQ(keyboard.interface_number, 0);
    CHECK_EQ(keyboard.endpoint_address, 0x81);
    CHECK_EQ(keyboard.max_packet, 8);
    CHECK_EQ(keyboard.interval, 10);
}

constexpr u8 kQemuMouse[] = {
    9, 2, 34, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 1, 2, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 63, 0,
    7, 5, 0x81, 3, 8, 0, 10,
};

void find_boot_keyboard_rejects_other_hid() {
    xhci::KeyboardInterface keyboard = {};
    CHECK(!xhci::find_boot_keyboard(kQemuMouse, sizeof(kQemuMouse), keyboard));
}

void find_boot_keyboard_rejects_truncated_and_zero_length_descriptors() {
    xhci::KeyboardInterface keyboard = {};
    CHECK(!xhci::find_boot_keyboard(kQemuKeyboard, 20, keyboard));

    constexpr u8 kZeroLengthDescriptor[] = {
        9, 2, 18, 0, 1, 1, 0, 0x80, 50,
        0, 4,
    };
    CHECK(!xhci::find_boot_keyboard(kZeroLengthDescriptor, sizeof(kZeroLengthDescriptor),
                                    keyboard));

    constexpr u8 kHalfDescriptor[] = {
        9, 2, 10, 0, 1, 1, 0, 0x80, 50,
        4,
    };
    CHECK(!xhci::find_boot_keyboard(kHalfDescriptor, sizeof(kHalfDescriptor), keyboard));
}

void endpoint_interval_conversion() {
    CHECK_EQ(static_cast<u32>(xhci::endpoint_interval(1, 10)), 6u);
    CHECK_EQ(static_cast<u32>(xhci::endpoint_interval(1, 16)), 7u);
    CHECK_EQ(static_cast<u32>(xhci::endpoint_interval(1, 1)), 3u);
    CHECK_EQ(static_cast<u32>(xhci::endpoint_interval(2, 255)), 10u);
    CHECK_EQ(static_cast<u32>(xhci::endpoint_interval(3, 4)), 3u);
    CHECK_EQ(static_cast<u32>(xhci::endpoint_interval(3, 1)), 0u);
    CHECK_EQ(static_cast<u32>(xhci::endpoint_interval(3, 0)), 0u);
    CHECK_EQ(static_cast<u32>(xhci::endpoint_interval(4, 16)), 15u);
}

constexpr u8 kBootMouse[] = {
    9, 2, 34, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 1, 2, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 50, 0,
    7, 5, 0x81, 3, 4, 0, 10,
};

void find_boot_hid_finds_a_mouse_interface() {
    xhci::KeyboardInterface mouse = {};
    CHECK(xhci::find_boot_hid(kBootMouse, sizeof(kBootMouse), xhci::kMouseProtocol, mouse));
    CHECK_EQ(mouse.configuration_value, 1);
    CHECK_EQ(mouse.interface_number, 0);
    CHECK_EQ(mouse.endpoint_address, 0x81);
    CHECK_EQ(mouse.max_packet, 4);
    CHECK_EQ(mouse.interval, 10);
}

void find_boot_hid_rejects_the_other_protocol() {
    xhci::KeyboardInterface out = {};
    CHECK(!xhci::find_boot_hid(kQemuKeyboard, sizeof(kQemuKeyboard), xhci::kMouseProtocol, out));
    CHECK(!xhci::find_boot_hid(kBootMouse, sizeof(kBootMouse), xhci::kKeyboardProtocol, out));
}

void find_boot_keyboard_still_rejects_a_mouse() {
    xhci::KeyboardInterface out = {};
    CHECK(!xhci::find_boot_keyboard(kBootMouse, sizeof(kBootMouse), out));
}

void report_queue_is_fifo() {
    xhci::ReportQueue queue;
    CHECK_EQ(queue.size(), 0u);

    const u8 first[3] = {0x01, 0x10, 0x20};
    const u8 second[4] = {0x02, 0x30, 0x40, 0x50};
    const u8 third[8] = {0x04, 0x00, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    CHECK(queue.push(first, sizeof(first)));
    CHECK(queue.push(second, sizeof(second)));
    CHECK(queue.push(third, sizeof(third)));
    CHECK_EQ(queue.size(), 3u);

    u8 out[8] = {};
    u32 length = 0;
    CHECK(queue.pop(out, length));
    CHECK_EQ(length, 3u);
    CHECK_EQ(out[0], 0x01);
    CHECK_EQ(out[1], 0x10);
    CHECK_EQ(out[2], 0x20);
    CHECK_EQ(queue.size(), 2u);

    CHECK(queue.pop(out, length));
    CHECK_EQ(length, 4u);
    CHECK_EQ(out[0], 0x02);
    CHECK_EQ(out[1], 0x30);
    CHECK_EQ(out[2], 0x40);
    CHECK_EQ(out[3], 0x50);
    CHECK_EQ(queue.size(), 1u);

    CHECK(queue.pop(out, length));
    CHECK_EQ(length, 8u);
    for (u32 i = 0; i < 8; ++i) {
        CHECK_EQ(out[i], third[i]);
    }
    CHECK_EQ(queue.size(), 0u);
}

void report_queue_drops_the_oldest_when_full() {
    xhci::ReportQueue queue;
    for (u32 i = 0; i < xhci::ReportQueue::kCapacity + 2; ++i) {
        const u8 report[2] = {static_cast<u8>(i), static_cast<u8>(i + 100)};
        CHECK(queue.push(report, sizeof(report)));
    }
    CHECK_EQ(queue.size(), xhci::ReportQueue::kCapacity);

    for (u32 i = 0; i < xhci::ReportQueue::kCapacity; ++i) {
        u8 out[8] = {};
        u32 length = 0;
        CHECK(queue.pop(out, length));
        CHECK_EQ(length, 2u);
        CHECK_EQ(out[0], static_cast<u8>(i + 2));
        CHECK_EQ(out[1], static_cast<u8>(i + 102));
    }
    CHECK_EQ(queue.size(), 0u);
}

void report_queue_truncates_long_reports_and_rejects_empty_ones() {
    xhci::ReportQueue queue;
    const u8 long_report[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(!queue.push(nullptr, 4));
    CHECK(!queue.push(long_report, 0));
    CHECK_EQ(queue.size(), 0u);

    CHECK(queue.push(long_report, sizeof(long_report)));
    CHECK_EQ(queue.size(), 1u);

    u8 out[12] = {};
    u32 length = 0;
    CHECK(queue.pop(out, length));
    CHECK_EQ(length, xhci::ReportQueue::kMaxReport);
    for (u32 i = 0; i < xhci::ReportQueue::kMaxReport; ++i) {
        CHECK_EQ(out[i], long_report[i]);
    }
    CHECK_EQ(out[8], 0);
}

void report_queue_pop_on_empty_returns_false() {
    xhci::ReportQueue queue;
    u8 out[8] = {};
    u32 length = 99;
    CHECK(!queue.pop(out, length));
    CHECK_EQ(length, 99u);

    const u8 report[4] = {1, 2, 3, 4};
    CHECK(queue.push(report, sizeof(report)));
    queue.clear();
    CHECK_EQ(queue.size(), 0u);
    CHECK(!queue.pop(out, length));
}

void default_ep0_max_packet() {
    CHECK_EQ(xhci::default_ep0_max_packet(2), 8);
    CHECK_EQ(xhci::default_ep0_max_packet(1), 64);
    CHECK_EQ(xhci::default_ep0_max_packet(3), 64);
    CHECK_EQ(xhci::default_ep0_max_packet(4), 512);
    CHECK_EQ(xhci::default_ep0_max_packet(5), 512);
}

void interrupt_transfer_length_is_the_masked_max_packet_capped_at_64() {
    CHECK_EQ(xhci::interrupt_transfer_length(4), 4);
    CHECK_EQ(xhci::interrupt_transfer_length(8), 8);
    CHECK_EQ(xhci::interrupt_transfer_length(16), 16);
    CHECK_EQ(xhci::interrupt_transfer_length(64), 64);
    CHECK_EQ(xhci::interrupt_transfer_length(65), 64);
    CHECK_EQ(xhci::interrupt_transfer_length(512), 64);
    CHECK_EQ(xhci::interrupt_transfer_length(static_cast<u16>((2u << 11) | 8u)), 8);
    CHECK_EQ(xhci::interrupt_transfer_length(static_cast<u16>((1u << 11) | 16u)), 16);
    CHECK_EQ(xhci::interrupt_transfer_length(static_cast<u16>((2u << 11) | 128u)), 64);
}

}

int main() {
    trb_builders_set_type_and_fields();
    context_and_endpoint_builders_set_type_and_fields();
    scratchpad_count_swaps_hi_and_lo_correctly();
    next_extended_capability_is_relative();
    producer_ring_writes_cycle_bits_and_wraps();
    producer_ring_rejects_tiny_rings();
    event_ring_pops_in_order_and_wraps();
    event_decoding();
    find_boot_keyboard_in_a_qemu_style_descriptor();
    find_boot_keyboard_rejects_other_hid();
    find_boot_keyboard_rejects_truncated_and_zero_length_descriptors();
    find_boot_hid_finds_a_mouse_interface();
    find_boot_hid_rejects_the_other_protocol();
    find_boot_keyboard_still_rejects_a_mouse();
    report_queue_is_fifo();
    report_queue_drops_the_oldest_when_full();
    report_queue_truncates_long_reports_and_rejects_empty_ones();
    report_queue_pop_on_empty_returns_false();
    endpoint_interval_conversion();
    default_ep0_max_packet();
    interrupt_transfer_length_is_the_masked_max_packet_capped_at_64();
    return check::finish("test_xhci_trb");
}
