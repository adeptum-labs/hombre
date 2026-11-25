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

#include "apic.hpp"

namespace hombre {

namespace {

constinit MonitorHost monitor_host{};
constinit CpuMeter cpu_meter{};

}

void monitor_set_host(const MonitorHost& host) {
    monitor_host = host;
}

void monitor_timer_tick(const TrapFrame&) {
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
