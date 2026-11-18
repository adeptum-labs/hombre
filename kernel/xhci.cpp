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

#include "xhci.hpp"

#include "clock.hpp"

namespace hombre {

namespace {

constexpr u8 kPciBar0Offset = 0x10;
constexpr u8 kPciClassOffset = 0x08;

// The class code dword holds class, subclass and interface above the
// revision in bits 7:0.
constexpr u32 kXhciClassCode = 0x0C0330;
constexpr u32 kClassCodeShift = 8;

// Capability registers, at the start of BAR0.
constexpr u32 kCapLength = 0x00;
constexpr u32 kHcsParams1 = 0x04;
constexpr u32 kHcsParams2 = 0x08;
constexpr u32 kHccParams1 = 0x10;
constexpr u32 kDbOff = 0x14;
constexpr u32 kRtOff = 0x18;

// Operational registers, at CAPLENGTH. The port register sets follow at
// 0x400, one 0x10-byte set per port.
constexpr u32 kUsbCmd = 0x00;
constexpr u32 kUsbSts = 0x04;
constexpr u32 kCrcr = 0x18;
constexpr u32 kDcbaap = 0x30;
constexpr u32 kConfig = 0x38;
constexpr u32 kPortRegisters = 0x400;
constexpr u32 kPortRegisterBytes = 0x10;

// Interrupter 0, at RTSOFF.
constexpr u32 kInterrupter0 = 0x20;
constexpr u32 kErstsz = 0x08;
constexpr u32 kErstba = 0x10;
constexpr u32 kErdp = 0x18;
constexpr u32 kInterrupterBytes = 0x20;

constexpr u32 kUsbCmdRun = 1u << 0;
constexpr u32 kUsbCmdReset = 1u << 1;
constexpr u32 kUsbStsHalted = 1u << 0;
constexpr u32 kUsbStsNotReady = 1u << 11;
constexpr u32 kCrcrCycle = 1u << 0;
constexpr u32 kErdpEhb = 1u << 3;

constexpr u32 kHccParamsAc64 = 1u << 0;
constexpr u32 kHccParamsContext64 = 1u << 2;
constexpr u32 kHccParamsExtendedShift = 16;

constexpr u32 kMaxSlots = 64;
constexpr u32 kMaxExtendedCapabilities = 64;
constexpr u32 kLegacyCapability = 1;
constexpr u32 kSupportedProtocolCapability = 2;
constexpr u32 kExtCapIdMask = 0xFFu;
constexpr u32 kExtCapNextShift = 8;
constexpr u32 kExtCapNextMask = 0xFFu;
constexpr u32 kProtocolDword2 = 0x08;
constexpr u32 kProtocolRevisionShift = 24;
constexpr u32 kProtocolPortOffsetMask = 0xFFu;
constexpr u32 kProtocolPortCountShift = 8;
constexpr u32 kMaxPorts = 256;
constexpr u32 kLegacyBiosOwned = 1u << 16;
constexpr u32 kLegacyOsOwned = 1u << 24;
constexpr u32 kLegacySmiEnables =
    (1u << 0) | (1u << 4) | (1u << 5) | (1u << 16) | (1u << 20);
constexpr u32 kLegacySmiStatus = 0x7u << 29;

// PORTSC. PED and the change bits are write-one-to-clear; a write keeps only
// PP and the bits it means to set. CSC through CEC are bits 17 to 23.
constexpr u32 kPortConnected = 1u << 0;
constexpr u32 kPortEnabled = 1u << 1;
constexpr u32 kPortReset = 1u << 4;
constexpr u32 kPortPower = 1u << 9;
constexpr u32 kPortSpeedShift = 10;
constexpr u32 kPortSpeedMask = 0xFu;
constexpr u32 kPortResetChange = 1u << 21;
constexpr u32 kPortChanges = 0x7Fu << 17;

constexpr u32 kControlBufferBytes = 512;

constexpr u8 kEndpointControl = 4;
constexpr u8 kEndpointInterruptIn = 7;

// The first map covers the capability registers; a larger window is mapped
// only when the other registers or the extended capabilities lie beyond it.
constexpr u32 kCapabilityWindow = 0x1000;
constexpr u32 kExtendedCapabilityWindow = 0x1000;

constexpr u64 kPageBytes = 4096;
constexpr u32 kRingTrbs = static_cast<u32>(kPageBytes / sizeof(xhci::Trb));

constexpr u64 kReadyTimeoutUs = 1000000;
constexpr u64 kHandoffTimeoutUs = 1000000;
constexpr u64 kCommandTimeoutUs = 1000000;
constexpr u64 kPortResetTimeoutUs = 500000;
constexpr u64 kControlTimeoutUs = 500000;
constexpr u64 kFourGiB = 1ull << 32;

constexpr const char* kErrorNoBar = "controller has no memory BAR";
constexpr const char* kErrorMap = "cannot map controller registers";
constexpr const char* kErrorHandoff = "controller handoff did not complete";
constexpr const char* kErrorReady = "controller does not become ready";
constexpr const char* kErrorHalt = "controller does not halt";
constexpr const char* kErrorMemory = "no memory for controller structures";
constexpr const char* kErrorBelow4G = "controller needs memory below 4 GiB";
constexpr const char* kErrorNoop = "no-op command failed";
constexpr const char* kErrorDeviceMemory = "no memory for the device";
constexpr const char* kErrorEnableSlot = "enable slot command failed";
constexpr const char* kErrorAddressDevice = "address device command failed";
constexpr const char* kErrorDeviceDescriptor = "device descriptor read failed";
constexpr const char* kErrorConfigurationDescriptor = "configuration descriptor read failed";
constexpr const char* kErrorConfigurationInvalid = "configuration descriptor is invalid";
constexpr const char* kErrorSetConfiguration = "set configuration failed";
constexpr const char* kErrorSetProtocol = "set protocol failed";
constexpr const char* kErrorSetIdle = "set idle failed";
constexpr const char* kErrorConfigureEndpoint = "configure endpoint command failed";

struct EventRingSegment {
    u64 base;
    u32 size;
    u32 reserved;
};

static_assert(sizeof(EventRingSegment) == 16);

void cpu_pause() {
    asm volatile("pause");
}

// Orders the ring writes before the doorbell write to device memory.
void memory_fence() {
    asm volatile("mfence" : : : "memory");
}

// Keeps the compiler from reusing a slot value across polls.
void compiler_barrier() {
    asm volatile("" : : : "memory");
}

u32 read32(const volatile u8* base, u32 offset) {
    return *reinterpret_cast<const volatile u32*>(base + offset);
}

void write32(volatile u8* base, u32 offset, u32 value) {
    *reinterpret_cast<volatile u32*>(base + offset) = value;
}

// 64-bit registers are written as two dwords because a controller need not
// accept one 64-bit store. The low half goes first.
void write64(volatile u8* base, u32 offset, u64 value) {
    write32(base, offset, static_cast<u32>(value));
    write32(base, offset + 4, static_cast<u32>(value >> 32));
}

// Polls a 32-bit register until the bits in `mask` equal `value`, or one
// second passes.
bool wait_bits(const volatile u8* base, u32 offset, u32 mask, u32 value) {
    const Deadline deadline = deadline_in_us(kReadyTimeoutUs);
    for (;;) {
        if ((read32(base, offset) & mask) == value) {
            return true;
        }
        if (expired(deadline)) {
            return false;
        }
        cpu_pause();
    }
}

constexpr u32 scratchpad_count(u32 hcsparams2) {
    return (((hcsparams2 >> 27) & 0x1Fu) << 5) | ((hcsparams2 >> 21) & 0x1Fu);
}

constexpr bool fits_below_4g(u64 phys, u64 bytes) {
    return phys + bytes <= kFourGiB;
}

// Takes the legacy capability at `offset`: OS ownership on, then wait for
// the firmware to drop BIOS ownership, then SMI generation off.
bool take_legacy_capability(volatile u8* registers, u32 offset) {
    write32(registers, offset, read32(registers, offset) | kLegacyOsOwned);
    const Deadline deadline = deadline_in_us(kHandoffTimeoutUs);
    while ((read32(registers, offset) & kLegacyBiosOwned) != 0) {
        if (expired(deadline)) {
            return false;
        }
        cpu_pause();
    }
    write32(registers, offset + 4,
            (read32(registers, offset + 4) & ~kLegacySmiEnables) | kLegacySmiStatus);
    return true;
}

// One endpoint to add to an input context.
struct EndpointSetup {
    u8 dci;
    u8 type;
    u16 max_packet;
    u8 interval;
    u64 ring_phys;
    u16 average_trb;
    u16 max_esit;
};

void set_dword(u8* base, u32 offset, u32 value) {
    *reinterpret_cast<u32*>(base + offset) = value;
}

// Builds the input context a command hands to the controller: the input
// control context, the slot context and the context of one endpoint.
void build_input_context(u8* input, u32 context_size, u8 speed, u32 port, u32 entries,
                         const EndpointSetup& endpoint) {
    __builtin_memset(input, 0, kPageBytes);
    set_dword(input, 4, 1u | (1u << endpoint.dci));
    u8* slot = input + context_size;
    set_dword(slot, 0, (static_cast<u32>(speed) << 20) | (entries << 27));
    set_dword(slot, 4, port << 16);
    u8* ep = input + (static_cast<u32>(endpoint.dci) + 1) * context_size;
    set_dword(ep, 0, static_cast<u32>(endpoint.interval) << 16);
    set_dword(ep, 4,
              (3u << 1) | (static_cast<u32>(endpoint.type) << 3) |
                  (static_cast<u32>(endpoint.max_packet) << 16));
    set_dword(ep, 8, static_cast<u32>(endpoint.ring_phys) | 1u);
    set_dword(ep, 12, static_cast<u32>(endpoint.ring_phys >> 32));
    set_dword(ep, 16, static_cast<u32>(endpoint.average_trb) |
                          (static_cast<u32>(endpoint.max_esit) << 16));
}

// Runs one control transfer on the slot's EP0 ring and waits for the event
// that names the status TRB. The status stage is an IN transaction unless an
// IN data stage came before it.
bool control_transfer(Xhci& controller, u8 slot, xhci::ProducerRing& ring, u64 ring_phys,
                      u64 data_phys, u16 length, bool in, u8 request_type, u8 request,
                      u16 value, u16 index) {
    const u32 transfer_type = length == 0 ? 0u : (in ? 3u : 2u);
    if (!ring.push(xhci::make_setup_stage(request_type, request, value, index, length,
                                          transfer_type))) {
        return false;
    }
    if (length != 0 && !ring.push(xhci::make_data_stage(data_phys, length, in))) {
        return false;
    }
    const u64 status_index = ring.enqueue_index();
    if (!ring.push(xhci::make_status_stage(length == 0 || !in))) {
        return false;
    }
    const u64 status_phys = ring_phys + status_index * sizeof(xhci::Trb);
    memory_fence();
    controller.doorbells()[slot] = 1;

    const Deadline deadline = deadline_in_us(kControlTimeoutUs);
    while (!expired(deadline)) {
        xhci::Trb event{};
        if (!controller.poll_event(event)) {
            cpu_pause();
            continue;
        }
        if (xhci::event_type(event) != xhci::kTrbTransferEvent ||
            xhci::event_slot(event) != slot || xhci::event_endpoint(event) != 1 ||
            event.parameter != status_phys) {
            continue;
        }
        const u32 code = xhci::completion_code(event);
        return code == 1 || code == 13;
    }
    return false;
}

struct KeyboardState {
    u8 slot = 0;
    u8 dci = 0;
    xhci::ProducerRing ring{};
    u64 report_phys = 0;
    u8* report = nullptr;
};

// Brings the device on `port` up far enough to tell what it is. A boot
// keyboard is left configured with its interrupt ring armed.
Xhci::Scan enumerate_port(Xhci& controller, u32 port, u8 speed, PhysAllocator& pages,
                          KeyboardState& keyboard, const char*& error) {
    const u64 output_phys = pages.alloc_pages(1);
    const u64 input_phys = pages.alloc_pages(1);
    const u64 ep0_phys = pages.alloc_pages(1);
    const u64 data_phys = pages.alloc_pages(1);
    if (output_phys == 0 || input_phys == 0 || ep0_phys == 0 || data_phys == 0) {
        error = kErrorDeviceMemory;
        return Xhci::Scan::Failed;
    }
    u8* input = pages.to_ptr(input_phys);
    u8* data = pages.to_ptr(data_phys);

    xhci::Trb completion{};
    if (!controller.command(xhci::make_enable_slot(), completion) ||
        xhci::completion_code(completion) != 1) {
        error = kErrorEnableSlot;
        return Xhci::Scan::Failed;
    }
    const u8 slot = xhci::event_slot(completion);
    if (slot == 0 || slot > controller.slot_count()) {
        error = kErrorEnableSlot;
        return Xhci::Scan::Failed;
    }

    const EndpointSetup ep0{1, kEndpointControl, xhci::default_ep0_max_packet(speed),
                            0, ep0_phys, 8, 0};
    build_input_context(input, controller.context_size(), speed, port, 1, ep0);
    controller.dcbaa()[slot] = output_phys;
    if (!controller.command(xhci::make_address_device(input_phys, slot, false),
                            completion) ||
        xhci::completion_code(completion) != 1) {
        error = kErrorAddressDevice;
        return Xhci::Scan::Failed;
    }

    xhci::ProducerRing ep0_ring;
    ep0_ring.init(reinterpret_cast<xhci::Trb*>(pages.to_ptr(ep0_phys)), kRingTrbs,
                  ep0_phys);

    if (!control_transfer(controller, slot, ep0_ring, ep0_phys, data_phys, 8, true,
                          0x80, 6, 0x0100, 0)) {
        error = kErrorDeviceDescriptor;
        return Xhci::Scan::Failed;
    }
    const u8 device_class = data[4];

    if (!control_transfer(controller, slot, ep0_ring, ep0_phys, data_phys, 9, true,
                          0x80, 6, 0x0200, 0)) {
        error = kErrorConfigurationDescriptor;
        return Xhci::Scan::Failed;
    }
    const u32 total = static_cast<u32>(data[2]) | (static_cast<u32>(data[3]) << 8);
    if (total < 9 || total > kControlBufferBytes) {
        error = kErrorConfigurationInvalid;
        return Xhci::Scan::Failed;
    }
    if (!control_transfer(controller, slot, ep0_ring, ep0_phys, data_phys,
                          static_cast<u16>(total), true, 0x80, 6, 0x0200, 0)) {
        error = kErrorConfigurationDescriptor;
        return Xhci::Scan::Failed;
    }

    xhci::KeyboardInterface info{};
    if (!xhci::find_boot_keyboard(data, total, info)) {
        return device_class == 9 ? Xhci::Scan::OnlyHub : Xhci::Scan::NoKeyboard;
    }

    if (!control_transfer(controller, slot, ep0_ring, ep0_phys, data_phys, 0, false,
                          0x00, 9, info.configuration_value, 0)) {
        error = kErrorSetConfiguration;
        return Xhci::Scan::Failed;
    }
    if (!control_transfer(controller, slot, ep0_ring, ep0_phys, data_phys, 0, false,
                          0x21, 0x0B, 0, info.interface_number)) {
        error = kErrorSetProtocol;
        return Xhci::Scan::Failed;
    }
    if (!control_transfer(controller, slot, ep0_ring, ep0_phys, data_phys, 0, false,
                          0x21, 0x0A, 0, info.interface_number)) {
        error = kErrorSetIdle;
        return Xhci::Scan::Failed;
    }

    const u64 ring_phys = pages.alloc_pages(1);
    const u64 report_phys = pages.alloc_pages(1);
    if (ring_phys == 0 || report_phys == 0) {
        error = kErrorDeviceMemory;
        return Xhci::Scan::Failed;
    }
    const u8 dci = static_cast<u8>(2 * (info.endpoint_address & 0x0Fu) + 1);
    const EndpointSetup endpoint{dci, kEndpointInterruptIn, info.max_packet,
                                 xhci::endpoint_interval(speed, info.interval), ring_phys,
                                 8, 8};
    build_input_context(input, controller.context_size(), speed, port, dci, endpoint);
    if (!controller.command(xhci::make_configure_endpoint(input_phys, slot), completion) ||
        xhci::completion_code(completion) != 1) {
        error = kErrorConfigureEndpoint;
        return Xhci::Scan::Failed;
    }

    keyboard.slot = slot;
    keyboard.dci = dci;
    keyboard.ring.init(reinterpret_cast<xhci::Trb*>(pages.to_ptr(ring_phys)), kRingTrbs,
                       ring_phys);
    keyboard.report = pages.to_ptr(report_phys);
    keyboard.report_phys = report_phys;
    if (!keyboard.ring.push(xhci::make_normal(report_phys, 8))) {
        error = kErrorDeviceMemory;
        return Xhci::Scan::Failed;
    }
    memory_fence();
    controller.doorbells()[slot] = dci;
    return Xhci::Scan::Keyboard;
}

struct FindState {
    PciAddress* out;
    u32 capacity;
    u32 found;
};

bool match_controller(void* context, PciAddress address, u16, u16) {
    if ((pci_read32(address, kPciClassOffset) >> kClassCodeShift) != kXhciClassCode) {
        return false;
    }
    FindState& state = *static_cast<FindState*>(context);
    if (state.out != nullptr && state.found < state.capacity) {
        state.out[state.found] = address;
    }
    ++state.found;
    return false;
}

}

u32 xhci_find(PciAddress* out, u32 capacity) {
    FindState state{out, capacity, 0};
    pci_scan(&match_controller, &state);
    return state.found;
}

bool Xhci::init(PciAddress address, MmioMapper& mmio, PhysAllocator& pages,
                const char*& error) {
    *this = Xhci{};
    error = nullptr;

    const u32 bar_low = pci_read32(address, kPciBar0Offset);
    u32 bar_high = 0;
    if ((bar_low & 1u) == 0 && ((bar_low >> 1) & 3u) == 2) {
        bar_high = pci_read32(address, static_cast<u8>(kPciBar0Offset + 4));
    }
    const PciBar bar = pci_decode_bar(bar_low, bar_high);
    if (bar.is_io || bar.phys == 0) {
        error = kErrorNoBar;
        return false;
    }

    // Memory decoding and bus mastering: the registers are reached through
    // the window and the controller reads its rings from memory.
    const u32 pci_command = pci_read32(address, kPciCommandOffset) & 0xFFFFu;
    pci_write32(address, kPciCommandOffset,
                pci_command | kPciCommandMemorySpace | kPciCommandBusMaster);

    volatile u8* registers =
        static_cast<volatile u8*>(mmio.map(bar.phys, kCapabilityWindow));
    if (registers == nullptr) {
        error = kErrorMap;
        return false;
    }

    const u32 caplength = read32(registers, kCapLength) & 0xFFu;
    const u32 hcsparams1 = read32(registers, kHcsParams1);
    const u32 hcsparams2 = read32(registers, kHcsParams2);
    const u32 hccparams1 = read32(registers, kHccParams1);
    const u32 dboff = read32(registers, kDbOff);
    const u32 rtsoff = read32(registers, kRtOff);
    const u32 max_ports = (hcsparams1 >> 24) & 0xFFu;
    const u32 max_slots = hcsparams1 & 0xFFu;
    const u32 extended = (hccparams1 >> kHccParamsExtendedShift) & 0xFFFFu;
    const bool ac64 = (hccparams1 & kHccParamsAc64) != 0;
    ports_ = max_ports;
    slots_ = max_slots < kMaxSlots ? max_slots : kMaxSlots;
    context_size_ = (hccparams1 & kHccParamsContext64) != 0 ? 64 : 32;

    u64 window = kCapabilityWindow;
    const u64 port_end = static_cast<u64>(caplength) + kPortRegisters +
                         kPortRegisterBytes * max_ports;
    const u64 runtime_end =
        static_cast<u64>(rtsoff) + kInterrupter0 + kInterrupterBytes;
    const u64 doorbell_end = static_cast<u64>(dboff) + 4ull * (max_slots + 1);
    if (port_end > window) {
        window = port_end;
    }
    if (runtime_end > window) {
        window = runtime_end;
    }
    if (doorbell_end > window) {
        window = doorbell_end;
    }
    if (extended != 0) {
        const u64 extended_end =
            (static_cast<u64>(extended) << 2) + kExtendedCapabilityWindow;
        if (extended_end > window) {
            window = extended_end;
        }
    }
    if (window > kCapabilityWindow) {
        registers = static_cast<volatile u8*>(mmio.map(bar.phys, window));
        if (registers == nullptr) {
            error = kErrorMap;
            return false;
        }
    }

    operational_ = registers + caplength;
    runtime_ = registers + rtsoff;
    doorbells_ = reinterpret_cast<volatile u32*>(registers + dboff);

    // The extended capability list names the legacy handoff and which ports
    // follow the USB 3 protocol.
    u32 capability = extended << 2;
    for (u32 n = 0; n < kMaxExtendedCapabilities && capability != 0; ++n) {
        const u32 header = read32(registers, capability);
        const u32 id = header & kExtCapIdMask;
        if (id == kLegacyCapability) {
            if (!take_legacy_capability(registers, capability)) {
                error = kErrorHandoff;
                return false;
            }
        } else if (id == kSupportedProtocolCapability) {
            const u32 revision = header >> kProtocolRevisionShift;
            const u32 protocols = read32(registers, capability + kProtocolDword2);
            if (revision == 3) {
                set_usb3(protocols & kProtocolPortOffsetMask,
                         (protocols >> kProtocolPortCountShift) & 0xFFu);
            }
        }
        capability = ((header >> kExtCapNextShift) & kExtCapNextMask) << 2;
    }
    if (!wait_bits(operational_, kUsbSts, kUsbStsNotReady, 0)) {
        error = kErrorReady;
        return false;
    }

    write32(operational_, kUsbCmd, 0);
    if (!wait_bits(operational_, kUsbSts, kUsbStsHalted, kUsbStsHalted)) {
        error = kErrorHalt;
        return false;
    }

    write32(operational_, kUsbCmd, kUsbCmdReset);
    if (!wait_bits(operational_, kUsbCmd, kUsbCmdReset, 0) ||
        !wait_bits(operational_, kUsbSts, kUsbStsNotReady, 0)) {
        error = kErrorReady;
        return false;
    }
    write32(operational_, kConfig, slots_);

    const u32 scratchpads = scratchpad_count(hcsparams2);
    dcbaa_phys_ = pages.alloc_pages(1);
    command_phys_ = pages.alloc_pages(1);
    event_phys_ = pages.alloc_pages(1);
    const u64 segment_phys = pages.alloc_pages(1);
    if (dcbaa_phys_ == 0 || command_phys_ == 0 || event_phys_ == 0 ||
        segment_phys == 0) {
        error = kErrorMemory;
        return false;
    }
    u64* dcbaa = reinterpret_cast<u64*>(pages.to_ptr(dcbaa_phys_));
    dcbaa_ = dcbaa;
    xhci::Trb* command_trbs =
        reinterpret_cast<xhci::Trb*>(pages.to_ptr(command_phys_));
    xhci::Trb* event_trbs = reinterpret_cast<xhci::Trb*>(pages.to_ptr(event_phys_));
    EventRingSegment* segment =
        reinterpret_cast<EventRingSegment*>(pages.to_ptr(segment_phys));

    if (scratchpads != 0) {
        const u64 pointers_per_page = kPageBytes / sizeof(u64);
        const u64 array_pages =
            (scratchpads + pointers_per_page - 1) / pointers_per_page;
        const u64 array_phys = pages.alloc_pages(array_pages);
        if (array_phys == 0) {
            error = kErrorMemory;
            return false;
        }
        if (!ac64 && !fits_below_4g(array_phys, array_pages * kPageBytes)) {
            error = kErrorBelow4G;
            return false;
        }
        u64* array = reinterpret_cast<u64*>(pages.to_ptr(array_phys));
        for (u32 i = 0; i < scratchpads; ++i) {
            const u64 buffer_phys = pages.alloc_pages(1);
            if (buffer_phys == 0) {
                error = kErrorMemory;
                return false;
            }
            if (!ac64 && !fits_below_4g(buffer_phys, kPageBytes)) {
                error = kErrorBelow4G;
                return false;
            }
            array[i] = buffer_phys;
        }
        dcbaa[0] = array_phys;
    }

    if (!ac64 && (!fits_below_4g(dcbaa_phys_, kPageBytes) ||
                  !fits_below_4g(command_phys_, kPageBytes) ||
                  !fits_below_4g(event_phys_, kPageBytes) ||
                  !fits_below_4g(segment_phys, kPageBytes))) {
        error = kErrorBelow4G;
        return false;
    }

    *segment = EventRingSegment{event_phys_, kRingTrbs, 0};
    command_trbs_ = command_trbs;
    command_ring_.init(command_trbs, kRingTrbs, command_phys_);
    event_ring_.init(event_trbs, kRingTrbs);

    write64(operational_, kDcbaap, dcbaa_phys_);
    write64(operational_, kCrcr, command_phys_ | kCrcrCycle);
    // Writing ERSTBA starts the event ring, so its size and dequeue pointer
    // go first.
    write32(runtime_, kInterrupter0 + kErstsz, 1);
    write64(runtime_, kInterrupter0 + kErdp, event_phys_ | kErdpEhb);
    write64(runtime_, kInterrupter0 + kErstba, segment_phys);

    memory_fence();
    write32(operational_, kUsbCmd, kUsbCmdRun);
    if (!wait_bits(operational_, kUsbSts, kUsbStsHalted, 0)) {
        error = kErrorReady;
        return false;
    }

    xhci::Trb completion{};
    if (!command(xhci::make_noop_command(), completion) ||
        xhci::completion_code(completion) != 1) {
        error = kErrorNoop;
        return false;
    }
    return true;
}

bool Xhci::command(xhci::Trb trb, xhci::Trb& completion) {
    if (command_trbs_ == nullptr) {
        return false;
    }
    const u64 index = command_ring_.enqueue_index();
    if (!command_ring_.push(trb)) {
        return false;
    }
    memory_fence();
    doorbells_[0] = 0;

    const u64 physical = command_phys_ + index * sizeof(xhci::Trb);
    const Deadline deadline = deadline_in_us(kCommandTimeoutUs);
    while (!expired(deadline)) {
        if (poll_event(completion) &&
            xhci::event_type(completion) == xhci::kTrbCommandCompletionEvent &&
            completion.parameter == physical) {
            return true;
        }
        cpu_pause();
    }
    return false;
}

bool Xhci::poll_event(xhci::Trb& out) {
    if (runtime_ == nullptr) {
        return false;
    }
    compiler_barrier();
    if (!event_ring_.pop(out)) {
        return false;
    }
    const u64 dequeue =
        event_phys_ + static_cast<u64>(event_ring_.dequeue_index()) * sizeof(xhci::Trb);
    write64(runtime_, kInterrupter0 + kErdp, dequeue | kErdpEhb);
    return true;
}

void Xhci::set_usb3(u32 first, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const u32 port = first + i;
        if (port == 0 || port > kMaxPorts) {
            break;
        }
        const u32 bit = port - 1;
        usb3_ports_[bit >> 5] |= 1u << (bit & 31);
    }
}

bool Xhci::is_usb3(u32 port) const {
    if (port == 0 || port > kMaxPorts) {
        return false;
    }
    const u32 bit = port - 1;
    return ((usb3_ports_[bit >> 5] >> (bit & 31)) & 1u) != 0;
}

// Brings a connected port up and returns its speed id, or 0 when it does not
// come up. USB 2 ports are reset; USB 3 links train on their own.
u8 Xhci::bring_up_port(u32 port) {
    const u32 offset = kPortRegisters + kPortRegisterBytes * (port - 1);
    u32 status = read32(operational_, offset);
    if ((status & kPortConnected) == 0) {
        return 0;
    }
    if (is_usb3(port)) {
        write32(operational_, offset, status & (kPortPower | kPortChanges));
        if ((status & kPortEnabled) == 0) {
            return 0;
        }
    } else {
        write32(operational_, offset, (status & kPortPower) | kPortReset);
        const Deadline deadline = deadline_in_us(kPortResetTimeoutUs);
        for (;;) {
            status = read32(operational_, offset);
            if ((status & (kPortEnabled | kPortResetChange)) ==
                (kPortEnabled | kPortResetChange)) {
                break;
            }
            if (expired(deadline)) {
                write32(operational_, offset, status & (kPortPower | kPortChanges));
                return 0;
            }
            cpu_pause();
        }
        write32(operational_, offset, status & (kPortPower | kPortChanges));
    }
    return static_cast<u8>((status >> kPortSpeedShift) & kPortSpeedMask);
}

Xhci::Scan Xhci::scan(PhysAllocator& pages, const char*& error) {
    error = nullptr;
    keyboard_running_ = false;
    bool hub = false;
    const char* failure = nullptr;
    for (u32 port = 1; port <= ports_; ++port) {
        const u8 speed = bring_up_port(port);
        if (speed == 0) {
            continue;
        }
        KeyboardState keyboard;
        const char* port_error = nullptr;
        const Scan result =
            enumerate_port(*this, port, speed, pages, keyboard, port_error);
        if (result == Scan::Keyboard) {
            keyboard_port_ = port;
            keyboard_slot_ = keyboard.slot;
            keyboard_dci_ = keyboard.dci;
            keyboard_ring_ = keyboard.ring;
            keyboard_report_phys_ = keyboard.report_phys;
            keyboard_report_ = keyboard.report;
            keyboard_running_ = true;
            return Scan::Keyboard;
        }
        if (result == Scan::OnlyHub) {
            hub = true;
        }
        if (result == Scan::Failed && failure == nullptr) {
            failure = port_error;
        }
    }
    if (failure != nullptr) {
        error = failure;
        return Scan::Failed;
    }
    return hub ? Scan::OnlyHub : Scan::NoKeyboard;
}

Xhci::KeyEvent Xhci::poll_keyboard(u8 report[8]) {
    if (!keyboard_running_) {
        return KeyEvent::None;
    }
    xhci::Trb event{};
    while (poll_event(event)) {
        if (xhci::event_type(event) == xhci::kTrbPortStatusChangeEvent) {
            if (static_cast<u32>(xhci::port_id(event)) == keyboard_port_) {
                const u32 offset =
                    kPortRegisters + kPortRegisterBytes * (keyboard_port_ - 1);
                const u32 status = read32(operational_, offset);
                write32(operational_, offset, status & (kPortPower | kPortChanges));
                if ((status & kPortConnected) == 0) {
                    keyboard_running_ = false;
                    return KeyEvent::Lost;
                }
            }
            continue;
        }
        if (xhci::event_type(event) != xhci::kTrbTransferEvent ||
            xhci::event_slot(event) != keyboard_slot_ ||
            xhci::event_endpoint(event) != keyboard_dci_) {
            continue;
        }
        const u32 code = xhci::completion_code(event);
        if (code != 1 && code != 13) {
            keyboard_running_ = false;
            return KeyEvent::Lost;
        }
        __builtin_memcpy(report, keyboard_report_, 8);
        __builtin_memset(keyboard_report_, 0, 8);
        if (!keyboard_ring_.push(xhci::make_normal(keyboard_report_phys_, 8))) {
            keyboard_running_ = false;
            return KeyEvent::Lost;
        }
        memory_fence();
        doorbells_[keyboard_slot_] = keyboard_dci_;
        return KeyEvent::Report;
    }
    return KeyEvent::None;
}

}

