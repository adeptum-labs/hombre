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
#include <vector>

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

std::string row_text(const MonitorGrid& grid, u32 row) {
    std::string text;
    for (u32 column = 0; column < grid.columns; ++column) {
        text.push_back(grid.at(column, row).ch);
    }
    return text;
}

std::string row_trimmed(const MonitorGrid& grid, u32 row) {
    std::string text = row_text(grid, row);
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
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

void layout_at_80x25() {
    const MonitorSnapshot snapshot = sample_snapshot();
    MonitorGrid grid;
    grid.columns = 80;
    grid.rows = 25;
    MonitorUi ui;
    ui.reset();
    ui.render(snapshot, grid);

    CHECK(row_trimmed(grid, 0) == "HOMBRE MONITOR        up 00:12:41     F12 close");
    CHECK(row_trimmed(grid, 1) == "CPU  0 #######... 68%   1 off   2 off   3 off");
    CHECK(row_trimmed(grid, 2) == "MEM  chip 12/64 MiB   fast 40/512 MiB");
    CHECK(row_trimmed(grid, 3) == "     kernel 3 MiB/8 MiB     free 2.1 GiB     code 4/64 KiB");
    CHECK(row_trimmed(grid, 4) == "TASKS                   state  pri  cpu%");
    CHECK(row_trimmed(grid, 5) == "> Clock                 run      0  63.0");
    CHECK(row_trimmed(grid, 6) == "  Workbench             wait     1   0.4");
    for (u32 row = 7; row < 21; ++row) {
        CHECK(row_trimmed(grid, row).empty());
    }
    CHECK(row_trimmed(grid, 21) == "DF0 Workbench3.1   DF1 Extras3.1");
    CHECK(row_trimmed(grid, 22) == "DF2 (empty)        DF3 (empty)");
    CHECK(row_trimmed(grid, 23) == "monitor ready");
    CHECK(row_trimmed(grid, 24) ==
          "Tab drives  Up/Down select  Enter inspect  S suspend  K end  R reboot");

    CHECK(grid.at(0, 0).style == monitor_style(MonitorStyle::Title));
    CHECK(grid.at(0, 5).style == monitor_style(MonitorStyle::Selected));
    CHECK(grid.at(0, 21).style == monitor_style(MonitorStyle::Selected));
    CHECK_EQ(row_text(grid, 0).size(), 80u);
}

void many_cores_are_summarised() {
    MonitorSnapshot snapshot = sample_snapshot();
    snapshot.cpu_count = MonitorSnapshot::kMaxCpus;
    for (u32 i = 0; i < snapshot.cpu_count; ++i) {
        snapshot.cpus[i] = MonitorCpu{i, true, 500};
    }
    MonitorGrid grid;
    grid.columns = 80;
    grid.rows = 25;
    MonitorUi ui;
    ui.reset();
    ui.render(snapshot, grid);

    const std::string line = row_trimmed(grid, 1);
    CHECK(line.find('+') != std::string::npos);
    CHECK(line.size() <= 80u);
}

void tiny_grids_stay_in_bounds() {
    const MonitorSnapshot snapshot = sample_snapshot();
    constexpr u32 kSizes[][2] = {{1, 1}, {5, 3}, {20, 8}, {40, 12}, {80, 10}};
    for (const auto& size : kSizes) {
        MonitorGrid grid;
        grid.columns = size[0];
        grid.rows = size[1];
        MonitorUi ui;
        ui.reset();
        ui.render(snapshot, grid);
        CHECK_EQ(grid.columns, size[0]);
        CHECK_EQ(grid.rows, size[1]);
        for (u32 row = 0; row < grid.rows; ++row) {
            for (u32 column = 0; column < grid.columns; ++column) {
                CHECK(grid.at(column, row).ch != '\0');
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
    MonitorGrid grid;
    grid.columns = 80;
    grid.rows = 25;
    MonitorUi ui;
    ui.reset();
    ui.render(snapshot, grid);

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
    MonitorGrid grid;
    grid.columns = 80;
    grid.rows = 25;
    MonitorUi ui;
    ui.reset();
    ui.render(snapshot, grid);

    CHECK(press(ui, snapshot, kS).op == MonitorOp::None);
    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 23).find("Confirm suspend of Clock") != std::string::npos);
    CHECK(press(ui, snapshot, kN).op == MonitorOp::None);
    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 23).find("Confirm") == std::string::npos);
    CHECK(press(ui, snapshot, kY).op == MonitorOp::None);

    CHECK(press(ui, snapshot, kK).op == MonitorOp::None);
    CHECK(press(ui, snapshot, kDown).op == MonitorOp::None);
    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 5) == "> Clock                 run      0  63.0");
    CHECK(press(ui, snapshot, kY).op == MonitorOp::None);

    CHECK(press(ui, snapshot, kR).op == MonitorOp::None);
    CHECK(press(ui, snapshot, kY).op == MonitorOp::Reboot);

    CHECK(press(ui, snapshot, kS).op == MonitorOp::None);
    snapshot.task_count = 0;
    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 23).find("Confirm") == std::string::npos);
}

void inspect_view_shows_registers_and_waiting() {
    MonitorSnapshot snapshot = sample_snapshot();
    MonitorGrid grid;
    grid.columns = 80;
    grid.rows = 25;
    MonitorUi ui;
    ui.reset();
    ui.render(snapshot, grid);

    CHECK(press(ui, snapshot, kEnter).op == MonitorOp::Inspect);
    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 4) == "INSPECT  Clock");
    CHECK(row_trimmed(grid, 5) == "waiting for registers");

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
    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 5) == "D0 12345678  D1 00000000  D2 00000000  D3 00000000");
    CHECK(row_trimmed(grid, 7) == "A0 00000000  A1 00000000  A2 00000000  A3 00000000");
    CHECK(row_trimmed(grid, 8) == "A4 00000000  A5 00000000  A6 00000000  A7 0000abcd");
    CHECK(row_trimmed(grid, 9) == "PC 00002000  SR 0010");
    CHECK(row_trimmed(grid, 10) == "Native: dos.library/Open");
    CHECK(row_trimmed(grid, 11) == "x86 RIP 1122334455667788  RSP 99aabbccddeeff00");
    CHECK(row_trimmed(grid, 12) == "    RFLAGS 0000000000000202");

    snapshot.task_count = 0;
    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 4) == "TASKS                   state  pri  cpu%");
    CHECK(row_trimmed(grid, 5).empty());
}

void selection_follows_task_and_clamps() {
    MonitorSnapshot snapshot = sample_snapshot();
    snapshot.tasks[2] =
        MonitorTask{2, 0x3000, "Shell", TaskState::Ready, false, -1, 120, true, false};
    snapshot.task_count = 3;
    MonitorGrid grid;
    grid.columns = 80;
    grid.rows = 25;
    MonitorUi ui;
    ui.reset();
    ui.render(snapshot, grid);

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
    MonitorGrid grid;
    grid.columns = 80;
    grid.rows = 25;
    MonitorUi ui;
    ui.reset();

    CHECK(press(ui, snapshot, kTab).op == MonitorOp::None);
    MonitorRequest request = press(ui, snapshot, kEnter);
    CHECK(request.op == MonitorOp::ListDirectory);
    CHECK_EQ(request.drive, 0u);
    CHECK_EQ(request.path[0], '\0');

    set_entries(snapshot, "", {{"DH0:", true}, {"DH1:", true}});
    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 4) == "BROWSE  partitions");
    CHECK(row_trimmed(grid, 5) == "> DH0:");
    CHECK(row_trimmed(grid, 6) == "  DH1:");
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

    ui.render(snapshot, grid);
    CHECK(row_trimmed(grid, 4) == "TASKS                   state  pri  cpu%");
}

void changed_cells_visitor_reports_only_differences() {
    MonitorGrid before;
    before.columns = 4;
    before.rows = 3;
    MonitorGrid after = before;
    after.at(1, 1).ch = 'x';
    after.at(2, 2).style = monitor_style(MonitorStyle::Warning);

    std::vector<std::pair<u32, u32>> changed;
    visit_changed_cells(before, after,
                        [&](u32 column, u32 row, const MonitorCell& cell) {
                            changed.emplace_back(column, row);
                            CHECK_EQ(cell.ch, after.at(column, row).ch);
                            CHECK_EQ(cell.style, after.at(column, row).style);
                        });
    CHECK_EQ(changed.size(), 2u);
    CHECK_EQ(changed[0].first, 1u);
    CHECK_EQ(changed[0].second, 1u);
    CHECK_EQ(changed[1].first, 2u);
    CHECK_EQ(changed[1].second, 2u);

    std::vector<std::pair<u32, u32>> unchanged;
    visit_changed_cells(after, after, [&](u32 column, u32 row, const MonitorCell&) {
        unchanged.emplace_back(column, row);
    });
    CHECK(unchanged.empty());
}

}

int main() {
    layout_at_80x25();
    many_cores_are_summarised();
    tiny_grids_stay_in_bounds();
    held_keys_act_once();
    requests_carry_their_fields();
    confirmations_are_accepted_and_refused();
    inspect_view_shows_registers_and_waiting();
    selection_follows_task_and_clamps();
    browsing_into_and_out_of_directories();
    changed_cells_visitor_reports_only_differences();
    return check::finish("test_monitor_ui");
}
