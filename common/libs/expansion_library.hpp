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

// expansion.library written in C++. It has no functions of its own: its
// library base carries eb_MountList, the boot nodes, which programs such
// as GuessBootDev walk to find the device the system started from. Being
// in the library list, it is opened instead of an expansion.library in
// LIBS:.

namespace hombre {

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
    u32 base_ = 0;
};

}
