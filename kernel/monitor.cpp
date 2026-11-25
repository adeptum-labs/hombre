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

#include "../common/irq_ring.hpp"
#include "../common/monitor_hotkey.hpp"
#include "../common/serial.hpp"
#include "apic.hpp"
#include "usb_keyboard.hpp"

namespace hombre {

namespace {

constinit MonitorHost monitor_host{};
constinit CpuMeter cpu_meter{};
constinit MonitorHotkey hotkey{};
constinit bool open = false;
// Lines waiting for the serial port to be between lines.
constinit IrqRing<const char*, 8> log_lines{};
constinit const char* log_next = nullptr;

bool take_key_report(u8 (&report)[8]) {
    if (hotkey.take(report)) {
        open = !open;
        log_lines.push(open ? "monitor: open" : "monitor: closed");
    }
    return true;
}

void write_log() {
    if (log_next == nullptr) {
        log_lines.pop(log_next);
    }
    if (log_next != nullptr && serial::write_line_from_interrupt(log_next)) {
        log_next = nullptr;
    }
}

}

void monitor_set_host(const MonitorHost& host) {
    monitor_host = host;
}

void monitor_timer_tick(const TrapFrame&) {
    usb_pump_from_interrupt(&take_key_report);
    write_log();
    if (monitor_host.current_task == nullptr) {
        return;
    }
    i32 slot = -1;
    u32 task = 0;
    monitor_host.current_task(slot, task);
    cpu_meter.sample(slot, task, cpu_halted());
}

const CpuMeter& monitor_cpu_meter() {
    return cpu_meter;
}

}
