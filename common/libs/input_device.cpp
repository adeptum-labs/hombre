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

#include "input_device.hpp"

namespace hombre {

using m68k::ExitReason;
using m68k::Machine;
using native::Bus;
using native::LogLine;
using namespace ndk;

namespace {

constexpr u64 kMicroPerSec = 1000000ull;
constexpr char kTaskName[] = "input.device";
constexpr u32 kTaskNameAt = TC_SIZE;
constexpr u32 kTaskBlock = TC_SIZE + sizeof kTaskName;
// The InputEvents of a pass sit on even addresses.
constexpr u32 kEventStride = (ie_SIZEOF + 1) & ~1u;
constexpr u32 kTimerTime = 32;  // tr_time in a timerequest

void fault_null(Bus& b) {
    b.bytes_at(0, b.machine().mem.size + 1);
}

// Guest memory reads and writes that leave the running task's CPU state
// alone, for the device's own task and the hooks exec calls from any task.
struct Mem {
    Machine& m;

    u32 mask() const { return m.jit != nullptr ? m.jit->address_mask() : 0xFFFFFFFF; }

    bool r8(u32 at, u32& v) const { return m.mem.read8(at & mask(), v); }
    bool r16(u32 at, u32& v) const { return (at & 1) == 0 && m.mem.read16(at & mask(), v); }
    bool r32(u32 at, u32& v) const { return (at & 1) == 0 && m.mem.read32(at & mask(), v); }
    bool w8(u32 at, u32 v) const { return m.mem.write8(at & mask(), v); }
    bool w16(u32 at, u32 v) const { return (at & 1) == 0 && m.mem.write16(at & mask(), v); }
    bool w32(u32 at, u32 v) const { return (at & 1) == 0 && m.mem.write32(at & mask(), v); }
};

}

struct InputOps {
    static InputDevice& of(void* ctx) { return *static_cast<InputDevice*>(ctx); }

    static void complete_now(InputDevice& d, Bus& b, u32 io_raw, u8 flags, i8 err) {
        b.w8(io_raw + io_Error, static_cast<u8>(err));
        if (!b.ok() || (flags & IOF_QUICK) != 0) {
            return;
        }
        d.exec_->reply_message(io_raw);
    }

    static void open(void*, Machine& m) {
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        const u32 dev = m.cpu.a[6];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        b.w16(dev + LIB_OPENCNT, b.r16(dev + LIB_OPENCNT) + 1);
        b.w8(dev + LIB_FLAGS, b.r8(dev + LIB_FLAGS) & ~LIBF_DELEXP);
        b.w32(io_raw + io_Unit, 0);
        b.w8(io_raw + io_Error, 0);
        if (b.ok()) {
            native::set_result(m, 0);
        }
    }

    static void close(void*, Machine& m) {
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        const u32 dev = m.cpu.a[6];
        const u32 count = b.r16(dev + LIB_OPENCNT);
        if (count != 0) {
            b.w16(dev + LIB_OPENCNT, count - 1);
        }
        if (native::guest_address(m, io_raw) != 0) {
            b.w32(io_raw + io_Device, 0xFFFFFFFF);
            b.w32(io_raw + io_Unit, 0xFFFFFFFF);
        }
        native::set_result(m, 0);
    }

    static u64 timeval_us(Bus& b, u32 at) {
        const u64 secs = b.r32(at);
        const u64 micro = b.r32(at + 4);
        return secs * kMicroPerSec + micro;
    }

    static void begin_io(void* ctx, Machine& m) {
        InputDevice& d = of(ctx);
        Bus b(m);
        const u32 io_raw = native::guest_address(m, m.cpu.a[1]);
        if (io_raw == 0) {
            fault_null(b);
            return;
        }
        const u16 cmd = static_cast<u16>(b.r16(io_raw + io_Command));
        const u8 flags = static_cast<u8>(b.r8(io_raw + io_Flags));
        const u32 data = native::guest_address(m, b.r32(io_raw + io_Data));
        if (!b.ok()) {
            return;
        }
        b.w8(io_raw + io_Error, 0);
        switch (cmd) {
        case IND_ADDHANDLER: {
            if (data == 0) {
                complete_now(d, b, io_raw, flags, IOERR_BADLENGTH);
                return;
            }
            b.r8(data + LN_PRI);
            if (!b.ok()) {
                return;
            }
            const bool added = d.add_handler(data);
            if (added) {
                d.log_handler("added the", data, nullptr, true);
            }
            complete_now(d, b, io_raw, flags, added ? 0 : -1);
            return;
        }
        case IND_REMHANDLER: {
            if (d.remove_handler(data)) {
                d.log_handler("removed the", data);
            }
            // A call into the handler that another task is in ends before
            // the request comes back, as the device's task would take it
            // only between two calls.
            if (data != 0 && d.running_ == data && d.exec_->this_task() != d.task_) {
                for (InputDevice::PendingRemove& p : d.pending_) {
                    if (p.request == 0) {
                        b.w8(io_raw + io_Flags, flags & ~IOF_QUICK);
                        b.w8(io_raw + LN_TYPE, NT_MESSAGE);
                        if (!b.ok()) {
                            return;
                        }
                        p = InputDevice::PendingRemove{io_raw, d.exec_->this_task(), data};
                        return;
                    }
                }
            }
            complete_now(d, b, io_raw, flags, 0);
            return;
        }
        case IND_WRITEEVENT: {
            if (data == 0) {
                complete_now(d, b, io_raw, flags, IOERR_BADLENGTH);
                return;
            }
            InputEventData e{};
            e.ie_class = static_cast<u8>(b.r8(data + ie_Class));
            e.subclass = static_cast<u8>(b.r8(data + ie_SubClass));
            e.code = static_cast<u16>(b.r16(data + ie_Code));
            e.qualifier = static_cast<u16>(b.r16(data + ie_Qualifier));
            e.x = static_cast<i16>(b.r16(data + ie_X));
            e.y = static_cast<i16>(b.r16(data + ie_Y));
            const u64 now = d.now_us();
            b.w32(data + ie_TimeStamp, static_cast<u32>(now / kMicroPerSec));
            b.w32(data + ie_TimeStamp + 4, static_cast<u32>(now % kMicroPerSec));
            if (!b.ok()) {
                return;
            }
            d.submit(e);
            complete_now(d, b, io_raw, flags, 0);
            return;
        }
        case IND_SETTHRESH:
        case IND_SETPERIOD: {
            const u64 us = timeval_us(b, io_raw + kTimerTime);
            if (!b.ok()) {
                return;
            }
            (cmd == IND_SETTHRESH ? d.threshold_us_ : d.period_us_) = us;
            complete_now(d, b, io_raw, flags, 0);
            return;
        }
        case IND_SETMPORT:
        case IND_SETMTYPE: {
            const u8 value = data != 0 ? static_cast<u8>(b.r8(data)) : 0;
            if (!b.ok()) {
                return;
            }
            (cmd == IND_SETMPORT ? d.mouse_port_ : d.mouse_type_) = value;
            complete_now(d, b, io_raw, flags, 0);
            return;
        }
        case IND_SETMTRIG:
        case CMD_RESET:
        case CMD_FLUSH:
        case CMD_START:
        case CMD_STOP:
            complete_now(d, b, io_raw, flags, 0);
            return;
        default:
            complete_now(d, b, io_raw, flags, IOERR_NOCMD);
            return;
        }
    }

    static void abort_io(void* ctx, Machine& m) {
        InputDevice& d = of(ctx);
        Bus b(m);
        const u32 io_raw = native::guest_address(m, m.cpu.a[1]);
        if (io_raw == 0) {
            fault_null(b);
            return;
        }
        for (InputDevice::PendingRemove& p : d.pending_) {
            if (p.request == io_raw) {
                p = InputDevice::PendingRemove{};
                b.w8(io_raw + io_Error, static_cast<u8>(IOERR_ABORTED));
                if (b.ok()) {
                    d.exec_->reply_message(io_raw);
                }
                if (b.ok()) {
                    native::set_result(m, 0);
                }
                return;
            }
        }
        native::set_result(m, static_cast<u32>(-1));
    }

    static void peek_qualifier(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).qualifier_);
    }

    static void task_body(void* ctx) {
        InputDevice& d = of(ctx);
        for (;;) {
            d.process();
            d.exec_->wait_for(1u << InputDevice::kWakeSignal, ~u64(0));
            d.m_->cpu.exit = ExitReason::None;
        }
    }

    static void drop_hook(void* ctx, u32 task) {
        of(ctx).drop_requests_of(task);
    }

    static void program_end_hook(void* ctx, u32 program) {
        of(ctx).program_ended(program);
    }
};

namespace {

constexpr NativeVector kInputVectors[] = {
    {InputOps::open, "Open"},
    {InputOps::close, "Close"},
    {nullptr, "Expunge"},
    {nullptr, "Reserved"},
    {InputOps::begin_io, "BeginIO"},
    {InputOps::abort_io, "AbortIO"},
    {InputOps::peek_qualifier, "PeekQualifier"},
};

}

bool InputDevice::init(Machine& m, Exec& exec) {
    m_ = &m;
    exec_ = &exec;
    base_ = 0;
    task_ = 0;
    stack_top_ = 0;
    events_ = 0;
    handler_count_ = 0;
    queue_head_ = 0;
    queue_count_ = 0;
    overflow_logged_ = false;
    busy_ = false;
    running_ = 0;
    for (PendingRemove& p : pending_) {
        p = PendingRemove{};
    }
    qualifier_ = 0;
    threshold_us_ = 0;
    period_us_ = 0;
    mouse_port_ = 0;
    mouse_type_ = 0;
    base_ = exec.make_native_device("input.device", kVersion, kInputVectors,
                                    sizeof kInputVectors / sizeof kInputVectors[0], this);
    if (base_ == 0 || !exec.add_timer_drop_hook(InputOps::drop_hook, this) ||
        !exec.add_program_end_hook(InputOps::program_end_hook, this)) {
        m_ = nullptr;
        exec_ = nullptr;
        base_ = 0;
        return false;
    }
    return true;
}

u64 InputDevice::now_us() const {
    return exec_ != nullptr ? exec_->now_us() : 0;
}

// The task, its stack and the InputEvents of a pass, made once.
bool InputDevice::start_task() {
    if (task_ != 0) {
        return true;
    }
    const u32 task = exec_->alloc(kTaskBlock, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 stack = exec_->alloc(kStackBytes, MEMF_PUBLIC | MEMF_CLEAR);
    const u32 events = exec_->alloc(kBatchEvents * kEventStride, MEMF_PUBLIC | MEMF_CLEAR);
    const Mem mem{*m_};
    bool ok = task != 0 && stack != 0 && events != 0;
    for (u32 i = 0; ok && i < sizeof kTaskName; ++i) {
        ok = mem.w8(task + kTaskNameAt + i, static_cast<u8>(kTaskName[i]));
    }
    ok = ok && mem.w8(task + LN_TYPE, NT_TASK) && mem.w8(task + LN_PRI, kTaskPriority) &&
         mem.w32(task + LN_NAME, task + kTaskNameAt) &&
         mem.w32(task + TC_SIGALLOC, 0x0000FFFF | (1u << kWakeSignal)) &&
         mem.w32(task + TC_SPLOWER, stack) && mem.w32(task + TC_SPUPPER, stack + kStackBytes) &&
         mem.w32(task + TC_SPREG, stack + kStackBytes);
    if (ok && exec_->spawn_native(task, InputOps::task_body, this, kTaskPriority, true) != 0) {
        task_ = task;
        stack_top_ = stack + kStackBytes;
        events_ = events;
        return true;
    }
    LogLine().text("input.device: cannot start its task").send();
    if (task != 0) {
        exec_->free(task, kTaskBlock);
    }
    if (stack != 0) {
        exec_->free(stack, kStackBytes);
    }
    if (events != 0) {
        exec_->free(events, kBatchEvents * kEventStride);
    }
    return false;
}

bool InputDevice::installed(u32 interrupt) const {
    for (u32 i = 0; i < handler_count_; ++i) {
        if (handlers_[i].interrupt == interrupt) {
            return true;
        }
    }
    return false;
}

// Enqueued by priority, after those of the same priority, as Enqueue does.
bool InputDevice::add_handler(u32 interrupt) {
    if (installed(interrupt)) {
        return true;
    }
    if (handler_count_ == kMaxHandlers || !start_task()) {
        return false;
    }
    u32 pri = 0;
    if (!Mem{*m_}.r8(interrupt + LN_PRI, pri)) {
        return false;
    }
    const Handler h{interrupt, static_cast<i8>(static_cast<u8>(pri))};
    u32 at = 0;
    while (at < handler_count_ && handlers_[at].priority >= h.priority) {
        ++at;
    }
    for (u32 i = handler_count_; i > at; --i) {
        handlers_[i] = handlers_[i - 1];
    }
    handlers_[at] = h;
    ++handler_count_;
    return true;
}

bool InputDevice::remove_handler(u32 interrupt) {
    for (u32 i = 0; i < handler_count_; ++i) {
        if (handlers_[i].interrupt != interrupt) {
            continue;
        }
        for (u32 j = i + 1; j < handler_count_; ++j) {
            handlers_[j - 1] = handlers_[j];
        }
        --handler_count_;
        return true;
    }
    return false;
}

void InputDevice::log_handler(const char* what, u32 interrupt, const char* why,
                              bool priority) {
    const Mem mem{*m_};
    char name[32];
    name[0] = '\0';
    u32 at = 0;
    if (mem.r32(interrupt + LN_NAME, at) && at != 0) {
        for (u32 i = 0; i + 1 < sizeof name; ++i) {
            u32 c = 0;
            if (!mem.r8(at + i, c) || c == 0) {
                name[i] = '\0';
                break;
            }
            name[i] = static_cast<char>(c);
            name[i + 1] = '\0';
        }
    }
    LogLine line;
    line.text("input.device: ").text(what).text(" handler ");
    if (name[0] != '\0') {
        line.text(name).text(" ");
    }
    line.text("at ").hex(interrupt);
    u32 pri = 0;
    if (priority && mem.r8(interrupt + LN_PRI, pri)) {
        const i8 value = static_cast<i8>(static_cast<u8>(pri));
        line.text(", priority ");
        if (value < 0) {
            line.text("-");
        }
        line.dec(static_cast<u32>(value < 0 ? -value : value));
    }
    if (why != nullptr) {
        line.text(", ").text(why);
    }
    line.send();
}

void InputDevice::submit(const InputEventData& event) {
    if (!chain_active()) {
        if (sink_ != nullptr) {
            sink_(sink_ctx_, event);
        }
        return;
    }
    if (event.ie_class == IECLASS_RAWMOUSE && event.code == IECODE_NOBUTTON &&
        queue_count_ != 0) {
        QueuedEvent& last = queue_[(queue_head_ + queue_count_ - 1) % kQueueCapacity];
        if (last.event.ie_class == IECLASS_RAWMOUSE && last.event.code == IECODE_NOBUTTON &&
            last.event.qualifier == event.qualifier && last.event.subclass == event.subclass) {
            const i32 x = last.event.x + event.x;
            const i32 y = last.event.y + event.y;
            if (x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767) {
                last.event.x = static_cast<i16>(x);
                last.event.y = static_cast<i16>(y);
                return;
            }
        }
    }
    if (queue_count_ == kQueueCapacity) {
        if (!overflow_logged_) {
            overflow_logged_ = true;
            LogLine().text("input.device: events lost, the queue is full").send();
        }
        return;
    }
    queue_[(queue_head_ + queue_count_) % kQueueCapacity] = QueuedEvent{event, now_us()};
    ++queue_count_;
    if (task_ != 0) {
        exec_->signal_task(task_, 1u << kWakeSignal);
    }
}

void InputDevice::submit_host_key(u8 usage, u8 modifiers) {
    InputEventData e{};
    e.ie_class = kClassHostKey;
    e.code = usage;
    e.qualifier = modifiers;
    submit(e);
}

// Calls the handler of the Interrupt with the events at head, as
// Handler(events, is_Data) with the events in A0 and the data in A1, and
// takes the events it returns in D0. A handler that does not return is
// taken out, and the events stay as they were.
bool InputDevice::call_handler(u32 interrupt, u32& head) {
    Machine& m = *m_;
    const Mem mem{m};
    u32 code = 0;
    u32 data = 0;
    if (!mem.r32(interrupt + IS_CODE, code) || !mem.r32(interrupt + IS_DATA, data)) {
        log_handler("removed the", interrupt, "its Interrupt cannot be read");
        remove_handler(interrupt);
        return false;
    }
    if (code == 0 || !exec_->enter_nested_run()) {
        return true;
    }
    const m68k::CpuState saved = m.cpu;
    m.cpu.exit = ExitReason::None;
    m.cpu.a[0] = head;
    m.cpu.a[1] = data;
    m.cpu.a[6] = exec_->exec_base();
    m.cpu.a[7] = stack_top_;
    running_ = interrupt;
    const ExitReason why = m.jit->call_guest(code, kHandlerBlocks);
    running_ = 0;
    exec_->leave_nested_run();
    const u32 result = m.cpu.d[0];
    m.cpu = saved;
    // Forbid and Disable do not outlast the call: a handler is no task of
    // its own that could give them back.
    const u32 exec_base = exec_->exec_base();
    u32 td = 0xFF;
    u32 id = 0xFF;
    mem.r8(exec_base + TDNestCnt, td);
    mem.r8(exec_base + IDNestCnt, id);
    if (td != 0xFF || id != 0xFF) {
        mem.w8(exec_base + TDNestCnt, 0xFF);
        mem.w8(exec_base + IDNestCnt, 0xFF);
        mem.w8(task_ + TC_TDNESTCNT, 0xFF);
        mem.w8(task_ + TC_IDNESTCNT, 0xFF);
        if (why == ExitReason::Returned) {
            log_handler("ended Forbid or Disable left by the", interrupt);
        }
    }
    reply_pending();
    if (why != ExitReason::Returned) {
        if (installed(interrupt)) {
            log_handler("removed the", interrupt, m68k::exit_reason_name(why));
            remove_handler(interrupt);
        }
        return false;
    }
    head = native::guest_address(m, result);
    return true;
}

// Replies the IND_REMHANDLER requests whose handler call has ended.
void InputDevice::reply_pending() {
    for (PendingRemove& p : pending_) {
        if (p.request == 0 || p.interrupt == running_) {
            continue;
        }
        const u32 request = p.request;
        p = PendingRemove{};
        const m68k::CpuState saved = m_->cpu;
        m_->cpu.exit = ExitReason::None;
        Bus b(*m_);
        b.w8(request + io_Error, 0);
        if (b.ok()) {
            exec_->reply_message(request);
        }
        m_->cpu = saved;
    }
}

// The handlers above the sink, or those at or below it, each with the
// events the one before returned. A handler added during the stage waits
// for the next pass; one removed is not called.
void InputDevice::run_stage(bool above_sink, u32& head) {
    u32 order[kMaxHandlers];
    u32 count = 0;
    for (u32 i = 0; i < handler_count_; ++i) {
        if ((handlers_[i].priority > kSinkPriority) == above_sink) {
            order[count++] = handlers_[i].interrupt;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if (installed(order[i])) {
            call_handler(order[i], head);
        }
    }
}

// Takes the events up to the next host key, at most kBatchEvents, off the
// queue and into the InputEvents of a pass, linked in order. Returns the
// first, 0 when the queue starts with a host key or is empty.
u32 InputDevice::pop_batch(u32& count) {
    const Mem mem{*m_};
    count = 0;
    u32 prev = 0;
    while (queue_count_ != 0 && count < kBatchEvents &&
           queue_[queue_head_].event.ie_class != kClassHostKey) {
        const QueuedEvent& q = queue_[queue_head_];
        queue_head_ = (queue_head_ + 1) % kQueueCapacity;
        --queue_count_;
        const u32 at = events_ + count * kEventStride;
        mem.w32(at + ie_NextEvent, 0);
        mem.w8(at + ie_Class, q.event.ie_class);
        mem.w8(at + ie_SubClass, q.event.subclass);
        mem.w16(at + ie_Code, q.event.code);
        mem.w16(at + ie_Qualifier, q.event.qualifier);
        mem.w16(at + ie_X, static_cast<u16>(q.event.x));
        mem.w16(at + ie_Y, static_cast<u16>(q.event.y));
        mem.w32(at + ie_TimeStamp, static_cast<u32>(q.time_us / kMicroPerSec));
        mem.w32(at + ie_TimeStamp + 4, static_cast<u32>(q.time_us % kMicroPerSec));
        if (prev != 0) {
            mem.w32(prev + ie_NextEvent, at);
        }
        prev = at;
        ++count;
    }
    return count != 0 ? events_ : 0;
}

void InputDevice::process() {
    if (m_ == nullptr || busy_) {
        return;
    }
    busy_ = true;
    const Mem mem{*m_};
    while (queue_count_ != 0) {
        if (queue_[queue_head_].event.ie_class == kClassHostKey) {
            const InputEventData key = queue_[queue_head_].event;
            queue_head_ = (queue_head_ + 1) % kQueueCapacity;
            --queue_count_;
            if (sink_ != nullptr) {
                sink_(sink_ctx_, key);
            }
            continue;
        }
        u32 count = 0;
        u32 head = pop_batch(count);
        run_stage(true, head);
        // What reached the sink is read before it acts, so that nothing a
        // handler does with its own events in the meantime matters.
        u32 passed_count = 0;
        for (u32 at = head; at != 0 && passed_count < kMaxPassEvents;) {
            InputEventData e{};
            u32 cls = 0;
            u32 sub = 0;
            u32 code = 0;
            u32 qual = 0;
            u32 x = 0;
            u32 y = 0;
            u32 next = 0;
            if (!mem.r8(at + ie_Class, cls) || !mem.r8(at + ie_SubClass, sub) ||
                !mem.r16(at + ie_Code, code) || !mem.r16(at + ie_Qualifier, qual) ||
                !mem.r16(at + ie_X, x) || !mem.r16(at + ie_Y, y) ||
                !mem.r32(at + ie_NextEvent, next)) {
                LogLine().text("input.device: an event at ").hex(at).text(" cannot be read").send();
                break;
            }
            e.ie_class = static_cast<u8>(cls);
            e.subclass = static_cast<u8>(sub);
            e.code = static_cast<u16>(code);
            e.qualifier = static_cast<u16>(qual);
            e.x = static_cast<i16>(static_cast<u16>(x));
            e.y = static_cast<i16>(static_cast<u16>(y));
            passed_[passed_count++] = e;
            at = native::guest_address(*m_, next);
        }
        for (u32 i = 0; i < passed_count; ++i) {
            if (sink_ != nullptr) {
                sink_(sink_ctx_, passed_[i]);
            }
        }
        run_stage(false, head);
    }
    busy_ = false;
}

void InputDevice::program_ended(u32 program) {
    if (m_ == nullptr || exec_ == nullptr) {
        return;
    }
    const Mem mem{*m_};
    for (u32 i = 0; i < handler_count_;) {
        const u32 interrupt = handlers_[i].interrupt;
        u32 code = 0;
        u32 data = 0;
        mem.r32(interrupt + IS_CODE, code);
        mem.r32(interrupt + IS_DATA, data);
        if (!exec_->program_holds(program, interrupt) &&
            !exec_->program_holds(program, native::guest_address(*m_, code)) &&
            !exec_->program_holds(program, native::guest_address(*m_, data))) {
            ++i;
            continue;
        }
        log_handler("removed the", interrupt, "its program was ended");
        remove_handler(interrupt);
        if (running_ == interrupt) {
            exec_->stop_code_of(task_);
        }
    }
    for (PendingRemove& p : pending_) {
        if (p.request != 0 && exec_->program_holds(program, p.request)) {
            p = PendingRemove{};
        }
    }
}

void InputDevice::drop_requests_of(u32 task) {
    for (PendingRemove& p : pending_) {
        if (p.request != 0 && p.task == task) {
            p = PendingRemove{};
        }
    }
}

}
