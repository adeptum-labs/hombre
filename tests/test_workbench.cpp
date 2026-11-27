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

#include "libs/chrome.hpp"
#include "libs/chrome_palette.hpp"
#include "libs/display_modes.hpp"
#include "libs/dos.hpp"
#include "libs/exec.hpp"
#include "libs/gadtools.hpp"
#include "libs/graphics.hpp"
#include "libs/icon.hpp"
#include "libs/intuition.hpp"
#include "libs/workbench.hpp"
#include "check.hpp"
#include "ofs_writer.hpp"
#include "test_guest.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;
using namespace hombre::ndk;

namespace {

constexpr u32 kCall = 0x1800;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;  // test structures and strings up to 0xFFFF
constexpr u32 kFreeStart = 0x10000;

constexpr u8 kWBDisk = 1;
constexpr u8 kWBDrawer = 2;
constexpr u8 kWBTool = 3;
constexpr u8 kWBProject = 4;
constexpr u8 kWBGarbage = 5;
constexpr u32 kNoIconPosition = 0x80000000;
constexpr i32 kDefaultDrawerWidth = 320;
constexpr i32 kDefaultDrawerHeight = 160;

// struct Menu and struct MenuItem
constexpr u32 mu_NextMenu = 0;
constexpr u32 mu_Flags = 12;
constexpr u32 mu_MenuName = 14;
constexpr u32 mu_FirstItem = 18;
constexpr u32 mi_NextItem = 0;
constexpr u32 mi_Flags = 12;
constexpr u32 mi_MutualExclude = 14;
constexpr u32 mi_ItemFill = 18;
constexpr u32 mi_SubItem = 28;
constexpr u16 MENUENABLED = 0x0001;
constexpr u16 ITEMTEXT = 0x0002;
constexpr u16 ITEMENABLED = 0x0010;
constexpr u16 CHECKIT = 0x0001;
constexpr u16 MENUTOGGLE = 0x0008;
constexpr u16 CHECKED = 0x0100;
constexpr u32 it_IText = 12;

// struct DiskObject and its DrawerData's NewWindow
constexpr u32 do_Gadget = 4;
constexpr u32 do_Type = 48;
constexpr u32 do_DefaultTool = 50;
constexpr u32 do_ToolTypes = 54;
constexpr u32 do_CurrentX = 58;
constexpr u32 do_CurrentY = 62;
constexpr u32 do_DrawerData = 66;
constexpr u32 do_StackSize = 74;
constexpr u32 dd_Flags = 56;

// struct WBStartup and struct WBArg
constexpr u32 sm_Process = 20;
constexpr u32 sm_Segment = 24;
constexpr u32 sm_NumArgs = 28;
constexpr u32 sm_ToolWindow = 32;
constexpr u32 sm_ArgList = 36;
constexpr u32 WBStartup_SIZEOF = 40;
constexpr u32 wa_Lock = 0;
constexpr u32 wa_Name = 4;

// The private struct IPrefs passes WBConfig for the icon and system fonts.
constexpr u32 fc_TextAttr = 0;
constexpr u32 fc_TextFont = 4;
constexpr u32 fc_DrawMode = 8;
constexpr u32 fc_FrontPen = 10;
constexpr u32 fc_BackPen = 11;

// Static storage, as the kernel keeps them.
Jit g_jit;
Exec g_exec;
Dos g_dos;
Icon g_icon;
Graphics g_graphics;
Intuition g_intuition;
GadTools g_gadtools;
DisplayState g_display;
std::vector<std::string> g_lines;

void capture(const char* line) { g_lines.push_back(line); }

bool logged(const std::string& text) {
    for (const std::string& line : g_lines) {
        if (line == text) {
            return true;
        }
    }
    return false;
}

u32 logged_count(const std::string& text) {
    u32 count = 0;
    for (const std::string& line : g_lines) {
        if (line == text) {
            ++count;
        }
    }
    return count;
}

// --- the .info files of the test disk ------------------------------------

struct Info {
    std::vector<u8> bytes;

    void byte(u32 v) { bytes.push_back(static_cast<u8>(v)); }

    void word(u32 v) {
        byte(v >> 8);
        byte(v);
    }

    void u32v(u32 v) {
        word(v >> 16);
        word(v);
    }
};

void put_gadget(Info& f, u32 width, u32 height, u32 user_data = 1) {
    f.u32v(0);  // NextGadget
    f.word(0);  // LeftEdge
    f.word(0);  // TopEdge
    f.word(width);
    f.word(height);
    f.word(0);  // Flags
    f.word(1);  // Activation
    f.word(1);  // GadgetType
    f.u32v(1);  // GadgetRender
    f.u32v(0);  // SelectRender
    f.u32v(0);  // GadgetText
    f.u32v(0);  // MutualExclude
    f.u32v(0);  // SpecialInfo
    f.word(0);  // GadgetID
    f.u32v(user_data);  // UserData: WB_DISKREVISION
}

void put_image(Info& f, u32 width, u32 height, u32 depth, u32 rows) {
    f.word(0);  // LeftEdge
    f.word(0);  // TopEdge
    f.word(width);
    f.word(height);
    f.word(depth);
    f.u32v(1);  // ImageData
    f.byte((1u << depth) - 1);  // PlanePick
    f.byte(0);                  // PlaneOnOff
    f.u32v(0);                  // NextImage
    const u32 row_bytes = (width + 15) / 16 * 2;
    for (u32 i = 0; i < row_bytes * rows * depth; ++i) {
        f.byte((i * 37 + 11) & 0xFF);
    }
}

// The DiskObject every icon of the test disk starts with. drawer_data adds
// the old DrawerData NewWindow, default_tool a DefaultTool string after the
// image.
void put_header(Info& f, u8 type, i32 current_x, i32 current_y, bool drawer_data,
                bool default_tool, u32 revision = 1) {
    f.word(0xE310);
    f.word(1);
    put_gadget(f, 32, 12, revision);
    f.byte(type);
    f.byte(0);
    f.u32v(default_tool ? 1 : 0);
    f.u32v(0);  // ToolTypes
    f.u32v(static_cast<u32>(current_x));
    f.u32v(static_cast<u32>(current_y));
    f.u32v(drawer_data ? 1 : 0);
    f.u32v(0);  // ToolWindow
    f.u32v(4096);
}

std::vector<u8> icon_info(u8 type, i32 current_x, i32 current_y) {
    Info f;
    put_header(f, type, current_x, current_y, false, false);
    put_image(f, 32, 12, 1, 12);
    return std::move(f.bytes);
}

std::vector<u8> drawer_icon_info(i32 current_x, i32 current_y, i32 left, i32 top, i32 width,
                                 i32 height, i32 scroll_x = 0, i32 scroll_y = 0,
                                 u32 dd_flags = 0, u32 revision = 1) {
    Info f;
    put_header(f, kWBDrawer, current_x, current_y, true, false, revision);
    // OldDrawerData: the NewWindow of the drawer.
    f.word(static_cast<u32>(left));
    f.word(static_cast<u32>(top));
    f.word(static_cast<u32>(width));
    f.word(static_cast<u32>(height));
    f.byte(0);   // DetailPen
    f.byte(1);   // BlockPen
    f.u32v(0);   // IDCMPFlags
    f.u32v(0);   // Flags
    f.u32v(0);   // FirstGadget
    f.u32v(0);   // CheckMark
    f.u32v(0);   // Title
    f.u32v(0);   // Screen
    f.u32v(0);   // BitMap
    f.word(0);   // MinWidth
    f.word(0);   // MinHeight
    f.word(0);   // MaxWidth
    f.word(0);   // MaxHeight
    f.word(1);   // Type: WBENCHSCREEN
    f.u32v(static_cast<u32>(scroll_x));  // CurrentX
    f.u32v(static_cast<u32>(scroll_y));  // CurrentY
    put_image(f, 32, 12, 2, 12);
    // The OS 2 disk revision follows the other parts.
    if (revision != 0) {
        f.u32v(dd_flags);  // dd_Flags
        f.word(0);         // dd_ViewModes
    }
    return std::move(f.bytes);
}

std::vector<u8> project_info(const std::string& default_tool, i32 current_x, i32 current_y) {
    Info f;
    put_header(f, kWBProject, current_x, current_y, false, true);
    put_image(f, 32, 12, 1, 12);
    f.u32v(static_cast<u32>(default_tool.size()) + 1);
    f.bytes.insert(f.bytes.end(), default_tool.begin(), default_tool.end());
    f.byte(0);
    if (f.bytes.size() % 2 != 0) {
        f.byte(0);
    }
    return std::move(f.bytes);
}

// A project icon with both a default tool and tool types.
std::vector<u8> typed_project_info(const std::string& default_tool,
                                   const std::vector<std::string>& types, i32 stack,
                                   i32 current_x, i32 current_y) {
    Info f;
    f.word(0xE310);
    f.word(1);
    put_gadget(f, 32, 12);
    f.byte(kWBProject);
    f.byte(0);
    f.u32v(1);  // DefaultTool
    f.u32v(1);  // ToolTypes
    f.u32v(static_cast<u32>(current_x));
    f.u32v(static_cast<u32>(current_y));
    f.u32v(0);  // DrawerData
    f.u32v(0);  // ToolWindow
    f.u32v(static_cast<u32>(stack));
    put_image(f, 32, 12, 1, 12);
    f.u32v(static_cast<u32>(default_tool.size()) + 1);
    f.bytes.insert(f.bytes.end(), default_tool.begin(), default_tool.end());
    f.byte(0);
    f.u32v(static_cast<u32>((types.size() + 1) * 4));
    for (const std::string& type : types) {
        f.u32v(static_cast<u32>(type.size()) + 1);
        f.bytes.insert(f.bytes.end(), type.begin(), type.end());
        f.byte(0);
    }
    return std::move(f.bytes);
}

// A hunk file whose code is one RTS.
std::vector<u8> hunk_rts() {
    std::vector<u8> out;
    auto put = [&](u32 v) {
        out.push_back(static_cast<u8>(v >> 24));
        out.push_back(static_cast<u8>(v >> 16));
        out.push_back(static_cast<u8>(v >> 8));
        out.push_back(static_cast<u8>(v));
    };
    for (u32 v : {1011u, 0u, 1u, 0u, 0u, 1u, 1001u, 1u, 0x4E750000u, 1010u}) {
        put(v);
    }
    return out;
}

std::vector<u8> text(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }

std::vector<AdfFile> test_files() {
    return {
        {"S/Startup-Sequence", text("echo hello\n")},
        {"Disk.info", icon_info(kWBDisk, static_cast<i32>(kNoIconPosition),
                                static_cast<i32>(kNoIconPosition))},
        {"Tools.info", drawer_icon_info(static_cast<i32>(kNoIconPosition),
                                        static_cast<i32>(kNoIconPosition), 30, 50, 300, 120)},
        {"Tools/Tool.info", icon_info(kWBTool, 100, 40)},
        {"Tools/Free.info", icon_info(kWBTool, static_cast<i32>(kNoIconPosition),
                                      static_cast<i32>(kNoIconPosition))},
        {"Tools/Put.info", icon_info(kWBTool, 20, 100)},
        {"Tools/Tiny", hunk_rts()},
        {"Tools/Proj.info", project_info("Tiny", 60, 80)},
        {"Tools/Proj", text("a project")},
        {"Utilities/Finder", hunk_rts()},
        {"Tools/Lost.info", project_info("Finder", 90, 80)},
        {"Tools/Lost", text("a project")},
        {"Tools/Up.info", project_info("/Tiny2", 120, 80)},
        {"Tools/Up", text("a project")},
        {"Tiny2", hunk_rts()},
        {"Tools/Sub.info", drawer_icon_info(200, 20, 60, 70, 200, 90)},
        {"Tools/Sub/Deep.info", icon_info(kWBTool, 10, 10)},
        {"Tools/Fake.info", icon_info(kWBDisk, 250, 10)},
        {"Tools/Fake/Inside.info", icon_info(kWBTool, 10, 10)},
    };
}

// Reads one block of the disk image a hard disk partition is served from.
bool read_image_block(void* ctx, u32 block, u8* out) {
    const std::vector<u8>& image = *static_cast<const std::vector<u8>*>(ctx);
    const usize at = static_cast<usize>(block) * 512;
    if (at + 512 > image.size()) {
        return false;
    }
    std::memcpy(out, image.data() + at, 512);
    return true;
}

// One memfd mapped twice for the code region, 16 MiB of guest memory, the
// Jit and the native libraries over them, with the Workbench's screen open
// and made the default one. A second drive after the floppy drives holds
// its image as a hard disk partition.
class Rig {
public:
    explicit Rig(const std::vector<AdfFile>& files = test_files(),
                 std::vector<u8> second = {}, u32 second_drive = 1, u32 depth = 2,
                 u32 screen_height = 400)
        : memory_(kTestGuestBytes),
          image_(build_adf("HombreTest", false, files)),
          second_image_(std::move(second)) {
        fd_ = memfd_create("workbench", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        m.cpu = CpuState{};
        m.mem = GuestMemory{memory_.data(), kTestGuestBytes};
        m.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, kRegion}, m));
        g_lines.clear();
        g_log = capture;
        CHECK(g_exec.init(m, kFreeStart, kTestChipEnd, kTestGuestBytes));
        CHECK(adf_mount(image_.data(), image_.size(), volume) == FsError::Ok);
        if (second_image_.empty()) {
            CHECK(g_dos.init(m, g_exec, volume, nullptr, nullptr));
        } else {
            Drives drives;
            drives.volume[0] = &volume;
            drives.volume[second_drive] = &second_volume;
            if (second_drive < kFloppyDrives) {
                CHECK(adf_mount(second_image_.data(), second_image_.size(), second_volume) ==
                      FsError::Ok);
            } else {
                const BlockSource source{&second_image_, read_image_block, nullptr};
                CHECK(adf_mount_partition(source, 0, static_cast<u32>(second_image_.size() / 512),
                                          2, second_volume) == FsError::Ok);
                char* device = drives.info[second_drive].device;
                std::strcpy(device, "DH0");
                device[2] = static_cast<char>('0' + second_drive - kFloppyDrives);
            }
            CHECK(g_dos.init(m, g_exec, drives, nullptr, nullptr));
        }
        CHECK(g_icon.init(m, g_exec, g_dos));
        g_display = DisplayState{};
        CHECK(g_graphics.init(m, g_exec, g_display));
        CHECK(g_intuition.init(m, g_exec, g_graphics, g_display));
        CHECK(g_gadtools.init(m, g_exec, g_graphics));
        g_gadtools.set_intuition(&g_intuition);
        g_intuition.set_gadget_renderer(&GadTools::render_gadget, &g_gadtools);
        g_intuition.set_gadget_release(&GadTools::release_gadget);
        CHECK(workbench.init(m, g_exec, g_dos, g_graphics, g_intuition, g_gadtools, g_icon));
        m.cpu.a[7] = kStack;
        screen = open_screen(depth, screen_height);
        g_intuition.set_default_screen(screen);
    }

    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_segment_loader(SegmentLoader{nullptr, nullptr});
        g_exec.set_task_host(TaskHost{});
        g_intuition.set_default_screen(0);
        g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;
    AdfVolume volume{};
    AdfVolume second_volume{};
    Workbench workbench;
    u32 screen = 0;
    u32 free_bytes() const { return g_exec.free_bytes(); }

    void put8(u32 at, u32 v) { memory_[at] = static_cast<u8>(v); }

    void put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
    }

    void put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
    }

    u32 get8(u32 at) const { return memory_[at]; }
    u32 get16(u32 at) const { return (get8(at) << 8) | get8(at + 1); }
    u32 get32(u32 at) const { return (get16(at) << 16) | get16(at + 2); }

    std::string str(u32 at) const {
        std::string s;
        while (memory_[at] != 0 && s.size() < 4096) {
            s += static_cast<char>(memory_[at++]);
        }
        return s;
    }

    u32 open_screen(u32 depth, u32 height) {
        put16(kData + ns_Width, 640);
        put16(kData + ns_Height, height);
        put16(kData + ns_Depth, depth);
        put16(kData + ns_Type, CUSTOMSCREEN);
        const u32 opened = g_intuition.open_screen(kData, 0);
        CHECK(opened != 0);
        return opened;
    }

    u32 find_icon(const std::string& name) const {
        for (const Workbench::IconEntry& icon : workbench.icons()) {
            if (name == icon.name) {
                return icon.id;
            }
        }
        return Workbench::kNoIcon;
    }

    u32 find_window(const std::string& path) const {
        for (const Workbench::DrawerWindow& w : workbench.windows()) {
            if (path == w.path) {
                return w.id;
            }
        }
        return Workbench::kNoWindow;
    }

    const Workbench::RunningTool* find_tool(const std::string& name) const {
        for (const Workbench::RunningTool& tool : workbench.tools()) {
            if (name == tool.name) {
                return &tool;
            }
        }
        return nullptr;
    }

    std::string window_title(u32 window) const { return str(get32(window + wd_Title)); }

    // JSR -lvo(A6) with A6 = base, from hand-written 68000 code.
    ExitReason call(u32 base, u32 lvo) {
        put16(kCall, 0x4EAE);
        put16(kCall + 2, static_cast<u32>(0x10000 - lvo));
        put16(kCall + 4, 0x4EF8);  // JMP $400.w
        put16(kCall + 6, 0x0400);
        g_jit.flush();
        m.cpu.a[6] = base;
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCall;
        return g_jit.run(1000000);
    }

    // Calls the vector, which must return; D0 is what it gave.
    u32 result(u32 base, u32 lvo) {
        CHECK_EQ(call(base, lvo), ExitReason::Halt);
        return m.cpu.d[0];
    }

    // The pen at x, y of a bitmap.
    u32 bitmap_pixel(u32 bitmap, i32 x, i32 y) const {
        const u32 at = static_cast<u32>(y) * get16(bitmap + bm_BytesPerRow) +
                       (static_cast<u32>(x) >> 3);
        const u32 mask = 0x80u >> (static_cast<u32>(x) & 7);
        u32 pen = 0;
        for (u32 plane = 0; plane < get8(bitmap + bm_Depth); ++plane) {
            if ((get8(get32(bitmap + bm_Planes + plane * 4) + at) & mask) != 0) {
                pen |= 1u << plane;
            }
        }
        return pen;
    }

    // The pen at x, y of the window's own bitmap.
    u32 window_pixel(u32 window, i32 x, i32 y) const {
        return bitmap_pixel(get32(get32(window + wd_RPort) + rp_BitMap), x, y);
    }

    // The pen at x, y of the screen's composed bitmap.
    u32 screen_pixel(u32 screen, i32 x, i32 y) const {
        return bitmap_pixel(screen + sc_BitMap, x, y);
    }

    // A gadget's box in its window, its edges and size taken from the
    // window's as Intuition takes them; x1 and y1 are its last pixels.
    struct Box {
        i32 x0;
        i32 y0;
        i32 x1;
        i32 y1;
        bool holds(i32 x, i32 y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };
    Box gadget_box(u32 window, u32 gadget) const {
        const i32 width = static_cast<i16>(get16(window + wd_Width));
        const i32 height = static_cast<i16>(get16(window + wd_Height));
        const u32 flags = get16(gadget + gg_Flags);
        i32 x = static_cast<i16>(get16(gadget + gg_LeftEdge));
        i32 y = static_cast<i16>(get16(gadget + gg_TopEdge));
        i32 w = static_cast<i16>(get16(gadget + gg_Width));
        i32 h = static_cast<i16>(get16(gadget + gg_Height));
        x += (flags & GFLG_RELRIGHT) != 0 ? width - 1 : 0;
        y += (flags & GFLG_RELBOTTOM) != 0 ? height - 1 : 0;
        w += (flags & GFLG_RELWIDTH) != 0 ? width : 0;
        h += (flags & GFLG_RELHEIGHT) != 0 ? height : 0;
        return Box{x, y, x + w - 1, y + h - 1};
    }

    // The screen point at the middle of a window's scroll gadget.
    std::pair<i32, i32> gadget_middle(u32 window_index, u16 id) const {
        const u32 window = workbench.window(window_index).window;
        const Box box = gadget_box(window, workbench.scroll_gadget(window_index, id));
        return {static_cast<i16>(get16(window + wd_LeftEdge)) + (box.x0 + box.x1) / 2,
                static_cast<i16>(get16(window + wd_TopEdge)) + (box.y0 + box.y1) / 2};
    }

    // One press and release of the left button at x, y of the window's
    // inner area, and the desktop's answer to it.
    void click(u32 window_index, i32 x, i32 y) {
        const Workbench::DrawerWindow& w = workbench.window(window_index);
        const i32 sx = static_cast<i32>(get16(w.window + wd_LeftEdge)) + w.inner_left + x;
        const i32 sy = static_cast<i32>(get16(w.window + wd_TopEdge)) + w.inner_top + y;
        g_intuition.pointer_event(sx, sy, 1, 1, 0, true);
        g_intuition.pointer_event(sx, sy, 0, 0, 1, false);
        workbench.pump();
    }

    // The screen point of x, y in the window's inner area.
    i32 screen_x(u32 window_index, i32 x) const {
        const Workbench::DrawerWindow& w = workbench.window(window_index);
        return static_cast<i16>(get16(w.window + wd_LeftEdge)) + w.inner_left + x;
    }
    i32 screen_y(u32 window_index, i32 y) const {
        const Workbench::DrawerWindow& w = workbench.window(window_index);
        return static_cast<i16>(get16(w.window + wd_TopEdge)) + w.inner_top + y;
    }

    // The left button pressed at x, y of the window's inner area, the
    // pointer moved with it held to the screen point sx, sy, and released
    // there; the desktop answers each step.
    void press(u32 window_index, i32 x, i32 y) {
        g_intuition.pointer_event(screen_x(window_index, x), screen_y(window_index, y), 1, 1, 0,
                                  true);
        workbench.pump();
    }
    void move_held(i32 sx, i32 sy) {
        g_intuition.pointer_event(sx, sy, 1, 0, 0, true);
        workbench.pump();
    }
    void release(i32 sx, i32 sy) {
        g_intuition.pointer_event(sx, sy, 0, 0, 1, false);
        workbench.pump();
    }
    void drag(u32 window_index, i32 x, i32 y, i32 sx, i32 sy) {
        press(window_index, x, y);
        move_held((screen_x(window_index, x) + sx) / 2, (screen_y(window_index, y) + sy) / 2);
        move_held(sx, sy);
        release(sx, sy);
    }

    u32 find_icon_in(u32 window_index, const std::string& name) const {
        for (const Workbench::IconEntry& icon : workbench.icons()) {
            if (icon.owner == window_index && name == icon.name) {
                return icon.id;
            }
        }
        return Workbench::kNoIcon;
    }

    bool exists(const std::string& path) const {
        return g_dos.examine(path.c_str()).kind != Dos::TargetKind::Missing;
    }

    // The icon's place as its .info keeps it.
    std::pair<u32, u32> saved_place(const std::string& path) const {
        const u32 object = g_icon.load_object(path.c_str());
        CHECK(object != 0);
        if (object == 0) {
            return {0, 0};
        }
        const std::pair<u32, u32> place{get32(object + do_CurrentX), get32(object + do_CurrentY)};
        g_icon.free_object(object);
        return place;
    }

    u32 put_string(u32 at, const std::string& text) {
        for (usize i = 0; i <= text.size(); ++i) {
            put8(at + static_cast<u32>(i), i < text.size() ? static_cast<u8>(text[i]) : 0);
        }
        return at;
    }

    void rescan(const std::string& path) {
        CHECK(workbench.update_workbench(put_string(kData + 0x400, path)));
    }

    // CreateDir and UnLock through dos.library.
    void make_dir(const std::string& path) {
        m.cpu.d[1] = put_string(kData + 0x500, path);
        const u32 lock = result(g_dos.dos_base(), dos_lvo::CreateDir);
        CHECK(lock != 0);
        m.cpu.d[1] = lock;
        result(g_dos.dos_base(), dos_lvo::UnLock);
    }

private:
    static constexpr usize kRegion = 1 << 20;
    std::vector<u8> memory_;
    std::vector<u8> image_;
    std::vector<u8> second_image_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

// WBConfig through the vector: D0 the kind and D1 its data.
u32 wbconfig(Rig& r, u32 kind, u32 data) {
    r.m.cpu.d[0] = kind;
    r.m.cpu.d[1] = data;
    return r.result(r.workbench.base(), workbench_lvo::WBConfig);
}

// The hasklig font of y_size opened through graphics.library, as IPrefs
// opens the font it passes over.
u32 open_hasklig(Rig& r, u32 attr, u32 name, u32 y_size) {
    r.put32(attr + ta_Name, name);
    r.put16(attr + ta_YSize, y_size);
    r.put8(attr + ta_Style, 0);
    r.put8(attr + ta_Flags, 0);
    r.put_string(name, "hasklig.font");
    const u32 font = g_graphics.open_font(attr);
    CHECK(font != 0);
    return font;
}

// A font struct at at as IPrefs builds it: the TextFont, the draw mode and
// pens to draw with, and a TextAttr and name of its own.
u32 put_font_config(Rig& r, u32 at, u32 font, u32 mode, u32 front, u32 back) {
    r.put32(at + fc_TextAttr, at + 12);
    r.put32(at + fc_TextFont, font);
    r.put16(at + fc_DrawMode, mode);
    r.put8(at + fc_FrontPen, front);
    r.put8(at + fc_BackPen, back);
    r.put32(at + 12 + ta_Name, at + 20);
    r.put16(at + 12 + ta_YSize, 8);
    r.put8(at + 12 + ta_Style, 0);
    r.put8(at + 12 + ta_Flags, 0);
    r.put_string(at + 20, "topaz.font");
    return at;
}

// --- the vectors ---------------------------------------------------------

void layout_matches_the_ndk() {
    CHECK_EQ(workbench_lvo::UpdateWorkbench, 30u);
    CHECK_EQ(workbench_lvo::QuoteWorkbench, 36u);
    CHECK_EQ(workbench_lvo::StartWorkbench, 42u);
    CHECK_EQ(workbench_lvo::AddAppWindowA, 48u);
    CHECK_EQ(workbench_lvo::RemoveAppWindow, 54u);
    CHECK_EQ(workbench_lvo::AddAppIconA, 60u);
    CHECK_EQ(workbench_lvo::RemoveAppIcon, 66u);
    CHECK_EQ(workbench_lvo::AddAppMenuItemA, 72u);
    CHECK_EQ(workbench_lvo::RemoveAppMenuItem, 78u);
    CHECK_EQ(workbench_lvo::WBConfig, 84u);
    CHECK_EQ(workbench_lvo::WBInfo, 90u);
    CHECK_EQ(sm_Process, 20u);
    CHECK_EQ(WBStartup_SIZEOF, 40u);
}

// IPrefs hands the fonts it opened over through WBConfig, which keeps each
// struct and gives back the one of its kind it replaces, 0 the first time.
void wb_config_keeps_the_fonts() {
    Rig r;
    const u32 hasklig13 = open_hasklig(r, kData + 0x100, kData + 0x140, 13);
    const u32 s1 = put_font_config(r, kData + 0x600, hasklig13, JAM2, 2, 3);
    const u32 s2 = put_font_config(r, kData + 0x700, 0, JAM1, 1, 0);
    const u32 t1 = put_font_config(r, kData + 0x800, hasklig13, JAM2, 2, 3);
    const u32 t2 = put_font_config(r, kData + 0x900, 0, JAM1, 1, 0);

    CHECK_EQ(wbconfig(r, 3, s1), 0u);
    CHECK_EQ(r.workbench.icon_font(), hasklig13);
    CHECK_EQ(r.workbench.system_font(), 0u);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 1u);  // still IPrefs' to close
    CHECK_EQ(wbconfig(r, 4, t1), 0u);
    CHECK_EQ(r.workbench.system_font(), hasklig13);
    CHECK_EQ(r.workbench.icon_font(), hasklig13);

    // Each kind hands back its own previous struct, which Workbench does
    // not close.
    CHECK_EQ(wbconfig(r, 3, s2), s1);
    CHECK_EQ(r.get16(hasklig13 + tf_Accessors), 1u);
    CHECK_EQ(r.workbench.icon_font(), 0u);
    CHECK_EQ(r.workbench.system_font(), hasklig13);
    CHECK_EQ(wbconfig(r, 4, t2), t1);
    CHECK_EQ(r.workbench.system_font(), 0u);
    CHECK_EQ(wbconfig(r, 3, s1), s2);
    CHECK_EQ(r.workbench.icon_font(), hasklig13);

    // The other kinds answer 0 and leave the fonts alone.
    CHECK_EQ(wbconfig(r, 1, 0), 0u);
    CHECK_EQ(wbconfig(r, 2, 0), 0u);
    CHECK_EQ(wbconfig(r, 5, s1), 0u);
    CHECK_EQ(r.workbench.icon_font(), hasklig13);
    CHECK_EQ(r.workbench.system_font(), 0u);
    CHECK(!logged("workbench: unimplemented LVO -84"));
}

// A backdrop kind wakes the Workbench to read the pattern prefs; a font
// kind only hands its struct back.
void wb_config_wakes_the_workbench_for_the_backdrops() {
    Rig r;
    r.workbench.open_desktop();
    CHECK_EQ(r.workbench.start(0, 0), DOSTRUE);
    const u32 process = r.workbench.process();
    const u32 bit = r.get8(r.workbench.reply_port() + mp_SigBit);
    CHECK(bit < 32);
    for (u32 kind = 1; kind <= 2; ++kind) {
        r.put32(process + TC_SIGRECVD, 0);
        CHECK_EQ(r.workbench.wb_config(kind, 0), 0u);
        CHECK_EQ(r.get32(process + TC_SIGRECVD), 1u << bit);
    }
    r.put32(process + TC_SIGRECVD, 0);
    CHECK_EQ(r.workbench.wb_config(3, 0), 0u);
    CHECK_EQ(r.get32(process + TC_SIGRECVD), 0u);
}

// The icon text font IPrefs passes over draws the labels in its own font,
// pens and draw mode, as tall as its cells and from the same top line.
void an_icon_font_draws_and_lays_out_the_labels() {
    std::vector<AdfFile> files = test_files();
    files.push_back({"Fonts.info", drawer_icon_info(static_cast<i32>(kNoIconPosition),
                                                    static_cast<i32>(kNoIconPosition), 30,
                                                    50, 300, 120)});
    files.push_back({"Fonts/WideLabel.info", icon_info(kWBTool, 10, 10)});
    Rig r(files);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon_in(root, "Fonts"));
    const u32 labels = r.find_window("HombreTest:Fonts");
    CHECK(labels != Workbench::kNoWindow);
    const Workbench::DrawerWindow& w = r.workbench.window(labels);
    const u32 window = w.window;
    const u32 hasklig13 = open_hasklig(r, kData + 0x100, kData + 0x140, 13);

    struct LabelPixels {
        i32 first = -1;
        i32 last = -1;
        u32 front = 0;
        u32 back = 0;
    };
    auto label_pixels = [&]() {
        LabelPixels pixels;
        for (i32 y = w.inner_top + 25; y <= w.inner_top + 50; ++y) {
            for (i32 x = w.inner_left; x < w.inner_left + w.inner_width; ++x) {
                const u32 pen = r.window_pixel(window, x, y);
                if (pen == 2 || pen == 3) {
                    pixels.first = pixels.first < 0 ? y : pixels.first;
                    pixels.last = y;
                }
                pixels.front += pen == 2 ? 1 : 0;
                pixels.back += pen == 3 ? 1 : 0;
            }
        }
        return pixels;
    };

    const Workbench::Extent topaz = r.workbench.extent(labels);
    // The label in the built-in font, in the struct's pens and draw mode:
    // JAM2 fills every row of its eight-row cell.
    CHECK_EQ(wbconfig(r, 3, put_font_config(r, kData + 0x600, 0, JAM2, 2, 3)), 0u);
    const LabelPixels topaz_pixels = label_pixels();
    CHECK_EQ(topaz_pixels.first, w.inner_top + 26);
    CHECK_EQ(topaz_pixels.last, w.inner_top + 33);
    CHECK(topaz_pixels.front > 0);
    CHECK(topaz_pixels.back > 0);

    // hasklig 13 starts the label on the same row and is five rows taller;
    // the icon's box grows with the label and narrows with its width.
    CHECK_EQ(wbconfig(r, 3, put_font_config(r, kData + 0x700, hasklig13, JAM2, 2, 3)),
             kData + 0x600);
    CHECK_EQ(r.workbench.icon_font(), hasklig13);
    const LabelPixels hasklig_pixels = label_pixels();
    CHECK_EQ(hasklig_pixels.first, topaz_pixels.first);
    CHECK_EQ(hasklig_pixels.last, topaz_pixels.last + 5);
    CHECK(hasklig_pixels.front > 0);
    CHECK(hasklig_pixels.back > 0);
    const Workbench::Extent taller = r.workbench.extent(labels);
    CHECK_EQ(taller.max_y, topaz.max_y + 5);
    CHECK_EQ(taller.max_x, topaz.max_x - 4);
}

void start_starts_the_workbench_once() {
    Rig r;
    CHECK_EQ(r.workbench.start(0, 0), DOSTRUE);
    CHECK(r.workbench.running());
    CHECK(r.workbench.process() != 0);
    CHECK_EQ(logged_count("workbench: started"), 1u);

    const u32 process = r.workbench.process();
    CHECK_EQ(r.workbench.start(0, 0), DOSTRUE);
    CHECK_EQ(logged_count("workbench: started"), 1u);
    CHECK_EQ(r.workbench.process(), process);
}

void desktop_shows_the_volumes_and_their_menus() {
    Rig r;
    r.workbench.open_desktop();
    CHECK(r.workbench.desktop_open());
    CHECK(logged("workbench: volume HombreTest"));
    CHECK(logged("workbench: volume Ram Disk"));

    const u32 volume = r.find_icon("HombreTest");
    CHECK(volume != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(volume).type, kWBDisk);
    CHECK(r.workbench.icon(volume).object != 0);
    const u32 ram = r.find_icon("Ram Disk");
    CHECK(ram != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(ram).type, kWBDisk);

    CHECK(r.workbench.has_window(Workbench::kRootWindow));
    const Workbench::DrawerWindow& backdrop = r.workbench.window(Workbench::kRootWindow);
    CHECK(backdrop.window != 0);
    CHECK(r.window_title(backdrop.window) == "Workbench");
    CHECK_EQ(r.get16(backdrop.window + wd_LeftEdge), 0u);
    CHECK_EQ(r.get16(backdrop.window + wd_TopEdge), 14u);
    CHECK_EQ(r.get16(backdrop.window + wd_Width), 640u);
    CHECK_EQ(r.get16(backdrop.window + wd_Height), 386u);
    CHECK_EQ(r.get32(backdrop.window + 28), r.workbench.menu());  // wd_MenuStrip

    const std::string title = r.str(r.get32(r.screen + sc_Title));
    // "<free chip> graphics mem  <free fast> other mem", each a share of its region.
    const std::string prefix = "Hombre Workbench  ";
    const std::string chip_label = " graphics mem  ";
    const std::string fast_label = " other mem";
    CHECK(title.rfind(prefix, 0) == 0);
    const usize chip_end = title.find(chip_label);
    CHECK(chip_end != std::string::npos);
    const usize fast_begin = chip_end + chip_label.size();
    const usize fast_end = title.find(fast_label, fast_begin);
    CHECK(fast_end != std::string::npos);
    CHECK_EQ(fast_end + fast_label.size(), title.size());
    const usize chip_begin = prefix.size();
    const u32 chip = static_cast<u32>(std::stoul(title.substr(chip_begin, chip_end - chip_begin)));
    const u32 fast = static_cast<u32>(std::stoul(title.substr(fast_begin, fast_end - fast_begin)));
    CHECK(chip > 0 && chip <= kTestChipEnd - kFreeStart);
    CHECK(fast > 0 && fast <= kTestGuestBytes - kTestChipEnd);
}

void drawer_icons_keep_their_positions_and_use_the_grid() {
    Rig r;
    r.workbench.open_desktop();
    const u32 volume = r.find_icon("HombreTest");
    CHECK(volume != Workbench::kNoIcon);

    r.workbench.open_icon(0, volume);
    const u32 root = r.find_window("HombreTest:");
    CHECK(root != Workbench::kNoWindow);
    const u32 tools = r.find_icon("Tools");
    CHECK(tools != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(tools).type, kWBDrawer);

    r.workbench.open_icon(root, tools);
    const u32 drawer = r.find_window("HombreTest:Tools");
    CHECK(drawer != Workbench::kNoWindow);
    const u32 window = r.workbench.window(drawer).window;
    CHECK(r.window_title(window) == "Tools");
    CHECK_EQ(r.get16(window + wd_LeftEdge), 30u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 50u);
    CHECK_EQ(r.get16(window + wd_Width), 300u);
    CHECK_EQ(r.get16(window + wd_Height), 120u);
    CHECK_EQ(r.get32(window + 28), r.workbench.menu());

    const u32 tool = r.find_icon("Tool");
    CHECK(tool != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(tool).left, 100);
    CHECK_EQ(r.workbench.icon(tool).top, 40);
    const u32 put = r.find_icon("Put");
    CHECK(put != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(put).left, 20);
    CHECK_EQ(r.workbench.icon(put).top, 100);

    const u32 free = r.find_icon("Free");
    CHECK(free != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(free).left, 8);
    CHECK_EQ(r.workbench.icon(free).top, 8);

    // Opening the same drawer again brings its window forward instead of
    // opening a second one.
    r.workbench.open_icon(root, tools);
    CHECK_EQ(r.find_window("HombreTest:Tools"), drawer);
}

void clicks_select_and_double_click_opens() {
    Workbench::Clicks clicks;
    CHECK_EQ(clicks.click(3, false, 1000), Workbench::ClickAction::Select);
    CHECK(clicks.is_selected(3));
    CHECK_EQ(clicks.click(3, false, 1000 + 400000), Workbench::ClickAction::Open);
    CHECK_EQ(clicks.click(7, false, 2000000), Workbench::ClickAction::Select);
    CHECK(clicks.is_selected(7));
    CHECK_EQ(clicks.click(7, false, 2100000), Workbench::ClickAction::Open);
    // A third quick click does not open the icon a second time.
    CHECK_EQ(clicks.click(7, false, 2150000), Workbench::ClickAction::None);
    CHECK_EQ(clicks.click(7, false, 2200000), Workbench::ClickAction::Open);
    const u64 late = 2200000 + Workbench::kDoubleClickUs + 1;
    CHECK_EQ(clicks.click(7, false, late), Workbench::ClickAction::None);
    CHECK_EQ(clicks.click(7, false, late + 200000), Workbench::ClickAction::Open);
    CHECK_EQ(clicks.click(Workbench::kNoIcon, false, late + 2000000),
            Workbench::ClickAction::Clear);
    CHECK(clicks.selected().empty());
    CHECK_EQ(clicks.click(Workbench::kNoIcon, false, late + 2100000),
             Workbench::ClickAction::None);

    // A second click stops being a double click after kDoubleClickUs.
    Workbench::Clicks slow;
    CHECK_EQ(slow.click(2, false, 0), Workbench::ClickAction::Select);
    CHECK_EQ(slow.click(2, false, Workbench::kDoubleClickUs + 1), Workbench::ClickAction::None);
    const u64 base = 3 * Workbench::kDoubleClickUs;
    CHECK_EQ(slow.click(5, false, base), Workbench::ClickAction::Select);
    CHECK_EQ(slow.click(5, false, base + Workbench::kDoubleClickUs),
             Workbench::ClickAction::Open);
}

void clicks_shift_adds_and_removes_on_release() {
    Workbench::Clicks clicks;
    CHECK_EQ(clicks.click(3, false, 0), Workbench::ClickAction::Select);
    CHECK_EQ(clicks.click(7, true, 100), Workbench::ClickAction::Select);
    CHECK_EQ(clicks.selected().size(), 2u);
    CHECK_EQ(clicks.selected()[0], 3u);
    CHECK_EQ(clicks.selected()[1], 7u);

    // Shift on a selected icon takes it away when the button comes up.
    CHECK_EQ(clicks.click(7, true, 200), Workbench::ClickAction::None);
    CHECK(clicks.is_selected(7));
    CHECK(clicks.release());
    CHECK(!clicks.is_selected(7));
    CHECK(clicks.is_selected(3));
    CHECK_EQ(clicks.selected().size(), 1u);

    // A release with nothing put off changes nothing.
    CHECK(!clicks.release());
}

void clicks_plain_press_on_a_member_collapses_on_release() {
    Workbench::Clicks clicks;
    clicks.select(3);
    clicks.add(7);
    clicks.add(9);
    // The press keeps the whole selection so that it can be dragged.
    CHECK_EQ(clicks.click(7, false, 0), Workbench::ClickAction::None);
    CHECK_EQ(clicks.selected().size(), 3u);
    CHECK(clicks.release());
    CHECK_EQ(clicks.selected().size(), 1u);
    CHECK(clicks.is_selected(7));

    // A press on the only selected icon has nothing to collapse.
    CHECK_EQ(clicks.click(7, false, 2 * Workbench::kDoubleClickUs),
             Workbench::ClickAction::None);
    CHECK(!clicks.release());
    CHECK_EQ(clicks.selected().size(), 1u);
}

void clicks_shift_on_empty_keeps_the_selection() {
    Workbench::Clicks clicks;
    CHECK_EQ(clicks.click(3, false, 0), Workbench::ClickAction::Select);
    CHECK_EQ(clicks.click(Workbench::kNoIcon, true, 100), Workbench::ClickAction::None);
    CHECK(clicks.is_selected(3));
    CHECK_EQ(clicks.selected().size(), 1u);
    // Without shift the empty space clears the selection.
    CHECK_EQ(clicks.click(Workbench::kNoIcon, false, 200), Workbench::ClickAction::Clear);
    CHECK(clicks.selected().empty());
}

void clicks_forget_drops_a_pending_change() {
    Workbench::Clicks clicks;
    clicks.select(3);
    clicks.add(7);
    // Shift on a selected icon puts its removal off; forget drops that.
    CHECK_EQ(clicks.click(7, true, 0), Workbench::ClickAction::None);
    clicks.forget();
    CHECK(!clicks.release());
    CHECK_EQ(clicks.selected().size(), 2u);
    CHECK(clicks.is_selected(7));

    // A plain press on a member puts its collapse off; forget drops that.
    CHECK_EQ(clicks.click(7, false, 2 * Workbench::kDoubleClickUs),
             Workbench::ClickAction::None);
    clicks.forget();
    CHECK(!clicks.release());
    CHECK_EQ(clicks.selected().size(), 2u);

    // Forget drops the double click memory too.
    CHECK_EQ(clicks.click(3, false, 4 * Workbench::kDoubleClickUs),
             Workbench::ClickAction::None);
    clicks.forget();
    CHECK_EQ(clicks.click(3, false, 4 * Workbench::kDoubleClickUs + 100000),
             Workbench::ClickAction::None);
}

void shift_clicks_select_several_icons_in_a_drawer() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon("Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kNoWindow);
    const Workbench::DrawerWindow drawer = r.workbench.window(tools);
    const Workbench::IconEntry tool = r.workbench.icon(r.find_icon("Tool"));
    const Workbench::IconEntry proj = r.workbench.icon(r.find_icon("Proj"));

    auto image = [&](const Workbench::IconEntry& icon) {
        std::vector<u32> pens;
        for (i32 y = 0; y < 12; ++y) {
            for (i32 x = 0; x < 32; ++x) {
                pens.push_back(r.window_pixel(drawer.window, drawer.inner_left + icon.left + x,
                                              drawer.inner_top + icon.top + y));
            }
        }
        return pens;
    };
    auto inverted = [](const std::vector<u32>& plain) {
        std::vector<u32> pens;
        for (const u32 pen : plain) {
            pens.push_back(pen ^ 3u);
        }
        return pens;
    };
    const std::vector<u32> tool_plain = image(tool);
    const std::vector<u32> proj_plain = image(proj);

    // A plain click selects one icon.
    r.click(tools, tool.left + 3, tool.top + 3);
    CHECK(r.workbench.window(tools).clicks.is_selected(tool.id));
    CHECK(image(tool) == inverted(tool_plain));
    CHECK(image(proj) == proj_plain);

    // Shift adds the other one: both are drawn selected.
    g_intuition.set_qualifier(1);
    r.click(tools, proj.left + 3, proj.top + 3);
    g_intuition.set_qualifier(0);
    CHECK(r.workbench.window(tools).clicks.is_selected(proj.id));
    CHECK_EQ(r.workbench.window(tools).clicks.selected().size(), 2u);
    CHECK(image(tool) == inverted(tool_plain));
    CHECK(image(proj) != proj_plain);

    // Shift on a selected icon takes it away when the button comes up.
    g_intuition.set_qualifier(1);
    r.click(tools, proj.left + 3, proj.top + 3);
    g_intuition.set_qualifier(0);
    CHECK(!r.workbench.window(tools).clicks.is_selected(proj.id));
    CHECK(r.workbench.window(tools).clicks.is_selected(tool.id));
    CHECK(image(proj) == proj_plain);
    CHECK(image(tool) == inverted(tool_plain));

    // A plain click on empty space clears the selection.
    r.click(tools, 170, 62);
    CHECK(r.workbench.window(tools).clicks.selected().empty());
    CHECK(image(tool) == tool_plain);
}

void select_contents_selects_every_icon() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon("Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kNoWindow);
    std::vector<u32> ids;
    for (const Workbench::IconEntry& icon : r.workbench.icons()) {
        if (icon.owner == tools) {
            ids.push_back(icon.id);
        }
    }
    CHECK(ids.size() > 1u);

    // Window > Select Contents.
    r.workbench.menu_pick(tools, (4 << 5) | 1 | (31 << 11));
    const Workbench::Clicks& clicks = r.workbench.window(tools).clicks;
    CHECK_EQ(clicks.selected().size(), ids.size());
    for (const u32 id : ids) {
        CHECK(clicks.is_selected(id));
    }
}

void tool_startup_message_has_the_documented_layout() {
    Rig r;
    r.workbench.open_desktop();
    const Workbench::StartupArg args[1] = {{"HombreTest:Tools", "Tiny"}};
    u32 locks[2]{};
    u32 lock_count = 0;
    u32 bytes = 0;
    const u32 message = r.workbench.build_startup(0x1234, 0x5678, args, 1, "CON:0/0/100/50/Tiny",
                                                  locks, lock_count, bytes);
    CHECK(message != 0);
    CHECK_EQ(lock_count, 1u);
    CHECK(locks[0] != 0);

    CHECK_EQ(r.get32(message + mn_ReplyPort), r.workbench.reply_port());
    CHECK_EQ(r.get16(message + mn_Length), WBStartup_SIZEOF);
    CHECK_EQ(r.get32(message + sm_Process), 0x1234u + pr_MsgPort);
    CHECK_EQ(r.get32(message + sm_Segment), 0x5678u);
    CHECK_EQ(r.get32(message + sm_NumArgs), 1u);
    CHECK(r.str(r.get32(message + sm_ToolWindow)) == "CON:0/0/100/50/Tiny");

    const u32 arglist = r.get32(message + sm_ArgList);
    CHECK_EQ(arglist, message + WBStartup_SIZEOF);
    CHECK_EQ(r.get32(arglist + wa_Lock), locks[0]);
    CHECK(r.str(r.get32(arglist + wa_Name)) == "Tiny");
    CHECK_EQ(bytes, WBStartup_SIZEOF + 8 + 5 + 20);

    CHECK(g_dos.unlock_path(locks[0]));
}

void project_startup_names_the_tool_and_the_project() {
    Rig r;
    r.workbench.open_desktop();
    const Workbench::StartupArg args[2] = {{"HombreTest:Tools", "Tiny"}, {"HombreTest:Tools", "Proj"}};
    u32 locks[2]{};
    u32 lock_count = 0;
    u32 bytes = 0;
    const u32 message =
        r.workbench.build_startup(0x2000, 0x3000, args, 2, nullptr, locks, lock_count, bytes);
    CHECK(message != 0);
    CHECK_EQ(lock_count, 2u);
    CHECK(locks[0] != 0 && locks[1] != 0 && locks[0] != locks[1]);
    CHECK_EQ(r.get32(message + sm_NumArgs), 2u);
    CHECK_EQ(r.get32(message + sm_ToolWindow), 0u);

    const u32 arglist = r.get32(message + sm_ArgList);
    CHECK_EQ(r.get32(arglist + wa_Lock), locks[0]);
    CHECK(r.str(r.get32(arglist + wa_Name)) == "Tiny");
    CHECK_EQ(r.get32(arglist + 8 + wa_Lock), locks[1]);
    CHECK(r.str(r.get32(arglist + 8 + wa_Name)) == "Proj");
    CHECK_EQ(bytes, WBStartup_SIZEOF + 16 + 5 + 5);

    CHECK(g_dos.unlock_path(locks[0]));
    CHECK(g_dos.unlock_path(locks[1]));
}

void reply_unloads_the_seglist_and_unlocks() {
    Rig r;
    r.workbench.open_desktop();
    Workbench::IconEntry icon;
    std::strcpy(icon.file, "Tiny");
    std::strcpy(icon.name, "Tiny");
    icon.stack_size = 4096;
    CHECK(r.workbench.start_tool(icon, "HombreTest:Tools"));
    CHECK(logged("workbench: started Tiny"));

    const Workbench::RunningTool* started = r.find_tool("Tiny");
    CHECK(started != nullptr);
    const Workbench::RunningTool tool = *started;
    CHECK(tool.message != 0);
    CHECK(tool.seglist != 0);
    CHECK(tool.process != 0);
    CHECK_EQ(tool.lock_count, 1u);
    CHECK(tool.locks[0] != 0);
    const u32 message = r.get32(tool.message + sm_ArgList);
    CHECK(r.str(r.get32(message + wa_Name)) == "Tiny");

    const u32 before = r.free_bytes();
    CHECK(r.workbench.tool_reply(tool.message));
    CHECK(r.free_bytes() > before);
    CHECK(!g_dos.unlock_path(tool.locks[0]));
    CHECK(r.find_tool("Tiny") == nullptr);
    CHECK(logged("workbench: Tiny ended"));
    CHECK(!r.workbench.tool_reply(0x12345678));
}

void a_project_starts_its_default_tool() {
    Rig r;
    r.workbench.open_desktop();
    const u32 volume = r.find_icon("HombreTest");
    r.workbench.open_icon(0, volume);
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon("Tools"));
    const u32 tools_window = r.find_window("HombreTest:Tools");
    CHECK(tools_window != Workbench::kNoWindow);

    const u32 project = r.find_icon("Proj");
    CHECK(project != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(project).type, kWBProject);
    r.workbench.open_icon(tools_window, project);

    const Workbench::RunningTool* started = r.find_tool("Tiny");
    CHECK(started != nullptr);
    const Workbench::RunningTool tool = *started;
    const u32 arglist = r.get32(tool.message + sm_ArgList);
    CHECK_EQ(r.get32(tool.message + sm_NumArgs), 2u);
    CHECK(r.str(r.get32(arglist + wa_Name)) == "Tiny");
    CHECK(r.str(r.get32(arglist + 8 + wa_Name)) == "Proj");
    CHECK(r.workbench.tool_reply(tool.message));
}

void a_bare_default_tool_is_found_on_the_command_path() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    r.workbench.open_icon(r.find_window("HombreTest:"), r.find_icon("Tools"));
    const u32 tools_window = r.find_window("HombreTest:Tools");
    r.workbench.open_icon(tools_window, r.find_icon("Lost"));
    const Workbench::RunningTool* started = r.find_tool("Finder");
    CHECK(started != nullptr);
    CHECK(r.workbench.tool_reply(started->message));
}

void a_leading_slash_default_tool_is_in_the_parent_drawer() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    r.workbench.open_icon(r.find_window("HombreTest:"), r.find_icon("Tools"));
    r.workbench.open_icon(r.find_window("HombreTest:Tools"), r.find_icon("Up"));
    const Workbench::RunningTool* started = r.find_tool("Tiny2");
    CHECK(started != nullptr);
    CHECK(r.workbench.tool_reply(started->message));
}

// A disk changed in a drive changes the icon on the desktop: the old disk's
// icon and open window go and the new disk's icon comes; an empty drive has
// no icon.
void a_swapped_disk_changes_the_desktop() {
    Rig r(test_files(), build_adf("Second", false, test_files()), 1);
    r.workbench.open_desktop();
    const u32 second = r.find_icon("Second");
    CHECK(second != Workbench::kNoIcon);
    r.workbench.open_icon(0, second);
    CHECK(r.find_window("Second:") != Workbench::kNoWindow);

    std::vector<u8> third = build_adf("Third", false, test_files());
    AdfVolume third_volume{};
    CHECK(adf_mount(third.data(), third.size(), third_volume) == FsError::Ok);
    CHECK(g_dos.replace_disk(1, &third_volume));
    r.workbench.refresh_volumes();
    CHECK_EQ(r.find_icon("Second"), Workbench::kNoIcon);
    CHECK(r.find_icon("Third") != Workbench::kNoIcon);
    CHECK_EQ(r.find_window("Second:"), Workbench::kNoWindow);
    CHECK(r.find_icon("HombreTest") != Workbench::kNoIcon);

    // Nothing changed: the icons stay as they are.
    const u32 third_icon = r.find_icon("Third");
    r.workbench.refresh_volumes();
    CHECK_EQ(r.find_icon("Third"), third_icon);

    CHECK(g_dos.replace_disk(1, nullptr));
    r.workbench.refresh_volumes();
    CHECK_EQ(r.find_icon("Third"), Workbench::kNoIcon);
    CHECK(r.find_icon("Ram Disk") != Workbench::kNoIcon);
}

// The DiskObject an AppIcon shows stays when the system monitor ends the
// program it came from: the Workbench still draws it.
void an_app_icons_object_outlives_an_ended_program() {
    Rig r;
    r.workbench.open_desktop();
    // A one-segment seglist holding the code at 0x1800 the call returns to.
    r.put32(0x17F8, 0x808);
    r.put32(0x17FC, 0);
    const u32 task = g_exec.this_task();
    CHECK(g_exec.begin_program(task, 0x17FC >> 2, true) != 0);
    const u32 object = g_icon.default_object(kWBDisk);
    CHECK(object != 0);
    r.put32(kStack - 4, 0x1804);
    r.m.cpu.a[7] = kStack - 4;
    g_exec.claim_for_program(object, 64);
    r.m.cpu.a[7] = kStack;
    const u32 name = kData + 0x400;
    for (u32 i = 0; i < 5; ++i) {
        r.put8(name + i, static_cast<u32>("App\0"[i]));
    }
    CHECK(r.workbench.add_app_icon(9, 0x22, name, 0, 0, object, 0) != 0);

    r.put16(0x1000, 0x60FE);
    g_jit.flush();
    g_exec.request_rescue(Rescue::End, static_cast<u32>(g_exec.scheduler().current()), task);
    r.m.cpu.pc = 0x1000;
    CHECK_EQ(g_jit.run(100), ExitReason::Stopped);
    g_exec.end_program(task);
    CHECK(logged("exec: freed 0 blocks, 0 bytes, of an ended program; kept 1"));
}

void app_registrations_add_and_remove() {
    Rig r;
    r.workbench.open_desktop();
    const u32 window = r.workbench.add_app_window(7, 0x11, 0, 0, 0);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window), 0u);
    CHECK_EQ(r.workbench.apps().size(), 1u);
    CHECK_EQ(r.workbench.apps()[0].id, 7u);
    CHECK_EQ(r.workbench.apps()[0].user, 0x11u);
    CHECK(r.workbench.remove_app_window(window));
    CHECK(r.workbench.apps().empty());
    CHECK(!r.workbench.remove_app_window(window));

    const u32 object = g_icon.default_object(kWBDisk);
    CHECK(object != 0);
    const u32 name = kData + 0x400;
    for (u32 i = 0; i < 5; ++i) {
        r.put8(name + i, static_cast<u32>("App\0"[i]));
    }
    const u32 app_icon = r.workbench.add_app_icon(9, 0x22, name, 0, 0, object, 0);
    CHECK(app_icon != 0);
    const u32 icon = r.find_icon("App");
    CHECK(icon != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(icon).type, Workbench::kAppIcon);
    r.workbench.remove_app_icon(app_icon);
    CHECK_EQ(r.find_icon("App"), Workbench::kNoIcon);
    g_icon.free_object(object);

    const u32 item = r.workbench.add_app_menu_item(11, 0x33, 0, 0, 0);
    CHECK(item != 0);
    r.workbench.remove_app_menu_item(item);
    CHECK(r.workbench.apps().empty());
}

void app_vectors_take_their_arguments_from_the_documented_registers() {
    Rig r;
    r.workbench.open_desktop();
    const u32 object = g_icon.default_object(kWBDisk);
    CHECK(object != 0);
    const u32 name = kData + 0x400;
    for (u32 i = 0; i < 6; ++i) {
        r.put8(name + i, static_cast<u32>("Hatch"[i]));
    }

    // AddAppIconA: the lock, which is always 0, is in A2 and the
    // DiskObject in A3.
    r.m.cpu.d[0] = 9;
    r.m.cpu.d[1] = 0x22;
    r.m.cpu.d[2] = 0x7777;
    r.m.cpu.a[0] = name;
    r.m.cpu.a[1] = kData + 0x500;
    r.m.cpu.a[2] = 0;
    r.m.cpu.a[3] = object;
    r.m.cpu.a[4] = 0;
    const u32 app_icon = r.result(r.workbench.base(), workbench_lvo::AddAppIconA);
    CHECK(app_icon != 0);
    const u32 icon = r.find_icon("Hatch");
    CHECK(icon != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(icon).object, object);
    CHECK_EQ(r.workbench.apps()[0].id, 9u);
    CHECK_EQ(r.workbench.apps()[0].user, 0x22u);

    // RemoveAppIcon is TRUE for the handle and FALSE once it is gone.
    r.m.cpu.a[0] = app_icon;
    CHECK_EQ(r.result(r.workbench.base(), workbench_lvo::RemoveAppIcon), 1u);
    CHECK_EQ(r.find_icon("Hatch"), Workbench::kNoIcon);
    r.m.cpu.a[0] = app_icon;
    CHECK_EQ(r.result(r.workbench.base(), workbench_lvo::RemoveAppIcon), 0u);

    // Without a DiskObject there is no AppIcon.
    r.m.cpu.a[0] = name;
    r.m.cpu.a[2] = object;
    r.m.cpu.a[3] = 0;
    CHECK_EQ(r.result(r.workbench.base(), workbench_lvo::AddAppIconA), 0u);

    r.m.cpu.d[0] = 3;
    r.m.cpu.d[1] = 4;
    r.m.cpu.a[0] = 0;
    r.m.cpu.a[1] = 0;
    r.m.cpu.a[2] = 0;
    const u32 app_window = r.result(r.workbench.base(), workbench_lvo::AddAppWindowA);
    CHECK(app_window != 0);
    r.m.cpu.a[0] = app_window;
    CHECK_EQ(r.result(r.workbench.base(), workbench_lvo::RemoveAppMenuItem), 0u);
    r.m.cpu.a[0] = app_window;
    CHECK_EQ(r.result(r.workbench.base(), workbench_lvo::RemoveAppWindow), 1u);
    g_icon.free_object(object);
}

void a_drawer_window_keeps_its_title_when_the_memory_it_came_from_is_reused() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    CHECK(root != Workbench::kNoWindow);
    const u32 window = r.workbench.window(root).window;
    // Blocks of the size the title was built in, filled with other bytes.
    for (u32 i = 0; i < 16; ++i) {
        const u32 block = g_exec.alloc(11, MEMF_PUBLIC);
        CHECK(block != 0);
        for (u32 k = 0; k < 11; ++k) {
            r.put8(block + k, '#');
        }
    }
    CHECK(r.window_title(window) == "HombreTest");
    CHECK(r.window_title(r.workbench.window(0).window) == "Workbench");
}

void open_parent_opens_the_drawer_above() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon("Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kNoWindow);
    r.workbench.open_icon(tools, r.find_icon("Sub"));
    const u32 sub = r.find_window("HombreTest:Tools/Sub");
    CHECK(sub != Workbench::kNoWindow);
    CHECK(r.find_icon("Deep") != Workbench::kNoIcon);

    // With the drawers above closed, Open Parent opens each again at the
    // place its icon gives it.
    r.workbench.close_drawer(tools);
    r.workbench.close_drawer(root);
    CHECK_EQ(r.find_window("HombreTest:Tools"), Workbench::kNoWindow);
    r.workbench.open_parent(sub);
    const u32 tools_again = r.find_window("HombreTest:Tools");
    CHECK(tools_again != Workbench::kNoWindow);
    const u32 tools_window = r.workbench.window(tools_again).window;
    CHECK(r.window_title(tools_window) == "Tools");
    CHECK_EQ(r.get16(tools_window + wd_LeftEdge), 30u);
    CHECK_EQ(r.get16(tools_window + wd_TopEdge), 50u);
    CHECK_EQ(r.get16(tools_window + wd_Width), 300u);
    CHECK(r.find_icon("Proj") != Workbench::kNoIcon);

    r.workbench.open_parent(tools_again);
    const u32 root_again = r.find_window("HombreTest:");
    CHECK(root_again != Workbench::kNoWindow);
    CHECK(r.window_title(r.workbench.window(root_again).window) == "HombreTest");

    // An open parent only comes forward, and a disk has none.
    const usize open_before = r.workbench.windows().size();
    r.workbench.open_parent(sub);
    r.workbench.open_parent(root_again);
    r.workbench.open_parent(0);
    CHECK_EQ(r.workbench.windows().size(), open_before);
}

void a_click_shows_the_icon_selected_and_a_rescan_drops_the_selection() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon("Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kNoWindow);
    const Workbench::DrawerWindow drawer = r.workbench.window(tools);
    const Workbench::IconEntry tool = r.workbench.icon(r.find_icon("Tool"));

    auto image = [&]() {
        std::vector<u32> pens;
        for (i32 y = 0; y < 12; ++y) {
            for (i32 x = 0; x < 32; ++x) {
                pens.push_back(r.window_pixel(drawer.window, drawer.inner_left + tool.left + x,
                                              drawer.inner_top + tool.top + y));
            }
        }
        return pens;
    };
    const std::vector<u32> plain = image();

    // A mark where no icon is: a click must paint only the icons it
    // changes, so the mark stays.
    const i32 mark_x = drawer.inner_left + 170;
    const i32 mark_y = drawer.inner_top + 62;
    g_graphics.set_pens(drawer.rp, 3, 0, ndk::JAM1);
    g_graphics.rect_fill(drawer.rp, mark_x, mark_y, mark_x, mark_y);
    CHECK_EQ(r.window_pixel(drawer.window, mark_x, mark_y), 3u);

    // The icon has no second image: selected, every pen of it is inverted.
    r.click(tools, tool.left + 3, tool.top + 3);
    const std::vector<u32> selected = image();
    CHECK(selected != plain);
    for (usize i = 0; i < plain.size(); ++i) {
        CHECK_EQ(selected[i], plain[i] ^ 3u);
    }
    CHECK_EQ(r.window_pixel(drawer.window, mark_x, mark_y), 3u);

    // A click beside every icon lets it go.
    r.click(tools, 170, 62);
    CHECK(image() == plain);
    CHECK_EQ(r.window_pixel(drawer.window, mark_x, mark_y), 3u);
    r.click(tools, tool.left + 3, tool.top + 3);

    // Rescanning the drawer draws it unselected again.
    r.put8(kData + 0x400, 0);
    const char path[] = "HombreTest:Tools";
    for (u32 i = 0; i < sizeof path; ++i) {
        r.put8(kData + 0x400 + i, static_cast<u32>(path[i]));
    }
    CHECK(r.workbench.update_workbench(kData + 0x400));
    CHECK(image() == plain);

    // A click on the icon and one beside it: selected, then cleared.
    r.click(tools, tool.left + 3, tool.top + 3);
    CHECK(image() == selected);
    r.click(tools, 250, 5);
    CHECK(image() == plain);
}

void the_desktop_waits_on_the_signals_of_its_ports() {
    Rig r;
    r.workbench.open_desktop();
    const u32 reply_bit = r.get8(r.workbench.reply_port() + mp_SigBit);
    const u32 window_bit = r.get8(r.workbench.window_port() + mp_SigBit);
    CHECK(reply_bit != window_bit);
    CHECK_EQ(r.workbench.wait_mask(), (1u << reply_bit) | (1u << window_bit));
    CHECK_EQ(r.get32(r.workbench.window(0).window + wd_UserPort), r.workbench.window_port());

    // Every window of the desktop uses the one port, so another window adds
    // no signal to wait on.
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    CHECK_EQ(r.get32(r.workbench.window(root).window + wd_UserPort), r.workbench.window_port());
    CHECK_EQ(r.workbench.wait_mask(), (1u << reply_bit) | (1u << window_bit));

    // Nothing waits at the ports once they are read; a click in the
    // window raises its signal again.
    r.workbench.pump();
    g_exec.wait_for(r.workbench.wait_mask(), 0);
    CHECK_EQ(g_exec.wait_for(r.workbench.wait_mask(), 0), 0u);
    const Workbench::DrawerWindow& w = r.workbench.window(root);
    const i32 x = static_cast<i32>(r.get16(w.window + wd_LeftEdge)) + w.inner_left + 250;
    const i32 y = static_cast<i32>(r.get16(w.window + wd_TopEdge)) + w.inner_top + 100;
    g_intuition.pointer_event(x, y, 1, 1, 0, true);
    CHECK_EQ(g_exec.wait_for(r.workbench.wait_mask(), 0), 1u << window_bit);
    CHECK(!r.workbench.pump());

    // A message still queued for a window goes with it when it closes, and
    // the port stays.
    g_intuition.pointer_event(x, y, 0, 0, 1, false);
    CHECK_EQ(g_exec.wait_for(r.workbench.wait_mask(), 0), 1u << window_bit);
    r.workbench.close_drawer(root);
    CHECK_EQ(g_exec.get_message(r.workbench.window_port()), 0u);
    CHECK_EQ(r.workbench.wait_mask(), (1u << reply_bit) | (1u << window_bit));
    CHECK(!r.workbench.pump());
}

void a_resized_drawer_is_measured_again() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    const Workbench::DrawerWindow before = r.workbench.window(root);
    g_intuition.size_window_to(before.window, 400, 200);
    r.workbench.pump();
    const Workbench::DrawerWindow& after = r.workbench.window(root);
    CHECK_EQ(after.inner_width, before.inner_width + 400 - kDefaultDrawerWidth);
    CHECK_EQ(after.inner_height, before.inner_height + 200 - kDefaultDrawerHeight);
}

void the_startup_message_signals_the_started_process() {
    Rig r;
    r.workbench.open_desktop();
    Workbench::IconEntry icon;
    std::strcpy(icon.file, "Tiny");
    std::strcpy(icon.name, "Tiny");
    icon.stack_size = 4096;
    CHECK(r.workbench.start_tool(icon, "HombreTest:Tools"));
    const Workbench::RunningTool tool = *r.find_tool("Tiny");

    // The message is the one on the process's port, and the port's signal
    // is set in the task for a program that waits for it.
    const u32 port = tool.process + pr_MsgPort;
    CHECK_EQ(r.get32(port + mp_MsgList), tool.message);
    CHECK_EQ(r.get32(tool.process + TC_SIGRECVD) & (1u << r.get8(port + mp_SigBit)),
             1u << r.get8(port + mp_SigBit));
    for (const std::string& line : g_lines) {
        CHECK(line.find("Signal to unknown task") == std::string::npos);
    }
    CHECK(r.workbench.tool_reply(tool.message));
}

void a_program_that_ends_without_replying_gives_back_what_it_held() {
    Rig r;
    r.workbench.open_desktop();
    Workbench::IconEntry icon;
    std::strcpy(icon.file, "Tiny");
    std::strcpy(icon.name, "Tiny");
    icon.stack_size = 4096;
    const u32 before = r.free_bytes();
    CHECK(r.workbench.start_tool(icon, "HombreTest:Tools"));
    const Workbench::RunningTool tool = *r.find_tool("Tiny");

    // While its process lives the program keeps its record.
    r.workbench.reap_tools();
    CHECK(r.find_tool("Tiny") != nullptr);

    // The process is removed as a faulted one is, its message unanswered.
    r.m.cpu.a[1] = tool.process;
    CHECK_EQ(r.call(g_exec.exec_base(), exec_lvo::RemTask), ExitReason::Halt);
    r.workbench.pump();
    CHECK(r.find_tool("Tiny") == nullptr);
    CHECK(logged("workbench: Tiny ended without a reply"));
    CHECK(!g_dos.unlock_path(tool.locks[0]));
    CHECK_EQ(r.free_bytes(), before);
}

void menu_strip_lists_the_titles_and_disabled_items() {
    Rig r;
    r.workbench.open_desktop();
    u32 menu = r.workbench.menu();
    CHECK(menu != 0);
    const char* titles[] = {"Workbench", "Window", "Icons", "Tools"};
    for (const char* want : titles) {
        CHECK(menu != 0);
        CHECK(r.str(r.get32(menu + mu_MenuName)) == want);
        CHECK_EQ(r.get16(menu + mu_Flags), MENUENABLED);
        menu = r.get32(menu + mu_NextMenu);
    }
    CHECK_EQ(menu, 0u);

    const u32 first = r.workbench.menu();
    const u32 windows = r.get32(first + mu_NextMenu);
    const u32 icons = r.get32(windows + mu_NextMenu);
    CHECK(windows != 0 && icons != 0);

    const u32 new_drawer = r.get32(windows + mu_FirstItem);
    const u32 open_parent = r.get32(new_drawer + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(new_drawer + mi_ItemFill) + it_IText)) == "New Drawer");
    CHECK_EQ(r.get16(new_drawer + mi_Flags) & ITEMENABLED, ITEMENABLED);
    CHECK(r.str(r.get32(r.get32(open_parent + mi_ItemFill) + it_IText)) == "Open Parent");
    CHECK_EQ(r.get16(open_parent + mi_Flags) & ITEMENABLED, ITEMENABLED);
    CHECK_EQ(r.get16(open_parent + mi_Flags) & ITEMTEXT, ITEMTEXT);

    const u32 open = r.get32(icons + mu_FirstItem);
    CHECK(r.str(r.get32(r.get32(open + mi_ItemFill) + it_IText)) == "Open");
    CHECK_EQ(r.get16(open + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 copy = r.get32(open + mi_NextItem);
    CHECK_EQ(r.get16(copy + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 rename = r.get32(copy + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(rename + mi_ItemFill) + it_IText)) == "Rename...");
    CHECK_EQ(r.get16(rename + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 information = r.get32(rename + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(information + mi_ItemFill) + it_IText)) == "Information...");
    CHECK_EQ(r.get16(information + mi_Flags) & ITEMENABLED, ITEMENABLED);

    const u32 workbench_items = r.get32(first + mu_FirstItem);
    const u32 redraw_all = r.get32(r.get32(workbench_items + mi_NextItem) + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(redraw_all + mi_ItemFill) + it_IText)) == "Redraw All");
    CHECK_EQ(r.get16(redraw_all + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 update_all = r.get32(redraw_all + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(update_all + mi_ItemFill) + it_IText)) == "Update All");
    CHECK_EQ(r.get16(update_all + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 last_message = r.get32(update_all + mi_NextItem);
    CHECK_EQ(r.get16(last_message + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 about = r.get32(last_message + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(about + mi_ItemFill) + it_IText)) == "About...");
    CHECK_EQ(r.get16(about + mi_Flags) & ITEMENABLED, ITEMENABLED);
    CHECK_EQ(r.get16(about + mi_Flags) & ITEMTEXT, ITEMTEXT);

    const u32 update = r.get32(r.get32(open_parent + mi_NextItem) + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(update + mi_ItemFill) + it_IText)) == "Update");
    CHECK_EQ(r.get16(update + mi_Flags) & ITEMENABLED, ITEMENABLED);

    // Window > Snapshot opens Window and All; Icons > Snapshot and
    // Unsnapshot are on.
    u32 window_item = r.get32(windows + mu_FirstItem);
    for (u32 i = 0; i < 6; ++i) {
        window_item = r.get32(window_item + mi_NextItem);
    }
    CHECK(r.str(r.get32(r.get32(window_item + mi_ItemFill) + it_IText)) == "Snapshot");
    CHECK_EQ(r.get16(window_item + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 sub_window = r.get32(window_item + mi_SubItem);
    CHECK(sub_window != 0);
    CHECK(r.str(r.get32(r.get32(sub_window + mi_ItemFill) + it_IText)) == "Window");
    const u32 sub_all = r.get32(sub_window + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(sub_all + mi_ItemFill) + it_IText)) == "All");
    CHECK_EQ(r.get32(sub_all + mi_NextItem), 0u);
    u32 icon_item = r.get32(icons + mu_FirstItem);
    for (u32 i = 0; i < 4; ++i) {
        icon_item = r.get32(icon_item + mi_NextItem);
    }
    CHECK(r.str(r.get32(r.get32(icon_item + mi_ItemFill) + it_IText)) == "Snapshot");
    CHECK_EQ(r.get16(icon_item + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 unsnapshot = r.get32(icon_item + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(unsnapshot + mi_ItemFill) + it_IText)) == "Unsnapshot");
    CHECK_EQ(r.get16(unsnapshot + mi_Flags) & ITEMENABLED, ITEMENABLED);
}

void backdrop_toggles_the_root_window() {
    Rig r;
    r.workbench.open_desktop();
    CHECK(r.workbench.backdrop());
    const u32 backdrop_window = r.workbench.window(0).window;
    CHECK_EQ(r.get32(backdrop_window + wd_Flags) & WFLG_BACKDROP, WFLG_BACKDROP);

    const u32 item = r.get32(r.workbench.menu() + mu_FirstItem);
    CHECK(r.str(r.get32(r.get32(item + mi_ItemFill) + it_IText)) == "Backdrop");
    const u32 flags = r.get16(item + mi_Flags);
    CHECK_EQ(flags & ITEMENABLED, ITEMENABLED);
    CHECK_EQ(flags & (CHECKIT | MENUTOGGLE | CHECKED), static_cast<u32>(CHECKIT | MENUTOGGLE | CHECKED));

    const u32 volume = r.find_icon("HombreTest");
    const u32 ram = r.find_icon("Ram Disk");
    const i32 volume_left = r.workbench.icon(volume).left;
    const i32 volume_top = r.workbench.icon(volume).top;
    const i32 ram_left = r.workbench.icon(ram).left;
    const i32 ram_top = r.workbench.icon(ram).top;

    r.workbench.open_icon(0, volume);
    const u32 drawer = r.find_window("HombreTest:");
    CHECK(drawer != Workbench::kNoWindow);
    const u32 drawer_window = r.workbench.window(drawer).window;
    const u32 before = r.free_bytes();

    // Intuition turns the check mark off before the pick reaches the
    // desktop.
    r.put16(item + mi_Flags, r.get16(item + mi_Flags) & ~CHECKED);
    r.workbench.menu_pick(0, 0);
    CHECK(!r.workbench.backdrop());
    CHECK(logged("workbench: backdrop off"));
    CHECK(r.workbench.has_window(Workbench::kRootWindow));
    const Workbench::DrawerWindow& root = r.workbench.window(Workbench::kRootWindow);
    CHECK(r.window_title(root.window) == "Workbench");
    const u32 window_flags = r.get32(root.window + wd_Flags);
    CHECK_EQ(window_flags & (WFLG_BACKDROP | WFLG_BORDERLESS | WFLG_CLOSEGADGET), 0u);
    CHECK_EQ(window_flags & (WFLG_SIZEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET),
             WFLG_SIZEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET);
    CHECK(root.inner_left > 0 && root.inner_top > 0);
    CHECK_EQ(r.get32(root.window + 28), r.workbench.menu());  // wd_MenuStrip
    CHECK_EQ(r.get16(item + mi_Flags) & CHECKED, 0u);
    CHECK_EQ(r.find_icon("HombreTest"), volume);
    CHECK_EQ(r.find_icon("Ram Disk"), ram);
    for (u32 icon : {volume, ram}) {
        const Workbench::IconEntry& entry = r.workbench.icon(icon);
        CHECK(entry.left >= 0 && entry.left + entry.width <= root.inner_width);
        CHECK(entry.top >= 0 && entry.top + entry.height <= root.inner_height);
    }
    CHECK_EQ(r.find_window("HombreTest:"), drawer);
    CHECK_EQ(r.workbench.window(drawer).window, drawer_window);
    CHECK(r.str(r.get32(r.screen + sc_Title)).rfind("Hombre Workbench", 0) == 0);

    // A double click on the disk in the window finds its drawer open.
    const Workbench::IconEntry& disk = r.workbench.icon(volume);
    r.click(0, disk.left + 1, disk.top + 1);
    r.click(0, disk.left + 1, disk.top + 1);
    CHECK_EQ(r.find_window("HombreTest:"), drawer);
    CHECK_EQ(r.workbench.window(drawer).window, drawer_window);

    // A second pick off the menu is not needed to come back.
    r.workbench.set_backdrop(false);
    CHECK(!r.workbench.backdrop());
    r.workbench.set_backdrop(true);
    CHECK(r.workbench.backdrop());
    CHECK(logged("workbench: backdrop on"));
    const Workbench::DrawerWindow& again = r.workbench.window(0);
    CHECK_EQ(r.get32(again.window + wd_Flags) & WFLG_BACKDROP, WFLG_BACKDROP);
    CHECK_EQ(r.get16(again.window + wd_Width), 640u);
    CHECK_EQ(r.get16(again.window + wd_Height), 386u);
    CHECK_EQ(r.get16(item + mi_Flags) & CHECKED, CHECKED);
    CHECK_EQ(r.workbench.icon(volume).left, volume_left);
    CHECK_EQ(r.workbench.icon(volume).top, volume_top);
    CHECK_EQ(r.workbench.icon(ram).left, ram_left);
    CHECK_EQ(r.workbench.icon(ram).top, ram_top);
    CHECK_EQ(r.free_bytes(), before);
}



// The ADF's drawer and the Ram Disk's window side by side.
struct TwoWindows {
    u32 tools = Workbench::kNoWindow;
    u32 ram = Workbench::kNoWindow;
};

TwoWindows open_tools_and_ram(Rig& r) {
    TwoWindows w;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon_in(root, "Tools"));
    w.tools = r.find_window("HombreTest:Tools");
    CHECK(w.tools != Workbench::kNoWindow);
    r.workbench.close_drawer(root);
    g_intuition.move_window_to(r.workbench.window(w.tools).window, 20, 30);
    r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
    w.ram = r.find_window("Ram Disk:");
    CHECK(w.ram != Workbench::kNoWindow);
    g_intuition.move_window_to(r.workbench.window(w.ram).window, 330, 200);
    return w;
}

void put_ram_project(const std::string& name) {
    u32 error = 0;
    const u8 byte = 'x';
    CHECK(g_dos.write_file(("RAM:" + name).c_str(), &byte, 1, error));
    const u32 object = g_icon.default_object(kWBProject);
    CHECK(object != 0);
    CHECK(g_icon.save_object(("RAM:" + name).c_str(), object, error));
    g_icon.free_object(object);
}

// A project icon with a default tool and tool types; with_object adds the
// file it stands for, without it the icon has only the .info.
void put_ram_typed(const std::string& name, const std::vector<std::string>& types,
                   bool with_object = true) {
    u32 error = 0;
    const std::vector<u8> info = typed_project_info("OldTool", types, 4096, 10, 10);
    CHECK(g_dos.write_file(("RAM:" + name + ".info").c_str(), info.data(),
                           static_cast<u32>(info.size()), error));
    if (with_object) {
        const u8 byte = 'x';
        CHECK(g_dos.write_file(("RAM:" + name).c_str(), &byte, 1, error));
    }
}

// One click with Shift held at x, y of the window's inner area.
void shift_click_at(Rig& r, u32 window, i32 x, i32 y) {
    g_intuition.set_qualifier(1);
    r.click(window, x, y);
    g_intuition.set_qualifier(0);
}

void an_icon_dragged_in_its_window_takes_the_new_place() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 window = r.workbench.window(w.tools).window;

    // A press with a pixel of movement stays a click.
    const u32 tool = r.find_icon_in(w.tools, "Tool");
    r.press(w.tools, 103, 43);
    CHECK(r.workbench.drag().armed);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, WFLG_REPORTMOUSE);
    r.move_held(r.screen_x(w.tools, 104), r.screen_y(w.tools, 43));
    CHECK(!r.workbench.drag().active);
    r.release(r.screen_x(w.tools, 104), r.screen_y(w.tools, 43));
    CHECK(!r.workbench.drag().armed);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, 0u);
    CHECK_EQ(r.workbench.icon(tool).left, 100);
    CHECK_EQ(r.workbench.icon(tool).top, 40);

    // A drag moves the icon by where the pointer went.
    const u32 proj = r.find_icon_in(w.tools, "Proj");
    r.press(w.tools, 63, 83);
    r.move_held(r.screen_x(w.tools, 120), r.screen_y(w.tools, 30));
    CHECK(r.workbench.drag().active);
    r.release(r.screen_x(w.tools, 160), r.screen_y(w.tools, 23));
    CHECK(!r.workbench.drag().armed);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, 0u);
    CHECK_EQ(r.workbench.icon(proj).left, 157);
    CHECK_EQ(r.workbench.icon(proj).top, 20);
    CHECK(r.workbench.icon(proj).placed);
    // Only in memory: the disk keeps the old place.
    CHECK(r.saved_place("HombreTest:Tools/Proj") == std::make_pair(60u, 80u));
}

void a_dragged_file_is_copied_moved_or_refused() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);

    // From the disk to the Ram Disk: a copy, the icon where it was dropped.
    r.drag(w.tools, 63, 83, r.screen_x(w.ram, 153), r.screen_y(w.ram, 43));
    CHECK(logged("workbench: copied HombreTest:Tools/Proj to Ram Disk:"));
    CHECK(r.exists("RAM:Proj"));
    CHECK(r.exists("RAM:Proj.info"));
    CHECK(r.exists("HombreTest:Tools/Proj"));
    CHECK(r.saved_place("RAM:Proj") == std::make_pair(150u, 40u));
    const u32 copy = r.find_icon_in(w.ram, "Proj");
    CHECK(copy != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(copy).left, 150);
    CHECK_EQ(r.workbench.icon(copy).top, 40);
    CHECK(r.find_icon_in(w.tools, "Proj") != Workbench::kNoIcon);

    // Back to the disk: it cannot be written, and nothing changes.
    r.drag(w.ram, 153, 43, r.screen_x(w.tools, 200), r.screen_y(w.tools, 20));
    CHECK(logged("workbench: cannot copy Proj: Disk is write-protected"));
    CHECK(r.exists("RAM:Proj"));
    CHECK_EQ(r.workbench.icon(r.find_icon_in(w.ram, "Proj")).left, 150);

    // Onto a drawer of the same volume: a move, the icon without a place.
    r.make_dir("RAM:Box");
    u32 error = 0;
    const u32 drawer = g_icon.default_object(kWBDrawer);
    CHECK(g_icon.save_object("RAM:Box", drawer, error));
    g_icon.free_object(drawer);
    r.rescan("Ram Disk:");
    const Workbench::IconEntry box = r.workbench.icon(r.find_icon_in(w.ram, "Box"));
    r.drag(w.ram, 153, 43, r.screen_x(w.ram, box.left + 3), r.screen_y(w.ram, box.top + 3));
    CHECK(logged("workbench: moved Ram Disk:Proj to Ram Disk:Box"));
    CHECK(!r.exists("RAM:Proj"));
    CHECK(!r.exists("RAM:Proj.info"));
    CHECK(r.exists("RAM:Box/Proj"));
    CHECK(r.saved_place("RAM:Box/Proj") == std::make_pair(kNoIconPosition, kNoIconPosition));
    CHECK_EQ(r.find_icon_in(w.ram, "Proj"), Workbench::kNoIcon);

    // A drawer is no file to move.
    r.drag(w.tools, 203, 23, r.screen_x(w.ram, 200), r.screen_y(w.ram, 60));
    CHECK(logged("workbench: Sub cannot be moved there"));
}

void a_drag_ends_when_its_window_is_filled_again() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 window = r.workbench.window(w.tools).window;
    r.press(w.tools, 63, 83);
    r.move_held(r.screen_x(w.tools, 120), r.screen_y(w.tools, 30));
    CHECK(r.workbench.drag().active);
    r.rescan("HombreTest:Tools");
    CHECK(!r.workbench.drag().armed);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, 0u);
    r.release(r.screen_x(w.tools, 120), r.screen_y(w.tools, 30));
    const u32 proj = r.find_icon_in(w.tools, "Proj");
    CHECK_EQ(r.workbench.icon(proj).left, 60);
    CHECK_EQ(r.workbench.icon(proj).top, 80);
}

void a_box_selects_the_icons_it_touches() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 window = r.workbench.window(w.tools).window;
    const u32 free = r.find_icon_in(w.tools, "Free");
    const u32 tool = r.find_icon_in(w.tools, "Tool");
    const i32 corner_x = r.screen_x(w.tools, 150);
    const i32 corner_y = r.screen_y(w.tools, 60);
    const u32 plain = r.screen_pixel(r.screen, corner_x, corner_y);

    // A box from the empty corner over Free and Tool.
    r.press(w.tools, 150, 60);
    r.move_held(r.screen_x(w.tools, 5), r.screen_y(w.tools, 5));
    CHECK(r.workbench.box().active);
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected().size(), 2u);
    CHECK(r.workbench.window(w.tools).clicks.is_selected(free));
    CHECK(r.workbench.window(w.tools).clicks.is_selected(tool));
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, WFLG_REPORTMOUSE);
    CHECK(r.screen_pixel(r.screen, corner_x, corner_y) != plain);

    // Shrinking the box drops the icon it left.
    r.move_held(r.screen_x(w.tools, 5), r.screen_y(w.tools, 45));
    CHECK(r.workbench.window(w.tools).clicks.is_selected(tool));
    CHECK(!r.workbench.window(w.tools).clicks.is_selected(free));
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected().size(), 1u);

    // The release takes the box away and leaves the selection.
    r.release(r.screen_x(w.tools, 5), r.screen_y(w.tools, 45));
    CHECK(!r.workbench.box().armed);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, 0u);
    CHECK(r.workbench.window(w.tools).clicks.is_selected(tool));
    CHECK_EQ(r.screen_pixel(r.screen, corner_x, corner_y), plain);
}

void a_box_adds_to_the_selection_with_shift() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 tool = r.find_icon_in(w.tools, "Tool");
    const u32 free = r.find_icon_in(w.tools, "Free");
    r.click(w.tools, 103, 43);
    CHECK(r.workbench.window(w.tools).clicks.is_selected(tool));

    g_intuition.set_qualifier(1);
    r.press(w.tools, 50, 30);
    g_intuition.set_qualifier(0);
    r.move_held(r.screen_x(w.tools, 5), r.screen_y(w.tools, 5));
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected().size(), 2u);
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected()[0], tool);
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected()[1], free);
    CHECK(r.workbench.window(w.tools).clicks.is_selected(tool));
    CHECK(r.workbench.window(w.tools).clicks.is_selected(free));
    r.release(r.screen_x(w.tools, 5), r.screen_y(w.tools, 5));
}

void a_box_without_shift_starts_from_nothing() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 tool = r.find_icon_in(w.tools, "Tool");
    const u32 free = r.find_icon_in(w.tools, "Free");
    r.click(w.tools, 103, 43);
    CHECK(r.workbench.window(w.tools).clicks.is_selected(tool));

    r.press(w.tools, 50, 30);
    r.move_held(r.screen_x(w.tools, 5), r.screen_y(w.tools, 5));
    CHECK(!r.workbench.window(w.tools).clicks.is_selected(tool));
    CHECK(r.workbench.window(w.tools).clicks.is_selected(free));
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected().size(), 1u);
    r.release(r.screen_x(w.tools, 5), r.screen_y(w.tools, 5));
}

void a_box_is_clamped_to_the_inner_area() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 free = r.find_icon_in(w.tools, "Free");
    const u32 put = r.find_icon_in(w.tools, "Put");
    r.press(w.tools, 5, 5);
    r.move_held(r.screen_x(w.tools, 2000), r.screen_y(w.tools, 2000));
    CHECK(r.workbench.box().active);
    CHECK(r.workbench.window(w.tools).clicks.is_selected(free));
    // Put is below the visible inner area and so outside the clamped box.
    CHECK(!r.workbench.window(w.tools).clicks.is_selected(put));
    r.release(r.screen_x(w.tools, 2000), r.screen_y(w.tools, 2000));
}

void a_one_pixel_move_arms_a_box_without_activating_it() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    r.press(w.tools, 50, 30);
    CHECK(r.workbench.box().armed);
    CHECK(!r.workbench.box().active);
    r.move_held(r.screen_x(w.tools, 51), r.screen_y(w.tools, 30));
    CHECK(r.workbench.box().armed);
    CHECK(!r.workbench.box().active);
    r.release(r.screen_x(w.tools, 51), r.screen_y(w.tools, 30));
    CHECK(!r.workbench.box().armed);
}

void selected_icons_drag_together() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 tool = r.find_icon_in(w.tools, "Tool");
    const u32 free = r.find_icon_in(w.tools, "Free");
    r.click(w.tools, 103, 43);
    shift_click_at(r, w.tools, 11, 11);
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected().size(), 2u);

    r.press(w.tools, 103, 43);
    r.move_held(r.screen_x(w.tools, 120), r.screen_y(w.tools, 60));
    r.release(r.screen_x(w.tools, 123), r.screen_y(w.tools, 63));
    CHECK_EQ(r.workbench.icon(tool).left, 120);
    CHECK_EQ(r.workbench.icon(tool).top, 60);
    CHECK_EQ(r.workbench.icon(free).left, 28);
    CHECK_EQ(r.workbench.icon(free).top, 28);
    CHECK(r.workbench.icon(tool).placed);
    CHECK(r.workbench.icon(free).placed);
}

void a_dragged_group_keeps_its_offset_at_the_edge() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 tool = r.find_icon_in(w.tools, "Tool");
    const u32 free = r.find_icon_in(w.tools, "Free");
    r.click(w.tools, 103, 43);
    shift_click_at(r, w.tools, 11, 11);

    r.press(w.tools, 103, 43);
    r.move_held(r.screen_x(w.tools, 13), r.screen_y(w.tools, 13));
    r.release(r.screen_x(w.tools, 13), r.screen_y(w.tools, 13));
    CHECK_EQ(r.workbench.icon(free).left, 0);
    CHECK_EQ(r.workbench.icon(free).top, 0);
    CHECK_EQ(r.workbench.icon(tool).left, 92);
    CHECK_EQ(r.workbench.icon(tool).top, 32);
    CHECK_EQ(r.workbench.icon(tool).left - r.workbench.icon(free).left, 92);
    CHECK_EQ(r.workbench.icon(tool).top - r.workbench.icon(free).top, 32);
}

void two_projects_dragged_to_a_window_arrive_there() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    r.click(w.tools, 63, 83);
    shift_click_at(r, w.tools, 93, 83);
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected().size(), 2u);

    r.press(w.tools, 63, 83);
    r.move_held(r.screen_x(w.ram, 153), r.screen_y(w.ram, 43));
    r.release(r.screen_x(w.ram, 153), r.screen_y(w.ram, 43));
    CHECK(r.exists("RAM:Proj"));
    CHECK(r.exists("RAM:Lost"));
    const u32 proj = r.find_icon_in(w.ram, "Proj");
    const u32 lost = r.find_icon_in(w.ram, "Lost");
    CHECK(proj != Workbench::kNoIcon);
    CHECK(lost != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(proj).left, 150);
    CHECK_EQ(r.workbench.icon(proj).top, 40);
    CHECK_EQ(r.workbench.icon(lost).left, 180);
    CHECK_EQ(r.workbench.icon(lost).top, 40);
    CHECK(r.saved_place("RAM:Proj") == std::make_pair(150u, 40u));
    CHECK(r.saved_place("RAM:Lost") == std::make_pair(180u, 40u));
}

void two_projects_dropped_on_a_drawer_icon_go_into_it() {
    Rig r;
    put_ram_project("Note");
    put_ram_project("Other");
    r.make_dir("RAM:Box");
    u32 error = 0;
    const u32 drawer = g_icon.default_object(kWBDrawer);
    CHECK(drawer != 0);
    CHECK(g_icon.save_object("RAM:Box", drawer, error));
    g_icon.free_object(drawer);
    const TwoWindows w = open_tools_and_ram(r);
    const Workbench::IconEntry note = r.workbench.icon(r.find_icon_in(w.ram, "Note"));
    const Workbench::IconEntry other = r.workbench.icon(r.find_icon_in(w.ram, "Other"));
    const Workbench::IconEntry box = r.workbench.icon(r.find_icon_in(w.ram, "Box"));

    r.click(w.ram, note.left + 3, note.top + 3);
    shift_click_at(r, w.ram, other.left + 3, other.top + 3);
    r.press(w.ram, note.left + 3, note.top + 3);
    r.move_held(r.screen_x(w.ram, box.left + 3), r.screen_y(w.ram, box.top + 3));
    r.release(r.screen_x(w.ram, box.left + 3), r.screen_y(w.ram, box.top + 3));
    CHECK(!r.exists("RAM:Note"));
    CHECK(!r.exists("RAM:Other"));
    CHECK(r.exists("RAM:Box/Note"));
    CHECK(r.exists("RAM:Box/Other"));
    CHECK(r.saved_place("RAM:Box/Note") ==
          std::make_pair(kNoIconPosition, kNoIconPosition));
    CHECK(r.saved_place("RAM:Box/Other") ==
          std::make_pair(kNoIconPosition, kNoIconPosition));
    CHECK_EQ(r.find_icon_in(w.ram, "Note"), Workbench::kNoIcon);
    CHECK_EQ(r.find_icon_in(w.ram, "Other"), Workbench::kNoIcon);
}

void a_shift_press_on_a_selected_icon_drags_the_selection() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 tool = r.find_icon_in(w.tools, "Tool");
    const u32 free = r.find_icon_in(w.tools, "Free");
    r.click(w.tools, 103, 43);
    shift_click_at(r, w.tools, 11, 11);
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected().size(), 2u);

    g_intuition.set_qualifier(1);
    r.press(w.tools, 103, 43);
    g_intuition.set_qualifier(0);
    r.move_held(r.screen_x(w.tools, 120), r.screen_y(w.tools, 60));
    r.release(r.screen_x(w.tools, 123), r.screen_y(w.tools, 63));
    CHECK_EQ(r.workbench.icon(tool).left, 120);
    CHECK_EQ(r.workbench.icon(tool).top, 60);
    CHECK_EQ(r.workbench.icon(free).left, 28);
    CHECK_EQ(r.workbench.icon(free).top, 28);
    CHECK_EQ(r.workbench.window(w.tools).clicks.selected().size(), 2u);
}

void a_box_on_the_root_window_selects_disks() {
    Rig r;
    r.workbench.open_desktop();
    const u32 disk = r.find_icon("HombreTest");
    const u32 ram = r.find_icon("Ram Disk");
    const Workbench::IconEntry a = r.workbench.icon(disk);
    const Workbench::IconEntry b = r.workbench.icon(ram);
    const i32 right = std::max(a.left + a.width, b.left + b.width) + 2;
    const i32 bottom = std::max(a.top + a.height, b.top + b.height) + 2;
    r.press(0, 0, 0);
    r.move_held(r.screen_x(0, right), r.screen_y(0, bottom));
    CHECK(r.workbench.box().active);
    CHECK(r.workbench.window(0).clicks.is_selected(disk));
    CHECK(r.workbench.window(0).clicks.is_selected(ram));
    CHECK_EQ(r.workbench.window(0).clicks.selected().size(), 2u);
    r.release(r.screen_x(0, right), r.screen_y(0, bottom));
    CHECK(!r.workbench.box().armed);
}

void a_rescan_ends_a_box() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 window = r.workbench.window(w.tools).window;
    r.press(w.tools, 50, 30);
    r.move_held(r.screen_x(w.tools, 5), r.screen_y(w.tools, 5));
    CHECK(r.workbench.box().active);
    r.rescan("HombreTest:Tools");
    CHECK(!r.workbench.box().armed);
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_REPORTMOUSE, 0u);
}

void a_dragged_disk_keeps_its_place_over_a_backdrop_toggle() {
    Rig r;
    r.workbench.open_desktop();
    const u32 disk = r.find_icon("HombreTest");
    const Workbench::IconEntry before = r.workbench.icon(disk);
    r.drag(0, before.left + 3, before.top + 3, r.screen_x(0, 103), r.screen_y(0, 123));
    CHECK_EQ(r.workbench.icon(disk).left, 100);
    CHECK_EQ(r.workbench.icon(disk).top, 120);
    CHECK(r.workbench.window(Workbench::kRootWindow).clicks.is_selected(disk));
    r.workbench.set_backdrop(false);
    CHECK_EQ(r.workbench.icon(disk).left, 100);
    CHECK_EQ(r.workbench.icon(disk).top, 120);
    CHECK(r.workbench.window(Workbench::kRootWindow).clicks.is_selected(disk));
    r.workbench.set_backdrop(true);
    CHECK_EQ(r.workbench.icon(disk).left, 100);
    CHECK_EQ(r.workbench.icon(disk).top, 120);
    CHECK(r.workbench.window(Workbench::kRootWindow).clicks.is_selected(disk));
}

void a_disk_icon_inside_a_drawer_is_no_volume() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 fake = r.find_icon_in(w.tools, "Fake");
    CHECK(fake != Workbench::kNoIcon);
    r.workbench.open_icon(w.tools, fake);
    CHECK(r.find_window("HombreTest:Tools/Fake") != Workbench::kNoWindow);
    CHECK_EQ(r.find_window("Fake:"), Workbench::kNoWindow);
}

void a_resized_drawer_clips_its_icons_and_keeps_their_places() {
    std::vector<AdfFile> files = test_files();
    files.push_back({"Labels.info", drawer_icon_info(static_cast<i32>(kNoIconPosition),
                                                    static_cast<i32>(kNoIconPosition), 30,
                                                    50, 300, 120)});
    files.push_back({"Labels/LongLabelName.info", icon_info(kWBTool, 10, 10)});
    files.push_back({"Labels/Right.info", icon_info(kWBTool, 200, 10)});
    Rig r(files);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon_in(root, "Labels"));
    const u32 labels = r.find_window("HombreTest:Labels");
    CHECK(labels != Workbench::kNoWindow);
    const u32 window = r.workbench.window(labels).window;
    const u32 layer = r.get32(r.workbench.window(labels).rp + rp_Layer);
    CHECK(layer != 0);

    // The pixels of the label rows under icons placed at top 10.
    auto label_pixels = [&]() {
        const Workbench::DrawerWindow& w = r.workbench.window(labels);
        u32 count = 0;
        for (i32 y = w.inner_top + 22; y <= w.inner_top + 34; ++y) {
            for (i32 x = w.inner_left; x < w.inner_left + w.inner_width; ++x) {
                count += r.window_pixel(window, x, y) == 1 ? 1 : 0;
            }
        }
        return count;
    };

    // A label wider than the room left of its icon is cut at the inner
    // edge, not moved inside it.
    const u32 cut = label_pixels();
    const u32 wide = r.find_icon_in(labels, "LongLabelName");
    const u32 right = r.find_icon_in(labels, "Right");
    r.drag(labels, 13, 13, r.screen_x(labels, 103), r.screen_y(labels, 13));
    CHECK_EQ(r.workbench.icon(wide).left, 100);
    const u32 whole = label_pixels();
    CHECK(cut < whole);
    CHECK(cut > 0);

    // Narrower than the right icon reaches: the frame keeps its pixels and
    // the icons keep their places.
    g_intuition.size_window_to(window, 215, 70);
    const u32 width = r.get16(window + wd_Width);
    const u32 height = r.get16(window + wd_Height);
    // The frame as Intuition drew it, before the desktop draws again.
    const i32 left = static_cast<i32>(r.get8(window + wd_BorderLeft));
    const i32 top = static_cast<i32>(r.get8(window + wd_BorderTop));
    const i32 inner_right = static_cast<i32>(width) - static_cast<i32>(r.get8(window + wd_BorderRight));
    const i32 inner_bottom =
        static_cast<i32>(height) - static_cast<i32>(r.get8(window + wd_BorderBottom));
    // The sliders show the new size once the desktop has measured it.
    const Rig::Box vert =
        r.gadget_box(window, r.workbench.scroll_gadget(labels, Workbench::kScrollVert));
    const Rig::Box horiz =
        r.gadget_box(window, r.workbench.scroll_gadget(labels, Workbench::kScrollHoriz));
    auto read_frame = [&]() {
        std::vector<u32> pens;
        for (i32 y = 0; y < static_cast<i32>(height); ++y) {
            for (i32 x = 0; x < static_cast<i32>(width); ++x) {
                if (vert.holds(x, y) || horiz.holds(x, y)) {
                    continue;
                }
                if (x < left || x >= inner_right || y < top || y >= inner_bottom) {
                    pens.push_back(r.window_pixel(window, x, y));
                }
            }
        }
        return pens;
    };
    const std::vector<u32> frame = read_frame();
    r.workbench.pump();
    CHECK(read_frame() == frame);
    CHECK(r.workbench.window(labels).inner_width < 200 + 32);
    CHECK_EQ(r.workbench.icon(right).left, 200);
    CHECK_EQ(r.workbench.icon(right).top, 10);
    CHECK_EQ(r.workbench.icon(wide).left, 100);
    // Drawing again draws nothing on the frame.
    r.rescan("HombreTest:Labels");
    CHECK(read_frame() == frame);
    CHECK_EQ(r.get32(layer + ly_ClipRegion), 0u);

    // Closing the drawer gives its clip region back.
    const u32 before = r.free_bytes();
    r.workbench.open_icon(root, r.find_icon_in(root, "Tools"));
    r.workbench.close_drawer(r.find_window("HombreTest:Tools"));
    CHECK_EQ(r.free_bytes(), before);
}

// A drawer of two icons, one far below the other, in a window too short
// for both.
std::vector<AdfFile> scroll_files(i32 scroll_x = 0, i32 scroll_y = 0) {
    std::vector<AdfFile> files = test_files();
    files.push_back({"Long.info", drawer_icon_info(static_cast<i32>(kNoIconPosition),
                                                   static_cast<i32>(kNoIconPosition), 40, 50,
                                                   240, 110, scroll_x, scroll_y)});
    files.push_back({"Long/Top.info", icon_info(kWBTool, 10, 10)});
    files.push_back({"Long/Bottom.info", icon_info(kWBTool, 10, 300)});
    return files;
}

u32 open_long(Rig& r) {
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon_in(root, "Long"));
    const u32 long_drawer = r.find_window("HombreTest:Long");
    CHECK(long_drawer != Workbench::kNoWindow);
    return long_drawer;
}

// The left button pressed and released at a screen point.
void click_at(Rig& r, std::pair<i32, i32> at) {
    g_intuition.pointer_event(at.first, at.second, 1, 1, 0, true);
    r.workbench.pump();
    g_intuition.pointer_event(at.first, at.second, 0, 0, 1, false);
    r.workbench.pump();
}

void drawers_have_scroll_gadgets_in_their_borders() {
    Rig r;
    r.workbench.open_desktop();
    // The backdrop has none.
    CHECK_EQ(r.workbench.window(0).gadgets, 0u);
    CHECK_EQ(r.get32(r.workbench.window(0).window + wd_FirstGadget), 0u);
    // The first window makes the arrows every window shares.
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    r.workbench.close_drawer(r.find_window("HombreTest:"));
    const u32 before = r.free_bytes();

    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 drawer = r.find_window("HombreTest:");
    const u32 window = r.workbench.window(drawer).window;
    CHECK_EQ(r.get32(window + wd_FirstGadget), r.workbench.scroll_gadget(drawer, 1));
    const i32 width = static_cast<i16>(r.get16(window + wd_Width));
    const i32 height = static_cast<i16>(r.get16(window + wd_Height));
    const Rect size = Intuition::size_box_rect(0, 0, width, height, r.get32(window + wd_Flags));
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    u32 gadget = r.get32(window + wd_FirstGadget);
    for (u16 id = 1; id <= Workbench::kScrollGadgets; ++id) {
        CHECK_EQ(gadget, r.workbench.scroll_gadget(drawer, id));
        CHECK_EQ(r.get16(gadget + gg_GadgetID), static_cast<u32>(id));
        const Rig::Box box = r.gadget_box(window, gadget);
        CHECK(box.x0 <= box.x1 && box.y0 <= box.y1);
        // In the borders, clear of the size gadget and the title bar.
        CHECK(box.x0 >= 0 && box.x1 < width && box.y0 >= w.inner_top && box.y1 < height);
        CHECK(box.x1 < size.x0 || box.y1 < size.y0);
        const bool right = box.x0 >= w.inner_left + w.inner_width;
        const bool bottom = box.y0 >= w.inner_top + w.inner_height;
        CHECK(right || bottom);
        gadget = r.get32(gadget + gg_NextGadget);
    }
    // A volume window adds its fuel gauge after the scroll gadgets.
    CHECK_EQ(gadget, w.gauge);
    CHECK(gadget != 0);
    CHECK_EQ(r.get16(gadget + gg_GadgetType) & GTYP_GTYPEMASK, GTYP_BOOLGADGET);
    CHECK_EQ(r.get16(gadget + gg_Activation), GACT_LEFTBORDER);
    CHECK_EQ(r.get32(gadget + gg_NextGadget), 0u);
    CHECK_EQ(r.get16(r.workbench.scroll_gadget(drawer, Workbench::kScrollVert) + gg_GadgetType) &
                 GTYP_GTYPEMASK,
             GTYP_PROPGADGET);
    CHECK_EQ(r.get16(r.workbench.scroll_gadget(drawer, Workbench::kScrollUp) + gg_GadgetType) &
                 GTYP_GTYPEMASK,
             GTYP_BOOLGADGET);

    // Closing the drawer gives its gadgets back.
    r.workbench.close_drawer(drawer);
    CHECK_EQ(r.free_bytes(), before);

    // The root window has them while it is no backdrop.
    r.workbench.set_backdrop(false);
    CHECK(r.workbench.scroll_gadget(0, Workbench::kScrollVert) != 0);
    CHECK_EQ(r.get32(r.workbench.window(0).window + wd_FirstGadget),
             r.workbench.scroll_gadget(0, Workbench::kScrollVert));
    r.workbench.set_backdrop(true);
    CHECK_EQ(r.workbench.scroll_gadget(0, Workbench::kScrollVert), 0u);
}

// The pens of an Image's planes, row by row, as Workbench's arrow images
// hold them: one word to a row of a plane.
// struct Image: Width, Height, Depth and ImageData.
constexpr u32 kImageWidth = 4;
constexpr u32 kImageHeight = 6;
constexpr u32 kImageDepth = 8;
constexpr u32 kImageData = 10;

void put_image32(std::vector<u8>& image, u32 block, u32 index, u32 value) {
    u8* p = image.data() + static_cast<usize>(block) * 512 + index * 4;
    p[0] = static_cast<u8>(value >> 24);
    p[1] = static_cast<u8>(value >> 16);
    p[2] = static_cast<u8>(value >> 8);
    p[3] = static_cast<u8>(value);
}

// The bitmap the writer leaves out, in the block after the root: the root
// names it, and every block starts used until free_image_block frees one.
void add_image_bitmap(std::vector<u8>& image) {
    put_image32(image, 880, 78, 0xFFFFFFFFu);
    put_image32(image, 880, 79, 881);
    adf_fix_checksum(image, 880);
}

void free_image_block(std::vector<u8>& image, u32 block, bool is_free) {
    // A bitmap block is a checksum and longs of bits from block 2 on, the
    // lowest bit of each long first.
    const u32 bit = block - 2;
    u8* word = image.data() + static_cast<usize>(881) * 512 + 4 + (bit / 32) * 4;
    u8& byte = word[3 - (bit % 32) / 8];
    const u8 mask = static_cast<u8>(1u << (bit % 8));
    byte = is_free ? static_cast<u8>(byte | mask) : static_cast<u8>(byte & ~mask);
}

std::vector<u8> image_pens(const Rig& r, u32 image) {
    const u32 width = r.get16(image + kImageWidth);
    const u32 height = r.get16(image + kImageHeight);
    const u32 depth = r.get16(image + kImageDepth);
    const u32 data = r.get32(image + kImageData);
    std::vector<u8> pens(width * height);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            for (u32 plane = 0; plane < depth; ++plane) {
                const u32 row = r.get16(data + (plane * height + y) * 2);
                pens[y * width + x] |= static_cast<u8>(((row >> (15 - x)) & 1u) << plane);
            }
        }
    }
    return pens;
}

std::vector<AdfFile> gauge_files() {
    return {
        {"Sub.info", drawer_icon_info(static_cast<i32>(kNoIconPosition),
                                      static_cast<i32>(kNoIconPosition), 40, 50, 160, 100)},
        {"Sub/Deep.info", icon_info(kWBTool, 10, 10)},
    };
}

// A disk whose bitmap leaves exactly used_blocks blocks in use.
std::vector<u8> gauge_disk(const char* name, u32 used_blocks) {
    auto image = build_adf(name, false, gauge_files());
    add_image_bitmap(image);
    for (u32 block = 2; block < 1760; ++block) {
        const bool used = block < used_blocks || block == 880 || block == 881;
        free_image_block(image, block, !used);
    }
    return image;
}

// The gauge a volume window shows when used of its volume's total blocks are
// in use: in the look of the screen's depth and the screen's pens, as high as
// its gadget and for the window as it is, active or not.
chrome::FuelGauge gauge_of(Rig& r, u32 drawer, u32 used, u32 total) {
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    chrome::FuelGauge gauge;
    gauge.depth = r.get8(r.screen + sc_BitMap + bm_Depth);
    gauge.style = chrome::style_for_depth(gauge.depth);
    gauge.width = chrome::fuel_gauge_width(gauge.style);
    gauge.height = static_cast<i32>(r.get16(w.gauge + gg_Height));
    gauge.filled = chrome::fuel_gauge_filled(gauge.height, used, total);
    gauge.active = g_intuition.active_window() == w.window;
    native::Bus bus(r.m);
    sysimage::read_pens(bus, g_intuition.screen_draw_info(r.screen), gauge.pens.pen);
    return gauge;
}

// The window's gauge gadget is in the left border, from two pixels below the
// inner top to the inner bottom, and its image and the window's border show
// the gauge pixel for pixel. The image's pens, row by row.
std::vector<u8> check_gauge_shown(Rig& r, u32 drawer, const chrome::FuelGauge& gauge) {
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    const u32 window = w.window;
    const i32 left = chrome::fuel_gauge_left(gauge.style);
    const i32 top = static_cast<i32>(r.get8(window + wd_BorderTop)) + 2;
    CHECK_EQ(r.get16(w.gauge + gg_LeftEdge), static_cast<u32>(left));
    CHECK_EQ(r.get16(w.gauge + gg_Width), static_cast<u32>(gauge.width));
    CHECK_EQ(r.get16(w.gauge + gg_TopEdge), static_cast<u32>(top));
    CHECK_EQ(gauge.height, static_cast<i16>(r.get16(window + wd_Height)) - top -
                               static_cast<i32>(r.get8(window + wd_BorderBottom)));
    CHECK_EQ(r.get8(window + wd_BorderLeft), static_cast<u32>(left + gauge.width + 2));
    CHECK_EQ(w.inner_left, left + gauge.width + 2);
    const u32 image = r.get32(w.gauge + gg_GadgetRender);
    CHECK_EQ(r.get16(image + kImageWidth), static_cast<u32>(gauge.width));
    CHECK_EQ(r.get16(image + kImageHeight), static_cast<u32>(gauge.height));
    CHECK_EQ(r.get16(image + kImageDepth), gauge.depth);
    const std::vector<u8> pens = image_pens(r, image);
    for (i32 y = 0; y < gauge.height; ++y) {
        for (i32 x = 0; x < gauge.width; ++x) {
            const u32 pen = chrome::fuel_gauge_pen(gauge, x, y);
            CHECK_EQ(static_cast<u32>(pens[static_cast<usize>(y * gauge.width + x)]), pen);
            CHECK_EQ(r.window_pixel(window, left + x, top + y), pen);
        }
    }
    return pens;
}

// The rows a gauge fills follow the used share of the volume: none of an
// empty volume, every row inside the track's edge of a full one or one whose
// size is unknown, and at least two as soon as anything is stored.
void the_fuel_gauge_fills_by_the_used_share() {
    CHECK_EQ(chrome::fuel_gauge_filled(102, 0, 1000), 0);
    CHECK_EQ(chrome::fuel_gauge_filled(102, 250, 1000), 25);
    CHECK_EQ(chrome::fuel_gauge_filled(102, 500, 1000), 50);
    CHECK_EQ(chrome::fuel_gauge_filled(102, 1000, 1000), 100);
    CHECK_EQ(chrome::fuel_gauge_filled(102, 2000, 1000), 100);
    CHECK_EQ(chrome::fuel_gauge_filled(102, 7, 0), 100);
    CHECK_EQ(chrome::fuel_gauge_filled(102, 1, 1000), 2);
    CHECK_EQ(chrome::fuel_gauge_filled(102, 0xFFFFFFF0u, 0xFFFFFFFFu), 100);
    CHECK_EQ(chrome::fuel_gauge_filled(3, 1, 1000), 1);
    CHECK_EQ(chrome::fuel_gauge_filled(2, 1, 2), 0);

    // The classic gauge of an empty volume has no box, that of a full one a
    // box over the whole inside of its track, whose marks it covers.
    chrome::FuelGauge gauge;
    gauge.width = chrome::fuel_gauge_width(chrome::Style::Classic);
    gauge.height = 102;
    gauge.depth = 2;
    gauge.pens.pen[sysimage::ShinePen] = 2;
    gauge.pens.pen[sysimage::ShadowPen] = 1;
    gauge.pens.pen[sysimage::FillPen] = 3;
    const i32 mark = chrome::fuel_gauge_mark_row(gauge.height, 9);
    CHECK_EQ(mark, 91);
    CHECK_EQ(chrome::fuel_gauge_pen(gauge, 1, mark), 1u);
    CHECK_EQ(chrome::fuel_gauge_pen(gauge, 1, mark + 1), 2u);
    for (i32 y = mark + 2; y <= 100; ++y) {
        CHECK_EQ(chrome::fuel_gauge_pen(gauge, 1, y), 0u);
    }
    gauge.filled = 100;
    CHECK_EQ(chrome::fuel_gauge_pen(gauge, 1, 1), 2u);
    CHECK_EQ(chrome::fuel_gauge_pen(gauge, 9, 100), 1u);
    for (i32 y = 2; y < 100; ++y) {
        CHECK_EQ(chrome::fuel_gauge_pen(gauge, 1, y), 2u);
        CHECK_EQ(chrome::fuel_gauge_pen(gauge, 4, y), 0u);
        CHECK_EQ(chrome::fuel_gauge_pen(gauge, 9, y), 1u);
    }
    // A gauge too short for a scale has none, and one of any size, however
    // small, is drawn in the look's pens only.
    gauge.filled = 0;
    gauge.height = 40;
    for (i32 y = 1; y < 39; ++y) {
        CHECK_EQ(chrome::fuel_gauge_pen(gauge, 1, y), 0u);
    }
    for (const chrome::Style style : {chrome::Style::Classic, chrome::Style::Modern}) {
        gauge.style = style;
        gauge.depth = style == chrome::Style::Modern ? 8 : 2;
        gauge.width = chrome::fuel_gauge_width(style);
        for (i32 height = 1; height <= 12; ++height) {
            gauge.height = height;
            gauge.filled = chrome::fuel_gauge_filled(height, 1, 2);
            for (i32 y = 0; y < height; ++y) {
                for (i32 x = 0; x < gauge.width; ++x) {
                    const u32 pen = chrome::fuel_gauge_pen(gauge, x, y);
                    CHECK(style == chrome::Style::Modern ? pen >= chrome::kScreenBar0 : pen < 4);
                }
            }
        }
    }
    CHECK_EQ(chrome::fuel_gauge_pen(gauge, -1, 0), 0u);
    CHECK_EQ(chrome::fuel_gauge_pen(gauge, 0, gauge.height), 0u);
}

// A volume window of the classic look carries the gauge of the 3.2 Workbench
// in a left border of 19 pixels: a sunken track on the border's face with a
// scale of long and short marks, and the used share of the disk as a raised
// box at its bottom. A drawer below the volume has no gauge.
void volume_windows_show_a_fuel_gauge() {
    constexpr u32 kUsed = 440;
    Rig r(test_files(), gauge_disk("Second", kUsed), 1);
    CHECK_EQ(adf_used_blocks(r.second_volume), kUsed);
    r.workbench.open_desktop();
    const u32 volume = r.find_icon("Second");
    CHECK(volume != Workbench::kNoIcon);
    r.workbench.open_icon(0, volume);
    const u32 drawer = r.find_window("Second:");
    CHECK(drawer != Workbench::kNoWindow);
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    const u32 window = w.window;
    CHECK_EQ(r.get8(window + wd_BorderLeft), 19u);
    CHECK_EQ(w.inner_left, 19);
    CHECK(w.gauge != 0);

    // The gauge follows the scroll gadgets in the window's list.
    u32 gadget = r.get32(window + wd_FirstGadget);
    for (u32 i = 0; i < Workbench::kScrollGadgets; ++i) {
        gadget = r.get32(gadget + gg_NextGadget);
    }
    CHECK_EQ(gadget, w.gauge);
    CHECK_EQ(r.get16(gadget + gg_GadgetType) & GTYP_GTYPEMASK, GTYP_BOOLGADGET);
    CHECK_EQ(r.get16(gadget + gg_Activation), GACT_LEFTBORDER);
    CHECK_EQ(r.get16(gadget + gg_Flags), GFLG_GADGIMAGE | GFLG_GADGHNONE);
    CHECK_EQ(r.get16(gadget + gg_LeftEdge), 4u);
    CHECK_EQ(r.get16(gadget + gg_Width), 13u);
    CHECK_EQ(r.get32(gadget + gg_NextGadget), 0u);

    const u32 total = r.second_volume.block_count - 2;
    const chrome::FuelGauge gauge = gauge_of(r, drawer, kUsed, total);
    CHECK(gauge.active);
    CHECK(gauge.style == chrome::Style::Classic);
    const i32 height = gauge.height;
    const i32 filled = gauge.filled;
    CHECK_EQ(filled, static_cast<i32>((kUsed * static_cast<u32>(height - 2) + total / 2) / total));
    CHECK(filled > 3 && filled < height / 2);
    const std::vector<u8> pens = check_gauge_shown(r, drawer, gauge);
    const auto pen_at = [&](i32 x, i32 y) {
        return static_cast<u32>(pens[static_cast<usize>(y) * 13 + static_cast<usize>(x)]);
    };
    const u32 face = gauge.pens.pen[sysimage::FillPen];
    const u32 shine = gauge.pens.pen[sysimage::ShinePen];
    const u32 shadow = gauge.pens.pen[sysimage::ShadowPen];
    // The track is 11 columns, dark at its top and left and light at its
    // bottom and right, with two columns of the border's face beside it.
    CHECK_EQ(pen_at(0, 0), shadow);
    CHECK_EQ(pen_at(9, 0), shadow);
    CHECK_EQ(pen_at(10, 0), face);
    CHECK_EQ(pen_at(0, height - 2), shadow);
    CHECK_EQ(pen_at(0, height - 1), face);
    CHECK_EQ(pen_at(1, height - 1), shine);
    CHECK_EQ(pen_at(10, height - 1), shine);
    CHECK_EQ(pen_at(10, 1), shine);
    CHECK_EQ(pen_at(11, 1), face);
    CHECK_EQ(pen_at(12, height - 1), face);
    CHECK_EQ(pen_at(5, 1), face);
    // The scale starts with a long mark of four pixels, a dark row over a
    // light one, and goes on with a short one of two.
    const i32 long_mark = chrome::fuel_gauge_mark_row(height, 1);
    const i32 short_mark = chrome::fuel_gauge_mark_row(height, 2);
    CHECK_EQ(long_mark, 1 + (height - 1) / 10);
    CHECK(short_mark + 1 < height - 1 - filled);
    for (i32 x = 1; x <= 4; ++x) {
        CHECK_EQ(pen_at(x, long_mark - 1), face);
        CHECK_EQ(pen_at(x, long_mark), shadow);
        CHECK_EQ(pen_at(x, long_mark + 1), shine);
        CHECK_EQ(pen_at(x, long_mark + 2), face);
        CHECK_EQ(pen_at(x, short_mark), x <= 2 ? shadow : face);
        CHECK_EQ(pen_at(x, short_mark + 1), x <= 2 ? shine : face);
    }
    CHECK_EQ(pen_at(5, long_mark), face);
    // The box of the used share is as wide as the track's inside, light at
    // its top and left and dark at its bottom and right.
    const i32 box_top = height - 1 - filled;
    CHECK_EQ(pen_at(1, box_top - 1), face);
    CHECK_EQ(pen_at(1, box_top), shine);
    CHECK_EQ(pen_at(8, box_top), shine);
    CHECK_EQ(pen_at(9, box_top), face);
    CHECK_EQ(pen_at(1, box_top + 1), shine);
    CHECK_EQ(pen_at(5, box_top + 1), face);
    CHECK_EQ(pen_at(9, box_top + 1), shadow);
    CHECK_EQ(pen_at(1, height - 2), face);
    CHECK_EQ(pen_at(2, height - 2), shadow);
    CHECK_EQ(pen_at(9, height - 2), shadow);

    // A drawer below the volume keeps the default border and has no gauge.
    r.workbench.open_icon(drawer, r.find_icon_in(drawer, "Sub"));
    const u32 sub = r.find_window("Second:Sub");
    CHECK(sub != Workbench::kNoWindow);
    CHECK_EQ(r.workbench.window(sub).gauge, 0u);
    CHECK_EQ(r.get8(r.workbench.window(sub).window + wd_BorderLeft), 2u);
    u32 sub_gadget = r.get32(r.workbench.window(sub).window + wd_FirstGadget);
    for (u32 i = 0; i < Workbench::kScrollGadgets; ++i) {
        sub_gadget = r.get32(sub_gadget + gg_NextGadget);
    }
    CHECK_EQ(sub_gadget, 0u);

    // Opening and closing the volume window gives the gauge's memory back.
    r.workbench.close_drawer(sub);
    r.workbench.close_drawer(drawer);
    const u32 before = r.free_bytes();
    r.workbench.open_icon(0, volume);
    const u32 again = r.find_window("Second:");
    CHECK(again != Workbench::kNoWindow);
    r.workbench.close_drawer(again);
    CHECK_EQ(r.free_bytes(), before);
}

// A click on the desktop beside every drawer, which makes the root window
// the active one.
void click_desktop(Rig& r) {
    const Workbench::DrawerWindow& root = r.workbench.window(0);
    r.click(0, 4 - root.inner_left, 380 - root.inner_top);
    CHECK_EQ(g_intuition.active_window(), root.window);
}

// The classic gauge's face is the border's: the fill pen while its window is
// active and the background pen while it is not. The gauge is painted again
// and stays shown over the frame each time the window's state changes.
void the_classic_fuel_gauge_follows_its_window_state() {
    constexpr u32 kUsed = 600;
    Rig r(test_files(), gauge_disk("Second", kUsed), 1);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("Second"));
    const u32 drawer = r.find_window("Second:");
    CHECK(drawer != Workbench::kNoWindow);
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    const u32 window = w.window;
    const u32 total = r.second_volume.block_count - 2;
    const u32 image = r.get32(w.gauge + gg_GadgetRender);
    const i32 top = static_cast<i32>(r.get16(w.gauge + gg_TopEdge));
    CHECK_EQ(g_intuition.active_window(), window);
    const chrome::FuelGauge active = gauge_of(r, drawer, kUsed, total);
    CHECK(active.active);
    const u32 fill = active.pens.pen[sysimage::FillPen];
    const u32 background = active.pens.pen[sysimage::BackgroundPen];
    CHECK(fill != background);
    const std::vector<u8> lit = check_gauge_shown(r, drawer, active);
    CHECK_EQ(static_cast<u32>(lit[13 + 5]), fill);
    CHECK_EQ(r.window_pixel(window, 2, top + 1), fill);

    const u32 before = r.free_bytes();
    click_desktop(r);
    const chrome::FuelGauge inactive = gauge_of(r, drawer, kUsed, total);
    CHECK(!inactive.active);
    const std::vector<u8> dim = check_gauge_shown(r, drawer, inactive);
    CHECK_EQ(static_cast<u32>(dim[13 + 5]), background);
    CHECK_EQ(static_cast<u32>(dim[13 + 12]), background);
    CHECK_EQ(r.window_pixel(window, 2, top + 1), background);
    // The edges, the marks and the box are where they were.
    CHECK_EQ(lit.size(), dim.size());
    for (usize i = 0; i < lit.size(); ++i) {
        CHECK_EQ(lit[i] == fill, dim[i] == background);
        CHECK(lit[i] == fill || lit[i] == dim[i]);
    }

    // A click in the window makes it active again.
    r.click(drawer, 150, 50);
    CHECK_EQ(g_intuition.active_window(), window);
    CHECK(check_gauge_shown(r, drawer, active) == lit);
    // The image is the one the window opened with, painted in place.
    CHECK_EQ(r.get32(w.gauge + gg_GadgetRender), image);
    CHECK_EQ(r.free_bytes(), before);
}

// A click on the middle of the window's zoom button, and the desktop's
// answer to what it sets going.
void click_zoom(Rig& r, u32 window) {
    const Rect box = Intuition::zoom_box_rect(
        static_cast<i16>(r.get16(window + wd_LeftEdge)),
        static_cast<i16>(r.get16(window + wd_TopEdge)),
        static_cast<i16>(r.get16(window + wd_Width)), r.get32(window + wd_Flags));
    click_at(r, {(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2});
}

// A volume window of the modern look carries a gauge in the look of the
// sliders, in a left border of 15 pixels: a rounded, outlined track as wide
// as a slider's knob with short marks, and the used share shaded from its top
// to the track's bottom under a line of the outline's pen. It looks the same
// in an active and an inactive window.
void the_modern_fuel_gauge_is_in_the_look_of_the_sliders() {
    for (const u32 depth : {5u, 8u}) {
        constexpr u32 kUsed = 700;
        Rig r(test_files(), gauge_disk("Second", kUsed), 1, depth);
        r.workbench.open_desktop();
        const u32 volume = r.find_icon("Second");
        r.workbench.open_icon(0, volume);
        const u32 drawer = r.find_window("Second:");
        CHECK(drawer != Workbench::kNoWindow);
        const Workbench::DrawerWindow& w = r.workbench.window(drawer);
        const u32 window = w.window;
        CHECK_EQ(r.get8(window + wd_BorderLeft), 15u);
        CHECK_EQ(r.get16(w.gauge + gg_LeftEdge), 3u);
        CHECK_EQ(r.get16(w.gauge + gg_Width), 10u);
        const u32 total = r.second_volume.block_count - 2;
        const chrome::FuelGauge gauge = gauge_of(r, drawer, kUsed, total);
        CHECK(gauge.style == chrome::Style::Modern);
        CHECK(gauge.active);
        const i32 height = gauge.height;
        const i32 filled = gauge.filled;
        CHECK_EQ(filled,
                 static_cast<i32>((kUsed * static_cast<u32>(height - 2) + total / 2) / total));
        CHECK(filled > 8 && filled < height / 2);
        const std::vector<u8> pens = check_gauge_shown(r, drawer, gauge);
        const auto pen_at = [&](i32 x, i32 y) {
            return static_cast<u32>(pens[static_cast<usize>(y) * 10 + static_cast<usize>(x)]);
        };
        const auto pen = [&](u8 chrome_pen) {
            return static_cast<u32>(chrome::physical_pen(chrome_pen, depth));
        };
        // The outline is rounded as a knob's, on the border's face.
        CHECK_EQ(pen_at(0, 0), pen(chrome::kInactiveBar5));
        CHECK_EQ(pen_at(1, 0), pen(chrome::kInactiveBar5));
        CHECK_EQ(pen_at(2, 0), pen(chrome::kFrameDark));
        CHECK_EQ(pen_at(7, 0), pen(chrome::kFrameDark));
        CHECK_EQ(pen_at(9, 0), pen(chrome::kInactiveBar5));
        CHECK_EQ(pen_at(1, 1), pen(chrome::kFrameDark));
        CHECK_EQ(pen_at(0, 2), pen(chrome::kFrameDark));
        CHECK_EQ(pen_at(9, height / 2), pen(chrome::kFrameDark));
        CHECK_EQ(pen_at(0, height - 1), pen(chrome::kInactiveBar5));
        CHECK_EQ(pen_at(4, height - 1), pen(chrome::kFrameDark));
        // The empty part is the sliders' track with a mark of three pixels
        // and one of two in turn.
        CHECK_EQ(pen_at(4, 2), pen(chrome::kTrack));
        const i32 long_mark = chrome::fuel_gauge_mark_row(height, 1);
        const i32 short_mark = chrome::fuel_gauge_mark_row(height, 2);
        CHECK(short_mark < height - 2 - filled);
        for (i32 x = 1; x <= 4; ++x) {
            CHECK_EQ(pen_at(x, long_mark - 1), pen(chrome::kTrack));
            CHECK_EQ(pen_at(x, long_mark), pen(x <= 3 ? chrome::kKnob2 : chrome::kTrack));
            CHECK_EQ(pen_at(x, long_mark + 1), pen(chrome::kTrack));
            CHECK_EQ(pen_at(x, short_mark), pen(x <= 2 ? chrome::kKnob2 : chrome::kTrack));
        }
        // The used share is shaded as a held knob, from its top to the
        // track's bottom, under a line across the track.
        const i32 fill_top = height - 1 - filled;
        CHECK_EQ(pen_at(4, fill_top - 2), pen(chrome::kTrack));
        for (i32 x = 1; x <= 8; ++x) {
            CHECK_EQ(pen_at(x, fill_top - 1), pen(chrome::kFrameDark));
            CHECK_EQ(pen_at(x, fill_top), pen(chrome::kActiveBar2));
            CHECK_EQ(pen_at(x, fill_top + (filled + 1) / 2), pen(chrome::kActiveBar4));
        }
        CHECK_EQ(pen_at(4, height - 2), pen(chrome::kActiveBar5));
        CHECK_EQ(pen_at(0, fill_top), pen(chrome::kFrameDark));

        // An inactive window shows the same gauge.
        click_desktop(r);
        CHECK(!gauge_of(r, drawer, kUsed, total).active);
        CHECK(check_gauge_shown(r, drawer, gauge) == pens);

        // The gauge follows the window to the small box of the zoom gadget
        // and back, and all its memory goes back when the window closes.
        r.workbench.close_drawer(drawer);
        const u32 before = r.free_bytes();
        r.workbench.open_icon(0, volume);
        const u32 again = r.find_window("Second:");
        CHECK(again != Workbench::kNoWindow);
        const Workbench::DrawerWindow& zoomed = r.workbench.window(again);
        click_zoom(r, zoomed.window);
        CHECK(static_cast<i32>(r.get16(zoomed.gauge + gg_Height)) < height);
        CHECK_EQ(r.get16(r.get32(zoomed.gauge + gg_GadgetRender) + kImageHeight),
                 r.get16(zoomed.gauge + gg_Height));
        click_zoom(r, zoomed.window);
        CHECK(check_gauge_shown(r, again, gauge_of(r, again, kUsed, total)) == pens);
        click_desktop(r);
        r.workbench.close_drawer(again);
        CHECK_EQ(r.free_bytes(), before);
    }
}

// The pens of the window's borders, row by row: its frame and the gadgets
// there, without its inner area.
std::vector<u32> border_pens(Rig& r, u32 window) {
    const i32 width = static_cast<i16>(r.get16(window + wd_Width));
    const i32 height = static_cast<i16>(r.get16(window + wd_Height));
    const i32 x0 = static_cast<i32>(r.get8(window + wd_BorderLeft));
    const i32 y0 = static_cast<i32>(r.get8(window + wd_BorderTop));
    const i32 x1 = width - static_cast<i32>(r.get8(window + wd_BorderRight));
    const i32 y1 = height - static_cast<i32>(r.get8(window + wd_BorderBottom));
    std::vector<u32> pens;
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            if (x < x0 || x >= x1 || y < y0 || y >= y1) {
                pens.push_back(r.window_pixel(window, x, y));
            }
        }
    }
    return pens;
}

// The gauge ends at the window's inner bottom or has no rows at all, and the
// window's borders show what Intuition draws there from the frame and the
// gadgets as they are: nothing of a gauge that was higher is left in them.
// The gauge's rows.
i32 check_gauge_fits(Rig& r, u32 drawer) {
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    const u32 window = w.window;
    const i32 inner_top = static_cast<i32>(r.get8(window + wd_BorderTop));
    const i32 inner_bottom = static_cast<i16>(r.get16(window + wd_Height)) -
                             static_cast<i32>(r.get8(window + wd_BorderBottom));
    const i32 top = static_cast<i32>(r.get16(w.gauge + gg_TopEdge));
    const i32 rows = static_cast<i32>(r.get16(w.gauge + gg_Height));
    CHECK_EQ(top, inner_top + 2);
    CHECK(rows == 0 || rows >= 6);
    CHECK(top + rows <= inner_bottom || rows == 0);
    if (rows != 0) {
        CHECK_EQ(r.get16(r.get32(w.gauge + gg_GadgetRender) + kImageHeight),
                 static_cast<u32>(rows));
    }
    const std::vector<u32> shown = border_pens(r, window);
    g_intuition.refresh_window_frame(window);
    g_intuition.refresh_glist(r.get32(window + wd_FirstGadget), window, -1);
    CHECK(border_pens(r, window) == shown);
    return rows;
}

// A volume window that gets smaller, by its zoom gadget, ZipWindow or
// ChangeWindowBox, never shows its gauge in the bottom border: the gauge is
// as high as the smaller window's inner area before the screen shows the
// window, and the bottom border and its gadgets are whole. A window with no
// room for a gauge shows none. The gauge grows again with the window.
void the_fuel_gauge_stays_out_of_the_bottom_border() {
    for (const u32 depth : {2u, 8u}) {
        constexpr u32 kUsed = 900;
        Rig r(test_files(), gauge_disk("Second", kUsed), 1, depth);
        r.workbench.open_desktop();
        const u32 volume = r.find_icon("Second");
        r.workbench.open_icon(0, volume);
        r.workbench.close_drawer(r.find_window("Second:"));
        const u32 before = r.free_bytes();
        r.workbench.open_icon(0, volume);
        const u32 drawer = r.find_window("Second:");
        CHECK(drawer != Workbench::kNoWindow);
        const Workbench::DrawerWindow& w = r.workbench.window(drawer);
        const u32 window = w.window;
        const u32 total = r.second_volume.block_count - 2;
        const i32 border_top = static_cast<i32>(r.get8(window + wd_BorderTop));
        const i32 border_bottom = static_cast<i32>(r.get8(window + wd_BorderBottom));
        const i32 left = static_cast<i16>(r.get16(window + wd_LeftEdge));
        const i32 top = static_cast<i16>(r.get16(window + wd_TopEdge));
        const i32 width = static_cast<i16>(r.get16(window + wd_Width));
        const i32 height = static_cast<i16>(r.get16(window + wd_Height));
        const i32 full = check_gauge_fits(r, drawer);
        CHECK_EQ(full, height - border_top - 2 - border_bottom);
        const std::vector<u8> pens =
            check_gauge_shown(r, drawer, gauge_of(r, drawer, kUsed, total));

        // The small box of the zoom gadget: the gauge fits it at once, before
        // the desktop hears of the new size.
        g_intuition.zip_window(window);
        const i32 small = static_cast<i16>(r.get16(window + wd_Height));
        CHECK(small < height);
        CHECK_EQ(check_gauge_fits(r, drawer), small - border_top - 2 - border_bottom);
        r.workbench.pump();
        CHECK_EQ(check_gauge_fits(r, drawer), small - border_top - 2 - border_bottom);
        check_gauge_shown(r, drawer, gauge_of(r, drawer, kUsed, total));

        // Back in its box the window still has the short gauge until the
        // desktop answers, and then the one it had.
        g_intuition.zip_window(window);
        CHECK_EQ(static_cast<i16>(r.get16(window + wd_Height)), height);
        CHECK_EQ(check_gauge_fits(r, drawer), small - border_top - 2 - border_bottom);
        r.workbench.pump();
        CHECK_EQ(check_gauge_fits(r, drawer), full);
        CHECK(check_gauge_shown(r, drawer, gauge_of(r, drawer, kUsed, total)) == pens);

        // The zoom gadget itself, and a box set from outside.
        click_zoom(r, window);
        CHECK_EQ(check_gauge_fits(r, drawer), small - border_top - 2 - border_bottom);
        click_zoom(r, window);
        CHECK_EQ(check_gauge_fits(r, drawer), full);
        g_intuition.change_window_box(window, left + 10, top + 5, width, height - 20);
        CHECK_EQ(check_gauge_fits(r, drawer), full - 20);
        r.workbench.pump();
        CHECK_EQ(check_gauge_fits(r, drawer), full - 20);
        check_gauge_shown(r, drawer, gauge_of(r, drawer, kUsed, total));

        // A window of four rows between its bar and its bottom border has no
        // room for a gauge: the gadget has no rows and the border is plain.
        CHECK(g_intuition.window_limits(window, 0, 20, 0, 0));
        g_intuition.change_window_box(window, left, top, width, border_top + 4 + border_bottom);
        CHECK_EQ(static_cast<i16>(r.get16(window + wd_Height)), border_top + 4 + border_bottom);
        CHECK_EQ(check_gauge_fits(r, drawer), 0);
        r.workbench.pump();
        CHECK_EQ(check_gauge_fits(r, drawer), 0);
        const chrome::FuelGauge none = gauge_of(r, drawer, kUsed, total);
        const u32 face = none.style == chrome::Style::Modern
                             ? chrome::physical_pen(chrome::kInactiveBar5, depth)
                             : none.pens.pen[none.active ? sysimage::FillPen
                                                         : sysimage::BackgroundPen];
        for (i32 y = border_top; y < border_top + 4; ++y) {
            for (i32 x = 2; x < w.inner_left - 1; ++x) {
                CHECK_EQ(r.window_pixel(window, x, y), face);
            }
        }

        // With room again the gauge is back, whole.
        g_intuition.change_window_box(window, left, top, width, height);
        CHECK_EQ(check_gauge_fits(r, drawer), 0);
        r.workbench.pump();
        CHECK_EQ(check_gauge_fits(r, drawer), full);
        CHECK(check_gauge_shown(r, drawer, gauge_of(r, drawer, kUsed, total)) == pens);

        r.workbench.close_drawer(drawer);
        CHECK_EQ(r.free_bytes(), before);
    }
}

// The arrow buttons of the drawer are as deep as depth and in the look of
// that depth, in the pens of the screen's DrawInfo.
void check_drawer_arrows(Rig& r, u32 drawer, u32 depth) {
    native::Bus bus(r.m);
    chrome::DrawInfoPens drawinfo;
    sysimage::read_pens(bus, g_intuition.screen_draw_info(r.screen), drawinfo.pen);
    for (u16 id = Workbench::kScrollUp; id <= Workbench::kScrollRight; ++id) {
        const u32 gadget = r.workbench.scroll_gadget(drawer, id);
        const auto arrow = static_cast<chrome::Arrow>(id - Workbench::kScrollUp);
        for (const bool pressed : {false, true}) {
            const u32 image = r.get32(gadget + (pressed ? gg_SelectRender : gg_GadgetRender));
            const i32 width = static_cast<i32>(r.get16(image + kImageWidth));
            const i32 height = static_cast<i32>(r.get16(image + kImageHeight));
            CHECK_EQ(r.get16(image + kImageDepth), depth);
            std::vector<u8> expected(static_cast<size_t>(width * height));
            chrome::paint_arrow_button(expected.data(), width, height, arrow, pressed,
                                       chrome::style_for_depth(depth), depth, drawinfo);
            const std::vector<u8> pens = image_pens(r, image);
            CHECK(pens == expected);
            if (chrome::style_for_depth(depth) == chrome::Style::Classic) {
                const u32 light = pressed ? sysimage::ShadowPen : sysimage::ShinePen;
                CHECK_EQ(static_cast<u32>(pens[0]), static_cast<u32>(drawinfo.pen[light]));
            } else {
                CHECK_EQ(static_cast<u32>(pens[0]),
                         static_cast<u32>(chrome::physical_pen(chrome::kTrack, depth)));
                for (const u8 pen : pens) {
                    CHECK(pen >= chrome::physical_pen(chrome::kScreenBar0, depth));
                }
            }
        }
    }
}

// The arrow buttons in a drawer's borders are as deep as the screen and in
// the look of its depth: the 3.1 arrow image in the screen's DrawInfo pens on
// 2 and 4 planes, the modern button in the chrome pens of that depth on 5, 6
// and 7.
void drawer_arrows_take_the_look_of_the_screen() {
    for (const u32 depth : {2u, 4u, 5u, 6u, 7u}) {
        Rig r(test_files(), {}, 1, depth);
        r.workbench.open_desktop();
        r.workbench.open_icon(0, r.find_icon("HombreTest"));
        const u32 drawer = r.find_window("HombreTest:");
        CHECK(drawer != Workbench::kNoWindow);
        check_drawer_arrows(r, drawer, depth);
    }
}

// A screen mode change resets the Workbench, and the drawers it opens again
// get arrows in the look of the screen's new depth and pens, not the first.
void drawer_arrows_follow_the_screen_through_a_reset() {
    Rig r(test_files(), {}, 1, 8);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    CHECK(r.find_window("HombreTest:") != Workbench::kNoWindow);
    const u32 draw_info = g_intuition.screen_draw_info(r.screen);
    const u32 pens = r.get32(draw_info + 4);
    // struct DrawInfo: dri_Depth.
    constexpr u32 kDriDepth = 12;
    const auto reset_to = [&](u32 depth, u32 shine) {
        CHECK(r.workbench.close_for_reset());
        r.put16(draw_info + kDriDepth, depth);
        r.put16(pens + sysimage::ShinePen * 2, shine);
        r.workbench.open_after_reset();
        const u32 drawer = r.find_window("HombreTest:");
        CHECK(drawer != Workbench::kNoWindow);
        check_drawer_arrows(r, drawer, depth);
    };
    reset_to(5, 2);
    reset_to(4, 2);
    // The same depth with other pens.
    reset_to(4, 3);
    reset_to(8, 2);
}

// On a screen of the classic look a window's whole frame is repainted in
// the pens of its new state when it turns inactive, and the arrows and the
// sliders in its borders come back on top of it.
void border_gadgets_stay_when_a_drawer_turns_inactive() {
    Rig r(test_files(), {}, 1, 2);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 drawer = r.find_window("HombreTest:");
    CHECK(drawer != Workbench::kNoWindow);
    const u32 window = r.workbench.window(drawer).window;
    CHECK_EQ(g_intuition.active_window(), window);

    const auto arrows_shown = [&] {
        for (u16 id = Workbench::kScrollUp; id <= Workbench::kScrollRight; ++id) {
            const u32 gadget = r.workbench.scroll_gadget(drawer, id);
            const u32 image = r.get32(gadget + gg_GadgetRender);
            const std::vector<u8> pens = image_pens(r, image);
            const i32 width = static_cast<i32>(r.get16(image + kImageWidth));
            const Rig::Box box = r.gadget_box(window, gadget);
            for (i32 y = box.y0; y <= box.y1; ++y) {
                for (i32 x = box.x0; x <= box.x1; ++x) {
                    CHECK_EQ(r.window_pixel(window, x, y),
                             static_cast<u32>(pens[(y - box.y0) * width + (x - box.x0)]));
                }
            }
        }
    };
    // The slider's raised knob has its light edge at its top left.
    const auto knob_shown = [&] {
        native::Bus bus(r.m);
        chrome::DrawInfoPens drawinfo;
        sysimage::read_pens(bus, g_intuition.screen_draw_info(r.screen), drawinfo.pen);
        const u32 slider = r.workbench.scroll_gadget(drawer, Workbench::kScrollVert);
        const Rig::Box box = r.gadget_box(window, slider);
        bool lit = false;
        for (i32 y = box.y0; y <= box.y1 && !lit; ++y) {
            for (i32 x = box.x0; x <= box.x1 && !lit; ++x) {
                lit = r.window_pixel(window, x, y) == drawinfo.pen[sysimage::ShinePen];
            }
        }
        CHECK(lit);
    };
    arrows_shown();
    knob_shown();

    // A click on the desktop beside the drawer makes the root window active.
    const i32 left = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 width = static_cast<i16>(r.get16(window + wd_Width));
    const Workbench::DrawerWindow& root = r.workbench.window(0);
    const i32 x = left > 8 ? 2 : left + width + 4;
    r.click(0, x - root.inner_left, 300 - root.inner_top);
    CHECK(g_intuition.active_window() != window);
    arrows_shown();
    knob_shown();
}

void the_arrows_and_sliders_scroll_a_drawer() {
    Rig r(scroll_files());
    const u32 drawer = open_long(r);
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    const u32 window = w.window;
    const u32 top_icon = r.find_icon_in(drawer, "Top");
    const u32 vert = r.workbench.scroll_gadget(drawer, Workbench::kScrollVert);
    const u32 horiz = r.workbench.scroll_gadget(drawer, Workbench::kScrollHoriz);
    const u32 vert_info = r.get32(vert + gg_SpecialInfo);
    const u32 horiz_info = r.get32(horiz + gg_SpecialInfo);
    CHECK_EQ(w.scroll_x, 0);
    CHECK_EQ(w.scroll_y, 0);
    // Taller than the window, as wide as it.
    const Workbench::Extent e = r.workbench.extent(drawer);
    CHECK(e.max_y > 300);
    CHECK(r.get16(vert_info + pi_VertBody) < MAXBODY);
    CHECK_EQ(r.get16(vert_info + pi_VertPot), 0u);
    CHECK_EQ(r.get16(horiz_info + pi_HorizBody), MAXBODY);

    // The icon's image, where it is drawn with the view scrolled down by.
    auto image = [&](i32 down) {
        std::vector<u32> pens;
        for (i32 y = 0; y < 12; ++y) {
            for (i32 x = 0; x < 32; ++x) {
                pens.push_back(r.window_pixel(window, w.inner_left + 10 + x,
                                              w.inner_top + 10 - down + y));
            }
        }
        return pens;
    };
    const std::vector<u32> plain = image(0);

    // Up and left go nowhere from the origin.
    click_at(r, r.gadget_middle(drawer, Workbench::kScrollUp));
    click_at(r, r.gadget_middle(drawer, Workbench::kScrollLeft));
    click_at(r, r.gadget_middle(drawer, Workbench::kScrollRight));
    CHECK_EQ(w.scroll_x, 0);
    CHECK_EQ(w.scroll_y, 0);

    // Down moves the view, the icons with it, and the slider's knob.
    click_at(r, r.gadget_middle(drawer, Workbench::kScrollDown));
    CHECK_EQ(w.scroll_y, Workbench::kScrollStep);
    CHECK(image(Workbench::kScrollStep) == plain);
    CHECK(r.get16(vert_info + pi_VertPot) > 0u);
    // A click where the icon shows now selects it.
    r.click(drawer, 13, 13 - Workbench::kScrollStep);
    const std::vector<u32> selected = image(Workbench::kScrollStep);
    for (usize i = 0; i < plain.size(); ++i) {
        CHECK_EQ(selected[i], plain[i] ^ 3u);
    }
    r.click(drawer, 200, 60);

    // An icon dragged within the window lands where the view shows it.
    r.drag(drawer, 13, 13 - Workbench::kScrollStep, r.screen_x(drawer, 113),
           r.screen_y(drawer, 25));
    CHECK_EQ(r.workbench.icon(top_icon).left, 110);
    CHECK_EQ(r.workbench.icon(top_icon).top, 25 + Workbench::kScrollStep - 3);

    // The knob dragged to the bottom shows the end of the icons.
    const Rig::Box track = r.gadget_box(window, vert);
    const i32 left = static_cast<i16>(r.get16(window + wd_LeftEdge));
    const i32 top = static_cast<i16>(r.get16(window + wd_TopEdge));
    const i32 knob_x = left + (track.x0 + track.x1) / 2;
    // The knob's top, a little below where the view's scroll puts it.
    const i32 knob_y = top + track.y0 + 2 +
                       static_cast<i32>((track.y1 - track.y0) *
                                        static_cast<u64>(r.get16(vert_info + pi_VertPot)) /
                                        MAXPOT / 8);
    g_intuition.pointer_event(knob_x, knob_y, 1, 1, 0, true);
    r.workbench.pump();
    g_intuition.pointer_event(knob_x, top + track.y1 + 200, 1, 0, 0, true);
    r.workbench.pump();
    g_intuition.pointer_event(knob_x, top + track.y1 + 200, 0, 0, 1, false);
    r.workbench.pump();
    const Workbench::Extent end = r.workbench.extent(drawer);
    CHECK_EQ(w.scroll_y, end.max_y - w.inner_height);
    CHECK_EQ(r.get16(vert_info + pi_VertPot), MAXPOT);
    // Down goes no further.
    click_at(r, r.gadget_middle(drawer, Workbench::kScrollDown));
    CHECK_EQ(w.scroll_y, end.max_y - w.inner_height);

    // A click on the track above the knob pages back towards the top.
    g_intuition.pointer_event(knob_x, top + track.y0 + 1, 1, 1, 0, true);
    r.workbench.pump();
    g_intuition.pointer_event(knob_x, top + track.y0 + 1, 0, 0, 1, false);
    r.workbench.pump();
    CHECK(w.scroll_y < end.max_y - w.inner_height);

    // A taller window needs less of a scroll, and the view stays inside.
    g_intuition.move_window_to(window, 40, 12);
    g_intuition.size_window_to(window, 240, 388);
    r.workbench.pump();
    const Workbench::Extent fits = r.workbench.extent(drawer);
    CHECK(w.inner_height >= fits.max_y);
    CHECK_EQ(w.scroll_y, 0);
    CHECK_EQ(r.get16(vert_info + pi_VertBody), MAXBODY);
}

// The wheel over a drawer scrolls its view three arrow steps a notch, down
// for down, and moves the slider's knob with it.
void the_wheel_scrolls_a_drawer() {
    Rig r(scroll_files());
    const u32 drawer = open_long(r);
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    const u32 window = w.window;
    CHECK_EQ(g_intuition.active_window(), window);
    // Typed keys still go to the shell, not the drawer.
    CHECK(!g_intuition.wants_keys(window));
    const u32 vert = r.workbench.scroll_gadget(drawer, Workbench::kScrollVert);
    const u32 vert_info = r.get32(vert + gg_SpecialInfo);
    g_intuition.pointer_event(r.screen_x(drawer, 100), r.screen_y(drawer, 60), 0, 0, 0, true);
    r.workbench.pump();

    g_intuition.wheel_event(1);
    r.workbench.pump();
    CHECK_EQ(w.scroll_y, 0);
    g_intuition.wheel_event(-2);
    r.workbench.pump();
    CHECK_EQ(w.scroll_y, 2 * Workbench::kWheelStep);
    CHECK(r.get16(vert_info + pi_VertPot) > 0u);
    g_intuition.wheel_event(1);
    r.workbench.pump();
    CHECK_EQ(w.scroll_y, Workbench::kWheelStep);
    CHECK_EQ(w.scroll_x, 0);
}

void a_window_snapshot_keeps_its_scroll() {
    // A drawer kept scrolled opens scrolled, as far as its icons reach.
    {
        Rig r(scroll_files(0, 40));
        const u32 drawer = open_long(r);
        CHECK_EQ(r.workbench.window(drawer).scroll_y, 40);
    }
    {
        Rig r(scroll_files(0, 5000));
        const u32 drawer = open_long(r);
        const Workbench::DrawerWindow& w = r.workbench.window(drawer);
        CHECK_EQ(w.scroll_y, r.workbench.extent(drawer).max_y - w.inner_height);
    }

    Rig r;
    for (const char* name : {"A", "B", "C", "D", "E", "F", "G", "H"}) {
        put_ram_project(name);
    }
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
    const u32 ram = r.find_window("Ram Disk:");
    CHECK(ram != Workbench::kNoWindow);
    const u32 window = r.workbench.window(ram).window;
    g_intuition.size_window_to(window, 200, 90);
    r.workbench.pump();
    CHECK(r.workbench.extent(ram).max_x > r.workbench.window(ram).inner_width + 16);
    click_at(r, r.gadget_middle(ram, Workbench::kScrollRight));
    click_at(r, r.gadget_middle(ram, Workbench::kScrollRight));
    CHECK_EQ(r.workbench.window(ram).scroll_x, 2 * Workbench::kScrollStep);

    CHECK(r.workbench.snapshot_window(ram, false));
    const u32 object = g_icon.load_object("RAM:Disk");
    CHECK(object != 0);
    const u32 data = r.get32(object + do_DrawerData);
    CHECK_EQ(r.get32(data + 48), static_cast<u32>(2 * Workbench::kScrollStep));
    CHECK_EQ(r.get32(data + 52), 0u);
    g_icon.free_object(object);

    r.workbench.close_drawer(ram);
    r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
    const u32 again = r.find_window("Ram Disk:");
    CHECK_EQ(r.workbench.window(again).scroll_x, 2 * Workbench::kScrollStep);
}

void drawers_open_below_the_screen_bar() {
    std::vector<AdfFile> files = test_files();
    files.push_back({"High.info", drawer_icon_info(static_cast<i32>(kNoIconPosition),
                                                   static_cast<i32>(kNoIconPosition), 30, 3,
                                                   240, 110)});
    Rig r(files);
    r.workbench.open_desktop();
    const u32 screen = r.get32(r.workbench.window(0).window + wd_WScreen);
    const u32 bar = r.get8(screen + sc_BarHeight);
    CHECK(bar > 0);

    // The Ram Disk has no Disk.info: its default box at the corner is no
    // place, so its window cascades.
    r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
    const u32 ram = r.workbench.window(r.find_window("Ram Disk:")).window;
    CHECK(r.get16(ram + wd_LeftEdge) != 0);
    CHECK(r.get16(ram + wd_TopEdge) > bar);

    // A box kept over the bar opens just below it.
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon_in(root, "High"));
    const u32 high = r.workbench.window(r.find_window("HombreTest:High")).window;
    CHECK_EQ(r.get16(high + wd_LeftEdge), 30u);
    CHECK_EQ(r.get16(high + wd_TopEdge), bar);
}

void every_disk_in_a_drive_gets_an_icon() {
    {
        const i32 nowhere = static_cast<i32>(kNoIconPosition);
        const std::vector<AdfFile> second_files = {
            {"Docs.info", drawer_icon_info(nowhere, nowhere, 30, 50, 300, 120)},
            {"Docs/Readme", text("second")},
        };
        Rig r(test_files(), build_adf("Second", false, second_files), 2);
        r.workbench.open_desktop();
        const u32 first = r.find_icon_in(0, "HombreTest");
        const u32 second = r.find_icon_in(0, "Second");
        const u32 ram = r.find_icon_in(0, "Ram Disk");
        CHECK(first != Workbench::kNoIcon);
        CHECK(second != Workbench::kNoIcon);
        CHECK(ram != Workbench::kNoIcon);
        CHECK(first < second && second < ram);
        CHECK(logged("workbench: volume Second"));
        r.workbench.open_icon(0, second);
        const u32 window = r.find_window("Second:");
        CHECK(window != Workbench::kNoWindow);
        CHECK(r.find_icon_in(window, "Docs") != Workbench::kNoIcon);
        CHECK(r.window_title(r.workbench.window(window).window) == "Second");
    }

    // Two disks of one name open their own drives.
    const std::vector<AdfFile> twin_files = {{"Only.info", icon_info(kWBTool, 10, 10)}};
    Rig twins(test_files(), build_adf("HombreTest", false, twin_files), 1);
    twins.workbench.open_desktop();
    u32 disks[2] = {Workbench::kNoIcon, Workbench::kNoIcon};
    u32 found = 0;
    for (const Workbench::IconEntry& icon : twins.workbench.icons()) {
        if (found < 2 && icon.owner == Workbench::kRootWindow &&
            std::string(icon.name) == "HombreTest") {
            disks[found++] = icon.id;
        }
    }
    CHECK_EQ(found, 2u);
    twins.workbench.open_icon(0, disks[0]);
    twins.workbench.open_icon(0, disks[1]);
    const u32 df0 = twins.find_window("DF0:");
    const u32 df1 = twins.find_window("DF1:");
    CHECK(df0 != Workbench::kNoWindow);
    CHECK(df1 != Workbench::kNoWindow);
    CHECK(twins.find_icon_in(df1, "Only") != Workbench::kNoIcon);
    CHECK(twins.find_icon_in(df0, "Tools") != Workbench::kNoIcon);
    CHECK(twins.window_title(twins.workbench.window(df1).window) == "HombreTest");
}

void a_hard_disk_partition_gets_an_icon_after_the_floppies() {
    {
        const i32 nowhere = static_cast<i32>(kNoIconPosition);
        const std::vector<AdfFile> work_files = {
            {"Docs.info", drawer_icon_info(nowhere, nowhere, 30, 50, 300, 120)},
            {"Docs/Readme", text("on the partition")},
        };
        Rig r(test_files(), build_adf("Work", true, work_files), kFloppyDrives);
        r.workbench.open_desktop();
        const u32 floppy = r.find_icon_in(0, "HombreTest");
        const u32 hard = r.find_icon_in(0, "Work");
        const u32 ram = r.find_icon_in(0, "Ram Disk");
        CHECK(floppy != Workbench::kNoIcon);
        CHECK(hard != Workbench::kNoIcon);
        CHECK(ram != Workbench::kNoIcon);
        CHECK(floppy < hard && hard < ram);
        CHECK(logged("workbench: volume Work"));
        r.workbench.open_icon(0, hard);
        const u32 window = r.find_window("Work:");
        CHECK(window != Workbench::kNoWindow);
        CHECK(r.find_icon_in(window, "Docs") != Workbench::kNoIcon);
    }

    // Two disks of one name open their own drives, by device name.
    const std::vector<AdfFile> twin_files = {{"Only.info", icon_info(kWBTool, 10, 10)}};
    Rig twins(test_files(), build_adf("HombreTest", true, twin_files), kFloppyDrives);
    twins.workbench.open_desktop();
    u32 disks[2] = {Workbench::kNoIcon, Workbench::kNoIcon};
    u32 found = 0;
    for (const Workbench::IconEntry& icon : twins.workbench.icons()) {
        if (found < 2 && icon.owner == Workbench::kRootWindow &&
            std::string(icon.name) == "HombreTest") {
            disks[found++] = icon.id;
        }
    }
    CHECK_EQ(found, 2u);
    twins.workbench.open_icon(0, disks[0]);
    twins.workbench.open_icon(0, disks[1]);
    const u32 df0 = twins.find_window("DF0:");
    const u32 dh0 = twins.find_window("DH0:");
    CHECK(df0 != Workbench::kNoWindow);
    CHECK(dh0 != Workbench::kNoWindow);
    CHECK(twins.find_icon_in(dh0, "Only") != Workbench::kNoIcon);
    CHECK(twins.find_icon_in(df0, "Tools") != Workbench::kNoIcon);
}

void snapshot_and_unsnapshot_store_the_icon_place() {
    Rig r;
    put_ram_project("Note");
    const TwoWindows w = open_tools_and_ram(r);
    const u32 note = r.find_icon_in(w.ram, "Note");
    CHECK(note != Workbench::kNoIcon);
    const Workbench::IconEntry start = r.workbench.icon(note);
    r.drag(w.ram, start.left + 3, start.top + 3, r.screen_x(w.ram, 123), r.screen_y(w.ram, 33));

    // Icons > Snapshot on the selected icon.
    r.workbench.menu_pick(w.ram, 2 | (4 << 5) | (31 << 11));
    CHECK(r.saved_place("RAM:Note") == std::make_pair(120u, 30u));
    r.rescan("Ram Disk:");
    const u32 again = r.find_icon_in(w.ram, "Note");
    CHECK(r.workbench.icon(again).placed);
    CHECK_EQ(r.workbench.icon(again).left, 120);
    CHECK_EQ(r.workbench.icon(again).top, 30);

    CHECK(r.workbench.unsnapshot_icon(w.ram, again));
    CHECK(r.saved_place("RAM:Note") == std::make_pair(kNoIconPosition, kNoIconPosition));
    CHECK(!r.workbench.icon(again).placed);
    CHECK_EQ(r.workbench.icon(again).left, start.left);
    CHECK_EQ(r.workbench.icon(again).top, start.top);

    // The disk cannot keep one.
    CHECK(!r.workbench.snapshot_icon(w.tools, r.find_icon_in(w.tools, "Tool")));
    CHECK(logged("workbench: cannot snapshot Tool: Disk is write-protected"));
}

// The screen position of a point of a request's gadget.
std::pair<i32, i32> gadget_point(const Rig& r, const Workbench::TextRequest& request, u16 id) {
    u32 gadget = request.gadgets;
    while (gadget != 0 && r.get16(gadget + ndk::gg_GadgetID) != id) {
        gadget = r.get32(gadget);
    }
    CHECK(gadget != 0);
    const auto word = [&](u32 at) { return static_cast<i32>(static_cast<i16>(r.get16(at))); };
    return {word(request.window + ndk::wd_LeftEdge) + word(gadget + ndk::gg_LeftEdge) + 4,
            word(request.window + ndk::wd_TopEdge) + word(gadget + ndk::gg_TopEdge) + 4};
}

void click_gadget(Rig& r, const Workbench::TextRequest& request, u16 id) {
    const std::pair<i32, i32> at = gadget_point(r, request, id);
    g_intuition.pointer_event(at.first, at.second, 1, 1, 0, true);
    g_intuition.pointer_event(at.first, at.second, 0, 0, 1, false);
}

void a_text_request_answers_return_and_its_buttons() {
    Rig r;
    r.workbench.open_desktop();
    char text[32]{};
    Workbench::TextRequest request{};
    const auto open = [&] {
        CHECK(r.workbench.open_text_request("Rename", "New name", "Old", sizeof text, request));
        CHECK(r.workbench.text_request_input(request, text, sizeof text) ==
              Workbench::Reply::Waiting);
    };
    const auto answered = [&](Workbench::Reply want) {
        CHECK(r.workbench.text_request_input(request, text, sizeof text) == want);
        r.workbench.close_dialog(request);
        CHECK_EQ(request.window, 0u);
    };
    open();
    const u32 free = r.free_bytes();
    // Return in the string gadget, which is being edited from the start.
    g_intuition.string_key('\r');
    answered(Workbench::Reply::Accepted);
    CHECK(std::string(text) == "Old");
    CHECK(r.free_bytes() > free);

    text[0] = '\0';
    open();
    click_gadget(r, request, Workbench::kRequestOk);
    answered(Workbench::Reply::Accepted);
    CHECK(std::string(text) == "Old");

    open();
    click_gadget(r, request, Workbench::kRequestCancel);
    answered(Workbench::Reply::Cancelled);

    // Opening and closing it again and again keeps no memory.
    open();
    r.workbench.close_dialog(request);
    const u32 before = r.free_bytes();
    open();
    r.workbench.close_dialog(request);
    CHECK_EQ(r.free_bytes(), before);
}

void a_set_answer_stands_in_for_the_requesters() {
    Rig r;
    r.workbench.open_desktop();
    char text[32]{};
    // Without a user to answer, a requester is cancelled.
    CHECK(!r.workbench.confirm("Delete", "Really?"));
    CHECK(!r.workbench.ask_text("Rename", "New name", "Old", text, sizeof text));
    std::string seen;
    r.workbench.set_answer(
        [](void* ctx, const char* title, const char*, char* out, u32 size) {
            *static_cast<std::string*>(ctx) += title;
            if (out != nullptr && size > 4) {
                __builtin_memcpy(out, "New", 4);
            }
            return true;
        },
        &seen);
    CHECK(r.workbench.confirm("Delete", "Really?"));
    CHECK(r.workbench.ask_text("Rename", "New name", "Old", text, sizeof text));
    CHECK(std::string(text) == "New");
    CHECK(seen == "DeleteRename");
}

// --- Icons > Information -------------------------------------------------

u32 info_gadget_at(const Rig& r, const Workbench::InfoRequest& request, u16 id) {
    for (u32 gadget = request.gadgets; gadget != 0; gadget = r.get32(gadget)) {
        if (r.get16(gadget + ndk::gg_GadgetID) == id) {
            return gadget;
        }
    }
    return 0;
}

u32 info_gadget(const Rig& r, const Workbench::InfoRequest& request, u16 id) {
    const u32 gadget = info_gadget_at(r, request, id);
    CHECK(gadget != 0);
    return gadget;
}

// Where a GadTools gadget keeps the text GTTX_Text gave it.
constexpr u32 kGadgetText = 52;

std::string info_value(const Rig& r, const Workbench::InfoRequest& request, u16 id) {
    return r.str(r.get32(info_gadget(r, request, id) + kGadgetText));
}

bool info_checked(const Rig& r, const Workbench::InfoRequest& request, u16 id) {
    return (r.get16(info_gadget(r, request, id) + ndk::gg_Flags) & GFLG_SELECTED) != 0;
}

bool info_disabled(const Rig& r, const Workbench::InfoRequest& request, u16 id) {
    return (r.get16(info_gadget(r, request, id) + ndk::gg_Flags) & ndk::GFLG_DISABLED) != 0;
}

// Where a GadTools gadget keeps its label and where the label goes.
constexpr u32 kGadgetPlace = 46;
constexpr u32 kGadgetLabel = 48;

std::string info_label(const Rig& r, const Workbench::InfoRequest& request, u16 id) {
    return r.str(r.get32(info_gadget(r, request, id) + kGadgetLabel));
}

u32 info_place(const Rig& r, const Workbench::InfoRequest& request, u16 id) {
    return r.get16(info_gadget(r, request, id) + kGadgetPlace);
}

struct GadgetBox {
    i32 left;
    i32 top;
    i32 width;
    i32 height;
};

GadgetBox gadget_box(const Rig& r, u32 gadget) {
    const auto word = [&](u32 at) { return static_cast<i32>(static_cast<i16>(r.get16(at))); };
    return {word(gadget + ndk::gg_LeftEdge), word(gadget + ndk::gg_TopEdge),
            word(gadget + ndk::gg_Width), word(gadget + ndk::gg_Height)};
}

std::string info_gadget_text(const Rig& r, u32 gadget) {
    const u32 string_info = r.get32(gadget + ndk::gg_SpecialInfo);
    return r.str(r.get32(string_info + ndk::si_Buffer));
}

std::string info_comment(const Rig& r, const Workbench::InfoRequest& request) {
    return info_gadget_text(r, request.comment_gadget);
}

std::string info_default_tool(const Rig& r, const Workbench::InfoRequest& request) {
    return info_gadget_text(r, info_gadget(r, request, Workbench::kInfoDefaultTool));
}

std::string info_tool_editor(const Rig& r, const Workbench::InfoRequest& request) {
    return info_gadget_text(r, info_gadget(r, request, Workbench::kInfoTool));
}

i32 info_stack(const Rig& r, const Workbench::InfoRequest& request) {
    const u32 gadget = info_gadget(r, request, Workbench::kInfoStack);
    const u32 string_info = r.get32(gadget + ndk::gg_SpecialInfo);
    return static_cast<i32>(r.get32(string_info + ndk::si_LongInt));
}

std::string info_row(const Rig& r, const Workbench::InfoRequest& request, u32 row) {
    return r.str(request.rows + row * (Workbench::kMaxToolTypeLength + 1));
}

void click_info(Rig& r, const Workbench::InfoRequest& request, u16 id) {
    const u32 gadget = info_gadget(r, request, id);
    const auto word = [&](u32 at) { return static_cast<i32>(static_cast<i16>(r.get16(at))); };
    const i32 x = word(request.window + ndk::wd_LeftEdge) + word(gadget + ndk::gg_LeftEdge) + 4;
    const i32 y = word(request.window + ndk::wd_TopEdge) + word(gadget + ndk::gg_TopEdge) + 4;
    g_intuition.pointer_event(x, y, 1, 1, 0, true);
    g_intuition.pointer_event(x, y, 0, 0, 1, false);
}

void click_info_row(Rig& r, const Workbench::InfoRequest& request, u32 row) {
    const u32 list = info_gadget(r, request, Workbench::kInfoToolTypes);
    const auto word = [&](u32 at) { return static_cast<i32>(static_cast<i16>(r.get16(at))); };
    const Graphics::FontMetrics font =
        g_graphics.font_metrics(g_intuition.font_of_screen(r.screen));
    const i32 row_height = font.y_size + 1;
    const i32 x = word(request.window + ndk::wd_LeftEdge) + word(list + ndk::gg_LeftEdge) + 4;
    const i32 y = word(request.window + ndk::wd_TopEdge) + word(list + ndk::gg_TopEdge) +
                  gadtools_defs::kListViewFrame + static_cast<i32>(row) * row_height +
                  row_height / 2;
    g_intuition.pointer_event(x, y, 1, 1, 0, true);
    g_intuition.pointer_event(x, y, 0, 0, 1, false);
}

// Types a value into an integer gadget, replacing what it held, and ends
// the editing with Return.
// Empties the string being edited, wherever the cursor is in it.
void erase_string(u32 length) {
    for (u32 i = 0; i < length; ++i) {
        g_intuition.string_key(0x08);
        g_intuition.string_key(0x7F);
    }
}

void set_info_integer(Rig& r, const Workbench::InfoRequest& request, u16 id, u32 value) {
    click_info(r, request, id);
    erase_string(12);
    for (const char c : std::to_string(value)) {
        g_intuition.string_key(static_cast<u8>(c));
    }
    g_intuition.string_key('\r');
}

// Types text into the active string gadget and ends it with Return.
void type_info_text(const std::string& text, bool erase = false) {
    if (erase) {
        erase_string(130);
    }
    for (const char c : text) {
        g_intuition.string_key(static_cast<u8>(c));
    }
    g_intuition.string_key('\r');
}

// The icon fields an .info file has.
u32 load_ram_object(const std::string& name) { return g_icon.load_object(("RAM:" + name).c_str()); }

std::string object_default_tool(const Rig& r, u32 object) {
    const u32 text = r.get32(object + do_DefaultTool);
    return text != 0 ? r.str(text) : std::string();
}

std::vector<std::string> object_tool_types(const Rig& r, u32 object) {
    std::vector<std::string> types;
    const u32 array = r.get32(object + do_ToolTypes);
    for (u32 i = 0; array != 0 && i < 2 * Workbench::kMaxToolTypes; ++i) {
        const u32 entry = r.get32(array + i * 4);
        if (entry == 0) {
            break;
        }
        types.push_back(r.str(entry));
    }
    return types;
}

void the_information_window_shows_a_file() {
    Rig r;
    put_ram_project("Note");
    u32 error = 0;
    CHECK(g_dos.set_file_comment("RAM:Note", "old note", error));
    CHECK(g_dos.set_protection_bits("RAM:Note", 0x11B1, error));
    const TwoWindows w = open_tools_and_ram(r);
    const u32 note = r.find_icon_in(w.ram, "Note");

    // The title is the drawer the file is in, the name and type one line.
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, note, request));
    CHECK(request.has_object);
    CHECK(r.window_title(request.window) == "Ram Disk:");
    CHECK_EQ(r.get32(request.window + wd_Flags) & WFLG_CLOSEGADGET, WFLG_CLOSEGADGET);

    CHECK(info_value(r, request, Workbench::kInfoName) == "Note (Project)");
    CHECK(info_label(r, request, Workbench::kInfoBlocks) == "Blocks:");
    CHECK(info_value(r, request, Workbench::kInfoBlocks) == "1");
    CHECK(info_label(r, request, Workbench::kInfoBytes) == "Bytes:");
    CHECK(info_value(r, request, Workbench::kInfoBytes) == "1");
    CHECK(info_label(r, request, Workbench::kInfoDate) == "Last Changed:");
    CHECK(!info_value(r, request, Workbench::kInfoDate).empty());
    CHECK(info_label(r, request, Workbench::kInfoComment) == "Comment:");
    CHECK(info_comment(r, request) == "old note");
    CHECK(info_label(r, request, Workbench::kInfoStack) == "Stack:");
    CHECK_EQ(info_stack(r, request), 4096);
    CHECK(info_label(r, request, Workbench::kInfoDefaultTool) == "Default Tool:");
    CHECK(info_default_tool(r, request).empty());
    CHECK_EQ(request.row_count, 0u);
    CHECK(info_label(r, request, Workbench::kInfoToolTypes) == "Tool Types:");
    CHECK(!info_checked(r, request, Workbench::kInfoScript));
    CHECK(info_checked(r, request, Workbench::kInfoArchived));
    CHECK(info_checked(r, request, Workbench::kInfoReadable));
    CHECK(info_checked(r, request, Workbench::kInfoWritable));
    CHECK(info_checked(r, request, Workbench::kInfoExecutable));
    CHECK(!info_checked(r, request, Workbench::kInfoDeletable));
    CHECK(!info_disabled(r, request, Workbench::kInfoScript));
    CHECK(!info_disabled(r, request, Workbench::kInfoComment));
    // A disk's lines are not a file's.
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoUsed), 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoStatus), 0u);

    // It fits the 640 by 400 screen with all its rows of tool types.
    const auto word = [&](u32 at) { return static_cast<i32>(static_cast<i16>(r.get16(at))); };
    const i32 left = word(request.window + ndk::wd_LeftEdge);
    const i32 top = word(request.window + ndk::wd_TopEdge);
    const i32 width = word(request.window + ndk::wd_Width);
    const i32 height = word(request.window + ndk::wd_Height);
    CHECK(left >= 0 && top >= 0 && left + width <= 640 && top + height <= 400);
    CHECK(height < 260);
    CHECK_EQ(request.visible_rows, Workbench::kInfoToolTypeRows);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    r.workbench.close_dialog(request);
    CHECK_EQ(request.window, 0u);
    CHECK_EQ(request.gadgets, 0u);

    // Opening and closing it again and again keeps no memory.
    CHECK(r.workbench.open_info(w.ram, note, request));
    r.workbench.close_dialog(request);
    const u32 before = r.free_bytes();
    CHECK(r.workbench.open_info(w.ram, note, request));
    r.workbench.close_dialog(request);
    CHECK_EQ(r.free_bytes(), before);
}

void the_information_window_is_laid_out_around_the_image() {
    Rig r;
    put_ram_typed("Typed", {"ONE=1"});
    const TwoWindows w = open_tools_and_ram(r);
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, r.find_icon_in(w.ram, "Typed"), request));
    const auto box = [&](u16 id) { return gadget_box(r, info_gadget(r, request, id)); };
    const i32 width = static_cast<i16>(r.get16(request.window + ndk::wd_Width));
    const i32 height = static_cast<i16>(r.get16(request.window + ndk::wd_Height));

    // The check boxes go down the right edge, a row each, in this order.
    const u16 boxes[] = {Workbench::kInfoScript,   Workbench::kInfoArchived,
                         Workbench::kInfoReadable, Workbench::kInfoWritable,
                         Workbench::kInfoExecutable, Workbench::kInfoDeletable};
    const i32 pitch = box(boxes[1]).top - box(boxes[0]).top;
    CHECK(pitch >= box(boxes[0]).height);
    for (u32 i = 0; i < 6; ++i) {
        const GadgetBox at = box(boxes[i]);
        CHECK_EQ(at.left + at.width, width - 8);
        CHECK_EQ(at.top, box(boxes[0]).top + static_cast<i32>(i) * pitch);
        CHECK_EQ(info_place(r, request, boxes[i]), gadtools_defs::PLACETEXT_LEFT);
    }
    // The left block shares their rows: blocks, bytes, then the stack.
    CHECK_EQ(box(Workbench::kInfoBlocks).top, box(Workbench::kInfoArchived).top);
    CHECK_EQ(box(Workbench::kInfoBytes).top, box(Workbench::kInfoReadable).top);
    CHECK_EQ(box(Workbench::kInfoBlocks).left, box(Workbench::kInfoStack).left);
    const GadgetBox stack = box(Workbench::kInfoStack);
    CHECK(stack.top >= box(Workbench::kInfoWritable).top);
    CHECK(stack.top + stack.height <= box(Workbench::kInfoDeletable).top);
    CHECK_EQ(box(Workbench::kInfoDate).top, box(Workbench::kInfoDeletable).top);

    // The frame stands between the stack and the labels of the boxes, the
    // name in the middle above it.
    CHECK(request.frame_left > stack.left + stack.width);
    CHECK(request.frame_left + request.frame_width <
          box(Workbench::kInfoExecutable).left - 10 * 8);
    CHECK(request.frame_width > 100);
    CHECK(request.frame_top + request.frame_height <= box(Workbench::kInfoDeletable).top);
    const GadgetBox name = box(Workbench::kInfoName);
    CHECK(name.top + name.height <= request.frame_top);
    const i32 off_middle =
        2 * name.left + name.width - (2 * request.frame_left + request.frame_width);
    CHECK(off_middle >= -1 && off_middle <= 1);
    // It is recessed, and the image is drawn in the middle of it.
    const i32 x1 = request.frame_left + request.frame_width - 1;
    const i32 y1 = request.frame_top + request.frame_height - 1;
    CHECK_EQ(r.window_pixel(request.window, request.frame_left, request.frame_top), 1u);
    CHECK_EQ(r.window_pixel(request.window, x1, y1), 2u);
    u32 drawn = 0;
    for (i32 y = request.frame_top + 1; y < y1; ++y) {
        for (i32 x = request.frame_left + 1; x < x1; ++x) {
            drawn += r.window_pixel(request.window, x, y) != 0 ? 1 : 0;
        }
    }
    CHECK(drawn > 0);
    CHECK_EQ(r.window_pixel(request.window, request.frame_left + 2, request.frame_top + 2), 0u);

    // Under the rows: the comment, the default tool and the tool types
    // share a column, their labels left of them.
    const GadgetBox comment = box(Workbench::kInfoComment);
    const GadgetBox tool = box(Workbench::kInfoDefaultTool);
    const GadgetBox list = box(Workbench::kInfoToolTypes);
    const GadgetBox edit = box(Workbench::kInfoTool);
    CHECK(comment.top >= box(Workbench::kInfoDeletable).top + pitch);
    CHECK(tool.top >= comment.top + comment.height);
    CHECK(list.top >= tool.top + tool.height);
    CHECK_EQ(comment.left, box(Workbench::kInfoDate).left);
    CHECK_EQ(tool.left, comment.left);
    CHECK_EQ(list.left, comment.left);
    CHECK_EQ(comment.left + comment.width, width - 8);
    for (const u16 id : {Workbench::kInfoComment, Workbench::kInfoDefaultTool,
                         Workbench::kInfoToolTypes, Workbench::kInfoStack,
                         Workbench::kInfoBlocks, Workbench::kInfoDate}) {
        CHECK_EQ(info_place(r, request, id), gadtools_defs::PLACETEXT_LEFT);
    }
    // The string that edits a row is right under the rows, as wide as the
    // list with its scroller, with New and Del left of it.
    CHECK_EQ(edit.top, list.top + list.height);
    CHECK_EQ(edit.left, list.left);
    CHECK_EQ(edit.left + edit.width, width - 8);
    const GadgetBox add = box(Workbench::kInfoNew);
    const GadgetBox del = box(Workbench::kInfoDel);
    CHECK_EQ(add.top, edit.top);
    CHECK_EQ(del.top, edit.top);
    CHECK_EQ(add.left, 8);
    CHECK(del.left > add.left + add.width);
    CHECK(del.left + del.width < edit.left);
    // Save and Cancel are the last row, at the two edges.
    const GadgetBox save = box(Workbench::kInfoSave);
    const GadgetBox cancel = box(Workbench::kInfoCancel);
    CHECK(save.top >= edit.top + edit.height);
    CHECK_EQ(cancel.top, save.top);
    CHECK_EQ(save.left, 8);
    CHECK_EQ(cancel.left + cancel.width, width - 8);
    CHECK(save.top + save.height < height);
    r.workbench.close_dialog(request);
}

void the_information_window_groups_the_digits_of_a_size() {
    Rig r;
    u32 error = 0;
    const std::vector<u8> bytes(83248, 'x');
    CHECK(g_dos.write_file("RAM:Big", bytes.data(), static_cast<u32>(bytes.size()), error));
    const u32 object = g_icon.default_object(kWBTool);
    CHECK(object != 0);
    CHECK(g_icon.save_object("RAM:Big", object, error));
    g_icon.free_object(object);
    const TwoWindows w = open_tools_and_ram(r);
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, r.find_icon_in(w.ram, "Big"), request));
    CHECK(info_value(r, request, Workbench::kInfoName) == "Big (Tool)");
    CHECK(info_value(r, request, Workbench::kInfoBytes) == "83 248");
    // A tool has no default tool to name.
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoDefaultTool), 0u);
    r.workbench.close_dialog(request);
}

void a_check_box_and_the_comment_save_to_the_file() {
    Rig r;
    put_ram_project("Note");
    u32 error = 0;
    CHECK(g_dos.set_file_comment("RAM:Note", "old note", error));
    CHECK(g_dos.set_protection_bits("RAM:Note", 0x11B1, error));
    const TwoWindows w = open_tools_and_ram(r);
    const u32 note = r.find_icon_in(w.ram, "Note");

    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, note, request));
    click_info(r, request, Workbench::kInfoScript);
    click_info(r, request, Workbench::kInfoReadable);
    CHECK(info_checked(r, request, Workbench::kInfoScript));
    CHECK(!info_checked(r, request, Workbench::kInfoReadable));
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    CHECK(logged("workbench: saved Ram Disk:Note"));
    CHECK_EQ(g_dos.file_info("RAM:Note").protection, 0x11F9u);
    CHECK(std::string(g_dos.file_info("RAM:Note").comment) == "old note");
    r.workbench.close_dialog(request);

    // The comment the window shows can be edited and saved.
    CHECK(r.workbench.open_info(w.ram, note, request));
    CHECK(info_comment(r, request) == "old note");
    CHECK(g_intuition.activate_string(request.comment_gadget, request.window));
    g_intuition.string_key('!');
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    CHECK(std::string(g_dos.file_info("RAM:Note").comment) == "old note!");
    CHECK_EQ(g_dos.file_info("RAM:Note").protection, 0x11F9u);
    r.workbench.close_dialog(request);
}

void information_saves_only_what_changed() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 proj = r.find_icon_in(w.tools, "Proj");
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.tools, proj, request));
    CHECK(r.window_title(request.window) == "HombreTest:Tools");
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    CHECK(!logged("workbench: cannot save Proj: Disk is write-protected"));
    r.workbench.close_dialog(request);

    // A protection bit that changed cannot be written to the floppy.
    CHECK(r.workbench.open_info(w.tools, proj, request));
    click_info(r, request, Workbench::kInfoScript);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(!r.workbench.save_info(request));
    CHECK(logged("workbench: cannot save Proj: Disk is write-protected"));
    r.workbench.close_dialog(request);
}

void information_cancels_without_changing_anything() {
    Rig r;
    put_ram_project("Note");
    u32 error = 0;
    CHECK(g_dos.set_file_comment("RAM:Note", "old", error));
    CHECK(g_dos.set_protection_bits("RAM:Note", 0x11B1, error));
    const TwoWindows w = open_tools_and_ram(r);
    const u32 note = r.find_icon_in(w.ram, "Note");

    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, note, request));
    CHECK(g_intuition.activate_string(request.comment_gadget, request.window));
    g_intuition.string_key('!');
    click_info(r, request, Workbench::kInfoScript);
    click_info(r, request, Workbench::kInfoCancel);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Cancelled);
    r.workbench.close_dialog(request);
    const Dos::FileInfo info = g_dos.file_info("RAM:Note");
    CHECK_EQ(info.protection, 0x11B1u);
    CHECK(std::string(info.comment) == "old");
}

void the_information_window_shows_a_drawer() {
    Rig r;
    u32 error = 0;
    CHECK(g_dos.make_dir("RAM:Box", error));
    const u32 object = g_icon.default_object(kWBDrawer);
    CHECK(object != 0);
    CHECK(g_icon.save_object("RAM:Box", object, error));
    g_icon.free_object(object);
    // The script and execute bits, which a drawer has no box for.
    CHECK(g_dos.set_protection_bits("RAM:Box", 0x42, error));
    CHECK(g_dos.make_dir("RAM:Trashcan", error));
    const u32 trashcan = g_icon.default_object(kWBGarbage);
    CHECK(trashcan != 0);
    CHECK(g_icon.save_object("RAM:Trashcan", trashcan, error));
    g_icon.free_object(trashcan);
    const TwoWindows w = open_tools_and_ram(r);
    const u32 box = r.find_icon_in(w.ram, "Box");
    CHECK(box != Workbench::kNoIcon);

    // A drawer has no sizes or stack and no Script or Executable box, but
    // a date, the other check boxes in their rows, a comment and tool types.
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, box, request));
    CHECK(info_value(r, request, Workbench::kInfoName) == "Box (Drawer)");
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoBlocks), 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoBytes), 0u);
    CHECK(!info_value(r, request, Workbench::kInfoDate).empty());
    CHECK(info_gadget_at(r, request, Workbench::kInfoComment) != 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoScript), 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoExecutable), 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoArchived) != 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoReadable) != 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoWritable) != 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoDeletable) != 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoStack), 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoDefaultTool), 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoToolTypes) != 0u);
    const i32 pitch = gadget_box(r, info_gadget(r, request, Workbench::kInfoReadable)).top -
                      gadget_box(r, info_gadget(r, request, Workbench::kInfoArchived)).top;
    CHECK_EQ(gadget_box(r, info_gadget(r, request, Workbench::kInfoDeletable)).top,
             gadget_box(r, info_gadget(r, request, Workbench::kInfoWritable)).top + 2 * pitch);

    // A box that changes leaves the bits without one as they were.
    click_info(r, request, Workbench::kInfoArchived);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    CHECK_EQ(g_dos.file_info("RAM:Box").protection, 0x52u);
    r.workbench.close_dialog(request);

    // A Trashcan shows what a drawer does, under its own type.
    CHECK(r.workbench.open_info(w.ram, r.find_icon_in(w.ram, "Trashcan"), request));
    CHECK(info_value(r, request, Workbench::kInfoName) == "Trashcan (Trashcan)");
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoBlocks), 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoStack), 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoScript), 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoExecutable), 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoDeletable) != 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoComment) != 0u);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoDefaultTool), 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoToolTypes) != 0u);
    r.workbench.close_dialog(request);
}

void information_shows_an_icon_without_an_object() {
    Rig r;
    u32 error = 0;
    const u32 object = g_icon.default_object(kWBTool);
    CHECK(object != 0);
    CHECK(g_icon.save_object("RAM:Lonely", object, error));
    g_icon.free_object(object);
    const TwoWindows w = open_tools_and_ram(r);
    r.rescan("Ram Disk:");
    const u32 lonely = r.find_icon_in(w.ram, "Lonely");
    CHECK(lonely != Workbench::kNoIcon);

    // Only the .info is there: the window is a tool's, with no sizes or
    // date to show and its check boxes and comment ghosted, and Save has
    // nothing to write.
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, lonely, request));
    CHECK(!request.has_object);
    CHECK(info_value(r, request, Workbench::kInfoName) == "Lonely (Tool)");
    CHECK(info_value(r, request, Workbench::kInfoBlocks).empty());
    CHECK(info_value(r, request, Workbench::kInfoBytes).empty());
    CHECK(info_value(r, request, Workbench::kInfoDate).empty());
    CHECK(info_disabled(r, request, Workbench::kInfoComment));
    for (const u16 id : {Workbench::kInfoScript, Workbench::kInfoArchived,
                         Workbench::kInfoReadable, Workbench::kInfoWritable,
                         Workbench::kInfoExecutable, Workbench::kInfoDeletable}) {
        CHECK(info_disabled(r, request, id));
        CHECK(!info_checked(r, request, id));
        click_info(r, request, id);
        CHECK(!info_checked(r, request, id));
    }
    CHECK(!info_disabled(r, request, Workbench::kInfoStack));
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    CHECK(!r.exists("RAM:Lonely"));
    r.workbench.close_dialog(request);

    // A disk's icon in a drawer and an icon that is not there are nothing
    // to show.
    CHECK(!r.workbench.open_info(w.tools, r.find_icon_in(w.tools, "Fake"), request));
    CHECK(logged("workbench: cannot show Fake: Error 212"));
    CHECK(!r.workbench.open_info(w.ram, Workbench::kNoIcon, request));
    CHECK(logged("workbench: cannot show ?: Error 212"));
}

void the_information_window_shows_a_tool_and_a_project() {
    Rig r;
    put_ram_typed("Typed", {"ONE=1", "TWO=2", "THREE=3"});
    const TwoWindows w = open_tools_and_ram(r);

    // The tool has no file: its icon fields still show.
    const u32 tool = r.find_icon_in(w.tools, "Tool");
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.tools, tool, request));
    CHECK(!request.has_object);
    CHECK(info_value(r, request, Workbench::kInfoName) == "Tool (Tool)");
    CHECK_EQ(info_stack(r, request), 4096);
    CHECK_EQ(info_gadget_at(r, request, Workbench::kInfoDefaultTool), 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoToolTypes) != 0u);
    CHECK_EQ(request.row_count, 0u);
    const i32 tool_height = static_cast<i16>(r.get16(request.window + ndk::wd_Height));
    r.workbench.close_dialog(request);

    // The project shows its default tool, stack and tool types, and is a
    // row taller for the default tool.
    const u32 typed = r.find_icon_in(w.ram, "Typed");
    CHECK(r.workbench.open_info(w.ram, typed, request));
    CHECK(request.has_object);
    CHECK(info_value(r, request, Workbench::kInfoName) == "Typed (Project)");
    CHECK(info_default_tool(r, request) == "OldTool");
    CHECK_EQ(info_stack(r, request), 4096);
    CHECK_EQ(request.row_count, 3u);
    CHECK(info_row(r, request, 0) == "ONE=1");
    CHECK(info_row(r, request, 1) == "TWO=2");
    CHECK(info_row(r, request, 2) == "THREE=3");
    CHECK(static_cast<i16>(r.get16(request.window + ndk::wd_Height)) > tool_height);
    r.workbench.close_dialog(request);
}

void tool_types_are_picked_edited_added_and_deleted() {
    Rig r;
    put_ram_typed("Typed", {"ONE=1", "TWO=2", "THREE=3"});
    const TwoWindows w = open_tools_and_ram(r);
    u32 typed = r.find_icon_in(w.ram, "Typed");
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, typed, request));

    // With no row selected, Del and the string are ghosted and do nothing;
    // New is not.
    CHECK(info_disabled(r, request, Workbench::kInfoDel));
    CHECK(info_disabled(r, request, Workbench::kInfoTool));
    CHECK(!info_disabled(r, request, Workbench::kInfoNew));
    click_info(r, request, Workbench::kInfoDel);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    CHECK_EQ(request.row_count, 3u);
    click_info(r, request, Workbench::kInfoTool);
    g_intuition.string_key('Z');
    g_intuition.string_key('\r');
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    CHECK_EQ(request.row_count, 3u);
    CHECK(info_row(r, request, 0) == "ONE=1");
    CHECK(info_tool_editor(r, request).empty());

    // A picked row's text is put in the string gadget, which works on it
    // with Del from then on.
    click_info_row(r, request, 1);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    CHECK_EQ(request.selected, 1u);
    CHECK(info_tool_editor(r, request) == "TWO=2");
    CHECK(!info_disabled(r, request, Workbench::kInfoDel));
    CHECK(!info_disabled(r, request, Workbench::kInfoTool));

    // Return in it replaces the row.
    type_info_text("X");
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    CHECK(info_row(r, request, 1) == "TWO=2X");

    // New adds an empty row at the end, selected and being edited.
    click_info(r, request, Workbench::kInfoNew);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    CHECK_EQ(request.row_count, 4u);
    CHECK_EQ(request.selected, 3u);
    CHECK(info_tool_editor(r, request).empty());
    type_info_text("FOUR=4");
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    CHECK(info_row(r, request, 3) == "FOUR=4");

    // Del removes the selected row and selects the one now in its place.
    click_info(r, request, Workbench::kInfoDel);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    CHECK_EQ(request.row_count, 3u);
    CHECK_EQ(request.selected, 2u);
    CHECK(info_tool_editor(r, request) == "THREE=3");

    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    const u32 object = load_ram_object("Typed");
    CHECK(object_tool_types(r, object) ==
          (std::vector<std::string>{"ONE=1", "TWO=2X", "THREE=3"}));
    g_icon.free_object(object);

    // With the last row deleted there is none to work on: both are
    // ghosted again, until New adds one.
    typed = r.find_icon_in(w.ram, "Typed");
    CHECK(r.workbench.open_info(w.ram, typed, request));
    click_info_row(r, request, 0);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    for (u32 left = 3; left > 0; --left) {
        CHECK(!info_disabled(r, request, Workbench::kInfoDel));
        click_info(r, request, Workbench::kInfoDel);
        CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
        CHECK_EQ(request.row_count, left - 1);
    }
    CHECK_EQ(request.selected, Workbench::kNoToolType);
    CHECK(info_disabled(r, request, Workbench::kInfoDel));
    CHECK(info_disabled(r, request, Workbench::kInfoTool));
    CHECK(info_tool_editor(r, request).empty());
    click_info(r, request, Workbench::kInfoNew);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    CHECK(!info_disabled(r, request, Workbench::kInfoDel));
    CHECK(!info_disabled(r, request, Workbench::kInfoTool));
    r.workbench.close_dialog(request);

    // New and Del keep no memory either.
    CHECK(r.workbench.open_info(w.ram, typed, request));
    r.workbench.close_dialog(request);
    const u32 before = r.free_bytes();
    CHECK(r.workbench.open_info(w.ram, typed, request));
    click_info(r, request, Workbench::kInfoNew);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoDel);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    r.workbench.close_dialog(request);
    CHECK_EQ(r.free_bytes(), before);
}

void a_long_tool_type_and_a_typed_one_survive_a_save() {
    Rig r;
    const std::string long_type(200, 'L');
    put_ram_typed("Typed", {long_type, "TWO=2"});
    const TwoWindows w = open_tools_and_ram(r);
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, r.find_icon_in(w.ram, "Typed"), request));
    // A new row typed into and saved without Return, past a long one that
    // was only looked at.
    click_info_row(r, request, 0);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoNew);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    for (const char c : std::string("TYPED")) {
        g_intuition.string_key(static_cast<u8>(c));
    }
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    u32 object = load_ram_object("Typed");
    CHECK(object_tool_types(r, object) ==
          std::vector<std::string>({long_type, "TWO=2", "TYPED"}));
    g_icon.free_object(object);

    // A new row left empty is not saved.
    CHECK(r.workbench.open_info(w.ram, r.find_icon_in(w.ram, "Typed"), request));
    click_info(r, request, Workbench::kInfoNew);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    object = load_ram_object("Typed");
    CHECK_EQ(object_tool_types(r, object).size(), 3u);
    g_icon.free_object(object);
}

void tool_types_past_the_cap_survive_a_save() {
    Rig r;
    std::vector<std::string> types;
    for (u32 i = 0; i < Workbench::kMaxToolTypes + 2; ++i) {
        types.push_back("TYPE" + std::to_string(i));
    }
    put_ram_typed("Many", types);
    const TwoWindows w = open_tools_and_ram(r);
    const u32 many = r.find_icon_in(w.ram, "Many");
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, many, request));
    CHECK_EQ(request.row_count, Workbench::kMaxToolTypes);
    click_info_row(r, request, 0);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    type_info_text("X");
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    const u32 object = load_ram_object("Many");
    const std::vector<std::string> saved = object_tool_types(r, object);
    CHECK_EQ(saved.size(), Workbench::kMaxToolTypes + 2);
    CHECK(saved[0] == "TYPE0X");
    CHECK(saved[Workbench::kMaxToolTypes] == "TYPE64");
    CHECK(saved[Workbench::kMaxToolTypes + 1] == "TYPE65");
    g_icon.free_object(object);
}

void information_saves_the_icon_fields() {
    Rig r;
    put_ram_typed("Typed", {"ONE=1", "TWO=2"});
    const TwoWindows w = open_tools_and_ram(r);
    Workbench::InfoRequest request{};
    u32 typed = r.find_icon_in(w.ram, "Typed");

    // Stack: 4096 to 8192.
    CHECK(r.workbench.open_info(w.ram, typed, request));
    set_info_integer(r, request, Workbench::kInfoStack, 8192);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    u32 object = load_ram_object("Typed");
    CHECK_EQ(r.get32(object + do_StackSize), 8192u);
    g_icon.free_object(object);

    // Default tool: OldTool to NewTool.
    typed = r.find_icon_in(w.ram, "Typed");
    CHECK(r.workbench.open_info(w.ram, typed, request));
    click_info(r, request, Workbench::kInfoDefaultTool);
    type_info_text("NewTool", true);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    object = load_ram_object("Typed");
    CHECK(object_default_tool(r, object) == "NewTool");
    CHECK_EQ(r.get32(object + do_StackSize), 8192u);
    g_icon.free_object(object);

    // Tool types: the first row gains Z.
    typed = r.find_icon_in(w.ram, "Typed");
    CHECK(r.workbench.open_info(w.ram, typed, request));
    click_info_row(r, request, 0);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    type_info_text("Z");
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    object = load_ram_object("Typed");
    CHECK(object_tool_types(r, object) == (std::vector<std::string>{"ONE=1Z", "TWO=2"}));
    CHECK(object_default_tool(r, object) == "NewTool");
    g_icon.free_object(object);
}

void information_cancels_the_icon_fields() {
    Rig r;
    put_ram_typed("Typed", {"ONE=1"});
    const TwoWindows w = open_tools_and_ram(r);
    const u32 typed = r.find_icon_in(w.ram, "Typed");
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, typed, request));
    set_info_integer(r, request, Workbench::kInfoStack, 8192);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoDefaultTool);
    type_info_text("Changed", true);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info_row(r, request, 0);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    type_info_text("ONE=2");
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoCancel);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Cancelled);
    r.workbench.close_dialog(request);
    const u32 object = load_ram_object("Typed");
    CHECK_EQ(r.get32(object + do_StackSize), 4096u);
    CHECK(object_default_tool(r, object) == "OldTool");
    CHECK(object_tool_types(r, object) == (std::vector<std::string>{"ONE=1"}));
    g_icon.free_object(object);
}

void an_icon_without_an_object_saves_its_icon_fields() {
    Rig r;
    put_ram_typed("Lonely", {"LONE=1"}, false);
    const TwoWindows w = open_tools_and_ram(r);
    const u32 lonely = r.find_icon_in(w.ram, "Lonely");
    CHECK(lonely != Workbench::kNoIcon);

    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(w.ram, lonely, request));
    CHECK(!request.has_object);
    CHECK_EQ(info_stack(r, request), 4096);
    CHECK_EQ(request.row_count, 1u);
    CHECK(info_row(r, request, 0) == "LONE=1");
    set_info_integer(r, request, Workbench::kInfoStack, 2048);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    CHECK(!r.exists("RAM:Lonely"));
    const u32 object = load_ram_object("Lonely");
    CHECK_EQ(r.get32(object + do_StackSize), 2048u);
    CHECK(object_tool_types(r, object) == (std::vector<std::string>{"LONE=1"}));
    g_icon.free_object(object);
}

void the_information_window_shows_a_disk() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    (void)w;
    const u32 ram = r.find_icon_in(0, "Ram Disk");
    Workbench::InfoRequest request{};
    CHECK(r.workbench.open_info(0, ram, request));
    CHECK(request.has_object);
    CHECK(request.is_disk);
    CHECK(r.window_title(request.window) == "Ram Disk:");
    CHECK(info_value(r, request, Workbench::kInfoName) == "Ram Disk (Volume)");

    // Its blocks, those used and free and the size of one, each on a row,
    // the date it was made and whether it can be written.
    u32 blocks = 0;
    u32 used = 0;
    CHECK(g_dos.volume_usage("RAM:", blocks, used));
    CHECK(info_label(r, request, Workbench::kInfoBlocks) == "Blocks:");
    CHECK(info_value(r, request, Workbench::kInfoBlocks) == std::to_string(blocks));
    CHECK(info_label(r, request, Workbench::kInfoUsed) == "Used:");
    CHECK(info_value(r, request, Workbench::kInfoUsed) == std::to_string(used));
    CHECK(info_label(r, request, Workbench::kInfoFree) == "Free:");
    CHECK(info_value(r, request, Workbench::kInfoFree) == std::to_string(blocks - used));
    CHECK(info_label(r, request, Workbench::kInfoBlockSize) == "Block size:");
    CHECK(info_value(r, request, Workbench::kInfoBlockSize) == "512");
    CHECK(info_label(r, request, Workbench::kInfoDate) == "Created:");
    CHECK(!info_value(r, request, Workbench::kInfoDate).empty());
    CHECK(info_label(r, request, Workbench::kInfoStatus) == "Status:");
    CHECK(info_value(r, request, Workbench::kInfoStatus) == "Read/Write");
    const auto top_of = [&](u16 id) { return gadget_box(r, info_gadget(r, request, id)).top; };
    const i32 pitch = top_of(Workbench::kInfoUsed) - top_of(Workbench::kInfoBlocks);
    CHECK(pitch > 0);
    CHECK_EQ(top_of(Workbench::kInfoFree), top_of(Workbench::kInfoUsed) + pitch);
    CHECK_EQ(top_of(Workbench::kInfoBlockSize), top_of(Workbench::kInfoFree) + pitch);
    CHECK_EQ(top_of(Workbench::kInfoDate), top_of(Workbench::kInfoBlockSize) + pitch);
    CHECK_EQ(top_of(Workbench::kInfoStatus), top_of(Workbench::kInfoDate));

    // It has no file's lines, check boxes or comment, but a default tool
    // and tool types.
    for (const u16 id : {Workbench::kInfoBytes, Workbench::kInfoStack, Workbench::kInfoComment,
                         Workbench::kInfoScript, Workbench::kInfoArchived,
                         Workbench::kInfoReadable, Workbench::kInfoWritable,
                         Workbench::kInfoExecutable, Workbench::kInfoDeletable}) {
        CHECK_EQ(info_gadget_at(r, request, id), 0u);
    }
    CHECK(info_gadget_at(r, request, Workbench::kInfoDefaultTool) != 0u);
    CHECK(info_gadget_at(r, request, Workbench::kInfoToolTypes) != 0u);
    CHECK(request.frame_width > 100);

    // Saved unchanged it writes nothing; a tool type added gives the Ram
    // Disk a Disk.info, which its icon then shows.
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    CHECK(!r.exists("RAM:Disk.info"));
    r.workbench.close_dialog(request);
    CHECK(r.workbench.open_info(0, ram, request));
    click_info(r, request, Workbench::kInfoNew);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    type_info_text("KEEP=1");
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
    click_info(r, request, Workbench::kInfoSave);
    CHECK(r.workbench.info_input(request) == Workbench::Reply::Accepted);
    CHECK(r.workbench.save_info(request));
    r.workbench.close_dialog(request);
    CHECK(r.exists("RAM:Disk.info"));
    const u32 object = load_ram_object("Disk");
    CHECK(object != 0);
    CHECK_EQ(r.get8(object + do_Type), kWBDisk);
    CHECK(object_tool_types(r, object) == (std::vector<std::string>{"KEEP=1"}));
    g_icon.free_object(object);
    CHECK(r.workbench.open_info(0, ram, request));
    CHECK_EQ(request.row_count, 1u);
    CHECK(info_row(r, request, 0) == "KEEP=1");
    r.workbench.close_dialog(request);

    // A disk that cannot be written says so.
    const u32 floppy = r.find_icon_in(0, "HombreTest");
    CHECK(r.workbench.open_info(0, floppy, request));
    CHECK(r.window_title(request.window) == "HombreTest:");
    CHECK(info_value(r, request, Workbench::kInfoName) == "HombreTest (Volume)");
    CHECK(info_value(r, request, Workbench::kInfoBlocks) == "1 758");
    CHECK(info_value(r, request, Workbench::kInfoBlockSize) == "488");
    CHECK(info_value(r, request, Workbench::kInfoStatus) == "Read Only");
    r.workbench.close_dialog(request);

    // Opening and closing a disk's window keeps no memory.
    const u32 before = r.free_bytes();
    CHECK(r.workbench.open_info(0, ram, request));
    r.workbench.close_dialog(request);
    CHECK_EQ(r.free_bytes(), before);
}

void the_information_window_fits_a_short_screen() {
    for (const u32 screen_height : {200u, 256u}) {
        Rig r(test_files(), {}, 1, 2, screen_height);
        put_ram_typed("Typed", {"ONE=1", "TWO=2", "THREE=3", "FOUR=4", "FIVE=5", "SIX=6"});
        r.workbench.open_desktop();
        r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
        const u32 ram = r.find_window("Ram Disk:");
        CHECK(ram != Workbench::kNoWindow);

        // The project's window is the tallest: it stays on the screen, with
        // fewer rows of tool types on the shorter one, all its gadgets
        // inside it.
        Workbench::InfoRequest request{};
        CHECK(r.workbench.open_info(ram, r.find_icon_in(ram, "Typed"), request));
        const i32 top = static_cast<i16>(r.get16(request.window + ndk::wd_TopEdge));
        const i32 height = static_cast<i16>(r.get16(request.window + ndk::wd_Height));
        CHECK(top >= 0);
        CHECK(top + height <= static_cast<i32>(screen_height));
        CHECK(request.visible_rows >= 1u);
        if (screen_height == 200) {
            CHECK(request.visible_rows < Workbench::kInfoToolTypeRows);
        } else {
            CHECK_EQ(request.visible_rows, Workbench::kInfoToolTypeRows);
        }
        const GadgetBox save = gadget_box(r, info_gadget(r, request, Workbench::kInfoSave));
        CHECK(save.top + save.height < height);

        // The rows that do not show are scrolled to.
        click_info_row(r, request, 0);
        CHECK(r.workbench.info_input(request) == Workbench::Reply::Waiting);
        CHECK_EQ(request.selected, 0u);
        CHECK(info_tool_editor(r, request) == "ONE=1");
        r.workbench.close_dialog(request);
    }
}

// A drawer in RAM: holding a file and a drawer with a file, with its icon.
void put_ram_drawer(Rig& r, const std::string& name) {
    r.make_dir("RAM:" + name);
    r.make_dir("RAM:" + name + "/Deep");
    put_ram_project(name + "/One");
    u32 error = 0;
    const u8 byte = 'y';
    CHECK(g_dos.write_file(("RAM:" + name + "/Deep/Two").c_str(), &byte, 1, error));
    const u32 object = g_icon.default_object(kWBDrawer);
    CHECK(object != 0);
    CHECK(g_icon.save_object(("RAM:" + name).c_str(), object, error));
    g_icon.free_object(object);
}

void click_icon(Rig& r, u32 window, u32 icon) {
    const Workbench::IconEntry at = r.workbench.icon(icon);
    r.press(window, at.left + 3, at.top + 3);
    r.release(r.screen_x(window, at.left + 3), r.screen_y(window, at.top + 3));
}

// A click with Shift held, adding the icon to the selection.
void shift_click_icon(Rig& r, u32 window, u32 icon) {
    g_intuition.set_qualifier(1);
    click_icon(r, window, icon);
    g_intuition.set_qualifier(0);
}

// The number of the Icons menu's item.
u32 icons_pick(u32 item) { return 2 | (item << 5) | (31 << 11); }

void delete_removes_a_file_or_a_drawer_and_its_icon() {
    Rig r;
    put_ram_project("Note");
    const TwoWindows w = open_tools_and_ram(r);
    put_ram_drawer(r, "Box");
    r.rescan("Ram Disk:");
    CHECK(r.workbench.delete_icon(w.ram, r.find_icon_in(w.ram, "Note")));
    CHECK(!r.exists("RAM:Note"));
    CHECK(!r.exists("RAM:Note.info"));
    CHECK_EQ(r.find_icon_in(w.ram, "Note"), Workbench::kNoIcon);

    // A drawer goes with all that is in it, and its window closes.
    r.workbench.open_icon(w.ram, r.find_icon_in(w.ram, "Box"));
    const u32 box = r.find_window("Ram Disk:Box");
    CHECK(box != Workbench::kNoWindow);
    CHECK(r.workbench.delete_icon(w.ram, r.find_icon_in(w.ram, "Box")));
    CHECK(!r.workbench.has_window(box));
    CHECK(!r.exists("RAM:Box"));
    CHECK(!r.exists("RAM:Box.info"));
    CHECK(logged("workbench: deleted Ram Disk:Box"));
}

void delete_refuses_disks_and_what_cannot_be_written() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    CHECK(!r.workbench.delete_icon(0, r.find_icon_in(0, "Ram Disk")));
    CHECK(!r.workbench.delete_icon(w.tools, r.find_icon_in(w.tools, "Tool")));
    CHECK(logged("workbench: cannot delete Tool: Disk is write-protected"));
    CHECK(r.find_icon_in(w.tools, "Tool") != Workbench::kNoIcon);
}

void the_delete_item_asks_before_it_deletes() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    put_ram_drawer(r, "Box");
    r.rescan("Ram Disk:");
    struct Asked {
        bool answer = false;
        std::string text;
    } asked;
    r.workbench.set_answer(
        [](void* ctx, const char*, const char* body, char*, u32) {
            Asked& a = *static_cast<Asked*>(ctx);
            a.text = body;
            return a.answer;
        },
        &asked);
    click_icon(r, w.ram, r.find_icon_in(w.ram, "Box"));
    r.workbench.menu_pick(w.ram, icons_pick(8));
    CHECK(asked.text.find("OK to delete Box") != std::string::npos);
    // One, One.info, Deep and Two.
    CHECK(asked.text.find("and the 4 objects in it?") != std::string::npos);
    CHECK(r.exists("RAM:Box/Deep/Two"));
    asked.answer = true;
    r.workbench.menu_pick(w.ram, icons_pick(8));
    CHECK(!r.exists("RAM:Box"));
}

void rename_gives_the_file_and_its_icon_the_new_name() {
    Rig r;
    put_ram_project("Note");
    put_ram_project("Other");
    const TwoWindows w = open_tools_and_ram(r);
    CHECK(r.workbench.rename_icon(w.ram, r.find_icon_in(w.ram, "Note"), "Memo"));
    CHECK(r.exists("RAM:Memo"));
    CHECK(r.exists("RAM:Memo.info"));
    CHECK(!r.exists("RAM:Note"));
    CHECK(!r.exists("RAM:Note.info"));
    const u32 memo = r.find_icon_in(w.ram, "Memo");
    CHECK(memo != Workbench::kNoIcon);

    // A name that is taken or no name leaves things as they were.
    CHECK(!r.workbench.rename_icon(w.ram, memo, "Other"));
    CHECK(logged("workbench: cannot rename Memo: Object already exists"));
    CHECK(!r.workbench.rename_icon(w.ram, r.find_icon_in(w.ram, "Memo"), ""));
    CHECK(!r.workbench.rename_icon(w.ram, r.find_icon_in(w.ram, "Memo"), "a/b"));
    CHECK(!r.workbench.rename_icon(w.ram, r.find_icon_in(w.ram, "Memo"),
                                   std::string(31, 'n').c_str()));
    CHECK(r.exists("RAM:Memo"));
    CHECK(!r.workbench.rename_icon(0, r.find_icon_in(0, "Ram Disk"), "Disk"));
}

void a_rename_that_loses_the_icon_name_is_undone() {
    Rig r;
    put_ram_project("Note");
    u32 error = 0;
    const u8 byte = 'z';
    CHECK(g_dos.write_file("RAM:Taken.info", &byte, 1, error));
    const TwoWindows w = open_tools_and_ram(r);
    CHECK(!r.workbench.rename_icon(w.ram, r.find_icon_in(w.ram, "Note"), "Taken"));
    CHECK(r.exists("RAM:Note"));
    CHECK(r.exists("RAM:Note.info"));
    CHECK(!r.exists("RAM:Taken"));
}

void the_rename_item_asks_for_the_name() {
    Rig r;
    put_ram_project("Note");
    const TwoWindows w = open_tools_and_ram(r);
    r.workbench.set_answer(
        [](void*, const char*, const char*, char* text, u32 size) {
            CHECK(std::string(text) == "Note");
            CHECK(size > 4);
            __builtin_memcpy(text, "Memo", 5);
            return true;
        },
        nullptr);
    click_icon(r, w.ram, r.find_icon_in(w.ram, "Note"));
    r.workbench.menu_pick(w.ram, icons_pick(2));
    CHECK(r.exists("RAM:Memo"));
}

void new_drawer_makes_a_drawer_with_its_icon() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    CHECK(r.workbench.new_drawer(w.ram, "Box"));
    CHECK(r.exists("RAM:Box"));
    CHECK(r.exists("RAM:Box.info"));
    const u32 box = r.find_icon_in(w.ram, "Box");
    CHECK(box != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(box).type, kWBDrawer);

    CHECK(!r.workbench.new_drawer(w.ram, "Box"));
    CHECK(!r.workbench.new_drawer(w.ram, "a:b"));
    CHECK(!r.workbench.new_drawer(0, "Root"));
    CHECK(!r.workbench.new_drawer(w.tools, "NotHere"));
    CHECK(logged("workbench: cannot make NotHere: Disk is write-protected"));
}

void the_new_drawer_item_suggests_a_free_name() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    r.workbench.set_answer([](void*, const char*, const char*, char*, u32) { return true; },
                           nullptr);
    // Window > New Drawer, twice with the suggested name.
    r.workbench.menu_pick(w.ram, 1 | (0 << 5) | (31 << 11));
    r.workbench.menu_pick(w.ram, 1 | (0 << 5) | (31 << 11));
    CHECK(r.exists("RAM:Unnamed1"));
    CHECK(r.exists("RAM:Unnamed2"));
}

void copy_duplicates_a_file_or_a_drawer_under_a_new_name() {
    Rig r;
    put_ram_project("Note");
    const TwoWindows w = open_tools_and_ram(r);
    put_ram_drawer(r, "Box");
    r.rescan("Ram Disk:");
    const u32 note = r.find_icon_in(w.ram, "Note");
    CHECK(r.workbench.copy_icon(w.ram, note));
    CHECK(r.exists("RAM:copy_of_Note"));
    CHECK(r.exists("RAM:copy_of_Note.info"));
    CHECK(r.saved_place("RAM:copy_of_Note") ==
          std::make_pair(kNoIconPosition, kNoIconPosition));
    CHECK(r.find_icon_in(w.ram, "copy_of_Note") != Workbench::kNoIcon);
    CHECK(r.workbench.copy_icon(w.ram, r.find_icon_in(w.ram, "Note")));
    CHECK(r.exists("RAM:copy_2_of_Note"));

    // A drawer with all that is in it, picked from the menu.
    click_icon(r, w.ram, r.find_icon_in(w.ram, "Box"));
    r.workbench.menu_pick(w.ram, icons_pick(1));
    CHECK(r.exists("RAM:copy_of_Box/One.info"));
    CHECK(r.exists("RAM:copy_of_Box/Deep/Two"));
    CHECK(r.exists("RAM:copy_of_Box.info"));
    CHECK(r.exists("RAM:Box/Deep/Two"));
}

void copy_keeps_attributes_and_all_of_the_icon() {
    Rig r;
    u32 error = 0;
    const u8 byte = 'x';
    CHECK(g_dos.write_file("RAM:Run", &byte, 1, error));
    std::vector<u8> info = icon_info(kWBTool, 20, 30);
    const std::string trailer = "FORMtrailer";
    info.insert(info.end(), trailer.begin(), trailer.end());
    CHECK(g_dos.write_file("RAM:Run.info", info.data(), static_cast<u32>(info.size()), error));
    CHECK(g_dos.set_file_comment("RAM:Run", "kept", error));
    CHECK(g_dos.set_protection_bits("RAM:Run", 0x41, error));
    const TwoWindows w = open_tools_and_ram(r);
    CHECK(r.workbench.copy_icon(w.ram, r.find_icon_in(w.ram, "Run")));
    const Dos::FileInfo copy = g_dos.file_info("RAM:copy_of_Run");
    CHECK_EQ(copy.protection, 0x41u);
    CHECK(std::string(copy.comment) == "kept");
    CHECK_EQ(g_dos.examine("RAM:copy_of_Run.info").size, static_cast<u32>(info.size()));
    CHECK(r.saved_place("RAM:copy_of_Run") ==
          std::make_pair(kNoIconPosition, kNoIconPosition));
}

void copy_refuses_disks_and_keeps_names_short() {
    Rig r;
    const std::string long_name(25, 'n');
    put_ram_project(long_name);
    const TwoWindows w = open_tools_and_ram(r);
    CHECK(r.workbench.copy_icon(w.ram, r.find_icon_in(w.ram, long_name)));
    CHECK(r.exists("RAM:copy_of_" + std::string(17, 'n') + ".info"));
    CHECK(!r.workbench.copy_icon(0, r.find_icon_in(0, "Ram Disk")));
    CHECK(!r.workbench.copy_icon(w.tools, r.find_icon_in(w.tools, "Tool")));
    CHECK(logged("workbench: cannot copy Tool: Disk is write-protected"));
}

void the_copy_item_copies_every_selected_icon() {
    Rig r;
    put_ram_project("Note");
    put_ram_project("Other");
    const TwoWindows w = open_tools_and_ram(r);
    click_icon(r, w.ram, r.find_icon_in(w.ram, "Note"));
    shift_click_icon(r, w.ram, r.find_icon_in(w.ram, "Other"));
    CHECK_EQ(r.workbench.window(w.ram).clicks.selected().size(), 2u);
    r.workbench.menu_pick(w.ram, icons_pick(1));
    CHECK(r.exists("RAM:copy_of_Note"));
    CHECK(r.exists("RAM:copy_of_Other"));
    CHECK(r.find_icon_in(w.ram, "copy_of_Note") != Workbench::kNoIcon);
    CHECK(r.find_icon_in(w.ram, "copy_of_Other") != Workbench::kNoIcon);
}

void the_delete_item_asks_once_for_several_icons() {
    Rig r;
    put_ram_project("Note");
    put_ram_drawer(r, "Box");
    const TwoWindows w = open_tools_and_ram(r);
    struct Asked {
        u32 calls = 0;
        std::string text;
        bool answer = false;
    } asked;
    r.workbench.set_answer(
        [](void* ctx, const char*, const char* body, char*, u32) {
            Asked& a = *static_cast<Asked*>(ctx);
            ++a.calls;
            a.text = body;
            return a.answer;
        },
        &asked);
    click_icon(r, w.ram, r.find_icon_in(w.ram, "Note"));
    shift_click_icon(r, w.ram, r.find_icon_in(w.ram, "Box"));

    r.workbench.menu_pick(w.ram, icons_pick(8));
    CHECK_EQ(asked.calls, 1u);
    CHECK(asked.text == "Warning: you cannot get back\nwhat you delete!\n"
                        "OK to delete 2 icons\nand the 4 objects in them?");
    CHECK(r.exists("RAM:Note"));
    CHECK(r.exists("RAM:Box"));

    asked.answer = true;
    r.workbench.menu_pick(w.ram, icons_pick(8));
    CHECK_EQ(asked.calls, 2u);
    CHECK(!r.exists("RAM:Note"));
    CHECK(!r.exists("RAM:Box"));
    CHECK_EQ(r.find_icon_in(w.ram, "Note"), Workbench::kNoIcon);
    CHECK_EQ(r.find_icon_in(w.ram, "Box"), Workbench::kNoIcon);
}

void the_rename_item_renames_every_selected_icon() {
    Rig r;
    put_ram_project("Note");
    put_ram_project("Other");
    const TwoWindows w = open_tools_and_ram(r);
    std::vector<std::string> asked;
    r.workbench.set_answer(
        [](void* ctx, const char*, const char*, char* text, u32 size) {
            auto& names = *static_cast<std::vector<std::string>*>(ctx);
            names.push_back(text);
            const char* name = names.size() == 1u ? "Memo" : "Later";
            const usize length = std::strlen(name) + 1;
            CHECK(length <= size);
            __builtin_memcpy(text, name, length);
            return true;
        },
        &asked);
    click_icon(r, w.ram, r.find_icon_in(w.ram, "Note"));
    shift_click_icon(r, w.ram, r.find_icon_in(w.ram, "Other"));
    r.workbench.menu_pick(w.ram, icons_pick(2));
    CHECK(asked == std::vector<std::string>({"Note", "Other"}));
    CHECK(r.exists("RAM:Memo"));
    CHECK(r.exists("RAM:Later"));
    CHECK(!r.exists("RAM:Note"));
    CHECK(!r.exists("RAM:Other"));
}

void the_snapshot_item_snapshots_every_selected_icon() {
    Rig r;
    put_ram_project("Note");
    put_ram_project("Other");
    const TwoWindows w = open_tools_and_ram(r);
    const Workbench::IconEntry note = r.workbench.icon(r.find_icon_in(w.ram, "Note"));
    const Workbench::IconEntry other = r.workbench.icon(r.find_icon_in(w.ram, "Other"));
    r.drag(w.ram, note.left + 3, note.top + 3, r.screen_x(w.ram, 43), r.screen_y(w.ram, 33));
    r.drag(w.ram, other.left + 3, other.top + 3, r.screen_x(w.ram, 143),
           r.screen_y(w.ram, 33));
    click_icon(r, w.ram, r.find_icon_in(w.ram, "Note"));
    shift_click_icon(r, w.ram, r.find_icon_in(w.ram, "Other"));
    r.workbench.menu_pick(w.ram, icons_pick(4));
    CHECK(r.saved_place("RAM:Note") == std::make_pair(40u, 30u));
    CHECK(r.saved_place("RAM:Other") == std::make_pair(140u, 30u));
}

void execute_command_hands_the_line_to_a_shell() {
    Rig r;
    r.workbench.open_desktop();
    // With no shell to run it in, the command is refused.
    CHECK(!r.workbench.execute_command("Dir"));
    std::vector<std::string> ran;
    r.workbench.set_run_command(
        [](void* ctx, const char* command) {
            static_cast<std::vector<std::string>*>(ctx)->push_back(command);
            return true;
        },
        &ran);
    CHECK(!r.workbench.execute_command(""));
    std::vector<std::string> offered;
    r.workbench.set_answer(
        [](void* ctx, const char*, const char*, char* text, u32 size) {
            static_cast<std::vector<std::string>*>(ctx)->push_back(text);
            CHECK(size > 16);
            __builtin_memcpy(text, "List RAM:", 10);
            return true;
        },
        &offered);
    // Workbench > Execute Command, twice: the last line is offered again.
    r.workbench.menu_pick(0, 0 | (1 << 5) | (31 << 11));
    r.workbench.menu_pick(0, 0 | (1 << 5) | (31 << 11));
    CHECK(ran == std::vector<std::string>({"List RAM:", "List RAM:"}));
    CHECK(offered == std::vector<std::string>({"", "List RAM:"}));
}

void an_icon_without_a_place_keeps_clear_of_the_others() {
    Rig r;
    put_ram_project("Loose");
    const TwoWindows w = open_tools_and_ram(r);
    const Workbench::IconEntry first = r.workbench.icon(r.find_icon_in(w.ram, "Loose"));

    // An icon snapshot where the grid would put the other one.
    put_ram_project("Fixed");
    r.rescan("Ram Disk:");
    const u32 fixed = r.find_icon_in(w.ram, "Fixed");
    const Workbench::IconEntry at = r.workbench.icon(fixed);
    r.drag(w.ram, at.left + 3, at.top + 3, r.screen_x(w.ram, first.left + 3),
           r.screen_y(w.ram, first.top + 3));
    CHECK(r.workbench.snapshot_icon(w.ram, fixed));
    r.rescan("Ram Disk:");

    const Workbench::IconEntry kept = r.workbench.icon(r.find_icon_in(w.ram, "Fixed"));
    const Workbench::IconEntry loose = r.workbench.icon(r.find_icon_in(w.ram, "Loose"));
    CHECK_EQ(kept.left, first.left);
    CHECK_EQ(kept.top, first.top);
    CHECK(loose.left >= kept.left + kept.width || loose.top >= kept.top + kept.height);
}

std::string ram_text(const std::string& path) {
    u8 data[256]{};
    u32 got = 0;
    u32 error = 0;
    if (!g_dos.read_file(path.c_str(), data, sizeof data, got, error)) {
        return "";
    }
    return std::string(reinterpret_cast<char*>(data), got);
}

void leave_out_keeps_an_icon_on_the_root_window() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    put_ram_drawer(r, "Box");
    put_ram_project("Note");
    r.rescan("Ram Disk:");
    CHECK(r.workbench.leave_out(w.ram, r.find_icon_in(w.ram, "Note")));
    CHECK(ram_text("RAM:.backdrop") == ":Note\n");
    const u32 out = r.find_icon_in(0, "Note");
    CHECK(out != Workbench::kNoIcon);
    CHECK(r.find_icon_in(w.ram, "Note") != Workbench::kNoIcon);
    // Once is enough.
    CHECK(r.workbench.leave_out(w.ram, r.find_icon_in(w.ram, "Note")));
    CHECK(ram_text("RAM:.backdrop") == ":Note\n");

    // An icon in a drawer is listed with its path from the volume.
    r.workbench.open_icon(w.ram, r.find_icon_in(w.ram, "Box"));
    const u32 box = r.find_window("Ram Disk:Box");
    CHECK(r.workbench.leave_out(box, r.find_icon_in(box, "One")));
    CHECK(ram_text("RAM:.backdrop") == ":Note\n:Box/One\n");

    // A left out icon keeps no snapshot and is no file to delete.
    CHECK(!r.workbench.snapshot_icon(0, out));
    CHECK(!r.workbench.delete_icon(0, out));
    CHECK(!r.workbench.put_away(r.find_icon_in(0, "Ram Disk")));

    CHECK(r.workbench.put_away(out));
    CHECK(ram_text("RAM:.backdrop") == ":Box/One\n");
    CHECK_EQ(r.find_icon_in(0, "Note"), Workbench::kNoIcon);
    CHECK(r.workbench.put_away(r.find_icon_in(0, "One")));
    CHECK(!r.exists("RAM:.backdrop"));
    CHECK(!r.workbench.leave_out(w.tools, r.find_icon_in(w.tools, "Tool")));
    CHECK(logged("workbench: cannot leave out Tool: Disk is write-protected"));
}

void a_left_out_icon_follows_its_file() {
    Rig r;
    r.workbench.open_desktop();
    r.make_dir("RAM:Box");
    u32 error = 0;
    const u32 object = g_icon.default_object(kWBDrawer);
    CHECK(g_icon.save_object("RAM:Box", object, error));
    g_icon.free_object(object);
    put_ram_project("Box/One");
    put_ram_project("Note");
    const TwoWindows w = open_tools_and_ram(r);
    r.workbench.open_icon(w.ram, r.find_icon_in(w.ram, "Box"));
    const u32 box = r.find_window("Ram Disk:Box");
    CHECK(r.workbench.leave_out(box, r.find_icon_in(box, "One")));
    CHECK(r.workbench.leave_out(w.ram, r.find_icon_in(w.ram, "Note")));

    // Renaming a file, or a drawer above one, renames its line and icon.
    CHECK(r.workbench.rename_icon(w.ram, r.find_icon_in(w.ram, "Note"), "Memo"));
    CHECK(r.workbench.rename_icon(w.ram, r.find_icon_in(w.ram, "Box"), "Case"));
    CHECK(ram_text("RAM:.backdrop") == ":Memo\n:Case/One\n");
    CHECK(r.find_icon_in(0, "Memo") != Workbench::kNoIcon);
    CHECK(r.find_icon_in(0, "One") != Workbench::kNoIcon);
    CHECK_EQ(r.find_icon_in(0, "Note"), Workbench::kNoIcon);

    // Deleting takes both away.
    CHECK(r.workbench.delete_icon(w.ram, r.find_icon_in(w.ram, "Case")));
    CHECK(r.workbench.delete_icon(w.ram, r.find_icon_in(w.ram, "Memo")));
    CHECK(!r.exists("RAM:.backdrop"));
    CHECK_EQ(r.find_icon_in(0, "Memo"), Workbench::kNoIcon);
    CHECK_EQ(r.find_icon_in(0, "One"), Workbench::kNoIcon);
}

void the_backdrop_file_brings_icons_back() {
    Rig r;
    u32 error = 0;
    r.make_dir("RAM:Box");
    put_ram_project("Box/One");
    put_ram_project("Note");
    const std::string list = ":Box/One\n:Gone\n:note\n";
    CHECK(g_dos.write_file("RAM:.backdrop", reinterpret_cast<const u8*>(list.data()),
                           static_cast<u32>(list.size()), error));
    r.workbench.open_desktop();
    CHECK(r.find_icon_in(0, "One") != Workbench::kNoIcon);
    CHECK(r.find_icon_in(0, "note") != Workbench::kNoIcon);
    CHECK_EQ(r.find_icon_in(0, "Gone"), Workbench::kNoIcon);
    // Put Away drops its line whatever its case; the others stay.
    click_icon(r, 0, r.find_icon_in(0, "note"));
    r.workbench.menu_pick(0, icons_pick(7));
    CHECK(ram_text("RAM:.backdrop") == ":Box/One\n:Gone\n");
}

// A Trashcan in RAM: holding a file and a drawer with a file.
void put_ram_trash(Rig& r) {
    put_ram_drawer(r, "Trashcan");
    u32 error = 0;
    const u32 object = g_icon.default_object(Workbench::kGarbage);
    CHECK(object != 0);
    CHECK(g_icon.save_object("RAM:Trashcan", object, error));
    g_icon.free_object(object);
}

void empty_trash_deletes_what_the_trashcan_holds() {
    Rig r;
    put_ram_project("Note");
    put_ram_project("Kept");
    put_ram_trash(r);
    const TwoWindows w = open_tools_and_ram(r);
    const u32 trash = r.find_icon_in(w.ram, "Trashcan");
    CHECK(!r.workbench.empty_trash(w.ram, r.find_icon_in(w.ram, "Note")));
    r.workbench.open_icon(w.ram, trash);
    const u32 inside = r.find_window("Ram Disk:Trashcan");
    CHECK(r.find_icon_in(inside, "One") != Workbench::kNoIcon);

    // A project dropped on the Trashcan moves into it.
    r.workbench.close_drawer(inside);
    const Workbench::IconEntry from = r.workbench.icon(r.find_icon_in(w.ram, "Note"));
    const Workbench::IconEntry can = r.workbench.icon(trash);
    r.drag(w.ram, from.left + 3, from.top + 3, r.screen_x(w.ram, can.left + 3),
           r.screen_y(w.ram, can.top + 3));
    CHECK(r.exists("RAM:Trashcan/Note"));
    CHECK(!r.exists("RAM:Note"));
    // The drop filled the drawer anew, with new icons.
    const u32 again = r.find_icon_in(w.ram, "Trashcan");

    // Icons > Empty Trash asks first.
    bool answer = false;
    r.workbench.set_answer(
        [](void* ctx, const char*, const char*, char*, u32) { return *static_cast<bool*>(ctx); },
        &answer);
    click_icon(r, w.ram, again);
    r.workbench.menu_pick(w.ram, icons_pick(10));
    CHECK(r.exists("RAM:Trashcan/Deep/Two"));
    answer = true;
    r.workbench.menu_pick(w.ram, icons_pick(10));
    CHECK(!r.exists("RAM:Trashcan/Deep"));
    CHECK(!r.exists("RAM:Trashcan/One"));
    CHECK(r.exists("RAM:Trashcan"));
    CHECK(r.exists("RAM:Trashcan.info"));
    CHECK(!r.exists("RAM:Trashcan/Note"));
    CHECK(r.exists("RAM:Kept"));
}

void last_message_keeps_the_error_shown_last() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    CHECK(std::string(r.workbench.last_message()).empty());
    CHECK(!r.workbench.delete_icon(w.tools, r.find_icon_in(w.tools, "Tool")));
    CHECK(std::string(r.workbench.last_message()) == "Tool: Disk is write-protected");
}

void quit_closes_the_desktop_and_it_can_open_again() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("Ram Disk"));
    bool answer = false;
    r.workbench.set_answer(
        [](void* ctx, const char*, const char*, char*, u32) { return *static_cast<bool*>(ctx); },
        &answer);
    // Workbench > Quit asks, and the task quits after it.
    r.workbench.menu_pick(0, 0 | (6 << 5) | (31 << 11));
    CHECK(!r.workbench.quit_pending());
    answer = true;
    r.workbench.menu_pick(0, 0 | (6 << 5) | (31 << 11));
    CHECK(r.workbench.quit_pending());
    CHECK(r.workbench.quit());
    CHECK(!r.workbench.desktop_open());
    CHECK(r.workbench.windows().empty());
    CHECK(r.workbench.icons().empty());
    CHECK(!r.workbench.quit());

    // Opened and quit again it keeps no memory.
    r.workbench.open_desktop();
    CHECK(r.workbench.has_window(Workbench::kRootWindow));
    CHECK(r.find_icon("Ram Disk") != Workbench::kNoIcon);
    CHECK(r.workbench.quit());
    const u32 before = r.free_bytes();
    r.workbench.open_desktop();
    CHECK(r.workbench.quit());
    CHECK_EQ(r.free_bytes(), before);
}

void quit_waits_for_the_programs_it_started() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 volume = r.find_window("HombreTest:");
    r.workbench.open_icon(volume, r.find_icon_in(volume, "Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    r.workbench.open_icon(tools, r.find_icon_in(tools, "Proj"));
    CHECK(!r.workbench.tools().empty());
    CHECK(!r.workbench.quit());
    CHECK(r.workbench.desktop_open());
    CHECK(logged("workbench: cannot quit, programs are using it"));
}

void quit_waits_for_registrations() {
    Rig r;
    r.workbench.open_desktop();
    const u32 item = r.workbench.add_app_menu_item(1, 0, 0, 0, 0);
    CHECK(item != 0);
    CHECK(!r.workbench.quit());
    CHECK(r.workbench.remove_app_menu_item(item));
    CHECK(r.workbench.quit());
}

void reset_wb_reopens_the_windows() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("Ram Disk"));
    const u32 first = r.workbench.window(0).window;
    CHECK(first != 0);
    // Tools > ResetWB.
    r.workbench.menu_pick(0, 3 | (0 << 5) | (31 << 11));
    CHECK(r.workbench.has_window(Workbench::kRootWindow));
    CHECK_EQ(r.workbench.windows().size(), 2u);
    CHECK(r.find_window("Ram Disk:") != Workbench::kNoWindow);
}

void window_update_rescans_the_drawer() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    put_ram_project("Late");
    CHECK_EQ(r.find_icon_in(w.ram, "Late"), Workbench::kNoIcon);

    // Window > Update.
    r.workbench.menu_pick(w.ram, 1 | (3 << 5) | (31 << 11));
    CHECK(r.find_icon_in(w.ram, "Late") != Workbench::kNoIcon);
}

void a_window_snapshot_is_used_when_the_window_opens_again() {
    Rig r;
    put_ram_project("Note");
    const TwoWindows w = open_tools_and_ram(r);
    const u32 window = r.workbench.window(w.ram).window;
    g_intuition.move_window_to(window, 100, 120);
    g_intuition.size_window_to(window, 250, 90);
    r.workbench.pump();

    // Window > Snapshot > Window.
    r.workbench.menu_pick(w.ram, 1 | (6 << 5) | (0 << 11));
    CHECK(r.exists("RAM:Disk.info"));
    const u32 object = g_icon.load_object("RAM:Disk");
    CHECK(object != 0);
    const u32 data = r.get32(object + do_DrawerData);
    CHECK(data != 0);
    CHECK_EQ(r.get16(data + nw_LeftEdge), 100u);
    CHECK_EQ(r.get16(data + nw_TopEdge), 120u);
    CHECK_EQ(r.get16(data + nw_Width), 250u);
    CHECK_EQ(r.get16(data + nw_Height), 90u);
    g_icon.free_object(object);
    // Window alone leaves the icons' places as they were.
    CHECK(r.saved_place("RAM:Note") == std::make_pair(kNoIconPosition, kNoIconPosition));

    r.workbench.close_drawer(w.ram);
    r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
    const u32 ram = r.find_window("Ram Disk:");
    CHECK(ram != Workbench::kNoWindow);
    const u32 reopened = r.workbench.window(ram).window;
    CHECK_EQ(r.get16(reopened + wd_LeftEdge), 100u);
    CHECK_EQ(r.get16(reopened + wd_TopEdge), 120u);
    CHECK_EQ(r.get16(reopened + wd_Width), 250u);
    CHECK_EQ(r.get16(reopened + wd_Height), 90u);

    // Window > Snapshot > All also keeps every icon's place.
    r.workbench.menu_pick(ram, 1 | (6 << 5) | (1 << 11));
    const Workbench::IconEntry note = r.workbench.icon(r.find_icon_in(ram, "Note"));
    CHECK(r.saved_place("RAM:Note") ==
          std::make_pair(static_cast<u32>(note.left), static_cast<u32>(note.top)));

    // The disk's drawer cannot keep one.
    CHECK(!r.workbench.snapshot_window(w.tools, false));
    CHECK(logged("workbench: cannot snapshot Tools: Disk is write-protected"));
    CHECK(!r.workbench.snapshot_window(0, false));
}
}

// WBPattern's prefs: a FORM PREF with a PRHD and one PTRN for the root
// window, a 16 by 16 pattern of one plane in vertical stripes.
std::vector<u8> pattern_prefs() {
    std::vector<u8> f;
    auto id = [&](const char* s) { f.insert(f.end(), s, s + 4); };
    auto u32v = [&](u32 v) {
        for (int i = 3; i >= 0; --i) {
            f.push_back(static_cast<u8>(v >> (i * 8)));
        }
    };
    auto u16v = [&](u32 v) {
        f.push_back(static_cast<u8>(v >> 8));
        f.push_back(static_cast<u8>(v));
    };
    id("FORM");
    u32v(4 + 8 + 6 + 8 + 24 + 32);
    id("PREF");
    id("PRHD");
    u32v(6);
    u16v(0);
    u32v(0);
    id("PTRN");
    u32v(24 + 32);
    for (int i = 0; i < 4; ++i) {
        u32v(0);
    }
    u16v(0);   // WBP_ROOT
    u16v(1);   // WBPF_PATTERN
    f.push_back(0);   // revision
    f.push_back(1);   // depth
    u16v(32);  // data length
    for (int row = 0; row < 16; ++row) {
        u16v(0xAAAA);
    }
    return f;
}

void the_backdrop_shows_the_wbpattern_pattern() {
    Rig r;
    const char dir[] = "ENV:Sys";
    for (u32 i = 0; i < sizeof dir; ++i) {
        r.put8(kData + i, static_cast<u8>(dir[i]));
    }
    r.m.cpu.d[1] = kData;
    CHECK(r.result(g_dos.dos_base(), dos_lvo::CreateDir) != 0);
    const std::vector<u8> prefs = pattern_prefs();
    u32 error = 0;
    CHECK(g_dos.write_file("ENV:Sys/WBPattern.prefs", prefs.data(),
                           static_cast<u32>(prefs.size()), error));
    r.workbench.open_desktop();
    CHECK(logged("workbench: backdrop patterns read"));
    const Workbench::DrawerWindow& root = r.workbench.window(0);
    // Away from the icons: pen 1 at even columns, pen 0 at odd ones.
    CHECK_EQ(r.window_pixel(root.window, root.inner_left + 20, root.inner_top + 300), 1u);
    CHECK_EQ(r.window_pixel(root.window, root.inner_left + 21, root.inner_top + 300), 0u);
    CHECK_EQ(r.window_pixel(root.window, root.inner_left + 36, root.inner_top + 301), 1u);

    // Selecting an icon and letting it go leaves the tiles as a full paint
    // laid them, under the icon and around it.
    const Workbench::IconEntry disk = r.workbench.icon(r.find_icon("HombreTest"));
    auto around = [&]() {
        std::vector<u32> pens;
        for (i32 y = disk.top - 8; y < disk.top + disk.height + 24; ++y) {
            for (i32 x = disk.left - 40; x < disk.left + disk.width + 8; ++x) {
                pens.push_back(r.window_pixel(root.window, root.inner_left + x,
                                              root.inner_top + y));
            }
        }
        return pens;
    };
    const std::vector<u32> before = around();
    r.click(0, disk.left + 3, disk.top + 3);
    CHECK(around() != before);
    r.click(0, 20, 300);
    CHECK(around() == before);
}

// An ILBM of one plane, 16 by 8, in vertical stripes: colour 1 (white)
// at even columns, colour 0 (black) at odd ones, packed with ByteRun1.
std::vector<u8> stripe_ilbm() {
    std::vector<u8> body;
    for (int row = 0; row < 8; ++row) {
        body.push_back(static_cast<u8>(257 - 2));  // two of
        body.push_back(0xAA);
    }
    std::vector<u8> f;
    auto id = [&](const char* s) { f.insert(f.end(), s, s + 4); };
    auto u32v = [&](u32 v) {
        for (int i = 3; i >= 0; --i) {
            f.push_back(static_cast<u8>(v >> (i * 8)));
        }
    };
    id("FORM");
    u32v(4 + 8 + 20 + 8 + 6 + 8 + static_cast<u32>(body.size()));
    id("ILBM");
    id("BMHD");
    u32v(20);
    for (u8 v : {0, 16, 0, 8, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 10, 11, 1, 64, 0, 200}) {
        f.push_back(v);
    }
    id("CMAP");
    u32v(6);
    for (u8 v : {0, 0, 0, 255, 255, 255}) {
        f.push_back(v);
    }
    id("BODY");
    u32v(static_cast<u32>(body.size()));
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

// WBPattern's prefs naming a picture for the root window.
std::vector<u8> picture_prefs(const std::string& name) {
    std::vector<u8> f;
    auto id = [&](const char* s) { f.insert(f.end(), s, s + 4); };
    auto u32v = [&](u32 v) {
        for (int i = 3; i >= 0; --i) {
            f.push_back(static_cast<u8>(v >> (i * 8)));
        }
    };
    const u32 data = 24 + static_cast<u32>(name.size()) + 1;
    id("FORM");
    u32v(4 + 8 + data + (data & 1));
    id("PREF");
    id("PTRN");
    u32v(data);
    for (int i = 0; i < 16; ++i) {
        f.push_back(0);
    }
    f.push_back(0);  // WBP_ROOT
    f.push_back(0);
    f.push_back(0);  // a picture, no WBPF_PATTERN
    f.push_back(0);
    f.push_back(0);  // revision
    f.push_back(0);  // depth
    f.push_back(0);
    f.push_back(static_cast<u8>(name.size() + 1));
    f.insert(f.end(), name.begin(), name.end());
    f.push_back(0);
    if (data & 1) {
        f.push_back(0);
    }
    return f;
}

void the_backdrop_shows_a_wbpattern_picture() {
    Rig r;
    const char dir[] = "ENV:Sys";
    for (u32 i = 0; i < sizeof dir; ++i) {
        r.put8(kData + i, static_cast<u8>(dir[i]));
    }
    r.m.cpu.d[1] = kData;
    CHECK(r.result(g_dos.dos_base(), dos_lvo::CreateDir) != 0);
    const std::vector<u8> picture = stripe_ilbm();
    const std::vector<u8> prefs = picture_prefs("RAM:Stripes.ilbm");
    u32 error = 0;
    CHECK(g_dos.write_file("RAM:Stripes.ilbm", picture.data(), static_cast<u32>(picture.size()),
                           error));
    CHECK(g_dos.write_file("ENV:Sys/WBPattern.prefs", prefs.data(),
                           static_cast<u32>(prefs.size()), error));
    r.workbench.open_desktop();
    CHECK(logged("workbench: backdrop picture RAM:Stripes.ilbm"));
    const Workbench::DrawerWindow& root = r.workbench.window(0);
    // White is pen 2 and black pen 1 of the screen, the picture tiled.
    CHECK_EQ(r.window_pixel(root.window, root.inner_left + 20, root.inner_top + 300), 2u);
    CHECK_EQ(r.window_pixel(root.window, root.inner_left + 21, root.inner_top + 300), 1u);
    CHECK_EQ(r.window_pixel(root.window, root.inner_left + 36, root.inner_top + 301), 2u);
}

void a_reset_closes_and_reopens_the_windows() {
    Rig r;
    r.workbench.open_desktop();
    CHECK(r.workbench.has_window(Workbench::kRootWindow));
    CHECK_EQ(r.get32(r.workbench.window(0).window + wd_Flags) & WFLG_WBENCHWINDOW,
             WFLG_WBENCHWINDOW);

    const u32 volume = r.find_icon("HombreTest");
    CHECK(volume != Workbench::kNoIcon);
    r.workbench.open_icon(0, volume);
    const u32 drawer = r.find_window("HombreTest:");
    CHECK(drawer != Workbench::kNoWindow);
    const u32 window = r.workbench.window(drawer).window;
    CHECK_EQ(r.get32(window + wd_Flags) & WFLG_WBENCHWINDOW, WFLG_WBENCHWINDOW);
    const u32 left = r.get16(window + wd_LeftEdge);
    const u32 top = r.get16(window + wd_TopEdge);
    const u32 width = r.get16(window + wd_Width);
    const u32 height = r.get16(window + wd_Height);

    CHECK(r.workbench.close_for_reset());
    CHECK(r.workbench.windows().empty());
    CHECK_EQ(r.find_window("HombreTest:"), Workbench::kNoWindow);

    r.workbench.open_after_reset();
    CHECK(r.workbench.has_window(Workbench::kRootWindow));
    CHECK_EQ(r.get32(r.workbench.window(0).window + wd_Flags) & WFLG_WBENCHWINDOW,
             WFLG_WBENCHWINDOW);
    const u32 reopened = r.find_window("HombreTest:");
    CHECK(reopened != Workbench::kNoWindow);
    const u32 again = r.workbench.window(reopened).window;
    CHECK_EQ(r.get16(again + wd_LeftEdge), left);
    CHECK_EQ(r.get16(again + wd_TopEdge), top);
    CHECK_EQ(r.get16(again + wd_Width), width);
    CHECK_EQ(r.get16(again + wd_Height), height);
    CHECK_EQ(r.get32(again + wd_Flags) & WFLG_WBENCHWINDOW, WFLG_WBENCHWINDOW);
}

// Resets with a drawer open leave the memory as it was.
void resets_give_back_their_memory() {
    Rig r;
    r.workbench.open_desktop();
    const u32 volume = r.find_icon("HombreTest");
    r.workbench.open_icon(0, volume);
    CHECK(r.workbench.close_for_reset());
    r.workbench.open_after_reset();
    const u32 before = r.free_bytes();
    for (int i = 0; i < 5; ++i) {
        CHECK(r.workbench.close_for_reset());
        r.workbench.open_after_reset();
    }
    CHECK_EQ(r.free_bytes(), before);
}

// A disk whose root holds count drawers, Many0 and on, each with one icon.
std::vector<AdfFile> drawer_files(u32 count) {
    const i32 nowhere = static_cast<i32>(kNoIconPosition);
    std::vector<AdfFile> files = test_files();
    for (u32 i = 0; i < count; ++i) {
        const std::string name = "Many" + std::to_string(i);
        files.push_back({name + ".info", drawer_icon_info(nowhere, nowhere, 0, 0, 0, 0)});
        files.push_back({name + "/Item.info", icon_info(kWBTool, nowhere, nowhere)});
    }
    return files;
}

// The window of the drawer opened from the window of the disk, or kNoWindow.
u32 open_drawer_named(Rig& r, u32 disk_window, const std::string& name) {
    r.workbench.open_icon(disk_window, r.find_icon_in(disk_window, name));
    return r.find_window("HombreTest:" + name);
}

// A task for the rig to run the desktop in, as the Workbench task does, so
// the signal bits its ports take can be read from the task.
u32 enter_task(Rig& r) {
    const u32 task = g_exec.alloc(TC_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(task != 0);
    r.put8(task + LN_TYPE, NT_TASK);
    r.put32(task + TC_SIGALLOC, 0x0000FFFF);
    g_exec.set_this_task(task);
    return task;
}

void more_drawers_open_than_the_old_table_held() {
    constexpr u32 kDrawers = 20;
    Rig r(drawer_files(kDrawers));
    const u32 task = enter_task(r);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 disk = r.find_window("HombreTest:");
    CHECK(disk != Workbench::kNoWindow);
    // The windows share one port: more of them take no more signal bits.
    const u32 signals = r.get32(task + TC_SIGALLOC);

    std::vector<u32> drawers;
    std::vector<u32> windows;
    for (u32 i = 0; i < kDrawers; ++i) {
        drawers.push_back(open_drawer_named(r, disk, "Many" + std::to_string(i)));
        CHECK(drawers.back() != Workbench::kNoWindow);
        CHECK(r.find_icon_in(drawers.back(), "Item") != Workbench::kNoIcon);
        windows.push_back(r.workbench.window(drawers.back()).window);
    }
    // The root window, the disk's drawer and the others.
    CHECK_EQ(r.workbench.windows().size(), 2 + kDrawers);
    CHECK_EQ(r.get32(task + TC_SIGALLOC), signals);
    for (const u32 window : windows) {
        CHECK_EQ(r.get32(window + wd_UserPort), r.workbench.window_port());
    }

    // Each drawer opens a step below and right of the one before, and the
    // steps start over after seven.
    for (u32 i = 0; i < kDrawers; ++i) {
        CHECK_EQ(r.get16(windows[i] + wd_LeftEdge), 40u + 20u * ((i + 1) % 7));
    }

    // Closing some leaves the others as they were: an id names its window,
    // and the record it holds stays where it is.
    const Workbench::DrawerWindow& kept = r.workbench.window(drawers[1]);
    const Workbench::IconEntry& kept_icon = r.workbench.icon(r.find_icon_in(drawers[1], "Item"));
    u32 closed_count = 0;
    for (u32 i = 0; i < kDrawers; i += 3) {
        r.workbench.close_drawer(drawers[i]);
        ++closed_count;
    }
    for (u32 i = 0; i < kDrawers; ++i) {
        const bool closed = i % 3 == 0;
        CHECK_EQ(r.workbench.has_window(drawers[i]), !closed);
        CHECK_EQ(r.find_icon_in(drawers[i], "Item") == Workbench::kNoIcon, closed);
        if (!closed) {
            CHECK_EQ(r.workbench.window(drawers[i]).window, windows[i]);
        }
    }
    CHECK(&r.workbench.window(drawers[1]) == &kept);
    CHECK(&r.workbench.icon(kept_icon.id) == &kept_icon);

    // A drawer opened again is a new window, and a reset reopens the drawers
    // in the order they were opened.
    const u32 again = open_drawer_named(r, disk, "Many0");
    CHECK(again != Workbench::kNoWindow);
    CHECK(again > drawers.back());
    auto open_paths = [&]() {
        std::vector<std::string> paths;
        for (const Workbench::DrawerWindow& w : r.workbench.windows()) {
            if (w.id != Workbench::kRootWindow) {
                paths.push_back(w.path);
            }
        }
        return paths;
    };
    const std::vector<std::string> before = open_paths();
    // The disk's drawer, the drawers still open and the one opened again.
    CHECK_EQ(before.size(), 1 + (kDrawers - closed_count) + 1);
    CHECK(r.workbench.close_for_reset());
    CHECK(r.workbench.windows().empty());
    r.workbench.open_after_reset();
    CHECK(open_paths() == before);
    CHECK_EQ(r.get32(task + TC_SIGALLOC), signals);
}

void no_window_is_left_once_the_table_is_full() {
    Rig r(drawer_files(Workbench::kMaxWindows));
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 disk = r.find_window("HombreTest:");
    // The root window and the disk's drawer take two of the places.
    const u32 room = Workbench::kMaxWindows - 2;
    std::vector<u32> drawers;
    for (u32 i = 0; i < Workbench::kMaxWindows; ++i) {
        drawers.push_back(open_drawer_named(r, disk, "Many" + std::to_string(i)));
    }
    CHECK_EQ(r.workbench.windows().size(), Workbench::kMaxWindows);
    for (u32 i = 0; i < Workbench::kMaxWindows; ++i) {
        CHECK_EQ(drawers[i] != Workbench::kNoWindow, i < room);
    }
    CHECK_EQ(logged_count("workbench: no window left for Many" + std::to_string(room)), 1u);

    // A closed drawer makes room for the next.
    r.workbench.close_drawer(drawers[0]);
    CHECK(open_drawer_named(r, disk, "Many" + std::to_string(room)) != Workbench::kNoWindow);
    CHECK_EQ(r.workbench.windows().size(), Workbench::kMaxWindows);
}

void a_drawer_shows_more_icons_than_the_old_table_held() {
    constexpr u32 kEntries = 150;
    constexpr usize kOldTable = 96;
    const i32 nowhere = static_cast<i32>(kNoIconPosition);
    std::vector<AdfFile> files = test_files();
    files.push_back({"Crowd.info", drawer_icon_info(nowhere, nowhere, 30, 50, 300, 120)});
    for (u32 i = 0; i < kEntries; ++i) {
        files.push_back({"Crowd/E" + std::to_string(i) + ".info",
                         icon_info(kWBTool, nowhere, nowhere)});
    }
    Rig r(files);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 disk = r.find_window("HombreTest:");
    r.workbench.open_icon(disk, r.find_icon_in(disk, "Crowd"));
    const u32 crowd = r.find_window("HombreTest:Crowd");
    CHECK(crowd != Workbench::kNoWindow);

    u32 shown = 0;
    u32 newest = 0;
    for (const Workbench::IconEntry& icon : r.workbench.icons()) {
        shown += icon.owner == crowd ? 1 : 0;
        newest = icon.id > newest ? icon.id : newest;
        CHECK(&r.workbench.icon(icon.id) == &icon);
    }
    CHECK_EQ(shown, kEntries);
    CHECK(r.workbench.icons().size() > kOldTable);

    // A rescan gives the icons back and makes them again under new ids.
    r.rescan("HombreTest:Crowd");
    shown = 0;
    for (const Workbench::IconEntry& icon : r.workbench.icons()) {
        if (icon.owner == crowd) {
            ++shown;
            CHECK(icon.id > newest);
        }
    }
    CHECK_EQ(shown, kEntries);

    // Icons of another window coming and going leave an icon where it is.
    const Workbench::IconEntry& kept = r.workbench.icon(r.find_icon_in(crowd, "E7"));
    r.workbench.open_icon(disk, r.find_icon_in(disk, "Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kNoWindow);
    r.workbench.close_drawer(tools);
    CHECK_EQ(r.find_icon_in(tools, "Tool"), Workbench::kNoIcon);
    CHECK(&r.workbench.icon(kept.id) == &kept);
    CHECK(std::string(kept.name) == "E7");
}

void more_applications_register_than_the_old_table_held() {
    Rig r;
    r.workbench.open_desktop();
    std::vector<u32> handles;
    for (u32 i = 0; i < Workbench::kMaxApps; ++i) {
        handles.push_back(r.workbench.add_app_window(i, 0, 0, 0, 0));
        CHECK(handles.back() != 0);
    }
    CHECK_EQ(r.workbench.apps().size(), Workbench::kMaxApps);
    CHECK_EQ(logged_count("workbench: application table full"), 0u);

    // A full table refuses another registration, until one is removed.
    CHECK_EQ(r.workbench.add_app_window(1, 0, 0, 0, 0), 0u);
    CHECK_EQ(logged_count("workbench: application table full"), 1u);
    CHECK(r.workbench.remove_app_window(handles[500]));
    CHECK_EQ(r.workbench.apps().size(), Workbench::kMaxApps - 1);
    const u32 again = r.workbench.add_app_window(2, 0, 0, 0, 0);
    CHECK(again != 0);
    CHECK_EQ(r.workbench.apps().size(), Workbench::kMaxApps);

    // Every other handle still names its own registration.
    for (u32 i = 0; i < Workbench::kMaxApps; ++i) {
        if (i != 500) {
            CHECK(r.workbench.remove_app_window(handles[i]));
        }
    }
    CHECK(r.workbench.remove_app_window(again));
    CHECK(r.workbench.apps().empty());
}

void several_programs_run_at_once() {
    Rig r;
    r.workbench.open_desktop();
    Workbench::IconEntry icon;
    std::strcpy(icon.file, "Tiny");
    std::strcpy(icon.name, "Tiny");
    icon.stack_size = 4096;
    constexpr u32 kPrograms = 4;
    for (u32 i = 0; i < kPrograms; ++i) {
        CHECK(r.workbench.start_tool(icon, "HombreTest:Tools"));
    }
    CHECK_EQ(r.workbench.tools().size(), kPrograms);
    std::vector<Workbench::RunningTool> started;
    for (const Workbench::RunningTool& tool : r.workbench.tools()) {
        started.push_back(tool);
    }

    // The reply to one message ends that program and no other.
    CHECK(r.workbench.tool_reply(started[1].message));
    CHECK(!g_dos.unlock_path(started[1].locks[0]));
    CHECK_EQ(r.workbench.tools().size(), kPrograms - 1);

    // So does a program that ends without a reply.
    r.m.cpu.a[1] = started[0].process;
    CHECK_EQ(r.call(g_exec.exec_base(), exec_lvo::RemTask), ExitReason::Halt);
    r.workbench.reap_tools();
    CHECK(!g_dos.unlock_path(started[0].locks[0]));
    CHECK_EQ(r.workbench.tools().size(), kPrograms - 2);
    CHECK_EQ(r.workbench.tools()[0].message, started[2].message);
    CHECK_EQ(r.workbench.tools()[1].message, started[3].message);

    CHECK(r.workbench.tool_reply(started[3].message));
    CHECK(r.workbench.tool_reply(started[2].message));
    CHECK(r.workbench.tools().empty());
}

// The pens of the window's inner area, row by row.
std::vector<u32> inner_pens(Rig& r, u32 window_index) {
    const Workbench::DrawerWindow& w = r.workbench.window(window_index);
    std::vector<u32> pens;
    for (i32 y = w.inner_top; y < w.inner_top + w.inner_height; ++y) {
        for (i32 x = w.inner_left; x < w.inner_left + w.inner_width; ++x) {
            pens.push_back(r.window_pixel(w.window, x, y));
        }
    }
    return pens;
}

void a_drawer_zooms_to_a_small_box_and_back_with_its_icons_in_place() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon_in(root, "Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kNoWindow);
    const u32 window = r.workbench.window(tools).window;
    const u32 flags = r.get32(window + wd_Flags);
    CHECK_EQ(flags & WFLG_HASZOOM, WFLG_HASZOOM);
    const u32 tool = r.find_icon("Tool");
    const u32 put = r.find_icon("Put");
    const Workbench::DrawerWindow before = r.workbench.window(tools);
    const std::vector<u32> shown = inner_pens(r, tools);

    // The small box is at the drawer's place, wide enough for its title and
    // the bar buttons and no higher than the drawer's smallest size and the
    // title bar and bottom border need.
    click_zoom(r, window);
    const i32 small_width = static_cast<i32>(r.get16(window + wd_Width));
    const i32 small_height = static_cast<i32>(r.get16(window + wd_Height));
    const u8 title[] = {'T', 'o', 'o', 'l', 's'};
    const i32 fit = chrome::title_bar_width(
        static_cast<i32>(g_graphics.text_width(g_intuition.font_of_screen(r.screen), title, 5)),
        flags);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 30u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 50u);
    CHECK(small_width >= fit && small_width < 300);
    CHECK(small_height >= chrome::Metrics{}.border_top() +
                              static_cast<i32>(chrome::kSizeBorderBottom) &&
          small_height < 120);
    // The desktop measured it again and kept every icon where it was.
    const Workbench::DrawerWindow& zoomed = r.workbench.window(tools);
    CHECK_EQ(zoomed.inner_width, small_width - zoomed.inner_left -
                                     static_cast<i32>(r.get8(window + wd_BorderRight)));
    CHECK(zoomed.inner_width < before.inner_width);
    CHECK(zoomed.inner_height < before.inner_height);
    CHECK_EQ(r.workbench.icon(tool).left, 100);
    CHECK_EQ(r.workbench.icon(tool).top, 40);
    CHECK_EQ(r.workbench.icon(put).left, 20);
    CHECK_EQ(r.workbench.icon(put).top, 100);

    // Back in the box it had, the window shows what it showed.
    click_zoom(r, window);
    CHECK_EQ(r.get16(window + wd_LeftEdge), 30u);
    CHECK_EQ(r.get16(window + wd_TopEdge), 50u);
    CHECK_EQ(r.get16(window + wd_Width), 300u);
    CHECK_EQ(r.get16(window + wd_Height), 120u);
    CHECK_EQ(r.workbench.window(tools).inner_width, before.inner_width);
    CHECK_EQ(r.workbench.window(tools).inner_height, before.inner_height);
    CHECK_EQ(r.workbench.icon(tool).left, 100);
    CHECK_EQ(r.workbench.icon(tool).top, 40);
    CHECK_EQ(r.workbench.icon(put).left, 20);
    CHECK_EQ(r.workbench.icon(put).top, 100);
    CHECK(inner_pens(r, tools) == shown);
}

void the_root_window_has_a_zoom_gadget_only_when_it_is_not_a_backdrop() {
    Rig r;
    r.workbench.open_desktop();
    CHECK(r.workbench.backdrop());
    CHECK_EQ(r.get32(r.workbench.window(Workbench::kRootWindow).window + wd_Flags) & WFLG_HASZOOM,
             0u);
    r.workbench.set_backdrop(false);
    CHECK(!r.workbench.backdrop());
    CHECK_EQ(r.get32(r.workbench.window(Workbench::kRootWindow).window + wd_Flags) & WFLG_HASZOOM,
             WFLG_HASZOOM);
}

// --- Window > Show -------------------------------------------------------

u32 show_parent(const Rig& r) {
    const u32 window_menu = r.get32(r.workbench.menu() + mu_NextMenu);
    u32 item = r.get32(window_menu + mu_FirstItem);
    for (u32 i = 0; i < 7; ++i) {
        item = r.get32(item + mi_NextItem);
    }
    return item;
}

u32 show_entry(const Rig& r, u32 index) {
    u32 item = r.get32(show_parent(r) + mi_SubItem);
    for (u32 i = 0; i < index; ++i) {
        item = r.get32(item + mi_NextItem);
    }
    return item;
}

constexpr u32 show_pick(u32 sub) { return 1 | (7 << 5) | (sub << 11); }

void the_show_menu_offers_only_icons_and_all_files() {
    Rig r;
    r.workbench.open_desktop();
    const u32 show = show_parent(r);
    CHECK(r.str(r.get32(r.get32(show + mi_ItemFill) + it_IText)) == "Show");
    CHECK_EQ(r.get16(show + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 only = r.get32(show + mi_SubItem);
    const u32 all = r.get32(only + mi_NextItem);
    CHECK(only != 0 && all != 0);
    CHECK(r.str(r.get32(r.get32(only + mi_ItemFill) + it_IText)) == "Only Icons");
    CHECK(r.str(r.get32(r.get32(all + mi_ItemFill) + it_IText)) == "All Files");
    CHECK_EQ(r.get32(all + mi_NextItem), 0u);
    for (u32 item : {only, all}) {
        const u16 flags = r.get16(item + mi_Flags);
        CHECK_EQ(flags & ITEMENABLED, ITEMENABLED);
        CHECK_EQ(flags & CHECKIT, CHECKIT);
        CHECK_EQ(flags & MENUTOGGLE, 0);
    }
    CHECK_EQ(r.get16(only + mi_Flags) & CHECKED, CHECKED);
    CHECK_EQ(r.get16(all + mi_Flags) & CHECKED, 0u);
    CHECK_EQ(r.get32(only + mi_MutualExclude), 2u);
    CHECK_EQ(r.get32(all + mi_MutualExclude), 1u);
}

void the_show_items_toggle_a_drawer_and_follow_the_active_window() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 volume = r.find_window("HombreTest:");
    CHECK(volume != Workbench::kNoWindow);
    CHECK(!r.workbench.window(volume).show_all);
    CHECK_EQ(r.get16(show_entry(r, 0) + mi_Flags) & CHECKED, CHECKED);
    CHECK_EQ(r.get16(show_entry(r, 1) + mi_Flags) & CHECKED, 0u);
    CHECK_EQ(r.find_icon_in(volume, "S"), Workbench::kNoIcon);

    // All Files adds an icon for what has no .info, after the others and
    // in name order.
    r.workbench.menu_pick(volume, show_pick(1));
    CHECK(r.workbench.window(volume).show_all);
    CHECK_EQ(r.get16(show_entry(r, 0) + mi_Flags) & CHECKED, 0u);
    CHECK_EQ(r.get16(show_entry(r, 1) + mi_Flags) & CHECKED, CHECKED);
    const u32 s = r.find_icon_in(volume, "S");
    const u32 tiny2 = r.find_icon_in(volume, "Tiny2");
    const u32 utilities = r.find_icon_in(volume, "Utilities");
    CHECK(s != Workbench::kNoIcon && tiny2 != Workbench::kNoIcon &&
          utilities != Workbench::kNoIcon);
    CHECK(r.workbench.icon(s).plain);
    CHECK_EQ(r.workbench.icon(s).type, kWBDrawer);
    CHECK_EQ(r.workbench.icon(tiny2).type, kWBTool);
    CHECK_EQ(r.workbench.icon(utilities).type, kWBDrawer);
    // .info files are no files of their own.
    CHECK_EQ(r.find_icon_in(volume, "Disk"), Workbench::kNoIcon);
    CHECK_EQ(r.find_icon_in(volume, "Tools.info"), Workbench::kNoIcon);
    std::vector<std::string> plain_names;
    for (const Workbench::IconEntry& icon : r.workbench.icons()) {
        if (icon.owner != volume) {
            continue;
        }
        if (icon.plain) {
            plain_names.push_back(icon.name);
        } else {
            CHECK(plain_names.empty());
        }
    }
    CHECK(plain_names == std::vector<std::string>({"S", "Tiny2", "Utilities"}));
    // All Files fills rows across the window: the icons without a place
    // stay inside its width.
    for (const Workbench::IconEntry& icon : r.workbench.icons()) {
        if (icon.owner == volume && !icon.placed) {
            CHECK(icon.left + icon.width <= r.workbench.window(volume).inner_width);
        }
    }
    // The refilled window drew the new icon.
    const Workbench::DrawerWindow& w = r.workbench.window(volume);
    const Workbench::IconEntry& s_icon = r.workbench.icon(s);
    CHECK_EQ(r.window_pixel(w.window, w.inner_left + s_icon.left, w.inner_top + s_icon.top), 1u);

    // Picking the checked item changes nothing, the icons included.
    const usize before = r.workbench.icons().size();
    r.workbench.menu_pick(volume, show_pick(1));
    CHECK_EQ(r.workbench.icons().size(), before);
    CHECK_EQ(r.find_icon_in(volume, "S"), s);
    CHECK(r.workbench.window(volume).show_all);

    // Only Icons takes them away again.
    r.workbench.menu_pick(volume, show_pick(0));
    CHECK(!r.workbench.window(volume).show_all);
    CHECK_EQ(r.find_icon_in(volume, "S"), Workbench::kNoIcon);
    CHECK_EQ(r.get16(show_entry(r, 0) + mi_Flags) & CHECKED, CHECKED);
    r.workbench.menu_pick(volume, show_pick(1));

    // The root window always shows only icons, even when it is active and
    // All Files is picked.
    r.click(Workbench::kRootWindow, 2, 2);
    CHECK_EQ(r.get16(show_entry(r, 0) + mi_Flags) & CHECKED, CHECKED);
    // Intuition checks the picked item before the desktop sees the pick.
    const u32 root_only = show_entry(r, 0);
    const u32 root_all = show_entry(r, 1);
    r.put16(root_only + mi_Flags, r.get16(root_only + mi_Flags) & ~CHECKED);
    r.put16(root_all + mi_Flags, r.get16(root_all + mi_Flags) | CHECKED);
    r.workbench.menu_pick(Workbench::kRootWindow, show_pick(1));
    CHECK(!r.workbench.window(Workbench::kRootWindow).show_all);
    CHECK_EQ(r.get16(root_only + mi_Flags) & CHECKED, CHECKED);
    CHECK_EQ(r.get16(root_all + mi_Flags) & CHECKED, 0u);

    // Active again, the drawer's All Files shows.
    r.click(volume, 2, 2);
    CHECK_EQ(r.get16(show_entry(r, 1) + mi_Flags) & CHECKED, CHECKED);

    // A plain drawer opens, with the setting of the window it came from.
    const u32 s_again_id = r.find_icon_in(volume, "S");
    CHECK(s_again_id != Workbench::kNoIcon);
    const Workbench::IconEntry& s_again = r.workbench.icon(s_again_id);
    r.click(volume, s_again.left + 1, s_again.top + 1);
    r.click(volume, s_again.left + 1, s_again.top + 1);
    const u32 s_window = r.find_window("HombreTest:S");
    CHECK(s_window != Workbench::kNoWindow);
    CHECK(r.workbench.window(s_window).show_all);
    const u32 startup = r.find_icon_in(s_window, "Startup-Sequence");
    CHECK(startup != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(startup).type, kWBProject);
}

void a_drawer_opens_with_the_show_setting_in_its_icon() {
    const i32 nowhere = static_cast<i32>(kNoIconPosition);
    std::vector<AdfFile> files = test_files();
    files.push_back({"Wide.info", drawer_icon_info(nowhere, nowhere, 30, 50, 300, 120, 0, 0, 2)});
    files.push_back(
        {"Wide/Inner.info", drawer_icon_info(nowhere, nowhere, 30, 50, 300, 120, 0, 0, 0)});
    files.push_back({"Wide/Inner/Loose", text("plain")});
    files.push_back(
        {"Wide/Strict.info", drawer_icon_info(nowhere, nowhere, 30, 50, 300, 120, 0, 0, 1)});
    files.push_back({"Wide/Strict/Hidden", text("plain")});
    files.push_back({"Wide/Bare", text("plain")});
    Rig r(files);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 volume = r.find_window("HombreTest:");
    r.workbench.open_icon(volume, r.find_icon_in(volume, "Wide"));
    const u32 wide = r.find_window("HombreTest:Wide");
    CHECK(wide != Workbench::kNoWindow);
    CHECK(r.workbench.window(wide).show_all);
    CHECK_EQ(r.get16(show_entry(r, 1) + mi_Flags) & CHECKED, CHECKED);
    CHECK(r.find_icon_in(wide, "Bare") != Workbench::kNoIcon);

    // 0 is the setting of the window it was opened from.
    r.workbench.open_icon(wide, r.find_icon_in(wide, "Inner"));
    const u32 inner = r.find_window("HombreTest:Wide/Inner");
    CHECK(inner != Workbench::kNoWindow);
    CHECK(r.workbench.window(inner).show_all);
    CHECK(r.find_icon_in(inner, "Loose") != Workbench::kNoIcon);

    // 1 is only icons, whatever the window it was opened from.
    r.workbench.open_icon(wide, r.find_icon_in(wide, "Strict"));
    const u32 strict = r.find_window("HombreTest:Wide/Strict");
    CHECK(strict != Workbench::kNoWindow);
    CHECK(!r.workbench.window(strict).show_all);
    CHECK(r.find_icon_in(strict, "Hidden") == Workbench::kNoIcon);
    CHECK_EQ(r.get16(show_entry(r, 1) + mi_Flags) & CHECKED, 0u);

    // Opened from the root, a drawer with 0 shows only icons.
    r.workbench.close_drawer(volume);
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 again = r.find_window("HombreTest:");
    r.workbench.open_icon(again, r.find_icon_in(again, "Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kNoWindow);
    CHECK(!r.workbench.window(tools).show_all);
}

void a_window_snapshot_stores_the_show_setting() {
    Rig r;
    r.workbench.open_desktop();
    put_ram_project("Note");
    u32 error = 0;
    const u8 byte = 'x';
    CHECK(g_dos.write_file("RAM:Bare", &byte, 1, error));
    r.workbench.open_icon(0, r.find_icon("Ram Disk"));
    const u32 ram = r.find_window("Ram Disk:");
    CHECK(ram != Workbench::kNoWindow);
    r.workbench.menu_pick(ram, show_pick(1));
    CHECK(r.workbench.window(ram).show_all);
    CHECK(r.find_icon_in(ram, "Bare") != Workbench::kNoIcon);

    // Window > Snapshot > Window.
    r.workbench.menu_pick(ram, 1 | (6 << 5) | (0 << 11));
    u32 object = g_icon.load_object("RAM:Disk");
    CHECK(object != 0);
    u32 data = r.get32(object + do_DrawerData);
    CHECK(data != 0);
    CHECK_EQ(r.get32(data + dd_Flags), 2u);
    CHECK_EQ(r.get32(object + do_Gadget + gg_UserData) & 0xFF, 1u);
    g_icon.free_object(object);

    // The drawer opens again with the setting.
    r.workbench.close_drawer(ram);
    r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
    const u32 again = r.find_window("Ram Disk:");
    CHECK(again != Workbench::kNoWindow);
    CHECK(r.workbench.window(again).show_all);
    CHECK(r.find_icon_in(again, "Bare") != Workbench::kNoIcon);

    // Only Icons snapshots as 1.
    r.workbench.menu_pick(again, show_pick(0));
    r.workbench.menu_pick(again, 1 | (6 << 5) | (0 << 11));
    object = g_icon.load_object("RAM:Disk");
    CHECK(object != 0);
    data = r.get32(object + do_DrawerData);
    CHECK(data != 0);
    CHECK_EQ(r.get32(data + dd_Flags), 1u);
    g_icon.free_object(object);
}

void an_old_style_drawer_icon_keeps_the_show_setting() {
    const i32 nowhere = static_cast<i32>(kNoIconPosition);
    Rig r;
    r.workbench.open_desktop();
    u32 error = 0;
    r.make_dir("RAM:Old");
    const std::vector<u8> old_info =
        drawer_icon_info(nowhere, nowhere, 30, 50, 300, 120, 0, 0, 0, 0);
    CHECK(g_dos.write_file("RAM:Old.info", old_info.data(), static_cast<u32>(old_info.size()),
                           error));
    const std::vector<u8> one_info = icon_info(kWBTool, 10, 10);
    CHECK(g_dos.write_file("RAM:Old/One.info", one_info.data(),
                           static_cast<u32>(one_info.size()), error));
    const std::vector<u8> bare = text("plain");
    CHECK(g_dos.write_file("RAM:Old/Bare", bare.data(), static_cast<u32>(bare.size()), error));
    r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
    const u32 volume = r.find_window("Ram Disk:");
    r.workbench.open_icon(volume, r.find_icon_in(volume, "Old"));
    const u32 old = r.find_window("Ram Disk:Old");
    CHECK(old != Workbench::kNoWindow);
    CHECK(!r.workbench.window(old).show_all);
    CHECK(r.find_icon_in(old, "Bare") == Workbench::kNoIcon);
    r.workbench.menu_pick(old, show_pick(1));
    CHECK(r.workbench.window(old).show_all);
    CHECK(r.workbench.snapshot_window(old, false));

    // The revision byte makes the flags part of the saved icon.
    const u32 object = g_icon.load_object("RAM:Old");
    CHECK(object != 0);
    CHECK_EQ(r.get32(object + do_Gadget + gg_UserData) & 0xFF, 1u);
    const u32 data = r.get32(object + do_DrawerData);
    CHECK(data != 0);
    CHECK_EQ(r.get32(data + dd_Flags), 2u);
    CHECK_EQ(r.get16(data + nw_LeftEdge), 30u);
    CHECK_EQ(r.get16(data + nw_Width), 300u);
    g_icon.free_object(object);

    r.workbench.close_drawer(old);
    r.workbench.open_icon(volume, r.find_icon_in(volume, "Old"));
    const u32 again = r.find_window("Ram Disk:Old");
    CHECK(again != Workbench::kNoWindow);
    CHECK(r.workbench.window(again).show_all);
    CHECK(r.find_icon_in(again, "Bare") != Workbench::kNoIcon);
}

void plain_icons_come_after_the_real_ones_in_name_order() {
    Rig r;
    r.workbench.open_desktop();
    u32 error = 0;
    r.make_dir("RAM:Names");
    const u32 drawer = g_icon.default_object(kWBDrawer);
    CHECK(g_icon.save_object("RAM:Names", drawer, error));
    g_icon.free_object(drawer);
    r.make_dir("RAM:Names/dir");
    const std::vector<u8> a_info = icon_info(kWBTool, 10, 10);
    const std::vector<u8> d_info = icon_info(kWBTool, 40, 10);
    const std::vector<u8> mixed_info = icon_info(kWBTool, 70, 10);
    CHECK(g_dos.write_file("RAM:Names/A.info", a_info.data(),
                           static_cast<u32>(a_info.size()), error));
    CHECK(g_dos.write_file("RAM:Names/D.info", d_info.data(),
                           static_cast<u32>(d_info.size()), error));
    CHECK(g_dos.write_file("RAM:Names/MIXED.INFO", mixed_info.data(),
                           static_cast<u32>(mixed_info.size()), error));
    const std::vector<u8> b = text("b");
    const std::vector<u8> e = text("e");
    const std::vector<u8> c = hunk_rts();
    CHECK(g_dos.write_file("RAM:Names/b", b.data(), static_cast<u32>(b.size()), error));
    CHECK(g_dos.write_file("RAM:Names/c", c.data(), static_cast<u32>(c.size()), error));
    CHECK(g_dos.write_file("RAM:Names/E", e.data(), static_cast<u32>(e.size()), error));
    CHECK(g_dos.write_file("RAM:Names/mixed", b.data(), static_cast<u32>(b.size()), error));

    r.workbench.open_icon(0, r.find_icon("Ram Disk"));
    const u32 ram = r.find_window("Ram Disk:");
    r.workbench.open_icon(ram, r.find_icon_in(ram, "Names"));
    const u32 names = r.find_window("Ram Disk:Names");
    CHECK(names != Workbench::kNoWindow);
    r.workbench.menu_pick(names, show_pick(1));

    std::vector<std::string> plain_names;
    for (const Workbench::IconEntry& icon : r.workbench.icons()) {
        if (icon.owner != names) {
            continue;
        }
        if (icon.plain) {
            plain_names.push_back(icon.name);
        } else {
            CHECK(plain_names.empty());
        }
    }
    CHECK(plain_names == std::vector<std::string>({"b", "c", "dir", "E"}));
    const u32 b_icon = r.find_icon_in(names, "b");
    const u32 c_icon = r.find_icon_in(names, "c");
    const u32 dir_icon = r.find_icon_in(names, "dir");
    const u32 e_icon = r.find_icon_in(names, "E");
    CHECK(b_icon != Workbench::kNoIcon && c_icon != Workbench::kNoIcon &&
          dir_icon != Workbench::kNoIcon && e_icon != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(b_icon).type, kWBProject);
    CHECK_EQ(r.workbench.icon(c_icon).type, kWBTool);
    CHECK_EQ(r.workbench.icon(dir_icon).type, kWBDrawer);
    CHECK_EQ(r.workbench.icon(e_icon).type, kWBProject);
    // The .info matches its file without regard to case.
    CHECK_EQ(r.find_icon_in(names, "mixed"), Workbench::kNoIcon);
    CHECK(r.find_icon_in(names, "MIXED") != Workbench::kNoIcon);
}

void a_plain_icon_is_copied_renamed_and_deleted() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    u32 error = 0;
    const u8 byte = 'x';
    CHECK(g_dos.write_file("RAM:Plain", &byte, 1, error));
    r.rescan("Ram Disk:");
    CHECK_EQ(r.find_icon_in(w.ram, "Plain"), Workbench::kNoIcon);
    r.workbench.menu_pick(w.ram, show_pick(1));
    const u32 plain = r.find_icon_in(w.ram, "Plain");
    CHECK(plain != Workbench::kNoIcon);
    CHECK(r.workbench.icon(plain).plain);
    CHECK_EQ(r.workbench.icon(plain).type, kWBProject);
    CHECK(!r.exists("RAM:Plain.info"));

    // It drags within its window like any other icon.
    const Workbench::IconEntry start = r.workbench.icon(plain);
    r.drag(w.ram, start.left + 3, start.top + 3, r.screen_x(w.ram, start.left + 7),
           r.screen_y(w.ram, start.top + 7));
    CHECK_EQ(r.workbench.icon(plain).left, start.left + 4);
    CHECK_EQ(r.workbench.icon(plain).top, start.top + 4);

    // Icons > Copy needs no .info and writes none.
    r.click(w.ram, start.left + 7, start.top + 7);
    r.workbench.menu_pick(w.ram, icons_pick(1));
    CHECK(logged("workbench: copied Ram Disk:Plain to Ram Disk:copy_of_Plain"));
    CHECK(r.exists("RAM:copy_of_Plain"));
    CHECK(!r.exists("RAM:copy_of_Plain.info"));
    const u32 copy = r.find_icon_in(w.ram, "copy_of_Plain");
    CHECK(copy != Workbench::kNoIcon);
    CHECK(r.workbench.icon(copy).plain);

    // Rename and delete it without an .info appearing.
    CHECK(r.workbench.rename_icon(w.ram, copy, "Renamed"));
    CHECK(r.exists("RAM:Renamed"));
    CHECK(!r.exists("RAM:Renamed.info"));
    CHECK(!r.exists("RAM:copy_of_Plain"));
    const u32 renamed = r.find_icon_in(w.ram, "Renamed");
    CHECK(renamed != Workbench::kNoIcon);
    CHECK(r.workbench.delete_icon(w.ram, renamed));
    CHECK(!r.exists("RAM:Renamed"));
    CHECK(!r.exists("RAM:Renamed.info"));
}

void a_plain_icon_moves_and_copies_between_drawers() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);

    // All Files shows the tool without an .info; to another volume it is
    // copied without one.
    r.workbench.menu_pick(w.tools, show_pick(1));
    const u32 tiny = r.find_icon_in(w.tools, "Tiny");
    CHECK(tiny != Workbench::kNoIcon);
    CHECK(r.workbench.icon(tiny).plain);
    const Workbench::IconEntry t = r.workbench.icon(tiny);
    r.drag(w.tools, t.left + 3, t.top + 3, r.screen_x(w.ram, 153), r.screen_y(w.ram, 43));
    CHECK(logged("workbench: copied HombreTest:Tools/Tiny to Ram Disk:"));
    CHECK(r.exists("RAM:Tiny"));
    CHECK(!r.exists("RAM:Tiny.info"));
    CHECK(r.exists("HombreTest:Tools/Tiny"));

    // On one volume a drop on a drawer moves it.
    r.workbench.menu_pick(w.ram, show_pick(1));
    u32 error = 0;
    r.make_dir("RAM:Box");
    const u32 drawer = g_icon.default_object(kWBDrawer);
    CHECK(g_icon.save_object("RAM:Box", drawer, error));
    g_icon.free_object(drawer);
    const u8 byte = 'x';
    CHECK(g_dos.write_file("RAM:Moved", &byte, 1, error));
    r.rescan("Ram Disk:");
    const u32 moved = r.find_icon_in(w.ram, "Moved");
    const u32 box_id = r.find_icon_in(w.ram, "Box");
    CHECK(moved != Workbench::kNoIcon && box_id != Workbench::kNoIcon);
    const Workbench::IconEntry m = r.workbench.icon(moved);
    const Workbench::IconEntry box = r.workbench.icon(box_id);
    r.drag(w.ram, m.left + 3, m.top + 3, r.screen_x(w.ram, box.left + 3),
           r.screen_y(w.ram, box.top + 3));
    CHECK(logged("workbench: moved Ram Disk:Moved to Ram Disk:Box"));
    CHECK(!r.exists("RAM:Moved"));
    CHECK(r.exists("RAM:Box/Moved"));
    CHECK(!r.exists("RAM:Box/Moved.info"));
}

void snapshots_and_leave_out_treat_a_plain_icon() {
    Rig r;
    put_ram_project("Note");
    const TwoWindows w = open_tools_and_ram(r);
    u32 error = 0;
    const u8 byte = 'x';
    CHECK(g_dos.write_file("RAM:Bare", &byte, 1, error));
    r.workbench.menu_pick(w.ram, show_pick(1));
    const u32 bare = r.find_icon_in(w.ram, "Bare");
    CHECK(bare != Workbench::kNoIcon);

    // Leave Out is refused with a log line, not a requester.
    r.click(w.ram, r.workbench.icon(bare).left + 1, r.workbench.icon(bare).top + 1);
    r.workbench.menu_pick(w.ram, icons_pick(6));
    CHECK(logged("workbench: Bare cannot be left out"));
    CHECK(std::string(r.workbench.last_message()).empty());
    CHECK(!r.exists("RAM:.backdrop"));
    CHECK(!r.exists("RAM:Bare.info"));

    // Unsnapshot keeps no snapshot.
    CHECK(!r.workbench.unsnapshot_icon(w.ram, bare));
    CHECK(logged("workbench: Bare keeps no snapshot"));
    CHECK(!r.exists("RAM:Bare.info"));

    // Snapshot All skips the plain icon but keeps the others' places.
    const u32 note = r.find_icon_in(w.ram, "Note");
    CHECK(note != Workbench::kNoIcon);
    r.workbench.menu_pick(w.ram, 1 | (6 << 5) | (1 << 11));
    CHECK(!r.exists("RAM:Bare.info"));
    CHECK(r.saved_place("RAM:Note") ==
          std::make_pair(static_cast<u32>(r.workbench.icon(note).left),
                         static_cast<u32>(r.workbench.icon(note).top)));
    const u32 disk = g_icon.load_object("RAM:Disk");
    CHECK(disk != 0);
    const u32 disk_data = r.get32(disk + do_DrawerData);
    CHECK(disk_data != 0);
    CHECK_EQ(r.get32(disk_data + dd_Flags), 2u);
    g_icon.free_object(disk);

    // Snapshot on the plain icon writes its .info; a refill makes it a
    // normal icon.
    CHECK(r.workbench.snapshot_icon(w.ram, bare));
    CHECK(r.exists("RAM:Bare.info"));
    CHECK(r.workbench.icon(bare).plain);
    r.rescan("Ram Disk:");
    const u32 normal = r.find_icon_in(w.ram, "Bare");
    CHECK(normal != Workbench::kNoIcon);
    CHECK(!r.workbench.icon(normal).plain);
}

void a_reset_keeps_each_drawers_show_setting() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    u32 error = 0;
    const u8 byte = 'x';
    CHECK(g_dos.write_file("RAM:Bare", &byte, 1, error));
    r.workbench.menu_pick(w.ram, show_pick(1));
    CHECK(r.find_icon_in(w.ram, "Bare") != Workbench::kNoIcon);
    CHECK(!r.workbench.window(w.tools).show_all);

    // Update keeps the setting and shows a new file.
    CHECK(g_dos.write_file("RAM:Late", &byte, 1, error));
    r.workbench.menu_pick(w.ram, 1 | (3 << 5) | (31 << 11));
    const u32 late = r.find_icon_in(w.ram, "Late");
    CHECK(late != Workbench::kNoIcon);
    CHECK(r.workbench.icon(late).plain);
    CHECK(!r.exists("RAM:Late.info"));

    // Tools > ResetWB.
    r.workbench.menu_pick(0, 3 | (0 << 5) | (31 << 11));
    const u32 ram = r.find_window("Ram Disk:");
    CHECK(ram != Workbench::kNoWindow);
    CHECK(r.workbench.window(ram).show_all);
    CHECK(r.find_icon_in(ram, "Bare") != Workbench::kNoIcon);
    CHECK(r.find_icon_in(ram, "Late") != Workbench::kNoIcon);
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kNoWindow);
    CHECK(!r.workbench.window(tools).show_all);
    CHECK_EQ(r.find_icon_in(tools, "Tiny"), Workbench::kNoIcon);
}

// On a 24-bit HOMBRE screen, whose BitMap says depth 24 but which has 256
// pens, a volume window still gets its fuel gauge.
void the_fuel_gauge_shows_on_a_24_bit_screen() {
    constexpr u32 kUsed = 700;
    Rig r(test_files(), gauge_disk("Second", kUsed), 1, 8);
    display_modes::configure_hombre(1280, 800, 8u * 1024 * 1024);
    constexpr u32 kTags = kData + 0x200;
    r.put32(kTags, SA_DisplayID);
    r.put32(kTags + 4, display_modes::hombre_id(0, 2));
    r.put32(kTags + 8, 0);
    const u32 deep = g_intuition.open_screen(0, kTags);
    CHECK(deep != 0);
    CHECK_EQ(r.get8(deep + sc_BitMap + bm_Depth), 24u);
    g_intuition.set_default_screen(deep);
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("Second"));
    const u32 drawer = r.find_window("Second:");
    CHECK(drawer != Workbench::kNoWindow);
    const Workbench::DrawerWindow& w = r.workbench.window(drawer);
    CHECK(w.gauge != 0);
    CHECK(w.gauge_image != 0);
    CHECK(r.get16(w.gauge + gg_Height) > 0u);
    display_modes::configure_hombre(0, 0, 0);
}

int main() {
    the_fuel_gauge_shows_on_a_24_bit_screen();
    resets_give_back_their_memory();
    the_backdrop_shows_a_wbpattern_picture();
    the_backdrop_shows_the_wbpattern_pattern();
    layout_matches_the_ndk();
    wb_config_keeps_the_fonts();
    wb_config_wakes_the_workbench_for_the_backdrops();
    an_icon_font_draws_and_lays_out_the_labels();
    start_starts_the_workbench_once();
    desktop_shows_the_volumes_and_their_menus();
    drawer_icons_keep_their_positions_and_use_the_grid();
    clicks_select_and_double_click_opens();
    clicks_shift_adds_and_removes_on_release();
    clicks_plain_press_on_a_member_collapses_on_release();
    clicks_shift_on_empty_keeps_the_selection();
    clicks_forget_drops_a_pending_change();
    shift_clicks_select_several_icons_in_a_drawer();
    select_contents_selects_every_icon();
    tool_startup_message_has_the_documented_layout();
    project_startup_names_the_tool_and_the_project();
    reply_unloads_the_seglist_and_unlocks();
    a_project_starts_its_default_tool();
    a_swapped_disk_changes_the_desktop();
    a_bare_default_tool_is_found_on_the_command_path();
    a_leading_slash_default_tool_is_in_the_parent_drawer();
    app_registrations_add_and_remove();
    an_app_icons_object_outlives_an_ended_program();
    menu_strip_lists_the_titles_and_disabled_items();
    the_show_menu_offers_only_icons_and_all_files();
    the_show_items_toggle_a_drawer_and_follow_the_active_window();
    a_drawer_opens_with_the_show_setting_in_its_icon();
    a_window_snapshot_stores_the_show_setting();
    an_old_style_drawer_icon_keeps_the_show_setting();
    plain_icons_come_after_the_real_ones_in_name_order();
    a_plain_icon_is_copied_renamed_and_deleted();
    a_plain_icon_moves_and_copies_between_drawers();
    snapshots_and_leave_out_treat_a_plain_icon();
    a_reset_keeps_each_drawers_show_setting();
    app_vectors_take_their_arguments_from_the_documented_registers();
    a_drawer_window_keeps_its_title_when_the_memory_it_came_from_is_reused();
    open_parent_opens_the_drawer_above();
    a_click_shows_the_icon_selected_and_a_rescan_drops_the_selection();
    the_desktop_waits_on_the_signals_of_its_ports();
    a_resized_drawer_is_measured_again();
    the_startup_message_signals_the_started_process();
    a_program_that_ends_without_replying_gives_back_what_it_held();
    backdrop_toggles_the_root_window();
    an_icon_dragged_in_its_window_takes_the_new_place();
    a_dragged_file_is_copied_moved_or_refused();
    a_drag_ends_when_its_window_is_filled_again();
    selected_icons_drag_together();
    a_dragged_group_keeps_its_offset_at_the_edge();
    two_projects_dragged_to_a_window_arrive_there();
    two_projects_dropped_on_a_drawer_icon_go_into_it();
    a_shift_press_on_a_selected_icon_drags_the_selection();
    a_box_selects_the_icons_it_touches();
    a_box_adds_to_the_selection_with_shift();
    a_box_without_shift_starts_from_nothing();
    a_box_is_clamped_to_the_inner_area();
    a_one_pixel_move_arms_a_box_without_activating_it();
    a_box_on_the_root_window_selects_disks();
    a_rescan_ends_a_box();
    a_dragged_disk_keeps_its_place_over_a_backdrop_toggle();
    a_disk_icon_inside_a_drawer_is_no_volume();
    a_resized_drawer_clips_its_icons_and_keeps_their_places();
    drawers_have_scroll_gadgets_in_their_borders();
    the_fuel_gauge_fills_by_the_used_share();
    volume_windows_show_a_fuel_gauge();
    the_classic_fuel_gauge_follows_its_window_state();
    the_modern_fuel_gauge_is_in_the_look_of_the_sliders();
    the_fuel_gauge_stays_out_of_the_bottom_border();
    drawer_arrows_take_the_look_of_the_screen();
    drawer_arrows_follow_the_screen_through_a_reset();
    border_gadgets_stay_when_a_drawer_turns_inactive();
    the_arrows_and_sliders_scroll_a_drawer();
    the_wheel_scrolls_a_drawer();
    a_window_snapshot_keeps_its_scroll();
    drawers_open_below_the_screen_bar();
    every_disk_in_a_drive_gets_an_icon();
    a_hard_disk_partition_gets_an_icon_after_the_floppies();
    snapshot_and_unsnapshot_store_the_icon_place();
    the_snapshot_item_snapshots_every_selected_icon();
    window_update_rescans_the_drawer();
    empty_trash_deletes_what_the_trashcan_holds();
    last_message_keeps_the_error_shown_last();
    quit_closes_the_desktop_and_it_can_open_again();
    quit_waits_for_the_programs_it_started();
    quit_waits_for_registrations();
    reset_wb_reopens_the_windows();
    leave_out_keeps_an_icon_on_the_root_window();
    a_left_out_icon_follows_its_file();
    the_backdrop_file_brings_icons_back();
    an_icon_without_a_place_keeps_clear_of_the_others();
    execute_command_hands_the_line_to_a_shell();
    copy_duplicates_a_file_or_a_drawer_under_a_new_name();
    copy_keeps_attributes_and_all_of_the_icon();
    copy_refuses_disks_and_keeps_names_short();
    the_copy_item_copies_every_selected_icon();
    new_drawer_makes_a_drawer_with_its_icon();
    the_new_drawer_item_suggests_a_free_name();
    rename_gives_the_file_and_its_icon_the_new_name();
    a_rename_that_loses_the_icon_name_is_undone();
    the_rename_item_asks_for_the_name();
    the_rename_item_renames_every_selected_icon();
    delete_removes_a_file_or_a_drawer_and_its_icon();
    delete_refuses_disks_and_what_cannot_be_written();
    the_delete_item_asks_before_it_deletes();
    the_delete_item_asks_once_for_several_icons();
    a_text_request_answers_return_and_its_buttons();
    a_set_answer_stands_in_for_the_requesters();
    the_information_window_shows_a_file();
    the_information_window_is_laid_out_around_the_image();
    the_information_window_groups_the_digits_of_a_size();
    a_check_box_and_the_comment_save_to_the_file();
    information_saves_only_what_changed();
    information_cancels_without_changing_anything();
    the_information_window_shows_a_drawer();
    the_information_window_shows_a_tool_and_a_project();
    tool_types_are_picked_edited_added_and_deleted();
    a_long_tool_type_and_a_typed_one_survive_a_save();
    tool_types_past_the_cap_survive_a_save();
    information_saves_the_icon_fields();
    information_cancels_the_icon_fields();
    an_icon_without_an_object_saves_its_icon_fields();
    information_shows_an_icon_without_an_object();
    the_information_window_shows_a_disk();
    the_information_window_fits_a_short_screen();
    a_window_snapshot_is_used_when_the_window_opens_again();
    a_reset_closes_and_reopens_the_windows();
    more_drawers_open_than_the_old_table_held();
    no_window_is_left_once_the_table_is_full();
    a_drawer_shows_more_icons_than_the_old_table_held();
    more_applications_register_than_the_old_table_held();
    several_programs_run_at_once();
    a_drawer_zooms_to_a_small_box_and_back_with_its_icons_in_place();
    the_root_window_has_a_zoom_gadget_only_when_it_is_not_a_backdrop();
    return check::finish("test_workbench");
}
