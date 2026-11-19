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
#include "sched.hpp"

using namespace hombre;

namespace {

void boot_task_and_empty_ready() {
    Scheduler s;
    s.init(0x1000);
    CHECK_EQ(s.current(), 0);
    CHECK_EQ(s.slot(0).task_addr, 0x1000u);
    CHECK_EQ(s.slot(0).priority, 0);
    CHECK(s.slot(0).state == TaskState::Running);
    CHECK_EQ(s.pick_next(), -1);
    CHECK(!s.should_preempt());
    CHECK(!s.should_switch_now());
    CHECK_EQ(s.active_count(), 1u);
}

void ordering_by_priority_and_fifo() {
    Scheduler s;
    s.init(0x1000);

    const i32 low = s.add(0x2000, -5);
    const i32 eq1 = s.add(0x3000, 0);
    const i32 eq2 = s.add(0x4000, 0);
    const i32 hi1 = s.add(0x5000, 10);
    const i32 hi2 = s.add(0x6000, 10);
    CHECK(low > 0 && eq1 > 0 && eq2 > 0 && hi1 > 0 && hi2 > 0);

    CHECK(s.should_preempt());
    CHECK(s.should_switch_now());
    CHECK_EQ(s.pick_next(), hi1);

    s.switch_to(hi1);
    CHECK_EQ(s.current(), hi1);
    CHECK(s.slot(0).state == TaskState::Ready);
    CHECK(s.slot(hi1).state == TaskState::Running);

    // hi2 has equal priority to running hi1: preempt at slice end, not immediately.
    CHECK(s.should_preempt());
    CHECK(!s.should_switch_now());
    CHECK_EQ(s.pick_next(), hi2);

    // Round-robin between hi1 and hi2, leaving lower priorities waiting.
    s.switch_to(hi2);
    CHECK_EQ(s.current(), hi2);
    CHECK_EQ(s.pick_next(), hi1);
    s.switch_to(hi1);
    CHECK_EQ(s.current(), hi1);
    CHECK_EQ(s.pick_next(), hi2);

    // Remove hi1 and hi2, leaving eq1, eq2, 0 (which was re-queued after eq2).
    s.remove(hi2);
    s.release(hi2);
    CHECK(!s.should_preempt());  // running hi1 (10) > all ready (0, -5)
    CHECK(!s.should_switch_now());
    s.wait(hi1);
    CHECK_EQ(s.pick_next(), eq1);
    s.switch_to(eq1);
    CHECK_EQ(s.pick_next(), eq2);
    s.switch_to(eq2);
    CHECK_EQ(s.pick_next(), 0);
    s.switch_to(0);
    CHECK_EQ(s.pick_next(), eq1);
}

void wait_and_wake_order() {
    Scheduler s;
    s.init(0x1000);
    const i32 a = s.add(0x2000, 0);
    const i32 b = s.add(0x3000, 0);
    const i32 c = s.add(0x4000, 5);

    // Switch to c, then wait c.
    s.switch_to(c);
    CHECK_EQ(s.current(), c);
    s.wait(c);
    CHECK(s.slot(c).state == TaskState::Waiting);

    // Next is a (added before b, and 0 was re-queued behind b when switched out).
    CHECK_EQ(s.pick_next(), a);
    s.switch_to(a);
    s.wait(a);

    // Now b and 0 are Ready: b was queued before 0.
    CHECK_EQ(s.pick_next(), b);
    s.switch_to(b);
    CHECK_EQ(s.pick_next(), 0);

    // Wake a (pri 0): goes behind 0 in pri 0 queue; switching to 0 puts b behind a.
    s.wake(a);
    CHECK(s.slot(a).state == TaskState::Ready);
    CHECK_EQ(s.pick_next(), 0);
    s.switch_to(0);
    CHECK_EQ(s.pick_next(), a);
    s.switch_to(a);
    CHECK_EQ(s.pick_next(), b);

    // Wake c (pri 5 > running 0): should_switch_now becomes true.
    s.wake(c);
    CHECK(s.should_switch_now());
    CHECK_EQ(s.pick_next(), c);
}

void remove_ready_waiting_and_running() {
    Scheduler s;
    s.init(0x1000);
    const i32 rdy = s.add(0x2000, 1);
    const i32 wtg = s.add(0x3000, 2);
    const i32 run = s.add(0x4000, 3);

    s.switch_to(wtg);
    s.wait(wtg);
    s.switch_to(run);
    CHECK_EQ(s.current(), run);

    // Remove Ready task.
    s.remove(rdy);
    CHECK(s.slot(rdy).state == TaskState::Removed);
    CHECK_EQ(s.find(0x2000), -1);

    // Remove Waiting task.
    s.remove(wtg);
    CHECK(s.slot(wtg).state == TaskState::Removed);
    CHECK_EQ(s.find(0x3000), -1);

    // Remove Running task and switch away to boot task 0.
    s.remove(run);
    CHECK(s.slot(run).state == TaskState::Removed);
    CHECK_EQ(s.find(0x4000), -1);
    CHECK_EQ(s.pick_next(), 0);
    s.switch_to(0);
    CHECK_EQ(s.current(), 0);
    CHECK(s.slot(run).state == TaskState::Removed);

    s.release(rdy);
    s.release(wtg);
    s.release(run);
    CHECK(s.slot(rdy).state == TaskState::Free);
    CHECK(s.slot(wtg).state == TaskState::Free);
    CHECK(s.slot(run).state == TaskState::Free);
    CHECK_EQ(s.active_count(), 1u);
}

void full_table_and_reuse() {
    Scheduler s;
    s.init(0x1000);
    i32 ids[Scheduler::kMaxTasks]{};
    ids[0] = 0;
    for (u32 i = 1; i < Scheduler::kMaxTasks; ++i) {
        ids[i] = s.add(0x10000 + i * 0x100, 0);
        CHECK_EQ(ids[i], static_cast<i32>(i));
    }
    CHECK_EQ(s.add(0x99999, 0), -1);
    CHECK_EQ(s.active_count(), Scheduler::kMaxTasks);

    // Removed slot stays unavailable until released.
    s.remove(5);
    CHECK_EQ(s.add(0x99999, 0), -1);
    s.release(5);
    CHECK_EQ(s.add(0x99999, 0), 5);
    CHECK_EQ(s.find(0x99999), 5);
}

void set_priority_requeues() {
    Scheduler s;
    s.init(0x1000);
    const i32 a = s.add(0x2000, 4);
    const i32 b = s.add(0x3000, 4);

    // Both at priority 4: a is ahead of b.
    CHECK_EQ(s.pick_next(), a);

    // Re-queueing a at the same priority moves it behind b.
    CHECK_EQ(s.set_priority(a, 4), 4);
    CHECK_EQ(s.pick_next(), b);

    // Lowering b below a puts a first again.
    CHECK_EQ(s.set_priority(b, 2), 4);
    CHECK_EQ(s.pick_next(), a);

    // b (pri 2) is still above running 0 (pri 0).
    s.set_priority(a, -1);
    CHECK(s.should_switch_now());
    s.switch_to(b);  // b (2) is running, 0 (0) and a (-1) are ready
    CHECK(!s.should_switch_now());
    CHECK_EQ(s.set_priority(a, 5), -1);
    CHECK(s.should_switch_now());
    CHECK_EQ(s.pick_next(), a);
}

void find_across_states() {
    Scheduler s;
    s.init(0x1000);
    const i32 t1 = s.add(0x2000, 0);
    const i32 t2 = s.add(0x3000, 0);

    CHECK_EQ(s.find(0x1000), 0);
    CHECK_EQ(s.find(0x2000), t1);
    CHECK_EQ(s.find(0x3000), t2);
    CHECK_EQ(s.find(0x4000), -1);

    s.switch_to(t2);
    s.wait(t2);
    CHECK_EQ(s.find(0x3000), t2);

    s.remove(t1);
    CHECK_EQ(s.find(0x2000), -1);

    s.set_task_addr(0, 0x1111);
    CHECK_EQ(s.find(0x1000), -1);
    CHECK_EQ(s.find(0x1111), 0);
}

}  // namespace

int main() {
    boot_task_and_empty_ready();
    ordering_by_priority_and_fifo();
    wait_and_wake_order();
    remove_ready_waiting_and_running();
    full_table_and_reuse();
    set_priority_requeues();
    find_across_states();
    return check::finish("test_sched");
}
