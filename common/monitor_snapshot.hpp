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

#include "sched.hpp"
#include "types.hpp"

namespace hombre {

struct MonitorCpu {
    u32 apic_id = 0;
    bool started = false;
    u32 busy_permille = 0;
};

struct MonitorTask {
    u32 slot = 0;
    u32 task_addr = 0;
    char name[32]{};
    TaskState state = TaskState::Free;
    bool suspended = false;
    i8 priority = 0;
    u32 cpu_permille = 0;
    bool forbid = false;
    bool disable = false;
};

struct MonitorInspect {
    bool valid = false;
    u32 slot = 0;
    u32 task_addr = 0;
    char name[32]{};
    bool pc_exact = false;
    u32 d[8]{};
    u32 a[8]{};
    u32 pc = 0;
    u16 sr = 0;
    char native[32]{};
    bool has_x86 = false;
    u64 rip = 0;
    u64 rsp = 0;
    u64 rflags = 0;
};

struct MonitorDrive {
    bool inserted = false;
    char volume[32]{};
};

struct MonitorEntry {
    char name[32]{};
    bool is_dir = false;
};

struct MonitorListing {
    char path[256]{};
    MonitorEntry entries[256]{};
    u32 count = 0;
    bool truncated = false;
    bool loading = false;
    char error[64]{};
};

struct MonitorSnapshot {
    static constexpr u32 kMaxCpus = 64;
    static constexpr u32 kMaxTasks = Scheduler::kMaxTasks;
    static constexpr u32 kDriveCount = 4;
    static constexpr u32 kMaxEntries = 256;

    MonitorCpu cpus[kMaxCpus]{};
    u32 cpu_count = 0;

    u32 chip_free = 0;
    u32 chip_total = 0;
    u32 fast_free = 0;
    u32 fast_total = 0;
    u32 heap_used = 0;
    u32 heap_size = 0;
    u64 physical_free = 0;
    u64 physical_total = 0;
    u32 code_used = 0;
    u32 code_total = 0;

    MonitorTask tasks[kMaxTasks]{};
    u32 task_count = 0;

    MonitorInspect inspect{};

    MonitorDrive drives[kDriveCount]{};

    MonitorListing listing{};

    char status[80]{};
    u64 uptime_us = 0;
};

}
