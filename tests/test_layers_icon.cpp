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

#include "libs/exec.hpp"
#include "libs/icon.hpp"
#include "libs/layers.hpp"
#include "check.hpp"

#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace hombre;
using namespace hombre::m68k;

namespace {

constexpr u32 kCall = 0x1800;
constexpr u32 kStack = 0x8000;
constexpr u32 kData = 0x9000;  // test structures up to 0xFFFF
constexpr u32 kFreeStart = 0x10000;
constexpr u32 kFreeEnd = 0x400000;

// 16 MiB of guest memory, the Jit, exec, layers.library and icon.library
// over them.
class Rig {
public:
    Rig() : memory_(kGuestBytes) {
        fd_ = memfd_create("layers-icon", 0);
        CHECK(fd_ >= 0);
        CHECK(ftruncate(fd_, static_cast<off_t>(kRegion)) == 0);
        void* rw = mmap(nullptr, kRegion, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        void* rx = mmap(nullptr, kRegion, PROT_READ | PROT_EXEC, MAP_SHARED, fd_, 0);
        CHECK(rw != MAP_FAILED);
        CHECK(rx != MAP_FAILED);
        rw_ = static_cast<u8*>(rw);
        rx_ = static_cast<const u8*>(rx);
        machine.cpu = CpuState{};
        machine.mem = GuestMemory{memory_.data(), kGuestBytes};
        machine.jit = nullptr;
        CHECK(jit.init(CodeRegion{rw_, rx_, kRegion}, machine));
        CHECK(exec.init(machine, kFreeStart, kFreeEnd));
        CHECK(layers.init(machine, exec));
        CHECK(icon.init(machine, exec));
        machine.cpu.a[7] = kStack;
    }

    ~Rig() {
        munmap(rw_, kRegion);
        munmap(const_cast<u8*>(rx_), kRegion);
        close(fd_);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    Machine machine;
    Jit jit;
    Exec exec;
    Layers layers;
    Icon icon;

    u32 layers_base() const { return layers.base(); }
    u32 icon_base() const { return icon.base(); }

    void put16(u32 at, u32 v) {
        memory_[at] = static_cast<u8>(v >> 8);
        memory_[at + 1] = static_cast<u8>(v);
    }

    // JSR -lvo(A6) with A6 = base, from hand-written 68000 code.
    ExitReason call(u32 base, u32 lvo) {
        put16(kCall, 0x4EAE);
        put16(kCall + 2, static_cast<u32>(0x10000 - lvo));
        put16(kCall + 4, 0x4EF8);  // JMP $400.w
        put16(kCall + 6, 0x0400);
        jit.flush();
        machine.cpu.a[6] = base;
        machine.cpu.a[7] = kStack;
        machine.cpu.pc = kCall;
        return jit.run(1000000);
    }

    // Calls the vector, which must return; D0 is what it gave.
    u32 result(u32 base, u32 lvo) {
        CHECK_EQ(call(base, lvo), ExitReason::Halt);
        return machine.cpu.d[0];
    }

private:
    static constexpr usize kRegion = 1 << 20;

    std::vector<u8> memory_;
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
        r.machine.cpu.d[0] = 0x12345678;
        r.machine.cpu.a[0] = kData;
        r.machine.cpu.a[1] = kData + 0x100;
        CHECK_EQ(r.result(r.layers_base(), lvo), 0x12345678u);
    }
}

void install_clip_region_returns_zero() {
    Rig r;
    r.machine.cpu.a[0] = kData;
    r.machine.cpu.a[1] = kData + 0x100;
    CHECK_EQ(r.result(r.layers_base(), layers_lvo::InstallClipRegion), 0u);
}

void icon_finds_no_icon_or_tool_type() {
    Rig r;
    r.machine.cpu.a[0] = kData;
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::GetDiskObject), 0u);
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::GetDiskObjectNew), 0u);

    r.machine.cpu.a[0] = kData;
    r.machine.cpu.a[1] = kData + 0x100;
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::FindToolType), 0u);
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::MatchToolValue), 0u);

    r.machine.cpu.d[0] = 0x12345678;
    r.machine.cpu.a[0] = kData;
    CHECK_EQ(r.result(r.icon_base(), icon_lvo::FreeDiskObject), 0x12345678u);
}

}

int main() {
    layer_locks_do_nothing();
    install_clip_region_returns_zero();
    icon_finds_no_icon_or_tool_type();
    return check::finish("test_layers_icon");
}