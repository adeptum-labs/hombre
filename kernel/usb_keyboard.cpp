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

#include "usb_keyboard.hpp"

#include "clock.hpp"

#include "../common/format.hpp"
#include "../common/serial.hpp"
#include "pci.hpp"
#include "xhci.hpp"

namespace hombre {

namespace {

constexpr u32 kMaxControllers = 4;

constinit Xhci controllers[kMaxControllers];
constinit u32 active_controller = 0;
constinit UsbStatus status = UsbStatus::NoController;
constinit char failure_text[64] = {};
constinit const char* last_message = nullptr;

void write_hex4(u32 value) {
    char text[5];
    const usize count = format_u64(text, sizeof(text), value, 16);
    for (usize i = count; i < 4; ++i) {
        serial::write_char('0');
    }
    serial::write(text);
}

void write_line(const char* message) {
    serial::write(message);
    serial::write_char('\n');
}

UsbResult publish(UsbStatus result, const char* message) {
    status = result;
    last_message = message;
    write_line(message);
    return UsbResult{result, message};
}

void check_mouse_lost() {
    Xhci::HidDevice& mouse = controllers[active_controller].mouse();
    if (mouse.state == Xhci::HidDevice::State::Lost) {
        mouse.state = Xhci::HidDevice::State::Absent;
        write_line("usb: mouse lost");
    }
}

// Builds the "usb: " line of a driver failure in `failure_text`, which then
// stays valid like any returned message.
const char* with_prefix(const char* error) {
    constexpr usize prefix = sizeof("usb: ") - 1;
    usize at = 0;
    for (; at < prefix; ++at) {
        failure_text[at] = "usb: "[at];
    }
    if (error != nullptr) {
        for (usize i = 0; error[i] != '\0' && at + 1 < sizeof(failure_text); ++i) {
            failure_text[at++] = error[i];
        }
    }
    failure_text[at] = '\0';
    return failure_text;
}

}

UsbResult usb_keyboard_start(MmioMapper& mmio, PhysAllocator& pages) {
    status = UsbStatus::NoController;
    active_controller = 0;

    if (!clock_init()) {
        return publish(UsbStatus::Failed, "usb: no usable clock");
    }

    PciAddress addresses[kMaxControllers];
    const u32 found = xhci_find(addresses, kMaxControllers);
    if (found == 0) {
        return publish(UsbStatus::NoController, "usb: no controller");
    }
    const u32 count = found < kMaxControllers ? found : kMaxControllers;
    bool controller_ready = false;
    bool hub = false;
    const char* scan_failure = nullptr;
    const char* init_failure = nullptr;
    for (u32 i = 0; i < count; ++i) {
        const char* error = nullptr;
        if (!controllers[i].init(addresses[i], mmio, pages, error)) {
            const char* reason = error != nullptr ? error : "controller init failed";
            serial::write("usb: ");
            serial::write(reason);
            serial::write_char('\n');
            if (init_failure == nullptr) {
                init_failure = reason;
            }
            continue;
        }
        controller_ready = true;
        const u32 ids = pci_read32(addresses[i], kPciVendorOffset);
        serial::write("usb: xhci ");
        write_hex4(ids & 0xFFFFu);
        serial::write_char(':');
        write_hex4((ids >> 16) & 0xFFFFu);
        serial::write_char('\n');

        const char* port_error = nullptr;
        const Xhci::Scan scan = controllers[i].scan(pages, port_error);
        if (scan == Xhci::Scan::Keyboard) {
            active_controller = i;
            const UsbResult result = publish(UsbStatus::Ready, "usb: keyboard ready");
            if (controllers[i].mouse_present()) {
                write_line("usb: mouse ready");
            }
            return result;
        }
        if (scan == Xhci::Scan::OnlyHub) {
            hub = true;
        }
        if (scan == Xhci::Scan::Failed && scan_failure == nullptr) {
            scan_failure = port_error;
        }
    }
    if (!controller_ready) {
        return publish(UsbStatus::Failed,
                       init_failure != nullptr ? with_prefix(init_failure)
                                               : "usb: controller init failed");
    }
    if (scan_failure != nullptr) {
        return publish(UsbStatus::Failed, with_prefix(scan_failure));
    }
    if (hub) {
        return publish(UsbStatus::OnlyHub,
                       "usb: only a hub is attached; hubs are not supported");
    }
    return publish(UsbStatus::NoKeyboard, "usb: no keyboard found");
}

UsbStatus usb_keyboard_status() {
    return status;
}

UsbResult usb_keyboard_result() {
    return UsbResult{status, last_message != nullptr ? last_message : "usb: not started"};
}

bool usb_keyboard_poll(u8 report[8]) {
    if (status != UsbStatus::Ready) {
        return false;
    }
    const Xhci::KeyEvent event = controllers[active_controller].poll_keyboard(report);
    check_mouse_lost();
    switch (event) {
    case Xhci::KeyEvent::Report:
        return true;
    case Xhci::KeyEvent::Lost:
        publish(UsbStatus::Failed, "usb: keyboard lost");
        return false;
    case Xhci::KeyEvent::None:
        break;
    }
    return false;
}

void usb_keyboard_drain() {
    u8 report[8] = {};
    while (usb_keyboard_poll(report)) {
    }
    u8 mouse_report[4] = {};
    while (usb_mouse_poll(mouse_report)) {
    }
}

bool usb_mouse_present() {
    check_mouse_lost();
    return controllers[active_controller].mouse_present();
}

bool usb_mouse_poll(u8 report[4]) {
    __builtin_memset(report, 0, 4);
    Xhci& controller = controllers[active_controller];
    u8 raw[xhci::ReportQueue::kMaxReport] = {};
    u32 length = 0;
    const Xhci::KeyEvent event = controller.poll_hid(controller.mouse(), raw, length);
    check_mouse_lost();
    if (event != Xhci::KeyEvent::Report) {
        return false;
    }
    __builtin_memcpy(report, raw, 4);
    return true;
}

}
