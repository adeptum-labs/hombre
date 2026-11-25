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

#include "monitor.hpp"

#include "../common/bitmap_surface.hpp"
#include "../common/format.hpp"
#include "../common/hid_mouse.hpp"
#include "../common/irq_ring.hpp"
#include "../common/monitor_hotkey.hpp"
#include "../common/monitor_paint.hpp"
#include "../common/monitor_ui.hpp"
#include "../common/pointer_paint.hpp"
#include "../common/port_io.hpp"
#include "../common/serial.hpp"
#include "acpi.hpp"
#include "apic.hpp"
#include "clock.hpp"
#include "percpu.hpp"
#include "usb_keyboard.hpp"

namespace hombre {

namespace {

constexpr u64 kRenderUs = 50000;
// Every cell is drawn again this often, over whatever a draw that was under
// way when the monitor opened has left.
constexpr u64 kRedrawUs = 1000000;
// What a tick may spend drawing; the rest waits for the next.
constexpr u64 kSliceUs = 250;
// How long the tasks have to poll, and so redraw their screen, after the
// monitor closes before the interrupt draws it for them.
constexpr u64 kRestoreUs = 100000;
// The rows of the window painted and shown in one piece.
constexpr u32 kBandRows = 16;
constexpr u32 kBands = (kMonitorHeight + kBandRows - 1) / kBandRows;
// Around the window on a larger screen, the Workbench grey.
constexpr u32 kBackdrop = 0xAAAAAA;
constexpr u32 kLogLineBytes = 80;
constexpr u32 kLogLines = 64;

// A serial line, composed whole: the timer interrupt may only write between
// lines. Too long a line is cut rather than left unterminated.
struct LogLine {
    char text[kLogLineBytes] = {};
    u32 length = 0;

    void add(char c) {
        if (length + 1 < kLogLineBytes) {
            text[length++] = c;
            text[length] = '\0';
        }
    }

    void add(const char* s) {
        for (; *s != '\0'; ++s) {
            add(*s);
        }
    }

    void add_dec(u64 value) { add_number(value, 10); }
    void add_hex(u64 value) { add_number(value, 16); }

    void add_number(u64 value, unsigned base) {
        char digits[21];
        if (format_u64(digits, sizeof digits, value, base) != 0) {
            add(digits);
        }
    }
};

constinit MonitorHost monitor_host{};
constinit CpuMeter cpu_meter{};
constinit MonitorHotkey hotkey{};
constinit MonitorUi ui{};
constinit MonitorSnapshot snapshot{};
constinit MonitorView view{};
// The pens the window is painted into and those the screen shows, the
// pointer aside.
constinit u8 painted[kMonitorWidth * kMonitorHeight]{};
constinit u8 shown[kMonitorWidth * kMonitorHeight]{};
// A row the screen must be given again although its pens have not changed.
constinit bool stale[kMonitorHeight]{};
constinit u32 colours[256]{};
constinit u32 row_colours[kMonitorWidth]{};
constinit BitmapSurface bitmap{};
constinit Framebuffer screen{};
constinit MonitorPlacement place{1, 0, 0};
// The monitor's own pointer, in the shape the system has when it opens.
constinit PointerShape shape{};
constinit PointerState pointer{};
constinit MonitorMouseGate mouse_gate{};
// Where the pointer was last drawn, in pixels of the window.
constinit i32 drawn_x = 0;
constinit i32 drawn_y = 0;
constinit bool wanted = false;
constinit bool open = false;
constinit bool restoring = false;
// The band to paint next; kBands when the window is whole.
constinit u32 band = kBands;
// Input has changed the window since the paint under way began.
constinit bool repaint = false;
constinit u64 next_render_us = 0;
constinit u64 next_redraw_us = 0;
constinit u64 restore_us = 0;
constinit u32 polls_at_close = 0;
constinit bool summary_logged = false;
// The task Enter asked to see, and whether its registers have been logged.
constinit MonitorRequest inspected{};
constinit bool inspect_logged = false;
// Lines waiting for the serial port to be between lines.
constinit IrqRing<LogLine, kLogLines> log_lines{};
constinit LogLine log_next{};
constinit bool log_pending = false;

void log_text(const char* text) {
    LogLine line;
    line.add(text);
    log_lines.push(line);
}

// What the monitor shows, once per open; a line the full queue has no room
// for is dropped.
void log_summary(const MonitorSnapshot& out) {
    LogLine cpus;
    cpus.add("monitor: cpus ");
    cpus.add_dec(out.cpu_count);
    log_lines.push(cpus);

    LogLine memory;
    memory.add("monitor: memory chip ");
    memory.add_dec(out.chip_free >> 20);
    memory.add('/');
    memory.add_dec(out.chip_total >> 20);
    memory.add(" MiB fast ");
    memory.add_dec(out.fast_free >> 20);
    memory.add('/');
    memory.add_dec(out.fast_total >> 20);
    memory.add(" MiB");
    log_lines.push(memory);

    const u32 tasks =
        out.task_count < MonitorSnapshot::kMaxTasks ? out.task_count : MonitorSnapshot::kMaxTasks;
    for (u32 i = 0; i < tasks; ++i) {
        LogLine task;
        task.add("monitor: task ");
        task.add(out.tasks[i].name);
        log_lines.push(task);
    }
}

void close_monitor() {
    wanted = false;
    if (!open) {
        return;
    }
    open = false;
    monitor_host.give_screen();
    restoring = true;
    restore_us = clock_us() + kRestoreUs;
    polls_at_close = monitor_host.polls();
    log_text("monitor: closed");
}

void open_monitor() {
    if (monitor_host.take_screen == nullptr || !monitor_host.take_screen(screen)) {
        return;
    }
    open = true;
    restoring = false;
    place = monitor_placement(screen.width(), screen.height());
    screen.fill(kBackdrop);
    bitmap.init(painted);
    monitor_palette(colours);
    i32 x = kPointerGridWidth / 2;
    i32 y = kPointerGridHeight / 2;
    u8 buttons = 0;
    shape = kDefaultPointerShape;
    if (monitor_host.pointer != nullptr) {
        monitor_host.pointer(shape, x, y, buttons);
    }
    pointer.init(kPointerGridWidth, kPointerGridHeight, x, y);
    drawn_x = pointer.screen_x(kMonitorWidth);
    drawn_y = pointer.screen_y(kMonitorHeight);
    ui.reset();
    ui.start_mouse(drawn_x, drawn_y, buttons);
    // The first report after this one starts from the buttons held now.
    pointer.apply(MouseSample{buttons, 0, 0, 0});
    mouse_gate.passes(true, buttons);
    view.count = 0;
    for (bool& row : stale) {
        row = true;
    }
    band = kBands;
    repaint = false;
    next_render_us = 0;
    next_redraw_us = 0;
    summary_logged = false;
    inspected = MonitorRequest{};
    // The tasks must not go on thinking a key or button is held.
    usb_deliver_release();
    log_text("monitor: open");
}

// Resets the machine by the chipset's reset register, then the keyboard
// controller's reset line, and last by faulting with no handlers left.
[[noreturn]] void reboot() {
    constexpr u16 kResetControl = 0xCF9;
    constexpr u8 kHardReset = 0x02;
    constexpr u8 kResetNow = 0x04;
    constexpr u16 kKeyboardCommand = 0x64;
    constexpr u8 kPulseReset = 0xFE;
    if (serial::mid_line) {
        serial::send('\n');
        serial::mid_line = false;
    }
    serial::write_line_from_interrupt("monitor: reboot");
    outb(kResetControl, kHardReset);
    outb(kResetControl, kHardReset | kResetNow);
    delay_us(10000);
    outb(kKeyboardCommand, kPulseReset);
    delay_us(10000);
    struct __attribute__((packed)) {
        u16 limit;
        u64 base;
    } const no_handlers{0, 0};
    asm volatile("lidt %0; int3" : : "m"(no_handlers));
    for (;;) {
        asm volatile("cli; hlt");
    }
}

void log_disk_request(const MonitorRequest& request) {
    LogLine line;
    if (request.op == MonitorOp::Insert) {
        line.add("monitor: insert DF");
        line.add_dec(request.drive);
        line.add(' ');
        line.add(request.path);
    } else {
        line.add("monitor: eject DF");
        line.add_dec(request.drive);
    }
    log_lines.push(line);
}

void handle_request(const MonitorRequest& request) {
    if (request.op == MonitorOp::Reboot) {
        reboot();
    }
    if (request.op == MonitorOp::Close) {
        close_monitor();
    } else if (request.op == MonitorOp::Inspect) {
        inspected = request;
        inspect_logged = false;
    } else if ((request.op == MonitorOp::Suspend || request.op == MonitorOp::Resume ||
                request.op == MonitorOp::End) &&
               monitor_host.rescue != nullptr) {
        monitor_host.rescue(request);
    } else if ((request.op == MonitorOp::ListDirectory || request.op == MonitorOp::Insert ||
                request.op == MonitorOp::Eject) &&
               monitor_host.disk != nullptr) {
        if (request.op != MonitorOp::ListDirectory) {
            log_disk_request(request);
        }
        monitor_host.disk(request);
    }
}

// Gives the window its parts again, to be painted from the top.
void relayout() {
    ui.layout(snapshot, view);
    repaint = true;
}

bool take_key_report(u8 (&report)[8]) {
    if (hotkey.take(report)) {
        if (open || wanted) {
            close_monitor();
        } else {
            wanted = true;
            open_monitor();
        }
        return false;
    }
    if (!open) {
        return true;
    }
    handle_request(ui.key_report(report, snapshot));
    if (open) {
        relayout();
    }
    return false;
}

void take_mouse_report(const u8* report, u32 length) {
    MouseSample sample{};
    if (!decode_mouse_report(report, length, sample)) {
        return;
    }
    const u8 held = pointer.buttons();
    pointer.apply(sample);
    const MonitorMouse state{pointer.screen_x(kMonitorWidth), pointer.screen_y(kMonitorHeight),
                             sample.buttons, sample.wheel, clock_us()};
    const MonitorRequest request = ui.mouse(state, snapshot, view);
    handle_request(request);
    // A move with no button held changes nothing but the pointer.
    if (open && (request.op != MonitorOp::None || held != 0 || sample.buttons != 0 ||
                 sample.wheel != 0)) {
        relayout();
    }
}

bool mouse_report_passes(const u8* report, u32 length) {
    const bool passes = mouse_gate.passes(open, static_cast<u8>(report[0] & 7u));
    if (open) {
        take_mouse_report(report, length);
    }
    return passes;
}

// Only the boot processor runs, so its meter speaks for the machine; the
// others are listed from the ACPI tables but idle.
void fill_cpus(MonitorSnapshot& out) {
    const u32 boot_apic_id = this_cpu().apic_id;
    u32 count = 0;
    out.cpus[count++] = MonitorCpu{boot_apic_id, true, cpu_meter.busy_permille()};
    const AcpiProcessorList& cpus = acpi_processors();
    for (u32 i = 0; i < cpus.count && count < MonitorSnapshot::kMaxCpus; ++i) {
        if (cpus.apic_id[i] != boot_apic_id) {
            out.cpus[count++] = MonitorCpu{cpus.apic_id[i], false, 0};
        }
    }
    out.cpu_count = count;
}

void fill_task_cpu(MonitorSnapshot& out) {
    const u32 count = out.task_count < MonitorSnapshot::kMaxTasks ? out.task_count
                                                                  : MonitorSnapshot::kMaxTasks;
    for (u32 i = 0; i < count; ++i) {
        MonitorTask& task = out.tasks[i];
        task.cpu_permille = cpu_meter.task_permille(task.slot, task.task_addr);
    }
}

void log_inspect(const MonitorInspect& out) {
    LogLine line;
    line.add("monitor: inspect ");
    line.add(out.name);
    line.add(" pc=");
    line.add_hex(out.pc);
    if (out.native[0] != '\0') {
        line.add(" in ");
        line.add(out.native);
    }
    log_lines.push(line);
}

// Asks the machine how it is and lays the window out to match.
void refresh_window(u64 now, const TrapFrame& frame) {
    if (monitor_host.snapshot != nullptr) {
        monitor_host.snapshot(snapshot);
    }
    if (inspected.op == MonitorOp::Inspect && monitor_host.inspect != nullptr) {
        monitor_host.inspect(inspected.slot, inspected.task_addr, frame, snapshot.inspect);
        if (snapshot.inspect.valid && !inspect_logged) {
            inspect_logged = true;
            log_inspect(snapshot.inspect);
        }
    }
    fill_cpus(snapshot);
    fill_task_cpu(snapshot);
    snapshot.uptime_us = now;
    ui.layout(snapshot, view);
    if (now >= next_redraw_us) {
        next_redraw_us = now + kRedrawUs;
        for (bool& row : stale) {
            row = true;
        }
    }
    if (!summary_logged) {
        summary_logged = true;
        log_summary(snapshot);
    }
}

// Gives the screen the pixels first to last of a row of the window.
void show_span(u32 row, u32 first, u32 last) {
    const u8* pens = shown + row * kMonitorWidth;
    for (u32 x = first; x <= last; ++x) {
        row_colours[x] = colours[pens[x]];
    }
    screen.put_row(place.left + static_cast<u64>(first) * place.scale,
                   place.top + static_cast<u64>(row) * place.scale, row_colours + first,
                   last - first + 1, place.scale);
}

// Draws the pointer where it was last put, as far as it lies on the rows
// first to last of the window.
void draw_pointer(i32 first, i32 last) {
    const i32 scale = static_cast<i32>(place.scale);
    const i32 left = static_cast<i32>(place.left);
    const i32 top = static_cast<i32>(place.top);
    const Rect clip{left, top + first * scale,
                    left + static_cast<i32>(kMonitorWidth) * scale - 1,
                    top + (last + 1) * scale - 1};
    paint_pointer(shape, left + (drawn_x - shape.hot_x) * scale,
                  top + (drawn_y - shape.hot_y) * scale, scale, scale, clip,
                  [](i64 x, i64 y, i64 width, i64 height, u32 rgb) {
                      screen.fill_rect(static_cast<u64>(x), static_cast<u64>(y),
                                       static_cast<u64>(width), static_cast<u64>(height), rgb);
                  });
}

void paint_band(u32 index) {
    const u32 first = index * kBandRows;
    const u32 last = first + kBandRows < kMonitorHeight ? first + kBandRows - 1
                                                        : kMonitorHeight - 1;
    bitmap.set_clip(Rect{0, static_cast<i32>(first), static_cast<i32>(kMonitorWidth) - 1,
                         static_cast<i32>(last)});
    paint_monitor(bitmap.surface(), view);
    bool changed = false;
    for (u32 row = first; row <= last; ++row) {
        const usize at = static_cast<usize>(row) * kMonitorWidth;
        if (!stale[row] && __builtin_memcmp(painted + at, shown + at, kMonitorWidth) == 0) {
            continue;
        }
        stale[row] = false;
        __builtin_memcpy(shown + at, painted + at, kMonitorWidth);
        show_span(row, 0, kMonitorWidth - 1);
        changed = true;
    }
    if (changed) {
        draw_pointer(static_cast<i32>(first), static_cast<i32>(last));
    }
}

// Paints bands, from where the last tick stopped, until its time is up.
void paint_slice() {
    const u64 until = clock_us() + kSliceUs;
    while (band < kBands) {
        paint_band(band++);
        if (clock_us() >= until) {
            return;
        }
    }
}

i32 clamp_to(i32 value, i32 side) {
    return value < 0 ? 0 : value >= side ? side - 1 : value;
}

// Uncovers what the pointer was over and draws it where it is now.
void move_pointer() {
    const i32 x = pointer.screen_x(kMonitorWidth);
    const i32 y = pointer.screen_y(kMonitorHeight);
    if (x == drawn_x && y == drawn_y) {
        return;
    }
    constexpr i32 kWidth = static_cast<i32>(kMonitorWidth);
    constexpr i32 kHeight = static_cast<i32>(kMonitorHeight);
    const i32 old_left = drawn_x - shape.hot_x;
    const i32 old_top = drawn_y - shape.hot_y;
    const i32 first = clamp_to(old_left, kWidth);
    const i32 last = clamp_to(old_left + shape.width - 1, kWidth);
    const i32 bottom = clamp_to(old_top + shape.height - 1, kHeight);
    drawn_x = x;
    drawn_y = y;
    for (i32 row = clamp_to(old_top, kHeight); row <= bottom; ++row) {
        show_span(static_cast<u32>(row), static_cast<u32>(first), static_cast<u32>(last));
    }
    draw_pointer(0, kHeight - 1);
}

void run_monitor(const TrapFrame& frame) {
    if (wanted && !open) {
        open_monitor();
    }
    const u64 now = clock_us();
    if (open) {
        if (band == kBands && (repaint || now >= next_render_us)) {
            repaint = false;
            next_render_us = now + kRenderUs;
            refresh_window(now, frame);
            band = 0;
        }
        paint_slice();
        move_pointer();
    } else if (restoring && now >= restore_us) {
        restoring = false;
        if (monitor_host.polls() == polls_at_close) {
            monitor_host.refresh_screen();
        }
    }
}

void write_log() {
    if (!log_pending) {
        log_pending = log_lines.pop(log_next);
    }
    if (log_pending && serial::write_line_from_interrupt(log_next.text)) {
        log_pending = false;
    }
}

}

void monitor_set_host(const MonitorHost& host) {
    monitor_host = host;
}

void monitor_timer_tick(const TrapFrame& frame) {
    static constexpr UsbFilter filter{&take_key_report, &mouse_report_passes};
    usb_pump_from_interrupt(&filter);
    write_log();
    if (monitor_host.current_task == nullptr) {
        return;
    }
    i32 slot = -1;
    u32 task = 0;
    monitor_host.current_task(slot, task);
    cpu_meter.sample(slot, task, cpu_halted());
    run_monitor(frame);
}

}
