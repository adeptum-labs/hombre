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

#include "../common/cpu_meter.hpp"
#include "../common/types.hpp"
#include "trap_frame.hpp"

// The system monitor's side of the timer interrupt. All that is called from
// there only reads what the interrupted code keeps.

namespace hombre {

struct MonitorHost {
    // The scheduler slot that runs and its task.
    void (*current_task)(i32& slot, u32& task_addr) = nullptr;
};

void monitor_set_host(const MonitorHost& host);

// Called on every tick of the timer.
void monitor_timer_tick(const TrapFrame& frame);

const CpuMeter& monitor_cpu_meter();

}
