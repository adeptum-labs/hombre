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

#include "../types.hpp"
#include "exec.hpp"

// input.device implemented as a native Exec device.
//
// The keyboard and the mouse hand their events to submit. While no input
// handler is added and nothing is queued, an event goes straight to the
// sink, where the kernel lets Intuition and the shell windows act on it, as
// before there was an input.device. Once a program adds a handler, events
// queue for the device's own task, "input.device" at priority 20, which
// runs them through the handlers by priority, as InputEvents in guest
// memory, and hands what is left to the sink. The sink stands where
// Intuition's handler stands on an Amiga, at priority 50: handlers above
// it see the events first and may change, remove or add them; handlers at
// or below it get the events it was given afterwards.
//
// A handler that faults, runs past its block budget or is stopped by the
// system monitor is taken out of the chain and the rest of the chain goes
// on. One that returns with Forbid or Disable held has them undone. A
// handler whose Interrupt, code or data belong to a program the system
// monitor ends is taken out when the End is asked for; if the device's
// task is inside it at that moment, the call is stopped.

namespace hombre {

namespace input_lvo {

constexpr u32 PeekQualifier = 42;

}

namespace ndk {

// devices/input.h
constexpr u16 IND_ADDHANDLER = CMD_NONSTD + 0;
constexpr u16 IND_REMHANDLER = CMD_NONSTD + 1;
constexpr u16 IND_WRITEEVENT = CMD_NONSTD + 2;
constexpr u16 IND_SETTHRESH = CMD_NONSTD + 3;
constexpr u16 IND_SETPERIOD = CMD_NONSTD + 4;
constexpr u16 IND_SETMPORT = CMD_NONSTD + 5;
constexpr u16 IND_SETMTYPE = CMD_NONSTD + 6;
constexpr u16 IND_SETMTRIG = CMD_NONSTD + 7;

// struct Interrupt
constexpr u32 IS_DATA = 14;
constexpr u32 IS_CODE = 18;
constexpr u32 IS_SIZE = 22;

// struct InputEvent
constexpr u32 ie_NextEvent = 0;
constexpr u32 ie_Class = 4;
constexpr u32 ie_SubClass = 5;
constexpr u32 ie_Code = 6;
constexpr u32 ie_Qualifier = 8;
constexpr u32 ie_X = 10;
constexpr u32 ie_Y = 12;
constexpr u32 ie_TimeStamp = 14;
constexpr u32 ie_SIZEOF = 22;

constexpr u8 IECLASS_NULL = 0x00;
constexpr u8 IECLASS_RAWKEY = 0x01;
constexpr u8 IECLASS_RAWMOUSE = 0x02;

constexpr u16 IECODE_UP_PREFIX = 0x80;
constexpr u16 IECODE_LBUTTON = 0x68;
constexpr u16 IECODE_RBUTTON = 0x69;
constexpr u16 IECODE_MBUTTON = 0x6A;
constexpr u16 IECODE_NOBUTTON = 0xFF;

constexpr u16 IEQUALIFIER_MIDBUTTON = 0x1000;
constexpr u16 IEQUALIFIER_RBUTTON = 0x2000;
constexpr u16 IEQUALIFIER_LEFTBUTTON = 0x4000;
constexpr u16 IEQUALIFIER_RELATIVEMOUSE = 0x8000;

}

// The fields of an InputEvent the device and the sink work with.
struct InputEventData {
    u8 ie_class = 0;
    u8 subclass = 0;
    u16 code = 0;
    u16 qualifier = 0;
    i16 x = 0;
    i16 y = 0;
};

class InputDevice {
public:
    static constexpr u16 kVersion = 40;
    static constexpr i8 kTaskPriority = 20;
    // Where the sink stands in the chain, as Intuition's handler.
    static constexpr i8 kSinkPriority = 50;
    static constexpr u32 kMaxHandlers = 32;
    static constexpr u32 kQueueCapacity = 256;
    // InputEvents one pass through the chain carries at most.
    static constexpr u32 kBatchEvents = 32;
    // Events a pass hands the sink at most, those the handlers add included.
    static constexpr u32 kMaxPassEvents = 256;
    static constexpr u32 kStackBytes = 16384;
    // Blocks one handler call may run before it is taken out.
    static constexpr u64 kHandlerBlocks = 1ull << 24;
    // The signal that wakes the device's task.
    static constexpr u32 kWakeSignal = 16;
    // A key with no Amiga raw key code, which bypasses the handlers in its
    // turn: code is its HID usage and qualifier its report's modifier byte.
    static constexpr u8 kClassHostKey = 0xFF;

    using Sink = void (*)(void* ctx, const InputEventData& event);

    // Creates "input.device" version 40 on ExecBase->DeviceList and
    // registers its task-drop and program-end hooks with exec, which must
    // be initialised. The device's task starts with the first handler.
    bool init(m68k::Machine& m, Exec& exec);

    u32 device_base() const { return base_; }

    // Where the events that pass the chain go.
    void set_sink(Sink sink, void* ctx) {
        sink_ = sink;
        sink_ctx_ = ctx;
    }

    // True while events go through the device's task: a handler is added,
    // events wait for the task, or the task is running the chain.
    bool chain_active() const { return handler_count_ != 0 || queue_count_ != 0 || busy_; }

    // True while events wait for the device's task.
    bool has_pending() const { return queue_count_ != 0; }

    // True while the task is in a pass over the handlers, from taking the
    // events off the queue to the last handler below the sink.
    bool in_pass() const { return busy_; }

    // The device's task, 0 before the first handler.
    u32 task() const { return task_; }

    u32 handler_count() const { return handler_count_; }

    // The Interrupt of handler i, by priority, highest first.
    u32 handler(u32 i) const { return i < handler_count_ ? handlers_[i].interrupt : 0; }

    // Hands an event to the chain: queued for the device's task while the
    // chain is active, otherwise straight to the sink. A mouse move joins
    // a move still waiting in the queue with the same qualifier.
    void submit(const InputEventData& event);

    // Hands a key with no Amiga raw key code to the sink in its turn,
    // after the events queued before it.
    void submit_host_key(u8 usage, u8 modifiers);

    // What PeekQualifier reports: the keyboard and mouse button qualifiers
    // as the hardware last showed them.
    void set_qualifier(u16 qualifier) { qualifier_ = qualifier; }
    u16 qualifier() const { return qualifier_; }

    // The key repeat threshold and period IND_SETTHRESH and IND_SETPERIOD
    // last set, in microseconds.
    u64 threshold_us() const { return threshold_us_; }
    u64 period_us() const { return period_us_; }

    // Runs every queued event through the handlers and the sink. The
    // device's task calls it each time it wakes; it must run in that task.
    void process();

    // Takes out the handlers of a program the system monitor ends.
    void program_ended(u32 program);

    // Forgets the requests the task left waiting, without replying.
    void drop_requests_of(u32 task);

private:
    friend struct InputOps;

    struct Handler {
        u32 interrupt = 0;
        i8 priority = 0;
    };

    struct QueuedEvent {
        InputEventData event{};
        u64 time_us = 0;
    };

    // An IND_REMHANDLER waiting for the pass it was made in to end.
    struct PendingRemove {
        u32 request = 0;
        u32 task = 0;
    };
    static constexpr u32 kMaxPending = 8;

    bool start_task();
    bool add_handler(u32 interrupt);
    bool remove_handler(u32 interrupt);
    bool installed(u32 interrupt) const;
    bool call_handler(u32 interrupt, u32& head);
    void run_stage(bool above_sink);
    void drop_events_of(u32 program);
    void sweep_dropped();
    void reply_pending();
    void log_handler(const char* what, u32 interrupt, const char* why = nullptr,
                     bool priority = false);
    u32 overflow_victim() const;
    void drop_queued(u32 from_head);
    u32 pop_batch(u32& count);
    u64 now_us() const;

    m68k::Machine* m_ = nullptr;
    Exec* exec_ = nullptr;
    u32 base_ = 0;
    u32 task_ = 0;
    u32 stack_top_ = 0;
    u32 events_ = 0;  // kBatchEvents InputEvents in guest memory
    Sink sink_ = nullptr;
    void* sink_ctx_ = nullptr;
    Handler handlers_[kMaxHandlers]{};
    u32 handler_count_ = 0;
    QueuedEvent queue_[kQueueCapacity]{};
    InputEventData passed_[kMaxPassEvents]{};  // what a pass hands the sink
    u32 queue_head_ = 0;
    u32 queue_count_ = 0;
    bool overflow_logged_ = false;
    bool busy_ = false;
    u32 pass_head_ = 0;  // the events the rest of the pass works with
    u32 dropped_[kMaxPassEvents]{};  // events dropped for an ended program
    u32 dropped_count_ = 0;
    u32 running_ = 0;  // the Interrupt whose handler runs
    PendingRemove pending_[kMaxPending]{};
    u16 qualifier_ = 0;
    u64 threshold_us_ = 0;
    u64 period_us_ = 0;
    u8 mouse_port_ = 0;
    u8 mouse_type_ = 0;
};

}
