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

#include "m68k_run.hpp"

#include "../common/libs/dos.hpp"
#include "../common/libs/exec.hpp"
#include "../common/libs/graphics.hpp"
#include "../common/libs/intuition.hpp"
#include "../common/libs/native.hpp"
#include "../common/libs/shell_window.hpp"
#include "../common/libs/utility.hpp"
#include "../common/m68k_decode.hpp"
#include "../common/m68k_jit.hpp"
#include "../common/printable.hpp"
#include "../common/serial.hpp"
#include "amiga_display.hpp"
#include "boot_framebuffer.hpp"
#include "code_region.hpp"

namespace hombre {

namespace {

using m68k::ExitReason;

constexpr u64 page_size = 4096;
// Generated code; the translator starts over when it is full.
constexpr u64 code_region_bytes = 2ull << 20;
// The 68000 has 24 address lines, and software written for it may keep
// flags in the top byte of a pointer. Guest memory is 16 MiB, all of it
// below the mask, so the mask costs nothing.
constexpr u32 address_mask = 0x00FFFFFF;
// The stack exec and dos start on; commands run on a stack dos allocates.
constexpr u32 boot_stack = 0x8000;
// exec's free memory: everything above the vectors, the stubs and the boot
// stack.
constexpr u32 free_start = 0x10000;
constexpr u64 block_budget = 1ull << 24;

// Constant initialised, so they sit in .bss (or .data) without a
// constructor run.
constinit m68k::Machine machine{};
constinit m68k::Jit jit{};
constinit Exec exec{};
constinit Utility utility{};
constinit Dos dos{};
constinit Graphics graphics{};
constinit Intuition intuition{};
constinit DisplayState display{};
constinit AmigaDisplay amiga_display{};
constinit ShellWindow shell{};
// The default Amiga screen; 0 until it is open.
constinit u32 amiga_screen = 0;

// Whether the console output so far ends a line.
constinit bool console_at_line_start = true;

void write_filtered(const char* text) {
    for (; *text != '\0'; ++text) {
        serial::write_char(printable_char(static_cast<u8>(*text)));
    }
}

// The native libraries' diagnostics, one line each. They can carry names
// from the guest, so they go through the printable filter.
void log_line(const char* line) {
    if (!console_at_line_start) {
        serial::write_char('\n');
        console_at_line_start = true;
    }
    write_filtered(line);
    serial::write_char('\n');
}

// What the program writes to the console. Newlines and tabs are kept.
void console_write(void*, const u8* data, u32 bytes) {
    for (u32 i = 0; i < bytes; ++i) {
        const u8 c = data[i];
        serial::write_char(c == '\n' || c == '\t' ? static_cast<char>(c) : printable_char(c));
        console_at_line_start = c == '\n';
    }
    if (shell.is_open()) {
        shell.write(data, bytes);
    }
}

void fail(const char* reason) {
    serial::write("m68k: ");
    serial::write(reason);
    serial::write("\n");
}

// Opens the default Amiga screen, when there is a framebuffer. Nothing is
// drawn yet: a failure is logged and leaves the kernel's text console as the
// display.
void open_amiga_screen(const BootInfo& info) {
    if (info.fb.phys == 0) {
        return;
    }
    if (!graphics.init(machine, exec, display) ||
        !intuition.init(machine, exec, graphics, display)) {
        fail("graphics init failed");
        return;
    }
    constexpr char title[] = "Amiga Workbench";
    const u32 new_screen = exec.alloc(ndk::NewScreen_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    const u32 title_at = exec.alloc(sizeof(title), ndk::MEMF_PUBLIC);
    if (new_screen == 0 || title_at == 0) {
        fail("no memory for the screen");
        return;
    }
    native::Bus bus(machine);
    for (u32 i = 0; i < sizeof(title); ++i) {
        bus.w8(title_at + i, static_cast<u8>(title[i]));
    }
    bus.w16(new_screen + ndk::ns_Width, 640);
    bus.w16(new_screen + ndk::ns_Height, 400);
    bus.w16(new_screen + ndk::ns_Depth, 2);
    bus.w16(new_screen + ndk::ns_Type, ndk::CUSTOMSCREEN);
    bus.w32(new_screen + ndk::ns_DefaultTitle, title_at);
    amiga_screen = bus.ok() ? intuition.open_screen(new_screen, 0) : 0;
    exec.free(new_screen, ndk::NewScreen_SIZEOF);
    exec.free(title_at, sizeof(title));
    if (amiga_screen == 0) {
        fail("could not open the screen");
        return;
    }
    serial::write("intuition: screen opened 640x400\n");
}

// Hands the framebuffer to the Amiga screen: the kernel's text console stops
// and the screen is painted in full.
void show_amiga_screen(const BootInfo& info) {
    serial::mirror = nullptr;
    amiga_display.init(make_framebuffer(info));
    display.sink = amiga_display.sink();
    intuition.screen_to_front(amiga_screen);
}

// Prepares guest memory, the translator, exec, utility and dos. Logs one
// line and returns false when a step fails.
bool start(const BootInfo& info, PhysAllocator& pages, const AdfVolume& volume) {
    constexpr u64 guest_pages = m68k::kGuestBytes / page_size;
    const u64 guest = pages.alloc_pages(guest_pages);
    if (guest == 0) {
        fail("no memory for the guest");
        return false;
    }
    m68k::CodeRegion region{};
    if (!code_region_create(info, pages, code_region_bytes, region)) {
        pages.free_pages(guest, guest_pages);
        fail("no code region");
        return false;
    }

    m68k::decode_init();
    machine.cpu = m68k::CpuState{};
    machine.mem = m68k::GuestMemory{pages.to_ptr(guest), m68k::kGuestBytes};
    machine.jit = nullptr;
    if (!jit.init(region, machine)) {
        fail("translator init failed");
        return false;
    }
    jit.set_address_mask(address_mask);
    g_log = &log_line;
    if (!exec.init(machine, free_start, m68k::kGuestBytes)) {
        fail("exec init failed");
        return false;
    }
    if (!utility.init(machine, exec)) {
        fail("utility init failed");
        return false;
    }
    if (!dos.init(machine, exec, volume, &console_write, nullptr)) {
        fail("dos init failed");
        return false;
    }
    machine.cpu.a[7] = boot_stack;
    open_amiga_screen(info);
    return true;
}

}

void m68k_run_command(const BootInfo& info, PhysAllocator& pages, const AdfVolume& volume,
                      const char* path, const char* args) {
    if (!start(info, pages, volume)) {
        return;
    }
    if (amiga_screen != 0) {
        if (shell.open(machine, exec, graphics, intuition, display)) {
            serial::write("intuition: window opened AmigaShell\n");
            show_amiga_screen(info);
        } else {
            // The kernel's text console stays as the display.
            fail("could not open the shell window");
            intuition.close_screen(amiga_screen);
            amiga_screen = 0;
        }
    }
    serial::write("m68k: running ");
    write_filtered(path);
    serial::write("\n");

    machine.cpu.exit = ExitReason::None;
    u32 result = 0;
    const bool ok = dos.run_command(path, args, result, block_budget);
    if (!console_at_line_start) {
        serial::write_char('\n');
        console_at_line_start = true;
    }

    const m68k::CpuState& cpu = machine.cpu;
    serial::write("m68k: ");
    if (ok) {
        write_filtered(path);
        serial::write(" returned ");
        serial::write_dec(result);
    } else if (cpu.exit == ExitReason::None) {
        // run_command fails without running anything only when it cannot
        // load the program.
        serial::write("could not load ");
        write_filtered(path);
    } else {
        serial::write(cpu.exit == ExitReason::Budget ? "budget exhausted"
                                                     : m68k::exit_reason_name(cpu.exit));
        serial::write(" at pc=0x");
        serial::write_hex(cpu.pc);
        // An untranslated or illegal opcode, or an A-line call without a
        // native: the word at pc tells them apart.
        u32 op = 0;
        if ((cpu.exit == ExitReason::Unimplemented || cpu.exit == ExitReason::Illegal) &&
            machine.mem.read16(cpu.pc & address_mask, op)) {
            serial::write(" op=0x");
            serial::write_hex(op);
        }
    }
    serial::write("\n");
    if (shell.is_open()) {
        shell.write_prompt(1, volume.name);
    }
}

}
