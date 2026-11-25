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

#include "../common/format.hpp"
#include "../common/irq_ring.hpp"
#include "../common/monitor_hotkey.hpp"
#include "../common/monitor_ui.hpp"
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
constexpr u32 kGlyph = 8;
// A line is a glyph high with a pixel of air above and below it.
constexpr u32 kLine = kGlyph + 2;
constexpr u32 kColumnsPerScale = 640;
constexpr u32 kBackground = 0x102030;
constexpr u32 kLogLineBytes = 80;
constexpr u32 kLogLines = 64;

struct Colours {
    u32 text;
    u32 back;
};

// In the order of MonitorStyle.
constexpr Colours kStyles[] = {
    {0xD0D8E0, kBackground}, {0xFFFFFF, kBackground}, {0xFFFFFF, 0x3060A0},
    {0x8090A0, kBackground}, {0xFFD040, kBackground},
};

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

    void add_dec(u64 value) {
        char digits[21];
        if (format_u64(digits, sizeof digits, value, 10) != 0) {
            add(digits);
        }
    }
};

constinit MonitorHost monitor_host{};
constinit CpuMeter cpu_meter{};
constinit MonitorHotkey hotkey{};
constinit MonitorUi ui{};
constinit MonitorSnapshot snapshot{};
// What the screen should show and what has been drawn of it.
constinit MonitorGrid grid{};
constinit MonitorGrid shown{};
constinit Framebuffer screen{};
constinit bool wanted = false;
constinit bool open = false;
constinit bool restoring = false;
constinit u32 scale = 1;
constinit u32 next_cell = 0;
constinit u64 next_render_us = 0;
constinit u64 next_redraw_us = 0;
constinit u64 restore_us = 0;
constinit u32 polls_at_close = 0;
constinit bool summary_logged = false;
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
    scale = screen.width() >= kColumnsPerScale ? screen.width() / kColumnsPerScale : 1;
    const u32 columns = screen.width() / (kGlyph * scale);
    const u32 rows = screen.height() / (kLine * scale);
    grid.columns = columns < MonitorGrid::kMaxColumns ? columns : MonitorGrid::kMaxColumns;
    grid.rows = rows < MonitorGrid::kMaxRows ? rows : MonitorGrid::kMaxRows;
    shown.columns = grid.columns;
    shown.rows = grid.rows;
    screen.fill(kBackground);
    ui.reset();
    next_render_us = 0;
    next_redraw_us = 0;
    summary_logged = false;
    // The tasks must not go on thinking a key or button is held.
    usb_deliver_release();
    log_text("monitor: open");
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
    if (ui.key_report(report, snapshot).op == MonitorOp::Close) {
        close_monitor();
    }
    return false;
}

bool mouse_report_passes() {
    return !open;
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

void render(u64 now) {
    if (monitor_host.snapshot != nullptr) {
        monitor_host.snapshot(snapshot);
    }
    fill_cpus(snapshot);
    fill_task_cpu(snapshot);
    snapshot.uptime_us = now;
    ui.render(snapshot, grid);
    if (now >= next_redraw_us) {
        next_redraw_us = now + kRedrawUs;
        // No cell holds a NUL, so every one differs and is drawn again.
        for (MonitorCell& cell : shown.cells) {
            cell.ch = '\0';
        }
    }
    if (!summary_logged) {
        summary_logged = true;
        log_summary(snapshot);
    }
}

void draw_cell(u32 column, u32 row, const MonitorCell& cell) {
    const u32 width = kGlyph * scale;
    const u32 height = kLine * scale;
    const Colours& colours = kStyles[cell.style < 5 ? cell.style : 0];
    screen.fill_rect(column * width, row * height, width, height, colours.back);
    const char text[2] = {cell.ch, '\0'};
    screen.draw_text(column * width, row * height + scale, text, colours.text, scale);
}

// Draws changed cells, from where the last tick stopped, until its time is
// up.
void draw_slice(u64 now) {
    const u32 cells = grid.columns * grid.rows;
    const u64 until = now + kSliceUs;
    for (u32 seen = 0; seen < cells; ++seen) {
        next_cell = next_cell + 1 < cells ? next_cell + 1 : 0;
        const u32 column = next_cell % grid.columns;
        const u32 row = next_cell / grid.columns;
        const MonitorCell& want = grid.at(column, row);
        MonitorCell& have = shown.at(column, row);
        if (want.ch == have.ch && want.style == have.style) {
            continue;
        }
        draw_cell(column, row, want);
        have = want;
        if (clock_us() >= until) {
            return;
        }
    }
}

void run_monitor() {
    if (wanted && !open) {
        open_monitor();
    }
    const u64 now = clock_us();
    if (open) {
        if (now >= next_render_us) {
            next_render_us = now + kRenderUs;
            render(now);
        }
        draw_slice(now);
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

void monitor_timer_tick(const TrapFrame&) {
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
    run_monitor();
}

}
