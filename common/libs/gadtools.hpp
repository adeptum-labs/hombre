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

#include "../m68k_cpu.hpp"
#include "../types.hpp"
#include "exec.hpp"
#include "native.hpp"
#include "version.hpp"

// gadtools.library written in C++ for code run by the 68000 translator.
//
// It hands out visual info for a screen, makes the context gadget a
// gadget list starts with and the gadgets after it, filters and replies
// IntuiMessages, refreshes windows and draws bevel boxes. Buttons, text
// and number displays work; a gadget of another kind shows its frame and
// does nothing, and the first of each such kind logs "gadtools: gadget
// kind <n> not handled". Every other vector logs "gadtools: unimplemented
// LVO -<n>" and ends the run with Unimplemented.

namespace hombre {

class Graphics;
class Intuition;

// Library vector offsets of gadtools.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace gadtools_lvo {

constexpr u32 CreateGadgetA = 30;
constexpr u32 FreeGadgets = 36;
constexpr u32 GT_SetGadgetAttrsA = 42;
constexpr u32 CreateMenusA = 48;
constexpr u32 FreeMenus = 54;
constexpr u32 LayoutMenuItemsA = 60;
constexpr u32 LayoutMenusA = 66;
constexpr u32 GT_GetIMsg = 72;
constexpr u32 GT_ReplyIMsg = 78;
constexpr u32 GT_RefreshWindow = 84;
constexpr u32 GT_BeginRefresh = 90;
constexpr u32 GT_EndRefresh = 96;
constexpr u32 GT_FilterIMsg = 102;
constexpr u32 GT_PostFilterIMsg = 108;
constexpr u32 CreateContext = 114;
constexpr u32 DrawBevelBoxA = 120;
constexpr u32 GetVisualInfoA = 126;
constexpr u32 FreeVisualInfo = 132;
// The last vector of the 3.1 library; the table reaches down to it.
constexpr u32 GT_GetGadgetAttrsA = 174;

}

class GadTools {
public:
    // Makes gadtools.library and adds it to exec's library list. exec must
    // be initialised. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, Graphics& graphics);

    u32 gadtools_base() const { return base_; }

    // CreateMenusA and LayoutMenusA for code that is not the guest's: the
    // strip made from the NewMenu array at array, and the layout of the
    // strip starting at menu. False for an empty or faulty array.
    u32 create_menus(u32 array);
    bool layout_menus(u32 menu);

    // Draws a gadget gadtools made, through rp of the window, as it stands:
    // Intuition calls it when a button is pressed or let go. False for a
    // GENERIC_KIND gadget, which its program draws, as Intuition does any
    // other boolean gadget.
    bool render(u32 window, u32 rp, u32 gadget);
    static bool render_gadget(void* ctx, u32 window, u32 rp, u32 gadget) {
        return static_cast<GadTools*>(ctx)->render(window, rp, gadget);
    }
    bool release(u32 window, u32 gadget, i32 x, i32 y, u16& code);
    u16 prop_code(u32 gadget);
    static u16 prop_gadget_code(void* ctx, u32 gadget) {
        return static_cast<GadTools*>(ctx)->prop_code(gadget);
    }
    // Intuition, which draws the scrollers and string gadgets gadtools
    // makes when their attributes change.
    void set_intuition(Intuition* intuition) { intuition_ = intuition; }
    static bool release_gadget(void* ctx, u32 window, u32 gadget, i32 x, i32 y, u16& code) {
        return static_cast<GadTools*>(ctx)->release(window, gadget, x, y, code);
    }

private:
    friend struct GadToolsNatives;

    void draw_cycle(native::Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                    i32 bottom, i32 baseline, bool selected);
    void draw_palette(native::Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                      i32 bottom);
    void draw_list(native::Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                   i32 bottom);
    void ghost(u32 rp, u32 flags, i32 left, i32 top, i32 right, i32 bottom);

    static constexpr u32 kVectors = gadtools_lvo::GT_GetGadgetAttrsA / 6;

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Graphics* graphics_ = nullptr;
    Intuition* intuition_ = nullptr;
    u32 base_ = 0;
    bool unhandled_kinds_[14]{};
    NativeVector vectors_[kVectors]{};
};

}
