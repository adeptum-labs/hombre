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

// layers.library written in C++ for code run by the 68000 translator.
//
// Windows keep their own bitmaps and tasks switch only at safe points on
// one CPU, so the layer locks have nothing to guard and return at once.
// InstallClipRegion keeps no region and returns none. Every other vector
// logs "layers: unimplemented LVO -<n>" and ends the run with
// Unimplemented.

namespace hombre {

// Library vector offsets of layers.library, n of LVO -n, as the 3.1 function
// description numbers them.
namespace layers_lvo {

constexpr u32 LockLayer = 96;
constexpr u32 UnlockLayer = 102;
constexpr u32 LockLayers = 108;
constexpr u32 UnlockLayers = 114;
constexpr u32 LockLayerInfo = 120;
constexpr u32 UnlockLayerInfo = 138;
constexpr u32 InstallClipRegion = 174;
// The last vector of the 3.1 library; the table reaches down to it.
constexpr u32 Last = 180;

}

class Layers {
public:
    // Makes layers.library and adds it to exec's library list. exec must be
    // initialised. False if exec runs out of memory or native ids.
    bool init(m68k::Machine& m, Exec& exec);

    u32 base() const { return base_; }

private:
    static constexpr u32 kVectors = layers_lvo::Last / 6;

    u32 base_ = 0;
    NativeVector vectors_[kVectors]{};
};

}
