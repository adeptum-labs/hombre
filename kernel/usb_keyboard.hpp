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

namespace hombre {

enum class UsbStatus {
    NoController,
    NoKeyboard,
    OnlyHub,
    Failed,
    Ready
};

struct UsbResult {
    UsbStatus status;
    const char* message;
};

// Scans the xHCI controllers for a USB boot keyboard on a root port, brings
// the first one it finds up and leaves it delivering 8-byte reports. The
// message of the result is also written to the serial console.
UsbResult usb_keyboard_start(MmioMapper& mmio, PhysAllocator& pages);

UsbStatus usb_keyboard_status();

// The outcome of usb_keyboard_start, or of the keyboard since: its status
// and the "usb: ..." line that says why. Before a start, the line says so.
UsbResult usb_keyboard_result();

// Sees the reports in the interrupt before anything else does: false keeps a
// report back. A mouse report is at least 3 bytes long.
struct UsbFilter {
    bool (*key)(u8 (&report)[8]);
    bool (*mouse)(const u8* report, u32 length);
};

// Moves the reports the controller has into the queues the polls read. The
// timer interrupt calls it, and once the keyboard is ready nothing else
// touches the controller.
void usb_pump_from_interrupt(const UsbFilter* filter);

// Queues a keyboard and a mouse report with nothing held. Only a filter may
// call it.
void usb_deliver_release();

// Returns true when a new report arrived. A failed transfer or a device that
// went away marks the keyboard Failed. Returns false when nothing is pending.
bool usb_keyboard_poll(u8 report[8]);

// Reads and discards the reports that are pending.
void usb_keyboard_drain();

// Returns true while a configured USB boot mouse is Ready.
bool usb_mouse_present();

// Pops one pending mouse report (up to 4 bytes, zero-padded). A failed transfer or unplugged mouse logs "usb: mouse lost" on
// serial and clears usb_mouse_present() without affecting keyboard status.
bool usb_mouse_poll(u8 report[4]);

}
