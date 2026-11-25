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

// Runs in the timer interrupt, so it keeps plain counters only and takes no
// locks; readers run in the same interrupt and see a consistent window.
class CpuMeter {
public:
    static constexpr u32 kWindowSamples = 1000;

    // idle is true when the processor was halted; otherwise the task in slot
    // ran. A slot outside the scheduler's range is busy with no task.
    void sample(i32 slot, u32 task_addr, bool idle) {
        if (!idle) {
            ++running_busy_;
            if (valid_slot(slot)) {
                TaskCounts& counts = slots_[static_cast<u32>(slot)];
                if (counts.task_addr != task_addr) {
                    counts = TaskCounts{task_addr, 0, 0};
                }
                ++counts.running;
            }
        }
        if (++running_samples_ == kWindowSamples) {
            publish();
        }
    }

    u32 busy_permille() const {
        return has_published_ ? permille(published_busy_, kWindowSamples)
                              : permille(running_busy_, running_samples_);
    }

    u32 task_permille(u32 slot, u32 task_addr) const {
        if (slot >= Scheduler::kMaxTasks) {
            return 0;
        }
        const TaskCounts& counts = slots_[slot];
        if (counts.task_addr != task_addr) {
            return 0;
        }
        return has_published_ ? permille(counts.published, kWindowSamples)
                              : permille(counts.running, running_samples_);
    }

private:
    struct TaskCounts {
        u32 task_addr = 0;
        u32 running = 0;
        u32 published = 0;
    };

    static bool valid_slot(i32 slot) {
        return slot >= 0 && static_cast<u32>(slot) < Scheduler::kMaxTasks;
    }

    static u32 permille(u32 part, u32 total) {
        return total == 0 ? 0 : part * 1000 / total;
    }

    void publish() {
        published_busy_ = running_busy_;
        for (TaskCounts& counts : slots_) {
            counts.published = counts.running;
            counts.running = 0;
        }
        has_published_ = true;
        running_busy_ = 0;
        running_samples_ = 0;
    }

    TaskCounts slots_[Scheduler::kMaxTasks]{};
    u32 running_busy_ = 0;
    u32 running_samples_ = 0;
    u32 published_busy_ = 0;
    bool has_published_ = false;
};

}
