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

#include "bitmap_surface.hpp"
#include "check.hpp"
#include "libs/chrome_palette.hpp"
#include "monitor_paint.hpp"
#include "monitor_snapshot.hpp"
#include "monitor_ui.hpp"
#include "monitor_view.hpp"

#include <array>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace hombre;

namespace {

constexpr u8 kEnter = 0x28;
constexpr u8 kEscape = 0x29;
constexpr u8 kTab = 0x2B;
constexpr u8 kDown = 0x51;

std::array<u8, 8> report(u8 usage) {
    std::array<u8, 8> bytes{};
    bytes[2] = usage;
    return bytes;
}

MonitorRequest key_press(MonitorUi& ui, const MonitorSnapshot& snapshot, u8 usage) {
    const std::array<u8, 8> down = report(usage);
    const MonitorRequest request = ui.key_report(down.data(), snapshot);
    const std::array<u8, 8> released{};
    ui.key_report(released.data(), snapshot);
    return request;
}

template <usize N>
void set_text(char (&destination)[N], const char* text) {
    usize i = 0;
    for (; i + 1 < N && text[i] != '\0'; ++i) {
        destination[i] = text[i];
    }
    destination[i] = '\0';
}

MonitorSnapshot sample_snapshot() {
    MonitorSnapshot snapshot;
    snapshot.uptime_us = 761000000;
    snapshot.cpu_count = 4;
    snapshot.cpus[0] = MonitorCpu{0, true, 684};
    snapshot.cpus[1] = MonitorCpu{1, false, 0};
    snapshot.cpus[2] = MonitorCpu{2, false, 0};
    snapshot.cpus[3] = MonitorCpu{3, false, 0};

    snapshot.chip_free = 12u << 20;
    snapshot.chip_total = 64u << 20;
    snapshot.fast_free = 40u << 20;
    snapshot.fast_total = 512u << 20;
    snapshot.heap_used = 3u << 20;
    snapshot.heap_size = 8u << 20;
    snapshot.physical_free = 2254857830ull;
    snapshot.physical_total = 8ull << 30;
    snapshot.code_used = 4096;
    snapshot.code_total = 65536;

    snapshot.task_count = 2;
    snapshot.tasks[0] =
        MonitorTask{0, 0x1000, "Clock", TaskState::Running, false, 0, 630, false, false};
    snapshot.tasks[1] =
        MonitorTask{1, 0x2000, "Workbench", TaskState::Waiting, false, 1, 4, false, false};

    snapshot.drives[0] = MonitorDrive{true, "Workbench3.1"};
    snapshot.drives[1] = MonitorDrive{true, "Extras3.1"};

    set_text(snapshot.status, "monitor ready");
    return snapshot;
}

void set_entries(MonitorSnapshot& snapshot, const char* path,
                 std::initializer_list<std::pair<const char*, bool>> entries) {
    set_text(snapshot.listing.path, path);
    snapshot.listing.count = 0;
    for (const auto& entry : entries) {
        MonitorEntry& out = snapshot.listing.entries[snapshot.listing.count++];
        set_text(out.name, entry.first);
        out.is_dir = entry.second;
    }
    snapshot.listing.loading = false;
    snapshot.listing.error[0] = '\0';
}

struct Rig {
    MonitorSnapshot snapshot = sample_snapshot();
    MonitorView view;
    MonitorUi ui;

    Rig() { ui.reset(); }

    void layout() { ui.layout(snapshot, view); }

    const MonitorPart& part(MonitorAction action, u32 index = 0) {
        layout();
        const MonitorPart* found = view.find(action, index);
        CHECK(found != nullptr);
        return *found;
    }

    i32 centre_x(MonitorAction action, u32 index = 0) {
        const MonitorPart& found = part(action, index);
        return (found.box.x0 + found.box.x1) / 2;
    }

    i32 centre_y(MonitorAction action, u32 index = 0) {
        const MonitorPart& found = part(action, index);
        return (found.box.y0 + found.box.y1) / 2;
    }
};

MonitorRequest click_point(Rig& rig, i32 x, i32 y, u64 now_us) {
    const MonitorRequest pressed =
        rig.ui.mouse(MonitorMouse{x, y, 1, 0, now_us}, rig.snapshot, rig.view);
    const MonitorRequest released =
        rig.ui.mouse(MonitorMouse{x, y, 0, 0, now_us}, rig.snapshot, rig.view);
    return pressed.op != MonitorOp::None ? pressed : released;
}

MonitorRequest click(Rig& rig, MonitorAction action, u32 index, u64 now_us) {
    const i32 x = rig.centre_x(action, index);
    const i32 y = rig.centre_y(action, index);
    return click_point(rig, x, y, now_us);
}

MonitorRequest double_click(Rig& rig, MonitorAction action, u32 index, u64 now_us) {
    const MonitorRequest first = click(rig, action, index, now_us);
    CHECK(first.op == MonitorOp::None);
    return click(rig, action, index, now_us + 1000);
}

void check_parts_in_window(const MonitorView& view) {
    CHECK(view.count > 0);
    CHECK(view.count <= MonitorView::kMaxParts);
    CHECK(view.parts[0].kind == MonitorPartKind::Window);
    const Rect& window = view.parts[0].box;
    for (u32 i = 0; i < view.count; ++i) {
        const MonitorPart& part = view.parts[i];
        CHECK(part.box.x0 <= part.box.x1);
        CHECK(part.box.y0 <= part.box.y1);
        CHECK(part.box.x0 >= window.x0);
        CHECK(part.box.y0 >= window.y0);
        CHECK(part.box.x1 <= window.x1);
        CHECK(part.box.y1 <= window.y1);
        CHECK(part.box.x0 >= 0);
        CHECK(part.box.y0 >= 0);
        CHECK(part.box.x1 < static_cast<i32>(kMonitorWidth));
        CHECK(part.box.y1 < static_cast<i32>(kMonitorHeight));
    }
}

bool overlaps(const Rect& a, const Rect& b) {
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

void check_actions_do_not_overlap(const MonitorView& view) {
    u32 dialog_at = view.count;
    for (u32 i = 0; i < view.count; ++i) {
        if (view.parts[i].kind == MonitorPartKind::Dialog) {
            dialog_at = i;
            break;
        }
    }
    for (u32 i = 0; i < view.count; ++i) {
        for (u32 j = i + 1; j < view.count; ++j) {
            if (view.parts[i].action == MonitorAction::None ||
                view.parts[j].action == MonitorAction::None) {
                continue;
            }
            if (i > dialog_at || j > dialog_at) {
                continue;
            }
            CHECK(!overlaps(view.parts[i].box, view.parts[j].box));
        }
    }
}

void parts_are_inside_the_window_and_actionable_boxes_do_not_overlap() {
    Rig rig;
    set_entries(rig.snapshot, "DH0:",
                {{"Disks", true}, {"Games", true}, {"Readme", false}});
    rig.layout();
    check_parts_in_window(rig.view);
    check_actions_do_not_overlap(rig.view);

    CHECK(key_press(rig.ui, rig.snapshot, kTab).op == MonitorOp::None);
    CHECK(key_press(rig.ui, rig.snapshot, kEnter).op == MonitorOp::ListDirectory);
    rig.layout();
    check_parts_in_window(rig.view);
    check_actions_do_not_overlap(rig.view);
    CHECK(rig.view.find(MonitorAction::Open, 0) != nullptr);
    CHECK(rig.view.find(MonitorAction::Up, 0) != nullptr);
    CHECK(rig.view.find(MonitorAction::Cancel, 0) != nullptr);

    CHECK(key_press(rig.ui, rig.snapshot, kEscape).op == MonitorOp::None);
    CHECK(key_press(rig.ui, rig.snapshot, kTab).op == MonitorOp::None);
    CHECK(key_press(rig.ui, rig.snapshot, kEnter).op == MonitorOp::Inspect);
    rig.layout();
    check_parts_in_window(rig.view);
    check_actions_do_not_overlap(rig.view);
    CHECK(rig.view.find(MonitorAction::Back, 0) != nullptr);
}

void list_shows_tasks_with_state_priority_and_cpu() {
    Rig rig;
    rig.snapshot.tasks[1].forbid = true;
    rig.snapshot.tasks[1].disable = true;
    rig.layout();

    const MonitorPart* clock = rig.view.find(MonitorAction::SelectTask, 0);
    const MonitorPart* workbench = rig.view.find(MonitorAction::SelectTask, 1);
    CHECK(clock != nullptr);
    CHECK(workbench != nullptr);
    CHECK(clock->selected);
    CHECK(!workbench->selected);
    CHECK(!clock->warning);
    CHECK(workbench->warning);
    const std::string first(clock->text);
    const std::string second(workbench->text);
    CHECK(first.find("Clock") != std::string::npos);
    CHECK(first.find("run") != std::string::npos);
    CHECK(first.find("63.0") != std::string::npos);
    CHECK(first.find("  0") != std::string::npos);
    CHECK(second.find("Workbench") != std::string::npos);
    CHECK(second.find("wait") != std::string::npos);
    CHECK(second.find("0.4") != std::string::npos);
    CHECK(second.find('F') != std::string::npos);
    CHECK(second.find('D') != std::string::npos);

    bool found_header = false;
    for (u32 i = 0; i < rig.view.count; ++i) {
        const MonitorPart& part = rig.view.parts[i];
        if (part.kind == MonitorPartKind::Text &&
            std::string(part.text).find("state  pri  cpu%") != std::string::npos) {
            found_header = true;
        }
    }
    CHECK(found_header);
}

void click_selects_a_task_and_inspect_button_requests_it() {
    Rig rig;
    rig.snapshot.tasks[2] =
        MonitorTask{2, 0x3000, "Shell", TaskState::Ready, false, -1, 120, false, false};
    rig.snapshot.task_count = 3;
    CHECK(click(rig, MonitorAction::SelectTask, 1, 0).op == MonitorOp::None);
    rig.layout();
    const MonitorPart* first = rig.view.find(MonitorAction::SelectTask, 0);
    const MonitorPart* second = rig.view.find(MonitorAction::SelectTask, 1);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK(!first->selected);
    CHECK(second->selected);

    const MonitorRequest request = click(rig, MonitorAction::Inspect, 0, 1000);
    CHECK(request.op == MonitorOp::Inspect);
    CHECK_EQ(request.slot, 1u);
    CHECK_EQ(request.task_addr, 0x2000u);
}

void double_click_within_half_a_second_inspects() {
    Rig rig;
    CHECK(click(rig, MonitorAction::SelectTask, 0, 1000).op == MonitorOp::None);
    const MonitorRequest request = click(rig, MonitorAction::SelectTask, 0, 501000);
    CHECK(request.op == MonitorOp::Inspect);
    CHECK_EQ(request.task_addr, 0x1000u);

    Rig apart;
    CHECK(click(apart, MonitorAction::SelectTask, 0, 1000).op == MonitorOp::None);
    const MonitorRequest none = click(apart, MonitorAction::SelectTask, 0, 501001);
    CHECK(none.op == MonitorOp::None);
}

void a_click_elsewhere_breaks_a_double_click() {
    Rig rig;
    CHECK(click(rig, MonitorAction::SelectTask, 0, 1000).op == MonitorOp::None);
    CHECK(click_point(rig, 2, 2, 2000).op == MonitorOp::None);
    CHECK(click(rig, MonitorAction::SelectTask, 0, 3000).op == MonitorOp::None);

    Rig button;
    CHECK(click(button, MonitorAction::SelectTask, 0, 1000).op == MonitorOp::None);
    CHECK(click(button, MonitorAction::Reboot, 0, 2000).op == MonitorOp::None);
    CHECK(click(button, MonitorAction::Dismiss, 0, 3000).op == MonitorOp::None);
    CHECK(click(button, MonitorAction::SelectTask, 0, 4000).op == MonitorOp::None);
}

void a_task_that_moved_under_the_pointer_is_not_inspected() {
    Rig rig;
    CHECK(click(rig, MonitorAction::SelectTask, 0, 1000).op == MonitorOp::None);
    rig.snapshot.tasks[0] =
        MonitorTask{2, 0x3000, "Shell", TaskState::Ready, false, 0, 10, false, false};
    CHECK(click(rig, MonitorAction::SelectTask, 0, 2000).op == MonitorOp::None);
    const MonitorRequest request = click(rig, MonitorAction::SelectTask, 0, 3000);
    CHECK(request.op == MonitorOp::Inspect);
    CHECK_EQ(request.task_addr, 0x3000u);
}

void button_released_outside_does_nothing() {
    Rig rig;
    const i32 x = rig.centre_x(MonitorAction::Inspect);
    const i32 y = rig.centre_y(MonitorAction::Inspect);
    CHECK(rig.ui.mouse(MonitorMouse{x, y, 1, 0, 0}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Inspect, 0)->pressed);
    CHECK(rig.ui.mouse(MonitorMouse{2, 2, 1, 0, 1}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.layout();
    CHECK(!rig.view.find(MonitorAction::Inspect, 0)->pressed);
    CHECK(rig.ui.mouse(MonitorMouse{2, 2, 0, 0, 2}, rig.snapshot, rig.view).op ==
          MonitorOp::None);

    rig.layout();
    const i32 x2 = rig.centre_x(MonitorAction::Inspect);
    const i32 y2 = rig.centre_y(MonitorAction::Inspect);
    CHECK(rig.ui.mouse(MonitorMouse{2, 2, 1, 0, 3}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    CHECK(rig.ui.mouse(MonitorMouse{x2, y2, 0, 0, 4}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
}

void suspend_and_end_and_reboot_open_a_dialog() {
    Rig rig;
    rig.layout();
    CHECK(click(rig, MonitorAction::Suspend, 0, 0).op == MonitorOp::None);
    rig.layout();
    const MonitorPart* dialog = nullptr;
    for (u32 i = 0; i < rig.view.count; ++i) {
        if (rig.view.parts[i].kind == MonitorPartKind::Dialog) {
            dialog = &rig.view.parts[i];
        }
    }
    CHECK(dialog != nullptr);
    CHECK(std::string(dialog->text).find("Confirm suspend of Clock") != std::string::npos);

    MonitorRequest request = click(rig, MonitorAction::Confirm, 0, 1000);
    CHECK(request.op == MonitorOp::Suspend);
    CHECK_EQ(request.task_addr, 0x1000u);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) == nullptr);

    CHECK(click(rig, MonitorAction::End, 0, 2000).op == MonitorOp::None);
    request = click(rig, MonitorAction::Confirm, 0, 3000);
    CHECK(request.op == MonitorOp::End);
    CHECK_EQ(request.task_addr, 0x1000u);

    CHECK(click(rig, MonitorAction::Reboot, 0, 4000).op == MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) != nullptr);
    CHECK(click_point(rig, 2, 2, 5000).op == MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) != nullptr);
    request = click(rig, MonitorAction::Confirm, 0, 6000);
    CHECK(request.op == MonitorOp::Reboot);

    CHECK(click(rig, MonitorAction::Reboot, 0, 7000).op == MonitorOp::None);
    CHECK(click(rig, MonitorAction::Dismiss, 0, 8000).op == MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) == nullptr);
}

void eject_df0_asks_eject_df1_does_not() {
    Rig rig;
    rig.layout();
    CHECK(!rig.view.find(MonitorAction::Eject, 0)->disabled);
    CHECK(click(rig, MonitorAction::Eject, 0, 0).op == MonitorOp::None);
    rig.layout();
    bool asks = false;
    for (u32 i = 0; i < rig.view.count; ++i) {
        const MonitorPart& part = rig.view.parts[i];
        if (part.kind == MonitorPartKind::Dialog &&
            std::string(part.text).find("Confirm eject of DF0") != std::string::npos) {
            asks = true;
        }
    }
    CHECK(asks);
    MonitorRequest request = click(rig, MonitorAction::Confirm, 0, 1000);
    CHECK(request.op == MonitorOp::Eject);
    CHECK_EQ(request.drive, 0u);

    CHECK(click(rig, MonitorAction::SelectDrive, 1, 2000).op == MonitorOp::None);
    request = click(rig, MonitorAction::Eject, 0, 3000);
    CHECK(request.op == MonitorOp::Eject);
    CHECK_EQ(request.drive, 1u);

    CHECK(click(rig, MonitorAction::SelectDrive, 2, 4000).op == MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Eject, 0)->disabled);
    CHECK(click(rig, MonitorAction::Eject, 0, 5000).op == MonitorOp::None);
}

void drive_double_click_and_insert_button_list_partitions() {
    Rig rig;
    CHECK(click(rig, MonitorAction::SelectDrive, 1, 1000).op == MonitorOp::None);
    const MonitorRequest insert = click(rig, MonitorAction::Insert, 0, 2000);
    CHECK(insert.op == MonitorOp::ListDirectory);
    CHECK_EQ(insert.drive, 1u);
    CHECK_EQ(insert.path[0], '\0');

    CHECK(click(rig, MonitorAction::Cancel, 0, 3000).op == MonitorOp::None);
    const MonitorRequest request =
        double_click(rig, MonitorAction::SelectDrive, 1, 1000000);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK_EQ(request.drive, 1u);
    CHECK_EQ(request.path[0], '\0');
}

void browser_double_click_opens_drawer_or_inserts() {
    Rig rig;
    CHECK(double_click(rig, MonitorAction::SelectDrive, 0, 1000).op ==
          MonitorOp::ListDirectory);
    set_entries(rig.snapshot, "", {{"DH0:", true}, {"DH1:", true}});
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Open, 0) != nullptr);

    MonitorRequest request = double_click(rig, MonitorAction::SelectEntry, 0, 2000);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK(std::string(request.path) == "DH0:");

    set_entries(rig.snapshot, "DH0:", {{"Disks", true}, {"Readme", false}});
    rig.layout();
    request = double_click(rig, MonitorAction::SelectEntry, 0, 3000);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK(std::string(request.path) == "DH0:Disks");

    request = click(rig, MonitorAction::Up, 0, 4000);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK(std::string(request.path) == "DH0:");
    request = click(rig, MonitorAction::Up, 0, 5000);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK_EQ(request.path[0], '\0');
    CHECK(click(rig, MonitorAction::Up, 0, 6000).op == MonitorOp::None);

    CHECK(click(rig, MonitorAction::Cancel, 0, 7000).op == MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::SelectTask, 0) != nullptr);

    CHECK(click(rig, MonitorAction::Insert, 0, 8000).op == MonitorOp::ListDirectory);
    set_entries(rig.snapshot, "", {{"Readme", false}});
    rig.layout();
    request = double_click(rig, MonitorAction::SelectEntry, 0, 9000);
    CHECK(request.op == MonitorOp::Insert);
    CHECK_EQ(request.drive, 0u);
    CHECK(std::string(request.path) == "Readme");
}

void wheel_scrolls_without_moving_the_selection() {
    Rig rig;
    rig.snapshot.task_count = MonitorSnapshot::kMaxTasks;
    for (u32 i = 0; i < MonitorSnapshot::kMaxTasks; ++i) {
        MonitorTask& task = rig.snapshot.tasks[i];
        task.slot = i;
        task.task_addr = 0x1000 + i * 0x100;
        set_text(task.name, i % 2 == 0 ? "Clock" : "Workbench");
        task.state = TaskState::Ready;
        task.priority = static_cast<i8>(i % 4);
        task.cpu_permille = i * 10;
    }
    rig.layout();
    CHECK(rig.view.find(MonitorAction::SelectTask, 0) != nullptr);
    CHECK(rig.view.find(MonitorAction::Scroll, 0) != nullptr);

    CHECK(rig.ui.mouse(MonitorMouse{300, 200, 0, -1, 0}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::SelectTask, 0) == nullptr);
    const MonitorPart* first_visible = rig.view.find(MonitorAction::SelectTask, 3);
    CHECK(first_visible != nullptr);
    CHECK(!first_visible->selected);

    CHECK(key_press(rig.ui, rig.snapshot, kDown).op == MonitorOp::None);
    rig.layout();
    const MonitorPart* selected = rig.view.find(MonitorAction::SelectTask, 1);
    CHECK(selected != nullptr);
    CHECK(selected->selected);
    CHECK(rig.view.find(MonitorAction::SelectTask, 0) == nullptr);

    CHECK(rig.ui.mouse(MonitorMouse{300, 200, 0, 1, 1}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::SelectTask, 0) != nullptr);
    CHECK(!rig.view.find(MonitorAction::SelectTask, 0)->selected);
    CHECK(rig.view.find(MonitorAction::SelectTask, 1)->selected);
}

void scroller_knob_drag_and_track_page() {
    Rig rig;
    rig.snapshot.task_count = MonitorSnapshot::kMaxTasks;
    for (u32 i = 0; i < MonitorSnapshot::kMaxTasks; ++i) {
        MonitorTask& task = rig.snapshot.tasks[i];
        task.slot = i;
        task.task_addr = 0x1000 + i * 0x100;
        set_text(task.name, "Task");
        task.state = TaskState::Ready;
    }
    rig.layout();
    const MonitorPart* scroll = rig.view.find(MonitorAction::Scroll, 0);
    CHECK(scroll != nullptr);
    const Rect track = scroll->box;
    const Rect knob = scroll->knob;
    CHECK(knob.y1 - knob.y0 + 1 < track.y1 - track.y0 + 1);
    CHECK_EQ(knob.y0, track.y0);

    const i32 x = (track.x0 + track.x1) / 2;
    CHECK(rig.ui.mouse(MonitorMouse{x, knob.y0 + 1, 1, 0, 0}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    CHECK(rig.ui.mouse(MonitorMouse{x, track.y1, 1, 0, 1}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    CHECK(rig.ui.mouse(MonitorMouse{x, track.y1, 0, 0, 2}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.layout();
    scroll = rig.view.find(MonitorAction::Scroll, 0);
    CHECK(scroll != nullptr);
    CHECK(scroll->knob.y0 > knob.y0);
    CHECK(scroll->knob.y1 <= track.y1);

    CHECK(rig.ui.mouse(MonitorMouse{x, track.y0 + 1, 1, 0, 3}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    CHECK(rig.ui.mouse(MonitorMouse{x, track.y0 + 1, 0, 0, 4}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.layout();
    scroll = rig.view.find(MonitorAction::Scroll, 0);
    CHECK(scroll != nullptr);
    CHECK_EQ(scroll->knob.y0, track.y0);

    CHECK(rig.ui.mouse(MonitorMouse{x, track.y1 - 1, 1, 0, 5}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    CHECK(rig.ui.mouse(MonitorMouse{x, track.y1 - 1, 0, 0, 6}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.layout();
    scroll = rig.view.find(MonitorAction::Scroll, 0);
    CHECK(scroll != nullptr);
    CHECK(scroll->knob.y0 > track.y0);
}

void close_gadget_requests_close() {
    Rig rig;
    const MonitorRequest request = click(rig, MonitorAction::Close, 0, 0);
    CHECK(request.op == MonitorOp::Close);
}

void buttons_held_at_start_cause_no_click() {
    Rig rig;
    const i32 x = rig.centre_x(MonitorAction::Inspect);
    const i32 y = rig.centre_y(MonitorAction::Inspect);
    rig.ui.start_mouse(x, y, 1);
    CHECK(rig.ui.mouse(MonitorMouse{x, y, 0, 0, 0}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) == nullptr);
}

void row_gone_before_release_acts_on_nothing() {
    Rig rig;
    rig.snapshot.tasks[2] =
        MonitorTask{2, 0x3000, "Shell", TaskState::Ready, false, -1, 120, false, false};
    rig.snapshot.task_count = 3;
    CHECK(click(rig, MonitorAction::SelectTask, 1, 0).op == MonitorOp::None);
    rig.layout();
    const i32 x = rig.centre_x(MonitorAction::Inspect);
    const i32 y = rig.centre_y(MonitorAction::Inspect);
    CHECK(rig.ui.mouse(MonitorMouse{x, y, 1, 0, 1000}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
    rig.snapshot.task_count = 0;
    rig.layout();
    CHECK(rig.ui.mouse(MonitorMouse{x, y, 0, 0, 2000}, rig.snapshot, rig.view).op ==
          MonitorOp::None);
}

void listing_arriving_between_press_and_release_inserts_nothing() {
    Rig rig;
    rig.snapshot.listing.loading = true;
    rig.snapshot.listing.count = 0;
    CHECK(key_press(rig.ui, rig.snapshot, kTab).op == MonitorOp::None);
    CHECK(key_press(rig.ui, rig.snapshot, kEnter).op == MonitorOp::ListDirectory);
    rig.layout();
    const MonitorPart* open = rig.view.find(MonitorAction::Open, 0);
    CHECK(open != nullptr);
    CHECK(open->disabled);
    const i32 open_x = (open->box.x0 + open->box.x1) / 2;
    const i32 open_y = (open->box.y0 + open->box.y1) / 2;
    CHECK(rig.ui.mouse(MonitorMouse{open_x, open_y, 1, 0, 0}, rig.snapshot, rig.view).op ==
          MonitorOp::None);

    set_entries(rig.snapshot, "", {{"Readme", false}});
    rig.layout();
    CHECK(!rig.view.find(MonitorAction::Open, 0)->disabled);
    CHECK(rig.ui.mouse(MonitorMouse{open_x, open_y, 0, 0, 1000}, rig.snapshot, rig.view).op ==
          MonitorOp::None);

    Rig rows;
    rows.snapshot.listing.loading = true;
    rows.snapshot.listing.count = 0;
    CHECK(key_press(rows.ui, rows.snapshot, kTab).op == MonitorOp::None);
    CHECK(key_press(rows.ui, rows.snapshot, kEnter).op == MonitorOp::ListDirectory);
    rows.layout();
    const MonitorPart* scroll = rows.view.find(MonitorAction::Scroll, 0);
    CHECK(scroll != nullptr);
    const i32 row_x = scroll->box.x0 - 30;
    const i32 row_y = scroll->box.y0 + 5;
    CHECK(rows.ui.mouse(MonitorMouse{row_x, row_y, 1, 0, 0}, rows.snapshot, rows.view).op ==
          MonitorOp::None);
    set_entries(rows.snapshot, "", {{"Readme", false}});
    rows.layout();
    const MonitorPart* arrived = rows.view.find(MonitorAction::SelectEntry, 0);
    CHECK(arrived != nullptr);
    CHECK(row_x >= arrived->box.x0 && row_x <= arrived->box.x1 && row_y >= arrived->box.y0 &&
          row_y <= arrived->box.y1);
    CHECK(rows.ui.mouse(MonitorMouse{row_x, row_y, 0, 0, 1000}, rows.snapshot, rows.view).op ==
          MonitorOp::None);
}

void keys_still_confirm_with_y_and_cancel_the_dialog() {
    Rig rig;
    CHECK(click(rig, MonitorAction::Suspend, 0, 0).op == MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) != nullptr);
    const MonitorRequest request = key_press(rig.ui, rig.snapshot, 0x1C);
    CHECK(request.op == MonitorOp::Suspend);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) == nullptr);

    CHECK(click(rig, MonitorAction::Reboot, 0, 1000).op == MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) != nullptr);
    CHECK(key_press(rig.ui, rig.snapshot, 0x11).op == MonitorOp::None);
    rig.layout();
    CHECK(rig.view.find(MonitorAction::Confirm, 0) == nullptr);
}

void odd_input_stays_in_bounds() {
    {
        Rig rig;
        rig.snapshot.task_count = 0;
        rig.layout();
        check_parts_in_window(rig.view);
        CHECK(rig.view.count <= MonitorView::kMaxParts);
    }
    {
        Rig rig;
        rig.snapshot.task_count = MonitorSnapshot::kMaxTasks;
        for (u32 i = 0; i < MonitorSnapshot::kMaxTasks; ++i) {
            MonitorTask& task = rig.snapshot.tasks[i];
            task.slot = i;
            task.task_addr = 0x1000 + i;
            for (u32 c = 0; c < 31; ++c) {
                task.name[c] = static_cast<char>('A' + i % 26);
            }
            task.name[31] = '\0';
            task.suspended = i % 2 == 0;
        }
        rig.snapshot.cpu_count = MonitorSnapshot::kMaxCpus;
        for (u32 i = 0; i < MonitorSnapshot::kMaxCpus; ++i) {
            rig.snapshot.cpus[i] = MonitorCpu{i, i % 3 != 0, i * 20};
        }
        rig.layout();
        check_parts_in_window(rig.view);
        CHECK(rig.view.count <= MonitorView::kMaxParts);
        CHECK(rig.ui.mouse(MonitorMouse{-1000, -1000, 1, 0, 0}, rig.snapshot, rig.view).op ==
              MonitorOp::None);
        CHECK(rig.ui.mouse(MonitorMouse{-1000, -1000, 0, 0, 1}, rig.snapshot, rig.view).op ==
              MonitorOp::None);
        CHECK(rig.ui.mouse(MonitorMouse{100000, 100000, 1, 0, 2}, rig.snapshot, rig.view).op ==
              MonitorOp::None);
        CHECK(rig.ui.mouse(MonitorMouse{100000, 100000, 0, 0, 3}, rig.snapshot, rig.view).op ==
              MonitorOp::None);
    }
    {
        Rig rig;
        CHECK(key_press(rig.ui, rig.snapshot, kTab).op == MonitorOp::None);
        CHECK(key_press(rig.ui, rig.snapshot, kEnter).op == MonitorOp::ListDirectory);
        for (u32 c = 0; c < 255; ++c) {
            rig.snapshot.listing.path[c] = static_cast<char>('a' + c % 26);
        }
        rig.snapshot.listing.path[255] = '\0';
        rig.snapshot.listing.count = MonitorSnapshot::kMaxEntries;
        for (u32 i = 0; i < MonitorSnapshot::kMaxEntries; ++i) {
            MonitorEntry& entry = rig.snapshot.listing.entries[i];
            for (u32 c = 0; c < 31; ++c) {
                entry.name[c] = static_cast<char>('a' + i % 26);
            }
            entry.name[31] = '\0';
            entry.is_dir = i % 2 == 0;
        }
        rig.snapshot.listing.loading = false;
        rig.layout();
        check_parts_in_window(rig.view);
        CHECK(rig.view.count <= MonitorView::kMaxParts);
    }
}

void placement_scales_and_centres() {
    MonitorPlacement placement = monitor_placement(640, 400);
    CHECK_EQ(placement.scale, 1u);
    CHECK_EQ(placement.left, 0u);
    CHECK_EQ(placement.top, 0u);

    placement = monitor_placement(1920, 1080);
    CHECK_EQ(placement.scale, 2u);
    CHECK_EQ(placement.left, 320u);
    CHECK_EQ(placement.top, 140u);

    placement = monitor_placement(3840, 2160);
    CHECK_EQ(placement.scale, 5u);
    CHECK_EQ(placement.left, 320u);
    CHECK_EQ(placement.top, 80u);

    placement = monitor_placement(320, 200);
    CHECK_EQ(placement.scale, 1u);
    CHECK_EQ(placement.left, 0u);
    CHECK_EQ(placement.top, 0u);
}

void paint_marks_selected_row_and_dialog() {
    Rig rig;
    rig.layout();
    std::vector<u8> pens(kMonitorWidth * kMonitorHeight, 0xAB);
    BitmapSurface bitmap;
    bitmap.init(pens.data());
    paint_monitor(bitmap.surface(), rig.view);
    CHECK_EQ(pens[0], 0);

    const Rect window = rig.view.parts[0].box;
    const u8 title_pen = pens[(window.y0 + 5) * kMonitorWidth + window.x0 + 250];
    CHECK(title_pen >= chrome::kActiveBar0 && title_pen <= chrome::kActiveBar7);

    const MonitorPart* row = rig.view.find(MonitorAction::SelectTask, 0);
    CHECK(row != nullptr);
    CHECK(row->selected);
    CHECK_EQ(pens[(row->box.y0 + 1) * kMonitorWidth + row->box.x1 - 1],
             chrome::kActiveBar4);

    CHECK(click(rig, MonitorAction::Reboot, 0, 0).op == MonitorOp::None);
    rig.layout();
    const MonitorPart* dialog = nullptr;
    for (u32 i = 0; i < rig.view.count; ++i) {
        if (rig.view.parts[i].kind == MonitorPartKind::Dialog) {
            dialog = &rig.view.parts[i];
        }
    }
    CHECK(dialog != nullptr);
    std::vector<u8> dialog_pens(kMonitorWidth * kMonitorHeight, 0xAB);
    BitmapSurface dialog_bitmap;
    dialog_bitmap.init(dialog_pens.data());
    paint_monitor(dialog_bitmap.surface(), rig.view);
    CHECK_EQ(dialog_pens[(dialog->box.y1 - 5) * kMonitorWidth + dialog->box.x0 + 5],
             chrome::kInactiveBar0);
    CHECK_EQ(dialog_pens[(dialog->box.y0 + 1) * kMonitorWidth + dialog->box.x0 + 1],
             chrome::kFrameLight);
}

}

int main() {
    parts_are_inside_the_window_and_actionable_boxes_do_not_overlap();
    list_shows_tasks_with_state_priority_and_cpu();
    click_selects_a_task_and_inspect_button_requests_it();
    double_click_within_half_a_second_inspects();
    a_click_elsewhere_breaks_a_double_click();
    a_task_that_moved_under_the_pointer_is_not_inspected();
    button_released_outside_does_nothing();
    suspend_and_end_and_reboot_open_a_dialog();
    eject_df0_asks_eject_df1_does_not();
    drive_double_click_and_insert_button_list_partitions();
    browser_double_click_opens_drawer_or_inserts();
    wheel_scrolls_without_moving_the_selection();
    scroller_knob_drag_and_track_page();
    close_gadget_requests_close();
    buttons_held_at_start_cause_no_click();
    row_gone_before_release_acts_on_nothing();
    listing_arriving_between_press_and_release_inserts_nothing();
    keys_still_confirm_with_y_and_cancel_the_dialog();
    odd_input_stays_in_bounds();
    placement_scales_and_centres();
    paint_marks_selected_row_and_dialog();
    return check::finish("test_monitor_view");
}
