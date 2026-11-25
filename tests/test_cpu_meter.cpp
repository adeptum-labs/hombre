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

#include "check.hpp"
#include "cpu_meter.hpp"
#include "sched.hpp"

using namespace hombre;

namespace {

void take_samples(CpuMeter& meter, u32 count, i32 slot, u32 task_addr, bool idle) {
    for (u32 i = 0; i < count; ++i) {
        meter.sample(slot, task_addr, idle);
    }
}

void empty_meter() {
    CpuMeter meter;
    CHECK_EQ(meter.busy_permille(), 0u);
    CHECK_EQ(meter.task_permille(0, 0x1000), 0u);
    CHECK_EQ(meter.task_permille(Scheduler::kMaxTasks, 0x1000), 0u);
}

void partial_window() {
    CpuMeter meter;
    take_samples(meter, 250, 0, 0x1000, false);
    take_samples(meter, 250, 1, 0x2000, true);
    CHECK_EQ(meter.busy_permille(), 500u);
    CHECK_EQ(meter.task_permille(0, 0x1000), 500u);
    CHECK_EQ(meter.task_permille(1, 0x2000), 0u);
}

void full_window_with_idle_and_two_tasks() {
    CpuMeter meter;
    take_samples(meter, 400, 0, 0x1000, false);
    take_samples(meter, 350, 1, 0x2000, false);
    take_samples(meter, 250, 0, 0x1000, true);
    CHECK_EQ(meter.busy_permille(), 750u);
    CHECK_EQ(meter.task_permille(0, 0x1000), 400u);
    CHECK_EQ(meter.task_permille(1, 0x2000), 350u);
    CHECK_EQ(meter.task_permille(0, 0x2000), 0u);
}

void published_window_stays_while_next_fills() {
    CpuMeter meter;
    take_samples(meter, 700, 0, 0x1000, false);
    take_samples(meter, 300, 0, 0x1000, true);
    CHECK_EQ(meter.busy_permille(), 700u);
    CHECK_EQ(meter.task_permille(0, 0x1000), 700u);

    take_samples(meter, 100, 0, 0x1000, true);
    CHECK_EQ(meter.busy_permille(), 700u);
    CHECK_EQ(meter.task_permille(0, 0x1000), 700u);

    take_samples(meter, 900, 0, 0x1000, false);
    CHECK_EQ(meter.busy_permille(), 900u);
    CHECK_EQ(meter.task_permille(0, 0x1000), 900u);
}

void slot_reuse_by_another_task() {
    CpuMeter meter;
    take_samples(meter, 500, 0, 0x1000, false);
    take_samples(meter, 500, 1, 0x2000, false);
    CHECK_EQ(meter.task_permille(0, 0x1000), 500u);

    // The slot now runs a new task: the old identity and both counts restart,
    // so there is no full-window figure for the new task until the next one.
    take_samples(meter, 1, 0, 0x3000, false);
    CHECK_EQ(meter.task_permille(0, 0x1000), 0u);
    CHECK_EQ(meter.task_permille(0, 0x3000), 0u);

    // The next window counts only the samples since the new task appeared.
    take_samples(meter, 999, 1, 0x2000, false);
    CHECK_EQ(meter.task_permille(0, 0x1000), 0u);
    CHECK_EQ(meter.task_permille(0, 0x3000), 1u);
    CHECK_EQ(meter.task_permille(1, 0x2000), 999u);
}

void out_of_range_slot() {
    CpuMeter meter;
    meter.sample(static_cast<i32>(Scheduler::kMaxTasks), 0x1000, false);
    meter.sample(-1, 0x1000, false);
    CHECK_EQ(meter.busy_permille(), 1000u);
    CHECK_EQ(meter.task_permille(0, 0x1000), 0u);
    CHECK_EQ(meter.task_permille(Scheduler::kMaxTasks, 0x1000), 0u);

    take_samples(meter, 2, 0, 0x1000, true);
    CHECK_EQ(meter.busy_permille(), 500u);
}

}  // namespace

int main() {
    empty_meter();
    partial_window();
    full_window_with_idle_and_two_tasks();
    published_window_stays_while_next_fills();
    slot_reuse_by_another_task();
    out_of_range_slot();
    return check::finish("test_cpu_meter");
}
