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
#include "native.hpp"
#include "version.hpp"

// icon.library written in C++ for code run by the 68000 translator.
//
// Icons are not read yet: GetDiskObject and GetDiskObjectNew find none,
// as for a file without a .info, and the tool type calls find nothing.
// Every other vector logs "icon: unimplemented LVO -<n>" and ends the run
// with Unimplemented.

namespace hombre {

// Library vector offsets of icon.library, n of LVO -n, as the 3.1 function
// description numbers them.
namespace icon_lvo {

constexpr u32 GetDiskObject = 78;
constexpr u32 FreeDiskObject = 90;
constexpr u32 FindToolType = 96;
constexpr u32 MatchToolValue = 102;
constexpr u32 GetDiskObjectNew = 132;
// The last vector of the 3.1 library; the table reaches down to it.
constexpr u32 Last = 138;

}

class Icon {
public:
    // Makes icon.library and adds it to exec's library list. exec must be
    // initialised. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec);

    u32 base() const { return base_; }

private:
    static constexpr u32 kVectors = icon_lvo::Last / 6;

    u32 base_ = 0;
    NativeVector vectors_[kVectors]{};
};

}
