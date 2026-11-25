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
#include "monitor_snapshot.hpp"
#include "monitor_ui.hpp"

#include <array>
#include <initializer_list>
#include <string>
#include <utility>

using namespace hombre;

namespace {

constexpr u8 kEnter = 0x28;
constexpr u8 kEscape = 0x29;
constexpr u8 kBackspace = 0x2A;
constexpr u8 kTab = 0x2B;
constexpr u8 kRight = 0x4F;
constexpr u8 kLeft = 0x50;
constexpr u8 kDown = 0x51;
constexpr u8 kE = 0x08;
constexpr u8 kK = 0x0E;
constexpr u8 kN = 0x11;
constexpr u8 kR = 0x15;
constexpr u8 kS = 0x16;
constexpr u8 kY = 0x1C;

std::array<u8, 8> report(u8 usage) {
    std::array<u8, 8> bytes{};
    bytes[2] = usage;
    return bytes;
}

MonitorRequest press(MonitorUi& ui, const MonitorSnapshot& snapshot, u8 usage) {
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

std::string trimmed_text(const char* text) {
    std::string trimmed(text);
    while (!trimmed.empty() && trimmed.back() == ' ') {
        trimmed.pop_back();
    }
    return trimmed;
}

u32 parts_with_text(const MonitorView& view, const char* text) {
    const std::string wanted = trimmed_text(text);
    u32 found = 0;
    for (u32 i = 0; i < view.count; ++i) {
        if (trimmed_text(view.parts[i].text) == wanted) {
            ++found;
        }
    }
    return found;
}

const MonitorPart* find_kind(const MonitorView& view, MonitorPartKind kind) {
    for (u32 i = 0; i < view.count; ++i) {
        if (view.parts[i].kind == kind) {
            return &view.parts[i];
        }
    }
    return nullptr;
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

void main_window_shows_cpu_memory_lists_and_drives() {
    const MonitorSnapshot snapshot = sample_snapshot();
    MonitorView view;
    MonitorUi ui;
    ui.reset();
    ui.layout(snapshot, view);

    CHECK_EQ(parts_with_text(view, "System Monitor"), 1u);
    CHECK_EQ(parts_with_text(view, "up 00:12:41"), 1u);
    CHECK_EQ(parts_with_text(view, "CPU"), 1u);
    CHECK_EQ(parts_with_text(view, "MEM"), 1u);
    CHECK_EQ(parts_with_text(view, "chip 12/64 MiB   fast 40/512 MiB"), 1u);
    CHECK_EQ(parts_with_text(view, "kernel 3 MiB/8 MiB     free 2.1 GiB     code 4/64 KiB"), 1u);
    CHECK_EQ(parts_with_text(view, "TASKS                   state  pri  cpu%"), 1u);

    const MonitorPart* gauge = find_kind(view, MonitorPartKind::Gauge);
    CHECK(gauge != nullptr);
    CHECK_EQ(gauge->permille, 684u);
    CHECK_EQ(parts_with_text(view, "68%"), 1u);
    CHECK_EQ(parts_with_text(view, "off"), 3u);

    const MonitorPart* clock = view.find(MonitorAction::SelectTask, 0);
    const MonitorPart* workbench = view.find(MonitorAction::SelectTask, 1);
    CHECK(clock != nullptr);
    CHECK(workbench != nullptr);
    CHECK(clock->selected);
    CHECK(!workbench->selected);
    CHECK(trimmed_text(clock->text) == "  Clock                 run      0  63.0");
    CHECK(trimmed_text(workbench->text) == "  Workbench             wait     1   0.4");
    CHECK(view.find(MonitorAction::SelectTask, 2) == nullptr);

    const MonitorPart* df0 = view.find(MonitorAction::SelectDrive, 0);
    const MonitorPart* df1 = view.find(MonitorAction::SelectDrive, 1);
    const MonitorPart* df2 = view.find(MonitorAction::SelectDrive, 2);
    const MonitorPart* df3 = view.find(MonitorAction::SelectDrive, 3);
    CHECK(df0 != nullptr);
    CHECK(df1 != nullptr);
    CHECK(df2 != nullptr);
    CHECK(df3 != nullptr);
    CHECK(df0->selected);
    CHECK(std::string(df0->text) == "DF0 Workbench3.1");
    CHECK(std::string(df1->text) == "DF1 Extras3.1");
    CHECK(std::string(df2->text) == "DF2 (empty)");
    CHECK(std::string(df3->text) == "DF3 (empty)");

    CHECK_EQ(parts_with_text(view, "monitor ready"), 1u);
}

void many_cores_are_summarised() {
    MonitorSnapshot snapshot = sample_snapshot();
    snapshot.cpu_count = MonitorSnapshot::kMaxCpus;
    for (u32 i = 0; i < snapshot.cpu_count; ++i) {
        snapshot.cpus[i] = MonitorCpu{i, true, 500};
    }
    MonitorView view;
    MonitorUi ui;
    ui.reset();
    ui.layout(snapshot, view);

    CHECK_EQ(parts_with_text(view, "+53"), 1u);
}

// Every core is named, and nothing in the processor group comes close enough
// to its neighbour to read as one word.
void cores_are_numbered_and_apart() {
    MonitorSnapshot snapshot = sample_snapshot();
    snapshot.cpu_count = 12;
    for (u32 i = 0; i < snapshot.cpu_count; ++i) {
        snapshot.cpus[i] = MonitorCpu{100 + i, i % 2 == 0, 1000};
    }
    MonitorView view;
    MonitorUi ui;
    ui.reset();
    ui.layout(snapshot, view);

    CHECK_EQ(parts_with_text(view, "100"), 1u);
    CHECK_EQ(parts_with_text(view, "111"), 1u);
    const MonitorPart* group = find_kind(view, MonitorPartKind::Group);
    CHECK(group != nullptr);
    for (u32 i = 0; i < view.count; ++i) {
        const MonitorPart& a = view.parts[i];
        if (a.box.y0 <= group->box.y0 || a.box.y1 >= group->box.y1) {
            continue;
        }
        CHECK(a.box.x0 >= group->box.x0 + 4);
        CHECK(a.box.x1 <= group->box.x1 - 4);
        const i32 width = static_cast<i32>(std::string(a.text).size()) * 8;
        CHECK(width <= a.box.x1 - a.box.x0 + 1);
        for (u32 j = 0; j < view.count; ++j) {
            const MonitorPart& b = view.parts[j];
            if (i != j && b.box.y0 == a.box.y0 && b.box.x0 >= a.box.x0) {
                CHECK(b.box.x0 > a.box.x1 + 1);
            }
        }
    }
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

void held_keys_act_once() {
    MonitorSnapshot snapshot = sample_snapshot();
    snapshot.tasks[2] =
        MonitorTask{2, 0x3000, "Shell", TaskState::Ready, false, -1, 120, true, false};
    snapshot.task_count = 3;
    MonitorView view;
    MonitorUi ui;
    ui.reset();
    ui.layout(snapshot, view);

    const std::array<u8, 8> down = report(kDown);
    CHECK(ui.key_report(down.data(), snapshot).op == MonitorOp::None);
    CHECK(ui.key_report(down.data(), snapshot).op == MonitorOp::None);

    std::array<u8, 8> enter_while_down{};
    enter_while_down[2] = kDown;
    enter_while_down[3] = kEnter;
    const MonitorRequest request = ui.key_report(enter_while_down.data(), snapshot);
    CHECK(request.op == MonitorOp::Inspect);
    CHECK_EQ(request.slot, 1u);
    CHECK_EQ(request.task_addr, 0x2000u);
}

void requests_carry_their_fields() {
    MonitorSnapshot snapshot = sample_snapshot();
    MonitorUi ui;
    ui.reset();

    MonitorRequest request = press(ui, snapshot, kEscape);
    CHECK(request.op == MonitorOp::Close);

    request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::Inspect);
    CHECK_EQ(request.slot, 0u);
    CHECK_EQ(request.task_addr, 0x1000u);
    CHECK(press(ui, snapshot, kEscape).op == MonitorOp::None);

    CHECK(press(ui, snapshot, kS).op == MonitorOp::None);
    request = press(ui, snapshot, kY);
    CHECK(request.op == MonitorOp::Suspend);
    CHECK_EQ(request.slot, 0u);
    CHECK_EQ(request.task_addr, 0x1000u);

    snapshot.tasks[0].suspended = true;
    request = press(ui, snapshot, kS);
    CHECK(request.op == MonitorOp::Resume);
    CHECK_EQ(request.slot, 0u);
    CHECK_EQ(request.task_addr, 0x1000u);

    snapshot.tasks[0].suspended = false;
    CHECK(press(ui, snapshot, kK).op == MonitorOp::None);
    request = press(ui, snapshot, kY);
    CHECK(request.op == MonitorOp::End);
    CHECK_EQ(request.slot, 0u);
    CHECK_EQ(request.task_addr, 0x1000u);

    CHECK(press(ui, snapshot, kR).op == MonitorOp::None);
    request = press(ui, snapshot, kY);
    CHECK(request.op == MonitorOp::Reboot);

    CHECK(press(ui, snapshot, kTab).op == MonitorOp::None);
    request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK_EQ(request.drive, 0u);
    CHECK_EQ(request.path[0], '\0');
    CHECK(press(ui, snapshot, kEscape).op == MonitorOp::None);

    CHECK(press(ui, snapshot, kRight).op == MonitorOp::None);
    request = press(ui, snapshot, kE);
    CHECK(request.op == MonitorOp::Eject);
    CHECK_EQ(request.drive, 1u);

    CHECK(press(ui, snapshot, kLeft).op == MonitorOp::None);
    CHECK(press(ui, snapshot, kE).op == MonitorOp::None);
    request = press(ui, snapshot, kY);
    CHECK(request.op == MonitorOp::Eject);
    CHECK_EQ(request.drive, 0u);
}

void confirmations_are_accepted_and_refused() {
    MonitorSnapshot snapshot = sample_snapshot();
    MonitorView view;
    MonitorUi ui;
    ui.reset();
    ui.layout(snapshot, view);

    CHECK(press(ui, snapshot, kS).op == MonitorOp::None);
    ui.layout(snapshot, view);
    const MonitorPart* dialog = find_kind(view, MonitorPartKind::Dialog);
    CHECK(dialog != nullptr);
    CHECK(std::string(dialog->text) == "Confirm suspend of Clock");
    CHECK(press(ui, snapshot, kN).op == MonitorOp::None);
    ui.layout(snapshot, view);
    CHECK(find_kind(view, MonitorPartKind::Dialog) == nullptr);
    CHECK(press(ui, snapshot, kY).op == MonitorOp::None);

    CHECK(press(ui, snapshot, kK).op == MonitorOp::None);
    CHECK(press(ui, snapshot, kDown).op == MonitorOp::None);
    ui.layout(snapshot, view);
    CHECK(view.find(MonitorAction::SelectTask, 0)->selected);
    CHECK(press(ui, snapshot, kY).op == MonitorOp::None);

    CHECK(press(ui, snapshot, kR).op == MonitorOp::None);
    CHECK(press(ui, snapshot, kY).op == MonitorOp::Reboot);

    CHECK(press(ui, snapshot, kS).op == MonitorOp::None);
    snapshot.task_count = 0;
    ui.layout(snapshot, view);
    CHECK(find_kind(view, MonitorPartKind::Dialog) == nullptr);
}

void inspect_view_shows_registers_and_waiting() {
    MonitorSnapshot snapshot = sample_snapshot();
    MonitorView view;
    MonitorUi ui;
    ui.reset();
    ui.layout(snapshot, view);

    CHECK(press(ui, snapshot, kEnter).op == MonitorOp::Inspect);
    ui.layout(snapshot, view);
    CHECK_EQ(parts_with_text(view, "INSPECT  Clock"), 1u);
    CHECK_EQ(parts_with_text(view, "waiting for registers"), 1u);

    snapshot.inspect.valid = true;
    snapshot.inspect.slot = 0;
    snapshot.inspect.task_addr = 0x1000;
    set_text(snapshot.inspect.name, "Clock");
    snapshot.inspect.pc_exact = true;
    snapshot.inspect.d[0] = 0x12345678;
    snapshot.inspect.a[7] = 0x0000ABCD;
    snapshot.inspect.pc = 0x2000;
    snapshot.inspect.sr = 0x0010;
    set_text(snapshot.inspect.native, "dos.library/Open");
    snapshot.inspect.has_x86 = true;
    snapshot.inspect.rip = 0x1122334455667788ull;
    snapshot.inspect.rsp = 0x99AABBCCDDEEFF00ull;
    snapshot.inspect.rflags = 0x202;
    ui.layout(snapshot, view);
    CHECK_EQ(parts_with_text(view, "D0 12345678  D1 00000000  D2 00000000  D3 00000000"), 1u);
    CHECK_EQ(parts_with_text(view, "A0 00000000  A1 00000000  A2 00000000  A3 00000000"), 1u);
    CHECK_EQ(parts_with_text(view, "A4 00000000  A5 00000000  A6 00000000  A7 0000abcd"), 1u);
    CHECK_EQ(parts_with_text(view, "PC 00002000  SR 0010"), 1u);
    CHECK_EQ(parts_with_text(view, "Native: dos.library/Open"), 1u);
    CHECK_EQ(parts_with_text(view, "x86 RIP 1122334455667788  RSP 99aabbccddeeff00"), 1u);
    CHECK_EQ(parts_with_text(view, "    RFLAGS 0000000000000202"), 1u);

    snapshot.task_count = 0;
    ui.layout(snapshot, view);
    CHECK_EQ(parts_with_text(view, "TASKS                   state  pri  cpu%"), 1u);
    CHECK(view.find(MonitorAction::SelectTask, 0) == nullptr);
}

void selection_follows_task_and_clamps() {
    MonitorSnapshot snapshot = sample_snapshot();
    snapshot.tasks[2] =
        MonitorTask{2, 0x3000, "Shell", TaskState::Ready, false, -1, 120, true, false};
    snapshot.task_count = 3;
    MonitorView view;
    MonitorUi ui;
    ui.reset();
    ui.layout(snapshot, view);

    CHECK(press(ui, snapshot, kDown).op == MonitorOp::None);
    const MonitorTask clock = snapshot.tasks[0];
    const MonitorTask workbench = snapshot.tasks[1];
    const MonitorTask shell = snapshot.tasks[2];
    snapshot.tasks[0] = shell;
    snapshot.tasks[1] = clock;
    snapshot.tasks[2] = workbench;

    MonitorRequest request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::Inspect);
    CHECK_EQ(request.slot, 1u);
    CHECK_EQ(request.task_addr, 0x2000u);
    CHECK(press(ui, snapshot, kEscape).op == MonitorOp::None);

    snapshot.tasks[0] = clock;
    snapshot.task_count = 1;
    request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::Inspect);
    CHECK_EQ(request.slot, 0u);
    CHECK_EQ(request.task_addr, 0x1000u);
}

void browsing_into_and_out_of_directories() {
    MonitorSnapshot snapshot = sample_snapshot();
    MonitorView view;
    MonitorUi ui;
    ui.reset();

    CHECK(press(ui, snapshot, kTab).op == MonitorOp::None);
    MonitorRequest request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK_EQ(request.drive, 0u);
    CHECK_EQ(request.path[0], '\0');

    set_entries(snapshot, "", {{"DH0:", true}, {"DH1:", true}});
    ui.layout(snapshot, view);
    CHECK_EQ(parts_with_text(view, "BROWSE  partitions"), 1u);
    const MonitorPart* first = view.find(MonitorAction::SelectEntry, 0);
    const MonitorPart* second = view.find(MonitorAction::SelectEntry, 1);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK(std::string(first->text) == "  DH0:");
    CHECK(first->selected);
    CHECK(std::string(second->text) == "  DH1:");
    CHECK(!second->selected);
    request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK(std::string(request.path) == "DH0:");

    set_entries(snapshot, "DH0:", {{"Disks", true}, {"Games", true}, {"Readme", false}});
    request = press(ui, snapshot, kEnter);
    CHECK(std::string(request.path) == "DH0:Disks");

    set_entries(snapshot, "DH0:Disks", {{"Games", true}});
    request = press(ui, snapshot, kEnter);
    CHECK(std::string(request.path) == "DH0:Disks/Games");

    request = press(ui, snapshot, kBackspace);
    CHECK(std::string(request.path) == "DH0:Disks");
    request = press(ui, snapshot, kBackspace);
    CHECK(std::string(request.path) == "DH0:");
    request = press(ui, snapshot, kBackspace);
    CHECK_EQ(request.path[0], '\0');
    CHECK(press(ui, snapshot, kBackspace).op == MonitorOp::None);

    set_entries(snapshot, "", {{"DH0:", true}, {"DH1:", true}});
    request = press(ui, snapshot, kEnter);
    CHECK(std::string(request.path) == "DH0:");
    set_entries(snapshot, "DH0:", {{"Readme", false}});
    request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::Insert);
    CHECK_EQ(request.drive, 0u);
    CHECK(std::string(request.path) == "DH0:Readme");

    request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK_EQ(request.path[0], '\0');
    CHECK(press(ui, snapshot, kEscape).op == MonitorOp::None);

    ui.layout(snapshot, view);
    CHECK_EQ(parts_with_text(view, "TASKS                   state  pri  cpu%"), 1u);
}

}

int main() {
    main_window_shows_cpu_memory_lists_and_drives();
    many_cores_are_summarised();
    cores_are_numbered_and_apart();
    held_keys_act_once();
    requests_carry_their_fields();
    confirmations_are_accepted_and_refused();
    inspect_view_shows_registers_and_waiting();
    selection_follows_task_and_clamps();
    browsing_into_and_out_of_directories();
    return check::finish("test_monitor_ui");
}
