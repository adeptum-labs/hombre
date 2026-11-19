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

#include "workbench.hpp"

#include "../format.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using native::LogLine;
using i16 = __INT16_TYPE__;

namespace {

constexpr char kLibraryName[] = "workbench.library";
constexpr char kIdString[] = "workbench 60.0 (Hombre)\r\n";
constexpr char kProcessName[] = "Workbench";
constexpr char kBackdropName[] = "Workbench";
constexpr char kRamDiskName[] = "Ram Disk";
constexpr char kWbStartupDrawer[] = "SYS:WBStartup";

// struct DiskObject (workbench/workbench.h): the Gadget comes first.
constexpr u32 do_Gadget = 4;
constexpr u32 do_Type = 48;
constexpr u32 do_DefaultTool = 50;
constexpr u32 do_CurrentX = 58;
constexpr u32 do_CurrentY = 62;
constexpr u32 do_DrawerData = 66;
constexpr u32 do_ToolWindow = 70;
constexpr u32 do_StackSize = 74;
constexpr u32 kNoIconPosition = 0x80000000;
constexpr u32 kMinStack = 4096;

// struct Image (intuition/intuition.h).
constexpr u32 im_LeftEdge = 0;
constexpr u32 im_TopEdge = 2;
constexpr u32 im_Width = 4;
constexpr u32 im_Height = 6;

// struct IntuiMessage classes and input codes intuition sends.
constexpr u32 kIdcmpRefreshWindow = 0x00000004;
constexpr u32 kIdcmpMenuPick = 0x00000100;
constexpr u32 kMenuNull = 0xFFFF;

// struct NewMenu (intuition/intuition.h) and its types and flags.
constexpr u32 nm_Type = 0;
constexpr u32 nm_Label = 2;
constexpr u32 nm_CommKey = 6;
constexpr u32 nm_Flags = 10;
constexpr u32 nm_MutualExclude = 12;
constexpr u32 nm_UserData = 16;
constexpr u32 NewMenu_SIZEOF = 20;
constexpr u8 NM_END = 0;
constexpr u8 NM_TITLE = 1;
constexpr u8 NM_ITEM = 2;
constexpr u16 NM_ITEMDISABLED = 0x0010;

// struct WBStartup and struct WBArg (workbench/startup.h).
constexpr u32 sm_Message = 0;
constexpr u32 sm_Process = 20;
constexpr u32 sm_Segment = 24;
constexpr u32 sm_NumArgs = 28;
constexpr u32 sm_ToolWindow = 32;
constexpr u32 sm_ArgList = 36;
constexpr u32 WBStartup_SIZEOF = 40;
constexpr u32 wa_Lock = 0;
constexpr u32 wa_Name = 4;
constexpr u32 WBArg_SIZEOF = 8;

// The desktop's spacing: the right-hand column of volume icons, the label
// rows under an image, and the grid Clean Up and the unplaced icons use.
constexpr i32 kDiskMargin = 16;
constexpr i32 kVolumeTop = 16;
constexpr i32 kLabelBaseline = 9;  // from the bottom of the image to the label's
constexpr u32 kLabelWidth = 8;     // the topaz font's character width
constexpr u32 kLabelRows = 8;
constexpr i32 kIconGapX = 16;
constexpr i32 kIconGapY = 12;
constexpr i32 kCellGapX = 16;
constexpr i32 kCellGapY = 12;
constexpr i32 kGridMarginX = 8;
constexpr i32 kGridMarginY = 8;
constexpr i32 kDefaultDrawerWidth = 320;
constexpr i32 kDefaultDrawerHeight = 160;
constexpr u32 kMinDrawerWidth = 90;
constexpr u32 kMinDrawerHeight = 40;
constexpr i32 kCascadeX = 40;
constexpr i32 kCascadeY = 44;
constexpr i32 kCascadeStep = 20;
constexpr u32 kMaxWindowTitle = 64;

// The menu labels, in the order the menus number them.
constexpr u16 kDisabled = NM_ITEMDISABLED;

struct MenuItemSpec {
    const char* label;
    bool enabled;
};

struct MenuSpec {
    const char* title;
    const MenuItemSpec* items;
    u32 count;
};

constexpr MenuItemSpec kWorkbenchItems[] = {
    {"Backdrop", false},  {"Execute Command...", false}, {"Redraw All", true},
    {"Update All", true}, {"Last Message", false},       {"About...", true},
    {"Quit", false},
};

constexpr MenuItemSpec kWindowItems[] = {
    {"New Drawer", false},     {"Open Parent", true}, {"Close", true},
    {"Update", false},         {"Select Contents", true}, {"Clean Up", true},
    {"Snapshot", false},       {"Show", false},      {"View By", false},
};

constexpr MenuItemSpec kIconsItems[] = {
    {"Open", true},          {"Copy", false},         {"Rename...", false},
    {"Information...", false}, {"Snapshot", false},   {"Unsnapshot", false},
    {"Leave Out", false},    {"Put Away", false},     {"Delete...", false},
    {"Format Disk...", false}, {"Empty Trash", false},
};

constexpr MenuItemSpec kToolsItems[] = {
    {"ResetWB", false},
};

constexpr MenuSpec kMenus[] = {
    {"Workbench", kWorkbenchItems, sizeof kWorkbenchItems / sizeof kWorkbenchItems[0]},
    {"Window", kWindowItems, sizeof kWindowItems / sizeof kWindowItems[0]},
    {"Icons", kIconsItems, sizeof kIconsItems / sizeof kIconsItems[0]},
    {"Tools", kToolsItems, sizeof kToolsItems / sizeof kToolsItems[0]},
};

constexpr u32 kMenuCount = sizeof kMenus / sizeof kMenus[0];

constexpr u32 kMenuWorkbench = 0;
constexpr u32 kMenuWindow = 1;
constexpr u32 kMenuIcons = 2;

constexpr u32 text_length(const char* text) {
    u32 n = 0;
    while (text[n] != '\0') {
        ++n;
    }
    return n;
}

char upper_ascii(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

bool same_ci(const char* a, const char* b) {
    for (u32 i = 0;; ++i) {
        if (upper_ascii(a[i]) != upper_ascii(b[i])) {
            return false;
        }
        if (a[i] == '\0') {
            return true;
        }
    }
}

bool less_ci(const char* a, const char* b) {
    for (u32 i = 0;; ++i) {
        const char ca = upper_ascii(a[i]);
        const char cb = upper_ascii(b[i]);
        if (ca != cb) {
            return ca < cb;
        }
        if (ca == '\0') {
            return false;
        }
    }
}

bool ends_with_ci(const char* text, const char* suffix) {
    const u32 text_len = text_length(text);
    const u32 suffix_len = text_length(suffix);
    if (suffix_len > text_len) {
        return false;
    }
    return same_ci(text + (text_len - suffix_len), suffix);
}

void copy_text(char* dst, u32 capacity, const char* src) {
    u32 i = 0;
    for (; i + 1 < capacity && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

// The drawer and file name as one AmigaDOS path: a device stays glued to
// the name after it.
bool join_path(const char* drawer, const char* name, char (&out)[Dos::kMaxPath]) {
    u32 at = 0;
    const u32 drawer_len = text_length(drawer);
    const bool separator = drawer_len != 0 && drawer[drawer_len - 1] != ':' &&
                           drawer[drawer_len - 1] != '/';
    const u32 needed = drawer_len + (separator ? 1 : 0) + text_length(name);
    if (needed + 1 > Dos::kMaxPath) {
        return false;
    }
    for (u32 i = 0; i < drawer_len; ++i) {
        out[at++] = drawer[i];
    }
    if (separator) {
        out[at++] = '/';
    }
    for (u32 i = 0; name[i] != '\0'; ++i) {
        out[at++] = name[i];
    }
    out[at] = '\0';
    return true;
}

bool is_volume_root(const char* path) {
    const u32 length = text_length(path);
    return length != 0 && path[length - 1] == ':';
}

// The first 31 characters of entry without its ".info" suffix.
void file_stem(const char* entry, char (&out)[32]) {
    u32 length = text_length(entry);
    length = length >= 5 ? length - 5 : 0;
    if (length > 31) {
        length = 31;
    }
    for (u32 i = 0; i < length; ++i) {
        out[i] = entry[i];
    }
    out[length] = '\0';
}

struct IconArt {
    u32 render = 0;
    u32 select = 0;
    i32 left = 0;
    i32 top = 0;
    i32 width = 0;
    i32 height = 0;
};

IconArt art_of(Bus& b, u32 object) {
    IconArt art;
    if (object == 0) {
        return art;
    }
    const u32 gadget = object + do_Gadget;
    art.render = b.r32(gadget + ndk::gg_GadgetRender);
    art.select = b.r32(gadget + ndk::gg_SelectRender);
    if (art.render != 0) {
        art.left = static_cast<i16>(b.r16(art.render + im_LeftEdge));
        art.top = static_cast<i16>(b.r16(art.render + im_TopEdge));
        art.width = static_cast<i16>(b.r16(art.render + im_Width));
        art.height = static_cast<i16>(b.r16(art.render + im_Height));
    }
    return art;
}

struct Function {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

}  // namespace

// --- the click state of a window -----------------------------------------

Workbench::ClickAction Workbench::Clicks::click(u32 icon, u64 now_us) {
    if (icon == kNoIcon) {
        last_icon_ = kNoIcon;
        if (selected_ != kNoIcon) {
            selected_ = kNoIcon;
            return ClickAction::Clear;
        }
        return ClickAction::None;
    }
    const bool again = icon == last_icon_ && last_icon_ != kNoIcon && now_us >= last_us_ &&
                       now_us - last_us_ <= kDoubleClickUs;
    last_icon_ = icon;
    last_us_ = now_us;
    const bool changed = selected_ != icon;
    selected_ = icon;
    if (again) {
        // The click that opened the icon is not the first of another pair.
        last_icon_ = kNoIcon;
        return ClickAction::Open;
    }
    return changed ? ClickAction::Select : ClickAction::None;
}

// --- the desktop ---------------------------------------------------------

u32 Workbench::build_menu_strip() {
    if (gadtools_ == nullptr || exec_ == nullptr) {
        return 0;
    }
    u32 entries = 1;  // NM_END
    u32 label_bytes = 0;
    for (u32 m = 0; m < kMenuCount; ++m) {
        entries += 1 + kMenus[m].count;
        label_bytes += text_length(kMenus[m].title) + 1;
        for (u32 i = 0; i < kMenus[m].count; ++i) {
            label_bytes += text_length(kMenus[m].items[i].label) + 1;
        }
    }
    const u32 bytes = entries * NewMenu_SIZEOF + label_bytes;
    const u32 block = exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (block == 0) {
        return 0;
    }
    Bus b(*machine_);
    u32 at = block;
    u32 label_at = block + entries * NewMenu_SIZEOF;
    auto put_label = [&](const char* text) {
        const u32 length = text_length(text);
        for (u32 i = 0; i <= length; ++i) {
            b.w8(label_at + i, static_cast<u8>(text[i]));
        }
        const u32 start = label_at;
        label_at += length + 1;
        return start;
    };
    auto put_entry = [&](u8 type, const char* label, u16 flags) {
        b.w8(at + nm_Type, type);
        b.w32(at + nm_Label, label != nullptr ? put_label(label) : 0);
        b.w32(at + nm_CommKey, 0);
        b.w16(at + nm_Flags, flags);
        b.w32(at + nm_MutualExclude, 0);
        b.w32(at + nm_UserData, 0);
        at += NewMenu_SIZEOF;
    };
    for (u32 m = 0; m < kMenuCount; ++m) {
        put_entry(NM_TITLE, kMenus[m].title, 0);
        for (u32 i = 0; i < kMenus[m].count; ++i) {
            put_entry(NM_ITEM, kMenus[m].items[i].label,
                      kMenus[m].items[i].enabled ? 0 : kDisabled);
        }
    }
    put_entry(NM_END, nullptr, 0);
    if (!b.ok()) {
        exec_->free(block, bytes);
        return 0;
    }
    const u32 strip = gadtools_->create_menus(block);
    if (strip == 0) {
        exec_->free(block, bytes);
        return 0;
    }
    gadtools_->layout_menus(strip);
    return strip;
}

void Workbench::measure_window(u32 index) {
    DrawerWindow& w = windows_[index];
    Bus b(*machine_);
    w.inner_left = b.r8(w.window + ndk::wd_BorderLeft);
    w.inner_top = b.r8(w.window + ndk::wd_BorderTop);
    const i32 width = static_cast<i16>(b.r16(w.window + ndk::wd_Width));
    const i32 height = static_cast<i16>(b.r16(w.window + ndk::wd_Height));
    const i32 right = b.r8(w.window + ndk::wd_BorderRight);
    const i32 bottom = b.r8(w.window + ndk::wd_BorderBottom);
    w.inner_width = width - w.inner_left - right;
    w.inner_height = height - w.inner_top - bottom;
}

void Workbench::attach_window(u32 index, u32 window, const char* name, const char* path) {
    DrawerWindow& w = windows_[index];
    w = DrawerWindow{};
    w.used = true;
    w.window = window;
    Bus b(*machine_);
    w.user_port = b.r32(window + ndk::wd_UserPort);
    w.rp = b.r32(window + ndk::wd_RPort);
    measure_window(index);
    copy_text(w.name, sizeof w.name, name);
    copy_text(w.path, sizeof w.path, path);
    if (menu_ != 0) {
        intuition_->set_menu_strip(window, menu_);
    }
}

void Workbench::open_backdrop() {
    Bus b(*machine_);
    const u32 bar_height = b.r8(screen_ + ndk::sc_BarHeight);
    const u32 new_window =
        exec_->alloc(ndk::NewWindow_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    const u32 title = exec_->alloc(sizeof kBackdropName, ndk::MEMF_PUBLIC);
    if (new_window == 0 || title == 0) {
        if (new_window != 0) {
            exec_->free(new_window, ndk::NewWindow_SIZEOF);
        }
        if (title != 0) {
            exec_->free(title, sizeof kBackdropName);
        }
        LogLine().text("workbench: no memory for the backdrop").send();
        return;
    }
    for (u32 i = 0; i < sizeof kBackdropName; ++i) {
        b.w8(title + i, static_cast<u8>(kBackdropName[i]));
    }
    b.w16(new_window + ndk::nw_LeftEdge, 0);
    b.w16(new_window + ndk::nw_TopEdge, bar_height);
    b.w16(new_window + ndk::nw_Width, 0);
    b.w16(new_window + ndk::nw_Height, 0);
    b.w32(new_window + ndk::nw_IDCMPFlags,
          ndk::IDCMP_MOUSEBUTTONS | kIdcmpMenuPick | kIdcmpRefreshWindow);
    b.w32(new_window + ndk::nw_Flags, ndk::WFLG_BACKDROP | ndk::WFLG_BORDERLESS);
    b.w32(new_window + ndk::nw_Title, title);
    b.w32(new_window + ndk::nw_Screen, screen_);
    b.w16(new_window + ndk::nw_Type, ndk::CUSTOMSCREEN);
    const u32 window = b.ok() ? intuition_->open_window(new_window, 0) : 0;
    exec_->free(new_window, ndk::NewWindow_SIZEOF);
    exec_->free(title, sizeof kBackdropName);
    if (window == 0) {
        LogLine().text("workbench: could not open the backdrop").send();
        return;
    }
    attach_window(0, window, kBackdropName, "");
}

void Workbench::measure(IconEntry& icon) {
    if (icon.object == 0) {
        return;
    }
    Bus b(*machine_);
    const u32 gadget = icon.object + do_Gadget;
    icon.width = static_cast<i16>(b.r16(gadget + ndk::gg_Width));
    icon.height = static_cast<i16>(b.r16(gadget + ndk::gg_Height));
    if (icon.width < 8) {
        icon.width = 8;
    }
    if (icon.height < 8) {
        icon.height = 8;
    }
    const u32 stack = b.r32(icon.object + do_StackSize);
    icon.stack_size = stack < kMinStack ? kMinStack : stack;
    icon.tool_window = b.r32(icon.object + do_ToolWindow);
}

Workbench::IconEntry* Workbench::take_icon(u32 owner) {
    for (IconEntry& icon : icons_) {
        if (!icon.used) {
            icon = IconEntry{};
            icon.used = true;
            icon.owner = static_cast<u8>(owner);
            return &icon;
        }
    }
    return nullptr;
}

void Workbench::free_icons(u32 owner) {
    for (IconEntry& icon : icons_) {
        if (!icon.used || icon.owner != owner) {
            continue;
        }
        if (icon.owned && icon.object != 0) {
            icon_->free_object(icon.object);
        }
        icon = IconEntry{};
    }
}

void Workbench::add_volume(const char* label, const char* icon_path) {
    if (!windows_[0].used) {
        return;
    }
    IconEntry* icon = take_icon(0);
    if (icon == nullptr) {
        LogLine().text("workbench: icon table full").send();
        return;
    }
    icon->type = kDisk;
    if (icon_path != nullptr) {
        icon->object = icon_->load_object(icon_path);
        icon->owned = icon->object != 0;
    }
    if (icon->object == 0) {
        icon->object = icon_->default_object(kDisk);
        icon->owned = icon->object != 0;
    }
    copy_text(icon->name, sizeof icon->name, label);
    copy_text(icon->file, sizeof icon->file, "Disk");
    measure(*icon);
    const DrawerWindow& backdrop = windows_[0];
    i32 left = backdrop.inner_width - icon->width - kDiskMargin;
    i32 top = volume_top_ == 0 ? kVolumeTop : volume_top_;
    if (top + icon->height + static_cast<i32>(kLabelRows) > backdrop.inner_height) {
        left -= icon->width + kIconGapX;
        top = kVolumeTop;
    }
    icon->left = left < 0 ? 0 : left;
    icon->top = top;
    volume_top_ = top + icon->height + static_cast<i32>(kLabelRows) + kIconGapY;
    LogLine().text("workbench: volume ").text(label).send();
}

void Workbench::add_volume_icons() {
    const char* volume = dos_->volume_name();
    if (volume[0] != '\0') {
        char device[Dos::kMaxPath];
        copy_text(device, sizeof device, volume);
        const u32 length = text_length(device);
        if (length + 2 <= sizeof device) {
            device[length] = ':';
            device[length + 1] = '\0';
            char path[Dos::kMaxPath];
            if (join_path(device, "Disk", path)) {
                add_volume(volume, path);
            }
        }
    }
    add_volume(kRamDiskName, nullptr);
}

// --- drawer windows ------------------------------------------------------

void Workbench::add_dir_icon(u32 window_index, const char* entry, bool is_dir) {
    if (is_dir || !ends_with_ci(entry, ".info")) {
        return;
    }
    char stem[32];
    file_stem(entry, stem);
    if (stem[0] == '\0') {
        return;
    }
    const DrawerWindow& drawer = windows_[window_index];
    if (is_volume_root(drawer.path) && same_ci(stem, "Disk")) {
        return;
    }
    char path[Dos::kMaxPath];
    if (!join_path(drawer.path, stem, path)) {
        return;
    }
    const u32 object = icon_->load_object(path);
    if (object == 0) {
        return;
    }
    IconEntry* icon = take_icon(window_index);
    if (icon == nullptr) {
        icon_->free_object(object);
        LogLine().text("workbench: icon table full").send();
        return;
    }
    Bus b(*machine_);
    icon->object = object;
    icon->owned = true;
    icon->type = static_cast<u8>(b.r8(object + do_Type));
    icon->left = static_cast<i32>(b.r32(object + do_CurrentX));
    icon->top = static_cast<i32>(b.r32(object + do_CurrentY));
    icon->placed = static_cast<u32>(icon->left) != kNoIconPosition &&
                   static_cast<u32>(icon->top) != kNoIconPosition;
    copy_text(icon->name, sizeof icon->name, stem);
    copy_text(icon->file, sizeof icon->file, stem);
    measure(*icon);
}

void Workbench::fill_drawer(u32 window_index) {
    free_icons(window_index);
    // The icon the selection named is gone with the others.
    clicks_[window_index].clear();
    struct Collect {
        Workbench* self;
        u32 window_index;
    } collect{this, window_index};
    auto visit = [](void* at, const char* name, bool is_dir) {
        const Collect& c = *static_cast<const Collect*>(at);
        c.self->add_dir_icon(c.window_index, name, is_dir);
    };
    u32 error = 0;
    dos_->list_dir(windows_[window_index].path, visit, &collect, error);
    place_icons(window_index);
}

void Workbench::place_icons(u32 window_index) {
    const DrawerWindow& drawer = windows_[window_index];
    i32 cell_width = 16;
    i32 cell_height = 16;
    for (const IconEntry& icon : icons_) {
        if (!icon.used || icon.owner != window_index) {
            continue;
        }
        cell_width = icon.width + kCellGapX > cell_width ? icon.width + kCellGapX : cell_width;
        cell_height = icon.height + static_cast<i32>(kLabelRows) + kCellGapY > cell_height
                          ? icon.height + static_cast<i32>(kLabelRows) + kCellGapY
                          : cell_height;
    }
    i32 rows = (drawer.inner_height - kGridMarginY) / cell_height;
    if (rows < 1) {
        rows = 1;
    }
    u32 slot = 0;
    for (IconEntry& icon : icons_) {
        if (!icon.used || icon.owner != window_index || icon.placed) {
            continue;
        }
        icon.left = kGridMarginX + static_cast<i32>(slot / static_cast<u32>(rows)) * cell_width;
        icon.top = kGridMarginY + static_cast<i32>(slot % static_cast<u32>(rows)) * cell_height;
        ++slot;
    }
}

u32 Workbench::free_window_slot() const {
    for (u32 i = 1; i < kMaxWindows; ++i) {
        if (!windows_[i].used) {
            return i;
        }
    }
    return kMaxWindows;
}

void Workbench::open_drawer(u32 window_index, const IconEntry& icon) {
    char path[Dos::kMaxPath];
    if (window_index == 0 && icon.type == kDisk) {
        const u32 length = text_length(icon.name);
        if (length + 2 > sizeof path) {
            return;
        }
        for (u32 i = 0; i < length; ++i) {
            path[i] = icon.name[i];
        }
        path[length] = ':';
        path[length + 1] = '\0';
    } else if (!join_path(windows_[window_index].path, icon.file, path)) {
        return;
    }
    open_drawer_window(path, icon.name, icon.object);
}

void Workbench::open_drawer_window(const char* path, const char* name, u32 object) {
    for (u32 i = 1; i < kMaxWindows; ++i) {
        if (windows_[i].used && same_ci(windows_[i].path, path)) {
            intuition_->window_to_front(windows_[i].window);
            return;
        }
    }
    const u32 slot = free_window_slot();
    if (slot == kMaxWindows) {
        LogLine().text("workbench: no window left for ").text(name).send();
        return;
    }
    Bus b(*machine_);
    i32 left = -1;
    i32 top = -1;
    i32 width = 0;
    i32 height = 0;
    const u32 drawer_data = object != 0 ? b.r32(object + do_DrawerData) : 0;
    if (drawer_data != 0) {
        left = static_cast<i16>(b.r16(drawer_data + ndk::nw_LeftEdge));
        top = static_cast<i16>(b.r16(drawer_data + ndk::nw_TopEdge));
        width = static_cast<i16>(b.r16(drawer_data + ndk::nw_Width));
        height = static_cast<i16>(b.r16(drawer_data + ndk::nw_Height));
    }
    if (width <= 0) {
        width = kDefaultDrawerWidth;
    }
    if (height <= 0) {
        height = kDefaultDrawerHeight;
    }
    if (left < 0) {
        left = kCascadeX + kCascadeStep * static_cast<i32>(slot - 1);
    }
    if (top < 0) {
        top = kCascadeY + kCascadeStep * static_cast<i32>(slot - 1);
    }
    const u32 new_window =
        exec_->alloc(ndk::NewWindow_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    const u32 title_bytes = text_length(name) + 1;
    const u32 title = exec_->alloc(title_bytes, ndk::MEMF_PUBLIC);
    if (new_window == 0 || title == 0) {
        if (new_window != 0) {
            exec_->free(new_window, ndk::NewWindow_SIZEOF);
        }
        if (title != 0) {
            exec_->free(title, title_bytes);
        }
        LogLine().text("workbench: no memory for ").text(name).send();
        return;
    }
    for (u32 i = 0; i < title_bytes; ++i) {
        b.w8(title + i, static_cast<u8>(name[i]));
    }
    b.w16(new_window + ndk::nw_LeftEdge, static_cast<u32>(left));
    b.w16(new_window + ndk::nw_TopEdge, static_cast<u32>(top));
    b.w16(new_window + ndk::nw_Width, static_cast<u32>(width));
    b.w16(new_window + ndk::nw_Height, static_cast<u32>(height));
    b.w8(new_window + ndk::nw_DetailPen, 0);
    b.w8(new_window + ndk::nw_BlockPen, 1);
    b.w32(new_window + ndk::nw_IDCMPFlags,
          ndk::IDCMP_CLOSEWINDOW | ndk::IDCMP_MOUSEBUTTONS | ndk::IDCMP_NEWSIZE | kIdcmpMenuPick |
              kIdcmpRefreshWindow);
    b.w32(new_window + ndk::nw_Flags,
          ndk::WFLG_DRAGBAR | ndk::WFLG_DEPTHGADGET | ndk::WFLG_CLOSEGADGET |
              ndk::WFLG_SIZEGADGET | ndk::WFLG_SIZEBRIGHT | ndk::WFLG_SIZEBBOTTOM |
              ndk::WFLG_ACTIVATE);
    b.w32(new_window + ndk::nw_Title, title);
    b.w32(new_window + ndk::nw_Screen, screen_);
    b.w16(new_window + ndk::nw_Type, ndk::CUSTOMSCREEN);
    // The size gadget takes the drawer from a few icons' room up to the
    // whole screen.
    b.w16(new_window + ndk::nw_MinWidth, kMinDrawerWidth);
    b.w16(new_window + ndk::nw_MinHeight, kMinDrawerHeight);
    b.w16(new_window + ndk::nw_MaxWidth, 0xFFFF);
    b.w16(new_window + ndk::nw_MaxHeight, 0xFFFF);
    // The window keeps a copy of its title, so the one built here goes
    // back with the NewWindow.
    const u32 window = b.ok() ? intuition_->open_window(new_window, 0) : 0;
    exec_->free(new_window, ndk::NewWindow_SIZEOF);
    exec_->free(title, title_bytes);
    if (window == 0) {
        LogLine().text("workbench: could not open ").text(name).send();
        return;
    }
    attach_window(slot, window, name, path);
    fill_drawer(slot);
    draw_window(slot);
    LogLine().text("workbench: opened ").text(name).send();
}

void Workbench::close_drawer(u32 window_index) {
    if (window_index == 0 || !windows_[window_index].used) {
        return;
    }
    free_icons(window_index);
    clicks_[window_index].clear();
    intuition_->close_window(windows_[window_index].window);
    windows_[window_index] = DrawerWindow{};
    draw_window(0);
}

// --- drawing -------------------------------------------------------------

void Workbench::draw_icon(const DrawerWindow& drawer, const IconEntry& icon, bool selected) {
    if (icon.object == 0) {
        return;
    }
    Bus b(*machine_);
    const IconArt art = art_of(b, icon.object);
    const i32 left = drawer.inner_left + icon.left;
    const i32 top = drawer.inner_top + icon.top;
    if (art.render != 0) {
        if (selected && art.select != 0) {
            intuition_->draw_image(drawer.rp, art.select, left, top);
        } else if (selected) {
            // Without a second image the icon shows selected by its own
            // image with every pen complemented.
            intuition_->draw_image_complemented(drawer.rp, art.render, left, top);
        } else {
            intuition_->draw_image(drawer.rp, art.render, left, top);
        }
    }
    const u32 length = text_length(icon.name);
    const i32 span = static_cast<i32>(length * kLabelWidth);
    i32 label_x = left;
    i32 label_y = top + icon.height + kLabelBaseline;
    if (art.render != 0 && art.width > 0) {
        label_x = left + art.left + (art.width - span) / 2;
        label_y = top + art.top + art.height + kLabelBaseline;
    }
    const i32 inner_right = drawer.inner_left + drawer.inner_width - 1;
    if (label_x + span > inner_right) {
        label_x = inner_right - span;
    }
    if (label_x < drawer.inner_left) {
        label_x = drawer.inner_left;
    }
    graphics_->set_pens(drawer.rp, 1, 0, ndk::JAM1);
    graphics_->move(drawer.rp, label_x, label_y);
    graphics_->text(drawer.rp, reinterpret_cast<const u8*>(icon.name), length);
}

void Workbench::draw_window(u32 window_index) {
    if (!windows_[window_index].used) {
        return;
    }
    const DrawerWindow& drawer = windows_[window_index];
    const u32 selected = clicks_[window_index].selected();
    const i32 right = drawer.inner_left + drawer.inner_width - 1;
    const i32 bottom = drawer.inner_top + drawer.inner_height - 1;
    if (right < drawer.inner_left || bottom < drawer.inner_top) {
        return;
    }
    graphics_->set_pens(drawer.rp, 0, 0, ndk::JAM1);
    graphics_->rect_fill(drawer.rp, drawer.inner_left, drawer.inner_top, right, bottom);
    for (u32 i = 0; i < kMaxIcons; ++i) {
        const IconEntry& icon = icons_[i];
        if (icon.used && icon.owner == window_index) {
            draw_icon(drawer, icon, selected == i);
        }
    }
}

void Workbench::redraw_all() {
    for (u32 i = 0; i < kMaxWindows; ++i) {
        draw_window(i);
    }
}

void Workbench::refresh_title() {
    if (!windows_[0].used) {
        return;
    }
    if (title_buffer_ == 0) {
        title_buffer_ = exec_->alloc(kMaxWindowTitle, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (title_buffer_ == 0) {
            return;
        }
    }
    char digits[16];
    char text[kMaxWindowTitle];
    u32 at = 0;
    auto put = [&](const char* part) {
        for (u32 i = 0; part[i] != '\0' && at + 1 < sizeof text; ++i) {
            text[at++] = part[i];
        }
    };
    put("Amiga Workbench  ");
    format_u64(digits, sizeof digits, exec_->free_bytes(), 10);
    put(digits);
    put(" graphics mem  0 other mem");
    text[at] = '\0';
    Bus b(*machine_);
    for (u32 i = 0; i <= at; ++i) {
        b.w8(title_buffer_ + i, static_cast<u8>(text[i]));
    }
    if (b.ok()) {
        intuition_->set_window_titles(windows_[0].window, 0xFFFFFFFF, title_buffer_);
    }
}

// --- the desktop's actions ----------------------------------------------

u32 Workbench::icon_at(u32 window_index, i32 x, i32 y) const {
    for (u32 i = 0; i < kMaxIcons; ++i) {
        const IconEntry& icon = icons_[i];
        if (!icon.used || icon.owner != window_index) {
            continue;
        }
        if (x >= icon.left && x < icon.left + icon.width && y >= icon.top &&
            y < icon.top + icon.height) {
            return i;
        }
    }
    return kNoIcon;
}

void Workbench::open_icon(u32 window_index, u32 icon_index) {
    if (icon_index >= kMaxIcons || !icons_[icon_index].used) {
        return;
    }
    const IconEntry& icon = icons_[icon_index];
    const char* drawer = windows_[window_index].path;
    switch (icon.type) {
    case kDisk:
    case kDrawer:
        open_drawer(window_index, icon);
        break;
    case kTool:
        start_tool(icon, drawer);
        break;
    case kProject:
        start_project(icon, drawer);
        break;
    default:
        break;
    }
}

void Workbench::mouse_click(u32 window_index, i32 mx, i32 my, u64 now_us) {
    if (!windows_[window_index].used) {
        return;
    }
    const DrawerWindow& drawer = windows_[window_index];
    const u32 hit = icon_at(window_index, mx - drawer.inner_left, my - drawer.inner_top);
    switch (clicks_[window_index].click(hit, now_us)) {
    case ClickAction::Select:
    case ClickAction::Clear:
        draw_window(window_index);
        break;
    case ClickAction::Open:
        draw_window(window_index);
        open_icon(window_index, hit);
        break;
    case ClickAction::None:
        break;
    }
}

void Workbench::open_parent(u32 window_index) {
    if (window_index == 0 || !windows_[window_index].used) {
        return;
    }
    const char* path = windows_[window_index].path;
    if (is_volume_root(path)) {
        return;  // a disk's window has no drawer above it
    }
    // The drawer above: the path up to its last '/', or the volume itself.
    i32 cut = -1;
    i32 colon = -1;
    for (i32 i = 0; path[i] != '\0'; ++i) {
        if (path[i] == '/') {
            cut = i;
        } else if (path[i] == ':') {
            colon = i;
        }
    }
    if (cut < 0 && colon < 0) {
        return;
    }
    char parent[Dos::kMaxPath];
    copy_text(parent, sizeof parent, path);
    const bool is_volume = cut < 0;
    parent[static_cast<u32>(is_volume ? colon + 1 : cut)] = '\0';
    // Its title and icon: the volume's name and Disk.info, or the drawer's
    // own name and icon.
    char name[32];
    char icon_path[Dos::kMaxPath];
    if (is_volume) {
        copy_text(name, sizeof name, parent);
        const u32 length = text_length(name);
        if (length != 0 && name[length - 1] == ':') {
            name[length - 1] = '\0';
        }
        if (!join_path(parent, "Disk", icon_path)) {
            icon_path[0] = '\0';
        }
    } else {
        i32 start = 0;
        for (i32 i = 0; parent[i] != '\0'; ++i) {
            if (parent[i] == '/' || parent[i] == ':') {
                start = i + 1;
            }
        }
        copy_text(name, sizeof name, parent + start);
        copy_text(icon_path, sizeof icon_path, parent);
    }
    const u32 object = icon_path[0] != '\0' ? icon_->load_object(icon_path) : 0;
    open_drawer_window(parent, name, object);
    if (object != 0) {
        icon_->free_object(object);
    }
}

void Workbench::select_contents(u32 window_index) {
    for (u32 i = 0; i < kMaxIcons; ++i) {
        if (icons_[i].used && icons_[i].owner == window_index) {
            clicks_[window_index].select(i);
            draw_window(window_index);
            return;
        }
    }
}

void Workbench::clean_up(u32 window_index) {
    for (IconEntry& icon : icons_) {
        if (icon.used && icon.owner == window_index) {
            icon.placed = false;
        }
    }
    place_icons(window_index);
    draw_window(window_index);
}

void Workbench::open_selected(u32 window_index) {
    const u32 selected = clicks_[window_index].selected();
    if (selected != kNoIcon) {
        open_icon(window_index, selected);
    }
}

void Workbench::update_all() {
    for (u32 i = 1; i < kMaxWindows; ++i) {
        if (!windows_[i].used) {
            continue;
        }
        fill_drawer(i);
        draw_window(i);
    }
}

// The requester is built in guest memory, answered by the Workbench task
// itself, and given back once a button is picked.
void Workbench::show_about() {
    if (machine_ == nullptr || exec_ == nullptr || intuition_ == nullptr) {
        return;
    }
    constexpr char kText[] =
        "Workbench version %ld.%ld\nKickstart version %ld.%ld\n\nCopyright \xA9 2026\n"
        "Adam Waldenberg and Adeptum AB";
    constexpr char kGadgetFormat[] = "Ok";
    constexpr char kTitle[] = "About Workbench";
    constexpr u32 kArgs = 4;
    const u32 bytes = ndk::EasyStruct_SIZEOF + sizeof kText + sizeof kGadgetFormat +
                      sizeof kTitle + kArgs * 4;
    const u32 block = exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (block == 0) {
        LogLine().text("workbench: no memory for About").send();
        return;
    }
    Bus b(*machine_);
    const u32 args = block + ndk::EasyStruct_SIZEOF;
    u32 at = args + kArgs * 4;
    auto put = [&](const char* text, u32 length) {
        const u32 start = at;
        for (u32 i = 0; i < length; ++i) {
            b.w8(at + i, static_cast<u8>(text[i]));
        }
        at += length;
        return start;
    };
    const u32 text_at = put(kText, sizeof kText);
    const u32 gadget_at = put(kGadgetFormat, sizeof kGadgetFormat);
    const u32 title_at = put(kTitle, sizeof kTitle);
    b.w32(block + ndk::es_StructSize, ndk::EasyStruct_SIZEOF);
    b.w32(block + ndk::es_Flags, 0);
    b.w32(block + ndk::es_Title, title_at);
    b.w32(block + ndk::es_TextFormat, text_at);
    b.w32(block + ndk::es_GadgetFormat, gadget_at);
    b.w32(args, kHombreLibVersion);
    b.w32(args + 4, kHombreLibRevision);
    b.w32(args + 8, kHombreLibVersion);
    b.w32(args + 12, kHombreLibRevision);
    const u32 window = windows_[0].used ? windows_[0].window : 0;
    if (b.ok()) {
        intuition_->easy_request(window, block, 0, args);
    }
    exec_->free(block, bytes);
}

void Workbench::menu_pick(u32 window_index, u32 number) {
    const u32 menu = number & 0x1F;
    const u32 item = (number >> 5) & 0x3F;
    if (number == kMenuNull || item == 0x3F) {
        return;
    }
    if (menu == kMenuWorkbench) {
        if (item == 2) {
            redraw_all();
        } else if (item == 3) {
            update_all();
        } else if (item == 5) {
            show_about();
        }
    } else if (menu == kMenuWindow) {
        if (item == 1) {
            open_parent(window_index);
        } else if (item == 2) {
            close_drawer(window_index);
        } else if (item == 4) {
            select_contents(window_index);
        } else if (item == 5) {
            clean_up(window_index);
        }
    } else if (menu == kMenuIcons) {
        if (item == 0) {
            open_selected(window_index);
        }
    }
}

// --- messages ------------------------------------------------------------

void Workbench::handle_message(u32 window_index, u32 message) {
    Bus b(*machine_);
    const u32 idcmp_class = b.r32(message + ndk::im_Class);
    const u32 code = b.r16(message + ndk::im_Code);
    const i32 mx = static_cast<i16>(b.r16(message + ndk::im_MouseX));
    const i32 my = static_cast<i16>(b.r16(message + ndk::im_MouseY));
    const u64 when = static_cast<u64>(b.r32(message + ndk::im_Seconds)) * 1000000 +
                     b.r32(message + ndk::im_Micros);
    // The message goes back before it is acted on: closing the window it
    // came from takes the port and everything still tied to it away.
    exec_->reply_message(message);
    if (!b.ok()) {
        return;
    }
    if (idcmp_class == ndk::IDCMP_CLOSEWINDOW) {
        close_drawer(window_index);
    } else if (idcmp_class == ndk::IDCMP_NEWSIZE) {
        measure_window(window_index);
        draw_window(window_index);
    } else if (idcmp_class == ndk::IDCMP_MOUSEBUTTONS && code == ndk::SELECTDOWN) {
        mouse_click(window_index, mx, my, when);
    } else if (idcmp_class == kIdcmpMenuPick) {
        menu_pick(window_index, code);
    }
}

bool Workbench::pump() {
    bool more = false;
    if (reply_port_ != 0) {
        for (u32 i = 0; i < kMaxTools; ++i) {
            const u32 message = exec_->get_message(reply_port_);
            if (message == 0) {
                break;
            }
            tool_reply(message);
        }
    }
    reap_tools();
    for (u32 i = 0; i < kMaxWindows; ++i) {
        for (u32 n = 0;; ++n) {
            // A close gadget closes the window and its port, so both are
            // read again for every message.
            const u32 port = windows_[i].user_port;
            if (!windows_[i].used || port == 0) {
                break;
            }
            if (n == kMaxMessagesPerPump) {
                more = true;
                break;
            }
            const u32 message = exec_->get_message(port);
            if (message == 0) {
                break;
            }
            handle_message(i, message);
        }
    }
    return more;
}

u32 Workbench::wait_mask() const {
    if (machine_ == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    u32 mask = 0;
    auto add = [&](u32 port) {
        if (port == 0) {
            return;
        }
        const u32 bit = b.r8(port + ndk::mp_SigBit);
        if (b.ok() && bit < 32) {
            mask |= 1u << bit;
        }
    };
    add(reply_port_);
    for (const DrawerWindow& w : windows_) {
        if (w.used) {
            add(w.user_port);
        }
    }
    return mask;
}

void Workbench::run() {
    open_desktop();
    for (;;) {
        const bool more = pump();
        const u64 now = exec_->now_us();
        if (now >= next_refresh_us_) {
            refresh_title();
            next_refresh_us_ = now + kRefreshUs;
        }
        // Until a message arrives at one of the ports, or the title is due
        // again. Messages a full pump left behind are taken at once.
        if (!more) {
            exec_->wait_for(wait_mask(), next_refresh_us_ - now);
        }
    }
}

void Workbench::task_entry(void* ctx) { static_cast<Workbench*>(ctx)->run(); }

// --- starting programs ---------------------------------------------------

u32 Workbench::build_startup(u32 process, u32 seglist, const StartupArg* args, u32 nargs,
                             const char* tool_window, u32* locks, u32& lock_count, u32& bytes) {
    bytes = 0;
    lock_count = 0;
    if (machine_ == nullptr || process == 0 || args == nullptr || locks == nullptr ||
        nargs == 0 || nargs > 2) {
        return 0;
    }
    u32 name_bytes = 0;
    for (u32 i = 0; i < nargs; ++i) {
        name_bytes += text_length(args[i].name) + 1;
    }
    const u32 window_bytes =
        tool_window != nullptr && tool_window[0] != '\0' ? text_length(tool_window) + 1 : 0;
    const u32 total = WBStartup_SIZEOF + nargs * WBArg_SIZEOF + name_bytes + window_bytes;
    const u32 message = exec_->alloc(total, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (message == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 args_at = message + WBStartup_SIZEOF;
    u32 name_at = args_at + nargs * WBArg_SIZEOF;
    u32 locked = 0;
    auto unlock_all = [&]() {
        for (u32 i = 0; i < locked; ++i) {
            dos_->unlock_path(locks[i]);
        }
        locked = 0;
        exec_->free(message, total);
    };
    for (u32 i = 0; i < nargs; ++i) {
        const u32 lock = dos_->lock_path(args[i].drawer);
        if (lock == 0) {
            unlock_all();
            return 0;
        }
        locks[locked++] = lock;
        const u32 length = text_length(args[i].name);
        for (u32 k = 0; k <= length; ++k) {
            b.w8(name_at + k, static_cast<u8>(args[i].name[k]));
        }
        b.w32(args_at + i * WBArg_SIZEOF + wa_Lock, lock);
        b.w32(args_at + i * WBArg_SIZEOF + wa_Name, name_at);
        name_at += length + 1;
    }
    u32 window_at = 0;
    if (window_bytes != 0) {
        window_at = name_at;
        for (u32 i = 0; i < window_bytes; ++i) {
            b.w8(window_at + i, static_cast<u8>(tool_window[i]));
        }
    }
    b.w32(message + sm_Message + ndk::mn_ReplyPort, reply_port_);
    b.w16(message + sm_Message + ndk::mn_Length, WBStartup_SIZEOF);
    b.w32(message + sm_Process, process + ndk::pr_MsgPort);
    b.w32(message + sm_Segment, seglist);
    b.w32(message + sm_NumArgs, nargs);
    b.w32(message + sm_ToolWindow, window_at);
    b.w32(message + sm_ArgList, args_at);
    if (!b.ok()) {
        unlock_all();
        return 0;
    }
    lock_count = locked;
    bytes = total;
    return message;
}

bool Workbench::start_program(const char* name, u32 seglist, const char* current_dir,
                              const StartupArg* args, u32 nargs, const char* tool_window,
                              u32 stack_size) {
    RunningTool* record = nullptr;
    for (RunningTool& tool : tools_) {
        if (!tool.used) {
            record = &tool;
            break;
        }
    }
    if (record == nullptr) {
        LogLine().text("workbench: too many programs running").send();
        dos_->unload_seg(seglist);
        return false;
    }
    const u32 current_lock = dos_->lock_path(current_dir);
    if (current_lock == 0) {
        LogLine().text("workbench: cannot lock ").text(current_dir).send();
        dos_->unload_seg(seglist);
        return false;
    }
    u32 process = 0;
    if (!dos_->create_program_process(name, seglist, stack_size, current_lock, process)) {
        dos_->unlock_path(current_lock);
        dos_->unload_seg(seglist);
        LogLine().text("workbench: cannot make a process for ").text(name).send();
        return false;
    }
    u32 locks[2]{};
    u32 lock_count = 0;
    u32 message_bytes = 0;
    const u32 message =
        build_startup(process, seglist, args, nargs, tool_window, locks, lock_count, message_bytes);
    // The process is scheduled first, so the message's signal reaches a
    // task exec knows; it cannot run before this call is over, and by then
    // the message is on its port.
    machine_->cpu.d[0] = 0;
    machine_->cpu.a[0] = 0;
    const u32 port = process + ndk::pr_MsgPort;
    if (message == 0 || exec_->spawn(process, (seglist << 2) + 4, 0, true) == 0) {
        for (u32 i = 0; i < lock_count; ++i) {
            dos_->unlock_path(locks[i]);
        }
        if (message != 0) {
            exec_->free(message, message_bytes);
        }
        dos_->end_process(process);
        dos_->unload_seg(seglist);
        LogLine().text("workbench: cannot start ").text(name).send();
        return false;
    }
    if (!exec_->post_message(port, message)) {
        LogLine().text("workbench: no startup message for ").text(name).send();
    }
    record->used = true;
    record->message = message;
    record->message_bytes = message_bytes;
    record->process = process;
    record->seglist = seglist;
    for (u32 i = 0; i < lock_count; ++i) {
        record->locks[i] = locks[i];
    }
    record->lock_count = lock_count;
    copy_text(record->name, sizeof record->name, name);
    LogLine().text("workbench: started ").text(name).send();
    return true;
}

bool Workbench::start_tool(const IconEntry& icon, const char* drawer) {
    char path[Dos::kMaxPath];
    if (!join_path(drawer, icon.file, path)) {
        LogLine().text("workbench: cannot start ").text(icon.file).send();
        return false;
    }
    const u32 seglist = dos_->load_seg(path);
    if (seglist == 0) {
        LogLine().text("workbench: cannot load ").text(path).send();
        return false;
    }
    char window_title[kMaxWindowTitle];
    window_title[0] = '\0';
    if (icon.tool_window != 0) {
        Bus b(*machine_);
        if (!b.string(icon.tool_window, window_title, sizeof window_title)) {
            window_title[0] = '\0';
        }
    }
    const StartupArg args[1] = {{drawer, icon.file}};
    return start_program(icon.file, seglist, drawer, args, 1, window_title, icon.stack_size);
}

bool Workbench::split_tool(const char* reference, const char* drawer,
                           char (&dir)[Dos::kMaxPath], char (&name)[Dos::kMaxPath]) const {
    i32 slash = -1;
    i32 colon = -1;
    for (i32 i = 0; reference[i] != '\0'; ++i) {
        if (reference[i] == '/') {
            slash = i;
        } else if (reference[i] == ':') {
            colon = i;
        }
    }
    if (slash >= 0) {
        copy_text(dir, Dos::kMaxPath, reference);
        dir[static_cast<u32>(slash)] = '\0';
        copy_text(name, Dos::kMaxPath, reference + slash + 1);
        return name[0] != '\0';
    }
    if (colon >= 0) {
        copy_text(dir, Dos::kMaxPath, reference);
        dir[static_cast<u32>(colon) + 1] = '\0';
        copy_text(name, Dos::kMaxPath, reference + colon + 1);
        return name[0] != '\0';
    }
    copy_text(name, Dos::kMaxPath, reference);
    copy_text(dir, Dos::kMaxPath, drawer);
    char probe[Dos::kMaxPath];
    if (join_path(drawer, reference, probe) &&
        dos_->examine(probe).kind != Dos::TargetKind::Missing) {
        return true;
    }
    copy_text(dir, Dos::kMaxPath, "C:");
    return name[0] != '\0';
}

bool Workbench::start_project(const IconEntry& icon, const char* drawer) {
    char reference[Dos::kMaxPath];
    reference[0] = '\0';
    Bus b(*machine_);
    const u32 tool_at = icon.object != 0 ? b.r32(icon.object + do_DefaultTool) : 0;
    if (tool_at == 0 || !b.string(tool_at, reference, sizeof reference) ||
        reference[0] == '\0') {
        LogLine().text("workbench: ").text(icon.file).text(" has no default tool").send();
        return false;
    }
    char tool_dir[Dos::kMaxPath];
    char tool_name[Dos::kMaxPath];
    if (!split_tool(reference, drawer, tool_dir, tool_name)) {
        LogLine().text("workbench: bad default tool ").text(reference).send();
        return false;
    }
    char tool_path[Dos::kMaxPath];
    if (!join_path(tool_dir, tool_name, tool_path)) {
        return false;
    }
    const u32 seglist = dos_->load_seg(tool_path);
    if (seglist == 0) {
        LogLine().text("workbench: cannot load ").text(tool_path).send();
        return false;
    }
    char window_title[kMaxWindowTitle];
    window_title[0] = '\0';
    if (icon.tool_window != 0 &&
        !b.string(icon.tool_window, window_title, sizeof window_title)) {
        window_title[0] = '\0';
    }
    const StartupArg args[2] = {{tool_dir, tool_name}, {drawer, icon.file}};
    return start_program(tool_name, seglist, drawer, args, 2, window_title, icon.stack_size);
}

bool Workbench::launch(const IconEntry& icon, const char* drawer) {
    if (icon.type == kProject) {
        return start_project(icon, drawer);
    }
    return start_tool(icon, drawer);
}

void Workbench::release_tool(RunningTool& tool) {
    dos_->unload_seg(tool.seglist);
    for (u32 i = 0; i < tool.lock_count; ++i) {
        dos_->unlock_path(tool.locks[i]);
    }
    exec_->free(tool.message, tool.message_bytes);
    tool = RunningTool{};
}

bool Workbench::tool_reply(u32 message) {
    for (RunningTool& tool : tools_) {
        if (!tool.used || tool.message != message) {
            continue;
        }
        LogLine().text("workbench: ").text(tool.name).text(" ended").send();
        release_tool(tool);
        return true;
    }
    LogLine().text("workbench: reply to no running program").send();
    return false;
}

void Workbench::reap_tools() {
    for (RunningTool& tool : tools_) {
        if (!tool.used || exec_->scheduler().find(tool.process) >= 0) {
            continue;
        }
        LogLine().text("workbench: ").text(tool.name).text(" ended without a reply").send();
        release_tool(tool);
    }
}

void Workbench::start_wbstartup() {
    char names[kMaxDirEntries][32]{};
    struct Collect {
        char (*names)[32];
        u32 count;
    } collect{names, 0};
    auto visit = [](void* at, const char* name, bool is_dir) {
        Collect& c = *static_cast<Collect*>(at);
        if (is_dir || c.count >= kMaxDirEntries) {
            return;
        }
        copy_text(c.names[c.count++], 32, name);
    };
    u32 error = 0;
    if (!dos_->list_dir(kWbStartupDrawer, visit, &collect, error)) {
        return;
    }
    for (u32 i = 1; i < collect.count; ++i) {
        char name[32];
        copy_text(name, sizeof name, names[i]);
        u32 j = i;
        for (; j > 0 && less_ci(name, names[j - 1]); --j) {
            copy_text(names[j], 32, names[j - 1]);
        }
        copy_text(names[j], 32, name);
    }
    for (u32 i = 0; i < collect.count; ++i) {
        if (!ends_with_ci(names[i], ".info")) {
            continue;
        }
        char stem[32];
        file_stem(names[i], stem);
        char path[Dos::kMaxPath];
        if (stem[0] == '\0' || !join_path(kWbStartupDrawer, stem, path)) {
            continue;
        }
        const u32 object = icon_->load_object(path);
        if (object == 0) {
            continue;
        }
        Bus b(*machine_);
        IconEntry icon;
        icon.object = object;
        icon.owned = true;
        icon.type = static_cast<u8>(b.r8(object + do_Type));
        copy_text(icon.file, sizeof icon.file, stem);
        copy_text(icon.name, sizeof icon.name, stem);
        measure(icon);
        if (icon.type == kTool || icon.type == kProject) {
            launch(icon, kWbStartupDrawer);
        }
        icon_->free_object(object);
    }
}

// --- the desktop as a whole ---------------------------------------------

void Workbench::open_desktop() {
    if (desktop_open_ || machine_ == nullptr) {
        return;
    }
    screen_ = intuition_->default_screen();
    if (screen_ == 0) {
        LogLine().text("workbench: no screen").send();
        return;
    }
    reply_port_ = exec_->create_port();
    if (reply_port_ == 0) {
        LogLine().text("workbench: no message port").send();
        return;
    }
    menu_ = build_menu_strip();
    open_backdrop();
    if (!windows_[0].used) {
        return;
    }
    add_volume_icons();
    draw_window(0);
    start_wbstartup();
    desktop_open_ = true;
    refresh_title();
    next_refresh_us_ = exec_->now_us() + kRefreshUs;
}

u32 Workbench::start(u32 flags, u32 path) {
    (void)flags;
    (void)path;
    if (running_) {
        return ndk::DOSTRUE;
    }
    if (machine_ == nullptr || dos_ == nullptr) {
        return ndk::DOSFALSE;
    }
    u32 process = 0;
    if (!dos_->create_native_process(kProcessName, kTaskStackBytes, process)) {
        LogLine().text("workbench: no process").send();
        return ndk::DOSFALSE;
    }
    if (exec_->spawn_native(process, Workbench::task_entry, this, 1, true) == 0) {
        dos_->end_process(process);
        LogLine().text("workbench: cannot start").send();
        return ndk::DOSFALSE;
    }
    running_ = true;
    process_ = process;
    LogLine().text("workbench: started").send();
    return ndk::DOSTRUE;
}

bool Workbench::update_workbench(u32 name) {
    if (name == 0 || machine_ == nullptr) {
        return false;
    }
    char text[Dos::kMaxPath];
    Bus b(*machine_);
    if (!b.string(name, text, sizeof text)) {
        return false;
    }
    for (u32 i = 1; i < kMaxWindows; ++i) {
        if (!windows_[i].used) {
            continue;
        }
        if (same_ci(windows_[i].path, text) || same_ci(windows_[i].name, text)) {
            fill_drawer(i);
            draw_window(i);
            return true;
        }
    }
    return false;
}

// --- application registrations -------------------------------------------

u32 Workbench::record_app(AppKind kind, u32 id, u32 user) {
    for (App& app : apps_) {
        if (app.used) {
            continue;
        }
        const u32 handle = exec_->alloc(kAppBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (handle == 0) {
            return 0;
        }
        app = App{};
        app.used = true;
        app.kind = kind;
        app.handle = handle;
        app.id = id;
        app.user = user;
        return handle;
    }
    LogLine().text("workbench: application table full").send();
    return 0;
}

Workbench::App* Workbench::find_app(u32 handle, AppKind kind) {
    for (App& app : apps_) {
        if (app.used && app.handle == handle && app.kind == kind) {
            return &app;
        }
    }
    return nullptr;
}

void Workbench::drop_app_icon(const App& app) {
    if (app.icon == kNoIcon || app.icon >= kMaxIcons) {
        return;
    }
    IconEntry& icon = icons_[app.icon];
    if (icon.used && icon.owner == 0) {
        icon = IconEntry{};
        if (clicks_[0].selected() == app.icon) {
            clicks_[0].clear();
        }
        draw_window(0);
    }
}

u32 Workbench::add_app_window(u32 id, u32 user, u32 window, u32 msg_port, u32 tags) {
    (void)window;
    (void)msg_port;
    (void)tags;
    return record_app(AppKind::Window, id, user);
}

bool Workbench::remove_app_window(u32 handle) {
    App* app = find_app(handle, AppKind::Window);
    if (app == nullptr) {
        return false;
    }
    exec_->free(app->handle, kAppBytes);
    *app = App{};
    return true;
}

u32 Workbench::add_app_icon(u32 id, u32 user, u32 name, u32 msg_port, u32 lock, u32 object,
                            u32 tags) {
    (void)msg_port;
    (void)lock;
    (void)tags;
    if (object == 0) {
        return 0;
    }
    const u32 handle = record_app(AppKind::Icon, id, user);
    if (handle == 0) {
        return 0;
    }
    App* app = find_app(handle, AppKind::Icon);
    if (!windows_[0].used) {
        return handle;
    }
    IconEntry* icon = take_icon(0);
    if (icon == nullptr) {
        return handle;
    }
    char label[32];
    label[0] = '\0';
    if (name != 0) {
        Bus b(*machine_);
        if (!b.string(name, label, sizeof label)) {
            label[0] = '\0';
        }
    }
    icon->type = kAppIcon;
    icon->object = object;
    icon->owned = false;
    copy_text(icon->name, sizeof icon->name, label);
    copy_text(icon->file, sizeof icon->file, label);
    measure(*icon);
    const DrawerWindow& backdrop = windows_[0];
    i32 left = backdrop.inner_width - icon->width - kDiskMargin;
    i32 top = volume_top_ == 0 ? kVolumeTop : volume_top_;
    if (top + icon->height + static_cast<i32>(kLabelRows) > backdrop.inner_height) {
        left -= icon->width + kIconGapX;
        top = kVolumeTop;
    }
    icon->left = left < 0 ? 0 : left;
    icon->top = top;
    volume_top_ = top + icon->height + static_cast<i32>(kLabelRows) + kIconGapY;
    app->icon = static_cast<u32>(icon - icons_);
    draw_window(0);
    return handle;
}

bool Workbench::remove_app_icon(u32 handle) {
    App* app = find_app(handle, AppKind::Icon);
    if (app == nullptr) {
        return false;
    }
    drop_app_icon(*app);
    exec_->free(app->handle, kAppBytes);
    *app = App{};
    return true;
}

u32 Workbench::add_app_menu_item(u32 id, u32 user, u32 text, u32 msg_port, u32 tags) {
    (void)text;
    (void)msg_port;
    (void)tags;
    return record_app(AppKind::MenuItem, id, user);
}

bool Workbench::remove_app_menu_item(u32 handle) {
    App* app = find_app(handle, AppKind::MenuItem);
    if (app == nullptr) {
        return false;
    }
    exec_->free(app->handle, kAppBytes);
    *app = App{};
    return true;
}

// --- the vectors ---------------------------------------------------------

struct WorkbenchNatives {
    static Workbench& of(void* ctx) { return *static_cast<Workbench*>(ctx); }

    static void update_workbench(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).update_workbench(m.cpu.a[0]) ? ndk::DOSTRUE
                                                                  : ndk::DOSFALSE);
    }

    static void quote_workbench(void*, Machine& m) { native::set_result(m, ndk::DOSTRUE); }

    static void start_workbench(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).start(m.cpu.d[0], m.cpu.d[1]));
    }

    // AddAppWindowA: d0 id, d1 userdata, a0 window, a1 msgport, a2 tags.
    static void add_app_window(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).add_app_window(m.cpu.d[0], m.cpu.d[1], m.cpu.a[0],
                                                     m.cpu.a[1], m.cpu.a[2]));
    }

    // Each Remove call gives TRUE for a handle its Add call handed out.
    static void remove_app_window(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).remove_app_window(m.cpu.a[0]) ? 1 : 0);
    }

    // AddAppIconA: d0 id, d1 userdata, a0 text, a1 msgport, a2 lock, a3
    // diskobj, a4 tags.
    static void add_app_icon(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).add_app_icon(m.cpu.d[0], m.cpu.d[1], m.cpu.a[0],
                                                   m.cpu.a[1], m.cpu.a[2], m.cpu.a[3],
                                                   m.cpu.a[4]));
    }

    static void remove_app_icon(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).remove_app_icon(m.cpu.a[0]) ? 1 : 0);
    }

    static void add_app_menu_item(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).add_app_menu_item(m.cpu.d[0], m.cpu.d[1], m.cpu.a[0],
                                                        m.cpu.a[1], m.cpu.a[2]));
    }

    static void remove_app_menu_item(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).remove_app_menu_item(m.cpu.a[0]) ? 1 : 0);
    }

    static void wb_info(void*, Machine& m) { native::set_result(m, 0); }
};

namespace {

constexpr Function kFunctions[] = {
    {workbench_lvo::UpdateWorkbench, WorkbenchNatives::update_workbench, "UpdateWorkbench"},
    {workbench_lvo::QuoteWorkbench, WorkbenchNatives::quote_workbench, "QuoteWorkbench"},
    {workbench_lvo::StartWorkbench, WorkbenchNatives::start_workbench, "StartWorkbench"},
    {workbench_lvo::AddAppWindowA, WorkbenchNatives::add_app_window, "AddAppWindowA"},
    {workbench_lvo::RemoveAppWindow, WorkbenchNatives::remove_app_window, "RemoveAppWindow"},
    {workbench_lvo::AddAppIconA, WorkbenchNatives::add_app_icon, "AddAppIconA"},
    {workbench_lvo::RemoveAppIcon, WorkbenchNatives::remove_app_icon, "RemoveAppIcon"},
    {workbench_lvo::AddAppMenuItemA, WorkbenchNatives::add_app_menu_item, "AddAppMenuItemA"},
    {workbench_lvo::RemoveAppMenuItem, WorkbenchNatives::remove_app_menu_item,
     "RemoveAppMenuItem"},
    {workbench_lvo::WBInfo, WorkbenchNatives::wb_info, "WBInfo"},
};

}  // namespace

bool Workbench::init(Machine& m, Exec& exec, Dos& dos, Graphics& graphics, Intuition& intuition,
                     GadTools& gadtools, Icon& icon) {
    machine_ = &m;
    exec_ = &exec;
    dos_ = &dos;
    graphics_ = &graphics;
    intuition_ = &intuition;
    gadtools_ = &gadtools;
    icon_ = &icon;
    base_ = 0;
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const Function& f : kFunctions) {
        vectors_[f.lvo / ndk::LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }
    const u32 id_string = exec.alloc(sizeof kIdString, ndk::MEMF_PUBLIC);
    if (id_string == 0) {
        return false;
    }
    const u32 base =
        exec.make_native_library(kLibraryName, kHombreLibVersion, vectors_, kVectors, this);
    if (base == 0) {
        exec.free(id_string, sizeof kIdString);
        return false;
    }
    Bus b(m);
    for (u32 i = 0; i < sizeof kIdString; ++i) {
        b.w8(id_string + i, static_cast<u8>(kIdString[i]));
    }
    b.w16(base + ndk::LIB_REVISION, kHombreLibRevision);
    b.w32(base + ndk::LIB_IDSTRING, id_string);
    if (!b.ok()) {
        return false;
    }
    base_ = base;
    return true;
}

}
