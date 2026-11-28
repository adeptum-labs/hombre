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

// A BOOPSI class library written in C++, as datatypes.library opens one
// (datatypes/<name>.datatype): its first Open makes the class (an Open
// from another task meanwhile waits for it), the vector
// at LVO -30 (ObtainEngine) hands it out, and the class's cl_UserData is the
// library base, which datatypes.library closes when an object of the class
// is disposed of. The library is never expunged, so the class stays.

namespace hombre {

class NativeClassLibrary {
public:
    // Makes the class on the library's first open, with base the library
    // base: the class, or 0 when it cannot be made, and the open fails.
    using MakeClass = u32 (*)(void* ctx, u32 base);

    // The vector datatypes.library calls for the class.
    static constexpr u32 kObtainEngine = 30;

    // Adds the library to exec's library list under name, at Hombre's
    // version, with id as its version string; both must stay valid. The
    // library base, 0 when exec has no memory or native ids left.
    u32 make(m68k::Machine& m, Exec& exec, const char* name, const char* id,
             MakeClass make_class, void* ctx);

    u32 base() const { return base_; }
    // The class, 0 until the first open made it.
    u32 class_ptr() const { return class_; }

private:
    static void open(void* ctx, m68k::Machine& m);
    static void obtain_engine(void* ctx, m68k::Machine& m);

    static constexpr u32 kVectors = kObtainEngine / 6;

    // Makes the class unless another task is making it: then waits for
    // that, at most kMakeWaitUs. False when there is no class after.
    bool ensure_class(m68k::Machine& m, u32 base);

    static constexpr u64 kMakeWaitUs = 5000000;
    static constexpr u64 kMakePollUs = 1000;

    NativeVector vectors_[kVectors]{};
    MakeClass make_class_ = nullptr;
    void* ctx_ = nullptr;
    Exec* exec_ = nullptr;
    u32 base_ = 0;
    u32 class_ = 0;
    // Whether a task is making the class, and which.
    bool making_ = false;
    u32 maker_ = 0;
};

}
