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

#include "hda.hpp"

#include "../common/serial.hpp"
#include "clock.hpp"
#include "pci.hpp"

namespace hombre {

namespace {

constexpr u8 kPciClassOffset = 0x08;
constexpr u8 kPciBar0Offset = 0x10;
constexpr u32 kPciCommandInterruptDisable = 1u << 10;
// Class 0x04 (multimedia) and subclass 0x03 (High Definition Audio), above
// the programming interface and the revision.
constexpr u32 kHdaClass = 0x0403;
constexpr u32 kClassShift = 16;

// Controller registers, at the start of BAR0. The stream descriptors
// follow at 0x80: input streams first, then output, then bidirectional.
constexpr u32 kGcap = 0x00;
constexpr u32 kGctl = 0x08;
constexpr u32 kStatests = 0x0E;
constexpr u32 kIntctl = 0x20;
constexpr u32 kCorbBase = 0x40;
constexpr u32 kCorbWp = 0x48;
constexpr u32 kCorbRp = 0x4A;
constexpr u32 kCorbCtl = 0x4C;
constexpr u32 kCorbSize = 0x4E;
constexpr u32 kRirbBase = 0x50;
constexpr u32 kRirbWp = 0x58;
constexpr u32 kRintCnt = 0x5A;
constexpr u32 kRirbCtl = 0x5C;
constexpr u32 kRirbSts = 0x5D;
constexpr u32 kRirbSize = 0x5E;
constexpr u32 kStreams = 0x80;
constexpr u32 kStreamBytes = 0x20;
// Covers the stream descriptors of the largest controller, 30 streams.
constexpr u32 kRegisterWindow = 0x1000;

constexpr u16 kGcap64Ok = 1u << 0;
constexpr u32 kGctlRunning = 1u << 0;
constexpr u16 kCorbRpReset = 1u << 15;
constexpr u16 kRirbWpReset = 1u << 15;
constexpr u8 kRirbResponseInterrupt = 1u << 0;
constexpr u8 kCorbRun = 1u << 1;
constexpr u8 kRirbDmaEnable = 1u << 1;
// RINTFL and RIRBOIS. RINTFL is set once RINTCNT responses have arrived, and
// a controller may hold back further commands until it is cleared, so it is
// cleared after every response.
constexpr u8 kRirbStatusClear = 0x05;
constexpr u32 kRirbUnsolicited = 1u << 4;
constexpr u16 kResponsesPerStatus = 1;

// Stream descriptor registers. CTL is three bytes; its third holds the
// stream tag in bits 7:4.
constexpr u32 kSdCtl = 0x00;
constexpr u32 kSdCtlTag = 0x02;
constexpr u32 kSdSts = 0x03;
constexpr u32 kSdLpib = 0x04;
constexpr u32 kSdCbl = 0x08;
constexpr u32 kSdLvi = 0x0C;
constexpr u32 kSdFmt = 0x12;
constexpr u32 kSdBdl = 0x18;
constexpr u8 kSdReset = 1u << 0;
constexpr u8 kSdRun = 1u << 1;
constexpr u8 kSdStatusClear = 0x1C;  // BCIS, FIFOE and DESE

// Verbs and the parameters of GET_PARAMETER.
constexpr u32 kGetParameter = 0xF00;
constexpr u32 kGetConnectionEntry = 0xF02;
constexpr u32 kSetConnectionSelect = 0x701;
constexpr u32 kSetPowerState = 0x705;
constexpr u32 kSetChannelStreamId = 0x706;
constexpr u32 kSetPinControl = 0x707;
constexpr u32 kGetEapd = 0xF0C;
constexpr u32 kSetEapd = 0x70C;
constexpr u32 kGetConfigDefault = 0xF1C;
constexpr u32 kSetConverterFormat = 0x2;
constexpr u32 kSetAmpGainMute = 0x3;

constexpr u32 kParamVendorId = 0x00;
constexpr u32 kParamNodeCount = 0x04;
constexpr u32 kParamFunctionType = 0x05;
constexpr u32 kParamWidgetCaps = 0x09;
constexpr u32 kParamPinCaps = 0x0C;
constexpr u32 kParamInAmpCaps = 0x0D;
constexpr u32 kParamConnectionLength = 0x0E;
constexpr u32 kParamOutAmpCaps = 0x12;

constexpr u32 kAudioFunctionGroup = 0x01;
constexpr u32 kPowerD0 = 0;
constexpr u32 kConnectionLongForm = 1u << 7;
constexpr u32 kConnectionCountMask = 0x7Fu;

constexpr u32 kWidgetTypeShift = 20;
constexpr u32 kWidgetOutput = 0x0;
constexpr u32 kWidgetMixer = 0x2;
constexpr u32 kWidgetSelector = 0x3;
constexpr u32 kWidgetPin = 0x4;
constexpr u32 kWidgetInAmp = 1u << 1;
constexpr u32 kWidgetOutAmp = 1u << 2;
constexpr u32 kWidgetOwnAmpCaps = 1u << 3;
constexpr u32 kWidgetConnectionList = 1u << 8;
constexpr u32 kWidgetDigital = 1u << 9;
constexpr u32 kWidgetPowerControl = 1u << 10;

constexpr u32 kPinHeadphoneDrive = 1u << 3;
constexpr u32 kPinOutput = 1u << 4;
constexpr u32 kPinEapd = 1u << 16;
constexpr u32 kPinControlOut = 1u << 6;
constexpr u32 kPinControlHeadphone = 1u << 7;
constexpr u32 kEapdEnable = 1u << 1;

constexpr u32 kConfigPortShift = 30;
constexpr u32 kConfigPortNone = 1;
constexpr u32 kConfigDeviceShift = 20;
constexpr u32 kDeviceLineOut = 0x0;
constexpr u32 kDeviceSpeaker = 0x1;
constexpr u32 kDeviceHeadphone = 0x2;
constexpr u32 kPreferredDevices[] = {kDeviceLineOut, kDeviceSpeaker, kDeviceHeadphone};

// Node ids are seven bits wide in a command.
constexpr u32 kMaxNodes = 128;
// A pin, at most three mixers or selectors, and the converter.
constexpr u32 kMaxPathNodes = 5;

constexpr u64 kResetTimeoutUs = 100000;
// The link is held in reset for at least four frames of 20.8 us.
constexpr u64 kLinkResetHoldUs = 100;
// Codecs ask for an address within 25 frames of the link leaving reset.
constexpr u64 kCodecWakeUs = 1000;
// Some controllers clear the CORB read pointer without ever showing the
// reset bit as set, so the wait for it is short and its outcome ignored.
constexpr u64 kPointerResetUs = 1000;
constexpr u64 kVerbTimeoutUs = 100000;
constexpr u64 kPowerSettleUs = 10000;

// One page holds the CORB (up to 256 four-byte entries), the RIRB (up to 256
// eight-byte entries) and the buffer descriptor list, each 128-byte
// aligned. The sample ring follows in the pages after it.
constexpr u32 kPageBytes = 4096;
constexpr u32 kCorbOffset = 0;
constexpr u32 kRirbOffset = 1024;
constexpr u32 kBdlOffset = 3072;
constexpr u32 kSamplesOffset = kPageBytes;
constexpr u32 kBufferBytes = kHdaBufferFrames * kHdaFrameBytes;
constexpr u32 kRingBytes = kHdaRingFrames * kHdaFrameBytes;
constexpr u64 kDmaPages = 1 + kRingBytes / kPageBytes;
constexpr u64 kFourGiB = 1ull << 32;

constexpr const char* kErrorNoResponse = "codec does not respond";

struct BdlEntry {
    u64 address;
    u32 length;
    u32 flags;
};

static_assert(sizeof(BdlEntry) == 16);
static_assert(kBdlOffset + kHdaBuffers * sizeof(BdlEntry) <= kPageBytes);

struct Path {
    u32 nodes[kMaxPathNodes];
    // The index in the connection list of nodes[k] that leads to nodes[k + 1],
    // and how many connections nodes[k] has.
    u32 select[kMaxPathNodes];
    u32 choices[kMaxPathNodes];
    u32 length;
    u32 pin_caps;
    u32 device;
};

struct Hda {
    PciAddress address{};
    volatile u8* regs = nullptr;
    volatile u8* stream = nullptr;
    u64 dma_phys = 0;
    u32* corb = nullptr;
    // Response and extended response, two dwords per entry.
    const volatile u32* rirb = nullptr;
    u32 corb_mask = 0;
    u32 rirb_mask = 0;
    u32 corb_wp = 0;
    u32 rirb_rp = 0;
    u32 codec = 0;
    // Set by the first verb that gets no response; later verbs are not sent.
    bool no_response = false;
    u32 widgets_first = 0;
    u32 widgets_end = 0;
    u32 widget_caps[kMaxNodes] = {};
    u32 out_amp_caps = 0;
    u32 in_amp_caps = 0;
    i16* samples = nullptr;
    u32 write_frame = 0;
    bool ready = false;
};

constinit Hda hda{};

void cpu_pause() {
    asm volatile("pause");
}

// Orders memory writes the controller reads before the register write that
// tells it to read them.
void memory_fence() {
    asm volatile("mfence" : : : "memory");
}

template <typename T>
T read(const volatile u8* base, u32 offset) {
    return *reinterpret_cast<const volatile T*>(base + offset);
}

template <typename T>
void write(volatile u8* base, u32 offset, T value) {
    *reinterpret_cast<volatile T*>(base + offset) = value;
}

// Lower half first: a controller need not accept one 64-bit store.
void write_address(volatile u8* base, u32 offset, u64 phys) {
    write<u32>(base, offset, static_cast<u32>(phys));
    write<u32>(base, offset + 4, static_cast<u32>(phys >> 32));
}

template <typename T>
bool wait_for(const volatile u8* base, u32 offset, T mask, T value, u64 timeout_us) {
    const Deadline deadline = deadline_in_us(timeout_us);
    while ((read<T>(base, offset) & mask) != value) {
        if (expired(deadline)) {
            return false;
        }
        cpu_pause();
    }
    return true;
}

void log(const char* text) {
    serial::write("hda: ");
    serial::write(text);
    serial::write_char('\n');
}

// Puts the controller in reset, which stops all its DMA, and turns bus
// mastering off before the pages are given back.
bool fail(PhysAllocator& pages, const char* reason) {
    log(reason);
    if (hda.regs != nullptr) {
        write<u32>(hda.regs, kGctl, 0);
        const u32 command = pci_read32(hda.address, kPciCommandOffset) & 0xFFFFu;
        pci_write32(hda.address, kPciCommandOffset, command & ~kPciCommandBusMaster);
    }
    if (hda.dma_phys != 0) {
        pages.free_pages(hda.dma_phys, kDmaPages);
    }
    hda = Hda{};
    return false;
}

struct FindState {
    PciAddress address;
    bool found;
};

bool match_controller(void* ctx, PciAddress address, u16, u16) {
    if ((pci_read32(address, kPciClassOffset) >> kClassShift) != kHdaClass) {
        return false;
    }
    *static_cast<FindState*>(ctx) = FindState{address, true};
    return true;
}

constexpr u32 input_streams(u16 gcap) {
    return (gcap >> 8) & 0xFu;
}

constexpr u32 output_streams(u16 gcap) {
    return (gcap >> 12) & 0xFu;
}

constexpr u32 bidirectional_streams(u16 gcap) {
    return (gcap >> 3) & 0x1Fu;
}

// The DMA engines must be stopped before the link enters reset. Leaving
// reset writes GCTL whole, so unsolicited responses stay off.
bool reset_controller() {
    volatile u8* regs = hda.regs;
    const u16 gcap = read<u16>(regs, kGcap);
    const u32 streams = input_streams(gcap) + output_streams(gcap) + bidirectional_streams(gcap);
    write<u8>(regs, kCorbCtl, 0);
    write<u8>(regs, kRirbCtl, 0);
    for (u32 i = 0; i < streams; ++i) {
        const u32 control = kStreams + i * kStreamBytes + kSdCtl;
        write<u8>(regs, control, 0);
        if (!wait_for<u8>(regs, control, kSdRun, 0, kResetTimeoutUs)) {
            return false;
        }
    }
    if (!wait_for<u8>(regs, kCorbCtl, kCorbRun, 0, kResetTimeoutUs) ||
        !wait_for<u8>(regs, kRirbCtl, kRirbDmaEnable, 0, kResetTimeoutUs)) {
        return false;
    }
    write<u32>(regs, kGctl, 0);
    if (!wait_for<u32>(regs, kGctl, kGctlRunning, 0, kResetTimeoutUs)) {
        return false;
    }
    delay_us(kLinkResetHoldUs);
    write<u32>(regs, kGctl, kGctlRunning);
    if (!wait_for<u32>(regs, kGctl, kGctlRunning, kGctlRunning, kResetTimeoutUs)) {
        return false;
    }
    write<u32>(regs, kIntctl, 0);
    return true;
}

// Returns the STATESTS bits of the codecs that asked for an address, 0 when
// none did in time.
u16 wait_for_codecs() {
    delay_us(kCodecWakeUs);
    const Deadline deadline = deadline_in_us(kResetTimeoutUs);
    u16 codecs = read<u16>(hda.regs, kStatests) & 0x7FFFu;
    while (codecs == 0 && !expired(deadline)) {
        cpu_pause();
        codecs = read<u16>(hda.regs, kStatests) & 0x7FFFu;
    }
    return codecs;
}

bool start_command_rings(u8* dma) {
    volatile u8* regs = hda.regs;
    const HdaRingSize corb = hda_ring_size(read<u8>(regs, kCorbSize));
    const HdaRingSize rirb = hda_ring_size(read<u8>(regs, kRirbSize));
    write<u8>(regs, kCorbSize, static_cast<u8>((read<u8>(regs, kCorbSize) & ~3u) | corb.code));
    write<u8>(regs, kRirbSize, static_cast<u8>((read<u8>(regs, kRirbSize) & ~3u) | rirb.code));
    hda.corb = reinterpret_cast<u32*>(dma + kCorbOffset);
    hda.rirb = reinterpret_cast<const volatile u32*>(dma + kRirbOffset);
    hda.corb_mask = corb.entries - 1;
    hda.rirb_mask = rirb.entries - 1;

    write_address(regs, kCorbBase, hda.dma_phys + kCorbOffset);
    write<u16>(regs, kCorbRp, kCorbRpReset);
    wait_for<u16>(regs, kCorbRp, kCorbRpReset, kCorbRpReset, kPointerResetUs);
    write<u16>(regs, kCorbRp, 0);
    if (!wait_for<u16>(regs, kCorbRp, kCorbRpReset, 0, kResetTimeoutUs)) {
        return false;
    }
    write<u16>(regs, kCorbWp, 0);

    write_address(regs, kRirbBase, hda.dma_phys + kRirbOffset);
    write<u16>(regs, kRirbWp, kRirbWpReset);
    write<u16>(regs, kRintCnt, kResponsesPerStatus);

    write<u8>(regs, kCorbCtl, kCorbRun);
    // The response count interrupt flag must be on for the controller to
    // flag each batch of responses; clearing the flag lets it fetch more
    // commands. No interrupt is raised: INTCTL stays 0.
    write<u8>(regs, kRirbCtl, kRirbDmaEnable | kRirbResponseInterrupt);
    return wait_for<u8>(regs, kCorbCtl, kCorbRun, kCorbRun, kResetTimeoutUs) &&
           wait_for<u8>(regs, kRirbCtl, kRirbDmaEnable, kRirbDmaEnable, kResetTimeoutUs);
}

// Sends one command through the CORB and waits for its response in the
// RIRB, passing over unsolicited ones. A command that gets no response sets
// hda.no_response, after which no command is sent and every one returns 0,
// so a sequence of commands is checked once at its end.
u32 command(u32 encoded) {
    if (hda.no_response) {
        return 0;
    }
    hda.corb_wp = (hda.corb_wp + 1) & hda.corb_mask;
    hda.corb[hda.corb_wp] = encoded;
    memory_fence();
    write<u16>(hda.regs, kCorbWp, static_cast<u16>(hda.corb_wp));
    const Deadline deadline = deadline_in_us(kVerbTimeoutUs);
    while (!expired(deadline)) {
        while (hda.rirb_rp != (read<u16>(hda.regs, kRirbWp) & hda.rirb_mask)) {
            hda.rirb_rp = (hda.rirb_rp + 1) & hda.rirb_mask;
            const u32 response = hda.rirb[2 * hda.rirb_rp];
            const u32 extended = hda.rirb[2 * hda.rirb_rp + 1];
            write<u8>(hda.regs, kRirbSts, kRirbStatusClear);
            if ((extended & kRirbUnsolicited) == 0) {
                return response;
            }
        }
        cpu_pause();
    }
    hda.no_response = true;
    return 0;
}

u32 verb(u32 node, u32 id, u32 payload) {
    return command(hda_verb(hda.codec, node, id, payload));
}

u32 verb16(u32 node, u32 id, u32 payload) {
    return command(hda_verb16(hda.codec, node, id, payload));
}

u32 parameter(u32 node, u32 id) {
    return verb(node, kGetParameter, id);
}

u32 widget_type(u32 caps) {
    return (caps >> kWidgetTypeShift) & 0xFu;
}

// The subordinate nodes of `node`, limited to the ids a command can name.
void subordinate_nodes(u32 node, u32& first, u32& end) {
    const u32 count = parameter(node, kParamNodeCount);
    first = (count >> 16) & 0xFFu;
    const u32 last = first + (count & 0xFFu);
    end = last < kMaxNodes ? last : kMaxNodes;
}

bool is_widget(u32 node) {
    return node >= hda.widgets_first && node < hda.widgets_end;
}

HdaConnections connections(u32 node) {
    HdaConnections list{};
    if ((hda.widget_caps[node] & kWidgetConnectionList) == 0) {
        return list;
    }
    const u32 length = parameter(node, kParamConnectionLength);
    const bool long_form = (length & kConnectionLongForm) != 0;
    const u32 per_response = long_form ? 2 : 4;
    const u32 count = length & kConnectionCountMask;
    for (u32 i = 0; i < count; i += per_response) {
        const u32 left = count - i;
        hda_add_connections(list, verb(node, kGetConnectionEntry, i),
                            left < per_response ? left : per_response, long_form);
    }
    return list;
}

bool on_path(const Path& path, u32 node) {
    for (u32 k = 0; k < path.length; ++k) {
        if (path.nodes[k] == node) {
            return true;
        }
    }
    return false;
}

// Extends `path` depth first from its last node through mixers and
// selectors until it ends at an analog Audio Output converter.
bool extend(Path& path) {
    const u32 last = path.length - 1;
    const HdaConnections list = connections(path.nodes[last]);
    path.choices[last] = list.count;
    for (u32 i = 0; i < list.count; ++i) {
        const u32 node = list.nodes[i];
        if (!is_widget(node) || on_path(path, node)) {
            continue;
        }
        const u32 caps = hda.widget_caps[node];
        const u32 type = widget_type(caps);
        path.select[last] = i;
        path.nodes[path.length++] = node;
        if (type == kWidgetOutput && (caps & kWidgetDigital) == 0) {
            return true;
        }
        if ((type == kWidgetMixer || type == kWidgetSelector) && path.length < kMaxPathNodes &&
            extend(path)) {
            return true;
        }
        --path.length;
    }
    return false;
}

// Tries the output-capable, connected pins by device type, Line Out first,
// then Speaker, then HP Out, and takes the first with a path to a converter.
bool find_path(Path& path) {
    for (const u32 device : kPreferredDevices) {
        for (u32 pin = hda.widgets_first; pin < hda.widgets_end; ++pin) {
            if (widget_type(hda.widget_caps[pin]) != kWidgetPin) {
                continue;
            }
            const u32 pin_caps = parameter(pin, kParamPinCaps);
            const u32 config = verb(pin, kGetConfigDefault, 0);
            if ((pin_caps & kPinOutput) == 0 || (config >> kConfigPortShift) == kConfigPortNone ||
                ((config >> kConfigDeviceShift) & 0xFu) != device) {
                continue;
            }
            path = Path{{pin}, {}, {}, 1, pin_caps, device};
            if (extend(path)) {
                return true;
            }
        }
    }
    return false;
}

u32 amp_caps(u32 node, u32 parameter_id, u32 function_group_caps) {
    return (hda.widget_caps[node] & kWidgetOwnAmpCaps) != 0 ? parameter(node, parameter_id)
                                                             : function_group_caps;
}

// Powers every widget on the path, unmutes its amplifiers at 0 dB and
// selects the next node where there is a choice, then enables the pin and
// sets the converter to the stream's format and tag. A mixer sums its
// inputs, so it gets no selection; a pin's input amplifier belongs to
// recording and is left alone.
void configure(const Path& path) {
    for (u32 k = 0; k < path.length; ++k) {
        const u32 node = path.nodes[k];
        const u32 caps = hda.widget_caps[node];
        const u32 type = widget_type(caps);
        if ((caps & kWidgetPowerControl) != 0) {
            verb(node, kSetPowerState, kPowerD0);
        }
        if ((caps & kWidgetOutAmp) != 0) {
            verb16(node, kSetAmpGainMute,
                   hda_amp_unmute(amp_caps(node, kParamOutAmpCaps, hda.out_amp_caps), true, 0));
        }
        if (k + 1 == path.length) {
            continue;
        }
        if (type != kWidgetPin && (caps & kWidgetInAmp) != 0) {
            verb16(node, kSetAmpGainMute,
                   hda_amp_unmute(amp_caps(node, kParamInAmpCaps, hda.in_amp_caps), false,
                                  path.select[k]));
        }
        if (type != kWidgetMixer && path.choices[k] > 1) {
            verb(node, kSetConnectionSelect, path.select[k]);
        }
    }
    const u32 pin = path.nodes[0];
    const bool headphone =
        path.device == kDeviceHeadphone && (path.pin_caps & kPinHeadphoneDrive) != 0;
    verb(pin, kSetPinControl, kPinControlOut | (headphone ? kPinControlHeadphone : 0));
    if ((path.pin_caps & kPinEapd) != 0) {
        verb(pin, kSetEapd, verb(pin, kGetEapd, 0) | kEapdEnable);
    }
    const u32 converter = path.nodes[path.length - 1];
    verb16(converter, kSetConverterFormat, kHdaStreamFormat);
    verb(converter, kSetChannelStreamId, static_cast<u32>(kHdaStreamTag) << 4);
}

void log_codec(u32 vendor) {
    serial::write("hda: codec ");
    serial::write_dec(hda.codec);
    serial::write(" vendor 0x");
    serial::write_hex(vendor);
    serial::write_char('\n');
}

void log_path(const Path& path) {
    serial::write("hda: pin ");
    serial::write_dec(path.nodes[0]);
    serial::write(", converter ");
    serial::write_dec(path.nodes[path.length - 1]);
    serial::write(", path ");
    for (u32 k = 0; k < path.length; ++k) {
        serial::write(k == 0 ? "" : " > ");
        serial::write_dec(path.nodes[k]);
    }
    serial::write_char('\n');
}

// Sets up the audio function group of codec `codec` and a path from an
// output pin to a converter. Returns why it failed, or nullptr.
const char* setup_codec(u32 codec) {
    hda.codec = codec;
    const u32 vendor = parameter(0, kParamVendorId);
    u32 first = 0;
    u32 end = 0;
    subordinate_nodes(0, first, end);
    u32 group = 0;
    for (u32 node = first; node < end && group == 0; ++node) {
        if ((parameter(node, kParamFunctionType) & 0xFFu) == kAudioFunctionGroup) {
            group = node;
        }
    }
    if (hda.no_response) {
        return kErrorNoResponse;
    }
    log_codec(vendor);
    if (group == 0) {
        return "codec has no audio function group";
    }

    verb(group, kSetPowerState, kPowerD0);
    delay_us(kPowerSettleUs);
    subordinate_nodes(group, hda.widgets_first, hda.widgets_end);
    hda.out_amp_caps = parameter(group, kParamOutAmpCaps);
    hda.in_amp_caps = parameter(group, kParamInAmpCaps);
    for (u32 node = hda.widgets_first; node < hda.widgets_end; ++node) {
        hda.widget_caps[node] = parameter(node, kParamWidgetCaps);
    }

    Path path{};
    const bool found = find_path(path);
    if (hda.no_response) {
        return kErrorNoResponse;
    }
    if (!found) {
        return "no output pin with a path to a converter";
    }
    configure(path);
    if (hda.no_response) {
        return kErrorNoResponse;
    }
    log_path(path);
    return nullptr;
}

// Resets the stream, points it at the ring through four buffer descriptors
// without completion interrupts and sets it running.
bool start_stream(u8* dma) {
    volatile u8* sd = hda.stream;
    write<u8>(sd, kSdCtl, kSdReset);
    if (!wait_for<u8>(sd, kSdCtl, kSdReset, kSdReset, kResetTimeoutUs)) {
        return false;
    }
    write<u8>(sd, kSdCtl, 0);
    if (!wait_for<u8>(sd, kSdCtl, kSdReset, 0, kResetTimeoutUs)) {
        return false;
    }
    write<u8>(sd, kSdSts, kSdStatusClear);

    BdlEntry* bdl = reinterpret_cast<BdlEntry*>(dma + kBdlOffset);
    for (u32 i = 0; i < kHdaBuffers; ++i) {
        bdl[i] = BdlEntry{hda.dma_phys + kSamplesOffset + i * kBufferBytes, kBufferBytes, 0};
    }
    write_address(sd, kSdBdl, hda.dma_phys + kBdlOffset);
    write<u32>(sd, kSdCbl, kRingBytes);
    write<u16>(sd, kSdLvi, kHdaBuffers - 1);
    write<u16>(sd, kSdFmt, kHdaStreamFormat);
    write<u8>(sd, kSdCtlTag, static_cast<u8>(kHdaStreamTag << 4));
    memory_fence();
    write<u8>(sd, kSdCtl, kSdRun);
    return true;
}

}

bool hda_init(MmioMapper& mmio, PhysAllocator& pages) {
    hda = Hda{};
    if (!clock_init()) {
        return fail(pages, "no usable clock");
    }
    FindState found{{0, 0, 0}, false};
    pci_scan(&match_controller, &found);
    if (!found.found) {
        return fail(pages, "no controller");
    }
    hda.address = found.address;

    const u32 bar_low = pci_read32(hda.address, kPciBar0Offset);
    const bool wide = (bar_low & 1u) == 0 && ((bar_low >> 1) & 3u) == 2;
    const PciBar bar = pci_decode_bar(
        bar_low, wide ? pci_read32(hda.address, static_cast<u8>(kPciBar0Offset + 4)) : 0);
    if (bar.is_io || bar.phys == 0) {
        return fail(pages, "controller has no memory BAR");
    }
    // The controller fetches commands and samples from memory, and is polled.
    const u32 pci_command = pci_read32(hda.address, kPciCommandOffset) & 0xFFFFu;
    pci_write32(hda.address, kPciCommandOffset,
                pci_command | kPciCommandMemorySpace | kPciCommandBusMaster |
                    kPciCommandInterruptDisable);
    hda.regs = static_cast<volatile u8*>(mmio.map(bar.phys, kRegisterWindow));
    if (hda.regs == nullptr) {
        return fail(pages, "cannot map controller registers");
    }

    if (!reset_controller()) {
        return fail(pages, "controller does not reset");
    }
    const u16 codecs = wait_for_codecs();
    if (codecs == 0) {
        return fail(pages, "no codec");
    }
    const u16 gcap = read<u16>(hda.regs, kGcap);
    if (output_streams(gcap) == 0) {
        return fail(pages, "controller has no output stream");
    }
    hda.dma_phys = pages.alloc_pages(kDmaPages);
    if (hda.dma_phys == 0) {
        return fail(pages, "no memory for the rings");
    }
    if ((gcap & kGcap64Ok) == 0 && hda.dma_phys + kDmaPages * kPageBytes > kFourGiB) {
        return fail(pages, "controller needs memory below 4 GiB");
    }
    u8* dma = pages.to_ptr(hda.dma_phys);
    if (!start_command_rings(dma)) {
        return fail(pages, "command rings do not start");
    }
    if (const char* error = setup_codec(static_cast<u32>(__builtin_ctz(codecs)))) {
        return fail(pages, error);
    }
    hda.stream = hda.regs + kStreams + input_streams(gcap) * kStreamBytes;
    if (!start_stream(dma)) {
        return fail(pages, "output stream does not reset");
    }
    hda.samples = reinterpret_cast<i16*>(dma + kSamplesOffset);
    hda.ready = true;
    log("output stream running");
    return true;
}

bool hda_ready() {
    return hda.ready;
}

void hda_pump(u32 (*fill)(void* ctx, i16* out, u32 frames), void* ctx) {
    if (!hda.ready) {
        return;
    }
    const u32 play = read<u32>(hda.stream, kSdLpib) / kHdaFrameBytes % kHdaRingFrames;
    const HdaSpan span = hda_fill_span(play, hda.write_frame);
    u32 at = span.start;
    for (u32 left = span.frames; left != 0;) {
        const u32 piece = left < kHdaRingFrames - at ? left : kHdaRingFrames - at;
        i16* out = hda.samples + at * kHdaChannels;
        const u32 filled = fill(ctx, out, piece);
        for (u32 i = (filled < piece ? filled : piece) * kHdaChannels; i < piece * kHdaChannels;
             ++i) {
            out[i] = 0;
        }
        at = (at + piece) % kHdaRingFrames;
        left -= piece;
    }
    // Drains the CPU's store buffer so the controller's next fetch sees the
    // new frames.
    memory_fence();
    hda.write_frame = at;
}

}
