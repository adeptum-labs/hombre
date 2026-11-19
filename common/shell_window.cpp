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

#include "shell_window.hpp"

#include "libs/exec.hpp"
#include "libs/graphics.hpp"
#include "libs/intuition.hpp"

namespace hombre {

using m68k::Machine;
using native::Bus;
using i16 = __INT16_TYPE__;

namespace {

constexpr char kTitle[] = "HombreShell";
constexpr u32 kMaxWrite = 65536;
constexpr u32 kCell = 8;
constexpr u32 kBaseline = 6;
constexpr u32 kMinWidth = 160;
constexpr u32 kMinHeight = 50;
constexpr u32 kMaxSize = 0xFFFF;
constexpr u32 kMaxRows = 4096 / kCell;
constexpr i32 kScrollWidth = 8;
constexpr u32 kPropFlags = ndk::AUTOKNOB | ndk::FREEVERT | ndk::PROPNEWLOOK;
constexpr u32 kGadgetFlags = ndk::GFLG_RELRIGHT | ndk::GFLG_RELHEIGHT;
constexpr u32 kGadgetActivation = ndk::GACT_RIGHTBORDER | ndk::GACT_IMMEDIATE |
                                  ndk::GACT_FOLLOWMOUSE | ndk::GACT_RELVERIFY;

// Printable Latin-1: the ASCII range and the codes above the control block.
constexpr bool is_printable(u8 c) {
    return (c >= 0x20 && c <= 0x7e) || c >= 0xa0;
}

}

bool ShellWindow::open(Machine& m, Exec& exec, Graphics& gfx, Intuition& intuition,
                       DisplayState& display, i32 left, i32 top, u32 window_width, u32 window_height) {
    close();
    if (display.front_screen == 0) {
        return false;
    }
    const u32 nw = exec.alloc(ndk::NewWindow_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    const u32 title = exec.alloc(sizeof kTitle, ndk::MEMF_PUBLIC);
    if (nw == 0 || title == 0) {
        if (nw != 0) {
            exec.free(nw, ndk::NewWindow_SIZEOF);
        }
        if (title != 0) {
            exec.free(title, sizeof kTitle);
        }
        return false;
    }
    Bus b(m);
    for (u32 i = 0; i < sizeof kTitle; ++i) {
        b.w8(title + i, static_cast<u8>(kTitle[i]));
    }
    b.w16(nw + ndk::nw_LeftEdge, static_cast<u16>(static_cast<i16>(left)));
    // Below the screen's title bar and the line under it when no top is given.
    const i32 top_edge =
        top >= 0 ? top : static_cast<i32>(b.r8(display.front_screen + ndk::sc_BarHeight) + 1);
    b.w16(nw + ndk::nw_TopEdge, static_cast<u16>(static_cast<i16>(top_edge)));
    b.w16(nw + ndk::nw_Width, static_cast<u16>(window_width));
    b.w16(nw + ndk::nw_Height, static_cast<u16>(window_height));
    b.w8(nw + ndk::nw_DetailPen, 0);
    b.w8(nw + ndk::nw_BlockPen, 1);
    b.w16(nw + ndk::nw_IDCMPFlags, 0);
    b.w32(nw + ndk::nw_Flags,
           ndk::WFLG_SIZEGADGET | ndk::WFLG_DEPTHGADGET | ndk::WFLG_CLOSEGADGET |
               ndk::WFLG_DRAGBAR | ndk::WFLG_ACTIVATE);
    b.w32(nw + ndk::nw_Title, title);
    b.w32(nw + ndk::nw_Screen, display.front_screen);
    b.w16(nw + ndk::nw_MinWidth, kMinWidth);
    b.w16(nw + ndk::nw_MinHeight, kMinHeight);
    b.w16(nw + ndk::nw_MaxWidth, kMaxSize);
    b.w16(nw + ndk::nw_MaxHeight, kMaxSize);
    b.w16(nw + ndk::nw_Type, ndk::CUSTOMSCREEN);
    if (!b.ok()) {
        exec.free(nw, ndk::NewWindow_SIZEOF);
        exec.free(title, sizeof kTitle);
        return false;
    }

    const u32 window = intuition.open_window(nw, 0);
    exec.free(nw, ndk::NewWindow_SIZEOF);
    exec.free(title, sizeof kTitle);
    if (window == 0) {
        return false;
    }
    const u32 rp = b.r32(window + ndk::wd_RPort);
    const i32 win_w = static_cast<i16>(b.r16(window + ndk::wd_Width));
    const i32 win_h = static_cast<i16>(b.r16(window + ndk::wd_Height));
    const i32 border_left = static_cast<i32>(b.r8(window + ndk::wd_BorderLeft));
    const i32 border_top = static_cast<i32>(b.r8(window + ndk::wd_BorderTop));
    const i32 border_right = static_cast<i32>(b.r8(window + ndk::wd_BorderRight));
    const i32 border_bottom = static_cast<i32>(b.r8(window + ndk::wd_BorderBottom));
    if (!b.ok() || rp == 0) {
        intuition.close_window(window);
        return false;
    }
    const i32 width = win_w - border_left - border_right;
    const i32 height = win_h - border_top - border_bottom;
    if (width <= 0 || height <= 0) {
        intuition.close_window(window);
        return false;
    }

    const u32 gadget = exec.alloc(ndk::Gadget_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    const u32 prop = exec.alloc(ndk::PropInfo_SIZEOF, ndk::MEMF_PUBLIC | ndk::MEMF_CLEAR);
    if (gadget == 0 || prop == 0) {
        if (gadget != 0) {
            exec.free(gadget, ndk::Gadget_SIZEOF);
        }
        if (prop != 0) {
            exec.free(prop, ndk::PropInfo_SIZEOF);
        }
        intuition.close_window(window);
        return false;
    }
    b.w16(prop + ndk::pi_Flags, kPropFlags);
    b.w16(prop + ndk::pi_HorizBody, ndk::MAXBODY);
    b.w16(prop + ndk::pi_VertBody, ndk::MAXBODY);
    b.w16(gadget + ndk::gg_LeftEdge, static_cast<u16>(static_cast<i16>(-(kScrollWidth + 1))));
    b.w16(gadget + ndk::gg_TopEdge, static_cast<u16>(border_top + 1));
    b.w16(gadget + ndk::gg_Width, static_cast<u16>(kScrollWidth));
    // Inside the right border with 1-px margins below the bar and above the size gadget.
    b.w16(gadget + ndk::gg_Height, static_cast<u16>(static_cast<i16>(-(border_top + 13))));
    b.w16(gadget + ndk::gg_Flags, kGadgetFlags);
    b.w16(gadget + ndk::gg_Activation, kGadgetActivation);
    b.w16(gadget + ndk::gg_GadgetType, ndk::GTYP_PROPGADGET);
    b.w32(gadget + ndk::gg_SpecialInfo, prop);
    if (!b.ok() || intuition.add_glist(window, gadget, -1, 1) == 0xFFFFFFFF) {
        exec.free(prop, ndk::PropInfo_SIZEOF);
        exec.free(gadget, ndk::Gadget_SIZEOF);
        intuition.close_window(window);
        return false;
    }

    machine_ = &m;
    exec_ = &exec;
    intuition_ = &intuition;
    graphics_ = &gfx;
    window_ = window;
    rp_ = rp;
    gadget_ = gadget;
    prop_info_ = prop;
    origin_x_ = border_left;
    origin_y_ = border_top;
    inner_w_ = width;
    inner_h_ = height;
    columns_ = static_cast<u32>(width) / kCell;
    rows_ = static_cast<u32>(height) / kCell;
    col_ = 0;
    row_ = 0;
    view_ = 0;
    cursor_visible_ = false;
    cursor_back_ = 0;
    text_.clear();
    mark_line_start();
    intuition.set_size_hook(window, &ShellWindow::on_size, this);
    intuition.set_gadget_hook(gadget, &ShellWindow::on_gadget, this);
    update_knob();
    gfx.set_pens(rp_, 1, 0, ndk::JAM2);
    return true;
}

bool ShellWindow::is_open() const {
    return window_ != 0 && intuition_ != nullptr && intuition_->window_open(window_);
}

void ShellWindow::write(const u8* data, u32 bytes) {
    if (!is_open() || data == nullptr || columns_ == 0 || rows_ == 0) {
        return;
    }
    if (bytes > kMaxWrite) {
        bytes = kMaxWrite;
    }
    cursor_visible_ = false;
    cursor_back_ = 0;
    const u32 old_total = text_.total_rows(columns_);
    intuition_->begin_batch();
    if (view_ > 0) {
        // The view stays on the same top line: remember it by its number,
        // which survives lines being dropped from the front.
        const u32 old_view = view_;
        const u32 old_top = old_total > rows_ + view_ ? old_total - rows_ - view_ : 0;
        u32 line = 0;
        u32 row_in_line = 0;
        text_.locate_row(columns_, old_top, line, row_in_line);
        const u32 top_number = text_.dropped() + line;
        for (u32 i = 0; i < bytes; ++i) {
            put_byte(data[i], false);
        }
        const u32 new_total = text_.total_rows(columns_);
        const u32 limit = new_total > rows_ ? new_total - rows_ : 0;
        bool redraw = false;
        u32 new_top = 0;
        if (top_number >= text_.dropped()) {
            new_top = text_.rows_before(columns_, top_number - text_.dropped()) + row_in_line;
        } else {
            redraw = true;  // the top line was dropped: show the oldest kept
        }
        if (new_top > limit) {
            new_top = limit;
            redraw = true;
        }
        view_ = limit - new_top;
        if (view_ == 0 || redraw) {
            redraw_viewport();
        }
        if (redraw || view_ != old_view || new_total != old_total) {
            update_knob();
        }
        intuition_->end_batch();
        return;
    }
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    for (u32 i = 0; i < bytes; ++i) {
        put_byte(data[i], true);
    }
    if (text_.total_rows(columns_) != old_total) {
        update_knob();
    }
    intuition_->end_batch();
}

void ShellWindow::write_prompt(u32 cli_number, const char* volume) {
    if (!is_open()) {
        return;
    }
    char prompt[kMaxPrompt + 16];
    u32 length = 0;
    char digits[12];
    u32 count = 0;
    if (cli_number == 0) {
        digits[count++] = '0';
    }
    while (cli_number != 0) {
        digits[count++] = static_cast<char>('0' + cli_number % 10);
        cli_number /= 10;
    }
    while (count != 0) {
        prompt[length++] = digits[--count];
    }
    prompt[length++] = '.';
    if (volume != nullptr) {
        const u32 cap = kMaxPrompt - 8;
        for (u32 i = 0; i < cap && volume[i] != '\0'; ++i) {
            prompt[length++] = volume[i];
        }
    }
    prompt[length++] = ':';
    prompt[length++] = '>';
    prompt[length++] = ' ';
    prompt[length] = '\0';
    write_prompt_text(prompt);
}

void ShellWindow::write_prompt_text(const char* text) {
    if (!is_open() || text == nullptr) {
        return;
    }
    intuition_->begin_batch();
    if (view_ > 0) {
        scroll_to_bottom();
    }
    const u32 old_total = text_.total_rows(columns_);
    if (col_ != 0) {
        text_.new_line();
        col_ = 0;
        advance_row();
    }
    u32 length = 0;
    while (length < kMaxWrite && text[length] != '\0') {
        ++length;
    }
    write(reinterpret_cast<const u8*>(text), length);
    draw_cursor();
    mark_line_start();
    if (text_.total_rows(columns_) != old_total) {
        update_knob();
    }
    intuition_->end_batch();
}

void ShellWindow::begin_input() {
    if (!is_open()) {
        return;
    }
    cursor_visible_ = true;
    if (view_ == 0) {
        draw_cursor();
    }
    mark_line_start();
}

void ShellWindow::echo(u8 ch) {
    if (!is_open() || !is_printable(ch) || columns_ == 0 || rows_ == 0) {
        return;
    }
    intuition_->begin_batch();
    if (view_ > 0) {
        scroll_to_bottom();
    }
    cursor_to_end();
    const u32 old_total = text_.total_rows(columns_);
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    put_byte(ch, true);
    draw_cursor();
    if (text_.total_rows(columns_) != old_total) {
        update_knob();
    }
    intuition_->end_batch();
}

void ShellWindow::erase_last() {
    if (!is_open() || columns_ == 0 || rows_ == 0) {
        return;
    }
    intuition_->begin_batch();
    if (view_ > 0) {
        scroll_to_bottom();
    }
    cursor_to_end();
    const u32 old_total = text_.total_rows(columns_);
    if (!text_.erase_last()) {
        intuition_->end_batch();
        return;
    }
    if (row_ == 0 && col_ == 0) {
        // The erased byte is on a row above the window: lay the text out
        // again so that row comes into view.
        cursor_visible_ = true;
        on_size(this, window_);
        intuition_->end_batch();
        return;
    }
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    draw_cell(' ');
    if (col_ == 0) {
        --row_;
        col_ = columns_ - 1;
    } else {
        --col_;
    }
    draw_cell(' ');
    draw_cursor();
    if (text_.total_rows(columns_) != old_total) {
        update_knob();
    }
    intuition_->end_batch();
}

void ShellWindow::end_line() {
    if (!is_open() || columns_ == 0 || rows_ == 0) {
        return;
    }
    intuition_->begin_batch();
    if (view_ > 0) {
        scroll_to_bottom();
    }
    cursor_to_end();
    const u32 old_total = text_.total_rows(columns_);
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    draw_cell(' ');
    text_.new_line();
    col_ = 0;
    advance_row();
    mark_line_start();
    cursor_visible_ = false;
    if (text_.total_rows(columns_) != old_total) {
        update_knob();
    }
    intuition_->end_batch();
}

void ShellWindow::print_line(const char* text) {
    if (!is_open() || text == nullptr) {
        return;
    }
    intuition_->begin_batch();
    if (col_ != 0) {
        if (view_ == 0) {
            graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
            draw_cell(' ');
            text_.new_line();
            col_ = 0;
            advance_row();
        } else {
            const u8 nl = '\n';
            write(&nl, 1);
        }
    }
    u32 length = 0;
    while (length < kMaxWrite && text[length] != '\0') {
        ++length;
    }
    write(reinterpret_cast<const u8*>(text), length);
    end_line();
    intuition_->end_batch();
}

void ShellWindow::close() {
    if (is_open()) {
        intuition_->close_window(window_);
    }
    if (exec_ != nullptr) {
        if (prop_info_ != 0) {
            exec_->free(prop_info_, ndk::PropInfo_SIZEOF);
        }
        if (gadget_ != 0) {
            exec_->free(gadget_, ndk::Gadget_SIZEOF);
        }
    }
    prop_info_ = 0;
    gadget_ = 0;
    window_ = 0;
    rp_ = 0;
    columns_ = 0;
    rows_ = 0;
    view_ = 0;
    cursor_visible_ = false;
    machine_ = nullptr;
    exec_ = nullptr;
    intuition_ = nullptr;
    graphics_ = nullptr;
}

void ShellWindow::scroll_by(i32 rows) {
    if (!is_open() || columns_ == 0 || rows_ == 0) {
        return;
    }
    const u32 limit = max_view();
    i64 target = static_cast<i64>(view_) + static_cast<i64>(rows);
    if (target < 0) {
        target = 0;
    } else if (target > static_cast<i64>(limit)) {
        target = static_cast<i64>(limit);
    }
    apply_user_view(static_cast<u32>(target));
}

void ShellWindow::scroll_to_top() {
    if (!is_open() || columns_ == 0 || rows_ == 0) {
        return;
    }
    apply_user_view(max_view());
}

void ShellWindow::scroll_to_bottom() {
    if (!is_open() || columns_ == 0 || rows_ == 0) {
        return;
    }
    apply_user_view(0);
}

void ShellWindow::set_view_listener(void (*fn)(void* ctx, u32 view), void* ctx) {
    view_listener_ = fn;
    view_ctx_ = ctx;
}

void ShellWindow::sink(void* ctx, const u8* data, u32 bytes) {
    if (ctx != nullptr) {
        static_cast<ShellWindow*>(ctx)->write(data, bytes);
    }
}

void ShellWindow::on_size(void* ctx, u32 window) {
    if (ctx == nullptr) {
        return;
    }
    ShellWindow& self = *static_cast<ShellWindow*>(ctx);
    if (!self.is_open() || window != self.window_ || self.machine_ == nullptr ||
        self.graphics_ == nullptr) {
        return;
    }
    Bus b(*self.machine_);
    const i32 win_w = static_cast<i16>(b.r16(window + ndk::wd_Width));
    const i32 win_h = static_cast<i16>(b.r16(window + ndk::wd_Height));
    const i32 border_left = static_cast<i32>(b.r8(window + ndk::wd_BorderLeft));
    const i32 border_top = static_cast<i32>(b.r8(window + ndk::wd_BorderTop));
    const i32 border_right = static_cast<i32>(b.r8(window + ndk::wd_BorderRight));
    const i32 border_bottom = static_cast<i32>(b.r8(window + ndk::wd_BorderBottom));
    if (!b.ok()) {
        return;
    }
    const u32 old_columns = self.columns_;
    const u32 old_rows = self.rows_;
    const u32 old_view = self.view_;
    u32 top_number = 0;
    u32 row_in_line = 0;
    bool had_top = false;
    if (old_view > 0 && old_columns != 0 && old_rows != 0) {
        const u32 old_total = self.text_.total_rows(old_columns);
        const u32 old_limit = old_total > old_rows ? old_total - old_rows : 0;
        const u32 clamped_old_view = old_view < old_limit ? old_view : old_limit;
        const u32 old_top_row = old_limit - clamped_old_view;
        u32 line = 0;
        self.text_.locate_row(old_columns, old_top_row, line, row_in_line);
        top_number = self.text_.dropped() + line;
        had_top = true;
    }

    const i32 width = win_w - border_left - border_right;
    const i32 height = win_h - border_top - border_bottom;
    self.origin_x_ = border_left;
    self.origin_y_ = border_top;
    self.inner_w_ = width > 0 ? width : 0;
    self.inner_h_ = height > 0 ? height : 0;
    self.columns_ = static_cast<u32>(self.inner_w_) / kCell;
    self.rows_ = static_cast<u32>(self.inner_h_) / kCell;

    if (!had_top || self.columns_ == 0 || self.rows_ == 0) {
        self.view_ = 0;
    } else {
        const u32 new_limit = self.max_view();
        u32 top_row = 0;
        if (top_number >= self.text_.dropped()) {
            const u32 line = top_number - self.text_.dropped();
            const u32 line_r = self.text_.rows_of_line(self.columns_, line);
            const u32 max_in_line = line_r > 0 ? line_r - 1 : 0;
            const u32 scaled = static_cast<u32>(
                (static_cast<u64>(row_in_line) * old_columns) / self.columns_);
            const u32 offset = scaled < max_in_line ? scaled : max_in_line;
            top_row = self.text_.rows_before(self.columns_, line) + offset;
        }
        if (top_row > new_limit) {
            top_row = new_limit;
        }
        self.view_ = new_limit - top_row;
    }

    self.intuition_->begin_batch();
    self.redraw_viewport();
    self.update_knob();
    self.intuition_->end_batch();
    if (self.view_ != old_view) {
        self.notify_view_listener();
    }
}

void ShellWindow::on_gadget(void* ctx, u32 gadget, u32 vert_pot) {
    if (ctx == nullptr) {
        return;
    }
    ShellWindow& self = *static_cast<ShellWindow*>(ctx);
    if (!self.is_open() || gadget != self.gadget_ || self.columns_ == 0 || self.rows_ == 0) {
        return;
    }
    const u32 limit = self.max_view();
    u32 new_view = 0;
    if (limit > 0) {
        const u32 pot = vert_pot > ndk::MAXPOT ? ndk::MAXPOT : vert_pot;
        u32 top_row = static_cast<u32>(
            (static_cast<u64>(pot) * limit + ndk::MAXPOT / 2) / ndk::MAXPOT);
        if (top_row > limit) {
            top_row = limit;
        }
        new_view = limit - top_row;
    }
    if (new_view == self.view_) {
        return;
    }
    self.intuition_->begin_batch();
    self.view_ = new_view;
    self.redraw_viewport();
    self.intuition_->end_batch();
    self.notify_view_listener();
}

u32 ShellWindow::max_view() const {
    if (columns_ == 0 || rows_ == 0) {
        return 0;
    }
    const u32 total = text_.total_rows(columns_);
    return total > rows_ ? total - rows_ : 0;
}

void ShellWindow::apply_user_view(u32 new_view) {
    const u32 limit = max_view();
    if (new_view > limit) {
        new_view = limit;
    }
    if (new_view == view_) {
        return;
    }
    intuition_->begin_batch();
    view_ = new_view;
    redraw_viewport();
    update_knob();
    intuition_->end_batch();
    notify_view_listener();
}

void ShellWindow::redraw_viewport() {
    if (inner_w_ > 0 && inner_h_ > 0) {
        graphics_->set_pens(rp_, 0, 0, ndk::JAM2);
        graphics_->rect_fill(rp_, origin_x_, origin_y_,
                             origin_x_ + inner_w_ - 1,
                             origin_y_ + inner_h_ - 1);
    }
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    const u32 max_rows = rows_ < kMaxRows ? rows_ : kMaxRows;
    // Static: 8 KiB is too much for the kernel stack, and there is one shell.
    static ShellText::Row visible[kMaxRows];
    u32 filled = 0;
    if (columns_ != 0 && max_rows != 0) {
        const u32 total = text_.total_rows(columns_);
        const u32 take = max_rows < total ? max_rows : total;
        const u32 limit = total - take;
        const u32 clamped_view = view_ < limit ? view_ : limit;
        const u32 top_row = limit - clamped_view;
        filled = text_.rows_from(columns_, top_row, max_rows, visible);
    }
    for (u32 r = 0; r < filled; ++r) {
        if (visible[r].length != 0) {
            graphics_->move(
                rp_, origin_x_,
                origin_y_ + static_cast<i32>(r * kCell + kBaseline));
            graphics_->text(rp_, visible[r].text, visible[r].length);
        }
    }
    if (view_ == 0) {
        row_ = filled != 0 ? filled - 1 : 0;
        col_ = text_.cursor_column(columns_);
        if (cursor_visible_) {
            draw_cursor();
        }
    }
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
}

void ShellWindow::update_knob() {
    if (!is_open() || gadget_ == 0) {
        return;
    }
    const u32 total = columns_ == 0 ? 0 : text_.total_rows(columns_);
    const u32 visible = rows_;
    u32 vbody = ndk::MAXBODY;
    u32 vpot = 0;
    if (visible > 0) {
        const u32 denom = total > visible ? total : visible;
        vbody = static_cast<u32>((static_cast<u64>(ndk::MAXBODY) * visible) / denom);
        if (total > visible) {
            const u32 range = total - visible;
            const u32 clamped = view_ < range ? view_ : range;
            const u32 top_row = range - clamped;
            vpot = static_cast<u32>((static_cast<u64>(ndk::MAXPOT) * top_row) / range);
        }
    }
    intuition_->new_modify_prop(gadget_, window_, kPropFlags, 0, vpot, ndk::MAXBODY, vbody, 1);
}

void ShellWindow::notify_view_listener() {
    if (view_listener_ != nullptr) {
        view_listener_(view_ctx_, view_);
    }
}

void ShellWindow::put_byte(u8 c, bool draw) {
    if (c == '\n') {
        text_.new_line();
        col_ = 0;
        if (draw) {
            advance_row();
        }
        return;
    }
    if (c == '\r') {
        // Carriage return moves the on-screen cursor to column 0 without
        // altering the logical line in the text store.
        col_ = 0;
        return;
    }
    if (c == '\t') {
        const u32 next = (col_ / kCell + 1) * kCell;
        const u32 end = next < columns_ ? next : columns_;
        for (u32 i = col_; i < end; ++i) {
            text_.put(' ');
        }
        if (next >= columns_) {
            col_ = 0;
            if (draw) {
                advance_row();
            }
        } else {
            col_ = next;
        }
        return;
    }
    if (draw) {
        draw_cell(c);
    }
    text_.put(c);
    ++col_;
    if (col_ >= columns_) {
        col_ = 0;
        if (draw) {
            advance_row();
        }
    }
}

void ShellWindow::advance_row() {
    if (rows_ == 0) {
        return;
    }
    if (row_ + 1 < rows_) {
        ++row_;
        return;
    }
    if (columns_ == 0) {
        return;
    }
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    graphics_->scroll(rp_, 0, static_cast<i32>(kCell), origin_x_, origin_y_,
                      origin_x_ + inner_w_ - 1, origin_y_ + inner_h_ - 1);
}

void ShellWindow::draw_cell(u8 c) {
    graphics_->move(rp_, origin_x_ + static_cast<i32>(col_ * kCell),
                    origin_y_ + static_cast<i32>(row_ * kCell) + static_cast<i32>(kBaseline));
    graphics_->text(rp_, &c, 1);
}

void ShellWindow::mark_line_start() {
    text_.mark_input_start();
}

void ShellWindow::set_cursor_back(u32 back) {
    if (!is_open() || columns_ == 0 || rows_ == 0) {
        return;
    }
    const u32 typed = text_.input_length();
    if (back > typed) {
        back = typed;
    }
    intuition_->begin_batch();
    if (view_ > 0) {
        scroll_to_bottom();
    }
    hide_cursor();
    cursor_back_ = back;
    draw_cursor();
    intuition_->end_batch();
}

void ShellWindow::cursor_to_end() {
    if (cursor_back_ == 0) {
        return;
    }
    hide_cursor();
    cursor_back_ = 0;
    draw_cursor();
}

// The cell the cursor is on when it sits cursor_back_ bytes before the end
// of the input; false when that cell is above the window.
bool ShellWindow::cursor_cell(u32& row, u32& column) const {
    const u64 at = static_cast<u64>(row_) * columns_ + col_;
    if (cursor_back_ > at) {
        return false;
    }
    row = static_cast<u32>((at - cursor_back_) / columns_);
    column = static_cast<u32>((at - cursor_back_) % columns_);
    return true;
}

void ShellWindow::draw_cell_at(u32 row, u32 column, u8 c) {
    graphics_->move(rp_, origin_x_ + static_cast<i32>(column * kCell),
                    origin_y_ + static_cast<i32>(row * kCell) + static_cast<i32>(kBaseline));
    graphics_->text(rp_, &c, 1);
}

// Draws the cell under the cursor as plain text again.
void ShellWindow::hide_cursor() {
    if (columns_ == 0 || rows_ == 0 || view_ > 0) {
        return;
    }
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    if (cursor_back_ == 0) {
        draw_cell(' ');
        return;
    }
    u32 row = 0;
    u32 column = 0;
    if (cursor_cell(row, column)) {
        draw_cell_at(row, column, text_.current_byte(text_.current_length() - cursor_back_));
    }
}

void ShellWindow::draw_cursor() {
    cursor_visible_ = true;
    if (columns_ == 0 || rows_ == 0) {
        return;
    }
    if (cursor_back_ != 0) {
        u32 row = 0;
        u32 column = 0;
        if (cursor_cell(row, column)) {
            graphics_->set_pens(rp_, 0, 1, ndk::JAM2);
            draw_cell_at(row, column,
                         text_.current_byte(text_.current_length() - cursor_back_));
            graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
        }
        return;
    }
    graphics_->set_pens(rp_, 1, 1, ndk::JAM2);
    const i32 x = origin_x_ + static_cast<i32>(col_ * kCell);
    const i32 y = origin_y_ + static_cast<i32>(row_ * kCell);
    graphics_->rect_fill(rp_, x, y, x + static_cast<i32>(kCell) - 1,
                         y + static_cast<i32>(kCell) - 1);
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
}

}
