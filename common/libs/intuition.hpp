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
#include "boopsi.hpp"
#include "chrome_palette.hpp"
#include "gfx.hpp"
#include "menu_layout.hpp"
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

namespace chrome {
enum class BarButton : u8;
}

namespace raster {
struct Bitmap;
struct RastPortView;
}

// Library vector offsets of intuition.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace intuition_lvo {

constexpr u32 AddGadget = 42;
constexpr u32 GetDefPrefs = 126;
constexpr u32 GetPrefs = 132;
constexpr u32 SetPrefs = 324;
constexpr u32 SetIPrefs = 576;
constexpr u32 PubScreenStatus = 552;
constexpr u32 DrawImage = 114;
constexpr u32 CloseScreen = 66;
constexpr u32 CloseWorkBench = 78;
constexpr u32 ClearMenuStrip = 54;
constexpr u32 CloseWindow = 72;
constexpr u32 ItemAddress = 144;
constexpr u32 ModifyIDCMP = 150;
constexpr u32 ModifyProp = 156;
constexpr u32 OffGadget = 174;
constexpr u32 OnGadget = 186;
constexpr u32 OpenScreen = 198;
constexpr u32 OpenWindow = 204;
constexpr u32 OpenWorkBench = 210;
constexpr u32 RefreshGadgets = 222;
constexpr u32 RemoveGadget = 228;
constexpr u32 ScreenToBack = 246;
constexpr u32 ScreenToFront = 252;
constexpr u32 SetMenuStrip = 264;
constexpr u32 SetWindowTitles = 276;
constexpr u32 WindowToBack = 306;
constexpr u32 WindowToFront = 312;
constexpr u32 WindowLimits = 318;
constexpr u32 BeginRefresh = 354;
constexpr u32 EndRefresh = 366;
constexpr u32 FreeSysRequest = 372;
constexpr u32 LockIBase = 414;
constexpr u32 UnlockIBase = 420;
constexpr u32 GetScreenData = 426;
constexpr u32 RefreshGList = 432;
constexpr u32 AddGList = 438;
constexpr u32 RemoveGList = 444;
constexpr u32 NewModifyProp = 468;
constexpr u32 LockPubScreen = 510;
constexpr u32 UnlockPubScreen = 516;
constexpr u32 LockPubScreenList = 522;
constexpr u32 UnlockPubScreenList = 528;
constexpr u32 GetDefaultPubScreen = 582;
constexpr u32 EasyRequestArgs = 588;
constexpr u32 BuildEasyRequestArgs = 594;
constexpr u32 SysReqHandler = 600;
constexpr u32 GetScreenDrawInfo = 690;
constexpr u32 FreeScreenDrawInfo = 696;
constexpr u32 ResetMenuStrip = 702;
constexpr u32 OpenWindowTagList = 606;
constexpr u32 OpenScreenTagList = 612;
constexpr u32 EraseImage = 630;
constexpr u32 DrawImageState = 618;
constexpr u32 ObtainGIRPort = 558;
constexpr u32 IntuiTextLength = 330;
constexpr u32 InitRequester = 138;
constexpr u32 ActivateGadget = 462;
constexpr u32 RefreshWindowFrame = 456;
constexpr u32 ClearPointer = 60;
constexpr u32 SetPointer = 270;
constexpr u32 SetWindowPointerA = 816;
constexpr u32 DoubleClick = 102;
constexpr u32 ScrollWindowRaster = 798;
constexpr u32 Request = 240;
constexpr u32 EndRequest = 120;
constexpr u32 DrawBorder = 108;
constexpr u32 PrintIText = 216;
constexpr u32 ReleaseGIRPort = 564;
constexpr u32 GadgetMouse = 570;
constexpr u32 NewObjectA = 636;
constexpr u32 DisposeObject = 642;
constexpr u32 SetAttrsA = 648;
constexpr u32 GetAttr = 654;
constexpr u32 SetGadgetAttrsA = 660;
constexpr u32 NextObject = 666;
constexpr u32 MakeClass = 678;
constexpr u32 AddClass = 684;
constexpr u32 RemoveClass = 708;
constexpr u32 FreeClass = 714;
constexpr u32 DoGadgetMethodA = 810;
// The last vector of the 3.9 library; the table reaches down to it.
constexpr u32 Last = 828;

}

namespace ndk {

// IDCMP_IDCMPUPDATE (intuition/intuition.h): a notification for a window,
// its IAddress an attribute list.
constexpr u32 IDCMP_IDCMPUPDATE = 0x00800000;

}

class Intuition {
public:
    using WindowHook = void (*)(void* ctx, u32 window);
    using GadgetHook = void (*)(void* ctx, u32 gadget, u32 vert_pot);
    // Draws a GTYP_GADTOOLS gadget of the window through rp, as its
    // GFLG_SELECTED flag shows it. False when the gadget is its program's
    // to draw, as a GENERIC_KIND one is.
    using GadgetRenderer = bool (*)(void* ctx, u32 window, u32 rp, u32 gadget);
    // A click let go over a GTYP_GADTOOLS gadget at x, y of its box: the
    // gadget takes its new state and sets the code of its GADGETUP. False
    // when the click is to send no GADGETUP.
    using GadgetRelease = bool (*)(void* ctx, u32 window, u32 gadget, i32 x, i32 y, u16& code);
    // A GTYP_GADTOOLS prop gadget of the window moved: the gadget takes its
    // new state from its PropInfo and gives the code its messages carry.
    // False when the gadget keeps its messages to itself.
    using GadgetPropCode = bool (*)(void* ctx, u32 window, u32 gadget, u16& code);

    // What CloseWorkBench and OpenWorkBench call of the Workbench: close
    // its windows for a screen reset, and open them again after it. A close
    // that returns false leaves the Workbench as it is.
    struct WorkbenchHooks {
        bool (*close)(void* ctx);
        void (*open)(void* ctx);
        void* ctx;
    };

    static constexpr u32 kMaxScreens = 4;
    static constexpr u32 kMaxWindows = 16;
    static constexpr u32 kMaxTagItems = 256;
    static constexpr u32 kMaxMessages = 128;
    static constexpr u32 kMaxGadgets = 64;
    static constexpr u32 kMaxGadgetHooks = 8;

    static Rect close_box_rect(i32 left, i32 top);
    static Rect depth_box_rect(i32 left, i32 top, i32 width);
    static Rect size_box_rect(i32 left, i32 top, i32 width, i32 height, u32 flags = 0);

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

    // EasyRequestArgs, BuildEasyRequestArgs, SysReqHandler and
    // FreeSysRequest for code that is not the guest's. The EasyStruct is at
    // easy_struct, the argument stream the formats consume at args, and
    // idcmp the caller's own IDCMP classes.
    u32 build_easy_request(u32 window, u32 easy_struct, u32 idcmp, u32 args);
    // Answers the requester's input: a button's number, -1 for a message of
    // one of the caller's classes, left in the ULONG at idcmp_ptr, or -2
    // when there is no input and wait is false.
    i32 sys_req_handler(u32 window, u32 idcmp_ptr, bool wait);
    // Closes a window BuildEasyRequestArgs opened and drops its buttons;
    // any other window is left alone.
    void free_sys_request(u32 window);
    // Builds the requester, answers it until a button or the caller's event
    // ends it, and frees it.
    i32 easy_request(u32 window, u32 easy_struct, u32 idcmp_ptr, u32 args);

    bool modify_idcmp(u32 window, u32 flags);
    void set_window_titles(u32 window, u32 title, u32 screen_title);
    // Draws the window's border and title bar again; what is inside the
    // border stays as it is.
    void refresh_window_frame(u32 window);
    void window_to_front(u32 window);
    void window_to_back(u32 window);
    bool window_limits(u32 window, i32 width_min, i32 height_min, u32 width_max,
                       u32 height_max);
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
    // The library that draws GTYP_GADTOOLS gadgets: gadtools.
    void set_gadget_renderer(GadgetRenderer renderer, void* ctx) {
        gadget_renderer_ = renderer;
        gadget_renderer_ctx_ = ctx;
    }
    // Takes the renderer's ctx.
    void set_gadget_release(GadgetRelease release) { gadget_release_ = release; }
    void set_gadget_prop_code(GadgetPropCode code) { gadget_prop_code_ = code; }

    // A string gadget being edited takes the keyboard: true while one is,
    // and a decoded key byte for it (CSI sequences for the cursor keys).
    bool string_active() const { return string_.gadget != 0; }
    void string_key(u8 byte);
    // Makes the string gadget of the window the one being edited.
    bool activate_string(u32 gadget, u32 window);
    void wheel_event(i32 steps);
    u32 window_under(i32 x, i32 y) const { return window_at(x, y); }
    void begin_batch();
    void end_batch();
    void flush_palette();

    // Draws the Image chain at image at left, top on the RastPort, as
    // DrawImage does: every pixel's pen comes from the data planes
    // PlanePick selects, the bits of unselected planes from PlaneOnOff.
    // GetScreenDrawInfo: the screen's DrawInfo, 0 for no open screen.
    u32 screen_draw_info(u32 screen);

    // BOOPSI: the classes and objects of the BOOPSI vectors.
    Boopsi& boopsi() { return boopsi_; }

    // A GadgetInfo for a gadget of the window, in guest memory, and its
    // release. 0 if the window is not open or memory runs out.
    u32 make_gadget_info(u32 window);
    void free_gadget_info(u32 info);
    void draw_image(u32 rp, u32 image, i32 left, i32 top);

    // The same image with every pen of its box complemented, as a
    // complement highlight over the drawn image leaves it: the look of a
    // selected icon without a SelectRender image.
    void draw_image_complemented(u32 rp, u32 image, i32 left, i32 top);

    // DrawImageState: the image as its state shows it, with draw_info over
    // the screen's DrawInfo when it is 0. A BOOPSI image gets IM_DRAW.
    void draw_image_state(u32 rp, u32 image, i32 left, i32 top, u32 state, u32 draw_info);

    // SetMenuStrip and ClearMenuStrip for code that is not the guest's:
    // the strip the window's right button opens.
    void set_menu_strip(u32 window, u32 menu);
    void clear_menu_strip(u32 window);

    // Clears each image's rectangle to pen 0, as EraseImage does through
    // EraseRect on a RastPort without a backfill hook.
    void erase_image(u32 rp, u32 image, i32 left, i32 top);

    // ReportMouse: whether the window gets IDCMP_MOUSEMOVE as the pointer
    // moves.
    void report_mouse(u32 window, bool on);

    // An Image drawn above every window of the screen with its top-left
    // corner at x, y, as the Workbench shows an icon being dragged. A new
    // call moves it; clear_drag_image takes it away.
    void set_drag_image(u32 screen, u32 image, i32 x, i32 y);
    void clear_drag_image();

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
    // Closes the windows and screens task opened, except keep_window and
    // keep_screen, and frees the IntuiMessages it posted. Windows, screens
    // and messages of other tasks stay.
    void reclaim_owned_by(u32 task, u32 keep_window = 0, u32 keep_screen = 0);
    // Like the above, keeping the windows in keep_windows.
    void reclaim_owned_by(u32 task, const u32* keep_windows, u32 count, u32 keep_screen);

    // The task that opened the window, 0 when it is not open.
    u32 window_owner(u32 window) const;

    // The screen shells open their windows on. A screen made the default
    // takes the palette and DrawInfo pens IPrefs has sent, and its chrome
    // colours are worked out again from them.
    void set_default_screen(u32 screen);
    u32 default_screen() const { return default_screen_; }

    // The Workbench's side of CloseWorkBench and OpenWorkBench. The task
    // that sets them is the one the Workbench windows belong to.
    void set_workbench_hooks(const WorkbenchHooks& hooks);

    // Rewrites the pens of the screen's existing DrawInfo from the set
    // IPrefs sent that fits its depth, when that set has arrived.
    void apply_pen_prefs(u32 screen);
    // Returns pointers to the screen's native 256-entry strong/light shade
    // tables, rebuilding them if the palette changed. False for unknown or
    // shallow (< 8 plane) screens.
    bool shade_tables(u32 screen, const u8*& strong, const u8*& light);
    u32 backdrop_rastport(u32 screen) const;

private:
    friend struct IntuitionNatives;
    friend struct IntuitionTest;

    static constexpr u32 kVectors = intuition_lvo::Last / 6;

    static constexpr u32 kMaxRequesters = 8;
    static constexpr u32 kMaxRequesterButtons = 8;
    static constexpr u32 kMaxRequesterLabel = 40;
    static constexpr u32 kMaxRequesterText = 1024;
    static constexpr u32 kMaxRequesterGadgetText = 256;
    static constexpr u32 kMaxRequesterLines = 32;
    static constexpr i32 kRequesterMargin = 8;
    static constexpr i32 kRequesterTextPad = 8;
    static constexpr i32 kRequesterGap = 8;
    static constexpr i32 kRequesterButtonHeight = 14;
    static constexpr i32 kRequesterButtonPad = 12;
    static constexpr i32 kRequesterButtonGap = 8;

    // One of the two DrawInfo pen sets IPrefs sends: kind 0 for screens of
    // four colours or fewer, kind 1 for the rest.
    struct PenPrefs {
        bool set = false;
        u16 pen[chrome::kDrawInfoPens]{};
    };

    // What a screen owns in guest memory, so that closing it never has to
    // believe a guest field.
    struct ScreenRecord {
        u32 addr = 0;
        u32 owner = 0;
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
        u32 draw_info = 0;  // the DrawInfo GetScreenDrawInfo hands out, made on first use
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
        u32 owner = 0;
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

    // A requester window EasyRequestArgs built: the IDCMP classes that are
    // the caller's rather than the requester's, where each button lies, its
    // label kept for redrawing, and the button the pointer holds down.
    struct Requester {
        u32 window = 0;
        u32 idcmp = 0;
        u32 button_count = 0;
        i32 pressed = -1;
        Rect buttons[kMaxRequesterButtons]{};
        char labels[kMaxRequesterButtons][kMaxRequesterLabel]{};
        u8 label_length[kMaxRequesterButtons]{};
    };

    struct GadgetHookEntry {
        u32 gadget = 0;
        u32 window = 0;
        GadgetHook hook = nullptr;
        void* ctx = nullptr;
    };

    // The strip shown while the right button is held: which title is open,
    // which items are highlighted, and the layout read from the guest
    // structures.
    struct MenuMode {
        bool active = false;
        u32 screen = 0;
        u32 window = 0;
        u32 strip = 0;
        u32 title = menu_layout::kNoTitle;
        i32 item = -1;
        i32 sub = -1;
        bool sub_hover = false;
        u32 title_count = 0;
        u32 item_count = 0;
        u32 sub_count = 0;
        Rect overlay{0, 0, -1, -1};
        menu_layout::Title titles[menu_layout::kMaxTitles]{};
        menu_layout::Item items[menu_layout::kMaxItems]{};
        menu_layout::Item subs[menu_layout::kMaxItems]{};
        menu_layout::Geometry geometry{};
    };

    enum class GadgetKind : u8 { None = 0, Close = 1, Depth = 2, Prop = 3, Bool = 4 };
    enum class Drag : u8 { None = 0, Move = 1, Size = 2, Prop = 3 };

    static void reply_message(void* ctx, u32 message);
    static void on_damage(void* ctx, m68k::Machine& m, u32 bitmap, const Rect& rect);

    void rebuild_shade_tables(ScreenRecord& srec);
    // The screen is the default one, or the front one while none has been
    // made default yet.
    bool default_candidate(u32 screen) const;
    // The stored pens that fit a screen of depth, or null when that set has
    // not arrived.
    const u16* pen_prefs_for(u32 depth) const;
    void store_palette_prefs(u32 prefs, u32 size);
    void store_pen_prefs(u32 prefs, u32 size);
    void write_colormap_rgb(ScreenRecord& srec, u32 pen, u32 rgb);
    // Writes the stored palette into the screen's ColorMap over its default
    // palette and marks the palette changed.
    void apply_palette_prefs(ScreenRecord& srec);
    // Works the menu and chrome colours out of the screen's DrawInfo pens
    // and writes them into its ColorMap, then recomposes.
    void update_chrome_palette(u32 screen);
    // Clears the screen to its BACKGROUNDPEN, draws its title bar again and
    // recomposes every pixel, as OpenWorkBench starts the screen over.
    void reset_screen(u32 screen);
    void compose(u32 screen, Rect r);
    u32 find_screen(u32 screen) const;
    // The ColorMap of the screen a BitMap is shown on: the screen's own,
    // its backdrop's or one of its windows'. 0 for any other BitMap.
    u32 colormap_of_bitmap(u32 bitmap) const;
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
    u32 last_normal_window(const ScreenRecord& srec, u32 skip_window = 0) const;
    u32 last_window(const ScreenRecord& srec, u32 skip_window = 0) const;
    void insert_window_after(ScreenRecord& srec, WindowRecord& rec, u32 after);
    bool move_window_after(ScreenRecord& srec, WindowRecord& rec, u32 after);
    void activate_window(u32 window);
    void redraw_window_bar(u32 window);
    // The title-bar button of `window` that shows pressed: the one the left
    // button holds, while the pointer is over it.
    chrome::BarButton held_bar_button(u32 window) const;
    // Shows the held title-bar button pressed or released, redrawing its
    // window's bar when that changes.
    void light_bar_button(bool lit);
    bool post_intuimessage(u32 window, u32 idcmp_class, u16 code, u32 iaddress = 0,
                           u32 iaddress_bytes = 0);
    static void notify_idcmp_update(void* ctx, u32 window, const u32* tags, u32 count,
                                    u32 flags);
    void post_idcmp_update(u32 window, const u32* tags, u32 count);
    void free_message_update(u32 index);
    void post_newsize(WindowRecord& rec);
    void route_pointer(i32 x, i32 y, u8 buttons, u8 pressed, u8 released, bool moved);
    bool menu_strip_ready(native::Bus& b) const;
    bool enter_menu_mode(native::Bus& b, i32 x, i32 y);
    void leave_menu_mode();
    void reload_menu(native::Bus& b);
    void hover_menu(native::Bus& b, i32 x, i32 y);
    void release_menu(native::Bus& b, i32 x, i32 y);
    void clear_menu_checked(native::Bus& b, const menu_layout::Item* items, u32 count, u32 mask);
    void draw_menu_overlay(native::Bus& b, raster::Bitmap& bitmap, const Rect& clip);
    Rect menu_overlay(const ScreenRecord& srec) const;
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
    bool hit_bool_gadget(native::Bus& b, u32 window, i32 x, i32 y);
    void draw_custom_gadget(native::Bus& b, u32 window, u32 rp, u32 gadget);
    void draw_string_gadget(native::Bus& b, u32 window, u32 rp, u32 gadget);
    bool hit_string_gadget(native::Bus& b, u32 window, i32 x, i32 y);
    void end_string(bool verify, u16 code);
    Rect string_text_box(native::Bus& b, u32 window, u32 gadget, u32& columns);
    bool hit_custom_gadget(native::Bus& b, u32 window, i32 x, i32 y);
    void custom_input(native::Bus& b, i32 x, i32 y, u32 code, u32 qualifier);
    void custom_result(native::Bus& b, u32 result);
    void drop_custom_gadget();
    void release_bool_gadget(native::Bus& b, u32 window, i32 x, i32 y);
    void draw_bool_gadget(native::Bus& b, u32 window, u32 rp, u32 gadget);
    bool gadtools_draws(native::Bus& b, u32 window, u32 rp, u32 gadget);
    void complement_gadget(native::Bus& b, u32 window, u32 rp, u32 gadget);
    void drag_prop(i32 x, i32 y);
    void end_prop_drag();
    void call_gadget_hook(u32 gadget, u32 vert_pot);
    void report_image_damage(const raster::RastPortView& view, const Rect& rect);
    void draw_image_mode(u32 rp, u32 image, i32 left, i32 top, u32 state, u32 draw_info);
    static void draw_sys_image(void* ctx, u32 rp, u32 which, i32 left, i32 top, i32 width,
                               i32 height, u32 state, u32 draw_info);
    void draw_image_view(native::Bus& b, const raster::RastPortView& view, u32 image, i32 left,
                         i32 top, bool complement, bool damage);
    Rect icon_drag_rect(native::Bus& b) const;
    void clear_gadget_hook(u32 gadget);
    void clear_window_gadget_hooks(u32 window);
    void log_unhandled_gadget_type(u32 gtype);
    u32 find_gadget_window(u32 gadget) const;
    Requester* find_requester(u32 window);
    const Requester* find_requester(u32 window) const;
    Requester* free_requester();
    void drop_requester(u32 window);
    void draw_requester_button(u32 window, const Requester& req, u32 index, bool pressed);

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Graphics* graphics_ = nullptr;
    DisplayState* display_ = nullptr;
    u64 (*clock_)() = nullptr;
    u32 base_ = 0;
    u32 front_ = 0;
    u32 default_screen_ = 0;
    WorkbenchHooks workbench_hooks_{};
    bool workbench_closed_ = false;
    // The Workbench's task, and the task its open hook's windows belong to
    // while the hook runs.
    u32 workbench_task_ = 0;
    u32 workbench_hook_task_ = 0;
    u32 pub_screen_list_ = 0;  // LockPubScreenList's List and its one node
    u32 reply_port_ = 0;
    u32 active_window_ = 0;
    u32 pressed_gadget_window_ = 0;
    GadgetKind pressed_gadget_ = GadgetKind::None;
    // The BOOPSI gadget that has the input, between GM_GOACTIVE and
    // GM_GOINACTIVE: its window, its GadgetInfo, and the InputEvent and
    // termination long its methods get.
    struct ActiveCustom {
        u32 window = 0;
        u32 gadget = 0;
        u32 info = 0;
        u32 event = 0;
    };
    ActiveCustom custom_;
    // The string gadget being edited, its window, and how far a CSI
    // sequence of the keyboard has come.
    struct ActiveString {
        u32 window = 0;
        u32 gadget = 0;
        u8 csi = 0;
    };
    ActiveString string_;
    // The struct Preferences GetPrefs hands out and SetPrefs replaces.
    static constexpr u32 kPrefsBytes = 232;
    u8 prefs_[kPrefsBytes]{};
    bool prefs_set_ = false;
    // The palette colours IPrefs sent, by pen, with those it set.
    bool palette_prefs_set_[256]{};
    u32 palette_prefs_[256]{};
    PenPrefs pen_prefs_[2];
    // The windows a Request() blocks, with its Requester, until EndRequest:
    // a press in one reaches none of its gadgets.
    struct Block {
        u32 window = 0;
        u32 requester = 0;
    };
    static constexpr u32 kMaxBlocks = 16;
    Block blocks_[kMaxBlocks]{};
    bool blocked(u32 window) const;
    bool bar_button_lit_ = false;  // the held Close or Depth button shows pressed
    u32 pressed_bool_gadget_ = 0;  // the boolean gadget the left button holds
    GadgetRenderer gadget_renderer_ = nullptr;
    void* gadget_renderer_ctx_ = nullptr;
    GadgetRelease gadget_release_ = nullptr;
    GadgetPropCode gadget_prop_code_ = nullptr;
    void post_prop_message(native::Bus& b, u32 window, u32 idcmp_class, u32 gadget);
    // The window the left button drags, and the pointer's offset from its
    // top-left corner (Move) or to its bottom-right corner (Size), or from
    // the knob's top-left corner (Prop).
    Drag drag_ = Drag::None;
    u32 drag_window_ = 0;
    u32 drag_gadget_ = 0;
    MenuMode menu_;
    // The image an application drags over its screen, drawn above every
    // window: the screen, the Image and its top-left corner.
    struct IconDrag {
        u32 screen = 0;
        u32 image = 0;
        i32 x = 0;
        i32 y = 0;
    };
    IconDrag icon_drag_;
    i32 grab_x_ = 0;
    i32 grab_y_ = 0;
    i32 mouse_x_ = 0;
    i32 mouse_y_ = 0;
    u32 dropped_messages_ = 0;
    u32 batch_depth_ = 0;
    u32 screens_[kMaxScreens]{};
    u32 windows_[kMaxWindows]{};
    u32 messages_[kMaxMessages]{};
    u32 message_owners_[kMaxMessages]{};
    // The attribute list an IDCMP_IDCMPUPDATE's IAddress points at, and its
    // size, freed with the message.
    u32 message_updates_[kMaxMessages]{};
    u32 message_update_bytes_[kMaxMessages]{};
    ScreenRecord screen_records_[kMaxScreens]{};
    WindowRecord window_records_[kMaxWindows]{};
    Requester requesters_[kMaxRequesters]{};
    GadgetHookEntry gadget_hooks_[kMaxGadgetHooks]{};
    bool unhandled_gadget_types_[8]{};
    NativeVector vectors_[kVectors]{};
    Boopsi boopsi_;
};

}
