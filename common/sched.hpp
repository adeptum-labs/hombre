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

#include "types.hpp"

namespace hombre {

enum class TaskState : u8 {
    Free,
    Ready,
    Running,
    Waiting,
    Removed,
};

// Fixed-capacity priority scheduler with FIFO ordering per priority.
// Holds no pointers to guest memory so it can be unit tested on the host.
class Scheduler {
public:
    static constexpr u32 kMaxTasks = 32;

    struct Slot {
        u32 task_addr = 0;
        i8 priority = 0;
        TaskState state = TaskState::Free;
        u64 seq = 0;
    };

    // Resets the table and places the boot task in slot 0 as Running with
    // priority 0.
    void init(u32 boot_task_addr = 0) {
        for (Slot& s : slots_) {
            s = Slot{};
        }
        slots_[0] = Slot{boot_task_addr, 0, TaskState::Running, 0};
        current_ = 0;
        next_seq_ = 1;
    }

    // Adds a task in the Ready state, or returns -1 when all slots are taken.
    i32 add(u32 task_addr, i8 pri) {
        for (u32 i = 0; i < kMaxTasks; ++i) {
            if (slots_[i].state == TaskState::Free) {
                slots_[i] = Slot{task_addr, pri, TaskState::Ready, next_seq_++};
                return static_cast<i32>(i);
            }
        }
        return -1;
    }

    // Marks an active slot Removed. The slot stays occupied until release() is
    // called after switching away from its stack.
    void remove(i32 id) {
        if (!valid_id(id)) {
            return;
        }
        Slot& s = slots_[id];
        if (s.state == TaskState::Free || s.state == TaskState::Removed) {
            return;
        }
        s.state = TaskState::Removed;
    }

    // Returns a Removed slot to Free so a later add() can reuse it.
    void release(i32 id) {
        if (!valid_id(id)) {
            return;
        }
        if (slots_[id].state == TaskState::Removed) {
            slots_[id] = Slot{};
        }
    }

    // Transitions a Running task to Waiting.
    void wait(i32 id) {
        if (!valid_id(id)) {
            return;
        }
        if (slots_[id].state == TaskState::Running) {
            slots_[id].state = TaskState::Waiting;
        }
    }

    // Transitions a Waiting task to Ready at the back of its priority's queue.
    void wake(i32 id) {
        if (!valid_id(id)) {
            return;
        }
        if (slots_[id].state == TaskState::Waiting) {
            slots_[id].state = TaskState::Ready;
            slots_[id].seq = next_seq_++;
        }
    }

    // Updates priority, re-queueing at the back of the new priority if Ready,
    // and returns the previous priority.
    i8 set_priority(i32 id, i8 pri) {
        if (!valid_id(id)) {
            return 0;
        }
        Slot& s = slots_[id];
        if (s.state == TaskState::Free || s.state == TaskState::Removed) {
            return 0;
        }
        const i8 old = s.priority;
        s.priority = pri;
        if (s.state == TaskState::Ready) {
            s.seq = next_seq_++;
        }
        return old;
    }

    void set_task_addr(i32 id, u32 task_addr) {
        if (valid_id(id) && slots_[id].state != TaskState::Free &&
            slots_[id].state != TaskState::Removed) {
            slots_[id].task_addr = task_addr;
        }
    }

    // Returns the highest-priority Ready task (FIFO within a priority), or -1
    // when no task is Ready.
    i32 pick_next() const {
        i32 best = -1;
        for (u32 i = 0; i < kMaxTasks; ++i) {
            const Slot& s = slots_[i];
            if (s.state != TaskState::Ready) {
                continue;
            }
            if (best < 0 || s.priority > slots_[best].priority ||
                (s.priority == slots_[best].priority && s.seq < slots_[best].seq)) {
                best = static_cast<i32>(i);
            }
        }
        return best;
    }

    // Switches the Running slot to next_id. If the previous current task is
    // still Running, it becomes Ready at the back of its priority's queue;
    // Waiting and Removed tasks keep their state.
    void switch_to(i32 next_id) {
        if (!valid_id(next_id) || slots_[next_id].state != TaskState::Ready) {
            return;
        }
        if (valid_id(current_) && slots_[current_].state == TaskState::Running) {
            slots_[current_].state = TaskState::Ready;
            slots_[current_].seq = next_seq_++;
        }
        slots_[next_id].state = TaskState::Running;
        current_ = next_id;
    }

    i32 current() const { return current_; }

    i32 find(u32 task_addr) const {
        for (u32 i = 0; i < kMaxTasks; ++i) {
            const TaskState st = slots_[i].state;
            if ((st == TaskState::Ready || st == TaskState::Running || st == TaskState::Waiting) &&
                slots_[i].task_addr == task_addr) {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }

    // True at time-slice end when a Ready task has priority >= the running one.
    bool should_preempt() const {
        if (!valid_id(current_) || slots_[current_].state != TaskState::Running) {
            return pick_next() >= 0;
        }
        const i8 cur_pri = slots_[current_].priority;
        for (u32 i = 0; i < kMaxTasks; ++i) {
            if (slots_[i].state == TaskState::Ready && slots_[i].priority >= cur_pri) {
                return true;
            }
        }
        return false;
    }

    // True when a Ready task has strictly higher priority than the running one.
    bool should_switch_now() const {
        if (!valid_id(current_) || slots_[current_].state != TaskState::Running) {
            return pick_next() >= 0;
        }
        const i8 cur_pri = slots_[current_].priority;
        for (u32 i = 0; i < kMaxTasks; ++i) {
            if (slots_[i].state == TaskState::Ready && slots_[i].priority > cur_pri) {
                return true;
            }
        }
        return false;
    }

    bool any_ready() const { return pick_next() >= 0; }

    u32 active_count() const {
        u32 n = 0;
        for (const Slot& s : slots_) {
            if (s.state == TaskState::Ready || s.state == TaskState::Running ||
                s.state == TaskState::Waiting) {
                ++n;
            }
        }
        return n;
    }

    const Slot& slot(u32 id) const { return slots_[id]; }

private:
    static bool valid_id(i32 id) { return id >= 0 && static_cast<u32>(id) < kMaxTasks; }

    Slot slots_[kMaxTasks]{};
    i32 current_ = -1;
    u64 next_seq_ = 1;
};

}
