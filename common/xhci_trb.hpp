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

#include "types.hpp"

// TRBs, the ring cursors the driver keeps on the host side, and the descriptor
// and speed helpers the enumeration path needs. Nothing here touches hardware:
// the rings write into DMA memory that the caller owns. The controller reads
// TRBs as little endian, which is what plain u64 and u32 stores give on x86.

namespace hombre::xhci {

struct Trb {
    u64 parameter;
    u32 status;
    u32 control;
};

static_assert(sizeof(Trb) == 16);
static_assert(__builtin_offsetof(Trb, status) == 8);
static_assert(__builtin_offsetof(Trb, control) == 12);

inline constexpr u32 kTrbNormal = 1;
inline constexpr u32 kTrbSetupStage = 2;
inline constexpr u32 kTrbDataStage = 3;
inline constexpr u32 kTrbStatusStage = 4;
inline constexpr u32 kTrbLink = 6;
inline constexpr u32 kTrbEnableSlot = 9;
inline constexpr u32 kTrbAddressDevice = 11;
inline constexpr u32 kTrbConfigureEndpoint = 12;
inline constexpr u32 kTrbEvaluateContext = 13;
inline constexpr u32 kTrbResetEndpoint = 14;
inline constexpr u32 kTrbSetTrDequeuePointer = 16;
inline constexpr u32 kTrbNoOpCommand = 23;
inline constexpr u32 kTrbTransferEvent = 32;
inline constexpr u32 kTrbCommandCompletionEvent = 33;
inline constexpr u32 kTrbPortStatusChangeEvent = 34;

inline constexpr u32 kTrbCycle = 1u << 0;
inline constexpr u32 kTrbToggleCycle = 1u << 1;
inline constexpr u32 kTrbIoc = 1u << 5;
inline constexpr u32 kTrbIdt = 1u << 6;
inline constexpr u32 kTrbBlockSetAddressRequest = 1u << 9;
inline constexpr u32 kTrbTypeShift = 10;
inline constexpr u32 kTrbTypeMask = 0x3Fu;
inline constexpr u32 kTrbDirection = 1u << 16;
inline constexpr u32 kTrbSlotShift = 24;

// The setup stage carries the eight byte setup packet as immediate data.
inline constexpr u32 kSetupPacketSize = 8;

constexpr u32 trb_type(u32 control) {
    return (control >> kTrbTypeShift) & kTrbTypeMask;
}

constexpr Trb make_noop_command() {
    return {0, 0, kTrbNoOpCommand << kTrbTypeShift};
}

constexpr Trb make_enable_slot() {
    return {0, 0, kTrbEnableSlot << kTrbTypeShift};
}

constexpr Trb make_address_device(u64 input_context, u8 slot, bool bsr) {
    return {input_context, 0,
            (static_cast<u32>(slot) << kTrbSlotShift) | (bsr ? kTrbBlockSetAddressRequest : 0u) |
                (kTrbAddressDevice << kTrbTypeShift)};
}

constexpr Trb make_configure_endpoint(u64 input_context, u8 slot) {
    return {input_context, 0,
            (static_cast<u32>(slot) << kTrbSlotShift) | (kTrbConfigureEndpoint << kTrbTypeShift)};
}

constexpr Trb make_evaluate_context(u64 input_context, u8 slot) {
    return {input_context, 0,
            (static_cast<u32>(slot) << kTrbSlotShift) |
                (kTrbEvaluateContext << kTrbTypeShift)};
}

constexpr Trb make_reset_endpoint(u8 slot, u8 endpoint) {
    return {0, 0,
            (static_cast<u32>(slot) << kTrbSlotShift) |
                (static_cast<u32>(endpoint) << 16) | (kTrbResetEndpoint << kTrbTypeShift)};
}

constexpr Trb make_set_tr_dequeue_pointer(u64 dequeue, u8 slot, u8 endpoint) {
    return {dequeue, 0,
            (static_cast<u32>(slot) << kTrbSlotShift) |
                (static_cast<u32>(endpoint) << 16) |
                (kTrbSetTrDequeuePointer << kTrbTypeShift)};
}

constexpr Trb make_link(u64 next, bool toggle_cycle) {
    return {next, 0, (kTrbLink << kTrbTypeShift) | (toggle_cycle ? kTrbToggleCycle : 0u)};
}

constexpr Trb make_setup_stage(u8 request_type, u8 request, u16 value, u16 index, u16 length,
                               u32 transfer_type) {
    const u64 packet = static_cast<u64>(request_type) | (static_cast<u64>(request) << 8) |
                       (static_cast<u64>(value) << 16) | (static_cast<u64>(index) << 32) |
                       (static_cast<u64>(length) << 48);
    return {packet, kSetupPacketSize,
            (kTrbSetupStage << kTrbTypeShift) | kTrbIdt | ((transfer_type & 3u) << 16)};
}

constexpr Trb make_data_stage(u64 buffer, u16 length, bool in) {
    return {buffer, length, (kTrbDataStage << kTrbTypeShift) | (in ? kTrbDirection : 0u)};
}

constexpr Trb make_status_stage(bool in) {
    return {0, 0, (kTrbStatusStage << kTrbTypeShift) | kTrbIoc | (in ? kTrbDirection : 0u)};
}

constexpr Trb make_normal(u64 buffer, u16 length) {
    return {buffer, length, (kTrbNormal << kTrbTypeShift) | kTrbIoc};
}

// The number of scratchpad buffers in HCSPARAMS2. Bits 25:21 hold the high
// five bits of the count and bits 31:27 the low five.
constexpr u32 scratchpad_count(u32 hcsparams2) {
    return (((hcsparams2 >> 21) & 0x1Fu) << 5) | ((hcsparams2 >> 27) & 0x1Fu);
}

// The offset of the next extended capability, or 0 at the end of the list.
// The Next field names a dword offset from the current capability.
constexpr u32 next_extended_capability(u32 offset, u32 header) {
    const u32 next = (header >> 8) & 0xFFu;
    return next == 0 ? 0 : offset + (next << 2);
}

// The producer side of a command or transfer ring. The last slot holds a Link
// TRB back to the start, so only the other slots carry work, and the producer
// cycle flips each time the Link TRB is written.
class ProducerRing {
public:
    // base is where the host writes the ring; base_physical is the address
    // the controller reads it at, which the wrap link points to.
    void init(Trb* base, u32 trbs, u64 base_physical) {
        base_ = base;
        trbs_ = trbs;
        base_physical_ = base_physical;
        index_ = 0;
        cycle_ = true;
    }

    bool push(Trb trb) {
        if (trbs_ < 4) {
            return false;
        }
        write(base_[index_], with_cycle(trb));
        if (++index_ == trbs_ - 1) {
            write(base_[index_], with_cycle(make_link(base_physical_, true)));
            index_ = 0;
            cycle_ = !cycle_;
        }
        return true;
    }

    u64 enqueue_index() const { return index_; }

    bool cycle() const { return cycle_; }

private:
    Trb with_cycle(Trb trb) const {
        trb.control = (trb.control & ~kTrbCycle) | (cycle_ ? kTrbCycle : 0u);
        return trb;
    }

    // The cycle bit publishes a TRB, so it is stored after the other fields
    // the controller reads.
    static void write(Trb& slot, const Trb& trb) {
        slot.parameter = trb.parameter;
        slot.status = trb.status;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        slot.control = trb.control;
    }

    Trb* base_ = nullptr;
    u64 base_physical_ = 0;
    u32 trbs_ = 0;
    u32 index_ = 0;
    bool cycle_ = true;
};

// The consumer side of the event ring. The controller fills a slot and then
// sets its cycle bit to the value the consumer is waiting for; until then the
// slot is left alone.
class EventRing {
public:
    void init(const Trb* base, u32 trbs) {
        base_ = base;
        trbs_ = trbs;
        index_ = 0;
        cycle_ = true;
    }

    bool pop(Trb& out) {
        if (base_ == nullptr || trbs_ == 0) {
            return false;
        }
        const volatile Trb* slot = base_ + index_;
        const u32 control = slot->control;
        if ((control & kTrbCycle) != (cycle_ ? kTrbCycle : 0u)) {
            return false;
        }
        out.parameter = slot->parameter;
        out.status = slot->status;
        out.control = control;
        if (++index_ == trbs_) {
            index_ = 0;
            cycle_ = !cycle_;
        }
        return true;
    }

    u32 dequeue_index() const { return index_; }

private:
    const Trb* base_ = nullptr;
    u32 trbs_ = 0;
    u32 index_ = 0;
    bool cycle_ = true;
};

constexpr u32 event_type(const Trb& trb) {
    return trb_type(trb.control);
}

constexpr u32 completion_code(const Trb& trb) {
    return (trb.status >> 24) & 0xFFu;
}

constexpr u8 event_slot(const Trb& trb) {
    return static_cast<u8>((trb.control >> kTrbSlotShift) & 0xFFu);
}

constexpr u8 event_endpoint(const Trb& trb) {
    return static_cast<u8>((trb.control >> 16) & 0x1Fu);
}

constexpr u8 port_id(const Trb& trb) {
    return static_cast<u8>((trb.parameter >> 24) & 0xFFu);
}

struct KeyboardInterface {
    u8 configuration_value;
    u8 interface_number;
    u8 endpoint_address;
    u16 max_packet;
    u8 interval;
};

inline constexpr u8 kDescriptorConfiguration = 2;
inline constexpr u8 kDescriptorInterface = 4;
inline constexpr u8 kDescriptorEndpoint = 5;

inline constexpr u8 kHidClass = 3;
inline constexpr u8 kBootSubclass = 1;
inline constexpr u8 kKeyboardProtocol = 1;
inline constexpr u8 kMouseProtocol = 2;

inline constexpr u8 kEndpointInterrupt = 3;
inline constexpr u8 kEndpointIn = 0x80;

// Walks the length and type pairs of a configuration descriptor and reports the
// first HID boot interface with class 3, subclass 1 and `protocol` (1 keyboard,
// 2 mouse), followed by its interrupt IN endpoint. A descriptor shorter than
// two bytes or one that reaches past the buffer makes the whole descriptor
// invalid.
constexpr bool find_boot_hid(const u8* config, u32 length, u8 protocol, KeyboardInterface& out) {
    u32 offset = 0;
    u8 configuration_value = 0;
    u8 interface_number = 0;
    bool boot_interface = false;
    while (offset < length) {
        if (length - offset < 2u) {
            return false;
        }
        const u8 descriptor_length = config[offset];
        const u8 descriptor_type = config[offset + 1];
        if (descriptor_length < 2 || static_cast<u32>(descriptor_length) > length - offset) {
            return false;
        }
        if (descriptor_type == kDescriptorConfiguration && descriptor_length >= 9) {
            configuration_value = config[offset + 5];
        } else if (descriptor_type == kDescriptorInterface) {
            boot_interface = descriptor_length >= 9 && config[offset + 5] == kHidClass &&
                             config[offset + 6] == kBootSubclass &&
                             config[offset + 7] == protocol;
            if (boot_interface) {
                interface_number = config[offset + 2];
            }
        } else if (descriptor_type == kDescriptorEndpoint && boot_interface &&
                   descriptor_length >= 7 &&
                   (config[offset + 3] & kEndpointInterrupt) == kEndpointInterrupt &&
                   (config[offset + 2] & kEndpointIn) != 0) {
            out.configuration_value = configuration_value;
            out.interface_number = interface_number;
            out.endpoint_address = config[offset + 2];
            out.max_packet = static_cast<u16>(config[offset + 4] | (config[offset + 5] << 8));
            out.interval = config[offset + 6];
            return true;
        }
        offset += descriptor_length;
    }
    return false;
}

constexpr bool find_boot_keyboard(const u8* config, u32 length, KeyboardInterface& out) {
    return find_boot_hid(config, length, kKeyboardProtocol, out);
}

// Fixed-capacity FIFO of boot HID reports. When full, pushing a new report
// overwrites the oldest entry so the most recent input is kept.
class ReportQueue {
public:
    static constexpr u32 kCapacity = 16;
    static constexpr u32 kMaxReport = 8;

    constexpr void clear() {
        head_ = 0;
        count_ = 0;
    }

    constexpr bool push(const u8* report, u32 length) {
        if (report == nullptr || length == 0) {
            return false;
        }
        const u32 capped = length > kMaxReport ? kMaxReport : length;
        u32 slot = 0;
        if (count_ == kCapacity) {
            slot = head_;
            head_ = (head_ + 1) % kCapacity;
        } else {
            slot = (head_ + count_) % kCapacity;
            ++count_;
        }
        for (u32 i = 0; i < capped; ++i) {
            reports_[slot][i] = report[i];
        }
        for (u32 i = capped; i < kMaxReport; ++i) {
            reports_[slot][i] = 0;
        }
        lengths_[slot] = static_cast<u8>(capped);
        return true;
    }

    constexpr bool pop(u8* out, u32& length) {
        if (count_ == 0) {
            return false;
        }
        const u32 slot = head_;
        head_ = (head_ + 1) % kCapacity;
        --count_;
        const u32 n = lengths_[slot];
        if (out != nullptr) {
            for (u32 i = 0; i < n; ++i) {
                out[i] = reports_[slot][i];
            }
        }
        length = n;
        return true;
    }

    constexpr u32 size() const { return count_; }

private:
    u8 reports_[kCapacity][kMaxReport] = {};
    u8 lengths_[kCapacity] = {};
    u32 head_ = 0;
    u32 count_ = 0;
};

constexpr u8 clamp_interval(int value, int low, int high) {
    return static_cast<u8>(value < low ? low : (value > high ? high : value));
}

// The interval the endpoint context takes. Full and low speed name a period in
// frames, which becomes an exponent by taking its base two logarithm; high speed
// and above already name a power of two microframes.
constexpr u8 endpoint_interval(u8 speed_id, u8 binterval) {
    if (speed_id == 1 || speed_id == 2) {
        int exponent = 0;
        for (u32 value = binterval; value > 1u; value >>= 1) {
            ++exponent;
        }
        return clamp_interval(exponent + 3, 3, 10);
    }
    return clamp_interval(static_cast<int>(binterval) - 1, 0, 15);
}

constexpr u16 default_ep0_max_packet(u8 speed_id) {
    if (speed_id == 2) {
        return 8;
    }
    if (speed_id >= 4) {
        return 512;
    }
    return 64;
}

inline constexpr u16 kMaxInterruptTransferBytes = 64;

// The Normal TRB length queued on a boot HID interrupt IN endpoint: the
// endpoint's maximum packet size (bits 10:0 of wMaxPacketSize), capped at 64
// bytes.
constexpr u16 interrupt_transfer_length(u16 max_packet) {
    const u16 packet = max_packet & 0x7FFu;
    return packet < kMaxInterruptTransferBytes ? packet : kMaxInterruptTransferBytes;
}

}
