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
#include "../common/xhci_trb.hpp"
#include "mmio.hpp"
#include "page_alloc.hpp"
#include "pci.hpp"

namespace hombre {

// Finds every PCI function with class 0x0C, subclass 0x03 and programming
// interface 0x30. Writes up to `capacity` of their addresses to `out` and
// returns how many functions there are; `out` may be null to only count.
u32 xhci_find(PciAddress* out, u32 capacity);

// An xHCI controller brought up for polling: interrupts stay off and every
// wait has a deadline. init() takes the controller over from the firmware,
// resets it and leaves it running on empty command and event rings;
// command() and poll_event() drive it from there.
//
// The default constructor is constant, so a file-scope object needs no
// initialisation code.
class Xhci {
public:
    constexpr Xhci() = default;

    // How far a scan of the ports got.
    enum class Scan {
        Keyboard,   // a boot keyboard is configured and delivering reports
        OnlyHub,    // a hub was found and nothing else
        NoKeyboard, // devices were seen but none is a boot keyboard
        Failed      // a step failed; `error` says which
    };

    // What polling the keyboard endpoint found.
    enum class KeyEvent {
        None,   // nothing pending
        Report, // one 8-byte report was taken
        Lost    // the endpoint failed or the device went away
    };

    // Brings the controller up. On failure returns false and points `error`
    // at a static description; on success `error` is null.
    bool init(PciAddress address, MmioMapper& mmio, PhysAllocator& pages, const char*& error);

    // The number of ports HCSPARAMS1 reports.
    u32 port_count() const { return ports_; }

    // The number of slots enabled in CONFIG: the controller's maximum,
    // capped at 64.
    u32 slot_count() const { return slots_; }

    // Bytes per device context: 32, or 64 when the controller asks for it.
    u32 context_size() const { return context_size_; }

    // The operational register window (USBCMD, and the port register sets
    // at 0x400), the runtime window and the doorbell array.
    volatile u8* operational() const { return operational_; }
    volatile u8* runtime() const { return runtime_; }
    volatile u32* doorbells() const { return doorbells_; }

    // The device context base address array. Entry 0 points at the
    // scratchpad pointer array when the controller uses scratchpads.
    u64* dcbaa() const { return dcbaa_; }
    u64 dcbaa_phys() const { return dcbaa_phys_; }

    // Queues `trb` on the command ring, rings doorbell 0 and waits up to one
    // second for the Command Completion event that names the queued TRB.
    // Returns false when the ring is full or the event does not arrive;
    // `completion` is only valid on true and its completion code is left to
    // the caller.
    bool command(xhci::Trb trb, xhci::Trb& completion);

    // Takes the next event the controller posted and moves the event ring
    // dequeue pointer past it. Returns false when no event is pending.
    bool poll_event(xhci::Trb& out);

    // Scans every port: each connected device is reset if it sits on a USB 2
    // port, addressed, and asked for its descriptors. The first boot keyboard
    // is configured and left delivering reports. `error` describes a Failed
    // scan and is null otherwise.
    Scan scan(PhysAllocator& pages, const char*& error);

    // Takes one pending report from the configured keyboard. Events for
    // other endpoints and ports are read and discarded; a Port Status Change
    // that says the keyboard's port lost its device marks it lost.
    KeyEvent poll_keyboard(u8 report[8]);

private:
    u8 bring_up_port(u32 port);
    void set_usb3(u32 first, u32 count);
    bool is_usb3(u32 port) const;

    volatile u8* operational_ = nullptr;
    volatile u8* runtime_ = nullptr;
    volatile u32* doorbells_ = nullptr;
    xhci::Trb* command_trbs_ = nullptr;
    u64 command_phys_ = 0;
    xhci::ProducerRing command_ring_{};
    xhci::EventRing event_ring_{};
    u64 event_phys_ = 0;
    u64* dcbaa_ = nullptr;
    u64 dcbaa_phys_ = 0;
    u32 slots_ = 0;
    u32 ports_ = 0;
    u32 context_size_ = 32;
    u32 usb3_ports_[8] = {};
    u32 keyboard_port_ = 0;
    u8 keyboard_slot_ = 0;
    u8 keyboard_dci_ = 0;
    u16 keyboard_report_bytes_ = 8;
    xhci::ProducerRing keyboard_ring_{};
    u64 keyboard_report_phys_ = 0;
    u8* keyboard_report_ = nullptr;
    bool keyboard_running_ = false;
};

}
