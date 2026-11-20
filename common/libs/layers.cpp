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

#include "layers.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;

namespace {

constexpr char kName[] = "layers.library";
constexpr char kIdString[] = "layers 60.0 (Hombre)\r\n";

void nothing(void*, Machine&) {}

// InstallClipRegion: makes the region at A1, or none for 0, the clip region
// of the layer at A0 and returns the one it replaces. Drawing through the
// layer's RastPort stays inside the region until another call takes it out
// again; the region stays the caller's to dispose of.
void install_clip_region(void*, Machine& m) {
    Bus b(m);
    const u32 layer = m.cpu.a[0];
    if (layer == 0) {
        native::set_result(m, 0);
        return;
    }
    const u32 old = b.r32(layer + ndk::ly_ClipRegion);
    b.w32(layer + ndk::ly_ClipRegion, m.cpu.a[1]);
    if (b.ok()) {
        native::set_result(m, old);
    }
}

// struct Layer's BackFill: the hook that fills what drawing uncovers.
constexpr u32 ly_BackFill = 118;

// InstallLayerHook: the layer at A0 takes the backfill hook at A1 and the
// one it had comes back. The display fills uncovered parts with pen 0,
// whatever the hook.
void install_layer_hook(void*, Machine& m) {
    Bus b(m);
    const u32 layer = m.cpu.a[0];
    if (layer == 0) {
        native::set_result(m, 0);
        return;
    }
    const u32 old = b.r32(layer + ly_BackFill);
    b.w32(layer + ly_BackFill, m.cpu.a[1]);
    if (b.ok()) {
        native::set_result(m, old);
    }
}

// InstallLayerInfoHook: the screen's backfill hook; there is none to give
// back, the default.
void install_layer_info_hook(void*, Machine& m) { native::set_result(m, 0); }

struct Function {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr Function kFunctions[] = {
    {layers_lvo::LockLayer, nothing, "LockLayer"},
    {layers_lvo::UnlockLayer, nothing, "UnlockLayer"},
    {layers_lvo::LockLayers, nothing, "LockLayers"},
    {layers_lvo::UnlockLayers, nothing, "UnlockLayers"},
    {layers_lvo::LockLayerInfo, nothing, "LockLayerInfo"},
    {layers_lvo::UnlockLayerInfo, nothing, "UnlockLayerInfo"},
    {layers_lvo::InstallClipRegion, install_clip_region, "InstallClipRegion"},
    {layers_lvo::InstallLayerHook, install_layer_hook, "InstallLayerHook"},
    {layers_lvo::InstallLayerInfoHook, install_layer_info_hook, "InstallLayerInfoHook"},
};

}

bool Layers::init(Machine& m, Exec& exec) {
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
    const u32 base = exec.make_native_library(kName, kHombreLibVersion, vectors_, kVectors);
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
