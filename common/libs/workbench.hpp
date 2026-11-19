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
// of its own: a backdrop window on the default screen with the volume
// icons of the boot volume and the Ram Disk, and a drawer window for every
// icon opened. A drawer shows the "<name>.info" files of its directory;
// opening an icon starts its program with a WBStartup message. The other
// vectors register application windows, icons and menu items, or rescan a
// drawer. The vector gap at LVO -84 logs "workbench: unimplemented LVO
// -<n>" and ends the run with Unimplemented.

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
    static constexpr u32 kNoIcon = 0xFFFFFFFF;
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
    };

    // An open window of the desktop: the backdrop (index 0) or a drawer.
    struct DrawerWindow {
        bool used = false;
        u32 window = 0;
        u32 parent = 0;  // the window the drawer was opened from
        u32 user_port = 0;
        u32 rp = 0;
        i32 inner_left = 0;
        i32 inner_top = 0;
        i32 inner_width = 0;
        i32 inner_height = 0;
        char name[32]{};
        char path[Dos::kMaxPath]{};
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
    u32 menu() const { return menu_; }
    const DrawerWindow& window(u32 index) const { return windows_[index]; }
    const IconEntry& icon(u32 index) const { return icons_[index]; }
    const RunningTool& tool(u32 index) const { return tools_[index]; }
    const App& app(u32 index) const { return apps_[index]; }

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

    // The application registration vectors. Every Add hands out a handle
    // the matching Remove takes back.
    u32 add_app_window(u32 id, u32 user, u32 window, u32 tags);
    void remove_app_window(u32 handle);
    u32 add_app_icon(u32 id, u32 user, u32 name, u32 msg_port, u32 lock, u32 object, u32 tags);
    void remove_app_icon(u32 handle);
    u32 add_app_menu_item(u32 id, u32 user, u32 text, u32 msg_port, u32 tags);
    void remove_app_menu_item(u32 handle);

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

private:
    friend struct WorkbenchNatives;

    static constexpr u32 kVectors = workbench_lvo::Last / 6;
    static constexpr u32 kTaskStackBytes = 8192;

    static void task_entry(void* ctx);
    void run();
    void pump();
    void handle_message(u32 window_index, u32 message);
    void menu_pick(u32 window_index, u32 number);
    void refresh_title();

    u32 build_menu_strip();
    void attach_window(u32 index, u32 window, u32 parent, const char* name, const char* path);
    void open_backdrop();
    void add_volume_icons();
    void add_dir_icon(u32 window_index, const char* entry, bool is_dir);
    void add_volume(const char* label, const char* icon_path);
    void close_drawer(u32 window_index);
    void fill_drawer(u32 window_index);
    void place_icons(u32 window_index);
    void draw_window(u32 window_index);
    void draw_icon(const DrawerWindow& drawer, const IconEntry& icon, bool selected);
    void redraw_all();
    void update_all();
    void open_parent(u32 window_index);
    void select_contents(u32 window_index);
    void clean_up(u32 window_index);
    void open_selected(u32 window_index);
    void mouse_click(u32 window_index, i32 mx, i32 my, u64 now_us);
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
    i32 volume_top_ = 0;
    u64 next_refresh_us_ = 0;
    bool running_ = false;
    bool desktop_open_ = false;
    IconEntry icons_[kMaxIcons]{};
    DrawerWindow windows_[kMaxWindows]{};
    RunningTool tools_[kMaxTools]{};
    App apps_[kMaxApps]{};
    Clicks clicks_[kMaxWindows]{};
    NativeVector vectors_[kVectors]{};
};

}
