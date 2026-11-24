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

#include "expansion_library.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;

namespace {

constexpr char kName[] = "expansion.library";
constexpr u32 kBootNodeBytes = 18;  // Node, bn_Flags, bn_DeviceNode

}

struct ExpansionNatives {
    static ExpansionLibrary& of(void* ctx) { return *static_cast<ExpansionLibrary*>(ctx); }

    // FindConfigDev(old A0, manufacturer D0, product D1): no board.
    static void find_config_dev(void*, Machine& m) { native::set_result(m, 0); }

    // AllocConfigDev(): a cleared ConfigDev.
    static void alloc_config_dev(void* ctx, Machine& m) {
        ExpansionLibrary& e = of(ctx);
        native::set_result(m, e.exec_->alloc(ExpansionLibrary::kConfigDevBytes,
                                             ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR));
    }

    // FreeConfigDev(configDev A0)
    static void free_config_dev(void* ctx, Machine& m) {
        ExpansionLibrary& e = of(ctx);
        if (m.cpu.a[0] != 0) {
            e.exec_->free(m.cpu.a[0], ExpansionLibrary::kConfigDevBytes);
        }
    }

    // The calls that only keep the books of a board list there is none of.
    static void nothing(void*, Machine&) {}

    // SetCurrentBinding(binding A0, size D0): the binding is kept for
    // GetCurrentBinding, up to its own size.
    static void set_current_binding(void* ctx, Machine& m) {
        ExpansionLibrary& e = of(ctx);
        Bus b(m);
        const u32 size = m.cpu.d[0] < ExpansionLibrary::kBindingBytes
                             ? m.cpu.d[0]
                             : ExpansionLibrary::kBindingBytes;
        for (u32 i = 0; i < size; ++i) {
            e.binding_[i] = static_cast<u8>(b.r8(m.cpu.a[0] + i));
        }
    }

    // GetCurrentBinding(binding A0, size D0)
    static void get_current_binding(void* ctx, Machine& m) {
        ExpansionLibrary& e = of(ctx);
        Bus b(m);
        const u32 size = m.cpu.d[0] < ExpansionLibrary::kBindingBytes
                             ? m.cpu.d[0]
                             : ExpansionLibrary::kBindingBytes;
        for (u32 i = 0; i < size; ++i) {
            b.w8(m.cpu.a[0] + i, e.binding_[i]);
        }
    }
};

namespace {

struct Function {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr Function kFunctions[] = {
    {expansion_lvo::AddConfigDev, ExpansionNatives::nothing, "AddConfigDev"},
    {expansion_lvo::AllocConfigDev, ExpansionNatives::alloc_config_dev, "AllocConfigDev"},
    {expansion_lvo::FindConfigDev, ExpansionNatives::find_config_dev, "FindConfigDev"},
    {expansion_lvo::FreeConfigDev, ExpansionNatives::free_config_dev, "FreeConfigDev"},
    {expansion_lvo::RemConfigDev, ExpansionNatives::nothing, "RemConfigDev"},
    {expansion_lvo::ObtainConfigBinding, ExpansionNatives::nothing, "ObtainConfigBinding"},
    {expansion_lvo::ReleaseConfigBinding, ExpansionNatives::nothing, "ReleaseConfigBinding"},
    {expansion_lvo::SetCurrentBinding, ExpansionNatives::set_current_binding,
     "SetCurrentBinding"},
    {expansion_lvo::GetCurrentBinding, ExpansionNatives::get_current_binding,
     "GetCurrentBinding"},
};

}

bool ExpansionLibrary::init(Machine& m, Exec& exec) {
    base_ = 0;
    machine_ = &m;
    exec_ = &exec;
    for (NativeVector& v : vectors_) {
        v = NativeVector{nullptr, nullptr};
    }
    for (const Function& f : kFunctions) {
        vectors_[f.lvo / ndk::LIB_VECTSIZE - 1] = NativeVector{f.fn, f.name};
    }
    const u32 base = exec.make_native_library(kName, 40, vectors_, kVectors, this, kBaseBytes);
    if (base == 0) {
        return false;
    }
    Bus b(m);
    const u32 list = base + kMountListOffset;
    b.w32(list, list + 4);      // lh_Head: the tail
    b.w32(list + 4, 0);         // lh_Tail
    b.w32(list + 8, list);      // lh_TailPred: the head
    b.w8(list + 12, 0);         // lh_Type
    if (!b.ok()) {
        return false;
    }
    base_ = base;
    return true;
}

bool ExpansionLibrary::add_boot_node(Machine& m, Exec& exec, u32 device_node, i32 priority) {
    if (base_ == 0) {
        return false;
    }
    const u32 node = exec.alloc(kBootNodeBytes, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (node == 0) {
        return false;
    }
    Bus b(m);
    const u32 list = base_ + kMountListOffset;
    const u32 last = b.r32(list + 8);
    b.w8(node + 8, kBootNodeType);
    b.w8(node + 9, static_cast<u8>(priority));
    b.w32(node + 16, device_node);
    b.w32(node, list + 4);      // ln_Succ: the tail
    b.w32(node + 4, last);      // ln_Pred
    b.w32(last, node);
    b.w32(list + 8, node);
    return b.ok();
}

}
