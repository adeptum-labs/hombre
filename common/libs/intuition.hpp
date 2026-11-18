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

#include "../types.hpp"
#include "gfx.hpp"
#include "native.hpp"

// intuition.library written in C++ for code run by the 68000 translator.
//
// It owns the screens: OpenScreen and OpenScreenTagList build a Screen with
// its BitMap, RastPort, ViewPort and ColorMap in guest memory, CloseScreen
// frees it, and ScreenToFront and ScreenToBack order the list. The chrome
// is drawn with the Graphics C++ methods. Every other vector logs
// "intuition: unimplemented LVO -<n>" and ends the run with Unimplemented.

namespace hombre {

class Exec;
class Graphics;

// Library vector offsets of intuition.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace intuition_lvo {

constexpr u32 CloseScreen = 66;
constexpr u32 CloseWindow = 72;
constexpr u32 OpenScreen = 198;
constexpr u32 OpenWindow = 204;
constexpr u32 ScreenToBack = 246;
constexpr u32 ScreenToFront = 252;
constexpr u32 SetWindowTitles = 276;
constexpr u32 WindowToBack = 306;
constexpr u32 WindowToFront = 312;
constexpr u32 LockIBase = 414;
constexpr u32 UnlockIBase = 420;
constexpr u32 GetScreenData = 426;
constexpr u32 OpenWindowTagList = 606;
constexpr u32 OpenScreenTagList = 612;
// The last vector of the 3.9 library; the table reaches down to it.
constexpr u32 Last = 828;

}

class Intuition {
public:
    static constexpr u32 kMaxScreens = 4;
    static constexpr u32 kMaxWindows = 16;
    static constexpr u32 kMaxTagItems = 256;

    // Makes intuition.library over graphics, which must be initialised.
    // False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, Graphics& graphics, DisplayState& display);

    u32 intuition_base() const { return base_; }

    // Opens a screen from the NewScreen at new_screen, overridden by the
    // tags at tags. Either may be 0. Returns the Screen, or 0 with a log.
    u32 open_screen(u32 new_screen, u32 tags);
    void close_screen(u32 screen);
    void screen_to_front(u32 screen);
    u32 front_screen() const { return front_; }

    // Opens a window from the NewWindow at new_window, overridden by the
    // tags at tags. Either may be 0 but not both. Returns the Window, or 0
    // with a log.
    u32 open_window(u32 new_window, u32 tags);
    void close_window(u32 window);
    void set_window_titles(u32 window, u32 title, u32 screen_title);
    void window_to_front(u32 window);

private:
    friend struct IntuitionNatives;

    static constexpr u32 kVectors = intuition_lvo::Last / 6;

    u32 find_screen(u32 screen) const;
    void remove_at(u32 index);
    void write_links();
    void screen_to_back(u32 screen);
    void refresh_screen(u32 screen);
    u32 find_window(u32 window) const;
    void remove_window_at(u32 index);

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Graphics* graphics_ = nullptr;
    DisplayState* display_ = nullptr;
    u32 base_ = 0;
    u32 front_ = 0;
    u32 screens_[kMaxScreens]{};
    u32 windows_[kMaxWindows]{};
    NativeVector vectors_[kVectors]{};
};

}
