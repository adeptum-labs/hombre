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

#include "console_device.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using namespace console_ndk;
using namespace keymap_ndk;
using namespace ndk;

namespace {

constexpr u32 kNulTerminated = 0xFFFFFFFF;  // an io_Length of -1 for CMD_WRITE

void fault_null(Bus& b) {
    b.bytes_at(0, b.machine().mem.size + 1);
}

void copy_keymap(Bus& b, u32 to, u32 from) {
    for (u32 i = 0; i < KEYMAP_SIZE; i += 4) {
        b.w32(to + i, b.r32(from + i));
    }
}

bool opens_a_window(u32 unit) {
    return unit == CONU_STANDARD || unit == CONU_CHARMAP || unit == CONU_SNIPMAP;
}

}

struct ConsoleOps {
    static ConsoleDevice& of(void* ctx) { return *static_cast<ConsoleDevice*>(ctx); }

    // The unit of an open: none for library use, else a copy of the default
    // KeyMap that the window's console changes without touching the default.
    static bool make_unit(ConsoleDevice& c, Bus& b, u32 number, u32 window, u32& unit) {
        unit = 0;
        if (number == CONU_LIBRARY) {
            return true;
        }
        if (!opens_a_window(number) || window == 0) {
            return false;
        }
        unit = c.exec_->alloc(KEYMAP_SIZE, MEMF_PUBLIC);
        if (unit == 0) {
            return false;
        }
        copy_keymap(b, unit, c.keymaps_->default_keymap());
        return true;
    }

    static void open(void* ctx, Machine& m) {
        ConsoleDevice& c = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        const u32 dev = m.cpu.a[6];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        u32 unit = 0;
        const bool opened = make_unit(c, b, m.cpu.d[0], b.r32(io_raw + io_Data), unit);
        if (!b.ok()) {
            return;
        }
        if (!opened) {
            b.w32(io_raw + io_Unit, 0);
            b.w32(io_raw + io_Device, 0);
            b.w8(io_raw + io_Error, static_cast<u8>(IOERR_OPENFAIL));
            if (b.ok()) {
                native::set_result(m, static_cast<u32>(-1));
            }
            return;
        }
        b.w16(dev + LIB_OPENCNT, b.r16(dev + LIB_OPENCNT) + 1);
        b.w8(dev + LIB_FLAGS, b.r8(dev + LIB_FLAGS) & ~LIBF_DELEXP);
        b.w32(io_raw + io_Unit, unit);
        b.w8(io_raw + io_Error, 0);
        if (b.ok()) {
            native::set_result(m, 0);
        }
    }

    static void close(void* ctx, Machine& m) {
        ConsoleDevice& c = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        const u32 dev = m.cpu.a[6];
        const u32 count = b.r16(dev + LIB_OPENCNT);
        if (count != 0) {
            b.w16(dev + LIB_OPENCNT, count - 1);
        }
        if (native::guest_address(m, io_raw) != 0) {
            const u32 unit = b.r32(io_raw + io_Unit);
            if (unit != 0) {
                c.exec_->free(unit, KEYMAP_SIZE);
            }
            b.w32(io_raw + io_Unit, 0);
        }
        native::set_result(m, 0);
    }

    // The KeyMap at keymap to the buffer in io_Data.
    static i8 ask_keymap(Bus& b, u32 io_raw, u32 keymap) {
        if (b.r32(io_raw + io_Length) < KEYMAP_SIZE) {
            return IOERR_BADLENGTH;
        }
        copy_keymap(b, b.r32(io_raw + io_Data), keymap);
        b.w32(io_raw + io_Actual, KEYMAP_SIZE);
        return 0;
    }

    static i8 write(Bus& b, u32 io_raw) {
        const u32 length = b.r32(io_raw + io_Length);
        u32 written = length;
        if (length == kNulTerminated) {
            const u32 text = b.r32(io_raw + io_Data);
            written = 0;
            while (b.ok() && b.r8(text + written) != 0) {
                ++written;
            }
        }
        // TODO: render the text into the unit's window.
        b.w32(io_raw + io_Actual, written);
        return 0;
    }

    static i8 run_command(ConsoleDevice& c, Bus& b, u32 io_raw, u16 cmd) {
        if (cmd == CD_ASKDEFAULTKEYMAP) {
            return ask_keymap(b, io_raw, c.keymaps_->default_keymap());
        }
        if (cmd == CD_SETDEFAULTKEYMAP) {
            c.keymaps_->set_default_keymap(b.r32(io_raw + io_Data));
            return 0;
        }
        const u32 unit = b.r32(io_raw + io_Unit);
        if (unit == 0) {
            return IOERR_NOCMD;  // opened for library use, no unit to command
        }
        switch (cmd) {
        case CD_ASKKEYMAP:
            return ask_keymap(b, io_raw, unit);
        case CD_SETKEYMAP:
            if (b.r32(io_raw + io_Length) < KEYMAP_SIZE) {
                return IOERR_BADLENGTH;
            }
            copy_keymap(b, unit, b.r32(io_raw + io_Data));
            return 0;
        case CMD_WRITE:
            return write(b, io_raw);
        case CMD_CLEAR:
            return 0;
        default:
            // TODO: CMD_READ waits for the input handler to deliver keys.
            return IOERR_NOCMD;
        }
    }

    static void begin_io(void* ctx, Machine& m) {
        ConsoleDevice& c = of(ctx);
        Bus b(m);
        const u32 io_raw = m.cpu.a[1];
        if (native::guest_address(m, io_raw) == 0) {
            fault_null(b);
            return;
        }
        const u8 flags = static_cast<u8>(b.r8(io_raw + io_Flags));
        const i8 err = run_command(c, b, io_raw, static_cast<u16>(b.r16(io_raw + io_Command)));
        b.w8(io_raw + io_Error, static_cast<u8>(err));
        if (b.ok() && (flags & IOF_QUICK) == 0) {
            c.exec_->reply_message(io_raw);
        }
    }

    // Every request has completed by the time BeginIO returns.
    static void abort_io(void*, Machine& m) {
        native::set_result(m, static_cast<u32>(-1));
    }

    static void input_handler(void*, Machine& m) {
        native::set_result(m, m.cpu.a[0]);
    }

    static void raw_key_convert(void* ctx, Machine& m) {
        Bus b(m);
        native::set_result(m, of(ctx).keymaps_->map_raw(b, m.cpu.a[0], m.cpu.a[1], m.cpu.d[1],
                                                         m.cpu.a[2]));
    }
};

namespace {

constexpr NativeVector kConsoleVectors[] = {
    {ConsoleOps::open, "Open"},
    {ConsoleOps::close, "Close"},
    {nullptr, "Expunge"},
    {nullptr, "Reserved"},
    {ConsoleOps::begin_io, "BeginIO"},
    {ConsoleOps::abort_io, "AbortIO"},
    {ConsoleOps::input_handler, "CDInputHandler"},
    {ConsoleOps::raw_key_convert, "RawKeyConvert"},
};

}

bool ConsoleDevice::init(Machine&, Exec& exec, KeymapLibrary& keymaps) {
    exec_ = &exec;
    keymaps_ = &keymaps;
    base_ = exec.make_native_device("console.device", kVersion, kConsoleVectors,
                                    sizeof kConsoleVectors / sizeof kConsoleVectors[0], this);
    return base_ != 0;
}

}
