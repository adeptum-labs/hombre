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

#include "chrome.hpp"

#include "../dos_date.hpp"
#include "../format.hpp"

#include <algorithm>
#include <initializer_list>

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
constexpr char kBackdropFile[] = ".backdrop";
// More than any list of left out icons needs.
constexpr u32 kMaxBackdropBytes = 16384;

// struct DiskObject (workbench/workbench.h): the Gadget comes first.
constexpr u32 do_Gadget = 4;
constexpr u32 do_Type = 48;
constexpr u32 do_DefaultTool = 50;
constexpr u32 do_ToolTypes = 54;
constexpr u32 do_CurrentX = 58;
constexpr u32 do_CurrentY = 62;
constexpr u32 do_DrawerData = 66;
constexpr u32 do_ToolWindow = 70;
constexpr u32 do_StackSize = 74;
constexpr u32 kNoIconPosition = 0x80000000;

// struct DrawerData after its NewWindow: where the window was scrolled to
// and which icons its window shows.
constexpr u32 dd_CurrentX = 48;
constexpr u32 dd_CurrentY = 52;
constexpr u32 dd_Flags = 56;
constexpr u32 DDFLAGS_SHOWDEFAULT = 0;  // as the window it was opened from
constexpr u32 DDFLAGS_SHOWICONS = 1;
constexpr u32 DDFLAGS_SHOWALL = 2;
static_assert(DDFLAGS_SHOWDEFAULT == 0 && DDFLAGS_SHOWICONS == 1 && DDFLAGS_SHOWALL == 2);
constexpr u32 kMinStack = 4096;

// struct Image (intuition/intuition.h).
constexpr u32 im_LeftEdge = 0;
constexpr u32 im_TopEdge = 2;
constexpr u32 im_Width = 4;
constexpr u32 im_Height = 6;
constexpr u32 im_Depth = 8;
constexpr u32 im_ImageData = 10;
constexpr u32 im_PlanePick = 14;
constexpr u32 Image_SIZEOF = 20;

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
constexpr u8 NM_SUB = 3;
constexpr u16 NM_ITEMDISABLED = 0x0010;
constexpr u16 CHECKIT = 0x0001;
constexpr u16 MENUTOGGLE = 0x0008;
constexpr u16 CHECKED = 0x0100;

// struct Menu and struct MenuItem (intuition/intuition.h).
constexpr u32 mu_NextMenu = 0;
constexpr u32 mu_FirstItem = 18;
constexpr u32 mi_NextItem = 0;
constexpr u32 mi_Flags = 12;
constexpr u32 mi_SubItem = 28;

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
constexpr i32 kDiskMargin = 8;
constexpr u32 kDiskLabelChars = 13;  // the widest label a disk column fits
constexpr i32 kVolumeTop = 16;
constexpr i32 kLabelBaseline = 10;  // from the bottom of the image to a topaz label's baseline
constexpr i32 kTopazBaseline = 6;   // the built-in font's rows above its baseline
constexpr i32 kLabelGap = kLabelBaseline - kTopazBaseline;  // blank rows above a label

// The struct IPrefs passes over for the icon and the system font: the
// TextAttr and TextFont it opened, the draw mode and pens to draw text
// with, and a TextAttr and name the struct keeps.
constexpr u32 fc_TextFont = 4;
constexpr u32 fc_DrawMode = 8;
constexpr u32 fc_FrontPen = 10;
constexpr u32 fc_BackPen = 11;
// The raised frame round an icon's image, as on 3.1: how far its lines sit
// out from the image on either side and above and below it.
constexpr i32 kFrameOutX = 3;
constexpr i32 kFrameOutY = 3;
constexpr u32 kFrameShinePen = 2;
constexpr u32 kFrameShadowPen = 1;
constexpr i32 kIconGapX = 16;
constexpr i32 kIconGapY = 12;
constexpr i32 kCellGapX = 16;
constexpr i32 kCellGapY = 12;
constexpr i32 kGridMarginX = 8;
constexpr i32 kGridMarginY = 8;
constexpr i32 kDefaultDrawerWidth = 320;
constexpr i32 kDefaultDrawerHeight = 160;
constexpr u32 kMinDrawerWidth = 90;
// Tall enough for the title bar, both vertical arrows and a slider.
constexpr u32 kMinDrawerHeight = 56;
constexpr i32 kCascadeX = 40;
constexpr i32 kCascadeY = 44;
constexpr i32 kCascadeStep = 20;
constexpr u32 kCascadeSlots = 7;
constexpr u32 kMaxWindowTitle = 64;
constexpr u32 kClipBytes = ndk::Region_SIZEOF + ndk::RegionRectangle_SIZEOF;
// The WA_Zoom tag and the end of the list, then the four WORDs of the box it
// points to.
constexpr u32 kZoomTagBytes = 2 * ndk::ti_SIZEOF + 8;

// AmigaDOS protection bits: the four that deny an access are active low, so
// their check boxes show an allowed access; archive, pure, script and hold
// are set flags. Only the bits with a check box in the information window
// are here; the others stay as the object had them.
constexpr u32 kProtectDelete = 1u << 0;
constexpr u32 kProtectExecute = 1u << 1;
constexpr u32 kProtectWrite = 1u << 2;
constexpr u32 kProtectRead = 1u << 3;
constexpr u32 kProtectArchive = 1u << 4;
constexpr u32 kProtectScript = 1u << 6;

struct ProtectionBox {
    u16 id;
    u32 bit;
    bool denies;     // checked while the bit is clear
    bool on_drawer;  // a drawer has the box too
    const char* label;
};

// The boxes from the top down, a row each.
constexpr ProtectionBox kProtectionBoxes[] = {
    {Workbench::kInfoScript, kProtectScript, false, false, "Script"},
    {Workbench::kInfoArchived, kProtectArchive, false, true, "Archived"},
    {Workbench::kInfoReadable, kProtectRead, true, true, "Readable"},
    {Workbench::kInfoWritable, kProtectWrite, true, true, "Writable"},
    {Workbench::kInfoExecutable, kProtectExecute, true, false, "Executable"},
    {Workbench::kInfoDeletable, kProtectDelete, true, true, "Deletable"},
};
constexpr u32 kProtectionBoxCount = sizeof kProtectionBoxes / sizeof kProtectionBoxes[0];
// The characters of the longest of the labels.
constexpr i32 kProtectionLabelChars = 10;

// The type a Workbench object has, as the information window names it.
const char* object_type_name(u8 type) {
    switch (type) {
    case Workbench::kDrawer:
        return "Drawer";
    case Workbench::kTool:
        return "Tool";
    case Workbench::kProject:
        return "Project";
    case Workbench::kGarbage:
        return "Trashcan";
    case Workbench::kDisk:
        return "Volume";
    default:
        return "Unknown";
    }
}

// The scroll gadgets in the borders WFLG_SIZEBRIGHT and WFLG_SIZEBBOTTOM
// make (14 pixels on the right and at the bottom), around the size gadget
// in the corner. Their boxes follow the window's size: a RELRIGHT edge
// counts from the right edge, a RELWIDTH size from the window's width.
// A slider's thickness is set when it is made, from the screen's aspect.
struct ScrollGadgetSpec {
    u16 id;
    i16 left;
    i16 top;
    i16 width;
    i16 height;
    u16 flags;
    u16 activation;
    u8 arrow;  // the chrome::Arrow image, kNoArrow for a slider
};
constexpr u8 kNoArrow = 0xFF;
constexpr u16 kArrowFlags = ndk::GFLG_RELRIGHT | ndk::GFLG_RELBOTTOM | ndk::GFLG_GADGIMAGE |
                            ndk::GFLG_GADGHIMAGE;
constexpr u16 kSliderActivation =
    ndk::GACT_RELVERIFY | ndk::GACT_IMMEDIATE | ndk::GACT_FOLLOWMOUSE;
constexpr u16 kArrowActivation = ndk::GACT_RELVERIFY | ndk::GACT_IMMEDIATE;
constexpr u8 kUp = static_cast<u8>(chrome::Arrow::Up);
constexpr u8 kDown = static_cast<u8>(chrome::Arrow::Down);
constexpr u8 kLeft = static_cast<u8>(chrome::Arrow::Left);
constexpr u8 kRight = static_cast<u8>(chrome::Arrow::Right);
// The sliders come first, so their PropInfos and knobs pair with the
// first two gadgets.
constexpr ScrollGadgetSpec kScrollSpecs[Workbench::kScrollGadgets] = {
    {Workbench::kScrollVert, -12, 16, 12, -50, ndk::GFLG_RELRIGHT | ndk::GFLG_RELHEIGHT,
     kSliderActivation | ndk::GACT_RIGHTBORDER, kNoArrow},
    {Workbench::kScrollHoriz, 3, -12, -45, 12, ndk::GFLG_RELBOTTOM | ndk::GFLG_RELWIDTH,
     kSliderActivation | ndk::GACT_BOTTOMBORDER, kNoArrow},
    {Workbench::kScrollUp, -13, -33, 14, 10, kArrowFlags,
     kArrowActivation | ndk::GACT_RIGHTBORDER, kUp},
    {Workbench::kScrollDown, -13, -23, 14, 10, kArrowFlags,
     kArrowActivation | ndk::GACT_RIGHTBORDER, kDown},
    {Workbench::kScrollLeft, -41, -13, 14, 14, kArrowFlags,
     kArrowActivation | ndk::GACT_BOTTOMBORDER, kLeft},
    {Workbench::kScrollRight, -27, -13, 14, 14, kArrowFlags,
     kArrowActivation | ndk::GACT_BOTTOMBORDER, kRight},
};
// The room for a slider across its border: twelve pixels in each, a
// pixel clear of each edge.
constexpr i32 kVertSliderRoom = 12;
constexpr i32 kHorizSliderRoom = 12;
constexpr i32 kMinSlider = 4;
constexpr u32 kSliders = 2;
constexpr u16 kVertSliderFlags = ndk::AUTOKNOB | ndk::FREEVERT | ndk::PROPBORDERLESS |
                                 ndk::PROPNEWLOOK;
constexpr u16 kHorizSliderFlags = ndk::AUTOKNOB | ndk::FREEHORIZ | ndk::PROPBORDERLESS |
                                  ndk::PROPNEWLOOK;
constexpr u32 kPropInfoAt = Workbench::kScrollGadgets * ndk::Gadget_SIZEOF;
constexpr u32 kKnobAt = kPropInfoAt + kSliders * ndk::PropInfo_SIZEOF;
constexpr u32 kScrollBlockBytes = kKnobAt + kSliders * Image_SIZEOF;

// The fuel gauge of a volume window: a column in its left border, from two
// pixels below the window's inner top to its inner bottom, as wide as the
// look of the screen has it and never more than a word of image data. A
// window with fewer than kGaugeMinHeight rows for it shows none.
constexpr i32 kGaugeInset = 2;
constexpr u32 kGaugeRowBytes = 2;
constexpr i32 kGaugeMinHeight = 6;
static_assert(chrome::fuel_gauge_width(chrome::Style::Classic) <= 16 &&
              chrome::fuel_gauge_width(chrome::Style::Modern) <= 16);

// The arrow images, each plain and pressed, as deep as the screen.
constexpr u32 kArrowImages = 8;
// The up and down arrows are 14 by 10, the left and right ones 14 by 14;
// every image keeps room for the larger, a word a row.
constexpr i32 kArrowWidth = 14;
constexpr i32 kArrowMaxHeight = 14;
constexpr u32 kArrowRowBytes = 2;
constexpr u32 kArrowPlaneBytes = kArrowMaxHeight * kArrowRowBytes;
constexpr u32 kMaxArrowDepth = 8;

// struct DrawInfo (intuition/screens.h): the screen's depth and the
// aspect of its pixels.
constexpr u32 dri_Depth = 12;
constexpr u32 dri_ResolutionX = 14;
constexpr u32 dri_ResolutionY = 16;

// The menu labels, in the order the menus number them.
constexpr u16 kDisabled = NM_ITEMDISABLED;

struct MenuItemSpec {
    const char* label;
    bool enabled;
    bool toggle = false;  // a check mark each pick turns on or off
    const MenuItemSpec* subs = nullptr;  // the items of its submenu
    u32 sub_count = 0;
    // A check mark that stays on when picked; the check-marked items of a
    // submenu exclude each other.
    bool check = false;
    bool checked = false;  // its check mark when the strip is built
};

struct MenuSpec {
    const char* title;
    const MenuItemSpec* items;
    u32 count;
};

constexpr MenuItemSpec kWorkbenchItems[] = {
    {"Backdrop", true, true}, {"Execute Command...", true}, {"Redraw All", true},
    {"Update All", true}, {"Last Message", true},        {"About...", true},
    {"Quit", true},
};

constexpr MenuItemSpec kSnapshotItems[] = {
    {"Window", true},
    {"All", true},
};

constexpr MenuItemSpec kShowItems[] = {
    {"Only Icons", true, false, nullptr, 0, true, true},
    {"All Files", true, false, nullptr, 0, true},
};

constexpr MenuItemSpec kWindowItems[] = {
    {"New Drawer", true},
    {"Open Parent", true},
    {"Close", true},
    {"Update", true},
    {"Select Contents", true},
    {"Clean Up", true},
    {"Snapshot", true, false, kSnapshotItems, sizeof kSnapshotItems / sizeof kSnapshotItems[0]},
    {"Show", true, false, kShowItems, sizeof kShowItems / sizeof kShowItems[0]},
    {"View By", false},
};

constexpr MenuItemSpec kIconsItems[] = {
    {"Open", true},          {"Copy", true},         {"Rename...", true},
    {"Information...", true}, {"Snapshot", true},    {"Unsnapshot", true},
    {"Leave Out", true},     {"Put Away", true},      {"Delete...", true},
    {"Format Disk...", false}, {"Empty Trash", true},
};

constexpr MenuItemSpec kToolsItems[] = {
    {"ResetWB", true},
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
constexpr u32 kMenuTools = 3;
constexpr u32 kItemUpdateAll = 3;
constexpr u32 kItemLastMessage = 4;
constexpr u32 kItemAbout = 5;
constexpr u32 kItemQuit = 6;
constexpr u32 kItemEmptyTrash = 10;
constexpr u32 kItemResetWb = 0;
constexpr u32 kItemBackdrop = 0;
constexpr u32 kItemExecute = 1;
constexpr u32 kItemNewDrawer = 0;
constexpr u32 kItemWindowUpdate = 3;
constexpr u32 kItemWindowSnapshot = 6;
constexpr u32 kItemWindowShow = 7;
constexpr u32 kShowItemCount = 2;  // Only Icons and All Files
constexpr u32 kItemIconSnapshot = 4;
constexpr u32 kItemIconCopy = 1;
constexpr u32 kItemIconRename = 2;
constexpr u32 kItemIconInformation = 3;
constexpr u32 kItemIconUnsnapshot = 5;
constexpr u32 kItemIconLeaveOut = 6;
constexpr u32 kItemIconPutAway = 7;
constexpr u32 kItemIconDelete = 8;
constexpr u32 kSubAll = 1;
constexpr u32 kSubAllFiles = 1;
constexpr u32 kNoSub = 31;

constexpr u32 text_length(const char* text) {
    u32 n = 0;
    while (text[n] != '\0') {
        ++n;
    }
    return n;
}

char upper_ascii(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

bool same_text(const char* a, const char* b) {
    u32 i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        ++i;
    }
    return a[i] == b[i];
}

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

bool starts_with_ci(const char* text, const char* prefix) {
    const u32 prefix_len = text_length(prefix);
    if (prefix_len > text_length(text)) {
        return false;
    }
    char head[Dos::kMaxPath];
    u32 i = 0;
    for (; i < prefix_len && i + 1 < sizeof head; ++i) {
        head[i] = text[i];
    }
    head[i] = '\0';
    return same_ci(head, prefix);
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
// The text of a requester, cut off where it does not fit.
struct Message {
    char text[Dos::kMaxPath + 128]{};
    u32 at = 0;

    Message& add(const char* part) {
        for (u32 i = 0; part[i] != '\0' && at + 1 < sizeof text; ++i) {
            text[at++] = part[i];
        }
        return *this;
    }
    Message& number(u64 value) {
        char digits[24];
        format_u64(digits, sizeof digits, value, 10);
        return add(digits);
    }
    // The number with a space between its thousands.
    Message& grouped(u64 value) {
        char digits[24];
        format_u64(digits, sizeof digits, value, 10);
        const u32 count = text_length(digits);
        for (u32 i = 0; i < count && at + 2 < sizeof text; ++i) {
            if (i != 0 && (count - i) % 3 == 0) {
                text[at++] = ' ';
            }
            text[at++] = digits[i];
        }
        return *this;
    }
};

// Writes a tag list from pairs of tag and data, ending it with TAG_DONE.
u32 write_tags(Bus& b, u32 at, std::initializer_list<u32> words) {
    u32 p = at;
    for (const u32 word : words) {
        b.w32(p, word);
        p += 4;
    }
    b.w32(p, ndk::TAG_DONE);
    return at;
}

// Builds the window of a dialog, sized by the screen's font. Its block of
// exec memory holds the NewWindow, the NewGadget and tags of the gadget
// being made and the strings the gadgets keep, and room a dialog asks for.
class DialogBuilder {
public:
    static constexpr i32 kMargin = 8;
    static constexpr u32 kTagWords = 10;

    DialogBuilder(m68k::Machine& m, Exec& exec, Graphics& graphics, Intuition& intuition,
                  GadTools& gadtools, u32 screen, Workbench::Dialog& dialog,
                  std::initializer_list<const char*> strings, u32 extra_bytes = 0,
                  i32 columns = 40)
        : bus_(m), intuition_(intuition), gadtools_(gadtools), dialog_(dialog), screen_(screen),
          font_(graphics.font_metrics(intuition.font_of_screen(screen))),
          bar_height_(static_cast<i32>(bus_.r8(screen + ndk::sc_BarHeight))),
          screen_width_(static_cast<i32>(bus_.r16(screen + ndk::sc_Width))),
          screen_height_(static_cast<i32>(bus_.r16(screen + ndk::sc_Height))),
          columns_(columns) {
        u32 bytes = kList + 4;
        for (const char* text : strings) {
            bytes += text_length(text) + 1;
        }
        // The structs in the extra room need an even address.
        bytes = (bytes + 3) & ~3u;
        dialog.block = exec.alloc(bytes + extra_bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        dialog.block_bytes = bytes + extra_bytes;
        extra_ = dialog.block != 0 ? dialog.block + bytes : 0;
        at_ = dialog.block + kList + 4;
        if (dialog.block != 0) {
            dialog.gadgets = gadtools.create_context(dialog.block + kList);
            previous_ = dialog.gadgets;
        }
    }

    i32 width() const {
        const i32 wanted = columns_ * font_.x_size;
        const i32 room = screen_width_ - 2 * kCascadeX;
        return wanted < room ? wanted : room;
    }
    i32 inner_width() const { return width() - 2 * kMargin; }
    i32 line_height() const { return font_.y_size + 6; }
    i32 char_width() const { return font_.x_size; }
    i32 font_height() const { return font_.y_size; }
    i32 top() const { return bar_height_ + kMargin; }
    // The rows a window can take under the screen's bar.
    i32 room_below_bar() const { return screen_height_ - bar_height_ - 1; }
    // The room after the strings, which a dialog lays out itself.
    u32 extra() const { return extra_; }

    // Copies one of the strings the block was sized for into it.
    u32 put(const char* text) {
        const u32 start = at_;
        if (dialog_.block != 0) {
            for (u32 i = 0; text[i] != '\0'; ++i) {
                bus_.w8(at_++, static_cast<u8>(text[i]));
            }
            bus_.w8(at_++, 0);
        }
        return start;
    }

    // The tag list of the next gadget made, from pairs of tag and data.
    u32 tags(std::initializer_list<u32> words) {
        return dialog_.block != 0 ? write_tags(bus_, dialog_.block + kTags, words) : 0;
    }

    u32 add(u32 kind, u16 id, i32 left, i32 top, i32 width, u32 label, u32 place,
            u32 gadget_tags = 0, i32 height = 0) {
        using namespace gadtools_defs;
        if (previous_ == 0) {
            return 0;
        }
        const u32 gadget = dialog_.block + kNewGadget;
        bus_.w16(gadget + ng_LeftEdge, static_cast<u32>(left));
        bus_.w16(gadget + ng_TopEdge, static_cast<u32>(top));
        bus_.w16(gadget + ng_Width, static_cast<u32>(width));
        bus_.w16(gadget + ng_Height, static_cast<u32>(height > 0 ? height : line_height()));
        bus_.w32(gadget + ng_GadgetText, label);
        bus_.w16(gadget + ng_GadgetID, id);
        bus_.w32(gadget + ng_Flags, place);
        previous_ = gadtools_.create_gadget(kind, previous_, gadget, gadget_tags);
        return previous_;
    }

    // The accepting button on the left and Cancel on the right, each
    // columns characters wide.
    void buttons(i32 top, u32 accept, u32 cancel, i32 columns = 9) {
        using namespace gadtools_defs;
        const i32 button = columns * font_.x_size;
        add(BUTTON_KIND, Workbench::kDialogAccept, kMargin, top, button, accept, PLACETEXT_IN);
        add(BUTTON_KIND, Workbench::kDialogCancel, width() - kMargin - button, top, button,
            cancel, PLACETEXT_IN);
    }

    // Opens the window and draws its gadgets; height is what they take,
    // with bottom rows under them. A window that would pass the bottom of
    // the screen opens further up.
    bool open(u32 title, i32 height, i32 bottom = kMargin) {
        if (previous_ == 0) {
            return false;
        }
        const u32 window = dialog_.block;
        const i32 rows = height + bottom;
        i32 top = 2 * bar_height_;
        if (top + rows > screen_height_) {
            top = screen_height_ > rows ? screen_height_ - rows : 0;
        }
        bus_.w16(window + ndk::nw_LeftEdge, kCascadeX);
        bus_.w16(window + ndk::nw_TopEdge, static_cast<u32>(top));
        bus_.w16(window + ndk::nw_Width, static_cast<u32>(width()));
        bus_.w16(window + ndk::nw_Height, static_cast<u32>(rows));
        bus_.w8(window + ndk::nw_BlockPen, 1);
        bus_.w32(window + ndk::nw_IDCMPFlags, ndk::IDCMP_CLOSEWINDOW | ndk::IDCMP_GADGETUP);
        bus_.w32(window + ndk::nw_Flags, ndk::WFLG_DRAGBAR | ndk::WFLG_DEPTHGADGET |
                                             ndk::WFLG_CLOSEGADGET | ndk::WFLG_ACTIVATE);
        bus_.w32(window + ndk::nw_FirstGadget, dialog_.gadgets);
        bus_.w32(window + ndk::nw_Title, title);
        bus_.w32(window + ndk::nw_Screen, screen_);
        bus_.w16(window + ndk::nw_Type, ndk::CUSTOMSCREEN);
        dialog_.window = bus_.ok() ? intuition_.open_window(window, 0) : 0;
        if (dialog_.window != 0) {
            gadtools_.refresh_window(dialog_.window);
        }
        return dialog_.window != 0;
    }

private:
    static constexpr u32 kNewGadget = ndk::NewWindow_SIZEOF;
    static constexpr u32 kTags = kNewGadget + gadtools_defs::NewGadget_SIZEOF;
    static constexpr u32 kList = kTags + kTagWords * 4;

    Bus bus_;
    Intuition& intuition_;
    GadTools& gadtools_;
    Workbench::Dialog& dialog_;
    u32 screen_;
    Graphics::FontMetrics font_;
    i32 bar_height_;
    i32 screen_width_;
    i32 screen_height_;
    i32 columns_;
    u32 extra_ = 0;
    u32 at_ = 0;
    u32 previous_ = 0;
};

// Where an information dialog keeps the tool type rows, the default tool
// and the tag list of its later gadget changes, after its strings.
constexpr u32 kInfoRowStride = Workbench::kMaxToolTypeLength + 1;
constexpr u32 kInfoRowBytes = Workbench::kMaxToolTypes * kInfoRowStride;
constexpr u32 kInfoTagWords = 10;
constexpr u32 kInfoNodesAt = ndk::LH_SIZE;
constexpr u32 kInfoRowsAt = kInfoNodesAt + Workbench::kMaxToolTypes * ndk::LN_SIZE;
constexpr u32 kInfoDefaultAt = kInfoRowsAt + kInfoRowBytes;
constexpr u32 kInfoOriginalDefaultAt = kInfoDefaultAt + Workbench::kMaxDefaultToolLength + 1;
constexpr u32 kInfoTagsAt =
    kInfoOriginalDefaultAt + Workbench::kMaxDefaultToolLength + 1;
// The Region the image is cut off at while it is drawn.
constexpr u32 kInfoClipAt = kInfoTagsAt + kInfoTagWords * 4;
constexpr u32 kInfoExtraBytes = kInfoClipAt + kClipBytes;
// The most entries a tool type array can hold, its 64K .info bounding it.
constexpr u32 kMaxToolTypeTail = Icon::kMaxInfoBytes / 5;

// Copies the NUL-terminated string at from into to, at most max bytes of it.
void copy_guest_text(Bus& b, u32 to, u32 from, u32 max) {
    u32 i = 0;
    for (; i < max; ++i) {
        const u32 c = b.r8(from + i);
        if (c == 0) {
            break;
        }
        b.w8(to + i, static_cast<u8>(c));
    }
    b.w8(to + i, 0);
}

// Whether the text equals the string at guest up to text's NUL, a text of
// max bytes counting equal into a longer string: the window shows no more.
bool same_guest_prefix(Bus& b, u32 guest, const char* text, u32 max) {
    if (guest == 0) {
        return text[0] == '\0';
    }
    for (u32 i = 0;; ++i) {
        const u32 c = b.r8(guest + i);
        if (text[i] == '\0') {
            return c == 0;
        }
        if (c != static_cast<u8>(text[i])) {
            return false;
        }
        if (i + 1 == max) {
            return true;
        }
    }
}

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

// A name a file can have: not empty, not too long and no path.
bool is_file_name(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (u32 i = 0; name[i] != '\0'; ++i) {
        if (name[i] == ':' || name[i] == '/' || i >= Workbench::kMaxName) {
            return false;
        }
    }
    return true;
}

// The path of the .info beside the object at path.
bool info_path(const char* path, char (&out)[Dos::kMaxPath]) {
    const u32 length = text_length(path);
    if (length + sizeof ".info" > Dos::kMaxPath) {
        return false;
    }
    copy_text(out, sizeof out, path);
    copy_text(out + length, sizeof ".info", ".info");
    return true;
}

// Whether path names root or something in it.
bool at_or_below(const char* path, const char* root) {
    u32 i = 0;
    while (root[i] != '\0' && upper_ascii(path[i]) == upper_ascii(root[i])) {
        ++i;
    }
    return root[i] == '\0' && (path[i] == '\0' || path[i] == '/');
}

// Cuts the path of a file down to its drawer, which ends before the '/' or
// with the volume's ':', and gives the file's name. False when the path
// names no file or one too long.
bool split_file(char* path, char (&file)[32]) {
    u32 leaf = 0;
    for (u32 i = 0; path[i] != '\0'; ++i) {
        leaf = path[i] == '/' || path[i] == ':' ? i + 1 : leaf;
    }
    if (leaf == 0 || path[leaf] == '\0' || text_length(path + leaf) >= sizeof file) {
        return false;
    }
    copy_text(file, sizeof file, path + leaf);
    path[path[leaf - 1] == '/' ? leaf - 1 : leaf] = '\0';
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

// The label under an icon in drawer coordinates: the start of its
// baseline, its top and bottom rows and its width, centred under the image
// when there is one.
struct LabelPlace {
    i32 x = 0;
    i32 y = 0;       // the baseline
    i32 top = 0;
    i32 bottom = 0;  // one past the label's last row
    i32 span = 0;
};

LabelPlace label_place(const IconArt& art, const Workbench::IconEntry& icon,
                       const Graphics::FontMetrics& metrics, i32 span) {
    LabelPlace place;
    place.span = span;
    place.x = icon.left;
    place.top = icon.top + icon.height + kLabelGap;
    if (art.render != 0 && art.width > 0) {
        place.x = icon.left + art.left + (art.width - place.span) / 2;
        place.top = icon.top + art.top + art.height + kLabelGap;
    }
    place.y = place.top + metrics.baseline;
    place.bottom = place.top + metrics.y_size;
    return place;
}

// One axis of a window's view, inner wide over icons from min to max
// (min is never past the origin): it scrolls from min to the furthest
// point that still shows max, and never less far than the origin, so the
// view a window opens with is always one it can keep.
i32 scroll_limit(i32 max, i32 inner) { return max - inner > 0 ? max - inner : 0; }

u32 slider_body(i32 min, i32 max, i32 inner) {
    const i32 travel = scroll_limit(max, inner) - min;
    if (inner <= 0 || travel <= 0) {
        return ndk::MAXBODY;
    }
    const u64 total = static_cast<u64>(travel) + static_cast<u64>(inner);
    return static_cast<u32>(static_cast<u64>(inner) * ndk::MAXBODY / total);
}

u32 slider_pot(i32 min, i32 max, i32 inner, i32 scroll) {
    const i32 travel = scroll_limit(max, inner) - min;
    if (travel <= 0) {
        return 0;
    }
    const u64 offset = static_cast<u64>(scroll - min);
    return static_cast<u32>((offset * ndk::MAXPOT + static_cast<u64>(travel) / 2) /
                            static_cast<u64>(travel));
}

i32 scroll_of_pot(i32 min, i32 max, i32 inner, u32 pot) {
    const i32 travel = scroll_limit(max, inner) - min;
    if (travel <= 0) {
        return min;
    }
    return min + static_cast<i32>((static_cast<u64>(pot) * static_cast<u64>(travel) +
                                   ndk::MAXPOT / 2) /
                                  ndk::MAXPOT);
}

i32 clamp_scroll(i32 scroll, i32 min, i32 max, i32 inner) {
    const i32 limit = scroll_limit(max, inner);
    return scroll < min ? min : (scroll > limit ? limit : scroll);
}

// The window's clip Region in its layer while the Workbench draws, and
// the layer's own region back after: Intuition draws the frame through the
// same layer.
class InnerClip {
public:
    InnerClip(Machine& m, u32 rp, u32 region) : machine_(m) {
        if (rp == 0 || region == 0) {
            return;
        }
        Bus b(machine_);
        layer_ = b.r32(rp + ndk::rp_Layer);
        if (!b.ok() || layer_ == 0) {
            layer_ = 0;
            return;
        }
        old_ = b.r32(layer_ + ndk::ly_ClipRegion);
        b.w32(layer_ + ndk::ly_ClipRegion, region);
    }
    ~InnerClip() {
        if (layer_ != 0) {
            Bus b(machine_);
            b.w32(layer_ + ndk::ly_ClipRegion, old_);
        }
    }
    InnerClip(const InnerClip&) = delete;
    InnerClip& operator=(const InnerClip&) = delete;

private:
    Machine& machine_;
    u32 layer_ = 0;
    u32 old_ = 0;
};

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
            const MenuItemSpec& spec = kMenus[m].items[i];
            label_bytes += text_length(spec.label) + 1;
            entries += spec.sub_count;
            for (u32 k = 0; k < spec.sub_count; ++k) {
                label_bytes += text_length(spec.subs[k].label) + 1;
            }
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
    auto put_entry = [&](u8 type, const char* label, u16 flags, u32 exclude) {
        b.w8(at + nm_Type, type);
        b.w32(at + nm_Label, label != nullptr ? put_label(label) : 0);
        b.w32(at + nm_CommKey, 0);
        b.w16(at + nm_Flags, flags);
        b.w32(at + nm_MutualExclude, exclude);
        b.w32(at + nm_UserData, 0);
        at += NewMenu_SIZEOF;
    };
    for (u32 m = 0; m < kMenuCount; ++m) {
        put_entry(NM_TITLE, kMenus[m].title, 0, 0);
        for (u32 i = 0; i < kMenus[m].count; ++i) {
            const MenuItemSpec& spec = kMenus[m].items[i];
            u16 flags = spec.enabled ? 0 : kDisabled;
            if (spec.toggle) {
                flags |= CHECKIT | MENUTOGGLE | (backdrop_ ? CHECKED : 0);
            }
            put_entry(NM_ITEM, spec.label, flags, 0);
            for (u32 k = 0; k < spec.sub_count; ++k) {
                const MenuItemSpec& sub = spec.subs[k];
                u16 sub_flags = sub.enabled ? 0 : kDisabled;
                u32 exclude = 0;
                if (sub.check) {
                    sub_flags |= CHECKIT | (sub.checked ? CHECKED : 0);
                    for (u32 n = 0; n < spec.sub_count; ++n) {
                        if (n != k && spec.subs[n].check) {
                            exclude |= 1u << n;
                        }
                    }
                }
                put_entry(NM_SUB, sub.label, sub_flags, exclude);
            }
        }
    }
    put_entry(NM_END, nullptr, 0, 0);
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

namespace {

Rect inner_area(const Workbench::DrawerWindow& w) {
    return Rect{w.inner_left, w.inner_top, w.inner_left + w.inner_width - 1,
                w.inner_top + w.inner_height - 1};
}

void cover(Rect& area, i32 x0, i32 y0, i32 x1, i32 y1) {
    area.x0 = std::min(area.x0, x0);
    area.y0 = std::min(area.y0, y0);
    area.x1 = std::max(area.x1, x1);
    area.y1 = std::max(area.y1, y1);
}

}  // namespace

void Workbench::measure_window(DrawerWindow& w) {
    Bus b(*machine_);
    w.inner_left = b.r8(w.window + ndk::wd_BorderLeft);
    w.inner_top = b.r8(w.window + ndk::wd_BorderTop);
    const i32 width = static_cast<i16>(b.r16(w.window + ndk::wd_Width));
    const i32 height = static_cast<i16>(b.r16(w.window + ndk::wd_Height));
    const i32 right = b.r8(w.window + ndk::wd_BorderRight);
    const i32 bottom = b.r8(w.window + ndk::wd_BorderBottom);
    w.inner_width = width - w.inner_left - right;
    w.inner_height = height - w.inner_top - bottom;
    set_clip(w, inner_area(w));
}

void Workbench::set_clip(const DrawerWindow& w, const Rect& area) {
    if (w.clip == 0) {
        return;
    }
    Bus b(*machine_);
    // The Region's box and its one rectangle, which is relative to the
    // box; an empty area has no rectangle and clips everything.
    const u32 rect = w.clip + ndk::Region_SIZEOF;
    const bool empty = area.x1 < area.x0 || area.y1 < area.y0;
    b.w16(w.clip + ndk::rg_bounds, static_cast<u32>(area.x0));
    b.w16(w.clip + ndk::rg_bounds + 2, static_cast<u32>(area.y0));
    b.w16(w.clip + ndk::rg_bounds + 4, static_cast<u32>(area.x1));
    b.w16(w.clip + ndk::rg_bounds + 6, static_cast<u32>(area.y1));
    b.w32(w.clip + ndk::rg_RegionRectangle, empty ? 0 : rect);
    b.w32(rect + ndk::rr_Next, 0);
    b.w32(rect + ndk::rr_Prev, 0);
    b.w16(rect + ndk::rr_bounds, 0);
    b.w16(rect + ndk::rr_bounds + 2, 0);
    b.w16(rect + ndk::rr_bounds + 4, static_cast<u32>(area.x1 - area.x0));
    b.w16(rect + ndk::rr_bounds + 6, static_cast<u32>(area.y1 - area.y0));
}

// Gives back the window record, its clip Region, its scroll gadgets and
// its fuel gauge; the window itself is the caller's to close first.
void Workbench::release_window(DrawerWindow& w) {
    if (w.clip != 0) {
        exec_->free(w.clip, kClipBytes);
    }
    if (w.gauge_image != 0) {
        exec_->free(w.gauge_image, w.gauge_bytes);
    }
    if (w.gauge != 0) {
        exec_->free(w.gauge, ndk::Gadget_SIZEOF);
    }
    if (w.gadgets != 0) {
        exec_->free(w.gadgets, kScrollBlockBytes);
    }
    windows_.remove(&w);
}

// The tags that give the NewWindow's window a zoom gadget, in a block of
// kZoomTagBytes the caller frees, or 0 when there is no memory for them. The
// window zooms to where it is, as wide as the title and the bar buttons need
// and as high as the title bar and the bottom border. Intuition holds that box
// to the drawer's smallest size, so no drawer zooms to less than that.
u32 Workbench::make_zoom_tags(u32 new_window, const char* title) {
    const u32 block = exec_->alloc(kZoomTagBytes, ndk::MEMF_PUBLIC);
    if (block == 0) {
        return 0;
    }
    Bus b(*machine_);
    // The bar buttons are as large as the screen's font makes its bar.
    chrome::Metrics metrics;
    metrics.font_height =
        static_cast<i32>(b.r8(screen_ + ndk::sc_BarHeight)) - static_cast<i32>(chrome::kWBorTop);
    const u32 flags = b.r32(new_window + ndk::nw_Flags) | ndk::WFLG_HASZOOM;
    const i32 text_width = static_cast<i32>(graphics_->text_width(
        intuition_->font_of_screen(screen_), reinterpret_cast<const u8*>(title),
        text_length(title)));
    const u32 box = block + 2 * ndk::ti_SIZEOF;
    b.w32(block + ndk::ti_Tag, ndk::WA_Zoom);
    b.w32(block + ndk::ti_Data, box);
    b.w32(block + ndk::ti_SIZEOF + ndk::ti_Tag, ndk::TAG_DONE);
    b.w32(block + ndk::ti_SIZEOF + ndk::ti_Data, 0);
    b.w16(box, 0xFFFF);
    b.w16(box + 2, 0xFFFF);
    b.w16(box + 4, static_cast<u32>(chrome::title_bar_width(text_width, flags, metrics)));
    b.w16(box + 6, static_cast<u32>(metrics.border_top()) + chrome::kSizeBorderBottom);
    return block;
}

// The Workbench's windows share one message port, as the Amiga Workbench's
// do, so a desktop of many windows takes one signal bit: a window opens
// without IDCMP, adopts the port and then asks for its messages. Closing it
// takes its queued messages off the port again and leaves the port alone. A
// window opened with a zoom_title gets a zoom gadget whose other box is made
// for that title.
u32 Workbench::open_window_on_port(u32 new_window, u32 idcmp, const char* zoom_title) {
    const u32 tags = zoom_title != nullptr ? make_zoom_tags(new_window, zoom_title) : 0;
    const u32 window = intuition_->open_window(new_window, tags);
    if (tags != 0) {
        exec_->free(tags, kZoomTagBytes);
    }
    if (window == 0) {
        return 0;
    }
    Bus(*machine_).w32(window + ndk::wd_UserPort, window_port_);
    // The mouse wheel scrolls a drawer: it comes as RAWKEY, without the
    // keys, which still go to the shell.
    intuition_->take_wheel(window);
    if (!intuition_->modify_idcmp(window, idcmp)) {
        intuition_->close_window(window);
        return 0;
    }
    return window;
}

// The caller has made sure there is room for it: windows_ is not full.
Workbench::DrawerWindow& Workbench::attach_window(u32 id, u32 window, const char* name,
                                                  const char* path, u32 gadgets) {
    DrawerWindow& w = *windows_.add();
    w.id = id;
    w.window = window;
    w.gadgets = gadgets;
    Bus b(*machine_);
    w.rp = b.r32(window + ndk::wd_RPort);
    w.clip = exec_->alloc(kClipBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    measure_window(w);
    copy_text(w.name, sizeof w.name, name);
    copy_text(w.path, sizeof w.path, path);
    if (menu_ != 0) {
        // The screen may have taken another font since the strip was made.
        gadtools_->layout_menus(menu_);
        intuition_->set_menu_strip(window, menu_);
    }
    return w;
}

// The root window as backdrop_ asks for it: a borderless backdrop under
// the screen's bar, or an ordinary window of the same size that can be
// dragged, sized and put behind the drawers, but not closed.
void Workbench::open_root() {
    if (windows_.full()) {
        LogLine().text("workbench: no window left for the backdrop").send();
        return;
    }
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
    const u32 gadgets = backdrop_ ? 0 : make_scroll_gadgets();
    u32 idcmp;
    if (backdrop_) {
        idcmp = ndk::IDCMP_MOUSEBUTTONS | ndk::IDCMP_MOUSEMOVE | kIdcmpMenuPick |
                kIdcmpRefreshWindow | ndk::IDCMP_ACTIVEWINDOW;
        b.w32(new_window + ndk::nw_Flags, ndk::WFLG_BACKDROP | ndk::WFLG_BORDERLESS |
                                               ndk::WFLG_WBENCHWINDOW);
    } else {
        b.w8(new_window + ndk::nw_DetailPen, 0);
        b.w8(new_window + ndk::nw_BlockPen, 1);
        idcmp = ndk::IDCMP_MOUSEBUTTONS | ndk::IDCMP_MOUSEMOVE | ndk::IDCMP_NEWSIZE |
                ndk::IDCMP_GADGETDOWN | ndk::IDCMP_GADGETUP | kIdcmpMenuPick |
                kIdcmpRefreshWindow | ndk::IDCMP_ACTIVEWINDOW;
        b.w32(new_window + ndk::nw_FirstGadget, gadgets);
        b.w32(new_window + ndk::nw_Flags,
              ndk::WFLG_DRAGBAR | ndk::WFLG_DEPTHGADGET | ndk::WFLG_SIZEGADGET |
                  ndk::WFLG_SIZEBRIGHT | ndk::WFLG_SIZEBBOTTOM | ndk::WFLG_ACTIVATE |
                  ndk::WFLG_WBENCHWINDOW);
        b.w16(new_window + ndk::nw_MinWidth, kMinDrawerWidth);
        b.w16(new_window + ndk::nw_MinHeight, kMinDrawerHeight);
        b.w16(new_window + ndk::nw_MaxWidth, 0xFFFF);
        b.w16(new_window + ndk::nw_MaxHeight, 0xFFFF);
    }
    b.w32(new_window + ndk::nw_Title, title);
    b.w32(new_window + ndk::nw_Screen, screen_);
    b.w16(new_window + ndk::nw_Type, ndk::CUSTOMSCREEN);
    const u32 window =
        b.ok() ? open_window_on_port(new_window, idcmp, backdrop_ ? nullptr : kBackdropName) : 0;
    exec_->free(new_window, ndk::NewWindow_SIZEOF);
    exec_->free(title, sizeof kBackdropName);
    if (window == 0) {
        if (gadgets != 0) {
            exec_->free(gadgets, kScrollBlockBytes);
        }
        LogLine().text("workbench: could not open the root window").send();
        return;
    }
    attach_window(kRootWindow, window, kBackdropName, "", gadgets);
}

// The right-hand column of the root window, top to bottom, then the next
// column to the left once one is full.
void Workbench::place_root_icon(const DrawerWindow& root, IconEntry& icon) {
    const Graphics::FontMetrics metrics = label_metrics();
    const i32 label_width = static_cast<i32>(kDiskLabelChars) * metrics.x_size;
    // The column's icons are centred on one line, far enough in for a
    // label of kDiskLabelChars characters to stay on the screen.
    const i32 centre = root.inner_width - kDiskMargin - label_width / 2;
    i32 left = centre - icon.width / 2;
    i32 top = volume_top_ == 0 ? kVolumeTop : volume_top_;
    if (top + icon.height + metrics.y_size > root.inner_height) {
        left -= label_width + kIconGapX;
        top = kVolumeTop;
    }
    icon.left = left < 0 ? 0 : left;
    icon.top = top;
    volume_top_ = top + icon.height + metrics.y_size + kIconGapY;
}

// Icons with a place of their own keep it, moved inside the window when
// it no longer reaches there; the others go down the right-hand column.
void Workbench::place_root_icons(const DrawerWindow& root) {
    volume_top_ = 0;
    const Graphics::FontMetrics metrics = label_metrics();
    for (IconEntry& icon : icons_) {
        if (icon.owner != kRootWindow) {
            continue;
        }
        if (!icon.placed) {
            place_root_icon(root, icon);
            continue;
        }
        const i32 max_left = root.inner_width - icon.width;
        const i32 max_top = root.inner_height - icon.height - metrics.y_size;
        icon.left = icon.left > max_left ? max_left : icon.left;
        icon.top = icon.top > max_top ? max_top : icon.top;
        icon.left = icon.left < 0 ? 0 : icon.left;
        icon.top = icon.top < 0 ? 0 : icon.top;
    }
}

// The place the icon's DiskObject keeps, if it keeps one.
void Workbench::read_position(IconEntry& icon) {
    icon.placed = false;
    if (icon.object == 0) {
        return;
    }
    Bus b(*machine_);
    const u32 x = b.r32(icon.object + do_CurrentX);
    const u32 y = b.r32(icon.object + do_CurrentY);
    if (b.ok() && x != kNoIconPosition && y != kNoIconPosition) {
        icon.left = static_cast<i32>(x);
        icon.top = static_cast<i32>(y);
        icon.placed = true;
    }
}

u32 Workbench::backdrop_item() const {
    if (menu_ == 0) {
        return 0;
    }
    Bus b(*machine_);
    u32 item = b.r32(menu_ + mu_FirstItem);  // the Workbench menu comes first
    for (u32 i = 0; i < kItemBackdrop && item != 0; ++i) {
        item = b.r32(item + mi_NextItem);
    }
    return b.ok() ? item : 0;
}

// The Window menu's Show submenu item at index: 0 Only Icons, 1 All Files.
u32 Workbench::show_item(u32 index) const {
    if (menu_ == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 menu = b.r32(menu_ + mu_NextMenu);  // the Window menu
    if (menu == 0) {
        return 0;
    }
    u32 item = b.r32(menu + mu_FirstItem);
    for (u32 i = 0; i < kItemWindowShow && item != 0; ++i) {
        item = b.r32(item + mi_NextItem);
    }
    if (item == 0) {
        return 0;
    }
    item = b.r32(item + mi_SubItem);
    for (u32 i = 0; i < index && item != 0; ++i) {
        item = b.r32(item + mi_NextItem);
    }
    return b.ok() ? item : 0;
}

// Sets the Show check marks to the setting, touching no other flag, as
// set_backdrop does with its own.
void Workbench::set_show_marks(bool all) {
    Bus b(*machine_);
    for (u32 i = 0; i < kShowItemCount; ++i) {
        const u32 item = show_item(i);
        if (item == 0) {
            return;
        }
        const u16 flags = static_cast<u16>(b.r16(item + mi_Flags));
        const bool checked = (i == kSubAllFiles) == all;
        b.w16(item + mi_Flags, checked ? flags | CHECKED : flags & ~CHECKED);
    }
}

void Workbench::pick_show(u32 window_id, bool all) {
    DrawerWindow* window = find_window(window_id);
    if (window == nullptr) {
        return;
    }
    // The root window shows only icons, whatever the menu says.
    if (window->id != kRootWindow && window->show_all != all) {
        window->show_all = all;
        update_window(*window);
    }
    set_show_marks(window->id != kRootWindow && window->show_all);
}

void Workbench::set_backdrop(bool on) {
    DrawerWindow* old_root = find_window(kRootWindow);
    if (!desktop_open_ || old_root == nullptr || on == backdrop_) {
        return;
    }
    cancel_drag();
    // The icons stay, so the selection does.
    const Clicks selection = old_root->clicks;
    intuition_->clear_menu_strip(old_root->window);
    intuition_->close_window(old_root->window);
    release_window(*old_root);
    backdrop_ = on;
    open_root();
    DrawerWindow* root = find_window(kRootWindow);
    if (root == nullptr) {
        // The old kind of window comes back rather than none at all.
        backdrop_ = !on;
        open_root();
        root = find_window(kRootWindow);
        if (root == nullptr) {
            return;
        }
    }
    root->clicks = selection;
    place_root_icons(*root);
    draw_window(kRootWindow);
    refresh_title();
    const u32 item = backdrop_item();
    if (item != 0) {
        Bus b(*machine_);
        const u16 flags = static_cast<u16>(b.r16(item + mi_Flags));
        b.w16(item + mi_Flags, backdrop_ ? flags | CHECKED : flags & ~CHECKED);
    }
    // A root window that took the activation shows only icons.
    if (intuition_->active_window() == root->window) {
        set_show_marks(false);
    }
    LogLine().text("workbench: backdrop ").text(backdrop_ ? "on" : "off").send();
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

Workbench::DrawerWindow* Workbench::find_window(u32 id) {
    return windows_.find_if([&](const DrawerWindow& w) { return w.id == id; });
}

const Workbench::DrawerWindow* Workbench::find_window(u32 id) const {
    return windows_.find_if([&](const DrawerWindow& w) { return w.id == id; });
}

Workbench::IconEntry* Workbench::find_icon(u32 id) {
    return icons_.find_if([&](const IconEntry& icon) { return icon.id == id; });
}

const Workbench::IconEntry* Workbench::find_icon(u32 id) const {
    return icons_.find_if([&](const IconEntry& icon) { return icon.id == id; });
}

Workbench::IconEntry* Workbench::take_icon(u32 owner) {
    IconEntry* icon = icons_.add();
    if (icon != nullptr) {
        icon->id = ++last_icon_id_;
        icon->owner = owner;
    }
    return icon;
}

void Workbench::free_icons(u32 owner) {
    icons_.remove_if([&](const IconEntry& icon) {
        if (icon.owner != owner) {
            return false;
        }
        if (icon.owned && icon.object != 0) {
            icon_->free_object(icon.object);
        }
        std::erase_if(left_out_, [&](const LeftOut& out) { return out.icon == icon.id; });
        return true;
    });
}

void Workbench::add_volume(const char* label, const char* icon_path, u8 drive) {
    const DrawerWindow* root = find_window(kRootWindow);
    if (root == nullptr) {
        return;
    }
    IconEntry* icon = take_icon(kRootWindow);
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
        icon->object = drive == kNoDrive ? icon_->ram_disk_object() : icon_->default_object(kDisk);
        icon->owned = icon->object != 0;
    }
    copy_text(icon->name, sizeof icon->name, label);
    copy_text(icon->file, sizeof icon->file, "Disk");
    icon->drive = drive;
    measure(*icon);
    read_position(*icon);
    if (!icon->placed) {
        place_root_icon(*root, *icon);
    }
    LogLine().text("workbench: volume ").text(label).send();
    char volume[Dos::kMaxPath];
    if (volume_root(*icon, volume)) {
        add_left_outs(volume);
    }
}

// A disk in every drive that holds one, the floppy drives first, then the
// hard disk partitions, then the Ram Disk.
void Workbench::add_volume_icons() {
    for (u32 n = 0; n < dos_->drive_count(); ++n) {
        add_drive_volume(n);
    }
    add_volume(kRamDiskName, "RAM:Disk", kNoDrive);
}

// The icon of the disk in a drive, when the drive holds one.
void Workbench::add_drive_volume(u32 drive) {
    if (!dos_->has_disk(drive)) {
        return;
    }
    IconEntry probe;
    copy_text(probe.name, sizeof probe.name, dos_->volume_name(drive));
    probe.type = kDisk;
    probe.drive = static_cast<u8>(drive);
    char root[Dos::kMaxPath];
    char path[Dos::kMaxPath];
    if (volume_root(probe, root) && join_path(root, "Disk", path)) {
        add_volume(probe.name, path, probe.drive);
    }
}

void Workbench::refresh_volumes() {
    DrawerWindow* root = find_window(kRootWindow);
    if (!desktop_open_ || root == nullptr) {
        return;
    }
    bool changed = false;
    for (;;) {
        IconEntry* stale = icons_.find_if([&](const IconEntry& icon) {
            return icon.owner == kRootWindow && icon.type == kDisk && icon.drive != kNoDrive &&
                   (!dos_->has_disk(icon.drive) ||
                    !same_ci(dos_->volume_name(icon.drive), icon.name));
        });
        if (stale == nullptr) {
            break;
        }
        // The windows of the volume go: whatever path begins with its name,
        // or with the device of its drive.
        char by_name[Dos::kMaxPath];
        char by_device[Dos::kMaxPath];
        copy_text(by_name, sizeof by_name, stale->name);
        const u32 name_length = text_length(by_name);
        by_name[name_length] = ':';
        by_name[name_length + 1] = '\0';
        copy_text(by_device, sizeof by_device, dos_->device_name(stale->drive));
        by_device[text_length(by_device)] = ':';
        by_device[text_length(by_device) + 1] = '\0';
        for (;;) {
            const DrawerWindow* drawer = windows_.find_if([&](const DrawerWindow& w) {
                return w.id != kRootWindow &&
                       (starts_with_ci(w.path, by_name) || starts_with_ci(w.path, by_device));
            });
            if (drawer == nullptr) {
                break;
            }
            close_drawer(drawer->id);
        }
        const u32 gone = stale->id;
        for (;;) {
            const auto out = std::find_if(left_out_.begin(), left_out_.end(), [&](const LeftOut& o) {
                return starts_with_ci(o.drawer, by_name) || starts_with_ci(o.drawer, by_device);
            });
            if (out == left_out_.end()) {
                break;
            }
            remove_root_icon(out->icon);
        }
        remove_root_icon(gone);
        changed = true;
    }
    const usize before = icons_.size();
    for (u32 n = 0; n < dos_->drive_count(); ++n) {
        const bool shown = icons_.find_if([&](const IconEntry& icon) {
                               return icon.owner == kRootWindow && icon.type == kDisk &&
                                      icon.drive == n;
                           }) != nullptr;
        if (!shown) {
            add_drive_volume(n);
        }
    }
    changed = changed || icons_.size() != before;
    if (changed) {
        draw_window(kRootWindow);
    }
}

// The root of the disk an icon stands for: "Name:" by its volume's name,
// or "<device>:" of its drive when another drive holds a disk of the same
// name.
bool Workbench::volume_root(const IconEntry& icon, char (&out)[Dos::kMaxPath]) const {
    bool shared = false;
    if (icon.drive != kNoDrive) {
        for (u32 n = 0; n < dos_->drive_count(); ++n) {
            shared = shared || (n != icon.drive && dos_->has_disk(n) &&
                                same_ci(dos_->volume_name(n), icon.name));
        }
    }
    const char* name = shared ? dos_->device_name(icon.drive) : icon.name;
    const u32 length = text_length(name);
    if (length + 2 > sizeof out) {
        return false;
    }
    copy_text(out, sizeof out, name);
    out[length] = ':';
    out[length + 1] = '\0';
    return true;
}

// --- drawer windows ------------------------------------------------------

void Workbench::add_dir_icon(const DrawerWindow& drawer, const char* entry, bool is_dir) {
    if (is_dir || !ends_with_ci(entry, ".info")) {
        return;
    }
    char stem[32];
    file_stem(entry, stem);
    if (stem[0] == '\0') {
        return;
    }
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
    IconEntry* icon = take_icon(drawer.id);
    if (icon == nullptr) {
        icon_->free_object(object);
        LogLine().text("workbench: icon table full").send();
        return;
    }
    Bus b(*machine_);
    icon->object = object;
    icon->owned = true;
    icon->type = static_cast<u8>(b.r8(object + do_Type));
    read_position(*icon);
    copy_text(icon->name, sizeof icon->name, stem);
    copy_text(icon->file, sizeof icon->file, stem);
    measure(*icon);
}

// The type of the icon a file without an .info gets: a drawer for a
// drawer, a tool for a loadable program, a project for anything else.
u8 Workbench::plain_type(const char* path, bool is_dir) const {
    if (is_dir) {
        return kDrawer;
    }
    u8 magic[4]{};
    if (dos_->read_head(path, magic, sizeof magic) && magic[0] == 0x00 && magic[1] == 0x00 &&
        magic[2] == 0x03 && magic[3] == 0xF3) {
        return kTool;
    }
    return kProject;
}

void Workbench::add_plain_icon(const DrawerWindow& drawer, const char* name, bool is_dir) {
    if (text_length(name) >= sizeof(IconEntry::file)) {
        return;
    }
    char path[Dos::kMaxPath];
    if (!join_path(drawer.path, name, path)) {
        return;
    }
    const u8 type = plain_type(path, is_dir);
    const u32 object = icon_->default_object(type);
    if (object == 0) {
        return;
    }
    IconEntry* icon = take_icon(drawer.id);
    if (icon == nullptr) {
        icon_->free_object(object);
        LogLine().text("workbench: icon table full").send();
        return;
    }
    icon->object = object;
    icon->owned = true;
    icon->plain = true;
    icon->type = type;
    copy_text(icon->name, sizeof icon->name, name);
    copy_text(icon->file, sizeof icon->file, name);
    measure(*icon);
}

void Workbench::fill_drawer(DrawerWindow& drawer) {
    cancel_drag();
    free_icons(drawer.id);
    // The icon the selection named is gone with the others.
    drawer.clicks.clear();
    struct Collect {
        Workbench* self;
        const DrawerWindow* drawer;
    } collect{this, &drawer};
    auto visit = [](void* at, const char* name, bool is_dir) {
        const Collect& c = *static_cast<const Collect*>(at);
        c.self->add_dir_icon(*c.drawer, name, is_dir);
    };
    u32 error = 0;
    dos_->list_dir(drawer.path, visit, &collect, error);
    if (drawer.show_all && drawer.id != kRootWindow) {
        struct Listing {
            char name[32];
            bool is_dir;
        };
        std::vector<Listing> plain;
        struct CollectPlain {
            Workbench* self;
            const DrawerWindow* drawer;
            std::vector<Listing>* out;
        } collect_plain{this, &drawer, &plain};
        auto visit_plain = [](void* at, const char* name, bool is_dir) {
            const CollectPlain& c = *static_cast<const CollectPlain*>(at);
            if (!is_dir && ends_with_ci(name, ".info")) {
                return;
            }
            char path[Dos::kMaxPath];
            char info[Dos::kMaxPath];
            if (!join_path(c.drawer->path, name, path) || !info_path(path, info) ||
                c.self->dos_->examine(info).kind != Dos::TargetKind::Missing) {
                return;
            }
            Listing entry{};
            entry.is_dir = is_dir;
            if (text_length(name) >= sizeof entry.name) {
                return;
            }
            copy_text(entry.name, sizeof entry.name, name);
            c.out->push_back(entry);
        };
        dos_->list_dir(drawer.path, visit_plain, &collect_plain, error);
        std::sort(plain.begin(), plain.end(),
                  [](const Listing& a, const Listing& b) { return less_ci(a.name, b.name); });
        for (const Listing& entry : plain) {
            add_plain_icon(drawer, entry.name, entry.is_dir);
        }
    }
    place_icons(drawer);
}

void Workbench::place_icons(const DrawerWindow& drawer) {
    const i32 label_height = label_metrics().y_size;
    // The icons go down the columns. All Files, which has many, fills rows
    // across the window instead, so it scrolls downwards, in cells wide
    // enough for the names under them.
    const bool across = drawer.show_all;
    i32 cell_width = 16;
    i32 cell_height = 16;
    for (const IconEntry& icon : icons_) {
        if (icon.owner != drawer.id) {
            continue;
        }
        const i32 span = across ? std::max(icon.width, label_span(icon)) : icon.width;
        cell_width = std::max(span + kCellGapX, cell_width);
        cell_height = icon.height + label_height + kCellGapY > cell_height
                          ? icon.height + label_height + kCellGapY
                          : cell_height;
    }
    i32 run = across ? (drawer.inner_width - kGridMarginX) / cell_width
                     : (drawer.inner_height - kGridMarginY) / cell_height;
    if (run < 1) {
        run = 1;
    }
    // Whether the icon at left, top would cover one with a place of its own,
    // in rows counting the frames both icons stand in.
    const i32 frame_x = across ? 2 * kFrameOutX : 0;
    const i32 frame_y = across ? 2 * kFrameOutY : 0;
    const auto taken = [&](const IconEntry& icon, i32 left, i32 top) {
        for (const IconEntry& other : icons_) {
            if (other.owner == drawer.id && other.placed &&
                left < other.left + other.width + frame_x &&
                other.left < left + icon.width + frame_x &&
                top < other.top + other.height + label_height + frame_y &&
                other.top < top + icon.height + label_height + frame_y) {
                return true;
            }
        }
        return false;
    };
    u32 slot = 0;
    for (IconEntry& icon : icons_) {
        if (icon.owner != drawer.id || icon.placed) {
            continue;
        }
        do {
            const i32 along = static_cast<i32>(slot % static_cast<u32>(run));
            const i32 line = static_cast<i32>(slot / static_cast<u32>(run));
            icon.left = kGridMarginX + (across ? along : line) * cell_width +
                        (across ? (cell_width - kCellGapX - icon.width) / 2 : 0);
            icon.top = kGridMarginY + (across ? line : along) * cell_height;
            ++slot;
        } while (taken(icon, icon.left, icon.top));
    }
}

void Workbench::open_drawer(u32 window_id, const IconEntry& icon) {
    const DrawerWindow* window = find_window(window_id);
    if (window == nullptr) {
        return;
    }
    char path[Dos::kMaxPath];
    if (!icon_target_path(*window, icon, path)) {
        return;
    }
    // The window's box comes from the icon as it is saved now, so a
    // Snapshot since the icon was read counts.
    char info[Dos::kMaxPath];
    const u32 fresh = icon_path(*window, icon, info) ? icon_->load_object(info) : 0;
    open_drawer_window(path, icon.name, fresh != 0 ? fresh : icon.object, nullptr,
                       window->show_all);
    if (fresh != 0) {
        icon_->free_object(fresh);
    }
}

// The drawer a disk or drawer icon stands for: "Name:" for a volume on
// the root window, the drawer's path for anything else, a disk-typed icon
// inside a drawer included.
bool Workbench::icon_target_path(const DrawerWindow& window, const IconEntry& icon,
                                 char (&out)[Dos::kMaxPath]) const {
    if (window.id == kRootWindow && icon.type == kDisk) {
        return volume_root(icon, out);
    }
    return join_path(drawer_of(window, icon), icon.file, out);
}

// The path of the icon's .info without the suffix: "Name:Disk" for a disk.
bool Workbench::icon_path(const DrawerWindow& window, const IconEntry& icon,
                          char (&out)[Dos::kMaxPath]) const {
    if (window.id == kRootWindow && icon.type == kDisk) {
        char volume[Dos::kMaxPath];
        return icon_target_path(window, icon, volume) && join_path(volume, "Disk", out);
    }
    return join_path(drawer_of(window, icon), icon.file, out);
}

void Workbench::open_drawer_window(const char* path, const char* name, u32 object,
                                   const DrawerBox* box, bool inherit_all) {
    const DrawerWindow* existing = windows_.find_if([&](const DrawerWindow& w) {
        return w.id != kRootWindow && same_ci(w.path, path);
    });
    if (existing != nullptr) {
        intuition_->window_to_front(existing->window);
        return;
    }
    if (windows_.full()) {
        LogLine().text("workbench: no window left for ").text(name).send();
        return;
    }
    Bus b(*machine_);
    i32 left = -1;
    i32 top = -1;
    i32 width = 0;
    i32 height = 0;
    i32 scroll_x = 0;
    i32 scroll_y = 0;
    bool show_all = inherit_all;
    if (box != nullptr) {
        left = box->left;
        top = box->top;
        width = box->width;
        height = box->height;
    } else {
        const u32 drawer_data = object != 0 ? b.r32(object + do_DrawerData) : 0;
        if (drawer_data != 0) {
            left = static_cast<i16>(b.r16(drawer_data + ndk::nw_LeftEdge));
            top = static_cast<i16>(b.r16(drawer_data + ndk::nw_TopEdge));
            width = static_cast<i16>(b.r16(drawer_data + ndk::nw_Width));
            height = static_cast<i16>(b.r16(drawer_data + ndk::nw_Height));
            scroll_x = static_cast<i32>(b.r32(drawer_data + dd_CurrentX));
            scroll_y = static_cast<i32>(b.r32(drawer_data + dd_CurrentY));
            // Only a revision 1 icon keeps the DrawerData flags.
            const u32 user_data = b.r32(object + do_Gadget + ndk::gg_UserData);
            if ((user_data & 0xFF) == 1) {
                const u32 flags = b.r32(drawer_data + dd_Flags);
                if (flags == DDFLAGS_SHOWALL) {
                    show_all = true;
                } else if (flags == DDFLAGS_SHOWICONS) {
                    show_all = false;
                }
            }
        }
    }
    if (width <= 0) {
        width = kDefaultDrawerWidth;
    }
    if (height <= 0) {
        height = kDefaultDrawerHeight;
    }
    // A box at the screen's corner is the place of an icon that was never
    // snapshot, as a default drawer icon's is: such a window cascades as
    // one without a box does.
    if (box == nullptr && left == 0 && top == 0) {
        left = -1;
        top = -1;
    }
    // Each drawer without a box of its own opens a step below and to the
    // right of the one before, and the steps start over after kCascadeSlots.
    const i32 cascade = static_cast<i32>((windows_.size() - 1) % kCascadeSlots);
    if (left < 0) {
        left = kCascadeX + kCascadeStep * cascade;
    }
    if (top < 0) {
        top = kCascadeY + kCascadeStep * cascade;
    }
    // No drawer opens over the screen's title bar.
    const i32 bar_height = static_cast<i32>(b.r8(screen_ + ndk::sc_BarHeight));
    if (top < bar_height) {
        top = bar_height;
    }
    const u32 new_window =
        exec_->alloc(ndk::NewWindow_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    const u32 title_bytes = text_length(name) + 1;
    const u32 title = exec_->alloc(title_bytes, ndk::MEMF_PUBLIC);
    const u32 gadgets = make_scroll_gadgets();
    if (new_window == 0 || title == 0 || gadgets == 0) {
        if (new_window != 0) {
            exec_->free(new_window, ndk::NewWindow_SIZEOF);
        }
        if (title != 0) {
            exec_->free(title, title_bytes);
        }
        if (gadgets != 0) {
            exec_->free(gadgets, kScrollBlockBytes);
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
    b.w32(new_window + ndk::nw_FirstGadget, gadgets);
    b.w32(new_window + ndk::nw_Flags,
          ndk::WFLG_DRAGBAR | ndk::WFLG_DEPTHGADGET | ndk::WFLG_CLOSEGADGET |
              ndk::WFLG_SIZEGADGET | ndk::WFLG_SIZEBRIGHT | ndk::WFLG_SIZEBBOTTOM |
              ndk::WFLG_ACTIVATE | ndk::WFLG_WBENCHWINDOW);
    b.w32(new_window + ndk::nw_Title, title);
    b.w32(new_window + ndk::nw_Screen, screen_);
    b.w16(new_window + ndk::nw_Type, ndk::CUSTOMSCREEN);
    // The size gadget takes the drawer from a few icons' room up to the
    // whole screen.
    b.w16(new_window + ndk::nw_MinWidth, kMinDrawerWidth);
    b.w16(new_window + ndk::nw_MinHeight, kMinDrawerHeight);
    b.w16(new_window + ndk::nw_MaxWidth, 0xFFFF);
    b.w16(new_window + ndk::nw_MaxHeight, 0xFFFF);
    // A volume window shows how full its disk is; the gauge joins the
    // gadget list before the window opens, for the border it needs.
    u32 gauge = 0;
    if (is_volume_root(path)) {
        const i32 gauge_top = bar_height + 1 + kGaugeInset;  // below the bar and its line
        gauge = make_volume_gauge(
            gadgets, gauge_top, height - gauge_top - static_cast<i32>(chrome::kSizeBorderBottom));
    }
    // The gauge's face is the border's, which follows the window's state.
    const u32 idcmp = ndk::IDCMP_CLOSEWINDOW | ndk::IDCMP_MOUSEBUTTONS | ndk::IDCMP_MOUSEMOVE |
                      ndk::IDCMP_NEWSIZE | ndk::IDCMP_GADGETDOWN | ndk::IDCMP_GADGETUP |
                      kIdcmpMenuPick | kIdcmpRefreshWindow | ndk::IDCMP_ACTIVEWINDOW |
                      (gauge != 0 ? ndk::IDCMP_INACTIVEWINDOW : 0u);
    // The window keeps a copy of its title, so the one built here goes
    // back with the NewWindow.
    const u32 window = b.ok() ? open_window_on_port(new_window, idcmp, name) : 0;
    exec_->free(new_window, ndk::NewWindow_SIZEOF);
    exec_->free(title, title_bytes);
    if (window == 0) {
        if (gauge != 0) {
            exec_->free(gauge, ndk::Gadget_SIZEOF);
        }
        exec_->free(gadgets, kScrollBlockBytes);
        LogLine().text("workbench: could not open ").text(name).send();
        return;
    }
    DrawerWindow& drawer = attach_window(++open_count_, window, name, path, gadgets);
    drawer.gauge = gauge;
    if (gauge != 0) {
        intuition_->set_size_hook(window, &Workbench::gauge_sized, this);
    }
    // The view a snapshot kept; drawing keeps it inside the icons.
    drawer.scroll_x = scroll_x;
    drawer.scroll_y = scroll_y;
    drawer.show_all = show_all;
    fill_drawer(drawer);
    layout_gauge(drawer);
    draw_window(drawer.id);
    // The drawer opened as the active window.
    set_show_marks(drawer.show_all);
    LogLine().text("workbench: opened ").text(name).send();
}

void Workbench::close_drawer(u32 window_id) {
    DrawerWindow* drawer = find_window(window_id);
    if (window_id == kRootWindow || drawer == nullptr) {
        return;
    }
    cancel_drag();
    free_icons(window_id);
    intuition_->close_window(drawer->window);
    release_window(*drawer);
    draw_window(kRootWindow);
}

// --- drawing -------------------------------------------------------------

Graphics::FontMetrics Workbench::label_metrics() const {
    return graphics_->font_metrics(icon_font());
}

i32 Workbench::label_span(const IconEntry& icon) const {
    return static_cast<i32>(
        graphics_->text_width(icon_font(), reinterpret_cast<const u8*>(icon.name),
                              text_length(icon.name)));
}

// The pens and draw mode the icon font's struct names, or the label's own
// for a Workbench without one.
void Workbench::set_label_pens(u32 rp) {
    u32 front = 1;
    u32 back = 0;
    u32 mode = ndk::JAM1;
    if (icon_font_ != 0) {
        Bus b(*machine_);
        mode = b.r16(icon_font_ + fc_DrawMode);
        front = b.r8(icon_font_ + fc_FrontPen);
        back = b.r8(icon_font_ + fc_BackPen);
    }
    graphics_->set_pens(rp, front, back, mode);
}

void Workbench::draw_icon(const DrawerWindow& drawer, const IconEntry& icon, bool selected) {
    if (icon.object == 0) {
        return;
    }
    Bus b(*machine_);
    const IconArt art = art_of(b, icon.object);
    const i32 origin_x = drawer.inner_left - drawer.scroll_x;
    const i32 origin_y = drawer.inner_top - drawer.scroll_y;
    const i32 left = origin_x + icon.left;
    const i32 top = origin_y + icon.top;
    if (art.render != 0) {
        // The frame: light along the top and left and dark along the bottom
        // and right, the other way round while the icon is selected.
        const i32 x0 = left + art.left - kFrameOutX;
        const i32 y0 = top + art.top - kFrameOutY;
        const i32 x1 = left + art.left + art.width - 1 + kFrameOutX;
        const i32 y1 = top + art.top + art.height - 1 + kFrameOutY;
        const u32 light = selected ? kFrameShadowPen : kFrameShinePen;
        const u32 dark = selected ? kFrameShinePen : kFrameShadowPen;
        graphics_->set_pens(drawer.rp, light, 0, ndk::JAM1);
        graphics_->rect_fill(drawer.rp, x0, y0, x1 - 1, y0);
        graphics_->rect_fill(drawer.rp, x0, y0, x0, y1 - 1);
        graphics_->set_pens(drawer.rp, dark, 0, ndk::JAM1);
        graphics_->rect_fill(drawer.rp, x0 + 1, y1, x1, y1);
        graphics_->rect_fill(drawer.rp, x1, y0 + 1, x1, y1);
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
    const LabelPlace label = label_place(art, icon, label_metrics(), label_span(icon));
    // A label past the inner edges is cut off there, as the image is.
    set_label_pens(drawer.rp);
    graphics_->move(drawer.rp, origin_x + label.x, origin_y + label.y);
    graphics_->text_in(drawer.rp, icon_font(), reinterpret_cast<const u8*>(icon.name),
                       text_length(icon.name));
}

// The window's view kept inside its icons and its sliders set to match,
// then its icons drawn.
void Workbench::draw_window(u32 window_id) {
    DrawerWindow* w = find_window(window_id);
    if (w == nullptr) {
        return;
    }
    update_scroll(*w);
    paint_window(*w);
}

void Workbench::paint_window(const DrawerWindow& drawer) {
    paint_area(drawer, inner_area(drawer));
}

void Workbench::paint_area(const DrawerWindow& drawer, const Rect& area) {
    const Rect inner = inner_area(drawer);
    const Rect part{std::max(area.x0, inner.x0), std::max(area.y0, inner.y0),
                    std::min(area.x1, inner.x1), std::min(area.y1, inner.y1)};
    if (part.x1 < part.x0 || part.y1 < part.y0) {
        return;
    }
    const u32 selected = drawer.clicks.selected();
    set_clip(drawer, part);
    {
        const InnerClip clip(*machine_, drawer.rp, drawer.clip);
        graphics_->set_pens(drawer.rp, 0, 0, ndk::JAM1);
        graphics_->rect_fill(drawer.rp, part.x0, part.y0, part.x1, part.y1);
        fill_pattern(drawer, drawer.id == kRootWindow ? 0 : 1, part);
        for (const IconEntry& icon : icons_) {
            if (icon.owner != drawer.id) {
                continue;
            }
            const Rect box = icon_area(drawer, icon);
            if (box.x0 <= part.x1 && box.x1 >= part.x0 && box.y0 <= part.y1 &&
                box.y1 >= part.y0) {
                draw_icon(drawer, icon, selected == icon.id);
            }
        }
    }
    set_clip(drawer, inner);
}

Rect Workbench::icon_area(const DrawerWindow& drawer, const IconEntry& icon) const {
    Bus b(*machine_);
    const IconArt art = art_of(b, icon.object);
    const LabelPlace label = label_place(art, icon, label_metrics(), label_span(icon));
    Rect area{icon.left, icon.top, icon.left + icon.width - 1, icon.top + icon.height - 1};
    cover(area, label.x, label.top, label.x + label.span - 1, label.bottom - 1);
    if (art.render != 0) {
        cover(area, icon.left + art.left - kFrameOutX, icon.top + art.top - kFrameOutY,
              icon.left + art.left + art.width - 1 + kFrameOutX,
              icon.top + art.top + art.height - 1 + kFrameOutY);
    }
    if (art.select != 0) {
        const i32 left = icon.left + static_cast<i16>(b.r16(art.select + im_LeftEdge));
        const i32 top = icon.top + static_cast<i16>(b.r16(art.select + im_TopEdge));
        cover(area, left, top, left + static_cast<i16>(b.r16(art.select + im_Width)) - 1,
              top + static_cast<i16>(b.r16(art.select + im_Height)) - 1);
    }
    const i32 dx = drawer.inner_left - drawer.scroll_x;
    const i32 dy = drawer.inner_top - drawer.scroll_y;
    return Rect{area.x0 + dx, area.y0 + dy, area.x1 + dx, area.y1 + dy};
}

void Workbench::repaint_selection(u32 window_id, u32 before, u32 after) {
    const DrawerWindow* drawer = find_window(window_id);
    if (drawer == nullptr) {
        return;
    }
    const u32 changed[] = {before, after == before ? kNoIcon : after};
    for (const u32 id : changed) {
        const IconEntry* icon = find_icon(id);
        if (icon != nullptr && icon->owner == window_id) {
            paint_area(*drawer, icon_area(*drawer, *icon));
        }
    }
}

Workbench::Extent Workbench::extent(u32 window_id) const {
    Extent e;
    Bus b(*machine_);
    const Graphics::FontMetrics metrics = label_metrics();
    for (const IconEntry& icon : icons_) {
        if (icon.owner != window_id) {
            continue;
        }
        const LabelPlace label =
            label_place(art_of(b, icon.object), icon, metrics, label_span(icon));
        const i32 x0 = label.x < icon.left ? label.x : icon.left;
        const i32 x1 = label.x + label.span > icon.left + icon.width ? label.x + label.span
                                                                     : icon.left + icon.width;
        const i32 y0 = icon.top;
        const i32 y1 = label.bottom > icon.top + icon.height ? label.bottom
                                                             : icon.top + icon.height;
        e.min_x = x0 < e.min_x ? x0 : e.min_x;
        e.min_y = y0 < e.min_y ? y0 : e.min_y;
        e.max_x = x1 + kGridMarginX > e.max_x ? x1 + kGridMarginX : e.max_x;
        e.max_y = y1 + kGridMarginY > e.max_y ? y1 + kGridMarginY : e.max_y;
    }
    return e;
}

// Keeps the view inside the window's icons and sets the sliders to it; a
// window without scroll gadgets shows its icons from the origin.
void Workbench::update_scroll(DrawerWindow& w) {
    if (w.gadgets == 0) {
        w.scroll_x = 0;
        w.scroll_y = 0;
        return;
    }
    const Extent e = extent(w.id);
    w.scroll_x = clamp_scroll(w.scroll_x, e.min_x, e.max_x, w.inner_width);
    w.scroll_y = clamp_scroll(w.scroll_y, e.min_y, e.max_y, w.inner_height);
    Bus b(*machine_);
    for (u32 i = 0; i < kSliders; ++i) {
        const bool vertical = kScrollSpecs[i].id == kScrollVert;
        const u32 gadget = w.gadgets + i * ndk::Gadget_SIZEOF;
        const u32 pi = w.gadgets + kPropInfoAt + i * ndk::PropInfo_SIZEOF;
        const u32 flags = vertical ? kVertSliderFlags : kHorizSliderFlags;
        u32 hpot = 0;
        u32 vpot = 0;
        u32 hbody = ndk::MAXBODY;
        u32 vbody = ndk::MAXBODY;
        if (vertical) {
            vpot = slider_pot(e.min_y, e.max_y, w.inner_height, w.scroll_y);
            vbody = slider_body(e.min_y, e.max_y, w.inner_height);
        } else {
            hpot = slider_pot(e.min_x, e.max_x, w.inner_width, w.scroll_x);
            hbody = slider_body(e.min_x, e.max_x, w.inner_width);
        }
        // Only a slider that changes is drawn again.
        if (b.r16(pi + ndk::pi_HorizPot) == hpot && b.r16(pi + ndk::pi_VertPot) == vpot &&
            b.r16(pi + ndk::pi_HorizBody) == hbody && b.r16(pi + ndk::pi_VertBody) == vbody) {
            continue;
        }
        intuition_->new_modify_prop(gadget, w.window, flags, hpot, vpot, hbody, vbody, 1);
    }
}

void Workbench::scroll_by(u32 window_id, i32 dx, i32 dy) {
    DrawerWindow* found = find_window(window_id);
    if (found == nullptr) {
        return;
    }
    DrawerWindow& w = *found;
    const i32 old_x = w.scroll_x;
    const i32 old_y = w.scroll_y;
    w.scroll_x += dx;
    w.scroll_y += dy;
    update_scroll(w);
    if (w.scroll_x != old_x || w.scroll_y != old_y) {
        paint_window(w);
    }
}

// The view where the user has put the sliders. Intuition has drawn their
// knobs already, so they are not set again.
void Workbench::follow_sliders(u32 window_id) {
    DrawerWindow* found = find_window(window_id);
    if (found == nullptr || found->gadgets == 0) {
        return;
    }
    DrawerWindow& w = *found;
    Bus b(*machine_);
    const u32 vert = w.gadgets + kPropInfoAt;
    const u32 horiz = vert + ndk::PropInfo_SIZEOF;
    const u32 vpot = b.r16(vert + ndk::pi_VertPot);
    const u32 hpot = b.r16(horiz + ndk::pi_HorizPot);
    if (!b.ok()) {
        return;
    }
    const Extent e = extent(window_id);
    const i32 x = scroll_of_pot(e.min_x, e.max_x, w.inner_width, hpot);
    const i32 y = scroll_of_pot(e.min_y, e.max_y, w.inner_height, vpot);
    if (x != w.scroll_x || y != w.scroll_y) {
        w.scroll_x = x;
        w.scroll_y = y;
        paint_window(w);
    }
}

u16 Workbench::scroll_gadget_id(u32 window_id, u32 address) const {
    const DrawerWindow* w = find_window(window_id);
    const u32 block = w != nullptr ? w->gadgets : 0;
    if (block == 0 || address < block) {
        return 0;
    }
    const u32 offset = address - block;
    if (offset % ndk::Gadget_SIZEOF != 0 || offset / ndk::Gadget_SIZEOF >= kScrollGadgets) {
        return 0;
    }
    return kScrollSpecs[offset / ndk::Gadget_SIZEOF].id;
}

u32 Workbench::scroll_gadget(u32 window_id, u16 id) const {
    const DrawerWindow* w = find_window(window_id);
    if (w == nullptr || w->gadgets == 0) {
        return 0;
    }
    for (u32 i = 0; i < kScrollGadgets; ++i) {
        if (kScrollSpecs[i].id == id) {
            return w->gadgets + i * ndk::Gadget_SIZEOF;
        }
    }
    return 0;
}

// A slider moved, by its knob or a click beside it, or an arrow pressed.
void Workbench::scroll_message(u32 window_id, u32 idcmp_class, u16 id) {
    if (id == kScrollVert || id == kScrollHoriz) {
        follow_sliders(window_id);
    } else if (idcmp_class == ndk::IDCMP_GADGETDOWN) {
        // TODO: repeat while the arrow is held, once Intuition sends
        // IDCMP_INTUITICKS.
        const i32 dx = id == kScrollLeft ? -kScrollStep : (id == kScrollRight ? kScrollStep : 0);
        const i32 dy = id == kScrollUp ? -kScrollStep : (id == kScrollDown ? kScrollStep : 0);
        scroll_by(window_id, dx, dy);
    }
}

// The window's scroll gadgets, linked in kScrollSpecs order, with the
// sliders' PropInfos and knob images after them in the same block. The
// sliders look as thick across as each other on the screen: the
// horizontal one is as many pixels high as the vertical one is wide,
// times the pixels' width over their height, as far as the borders allow.
u32 Workbench::make_scroll_gadgets() {
    if (arrows_ == 0 && !build_arrow_images()) {
        return 0;
    }
    paint_arrow_images();
    const u32 block = exec_->alloc(kScrollBlockBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (block == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 draw_info = intuition_->screen_draw_info(screen_);
    i32 res_x = draw_info != 0 ? static_cast<i32>(b.r16(draw_info + dri_ResolutionX)) : 0;
    i32 res_y = draw_info != 0 ? static_cast<i32>(b.r16(draw_info + dri_ResolutionY)) : 0;
    if (res_x <= 0 || res_y <= 0) {
        res_x = 1;
        res_y = 1;
    }
    i32 vert = kVertSliderRoom;
    i32 horiz = (vert * res_x + res_y / 2) / res_y;
    if (horiz > kHorizSliderRoom) {
        horiz = kHorizSliderRoom;
        vert = (horiz * res_y + res_x / 2) / res_x;
    }
    vert = vert < kMinSlider ? kMinSlider : (vert > kVertSliderRoom ? kVertSliderRoom : vert);
    horiz = horiz < kMinSlider ? kMinSlider : horiz;
    for (u32 i = 0; i < kScrollGadgets; ++i) {
        ScrollGadgetSpec spec = kScrollSpecs[i];
        if (spec.id == kScrollVert) {
            spec.left = static_cast<i16>(spec.left + (kVertSliderRoom - vert) / 2);
            spec.width = static_cast<i16>(vert);
        } else if (spec.id == kScrollHoriz) {
            spec.top = static_cast<i16>(spec.top + (kHorizSliderRoom - horiz) / 2);
            spec.height = static_cast<i16>(horiz);
        }
        const u32 gadget = block + i * ndk::Gadget_SIZEOF;
        const bool last = i + 1 == kScrollGadgets;
        b.w32(gadget + ndk::gg_NextGadget, last ? 0 : gadget + ndk::Gadget_SIZEOF);
        b.w16(gadget + ndk::gg_LeftEdge, static_cast<u16>(spec.left));
        b.w16(gadget + ndk::gg_TopEdge, static_cast<u16>(spec.top));
        b.w16(gadget + ndk::gg_Width, static_cast<u16>(spec.width));
        b.w16(gadget + ndk::gg_Height, static_cast<u16>(spec.height));
        b.w16(gadget + ndk::gg_Flags, spec.flags);
        b.w16(gadget + ndk::gg_Activation, spec.activation);
        b.w16(gadget + ndk::gg_GadgetID, spec.id);
        if (spec.arrow == kNoArrow) {
            const u32 pi = block + kPropInfoAt + i * ndk::PropInfo_SIZEOF;
            const bool vertical = spec.id == kScrollVert;
            b.w16(gadget + ndk::gg_GadgetType, ndk::GTYP_PROPGADGET);
            b.w32(gadget + ndk::gg_GadgetRender, block + kKnobAt + i * Image_SIZEOF);
            b.w32(gadget + ndk::gg_SpecialInfo, pi);
            b.w16(pi + ndk::pi_Flags, vertical ? kVertSliderFlags : kHorizSliderFlags);
            b.w16(pi + ndk::pi_HorizBody, ndk::MAXBODY);
            b.w16(pi + ndk::pi_VertBody, ndk::MAXBODY);
        } else {
            const u32 plain = arrows_ + (spec.arrow * 2u) * Image_SIZEOF;
            b.w16(gadget + ndk::gg_GadgetType, ndk::GTYP_BOOLGADGET);
            b.w32(gadget + ndk::gg_GadgetRender, plain);
            b.w32(gadget + ndk::gg_SelectRender, plain + Image_SIZEOF);
        }
    }
    if (!b.ok()) {
        exec_->free(block, kScrollBlockBytes);
        return 0;
    }
    return block;
}

// The fuel gauge gadget of a volume window, linked after the scroll
// gadgets before the window opens so that Intuition makes the left border
// wide enough for it. Its box across the border is that of the screen's
// look. Its image is made once the window's size is known.
u32 Workbench::make_volume_gauge(u32 gadgets, i32 top, i32 height) {
    const u32 gauge = exec_->alloc(ndk::Gadget_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (gauge == 0) {
        return 0;
    }
    if (height < kGaugeMinHeight) {
        height = 0;
    }
    Bus b(*machine_);
    const chrome::Style style =
        chrome::style_for_depth(b.r8(screen_ + ndk::sc_BitMap + ndk::bm_Depth));
    u32 last = gadgets;
    for (u32 i = 1; i < kScrollGadgets; ++i) {
        last = b.r32(last + ndk::gg_NextGadget);
    }
    b.w32(last + ndk::gg_NextGadget, gauge);
    b.w16(gauge + ndk::gg_LeftEdge, static_cast<u32>(chrome::fuel_gauge_left(style)));
    b.w16(gauge + ndk::gg_TopEdge, static_cast<u32>(top));
    b.w16(gauge + ndk::gg_Width, static_cast<u32>(chrome::fuel_gauge_width(style)));
    b.w16(gauge + ndk::gg_Height, static_cast<u32>(height));
    b.w16(gauge + ndk::gg_Flags, ndk::GFLG_GADGIMAGE | ndk::GFLG_GADGHNONE);
    b.w16(gauge + ndk::gg_Activation, ndk::GACT_LEFTBORDER);
    b.w16(gauge + ndk::gg_GadgetType, ndk::GTYP_BOOLGADGET);
    if (!b.ok()) {
        exec_->free(gauge, ndk::Gadget_SIZEOF);
        return 0;
    }
    return gauge;
}

namespace {

// Where the gauge of a volume window sits: from two pixels below the
// window's inner top to its inner bottom. A window with fewer rows than
// kGaugeMinHeight there has no room for a gauge, which has no rows then.
struct GaugeBox {
    i32 top;
    i32 height;
};

GaugeBox gauge_box(Bus& b, u32 window) {
    const i32 border_top = static_cast<i32>(b.r8(window + ndk::wd_BorderTop));
    const i32 border_bottom = static_cast<i32>(b.r8(window + ndk::wd_BorderBottom));
    const i32 window_height = static_cast<i16>(b.r16(window + ndk::wd_Height));
    GaugeBox box;
    box.top = border_top + kGaugeInset;
    box.height = window_height - box.top - border_bottom;
    if (box.height < kGaugeMinHeight) {
        box.height = 0;
    }
    return box;
}

}

// Asks the volume how full it is, fits the gauge to the window and draws
// it.
void Workbench::layout_gauge(DrawerWindow& w) {
    if (w.gauge == 0) {
        return;
    }
    if (!dos_->volume_usage(w.path, w.gauge_total, w.gauge_used)) {
        w.gauge_total = 1;
        w.gauge_used = 1;
    }
    w.gauge_percent = w.gauge_total != 0
                          ? static_cast<u32>((static_cast<u64>(w.gauge_used) * 100) /
                                             w.gauge_total)
                          : 100;
    if (fit_gauge(w)) {
        intuition_->refresh_glist(w.gauge, w.window, 1);
    }
}

// Gives the gauge's gadget and image the rows the window has for them and
// paints the image for the share of the volume last asked for. A gauge that
// shrinks keeps its image's memory, so this takes none then. False when the
// window has no room for a gauge, whose gadget of no rows is not drawn.
// The planes of the screen's pens: its BitMap's depth, or 8 for a deep
// screen of a HOMBRE mode, which has 256 pens whatever its pixels hold.
u32 Workbench::pen_depth(Bus& b) const {
    const u32 bitmap = screen_ + ndk::sc_BitMap;
    return graphics_->deep(bitmap) != nullptr ? 8 : b.r8(bitmap + ndk::bm_Depth);
}

bool Workbench::fit_gauge(DrawerWindow& w) {
    Bus b(*machine_);
    const u32 depth = pen_depth(b);
    if (!b.ok() || depth == 0 || depth > 8) {
        return false;
    }
    const GaugeBox box = gauge_box(b, w.window);
    const u32 bytes = Image_SIZEOF + kGaugeRowBytes * static_cast<u32>(box.height) * depth;
    if (box.height != 0 && (w.gauge_image == 0 || bytes > w.gauge_bytes)) {
        const u32 image = exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (image == 0) {
            return false;
        }
        if (w.gauge_image != 0) {
            exec_->free(w.gauge_image, w.gauge_bytes);
        }
        w.gauge_image = image;
        w.gauge_bytes = bytes;
        b.w32(image + im_ImageData, image + Image_SIZEOF);
        b.w32(w.gauge + ndk::gg_GadgetRender, image);
    }
    w.gauge_height = box.height;
    w.gauge_depth = depth;
    b.w16(w.gauge + ndk::gg_TopEdge, static_cast<u32>(box.top));
    b.w16(w.gauge + ndk::gg_Height, static_cast<u32>(box.height));
    if (box.height == 0) {
        return false;
    }
    b.w16(w.gauge_image + im_Width, b.r16(w.gauge + ndk::gg_Width));
    b.w16(w.gauge_image + im_Height, static_cast<u32>(box.height));
    b.w16(w.gauge_image + im_Depth, depth);
    b.w8(w.gauge_image + im_PlanePick, (1u << depth) - 1);
    fill_gauge_image(w);
    return b.ok();
}

// Intuition sized the window and drew its gadgets, the gauge as high as it
// was. A gauge that reaches past the window's new inner bottom was drawn
// through the bottom border and the gadgets there, so it is made to fit
// and the borders and their gadgets are drawn again, all before the screen
// shows the window. A gauge with room to grow is left to the window's
// IDCMP_NEWSIZE.
void Workbench::gauge_sized(void* ctx, u32 window) {
    Workbench& self = *static_cast<Workbench*>(ctx);
    DrawerWindow* w =
        self.windows_.find_if([&](const DrawerWindow& d) { return d.window == window; });
    if (w == nullptr || w->gauge == 0 || w->gauge_image == 0) {
        return;
    }
    Bus b(*self.machine_);
    if (gauge_box(b, window).height >= w->gauge_height) {
        return;
    }
    self.fit_gauge(*w);
    self.intuition_->refresh_window_frame(window);
    self.intuition_->refresh_glist(b.r32(window + ndk::wd_FirstGadget), window, -1);
}

// The gauge's image in the look of the screen's depth, for the window as it
// is now, active or not.
void Workbench::fill_gauge_image(DrawerWindow& w) {
    Bus b(*machine_);
    const u32 data = b.r32(w.gauge_image + im_ImageData);
    chrome::FuelGauge gauge;
    gauge.width = static_cast<i32>(b.r16(w.gauge_image + im_Width));
    gauge.height = w.gauge_height;
    gauge.filled = chrome::fuel_gauge_filled(w.gauge_height, w.gauge_used, w.gauge_total);
    gauge.active = intuition_->active_window() == w.window;
    gauge.style = chrome::style_for_depth(w.gauge_depth);
    gauge.depth = w.gauge_depth;
    sysimage::read_pens(b, intuition_->screen_draw_info(screen_), gauge.pens.pen);
    const u32 height = static_cast<u32>(w.gauge_height);
    const u32 plane_bytes = kGaugeRowBytes * height;
    for (u32 y = 0; y < height; ++y) {
        u32 bits[8]{};
        for (i32 x = 0; x < gauge.width; ++x) {
            const u32 pen = chrome::fuel_gauge_pen(gauge, x, static_cast<i32>(y));
            for (u32 plane = 0; plane < w.gauge_depth; ++plane) {
                if ((pen & (1u << plane)) != 0) {
                    bits[plane] |= 0x8000u >> x;
                }
            }
        }
        for (u32 plane = 0; plane < w.gauge_depth; ++plane) {
            b.w16(data + plane * plane_bytes + y * kGaugeRowBytes, bits[plane]);
        }
    }
}

// The fuel gauges whose volume's share of used blocks changed since they
// were drawn.
void Workbench::refresh_gauges() {
    for (DrawerWindow& w : windows_) {
        if (w.gauge == 0) {
            continue;
        }
        u32 total = 0;
        u32 used = 0;
        if (!dos_->volume_usage(w.path, total, used)) {
            continue;
        }
        const u32 percent = total != 0
                                ? static_cast<u32>((static_cast<u64>(used) * 100) / total)
                                : 100;
        if (percent != w.gauge_percent) {
            layout_gauge(w);
        }
    }
}

// The images of the four arrow buttons, plain and pressed, which every
// window's arrows share. They stay as long as the library, with room for
// the deepest screen, and take the look of the screen's depth and pens
// each time a window gets them.
bool Workbench::build_arrow_images() {
    const u32 data_bytes = kArrowPlaneBytes * kMaxArrowDepth;
    const u32 block_bytes = kArrowImages * (Image_SIZEOF + data_bytes);
    const u32 block = exec_->alloc(block_bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (block == 0) {
        return false;
    }
    arrows_ = block;
    arrow_depth_ = 0;
    return true;
}

// Paints the arrow images for the screen as it is now, unless they already
// show its depth and pens. A screen mode change closes every window first,
// so no window shows the images while they change.
void Workbench::paint_arrow_images() {
    Bus b(*machine_);
    const u32 draw_info = intuition_->screen_draw_info(screen_);
    u32 depth = draw_info != 0 ? b.r16(draw_info + dri_Depth) : 2;
    if (depth == 0 || !b.ok()) {
        depth = 2;
    }
    depth = depth > kMaxArrowDepth ? kMaxArrowDepth : depth;
    chrome::DrawInfoPens drawinfo;
    sysimage::read_pens(b, draw_info, drawinfo.pen);
    bool same = depth == arrow_depth_;
    for (u32 i = 0; same && i < sysimage::kPenCount; ++i) {
        same = drawinfo.pen[i] == arrow_pens_.pen[i];
    }
    if (same) {
        return;
    }
    const u32 data_bytes = kArrowPlaneBytes * kMaxArrowDepth;
    const u32 data_at = arrows_ + kArrowImages * Image_SIZEOF;
    u8 pens[kArrowWidth * kArrowMaxHeight];
    for (u32 i = 0; i < kArrowImages; ++i) {
        const chrome::Arrow arrow = static_cast<chrome::Arrow>(i / 2);
        const bool pressed = (i & 1) != 0;
        const bool vertical = arrow == chrome::Arrow::Up || arrow == chrome::Arrow::Down;
        const i32 height = vertical ? 10 : kArrowMaxHeight;
        chrome::paint_arrow_button(pens, kArrowWidth, height, arrow, pressed,
                                   chrome::style_for_depth(depth), depth, drawinfo);
        const u32 image = arrows_ + i * Image_SIZEOF;
        const u32 data = data_at + i * data_bytes;
        b.w16(image + im_Width, static_cast<u32>(kArrowWidth));
        b.w16(image + im_Height, static_cast<u32>(height));
        b.w16(image + im_Depth, depth);
        b.w32(image + im_ImageData, data);
        b.w8(image + im_PlanePick, (1u << depth) - 1);
        const u32 plane_bytes = static_cast<u32>(height) * kArrowRowBytes;
        for (u32 plane = 0; plane < depth; ++plane) {
            for (i32 y = 0; y < height; ++y) {
                u32 bits = 0;
                for (i32 x = 0; x < kArrowWidth; ++x) {
                    if ((pens[y * kArrowWidth + x] & (1u << plane)) != 0) {
                        bits |= 0x8000u >> x;
                    }
                }
                b.w16(data + plane * plane_bytes + static_cast<u32>(y) * kArrowRowBytes, bits);
            }
        }
    }
    if (b.ok()) {
        arrow_depth_ = depth;
        arrow_pens_ = drawinfo;
    }
}

namespace {

constexpr char kPatternPrefs[] = "ENV:Sys/WBPattern.prefs";
constexpr u32 kMaxPatternPrefs = 2048;
constexpr i32 kPatternSize = 16;
constexpr u32 kPatternPlaneBytes = 32;
constexpr u32 kPatternBlockBytes = ndk::BitMap_SIZEOF + 8 * kPatternPlaneBytes;
// struct WBPatternPrefs, after its four reserved longs.
constexpr u32 wbp_Which = 16;
constexpr u32 wbp_Flags = 18;
constexpr u32 wbp_Depth = 21;
constexpr u32 wbp_DataLength = 22;
constexpr u32 WBPatternPrefs_SIZEOF = 24;
constexpr u32 WBPF_PATTERN = 1;

u32 be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}

u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }

bool is_id(const u8* p, const char* id) {
    return p[0] == id[0] && p[1] == id[1] && p[2] == id[2] && p[3] == id[3];
}

}

// Reads the backdrop patterns from WBPattern's prefs: a PTRN chunk for the
// root window (WBP_ROOT) and one for drawers (WBP_DRAWER), each a pattern
// of depth planes of 16 words. A picture, or a pattern of only pen 0,
// leaves the plain backdrop. True when the file is not what it was.
bool Workbench::load_patterns() {
    static u8 file[kMaxPatternPrefs];
    u32 got = 0;
    u32 error = 0;
    if (!dos_->read_file(kPatternPrefs, file, sizeof file, got, error)) {
        got = 0;
    }
    u32 sum = got;
    for (u32 i = 0; i < got; ++i) {
        sum = sum * 31 + file[i];
    }
    if (sum == pattern_sum_) {
        return false;
    }
    pattern_sum_ = sum;
    Bus b(*machine_);
    for (u32 which = 0; which < 2; ++which) {
        drop_picture(which);
        Pattern& pattern = patterns_[which];
        pattern.depth = 0;
        if (pattern.bitmap == 0) {
            pattern.bitmap = exec_->alloc(kPatternBlockBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
            if (pattern.bitmap == 0) {
                continue;
            }
            b.w16(pattern.bitmap + ndk::bm_BytesPerRow, 2);
            b.w16(pattern.bitmap + ndk::bm_Rows, kPatternSize);
            for (u32 plane = 0; plane < 8; ++plane) {
                b.w32(pattern.bitmap + ndk::bm_Planes + plane * 4,
                      pattern.bitmap + ndk::BitMap_SIZEOF + plane * kPatternPlaneBytes);
            }
        }
    }
    if (got < 12 || !is_id(file, "FORM") || !is_id(file + 8, "PREF")) {
        return true;
    }
    for (u32 at = 12; at + 8 <= got;) {
        const u32 length = be32(file + at + 4);
        const u8* data = file + at + 8;
        if (length > got - at - 8) {
            break;
        }
        if (is_id(file + at, "PTRN") && length >= WBPatternPrefs_SIZEOF) {
            const u32 which = be16(data + wbp_Which);
            const u32 flags = be16(data + wbp_Flags);
            const u32 depth = data[wbp_Depth];
            const u32 bytes = be16(data + wbp_DataLength);
            bool any = false;
            if (which < 2 && (flags & WBPF_PATTERN) != 0 && depth >= 1 && depth <= 8 &&
                bytes >= depth * kPatternPlaneBytes &&
                WBPatternPrefs_SIZEOF + depth * kPatternPlaneBytes <= length &&
                patterns_[which].bitmap != 0) {
                const u32 planes = patterns_[which].bitmap + ndk::BitMap_SIZEOF;
                for (u32 i = 0; i < depth * kPatternPlaneBytes; ++i) {
                    const u8 v = data[WBPatternPrefs_SIZEOF + i];
                    b.w8(planes + i, v);
                    any = any || v != 0;
                }
                b.w8(patterns_[which].bitmap + ndk::bm_Depth, depth);
                patterns_[which].depth = any ? depth : 0;
            } else if (which < 2 && (flags & WBPF_PATTERN) == 0 && bytes != 0 &&
                       WBPatternPrefs_SIZEOF + bytes <= length) {
                // A picture: its file name follows.
                char name[Dos::kMaxPath];
                u32 n = 0;
                for (; n < bytes && n + 1 < sizeof name; ++n) {
                    const u8 c = data[WBPatternPrefs_SIZEOF + n];
                    if (c == 0) {
                        break;
                    }
                    name[n] = static_cast<char>(c);
                }
                name[n] = '\0';
                load_picture(which, name);
            }
        }
        at += 8 + length + (length & 1);
    }
    LogLine().text("workbench: backdrop patterns read").send();
    return true;
}

// The window's pattern over its inner area, in tiles from its top-left
// corner; the window's clip keeps them inside.
// The tiles lie from the inner area's corner, whichever part is painted.
void Workbench::fill_pattern(const DrawerWindow& drawer, u32 which, const Rect& area) {
    const Pattern& pattern = patterns_[which];
    const u32 bitmap = pattern.picture != 0 ? pattern.picture : pattern.bitmap;
    if (pattern.depth == 0 || bitmap == 0 || pattern.width <= 0 || pattern.height <= 0) {
        return;
    }
    const i32 first_x =
        drawer.inner_left + (area.x0 - drawer.inner_left) / pattern.width * pattern.width;
    const i32 first_y =
        drawer.inner_top + (area.y0 - drawer.inner_top) / pattern.height * pattern.height;
    for (i32 y = first_y; y <= area.y1; y += pattern.height) {
        for (i32 x = first_x; x <= area.x1; x += pattern.width) {
            const i32 w = std::min(area.x1 - x + 1, pattern.width);
            const i32 h = std::min(area.y1 - y + 1, pattern.height);
            graphics_->blt_bitmap_rastport(bitmap, 0, 0, drawer.rp, x, y, w, h, 0xC0);
        }
    }
}

void Workbench::drop_picture(u32 which) {
    Pattern& pattern = patterns_[which];
    if (pattern.picture != 0) {
        exec_->free(pattern.picture, pattern.picture_bytes);
    }
    pattern.picture = 0;
    pattern.picture_bytes = 0;
    pattern.width = kPatternSize;
    pattern.height = kPatternSize;
}

namespace {

constexpr u32 kMaxPicture = 256 * 1024;
constexpr u32 kMaxPictureSide = 1024;

// Unpacks one row of ByteRun1 data from at into out: false if it runs
// past the end of the data.
bool unpack_row(const u8* data, u32 size, u32& at, u8* out, u32 bytes) {
    u32 done = 0;
    while (done < bytes) {
        if (at >= size) {
            return false;
        }
        const i32 n = static_cast<i8>(data[at++]);
        if (n >= 0) {
            const u32 count = static_cast<u32>(n) + 1;
            if (at + count > size || done + count > bytes) {
                return false;
            }
            for (u32 i = 0; i < count; ++i) {
                out[done++] = data[at++];
            }
        } else if (n != -128) {
            const u32 count = static_cast<u32>(-n) + 1;
            if (at >= size || done + count > bytes) {
                return false;
            }
            const u8 value = data[at++];
            for (u32 i = 0; i < count; ++i) {
                out[done++] = value;
            }
        }
    }
    return true;
}

}

// An ILBM picture as a backdrop: its colours go to the nearest pens of the
// screen, and it tiles the window as large as it is. A file that cannot
// be read or is no ILBM of up to eight planes leaves the plain backdrop.
void Workbench::load_picture(u32 which, const char* name) {
    static u8 file[kMaxPicture];
    static u8 rows[9][kMaxPictureSide / 8];
    u32 got = 0;
    u32 error = 0;
    if (!dos_->read_file(name, file, sizeof file, got, error) || got < 12 ||
        !is_id(file, "FORM") || !is_id(file + 8, "ILBM")) {
        LogLine().text("workbench: no picture in ").text(name).send();
        return;
    }
    u32 width = 0;
    u32 height = 0;
    u32 planes = 0;
    u32 masking = 0;
    u32 compression = 0;
    const u8* cmap = nullptr;
    u32 colors = 0;
    const u8* body = nullptr;
    u32 body_bytes = 0;
    for (u32 at = 12; at + 8 <= got;) {
        const u32 length = be32(file + at + 4);
        const u8* data = file + at + 8;
        if (length > got - at - 8) {
            break;
        }
        if (is_id(file + at, "BMHD") && length >= 20) {
            width = be16(data);
            height = be16(data + 2);
            planes = data[8];
            masking = data[9];
            compression = data[10];
        } else if (is_id(file + at, "CMAP")) {
            cmap = data;
            colors = length / 3;
        } else if (is_id(file + at, "BODY")) {
            body = data;
            body_bytes = length;
        }
        at += 8 + length + (length & 1);
    }
    if (body == nullptr || width == 0 || height == 0 || width > kMaxPictureSide ||
        height > kMaxPictureSide || planes == 0 || planes > 8 || compression > 1) {
        LogLine().text("workbench: cannot show the picture ").text(name).send();
        return;
    }
    // The screen's pens nearest each colour of the picture.
    Bus b(*machine_);
    const u32 depth = pen_depth(b);
    const u32 colormap = b.r32(screen_ + ndk::sc_ViewPort + ndk::vp_ColorMap);
    if (!b.ok() || depth == 0 || depth > 8) {
        return;
    }
    u8 pens[256]{};
    const u32 screen_pens = 1u << depth;
    for (u32 c = 0; c < (1u << planes); ++c) {
        const i32 r = c < colors ? cmap[c * 3] : 0;
        const i32 g = c < colors ? cmap[c * 3 + 1] : 0;
        const i32 bl = c < colors ? cmap[c * 3 + 2] : 0;
        u32 best = 0;
        i32 best_distance = -1;
        for (u32 pen = 0; pen < screen_pens; ++pen) {
            u8 pr = 0;
            u8 pg = 0;
            u8 pb = 0;
            if (!graphics_->color_of(colormap, pen, pr, pg, pb)) {
                break;
            }
            const i32 distance = (r - pr) * (r - pr) + (g - pg) * (g - pg) + (bl - pb) * (bl - pb);
            if (best_distance < 0 || distance < best_distance) {
                best_distance = distance;
                best = pen;
            }
        }
        pens[c] = static_cast<u8>(best);
    }
    const u32 row_bytes = ((width + 15) / 16) * 2;
    const u32 plane_bytes = row_bytes * height;
    const u32 bytes = ndk::BitMap_SIZEOF + depth * plane_bytes;
    const u32 bitmap = exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (bitmap == 0) {
        return;
    }
    b.w16(bitmap + ndk::bm_BytesPerRow, row_bytes);
    b.w16(bitmap + ndk::bm_Rows, height);
    b.w8(bitmap + ndk::bm_Depth, depth);
    for (u32 plane = 0; plane < depth; ++plane) {
        b.w32(bitmap + ndk::bm_Planes + plane * 4,
              bitmap + ndk::BitMap_SIZEOF + plane * plane_bytes);
    }
    // Each row has a row of every plane, and of the mask with
    // mskHasMask, plain or packed with ByteRun1.
    const u32 stored = planes + (masking == 1 ? 1 : 0);
    u32 at = 0;
    bool whole = true;
    for (u32 y = 0; y < height && whole; ++y) {
        for (u32 p = 0; p < stored && whole; ++p) {
            if (compression == 1) {
                whole = unpack_row(body, body_bytes, at, rows[p], row_bytes);
            } else if (at + row_bytes <= body_bytes) {
                for (u32 i = 0; i < row_bytes; ++i) {
                    rows[p][i] = body[at++];
                }
            } else {
                whole = false;
            }
        }
        for (u32 x = 0; x < width && whole; ++x) {
            u32 index = 0;
            for (u32 p = 0; p < planes; ++p) {
                index |= ((rows[p][x / 8] >> (7 - (x & 7))) & 1u) << p;
            }
            const u32 pen = pens[index];
            for (u32 plane = 0; plane < depth; ++plane) {
                if ((pen >> plane) & 1) {
                    const u32 byte = bitmap + ndk::BitMap_SIZEOF + plane * plane_bytes +
                                     y * row_bytes + x / 8;
                    b.w8(byte, b.r8(byte) | (0x80u >> (x & 7)));
                }
            }
        }
    }
    if (!whole || !b.ok()) {
        exec_->free(bitmap, bytes);
        LogLine().text("workbench: the picture ").text(name).text(" is cut short").send();
        return;
    }
    Pattern& pattern = patterns_[which];
    pattern.picture = bitmap;
    pattern.picture_bytes = bytes;
    pattern.width = static_cast<i32>(width);
    pattern.height = static_cast<i32>(height);
    pattern.depth = depth;
    LogLine().text("workbench: backdrop picture ").text(name).send();
}

void Workbench::redraw_all() {
    for (const DrawerWindow& w : windows_) {
        draw_window(w.id);
    }
}

// The labels take their size from the icon font: the unplaced icons take
// their cells again and every window draws its labels in the new one.
void Workbench::relayout_labels() {
    const DrawerWindow* root = find_window(kRootWindow);
    if (root == nullptr) {
        return;
    }
    place_root_icons(*root);
    for (const DrawerWindow& w : windows_) {
        if (w.id != kRootWindow) {
            place_icons(w);
        }
    }
    redraw_all();
}

void Workbench::refresh_title() {
    const DrawerWindow* root = find_window(kRootWindow);
    if (root == nullptr) {
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
    put("Hombre Workbench  ");
    format_u64(digits, sizeof digits, exec_->free_bytes(ndk::MEMF_CHIP), 10);
    put(digits);
    put(" graphics mem  ");
    format_u64(digits, sizeof digits, exec_->free_bytes(ndk::MEMF_FAST), 10);
    put(digits);
    put(" other mem");
    text[at] = '\0';
    Bus b(*machine_);
    for (u32 i = 0; i <= at; ++i) {
        b.w8(title_buffer_ + i, static_cast<u8>(text[i]));
    }
    if (b.ok()) {
        intuition_->set_window_titles(root->window, 0xFFFFFFFF, title_buffer_);
    }
}

// --- the desktop's actions ----------------------------------------------

const Workbench::IconEntry* Workbench::icon_at(u32 window_id, i32 x, i32 y) const {
    return icons_.find_if([&](const IconEntry& icon) {
        return icon.owner == window_id && x >= icon.left && x < icon.left + icon.width &&
               y >= icon.top && y < icon.top + icon.height;
    });
}

void Workbench::open_icon(u32 window_id, u32 icon_id) {
    const DrawerWindow* window = find_window(window_id);
    const IconEntry* icon = find_icon(icon_id);
    if (window == nullptr || icon == nullptr) {
        return;
    }
    const char* drawer = drawer_of(*window, *icon);
    switch (icon->type) {
    case kDisk:
    case kDrawer:
    case kGarbage:
        open_drawer(window_id, *icon);
        break;
    case kTool:
        start_tool(*icon, drawer);
        break;
    case kProject:
        start_project(*icon, drawer);
        break;
    default:
        break;
    }
}

void Workbench::mouse_click(u32 window_id, i32 mx, i32 my, u64 now_us) {
    DrawerWindow* drawer = find_window(window_id);
    if (drawer == nullptr) {
        return;
    }
    cancel_drag();
    const i32 x = mx - drawer->inner_left + drawer->scroll_x;
    const i32 y = my - drawer->inner_top + drawer->scroll_y;
    const IconEntry* hit = icon_at(window_id, x, y);
    const u32 hit_id = hit != nullptr ? hit->id : kNoIcon;
    const u32 before = drawer->clicks.selected();
    const ClickAction action = drawer->clicks.click(hit_id, now_us);
    switch (action) {
    case ClickAction::Select:
    case ClickAction::Clear:
        repaint_selection(window_id, before, drawer->clicks.selected());
        break;
    case ClickAction::Open:
        repaint_selection(window_id, before, drawer->clicks.selected());
        open_icon(window_id, hit_id);
        break;
    case ClickAction::None:
        break;
    }
    // The press that does not open the icon may start dragging it.
    i32 origin_x = 0;
    i32 origin_y = 0;
    if (hit == nullptr || action == ClickAction::Open ||
        !screen_origin(window_id, origin_x, origin_y)) {
        return;
    }
    drag_ = IconDrag{};
    drag_.armed = true;
    drag_.window = window_id;
    drag_.icon = hit_id;
    drag_.grab_x = x - hit->left;
    drag_.grab_y = y - hit->top;
    drag_.start_x = origin_x + mx;
    drag_.start_y = origin_y + my;
    intuition_->report_mouse(drawer->window, true);
}

// The window's top-left corner on the screen; the pointer positions its
// messages carry are relative to it.
bool Workbench::screen_origin(u32 window_id, i32& x, i32& y) const {
    const DrawerWindow* w = find_window(window_id);
    if (w == nullptr) {
        return false;
    }
    Bus b(*machine_);
    x = static_cast<i16>(b.r16(w->window + ndk::wd_LeftEdge));
    y = static_cast<i16>(b.r16(w->window + ndk::wd_TopEdge));
    return b.ok();
}

void Workbench::drag_to(u32 window_id, i32 mx, i32 my) {
    if (!drag_.armed || window_id != drag_.window) {
        return;
    }
    i32 origin_x = 0;
    i32 origin_y = 0;
    if (!screen_origin(window_id, origin_x, origin_y)) {
        return;
    }
    const i32 sx = origin_x + mx;
    const i32 sy = origin_y + my;
    if (!drag_.active) {
        const i32 dx = sx - drag_.start_x;
        const i32 dy = sy - drag_.start_y;
        if (dx <= kDragThreshold && dx >= -kDragThreshold && dy <= kDragThreshold &&
            dy >= -kDragThreshold) {
            return;
        }
        drag_.active = true;
    }
    Bus b(*machine_);
    const IconEntry* icon = find_icon(drag_.icon);
    const IconArt art = art_of(b, icon != nullptr ? icon->object : 0);
    if (art.render == 0) {
        return;
    }
    // The image's corner goes where the icon's corner would be.
    intuition_->set_drag_image(screen_, art.render, sx - drag_.grab_x, sy - drag_.grab_y);
}

void Workbench::end_drag(u32 window_id, i32 mx, i32 my) {
    if (!drag_.armed || window_id != drag_.window) {
        return;
    }
    const IconDrag ended = drag_;
    i32 origin_x = 0;
    i32 origin_y = 0;
    DrawerWindow* drawer = find_window(window_id);
    const bool placed = drawer != nullptr && screen_origin(window_id, origin_x, origin_y);
    cancel_drag();
    if (!ended.active || !placed) {
        return;
    }
    // A drag is no first click of a double click.
    drawer->clicks.forget();
    drop(ended, origin_x + mx, origin_y + my);
}

void Workbench::cancel_drag() {
    if (!drag_.armed) {
        return;
    }
    const DrawerWindow* drawer = find_window(drag_.window);
    if (drawer != nullptr) {
        intuition_->report_mouse(drawer->window, false);
    }
    intuition_->clear_drag_image();
    drag_ = IconDrag{};
}

// Where a dragged icon lands: its own window takes it at the new place; a
// drawer or disk icon, or another drawer's window, takes the file.
void Workbench::drop(const IconDrag& drag, i32 sx, i32 sy) {
    IconEntry* dragged = find_icon(drag.icon);
    const DrawerWindow* source = find_window(drag.window);
    if (dragged == nullptr || dragged->owner != drag.window || source == nullptr) {
        return;
    }
    IconEntry& icon = *dragged;
    const u32 under = intuition_->window_under(sx, sy);
    const DrawerWindow* to =
        windows_.find_if([&](const DrawerWindow& w) { return w.window == under; });
    i32 origin_x = 0;
    i32 origin_y = 0;
    if (to == nullptr || !screen_origin(to->id, origin_x, origin_y)) {
        LogLine().text("workbench: ").text(icon.name).text(" dropped nowhere").send();
        return;
    }
    const i32 px = sx - origin_x - to->inner_left + to->scroll_x;
    const i32 py = sy - origin_y - to->inner_top + to->scroll_y;
    const i32 left = px - drag.grab_x;
    const i32 top = py - drag.grab_y;
    const IconEntry* on = icon_at(to->id, px, py);
    if (on == &icon) {
        on = nullptr;
    }
    const bool into_icon =
        on != nullptr && (on->type == kDrawer || on->type == kDisk || on->type == kGarbage);
    if (to->id == drag.window && !into_icon) {
        icon.left = left < 0 ? 0 : left;
        icon.top = top < 0 ? 0 : top;
        icon.placed = true;
        draw_window(to->id);
        return;
    }
    if ((icon.type != kTool && icon.type != kProject) || left_out(icon.id) != nullptr) {
        LogLine().text("workbench: ").text(icon.name).text(" cannot be moved there").send();
        return;
    }
    char dest[Dos::kMaxPath];
    if (into_icon) {
        if (!icon_target_path(*to, *on, dest)) {
            return;
        }
    } else if (to->id == kRootWindow) {
        LogLine().text("workbench: ").text(icon.name).text(" cannot be left out").send();
        return;
    } else {
        copy_text(dest, sizeof dest, to->path);
    }
    char from[Dos::kMaxPath];
    copy_text(from, sizeof from, source->path);
    if (same_ci(dest, from)) {
        return;
    }
    // The windows are filled again, so the icon is copied first.
    const IconEntry moving = icon;
    const i32 keep_left = into_icon ? static_cast<i32>(kNoIconPosition) : (left < 0 ? 0 : left);
    const i32 keep_top = into_icon ? static_cast<i32>(kNoIconPosition) : (top < 0 ? 0 : top);
    if (!transfer(moving, from, dest, keep_left, keep_top)) {
        return;
    }
    for (DrawerWindow& w : windows_) {
        if (w.id != kRootWindow && (same_ci(w.path, from) || same_ci(w.path, dest))) {
            fill_drawer(w);
            draw_window(w.id);
        }
    }
}

void Workbench::open_parent(u32 window_id) {
    const DrawerWindow* window = find_window(window_id);
    if (window_id == kRootWindow || window == nullptr) {
        return;
    }
    const char* path = window->path;
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
    open_drawer_window(parent, name, object, nullptr, window->show_all);
    if (object != 0) {
        icon_->free_object(object);
    }
}

void Workbench::select_contents(u32 window_id) {
    DrawerWindow* drawer = find_window(window_id);
    const IconEntry* first =
        icons_.find_if([&](const IconEntry& icon) { return icon.owner == window_id; });
    if (drawer != nullptr && first != nullptr) {
        drawer->clicks.select(first->id);
        draw_window(window_id);
    }
}

void Workbench::clean_up(u32 window_id) {
    const DrawerWindow* drawer = find_window(window_id);
    if (drawer == nullptr) {
        return;
    }
    for (IconEntry& icon : icons_) {
        if (icon.owner == window_id) {
            icon.placed = false;
        }
    }
    place_icons(*drawer);
    draw_window(window_id);
}

void Workbench::open_selected(u32 window_id) {
    const DrawerWindow* drawer = find_window(window_id);
    if (drawer != nullptr && drawer->clicks.selected() != kNoIcon) {
        open_icon(window_id, drawer->clicks.selected());
    }
}

namespace {

// The volume a path is on: a disk's by the device name of its drive ("DF0",
// "DH0"), whether the path names the drive or the disk's volume, and RAM: by
// the Ram Disk's name.
void volume_of(const Dos& dos, const char* path, char (&out)[Dos::kMaxPath]) {
    copy_text(out, sizeof out, path);
    for (u32 i = 0; out[i] != '\0'; ++i) {
        if (out[i] == ':') {
            out[i] = '\0';
            break;
        }
    }
    if (same_ci(out, "RAM")) {
        copy_text(out, sizeof out, "Ram Disk");
        return;
    }
    for (u32 n = 0; n < dos.drive_count(); ++n) {
        const char* device = dos.device_name(n);
        if (same_ci(out, device) || (dos.has_disk(n) && same_ci(out, dos.volume_name(n)))) {
            copy_text(out, sizeof out, device);
            return;
        }
    }
}

bool same_volume(const Dos& dos, const char* a, const char* b) {
    char va[Dos::kMaxPath];
    char vb[Dos::kMaxPath];
    volume_of(dos, a, va);
    volume_of(dos, b, vb);
    return same_ci(va, vb);
}

const char* error_text(u32 error) {
    switch (error) {
    case ERROR_DISK_WRITE_PROTECTED:
        return "Disk is write-protected";
    case ERROR_OBJECT_EXISTS:
        return "Object already exists";
    case ERROR_OBJECT_NOT_FOUND:
        return "Object not found";
    case ERROR_NO_FREE_STORE:
        return "Not enough memory";
    default:
        return nullptr;
    }
}

}  // namespace

// Copies the file and its icon from one drawer to another, the icon at
// left, top; on the same volume the originals then go, so the file moves.
bool Workbench::transfer(const IconEntry& icon, const char* from, const char* to, i32 left,
                         i32 top) {
    char src[Dos::kMaxPath];
    char dst[Dos::kMaxPath];
    if (!join_path(from, icon.file, src) || !join_path(to, icon.file, dst)) {
        LogLine().text("workbench: path too long for ").text(icon.file).send();
        return false;
    }
    if (dos_->examine(dst).kind != Dos::TargetKind::Missing) {
        show_error("cannot copy", icon.file, ERROR_OBJECT_EXISTS);
        return false;
    }
    u32 error = 0;
    if (!copy_file(src, dst, error)) {
        show_error("cannot copy", icon.file, error);
        return false;
    }
    // A plain icon has no .info to take along or to write.
    bool saved = true;
    if (!icon.plain) {
        const u32 object = icon_->load_object(src);
        saved = false;
        if (object != 0) {
            Bus b(*machine_);
            b.w32(object + do_CurrentX, static_cast<u32>(left));
            b.w32(object + do_CurrentY, static_cast<u32>(top));
            saved = b.ok() && icon_->save_object(dst, object, error);
            icon_->free_object(object);
        }
    }
    if (!saved) {
        u32 ignored = 0;
        dos_->delete_file(dst, ignored);
        show_error("cannot copy", icon.file, error);
        return false;
    }
    if (!same_volume(*dos_, from, to)) {
        LogLine().text("workbench: copied ").text(src).text(" to ").text(to).send();
        return true;
    }
    char src_info[Dos::kMaxPath];
    u32 ignored = 0;
    dos_->delete_file(src, ignored);
    if (!icon.plain && info_path(src, src_info)) {
        dos_->delete_file(src_info, ignored);
    }
    move_left_outs(src, dst);
    LogLine().text("workbench: moved ").text(src).text(" to ").text(to).send();
    return true;
}

void Workbench::show_error(const char* what, const char* name, u32 error) {
    Message message;
    message.add(name).add(": ");
    const char* reason = error_text(error);
    if (reason != nullptr) {
        message.add(reason);
    } else {
        message.add("Error ").number(error);
    }
    LogLine().text("workbench: ").text(what).text(" ").text(message.text).send();
    copy_text(last_message_, sizeof last_message_, message.text);
    show_requester("Workbench Request", message.text);
}

void Workbench::show_requester(const char* title, const char* text) {
    easy(title, text, "Ok");
}

bool Workbench::confirm(const char* title, const char* text) {
    if (answer_ != nullptr) {
        return answer_(answer_ctx_, title, text, nullptr, 0);
    }
    return easy(title, text, "Ok|Cancel") == 1;
}

// Only the running desktop shows a requester: it waits for a button, which
// nothing presses in a test.
i32 Workbench::easy(const char* title, const char* text, const char* gadgets) {
    if (!running_ || machine_ == nullptr || exec_ == nullptr || intuition_ == nullptr) {
        return 0;
    }
    constexpr char kFormat[] = "%s";
    const u32 title_bytes = text_length(title) + 1;
    const u32 text_bytes = text_length(text) + 1;
    const u32 gadgets_bytes = text_length(gadgets) + 1;
    const u32 bytes = ndk::EasyStruct_SIZEOF + 4 + sizeof kFormat + gadgets_bytes + title_bytes +
                      text_bytes;
    const u32 block = exec_->alloc(bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (block == 0) {
        return 0;
    }
    Bus b(*machine_);
    const u32 args = block + ndk::EasyStruct_SIZEOF;
    u32 at = args + 4;
    auto put = [&](const char* part, u32 length) {
        const u32 start = at;
        for (u32 i = 0; i < length; ++i) {
            b.w8(at + i, static_cast<u8>(part[i]));
        }
        at += length;
        return start;
    };
    const u32 format_at = put(kFormat, sizeof kFormat);
    const u32 gadgets_at = put(gadgets, gadgets_bytes);
    const u32 title_at = put(title, title_bytes);
    const u32 text_at = put(text, text_bytes);
    b.w32(block + ndk::es_StructSize, ndk::EasyStruct_SIZEOF);
    b.w32(block + ndk::es_Flags, 0);
    b.w32(block + ndk::es_Title, title_at);
    b.w32(block + ndk::es_TextFormat, format_at);
    b.w32(block + ndk::es_GadgetFormat, gadgets_at);
    b.w32(args, text_at);
    const DrawerWindow* root = find_window(kRootWindow);
    const u32 window = root != nullptr ? root->window : 0;
    const i32 answer = b.ok() ? intuition_->easy_request(window, block, 0, args) : 0;
    exec_->free(block, bytes);
    return answer;
}

bool Workbench::can_show_dialog() const {
    return machine_ != nullptr && exec_ != nullptr && intuition_ != nullptr &&
           gadtools_ != nullptr && graphics_ != nullptr && dos_ != nullptr && screen_ != 0;
}

template <typename OnGadget>
Workbench::Reply Workbench::dialog_input(const Dialog& dialog, OnGadget on_gadget) {
    Bus b(*machine_);
    const u32 port = b.r32(dialog.window + ndk::wd_UserPort);
    u32 message = 0;
    while (b.ok() && port != 0 && (message = exec_->get_message(port)) != 0) {
        const u32 idcmp_class = b.r32(message + ndk::im_Class);
        const u32 code = b.r16(message + ndk::im_Code);
        const u32 gadget = b.r32(message + ndk::im_IAddress);
        exec_->reply_message(message);
        if (idcmp_class == ndk::IDCMP_CLOSEWINDOW) {
            return Reply::Cancelled;
        }
        if (idcmp_class != ndk::IDCMP_GADGETUP) {
            continue;
        }
        const u32 id = b.r16(gadget + ndk::gg_GadgetID);
        const Reply reply = id == kDialogCancel ? Reply::Cancelled : on_gadget(id, code);
        if (reply != Reply::Waiting) {
            return reply;
        }
    }
    return b.ok() && port != 0 ? Reply::Waiting : Reply::Cancelled;
}

template <typename Step>
Workbench::Reply Workbench::wait_for_reply(const Dialog& dialog, Step step) {
    Bus b(*machine_);
    const u32 bit = b.r8(b.r32(dialog.window + ndk::wd_UserPort) + ndk::mp_SigBit);
    Reply reply = Reply::Cancelled;
    while (b.ok() && bit < 32 && (reply = step()) == Reply::Waiting &&
           exec_->wait_for(1u << bit, ~u64(0)) != 0) {
    }
    return reply;
}

void Workbench::close_dialog(Dialog& dialog) {
    if (dialog.window != 0) {
        intuition_->close_window(dialog.window);
    }
    gadtools_->free_gadgets(dialog.gadgets);
    if (dialog.block != 0) {
        exec_->free(dialog.block, dialog.block_bytes);
    }
    dialog = Dialog{};
}

bool Workbench::ask_text(const char* title, const char* prompt, const char* initial, char* out,
                         u32 size) {
    if (out == nullptr || size == 0) {
        return false;
    }
    if (answer_ != nullptr) {
        copy_text(out, size, initial);
        return answer_(answer_ctx_, title, prompt, out, size);
    }
    TextRequest request{};
    if (!running_ || !open_text_request(title, prompt, initial, size, request)) {
        return false;
    }
    const Reply reply =
        wait_for_reply(request, [&] { return text_request_input(request, out, size); });
    close_dialog(request);
    return reply == Reply::Accepted;
}

// A prompt above a string gadget, with Ok and Cancel buttons under it.
bool Workbench::open_text_request(const char* title, const char* prompt, const char* initial,
                                  u32 size, TextRequest& request) {
    using namespace gadtools_defs;
    if (!can_show_dialog() || size < 2) {
        return false;
    }
    request = TextRequest{};
    DialogBuilder dialog(*machine_, *exec_, *graphics_, *intuition_, *gadtools_, screen_, request,
                         {title, prompt, initial, "Ok", "Cancel"});
    const u32 title_at = dialog.put(title);
    const u32 prompt_at = dialog.put(prompt);
    const u32 initial_at = dialog.put(initial);
    const u32 ok_at = dialog.put("Ok");
    const u32 cancel_at = dialog.put("Cancel");
    // The prompt takes a line above the string gadget.
    const i32 string_top = dialog.top() + dialog.font_height() + 2;
    const i32 button_top = string_top + dialog.line_height() + DialogBuilder::kMargin;
    request.string =
        dialog.add(STRING_KIND, kRequestString, DialogBuilder::kMargin, string_top,
                   dialog.inner_width(), prompt_at, PLACETEXT_ABOVE,
                   dialog.tags({GTST_String, initial_at, GTST_MaxChars, size - 1}));
    dialog.buttons(button_top, ok_at, cancel_at);
    if (!dialog.open(title_at, button_top + dialog.line_height())) {
        LogLine().text("workbench: could not ask for ").text(prompt).send();
        close_dialog(request);
        return false;
    }
    intuition_->activate_string(request.string, request.window);
    return true;
}

Workbench::Reply Workbench::text_request_input(const TextRequest& request, char* out, u32 size) {
    return dialog_input(request, [&](u32 id, u32 code) {
        if (id != kRequestOk && (id != kRequestString || code != '\r')) {
            return Reply::Waiting;
        }
        Bus b(*machine_);
        const u32 info = b.r32(request.string + ndk::gg_SpecialInfo);
        return b.string(b.r32(info + ndk::si_Buffer), out, size) ? Reply::Accepted
                                                                  : Reply::Cancelled;
    });
}

// The information window, laid out as the 3.1 one. The check boxes go down
// the right edge, a row each; the lines of the left block share those rows
// and the framed image stands between the two, under the name. The date is
// on the last of the rows, and the string gadgets, the tool types and the
// buttons follow below. A short screen gets fewer rows of tool types.
bool Workbench::open_info(u32 window_id, u32 icon_id, InfoRequest& request) {
    using namespace gadtools_defs;
    const DrawerWindow* window = find_window(window_id);
    const IconEntry* icon = file_icon(window_id, icon_id);
    if (icon == nullptr && window != nullptr && window_id == kRootWindow) {
        // A disk on the desktop.
        const IconEntry* disk = find_icon(icon_id);
        icon = disk != nullptr && disk->owner == kRootWindow && disk->type == kDisk ? disk
                                                                                   : nullptr;
    }
    if (icon == nullptr) {
        const IconEntry* other = find_icon(icon_id);
        show_error("cannot show", other != nullptr ? other->name : "?",
                   ERROR_OBJECT_WRONG_TYPE);
        return false;
    }
    if (!can_show_dialog() || icon->object == 0) {
        return false;
    }
    const bool is_disk = icon->type == kDisk;
    char path[Dos::kMaxPath];
    char drawer[Dos::kMaxPath];
    if (!icon_path(*window, *icon, path)) {
        return false;
    }
    if (!is_disk) {
        copy_text(drawer, sizeof drawer, drawer_of(*window, *icon));
    } else if (!volume_root(*icon, drawer)) {
        return false;
    }
    Bus b(*machine_);
    Dos::FileInfo info{};
    Dos::VolumeInfo volume{};
    bool has_object = false;
    if (is_disk) {
        has_object = dos_->volume_info(drawer, volume);
    } else {
        info = dos_->file_info(path);
        has_object = info.kind != Dos::TargetKind::Missing;
    }
    // An icon that stands for nothing but its .info shows what its type has.
    const bool as_file = !is_disk && (has_object ? info.kind == Dos::TargetKind::File
                                                 : icon->type == kTool || icon->type == kProject);
    const bool show_stack = icon->type == kTool || icon->type == kProject;
    const bool show_default_tool = icon->type == kProject || is_disk;
    const bool show_tool_types = show_stack || is_disk || icon->type == kDrawer ||
                                 icon->type == kGarbage;

    Message name_line;
    name_line.add(icon->name).add(" (").add(object_type_name(icon->type)).add(")");
    // A file's blocks and bytes; a disk's blocks, those used and free and
    // the bytes of one.
    Message blocks_line;
    Message second_line;
    Message free_line;
    Message block_size_line;
    if (is_disk && has_object) {
        blocks_line.grouped(volume.blocks);
        second_line.grouped(volume.used);
        free_line.grouped(volume.blocks > volume.used ? volume.blocks - volume.used : 0);
        block_size_line.grouped(volume.block_bytes);
    } else if (has_object && as_file) {
        blocks_line.grouped(info.blocks);
        second_line.grouped(info.size);
    }
    Message date_line;
    if (has_object) {
        char date[LEN_DATSTRING];
        char time[LEN_DATSTRING];
        const DosStamp stamp = is_disk ? DosStamp{volume.days, volume.minutes, volume.ticks}
                                       : DosStamp{info.days, info.minutes, info.ticks};
        if (dos_format_date(stamp, FORMAT_DEF, 0, 0, date, sizeof date, nullptr, 0, time,
                            sizeof time) != 0) {
            date_line.add(date).add(" ").add(time);
        }
    }
    const char* status = !is_disk || !has_object ? ""
                         : volume.writable       ? "Read/Write"
                                                 : "Read Only";
    char default_tool[kMaxDefaultToolLength + 1]{};
    const u32 default_ptr = b.r32(icon->object + do_DefaultTool);
    for (u32 i = 0; default_ptr != 0 && i < kMaxDefaultToolLength; ++i) {
        const u32 c = b.r8(default_ptr + i);
        if (c == 0) {
            break;
        }
        default_tool[i] = static_cast<char>(c);
        default_tool[i + 1] = '\0';
    }
    const i32 stack = static_cast<i32>(b.r32(icon->object + do_StackSize));

    request = InfoRequest{};
    copy_text(request.path, sizeof request.path, path);
    copy_text(request.name, sizeof request.name, icon->name);
    copy_text(request.comment_text, sizeof request.comment_text, info.comment);
    request.protection = info.protection;
    request.has_object = has_object;
    request.is_disk = is_disk;
    request.drive = icon->drive;
    request.window_id = window_id;
    request.icon_id = icon_id;
    request.show_stack = show_stack;
    request.show_default_tool = show_default_tool;
    request.show_tool_types = show_tool_types;

    static_assert(kProtectionBoxCount == 6);
    DialogBuilder dialog(
        *machine_, *exec_, *graphics_, *intuition_, *gadtools_, screen_, request,
        {name_line.text, blocks_line.text, second_line.text, free_line.text,
         block_size_line.text, date_line.text, status, default_tool, drawer, info.comment,
         "Blocks:", "Bytes:", "Used:", "Free:", "Block size:", "Stack:", "Last Changed:",
         "Created:", "Status:", "Comment:", "Default Tool:", "Tool Types:", "New", "Del",
         "Save", "Cancel", "", kProtectionBoxes[0].label, kProtectionBoxes[1].label,
         kProtectionBoxes[2].label, kProtectionBoxes[3].label, kProtectionBoxes[4].label,
         kProtectionBoxes[5].label},
        kInfoExtraBytes, 58);
    if (dialog.extra() == 0) {
        close_dialog(request);
        return false;
    }

    const u32 extra = dialog.extra();
    request.list = extra;
    request.rows = extra + kInfoRowsAt;
    request.default_text = extra + kInfoDefaultAt;
    request.original_default = extra + kInfoOriginalDefaultAt;
    request.tags = extra + kInfoTagsAt;
    request.empty_text = dialog.put("");
    request.original_stack = static_cast<u32>(stack);
    u32 default_length = 0;
    for (; default_tool[default_length] != '\0'; ++default_length) {
        b.w8(request.default_text + default_length,
             static_cast<u8>(default_tool[default_length]));
        b.w8(request.original_default + default_length,
             static_cast<u8>(default_tool[default_length]));
    }
    b.w8(request.default_text + default_length, 0);
    b.w8(request.original_default + default_length, 0);
    if (show_tool_types) {
        const u32 array = b.r32(icon->object + do_ToolTypes);
        while (array != 0 && request.row_count < kMaxToolTypes) {
            const u32 entry = b.r32(array + request.row_count * 4);
            if (entry == 0) {
                break;
            }
            const u32 row = request.rows + request.row_count * kInfoRowStride;
            copy_guest_text(b, row, entry, kMaxToolTypeLength);
            request.origin[request.row_count] = static_cast<u8>(request.row_count);
            ++request.row_count;
        }
    }
    request.original_row_count = request.row_count;
    link_tool_types(request);

    // The columns: a label ends a gap before its gadget, the labels of the
    // left block before value_left, those of the rows below before
    // field_left, and those of the check boxes before box_left.
    constexpr i32 kLeftLabelChars = 11;   // "Block size:"
    constexpr i32 kFieldLabelChars = 13;  // "Last Changed:"
    constexpr i32 kValueChars = 12;
    constexpr i32 kStatusChars = 10;      // "Read/Write"
    constexpr i32 kRowGap = 4;
    constexpr i32 kBottom = 6;
    const i32 margin = DialogBuilder::kMargin;
    const i32 cell = dialog.char_width();
    const i32 font_h = dialog.font_height();
    const i32 gadget_h = dialog.line_height();
    const i32 right = dialog.width() - margin;
    const i32 box_w = static_cast<i32>(kCheckBoxWidth);
    const i32 box_h = static_cast<i32>(kCheckBoxHeight);
    const i32 row_h = std::max(box_h, font_h + 3);
    const i32 rows_top = dialog.top() - 4;
    const i32 box_left = right - box_w;
    const i32 box_labels_left = box_left - kGadgetLabelGap - kProtectionLabelChars * cell;
    const i32 value_left = margin + kLeftLabelChars * cell + kGadgetLabelGap;
    const i32 field_left = margin + kFieldLabelChars * cell + kGadgetLabelGap;
    const i32 field_width = right - field_left;
    const auto row_top = [&](i32 row) { return rows_top + row * row_h; };
    const auto text_line = [&](u16 id, i32 left, i32 row, i32 width, const char* label,
                               const char* text) {
        dialog.add(TEXT_KIND, id, left, row_top(row), std::max(width, cell), dialog.put(label),
                   PLACETEXT_LEFT, dialog.tags({GTTX_Text, dialog.put(text)}), row_h);
    };

    // The frame of the image, between the left block and the labels of
    // the check boxes, and the name in the middle above it. A long name
    // moves left to stay clear of the first check box's label.
    request.frame_left = value_left + (kValueChars + 1) * cell + 4;
    request.frame_top = rows_top + font_h + 3;
    request.frame_width = box_labels_left - 5 - request.frame_left;
    request.frame_height = row_top(5) - 2 - request.frame_top;
    const u32 font = intuition_->font_of_screen(screen_);
    const i32 name_span = static_cast<i32>(graphics_->text_width(
        font, reinterpret_cast<const u8*>(name_line.text), text_length(name_line.text)));
    const i32 name_limit =
        as_file ? box_left - kGadgetLabelGap -
                      (static_cast<i32>(text_length(kProtectionBoxes[0].label)) + 1) * cell
                : right;
    i32 name_left = request.frame_left + (request.frame_width - name_span) / 2;
    name_left = std::max(margin, std::min(name_left, name_limit - name_span));
    dialog.add(TEXT_KIND, kInfoName, name_left, rows_top,
               std::max(cell, std::min(name_span, name_limit - name_left)), 0, 0,
               dialog.tags({GTTX_Text, dialog.put(name_line.text)}), font_h + 2);

    const i32 value_width = kValueChars * cell;
    if (is_disk) {
        text_line(kInfoBlocks, value_left, 1, value_width, "Blocks:", blocks_line.text);
        text_line(kInfoUsed, value_left, 2, value_width, "Used:", second_line.text);
        text_line(kInfoFree, value_left, 3, value_width, "Free:", free_line.text);
        text_line(kInfoBlockSize, value_left, 4, value_width, "Block size:",
                  block_size_line.text);
    } else if (as_file) {
        text_line(kInfoBlocks, value_left, 1, value_width, "Blocks:", blocks_line.text);
        text_line(kInfoBytes, value_left, 2, value_width, "Bytes:", second_line.text);
    }
    if (show_stack) {
        // In the middle of the two rows above the date.
        request.stack_gadget = dialog.add(
            INTEGER_KIND, kInfoStack, value_left, row_top(3) + (2 * row_h - gadget_h) / 2,
            value_width, dialog.put("Stack:"), PLACETEXT_LEFT,
            dialog.tags({GTIN_Number, static_cast<u32>(stack), GTIN_MaxChars, 10}));
    }
    for (u32 i = 0; !is_disk && i < kProtectionBoxCount; ++i) {
        const ProtectionBox& box = kProtectionBoxes[i];
        if (!as_file && !box.on_drawer) {
            continue;
        }
        const bool set = (info.protection & box.bit) != 0;
        dialog.add(CHECKBOX_KIND, box.id, box_left,
                   row_top(static_cast<i32>(i)) + (row_h - box_h) / 2, 0,
                   dialog.put(box.label), PLACETEXT_LEFT,
                   dialog.tags({GTCB_Checked, has_object && box.denies != set ? 1u : 0u,
                                ndk::GA_Disabled, has_object ? 0u : 1u}));
    }
    // The date shares the last row with the Deletable box, or with
    // whether a disk can be written.
    i32 date_right = box_labels_left;
    if (is_disk) {
        const i32 status_left = right - kStatusChars * cell;
        text_line(kInfoStatus, status_left, 5, kStatusChars * cell, "Status:", status);
        date_right = status_left - kGadgetLabelGap - 7 * cell;
    }
    text_line(kInfoDate, field_left, 5, date_right - cell - field_left,
              is_disk ? "Created:" : "Last Changed:", date_line.text);

    i32 top = row_top(static_cast<i32>(kProtectionBoxCount)) + 3;
    if (!is_disk) {
        request.comment_gadget = dialog.add(
            STRING_KIND, kInfoComment, field_left, top, field_width, dialog.put("Comment:"),
            PLACETEXT_LEFT,
            dialog.tags({GTST_String, dialog.put(info.comment), GTST_MaxChars,
                         Dos::kMaxComment, ndk::GA_Disabled, has_object ? 0u : 1u}));
        top += gadget_h + kRowGap;
    }
    if (show_default_tool) {
        request.default_tool_gadget = dialog.add(
            STRING_KIND, kInfoDefaultTool, field_left, top, field_width,
            dialog.put("Default Tool:"), PLACETEXT_LEFT,
            dialog.tags({GTST_String, dialog.put(default_tool), GTST_MaxChars,
                         kMaxDefaultToolLength}));
        top += gadget_h + kRowGap;
    }
    if (show_tool_types) {
        // As many rows as the screen has room for under its bar, with the
        // string, the buttons and the bottom edge below them.
        const i32 item_h = font_h + 1;
        const i32 below = 2 * kListViewFrame + gadget_h + kRowGap + gadget_h + kBottom;
        const i32 rows = std::clamp((dialog.room_below_bar() - top - below) / item_h, 1,
                                    static_cast<i32>(kInfoToolTypeRows));
        const i32 list_h = rows * item_h + 2 * kListViewFrame;
        request.visible_rows = static_cast<u32>(rows);
        // The list view puts the string that edits the selected row under
        // its rows; New and Del stand left of it, and Del and the string
        // are ghosted until a row is selected.
        request.tool_gadget = dialog.add(
            STRING_KIND, kInfoTool, field_left, top + list_h, field_width, 0, 0,
            dialog.tags({GTST_String, request.empty_text, GTST_MaxChars, kMaxToolTypeLength,
                         ndk::GA_Disabled, 1}));
        request.tool_types_gadget = dialog.add(
            LISTVIEW_KIND, kInfoToolTypes, field_left, top, field_width,
            dialog.put("Tool Types:"), PLACETEXT_LEFT,
            dialog.tags({GTLV_Labels, request.list, GTLV_ShowSelected, request.tool_gadget,
                         GTLV_ItemHeight, static_cast<u32>(item_h)}),
            list_h + gadget_h);
        const i32 button = (kFieldLabelChars * cell + 2) / 2;
        dialog.add(BUTTON_KIND, kInfoNew, margin, top + list_h, button, dialog.put("New"),
                   PLACETEXT_IN);
        request.delete_gadget = dialog.add(
            BUTTON_KIND, kInfoDel, margin + button + 2, top + list_h, button,
            dialog.put("Del"), PLACETEXT_IN, dialog.tags({ndk::GA_Disabled, 1}));
        top += list_h + gadget_h + kRowGap;
    }
    const u32 save_at = dialog.put("Save");
    const u32 cancel_at = dialog.put("Cancel");
    dialog.buttons(top, save_at, cancel_at, 8);
    if (!dialog.open(dialog.put(drawer), top + gadget_h, kBottom)) {
        LogLine().text("workbench: could not show information for ").text(icon->name).send();
        close_dialog(request);
        return false;
    }
    draw_info_image(request, icon->object, extra + kInfoClipAt);
    return true;
}

// The frame is recessed: dark along the top and left, light along the
// bottom and right.
void Workbench::draw_info_image(const InfoRequest& request, u32 object, u32 clip) {
    const i32 inner_width = request.frame_width - 2;
    const i32 inner_height = request.frame_height - 2;
    if (inner_width <= 0 || inner_height <= 0) {
        return;
    }
    Bus b(*machine_);
    const u32 rp = b.r32(request.window + ndk::wd_RPort);
    const i32 x0 = request.frame_left;
    const i32 y0 = request.frame_top;
    const i32 x1 = x0 + request.frame_width - 1;
    const i32 y1 = y0 + request.frame_height - 1;
    graphics_->set_pens(rp, kFrameShadowPen, 0, ndk::JAM1);
    graphics_->rect_fill(rp, x0, y0, x1 - 1, y0);
    graphics_->rect_fill(rp, x0, y0, x0, y1 - 1);
    graphics_->set_pens(rp, kFrameShinePen, 0, ndk::JAM1);
    graphics_->rect_fill(rp, x0 + 1, y1, x1, y1);
    graphics_->rect_fill(rp, x1, y0 + 1, x1, y1);
    const IconArt art = art_of(b, object);
    if (art.render == 0) {
        return;
    }
    // The Region's box is the inside of the frame; its one rectangle is
    // relative to the box.
    const u32 rect = clip + ndk::Region_SIZEOF;
    b.w16(clip + ndk::rg_bounds, static_cast<u32>(x0 + 1));
    b.w16(clip + ndk::rg_bounds + 2, static_cast<u32>(y0 + 1));
    b.w16(clip + ndk::rg_bounds + 4, static_cast<u32>(x1 - 1));
    b.w16(clip + ndk::rg_bounds + 6, static_cast<u32>(y1 - 1));
    b.w32(clip + ndk::rg_RegionRectangle, rect);
    b.w32(rect + ndk::rr_Next, 0);
    b.w32(rect + ndk::rr_Prev, 0);
    b.w16(rect + ndk::rr_bounds, 0);
    b.w16(rect + ndk::rr_bounds + 2, 0);
    b.w16(rect + ndk::rr_bounds + 4, static_cast<u32>(inner_width - 1));
    b.w16(rect + ndk::rr_bounds + 6, static_cast<u32>(inner_height - 1));
    const InnerClip inside(*machine_, rp, clip);
    intuition_->draw_image(rp, art.render,
                           x0 + 1 + (inner_width - art.width) / 2 - art.left,
                           y0 + 1 + (inner_height - art.height) / 2 - art.top);
}

// Makes the exec List of the rows anew.
void Workbench::link_tool_types(const InfoRequest& request) {
    Bus b(*machine_);
    b.w32(request.list + ndk::LH_HEAD, request.list + ndk::LH_TAIL);
    b.w32(request.list + ndk::LH_TAIL, 0);
    b.w32(request.list + ndk::LH_TAILPRED, request.list + ndk::LH_HEAD);
    for (u32 i = 0; i < request.row_count; ++i) {
        const u32 node = request.list + ndk::LH_SIZE + i * ndk::LN_SIZE;
        const u32 last = b.r32(request.list + ndk::LH_TAILPRED);
        b.w32(node + ndk::LN_SUCC, request.list + ndk::LH_TAIL);
        b.w32(node + ndk::LN_PRED, last);
        b.w32(last + ndk::LN_SUCC, node);
        b.w32(request.list + ndk::LH_TAILPRED, node);
        b.w32(node + ndk::LN_NAME, request.rows + i * kInfoRowStride);
    }
}

// The list scrolls the selected row into view and puts its text in the
// string gadget.
void Workbench::show_tool_types(InfoRequest& request, u32 selected) {
    using namespace gadtools_defs;
    Bus b(*machine_);
    link_tool_types(request);
    if (selected == kNoToolType) {
        gadtools_->set_gadget_attrs(
            request.tool_gadget, request.window,
            write_tags(b, request.tags, {GTST_String, request.empty_text}));
    }
    // The tags stop before GTLV_MakeVisible when no row is selected.
    const u32 tags = write_tags(b, request.tags,
                                {GTLV_Labels, request.list, GTLV_Selected, selected,
                                 selected != kNoToolType ? GTLV_MakeVisible : ndk::TAG_DONE,
                                 selected});
    gadtools_->set_gadget_attrs(request.tool_types_gadget, request.window, tags);
    request.selected = selected;
    set_tool_type_editing(request, selected != kNoToolType);
}

void Workbench::set_tool_type_editing(InfoRequest& request, bool editing) {
    if (request.editing == editing) {
        return;
    }
    Bus b(*machine_);
    for (const u32 gadget : {request.tool_gadget, request.delete_gadget}) {
        gadtools_->set_gadget_attrs(
            gadget, request.window,
            write_tags(b, request.tags, {ndk::GA_Disabled, editing ? 0u : 1u}));
    }
    request.editing = editing;
}

void Workbench::add_tool_type(InfoRequest& request) {
    if (request.row_count >= kMaxToolTypes) {
        return;
    }
    Bus b(*machine_);
    b.w8(request.rows + request.row_count * kInfoRowStride, 0);
    request.origin[request.row_count] = kNewToolType;
    ++request.row_count;
    show_tool_types(request, request.row_count - 1);
    intuition_->activate_string(request.tool_gadget, request.window);
}

void Workbench::delete_tool_type(InfoRequest& request) {
    const u32 removed = request.selected;
    if (removed >= request.row_count) {
        return;
    }
    Bus b(*machine_);
    const u32 later = request.row_count - removed - 1;
    u8* rows = b.bytes_at(request.rows + removed * kInfoRowStride,
                          (later + 1) * kInfoRowStride);
    if (rows == nullptr) {
        return;
    }
    __builtin_memmove(rows, rows + kInfoRowStride, later * kInfoRowStride);
    __builtin_memmove(request.origin + removed, request.origin + removed + 1, later);
    --request.row_count;
    show_tool_types(request, request.row_count == 0 ? kNoToolType
                             : later != 0          ? removed
                                                   : removed - 1);
}

// A row that shows only the start of a longer tool type stays that tool
// type until its text is changed.
void Workbench::commit_tool_type(InfoRequest& request) {
    if (request.selected >= request.row_count) {
        return;
    }
    Bus b(*machine_);
    const u32 string_info = b.r32(request.tool_gadget + ndk::gg_SpecialInfo);
    char text[kMaxToolTypeLength + 1]{};
    const u32 row = request.rows + request.selected * kInfoRowStride;
    if (b.string(b.r32(string_info + ndk::si_Buffer), text, sizeof text) &&
        !same_guest_prefix(b, row, text, kMaxToolTypeLength)) {
        copy_guest_text(b, row, b.r32(string_info + ndk::si_Buffer), kMaxToolTypeLength);
        request.origin[request.selected] = kNewToolType;
    }
}

// Rows added and left empty are no tool types.
void Workbench::drop_empty_tool_types(InfoRequest& request) {
    Bus b(*machine_);
    for (u32 i = request.row_count; i > 0; --i) {
        if (request.origin[i - 1] == kNewToolType &&
            b.r8(request.rows + (i - 1) * kInfoRowStride) == 0) {
            request.selected = i - 1;
            delete_tool_type(request);
        }
    }
}

// Return in the string gadget replaces the selected row's text.
void Workbench::edit_tool_type(InfoRequest& request) {
    commit_tool_type(request);
    if (request.selected < request.row_count) {
        show_tool_types(request, request.selected);
    }
}

Workbench::Reply Workbench::info_input(InfoRequest& request) {
    return dialog_input(request, [&](u32 id, u32 code) {
        if (id == kInfoSave) {
            return Reply::Accepted;
        }
        if (id == kInfoToolTypes && code < request.row_count) {
            request.selected = code;
            set_tool_type_editing(request, true);
            intuition_->activate_string(request.tool_gadget, request.window);
        } else if (id == kInfoNew) {
            add_tool_type(request);
        } else if (id == kInfoDel) {
            delete_tool_type(request);
        } else if (id == kInfoTool && code == '\r') {
            edit_tool_type(request);
        }
        return Reply::Waiting;
    });
}

// Writes the stack, default tool and tool types the window changed into the
// .info. The rows and strings set on the object are this block's own, and
// the object's pointers go back before it is freed, so it frees only its
// own memory.
bool Workbench::save_icon_info(InfoRequest& request, bool& changed, u32& error) {
    changed = false;
    error = 0;
    Bus b(*machine_);
    const u32 stack_info =
        request.stack_gadget != 0 ? b.r32(request.stack_gadget + ndk::gg_SpecialInfo) : 0;
    const i32 stack =
        stack_info != 0 ? static_cast<i32>(b.r32(stack_info + ndk::si_LongInt)) : 0;
    char default_tool[kMaxDefaultToolLength + 1]{};
    if (request.show_default_tool) {
        const u32 string_info = b.r32(request.default_tool_gadget + ndk::gg_SpecialInfo);
        if (string_info == 0 ||
            !b.string(b.r32(string_info + ndk::si_Buffer), default_tool, sizeof default_tool)) {
            error = ERROR_OBJECT_WRONG_TYPE;
            return false;
        }
    }
    const bool stack_changed =
        request.show_stack && static_cast<u32>(stack) != request.original_stack;
    const bool default_changed =
        request.show_default_tool &&
        !same_guest_prefix(b, request.original_default, default_tool, kMaxDefaultToolLength);
    if (request.show_tool_types) {
        // The string gadget may hold a row Return was not pressed in.
        commit_tool_type(request);
        drop_empty_tool_types(request);
    }
    bool types_changed = request.show_tool_types &&
                         request.row_count != request.original_row_count;
    for (u32 i = 0; request.show_tool_types && !types_changed && i < request.row_count; ++i) {
        types_changed = request.origin[i] != i;
    }
    if (!stack_changed && !default_changed && !types_changed) {
        return true;
    }

    u32 object = icon_->load_object(request.path);
    if (object == 0 && request.is_disk) {
        // A disk without a Disk.info gets one of the shape it shows.
        object = request.drive == kNoDrive ? icon_->ram_disk_object()
                                           : icon_->default_object(kDisk);
    }
    if (object == 0) {
        error = dos_->last_error();
        if (error == 0) {
            error = ERROR_OBJECT_WRONG_TYPE;
        }
        return false;
    }
    const u32 object_default = b.r32(object + do_DefaultTool);
    const u32 object_types = b.r32(object + do_ToolTypes);
    u32 block = 0;
    u32 block_bytes = 0;
    if (types_changed) {
        // Rows past the window's cap stay as the .info has them.
        u32 original_count = 0;
        while (object_types != 0 && original_count < kMaxToolTypeTail &&
               b.r32(object_types + original_count * 4) != 0) {
            ++original_count;
        }
        const u32 tail = original_count > kMaxToolTypes ? original_count - kMaxToolTypes : 0;
        const u32 count = request.row_count + tail;
        if (count != 0) {
            block_bytes = (count + 1) * 4;
            block = exec_->alloc(block_bytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
            if (block == 0) {
                error = ERROR_NO_FREE_STORE;
                icon_->free_object(object);
                return false;
            }
            // A row that was not changed keeps the object's own string,
            // which may be longer than the row.
            for (u32 i = 0; i < request.row_count; ++i) {
                const u32 origin = request.origin[i];
                b.w32(block + i * 4, origin < original_count
                                         ? b.r32(object_types + origin * 4)
                                         : request.rows + i * kInfoRowStride);
            }
            for (u32 i = 0; i < tail; ++i) {
                b.w32(block + (request.row_count + i) * 4,
                      b.r32(object_types + (kMaxToolTypes + i) * 4));
            }
        }
        b.w32(object + do_ToolTypes, block);
    }
    if (stack_changed) {
        b.w32(object + do_StackSize, static_cast<u32>(stack));
    }
    if (default_changed) {
        u32 length = 0;
        for (; default_tool[length] != '\0'; ++length) {
            b.w8(request.default_text + length, static_cast<u8>(default_tool[length]));
        }
        b.w8(request.default_text + length, 0);
        b.w32(object + do_DefaultTool,
              default_tool[0] != '\0' ? request.default_text : 0);
    }
    const bool saved = b.ok() && icon_->save_object(request.path, object, error);
    b.w32(object + do_DefaultTool, object_default);
    b.w32(object + do_ToolTypes, object_types);
    icon_->free_object(object);
    if (block != 0) {
        exec_->free(block, block_bytes);
    }
    if (!saved) {
        if (error == 0) {
            error = ERROR_OBJECT_WRONG_TYPE;
        }
        return false;
    }
    request.original_stack = static_cast<u32>(stack);
    request.original_row_count = request.row_count;
    for (u32 i = 0; i < request.row_count; ++i) {
        request.origin[i] = static_cast<u8>(i);
    }
    copy_guest_text(b, request.original_default, request.default_text, kMaxDefaultToolLength);
    changed = true;
    return true;
}

bool Workbench::save_info(InfoRequest& request) {
    if (machine_ == nullptr || dos_ == nullptr || icon_ == nullptr) {
        return false;
    }
    Bus b(*machine_);
    u32 error = 0;
    bool changed = false;
    if (request.has_object && request.comment_gadget != 0) {
        const auto box_gadget = [&](u16 id) {
            for (u32 gadget = request.gadgets, i = 0; gadget != 0 && i < 64;
                 ++i, gadget = b.r32(gadget + ndk::gg_NextGadget)) {
                if (b.r16(gadget + ndk::gg_GadgetID) == id) {
                    return gadget;
                }
            }
            return u32{0};
        };
        // A bit without a check box stays as the object has it.
        u32 protection = request.protection;
        for (const ProtectionBox& box : kProtectionBoxes) {
            const u32 gadget = box_gadget(box.id);
            if (gadget == 0) {
                continue;
            }
            const bool allowed = (b.r16(gadget + ndk::gg_Flags) & ndk::GFLG_SELECTED) != 0;
            const bool set = box.denies ? !allowed : allowed;
            protection = set ? protection | box.bit : protection & ~box.bit;
        }
        char comment[Dos::kMaxComment + 1]{};
        const u32 string_info = b.r32(request.comment_gadget + ndk::gg_SpecialInfo);
        if (!b.ok() || !b.string(b.r32(string_info + ndk::si_Buffer), comment, sizeof comment)) {
            return false;
        }
        if (protection != request.protection) {
            if (!dos_->set_protection_bits(request.path, protection, error)) {
                show_error("cannot save", request.name, error);
                return false;
            }
            request.protection = protection;
            changed = true;
        }
        if (!same_text(comment, request.comment_text)) {
            if (!dos_->set_file_comment(request.path, comment, error)) {
                show_error("cannot save", request.name, error);
                return false;
            }
            copy_text(request.comment_text, sizeof request.comment_text, comment);
            changed = true;
        }
    }
    bool info_changed = false;
    if ((request.show_stack || request.show_default_tool || request.show_tool_types) &&
        !save_icon_info(request, info_changed, error)) {
        show_error("cannot save", request.name, error);
        return false;
    }
    if (info_changed) {
        if (request.is_disk) {
            reload_disk_icon(request);
        } else if (DrawerWindow* window = find_window(request.window_id); window != nullptr) {
            update_window(*window);
        }
        changed = true;
    }
    if (changed) {
        LogLine().text("workbench: saved ").text(request.path).send();
    }
    return true;
}

void Workbench::reload_disk_icon(const InfoRequest& request) {
    IconEntry* icon = find_icon(request.icon_id);
    if (icon == nullptr || icon->owner != kRootWindow || icon->type != kDisk) {
        return;
    }
    const u32 fresh = icon_->load_object(request.path);
    if (fresh == 0) {
        return;
    }
    if (icon->owned && icon->object != 0) {
        icon_->free_object(icon->object);
    }
    icon->object = fresh;
    icon->owned = true;
}

void Workbench::show_information(u32 window_id, u32 icon_id) {
    InfoRequest request{};
    if (!running_ || !open_info(window_id, icon_id, request)) {
        return;
    }
    if (wait_for_reply(request, [&] { return info_input(request); }) == Reply::Accepted) {
        save_info(request);
    }
    close_dialog(request);
}

// --- file operations -----------------------------------------------------

const Workbench::IconEntry* Workbench::file_icon(u32 window_id, u32 icon_id) const {
    const IconEntry* icon = find_icon(icon_id);
    const bool file = find_window(window_id) != nullptr && icon != nullptr &&
                      icon->owner == window_id && window_id != kRootWindow &&
                      icon->type != kDisk && icon->type != kAppIcon;
    return file ? icon : nullptr;
}

bool Workbench::collect_tree(const char* path, std::vector<TreeEntry>& out, u32& error) {
    out.clear();
    error = 0;
    const Dos::TargetKind kind = dos_->examine(path).kind;
    if (kind == Dos::TargetKind::Missing) {
        return true;
    }
    TreeEntry root{};
    copy_text(root.path, sizeof root.path, path);
    root.is_dir = kind == Dos::TargetKind::Dir;
    out.push_back(root);
    // The list grows as its drawers are read, so no call nests per level.
    struct Found {
        std::vector<TreeEntry>* out;
        char parent[Dos::kMaxPath];
        bool fits;
    } found{&out, {}, true};
    for (usize at = 0; at < out.size(); ++at) {
        if (!out[at].is_dir) {
            continue;
        }
        copy_text(found.parent, sizeof found.parent, out[at].path);
        const auto add = [](void* ctx, const char* name, bool is_dir) {
            Found& f = *static_cast<Found*>(ctx);
            TreeEntry entry{};
            entry.is_dir = is_dir;
            if (join_path(f.parent, name, entry.path)) {
                f.out->push_back(entry);
            } else {
                f.fits = false;
            }
        };
        if (!dos_->list_dir(found.parent, add, &found, error)) {
            return false;
        }
        if (!found.fits) {
            error = ERROR_INVALID_COMPONENT_NAME;
            return false;
        }
        if (out.size() > kMaxTreeEntries) {
            error = ERROR_NO_FREE_STORE;
            return false;
        }
    }
    return true;
}

void Workbench::close_windows_below(const char* path) {
    std::vector<u32> below;
    for (const DrawerWindow& w : windows_) {
        if (w.id != kRootWindow && at_or_below(w.path, path)) {
            below.push_back(w.id);
        }
    }
    for (const u32 id : below) {
        close_drawer(id);
    }
}

bool Workbench::delete_tree(const char* path, u32& error) {
    std::vector<TreeEntry> tree;
    bool done = collect_tree(path, tree, error);
    // What is in a drawer goes before the drawer.
    for (usize i = tree.size(); done && i > 0; --i) {
        done = dos_->delete_object(tree[i - 1].path, error);
    }
    return done;
}

bool Workbench::copy_file(const char* from, const char* to, u32& error) {
    const Dos::Target source = dos_->examine(from);
    if (source.kind != Dos::TargetKind::File) {
        error = ERROR_OBJECT_NOT_FOUND;
        return false;
    }
    if (source.size == 0) {
        return dos_->write_file(to, nullptr, 0, error);
    }
    // The bytes go through exec memory: the task's stack is small.
    const u32 block = exec_->alloc(source.size, ndk::MEMF_PUBLIC);
    if (block == 0) {
        error = ERROR_NO_FREE_STORE;
        return false;
    }
    Bus b(*machine_);
    u8* data = b.bytes_at(block, source.size);
    u32 got = 0;
    const bool copied = data != nullptr &&
                        dos_->read_file(from, data, source.size, got, error) &&
                        dos_->write_file(to, data, got, error);
    exec_->free(block, source.size);
    return copied;
}

// The position is written over in the bytes of the .info, as writing the
// icon anew would drop what follows the parts the icon library knows.
void Workbench::clear_icon_position(const char* info) {
    const Dos::Target file = dos_->examine(info);
    if (file.kind != Dos::TargetKind::File || file.size < do_CurrentY + 4) {
        return;
    }
    std::vector<u8> bytes(file.size);
    u32 got = 0;
    u32 error = 0;
    if (!dos_->read_file(info, bytes.data(), file.size, got, error) || bytes[0] != 0xE3 ||
        bytes[1] != 0x10) {
        return;
    }
    for (u32 i = 0; i < 4; ++i) {
        const u8 byte = static_cast<u8>(kNoIconPosition >> (24 - 8 * i));
        bytes[do_CurrentX + i] = byte;
        bytes[do_CurrentY + i] = byte;
    }
    if (!dos_->replace_file(info, bytes.data(), got, error)) {
        LogLine().text("workbench: the copy keeps its place, ").text(info).send();
    }
}

// Gives to the comment and protection of from, the comment first as the
// protection may forbid it.
void Workbench::copy_attributes(const char* from, const char* to) {
    const Dos::FileInfo info = dos_->file_info(from);
    if (info.kind == Dos::TargetKind::Missing) {
        return;
    }
    u32 error = 0;
    if (info.comment[0] != '\0') {
        dos_->set_file_comment(to, info.comment, error);
    }
    dos_->set_protection_bits(to, info.protection, error);
}

bool Workbench::duplicate(const char* from, const char* to, u32& error) {
    std::vector<TreeEntry> tree;
    if (!collect_tree(from, tree, error)) {
        return false;
    }
    const u32 length = text_length(from);
    // The same path below to as the entry has below from.
    const auto target_of = [&](const TreeEntry& entry, char (&out)[Dos::kMaxPath]) {
        const char* below = entry.path + length;
        if (*below == '\0') {
            copy_text(out, sizeof out, to);
            return true;
        }
        return join_path(to, *below == '/' ? below + 1 : below, out);
    };
    char target[Dos::kMaxPath];
    for (const TreeEntry& entry : tree) {
        if (!target_of(entry, target)) {
            error = ERROR_INVALID_COMPONENT_NAME;
            return false;
        }
        if (entry.is_dir ? !dos_->make_dir(target, error)
                         : !copy_file(entry.path, target, error)) {
            return false;
        }
    }
    char from_info[Dos::kMaxPath];
    char to_info[Dos::kMaxPath];
    if (!info_path(from, from_info) || !info_path(to, to_info)) {
        error = ERROR_INVALID_COMPONENT_NAME;
        return false;
    }
    if (dos_->examine(from_info).kind != Dos::TargetKind::Missing) {
        if (!copy_file(from_info, to_info, error)) {
            return false;
        }
        // The copy takes a free place, not the one of what it copies.
        clear_icon_position(to_info);
        copy_attributes(from_info, to_info);
    }
    // Last, so that a copy that fails can still be deleted.
    for (const TreeEntry& entry : tree) {
        if (target_of(entry, target)) {
            copy_attributes(entry.path, target);
        }
    }
    return true;
}

bool Workbench::copy_icon(u32 window_id, u32 icon_id) {
    const IconEntry* icon = file_icon(window_id, icon_id);
    if (icon == nullptr) {
        const IconEntry* other = find_icon(icon_id);
        show_error("cannot copy", other != nullptr ? other->name : "?", ERROR_OBJECT_WRONG_TYPE);
        return false;
    }
    char name[32];
    char file[32];
    copy_text(name, sizeof name, icon->name);
    copy_text(file, sizeof file, icon->file);
    const DrawerWindow& window = *find_window(window_id);
    char from[Dos::kMaxPath];
    char to[Dos::kMaxPath];
    char info[Dos::kMaxPath];
    u32 error = ERROR_INVALID_COMPONENT_NAME;
    bool done = icon_path(window, *icon, from);
    for (u32 number = 1; done; ++number) {
        Message copy;
        copy.add("copy_");
        if (number > 1) {
            copy.number(number).add("_");
        }
        copy.add("of_").add(file);
        // Room stays for the ".info".
        copy.text[kMaxName - (sizeof ".info" - 1)] = '\0';
        done = join_path(window.path, copy.text, to) && info_path(to, info);
        if (done && dos_->examine(to).kind == Dos::TargetKind::Missing &&
            dos_->examine(info).kind == Dos::TargetKind::Missing) {
            break;
        }
    }
    if (done && !duplicate(from, to, error)) {
        done = false;
        u32 ignored = 0;
        delete_tree(to, ignored);
        dos_->delete_object(info, ignored);
    }
    if (done) {
        LogLine().text("workbench: copied ").text(from).text(" to ").text(to).send();
    } else {
        show_error("cannot copy", name, error);
    }
    if (DrawerWindow* w = find_window(window_id); w != nullptr) {
        update_window(*w);
    }
    return done;
}

bool Workbench::delete_icon(u32 window_id, u32 icon_id) {
    const IconEntry* icon = file_icon(window_id, icon_id);
    if (icon == nullptr) {
        const IconEntry* other = find_icon(icon_id);
        show_error("cannot delete", other != nullptr ? other->name : "?",
                   ERROR_OBJECT_WRONG_TYPE);
        return false;
    }
    char path[Dos::kMaxPath];
    char info[Dos::kMaxPath];
    char name[32];
    copy_text(name, sizeof name, icon->name);
    u32 error = ERROR_INVALID_COMPONENT_NAME;
    bool done = icon_path(*find_window(window_id), *icon, path) && info_path(path, info);
    if (done) {
        close_windows_below(path);
        done = delete_tree(path, error);
    }
    if (done && dos_->examine(info).kind != Dos::TargetKind::Missing) {
        done = dos_->delete_object(info, error);
    }
    if (done) {
        move_left_outs(path, nullptr);
        LogLine().text("workbench: deleted ").text(path).send();
    } else {
        show_error("cannot delete", name, error);
    }
    if (DrawerWindow* window = find_window(window_id); window != nullptr) {
        update_window(*window);
    }
    return done;
}

void Workbench::ask_delete(u32 window_id, u32 icon_id) {
    const IconEntry* icon = file_icon(window_id, icon_id);
    char path[Dos::kMaxPath];
    std::vector<TreeEntry> tree;
    u32 error = ERROR_INVALID_COMPONENT_NAME;
    if (icon == nullptr) {
        // Refused, and shown, there.
        delete_icon(window_id, icon_id);
        return;
    }
    // Nothing is deleted that could not be asked about.
    if (!icon_path(*find_window(window_id), *icon, path) || !collect_tree(path, tree, error)) {
        show_error("cannot delete", icon->name, error);
        return;
    }
    Message message;
    message.add("Warning: you cannot get back\nwhat you delete!\nOK to delete ").add(icon->name);
    if (tree.size() > 1) {
        message.add("\nand the ").number(tree.size() - 1).add(" objects in it");
    }
    message.add("?");
    if (confirm("Delete", message.text)) {
        delete_icon(window_id, icon_id);
    }
}

bool Workbench::rename_with_info(const char* from, const char* to, u32& error) {
    char from_info[Dos::kMaxPath];
    char to_info[Dos::kMaxPath];
    if (!info_path(from, from_info) || !info_path(to, to_info)) {
        error = ERROR_INVALID_COMPONENT_NAME;
        return false;
    }
    // An icon may stand for nothing but its .info.
    const bool object = dos_->examine(from).kind != Dos::TargetKind::Missing;
    if (object && !dos_->rename_path(from, to, error)) {
        return false;
    }
    if (dos_->examine(from_info).kind == Dos::TargetKind::Missing ||
        dos_->rename_path(from_info, to_info, error)) {
        return true;
    }
    u32 ignored = 0;
    if (object) {
        dos_->rename_path(to, from, ignored);
    }
    return false;
}

bool Workbench::rename_icon(u32 window_id, u32 icon_id, const char* new_name) {
    const IconEntry* icon = file_icon(window_id, icon_id);
    if (icon == nullptr) {
        const IconEntry* other = find_icon(icon_id);
        show_error("cannot rename", other != nullptr ? other->name : "?",
                   ERROR_OBJECT_WRONG_TYPE);
        return false;
    }
    char name[32];
    copy_text(name, sizeof name, icon->name);
    char from[Dos::kMaxPath];
    char to[Dos::kMaxPath];
    const DrawerWindow& window = *find_window(window_id);
    u32 error = ERROR_INVALID_COMPONENT_NAME;
    bool done = is_file_name(new_name) && icon_path(window, *icon, from) &&
                join_path(window.path, new_name, to);
    if (done) {
        close_windows_below(from);
        done = rename_with_info(from, to, error);
    }
    if (done) {
        move_left_outs(from, to);
        LogLine().text("workbench: renamed ").text(from).text(" to ").text(new_name).send();
    } else {
        show_error("cannot rename", name, error);
    }
    if (DrawerWindow* w = find_window(window_id); w != nullptr) {
        update_window(*w);
    }
    return done;
}

void Workbench::ask_rename(u32 window_id, u32 icon_id) {
    const IconEntry* icon = file_icon(window_id, icon_id);
    if (icon == nullptr) {
        rename_icon(window_id, icon_id, "");
        return;
    }
    char old_name[32];
    char new_name[kMaxName + 1];
    copy_text(old_name, sizeof old_name, icon->file);
    if (ask_text("Rename", "Enter a new name", old_name, new_name, sizeof new_name) &&
        !same_text(old_name, new_name)) {
        rename_icon(window_id, icon_id, new_name);
    }
}

bool Workbench::new_drawer(u32 window_id, const char* name) {
    const DrawerWindow* window = window_id != kRootWindow ? find_window(window_id) : nullptr;
    if (window == nullptr) {
        show_error("cannot make", name != nullptr ? name : "?", ERROR_OBJECT_WRONG_TYPE);
        return false;
    }
    char path[Dos::kMaxPath];
    char info[Dos::kMaxPath];
    u32 error = ERROR_INVALID_COMPONENT_NAME;
    bool done = is_file_name(name) && join_path(window->path, name, path) && info_path(path, info);
    // An icon that is there already, with no drawer, is not written over.
    if (done && dos_->examine(info).kind != Dos::TargetKind::Missing) {
        error = ERROR_OBJECT_EXISTS;
        done = false;
    }
    done = done && dos_->make_dir(path, error);
    if (done) {
        const u32 object = icon_->default_object(kDrawer);
        error = ERROR_NO_FREE_STORE;
        done = object != 0 && icon_->save_object(path, object, error);
        if (object != 0) {
            icon_->free_object(object);
        }
        if (!done) {
            u32 ignored = 0;
            dos_->delete_object(path, ignored);
        }
    }
    if (done) {
        LogLine().text("workbench: made ").text(path).send();
    } else {
        show_error("cannot make", name != nullptr ? name : "?", error);
    }
    if (DrawerWindow* w = find_window(window_id); w != nullptr) {
        update_window(*w);
    }
    return done;
}

void Workbench::ask_new_drawer(u32 window_id) {
    const DrawerWindow* window = find_window(window_id);
    if (window == nullptr || window_id == kRootWindow) {
        return;
    }
    // The first "Unnamed" number that neither a file nor an icon has.
    char suggested[kMaxName + 1];
    for (u32 number = 1;; ++number) {
        Message candidate;
        candidate.add("Unnamed").number(number);
        copy_text(suggested, sizeof suggested, candidate.text);
        char path[Dos::kMaxPath];
        char info[Dos::kMaxPath];
        if (!join_path(window->path, suggested, path) || !info_path(path, info) ||
            (dos_->examine(path).kind == Dos::TargetKind::Missing &&
             dos_->examine(info).kind == Dos::TargetKind::Missing)) {
            break;
        }
    }
    char name[kMaxName + 1];
    if (ask_text("New Drawer", "Enter the name of the drawer", suggested, name, sizeof name)) {
        new_drawer(window_id, name);
    }
}

bool Workbench::execute_command(const char* command) {
    if (command == nullptr || command[0] == '\0') {
        return false;
    }
    if (run_command_ == nullptr || !run_command_(run_command_ctx_, command)) {
        show_error("cannot run", command, ERROR_NO_FREE_STORE);
        return false;
    }
    LogLine().text("workbench: running ").text(command).send();
    return true;
}

void Workbench::ask_command() {
    char command[kMaxCommand];
    if (ask_text("Execute a File", "Enter command and its arguments", last_command_, command,
                 sizeof command)) {
        copy_text(last_command_, sizeof last_command_, command);
        execute_command(command);
    }
}

// --- left out icons ------------------------------------------------------

const Workbench::LeftOut* Workbench::left_out(u32 icon_id) const {
    for (const LeftOut& out : left_out_) {
        if (out.icon == icon_id) {
            return &out;
        }
    }
    return nullptr;
}

const char* Workbench::drawer_of(const DrawerWindow& window, const IconEntry& icon) const {
    const LeftOut* out = left_out(icon.id);
    return out != nullptr ? out->drawer : window.path;
}

void Workbench::remove_root_icon(u32 icon_id) {
    IconEntry* icon = find_icon(icon_id);
    if (icon == nullptr) {
        return;
    }
    if (drag_.armed && drag_.icon == icon_id) {
        cancel_drag();
    }
    if (DrawerWindow* root = find_window(kRootWindow);
        root != nullptr && root->clicks.selected() == icon_id) {
        root->clicks.clear();
    }
    if (icon->owned && icon->object != 0) {
        icon_->free_object(icon->object);
    }
    std::erase_if(left_out_, [&](const LeftOut& out) { return out.icon == icon_id; });
    icons_.remove(icon);
}

bool Workbench::backdrop_entry(const char* path, char (&file)[Dos::kMaxPath],
                               char (&line)[Dos::kMaxPath]) const {
    u32 colon = 0;
    while (path[colon] != '\0' && path[colon] != ':') {
        ++colon;
    }
    if (path[colon] != ':' || colon + sizeof kBackdropFile + 1 > Dos::kMaxPath) {
        return false;
    }
    copy_text(file, sizeof file, path);
    copy_text(file + colon + 1, sizeof kBackdropFile, kBackdropFile);
    copy_text(line, sizeof line, path + colon);
    return true;
}

bool Workbench::set_backdrop_line(const char* file, const char* line, bool listed, u32& error) {
    const Dos::Target target = dos_->examine(file);
    const u32 size = target.kind == Dos::TargetKind::File ? target.size : 0;
    if (size > kMaxBackdropBytes) {
        error = ERROR_OBJECT_TOO_LARGE;
        return false;
    }
    std::vector<u8> old(size);
    u32 got = 0;
    if (size != 0 && !dos_->read_file(file, old.data(), size, got, error)) {
        return false;
    }
    // Every other line stays as it is.
    std::vector<u8> text;
    const u32 length = text_length(line);
    for (u32 start = 0, end = 0; start < got; start = end + 1) {
        for (end = start; end < got && old[end] != '\n'; ++end) {
        }
        u32 same = 0;
        while (same < length && start + same < end &&
               upper_ascii(static_cast<char>(old[start + same])) == upper_ascii(line[same])) {
            ++same;
        }
        if (same != length || start + same != end) {
            text.insert(text.end(), old.begin() + start, old.begin() + end);
            text.push_back('\n');
        }
    }
    if (listed) {
        text.insert(text.end(), line, line + length);
        text.push_back('\n');
    }
    if (text.empty()) {
        return size == 0 && target.kind == Dos::TargetKind::Missing
                   ? true
                   : dos_->delete_file(file, error);
    }
    return dos_->replace_file(file, text.data(), static_cast<u32>(text.size()), error);
}

bool Workbench::add_left_out(const char* drawer, const char* file) {
    const DrawerWindow* root = find_window(kRootWindow);
    char path[Dos::kMaxPath];
    if (root == nullptr || !join_path(drawer, file, path)) {
        return false;
    }
    for (const LeftOut& out : left_out_) {
        const IconEntry* icon = find_icon(out.icon);
        if (icon != nullptr && same_ci(out.drawer, drawer) && same_ci(icon->file, file)) {
            return true;
        }
    }
    const u32 object = icon_->load_object(path);
    if (object == 0) {
        return false;
    }
    IconEntry* icon = take_icon(kRootWindow);
    if (icon == nullptr) {
        icon_->free_object(object);
        LogLine().text("workbench: icon table full").send();
        return false;
    }
    Bus b(*machine_);
    icon->object = object;
    icon->owned = true;
    icon->type = static_cast<u8>(b.r8(object + do_Type));
    copy_text(icon->name, sizeof icon->name, file);
    copy_text(icon->file, sizeof icon->file, file);
    LeftOut out;
    out.icon = icon->id;
    copy_text(out.drawer, sizeof out.drawer, drawer);
    left_out_.push_back(out);
    measure(*icon);
    place_root_icon(*root, *icon);
    LogLine().text("workbench: left out ").text(path).send();
    return true;
}

void Workbench::add_left_outs(const char* volume) {
    char list[Dos::kMaxPath];
    if (!join_path(volume, kBackdropFile, list)) {
        return;
    }
    const Dos::Target target = dos_->examine(list);
    if (target.kind != Dos::TargetKind::File || target.size > kMaxBackdropBytes) {
        return;
    }
    std::vector<u8> text(target.size);
    u32 got = 0;
    u32 error = 0;
    if (target.size == 0 || !dos_->read_file(list, text.data(), target.size, got, error)) {
        return;
    }
    // Each line names a file from the volume's root: ":Drawer/File".
    for (u32 start = 0, end = 0; start < got; start = end + 1) {
        for (end = start; end < got && text[end] != '\n'; ++end) {
        }
        if (text[start] != ':' || end - start >= Dos::kMaxPath) {
            continue;
        }
        char name[Dos::kMaxPath]{};
        __builtin_memcpy(name, text.data() + start + 1, end - start - 1);
        char path[Dos::kMaxPath];
        char file[32];
        if (join_path(volume, name, path) && split_file(path, file)) {
            add_left_out(path, file);
        }
    }
}

bool Workbench::leave_out(u32 window_id, u32 icon_id) {
    const IconEntry* icon = file_icon(window_id, icon_id);
    if (icon == nullptr) {
        const IconEntry* other = find_icon(icon_id);
        show_error("cannot leave out", other != nullptr ? other->name : "?",
                   ERROR_OBJECT_WRONG_TYPE);
        return false;
    }
    // An icon without an .info cannot come back with the volume.
    if (icon->plain) {
        LogLine().text("workbench: ").text(icon->name).text(" cannot be left out").send();
        return false;
    }
    const DrawerWindow& window = *find_window(window_id);
    char drawer[Dos::kMaxPath];
    char file[32];
    copy_text(drawer, sizeof drawer, window.path);
    copy_text(file, sizeof file, icon->file);
    char path[Dos::kMaxPath];
    char list[Dos::kMaxPath];
    char line[Dos::kMaxPath];
    u32 error = ERROR_INVALID_COMPONENT_NAME;
    const usize shown = left_out_.size();
    if (!join_path(drawer, file, path) || !backdrop_entry(path, list, line)) {
        show_error("cannot leave out", file, error);
        return false;
    }
    // The icon comes first: a line without one would stay unseen.
    if (!add_left_out(drawer, file)) {
        show_error("cannot leave out", file, ERROR_NO_FREE_STORE);
        return false;
    }
    if (!set_backdrop_line(list, line, true, error)) {
        if (left_out_.size() > shown) {
            remove_root_icon(left_out_.back().icon);
        }
        show_error("cannot leave out", file, error);
        return false;
    }
    draw_window(kRootWindow);
    return true;
}

void Workbench::move_left_outs(const char* from, const char* to) {
    struct Moved {
        u32 icon;
        char path[Dos::kMaxPath];
    };
    std::vector<Moved> moved;
    for (const LeftOut& out : left_out_) {
        const IconEntry* icon = find_icon(out.icon);
        Moved candidate{out.icon, {}};
        if (icon != nullptr && join_path(out.drawer, icon->file, candidate.path) &&
            at_or_below(candidate.path, from)) {
            moved.push_back(candidate);
        }
    }
    for (const Moved& gone : moved) {
        char list[Dos::kMaxPath];
        char line[Dos::kMaxPath];
        u32 error = 0;
        if (backdrop_entry(gone.path, list, line)) {
            set_backdrop_line(list, line, false, error);
        }
        remove_root_icon(gone.icon);
        if (to == nullptr) {
            continue;
        }
        // The same path below to as the icon's file had below from.
        const char* below = gone.path + text_length(from);
        char path[Dos::kMaxPath];
        char file[32];
        copy_text(path, sizeof path, to);
        if ((*below != '/' || join_path(to, below + 1, path)) &&
            backdrop_entry(path, list, line) && set_backdrop_line(list, line, true, error) &&
            split_file(path, file)) {
            add_left_out(path, file);
        }
    }
    if (!moved.empty()) {
        draw_window(kRootWindow);
    }
}

bool Workbench::put_away(u32 icon_id) {
    const LeftOut* out = left_out(icon_id);
    const IconEntry* icon = find_icon(icon_id);
    if (out == nullptr || icon == nullptr) {
        return false;
    }
    char path[Dos::kMaxPath];
    char list[Dos::kMaxPath];
    char line[Dos::kMaxPath];
    u32 error = ERROR_INVALID_COMPONENT_NAME;
    if (!join_path(out->drawer, icon->file, path) || !backdrop_entry(path, list, line) ||
        !set_backdrop_line(list, line, false, error)) {
        show_error("cannot put away", icon->name, error);
        return false;
    }
    remove_root_icon(icon_id);
    draw_window(kRootWindow);
    LogLine().text("workbench: put away ").text(path).send();
    return true;
}

bool Workbench::trash_path(u32 window_id, u32 icon_id, char (&out)[Dos::kMaxPath]) const {
    const DrawerWindow* window = find_window(window_id);
    const IconEntry* icon = find_icon(icon_id);
    return window != nullptr && icon != nullptr && icon->owner == window_id &&
           icon->type == kGarbage && icon_target_path(*window, *icon, out);
}

bool Workbench::empty_trash(u32 window_id, u32 icon_id) {
    char path[Dos::kMaxPath];
    if (!trash_path(window_id, icon_id, path)) {
        return false;
    }
    std::vector<TreeEntry> tree;
    u32 error = 0;
    bool done = collect_tree(path, tree, error);
    for (usize i = 1; i < tree.size(); ++i) {
        if (tree[i].is_dir) {
            close_windows_below(tree[i].path);
        }
    }
    // Everything but the drawer itself, what is in a drawer first.
    for (usize i = tree.size(); done && i > 1; --i) {
        done = dos_->delete_object(tree[i - 1].path, error);
    }
    if (done) {
        LogLine().text("workbench: emptied ").text(path).send();
    } else {
        show_error("cannot empty", path, error);
    }
    for (DrawerWindow& w : windows_) {
        if (same_ci(w.path, path)) {
            update_window(w);
        }
    }
    return done;
}

void Workbench::ask_empty_trash(u32 window_id, u32 icon_id) {
    char path[Dos::kMaxPath];
    if (trash_path(window_id, icon_id, path) &&
        confirm("Empty Trash", "Warning: you cannot get back\nwhat you delete!\nOK to empty the "
                               "Trashcan?")) {
        empty_trash(window_id, icon_id);
    }
}

// --- snapshots -----------------------------------------------------------

bool Workbench::snapshot_icon(u32 window_id, u32 icon_id) {
    const DrawerWindow* window = find_window(window_id);
    IconEntry* found = find_icon(icon_id);
    if (window == nullptr || found == nullptr || found->owner != window_id) {
        return false;
    }
    IconEntry& icon = *found;
    // The place a left out icon has in its .info is the one in its drawer.
    if (!icon.owned || icon.type == kAppIcon || icon.object == 0 || left_out(icon.id) != nullptr) {
        LogLine().text("workbench: ").text(icon.name).text(" keeps no snapshot").send();
        return false;
    }
    char path[Dos::kMaxPath];
    if (!icon_path(*window, icon, path)) {
        return false;
    }
    Bus b(*machine_);
    b.w32(icon.object + do_CurrentX, static_cast<u32>(icon.left));
    b.w32(icon.object + do_CurrentY, static_cast<u32>(icon.top));
    u32 error = 0;
    if (!b.ok() || !icon_->save_object(path, icon.object, error)) {
        show_error("cannot snapshot", icon.name, error);
        return false;
    }
    icon.placed = true;
    LogLine().text("workbench: snapshot ").text(path).send();
    return true;
}

bool Workbench::unsnapshot_icon(u32 window_id, u32 icon_id) {
    const DrawerWindow* window = find_window(window_id);
    IconEntry* found = find_icon(icon_id);
    if (window == nullptr || found == nullptr || found->owner != window_id) {
        return false;
    }
    IconEntry& icon = *found;
    if (icon.plain || !icon.owned || icon.type == kAppIcon || icon.object == 0 ||
        left_out(icon.id) != nullptr) {
        LogLine().text("workbench: ").text(icon.name).text(" keeps no snapshot").send();
        return false;
    }
    char path[Dos::kMaxPath];
    if (!icon_path(*window, icon, path)) {
        return false;
    }
    Bus b(*machine_);
    b.w32(icon.object + do_CurrentX, kNoIconPosition);
    b.w32(icon.object + do_CurrentY, kNoIconPosition);
    u32 error = 0;
    if (!b.ok() || !icon_->save_object(path, icon.object, error)) {
        show_error("cannot unsnapshot", icon.name, error);
        return false;
    }
    icon.placed = false;
    if (window_id == kRootWindow) {
        place_root_icons(*window);
    } else {
        place_icons(*window);
    }
    draw_window(window_id);
    LogLine().text("workbench: unsnapshot ").text(path).send();
    return true;
}

bool Workbench::snapshot_window(u32 window_id, bool all) {
    const DrawerWindow* found = find_window(window_id);
    if (found == nullptr) {
        return false;
    }
    if (window_id == kRootWindow) {
        LogLine().text("workbench: the Workbench window keeps no snapshot").send();
        return false;
    }
    const DrawerWindow& drawer = *found;
    // A disk keeps its window in Disk.info, a drawer in its own icon.
    char path[Dos::kMaxPath];
    const bool volume = is_volume_root(drawer.path);
    if (volume) {
        if (!join_path(drawer.path, "Disk", path)) {
            return false;
        }
    } else {
        copy_text(path, sizeof path, drawer.path);
    }
    u32 object = icon_->load_object(path);
    if (object == 0) {
        object = icon_->default_object(volume ? kDisk : kDrawer);
    }
    if (object == 0) {
        show_error("cannot snapshot", drawer.name, ERROR_NO_FREE_STORE);
        return false;
    }
    Bus b(*machine_);
    const u32 data = b.r32(object + do_DrawerData);
    u32 error = ERROR_OBJECT_NOT_FOUND;
    bool saved = false;
    if (b.ok() && data != 0) {
        b.w16(data + ndk::nw_LeftEdge, b.r16(drawer.window + ndk::wd_LeftEdge));
        b.w16(data + ndk::nw_TopEdge, b.r16(drawer.window + ndk::wd_TopEdge));
        b.w16(data + ndk::nw_Width, b.r16(drawer.window + ndk::wd_Width));
        b.w16(data + ndk::nw_Height, b.r16(drawer.window + ndk::wd_Height));
        b.w32(data + dd_CurrentX, static_cast<u32>(drawer.scroll_x));
        b.w32(data + dd_CurrentY, static_cast<u32>(drawer.scroll_y));
        b.w32(data + dd_Flags, drawer.show_all ? DDFLAGS_SHOWALL : DDFLAGS_SHOWICONS);
        // The revision byte makes the save write the DrawerData flags.
        const u32 gadget = object + do_Gadget;
        const u32 user_data = b.r32(gadget + ndk::gg_UserData);
        b.w32(gadget + ndk::gg_UserData, (user_data & ~0xFFu) | 1u);
        saved = b.ok() && icon_->save_object(path, object, error);
    }
    icon_->free_object(object);
    if (!saved) {
        show_error("cannot snapshot", drawer.name, error);
        return false;
    }
    LogLine().text("workbench: snapshot ").text(path).send();
    bool every = true;
    if (all) {
        // Ids, not the icons themselves: a snapshot that fails waits for its
        // requester to be answered, and other tasks may remove icons then.
        std::vector<u32> ids;
        for (const IconEntry& icon : icons_) {
            if (icon.owner == window_id && !icon.plain) {
                ids.push_back(icon.id);
            }
        }
        for (const u32 id : ids) {
            every = snapshot_icon(window_id, id) && every;
        }
    }
    return every;
}

void Workbench::update_all() {
    for (DrawerWindow& w : windows_) {
        update_window(w);
    }
}

void Workbench::update_window(DrawerWindow& drawer) {
    if (drawer.id != kRootWindow) {
        fill_drawer(drawer);
        draw_window(drawer.id);
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
    const DrawerWindow* root = find_window(kRootWindow);
    const u32 window = root != nullptr ? root->window : 0;
    if (b.ok()) {
        intuition_->easy_request(window, block, 0, args);
    }
    exec_->free(block, bytes);
}

void Workbench::menu_pick(u32 window_id, u32 number) {
    const u32 menu = number & 0x1F;
    const u32 item = (number >> 5) & 0x3F;
    const u32 sub = (number >> 11) & 0x1F;
    if (number == kMenuNull || item == 0x3F) {
        return;
    }
    if (menu == kMenuWorkbench) {
        if (item == kItemBackdrop) {
            // Intuition has turned the check mark over already.
            const u32 address = backdrop_item();
            if (address != 0) {
                Bus b(*machine_);
                const bool checked = (b.r16(address + mi_Flags) & CHECKED) != 0;
                if (b.ok()) {
                    set_backdrop(checked);
                }
            }
        } else if (item == kItemExecute) {
            ask_command();
        } else if (item == 2) {
            redraw_all();
        } else if (item == kItemUpdateAll) {
            update_all();
        } else if (item == kItemLastMessage) {
            show_requester("Workbench Request",
                           last_message_[0] != '\0' ? last_message_ : "No messages");
        } else if (item == kItemAbout) {
            show_about();
        } else if (item == kItemQuit) {
            // The task quits once the pass over its messages ends.
            quit_pending_ = confirm("Quit", "OK to quit Workbench?");
        }
    } else if (menu == kMenuWindow) {
        if (item == kItemNewDrawer) {
            ask_new_drawer(window_id);
        } else if (item == 1) {
            open_parent(window_id);
        } else if (item == 2) {
            close_drawer(window_id);
        } else if (item == kItemWindowUpdate) {
            if (DrawerWindow* w = find_window(window_id); w != nullptr) {
                update_window(*w);
            }
        } else if (item == 4) {
            select_contents(window_id);
        } else if (item == 5) {
            clean_up(window_id);
        } else if (item == kItemWindowSnapshot && sub != kNoSub) {
            snapshot_window(window_id, sub == kSubAll);
        } else if (item == kItemWindowShow && sub != kNoSub) {
            pick_show(window_id, sub == kSubAllFiles);
        }
    } else if (menu == kMenuTools) {
        if (item == kItemResetWb && close_for_reset()) {
            open_after_reset();
        }
    } else if (menu == kMenuIcons) {
        const DrawerWindow* window = find_window(window_id);
        const u32 selected = window != nullptr ? window->clicks.selected() : kNoIcon;
        if (item == 0) {
            open_selected(window_id);
        } else if (item == kItemIconCopy && selected != kNoIcon) {
            copy_icon(window_id, selected);
        } else if (item == kItemIconRename && selected != kNoIcon) {
            ask_rename(window_id, selected);
        } else if (item == kItemIconInformation && selected != kNoIcon) {
            show_information(window_id, selected);
        } else if (item == kItemIconSnapshot && selected != kNoIcon) {
            snapshot_icon(window_id, selected);
        } else if (item == kItemIconUnsnapshot && selected != kNoIcon) {
            unsnapshot_icon(window_id, selected);
        } else if (item == kItemEmptyTrash && selected != kNoIcon) {
            ask_empty_trash(window_id, selected);
        } else if (item == kItemIconLeaveOut && selected != kNoIcon) {
            leave_out(window_id, selected);
        } else if (item == kItemIconPutAway && selected != kNoIcon) {
            put_away(selected);
        } else if (item == kItemIconDelete && selected != kNoIcon) {
            ask_delete(window_id, selected);
        }
    }
}

// --- messages ------------------------------------------------------------

void Workbench::handle_message(u32 message) {
    Bus b(*machine_);
    const u32 idcmp_class = b.r32(message + ndk::im_Class);
    const u32 code = b.r16(message + ndk::im_Code);
    const i32 mx = static_cast<i16>(b.r16(message + ndk::im_MouseX));
    const i32 my = static_cast<i16>(b.r16(message + ndk::im_MouseY));
    const u64 when = static_cast<u64>(b.r32(message + ndk::im_Seconds)) * 1000000 +
                     b.r32(message + ndk::im_Micros);
    const u32 window = b.r32(message + ndk::im_IDCMPWindow);
    const DrawerWindow* source =
        windows_.find_if([&](const DrawerWindow& w) { return w.window == window; });
    const u32 window_id = source != nullptr ? source->id : kNoWindow;
    const u16 scroll_id = scroll_gadget_id(window_id, b.r32(message + ndk::im_IAddress));
    // The message goes back before it is acted on: closing the window it
    // came from takes everything still tied to it away.
    exec_->reply_message(message);
    // A message for a window the desktop no longer has is only answered.
    if (!b.ok() || window_id == kNoWindow) {
        return;
    }
    if (idcmp_class == ndk::IDCMP_CLOSEWINDOW) {
        close_drawer(window_id);
    } else if (idcmp_class == ndk::IDCMP_NEWSIZE) {
        if (DrawerWindow* w = find_window(window_id); w != nullptr) {
            measure_window(*w);
            layout_gauge(*w);
        }
        draw_window(window_id);
    } else if (idcmp_class == ndk::IDCMP_ACTIVEWINDOW ||
               idcmp_class == ndk::IDCMP_INACTIVEWINDOW) {
        // The border changed its face under the gauge; the menu's Show
        // marks follow the window that became active.
        if (DrawerWindow* w = find_window(window_id); w != nullptr) {
            layout_gauge(*w);
            if (idcmp_class == ndk::IDCMP_ACTIVEWINDOW) {
                set_show_marks(w->id != kRootWindow && w->show_all);
            }
        }
    } else if (scroll_id != 0 &&
               (idcmp_class == ndk::IDCMP_GADGETDOWN || idcmp_class == ndk::IDCMP_GADGETUP ||
                idcmp_class == ndk::IDCMP_MOUSEMOVE)) {
        scroll_message(window_id, idcmp_class, scroll_id);
    } else if (idcmp_class == ndk::IDCMP_MOUSEBUTTONS && code == ndk::SELECTDOWN) {
        mouse_click(window_id, mx, my, when);
    } else if (idcmp_class == ndk::IDCMP_MOUSEBUTTONS && code == ndk::SELECTUP) {
        end_drag(window_id, mx, my);
    } else if (idcmp_class == ndk::IDCMP_MOUSEMOVE) {
        drag_to(window_id, mx, my);
    } else if (idcmp_class == kIdcmpMenuPick) {
        menu_pick(window_id, code);
    } else if (idcmp_class == ndk::IDCMP_RAWKEY &&
               (code == ndk::NM_WHEEL_UP || code == ndk::NM_WHEEL_DOWN)) {
        // Up and down, or across when the drawer is as tall as its icons.
        const i32 step = code == ndk::NM_WHEEL_UP ? -kWheelStep : kWheelStep;
        // The backdrop, which has no scroll gadgets, does not scroll.
        if (const DrawerWindow* w = find_window(window_id); w != nullptr && w->gadgets != 0) {
            const i32 old_y = w->scroll_y;
            scroll_by(window_id, 0, step);
            if (w->scroll_y == old_y) {
                scroll_by(window_id, step, 0);
            }
        }
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
    if (window_port_ != 0) {
        for (u32 n = 0;; ++n) {
            if (n == kMaxMessagesPerPump) {
                more = true;
                break;
            }
            const u32 message = exec_->get_message(window_port_);
            if (message == 0) {
                break;
            }
            handle_message(message);
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
    add(window_port_);
    return mask;
}

void Workbench::run() {
    open_desktop();
    for (;;) {
        const bool more = pump();
        if (quit_pending_) {
            quit_pending_ = false;
            if (quit()) {
                return;
            }
        }
        const u64 now = exec_->now_us();
        if (reopen_pending_) {
            // OpenWorkBench after a reset: the windows come back.
            reopen_pending_ = false;
            open_after_reset();
        }
        if (patterns_changed_) {
            // WBPattern's Test and Use show at once.
            patterns_changed_ = false;
            if (load_patterns()) {
                redraw_all();
            }
        }
        if (now >= next_volume_us_) {
            refresh_volumes();
            next_volume_us_ = now + kVolumeCheckUs;
        }
        if (now >= next_refresh_us_) {
            // WBPattern's Use takes effect at the next look.
            if (load_patterns()) {
                redraw_all();
            }
            refresh_title();
            refresh_gauges();
            next_refresh_us_ = now + kRefreshUs;
        }
        // Until a message arrives at one of the ports, or the title is due
        // again. Messages a full pump left behind are taken at once.
        if (!more) {
            const u64 due = next_volume_us_ < next_refresh_us_ ? next_volume_us_ : next_refresh_us_;
            exec_->wait_for(wait_mask(), due > now ? due - now : 0);
        }
    }
}

void Workbench::task_entry(void* ctx) { static_cast<Workbench*>(ctx)->run(); }

// A file written and closed: when it is the pattern prefs, the Workbench
// wakes to read them.
void Workbench::file_closed(void* ctx, const char* path) {
    Workbench& wb = *static_cast<Workbench*>(ctx);
    constexpr char kName[] = "WBPattern.prefs";
    constexpr u32 kNameLength = sizeof kName - 1;
    u32 length = 0;
    while (path[length] != '\0') {
        ++length;
    }
    if (length < kNameLength) {
        return;
    }
    for (u32 i = 0; i < kNameLength; ++i) {
        char c = path[length - kNameLength + i];
        char want = kName[i];
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        want = want >= 'A' && want <= 'Z' ? static_cast<char>(want - 'A' + 'a') : want;
        if (c != want) {
            return;
        }
    }
    wb.wake_for_patterns();
}

// The Workbench wakes to read the pattern prefs again.
void Workbench::wake_for_patterns() {
    patterns_changed_ = true;
    Bus b(*machine_);
    const u32 bit = reply_port_ != 0 ? b.r8(reply_port_ + ndk::mp_SigBit) : 32;
    if (b.ok() && bit < 32 && process_ != 0) {
        exec_->signal_task(process_, 1u << bit);
    }
}

u32 Workbench::icon_font() const {
    if (icon_font_ == 0 || machine_ == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    return b.r32(icon_font_ + fc_TextFont);
}

u32 Workbench::system_font() const {
    if (system_font_config_ == 0 || machine_ == nullptr) {
        return 0;
    }
    Bus b(*machine_);
    return b.r32(system_font_config_ + fc_TextFont);
}

// WBConfig: IPrefs passes the desktop's and the windows' backdrop, kinds 1
// and 2, as it applies WBPattern's prefs. The Workbench reads those prefs
// itself, so a backdrop only wakes it. Kinds 3 and 4 are the icon text font
// and the system font: the struct is kept for the Workbench to draw with
// and the one it replaces goes back, IPrefs closing its font. Every other
// kind is taken and left.
u32 Workbench::wb_config(u32 kind, u32 data) {
    if (kind == 1 || kind == 2) {
        wake_for_patterns();
        return 0;
    }
    if (kind != 3 && kind != 4) {
        return 0;
    }
    u32& config = kind == 3 ? icon_font_ : system_font_config_;
    const u32 previous = config;
    if (data == previous) {
        return previous;
    }
    config = data;
    if (kind == 3) {
        relayout_labels();
    }
    return previous;
}

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
    if (tools_.full()) {
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
    // The tool is a program of its own, whatever task started it.
    exec_->begin_program(process, seglist, true);
    if (!exec_->post_message(port, message)) {
        LogLine().text("workbench: no startup message for ").text(name).send();
    }
    RunningTool& record = *tools_.add();
    record.message = message;
    record.message_bytes = message_bytes;
    record.process = process;
    record.seglist = seglist;
    for (u32 i = 0; i < lock_count; ++i) {
        record.locks[i] = locks[i];
    }
    record.lock_count = lock_count;
    copy_text(record.name, sizeof record.name, name);
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
        // Without a volume the path is relative to the drawer, and each
        // leading slash steps up one level: "/Installer" is the tool in the
        // drawer's parent.
        char full[Dos::kMaxPath];
        const char* rest = reference;
        if (colon >= 0) {
            copy_text(full, Dos::kMaxPath, reference);
        } else {
            copy_text(full, Dos::kMaxPath, drawer);
            while (*rest == '/') {
                u32 end = text_length(full);
                while (end > 0 && full[end - 1] != '/' && full[end - 1] != ':') {
                    --end;
                }
                if (end > 0 && full[end - 1] == '/') {
                    --end;
                }
                full[end] = '\0';
                ++rest;
            }
            char joined[Dos::kMaxPath];
            if (!join_path(full, rest, joined)) {
                return false;
            }
            copy_text(full, Dos::kMaxPath, joined);
        }
        u32 last = 0;
        for (u32 i = 0; full[i] != '\0'; ++i) {
            if (full[i] == '/' || full[i] == ':') {
                last = i;
            }
        }
        copy_text(name, Dos::kMaxPath, full + last + 1);
        full[full[last] == ':' ? last + 1 : last] = '\0';
        copy_text(dir, Dos::kMaxPath, full);
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
    static const char* const kToolPath[] = {"C:", "SYS:Utilities", "SYS:System", "SYS:Tools",
                                            "SYS:Tools/Commodities", "SYS:Prefs"};
    for (const char* where : kToolPath) {
        if (join_path(where, reference, probe) &&
            dos_->examine(probe).kind != Dos::TargetKind::Missing) {
            copy_text(dir, Dos::kMaxPath, where);
            return name[0] != '\0';
        }
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
}

bool Workbench::tool_reply(u32 message) {
    RunningTool* tool =
        tools_.find_if([&](const RunningTool& candidate) { return candidate.message == message; });
    if (tool == nullptr) {
        LogLine().text("workbench: reply to no running program").send();
        return false;
    }
    LogLine().text("workbench: ").text(tool->name).text(" ended").send();
    release_tool(*tool);
    tools_.remove(tool);
    return true;
}

void Workbench::reap_tools() {
    tools_.remove_if([&](RunningTool& tool) {
        const bool ended = exec_->scheduler().find(tool.process) < 0;
        if (ended) {
            LogLine().text("workbench: ").text(tool.name).text(" ended without a reply").send();
            release_tool(tool);
        }
        return ended;
    });
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
    window_port_ = exec_->create_port();
    if (reply_port_ == 0 || window_port_ == 0) {
        LogLine().text("workbench: no message port").send();
        return;
    }
    // A desktop that quit has its menu strip still.
    if (menu_ == 0) {
        menu_ = build_menu_strip();
    }
    open_root();
    if (!has_window(kRootWindow)) {
        return;
    }
    set_show_marks(false);
    add_volume_icons();
    load_patterns();
    dos_->set_file_closed(&Workbench::file_closed, this);
    intuition_->set_workbench_hooks(
        Intuition::WorkbenchHooks{&Workbench::hook_close, &Workbench::hook_open, this});
    draw_window(kRootWindow);
    start_wbstartup();
    desktop_open_ = true;
    refresh_title();
    next_refresh_us_ = exec_->now_us() + kRefreshUs;
}

bool Workbench::close_for_reset() {
    DrawerWindow* root = find_window(kRootWindow);
    if (!desktop_open_ || root == nullptr) {
        return false;
    }
    cancel_drag();
    remembered_.clear();
    Bus b(*machine_);
    for (const DrawerWindow& w : windows_) {
        if (w.id == kRootWindow) {
            continue;
        }
        RememberedDrawer& drawer = remembered_.emplace_back();
        copy_text(drawer.path, sizeof drawer.path, w.path);
        copy_text(drawer.name, sizeof drawer.name, w.name);
        drawer.left = static_cast<i16>(b.r16(w.window + ndk::wd_LeftEdge));
        drawer.top = static_cast<i16>(b.r16(w.window + ndk::wd_TopEdge));
        drawer.width = static_cast<i16>(b.r16(w.window + ndk::wd_Width));
        drawer.height = static_cast<i16>(b.r16(w.window + ndk::wd_Height));
        drawer.show_all = w.show_all;
    }
    while (const DrawerWindow* drawer = windows_.find_if(
               [](const DrawerWindow& w) { return w.id != kRootWindow; })) {
        close_drawer(drawer->id);
    }
    intuition_->clear_menu_strip(root->window);
    intuition_->close_window(root->window);
    release_window(*root);
    return true;
}

bool Workbench::quit() {
    reap_tools();
    // A program that registered an application icon, window or menu item
    // would lose it.
    if (desktop_open_ && (!tools_.empty() || !apps_.empty())) {
        constexpr char kRunning[] = "Cannot quit while programs are using\nthe Workbench";
        LogLine().text("workbench: cannot quit, programs are using it").send();
        show_requester("Workbench Request", kRunning);
        return false;
    }
    if (!close_for_reset()) {
        return false;
    }
    remembered_.clear();
    free_icons(kRootWindow);
    dos_->set_file_closed(nullptr, nullptr);
    intuition_->set_workbench_hooks({});
    exec_->delete_port(reply_port_);
    exec_->delete_port(window_port_);
    reply_port_ = 0;
    window_port_ = 0;
    volume_top_ = 0;
    next_volume_us_ = 0;
    reopen_pending_ = false;
    desktop_open_ = false;
    running_ = false;
    process_ = 0;
    LogLine().text("workbench: quit").send();
    return true;
}

void Workbench::open_after_reset() {
    if (!desktop_open_ || machine_ == nullptr || has_window(kRootWindow)) {
        return;
    }
    open_root();
    const DrawerWindow* root = find_window(kRootWindow);
    if (root == nullptr) {
        remembered_.clear();
        return;
    }
    place_root_icons(*root);
    draw_window(kRootWindow);
    refresh_title();
    set_show_marks(false);
    for (const RememberedDrawer& drawer : remembered_) {
        const DrawerBox box{drawer.left, drawer.top, drawer.width, drawer.height};
        open_drawer_window(drawer.path, drawer.name, 0, &box, drawer.show_all);
    }
    remembered_.clear();
}

bool Workbench::hook_close(void* ctx) { return static_cast<Workbench*>(ctx)->close_for_reset(); }

// The windows reopen in the Workbench's own task, so they belong to it and
// not to the task that called OpenWorkBench; without a task of its own they
// reopen at once. Their port is the one it made, whichever task opens them.
void Workbench::hook_open(void* ctx) {
    Workbench& wb = *static_cast<Workbench*>(ctx);
    Bus b(*wb.machine_);
    const u32 bit = wb.reply_port_ != 0 ? b.r8(wb.reply_port_ + ndk::mp_SigBit) : 32;
    if (!b.ok() || bit >= 32 || wb.process_ == 0) {
        wb.open_after_reset();
        return;
    }
    wb.reopen_pending_ = true;
    wb.exec_->signal_task(wb.process_, 1u << bit);
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
    DrawerWindow* drawer = windows_.find_if([&](const DrawerWindow& w) {
        return w.id != kRootWindow && (same_ci(w.path, text) || same_ci(w.name, text));
    });
    if (drawer == nullptr) {
        return false;
    }
    update_window(*drawer);
    return true;
}

// --- application registrations -------------------------------------------

void Workbench::keep_app_icons(void* ctx) {
    Workbench& w = *static_cast<Workbench*>(ctx);
    for (const App& app : w.apps_) {
        const IconEntry* icon = app.kind == AppKind::Icon ? w.find_icon(app.icon) : nullptr;
        if (icon != nullptr) {
            w.exec_->keep_block_at(icon->object);
        }
    }
}

u32 Workbench::record_app(AppKind kind, u32 id, u32 user) {
    if (apps_.full()) {
        LogLine().text("workbench: application table full").send();
        return 0;
    }
    const u32 handle = exec_->alloc(kAppBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (handle == 0) {
        return 0;
    }
    App& app = *apps_.add();
    app.kind = kind;
    app.handle = handle;
    app.id = id;
    app.user = user;
    return handle;
}

Workbench::App* Workbench::find_app(u32 handle, AppKind kind) {
    return apps_.find_if(
        [&](const App& app) { return app.handle == handle && app.kind == kind; });
}

void Workbench::drop_app_icon(const App& app) {
    IconEntry* icon = find_icon(app.icon);
    if (icon == nullptr || icon->owner != kRootWindow) {
        return;
    }
    if (drag_.armed && drag_.icon == app.icon) {
        cancel_drag();
    }
    icons_.remove(icon);
    DrawerWindow* root = find_window(kRootWindow);
    if (root != nullptr && root->clicks.selected() == app.icon) {
        root->clicks.clear();
    }
    draw_window(kRootWindow);
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
    apps_.remove(app);
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
    const DrawerWindow* root = find_window(kRootWindow);
    if (root == nullptr) {
        return handle;
    }
    IconEntry* icon = take_icon(kRootWindow);
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
    place_root_icon(*root, *icon);
    app->icon = icon->id;
    draw_window(kRootWindow);
    return handle;
}

bool Workbench::remove_app_icon(u32 handle) {
    App* app = find_app(handle, AppKind::Icon);
    if (app == nullptr) {
        return false;
    }
    drop_app_icon(*app);
    exec_->free(app->handle, kAppBytes);
    apps_.remove(app);
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
    apps_.remove(app);
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

    static void wb_config(void* ctx, Machine& m) {
        native::set_result(m, of(ctx).wb_config(m.cpu.d[0], m.cpu.d[1]));
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
    {workbench_lvo::WBConfig, WorkbenchNatives::wb_config, "WBConfig"},
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
    intuition.set_workbench_hooks(Intuition::WorkbenchHooks{nullptr, nullptr, nullptr});
    exec.add_program_keeper(&Workbench::keep_app_icons, this);
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
