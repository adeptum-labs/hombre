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
#include "dos.hpp"
#include "exec.hpp"
#include "gadtools.hpp"
#include "graphics.hpp"
#include "icon.hpp"
#include "intuition.hpp"
#include "native.hpp"
#include "version.hpp"

// workbench.library written in C++ for code run by the 68000 translator.
//
// StartWorkbench starts the Workbench as a native task with a dos Process
// of its own: a root window on the default screen with the icons of the
// disks in the floppy drives and of the Ram Disk, and a drawer window for
// every icon opened. The root window is a backdrop until the Workbench menu's Backdrop
// item turns it into an ordinary window. A drawer shows the "<name>.info"
// files of its directory; opening an icon starts its program with a
// WBStartup message. An icon dragged within its window takes the new
// place; dropped on another drawer, or a drawer's or disk's icon, its file
// moves there on the same volume and is copied to another. A window with
// borders scrolls over its icons with the sliders and arrows in its right
// and bottom borders. Snapshot keeps an icon's place, or a drawer window's
// box and scroll, in the .info. The other
// vectors register application windows, icons and menu items, or rescan a
// drawer. WBConfig, IPrefs' private call, hands over the icon and system
// fonts and makes the Workbench read the backdrop pattern prefs again.

namespace hombre {

// Library vector offsets of workbench.library, n of LVO -n, as the 3.1
// function description numbers them.
namespace workbench_lvo {

constexpr u32 UpdateWorkbench = 30;
constexpr u32 QuoteWorkbench = 36;
constexpr u32 StartWorkbench = 42;
constexpr u32 AddAppWindowA = 48;
constexpr u32 RemoveAppWindow = 54;
constexpr u32 AddAppIconA = 60;
constexpr u32 RemoveAppIcon = 66;
constexpr u32 AddAppMenuItemA = 72;
constexpr u32 RemoveAppMenuItem = 78;
// Private: IPrefs hands the Workbench its settings, a kind in D0 and its
// data in D1.
constexpr u32 WBConfig = 84;
constexpr u32 WBInfo = 90;
// The last vector of the library; the table reaches down to it.
constexpr u32 Last = 90;

}

class Workbench {
public:
    static constexpr u32 kMaxWindows = 8;  // the backdrop and seven drawers
    static constexpr u32 kMaxIcons = 96;
    static constexpr u32 kMaxTools = 8;
    static constexpr u32 kMaxApps = 32;
    static constexpr u32 kMaxDirEntries = 64;
    // The most messages of one window a pass over the ports handles.
    static constexpr u32 kMaxMessagesPerPump = 64;
    static constexpr u32 kNoIcon = 0xFFFFFFFF;
    static constexpr u8 kNoDrive = 0xFF;
    static constexpr u32 kAppBytes = 16;
    // Two clicks on the same icon within this many microseconds open it:
    // a second, so an unhurried double click still counts.
    static constexpr u64 kDoubleClickUs = 1000000;
    // The title bar of the desktop is looked at again this often.
    static constexpr u64 kRefreshUs = 3000000;

    // workbench/workbench.h object types.
    static constexpr u8 kDisk = 1;
    static constexpr u8 kDrawer = 2;
    static constexpr u8 kTool = 3;
    static constexpr u8 kProject = 4;
    static constexpr u8 kGarbage = 5;
    static constexpr u8 kAppIcon = 8;

    // One icon of a window: the DiskObject it was built from, the label
    // under it and the place it took.
    struct IconEntry {
        bool used = false;
        bool placed = false;  // left and top are the icon's own position
        bool owned = false;   // the DiskObject is this library's to free
        u8 type = 0;
        u8 owner = 0;  // the window the icon belongs to
        u32 object = 0;
        i32 left = 0;
        i32 top = 0;
        i32 width = 0;   // the gadget's box
        i32 height = 0;
        u32 stack_size = 0;
        u32 tool_window = 0;  // the guest string of the DiskObject
        char name[32]{};      // the label under the icon
        char file[32]{};      // the file the icon stands for
        // A floppy's disk icon: the drive it is in; kNoDrive otherwise.
        u8 drive = kNoDrive;
    };

    // An open window of the desktop: the root window (index 0), a backdrop
    // or an ordinary window, or a drawer. clip is a Region of its inner
    // area that the Workbench's drawing stays inside, so icons past the
    // edges of a sized window are cut off rather than moved; the window's
    // scroll gadgets bring them into view.
    struct DrawerWindow {
        bool used = false;
        u32 window = 0;
        u32 user_port = 0;
        u32 rp = 0;
        u32 clip = 0;
        i32 inner_left = 0;
        i32 inner_top = 0;
        i32 inner_width = 0;
        i32 inner_height = 0;
        char name[32]{};
        char path[Dos::kMaxPath]{};
        // The drawer point shown at the inner area's top-left corner.
        i32 scroll_x = 0;
        i32 scroll_y = 0;
        // The block of its scroll gadgets, the window's gadget list; 0 for
        // a backdrop, which has none.
        u32 gadgets = 0;
        // Its place in the order the desktop opened the windows, so a
        // reset reopens it in its turn.
        u32 order = 0;
    };

    // The scroll gadgets of a window, by their GadgetID: the two sliders
    // in the right and bottom borders and the arrows beside them.
    static constexpr u16 kScrollVert = 1;
    static constexpr u16 kScrollHoriz = 2;
    static constexpr u16 kScrollUp = 3;
    static constexpr u16 kScrollDown = 4;
    static constexpr u16 kScrollLeft = 5;
    static constexpr u16 kScrollRight = 6;
    static constexpr u32 kScrollGadgets = 6;
    // How far one click on an arrow moves the view.
    static constexpr i32 kScrollStep = 8;

    // The box the icons of a window take, in drawer coordinates: the
    // origin, every icon with its label, and a margin past the right and
    // bottom ones. max is one past the last point.
    struct Extent {
        i32 min_x = 0;
        i32 min_y = 0;
        i32 max_x = 0;
        i32 max_y = 0;
    };

    // What a click did to the icons of its window.
    enum class ClickAction : u8 { None = 0, Select = 1, Clear = 2, Open = 3 };

    // The state of the mouse between clicks: the second click on the same
    // icon within kDoubleClickUs opens it; a click on empty space clears
    // the selection.
    class Clicks {
    public:
        ClickAction click(u32 icon, u64 now_us);
        u32 selected() const { return selected_; }
        void clear() { selected_ = kNoIcon; }
        // The last click starts no double click: a drag ended it.
        void forget() { last_icon_ = kNoIcon; }
        void select(u32 icon) {
            selected_ = icon;
            last_icon_ = kNoIcon;
        }

    private:
        u32 selected_ = kNoIcon;
        u32 last_icon_ = kNoIcon;
        u64 last_us_ = 0;
    };

    // A program started with a WBStartup message, until the message comes
    // back.
    struct RunningTool {
        bool used = false;
        u32 message = 0;
        u32 message_bytes = 0;
        u32 process = 0;
        u32 seglist = 0;
        u32 locks[2]{};
        u32 lock_count = 0;
        char name[32]{};
    };

    // An icon held by the left button: armed by the press on it, active
    // once the pointer has moved further than kDragThreshold. grab is the
    // pointer's offset from the icon's corner; the start is on the screen.
    struct IconDrag {
        bool armed = false;
        bool active = false;
        u32 window = 0;
        u32 icon = kNoIcon;
        i32 grab_x = 0;
        i32 grab_y = 0;
        i32 start_x = 0;
        i32 start_y = 0;
    };
    static constexpr i32 kDragThreshold = 2;

    enum class AppKind : u8 { Window = 1, Icon = 2, MenuItem = 3 };

    // One AddAppWindowA, AddAppIconA or AddAppMenuItemA registration.
    struct App {
        bool used = false;
        AppKind kind = AppKind::Window;
        u32 handle = 0;
        u32 id = 0;
        u32 user = 0;
        u32 icon = kNoIcon;  // the backdrop icon an AppIcon drew
    };

    // One argument of a WBStartup message: the drawer to lock and the file
    // name in it.
    struct StartupArg {
        const char* drawer;
        const char* name;
    };

    // Makes workbench.library and adds it to exec's library list. exec, dos
    // and icon must be initialised; graphics, intuition and gadtools must
    // be before a desktop is opened. False if exec runs out of memory or
    // native ids.
    bool init(m68k::Machine& m, Exec& exec, Dos& dos, Graphics& graphics, Intuition& intuition,
              GadTools& gadtools, Icon& icon);

    u32 base() const { return base_; }
    u32 process() const { return process_; }
    u32 reply_port() const { return reply_port_; }
    bool running() const { return running_; }
    bool desktop_open() const { return desktop_open_; }
    bool backdrop() const { return backdrop_; }
    u32 menu() const { return menu_; }
    const DrawerWindow& window(u32 index) const { return windows_[index]; }
    const IconEntry& icon(u32 index) const { return icons_[index]; }
    const RunningTool& tool(u32 index) const { return tools_[index]; }
    const App& app(u32 index) const { return apps_[index]; }
    const IconDrag& drag() const { return drag_; }
    // The TextFont of the icon text font and of the system font IPrefs
    // passed over; 0 is the built-in font.
    u32 icon_font() const;
    u32 system_font() const;

    // The window's scroll gadget with the GadgetID id; 0 when it has none.
    u32 scroll_gadget(u32 window_index, u16 id) const;
    Extent extent(u32 window_index) const;

    // StartWorkbench: starts the Workbench as a detached task with a
    // Process of its own and returns DOSTRUE. A call while it runs returns
    // DOSTRUE without a second desktop.
    u32 start(u32 flags, u32 path);

    // Builds the desktop on the default screen: the menu strip, the
    // backdrop with the volume icons, and the tools of SYS:WBStartup. The
    // task calls it first; the tests call it themselves.
    void open_desktop();

    // Rescans the drawer window whose path or title is name. False when no
    // open window matches.
    bool update_workbench(u32 name);

    // CloseWorkBench: remembers every drawer's place and closes the
    // drawers and the root window, dropping the drag and the selection.
    // False when no desktop is open.
    bool close_for_reset();

    // OpenWorkBench: opens the root window again and the drawers
    // close_for_reset remembered, at their boxes and in the order they
    // were opened.
    void open_after_reset();

    // The application registration vectors. Every Add hands out a handle
    // the matching Remove takes back; a Remove is false for a handle no
    // Add of its kind handed out.
    u32 add_app_window(u32 id, u32 user, u32 window, u32 msg_port, u32 tags);
    bool remove_app_window(u32 handle);
    u32 add_app_icon(u32 id, u32 user, u32 name, u32 msg_port, u32 lock, u32 object, u32 tags);
    bool remove_app_icon(u32 handle);
    u32 add_app_menu_item(u32 id, u32 user, u32 text, u32 msg_port, u32 tags);
    bool remove_app_menu_item(u32 handle);
    // WBConfig of the kind in D0 and its data in D1: the icon and system
    // font kinds keep the struct and give back the one they replace, 0 the
    // first time; the backdrop kinds wake the Workbench to read the pattern
    // prefs again.
    u32 wb_config(u32 kind, u32 data);

    // Builds the WBStartup message posted to a started process: its
    // ReplyPort is the Workbench's port, and each argument gets the lock of
    // its drawer. locks receives those locks and bytes the size to free the
    // message with. 0 when memory runs out or a lock cannot be had, with
    // lock_count 0.
    u32 build_startup(u32 process, u32 seglist, const StartupArg* args, u32 nargs,
                      const char* tool_window, u32* locks, u32& lock_count, u32& bytes);

    // Opens the window the icon of window window_index stands for, or
    // brings the window already open for it to the front.
    void open_drawer(u32 window_index, const IconEntry& icon);

    // Closes a drawer's window and gives its icons back; the backdrop
    // stays.
    void close_drawer(u32 window_index);

    // Starts the tool or project the icon describes from drawer, with a
    // message it finds on its port. False, logged, when the program cannot
    // be loaded or started.
    bool launch(const IconEntry& icon, const char* drawer);
    bool start_tool(const IconEntry& icon, const char* drawer);
    bool start_project(const IconEntry& icon, const char* drawer);

    // What a double click on the icon does: its window, its program or its
    // project's program.
    void open_icon(u32 window_index, u32 icon_index);

    // The reply to a running tool's message: unloads its seglist, unlocks
    // the arguments' locks and frees the message. False for a message no
    // running tool sent.
    bool tool_reply(u32 message);

    // Does the same for every program whose process has ended without
    // replying its message, as one that faulted has.
    void reap_tools();

    // The Window menu's Open Parent: opens the drawer the window's drawer
    // lies in, or brings its window to the front. A disk's window has none.
    void open_parent(u32 window_index);

    // The Workbench menu's Backdrop: reopens the root window as a backdrop
    // behind every other window, or as an ordinary "Workbench" window with
    // a drag bar, depth and size gadgets. Its icons stay, placed again for
    // the new size, and the menu item's check mark follows.
    void set_backdrop(bool on);

    // Does what the menu item numbered number picks in the window.
    void menu_pick(u32 window_index, u32 number);

    // Icons > Snapshot: stores the icon's place in its .info. Unsnapshot
    // stores "no place" and puts it back on the grid. False, logged, when
    // the .info cannot be written; an AppIcon's object is left alone.
    bool snapshot_icon(u32 window_index, u32 icon_index);
    bool unsnapshot_icon(u32 window_index, u32 icon_index);

    // Window > Snapshot: stores the drawer window's place, size and scroll
    // in the icon of its drawer or disk, and with all every icon's place
    // too.
    bool snapshot_window(u32 window_index, bool all);

    // Handles the replies of ended programs and the messages waiting at
    // the windows' ports. True when a window had more messages than one
    // pass takes.
    bool pump();

    // The signals of the ports the desktop gets its messages at: the reply
    // port and every open window's.
    u32 wait_mask() const;

private:
    friend struct WorkbenchNatives;

    static constexpr u32 kVectors = workbench_lvo::Last / 6;
    static constexpr u32 kTaskStackBytes = 8192;

    // A drawer close_for_reset closed: how to find it again and where its
    // window stood.
    struct RememberedDrawer {
        char path[Dos::kMaxPath]{};
        char name[32]{};
        i32 left = 0;
        i32 top = 0;
        i32 width = 0;
        i32 height = 0;
        u32 order = 0;
    };

    // The box open_drawer_window opens a drawer at instead of the one its
    // icon keeps.
    struct DrawerBox {
        i32 left = 0;
        i32 top = 0;
        i32 width = 0;
        i32 height = 0;
    };

    static void task_entry(void* ctx);
    static void file_closed(void* ctx, const char* path);
    void wake_for_patterns();
    static bool hook_close(void* ctx);
    static void hook_open(void* ctx);
    void run();
    void handle_message(u32 window_index, u32 message);
    void show_about();
    void refresh_title();

    u32 build_menu_strip();
    void attach_window(u32 index, u32 window, const char* name, const char* path, u32 gadgets);
    void measure_window(u32 index);
    void release_window(u32 index);
    void open_drawer_window(const char* path, const char* name, u32 object,
                            const DrawerBox* box = nullptr);
    void release_tool(RunningTool& tool);
    void open_root();
    void place_root_icon(IconEntry& icon);
    void place_root_icons();
    void read_position(IconEntry& icon);
    u32 backdrop_item() const;
    void add_volume_icons();
    void add_dir_icon(u32 window_index, const char* entry, bool is_dir);
    void add_volume(const char* label, const char* icon_path, u8 drive);
    bool volume_root(const IconEntry& icon, char (&out)[Dos::kMaxPath]) const;
    void fill_drawer(u32 window_index);
    void place_icons(u32 window_index);
    void draw_window(u32 window_index);
    void paint_window(u32 window_index);
    bool load_patterns();
    void load_picture(u32 which, const char* name);
    void drop_picture(u32 which);
    void fill_pattern(const DrawerWindow& drawer, u32 which, i32 right, i32 bottom);
    u32 make_scroll_gadgets();
    bool build_arrow_images();
    void update_scroll(u32 window_index);
    void scroll_by(u32 window_index, i32 dx, i32 dy);
    void follow_sliders(u32 window_index);
    u16 scroll_gadget_id(u32 window_index, u32 address) const;
    void scroll_message(u32 window_index, u32 idcmp_class, u16 id);
    void draw_icon(const DrawerWindow& drawer, const IconEntry& icon, bool selected);
    Graphics::FontMetrics label_metrics() const;
    i32 label_span(const IconEntry& icon) const;
    void set_label_pens(u32 rp);
    void relayout_labels();
    void redraw_all();
    void update_all();
    void select_contents(u32 window_index);
    void clean_up(u32 window_index);
    void open_selected(u32 window_index);
    void mouse_click(u32 window_index, i32 mx, i32 my, u64 now_us);
    void drag_to(u32 window_index, i32 mx, i32 my);
    void end_drag(u32 window_index, i32 mx, i32 my);
    void cancel_drag();
    void drop(const IconDrag& drag, i32 sx, i32 sy);
    bool transfer(const IconEntry& icon, const char* from, const char* to, i32 left, i32 top);
    bool icon_path(u32 window_index, const IconEntry& icon, char (&out)[Dos::kMaxPath]) const;
    bool icon_target_path(u32 window_index, const IconEntry& icon,
                          char (&out)[Dos::kMaxPath]) const;
    bool screen_origin(u32 window_index, i32& x, i32& y) const;
    void show_error(const char* what, const char* name, u32 error);
    void show_requester(const char* title, const char* text);
    u32 icon_at(u32 window_index, i32 x, i32 y) const;
    void start_wbstartup();

    IconEntry* take_icon(u32 owner);
    void free_icons(u32 owner);
    void measure(IconEntry& icon);
    u32 free_window_slot() const;
    bool split_tool(const char* reference, const char* drawer, char (&dir)[Dos::kMaxPath],
                    char (&name)[Dos::kMaxPath]) const;
    bool start_program(const char* name, u32 seglist, const char* current_dir,
                       const StartupArg* args, u32 nargs, const char* tool_window,
                       u32 stack_size);
    u32 record_app(AppKind kind, u32 id, u32 user);
    App* find_app(u32 handle, AppKind kind);
    void drop_app_icon(const App& app);

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    Dos* dos_ = nullptr;
    Graphics* graphics_ = nullptr;
    Intuition* intuition_ = nullptr;
    GadTools* gadtools_ = nullptr;
    Icon* icon_ = nullptr;
    u32 base_ = 0;
    u32 process_ = 0;
    u32 screen_ = 0;
    u32 reply_port_ = 0;
    u32 menu_ = 0;
    u32 title_buffer_ = 0;
    u32 arrows_ = 0;  // the arrow gadgets' images, made once
    // The structs IPrefs passed over for the icon text font and the system
    // font: a TextAttr, a TextFont, a draw mode, a front and a back pen,
    // and a TextAttr and name of their own. IPrefs frees the ones a
    // WBConfig hands back.
    u32 icon_font_ = 0;
    u32 system_font_config_ = 0;
    // The backdrops of ENV:Sys/WBPattern.prefs, the root window's and the
    // drawers': a BitMap of up to eight planes to tile them from, a 16 by
    // 16 pixel pattern or a picture as large as it is; depth 0 for none.
    // A picture's BitMap is a block of picture_bytes of its own.
    // pattern_sum_ tells when the file changed.
    struct Pattern {
        u32 bitmap = 0;
        u32 depth = 0;
        i32 width = 16;
        i32 height = 16;
        u32 picture = 0;
        u32 picture_bytes = 0;
    };
    Pattern patterns_[2]{};
    u32 pattern_sum_ = 0;
    i32 volume_top_ = 0;
    u64 next_refresh_us_ = 0;
    bool patterns_changed_ = false;  // WBPattern's prefs were written
    bool reopen_pending_ = false;  // OpenWorkBench asked for the windows back
    bool running_ = false;
    bool desktop_open_ = false;
    bool backdrop_ = true;
    IconEntry icons_[kMaxIcons]{};
    DrawerWindow windows_[kMaxWindows]{};
    RememberedDrawer remembered_[kMaxWindows - 1]{};
    u32 remembered_count_ = 0;
    u32 open_count_ = 0;
    RunningTool tools_[kMaxTools]{};
    App apps_[kMaxApps]{};
    Clicks clicks_[kMaxWindows]{};
    IconDrag drag_{};
    NativeVector vectors_[kVectors]{};
};

}
