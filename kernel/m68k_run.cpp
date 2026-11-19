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

#include "../common/console_keys.hpp"
#include "../common/dos_date.hpp"
#include "../common/format.hpp"
#include "../common/libs/dos.hpp"
#include "../common/libs/exec.hpp"
#include "../common/libs/graphics.hpp"
#include "../common/libs/intuition.hpp"
#include "../common/libs/native.hpp"
#include "../common/libs/utility.hpp"
#include "../common/m68k_decode.hpp"
#include "../common/m68k_jit.hpp"
#include "../common/printable.hpp"
#include "../common/hid_keyboard.hpp"
#include "../common/hid_mouse.hpp"
#include "../common/serial.hpp"
#include "../common/shell.hpp"
#include "../common/shell_commands.hpp"
#include "../common/shell_window.hpp"
#include "amiga_display.hpp"
#include "apic.hpp"
#include "boot_framebuffer.hpp"
#include "clock.hpp"
#include "code_region.hpp"
#include "rtc.hpp"
#include "usb_keyboard.hpp"

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
constexpr u32 poll_budget = 64;
constexpr u8 usage_key_c = 0x06;
constexpr u8 usage_key_backslash_us = 0x31;
constexpr u8 usage_key_backslash_iso = 0x32;
constexpr u8 ctrl_backslash = 0x1C;
constexpr u32 key_queue_cap = 64;
constexpr u32 sigbreakf_ctrl_c = 1u << 12;

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
constinit PointerState pointer{};
constinit ShellWindow shell{};
// The default Amiga screen; 0 until it is open.
constinit u32 amiga_screen = 0;
// The key slots of the last keyboard report seen while a command waited.
constinit u8 prev_key_slots[KeyDecoder::kKeySlots]{};
// The key decoder and console sequence filter of the shell and
// running-command input paths.
constinit KeyDecoder typed_decoder{};
constinit CsiFilter console_filter{};
constinit u8 key_queue[key_queue_cap]{};
constinit u32 key_queue_head = 0;
constinit u32 key_queue_count = 0;

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

void poll_keys_while_running();

// True while a command runs: its console output is also when the keyboard
// is read, so keys typed during a long listing are not lost.
constinit bool command_running = false;

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
    if (command_running) {
        poll_keys_while_running();
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
    intuition.set_clock(&clock_us);
    constexpr char title[] = "Hombre Workbench";
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
    bus.w16(new_screen + ndk::ns_Depth, 8);
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
    pointer.init(640, 400, 320, 200);
    amiga_display.move_pointer(machine, amiga_screen, pointer.x(), pointer.y());
    display.sink = amiga_display.sink();
    intuition.screen_to_front(amiga_screen);
    if (usb_mouse_present()) {
        amiga_display.set_pointer_visible(machine, amiga_screen, true);
    }
}

void log_mouse_buttons(const char* prefix, u8 mask, i32 x, i32 y) {
    for (u32 bit = 0; bit < 3; ++bit) {
        if ((mask & (1u << bit)) == 0) {
            continue;
        }
        serial::write(prefix);
        serial::write_dec(bit + 1);
        serial::write(" at ");
        serial::write_dec(static_cast<u64>(x));
        serial::write(",");
        serial::write_dec(static_cast<u64>(y));
        serial::write("\n");
    }
}

void flush_mouse_move() {
    amiga_display.move_pointer(machine, intuition.front_screen(), pointer.x(), pointer.y());
    intuition.pointer_event(pointer.x(), pointer.y(), pointer.buttons(), 0, 0, true);
}

void on_shell_view(void*, u32 view) {
    if (!console_at_line_start) {
        serial::write_char('\n');
        console_at_line_start = true;
    }
    serial::write("shell: view ");
    serial::write_dec(view);
    serial::write("\n");
}

// The AmigaDOS time: the real-time clock read once, advanced by the TSC
// clock. Without a readable clock DateStamp stays at the start of 1978.
u64 boot_dos_ticks = 0;
u64 boot_clock_us = 0;

u64 dos_now_ticks() {
    return boot_dos_ticks + (clock_us() - boot_clock_us) / 20000;
}

void start_dos_clock() {
    CivilTime now{};
    if (!rtc_read(now)) {
        serial::write("clock: no real-time clock\n");
        return;
    }
    const DosStamp stamp = dos_stamp_from_civil(now);
    boot_dos_ticks = (static_cast<u64>(stamp.days) * 1440 + stamp.minutes) * 3000 + stamp.ticks;
    boot_clock_us = clock_us();
    dos.set_clock(&dos_now_ticks);
    serial::write("clock: ");
    serial::write_dec(now.year);
    serial::write("-");
    serial::write_dec(now.month);
    serial::write("-");
    serial::write_dec(now.day);
    serial::write(" ");
    serial::write_dec(now.hour);
    serial::write(":");
    serial::write_dec(now.minute);
    serial::write("\n");
}

void flush_wheel(i32 wheel) {
    if (wheel == 0) {
        return;
    }
    const u32 win = shell.window();
    if (win != 0 && intuition.window_under(pointer.x(), pointer.y()) == win) {
        shell.scroll_by(3 * wheel);
    } else {
        intuition.wheel_event(wheel);
    }
}

void poll_mouse() {
    u8 report[4];
    bool moved = false;
    i32 wheel = 0;
    for (u32 i = 0; i < poll_budget && usb_mouse_poll(report); ++i) {
        MouseSample sample{};
        if (!decode_mouse_report(report, sizeof(report), sample)) {
            continue;
        }
        const PointerState::Update update = pointer.apply(sample);
        if (update.moved) {
            moved = true;
        }
        wheel += update.wheel;
        if (update.pressed != 0 || update.released != 0) {
            if (moved) {
                flush_mouse_move();
                moved = false;
            }
            if (wheel != 0) {
                flush_wheel(wheel);
                wheel = 0;
            }
            intuition.pointer_event(pointer.x(), pointer.y(), pointer.buttons(), update.pressed,
                                    update.released, false);
            log_mouse_buttons("mouse: down ", update.pressed, pointer.x(), pointer.y());
            log_mouse_buttons("mouse: up ", update.released, pointer.x(), pointer.y());
        }
    }
    if (moved) {
        flush_mouse_move();
    }
    if (wheel != 0) {
        flush_wheel(wheel);
    }
}

void key_queue_clear() {
    key_queue_head = 0;
    key_queue_count = 0;
}

void key_queue_push(u8 ch) {
    if (key_queue_count >= key_queue_cap) {
        return;
    }
    key_queue[(key_queue_head + key_queue_count) % key_queue_cap] = ch;
    ++key_queue_count;
}

bool key_queue_pop(u8& out) {
    if (key_queue_count == 0) {
        return false;
    }
    out = key_queue[key_queue_head];
    key_queue_head = (key_queue_head + 1) % key_queue_cap;
    --key_queue_count;
    return true;
}

void handle_decoded_byte(u8 raw, Shell* line_editor) {
    u8 out = 0;
    const ConsoleKey key = console_filter.feed(raw, out);
    switch (key) {
    case ConsoleKey::Byte:
        if (line_editor != nullptr) {
            line_editor->key(out);
        } else {
            key_queue_push(out);
        }
        break;
    case ConsoleKey::RowUp:
        shell.scroll_by(1);
        break;
    case ConsoleKey::RowDown:
        shell.scroll_by(-1);
        break;
    case ConsoleKey::PageUp: {
        const u32 rows = shell.rows();
        const i32 step = rows > 1 ? static_cast<i32>(rows - 1) : 0;
        shell.scroll_by(step);
        break;
    }
    case ConsoleKey::PageDown: {
        const u32 rows = shell.rows();
        const i32 step = rows > 1 ? static_cast<i32>(rows - 1) : 0;
        shell.scroll_by(-step);
        break;
    }
    case ConsoleKey::Top:
        shell.scroll_to_top();
        break;
    case ConsoleKey::Bottom:
        shell.scroll_to_bottom();
        break;
    case ConsoleKey::HistoryUp:
        if (line_editor != nullptr) {
            line_editor->history_up();
        }
        break;
    case ConsoleKey::HistoryDown:
        if (line_editor != nullptr) {
            line_editor->history_down();
        }
        break;
    case ConsoleKey::CursorLeft:
        if (line_editor != nullptr) {
            line_editor->cursor_left();
        }
        break;
    case ConsoleKey::CursorRight:
        if (line_editor != nullptr) {
            line_editor->cursor_right();
        }
        break;
    case ConsoleKey::None:
    case ConsoleKey::Swallowed:
        break;
    }
}

bool slots_have_key(const u8* slots, u8 usage) {
    for (usize i = 0; i < KeyDecoder::kKeySlots; ++i) {
        if (slots[i] == usage) {
            return true;
        }
    }
    return false;
}

// Reads the keyboard while a command runs: Ctrl+C becomes the break signal
// and other keys go to the queue the console input and the shell read.
void poll_keys_while_running() {
    if (usb_keyboard_status() != UsbStatus::Ready) {
        return;
    }
    u8 report[KeyDecoder::kReportBytes];
    for (u32 i = 0; i < poll_budget && usb_keyboard_poll(report); ++i) {
        const bool ctrl = (report[0] & (kModLCtrl | kModRCtrl)) != 0;
        const u8* slots = report + 2;
        u8 chars[KeyDecoder::kKeySlots * KeyDecoder::kMaxBytesPerKey];
        const usize count = typed_decoder.feed(report, chars, sizeof(chars));
        if (ctrl) {
            for (usize s = 0; s < KeyDecoder::kKeySlots; ++s) {
                const u8 usage = slots[s];
                if (usage == 0 || slots_have_key(prev_key_slots, usage)) {
                    continue;
                }
                if (usage == usage_key_c) {
                    exec.signal_current_task(sigbreakf_ctrl_c);
                } else if (usage == usage_key_backslash_us ||
                           usage == usage_key_backslash_iso ||
                           keymap_se(usage, report[0], false) == '\\') {
                    key_queue_push(ctrl_backslash);
                }
            }
        } else {
            for (usize k = 0; k < count; ++k) {
                handle_decoded_byte(chars[k], nullptr);
            }
        }
        for (usize s = 0; s < KeyDecoder::kKeySlots; ++s) {
            prev_key_slots[s] = slots[s];
        }
    }
}

bool poll_input_while_running() {
    poll_mouse();
    poll_keys_while_running();
    idle_wait();
    // Without a keyboard nothing could stop a blocked program: let the run end.
    return usb_keyboard_status() == UsbStatus::Ready;
}

bool hook_trampoline(void*) {
    intuition.flush_palette();
    return poll_input_while_running();
}

constexpr bool is_console_printable(u8 c) {
    return (c >= 0x20 && c <= 0x7e) || c >= 0xa0;
}

u32 console_read(void*, u8* buf, u32 size) {
    if (buf == nullptr || size == 0) {
        return 0;
    }
    shell.begin_input();
    u32 len = 0;
    for (;;) {
        u8 ch = 0;
        if (!key_queue_pop(ch)) {
            if (usb_keyboard_status() != UsbStatus::Ready) {
                if (len > 0) {
                    shell.end_line();
                }
                return 0;
            }
            poll_input_while_running();
            continue;
        }
        if (ch == ctrl_backslash) {
            shell.end_line();
            return 0;
        }
        if (ch == '\n') {
            shell.end_line();
            buf[len++] = '\n';
            return len;
        }
        if (ch == '\b') {
            shell.scroll_to_bottom();
            if (len > 0) {
                --len;
                shell.erase_last();
            }
            continue;
        }
        if (is_console_printable(ch)) {
            shell.scroll_to_bottom();
            if (len + 1 < size && len < Shell::kMaxLine) {
                buf[len++] = ch;
                shell.echo(ch);
            }
        }
    }
}

bool console_ready(void*, u32 timeout_us) {
    if (key_queue_count > 0) {
        return true;
    }
    if (usb_keyboard_status() != UsbStatus::Ready) {
        return false;
    }
    poll_mouse();
    poll_keys_while_running();
    if (key_queue_count > 0) {
        return true;
    }
    if (timeout_us == 0 || clock_us() == 0) {
        return false;
    }
    const Deadline deadline = deadline_in_us(timeout_us);
    while (!expired(deadline)) {
        if (!poll_input_while_running()) {
            return key_queue_count > 0;
        }
        if (key_queue_count > 0) {
            return true;
        }
    }
    return false;
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
    exec.set_idle_hook(IdleHook{&hook_trampoline, nullptr});
    if (!utility.init(machine, exec)) {
        fail("utility init failed");
        return false;
    }
    if (!dos.init(machine, exec, volume, &console_write, nullptr)) {
        fail("dos init failed");
        return false;
    }
    dos.set_console_input(&console_read, &console_ready, nullptr);
    start_dos_clock();
    machine.cpu.a[7] = boot_stack;
    open_amiga_screen(info);
    return true;
}


// What a finished program may have left behind: its windows and screens, the
// messages it never replied to and the signals it allocated.
void reclaim_after_program() {
    if (amiga_screen != 0) {
        intuition.reclaim(shell.window(), amiga_screen);
    }
    exec.reset_task_signals();
}

// Runs the command and reports on serial how it went. False when the
// program cannot be loaded; any other end is reported and counts as run.
bool execute(const char* path, const char* args) {
    serial::write("m68k: running ");
    write_filtered(path);
    serial::write("\n");

    machine.cpu.exit = ExitReason::None;
    u32 result = 0;
    command_running = true;
    const bool ok = dos.run_command(path, args, result, block_budget);
    command_running = false;
    console_filter.reset();
    reclaim_after_program();
    intuition.flush_palette();
    if (!console_at_line_start) {
        serial::write_char('\n');
        console_at_line_start = true;
    }

    const m68k::CpuState& cpu = machine.cpu;
    serial::write("m68k: ");
    bool loaded = true;
    if (ok) {
        write_filtered(path);
        serial::write(" returned ");
        serial::write_dec(result);
    } else if (cpu.exit == ExitReason::None) {
        // run_command fails without running anything only when it cannot
        // load the program.
        serial::write("could not load ");
        write_filtered(path);
        loaded = false;
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
    return loaded;
}

// The shell's view of the window, the log and the commands.
struct TypedShell {
    ShellCommands* commands;
};

void shell_print(void*, const char* text) {
    if (text == nullptr) {
        return;
    }
    u32 len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    console_write(nullptr, reinterpret_cast<const u8*>(text), len);
}

void write_shell_prompt() {
    char buf[260];
    format_prompt(dos, 1, buf, sizeof buf);
    shell.write_prompt_text(buf);
}

void typed_echo(void*, u8 ch) { shell.echo(ch); }
void typed_erase(void*) { shell.erase_last(); }
void typed_cursor_back(void*, u32 back) { shell.set_cursor_back(back); }
void typed_end_line(void*) { shell.end_line(); }
void typed_message(void*, const char* text) { shell.print_line(text); }
void typed_prompt(void*) { write_shell_prompt(); }
void typed_scroll_to_bottom(void*) { shell.scroll_to_bottom(); }
void typed_log(void*, const char* text) {
    write_filtered(text);
    serial::write_char('\n');
}

bool typed_builtin(void* ctx, const char* word, const char* args) {
    ShellCommands* cmds = static_cast<TypedShell*>(ctx)->commands;
    if (cmds == nullptr || !cmds->run(word, args)) {
        return false;
    }
    if (!console_at_line_start) {
        serial::write_char('\n');
        console_at_line_start = true;
    }
    serial::write("shell: builtin ");
    write_filtered(word);
    serial::write(" returned ");
    serial::write_dec(dos.last_return_code());
    serial::write("\n");
    return true;
}

// A typed command may fault like any other; the next one starts clean.
// Keys typed while it ran and not read by it stay queued, and the shell
// loop gives them to the line editor afterwards, as type-ahead.
bool typed_run(void*, const char* path, const char* args) {
    const char* word = path;
    if (word[0] == 'C' && word[1] == ':') {
        word += 2;
    }
    char cmd[Dos::kMaxPath];
    if (!dos.find_command(word, cmd, sizeof cmd)) {
        return false;
    }
    const bool loaded = execute(cmd, args);
    if (loaded && (machine.cpu.exit == ExitReason::None ||
                   machine.cpu.exit == ExitReason::Returned)) {
        const u32 rc = dos.last_return_code();
        if (rc != 0 && rc >= dos.fail_level()) {
            if (!console_at_line_start) {
                console_write(nullptr, reinterpret_cast<const u8*>("\n"), 1);
            }
            char digits[16];
            format_u64(digits, sizeof digits, rc, 10);
            shell_print(nullptr, word);
            shell_print(nullptr, " failed returncode ");
            shell_print(nullptr, digits);
            shell_print(nullptr, "\n");
        }
    }
    return loaded;
}

// Reads the keyboard for good: keys go to the line editor, Enter runs the
// line. Never returns; a lost keyboard leaves the window as it is.
[[noreturn]] void run_shell_loop() {
    static constinit ShellCommands commands;
    commands.init(dos, &shell_print, nullptr);
    static constinit TypedShell typed{};
    typed.commands = &commands;
    static constinit Shell line_editor;
    line_editor.init(ShellHost{&typed_echo, &typed_erase, &typed_end_line, &typed_run,
                               &typed_message, &typed_prompt, &typed_log, &typed,
                               &typed_builtin, &typed_scroll_to_bottom,
                               &typed_cursor_back});
    serial::write("shell: ready\n");
    while (usb_keyboard_status() == UsbStatus::Ready) {
        intuition.flush_palette();
        poll_mouse();
        u8 queued = 0;
        while (key_queue_pop(queued)) {
            if (queued != ctrl_backslash) {
                line_editor.key(queued);
            }
        }
        u8 report[KeyDecoder::kReportBytes];
        if (usb_keyboard_poll(report)) {
            u8 chars[KeyDecoder::kKeySlots * KeyDecoder::kMaxBytesPerKey];
            const usize count = typed_decoder.feed(report, chars, sizeof(chars));
            for (usize s = 0; s < KeyDecoder::kKeySlots; ++s) {
                prev_key_slots[s] = report[2 + s];
            }
            for (usize i = 0; i < count; ++i) {
                handle_decoded_byte(chars[i], &line_editor);
            }
        }
        idle_wait();
    }
    shell.print_line(usb_keyboard_result().message);
    for (;;) {
        asm volatile("cli; hlt");
    }
}

}

void m68k_run_command(const BootInfo& info, PhysAllocator& pages, const AdfVolume& volume,
                      const char* path, const char* args) {
    if (!start(info, pages, volume)) {
        return;
    }
    if (amiga_screen != 0) {
        if (shell.open(machine, exec, graphics, intuition, display)) {
            shell.set_view_listener(&on_shell_view, nullptr);
            serial::write("intuition: window opened HombreShell\n");
            show_amiga_screen(info);
        } else {
            // The kernel's text console stays as the display.
            fail("could not open the shell window");
            intuition.close_screen(amiga_screen);
            amiga_screen = 0;
        }
    }
    execute(path, args);
    key_queue_clear();
    if (shell.is_open()) {
        write_shell_prompt();
        if (usb_keyboard_status() == UsbStatus::Ready) {
            run_shell_loop();
        }
        // No keyboard: the window says why, and the boot ends as it always did.
        shell.print_line(usb_keyboard_result().message);
    }
}

}
