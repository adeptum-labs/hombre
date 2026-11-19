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

#include "icon.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;

namespace {

constexpr char kName[] = "icon.library";
constexpr char kIdString[] = "icon 60.0 (Hombre)\r\n";

// No icon: IoErr is left as it is, as for a missing .info file.
void no_icon(void*, Machine& m) { native::set_result(m, 0); }

void nothing(void*, Machine&) {}

void none(void*, Machine& m) { native::set_result(m, 0); }

struct Function {
    u32 lvo;
    m68k::NativeFn fn;
    const char* name;
};

constexpr Function kFunctions[] = {
    {icon_lvo::GetDiskObject, no_icon, "GetDiskObject"},
    {icon_lvo::FreeDiskObject, nothing, "FreeDiskObject"},
    {icon_lvo::FindToolType, none, "FindToolType"},
    {icon_lvo::MatchToolValue, none, "MatchToolValue"},
    {icon_lvo::GetDiskObjectNew, no_icon, "GetDiskObjectNew"},
};

}

bool Icon::init(Machine& m, Exec& exec) {
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
