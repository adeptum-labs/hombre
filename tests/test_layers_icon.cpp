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
#include "libs/icon.hpp"
#include "libs/layers.hpp"
#include "libs/version_library.hpp"
#include "check.hpp"
#include "ofs_writer.hpp"

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
constexpr u32 kData = 0x9000;  // test structures up to 0xFFFF
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;

constexpr u32 do_Type = 48;
constexpr u8 kWBProject = 4;

// Static storage, as the kernel keeps them.
Jit g_jit;
Exec g_exec;
Dos g_dos;
Layers g_layers;
VersionLibrary g_version;
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

// 16 MiB of guest memory, the Jit, exec, layers.library, dos and
// icon.library over them.
class Rig {
public:
    Rig()
        : memory_(kGuestBytes), image_(build_adf("LayersIcon", false,
                                                 {{"Readme", std::vector<u8>{'h', 'i'}}})) {
        fd_ = memfd_create("layers-icon", 0);
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
        CHECK(g_layers.init(m, g_exec));
        CHECK(g_version.init(m, g_exec));
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

    u32 layers_base() const { return g_layers.base(); }
    u32 icon_base() const { return g_icon.base(); }

    u32 put8(u32 at, u32 v) {
        memory_[at] = static_cast<u8>(v);
        return at;
    }

    u32 put16(u32 at, u32 v) {
        put8(at, v >> 8);
        put8(at + 1, v);
        return at;
    }

    u32 get8(u32 at) const { return memory_[at]; }
    u32 get32(u32 at) const {
        return (get8(at) << 24) | (get8(at + 1) << 16) | (get8(at + 2) << 8) | get8(at + 3);
    }

    u32 str(u32 at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            put8(at + static_cast<u32>(i), static_cast<u8>(s[i]));
        }
        put8(at + static_cast<u32>(s.size()), 0);
        return at;
    }

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

private:
    static constexpr usize kRegion = 1 << 20;

    std::vector<u8> memory_;
    std::vector<u8> image_;
    int fd_ = -1;
    u8* rw_ = nullptr;
    const u8* rx_ = nullptr;
};

void layer_locks_do_nothing() {
    Rig r;
    const u32 vectors[] = {layers_lvo::LockLayer,      layers_lvo::UnlockLayer,
                           layers_lvo::LockLayers,     layers_lvo::UnlockLayers,
                           layers_lvo::LockLayerInfo, layers_lvo::UnlockLayerInfo};
    for (u32 lvo : vectors) {
        r.m.cpu.d[0] = 0x12345678;
        r.m.cpu.a[0] = kData;
        r.m.cpu.a[1] = kData + 0x100;
        CHECK_EQ(r.result(r.layers_base(), lvo), 0x12345678u);
    }
}

void install_clip_region_keeps_the_region_and_returns_the_one_before() {
    Rig r;
    const u32 layer = kData;
    const u32 first = kData + 0x100;
    const u32 second = kData + 0x200;
    CHECK_EQ(ly_ClipRegion, 126u);

    // A layer without a region gives none back and takes the new one.
    r.m.cpu.a[0] = layer;
    r.m.cpu.a[1] = first;
    CHECK_EQ(r.result(r.layers_base(), layers_lvo::InstallClipRegion), 0u);
    CHECK_EQ(r.get32(layer + ly_ClipRegion), first);

    r.m.cpu.a[0] = layer;
    r.m.cpu.a[1] = second;
    CHECK_EQ(r.result(r.layers_base(), layers_lvo::InstallClipRegion), first);
    CHECK_EQ(r.get32(layer + ly_ClipRegion), second);

    // No region takes the clip out again and hands the last one back.
    r.m.cpu.a[0] = layer;
    r.m.cpu.a[1] = 0;
    CHECK_EQ(r.result(r.layers_base(), layers_lvo::InstallClipRegion), second);
    CHECK_EQ(r.get32(layer + ly_ClipRegion), 0u);

    // No layer has no region to give.
    r.m.cpu.a[0] = 0;
    r.m.cpu.a[1] = first;
    CHECK_EQ(r.result(r.layers_base(), layers_lvo::InstallClipRegion), 0u);

    // InstallLayerHook swaps the layer's backfill hook.
    r.put16(layer + 118, 0);
    r.put16(layer + 120, 0);
    r.m.cpu.a[0] = layer;
    r.m.cpu.a[1] = 1;
    CHECK_EQ(r.result(r.layers_base(), layers_lvo::InstallLayerHook), 0u);
    r.m.cpu.a[0] = layer;
    r.m.cpu.a[1] = 0;
    CHECK_EQ(r.result(r.layers_base(), layers_lvo::InstallLayerHook), 1u);
    CHECK_EQ(r.result(r.layers_base(), layers_lvo::InstallLayerInfoHook), 0u);
}

void version_library_carries_the_system_version() {
    Rig r;
    // The library in the list answers; LIBS: is never asked.
    bool asked = false;
    g_exec.set_segment_loader(SegmentLoader{
        [](void* ctx, const char*, u32&) {
            *static_cast<bool*>(ctx) = true;
            return false;
        },
        &asked});
    r.m.cpu.a[1] = r.str(kData, "version.library");
    r.m.cpu.d[0] = 0;
    const u32 base = r.result(g_exec.exec_base(), exec_lvo::OpenLibrary);
    CHECK_EQ(base, g_version.base());
    CHECK(!asked);
    CHECK_EQ((r.get8(base + LIB_VERSION) << 8) | r.get8(base + LIB_VERSION + 1), kHombreLibVersion);
    CHECK_EQ((r.get8(base + LIB_REVISION) << 8) | r.get8(base + LIB_REVISION + 1), kHombreLibRevision);
    r.m.cpu.a[1] = base;
    r.result(g_exec.exec_base(), exec_lvo::CloseLibrary);
}

void icon_reads_what_is_there_and_defaults_the_rest() {
    Rig r;
    r.str(kData, "NoIcon");
    r.m.cpu.a[0] = kData;
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::GetDiskObject), 0u);
    CHECK_EQ(g_dos.last_error(), ERROR_OBJECT_NOT_FOUND);

    // A missing .info falls back to the default icon of the object's type.
    r.m.cpu.a[0] = kData;
    const u32 object = r.result(r.icon_base(), icon_lvo::GetDiskObjectNew);
    CHECK(object != 0);
    CHECK_EQ(r.get8(object + do_Type), kWBProject);
    r.m.cpu.a[0] = object;
    CHECK_EQ(r.call(r.icon_base(), icon_lvo::FreeDiskObject), ExitReason::Halt);

    r.m.cpu.a[0] = 0;
    r.m.cpu.a[1] = 0;
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::FindToolType), 0u);
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::MatchToolValue), 0u);

    r.m.cpu.d[0] = 0x12345678;
    r.m.cpu.a[0] = kData;
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::FreeDiskObject), 0x12345678u);
    CHECK(logged_containing("FreeDiskObject"));
}

}

int main() {
    layer_locks_do_nothing();
    install_clip_region_keeps_the_region_and_returns_the_one_before();
    icon_reads_what_is_there_and_defaults_the_rest();
    version_library_carries_the_system_version();
    return check::finish("test_layers_icon");
}