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

constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;  // test structures and strings up to 0xFFFF
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;

constexpr u8 kWBDisk = 1;
constexpr u8 kWBDrawer = 2;
constexpr u8 kWBTool = 3;
constexpr u8 kWBProject = 4;
constexpr u32 kNoIconPosition = 0x80000000;

// struct Menu and struct MenuItem
constexpr u32 mu_NextMenu = 0;
constexpr u32 mu_Flags = 12;
constexpr u32 mu_MenuName = 14;
constexpr u32 mu_FirstItem = 18;
constexpr u32 mi_NextItem = 0;
constexpr u32 mi_Flags = 12;
constexpr u32 mi_ItemFill = 18;
constexpr u16 MENUENABLED = 0x0001;
constexpr u16 ITEMTEXT = 0x0002;
constexpr u16 ITEMENABLED = 0x0010;
constexpr u32 it_IText = 12;

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
                                 i32 height) {
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
    f.u32v(0);   // CurrentX
    f.u32v(0);   // CurrentY
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
    };
}

// One memfd mapped twice for the code region, 16 MiB of guest memory, the
// Jit and the native libraries over them, with the Workbench's screen open
// and made the default one.
class Rig {
public:
    explicit Rig(const std::vector<AdfFile>& files = test_files())
        : memory_(kGuestBytes), image_(build_adf("HombreTest", false, files)) {
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
        CHECK(g_dos.init(m, g_exec, volume, nullptr, nullptr));
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

private:
    static constexpr usize kRegion = 1 << 20;
    std::vector<u8> memory_;
    std::vector<u8> image_;
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
    CHECK(title.rfind("Amiga Workbench", 0) == 0);
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
    const u64 late = 2100000 + Workbench::kDoubleClickUs + 1;
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
    const u32 window = r.workbench.add_app_window(7, 0x11, 0, 0);
    CHECK(window != 0);
    CHECK_EQ(r.get32(window), 0u);
    CHECK_EQ(r.workbench.app(0).id, 7u);
    CHECK_EQ(r.workbench.app(0).user, 0x11u);
    r.workbench.remove_app_window(window);
    CHECK(!r.workbench.app(0).used);
    r.workbench.remove_app_window(window);

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
    CHECK_EQ(r.get16(r.get32(update_all + mi_NextItem) + mi_Flags) & ITEMENABLED, 0u);
}

}

int main() {
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
    return check::finish("test_workbench");
}
