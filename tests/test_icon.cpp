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
#include "libs/gfx.hpp"
#include "libs/icon.hpp"
#include "check.hpp"
#include "ofs_writer.hpp"

#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
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
constexpr u8 kWBGarbage = 5;
constexpr u8 kWBKick = 7;

// struct DiskObject
constexpr u32 do_Magic = 0;
constexpr u32 do_Version = 2;
constexpr u32 do_Gadget = 4;
constexpr u32 do_Type = 48;
constexpr u32 do_DefaultTool = 50;
constexpr u32 do_ToolTypes = 54;
constexpr u32 do_CurrentX = 58;
constexpr u32 do_CurrentY = 62;
constexpr u32 do_DrawerData = 66;
constexpr u32 do_ToolWindow = 70;
constexpr u32 do_StackSize = 74;

// struct Image
constexpr u32 img_LeftEdge = 0;
constexpr u32 img_TopEdge = 2;
constexpr u32 img_Width = 4;
constexpr u32 img_Height = 6;
constexpr u32 img_Depth = 8;
constexpr u32 img_ImageData = 10;
constexpr u32 img_PlanePick = 14;
constexpr u32 img_PlaneOnOff = 15;
constexpr u32 img_NextImage = 16;

// struct DrawerData
constexpr u32 dd_CurrentX = 48;
constexpr u32 dd_CurrentY = 52;
constexpr u32 dd_Flags = 56;
constexpr u32 dd_ViewModes = 60;

// Static storage, as the kernel keeps them.
Jit g_jit;
Exec g_exec;
Dos g_dos;
Icon g_icon;
std::vector<std::string> g_lines;

void capture(const char* line) { g_lines.push_back(line); }

bool logged_containing(const std::string& text) {
    for (const std::string& line : g_lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

u16 disp(u32 offset) { return static_cast<u16>(0x10000 - offset); }

// A big-endian .info builder: a DiskObject and the parts that follow it.
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

    void blob(const std::vector<u8>& data) { bytes.insert(bytes.end(), data.begin(), data.end()); }

    // A LONG length including the NUL and the bytes.
    void string(const std::string& text) {
        u32v(static_cast<u32>(text.size()) + 1);
        for (char c : text) {
            byte(static_cast<u8>(c));
        }
        byte(0);
    }
};

// The gadget of a DiskObject. render and select say whether an Image
// follows, as the file format stores the pointers.
void put_gadget(Info& f, u32 left, u32 top, u32 width, u32 height, u32 flags, u32 render,
                u32 select, u32 user_data) {
    f.u32v(0);  // NextGadget
    f.word(left);
    f.word(top);
    f.word(width);
    f.word(height);
    f.word(flags);
    f.word(1);  // Activation
    f.word(1);  // GadgetType
    f.u32v(render);
    f.u32v(select);
    f.u32v(0);  // GadgetText
    f.u32v(0);  // MutualExclude
    f.u32v(0);  // SpecialInfo
    f.word(0);   // GadgetID
    f.u32v(user_data);
}

void put_image(Info& f, u32 left, u32 top, u32 width, u32 height, u32 depth, bool with_data,
               const std::vector<u8>& data, u32 plane_pick, u32 plane_on_off) {
    f.word(left);
    f.word(top);
    f.word(width);
    f.word(height);
    f.word(depth);
    f.u32v(with_data ? 1 : 0);
    f.byte(plane_pick);
    f.byte(plane_on_off);
    f.u32v(0);  // NextImage
    if (with_data) {
        f.blob(data);
    }
}

std::vector<u8> tool_info() {
    Info f;
    f.word(0xE310);
    f.word(1);
    put_gadget(f, 5, 6, 16, 9, 0, 1, 0, 0);
    f.byte(kWBTool);
    f.byte(0);
    f.u32v(1);           // do_DefaultTool
    f.u32v(1);           // do_ToolTypes
    f.u32v(0x00010002);  // do_CurrentX
    f.u32v(0xFFFFFFFE);  // do_CurrentY
    f.u32v(0);           // do_DrawerData
    f.u32v(0);           // do_ToolWindow
    f.u32v(8192);        // do_StackSize
    std::vector<u8> planes(18);
    for (u32 i = 0; i < 18; ++i) {
        planes[i] = static_cast<u8>(i == 0 ? 0xA5 : i * 3);
    }
    put_image(f, 1, 2, 16, 9, 1, true, planes, 1, 0);
    f.string("C:Editor");
    f.u32v((3 + 1) * 4);
    f.string("FONT=topaz");
    f.string("PUBSCREEN=Workbench");
    f.string("STACK");
    return std::move(f.bytes);
}

// user_data's low byte 1 is WB_DISKREVISION.
std::vector<u8> drawer_info(u32 user_data = 1) {
    Info f;
    f.word(0xE310);
    f.word(1);
    put_gadget(f, 0, 0, 8, 4, 0, 1, 0, user_data);
    f.byte(kWBDrawer);
    f.byte(0);
    f.u32v(0);            // do_DefaultTool
    f.u32v(0);            // do_ToolTypes
    f.u32v(0x01020304);   // do_CurrentX
    f.u32v(0x05060708);   // do_CurrentY
    f.u32v(1);            // do_DrawerData
    f.u32v(0);            // do_ToolWindow
    f.u32v(4096);         // do_StackSize
    // OldDrawerData
    f.word(10);
    f.word(20);
    f.word(300);
    f.word(120);
    f.byte(1);
    f.byte(2);
    f.u32v(0x1234);  // IDCMPFlags
    f.u32v(0x5678);  // Flags
    f.u32v(0);       // FirstGadget
    f.u32v(0);       // CheckMark
    f.u32v(0);       // Title
    f.u32v(0);       // Screen
    f.u32v(0);       // BitMap
    f.word(100);
    f.word(50);
    f.word(400);
    f.word(200);
    f.word(1);  // Type
    f.u32v(0x11223344);
    f.u32v(0x55667788);
    std::vector<u8> planes(16);
    for (u32 i = 0; i < 16; ++i) {
        planes[i] = static_cast<u8>(0x80 + i);
    }
    put_image(f, 0, 0, 8, 4, 2, true, planes, 3, 0);
    // The OS 2 disk revision comes after every other part.
    if ((user_data & 255) == 1) {
        f.u32v(0x0000ABCD);  // dd_Flags
        f.word(0x0F0F);       // dd_ViewModes
    }
    return std::move(f.bytes);
}

// The DiskObject of a tool icon with no drawer data; the flags say which
// parts follow.
void put_header(Info& f, u32 gadget_flags, u32 render, u32 select, u32 default_tool,
                u32 tool_types, u32 tool_window) {
    f.word(0xE310);
    f.word(1);
    put_gadget(f, 0, 0, 16, 2, gadget_flags, render, select, 0);
    f.byte(kWBTool);
    f.byte(0);
    f.u32v(default_tool);
    f.u32v(tool_types);
    f.u32v(0);  // do_CurrentX
    f.u32v(0);  // do_CurrentY
    f.u32v(0);  // do_DrawerData
    f.u32v(tool_window);
    f.u32v(0);  // do_StackSize
}

const std::vector<u8> kSmallPlane = {0x11, 0x22, 0x33, 0x44};

// A tool icon with a default tool, count tool types "T<i>=<i>" and a tool
// window.
std::vector<u8> many_types_info(u32 count) {
    Info f;
    put_header(f, 0, 1, 0, 1, 1, 1);
    put_image(f, 0, 0, 16, 2, 1, true, kSmallPlane, 1, 0);
    f.string("SYS:Tool");
    f.u32v((count + 1) * 4);
    for (u32 i = 0; i < count; ++i) {
        f.string("T" + std::to_string(i) + "=" + std::to_string(i));
    }
    f.string("CON:0/0/100/50/Window");
    return std::move(f.bytes);
}

// SelectRender set although the gadget does not highlight by image: no
// second Image follows.
std::vector<u8> select_without_highlight_info() {
    Info f;
    put_header(f, 0, 1, 1, 1, 0, 0);
    put_image(f, 0, 0, 16, 2, 1, true, kSmallPlane, 1, 0);
    f.string("Tool");
    return std::move(f.bytes);
}

// An Image whose ImageData is null has no plane data after it.
std::vector<u8> image_without_data_info() {
    Info f;
    put_header(f, 0, 1, 0, 1, 0, 0);
    put_image(f, 0, 0, 16, 2, 0, false, {}, 0, 2);
    f.string("Tool");
    return std::move(f.bytes);
}

std::vector<u8> project_info() {
    Info f;
    f.word(0xE310);
    f.word(1);
    put_gadget(f, 0, 0, 12, 6, 2, 1, 1, 0);  // GFLG_GADGHIMAGE
    f.byte(kWBProject);
    f.byte(0);
    f.u32v(0);
    f.u32v(0);
    f.u32v(0);
    f.u32v(0);
    f.u32v(0);
    f.u32v(0);
    f.u32v(0);
    std::vector<u8> render(12);
    std::vector<u8> select(12);
    for (u32 i = 0; i < 12; ++i) {
        render[i] = static_cast<u8>(0x10 + i);
        select[i] = static_cast<u8>(0xF0 - i);
    }
    put_image(f, 0, 0, 12, 6, 1, true, render, 1, 0);
    put_image(f, 0, 0, 12, 6, 1, true, select, 1, 0);
    return std::move(f.bytes);
}

std::vector<u8> text(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }

std::vector<AdfFile> test_files() {
    std::vector<u8> bad = tool_info();
    bad[0] = 0x12;
    bad[1] = 0x34;
    std::vector<u8> truncated = tool_info();
    truncated.pop_back();
    return {
        {"Tools/Tool.info", tool_info()},
        {"Tools/Drawer.info", drawer_info()},
        {"Tools/Project.info", project_info()},
        {"Tools/BadMagic.info", bad},
        {"Tools/Truncated.info", truncated},
        {"Tools/Many.info", many_types_info(300)},
        {"Tools/NoHighlight.info", select_without_highlight_info()},
        {"Tools/NoData.info", image_without_data_info()},
        {"Tools/HighRev.info", drawer_info(0x12345601)},
        {"Tools/OldRev.info", drawer_info(0)},
        {"Tools/NoIcon", text("plain file")},
        {"Tools/Sub/Deep", text("deep")},
    };
}

// One memfd mapped twice for the code region, 16 MiB of guest memory, the
// Jit, exec, dos and icon.library over them.
class Rig {
public:
    explicit Rig(const std::vector<AdfFile>& files = test_files())
        : memory_(kGuestBytes), image_(build_adf("TestDisk", false, files)) {
        fd_ = memfd_create("icon", 0);
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
        m.cpu.a[7] = kStack;
    }

    ~Rig() {
        g_exec.set_this_task(0);
        g_exec.set_segment_loader(SegmentLoader{nullptr, nullptr});
        g_exec.set_task_host(TaskHost{});
        g_log = nullptr;
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine m;
    AdfVolume volume{};

    u32 put8(u32 at, u32 v) {
        memory_[at] = static_cast<u8>(v);
        return at;
    }

    u32 put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
        return at;
    }

    u32 put32(u32 at, u32 v) {
        put16(at, v >> 16);
        put16(at + 2, v);
        return at;
    }

    u32 get8(u32 at) const { return memory_[at]; }
    u32 get16(u32 at) const { return (get8(at) << 8) | get8(at + 1); }
    u32 get32(u32 at) const { return (get16(at) << 16) | get16(at + 2); }

    u32 str(u32 at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            put8(at + static_cast<u32>(i), static_cast<u8>(s[i]));
        }
        put8(at + static_cast<u32>(s.size()), 0);
        return at;
    }

    std::string read_str(u32 at) const {
        std::string s;
        while (memory_[at] != 0 && s.size() < 4096) {
            s += static_cast<char>(memory_[at++]);
        }
        return s;
    }

    // JSR -lvo(A6) with A6 = the library base, from hand-written 68000 code.
    ExitReason call(u32 base, u32 lvo) {
        put16(kCall, 0x4EAE);
        put16(kCall + 2, disp(lvo));
        put16(kCall + 4, 0x4EF8);  // JMP $400.w
        put16(kCall + 6, 0x0400);
        g_jit.flush();
        m.cpu.a[6] = base;
        m.cpu.a[7] = kStack;
        m.cpu.pc = kCall;
        return g_jit.run(1000000);
    }

    u32 result(u32 base, u32 lvo) {
        CHECK_EQ(call(base, lvo), ExitReason::Halt);
        return m.cpu.d[0];
    }

    u32 icon_call(u32 lvo, u32 a0 = 0, u32 a1 = 0, u32 d0 = 0) {
        m.cpu.a[0] = a0;
        m.cpu.a[1] = a1;
        m.cpu.d[0] = d0;
        return result(g_icon.base(), lvo);
    }

    u32 avail_mem() {
        m.cpu.a[0] = 0;
        m.cpu.d[1] = 0;
        return result(g_exec.exec_base(), exec_lvo::AvailMem);
    }

    u32 io_err() { return g_dos.last_error(); }

    u32 dos_call(u32 lvo, u32 d1, u32 d2 = 0) {
        m.cpu.d[1] = d1;
        m.cpu.d[2] = d2;
        return result(g_dos.dos_base(), lvo);
    }

private:
    static constexpr usize kRegion = 1 << 20;

    std::vector<u8> memory_;
    std::vector<u8> image_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

u32 get_disk_object(Rig& r, const std::string& name) {
    return r.icon_call(icon_lvo::GetDiskObject, r.str(kData, name));
}

u32 get_disk_object_new(Rig& r, const std::string& name) {
    return r.icon_call(icon_lvo::GetDiskObjectNew, r.str(kData, name));
}

u32 free_disk_object(Rig& r, u32 object) {
    r.m.cpu.a[0] = object;
    r.m.cpu.d[0] = 0;
    CHECK_EQ(r.call(g_icon.base(), icon_lvo::FreeDiskObject), ExitReason::Halt);
    return r.m.cpu.d[0];
}

// Writes the bytes as RAM:<name>.info.
void ram_icon(const std::string& name, const std::vector<u8>& bytes) {
    const u8 none = 0;
    u32 error = 0;
    CHECK(g_dos.write_file(("RAM:" + name + ".info").c_str(),
                           bytes.empty() ? &none : bytes.data(), static_cast<u32>(bytes.size()),
                           error));
}

std::vector<u8> ram_file(const std::string& path) {
    std::vector<u8> bytes(Icon::kMaxInfoBytes);
    u32 got = 0;
    u32 error = 0;
    CHECK(g_dos.read_file(path.c_str(), bytes.data(), static_cast<u32>(bytes.size()), got, error));
    bytes.resize(got);
    return bytes;
}

void tool_icon_has_every_field() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    CHECK_EQ(r.get16(obj + do_Magic), 0xE310u);
    CHECK_EQ(r.get16(obj + do_Version), 1u);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_LeftEdge), 5u);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_TopEdge), 6u);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_Width), 16u);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_Height), 9u);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_Flags), 0u);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_Activation), 1u);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_GadgetType), 1u);
    CHECK_EQ(r.get32(obj + do_Gadget + gg_GadgetText), 0u);
    CHECK_EQ(r.get32(obj + do_Gadget + gg_MutualExclude), 0u);
    CHECK_EQ(r.get32(obj + do_Gadget + gg_SpecialInfo), 0u);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_GadgetID), 0u);
    CHECK_EQ(r.get32(obj + do_Gadget + gg_UserData), 0u);

    CHECK_EQ(r.get8(obj + do_Type), kWBTool);
    CHECK_EQ(r.get8(obj + do_Type + 1), 0u);
    CHECK_EQ(r.get32(obj + do_CurrentX), 0x00010002u);
    CHECK_EQ(r.get32(obj + do_CurrentY), 0xFFFFFFFEu);
    CHECK_EQ(r.get32(obj + do_DrawerData), 0u);
    CHECK_EQ(r.get32(obj + do_ToolWindow), 0u);
    CHECK_EQ(r.get32(obj + do_StackSize), 8192u);

    const u32 default_tool = r.get32(obj + do_DefaultTool);
    CHECK(default_tool != 0);
    CHECK(r.read_str(default_tool) == "C:Editor");

    const u32 tool_types = r.get32(obj + do_ToolTypes);
    CHECK(tool_types != 0);
    const u32 font = r.get32(tool_types);
    const u32 pubscreen = r.get32(tool_types + 4);
    const u32 stack = r.get32(tool_types + 8);
    CHECK(font != 0 && pubscreen != 0 && stack != 0);
    CHECK_EQ(r.get32(tool_types + 12), 0u);
    CHECK(r.read_str(font) == "FONT=topaz");
    CHECK(r.read_str(pubscreen) == "PUBSCREEN=Workbench");
    CHECK(r.read_str(stack) == "STACK");

    const u32 render = r.get32(obj + do_Gadget + gg_GadgetRender);
    CHECK(render != 0);
    CHECK_EQ(r.get32(obj + do_Gadget + gg_SelectRender), 0u);
    CHECK_EQ(r.get16(render + img_LeftEdge), 1u);
    CHECK_EQ(r.get16(render + img_TopEdge), 2u);
    CHECK_EQ(r.get16(render + img_Width), 16u);
    CHECK_EQ(r.get16(render + img_Height), 9u);
    CHECK_EQ(r.get16(render + img_Depth), 1u);
    CHECK_EQ(r.get8(render + img_PlanePick), 1u);
    CHECK_EQ(r.get8(render + img_PlaneOnOff), 0u);
    CHECK_EQ(r.get32(render + img_NextImage), 0u);
    const u32 data = r.get32(render + img_ImageData);
    CHECK(data != 0);
    CHECK_EQ(r.get8(data), 0xA5u);
    CHECK_EQ(r.get8(data + 1), 3u);
    CHECK_EQ(r.get8(data + 17), static_cast<u32>(17 * 3));

    CHECK_EQ(free_disk_object(r, obj), 0u);
}

void drawer_icon_carries_the_drawer_data_and_extension() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Drawer");
    CHECK(obj != 0);
    CHECK_EQ(r.get8(obj + do_Type), kWBDrawer);
    CHECK_EQ(r.get32(obj + do_Gadget + gg_UserData), 1u);
    const u32 drawer = r.get32(obj + do_DrawerData);
    CHECK(drawer != 0);
    CHECK_EQ(r.get16(drawer + nw_LeftEdge), 10u);
    CHECK_EQ(r.get16(drawer + nw_TopEdge), 20u);
    CHECK_EQ(r.get16(drawer + nw_Width), 300u);
    CHECK_EQ(r.get16(drawer + nw_Height), 120u);
    CHECK_EQ(r.get8(drawer + nw_DetailPen), 1u);
    CHECK_EQ(r.get8(drawer + nw_BlockPen), 2u);
    CHECK_EQ(r.get32(drawer + nw_IDCMPFlags), 0x1234u);
    CHECK_EQ(r.get32(drawer + nw_Flags), 0x5678u);
    CHECK_EQ(r.get32(drawer + nw_FirstGadget), 0u);
    CHECK_EQ(r.get16(drawer + nw_MinWidth), 100u);
    CHECK_EQ(r.get16(drawer + nw_MinHeight), 50u);
    CHECK_EQ(r.get16(drawer + nw_MaxWidth), 400u);
    CHECK_EQ(r.get16(drawer + nw_MaxHeight), 200u);
    CHECK_EQ(r.get16(drawer + nw_Type), 1u);
    CHECK_EQ(r.get32(drawer + dd_CurrentX), 0x11223344u);
    CHECK_EQ(r.get32(drawer + dd_CurrentY), 0x55667788u);
    CHECK_EQ(r.get32(drawer + dd_Flags), 0x0000ABCDu);
    CHECK_EQ(r.get16(drawer + dd_ViewModes), 0x0F0Fu);

    const u32 render = r.get32(obj + do_Gadget + gg_GadgetRender);
    CHECK(render != 0);
    CHECK_EQ(r.get16(render + img_Width), 8u);
    CHECK_EQ(r.get16(render + img_Height), 4u);
    CHECK_EQ(r.get16(render + img_Depth), 2u);
    CHECK_EQ(r.get8(render + img_PlanePick), 3u);
    const u32 data = r.get32(render + img_ImageData);
    CHECK(data != 0);
    CHECK_EQ(r.get8(data), 0x80u);
    CHECK_EQ(r.get8(data + 8), 0x88u);
    CHECK_EQ(r.get8(data + 15), 0x8Fu);
    CHECK_EQ(free_disk_object(r, obj), 0u);
}

void project_icon_has_both_images() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Project");
    CHECK(obj != 0);
    CHECK_EQ(r.get8(obj + do_Type), kWBProject);
    CHECK_EQ(r.get16(obj + do_Gadget + gg_Flags), 2u);
    const u32 render = r.get32(obj + do_Gadget + gg_GadgetRender);
    const u32 select = r.get32(obj + do_Gadget + gg_SelectRender);
    CHECK(render != 0);
    CHECK(select != 0);
    CHECK(render != select);
    for (u32 image : {render, select}) {
        CHECK_EQ(r.get16(image + img_Width), 12u);
        CHECK_EQ(r.get16(image + img_Height), 6u);
        CHECK_EQ(r.get16(image + img_Depth), 1u);
        CHECK_EQ(r.get8(image + img_PlanePick), 1u);
        CHECK_EQ(r.get32(image + img_NextImage), 0u);
    }
    const u32 render_data = r.get32(render + img_ImageData);
    const u32 select_data = r.get32(select + img_ImageData);
    CHECK(render_data != 0 && select_data != 0);
    CHECK_EQ(r.get8(render_data), 0x10u);
    CHECK_EQ(r.get8(render_data + 11), 0x1Bu);
    CHECK_EQ(r.get8(select_data), 0xF0u);
    CHECK_EQ(r.get8(select_data + 11), 0xE5u);
    CHECK_EQ(free_disk_object(r, obj), 0u);
}

void malformed_and_missing_icons_fail() {
    Rig r;
    const u32 magic = get_disk_object(r, "Tools/BadMagic");
    CHECK_EQ(magic, 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    const u32 truncated = get_disk_object(r, "Tools/Truncated");
    CHECK_EQ(truncated, 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    const u32 missing = get_disk_object(r, "Tools/Absent");
    CHECK_EQ(missing, 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
}

void free_disk_object_returns_the_memory() {
    Rig r;
    const u32 before = r.avail_mem();
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    CHECK(r.avail_mem() < before);
    CHECK_EQ(free_disk_object(r, obj), 0u);
    CHECK_EQ(r.avail_mem(), before);

    // A second free of the same object frees nothing more.
    CHECK(!logged_containing("FreeDiskObject"));
    CHECK_EQ(free_disk_object(r, obj), 0u);
    CHECK(logged_containing("FreeDiskObject"));
    CHECK_EQ(r.avail_mem(), before);

    // Memory this library did not hand out is logged and left alone.
    g_lines.clear();
    CHECK_EQ(free_disk_object(r, kData + 0x100), 0u);
    CHECK(logged_containing("FreeDiskObject"));
    CHECK_EQ(free_disk_object(r, 0), 0u);
}

void null_name_gives_an_empty_disk_object() {
    Rig r;
    const u32 before = r.avail_mem();
    for (u32 lvo : {icon_lvo::GetDiskObject, icon_lvo::GetDiskObjectNew}) {
        const u32 obj = r.icon_call(lvo, 0);
        CHECK(obj != 0);
        CHECK_EQ(r.get16(obj + do_Magic), 0xE310u);
        CHECK_EQ(r.get16(obj + do_Version), 1u);
        for (u32 at = do_Gadget; at < do_StackSize + 4; ++at) {
            CHECK_EQ(r.get8(obj + at), 0u);
        }
        CHECK(r.avail_mem() < before);
        CHECK_EQ(free_disk_object(r, obj), 0u);
        CHECK_EQ(r.avail_mem(), before);
    }
}

void disk_object_table_fills_without_leaking() {
    Rig r;
    std::vector<u32> objects;
    for (;;) {
        const u32 obj = r.icon_call(icon_lvo::GetDefDiskObject, 0, 0, kWBTool);
        if (obj == 0) {
            break;
        }
        objects.push_back(obj);
        CHECK(objects.size() < 100000);
    }
    CHECK(objects.size() >= 256);
    CHECK_EQ(r.io_err(), ERROR_NO_FREE_STORE);
    const u32 full = r.avail_mem();
    CHECK_EQ(get_disk_object(r, "Tools/Tool"), 0u);
    CHECK_EQ(r.io_err(), ERROR_NO_FREE_STORE);
    CHECK_EQ(r.avail_mem(), full);

    CHECK_EQ(free_disk_object(r, objects.back()), 0u);
    objects.pop_back();
    const u32 again = get_disk_object(r, "Tools/Tool");
    CHECK(again != 0);
    objects.push_back(again);
    for (u32 obj : objects) {
        CHECK_EQ(free_disk_object(r, obj), 0u);
    }
    CHECK(!logged_containing("FreeDiskObject"));
}

void disk_revision_is_the_low_byte_of_user_data() {
    Rig r;
    const u32 high = get_disk_object(r, "Tools/HighRev");
    CHECK(high != 0);
    CHECK_EQ(r.get32(high + do_Gadget + gg_UserData), 0x12345601u);
    const u32 high_drawer = r.get32(high + do_DrawerData);
    CHECK(high_drawer != 0);
    CHECK_EQ(r.get32(high_drawer + dd_Flags), 0x0000ABCDu);
    CHECK_EQ(r.get16(high_drawer + dd_ViewModes), 0x0F0Fu);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:HighRev"), high), 1u);
    CHECK(ram_file("RAM:HighRev.info") == drawer_info(0x12345601));
    CHECK_EQ(free_disk_object(r, high), 0u);

    // An icon of revision 0 has no dd_Flags and dd_ViewModes on disk.
    const u32 old = get_disk_object(r, "Tools/OldRev");
    CHECK(old != 0);
    const u32 old_drawer = r.get32(old + do_DrawerData);
    CHECK(old_drawer != 0);
    CHECK_EQ(r.get32(old_drawer + dd_CurrentY), 0x55667788u);
    CHECK_EQ(r.get32(old_drawer + dd_Flags), 0u);
    CHECK_EQ(r.get16(old_drawer + dd_ViewModes), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:OldRev"), old), 1u);
    CHECK(ram_file("RAM:OldRev.info") == drawer_info(0));
    CHECK_EQ(free_disk_object(r, old), 0u);
}

void save_object_writes_a_host_path_and_refuses_the_disk() {
    Rig r;
    const u32 object = g_icon.load_object("Tools/HighRev");
    CHECK(object != 0);
    r.put32(object + do_CurrentX, 123);
    r.put32(object + do_CurrentY, 45);
    u32 error = 0;
    CHECK(g_icon.save_object("RAM:Saved", object, error));
    CHECK_EQ(error, 0u);
    const u32 saved = g_icon.load_object("RAM:Saved");
    CHECK(saved != 0);
    CHECK_EQ(r.get32(saved + do_CurrentX), 123u);
    CHECK_EQ(r.get32(saved + do_CurrentY), 45u);
    g_icon.free_object(saved);

    CHECK(!g_icon.save_object("TestDisk:Tools/HighRev", object, error));
    CHECK_EQ(error, ERROR_DISK_WRITE_PROTECTED);
    CHECK(!g_icon.save_object("RAM:Nothing", 0, error));
    CHECK_EQ(error, ERROR_REQUIRED_ARG_MISSING);
    g_icon.free_object(object);
}

void many_tool_types_load_and_are_found() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Many");
    CHECK(obj != 0);
    CHECK(r.read_str(r.get32(obj + do_DefaultTool)) == "SYS:Tool");
    CHECK(r.read_str(r.get32(obj + do_ToolWindow)) == "CON:0/0/100/50/Window");
    const u32 array = r.get32(obj + do_ToolTypes);
    CHECK(array != 0);
    CHECK_EQ(array % 4, 0u);
    for (u32 i = 0; i < 300; ++i) {
        const u32 entry = r.get32(array + i * 4);
        CHECK(entry != 0);
        CHECK(r.read_str(entry) == "T" + std::to_string(i) + "=" + std::to_string(i));
    }
    CHECK_EQ(r.get32(array + 300 * 4), 0u);

    const u32 last = r.icon_call(icon_lvo::FindToolType, array, r.str(kData + 0x100, "t299"));
    CHECK(last != 0);
    CHECK(r.read_str(last) == "299");
    const u32 two = r.icon_call(icon_lvo::FindToolType, array, r.str(kData + 0x100, "T2"));
    CHECK(two != 0);
    CHECK(r.read_str(two) == "2");
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, r.str(kData + 0x100, "T300")), 0u);
    CHECK_EQ(free_disk_object(r, obj), 0u);
}

void find_tool_type_skips_entries_it_does_not_match() {
    Rig r;
    const u32 array = kData + 0x400;
    const u32 long_entry = r.str(kData + 0x1000, std::string(600, 'x'));
    const u32 key = r.str(kData + 0x1400, "KEY=value");
    const u32 keys = r.str(kData + 0x1500, "KEYS");
    const u32 empty = r.str(kData + 0x1600, "EMPTY=");
    r.put32(array, long_entry);
    r.put32(array + 4, key);
    r.put32(array + 8, keys);
    r.put32(array + 12, empty);
    r.put32(array + 16, 0);
    const u32 name = kData + 0x100;
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, r.str(name, "key")), key + 4);
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, r.str(name, "KEYS")), keys + 4);
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, r.str(name, "empty")), empty + 6);
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, r.str(name, "KE")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, r.str(name, "KEY=value")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, r.str(name, "value")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, 0), 0u);
}

void optional_parts_follow_the_gadget_flags() {
    Rig r;
    const u32 plain = get_disk_object(r, "Tools/NoHighlight");
    CHECK(plain != 0);
    CHECK(r.get32(plain + do_Gadget + gg_GadgetRender) != 0);
    CHECK_EQ(r.get32(plain + do_Gadget + gg_SelectRender), 0u);
    CHECK(r.read_str(r.get32(plain + do_DefaultTool)) == "Tool");
    CHECK_EQ(free_disk_object(r, plain), 0u);

    const u32 solid = get_disk_object(r, "Tools/NoData");
    CHECK(solid != 0);
    const u32 render = r.get32(solid + do_Gadget + gg_GadgetRender);
    CHECK(render != 0);
    CHECK_EQ(r.get16(render + img_Width), 16u);
    CHECK_EQ(r.get16(render + img_Height), 2u);
    CHECK_EQ(r.get16(render + img_Depth), 0u);
    CHECK_EQ(r.get32(render + img_ImageData), 0u);
    CHECK_EQ(r.get8(render + img_PlaneOnOff), 2u);
    CHECK(r.read_str(r.get32(solid + do_DefaultTool)) == "Tool");
    CHECK_EQ(free_disk_object(r, solid), 0u);
}

// No icon comes of the bytes, IoErr says they are no icon, and no memory
// stays allocated.
void expect_no_icon(Rig& r, const std::vector<u8>& bytes) {
    ram_icon("Bad", bytes);
    const u32 before = r.avail_mem();
    CHECK_EQ(get_disk_object(r, "RAM:Bad"), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    CHECK_EQ(r.avail_mem(), before);
}

void truncated_icons_fail_at_every_length() {
    Rig r;
    for (const std::vector<u8>& whole :
         {tool_info(), drawer_info(), project_info(), many_types_info(3)}) {
        for (usize length = 0; length < whole.size(); ++length) {
            expect_no_icon(r, std::vector<u8>(whole.begin(), whole.begin() + length));
        }
        ram_icon("Whole", whole);
        const u32 obj = get_disk_object(r, "RAM:Whole");
        CHECK(obj != 0);
        CHECK_EQ(free_disk_object(r, obj), 0u);
    }
}

void hostile_sizes_fail() {
    Rig r;
    {
        // More plane data than any file holds.
        Info f;
        put_header(f, 0, 1, 0, 0, 0, 0);
        put_image(f, 0, 0, 0x7FFF, 0x7FFF, 8, true, kSmallPlane, 1, 0);
        expect_no_icon(r, f.bytes);
    }
    {
        Info f;
        put_header(f, 0, 1, 0, 0, 0, 0);
        put_image(f, 0, 0, 0xFFF0, 2, 1, true, kSmallPlane, 1, 0);  // Width -16
        expect_no_icon(r, f.bytes);
    }
    {
        Info f;
        put_header(f, 0, 1, 0, 0, 0, 0);
        put_image(f, 0, 0, 16, 0xFFFF, 1, true, kSmallPlane, 1, 0);  // Height -1
        expect_no_icon(r, f.bytes);
    }
    {
        Info f;
        put_header(f, 0, 1, 0, 0, 0, 0);
        put_image(f, 0, 0, 16, 2, 9, true, std::vector<u8>(36), 1, 0);
        expect_no_icon(r, f.bytes);
    }
    for (u32 length : {0u, 5u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        // A default tool with no bytes, or more than the file has.
        Info f;
        put_header(f, 0, 1, 0, 1, 0, 0);
        put_image(f, 0, 0, 16, 2, 1, true, kSmallPlane, 1, 0);
        f.u32v(length);
        f.blob({'a', 'b', 'c', 0});
        expect_no_icon(r, f.bytes);
    }
    {
        // A string the file does not terminate.
        Info f;
        put_header(f, 0, 1, 0, 1, 0, 0);
        put_image(f, 0, 0, 16, 2, 1, true, kSmallPlane, 1, 0);
        f.u32v(4);
        f.blob({'a', 'b', 'c', 'd'});
        expect_no_icon(r, f.bytes);
    }
    for (u32 table : {0u, 2u, 6u, 12u, 0x40000000u, 0xFFFFFFFCu}) {
        // A tool type table of no entries at all, of a size that is no
        // count, or of more entries than strings follow.
        Info f;
        put_header(f, 0, 1, 0, 0, 1, 0);
        put_image(f, 0, 0, 16, 2, 1, true, kSmallPlane, 1, 0);
        f.u32v(table);
        f.string("ONE=1");
        expect_no_icon(r, f.bytes);
    }
}

void put_disk_object_writes_the_bytes_it_read() {
    Rig r;
    const std::pair<const char*, std::vector<u8>> icons[] = {
        {"Tools/Tool", tool_info()},
        {"Tools/Drawer", drawer_info()},
        {"Tools/Project", project_info()},
        {"Tools/Many", many_types_info(300)},
        {"Tools/NoData", image_without_data_info()},
    };
    for (const auto& [name, bytes] : icons) {
        const u32 obj = get_disk_object(r, name);
        CHECK(obj != 0);
        CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:Out"), obj), 1u);
        CHECK_EQ(free_disk_object(r, obj), 0u);
        CHECK(ram_file("RAM:Out.info") == bytes);
    }
}

void put_disk_object_writes_every_tool_type() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    // A tool type array the program built itself, longer than the icon's.
    const u32 array = kData + 0x2000;
    const u32 strings = kData + 0x3000;
    const u32 count = 300;
    for (u32 i = 0; i < count; ++i) {
        r.put32(array + i * 4, r.str(strings + i * 8, "A" + std::to_string(i)));
    }
    r.put32(array + count * 4, 0);
    r.put32(obj + do_ToolTypes, array);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:Long"), obj), 1u);
    CHECK_EQ(free_disk_object(r, obj), 0u);

    // The table follows the DiskObject, the Image, its 18 bytes of plane
    // data and the default tool "C:Editor".
    const std::vector<u8> file = ram_file("RAM:Long.info");
    const usize table = 78 + 20 + 18 + 4 + 9;
    CHECK(file.size() > table + 4);
    const u32 table_bytes = (u32{file[table]} << 24) | (u32{file[table + 1]} << 16) |
                            (u32{file[table + 2]} << 8) | file[table + 3];
    CHECK_EQ(table_bytes, (count + 1) * 4);

    const u32 copy = get_disk_object(r, "RAM:Long");
    CHECK(copy != 0);
    const u32 types = r.get32(copy + do_ToolTypes);
    CHECK(types != 0);
    CHECK(r.read_str(r.get32(types)) == "A0");
    CHECK(r.read_str(r.get32(types + (count - 1) * 4)) == "A299");
    CHECK_EQ(r.get32(types + count * 4), 0u);
    CHECK_EQ(free_disk_object(r, copy), 0u);

    // An array without an end that a file could hold is refused.
    const u32 big = get_disk_object(r, "Tools/Tool");
    CHECK(big != 0);
    const u32 endless = 0x500000;  // above the memory exec hands out
    const u32 word = r.str(endless - 16, "X");
    for (u32 i = 0; i < 20000; ++i) {
        r.put32(endless + i * 4, word);
    }
    r.put32(big + do_ToolTypes, endless);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:Endless"), big), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_TOO_LARGE);
    CHECK_EQ(g_dos.examine("RAM:Endless.info").kind, Dos::TargetKind::Missing);
    CHECK_EQ(free_disk_object(r, big), 0u);
}

void put_disk_object_respects_write_protection() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:Kept"), obj), 1u);
    CHECK(r.dos_call(dos_lvo::SetProtection, r.str(kData + 0x100, "RAM:Kept.info"), 4) != 0);
    r.put32(obj + do_StackSize, 99999);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:Kept"), obj), 0u);
    CHECK_EQ(r.io_err(), ERROR_WRITE_PROTECTED);
    CHECK(ram_file("RAM:Kept.info") == tool_info());
    CHECK_EQ(free_disk_object(r, obj), 0u);
}

void put_disk_object_refuses_what_it_cannot_write() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:None"), 0), 0u);
    CHECK_EQ(r.io_err(), ERROR_REQUIRED_ARG_MISSING);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:NoDir/Icon"), obj), 0u);
    CHECK_EQ(r.io_err(), ERROR_DIR_NOT_FOUND);

    // An image larger than an icon file may be.
    const u32 render = r.get32(obj + do_Gadget + gg_GadgetRender);
    r.put16(render + img_Width, 4000);
    r.put16(render + img_Height, 4000);
    r.put32(render + img_ImageData, 0x100000);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:Huge"), obj), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_TOO_LARGE);
    CHECK_EQ(g_dos.examine("RAM:Huge.info").kind, Dos::TargetKind::Missing);
    CHECK_EQ(free_disk_object(r, obj), 0u);
}

void tool_type_calls_match_case_blind() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    const u32 array = r.get32(obj + do_ToolTypes);
    CHECK(array != 0);

    const u32 value = r.icon_call(icon_lvo::FindToolType, array, r.str(kData + 0x100, "font"));
    CHECK(value != 0);
    CHECK(r.read_str(value) == "topaz");
    const u32 alone = r.icon_call(icon_lvo::FindToolType, array, r.str(kData + 0x100, "STACK"));
    CHECK(alone != 0);
    CHECK(r.read_str(alone) == "");
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, array, r.str(kData + 0x100, "ABSENT")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::FindToolType, 0, r.str(kData + 0x100, "FONT")), 0u);
    CHECK_EQ(free_disk_object(r, obj), 0u);

    const u32 types = r.str(kData + 0x200, "one|Two|three");
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, types, r.str(kData + 0x300, "two")), 1u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, types, r.str(kData + 0x300, "THREE")), 1u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, types, r.str(kData + 0x300, "four")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, types, r.str(kData + 0x300, "tw")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, types, r.str(kData + 0x300, "twoo")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, types, r.str(kData + 0x300, "one|Two")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, types, r.str(kData + 0x300, "")), 0u);
    const u32 single = r.str(kData + 0x200, "text");
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, single, r.str(kData + 0x300, "TEXT")), 1u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, single, r.str(kData + 0x300, "data")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, 0, r.str(kData + 0x300, "text")), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::MatchToolValue, single, 0), 0u);
}

void bump_revision_counts_copies_and_truncates() {
    Rig r;
    const u32 result = kData + 0x100;
    const u32 old = kData + 0x200;
    // The names of the BumpRevision autodoc.
    const std::pair<const char*, const char*> names[] = {
        {"foo", "copy_of_foo"},
        {"copy_of_foo", "copy_2_of_foo"},
        {"copy_2_of_foo", "copy_3_of_foo"},
        {"copy_199_of_foo", "copy_200_of_foo"},
        {"copy foo", "copy_of_copy foo"},
        {"copy_0_of_foo", "copy_1_of_foo"},
        {"012345678901234567890123456789", "copy_of_0123456789012345678901"},
        // And names that only look like copies.
        {"copy of foo", "copy_of_copy of foo"},
        {"copy_of", "copy_of_copy_of"},
        {"copy_x_of_foo", "copy_of_copy_x_of_foo"},
        {"copy_9_foo", "copy_of_copy_9_foo"},
        {"copy_99999_of_a_name_that_is_long", "copy_100000_of_a_name_that_is_"},
        {"", "copy_of_"},
    };
    for (const auto& [from, to] : names) {
        r.str(result, "the buffer holds something else before the call");
        CHECK_EQ(r.icon_call(icon_lvo::BumpRevision, result, r.str(old, from)), result);
        CHECK(r.read_str(result) == to);
        CHECK(r.read_str(result).size() <= 30);
    }
    // The new name may be built over the old one.
    CHECK_EQ(r.icon_call(icon_lvo::BumpRevision, old, r.str(old, "copy_of_foo")), old);
    CHECK(r.read_str(old) == "copy_2_of_foo");
}

void default_icons_cover_the_types() {
    Rig r;
    const struct {
        u32 type;
        u32 width;
        u32 height;
        bool drawer;
    } expected[] = {
        {kWBDisk, 35, 18, true},   {kWBDrawer, 54, 23, true}, {kWBTool, 40, 23, false},
        {kWBProject, 40, 23, false}, {kWBGarbage, 54, 23, true}, {kWBKick, 35, 18, false},
    };
    for (const auto& want : expected) {
        const u32 obj = r.icon_call(icon_lvo::GetDefDiskObject, 0, 0, want.type);
        CHECK(obj != 0);
        CHECK_EQ(r.get8(obj + do_Type), want.type);
        // The gadget renders an Image, and the icon is of the revision
        // that keeps dd_Flags and dd_ViewModes.
        CHECK_EQ(r.get16(obj + do_Gadget + gg_Flags), 4u);
        CHECK_EQ(r.get32(obj + do_Gadget + gg_UserData), 1u);
        CHECK_EQ(r.get16(obj + do_Gadget + gg_Width), want.width);
        CHECK_EQ(r.get16(obj + do_Gadget + gg_Height), want.height);
        CHECK_EQ(r.get32(obj + do_CurrentX), 0x80000000u);
        CHECK_EQ(r.get32(obj + do_CurrentY), 0x80000000u);
        CHECK_EQ(r.get32(obj + do_StackSize) != 0, true);
        const u32 render = r.get32(obj + do_Gadget + gg_GadgetRender);
        CHECK(render != 0);
        CHECK_EQ(r.get16(render + img_Width), want.width);
        CHECK_EQ(r.get16(render + img_Height), want.height);
        CHECK_EQ(r.get16(render + img_Depth), 2u);
        CHECK_EQ(r.get8(render + img_PlanePick), 3u);
        CHECK(r.get32(render + img_ImageData) != 0);
        CHECK_EQ(r.get32(obj + do_DrawerData) != 0, want.drawer);
        CHECK_EQ(free_disk_object(r, obj), 0u);
    }
    for (u32 type : {0u, 6u, 8u, 99u}) {  // WBDEVICE and WBAPPICON have no default
        CHECK_EQ(r.icon_call(icon_lvo::GetDefDiskObject, 0, 0, type), 0u);
        CHECK_EQ(r.io_err(), ERROR_OBJECT_WRONG_TYPE);
    }

    const u32 tool = r.icon_call(icon_lvo::GetDefDiskObject, 0, 0, kWBTool);
    CHECK(tool != 0);
    CHECK_EQ(r.icon_call(icon_lvo::PutDefDiskObject, tool), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    CHECK_EQ(free_disk_object(r, tool), 0u);
}

void get_disk_object_new_falls_back_to_defaults() {
    Rig r;
    const u32 file = get_disk_object_new(r, "Tools/NoIcon");
    CHECK(file != 0);
    CHECK_EQ(r.get8(file + do_Type), kWBProject);
    CHECK(r.get32(file + do_Gadget + gg_GadgetRender) != 0);
    CHECK_EQ(free_disk_object(r, file), 0u);

    const u32 dir = get_disk_object_new(r, "Tools/Sub");
    CHECK(dir != 0);
    CHECK_EQ(r.get8(dir + do_Type), kWBDrawer);
    CHECK(r.get32(dir + do_DrawerData) != 0);
    CHECK_EQ(free_disk_object(r, dir), 0u);

    const u32 volume = get_disk_object_new(r, "TestDisk:");
    CHECK(volume != 0);
    CHECK_EQ(r.get8(volume + do_Type), kWBDisk);
    CHECK(r.get32(volume + do_DrawerData) != 0);
    CHECK_EQ(free_disk_object(r, volume), 0u);

    const u32 missing = get_disk_object_new(r, "Tools/Nope");
    CHECK(missing != 0);
    CHECK_EQ(r.get8(missing + do_Type), kWBProject);
    CHECK_EQ(free_disk_object(r, missing), 0u);

    // An icon that is there but does not load is replaced by a default too.
    CHECK_EQ(get_disk_object(r, "Tools/Truncated"), 0u);
    const u32 malformed = get_disk_object_new(r, "Tools/Truncated");
    CHECK(malformed != 0);
    CHECK_EQ(r.get8(malformed + do_Type), kWBProject);
    CHECK_EQ(free_disk_object(r, malformed), 0u);

    // A real icon wins over the default.
    const u32 real = get_disk_object_new(r, "Tools/Drawer");
    CHECK(real != 0);
    CHECK_EQ(r.get16(real + do_Gadget + gg_Width), 8u);
    CHECK_EQ(free_disk_object(r, real), 0u);

    // A name too long for a path has no object to take a type from.
    CHECK_EQ(get_disk_object_new(r, std::string(300, 'n')), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_COMPONENT_NAME);
}

void put_disk_object_round_trips_on_ram() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:Copy"), obj), 1u);
    CHECK_EQ(g_dos.examine("RAM:Copy.info").kind, Dos::TargetKind::File);

    const u32 copy = get_disk_object(r, "RAM:Copy");
    CHECK(copy != 0);
    CHECK(copy != obj);
    CHECK_EQ(r.get16(copy + do_Magic), 0xE310u);
    CHECK_EQ(r.get16(copy + do_Version), 1u);
    CHECK_EQ(r.get8(copy + do_Type), kWBTool);
    CHECK_EQ(r.get16(copy + do_Gadget + gg_LeftEdge), 5u);
    CHECK_EQ(r.get16(copy + do_Gadget + gg_TopEdge), 6u);
    CHECK_EQ(r.get16(copy + do_Gadget + gg_Width), 16u);
    CHECK_EQ(r.get16(copy + do_Gadget + gg_Height), 9u);
    CHECK_EQ(r.get32(copy + do_CurrentX), 0x00010002u);
    CHECK_EQ(r.get32(copy + do_CurrentY), 0xFFFFFFFEu);
    CHECK_EQ(r.get32(copy + do_StackSize), 8192u);
    CHECK(r.read_str(r.get32(copy + do_DefaultTool)) == "C:Editor");
    const u32 types = r.get32(copy + do_ToolTypes);
    CHECK(types != 0);
    CHECK(r.read_str(r.get32(types)) == "FONT=topaz");
    CHECK(r.read_str(r.get32(types + 4)) == "PUBSCREEN=Workbench");
    CHECK(r.read_str(r.get32(types + 8)) == "STACK");
    CHECK_EQ(r.get32(types + 12), 0u);
    const u32 a = r.get32(obj + do_Gadget + gg_GadgetRender);
    const u32 b = r.get32(copy + do_Gadget + gg_GadgetRender);
    CHECK_EQ(r.get16(b + img_Width), r.get16(a + img_Width));
    CHECK_EQ(r.get16(b + img_Height), r.get16(a + img_Height));
    CHECK_EQ(r.get16(b + img_Depth), r.get16(a + img_Depth));
    CHECK_EQ(r.get8(b + img_PlanePick), r.get8(a + img_PlanePick));
    CHECK_EQ(r.get8(b + img_PlaneOnOff), r.get8(a + img_PlaneOnOff));
    const u32 ad = r.get32(a + img_ImageData);
    const u32 bd = r.get32(b + img_ImageData);
    for (u32 i = 0; i < 18; ++i) {
        CHECK_EQ(r.get8(bd + i), r.get8(ad + i));
    }
    CHECK_EQ(free_disk_object(r, copy), 0u);
    CHECK_EQ(free_disk_object(r, obj), 0u);
}

void put_disk_object_writes_only_ram() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "Tools/Written"), obj), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    CHECK_EQ(free_disk_object(r, obj), 0u);
}

void delete_disk_object_removes_the_icon() {
    Rig r;
    const u32 obj = get_disk_object(r, "Tools/Tool");
    CHECK(obj != 0);
    CHECK_EQ(r.icon_call(icon_lvo::PutDiskObject, r.str(kData, "RAM:Gone"), obj), 1u);
    CHECK_EQ(free_disk_object(r, obj), 0u);
    const u32 again = get_disk_object(r, "RAM:Gone");
    CHECK(again != 0);
    CHECK_EQ(free_disk_object(r, again), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::DeleteDiskObject, r.str(kData, "RAM:Gone")), 1u);
    CHECK_EQ(get_disk_object(r, "RAM:Gone"), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);
    CHECK_EQ(r.icon_call(icon_lvo::DeleteDiskObject, r.str(kData, "RAM:Gone")), 0u);
    CHECK_EQ(r.io_err(), ERROR_OBJECT_NOT_FOUND);

    // An icon on the read-only volume stays.
    CHECK_EQ(r.icon_call(icon_lvo::DeleteDiskObject, r.str(kData, "Tools/Tool")), 0u);
    CHECK_EQ(r.io_err(), ERROR_DISK_WRITE_PROTECTED);
    const u32 kept = get_disk_object(r, "Tools/Tool");
    CHECK(kept != 0);
    CHECK_EQ(free_disk_object(r, kept), 0u);
    CHECK_EQ(r.icon_call(icon_lvo::DeleteDiskObject, 0), 0u);
    CHECK_EQ(r.io_err(), ERROR_INVALID_COMPONENT_NAME);
}

}

int main() {
    tool_icon_has_every_field();
    drawer_icon_carries_the_drawer_data_and_extension();
    project_icon_has_both_images();
    malformed_and_missing_icons_fail();
    free_disk_object_returns_the_memory();
    null_name_gives_an_empty_disk_object();
    disk_object_table_fills_without_leaking();
    disk_revision_is_the_low_byte_of_user_data();
    save_object_writes_a_host_path_and_refuses_the_disk();
    many_tool_types_load_and_are_found();
    find_tool_type_skips_entries_it_does_not_match();
    optional_parts_follow_the_gadget_flags();
    truncated_icons_fail_at_every_length();
    hostile_sizes_fail();
    put_disk_object_writes_the_bytes_it_read();
    put_disk_object_writes_every_tool_type();
    put_disk_object_respects_write_protection();
    put_disk_object_refuses_what_it_cannot_write();
    tool_type_calls_match_case_blind();
    bump_revision_counts_copies_and_truncates();
    default_icons_cover_the_types();
    get_disk_object_new_falls_back_to_defaults();
    put_disk_object_round_trips_on_ram();
    put_disk_object_writes_only_ram();
    delete_disk_object_removes_the_icon();
    return check::finish("test_icon");
}
