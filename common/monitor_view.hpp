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

#include "bitmap_surface.hpp"
#include "libs/gfx.hpp"
#include "types.hpp"

namespace hombre {

enum class MonitorPartKind : u8 {
    Window,
    Group,
    Text,
    Gauge,
    Row,
    Scroller,
    Button,
    Drive,
    Dialog
};

enum class MonitorAction : u8 {
    None,
    Close,
    SelectTask,
    Inspect,
    Suspend,
    End,
    Reboot,
    SelectDrive,
    Insert,
    Eject,
    SelectEntry,
    Open,
    Up,
    Cancel,
    Back,
    Scroll,
    Confirm,
    Dismiss
};

// One painted and hit-tested piece of the monitor window. A part with an
// action is a target; index is the task, entry or drive it stands for.
struct MonitorPart {
    MonitorPartKind kind;
    MonitorAction action = MonitorAction::None;
    u16 index = 0;
    Rect box;
    Rect knob;
    bool selected, disabled, pressed, warning;
    u32 permille = 0;
    char text[80];
};

struct MonitorView {
    static constexpr u32 kMaxParts = 160;
    MonitorPart parts[kMaxParts];
    u32 count = 0;

    // The topmost part with an action at the point, null if none; under a
    // dialog only its OK and Cancel.
    const MonitorPart* at(i32 x, i32 y) const;
    const MonitorPart* find(MonitorAction action, u32 index = 0) const;
};

struct MonitorMouse {
    i32 x, y;
    u8 buttons;
    i32 wheel;
    u64 now_us;
};

struct MonitorPlacement {
    u32 scale, left, top;
};

// Where the 640x400 window goes on a screen: the largest whole scale that
// fits, at least 1, centred; left and top are 0 on a smaller screen.
inline MonitorPlacement monitor_placement(u32 width, u32 height) {
    u32 scale = width / kMonitorWidth;
    const u32 by_height = height / kMonitorHeight;
    if (by_height < scale) {
        scale = by_height;
    }
    if (scale == 0) {
        scale = 1;
    }
    const u32 used_width = kMonitorWidth * scale;
    const u32 used_height = kMonitorHeight * scale;
    return MonitorPlacement{scale, width > used_width ? (width - used_width) / 2 : 0,
                            height > used_height ? (height - used_height) / 2 : 0};
}

inline const MonitorPart* MonitorView::at(i32 x, i32 y) const {
    const u32 total = count < kMaxParts ? count : kMaxParts;
    u32 first = 0;
    for (u32 i = total; i > 0; --i) {
        if (parts[i - 1].kind == MonitorPartKind::Dialog) {
            first = i;
            break;
        }
    }
    for (u32 i = total; i > first; --i) {
        const MonitorPart& part = parts[i - 1];
        if (part.action != MonitorAction::None && x >= part.box.x0 && x <= part.box.x1 &&
            y >= part.box.y0 && y <= part.box.y1) {
            return &part;
        }
    }
    return nullptr;
}

inline const MonitorPart* MonitorView::find(MonitorAction action, u32 index) const {
    const u32 total = count < kMaxParts ? count : kMaxParts;
    for (u32 i = 0; i < total; ++i) {
        if (parts[i].action == action && parts[i].index == index) {
            return &parts[i];
        }
    }
    return nullptr;
}

}
