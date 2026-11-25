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

#include <vector>

// gadtools.library written in C++ for code run by the 68000 translator.
//
// It hands out visual info for a screen, makes the context gadget a
// gadget list starts with and the gadgets after it, filters and replies
// IntuiMessages, refreshes windows and draws bevel boxes. Buttons,
// checkboxes, cycle, palette, string, integer, scroller, slider and list
// view gadgets and text and number displays work; a gadget of another kind
// shows its frame and does nothing, and the first of each such kind logs
// "gadtools: gadget kind <n> not handled". Every other vector logs
// "gadtools: unimplemented LVO -<n>" and ends the run with Unimplemented.

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

// The parts of libraries/gadtools.h that ROM code making gadgets shares
// with the library.
namespace gadtools_defs {

// Gadget kinds (libraries/gadtools.h).
constexpr u32 GENERIC_KIND = 0;
constexpr u32 BUTTON_KIND = 1;
constexpr u32 CHECKBOX_KIND = 2;
constexpr u32 INTEGER_KIND = 3;
constexpr u32 LISTVIEW_KIND = 4;
constexpr u32 NUMBER_KIND = 6;
constexpr u32 CYCLE_KIND = 7;
constexpr u32 PALETTE_KIND = 8;
constexpr u32 SCROLLER_KIND = 9;
constexpr u32 SLIDER_KIND = 11;
constexpr u32 STRING_KIND = 12;
constexpr u32 TEXT_KIND = 13;
constexpr u32 kKinds = 14;

// struct NewGadget.
constexpr u32 ng_LeftEdge = 0;
constexpr u32 ng_TopEdge = 2;
constexpr u32 ng_Width = 4;
constexpr u32 ng_Height = 6;
constexpr u32 ng_GadgetText = 8;
constexpr u32 ng_TextAttr = 12;
constexpr u32 ng_GadgetID = 16;
constexpr u32 ng_Flags = 18;
constexpr u32 ng_VisualInfo = 22;
constexpr u32 ng_UserData = 26;
constexpr u32 NewGadget_SIZEOF = 30;

// Label placement, ng_Flags.
constexpr u32 PLACETEXT_LEFT = 0x1;
constexpr u32 PLACETEXT_RIGHT = 0x2;
constexpr u32 PLACETEXT_ABOVE = 0x4;
constexpr u32 PLACETEXT_BELOW = 0x8;
constexpr u32 PLACETEXT_IN = 0x10;

constexpr u32 GTTX_Text = 0x8008000B;
constexpr u32 GTCB_Checked = 0x80080004;
constexpr u32 GTST_String = 0x8008002D;
constexpr u32 GTST_MaxChars = 0x8008002E;
constexpr u32 GTIN_Number = 0x8008002F;
constexpr u32 GTIN_MaxChars = 0x80080030;
constexpr u32 GTLV_Labels = 0x80080006;
constexpr u32 GTLV_ShowSelected = 0x80080035;
constexpr u32 GTLV_Selected = 0x80080036;
constexpr u32 GTLV_ItemHeight = 0x8008004D;
constexpr u32 GTLV_MakeVisible = 0x8008004E;

// The frame around the rows of a list view.
constexpr i32 kListViewFrame = 2;

}

class GadTools {
public:
    // Makes gadtools.library and adds it to exec's library list. exec must
    // be initialised. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, Graphics& graphics);

    u32 gadtools_base() const { return base_; }

    // CreateMenusA and LayoutMenusA for code that is not the guest's: the
    // strip made from the NewMenu array at array, and the layout of the
    // strip starting at menu for the TextFont at font, which is the
    // default screen's for 0. False for an empty or faulty array.
    u32 create_menus(u32 array);
    bool layout_menus(u32 menu, u32 font = 0);

    // CreateContext, CreateGadgetA, FreeGadgets and GT_RefreshWindow for
    // code that is not the guest's, with their arguments and results.
    u32 create_context(u32 list);
    u32 create_gadget(u32 kind, u32 previous, u32 new_gadget, u32 tags);
    // GT_SetGadgetAttrsA for code that is not the guest's: the gadget of
    // the window takes the tags and is drawn again. False when they fault.
    bool set_gadget_attrs(u32 gadget, u32 window, u32 tags);
    void free_gadgets(u32 first);
    void refresh_window(u32 window);

    // Draws a gadget gadtools made, through rp of the window, as it stands:
    // Intuition calls it when a button is pressed or let go. False for a
    // GENERIC_KIND gadget, which its program draws, as Intuition does any
    // other boolean gadget.
    bool render(u32 window, u32 rp, u32 gadget);
    static bool render_gadget(void* ctx, u32 window, u32 rp, u32 gadget) {
        return static_cast<GadTools*>(ctx)->render(window, rp, gadget);
    }
    // A click let go over a gadget gadtools made, at x, y of its box: the
    // gadget takes its new state and gives the code of its IDCMP_GADGETUP.
    // False when the click sends none.
    bool release(u32 window, u32 gadget, i32 x, i32 y, u16& code);
    // A scroller or slider takes its state from where Intuition moved its
    // knob to, and gives the code its messages carry.
    u16 prop_code(u32 gadget);
    // The same for a prop gadget of the window; false for a list view's
    // scroller, which scrolls the list and sends no message.
    bool prop_moved(u32 window, u32 gadget, u16& code);
    static bool prop_gadget_moved(void* ctx, u32 window, u32 gadget, u16& code) {
        return static_cast<GadTools*>(ctx)->prop_moved(window, gadget, code);
    }
    // Intuition, which draws the scrollers and string gadgets gadtools
    // makes when their attributes change.
    void set_intuition(Intuition* intuition) { intuition_ = intuition; }
    static bool release_gadget(void* ctx, u32 window, u32 gadget, i32 x, i32 y, u16& code) {
        return static_cast<GadTools*>(ctx)->release(window, gadget, x, y, code);
    }
    // The wheel turned steps notches over a gadget gadtools made, up for
    // positive: a list view, or its scroller or arrows, scrolls a row a
    // notch and sends no message; a slider's level moves a step a notch, up
    // for more, and a scroller's top a step, up towards the start, and
    // report and code ask for the GADGETUP a drag ends with. False for any
    // other gadget.
    bool wheel(u32 window, u32 gadget, i32 steps, u16& code, bool& report);
    static bool wheel_gadget(void* ctx, u32 window, u32 gadget, i32 steps, u16& code,
                             bool& report) {
        return static_cast<GadTools*>(ctx)->wheel(window, gadget, steps, code, report);
    }

private:
    friend struct GadToolsNatives;

    // The font a menu strip is laid out for, with the height and width of
    // its cells, and the TextAttr its items' IntuiTexts are to name, 0 for
    // none.
    struct MenuFont {
        u32 font;
        u32 text_attr;
        u32 height;
        u32 cell;
    };
    MenuFont menu_font(u32 font, u32 text_attr) const;
    bool layout_strip(u32 menu, const MenuFont& font);

    void draw_cycle(native::Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                    i32 bottom, i32 baseline, bool selected, u32 font);
    void draw_checkbox(u32 rp, i32 left, i32 top, i32 right, i32 bottom, bool selected);
    void draw_palette(native::Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                      i32 bottom);
    void draw_list(native::Bus& b, u32 rp, u32 gadget, i32 left, i32 top, i32 right,
                   i32 bottom, u32 font);
    void draw_list_arrow(native::Bus& b, u32 rp, i32 left, i32 top, i32 width, i32 height,
                         bool down, bool pressed);
    void draw_list_view(native::Bus& b, u32 window, u32 gadget);
    void ghost(u32 rp, u32 flags, i32 left, i32 top, i32 right, i32 bottom);
    // A slider's level as text beside its box, when it has a level format.
    void draw_slider_level(native::Bus& b, u32 rp, u32 gadget);
    u32 level_text(native::Bus& b, u32 gadget, i32 level, char* out, u32 capacity);
    i32 displayed_level(native::Bus& b, u32 gadget, i32 level);
    // Draws the levels of sliders moved since the program last fetched a
    // message from port, whose level text needs its own function.
    void draw_pending_levels(native::Bus& b, u32 port);
    // The font of the VisualInfo at vi; the default screen's for 0 or for
    // what is no VisualInfo, and 0, the built-in font, without one.
    u32 visual_font(native::Bus& b, u32 vi) const;

    static constexpr u32 kVectors = gadtools_lvo::GT_GetGadgetAttrsA / 6;

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Graphics* graphics_ = nullptr;
    Intuition* intuition_ = nullptr;
    u32 base_ = 0;
    bool unhandled_kinds_[14]{};
    // Sliders with a level function Intuition moved: the function runs in
    // the program's own task, when it next fetches a message.
    struct PendingLevel {
        u32 window;
        u32 gadget;
    };
    static constexpr u32 kPendingLevels = 256;
    std::vector<PendingLevel> pending_levels_;
    NativeVector vectors_[kVectors]{};
};

}
