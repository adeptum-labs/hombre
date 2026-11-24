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

bool ExpansionLibrary::init(Machine& m, Exec& exec) {
    base_ = 0;
    const u32 base = exec.make_native_library(kName, 40, nullptr, 0, nullptr, kBaseBytes);
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
