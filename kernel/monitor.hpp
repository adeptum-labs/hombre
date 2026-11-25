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
#include "../common/monitor_snapshot.hpp"
#include "../common/monitor_ui.hpp"
#include "../common/types.hpp"
#include "framebuffer.hpp"
#include "trap_frame.hpp"

// The system monitor's side of the timer interrupt. All that is called from
// there only reads what the interrupted code keeps.

namespace hombre {

struct MonitorHost {
    // The scheduler slot that runs and its task.
    void (*current_task)(i32& slot, u32& task_addr) = nullptr;
    // Covers the Amiga display and gives the framebuffer to draw on; false
    // when it cannot be had now.
    bool (*take_screen)(Framebuffer& out) = nullptr;
    // Uncovers the display, for the tasks to draw again.
    void (*give_screen)() = nullptr;
    // Draws the front screen from the interrupt, unless a draw is under way.
    void (*refresh_screen)() = nullptr;
    // Counts the polls of the tasks, which show that they run.
    u32 (*polls)() = nullptr;
    // Fills in what the host knows of the machine.
    void (*snapshot)(MonitorSnapshot& out) = nullptr;
    // The registers of the task in the scheduler slot, if that is still
    // task_addr; frame is what the tick interrupted.
    void (*inspect)(u32 slot, u32 task_addr, const TrapFrame& frame,
                    MonitorInspect& out) = nullptr;
    // Posts a request to suspend, resume or end a task, carried out in
    // normal context.
    void (*rescue)(const MonitorRequest& request) = nullptr;
};

void monitor_set_host(const MonitorHost& host);

// Called on every tick of the timer.
void monitor_timer_tick(const TrapFrame& frame);

}
