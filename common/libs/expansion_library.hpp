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

#pragma once

#include "../m68k_cpu.hpp"
#include "../types.hpp"
#include "exec.hpp"

// expansion.library written in C++. The machine has no expansion boards,
// so FindConfigDev finds none and the board calls that make or free a
// ConfigDev only keep the books; the library base carries eb_MountList,
// the boot nodes, which programs such as GuessBootDev walk to find the
// device the system started from. The calls it has no use for end the run
// as unimplemented. Being in the library list, it is opened instead of an
// expansion.library in LIBS:.

namespace hombre {

namespace expansion_lvo {
constexpr u32 AddConfigDev = 30;
constexpr u32 AddBootNode = 36;
constexpr u32 AllocConfigDev = 48;
constexpr u32 FindConfigDev = 72;
constexpr u32 FreeConfigDev = 84;
constexpr u32 RemConfigDev = 108;
constexpr u32 ObtainConfigBinding = 120;
constexpr u32 ReleaseConfigBinding = 126;
constexpr u32 SetCurrentBinding = 132;
constexpr u32 GetCurrentBinding = 138;
constexpr u32 MakeDosNode = 144;
constexpr u32 AddDosNode = 150;
constexpr u32 Last = 162;
}

class ExpansionLibrary {
public:
    static constexpr u32 kMountListOffset = 74;  // eb_MountList
    static constexpr u32 kBaseBytes = kMountListOffset + 14;
    static constexpr u8 kBootNodeType = 16;      // NT_BOOTNODE

    // Makes expansion.library with an empty boot node list and adds it to
    // exec's library list. exec must be initialised. False if exec runs
    // out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec);

    // Adds a BootNode for a device node at the end of the list, at the
    // given priority. False on no memory or before init.
    bool add_boot_node(m68k::Machine& m, Exec& exec, u32 device_node, i32 priority);

    u32 base() const { return base_; }

private:
    friend struct ExpansionNatives;
    static constexpr u32 kVectors = expansion_lvo::Last / 6;
    static constexpr u32 kConfigDevBytes = 68;
    static constexpr u32 kBindingBytes = 16;

    m68k::Machine* machine_ = nullptr;
    Exec* exec_ = nullptr;
    u32 base_ = 0;
    NativeVector vectors_[kVectors]{};
    u8 binding_[kBindingBytes]{};
};

}
