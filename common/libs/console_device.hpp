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

#include "../types.hpp"
#include "exec.hpp"
#include "keymap_library.hpp"

#include <vector>

// console.device implemented as a native Exec device, minimal for now: it
// opens, keeps a KeyMap per opened unit and answers the keymap commands.
// Requests complete inside BeginIO. Opening unit CONU_LIBRARY gives the
// library vectors alone; the other units need a Window in io_Data, as
// CONU_STANDARD documents, and get their own copy of keymap.library's
// default KeyMap, which io_Unit then points at.

namespace hombre {

namespace console_lvo {

constexpr u32 CDInputHandler = 42;
constexpr u32 RawKeyConvert = 48;
// Private since 2.0: the text snipped from a console window, and the hooks
// told when it changes. ConClip uses them.
constexpr u32 GetConSnip = 54;
constexpr u32 SetConSnip = 60;
constexpr u32 AddConSnipHook = 66;
constexpr u32 RemConSnipHook = 72;

}

namespace console_ndk {

constexpr u16 CD_ASKKEYMAP = 9;
constexpr u16 CD_SETKEYMAP = 10;
constexpr u16 CD_ASKDEFAULTKEYMAP = 11;
constexpr u16 CD_SETDEFAULTKEYMAP = 12;

constexpr u32 CONU_LIBRARY = 0xFFFFFFFF;
constexpr u32 CONU_STANDARD = 0;
constexpr u32 CONU_CHARMAP = 1;
constexpr u32 CONU_SNIPMAP = 3;

}

class ConsoleDevice {
public:
    static constexpr u16 kVersion = 40;

    // Creates "console.device" version 40 on ExecBase->DeviceList. exec and
    // keymaps must be initialised, and outlive the device.
    bool init(m68k::Machine& m, Exec& exec, KeymapLibrary& keymaps);

    u32 device_base() const { return base_; }

    // The current snip, "" when there is none.
    const char* snip() const { return snip_; }
    // The hooks AddConSnipHook registered.
    static constexpr u32 kMaxSnipHooks = 64;
    u32 snip_hook(u32 index) const {
        return index < snip_hooks_.size() ? snip_hooks_[index] : 0;
    }

private:
    friend struct ConsoleOps;

    static constexpr u32 kMaxSnip = 4096;

    Exec* exec_ = nullptr;
    KeymapLibrary* keymaps_ = nullptr;
    u32 base_ = 0;
    char snip_[kMaxSnip + 1]{};
    std::vector<u32> snip_hooks_;
};

}
