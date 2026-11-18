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

void write_hex4(u32 value) {
    char text[5];
    const usize count = format_u64(text, sizeof(text), value, 16);
    for (usize i = count; i < 4; ++i) {
        serial::write_char('0');
    }
    serial::write(text);
}

UsbResult publish(UsbStatus result, const char* message) {
    status = result;
    serial::write(message);
    serial::write_char('\n');
    return UsbResult{result, message};
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

    PciAddress addresses[kMaxControllers];
    const u32 found = xhci_find(addresses, kMaxControllers);
    if (found == 0) {
        return publish(UsbStatus::NoController, "usb: no controller");
    }
    const u32 count = found < kMaxControllers ? found : kMaxControllers;
    bool controller_ready = false;
    bool hub = false;
    const char* scan_failure = nullptr;
    for (u32 i = 0; i < count; ++i) {
        const char* error = nullptr;
        if (!controllers[i].init(addresses[i], mmio, pages, error)) {
            serial::write("usb: ");
            serial::write(error != nullptr ? error : "controller init failed");
            serial::write_char('\n');
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
            return publish(UsbStatus::Ready, "usb: keyboard ready");
        }
        if (scan == Xhci::Scan::OnlyHub) {
            hub = true;
        }
        if (scan == Xhci::Scan::Failed && scan_failure == nullptr) {
            scan_failure = port_error;
        }
    }
    if (scan_failure != nullptr) {
        return publish(UsbStatus::Failed, with_prefix(scan_failure));
    }
    if (hub) {
        return publish(UsbStatus::OnlyHub,
                       "usb: only a hub is attached; hubs are not supported");
    }
    if (!controller_ready) {
        return publish(UsbStatus::Failed, "usb: controller init failed");
    }
    return publish(UsbStatus::NoKeyboard, "usb: no keyboard found");
}

UsbStatus usb_keyboard_status() {
    return status;
}

bool usb_keyboard_poll(u8 report[8]) {
    if (status != UsbStatus::Ready) {
        return false;
    }
    switch (controllers[active_controller].poll_keyboard(report)) {
    case Xhci::KeyEvent::Report:
        return true;
    case Xhci::KeyEvent::Lost:
        status = UsbStatus::Failed;
        serial::write("usb: keyboard lost\n");
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
}

}
