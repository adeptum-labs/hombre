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

constexpr char kTitle[] = "AmigaShell";
constexpr u32 kMaxWrite = 65536;
constexpr u32 kCell = 8;
constexpr u32 kBaseline = 6;

// Printable Latin-1: the ASCII range and the codes above the control block.
constexpr bool is_printable(u8 c) {
    return (c >= 0x20 && c <= 0x7e) || c >= 0xa0;
}

}

bool ShellWindow::open(Machine& m, Exec& exec, Graphics& gfx, Intuition& intuition,
                       DisplayState& display) {
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
    b.w16(nw + ndk::nw_LeftEdge, 0);
    // Below the screen's title bar and the line under it.
    b.w16(nw + ndk::nw_TopEdge, b.r8(display.front_screen + ndk::sc_BarHeight) + 1);
    b.w16(nw + ndk::nw_Width, 0);
    b.w16(nw + ndk::nw_Height, 0);
    b.w8(nw + ndk::nw_DetailPen, 0);
    b.w8(nw + ndk::nw_BlockPen, 1);
    b.w16(nw + ndk::nw_IDCMPFlags, 0);
    b.w32(nw + ndk::nw_Flags,
           ndk::WFLG_DEPTHGADGET | ndk::WFLG_CLOSEGADGET | ndk::WFLG_DRAGBAR);
    b.w32(nw + ndk::nw_Title, title);
    b.w32(nw + ndk::nw_Screen, display.front_screen);
    b.w16(nw + ndk::nw_MinWidth, 0);
    b.w16(nw + ndk::nw_MinHeight, 0);
    b.w16(nw + ndk::nw_MaxWidth, 0);
    b.w16(nw + ndk::nw_MaxHeight, 0);
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
    const u32 layer = b.r32(window + ndk::wd_WLayer);
    if (!b.ok() || rp == 0 || layer == 0) {
        intuition.close_window(window);
        return false;
    }
    const i32 min_x = static_cast<i16>(b.r16(layer + ndk::ly_MinX));
    const i32 min_y = static_cast<i16>(b.r16(layer + ndk::ly_MinY));
    const i32 max_x = static_cast<i16>(b.r16(layer + ndk::ly_MaxX));
    const i32 max_y = static_cast<i16>(b.r16(layer + ndk::ly_MaxY));
    const i32 border_left = static_cast<i32>(b.r8(window + ndk::wd_BorderLeft));
    const i32 border_top = static_cast<i32>(b.r8(window + ndk::wd_BorderTop));
    const i32 border_right = static_cast<i32>(b.r8(window + ndk::wd_BorderRight));
    const i32 border_bottom = static_cast<i32>(b.r8(window + ndk::wd_BorderBottom));
    if (!b.ok()) {
        intuition.close_window(window);
        return false;
    }
    const i32 width = max_x - min_x + 1 - border_left - border_right;
    const i32 height = max_y - min_y + 1 - border_top - border_bottom;
    if (width <= 0 || height <= 0) {
        intuition.close_window(window);
        return false;
    }

    intuition_ = &intuition;
    graphics_ = &gfx;
    window_ = window;
    rp_ = rp;
    origin_x_ = border_left;
    origin_y_ = border_top;
    inner_w_ = width;
    inner_h_ = height;
    columns_ = static_cast<u32>(width) / kCell;
    rows_ = static_cast<u32>(height) / kCell;
    col_ = 0;
    row_ = 0;
    mark_line_start();
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
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    for (u32 i = 0; i < bytes; ++i) {
        put_byte(data[i]);
    }
}

void ShellWindow::write_prompt(u32 cli_number, const char* volume) {
    if (!is_open()) {
        return;
    }
    if (col_ != 0) {
        col_ = 0;
        advance_row();
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
    write(reinterpret_cast<const u8*>(prompt), length);
    draw_cursor();
    mark_line_start();
}

void ShellWindow::echo(u8 ch) {
    if (!is_open() || !is_printable(ch) || columns_ == 0 || rows_ == 0) {
        return;
    }
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    put_byte(ch);
    draw_cursor();
}

void ShellWindow::erase_last() {
    if (!is_open() || columns_ == 0 || rows_ == 0 || !has_typed_input()) {
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
}

void ShellWindow::end_line() {
    if (!is_open() || columns_ == 0 || rows_ == 0) {
        return;
    }
    graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
    draw_cell(' ');
    col_ = 0;
    advance_row();
    mark_line_start();
}

void ShellWindow::print_line(const char* text) {
    if (!is_open() || text == nullptr) {
        return;
    }
    if (col_ != 0) {
        graphics_->set_pens(rp_, 1, 0, ndk::JAM2);
        draw_cell(' ');
        col_ = 0;
        advance_row();
    }
    u32 length = 0;
    while (length < kMaxWrite && text[length] != '\0') {
        ++length;
    }
    write(reinterpret_cast<const u8*>(text), length);
    end_line();
}

void ShellWindow::close() {
    if (is_open()) {
        intuition_->close_window(window_);
    }
    window_ = 0;
    rp_ = 0;
    intuition_ = nullptr;
    graphics_ = nullptr;
}

void ShellWindow::sink(void* ctx, const u8* data, u32 bytes) {
    if (ctx != nullptr) {
        static_cast<ShellWindow*>(ctx)->write(data, bytes);
    }
}

void ShellWindow::put_byte(u8 c) {
    if (c == '\n') {
        col_ = 0;
        advance_row();
        return;
    }
    if (c == '\r') {
        col_ = 0;
        return;
    }
    if (c == '\t') {
        const u32 next = (col_ / kCell + 1) * kCell;
        if (next >= columns_) {
            col_ = 0;
            advance_row();
        } else {
            col_ = next;
        }
        return;
    }
    draw_cell(c);
    ++col_;
    if (col_ >= columns_) {
        col_ = 0;
        advance_row();
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
    // The start of the typed line moves up with its text.
    if (line_start_row_ != 0) {
        --line_start_row_;
    }
}

void ShellWindow::draw_cell(u8 c) {
    graphics_->move(rp_, origin_x_ + static_cast<i32>(col_ * kCell),
                    origin_y_ + static_cast<i32>(row_ * kCell) + static_cast<i32>(kBaseline));
    graphics_->text(rp_, &c, 1);
}

void ShellWindow::mark_line_start() {
    line_start_row_ = row_;
    line_start_col_ = col_;
}

bool ShellWindow::has_typed_input() const {
    return row_ > line_start_row_ || col_ > line_start_col_;
}

void ShellWindow::draw_cursor() {
    if (columns_ == 0 || rows_ == 0) {
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
