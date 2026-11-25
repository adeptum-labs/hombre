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
#include "../slot_list.hpp"
#include "../types.hpp"
#include "dos.hpp"
#include "exec.hpp"
#include "gadtools.hpp"
#include "graphics.hpp"
#include "icon.hpp"
#include "intuition.hpp"
#include "native.hpp"
#include "version.hpp"

#include <vector>

// workbench.library written in C++ for code run by the 68000 translator.
//
// StartWorkbench starts the Workbench as a native task with a dos Process
// of its own: a root window on the default screen with the icons of the
// disks in the floppy drives and hard disk partitions and of the Ram Disk,
// and a drawer window for every icon opened. The root window is a backdrop
// until the Workbench menu's Backdrop item turns it into an ordinary window.
// A drawer shows the "<name>.info" files of its directory; opening an icon
// starts its program with a WBStartup message. An icon dragged within its
// window takes the new place; dropped on another drawer, or a drawer's or
// disk's icon, its file moves there on the same volume and is copied to
// another. A window with
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
    static constexpr u32 kMaxWindows = 64;  // the root window and 63 drawers
    static constexpr u32 kMaxIcons = 8192;
    static constexpr u32 kMaxTools = 256;
    static constexpr u32 kMaxApps = 1024;
    static constexpr u32 kMaxDirEntries = 64;
    // The most objects a drawer to copy or delete may hold.
    static constexpr u32 kMaxTreeEntries = 8192;
    // The most window messages a pass over the ports handles.
    static constexpr u32 kMaxMessagesPerPump = 64;
    static constexpr u32 kNoIcon = 0xFFFFFFFF;
    static constexpr u32 kNoWindow = 0xFFFFFFFF;
    // The longest name a file can have on an Amiga disk.
    static constexpr u32 kMaxName = 30;
    // The most bytes Execute Command takes, the ending NUL included.
    static constexpr u32 kMaxCommand = 256;
    // The id of the root window; the id of every drawer is above it.
    static constexpr u32 kRootWindow = 0;
    static constexpr u8 kNoDrive = 0xFF;
    static constexpr u32 kAppBytes = 16;
    // Two clicks on the same icon within this many microseconds open it:
    // a second, so an unhurried double click still counts.
    static constexpr u64 kDoubleClickUs = 1000000;
    // The title bar of the desktop is looked at again this often.
    static constexpr u64 kRefreshUs = 3000000;
    static constexpr u64 kVolumeCheckUs = 1000000;

    // workbench/workbench.h object types.
    static constexpr u8 kDisk = 1;
    static constexpr u8 kDrawer = 2;
    static constexpr u8 kTool = 3;
    static constexpr u8 kProject = 4;
    static constexpr u8 kGarbage = 5;
    static constexpr u8 kAppIcon = 8;

    // One icon of a window: the DiskObject it was built from, the label
    // under it and the place it took. Its id names it for as long as it
    // lives: ids count up from 1 and are not given out again, so the id of
    // an icon that is gone names no icon.
    struct IconEntry {
        u32 id = 0;
        bool placed = false;  // left and top are the icon's own position
        bool owned = false;   // the DiskObject is this library's to free
        u8 type = 0;
        u32 owner = 0;  // the id of the window the icon belongs to
        u32 object = 0;
        i32 left = 0;
        i32 top = 0;
        i32 width = 0;   // the gadget's box
        i32 height = 0;
        u32 stack_size = 0;
        u32 tool_window = 0;  // the guest string of the DiskObject
        char name[32]{};      // the label under the icon
        char file[32]{};      // the file the icon stands for
        // A disk icon of a floppy or a hard disk partition: the drive it is
        // in; kNoDrive otherwise.
        u8 drive = kNoDrive;
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

    // An open window of the desktop: the root window (kRootWindow), a
    // backdrop or an ordinary window, or a drawer. clip is a Region of its
    // inner area that the Workbench's drawing stays inside, so icons past the
    // edges of a sized window are cut off rather than moved; the window's
    // scroll gadgets bring them into view.
    struct DrawerWindow {
        // kRootWindow, or a drawer's place in the order the desktop opened
        // the windows, so a reset reopens it in its turn.
        u32 id = 0;
        u32 window = 0;
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
        // The fuel gauge of a volume window: its gadget and the image it
        // shows, the memory of which may hold more rows than the image
        // has, and the used blocks and the share of the volume that image
        // was drawn for.
        u32 gauge = 0;
        u32 gauge_image = 0;
        u32 gauge_bytes = 0;
        i32 gauge_height = 0;
        u32 gauge_depth = 0;
        u32 gauge_used = 0;
        u32 gauge_total = 0;
        u32 gauge_percent = 0;
        Clicks clicks;
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
    // How far a notch of the mouse wheel scrolls a drawer.
    static constexpr i32 kWheelStep = 3 * kScrollStep;

    // The box the icons of a window take, in drawer coordinates: the
    // origin, every icon with its label, and a margin past the right and
    // bottom ones. max is one past the last point.
    struct Extent {
        i32 min_x = 0;
        i32 min_y = 0;
        i32 max_x = 0;
        i32 max_y = 0;
    };

    // A program started with a WBStartup message, until the message comes
    // back.
    struct RunningTool {
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
    // The one port the IDCMP messages of every window of the desktop come to.
    u32 window_port() const { return window_port_; }
    bool running() const { return running_; }
    bool desktop_open() const { return desktop_open_; }
    bool backdrop() const { return backdrop_; }
    u32 menu() const { return menu_; }
    bool has_window(u32 id) const { return find_window(id) != nullptr; }
    // The window or icon with that id, which must exist.
    const DrawerWindow& window(u32 id) const { return *find_window(id); }
    const IconEntry& icon(u32 id) const { return *find_icon(id); }
    // The open windows, the icons, the running programs and the
    // registrations, each in the order it was added.
    const SlotList<DrawerWindow>& windows() const { return windows_; }
    const SlotList<IconEntry>& icons() const { return icons_; }
    const SlotList<RunningTool>& tools() const { return tools_; }
    const SlotList<App>& apps() const { return apps_; }
    const IconDrag& drag() const { return drag_; }
    // The TextFont of the icon text font and of the system font IPrefs
    // passed over; 0 is the built-in font.
    u32 icon_font() const;
    u32 system_font() const;

    // Answers confirm and ask_text in place of the user: text holds the
    // initial line of ask_text and takes the entered one, and is null for
    // confirm. False cancels.
    using Answer = bool (*)(void* ctx, const char* title, const char* body, char* text,
                            u32 size);
    void set_answer(Answer answer, void* ctx) {
        answer_ = answer;
        answer_ctx_ = ctx;
    }

    // Starts a command line in a shell of its own, as Execute Command does;
    // false when no shell can be opened.
    using RunCommand = bool (*)(void* ctx, const char* command);
    void set_run_command(RunCommand run, void* ctx) {
        run_command_ = run;
        run_command_ctx_ = ctx;
    }

    // Workbench > Execute Command: runs the command line. False, shown, when
    // no shell can run it, and for an empty line.
    bool execute_command(const char* command);

    // A requester with Ok and Cancel buttons; true for Ok.
    bool confirm(const char* title, const char* text);

    // Asks for a line of text, starting from initial; true for Ok or
    // Return, with the line in out.
    bool ask_text(const char* title, const char* prompt, const char* initial, char* out,
                  u32 size);

    // A modal window of GadTools gadgets, apart from the loop that waits at
    // it so that its input can be fed one step at a time.
    struct Dialog {
        u32 window = 0;
        u32 gadgets = 0;  // the first of its gadget list
        u32 block = 0;    // the strings and structs the gadgets keep
        u32 block_bytes = 0;
    };
    static constexpr u32 kMaxToolTypes = 64;
    static constexpr u8 kNewToolType = 0xFF;
    static constexpr u32 kMaxToolTypeLength = 127;
    static constexpr u32 kMaxDefaultToolLength = 255;
    static constexpr u32 kNoToolType = 0xFFFFFFFF;
    static constexpr u16 kDialogAccept = 2;
    static constexpr u16 kDialogCancel = 3;
    enum class Reply : u8 { Waiting, Accepted, Cancelled };
    void close_dialog(Dialog& dialog);

    // The window of ask_text.
    struct TextRequest : Dialog {
        u32 string = 0;
    };
    static constexpr u16 kRequestString = 1;
    static constexpr u16 kRequestOk = kDialogAccept;
    static constexpr u16 kRequestCancel = kDialogCancel;
    bool open_text_request(const char* title, const char* prompt, const char* initial, u32 size,
                           TextRequest& request);
    // Handles the input waiting at the request's window.
    Reply text_request_input(const TextRequest& request, char* out, u32 size);

    // The window of Icons > Information: the name and type of what the icon
    // stands for, the size and blocks of a file, its date, a check box for
    // each protection bit it has one for, its comment, and the icon's own
    // stack, default tool and tool types. path finds the object again when
    // it is saved, name names it in an error; protection, comment_text and
    // the original_ fields are what the window opened with. has_object is
    // false for an icon that stands for nothing but its .info.
    struct InfoRequest : Dialog {
        u32 comment_gadget = 0;
        u32 stack_gadget = 0;
        u32 default_tool_gadget = 0;
        u32 tool_types_gadget = 0;  // the list view of the rows
        u32 tool_gadget = 0;        // the string editing the selected row
        u32 list = 0;               // the exec List of the rows, in extra
        u32 rows = 0;               // the row strings, in extra
        u32 default_text = 0;       // the default tool string, in extra
        u32 original_default = 0;   // the default tool as it opened, in extra
        u32 tags = 0;               // the tag list of gadget changes, in extra
        u32 empty_text = 0;
        u32 window_id = 0;
        u32 row_count = 0;
        u32 original_row_count = 0;
        u32 original_stack = 0;
        u32 selected = kNoToolType;
        char path[Dos::kMaxPath]{};
        char name[32]{};
        char comment_text[Dos::kMaxComment + 1]{};
        u32 protection = 0;
        bool has_object = false;
        bool show_stack = false;
        bool show_default_tool = false;
        bool show_tool_types = false;
        // The tool type of the .info each row is, kNewToolType for a row
        // that was added or changed.
        u8 origin[kMaxToolTypes]{};
    };
    static constexpr u16 kInfoComment = 1;
    static constexpr u16 kInfoSave = kDialogAccept;
    static constexpr u16 kInfoCancel = kDialogCancel;
    static constexpr u16 kInfoScript = 4;
    static constexpr u16 kInfoArchived = 5;
    static constexpr u16 kInfoReadable = 6;
    static constexpr u16 kInfoWritable = 7;
    static constexpr u16 kInfoExecutable = 8;
    static constexpr u16 kInfoDeletable = 9;
    static constexpr u16 kInfoName = 10;
    static constexpr u16 kInfoType = 11;
    static constexpr u16 kInfoSize = 12;
    static constexpr u16 kInfoDate = 13;
    static constexpr u16 kInfoStack = 14;
    static constexpr u16 kInfoDefaultTool = 15;
    static constexpr u16 kInfoToolTypes = 16;
    static constexpr u16 kInfoTool = 17;
    static constexpr u16 kInfoNew = 18;
    static constexpr u16 kInfoDel = 19;
    static constexpr u16 kInfoBlocks = 20;
    bool open_info(u32 window_id, u32 icon_id, InfoRequest& request);
    // Handles the input waiting at the information window: Accepted for
    // Save, Cancelled for Cancel or the close gadget.
    Reply info_input(InfoRequest& request);
    // Writes the protection bits and the comment that changed. False, shown,
    // when one cannot be written; nothing is written for an icon without an
    // object or when nothing changed.
    bool save_info(InfoRequest& request);
    // The Icons > Information menu item: opens the window, waits for Save or
    // Cancel, saves on Save and closes it. A disk's icon is refused.
    void show_information(u32 window_id, u32 icon_id);

    // The window's scroll gadget with the GadgetID id; 0 when it has none.
    u32 scroll_gadget(u32 window_id, u16 id) const;
    Extent extent(u32 window_id) const;

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

    // Icons > Delete: deletes what the icon stands for, a drawer with
    // everything in it, and the icon's .info, closing the windows of what
    // goes. False, shown, for a disk or an AppIcon and when something
    // cannot be deleted.
    bool delete_icon(u32 window_id, u32 icon_id);

    // Icons > Rename: gives what the icon stands for and its .info the name
    // new_name, closing the windows of a renamed drawer. False, shown, for a
    // disk or an AppIcon and for a name that cannot be had.
    bool rename_icon(u32 window_id, u32 icon_id, const char* new_name);

    // Window > New Drawer: makes the drawer name with its icon in the
    // window's drawer. False, shown, when either cannot be made and for the
    // root window.
    bool new_drawer(u32 window_id, const char* name);

    // Icons > Copy: duplicates what the icon stands for, a drawer with
    // everything in it, and its .info under a "copy_of_" name in the same
    // drawer. False, shown, for a disk or an AppIcon and when something
    // cannot be copied, which leaves no part of the copy.
    bool copy_icon(u32 window_id, u32 icon_id);

    // Icons > Leave Out: shows the icon of the drawer window on the root
    // window too and lists it in its volume's .backdrop file, which brings
    // it back with the volume. False, shown, for a disk or an AppIcon and
    // when the file cannot be written.
    bool leave_out(u32 window_id, u32 icon_id);

    // Icons > Put Away: takes a left out icon off the root window and out
    // of its volume's .backdrop. False for any other icon and, shown, when
    // the file cannot be written.
    bool put_away(u32 icon_id);

    // Icons > Empty Trash: deletes everything in the Trashcan the icon
    // stands for. False for any other icon and, shown, when something
    // cannot be deleted.
    bool empty_trash(u32 window_id, u32 icon_id);

    // Workbench > Quit: closes the desktop for good, which ends its task.
    // False, shown, while programs it started run or have application
    // icons, windows or menu items, and when no desktop is open.
    bool quit();
    bool quit_pending() const { return quit_pending_; }

    // The error shown last, which Workbench > Last Message shows again.
    const char* last_message() const { return last_message_; }

    // Opens the window the icon of window window_id stands for, or
    // brings the window already open for it to the front.
    void open_drawer(u32 window_id, const IconEntry& icon);

    // Closes a drawer's window and gives its icons back; the backdrop
    // stays.
    void close_drawer(u32 window_id);

    // Starts the tool or project the icon describes from drawer, with a
    // message it finds on its port. False, logged, when the program cannot
    // be loaded or started.
    bool launch(const IconEntry& icon, const char* drawer);
    bool start_tool(const IconEntry& icon, const char* drawer);
    bool start_project(const IconEntry& icon, const char* drawer);

    // What a double click on the icon does: its window, its program or its
    // project's program.
    void open_icon(u32 window_id, u32 icon_id);

    // The reply to a running tool's message: unloads its seglist, unlocks
    // the arguments' locks and frees the message. False for a message no
    // running tool sent.
    bool tool_reply(u32 message);

    // Does the same for every program whose process has ended without
    // replying its message, as one that faulted has.
    void reap_tools();

    // The Window menu's Open Parent: opens the drawer the window's drawer
    // lies in, or brings its window to the front. A disk's window has none.
    void open_parent(u32 window_id);

    // The Workbench menu's Backdrop: reopens the root window as a backdrop
    // behind every other window, or as an ordinary "Workbench" window with
    // a drag bar, depth and size gadgets. Its icons stay, placed again for
    // the new size, and the menu item's check mark follows.
    void set_backdrop(bool on);

    // Does what the menu item numbered number picks in the window.
    void menu_pick(u32 window_id, u32 number);

    // Icons > Snapshot: stores the icon's place in its .info. Unsnapshot
    // stores "no place" and puts it back on the grid. False, logged, when
    // the .info cannot be written; an AppIcon's object is left alone.
    bool snapshot_icon(u32 window_id, u32 icon_id);
    bool unsnapshot_icon(u32 window_id, u32 icon_id);

    // Window > Snapshot: stores the drawer window's place, size and scroll
    // in the icon of its drawer or disk, and with all every icon's place
    // too.
    bool snapshot_window(u32 window_id, bool all);

    // Handles the replies of ended programs and the messages waiting at
    // the windows' port. True when the port had more messages than one
    // pass takes.
    bool pump();

    // The signals of the ports the desktop gets its messages at: the reply
    // port and the windows' port.
    u32 wait_mask() const;

private:
    friend struct WorkbenchNatives;

    static constexpr u32 kVectors = workbench_lvo::Last / 6;
    static constexpr u32 kTaskStackBytes = 8192;

    // An object of a drawer tree.
    struct TreeEntry {
        char path[Dos::kMaxPath]{};
        bool is_dir = false;
    };

    // The drawer of a left out icon, which the root window shows.
    struct LeftOut {
        u32 icon = 0;
        char drawer[Dos::kMaxPath]{};
    };

    const LeftOut* left_out(u32 icon_id) const;
    // The drawer the icon's file is in: its window's, or the one a left out
    // icon came from.
    const char* drawer_of(const DrawerWindow& window, const IconEntry& icon) const;
    // The .backdrop of the volume path is on and the line that names path
    // in it.
    bool backdrop_entry(const char* path, char (&file)[Dos::kMaxPath],
                        char (&line)[Dos::kMaxPath]) const;
    // Rewrites the .backdrop file with or without the line.
    bool set_backdrop_line(const char* file, const char* line, bool listed, u32& error);
    // Shows the icon of the object file of drawer on the root window,
    // unless it is there.
    bool add_left_out(const char* drawer, const char* file);
    // The icons the .backdrop of the volume "Name:" lists.
    void add_left_outs(const char* volume);
    void remove_root_icon(u32 icon_id);
    // Follows a file or drawer that went from from to to with its left out
    // icons; a null to drops them.
    void move_left_outs(const char* from, const char* to);

    // A drawer close_for_reset closed: how to find it again and where its
    // window stood.
    struct RememberedDrawer {
        char path[Dos::kMaxPath]{};
        char name[32]{};
        i32 left = 0;
        i32 top = 0;
        i32 width = 0;
        i32 height = 0;
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
    void handle_message(u32 message);
    void show_about();
    void refresh_title();

public:
    // Makes the disk icons follow the disks in the drives: the icon of a
    // disk that is gone or replaced goes, with its open windows, and a disk
    // without an icon gets one. The desktop does it a second.
    void refresh_volumes();

private:

    u32 build_menu_strip();
    DrawerWindow& attach_window(u32 id, u32 window, const char* name, const char* path,
                                u32 gadgets);
    void measure_window(DrawerWindow& w);
    void release_window(DrawerWindow& w);
    u32 open_window_on_port(u32 new_window, u32 idcmp, const char* zoom_title = nullptr);
    u32 make_zoom_tags(u32 new_window, const char* title);
    void open_drawer_window(const char* path, const char* name, u32 object,
                            const DrawerBox* box = nullptr);
    void release_tool(RunningTool& tool);
    void open_root();
    void place_root_icon(const DrawerWindow& root, IconEntry& icon);
    void place_root_icons(const DrawerWindow& root);
    void read_position(IconEntry& icon);
    u32 backdrop_item() const;
    void add_volume_icons();
    void add_dir_icon(const DrawerWindow& drawer, const char* entry, bool is_dir);
    void add_volume(const char* label, const char* icon_path, u8 drive);
    void add_drive_volume(u32 drive);
    bool volume_root(const IconEntry& icon, char (&out)[Dos::kMaxPath]) const;
    void fill_drawer(DrawerWindow& drawer);
    void place_icons(const DrawerWindow& drawer);
    void draw_window(u32 window_id);
    void paint_window(const DrawerWindow& drawer);
    bool load_patterns();
    void load_picture(u32 which, const char* name);
    void drop_picture(u32 which);
    void fill_pattern(const DrawerWindow& drawer, u32 which, i32 right, i32 bottom);
    u32 make_scroll_gadgets();
    u32 make_volume_gauge(u32 gadgets, i32 top, i32 height);
    void layout_gauge(DrawerWindow& w);
    u32 pen_depth(native::Bus& b) const;
    bool fit_gauge(DrawerWindow& w);
    void fill_gauge_image(DrawerWindow& w);
    static void gauge_sized(void* ctx, u32 window);
    void refresh_gauges();
    bool build_arrow_images();
    void paint_arrow_images();
    void update_scroll(DrawerWindow& w);
    void scroll_by(u32 window_id, i32 dx, i32 dy);
    void follow_sliders(u32 window_id);
    u16 scroll_gadget_id(u32 window_id, u32 address) const;
    void scroll_message(u32 window_id, u32 idcmp_class, u16 id);
    void draw_icon(const DrawerWindow& drawer, const IconEntry& icon, bool selected);
    Graphics::FontMetrics label_metrics() const;
    i32 label_span(const IconEntry& icon) const;
    void set_label_pens(u32 rp);
    void relayout_labels();
    void redraw_all();
    void update_all();
    // Reads a drawer window's directory again; the root window stays.
    void update_window(DrawerWindow& drawer);
    // The object at path and, for a drawer, everything below it, each
    // drawer before what is in it. Nothing there leaves out empty. False,
    // with error set, when a drawer cannot be read.
    bool collect_tree(const char* path, std::vector<TreeEntry>& out, u32& error);
    // Deletes the object at path, a drawer with everything in it.
    bool delete_tree(const char* path, u32& error);
    // Copies the bytes of the file at from to the new file to.
    bool copy_file(const char* from, const char* to, u32& error);
    // Takes the place away from the icon in the .info file.
    void clear_icon_position(const char* info);
    void copy_attributes(const char* from, const char* to);
    // Copies the object at from and its .info to the free path to.
    bool duplicate(const char* from, const char* to, u32& error);
    // The icon that stands for a file or drawer the desktop may change, or
    // null.
    const IconEntry* file_icon(u32 window_id, u32 icon_id) const;
    void close_windows_below(const char* path);
    void ask_delete(u32 window_id, u32 icon_id);
    void ask_rename(u32 window_id, u32 icon_id);
    void ask_new_drawer(u32 window_id);
    void ask_command();
    void ask_empty_trash(u32 window_id, u32 icon_id);
    // The path of the Trashcan the icon stands for.
    bool trash_path(u32 window_id, u32 icon_id, char (&out)[Dos::kMaxPath]) const;
    bool can_show_dialog() const;
    // Handles the messages at the dialog's window: its close gadget and
    // Cancel cancel it, on_gadget answers for any other gadget let go.
    template <typename OnGadget>
    Reply dialog_input(const Dialog& dialog, OnGadget on_gadget);
    // Waits at the dialog's window until step answers.
    template <typename Step>
    Reply wait_for_reply(const Dialog& dialog, Step step);
    // The rows of the information window's tool types: their exec List, how
    // they show with one selected, and what New, Del and Return do.
    void link_tool_types(const InfoRequest& request);
    void show_tool_types(InfoRequest& request, u32 selected);
    void add_tool_type(InfoRequest& request);
    void delete_tool_type(InfoRequest& request);
    void edit_tool_type(InfoRequest& request);
    void commit_tool_type(InfoRequest& request);
    void drop_empty_tool_types(InfoRequest& request);
    // Writes the stack, default tool and tool types the information window
    // changed into the icon's .info; changed tells whether it wrote it.
    bool save_icon_info(InfoRequest& request, bool& changed, u32& error);
    // Renames the object and its .info, both or neither.
    bool rename_with_info(const char* from, const char* to, u32& error);
    void select_contents(u32 window_id);
    void clean_up(u32 window_id);
    void open_selected(u32 window_id);
    void mouse_click(u32 window_id, i32 mx, i32 my, u64 now_us);
    void drag_to(u32 window_id, i32 mx, i32 my);
    void end_drag(u32 window_id, i32 mx, i32 my);
    void cancel_drag();
    void drop(const IconDrag& drag, i32 sx, i32 sy);
    bool transfer(const IconEntry& icon, const char* from, const char* to, i32 left, i32 top);
    bool icon_path(const DrawerWindow& window, const IconEntry& icon,
                   char (&out)[Dos::kMaxPath]) const;
    bool icon_target_path(const DrawerWindow& window, const IconEntry& icon,
                          char (&out)[Dos::kMaxPath]) const;
    bool screen_origin(u32 window_id, i32& x, i32& y) const;
    void show_error(const char* what, const char* name, u32 error);
    void show_requester(const char* title, const char* text);
    // EasyRequest over the desktop with the text and the buttons of
    // gadgets, giving its answer; 0 when none can be shown.
    i32 easy(const char* title, const char* text, const char* gadgets);
    const IconEntry* icon_at(u32 window_id, i32 x, i32 y) const;
    void start_wbstartup();

    DrawerWindow* find_window(u32 id);
    const DrawerWindow* find_window(u32 id) const;
    IconEntry* find_icon(u32 id);
    const IconEntry* find_icon(u32 id) const;
    IconEntry* take_icon(u32 owner);
    void free_icons(u32 owner);
    void measure(IconEntry& icon);
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
    RunCommand run_command_ = nullptr;
    void* run_command_ctx_ = nullptr;
    char last_command_[kMaxCommand]{};  // offered again by Execute Command
    char last_message_[Dos::kMaxPath + 128]{};
    Answer answer_ = nullptr;
    void* answer_ctx_ = nullptr;
    u32 base_ = 0;
    u32 process_ = 0;
    u32 screen_ = 0;
    u32 reply_port_ = 0;
    u32 window_port_ = 0;
    u32 menu_ = 0;
    u32 title_buffer_ = 0;
    u32 arrows_ = 0;  // the arrow gadgets' images, made once
    // The depth and pens the arrow images were last painted for.
    u32 arrow_depth_ = 0;
    chrome::DrawInfoPens arrow_pens_{};
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
    u64 next_volume_us_ = 0;
    u64 next_refresh_us_ = 0;
    bool patterns_changed_ = false;  // WBPattern's prefs were written
    bool reopen_pending_ = false;  // OpenWorkBench asked for the windows back
    bool quit_pending_ = false;    // Quit was picked and answered Ok
    bool running_ = false;
    bool desktop_open_ = false;
    bool backdrop_ = true;
    std::vector<LeftOut> left_out_;
    SlotList<IconEntry> icons_{kMaxIcons};
    u32 last_icon_id_ = 0;  // the id of the icon made last
    SlotList<DrawerWindow> windows_{kMaxWindows};
    // The drawers close_for_reset closed, in the order they were opened.
    std::vector<RememberedDrawer> remembered_;
    u32 open_count_ = 0;  // the drawers opened so far; the id of the last
    SlotList<RunningTool> tools_{kMaxTools};
    SlotList<App> apps_{kMaxApps};
    IconDrag drag_{};
    NativeVector vectors_[kVectors]{};
};

}
