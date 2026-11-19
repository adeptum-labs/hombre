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
// It owns the screens and windows: OpenScreen and OpenScreenTagList build a
// Screen with its BitMap, RastPort, ViewPort, ColorMap and a private backdrop
// BitMap in guest memory, and each window gets its own planar BitMap. The
// screen's sc_BitMap is composed from the backdrop and the windows from back
// to front whenever a backdrop or window bitmap is damaged or windows change
// order or close. A guest that draws through sc_RastPort still writes
// sc_BitMap directly; that content is overwritten by the next composition of
// that area. Every unimplemented vector logs "intuition: unimplemented LVO
// -<n>" and ends the run with Unimplemented.

namespace hombre {

class Exec;
class Graphics;

// Library vector offsets of intuition.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace intuition_lvo {

constexpr u32 AddGadget = 42;
constexpr u32 CloseScreen = 66;
constexpr u32 CloseWindow = 72;
constexpr u32 ModifyIDCMP = 150;
constexpr u32 ModifyProp = 156;
constexpr u32 OffGadget = 174;
constexpr u32 OnGadget = 186;
constexpr u32 OpenScreen = 198;
constexpr u32 OpenWindow = 204;
constexpr u32 RefreshGadgets = 222;
constexpr u32 RemoveGadget = 228;
constexpr u32 ScreenToBack = 246;
constexpr u32 ScreenToFront = 252;
constexpr u32 SetWindowTitles = 276;
constexpr u32 WindowToBack = 306;
constexpr u32 WindowToFront = 312;
constexpr u32 LockIBase = 414;
constexpr u32 UnlockIBase = 420;
constexpr u32 GetScreenData = 426;
constexpr u32 RefreshGList = 432;
constexpr u32 AddGList = 438;
constexpr u32 RemoveGList = 444;
constexpr u32 NewModifyProp = 468;
constexpr u32 OpenWindowTagList = 606;
constexpr u32 OpenScreenTagList = 612;
// The last vector of the 3.9 library; the table reaches down to it.
constexpr u32 Last = 828;

}

class Intuition {
public:
    using WindowHook = void (*)(void* ctx, u32 window);
    using GadgetHook = void (*)(void* ctx, u32 gadget, u32 vert_pot);

    static constexpr u32 kMaxScreens = 4;
    static constexpr u32 kMaxWindows = 16;
    static constexpr u32 kMaxTagItems = 256;
    static constexpr u32 kMaxMessages = 128;
    static constexpr u32 kMaxGadgets = 64;
    static constexpr u32 kMaxGadgetHooks = 8;

    static Rect close_box_rect(i32 left, i32 top);
    static Rect depth_box_rect(i32 left, i32 top, i32 width);
    static Rect size_box_rect(i32 left, i32 top, i32 width, i32 height);

    // Makes intuition.library over graphics, which must be initialised.
    // False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec, Graphics& graphics, DisplayState& display);

    void set_clock(u64 (*now_us)()) { clock_ = now_us; }

    u32 intuition_base() const { return base_; }

    // Opens a screen from the NewScreen at new_screen, overridden by the
    // tags at tags. Either may be 0. Returns the Screen, or 0 with a log.
    u32 open_screen(u32 new_screen, u32 tags);

    // Closes the screen unless it is unknown or still has open windows.
    bool close_screen(u32 screen);
    void screen_to_front(u32 screen);
    u32 front_screen() const { return front_; }

    // Opens a window from the NewWindow at new_window, overridden by the
    // tags at tags. Either may be 0 but not both. Returns the Window, or 0
    // with a log.
    u32 open_window(u32 new_window, u32 tags);
    void close_window(u32 window);
    bool modify_idcmp(u32 window, u32 flags);
    void set_window_titles(u32 window, u32 title, u32 screen_title);
    void window_to_front(u32 window);
    void window_to_back(u32 window);
    bool window_open(u32 window) const;
    void set_size_hook(u32 window, WindowHook hook, void* ctx);
    u32 add_glist(u32 window, u32 gadget, i32 position, i32 count);
    u32 remove_glist(u32 window, u32 gadget, i32 count);
    void refresh_glist(u32 gadget, u32 window, i32 count);
    void new_modify_prop(u32 gadget, u32 window, u32 flags, u32 hpot, u32 vpot, u32 hbody,
                         u32 vbody, i32 count);
    void on_gadget(u32 gadget, u32 window);
    void off_gadget(u32 gadget, u32 window);
    void set_gadget_hook(u32 gadget, GadgetHook hook, void* ctx);
    void wheel_event(i32 steps);
    u32 window_under(i32 x, i32 y) const { return window_at(x, y); }
    void begin_batch();
    void end_batch();
    u32 active_window() const { return active_window_; }
    u32 dropped_messages() const { return dropped_messages_; }
    void pointer_event(i32 x, i32 y, u8 buttons, u8 pressed, u8 released, bool moved);

    // Moves the window, kept entirely on its screen, and recomposes the
    // screen where it was and where it is.
    void move_window_to(u32 window, i32 left, i32 top);

    // Sizes the window within its limits and its screen: the interior it
    // had keeps its pixels, new area is cleared, the frame is redrawn, the
    // size hook runs and IDCMP_NEWSIZE is posted when the window asked for
    // it and has no NEWSIZE waiting for a reply.
    void size_window_to(u32 window, i32 width, i32 height);
    void reclaim(u32 keep_window = 0, u32 keep_screen = 0);
    // Returns pointers to the screen's native 256-entry strong/light shade
    // tables, rebuilding them if the palette changed. False for unknown or
    // shallow (< 8 plane) screens.
    bool shade_tables(u32 screen, const u8*& strong, const u8*& light);
    u32 backdrop_rastport(u32 screen) const;

private:
    friend struct IntuitionNatives;

    static constexpr u32 kVectors = intuition_lvo::Last / 6;

    // What a screen owns in guest memory, so that closing it never has to
    // believe a guest field.
    struct ScreenRecord {
        u32 addr = 0;
        u32 screen_bytes = 0;
        u32 planes = 0;
        u32 planes_bytes = 0;
        u32 backdrop_bitmap = 0;
        u32 backdrop_bitmap_bytes = 0;
        u32 backdrop_planes = 0;
        u32 backdrop_planes_bytes = 0;
        u32 backdrop_rp = 0;
        u32 backdrop_rp_bytes = 0;
        u32 colormap = 0;
        u32 colormap_bytes = 0;
        u32 colors = 0;
        u32 colors_bytes = 0;
        u32 low_colors = 0;
        u32 low_colors_bytes = 0;
        u32 title = 0;
        u32 title_bytes = 0;
        u32 width = 0;
        u32 height = 0;
        u32 depth = 0;
        u32 first_window = 0;
        u32 palette_gen = 0;
        Rect pending = {0, 0, -1, -1};
        u8 strong_shade[256]{};
        u8 light_shade[256]{};
    };

    // What a window owns, with its place in the screen's window list.
    struct WindowRecord {
        u32 addr = 0;
        u32 window_bytes = 0;
        u32 screen = 0;
        u32 next_window = 0;
        i32 left = 0;
        i32 top = 0;
        i32 width = 0;
        i32 height = 0;
        u32 flags = 0;
        i32 max_w = 0;
        i32 max_h = 0;
        i32 min_w = 0;
        i32 min_h = 0;
        i32 lim_w = 0;
        i32 lim_h = 0;
        u32 rp = 0;
        u32 rp_bytes = 0;
        u32 layer = 0;
        u32 layer_bytes = 0;
        u32 bitmap = 0;
        u32 bitmap_bytes = 0;
        u32 planes = 0;
        u32 planes_bytes = 0;
        u32 title = 0;
        u32 title_bytes = 0;
        u32 user_port = 0;
        bool user_port_owned = false;
        WindowHook size_hook = nullptr;
        void* size_ctx = nullptr;
        // A NEWSIZE is queued and not yet replied to, and the size the last
        // NEWSIZE reported.
        bool newsize_pending = false;
        i32 newsize_w = 0;
        i32 newsize_h = 0;
    };

    struct GadgetHookEntry {
        u32 gadget = 0;
        u32 window = 0;
        GadgetHook hook = nullptr;
        void* ctx = nullptr;
    };

    enum class GadgetKind : u8 { None = 0, Close = 1, Depth = 2, Prop = 3 };
    enum class Drag : u8 { None = 0, Move = 1, Size = 2, Prop = 3 };

    static void reply_message(void* ctx, u32 message);
    static void on_damage(void* ctx, m68k::Machine& m, u32 bitmap, const Rect& rect);

    void rebuild_shade_tables(ScreenRecord& srec);
    void compose(u32 screen, Rect r);
    u32 find_screen(u32 screen) const;
    ScreenRecord* find_screen_record(u32 screen);
    ScreenRecord* free_screen_record();
    void remove_at(u32 index);
    void write_links();
    void write_window_links(u32 screen);
    void screen_to_back(u32 screen);
    void refresh_screen(u32 screen);
    u32 find_window(u32 window) const;
    WindowRecord* find_window_record(u32 window);
    const WindowRecord* find_window_record(u32 window) const;
    WindowRecord* free_window_record();
    void remove_window_at(u32 index);
    void activate_window(u32 window);
    void redraw_window_bar(u32 window);
    bool post_intuimessage(u32 window, u32 idcmp_class, u16 code, u32 iaddress = 0);
    void post_newsize(WindowRecord& rec);
    void route_pointer(i32 x, i32 y, u8 buttons, u8 pressed, u8 released, bool moved);
    bool start_drag(u32 window, i32 x, i32 y);
    void drag_to(i32 x, i32 y);
    void end_drag();
    void flush_window_messages(u32 port);
    void remove_window_messages(u32 port, u32 window);
    bool is_tracked_message(u32 message) const;
    void free_tracked_message(u32 message);
    u32 window_at(i32 x, i32 y) const;
    void set_gadget_disabled(u32 gadget, u32 window, bool disabled);
    void draw_gadget_list(native::Bus& b, u32 window, u32 first, u32 max_count);
    bool hit_prop_gadget(native::Bus& b, u32 window, i32 x, i32 y);
    void drag_prop(i32 x, i32 y);
    void end_prop_drag();
    void call_gadget_hook(u32 gadget, u32 vert_pot);
    void clear_gadget_hook(u32 gadget);
    void clear_window_gadget_hooks(u32 window);
    void log_unhandled_gadget_type(u32 gtype);
    u32 find_gadget_window(u32 gadget) const;

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Graphics* graphics_ = nullptr;
    DisplayState* display_ = nullptr;
    u64 (*clock_)() = nullptr;
    u32 base_ = 0;
    u32 front_ = 0;
    u32 reply_port_ = 0;
    u32 active_window_ = 0;
    u32 pressed_gadget_window_ = 0;
    GadgetKind pressed_gadget_ = GadgetKind::None;
    // The window the left button drags, and the pointer's offset from its
    // top-left corner (Move) or to its bottom-right corner (Size), or from
    // the knob's top-left corner (Prop).
    Drag drag_ = Drag::None;
    u32 drag_window_ = 0;
    u32 drag_gadget_ = 0;
    i32 grab_x_ = 0;
    i32 grab_y_ = 0;
    i32 mouse_x_ = 0;
    i32 mouse_y_ = 0;
    u32 dropped_messages_ = 0;
    u32 batch_depth_ = 0;
    u32 screens_[kMaxScreens]{};
    u32 windows_[kMaxWindows]{};
    u32 messages_[kMaxMessages]{};
    ScreenRecord screen_records_[kMaxScreens]{};
    WindowRecord window_records_[kMaxWindows]{};
    GadgetHookEntry gadget_hooks_[kMaxGadgetHooks]{};
    bool unhandled_gadget_types_[8]{};
    NativeVector vectors_[kVectors]{};
};

}
