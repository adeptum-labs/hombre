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
#include "../common/libs/gadtools.hpp"
#include "../common/libs/icon.hpp"
#include "../common/libs/layers.hpp"
#include "../common/libs/version_library.hpp"
#include "../common/libs/intuition.hpp"
#include "../common/libs/native.hpp"
#include "../common/libs/timer_device.hpp"
#include "../common/libs/utility.hpp"
#include "../common/libs/workbench.hpp"
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
#include "task_switch.hpp"
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
constinit TimerDevice timer_device{};
constinit TaskStacks task_stacks{};
constinit Utility utility{};
constinit Dos dos{};
constinit Graphics graphics{};
constinit Intuition intuition{};
constinit GadTools gadtools{};
constinit Layers layers{};
constinit VersionLibrary version_library{};
constinit Icon icon{};
constinit Workbench workbench{};
constinit DisplayState display{};
constinit AmigaDisplay amiga_display{};
constinit PointerState pointer{};
// The default Amiga screen; 0 until it is open.
constinit u32 amiga_screen = 0;
// The key slots of the last keyboard report seen.
constinit u8 prev_key_slots[KeyDecoder::kKeySlots]{};
// The one keyboard's decoder.
constinit KeyDecoder typed_decoder{};

struct Console;

// What a shell's line editor callbacks look up from their context.
struct TypedShell {
    Console* console = nullptr;
};

// One shell: its window, its console callbacks and buffers, its command
// process, and the line editor and builtins that run on it.
struct Console {
    ShellWindow shell;
    CsiFilter filter;
    u8 key_queue[key_queue_cap]{};
    u32 key_head = 0;
    u32 key_count = 0;
    bool mid_line = false;
    bool command_running = false;
    bool line_mode = false;
    bool input_wait = false;
    bool open = false;
    u8 index = 0;
    u32 process = 0;
    u32 cli = 0;
    Shell line_editor;
    ShellCommands commands;
    TypedShell typed{};
};

constinit Console consoles[Dos::kMaxConsoles]{};

// The console the last keyboard report went to.
constinit u8 last_active_console = 0;

// Whether what reached the serial log so far ends a line.
constinit bool serial_at_line_start = true;

void write_filtered(const char* text) {
    for (; *text != '\0'; ++text) {
        serial::write_char(printable_char(static_cast<u8>(*text)));
    }
}

// The native libraries' diagnostics, one line each. They can carry names
// from the guest, so they go through the printable filter.
void log_line(const char* line) {
    if (!serial_at_line_start) {
        serial::write_char('\n');
        serial_at_line_start = true;
    }
    write_filtered(line);
    serial::write_char('\n');
}

// Reads the keyboard and gives what it decodes to the console of the active
// window.
void poll_keys();

// What a program writes to a console. Newlines and tabs are kept.
void console_write(void* ctx, const u8* data, u32 bytes) {
    Console& c = *static_cast<Console*>(ctx);
    for (u32 i = 0; i < bytes; ++i) {
        const u8 ch = data[i];
        serial::write_char(ch == '\n' || ch == '\t' ? static_cast<char>(ch) : printable_char(ch));
        serial_at_line_start = ch == '\n';
        c.mid_line = ch != '\n';
    }
    if (c.shell.is_open()) {
        c.shell.write(data, bytes);
    }
    if (c.command_running) {
        poll_keys();
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
        !intuition.init(machine, exec, graphics, display) ||
        !gadtools.init(machine, exec, graphics)) {
        fail("graphics init failed");
        return;
    }
    intuition.set_clock(&clock_us);
    intuition.set_gadget_renderer(&GadTools::render_gadget, &gadtools);
    intuition.set_gadget_release(&GadTools::release_gadget);
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
    intuition.set_default_screen(amiga_screen);
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
    if (!serial_at_line_start) {
        serial::write_char('\n');
        serial_at_line_start = true;
    }
    serial::write("shell: view ");
    serial::write_dec(view);
    serial::write("\n");
}

// The AmigaDOS time: the real-time clock read once, advanced by the TSC
// clock. Without a readable clock DateStamp stays at the start of 1978.
u64 dos_now_ticks() {
    return timer_device.sys_time_us() / 20000;
}

void start_dos_clock() {
    CivilTime now{};
    if (!rtc_read(now)) {
        serial::write("clock: no real-time clock\n");
        return;
    }
    const DosStamp stamp = dos_stamp_from_civil(now);
    const u64 boot_dos_ticks =
        (static_cast<u64>(stamp.days) * 1440 + stamp.minutes) * 3000 + stamp.ticks;
    const u64 boot_clock_us = clock_us();
    timer_device.set_sys_time_us(boot_dos_ticks * 20000, boot_clock_us);
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
    const u32 win = intuition.window_under(pointer.x(), pointer.y());
    for (Console& c : consoles) {
        if (win != 0 && c.open && c.shell.is_open() && c.shell.window() == win) {
            c.shell.scroll_by(3 * wheel);
            return;
        }
    }
    intuition.wheel_event(wheel);
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

// The console of the active window: its shell's console, or the console
// whose process opened the window; the last console keys went to when there
// is no such console.
Console& active_console() {
    const u32 window = intuition.active_window();
    if (window != 0) {
        for (Console& c : consoles) {
            if (c.open && c.shell.window() == window) {
                return c;
            }
        }
        const u32 owner = intuition.window_owner(window);
        for (Console& c : consoles) {
            if (owner != 0 && c.open && c.process == owner) {
                return c;
            }
        }
    }
    Console& last = consoles[last_active_console];
    return last.open ? last : consoles[0];
}

// True when a Ready task wants the CPU rather than a key.
bool other_task_ready() {
    const Scheduler& sched = exec.scheduler();
    for (u32 i = 0; i < Scheduler::kMaxTasks; ++i) {
        const Scheduler::Slot& slot = sched.slot(i);
        if (slot.state != TaskState::Ready) {
            continue;
        }
        bool waiting_for_a_key = false;
        for (const Console& c : consoles) {
            if (c.open && (c.line_mode || c.input_wait) && slot.task_addr == c.process) {
                waiting_for_a_key = true;
                break;
            }
        }
        if (!waiting_for_a_key) {
            return true;
        }
    }
    return false;
}

// Gives other tasks the CPU while one wants it. With only shells waiting at
// their prompts left, one yield lets another shell run before this task
// halts, so two idle shells do not switch to each other forever.
// The boot shell opens over the whole screen. Once the Workbench desktop
// is up, it moves to the lower part of the screen, so the volume icons and
// drawer windows are in view.
void make_room_for_workbench(Console& c) {
    static constinit bool moved = false;
    if (moved || c.index != 0 || !workbench.desktop_open() || !c.shell.is_open()) {
        return;
    }
    moved = true;
    // The Workbench screen is 640 x 400.
    constexpr i32 kScreenWidth = 640;
    constexpr i32 kScreenHeight = 400;
    constexpr i32 kHeight = kScreenHeight / 3;
    const u32 window = c.shell.window();
    intuition.size_window_to(window, kScreenWidth - 80, kHeight);
    intuition.move_window_to(window, 0, kScreenHeight - kHeight);
}

void wait_for_work(const Console& c) {
    if (c.key_count > 0) {
        return;
    }
    if (exec.yield_in_native() && (c.key_count > 0 || other_task_ready())) {
        return;
    }
    idle_wait();
}

void key_queue_clear(Console& c) {
    c.key_head = 0;
    c.key_count = 0;
}

void key_queue_push(Console& c, u8 ch) {
    if (c.key_count >= key_queue_cap) {
        return;
    }
    c.key_queue[(c.key_head + c.key_count) % key_queue_cap] = ch;
    ++c.key_count;
}

bool key_queue_pop(Console& c, u8& out) {
    if (c.key_count == 0) {
        return false;
    }
    out = c.key_queue[c.key_head];
    c.key_head = (c.key_head + 1) % key_queue_cap;
    --c.key_count;
    return true;
}

// A filtered keyboard byte: bytes wait in the console's queue for its shell
// loop or its console input, while scrolling and line editing act at once.
void handle_decoded_byte(Console& c, u8 raw) {
    u8 out = 0;
    const ConsoleKey key = c.filter.feed(raw, out);
    switch (key) {
    case ConsoleKey::Byte:
        key_queue_push(c, out);
        break;
    case ConsoleKey::RowUp:
        c.shell.scroll_by(1);
        break;
    case ConsoleKey::RowDown:
        c.shell.scroll_by(-1);
        break;
    case ConsoleKey::PageUp: {
        const u32 rows = c.shell.rows();
        const i32 step = rows > 1 ? static_cast<i32>(rows - 1) : 0;
        c.shell.scroll_by(step);
        break;
    }
    case ConsoleKey::PageDown: {
        const u32 rows = c.shell.rows();
        const i32 step = rows > 1 ? static_cast<i32>(rows - 1) : 0;
        c.shell.scroll_by(-step);
        break;
    }
    case ConsoleKey::Top:
        c.shell.scroll_to_top();
        break;
    case ConsoleKey::Bottom:
        c.shell.scroll_to_bottom();
        break;
    case ConsoleKey::HistoryUp:
        if (c.line_mode) {
            c.line_editor.history_up();
        }
        break;
    case ConsoleKey::HistoryDown:
        if (c.line_mode) {
            c.line_editor.history_down();
        }
        break;
    case ConsoleKey::CursorLeft:
        if (c.line_mode) {
            c.line_editor.cursor_left();
        }
        break;
    case ConsoleKey::CursorRight:
        if (c.line_mode) {
            c.line_editor.cursor_right();
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

// Reads the keyboard and routes each decoded byte to the active console:
// Ctrl+C signals that console's shell process, Ctrl+\ ends its console
// input, and every other byte waits for the console's reader or line editor.
void poll_keys() {
    if (usb_keyboard_status() != UsbStatus::Ready) {
        return;
    }
    u8 report[KeyDecoder::kReportBytes];
    for (u32 i = 0; i < poll_budget && usb_keyboard_poll(report); ++i) {
        Console& c = active_console();
        last_active_console = c.index;
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
                    if (c.command_running) {
                        exec.signal_task(c.process, sigbreakf_ctrl_c);
                    }
                } else if (usage == usage_key_backslash_us ||
                           usage == usage_key_backslash_iso ||
                           keymap_se(usage, report[0], false) == '\\') {
                    key_queue_push(c, ctrl_backslash);
                }
            }
        } else {
            for (usize k = 0; k < count; ++k) {
                handle_decoded_byte(c, chars[k]);
            }
        }
        for (usize s = 0; s < KeyDecoder::kKeySlots; ++s) {
            prev_key_slots[s] = slots[s];
        }
    }
}

void task_host_poll(void*) {
    intuition.flush_palette();
    poll_mouse();
    poll_keys();
}

bool hook_trampoline(void*) {
    task_host_poll(nullptr);
    idle_wait();
    // Without a keyboard nothing could stop a blocked program: let the run end.
    return usb_keyboard_status() == UsbStatus::Ready;
}

void task_entry(void* arg) {
    const u32 id = static_cast<u32>(reinterpret_cast<usize>(arg));
    exec.task_main(id);
}

bool task_host_create(void*, u32 id) {
    return task_stacks.create(id, &task_entry, reinterpret_cast<void*>(static_cast<usize>(id)));
}

void task_host_switch(void*, u32 from, u32 to) {
    task_stacks.switch_to(from, to);
}

void task_host_destroy(void*, u32 id) {
    task_stacks.destroy(id);
}

bool task_host_slice_over(void*) {
    return take_need_resched();
}

u64 task_host_now_us(void*) {
    return clock_us();
}

constexpr bool is_console_printable(u8 c) {
    return (c >= 0x20 && c <= 0x7e) || c >= 0xa0;
}

u32 console_read(void* ctx, u8* buf, u32 size) {
    Console& c = *static_cast<Console*>(ctx);
    if (buf == nullptr || size == 0) {
        return 0;
    }
    c.shell.begin_input();
    u32 len = 0;
    for (;;) {
        u8 ch = 0;
        if (!key_queue_pop(c, ch)) {
            if (usb_keyboard_status() != UsbStatus::Ready) {
                if (len > 0) {
                    c.shell.end_line();
                }
                return 0;
            }
            task_host_poll(nullptr);
            c.input_wait = true;
            wait_for_work(c);
            c.input_wait = false;
            continue;
        }
        if (ch == ctrl_backslash) {
            c.shell.end_line();
            return 0;
        }
        if (ch == '\n') {
            c.shell.end_line();
            buf[len++] = '\n';
            return len;
        }
        if (ch == '\b') {
            c.shell.scroll_to_bottom();
            if (len > 0) {
                --len;
                c.shell.erase_last();
            }
            continue;
        }
        if (is_console_printable(ch)) {
            c.shell.scroll_to_bottom();
            if (len + 1 < size && len < Shell::kMaxLine) {
                buf[len++] = ch;
                c.shell.echo(ch);
            }
        }
    }
}

bool console_ready(void* ctx, u32 timeout_us) {
    Console& c = *static_cast<Console*>(ctx);
    if (c.key_count > 0) {
        return true;
    }
    if (usb_keyboard_status() != UsbStatus::Ready) {
        return false;
    }
    poll_mouse();
    poll_keys();
    if (c.key_count > 0) {
        return true;
    }
    if (timeout_us == 0 || clock_us() == 0) {
        return false;
    }
    const Deadline deadline = deadline_in_us(timeout_us);
    while (!expired(deadline)) {
        task_host_poll(nullptr);
        if (c.key_count > 0) {
            return true;
        }
        if (usb_keyboard_status() != UsbStatus::Ready) {
            return false;
        }
        c.input_wait = true;
        wait_for_work(c);
        c.input_wait = false;
    }
    return false;
}

// Registers a console's output and input with dos.
void register_console(u8 index) {
    dos.set_console(index, &console_write, &console_read, &console_ready, &consoles[index]);
}

// Prepares guest memory, the translator, exec, utility and dos. Logs one
// line and returns false when a step fails.
bool start(const BootInfo& info, PhysAllocator& pages, const Drives& drives) {
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
    if (!task_stacks.init_kernel(pages)) {
        fail("task stack window init failed");
        return false;
    }
    exec.set_task_host(TaskHost{&task_host_create, &task_host_switch, &task_host_destroy,
                                &task_host_slice_over, &task_host_poll, &hook_trampoline,
                                &task_host_now_us, nullptr});
    if (!exec.init(machine, free_start, m68k::kGuestBytes)) {
        fail("exec init failed");
        return false;
    }
    exec.set_idle_hook(IdleHook{&hook_trampoline, nullptr});
    if (!timer_device.init(machine, exec)) {
        fail("timer init failed");
        return false;
    }
    if (!utility.init(machine, exec)) {
        fail("utility init failed");
        return false;
    }
    if (!layers.init(machine, exec)) {
        fail("layers init failed");
        return false;
    }
    if (!version_library.init(machine, exec)) {
        fail("version init failed");
        return false;
    }
    if (!dos.init(machine, exec, drives, &console_write, &consoles[0])) {
        fail("dos init failed");
        return false;
    }
    if (!icon.init(machine, exec, dos)) {
        fail("icon init failed");
        return false;
    }
    if (!workbench.init(machine, exec, dos, graphics, intuition, gadtools, icon)) {
        fail("workbench init failed");
        return false;
    }
    consoles[0].index = 0;
    consoles[0].process = dos.process();
    consoles[0].cli = 1;
    consoles[0].open = true;
    register_console(0);
    start_dos_clock();
    machine.cpu.a[7] = boot_stack;
    open_amiga_screen(info);
    return true;
}


// What a finished program may have left behind: its windows and screens, the
// messages it never replied to and the signals it allocated. Ending child
// tasks and resetting signals belongs to the boot task; a shell's command
// runs on the shell's own task.
void reclaim_after_program(u32 task) {
    if (exec.this_task() == dos.process()) {
        exec.end_child_tasks();
        exec.reset_nest_counts();
        exec.reset_task_signals();
    }
    if (amiga_screen != 0) {
        u32 keep[Dos::kMaxConsoles]{};
        u32 count = 0;
        for (Console& c : consoles) {
            if (c.shell.is_open()) {
                keep[count++] = c.shell.window();
            }
        }
        intuition.reclaim_owned_by(task, keep, count, amiga_screen);
    }
}

// A Ctrl+C pressed at a prompt belongs to no command.
void clear_ctrl_c(u32 process) {
    u32 recvd = 0;
    if (process != 0 && machine.mem.read32(process + ndk::TC_SIGRECVD, recvd)) {
        machine.mem.write32(process + ndk::TC_SIGRECVD, recvd & ~sigbreakf_ctrl_c);
    }
}

// Runs the command and reports on serial how it went. False when the
// program cannot be loaded; any other end is reported and counts as run.
bool execute(Console& c, const char* path, const char* args) {
    serial::write("m68k: running ");
    write_filtered(path);
    serial::write("\n");

    machine.cpu.exit = ExitReason::None;
    u32 result = 0;
    clear_ctrl_c(c.process);
    c.command_running = true;
    const bool ok = dos.run_command_in(c.process, path, args, result, block_budget);
    c.command_running = false;
    c.filter.reset();
    reclaim_after_program(c.process);
    intuition.flush_palette();
    if (!serial_at_line_start) {
        serial::write_char('\n');
        serial_at_line_start = true;
    }

    const m68k::CpuState& cpu = machine.cpu;
    serial::write("m68k: ");
    bool loaded = true;
    if (ok) {
        write_filtered(path);
        serial::write(" returned ");
        serial::write_dec(result);
    } else if (cpu.exit == ExitReason::None) {
        // run_command_in fails without running anything only when it cannot
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
    serial_at_line_start = true;
    return loaded;
}

Console& typed_console(void* ctx) { return *static_cast<TypedShell*>(ctx)->console; }

void shell_print(void* ctx, const char* text) {
    if (text == nullptr) {
        return;
    }
    Console& c = typed_console(ctx);
    u32 len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    console_write(&c, reinterpret_cast<const u8*>(text), len);
}

void write_shell_prompt(Console& c) {
    char buf[260];
    format_prompt(dos, c.cli, buf, sizeof buf, c.process);
    c.shell.write_prompt_text(buf);
}

void typed_echo(void* ctx, u8 ch) { typed_console(ctx).shell.echo(ch); }
void typed_erase(void* ctx) { typed_console(ctx).shell.erase_last(); }
void typed_cursor_back(void* ctx, u32 back) {
    typed_console(ctx).shell.set_cursor_back(back);
}
void typed_end_line(void* ctx) { typed_console(ctx).shell.end_line(); }
void typed_message(void* ctx, const char* text) { typed_console(ctx).shell.print_line(text); }
void typed_prompt(void* ctx) { write_shell_prompt(typed_console(ctx)); }
void typed_scroll_to_bottom(void* ctx) { typed_console(ctx).shell.scroll_to_bottom(); }
void typed_log(void*, const char* text) {
    write_filtered(text);
    serial::write_char('\n');
}

bool typed_builtin(void* ctx, const char* word, const char* args) {
    Console& c = typed_console(ctx);
    if (!c.commands.run(word, args)) {
        return false;
    }
    if (!serial_at_line_start) {
        serial::write_char('\n');
        serial_at_line_start = true;
    }
    serial::write("shell: builtin ");
    write_filtered(word);
    serial::write(" returned ");
    serial::write_dec(dos.last_return_code(c.process));
    serial::write("\n");
    return true;
}

// A typed command may fault like any other; the next one starts clean.
// Keys typed while it ran and not read by it stay queued, and the shell
// loop gives them to the line editor afterwards, as type-ahead.
bool typed_run(void* ctx, const char* path, const char* args) {
    Console& c = typed_console(ctx);
    const char* word = path;
    if (word[0] == 'C' && word[1] == ':') {
        word += 2;
    }
    char cmd[Dos::kMaxPath];
    if (!dos.find_command(word, cmd, sizeof cmd, c.process)) {
        return false;
    }
    const bool loaded = execute(c, cmd, args);
    if (loaded && (machine.cpu.exit == ExitReason::None ||
                   machine.cpu.exit == ExitReason::Returned)) {
        const u32 rc = dos.last_return_code(c.process);
        if (rc != 0 && rc >= dos.fail_level(c.process)) {
            if (c.mid_line) {
                console_write(&c, reinterpret_cast<const u8*>("\n"), 1);
            }
            char digits[16];
            format_u64(digits, sizeof digits, rc, 10);
            shell_print(&c.typed, word);
            shell_print(&c.typed, " failed returncode ");
            shell_print(&c.typed, digits);
            shell_print(&c.typed, "\n");
        }
    }
    return loaded;
}

constexpr u32 new_shell_width = 480;
constexpr u32 new_shell_height = 240;

void log_new_shell(u32 cli) {
    if (!serial_at_line_start) {
        serial::write_char('\n');
        serial_at_line_start = true;
    }
    serial::write("shell: new shell ");
    serial::write_dec(cli);
    serial::write("\n");
}

void shell_loop(Console& c);
void shell_task(void* arg);

// Opens the console's window and starts its shell as a native task.
bool open_new_shell(void* ctx) {
    (void)ctx;
    u32 index = Dos::kMaxConsoles;
    for (u32 i = 1; i < Dos::kMaxConsoles; ++i) {
        if (!consoles[i].open) {
            index = i;
            break;
        }
    }
    if (index == Dos::kMaxConsoles) {
        return false;
    }
    Console& c = consoles[index];
    c.index = static_cast<u8>(index);
    c.process = 0;
    c.cli = 0;
    c.command_running = false;
    c.line_mode = false;
    c.input_wait = false;
    c.open = false;
    c.mid_line = false;
    c.filter.reset();
    key_queue_clear(c);
    const i32 offset = static_cast<i32>(index - 1) * 20;
    if (!c.shell.open(machine, exec, graphics, intuition, 60 + offset, 60 + offset,
                      new_shell_width, new_shell_height)) {
        return false;
    }
    register_console(static_cast<u8>(index));
    u32 process = 0;
    u32 cli = 0;
    if (!dos.create_shell_process(static_cast<u8>(index), process, cli)) {
        c.shell.close();
        return false;
    }
    c.process = process;
    c.cli = cli;
    c.open = true;
    log_new_shell(cli);
    if (exec.spawn_native(process, &shell_task, &c, 0, true) == 0) {
        c.shell.close();
        c.open = false;
        c.process = 0;
        c.cli = 0;
        log_line("shell: cannot start new shell");
        return false;
    }
    return true;
}

// Reads the keyboard for good: keys go to the line editor, Enter runs the
// line. Console 0 never returns; another console returns when its shell
// ends and leaves its console free for a new shell.
void shell_loop(Console& c) {
    c.commands.init(dos, &shell_print, &c.typed);
    c.commands.set_process(c.process);
    c.commands.set_new_shell(&open_new_shell, &c);
    c.typed.console = &c;
    c.line_editor.init(ShellHost{&typed_echo, &typed_erase, &typed_end_line, &typed_run,
                                 &typed_message, &typed_prompt, &typed_log, &c.typed,
                                 &typed_builtin, &typed_scroll_to_bottom,
                                 &typed_cursor_back});
    if (c.index == 0) {
        serial::write("shell: ready\n");
    } else {
        write_shell_prompt(c);
    }
    c.line_mode = true;
    while (usb_keyboard_status() == UsbStatus::Ready &&
           (c.index == 0 || c.shell.is_open())) {
        intuition.flush_palette();
        poll_mouse();
        poll_keys();
        u8 queued = 0;
        while (key_queue_pop(c, queued)) {
            if (queued == ctrl_backslash) {
                continue;
            }
            if (queued == '\n') {
                c.line_mode = false;
                c.line_editor.key(queued);
                c.line_mode = true;
            } else {
                c.line_editor.key(queued);
            }
            if (c.commands.ended()) {
                break;
            }
        }
        if (c.commands.ended()) {
            break;
        }
        make_room_for_workbench(c);
        wait_for_work(c);
    }
    c.line_mode = false;
    if (c.index == 0) {
        c.shell.print_line(usb_keyboard_result().message);
        for (;;) {
            asm volatile("cli; hlt");
        }
    }
    c.shell.close();
    c.open = false;
    c.command_running = false;
    c.input_wait = false;
    c.process = 0;
    c.cli = 0;
    c.filter.reset();
    key_queue_clear(c);
}

void shell_task(void* arg) { shell_loop(*static_cast<Console*>(arg)); }

}

constexpr char kAddDataTypes[] = "C:AddDataTypes";

void m68k_run_command(const BootInfo& info, PhysAllocator& pages, const Drives& drives,
                      const char* path, const char* args) {
    if (!start(info, pages, drives)) {
        return;
    }
    Console& boot = consoles[0];
    if (amiga_screen != 0) {
        if (boot.shell.open(machine, exec, graphics, intuition)) {
            boot.shell.set_view_listener(&on_shell_view, nullptr);
            serial::write("intuition: window opened HombreShell\n");
            show_amiga_screen(info);
        } else {
            // The kernel's text console stays as the display.
            fail("could not open the shell window");
            intuition.close_screen(amiga_screen);
            amiga_screen = 0;
        }
    }
    execute(boot, path, args);
    // The datatypes on the boot disk, as its Startup-Sequence adds them:
    // datatypes.library opens only once they are listed.
    if (dos.examine(kAddDataTypes).kind == Dos::TargetKind::File) {
        execute(boot, kAddDataTypes, "REFRESH QUIET");
    }
    key_queue_clear(boot);
    if (boot.shell.is_open()) {
        write_shell_prompt(boot);
        if (usb_keyboard_status() == UsbStatus::Ready) {
            shell_loop(boot);
        }
        // No keyboard: the window says why, and the boot ends as it always did.
        boot.shell.print_line(usb_keyboard_result().message);
    }
}

}
