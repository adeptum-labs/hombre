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

#include "libs/dos.hpp"
#include "libs/exec.hpp"
#include "libs/gadtools.hpp"
#include "libs/graphics.hpp"
#include "libs/icon.hpp"
#include "libs/intuition.hpp"
#include "libs/workbench.hpp"
#include "check.hpp"
#include "ofs_writer.hpp"

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
constexpr u32 kFreeEnd = 0x400000;

constexpr u8 kWBDisk = 1;
constexpr u8 kWBDrawer = 2;
constexpr u8 kWBTool = 3;
constexpr u8 kWBProject = 4;
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
constexpr u32 do_CurrentX = 58;
constexpr u32 do_CurrentY = 62;
constexpr u32 do_DrawerData = 66;

// struct WBStartup and struct WBArg
constexpr u32 sm_Process = 20;
constexpr u32 sm_Segment = 24;
constexpr u32 sm_NumArgs = 28;
constexpr u32 sm_ToolWindow = 32;
constexpr u32 sm_ArgList = 36;
constexpr u32 WBStartup_SIZEOF = 40;
constexpr u32 wa_Lock = 0;
constexpr u32 wa_Name = 4;

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

void put_gadget(Info& f, u32 width, u32 height) {
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
    f.u32v(1);  // UserData: WB_DISKREVISION
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
                bool default_tool) {
    f.word(0xE310);
    f.word(1);
    put_gadget(f, 32, 12);
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
                                 i32 height, i32 scroll_x = 0, i32 scroll_y = 0) {
    Info f;
    put_header(f, kWBDrawer, current_x, current_y, true, false);
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
    f.u32v(0);  // dd_Flags
    f.word(0);  // dd_ViewModes
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
        {"Tools/Sub.info", drawer_icon_info(200, 20, 60, 70, 200, 90)},
        {"Tools/Sub/Deep.info", icon_info(kWBTool, 10, 10)},
        {"Tools/Fake.info", icon_info(kWBDisk, 250, 10)},
        {"Tools/Fake/Inside.info", icon_info(kWBTool, 10, 10)},
    };
}

// One memfd mapped twice for the code region, 16 MiB of guest memory, the
// Jit and the native libraries over them, with the Workbench's screen open
// and made the default one.
class Rig {
public:
    explicit Rig(const std::vector<AdfFile>& files = test_files(),
                 std::vector<u8> second = {}, u32 second_drive = 1)
        : memory_(kGuestBytes),
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
        m.mem = GuestMemory{memory_.data(), kGuestBytes};
        m.jit = nullptr;
        CHECK(g_jit.init(CodeRegion{rw_, rx_, kRegion}, m));
        g_lines.clear();
        g_log = capture;
        CHECK(g_exec.init(m, kFreeStart, kFreeEnd));
        CHECK(adf_mount(image_.data(), image_.size(), volume) == FsError::Ok);
        if (second_image_.empty()) {
            CHECK(g_dos.init(m, g_exec, volume, nullptr, nullptr));
        } else {
            CHECK(adf_mount(second_image_.data(), second_image_.size(), second_volume) ==
                  FsError::Ok);
            Drives drives;
            drives.volume[0] = &volume;
            drives.volume[second_drive] = &second_volume;
            CHECK(g_dos.init(m, g_exec, drives, nullptr, nullptr));
        }
        CHECK(g_icon.init(m, g_exec, g_dos));
        g_display = DisplayState{};
        CHECK(g_graphics.init(m, g_exec, g_display));
        CHECK(g_intuition.init(m, g_exec, g_graphics, g_display));
        CHECK(g_gadtools.init(m, g_exec, g_graphics));
        CHECK(workbench.init(m, g_exec, g_dos, g_graphics, g_intuition, g_gadtools, g_icon));
        m.cpu.a[7] = kStack;
        screen = open_screen();
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

    u32 open_screen() {
        put16(kData + ns_Width, 640);
        put16(kData + ns_Height, 400);
        put16(kData + ns_Depth, 2);
        put16(kData + ns_Type, CUSTOMSCREEN);
        const u32 opened = g_intuition.open_screen(kData, 0);
        CHECK(opened != 0);
        return opened;
    }

    u32 find_icon(const std::string& name) const {
        for (u32 i = 0; i < Workbench::kMaxIcons; ++i) {
            const Workbench::IconEntry& icon = workbench.icon(i);
            if (icon.used && name == icon.name) {
                return i;
            }
        }
        return Workbench::kNoIcon;
    }

    u32 find_window(const std::string& path) const {
        for (u32 i = 0; i < Workbench::kMaxWindows; ++i) {
            if (workbench.window(i).used && path == workbench.window(i).path) {
                return i;
            }
        }
        return Workbench::kMaxWindows;
    }

    u32 find_tool(const std::string& name) const {
        for (u32 i = 0; i < Workbench::kMaxTools; ++i) {
            if (workbench.tool(i).used && name == workbench.tool(i).name) {
                return i;
            }
        }
        return Workbench::kMaxTools;
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

    // The pen at x, y of the window's own bitmap.
    u32 window_pixel(u32 window, i32 x, i32 y) const {
        const u32 bitmap = get32(get32(window + wd_RPort) + rp_BitMap);
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
        for (u32 i = 0; i < Workbench::kMaxIcons; ++i) {
            const Workbench::IconEntry& icon = workbench.icon(i);
            if (icon.used && icon.owner == window_index && name == icon.name) {
                return i;
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
    CHECK_EQ(workbench_lvo::WBInfo, 90u);
    CHECK_EQ(sm_Process, 20u);
    CHECK_EQ(WBStartup_SIZEOF, 40u);
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

    const Workbench::DrawerWindow& backdrop = r.workbench.window(0);
    CHECK(backdrop.used);
    CHECK(backdrop.window != 0);
    CHECK(r.window_title(backdrop.window) == "Workbench");
    CHECK_EQ(r.get16(backdrop.window + wd_LeftEdge), 0u);
    CHECK_EQ(r.get16(backdrop.window + wd_TopEdge), 14u);
    CHECK_EQ(r.get16(backdrop.window + wd_Width), 640u);
    CHECK_EQ(r.get16(backdrop.window + wd_Height), 386u);
    CHECK_EQ(r.get32(backdrop.window + 28), r.workbench.menu());  // wd_MenuStrip

    const std::string title = r.str(r.get32(r.screen + sc_Title));
    CHECK(title.rfind("Hombre Workbench", 0) == 0);
    CHECK(title.find("graphics mem") != std::string::npos);
    CHECK(title.find("other mem") != std::string::npos);
}

void drawer_icons_keep_their_positions_and_use_the_grid() {
    Rig r;
    r.workbench.open_desktop();
    const u32 volume = r.find_icon("HombreTest");
    CHECK(volume != Workbench::kNoIcon);

    r.workbench.open_icon(0, volume);
    const u32 root = r.find_window("HombreTest:");
    CHECK(root != Workbench::kMaxWindows);
    const u32 tools = r.find_icon("Tools");
    CHECK(tools != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(tools).type, kWBDrawer);

    r.workbench.open_icon(root, tools);
    const u32 drawer = r.find_window("HombreTest:Tools");
    CHECK(drawer != Workbench::kMaxWindows);
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
    CHECK_EQ(clicks.click(3, 1000), Workbench::ClickAction::Select);
    CHECK_EQ(clicks.selected(), 3u);
    CHECK_EQ(clicks.click(3, 1000 + 400000), Workbench::ClickAction::Open);
    CHECK_EQ(clicks.click(7, 2000000), Workbench::ClickAction::Select);
    CHECK_EQ(clicks.selected(), 7u);
    CHECK_EQ(clicks.click(7, 2100000), Workbench::ClickAction::Open);
    // A third quick click does not open the icon a second time.
    CHECK_EQ(clicks.click(7, 2150000), Workbench::ClickAction::None);
    CHECK_EQ(clicks.click(7, 2200000), Workbench::ClickAction::Open);
    const u64 late = 2200000 + Workbench::kDoubleClickUs + 1;
    CHECK_EQ(clicks.click(7, late), Workbench::ClickAction::None);
    CHECK_EQ(clicks.click(7, late + 200000), Workbench::ClickAction::Open);
    CHECK_EQ(clicks.click(Workbench::kNoIcon, late + 2000000), Workbench::ClickAction::Clear);
    CHECK_EQ(clicks.selected(), Workbench::kNoIcon);
    CHECK_EQ(clicks.click(Workbench::kNoIcon, late + 2100000), Workbench::ClickAction::None);

    // A second click stops being a double click after kDoubleClickUs.
    Workbench::Clicks slow;
    CHECK_EQ(slow.click(2, 0), Workbench::ClickAction::Select);
    CHECK_EQ(slow.click(2, Workbench::kDoubleClickUs + 1), Workbench::ClickAction::None);
    const u64 base = 3 * Workbench::kDoubleClickUs;
    CHECK_EQ(slow.click(5, base), Workbench::ClickAction::Select);
    CHECK_EQ(slow.click(5, base + Workbench::kDoubleClickUs), Workbench::ClickAction::Open);
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

    const u32 slot = r.find_tool("Tiny");
    CHECK(slot != Workbench::kMaxTools);
    const Workbench::RunningTool tool = r.workbench.tool(slot);
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
    CHECK(!r.workbench.tool(slot).used);
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
    CHECK(tools_window != Workbench::kMaxWindows);

    const u32 project = r.find_icon("Proj");
    CHECK(project != Workbench::kNoIcon);
    CHECK_EQ(r.workbench.icon(project).type, kWBProject);
    r.workbench.open_icon(tools_window, project);

    const u32 slot = r.find_tool("Tiny");
    CHECK(slot != Workbench::kMaxTools);
    const Workbench::RunningTool tool = r.workbench.tool(slot);
    const u32 arglist = r.get32(tool.message + sm_ArgList);
    CHECK_EQ(r.get32(tool.message + sm_NumArgs), 2u);
    CHECK(r.str(r.get32(arglist + wa_Name)) == "Tiny");
    CHECK(r.str(r.get32(arglist + 8 + wa_Name)) == "Proj");
    CHECK(r.workbench.tool_reply(tool.message));
}

void app_registrations_add_and_remove() {
    Rig r;
    r.workbench.open_desktop();
    const u32 window = r.workbench.add_app_window(7, 0x11, 0, 0, 0);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window), 0u);
    CHECK_EQ(r.workbench.app(0).id, 7u);
    CHECK_EQ(r.workbench.app(0).user, 0x11u);
    CHECK(r.workbench.remove_app_window(window));
    CHECK(!r.workbench.app(0).used);
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
    CHECK(!r.workbench.app(0).used);
    CHECK(!r.workbench.app(1).used);
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
    CHECK_EQ(r.workbench.app(0).id, 9u);
    CHECK_EQ(r.workbench.app(0).user, 0x22u);

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
    CHECK(root != Workbench::kMaxWindows);
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
    CHECK(tools != Workbench::kMaxWindows);
    r.workbench.open_icon(tools, r.find_icon("Sub"));
    const u32 sub = r.find_window("HombreTest:Tools/Sub");
    CHECK(sub != Workbench::kMaxWindows);
    CHECK(r.find_icon("Deep") != Workbench::kNoIcon);

    // With the drawers above closed, Open Parent opens each again at the
    // place its icon gives it.
    r.workbench.close_drawer(tools);
    r.workbench.close_drawer(root);
    CHECK_EQ(r.find_window("HombreTest:Tools"), Workbench::kMaxWindows);
    r.workbench.open_parent(sub);
    const u32 tools_again = r.find_window("HombreTest:Tools");
    CHECK(tools_again != Workbench::kMaxWindows);
    const u32 tools_window = r.workbench.window(tools_again).window;
    CHECK(r.window_title(tools_window) == "Tools");
    CHECK_EQ(r.get16(tools_window + wd_LeftEdge), 30u);
    CHECK_EQ(r.get16(tools_window + wd_TopEdge), 50u);
    CHECK_EQ(r.get16(tools_window + wd_Width), 300u);
    CHECK(r.find_icon("Proj") != Workbench::kNoIcon);

    r.workbench.open_parent(tools_again);
    const u32 root_again = r.find_window("HombreTest:");
    CHECK(root_again != Workbench::kMaxWindows);
    CHECK(r.window_title(r.workbench.window(root_again).window) == "HombreTest");

    // An open parent only comes forward, and a disk has none.
    u32 open_before = 0;
    for (u32 i = 0; i < Workbench::kMaxWindows; ++i) {
        open_before += r.workbench.window(i).used ? 1 : 0;
    }
    r.workbench.open_parent(sub);
    r.workbench.open_parent(root_again);
    r.workbench.open_parent(0);
    u32 open_after = 0;
    for (u32 i = 0; i < Workbench::kMaxWindows; ++i) {
        open_after += r.workbench.window(i).used ? 1 : 0;
    }
    CHECK_EQ(open_after, open_before);
}

void a_click_shows_the_icon_selected_and_a_rescan_drops_the_selection() {
    Rig r;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon("Tools"));
    const u32 tools = r.find_window("HombreTest:Tools");
    CHECK(tools != Workbench::kMaxWindows);
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

    // The icon has no second image: selected, every pen of it is inverted.
    r.click(tools, tool.left + 3, tool.top + 3);
    const std::vector<u32> selected = image();
    CHECK(selected != plain);
    for (usize i = 0; i < plain.size(); ++i) {
        CHECK_EQ(selected[i], plain[i] ^ 3u);
    }

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
    const u32 backdrop_bit = r.get8(r.workbench.window(0).user_port + mp_SigBit);
    CHECK(reply_bit != backdrop_bit);
    CHECK_EQ(r.workbench.wait_mask(), (1u << reply_bit) | (1u << backdrop_bit));

    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    const u32 root_bit = r.get8(r.workbench.window(root).user_port + mp_SigBit);
    CHECK_EQ(r.workbench.wait_mask(),
             (1u << reply_bit) | (1u << backdrop_bit) | (1u << root_bit));

    // Nothing waits at the ports once they are read; a click in the
    // window raises its signal again.
    r.workbench.pump();
    g_exec.wait_for(r.workbench.wait_mask(), 0);
    CHECK_EQ(g_exec.wait_for(r.workbench.wait_mask(), 0), 0u);
    const Workbench::DrawerWindow& w = r.workbench.window(root);
    const i32 x = static_cast<i32>(r.get16(w.window + wd_LeftEdge)) + w.inner_left + 250;
    const i32 y = static_cast<i32>(r.get16(w.window + wd_TopEdge)) + w.inner_top + 100;
    g_intuition.pointer_event(x, y, 1, 1, 0, true);
    CHECK_EQ(g_exec.wait_for(r.workbench.wait_mask(), 0), 1u << root_bit);
    CHECK(!r.workbench.pump());

    r.workbench.close_drawer(root);
    CHECK_EQ(r.workbench.wait_mask(), (1u << reply_bit) | (1u << backdrop_bit));
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
    const Workbench::RunningTool tool = r.workbench.tool(r.find_tool("Tiny"));

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
    const u32 slot = r.find_tool("Tiny");
    const Workbench::RunningTool tool = r.workbench.tool(slot);

    // While its process lives the program keeps its record.
    r.workbench.reap_tools();
    CHECK(r.workbench.tool(slot).used);

    // The process is removed as a faulted one is, its message unanswered.
    r.m.cpu.a[1] = tool.process;
    CHECK_EQ(r.call(g_exec.exec_base(), exec_lvo::RemTask), ExitReason::Halt);
    r.workbench.pump();
    CHECK(!r.workbench.tool(slot).used);
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
    CHECK_EQ(r.get16(new_drawer + mi_Flags) & ITEMENABLED, 0u);
    CHECK(r.str(r.get32(r.get32(open_parent + mi_ItemFill) + it_IText)) == "Open Parent");
    CHECK_EQ(r.get16(open_parent + mi_Flags) & ITEMENABLED, ITEMENABLED);
    CHECK_EQ(r.get16(open_parent + mi_Flags) & ITEMTEXT, ITEMTEXT);

    const u32 open = r.get32(icons + mu_FirstItem);
    CHECK(r.str(r.get32(r.get32(open + mi_ItemFill) + it_IText)) == "Open");
    CHECK_EQ(r.get16(open + mi_Flags) & ITEMENABLED, ITEMENABLED);
    CHECK_EQ(r.get16(r.get32(open + mi_NextItem) + mi_Flags) & ITEMENABLED, 0u);

    const u32 workbench_items = r.get32(first + mu_FirstItem);
    const u32 redraw_all = r.get32(r.get32(workbench_items + mi_NextItem) + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(redraw_all + mi_ItemFill) + it_IText)) == "Redraw All");
    CHECK_EQ(r.get16(redraw_all + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 update_all = r.get32(redraw_all + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(update_all + mi_ItemFill) + it_IText)) == "Update All");
    CHECK_EQ(r.get16(update_all + mi_Flags) & ITEMENABLED, ITEMENABLED);
    const u32 last_message = r.get32(update_all + mi_NextItem);
    CHECK_EQ(r.get16(last_message + mi_Flags) & ITEMENABLED, 0u);
    const u32 about = r.get32(last_message + mi_NextItem);
    CHECK(r.str(r.get32(r.get32(about + mi_ItemFill) + it_IText)) == "About...");
    CHECK_EQ(r.get16(about + mi_Flags) & ITEMENABLED, ITEMENABLED);
    CHECK_EQ(r.get16(about + mi_Flags) & ITEMTEXT, ITEMTEXT);

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
    CHECK(drawer != Workbench::kMaxWindows);
    const u32 drawer_window = r.workbench.window(drawer).window;
    const u32 before = r.free_bytes();

    // Intuition turns the check mark off before the pick reaches the
    // desktop.
    r.put16(item + mi_Flags, r.get16(item + mi_Flags) & ~CHECKED);
    r.workbench.menu_pick(0, 0);
    CHECK(!r.workbench.backdrop());
    CHECK(logged("workbench: backdrop off"));
    const Workbench::DrawerWindow& root = r.workbench.window(0);
    CHECK(root.used);
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
    u32 tools = Workbench::kMaxWindows;
    u32 ram = Workbench::kMaxWindows;
};

TwoWindows open_tools_and_ram(Rig& r) {
    TwoWindows w;
    r.workbench.open_desktop();
    r.workbench.open_icon(0, r.find_icon("HombreTest"));
    const u32 root = r.find_window("HombreTest:");
    r.workbench.open_icon(root, r.find_icon_in(root, "Tools"));
    w.tools = r.find_window("HombreTest:Tools");
    CHECK(w.tools != Workbench::kMaxWindows);
    r.workbench.close_drawer(root);
    g_intuition.move_window_to(r.workbench.window(w.tools).window, 20, 30);
    r.workbench.open_icon(0, r.find_icon_in(0, "Ram Disk"));
    w.ram = r.find_window("Ram Disk:");
    CHECK(w.ram != Workbench::kMaxWindows);
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

void a_dragged_disk_keeps_its_place_over_a_backdrop_toggle() {
    Rig r;
    r.workbench.open_desktop();
    const u32 disk = r.find_icon("HombreTest");
    const Workbench::IconEntry before = r.workbench.icon(disk);
    r.drag(0, before.left + 3, before.top + 3, r.screen_x(0, 103), r.screen_y(0, 123));
    CHECK_EQ(r.workbench.icon(disk).left, 100);
    CHECK_EQ(r.workbench.icon(disk).top, 120);
    r.workbench.set_backdrop(false);
    CHECK_EQ(r.workbench.icon(disk).left, 100);
    CHECK_EQ(r.workbench.icon(disk).top, 120);
    r.workbench.set_backdrop(true);
    CHECK_EQ(r.workbench.icon(disk).left, 100);
    CHECK_EQ(r.workbench.icon(disk).top, 120);
}

void a_disk_icon_inside_a_drawer_is_no_volume() {
    Rig r;
    const TwoWindows w = open_tools_and_ram(r);
    const u32 fake = r.find_icon_in(w.tools, "Fake");
    CHECK(fake != Workbench::kNoIcon);
    r.workbench.open_icon(w.tools, fake);
    CHECK(r.find_window("HombreTest:Tools/Fake") != Workbench::kMaxWindows);
    CHECK_EQ(r.find_window("Fake:"), Workbench::kMaxWindows);
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
    CHECK(labels != Workbench::kMaxWindows);
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
    CHECK(long_drawer != Workbench::kMaxWindows);
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
    CHECK_EQ(gadget, 0u);
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
    CHECK(ram != Workbench::kMaxWindows);
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
        CHECK(window != Workbench::kMaxWindows);
        CHECK(r.find_icon_in(window, "Docs") != Workbench::kNoIcon);
        CHECK(r.window_title(r.workbench.window(window).window) == "Second");
    }

    // Two disks of one name open their own drives.
    const std::vector<AdfFile> twin_files = {{"Only.info", icon_info(kWBTool, 10, 10)}};
    Rig twins(test_files(), build_adf("HombreTest", false, twin_files), 1);
    twins.workbench.open_desktop();
    u32 disks[2] = {Workbench::kNoIcon, Workbench::kNoIcon};
    u32 found = 0;
    for (u32 i = 0; i < Workbench::kMaxIcons && found < 2; ++i) {
        const Workbench::IconEntry& icon = twins.workbench.icon(i);
        if (icon.used && icon.owner == 0 && std::string(icon.name) == "HombreTest") {
            disks[found++] = i;
        }
    }
    CHECK_EQ(found, 2u);
    twins.workbench.open_icon(0, disks[0]);
    twins.workbench.open_icon(0, disks[1]);
    const u32 df0 = twins.find_window("DF0:");
    const u32 df1 = twins.find_window("DF1:");
    CHECK(df0 != Workbench::kMaxWindows);
    CHECK(df1 != Workbench::kMaxWindows);
    CHECK(twins.find_icon_in(df1, "Only") != Workbench::kNoIcon);
    CHECK(twins.find_icon_in(df0, "Tools") != Workbench::kNoIcon);
    CHECK(twins.window_title(twins.workbench.window(df1).window) == "HombreTest");
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
    CHECK(ram != Workbench::kMaxWindows);
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
}

int main() {
    the_backdrop_shows_the_wbpattern_pattern();
    layout_matches_the_ndk();
    start_starts_the_workbench_once();
    desktop_shows_the_volumes_and_their_menus();
    drawer_icons_keep_their_positions_and_use_the_grid();
    clicks_select_and_double_click_opens();
    tool_startup_message_has_the_documented_layout();
    project_startup_names_the_tool_and_the_project();
    reply_unloads_the_seglist_and_unlocks();
    a_project_starts_its_default_tool();
    app_registrations_add_and_remove();
    menu_strip_lists_the_titles_and_disabled_items();
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
    a_dragged_disk_keeps_its_place_over_a_backdrop_toggle();
    a_disk_icon_inside_a_drawer_is_no_volume();
    a_resized_drawer_clips_its_icons_and_keeps_their_places();
    drawers_have_scroll_gadgets_in_their_borders();
    the_arrows_and_sliders_scroll_a_drawer();
    a_window_snapshot_keeps_its_scroll();
    drawers_open_below_the_screen_bar();
    every_disk_in_a_drive_gets_an_icon();
    snapshot_and_unsnapshot_store_the_icon_place();
    a_window_snapshot_is_used_when_the_window_opens_again();
    return check::finish("test_workbench");
}
