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

#include "../log.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;

namespace {

// sizeof(struct Layer_Info).
constexpr u32 kLayerInfoBytes = 102;

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

}

struct LayersNatives {
    static Layers& of(void* ctx) { return *static_cast<Layers*>(ctx); }

    // NewLayerInfo: an empty Layer_Info, or 0.
    static void new_layer_info(void* ctx, Machine& m) {
        native::set_result(
            m, of(ctx).exec_->alloc(kLayerInfoBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR));
    }

    // DisposeLayerInfo: gives back the Layer_Info at A0.
    static void dispose_layer_info(void* ctx, Machine& m) {
        if (m.cpu.a[0] != 0) {
            of(ctx).exec_->free(m.cpu.a[0], kLayerInfoBytes);
        }
    }

    // CreateUpfrontLayer: a layer over the BitMap at A1 from D0, D1 to D2,
    // D3, both corners inside, with the flags in D4 and a RastPort of its
    // own; 0 when it cannot be made.
    static void create_upfront_layer(void* ctx, Machine& m) {
        Layers& layers = of(ctx);
        const u32 bitmap = m.cpu.a[1];
        const i32 x0 = static_cast<i32>(m.cpu.d[0]);
        const i32 y0 = static_cast<i32>(m.cpu.d[1]);
        const i32 x1 = static_cast<i32>(m.cpu.d[2]);
        const i32 y1 = static_cast<i32>(m.cpu.d[3]);
        const u32 flags = m.cpu.d[4];
        native::set_result(m, 0);
        if ((flags & ndk::LAYERSUPER) != 0) {
            LogLine().text("layers: a super bitmap layer is not made").send();
            return;
        }
        if (layers.graphics_ == nullptr || m.cpu.a[0] == 0 || bitmap == 0 || x1 < x0 ||
            y1 < y0) {
            return;
        }
        Exec& exec = *layers.exec_;
        const u32 layer = exec.alloc(ndk::Layer_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        const u32 rp = exec.alloc(ndk::RastPort_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
        if (layer == 0 || rp == 0) {
            if (layer != 0) {
                exec.free(layer, ndk::Layer_SIZEOF);
            }
            if (rp != 0) {
                exec.free(rp, ndk::RastPort_SIZEOF);
            }
            return;
        }
        layers.graphics_->init_rastport(rp);
        Bus b(m);
        b.w32(rp + ndk::rp_BitMap, bitmap);
        b.w32(rp + ndk::rp_Layer, layer);
        b.w32(layer + ndk::ly_rp, rp);
        b.w16(layer + ndk::ly_MinX, static_cast<u32>(x0));
        b.w16(layer + ndk::ly_MinY, static_cast<u32>(y0));
        b.w16(layer + ndk::ly_MaxX, static_cast<u32>(x1));
        b.w16(layer + ndk::ly_MaxY, static_cast<u32>(y1));
        b.w16(layer + ndk::ly_Flags, flags);
        layers.made_.push_back(Layers::Made{layer, rp});
        native::set_result(m, layer);
    }

    // DeleteLayer: gives back the layer at A1 and its RastPort; FALSE for
    // one CreateUpfrontLayer did not make.
    static void delete_layer(void* ctx, Machine& m) {
        Layers& layers = of(ctx);
        const u32 layer = m.cpu.a[1];
        for (auto it = layers.made_.begin(); it != layers.made_.end(); ++it) {
            if (it->layer == layer) {
                layers.exec_->free(it->rp, ndk::RastPort_SIZEOF);
                layers.exec_->free(it->layer, ndk::Layer_SIZEOF);
                layers.made_.erase(it);
                native::set_result(m, 1);
                return;
            }
        }
        native::set_result(m, 0);
    }
};

namespace {

constexpr Function kFunctions[] = {
    {layers_lvo::CreateUpfrontLayer, LayersNatives::create_upfront_layer, "CreateUpfrontLayer"},
    {layers_lvo::DeleteLayer, LayersNatives::delete_layer, "DeleteLayer"},
    {layers_lvo::NewLayerInfo, LayersNatives::new_layer_info, "NewLayerInfo"},
    {layers_lvo::DisposeLayerInfo, LayersNatives::dispose_layer_info, "DisposeLayerInfo"},
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
    exec_ = &exec;
    made_.clear();
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
        exec.make_native_library(kName, kHombreLibVersion, vectors_, kVectors, this);
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
